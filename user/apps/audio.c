/* サービス audio：音の方針の持ち主。出口の選択、出口の形式、流れの変換と混合、音量と消音。カーネルは出口の一覧、
   形式の申告、DMA の輪、位置だけを持つ（kernel/src/dev/audio.h）。口は docs/nocturne-native-abi.md §17。

   スレッドは一つで、締切の級で走る。全部の事象を一つの列で受け、受けたものを最後まで処理する：錠は無い。

   流れごとの接続だけが輪を持つ。輪は触れた頁だけが実の記憶になる。混ぜるときに見るのは活動中の流れの列だけで、
   流れの数に上限は無い。流れが数百あっても、音の無い流れは列に居らず、拍ごとの費用は鳴っている流れの数で決まる。
   音の無い流れは asleep を立てて列を出る。アプリは head を書いた後で asleep を取り、立っていれば KICK を
   一通送る。続けて鳴る流れは輪だけで運ぶので、バッファごとの往復は無い。

   全部の流れが静かになって IDLE_NS たつと装置を止める。止まると拍も止まり、受ける事象が来るまで何も回らない。
   まだ一度も鳴っていなければ出口を開かないので、輪の記憶も持たない。

   時間の並び：装置は開いた時から frame を数える（position）。written はそこから先へ書き終えた位置で、塊の倍数。
   塊ごとに、各流れがその塊の始めにどこを読んでいたかを marks に残す。位置と marks から聞こえた所が分かる。
   取り戻し（flush）では、装置がまだ読まない最初の塊まで全部の流れを戻し、混ぜ直す。

   流れの時間は monotonic の時計で進む。装置の水晶はそれぞれの速さなので、位置と時計の差を遅い閉ループで追い、
   ratio の分だけ流れを読む速さを変える。 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"

/* ---- 決めの値。どれも音の遅れと CPU の釣り合いで、機械の資源の上限ではない。 ---- */
#define BLOCK_NS 5000000ull        /* 望む塊の時間。拍はこの間隔で走る。 */
#define BLOCKS 32u                 /* 望む塊の数。AHEAD_HIGH より多ければよい。 */
#define AHEAD_LOW 4u               /* 装置の読む所から書いておく塊の数。遅れると増やす。 */
#define AHEAD_HIGH 16u
#define URGENT_BLOCKS 2u           /* 残りがこれより少なければ、窓の足りない流れも待たずに混ぜる */
#define IDLE_NS 1000000000ull      /* 静かになってから装置を止めるまで */
/* 流れが混ぜる前に持てる量：48 kHz で 8192 frame。プログラムの SOUND_QUEUE と同じ時間。 */
#define QUEUE_FRAMES(rate) (((uint64_t)(rate) * 8192u + 47999u) / 48000u)
#define LATE_FRAMES(rate) ((uint64_t)(rate) / 4u) /* 混ぜた後で聞こえるまで輪に残す分の余裕 */
#define NULL_RATE 48000u           /* 出口が無いときの数え方。音は作らず、時計で流れを進める。 */
#define NULL_BLOCK_NS 10000000ull
#define NULL_BLOCKS 8u

/* 変換の窓：Kaiser 窓の sinc。半分の長さと β。 */
#define NEAR_LOBES 4               /* 同じ率：歩みの差は ppm で、短い窓で足りる */
#define NEAR_BETA 5.0
#define FAR_LOBES 16               /* 違う率 */
#define FAR_BETA 9.0
#define FAR_CUT 0.97               /* 折り返さないよう遮断をナイキストの手前に置く */
#define LOBE_STEPS 256             /* 窓の表の、山一つあたりの点 */
/* 一 frame に読む入力の上限。これを超える比（出口の 8 倍を超す率）は入力を間引く。率を拒まず、一つの流れが
   拍の時間を全部使うことだけを止める。可聴域の外の折り返しだけが増える。 */
#define TAPS_MOST 256u

/* 閉ループの係数。誤差は秒、ratio は比。Kp は 0.1 / s、Ki は Kp の二乗の 4 分の 1（行き過ぎない）。 */
#define PACE_KP 0.1
#define PACE_KI 0.0025
#define PACE_FILTER_NS 2048000000.0 /* 装置の転送の段差を均す */
#define PACE_SETTLE_NS 1000000000ull /* 最初のかたまりを均す */
#define PACE_LOST_S 0.05            /* これを超す差は取り戻さず、そこから測り直す */
#define PACE_FAR 0.002              /* 水晶はここまで狂わない（2000 ppm）。超えれば仮想の装置で、窓で測って直接追う。 */
#define PACE_MOST 0.125             /* 比の上限：これを超す装置はずれたままにする */
#define PACE_WINDOW_NS 2000000000ull
#define PACE_WINDOW_MOST_NS 64000000000ull

enum { TOKEN_INLET = 1, TOKEN_WATCH, TOKEN_SETTINGS, TOKEN_LINKED, TOKEN_DEVICE, TOKEN_TIMER };

/* 出口の位置（WAVE の順）から左右への重み。ここに無い位置の channel は混ぜない。 */
#define HALF 0.7071f
static const float position_gain[][2] = {
    {1, 0},       {0, 1},       {HALF, HALF}, {0, 0},       {HALF, 0}, {0, HALF}, /* FL FR FC LFE BL BR */
    {1, HALF},    {HALF, 1},    {HALF, HALF}, {HALF, 0},    {0, HALF},            /* FLC FRC BC SL SR */
    {HALF, HALF}, {HALF, 0},    {HALF, HALF}, {0, HALF},    {HALF, 0}, {HALF, HALF}, {0, HALF}, /* TC TFL TFC TFR TBL TBC TBR */
};
#define NAMED_POSITIONS (sizeof position_gain / sizeof *position_gain)

struct mark {
    uint64_t tail;
    uint32_t phase;
};

struct stream {
    struct stream *next;     /* 活動中の列 */
    bool active, closing;
    struct n_audio_format format;
    struct n_audio_ring *ring;
    uint8_t *data;
    size_t bytes;
    uint32_t capacity, queue, frame_bytes;
    uint64_t tail;           /* 読む所。phase は次の frame までの端数（1/2^32）。 */
    uint32_t phase;
    uint64_t heard;
    uint64_t base;
    uint64_t dry_since;      /* 全部を聞かせ終えて、次の音が来ていない */
    uint64_t underruns, underrun_ns;
    uint32_t generation;
    /* 今の出口に合わせて作る。活動中の流れだけが持つ。 */
    struct mark *marks;
    uint64_t joined;         /* この塊から marks が正しい。その前は join の値。 */
    struct mark join;
    float *matrix;           /* [入力 channel × 出力 channel]。NULL は同じ並び。 */
    float *decoded;
    size_t decoded_frames;
    float *frame;
    const float *kernel;
    int lobes;
    double cut;
    int64_t half;
    uint32_t stride;
};

struct client {
    struct client *next;
    n_handle channel;
    struct stream *stream;
    bool watching, dead;
};

/* 利用者が装置ごとに選んだ形式（0 は既定）。起動をまたぐ値は settings が持ち、当て直しで届く。 */
struct choice {
    struct choice *next;
    uint64_t device;
    int32_t rate, bits, channels;
};

