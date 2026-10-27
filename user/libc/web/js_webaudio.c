/* host.webaudio (js_webaudio.h): the tokens js_webaudio.js holds for the renderer's contexts,
 * nodes, buffers and periodic waves; the tasks that bring what the renderers posted, and
 * finished decodes, back to the page; and decodeAudioData's decoding. FFmpeg runs through
 * nmedia on the page's thread in slices (its allocator belongs to that thread), and the
 * resampling to the context's rate on a thread of its own. */
#include "js_webaudio.h"
#include "webaudio.h"
#include <media.h>
#include <nocturne.h>
#include <web.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WEB_AUDIO_TOKEN (UINT64_C(0x55) << 56)
#define TOKEN_MASK ((UINT64_C(1) << 56) - 1)
#define DECODE_SLICE_MS 4     /* decoding work per task */
#define RESAMPLE_CHUNK 65536  /* frames between the resampler's looks at whether it is still wanted */
#define WATCH_POLL_MS 16      /* when a word cannot be watched, how often to look at it */

static JSClassID context_class, node_class, buffer_class, wave_class;
static JSClassID float32_class, float64_class, uint8_class, array_buffer_class;
static uint64_t next_token;

static const char *const kind_names[WA_KINDS] = {
    "destination", "listener", "gain", "oscillator", "bufferSource", "constantSource", "delay", "biquad", "iir",
    "compressor", "stereoPanner", "panner", "convolver", "waveShaper", "analyser", "splitter", "merger",
};
static const char *const attribute_names[] = {
    "type", "loop", "loopStart", "loopEnd", "normalize", "oversample", "panningModel", "distanceModel", "refDistance",
    "maxDistance", "rolloffFactor", "coneInnerAngle", "coneOuterAngle", "coneOuterGain", "fftSize",
    "smoothingTimeConstant", "minDecibels", "maxDecibels",
};
static const char *const automation_names[] = {
    "setValue", "linearRamp", "exponentialRamp", "setTarget", "setValueCurve", "cancel", "cancelAndHold",
};
static const char *const mode_names[] = {"max", "clamped-max", "explicit"};
static const char *const interpretation_names[] = {"speakers", "discrete"};
static const char *const state_names[] = {"suspended", "running", "closed"};
/* A context's queue holds the renderer's events and, in the same order, the tasks
   js_webaudio.js queues (Web Audio fires statechange and complete each in a task of its own,
   after the task that changed the state or resolved the promise). */
enum { TASK_PAGE = WA_EVENT_COMPLETE + 1 };
static const char *const event_names[] = {"ended", "state", "suspended", "complete", "task"};
static const char *const error_names[] = {
    [WA_INDEX_SIZE] = "IndexSizeError", [WA_INVALID_ACCESS] = "InvalidAccessError",
    [WA_INVALID_STATE] = "InvalidStateError", [WA_NOT_SUPPORTED] = "NotSupportedError",
    [WA_RANGE] = "RangeError", [WA_SYNTAX] = "SyntaxError",
};
#define COUNT(a) (int)(sizeof(a) / sizeof *(a))

/* The 26 interfaces js_webaudio.js returns, by their global names. */
static const char *const interface_names[] = {
    "AudioContext", "BaseAudioContext", "OfflineAudioContext", "OfflineAudioCompletionEvent", "AudioBuffer",
    "AudioNode", "AudioParam", "AudioScheduledSourceNode", "AnalyserNode", "AudioBufferSourceNode",
    "AudioDestinationNode", "AudioListener", "BiquadFilterNode", "ChannelMergerNode", "ChannelSplitterNode",
    "ConstantSourceNode", "ConvolverNode", "DelayNode", "DynamicsCompressorNode", "GainNode", "IIRFilterNode",
    "OscillatorNode", "PannerNode", "PeriodicWave", "StereoPannerNode", "WaveShaperNode",
};

/* ---- contexts ---- */

/* A BaseAudioContext of a realm: the renderer's context, the function js_webaudio.js gave for
   its events, and the events drained from the renderer but not yet delivered. The context's
   token and each of its nodes' tokens hold a reference. */
struct audio_context {
    struct audio_context *next;
    struct wa_context *c;
    JSContext *realm;     /* NULL once the realm went */
    JSValue callback;     /* held by the token (marked by it), freed with it */
    uint32_t refs;
    struct wa_event *events;
    size_t first, count, capacity;
    uint32_t seen;        /* *wa_context_posted when the queue last caught up with it */
    uint64_t token;       /* its watch on web_wait_queue */
    bool watching;
};
static struct audio_context *contexts;

struct node_token { struct audio_context *a; uint32_t id; };
/* acquired: the renderer was given the data (Web Audio 1.4.3), so the page's next look at a
   channel copies it first. */
struct buffer_token { struct wa_buffer *b; bool acquired; };

static void unwatch(struct audio_context *a) {
    if (a->watching) n_events_unwatch(web_wait_queue(), a->token);
    a->watching = false;
}

static void drop_events(struct audio_context *a) {
    for (size_t i = a->first; i < a->count; i++) wa_buffer_unref(a->events[i].buffer);
    a->first = a->count = 0;
}

static void context_unref(struct audio_context *a) {
    if (--a->refs) return;
    for (struct audio_context **link = &contexts; *link; link = &(*link)->next)
        if (*link == a) { *link = a->next; break; }
    unwatch(a);
    drop_events(a);
    free(a->events);
    wa_context_release(a->c);
    free(a);
}

/* A context whose events still have somewhere to go. */
static bool deliverable(const struct audio_context *a, JSContext *ctx) {
    return a->realm == ctx && !JS_IsUndefined(a->callback);
}

static bool posted(const struct audio_context *a) {
    return __atomic_load_n(wa_context_posted(a->c), __ATOMIC_ACQUIRE) != a->seen;
}

/* Room at the queue's end for one more entry, at least. */
static bool make_room(struct audio_context *a) {
    if (a->first == a->count) a->first = a->count = 0;
    if (a->count < a->capacity) return true;
    if (a->first) {
        memmove(a->events, a->events + a->first, (a->count - a->first) * sizeof *a->events);
        a->count -= a->first;
        a->first = 0;
        return true;
    }
    size_t capacity = a->capacity ? a->capacity * 2 : 8;
    struct wa_event *events = realloc(a->events, capacity * sizeof *events);
    if (!events) return false;
    a->events = events;
    a->capacity = capacity;
    return true;
}

/* Move what the renderer posted into the queue. seen stays behind when memory runs out, so
   the next look tries again. */
