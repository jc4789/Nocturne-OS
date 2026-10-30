/* Sound: the apps' streams, the mixer and the pace of each sink (see audio.h). */
#include "dev/audio.h"
#include "gui/wm.h"
#include "sys/syscall.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "sys/object.h"
#include "sys/keep.h"
#include "sys/watch.h"
#include "abi.h"
#include "vm/vm.h"

#define MAX_STREAMS 8
#define QUEUE_BYTES (8192 * AUDIO_FRAME) /* 170 ms: what a writer may be ahead of the sink (SOUND_QUEUE) */
#define RING_BYTES  (2 * QUEUE_BYTES)    /* the queue, and the PCM a card may still give back (retained) */
#define RUNS        32                   /* a stream's runs in a sink's hands: an AC'97 card's whole ring */
#define NULL_BLOCK  (AUDIO_RATE / 100)   /* the sink of nothing plays 10 ms blocks, as a card its buffers */

/* A ring of bytes: the writer moves head, the mixer (a sink taking a block: a card's fill(), a
   remote client's sound thread, the sink of nothing's alarm) moves tail, both under audio_lock;
   head - tail is the number of bytes queued. The writer copies its PCM past head without the lock,
   and the mixer reads only up to head. Whole frames never wrap, since the ring holds a whole number
   of them. A block's share of the stream stays a run of ordinals in `issued` until its sink reports
   it played (audio_played); device_pending counts them. The `retained` frames just before tail
   were mixed into a card's buffers, which may give them back (take_back): the writer leaves them
   alone until the card has played them. */
struct stream {
    uint8_t *ring;
    uint32_t head, tail;
    uint64_t committed_bytes;
    uint32_t generation;
    /* Ordinals only: no stream or task pointer escapes into a sink's buffers, so close and reuse
       are safe. */
    struct {uint64_t start;uint32_t frames;} issued[RUNS];
    unsigned issued_head,issued_count;
    uint32_t device_pending;
    uint32_t retained; /* the last of device_pending, still in the ring before tail */
    bool clock_error;
    struct task *writer; /* the one submit copying past head; another waits for it */
    struct object object; /* an app's stream: what its handle reaches */
};

/* The card and the calls into it: its fill() and its unissue() (take_back) run under card_lock,
   and the card is set under it; a card's own code takes it (audio_card_lock) to change what those
   two read. The trace's card counters are under it too. Taken before audio_lock: fill() mixes with
   audio_card_mix. */
static struct spinlock card_lock;
/* The streams' queues and clocks (every field of a stream above but ring and object), the list of
   streams, the sinks' ordinals, the volume and muting, the remote clients, the sink of nothing, the
   system tone's writer, the card's identity (also under card_lock) and the trace's stream and
   service counters: any CPU, any context. Taken after card_lock, before a wait queue's and an
   object's lock and an alarm's (alarm_set). */
static struct spinlock audio_lock;
/* Every frame mixed for a sink has an ordinal: sink_mixed is the next one, and the sink has heard
   those before sink_played, at the clock time sink_played_at (the time every stream's played
   frames hold for). */
static uint64_t sink_mixed, sink_played, sink_played_at;

/* the last slot is the system's own stream, which no app owns */
static struct stream *streams[MAX_STREAMS + 1];

static struct wait_queue audio_wq; /* writers waiting for room or for the writer, closers waiting to drain */
static int volume = 80;            /* percent */
static bool muted;
static int32_t gain = 80 * 80 * 65536 / 10000; /* volume squared, 16.16: loudness feels linear */
/* Written under both locks; read under either, or (the atomic loads below) as a hint that may be a
   moment old: what plays the sound, whether a card does. */
static const char *card_name;
static void (*card_fill)(void);
static uint64_t (*card_unissue)(bool take);
/* The remote clients, in the order they came: the first plays the sound. Changed under audio_lock;
   read without it as a hint. */
static struct audio_remote *remotes;
static bool tone_writing; /* audio_system_tone is copying past the system stream's head */

static bool remote_now(void) { return __atomic_load_n(&remotes, __ATOMIC_RELAXED) != NULL; }

uint64_t audio_card_lock(void) { return spin_lock(&card_lock); }
void audio_card_unlock(uint64_t flags) { spin_unlock(&card_lock, flags); }
/* What SYS_AUDIO_OUTPUT reports (the volume, muting, what plays the sound) changed: its watches
   (SYS_AUDIO_OUTPUT_WATCH) hear of it, whoever changed it. */
