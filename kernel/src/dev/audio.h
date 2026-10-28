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
void audio_tick(void); /* from the timer interrupt, every millisecond */

/* The apps' streams (docs/nocturne-native-abi.md §9). The others take a handle audio_stream_is
   said yes to. */
struct handle;
struct n_audio_status;
int audio_stream_open(struct handle **out); /* N_ERR_BUSY when every stream is taken */
bool audio_stream_is(struct handle *h);
int64_t audio_stream_submit(struct handle *h, const void *user_frames, size_t count, bool wait); /* frames taken */
int audio_stream_status(struct handle *h, struct n_audio_status *user_out);
/* The same on an AP inside an AP read section, for any handle h: true with *result when served
   there, false when the BSP must (not a stream, the mixer kept changing it, out not mapped). */
bool audio_stream_status_ap(struct handle *h, struct n_audio_status *user_out, int64_t *result);
/* Drop this stream's not-yet-mixed PCM and retire its old clock generation.
 * Already mixed card/remote samples cannot be unmixed here, but they never
 * become the new generation's played samples. Ordinary close drains DMA too. */
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
bool audio_needs_tick(void); /* tickless idle: the timer must keep calling audio_tick */

/* A sound card: fill() tops up its buffers with audio_mix(); it is called from audio_tick().
   While audio_remote() it may only drain already-issued buffers, not mix new PCM.
   An idle sink should return without device I/O; the next tick wakes it on new PCM. */
void audio_set_card(const char *name, void (*fill)(void));
const char *audio_card(void); /* NULL without one */

/* A remote desktop client plays the sound: while one is attached the card is silent, and the
   client's session calls audio_mix() at its own pace. */
void audio_remote_attach(void);
void audio_remote_detach(void);
bool audio_remote(void); /* a remote client is playing the sound */
/* What plays the sound, named for people: "Remote desktop" while a remote client does, else the
   sound card; NULL when nothing does (the volume then changes nothing that is heard). */
const char *audio_output(void);

/* The master volume, 0-100 (setting it also unmutes), and mute: the user's. Sound keeps them in
   the user's AppData\Nocturne\Sound.conf (sys/keep.c) and reads them back at the start. BSP. */
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


/* Opt-in WM diagnostic only. BSP-owned, finite counters, no I/O in the
   measured interval. Queue shortages are raw observations, not EOF-aware
   underrun verdicts. This is an internal kernel interface, not a user ABI. */
extern bool audio_trace_active;
void audio_trace_begin(void);
void audio_trace_end(void);
void audio_trace_ac97(uint16_t before, uint16_t after, unsigned filled, bool restarted);
