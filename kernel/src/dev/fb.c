#include "kernel.h"
#include "dev/fb.h"
#include "limine.h"
#include "arch/smp.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/wait.h"
#include "vm/vm.h"

struct fb_info fb;

/* ---- buffers ----
   A buffer is memory a display engine shows, in one pixel format: the firmware's framebuffer
   (fb.addr), a display driver's surface (fb_buffer_map), or an emulator's display memory
   (fb_buffer_at). Any Limine/GOP bit-mask format: a 10-bit channel value -> that channel's bits at
   their position, rounded to the channel's own depth (1..16 bits). 8-bit content enters widened by
   bit replication, so 8 bpc XRGB8888 maps every value to itself and 10 bpc 2:10:10:10 shows 255
   as 1023. */
struct scanout {
    uint8_t *addr;  /* the first row */
    void *map;      /* a driver's surface: the kernel mapping addr lies in (vunmap), else NULL */
    size_t pages;
    uint32_t width, height, pitch, bpp;
    bool native;    /* 32bpp xRGB8888: rows are copied */
    struct scanout *next; /* the images' list (fb_image_add) */
    uint32_t lut_r[1024], lut_g[1024], lut_b[1024];
};
static struct scanout boot_out; /* the firmware's framebuffer */

static void build_lut(uint32_t *lut, uint8_t *size, uint8_t shift, uint32_t bpp) {
    if (*size > 16) *size = 16;
    if (shift >= bpp || shift + *size > bpp) *size = 0; /* a mask outside the pixel shows nothing */
    uint32_t max = (1u << *size) - 1;
    for (unsigned v = 0; v < 1024; v++) lut[v] = (v * max + 511) / 1023 << shift;
}

/* The firmware's monitor (fb.monitor): its ids, and the refresh of the firmware's mode: the first
   detailed timing of that size (the native one comes first), else the one rate the standard and
   established timings of that size have; with several, the rate the firmware chose is not known
   (0). */
static void fb_monitor(const uint8_t *e, size_t n) {
    struct edid_ids id;
    if (!edid_valid(e, n) || !edid_ids(e, n, &id)) return;
    edid_monitor_id(&id, fb.monitor);
    memcpy(fb.monitor_name, id.name, sizeof fb.monitor_name);
    struct edid_mode modes[32];
    unsigned count = edid_modes(e, modes, 32);
    uint32_t mhz = 0;
    bool several = false;
    for (unsigned i = 0; i < count; i++) {
        const struct edid_mode *m = &modes[i];
        if (m->width != fb.width || m->height != fb.height) continue;
        if (m->detailed) {
            mhz = m->refresh_mhz;
            several = false;
            break;
        }
        several |= mhz && mhz != m->refresh_mhz;
        mhz = m->refresh_mhz;
    }
    fb.refresh_mhz = several ? 0 : mhz;
}

static void screens_boot(void);

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
    struct scanout *o = &boot_out;
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
    screens_boot();
}

int64_t fb_buffer_check(uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, uint64_t offset,
                        uint64_t *bytes) {
    /* Any size a mode has (a display engine's sizes are 16 bits); whether the desktop's planes fit
       in memory at it is the window manager's to say. */
    if (!fb.addr || !width || !height || width > UINT16_MAX || height > UINT16_MAX) return -N_ERR_INVALID;
    uint32_t f = format & N_SCANOUT_FORMAT_MASK;
    if (format & ~(N_SCANOUT_FORMAT_MASK | N_SCANOUT_DIRECT)) return -N_ERR_INVALID;
    if (f != N_SCANOUT_XRGB8888 && f != N_SCANOUT_XRGB2101010 && f != N_SCANOUT_XBGR8888) return -N_ERR_INVALID;
    if (pitch < (uint64_t)width * 4 || pitch % 4) return -N_ERR_INVALID;
    uint64_t rows = (uint64_t)pitch * height;
    if (offset % 4 || offset > UINT64_MAX - rows) return -N_ERR_INVALID;
    *bytes = rows;
    return 0;
}

