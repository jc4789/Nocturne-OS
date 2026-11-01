#pragma once
#include "media.h"
/* Single-owner 48 kHz stereo transform. PTS remains in media time, not
 * stretched device time. No thread, filesystem, or network is created. */
typedef struct nmedia_rate nmedia_rate;
nmedia_rate *nmedia_rate_create(double rate, bool preserve_pitch);
bool nmedia_rate_push(nmedia_rate *, const struct nmedia_output *);
int nmedia_rate_pull(nmedia_rate *, struct nmedia_output *);
bool nmedia_rate_end(nmedia_rate *);
void nmedia_rate_close(nmedia_rate *);
/* A sink's PCM, stretched by parts per million so that the device plays it at
 * another clock's speed. The same 48 kHz stereo on both sides. */
typedef struct nmedia_trim nmedia_trim;
nmedia_trim *nmedia_trim_create(void);
/* ppm above 0: the PCM plays that much faster. */
bool nmedia_trim_set(nmedia_trim *, int ppm);
/* Frames written to out; in NULL gives the tail it still holds. Below 0: failed. */
int nmedia_trim_convert(nmedia_trim *, const int16_t *in, size_t frames, int16_t *out, size_t capacity);
/* Frames of the input it holds that have not come out yet. */
int64_t nmedia_trim_delay(const nmedia_trim *);
void nmedia_trim_close(nmedia_trim *);
