/* Bochs の VBE の画面（QEMU の標準の VGA、VirtualBox の VGA）：自分の緩衝を持つ画面と、眠りの後に同じモードを
   設定し直すこと。

   QEMU の標準の VGA（1234:1111）は BAR 0 に表示のメモリーを、BAR 2 にモニターの EDID（0 から 0x3ff）と VBE の登録
   （0x500 から、登録 i は 0x500 + 2 * i）を持つ。機械の二つ目は二つ目のモニターである（QEMU の
   docs/specs/standard-vga.rst）。firmware は自分が出した一つのモードを設定し、ほかはここでモードを設定するまで
   暗い。VirtualBox の VGA（80EE:BEEF）は登録を番号とデータのポートの後ろにだけ持ち、firmware が出した一つの画面に
   届く。

   表示のメモリーに二つの枠が入る画面は緩衝を二つ持つ。登録の Y の差が表示を始める行を言い、模倣器は次の枠を描く時に
   それを読むので、フリップはその登録への書込みで、画面は枠の半分を見せない。模倣器には垂直帰線の割り込みが無い：
   画面の拍は EDID の好みのタイミングの拍（枠はそれで進む）で、フリップは登録を書いた時に見えたと数える。

   電源。装置に DMA も割り込みも無い。電源の walk の間（hal/hal.h）、描画の道のうち実装が持つのはフリップの登録の
   書込みだけで、suspend から resume までは書かずに覚え、resume が最後のものを出してから見えたと言う。枠の画素は画面の
   層が表示のメモリーへ直に書く：D3hot の装置は届いた書込みを捨てるだけで、戻りの後に pm が画面を全部描き直す
   （screen_display->repaint）。電源を失った装置は登録を初めの値に戻しているので、resume はモードを設定し直す。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "sys/clock.h"

#define VBE_INDEX  0x1CE
#define VBE_DATA   0x1CF
enum { VBE_ID, VBE_XRES, VBE_YRES, VBE_BPP, VBE_ENABLE, VBE_BANK, VBE_VIRT_WIDTH, VBE_VIRT_HEIGHT,
       VBE_X_OFFSET, VBE_Y_OFFSET };
#define VBE_ENABLED   0x01
#define VBE_LFB       0x40
#define MMIO_EDID     0x000
#define MMIO_VGA      0x400 /* VGA のポート 0x3c0 から 0x3df */
#define MMIO_VBE      0x500
#define MMIO_BYTES    0x1000

/* 錠：vbe_lock は葉。フリップ（窓の合成の thread）と、suspend・resume（電源の walk の thread）の間で、asleep と
   覚えたフリップと shown を守り、ポートの番号とデータの組の書込みを一つにする。 */
struct vbe {
    volatile uint8_t *mmio; /* BAR 2（hal_map）。NULL：番号とデータのポート */
    uint8_t *vram;          /* 両方の緩衝。write-combined */
    size_t vram_pages;
    struct screen *screen;
    struct scanout *buf[2];
    unsigned count;
    uint32_t width, height;
    uint16_t virt_width; /* 緩衝を切った行の画素の数 */
    struct spinlock vbe_lock;
    bool asleep;           /* suspend から resume まで：登録を書かない */
    unsigned shown;        /* Y の差が見せている緩衝 */
    bool flip_waiting;     /* 眠りの間に頼まれたフリップがある */
    unsigned flip_buffer;  /* その緩衝 */
    uint64_t flip_serial;  /* その番号 */
};

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

/* 属性の制御器のパレットの番地の元を入れる：絵が出る（VGA はこれが無いと出力を消す）。 */
static void vbe_unblank(struct vbe *v) {
    if (v->mmio) {
        (void)mmio_read8(v->mmio + MMIO_VGA + 0x1A); /* 0x3da：flip-flop を「番号」へ */
        mmio_write8(v->mmio + MMIO_VGA, 0x20);       /* 0x3c0 */
    } else {
        inb(0x3DA);
        outb(0x3C0, 0x20);
    }
}