static struct scanout *buffer_new(uint8_t *addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
    struct scanout *o = kzalloc(sizeof *o);
    if (!o) return NULL;
    uint32_t f = format & N_SCANOUT_FORMAT_MASK;
    bool deep = f == N_SCANOUT_XRGB2101010, bgr = f == N_SCANOUT_XBGR8888;
    uint8_t rsize = deep ? 10 : 8, gsize = rsize, bsize = rsize;
    build_lut(o->lut_r, &rsize, deep ? 20 : bgr ? 0 : 16, 32);
    build_lut(o->lut_g, &gsize, deep ? 10 : 8, 32);
    build_lut(o->lut_b, &bsize, bgr ? 16 : 0, 32);
    o->addr = addr;
    o->width = width;
    o->height = height;
    o->pitch = pitch;
    o->bpp = 32;
    o->native = f == N_SCANOUT_XRGB8888;
    return o;
}

struct scanout *fb_buffer_map(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                              uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
    void *map = vmalloc_map_frames(frames, pages, PTE_W | type | PTE_NX);
    if (!map) return NULL;
    struct scanout *o = buffer_new((uint8_t *)map + first_byte, width, height, pitch, format);
    if (!o) {
        vunmap(map, pages);
        return NULL;
    }
    o->map = map;
    o->pages = pages;
    return o;
}

struct scanout *fb_buffer_at(uint8_t *addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
    return buffer_new(addr, width, height, pitch, format);
}

void fb_buffer_unmap(struct scanout *o) {
    if (!o || o == &boot_out) return;
    if (o->map) vunmap(o->map, o->pages);
    kfree(o);
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

/* Rows y..y+h of a rectangle into o. No allocation, scheduling, device commands or FPU, which the
   kernel's code does not use. The 10 bpc simulation dithers
   only onto an 8 bpc xRGB buffer; onto another the deep plane is converted as it is. */
static void present_area(const struct scanout *o, const uint32_t *src, int src_pitch, const uint8_t *deep,
                         int deep_pitch, int x, int y, int w, int h) {
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int64_t)o->width) right = o->width;
    if (right > src_pitch) right = src_pitch;
    if (deep && right > deep_pitch) right = deep_pitch;
    if (bottom > (int64_t)o->height) bottom = o->height;
    if (right <= x || bottom <= y) return;
    w = (int)(right - x);
    h = (int)(bottom - y);
    for (int j = 0; j < h; j++) {
        const uint32_t *s = &src[(size_t)(y + j) * src_pitch + x];
        const uint8_t *e = deep ? deep + (size_t)(y + j) * deep_pitch + x : NULL;
        uint8_t *d = o->addr + (uint64_t)(y + j) * o->pitch;
        if (o->native && !e) {
            memcpy(d + x * 4, s, (size_t)w * 4);
        } else if (fb.simulated && o->native) {
            uint32_t *d32 = (uint32_t *)d + x;
            for (int i = 0; i < w; i++) {
                if (!(e[i] & DEEP_VALID)) { d32[i] = s[i]; continue; }
                unsigned t = dither_threshold((unsigned)(x + i), (unsigned)(y + j));
                d32[i] = dither8(channel10(s[i], 16, e[i], 4), t) << 16 |
                         dither8(channel10(s[i], 8, e[i], 2), t) << 8 | dither8(channel10(s[i], 0, e[i], 0), t);
            }
        } else if (o->bpp == 32) {
            uint32_t *d32 = (uint32_t *)d + x;
            if (e) for (int i = 0; i < w; i++) d32[i] = convert(o, s[i], e[i]);
            else for (int i = 0; i < w; i++) d32[i] = convert(o, s[i], 0);
        } else if (o->bpp == 24) {
            uint8_t *d8 = d + x * 3;
            for (int i = 0; i < w; i++) {
                uint32_t v = convert(o, s[i], e ? e[i] : 0);
                d8[i * 3] = v & 0xFF;
                d8[i * 3 + 1] = (v >> 8) & 0xFF;
                d8[i * 3 + 2] = (v >> 16) & 0xFF;
            }
        } else if (o->bpp == 16) {
            uint16_t *d16 = (uint16_t *)d + x;
            for (int i = 0; i < w; i++) d16[i] = (uint16_t)convert(o, s[i], e ? e[i] : 0);
        }
    }
}

