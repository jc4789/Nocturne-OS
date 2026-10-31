#pragma once
#include "kernel.h"
#include "sys/wait.h"

/* Sound. An app's audio handle is a stream of 48 kHz stereo frames (two signed 16-bit
   little-endian samples, left first); a submit waits while the stream's buffer is full. The
   streams are mixed, scaled by the master volume (0-100) and played by one sink:
   a remote desktop client while one takes the sound, else the sound card, else nothing at all,
   at the pace a card would play it (so programs take as long as their sound does).

   No tick drives it. A sink mixes when its device needs more: the card from its own interrupt
   (or an alarm at the moment its queue falls to where it refills), a remote client's sound thread
   when the client's queue falls below its target, the sink of nothing from an alarm at the end of
   each 10 ms block on the clock (sys/clock.h). When nothing plays nothing is armed: a stream that
   gets PCM starts an idle sink at once, on any CPU.

   Every frame mixed for a sink has an ordinal, the count of frames mixed for sinks before it. A
   sink reports which it has played (audio_played), and a stream's played frames are its frames
   before the last ordinal reported, at the clock time of that report. */

#define AUDIO_RATE  48000
#define AUDIO_FRAME 4 /* bytes */

void audio_init(void);

/* The apps' streams (docs/nocturne-native-abi.md §9). The others take a handle audio_stream_is
   said yes to. */
struct handle;
struct n_audio_status;
int audio_stream_open(struct handle **out); /* N_ERR_BUSY when every stream is taken */
bool audio_stream_is(struct handle *h);
/* Any CPU: frames taken (one writer at a time), and the stream's clock. */
int64_t audio_stream_submit(struct handle *h, const void *user_frames, size_t count, bool wait);
int audio_stream_status(struct handle *h, struct n_audio_status *user_out);
/* Drop this stream's PCM at once and retire its old clock generation. What a card had mixed but
 * its device had not begun to fetch is taken back and mixed again without it (audio_set_card's
 * unissue), so after a flush the stream is heard only for what the device already holds (AC'97:
 * the rest of the playing descriptor and the prefetched one, at most 20 ms). A remote client's
 * blocks were sent and stay heard. None of it becomes the new generation's played samples.
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

/* A sound card. fill() reports what its device has played (audio_played), tops its buffers up with
   audio_card_mix() (deciding with audio_ready), and starts or stops its device. The card calls it
   when its device needs it (its interrupt, or an alarm where none tells it early enough); the mixer
   calls it when a stream gets PCM while the card has played everything it was given (an idle card
   starts at once), and after a flush. While audio_remote() it may only drain already-issued
   buffers, not mix new PCM. Called on an idle card that has nothing to start, it returns without
   device I/O. A card that reports less than all it was given will be called again by its device.
   unissue(take) is where the card's PCM that its device has not begun to fetch starts, as an
   ordinal of audio_card_mix's frames (the ordinal after the last issued frame when there is none);
   with take, the card also stops the device short of it, forgets it and lets fill() mix it again.
   The mixer takes back from that ordinal when a stream is flushed (audio_stream_flush).
   fill() and unissue() run on any CPU, one at a time, under the card's lock with interrupts off:
   they never sleep. */
void audio_set_card(const char *name, void (*fill)(void), uint64_t (*unissue)(bool take));
const char *audio_card(void); /* NULL without one */
/* Mix `frames` frames from every stream into out for the card, each stream's share remembered
   until the card reports it played or gives it back. False, with silence in out, when no stream
   had anything to play. */
bool audio_card_mix(int16_t *out, int frames);
/* The card's lock, for the card's own code that changes what fill() and unissue() read (its
   interrupt, its power changes): neither runs while it is held. It comes before the mixer's lock,
   so audio_card_mix and the others may be called under it. Never held across a sleep or a long
   wait. */
uint64_t audio_card_lock(void);
void audio_card_unlock(uint64_t flags);

/* A remote desktop client plays the sound: while one is attached the card is silent, and the
   client's sound thread mixes at the client's pace. Several may be attached; the sound goes to
   the first, and to the next when it leaves. Its thread sleeps on wq unless signals moved
   (wq_wait_unless): the mixer moves it on and wakes wq when the sound comes to this client or
   when a stream gets the PCM it waited for (audio_remote_ready), and its session may too. */
struct audio_remote {
    struct wait_queue wq;
    uint64_t signals;
    /* the mixer's, under its lock */
    struct audio_remote *next;
    int wants;      /* frames of a block it waits for, 0 when it does not */
    bool low;       /* ... and whether part of one is enough (audio_ready) */
    uint64_t given; /* the ordinal after the last block mixed for it */
};
void audio_remote_attach(struct audio_remote *r);
/* What was mixed for it and not reported played is counted played now: its client is gone. */
void audio_remote_detach(struct audio_remote *r);
bool audio_remote(void); /* a remote client is playing the sound */
/* Its sound thread, before it would mix a block: whether one is worth mixing now (audio_ready).
   When not, r is woken once it is, or when the sound comes to it. Always false while the sound
   goes to another client. */
bool audio_remote_ready(struct audio_remote *r, int frames, bool low);
/* Mix a block for r's client into out, as audio_card_mix; *end is the ordinal after it, which
   audio_played reports once the client says it played the block. False when nothing was mixed
   (nothing to play, or the sound goes to another client). */
bool audio_remote_mix(struct audio_remote *r, int16_t *out, int frames, uint64_t *end);
/* What plays the sound, named for people: "Remote desktop" while a remote client does, else the
   sound card; NULL when nothing does (the volume then changes nothing that is heard). */
const char *audio_output(void);

/* The master volume, 0-100 (setting it also unmutes), and mute: the user's. Sound keeps them in
   the user's AppData\Nocturne\Sound.conf (sys/keep.c) and reads them back at the start. Any CPU;
   a read may be a moment old, and the watches hear of every change. */
int audio_volume(void);
void audio_set_volume(int percent);
bool audio_muted(void);
void audio_set_muted(bool muted);
/* SYS_AUDIO_OUTPUT_WATCH: a watch (sys/watch.h) on the volume, muting and audio_output(), whoever
   changes them. */
int64_t audio_output_watch(void);

/* A short tone from the system itself (the volume control's tick), mixed in like a stream. It is
   dropped while the last one still sounds. */
void audio_system_tone(int hz, int ms);


/* Opt-in WM diagnostic only: finite counters, no I/O in the measured interval. Queue shortages are
   raw observations, not EOF-aware underrun verdicts. This is an internal kernel interface, not a
   user ABI. audio_trace_active changes under both the card's lock and the mixer's; a card reads it
   and calls audio_trace_ac97 from fill(). */
extern bool audio_trace_active;
void audio_trace_begin(void);
void audio_trace_end(void);
void audio_trace_ac97(uint16_t before, uint16_t after, unsigned filled, bool restarted);
