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
    bool pcie_v2;      /* its capability is version 2 or later: the registers past Root Status exist (7.5.3.2) */
    bool bridge;                  /* a PCI-to-PCI bridge (header type 1) */
    uint8_t secondary, subordinate; /* a bridge's buses: those below it, as enumeration found them */
    struct pci_device *upstream;  /* the bridge whose secondary bus this function is on, or NULL */
    uint16_t msix_entries;      /* set by pci_msix_init() */
    volatile uint32_t *msix_table; /* mapped MSI-X table, 16 bytes per entry */
    /* What its MSI and each MSI-X entry were last given (pci_msi_enable_multi, pci_msix_route),
       vector 0 for nothing: made again where the units that remapped them did not come back
       from a sleep (pci_resume). msix_given has msix_entries entries, allocated with the table. */
    struct pci_msi_given {
        uint32_t dest;
        uint8_t vector, count;
    } msi_given, *msix_given;
    bool kernel_owned; /* a kernel driver enabled it (pci_enable_busmaster): no app may claim it */
    bool claimed;      /* an app drives it (dev/pciuser.c) */
    bool iommu_blocked; /* in an ended claim's group: the IOMMU drops what it sends (dev/iommu.c) */
    bool needs_reset; /* stopped state unconfirmed, or residual state from an ended multi-function group */
    /* While set, the function is being reset (pci_function_reset) and its configuration from
       before is held to be written back: kept when an attempt is cancelled or fails, or a sleep
       comes between, until an FLR has been seen through. A function may discard Requests during
       its FLR (6.6.2): software that does not own the reset goes through pci_config_firmware_*. */
    struct pci_reset *reset;
    unsigned rrs_holds;  /* a Root Port: reset attempts or readiness waits below it running (pci.c) */
    bool rrs_firmware;   /* a Root Port: RRS Software Visibility was already on at the first hold */
    /* A Downstream Port after a Conventional Reset (sleep, pci_bus_reset), on the clock
       (clock_now_ns), 0 while not yet known: when the reset of its link ended, and from when what
       is below it may be read (CLOCK_NEVER: its link is down) */
    uint64_t reset_end, below;
    /* A Downstream Port: a configuration written back changed the Common Clock Configuration of
       the device below since its link trained, which takes effect once the link retrains (pci.c) */
    bool retrain;
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
#define PCI_COMMAND_IO     (1u << 0)  /* I/O Space Enable */
#define PCI_COMMAND_MEMORY (1u << 1)  /* Memory Space Enable */
#define PCI_COMMAND_MASTER (1u << 2)  /* Bus Master Enable */
#define PCI_COMMAND_PARITY (1u << 6)  /* Parity Error Response */
#define PCI_COMMAND_SERR   (1u << 8)  /* SERR# Enable */
#define PCI_COMMAND_INTX_DISABLE (1u << 10) /* Interrupt Disable: set, no INTx is asserted */
#define PCI_CONFIG_RRS_ID  0xFFFF0001u /* Vendor/Device ID read with RRS Software Visibility (2.3.2) */
#define PCI_BRIDGE_CONTROL 0x3E        /* a bridge's Bridge Control register (7.5.1.3.13) */
#define PCI_BRIDGE_CONTROL_SBR (1u << 6) /* Secondary Bus Reset */
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
   Device interrupts are handled on CPU 0: every message names pci_interrupt_dest(). */
