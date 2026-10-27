/* The Web Audio renderer: buffers, the graph and its order, AudioParam automation, channel
   mixing, the render quantum, and the thread that renders each context (webaudio.h). */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "webaudio_node.h"


/* ---- buffers ---- */

struct wa_buffer *wa_buffer_new(unsigned channels, size_t length, float rate) {
    if (!channels || channels > WA_MAX_CHANNELS || length > SIZE_MAX / sizeof(float) / channels) return NULL;
    float *data = calloc(length ? length * channels : 1, sizeof *data);
    return data ? wa_buffer_adopt(data, channels, length, rate) : NULL;
}

struct wa_buffer *wa_buffer_adopt(float *data, unsigned channels, size_t length, float rate) {
    struct wa_buffer *b = channels && channels <= WA_MAX_CHANNELS ? calloc(1, sizeof *b) : NULL;
    if (!b) { free(data); return NULL; }
    b->refs = 1;
    b->channels = channels;
    b->length = length;
    b->rate = rate;
    for (unsigned c = 0; c < channels; c++) b->data[c] = data + c * length;
    return b;
}

struct wa_buffer *wa_buffer_ref(struct wa_buffer *b) {
    if (b) __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);
    return b;
}

void wa_buffer_unref(struct wa_buffer *b) {
    if (!b || __atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(b->data[0]);
    free(b);
}

struct wa_buffer *wa_buffer_copy(const struct wa_buffer *b) {
    struct wa_buffer *copy = wa_buffer_new(b->channels, b->length, b->rate);
    if (copy && b->length) memcpy(copy->data[0], b->data[0], b->length * b->channels * sizeof(float));
    return copy;
}

/* ---- buses and mixing ---- */

bool wa_bus_ready(struct wa_bus *b, unsigned channels) {
    if (channels > b->capacity) {
        float *data = realloc(b->data, (size_t)channels * WA_QUANTUM * sizeof *data);
        if (!data) return false;
        b->data = data;
        b->capacity = channels;
    }
    b->channels = channels;
    b->silent = false;
    memset(b->data, 0, (size_t)channels * WA_QUANTUM * sizeof *b->data);
    return true;
}

void wa_bus_silence(struct wa_bus *b, unsigned channels) {
    if (!wa_bus_ready(b, channels)) {
        /* With no memory for more channels, the silence keeps the channels it has. */
        if (b->capacity) memset(b->data, 0, (size_t)b->capacity * WA_QUANTUM * sizeof *b->data);
        b->channels = b->capacity ? MIN(channels, b->capacity) : 0;
    }
    b->silent = true;
}

static void add(float *to, const float *from, float gain) {
    for (int i = 0; i < WA_QUANTUM; i++) to[i] += from[i] * gain;
}

/* The speaker layouts' up- and down-mix equations (Web Audio 4.4, 4.5) for 1, 2, 4 and 6
   channels; any other pair mixes discretely. */
void wa_mix_into(struct wa_bus *out, struct wa_bus *in, enum wa_interpretation interpretation) {
    if (in->silent) return;
    unsigned o = out->channels, n = in->channels;
    out->silent = false;
    bool layout = (o == 1 || o == 2 || o == 4 || o == 6) && (n == 1 || n == 2 || n == 4 || n == 6);
    if (interpretation == WA_DISCRETE || o == n || !layout) {
        for (unsigned c = 0; c < MIN(o, n); c++) add(wa_channel(out, c), wa_channel(in, c), 1);
        return;
    }
    const float h = (float)M_SQRT1_2;
#define FROM(c) wa_channel(in, c)
#define TO(c) wa_channel(out, c)
    if (n == 1) { /* mono up */
        if (o == 6) add(TO(2), FROM(0), 1);
        else { add(TO(0), FROM(0), 1); add(TO(1), FROM(0), 1); }
    } else if (n == 2) {
        if (o == 1) { add(TO(0), FROM(0), 0.5f); add(TO(0), FROM(1), 0.5f); }
        else { add(TO(0), FROM(0), 1); add(TO(1), FROM(1), 1); }
    } else if (n == 4) {
        if (o == 1) for (int c = 0; c < 4; c++) add(TO(0), FROM(c), 0.25f);
        else if (o == 2) { add(TO(0), FROM(0), 0.5f); add(TO(0), FROM(2), 0.5f); add(TO(1), FROM(1), 0.5f); add(TO(1), FROM(3), 0.5f); }
        else { add(TO(0), FROM(0), 1); add(TO(1), FROM(1), 1); add(TO(4), FROM(2), 1); add(TO(5), FROM(3), 1); }
    } else { /* 5.1: L R C LFE SL SR */
        if (o == 1) {
            add(TO(0), FROM(0), h); add(TO(0), FROM(1), h); add(TO(0), FROM(2), 1);
            add(TO(0), FROM(4), 0.5f); add(TO(0), FROM(5), 0.5f);
        } else if (o == 2) {
            add(TO(0), FROM(0), 1); add(TO(0), FROM(2), h); add(TO(0), FROM(4), h);
            add(TO(1), FROM(1), 1); add(TO(1), FROM(2), h); add(TO(1), FROM(5), h);
        } else {
            add(TO(0), FROM(0), 1); add(TO(0), FROM(2), h); add(TO(1), FROM(1), 1); add(TO(1), FROM(2), h);
            add(TO(2), FROM(4), 1); add(TO(3), FROM(5), 1);
        }
    }
#undef FROM
#undef TO
}

/* ---- AudioParam descriptions ---- */

unsigned wa_params(enum wa_kind kind, float rate, double max_delay, struct wa_param_info *out, unsigned max) {
    const float big = FLT_MAX, nyquist = rate / 2;
    const float cents = 1200.0f * log2f(FLT_MAX), decibels = 40.0f * log10f(FLT_MAX);
    struct wa_param_info list[WA_MAX_PARAMS];
    unsigned n = 0;
#define P(nm, d, lo, hi, a, fixed) list[n++] = (struct wa_param_info){nm, d, lo, hi, a, fixed}
    switch (kind) {
    case WA_LISTENER:
        P("positionX", 0, -big, big, true, false); P("positionY", 0, -big, big, true, false); P("positionZ", 0, -big, big, true, false);
        P("forwardX", 0, -big, big, true, false); P("forwardY", 0, -big, big, true, false); P("forwardZ", -1, -big, big, true, false);
        P("upX", 0, -big, big, true, false); P("upY", 1, -big, big, true, false); P("upZ", 0, -big, big, true, false);
        break;
    case WA_GAIN: P("gain", 1, -big, big, true, false); break;
    case WA_OSCILLATOR: P("frequency", 440, -nyquist, nyquist, true, false); P("detune", 0, -cents, cents, true, false); break;
    case WA_BUFFER_SOURCE: P("playbackRate", 1, -big, big, false, true); P("detune", 0, -big, big, false, true); break;
    case WA_CONSTANT_SOURCE: P("offset", 1, -big, big, true, false); break;
    case WA_DELAY: P("delayTime", 0, 0, (float)max_delay, true, false); break;
    case WA_BIQUAD:
        P("frequency", 350, 0, nyquist, true, false); P("detune", 0, -cents, cents, true, false);
        P("Q", 1, -big, big, true, false); P("gain", 0, -big, decibels, true, false);
        break;
    case WA_COMPRESSOR:
        P("threshold", -24, -100, 0, false, true); P("knee", 30, 0, 40, false, true); P("ratio", 12, 1, 20, false, true);
        P("attack", 0.003f, 0, 1, false, true); P("release", 0.25f, 0, 1, false, true);
        break;
    case WA_STEREO_PANNER: P("pan", 0, -1, 1, true, false); break;
    case WA_PANNER:
        P("positionX", 0, -big, big, true, false); P("positionY", 0, -big, big, true, false); P("positionZ", 0, -big, big, true, false);
        P("orientationX", 1, -big, big, true, false); P("orientationY", 0, -big, big, true, false); P("orientationZ", 0, -big, big, true, false);
        break;
    default: break;
    }
#undef P
    for (unsigned i = 0; i < n && i < max; i++) out[i] = list[i];
    return n;
}

/* ---- AudioParam automation (Web Audio 1.6) ---- */

static bool ramp_kind(enum wa_automation k) { return k == WA_LINEAR_RAMP || k == WA_EXPONENTIAL_RAMP; }

static double timeline_at(struct wa_param *p, size_t n, double t);

static double curve_at(const struct wa_automation_event *e, double t) {
    size_t last = e->curve_len - 1;
    if (t >= e->time + e->extra) return e->curve[last];
    double x = (t - e->time) * (double)last / e->extra;
    if (x <= 0) return e->curve[0];
    size_t k = (size_t)x;
    if (k >= last) return e->curve[last];
    return e->curve[k] + (e->curve[k + 1] - e->curve[k]) * (x - (double)k);
}

static double target_start(struct wa_param *p, size_t i) {
    struct wa_automation_event *e = &p->events[i];
    if (!e->v0_known) {
        e->v0 = timeline_at(p, i, e->time);
        e->v0_known = true;
    }
    return e->v0;
}

/* The value event i gives at time t, at or after its own time. */
static double event_value(struct wa_param *p, size_t i, double t) {
    struct wa_automation_event *e = &p->events[i];
    switch (e->kind) {
    case WA_SET_TARGET: {
        double v0 = target_start(p, i);
        return e->extra <= 0 ? e->value : e->value + (v0 - e->value) * exp(-(t - e->time) / e->extra);
    }
    case WA_SET_CURVE: return curve_at(e, t < e->end ? t : e->end);
    default: return e->value;
    }
}

/* A ramp e from value v0 at t0 to its own value at its time, at time t. */
static double ramp_at(const struct wa_automation_event *e, double t0, double v0, double t) {
    double t1 = e->time, v1 = e->value;
    if (t >= t1 || t1 <= t0) return v1;
    double f = (t - t0) / (t1 - t0);
    if (e->kind == WA_LINEAR_RAMP) return v0 + (v1 - v0) * f;
    if (v0 == 0 || (v0 < 0) != (v1 < 0)) return v0;
    return v0 * pow(v1 / v0, f);
}

/* The last of the first n events at or before t, or -1. */
static ptrdiff_t event_before(const struct wa_param *p, size_t n, double t) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (p->events[mid].time <= t) lo = mid + 1;
        else hi = mid;
    }
    return (ptrdiff_t)lo - 1;
}

