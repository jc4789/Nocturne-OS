/* PCI configuration space access (mechanism #1 and PCIe ECAM) and bus enumeration. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "abi.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/sched.h"

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
            if (!map || !vmm_map_mmio(phys, 4096)) return NULL;
            ecam_active[bdf >> 3] |= 1u << (bdf & 7);
        }
        return phys_to_virt(phys);
    }
    return NULL;
}

/* Mechanism #1 is an address register and a data window: an interrupt handler's own access
   between the two would redirect this one, so the pair runs with interrupts off. */
static uint32_t cf8_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    uint64_t f = irq_save();
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)func << 8 | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    irq_restore(f);
    return v;
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, off >= 256);
    if (r) return mmio_read32(r + ((off & 0xFFC) >> 2));
    if (off >= 256) return 0xFFFFFFFFu;
    return cf8_read32(bus, dev, func, off);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2) * 8));
}

/* Narrow writes are narrow on the bus too: a read-modify-write of the whole dword would write
   back the register beside this one, and Status (beside Command) clears its error bits on a
   written 1. bytes: 1, 2 or 4, at an offset aligned to it. */
static void config_write(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v, unsigned bytes) {
    volatile uint8_t *r = (volatile uint8_t *)ecam_regs(bus, dev, func, off >= 256);
    if (r) {
        if (bytes == 4) mmio_write32(r + (off & 0xFFC), v);
        else if (bytes == 2) mmio_write16(r + (off & 0xFFE), (uint16_t)v);
        else mmio_write8(r + (off & 0xFFF), (uint8_t)v);
        return;
    }
    if (off >= 256) return;
    uint64_t f = irq_save();
    outl(0xCF8, 0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)func << 8 | (off & 0xFC));
    if (bytes == 4) outl(0xCFC, v);
    else if (bytes == 2) outw(0xCFC + (off & 2), (uint16_t)v);
    else outb(0xCFC + (off & 3), (uint8_t)v);
    irq_restore(f);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v) {
    config_write(bus, dev, func, off, v, 4);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t v) {
    config_write(bus, dev, func, off, v, 2);
}

void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint8_t v) {
    config_write(bus, dev, func, off, v, 1);
}

/* Switch a found function to ECAM, checking that ECAM really reaches it (a wrong MCFG base turns it off). */
static void ecam_activate(uint8_t bus, uint8_t dev, uint8_t func, uint32_t id) {
    volatile uint32_t *r = ecam_regs(bus, dev, func, true);
    uint32_t seen = r ? mmio_read32(r) : id;
    if (seen != id) {
        kprintf("pci: ECAM read of %02x:%02x.%x gave %08x, expected %08x; falling back to CF8/CFC\n", bus, dev,
                func, seen, id);
        ecam_off = true;
    }
}

/* Every function enumeration found. The array grows while pci_init runs only: drivers keep
   pointers into it from then on. */
static struct pci_device *devices;
static size_t ndevices, devices_room;
static bool devices_lost;

static struct pci_device *device_new(void) {
    struct pci_device *d = kappend(&devices, &ndevices, &devices_room, sizeof *d);
    if (!d) {
        if (!devices_lost) kprintf("pci: no memory to keep more functions; the rest are not used\n");
        devices_lost = true;
        return NULL;
    }
    memset(d, 0, sizeof *d);
    return d;
}

bool pci_all_known(void) { return !devices_lost; }

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

/* A function still initializing answers RRS (FFFF0001h) where its Root Port shows that
   (2.3.2), and counts as broken only once 1.0 s has passed since its reset (6.6.1). `since`
   is a TSC count no earlier than that reset; the wait spins, as callers may have interrupts off. */
static uint32_t ready_id(uint8_t bus, uint8_t dev, uint8_t func, uint64_t since) {
    uint32_t id;
    while ((id = pci_read32(bus, dev, func, 0)) == PCI_CONFIG_RRS_ID && rdtsc() - since < tsc_hz) pause();
    return id;
}

