/* PCI の claim（io/io.h の struct io_claim_ops）：アプリの中のドライバーが PCI の機能を持つとき、バスが持つ分。
   docs/nocturne-native-abi.md §9。BAR の写し、割り込み、台帳への提供、眠りの知らせは io/claim.c が持ち、
   ここは機能の隔離、構成空間、リセットと回復、DMA の IOMMU を受け持つ。画面の口は dev/screen_driver.c、firmware の
   記述の口は dev/acpi_claim.c。
   カーネルが握るもの（BAR のレジスター、MSI と MSI-X の capability）は、ここだけが書く。IOMMU が付いていれば、
   機能の DMA は claim で作った DMA の object にしか届かない。claim の終わりは、機能を止めてから（BAR の写像の取消し、
   割り込み、バスマスター）IOMMU の遮断を確かめ、その後に DMA のメモリーが戻る。遮断を確かめられなければ、残る
   DMA の頁は二度と使わない：BME を切っても Transactions Pending が無くても、posted の書込みが全部届いた証拠に
   ならない。終わりは故障した装置を待たず、リセットもしない（pci_end が false を返し、io_claim_dma_reusable が
   偽になる）。
   claim の開始と終わり、全機能の予約（d->claimed）は claim_mutex が一度に一つにする。claim_mutex は pci.c のリセット
   と dev/iommu.c の錠より先に取る。 */
#include "kernel.h"
#include "abi.h"
#include "dev/fb.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "io/io.h"
#include "mm/heap.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define CLAIM_GUARDS 32     /* registers with bits only the kernel changes (guards_init) */

/* A config dword some of whose bits are the kernel's: a write that would change them is refused. */
struct guard {
    uint16_t offset;
    uint32_t bits;
};

struct pci_claim {
    struct io_claim *claim;
    struct pci_device *d;
    struct guard guards[CLAIM_GUARDS];
    unsigned guard_count;
    struct iommu_domain *domain; /* where an IOMMU isolates the function (dev/iommu.h) */
    struct pci_device *reset_port; /* where this claim's attempt sent a Secondary Bus Reset (bus_reset) */
};

#define PCI_OF(io_device) ((struct pci_device *)((char *)(io_device) - offsetof(struct pci_device, hal)))

/* Claiming and a claim's end, and every function's d->claimed (dev/pci.h): one at a time, as a
   claim's resets and its group's reservations span sleeps. Before pci.c's and dev/iommu.c's locks. */
static struct mutex claim_mutex;

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
    if (pm) guard(c, pm + 4, 3); /* PowerState：D 状態は装置の管理者のもの（io_bus_ops.power）。claim には D0 で渡る */
    if (d->pcie_cap) {
        /* Device Control: Max_Payload_Size must match the whole path to the root, phantom
           functions widen the requester IDs, and Initiate FLR resets the function. */
        guard(c, d->pcie_cap + PCIE_DEVCTL, 7u << 5 | 1u << 9 | PCIE_DEVCTL_FLR);
        guard(c, d->pcie_cap + PCIE_LNKCTL, PCIE_LNKCTL_DISABLE);
        if (d->pcie_v2)
            guard(c, d->pcie_cap + PCIE_LNKCTL2, PCIE_LNKCTL2_COMPLIANCE | PCIE_LNKCTL2_MOD_COMPLIANCE);
    }
    /* Extended capabilities (ECAM only): BAR sizes, virtual functions, and the controls that
       decide how its requests are routed and translated. */
    static const uint16_t sized[] = {0x15, 0x24}; /* Resizable BAR, VF Resizable BAR */
    for (unsigned k = 0; k < 2; k++) {
        uint16_t rebar = pci_find_ext_cap(d, sized[k]);
        if (!rebar) continue;
        unsigned bars = (pci_read32(d->address, rebar + 8) >> 5) & 7;
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
        uint32_t old = pci_read32(d->address, dword);
        uint32_t now = (old & ~written) | ((value << shift) & written);
        if ((old ^ now) & c->guards[i].bits) return true;
    }
    return false;
}

/* A Secondary Bus Reset (dev/pci.h) for m, which its FLR did not bring back, at the narrowest
   Downstream Port above it: once per port in a claim's attempt. Everything below that port is
   reset, so only where all of it is the claim's (its function, m, its domain's members) or in no
   one's use: never a function a kernel driver runs, the one the boot screen is drawn through
   (its scanout would be lost), one another claim holds or one in another domain. The others are
   reserved meanwhile. False when it was not done. */
static bool claim_owns(const struct pci_claim *c, const struct pci_device *m, const struct pci_device *f) {
    return f == c->d || f == m || (c->domain && iommu_member(c->domain, f));
}

