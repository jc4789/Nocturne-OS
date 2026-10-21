/* PCI configuration space access (mechanism #1 and PCIe ECAM) and bus enumeration. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "abi.h"
#include "mm/heap.h"
#include "mm/vmm.h"

/* ---- ECAM (MCFG) ----
   Only segment 0 is used (struct pci_device has no segment). A function's 4 KiB config page is mapped
   (uncached) once enumeration has found it, so the 256 MiB window is never mapped as a whole. Until a
   function is active, offsets below 256 go through CF8/CFC, which is also what enumeration probes with.
   Without an MCFG, or if an ECAM read disagrees with CF8, everything stays on CF8/CFC. */
static const struct acpi_ecam *ecam;
static unsigned necam;
static bool ecam_off;
static uint8_t ecam_active[65536 / 8]; /* one bit per bus/dev/func */

static volatile uint32_t *ecam_regs(uint8_t bus, uint8_t dev, uint8_t func, bool map) {
    if (ecam_off) return NULL;
    for (unsigned i = 0; i < necam; i++) {
        if (ecam[i].segment != 0 || bus < ecam[i].start_bus || bus > ecam[i].end_bus) continue;
        unsigned bdf = (unsigned)bus << 8 | (unsigned)dev << 3 | func;
        uint64_t phys = ecam[i].base + ((uint64_t)(bus - ecam[i].start_bus) << 20 | (uint64_t)dev << 15 |
                                         (uint64_t)func << 12);
        if (!(ecam_active[bdf >> 3] & (1u << (bdf & 7)))) {
            if (!map) return NULL;
            vmm_map_mmio(phys, 4096);
            ecam_active[bdf >> 3] |= 1u << (bdf & 7);
        }
        return phys_to_virt(phys);
    }
    return NULL;
}

static uint32_t cf8_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, off >= 256);
    if (r) return r[(off & 0xFFC) >> 2];
    if (off >= 256) return 0xFFFFFFFFu;
    return cf8_read32(bus, dev, func, (uint8_t)off);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, off >= 256);
    if (r) {
        r[(off & 0xFFC) >> 2] = v;
        return;
    }
    if (off >= 256) return;
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2) * 8));
}

/* Narrow writes are narrow on the bus too: a read-modify-write of the whole dword would write
   back the register beside this one, and Status (beside Command) clears its error bits on a
   written 1. */
static void cf8_select(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t v) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, off >= 256);
    if (r) {
        *(volatile uint16_t *)((volatile uint8_t *)r + (off & 0xFFE)) = v;
        return;
    }
    if (off >= 256) return;
    cf8_select(bus, dev, func, off);
    outw(0xCFC + (off & 2), v);
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint8_t v) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, off >= 256);
    if (r) {
        *((volatile uint8_t *)r + (off & 0xFFF)) = v;
        return;
    }
    if (off >= 256) return;
    cf8_select(bus, dev, func, off);
    outb(0xCFC + (off & 3), v);
}

/* Switch a found function to ECAM, checking that ECAM really reaches it (a wrong MCFG base turns it off). */
static void ecam_activate(uint8_t bus, uint8_t dev, uint8_t func, uint32_t id) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, true);
    if (r && r[0] != id) {
        kprintf("pci: ECAM read of %02x:%02x.%x gave %08x, expected %08x; falling back to CF8/CFC\n", bus, dev,
                func, r[0], id);
        ecam_off = true;
    }
}

static struct pci_device devices[256]; /* desktop chipsets expose well over 64 functions */
static int ndevices;

static const char *class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01: return s == 0x01 ? "IDE controller" : s == 0x06 ? "SATA controller" : "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia device";
    case 0x06: return s == 0x00 ? "host bridge" : s == 0x01 ? "ISA bridge" : s == 0x80 ? "bridge" : "PCI bridge";
    case 0x0C: return s == 0x03 ? "USB controller" : "serial bus controller";
    default: return "device";
    }
}

static uint64_t bar_probe(const struct pci_device *d, unsigned bar);

/* A bridge's header has two BARs and a CardBus bridge's one; the dwords after them are bus
   numbers and windows, which must never be probed or written back as BARs. */
unsigned pci_bar_count(const struct pci_device *d) {
    unsigned type = (pci_read32(d->bus, d->dev, d->func, 0x0C) >> 16) & 0x7F;
    return type == 0 ? 6 : type == 1 ? 2 : type == 2 ? 1 : 0;
}

