/* The parts of the audio graph that the renderer (webaudio.c) and the node kernels
   (webaudio_dsp.c) share. Everything here is touched only with the context's lock held. */
#pragma once
#include <threads.h>
#include "webaudio.h"

#define WA_MAX_PARAMS 9

/* One output of a node, or one input mixed from its connections: channels of a render
   quantum. silent says every sample is zero (the arrays then need not be). */
struct wa_bus {
    unsigned channels;
    bool silent;
    unsigned capacity;             /* channels allocated */
    float *data;                   /* capacity * WA_QUANTUM */
};
static inline float *wa_channel(struct wa_bus *b, unsigned c) { return b->data + (size_t)c * WA_QUANTUM; }
/* The bus with channels channels, zeroed; false when memory runs out. */
bool wa_bus_ready(struct wa_bus *b, unsigned channels);
void wa_bus_silence(struct wa_bus *b, unsigned channels);

struct wa_node;
/* A connection from an output of one node to an input of another, or to one of its params
   (param >= 0). Each is on two lists: its source's and its destination's. */
struct wa_conn {
    struct wa_node *from, *to;
    unsigned output;
    int input, param;
    struct wa_conn *next_out, *prev_out;
    struct wa_conn *next_in, *prev_in;
};

struct wa_automation_event {
    enum wa_automation kind;
    double time, value, extra;   /* extra: the time constant, or the curve's duration */
    double end;                  /* a curve cut short by cancelAndHoldAtTime ends here */
    float *curve;
    size_t curve_len;
    double v0;                   /* setTarget: the value at its start, once known */
    bool v0_known;
};
struct wa_param {
    struct wa_param_info info;
    bool a_rate;
    double base;                 /* the value before the first event */
    float current;               /* [[current value]] */
    struct wa_automation_event *events;
    size_t nevents, capacity;
    struct wa_conn *inputs;
    struct wa_bus input;
    float values[WA_QUANTUM];    /* this quantum's computed values; values[0] for k-rate */
    bool constant;               /* all of values are values[0] */
};

struct wa_input {
    struct wa_conn *conns;
    struct wa_bus bus;
    bool active;                 /* some connection's source is actively processing */
};

enum wa_play { WA_UNSCHEDULED, WA_SCHEDULED, WA_PLAYING, WA_FINISHED };

struct wa_node {
    struct wa_context *ctx;
    uint32_t id;
    enum wa_kind kind;
    unsigned ninputs, noutputs, nparams;
    struct wa_input *inputs;
    struct wa_bus *outputs;
    struct wa_conn *outs;
    unsigned channel_count;
    enum wa_mode mode;
    enum wa_interpretation interpretation;
    struct wa_param params[WA_MAX_PARAMS];
    bool page_holds;             /* the page still holds the node */
    bool muted;                  /* in a cycle without a DelayNode */
    bool breaker;                /* a DelayNode in a cycle */
    bool active;                 /* actively processing this quantum */
    uint64_t tail_left;          /* frames it stays active after its inputs go quiet */
    /* AudioScheduledSourceNode */
    enum wa_play play;
    double start, stop, offset, duration;
    bool ended_posted;
    void *dsp;                   /* the kind's own state (webaudio_dsp.c) */
    /* ordering */
    int index, low;
    bool on_stack, visited;
};

struct wa_context {
    mtx_t lock;
    cnd_t wake;                  /* the renderer waits here while it has nothing to do */
    uint32_t refs;               /* the control thread and the renderer */
    float rate;
    unsigned channels;
    uint64_t frame;
    uint64_t length;             /* an OfflineAudioContext's; 0 for an AudioContext */
    enum wa_state state;         /* [[rendering thread state]] */
    bool released, quit;
    struct wa_buffer *rendered;  /* OfflineAudioContext */
    uint64_t *suspends;
    size_t nsuspends, suspends_capacity;
    struct wa_node **nodes;      /* by id; NULL slots are free */
    uint32_t nnodes, nodes_capacity;
    struct wa_node *destination, *listener;
    bool order_dirty;
    struct wa_order { struct wa_node *node; int phase; } *order;
    size_t norder, order_capacity;
    struct wa_event *events;
    size_t nevents, events_capacity;
    volatile uint32_t posted;
    bool thread_started;
    thrd_t thread;
    /* the sound stream (AudioContext) */
    int sink;
    uint64_t output_written, output_played, played_at_us;
    double resample_phase;       /* where the next output frame falls between rendered frames */
    float last_frame[2];         /* the rendered frame before the quantum being resampled */
    struct wa_bus mix;           /* the destination's input, down-mixed to stereo for output */
    uint64_t dead_check;         /* the quantum at which released nodes were last looked at */
};

/* The context's time of frame f. */
static inline double wa_time(const struct wa_context *c, uint64_t f) { return (double)f / c->rate; }
struct wa_node *wa_node_at(struct wa_context *c, uint32_t id);

/* Node kernels (webaudio_dsp.c). init: the kind's state from its options; process: the
   node's outputs from its inputs and params for the quantum at c->frame (phase 1 and 2 are
   a cycle-breaking DelayNode's read and write halves, 0 is the whole); tail: frames of
   output after the input goes quiet. */
enum wa_error wa_dsp_init(struct wa_node *n, const struct wa_node_options *o);
void wa_dsp_free(struct wa_node *n);
void wa_dsp_process(struct wa_node *n, int phase);
uint64_t wa_dsp_tail(struct wa_node *n);
/* What a node does in a quantum with no active input once its tail is over: nothing (its
   outputs are silence), take the silence in (an analyser's window moves on) while its
   outputs stay silent, or sound (a WaveShaper whose curve is not 0 at 0). */
enum wa_idle { WA_IDLE_SILENT, WA_IDLE_LISTEN, WA_IDLE_SOUND };
enum wa_idle wa_dsp_idle(struct wa_node *n);
/* The kernels' attribute and data setters behind the wa_set* calls. */
enum wa_error wa_dsp_set(struct wa_node *n, enum wa_attribute attribute, double value);
enum wa_error wa_dsp_set_buffer(struct wa_node *n, struct wa_buffer *b);
enum wa_error wa_dsp_set_wave(struct wa_node *n, struct wa_wave *w);
enum wa_error wa_dsp_set_curve(struct wa_node *n, const float *curve, size_t len);
float wa_dsp_reduction(struct wa_node *n);
enum wa_error wa_dsp_response(struct wa_node *n, const float *hz, float *magnitude, float *phase, size_t len);
enum wa_error wa_dsp_analyse(struct wa_node *n, int kind, void *out, size_t len);

/* The renderer's helpers the kernels use. */
/* The source's playing span within this quantum: frames [*from, *to) play. false when none. */
bool wa_source_span(struct wa_node *n, unsigned *from, unsigned *to);
/* The source finished playing (stop time reached, or its buffer ran out). */
void wa_source_finished(struct wa_node *n);
/* Up- or down-mix in (in_channels) into out (out_channels) by interpretation, adding to out. */
void wa_mix_into(struct wa_bus *out, struct wa_bus *in, enum wa_interpretation interpretation);
