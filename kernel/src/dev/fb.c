#include "kernel.h"
#include "dev/fb.h"
#include "limine.h"
#include "arch/smp.h"
#include "arch/cpu.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/sched.h"
#include "vm/vm.h"

struct fb_info fb;
/* Boot options fbparallel and fbbench: presents are drawn by the idle CPUs as well (present_area). */
static bool present_parallel;

/* ---- where the screen is drawn ----
   A scanout is memory the display engine shows, in one pixel format. At boot it is the
   firmware's framebuffer (fb.addr); a display driver in an app that set up its own scanout gives
   the system that surface instead (n_pci_give_screen, dev/pciuser.c), and from then on nothing
   is written to the firmware's until the driver gives it back or its claim ends. A driver's
   scanout has the size of the mode it shows, which need not be the firmware's: each scanout
   keeps its own, presents are clipped to the one shown, and the desktop follows it
   (wm_screen_resized).
   Any Limine/GOP bit-mask format: a 10-bit channel value -> that channel's bits at their
   position, rounded to the channel's own depth (1..16 bits). 8-bit content enters widened by bit
   replication, so 8 bpc XRGB8888 maps every value to itself and 10 bpc 2:10:10:10 shows 255 as
   1023. */
struct scanout {
    uint8_t *addr;  /* the first row */
    void *map;      /* a driver's surface: the kernel mapping addr lies in (vunmap), else NULL */
    size_t pages;
    uint32_t width, height, pitch, bpp;
    bool native;    /* 32bpp xRGB8888: rows are copied */
    struct scanout *next; /* fb_scanout_also's list */
    uint32_t lut_r[1024], lut_g[1024], lut_b[1024];
};
/* The firmware's, and a driver's: the one shown and the one replacing it. */
static struct scanout scanouts[3];
static struct scanout *shown = &scanouts[0]; /* atomic: where fb_present writes */

static void build_lut(uint32_t *lut, uint8_t *size, uint8_t shift, uint32_t bpp) {
    if (*size > 16) *size = 16;
    if (shift >= bpp || shift + *size > bpp) *size = 0; /* a mask outside the pixel shows nothing */
    uint32_t max = (1u << *size) - 1;
    for (unsigned v = 0; v < 1024; v++) lut[v] = (v * max + 511) / 1023 << shift;
}

/* The firmware's monitor (fb.monitor): its ids, and the refresh of the firmware's mode: the first
   detailed timing of that size (the native one comes first), else the nominal rate nearest 60 Hz
   of a standard or established timing of that size, as firmware sets its other modes. */
static uint32_t from_60hz(uint32_t mhz) { return mhz > 60000 ? mhz - 60000 : 60000 - mhz; }

static void fb_monitor(const uint8_t *e, size_t n) {
    struct edid_ids id;
    if (!edid_valid(e, n) || !edid_ids(e, n, &id)) return;
    edid_monitor_id(&id, fb.monitor);
    memcpy(fb.monitor_name, id.name, sizeof fb.monitor_name);
    struct edid_mode modes[32];
    unsigned count = edid_modes(e, modes, 32);
    const struct edid_mode *best = NULL;
    for (unsigned i = 0; i < count; i++) {
        const struct edid_mode *m = &modes[i];
        if (m->width != fb.width || m->height != fb.height) continue;
        if (m->detailed) {
            best = m;
            break;
        }
        if (!best || from_60hz(m->refresh_mhz) < from_60hz(best->refresh_mhz)) best = m;
    }
    fb.refresh_mhz = best ? best->refresh_mhz : 0;
}

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
    if (fb.bpp != 16 && fb.bpp != 24 && fb.bpp != 32) fb.rsize = fb.gsize = fb.bsize = 0;
    struct scanout *o = &scanouts[0];
    build_lut(o->lut_r, &fb.rsize, fb.rshift, fb.bpp);
    build_lut(o->lut_g, &fb.gsize, fb.gshift, fb.bpp);
    build_lut(o->lut_b, &fb.bsize, fb.bshift, fb.bpp);
    fb.bpc = MIN(fb.rsize, MIN(fb.gsize, fb.bsize));
    fb.native = fb.bpp == 32 && fb.rshift == 16 && fb.gshift == 8 && fb.bshift == 0 &&
                fb.rsize == 8 && fb.gsize == 8 && fb.bsize == 8;
    fb.native_bpc = fb.bpc;
    o->addr = fb.addr;
    o->width = fb.width;
    o->height = fb.height;
    o->pitch = fb.pitch;
    o->bpp = fb.bpp;
    o->native = fb.native;
    fb_monitor(f->edid, f->edid_size);
    fb_set_simulated(cmdline_has("fbsim10"));
    present_parallel = cmdline_has("fbparallel") || cmdline_has("fbbench");
}