uint32_t pci_interrupt_dest(void); /* CPU 0's APIC ID, any CPU (read by pci_init on CPU 0) */
/* Deliver the function's interrupt as `vector` on CPU 0 through MSI or MSI-X (entry 0). */
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
#define PCIE_CAPS    0x02
#define PCIE_DEVCAP  0x04
#define PCIE_DEVCTL  0x08
#define PCIE_DEVSTA  0x0A
#define PCIE_LNKCAP  0x0C
#define PCIE_LNKCTL  0x10
#define PCIE_LNKSTA  0x12
#define PCIE_SLOTCAP 0x14
#define PCIE_ROOTCTL 0x1C
#define PCIE_ROOTCAP 0x1E
/* version 2 and later only (pcie_v2) */
#define PCIE_DEVCAP2 0x24
#define PCIE_DEVCTL2 0x28
#define PCIE_LNKCTL2 0x30
#define PCIE_CAPS_SLOT          (1u << 8)  /* Slot Implemented (7.5.3.2) */
#define PCIE_SLOTCAP_HOTPLUG    (1u << 6)  /* Hot-Plug Capable (7.5.3.9) */
#define PCIE_DEVCAP_FLR         (1u << 28) /* Function Level Reset Capability */
#define PCIE_DEVCTL_FLR         (1u << 15) /* Initiate Function Level Reset */
#define PCIE_DEVSTA_PENDING     (1u << 5)  /* Transactions Pending */
#define PCIE_DEVCTL2_CT_DISABLE (1u << 4)  /* Completion Timeout Disable */
#define PCIE_LNKCAP_SPEED        0xFu       /* Max Link Speed: above 2, faster than 5.0 GT/s (7.5.3.6) */
#define PCIE_LNKCAP_DLL_REPORTING (1u << 20) /* Data Link Layer Link Active Reporting Capable */
#define PCIE_DEVCTL2_LTR         (1u << 10) /* LTR Mechanism Enable (7.5.3.16) */
#define PCIE_LNKCTL_DISABLE      (1u << 4)  /* Link Disable (7.5.3.7) */
#define PCIE_LNKCTL_RETRAIN      (1u << 5)  /* Retrain Link (7.5.3.7) */
#define PCIE_LNKCTL_CCC          (1u << 6)  /* Common Clock Configuration (7.5.3.7) */
#define PCIE_LNKSTA_SPEED        0xFu       /* Current Link Speed (7.5.3.8) */
#define PCIE_LNKSTA_WIDTH        (0x3Fu << 4) /* Negotiated Link Width */
#define PCIE_LNKSTA_TRAINING     (1u << 11) /* Link Training (7.5.3.8) */
#define PCIE_LNKSTA_DLL_ACTIVE   (1u << 13) /* Data Link Layer Link Active (7.5.3.8) */
#define PCIE_LNKCTL2_COMPLIANCE  (1u << 4)  /* Enter Compliance (7.5.3.19) */
#define PCIE_LNKCTL2_MOD_COMPLIANCE (1u << 10) /* Enter Modified Compliance (7.5.3.19) */
#define PCIE_ROOTCTL_RRS_VISIBLE (1u << 4) /* Configuration RRS Software Visibility Enable (7.5.3.12) */
#define PCIE_ROOTCAP_RRS_VISIBLE (1u << 0) /* Configuration RRS Software Visibility (7.5.3.13) */
/* After bus mastering is off a function starts no memory/I/O request, but some may still be in
   flight. Transactions Pending reports outstanding non-posted PCIe requests only. A host read
   flushes TC0 writes under the ordering rules; other TCs and relaxed/ID ordering require the
   driver's own synchronization (6.4, 2.4). This is not a universal DMA drain. Checks once, no
   wait. A conventional PCI master waits for no completion: its
   memory writes are posted, and the read that saw bus mastering off pulled them out of every
   bridge on the way (PCI Local Bus 3.0, 3.2.5); a read a bridge still holds for it is never
   repeated, and its data is discarded (3.3.3.3.3). So for conventional PCI the function only has
   to still be there. False for a function gone from the bus. */
bool pci_requests_done(const struct pci_device *d);
/* Function Level Reset (6.6.2), its configuration saved before and written back after (the MSI-X
   table's entries excepted: a driver's, which it set up for itself), with Bus Master Enable
   always cleared in the restored configuration. Following 6.6.2's algorithm: the Command
   register is cleared but for Interrupt Disable (set, it keeps INTx deasserted), Transactions
   Pending is polled until it clears or no Completion can still arrive (the longest Completion
   Timeout range the function has, no less than 1 s, as a driver may have disabled or changed
   it with Requests outstanding), and the FLR is issued either way, so no stale Completion is
   still to come after it. If the function cannot be stopped so, its Command is put back, bus
   mastering off. Configuration waits for the FLR to complete (100 ms, or a valid Readiness
   Time Reporting FLR Time); then the Vendor ID read may give RRS, or fail where the Root
   Complex stops retrying, until the reported FLR Time, or 1.0 s without one (the time 6.6.1
   and 5.10.1 give before a function may be declared broken; 6.6.2 itself sets no limit). False
   when it has no FLR, is gone, is still mastering afterwards, or is not configuration-ready
   yet. An unfinished reset keeps its saved configuration for a later call (d->reset);
   cancellation must not discard or recapture that state. A call cancelled in an FLR's waits
   goes on with that FLR; one that ends before its FLR, or finds the function not ready by the
   limit, leaves the next call to stop the function and issue another, once its Vendor ID read
   gives its ID (a write to a function answering RRS is retried by the Root Complex, 2.3.2).
   A sleep or a bus reset in between counts as no FLR (pci_resume, pci_bus_reset). A function
   without FLR whose configuration a bus reset left held gets it back once it answers, and the
   call is false. This resets device state, not an IOMMU drain confirmation: its success alone
   must not release quarantined DMA pages. May sleep, with cancellation during scheduler waits;
   any CPU, holding no spinlock. Resets (this and pci_bus_reset) run one at a time.
   The Root Port above shows RRS during each call and then gets its former Root Control back;
   without one that supports it (an RCiEP, an RCRB root) the Root Complex retries inside the
   configuration read, which no cancellation can interrupt. */
