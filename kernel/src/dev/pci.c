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

/* A Vendor ID read that has not given a function's ID yet. A function still initializing answers
   RRS, which reads FFFF0001h where its Root Port shows that; where it does not, the Root Complex
   may stop re-issuing the Request and complete it as a failed transaction (2.3.2), so from a
   function known to be there (`there`) a failed read (Vendor ID FFFFh) is one too. */
static bool id_pending(uint32_t id, bool there) {
    return id == PCI_CONFIG_RRS_ID || (there && (id & 0xFFFF) == 0xFFFF);
}

/* A step of a wait: a scheduler tick, or a spin where interrupts are off (boot, sleep). */
static void wait_step(bool sleeps) {
    if (sleeps) sleep_ms(1);
    else pause();
}

static void rrs_show(const struct pci_device *port, bool on);

/* A function still initializing counts as broken only once 1.0 s has passed since its
   Conventional Reset without a Successful Completion (6.6.1). `since` is a TSC count no earlier
   than the end of that reset. At boot a failed read is an empty slot, as the platform guarantees
   the functions it shows are ready by then (6.6.1). rrs: the Root Port above, shown RRS again
   before each read (AML may rewrite Root Control while this sleeps), or NULL. */
static uint32_t ready_id(const struct pci_device *rrs, uint8_t bus, uint8_t dev, uint8_t func, uint64_t since,
                         bool there, bool sleeps) {
    for (;;) {
        if (rrs) rrs_show(rrs, true);
        uint32_t id = pci_read32(bus, dev, func, 0);
        if (!id_pending(id, there) || rdtsc() - since >= tsc_hz) return id;
        wait_step(sleeps);
    }
}

/* d's PCI Express capability: where it is, its Device/Port Type and whether it is version 2 or
   later, which 6.0 defines alone and lets software count on: an earlier one's structure ends at
   Root Status (7.5.3.2). */
static void pcie_identify(struct pci_device *d) {
    d->pcie_cap = pci_find_cap(d, 0x10);
    if (!d->pcie_cap) return;
    uint16_t caps = pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_CAPS);
    d->pcie_type = (caps >> 4) & 0xF;
    d->pcie_v2 = (caps & 0xF) >= 2;
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
                uint32_t id = ready_id(NULL, bus, dev, func, since, false, false);
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
                    pcie_identify(d);
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
    cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
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
    uint16_t cmd = pci_read16(d->bus, d->dev, d->func, PCI_COMMAND);
    pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, cmd | PCI_COMMAND_INTX_DISABLE);
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
    d->msi_given = (struct pci_msi_given){dest_apic_id, base_vector, (uint8_t)count};
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
    if (!d->msix_given && !(d->msix_given = kzalloc(entries * sizeof *d->msix_given))) return 0;
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
        d->msix_given[entry] = (struct pci_msi_given){dest_apic_id, vector, 1};
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
/* Root Control exists in Root Ports and Root Complex Event Collectors only (7.5.3.12), the
   registers past Root Status in a version 2 structure only */
static const uint8_t pcie_ctl_off[5] = {PCIE_DEVCTL, PCIE_LNKCTL, PCIE_ROOTCTL, PCIE_DEVCTL2, PCIE_LNKCTL2};
static bool pcie_ctl_has(const struct pci_device *d, unsigned j) {
    if (!d->pcie_cap) return false;
    if (pcie_ctl_off[j] == PCIE_ROOTCTL) return d->pcie_type == 4 || d->pcie_type == 10;
    return pcie_ctl_off[j] < PCIE_ROOTCTL || d->pcie_v2;
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
    for (unsigned j = 0; j < sizeof pcie_ctl_off; j++)
        if (pcie_ctl_has(d, j)) s->pcie_ctl[j] = pci_read16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j]);
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
        if (cmd & PCI_COMMAND_MEMORY)
            pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, cmd & ~PCI_COMMAND_MEMORY);
        for (unsigned j = 0; j < s->rebars; j++)
            pci_write32(d->bus, d->dev, d->func, s->rebar + 8 + 8 * j, s->rebar_ctl[j]);
    }
    for (unsigned k = 4; k < 16; k++) pci_write32(d->bus, d->dev, d->func, k * 4, s->hdr[k]);
    /* A Common Clock Configuration a reset cleared (an FLR does not, 6.6.2) and this sets again
       takes effect when the link retrains (7.5.3.7): the Downstream Port above retrains it once
       the rest of the device is back too (links_retrain). */
    struct pci_device *up = d->upstream;
    if (d->pcie_cap && up && up->pcie_cap && (up->pcie_type == 4 || up->pcie_type == 6) &&
        ((pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_LNKCTL) ^ s->pcie_ctl[1]) & PCIE_LNKCTL_CCC))
        up->retrain = true;
    for (unsigned j = 0; j < sizeof pcie_ctl_off; j++)
        if (pcie_ctl_has(d, j)) pci_write16(d->bus, d->dev, d->func, d->pcie_cap + pcie_ctl_off[j], s->pcie_ctl[j]);
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