bool fb_dither_able(void) { return __atomic_load_n(&shown, __ATOMIC_ACQUIRE)->native; }

/* Run the whole 10 bpc pipeline on an 8 bpc display and dither it at scanout (smooth gradients).
   Only the window manager calls this after boot, between frames under its lock (which every
   reader of fb.simulated, fb.bpc and fb.deep after boot holds), and then reallocates its planes;
   it turns it off when a scanout of another format is shown. */
bool fb_set_simulated(bool on) {
    if (on && !fb_dither_able()) return false;
    fb.simulated = on;
    fb.bpc = on ? 10 : fb.native_bpc;
    fb.deep = fb.bpc >= 10;
    return true;
}

static inline unsigned channel10(uint32_t c, unsigned shift, uint8_t deep, unsigned deep_shift) {
    unsigned v = (c >> shift) & 0xFF;
    return deep & DEEP_VALID ? v << 2 | ((deep >> deep_shift) & 3) : deep_widen(v);
}

static inline uint32_t convert(const struct scanout *o, uint32_t c, uint8_t deep) {
    return o->lut_r[channel10(c, 16, deep, 4)] | o->lut_g[channel10(c, 8, deep, 2)] |
           o->lut_b[channel10(c, 0, deep, 0)];
}

/* 10 -> 8 bits with the 4x4 ordered dither: +1 with probability frac/4. */
static inline unsigned dither8(unsigned v10, unsigned threshold) {
    unsigned v = (v10 * 4 + threshold) >> 4;
    return v > 255 ? 255 : v;
}

struct present_job {
    const struct scanout *out;
    const uint32_t *src; const uint8_t *deep; int pitch, deep_pitch, x, y, w, h;
};
static void present_area(const struct scanout *out, const uint32_t *src, int src_pitch, const uint8_t *deep,
                         int deep_pitch, int x, int y, int w, int h);

/* Disjoint row tiles only. No allocation, scheduling, device commands or FPU.
 * The framebuffer writes remain scalar: WHPX does not emulate SIMD MMIO. The 10 bpc simulation
 * dithers only onto an 8 bpc xRGB scanout; onto another the deep plane is converted as it is. */