static void drain(struct audio_context *a) {
    uint32_t now = __atomic_load_n(wa_context_posted(a->c), __ATOMIC_ACQUIRE);
    if (now == a->seen) return;
    unwatch(a); /* it waited for the old count */
    for (;;) {
        if (!make_room(a)) return;
        size_t room = a->capacity - a->count, n = wa_context_events(a->c, a->events + a->count, room);
        a->count += n;
        if (n < room) break;
    }
    a->seen = now;
}

/* Watch the posted count on q: 1 when watching, 0 when news already came, -1 when it cannot
   be watched. */
static int watch_word(n_handle q, volatile uint32_t *word, uint32_t expected, uint64_t token, bool *watching) {
    if (*watching) return 1;
    if (q < 0) return -1;
    n_result r = n_events_watch_address(q, word, expected, token);
    if (r == -N_ERR_NOT_READY) return 0;
    if (r < 0) return -1;
    *watching = true;
    return 1;
}

static void context_finalizer(JSRuntime *rt, JSValue val) {
    struct audio_context *a = JS_GetOpaque(val, context_class);
    if (!a) return;
    /* nothing can reach the context again: it stops, and its stream goes */
    wa_context_close(a->c);
    JS_FreeValueRT(rt, a->callback);
    a->callback = JS_UNDEFINED;
    context_unref(a);
}

static void context_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark) {
    struct audio_context *a = JS_GetOpaque(val, context_class);
    if (a) JS_MarkValue(rt, a->callback, mark);
}

static void node_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    struct node_token *t = JS_GetOpaque(val, node_class);
    if (!t) return;
    wa_node_release(t->a->c, t->id);
    context_unref(t->a);
    free(t);
}

static void buffer_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    struct buffer_token *t = JS_GetOpaque(val, buffer_class);
    if (!t) return;
    wa_buffer_unref(t->b);
    free(t);
}

static void wave_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    wa_wave_unref(JS_GetOpaque(val, wave_class));
}

/* A token for b, which it takes. */
static JSValue buffer_token(JSContext *ctx, struct wa_buffer *b) {
    struct buffer_token *t = calloc(1, sizeof *t);
    JSValue token = JS_NewObjectClass(ctx, (int)buffer_class);
    if (!t || JS_IsException(token)) {
        free(t);
        wa_buffer_unref(b);
        if (JS_IsException(token)) return token;
        JS_FreeValue(ctx, token);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->b = b;
    JS_SetOpaque(token, t);
    return token;
}

/* ---- arguments ---- */

static struct audio_context *context_arg(JSContext *ctx, JSValueConst v) {
    struct audio_context *a = JS_GetOpaque(v, context_class);
    if (!a) JS_ThrowTypeError(ctx, "Not a Web Audio context");
    return a;
}

static struct node_token *node_arg(JSContext *ctx, JSValueConst v) {
    struct node_token *t = JS_GetOpaque(v, node_class);
    if (!t) JS_ThrowTypeError(ctx, "Not a Web Audio node");
    return t;
}

static struct buffer_token *buffer_arg(JSContext *ctx, JSValueConst v) {
    struct buffer_token *t = JS_GetOpaque(v, buffer_class);
    if (!t) JS_ThrowTypeError(ctx, "Not a Web Audio buffer");
    return t;
}

/* The index of v's name in names; -1 for undefined, -2 with an exception thrown otherwise. */
static int lookup(JSContext *ctx, JSValueConst v, const char *const *names, int n) {
    if (JS_IsUndefined(v)) return -1;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return -2;
    int found = -1;
    for (int i = 0; i < n && found < 0; i++) if (!strcmp(s, names[i])) found = i;
    JS_FreeCString(ctx, s);
    if (found < 0) { JS_ThrowTypeError(ctx, "Unknown Web Audio name"); return -2; }
    return found;
}

/* A typed array's elements, when v is one of class cls: the data and the count (a detached
   array has none), or NULL with an exception thrown. */
static void *elements(JSContext *ctx, JSValueConst v, JSClassID cls, size_t size, size_t *count) {
    static uint8_t none;
    if (JS_GetClassID(v) != cls) { JS_ThrowTypeError(ctx, "Unexpected Web Audio array"); return NULL; }
    size_t offset, length, per;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &offset, &length, &per);
    if (JS_IsException(ab)) return NULL;
    *count = length / size;
    if (!length) { JS_FreeValue(ctx, ab); return &none; }
    size_t bytes;
    uint8_t *data = JS_GetArrayBuffer(ctx, &bytes, ab);
    JS_FreeValue(ctx, ab);
    return data ? data + offset : NULL;
}

static bool optional(JSValueConst v) { return !JS_IsUndefined(v) && !JS_IsNull(v); }

/* A renderer call's refusal for js_webaudio.js: undefined, the name of the exception to throw,
   or an out-of-memory exception. */
static JSValue refusal(JSContext *ctx, enum wa_error e) {
    if (e == WA_OK) return JS_UNDEFINED;
    if (e == WA_NO_MEMORY) return JS_ThrowOutOfMemory(ctx);
    return JS_NewString(ctx, error_names[e]);
}

/* ---- host.webaudio: contexts ---- */

/* context(rate, channels, length, callback): an AudioContext (length 0) or an
   OfflineAudioContext, its events delivered to callback(kind, node, value, buffer). */
/* Realms whose Document stopped being fully active (js.c retired them): no context starts in
   one, and the ones it had are closed. */
static JSContext **ended;
static size_t ended_count, ended_capacity;
static bool realm_ended(JSContext *ctx) {
    for (size_t i = 0; i < ended_count; i++) if (ended[i] == ctx) return true;
    return false;
}

/* active(): whether this realm's Document is still fully active as far as Web Audio knows. */
static JSValue js_active(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(ctx, !realm_ended(ctx));
}

static JSValue js_context(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    if (realm_ended(ctx)) return JS_NewString(ctx, "InvalidStateError");
    double rate, length;
    uint32_t channels;
    if (JS_ToFloat64(ctx, &rate, argv[0]) || JS_ToUint32(ctx, &channels, argv[1]) || JS_ToFloat64(ctx, &length, argv[2]))
        return JS_EXCEPTION;
    if (!JS_IsFunction(ctx, argv[3])) return JS_ThrowTypeError(ctx, "Web Audio events need a function");
    if (!(rate >= WA_MIN_RATE && rate <= WA_MAX_RATE) || !channels || channels > WA_MAX_CHANNELS ||
        !(length >= 0 && length <= (double)UINT32_MAX))
        return JS_NewString(ctx, "NotSupportedError");
    JSValue token = JS_NewObjectClass(ctx, (int)context_class);
    if (JS_IsException(token)) return token;
    struct audio_context *a = calloc(1, sizeof *a);
    if (a) a->c = wa_context_new((float)rate, channels, (uint64_t)length);
    if (!a || !a->c) { free(a); JS_FreeValue(ctx, token); return JS_ThrowOutOfMemory(ctx); }
    a->realm = ctx;
    a->callback = JS_DupValue(ctx, argv[3]);
    a->refs = 1;
    a->token = WEB_AUDIO_TOKEN | (++next_token & TOKEN_MASK);
    a->next = contexts;
    contexts = a;
    JS_SetOpaque(token, a);
    return token;
}

