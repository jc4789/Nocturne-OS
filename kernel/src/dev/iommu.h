#pragma once
/* IOMMU: AMD-Vi (dev/iommu_amd.c) or Intel VT-d (dev/iommu_intel.c), on the common part here.
   A function an app drives (dev/pciuser.c) gets a domain of its own while its claim lasts: its
   DMA reaches only the pages of the DMA objects made through the claim, mapped at their physical
   addresses (a bus address stays the physical one), and the ranges the firmware reserved for it;
   its interrupt messages reach only the vectors the claim was given. The functions that could
   reach it, or that it could reach, without passing the units (its group: a shared requester ID,
   a device or switch without ACS between them) join the domain, and none of them may be the
   kernel's, claimed, or in another domain. Every other function passes through untranslated, as
   on a machine without an IOMMU. When the claim ends its group is blocked: nothing they send
   reaches memory or a CPU until one is claimed again. That holds for the other members too: the
   driver reached them through the claimed function's peer-to-peer requests, so they may hold
   addresses it gave them, and no kernel driver uses them meanwhile (none runs a group member).
   A block the units did not confirm keeps the group out of every claim until a later attempt
   confirms it. Claims require isolation unless the administrator explicitly selected
   appdrivers-unisolated at boot; that opt-in never overrides a previous domain's quarantine. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct pci_device;
struct iommu_domain;

void iommu_init(void); /* after pci_init, before a driver enables an interrupt */
bool iommu_active(void); /* the units translate DMA: claims can be isolated */
/* false: unconfirmed hardware state, or units that did not come back from a sleep, forbid
   sleep/image restoration */
bool iommu_power_ready(void);
/* A new domain for d with nothing of the app's mapped yet, attached to every requester ID of its
   group: 0 with *out set. Only with appdrivers-unisolated, 0 with *out NULL when no unit
   translates, no unit sees one of its requester IDs, its group holds a function the kernel
   drives or that sends as the platform, or the bus was not all enumerated. Without that explicit
   opt-in these are errors, not a downgrade. Never when the units block a function of its group (an earlier
   claim's): -N_ERR_UNSUPPORTED (a unit does not see an ID, the group is unknown) or -N_ERR_BUSY
   (the kernel's or the platform's function) then. -N_ERR_BUSY when a function of the group is
   claimed or still in a domain (also one whose block was not confirmed), or no domain id is
   free; -N_ERR_NO_MEMORY; -N_ERR_IO when the units did not confirm it; -N_ERR_NOT_READY with
   *resetting set when forming the group has to read a function being reset (pci_device.reset),
   which only its reset may access: the group is formed once that is done. ACS controls the
   attempt turned on are put back as they were when no domain comes of it. */
int iommu_attach(struct pci_device *d, struct iommu_domain **out, struct pci_device **resetting);
/* Whether d is in a domain: attached, or blocked without the units' confirmation. */
bool iommu_in_use(const struct pci_device *d);
/* Whether d is a member of dom (its claimed function or the rest of its group). */
bool iommu_member(const struct iommu_domain *dom, const struct pci_device *d);
/* Nothing the group sends gets through any more; true once the units confirmed it. */
bool iommu_block(struct iommu_domain *dom);
/* Blocked first when it was not; kept (and its functions with it) until a block is confirmed. */
void iommu_domain_free(struct iommu_domain *dom);
/* Pages (frames, or count pages from base when frames is NULL) at their own addresses. unmap
   returns once no request in flight can reach them any more: false when the units did not
   confirm that. */
bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);
bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);

/* Interrupt messages (dev/pci.c). The address and data that make message `index` of d (the
   first of `count` for MSI, whose message n arrives as vector + n; an MSI-X entry with count 1)
   arrive as `vector` on the CPU with APIC id dest: the remappable form where the units remap
   that function's interrupts, the plain one otherwise. False when it cannot be delivered.
   clear: the function's messages lead nowhere any more (its MSI and MSI-X are off); false when
   the units did not confirm that, so that its vectors may still be reached. */
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                       uint32_t *address, uint32_t *data);
bool iommu_msi_clear(struct pci_device *d);
/* An IOAPIC redirection entry (arch/ioapic.c) for GSI gsi of the IOAPIC with this MADT id, given
   in the plain format and made what to write: remappable where interrupt remapping covers the
   IOAPICs. False when it cannot be delivered (the pin then stays masked). */