static bool transactions_pending(const struct pci_device *d) {
    return pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVSTA) & PCIE_DEVSTA_PENDING;
}

bool pci_requests_done(const struct pci_device *d) {
    return present(d) && (!d->pcie_cap || !transactions_pending(d));
}

/* RRS Software Visibility (Root Control, 7.5.3.12): with it on, a Vendor ID read of a function
   still initializing completes with FFFF0001h (2.3.2) instead of the Root Complex retrying it
   inside the CPU's access, so a reset's readiness wait stays a scheduler wait its caller can
   cancel. The Root Port above shows RRS while an attempt to reset a function below it, a bus
   reset below it or a readiness wait after sleep runs, and gets the firmware's setting back after
   the last. A root that cannot (no Root Port above, as for a Root Complex Integrated Endpoint, or
   no Root Capabilities support) keeps its hardware retries. */
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

/* How long after a function stopped issuing Requests a Completion for one it still had
   outstanding may arrive, in ms. 6.6.2 recommends "as much time as provided by the pre-FLR value
   for Completion Timeout": the top of the range its Completion Timeout Value selects (7.5.3.16;
   0 marks a reserved encoding, which selects none: the longest defined is taken). A Request
   pending when that value was changed may keep the old one, so the longest range the function
   supports (7.5.3.15: bit 0 Range A .. bit 3 Range D) bounds it as well. With Completion
   Timeout Disable set, or cleared while Requests were pending (which need not then get a timer),
   6.6.2 makes the time system dependent but no less than 100 ms. A driver can do either through
   the function's own registers, unseen here, and no completer's latency is known: the floor is
   1 s, what 6.6.1 gives a device before software may decide it is broken. A structure before
   version 2 has neither register: its function has the default value. */
#define COMPLETION_FLOOR_MS 1000
/* The top of the range each Completion Timeout Value selects (7.5.3.16), 0 for reserved ones. */
static const unsigned completion_top_ms[16] = {[0x0] = 50, [0x1] = 1, [0x2] = 10, [0x5] = 55, [0x6] = 210,
                                               [0x9] = 900, [0xA] = 3500, [0xD] = 13000, [0xE] = 64000};
static unsigned completion_window_ms(const struct pci_device *d) {
    static const uint8_t range_longest[4] = {0x2, 0x6, 0xA, 0xE};
    unsigned value = 0, ranges = 0;
    if (d->pcie_v2) {
        value = pci_read16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCTL2) & 0xF;
        ranges = pci_read32(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCAP2) & 0xF;
    }
    unsigned ms = completion_top_ms[value] ? completion_top_ms[value] : completion_top_ms[0xE];
    for (unsigned r = 0; r < 4; r++)
        if ((ranges & (1u << r)) && completion_top_ms[range_longest[r]] > ms) ms = completion_top_ms[range_longest[r]];
    return ms > COMPLETION_FLOOR_MS ? ms : COMPLETION_FLOOR_MS;
}

/* A CPU read of a function's memory BAR that gets no Completion ends when the Root Port above it
   times the Request out (Completion Timeout, 7.5.3.15/16; the read then returns all ones). Until
   then the CPU that issued it is stalled inside one instruction and takes no interrupt, so the
   BSP's TLB shootdowns (mm/vmm.c) wait for it. An app driver reads its device from the APs while
   the device may not answer for a while (a GPU whose firmware boots, resets an engine or retrains
   its memory once bus mastering lets it run), so the port's timer must run and end within
   PCI_CPU_READ_MAX_MS: Completion Timeout Disable (firmware may leave it set) is cleared, and a
   value in Range D (4 s to 64 s) is lowered to the longest of Range C, or to the default range
   (50 us to 50 ms) where the port has no Range C. A port without Device Control 2 (a structure
   before version 2) has the default range; without a Root Port (an integrated endpoint) the root
   complex's own timer applies, which no register here sets. False only for a Root Port that keeps
   a longer or disabled timer. */
bool pci_cpu_reads_bounded(const struct pci_device *d) {
    struct pci_device *port = d->upstream;
    while (port && !(port->pcie_cap && port->pcie_type == 4)) port = port->upstream;
    if (!port || !port->pcie_v2) return true;
    uint16_t at = port->pcie_cap + PCIE_DEVCTL2;
    uint16_t ctl = pci_read16(port->bus, port->dev, port->func, at);
    uint32_t ranges = pci_read32(port->bus, port->dev, port->func, port->pcie_cap + PCIE_DEVCAP2) & 0xF;
    uint16_t want = ctl & ~PCIE_DEVCTL2_CT_DISABLE;
    if (completion_top_ms[want & 0xF] > PCI_CPU_READ_MAX_MS || !completion_top_ms[want & 0xF])
        want = (want & ~0xFu) | (ranges & 4 ? 0xA : 0x0);
    if (want != ctl) {
        pci_write16(port->bus, port->dev, port->func, at, want);
        kprintf("pci: %02x:%02x.%u: Completion Timeout of Root Port %02x:%02x.%u set from %s%x to %x\n", d->bus,
                d->dev, d->func, port->bus, port->dev, port->func, ctl & PCIE_DEVCTL2_CT_DISABLE ? "disabled, " : "",
                ctl & 0xF, want & 0xF);
    }
    uint16_t now = pci_read16(port->bus, port->dev, port->func, at);
    return !(now & PCIE_DEVCTL2_CT_DISABLE) && completion_top_ms[now & 0xF] &&
           completion_top_ms[now & 0xF] <= PCI_CPU_READ_MAX_MS;
}

