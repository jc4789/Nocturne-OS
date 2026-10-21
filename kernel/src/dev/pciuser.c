/* Drivers in apps (docs/nocturne-native-abi.md §9). Only init claims a PCI function; the claim
   is a vnode whose handles init passes to the driver app it launches. The app reads config space
   (its device, the bridges above it with their devices, the host bridges), writes its own, maps BARs as
   shared objects (syscall.c) and waits for its message-signalled interrupts on the handle.
   The kernel keeps what decides where memory and interrupts go: BAR registers and the MSI and
   MSI-X capabilities are written here only. Where an IOMMU is on (dev/iommu.h) the function's
   DMA reaches only the DMA objects made through its claim. The last handle's close stops the
   function: every mapping of its BARs, in any process, loses the device pages; then no
   interrupts, no bus mastering, and the IOMMU blocks it, after which the app's DMA memory can
   return to the allocator. Without a confirmed IOMMU block, DMA pages that outlive the claim
   stay out of use for good: neither BME nor Transactions Pending proves every posted write
   arrived. Release never waits for a failed device or resets it (syscall.c asks
   pci_user_dma_reusable).
   BAR and DMA objects hold the claim record, so a later claim of the same function is a new
   record and the old objects stay dead. */
#include "kernel.h"
#include "abi.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "dev/fb.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "fs/vfs.h"
#include "gui/wm.h"
#include "mm/heap.h"
#include "sys/proc.h"
#include "sys/sched.h"
#include "sys/syscall.h"

#define CLAIM_VECTORS 32 /* one pending bit each */
#define CLAIM_GUARDS 32  /* registers with bits only the kernel changes (guards_init) */

#define PCI_COMMAND_MEMORY (1u << 1)
#define PCI_COMMAND_PARITY (1u << 6)

/* A config dword some of whose bits are the kernel's: a write that would change them is refused. */
struct guard {
    uint16_t offset;
    uint32_t bits;
};

struct pci_claim {
    struct pci_device *d;
    struct vnode *v;
    volatile uint32_t pending; /* message n arrived; cleared by take */
    struct wait_queue wq;
    unsigned vector_base, vectors;
    unsigned refs;   /* the vnode and every BAR or DMA object made through it */
    bool released;   /* the last handle closed: BAR objects are revoked, the function stopped */
    bool dma_unsafe; /* the ended claim's IOMMU block was not confirmed: retain its DMA pages */
    unsigned screen_holds; /* n_pci_hold_screen nesting; given back when the claim ends */
    struct guard guards[CLAIM_GUARDS];
    unsigned guard_count;
    uint16_t pmcsr; /* the power management capability's control register, or 0 */
    struct iommu_domain *domain; /* where an IOMMU isolates the function (dev/iommu.h) */
};
static struct pci_claim *claim_of_vector[256];

static void claim_drop(struct pci_claim *c) {
    ASSERT(c->refs);
    if (--c->refs) return;
    if (c->domain) iommu_domain_free(c->domain); /* every DMA object of the claim is gone */
    kfree(c);
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
    bool cleared = pci_msi_disable(c->d);
    uint64_t irq = irq_save();
    for (unsigned i = 0; i < c->vectors; i++) claim_of_vector[c->vector_base + i] = NULL;
    c->pending = 0;
    irq_restore(irq);
    /* a remapping entry the units may still hold could reach them: they stay taken, unhandled */
    if (cleared) irq_vector_free(c->vector_base, c->vectors);
    else kprintf("pci: %02x:%02x.%u: its interrupt vectors stay out of use\n", c->d->bus, c->d->dev, c->d->func);
    c->vectors = 0;
    /* A waiter must observe NOT_READY instead of sleeping forever after the
       driver disables its last interrupt (including an infinite take). */
    wq_wake_all(&c->wq);
    vfs_poll_notify(c->v);
}

static bool claim_can_read(struct vnode *v, struct file *f) {
    (void)f;
    return ((struct pci_claim *)v->priv)->pending != 0;
}

/* The memory BAR the boot screen is drawn through, or -1. */
static int screen_bar(const struct pci_device *d) {
    if (!fb.addr) return -1;
    for (unsigned i = 0; i < pci_bar_count(d); i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) continue;
        uint64_t address = pci_bar_address(d, i), size = pci_bar_size(d, i);
        if (address && size && fb.phys >= address && fb.phys - address < size) return (int)i;
        if (((raw >> 1) & 3) == 2) i++;
    }
    return -1;
}