void pci_init(void) {
    uint64_t since = rdtsc(); /* the platform reset was before the firmware ran */
    ecam = acpi_ecams(&necam);
    for (unsigned i = 0; i < necam; i++)
        if (ecam[i].segment == 0)
            kprintf("pci: ECAM segment 0 buses %u-%u at %lx\n", ecam[i].start_bus, ecam[i].end_bus, ecam[i].base);
    if (!necam) kprintf("pci: no MCFG, config space via CF8/CFC only (no extended capabilities)\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = ready_id(bus, dev, func, since);
                if (id == PCI_CONFIG_RRS_ID)
                    kprintf("pci: %02x:%02x.%x still initializing 1 s: not used\n", bus, dev, func);
                if ((id & 0xFFFF) == 0xFFFF || id == PCI_CONFIG_RRS_ID) {
                    if (func == 0) break;
                    continue;
                }
                uint32_t cls = pci_read32(bus, dev, func, 8);
                ecam_activate(bus, dev, func, id);
                struct pci_device *d = device_new();
                if (d) {
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
                    d->bridge = ((pci_read32(bus, dev, func, 0x0C) >> 16) & 0x7F) == 1;
                    if (d->bridge) {
                        uint32_t buses = pci_read32(bus, dev, func, 0x18);
                        d->secondary = buses >> 8 & 0xFF;
                        d->subordinate = buses >> 16 & 0xFF;
                    }
                    kprintf("pci: %02x:%02x.%x %04x:%04x %s\n", bus, dev, func, d->vendor, d->device,
                            class_name(d->cls, d->subcls));
                }
                if (func == 0 && !(pci_read32(bus, dev, 0, 0x0C) & 0x00800000)) break;
            }
        }
    }
    /* Bus numbers stay as the firmware gave them (sleep writes them back), so the tree is found once. */
    for (size_t i = 0; i < ndevices; i++)
        for (size_t k = 0; k < ndevices && !devices[i].upstream; k++)
            if (k != i && devices[k].bridge && devices[k].secondary == devices[i].bus)
                devices[i].upstream = &devices[k];
}

struct pci_device *pci_find(uint16_t vendor, uint16_t device) {
    for (size_t i = 0; i < ndevices; i++)
        if (devices[i].vendor == vendor && devices[i].device == device) return &devices[i];
    return NULL;
}

struct pci_device *pci_find_class(uint8_t cls, uint8_t subcls) {
    for (size_t i = 0; i < ndevices; i++)
        if (devices[i].cls == cls && devices[i].subcls == subcls) return &devices[i];
    return NULL;
}

void pci_enable_busmaster(struct pci_device *d) {
    d->kernel_owned = true;
    uint16_t cmd = pci_read16(d->bus, d->dev, d->func, PCI_COMMAND);
    cmd |= 0x7; /* I/O, memory, bus master */
    pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, cmd);
}

