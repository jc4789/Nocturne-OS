#include "kernel.h"
#include "dev/fb.h"
#include "limine.h"
#include "arch/smp.h"
#include "arch/cpu.h"

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

struct present_job { const uint32_t *src; int pitch, x, y, w, h; };

/* Disjoint row tiles only. No allocation, scheduling, device commands or FPU.
 * The framebuffer writes remain scalar: WHPX does not emulate SIMD MMIO. */
static void present_rows(size_t tile, void *context) {
    const struct present_job *p = context;
    int first=(int)tile*16, end=MIN(first+16,p->h);
    for (int j = first; j < end; j++) {
        const uint32_t *s = &p->src[(size_t)(p->y + j) * p->pitch + p->x];
        uint8_t *d = fb.addr + (uint64_t)(p->y + j) * fb.pitch;
        if (fb.native) {
            memcpy(d + p->x * 4, s, (size_t)p->w * 4);
        } else if (fb.bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + p->x;
            for (int i = 0; i < p->w; i++) d32[i] = convert(s[i]);
        } else if (fb.bpp == 24) {
            uint8_t *d8 = d + p->x * 3;
            for (int i = 0; i < p->w; i++) {
                uint32_t v = convert(s[i]);
                d8[i * 3] = v & 0xFF;
                d8[i * 3 + 1] = (v >> 8) & 0xFF;
                d8[i * 3 + 2] = (v >> 16) & 0xFF;
            }
        } else if (fb.bpp == 16) {
            uint16_t *d16 = (uint16_t *)d + p->x;
            for (int i = 0; i < p->w; i++) d16[i] = (uint16_t)convert(s[i]);
        }
    }
}

void fb_present(const uint32_t *src, int src_pitch, int x, int y, int w, int h) {
    if (!src || !fb.addr || src_pitch<=0 || w<=0 || h<=0) return;
    int64_t right=(int64_t)x+w, bottom=(int64_t)y+h;
    if (x<0) x=0;
    if (y<0) y=0;
    if (right>(int64_t)fb.width) right=fb.width;
    if (right>src_pitch) right=src_pitch;
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
            struct present_job job={src,src_pitch,x,y+first,w,MIN(h-first,128)};
            cpu_parallel_for(((size_t)job.h+15)/16,present_rows,&job);
        }
    } else {
        struct present_job job={src,src_pitch,x,y,w,h};
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
    struct present_job job={src,src_pitch,0,0,(int)fb.width,(int)fb.height};
    uint64_t start=measured_tick();
    for (unsigned n=0;n<3;n++) for (size_t tile=0;tile<(fb.height+15)/16;tile++) present_rows(tile,&job);
    uint64_t serial=measured_tick()-start,jobs=cpu_parallel_jobs();
    start=measured_tick();
    for (unsigned n=0;n<3;n++) fb_present(src,src_pitch,0,0,fb.width,fb.height);
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
