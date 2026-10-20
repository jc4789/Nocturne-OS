/* Drivers in apps (docs/nocturne-native-abi.md §9). Only init claims a PCI function; the claim
   is a vnode whose handles init passes to the driver app it launches. The app reads config space
   (its function, the functions beside it and the bridges above it), writes its own, maps BARs as
   shared objects (syscall.c) and waits for its message-signalled interrupts on the handle.
   The kernel keeps what decides where memory and interrupts go: BAR registers and the MSI and
   MSI-X capabilities are written here only. The last handle's close stops the function: every
   mapping of its BARs, in any process, loses the device pages; then no interrupts and no bus
   mastering, and the kernel waits until the function has no request in flight before the app's
   DMA memory can return to the allocator. A function that does not report its requests done in
   time keeps its DMA pages out of use for good (syscall.c asks pci_user_claim_dma_reusable).
   BAR and DMA objects hold the claim record, so a later claim of the same function is a new
   record and the old objects stay dead. */
#include "kernel.h"
#include "abi.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "dev/fb.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "sys/proc.h"
#include "sys/sched.h"
#include "sys/syscall.h"

#define CLAIM_VECTORS 32 /* one pending bit each */

struct pci_claim {
    struct pci_device *d;
    struct vnode *v;
    volatile uint32_t pending; /* message n arrived; cleared by take */
    struct wait_queue wq;
    unsigned vector_base, vectors;
    /* BAR sizes, probed once at claim: the probe switches memory decoding off for a moment, which
       would make a running function's registers read as all-ones (a GPU gone from the bus). */
    uint64_t bar_size[6];
    unsigned refs;   /* the vnode and every BAR or DMA object made through it */
    bool released;   /* the last handle closed: BAR objects are revoked, the function stopped */
    bool dma_unsafe; /* stopping it was not confirmed: its DMA pages are never reused */
};
static struct pci_claim *claim_of_vector[256];

static void claim_drop(struct pci_claim *c) {
    ASSERT(c->refs);
    if (--c->refs == 0) kfree(c);
}

static void claim_isr(struct regs *r) {
    struct pci_claim *c = claim_of_vector[r->vector & 0xFF];
    if (!c) return;
    c->pending |= 1u << (r->vector - c->vector_base);
    wq_wake_all(&c->wq);
    vfs_poll_notify(c->v);
}

static void interrupts_off(struct pci_claim *c) {
    if (!c->vectors) return;
    pci_msi_disable(c->d);
    uint64_t irq = irq_save();
    for (unsigned i = 0; i < c->vectors; i++) claim_of_vector[c->vector_base + i] = NULL;
    c->pending = 0;
    irq_restore(irq);
    irq_vector_free(c->vector_base, c->vectors);
    c->vectors = 0;
}

static bool claim_can_read(struct vnode *v, struct file *f) {
    (void)f;
    return ((struct pci_claim *)v->priv)->pending != 0;
}
/* After bus mastering is off the function starts no request, but some may still be in flight.
   A PCI Express function says so in Device Status (Transactions Pending), and the config read
   that sees it clear also follows any write it posted before (a completion does not pass a
   posted write). Conventional PCI has no such bit: the read and a short pause stand for it.
   Busy waiting with interrupts possibly off, so by the TSC, bounded: this runs once, when a
   driver ends. */
#define QUIESCE_MS 200
static bool function_quiet(struct pci_device *d) {
    uint64_t since = rdtsc(), spins = 0;
    for (;;) {
        if ((pci_read32(d->bus, d->dev, d->func, 0) & 0xFFFF) == 0xFFFF) return true; /* gone from the bus */
        if (!d->pcie_cap) {
            if (tsc_hz ? rdtsc() - since >= tsc_hz / 1000 : ++spins >= 100000) return true;
        } else if (!(pci_read16(d->bus, d->dev, d->func, d->pcie_cap + 0x0A) & 0x20)) return true;
        if (tsc_hz ? rdtsc() - since > tsc_hz / 1000 * QUIESCE_MS : ++spins > 20000000) return false;
        pause();
    }
}

