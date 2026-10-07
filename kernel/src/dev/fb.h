#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

struct fb_info {
    uint8_t *addr;
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    uint8_t rshift, gshift, bshift, rsize, gsize, bsize;
    bool native; /* 32bpp xRGB: direct copy */
};
extern struct fb_info fb;
struct limine_framebuffer;
void fb_init(struct limine_framebuffer *f);
/* copy a rectangle of a full-screen 32bpp xRGB buffer to the screen */
void fb_present(const uint32_t *src, int src_pitch, int x, int y, int w, int h);
/* Optional boot-time real-framebuffer test; only runs with the fbbench flag. */
void fb_benchmark(const uint32_t *src, int src_pitch);
