/* Sound: the apps' streams, the mixer, the outputs and the thread that feeds them (see audio.h). */
#include "dev/audio.h"
#include "gui/wm.h"
#include "sys/syscall.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "sys/object.h"
#include "sys/keep.h"
#include "sys/watch.h"
#include "abi.h"
#include "vm/vm.h"
#include "dev/audio_pace.h"

#define MAX_STREAMS 8
#define QUEUE_BYTES (8192 * AUDIO_FRAME) /* 170 ms: what a writer may be ahead of the sink (SOUND_QUEUE) */
#define RING_BYTES  (2 * QUEUE_BYTES)    /* the queue, and the PCM an output may still give back (retained) */
#define NULL_RUNS   2                    /* in the sink of nothing's hands: the block that plays, and its next */
#define NULL_BLOCK  (AUDIO_RATE / 100)   /* the sink of nothing plays 10 ms blocks, as a device its buffers */
#define MIX_CHUNK   64                   /* frames summed at a time, on the mixing thread's stack */

/* A ring of bytes: the writer moves head, the mixer (a sink taking a block: an output's fill(), a
   remote client's sound thread, the sink of nothing's alarm) moves tail, both under audio_lock;
   head - tail is the number of bytes queued. The writer copies its PCM past head without the lock,
   and the mixer reads only up to head. Whole frames never wrap, since the ring holds a whole number
   of them. A block's share of the stream stays a run of ordinals in `issued` until its sink reports
   it played (audio_played); device_pending counts them. Next to each other runs are one, so a stream
   has at most one for each block in its sink's hands: `issued` is a ring with room for what the sinks
   there are may hold (runs_most, runs_grow), a power of two. The `retained` frames just before tail
   were mixed into an output's buffers, which may give them back (take_back): the writer leaves them
   alone until the output has played them. The `held` frames just before tail are being summed
   outside the lock (mix_unlock): the writer leaves them alone too, and the stream is not freed. */
struct run {uint64_t start;uint32_t frames;};
struct stream {
    uint8_t *ring;
    uint32_t head, tail;
    uint64_t committed_bytes;
    uint32_t generation;
    /* Ordinals only: no stream or task pointer escapes into a sink's buffers, so close and reuse
       are safe. */
    struct run *issued;
    unsigned runs_room,issued_head,issued_count;
    uint32_t device_pending;
    uint32_t retained; /* the last of device_pending, still in the ring before tail */
    uint32_t held;     /* being summed outside the lock */
    bool clock_error;
    uint64_t dry_since; /* when its sink had played all it had, while no PCM has come since; or 0 */
    uint64_t underruns, underrun_ns;
    struct thread *writer; /* the one submit copying past head; another waits for it */
    struct object object; /* an app's stream: what its handle reaches */
};

/* The outputs, and every call into them: open, close, fill and unissue run one at a time, in threads
   that may sleep. The audio thread holds it around fill(), a flush around its taking back, a driver
   as it adds an output or says what changed. Taken before audio_lock. */
static struct mutex sink_mutex;
/* The streams' queues and clocks (every field of a stream above but ring and object), the list of
   streams, the sinks' ordinals, the volume and muting, the remote clients, the sink of nothing, the
   system tone's writer and which output plays: any CPU, any context. Taken after sink_mutex, before
   a wait queue's (an object's waiters', object_notify), an alarm's lock (alarm_set) and a driver's
   lock around its engine (unissue). */
static struct spinlock audio_lock;
/* Every frame of the streams' timeline mixed for a sink has an ordinal: sink_mixed is the next one,
   and the sink has heard those before sink_played, at the clock time sink_played_at (the time every
   stream's played frames hold for). */
static uint64_t sink_mixed, sink_played, sink_played_at;

/* the last slot is the system's own stream, which no app owns */
static struct stream *streams[MAX_STREAMS + 1];
/* The runs each stream has room for: the most any sink there is may hold, under audio_lock. A stream
   short of room (no memory for more, just now) skips blocks until a report frees a run. */
static unsigned runs_most = NULL_RUNS;

static struct wait_queue audio_wq; /* writers waiting for room or for the writer, closers waiting to drain */
static int volume = 80;            /* percent */
static bool muted;
static int32_t gain = 80 * 80 * 65536 / 10000; /* volume squared, 16.16: loudness feels linear */
/* The outputs, in the order they came, and the one the user chose (NULL: the best there is), under
   sink_mutex. An output lives for good once added. */
static struct audio_sink *sinks, *chosen;
/* The output that plays: written under both locks, read under either, or (the atomic load in
   audio_output) as a hint that may be a moment old. */
static struct audio_sink *active;
/* The remote clients, in the order they came: the first plays the sound. Changed under audio_lock;
   read without it as a hint. */
static struct audio_remote *remotes;
static bool tone_writing; /* audio_system_tone is copying past the system stream's head */

/* The thread that fills the output that plays (audio_thread). */
static struct thread *filler;
static struct wait_queue filler_wq;
static uint64_t filler_signals;

static bool remote_now(void) { return __atomic_load_n(&remotes, __ATOMIC_RELAXED) != NULL; }

void audio_sink_wake(void) {
    __atomic_add_fetch(&filler_signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&filler_wq);
}

/* What SYS_AUDIO_OUTPUT reports (the volume, muting, what plays the sound) changed: its watches
   (SYS_AUDIO_OUTPUT_WATCH) hear of it, whoever changed it. */
static struct watched output_watched;

/* The apps' streams moved on: waits for room look again. audio_lock held. */
static void streams_changed(void) {
    wq_wake_all(&audio_wq);
    for (int i = 0; i < MAX_STREAMS; i++) if (streams[i]) object_notify(&streams[i]->object);
}