int pci_list(struct n_pciinfo *out, int max) {
    int n = 0;
    for (size_t i = 0; i < ndevices && n < max; i++, n++) {
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

int pci_count(void) { return (int)ndevices; } /* at most 256 buses of 32 devices of 8 functions */

struct pci_device *pci_at(int index) { return index >= 0 && (size_t)index < ndevices ? &devices[index] : NULL; }

struct pci_device *pci_find_function(uint8_t bus, uint8_t dev, uint8_t func) {
    for (size_t i = 0; i < ndevices; i++)
        if (devices[i].bus == bus && devices[i].dev == dev && devices[i].func == func) return &devices[i];
    return NULL;
}

struct pci_device *pci_upstream(const struct pci_device *d) { return d->upstream; }

bool pci_below(const struct pci_device *b, const struct pci_device *o) {
    return b->bridge && o != b && o->bus >= b->secondary && o->bus <= b->subordinate;
}

bool pci_same_device(const struct pci_device *a, const struct pci_device *b) {
    if (a->bus != b->bus) return false;
    const struct pci_device *up = a->upstream;
    return a->dev == b->dev || (up && up->pcie_cap && (up->pcie_type == 4 || up->pcie_type == 6));
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
    /* off while address and data change (firmware may have left it on): no message half old, half new */
    if (ctl & 1) pci_write16(d->bus, d->dev, d->func, cap + 2, ctl & ~1u);
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
    pci_write16(d->bus, d->dev, d->func, cap + 2, ctl);
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
    if (!d->msix_table && !(d->msix_table = vmm_map_mmio(base + (table & ~7u), entries * 16))) return 0;
    d->msix_entries = entries;
    uint8_t msi = pci_find_cap(d, 0x05); /* never both */
    if (msi) {
        uint32_t mh = pci_read32(d->bus, d->dev, d->func, msi);
        pci_write32(d->bus, d->dev, d->func, msi, mh & ~0x00010000u);
    }
    /* enable with the whole function masked while the entries are written */
    pci_write32(d->bus, d->dev, d->func, cap, (h & 0x3FFFFFFF) | 0xC0000000u);
    for (unsigned i = 0; i < entries; i++) {
        volatile uint32_t *vc = d->msix_table + i * 4 + 3;
        mmio_write32(vc, mmio_read32(vc) | 1);
    }
    intx_disable(d);
    return entries;
}

bool pci_msix_route(struct pci_device *d, unsigned entry, uint8_t vector, uint32_t dest_apic_id) {
    if (!lapic_present() || !d->msix_table || entry >= d->msix_entries) return false;
    volatile uint32_t *t = d->msix_table + entry * 4;
    uint32_t vc = mmio_read32(t + 3);
    mmio_write32(t + 3, vc | 1); /* entry masked while its address/data change */
    uint32_t address, data;
    bool ok = iommu_msi_message(d, entry, 1, vector, dest_apic_id, &address, &data);
    if (ok) {
        mmio_write32(t, address);
        mmio_write32(t + 1, 0);
        mmio_write32(t + 2, data);
    }
    mmio_write32(t + 3, vc);
    return ok;
}

void pci_msix_mask(struct pci_device *d, unsigned entry, bool masked) {
    if (!d->msix_table || entry >= d->msix_entries) return;
    volatile uint32_t *t = d->msix_table + entry * 4;
    uint32_t vc = mmio_read32(t + 3);
    mmio_write32(t + 3, masked ? vc | 1 : vc & ~1u);
    (void)mmio_read32(t + 3); /* read back: the unmask must reach the device before the caller proceeds */
}

void pci_msix_function_mask(struct pci_device *d, bool masked) {
    uint8_t cap = pci_find_cap(d, 0x11);
    if (!cap) return;
    uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
    pci_write32(d->bus, d->dev, d->func, cap, masked ? h | 0x40000000u : h & ~0x40000000u);
}

bool pci_msi_disable(struct pci_device *d) {
    uint8_t cap = pci_find_cap(d, 0x05), msix = pci_find_cap(d, 0x11);
    if (cap) pci_write32(d->bus, d->dev, d->func, cap, pci_read32(d->bus, d->dev, d->func, cap) & ~0x00010000u);
    if (msix) pci_write32(d->bus, d->dev, d->func, msix,
                          pci_read32(d->bus, d->dev, d->func, msix) & ~0x80000000u);
    /* A stale Enable must not be saved and replayed by a later FLR, nor may its vector
       be recycled merely because interrupt-table invalidation completed. */
    bool off = (!cap || !(pci_read32(d->bus, d->dev, d->func, cap) & 0x00010000u)) &&
               (!msix || !(pci_read32(d->bus, d->dev, d->func, msix) & 0x80000000u));
    bool cleared = iommu_msi_clear(d); /* still revoke its remapping entry when a device refuses */
    return off && cleared;
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

/* ---- a function's configuration, kept across a reset ----
   After S3 or a Function Level Reset a function comes back reset: no BARs, no command bits, no
   MSI. The header, the MSI and MSI-X state (the MSI-X table lives in a BAR), the PCIe control
   registers, the Resizable BAR sizes (a BAR's size, which its address must match, 7.8.6) and ACS
   control (what keeps a claimed function's requests on their way to the IOMMU, dev/iommu.c) are
   saved before and written back before any driver touches its device again. Device-specific
   registers past the capabilities are the driver's. */
struct pci_saved {
    uint32_t id, hdr[16];
    uint8_t msi, msix, msi_dwords;
    uint32_t msi_regs[5];
    uint32_t msix_ctl, *msix_table;
    uint16_t pcie_ctl[5];
    uint16_t rebar;          /* the Resizable BAR capability's offset */
    uint8_t rebars;          /* how many of its BARs are resizable */
    uint32_t rebar_ctl[6];   /* their control registers, BAR Size in bits 13:8 */
    uint16_t acs, acs_ctl; /* the ACS capability's offset and its control register */
};
/* Root Control exists in Root Ports and Root Complex Event Collectors only (7.5.3.12) */
static const uint8_t pcie_ctl_off[5] = {PCIE_DEVCTL, PCIE_LNKCTL, PCIE_DEVCTL2, PCIE_LNKCTL2, PCIE_ROOTCTL};
static unsigned pcie_ctls(const struct pci_device *d) {
    if (!d->pcie_cap) return 0;
    return d->pcie_type == 4 || d->pcie_type == 10 ? 5 : 4;
}

/* table: the MSI-X table's entries too, into s->msix_table (allocated once, kept); false without
   memory for it. */
static bool state_save(const struct pci_device *d, struct pci_saved *s, bool table) {
    for (unsigned k = 0; k < 16; k++) s->hdr[k] = pci_read32(d->bus, d->dev, d->func, k * 4);
    s->id = s->hdr[0];
    s->msi = pci_find_cap(d, 0x05);
    if (s->msi) {
        uint16_t ctl = pci_read32(d->bus, d->dev, d->func, s->msi) >> 16;
        /* control, address (+ high half), data, and the mask bits when there are any */
        s->msi_dwords = 3 + !!(ctl & 0x80) + !!(ctl & 0x100);
        for (unsigned j = 0; j < s->msi_dwords; j++) s->msi_regs[j] = pci_read32(d->bus, d->dev, d->func, s->msi + 4 * j);
    }
    s->msix = pci_find_cap(d, 0x11);
    if (s->msix) {
        s->msix_ctl = pci_read32(d->bus, d->dev, d->func, s->msix);
        if (table && d->msix_table) {
            if (!s->msix_table && !(s->msix_table = kmalloc(d->msix_entries * 16))) return false;
            for (unsigned j = 0; j < d->msix_entries * 4u; j++) s->msix_table[j] = mmio_read32(d->msix_table + j);
        }
    }
    for (unsigned j = 0; j < pcie_ctls(d); j++)
        s->pcie_ctl[j] = pci_read16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j]);
    s->rebar = pci_find_ext_cap(d, 0x15);
    s->rebars = s->rebar ? (pci_read32(d->bus, d->dev, d->func, s->rebar + 8) >> 5) & 7 : 0;
    if (s->rebars > 6) s->rebars = 6; /* 1..6 (7.8.6.3) */
    for (unsigned j = 0; j < s->rebars; j++)
        s->rebar_ctl[j] = pci_read32(d->bus, d->dev, d->func, s->rebar + 8 + 8 * j);
    s->acs = pci_find_ext_cap(d, 0x0D);
    if (s->acs) s->acs_ctl = pci_read16(d->bus, d->dev, d->func, s->acs + 6);
    return true;
}

static void state_restore(const struct pci_device *d, const struct pci_saved *s) {
    /* cache line and latency, not BIST (a write there starts a self test); the BAR sizes, with
       memory decoding off first (wake firmware may have turned it on) since a size written
       leaves its BAR undefined (7.8.6); then BARs and the rest of the header (bus numbers and
       windows for a bridge); command last */
    pci_write16(d->bus, d->dev, d->func, 0x0C, s->hdr[3] & 0xFFFF);
    if (s->rebars) {
        uint16_t cmd = pci_read16(d->bus, d->dev, d->func, PCI_COMMAND);
        if (cmd & 2) pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, cmd & ~2u);
        for (unsigned j = 0; j < s->rebars; j++)
            pci_write32(d->bus, d->dev, d->func, s->rebar + 8 + 8 * j, s->rebar_ctl[j]);
    }
    for (unsigned k = 4; k < 16; k++) pci_write32(d->bus, d->dev, d->func, k * 4, s->hdr[k]);
    for (unsigned j = 0; j < pcie_ctls(d); j++)
        pci_write16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j], s->pcie_ctl[j]);
    if (s->acs) pci_write16(d->bus, d->dev, d->func, s->acs + 6, s->acs_ctl); /* before bus mastering */
    pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, s->hdr[1] & 0xFFFF);
    if (s->msix) {
        /* the table is memory space, which the command register has just opened */
        pci_write32(d->bus, d->dev, d->func, s->msix, s->msix_ctl | 0xC0000000u);
        if (d->msix_table && s->msix_table)
            for (unsigned j = 0; j < d->msix_entries * 4u; j++) mmio_write32(d->msix_table + j, s->msix_table[j]);
        pci_write32(d->bus, d->dev, d->func, s->msix, s->msix_ctl);
    }
    if (s->msi) {
        for (unsigned j = 1; j < s->msi_dwords; j++) pci_write32(d->bus, d->dev, d->func, s->msi + 4 * j, s->msi_regs[j]);
        pci_write32(d->bus, d->dev, d->func, s->msi, s->msi_regs[0]);
    }
}

