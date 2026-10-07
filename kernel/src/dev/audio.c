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

/* the last slot is the system's own stream, which no file owns */
static struct stream *streams[MAX_STREAMS + 1];
static struct wait_queue audio_wq; /* writers waiting for room, closers waiting to drain */
static int volume = 80;            /* percent */
static bool muted;
static int32_t gain = 80 * 80 * 65536 / 10000; /* volume squared, 16.16: loudness feels linear */
static const char *card_name;
static void (*card_fill)(void);
static int remote_users;

#define TRACE_SECONDS 22
bool audio_trace_active;
static struct {
    uint64_t start, last_tick, max_tick_gap, ticks, card_calls, restarts, filled;
    uint64_t sr_before[32], sr_after[32];
    struct {
        uint64_t frames, writes, last, max_gap, shortage, mix_calls;
        uint32_t min_queue, max_queue, pid, owner_changes, closes, flushes;
        uint64_t by_second[TRACE_SECONDS];
    } stream[MAX_STREAMS];
} trace;

static uint64_t trace_us(uint64_t ticks) {
    return tsc_hz ? ticks * 1000000 / tsc_hz : 0;
}
void audio_trace_begin(void) {
    /* WM invokes this before arming its timed interval. No reset under a
       long IRQ-disabled region; IRQ writers still see active=false. */
    audio_trace_active = false;
    memset(&trace, 0, sizeof trace);
    for (int i = 0; i < MAX_STREAMS; i++) trace.stream[i].min_queue = RING_BYTES / AUDIO_FRAME;
    uint64_t f = irq_save();
    trace.start = rdtsc(); trace.last_tick = trace.start;
    audio_trace_active = true;
    irq_restore(f);
}
void audio_trace_end(void) {
    uint64_t f = irq_save(), end = rdtsc();
    audio_trace_active = false;
    irq_restore(f);
    kprintf("audiotrace: elapsed_us=%lu service_calls=%lu max_service_gap_us=%lu card=%s remote=%d muted=%d\n",
            trace_us(end-trace.start), trace.ticks, trace_us(trace.max_tick_gap),
            card_name ? card_name : "none", remote_users, muted);
    kprintf("audiotrace: ac97_calls=%lu filled=%lu restart_actions=%lu raw_sr_low5_before_after\n",
            trace.card_calls, trace.filled, trace.restarts);
    for (int i = 0; i < 32; i++) if (trace.sr_before[i] || trace.sr_after[i])
        kprintf("audiotrace: sr=%x before=%lu after=%lu\n", i, trace.sr_before[i], trace.sr_after[i]);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!trace.stream[i].mix_calls && !trace.stream[i].writes && !trace.stream[i].closes) continue;
        uint64_t last = trace.stream[i].last ? trace.stream[i].last : trace.start;
        kprintf("audiotrace: slot=%d writer_pid=%u owner_changes=%u committed_frames=%lu writes=%lu max_commit_gap_us=%lu final_commit_age_us=%lu queue_min=%u queue_max=%u raw_shortage_frames=%lu mix_calls=%lu closes=%u flushes=%u\n",
                i, trace.stream[i].pid, trace.stream[i].owner_changes, trace.stream[i].frames,
                trace.stream[i].writes, trace_us(trace.stream[i].max_gap), trace_us(end-last),
                trace.stream[i].min_queue, trace.stream[i].max_queue, trace.stream[i].shortage,
                trace.stream[i].mix_calls, trace.stream[i].closes, trace.stream[i].flushes);
        for (int j = 0; j < TRACE_SECONDS; j++)
            kprintf("audiotrace: slot=%d second=%d committed_frames=%lu\n", i, j, trace.stream[i].by_second[j]);
    }
}
void audio_trace_ac97(uint16_t before, uint16_t after, unsigned filled, bool restarted) {
    if (!audio_trace_active) return;
    trace.card_calls++; trace.filled += filled; trace.restarts += restarted;
    trace.sr_before[before & 31]++; trace.sr_after[after & 31]++;
}
static void trace_commit(struct stream *s, uint32_t bytes) {
    if (!audio_trace_active) return;
    uint64_t f = irq_save(), now = rdtsc();
    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i] == s) {
        uint64_t last = trace.stream[i].last ? trace.stream[i].last : trace.start;
        trace.stream[i].max_gap = MAX(trace.stream[i].max_gap, now-last);
        trace.stream[i].last = now; trace.stream[i].writes++;
        trace.stream[i].frames += bytes / AUDIO_FRAME;
        uint32_t pid = (uint32_t)current_task->pid;
        if (trace.stream[i].pid && trace.stream[i].pid != pid) trace.stream[i].owner_changes++;
        trace.stream[i].pid = pid;
        uint32_t queue = (s->head-s->tail) / AUDIO_FRAME;
        trace.stream[i].max_queue = MAX(trace.stream[i].max_queue, queue);
        unsigned second = (unsigned)((now-trace.start) / (tsc_hz ? tsc_hz : 1));
        if (second >= TRACE_SECONDS) second = TRACE_SECONDS-1;
        trace.stream[i].by_second[second] += bytes / AUDIO_FRAME;
        break;
    }
    irq_restore(f);
}

