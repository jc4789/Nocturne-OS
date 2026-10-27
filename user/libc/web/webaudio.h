/* The Web Audio rendering engine (webaudio.c, the node kernels in webaudio_dsp.c): the
   audio graph of each BaseAudioContext, rendered one render quantum at a time on a thread
   of the context's own, out to a sound stream (AudioContext) or into a buffer
   (OfflineAudioContext). It knows nothing of JavaScript; js_webaudio.c binds it.

   The page's thread is the control thread. Each call below takes the context's lock and
   applies at once, between two render quanta, which is the order the specification's
   control messages give. What the renderer has to tell the control thread (a source
   ended, a state changed, rendering completed) waits in the context's event list: the
   renderer then adds one to *wa_context_posted, and wakes whoever waits on that address. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WA_QUANTUM 128
#define WA_MAX_CHANNELS 32
#define WA_MIN_RATE 3000.0f
#define WA_MAX_RATE 768000.0f
#define WA_OUTPUT_RATE 48000 /* the sound stream's rate (N_AUDIO_RATE) */
#define WA_OUTPUT_CHANNELS 2 /* and its channels: an AudioContext destination's maxChannelCount */
#define WA_OUTPUT_TARGET 2048 /* output frames an AudioContext keeps queued ahead of the stream */

enum wa_kind {
    WA_DESTINATION, WA_LISTENER, WA_GAIN, WA_OSCILLATOR, WA_BUFFER_SOURCE, WA_CONSTANT_SOURCE,
    WA_DELAY, WA_BIQUAD, WA_IIR, WA_COMPRESSOR, WA_STEREO_PANNER, WA_PANNER, WA_CONVOLVER,
    WA_WAVESHAPER, WA_ANALYSER, WA_SPLITTER, WA_MERGER, WA_KINDS
};
enum wa_mode { WA_MODE_MAX, WA_MODE_CLAMPED_MAX, WA_MODE_EXPLICIT };
enum wa_interpretation { WA_SPEAKERS, WA_DISCRETE };
enum wa_state { WA_SUSPENDED, WA_RUNNING, WA_CLOSED };
/* What a call refuses with; the binding throws the DOMException (or RangeError) of the name. */
enum wa_error {
    WA_OK, WA_NO_MEMORY, WA_INDEX_SIZE, WA_INVALID_ACCESS, WA_INVALID_STATE, WA_NOT_SUPPORTED,
    WA_RANGE, WA_SYNTAX
};
enum wa_event_kind {
    WA_EVENT_ENDED,     /* node: the source's playback ended */
    WA_EVENT_STATE,     /* value: the rendering state (enum wa_state) the context reached */
    WA_EVENT_SUSPENDED, /* value: the frame at which an OfflineAudioContext suspended itself */
    WA_EVENT_COMPLETE,  /* buffer: an OfflineAudioContext's rendering (NULL when it failed) */
};

/* PCM: channels arrays of length frames, one block. A buffer is shared by whoever holds a
   reference (the page's AudioBuffer, a source playing it, a convolver); data written after
   it was shared is the writer's own business, as with the page's Float32Arrays. */
struct wa_buffer {
    uint32_t refs;
    unsigned channels;
    size_t length;
    float rate;
    float *data[WA_MAX_CHANNELS];
};
struct wa_buffer *wa_buffer_new(unsigned channels, size_t length, float rate);
/* A buffer over data, which it takes: channels planes of length frames, one after another
   in one block from malloc. NULL (data freed) when memory runs out. */
struct wa_buffer *wa_buffer_adopt(float *data, unsigned channels, size_t length, float rate);
struct wa_buffer *wa_buffer_ref(struct wa_buffer *b);
void wa_buffer_unref(struct wa_buffer *b);
struct wa_buffer *wa_buffer_copy(const struct wa_buffer *b);

struct wa_event {
    enum wa_event_kind kind;
    uint32_t node;
    uint64_t value;
    struct wa_buffer *buffer; /* owned by the event's receiver */
};

/* An AudioParam's description: its name in the node's interface, the range and default
   it was made with, whether it starts "a-rate" and whether that can be changed. */