void pci_init(void) {
    ecam = acpi_ecams(&necam);
    for (unsigned i = 0; i < necam; i++)
        if (ecam[i].segment == 0)
            kprintf("pci: ECAM segment 0 buses %u-%u at %lx\n", ecam[i].start_bus, ecam[i].end_bus, ecam[i].base);
    if (!necam) kprintf("pci: no MCFG, config space via CF8/CFC only (no extended capabilities)\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint32_t cls = pci_read32(bus, dev, func, 8);
                ecam_activate(bus, dev, func, id);
                if (ndevices < (int)ARRAY_SIZE(devices)) {
                    struct pci_device *d = &devices[ndevices++];
                    d->bus = bus;
                    d->dev = dev;
                    d->func = func;
                    d->vendor = id & 0xFFFF;
                    d->device = id >> 16;
                    d->cls = cls >> 24;
                    d->subcls = (cls >> 16) & 0xFF;
                    d->progif = (cls >> 8) & 0xFF;
                    d->irq = pci_read32(bus, dev, func, 0x3C) & 0xFF;
                    unsigned bars = pci_bar_count(d);
                    for (unsigned b = 0; b < bars; b++) d->bar[b] = pci_read32(bus, dev, func, 0x10 + b * 4);
                    for (unsigned b = 0; b < bars; b++) {
                        d->bar_size[b] = bar_probe(d, b);
                        if (!(d->bar[b] & 1) && (d->bar[b] & 6) == 4) b++; /* the upper half is no BAR */
                    }
                    d->pcie_cap = pci_find_cap(d, 0x10);
                    if (d->pcie_cap) d->pcie_type = (pci_read16(bus, dev, func, d->pcie_cap + 2) >> 4) & 0xF;
                    kprintf("pci: %02x:%02x.%x %04x:%04x %s\n", bus, dev, func, d->vendor, d->device,
                            class_name(d->cls, d->subcls));
                }
                if (func == 0 && !(pci_read32(bus, dev, 0, 0x0C) & 0x00800000)) break;
            }
        }
    }
}

struct pci_device *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndevices; i++)
        if (devices[i].vendor == vendor && devices[i].device == device) return &devices[i];
    return NULL;
}

struct pci_device *pci_find_class(uint8_t cls, uint8_t subcls) {
    for (int i = 0; i < ndevices; i++)
        if (devices[i].cls == cls && devices[i].subcls == subcls) return &devices[i];
    return NULL;
}

void pci_enable_busmaster(struct pci_device *d) {
    d->kernel_owned = true;
    uint16_t cmd = pci_read16(d->bus, d->dev, d->func, 4);
    cmd |= 0x7; /* I/O, memory, bus master */
    pci_write16(d->bus, d->dev, d->func, 4, cmd);
}

int pci_list(struct n_pciinfo *out, int max) {
    int n = 0;
    for (int i = 0; i < ndevices && n < max; i++, n++) {
        out[n].bus = devices[i].bus;
        out[n].dev = devices[i].dev;
        out[n].func = devices[i].func;
        out[n].cls = devices[i].cls;
        out[n].subcls = devices[i].subcls;
        out[n].progif = devices[i].progif;
        out[n].vendor = devices[i].vendor;
        out[n].device = devices[i].device;
    }
    return n;
}

int pci_count(void) { return ndevices; }

struct pci_device *pci_at(int index) { return index >= 0 && index < ndevices ? &devices[index] : NULL; }

struct pci_device *pci_find_function(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < ndevices; i++)
        if (devices[i].bus == bus && devices[i].dev == dev && devices[i].func == func) return &devices[i];
    return NULL;
}

static bool is_bridge(const struct pci_device *d) {
    return ((pci_read32(d->bus, d->dev, d->func, 0x0C) >> 16) & 0x7F) == 1;
}

struct pci_device *pci_upstream(const struct pci_device *d) {
    for (int i = 0; i < ndevices; i++) {
        struct pci_device *b = &devices[i];
        if (b != d && is_bridge(b) && ((pci_read32(b->bus, b->dev, b->func, 0x18) >> 8) & 0xFF) == d->bus)
            return b;
    }
    return NULL;
}

