#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "display_abi.h"

struct fb_info {
    uint8_t *addr;
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    uint8_t rshift, gshift, bshift, rsize, gsize, bsize;
    uint8_t bpc;    /* the desktop's depth: 8, or 10 for 2:10:10:10 or while simulated */
    uint8_t native_bpc; /* smallest channel depth of the scanout itself */
    bool deep;      /* bpc >= 10: the compositor keeps a deep plane for this display */
    bool simulated; /* Settings: the 10 bpc pipeline, shown dithered on an 8 bpc scanout */
    bool native;    /* 32bpp xRGB: direct copy */
};
extern struct fb_info fb;
struct limine_framebuffer;
void fb_init(struct limine_framebuffer *f);
bool fb_set_simulated(bool on); /* false: the scanout is not 8 bpc xRGB */
/* Copy a rectangle of a full-screen 32bpp xRGB buffer to the screen. deep is
   that buffer's deep plane (display_abi.h) or NULL for 8-bit content. */
void fb_present(const uint32_t *src, int src_pitch, const uint8_t *deep, int deep_pitch,
                int x, int y, int w, int h);
/* A display driver rebuilding how the CPU reaches the screen (dev/pciuser.c) holds it: fb_present
   writes nothing until the last release, or until the hold outlives its limit (boot option
   fbhold=<seconds>, 30 by default, 0 for none). fb_hold returns once no present is under way.
   fb_release returns true when it ended a hold that kept frames off the screen, which its caller
   then has drawn again in full; fb_take_expiry is true once after a hold ran out, for the
   compositor to do the same. fb_drop_holds: the kernel stopped, the panic screen is drawn. */
void fb_hold(void);
bool fb_release(void);
bool fb_take_expiry(void);
void fb_drop_holds(void);
/* Optional boot-time real-framebuffer test; only runs with the fbbench flag. */
void fb_benchmark(const uint32_t *src, int src_pitch);
/* Sleep (dev/pm.h): the display registers once PCI is known; it resumes only where the mode can
   be set again (the Bochs VBE interface). */
void fb_pm_init(void);
