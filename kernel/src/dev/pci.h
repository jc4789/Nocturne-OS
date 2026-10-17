#pragma once
#include <stdint.h>
#include <stdbool.h>
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
int pci_count(void);
struct pci_device *pci_at(int index);
/* Memory BAR base (64-bit BARs combined); 0 for I/O, unassigned or out-of-range BARs. */
uint64_t pci_bar_address(const struct pci_device *d, unsigned bar);
uint64_t pci_bar_size(const struct pci_device *d, unsigned bar);
uint8_t pci_find_cap(const struct pci_device *d, uint8_t id);
/* Deliver the function's interrupt as `vector` on the BSP through MSI or MSI-X (entry 0). */
bool pci_msi_enable(struct pci_device *d, uint8_t vector);
