/* Image encoding (libc/png.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* encode 0xAARRGGBB pixels (stride in pixels) as an RGB PNG; *out is malloc'd. 0 or -1 */
int png_encode(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen);
/* Straight-alpha RGBA PNG with 96 dpi metadata. The bound is exact: one
 * allocation, stored DEFLATE, and no separate image/compressor scratch.
 * A zero bound means invalid dimensions or a PNG chunk size overflow. */
size_t png_encode_rgba_bound(int w, int h);
int png_encode_rgba(const uint32_t *px, int w, int h, int stride, uint8_t **out, size_t *outlen);
/* shrink by an integer factor with box filtering; returns a malloc'd w/f x h/f image */
uint32_t *img_shrink(const uint32_t *px, int w, int h, int stride, int f, int *nw, int *nh);
/* base64 text of n bytes (NUL-terminated, malloc'd) */
char *base64_encode(const void *data, size_t n, size_t *outlen);
