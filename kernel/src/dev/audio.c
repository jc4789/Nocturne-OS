/* Sound: /dev/audio streams, the mixer and the clock that drives the sink (see audio.h). */
#include "dev/audio.h"
#include "arch/cpu.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "sys/sched.h"

#define MAX_STREAMS 8
#define RING_BYTES  (8192 * AUDIO_FRAME) /* 170 ms: what a writer may be ahead of the sink */
#define TICK_MS     5

/* A ring of bytes: the writer (one task) moves head, the mixer (the timer interrupt, or a
   remote session's thread with interrupts off) moves tail. Both run free; head - tail is the
   number of bytes queued. Whole frames never wrap, since the ring holds a whole number of them. */
struct stream {
    uint8_t *ring;
    volatile uint32_t head, tail;
};

static struct stream *streams[MAX_STREAMS];
static struct wait_queue audio_wq; /* writers waiting for room, closers waiting to drain */
static int volume = 80;            /* percent */
static int32_t gain = 80 * 80 * 65536 / 10000; /* volume squared, 16.16: loudness feels linear */
static const char *card_name;
static void (*card_fill)(void);
static int remote_users;

bool audio_mix(int16_t *out, int frames) {
    uint64_t f = irq_save();
    bool any = false;
    if (out) memset(out, 0, (size_t)frames * AUDIO_FRAME);
    for (int i = 0; i < MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s) continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        int n = (int)MIN((uint32_t)frames, have);
        if (!n) continue;
        any = true;
        if (out) {
            for (int k = 0; k < n; k++) {
                const int16_t *in = (const int16_t *)(s->ring + (s->tail + (uint32_t)k * AUDIO_FRAME) % RING_BYTES);
                for (int ch = 0; ch < 2; ch++) {
                    int32_t v = out[k * 2 + ch] + (int32_t)(((int64_t)in[ch] * gain) >> 16);
                    out[k * 2 + ch] = (int16_t)MAX(-32768, MIN(32767, v));
                }
            }
        }
        s->tail += (uint32_t)n * AUDIO_FRAME;
    }
    if (any) {
        wq_wake_all(&audio_wq);
        poll_notify();
    }
    irq_restore(f);
    return any;
}

bool audio_pending(void) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] && streams[i]->head - streams[i]->tail >= AUDIO_FRAME) return true;
    return false;
}

void audio_tick(void) {
    static uint64_t last;
    uint64_t now = uptime_ms();
    if (now - last < TICK_MS) return;
    uint64_t elapsed = MIN(now - last, 100);
    last = now;
    if (remote_users) return; /* the client pulls the sound itself */
    if (card_fill) card_fill();
    else audio_mix(NULL, (int)(elapsed * AUDIO_RATE / 1000)); /* no card: play into nothing */
}

void audio_set_card(const char *name, void (*fill)(void)) {
    card_name = name;
    card_fill = fill;
}
const char *audio_card(void) { return card_name; }

void audio_remote_attach(void) {
    uint64_t f = irq_save();
    remote_users++;
    irq_restore(f);
}
void audio_remote_detach(void) {
    uint64_t f = irq_save();
    if (remote_users > 0) remote_users--;
    irq_restore(f);
}

/* ---- /dev/audio ---- */

static void audio_open(struct vnode *v, struct file *f) {
    struct stream *s = kzalloc(sizeof *s);
    if (s) s->ring = kmalloc(RING_BYTES);
    if (!s || !s->ring) {
        if (s) kfree(s);
        return; /* writes fail with ENOMEM */
    }
    uint64_t fl = irq_save();
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!streams[i]) {
            streams[i] = s;
            f->priv = s;
            break;
        }
    irq_restore(fl);
    if (!f->priv) { /* too many streams: writes fail with EBUSY */
        kfree(s->ring);
        kfree(s);
        f->priv = (void *)1;
    }
}

