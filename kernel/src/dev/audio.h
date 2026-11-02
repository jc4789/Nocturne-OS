#pragma once
#include "kernel.h"
#include "sys/wait.h"

/* Sound. An app's audio handle is a stream of 48 kHz stereo frames (two signed 16-bit
   little-endian samples, left first); a submit waits while the stream's buffer is full. The
   streams are mixed, scaled by the master volume (0-100) and played by one sink:
   a remote desktop client while one takes the sound, else the output that plays, else nothing at
   all, at the pace a device would play it (so programs take as long as their sound does).

   A machine has any number of outputs, each a way out of a sound device: a jack of a codec, a pin to
   a display. One plays at a time: the one the user chose, else the best there is (audio_sink_add).

   No tick drives it. A sink mixes when its device needs more: an output in the audio thread, which
   its device's interrupt (or an alarm at the moment its queue falls to where it refills) wakes, a
   remote client's sound thread when the client's queue falls below its target, the sink of nothing
   from an alarm at the end of each 10 ms block on the clock (sys/clock.h). When nothing plays nothing
   is armed: a stream that gets PCM starts an idle sink at once, on any CPU. No sink sums PCM with the
   mixer's lock held, or with interrupts off.

   Every frame a sink is given has an ordinal, the count of frames given to sinks before it. A sink
   reports which it has played (audio_played), and a stream's played frames are its frames before
   that place, at the clock time of the report. Its position and clock are what the mixer needs to
   play every stream at AUDIO_RATE frames per second of the clock, whatever its device's crystal
   does (the pace, audio.c): ordinals are the device's frames, and a stream's are the clock's. */

#define AUDIO_RATE  48000 /* of the streams and the mix; an output's own format is its device's to say */
#define AUDIO_FRAME 4     /* bytes of a stream's frame */

void audio_init(void);
void hda_init(void); /* dev/hda.c: every High Definition Audio controller's outputs */

/* The apps' streams (docs/nocturne-native-abi.md §9). The others take a handle audio_stream_is
   said yes to. */
struct handle;
struct n_audio_status;
int audio_stream_open(struct handle **out); /* N_ERR_BUSY when every stream is taken */
bool audio_stream_is(struct handle *h);
/* Any CPU: frames taken (one writer at a time), and the stream's clock. */
int64_t audio_stream_submit(struct handle *h, const void *user_frames, size_t count, bool wait);
int audio_stream_status(struct handle *h, struct n_audio_status *user_out);
/* Drop this stream's PCM at once and retire its old clock generation. What an output had mixed but
 * its device had not begun to fetch is taken back and mixed again without it (audio_sink_ops'
 * unissue), so after a flush the stream is heard only for what the device already holds. A remote
 * client's blocks were sent and stay heard. None of it becomes the new generation's played samples.
 * Ordinary close lets the queue play out (a program that writes a sound and exits); the close of
 * a stopped program drops it as a flush does. */
int audio_stream_flush(struct handle *h);

/* A sink reports what it has played: every frame mixed for it before `ordinal` had been heard at
   clock time `at` (clock_now_ns as the device's position was read, or the moment a remote client
   says it played them). Any CPU, any context; an ordinal at or below one reported before changes
   nothing. */
void audio_played(uint64_t ordinal, uint64_t at);
bool audio_pending(void); /* some stream has frames waiting */
/* Whether a sink should mix a block of `frames` now: some stream has PCM, and every stream that
   has any has a whole block of it, or the sink's device is about to run dry (`low`). A block mixed
   from a stream that has only part of one plays the rest as silence in the middle of its sound
   when its next PCM comes a moment later; while the device has enough queued, waiting does not. */
bool audio_ready(int frames, bool low);

/* The format an output's device plays, from what the device declares: AUDIO_RATE, which every device
   here has; one channel or two; bits of each sample in a container of 2 or 4 bytes, left-justified. */
struct audio_format {
    uint32_t rate;
    uint8_t channels, bits, container;
};

enum audio_kind { AUDIO_KIND_SPEAKER, AUDIO_KIND_HEADPHONE, AUDIO_KIND_LINE, AUDIO_KIND_DISPLAY, AUDIO_KIND_DIGITAL, AUDIO_KIND_OTHER };
/* NONE: nothing tells, or nothing needs plugging in. */
enum audio_jack { AUDIO_JACK_NONE, AUDIO_JACK_EMPTY, AUDIO_JACK_PLUGGED };

struct audio_sink;
/* The calls into an output, one at a time, in a thread that may sleep, with nothing held but the
   outputs' mutex (unissue: and the mixer's lock, so it does not sleep there).
   open: it becomes the output that plays: its device is routed; false when it cannot.
   close: it no longer does. Its device stops, and all it was given counts as played (audio_played).
   fill: reports what its device has played (audio_played), tops its buffers up with audio_sink_mix
   (deciding with audio_ready), and starts or stops its device. The audio thread calls it after a
   wake (audio_sink_wake): the device's interrupt, or an alarm where none tells it early enough. While
   audio_remote() it may only drain already-issued buffers. Called on an idle output that has nothing
   to start, it returns without device I/O. One that reports less than all it was given will be woken
   again by its device.
   unissue(take): where the PCM its device has not begun to fetch starts, as an ordinal (the one
   after the last issued frame when there is none); with take, it also stops the device short of
   it, forgets it and lets fill() mix it again. The mixer takes back from that ordinal when a stream
   is flushed (audio_stream_flush). */
