#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "display_abi.h"
#include "edid.h"

struct fb_info {
    uint8_t *addr;
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    uint8_t rshift, gshift, bshift, rsize, gsize, bsize;
    uint8_t bpc;    /* the desktop's depth: 8, or 10 for 2:10:10:10 or while simulated */
    uint8_t native_bpc; /* smallest channel depth of the scanout itself */
    bool deep;      /* bpc >= 10: the compositor keeps a deep plane for this display */
    bool simulated; /* Settings (smooth gradients): the 10 bpc pipeline, shown dithered on an 8 bpc scanout */
    bool native;    /* the firmware's framebuffer is 32bpp xRGB: direct copy */
    /* The monitor the firmware's mode is shown on, from the EDID the firmware read from it
       (monitor "" without one), and that mode's refresh where a timing of it names that size
       (0: not known). */
    char monitor[EDID_MONITOR_ID_SIZE], monitor_name[14];
    uint32_t refresh_mhz;
};
extern struct fb_info fb;
struct limine_framebuffer;
void fb_init(struct limine_framebuffer *f);
bool fb_set_simulated(bool on); /* false: the scanout shown is not 8 bpc xRGB (fb_dither_able) */
/* Whether the scanout shown is 8 bpc xRGB, the one the 10 bpc pipeline is dithered onto. */
bool fb_dither_able(void);
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
   scanout (of any size: the mode the driver shows) in a format fb_present writes (N_SCANOUT_DIRECT
   is the only flag it may carry; what it means is the window manager's), and the bytes from
   d->offset it spans. fb_scanout_take: draw into the given pages from now on, mapped with the memory type in
   type (PTE bits, the type every other mapping of them has), the first row first_byte into the
   first page; a surface shown before is no longer mapped when it returns. fb_scanout_boot: the
   firmware's framebuffer again. Both return once no present writes to the old one any more; the
   caller has the screen drawn again in full, at the new size (fb_screen_size: the scanout shown;
   presents are clipped to it). fb.width and fb.height stay the firmware's framebuffer's. */
struct n_scanout;
int64_t fb_scanout_check(const struct n_scanout *d, uint64_t *bytes);
int64_t fb_scanout_take(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d);
void fb_scanout_boot(void);
void fb_screen_size(uint32_t *w, uint32_t *h);
bool fb_boot_shown(void); /* the firmware's framebuffer is the scanout shown (no driver's is) */
/* What the display engine may show in the scanout's place (an app's image shown directly,
   n_pci_screen_image, dev/pciuser.c, the only caller, one call at a time): fb_scanout_also maps
   the pages as fb_scanout_take would, but nothing is drawn there except the panic screen.
   fb_scanout_also_end returns once nothing writes there any more and it is no longer mapped. */
struct scanout;
int64_t fb_scanout_also(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d, struct scanout **out);
void fb_scanout_also_end(struct scanout *o);
/* The kernel's last screen (the panic screen), a 32bpp xRGB buffer of the firmware's size: into
   the scanout shown and everything fb_scanout_also added, at the top left, the rest of a larger
   one black. */
void fb_present_last(const uint32_t *src, int src_pitch, int w, int h);
/* Optional boot-time real-framebuffer test; only runs with the fbbench flag. */
void fb_benchmark(const uint32_t *src, int src_pitch);
/* Sleep (dev/pm.h): the display registers once PCI is known; it resumes only where the mode can
   be set again (in a VM, vm/vbe.c). */
void fb_pm_init(void);