/* status(context, Float64Array(6)): [rendering state, current frame, output latency and
   frames played (at the output rate), ms since those were read, whether the realm is still
   the context's]. */
static JSValue js_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    if (!a) return JS_EXCEPTION;
    size_t n;
    double *out = elements(ctx, argv[1], float64_class, sizeof *out, &n);
    if (!out) return JS_EXCEPTION;
    if (n < 6) return JS_ThrowRangeError(ctx, "Web Audio status needs six values");
    struct wa_context_status st;
    wa_context_status(a->c, &st);
    uint64_t now = uptime_us();
    out[0] = st.state;
    out[1] = (double)st.frame;
    out[2] = (double)st.output_latency;
    out[3] = (double)st.output_played;
    out[4] = st.played_at_us && now > st.played_at_us ? (double)(now - st.played_at_us) / 1000.0 : 0;
    out[5] = a->realm == ctx;
    return JS_UNDEFINED;
}

static JSValue js_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    return a ? refusal(ctx, wa_context_run(a->c, JS_ToBool(ctx, argv[1]))) : JS_EXCEPTION;
}

static JSValue js_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    return a ? refusal(ctx, wa_context_close(a->c)) : JS_EXCEPTION;
}

/* post(context): queue a task of js_webaudio.js's behind what the renderer posted. */
static JSValue js_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    if (!a) return JS_EXCEPTION;
    if (!deliverable(a, ctx)) return JS_UNDEFINED;
    drain(a);
    if (!make_room(a)) return JS_ThrowOutOfMemory(ctx);
    a->events[a->count++] = (struct wa_event){.kind = (enum wa_event_kind)TASK_PAGE};
    return JS_UNDEFINED;
}

static JSValue js_suspend_at(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    double frame;
    if (!a || JS_ToFloat64(ctx, &frame, argv[1])) return JS_EXCEPTION;
    if (!(frame >= 0 && frame < 18446744073709551616.0)) return JS_NewString(ctx, "InvalidStateError");
    return refusal(ctx, wa_context_suspend_at(a->c, (uint64_t)frame));
}

/* ---- host.webaudio: nodes ---- */

/* node(context, kind, channelCount, mode, interpretation, inputs, outputs, maxDelay,
   feedforward, feedback): a token for a new node of kind (the context's own destination or
   listener for those), or the name of the exception its options call for. */
static JSValue js_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct audio_context *a = context_arg(ctx, argv[0]);
    if (!a) return JS_EXCEPTION;
    int kind = lookup(ctx, argv[1], kind_names, WA_KINDS);
    int mode = lookup(ctx, argv[3], mode_names, COUNT(mode_names));
    int interpretation = mode == -2 ? -2 : lookup(ctx, argv[4], interpretation_names, COUNT(interpretation_names));
    if (kind == -2 || mode == -2 || interpretation == -2) return JS_EXCEPTION;
    if (kind < 0) return JS_ThrowTypeError(ctx, "A Web Audio node needs a kind");
    uint32_t count, inputs, outputs;
    double max_delay;
    if (JS_ToUint32(ctx, &count, argv[2]) || JS_ToUint32(ctx, &inputs, argv[5]) || JS_ToUint32(ctx, &outputs, argv[6]) ||
        JS_ToFloat64(ctx, &max_delay, argv[7]))
        return JS_EXCEPTION;
    struct wa_node_options o = {.channel_count = count, .mode = mode, .interpretation = interpretation,
                                .inputs = inputs, .outputs = outputs, .max_delay = max_delay};
    size_t n;
    if (optional(argv[8])) {
        if (!(o.feedforward = elements(ctx, argv[8], float64_class, sizeof(double), &n))) return JS_EXCEPTION;
        o.nfeedforward = (unsigned)n;
    }
    if (optional(argv[9])) {
        if (!(o.feedback = elements(ctx, argv[9], float64_class, sizeof(double), &n))) return JS_EXCEPTION;
        o.nfeedback = (unsigned)n;
    }
    JSValue token = JS_NewObjectClass(ctx, (int)node_class);
    if (JS_IsException(token)) return token;
    struct node_token *t = calloc(1, sizeof *t);
    if (!t) { JS_FreeValue(ctx, token); return JS_ThrowOutOfMemory(ctx); }
    enum wa_error e = WA_OK;
    uint32_t id = kind == WA_DESTINATION ? wa_context_destination(a->c)
                : kind == WA_LISTENER ? wa_context_listener(a->c) : wa_node_new(a->c, kind, &o, &e);
    if (!id) {
        free(t);
        JS_FreeValue(ctx, token);
        return refusal(ctx, e ? e : WA_NO_MEMORY);
    }
    t->a = a;
    t->id = id;
    a->refs++;
    JS_SetOpaque(token, t);
    return token;
}

static JSValue js_node_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    return t ? JS_NewUint32(ctx, t->id) : JS_EXCEPTION;
}

/* params(kind, rate, maxDelay): [[name, default, min, max, a-rate, fixed rate]] in the order
   the node numbers them. */
static JSValue js_params(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    int kind = lookup(ctx, argv[0], kind_names, WA_KINDS);
    double rate, max_delay;
    if (kind == -2 || JS_ToFloat64(ctx, &rate, argv[1]) || JS_ToFloat64(ctx, &max_delay, argv[2])) return JS_EXCEPTION;
    if (kind < 0) return JS_ThrowTypeError(ctx, "Web Audio params need a kind");
    struct wa_param_info info[16];
    unsigned n = wa_params(kind, (float)rate, max_delay, info, COUNT(info));
    JSValue list = JS_NewArray(ctx);
    if (JS_IsException(list)) return list;
    for (unsigned i = 0; i < n; i++) {
        JSValue row = JS_NewArray(ctx);
        if (JS_IsException(row)) { JS_FreeValue(ctx, list); return row; }
        JS_SetPropertyUint32(ctx, row, 0, JS_NewString(ctx, info[i].name));
        JS_SetPropertyUint32(ctx, row, 1, JS_NewFloat64(ctx, info[i].default_value));
        JS_SetPropertyUint32(ctx, row, 2, JS_NewFloat64(ctx, info[i].min));
        JS_SetPropertyUint32(ctx, row, 3, JS_NewFloat64(ctx, info[i].max));
        JS_SetPropertyUint32(ctx, row, 4, JS_NewBool(ctx, info[i].a_rate));
        JS_SetPropertyUint32(ctx, row, 5, JS_NewBool(ctx, info[i].fixed_rate));
        JS_SetPropertyUint32(ctx, list, i, row);
    }
    return list;
}

