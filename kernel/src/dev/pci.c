/* PCI configuration space access (mechanism #1 and PCIe ECAM) and bus enumeration. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/acpi.h"
#include "abi.h"
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

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t v) {
    uint32_t shift = (off & 2) * 8, w = pci_read32(bus, dev, func, off);
    pci_write32(bus, dev, func, off, (w & ~(0xFFFFu << shift)) | (uint32_t)v << shift);
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
                    for (int b = 0; b < 6; b++) d->bar[b] = pci_read32(bus, dev, func, 0x10 + b * 4);
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
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4);
    cmd |= 0x7; /* I/O, memory, bus master */
    pci_write32(d->bus, d->dev, d->func, 4, cmd);
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

uint64_t pci_bar_address(const struct pci_device *d, unsigned bar) {
    if (bar >= 6 || (d->bar[bar] & 1)) return 0;
    uint64_t base = d->bar[bar] & ~15u;
    if ((d->bar[bar] & 6) == 4) {
        if (bar == 5) return 0;
        base |= (uint64_t)d->bar[bar + 1] << 32;
    }
    return base;
}

/* Size a memory BAR by the standard all-ones probe, with memory decoding off meanwhile. */
uint64_t pci_bar_size(const struct pci_device *d, unsigned bar) {
    if (!pci_bar_address(d, bar)) return 0;
    bool wide = (d->bar[bar] & 6) == 4;
    uint8_t off = 0x10 + bar * 4;
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4) & 0xFFFF; /* not the write-1-to-clear status half */
    uint64_t f = irq_save();
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

/* The MSI address carries the destination APIC id in bits 19:12 (physical mode, no redirection hint);
   without interrupt remapping that is all 8 bits there are. */
static uint32_t msi_address(uint32_t dest_apic_id) { return 0xFEE00000u | dest_apic_id << 12; }

static void intx_disable(struct pci_device *d) {
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4);
    pci_write32(d->bus, d->dev, d->func, 4, cmd | 0x400);
}

unsigned pci_msi_max_vectors(const struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x05);
    if (!cap) return 0;
    return 1u << ((pci_read32(d->bus, d->dev, d->func, cap) >> 17) & 7);
}

bool pci_msi_enable_multi(struct pci_device *d, uint8_t base_vector, unsigned count, uint32_t dest_apic_id) {
    if (!lapic_present() || dest_apic_id > 0xFF) return false;
    uint8_t cap = pci_find_cap(d, 0x05);
    if (!cap || !count || count > 32 || (count & (count - 1)) || (base_vector & (count - 1))) return false;
    uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
    uint16_t ctl = h >> 16;
    if (count > (1u << ((ctl >> 1) & 7))) return false;
    bool wide = ctl & 0x80;
    pci_write32(d->bus, d->dev, d->func, cap + 4, msi_address(dest_apic_id));
    if (wide) pci_write32(d->bus, d->dev, d->func, cap + 8, 0);
    uint8_t data_off = cap + (wide ? 0x0C : 0x08);
    uint32_t data = pci_read32(d->bus, d->dev, d->func, data_off);
    pci_write32(d->bus, d->dev, d->func, data_off, (data & 0xFFFF0000u) | base_vector);
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
    if (!lapic_present() || dest_apic_id > 0xFF || !d->msix_table || entry >= d->msix_entries) return false;
    volatile uint32_t *t = d->msix_table + entry * 4;
    uint32_t vc = t[3];
    t[3] = vc | 1; /* entry masked while its address/data change */
    t[0] = msi_address(dest_apic_id);
    t[1] = 0;
    t[2] = vector;
    t[3] = vc;
    return true;
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