/* FLR must finish in 100 ms (6.6.2); device-specific initialization may then still return RRS.
   A valid Readiness Time Reporting FLR Time (7.9.16) is when the function must be
   Configuration-Ready: its 9-bit value times 32^scale ns (at most A1Eh, 1.007 s, which also
   rules out the reserved scales 6 and 7), rounded up to the scheduler's millisecond; 0 without
   one. 6.6.2 itself sets no such limit. 6.6.1 gives a device 1.0 s after a Conventional Reset,
   and 5.10.1 a VF 1.0 s after an FLR, before software may decide it is broken: that is the
   limit without one. */
#define FLR_DONE_MS 100
#define READY_MS    1000
static unsigned flr_time_ms(const struct pci_device *d) {
    uint16_t cap = pci_find_ext_cap(d, 0x22);
    if (!cap || !(pci_read32(d->bus, d->dev, d->func, cap + 4) & (1u << 31))) return 0;
    unsigned time = pci_read32(d->bus, d->dev, d->func, cap + 8) & 0xFFF;
    if (time > 0xA1E) return 0;
    uint64_t ns = (uint64_t)(time & 0x1FF) << (5 * (time >> 9));
    unsigned ms = (unsigned)((ns + 1000000 - 1) / 1000000);
    return ms > FLR_DONE_MS ? ms : FLR_DONE_MS;
}

/* Owned by its enumerated device, not the app performing recovery. A cancelled or failed attempt
   must not lose the pre-FLR BARs/ACS, and a later one must not save over them what an FLR left.
   PCI claim/recovery runs on the BSP; its caller reserves d->claimed across any sleep, so no
   sleep comes inside an attempt. A bus reset holds the configuration of each function below
   its port the same way (pci_bus_reset), until the function answers after it. */
enum reset_phase {
    RESET_IDLE,     /* between attempts: the next one quiesces the function and issues an FLR */
    RESET_QUIESCED, /* this attempt stopped the function issuing Requests, at `quiesced` */
    RESET_ISSUED,   /* an FLR was initiated at `since` and the function not yet seen ready */
    RESET_BUS,      /* a bus reset above it is under way */
};
struct pci_reset {
    struct pci_saved state;  /* from before the first quiesce or the bus reset, Bus Master Enable cleared */
    enum reset_phase phase;
    uint64_t quiesced;
    unsigned window;         /* how long after `quiesced` a Completion may still arrive (ms) */
    uint64_t since;
    unsigned wait, ready;    /* ms after `since`: FLR complete (first access), readiness limit */
    bool owed;               /* an FLR is to come: pci_function_reset's, not only a bus reset's */
    bool stuck;              /* it did not come back (pci_reset_stuck) */
    bool broken;             /* nor after its last Conventional Reset (pci_bus_reset_port) */
    bool initializing;       /* its last Vendor ID read completed with RRS */
    bool refused;            /* another access was kept from the function (pci_config_blocked), logged */
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

/* The links whose device got a Common Clock Configuration back (state_restore) retrained by their
   Downstream Port, as 7.5.3.7 has software do once the bit changed. A retrain asked while the
   link trains need not use the new value (7.5.3.7), so its training ends first (Link Training
   clear, 7.5.3.8), within the 1.0 s 6.6.1 lets a device take after a reset; a link still
   training then is left as it is. */
static void links_retrain(bool sleeps) {
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *p = &devices[i];
        if (!p->retrain) continue;
        p->retrain = false;
        uint16_t status;
        for (uint64_t since = rdtsc(); ((status = pci_read16(p->bus, p->dev, p->func, p->pcie_cap + PCIE_LNKSTA)) &
                                        PCIE_LNKSTA_TRAINING) && rdtsc() - since < tsc_hz;)
            wait_step(sleeps);
        if (status & PCIE_LNKSTA_TRAINING) {
            kprintf("pci: %02x:%02x.%u: its link is still training; not retrained for the clock configuration below it\n",
                    p->bus, p->dev, p->func);
            continue;
        }
        uint16_t control = pci_read16(p->bus, p->dev, p->func, p->pcie_cap + PCIE_LNKCTL);
        pci_write16(p->bus, p->dev, p->func, p->pcie_cap + PCIE_LNKCTL, control | PCIE_LNKCTL_RETRAIN);
    }
}

/* 6.6.2 clears the whole Command register so the function issues no new Request. Its Interrupt
   Disable has the inverted sense (7.5.1.1.3): set, not cleared, it deasserts INTx and sends no
   Assert_INTx. No app reaches the registers any more (the caller's claim reservation). False,
   with nothing noted, when the function is gone or still masters. */
