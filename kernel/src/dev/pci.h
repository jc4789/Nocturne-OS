#pragma once
#include <stdint.h>
struct pci_device {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t cls, subcls, progif, irq;
    uint32_t bar[6];
};
void pci_init(void);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
struct pci_device *pci_find(uint16_t vendor, uint16_t device);
struct pci_device *pci_find_class(uint8_t cls, uint8_t subcls);
void pci_enable_busmaster(struct pci_device *d);