/* ---- screens ---- */
struct screen {
    unsigned id;
    /* Taken by a frame from its begin to its end, and by a change of buffers: a frame never
       writes into buffers that are being taken away. */
    struct mutex use;
    /* Under use (read by their writer without it): */
    const struct screen_ops *ops;
    void *owner;
    struct scanout *buf[SCREEN_BUFFERS_MAX];
    unsigned count;
    uint64_t generation;      /* moves on with every change of the buffers */
    struct screen_mode_desc mode;
    /* Under lock (a flip shown or a vertical blank comes from the driver's thread): */
    struct spinlock lock;
    unsigned shown;                         /* the buffer the engine shows */
    unsigned pending;                       /* bit i: a flip to buffer i asked and not shown yet */
    uint64_t asked[SCREEN_BUFFERS_MAX];     /* the serial of the last flip asked to each */
    uint64_t serial;                        /* the last flip asked */
    uint64_t shown_serial, shown_ns, vblank_count, vblank_ns;
    uint64_t events;                        /* atomic: moves on before each wake of wq */
    struct wait_queue wq;
};

/* Screens are never freed: the list only grows, under screens_lock; screen_get reads it there.
   The panic screen walks it without the lock (every other CPU stopped). */
static struct spinlock screens_lock;
static struct screen **screens;
static size_t nscreens, screens_room;
static struct screen screen0;
static struct screen *screen0_list[1] = {&screen0};
static void (*changed_hook)(struct screen *s);

static void boot_flip(struct screen *s, void *owner, unsigned buffer, uint64_t serial) {
    (void)owner;
    screen_flipped(s, buffer, serial, clock_now_ns()); /* nothing to flip: the copy is what is shown */
}
static const struct screen_ops boot_ops = {.flip = boot_flip};

/* s shows the firmware's framebuffer (screen 0 without a driver). use held, or at boot. */
static void show_boot(struct screen *s) {
    s->ops = &boot_ops;
    s->owner = NULL;
    s->buf[0] = &boot_out;
    s->count = 1;
    s->mode = (struct screen_mode_desc){.width = fb.width, .height = fb.height, .refresh_mhz = fb.refresh_mhz};
    memcpy(s->mode.monitor, fb.monitor, sizeof s->mode.monitor);
    memcpy(s->mode.name, fb.monitor_name, sizeof s->mode.name);
}

static void screens_boot(void) {
    screen0.id = 0;
    screens = screen0_list;
    nscreens = screens_room = 1;
    if (fb.addr) show_boot(&screen0);
}

struct screen *screen_new(void) {
    struct screen *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    uint64_t f = spin_lock(&screens_lock);
    /* the first list is screen 0's static one: the first growth copies it */
    if (nscreens == screens_room) {
        size_t room = screens_room * 2;
        struct screen **list = kmalloc(room * sizeof *list);
        if (!list) {
            spin_unlock(&screens_lock, f);
            kfree(s);
            return NULL;
        }
        memcpy(list, screens, nscreens * sizeof *list);
        struct screen **old = screens;
        __atomic_store_n(&screens, list, __ATOMIC_RELEASE);
        screens_room = room;
        if (old != screen0_list) kfree(old); /* kfree takes no lock that comes before this one */
    }
    s->id = (unsigned)nscreens;
    screens[nscreens] = s;
    __atomic_store_n(&nscreens, nscreens + 1, __ATOMIC_RELEASE);
    spin_unlock(&screens_lock, f);
    return s;
}