/* The intrinsic value at time t that the first n events give. */
static double timeline_with(struct wa_param *p, size_t n, double t, ptrdiff_t i) {
    if (i + 1 < (ptrdiff_t)n && ramp_kind(p->events[i + 1].kind)) {
        double t0 = 0, v0 = p->base;
        if (i >= 0) {
            t0 = p->events[i].time;
            v0 = p->events[i].kind == WA_SET_TARGET ? target_start(p, (size_t)i)
               : p->events[i].kind == WA_SET_CURVE ? curve_at(&p->events[i], t0) : p->events[i].value;
        }
        return ramp_at(&p->events[i + 1], t0, v0, t);
    }
    return i < 0 ? p->base : event_value(p, (size_t)i, t);
}

static double timeline_at(struct wa_param *p, size_t n, double t) {
    return timeline_with(p, n, t, event_before(p, n, t));
}

static void forget_starts(struct wa_param *p, size_t from) {
    for (size_t i = from; i < p->nevents; i++) p->events[i].v0_known = false;
}

static void drop_event(struct wa_param *p, size_t i) {
    free(p->events[i].curve);
    memmove(p->events + i, p->events + i + 1, (p->nevents - i - 1) * sizeof *p->events);
    p->nevents--;
}

/* Inserts e after the events at or before its time; false when memory runs out. */
static bool insert_event(struct wa_param *p, const struct wa_automation_event *e) {
    if (p->nevents == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 4;
        struct wa_automation_event *events = realloc(p->events, capacity * sizeof *events);
        if (!events) return false;
        p->events = events;
        p->capacity = capacity;
    }
    size_t at = (size_t)(event_before(p, p->nevents, e->time) + 1);
    memmove(p->events + at + 1, p->events + at, (p->nevents - at) * sizeof *p->events);
    p->events[at] = *e;
    p->nevents++;
    forget_starts(p, at);
    return true;
}

static bool inside_curve(const struct wa_param *p, double t) {
    for (size_t i = 0; i < p->nevents; i++) {
        const struct wa_automation_event *e = &p->events[i];
        if (e->kind == WA_SET_CURVE && t >= e->time && t < e->time + e->extra) return true;
    }
    return false;
}

static enum wa_error automate(struct wa_param *p, double now, enum wa_automation kind, double time, double value,
                              double extra, const float *curve, size_t curve_len) {
    struct wa_automation_event e = {.kind = kind, .time = time, .value = value, .extra = extra, .end = INFINITY};
    if (kind == WA_CANCEL) {
        /* the events at or after time, and a value curve still running at time */
        for (size_t i = p->nevents; i-- > 0;) {
            const struct wa_automation_event *x = &p->events[i];
            if (x->time >= time || (x->kind == WA_SET_CURVE && time < x->time + x->extra)) drop_event(p, i);
        }
        return WA_OK;
    }
    if (kind == WA_CANCEL_AND_HOLD) {
        /* The held value is a computed value of the param, so a float like the values
           an author passes (a ramp scheduled after it starts from the same number as
           one after setValueAtTime(that value)). */
        ptrdiff_t e1 = event_before(p, p->nevents, time);
        size_t e2 = (size_t)(e1 + 1);
        if (e2 < p->nevents && ramp_kind(p->events[e2].kind)) {
            double held = (float)timeline_with(p, p->nevents, time, e1);
            p->events[e2].time = time;
            p->events[e2].value = held;
            while (p->nevents > e2 + 1) drop_event(p, p->nevents - 1);
            return WA_OK;
        }
        if (e1 >= 0 && p->events[e1].kind == WA_SET_TARGET) {
            struct wa_automation_event hold = {.kind = WA_SET_VALUE, .time = time, .value = (float)event_value(p, (size_t)e1, time), .end = INFINITY};
            while (p->nevents > (size_t)e1 + 1) drop_event(p, p->nevents - 1);
            return insert_event(p, &hold) ? WA_OK : WA_NO_MEMORY;
        }
        if (e1 >= 0 && p->events[e1].kind == WA_SET_CURVE && time <= p->events[e1].time + p->events[e1].extra) {
            /* Cut to tc - t3: a curve cut at its own start has no length and leaves the
               value it would have begun from. */
            struct wa_automation_event *curve = &p->events[e1];
            bool begun = time > curve->time;
            double held = begun ? curve_at(curve, time) : timeline_at(p, (size_t)e1, time);
            struct wa_automation_event hold = {.kind = WA_SET_VALUE, .time = time, .value = (float)held, .end = INFINITY};
            size_t keep = begun ? (size_t)e1 + 1 : (size_t)e1;
            if (begun) curve->end = time;
            while (p->nevents > keep) drop_event(p, p->nevents - 1);
            return insert_event(p, &hold) ? WA_OK : WA_NO_MEMORY;
        }
        while (p->nevents > (size_t)(e1 + 1)) drop_event(p, p->nevents - 1);
        return WA_OK;
    }
    if (inside_curve(p, time)) return WA_NOT_SUPPORTED;
    if (kind == WA_SET_CURVE) {
        for (size_t i = 0; i < p->nevents; i++)
            if (p->events[i].time > time && p->events[i].time < time + extra) return WA_NOT_SUPPORTED;
        e.curve = malloc(curve_len * sizeof *e.curve);
        if (!e.curve) return WA_NO_MEMORY;
        memcpy(e.curve, curve, curve_len * sizeof *e.curve);
        e.curve_len = curve_len;
        struct wa_automation_event end = {.kind = WA_SET_VALUE, .time = time + extra, .value = curve[curve_len - 1], .end = INFINITY};
        if (!insert_event(p, &e)) { free(e.curve); return WA_NO_MEMORY; }
        /* the implicit setValueAtTime at the curve's end, so what follows starts from it */
        return insert_event(p, &end) ? WA_OK : WA_NO_MEMORY;
    }
    if (ramp_kind(kind)) {
        ptrdiff_t before = event_before(p, p->nevents, time);
        if (before < 0) {
            /* no event before: as if setValueAtTime(the current value, currentTime) */
            struct wa_automation_event start = {.kind = WA_SET_VALUE, .time = now, .value = p->current, .end = INFINITY};
            if (!insert_event(p, &start)) return WA_NO_MEMORY;
        } else if (p->events[before].kind == WA_SET_TARGET) {
            /* A ramp after a setTarget starts where the setTarget is: at its start if it has
               not started, else at the current time; the ramp replaces it. */
            struct wa_automation_event *t = &p->events[before];
            double t0 = t->time <= now ? now : t->time;
            double v0 = t->time <= now ? event_value(p, (size_t)before, now) : target_start(p, (size_t)before);
            *t = (struct wa_automation_event){.kind = WA_SET_VALUE, .time = t0, .value = v0, .end = INFINITY};
            forget_starts(p, (size_t)before);
        }
    }
    return insert_event(p, &e) ? WA_OK : WA_NO_MEMORY;
}

