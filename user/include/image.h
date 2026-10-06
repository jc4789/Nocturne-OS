/* Image decoding (libc/image.c): PNG, JPEG, GIF (first frame) and BMP through stb_image, WebP
   through jebp and SVG through nanosvg. Pixels are 0xAARRGGBB with straight (not premultiplied)
   alpha. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

typedef struct image {
    int w, h;
    uint32_t *px;
} image_t;

#define IMAGE_MAX_PIXELS (8 << 20) /* larger images are refused */

/* sniffs the format from the data; NULL if it is not an image we can decode */
image_t *image_decode(const void *data, size_t n);
/* an SVG drawn at w x h (scaled to fit, centred), or at its own size if w or h is 0 */
image_t *image_decode_svg(const char *svg, size_t n, int w, int h);
bool image_is_svg(const void *data, size_t n);
/* a resized copy (box filter when shrinking, bilinear when growing) */
image_t *image_scale(const image_t *src, int w, int h);
/* alpha-blend onto the canvas with its top-left corner at x, y */
void image_draw(canvas_t *c, const image_t *img, int x, int y);
void image_free(image_t *img);