static struct stream *stream_of(struct file *f, int64_t *err) {
    if (!f->priv) *err = -ENOMEM;
    else if (f->priv == (void *)1) *err = -EBUSY;
    else return f->priv;
    return NULL;
}

static int64_t audio_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    int64_t err;
    struct stream *s = stream_of(f, &err);
    if (!s) return err;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        uint32_t room = RING_BYTES - (s->head - s->tail);
        if (!room) {
            if (done) break; /* report what went in; the caller writes the rest */
            if (f->flags & O_NONBLOCK) return -EAGAIN;
            if (current_task->killed) return -EINTR;
            uint64_t fl = irq_save();
            if (s->head - s->tail == RING_BYTES) wq_wait(&audio_wq);
            irq_restore(fl);
            continue;
        }
        /* copy outside the lock: only this task writes past head, and user memory may fault */
        uint32_t c = (uint32_t)MIN((size_t)room, n - done);
        uint32_t at = s->head % RING_BYTES, first = MIN(c, RING_BYTES - at);
        memcpy(s->ring + at, p + done, first);
        memcpy(s->ring, p + done + first, c - first);
        __sync_synchronize();
        s->head += c;
        done += c;
    }
    return (int64_t)done;
}

/* Reading a stream tells how far ahead of the sink it is: a uint32_t, in frames. */
static int64_t audio_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    int64_t err;
    struct stream *s = stream_of(f, &err);
    if (!s) return err;
    if (n < 4) return -EINVAL;
    uint32_t q = (s->head - s->tail) / AUDIO_FRAME;
    memcpy(buf, &q, 4);
    return 4;
}

static bool audio_can_write(struct vnode *v, struct file *f) {
    int64_t err;
    struct stream *s = stream_of(f, &err);
    return !s || s->head - s->tail < RING_BYTES;
}

/* Closing lets what is queued play out (as when a program writes a sound and exits), unless the
   program is being killed. */
static void audio_close(struct vnode *v, struct file *f) {
    int64_t err;
    struct stream *s = stream_of(f, &err);
    if (!s) return;
    uint64_t give_up = uptime_ms() + 1000;
    while (!current_task->killed && s->head - s->tail >= AUDIO_FRAME && uptime_ms() < give_up) {
        uint64_t fl = irq_save();
        wq_wait_timeout(&audio_wq, 20);
        irq_restore(fl);
    }
    uint64_t fl = irq_save();
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) streams[i] = NULL;
    irq_restore(fl);
    kfree(s->ring);
    kfree(s);
}

static struct vnode_ops audio_ops = {.open = audio_open, .close = audio_close, .write = audio_write,
                                     .read = audio_read, .can_write = audio_can_write};

/* ---- /dev/volume: the master volume as text, 0 to 100 ---- */

static int64_t volume_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    char tmp[8];
    int len = ksnprintf(tmp, sizeof tmp, "%d\n", volume);
    if (off >= (uint64_t)len) return 0;
    size_t c = MIN(n, (size_t)len - off);
    memcpy(buf, tmp + off, c);
    f->off += c;
    return (int64_t)c;
}

static int64_t volume_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    const char *p = buf;
    int val = 0, digits = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '9' && digits < 4; i++, digits++) val = val * 10 + (p[i] - '0');
    if (!digits) return -EINVAL;
    volume = MIN(val, 100);
    gain = volume * volume * 65536 / 10000;
    return (int64_t)n;
}

static struct vnode_ops volume_ops = {.read = volume_read, .write = volume_write};

void audio_init(void) {
    struct vnode *dev = vfs_lookup("/dev");
    struct vnode *a = ramfs_new_node("audio", VT_CHAR), *vol = ramfs_new_node("volume", VT_CHAR);
    a->ops = &audio_ops;
    a->mode = 0666;
    vol->ops = &volume_ops;
    vol->mode = 0666;
    ramfs_add_child(dev, a);
    ramfs_add_child(dev, vol);
    ac97_init();
}