static void present_rows(size_t tile, void *context) {
    const struct present_job *p = context;
    const struct scanout *o = p->out;
    int first=(int)tile*16, end=MIN(first+16,p->h);
    for (int j = first; j < end; j++) {
        const uint32_t *s = &p->src[(size_t)(p->y + j) * p->pitch + p->x];
        const uint8_t *e = p->deep ? p->deep + (size_t)(p->y + j) * p->deep_pitch + p->x : NULL;
        uint8_t *d = o->addr + (uint64_t)(p->y + j) * o->pitch;
        if (o->native && !e) {
            memcpy(d + p->x * 4, s, (size_t)p->w * 4);
        } else if (fb.simulated && o->native) {
            uint32_t *d32 = (uint32_t *)d + p->x;
            for (int i = 0; i < p->w; i++) {
                if (!(e[i] & DEEP_VALID)) { d32[i] = s[i]; continue; }
                unsigned t = dither_threshold((unsigned)(p->x + i), (unsigned)(p->y + j));
                d32[i] = dither8(channel10(s[i], 16, e[i], 4), t) << 16 |
                         dither8(channel10(s[i], 8, e[i], 2), t) << 8 | dither8(channel10(s[i], 0, e[i], 0), t);
            }
        } else if (o->bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + p->x;
            if (e) for (int i = 0; i < p->w; i++) d32[i] = convert(o, s[i], e[i]);
            else for (int i = 0; i < p->w; i++) d32[i] = convert(o, s[i], 0);
        } else if (o->bpp == 24) {
            uint8_t *d8 = d + p->x * 3;
            for (int i = 0; i < p->w; i++) {
                uint32_t v = convert(o, s[i], e ? e[i] : 0);
                d8[i * 3] = v & 0xFF;
                d8[i * 3 + 1] = (v >> 8) & 0xFF;
                d8[i * 3 + 2] = (v >> 16) & 0xFF;
            }
        } else if (o->bpp == 16) {
            uint16_t *d16 = (uint16_t *)d + p->x;
            for (int i = 0; i < p->w; i++) d16[i] = (uint16_t)convert(o, s[i], e ? e[i] : 0);
        }
    }
}

/* ---- holding the screen ----
   A display driver that rebuilds the way the CPU reaches the framebuffer (NVIDIA's RM switches
   the BAR the boot screen lies in to virtual mode during its start and stop) asks that nothing is
   written there meanwhile. While a hold lasts, fb_present writes nothing: the compositor and the
   console keep drawing into their RAM canvases, and the whole screen is drawn again when the last
   hold ends. Time never ends a hold: drawing while the driver still rebuilds is the very write
   the hold keeps out. A hold ends when the driver gives it back, or when its claim ends
   (dev/pciuser.c), which stops the device before it gives the holds back; that is also how a
   driver stuck while holding is ended (pciuser.c, boot option fbhold=). A present runs on any CPU
   (the compositor's thread, the console) while a hold or a change of scanout comes on another
   (fbparallel tiles join before the present returns): a present counts itself in presenting before
   it looks at hold_count and shown, a hold or a change of scanout changes those before it waits
   for presenting to reach 0 (all four sequentially consistent), so either the present sees the
   change or the change waits for the present. The panic screen is drawn whatever is held, on the
   scanout shown and on what the display engine may show in its place (fb_present_last). */
static unsigned hold_count;    /* atomic */
static unsigned presenting;    /* atomic: fb_present calls between their check and return */
static uint64_t hold_since_ms; /* atomic: when hold_count left 0 */
static bool holds_dropped;     /* the kernel stopped (panic, every other CPU stopped): nothing is held any more */

/* Returns once every present that began before it has ended. */
static void presents_drain(void) {
    while (__atomic_load_n(&presenting, __ATOMIC_SEQ_CST)) sleep_ms(1);
    __asm__ volatile("sfence" ::: "memory"); /* write-combined pixels already sent leave the CPU now */
}

void fb_hold(void) {
    if (__atomic_fetch_add(&hold_count, 1, __ATOMIC_SEQ_CST) == 0) {
        __atomic_store_n(&hold_since_ms, uptime_ms(), __ATOMIC_RELAXED);
        kprintf("fb: a display driver holds the screen\n");
    }
    presents_drain();
}

bool fb_release(void) {
    unsigned before = __atomic_load_n(&hold_count, __ATOMIC_ACQUIRE);
    while (before && !__atomic_compare_exchange_n(&hold_count, &before, before - 1, false, __ATOMIC_ACQ_REL,
                                                  __ATOMIC_ACQUIRE)) {
    }
    if (before != 1) return false;
    kprintf("fb: the screen was released after %lu ms\n",
            (unsigned long)(uptime_ms() - __atomic_load_n(&hold_since_ms, __ATOMIC_RELAXED)));
    return true;
}

static bool hold_blocks(void) {
    return !holds_dropped && __atomic_load_n(&hold_count, __ATOMIC_SEQ_CST);
}

