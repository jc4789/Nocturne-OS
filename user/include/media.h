#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

/* Nocturne native input -> FFmpeg decoder -> 48 kHz stereo / GUI pixels.
 * No subprocess, FFmpeg URL protocol, file-system emulation, or FFmpeg CLI is used.
 * A handle is single-owner; do not call it concurrently from several CPUs. */
typedef struct nmedia nmedia;
enum { NMEDIA_AGAIN = 0, NMEDIA_AUDIO = 1, NMEDIA_VIDEO = 2, NMEDIA_END = 3, NMEDIA_ERROR = -1 };
#define NMEDIA_MAX_BYTES (32u * 1024u * 1024u)
#define NMEDIA_MAX_PIXELS (2048u * 1152u)
struct nmedia_info {
    bool audio, video;
    int channels, sample_rate, width, height;
    int64_t duration_ms; /* -1 if the container cannot provide a duration */
    char audio_codec[32], video_codec[32], container[32];
};
struct nmedia_output {
    int kind;
    int64_t pts_ms;
    const int16_t *samples;
    size_t frames; /* SOUND_RATE interleaved stereo frames */
    const uint32_t *pixels; /* opaque ARGB, row-major, valid until the next step */
    int width, height;
};
nmedia *nmedia_open(const char *path, char *error, size_t error_size);
/* Anonymous HTTP(S), bounded synchronous Range cache. Large resources require
 * a strong ETag; ignored Range is accepted only when the whole object fits in
 * 256 KiB. No credentials/cookies, browser origin bypass, or async guarantee. */
nmedia *nmedia_open_url(const char *url, char *error, size_t error_size);
/* The input bytes are copied; the caller can release its buffer immediately. */
nmedia *nmedia_open_memory(const void *bytes, size_t size, char *error, size_t error_size);
const struct nmedia_info *nmedia_get_info(const nmedia *media);
int nmedia_step(nmedia *media, struct nmedia_output *output);
/* Raw FLAC without a usable seek index may restart at zero and discard its
 * prefix over bounded steps. A successful call is not necessarily fast seek. */
bool nmedia_seek(nmedia *media, int64_t ms);
const char *nmedia_error(const nmedia *media);
void nmedia_close(nmedia *media);
bool nmedia_audio_extension(const char *name);
/* Returns "maybe", "probably", or ""; never claims an unbuilt codec/MSE/DRM. */
const char *nmedia_can_play_type(const char *type);
/* Aspect-fit nearest sampling. Destination clip and physical canvas bounds are
 * both enforced, including extreme/offscreen destination rectangles. */
void nmedia_draw(canvas_t *canvas, const uint32_t *pixels, int source_width, int source_height,
                 int x, int y, int width, int height);
