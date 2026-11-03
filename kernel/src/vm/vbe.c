/* The Bochs VBE display (QEMU's standard VGA, VirtualBox's VGA): screens of their own buffers, and
   their mode set again after a sleep.

   QEMU's standard VGA (1234:1111) has its display memory in BAR 0 and, in BAR 2, the EDID of its
   monitor (bytes 0 to 0x3ff) and the VBE registers (from 0x500, register i at 0x500 + 2 * i); a
   second one in the machine is a second monitor (QEMU's docs/specs/standard-vga.rst). The
   firmware set the mode of the one it showed; the others are dark until a mode is set here.
   VirtualBox's (80EE:BEEF) has only the registers behind the index and data ports, which reach
   the one display the firmware showed.

   Each display gets two buffers where its memory holds two frames: the registers' Y offset says
   which rows the display shows from, and the emulator reads it when it draws its next frame,
   so a flip is that register's write and the picture never shows half of a frame. The emulator
   has no vertical blank interrupt: the screen's refresh is the one the EDID's preferred timing
   says (frames are paced at it), and a flip counts as shown when the register is written. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "vm/devices.h"

#define VBE_INDEX  0x1CE
#define VBE_DATA   0x1CF
enum { VBE_ID, VBE_XRES, VBE_YRES, VBE_BPP, VBE_ENABLE, VBE_BANK, VBE_VIRT_WIDTH, VBE_VIRT_HEIGHT,
       VBE_X_OFFSET, VBE_Y_OFFSET, VBE_REGS };
#define VBE_ENABLED   0x01
#define VBE_LFB       0x40
#define VBE_NO_CLEAR  0x80
#define MMIO_EDID     0x000
#define MMIO_VGA      0x400 /* the VGA ports 0x3c0 to 0x3df */
#define MMIO_VBE      0x500
#define MMIO_BYTES    0x1000

struct vbe {
    struct pci_device *d;
    volatile uint8_t *mmio; /* BAR 2, NULL: the legacy ports */
    uint8_t *vram;          /* both buffers, write-combined */
    size_t vram_pages;
    struct screen *screen;
    struct scanout *buf[2];
    unsigned count;
    uint32_t height;
    uint16_t saved[VBE_REGS];
    struct vbe *next;
};
static struct vbe *displays;

static uint16_t vbe_read(struct vbe *v, unsigned reg) {
    if (v->mmio) return mmio_read16(v->mmio + MMIO_VBE + reg * 2);
    outw(VBE_INDEX, (uint16_t)reg);
    return inw(VBE_DATA);
}

static void vbe_write(struct vbe *v, unsigned reg, uint16_t value) {
    if (v->mmio) {
        mmio_write16(v->mmio + MMIO_VBE + reg * 2, value);
        return;
    }
    outw(VBE_INDEX, (uint16_t)reg);
    outw(VBE_DATA, value);
}

/* The attribute controller's palette address source on: the picture is shown (a VGA blanks its
   output without it). */
static void vbe_unblank(struct vbe *v) {
    if (v->mmio) {
        (void)mmio_read8(v->mmio + MMIO_VGA + 0x1A); /* 0x3da: the flip-flop to "index" */
        mmio_write8(v->mmio + MMIO_VGA, 0x20);       /* 0x3c0 */
    } else {
        inb(0x3DA);
        outb(0x3C0, 0x20);
    }
}

static void vbe_flip(struct screen *s, void *owner, unsigned buffer, uint64_t serial) {
    struct vbe *v = owner;
    vbe_write(v, VBE_Y_OFFSET, (uint16_t)(buffer * v->height));
    screen_flipped(s, buffer, serial, clock_now_ns());
}
static const struct screen_ops vbe_ops = {.flip = vbe_flip};

/* The monitor of a display with an EDID: its ids and the refresh of its preferred timing (the
   first detailed one), which is the mode set below. */