/* A stream's share of one block being mixed. */
struct mix_part {
    struct stream *s;
    uint32_t tail, frames;
};

/* audio_lock held: one block of `frames` is taken from the streams and has its ordinals, so the
   lock is held only for the positions. Each stream's share in parts, the number of them returned;
   parts NULL: for the sink of nothing, which hears none of it. keep: for an output, which may give
   it back (take_back). */
static unsigned mix_claim(struct mix_part *parts, int frames, bool keep) {
    if(frames<=0||(unsigned)frames>RING_BYTES/AUDIO_FRAME)return 0;
    if((uint64_t)frames>UINT64_MAX-sink_mixed)return 0;
    unsigned count = 0;
    bool error = false;
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s) continue;
        if(s->head-s->tail>RING_BYTES||s->issued_head>=s->runs_room||s->issued_count>s->runs_room){s->clock_error=true;error=true;continue;}
        if(s->clock_error)continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        int n = (int)MIN((uint32_t)frames, have);
        if (!n) continue;
        if(!keep)s->retained=0; /* an output's PCM no longer lies just before tail */
        unsigned last=(s->issued_head+s->issued_count-1)&(s->runs_room-1);
        if(s->issued_count&&s->issued[last].start+s->issued[last].frames==sink_mixed)
            s->issued[last].frames+=(uint32_t)n;
        else{
            /* A sink that stopped reporting (a lost device snapshot, a client that does not
               confirm) must not turn media input into a kernel panic or fictitious consumption:
               the PCM stays in its stream until a report frees a run; other streams still
               progress. */
            if(s->issued_count==s->runs_room)continue;
            unsigned next=(s->issued_head+s->issued_count)&(s->runs_room-1);
            s->issued[next].start=sink_mixed;s->issued[next].frames=(uint32_t)n;s->issued_count++;
        }
        s->device_pending+=(uint32_t)n;
        if(keep)s->retained+=(uint32_t)n;
        if (parts) {
            parts[count] = (struct mix_part){s, s->tail, (uint32_t)n};
            s->held += (uint32_t)n;
        }
        count++;
        s->tail += (uint32_t)n * AUDIO_FRAME;
    }
    if(count)sink_mixed+=(uint32_t)frames;
    if (error || (count && !parts)) streams_changed();
    return count;
}

/* Frames at..at + count of the parts, scaled and summed: no lock, interrupts on. A part's frames stay
   in its ring (held), and a flush that leaves them behind only lets its dropped sound be overwritten. */
static void mix_sum(int32_t *sum, const struct mix_part *parts, unsigned n, uint32_t at, uint32_t count, int32_t scale) {
    memset(sum, 0, count * 2 * sizeof *sum);
    for (unsigned p = 0; p < n; p++) {
        if (at >= parts[p].frames) continue;
        uint32_t c = MIN(count, parts[p].frames - at);
        const uint8_t *ring = parts[p].s->ring;
        uint32_t from = parts[p].tail + at * AUDIO_FRAME;
        for (uint32_t k = 0; k < c; k++) {
            const int16_t *in = (const int16_t *)(ring + (from + k * AUDIO_FRAME) % RING_BYTES);
            sum[k * 2] += (int32_t)(((int64_t)in[0] * scale) >> 16);
            sum[k * 2 + 1] += (int32_t)(((int64_t)in[1] * scale) >> 16);
        }
    }
}

/* The sum in the device's format: stereo or both channels as one, in a container of two bytes or a
   left-justified one of four (High Definition Audio 4.5.1). */
static void mix_pack(const struct audio_format *format, uint8_t *to, const int32_t *sum, uint32_t count) {
    bool mono = format->channels == 1, wide = format->container == 4;
    for (uint32_t k = 0; k < count; k++) {
        int32_t left = MAX(-32768, MIN(32767, sum[k * 2])), right = MAX(-32768, MIN(32767, sum[k * 2 + 1]));
        if (mono) left = (left + right) / 2;
        if (wide) {
            ((int32_t *)to)[0] = (int32_t)((uint32_t)left << 16);
            if (!mono) ((int32_t *)to)[1] = (int32_t)((uint32_t)right << 16);
        } else {
            ((int16_t *)to)[0] = (int16_t)left;
            if (!mono) ((int16_t *)to)[1] = (int16_t)right;
        }
        to += (mono ? 1u : 2u) * format->container;
    }
}

/* ---- the pace ----
   The streams' timeline runs on the monotonic clock whatever crystal the device that plays it has:
   every stream, whoever writes it, is played at AUDIO_RATE frames per second of clock_now_ns, so no
   stream can part from the clock or from another. A device counts its own frames (the ordinals a sink
   gets and reports). Each report, a pair of device position and clock time, says how far the timeline
   is from the clock; a slow closed loop sets how many of the timeline's frames one device frame takes,
   and the block is interpolated to that. Nothing is skipped or played twice. Crystals differ by parts
   per million: under any hearing's limit of pitch. A device far off (a virtual one) is measured over a
   growing window and followed directly. Where a device has lost time for good, or played all it was
   given, the loop begins again from there: the streams' clocks wait, and their underruns count it.
   The interpolation holds back PACE_DELAY frames: the last ones of a sound come out with the block after
   it, and sink_mixed runs on through that block. */