/* claim_mutex held. */
static void others_reserve(const struct pci_claim *c, const struct pci_device *m, const struct pci_device *port,
                           bool reserve) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *f = pci_at(i);
        if (pci_below(port, f) && !claim_owns(c, m, f)) f->claimed = reserve;
    }
}

static bool bus_reset(struct pci_claim *c, struct pci_device *m) {
    struct pci_device *port = pci_bus_reset_port(m);
    if (!port || port == c->reset_port || thread_current()->killed) return false;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *f = pci_at(i);
        if (!pci_below(port, f)) continue;
        bool ours = claim_owns(c, m, f);
        const char *why = !ours && f->kernel_owned     ? "a kernel driver runs it"
                          : screen_driver_boot_slot(&f->hal) >= 0         ? "the boot screen is drawn through it"
                          : !ours && f->claimed        ? "it is claimed"
                          : !ours && iommu_in_use(f)   ? "it is in another claim's domain"
                                                       : NULL;
        if (why) {
            kprintf("pci: %04x:%02x:%02x.%x: no Secondary Bus Reset at %04x:%02x:%02x.%x: %04x:%02x:%02x.%x is below it and %s\n",
                    PCI_ADDRESS_ARGS(m->address), PCI_ADDRESS_ARGS(port->address), PCI_ADDRESS_ARGS(f->address), why);
            return false;
        }
    }
    c->reset_port = port;
    others_reserve(c, m, port, true);
    bool done = pci_bus_reset(port);
    others_reserve(c, m, port, false);
    return done;
}

/* Completes a reset an earlier caller left (cancelled, or the function not ready then), with
   MSI off: the saved configuration may hold an old MSI Enable. A function its FLR did not bring
   back gets a Secondary Bus Reset where one may be done (bus_reset), and its FLR again after it:
   a Conventional Reset does not stop all that an FLR stops (PCIe 6.0, 6.6.2). */
static bool reset_finish(struct pci_claim *c, struct pci_device *m) {
    bool done = pci_function_reset(m);
    if (!done && pci_reset_stuck(m) && bus_reset(c, m)) done = pci_function_reset(m);
    if (!done || !pci_irq_disable(m)) return false;
    m->needs_reset = false;
    return true;
}

/* claim_mutex held, from here to claim_form.
   The claim's domain (iommu_attach). Forming the group reads its functions' ACS, which no
   access but a reset's own may reach while one is under reset (dev/pci.h): such a reset is
   finished first, its function reserved meanwhile, and the group formed again. Only the
   group's own resets: a claim does not wait on other functions'. One that another claim or
   domain holds, or that does not finish, leaves the claim -N_ERR_NOT_READY; one that does not
   come back is taken as without ACS (dev/iommu.c), which claim_recover settles. */
static int claim_attach(struct pci_claim *c) {
    for (;;) {
        struct pci_device *m;
        int r = iommu_attach(c->d, &c->domain, &m);
        if (r != -N_ERR_NOT_READY || !m) return r;
        bool peer = m != c->d; /* c->d is reserved by the caller */
        if (peer && (m->claimed || m->kernel_owned || iommu_in_use(m))) return r;
        if (peer) m->claimed = true;
        bool done = reset_finish(c, m);
        if (peer) m->claimed = false;
        if (!done && m->reset && !pci_reset_stuck(m)) return r;
    }
}

/* No app has the claim yet. The attached domain reserves every member across FLR sleeps;
   the primary is reserved by the caller too. A fresh peer must stop mastering even when
   it has not belonged to an app. An old app's peer also needs FLR: BME off pauses, not
   erases, its retained DMA state. Only the new primary can delegate missing generic reset
   to its native driver; that driver cannot enumerate/recover a dirty display/FLR-less peer.
   A peer that does not come back from its resets is left where it is: its bus mastering is
   off (cleared before its FLR, or by the reset), whatever it sends stays in the claim's domain,
   and the kernel reaches it only through its reset (dev/pci.h). The primary must come back. */
