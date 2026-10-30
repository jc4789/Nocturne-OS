#pragma once
#include "kernel.h"

/* Sound. An app's audio handle is a stream of 48 kHz stereo frames (two signed 16-bit
   little-endian samples, left first); a submit waits while the stream's buffer is full. The
   streams are mixed, scaled by the master volume (0-100) and played by one sink:
   a remote desktop client while one takes the sound, else the sound card, else nothing at all,
   at the pace a card would play it (so programs take as long as their sound does). */

#define AUDIO_RATE  48000
#define AUDIO_FRAME 4 /* bytes */

void audio_init(void);
void audio_tick(void); /* from CPU 0's timer interrupt, every millisecond */

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

/* Mix `frames` frames from every stream into out (NULL: just consume them). Returns false, with
   silence in out, when no stream had anything to play. */
bool audio_mix(int16_t *out, int frames);
/* Card-only ownership: record each stream's actual contribution, then retire
 * it when the cached DMA consumption ordinal advances. Ordinary remote mix
 * does not pretend that a server packet is an audible device completion. */
bool audio_card_mix(int16_t *out, int frames);
void audio_card_played(uint64_t ordinal);
bool audio_pending(void); /* some stream has frames waiting */
/* Whether a sink should mix a block of `frames` now: some stream has PCM, and every stream that
   has any has a whole block of it, or the sink's device is about to run dry (`low`). A block mixed
   from a stream that has only part of one plays the rest as silence in the middle of its sound
   when its next PCM comes a moment later; while the device has enough queued, waiting does not. */
bool audio_ready(int frames, bool low);
bool audio_needs_tick(void); /* tickless idle: the timer must keep calling audio_tick */

/* A sound card: fill() tops up its buffers with audio_card_mix() (deciding with audio_ready); it is
   called from audio_tick() and after a flush. While audio_remote() it may only drain already-issued
   buffers, not mix new PCM. An idle sink should return without device I/O; the next tick wakes it
   on new PCM. unissue(take) is where the card's PCM that its device has not begun to fetch starts,
   as an ordinal of audio_card_mix's frames (the ordinal after the last issued frame when there is
   none); with take, the card also stops the device short of it, forgets it and lets fill() mix it
   again. The mixer takes back from that ordinal when a stream is flushed (audio_stream_flush).
   fill() and unissue() run on any CPU, one at a time, under the card's lock with interrupts off:
   they never sleep. */
void audio_set_card(const char *name, void (*fill)(void), uint64_t (*unissue)(bool take));
const char *audio_card(void); /* NULL without one */
/* The card's lock, for the card's own code that changes what fill() and unissue() read (its power
   changes): neither runs while it is held. It comes before the mixer's lock, so audio_card_mix and
   the others may be called under it. Never held across a sleep or a long wait. */
uint64_t audio_card_lock(void);
void audio_card_unlock(uint64_t flags);

/* A remote desktop client plays the sound: while one is attached the card is silent, and the
   client's session calls audio_mix() at its own pace. */
void audio_remote_attach(void);
void audio_remote_detach(void);
bool audio_remote(void); /* a remote client is playing the sound */
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