#define PACE_ONE (1ULL << 32)
#define PACE_DELAY (PACE_TAPS - 3)                  /* a block's first frame is centred on its window's fourth */
#define PACE_MOST ((int64_t)(PACE_ONE / 8))         /* of the ratio: a device further off stays off */
#define PACE_FAR ((int64_t)(PACE_ONE / 500))        /* no crystal is this far off: 2000 ppm */
#define PACE_FILTER_MS 2048                         /* over a device's reports, which step with its transfers */
#define PACE_LOST_Q8 ((int64_t)(AUDIO_RATE / 20) << 8) /* 50 ms: not for the loop to catch up */
#define PACE_SETTLE_NS NS_PER_S                     /* a device's first burst and its transfers' steps average out */
#define PACE_WINDOW_NS (2 * NS_PER_S)
#define PACE_WINDOW_MOST_NS (64 * NS_PER_S)
/* The loop's gains, for an error in 1/256 frames and a ratio in 1/2^32: Kp 0.1 per second, and
   Ki 0.0025 per second squared (Kp squared over 4, no overshoot). */
#define PACE_KP 35
#define PACE_KI_NUM 7
#define PACE_KI_DEN 8000
static struct {
    bool running;
    uint64_t begin_at, begin_stream, last_at;
    int64_t settle_sum_q8, settle_reports; /* over the first second: where its line lies */
    bool settled;
    int64_t error_q8, integral, far;
    uint64_t window_at, window_device, window_ns;
    int64_t ratio;         /* timeline frames per device frame, less 1, in 1/2^32 */
    uint32_t fraction;     /* of the next timeline frame */
    uint64_t given;        /* the next device ordinal */
    uint64_t mark_given, mark_mixed; /* after the last block: device ordinal, and sink_mixed */
    int64_t mark_ratio;
    bool ringing;          /* the history holds sound not out yet */
    int32_t history[PACE_TAPS * 2]; /* the last frames of the timeline, summed */
} pace;

/* The frames that play in ns at AUDIO_RATE (rounded down). */
static uint64_t frames_in(uint64_t ns) { return ns / NS_PER_S * AUDIO_RATE + ns % NS_PER_S * AUDIO_RATE / NS_PER_S; }

/* audio_lock held: another device, or none: nothing known about its crystal. */
static void pace_reset(void) {
    pace.running = false;
    pace.integral = pace.far = pace.ratio = 0;
    pace.window_ns = PACE_WINDOW_NS;
    pace.fraction = 0;
    pace.ringing = false;
    memset(pace.history, 0, sizeof pace.history);
}
/* The timeline frames of `device` frames back from the last block's end. */
static uint64_t pace_back(uint64_t device) {
    return device + (uint64_t)(((__int128)device * pace.mark_ratio) >> 32);
}
/* audio_lock held: where the frames from device ordinal `given` on start, on the timeline. */
static uint64_t pace_unissued(uint64_t given) {
    if (given >= pace.mark_given) return sink_mixed;
    uint64_t back = pace_back(pace.mark_given - given);
    return pace.mark_mixed > back ? pace.mark_mixed - back : 0;
}
/* audio_lock held: what has been heard when a device reached ordinal `given`. When all a device was
   given is heard, all of it is. */
static uint64_t pace_heard(uint64_t given) {
    if (given >= pace.mark_given) return sink_mixed;
    uint64_t at = pace_unissued(given);
    return at > PACE_DELAY ? at - PACE_DELAY : 0;
}
/* audio_lock held: a sink's report, a pair. */
static void pace_report(uint64_t given, uint64_t heard, uint64_t at) {
    if (heard >= sink_mixed) { pace.running = false; return; } /* it stops, or stands dry */
    if (!pace.running || at <= pace.last_at) {
        if (pace.running) return;
        pace.running = true;
        pace.begin_at = pace.last_at = pace.window_at = at;
        pace.begin_stream = heard;
        pace.window_device = given;
        pace.error_q8 = pace.settle_sum_q8 = pace.settle_reports = 0;
        pace.settled = false;
        return;
    }
    int64_t error = ((int64_t)(heard - pace.begin_stream) - (int64_t)frames_in(at - pace.begin_at)) * 256;
    if (error > PACE_LOST_Q8 || error < -PACE_LOST_Q8) {
        /* A pair is one instant: a device this far off has lost that time for good. It goes on from
           here, and the window that measures its crystal goes on. */
        pace.begin_at = pace.last_at = at;
        pace.begin_stream = heard;
        pace.error_q8 = pace.settle_sum_q8 = pace.settle_reports = 0;
        pace.settled = false;
        return;
    }
    if (!pace.settled) {
        pace.settle_sum_q8 += error;
        pace.settle_reports++;
        if (at - pace.begin_at < PACE_SETTLE_NS) { pace.last_at = at; return; }
        pace.begin_stream += (uint64_t)(pace.settle_sum_q8 / pace.settle_reports / 256);
        pace.settled = true;
        pace.last_at = at;
        return;
    }
    uint64_t ms = MIN((at - pace.last_at) / NS_PER_MS, PACE_FILTER_MS);
    pace.last_at = at;
    pace.error_q8 += (error - pace.error_q8) * (int64_t)ms / PACE_FILTER_MS;
    pace.integral -= pace.error_q8 * (int64_t)ms * PACE_KI_NUM / PACE_KI_DEN;
    pace.integral = MAX(-PACE_MOST, MIN(PACE_MOST, pace.integral));
    if (at - pace.window_at >= pace.window_ns) {
        int64_t nominal = (int64_t)frames_in(at - pace.window_at), moved = (int64_t)(given - pace.window_device);
        int64_t off = moved >= 0 && moved < 2 * nominal ? (nominal - moved) * (int64_t)PACE_ONE / nominal : 0;
        off = off > PACE_FAR || off < -PACE_FAR ? off : 0;
        pace.integral -= off - pace.far; /* what the loop has found of it so far is in the measure now */
        pace.far = off;
        pace.window_at = at;
        pace.window_device = given;
        pace.window_ns = MIN(pace.window_ns * 2, PACE_WINDOW_MOST_NS);
    }
    pace.ratio = MAX(-PACE_MOST, MIN(PACE_MOST, pace.far + pace.integral - pace.error_q8 * PACE_KP));
}