static bool reset_quiesce(struct pci_device *d, struct pci_reset *r) {
    pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);
    if (!present(d) || (pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) & PCI_COMMAND_MASTER)) return false;
    r->phase = RESET_QUIESCED;
    r->quiesced = uptime_ms();
    r->window = completion_window_ms(d); /* the pre-FLR value: nothing changes it from here */
    return true;
}

/* One attempt, the Root Port above showing RRS (port, or NULL without one). */
static bool reset_attempt(struct pci_device *d, struct pci_device *port) {
    struct pci_reset *r = d->reset;
    if (!r) {
        if (!d->pcie_cap || !present(d)) return false;
        if (!(pci_read32(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR)) return false;
        if (!(r = kzalloc(sizeof *r))) return false;
        state_save(d, &r->state, false);
        /* Restoring configuration must never re-enable bus mastering. */
        r->state.hdr[1] &= ~PCI_COMMAND_MASTER;
        r->state.pcie_ctl[0] &= ~PCIE_DEVCTL_FLR;
        r->owed = true;
        if (!reset_quiesce(d, r)) {
            /* Not reset: Command back as saved, Bus Master Enable still clear (hdr[1] is the dword
               at 04h, Command its low half), so a later attempt saves its decoding again. */
            pci_write16(d->bus, d->dev, d->func, PCI_COMMAND, r->state.hdr[1] & 0xFFFF);
            kfree(r);
            return false;
        }
        d->reset = r; /* publish before any cancellable wait */
    } else if (r->phase == RESET_IDLE) {
        /* An earlier FLR may have left it initializing, and a Configuration Write to a function
           that completes with RRS is retried by the Root Complex (2.3.2): no write goes to it,
           another FLR either, before its Vendor ID read gives its ID. */
        uint32_t id = pci_read32(d->bus, d->dev, d->func, 0);
        r->initializing = id == PCI_CONFIG_RRS_ID;
        if (id != r->state.id) {
            r->stuck = true;
            kprintf("pci: %02x:%02x.%u: %s; it is reset again once it answers\n", d->bus, d->dev, d->func,
                    r->initializing ? "still initializing" : "not answering");
            return false;
        }
        r->stuck = r->broken = false;
        if (!r->owed) {
            /* A bus reset's: the configuration goes back, and an FLR follows on it where the
               function has one. */
            if (!d->pcie_cap ||
                !(pci_read32(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCAP) & PCIE_DEVCAP_FLR)) {
                state_restore(d, &r->state);
                d->reset = NULL;
                kfree(r);
                links_retrain(true);
                return false;
            }
            r->owed = true;
        }
        if (!reset_quiesce(d, r)) return false;
    }
    /* Times are whole ticks counted from a reading taken just after the event, which may be up
       to one short of the time since it: n ms have passed only once more than n are counted. */
    if (r->phase == RESET_QUIESCED) {
        /* Transactions Pending: until it clears, or until no Completion can still arrive, the
           FLR issued either way (6.6.2 permits it with the bit set). Polled to the end of the
           window, nothing of it remains to be waited out after the FLR. An attempt that ends
           before its FLR leaves the next one to stop the function and wait again. */
        bool pending, there = true;
        while ((pending = transactions_pending(d)) && uptime_ms() - r->quiesced <= r->window) {
            if (!(there = present(d)) || !reset_wait(1)) break; /* scheduler resolution, not a deadline */
        }
        if (there) there = present(d);
        if (!there || reset_cancelled()) {
            r->stuck = !there;
            r->phase = RESET_IDLE;
            return false;
        }
        if (pending)
            kprintf("pci: %02x:%02x.%u: requests still pending %u ms after it stopped issuing them; reset anyway\n",
                    d->bus, d->dev, d->func, r->window);
        unsigned rtr = flr_time_ms(d); /* read before the FLR, which may discard reads (6.6.2) */
        pci_write16(d->bus, d->dev, d->func, d->pcie_cap + PCIE_DEVCTL,
                    r->state.pcie_ctl[0] | PCIE_DEVCTL_FLR);
        r->since = uptime_ms(); /* allow the full delay after the initiating configuration write */
        r->phase = RESET_ISSUED;
        r->wait = rtr ? rtr : FLR_DONE_MS;
        r->ready = rtr ? rtr : READY_MS;
    }
    /* Issued: a cancelled wait leaves the FLR's times for the next attempt to go on with. */
    for (;;) {
        uint64_t elapsed = uptime_ms() - r->since;
        if (reset_cancelled()) return false;
        if (elapsed > r->wait) break;
        if (!reset_wait(r->wait - elapsed + 1)) return false;
    }
    /* The first access is this Vendor ID read (2.3.1). While the function initializes after the
       FLR's 100 ms (6.6.2) it does not give the ID (id_pending), up to the readiness limit. */
    uint32_t id;
    for (;;) {
        if (port) rrs_show(port, true); /* AML may have rewritten Root Control during a wait */
        id = pci_read32(d->bus, d->dev, d->func, 0);
        r->initializing = id == PCI_CONFIG_RRS_ID;
        if (!id_pending(id, true) || uptime_ms() - r->since > r->ready) break;
        if (!reset_wait(1)) return false;
    }
    /* Transactions Pending must be clear once the FLR has completed (6.6.2). Initiate Function
       Level Reset may be written again (7.5.3.4): the next attempt issues another FLR. */
    if (id != r->state.id || transactions_pending(d)) {
        kprintf("pci: %02x:%02x.%u: %s %u ms after its FLR; its saved configuration is kept for another FLR\n",
                d->bus, d->dev, d->func,
                r->initializing ? "still initializing" : id != r->state.id ? "not answering" : "requests pending",
                r->ready);
        r->stuck = true;
        r->phase = RESET_IDLE;
        return false;
    }
    state_restore(d, &r->state);
    links_retrain(true);
    /* still mastering: the next attempt finds it ready and writes the configuration again */
    if (!present(d) || (pci_read16(d->bus, d->dev, d->func, PCI_COMMAND) & PCI_COMMAND_MASTER)) return false;
    d->reset = NULL;
    kfree(r);
    return true;
}

bool pci_function_reset(struct pci_device *d) {
    if (reset_cancelled()) return false;
    struct pci_device *port = rrs_port(d);
    if (port) rrs_hold(port);
    bool done = reset_attempt(d, port);
    if (port) rrs_release(port);
    return done;
}

bool pci_reset_stuck(const struct pci_device *d) { return d->reset && d->reset->stuck; }

/* Software performing an FLR keeps the other software that may access the function from it
   until the FLR is done (6.6.2), and a Request reaching it meanwhile may be discarded: so while
   an attempt or a bus reset runs, and up to the readiness limit of an FLR one left. A function
   that last answered RRS is kept from it too: with the Root Port not showing RRS the Root
   Complex would retry that read (2.3.2). Its Vendor ID is read again here, the Root Port showing
   RRS, so that a function that came back is not kept from it for good. Like the reset, this
   runs on the BSP, where kernel code is not preempted: d->reset cannot change between this
   check and the access it guards. */
bool pci_config_blocked(uint8_t bus, uint8_t dev, uint8_t func) {
    struct pci_device *d = pci_find_function(bus, dev, func);
    struct pci_reset *r = d ? d->reset : NULL;
    if (!r) return false;
    bool resetting = r->phase == RESET_QUIESCED || r->phase == RESET_BUS ||
                     (r->phase == RESET_ISSUED && uptime_ms() - r->since <= r->ready);
    struct pci_device *port = resetting || !r->initializing ? NULL : rrs_port(d);
    if (port) {
        rrs_hold(port);
        r->initializing = pci_read32(bus, dev, func, 0) == PCI_CONFIG_RRS_ID;
        rrs_release(port);
    }
    if (!resetting && !r->initializing) return false;
    if (!r->refused)
        kprintf("pci: %02x:%02x.%u is being reset: other configuration accesses to it are dropped until it is ready\n",
                bus, dev, func);
    r->refused = true;
    return true;
}

/* ---- Conventional Reset: a sleep's, or a Secondary Bus Reset's (6.6.1) ----
   No Configuration Request may go to a device below a Downstream Port before 100 ms have passed:
   since the end of the reset where the port supports Link speeds up to 5.0 GT/s, since the end of
   Link training where it supports more, which its Data Link Layer Link Active bit shows (7.5.3.8;
   such a port must report it, 7.5.3.6). A link seen active had trained by then. One not active
   1.0 s after the reset leads to no device that must still be waited for. Once that can be told,
   sets b->below and returns true. */
static bool below_ready(struct pci_device *b) {
    uint32_t cap = pci_read32(b->bus, b->dev, b->func, b->pcie_cap + PCIE_LNKCAP);
    if ((cap & PCIE_LNKCAP_SPEED) <= 2 || !(cap & PCIE_LNKCAP_DLL_REPORTING)) b->below = b->reset_end + tsc_hz / 10;
    else if (pci_read16(b->bus, b->dev, b->func, b->pcie_cap + PCIE_LNKSTA) & PCIE_LNKSTA_DLL_ACTIVE)
        b->below = rdtsc() + tsc_hz / 10;
    else if (rdtsc() - b->reset_end >= tsc_hz) b->below = UINT64_MAX;
    else return false;
    return true;
}

/* At least ms by the TSC, which no kill shortens: a reset once sent is seen through. */
static void reset_hold(unsigned ms, bool sleeps) {
    for (uint64_t t = rdtsc(); rdtsc() - t < tsc_hz / 1000 * ms;) wait_step(sleeps);
}

/* The Downstream Port whose link d's device is on: the bridge above d, or above the switch d is
   a port of; NULL on a root bus. */
static struct pci_device *port_above(const struct pci_device *d) {
    for (struct pci_device *b = d->upstream; b; b = b->upstream)
        if (b->pcie_cap && (b->pcie_type == 4 || b->pcie_type == 6)) return b;
    return NULL;
}

/* Waits until d may be read as far as the Downstream Port above it goes: false when the link
   below that port is down. *since: given as the end of the reset, from which the 1.0 s for d's
   ID count; a Downstream Port of a switch the reset went through gives its own. The switch has
   its Downstream Ports send a hot reset while its Upstream Port's link is down or in Hot Reset
   (6.6.1), which is over once the switch can be read; each side of their links then leaves Hot
   Reset within 2 ms (4.2.7.11). So such a link's reset is taken to end 4 ms after something
   below it is first awaited, by when the switch was read. */
static bool below_wait(const struct pci_device *d, uint64_t *since, bool sleeps) {
    struct pci_device *b = port_above(d);
    if (!b) return true;
    if (!b->reset_end) {
        reset_hold(4, sleeps);
        b->reset_end = rdtsc();
    }
    *since = b->reset_end;
    while (!b->below && !below_ready(b)) wait_step(sleeps);
    if (b->below == UINT64_MAX) return false;
    while (rdtsc() < b->below) wait_step(sleeps);
    return true;
}

struct pci_device *pci_bus_reset_port(const struct pci_device *d) {
    struct pci_device *port = d->upstream;
    if (!port || !port->pcie_cap || (port->pcie_type != 4 && port->pcie_type != 6)) return NULL;
    if (d->reset && d->reset->broken) return NULL;
    if ((pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_CAPS) & PCIE_CAPS_SLOT) &&
        (pci_read32(port->bus, port->dev, port->func, port->pcie_cap + PCIE_SLOTCAP) & PCIE_SLOTCAP_HOTPLUG))
        return NULL;
    return port;
}