/* ---- sleep ---- */
static struct pci_saved *saved;
static bool saved_valid;

const char *pci_suspend(void) {
    saved_valid = false;
    if (!saved) saved = kzalloc(ndevices * sizeof *saved);
    if (!saved) return "PCI (no memory to save it)";
    for (size_t i = 0; i < ndevices; i++)
        if (!state_save(&devices[i], &saved[i], true)) return "PCI (no memory to save it)";
    saved_valid = true;
    return NULL;
}

/* Every function has been through a Conventional Reset together, so one readiness deadline
   (ready_id) covers them all. Root Ports come first in enumeration order: theirs is the Root
   Control the functions below them are read through. */
void pci_resume(void) {
    if (!saved_valid) return; /* the sleep was called off before anything went down */
    uint64_t since = rdtsc();
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *d = &devices[i];
        uint32_t id = ready_id(d->bus, d->dev, d->func, since);
        if (id != saved[i].id) {
            kprintf("pci: %02x:%02x.%x is %s after sleep (ID %08x, was %08x)\n", d->bus, d->dev, d->func,
                    id == PCI_CONFIG_RRS_ID ? "still initializing 1 s" : "not there", id, saved[i].id);
            continue;
        }
        state_restore(d, &saved[i]);
    }
}

/* ---- a function's requests, and its reset (PCI Express Base 6.0) ---- */
static bool present(const struct pci_device *d) {
    /* RRS Software Visibility may return Vendor ID 0001h: that is not a ready function. */
    return pci_read32(d->bus, d->dev, d->func, 0) == ((uint32_t)d->device << 16 | d->vendor);
}

