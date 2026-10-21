#pragma once
/* IOMMU: AMD-Vi (dev/iommu_amd.c) or Intel VT-d (dev/iommu_intel.c), on the common part here.
   A function an app drives (dev/pciuser.c) gets a domain of its own while its claim lasts: its
   DMA reaches only the pages of the DMA objects made through the claim, mapped at their physical
   addresses (a bus address stays the physical one), and the ranges the firmware reserved for it;
   its interrupt messages reach only the vectors the claim was given. Every other function passes
   through untranslated, as on a machine without an IOMMU. When the claim ends the function is
   blocked: nothing it sends reaches memory or a CPU until it is claimed again. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct pci_device;
struct iommu_domain;

void iommu_init(void); /* after pci_init, before a driver enables an interrupt */
bool iommu_active(void);
/* A new domain for d with nothing of the app's mapped yet, attached to every requester ID its
   requests may carry. 0, or -N_ERR_BUSY when another function shares one of those IDs (behind a
   bridge to conventional PCI) or -N_ERR_NO_MEMORY / -N_ERR_UNSUPPORTED. */
int iommu_attach(struct pci_device *d, struct iommu_domain **out);
/* Nothing the function sends gets through any more; true once the units confirmed it. */
bool iommu_block(struct iommu_domain *dom);
void iommu_domain_free(struct iommu_domain *dom); /* blocked first when it was not */
/* Pages (frames, or count pages from base when frames is NULL) at their own addresses. unmap
   returns once no request in flight can reach them any more: false when the units did not
   confirm that. */
bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);
bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);

/* Interrupt messages (dev/pci.c). The address and data that make message `index` of d (the
   first of `count` for MSI, whose message n arrives as vector + n; an MSI-X entry with count 1)
   arrive as `vector` on the CPU with APIC id dest: the remappable form where the units remap
   that function's interrupts, the plain one otherwise. False when it cannot be delivered.
   clear: the function's messages lead nowhere any more (its MSI and MSI-X are off). */
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                       uint32_t *address, uint32_t *data);
void iommu_msi_clear(struct pci_device *d);
/* An IOAPIC redirection entry (arch/ioapic.c) for GSI gsi of the IOAPIC with this id, given in
   the plain format: what to write, remappable where interrupt remapping covers the IOAPICs. */
uint64_t iommu_ioapic_entry(uint8_t ioapic_id, uint32_t gsi, uint64_t rte);
/* Sleep (dev/pm.c): the units come back reset; resume programs them again from the tables in
   memory, before the IOAPICs and devices are. shutdown turns them off: a hibernation restore,
   whose image brings tables of its own. Interrupts off. */
void iommu_resume(void);
void iommu_shutdown(void);

/* ---- for the back ends ---- */
#define IOMMU_IDS 4 /* requester IDs per function: its own and those of bridges above it */
struct iommu_domain {
    struct iommu_domain *next; /* attached domains */
    struct pci_device *d;      /* NULL: a back end's own domain */
    uint16_t ids[IOMMU_IDS];
    unsigned nids;
    uint64_t root; /* top-level page table */
    unsigned levels;
    uint16_t id;
    /* the highest level that holds leaves: 1 for 4 KiB pages only, 2 or 3 adding 2 MiB or 1 GiB
       ones (a back end's own domain, filled once and never unmapped or freed) */
    unsigned leaf_levels;
    bool blocked;
    uint64_t irt; /* AMD-Vi: its interrupt remapping table (VT-d shares one among all functions) */
};
struct iommu_hw {
    const char *name;
    bool coherent;   /* the units snoop their table reads; else every written entry is flushed */
    unsigned levels; /* of a claimed function's page tables */
    uint16_t domains; /* domain ids 1 .. domains - 1; 1 is the back end's own domain */
    /* Entries, bit 0 present in both formats (AMD-Vi PR, VT-d read): one at `level` pointing at
       a table, one mapping phys */
    uint64_t (*table)(uint64_t phys, unsigned level);
    uint64_t (*leaf)(uint64_t phys, unsigned level);
    bool (*start)(void); /* tables made, units on; false leaves them off */
    /* The ID the firmware says requests with this one arrive as (AMD-Vi IVRS aliases), or NULL */
    uint16_t (*alias)(uint16_t id);
    /* attach returns false only with nothing changed for the function's requester IDs */
    bool (*attach)(struct iommu_domain *dom);
    bool (*block)(struct iommu_domain *dom);
    bool (*flush)(struct iommu_domain *dom, bool unmapped); /* after its page tables changed */
    bool (*msi)(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                uint32_t *address, uint32_t *data);
    void (*msi_clear)(struct pci_device *d);
    uint64_t (*ioapic)(uint8_t ioapic_id, uint32_t gsi, uint64_t rte);
    void (*resume)(void);
    void (*shutdown)(void);
};
/* The units the firmware describes (IVRS, DMAR), their registers mapped and read: the back end,
   or NULL. Nothing is written to them before start. */
const struct iommu_hw *iommu_amd_probe(void);
const struct iommu_hw *iommu_intel_probe(void);
/* Memory the firmware has the functions with requester IDs first..last reach by DMA as it is
   (AMD-Vi IVMD, VT-d RMRR): mapped in their domains when claimed. Page aligned, end exclusive. */
void iommu_reserved(uint16_t first, uint16_t last, uint64_t start, uint64_t end);
unsigned iommu_reserved_count(void);
/* The back end's own domain, id 1 (the VT-d identity map): filled with iommu_map before a unit
   uses it, which then flushes nothing. */
struct iommu_domain *iommu_domain_new(unsigned levels, unsigned leaf_levels);
struct iommu_domain *iommu_domain_of(const struct pci_device *d); /* attached, or NULL */
/* The requester IDs d's requests may arrive with (IOMMU_IDS at most): how many, 0 when more. */
unsigned iommu_requester_ids(const struct pci_device *d, uint16_t *ids);
/* Writes a table entry the units may read, flushing it from the caches when they do not snoop. */
void iommu_entry_write(volatile uint64_t *e, uint64_t v);
void iommu_cache_flush(const volatile void *p, size_t bytes);
/* Whether to log one more blocked request: a few a second, so a device that keeps sending
   them cannot drown the log. */
bool iommu_fault_report(void);
/* A TSC deadline `ms` from now and whether it passed: units are polled with interrupts off. */
uint64_t iommu_deadline(unsigned ms);
bool iommu_expired(uint64_t deadline);
/* The plain MSI address for a physical APIC id (bits 19:12, no redirection hint). */
static inline uint32_t iommu_msi_plain(uint32_t dest) { return 0xFEE00000u | dest << 12; }
