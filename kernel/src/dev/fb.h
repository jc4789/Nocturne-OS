#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
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
   writes nothing until the last release; no time limit ends a hold. fb_hold returns once no
   present is under way. fb_release returns true when it ended the last hold, after which its
   caller has the screen drawn again in full. fb_drop_holds: the kernel stopped, the panic screen
   is drawn. */
void fb_hold(void);
bool fb_release(void);
void fb_drop_holds(void);
/* Where fb_present writes (dev/fb.c, "where the screen is drawn"): the firmware's framebuffer
   (fb.addr) until a display driver gives the system its own scanout (n_pci_give_screen,
   dev/pciuser.c, the only caller, one call at a time). fb_scanout_check: whether d describes a
   scanout of this screen's size in a format fb_present writes, and the bytes from d->offset it
   spans. fb_scanout_take: draw into the given pages from now on, mapped with the memory type in
   type (PTE bits, the type every other mapping of them has), the first row first_byte into the
   first page; a surface shown before is no longer mapped when it returns. fb_scanout_boot: the
   firmware's framebuffer again. Both return once no present writes to the old one any more; the
   caller has the screen drawn again in full. */
struct n_scanout;
int64_t fb_scanout_check(const struct n_scanout *d, uint64_t *bytes);
int64_t fb_scanout_take(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d);
void fb_scanout_boot(void);
/* Optional boot-time real-framebuffer test; only runs with the fbbench flag. */
void fb_benchmark(const uint32_t *src, int src_pitch);
/* Sleep (dev/pm.h): the display registers once PCI is known; it resumes only where the mode can
   be set again (in a VM, vm/vbe.c). */
void fb_pm_init(void);