uint64_t pci_bar_address(const struct pci_device *d, unsigned bar) {
    if (bar >= 6 || (d->bar[bar] & 1)) return 0;
    uint64_t base = d->bar[bar] & ~15u;
    if ((d->bar[bar] & 6) == 4) {
        if (bar == 5) return 0;
        base |= (uint64_t)d->bar[bar + 1] << 32;
    }
    return base;
}

/* Size a memory BAR by the standard all-ones probe, with memory decoding off meanwhile. Only
   during enumeration: while decoding is off the function answers nothing, so the same probe on a
   running device (a GPU the boot screen is drawn through, a driver's registers) would lose the
   writes and read all-ones. */
static uint64_t bar_probe(const struct pci_device *d, unsigned bar) {
    if (!pci_bar_address(d, bar)) return 0;
    bool wide = (d->bar[bar] & 6) == 4;
    uint8_t off = 0x10 + bar * 4;
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4) & 0xFFFF; /* not the write-1-to-clear status half */
    uint64_t f = irq_save();
    /* The boot console may have drawn through this BAR: its write-combined pixels leave the CPU
       before memory decoding goes off, or the function would drop them. */
    __asm__ volatile("sfence" ::: "memory");
    pci_write32(d->bus, d->dev, d->func, 4, cmd & ~2u);
    pci_write32(d->bus, d->dev, d->func, off, 0xFFFFFFFFu);
    uint64_t mask = pci_read32(d->bus, d->dev, d->func, off) & ~15u;
    pci_write32(d->bus, d->dev, d->func, off, d->bar[bar]);
    if (wide) {
        pci_write32(d->bus, d->dev, d->func, off + 4, 0xFFFFFFFFu);
        mask |= (uint64_t)pci_read32(d->bus, d->dev, d->func, off + 4) << 32;
        pci_write32(d->bus, d->dev, d->func, off + 4, d->bar[bar + 1]);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }
    pci_write32(d->bus, d->dev, d->func, 4, cmd);
    irq_restore(f);
    return mask ? ~mask + 1 : 0;
}

uint64_t pci_bar_size(const struct pci_device *d, unsigned bar) { return bar < 6 ? d->bar_size[bar] : 0; }

uint8_t pci_find_cap(const struct pci_device *d, uint8_t id) {
    if (!(pci_read16(d->bus, d->dev, d->func, 6) & 0x10)) return 0;
    uint8_t cap = pci_read32(d->bus, d->dev, d->func, 0x34) & 0xFC;
    for (int guard = 0; cap >= 0x40 && guard < 48; guard++) {
        uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
        if ((h & 0xFF) == id) return cap;
        cap = (h >> 8) & 0xFC;
    }
    return 0;
}

uint16_t pci_find_ext_cap(const struct pci_device *d, uint16_t id) {
    if (!d->pcie_cap || !ecam_regs(d->bus, d->dev, d->func, true)) return 0;
    uint16_t off = 0x100;
    for (int guard = 0; off >= 0x100 && off <= 0xFFC && guard < 96; guard++) {
        uint32_t h = pci_read32(d->bus, d->dev, d->func, off);
        if (!h || h == 0xFFFFFFFFu) return 0;
        if ((h & 0xFFFF) == id) return off;
        off = (h >> 20) & 0xFFC;
    }
    return 0;
}

static void intx_disable(struct pci_device *d) {
    uint16_t cmd = pci_read16(d->bus, d->dev, d->func, 4);
    pci_write16(d->bus, d->dev, d->func, 4, cmd | 0x400);
}

unsigned pci_msi_max_vectors(const struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x05);
    if (!cap) return 0;
    return 1u << ((pci_read32(d->bus, d->dev, d->func, cap) >> 17) & 7);
}

