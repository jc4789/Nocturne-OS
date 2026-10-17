/* PCI configuration space access (mechanism #1) and bus enumeration. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "abi.h"
#include "mm/vmm.h"

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2) * 8));
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
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, dev, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                uint32_t cls = pci_read32(bus, dev, func, 8);
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
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4);
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

bool pci_msi_enable(struct pci_device *d, uint8_t vector) {
    if (!lapic_present()) return false;
    uint32_t address = 0xFEE00000u | (lapic_current_id() & 0xFF) << 12;
    uint8_t cap = pci_find_cap(d, 0x05);
    if (cap) {
        uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
        uint16_t ctl = h >> 16;
        bool wide = ctl & 0x80;
        pci_write32(d->bus, d->dev, d->func, cap + 4, address);
        if (wide) pci_write32(d->bus, d->dev, d->func, cap + 8, 0);
        uint8_t data_off = cap + (wide ? 0x0C : 0x08);
        uint32_t data = pci_read32(d->bus, d->dev, d->func, data_off);
        pci_write32(d->bus, d->dev, d->func, data_off, (data & 0xFFFF0000u) | vector);
        uint8_t msix = pci_find_cap(d, 0x11); /* never both: firmware may have left MSI-X on */
        if (msix) {
            uint32_t xh = pci_read32(d->bus, d->dev, d->func, msix);
            pci_write32(d->bus, d->dev, d->func, msix, xh & ~0x80000000u);
        }
        ctl = (ctl & ~0x70u) | 1; /* one message, enabled */
        pci_write32(d->bus, d->dev, d->func, cap, (h & 0xFFFF) | (uint32_t)ctl << 16);
    } else if ((cap = pci_find_cap(d, 0x11))) {
        uint32_t h = pci_read32(d->bus, d->dev, d->func, cap);
        uint32_t table = pci_read32(d->bus, d->dev, d->func, cap + 4);
        uint64_t base = pci_bar_address(d, table & 7);
        unsigned entries = ((h >> 16) & 0x7FF) + 1;
        if (!base) return false;
        volatile uint32_t *t = vmm_map_mmio(base + (table & ~7u), entries * 16);
        /* enable with the whole function masked while the entries are written */
        pci_write32(d->bus, d->dev, d->func, cap, (h & 0x3FFFFFFF) | 0xC0000000u);
        for (unsigned i = 1; i < entries; i++) t[i * 4 + 3] |= 1;
        t[0] = address;
        t[1] = 0;
        t[2] = vector;
        t[3] &= ~1u;
        pci_write32(d->bus, d->dev, d->func, cap, (h & 0x3FFFFFFF) | 0x80000000u);
    } else {
        return false;
    }
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 4);
    pci_write32(d->bus, d->dev, d->func, 4, cmd | 0x400); /* INTx off */
    return true;
}