bool pci_disable_busmaster(struct pci_device *d, bool *was_mastering) {
    uint16_t command = pci_read16(d->bus, d->dev, d->func, PCI_COMMAND);
    if (was_mastering) *was_mastering = (command & PCI_COMMAND_MASTER) != 0;
    pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, command & ~PCI_COMMAND_MASTER);
    return present(d) && !(pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) & PCI_COMMAND_MASTER);
}

bool pci_requests_done(const struct pci_device *d, unsigned ms) {
    if (!d->pcie_cap) return present(d);
    for (uint64_t since = uptime_ms();; sleep_ms(1)) {
        if (!present(d)) return false;
        if (!(pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVSTA) & PCIE_DEVSTA_PENDING)) return true;
        if (uptime_ms() - since >= ms) return false;
    }
}

/* RRS Software Visibility (Root Control, 7.5.3.12): with it on, a Vendor ID read of a function
   still initializing completes with FFFF0001h (2.3.2) instead of the Root Complex retrying it
   inside the CPU's access, so a reset's readiness wait stays a scheduler wait its caller can
   cancel. The Root Port above a reset shows RRS while some reset below it awaits readiness, and
   gets the firmware's setting back after the last. A root that cannot (no Root Port above, as
   for a Root Complex Integrated Endpoint, or no Root Capabilities support) keeps its hardware
   retries. */