bool audio_mix(int16_t *out, int frames) {
    uint64_t f = irq_save();
    bool any = false;
    if (out) memset(out, 0, (size_t)frames * AUDIO_FRAME);
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s) continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        int n = (int)MIN((uint32_t)frames, have);
        if (audio_trace_active && i < MAX_STREAMS) {
            trace.stream[i].min_queue = MIN(trace.stream[i].min_queue, have);
            trace.stream[i].shortage += (uint32_t)frames - (uint32_t)n;
            trace.stream[i].mix_calls++;
        }
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
    for (int i = 0; i <= MAX_STREAMS; i++)
        if (streams[i] && streams[i]->head - streams[i]->tail >= AUDIO_FRAME) return true;
    return false;
}

void audio_tick(void) {
    static uint64_t last;
    uint64_t now = uptime_ms();
    if (now - last < TICK_MS) return;
    uint64_t elapsed = MIN(now - last, 100);
    last = now;
    if (audio_trace_active) {
        uint64_t at = rdtsc();
        trace.max_tick_gap = MAX(trace.max_tick_gap, at-trace.last_tick);
        trace.last_tick = at; trace.ticks++;
    }
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
bool audio_remote(void) { return remote_users > 0; }

static void set_gain(void) { gain = muted ? 0 : volume * volume * 65536 / 10000; }
int audio_volume(void) { return volume; }
bool audio_muted(void) { return muted; }
void audio_set_volume(int percent) {
    volume = MAX(0, MIN(100, percent));
    muted = false;
    set_gain();
}
void audio_set_muted(bool m) {
    muted = m;
    set_gain();
}

static uint8_t system_ring[RING_BYTES];
static struct stream system_stream = {.ring = system_ring};

/* a triangle wave (no floating point in the kernel) that fades out, with a 2 ms start */
void audio_system_tone(int hz, int ms) {
    struct stream *s = &system_stream;
    streams[MAX_STREAMS] = s;
    if (hz <= 0 || s->head - s->tail >= AUDIO_FRAME) return;
    int frames = MIN(ms * (AUDIO_RATE / 1000), (int)(RING_BYTES / AUDIO_FRAME)), period = AUDIO_RATE / hz;
    for (int k = 0; k < frames; k++) {
        int ph = k % period, tri = ph < period / 2 ? 4 * ph - period : 3 * period - 4 * ph; /* -period..period */
        int amp = 7000 * (frames - k) / frames;
        if (k < 96) amp = amp * k / 96;
        int16_t v = (int16_t)(tri * amp / period);
        int16_t *at = (int16_t *)(s->ring + (s->head + (uint32_t)k * AUDIO_FRAME) % RING_BYTES);
        at[0] = at[1] = v;
    }
    __sync_synchronize();
    s->head += (uint32_t)frames * AUDIO_FRAME;
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
        trace_commit(s, c);
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
        if (streams[i] == s) {
            if (audio_trace_active) trace.stream[i].closes++;
            streams[i] = NULL;
        }
    irq_restore(fl);
    kfree(s->ring);
    kfree(s);
}

static struct vnode_ops audio_ops = {.open = audio_open, .close = audio_close, .write = audio_write,
                                     .read = audio_read, .can_write = audio_can_write};

int audio_flush_file(struct file *f) {
    if (!f || !f->vn || f->vn->ops != &audio_ops) return -EINVAL;
    int64_t err = 0;
    struct stream *s = stream_of(f, &err);
    if (!s) return (int)err;
    uint64_t fl = irq_save();
    s->tail = s->head;
    if (audio_trace_active) for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) trace.stream[i].flushes++;
    wq_wake_all(&audio_wq);
    poll_notify();
    irq_restore(fl);
    return 0;
}

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
    audio_set_volume(val);
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