bool pci_msi_enable_multi(struct pci_device *d, uint8_t base_vector, unsigned count, uint32_t dest_apic_id) {
    if (!lapic_present()) return false;
    uint8_t cap = pci_find_cap(d, 0x05);
    if (!cap || !count || count > 32 || (count & (count - 1)) || (base_vector & (count - 1))) return false;
    uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
    uint16_t ctl = h >> 16;
    if (count > (1u << ((ctl >> 1) & 7))) return false;
    uint32_t address, message;
    if (!iommu_msi_message(d, 0, count, base_vector, dest_apic_id, &address, &message)) return false;
    bool wide = ctl & 0x80;
    pci_write32(d->bus, d->dev, d->func, cap + 4, address);
    if (wide) pci_write32(d->bus, d->dev, d->func, cap + 8, 0);
    uint8_t data_off = cap + (wide ? 0x0C : 0x08);
    uint32_t data = pci_read32(d->bus, d->dev, d->func, data_off);
    pci_write32(d->bus, d->dev, d->func, data_off, (data & 0xFFFF0000u) | message);
    if (ctl & 0x100) pci_write32(d->bus, d->dev, d->func, data_off + 4, 0); /* per-vector masking: all open */
    uint8_t msix = pci_find_cap(d, 0x11); /* never both: firmware may have left MSI-X on */
    if (msix) {
        uint32_t xh = pci_read32(d->bus, d->dev, d->func, msix);
        pci_write32(d->bus, d->dev, d->func, msix, xh & ~0x80000000u);
    }
    unsigned log2 = __builtin_ctz(count);
    ctl = (ctl & ~0x70u) | (uint16_t)(log2 << 4) | 1; /* `count` messages, enabled */
    pci_write32(d->bus, d->dev, d->func, cap, (h & 0xFFFF) | (uint32_t)ctl << 16);
    intx_disable(d);
    return true;
}

unsigned pci_msix_init(struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x11);
    if (!cap) return 0;
    uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
    uint32_t table = pci_read32(d->bus, d->dev, d->func, cap + 4);
    uint64_t base = pci_bar_address(d, table & 7);
    unsigned entries = ((h >> 16) & 0x7FF) + 1;
    if (!base) return 0;
    if (!d->msix_table) d->msix_table = vmm_map_mmio(base + (table & ~7u), entries * 16);
    d->msix_entries = entries;
    uint8_t msi = pci_find_cap(d, 0x05); /* never both */
    if (msi) {
        uint32_t mh = pci_read32(d->bus, d->dev, d->func, msi);
        pci_write32(d->bus, d->dev, d->func, msi, mh & ~0x00010000u);
    }
    /* enable with the whole function masked while the entries are written */
    pci_write32(d->bus, d->dev, d->func, cap, (h & 0x3FFFFFFF) | 0xC0000000u);
    for (unsigned i = 0; i < entries; i++) d->msix_table[i * 4 + 3] |= 1;
    intx_disable(d);
    return entries;
}

bool pci_msix_route(struct pci_device *d, unsigned entry, uint8_t vector, uint32_t dest_apic_id) {
    if (!lapic_present() || !d->msix_table || entry >= d->msix_entries) return false;
    volatile uint32_t *t = d->msix_table + entry * 4;
    uint32_t vc = t[3];
    t[3] = vc | 1; /* entry masked while its address/data change */
    uint32_t address, data;
    bool ok = iommu_msi_message(d, entry, 1, vector, dest_apic_id, &address, &data);
    if (ok) {
        t[0] = address;
        t[1] = 0;
        t[2] = data;
    }
    t[3] = vc;
    return ok;
}

void pci_msix_mask(struct pci_device *d, unsigned entry, bool masked) {
    if (!d->msix_table || entry >= d->msix_entries) return;
    volatile uint32_t *t = d->msix_table + entry * 4;
    t[3] = masked ? t[3] | 1 : t[3] & ~1u;
    (void)t[3]; /* read back: the unmask must reach the device before the caller proceeds */
}

void pci_msix_function_mask(struct pci_device *d, bool masked) {
    uint8_t cap = pci_find_cap(d, 0x11);
    if (!cap) return;
    uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
    pci_write32(d->bus, d->dev, d->func, cap, masked ? h | 0x40000000u : h & ~0x40000000u);
}

void pci_msi_disable(struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x05), msix = pci_find_cap(d, 0x11);
    if (cap) pci_write32(d->bus, d->dev, d->func, cap, pci_read32(d->bus, d->dev, d->func, cap) & ~0x00010000u);
    if (msix) pci_write32(d->bus, d->dev, d->func, msix,
                          pci_read32(d->bus, d->dev, d->func, msix) & ~0x80000000u);
    iommu_msi_clear(d);
}

