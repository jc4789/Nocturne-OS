/* Sound: the apps' streams, the mixer and the clock that drives the sink (see audio.h). */
#include "dev/audio.h"
#include "sys/syscall.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/sched.h"
#include "sys/object.h"
#include "sys/keep.h"
#include "sys/watch.h"
#include "abi.h"
#include "vm/vm.h"

#define MAX_STREAMS 8
#define QUEUE_BYTES (8192 * AUDIO_FRAME) /* 170 ms: what a writer may be ahead of the sink (SOUND_QUEUE) */
#define RING_BYTES  (2 * QUEUE_BYTES)    /* the queue, and the PCM a card may still give back (retained) */
#define TICK_MS     5

/* A ring of bytes: the writer moves head, the mixer (the timer interrupt, or a remote session's
   thread) moves tail, both under audio_lock; head - tail is the number of bytes queued. The writer
   copies its PCM past head without the lock, and the mixer reads only up to head. Whole frames
   never wrap, since the ring holds a whole number of them. The `retained` frames just before tail
   were mixed into a card's buffers, which may give them back (take_back): the writer leaves them
   alone until the card has played them. */
struct stream {
    uint8_t *ring;
    uint32_t head, tail;
    uint64_t committed_bytes;
    uint32_t generation;
    /* At most the actual 32 AC97 descriptors can own outstanding runs. No
       stream/task pointer escapes into the DMA ring; close/reuse is safe. */
    struct {uint64_t start;uint32_t frames;} issued[32];
    unsigned issued_head,issued_count;
    uint32_t device_pending;
    uint32_t retained; /* the last of device_pending, still in the ring before tail */
    bool clock_error;
    struct task *writer; /* the one submit copying past head; another waits for it */
    struct object object; /* an app's stream: what its handle reaches */
};

/* The streams' queues and clocks (every field of a stream above but ring and object), the list of
   streams, the card's ordinals and the trace: any CPU, interrupts off (the mixer runs in the
   timer interrupt). Taken before a wait queue's and an object's lock. The card's device stays the
   BSP's: its fill() runs from the BSP's tick and after a flush, without this lock (it takes it in
   audio_card_mix and audio_card_played); its unissue() runs under it, in take_back. */
static struct spinlock audio_lock;
static uint64_t card_submitted,card_consumed;

/* the last slot is the system's own stream, which no app owns */
static struct stream *streams[MAX_STREAMS + 1];

static struct wait_queue audio_wq; /* writers waiting for room or for the writer, closers waiting to drain */
static int volume = 80;            /* percent */
static bool muted;
static int32_t gain = 80 * 80 * 65536 / 10000; /* volume squared, 16.16: loudness feels linear */
static const char *card_name;
static void (*card_fill)(void);
static uint64_t (*card_unissue)(bool take);
static int remote_users;
/* What SYS_AUDIO_OUTPUT reports (the volume, muting, what plays the sound) changed: its watches
   (SYS_AUDIO_OUTPUT_WATCH) hear of it, whoever changed it. */
static struct watched output_watched;

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
    for (int i = 0; i < MAX_STREAMS; i++) trace.stream[i].min_queue = QUEUE_BYTES / AUDIO_FRAME;
    uint64_t f = spin_lock(&audio_lock);
    trace.start = rdtsc(); trace.last_tick = trace.start;
    audio_trace_active = true;
    spin_unlock(&audio_lock, f);
}
void audio_trace_end(void) {
    uint64_t f = spin_lock(&audio_lock), end = rdtsc();
    audio_trace_active = false;
    spin_unlock(&audio_lock, f);
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
/* audio_lock held. */
static void trace_commit(struct stream *s, uint32_t bytes) {
    if (!audio_trace_active) return;
    uint64_t now = rdtsc();
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
}

/* The apps' streams moved on: waits for room look again. audio_lock held. */
static void streams_changed(void) {
    wq_wake_all(&audio_wq);
    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i]) object_notify(&streams[i]->object);
}