static void vbe_monitor(struct vbe *v, struct screen_mode_desc *m, uint32_t *pref_w, uint32_t *pref_h) {
    *pref_w = *pref_h = 0;
    if (!v->mmio) return;
    uint8_t e[EDID_BLOCK];
    for (unsigned i = 0; i < EDID_BLOCK; i++) e[i] = mmio_read8(v->mmio + MMIO_EDID + i);
    struct edid_ids id;
    if (!edid_valid(e, sizeof e) || !edid_ids(e, sizeof e, &id)) return;
    edid_monitor_id(&id, m->monitor);
    memcpy(m->name, id.name, sizeof m->name);
    struct edid_mode modes[8];
    unsigned n = edid_modes(e, modes, ARRAY_SIZE(modes));
    for (unsigned i = 0; i < n; i++)
        if (modes[i].preferred) {
            *pref_w = modes[i].width;
            *pref_h = modes[i].height;
            if (modes[i].width == m->width && modes[i].height == m->height) m->refresh_mhz = modes[i].refresh_mhz;
            break;
        }
    /* the shown mode's refresh, where another timing of the EDID names that size */
    for (unsigned i = 0; i < n && !m->refresh_mhz; i++)
        if (modes[i].width == m->width && modes[i].height == m->height) m->refresh_mhz = modes[i].refresh_mhz;
}

/* One display: its registers, its mode (the firmware's for the one it showed, else the EDID's
   preferred), its buffers, and a screen for them (screen 0 for the firmware's). */
static int vbe_attach(struct pci_device *d) {
    struct vbe *v = kzalloc(sizeof *v);
    if (!v) return -N_ERR_NO_MEMORY;
    int why = -N_ERR_UNSUPPORTED;
    bool legacy_only = d->vendor == 0x80EE;
    v->d = d;
    uint64_t vram = pci_bar_address(d, 0), vram_size = pci_bar_size(d, 0);
    bool firmware = fb.addr && fb.phys >= vram && fb.phys < vram + vram_size;
    if (!legacy_only && pci_bar_size(d, 2) >= MMIO_BYTES) {
        v->mmio = vmalloc_map_phys(pci_bar_address(d, 2), MMIO_BYTES / PAGE_SIZE, PTE_W | PTE_PCD | PTE_PWT | PTE_NX);
        if (!v->mmio) {
            why = -N_ERR_NO_MEMORY;
            goto fail;
        }
    }
    if (!v->mmio && !firmware) goto fail; /* only the shown one is reached through the ports */
    uint16_t id = vbe_read(v, VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF) goto fail;
    pci_write16(d->bus, d->dev, d->func, 4, pci_read16(d->bus, d->dev, d->func, 4) | 2); /* memory space */
    struct screen_mode_desc m = {0};
    uint32_t pref_w, pref_h;
    if (firmware) {
        if (fb.bpp != 32 || !fb.native) goto fail;
        m.width = fb.width;
        m.height = fb.height;
        vbe_monitor(v, &m, &pref_w, &pref_h);
        if (!m.monitor[0]) { /* without an EDID of its own: the firmware's */
            memcpy(m.monitor, fb.monitor, sizeof m.monitor);
            memcpy(m.name, fb.monitor_name, sizeof m.name);
            m.refresh_mhz = fb.refresh_mhz;
        }
    } else {
        vbe_monitor(v, &m, &pref_w, &pref_h);
        if (!pref_w) goto fail; /* a second display without an EDID has no mode to show */
        m.width = pref_w;
        m.height = pref_h;
        vbe_write(v, VBE_ENABLE, 0);
        vbe_write(v, VBE_XRES, (uint16_t)m.width);
        vbe_write(v, VBE_YRES, (uint16_t)m.height);
        vbe_write(v, VBE_BPP, 32);
        vbe_write(v, VBE_ENABLE, VBE_ENABLED | VBE_LFB);
        if (vbe_read(v, VBE_XRES) != m.width || vbe_read(v, VBE_YRES) != m.height) {
            why = -N_ERR_IO;
            goto fail;
        }
        vbe_monitor(v, &m, &pref_w, &pref_h); /* the refresh of the mode now set */
        vbe_unblank(v);
    }
    uint32_t pitch = (uint32_t)vbe_read(v, VBE_VIRT_WIDTH) * 4;
    if (pitch < m.width * 4) goto fail;
    /* two frames where the memory and the virtual height (what the memory holds at that pitch)
       have room for them */
    uint64_t frame = (uint64_t)pitch * m.height;
    uint64_t first = firmware ? fb.phys : vram;
    v->count = vbe_read(v, VBE_VIRT_HEIGHT) >= 2 * m.height && first + 2 * frame <= vram + vram_size ? 2 : 1;
    v->height = m.height;
    v->vram_pages = ALIGN_UP(frame * v->count, PAGE_SIZE) / PAGE_SIZE;
    why = -N_ERR_NO_MEMORY; /* 残りの失敗は確保だけである */
    v->vram = vmalloc_map_phys(first, v->vram_pages, PTE_W | PTE_PWT | PTE_PAT4K | PTE_NX); /* write-combined */
    if (!v->vram) goto fail;
    for (unsigned i = 0; i < v->count; i++)
        if (!(v->buf[i] = fb_buffer_at(v->vram + frame * i, m.width, m.height, pitch, N_SCANOUT_XRGB8888))) goto fail;
    vbe_write(v, VBE_X_OFFSET, 0);
    vbe_write(v, VBE_Y_OFFSET, 0);
    v->screen = firmware ? screen_get(0) : screen_new();
    if (!v->screen) goto fail;
    d->kernel_owned = true; /* the kernel drives it: no app claims it */
    v->next = displays;
    displays = v;
    kprintf("video: emulated display %02x:%02x.%u: %ux%u at %u.%03u Hz, %u buffer%s, screen %u%s%s\n", d->bus, d->dev,
            d->func, m.width, m.height, m.refresh_mhz / 1000, m.refresh_mhz % 1000, v->count, v->count > 1 ? "s" : "",
            screen_id(v->screen), m.name[0] ? ", monitor " : "", m.name);
    screen_set_buffers(v->screen, &vbe_ops, v, &m, v->buf, v->count, 0);
    return 0;
fail:
    for (unsigned i = 0; i < 2; i++) fb_buffer_unmap(v->buf[i]);
    if (v->vram) vunmap(v->vram, v->vram_pages);
    if (v->mmio) vunmap((void *)v->mmio, MMIO_BYTES / PAGE_SIZE);
    kfree(v);
    return why;
}

