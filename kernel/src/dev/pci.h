#pragma once
#include <stdint.h>
#include <stdbool.h>
struct pci_reset;
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
    bool needs_reset; /* stopped state unconfirmed, or residual state from an ended multi-function group */
    struct pci_reset *reset; /* saved configuration while an FLR awaits readiness or its caller was cancelled */
    unsigned rrs_holds;  /* a Root Port: resets below it awaiting readiness through it (pci.c) */
    bool rrs_firmware;   /* a Root Port: RRS Software Visibility was already on at the first hold */
};
void pci_init(void);
/* Config space access. offsets are 0..4095: below 256 CF8/CFC or ECAM, 256 and up ECAM only (an
   unreachable register reads as 0xFFFFFFFF and ignores writes). Segment 0 only. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off);
void pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint16_t v);
void pci_write8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint8_t v);
#define PCI_COMMAND        0x04 /* Command register (7.5.1.1.3), narrow writes preserve Status */
#define PCI_COMMAND_MASTER (1u << 2) /* Command.Bus Master Enable (7.5.1.1.3) */
#define PCI_CONFIG_RRS_ID  0xFFFF0001u /* Vendor/Device ID read with RRS Software Visibility (2.3.2) */
struct pci_device *pci_find(uint16_t vendor, uint16_t device);
struct pci_device *pci_find_class(uint8_t cls, uint8_t subcls);
void pci_enable_busmaster(struct pci_device *d);
/* Clear BME and confirm the same function still answers with BME off. Optionally report its
   previous BME state. This stops new Memory/I/O Requests, not a posted DMA drain. No wait. */
bool pci_disable_busmaster(struct pci_device *d, bool *was_mastering);
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
/* Turns off both MSI and MSI-X: false when Enable readback or interrupt remapping did not
   confirm the function's messages lead nowhere; its vectors must not be given to anything else. */
bool pci_msi_disable(struct pci_device *d);
/* Registers of the PCI Express capability (PCI Express Base 6.0, 7.5.3), at pcie_cap + offset */
#define PCIE_DEVCAP  0x04
#define PCIE_DEVCTL  0x08
#define PCIE_DEVSTA  0x0A
#define PCIE_LNKCTL  0x10
#define PCIE_ROOTCTL 0x1C
#define PCIE_ROOTCAP 0x1E
#define PCIE_DEVCTL2 0x28
#define PCIE_LNKCTL2 0x30
#define PCIE_DEVCAP_FLR         (1u << 28) /* Function Level Reset Capability */
#define PCIE_DEVCTL_FLR         (1u << 15) /* Initiate Function Level Reset */
#define PCIE_DEVSTA_PENDING     (1u << 5)  /* Transactions Pending */
#define PCIE_DEVCTL2_CT_DISABLE (1u << 4)  /* Completion Timeout Disable */
#define PCIE_LNKCTL_DISABLE      (1u << 4)  /* Link Disable (7.5.3.7) */
#define PCIE_LNKCTL2_COMPLIANCE  (1u << 4)  /* Enter Compliance (7.5.3.19) */
#define PCIE_LNKCTL2_MOD_COMPLIANCE (1u << 10) /* Enter Modified Compliance (7.5.3.19) */
#define PCIE_ROOTCTL_RRS_VISIBLE (1u << 4) /* Configuration RRS Software Visibility Enable (7.5.3.12) */
#define PCIE_ROOTCAP_RRS_VISIBLE (1u << 0) /* Configuration RRS Software Visibility (7.5.3.13) */
/* After bus mastering is off a function starts no memory/I/O request, but some may still be in
   flight. Transactions Pending reports outstanding non-posted PCIe requests only. A host read
   flushes TC0 writes under the ordering rules; other TCs and relaxed/ID ordering require the
   driver's own synchronization (6.4, 2.4). This is not a universal DMA drain. Waits up to ms;
   ms == 0 checks once without sleeping. A conventional PCI master waits for no completion: its
   memory writes are posted, and the read that saw bus mastering off pulled them out of every
   bridge on the way (PCI Local Bus 3.0, 3.2.5); a read a bridge still holds for it is never
   repeated, and its data is discarded (3.3.3.3.3). So for conventional PCI the function only has
   to still be there. False for a function gone from the bus. May sleep. */
bool pci_requests_done(const struct pci_device *d, unsigned ms);
/* Function Level Reset (6.6.2), its configuration saved before and written back after (the MSI-X
   table's entries excepted: a driver's, which it set up for itself), with Bus Master Enable
   always cleared in the restored configuration. False when it has no FLR, is gone, is still
   mastering afterwards, or is not configuration-ready yet. A pending reset keeps its saved
   configuration for a later call; cancellation must not discard or recapture that state.
   This resets device state, not an IOMMU drain
   confirmation: its success alone must not release quarantined DMA pages. May sleep, with
   cancellation during scheduler waits: FLR/RTR, or while software-visible RRS reports
   initialization. The Root Port above shows RRS from the FLR until readiness and then gets its
   former Root Control back; without one that supports it (an RCiEP, an RCRB root) the Root
   Complex retries inside the configuration read, which no cancellation can interrupt.
   BME must be confirmed off and pre-FLR Transactions Pending must actually clear for every
   Completion Timeout value: a previous driver may have re-enabled timeout without starting
   timers for existing requests. No invented delay substitutes for that observation or
   device/platform-specific quiescence. */
bool pci_function_reset(struct pci_device *d);
/* Sleep (dev/pm.c): save every function's configuration before S3, write it back after (BSP,
   before the drivers resume). pci_suspend allocates and so runs with interrupts on; it returns
   NULL, or what could not be saved (the sleep is then called off and pci_resume writes nothing). */
const char *pci_suspend(void);
void pci_resume(void);
