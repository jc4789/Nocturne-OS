#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"

/* Nocturne native input -> FFmpeg decoder -> 48 kHz stereo / GUI pixels.
 * No FFmpeg URL protocol, file-system emulation, or FFmpeg CLI is used.
 * The Browser can isolate this same decoder in its native media worker.
 * A handle is single-owner; do not call it concurrently from several CPUs. */
typedef struct nmedia nmedia;
enum { NMEDIA_AGAIN = 0, NMEDIA_AUDIO = 1, NMEDIA_VIDEO = 2, NMEDIA_END = 3, NMEDIA_ERROR = -1 };
#define NMEDIA_MAX_BYTES (32u * 1024u * 1024u)
struct nmedia_info {
    bool audio, video;
    int channels, sample_rate, width, height;
    int64_t duration_ms; /* -1 if the container cannot provide a duration */
    char audio_codec[32], video_codec[32], container[32];
};
enum { NMEDIA_VCODEC_NONE, NMEDIA_VCODEC_AVC, NMEDIA_VCODEC_HEVC, NMEDIA_VCODEC_VP8, NMEDIA_VCODEC_VP9 };
struct nmedia_video_configuration {
    uint32_t codec,profile,level,depth,compatibility;
    uint64_t bitrate;
};
static inline bool nmedia_video_configuration_equal(const struct nmedia_video_configuration *a,const struct nmedia_video_configuration *b){
    return a->codec==b->codec&&a->profile==b->profile&&a->level==b->level&&a->depth==b->depth&&a->compatibility==b->compatibility;
}
enum { NMEDIA_ACODEC_NONE,NMEDIA_ACODEC_OPUS,NMEDIA_ACODEC_VORBIS,NMEDIA_ACODEC_MP3,NMEDIA_ACODEC_AAC };
struct nmedia_audio_observation {
    uint64_t id,frames,work_ms; /* frames are real converted 48 kHz stereo */
    uint32_t codec,profile,channels,rate;
};
struct nmedia_output {
    int kind;
    int64_t pts_ms;
    int64_t duration_ms; /* actual decoded frame span; zero if unknown */
    const int16_t *samples;
    size_t frames; /* SOUND_RATE interleaved stereo frames */
    const uint32_t *pixels; /* opaque ARGB, row-major, valid until the next step */
    int width, height;
    /* Successful native video conversions only; decode/conversion work,
     * excluding demux, transport and presentation waits. */
    uint64_t video_measured_frames, video_work_ms;
    uint64_t video_measurement_id;
    struct nmedia_video_configuration video_configuration;
    struct nmedia_audio_observation audio_observation;
    /* With nmedia_set_deep and a source of more than 8 bits per sample: the
       low bits of each pixel (display_abi.h deep plane, pitch width), pixels
       holding the top 8 bits. Otherwise NULL, and such sources are ordered-
       dithered to 8 bits; 8-bit sources are converted exactly as before. */
    const uint8_t *deep;
    /* With nmedia_set_audio_native, audio comes as decoded instead of samples:
       frames of native_channels float planes at native_rate (valid until the
       next step). */
    const float *const *native_planes;
    int native_channels, native_rate;
};
nmedia *nmedia_open(const char *path, char *error, size_t error_size);
/* Anonymous HTTP(S), bounded synchronous Range cache. Large resources require
 * a strong ETag; ignored Range is accepted only when the whole object fits in
 * 256 KiB. No credentials/cookies, browser origin bypass, or async guarantee. */
nmedia *nmedia_open_url(const char *url, char *error, size_t error_size);
/* Browser-only anonymous policy: document_url must be the native initiating
 * document, never an origin supplied by page JS. Every cross-origin response is
 * CORS-checked; nonsafelisted If-Range refills are preflighted. Redirects and
 * HTTPS mixed content are rejected.
 * Synchronous input, no cookies/authentication or async cancellation promise. */
nmedia *nmedia_open_url_cors(const char *url, const char *document_url,
                           char *error, size_t error_size);
/* The input bytes are copied; the caller can release its buffer immediately. */
nmedia *nmedia_open_memory(const void *bytes, size_t size, char *error, size_t error_size);
const struct nmedia_info *nmedia_get_info(const nmedia *media);
int nmedia_step(nmedia *media, struct nmedia_output *output);
/* Raw FLAC without a usable seek index may restart at zero and discard its
 * prefix over bounded steps. A successful call is not necessarily fast seek. */
bool nmedia_seek(nmedia *media, int64_t ms);
const char *nmedia_error(const nmedia *media);
void nmedia_close(nmedia *media);
/* Deliver high-bit-depth video as 10-bit deep frames instead of dithering it
   to 8 bits at decode (the default). For a drawer that scales: it keeps 10
   bits through nmedia_draw_deep and dithers where the pixel lands on an 8 bpc
   display, or keeps them on a 10 bpc one. */
void nmedia_set_deep(nmedia *media, bool deep);
/* Deliver audio as it was decoded, with no mixing to stereo and no resampling
   to SOUND_RATE (output native_planes): what decodeAudioData keeps. */
void nmedia_set_audio_native(nmedia *media, bool native);
bool nmedia_audio_extension(const char *name);
/* Returns "maybe", "probably", or ""; never claims an unbuilt codec/MSE/DRM. */
const char *nmedia_can_play_type(const char *type);
/* Exact single-video observation key; false for omitted or unobserved
   profiles/depth. A decoder name alone cannot identify a cost sample. */
bool nmedia_performance_codec(const char *type,struct nmedia_video_configuration *);
bool nmedia_performance_audio(const char *type,uint32_t *codec,uint32_t *profile);
/* Aspect-fit bilinear sampling. Destination clip and physical canvas bounds are
 * both enforced, including extreme/offscreen destination rectangles. */
/* nmedia_draw interpolating a deep frame's 10-bit channels: into the canvas's
 * deep plane (canvas_deep, pitch canvas->pitch) when it has one, otherwise
 * ordered-dithered to 8 bits after scaling. Without a frame deep plane it is
 * exactly nmedia_draw. */
void nmedia_draw_deep(canvas_t *canvas, uint8_t *canvas_deep, const uint32_t *pixels, const uint8_t *deep,
                      int source_width, int source_height, int x, int y, int width, int height);
void nmedia_draw(canvas_t *canvas, const uint32_t *pixels, int source_width, int source_height,
                 int x, int y, int width, int height);