struct audio_sink_ops {
    bool (*open)(struct audio_sink *sink);
    void (*close)(struct audio_sink *sink);
    void (*fill)(struct audio_sink *sink);
    uint64_t (*unissue)(struct audio_sink *sink, bool take);
};
/* An output, the driver's for good once added. It fills all fields but the mixer's before it does. */
struct audio_sink {
    const struct audio_sink_ops *ops;
    void *priv;
    char name[32];        /* for people; n_audio_output's device */
    enum audio_kind kind;
    enum audio_jack jack;
    struct audio_format format;
    uint64_t period_ns;   /* how long a block of its device lasts: the audio thread's deadline */
    unsigned blocks;      /* the most blocks of audio_sink_mix its device holds before it reports them played */
    /* the mixer's, under its mutex */
    struct audio_sink *next;
    bool available;
};
/* In a thread that may sleep. Add: there is one more output, which plays when it is the best one
   (something plugged in comes first, then what needs nothing plugged in; headphones, speakers, a line
   out, a display, a digital out in that order among equals). State: what is plugged in, and the name
   when not NULL, changed: the best one may be another now. Available false: it cannot play (its
   device is stopped or gone), and it is closed before this returns; true: it can again. */
void audio_sink_add(struct audio_sink *sink);
void audio_sink_state(struct audio_sink *sink, enum audio_jack jack, const char *name);
void audio_sink_available(struct audio_sink *sink, bool available);
/* Any context: the audio thread fills the output that plays. */
void audio_sink_wake(void);
/* Mix `frames` frames from every stream into out for the output that plays, in its format, each
   stream's share remembered until it reports them played or gives them back. *end: the ordinal after
   them, which it reports with audio_played. False, with out untouched, when no stream had anything,
   when it is not the one that plays, or while audio_remote(). */
bool audio_sink_mix(struct audio_sink *sink, void *out, int frames, uint64_t *end);

/* The outputs for the settings. audio_sinks: how many there are, the first `room` of them described
   in out. audio_sink_choose: output number index plays while it is available, whatever else is plugged
   in; AUDIO_SINK_BEST goes back to the best there is. False for a number with no output. Both sleep. */
struct audio_sink_info {
    char name[32];
    enum audio_kind kind;
    enum audio_jack jack;
    struct audio_format format;
    bool available, playing;
};
#define AUDIO_SINK_BEST UINT32_MAX
unsigned audio_sinks(struct audio_sink_info *out, unsigned room);
bool audio_sink_choose(unsigned index);

/* A remote desktop client plays the sound: while one is attached the output is silent, and the
   client's sound thread mixes at the client's pace. Several may be attached; the sound goes to
   the first, and to the next when it leaves. Its thread sleeps on wq unless signals moved
   (wq_wait_unless): the mixer moves it on and wakes wq when the sound comes to this client or
   when a stream gets the PCM it waited for (audio_remote_ready), and its session may too. */
struct audio_remote {
    struct wait_queue wq;
    uint64_t signals;
    unsigned blocks; /* the most blocks of audio_remote_mix its client holds unconfirmed: set before it attaches */
    /* the mixer's, under its lock */
    struct audio_remote *next;
    int wants;      /* frames of a block it waits for, 0 when it does not */
    bool low;       /* ... and whether part of one is enough (audio_ready) */
    uint64_t given; /* the ordinal after the last block mixed for it */
};
void audio_remote_attach(struct audio_remote *r); /* in a thread that may sleep */
/* What was mixed for it and not reported played is counted played now: its client is gone. */
void audio_remote_detach(struct audio_remote *r);
bool audio_remote(void); /* a remote client is playing the sound */
/* Its sound thread, before it would mix a block: whether one is worth mixing now (audio_ready).
   When not, r is woken once it is, or when the sound comes to it. Always false while the sound
   goes to another client. */
bool audio_remote_ready(struct audio_remote *r, int frames, bool low);
/* Mix a block for r's client into out, in a stream's format; *end is the ordinal after it, which
   audio_played reports once the client says it played the block. False when nothing was mixed
   (nothing to play, or the sound goes to another client). */
bool audio_remote_mix(struct audio_remote *r, int16_t *out, int frames, uint64_t *end);
/* What plays the sound, named for people: "Remote desktop" while a remote client does, else the
   output; NULL when nothing does (the volume then changes nothing that is heard). */
const char *audio_output(void);

/* The master volume, 0-100 (setting it also unmutes), and mute: the user's. Sound holds them and
   does not save them: the settings service hears of each change (N_DOMAIN_AUDIO, sys/settings.h)
   and applies them back at the start. Any CPU; a read may be a moment old, and the watches hear of
   every change. */
int audio_volume(void);
bool audio_muted(void);
/* SYS_AUDIO_OUTPUT_WATCH: a watch (sys/watch.h) on the volume, muting and audio_output(), whoever
   changes them. */
int64_t audio_output_watch(void);