static struct pci_device *rrs_port(const struct pci_device *d) {
    for (struct pci_device *b = d->upstream; b; b = b->upstream)
        if (b->pcie_cap && b->pcie_type == 4)
            return present(b) &&
                   (pci_read16(b->bus, b->dev, b->func, b->pcie_cap + PCIE_ROOTCAP) & PCIE_ROOTCAP_RRS_VISIBLE)
                       ? b : NULL;
    return NULL;
}

static void rrs_show(const struct pci_device *port, bool on) {
    uint16_t ctl = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_ROOTCTL);
    uint16_t want = on ? ctl | PCIE_ROOTCTL_RRS_VISIBLE : ctl & ~PCIE_ROOTCTL_RRS_VISIBLE;
    if (want != ctl) pci_write16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_ROOTCTL, want);
}

static void rrs_hold(struct pci_device *port) {
    if (!port->rrs_holds++)
        port->rrs_firmware = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_ROOTCTL) &
                             PCIE_ROOTCTL_RRS_VISIBLE;
    rrs_show(port, true);
}

static void rrs_release(struct pci_device *port) {
    if (!--port->rrs_holds && !port->rrs_firmware) rrs_show(port, false);
}

/* FLR must finish in 100 ms (6.6.2), but device-specific initialization may still return RRS.
   When valid, Readiness Time Reporting (7.9.16) supplies a configuration-ready time: its 9-bit
   value is multiplied by 32^scale nanoseconds, rounded up to the scheduler's millisecond unit.
   No generic one-second readiness deadline exists for all PFs. */
#define FLR_DONE_MS  100
static unsigned flr_ready_ms(const struct pci_device *d) {
    uint16_t cap = pci_find_ext_cap(d, 0x22);
    if (!cap || !(pci_read32(d->bus, d->dev, d->func, cap + 4) & (1u << 31))) return FLR_DONE_MS;
    unsigned time = pci_read32(d->bus, d->dev, d->func, cap + 8) & 0xFFF;
    unsigned scale = time >> 9;
    if (time > 0xA1E || scale > 5) return FLR_DONE_MS; /* Table 7-280's maximum; scales 6/7 reserved */
    uint64_t ns = (uint64_t)(time & 0x1FF) << (5 * scale);
    unsigned ms = (unsigned)((ns + 1000000 - 1) / 1000000);
    return ms > FLR_DONE_MS ? ms : FLR_DONE_MS;
}

/* Owned by its enumerated device, not the app performing recovery. A failed readiness check or
   a cancelled claim must not lose the pre-FLR BARs/ACS or initiate a second FLR over empty state.
   PCI claim/recovery runs on the BSP; its caller reserves d->claimed across any sleep. */
struct pci_reset {
    struct pci_saved state;
    uint64_t since;
    unsigned wait;
    struct pci_device *rrs; /* the Root Port held showing RRS until readiness, or NULL */
};

