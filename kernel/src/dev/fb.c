#include "kernel.h"
#include "dev/fb.h"
#include "limine.h"
#include "arch/smp.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/pm.h"

struct fb_info fb;

/* Any Limine/GOP bit-mask format: a 10-bit channel value -> that channel's
 * bits at their position, rounded to the channel's own depth (1..16 bits).
 * 8-bit content enters widened by bit replication, so 8 bpc XRGB8888 maps
 * every value to itself and 10 bpc 2:10:10:10 shows 255 as 1023. */
static uint32_t lut_r[1024], lut_g[1024], lut_b[1024];

static void build_lut(uint32_t *lut, uint8_t *size, uint8_t shift, uint32_t bpp) {
    if (*size > 16) *size = 16;
    if (shift >= bpp || shift + *size > bpp) *size = 0; /* a mask outside the pixel shows nothing */
    uint32_t max = (1u << *size) - 1;
    for (unsigned v = 0; v < 1024; v++) lut[v] = (v * max + 511) / 1023 << shift;
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
    build_lut(lut_r, &fb.rsize, fb.rshift, fb.bpp);
    build_lut(lut_g, &fb.gsize, fb.gshift, fb.bpp);
    build_lut(lut_b, &fb.bsize, fb.bshift, fb.bpp);
    fb.bpc = MIN(fb.rsize, MIN(fb.gsize, fb.bsize));
    fb.native = fb.bpp == 32 && fb.rshift == 16 && fb.gshift == 8 && fb.bshift == 0 &&
                fb.rsize == 8 && fb.gsize == 8 && fb.bsize == 8;
    fb.native_bpc = fb.bpc;
    fb_set_simulated(cmdline_has("fbsim10"));
}

/* Run the whole 10 bpc pipeline on an 8 bpc display (every QEMU/Hyper-V
   firmware mode is 8 bpc) and dither it at scanout. Only the window manager
   calls this after boot, between frames, and then reallocates its planes. */
bool fb_set_simulated(bool on) {
    if (on && !fb.native) return false;
    fb.simulated = on;
    fb.bpc = on ? 10 : fb.native_bpc;
    fb.deep = fb.bpc >= 10;
    return true;
}

static inline unsigned channel10(uint32_t c, unsigned shift, uint8_t deep, unsigned deep_shift) {
    unsigned v = (c >> shift) & 0xFF;
    return deep & DEEP_VALID ? v << 2 | ((deep >> deep_shift) & 3) : deep_widen(v);
}

static inline uint32_t convert(uint32_t c, uint8_t deep) {
    return lut_r[channel10(c, 16, deep, 4)] | lut_g[channel10(c, 8, deep, 2)] |
           lut_b[channel10(c, 0, deep, 0)];
}

/* 10 -> 8 bits with the 4x4 ordered dither: +1 with probability frac/4. */
static inline unsigned dither8(unsigned v10, unsigned threshold) {
    unsigned v = (v10 * 4 + threshold) >> 4;
    return v > 255 ? 255 : v;
}

struct present_job { const uint32_t *src; const uint8_t *deep; int pitch, deep_pitch, x, y, w, h; };

/* Disjoint row tiles only. No allocation, scheduling, device commands or FPU.
 * The framebuffer writes remain scalar: WHPX does not emulate SIMD MMIO. */
static void present_rows(size_t tile, void *context) {
    const struct present_job *p = context;
    int first=(int)tile*16, end=MIN(first+16,p->h);
    for (int j = first; j < end; j++) {
        const uint32_t *s = &p->src[(size_t)(p->y + j) * p->pitch + p->x];
        const uint8_t *e = p->deep ? p->deep + (size_t)(p->y + j) * p->deep_pitch + p->x : NULL;
        uint8_t *d = fb.addr + (uint64_t)(p->y + j) * fb.pitch;
        if (fb.native && !e) {
            memcpy(d + p->x * 4, s, (size_t)p->w * 4);
        } else if (fb.simulated) {
            uint32_t *d32 = (uint32_t *)d + p->x;
            for (int i = 0; i < p->w; i++) {
                if (!(e[i] & DEEP_VALID)) { d32[i] = s[i]; continue; }
                unsigned t = dither_threshold((unsigned)(p->x + i), (unsigned)(p->y + j));
                d32[i] = dither8(channel10(s[i], 16, e[i], 4), t) << 16 |
                         dither8(channel10(s[i], 8, e[i], 2), t) << 8 | dither8(channel10(s[i], 0, e[i], 0), t);
            }
        } else if (fb.bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + p->x;
            if (e) for (int i = 0; i < p->w; i++) d32[i] = convert(s[i], e[i]);
            else for (int i = 0; i < p->w; i++) d32[i] = convert(s[i], 0);
        } else if (fb.bpp == 24) {
            uint8_t *d8 = d + p->x * 3;
            for (int i = 0; i < p->w; i++) {
                uint32_t v = convert(s[i], e ? e[i] : 0);
                d8[i * 3] = v & 0xFF;
                d8[i * 3 + 1] = (v >> 8) & 0xFF;
                d8[i * 3 + 2] = (v >> 16) & 0xFF;
            }
        } else if (fb.bpp == 16) {
            uint16_t *d16 = (uint16_t *)d + p->x;
            for (int i = 0; i < p->w; i++) d16[i] = (uint16_t)convert(s[i], e ? e[i] : 0);
        }
    }
}