static const struct n_pci_match vbe_matches[] = {N_PCI_ID_CLASS(0x1234, 0x1111, 0x03, 0x00), N_PCI_ID_CLASS(0x80EE, 0xBEEF, 0x03, 0x00)};
const struct pci_driver vbe_driver = {"vbe", vbe_matches, ARRAY_SIZE(vbe_matches), vbe_attach};

static bool vbe_quiesce(void *arg) {
    (void)arg;
    for (struct vbe *v = displays; v; v = v->next)
        for (unsigned r = 1; r < VBE_REGS; r++) v->saved[r] = vbe_read(v, r);
    return true; /* scanout only: no DMA to stop */
}

static bool vbe_resume(void *arg) {
    (void)arg;
    bool ok = true;
    for (struct vbe *v = displays; v; v = v->next) {
        vbe_write(v, VBE_ENABLE, 0);
        for (unsigned r = 1; r < VBE_REGS; r++)
            if (r != VBE_ENABLE) vbe_write(v, r, v->saved[r]);
        vbe_write(v, VBE_ENABLE, v->saved[VBE_ENABLE] | VBE_NO_CLEAR); /* back on, without clearing the memory */
        vbe_unblank(v);
        ok &= (vbe_read(v, VBE_ENABLE) & (VBE_ENABLED | VBE_LFB)) == (VBE_ENABLED | VBE_LFB) &&
              vbe_read(v, VBE_XRES) == v->saved[VBE_XRES] && vbe_read(v, VBE_YRES) == v->saved[VBE_YRES];
    }
    return ok;
}

/* The display comes back from a sleep only where every display's mode can be set again: one
   these registers reach. */
bool vbe_display_pm(struct pm_device *display) {
    if (!displays) return false;
    display->quiesce = vbe_quiesce;
    display->resume = vbe_resume;
    return true;
}
