/* Image codecs from third_party/img, used by image.c. Compiled without warnings.
   stb_image (public domain), jebp (MIT-0), nanosvg (zlib). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

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