bool pci_function_reset(struct pci_device *d);
/* d's reset did not end because d did not come back: by the limit after its FLR, or at a later
   call, its Vendor ID read did not give its ID, or its requests were still pending; or it did
   not answer after a Conventional Reset (pci_bus_reset, sleep). Its Bus Master Enable is off,
   cleared before its FLR or by the reset. The kernel reaches it only through its reset (its ACS
   is not read either, dev/iommu.c); the firmware's AML not at all while it answers RRS
   (pci_config_firmware_*). What remains is a Conventional Reset (pci_bus_reset). */
bool pci_reset_stuck(const struct pci_device *d);
/* The Downstream Port whose Secondary Bus Reset resets d (pci_bus_reset), or NULL: d is not
   right below a Root Port or a Switch Downstream Port; that port's link leads to a hot-plug
   slot, whose link changes are events for the slot's software (6.7.3.3), the firmware's here;
   or d did not answer after the last Conventional Reset it had, after which software may take
   it as broken (6.6.1), and has not answered since. */
struct pci_device *pci_bus_reset_port(const struct pci_device *d);
/* A Secondary Bus Reset of port (pci_bus_reset_port): a hot reset, a Conventional Reset of every
   function below it (6.6.1, 7.5.1.3.13). The caller has all of them reserved: none another
   claim's or a kernel driver's, none the boot screen's. Each one's configuration is held first,
   its bus mastering stopped and kept off in it but for bridges (which forward the requests of
   what is below them). Secondary Bus Reset is held at least Trst (1 ms, PCI Local Bus 3.0,
   4.3.2) and, where the port reports it, until its link is down; the reset ends 2 ms after it
   is cleared (4.2.7.11). The functions are then read as 6.6.1 has it: 100 ms after the reset,
   or after their link trained above 5.0 GT/s, up to 1.0 s for their IDs. Each configuration is
   written back once its function answers, after the port's LTR Mechanism Enable, which its link
   going down cleared (7.5.3.16; closest to the Root Port first, 6.18). A function with an
   unfinished FLR is left to its next pci_function_reset: a Conventional Reset does not stop
   what an FLR stops (6.6.2). One that does not answer keeps its configuration held
   (pci_reset_stuck). Sleeps, not cancellable once the reset is sent: about 1.1 s at most for
   each link on the way. False when nothing was reset: a function below does not answer and has
   no configuration held, there is no memory to hold them, enumeration did not keep every
   function, a conventional PCI bus is below (its first access waits Trhfa, 2^25 of its clocks,
   which are not known here: 6.6.1, PCI Local Bus 3.0 4.3.2), or the port is in reset already or
   set to Enter Compliance (4.2.7.2.2). */
bool pci_bus_reset(struct pci_device *port);
/* A configuration access by software other than the function's reset (AML's PCI_Config regions,
   dev/acpi.c), from any CPU: false, and nothing reaches bus:dev.func, while it is kept from such
   software: during a reset attempt or a bus reset, up to the readiness limit of an FLR issued,
   and while it last answered RRS (until its Vendor ID read, repeated here, gives its ID). The
   check and the access are one step: a reset that begins on another CPU waits for an access
   under way. A read gives the dword at off & ~3 (all-ones when dropped); a write is bytes wide
   (1, 2 or 4) at off aligned to it. The first refusal during each reset is logged. */
bool pci_config_firmware_read(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t *v);
bool pci_config_firmware_write(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t v, unsigned bytes);
/* A driver's read of another function on its path (dev/pciuser.c): false while d has a reset
   record (d->reset: a reset under way, or one left to finish), checked and read in one step like
   the firmware's accesses. The dword at off & ~3. */
bool pci_config_read_unless_reset(const struct pci_device *d, uint16_t off, uint32_t *v);
/* Sleep (dev/pm.c): save every function's configuration before S3, write it back after (CPU 0,
   every other CPU parked, before the drivers resume). pci_suspend allocates and so runs with
   interrupts on; it returns NULL, or what could not be saved or why not (a reset under way: the
   sleep is then called off and pci_resume writes nothing).
   slept: the machine went to sleep, or a hibernation image was restored (machine_sleep).
   remake: the IOMMU's units did not come back (iommu_resume): each function's messages are made
   again from what they were given (iommu_msi_message), one that cannot be is left off, and a
   function in an ended claim's group keeps bus mastering off, as no unit drops its requests. */
const char *pci_suspend(void);
void pci_resume(bool slept, bool remake);