static bool claim_recover(struct pci_claim *c) {
    struct pci_device *d = c->d;
    unsigned count = c->domain ? c->domain->nmembers : 1;
    if (d->needs_reset && !c->domain) return false;
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *m = c->domain ? c->domain->members[i] : d;
        if (thread_current()->killed) return false;
        /* A reset left unfinished that forming the group did not read (claim_attach). Rejoin
           it before BME/MSI accesses: Requests can be discarded during an FLR (6.6.2), so their
           readback is not a replacement for its remaining wait. */
        if (m->reset && !reset_finish(c, m)) {
            if (m != d && pci_reset_stuck(m)) continue;
            return false;
        }
        if (m != d && !pci_disable_busmaster(m, NULL) && !m->needs_reset) {
            m->needs_reset = true;
            return false;
        }
        if (!m->needs_reset) continue;
        if (!pci_irq_disable(m)) return false; /* do not preserve an old MSI Enable in FLR state */
        bool screen = screen_driver_boot_slot(&m->hal) >= 0;
        bool flr = m->pcie_cap && (pci_read32(m->address, m->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR);
        if (!screen && flr) {
            if (!reset_finish(c, m)) {
                if (m != d && pci_reset_stuck(m)) continue;
                return false;
            }
        } else if (m != d) {
            return false;
        } else {
            if (!pci_disable_busmaster(d, NULL)) return false;
            kprintf("pci: %04x:%02x:%02x.%x: isolated, but native driver recovery is still required%s\n",
                    PCI_ADDRESS_ARGS(d->address), screen ? "; boot display is not automatically reset" : "");
        }
    }
    /* A bus reset for one member resets the others below its port too: one it brought back
       still owes its FLR, and one may not have come back from it. */
    for (unsigned i = 0; i < count; i++) {
        struct pci_device *m = c->domain ? c->domain->members[i] : d;
        if (m->reset && !pci_reset_stuck(m) && !reset_finish(c, m) && !pci_reset_stuck(m)) return false;
        if (!m->reset) continue;
        if (m == d) return false;
        m->needs_reset = true;
        kprintf("pci: %04x:%02x:%02x.%x does not answer after its resets: left as it is, bus mastering off, what it "
                "sends kept in the domain of %04x:%02x:%02x.%x\n", PCI_ADDRESS_ARGS(m->address), PCI_ADDRESS_ARGS(d->address));
    }
    return !thread_current()->killed;
}


/* claim_mutex held: the claim formed, or nothing of it left and d free again. */
static int claim_form(struct pci_claim *c) {
    struct pci_device *d = c->d;
    d->claimed = true; /* reserved across the resets' waits */
    /* From here the function reaches only the DMA objects made through the claim. */
    int r = claim_attach(c);
    if (r < 0) {
        d->claimed = false;
        return r;
    }
    if (!claim_recover(c)) {
        /* FLR may still be pending on any member: retain its device-owned saved state,
           expose no claim, and re-block the whole group before dropping the reservation. */
        if (c->domain) iommu_domain_free(c->domain);
        d->claimed = false;
        return -N_ERR_NOT_READY;
    }
    guards_init(c); /* pending FLR is complete: capability offsets/control now describe the claim */
    /* BAR sizes come from enumeration (pci_bar_size): measuring them here would switch memory
       decoding off under a running function, and the boot screen may be drawn through it. */
    if (c->domain) iommu_guests_start(c->domain, true);
    return 0;
}

/* N_PCI_TARGET's function, of those enumeration found. */
static struct pci_device *target_function(uint32_t target) {
    return pci_find_function((struct pci_address){N_PCI_TARGET_SEGMENT(target), N_PCI_TARGET_BUS(target),
                                                  N_PCI_TARGET_DEV(target), N_PCI_TARGET_FUNC(target)});
}