static bool mix(int16_t *out, int frames,bool card) {
    if(frames<=0||(unsigned)frames>RING_BYTES/AUDIO_FRAME)return false;
    uint64_t f = spin_lock(&audio_lock);
    if(card&&(uint64_t)frames>UINT64_MAX-card_submitted){spin_unlock(&audio_lock,f);return false;}
    bool any = false,error = false;
    if (out) memset(out, 0, (size_t)frames * AUDIO_FRAME);
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s) continue;
        if(s->head-s->tail>RING_BYTES||s->issued_head>=ARRAY_SIZE(s->issued)||s->issued_count>ARRAY_SIZE(s->issued)){s->clock_error=true;error=true;continue;}
        if(s->clock_error)continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        int n = (int)MIN((uint32_t)frames, have);
        if (audio_trace_active && i < MAX_STREAMS) {
            trace.stream[i].min_queue = MIN(trace.stream[i].min_queue, have);
            trace.stream[i].shortage += (uint32_t)frames - (uint32_t)n;
            trace.stream[i].mix_calls++;
        }
        if (!n) continue;
        if(!card)s->retained=0; /* the card's PCM no longer lies just before tail */
        if(card){
            unsigned last=(s->issued_head+s->issued_count-1)&31;
            if(s->issued_count&&s->issued[last].start+s->issued[last].frames==card_submitted)
                s->issued[last].frames+=(uint32_t)n;
            else{
                /* AC97 retires its cached CIV/PICB before submitting another
                   descriptor; 32 covers the complete physical ring. */
                /* A lost/stale device snapshot must not turn media input
                   into a kernel panic or fictitious consumption. Keep PCM
                   in its software owner until real DMA retirement frees a
                   physical-ring record; other streams may still progress. */
                if(s->issued_count==ARRAY_SIZE(s->issued))continue;
                unsigned next=(s->issued_head+s->issued_count)&31;
                s->issued[next].start=card_submitted;s->issued[next].frames=(uint32_t)n;s->issued_count++;
            }
            s->device_pending+=(uint32_t)n;
            s->retained+=(uint32_t)n;
        }
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
    if(card&&any)card_submitted+=(uint32_t)frames;
    if (any||error) streams_changed();
    spin_unlock(&audio_lock, f);
    return any;
}
bool audio_mix(int16_t *out,int frames){return mix(out,frames,false);}
bool audio_card_mix(int16_t *out,int frames){return mix(out,frames,true);}
void audio_card_played(uint64_t ordinal){
    uint64_t f=spin_lock(&audio_lock);ordinal=MIN(ordinal,card_submitted);
    if(ordinal>card_consumed){
        card_consumed=ordinal;
        for(unsigned i=0;i<=MAX_STREAMS;i++){
            struct stream *s=streams[i];if(!s)continue;
            if(s->issued_head>=ARRAY_SIZE(s->issued)||s->issued_count>ARRAY_SIZE(s->issued)){s->clock_error=true;continue;}
            while(s->issued_count){
                unsigned at=s->issued_head;uint64_t start=s->issued[at].start;
                if(start>=ordinal)break;
                uint32_t consumed=(uint32_t)MIN(ordinal-start,(uint64_t)s->issued[at].frames);
                if(consumed>s->device_pending){s->clock_error=true;break;}
                s->device_pending-=consumed;s->issued[at].start+=consumed;s->issued[at].frames-=consumed;
                if(s->issued[at].frames)break;
                s->issued_head=(at+1)&31;s->issued_count--;
            }
            if(s->retained>s->device_pending)s->retained=s->device_pending; /* played: the writer may reuse it */
        }
        streams_changed();
    }
    spin_unlock(&audio_lock,f);
}

bool audio_pending(void) {
    for (int i = 0; i <= MAX_STREAMS; i++)
        if (streams[i]&&!streams[i]->clock_error&&streams[i]->head-streams[i]->tail>=AUDIO_FRAME&&
            streams[i]->head-streams[i]->tail<=RING_BYTES) return true;
    return false;
}