static struct {
    uint32_t id;       /* 選んだ出口。0 は無し。 */
    struct n_audio_device_format format;
    uint32_t clock;    /* N_AUDIO_CLOCK_* */
    bool laid_out;     /* 下の形が決まっている */
    n_handle device;   /* 開いた装置。-1 は時計で数える。 */
    uint8_t *ring;
    size_t ring_bytes;
    uint32_t block_frames, blocks, flush_bytes, frame_bytes;
    double *timeline;  /* 塊ごとの、始めの時の流れの時間（出口の frame で数える） */
    float *bus;
    float gain;        /* 今の倍率。塊の中で目標へ動かす。 */
    bool running;
    uint32_t generation;
    uint64_t position, position_at, written, started_at;
    uint32_t guard, ahead;
    uint64_t idle_since;
    bool ticking;      /* 時計で数える拍が立っている */
    uint64_t errors;
} out = {.device = -1};

static struct {
    bool running, settled;
    uint64_t begin_at, last_at, settle_count;
    double begin_time, settle_sum;
    double error, integral, far, ratio;
    uint64_t window_at, window_position, window_ns;
    double time;       /* 次の塊の始めの流れの時間 */
} pace;

static n_handle queue, device_cap = -1, device_watch = -1, settings = -1, linked[2] = {-1, -1};
static struct stream *active;
static struct client *clients, *dead_clients;
static struct choice *choices;
static uint8_t *list;
static size_t list_room, list_bytes;
static int32_t volume = 80;
static bool muted;
static char *chosen_key; /* N_AUDIO_OUTPUT の値。NULL か空は一番良いもの。 */
static float near_kernel[NEAR_LOBES * LOBE_STEPS + 2], far_kernel[FAR_LOBES * LOBE_STEPS + 2];
static bool near_ready, far_ready;

static void output_tick(void);
static void fill(void);

/* ---- 窓の表 ---- */

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; term > sum * 1e-12; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static void kernel_build(float *table, int lobes, double beta) {
    const double pi = 3.14159265358979323846;
    double scale = bessel_i0(beta);
    for (int i = 0; i <= lobes * LOBE_STEPS + 1; i++) {
        double x = (double)i / LOBE_STEPS;
        if (x >= lobes) {
            table[i] = 0;
            continue;
        }
        double r = x / lobes, sinc = x ? sin(pi * x) / (pi * x) : 1;
        table[i] = (float)(sinc * bessel_i0(beta * sqrt(1 - r * r)) / scale);
    }
}

/* ---- 流れの形 ---- */

static uint32_t sample_bytes(uint32_t sample) { return sample == N_AUDIO_S16 ? 2 : 4; }

static void matrix_build(struct stream *s) {
    uint32_t in = s->format.channels, to = out.format.channels;
    free(s->matrix);
    s->matrix = NULL;
    if (in == to) return;
    float *m = calloc((size_t)in * to, sizeof *m);
    if (!m) return; /* 同じ並びとして読めないので、下で無音になる */
    float left = 0, right = 0;
    for (uint32_t c = 0; c < in && c < NAMED_POSITIONS; c++) {
        left += position_gain[c][0];
        right += position_gain[c][1];
    }
    for (uint32_t c = 0; c < in && c < NAMED_POSITIONS; c++) {
        float l = in == 1 ? 1 : left ? position_gain[c][0] / left : 0, r = in == 1 ? 1 : right ? position_gain[c][1] / right : 0;
        if (to == 1) m[c] = (l + r) / 2;
        else {
            m[c * to] = l;
            m[c * to + 1] = r;
        }
    }
    s->matrix = m;
}

/* 活動に入るときと出口が変わるときに、今の出口に合わせて作る。false は記憶が無い。 */
static bool stream_bind(struct stream *s) {
    struct mark *marks = realloc(s->marks, out.blocks * sizeof *marks);
    float *frame = realloc(s->frame, s->format.channels * sizeof *frame);
    if (marks) s->marks = marks;
    if (frame) s->frame = frame;
    if (!marks || !frame) return false;
    matrix_build(s);
    bool same = s->format.rate == out.format.rate;
    if (same && !near_ready) {
        kernel_build(near_kernel, NEAR_LOBES, NEAR_BETA);
        near_ready = true;
    }
    if (!same && !far_ready) {
        kernel_build(far_kernel, FAR_LOBES, FAR_BETA);
        far_ready = true;
    }
    s->kernel = same ? near_kernel : far_kernel;
    s->lobes = same ? NEAR_LOBES : FAR_LOBES;
    double down = (double)s->format.rate / out.format.rate;
    s->cut = same ? 1.0 : FAR_CUT * (down > 1 ? 1 / down : 1);
    s->half = (int64_t)ceil(s->lobes / s->cut);
    s->stride = (uint32_t)(2 * s->half) / TAPS_MOST + 1;
    s->joined = out.written / out.block_frames;
    s->join = (struct mark){s->tail, s->phase};
    return true;
}

static void stream_unbind(struct stream *s) {
    free(s->marks);
    free(s->matrix);
    free(s->decoded);
    free(s->frame);
    s->marks = NULL;
    s->matrix = NULL;
    s->decoded = NULL;
    s->frame = NULL;
    s->decoded_frames = 0;
}

static void stream_free(struct stream *s) {
    stream_unbind(s);
    n_memory_release(s->ring, s->bytes);
    free(s);
}

static uint64_t stream_step(const struct stream *s) {
    double step = (double)s->format.rate / out.format.rate * (1 + pace.ratio);
    return (uint64_t)(step * 4294967296.0);
}

/* 輪に書かれたのは [base, head) で、keep より前は書き潰されていない。外は無音。 */
static void stream_decode(struct stream *s, int64_t first, size_t frames, uint64_t head) {
    uint32_t channels = s->format.channels;
    if (frames > s->decoded_frames) {
        float *grown = realloc(s->decoded, frames * channels * sizeof *grown);
        if (!grown) {
            frames = s->decoded_frames;
        } else {
            s->decoded = grown;
            s->decoded_frames = frames;
        }
    }
    float *to = s->decoded;
    uint32_t bytes = sample_bytes(s->format.sample);
    for (size_t i = 0; i < frames; i++, to += channels) {
        int64_t at = first + (int64_t)i;
        if (at < (int64_t)s->base || at >= (int64_t)head) {
            memset(to, 0, channels * sizeof *to);
            continue;
        }
        const uint8_t *from = s->data + ((uint64_t)at % s->capacity) * s->frame_bytes;
        for (uint32_t c = 0; c < channels; c++, from += bytes) {
            float v;
            if (s->format.sample == N_AUDIO_S16) {
                int16_t x;
                memcpy(&x, from, 2);
                v = (float)x / 32768.0f;
            } else if (s->format.sample == N_AUDIO_S32) {
                int32_t x;
                memcpy(&x, from, 4);
                v = (float)((double)x / 2147483648.0);
            } else {
                memcpy(&v, from, 4);
                if (!(v >= -1.0f)) v = v != v ? 0.0f : -1.0f; /* NaN も無限大も全部の流れへ広げない */
                else if (v > 1.0f) v = 1.0f;
            }
            to[c] = v;
        }
    }
}

/* 今の塊を全部混ぜるのに足りるか。窓の先まで揃っていなければ、まだ書かれるかもしれない。 */
static bool stream_ready(const struct stream *s, uint64_t head) {
    if (head <= s->tail) return true;
    uint64_t taken = (s->phase + out.block_frames * stream_step(s)) >> 32;
    return head - s->tail >= taken + (uint64_t)s->half + 1;
}