struct screen *screen_get(unsigned id) {
    uint64_t f = spin_lock(&screens_lock);
    struct screen *s = id < nscreens ? screens[id] : NULL;
    spin_unlock(&screens_lock, f);
    return s;
}

unsigned screen_count(void) { return (unsigned)__atomic_load_n(&nscreens, __ATOMIC_ACQUIRE); }
unsigned screen_id(const struct screen *s) { return s->id; }

void screen_changed_hook(void (*changed)(struct screen *s)) { changed_hook = changed; }

static void screen_wake(struct screen *s) {
    __atomic_add_fetch(&s->events, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&s->wq);
}

void screen_poke(struct screen *s) { screen_wake(s); }

uint64_t screen_events(struct screen *s) { return __atomic_load_n(&s->events, __ATOMIC_SEQ_CST); }
void screen_wait(struct screen *s, uint64_t seen, uint64_t deadline) { wq_wait_unless(&s->wq, &s->events, seen, deadline); }

static void console_retarget(void);

uint64_t screen_set_buffers(struct screen *s, const struct screen_ops *ops, void *owner, const struct screen_mode_desc *desc,
                            struct scanout *const *buffers, unsigned count, unsigned shown) {
    mutex_lock(&s->use);
    if (count) {
        s->ops = ops;
        s->owner = owner;
        for (unsigned i = 0; i < count; i++) s->buf[i] = buffers[i];
        s->count = count;
        s->mode = *desc;
    } else if (s == &screen0 && fb.addr) {
        show_boot(s);
    } else {
        s->ops = NULL;
        s->owner = NULL;
        s->count = 0;
        s->mode = (struct screen_mode_desc){0};
    }
    s->generation++;
    uint64_t f = spin_lock(&s->lock);
    s->shown = count ? shown : 0;
    s->pending = 0;
    memset(s->asked, 0, sizeof s->asked);
    /* the flips asked of the old buffers are over: none is in flight */
    if (s->shown_serial != s->serial) {
        s->shown_serial = s->serial;
        s->shown_ns = clock_now_ns();
    }
    uint64_t last = s->serial;
    spin_unlock(&s->lock, f);
    mutex_unlock(&s->use);
    if (s == &screen0) console_retarget();
    screen_wake(s);
    if (changed_hook) changed_hook(s);
    return last;
}

bool screen_owned_by(struct screen *s, void *owner) {
    mutex_lock(&s->use);
    bool ours = s->count && s->owner == owner;
    mutex_unlock(&s->use);
    return ours;
}

bool screen_mode(struct screen *s, struct screen_mode_desc *out) {
    mutex_lock(&s->use);
    bool on = s->count != 0;
    *out = s->mode;
    mutex_unlock(&s->use);
    return on;
}

bool screen_boot(struct screen *s) {
    mutex_lock(&s->use);
    bool boot = s->count && s->buf[0] == &boot_out;
    mutex_unlock(&s->use);
    return boot;
}

void screen_set_refresh(struct screen *s, uint32_t refresh_mhz, uint32_t vrr_min_mhz) {
    mutex_lock(&s->use);
    bool changed = s->count && (s->mode.refresh_mhz != refresh_mhz || s->mode.vrr_min_mhz != vrr_min_mhz);
    if (changed) {
        s->mode.refresh_mhz = refresh_mhz;
        s->mode.vrr_min_mhz = vrr_min_mhz;
    }
    mutex_unlock(&s->use);
    if (changed && changed_hook) changed_hook(s);
}

