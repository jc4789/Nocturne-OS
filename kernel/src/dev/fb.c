#include "kernel.h"
#include "dev/fb.h"
#include "limine.h"

struct fb_info fb;

void fb_init(struct limine_framebuffer *f) {
    fb.addr = f->address;
    fb.phys = (uint64_t)f->address - hhdm_offset;
    fb.width = f->width;
    fb.height = f->height;
    fb.pitch = f->pitch;
    fb.bpp = f->bpp;
    fb.rshift = f->red_mask_shift;
    fb.gshift = f->green_mask_shift;
    fb.bshift = f->blue_mask_shift;
    fb.rsize = f->red_mask_size;
    fb.gsize = f->green_mask_size;
    fb.bsize = f->blue_mask_size;
    fb.native = fb.bpp == 32 && fb.rshift == 16 && fb.gshift == 8 && fb.bshift == 0;
}

static inline uint32_t convert(uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return ((r >> (8 - fb.rsize)) << fb.rshift) | ((g >> (8 - fb.gsize)) << fb.gshift) |
           ((b >> (8 - fb.bsize)) << fb.bshift);
}

void fb_present(const uint32_t *src, int src_pitch, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb.width) w = fb.width - x;
    if (y + h > (int)fb.height) h = fb.height - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const uint32_t *s = &src[(y + j) * src_pitch + x];
        uint8_t *d = fb.addr + (uint64_t)(y + j) * fb.pitch;
        if (fb.native) {
            memcpy(d + x * 4, s, (size_t)w * 4);
        } else if (fb.bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + x;
            for (int i = 0; i < w; i++) d32[i] = convert(s[i]);
        } else if (fb.bpp == 24) {
            uint8_t *d8 = d + x * 3;
            for (int i = 0; i < w; i++) {
                uint32_t v = convert(s[i]);
                d8[i * 3] = v & 0xFF;
                d8[i * 3 + 1] = (v >> 8) & 0xFF;
                d8[i * 3 + 2] = (v >> 16) & 0xFF;
            }
        } else if (fb.bpp == 16) {
            uint16_t *d16 = (uint16_t *)d + x;
            for (int i = 0; i < w; i++) d16[i] = (uint16_t)convert(s[i]);
        }
    }
}