/* Two nodes of one context, or the refusal for nodes of two. */
static bool pair(JSContext *ctx, JSValueConst from, JSValueConst to, struct node_token **f, struct node_token **t) {
    *f = node_arg(ctx, from);
    *t = *f ? node_arg(ctx, to) : NULL;
    return *t != NULL;
}

static JSValue js_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *f, *t;
    uint32_t output, input;
    if (!pair(ctx, argv[0], argv[2], &f, &t) || JS_ToUint32(ctx, &output, argv[1]) || JS_ToUint32(ctx, &input, argv[3]))
        return JS_EXCEPTION;
    if (f->a != t->a) return refusal(ctx, WA_INVALID_ACCESS);
    return refusal(ctx, wa_connect(f->a->c, f->id, output, t->id, input));
}

static JSValue js_connect_param(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *f, *t;
    uint32_t output, param;
    if (!pair(ctx, argv[0], argv[2], &f, &t) || JS_ToUint32(ctx, &output, argv[1]) || JS_ToUint32(ctx, &param, argv[3]))
        return JS_EXCEPTION;
    if (f->a != t->a) return refusal(ctx, WA_INVALID_ACCESS);
    return refusal(ctx, wa_connect_param(f->a->c, f->id, output, t->id, param));
}

/* disconnect(from, output, to | null, input, param): -1 is "any" (wa_disconnect). */
static JSValue js_disconnect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *f = node_arg(ctx, argv[0]), *t = NULL;
    int32_t output, input, param;
    if (!f || JS_ToInt32(ctx, &output, argv[1]) || JS_ToInt32(ctx, &input, argv[3]) || JS_ToInt32(ctx, &param, argv[4]))
        return JS_EXCEPTION;
    if (optional(argv[2]) && !(t = node_arg(ctx, argv[2]))) return JS_EXCEPTION;
    if (t && t->a != f->a) return refusal(ctx, WA_INVALID_ACCESS);
    return refusal(ctx, wa_disconnect(f->a->c, f->id, output, t ? t->id : 0, input, param));
}

static JSValue js_channels(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t count;
    if (!t || JS_ToUint32(ctx, &count, argv[1])) return JS_EXCEPTION;
    int mode = lookup(ctx, argv[2], mode_names, COUNT(mode_names));
    int interpretation = mode == -2 ? -2 : lookup(ctx, argv[3], interpretation_names, COUNT(interpretation_names));
    if (mode == -2 || interpretation == -2) return JS_EXCEPTION;
    if (mode < 0 || interpretation < 0) return JS_ThrowTypeError(ctx, "Web Audio channels need a mode and an interpretation");
    return refusal(ctx, wa_set_channels(t->a->c, t->id, count, mode, interpretation));
}

static JSValue js_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    double when, offset, duration;
    if (!t || JS_ToFloat64(ctx, &when, argv[1]) || JS_ToFloat64(ctx, &offset, argv[2]) || JS_ToFloat64(ctx, &duration, argv[3]))
        return JS_EXCEPTION;
    return refusal(ctx, wa_start(t->a->c, t->id, when, offset, duration));
}

static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    double when;
    if (!t || JS_ToFloat64(ctx, &when, argv[1])) return JS_EXCEPTION;
    return refusal(ctx, wa_stop(t->a->c, t->id, when));
}

/* ---- host.webaudio: params and attributes ---- */

/* automate(node, param, kind, time, value, extra, curve | null) */
static JSValue js_automate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t param;
    if (!t || JS_ToUint32(ctx, &param, argv[1])) return JS_EXCEPTION;
    int kind = lookup(ctx, argv[2], automation_names, COUNT(automation_names));
    double time, value, extra;
    if (kind == -2 || JS_ToFloat64(ctx, &time, argv[3]) || JS_ToFloat64(ctx, &value, argv[4]) || JS_ToFloat64(ctx, &extra, argv[5]))
        return JS_EXCEPTION;
    if (kind < 0) return JS_ThrowTypeError(ctx, "Web Audio automation needs a kind");
    const float *curve = NULL;
    size_t n = 0;
    if (optional(argv[6]) && !(curve = elements(ctx, argv[6], float32_class, sizeof *curve, &n))) return JS_EXCEPTION;
    return refusal(ctx, wa_param_automate(t->a->c, t->id, param, kind, time, value, extra, curve, n));
}

static JSValue js_set_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t param;
    double value;
    if (!t || JS_ToUint32(ctx, &param, argv[1]) || JS_ToFloat64(ctx, &value, argv[2])) return JS_EXCEPTION;
    return refusal(ctx, wa_param_set_value(t->a->c, t->id, param, (float)value));
}

static JSValue js_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t param;
    if (!t || JS_ToUint32(ctx, &param, argv[1])) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, wa_param_value(t->a->c, t->id, param));
}

static JSValue js_rate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t param;
    if (!t || JS_ToUint32(ctx, &param, argv[1])) return JS_EXCEPTION;
    return refusal(ctx, wa_param_set_rate(t->a->c, t->id, param, JS_ToBool(ctx, argv[2])));
}

static JSValue js_initial(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    uint32_t param;
    double value;
    if (!t || JS_ToUint32(ctx, &param, argv[1]) || JS_ToFloat64(ctx, &value, argv[2])) return JS_EXCEPTION;
    return refusal(ctx, wa_param_initial(t->a->c, t->id, param, (float)value));
}

/* set(node, attribute, value): numbers as they are, enums by index, booleans as 0 or 1. */
static JSValue js_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    int attribute = lookup(ctx, argv[1], attribute_names, COUNT(attribute_names));
    double value;
    if (attribute == -2 || JS_ToFloat64(ctx, &value, argv[2])) return JS_EXCEPTION;
    if (attribute < 0) return JS_ThrowTypeError(ctx, "A Web Audio attribute needs a name");
    return refusal(ctx, wa_set(t->a->c, t->id, attribute, value));
}

static JSValue js_set_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    struct buffer_token *b = NULL;
    if (!t || (optional(argv[1]) && !(b = buffer_arg(ctx, argv[1])))) return JS_EXCEPTION;
    return refusal(ctx, wa_set_buffer(t->a->c, t->id, b ? b->b : NULL));
}

static JSValue js_set_wave(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    struct wa_wave *w = JS_GetOpaque(argv[1], wave_class);
    if (!w) return JS_ThrowTypeError(ctx, "Not a Web Audio wave");
    return refusal(ctx, wa_set_wave(t->a->c, t->id, w));
}