/* ---- a driver that holds the screen (n_pci_hold_screen, dev/fb.c) ----
   Only the function the boot screen lies in can hold it, so one claim at most does. Its hold
   ends when the driver gives it back or when the claim ends; claim_release stops the function
   first, so drawing never resumes while the driver can still reach the device. A driver stuck
   while it holds is ended the same way: the user's Ctrl+Alt+Esc (gui/wm.c) stops the driver's
   app, and its claim's end gives the screen back; so does a hold that lasts as long as boot
   option fbhold=<seconds> says. No time ends a hold by itself: RM's waits follow one another
   without a bound a limit could be checked against, and stopping a driver that is only slow
   loses its GPU, so only the user, who sees the screen, decides that it is stuck. */
static struct pci_claim *screen_holder; /* BSP, interrupts off */
static int screen_holder_app;
static uint64_t screen_held_since, screen_hold_limit_ms;
static bool screen_holder_stopped, screen_watch_started;
static struct wait_queue screen_watch_wq;

/* Stops the app whose driver holds the screen, once a hold: false when none does. */
static bool screen_holder_stop(const char *why) {
    uint64_t irq = irq_save();
    int app = screen_holder && !screen_holder_stopped ? screen_holder_app : 0;
    struct pci_device *d = app ? screen_holder->d : NULL;
    if (app) screen_holder_stopped = true;
    irq_restore(irq);
    if (!app) return false;
    kprintf("pci: %02x:%02x.%u: %s; stopping app %d; framebuffer writes resume at release, device recovery may be needed\n", d->bus,
            d->dev, d->func, why, app);
    if (task_kill(app) < 0) kprintf("pci: app %d cannot be stopped; the screen stays held\n", app);
    return true;
}

bool pci_user_screen_take_back(void) { return screen_holder_stop("Ctrl+Alt+Esc takes the screen back from its driver"); }

static void screen_watch(void *arg) {
    (void)arg;
    char why[64];
    ksnprintf(why, sizeof why, "its driver has held the screen for %lu s (fbhold=)",
              (unsigned long)(screen_hold_limit_ms / 1000));
    for (;;) {
        uint64_t irq = irq_save();
        bool stop = false;
        uint64_t held = screen_holder ? uptime_ms() - screen_held_since : 0;
        if (!screen_holder || screen_holder_stopped) wq_wait(&screen_watch_wq);
        else if (held < screen_hold_limit_ms) wq_wait_timeout(&screen_watch_wq, screen_hold_limit_ms - held);
        else stop = true;
        irq_restore(irq);
        if (stop) screen_holder_stop(why);
    }
}

static void screen_hold_begin(struct pci_claim *c) {
    if (!screen_watch_started) {
        screen_watch_started = true;
        uint64_t seconds;
        if (cmdline_number("fbhold", &seconds) && seconds)
            screen_hold_limit_ms = seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
        if (screen_hold_limit_ms) kthread_create("screen hold", screen_watch, NULL);
    }
    uint64_t irq = irq_save();
    screen_holder = c;
    screen_holder_app = task_process()->pid;
    screen_held_since = uptime_ms();
    screen_holder_stopped = false;
    wq_wake_all(&screen_watch_wq);
    irq_restore(irq);
}

static void screen_hold_end(struct pci_claim *c) {
    uint64_t irq = irq_save();
    if (screen_holder == c) screen_holder = NULL;
    wq_wake_all(&screen_watch_wq);
    irq_restore(irq);
}

/* A driver that is done with its device stops it and then switches bus mastering off: that is
   how it tells the kernel the device will not reach memory again. A device that still masters
   when its claim ends lost its driver in the middle (a crash, or a driver that never stopped
   it), and what runs on it may be running still: a GPU's own firmware keeps the addresses it was
   given, and switching bus mastering off only pauses it until the next driver switches it on.
   Record that recovery is needed, but do not wait for it in process exit. A later non-display
   claim attempts FLR; the boot display cannot be generically reset without losing its scanout.
   Where an IOMMU isolates the function, a confirmed block settles DMA lifetime. Otherwise BME
   clear and no pending non-posted request do not prove that posted requests using other TCs,
   relaxed ordering or ID ordering have arrived (PCIe 6.0, 6.4). Keep the claim's remaining DMA
   pages even after a clean driver close; FLR success later is not proof of their drain either. */
