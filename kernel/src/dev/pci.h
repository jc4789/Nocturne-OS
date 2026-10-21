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
    bool bridge;                  /* a PCI-to-PCI bridge (header type 1) */
    uint8_t secondary, subordinate; /* a bridge's buses: those below it, as enumeration found them */
    struct pci_device *upstream;  /* the bridge whose secondary bus this function is on, or NULL */
    uint16_t msix_entries;      /* set by pci_msix_init() */
    volatile uint32_t *msix_table; /* mapped MSI-X table, 16 bytes per entry */
    bool kernel_owned; /* a kernel driver enabled it (pci_enable_busmaster): no app may claim it */
    bool claimed;      /* an app drives it (dev/pciuser.c) */
    bool iommu_blocked; /* in an ended claim's group: the IOMMU drops what it sends (dev/iommu.c) */
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
/* Whether o is below bridge b (on one of its buses), and whether a and b are functions of one
   device: the same device number, or any two on the link below a PCI Express port, which leads to
   one device only (an ARI device's functions use the device number bits too). */
bool pci_below(const struct pci_device *b, const struct pci_device *o);
/* False when enumeration ran out of memory: some functions are unknown (and so unusable). */
bool pci_all_known(void);
bool pci_same_device(const struct pci_device *a, const struct pci_device *b);
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
/* Turns off both MSI and MSI-X: false when interrupt remapping did not confirm the function's
   messages lead nowhere, so that its vectors must not be given to anything else. */
bool pci_msi_disable(struct pci_device *d);
/* Registers of the PCI Express capability (PCI Express Base 6.0, 7.5.3), at pcie_cap + offset */
#define PCIE_DEVCAP  0x04
#define PCIE_DEVCTL  0x08
#define PCIE_DEVSTA  0x0A
#define PCIE_DEVCTL2 0x28
#define PCIE_DEVCAP_FLR         (1u << 28) /* Function Level Reset Capability */
#define PCIE_DEVCTL_FLR         (1u << 15) /* Initiate Function Level Reset */
#define PCIE_DEVSTA_PENDING     (1u << 5)  /* Transactions Pending */
#define PCIE_DEVCTL2_CT_DISABLE (1u << 4)  /* Completion Timeout Disable */
/* After bus mastering is off a function starts no request, but some may still be in flight. A
   PCI Express function says when none is (Device Status, Transactions Pending), and the config
   read that sees it so also follows any write it posted before (a completion does not pass a
   posted write). Waits up to ms for that. A conventional PCI master waits for no completion: its
   memory writes are posted, and the read that saw bus mastering off pulled them out of every
   bridge on the way (PCI Local Bus 3.0, 3.2.5); a read a bridge still holds for it is never
   repeated, and its data is discarded (3.3.3.3.3). So for conventional PCI the function only has
   to still be there. False for a function gone from the bus. May sleep. */
bool pci_requests_done(const struct pci_device *d, unsigned ms);
/* Function Level Reset (6.6.2), its configuration saved before and written back after (the MSI-X
   table's entries excepted: a driver's, which it set up for itself). Afterwards the function
   starts no request and leaves the memory it was given alone until it is set up again. False
   when it has no FLR, is gone, or was not ready in time. May sleep: up to its Completion Timeout
   when requests were pending, otherwise about a second at most. */
bool pci_function_reset(struct pci_device *d);
/* Sleep (dev/pm.c): save every function's configuration before S3, write it back after (BSP,
   before the drivers resume). pci_suspend allocates and so runs with interrupts on; it returns
   NULL, or what could not be saved (the sleep is then called off and pci_resume writes nothing). */
const char *pci_suspend(void);
void pci_resume(void);
