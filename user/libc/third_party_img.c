/* Image codecs from third_party/img, used by image.c. Compiled without warnings.
   stb_image (public domain), jebp (MIT-0), nanosvg (zlib). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <nocturne.h>

/* stb's explicit no-TLS build has one error-state slot, shared with gzip.
 * Isolate that small codec boundary; pixel conversion/scaling is unlocked. */
static volatile uint32_t codec_lock;
void image_codec_acquire(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&codec_lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    while (__atomic_exchange_n(&codec_lock, 2, __ATOMIC_ACQUIRE)) wait_on_address(&codec_lock, 2, 100);
}
void image_codec_release(void) {
    if (__atomic_exchange_n(&codec_lock, 0, __ATOMIC_RELEASE) == 2) wake_address(&codec_lock, 1);
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

/* HTTP gzip uses the existing inflater with a hard output bound. Expose the
   consumed byte count as well: the public stb API silently ignores trailers.
   Input includes the gzip trailer, so speculative bit reads never need EOF
   padding on a valid stream. -2 means retry with a larger bounded buffer. */
static int http_inflate_unlocked(char *out, int cap, const char *in, int len, int *consumed) {
    stbi__zbuf z;
    z.zbuffer = (stbi_uc *)in;
    z.zbuffer_end = (stbi_uc *)in + len;
    /* Some invalid block types fail without setting stb's global error text. */
    stbi__g_failure_reason = NULL;
    if (!stbi__do_zlib(&z, out, cap, 0, 0)) {
        const char *reason = stbi_failure_reason();
        return reason && !strcmp(reason, "output buffer limit") ? -2 : -1;
    }
    if (z.hit_zeof_once || z.num_bits < 0) return -1;
    *consumed = (int)(z.zbuffer - (stbi_uc *)in) - z.num_bits / 8;
    return (int)(z.zout - z.zout_start);
}
int http_inflate(char *out, int cap, const char *in, int len, int *consumed) {
    image_codec_acquire();
    int result = http_inflate_unlocked(out, cap, in, len, consumed);
    image_codec_release();
    return result;
}

#define JEBP_IMPLEMENTATION
#define JEBP_NO_STDIO
#define JEBP_NO_SIMD
#include "jebp.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