static void claim_release(struct vnode *v) {
    struct pci_claim *c = v->priv;
    struct pci_device *d = c->d;
    c->released = true;
    shared_revoke_claim(c); /* no process reaches the registers any more */
    interrupts_off(c);
    bool left_running;
    bool stopped = pci_disable_busmaster(d, &left_running);
    if (c->domain && c->domain->nmembers > 1) {
        /* P2P lets the old driver program every member. FLR resets only its target
           Function (PCIe 6.0, 6.6.2), not its peers: even a clean primary must be
           recovered if it becomes another owner's peer. Never wait/reset at exit. */
        for (unsigned i = 0; i < c->domain->nmembers; i++) {
            struct pci_device *peer = c->domain->members[i];
            peer->needs_reset = true;
            if (peer != d && !pci_disable_busmaster(peer, NULL))
                kprintf("pci: %02x:%02x.%u: peer bus mastering did not stop; group recovery is required\n",
                        peer->bus, peer->dev, peer->func);
        }
    }
    bool blocked = c->domain && iommu_block(c->domain);
    bool running = left_running || !stopped || !pci_requests_done(d, 0);
    d->needs_reset |= running;
    c->dma_unsafe = !blocked;
    if (running)
        kprintf("pci: %02x:%02x.%u: %s; %s%s\n", d->bus, d->dev, d->func,
                left_running ? "its driver ended without stopping it (bus mastering was on)"
                : !stopped   ? "it does not stop"
                             : "it still has requests in flight",
                blocked ? "the IOMMU keeps it from memory" : "its DMA memory stays out of use",
                "; recovery is deferred to the next driver, not process exit");
    else if (!blocked)
        kprintf("pci: %02x:%02x.%u: bus mastering is off, but posted DMA drain is unconfirmed; remaining DMA memory stays out of use\n",
                d->bus, d->dev, d->func);
    /* Revoke CPU access before returning screen holds. Resuming CPU framebuffer writes is only
       a best-effort redraw: neither BME clear nor IOMMU blocking rebuilds a GPU's BAR/scanout. */
    if (c->screen_holds) screen_hold_end(c);
    for (; c->screen_holds; c->screen_holds--)
        if (fb_release()) wm_repaint();
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
uint64_t pci_user_dma_limit(const struct pci_claim *c) { return iommu_dma_limit(c->domain); }
bool pci_user_dma_map(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->domain || iommu_map(c->domain, frames, base, count);
}
bool pci_user_dma_unmap(struct pci_claim *c, const uint64_t *frames, uint64_t base, size_t count) {
    return !c->domain || iommu_unmap(c->domain, frames, base, count);
}

static void guard(struct pci_claim *c, uint32_t offset, uint32_t bits) {
    ASSERT(!(offset & 3) && c->guard_count < CLAIM_GUARDS);
    c->guards[c->guard_count++] = (struct guard){(uint16_t)offset, bits};
}

/* The registers a driver uses but some of whose bits decide things beyond its device: how the
   function reaches the rest of the system, resets it, or changes what its BARs decode. The
   whole-register ones (BARs, MSI, MSI-X) are kernel_register's. RM tolerates a refused write: it
   does not check what the platform's config accessors return. */
static void guards_init(struct pci_claim *c) {
    struct pci_device *d = c->d;
    /* Command: memory decode, bus mastering and parity response are the driver's. I/O decode
       (apps reach no I/O ports), INTx (apps get no legacy interrupt; one left asserting would
       hold a shared line) and the legacy PCI bits stay as the kernel set them. */
    guard(c, PCI_COMMAND, 0xFFFFu & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_PARITY));
    guard(c, 0x0C, 1u << 30); /* BIST: start */
    guard(c, 0x3C, 0xFF);     /* Interrupt Line: the platform's routing record */
    uint8_t pm = pci_find_cap(d, 0x01);
    if (pm) {
        c->pmcsr = pm + 4;
        guard(c, c->pmcsr, 3); /* power state: sleep belongs to dev/pm.c; waking to D0 is let through */
    }
    if (d->pcie_cap) {
        /* Device Control: Max_Payload_Size must match the whole path to the root, phantom
           functions widen the requester IDs, and Initiate FLR resets the function. */
        guard(c, d->pcie_cap + PCIE_DEVCTL, 7u << 5 | 1u << 9 | PCIE_DEVCTL_FLR);
        guard(c, d->pcie_cap + PCIE_LNKCTL, PCIE_LNKCTL_DISABLE);
        guard(c, d->pcie_cap + PCIE_LNKCTL2, PCIE_LNKCTL2_COMPLIANCE | PCIE_LNKCTL2_MOD_COMPLIANCE);
    }
    /* Extended capabilities (ECAM only): BAR sizes, virtual functions, and the controls that
       decide how its requests are routed and translated. */
    static const uint16_t sized[] = {0x15, 0x24}; /* Resizable BAR, VF Resizable BAR */
    for (unsigned k = 0; k < 2; k++) {
        uint16_t rebar = pci_find_ext_cap(d, sized[k]);
        if (!rebar) continue;
        unsigned bars = (pci_read32(d->bus, d->dev, d->func, rebar + 8) >> 5) & 7;
        for (unsigned i = 0; i < bars && i < 6; i++) guard(c, rebar + 8 + 8 * i, 0x3F00); /* BAR Size */
    }
    uint16_t sriov = pci_find_ext_cap(d, 0x10);
    if (sriov) {
        guard(c, sriov + 0x08, 0xFFFF); /* SR-IOV Control */
        guard(c, sriov + 0x10, 0xFFFF); /* NumVFs */
    }
    uint16_t acs = pci_find_ext_cap(d, 0x0D), ats = pci_find_ext_cap(d, 0x0F);
    uint16_t pri = pci_find_ext_cap(d, 0x13), pasid = pci_find_ext_cap(d, 0x1B);
    if (acs) guard(c, acs + 4, 0xFFFF0000);   /* ACS Control */
    if (ats) guard(c, ats + 4, 0xFFFF0000);   /* ATS Control */
    if (pri) guard(c, pri + 4, 0x3);          /* Page Request Control: enable, reset */
    if (pasid) guard(c, pasid + 4, 0x70000);  /* PASID Control */
}