static void vbe_flip(struct screen *s, void *owner, unsigned buffer, uint64_t serial) {
    struct vbe *v = owner;
    uint64_t f = spin_lock(&v->vbe_lock);
    bool asleep = v->asleep;
    if (asleep) {
        v->flip_waiting = true;
        v->flip_buffer = buffer;
        v->flip_serial = serial;
    } else {
        vbe_write(v, VBE_Y_OFFSET, (uint16_t)(buffer * v->height));
        v->shown = buffer;
    }
    spin_unlock(&v->vbe_lock, f);
    if (!asleep) screen_flipped(s, buffer, serial, clock_now_ns());
}
static const struct screen_ops vbe_ops = {.flip = vbe_flip};

/* EDID を持つ画面のモニター：その身元と、好みのタイミング（最初の詳しいもの）の拍。それが下で設定するモード。 */
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
    /* EDID のほかのタイミングがその大きさを言えば、出ているモードの拍 */
    for (unsigned i = 0; i < n && !m->refresh_mhz; i++)
        if (modes[i].width == m->width && modes[i].height == m->height) m->refresh_mhz = modes[i].refresh_mhz;
}

/* width × height、32 bpp、行は virt_width 画素のモードを設定し、絵を出す。X の差は 0、Y の差は呼ぶ側が書く。行の幅は
   有効にした後に書く：有効にするとき行の幅を XRES に戻す実装でも、戻さない実装でも同じ形になる。登録が設定した値を
   読み返せば true。二つ目の画面の attach と、全部の画面の resume が呼ぶ。 */
static bool vbe_mode_set(struct vbe *v) {
    vbe_write(v, VBE_ENABLE, 0);
    vbe_write(v, VBE_XRES, (uint16_t)v->width);
    vbe_write(v, VBE_YRES, (uint16_t)v->height);
    vbe_write(v, VBE_BPP, 32);
    vbe_write(v, VBE_ENABLE, VBE_ENABLED | VBE_LFB);
    vbe_write(v, VBE_VIRT_WIDTH, v->virt_width);
    vbe_write(v, VBE_X_OFFSET, 0);
    vbe_unblank(v);
    return vbe_read(v, VBE_XRES) == v->width && vbe_read(v, VBE_YRES) == v->height &&
           vbe_read(v, VBE_VIRT_WIDTH) == v->virt_width &&
           (vbe_read(v, VBE_ENABLE) & (VBE_ENABLED | VBE_LFB)) == (VBE_ENABLED | VBE_LFB);
}

/* 一つの画面：その登録、そのモード（firmware が出したものはそのモード、ほかは EDID の好み）、その緩衝、それを見せる
   画面（firmware のものは画面 0）。 */