bool iommu_ioapic_entry(uint8_t ioapic_id, uint32_t gsi, uint64_t *rte);
/* Sleep (dev/pm.c): the units come back reset; resume programs them again from the tables in
   memory, before the IOAPICs and devices are, and only after the machine really slept (an
   aborted sleep leaves them on). NULL once they are back. Units that do not come back are
   turned off where they confirm it, as iommu_init does with units that do not start: interrupt
   routes are then given in the plain format (iommu_msi_message, iommu_ioapic_entry), unless a
   unit must remap them, and then none is; no claim and no sleep follow until a restart. It
   returns that loss, for the user, and the routes given before it are to be made again (the
   IOAPICs' and the PCI functions' messages). shutdown turns them off: a hibernation restore,
   whose image brings tables of its own. Interrupts off. */
const char *iommu_resume(void);
bool iommu_shutdown(void); /* false: do not overwrite the hardware's live tables */
/* Why the units keep the CPUs out of x2APIC mode, or NULL: the firmware asks for it (VT-d
   X2APIC_OPT_OUT), or a unit remaps interrupts in xAPIC format only (VT-d without EIM, AMD-Vi
   without XTSup and GASup). For apic_init, before pci_init and iommu_init: read from the
   firmware's tables and the units' registers alone, nothing written. */
const char *iommu_x2apic_conflict(void);

/* ---- for the back ends ---- */
#define IOMMU_READ  1u
#define IOMMU_WRITE 2u
struct iommu_domain {
    struct iommu_domain *next;   /* attached, or blocked without confirmation (dev/iommu.c) */
    struct pci_device *d;        /* the claimed function; NULL: a back end's own domain */
    struct pci_device **members; /* d and the rest of its group */
    unsigned nmembers;
    uint16_t *ids; /* every requester ID the members' requests may arrive with */
    unsigned nids;
    uint64_t root; /* top-level page table */
    unsigned levels;
    uint16_t id;
    /* the highest level that holds leaves: 1 for 4 KiB pages only, 2 or 3 adding 2 MiB or 1 GiB
       ones (a back end's own domain, filled once and never unmapped or freed) */
    unsigned leaf_levels;
    bool written;  /* the back end changed the units' entries for its IDs: blocked before it is freed */
    bool blocked;  /* the units confirmed nothing of the group gets through */
    bool unwanted; /* freed while its block was unconfirmed: freed once a block is confirmed */
    uint64_t irt;  /* AMD-Vi: its interrupt remapping table (VT-d shares one among all functions) */
};
struct iommu_hw {
    const char *name;
    bool interrupt_remapping_required; /* stopped units cannot deliver plain interrupts (VT-d IRREQ) */
    bool coherent;   /* the units snoop their table reads; else every written entry is flushed */
    unsigned levels; /* of a claimed function's page tables */
    unsigned iova_bits; /* usable input address width shared by every translating unit */
    unsigned phys_bits; /* physical address width shared by every unit; table addresses too */
    uint16_t domains; /* domain ids 1 .. domains - 1; 1 is the back end's own domain */
    /* Entries, bit 0 present in both formats (AMD-Vi PR, VT-d read): one at `level` pointing at
       a table, one mapping phys with IOMMU_READ / IOMMU_WRITE access */
    uint64_t (*table)(uint64_t phys, unsigned level);
    uint64_t (*leaf)(uint64_t phys, unsigned level, unsigned access);
    bool (*is_leaf)(uint64_t entry, unsigned level); /* do not follow a large-page leaf as a table */
    /* Tables made, units on. False does not prove the units are off: the caller must confirm
       shutdown before treating hardware-owned tables/queues as stopped. translate false (noiommu, or the firmware's
       descriptions not all kept): only a unit that must remap interrupts is started, remapping
       them alone, and false when there is none. */
    bool (*start)(bool translate);
    bool translating; /* set by start: DMA passes the units' page tables, claims can be isolated */
    /* The ID the firmware says requests with this one arrive as (AMD-Vi IVRS aliases), or NULL */
    uint16_t (*alias)(uint16_t id);
    /* attach: true once the units confirmed the domain. dom->written says whether anything of the
       units changed for its requester IDs; after a false the common part then blocks it. */
    bool (*attach)(struct iommu_domain *dom);
    bool (*block)(struct iommu_domain *dom);
    bool (*flush)(struct iommu_domain *dom, bool unmapped); /* after its page tables changed */
    bool (*msi)(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                uint32_t *address, uint32_t *data);
    bool (*msi_clear)(struct pci_device *d);
    bool (*ioapic)(uint8_t ioapic_id, uint32_t gsi, uint64_t *rte);
    bool (*resume)(void);
    bool (*shutdown)(void);
};
/* The units the firmware describes (IVRS, DMAR), their registers mapped and read: the back end,
   or NULL. Nothing is written to them before start. On NULL, plain_safe is true only with no
   description or after ALL described units were identified and observed not to interfere with
   plain delivery or own live queues/tables. An incomplete description/read is not proof of off;
   mandatory interrupt remapping (VT-d IRREQ) is not plain-safe even with every enable clear. */