/* Whether the intrinsic value holds still through [t0, t1), event i being the last at or
   before t0. */
static bool steady(const struct wa_param *p, ptrdiff_t i, double t0, double t1) {
    if (i + 1 < (ptrdiff_t)p->nevents) {
        const struct wa_automation_event *next = &p->events[i + 1];
        if (next->time < t1 || ramp_kind(next->kind)) return false;
    }
    if (i < 0) return true;
    const struct wa_automation_event *e = &p->events[i];
    if (e->kind == WA_SET_TARGET) return e->extra <= 0;
    if (e->kind == WA_SET_CURVE) return t0 >= e->end || t0 >= e->time + e->extra;
    return true;
}

/* The param's values for the quantum at frame: its intrinsic value, plus its inputs (already
   mixed into p->input, mono), clamped to its nominal range. */
static void compute_param(struct wa_context *c, struct wa_param *p) {
    double t0 = wa_time(c, c->frame);
    unsigned n = p->a_rate ? WA_QUANTUM : 1;
    ptrdiff_t i = event_before(p, p->nevents, t0);
    bool flat = steady(p, i, t0, wa_time(c, c->frame + n));
    if (flat) {
        double v = timeline_with(p, p->nevents, t0, i);
        for (unsigned k = 0; k < n; k++) p->values[k] = (float)v;
    } else {
        for (unsigned k = 0; k < n; k++) {
            double t = wa_time(c, c->frame + k);
            while (i + 1 < (ptrdiff_t)p->nevents && p->events[i + 1].time <= t) i++;
            p->values[k] = (float)timeline_with(p, p->nevents, t, i);
        }
    }
    p->current = p->values[0];
    p->constant = flat;
    if (!p->input.silent) {
        const float *in = wa_channel(&p->input, 0);
        for (unsigned k = 0; k < n; k++) p->values[k] += in[k];
        p->constant = false;
    }
    for (unsigned k = 0; k < n; k++) {
        float v = p->values[k];
        if (isnan(v)) v = p->info.default_value;
        p->values[k] = v < p->info.min ? p->info.min : v > p->info.max ? p->info.max : v;
    }
    if (!p->a_rate) {
        for (unsigned k = 1; k < WA_QUANTUM; k++) p->values[k] = p->values[0];
        p->constant = true;
    }
    /* The events wholly in the past are no longer needed: what follows starts from the
       last of them. A setTarget keeps the value it started from. */
    double end = wa_time(c, c->frame + WA_QUANTUM);
    ptrdiff_t last = event_before(p, p->nevents, end);
    if (last > 0) {
        if (p->events[last].kind == WA_SET_TARGET) target_start(p, (size_t)last);
        for (ptrdiff_t k = 0; k < last; k++) free(p->events[k].curve);
        memmove(p->events, p->events + last, (p->nevents - (size_t)last) * sizeof *p->events);
        p->nevents -= (size_t)last;
    }
}

/* ---- nodes ---- */

struct wa_node *wa_node_at(struct wa_context *c, uint32_t id) {
    return id && id < c->nnodes ? c->nodes[id] : NULL;
}

static void unlink_conn(struct wa_conn *k) {
    struct wa_node *from = k->from, *to = k->to;
    if (k->prev_out) k->prev_out->next_out = k->next_out; else from->outs = k->next_out;
    if (k->next_out) k->next_out->prev_out = k->prev_out;
    struct wa_conn **head = k->param >= 0 ? &to->params[k->param].inputs : &to->inputs[k->input].conns;
    if (k->prev_in) k->prev_in->next_in = k->next_in; else *head = k->next_in;
    if (k->next_in) k->next_in->prev_in = k->prev_in;
    free(k);
}

static void free_node(struct wa_context *c, struct wa_node *n) {
    while (n->outs) unlink_conn(n->outs);
    for (unsigned i = 0; i < n->ninputs; i++) {
        while (n->inputs[i].conns) unlink_conn(n->inputs[i].conns);
        free(n->inputs[i].bus.data);
    }
    for (unsigned i = 0; i < n->nparams; i++) {
        struct wa_param *p = &n->params[i];
        while (p->inputs) unlink_conn(p->inputs);
        for (size_t k = 0; k < p->nevents; k++) free(p->events[k].curve);
        free(p->events);
        free(p->input.data);
    }
    for (unsigned i = 0; i < n->noutputs; i++) free(n->outputs[i].data);
    wa_dsp_free(n);
    free(n->inputs);
    free(n->outputs);
    if (n->id < c->nnodes) c->nodes[n->id] = NULL;
    c->order_dirty = true;
    free(n);
}

static const struct { unsigned inputs, outputs, channels; enum wa_mode mode; enum wa_interpretation interpretation; } shapes[WA_KINDS] = {
    [WA_DESTINATION] = {1, 1, 2, WA_MODE_EXPLICIT, WA_SPEAKERS},
    [WA_LISTENER] = {0, 0, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_GAIN] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_OSCILLATOR] = {0, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_BUFFER_SOURCE] = {0, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_CONSTANT_SOURCE] = {0, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_DELAY] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_BIQUAD] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_IIR] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_COMPRESSOR] = {1, 1, 2, WA_MODE_CLAMPED_MAX, WA_SPEAKERS},
    [WA_STEREO_PANNER] = {1, 1, 2, WA_MODE_CLAMPED_MAX, WA_SPEAKERS},
    [WA_PANNER] = {1, 1, 2, WA_MODE_CLAMPED_MAX, WA_SPEAKERS},
    [WA_CONVOLVER] = {1, 1, 2, WA_MODE_CLAMPED_MAX, WA_SPEAKERS},
    [WA_WAVESHAPER] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_ANALYSER] = {1, 1, 2, WA_MODE_MAX, WA_SPEAKERS},
    [WA_SPLITTER] = {1, 6, 6, WA_MODE_EXPLICIT, WA_DISCRETE},
    [WA_MERGER] = {6, 1, 1, WA_MODE_EXPLICIT, WA_SPEAKERS},
};