static int64_t info(struct pci_claim *c, struct n_hal_pci_info *out) {
    struct pci_device *d = c->d;
    memset(out, 0, sizeof *out);
    out->segment = d->address.segment; out->bus = d->address.bus; out->dev = d->address.dev; out->func = d->address.func;
    out->revision = pci_read32(d->address, 0x08) & 0xFF;
    out->vendor = d->vendor; out->device = d->device;
    uint32_t sub = pci_read32(d->address, 0x2C);
    out->subsystem_vendor = sub & 0xFFFF; out->subsystem_id = sub >> 16;
    out->cls = d->cls; out->subcls = d->subcls; out->progif = d->progif;
    out->interrupt_pin = (pci_read32(d->address, 0x3C) >> 8) & 0xFF;
    out->msi_vectors = pci_msi_max_vectors(d);
    out->msix_vectors = pci_msix_max_vectors(d);
    struct pci_device *up = pci_upstream(d);
    out->flags = c->domain ? N_PCI_ISOLATED : 0;
    if (up) {
        out->upstream = N_PCI_TARGET(up->address.segment, up->address.bus, up->address.dev, up->address.func);
        out->flags |= N_PCI_HAS_UPSTREAM;
    }
    if (d->needs_reset) out->flags |= N_PCI_NEEDS_RECOVERY;
    for (unsigned i = 0; i < pci_bar_count(d); i++) {
        uint32_t raw = d->bar[i];
        if (raw & 1) { out->bar[i] = (struct n_hal_pci_bar){.address = raw & ~3u, .flags = N_PCI_BAR_IO}; continue; }
        uint64_t address = pci_bar_address(d, i), size = pci_bar_size(d, i);
        if (!address || !size) continue;
        uint32_t flags = N_PCI_BAR_MEMORY | (raw & 8 ? N_PCI_BAR_PREFETCH : 0);
        bool wide = ((raw >> 1) & 3) == 2;
        if (wide) flags |= N_PCI_BAR_64BIT;
        out->bar[i] = (struct n_hal_pci_bar){.address = address, .size = size, .flags = flags};
        if (wide) i++; /* the upper half has no BAR of its own */
    }
    out->framebuffer_bar = N_PCI_NONE;
    int screen = screen_driver_boot_slot(&d->hal);
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


static bool config_access_ok(uint32_t offset, uint32_t width) {
    return (width == 1 || width == 2 || width == 4) && offset < 4096 && !(offset & (width - 1));
}
/* Reads reach the path from the host to the claimed function, but no unrelated device:
   - the function and the other functions of its device (an audio function on a graphics card);
   - each bridge above it with the other functions of that bridge's device. A driver scans a bus
     device by device and gives up on a device whose function 0 does not answer, and a root port
     is often function 1 or later beside a host-bridge function 0;
   - the host bridges, which tell the driver what chipset the path starts at. */
static bool is_host_bridge(const struct pci_device *d) { return d->cls == 0x06 && d->subcls == 0x00; }
static bool same_device_number(const struct pci_device *a, const struct pci_device *b) {
    return a->address.segment == b->address.segment && a->address.bus == b->address.bus && a->address.dev == b->address.dev;
}

static bool may_read(const struct pci_device *d, const struct pci_device *target) {
    if (same_device_number(target, d)) return true;
    for (const struct pci_device *b = pci_upstream(d); b; b = pci_upstream(b))
        if (same_device_number(target, b)) return true;
    return is_host_bridge(target);
}

static int64_t config_read(struct pci_claim *c, uint64_t target, uint32_t offset, uint32_t width) {
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *t = c->d;
    if (target != N_PCI_SELF) {
        if (target > UINT32_MAX) return -N_ERR_INVALID;
        t = target_function((uint32_t)target);
        if (!t) return -N_ERR_NOT_FOUND;
        if (!may_read(c->d, t)) return -N_ERR_DENIED;
    }
    uint32_t v;
    if (t == c->d) v = pci_read32(t->address, offset & ~3u);
    /* under an FLR (dev/pci.h): another claim's, or one an ended claim left for a later claim to
       finish; checked and read in one step against a reset beginning on another CPU */
    else if (!pci_config_read_unless_reset(t, (uint16_t)offset, &v)) return -N_ERR_NOT_READY;
    v >>= (offset & 3) * 8;
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

static int64_t config_write(struct pci_claim *c, uint32_t offset, uint32_t width, uint32_t value) {
    if (!config_access_ok(offset, width)) return -N_ERR_INVALID;
    struct pci_device *d = c->d;
    if (kernel_register(d, offset, width) && !bar_restore(d, offset, width, value)) return -N_ERR_DENIED;
    if (guarded(c, offset, width, value)) return -N_ERR_DENIED;
    if (width == 4) pci_write32(d->address, offset, value);
    else if (width == 2) pci_write16(d->address, offset, value);
    else pci_write8(d->address, offset, value);
    return 0;
}

static int pci_begin(struct io_claim *hc, void **bus) {
    struct pci_device *d = PCI_OF(io_claim_device(hc));
    /* A bridge's registers route other devices' memory, buses and resets. */
    if (d->cls == 0x06) return -N_ERR_DENIED;
    struct pci_claim *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    c->claim = hc;
    c->d = d;
    mutex_lock(&claim_mutex);
    int r = d->kernel_owned || d->claimed ? -N_ERR_BUSY : claim_form(c);
    mutex_unlock(&claim_mutex);
    if (r < 0) {
        kfree(c);
        return r;
    }
    if (!c->domain)
        kprintf("pci: %04x:%02x:%02x.%x (%04x:%04x)%s\n", PCI_ADDRESS_ARGS(d->address), d->vendor, d->device,
                iommu_active() ? ": the IOMMU cannot isolate it: its DMA reaches all of memory"
                               : ": no IOMMU isolates it: its DMA reaches all of memory");
    *bus = c;
    return 0;
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
   pages even after a clean driver close; FLR success later is not proof of their drain either.
   The mappings of its BARs and its interrupts are gone already (io/claim.c). */
static bool pci_end(struct io_claim *hc) {
    struct pci_claim *c = io_claim_bus(hc);
    struct pci_device *d = c->d;
    mutex_lock(&claim_mutex); /* no claim forms a group with these functions meanwhile */
    if (c->domain) iommu_guests_stop(c->domain);
    bool left_running;
    bool stopped = pci_disable_busmaster(d, &left_running);
    if (c->domain && c->domain->nmembers > 1) {
        /* P2P lets the old driver program every member. FLR resets only its target
           Function (PCIe 6.0, 6.6.2), not its peers: even a clean primary must be
           recovered if it becomes another owner's peer. Never wait/reset at exit. A peer
           left under reset (claim_recover) masters no more and is its reset's alone. */
        for (unsigned i = 0; i < c->domain->nmembers; i++) {
            struct pci_device *peer = c->domain->members[i];
            peer->needs_reset = true;
            if (peer != d && !peer->reset && !pci_disable_busmaster(peer, NULL))
                kprintf("pci: %04x:%02x:%02x.%x: peer bus mastering did not stop; group recovery is required\n",
                        PCI_ADDRESS_ARGS(peer->address));
        }
    }
    bool blocked = c->domain && iommu_block(c->domain);
    bool running = left_running || !stopped || !pci_requests_done(d);
    d->needs_reset |= running;
    if (running)
        kprintf("pci: %04x:%02x:%02x.%x: %s; %s%s\n", PCI_ADDRESS_ARGS(d->address),
                left_running ? "its driver ended without stopping it (bus mastering was on)"
                : !stopped   ? "it does not stop"
                             : "it still has requests in flight",
                blocked ? "the IOMMU keeps it from memory" : "its DMA memory stays out of use",
                "; recovery is deferred to the next driver, not process exit");
    else if (!blocked)
        kprintf("pci: %04x:%02x:%02x.%x: bus mastering is off, but posted DMA drain is unconfirmed; remaining DMA memory stays out of use\n",
                PCI_ADDRESS_ARGS(d->address));
    d->claimed = false;
    mutex_unlock(&claim_mutex);
    return blocked;
}

/* The last reference to the claim is gone, every DMA object of it too. */
static void pci_release(struct io_claim *hc) {
    struct pci_claim *c = io_claim_bus(hc);
    if (c->domain) iommu_domain_free(c->domain);
    kfree(c);
}

static unsigned pci_irq_max(struct io_claim *hc) {
    struct pci_device *d = PCI_OF(io_claim_device(hc));
    /* no legacy INTx for apps: one left asserting would hold a shared line */
    return MAX(pci_msix_max_vectors(d), pci_msi_max_vectors(d));
}

static unsigned pci_dma_bits(struct io_claim *hc) {
    struct pci_claim *c = io_claim_bus(hc);
    return iommu_dma_bits(c->domain);
}

static bool pci_dma_map(struct io_claim *hc, const uint64_t *frames, uint64_t base, size_t count) {
    struct pci_claim *c = io_claim_bus(hc);
    return !c->domain || iommu_map(c->domain, frames, base, count);
}

static bool pci_dma_unmap(struct io_claim *hc, const uint64_t *frames, uint64_t base, size_t count) {
    struct pci_claim *c = io_claim_bus(hc);
    return !c->domain || iommu_unmap(c->domain, frames, base, count);
}

/* The call's in and out are kernel copies. */
static int64_t pci_call(struct io_claim *hc, uint32_t op, const void *in, size_t in_size, void *out, size_t out_size,
                        size_t *used) {
    struct pci_claim *c = io_claim_bus(hc);
    switch (op) {
    case N_PCI_CALL_INFO:
        if (out_size < sizeof(struct n_hal_pci_info)) return -N_ERR_INVALID;
        *used = sizeof(struct n_hal_pci_info);
        return info(c, out);
    case N_PCI_CALL_CONFIG_READ:
    case N_PCI_CALL_CONFIG_WRITE: {
        const struct n_hal_pci_config *q = in;
        if (in_size != sizeof *q) return -N_ERR_INVALID;
        return op == N_PCI_CALL_CONFIG_READ ? config_read(c, q->target, q->offset, q->width)
                                            : config_write(c, q->offset, q->width, q->value);
    }
    }
    return -N_ERR_UNSUPPORTED;
}

const struct io_claim_ops pci_claim_ops = {
    .begin = pci_begin,
    .end = pci_end,
    .release = pci_release,
    .call = pci_call,
    .irq_max = pci_irq_max,
    .dma_bits = pci_dma_bits,
    .dma_map = pci_dma_map,
    .dma_unmap = pci_dma_unmap,
};