/* One device frame from the window of PACE_TAPS timeline frames at x, `position` of the way between
   its two middle ones. */
static void pace_frame(int32_t *to, const int32_t *x, uint32_t position) {
    uint32_t phase = position >> (32 - PACE_PHASE_BITS), part = (position >> (32 - PACE_PHASE_BITS - 15)) & 0x7fff;
    const int32_t *a = pace_filter[phase], *b = pace_filter[phase + 1];
    int64_t left = 0, right = 0;
    for (int m = 0; m < PACE_TAPS; m++) {
        int64_t c = a[m] + (((int64_t)(b[m] - a[m]) * part) >> 15);
        left += c * x[m * 2];
        right += c * x[m * 2 + 1];
    }
    to[0] = (int32_t)(left >> 15);
    to[1] = (int32_t)(right >> 15);
}

/* audio_lock held, flags f, given back here: a block for a sink, the lock held only for the positions.
   *end, written under it: the device ordinal after the block. Silence where a stream had too little.
   False, and out untouched, when no stream had anything. */
static bool mix_unlock(uint64_t f, void *out, const struct audio_format *format, int frames, bool keep, uint64_t *end) {
    struct mix_part parts[MAX_STREAMS + 1];
    uint64_t step = (uint64_t)((int64_t)PACE_ONE + pace.ratio), position = pace.fraction;
    uint64_t after = position + (uint64_t)frames * step;
    uint32_t taken = (uint32_t)(after >> 32);
    unsigned n = mix_claim(parts, (int)taken, keep);
    if (!n && !pace.ringing) {
        pace.fraction = 0; /* what comes next has nothing to do with it */
        spin_unlock(&audio_lock, f);
        return false;
    }
    if (!n) sink_mixed += taken;
    pace.ringing = n != 0;
    pace.fraction = (uint32_t)after;
    pace.given += (uint64_t)frames;
    pace.mark_given = pace.given;
    pace.mark_mixed = sink_mixed;
    pace.mark_ratio = pace.ratio;
    *end = pace.given;
    int32_t scale = gain;
    int32_t window[(PACE_TAPS + MIX_CHUNK) * 2];
    memcpy(window, pace.history, sizeof pace.history);
    spin_unlock(&audio_lock, f);
    uint32_t filled = PACE_TAPS, read = 0;
    int32_t sum[MIX_CHUNK * 2];
    uint8_t *to = out;
    size_t frame = (size_t)format->channels * format->container;
    for (int done = 0; done < frames;) {
        uint32_t count = 0;
        while (done + (int)count < frames && count < MIX_CHUNK) {
            uint32_t at = (uint32_t)(position >> 32);
            if (at + PACE_TAPS > filled) break;
            pace_frame(sum + count * 2, window + at * 2, (uint32_t)position);
            position += step;
            count++;
        }
        if (count) {
            mix_pack(format, to + (size_t)done * frame, sum, count);
            done += (int)count;
            continue;
        }
        uint32_t at = (uint32_t)(position >> 32), left = filled - at, more = MIN(taken - read, MIX_CHUNK);
        memmove(window, window + at * 2, left * 2 * sizeof *window);
        position -= (uint64_t)at << 32;
        mix_sum(window + left * 2, parts, n, read, more, scale);
        filled = left + more;
        read += more;
    }
    /* The next block's window starts at position: what is left here, then what of the block was not
       read yet, PACE_TAPS frames in all. */
    uint32_t at = (uint32_t)(position >> 32), left = filled - at;
    memmove(window, window + at * 2, left * 2 * sizeof *window);
    if (read < taken) mix_sum(window + left * 2, parts, n, read, taken - read, scale);
    f = spin_lock(&audio_lock);
    memcpy(pace.history, window, sizeof pace.history);
    for (unsigned p = 0; p < n; p++) parts[p].s->held -= parts[p].frames;
    streams_changed();
    spin_unlock(&audio_lock, f);
    return true;
}

bool audio_sink_mix(struct audio_sink *sink, void *out, int frames, uint64_t *end) {
    uint64_t f = spin_lock(&audio_lock);
    if (active != sink || remotes) {
        spin_unlock(&audio_lock, f);
        return false;
    }
    return mix_unlock(f, out, &sink->format, frames, true, end);
}

/* audio_lock held. */
static void played_locked(uint64_t ordinal,uint64_t at){
    ordinal=MIN(ordinal,sink_mixed);
    if(ordinal<=sink_played)return;
    sink_played=ordinal;sink_played_at=at;
    for(unsigned i=0;i<=MAX_STREAMS;i++){
        struct stream *s=streams[i];if(!s)continue;
        if(s->issued_head>=s->runs_room||s->issued_count>s->runs_room){s->clock_error=true;continue;}
        while(s->issued_count){
            unsigned at_run=s->issued_head;uint64_t start=s->issued[at_run].start;
            if(start>=ordinal)break;
            uint32_t consumed=(uint32_t)MIN(ordinal-start,(uint64_t)s->issued[at_run].frames);
            if(consumed>s->device_pending){s->clock_error=true;break;}
            s->device_pending-=consumed;s->issued[at_run].start+=consumed;s->issued[at_run].frames-=consumed;
            if(s->issued[at_run].frames)break;
            s->issued_head=(at_run+1)&(s->runs_room-1);s->issued_count--;
        }
        if(s->retained>s->device_pending)s->retained=s->device_pending; /* played: the writer may reuse it */
        if(!s->dry_since&&s->committed_bytes&&s->head==s->tail&&!s->device_pending)s->dry_since=at;
    }
    streams_changed();
}
void audio_played(uint64_t ordinal,uint64_t at){
    uint64_t f=spin_lock(&audio_lock);
    uint64_t heard=pace_heard(ordinal);
    pace_report(ordinal,heard,at);
    played_locked(heard,at);
    spin_unlock(&audio_lock,f);
}

