/* Image decoding (libc/image.c): ICO (PNG/DIB), PNG, JPEG, GIF (first frame) and BMP through stb_image, WebP
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
/* Decode on a native helper; service may handle chrome/cancellation only.
 * The source remains borrowed until the synchronous join returns. */
image_t *image_decode_serviced(const void *data, size_t n, void (*service)(void *), void *context);
/* an SVG drawn at w x h (scaled to fit, centred), or at its own size if w or h is 0 */
image_t *image_decode_svg(const char *svg, size_t n, int w, int h);
bool image_is_svg(const void *data, size_t n);
/* The coverage of SVG markup's filled shapes at scale 1, in its user space: *alpha (malloc'd, *w by
   *h bytes, 0..255) holds the whole pixels from (*left, *top) that the shapes' bounds reach. NULL
   when they reach none (*w and *h 0) or more than max_pixels (the rest still their bounds). False
   without memory or for markup nanosvg cannot read. */
bool image_svg_coverage(const char *svg, size_t n, size_t max_pixels, uint8_t **alpha, int *left, int *top, int *w, int *h);
/* a resized copy (box filter when shrinking, bilinear when growing) */
image_t *image_scale(const image_t *src, int w, int h);
/* alpha-blend onto the canvas with its top-left corner at x, y */
void image_draw(canvas_t *c, const image_t *img, int x, int y);
void image_free(image_t *img);