/* A write of `width` bytes at `offset` would change a bit only the kernel changes. */
static bool guarded(const struct pci_claim *c, uint32_t offset, uint32_t width, uint32_t value) {
    uint32_t dword = offset & ~3u, shift = (offset & 3) * 8;
    uint32_t written = width == 4 ? ~0u : ((1u << (width * 8)) - 1) << shift;
    for (unsigned i = 0; i < c->guard_count; i++) {
        if (c->guards[i].offset != dword) continue;
        const struct pci_device *d = c->d;
        uint32_t old = pci_read32(d->bus, d->dev, d->func, dword);
        uint32_t now = (old & ~written) | ((value << shift) & written);
        uint32_t bits = c->guards[i].bits;
        if (dword == c->pmcsr && !(now & 3)) bits &= ~3u; /* back to D0 */
        if ((old ^ now) & bits) return true;
    }
    return false;
}

/* No app has a handle yet. The attached domain reserves every member across FLR sleeps;
   the primary is reserved by the caller too. A fresh peer must stop mastering even when
   it has not belonged to an app. An old app's peer also needs FLR: BME off pauses, not
   erases, its retained DMA state. Only the new primary can delegate missing generic reset
   to its native driver; that driver cannot enumerate/recover a dirty display/FLR-less peer. */