bool audio_ready(int frames, bool low) {
    bool any = false, whole = true;
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s || s->clock_error || s->head - s->tail > RING_BYTES) continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        if (!have) continue;
        any = true;
        if (have < (uint32_t)frames) whole = false;
    }
    return any && (whole || low);
}

/* audio_tick has work: frames wait, or the card has not yet played all it was given. */
bool audio_needs_tick(void) { return audio_pending() || card_consumed != card_submitted; }

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
    /* A card may finish already-issued DMA while remote owns new PCM. Its
       fill() must not mix from our streams in remote mode; idle is I/O-free. */
    if (card_fill) card_fill();
    else if (!remote_users) audio_mix(NULL, (int)(elapsed * AUDIO_RATE / 1000)); /* no card: play into nothing */
}

void audio_set_card(const char *name, void (*fill)(void), uint64_t (*unissue)(bool take)) {
    bool changed = name != card_name;
    card_name = name;
    card_fill = fill;
    card_unissue = unissue;
    if (changed) watched_changed(&output_watched);
}
const char *audio_card(void) { return card_name; }

/* From a remote session's thread (vm/rdp.c, a kernel thread: the BSP). */
void audio_remote_attach(void) {
    uint64_t f = spin_lock(&audio_lock);
    bool first = remote_users++ == 0;
    spin_unlock(&audio_lock, f);
    if (first) watched_changed(&output_watched); /* "Remote desktop" plays the sound now */
}
void audio_remote_detach(void) {
    uint64_t f = spin_lock(&audio_lock);
    bool last = remote_users == 1;
    if (remote_users > 0) remote_users--;
    spin_unlock(&audio_lock, f);
    if (last) watched_changed(&output_watched);
}
bool audio_remote(void) { return remote_users > 0; }
const char *audio_output(void) { return remote_users ? "Remote desktop" : card_name; }

/* ---- the volume and muting: the user's, kept in AppData\Nocturne\Sound.conf (sys/keep.c) ---- */

static bool sound_line(const char *key, const char *value, bool apply);
static size_t sound_text(char *out, size_t cap);
static struct keep sound_keep = {.name = "Sound", .line = sound_line, .text = sound_text};

/* The volume and muting as v and m; whoever changes them (a program, the taskbar's volume control,
   the document read back), the watches hear of it and the change is kept. */
static void sound_put(int v, bool m) {
    bool changed = v != volume || m != muted;
    volume = v;
    muted = m;
    gain = muted ? 0 : volume * volume * 65536 / 10000;
    if (!changed) return;
    keep_changed(&sound_keep);
    watched_changed(&output_watched);
}
int audio_volume(void) { return volume; }
bool audio_muted(void) { return muted; }
void audio_set_volume(int percent) { sound_put(MAX(0, MIN(100, percent)), false); }
void audio_set_muted(bool m) { sound_put(volume, m); }

/* Sound.conf: volume=<0-100>, muted=on|off. */
static bool sound_line(const char *key, const char *value, bool apply) {
    int64_t v;
    bool on = !strcmp(value, "on"), off = !strcmp(value, "off");
    if (!strcmp(key, "volume")) {
        if (apply && keep_number(value, 0, 100, &v)) sound_put((int)v, muted);
        return true;
    }
    if (!strcmp(key, "muted")) {
        if (apply && (on || off)) sound_put(volume, on);
        return true;
    }
    return false;
}

static size_t sound_text(char *out, size_t cap) {
    uint64_t f = irq_save();
    int v = volume;
    bool m = muted;
    irq_restore(f);
    size_t n = 0;
    keep_put(out, cap, &n, "# The volume and muting. Nocturne keeps this: change them in Settings or with the taskbar's speaker.\n");
    keep_put(out, cap, &n, "volume=%d\nmuted=%s\n", v, m ? "on" : "off");
    return n;
}

int64_t audio_output_watch(void) { return watched_open(&output_watched); }

static uint8_t system_ring[RING_BYTES];
static struct stream system_stream = {.ring = system_ring};