/* ---- holding the boot framebuffer ----
   A display driver that rebuilds the way the CPU reaches the framebuffer (NVIDIA's RM switches
   the BAR the boot screen lies in to virtual mode during its start and stop) asks that nothing is
   written there meanwhile. While a hold lasts, nothing writes to the firmware's framebuffer: the
   compositor and the console keep drawing into their RAM canvases, and the whole screen is drawn
   again when the last hold ends. Time never ends a hold: drawing while the driver still rebuilds
   is the very write the hold keeps out. A hold ends when the driver gives it back, or when its
   claim ends (dev/pciuser.c), which stops the device before it gives the holds back; that is also
   how a driver stuck while holding is ended (pciuser.c, boot option fbhold=).
   The compositor's frames look at the holds with screen 0's use taken, which fb_hold takes after
   it counted itself. The console's presents count themselves in presenting before they look at
   hold_count and their target; a hold or a change of target changes those before it waits for
   presenting to reach 0 (all sequentially consistent), so either the present sees the change or
   the change waits for the present. A present that ends wakes those waiting (drain_waiting): after
   the scheduler started, presents come only from threads (the console's). The panic screen is
   drawn whatever is held. */
static unsigned hold_count;  /* atomic */
static uint64_t presenting;  /* atomic: fb_present calls between their check and return */
static unsigned drain_waiting; /* atomic: threads in presents_drain */
static struct wait_queue drain_wq;
static uint64_t hold_since;  /* atomic: clock ns when hold_count left 0 */
static bool holds_dropped;   /* the kernel stopped (panic, every other CPU stopped): nothing is held any more */
static struct scanout *console_out = &boot_out; /* atomic: where fb_present writes */

static void presents_drain(void) {
    __atomic_add_fetch(&drain_waiting, 1, __ATOMIC_SEQ_CST);
    for (uint64_t p; (p = __atomic_load_n(&presenting, __ATOMIC_SEQ_CST));)
        wq_wait_unless(&drain_wq, &presenting, p, CLOCK_NEVER);
    __atomic_sub_fetch(&drain_waiting, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sfence" ::: "memory"); /* write-combined pixels already sent leave the CPU now */
}

static void present_done(void) {
    __atomic_sub_fetch(&presenting, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&drain_waiting, __ATOMIC_SEQ_CST)) wq_wake_all(&drain_wq);
}

static bool hold_blocks(void) { return !holds_dropped && __atomic_load_n(&hold_count, __ATOMIC_SEQ_CST); }

/* The console writes into what screen 0 shows; screen 0's buffers changed. */
static void console_retarget(void) {
    mutex_lock(&screen0.use);
    struct scanout *o = screen0.count ? screen0.buf[screen0.shown] : &boot_out;
    mutex_unlock(&screen0.use);
    __atomic_store_n(&console_out, o, __ATOMIC_SEQ_CST);
    presents_drain();
}

void fb_hold(void) {
    if (__atomic_fetch_add(&hold_count, 1, __ATOMIC_SEQ_CST) == 0) {
        __atomic_store_n(&hold_since, clock_now_ns(), __ATOMIC_RELAXED);
        kprintf("fb: a display driver holds the screen\n");
    }
    presents_drain();
    mutex_lock(&screen0.use); /* a frame that began before the hold has ended */
    mutex_unlock(&screen0.use);
    __asm__ volatile("sfence" ::: "memory");
}

bool fb_release(void) {
    unsigned before = __atomic_load_n(&hold_count, __ATOMIC_ACQUIRE);
    while (before && !__atomic_compare_exchange_n(&hold_count, &before, before - 1, false, __ATOMIC_ACQ_REL,
                                                  __ATOMIC_ACQUIRE)) {
    }
    if (before != 1) return false;
    kprintf("fb: the screen was released after %lu ms\n",
            (unsigned long)((clock_now_ns() - __atomic_load_n(&hold_since, __ATOMIC_RELAXED)) / NS_PER_MS));
    return true;
}

void fb_drop_holds(void) { holds_dropped = true; }

bool fb_dither_able(void) {
    struct scanout *o = __atomic_load_n(&console_out, __ATOMIC_ACQUIRE);
    return o->native;
}