/* audio_lock held. */
static bool pending_locked(void) {
    if (pace.ringing) return true;
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
    uint32_t takes = (uint32_t)((pace.fraction + (uint64_t)frames * (uint64_t)((int64_t)PACE_ONE + pace.ratio)) >> 32);
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *s = streams[i];
        if (!s || s->clock_error || s->head - s->tail > RING_BYTES) continue;
        uint32_t have = (s->head - s->tail) / AUDIO_FRAME;
        if (!have) continue;
        any = true;
        if (have < takes) whole = false;
    }
    return any ? whole || low : low && pace.ringing;
}

bool audio_ready(int frames, bool low) {
    uint64_t f = spin_lock(&audio_lock);
    bool ready = ready_locked(frames, low);
    spin_unlock(&audio_lock, f);
    return ready;
}

/* ---- the sink of nothing ----
   While there is no output and no remote client, an ideal device plays the sound: NULL_BLOCK blocks
   back to back from null_start, the first mixed at once and each next one as the one before ends
   (null_alarm). The position heard is the time since null_start in frames, not a sum of blocks, so
   a stream is consumed exactly as fast as it would be played however late an alarm comes, and a
   program takes as long as its sound does. It stops when a block ends with nothing to mix after
   it; a stream that gets PCM starts it again. audio_lock guards it. */
static struct alarm null_alarm;
static bool null_running;
static uint64_t null_start, null_base; /* when it began to play, and sink_mixed then */

/* The ns that frames take (rounded up). */
static uint64_t ns_for(uint64_t frames) {
    return frames / AUDIO_RATE * NS_PER_S + (frames % AUDIO_RATE * NS_PER_S + AUDIO_RATE - 1) / AUDIO_RATE;
}

/* audio_lock held, now read under it: the blocks due by now are mixed, with the one that plays
   now; what has been heard is reported, and the alarm set for the end of the block that plays. */
static void null_play(uint64_t now) {
    uint64_t heard = frames_in(now - null_start), mixed = sink_mixed - null_base;
    played_locked(null_base + MIN(heard, mixed), now);
    while (mixed <= heard && mix_claim(NULL, NULL_BLOCK, false)) {
        mixed += NULL_BLOCK;
        played_locked(null_base + MIN(heard, mixed), now);
    }
    if (mixed > heard) alarm_set(&null_alarm, null_start + ns_for(mixed));
    else null_running = false; /* played out, and nothing waits */
}
static void null_begin(uint64_t now) {
    null_running = true;
    null_start = now;
    null_base = sink_mixed;
    null_play(now);
}
/* An output or a remote client plays the sound from now: the block that plays is over. An alarm still
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
   client's sound thread when a block it wants is there, the output when it has played all it was
   given (it is idle, or is about to stop), the sink of nothing when it is stopped. An output that has
   not played everything will be filled again as its device moves on, and sees this PCM then. */
static void sink_fed_locked(void) {
    struct audio_remote *r = remotes;
    if (r) {
        if (r->wants && ready_locked(r->wants, r->low)) {
            r->wants = 0;
            remote_wake(r);
        }
        return;
    }
    if (active) {
        if (sink_played == sink_mixed) audio_sink_wake();
        return;
    }
    if (!null_running) null_begin(clock_now_ns());
}

/* ---- the outputs ---- */

/* A sink that may hold this many blocks is there: each stream gets room for a run of each, the array
   allocated with no lock held and put in under it, with the stream's runs in their order. In a thread
   that may sleep. */
static void runs_grow(unsigned blocks) {
    unsigned room = 1, size = 0;
    while (room < blocks) room *= 2;
    struct run *spare = NULL;
    uint64_t f = spin_lock(&audio_lock);
    runs_most = MAX(runs_most, room);
    for (;;) {
        unsigned want = runs_most;
        struct stream *s = NULL;
        for (int i = 0; i <= MAX_STREAMS && !s; i++)
            if (streams[i] && streams[i]->runs_room < want) s = streams[i];
        if (!s) break;
        if (size == want) {
            for (unsigned k = 0; k < s->issued_count; k++) spare[k] = s->issued[(s->issued_head + k) & (s->runs_room - 1)];
            struct run *old = s->issued;
            size = s->runs_room;
            s->issued = spare;
            s->runs_room = want;
            s->issued_head = 0;
            spare = old;
            streams_changed(); /* a writer waiting for a run has room */
            continue;
        }
        spin_unlock(&audio_lock, f);
        kfree(spare);
        spare = kmalloc(want * sizeof *spare);
        size = spare ? want : 0;
        if (!spare) return;
        f = spin_lock(&audio_lock);
    }
    spin_unlock(&audio_lock, f);
    kfree(spare);
}

/* The better output: something plugged in, then one that needs nothing plugged in or cannot tell, then
   a jack with nothing in it; among equals what is meant for one person's ears, then the machine's own
   sound, then the others. */
static int sink_rank(const struct audio_sink *s) {
    static const uint8_t by_kind[] = {[AUDIO_KIND_OTHER] = 0, [AUDIO_KIND_DIGITAL] = 1, [AUDIO_KIND_DISPLAY] = 2,
                                      [AUDIO_KIND_LINE] = 3, [AUDIO_KIND_SPEAKER] = 4, [AUDIO_KIND_HEADPHONE] = 5};
    int plugged = s->jack == AUDIO_JACK_PLUGGED ? 2 : s->jack == AUDIO_JACK_NONE ? 1 : 0;
    return plugged * 16 + by_kind[s->kind];
}