/* What pci_bus_reset may not do, logged: NULL when it may. */
static const char *bus_reset_refused(const struct pci_device *port) {
    if (!port->pcie_cap || (port->pcie_type != 4 && port->pcie_type != 6)) return "it is no Downstream Port";
    if (devices_lost) return "some functions below it may be unknown";
    if (!present(port)) return "it does not answer";
    if (pci_read16(port->bus, port->dev, port->func, PCI_BRIDGE_CONTROL) & PCI_BRIDGE_CONTROL_SBR)
        return "it is in reset already";
    if (port->pcie_v2 &&
        (pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_LNKCTL2) & PCIE_LNKCTL2_COMPLIANCE))
        return "its link is set to Enter Compliance";
    for (size_t i = 0; i < ndevices; i++) {
        const struct pci_device *f = &devices[i], *up = f->upstream;
        if (pci_below(port, f) && (!up->pcie_cap || up->pcie_type == 7))
            return "a conventional PCI bus is below it, whose first access waits Trhfa";
    }
    return NULL;
}

/* Holds the configuration of each function below port that has none held (a new record, its
   phase RESET_BUS until the reset), its bus mastering off: false, with nothing held, when one
   does not answer or memory runs out. */
static bool bus_reset_hold(struct pci_device *port) {
    const char *why = NULL;
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *f = &devices[i];
        if (!pci_below(port, f) || f->reset) continue;
        struct pci_reset *r = NULL;
        if (!present(f)) why = "does not answer";
        else if (!(r = kzalloc(sizeof *r))) why = "has no memory to hold its configuration";
        if (why) {
            kprintf("pci: %02x:%02x.%u: no Secondary Bus Reset: %02x:%02x.%u below it %s\n", port->bus,
                    port->dev, port->func, f->bus, f->dev, f->func, why);
            break;
        }
        state_save(f, &r->state, false);
        r->state.pcie_ctl[0] &= ~PCIE_DEVCTL_FLR;
        r->phase = RESET_BUS;
        f->reset = r;
    }
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *f = &devices[i];
        struct pci_reset *r = f->reset;
        if (!pci_below(port, f) || !r || r->phase != RESET_BUS) continue;
        if (why) {
            f->reset = NULL;
            kfree(r);
        } else if (f->bridge) {
            /* a bridge forwards the requests of what is below it (7.5.1.1.3); its own bus is
               not reset when its configuration goes back */
            r->state.hdr[15] &= ~((uint32_t)PCI_BRIDGE_CONTROL_SBR << 16);
        } else {
            r->state.hdr[1] &= ~PCI_COMMAND_MASTER;
            pci_write16(f->bus, f->dev, f->func, PCI_COMMAND, r->state.hdr[1] & 0xFFFF);
        }
    }
    return !why;
}