struct wa_param_info {
    const char *name;
    float default_value, min, max;
    bool a_rate, fixed_rate;
};
/* The params of a node of kind, in the order wa_param_* numbers them. max_delay is the
   DelayNode's maxDelayTime (its delayTime's maximum); rate is the context's. */
unsigned wa_params(enum wa_kind kind, float rate, double max_delay, struct wa_param_info *out, unsigned max);

/* The node's options at creation, as the interface's dictionary gives them; what the
   dictionary leaves out is 0 (counts and max_delay) or -1 (mode and interpretation), and the
   kind's default then holds. */
struct wa_node_options {
    unsigned channel_count;
    int mode;                     /* enum wa_mode */
    int interpretation;           /* enum wa_interpretation */
    unsigned inputs, outputs;     /* ChannelSplitter outputs, ChannelMerger inputs */
    double max_delay;             /* DelayNode */
    const double *feedforward, *feedback; /* IIRFilterNode */
    unsigned nfeedforward, nfeedback;
};

struct wa_context;
/* An AudioContext (offline_length 0) or an OfflineAudioContext of offline_length frames.
   channels: the destination's channel count (an AudioContext's is 2). NULL when memory
   runs out. */
struct wa_context *wa_context_new(float rate, unsigned channels, uint64_t offline_length);
/* The control thread lets go of the context: it is closed, its renderer stops and its
   memory goes once the renderer is done with it. */
void wa_context_release(struct wa_context *c);
volatile uint32_t *wa_context_posted(struct wa_context *c);
/* The events waiting, oldest first; at most max. */
size_t wa_context_events(struct wa_context *c, struct wa_event *out, size_t max);
/* The rendering state, [[current frame]], and the sound stream's latency in frames of the
   output (WA_OUTPUT_RATE) and the frames it has played. */
struct wa_context_status {
    enum wa_state state;
    uint64_t frame;
    uint64_t output_latency, output_played;
    uint64_t played_at_us;      /* uptime_us when output_played was read */
};
void wa_context_status(struct wa_context *c, struct wa_context_status *out);
/* AudioContext: start or resume rendering (true), or suspend it. OfflineAudioContext:
   starting renders from [[current frame]] until the length or the next suspension. */
enum wa_error wa_context_run(struct wa_context *c, bool run);
enum wa_error wa_context_close(struct wa_context *c);
/* OfflineAudioContext.suspend(frame): frame is a multiple of WA_QUANTUM. */
enum wa_error wa_context_suspend_at(struct wa_context *c, uint64_t frame);
uint32_t wa_context_destination(struct wa_context *c);
uint32_t wa_context_listener(struct wa_context *c);

/* A node of kind; 0 when memory runs out (*error says why when it is not that). */
uint32_t wa_node_new(struct wa_context *c, enum wa_kind kind, const struct wa_node_options *o, enum wa_error *error);
/* The page no longer holds the node: it goes once nothing it does can be heard. */
void wa_node_release(struct wa_context *c, uint32_t node);
enum wa_error wa_connect(struct wa_context *c, uint32_t from, unsigned output, uint32_t to, unsigned input);
enum wa_error wa_connect_param(struct wa_context *c, uint32_t from, unsigned output, uint32_t to, unsigned param);
/* AudioNode.disconnect's forms: to 0 means every destination; output and input -1 mean any.
   param >= 0 disconnects from that param of to instead. WA_INVALID_ACCESS when nothing
   matched where the form names a destination. */
enum wa_error wa_disconnect(struct wa_context *c, uint32_t from, int output, uint32_t to, int input, int param);
enum wa_error wa_set_channels(struct wa_context *c, uint32_t node, unsigned count, enum wa_mode mode, enum wa_interpretation interpretation);

/* AudioScheduledSourceNode.start(when, offset, duration) and stop(when); duration < 0 is none. */
enum wa_error wa_start(struct wa_context *c, uint32_t node, double when, double offset, double duration);
enum wa_error wa_stop(struct wa_context *c, uint32_t node, double when);

/* AudioParam automation. */
enum wa_automation {
    WA_SET_VALUE, WA_LINEAR_RAMP, WA_EXPONENTIAL_RAMP, WA_SET_TARGET, WA_SET_CURVE,
    WA_CANCEL, WA_CANCEL_AND_HOLD
};
/* time: the event's time (already clamped to currentTime by the caller); value: its value;
   extra: setTargetAtTime's time constant or setValueCurveAtTime's duration; curve: the
   curve's values (copied). WA_NOT_SUPPORTED when it would overlap a value curve. */
