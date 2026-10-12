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
