#pragma once
#include <stdint.h>
#include <stdbool.h>
struct pci_device {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t cls, subcls, progif, irq;
    uint32_t bar[6];
    uint64_t bar_size[6]; /* memory BARs, measured once during enumeration; 0 for I/O, unused, upper halves */
    uint8_t pcie_cap;  /* PCI Express capability offset, 0 for conventional PCI */
    uint8_t pcie_type; /* device/port type field of the PCIe capability (valid when pcie_cap != 0) */
    uint16_t msix_entries;      /* set by pci_msix_init() */
    volatile uint32_t *msix_table; /* mapped MSI-X table, 16 bytes per entry */
    bool kernel_owned; /* a kernel driver enabled it (pci_enable_busmaster): no app may claim it */
    bool claimed;      /* an app drives it (dev/pciuser.c) */
};
void pci_init(void);
/* Config space access. offsets are 0..4095: below 256 CF8/CFC or ECAM, 256 and up ECAM only (an
   unreachable register reads as 0xFFFFFFFF and ignores writes). Segment 0 only. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t v);
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint8_t v);
struct pci_device *pci_find(uint16_t vendor, uint16_t device);
struct pci_device *pci_find_class(uint8_t cls, uint8_t subcls);
void pci_enable_busmaster(struct pci_device *d);
int pci_count(void);
struct pci_device *pci_at(int index);
struct pci_device *pci_find_function(uint8_t bus, uint8_t dev, uint8_t func);
/* The PCI-to-PCI bridge whose secondary bus is the function's bus, or NULL on the root buses. */
struct pci_device *pci_upstream(const struct pci_device *d);
/* Memory BAR base (64-bit BARs combined); 0 for I/O, unassigned or out-of-range BARs. */
uint64_t pci_bar_address(const struct pci_device *d, unsigned bar);
/* How many BAR dwords the function's header type has (6, 2 for a bridge, 1 for CardBus). */
unsigned pci_bar_count(const struct pci_device *d);
/* Its size, as measured during enumeration (pci_init), so asking never touches the device. */
uint64_t pci_bar_size(const struct pci_device *d, unsigned bar);
uint8_t pci_find_cap(const struct pci_device *d, uint8_t id);
/* Extended capability (config offset 0x100 chain, PCIe): its offset, or 0. Needs ECAM. */
uint16_t pci_find_ext_cap(const struct pci_device *d, uint16_t id);

/* Message-signalled interrupts. All of them target a physical APIC id of at most 8 bits (the MSI
   address, and the remapping entries dev/iommu.c writes, carry no more), so ids above 255 (x2APIC
   systems with many CPUs) are refused. The address and data come from iommu_msi_message.
   Device interrupts are handled on the BSP only: pass lapic_current_id() from the BSP. */
/* Deliver the function's interrupt as `vector` on the BSP through MSI or MSI-X (entry 0). */
bool pci_msi_enable(struct pci_device *d, uint8_t vector);
/* Number of messages the device's MSI capability supports (1, 2, 4 .. 32), 0 without MSI. */
unsigned pci_msi_max_vectors(const struct pci_device *d);
/* Enable `count` (power of two, <= pci_msi_max_vectors) MSI messages: message n arrives as base_vector + n,
   so base_vector must be a multiple of count (irq_vector_alloc(count, count)). Clears any per-vector masks. */
bool pci_msi_enable_multi(struct pci_device *d, uint8_t base_vector, unsigned count, uint32_t dest_apic_id);
/* MSI-X: init enables MSI-X with the function mask set and every entry masked; returns the table size
   (0 if unsupported). route programs an entry's address/data and leaves its mask state alone. Unmask the
   entries you want with pci_msix_mask(), then drop the function mask. INTx is disabled by init. */
unsigned pci_msix_init(struct pci_device *d);
bool pci_msix_route(struct pci_device *d, unsigned entry, uint8_t vector, uint32_t dest_apic_id);
void pci_msix_mask(struct pci_device *d, unsigned entry, bool masked);
void pci_msix_function_mask(struct pci_device *d, bool masked);
void pci_msi_disable(struct pci_device *d); /* turns off both MSI and MSI-X */
/* Sleep (dev/pm.c): save every function's configuration before S3, write it back after (BSP,
   before the drivers resume). pci_suspend allocates and so runs with interrupts on. */
void pci_suspend(void);
void pci_resume(void);