/* 一つの塊の分を bus へ足す。urgent でなくても呼ばれる時には足りている。 */
static void stream_mix(struct stream *s, uint64_t head) {
    if (head <= s->tail) return;
    uint64_t step = stream_step(s), available = head - s->tail;
    uint64_t frames = out.block_frames;
    if (!step) return;
    /* 足りない分は塊の後ろの無音になる */
    if ((s->phase + frames * step) >> 32 > available) frames = ((available << 32) - s->phase) / step;
    if (!frames) { /* 残りは出口の一 frame に満たない */
        s->tail = head;
        s->phase = 0;
        return;
    }
    bool null_output = out.device < 0;
    uint64_t end = s->phase + frames * step, taken = end >> 32;
    if (!null_output) {
        uint32_t in = s->format.channels, to = out.format.channels;
        int64_t first = (int64_t)s->tail - s->half;
        size_t need = (size_t)(taken + 2 * (uint64_t)s->half + 2);
        stream_decode(s, first, need, head);
        bool plain = s->format.rate == out.format.rate && step == (1ull << 32) && !s->phase;
        uint64_t position = s->phase;
        float *bus = out.bus;
        const float *kernel = s->kernel;
        double cut = s->cut;
        int lobes = s->lobes;
        uint32_t stride = s->stride;
        for (uint64_t f = 0; f < frames && need <= s->decoded_frames; f++, position += step, bus += to) {
            float *x = s->frame;
            if (plain) {
                memcpy(x, s->decoded + (size_t)(s->half + (int64_t)f) * in, in * sizeof *x);
            } else {
                double centre = (double)s->half + (double)position / 4294967296.0;
                int64_t low = (int64_t)floor(centre) - s->half + 1, high = (int64_t)floor(centre) + s->half;
                if (low < 0) low = 0;
                if (high >= (int64_t)need) high = (int64_t)need - 1;
                memset(x, 0, in * sizeof *x);
                float sum = 0;
                for (int64_t k = low; k <= high; k += stride) {
                    double place = fabs((double)k - centre) * cut * LOBE_STEPS;
                    if (place >= lobes * LOBE_STEPS) continue;
                    int at = (int)place;
                    float part = (float)(place - at), weight = kernel[at] + (kernel[at + 1] - kernel[at]) * part;
                    const float *d = s->decoded + (size_t)k * in;
                    sum += weight;
                    for (uint32_t c = 0; c < in; c++) x[c] += weight * d[c];
                }
                if (sum > 0.001f)
                    for (uint32_t c = 0; c < in; c++) x[c] /= sum;
            }
            if (!s->matrix) {
                if (in == to)
                    for (uint32_t c = 0; c < to; c++) bus[c] += x[c];
            } else {
                for (uint32_t c = 0; c < in; c++) {
                    const float *row = s->matrix + (size_t)c * to;
                    for (uint32_t t = 0; t < to; t++) bus[t] += row[t] * x[c];
                }
            }
        }
    }
    s->tail += taken;
    s->phase = (uint32_t)end;
    if (s->tail > head) {
        s->tail = head;
        s->phase = 0;
    }
}

/* ---- 塊 ---- */

static void block_out(uint64_t block) {
    uint32_t channels = out.format.channels, frames = out.block_frames;
    float target = muted ? 0.0f : ((float)volume / 100.0f) * ((float)volume / 100.0f);
    float gain = out.gain, slope = (target - gain) / (float)frames;
    uint8_t *to = out.ring + (block % out.blocks) * frames * out.frame_bytes;
    const float *bus = out.bus;
    for (uint32_t f = 0; f < frames; f++, gain += slope) {
        for (uint32_t c = 0; c < channels; c++) {
            float v = *bus++ * gain;
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            if (out.format.container == 2) {
                int16_t x = (int16_t)(v * 32767.0f);
                memcpy(to, &x, 2);
                to += 2;
            } else { /* 全幅で左に寄せる。装置は上の bits を読む。 */
                int32_t x = (int32_t)((double)v * 2147483647.0);
                memcpy(to, &x, 4);
                to += 4;
            }
        }
    }
    out.gain = target;
    if (out.flush_bytes) {
        uint8_t *from = out.ring + (block % out.blocks) * frames * out.frame_bytes;
        uint64_t size = (uint64_t)frames * out.frame_bytes, line = out.flush_bytes;
        for (uint64_t at = (uint64_t)from & ~(line - 1); at < (uint64_t)from + size; at += line) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
        __asm__ volatile("sfence" : : : "memory");
    }
}

static uint64_t head_of(struct stream *s) { return __atomic_load_n(&s->ring->head, __ATOMIC_ACQUIRE); }

/* 一つの塊を混ぜて書く。 */
static void block_mix(void) {
    uint64_t block = out.written / out.block_frames;
    out.timeline[block % out.blocks] = pace.time;
    pace.time += out.block_frames * (1 + pace.ratio);
    if (out.device >= 0) memset(out.bus, 0, (size_t)out.block_frames * out.format.channels * sizeof *out.bus);
    for (struct stream *s = active; s; s = s->next) {
        s->marks[block % out.blocks] = (struct mark){s->tail, s->phase};
        stream_mix(s, head_of(s));
    }
    if (out.device >= 0) block_out(block);
    out.written += out.block_frames;
}

static uint64_t block_after_guard(void) {
    uint64_t at = out.position + out.guard, b = out.block_frames;
    return (at + b - 1) / b * b;
}

/* 流れの印から、位置 p に聞こえている流れの場所を読む。 */
static uint64_t stream_heard_at(const struct stream *s, uint64_t p) {
    uint64_t b = p / out.block_frames, part = p % out.block_frames, written = out.written / out.block_frames;
    if (b >= written) return s->tail;
    if (b < s->joined) return s->join.tail;
    if (written - b > out.blocks) return s->heard; /* 印の無い古さ：次の報告まで動かさない */
    uint64_t from = s->marks[b % out.blocks].tail, to = b + 1 < written ? s->marks[(b + 1) % out.blocks].tail : s->tail;
    return from + (to - from) * part / out.block_frames;
}

/* ---- 流れの今を輪へ ---- */

static void stream_publish(struct stream *s, uint64_t at) {
    struct n_audio_ring *r = s->ring;
    uint32_t sequence = r->sequence;
    __atomic_store_n(&r->sequence, sequence + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    r->base = s->base;
    r->played = s->heard;
    r->played_at_ns = at;
    r->underruns = s->underruns;
    r->underrun_ns = s->underrun_ns;
    r->pace_ppb = (int64_t)(pace.ratio * 1e9);
    r->generation = s->generation;
    r->flags = out.clock;
    __atomic_store_n(&r->sequence, sequence + 2, __ATOMIC_RELEASE);
}

static void stream_ring_room(struct stream *s) {
    struct n_audio_ring *r = s->ring;
    uint64_t span = (uint64_t)s->half * 2 + 2, keep = s->heard > s->base + span ? s->heard - span : s->base;
    uint64_t old_tail = r->tail, old_keep = r->keep;
    /* keep は下がらない：アプリがもう書き潰したかもしれない */
    if (keep < old_keep) keep = old_keep;
    if (old_tail == s->tail && old_keep == keep) {
        if (!__atomic_load_n(&r->wants_room, __ATOMIC_ACQUIRE)) return;
    }
    __atomic_store_n(&r->keep, keep, __ATOMIC_RELEASE);
    __atomic_store_n(&r->tail, s->tail, __ATOMIC_RELEASE);
    __atomic_add_fetch(&r->room_bell, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&r->wants_room, 0, __ATOMIC_ACQ_REL)) wake_address(&r->room_bell, 0x7fffffff);
}