/* The port's LTR Mechanism Enable, which its link going down cleared (7.5.3.16), set again
   before the configuration below it goes back: closest to the Root Port first (6.18). */
static void ltr_enable(const struct pci_device *port) {
    uint16_t control2 = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_DEVCTL2);
    pci_write16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_DEVCTL2, control2 | PCIE_DEVCTL2_LTR);
}

bool pci_bus_reset(struct pci_device *port) {
    const char *why = bus_reset_refused(port);
    if (why) {
        kprintf("pci: %02x:%02x.%u: no Secondary Bus Reset: %s\n", port->bus, port->dev, port->func, why);
        return false;
    }
    if (!bus_reset_hold(port)) return false;
    struct pci_device *rrs = NULL;
    bool first = true;
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *f = &devices[i];
        if (!pci_below(port, f)) continue;
        if (first) rrs = rrs_port(f); /* the same for every function below */
        first = false;
        f->reset->phase = RESET_BUS;
        f->below = f->reset_end = 0;
    }
    uint16_t control = pci_read16(port->bus, port->dev, port->func, PCI_BRIDGE_CONTROL);
    bool dll = pci_read32(port->bus, port->dev, port->func, port->pcie_cap + PCIE_LNKCAP) & PCIE_LNKCAP_DLL_REPORTING;
    uint16_t link = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_LNKSTA);
    bool was_up = dll && (link & PCIE_LNKSTA_DLL_ACTIVE); /* its speed and width are valid */
    bool ltr = port->pcie_v2 &&
               (pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_DEVCTL2) & PCIE_DEVCTL2_LTR);
    if (rrs) rrs_hold(rrs);
    kprintf("pci: %02x:%02x.%u: Secondary Bus Reset\n", port->bus, port->dev, port->func);
    pci_write16(port->bus, port->dev, port->func, PCI_BRIDGE_CONTROL, control | PCI_BRIDGE_CONTROL_SBR);
    reset_hold(1, true); /* Trst */
    if (dll) {
        uint16_t status;
        for (uint64_t t = rdtsc(); ((status = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_LNKSTA)) &
                                    PCIE_LNKSTA_DLL_ACTIVE) && rdtsc() - t < tsc_hz;)
            wait_step(true);
        if (status & PCIE_LNKSTA_DLL_ACTIVE)
            kprintf("pci: %02x:%02x.%u: its link stayed up 1 s into the reset\n", port->bus, port->dev, port->func);
    }
    pci_write16(port->bus, port->dev, port->func, PCI_BRIDGE_CONTROL, control);
    reset_hold(2, true);
    port->reset_end = rdtsc();
    port->below = 0;
    bool came = false;
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *f = &devices[i];
        if (!pci_below(port, f)) continue;
        struct pci_reset *r = f->reset;
        uint64_t reset_end = port->reset_end;
        uint32_t id = UINT32_MAX;
        /* reached once the bridge above has its bus numbers back */
        if ((f->upstream == port || !f->upstream->reset) && below_wait(f, &reset_end, true))
            id = ready_id(rrs, f->bus, f->dev, f->func, reset_end, true, true);
        r->phase = RESET_IDLE;
        r->initializing = id == PCI_CONFIG_RRS_ID;
        r->stuck = r->broken = id != r->state.id;
        if (r->stuck) {
            kprintf("pci: %02x:%02x.%u: %s after the Secondary Bus Reset; its configuration stays held\n", f->bus,
                    f->dev, f->func, r->initializing ? "still initializing 1 s" : "not answering");
            continue;
        }
        came |= f->upstream == port;
        if (ltr) {
            ltr_enable(port);
            ltr = false;
        }
        if (r->owed) continue; /* its FLR writes it back (pci_function_reset) */
        state_restore(f, &r->state);
        f->reset = NULL;
        kfree(r);
    }
    if (ltr) ltr_enable(port); /* for a function that answers later */
    uint16_t now = pci_read16(port->bus, port->dev, port->func, port->pcie_cap + PCIE_LNKSTA);
    if (was_up && came &&
        ((now & PCIE_LNKSTA_SPEED) < (link & PCIE_LNKSTA_SPEED) || (now & PCIE_LNKSTA_WIDTH) < (link & PCIE_LNKSTA_WIDTH)))
        kprintf("pci: %02x:%02x.%u: its link came back at speed %u x%u, below speed %u x%u before the reset; "
                "equalization (4.2.3) is left to the hardware\n",
                port->bus, port->dev, port->func, now & PCIE_LNKSTA_SPEED, (now & PCIE_LNKSTA_WIDTH) >> 4,
                link & PCIE_LNKSTA_SPEED, (link & PCIE_LNKSTA_WIDTH) >> 4);
    links_retrain(true);
    if (rrs) rrs_release(rrs);
    return true;
}