/* sink_mutex held. The one the user chose while it is there; else the best, and what plays goes on
   playing against an equal. */
static struct audio_sink *sink_best(void) {
    if (chosen && chosen->available) return chosen;
    struct audio_sink *best = active && active->available ? active : NULL;
    for (struct audio_sink *s = sinks; s; s = s->next)
        if (s->available && (!best || sink_rank(s) > sink_rank(best))) best = s;
    return best;
}

/* sink_mutex held: the best output plays. What the one before held counts as played (its close), and
   the streams go on from where they are; with no output left the sink of nothing takes them. */
static void outputs_update(void) {
    bool changed = false;
    for (;;) {
        struct audio_sink *to = sink_best();
        if (to == active) break;
        changed = true;
        if (active) active->ops->close(active);
        uint64_t now = clock_now_ns(), f = spin_lock(&audio_lock);
        __atomic_store_n(&active, to, __ATOMIC_RELEASE);
        pace_reset();
        if (to) null_stop(now);
        else if (!remotes && !null_running && pending_locked()) null_begin(now);
        spin_unlock(&audio_lock, f);
        if (!to) {
            kprintf("audio: no output\n");
            break;
        }
        if (to->ops->open(to)) {
            kprintf("audio: output \"%s\", %u Hz, %u bits, %u channels\n", to->name, to->format.rate, to->format.bits,
                    to->format.channels);
            if (filler) sched_set_policy(filler, sched_deadline(to->period_ns));
            break;
        }
        kprintf("audio: output \"%s\" does not open\n", to->name);
        to->available = false; /* its driver says when it is back; it is closed by the next round */
    }
    if (changed) {
        audio_sink_wake();
        watched_changed(&output_watched);
    }
}

void audio_sink_add(struct audio_sink *s) {
    if (s->format.rate != AUDIO_RATE) { /* no output of ours is without it: High Definition Audio 7.3.4.7 */
        kprintf("audio: output \"%s\" does not take %u Hz\n", s->name, AUDIO_RATE);
        return;
    }
    mutex_lock(&sink_mutex);
    struct audio_sink **tail = &sinks;
    while (*tail) tail = &(*tail)->next;
    s->next = NULL;
    s->available = true;
    *tail = s;
    runs_grow(s->blocks);
    outputs_update();
    mutex_unlock(&sink_mutex);
}

void audio_sink_state(struct audio_sink *s, enum audio_jack jack, const char *name) {
    mutex_lock(&sink_mutex);
    bool named = name && strcmp(name, s->name);
    if (named) strlcpy(s->name, name, sizeof s->name);
    if (jack != s->jack) kprintf("audio: output \"%s\": %s\n", s->name, jack == AUDIO_JACK_PLUGGED ? "plugged in" : "unplugged");
    s->jack = jack;
    outputs_update();
    bool plays = active == s;
    mutex_unlock(&sink_mutex);
    if (named && plays) watched_changed(&output_watched);
}

void audio_sink_available(struct audio_sink *s, bool available) {
    mutex_lock(&sink_mutex);
    s->available = available;
    outputs_update();
    mutex_unlock(&sink_mutex);
}

unsigned audio_sinks(struct audio_sink_info *out, unsigned room) {
    unsigned n = 0;
    mutex_lock(&sink_mutex);
    for (struct audio_sink *s = sinks; s; s = s->next, n++) {
        if (n >= room) continue;
        strlcpy(out[n].name, s->name, sizeof out[n].name);
        out[n].kind = s->kind;
        out[n].jack = s->jack;
        out[n].format = s->format;
        out[n].available = s->available;
        out[n].playing = s == active;
    }
    mutex_unlock(&sink_mutex);
    return n;
}

bool audio_sink_choose(unsigned index) {
    bool found = index == AUDIO_SINK_BEST;
    mutex_lock(&sink_mutex);
    struct audio_sink *s = sinks;
    for (unsigned n = 0; s && n < index; n++) s = s->next;
    if (s || found) {
        chosen = found ? NULL : s;
        found = true;
        outputs_update();
    }
    mutex_unlock(&sink_mutex);
    return found;
}

/* Fills the output that plays each time something woke it: its device moved on, a stream got PCM
   while it had played everything, a flush took PCM back. The mixing happens here and not in the
   interrupt, with the CPU's interrupts on and audio_lock held only for the positions; the deadline
   class, with the period of the output's blocks, keeps it to its device's pace. */
static void audio_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t seen = __atomic_load_n(&filler_signals, __ATOMIC_SEQ_CST);
        mutex_lock(&sink_mutex);
        if (active) active->ops->fill(active);
        mutex_unlock(&sink_mutex);
        wq_wait_unless(&filler_wq, &filler_signals, seen, CLOCK_NEVER);
    }
}

/* ---- remote clients (vm/rdp.c), from their session's threads on any CPU ---- */

void audio_remote_attach(struct audio_remote *r) {
    runs_grow(r->blocks);
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
        pace_reset();
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
    if (playing) {
        played_locked(pace_heard(r->given), clock_now_ns()); /* its client will not say it played them */
        pace_reset();
        if (remotes) remote_wake(remotes);     /* the sound goes to the next one */
        else if (pending_locked()) sink_fed_locked();
    }
    spin_unlock(&audio_lock, f);
    if (playing && !remote_now()) watched_changed(&output_watched);
}
bool audio_remote(void) { return remote_now(); }
const char *audio_output(void) {
    if (remote_now()) return "Remote desktop";
    const struct audio_sink *s = __atomic_load_n(&active, __ATOMIC_ACQUIRE);
    return s ? s->name : NULL;
}