/* a triangle wave (no floating point in the kernel) that fades out, with a 2 ms start */
void audio_system_tone(int hz, int ms) {
    struct stream *s = &system_stream;
    uint64_t f = spin_lock(&audio_lock);
    streams[MAX_STREAMS] = s;
    uint32_t head = s->head, queued = head - s->tail, retained = s->retained;
    spin_unlock(&audio_lock, f);
    if (hz <= 0 || queued >= AUDIO_FRAME) return;
    /* the queue's worth at most, and never over what a card may still give back */
    int room = (int)((RING_BYTES - retained * AUDIO_FRAME) / AUDIO_FRAME);
    int frames = MIN(ms * (AUDIO_RATE / 1000), MIN((int)(QUEUE_BYTES / AUDIO_FRAME), room)), period = AUDIO_RATE / hz;
    for (int k = 0; k < frames; k++) {
        int ph = k % period, tri = ph < period / 2 ? 4 * ph - period : 3 * period - 4 * ph; /* -period..period */
        int amp = 7000 * (frames - k) / frames;
        if (k < 96) amp = amp * k / 96;
        int16_t v = (int16_t)(tri * amp / period);
        int16_t *at = (int16_t *)(s->ring + (head + (uint32_t)k * AUDIO_FRAME) % RING_BYTES);
        at[0] = at[1] = v;
    }
    f = spin_lock(&audio_lock);
    s->head += (uint32_t)frames * AUDIO_FRAME;
    spin_unlock(&audio_lock, f);
}

/* ---- the apps' streams (docs/nocturne-native-abi.md §9) ---- */

static struct stream *stream_of(struct handle *h) { return h->object->priv; }

static bool stream_broken(const struct stream *s) {
    return s->clock_error||s->head-s->tail>RING_BYTES||s->issued_head>=ARRAY_SIZE(s->issued)||
           s->issued_count>ARRAY_SIZE(s->issued);
}

/* No room: the queue is full, the ring holds no more beside what the card may give back, or the
   card's descriptors all carry this stream's frames. */
static bool stream_full(const struct stream *s) {
    uint32_t queued = s->head - s->tail;
    return queued >= QUEUE_BYTES || queued + s->retained * AUDIO_FRAME >= RING_BYTES ||
           (card_fill && !remote_users && s->issued_count == ARRAY_SIZE(s->issued));
}
/* Bytes a writer may add past head now. */
static uint32_t stream_room(const struct stream *s) {
    if (stream_full(s)) return 0;
    uint32_t queued = s->head - s->tail;
    return MIN(QUEUE_BYTES - queued, RING_BYTES - queued - s->retained * AUDIO_FRAME);
}

/* The frames of s a card holds from ordinal `at` on. */
static uint32_t frames_from(const struct stream *s, uint64_t at) {
    uint32_t back = 0;
    for (unsigned k = 0; k < s->issued_count && k < ARRAY_SIZE(s->issued); k++) {
        unsigned i = (s->issued_head + k) & 31;
        uint64_t end = s->issued[i].start + s->issued[i].frames;
        if (end > at) back += (uint32_t)(end - MAX(s->issued[i].start, at));
    }
    return back;
}

/* What the card's device has not begun to fetch goes back into the streams' queues, to be mixed
   again by the card's fill() (audio.h, audio_set_card). Only when s has PCM there, and when every
   stream still keeps its PCM from there in its ring (retained: not after a remote client mixed
   from it). The PCM keeps its order: each stream's frames from `at` on are the last it gave the
   card, just before its tail. audio_lock held, on the BSP (the card's unissue). */
