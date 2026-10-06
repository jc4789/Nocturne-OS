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

void ac97_init(void);