static void claim_release(struct vnode *v) {
    struct pci_claim *c = v->priv;
    struct pci_device *d = c->d;
    c->released = true;
    shared_revoke_claim(c); /* no process reaches the registers any more */
    interrupts_off(c);
    pci_write16(d->bus, d->dev, d->func, 4, pci_read16(d->bus, d->dev, d->func, 4) & ~0x4u); /* bus mastering off */
    c->dma_unsafe = !function_quiet(d);
    if (c->dma_unsafe)
        kprintf("pci: %02x:%02x.%u still has requests in flight; its DMA memory stays out of use\n",
                d->bus, d->dev, d->func);
    d->claimed = false;
    kprintf("pci: %02x:%02x.%u released\n", d->bus, d->dev, d->func);
    kfree(v);
    claim_drop(c);
}
static struct vnode_ops claim_ops = {.can_read = claim_can_read, .release = claim_release};

bool pci_user_is_claim(struct file *f) { return f && f->vn->ops == &claim_ops; }
static struct pci_claim *claim_of(struct file *f) { return pci_user_is_claim(f) ? f->vn->priv : NULL; }

struct pci_claim *pci_user_hold(struct file *f) {
    struct pci_claim *c = claim_of(f);
    if (c) c->refs++;
    return c;
}
void pci_user_drop(struct pci_claim *c) { if (c) claim_drop(c); }
bool pci_user_revoked(const struct pci_claim *c) { return c && c->released; }
bool pci_user_dma_reusable(const struct pci_claim *c) { return !c || !c->dma_unsafe; }

static struct pci_device *find_function(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d->bus == bus && d->dev == dev && d->func == func) return d;
    }
    return NULL;
}
static bool is_bridge(const struct pci_device *d) {
    return ((pci_read32(d->bus, d->dev, d->func, 0x0C) >> 16) & 0x7F) == 1;
}
/* The PCI-to-PCI bridge whose secondary bus is the function's bus, or NULL on bus 0's roots. */
static struct pci_device *upstream_of(const struct pci_device *d) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *b = pci_at(i);
        if (b != d && is_bridge(b) && ((pci_read32(b->bus, b->dev, b->func, 0x18) >> 8) & 0xFF) == d->bus)
            return b;
    }
    return NULL;
}

int64_t pci_user_claim(uint8_t bus, uint8_t dev, uint8_t func, struct file **out) {
    struct task *t = task_process();
    /* Boot options: noappdrivers keeps every function with the kernel; appdrivers=any lets any
       app claim (tests drive QEMU's edu device from a program they compile) */
    if (!t || cmdline_has("noappdrivers")) return -N_ERR_DENIED;
    if (t->pid != init_pid && !cmdline_has("appdrivers=any")) return -N_ERR_DENIED;
    if (dev > 31 || func > 7) return -N_ERR_INVALID;
    struct pci_device *d = find_function(bus, dev, func);
    if (!d) return -N_ERR_NOT_FOUND;
    if (d->kernel_owned || d->claimed) return -N_ERR_BUSY;
    struct vnode *v = kzalloc(sizeof *v);
    struct pci_claim *c = kzalloc(sizeof *c);
    if (!v || !c) { kfree(v); kfree(c); return -N_ERR_NO_MEMORY; }
    strlcpy(v->name, "pci", sizeof v->name);
    v->type = VT_PCI;
    v->ops = &claim_ops;
    v->priv = c;
    v->unlinked = true; /* the claim ends when its last handle closes */
    c->d = d;
    c->v = v;
    c->refs = 1;
    for (unsigned i = 0; i < 6; i++) {
        c->bar_size[i] = pci_bar_size(d, i);
        if (c->bar_size[i] && !(d->bar[i] & 1) && ((d->bar[i] >> 1) & 3) == 2) i++; /* 64-bit: the upper half is no BAR */
    }
    struct file *f = vfs_open_vnode(v, O_RDWR);
    if (!f) { kfree(c); kfree(v); return -N_ERR_NO_MEMORY; }
    d->claimed = true;
    kprintf("pci: %02x:%02x.%u (%04x:%04x) claimed by pid %d\n", bus, dev, func, d->vendor, d->device, t->pid);
    *out = f;
    return 0;
}

static unsigned msix_table_size(const struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x11);
    return cap ? ((pci_read32(d->bus, d->dev, d->func, cap) >> 16) & 0x7FF) + 1 : 0;
}