/* The channel settings kind allows (Web Audio 1.5.4 and each node's channel limitations). */
static enum wa_error channels_allowed(const struct wa_node *n, unsigned count, enum wa_mode mode, enum wa_interpretation interpretation) {
    if (n->kind == WA_DESTINATION) {
        if (n->ctx->length) return count != n->channel_count ? WA_INVALID_STATE : WA_OK;
        return !count ? WA_NOT_SUPPORTED : count > WA_OUTPUT_CHANNELS ? WA_INDEX_SIZE : WA_OK;
    }
    if (!count || count > WA_MAX_CHANNELS) return WA_NOT_SUPPORTED;
    switch (n->kind) {
    case WA_COMPRESSOR: case WA_STEREO_PANNER: case WA_PANNER: case WA_CONVOLVER:
        if (count > 2 || mode == WA_MODE_MAX) return WA_NOT_SUPPORTED;
        break;
    case WA_SPLITTER:
        if (count != n->channel_count || mode != WA_MODE_EXPLICIT || interpretation != WA_DISCRETE) return WA_INVALID_STATE;
        break;
    case WA_MERGER:
        if (count != 1 || mode != WA_MODE_EXPLICIT) return WA_INVALID_STATE;
        break;
    default: break;
    }
    return WA_OK;
}

static uint32_t node_slot(struct wa_context *c) {
    for (uint32_t i = 1; i < c->nnodes; i++)
        if (!c->nodes[i]) return i;
    if (c->nnodes + 1 >= c->nodes_capacity) {
        uint32_t capacity = c->nodes_capacity ? c->nodes_capacity * 2 : 16;
        struct wa_node **nodes = realloc(c->nodes, capacity * sizeof *nodes);
        if (!nodes) return 0;
        c->nodes = nodes;
        c->nodes_capacity = capacity;
    }
    if (!c->nnodes) c->nodes[c->nnodes++] = NULL; /* id 0 is none */
    c->nodes[c->nnodes] = NULL;
    return c->nnodes++;
}

static struct wa_node *make_node(struct wa_context *c, enum wa_kind kind, const struct wa_node_options *o, enum wa_error *error) {
    *error = WA_NO_MEMORY;
    uint32_t id = node_slot(c);
    struct wa_node *n = id ? calloc(1, sizeof *n) : NULL;
    if (!n) return NULL;
    n->ctx = c;
    n->id = id;
    n->kind = kind;
    n->ninputs = shapes[kind].inputs;
    n->noutputs = shapes[kind].outputs;
    n->channel_count = shapes[kind].channels;
    n->mode = shapes[kind].mode;
    n->interpretation = shapes[kind].interpretation;
    n->page_holds = true;
    n->stop = INFINITY;
    n->duration = -1;
    if (kind == WA_SPLITTER) n->noutputs = n->channel_count = o && o->outputs ? o->outputs : 6;
    if (kind == WA_MERGER) n->ninputs = o && o->inputs ? o->inputs : 6;
    if (kind == WA_DESTINATION) n->channel_count = c->channels;
    if (n->noutputs > WA_MAX_CHANNELS || n->ninputs > WA_MAX_CHANNELS) {
        *error = WA_INDEX_SIZE;
        free(n);
        return NULL;
    }
    if (o) {
        unsigned count = o->channel_count ? o->channel_count : n->channel_count;
        enum wa_mode mode = o->mode >= 0 ? (enum wa_mode)o->mode : n->mode;
        enum wa_interpretation interpretation = o->interpretation >= 0 ? (enum wa_interpretation)o->interpretation : n->interpretation;
        enum wa_error allowed = channels_allowed(n, count, mode, interpretation);
        if (allowed) { *error = allowed; free(n); return NULL; }
        n->channel_count = count;
        n->mode = mode;
        n->interpretation = interpretation;
    }
    n->inputs = calloc(n->ninputs ? n->ninputs : 1, sizeof *n->inputs);
    n->outputs = calloc(n->noutputs ? n->noutputs : 1, sizeof *n->outputs);
    if (!n->inputs || !n->outputs) { free(n->inputs); free(n->outputs); free(n); return NULL; }
    struct wa_param_info info[WA_MAX_PARAMS];
    n->nparams = wa_params(kind, c->rate, o && o->max_delay > 0 ? o->max_delay : 1, info, WA_MAX_PARAMS);
    for (unsigned i = 0; i < n->nparams; i++) {
        struct wa_param *p = &n->params[i];
        p->info = info[i];
        p->a_rate = info[i].a_rate;
        p->base = p->current = info[i].default_value;
        p->input.silent = true;
    }
    for (unsigned i = 0; i < n->ninputs; i++) n->inputs[i].bus.silent = true;
    for (unsigned i = 0; i < n->noutputs; i++) n->outputs[i].silent = true;
    enum wa_error dsp = wa_dsp_init(n, o);
    if (dsp) {
        *error = dsp;
        free(n->inputs);
        free(n->outputs);
        free(n);
        return NULL;
    }
    c->nodes[id] = n;
    c->order_dirty = true;
    *error = WA_OK;
    return n;
}

uint32_t wa_node_new(struct wa_context *c, enum wa_kind kind, const struct wa_node_options *o, enum wa_error *error) {
    enum wa_error ignored;
    if (!error) error = &ignored;
    if (kind <= WA_LISTENER || kind >= WA_KINDS) { *error = WA_NOT_SUPPORTED; return 0; }
    mtx_lock(&c->lock);
    struct wa_node *n = make_node(c, kind, o, error);
    mtx_unlock(&c->lock);
    return n ? n->id : 0;
}

void wa_node_release(struct wa_context *c, uint32_t id) {
    mtx_lock(&c->lock);
    struct wa_node *n = wa_node_at(c, id);
    if (n && n != c->destination && n != c->listener) n->page_holds = false;
    mtx_unlock(&c->lock);
}

/* A node the page let go of goes when nothing it does can be heard any more: no
   connection comes in, it is not a source still to play, its tail is over, and it makes no
   sound of its own. */
static bool reclaimable(struct wa_node *n) {
    if (n->page_holds || n->tail_left || n->active || wa_dsp_idle(n) == WA_IDLE_SOUND) return false;
    if (n->play == WA_SCHEDULED || n->play == WA_PLAYING) return false;
    for (unsigned i = 0; i < n->ninputs; i++) if (n->inputs[i].conns) return false;
    for (unsigned i = 0; i < n->nparams; i++) if (n->params[i].inputs) return false;
    return true;
}

static void reclaim(struct wa_context *c) {
    for (bool again = true; again;) {
        again = false;
        for (uint32_t i = 1; i < c->nnodes; i++) {
            struct wa_node *n = c->nodes[i];
            if (n && reclaimable(n)) { free_node(c, n); again = true; }
        }
    }
}

/* ---- connections ---- */

static bool connected(const struct wa_node *from, unsigned output, const struct wa_node *to, int input, int param) {
    for (const struct wa_conn *k = from->outs; k; k = k->next_out)
        if (k->to == to && k->output == output && k->input == input && k->param == param) return true;
    return false;
}

static enum wa_error link(struct wa_context *c, struct wa_node *from, unsigned output, struct wa_node *to, int input, int param) {
    if (connected(from, output, to, input, param)) return WA_OK;
    struct wa_conn *k = calloc(1, sizeof *k);
    if (!k) return WA_NO_MEMORY;
    *k = (struct wa_conn){.from = from, .to = to, .output = output, .input = input, .param = param};
    k->next_out = from->outs;
    if (from->outs) from->outs->prev_out = k;
    from->outs = k;
    struct wa_conn **head = param >= 0 ? &to->params[param].inputs : &to->inputs[input].conns;
    k->next_in = *head;
    if (*head) (*head)->prev_in = k;
    *head = k;
    c->order_dirty = true;
    return WA_OK;
}