bool audio_remote_ready(struct audio_remote *r, int frames, bool low) {
    uint64_t f = spin_lock(&audio_lock);
    bool ready = remotes == r && ready_locked(frames, low);
    r->wants = ready ? 0 : frames;
    r->low = low;
    spin_unlock(&audio_lock, f);
    return ready;
}
bool audio_remote_mix(struct audio_remote *r, int16_t *out, int frames, uint64_t *end) {
    static const struct audio_format pcm = {.rate = AUDIO_RATE, .channels = 2, .bits = 16, .container = 2};
    uint64_t f = spin_lock(&audio_lock);
    if (remotes != r) {
        spin_unlock(&audio_lock, f);
        return false;
    }
    if (!mix_unlock(f, out, &pcm, frames, false, &r->given)) return false;
    *end = r->given; /* only this thread mixes for r */
    return true;
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
    uint32_t head = s->head, queued = head - s->tail, retained = MAX(s->retained, s->held);
    bool drop = hz <= 0 || queued >= AUDIO_FRAME || tone_writing;
    if (!drop) tone_writing = true;
    spin_unlock(&audio_lock, f);
    if (drop) return;
    /* the queue's worth at most, and never over what an output may still give back */
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
    if (frames > 0) sink_fed_locked();
    spin_unlock(&audio_lock, f);
}

/* ---- the apps' streams (docs/nocturne-native-abi.md §9) ---- */

static struct stream *stream_of(struct handle *h) { return h->object->priv; }

static bool stream_broken(const struct stream *s) {
    return s->clock_error||s->head-s->tail>RING_BYTES||s->issued_head>=s->runs_room||s->issued_count>s->runs_room;
}

/* What the writer leaves alone just before tail: PCM an output may give back, or being summed. */
static uint32_t stream_kept(const struct stream *s) { return MAX(s->retained, s->held) * AUDIO_FRAME; }

/* No room: the queue is full, the ring holds no more beside what is kept, or all of the stream's runs
   are in a sink's hands (the sink skips it until one is played). */
static bool stream_full(const struct stream *s) {
    uint32_t queued = s->head - s->tail;
    return queued >= QUEUE_BYTES || queued + stream_kept(s) >= RING_BYTES || s->issued_count == s->runs_room;
}
/* Bytes a writer may add past head now. */
static uint32_t stream_room(const struct stream *s) {
    if (stream_full(s)) return 0;
    uint32_t queued = s->head - s->tail;
    return MIN(QUEUE_BYTES - queued, RING_BYTES - queued - stream_kept(s));
}

/* The frames of s a sink holds from ordinal `at` on. */
static uint32_t frames_from(const struct stream *s, uint64_t at) {
    uint32_t back = 0;
    for (unsigned k = 0; k < s->issued_count && k < s->runs_room; k++) {
        unsigned i = (s->issued_head + k) & (s->runs_room - 1);
        uint64_t end = s->issued[i].start + s->issued[i].frames;
        if (end > at) back += (uint32_t)(end - MAX(s->issued[i].start, at));
    }
    return back;
}

/* What the output's device has not begun to fetch goes back into the streams' queues, to be mixed
   again by the output's fill() (audio.h, audio_sink_ops). Only when s has PCM there, and when every
   stream still keeps its PCM from there in its ring (retained: not after a remote client mixed
   from it). The PCM keeps its order: each stream's frames from `at` on are the last it gave the
   output, just before its tail. sink_mutex (no fill() is under way) and audio_lock held. */
static bool take_back(struct stream *s) {
    if (!active || remotes) return false;
    uint64_t at = pace_unissued(active->ops->unissue(active, false));
    if (at >= sink_mixed || !frames_from(s, at)) return false;
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *o = streams[i];
        if (o && !stream_broken(o) && frames_from(o, at) > o->retained) return false;
    }
    uint64_t given = active->ops->unissue(active, true); /* the engine only moved on since: never an earlier ordinal */
    at = pace_unissued(given);
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *o = streams[i];
        if (!o || stream_broken(o)) continue;
        uint32_t back = frames_from(o, at);
        while (o->issued_count) {
            unsigned last = (o->issued_head + o->issued_count - 1) & (o->runs_room - 1);
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
    pace.given = pace.mark_given = given;
    pace.mark_mixed = at;
    pace.fraction = 0;
    /* The frames before `at` of the streams that go on, for the window of what is mixed next. */
    memset(pace.history, 0, sizeof pace.history);
    pace.ringing = false;
    uint64_t from = at > PACE_TAPS ? at - PACE_TAPS : 0;
    for (int i = 0; i <= MAX_STREAMS; i++) {
        struct stream *o = streams[i];
        if (!o || o == s || stream_broken(o)) continue;
        uint32_t count = MIN(frames_from(o, from), o->retained);
        for (uint32_t k = 0; k < count; k++) {
            const int16_t *in = (const int16_t *)(o->ring + (o->tail - (count - k) * AUDIO_FRAME) % RING_BYTES);
            int32_t *to = pace.history + (PACE_TAPS - count + k) * 2;
            to[0] += (int32_t)(((int64_t)in[0] * gain) >> 16);
            to[1] += (int32_t)(((int64_t)in[1] * gain) >> 16);
            pace.ringing = true;
        }
    }
    return true;
}

/* s's PCM goes at once, with what of it the output can still give back; the output mixes the other
   streams' again. Nothing mixes s's old PCM after this: its queue is empty, and no sink keeps a
   block of it unmixed. The clock starts a new generation. Any CPU, in a thread that may sleep. */
static void stream_drop(struct stream *s) {
    mutex_lock(&sink_mutex);
    uint64_t f = spin_lock(&audio_lock);
    bool taken = s->device_pending && take_back(s);
    s->tail = s->head;
    s->committed_bytes = 0; s->issued_head = s->issued_count = 0; s->device_pending = 0; s->retained = 0;
    s->generation++; s->clock_error = false;
    s->dry_since = s->underruns = s->underrun_ns = 0;
    streams_changed();
    spin_unlock(&audio_lock, f);
    mutex_unlock(&sink_mutex);
    if (taken) audio_sink_wake();
}