/* ---- sleep ---- */
static struct pci_saved *saved;
static bool saved_valid;

/* A function with a reset is left to it: the reset holds its configuration and writes it back
   once an FLR has been seen through (pci_function_reset; after a sleep, pci_resume). */
const char *pci_suspend(void) {
    saved_valid = false;
    if (!saved) saved = kzalloc(ndevices * sizeof *saved);
    if (!saved) return "PCI (no memory to save it)";
    for (size_t i = 0; i < ndevices; i++)
        if (!devices[i].reset && !state_save(&devices[i], &saved[i], true)) return "PCI (no memory to save it)";
    saved_valid = true;
    return NULL;
}

/* What was saved, for units that did not come back from the sleep (iommu_resume), before any of
   it is written back, now or by a reset later: the messages saved in the format of the units'
   remapping made again from what they were given, and one that cannot be left off (MSI Enable
   clear, an MSI-X entry masked); a function in an ended claim's group without bus mastering,
   as the units that dropped its requests are off. */
static void state_remake(struct pci_device *d, struct pci_saved *s) {
    uint32_t address, data;
    bool off = false;
    if (d->iommu_blocked) s->hdr[1] &= ~(uint32_t)PCI_COMMAND_MASTER;
    const struct pci_msi_given *g = &d->msi_given;
    if (s->msi && g->vector) {
        bool wide = (s->msi_regs[0] >> 16) & 0x80;
        uint32_t *at = &s->msi_regs[wide ? 3 : 2];
        if (iommu_msi_message(d, 0, g->count, g->vector, g->dest, &address, &data)) {
            s->msi_regs[1] = address;
            if (wide) s->msi_regs[2] = 0;
            *at = (*at & 0xFFFF0000u) | data;
        } else {
            s->msi_regs[0] &= ~0x00010000u;
            off = true;
        }
    }
    for (unsigned i = 0; s->msix && s->msix_table && i < d->msix_entries; i++) {
        uint32_t *e = s->msix_table + i * 4;
        g = &d->msix_given[i];
        if (!g->vector) continue;
        if (iommu_msi_message(d, i, 1, g->vector, g->dest, &address, &data)) {
            e[0] = address;
            e[1] = 0;
            e[2] = data;
        } else {
            e[3] |= 1;
            off = true;
        }
    }
    if (off) kprintf("pci: %02x:%02x.%u: an interrupt message could not be made again: left off\n", d->bus, d->dev,
                     d->func);
}