static JSValue js_set_curve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    const float *curve = NULL;
    size_t n = 0;
    if (optional(argv[1]) && !(curve = elements(ctx, argv[1], float32_class, sizeof *curve, &n))) return JS_EXCEPTION;
    return refusal(ctx, wa_set_curve(t->a->c, t->id, curve, n));
}

static JSValue js_reduction(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    return t ? JS_NewFloat64(ctx, wa_reduction(t->a->c, t->id)) : JS_EXCEPTION;
}

/* response(node, frequencyHz, magResponse, phaseResponse) */
static JSValue js_response(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    size_t n, nm, np;
    const float *hz = elements(ctx, argv[1], float32_class, sizeof *hz, &n);
    float *magnitude = hz ? elements(ctx, argv[2], float32_class, sizeof *magnitude, &nm) : NULL;
    float *phase = magnitude ? elements(ctx, argv[3], float32_class, sizeof *phase, &np) : NULL;
    if (!phase) return JS_EXCEPTION;
    if (nm < n || np < n) return refusal(ctx, WA_INVALID_ACCESS);
    return refusal(ctx, wa_frequency_response(t->a->c, t->id, hz, magnitude, phase, n));
}

/* analyse(node, kind, array): AnalyserNode's four get…Data, by wa_analyse's kind. */
static JSValue js_analyse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct node_token *t = node_arg(ctx, argv[0]);
    int32_t kind;
    if (!t || JS_ToInt32(ctx, &kind, argv[1])) return JS_EXCEPTION;
    if (kind < 0 || kind > 3) return JS_ThrowRangeError(ctx, "Unknown analysis");
    bool bytes = kind == 1 || kind == 3;
    size_t n;
    void *out = elements(ctx, argv[2], bytes ? uint8_class : float32_class, bytes ? 1 : sizeof(float), &n);
    if (!out) return JS_EXCEPTION;
    return refusal(ctx, wa_analyse(t->a->c, t->id, kind, out, n));
}

/* ---- host.webaudio: buffers and waves ---- */

/* buffer(channels, length, rate): a token for a new silent buffer, or NotSupportedError. */
static JSValue js_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    uint32_t channels, length;
    double rate;
    if (JS_ToUint32(ctx, &channels, argv[0]) || JS_ToUint32(ctx, &length, argv[1]) || JS_ToFloat64(ctx, &rate, argv[2]))
        return JS_EXCEPTION;
    if (!channels || channels > WA_MAX_CHANNELS || !length || !(rate >= WA_MIN_RATE && rate <= WA_MAX_RATE))
        return JS_NewString(ctx, "NotSupportedError");
    struct wa_buffer *b = wa_buffer_new(channels, length, (float)rate);
    return b ? buffer_token(ctx, b) : JS_ThrowOutOfMemory(ctx);
}

/* Detaching calls this, and the ArrayBuffer's finalizer calls it again with no data. */
static void channel_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)rt;
    if (ptr) wa_buffer_unref(opaque);
}

/* channel(buffer, channel): an ArrayBuffer over the channel's samples. The page holds it as
   long as it wants; it cannot transfer it, and only acquire detaches it. */
static JSValue js_channel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct buffer_token *t = buffer_arg(ctx, argv[0]);
    uint32_t channel;
    if (!t || JS_ToUint32(ctx, &channel, argv[1])) return JS_EXCEPTION;
    if (channel >= t->b->channels) return JS_ThrowRangeError(ctx, "No such channel");
    if (t->acquired) {
        struct wa_buffer *copy = wa_buffer_copy(t->b);
        if (!copy) return JS_ThrowOutOfMemory(ctx);
        wa_buffer_unref(t->b);
        t->b = copy;
        t->acquired = false;
    }
    struct wa_buffer *b = wa_buffer_ref(t->b);
    JSValue ab = JS_NewArrayBuffer(ctx, (uint8_t *)b->data[channel], b->length * sizeof(float), channel_free, b, false);
    /* QuickJS does not take an external buffer on failure */
    if (JS_IsException(ab)) { wa_buffer_unref(b); return ab; }
    JS_SetArrayBufferTransferable(ctx, ab, false);
    return ab;
}

/* acquire(buffer, arrayBuffers): the renderer has the data now; the page's channel arrays
   (their ArrayBuffers listed) are detached. */
static JSValue js_acquire(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct buffer_token *t = buffer_arg(ctx, argv[0]);
    uint32_t n;
    if (!t) return JS_EXCEPTION;
    JSValue length = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToUint32(ctx, &n, length)) { JS_FreeValue(ctx, length); return JS_EXCEPTION; }
    JS_FreeValue(ctx, length);
    for (uint32_t i = 0; i < n; i++) {
        JSValue ab = JS_GetPropertyUint32(ctx, argv[1], i);
        if (JS_IsException(ab)) return ab;
        if (JS_GetClassID(ab) == array_buffer_class) JS_DetachArrayBuffer(ctx, ab);
        JS_FreeValue(ctx, ab);
    }
    t->acquired = true;
    return JS_UNDEFINED;
}

static JSValue js_buffer_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    struct buffer_token *t = buffer_arg(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    JSValue info = JS_NewArray(ctx);
    if (JS_IsException(info)) return info;
    JS_SetPropertyUint32(ctx, info, 0, JS_NewUint32(ctx, t->b->channels));
    JS_SetPropertyUint32(ctx, info, 1, JS_NewFloat64(ctx, (double)t->b->length));
    JS_SetPropertyUint32(ctx, info, 2, JS_NewFloat64(ctx, t->b->rate));
    return info;
}

/* wave(real | null, imag | null, n, normalize): a token for a PeriodicWave of n coefficients. */
static JSValue js_wave(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    const float *real = NULL, *imag = NULL;
    size_t nr = 0, ni = 0;
    uint32_t n;
    if (optional(argv[0]) && !(real = elements(ctx, argv[0], float32_class, sizeof *real, &nr))) return JS_EXCEPTION;
    if (optional(argv[1]) && !(imag = elements(ctx, argv[1], float32_class, sizeof *imag, &ni))) return JS_EXCEPTION;
    if (JS_ToUint32(ctx, &n, argv[2])) return JS_EXCEPTION;
    if ((real && nr < n) || (imag && ni < n)) return JS_NewString(ctx, "IndexSizeError");
    JSValue token = JS_NewObjectClass(ctx, (int)wave_class);
    if (JS_IsException(token)) return token;
    struct wa_wave *w = wa_wave_new(real, imag, n, JS_ToBool(ctx, argv[3]));
    if (!w) { JS_FreeValue(ctx, token); return JS_ThrowOutOfMemory(ctx); }
    JS_SetOpaque(token, w);
    return token;
}