static struct watched output_watched;

#define TRACE_SECONDS 22
bool audio_trace_active;
static struct {
    uint64_t start, last_service, max_service_gap, services, card_calls, restarts, filled;
    uint64_t sr_before[32], sr_after[32];
    struct {
        uint64_t frames, writes, last, max_gap, shortage, mix_calls;
        uint32_t min_queue, max_queue, pid, owner_changes, closes, flushes;
        uint64_t by_second[TRACE_SECONDS];
    } stream[MAX_STREAMS];
} trace; /* times are clock_now_ns */

/* audio_trace_active changes under both locks, and every writer of the trace looks at it under the
   lock it holds: from begin to end the counters are written, and after end nothing writes them. */
void audio_trace_begin(void) {
    /* WM invokes this before arming its timed interval. */
    uint64_t f = spin_lock(&card_lock);
    spin_take(&audio_lock);
    memset(&trace, 0, sizeof trace);
    for (int i = 0; i < MAX_STREAMS; i++) trace.stream[i].min_queue = QUEUE_BYTES / AUDIO_FRAME;
    trace.start = clock_now_ns(); trace.last_service = trace.start;
    audio_trace_active = true;
    spin_give(&audio_lock);
    spin_unlock(&card_lock, f);
}
void audio_trace_end(void) {
    uint64_t f = spin_lock(&card_lock), end = clock_now_ns();
    spin_take(&audio_lock);
    audio_trace_active = false;
    const char *card = card_name;
    int remote = remotes != NULL;
    bool quiet = muted;
    spin_give(&audio_lock);
    spin_unlock(&card_lock, f);
    /* a service is a block mixed for a sink */
    kprintf("audiotrace: elapsed_us=%lu service_calls=%lu max_service_gap_us=%lu card=%s remote=%d muted=%d\n",
            (end-trace.start)/NS_PER_US, trace.services, trace.max_service_gap/NS_PER_US,
            card ? card : "none", remote, quiet);
    kprintf("audiotrace: ac97_calls=%lu filled=%lu restart_actions=%lu raw_sr_low5_before_after\n",
            trace.card_calls, trace.filled, trace.restarts);
    for (int i = 0; i < 32; i++) if (trace.sr_before[i] || trace.sr_after[i])
        kprintf("audiotrace: sr=%x before=%lu after=%lu\n", i, trace.sr_before[i], trace.sr_after[i]);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!trace.stream[i].mix_calls && !trace.stream[i].writes && !trace.stream[i].closes) continue;
        uint64_t last = trace.stream[i].last ? trace.stream[i].last : trace.start;
        kprintf("audiotrace: slot=%d writer_pid=%u owner_changes=%u committed_frames=%lu writes=%lu max_commit_gap_us=%lu final_commit_age_us=%lu queue_min=%u queue_max=%u raw_shortage_frames=%lu mix_calls=%lu closes=%u flushes=%u\n",
                i, trace.stream[i].pid, trace.stream[i].owner_changes, trace.stream[i].frames,
                trace.stream[i].writes, trace.stream[i].max_gap/NS_PER_US, (end-last)/NS_PER_US,
                trace.stream[i].min_queue, trace.stream[i].max_queue, trace.stream[i].shortage,
                trace.stream[i].mix_calls, trace.stream[i].closes, trace.stream[i].flushes);
        for (int j = 0; j < TRACE_SECONDS; j++)
            kprintf("audiotrace: slot=%d second=%d committed_frames=%lu\n", i, j, trace.stream[i].by_second[j]);
    }
}
/* The card's fill(), card_lock held. */
void audio_trace_ac97(uint16_t before, uint16_t after, unsigned filled, bool restarted) {
    if (!audio_trace_active) return;
    trace.card_calls++; trace.filled += filled; trace.restarts += restarted;
    trace.sr_before[before & 31]++; trace.sr_after[after & 31]++;
}
/* audio_lock held: a block was mixed for a sink. */
static void trace_service(void) {
    if (!audio_trace_active) return;
    uint64_t at = clock_now_ns();
    trace.max_service_gap = MAX(trace.max_service_gap, at-trace.last_service);
    trace.last_service = at; trace.services++;
}
/* audio_lock held. */
static void trace_commit(struct stream *s, uint32_t bytes) {
    if (!audio_trace_active) return;
    uint64_t now = clock_now_ns();
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
        uint64_t second = (now-trace.start) / NS_PER_S;
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

/* audio_lock held: one block of `frames` for a sink, into out (NULL: the sink of nothing). keep:
   for a card, which may give it back (take_back). */
static bool mix_locked(int16_t *out, int frames, bool keep) {
    if(frames<=0||(unsigned)frames>RING_BYTES/AUDIO_FRAME)return false;
    if((uint64_t)frames>UINT64_MAX-sink_mixed)return false;
    bool any = false,error = false;
    if (out) memset(out, 0, (size_t)frames * AUDIO_FRAME);
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s) continue;
        if(s->head-s->tail>RING_BYTES||s->issued_head>=RUNS||s->issued_count>RUNS){s->clock_error=true;error=true;continue;}
        if(s->clock_error)continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        int n = (int)MIN((uint32_t)frames, have);
        if (audio_trace_active && i < MAX_STREAMS) {
            trace.stream[i].min_queue = MIN(trace.stream[i].min_queue, have);
            trace.stream[i].shortage += (uint32_t)frames - (uint32_t)n;
            trace.stream[i].mix_calls++;
        }
        if (!n) continue;
        if(!keep)s->retained=0; /* a card's PCM no longer lies just before tail */
        unsigned last=(s->issued_head+s->issued_count-1)&(RUNS-1);
        if(s->issued_count&&s->issued[last].start+s->issued[last].frames==sink_mixed)
            s->issued[last].frames+=(uint32_t)n;
        else{
            /* A sink that stopped reporting (a lost device snapshot, a client that does not
               confirm) must not turn media input into a kernel panic or fictitious consumption:
               the PCM stays in its stream until a report frees a run; other streams still
               progress. */
            if(s->issued_count==RUNS)continue;
            unsigned next=(s->issued_head+s->issued_count)&(RUNS-1);
            s->issued[next].start=sink_mixed;s->issued[next].frames=(uint32_t)n;s->issued_count++;
        }
        s->device_pending+=(uint32_t)n;
        if(keep)s->retained+=(uint32_t)n;
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
    if(any){sink_mixed+=(uint32_t)frames;trace_service();}
    if (any||error) streams_changed();
    return any;
}
bool audio_card_mix(int16_t *out,int frames){
    uint64_t f=spin_lock(&audio_lock);
    bool any=mix_locked(out,frames,true);
    spin_unlock(&audio_lock,f);
    return any;
}

