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

#define JEBP_IMPLEMENTATION
#define JEBP_NO_STDIO
#define JEBP_NO_SIMD
#include "jebp.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