static bool take_back(struct stream *s) {
    if (!card_unissue || remote_users) return false;
    uint64_t at = card_unissue(false);
    if (at >= card_submitted || !frames_from(s, at)) return false;
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *o = streams[i];
        if (o && !stream_broken(o) && frames_from(o, at) > o->retained) return false;
    }
    at = card_unissue(true); /* the engine only moved on since: never an earlier ordinal */
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *o = streams[i];
        if (!o || stream_broken(o)) continue;
        uint32_t back = frames_from(o, at);
        while (o->issued_count) {
            unsigned last = (o->issued_head + o->issued_count - 1) & 31;
            if (o->issued[last].start >= at) { o->issued_count--; continue; }
            if (o->issued[last].start + o->issued[last].frames > at)
                o->issued[last].frames = (uint32_t)(at - o->issued[last].start);
            break;
        }
        o->tail -= back * AUDIO_FRAME;
        o->device_pending -= back;
        o->retained -= back;
    }
    card_submitted = at;
    return true;
}

/* s's PCM goes at once, with what of it the card can still give back; the card mixes the other
   streams' again. The clock starts a new generation. BSP (the card's unissue and fill), interrupts
   off. */
static void stream_drop(struct stream *s, bool flush) {
    spin_take(&audio_lock);
    bool taken = s->device_pending && take_back(s);
    s->tail = s->head;
    s->committed_bytes = 0; s->issued_head = s->issued_count = 0; s->device_pending = 0; s->retained = 0;
    s->generation++; s->clock_error = false;
    if (flush && audio_trace_active) for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) trace.stream[i].flushes++;
    streams_changed();
    spin_give(&audio_lock);
    if (taken && card_fill) card_fill();
}

/* Without audio_lock (an events wait asks under the object's lock, which audio_lock comes before):
   every change notifies the object after it, and the wait asks again. */
static uint32_t stream_ready(struct handle *h) {
    struct stream *s = stream_of(h);
    return s->clock_error||!stream_full(s) ? N_EVENT_SENDABLE : 0;
}

/* The last handle closed: what is queued plays out (as when a program writes a sound and exits),
   unless the program is being stopped: then its sound stops at once, as with a flush. */
static void stream_close(struct handle *h) {
    struct stream *s = stream_of(h);
    uint64_t give_up = uptime_ms() + 1000;
    while (!current_task->killed && !s->clock_error && (s->head - s->tail >= AUDIO_FRAME||s->device_pending) && uptime_ms() < give_up) {
        uint64_t fl = irq_save();
        wq_wait_timeout(&audio_wq, 20);
        irq_restore(fl);
    }
    uint64_t fl = irq_save();
    if (current_task->killed) stream_drop(s, false);
    spin_take(&audio_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) {
            if (audio_trace_active) trace.stream[i].closes++;
            streams[i] = NULL;
        }
    spin_give(&audio_lock);
    irq_restore(fl);
}

static void stream_release(struct object *o) {
    struct stream *s = o->priv;
    kfree(s->ring);
    kfree(s);
}

static const struct object_ops stream_ops = {
    .kind = N_KIND_DEVICE, .ready = stream_ready, .close = stream_close, .release = stream_release,
};

bool audio_stream_is(struct handle *h) { return handle_is(h, &stream_ops); }

int audio_stream_open(struct handle **out) {
    struct stream *s = kzalloc(sizeof *s);
    if (s) s->ring = kmalloc(RING_BYTES);
    if (!s || !s->ring) { if (s) kfree(s); return -N_ERR_NO_MEMORY; }
    object_init(&s->object, &stream_ops, s);
    struct handle *h = handle_open(&s->object, true);
    if (!h) { stream_release(&s->object); return -N_ERR_NO_MEMORY; }
    uint64_t fl = spin_lock(&audio_lock);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS && slot < 0; i++) if (!streams[i]) slot = i;
    if (slot >= 0) streams[slot] = s;
    spin_unlock(&audio_lock, fl);
    if (slot < 0) { handle_close(h); return -N_ERR_BUSY; } /* nothing queued: no wait */
    *out = h;
    return 0;
}

/* Queue frames from the app's memory, on any CPU; returns the frames taken. Without wait a full
   stream (or one another thread is writing) gives N_ERR_NOT_READY. */