enum wa_error wa_connect(struct wa_context *c, uint32_t from_id, unsigned output, uint32_t to_id, unsigned input) {
    mtx_lock(&c->lock);
    struct wa_node *from = wa_node_at(c, from_id), *to = wa_node_at(c, to_id);
    enum wa_error e = !from || !to ? WA_INVALID_ACCESS : output >= from->noutputs || input >= to->ninputs ? WA_INDEX_SIZE
                    : link(c, from, output, to, (int)input, -1);
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_connect_param(struct wa_context *c, uint32_t from_id, unsigned output, uint32_t to_id, unsigned param) {
    mtx_lock(&c->lock);
    struct wa_node *from = wa_node_at(c, from_id), *to = wa_node_at(c, to_id);
    enum wa_error e = !from || !to || param >= to->nparams ? WA_INVALID_ACCESS : output >= from->noutputs ? WA_INDEX_SIZE
                    : link(c, from, output, to, -1, (int)param);
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_disconnect(struct wa_context *c, uint32_t from_id, int output, uint32_t to_id, int input, int param) {
    mtx_lock(&c->lock);
    struct wa_node *from = wa_node_at(c, from_id), *to = to_id ? wa_node_at(c, to_id) : NULL;
    enum wa_error e = WA_OK;
    if (!from || (to_id && !to)) e = WA_INVALID_ACCESS;
    else if (output >= (int)from->noutputs) e = WA_INDEX_SIZE;
    else if (to && param < 0 && input >= (int)to->ninputs) e = WA_INDEX_SIZE;
    else {
        bool found = false;
        for (struct wa_conn *k = from->outs, *next; k; k = next) {
            next = k->next_out;
            if (output >= 0 && k->output != (unsigned)output) continue;
            if (to && k->to != to) continue;
            if (to && param >= 0 && k->param != param) continue;
            if (to && param < 0 && input >= 0 && k->input != input) continue;
            if (to && param < 0 && input < 0 && k->param >= 0 && output >= 0) continue; /* (node, output) names node inputs only */
            unlink_conn(k);
            found = true;
        }
        if (!found && to) e = WA_INVALID_ACCESS;
        c->order_dirty = true;
    }
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_set_channels(struct wa_context *c, uint32_t id, unsigned count, enum wa_mode mode, enum wa_interpretation interpretation) {
    mtx_lock(&c->lock);
    struct wa_node *n = wa_node_at(c, id);
    enum wa_error e = n ? channels_allowed(n, count, mode, interpretation) : WA_INVALID_ACCESS;
    if (!e) {
        n->channel_count = count;
        n->mode = mode;
        n->interpretation = interpretation;
    }
    mtx_unlock(&c->lock);
    return e;
}

/* ---- scheduled sources ---- */

enum wa_error wa_start(struct wa_context *c, uint32_t id, double when, double offset, double duration) {
    mtx_lock(&c->lock);
    struct wa_node *n = wa_node_at(c, id);
    enum wa_error e = WA_OK;
    if (!n) e = WA_INVALID_ACCESS;
    else if (n->play != WA_UNSCHEDULED) e = WA_INVALID_STATE;
    else {
        n->play = WA_SCHEDULED;
        n->start = when;
        n->offset = offset;
        n->duration = duration;
    }
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_stop(struct wa_context *c, uint32_t id, double when) {
    mtx_lock(&c->lock);
    struct wa_node *n = wa_node_at(c, id);
    enum wa_error e = WA_OK;
    if (!n) e = WA_INVALID_ACCESS;
    else if (n->play == WA_UNSCHEDULED) e = WA_INVALID_STATE;
    else if (n->play != WA_FINISHED) n->stop = when;
    mtx_unlock(&c->lock);
    return e;
}

bool wa_source_span(struct wa_node *n, unsigned *from, unsigned *to) {
    struct wa_context *c = n->ctx;
    if (n->play != WA_SCHEDULED && n->play != WA_PLAYING) return false;
    double rate = c->rate;
    /* frames whose time t has start <= t < stop */
    double first = ceil(n->start * rate - (double)c->frame), last = ceil(n->stop * rate - (double)c->frame);
    if (first < 0) first = 0;
    if (last > WA_QUANTUM) last = WA_QUANTUM;
    if (first >= WA_QUANTUM || last <= first) {
        if (last <= 0 || (n->stop <= n->start && first < WA_QUANTUM)) wa_source_finished(n);
        return false;
    }
    n->play = WA_PLAYING;
    *from = (unsigned)first;
    *to = (unsigned)last;
    if (last < WA_QUANTUM) wa_source_finished(n);
    return true;
}

static void post(struct wa_context *c, struct wa_event e) {
    if (c->nevents == c->events_capacity) {
        size_t capacity = c->events_capacity ? c->events_capacity * 2 : 16;
        struct wa_event *events = realloc(c->events, capacity * sizeof *events);
        if (!events) { wa_buffer_unref(e.buffer); return; }
        c->events = events;
        c->events_capacity = capacity;
    }
    c->events[c->nevents++] = e;
    __atomic_add_fetch(&c->posted, 1, __ATOMIC_RELEASE);
    wake_address(&c->posted, 0);
}

void wa_source_finished(struct wa_node *n) {
    if (n->play == WA_FINISHED) return;
    n->play = WA_FINISHED;
    if (!n->ended_posted) {
        n->ended_posted = true;
        post(n->ctx, (struct wa_event){.kind = WA_EVENT_ENDED, .node = n->id});
    }
}

/* ---- the order of processing (Web Audio 2.6) ---- */

struct order_state { struct wa_context *c; int counter; struct wa_node **stack; size_t depth; };

/* The edges into n: the sources of its inputs (none for a cycle-breaking delay's read
   half) and of its params. */
static void strong(struct order_state *s, struct wa_node *n, bool cut);

static void visit_edge(struct order_state *s, struct wa_node *n, struct wa_node *from, bool cut) {
    if (from->index < 0) {
        strong(s, from, cut);
        if (from->low < n->low) n->low = from->low;
    } else if (from->on_stack && from->index < n->low) n->low = from->index;
}

static void strong(struct order_state *s, struct wa_node *n, bool cut) {
    n->index = n->low = s->counter++;
    s->stack[s->depth++] = n;
    n->on_stack = true;
    if (!(cut && n->breaker))
        for (unsigned i = 0; i < n->ninputs; i++)
            for (struct wa_conn *k = n->inputs[i].conns; k; k = k->next_in) visit_edge(s, n, k->from, cut);
    for (unsigned i = 0; i < n->nparams; i++)
        for (struct wa_conn *k = n->params[i].inputs; k; k = k->next_in) visit_edge(s, n, k->from, cut);
    if (n->low != n->index) return;
    /* n roots a strongly connected component: a cycle if it has more than one node or an
       edge to itself */
    size_t top = s->depth;
    while (s->stack[top - 1] != n) top--;
    top--;
    bool cycle = s->depth - top > 1;
    for (unsigned i = 0; !cycle && i < n->ninputs && !(cut && n->breaker); i++)
        for (struct wa_conn *k = n->inputs[i].conns; k; k = k->next_in) if (k->from == n) cycle = true;
    for (unsigned i = 0; !cycle && i < n->nparams; i++)
        for (struct wa_conn *k = n->params[i].inputs; k; k = k->next_in) if (k->from == n) cycle = true;
    for (size_t i = top; i < s->depth; i++) {
        struct wa_node *m = s->stack[i];
        m->on_stack = false;
        if (!cycle) continue;
        if (!cut) m->breaker = m->kind == WA_DELAY;   /* first pass: the delays in cycles break them */
        else m->muted = true;                          /* second pass: a cycle left has no delay */
    }
    s->depth = top;
}

static bool add_order(struct wa_context *c, struct wa_node *n, int phase) {
    if (c->norder == c->order_capacity) {
        size_t capacity = c->order_capacity ? c->order_capacity * 2 : 32;
        struct wa_order *order = realloc(c->order, capacity * sizeof *order);
        if (!order) return false;
        c->order = order;
        c->order_capacity = capacity;
    }
    c->order[c->norder++] = (struct wa_order){n, phase};
    return true;
}

/* Depth first: what n reads comes before n. A breaker's read half (phase 1) reads only its
   params; its write half (phase 2) goes after every node, its inputs' sources among them. */
static bool place(struct wa_context *c, struct wa_node *n) {
    n->visited = true;
    if (!n->breaker)
        for (unsigned i = 0; i < n->ninputs; i++)
            for (struct wa_conn *k = n->inputs[i].conns; k; k = k->next_in)
                if (!k->from->visited && !place(c, k->from)) return false;
    for (unsigned i = 0; i < n->nparams; i++)
        for (struct wa_conn *k = n->params[i].inputs; k; k = k->next_in)
            if (!k->from->visited && !place(c, k->from)) return false;
    return add_order(c, n, n->breaker ? 1 : 0);
}

static bool build_order(struct wa_context *c) {
    struct order_state s = {.c = c, .stack = malloc((c->nnodes + 1) * sizeof *s.stack)};
    if (!s.stack) return false;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 1; i < c->nnodes; i++) {
            struct wa_node *n = c->nodes[i];
            if (!n) continue;
            n->index = -1;
            n->on_stack = false;
            if (!pass) { n->breaker = false; n->muted = false; }
        }
        s.counter = 0;
        for (uint32_t i = 1; i < c->nnodes; i++)
            if (c->nodes[i] && c->nodes[i]->index < 0) strong(&s, c->nodes[i], pass == 1);
    }
    free(s.stack);
    c->norder = 0;
    for (uint32_t i = 1; i < c->nnodes; i++) if (c->nodes[i]) c->nodes[i]->visited = false;
    /* the listener's params come first: panners read them */
    if (!place(c, c->listener)) return false;
    for (uint32_t i = 1; i < c->nnodes; i++) {
        struct wa_node *n = c->nodes[i];
        if (n && !n->visited && !place(c, n)) return false;
    }
    for (uint32_t i = 1; i < c->nnodes; i++) {
        struct wa_node *n = c->nodes[i];
        if (n && n->breaker && !add_order(c, n, 2)) return false;
    }
    c->order_dirty = false;
    return true;
}

/* ---- the render quantum ---- */

static unsigned connection_channels(const struct wa_conn *k) {
    const struct wa_bus *b = &k->from->outputs[k->output];
    return b->channels ? b->channels : 1;
}

/* Mixes the input's active connections into its bus: computedNumberOfChannels channels
   (Web Audio 1.5.1). An input with nothing active is one channel of silence. */
static void mix_input(struct wa_node *n, struct wa_input *in) {
    unsigned most = 0;
    for (struct wa_conn *k = in->conns; k; k = k->next_in)
        if (k->from->active && !k->from->muted) most = MAX(most, connection_channels(k));
    in->active = most > 0;
    if (!in->active) {
        wa_bus_silence(&in->bus, n->mode == WA_MODE_EXPLICIT ? n->channel_count : 1);
        return;
    }
    unsigned channels = n->mode == WA_MODE_EXPLICIT ? n->channel_count
                      : n->mode == WA_MODE_CLAMPED_MAX ? MIN(most, n->channel_count) : most;
    if (!wa_bus_ready(&in->bus, channels)) { wa_bus_silence(&in->bus, 1); in->active = false; return; }
    in->bus.silent = true;
    for (struct wa_conn *k = in->conns; k; k = k->next_in)
        if (k->from->active && !k->from->muted) wa_mix_into(&in->bus, &k->from->outputs[k->output], n->interpretation);
}

static void mix_param(struct wa_param *p) {
    bool any = false;
    for (struct wa_conn *k = p->inputs; k; k = k->next_in)
        if (k->from->active && !k->from->muted) any = true;
    if (!any || !wa_bus_ready(&p->input, 1)) { wa_bus_silence(&p->input, 1); return; }
    p->input.silent = true;
    for (struct wa_conn *k = p->inputs; k; k = k->next_in)
        if (k->from->active && !k->from->muted) wa_mix_into(&p->input, &k->from->outputs[k->output], WA_SPEAKERS);
}

static bool is_source(enum wa_kind k) { return k == WA_OSCILLATOR || k == WA_BUFFER_SOURCE || k == WA_CONSTANT_SOURCE; }

static void silence_outputs(struct wa_node *n) {
    for (unsigned i = 0; i < n->noutputs; i++) wa_bus_silence(&n->outputs[i], 1);
}

/* One node's part of the quantum (Web Audio 2.4, the rendering loop's step 4.6): its params,
   its inputs mixed, then its kernel if it is actively processing. */
static void process_node(struct wa_context *c, struct wa_node *n, int phase) {
    if (n->muted) {
        n->active = false;
        silence_outputs(n);
        return;
    }
    if (phase != 2)
        for (unsigned i = 0; i < n->nparams; i++) {
            mix_param(&n->params[i]);
            compute_param(c, &n->params[i]);
        }
    if (phase == 1) {          /* a breaker's read half: its output from what it holds */
        n->active = true;
        wa_dsp_process(n, 1);
        return;
    }
    bool inputs_active = false;
    for (unsigned i = 0; i < n->ninputs; i++) {
        mix_input(n, &n->inputs[i]);
        if (n->inputs[i].active) inputs_active = true;
    }
    if (phase == 2) {          /* its write half: this quantum's input into what it holds */
        wa_dsp_process(n, 2);
        return;
    }
    if (n->kind == WA_LISTENER) return;
    if (is_source(n->kind)) n->active = n->play == WA_SCHEDULED || n->play == WA_PLAYING;
    else if (inputs_active || n->kind == WA_DESTINATION) {
        n->active = true;
        n->tail_left = wa_dsp_tail(n);
    } else if (n->tail_left) {
        n->active = true;
        n->tail_left = n->tail_left > WA_QUANTUM ? n->tail_left - WA_QUANTUM : 0;
    } else {
        enum wa_idle idle = wa_dsp_idle(n);
        if (idle == WA_IDLE_LISTEN) wa_dsp_process(n, 0);
        n->active = idle == WA_IDLE_SOUND;
    }
    if (!n->active) {
        silence_outputs(n);
        return;
    }
    wa_dsp_process(n, 0);
}

/* Renders one quantum; the destination's input is then in c->destination's input bus. */
static void render_quantum(struct wa_context *c) {
    if (c->order_dirty && !build_order(c)) {
        /* without memory to order the graph, this quantum is silence */
        wa_bus_silence(&c->destination->inputs[0].bus, c->destination->channel_count);
        c->frame += WA_QUANTUM;
        return;
    }
    for (size_t i = 0; i < c->norder; i++) process_node(c, c->order[i].node, c->order[i].phase);
    c->frame += WA_QUANTUM;
    /* Every 32 quanta, the nodes the page let go of that can no longer be heard go. */
    if (c->frame / WA_QUANTUM - c->dead_check >= 32) {
        c->dead_check = c->frame / WA_QUANTUM;
        reclaim(c);
    }
}

/* ---- contexts ---- */

struct wa_context *wa_context_new(float rate, unsigned channels, uint64_t offline_length) {
    if (!(rate >= WA_MIN_RATE && rate <= WA_MAX_RATE) || !channels || channels > WA_MAX_CHANNELS) return NULL;
    struct wa_context *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    mtx_init(&c->lock, mtx_plain);
    cnd_init(&c->wake);
    c->refs = 1;
    c->rate = rate;
    c->channels = channels;
    c->length = offline_length;
    c->state = WA_SUSPENDED;
    c->sink = -1;
    enum wa_error e;
    c->destination = make_node(c, WA_DESTINATION, NULL, &e);
    c->listener = make_node(c, WA_LISTENER, NULL, &e);
    if (offline_length) c->rendered = wa_buffer_new(channels, offline_length, rate);
    if (!c->destination || !c->listener || (offline_length && !c->rendered)) {
        wa_context_release(c);
        return NULL;
    }
    return c;
}

static void context_free(struct wa_context *c) {
    for (uint32_t i = 1; i < c->nnodes; i++) if (c->nodes[i]) free_node(c, c->nodes[i]);
    for (size_t i = 0; i < c->nevents; i++) wa_buffer_unref(c->events[i].buffer);
    wa_buffer_unref(c->rendered);
    if (c->sink >= 0) n_handle_close(c->sink);
    free(c->nodes);
    free(c->order);
    free(c->events);
    free(c->suspends);
    free(c->mix.data);
    mtx_destroy(&c->lock);
    cnd_destroy(&c->wake);
    free(c);
}

static void context_unref(struct wa_context *c) {
    mtx_lock(&c->lock);
    bool last = --c->refs == 0;
    mtx_unlock(&c->lock);
    if (last) context_free(c);
}

void wa_context_release(struct wa_context *c) {
    if (!c) return;
    mtx_lock(&c->lock);
    c->released = c->quit = true;
    c->state = WA_CLOSED;
    cnd_broadcast(&c->wake);
    mtx_unlock(&c->lock);
    context_unref(c);
}

volatile uint32_t *wa_context_posted(struct wa_context *c) { return &c->posted; }

size_t wa_context_events(struct wa_context *c, struct wa_event *out, size_t max) {
    mtx_lock(&c->lock);
    size_t n = MIN(max, c->nevents);
    memcpy(out, c->events, n * sizeof *out);
    memmove(c->events, c->events + n, (c->nevents - n) * sizeof *c->events);
    c->nevents -= n;
    mtx_unlock(&c->lock);
    return n;
}

void wa_context_status(struct wa_context *c, struct wa_context_status *out) {
    mtx_lock(&c->lock);
    *out = (struct wa_context_status){.state = c->state, .frame = c->frame, .output_played = c->output_played,
                                      .output_latency = c->output_written - c->output_played, .played_at_us = c->played_at_us};
    mtx_unlock(&c->lock);
}

uint32_t wa_context_destination(struct wa_context *c) { return c->destination->id; }
uint32_t wa_context_listener(struct wa_context *c) { return c->listener->id; }

/* The most output frames one quantum makes: at the lowest rate, each rendered frame spans
   WA_OUTPUT_RATE / WA_MIN_RATE of them. */
#define OUTPUT_QUANTUM ((size_t)(WA_QUANTUM * (WA_OUTPUT_RATE / WA_MIN_RATE)) + 2)

/* The rendered quantum (the destination's input), mixed to stereo as speakers and resampled
   to the output rate, into frames; the count of frames made. */
static size_t output_frames(struct wa_context *c, int16_t *frames) {
    struct wa_bus *in = &c->destination->inputs[0].bus;
    if (!wa_bus_ready(&c->mix, 2)) return 0;
    wa_mix_into(&c->mix, in, WA_SPEAKERS);
    const float *l = wa_channel(&c->mix, 0), *r = wa_channel(&c->mix, 1);
    size_t made = 0;
    double step = c->rate / WA_OUTPUT_RATE;
    /* linear between the rendered frames; position -1 is the previous quantum's last */
    for (double x = c->resample_phase; x < WA_QUANTUM - 1 && made < OUTPUT_QUANTUM; x += step) {
        int k = (int)floor(x);
        float f = (float)(x - k);
        float l0 = k < 0 ? c->last_frame[0] : l[k], r0 = k < 0 ? c->last_frame[1] : r[k];
        float lv = l0 + (l[k + 1] - l0) * f, rv = r0 + (r[k + 1] - r0) * f;
        lv = lv > 1 ? 1 : lv < -1 ? -1 : lv;
        rv = rv > 1 ? 1 : rv < -1 ? -1 : rv;
        frames[2 * made] = (int16_t)lrintf(lv * 32767);
        frames[2 * made + 1] = (int16_t)lrintf(rv * 32767);
        made++;
        c->resample_phase = x + step;
    }
    c->resample_phase -= WA_QUANTUM;
    c->last_frame[0] = l[WA_QUANTUM - 1];
    c->last_frame[1] = r[WA_QUANTUM - 1];
    return made;
}

/* An AudioContext's renderer: keeps about WA_OUTPUT_TARGET frames queued on the sound stream.
   Without one (every stream taken), it renders at the pace of the clock into nothing and
   asks for a stream again each second. */
static void realtime(struct wa_context *c) {
    int16_t frames[2 * OUTPUT_QUANTUM];
    uint64_t clock_start = 0, clock_frames = 0, open_at = 0;
    mtx_lock(&c->lock);
    for (;;) {
        while (!c->quit && c->state != WA_RUNNING) {
            clock_start = 0;
            cnd_wait(&c->wake, &c->lock);
        }
        if (c->quit) break;
        uint64_t now = uptime_us();
        if (c->sink < 0 && now >= open_at) {
            open_at = now + 1000000;
            mtx_unlock(&c->lock);
            n_handle sink = n_audio_open();
            mtx_lock(&c->lock);
            if (sink >= 0) { c->sink = sink; c->output_written = c->output_played = 0; }
        }
        uint64_t queued;
        if (c->sink >= 0) {
            struct n_audio_status st;
            n_handle sink = c->sink;
            mtx_unlock(&c->lock);
            bool ok = n_audio_status(sink, &st) >= 0;
            mtx_lock(&c->lock);
            if (!ok) { n_handle_close(c->sink); c->sink = -1; continue; }
            queued = (uint64_t)st.queued_frames + st.device_pending_frames;
            c->output_played = c->output_written > queued ? c->output_written - queued : 0;
        } else {
            if (!clock_start) { clock_start = now; clock_frames = 0; }
            uint64_t due = (now - clock_start) * WA_OUTPUT_RATE / 1000000;
            queued = clock_frames > due ? clock_frames - due : 0;
            c->output_played = c->output_written > queued ? c->output_written - queued : 0;
        }
        c->played_at_us = now;
        if (queued >= WA_OUTPUT_TARGET) {
            uint64_t ms = (queued - WA_OUTPUT_TARGET) * 1000 / WA_OUTPUT_RATE + 2;
            mtx_unlock(&c->lock);
            n_sleep_ms(ms > 20 ? 20 : ms);
            mtx_lock(&c->lock);
            continue;
        }
        render_quantum(c);
        size_t made = output_frames(c, frames);
        c->output_written += made;
        if (c->sink >= 0 && made) {
            n_handle sink = c->sink;
            mtx_unlock(&c->lock);
            n_result taken = n_audio_submit(sink, frames, made, N_AUDIO_WAIT);
            mtx_lock(&c->lock);
            if (taken < 0) { n_handle_close(c->sink); c->sink = -1; }
        } else clock_frames += made;
    }
    /* closed: the stream goes, so another context can have it */
    if (c->sink >= 0) n_handle_close(c->sink);
    c->sink = -1;
    mtx_unlock(&c->lock);
}

/* An OfflineAudioContext's renderer: as fast as it can, up to its length or the next frame
   it was asked to suspend at. */
static void offline(struct wa_context *c) {
    mtx_lock(&c->lock);
    while (!c->quit) {
        if (c->state != WA_RUNNING) { cnd_wait(&c->wake, &c->lock); continue; }
        if (c->frame >= c->length) {
            struct wa_buffer *done = c->rendered;
            c->rendered = NULL;
            c->state = WA_CLOSED;
            post(c, (struct wa_event){.kind = WA_EVENT_COMPLETE, .buffer = done});
            break;
        }
        bool suspend = false;
        for (size_t i = 0; i < c->nsuspends; i++)
            if (c->suspends[i] == c->frame) {
                memmove(c->suspends + i, c->suspends + i + 1, (c->nsuspends - i - 1) * sizeof *c->suspends);
                c->nsuspends--;
                suspend = true;
                break;
            }
        if (suspend) {
            c->state = WA_SUSPENDED;
            post(c, (struct wa_event){.kind = WA_EVENT_SUSPENDED, .value = c->frame});
            continue;
        }
        uint64_t at = c->frame;
        render_quantum(c);
        struct wa_bus *in = &c->destination->inputs[0].bus;
        size_t n = (size_t)MIN((uint64_t)WA_QUANTUM, c->length - at);
        for (unsigned ch = 0; ch < c->rendered->channels; ch++) {
            float *to = c->rendered->data[ch] + at;
            if (in->silent || ch >= in->channels) memset(to, 0, n * sizeof *to);
            else memcpy(to, wa_channel(in, ch), n * sizeof *to);
        }
        /* let the control thread in between quanta */
        mtx_unlock(&c->lock);
        thrd_yield();
        mtx_lock(&c->lock);
    }
    mtx_unlock(&c->lock);
}

static int renderer(void *arg) {
    struct wa_context *c = arg;
    if (c->length) offline(c); else realtime(c);
    context_unref(c);
    return 0;
}

enum wa_error wa_context_run(struct wa_context *c, bool run) {
    mtx_lock(&c->lock);
    enum wa_error e = WA_OK;
    if (c->state == WA_CLOSED) e = WA_INVALID_STATE;
    else {
        if (run && !c->thread_started) {
            c->refs++;
            if (thrd_create(&c->thread, renderer, c) != thrd_success) { c->refs--; e = WA_NO_MEMORY; }
            else { c->thread_started = true; thrd_detach(c->thread); }
        }
        if (!e) {
            /* every call reports the state it reached, changed or not: its promise waits on it */
            c->state = run ? WA_RUNNING : WA_SUSPENDED;
            post(c, (struct wa_event){.kind = WA_EVENT_STATE, .value = c->state});
            cnd_broadcast(&c->wake);
        }
    }
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_context_close(struct wa_context *c) {
    mtx_lock(&c->lock);
    enum wa_error e = c->state == WA_CLOSED && c->quit ? WA_INVALID_STATE : WA_OK;
    if (!e) {
        c->state = WA_CLOSED;
        post(c, (struct wa_event){.kind = WA_EVENT_STATE, .value = WA_CLOSED});
        c->quit = true;
        cnd_broadcast(&c->wake);
    }
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_context_suspend_at(struct wa_context *c, uint64_t frame) {
    mtx_lock(&c->lock);
    enum wa_error e = WA_OK;
    if (!c->length || frame % WA_QUANTUM) e = WA_INVALID_STATE;
    else if (frame >= c->length || frame < c->frame) e = WA_INVALID_STATE;
    else {
        for (size_t i = 0; i < c->nsuspends; i++) if (c->suspends[i] == frame) e = WA_INVALID_STATE;
        if (!e && c->nsuspends == c->suspends_capacity) {
            size_t capacity = c->suspends_capacity ? c->suspends_capacity * 2 : 4;
            uint64_t *list = realloc(c->suspends, capacity * sizeof *list);
            if (!list) e = WA_NO_MEMORY;
            else { c->suspends = list; c->suspends_capacity = capacity; }
        }
        if (!e) c->suspends[c->nsuspends++] = frame;
    }
    mtx_unlock(&c->lock);
    return e;
}

/* ---- params and attributes from the control thread ---- */

static struct wa_param *param_of(struct wa_context *c, uint32_t node, unsigned param) {
    struct wa_node *n = wa_node_at(c, node);
    return n && param < n->nparams ? &n->params[param] : NULL;
}

enum wa_error wa_param_automate(struct wa_context *c, uint32_t node, unsigned param, enum wa_automation kind,
                                double time, double value, double extra, const float *curve, size_t curve_len) {
    mtx_lock(&c->lock);
    struct wa_param *p = param_of(c, node, param);
    enum wa_error e = p ? automate(p, wa_time(c, c->frame), kind, time, value, extra, curve, curve_len) : WA_INVALID_ACCESS;
    mtx_unlock(&c->lock);
    return e;
}

enum wa_error wa_param_set_value(struct wa_context *c, uint32_t node, unsigned param, float value) {
    mtx_lock(&c->lock);
    struct wa_param *p = param_of(c, node, param);
    enum wa_error e = WA_INVALID_ACCESS;
    if (p) {
        double now = wa_time(c, c->frame);
        e = automate(p, now, WA_SET_VALUE, now, value, 0, NULL, 0);
        if (!e) p->current = value;
    }
    mtx_unlock(&c->lock);
    return e;
}

float wa_param_value(struct wa_context *c, uint32_t node, unsigned param) {
    mtx_lock(&c->lock);
    struct wa_param *p = param_of(c, node, param);
    float v = p ? p->current : 0;
    mtx_unlock(&c->lock);
    return v;
}

enum wa_error wa_param_initial(struct wa_context *c, uint32_t node, unsigned param, float value) {
    mtx_lock(&c->lock);
    struct wa_param *p = param_of(c, node, param);
    if (p) p->base = p->current = value;
    mtx_unlock(&c->lock);
    return p ? WA_OK : WA_INVALID_ACCESS;
}

enum wa_error wa_param_set_rate(struct wa_context *c, uint32_t node, unsigned param, bool a_rate) {
    mtx_lock(&c->lock);
    struct wa_param *p = param_of(c, node, param);
    enum wa_error e = !p ? WA_INVALID_ACCESS : p->info.fixed_rate && a_rate != p->info.a_rate ? WA_INVALID_STATE : WA_OK;
    if (!e) p->a_rate = a_rate;
    mtx_unlock(&c->lock);
    return e;
}

#define WITH_NODE(id, expr) do { mtx_lock(&c->lock); struct wa_node *n = wa_node_at(c, id); \
    enum wa_error e = n ? (expr) : WA_INVALID_ACCESS; mtx_unlock(&c->lock); return e; } while (0)

enum wa_error wa_set(struct wa_context *c, uint32_t node, enum wa_attribute attribute, double value) {
    WITH_NODE(node, wa_dsp_set(n, attribute, value));
}
enum wa_error wa_set_buffer(struct wa_context *c, uint32_t node, struct wa_buffer *buffer) {
    WITH_NODE(node, wa_dsp_set_buffer(n, buffer));
}
enum wa_error wa_set_wave(struct wa_context *c, uint32_t node, struct wa_wave *wave) {
    WITH_NODE(node, wa_dsp_set_wave(n, wave));
}
enum wa_error wa_set_curve(struct wa_context *c, uint32_t node, const float *curve, size_t len) {
    WITH_NODE(node, wa_dsp_set_curve(n, curve, len));
}
enum wa_error wa_frequency_response(struct wa_context *c, uint32_t node, const float *hz, float *magnitude, float *phase, size_t len) {
    WITH_NODE(node, wa_dsp_response(n, hz, magnitude, phase, len));
}
enum wa_error wa_analyse(struct wa_context *c, uint32_t node, int kind, void *out, size_t len) {
    WITH_NODE(node, wa_dsp_analyse(n, kind, out, len));
}

float wa_reduction(struct wa_context *c, uint32_t node) {
    mtx_lock(&c->lock);
    struct wa_node *n = wa_node_at(c, node);
    float r = n ? wa_dsp_reduction(n) : 0;
    mtx_unlock(&c->lock);
    return r;
}