enum wa_error wa_param_automate(struct wa_context *c, uint32_t node, unsigned param, enum wa_automation kind,
                                double time, double value, double extra, const float *curve, size_t curve_len);
/* AudioParam.value's setter: [[current value]] becomes value, and setValueAtTime(value,
   currentTime) is scheduled. */
enum wa_error wa_param_set_value(struct wa_context *c, uint32_t node, unsigned param, float value);
/* [[current value]]: the intrinsic value at the start of the last rendered quantum, or the
   last value set. */
float wa_param_value(struct wa_context *c, uint32_t node, unsigned param);
enum wa_error wa_param_set_rate(struct wa_context *c, uint32_t node, unsigned param, bool a_rate);
/* A value given to the node's constructor: the param's value until automation says otherwise,
   with no event on its timeline. */
enum wa_error wa_param_initial(struct wa_context *c, uint32_t node, unsigned param, float value);

/* Node attributes that are not params, by name in the interface. Numbers go as double; enums
   by their index in the interface's enum; booleans as 0 or 1. */
enum wa_attribute {
    WA_TYPE,            /* OscillatorType, BiquadFilterType */
    WA_LOOP, WA_LOOP_START, WA_LOOP_END,
    WA_NORMALIZE,       /* ConvolverNode.normalize */
    WA_OVERSAMPLE,      /* WaveShaperNode: "none", "2x", "4x" */
    WA_PANNING_MODEL, WA_DISTANCE_MODEL, WA_REF_DISTANCE, WA_MAX_DISTANCE, WA_ROLLOFF,
    WA_CONE_INNER, WA_CONE_OUTER, WA_CONE_OUTER_GAIN,
    WA_FFT_SIZE, WA_SMOOTHING, WA_MIN_DECIBELS, WA_MAX_DECIBELS,
};
enum wa_error wa_set(struct wa_context *c, uint32_t node, enum wa_attribute attribute, double value);
/* AudioBufferSourceNode.buffer and ConvolverNode.buffer (NULL clears it); the node takes a
   reference. */
enum wa_error wa_set_buffer(struct wa_context *c, uint32_t node, struct wa_buffer *buffer);
/* A PeriodicWave: Fourier coefficients real and imag (n of each, either NULL for zeros; the
   first, DC, is not used), as the band-limited tables an oscillator plays. NULL when memory
   runs out. Shared by whoever holds a reference. */
struct wa_wave;
struct wa_wave *wa_wave_new(const float *real, const float *imag, size_t n, bool normalize);
void wa_wave_unref(struct wa_wave *w);
/* OscillatorNode.setPeriodicWave; the node takes a reference. */
enum wa_error wa_set_wave(struct wa_context *c, uint32_t node, struct wa_wave *wave);
/* WaveShaperNode.curve; NULL clears it. */
enum wa_error wa_set_curve(struct wa_context *c, uint32_t node, const float *curve, size_t n);
/* DynamicsCompressorNode.reduction */
float wa_reduction(struct wa_context *c, uint32_t node);
/* BiquadFilterNode and IIRFilterNode getFrequencyResponse for n frequencies in Hz. */
enum wa_error wa_frequency_response(struct wa_context *c, uint32_t node, const float *hz, float *magnitude, float *phase, size_t n);
/* AnalyserNode: kind 0 getFloatFrequencyData, 1 getByteFrequencyData, 2 getFloatTimeDomainData,
   3 getByteTimeDomainData, into out (float or uint8_t), at most n values. */
enum wa_error wa_analyse(struct wa_context *c, uint32_t node, int kind, void *out, size_t n);

/* decodeAudioData's resampling (webaudio_dsp.c), by windowed sinc: n frames at from Hz make
   wa_resampled_length frames at to Hz (0 when that is more than a buffer can hold), and
   wa_resample computes count of them from first, so the work can be cut into slices. */
size_t wa_resampled_length(size_t n, double from, double to);
void wa_resample(const float *in, size_t n, double from, double to, float *out, size_t first, size_t count);