/* Claim recovery and task_kill run on the BSP. Keep the cancellation check and the
   scheduler's wait registration atomic against kill_locked: otherwise a kill just
   before sleep_ms could lose its wake for the remaining FLR/readiness wait.
   schedule switches stacks: the next task resumes its own irq_restore (or startup/
   interrupt return), not this caller's IRQ guard. No IRQ-disabled busy wait or new
   polling/deadline slice is imposed while this task is blocked. */
static bool reset_wait(uint64_t ms) {
    uint64_t flags = irq_save();
    bool cancelled = current_task && current_task->killed;
    if (!cancelled) sleep_ms(ms);
    irq_restore(flags);
    return !cancelled;
}

static bool reset_cancelled(void) { return current_task && current_task->killed; }

bool pci_function_reset(struct pci_device *d) {
    if (reset_cancelled()) return false;
    struct pci_reset *r = d->reset;
    if (!r) {
        if (!d->pcie_cap || !present(d)) return false;
        if (!(pci_read32(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR)) return false;
        if (!pci_disable_busmaster(d, NULL)) return false;
        unsigned wait = flr_ready_ms(d);
        /* A former driver may have cleared CT Disable after issuing Requests: the
           current timeout value does not prove those Requests have timers (7.5.3.16).
           Observe their actual completion BEFORE FLR discards tags (6.6.2), with new
           Memory/I/O Requests prohibited by the confirmed BME clear. */
        while (!pci_requests_done(d, 0)) {
            if (!present(d)) return false;
            if (!reset_wait(1)) return false; /* scheduler resolution, not a deadline */
        }
        if (reset_cancelled()) return false;
        if (!(r = kzalloc(sizeof *r))) return false;
        state_save(d, &r->state, false);
        /* Restoring configuration must never re-enable bus mastering. */
        r->state.hdr[1] &= ~PCI_COMMAND_MASTER;
        r->state.pcie_ctl[0] &= ~PCIE_DEVCTL_FLR;
        r->wait = wait;
        if ((r->rrs = rrs_port(d))) rrs_hold(r->rrs);
        d->reset = r; /* publish before FLR or any cancellable wait */
        pci_write16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCTL,
                    r->state.pcie_ctl[0] | PCIE_DEVCTL_FLR);
        r->since = uptime_ms(); /* allow the full delay after the initiating configuration write */
    }
    for (;;) {
        uint64_t elapsed = uptime_ms() - r->since;
        if (reset_cancelled()) return false;
        if (elapsed >= r->wait) break;
        if (!reset_wait(r->wait - elapsed)) return false;
    }
    /* RRS Software Visibility returns FFFF0001h for this dword (2.3.2). Additional
       initialization after the 100ms FLR is permitted (6.6.2), even without RTR.
       Preserve the same saved state, yield, and let the caller cancel instead of
       failing a usable device at its first RRS or guessing a universal deadline. */
    uint32_t id;
    for (;;) {
        if (r->rrs) rrs_show(r->rrs, true); /* sleep or firmware may have rewritten Root Control */
        if ((id = pci_read32(d->bus, d->dev, d->func, 0)) != PCI_CONFIG_RRS_ID) break;
        if (!reset_wait(1)) return false;
    }
    if (id != r->state.id ||
        (pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVSTA) & PCIE_DEVSTA_PENDING)) {
        kprintf("pci: %02x:%02x.%u: FLR is not configuration-ready; saved state is retained for recovery\n",
                d->bus, d->dev, d->func);
        return false;
    }
    if (r->rrs) rrs_release(r->rrs); /* ready: no later read of it can see RRS until another reset */
    r->rrs = NULL;
    state_restore(d, &r->state);
    if (!present(d) || (pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) & PCI_COMMAND_MASTER)) return false;
    d->reset = NULL;
    kfree(r);
    return true;
}