void fb_drop_holds(void) { holds_dropped = true; }

/* Shows o: the presents that begin after this write to it. Once the presents that began before
   have ended, the driver's surface shown until now is no longer mapped. */
static void scanout_show(struct scanout *o) {
    struct scanout *old = __atomic_exchange_n(&shown, o, __ATOMIC_SEQ_CST);
    presents_drain();
    if (old == o || !old->map) return;
    vunmap(old->map, old->pages);
    old->map = NULL;
    old->addr = NULL;
}

int64_t fb_scanout_check(const struct n_scanout *d, uint64_t *bytes) {
    /* Any size a mode has (a display engine's sizes are 16 bits); whether the desktop's planes fit in
       memory at it is the window manager's to say (wm_screen_resized). */
    if (!fb.addr || !d->width || !d->height || d->width > UINT16_MAX || d->height > UINT16_MAX)
        return -N_ERR_INVALID;
    uint32_t format = d->format & N_SCANOUT_FORMAT_MASK;
    if (d->format & ~(N_SCANOUT_FORMAT_MASK | N_SCANOUT_DIRECT)) return -N_ERR_INVALID;
    if (format != N_SCANOUT_XRGB8888 && format != N_SCANOUT_XRGB2101010 && format != N_SCANOUT_XBGR8888)
        return -N_ERR_INVALID;
    if (d->pitch < (uint64_t)d->width * 4 || d->pitch % 4) return -N_ERR_INVALID;
    uint64_t rows = (uint64_t)d->pitch * d->height;
    if (d->offset % 4 || d->offset > UINT64_MAX - rows) return -N_ERR_INVALID;
    *bytes = rows;
    return 0;
}

/* o drawn into the given pages in d's format (fb_scanout_check passed). */
static int64_t scanout_map(struct scanout *o, const uint64_t *frames, size_t pages, uint64_t type,
                           size_t first_byte, const struct n_scanout *d) {
    void *map = vmalloc_map_frames(frames, pages, PTE_W | type | pte_nx);
    if (!map) return -N_ERR_NO_MEMORY;
    uint32_t format = d->format & N_SCANOUT_FORMAT_MASK;
    bool deep = format == N_SCANOUT_XRGB2101010, bgr = format == N_SCANOUT_XBGR8888;
    uint8_t rsize = deep ? 10 : 8, gsize = rsize, bsize = rsize;
    build_lut(o->lut_r, &rsize, deep ? 20 : bgr ? 0 : 16, 32);
    build_lut(o->lut_g, &gsize, deep ? 10 : 8, 32);
    build_lut(o->lut_b, &bsize, bgr ? 16 : 0, 32);
    o->map = map;
    o->pages = pages;
    o->addr = (uint8_t *)map + first_byte;
    o->width = d->width;
    o->height = d->height;
    o->pitch = d->pitch;
    o->bpp = 32;
    o->native = format == N_SCANOUT_XRGB8888;
    return 0;
}

int64_t fb_scanout_take(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d) {
    struct scanout *o = &scanouts[__atomic_load_n(&shown, __ATOMIC_ACQUIRE) == &scanouts[1] ? 2 : 1];
    ASSERT(!o->map);
    int64_t r = scanout_map(o, frames, pages, type, first_byte, d);
    if (!r) scanout_show(o);
    return r;
}

void fb_scanout_boot(void) { scanout_show(&scanouts[0]); }

bool fb_boot_shown(void) { return __atomic_load_n(&shown, __ATOMIC_ACQUIRE) == &scanouts[0]; }

void fb_screen_size(uint32_t *w, uint32_t *h) {
    const struct scanout *o = __atomic_load_n(&shown, __ATOMIC_ACQUIRE);
    *w = o->width;
    *h = o->height;
}