/* Run the whole 10 bpc pipeline on an 8 bpc display and dither it at scanout (smooth gradients).
   Only the window manager calls this after boot, between frames, and then reallocates its planes;
   it turns it off when screen 0 shows buffers of another format. */
bool fb_set_simulated(bool on) {
    if (on && !fb_dither_able()) return false;
    fb.simulated = on;
    fb.bpc = on ? 10 : fb.native_bpc;
    fb.deep = fb.bpc >= 10;
    return true;
}

void fb_present(const uint32_t *src, int src_pitch, int x, int y, int w, int h) {
    if (!src || !fb.addr || src_pitch <= 0 || w <= 0 || h <= 0) return;
    __atomic_add_fetch(&presenting, 1, __ATOMIC_SEQ_CST);
    struct scanout *o = __atomic_load_n(&console_out, __ATOMIC_SEQ_CST);
    if (!(o == &boot_out && hold_blocks())) present_area(o, src, src_pitch, NULL, 0, x, y, w, h);
    present_done();
}

/* ---- frames ---- */
int screen_frame_begin(struct screen *s, uint64_t deadline, uint64_t *generation) {
    mutex_lock(&s->use);
    for (;;) {
        if (!s->count || (s->buf[0] == &boot_out && hold_blocks())) {
            mutex_unlock(&s->use);
            return -N_ERR_CLOSED;
        }
        if (s->count == 1) {
            *generation = s->generation;
            return 0;
        }
        /* the free buffer shown longest ago: neither shown nor waiting for a flip */
        uint64_t f = spin_lock(&s->lock);
        int pick = -1;
        for (unsigned i = 0; i < s->count; i++)
            if (i != s->shown && !(s->pending & 1u << i) && (pick < 0 || s->asked[i] < s->asked[pick])) pick = (int)i;
        spin_unlock(&s->lock, f);
        if (pick >= 0) {
            *generation = s->generation;
            return pick;
        }
        uint64_t seen = screen_events(s);
        mutex_unlock(&s->use); /* a change of buffers may come meanwhile */
        if (clock_now_ns() >= deadline) return -N_ERR_TIMEOUT;
        screen_wait(s, seen, deadline);
        mutex_lock(&s->use);
    }
}

void screen_frame_write(struct screen *s, int buffer, const uint32_t *src, int src_pitch, const uint8_t *deep,
                        int deep_pitch, int x, int y, int w, int h) {
    if (!src || src_pitch <= 0 || w <= 0 || h <= 0) return;
    present_area(s->buf[buffer], src, src_pitch, deep, deep_pitch, x, y, w, h);
}

uint64_t screen_frame_end(struct screen *s, int buffer) {
    __asm__ volatile("sfence" ::: "memory"); /* the frame's write-combined pixels are out before the flip */
    uint64_t f = spin_lock(&s->lock);
    uint64_t serial = ++s->serial;
    s->asked[buffer] = serial;
    if (s->count > 1) {
        s->pending |= 1u << buffer;
    } else { /* written into the buffer shown: shown as it was written */
        s->shown_serial = serial;
        s->shown_ns = clock_now_ns();
    }
    spin_unlock(&s->lock, f);
    s->ops->flip(s, s->owner, (unsigned)buffer, serial);
    mutex_unlock(&s->use);
    return serial;
}

void screen_flipped(struct screen *s, unsigned buffer, uint64_t serial, uint64_t time_ns) {
    uint64_t f = spin_lock(&s->lock);
    if (buffer < SCREEN_BUFFERS_MAX && serial > s->shown_serial && serial <= s->serial) {
        s->shown = buffer;
        s->shown_serial = serial;
        s->shown_ns = time_ns;
        for (unsigned i = 0; i < SCREEN_BUFFERS_MAX; i++)
            if (s->asked[i] <= serial) s->pending &= ~(1u << i);
    }
    spin_unlock(&s->lock, f);
    screen_wake(s);
}