/* audio_lock held. */
static void played_locked(uint64_t ordinal,uint64_t at){
    ordinal=MIN(ordinal,sink_mixed);
    if(ordinal<=sink_played)return;
    sink_played=ordinal;sink_played_at=at;
    for(unsigned i=0;i<=MAX_STREAMS;i++){
        struct stream *s=streams[i];if(!s)continue;
        if(s->issued_head>=RUNS||s->issued_count>RUNS){s->clock_error=true;continue;}
        while(s->issued_count){
            unsigned at_run=s->issued_head;uint64_t start=s->issued[at_run].start;
            if(start>=ordinal)break;
            uint32_t consumed=(uint32_t)MIN(ordinal-start,(uint64_t)s->issued[at_run].frames);
            if(consumed>s->device_pending){s->clock_error=true;break;}
            s->device_pending-=consumed;s->issued[at_run].start+=consumed;s->issued[at_run].frames-=consumed;
            if(s->issued[at_run].frames)break;
            s->issued_head=(at_run+1)&(RUNS-1);s->issued_count--;
        }
        if(s->retained>s->device_pending)s->retained=s->device_pending; /* played: the writer may reuse it */
    }
    streams_changed();
}
void audio_played(uint64_t ordinal,uint64_t at){
    uint64_t f=spin_lock(&audio_lock);
    played_locked(ordinal,at);
    spin_unlock(&audio_lock,f);
}

/* audio_lock held. */
static bool pending_locked(void) {
    for (int i = 0; i <= MAX_STREAMS; i++)
        if (streams[i]&&!streams[i]->clock_error&&streams[i]->head-streams[i]->tail>=AUDIO_FRAME&&
            streams[i]->head-streams[i]->tail<=RING_BYTES) return true;
    return false;
}