/* ---- decodeAudioData ---- */

enum { DECODING, RESAMPLING, READY };

/* The resampling of one decode, on a thread of its own (or, when none could start, on the
   page's thread in slices): in, channels planes of frames samples stride apart, into out. */
struct resample_work {
    float *in;
    size_t stride, frames;
    double from, to;
    struct wa_buffer *out;
    unsigned channel;
    size_t first;
    uint32_t owner;          /* 0 both, 1 the thread finished, 2 the page let go */
    volatile uint32_t done;  /* 1 once the thread is through: the word web_wait_queue watches */
};

struct decode_job {
    struct decode_job *next;
    JSContext *realm;
    JSValue callback;        /* callback(error name | undefined, buffer token) */
    int stage;
    const char *error;       /* READY: the exception's name, or NULL */
    float rate;              /* the context's */
    nmedia *media;
    float *data;             /* channels planes of capacity frames */
    size_t frames, capacity;
    unsigned channels;
    int file_rate;
    struct resample_work *work;
    bool threaded, watching;
    uint64_t token;
    struct wa_buffer *result;
};
static struct decode_job *jobs, **jobs_tail = &jobs;

static void work_free(struct resample_work *w) {
    free(w->in);
    wa_buffer_unref(w->out);
    free(w);
}

/* The next chunk of resampling; false once every channel is done. */
static bool resample_chunk(struct resample_work *w) {
    if (w->channel >= w->out->channels) return false;
    size_t count = w->out->length - w->first;
    if (count > RESAMPLE_CHUNK) count = RESAMPLE_CHUNK;
    wa_resample(w->in + w->channel * w->stride, w->frames, w->from, w->to, w->out->data[w->channel], w->first, count);
    w->first += count;
    if (w->first == w->out->length) { w->first = 0; w->channel++; }
    return w->channel < w->out->channels;
}

static int resampler(void *arg) {
    struct resample_work *w = arg;
    while (__atomic_load_n(&w->owner, __ATOMIC_ACQUIRE) == 0 && resample_chunk(w)) {}
    /* wake first: the page takes the work, and may free it, only once owner says so */
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    wake_address(&w->done, UINT32_MAX);
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&w->owner, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        work_free(w); /* the page went; nobody waits for it */
    return 0;
}

static void job_unwatch(struct decode_job *j) {
    if (j->watching) n_events_unwatch(web_wait_queue(), j->token);
    j->watching = false;
}

static void job_finish(struct decode_job *j, const char *error, struct wa_buffer *result) {
    job_unwatch(j);
    if (j->media) nmedia_close(j->media);
    j->media = NULL;
    free(j->data);
    j->data = NULL;
    j->stage = READY;
    j->error = error;
    j->result = result;
}

/* The page lets go of the job: a thread still resampling frees its work when it is through. */
static void job_free(JSContext *ctx, struct decode_job *j) {
    job_unwatch(j);
    if (j->media) nmedia_close(j->media);
    free(j->data);
    if (j->work) {
        uint32_t expected = 0;
        if (!j->threaded || !__atomic_compare_exchange_n(&j->work->owner, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            work_free(j->work);
    }
    wa_buffer_unref(j->result);
    JS_FreeValue(ctx, j->callback);
    free(j);
}

static void job_unlink(struct decode_job *j) {
    for (struct decode_job **link = &jobs; *link; link = &(*link)->next)
        if (*link == j) {
            *link = j->next;
            if (jobs_tail == &j->next) jobs_tail = link;
            return;
        }
}

/* Room for frames more decoded frames in each plane. */
static bool job_room(struct decode_job *j, size_t frames) {
    if (j->frames + frames <= j->capacity) return true;
    size_t capacity = j->capacity ? j->capacity * 2 : 65536;
    if (capacity < j->frames + frames) capacity = j->frames + frames;
    if (capacity > SIZE_MAX / sizeof(float) / j->channels) return false;
    float *data = realloc(j->data, capacity * j->channels * sizeof *data);
    if (!data) return false;
    /* the planes move apart, the last first */
    for (unsigned ch = j->channels; ch-- > 1;)
        memmove(data + ch * capacity, data + ch * j->capacity, j->frames * sizeof *data);
    j->data = data;
    j->capacity = capacity;
    return true;
}

static bool job_append(struct decode_job *j, const struct nmedia_output *o) {
    if (!j->channels) {
        if (o->native_channels < 1 || o->native_channels > WA_MAX_CHANNELS || o->native_rate <= 0) return false;
        j->channels = (unsigned)o->native_channels;
        j->file_rate = o->native_rate;
        const struct nmedia_info *info = nmedia_get_info(j->media);
        /* the duration, when the file gives one, sizes the planes at once; it is only a hint,
           and the planes grow past it as they need */
        if (info->duration_ms > 0 && info->duration_ms < INT32_MAX)
            (void)job_room(j, (size_t)(info->duration_ms * (int64_t)j->file_rate / 1000) + 4096);
    } else if ((unsigned)o->native_channels != j->channels || o->native_rate != j->file_rate) return false;
    if (!job_room(j, o->frames)) return false;
    for (unsigned ch = 0; ch < j->channels; ch++)
        memcpy(j->data + ch * j->capacity + j->frames, o->native_planes[ch], o->frames * sizeof *j->data);
    j->frames += o->frames;
    return true;
}

/* All decoded: the buffer at the context's rate, or the resampling that makes it. */
static void job_decoded(struct decode_job *j) {
    nmedia_close(j->media);
    j->media = NULL;
    if (!j->frames) { job_finish(j, "EncodingError", NULL); return; }
    if ((float)j->file_rate == j->rate) {
        /* the planes close up to frames apart */
        for (unsigned ch = 1; ch < j->channels; ch++)
            memmove(j->data + ch * j->frames, j->data + ch * j->capacity, j->frames * sizeof *j->data);
        struct wa_buffer *b = wa_buffer_adopt(j->data, j->channels, j->frames, j->rate);
        j->data = NULL;
        job_finish(j, b ? NULL : "EncodingError", b);
        return;
    }
    size_t length = wa_resampled_length(j->frames, j->file_rate, j->rate);
    struct wa_buffer *out = length ? wa_buffer_new(j->channels, length, j->rate) : NULL;
    struct resample_work *w = out ? calloc(1, sizeof *w) : NULL;
    if (!w) { wa_buffer_unref(out); job_finish(j, "EncodingError", NULL); return; }
    *w = (struct resample_work){.in = j->data, .stride = j->capacity, .frames = j->frames, .from = j->file_rate,
                                .to = j->rate, .out = out};
    j->data = NULL;
    j->work = w;
    j->stage = RESAMPLING;
    thrd_t thread;
    j->threaded = thrd_create(&thread, resampler, w) == thrd_success;
    if (j->threaded) thrd_detach(thread);
}

/* One slice of the job's work on the page's thread. */
static void job_step(struct decode_job *j) {
    uint64_t until = uptime_ms() + DECODE_SLICE_MS;
    if (j->stage == RESAMPLING) {
        if (j->threaded) {
            if (__atomic_load_n(&j->work->owner, __ATOMIC_ACQUIRE) != 1) return;
        } else {
            while (resample_chunk(j->work))
                if (uptime_ms() >= until) return;
        }
        struct wa_buffer *b = j->work->out;
        j->work->out = NULL;
        work_free(j->work);
        j->work = NULL;
        job_finish(j, NULL, b);
        return;
    }
    do {
        struct nmedia_output o = {0};
        int r = nmedia_step(j->media, &o);
        if (r == NMEDIA_AUDIO && !job_append(j, &o)) { job_finish(j, "EncodingError", NULL); return; }
        if (r == NMEDIA_ERROR) { job_finish(j, "EncodingError", NULL); return; }
        if (r == NMEDIA_END) { job_decoded(j); return; }
        /* NMEDIA_AGAIN and video frames give the buffer nothing */
    } while (uptime_ms() < until);
}

/* A job whose next step can run now. */
static bool job_runnable(const struct decode_job *j) {
    return j->stage != RESAMPLING || !j->threaded || __atomic_load_n(&j->work->owner, __ATOMIC_ACQUIRE) == 1;
}

/* decode(rate, arrayBuffer, callback): starts decoding the bytes (the ArrayBuffer is detached)
   for a context of rate; callback(error, buffer) comes in a task. A refusal is returned as the
   exception's name, and still reaches callback in a task: DataCloneError for a detached
   ArrayBuffer, TypeError for one that cannot be detached. */
static JSValue js_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    double rate;
    if (JS_ToFloat64(ctx, &rate, argv[0])) return JS_EXCEPTION;
    if (JS_GetClassID(argv[1]) != array_buffer_class) return JS_ThrowTypeError(ctx, "decodeAudioData requires an ArrayBuffer");
    if (!JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "decodeAudioData needs a callback");
    struct decode_job *j = calloc(1, sizeof *j);
    if (!j) return JS_ThrowOutOfMemory(ctx);
    j->realm = ctx;
    j->callback = JS_DupValue(ctx, argv[2]);
    j->rate = (float)rate;
    j->token = WEB_AUDIO_TOKEN | (++next_token & TOKEN_MASK);
    *jobs_tail = j;
    jobs_tail = &j->next;
    size_t size;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &size, argv[1]);
    if (!bytes && JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        j->stage = READY;
        j->error = "DataCloneError";
        return JS_NewString(ctx, j->error);
    }
    if (!JS_IsArrayBufferTransferable(ctx, argv[1])) {
        j->stage = READY;
        j->error = "TypeError";
        return JS_NewString(ctx, j->error);
    }
    /* nmedia copies the bytes; the probe is the only decoding done here */
    char error[96];
    j->media = size ? nmedia_open_memory(bytes, size, error, sizeof error) : NULL;
    JS_DetachArrayBuffer(ctx, argv[1]);
    const struct nmedia_info *info = j->media ? nmedia_get_info(j->media) : NULL;
    if (!info || !info->audio) { job_finish(j, "EncodingError", NULL); return JS_UNDEFINED; }
    nmedia_set_audio_native(j->media, true);
    j->stage = DECODING;
    return JS_UNDEFINED;
}