int64_t audio_stream_submit(struct handle *h, const void *frames, size_t count, bool wait) {
    struct stream *s = stream_of(h);
    if (count > SIZE_MAX / AUDIO_FRAME) return -N_ERR_TOO_LARGE;
    const uint8_t *p = frames;
    size_t n = count * AUDIO_FRAME, done = 0;
    int64_t error = 0;
    uint64_t fl = spin_lock(&audio_lock);
    for (;;) {
        if (!s->writer) { s->writer = current_task; break; }
        if (!wait) { spin_unlock(&audio_lock, fl); return -N_ERR_NOT_READY; }
        if (current_task->killed) { spin_unlock(&audio_lock, fl); return -N_ERR_STOPPED; }
        wq_wait_locked(&audio_wq, &audio_lock);
        spin_take(&audio_lock);
    }
    while (done < n) {
        if (stream_broken(s)) { error = -N_ERR_IO; break; }
        uint32_t room = stream_room(s);
        if (!room) {
            if (done) break; /* report what went in; the caller sends the rest */
            if (!wait) { error = -N_ERR_NOT_READY; break; }
            if (current_task->killed) { error = -N_ERR_STOPPED; break; }
            wq_wait_locked(&audio_wq, &audio_lock);
            spin_take(&audio_lock);
            continue;
        }
        uint32_t c = (uint32_t)MIN((size_t)room, n - done), at = s->head % RING_BYTES, first = MIN(c, RING_BYTES - at);
        spin_unlock(&audio_lock, fl);
        /* past head, which only the writer moves: the mixer does not read there until it does */
        bool ok = user_ok(p + done, c);
        if (ok) {
            memcpy(s->ring + at, p + done, first);
            memcpy(s->ring, p + done + first, c - first);
        }
        fl = spin_lock(&audio_lock);
        if (!ok) { error = -N_ERR_BAD_ADDRESS; break; }
        s->head += c;
        s->committed_bytes += c;
        trace_commit(s, c);
        done += c;
    }
    s->writer = NULL;
    wq_wake_all(&audio_wq); /* a writer waiting for this one */
    spin_unlock(&audio_lock, fl);
    return done ? (int64_t)(done / AUDIO_FRAME) : error;
}

/* How far ahead of the sink the stream is, and how much of it was played. */
static struct n_audio_status status_of(const struct stream *s) {
    uint32_t software=(s->head-s->tail)/AUDIO_FRAME;
    uint32_t hardware=remote_users?0:s->device_pending;
    uint64_t committed=s->committed_bytes/AUDIO_FRAME;
    uint64_t remaining=(uint64_t)software+hardware;
    return (struct n_audio_status){.queued_frames=software,.device_pending_frames=hardware,
        .played_frames=committed>=remaining?committed-remaining:0,.committed_frames=committed,
        .generation=s->generation,.flags=remote_users?N_AUDIO_CLOCK_ESTIMATED|N_AUDIO_CLOCK_REMOTE:card_fill?N_AUDIO_CLOCK_DEVICE:N_AUDIO_CLOCK_ESTIMATED};
}

/* Any CPU. */
int audio_stream_status(struct handle *h, struct n_audio_status *user_out) {
    struct stream *s = stream_of(h);
    uint64_t fl = spin_lock(&audio_lock);
    bool broken = stream_broken(s);
    struct n_audio_status status = status_of(s);
    spin_unlock(&audio_lock, fl);
    if (broken) return -N_ERR_IO;
    return copy_to_user(user_out, &status, sizeof status) < 0 ? -N_ERR_BAD_ADDRESS : 0;
}

/* The BSP: the card's device. */
int audio_stream_flush(struct handle *h) {
    struct stream *s = stream_of(h);
    uint64_t fl = irq_save();
    stream_drop(s, true);
    irq_restore(fl);
    return 0;
}

void audio_init(void) {
    watched_init(&output_watched, NULL); /* before a card is set */
    keep_register(&sound_keep);          /* read back by keep_start, before the first program */
    vm_audio_init(); /* no driver for real sound hardware yet */
}