static bool claim_recover(struct pci_claim *c) {
    struct pci_device *d = c->d;
    unsigned count = c->domain ? c->domain->nmembers : 1;
    if (d->needs_reset && !c->domain) return false;
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *m = c->domain ? c->domain->members[i] : d;
        if (current_task && current_task->killed) return false;
        /* The earlier caller may have left before the FLR wait finished. Rejoin that
           saved operation before BME/MSI accesses: Requests can be discarded during
           FLR (6.6.2), so their readback is not a replacement for its remaining wait. */
        if (m->reset) {
            if (!pci_function_reset(m) || !pci_msi_disable(m)) return false;
            m->needs_reset = false;
        }
        if (m != d && !pci_disable_busmaster(m, NULL) && !m->needs_reset) {
            m->needs_reset = true;
            return false;
        }
        if (!m->needs_reset) continue;
        if (!pci_msi_disable(m)) return false; /* do not preserve an old MSI Enable in FLR state */
        bool screen = screen_bar(m) >= 0;
        bool flr = m->reset || (m->pcie_cap &&
            (pci_read32(m->bus, m->dev, m->func, m->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR));
        if (!screen && flr) {
            if (!pci_function_reset(m)) return false;
            m->needs_reset = false;
        } else if (m != d) {
            return false;
        } else {
            if (!pci_disable_busmaster(d, NULL)) return false;
            kprintf("pci: %02x:%02x.%u: isolated, but native driver recovery is still required%s\n",
                    d->bus, d->dev, d->func, screen ? "; boot display is not automatically reset" : "");
        }
    }
    return !(current_task && current_task->killed);
}

int64_t pci_user_claim(uint8_t bus, uint8_t dev, uint8_t func, struct file **out) {
    struct task *t = task_process();
    /* Boot options: noappdrivers keeps every function with the kernel; appdrivers=any lets any
       app claim (tests drive QEMU's edu device from a program they compile) */
    if (!t || cmdline_has("noappdrivers")) return -N_ERR_DENIED;
    if (t->pid != init_pid && !cmdline_has("appdrivers=any")) return -N_ERR_DENIED;
    if (dev > 31 || func > 7) return -N_ERR_INVALID;
    struct pci_device *d = pci_find_function(bus, dev, func);
    if (!d) return -N_ERR_NOT_FOUND;
    if (d->kernel_owned || d->claimed) return -N_ERR_BUSY;
    /* A bridge's registers route other devices' memory, buses and resets. */
    if (d->cls == 0x06) return -N_ERR_DENIED;
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
    /* From here the function reaches only the DMA objects made through the claim. */
    int r = iommu_attach(d, &c->domain);
    if (r < 0) { kfree(c); kfree(v); return r; }
    d->claimed = true;
    if (!claim_recover(c)) {
        /* FLR may still be pending on any member: retain its device-owned saved state,
           expose no handle, and re-block the whole group before dropping the reservation. */
        if (c->domain) iommu_domain_free(c->domain);
        d->claimed = false;
        kfree(c);
        kfree(v);
        return -N_ERR_NOT_READY;
    }
    guards_init(c); /* pending FLR is complete: capability offsets/control now describe the handle */
    /* BAR sizes come from enumeration (pci_bar_size): measuring them here would switch memory
       decoding off under a running function, and the boot screen may be drawn through it. */
    struct file *f = vfs_open_vnode(v, O_RDWR);
    if (!f) {
        if (c->domain) iommu_domain_free(c->domain);
        d->claimed = false;
        kfree(c);
        kfree(v);
        return -N_ERR_NO_MEMORY;
    }
    d->claimed = true;
    kprintf("pci: %02x:%02x.%u (%04x:%04x) claimed by pid %d%s\n", bus, dev, func, d->vendor, d->device, t->pid,
            c->domain ? "" : iommu_active() ? "; the IOMMU cannot isolate it: its DMA reaches all of memory"
                                            : "; no IOMMU isolates it: its DMA reaches all of memory");
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
    struct pci_device *up = pci_upstream(d);
    out->upstream = up ? N_PCI_TARGET(up->bus, up->dev, up->func) : N_PCI_NONE;
    out->flags = c->domain ? N_PCI_ISOLATED : 0;
    if (d->needs_reset) out->flags |= N_PCI_NEEDS_RECOVERY;
    for (unsigned i = 0; i < pci_bar_count(d); i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) { out->bar[i] = (struct n_pci_bar){.address = raw & ~3u, .flags = N_PCI_BAR_IO}; continue; }
        uint64_t address = pci_bar_address(d, i), size = pci_bar_size(d, i);
        if (!address || !size) continue;
        uint32_t flags = N_PCI_BAR_MEMORY | (raw & 8 ? N_PCI_BAR_PREFETCH : 0);
        bool wide = ((raw >> 1) & 3) == 2;
        if (wide) flags |= N_PCI_BAR_64BIT;
        out->bar[i] = (struct n_pci_bar){.address = address, .size = size, .flags = flags};
        if (wide) i++; /* the upper half has no BAR of its own */
    }
    out->framebuffer_bar = N_PCI_NONE;
    int screen = screen_bar(d);
    if (screen >= 0) {
        out->framebuffer_address = fb.phys;
        out->framebuffer_size = (uint64_t)fb.pitch * fb.height;
        out->framebuffer_bar = (uint32_t)screen;
        out->framebuffer_width = fb.width;
        out->framebuffer_height = fb.height;
        out->framebuffer_pitch = fb.pitch;
        out->framebuffer_bpp = fb.bpp;
    }
    return 0;
}