const struct iommu_hw *iommu_amd_probe(bool *plain_safe);
const struct iommu_hw *iommu_intel_probe(bool *plain_safe);
/* iommu_x2apic_conflict for the units each back end describes */
const char *iommu_amd_x2apic_conflict(void);
const char *iommu_intel_x2apic_conflict(void);
/* What the back ends find in the firmware's tables while they probe.
   reserved: memory the functions with requester IDs first..last reach by DMA as it is (AMD-Vi
   IVMD, VT-d RMRR) with IOMMU_READ / IOMMU_WRITE access, mapped in their domains when claimed.
   Page aligned, end exclusive. Access zero forbids mapping the range (AMD IVMD); overlapping
   restrictions intersect, and app DMA allocations cannot enlarge those permissions.
   cover: requests with IDs first..last pass a unit (IVHD device entries, DRHD scopes); a function
   whose requests no unit sees cannot be isolated.
   kernel_id: requests with this ID are the platform's (IOAPIC, HPET): never in a claim's domain. */
void iommu_reserved(uint16_t first, uint16_t last, uint64_t start, uint64_t end, unsigned access);
unsigned iommu_reserved_count(void);
void iommu_cover(uint16_t first, uint16_t last);
void iommu_kernel_id(uint16_t id);
/* The back end's own domain, id 1 (the VT-d identity map): filled with iommu_map before a unit
   uses it, which then flushes nothing. */
struct iommu_domain *iommu_domain_new(unsigned levels, unsigned leaf_levels);
struct iommu_domain *iommu_domain_of(const struct pci_device *d); /* the claim's attached domain, or NULL */
/* The requester IDs d's requests may arrive with, *n of them, in memory the caller frees with
   kfree; NULL without memory. */
uint16_t *iommu_requester_ids(const struct pci_device *d, unsigned *n);
/* Writes a table entry the units may read, flushing it from the caches when they do not snoop. */
void iommu_entry_write(volatile uint64_t *e, uint64_t v);
void iommu_cache_flush(const volatile void *p, size_t bytes);
/* Zeroed physically contiguous pages for hardware tables/queues. Addresses fit every unit's
   physical width and the page-table format; freed with pmm_free, one page at a time. */
uint64_t iommu_alloc_pages(size_t count);
/* Whether physical start..end (exclusive) may hold RAM the OS owns (the allocator's, the
   bootloader's, the modules'), where a structure the firmware left live may have been reused:
   true also when the memory map was not kept whole, which is no evidence that it does not. */
bool iommu_os_ram(uint64_t start, uint64_t end);
/* Exclusive physical ceiling for identity-mapped DMA; UINT64_MAX for an unisolated claim. */
uint64_t iommu_dma_limit(const struct iommu_domain *dom);
/* Whether to log one more blocked request: a few a second, so a device that keeps sending
   them cannot drown the log. */
bool iommu_fault_report(void);
/* A measured-TSC deadline `ms` from now: units are polled with interrupts off. An unknown
   TSC rate cannot prove elapsed time and expires immediately; unconfirmed hardware stays owned. */
uint64_t iommu_deadline(unsigned ms);
bool iommu_expired(uint64_t deadline);
/* The plain MSI address for a physical APIC id (bits 19:12, no redirection hint). */
static inline uint32_t iommu_msi_plain(uint32_t dest) { return 0xFEE00000u | dest << 12; }