static void stream_leave(struct stream **link, struct stream *s) {
    *link = s->next;
    s->next = NULL;
    s->active = false;
    stream_unbind(s);
    if (s->closing) stream_free(s);
}

/* 聞こえた所を進め、聞かせ終えた流れは列を出る。 */
static void streams_look(uint64_t now) {
    for (struct stream **link = &active, *s; (s = *link);) {
        uint64_t head = head_of(s);
        if (head < s->tail || head > __atomic_load_n(&s->ring->keep, __ATOMIC_RELAXED) + s->capacity) {
            s->ring->broken = 1; /* アプリが壊した。誰にも広げない。 */
            s->tail = head < s->tail ? s->tail : head;
            s->heard = s->tail;
            __atomic_store_n(&s->ring->tail, s->tail, __ATOMIC_RELEASE);
            s->ring->asleep = 0;
            stream_leave(link, s);
            continue;
        }
        uint64_t heard = stream_heard_at(s, out.position);
        if (heard > s->heard) s->heard = heard;
        if (s->heard > s->tail) s->heard = s->tail;
        stream_publish(s, out.position_at ? out.position_at : now);
        stream_ring_room(s);
        if (head > s->tail || s->heard < s->tail) {
            link = &s->next;
            continue;
        }
        /* 全部を聞かせた。次の音まで head を見ない。立てた後でもう一度見る。 */
        if (!s->dry_since) s->dry_since = now;
        __atomic_store_n(&s->ring->asleep, 1, __ATOMIC_SEQ_CST);
        if (head_of(s) != head && __atomic_exchange_n(&s->ring->asleep, 0, __ATOMIC_ACQ_REL)) {
            link = &s->next;
            continue;
        }
        stream_leave(link, s);
    }
}

/* ---- 閉ループ ---- */

static void pace_begin(uint64_t at, double time) {
    pace.begin_at = pace.last_at = at;
    pace.begin_time = time;
    pace.settled = false;
    pace.settle_sum = pace.error = 0;
    pace.settle_count = 0;
}

static void pace_reset(void) { memset(&pace, 0, sizeof pace); pace.window_ns = PACE_WINDOW_NS; }

static void pace_report(uint64_t position, uint64_t at) {
    double rate = out.format.rate;
    uint64_t b = position / out.block_frames, written = out.written / out.block_frames;
    if (b >= written || written - b > out.blocks) {
        pace.running = false;
        return;
    }
    double from = out.timeline[b % out.blocks], to = b + 1 < written ? out.timeline[(b + 1) % out.blocks] : pace.time;
    double time = from + (to - from) * (double)(position % out.block_frames) / out.block_frames;
    if (!pace.running) {
        pace.running = true;
        pace.window_at = at;
        pace.window_position = position;
        pace_begin(at, time);
        return;
    }
    if (at <= pace.last_at) return;
    double error = ((time - pace.begin_time) - (double)(at - pace.begin_at) * rate / 1e9) / rate;
    if (fabs(error) > PACE_LOST_S) {
        pace_begin(at, time);
        return;
    }
    if (!pace.settled) {
        pace.settle_sum += error;
        pace.settle_count++;
        pace.last_at = at;
        if (at - pace.begin_at < PACE_SETTLE_NS) return;
        pace.begin_time += pace.settle_sum / (double)pace.settle_count * rate;
        pace.settled = true;
        return;
    }
    double dt = (double)(at - pace.last_at);
    if (dt > PACE_FILTER_NS) dt = PACE_FILTER_NS;
    pace.last_at = at;
    pace.error += (error - pace.error) * dt / PACE_FILTER_NS;
    pace.integral -= pace.error * PACE_KI * dt / 1e9;
    if (pace.integral > PACE_MOST) pace.integral = PACE_MOST;
    if (pace.integral < -PACE_MOST) pace.integral = -PACE_MOST;
    if (at - pace.window_at >= pace.window_ns) {
        double nominal = (double)(at - pace.window_at) * rate / 1e9, moved = (double)position - (double)pace.window_position;
        double off = moved >= 0 && moved < 2 * nominal ? (nominal - moved) / nominal : 0;
        off = fabs(off) > PACE_FAR ? off : 0;
        pace.integral -= off - pace.far; /* 閉ループが見つけた分は測った値に入っている */
        pace.far = off;
        pace.window_at = at;
        pace.window_position = position;
        pace.window_ns = pace.window_ns * 2 < PACE_WINDOW_MOST_NS ? pace.window_ns * 2 : PACE_WINDOW_MOST_NS;
    }
    pace.ratio = pace.far + pace.integral - pace.error * PACE_KP;
    if (pace.ratio > PACE_MOST) pace.ratio = PACE_MOST;
    if (pace.ratio < -PACE_MOST) pace.ratio = -PACE_MOST;
}

/* ---- 書く ---- */

static void fill(void) {
    if (!out.running) return;
    uint64_t b = out.block_frames, floor_at = block_after_guard();
    if (out.written < floor_at) { /* 間に合わなかった。過ぎた塊には古い音が残る。 */
        /* 一周より古い分は印が要らない */
        uint64_t from = floor_at - out.written > (uint64_t)out.blocks * b ? floor_at - (uint64_t)out.blocks * b : out.written;
        pace.time += (double)(from - out.written) * (1 + pace.ratio);
        for (uint64_t at = from; at < floor_at; at += b) {
            uint64_t block = at / b;
            out.timeline[block % out.blocks] = pace.time;
            pace.time += b * (1 + pace.ratio);
            for (struct stream *s = active; s; s = s->next) s->marks[block % out.blocks] = (struct mark){s->tail, s->phase};
        }
        out.written = floor_at;
        if (out.ahead < AHEAD_HIGH) out.ahead *= 2;
        if (out.ahead > AHEAD_HIGH) out.ahead = AHEAD_HIGH;
    }
    uint64_t ahead = out.ahead < out.blocks - 1 ? out.ahead : out.blocks - 1;
    uint64_t target = floor_at + ahead * b, limit = out.position + (uint64_t)(out.blocks - 1) * b;
    if (target > limit) target = limit;
    while (out.written < target) {
        uint64_t lead = out.written > floor_at ? (out.written - floor_at) / b : 0;
        if (lead >= URGENT_BLOCKS) {
            bool ready = true;
            for (struct stream *s = active; s && ready; s = s->next) ready = stream_ready(s, head_of(s));
            if (!ready) break;
        }
        block_mix();
    }
}

/* ---- 出口 ---- */

static void timer_stop(void) {
    if (!out.ticking) return;
    n_events_unwatch(queue, TOKEN_TIMER);
    out.ticking = false;
}

static void output_stop(void) {
    if (out.running && out.device >= 0) n_audio_device_run(out.device, 0);
    timer_stop();
    out.running = false;
    out.idle_since = 0;
}