bool audio_pending(void) {
    uint64_t f = spin_lock(&audio_lock);
    bool any = pending_locked();
    spin_unlock(&audio_lock, f);
    return any;
}

/* audio_lock held. */
static bool ready_locked(int frames, bool low) {
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

bool audio_ready(int frames, bool low) {
    uint64_t f = spin_lock(&audio_lock);
    bool ready = ready_locked(frames, low);
    spin_unlock(&audio_lock, f);
    return ready;
}

/* ---- the sink of nothing ----
   While there is no card and no remote client, an ideal card plays the sound: NULL_BLOCK blocks
   back to back from null_start, the first mixed at once and each next one as the one before ends
   (null_alarm). The position heard is the time since null_start in frames, not a sum of blocks, so
   a stream is consumed exactly as fast as it would be played however late an alarm comes, and a
   program takes as long as its sound does. It stops when a block ends with nothing to mix after
   it; a stream that gets PCM starts it again. audio_lock guards it. */
static struct alarm null_alarm;
static bool null_running;
static uint64_t null_start, null_base; /* when it began to play, and sink_mixed then */

/* The frames that play in ns at AUDIO_RATE (rounded down), and the ns that frames take (up). */
static uint64_t frames_in(uint64_t ns) { return ns / NS_PER_S * AUDIO_RATE + ns % NS_PER_S * AUDIO_RATE / NS_PER_S; }
static uint64_t ns_for(uint64_t frames) {
    return frames / AUDIO_RATE * NS_PER_S + (frames % AUDIO_RATE * NS_PER_S + AUDIO_RATE - 1) / AUDIO_RATE;
}

/* audio_lock held, now read under it: the blocks due by now are mixed, with the one that plays
   now; what has been heard is reported, and the alarm set for the end of the block that plays. */
static void null_play(uint64_t now) {
    uint64_t heard = frames_in(now - null_start), mixed = sink_mixed - null_base;
    while (mixed <= heard && mix_locked(NULL, NULL_BLOCK, false)) mixed += NULL_BLOCK;
    played_locked(null_base + MIN(heard, mixed), now);
    if (mixed > heard) alarm_set(&null_alarm, null_start + ns_for(mixed));
    else null_running = false; /* played out, and nothing waits */
}
static void null_begin(uint64_t now) {
    null_running = true;
    null_start = now;
    null_base = sink_mixed;
    null_play(now);
}
/* A card or a remote client plays the sound from now: the block that plays is over. An alarm still
   armed finds the sink stopped (or begun again: then it only sets itself to the block's end). */
static void null_stop(uint64_t now) {
    if (!null_running) return;
    played_locked(sink_mixed, now);
    null_running = false;
}
static void null_fire(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    uint64_t f = spin_lock(&audio_lock);
    if (null_running) null_play(clock_now_ns());
    spin_unlock(&audio_lock, f);
}

/* ---- starting the sink ---- */

static void remote_wake(struct audio_remote *r) {
    __atomic_add_fetch(&r->signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&r->wq);
}

/* audio_lock held: a stream got PCM. What plays the sound starts if it waits for it: the remote
   client's sound thread when a block it wants is there, the sink of nothing when it is stopped.
   True when the card has played all it was given (it is idle, or is about to stop): the caller
   then asks it with card_start, once it has given audio_lock back. A card that has not played
   everything will call fill() itself (audio_set_card), and sees this PCM then. */
static bool sink_fed_locked(void) {
    struct audio_remote *r = remotes;
    if (r) {
        if (r->wants && ready_locked(r->wants, r->low)) {
            r->wants = 0;
            remote_wake(r);
        }
        return false;
    }
    if (card_fill) return sink_played == sink_mixed;
    if (!null_running) null_begin(clock_now_ns());
    return false;
}
static void card_start(void) {
    uint64_t f = spin_lock(&card_lock);
    if (card_fill) card_fill();
    spin_unlock(&card_lock, f);
}

void audio_set_card(const char *name, void (*fill)(void), uint64_t (*unissue)(bool take)) {
    uint64_t f = spin_lock(&card_lock);
    spin_take(&audio_lock);
    bool changed = name != card_name;
    __atomic_store_n(&card_name, name, __ATOMIC_RELAXED);
    __atomic_store_n(&card_fill, fill, __ATOMIC_RELAXED);
    card_unissue = unissue;
    if (fill) null_stop(clock_now_ns());
    spin_give(&audio_lock);
    spin_unlock(&card_lock, f);
    if (changed) watched_changed(&output_watched);
}
const char *audio_card(void) { return __atomic_load_n(&card_name, __ATOMIC_RELAXED); }

/* ---- remote clients (vm/rdp.c), from their session's threads on any CPU ---- */

void audio_remote_attach(struct audio_remote *r) {
    uint64_t f = spin_lock(&audio_lock);
    r->next = NULL;
    r->wants = 0;
    r->low = false;
    r->given = 0;
    struct audio_remote **p = &remotes;
    while (*p) p = &(*p)->next;
    bool first = p == &remotes;
    if (first) {
        null_stop(clock_now_ns());
        __atomic_store_n(&remotes, r, __ATOMIC_RELAXED);
    } else {
        *p = r;
    }
    spin_unlock(&audio_lock, f);
    if (first) watched_changed(&output_watched); /* "Remote desktop" plays the sound now */
}
void audio_remote_detach(struct audio_remote *r) {
    uint64_t f = spin_lock(&audio_lock);
    bool playing = remotes == r;
    struct audio_remote **p = &remotes;
    while (*p && *p != r) p = &(*p)->next;
    if (*p == r) {
        if (playing) __atomic_store_n(&remotes, r->next, __ATOMIC_RELAXED);
        else *p = r->next;
    }
    bool start = false;
    if (playing) {
        played_locked(r->given, clock_now_ns()); /* its client will not say it played them */
        if (remotes) remote_wake(remotes);     /* the sound goes to the next one */
        else start = pending_locked() && sink_fed_locked();
    }
    spin_unlock(&audio_lock, f);
    if (start) card_start();
    if (playing && !remote_now()) watched_changed(&output_watched);
}
bool audio_remote(void) { return remote_now(); }
const char *audio_output(void) { return remote_now() ? "Remote desktop" : audio_card(); }

bool audio_remote_ready(struct audio_remote *r, int frames, bool low) {
    uint64_t f = spin_lock(&audio_lock);
    bool ready = remotes == r && ready_locked(frames, low);
    r->wants = ready ? 0 : frames;
    r->low = low;
    spin_unlock(&audio_lock, f);
    return ready;
}
bool audio_remote_mix(struct audio_remote *r, int16_t *out, int frames, uint64_t *end) {
    uint64_t f = spin_lock(&audio_lock);
    bool any = remotes == r && mix_locked(out, frames, false);
    if (any) *end = r->given = sink_mixed;
    spin_unlock(&audio_lock, f);
    return any;
}

/* ---- the volume and muting: the user's, kept in AppData\Nocturne\Sound.conf (sys/keep.c) ---- */

static bool sound_line(const char *key, const char *value, bool apply);
static size_t sound_text(char *out, size_t cap);
static struct keep sound_keep = {.name = "Sound", .line = sound_line, .text = sound_text};

/* The volume and muting as v and m, KEEP for either one leaves it as it is when the change is made;
   whoever changes them (a program, the taskbar's volume control, the document read back), the
   watches and the taskbar hear of it and the change is kept. */
#define KEEP (-1)
static void sound_put(int v, int m) {
    uint64_t f = spin_lock(&audio_lock);
    if (v == KEEP) v = volume;
    bool quiet = m == KEEP ? muted : m != 0;
    bool changed = v != volume || quiet != muted;
    __atomic_store_n(&volume, v, __ATOMIC_RELAXED);
    __atomic_store_n(&muted, quiet, __ATOMIC_RELAXED);
    gain = quiet ? 0 : v * v * 65536 / 10000;
    spin_unlock(&audio_lock, f);
    if (!changed) return;
    keep_changed(&sound_keep);
    watched_changed(&output_watched);
    wm_taskbar_changed();
}
/* A moment old at most: the watches hear of every change after it is made. */
int audio_volume(void) { return __atomic_load_n(&volume, __ATOMIC_RELAXED); }
bool audio_muted(void) { return __atomic_load_n(&muted, __ATOMIC_RELAXED); }
void audio_set_volume(int percent) { sound_put(MAX(0, MIN(100, percent)), false); }
void audio_set_muted(bool m) { sound_put(KEEP, m); }

/* Sound.conf: volume=<0-100>, muted=on|off. */
static bool sound_line(const char *key, const char *value, bool apply) {
    int64_t v;
    bool on = !strcmp(value, "on"), off = !strcmp(value, "off");
    if (!strcmp(key, "volume")) {
        if (apply && keep_number(value, 0, 100, &v)) sound_put((int)v, KEEP);
        return true;
    }
    if (!strcmp(key, "muted")) {
        if (apply && (on || off)) sound_put(KEEP, on);
        return true;
    }
    return false;
}

static size_t sound_text(char *out, size_t cap) {
    uint64_t f = spin_lock(&audio_lock);
    int v = volume;
    bool m = muted;
    spin_unlock(&audio_lock, f);
    size_t n = 0;
    keep_put(out, cap, &n, "# The volume and muting. Nocturne keeps this: change them in Settings or with the taskbar's speaker.\n");
    keep_put(out, cap, &n, "volume=%d\nmuted=%s\n", v, m ? "on" : "off");
    return n;
}

int64_t audio_output_watch(void) { return watched_open(&output_watched); }

static uint8_t system_ring[RING_BYTES];
static struct stream system_stream = {.ring = system_ring};

/* a triangle wave (no floating point in the kernel) that fades out, with a 2 ms start; one caller
   at a time writes it (tone_writing), another one's tone is dropped as one that still sounds is */
void audio_system_tone(int hz, int ms) {
    struct stream *s = &system_stream;
    uint64_t f = spin_lock(&audio_lock);
    streams[MAX_STREAMS] = s;
    uint32_t head = s->head, queued = head - s->tail, retained = s->retained;
    bool drop = hz <= 0 || queued >= AUDIO_FRAME || tone_writing;
    if (!drop) tone_writing = true;
    spin_unlock(&audio_lock, f);
    if (drop) return;
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
    tone_writing = false;
    bool start = frames > 0 && sink_fed_locked();
    spin_unlock(&audio_lock, f);
    if (start) card_start();
}

/* ---- the apps' streams (docs/nocturne-native-abi.md §9) ---- */

static struct stream *stream_of(struct handle *h) { return h->object->priv; }

static bool stream_broken(const struct stream *s) {
    return s->clock_error||s->head-s->tail>RING_BYTES||s->issued_head>=RUNS||s->issued_count>RUNS;
}

/* No room: the queue is full, the ring holds no more beside what the card may give back, or all of
   the stream's runs are in a sink's hands (the sink skips it until one is played). */
static bool stream_full(const struct stream *s) {
    uint32_t queued = s->head - s->tail;
    return queued >= QUEUE_BYTES || queued + s->retained * AUDIO_FRAME >= RING_BYTES || s->issued_count == RUNS;
}
/* Bytes a writer may add past head now. */
static uint32_t stream_room(const struct stream *s) {
    if (stream_full(s)) return 0;
    uint32_t queued = s->head - s->tail;
    return MIN(QUEUE_BYTES - queued, RING_BYTES - queued - s->retained * AUDIO_FRAME);
}

/* The frames of s a sink holds from ordinal `at` on. */
static uint32_t frames_from(const struct stream *s, uint64_t at) {
    uint32_t back = 0;
    for (unsigned k = 0; k < s->issued_count && k < RUNS; k++) {
        unsigned i = (s->issued_head + k) & (RUNS - 1);
        uint64_t end = s->issued[i].start + s->issued[i].frames;
        if (end > at) back += (uint32_t)(end - MAX(s->issued[i].start, at));
    }
    return back;
}

/* What the card's device has not begun to fetch goes back into the streams' queues, to be mixed
   again by the card's fill() (audio.h, audio_set_card). Only when s has PCM there, and when every
   stream still keeps its PCM from there in its ring (retained: not after a remote client mixed
   from it). The PCM keeps its order: each stream's frames from `at` on are the last it gave the
   card, just before its tail. card_lock (the card's unissue) and audio_lock held. */
static bool take_back(struct stream *s) {
    if (!card_unissue || remotes) return false;
    uint64_t at = card_unissue(false);
    if (at >= sink_mixed || !frames_from(s, at)) return false;
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
            unsigned last = (o->issued_head + o->issued_count - 1) & (RUNS - 1);
            if (o->issued[last].start >= at) { o->issued_count--; continue; }
            if (o->issued[last].start + o->issued[last].frames > at)
                o->issued[last].frames = (uint32_t)(at - o->issued[last].start);
            break;
        }
        o->tail -= back * AUDIO_FRAME;
        o->device_pending -= back;
        o->retained -= back;
    }
    sink_mixed = at;
    return true;
}