int64_t pci_user_hold_screen(struct file *f, bool hold) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (screen_bar(c->d) < 0) return -N_ERR_NOT_FOUND;
    if (hold) {
        if (c->screen_holds == UINT32_MAX) return -N_ERR_BUSY;
        if (!c->screen_holds++) screen_hold_begin(c);
        fb_hold();
        return 0;
    }
    if (!c->screen_holds) return -N_ERR_INVALID;
    if (!--c->screen_holds) screen_hold_end(c);
    if (fb_release()) wm_repaint();
    return 0;
}

static bool config_access_ok(uint32_t offset, uint32_t width) {
    return (width == 1 || width == 2 || width == 4) && offset < 4096 && !(offset & (width - 1));
}
/* Reads reach the path from the host to the claimed function, but no unrelated device:
   - the function and the other functions of its device (an audio function on a graphics card);
   - each bridge above it with the other functions of that bridge's device. A driver scans a bus
     device by device and gives up on a device whose function 0 does not answer, and a root port
     is often function 1 or later beside a host-bridge function 0;
   - the host bridges, which tell the driver what chipset the path starts at. */
static bool is_host_bridge(const struct pci_device *d) {
    return ((pci_read32(d->bus, d->dev, d->func, 0x08) >> 16) & 0xFFFF) == 0x0600;
}
static bool may_read(const struct pci_device *d, const struct pci_device *target) {
    if (target->bus == d->bus && target->dev == d->dev) return true;
    for (const struct pci_device *b = pci_upstream(d); b; b = pci_upstream(b))
        if (target->bus == b->bus && target->dev == b->dev) return true;
    return is_host_bridge(target);
}

int64_t pci_user_config_read(struct file *f, uint32_t target, uint32_t offset, uint32_t width) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *t = c->d;
    if (target != N_PCI_SELF) {
        if (target > 0xFFFF) return -N_ERR_INVALID;
        t = pci_find_function(target >> 8, (target >> 3) & 31, target & 7);
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

/* A reset (the GPU fell off the bus and came back, or left a power state) clears the BARs, and
   the driver puts back the addresses it saw at boot one dword at a time. That write is let
   through when it is exactly the address the kernel found there (d->bar): a BAR can be put back,
   never moved. Half way through a 64-bit BAR it decodes at a mixed address, but only requests
   inside the bridge window above it reach it, so that touches no other device. */
static bool bar_restore(const struct pci_device *d, uint32_t offset, uint32_t width, uint32_t value) {
    return width == 4 && offset >= 0x10 && offset < 0x10 + 4 * pci_bar_count(d) &&
           value == d->bar[(offset - 0x10) / 4];
}

int64_t pci_user_config_write(struct file *f, uint32_t offset, uint32_t width, uint32_t value) {
    struct pci_claim *c = claim_of(f);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *d = c->d;
    if (kernel_register(d, offset, width) && !bar_restore(d, offset, width, value)) return -N_ERR_DENIED;
    if (guarded(c, offset, width, value)) return -N_ERR_DENIED;
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
    uint64_t a = pci_bar_address(c->d, bar), s = pci_bar_size(c->d, bar);
    if (!a || !s) return -N_ERR_NOT_FOUND;
    /* A smaller BAR shares its page with whatever lies beside it. */
    if ((a | s) & (PAGE_SIZE - 1)) return -N_ERR_UNSUPPORTED;
    *address = a;
    *size = s;
    return 0;
}
bool pci_user_bar_prefetchable(struct file *f, uint32_t bar) {
    struct pci_claim *c=claim_of(f);
    return c && bar<6 && !(c->d->bar[bar]&1) && (c->d->bar[bar]&8);
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