/* After S3 every function has been through a Conventional Reset, whose end is not visible here:
   the resume is an event known to follow it (6.6.1), from which the Root Ports' links count.
   Root Ports and switch ports come before the functions below them in enumeration order: a port
   is written back before anything is read through it. The Root Port above shows RRS while a
   function's readiness is awaited, as 6.6.1 strongly recommends for 100 ms waits.
   A function with a reset is awaited like the others but nothing is written to it while an FLR
   is owed: its reset holds what to write back. After a sleep, no FLR it had counts as done. What
   the sleep did to the function is the platform's (6.6: a Conventional Reset relates to a
   platform's reset as the platform makes it), and a Conventional Reset is not an FLR in any case
   (6.6.2: "Other defined reset operations do not guarantee that external I/O operations will be
   stopped"): its next attempt stops it and issues another. One a bus reset alone holds gets its
   configuration back like any other. Without a sleep (slept false: a hibernation's snapshot, or
   sleep called off) such a reset is left as it was. */
void pci_resume(bool slept, bool remake) {
    if (!saved_valid) return; /* the sleep was called off before anything went down */
    uint64_t since = rdtsc();
    for (size_t i = 0; remake && i < ndevices; i++)
        state_remake(&devices[i], devices[i].reset ? &devices[i].reset->state : &saved[i]);
    for (size_t i = 0; i < ndevices; i++) {
        devices[i].below = 0;
        devices[i].reset_end = devices[i].pcie_cap && devices[i].pcie_type == 4 ? since : 0;
    }
    /* The links below the Root Ports train side by side after the one reset: each port's wait
       is its own, so all of them are watched together and no wait adds to another. Only ports
       with a function below: an empty slot's is not waited for. */
    for (bool watching = true; watching;) {
        watching = false;
        for (size_t i = 0; i < ndevices; i++) {
            struct pci_device *b = devices[i].upstream;
            if (b && b->pcie_cap && b->pcie_type == 4 && !b->below) watching |= !below_ready(b);
        }
        if (watching) pause();
    }
    for (size_t i = 0; i < ndevices; i++) {
        struct pci_device *d = &devices[i];
        struct pci_reset *r = d->reset;
        if (r && !slept) continue;
        if (r) r->phase = RESET_IDLE;
        uint64_t reset_end = since;
        if (!below_wait(d, &reset_end, false)) {
            kprintf("pci: %02x:%02x.%x is not there after sleep (the link above it is down)\n", d->bus, d->dev,
                    d->func);
            if (r) r->stuck = r->broken = true;
            continue;
        }
        struct pci_device *port = rrs_port(d);
        if (port) rrs_hold(port);
        uint32_t id = ready_id(port, d->bus, d->dev, d->func, reset_end, true, false);
        if (port) rrs_release(port);
        uint32_t was = r ? r->state.id : saved[i].id;
        if (r) {
            r->initializing = id == PCI_CONFIG_RRS_ID;
            r->stuck = r->broken = id != was;
        }
        if (id != was) {
            kprintf("pci: %02x:%02x.%x is %s after sleep (ID %08x, was %08x)\n", d->bus, d->dev, d->func,
                    id == PCI_CONFIG_RRS_ID ? "still initializing 1 s" : "not there", id, was);
            continue;
        }
        if (r && r->owed) continue;
        state_restore(d, r ? &r->state : &saved[i]);
        if (r) {
            d->reset = NULL;
            kfree(r);
        }
    }
    links_retrain(false);
}