int64_t pci_user_info(struct file *f, struct n_pci_function *out) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    struct pci_device *d = c->d;
    memset(out, 0, sizeof *out);
    out->bus = d->bus; out->dev = d->dev; out->func = d->func;
    out->revision = pci_read32(d->bus, d->dev, d->func, 0x08) & 0xFF;
    out->vendor = d->vendor; out->device = d->device;
    uint32_t sub = pci_read32(d->bus, d->dev, d->func, 0x2C);
    out->subsystem_vendor = sub & 0xFFFF; out->subsystem_id = sub >> 16;
    out->cls = d->cls; out->subcls = d->subcls; out->progif = d->progif;
    out->interrupt_pin = (pci_read32(d->bus, d->dev, d->func, 0x3C) >> 8) & 0xFF;
    out->msi_vectors = pci_msi_max_vectors(d);
    out->msix_vectors = msix_table_size(d);
    struct pci_device *up = upstream_of(d);
    out->upstream = up ? N_PCI_TARGET(up->bus, up->dev, up->func) : N_PCI_NONE;
    out->framebuffer_bar = N_PCI_NONE;
    for (unsigned i = 0; i < 6; i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) { out->bar[i] = (struct n_pci_bar){.address = raw & ~3u, .flags = N_PCI_BAR_IO}; continue; }
        uint64_t address = pci_bar_address(d, i), size = c->bar_size[i];
        if (!address || !size) continue;
        uint32_t flags = N_PCI_BAR_MEMORY | (raw & 8 ? N_PCI_BAR_PREFETCH : 0);
        bool wide = ((raw >> 1) & 3) == 2;
        if (wide) flags |= N_PCI_BAR_64BIT;
        out->bar[i] = (struct n_pci_bar){.address = address, .size = size, .flags = flags};
        uint64_t fb_bytes = (uint64_t)fb.pitch * fb.height;
        if (fb.phys && fb.phys >= address && fb.phys - address < size) {
            out->framebuffer_address = fb.phys;
            out->framebuffer_size = fb_bytes;
            out->framebuffer_bar = i;
            out->framebuffer_width = fb.width;
            out->framebuffer_height = fb.height;
            out->framebuffer_pitch = fb.pitch;
            out->framebuffer_bpp = fb.bpp;
        }
        if (wide) i++; /* the upper half has no BAR of its own */
    }
    return 0;
}

static bool config_access_ok(uint32_t offset, uint32_t width) {
    return (width == 1 || width == 2 || width == 4) && offset < 4096 && !(offset & (width - 1));
}
/* Reads reach the claimed function, the other functions of its device (an audio function on a
   graphics card) and the bridges above it, but no unrelated device. */
static bool may_read(const struct pci_device *d, const struct pci_device *target) {
    if (target->bus == d->bus && target->dev == d->dev) return true;
    for (const struct pci_device *b = upstream_of(d); b; b = upstream_of(b))
        if (b == target) return true;
    return false;
}

int64_t pci_user_config_read(struct file *f, uint32_t target, uint32_t offset, uint32_t width) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *t = c->d;
    if (target != N_PCI_SELF) {
        if (target > 0xFFFF) return -N_ERR_INVALID;
        t = find_function(target >> 8, (target >> 3) & 31, target & 7);
        if (!t) return -N_ERR_NOT_FOUND;
        if (!may_read(c->d, t)) return -N_ERR_DENIED;
    }
    uint32_t v = pci_read32(t->bus, t->dev, t->func, offset & ~3u) >> ((offset & 3) * 8);
    return width == 4 ? v : v & ((1u << (width * 8)) - 1);
}

/* Registers only the kernel writes: the BARs, the expansion ROM BAR and the MSI and MSI-X
   capabilities, which decide where memory and interrupt messages go. */
static bool kernel_register(const struct pci_device *d, uint32_t offset, uint32_t width) {
    uint32_t end = offset + width;
    if (offset < 0x28 && end > 0x10) return true;
    if (offset < 0x34 && end > 0x30) return true;
    uint8_t msi = pci_find_cap(d, 0x05), msix = pci_find_cap(d, 0x11);
    if (msi && offset < msi + 24u && end > msi) return true;
    if (msix && offset < msix + 12u && end > msix) return true;
    return false;
}