void screen_vblank(struct screen *s, uint64_t count, uint64_t time_ns) {
    uint64_t f = spin_lock(&s->lock);
    s->vblank_count = count;
    s->vblank_ns = time_ns;
    spin_unlock(&s->lock, f);
    screen_wake(s);
}

void screen_times(struct screen *s, struct screen_times *out) {
    uint64_t f = spin_lock(&s->lock);
    *out = (struct screen_times){s->shown_serial, s->shown_ns, s->vblank_count, s->vblank_ns, s->serial};
    spin_unlock(&s->lock, f);
}

/* ---- what the display engine may show in a buffer's place ----
   A driver that shows an app's window directly (N_SCANOUT_DIRECT, gui/wm.c) flips the display
   engine from its buffers to the app's own images, which nothing of the system's is drawn into.
   When the kernel stops, the engine goes on showing whichever was flipped last, and no driver runs
   any more to flip back: the panic screen is drawn into each of them as well (fb_present_last).
   dev/pciuser.c adds and removes them, one call at a time. */
static struct scanout *images; /* atomic: the list fb_present_last walks */

void fb_image_add(struct scanout *o) {
    o->next = images;
    __atomic_store_n(&images, o, __ATOMIC_RELEASE);
}

void fb_image_remove(struct scanout *o) {
    for (struct scanout **at = &images; *at; at = &(*at)->next)
        if (*at == o) {
            __atomic_store_n(at, o->next, __ATOMIC_RELEASE);
            break;
        }
    presents_drain(); /* a panic screen drawn into it meanwhile is done */
}

/* What lies outside the w x h at the top left of o goes black (0 in every format it may have): a
   buffer larger than the panic screen shows nothing of the desktop around it. */
static void blank_around(const struct scanout *o, int w, int h) {
    uint32_t bytes = o->bpp / 8;
    for (uint32_t y = 0; y < o->height; y++) {
        uint32_t from = y < (uint32_t)h ? MIN((uint32_t)w, o->width) : 0;
        if (from < o->width) memset(o->addr + (uint64_t)y * o->pitch + (uint64_t)from * bytes, 0,
                                    (size_t)(o->width - from) * bytes);
    }
}

static void last_into(const struct scanout *o, const uint32_t *src, int src_pitch, int w, int h) {
    present_area(o, src, src_pitch, NULL, 0, 0, 0, w, h);
    blank_around(o, w, h);
}

void fb_present_last(const uint32_t *src, int src_pitch, int w, int h) {
    if (!src || !fb.addr || src_pitch <= 0 || w <= 0 || h <= 0) return;
    __atomic_add_fetch(&presenting, 1, __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < nscreens; i++) {
        struct screen *s = screens[i];
        for (unsigned b = 0; b < s->count; b++) last_into(s->buf[b], src, src_pitch, w, h);
    }
    if (!screen0.count || screen0.buf[0] != &boot_out) last_into(&boot_out, src, src_pitch, w, h);
    for (struct scanout *o = __atomic_load_n(&images, __ATOMIC_ACQUIRE); o; o = __atomic_load_n(&o->next, __ATOMIC_ACQUIRE))
        last_into(o, src, src_pitch, w, h);
    __atomic_sub_fetch(&presenting, 1, __ATOMIC_RELEASE);
}

/* ---- sleep ----
   The firmware set the display mode at boot; after a sleep nothing sets it again. No driver for
   real hardware sets it yet, so there the display has no resume and sleep is not offered. In a
   VM the emulated display may (vm/vbe.c).
   The firmware sets the same mode again on the next boot (a hibernation image checks it does).
   A driver's buffers live only while its claim does, and no sleep or hibernation starts while
   an app drives a PCI function (dev/pm.c): neither ever sees one. */
static struct pm_device display_pm = {.name = "display", .stage = PM_STAGE_IO, .cold_ok = true};

void fb_pm_init(void) {
    if (!fb.addr) return;
    vm_display_pm(&display_pm);
    pm_register(&display_pm);
}