static int vbe_attach(struct hal_device *d) {
    struct pci_device *pci = pci_device_of(d);
    const struct n_hal_resource *memory = hal_resource(d, N_HAL_MEMORY, 0);
    if (!memory) return -N_ERR_UNSUPPORTED;
    struct vbe *v = kzalloc(sizeof *v);
    if (!v) return -N_ERR_NO_MEMORY;
    int why = -N_ERR_UNSUPPORTED;
    bool legacy_only = pci->vendor == 0x80EE;
    uint64_t vram = memory->base, vram_size = memory->size;
    bool firmware = fb.addr && fb.phys >= vram && fb.phys < vram + vram_size;
    const struct n_hal_resource *registers = hal_resource(d, N_HAL_MEMORY, 2);
    if (!legacy_only && registers && registers->size >= MMIO_BYTES) {
        v->mmio = hal_map(d, 2);
        if (!v->mmio) {
            why = -N_ERR_NO_MEMORY;
            goto fail;
        }
    }
    if (!v->mmio && !firmware) goto fail; /* ポートで届くのは出ている一つだけ */
    uint16_t id = vbe_read(v, VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF) goto fail;
    hal_enable(d, HAL_ENABLE_MEMORY);
    struct screen_mode_desc m = {0};
    uint32_t pref_w, pref_h;
    if (firmware) {
        if (fb.bpp != 32 || !fb.native) goto fail;
        m.width = fb.width;
        m.height = fb.height;
        vbe_monitor(v, &m, &pref_w, &pref_h);
        if (!m.monitor[0]) { /* 自分の EDID が無ければ firmware のもの */
            memcpy(m.monitor, fb.monitor, sizeof m.monitor);
            memcpy(m.name, fb.monitor_name, sizeof m.name);
            m.refresh_mhz = fb.refresh_mhz;
        }
        v->width = m.width;
        v->height = m.height;
        v->virt_width = vbe_read(v, VBE_VIRT_WIDTH);
    } else {
        vbe_monitor(v, &m, &pref_w, &pref_h);
        if (!pref_w) goto fail; /* EDID の無い二つ目の画面には出すモードが無い */
        m.width = v->width = pref_w;
        m.height = v->height = pref_h;
        v->virt_width = (uint16_t)pref_w;
        if (!vbe_mode_set(v)) {
            why = -N_ERR_IO;
            goto fail;
        }
        vbe_monitor(v, &m, &pref_w, &pref_h); /* 今設定したモードの拍 */
    }
    uint32_t pitch = (uint32_t)v->virt_width * 4;
    if (pitch < m.width * 4) goto fail;
    /* メモリーと仮想の高さ（その行の幅でメモリーに入る行）に二つの枠の場所があれば二つ */
    uint64_t frame = (uint64_t)pitch * m.height;
    uint64_t first = firmware ? fb.phys : vram;
    v->count = vbe_read(v, VBE_VIRT_HEIGHT) >= 2 * m.height && first + 2 * frame <= vram + vram_size ? 2 : 1;
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
    d->driver_data = v;
    kprintf("video: emulated display %04x:%02x:%02x.%x: %ux%u at %u.%03u Hz, %u buffer%s, screen %u%s%s\n", PCI_ADDRESS_ARGS(pci->address), m.width, m.height, m.refresh_mhz / 1000, m.refresh_mhz % 1000, v->count, v->count > 1 ? "s" : "",
            screen_id(v->screen), m.name[0] ? ", monitor " : "", m.name);
    screen_set_buffers(v->screen, &vbe_ops, v, &m, v->buf, v->count, 0);
    if (firmware) fb_boot_release(); /* この実装は眠りの後にモードを設定し直す：firmware の画面には戻る道が無い */
    return 0;
fail:
    for (unsigned i = 0; i < 2; i++) fb_buffer_unmap(v->buf[i]);
    if (v->vram) vunmap(v->vram, v->vram_pages);
    kfree(v);
    return why;
}

/* フリップの登録を書かなくなる。止める DMA も割り込みも無い。 */
static bool vbe_suspend(struct hal_device *d, enum hal_power_target target) {
    (void)target;
    struct vbe *v = d->driver_data;
    uint64_t f = spin_lock(&v->vbe_lock);
    v->asleep = true;
    spin_unlock(&v->vbe_lock, f);
    return true;
}

/* モードを設定し直し、眠りの間に頼まれた最後のフリップか、前に見せていた緩衝を出す。電源を保った装置にも同じことを
   する（設定し直しは同じ値を書く）。 */
static bool vbe_resume(struct hal_device *d) {
    struct vbe *v = d->driver_data;
    bool ok = vbe_mode_set(v);
    uint64_t f = spin_lock(&v->vbe_lock);
    bool waiting = v->flip_waiting;
    unsigned buffer = waiting ? v->flip_buffer : v->shown;
    uint64_t serial = v->flip_serial;
    vbe_write(v, VBE_Y_OFFSET, (uint16_t)(buffer * v->height));
    v->shown = buffer;
    v->flip_waiting = false;
    v->asleep = false;
    spin_unlock(&v->vbe_lock, f);
    if (waiting) screen_flipped(v->screen, buffer, serial, clock_now_ns());
    if (ok) return true;
    kprintf("video: emulated display %s did not take its mode again\n", d->location);
    hal_failed(d, -N_ERR_IO);
    return false;
}

static const struct n_hal_rule vbe_rules[] = {N_PCI_ID_CLASS(0x1234, 0x1111, 0x03, 0x00),
                                              N_PCI_ID_CLASS(0x80EE, 0xBEEF, 0x03, 0x00)};
const struct hal_driver vbe_driver = {
    .name = "vbe",
    .rules = vbe_rules,
    .rule_count = ARRAY_SIZE(vbe_rules),
    .attach = vbe_attach,
    .suspend = vbe_suspend,
    .resume = vbe_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