/* Without audio_lock (an events wait asks under its queue's lock, which audio_lock comes before):
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
    while (!thread_current()->killed && !s->clock_error && (s->head - s->tail >= AUDIO_FRAME||s->device_pending)) {
        bool woken = wq_wait_locked_until(&audio_wq, &audio_lock, give_up);
        spin_take(&audio_lock);
        if (!woken) break;
    }
    spin_unlock(&audio_lock, fl);
    if (thread_current()->killed) stream_drop(s);
    fl = spin_lock(&audio_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i] == s) streams[i] = NULL;
    /* a sink summing it finishes before its ring goes (stream_release) */
    while (s->held) {
        wq_wait_locked(&audio_wq, &audio_lock);
        spin_take(&audio_lock);
    }
    spin_unlock(&audio_lock, fl);
}

static void stream_release(struct object *o) {
    struct stream *s = o->priv;
    kfree(s->issued);
    kfree(s->ring);
    kfree(s);
}

static const struct object_ops stream_ops = {
    .kind = N_KIND_DEVICE, .ready = stream_ready, .close = stream_close, .release = stream_release,
};

bool audio_stream_is(struct handle *h) { return handle_is(h, &stream_ops); }

int audio_stream_open(struct handle **out) {
    struct stream *s = kzalloc(sizeof *s);
    uint64_t fl = spin_lock(&audio_lock);
    unsigned room = runs_most;
    spin_unlock(&audio_lock, fl);
    if (s) {
        s->ring = kmalloc(RING_BYTES);
        s->issued = kmalloc(room * sizeof *s->issued);
        s->runs_room = room;
    }
    if (!s || !s->ring || !s->issued) {
        if (s) {
            kfree(s->issued);
            kfree(s->ring);
            kfree(s);
        }
        return -N_ERR_NO_MEMORY;
    }
    object_init(&s->object, &stream_ops, s);
    struct handle *h = handle_open(&s->object, HANDLE_WRITE);
    if (!h) { stream_release(&s->object); return -N_ERR_NO_MEMORY; }
    fl = spin_lock(&audio_lock);
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS && slot < 0; i++) if (!streams[i]) slot = i;
    if (slot >= 0) streams[slot] = s;
    spin_unlock(&audio_lock, fl);
    if (slot < 0) { handle_close(h); return -N_ERR_BUSY; } /* nothing queued: no wait */
    runs_grow(room); /* a sink that came since runs_most was read */
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
        if (!s->writer) { s->writer = thread_current(); break; }
        if (!wait) { spin_unlock(&audio_lock, fl); return -N_ERR_NOT_READY; }
        if (thread_current()->killed) { spin_unlock(&audio_lock, fl); return -N_ERR_STOPPED; }
        wq_wait_locked(&audio_wq, &audio_lock);
        spin_take(&audio_lock);
    }
    while (done < n) {
        if (stream_broken(s)) { error = -N_ERR_IO; break; }
        uint32_t room = stream_room(s);
        if (!room) {
            if (done) break; /* report what went in; the caller sends the rest */
            if (!wait) { error = -N_ERR_NOT_READY; break; }
            if (thread_current()->killed) { error = -N_ERR_STOPPED; break; }
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
        if (s->dry_since) {
            uint64_t now = clock_now_ns();
            s->underruns++;
            s->underrun_ns += now > s->dry_since ? now - s->dry_since : 0;
            s->dry_since = 0;
        }
        s->head += c;
        s->committed_bytes += c;
        done += c;
    }
    s->writer = NULL;
    wq_wake_all(&audio_wq); /* a writer waiting for this one */
    if (done) sink_fed_locked();
    spin_unlock(&audio_lock, fl);
    return done ? (int64_t)(done / AUDIO_FRAME) : error;
}

/* How far ahead of the sink the stream is, and how much of it was played: its frames before the
   last ordinal its sink reported, heard at sink_played_at. */
static struct n_audio_status status_of(const struct stream *s) {
    uint32_t software=(s->head-s->tail)/AUDIO_FRAME;
    uint64_t committed=s->committed_bytes/AUDIO_FRAME;
    uint64_t remaining=(uint64_t)software+s->device_pending;
    return (struct n_audio_status){.queued_frames=software,.device_pending_frames=s->device_pending,
        .played_frames=committed>=remaining?committed-remaining:0,.committed_frames=committed,
        .generation=s->generation,.flags=remotes?N_AUDIO_CLOCK_REMOTE:active?N_AUDIO_CLOCK_DEVICE:N_AUDIO_CLOCK_ESTIMATED,
        .underruns=s->underruns,.underrun_ns=s->underrun_ns,.played_at_ns=sink_played_at,
        .pace_ppb=pace.ratio*1000000000/(int64_t)PACE_ONE};
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
    stream_drop(stream_of(h));
    return 0;
}

void audio_init(void) {
    watched_init(&output_watched, NULL); /* before an output is added */
    keep_register(&sound_keep);          /* read back by keep_start, before the first program */
    alarm_init(&null_alarm, null_fire);
    system_stream.issued = kmalloc(NULL_RUNS * sizeof *system_stream.issued);
    if (!system_stream.issued) panic("audio: no memory for its system stream");
    system_stream.runs_room = NULL_RUNS;
    streams[MAX_STREAMS] = &system_stream;
    filler = kthread_create("audio", audio_thread, NULL);
    if (!filler) panic("audio: no memory for its thread");
    hda_init();
    vm_audio_init();
}