bool pci_msi_enable(struct pci_device *d, uint8_t vector) {
    if (!lapic_present()) return false;
    uint32_t dest = lapic_current_id(); /* the BSP: device interrupts are only handled there */
    if (dest > 0xFF) return false; /* not addressable by an MSI message; leave the device untouched */
    if (pci_find_cap(d, 0x05)) return pci_msi_enable_multi(d, vector, 1, dest);
    if (!pci_msix_init(d)) return false;
    if (!pci_msix_route(d, 0, vector, dest)) return false;
    pci_msix_mask(d, 0, false);
    pci_msix_function_mask(d, false);
    return true;
}

/* ---- sleep ----
   After S3 a function comes back reset: no BARs, no command bits, no MSI. The header, the MSI and
   MSI-X state (the MSI-X table lives in a BAR) and the PCIe control registers are saved before
   and written back before any driver touches its device again. Device-specific registers past
   the capabilities are the driver's. */
static struct pci_saved {
    uint32_t id, hdr[16];
    uint8_t msi, msix, msi_dwords;
    uint32_t msi_regs[5];
    uint32_t msix_ctl, *msix_table;
    uint16_t pcie_ctl[4];
} saved[ARRAY_SIZE(devices)];
static const uint8_t pcie_ctl_off[4] = {0x08, 0x10, 0x28, 0x30}; /* device, link, device 2, link 2 */

void pci_suspend(void) {
    for (int i = 0; i < ndevices; i++) {
        struct pci_device *d = &devices[i];
        struct pci_saved *s = &saved[i];
        for (unsigned k = 0; k < 16; k++) s->hdr[k] = pci_read32(d->bus, d->dev, d->func, k * 4);
        s->id = s->hdr[0];
        s->msi = pci_find_cap(d, 0x05);
        if (s->msi) {
            uint16_t ctl = pci_read32(d->bus, d->dev, d->func, s->msi) >> 16;
            /* control, address (+ high half), data, and the mask bits when there are any */
            s->msi_dwords = 3 + !!(ctl & 0x80) + !!(ctl & 0x100);
            for (unsigned j = 0; j < s->msi_dwords; j++)
                s->msi_regs[j] = pci_read32(d->bus, d->dev, d->func, s->msi + 4 * j);
        }
        s->msix = pci_find_cap(d, 0x11);
        if (s->msix) {
            s->msix_ctl = pci_read32(d->bus, d->dev, d->func, s->msix);
            if (d->msix_table && !s->msix_table) s->msix_table = kmalloc(d->msix_entries * 16);
            if (d->msix_table && s->msix_table)
                for (unsigned j = 0; j < d->msix_entries * 4u; j++) s->msix_table[j] = d->msix_table[j];
        }
        if (d->pcie_cap)
            for (unsigned j = 0; j < 4; j++) s->pcie_ctl[j] = pci_read16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j]);
    }
}

void pci_resume(void) {
    for (int i = 0; i < ndevices; i++) {
        struct pci_device *d = &devices[i];
        struct pci_saved *s = &saved[i];
        if (pci_read32(d->bus, d->dev, d->func, 0) != s->id) {
            kprintf("pci: %02x:%02x.%x is not there after sleep\n", d->bus, d->dev, d->func);
            continue;
        }
        /* cache line and latency, not BIST (a write there starts a self test); then BARs and the
           rest of the header (bus numbers and windows for a bridge); command last */
        pci_write16(d->bus, d->dev, d->func, 0x0C, s->hdr[3] & 0xFFFF);
        for (unsigned k = 4; k < 16; k++) pci_write32(d->bus, d->dev, d->func, k * 4, s->hdr[k]);
        if (d->pcie_cap)
            for (unsigned j = 0; j < 4; j++) pci_write16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j], s->pcie_ctl[j]);
        pci_write16(d->bus, d->dev, d->func, 4, s->hdr[1] & 0xFFFF);
        if (s->msix) {
            /* the table is memory space, which the command register has just opened */
            pci_write32(d->bus, d->dev, d->func, s->msix, s->msix_ctl | 0xC0000000u);
            if (d->msix_table && s->msix_table)
                for (unsigned j = 0; j < d->msix_entries * 4u; j++) d->msix_table[j] = s->msix_table[j];
            pci_write32(d->bus, d->dev, d->func, s->msix, s->msix_ctl);
        }
        if (s->msi) {
            for (unsigned j = 1; j < s->msi_dwords; j++)
                pci_write32(d->bus, d->dev, d->func, s->msi + 4 * j, s->msi_regs[j]);
            pci_write32(d->bus, d->dev, d->func, s->msi, s->msi_regs[0]);
        }
    }
}