static void output_close(void) {
    output_stop();
    if (out.device >= 0) {
        n_events_unwatch(queue, TOKEN_DEVICE);
        n_handle_close(out.device);
        out.device = -1;
    }
    if (out.ring) n_memory_release(out.ring, out.ring_bytes);
    out.ring = NULL;
    free(out.timeline);
    free(out.bus);
    out.timeline = NULL;
    out.bus = NULL;
    out.laid_out = false;
}

/* 時計で数える。出口が無いとき、開けないとき。音は作らない。 */
static bool layout_null(void) {
    out.device = -1;
    out.block_frames = (uint32_t)(out.format.rate * NULL_BLOCK_NS / 1000000000ull);
    out.blocks = NULL_BLOCKS;
    out.clock = N_AUDIO_CLOCK_ESTIMATED;
    out.frame_bytes = 0;
    out.timeline = calloc(out.blocks, sizeof *out.timeline);
    return out.timeline != NULL;
}

/* 出口を開いて形を決める。 */
static bool output_lay_out(void) {
    if (out.laid_out) return true;
    bool ok = false;
    if (out.id) {
        struct n_audio_device_ring ring = {.block_frames = (uint32_t)(out.format.rate * BLOCK_NS / 1000000000ull), .blocks = BLOCKS};
        if (!ring.block_frames) ring.block_frames = 1;
        n_handle device = n_audio_device_open(device_cap, out.id, &out.format, &ring);
        if (device >= 0) {
            out.frame_bytes = (uint32_t)out.format.channels * out.format.container;
            out.ring_bytes = (size_t)ring.block_frames * ring.blocks * out.frame_bytes;
            void *view;
            n_result mapped = n_shared_map(ring.memory, out.ring_bytes, N_MEMORY_READ_WRITE, &view);
            n_handle_close(ring.memory);
            if (mapped >= 0) {
                out.ring = view;
                out.device = device;
                out.block_frames = ring.block_frames;
                out.blocks = ring.blocks;
                out.flush_bytes = ring.flush_bytes;
                out.timeline = calloc(out.blocks, sizeof *out.timeline);
                out.bus = malloc((size_t)out.block_frames * out.format.channels * sizeof *out.bus);
                n_events_watch(queue, device, N_EVENT_RECEIVABLE, TOKEN_DEVICE);
                ok = out.timeline && out.bus;
                if (!ok) output_close();
            } else n_handle_close(device);
        } else printf("audio: output %u does not open (%s)\n", out.id, n_result_text(device));
    }
    if (!ok && !layout_null()) return false;
    out.laid_out = true;
    return true;
}

/* 位置 0 から始める。混ぜてあった音は聞かせたものとして数える。 */
static void output_start(uint64_t now) {
    if (out.running || !out.laid_out) return;
    out.written = out.position = 0;
    out.guard = 0;
    out.ahead = AHEAD_LOW;
    out.idle_since = 0;
    out.position_at = now;
    out.started_at = now;
    pace_reset();
    for (struct stream *s = active; s; s = s->next) {
        s->joined = 0;
        s->join = (struct mark){s->tail, s->phase};
        s->heard = s->tail;
    }
    out.running = true;
    if (out.device < 0) {
        out.ticking = true;
        uint64_t period = (uint64_t)out.block_frames * 1000000000ull / out.format.rate;
        n_events_timer(queue, now + period, period, TOKEN_TIMER);
        fill();
        return;
    }
    fill();
    n_result r = n_audio_device_run(out.device, 1);
    if (r < 0) out.running = false; /* 今は動けない（リンクが下りている）。装置の知らせでまた見る。 */
}

/* 活動中の流れを今の出口に結び直す。 */
static void streams_bind_all(void) {
    for (struct stream **link = &active, *s; (s = *link);) {
        if (stream_bind(s)) {
            link = &s->next;
            continue;
        }
        s->ring->broken = 1;
        stream_leave(link, s);
    }
}

/* ---- 選択 ---- */

#define LIST_EACH(d) \
    for (const struct n_audio_device *d = (const void *)list; (const uint8_t *)d < list + list_bytes; d = (const void *)((const uint8_t *)d + d->bytes))

static n_result devices_read(void) {
    for (;;) {
        n_result r = n_audio_device_list(device_cap, list, list_room);
        if (r < 0) return r;
        if ((size_t)r <= list_room) {
            list_bytes = (size_t)r;
            return 0;
        }
        uint8_t *grown = realloc(list, (size_t)r);
        if (!grown) return -N_ERR_NO_MEMORY;
        list = grown;
        list_room = (size_t)r;
    }
}

static int device_rank(const struct n_audio_device *d) {
    if (!d->available) return -1;
    if (d->kind == N_AUDIO_KIND_REMOTE) return 1000; /* 人は遠隔の相手の前に居る */
    static const int by_kind[] = {[N_AUDIO_KIND_SPEAKER] = 4, [N_AUDIO_KIND_HEADPHONE] = 5, [N_AUDIO_KIND_LINE] = 3,
                                  [N_AUDIO_KIND_DISPLAY] = 2, [N_AUDIO_KIND_DIGITAL] = 1, [N_AUDIO_KIND_OTHER] = 0};
    int jack = d->jack == N_AUDIO_JACK_PLUGGED ? 2 : d->jack == N_AUDIO_JACK_NONE ? 1 : 0;
    return jack * 16 + (d->kind <= N_AUDIO_KIND_OTHER ? by_kind[d->kind] : 0);
}

static const struct n_audio_device *device_best(void) {
    const struct n_audio_device *best = NULL;
    int rank = -1;
    LIST_EACH(d) {
        if (chosen_key && chosen_key[0] && d->available && !strcmp(n_audio_device_key(d), chosen_key)) return d;
        int r = device_rank(d);
        if (r > rank || (r == rank && r >= 0 && d->id == out.id)) {
            best = d;
            rank = r;
        }
    }
    return best;
}

static struct choice *choice_of(uint64_t device, bool add) {
    for (struct choice *c = choices; c; c = c->next)
        if (c->device == device) return c;
    if (!add) return NULL;
    struct choice *c = calloc(1, sizeof *c);
    if (c) {
        c->device = device;
        c->next = choices;
        choices = c;
    }
    return c;
}

static bool rate_declared(const struct n_audio_device *d, uint32_t rate) {
    const uint32_t *rates = n_audio_device_rates(d);
    for (uint32_t i = 0; i < d->rate_count; i++)
        if (rates[i] == rate) return true;
    return false;
}

static const struct n_audio_size *size_declared(const struct n_audio_device *d, uint32_t bits) {
    const struct n_audio_size *sizes = n_audio_device_sizes(d);
    for (uint32_t i = 0; i < d->size_count; i++)
        if (sizes[i].bits == bits) return &sizes[i];
    return NULL;
}