void fb_present(const uint32_t *src, int src_pitch, const uint8_t *deep, int deep_pitch,
                int x, int y, int w, int h) {
    if (!src || !fb.addr || src_pitch<=0 || w<=0 || h<=0) return;
    int64_t right=(int64_t)x+w, bottom=(int64_t)y+h;
    if (x<0) x=0;
    if (y<0) y=0;
    if (right>(int64_t)fb.width) right=fb.width;
    if (right>src_pitch) right=src_pitch;
    if (deep && right>deep_pitch) right=deep_pitch;
    if (bottom>(int64_t)fb.height) bottom=fb.height;
    if (right<=x || bottom<=y) return;
    w=(int)(right-x); h=(int)(bottom-y);
    /* User mappings are intentionally not accessible to kernel-only AP workers.
     * Current callers are the high-mapped console/compositor RAM buffers. */
    /* Real WHPX fbbench measured 2.4x slower with IPI/tile overhead. Keep
       the known-faster scalar default; explicit fbparallel/fbbench can still
       exercise AP display work without claiming a speedup. */
    if ((cmdline_has("fbparallel") || cmdline_has("fbbench")) &&
        (uint64_t)src>=0xffff800000000000ULL && (uint64_t)fb.addr>=0xffff800000000000ULL &&
        (uint64_t)w*h>=65536 && cpu_online_count()>1) {
        /* A join holds BSP interrupts disabled. Bound each join to 128 rows,
           restoring interrupts between batches for timer/audio delivery. */
        for (int first=0;first<h;first+=128) {
            struct present_job job={src,deep,src_pitch,deep_pitch,x,y+first,w,MIN(h-first,128)};
            cpu_parallel_for(((size_t)job.h+15)/16,present_rows,&job);
        }
    } else {
        struct present_job job={src,deep,src_pitch,deep_pitch,x,y,w,h};
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
    struct present_job job={src,NULL,src_pitch,0,0,0,(int)fb.width,(int)fb.height};
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
   The firmware set the display mode at boot; after a sleep nothing sets it again. Only a display
   with the Bochs VBE interface (QEMU's standard VGA, VirtualBox's VGA) can be put back here, by
   its index/data ports; on any other one the screen would stay dark, so the display has no
   resume there and sleep is not offered. */
#define VBE_INDEX  0x1CE
#define VBE_DATA   0x1CF
#define VBE_REGS   10 /* id, xres, yres, bpp, enable, bank, virt width/height, x/y offset */
#define VBE_ENABLE 4
static uint16_t vbe_saved[VBE_REGS];

static uint16_t vbe_read(uint16_t reg) {
    outw(VBE_INDEX, reg);
    return inw(VBE_DATA);
}

static void vbe_write(uint16_t reg, uint16_t value) {
    outw(VBE_INDEX, reg);
    outw(VBE_DATA, value);
}

static void vbe_quiesce(void *arg) {
    (void)arg;
    for (uint16_t r = 1; r < VBE_REGS; r++) vbe_saved[r] = vbe_read(r);
}

static void vbe_resume(void *arg) {
    (void)arg;
    vbe_write(VBE_ENABLE, 0);
    for (uint16_t r = 1; r < VBE_REGS; r++)
        if (r != VBE_ENABLE) vbe_write(r, vbe_saved[r]);
    vbe_write(VBE_ENABLE, vbe_saved[VBE_ENABLE] | 0x80); /* back on, without clearing the memory */
    inb(0x3DA);       /* the attribute controller's flip-flop to "index" */
    outb(0x3C0, 0x20); /* palette address source: the picture is shown again */
}

static struct pm_device display_pm = {.name = "display", .stage = PM_STAGE_IO};

void fb_pm_init(void) {
    if (!fb.addr) return;
    struct pci_device *d = pci_find(0x1234, 0x1111);
    if (!d) d = pci_find(0x80EE, 0xBEEF);
    uint16_t id = d && d->cls == 0x03 && d->subcls == 0x00 ? vbe_read(0) : 0;
    if (id >= 0xB0C0 && id <= 0xB0CF && (vbe_read(VBE_ENABLE) & 0x41) == 0x41) {
        display_pm.quiesce = vbe_quiesce;
        display_pm.resume = vbe_resume;
    }
    pm_register(&display_pm);
}
