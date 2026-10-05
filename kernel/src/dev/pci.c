/* PCI configuration space access (mechanism #1) and bus enumeration. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "abi.h"

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

static struct pci_device devices[64];
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