/* s's PCM goes at once, with what of it the card can still give back; the card mixes the other
   streams' again. Nothing mixes s's old PCM after this: its queue is empty, and no sink keeps a
   block of it unmixed. The clock starts a new generation. Any CPU: the card's unissue and fill
   run under card_lock. */
static void stream_drop(struct stream *s, bool flush) {
    uint64_t f = spin_lock(&card_lock);
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
    spin_unlock(&card_lock, f);
}

/* Without audio_lock (an events wait asks under the object's lock, which audio_lock comes before):
   a hint read from the fields as they are; every change notifies the object after it, and the wait
   asks again. */
static uint32_t stream_ready(struct handle *h) {
    struct stream *s = stream_of(h);
    return s->clock_error||!stream_full(s) ? N_EVENT_SENDABLE : 0;
}

/* The last handle closed: what is queued plays out (as when a program writes a sound and exits),
   for a second at most, unless the program is being stopped: then its sound stops at once, as
   with a flush. */
static void stream_close(struct handle *h) {
    struct stream *s = stream_of(h);
    uint64_t give_up = clock_after(NS_PER_S);
    /* the mixer and the sinks' reports wake audio_wq at each change, and a kill wakes the task */
    uint64_t fl = spin_lock(&audio_lock);
    while (!current_task->killed && !s->clock_error && (s->head - s->tail >= AUDIO_FRAME||s->device_pending)) {
        bool woken = wq_wait_locked_until(&audio_wq, &audio_lock, give_up);
        spin_take(&audio_lock);
        if (!woken) break;
    }
    spin_unlock(&audio_lock, fl);
    if (current_task->killed) stream_drop(s, false);
    fl = spin_lock(&audio_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) {
            if (audio_trace_active) trace.stream[i].closes++;
            streams[i] = NULL;
        }
    spin_unlock(&audio_lock, fl);
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
        int bad = copy_from_user(s->ring + at, p + done, first);
        if (!bad) bad = copy_from_user(s->ring, p + done + first, c - first);
        fl = spin_lock(&audio_lock);
        if (bad) { error = -N_ERR_BAD_ADDRESS; break; }
        s->head += c;
        s->committed_bytes += c;
        trace_commit(s, c);
        done += c;
    }
    s->writer = NULL;
    wq_wake_all(&audio_wq); /* a writer waiting for this one */
    bool start = done && sink_fed_locked();
    spin_unlock(&audio_lock, fl);
    if (start) card_start();
    return done ? (int64_t)(done / AUDIO_FRAME) : error;
}

/* How far ahead of the sink the stream is, and how much of it was played: its frames before the
   last ordinal its sink reported, heard at sink_played_at (the ABI has no field for that time
   yet). */
static struct n_audio_status status_of(const struct stream *s) {
    uint32_t software=(s->head-s->tail)/AUDIO_FRAME;
    uint64_t committed=s->committed_bytes/AUDIO_FRAME;
    uint64_t remaining=(uint64_t)software+s->device_pending;
    return (struct n_audio_status){.queued_frames=software,.device_pending_frames=s->device_pending,
        .played_frames=committed>=remaining?committed-remaining:0,.committed_frames=committed,
        .generation=s->generation,.flags=remotes?N_AUDIO_CLOCK_REMOTE:card_fill?N_AUDIO_CLOCK_DEVICE:N_AUDIO_CLOCK_ESTIMATED};
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

/* Any CPU. */
int audio_stream_flush(struct handle *h) {
    stream_drop(stream_of(h), true);
    return 0;
}

void audio_init(void) {
    watched_init(&output_watched, NULL); /* before a card is set */
    keep_register(&sound_keep);          /* read back by keep_start, before the first program */
    alarm_init(&null_alarm, null_fire);
    vm_audio_init(); /* no driver for real sound hardware yet */
}