/* ---- tasks ---- */

/* The oldest queued event of a, to its callback. */
static int deliver_event(JSContext *ctx, struct audio_context *a) {
    struct wa_event e = a->events[a->first++];
    if (a->first == a->count) a->first = a->count = 0;
    JSValue args[4] = {
        JS_NewString(ctx, event_names[e.kind]),
        JS_NewUint32(ctx, e.node),
        e.kind == WA_EVENT_STATE && e.value < 3 ? JS_NewString(ctx, state_names[e.value]) : JS_NewFloat64(ctx, (double)e.value),
        e.kind == WA_EVENT_COMPLETE && e.buffer ? buffer_token(ctx, e.buffer) : JS_NULL,
    };
    if (e.kind != WA_EVENT_COMPLETE) wa_buffer_unref(e.buffer);
    int result = 1;
    if (JS_IsException(args[3])) result = -1;
    else {
        JSValue fn = JS_DupValue(ctx, a->callback);
        a->refs++;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 4, args);
        if (JS_IsException(r)) result = -1;
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
        context_unref(a);
    }
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    return result;
}

static int deliver_job(JSContext *ctx, struct decode_job *j) {
    job_unlink(j);
    JSValue args[2] = {j->error ? JS_NewString(ctx, j->error) : JS_UNDEFINED, JS_UNDEFINED};
    if (j->result) {
        args[1] = buffer_token(ctx, j->result);
        j->result = NULL;
    }
    int result = 1;
    if (JS_IsException(args[1])) result = -1;
    else {
        JSValue r = JS_Call(ctx, j->callback, JS_UNDEFINED, 2, args);
        if (JS_IsException(r)) result = -1;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    job_free(ctx, j);
    return result;
}

bool web_audio_ready(JSContext *ctx) {
    for (struct audio_context *a = contexts; a; a = a->next)
        if (deliverable(a, ctx) && (a->first < a->count || posted(a))) return true;
    for (struct decode_job *j = jobs; j; j = j->next)
        if (j->realm == ctx && job_runnable(j)) return true;
    return false;
}

int web_audio_run(JSContext *ctx) {
    /* the contexts' events and the decodes take turns */
    static bool decodes_first;
    decodes_first = !decodes_first;
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 0) != decodes_first) {
            for (struct audio_context *a = contexts; a; a = a->next) {
                if (!deliverable(a, ctx)) continue;
                drain(a);
                if (a->first < a->count) return deliver_event(ctx, a);
            }
        } else {
            for (struct decode_job *j = jobs; j; j = j->next) {
                if (j->realm != ctx || !job_runnable(j)) continue;
                if (j->stage == READY) return deliver_job(ctx, j);
                job_step(j);
                return 1;
            }
        }
    }
    return 0;
}