/* 利用者の選択、無ければ既定：48 kHz に一番近い率、一番大きい標本、一番多い channel。 */
static struct n_audio_device_format format_for(const struct n_audio_device *d) {
    const struct choice *c = choice_of(n_setting_device(n_audio_device_key(d)), false);
    uint32_t rate = c && c->rate > 0 && rate_declared(d, (uint32_t)c->rate) ? (uint32_t)c->rate : 0;
    if (!rate) {
        const uint32_t *rates = n_audio_device_rates(d);
        for (uint32_t i = 0; i < d->rate_count; i++) {
            uint32_t now = rate > 48000 ? rate - 48000 : 48000 - rate, then = rates[i] > 48000 ? rates[i] - 48000 : 48000 - rates[i];
            if (!rate || then < now) rate = rates[i];
        }
    }
    const struct n_audio_size *size = c && c->bits > 0 ? size_declared(d, (uint32_t)c->bits) : NULL;
    if (!size && d->size_count) size = &n_audio_device_sizes(d)[d->size_count - 1];
    uint32_t channels = c && c->channels >= (int32_t)d->channels_low && c->channels <= (int32_t)d->channels_high && c->channels ? (uint32_t)c->channels : d->channels_high;
    struct n_audio_device_format f = {.rate = rate, .channels = (uint16_t)channels};
    if (size) {
        f.bits = size->bits;
        f.container = size->container;
    }
    return f;
}

/* 一番良い出口へ。変われば閉じ、鳴っていれば開き直す。 */
static void output_choose(uint64_t now) {
    const struct n_audio_device *d = device_best();
    uint32_t id = d ? d->id : 0;
    struct n_audio_device_format f = d ? format_for(d) : (struct n_audio_device_format){.rate = NULL_RATE, .channels = 2, .bits = 16, .container = 2};
    if (id != out.id || memcmp(&f, &out.format, sizeof f)) {
        output_close();
        out.id = id;
        out.format = f;
    }
    /* 開けずに時計で数えている出口は layout_null が言う */
    if (!out.laid_out || out.device >= 0) out.clock = d && d->kind == N_AUDIO_KIND_REMOTE ? N_AUDIO_CLOCK_REMOTE : d ? N_AUDIO_CLOCK_DEVICE : N_AUDIO_CLOCK_ESTIMATED;
    if (active && !out.laid_out && output_lay_out()) {
        streams_bind_all();
        output_start(now);
    }
}

static void watchers_tell(void) {
    for (struct client *c = clients; c; c = c->next)
        if (c->watching && !c->dead) n_channel_send(c->channel, "", 1, -1);
}

/* ---- 拍 ---- */

static void output_tick(void) {
    uint64_t now = n_clock_monotonic();
    if (!out.laid_out) return;
    if (out.device >= 0) {
        struct n_audio_device_status st;
        n_result r = n_audio_device_status(out.device, &st);
        if (r < 0) { /* 閉じられた：装置が止まった、消えた */
            output_close();
            if (devices_read() >= 0) output_choose(now);
            watchers_tell();
            return;
        }
        if (st.generation != out.generation || (!st.running && out.running)) {
            out.generation = st.generation;
            out.running = false;
        }
        out.errors = st.errors;
        if (!out.running) {
            if (active) output_start(now);
            return;
        }
        out.position = st.position;
        out.position_at = st.at_ns;
        out.guard = st.guard_frames;
    } else {
        if (!out.running) return;
        out.position = (now - out.started_at) * out.format.rate / 1000000000ull;
        out.position_at = now;
    }
    pace_report(out.position, out.position_at);
    fill();
    streams_look(now);
    if (active) {
        out.idle_since = 0;
        return;
    }
    if (!out.idle_since) out.idle_since = now;
    /* 時計で数えるだけなら続ける理由が無い */
    if (out.device < 0 || now - out.idle_since >= IDLE_NS) output_stop();
}

/* ---- 流れの操作 ---- */

static void stream_activate(struct stream *s, uint64_t now) {
    if (s->active) return;
    if (!out.laid_out && !output_lay_out()) return;
    if (!stream_bind(s)) {
        s->ring->broken = 1;
        return;
    }
    if (s->dry_since) {
        s->underruns++;
        s->underrun_ns += now - s->dry_since;
        s->dry_since = 0;
    }
    s->active = true;
    s->next = active;
    active = s;
    if (!out.running) output_start(now);
    else fill();
}

/* アプリが輪に音を書いた。 */
static void stream_kick(struct stream *s, uint64_t now) {
    __atomic_store_n(&s->ring->asleep, 0, __ATOMIC_SEQ_CST);
    uint64_t head = head_of(s);
    if (head < s->tail || head > __atomic_load_n(&s->ring->keep, __ATOMIC_RELAXED) + s->capacity) {
        s->ring->broken = 1;
        return;
    }
    if (head > s->tail) stream_activate(s, now);
    else __atomic_store_n(&s->ring->asleep, 1, __ATOMIC_SEQ_CST);
}

/* 流れの音を捨てる。装置がまだ読まない塊まで全部の流れを戻し、捨てた流れ抜きで混ぜ直す。 */
static void stream_flush(struct stream *s) {
    uint64_t now = n_clock_monotonic();
    if (out.running && out.laid_out && s->active) {
        uint64_t again = block_after_guard();
        if (again < out.written) {
            uint64_t block = again / out.block_frames;
            for (struct stream *o = active; o; o = o->next) {
                struct mark m = block > o->joined ? o->marks[block % out.blocks] : o->join;
                o->tail = m.tail;
                o->phase = m.phase;
            }
            pace.time = out.timeline[block % out.blocks];
            out.written = again;
        }
    }
    uint64_t head = head_of(s);
    s->tail = s->base = s->heard = head;
    s->phase = 0;
    s->dry_since = 0;
    s->generation++;
    if (s->active) {
        s->joined = (out.written + out.block_frames - 1) / out.block_frames;
        s->join = (struct mark){head, 0};
    }
    __atomic_store_n(&s->ring->keep, head, __ATOMIC_RELEASE);
    stream_publish(s, now);
    __atomic_store_n(&s->ring->tail, head, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s->ring->room_bell, 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&s->ring->wants_room, 0, __ATOMIC_ACQ_REL)) wake_address(&s->ring->room_bell, 0x7fffffff);
    if (out.running) {
        fill();
        streams_look(now);
    }
}

/* 流れを作る。輪は触れた頁だけが実の記憶になる。 */
static n_result stream_open(struct client *c, const struct n_audio_request *q, n_handle *memory, uint64_t *bytes_out) {
    const struct n_audio_format *f = &q->format;
    if (c->stream) return -N_ERR_BUSY;
    if (!f->rate || !f->channels || f->sample > N_AUDIO_F32) return -N_ERR_INVALID;
    uint64_t frame_bytes = (uint64_t)f->channels * sample_bytes(f->sample);
    uint64_t queue_frames = QUEUE_FRAMES(f->rate), capacity = queue_frames + LATE_FRAMES(f->rate);
    if (capacity > UINT32_MAX || capacity > (UINT64_MAX / 2) / frame_bytes) return -N_ERR_TOO_LARGE;
    uint32_t offset = (uint32_t)((sizeof(struct n_audio_ring) + 63u) & ~63u);
    size_t bytes = (size_t)(offset + capacity * frame_bytes);
    n_handle h = n_shared_reserve(bytes);
    if (h < 0) return h;
    void *view;
    n_result r = n_shared_map(h, bytes, N_MEMORY_READ_WRITE, &view);
    struct stream *s = r >= 0 ? calloc(1, sizeof *s) : NULL;
    if (!s) {
        if (r >= 0) n_memory_release(view, bytes);
        n_handle_close(h);
        return r < 0 ? r : -N_ERR_NO_MEMORY;
    }
    s->format = *f;
    s->ring = view;
    s->data = (uint8_t *)view + offset;
    s->bytes = bytes;
    s->capacity = (uint32_t)capacity;
    s->queue = (uint32_t)queue_frames;
    s->frame_bytes = (uint32_t)frame_bytes;
    s->tail = s->heard = q->first;
    s->base = q->base;
    s->underruns = q->underruns;
    s->underrun_ns = q->underrun_ns;
    s->generation = q->generation;
    s->half = 1;
    struct n_audio_ring *ring = view;
    ring->head = ring->tail = ring->keep = q->first;
    ring->asleep = 1;
    ring->capacity = s->capacity;
    ring->queue = s->queue;
    ring->frame_bytes = s->frame_bytes;
    ring->data_offset = offset;
    stream_publish(s, n_clock_monotonic());
    c->stream = s;
    *memory = h;
    *bytes_out = bytes;
    return 0;
}