/* ---- what the display engine may show in the scanout's place ----
   A driver that shows an app's window directly (N_SCANOUT_DIRECT, gui/wm.c) flips the display
   engine from its scanout to the app's own images, which nothing of the system's is drawn into.
   When the kernel stops, the engine goes on showing whichever was flipped last, and no driver runs
   any more to flip back: the panic screen is drawn into each of them as well (fb_present_last).
   Nothing else writes there. dev/pciuser.c adds and ends them, one call at a time. */
static struct scanout *alsos; /* atomic: the list fb_present_last walks */

int64_t fb_scanout_also(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                        const struct n_scanout *d, struct scanout **out) {
    struct scanout *o = kmalloc(sizeof *o);
    if (!o) return -N_ERR_NO_MEMORY;
    memset(o, 0, sizeof *o);
    int64_t r = scanout_map(o, frames, pages, type, first_byte, d);
    if (r) {
        kfree(o);
        return r;
    }
    o->next = alsos;
    __atomic_store_n(&alsos, o, __ATOMIC_RELEASE);
    *out = o;
    return 0;
}

void fb_scanout_also_end(struct scanout *o) {
    for (struct scanout **at = &alsos; *at; at = &(*at)->next)
        if (*at == o) {
            __atomic_store_n(at, o->next, __ATOMIC_RELEASE);
            break;
        }
    presents_drain(); /* a panic screen drawn into it meanwhile is done */
    vunmap(o->map, o->pages);
    kfree(o);
}

/* What lies outside the w x h at the top left of o goes black (0 in every format it may have): a
   scanout larger than the panic screen shows nothing of the desktop around it. */
static void blank_around(const struct scanout *o, int w, int h) {
    uint32_t bytes = o->bpp / 8;
    for (uint32_t y = 0; y < o->height; y++) {
        uint32_t from = y < (uint32_t)h ? MIN((uint32_t)w, o->width) : 0;
        if (from < o->width) memset(o->addr + (uint64_t)y * o->pitch + (uint64_t)from * bytes, 0,
                                    (size_t)(o->width - from) * bytes);
    }
}

void fb_present_last(const uint32_t *src, int src_pitch, int w, int h) {
    fb_present(src, src_pitch, NULL, 0, 0, 0, w, h);
    if (!src || !fb.addr || src_pitch <= 0 || w <= 0 || h <= 0) return;
    __atomic_add_fetch(&presenting, 1, __ATOMIC_SEQ_CST);
    blank_around(__atomic_load_n(&shown, __ATOMIC_SEQ_CST), w, h);
    for (struct scanout *o = __atomic_load_n(&alsos, __ATOMIC_ACQUIRE); o;
         o = __atomic_load_n(&o->next, __ATOMIC_ACQUIRE)) {
        present_area(o, src, src_pitch, NULL, 0, 0, 0, w, h);
        blank_around(o, w, h);
    }
    __atomic_sub_fetch(&presenting, 1, __ATOMIC_RELEASE);
}

void fb_present(const uint32_t *src, int src_pitch, const uint8_t *deep, int deep_pitch,
                int x, int y, int w, int h) {
    if (!src || !fb.addr || src_pitch<=0 || w<=0 || h<=0) return;
    __atomic_add_fetch(&presenting, 1, __ATOMIC_SEQ_CST);
    if (hold_blocks()) {
        __atomic_sub_fetch(&presenting, 1, __ATOMIC_RELEASE);
        return;
    }
    present_area(__atomic_load_n(&shown, __ATOMIC_SEQ_CST), src, src_pitch, deep, deep_pitch, x, y, w, h);
    __atomic_sub_fetch(&presenting, 1, __ATOMIC_RELEASE);
}