int64_t web_audio_deadline(JSContext *ctx, uint64_t now) {
    n_handle q = web_wait_queue();
    int64_t due = -1;
    for (struct audio_context *a = contexts; a; a = a->next) {
        if (!deliverable(a, ctx)) continue;
        if (a->first < a->count || posted(a)) return (int64_t)now;
        int w = watch_word(q, wa_context_posted(a->c), a->seen, a->token, &a->watching);
        if (!w) return (int64_t)now;
        if (w < 0) due = (int64_t)(now + WATCH_POLL_MS);
    }
    for (struct decode_job *j = jobs; j; j = j->next) {
        if (j->realm != ctx) continue;
        if (job_runnable(j)) return (int64_t)now;
        int w = watch_word(q, &j->work->done, 0, j->token, &j->watching);
        /* done but not yet handed over: a moment more */
        if (!w) return (int64_t)now;
        if (w < 0) due = (int64_t)(now + WATCH_POLL_MS);
    }
    return due;
}

static void end_realm(JSContext *ctx) {
    for (struct audio_context *a = contexts; a; a = a->next) {
        if (a->realm != ctx) continue;
        a->realm = NULL;
        wa_context_close(a->c);
        unwatch(a);
        drop_events(a);
    }
    for (struct decode_job **link = &jobs; *link;) {
        struct decode_job *j = *link;
        if (j->realm != ctx) { link = &j->next; continue; }
        *link = j->next;
        if (jobs_tail == &j->next) jobs_tail = link;
        job_free(ctx, j);
    }
}

void web_audio_realm_end(JSContext *ctx) {
    if (!realm_ended(ctx)) {
        if (ended_count == ended_capacity) {
            size_t capacity = ended_capacity ? ended_capacity * 2 : 8;
            JSContext **grown = realloc(ended, capacity * sizeof *grown);
            /* Without the record a context made later would only never hear back. */
            if (grown) { ended = grown; ended_capacity = capacity; }
        }
        if (ended_count < ended_capacity) ended[ended_count++] = ctx;
    }
    end_realm(ctx);
}

void web_audio_realm_free(JSContext *ctx) {
    end_realm(ctx);
    for (size_t i = 0; i < ended_count; i++)
        if (ended[i] == ctx) { ended[i] = ended[--ended_count]; break; }
}

/* ---- the realm ---- */

bool web_audio_install(JSContext *ctx, JSValueConst global, const JSLazyProperty *init) {
    for (int i = 0; i < COUNT(interface_names); i++) {
        JSAtom name = JS_NewAtom(ctx, interface_names[i]);
        if (name == JS_ATOM_NULL) return false;
        int r = JS_DefineLazyProperty(ctx, global, name, init, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, name);
        if (r < 0) return false;
    }
    return true;
}

void web_js_webaudio_init(JSContext *ctx, JSValueConst host) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!context_class) {
        JS_NewClassID(&context_class);
        JS_NewClassID(&node_class);
        JS_NewClassID(&buffer_class);
        JS_NewClassID(&wave_class);
    }
    if (!JS_IsRegisteredClass(rt, context_class)) {
        JSClassDef context = {"WebAudioContext", context_finalizer, context_mark, NULL, NULL};
        JSClassDef node = {"WebAudioNode", node_finalizer, NULL, NULL, NULL};
        JSClassDef buffer = {"WebAudioBuffer", buffer_finalizer, NULL, NULL, NULL};
        JSClassDef wave = {"WebAudioWave", wave_finalizer, NULL, NULL, NULL};
        JS_NewClass(rt, context_class, &context);
        JS_NewClass(rt, node_class, &node);
        JS_NewClass(rt, buffer_class, &buffer);
        JS_NewClass(rt, wave_class, &wave);
    }
    /* typed array and ArrayBuffer classes, from samples */
    JSValue zero = JS_NewInt32(ctx, 0);
    static const struct { JSClassID *cls; JSTypedArrayEnum type; } samples[] = {
        {&float32_class, JS_TYPED_ARRAY_FLOAT32}, {&float64_class, JS_TYPED_ARRAY_FLOAT64}, {&uint8_class, JS_TYPED_ARRAY_UINT8},
    };
    for (int i = 0; i < COUNT(samples); i++) {
        JSValue sample = JS_NewTypedArray(ctx, 1, &zero, samples[i].type);
        *samples[i].cls = JS_GetClassID(sample);
        JS_FreeValue(ctx, sample);
    }
    JSValue plain = JS_NewArrayBufferCopy(ctx, NULL, 0);
    array_buffer_class = JS_GetClassID(plain);
    JS_FreeValue(ctx, plain);
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("active", 0, js_active), JS_CFUNC_DEF("post", 1, js_post),
        JS_CFUNC_DEF("context", 4, js_context), JS_CFUNC_DEF("status", 2, js_status),
        JS_CFUNC_DEF("run", 2, js_run), JS_CFUNC_DEF("close", 1, js_close), JS_CFUNC_DEF("suspendAt", 2, js_suspend_at),
        JS_CFUNC_DEF("node", 10, js_node), JS_CFUNC_DEF("nodeId", 1, js_node_id), JS_CFUNC_DEF("params", 3, js_params),
        JS_CFUNC_DEF("connect", 4, js_connect), JS_CFUNC_DEF("connectParam", 4, js_connect_param),
        JS_CFUNC_DEF("disconnect", 5, js_disconnect), JS_CFUNC_DEF("channels", 4, js_channels),
        JS_CFUNC_DEF("start", 4, js_start), JS_CFUNC_DEF("stop", 2, js_stop),
        JS_CFUNC_DEF("automate", 7, js_automate), JS_CFUNC_DEF("setValue", 3, js_set_value),
        JS_CFUNC_DEF("value", 2, js_value), JS_CFUNC_DEF("rate", 3, js_rate), JS_CFUNC_DEF("initial", 3, js_initial),
        JS_CFUNC_DEF("set", 3, js_set), JS_CFUNC_DEF("setBuffer", 2, js_set_buffer), JS_CFUNC_DEF("setWave", 2, js_set_wave),
        JS_CFUNC_DEF("setCurve", 2, js_set_curve), JS_CFUNC_DEF("reduction", 1, js_reduction),
        JS_CFUNC_DEF("response", 4, js_response), JS_CFUNC_DEF("analyse", 3, js_analyse),
        JS_CFUNC_DEF("buffer", 3, js_buffer), JS_CFUNC_DEF("channel", 2, js_channel), JS_CFUNC_DEF("acquire", 2, js_acquire),
        JS_CFUNC_DEF("bufferInfo", 1, js_buffer_info), JS_CFUNC_DEF("wave", 4, js_wave),
        JS_CFUNC_DEF("decode", 3, js_decode),
    };
    JSValue webaudio = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, webaudio, functions, COUNT(functions));
    JS_SetPropertyStr(ctx, webaudio, "outputRate", JS_NewInt32(ctx, WA_OUTPUT_RATE));
    JS_SetPropertyStr(ctx, webaudio, "outputTarget", JS_NewInt32(ctx, WA_OUTPUT_TARGET));
    JS_SetPropertyStr(ctx, host, "webaudio", webaudio);
}
