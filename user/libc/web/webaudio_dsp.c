/* The node kernels of the Web Audio renderer (webaudio_node.h): what each kind of node does
   with its inputs and params in a render quantum, and the Fourier transform and wave tables
   they share. Everything here runs with the context's lock held. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "webaudio_node.h"

/* ---- the discrete Fourier transform ---- */

/* cos and sin of 2πk/n for k < n/2, made once for each size and kept. */
struct twiddles { unsigned n; double *cos, *sin; };
static struct twiddles *twiddle_cache[32];

static const struct twiddles *twiddles(unsigned n) {
    unsigned log = (unsigned)__builtin_ctz(n);
    struct twiddles *t = __atomic_load_n(&twiddle_cache[log], __ATOMIC_ACQUIRE);
    if (t) return t;
    t = malloc(sizeof *t + (n / 2 + 1) * 2 * sizeof(double));
    if (!t) return NULL;
    t->n = n;
    t->cos = (double *)(t + 1);
    t->sin = t->cos + n / 2 + 1;
    for (unsigned k = 0; k < n / 2; k++) {
        t->cos[k] = cos(2 * M_PI * k / n);
        t->sin[k] = sin(2 * M_PI * k / n);
    }
    struct twiddles *made = NULL;
    if (!__atomic_compare_exchange_n(&twiddle_cache[log], &made, t, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(t);
        t = made;
    }
    return t;
}

/* In place: x[j] = Σ x[i] e^(∓2πi·ij/n), the minus sign forward; unscaled. */
static void fft_complex(const struct twiddles *t, double *re, double *im, bool inverse) {
    unsigned n = t->n;
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double r = re[i], m = im[i];
            re[i] = re[j]; im[i] = im[j];
            re[j] = r; im[j] = m;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        unsigned half = len / 2, step = n / len;
        for (unsigned i = 0; i < n; i += len)
            for (unsigned k = 0; k < half; k++) {
                double wr = t->cos[k * step], wi = inverse ? t->sin[k * step] : -t->sin[k * step];
                unsigned a = i + k, b = a + half;
                double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}

/* The transform of n real points (n a power of two, at least 4), through one of n/2
   complex points: n/2 + 1 bins. */
struct fft { unsigned n; const struct twiddles *half, *full; };

static bool fft_ready(struct fft *f, unsigned n) {
    f->n = n;
    f->half = twiddles(n / 2);
    f->full = twiddles(n);
    return f->half && f->full;
}

/* The DFT of x: bins 0..n/2 into re and im (n/2 + 1 of each). */
static void rfft(const struct fft *f, const double *x, double *re, double *im) {
    unsigned m = f->n / 2;
    for (unsigned k = 0; k < m; k++) {
        re[k] = x[2 * k];
        im[k] = x[2 * k + 1];
    }
    fft_complex(f->half, re, im, false);
    /* Z holds the even samples' transform E and the odd ones' O as E + iO; then
       X[k] = E[k] + W^k O[k], with E[m-k] = conj E[k] and O[m-k] = conj O[k]. */
    double z0r = re[0], z0i = im[0];
    re[0] = z0r + z0i;
    im[0] = 0;
    re[m] = z0r - z0i;
    im[m] = 0;
    for (unsigned k = 1; k <= m / 2; k++) {
        unsigned j = m - k;
        double ar = re[k], ai = im[k], br = re[j], bi = im[j];
        double er = (ar + br) / 2, ei = (ai - bi) / 2, or = (ai + bi) / 2, oi = (br - ar) / 2;
        double wr = f->full->cos[k], wi = -f->full->sin[k];
        re[k] = er + wr * or - wi * oi;
        im[k] = ei + wr * oi + wi * or;
        if (j != k) {
            double vr = f->full->cos[j], vi = -f->full->sin[j];
            re[j] = er + vr * or + vi * oi;
            im[j] = -ei - vr * oi + vi * or;
        }
    }
}

/* The n real points whose DFT is bins 0..n/2 of re and im (which it uses up). */
static void irfft(const struct fft *f, double *re, double *im, double *x) {
    unsigned m = f->n / 2;
    double x0 = re[0], xm = re[m];
    re[0] = (x0 + xm) / 2;
    im[0] = (x0 - xm) / 2;
    for (unsigned k = 1; k <= m / 2; k++) {
        unsigned j = m - k;
        double ar = re[k], ai = im[k], br = re[j], bi = im[j];
        /* E[k] = (X[k] + conj X[m-k]) / 2, O[k] = (X[k] - conj X[m-k]) W^-k / 2, Z = E + iO */
        double er = (ar + br) / 2, ei = (ai - bi) / 2, dr = (ar - br) / 2, di = (ai + bi) / 2;
        double wr = f->full->cos[k], wi = f->full->sin[k];
        double or = dr * wr - di * wi, oi = dr * wi + di * wr;
        re[k] = er - oi;
        im[k] = ei + or;
        if (j != k) {
            double vr = f->full->cos[j], vi = f->full->sin[j];
            double pr = -dr * vr - di * vi, pi = -dr * vi + di * vr;
            re[j] = er - pi;
            im[j] = -ei + pr;
        }
    }
    fft_complex(f->half, re, im, true);
    double scale = 1.0 / m;
    for (unsigned k = 0; k < m; k++) {
        x[2 * k] = re[k] * scale;
        x[2 * k + 1] = im[k] * scale;
    }
}

/* ---- PeriodicWave: band-limited wave tables (Web Audio 1.30) ---- */

#define WAVE_SIZE 4096
#define WAVE_TABLES 12

/* Tables of one period, each with fewer partials than the one before, so that a frequency
   plays one whose partials stay below the Nyquist frequency. Each table holds WAVE_SIZE + 3
   samples: the period's last, the period, then its first two, for the cubic interpolation. */
struct wa_wave {
    uint32_t refs;
    bool kept;                   /* a built-in wave: made once, never freed */
    unsigned ntables;
    unsigned partials[WAVE_TABLES];
    float *table[WAVE_TABLES];
};

static void wave_free(struct wa_wave *w) {
    for (unsigned t = 0; t < w->ntables; t++) free(w->table[t]);
    free(w);
}

void wa_wave_unref(struct wa_wave *w) {
    if (w && !w->kept && !__atomic_sub_fetch(&w->refs, 1, __ATOMIC_ACQ_REL)) wave_free(w);
}

static struct wa_wave *wave_ref(struct wa_wave *w) {
    if (w && !w->kept) __atomic_add_fetch(&w->refs, 1, __ATOMIC_RELAXED);
    return w;
}

/* The tables of the wave Σ a[k] cos(kθ) + b[k] sin(kθ), k = 1..top, scaled so that its
   largest magnitude is 1 when normalize says so. */
static struct wa_wave *wave_build(const double *a, const double *b, unsigned top, bool normalize) {
    struct wa_wave *w = calloc(1, sizeof *w);
    double *re = malloc((WAVE_SIZE / 2 + 1) * sizeof *re), *im = malloc((WAVE_SIZE / 2 + 1) * sizeof *im);
    double *x = malloc(WAVE_SIZE * sizeof *x);
    struct fft f;
    bool ok = w && re && im && x && fft_ready(&f, WAVE_SIZE);
    double scale = 1;
    for (unsigned t = 0, p = top; ok && t < WAVE_TABLES; t++) {
        memset(re, 0, (WAVE_SIZE / 2 + 1) * sizeof *re);
        memset(im, 0, (WAVE_SIZE / 2 + 1) * sizeof *im);
        for (unsigned k = 1; k <= p; k++) {
            re[k] = a[k] * (WAVE_SIZE / 2);
            im[k] = -b[k] * (WAVE_SIZE / 2);
        }
        irfft(&f, re, im, x);
        if (t == 0 && normalize) {
            double most = 0;
            for (unsigned i = 0; i < WAVE_SIZE; i++) most = fmax(most, fabs(x[i]));
            if (most > 0) scale = 1 / most;
        }
        float *table = malloc((WAVE_SIZE + 3) * sizeof *table);
        if (!table) { ok = false; break; }
        for (unsigned i = 0; i < WAVE_SIZE; i++) table[i + 1] = (float)(x[i] * scale);
        table[0] = table[WAVE_SIZE];
        table[WAVE_SIZE + 1] = table[1];
        table[WAVE_SIZE + 2] = table[2];
        w->table[t] = table;
        w->partials[t] = p;
        w->ntables = t + 1;
        if (p <= 1) break;
        p /= 2;
    }
    free(re);
    free(im);
    free(x);
    if (!ok) {
        if (w) wave_free(w);
        return NULL;
    }
    w->refs = 1;
    return w;
}

struct wa_wave *wa_wave_new(const float *real, const float *imag, size_t n, bool normalize) {
    double *a = calloc(WAVE_SIZE / 2, sizeof *a), *b = calloc(WAVE_SIZE / 2, sizeof *b);
    struct wa_wave *w = NULL;
    if (a && b) {
        unsigned top = 0;
        for (size_t k = 1; k < n && k < WAVE_SIZE / 2; k++) {
            a[k] = real ? real[k] : 0;
            b[k] = imag ? imag[k] : 0;
            if (a[k] != 0 || b[k] != 0) top = (unsigned)k;
        }
        w = wave_build(a, b, top, normalize);
    }
    free(a);
    free(b);
    return w;
}

/* The built-in types' waves (Web Audio 1.30.4), normalized, made when first played. */
static struct wa_wave *builtin_waves[4];

static struct wa_wave *builtin_wave(unsigned type) {
    struct wa_wave *w = __atomic_load_n(&builtin_waves[type], __ATOMIC_ACQUIRE);
    if (w) return w;
    double *a = calloc(WAVE_SIZE / 2, sizeof *a), *b = calloc(WAVE_SIZE / 2, sizeof *b);
    if (a && b) {
        unsigned top = type == 0 ? 1 : WAVE_SIZE / 2 - 1;
        for (unsigned k = 1; k <= top; k++) {
            double odd = k & 1 ? 1 : -1;
            b[k] = type == 0 ? 1
                 : type == 1 ? 2 / (k * M_PI) * (1 - (k & 1 ? -1 : 1))
                 : type == 2 ? odd * 2 / (k * M_PI)
                 : 8 * sin(k * M_PI / 2) / ((M_PI * k) * (M_PI * k));
        }
        w = wave_build(a, b, top, true);
    }
    free(a);
    free(b);
    if (!w) return NULL;
    w->kept = true;
    struct wa_wave *made = NULL;
    if (!__atomic_compare_exchange_n(&builtin_waves[type], &made, w, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        wave_free(w);
        w = made;
    }
    return w;
}

/* The table's value at x periods·WAVE_SIZE, by the cubic through the four nearest samples. */
static double table_at(const float *table, double x) {
    int i = (int)x;
    double f = x - i;
    const float *p = table + i;
    return p[0] * (-f * (f - 1) * (f - 2) / 6) + p[1] * ((f + 1) * (f - 1) * (f - 2) / 2) +
           p[2] * (-(f + 1) * f * (f - 2) / 2) + p[3] * ((f + 1) * f * (f - 1) / 6);
}

/* For a frequency whose partials below limit sound (k·|f| < nyquist), the table with the most
   partials that all do, and the one with more that it fades towards as limit grows. */
struct wave_pick { const float *low, *high; double weight; };

static void wave_pick(const struct wa_wave *w, double limit, struct wave_pick *out) {
    double allowed = ceil(limit) - 1;
    unsigned t = 0;
    while (t + 1 < w->ntables && w->partials[t] > allowed) t++;
    out->low = w->table[t];
    out->high = NULL;
    out->weight = 0;
    if (t > 0 && w->partials[t] <= allowed) {
        out->high = w->table[t - 1];
        out->weight = (allowed - w->partials[t]) / (double)(w->partials[t - 1] - w->partials[t]);
    }
}

/* ---- the kernels' state ---- */

struct oscillator { struct wa_wave *wave; bool started; double phase; /* in periods */ };

struct buffer_source {
    struct wa_buffer *buffer;
    bool loop, started, entered_loop;
    double loop_start, loop_end;          /* seconds */
    double position;                      /* the playhead, in frames of the buffer */
    double played;                        /* frames of the buffer played, for duration */
};

struct delay {
    unsigned channels;
    size_t length, at;                    /* the line's frames, and where this quantum is written */
    float *line;                          /* channels × length */
    double max, last;                     /* maxDelayTime, and the last quantum's longest delay (s) */
};

struct biquad {
    unsigned type, channels;
    double (*z)[4];                       /* per channel: x1 x2 y1 y2 */
    uint64_t tail;
};

#define IIR_MAX 20
#define IIR_RING 32
struct iir {
    unsigned nb, na, channels, at;
    double b[IIR_MAX], a[IIR_MAX];        /* a[0] is 1 */
    double *x, *y;                        /* per channel: IIR_RING past inputs and outputs */
    uint64_t tail;
};

struct compressor {
    unsigned channels, lag;               /* the pre-delay in frames */
    size_t at;
    float *line;                          /* channels × lag */
    double detector, gain;
    float reduction;
};

/* One stage of a uniformly partitioned convolution: the impulse response from offset over
   parts blocks of block frames, by overlap-save with transforms of 2·block points. The
   spectra are kept and multiplied as float: half the memory, twice the products a vector
   holds, and an error like the output's own rounding. */
struct partitioned {
    unsigned block, bins, parts, head;
    struct fft fft;
    float *ir_re, *ir_im;                 /* per response channel: parts × bins */
    float *fdl_re, *fdl_im;               /* per input channel: the last parts input spectra */
    float *acc_re, *acc_im;
    double *window;                       /* per input channel: the last two input blocks */
    double *time, *re, *im;               /* the transforms' work */
};

/* The response's first LONG_BLOCK frames go through quantum-sized blocks, so the output has
   no latency; the rest through blocks of LONG_BLOCK, each run once its input block is whole,
   its output due from the next quantum on (that part of the response starts LONG_BLOCK
   frames in). Both stages see two input channels (a mono input twice) and make two outputs. */
#define LONG_BLOCK 2048
struct convolver {
    bool normalize;
    unsigned response_channels;
    size_t length;
    struct partitioned head, tail;
    bool has_tail;
    unsigned filled;                      /* frames of the long stage's input block received */
    double *long_in, *long_out;           /* 2 × LONG_BLOCK each: the long stage's block in and out */
    float *ring;                          /* 2 × 2·LONG_BLOCK: the long stage's output, by frame */
    uint64_t frames;                      /* frames processed */
};

struct panner {
    unsigned model, distance_model;
    double ref, max, rolloff, inner, outer, outer_gain;
    /* HRTF: the far ear's delay and each ear's head-shadow filter */
    float ears[2][64];
    unsigned at;
    double shadow[2][2];                  /* per ear: x1, y1 */
};

#define OVERSAMPLE_TAPS 47
struct waveshaper {
    float *curve;
    size_t length;
    unsigned oversample;                  /* 0, 1 (2x), 2 (4x) */
    unsigned channels;
    float *history;                       /* per channel: 4 filters' OVERSAMPLE_TAPS - 1 past samples */
};

#define ANALYSER_RING 32768
struct analyser {
    unsigned fft_size;
    double smoothing, min_db, max_db;
    float ring[ANALYSER_RING];
    size_t at;
    struct wa_bus mono;
    struct fft fft;
    double *window, *re, *im, *smoothed, *work;
    uint64_t analysed;                    /* the frame the frequency data was made at, + 1 */
};

/* ---- shared helpers ---- */

static struct wa_bus *input_bus(struct wa_node *n, unsigned i) { return &n->inputs[i].bus; }

/* The output with channels channels, zeroed; on running out of memory, one silent channel. */
static float *output_ready(struct wa_node *n, unsigned output, unsigned channels) {
    struct wa_bus *b = &n->outputs[output];
    if (wa_bus_ready(b, channels)) return b->data;
    wa_bus_silence(b, 1);
    return NULL;
}

static double param_at(const struct wa_param *p, unsigned k) { return p->values[p->constant ? 0 : k]; }

/* ---- sources ---- */

static void oscillator(struct wa_node *n) {
    struct oscillator *s = n->dsp;
    struct wa_context *c = n->ctx;
    unsigned from, to;
    if (!wa_source_span(n, &from, &to) || !output_ready(n, 0, 1)) {
        wa_bus_silence(&n->outputs[0], 1);
        return;
    }
    const struct wa_param *frequency = &n->params[0], *detune = &n->params[1];
    double nyquist = c->rate / 2.0;
    float *o = wa_channel(&n->outputs[0], 0);
    bool steady = frequency->constant && detune->constant;
    double f = frequency->values[from] * exp2(detune->values[from] / 1200.0);
    struct wave_pick pick = {0};
    if (fabs(f) < nyquist) wave_pick(s->wave, nyquist / fabs(f), &pick);
    if (!s->started) {
        /* the phase the wave has reached at the first frame, started between two frames */
        s->started = true;
        double late = wa_time(c, c->frame + from) - n->start;
        s->phase = isfinite(f) && late > 0 ? f * late - floor(f * late) : 0;
    }
    for (unsigned k = from; k < to; k++) {
        if (!steady && k > from) {
            f = frequency->values[k] * exp2(detune->values[k] / 1200.0);
            if (fabs(f) < nyquist) wave_pick(s->wave, nyquist / fabs(f), &pick);
        }
        /* NaN, and frequencies at or past the Nyquist frequency, are silence */
        if (!(fabs(f) < nyquist)) continue;
        double x = s->phase * WAVE_SIZE;
        if (x >= WAVE_SIZE) x -= WAVE_SIZE;
        double v = table_at(pick.low, x);
        if (pick.high) v += pick.weight * (table_at(pick.high, x) - v);
        o[k] = (float)v;
        s->phase += f / c->rate;
        s->phase -= floor(s->phase);
    }
}

static void constant_source(struct wa_node *n) {
    unsigned from, to;
    if (!wa_source_span(n, &from, &to) || !output_ready(n, 0, 1)) {
        wa_bus_silence(&n->outputs[0], 1);
        return;
    }
    float *o = wa_channel(&n->outputs[0], 0);
    for (unsigned k = from; k < to; k++) o[k] = (float)param_at(&n->params[0], k);
}

/* The buffer's value at position (frames) in channel ch: linear between its frames, the
   loop's start following its end once in the loop, the last frame held at the end. */
static float buffer_at(const struct buffer_source *s, const struct wa_buffer *b, unsigned ch, double position,
                       size_t loop_start, size_t loop_end) {
    size_t i = (size_t)position;
    double f = position - (double)i;
    const float *d = b->data[ch];
    if (f == 0 || i + 1 > b->length) return d[i];
    size_t next = i + 1;
    if (s->entered_loop && next >= loop_end) next = loop_start;
    if (next >= b->length) return d[i];
    return (float)(d[i] + (d[next] - d[i]) * f);
}

/* AudioBufferSourceNode's playback (Web Audio 1.9.5), its playhead in frames of the buffer. */
static void buffer_source(struct wa_node *n) {
    struct buffer_source *s = n->dsp;
    struct wa_context *c = n->ctx;
    struct wa_buffer *b = s->buffer;
    unsigned from, to;
    if (!wa_source_span(n, &from, &to)) {
        wa_bus_silence(&n->outputs[0], b ? b->channels : 1);
        return;
    }
    if (!b || !b->length) {
        wa_bus_silence(&n->outputs[0], 1);
        return;
    }
    float *data = output_ready(n, 0, b->channels);
    if (!data) return;
    double rate = n->params[0].values[0] * exp2(n->params[1].values[0] / 1200.0);
    if (!isfinite(rate)) rate = 0;
    double step = rate * b->rate / c->rate, duration = (double)b->length / b->rate;
    double loop_start = 0, loop_end = duration;
    if (s->loop && s->loop_start >= 0 && s->loop_end > 0 && s->loop_start < s->loop_end) {
        loop_start = s->loop_start;
        loop_end = fmin(s->loop_end, duration);
    }
    if (!s->loop) s->entered_loop = false;
    double start_frame = loop_start * b->rate, end_frame = loop_end * b->rate;
    size_t start_index = (size_t)ceil(start_frame), end_index = (size_t)ceil(end_frame);
    if (end_index > b->length) end_index = b->length;
    double limit = n->duration >= 0 ? n->duration * b->rate : INFINITY;
    for (unsigned k = from; k < to; k++) {
        if (s->played >= limit) { wa_source_finished(n); break; }
        if (!s->started) {
            double offset = fmin(fmax(n->offset, 0), duration);
            if (s->loop && rate >= 0 && offset >= loop_end) offset = loop_end;
            if (s->loop && rate < 0 && offset < loop_start) offset = loop_start;
            s->position = (offset + (wa_time(c, c->frame + k) - n->start) * rate) * b->rate;
            s->started = true;
        }
        if (s->loop) {
            if (!s->entered_loop) {
                double offset_frame = fmin(fmax(n->offset, 0), duration) * b->rate;
                if (offset_frame < end_frame && s->position >= start_frame) s->entered_loop = true;
                if (offset_frame >= end_frame && s->position < end_frame) s->entered_loop = true;
            }
            if (s->entered_loop && end_frame > start_frame) {
                double span = end_frame - start_frame;
                if (s->position >= end_frame) s->position -= span * floor((s->position - start_frame) / span);
                if (s->position < start_frame) s->position += span * ceil((start_frame - s->position) / span);
            }
        } else if ((step >= 0 && s->position >= (double)b->length) || s->position < 0) {
            if (step != 0 || s->position < 0) { wa_source_finished(n); break; }
        }
        if (s->position >= 0 && s->position < (double)b->length)
            for (unsigned ch = 0; ch < b->channels; ch++)
                data[(size_t)ch * WA_QUANTUM + k] = buffer_at(s, b, ch, s->position, start_index, end_index);
        s->position += step;
        s->played += fabs(step);
    }
}

/* ---- effects ---- */

static void copy_input(struct wa_node *n) {
    struct wa_bus *in = input_bus(n, 0);
    if (in->silent) { wa_bus_silence(&n->outputs[0], in->channels); return; }
    float *o = output_ready(n, 0, in->channels);
    if (o) memcpy(o, in->data, (size_t)in->channels * WA_QUANTUM * sizeof *o);
}

static void gain(struct wa_node *n) {
    struct wa_bus *in = input_bus(n, 0);
    if (in->silent) { wa_bus_silence(&n->outputs[0], in->channels); return; }
    float *o = output_ready(n, 0, in->channels);
    if (!o) return;
    const struct wa_param *g = &n->params[0];
    for (unsigned ch = 0; ch < in->channels; ch++) {
        const float *x = wa_channel(in, ch);
        float *y = o + (size_t)ch * WA_QUANTUM;
        if (g->constant) for (unsigned k = 0; k < WA_QUANTUM; k++) y[k] = x[k] * g->values[0];
        else for (unsigned k = 0; k < WA_QUANTUM; k++) y[k] = x[k] * g->values[k];
    }
}

static bool delay_channels(struct delay *s, unsigned channels) {
    if (channels <= s->channels) return true;
    float *line = realloc(s->line, (size_t)channels * s->length * sizeof *line);
    if (!line) return false;
    memset(line + (size_t)s->channels * s->length, 0, (size_t)(channels - s->channels) * s->length * sizeof *line);
    s->line = line;
    s->channels = channels;
    return true;
}

/* DelayNode: phase 0 writes this quantum's input and then reads; a cycle's breaker reads in
   phase 1 (its delay at least a quantum, so what it reads was written before) and writes in 2. */
static void delay(struct wa_node *n, int phase) {
    struct delay *s = n->dsp;
    struct wa_context *c = n->ctx;
    struct wa_bus *in = input_bus(n, 0);
    if (phase != 1) {
        if (!in->silent && !delay_channels(s, in->channels)) in->silent = true;
        for (unsigned ch = 0; ch < s->channels; ch++) {
            float *line = s->line + (size_t)ch * s->length;
            const float *x = !in->silent && ch < in->channels ? wa_channel(in, ch) : NULL;
            for (unsigned k = 0; k < WA_QUANTUM; k++) line[(s->at + k) % s->length] = x ? x[k] : 0;
        }
    }
    if (phase != 2) {
        float *o = output_ready(n, 0, s->channels);
        if (o) {
            const struct wa_param *d = &n->params[0];
            double least = n->breaker ? (double)WA_QUANTUM / c->rate : 0, longest = 0;
            for (unsigned k = 0; k < WA_QUANTUM; k++) {
                double seconds = fmin(fmax(param_at(d, k), least), s->max);
                longest = fmax(longest, seconds);
                double position = (double)(s->at + k) - seconds * c->rate;
                double whole = floor(position), f = position - whole;
                size_t i = (size_t)(((int64_t)whole % (int64_t)s->length + (int64_t)s->length) % (int64_t)s->length);
                size_t j = (i + 1) % s->length;
                for (unsigned ch = 0; ch < s->channels; ch++) {
                    const float *line = s->line + (size_t)ch * s->length;
                    o[(size_t)ch * WA_QUANTUM + k] = (float)(line[i] + (line[j] - line[i]) * f);
                }
            }
            s->last = longest;
        }
    }
    if (phase != 1) s->at = (s->at + WA_QUANTUM) % s->length;
}

/* The biquad's coefficients (Web Audio 1.13.6) for frequency as a fraction of the Nyquist
   frequency, with the limits the formulas reach at its ends and at Q of 0. */
struct biquad_coefficients { double b0, b1, b2, a1, a2; };
static struct biquad_coefficients wire(double gain) { return (struct biquad_coefficients){gain, 0, 0, 0, 0}; }

static struct biquad_coefficients biquad_coefficients(unsigned type, double frequency, double q, double gain) {
    struct biquad_coefficients k = wire(1);
    double A = pow(10, gain / 40), w0 = M_PI * frequency, cw = cos(w0), sw = sin(w0);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
    case 0: case 1: { /* lowpass, highpass: Q in dB */
        if (frequency >= 1) return wire(type == 0);
        if (type == 1 && frequency <= 0) return k;
        double alpha = sw / (2 * pow(10, q / 20)), beta = type == 0 ? (1 - cw) / 2 : (1 + cw) / 2;
        b0 = beta; b1 = type == 0 ? 2 * beta : -2 * beta; b2 = beta;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    }
    case 3: case 4: { /* lowshelf, highshelf: Q not used, S = 1 */
        if (frequency >= 1) return type == 3 ? wire(A * A) : k;
        if (frequency <= 0) return type == 3 ? k : wire(A * A);
        double alpha = sw / 2 * sqrt(2), k2 = 2 * sqrt(A) * alpha, p = A + 1, m = A - 1;
        if (type == 3) {
            b0 = A * (p - m * cw + k2); b1 = 2 * A * (m - p * cw); b2 = A * (p - m * cw - k2);
            a0 = p + m * cw + k2; a1 = -2 * (m + p * cw); a2 = p + m * cw - k2;
        } else {
            b0 = A * (p + m * cw + k2); b1 = -2 * A * (m + p * cw); b2 = A * (p + m * cw - k2);
            a0 = p - m * cw + k2; a1 = 2 * (m - p * cw); a2 = p - m * cw - k2;
        }
        break;
    }
    default: { /* bandpass, peaking, notch, allpass */
        if (frequency <= 0 || frequency >= 1) return type == 2 ? wire(0) : k;
        if (q <= 0)
            return wire(type == 2 ? 1 : type == 5 ? A * A : type == 6 ? 0 : -1);
        double alpha = sw / (2 * q);
        a1 = -2 * cw;
        if (type == 2) { b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a2 = 1 - alpha; }
        else if (type == 5) { b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a2 = 1 - alpha / A; }
        else if (type == 6) { b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a2 = 1 - alpha; }
        else { b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a2 = 1 - alpha; }
        break;
    }
    }
    return (struct biquad_coefficients){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

static struct biquad_coefficients biquad_at(struct wa_node *n, double frequency, double detune, double q, double gain) {
    struct biquad *s = n->dsp;
    double nyquist = n->ctx->rate / 2.0;
    double f = frequency * exp2(detune / 1200) / nyquist;
    f = isnan(f) ? 0 : fmin(fmax(f, 0), 1);
    return biquad_coefficients(s->type, f, q, gain);
}

/* Frames until the filter's impulse response falls 120 dB, from its poles' radius. */
static uint64_t pole_tail(double radius, double rate) {
    if (radius <= 0) return WA_QUANTUM;
    if (radius >= 1) return (uint64_t)(30 * rate);
    double frames = log(1e-6) / log(radius);
    return frames > 30 * rate ? (uint64_t)(30 * rate) : (uint64_t)frames + WA_QUANTUM;
}

static bool filter_channels(double (**z)[4], unsigned *have, unsigned channels) {
    if (channels <= *have) return true;
    double (*more)[4] = realloc(*z, channels * sizeof *more);
    if (!more) return false;
    memset(more + *have, 0, (channels - *have) * sizeof *more);
    *z = more;
    *have = channels;
    return true;
}

static void biquad(struct wa_node *n) {
    struct biquad *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    unsigned channels = in->channels;
    float *o = filter_channels(&s->z, &s->channels, channels) ? output_ready(n, 0, channels) : NULL;
    if (!o) { wa_bus_silence(&n->outputs[0], 1); return; }
    const struct wa_param *fr = &n->params[0], *dt = &n->params[1], *q = &n->params[2], *g = &n->params[3];
    bool steady = fr->constant && dt->constant && q->constant && g->constant;
    struct biquad_coefficients k = biquad_at(n, fr->values[0], dt->values[0], q->values[0], g->values[0]);
    double disc = k.a1 * k.a1 - 4 * k.a2;
    double radius = disc < 0 ? sqrt(k.a2) : fmax(fabs((-k.a1 + sqrt(disc)) / 2), fabs((-k.a1 - sqrt(disc)) / 2));
    s->tail = pole_tail(radius, n->ctx->rate);
    for (unsigned ch = 0; ch < channels; ch++) {
        const float *x = wa_channel(in, ch);
        float *y = o + (size_t)ch * WA_QUANTUM;
        double x1 = s->z[ch][0], x2 = s->z[ch][1], y1 = s->z[ch][2], y2 = s->z[ch][3];
        for (unsigned i = 0; i < WA_QUANTUM; i++) {
            if (!steady) k = biquad_at(n, fr->values[i], dt->values[i], q->values[i], g->values[i]);
            double v = k.b0 * x[i] + k.b1 * x1 + k.b2 * x2 - k.a1 * y1 - k.a2 * y2;
            x2 = x1; x1 = x[i];
            y2 = y1; y1 = v;
            y[i] = (float)v;
        }
        /* flush what has decayed below anything audible, so the filter never runs on denormals */
        if (fabs(y1) < 1e-30 && fabs(y2) < 1e-30) y1 = y2 = 0;
        s->z[ch][0] = x1; s->z[ch][1] = x2; s->z[ch][2] = y1; s->z[ch][3] = y2;
    }
}

static bool iir_channels(struct iir *s, unsigned channels) {
    if (channels <= s->channels) return true;
    double *x = realloc(s->x, (size_t)channels * IIR_RING * sizeof *x);
    if (!x) return false;
    s->x = x;
    double *y = realloc(s->y, (size_t)channels * IIR_RING * sizeof *y);
    if (!y) return false;
    s->y = y;
    memset(x + (size_t)s->channels * IIR_RING, 0, (size_t)(channels - s->channels) * IIR_RING * sizeof *x);
    memset(y + (size_t)s->channels * IIR_RING, 0, (size_t)(channels - s->channels) * IIR_RING * sizeof *y);
    s->channels = channels;
    return true;
}

static void iir(struct wa_node *n) {
    struct iir *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    unsigned channels = in->channels;
    float *o = iir_channels(s, channels) ? output_ready(n, 0, channels) : NULL;
    if (!o) { wa_bus_silence(&n->outputs[0], 1); return; }
    unsigned at = s->at;
    for (unsigned ch = 0; ch < channels; ch++) {
        const float *x = wa_channel(in, ch);
        float *y = o + (size_t)ch * WA_QUANTUM;
        double *xs = s->x + (size_t)ch * IIR_RING, *ys = s->y + (size_t)ch * IIR_RING;
        at = s->at;
        for (unsigned i = 0; i < WA_QUANTUM; i++) {
            xs[at] = x[i];
            double v = 0;
            for (unsigned k = 0; k < s->nb; k++) v += s->b[k] * xs[(at - k) & (IIR_RING - 1)];
            for (unsigned k = 1; k < s->na; k++) v -= s->a[k] * ys[(at - k) & (IIR_RING - 1)];
            if (fabs(v) < 1e-30) v = 0;
            ys[at] = v;
            y[i] = (float)v;
            at = (at + 1) & (IIR_RING - 1);
        }
    }
    s->at = at;
}

/* Frames until the impulse response stays below -120 dB of its peak, up to 10 seconds. */
static uint64_t iir_tail(const struct iir *s, double rate) {
    double x[IIR_RING] = {0}, y[IIR_RING] = {0}, peak = 0;
    uint64_t last = 0, limit = (uint64_t)(10 * rate);
    unsigned at = 0;
    for (uint64_t i = 0; i < limit; i++) {
        x[at] = i == 0;
        double v = 0;
        for (unsigned k = 0; k < s->nb; k++) v += s->b[k] * x[(at - k) & (IIR_RING - 1)];
        for (unsigned k = 1; k < s->na; k++) v -= s->a[k] * y[(at - k) & (IIR_RING - 1)];
        y[at] = v;
        at = (at + 1) & (IIR_RING - 1);
        if (!isfinite(v)) return limit;
        peak = fmax(peak, fabs(v));
        if (fabs(v) > peak * 1e-6) last = i;
        else if (i - last > (uint64_t)rate / 10) break;
    }
    return last + WA_QUANTUM;
}

/* DynamicsCompressorNode (Web Audio 1.19.5): the compression curve in dB is the identity up to
   the threshold, a quadratic knee to threshold + knee whose slope runs from 1 to 1/ratio, then
   the line of slope 1/ratio. */
static double curve_db(double x, double threshold, double knee, double ratio) {
    if (x <= threshold) return x;
    if (knee > 0 && x < threshold + knee) return x + (1 / ratio - 1) * (x - threshold) * (x - threshold) / (2 * knee);
    return threshold + knee + (1 / ratio - 1) * knee / 2 + (x - threshold - knee) / ratio;
}

static bool compressor_channels(struct compressor *s, unsigned channels) {
    if (channels <= s->channels) return true;
    float *line = realloc(s->line, (size_t)channels * s->lag * sizeof *line);
    if (!line) return false;
    memset(line + (size_t)s->channels * s->lag, 0, (size_t)(channels - s->channels) * s->lag * sizeof *line);
    s->line = line;
    s->channels = channels;
    return true;
}

static void compressor(struct wa_node *n) {
    struct compressor *s = n->dsp;
    struct wa_context *c = n->ctx;
    struct wa_bus *in = input_bus(n, 0);
    unsigned channels = in->channels;
    float *o = compressor_channels(s, channels) ? output_ready(n, 0, channels) : NULL;
    if (!o) { wa_bus_silence(&n->outputs[0], 1); return; }
    double threshold = n->params[0].values[0], knee = n->params[1].values[0], ratio = n->params[2].values[0];
    double attack = n->params[3].values[0] * c->rate, release = n->params[4].values[0] * c->rate;
    double makeup = pow(1 / pow(10, curve_db(0, threshold, knee, ratio) / 20), 0.6);
    /* attacking, the gain moves towards the detector by this fraction of the way each frame;
       releasing, it rises by 10 dB over the release time */
    double attack_rate = attack > 0 ? 1 - exp(-1 / attack) : 1;
    double release_rate = release > 0 ? pow(10, 0.5 / release) : INFINITY;
    double detector = s->detector, gain = s->gain, reduction = gain * makeup;
    for (unsigned k = 0; k < WA_QUANTUM; k++) {
        double level = 0;
        for (unsigned ch = 0; ch < channels; ch++) level = fmax(level, fabs(wa_channel(in, ch)[k]));
        double attenuation = 1;
        if (level >= 0.0001) attenuation = pow(10, curve_db(20 * log10(level), threshold, knee, ratio) / 20) / level;
        bool releasing = attenuation > gain;
        /* the detector follows faster the less it compresses */
        detector += (attenuation - detector) * (0.25 + 0.75 * attenuation);
        if (detector > 1) detector = 1;
        if (releasing) gain = fmin(gain * release_rate, 1);
        else gain += (detector - gain) * attack_rate;
        reduction = gain * makeup;
        for (unsigned ch = 0; ch < channels; ch++) {
            float *line = s->line + (size_t)ch * s->lag;
            float delayed = line[s->at];
            line[s->at] = wa_channel(in, ch)[k];
            o[(size_t)ch * WA_QUANTUM + k] = (float)(delayed * reduction);
        }
        s->at = (s->at + 1) % s->lag;
    }
    s->detector = detector;
    s->gain = gain;
    s->reduction = reduction > 0 ? (float)(20 * log10(reduction)) : -1000;
}

/* StereoPannerNode (Web Audio 1.31.3). */
static void stereo_gains(double pan, bool mono, double *left, double *right) {
    double x = mono ? (pan + 1) / 2 : pan <= 0 ? pan + 1 : pan;
    *left = cos(x * M_PI / 2);
    *right = sin(x * M_PI / 2);
}

static void stereo_pan(const float *l, const float *r, unsigned k, double pan, double gl, double gr, float *ol, float *or) {
    if (!r) { ol[k] = (float)(l[k] * gl); or[k] = (float)(l[k] * gr); }
    else if (pan <= 0) { ol[k] = (float)(l[k] + r[k] * gl); or[k] = (float)(r[k] * gr); }
    else { ol[k] = (float)(l[k] * gl); or[k] = (float)(r[k] + l[k] * gr); }
}

static void stereo_panner(struct wa_node *n) {
    struct wa_bus *in = input_bus(n, 0);
    float *o = output_ready(n, 0, 2);
    if (!o) return;
    const float *l = wa_channel(in, 0), *r = in->channels > 1 ? wa_channel(in, 1) : NULL;
    const struct wa_param *p = &n->params[0];
    double gl = 0, gr = 0;
    if (p->constant) stereo_gains(p->values[0], !r, &gl, &gr);
    for (unsigned k = 0; k < WA_QUANTUM; k++) {
        double pan = param_at(p, k);
        if (!p->constant) stereo_gains(pan, !r, &gl, &gr);
        stereo_pan(l, r, k, pan, gl, gr, o, o + WA_QUANTUM);
    }
}

/* ---- PannerNode (Web Audio 1.24, 6) ---- */

struct vec { double x, y, z; };
static struct vec vsub(struct vec a, struct vec b) { return (struct vec){a.x - b.x, a.y - b.y, a.z - b.z}; }
static struct vec vscale(double s, struct vec a) { return (struct vec){s * a.x, s * a.y, s * a.z}; }
static double vdot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static struct vec vcross(struct vec a, struct vec b) { return (struct vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static double vlength(struct vec a) { return sqrt(vdot(a, a)); }
static struct vec vnormal(struct vec a) { double l = vlength(a); return l > 0 ? vscale(1 / l, a) : a; }
static bool vzero(struct vec a) { return a.x == 0 && a.y == 0 && a.z == 0; }

struct placement { struct vec source, orientation, listener, forward, up; };

static struct placement placement_at(struct wa_node *n, unsigned k) {
    struct wa_param *p = n->params, *l = n->ctx->listener->params;
    return (struct placement){
        {param_at(&p[0], k), param_at(&p[1], k), param_at(&p[2], k)},
        {param_at(&p[3], k), param_at(&p[4], k), param_at(&p[5], k)},
        {param_at(&l[0], k), param_at(&l[1], k), param_at(&l[2], k)},
        {param_at(&l[3], k), param_at(&l[4], k), param_at(&l[5], k)},
        {param_at(&l[6], k), param_at(&l[7], k), param_at(&l[8], k)},
    };
}

/* The source's azimuth and elevation in degrees as the listener hears it (Web Audio 6.2). */
static void azimuth_elevation(const struct placement *p, double *azimuth, double *elevation) {
    struct vec d = vsub(p->source, p->listener);
    if (vzero(d)) { *azimuth = *elevation = 0; return; }
    d = vnormal(d);
    struct vec right = vnormal(vcross(p->forward, p->up)), forward = vnormal(p->forward);
    struct vec up = vcross(right, forward);
    struct vec projected = vnormal(vsub(d, vscale(vdot(d, up), up)));
    double a = 180 / M_PI * acos(fmin(fmax(vdot(projected, right), -1), 1));
    if (vdot(projected, forward) < 0) a = 360 - a;
    *azimuth = a >= 0 && a <= 270 ? 90 - a : 450 - a;
    double e = 90 - 180 / M_PI * acos(fmin(fmax(vdot(d, up), -1), 1));
    *elevation = e > 90 ? 180 - e : e < -90 ? -180 - e : e;
}

/* The distance model's gain and the cone's (Web Audio 6.4, 6.5). */
static double distance_gain(const struct panner *s, const struct placement *p) {
    double d = vlength(vsub(p->source, p->listener));
    switch (s->distance_model) {
    case 0: { /* linear */
        double low = fmin(s->ref, s->max), high = fmax(s->ref, s->max), f = fmin(fmax(s->rolloff, 0), 1);
        if (low == high) return 1 - f;
        return 1 - f * (fmin(fmax(d, low), high) - low) / (high - low);
    }
    case 1: { /* inverse */
        double r = fmax(d, s->ref);
        return r == 0 ? 1 : s->ref / (s->ref + s->rolloff * (r - s->ref));
    }
    default: { /* exponential */
        double r = fmax(d, s->ref);
        return r == 0 ? 1 : pow(r / s->ref, -s->rolloff);
    }
    }
}

static double cone_gain(const struct panner *s, const struct placement *p) {
    if (vzero(p->orientation) || (s->inner == 360 && s->outer == 360)) return 1;
    struct vec to_listener = vnormal(vsub(p->listener, p->source));
    double angle = fabs(180 / M_PI * acos(fmin(fmax(vdot(to_listener, vnormal(p->orientation)), -1), 1)));
    double inner = fabs(s->inner) / 2, outer = fabs(s->outer) / 2;
    if (angle <= inner) return 1;
    if (angle >= outer) return s->outer_gain;
    double x = (angle - inner) / (outer - inner);
    return (1 - x) + s->outer_gain * x;
}

/* Equal-power panning's gains for an azimuth (Web Audio 6.3). */
static void equal_power(double azimuth, bool mono, double *left, double *right) {
    azimuth = fmin(fmax(azimuth, -180), 180);
    if (azimuth < -90) azimuth = -180 - azimuth;
    else if (azimuth > 90) azimuth = 180 - azimuth;
    double x = mono ? (azimuth + 90) / 180 : azimuth <= 0 ? (azimuth + 90) / 90 : azimuth / 90;
    *left = cos(x * M_PI / 2);
    *right = sin(x * M_PI / 2);
}

/* "HRTF" without measured responses: a spherical head of radius 8.75 cm, the interaural
   time difference by Woodworth's formula and each ear's head shadow by Brown and Duda's
   one-pole, one-zero filter, whose high frequencies rise towards the ear facing the source
   and fall behind the head. */
#define HEAD_RADIUS 0.0875
#define SOUND_SPEED 343.0

static void hrtf(struct wa_node *n, double azimuth, double elevation, double level, float *ol, float *or) {
    struct panner *s = n->dsp;
    struct wa_context *c = n->ctx;
    struct wa_bus *in = input_bus(n, 0);
    double lateral = asin(fmin(fmax(sin(azimuth * M_PI / 180) * cos(elevation * M_PI / 180), -1), 1));
    double itd = HEAD_RADIUS / SOUND_SPEED * (fabs(lateral) + sin(fabs(lateral))) * c->rate;
    double w0 = SOUND_SPEED / HEAD_RADIUS, K = c->rate / w0;
    double scale = level * M_SQRT1_2;
    for (int ear = 0; ear < 2; ear++) {
        /* the angle between the source and this ear's axis (the right ear's points right) */
        double theta = ear ? M_PI / 2 - lateral : M_PI / 2 + lateral;
        double alpha = 1.05 + 0.95 * cos(theta / (5 * M_PI / 6) * M_PI);
        double b0 = (1 + alpha * K) / (1 + K), b1 = (1 - alpha * K) / (1 + K), a1 = (1 - K) / (1 + K);
        double lag = (ear == 0) == (lateral > 0) ? itd : 0;  /* the far ear hears it later */
        float *out = ear ? or : ol;
        double *z = s->shadow[ear];
        for (unsigned k = 0; k < WA_QUANTUM; k++) {
            unsigned at = (s->at + k) & 63;
            if (ear == 0) {
                float v = wa_channel(in, 0)[k];
                if (in->channels > 1) v = (v + wa_channel(in, 1)[k]) / 2;
                s->ears[0][at] = s->ears[1][at] = v;
            }
            double position = (double)(s->at + k) - lag;
            double whole = floor(position), f = position - whole;
            unsigned i = (unsigned)((int64_t)whole & 63), j = (i + 1) & 63;
            double x = s->ears[ear][i] + (s->ears[ear][j] - s->ears[ear][i]) * f;
            double y = b0 * x + b1 * z[0] - a1 * z[1];
            if (fabs(y) < 1e-30) y = 0;
            z[0] = x;
            z[1] = y;
            out[k] = (float)(y * scale);
        }
    }
    s->at = (s->at + WA_QUANTUM) & 63;
}

static void panner(struct wa_node *n) {
    struct panner *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    float *o = output_ready(n, 0, 2);
    if (!o) return;
    bool steady = true;
    for (unsigned i = 0; i < n->nparams; i++) steady &= n->params[i].constant;
    for (unsigned i = 0; i < n->ctx->listener->nparams; i++) steady &= n->ctx->listener->params[i].constant;
    struct placement p = placement_at(n, 0);
    double azimuth, elevation;
    azimuth_elevation(&p, &azimuth, &elevation);
    double level = distance_gain(s, &p) * cone_gain(s, &p);
    if (s->model == 1) {      /* HRTF follows the placement once a quantum */
        hrtf(n, azimuth, elevation, level, o, o + WA_QUANTUM);
        return;
    }
    const float *l = wa_channel(in, 0), *r = in->channels > 1 ? wa_channel(in, 1) : NULL;
    double gl, gr;
    equal_power(azimuth, !r, &gl, &gr);
    for (unsigned k = 0; k < WA_QUANTUM; k++) {
        if (!steady && k > 0) {
            p = placement_at(n, k);
            azimuth_elevation(&p, &azimuth, &elevation);
            level = distance_gain(s, &p) * cone_gain(s, &p);
            equal_power(azimuth, !r, &gl, &gr);
        }
        stereo_pan(l, r, k, azimuth, gl, gr, o, o + WA_QUANTUM);
        o[k] = (float)(o[k] * level);
        o[WA_QUANTUM + k] = (float)(o[WA_QUANTUM + k] * level);
    }
}

/* ---- ConvolverNode (Web Audio 1.16) ---- */

static void partitioned_free(struct partitioned *p) {
    free(p->ir_re);
    free(p->ir_im);
    free(p->fdl_re);
    free(p->fdl_im);
    free(p->window);
    free(p->acc_re);
    free(p->acc_im);
    free(p->time);
    free(p->re);
    free(p->im);
    memset(p, 0, sizeof *p);
}

/* The stage for the response's frames [from, to), in blocks of block frames, scaled. */
static bool partitioned_make(struct partitioned *p, const struct wa_buffer *b, size_t from, size_t to, unsigned block,
                             double scale) {
    p->block = block;
    p->bins = block + 1;
    p->parts = (unsigned)((to - from + block - 1) / block);
    p->head = 0;
    size_t spectra = (size_t)p->parts * p->bins;
    p->ir_re = calloc(spectra * b->channels, sizeof(float));
    p->ir_im = calloc(spectra * b->channels, sizeof(float));
    p->fdl_re = calloc(spectra * 2, sizeof(float));
    p->fdl_im = calloc(spectra * 2, sizeof(float));
    p->acc_re = malloc(p->bins * sizeof(float));
    p->acc_im = malloc(p->bins * sizeof(float));
    p->window = calloc((size_t)4 * block, sizeof(double));
    p->time = malloc((size_t)2 * block * sizeof(double));
    p->re = malloc(p->bins * sizeof(double));
    p->im = malloc(p->bins * sizeof(double));
    if (!p->ir_re || !p->ir_im || !p->fdl_re || !p->fdl_im || !p->window || !p->acc_re || !p->acc_im || !p->time ||
        !p->re || !p->im || !fft_ready(&p->fft, 2 * block)) {
        partitioned_free(p);
        return false;
    }
    for (unsigned ch = 0; ch < b->channels; ch++)
        for (unsigned part = 0; part < p->parts; part++) {
            memset(p->time, 0, (size_t)2 * block * sizeof(double));
            for (unsigned k = 0; k < block; k++) {
                size_t at = from + (size_t)part * block + k;
                if (at < to) p->time[k] = b->data[ch][at] * scale;
            }
            size_t spectrum = ((size_t)ch * p->parts + part) * p->bins;
            rfft(&p->fft, p->time, p->re, p->im);
            for (unsigned k = 0; k < p->bins; k++) {
                p->ir_re[spectrum + k] = (float)p->re[k];
                p->ir_im[spectrum + k] = (float)p->im[k];
            }
        }
    return true;
}

/* The stage's next block: a block of each of the two inputs in, a block of each of the two
   outputs out, routed as Web Audio 1.16.4 draws them: output o is input o through response
   channel o (or 0 when it has one), or for a true stereo response of four channels, input 0
   through channel o plus input 1 through channel 2 + o. */
static void partitioned_run(struct partitioned *p, unsigned response_channels, const double *input, double *out) {
    unsigned B = p->block;
    for (unsigned i = 0; i < 2; i++) {
        double *w = p->window + (size_t)2 * B * i;
        memmove(w, w + B, B * sizeof *w);
        memcpy(w + B, input + (size_t)i * B, B * sizeof *w);
        size_t spectrum = ((size_t)i * p->parts + p->head) * p->bins;
        rfft(&p->fft, w, p->re, p->im);
        for (unsigned k = 0; k < p->bins; k++) {
            p->fdl_re[spectrum + k] = (float)p->re[k];
            p->fdl_im[spectrum + k] = (float)p->im[k];
        }
    }
    for (unsigned o = 0; o < 2; o++) {
        memset(p->acc_re, 0, p->bins * sizeof *p->acc_re);
        memset(p->acc_im, 0, p->bins * sizeof *p->acc_im);
        for (unsigned route = 0; route < (response_channels == 4 ? 2u : 1u); route++) {
            unsigned i = response_channels == 4 ? route : o;
            unsigned r = response_channels == 4 ? 2 * route + o : response_channels > 1 ? o : 0;
            for (unsigned part = 0; part < p->parts; part++) {
                unsigned slot = (p->head + p->parts - part) % p->parts;
                const float *xr = p->fdl_re + ((size_t)i * p->parts + slot) * p->bins;
                const float *xi = p->fdl_im + ((size_t)i * p->parts + slot) * p->bins;
                const float *hr = p->ir_re + ((size_t)r * p->parts + part) * p->bins;
                const float *hi = p->ir_im + ((size_t)r * p->parts + part) * p->bins;
                for (unsigned k = 0; k < p->bins; k++) {
                    p->acc_re[k] += xr[k] * hr[k] - xi[k] * hi[k];
                    p->acc_im[k] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
        }
        for (unsigned k = 0; k < p->bins; k++) {
            p->re[k] = p->acc_re[k];
            p->im[k] = p->acc_im[k];
        }
        irfft(&p->fft, p->re, p->im, p->time);
        memcpy(out + (size_t)o * B, p->time + B, B * sizeof *out);
    }
    p->head = (p->head + 1) % p->parts;
}

static void convolver_clear(struct convolver *s) {
    partitioned_free(&s->head);
    partitioned_free(&s->tail);
    free(s->long_in);
    free(s->ring);
    s->long_in = s->long_out = NULL;
    s->ring = NULL;
    s->response_channels = 0;
    s->length = 0;
    s->has_tail = false;
}

/* calculateNormalizationScale (Web Audio 1.16.3) */
static double normalization(const struct wa_buffer *b) {
    double power = 0;
    for (unsigned ch = 0; ch < b->channels; ch++)
        for (size_t k = 0; k < b->length; k++) power += (double)b->data[ch][k] * b->data[ch][k];
    power = sqrt(power / ((double)b->channels * (double)b->length));
    if (!isfinite(power) || power < 0.000125) power = 0.000125;
    double scale = 1 / power * 0.00125;
    if (b->rate) scale *= 44100 / b->rate;
    if (b->channels == 4) scale *= 0.5;
    return scale;
}

static enum wa_error convolver_set(struct wa_node *n, struct wa_buffer *b) {
    struct convolver *s = n->dsp;
    convolver_clear(s);
    if (!b || !b->length) return WA_OK;
    double scale = s->normalize ? normalization(b) : 1;
    s->has_tail = b->length > LONG_BLOCK;
    bool ok = partitioned_make(&s->head, b, 0, MIN(b->length, (size_t)LONG_BLOCK), WA_QUANTUM, scale);
    if (ok && s->has_tail) {
        ok = partitioned_make(&s->tail, b, LONG_BLOCK, b->length, LONG_BLOCK, scale);
        /* the long stage's input block and output block, then the ring */
        s->long_in = calloc((size_t)4 * LONG_BLOCK, sizeof *s->long_in);
        s->ring = calloc((size_t)2 * 2 * LONG_BLOCK, sizeof *s->ring);
        ok = ok && s->long_in && s->ring;
        if (s->long_in) s->long_out = s->long_in + 2 * LONG_BLOCK;
    }
    if (!ok) {
        convolver_clear(s);
        return WA_NO_MEMORY;
    }
    s->response_channels = b->channels;
    s->length = b->length;
    s->filled = 0;
    s->frames = 0;
    return WA_OK;
}

static void convolver(struct wa_node *n) {
    struct convolver *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    if (!s->response_channels) { wa_bus_silence(&n->outputs[0], 1); return; }
    unsigned outputs = in->channels == 1 && s->response_channels == 1 ? 1 : 2;
    float *o = output_ready(n, 0, outputs);
    if (!o) return;
    double input[2 * WA_QUANTUM], head[2 * WA_QUANTUM];
    for (unsigned i = 0; i < 2; i++) {
        const float *x = wa_channel(in, in->channels > 1 ? i : 0);
        for (unsigned k = 0; k < WA_QUANTUM; k++) input[i * WA_QUANTUM + k] = x[k];
    }
    partitioned_run(&s->head, s->response_channels, input, head);
    size_t ring = 2 * LONG_BLOCK, at = (size_t)(s->frames % ring);
    for (unsigned ch = 0; ch < 2; ch++)
        for (unsigned k = 0; k < WA_QUANTUM; k++) {
            double v = head[ch * WA_QUANTUM + k];
            if (s->has_tail) {
                float *r = s->ring + ch * ring + at + k;
                v += *r;
                *r = 0;
            }
            if (ch < outputs) o[(size_t)ch * WA_QUANTUM + k] = (float)v;
        }
    if (s->has_tail) {
        for (unsigned i = 0; i < 2; i++)
            memcpy(s->long_in + (size_t)i * LONG_BLOCK + s->filled, input + i * WA_QUANTUM, WA_QUANTUM * sizeof *input);
        s->filled += WA_QUANTUM;
        if (s->filled == LONG_BLOCK) {
            /* the block's output is due from the next quantum: LONG_BLOCK frames after the
               block began, which is where this stage's part of the response starts */
            partitioned_run(&s->tail, s->response_channels, s->long_in, s->long_out);
            size_t next = (size_t)((s->frames + WA_QUANTUM) % ring);
            for (unsigned ch = 0; ch < 2; ch++)
                for (unsigned k = 0; k < LONG_BLOCK; k++)
                    s->ring[ch * ring + (next + k) % ring] += (float)s->long_out[ch * LONG_BLOCK + k];
            s->filled = 0;
        }
    }
    s->frames += WA_QUANTUM;
}

/* ---- WaveShaperNode (Web Audio 1.32) ---- */

/* The half-band lowpass the 2x stages up- and down-sample with: a Blackman-windowed sinc.
   Its centre tap is what makes the others sum to 1, added last, so a constant passes the
   down-sampler exactly. */
static float halfband[OVERSAMPLE_TAPS];
static bool halfband_made;

static void halfband_make(void) {
    if (__atomic_load_n(&halfband_made, __ATOMIC_ACQUIRE)) return;
    const int c = OVERSAMPLE_TAPS / 2;
    float h[OVERSAMPLE_TAPS];
    for (int j = 0; j < OVERSAMPLE_TAPS; j++) {
        double x = (j - c) / 2.0, w = 0.42 - 0.5 * cos(2 * M_PI * j / (OVERSAMPLE_TAPS - 1)) + 0.08 * cos(4 * M_PI * j / (OVERSAMPLE_TAPS - 1));
        h[j] = j == c ? 0 : (float)(0.5 * sin(M_PI * x) / (M_PI * x) * w);
    }
    float others = 0;
    for (int j = 0; j < OVERSAMPLE_TAPS; j++) if (j != c) others += h[j];
    h[c] = 1 - others;
    memcpy(halfband, h, sizeof h);
    __atomic_store_n(&halfband_made, true, __ATOMIC_RELEASE);
}

#define HISTORY (OVERSAMPLE_TAPS - 1)

/* n samples to 2n: zeros between them, then the lowpass at twice its gain. history holds
   the stuffed stream's last HISTORY samples. */
static void upsample(const float *in, size_t n, float *history, float *out) {
    float z[HISTORY + 4 * WA_QUANTUM];
    memcpy(z, history, HISTORY * sizeof *z);
    for (size_t k = 0; k < n; k++) {
        z[HISTORY + 2 * k] = in[k];
        z[HISTORY + 2 * k + 1] = 0;
    }
    for (size_t m = 0; m < 2 * n; m++) {
        float v = 0;
        for (int j = 0; j < OVERSAMPLE_TAPS; j++) v += 2 * halfband[j] * z[HISTORY + m - j];
        out[m] = v;
    }
    memcpy(history, z + 2 * n, HISTORY * sizeof *z);
}

/* 2n samples to n: the lowpass, then every other sample. */
static void downsample(const float *in, size_t n, float *history, float *out) {
    float z[HISTORY + 4 * WA_QUANTUM];
    const int c = OVERSAMPLE_TAPS / 2;
    memcpy(z, history, HISTORY * sizeof *z);
    memcpy(z + HISTORY, in, 2 * n * sizeof *z);
    for (size_t k = 0; k < n; k++) {
        float v = 0;
        for (int j = 0; j < OVERSAMPLE_TAPS; j++) if (j != c) v += halfband[j] * z[HISTORY + 2 * k + 1 - j];
        out[k] = v + halfband[c] * z[HISTORY + 2 * k + 1 - c];
    }
    memcpy(history, z + 2 * n, HISTORY * sizeof *z);
}

/* The curve at x (Web Audio 1.32.4). */
static float shape(const struct waveshaper *s, float x) {
    double v = (s->length - 1) / 2.0 * (x + 1.0);
    if (!(v > 0)) return s->curve[0];
    if (v >= s->length - 1) return s->curve[s->length - 1];
    size_t k = (size_t)v;
    double f = v - (double)k;
    return (float)((1 - f) * s->curve[k] + f * s->curve[k + 1]);
}

/* The four filters' histories: the up-samplers' as if the input had been silent, the
   down-samplers' as the curve made of that silence. */
static void shaper_reset(struct waveshaper *s, unsigned from) {
    float rest = s->curve ? shape(s, 0) : 0;
    for (unsigned ch = from; ch < s->channels; ch++) {
        float *h = s->history + (size_t)ch * 4 * HISTORY;
        memset(h, 0, 2 * HISTORY * sizeof *h);
        for (unsigned k = 2 * HISTORY; k < 4 * HISTORY; k++) h[k] = rest;
    }
}

static bool shaper_channels(struct waveshaper *s, unsigned channels) {
    if (channels <= s->channels) return true;
    float *history = realloc(s->history, (size_t)channels * 4 * HISTORY * sizeof *history);
    if (!history) return false;
    s->history = history;
    unsigned had = s->channels;
    s->channels = channels;
    shaper_reset(s, had);
    return true;
}

static void waveshaper(struct wa_node *n) {
    struct waveshaper *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    if (!s->curve) { copy_input(n); return; }
    unsigned channels = in->channels;
    float *o = shaper_channels(s, channels) ? output_ready(n, 0, channels) : NULL;
    if (!o) { wa_bus_silence(&n->outputs[0], 1); return; }
    for (unsigned ch = 0; ch < channels; ch++) {
        const float *x = wa_channel(in, ch);
        float *y = o + (size_t)ch * WA_QUANTUM, *h = s->history + (size_t)ch * 4 * HISTORY;
        if (!s->oversample) {
            for (unsigned k = 0; k < WA_QUANTUM; k++) y[k] = shape(s, x[k]);
            continue;
        }
        float a[4 * WA_QUANTUM], b[4 * WA_QUANTUM];
        size_t frames = WA_QUANTUM;
        upsample(x, frames, h, a);
        frames *= 2;
        if (s->oversample == 2) {
            upsample(a, frames, h + HISTORY, b);
            frames *= 2;
            memcpy(a, b, frames * sizeof *a);
        }
        for (size_t k = 0; k < frames; k++) a[k] = shape(s, a[k]);
        if (s->oversample == 2) {
            downsample(a, frames / 2, h + 3 * HISTORY, b);
            frames /= 2;
            memcpy(a, b, frames * sizeof *a);
        }
        downsample(a, frames / 2, h + 2 * HISTORY, y);
    }
}

/* ---- AnalyserNode (Web Audio 1.8) ---- */

static bool analyser_size(struct analyser *s, unsigned size) {
    struct fft f;
    double *window = malloc(size * sizeof *window), *work = malloc(size * sizeof *work), *re = malloc((size / 2 + 1) * sizeof *re);
    double *im = malloc((size / 2 + 1) * sizeof *im), *smoothed = calloc(size / 2, sizeof *smoothed);
    if (!window || !work || !re || !im || !smoothed || !fft_ready(&f, size)) {
        free(window); free(work); free(re); free(im); free(smoothed);
        return false;
    }
    free(s->window); free(s->work); free(s->re); free(s->im); free(s->smoothed);
    /* the Blackman window */
    for (unsigned k = 0; k < size; k++)
        window[k] = 0.42 - 0.5 * cos(2 * M_PI * k / size) + 0.08 * cos(4 * M_PI * k / size);
    s->window = window;
    s->work = work;
    s->re = re;
    s->im = im;
    s->smoothed = smoothed;
    s->fft = f;
    s->fft_size = size;
    s->analysed = 0;
    return true;
}

static void analyser(struct wa_node *n) {
    struct analyser *s = n->dsp;
    struct wa_bus *in = input_bus(n, 0);
    /* the window takes the input mixed to mono as speakers, whatever the node's channels */
    if (wa_bus_ready(&s->mono, 1)) {
        wa_mix_into(&s->mono, in, WA_SPEAKERS);
        const float *m = wa_channel(&s->mono, 0);
        for (unsigned k = 0; k < WA_QUANTUM; k++) s->ring[(s->at + k) % ANALYSER_RING] = m[k];
    }
    s->at = (s->at + WA_QUANTUM) % ANALYSER_RING;
    copy_input(n);
}

/* The frequency data: once a quantum, the window's transform smoothed with the last. */
static void analyse_frequencies(struct wa_node *n) {
    struct analyser *s = n->dsp;
    if (s->analysed == n->ctx->frame + 1) return;
    s->analysed = n->ctx->frame + 1;
    unsigned N = s->fft_size;
    for (unsigned k = 0; k < N; k++) s->work[k] = s->ring[(s->at + ANALYSER_RING - N + k) % ANALYSER_RING] * s->window[k];
    rfft(&s->fft, s->work, s->re, s->im);
    for (unsigned k = 0; k < N / 2; k++) {
        double magnitude = hypot(s->re[k], s->im[k]) / N;
        double v = s->smoothing * s->smoothed[k] + (1 - s->smoothing) * magnitude;
        s->smoothed[k] = isfinite(v) ? v : 0;
    }
}

static enum wa_error analyse(struct wa_node *n, int kind, void *out, size_t len) {
    struct analyser *s = n->dsp;
    unsigned N = s->fft_size;
    if (kind <= 1) {
        analyse_frequencies(n);
        size_t count = MIN(len, (size_t)N / 2);
        for (size_t k = 0; k < count; k++) {
            double db = 20 * log10(s->smoothed[k]);
            if (kind == 0) ((float *)out)[k] = (float)db;
            else {
                double b = floor(255 / (s->max_db - s->min_db) * (db - s->min_db));
                ((uint8_t *)out)[k] = (uint8_t)(b < 0 || isnan(b) ? 0 : b > 255 ? 255 : b);
            }
        }
        return WA_OK;
    }
    size_t count = MIN(len, (size_t)N);
    for (size_t k = 0; k < count; k++) {
        float v = s->ring[(s->at + ANALYSER_RING - N + k) % ANALYSER_RING];
        if (kind == 2) ((float *)out)[k] = v;
        else {
            double b = floor(128 * (1 + (double)v));
            ((uint8_t *)out)[k] = (uint8_t)(b < 0 || isnan(b) ? 0 : b > 255 ? 255 : b);
        }
    }
    return WA_OK;
}

/* ---- channel routing ---- */

static void splitter(struct wa_node *n) {
    struct wa_bus *in = input_bus(n, 0);
    for (unsigned i = 0; i < n->noutputs; i++) {
        if (in->silent || i >= in->channels) { wa_bus_silence(&n->outputs[i], 1); continue; }
        float *o = output_ready(n, i, 1);
        if (o) memcpy(o, wa_channel(in, i), WA_QUANTUM * sizeof *o);
    }
}

static void merger(struct wa_node *n) {
    float *o = output_ready(n, 0, n->ninputs);
    if (!o) return;
    for (unsigned i = 0; i < n->ninputs; i++) {
        struct wa_bus *in = input_bus(n, i);
        if (!in->silent) memcpy(o + (size_t)i * WA_QUANTUM, wa_channel(in, 0), WA_QUANTUM * sizeof *o);
    }
}

/* ---- the kernels' entry points ---- */

enum wa_error wa_dsp_init(struct wa_node *n, const struct wa_node_options *o) {
    struct wa_context *c = n->ctx;
    switch (n->kind) {
    case WA_OSCILLATOR: {
        struct oscillator *s = calloc(1, sizeof *s);
        if (!s || !(s->wave = builtin_wave(0))) { free(s); return WA_NO_MEMORY; }
        n->dsp = s;
        break;
    }
    case WA_BUFFER_SOURCE: n->dsp = calloc(1, sizeof(struct buffer_source)); break;
    case WA_DELAY: {
        struct delay *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        s->max = o && o->max_delay > 0 ? o->max_delay : 1;
        s->length = (size_t)ceil(s->max * c->rate) + 2 * WA_QUANTUM + 2;
        if (!delay_channels(s, 1)) { free(s); return WA_NO_MEMORY; }
        n->dsp = s;
        break;
    }
    case WA_BIQUAD: n->dsp = calloc(1, sizeof(struct biquad)); break;
    case WA_IIR: {
        if (!o || !o->nfeedforward || o->nfeedforward > IIR_MAX || !o->nfeedback || o->nfeedback > IIR_MAX) return WA_NOT_SUPPORTED;
        if (o->feedback[0] == 0) return WA_INVALID_STATE;
        struct iir *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        s->nb = o->nfeedforward;
        s->na = o->nfeedback;
        for (unsigned k = 0; k < s->nb; k++) s->b[k] = o->feedforward[k] / o->feedback[0];
        for (unsigned k = 0; k < s->na; k++) s->a[k] = o->feedback[k] / o->feedback[0];
        s->tail = iir_tail(s, c->rate);
        n->dsp = s;
        break;
    }
    case WA_COMPRESSOR: {
        struct compressor *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        s->lag = (unsigned)lround(0.006 * c->rate);
        if (!s->lag) s->lag = 1;
        s->gain = 1;
        if (!compressor_channels(s, 1)) { free(s); return WA_NO_MEMORY; }
        n->dsp = s;
        break;
    }
    case WA_PANNER: {
        struct panner *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        *s = (struct panner){.distance_model = 1, .ref = 1, .max = 10000, .rolloff = 1, .inner = 360, .outer = 360};
        n->dsp = s;
        break;
    }
    case WA_CONVOLVER: {
        struct convolver *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        s->normalize = true;
        n->dsp = s;
        break;
    }
    case WA_WAVESHAPER: halfband_make(); n->dsp = calloc(1, sizeof(struct waveshaper)); break;
    case WA_ANALYSER: {
        struct analyser *s = calloc(1, sizeof *s);
        if (!s) return WA_NO_MEMORY;
        s->smoothing = 0.8;
        s->min_db = -100;
        s->max_db = -30;
        if (!analyser_size(s, 2048)) { free(s); return WA_NO_MEMORY; }
        n->dsp = s;
        break;
    }
    default: return WA_OK;
    }
    return n->dsp ? WA_OK : WA_NO_MEMORY;
}

void wa_dsp_free(struct wa_node *n) {
    void *dsp = n->dsp;
    if (!dsp) return;
    switch (n->kind) {
    case WA_OSCILLATOR: wa_wave_unref(((struct oscillator *)dsp)->wave); break;
    case WA_BUFFER_SOURCE: wa_buffer_unref(((struct buffer_source *)dsp)->buffer); break;
    case WA_DELAY: free(((struct delay *)dsp)->line); break;
    case WA_BIQUAD: free(((struct biquad *)dsp)->z); break;
    case WA_IIR: free(((struct iir *)dsp)->x); free(((struct iir *)dsp)->y); break;
    case WA_COMPRESSOR: free(((struct compressor *)dsp)->line); break;
    case WA_CONVOLVER: convolver_clear(dsp); break;
    case WA_WAVESHAPER: free(((struct waveshaper *)dsp)->curve); free(((struct waveshaper *)dsp)->history); break;
    case WA_ANALYSER: {
        struct analyser *s = dsp;
        free(s->window); free(s->work); free(s->re); free(s->im); free(s->smoothed); free(s->mono.data);
        break;
    }
    default: break;
    }
    free(dsp);
    n->dsp = NULL;
}

void wa_dsp_process(struct wa_node *n, int phase) {
    switch (n->kind) {
    case WA_DESTINATION: copy_input(n); break;
    case WA_GAIN: gain(n); break;
    case WA_OSCILLATOR: oscillator(n); break;
    case WA_BUFFER_SOURCE: buffer_source(n); break;
    case WA_CONSTANT_SOURCE: constant_source(n); break;
    case WA_DELAY: delay(n, phase); break;
    case WA_BIQUAD: biquad(n); break;
    case WA_IIR: iir(n); break;
    case WA_COMPRESSOR: compressor(n); break;
    case WA_STEREO_PANNER: stereo_panner(n); break;
    case WA_PANNER: panner(n); break;
    case WA_CONVOLVER: convolver(n); break;
    case WA_WAVESHAPER: waveshaper(n); break;
    case WA_ANALYSER: analyser(n); break;
    case WA_SPLITTER: splitter(n); break;
    case WA_MERGER: merger(n); break;
    default: break;
    }
}

uint64_t wa_dsp_tail(struct wa_node *n) {
    double rate = n->ctx->rate;
    switch (n->kind) {
    case WA_DELAY: {
        struct delay *s = n->dsp;
        return (uint64_t)ceil(fmax(s->last, n->params[0].values[0]) * rate) + WA_QUANTUM;
    }
    case WA_BIQUAD: return ((struct biquad *)n->dsp)->tail ? ((struct biquad *)n->dsp)->tail : WA_QUANTUM;
    case WA_IIR: return ((struct iir *)n->dsp)->tail;
    case WA_COMPRESSOR: return ((struct compressor *)n->dsp)->lag + WA_QUANTUM;
    case WA_PANNER: return ((struct panner *)n->dsp)->model ? 64 + WA_QUANTUM : 0;
    case WA_CONVOLVER: return ((struct convolver *)n->dsp)->length + WA_QUANTUM;
    case WA_WAVESHAPER: return ((struct waveshaper *)n->dsp)->oversample ? 2 * OVERSAMPLE_TAPS : 0;
    default: return 0;
    }
}

enum wa_idle wa_dsp_idle(struct wa_node *n) {
    if (n->kind == WA_ANALYSER) return WA_IDLE_LISTEN;
    if (n->kind == WA_WAVESHAPER) {
        struct waveshaper *s = n->dsp;
        return s->curve && shape(s, 0) != 0 ? WA_IDLE_SOUND : WA_IDLE_SILENT;
    }
    return WA_IDLE_SILENT;
}

enum wa_error wa_dsp_set(struct wa_node *n, enum wa_attribute attribute, double value) {
    void *dsp = n->dsp;
    switch (n->kind) {
    case WA_OSCILLATOR:
        if (attribute == WA_TYPE && value >= 0 && value < 4) {
            struct oscillator *s = dsp;
            struct wa_wave *w = builtin_wave((unsigned)value);
            if (!w) return WA_NO_MEMORY;
            wa_wave_unref(s->wave);
            s->wave = w;
            return WA_OK;
        }
        break;
    case WA_BIQUAD:
        if (attribute == WA_TYPE && value >= 0 && value < 8) { ((struct biquad *)dsp)->type = (unsigned)value; return WA_OK; }
        break;
    case WA_BUFFER_SOURCE: {
        struct buffer_source *s = dsp;
        if (attribute == WA_LOOP) { s->loop = value != 0; return WA_OK; }
        if (attribute == WA_LOOP_START) { s->loop_start = value; return WA_OK; }
        if (attribute == WA_LOOP_END) { s->loop_end = value; return WA_OK; }
        break;
    }
    case WA_CONVOLVER:
        if (attribute == WA_NORMALIZE) { ((struct convolver *)dsp)->normalize = value != 0; return WA_OK; }
        break;
    case WA_WAVESHAPER:
        if (attribute == WA_OVERSAMPLE && value >= 0 && value < 3) {
            struct waveshaper *s = dsp;
            s->oversample = (unsigned)value;
            shaper_reset(s, 0);
            return WA_OK;
        }
        break;
    case WA_PANNER: {
        struct panner *s = dsp;
        switch (attribute) {
        case WA_PANNING_MODEL: s->model = value != 0; return WA_OK;
        case WA_DISTANCE_MODEL: s->distance_model = (unsigned)value; return WA_OK;
        case WA_REF_DISTANCE: s->ref = value; return WA_OK;
        case WA_MAX_DISTANCE: s->max = value; return WA_OK;
        case WA_ROLLOFF: s->rolloff = value; return WA_OK;
        case WA_CONE_INNER: s->inner = value; return WA_OK;
        case WA_CONE_OUTER: s->outer = value; return WA_OK;
        case WA_CONE_OUTER_GAIN: s->outer_gain = value; return WA_OK;
        default: break;
        }
        break;
    }
    case WA_ANALYSER: {
        struct analyser *s = dsp;
        switch (attribute) {
        case WA_FFT_SIZE: return analyser_size(s, (unsigned)value) ? WA_OK : WA_NO_MEMORY;
        case WA_SMOOTHING: s->smoothing = value; return WA_OK;
        case WA_MIN_DECIBELS: s->min_db = value; return WA_OK;
        case WA_MAX_DECIBELS: s->max_db = value; return WA_OK;
        default: break;
        }
        break;
    }
    default: break;
    }
    return WA_INVALID_ACCESS;
}

enum wa_error wa_dsp_set_buffer(struct wa_node *n, struct wa_buffer *b) {
    if (n->kind == WA_CONVOLVER) return convolver_set(n, b);
    if (n->kind != WA_BUFFER_SOURCE) return WA_INVALID_ACCESS;
    struct buffer_source *s = n->dsp;
    wa_buffer_unref(s->buffer);
    s->buffer = wa_buffer_ref(b);
    return WA_OK;
}

enum wa_error wa_dsp_set_wave(struct wa_node *n, struct wa_wave *w) {
    if (n->kind != WA_OSCILLATOR || !w) return WA_INVALID_ACCESS;
    struct oscillator *s = n->dsp;
    wave_ref(w);
    wa_wave_unref(s->wave);
    s->wave = w;
    return WA_OK;
}

enum wa_error wa_dsp_set_curve(struct wa_node *n, const float *curve, size_t len) {
    if (n->kind != WA_WAVESHAPER) return WA_INVALID_ACCESS;
    struct waveshaper *s = n->dsp;
    float *copy = NULL;
    if (curve && len) {
        copy = malloc(len * sizeof *copy);
        if (!copy) return WA_NO_MEMORY;
        memcpy(copy, curve, len * sizeof *copy);
    }
    free(s->curve);
    s->curve = copy;
    s->length = copy ? len : 0;
    shaper_reset(s, 0);
    return WA_OK;
}

float wa_dsp_reduction(struct wa_node *n) {
    return n->kind == WA_COMPRESSOR ? ((struct compressor *)n->dsp)->reduction : 0;
}

/* getFrequencyResponse: H(e^jω) at each frequency, from the params' current values. */
enum wa_error wa_dsp_response(struct wa_node *n, const float *hz, float *magnitude, float *phase, size_t len) {
    if (n->kind != WA_BIQUAD && n->kind != WA_IIR) return WA_INVALID_ACCESS;
    double nyquist = n->ctx->rate / 2.0;
    double b[IIR_MAX] = {0}, a[IIR_MAX] = {1};
    unsigned nb, na;
    if (n->kind == WA_BIQUAD) {
        double value[4];
        for (unsigned i = 0; i < 4; i++) {
            const struct wa_param *p = &n->params[i];
            value[i] = fmin(fmax(p->current, p->info.min), p->info.max);
        }
        struct biquad_coefficients k = biquad_at(n, value[0], value[1], value[2], value[3]);
        b[0] = k.b0; b[1] = k.b1; b[2] = k.b2; a[1] = k.a1; a[2] = k.a2;
        nb = na = 3;
    } else {
        struct iir *s = n->dsp;
        memcpy(b, s->b, sizeof b);
        memcpy(a, s->a, sizeof a);
        nb = s->nb;
        na = s->na;
    }
    for (size_t i = 0; i < len; i++) {
        double f = hz[i];
        if (!(f >= 0 && f <= nyquist)) { magnitude[i] = phase[i] = NAN; continue; }
        double w = M_PI * f / nyquist, nr = 0, ni = 0, dr = 0, di = 0;
        for (unsigned k = 0; k < nb; k++) { nr += b[k] * cos(w * k); ni -= b[k] * sin(w * k); }
        for (unsigned k = 0; k < na; k++) { dr += a[k] * cos(w * k); di -= a[k] * sin(w * k); }
        double d = dr * dr + di * di, hr = (nr * dr + ni * di) / d, hi = (ni * dr - nr * di) / d;
        magnitude[i] = (float)hypot(hr, hi);
        phase[i] = (float)atan2(hi, hr);
    }
    return WA_OK;
}

enum wa_error wa_dsp_analyse(struct wa_node *n, int kind, void *out, size_t len) {
    return n->kind == WA_ANALYSER && kind >= 0 && kind < 4 ? analyse(n, kind, out, len) : WA_INVALID_ACCESS;
}

/* ---- decodeAudioData's resampling ---- */

#define RESAMPLE_ZEROS 16        /* zero crossings of the kernel on each side */
#define RESAMPLE_STEPS 512       /* table entries per zero crossing */
#define RESAMPLE_BETA 9.0        /* the Kaiser window's shape */

static float resample_table[RESAMPLE_ZEROS * RESAMPLE_STEPS + 2];
static uint32_t resample_built;  /* 0, 1 building, 2 built */

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

/* sinc(u)·kaiser(u / RESAMPLE_ZEROS) for u in [0, RESAMPLE_ZEROS], once per process. */
static const float *resample_kernel(void) {
    uint32_t expected = 0;
    if (__atomic_load_n(&resample_built, __ATOMIC_ACQUIRE) == 2) return resample_table;
    if (__atomic_compare_exchange_n(&resample_built, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        double norm = bessel_i0(RESAMPLE_BETA);
        for (unsigned i = 0; i < sizeof resample_table / sizeof *resample_table; i++) {
            double u = (double)i / RESAMPLE_STEPS, r = u / RESAMPLE_ZEROS;
            double sinc = u == 0 ? 1 : sin(M_PI * u) / (M_PI * u);
            resample_table[i] = r >= 1 ? 0 : (float)(sinc * bessel_i0(RESAMPLE_BETA * sqrt(1 - r * r)) / norm);
        }
        __atomic_store_n(&resample_built, 2, __ATOMIC_RELEASE);
    } else
        while (__atomic_load_n(&resample_built, __ATOMIC_ACQUIRE) != 2) thrd_yield();
    return resample_table;
}

size_t wa_resampled_length(size_t n, double from, double to) {
    double frames = ceil((double)n * to / from);
    return frames > (double)UINT32_MAX ? 0 : (size_t)frames;
}

void wa_resample(const float *in, size_t n, double from, double to, float *out, size_t first, size_t count) {
    const float *kernel = resample_kernel();
    double ratio = from / to;                  /* input frames per output frame */
    double cutoff = to < from ? to / from : 1; /* the lower rate's Nyquist, in input frames */
    double reach = RESAMPLE_ZEROS / cutoff;    /* the kernel's half-width in input frames */
    double scale = cutoff * RESAMPLE_STEPS;
    for (size_t j = 0; j < count; j++) {
        double t = (double)(first + j) * ratio;
        ptrdiff_t lo = (ptrdiff_t)ceil(t - reach), hi = (ptrdiff_t)floor(t + reach);
        if (lo < 0) lo = 0;
        if (hi > (ptrdiff_t)n - 1) hi = (ptrdiff_t)n - 1;
        double sum = 0;
        for (ptrdiff_t k = lo; k <= hi; k++) {
            double x = fabs(t - (double)k) * scale;
            size_t i = (size_t)x;
            if (i >= RESAMPLE_ZEROS * RESAMPLE_STEPS) continue;
            double f = x - (double)i;
            sum += in[k] * (kernel[i] + (kernel[i + 1] - kernel[i]) * f);
        }
        out[j] = (float)(sum * cutoff);
    }
}