/* ---- 接続 ---- */

static void client_end(struct client *c) {
    if (c->dead) return;
    c->dead = true;
    n_events_unwatch(queue, (uint64_t)(uintptr_t)c);
    n_handle_close(c->channel);
    for (struct client **at = &clients; *at; at = &(*at)->next)
        if (*at == c) {
            *at = c->next;
            break;
        }
    c->next = dead_clients;
    dead_clients = c;
    struct stream *s = c->stream;
    if (!s) return;
    c->stream = NULL;
    s->closing = true;
    uint64_t head = head_of(s);
    /* 残りを鳴らしてから消える */
    if (!s->active && head > s->tail && head <= __atomic_load_n(&s->ring->keep, __ATOMIC_RELAXED) + s->capacity) stream_activate(s, n_clock_monotonic());
    if (!s->active) stream_free(s);
}

/* 全部の出口を一通の中に：struct n_audio_output の頭と申告の並び。 */
static void outputs_answer(struct client *c) {
    size_t bytes = 0;
    LIST_EACH(d) bytes += sizeof(struct n_audio_output) + d->bytes;
    uint8_t *message = bytes ? malloc(bytes) : NULL;
    struct n_audio_reply reply = {.result = bytes && !message ? -N_ERR_NO_MEMORY : 0, .bytes = message ? bytes : 0};
    n_channel_send(c->channel, &reply, sizeof reply, -1);
    if (!message) return;
    uint8_t *at = message;
    LIST_EACH(d) {
        struct n_audio_output *o = (struct n_audio_output *)at;
        *o = (struct n_audio_output){.bytes = (uint32_t)sizeof *o + d->bytes, .playing = d->id == out.id, .format = format_for(d)};
        memcpy(at + sizeof *o, d, d->bytes);
        at += o->bytes;
    }
    n_channel_send(c->channel, message, bytes, -1);
    free(message);
}

static void client_event(struct client *c, uint32_t events) {
    if (c->dead) return;
    if (events & N_EVENT_RECEIVABLE) {
        struct n_audio_request q;
        n_result got = n_channel_receive(c->channel, &q, sizeof q, 0, NULL);
        if (got == -N_ERR_NOT_READY) return;
        if (got != sizeof q) {
            client_end(c);
            return;
        }
        struct n_audio_reply reply = {.result = 0};
        n_handle attached = -1;
        switch (q.op) {
        case N_AUDIO_OP_OPEN:
            reply.result = stream_open(c, &q, &attached, &reply.bytes);
            break;
        case N_AUDIO_OP_FLUSH:
            if (c->stream) stream_flush(c->stream);
            else reply.result = -N_ERR_INVALID;
            break;
        case N_AUDIO_OP_KICK:
            if (c->stream) stream_kick(c->stream, n_clock_monotonic());
            return;
        case N_AUDIO_OP_OUTPUTS:
            outputs_answer(c);
            return;
        case N_AUDIO_OP_WATCH:
            c->watching = true;
            break;
        default:
            reply.result = -N_ERR_INVALID;
            break;
        }
        if (n_channel_send(c->channel, &reply, sizeof reply, attached) < 0) {
            if (attached >= 0) n_handle_close(attached);
            client_end(c);
            return;
        }
        if (attached >= 0) n_handle_close(attached);
    } else if (events & N_EVENT_CLOSED) {
        client_end(c);
    }
}

/* ---- 設定 ---- */

static const struct n_setting_decl declarations[] = {
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_VOLUME, .type = N_SETTING_INT, .scope = N_SCOPE_USER, .apply = N_APPLY_NOW, .min = 0, .max = 100,
     .fallback = 80, .name = "audio.volume", .label = "音量", .about = "全部の音の大きさです。"},
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_MUTED, .type = N_SETTING_BOOL, .scope = N_SCOPE_USER, .apply = N_APPLY_NOW, .max = 1,
     .name = "audio.muted", .label = "消音", .about = "全部の音を止めます。音量はそのままです。"},
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_OUTPUT, .type = N_SETTING_STRING, .scope = N_SCOPE_USER, .apply = N_APPLY_NOW, .flags = N_SETTING_DECL_CUSTOM,
     .max = N_SETTING_VALUE_MAX, .name = "audio.output", .label = "出口", .about = "音を出す出口です。空なら一番良いものを選びます。"},
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_FORMAT_RATE, .type = N_SETTING_INT, .scope = N_SCOPE_DEVICE, .apply = N_APPLY_NOW, .flags = N_SETTING_DECL_CUSTOM,
     .max = INT32_MAX, .name = "audio.format-rate", .label = "周波数", .about = "この出口の周波数です。0 は既定です。"},
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_FORMAT_BITS, .type = N_SETTING_INT, .scope = N_SCOPE_DEVICE, .apply = N_APPLY_NOW, .flags = N_SETTING_DECL_CUSTOM,
     .max = INT32_MAX, .name = "audio.format-bits", .label = "標本の大きさ", .about = "この出口の標本のビット数です。0 は既定です。"},
    {.domain = N_DOMAIN_AUDIO, .key = N_AUDIO_FORMAT_CHANNELS, .type = N_SETTING_INT, .scope = N_SCOPE_DEVICE, .apply = N_APPLY_NOW, .flags = N_SETTING_DECL_CUSTOM,
     .max = INT32_MAX, .name = "audio.format-channels", .label = "チャンネル数", .about = "この出口のチャンネル数です。0 は既定です。"},
};

/* settings への接続を作り、全部を宣言する。接続を持ち主にするので、値はこの接続へ届く。 */
static n_handle settings_link(void) {
    n_handle h = n_service_connect(N_SETTINGS_SERVICE);
    if (h < 0) return h;
    for (size_t i = 0; i < sizeof declarations / sizeof *declarations; i++) {
        struct {
            struct n_settings_request m;
            struct n_setting_decl d;
        } q = {.m = {.op = N_SETTINGS_DECLARE, .serial = (uint32_t)i + 1, .length = sizeof(struct n_setting_decl)}, .d = declarations[i]};
        n_result r = n_channel_send(h, &q, sizeof q, -1);
        if (r < 0) {
            n_handle_close(h);
            return r;
        }
    }
    return h;
}