int64_t pci_user_config_write(struct file *f, uint32_t offset, uint32_t width, uint32_t value) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *d = c->d;
    if (kernel_register(d, offset, width)) return -N_ERR_DENIED;
    if (width == 4) pci_write32(d->bus, d->dev, d->func, offset, value);
    else if (width == 2) pci_write16(d->bus, d->dev, d->func, offset, value);
    else pci_write8(d->bus, d->dev, d->func, offset, value);
    return 0;
}

int64_t pci_user_bar(struct file *f, uint32_t bar, uint64_t *address, uint64_t *size) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (bar > 5) return -N_ERR_INVALID;
    if (c->d->bar[bar] & 1) return -N_ERR_UNSUPPORTED; /* I/O ports */
    uint64_t a = pci_bar_address(c->d, bar), s = c->bar_size[bar];
    if (!a || !s) return -N_ERR_NOT_FOUND;
    /* A smaller BAR shares its page with whatever lies beside it. */
    if ((a | s) & (PAGE_SIZE - 1)) return -N_ERR_UNSUPPORTED;
    *address = a;
    *size = s;
    return 0;
}

static unsigned floor_pow2(unsigned n) {
    unsigned p = 1;
    while (p * 2 <= n) p *= 2;
    return p;
}

int64_t pci_user_interrupts(struct file *f, uint32_t count) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    struct pci_device *d = c->d;
    interrupts_off(c);
    if (!count) return 0;
    if (count > CLAIM_VECTORS) count = CLAIM_VECTORS;
    uint32_t dest = lapic_current_id(); /* the BSP: device interrupts are only handled there */
    if (dest > 0xFF) return -N_ERR_UNSUPPORTED;
    unsigned table = msix_table_size(d), most = pci_msi_max_vectors(d);
    bool msix = table != 0;
    unsigned n = msix ? MIN(count, table) : most ? floor_pow2(MIN(count, most)) : 0;
    if (!n) return -N_ERR_UNSUPPORTED; /* no legacy INTx for apps */
    int base = -1;
    for (; n; n = msix ? n - 1 : n / 2)
        if ((base = irq_vector_alloc(n, msix ? 1 : n)) >= 0) break;
    if (base < 0) return -N_ERR_BUSY;
    uint64_t irq = irq_save();
    c->vector_base = (unsigned)base;
    c->vectors = n;
    c->pending = 0;
    for (unsigned i = 0; i < n; i++) claim_of_vector[base + i] = c;
    irq_restore(irq);
    for (unsigned i = 0; i < n; i++) vector_register(base + (int)i, claim_isr);
    bool ok;
    if (msix) {
        ok = pci_msix_init(d) >= n;
        for (unsigned i = 0; ok && i < n; i++) ok = pci_msix_route(d, i, (uint8_t)(base + i), dest);
        for (unsigned i = 0; ok && i < n; i++) pci_msix_mask(d, i, false);
        if (ok) pci_msix_function_mask(d, false);
    } else {
        ok = pci_msi_enable_multi(d, (uint8_t)base, n, dest);
    }
    if (!ok) { interrupts_off(c); return -N_ERR_UNSUPPORTED; }
    return n;
}

int64_t pci_user_interrupt_take(struct file *f, int64_t timeout_ms) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    uint64_t give_up = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        uint64_t irq = irq_save();
        uint32_t got = c->pending;
        c->pending = 0;
        if (got) { irq_restore(irq); return got; }
        if (current_task->killed) { irq_restore(irq); return -N_ERR_STOPPED; }
        if (!timeout_ms || !c->vectors) { irq_restore(irq); return c->vectors ? -N_ERR_TIMEOUT : -N_ERR_NOT_READY; }
        if (timeout_ms < 0) wq_wait(&c->wq);
        else {
            uint64_t now = uptime_ms();
            if (now >= give_up || !wq_wait_timeout(&c->wq, give_up - now)) {
                bool late = c->pending != 0;
                irq_restore(irq);
                if (!late) return -N_ERR_TIMEOUT;
                continue;
            }
        }
        irq_restore(irq);
    }
}