static void present_area(const struct scanout *out, const uint32_t *src, int src_pitch, const uint8_t *deep,
                         int deep_pitch, int x, int y, int w, int h) {
    int64_t right=(int64_t)x+w, bottom=(int64_t)y+h;
    if (x<0) x=0;
    if (y<0) y=0;
    if (right>(int64_t)out->width) right=out->width;
    if (right>src_pitch) right=src_pitch;
    if (deep && right>deep_pitch) right=deep_pitch;
    if (bottom>(int64_t)out->height) bottom=out->height;
    if (right<=x || bottom<=y) return;
    w=(int)(right-x); h=(int)(bottom-y);
    /* The tiles run on other CPUs in whatever address space each has, where only the kernel's half
       is the same: the console's and the compositor's RAM canvases and the scanouts lie there. */
    /* Real WHPX fbbench measured 2.4x slower with IPI/tile overhead. Keep
       the known-faster scalar default; explicit fbparallel/fbbench can still
       exercise the work on other CPUs without claiming a speedup. */
    if (present_parallel &&
        (uint64_t)src>=0xffff800000000000ULL && (uint64_t)out->addr>=0xffff800000000000ULL &&
        (uint64_t)w*h>=65536 && cpu_online_count()>1) {
        /* The caller's interrupts stay off during a batch (cpu_parallel_for): batches of 128 rows
           let them in between, CPU 0's ticks (audio, the scheduler's timeouts) among them. */
        for (int first=0;first<h;first+=128) {
            struct present_job job={out,src,deep,src_pitch,deep_pitch,x,y+first,w,MIN(h-first,128)};
            cpu_parallel_for(((size_t)job.h+15)/16,present_rows,&job);
        }
    } else {
        struct present_job job={out,src,deep,src_pitch,deep_pitch,x,y,w,h};
        for (size_t tile=0;tile<((size_t)h+15)/16;tile++) present_rows(tile,&job);
    }
}

static uint64_t measured_tick(void) {
    /* Order write-combined framebuffer writes before the measurement. */
    __asm__ volatile("sfence; lfence" ::: "memory");
    return rdtsc();
}
void fb_benchmark(const uint32_t *src,int src_pitch) {
    if (!cmdline_has("fbbench") || !src || !fb.native || src_pitch<(int)fb.width) return;
    struct present_job job={&scanouts[0],src,NULL,src_pitch,0,0,0,(int)fb.width,(int)fb.height};
    uint64_t start=measured_tick();
    for (unsigned n=0;n<3;n++) for (size_t tile=0;tile<(fb.height+15)/16;tile++) present_rows(tile,&job);
    uint64_t serial=measured_tick()-start,jobs=cpu_parallel_jobs();
    start=measured_tick();
    for (unsigned n=0;n<3;n++) fb_present(src,src_pitch,NULL,0,0,0,fb.width,fb.height);
    uint64_t parallel=measured_tick()-start;
    bool same=true;
    for (unsigned y=0;y<fb.height;y++) {
        volatile uint32_t *row=(volatile uint32_t *)(fb.addr+(uint64_t)y*fb.pitch);
        /* Read actual display VRAM, not a separate mock destination. */
        for (unsigned x=0;x<fb.width;x++) if (row[x]!=src[(size_t)y*src_pitch+x]) { same=false; break; }
        if (!same) break;
    }
    kprintf("fbbench: %ux%u rounds=3 CPUs=%u serial_ticks=%lu parallel_ticks=%lu jobs=%lu max_job_us=%lu pixels=%s\n",
        fb.width,fb.height,cpu_online_count(),serial,parallel,cpu_parallel_jobs()-jobs,cpu_max_job_us(),same?"PASS":"FAIL");
    if (!same) panic("fbbench: display pixels differ after SMP presentation");
}

/* ---- sleep ----
   The firmware set the display mode at boot; after a sleep nothing sets it again. No driver for
   real hardware sets it yet, so there the display has no resume and sleep is not offered. In a
   VM the emulated display may (vm/vbe.c).
   The firmware sets the same mode again on the next boot (a hibernation image checks it does).
   A driver's scanout lives only while its claim does, and no sleep or hibernation starts while
   an app drives a PCI function (dev/pm.c): neither ever sees one. */
static struct pm_device display_pm = {.name = "display", .stage = PM_STAGE_IO, .cold_ok = true};

void fb_pm_init(void) {
    if (!fb.addr) return;
    vm_display_pm(&display_pm);
    pm_register(&display_pm);
}