/* settings が起き直したら繋ぎ直す。待つのは主のスレッドでない。 */
static void settings_relink(void *arg) {
    (void)arg;
    n_handle h;
    while ((h = settings_link()) < 0) n_sleep_until(n_clock_after(500 * N_NS_PER_MS));
    n_channel_send(linked[1], &h, sizeof h, -1);
}

static int32_t value_int(const void *data, uint32_t length, bool *ok) {
    int32_t v = 0;
    *ok = length == sizeof v;
    if (*ok) memcpy(&v, data, sizeof v);
    return v;
}

static n_result settings_apply(const struct n_settings_request *m, const void *data, uint64_t now) {
    bool ok = false;
    switch (m->id.key) {
    case N_AUDIO_VOLUME: {
        int32_t v = value_int(data, m->length, &ok);
        if (!ok || v < 0 || v > 100) return -N_ERR_INVALID;
        volume = v;
        return 0;
    }
    case N_AUDIO_MUTED: {
        int32_t v = value_int(data, m->length, &ok);
        if (!ok || (v != 0 && v != 1)) return -N_ERR_INVALID;
        muted = v != 0;
        return 0;
    }
    case N_AUDIO_OUTPUT: {
        char *key = malloc((size_t)m->length + 1);
        if (!key) return -N_ERR_NO_MEMORY;
        memcpy(key, data, m->length);
        key[m->length] = 0;
        free(chosen_key);
        chosen_key = key;
        break;
    }
    case N_AUDIO_FORMAT_RATE:
    case N_AUDIO_FORMAT_BITS:
    case N_AUDIO_FORMAT_CHANNELS: {
        int32_t v = value_int(data, m->length, &ok);
        if (!ok || v < 0) return -N_ERR_INVALID;
        LIST_EACH(d) {
            if (n_setting_device(n_audio_device_key(d)) != m->id.device || !v) continue;
            bool fits = m->id.key == N_AUDIO_FORMAT_RATE ? rate_declared(d, (uint32_t)v)
                      : m->id.key == N_AUDIO_FORMAT_BITS ? size_declared(d, (uint32_t)v) != NULL
                                                         : (uint32_t)v >= d->channels_low && (uint32_t)v <= d->channels_high;
            if (!fits) return -N_ERR_INVALID;
        }
        struct choice *c = choice_of(m->id.device, true);
        if (!c) return -N_ERR_NO_MEMORY;
        if (m->id.key == N_AUDIO_FORMAT_RATE) c->rate = v;
        else if (m->id.key == N_AUDIO_FORMAT_BITS) c->bits = v;
        else c->channels = v;
        break;
    }
    default:
        return -N_ERR_NOT_FOUND;
    }
    output_choose(now);
    watchers_tell();
    return 0;
}

/* settings からの一通。宣言の答えを数える。 */
static uint32_t declared_answers;
static uint8_t *settings_message;

static void settings_handle(n_result got) {
    if ((size_t)got >= sizeof(struct n_settings_reply) && ((const struct n_settings_reply *)settings_message)->op == N_SETTINGS_DECLARE) {
        const struct n_settings_reply *a = (const void *)settings_message;
        if (a->result < 0) printf("audio: a declaration was refused (%s)\n", n_result_text(a->result));
        declared_answers++;
        return;
    }
    if ((size_t)got < sizeof(struct n_settings_request)) return;
    const struct n_settings_request *m = (const void *)settings_message;
    if (m->op != N_SETTINGS_APPLY || (size_t)got < sizeof *m + m->length) return;
    struct n_settings_reply reply = {.op = N_SETTINGS_APPLY, .serial = m->serial, .result = settings_apply(m, m + 1, n_clock_monotonic())};
    if (m->serial) n_channel_send(settings, &reply, sizeof reply, -1);
}

static void settings_event(void) {
    n_result got = n_channel_receive(settings, settings_message, N_SETTINGS_MESSAGE_MAX, 0, NULL);
    if (got == -N_ERR_NOT_READY) return;
    if (got < 0) {
        n_events_unwatch(queue, TOKEN_SETTINGS);
        n_handle_close(settings);
        settings = -1;
        n_result t = thread_create(settings_relink, NULL);
        if (t >= 0) thread_detach((int)t);
        return;
    }
    settings_handle(got);
}

/* ---- 始まり ---- */

int main(void) {
    device_cap = n_cap_find(N_CAP_AUDIO_DEVICE);
    queue = n_events_create();
    if (device_cap < 0 || queue < 0 || n_channel_pair(linked) < 0) {
        printf("audio: the capability was not given (%s)\n", n_result_text(device_cap < 0 ? device_cap : queue));
        return 1;
    }
    device_watch = n_audio_device_watch(device_cap);
    if (device_watch >= 0) n_events_watch(queue, device_watch, N_EVENT_RECEIVABLE, TOKEN_WATCH);
    n_events_watch(queue, linked[0], N_EVENT_RECEIVABLE, TOKEN_LINKED);
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_INLET);
    devices_read();
    output_choose(n_clock_monotonic());
    settings_message = malloc(N_SETTINGS_MESSAGE_MAX);
    if (!settings_message) return 1;
    settings = settings_link();
    if (settings >= 0) {
        /* 依存する者（session）が鍵を読む前に宣言を済ませる */
        while (declared_answers < sizeof declarations / sizeof *declarations) {
            n_result got = n_channel_receive(settings, settings_message, N_SETTINGS_MESSAGE_MAX, N_CHANNEL_WAIT, NULL);
            if (got < 0) break;
            settings_handle(got);
        }
        n_events_watch(queue, settings, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_SETTINGS);
    } else printf("audio: settings cannot be reached (%s)\n", n_result_text(settings));
    n_thread_schedule(0, N_SCHEDULE_DEADLINE, BLOCK_NS);
    n_service_ready();

    for (;;) {
        struct n_event reports[16]; /* 一度に受ける数。残りは次の待ちで届く。 */
        n_result got = n_events_wait(queue, reports, sizeof reports / sizeof *reports, N_CLOCK_NEVER);
        if (got < 0) return 1;
        for (n_result i = 0; i < got; i++) {
            uint64_t token = reports[i].token;
            switch (token) {
            case TOKEN_INLET: {
                n_handle channel = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
                if (channel == -N_ERR_CLOSED) return 0;
                if (channel < 0) break;
                struct client *c = calloc(1, sizeof *c);
                if (!c) {
                    n_handle_close(channel);
                    break;
                }
                c->channel = channel;
                c->next = clients;
                clients = c;
                n_events_watch(queue, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, (uint64_t)(uintptr_t)c);
                break;
            }
            case TOKEN_WATCH:
                n_watch_take(device_watch);
                if (devices_read() >= 0) output_choose(n_clock_monotonic());
                watchers_tell();
                break;
            case TOKEN_SETTINGS:
                settings_event();
                break;
            case TOKEN_LINKED: {
                n_handle h;
                if (n_channel_receive(linked[0], &h, sizeof h, 0, NULL) == sizeof h) {
                    settings = h;
                    n_events_watch(queue, settings, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_SETTINGS);
                }
                break;
            }
            case TOKEN_DEVICE:
            case TOKEN_TIMER:
                output_tick();
                break;
            default:
                client_event((struct client *)(uintptr_t)token, reports[i].events);
                break;
            }
        }
        while (dead_clients) {
            struct client *c = dead_clients;
            dead_clients = c->next;
            free(c);
        }
    }
}
