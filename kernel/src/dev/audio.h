#pragma once
#include "kernel.h"

/* Sound. Every open of /dev/audio is a stream of 48 kHz stereo frames (two signed 16-bit
   little-endian samples, left first); writes block while the stream's buffer is full. The
   streams are mixed, scaled by the master volume (/dev/volume, 0-100) and played by one sink:
   a remote desktop client while one takes the sound, else the sound card, else nothing at all,
   at the pace a card would play it (so programs take as long as their sound does). */

#define AUDIO_RATE  48000
#define AUDIO_FRAME 4 /* bytes */

void audio_init(void);
void audio_tick(void); /* from the timer interrupt, every millisecond */

struct file;
/* Drop only this stream's not-yet-mixed PCM. Already submitted card/remote
 * sink buffers are outside this queue; ordinary close still drains. */
int audio_flush_file(struct file *file);

/* Mix `frames` frames from every stream into out (NULL: just consume them). Returns false, with
   silence in out, when no stream had anything to play. */
bool audio_mix(int16_t *out, int frames);
bool audio_pending(void); /* some stream has frames waiting */

/* A sound card: fill() tops up its buffers with audio_mix(); it is called from audio_tick(). */
void audio_set_card(const char *name, void (*fill)(void));
const char *audio_card(void); /* NULL without one */

/* A remote desktop client plays the sound: while one is attached the card is silent, and the
   client's session calls audio_mix() at its own pace. */
void audio_remote_attach(void);
void audio_remote_detach(void);
bool audio_remote(void); /* a remote client is playing the sound */

/* The master volume, 0-100 (setting it also unmutes), and mute. */
int audio_volume(void);
void audio_set_volume(int percent);
bool audio_muted(void);
void audio_set_muted(bool muted);

/* A short tone from the system itself (the volume control's tick), mixed in like a stream. It is
   dropped while the last one still sounds. */
void audio_system_tone(int hz, int ms);

void ac97_init(void);

/* Opt-in WM diagnostic only. BSP-owned, finite counters, no I/O in the
   measured interval. Queue shortages are raw observations, not EOF-aware
   underrun verdicts. This is an internal kernel interface, not a user ABI. */
extern bool audio_trace_active;
void audio_trace_begin(void);
void audio_trace_end(void);
void audio_trace_ac97(uint16_t before, uint16_t after, unsigned filled, bool restarted);
