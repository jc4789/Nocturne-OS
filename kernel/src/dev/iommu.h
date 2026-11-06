#pragma once
/* IOMMU: AMD-Vi (dev/iommu_amd.c) or Intel VT-d (dev/iommu_intel.c), on the common part here.
   A function an app drives (dev/pciuser.c) gets a domain of its own while its claim lasts: its
   DMA reaches only the pages of the DMA objects made through the claim, mapped at their physical
   addresses (a bus address stays the physical one), and the ranges the firmware reserved for it;
   its interrupt messages reach only the vectors the claim was given. The functions that could
   reach it, or that it could reach, without passing the units (its group: a shared requester ID,
   a device or switch without ACS between them) join the domain, and none of them may be the
   kernel's unless it can be a guest (below), claimed, or in another domain. Every other function
   passes through untranslated, as on a machine without an IOMMU. When the claim ends its group is
   blocked: nothing they send reaches memory or a CPU until one is claimed again. That holds for the
   other members too: the driver reached them through the claimed function's peer-to-peer requests,
   so they may hold addresses it gave them, and no kernel driver uses them meanwhile (a guest stays
   stopped).
   A block the units did not confirm keeps the group out of every claim until a later attempt
   confirms it. Claims require isolation unless the administrator explicitly selected
   the boot setting N_BOOT_UNISOLATED_DRIVERS; that opt-in never overrides a previous domain's quarantine. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct pci_device;
struct iommu_domain;
/* What a unit looks a request up by: its requester ID, unique within the segment the unit serves. */
struct iommu_id {
    uint16_t segment, requester;
};

/* A function the kernel drives that may share a group with one an app claims (a graphics card's sound
   function with its display function): its driver lets go of the group's isolation changing and comes
   back into the claim's domain, so the claim is not refused for it.
   stop: before its group joins a domain or a claim of it ends: nothing it sends goes out any more, and
   its interrupts are off. It may sleep, and nothing it holds waits for the claim.
   start: once the claim is formed, dom being its domain, in which the function's memory is then to be
   mapped (iommu_map, at its own addresses), and whose interrupt messages are the claimed function's: it
   uses its INTx or none. NULL: the domain did not come about, and its group is as it was.
   A group whose claim ended stays blocked, so the function stays stopped until the next claim. */
struct iommu_guest {
    void (*stop)(void *ctx);
    void (*start)(struct iommu_domain *dom, void *ctx);
    void *ctx;
};

void iommu_init(void); /* after pci_init, before a driver enables an interrupt */
bool iommu_active(void); /* the units translate DMA: claims can be isolated */
/* false: unconfirmed hardware state, or units that did not come back from a sleep, forbid
   sleep/image restoration */
bool iommu_power_ready(void);
/* A new domain for d with nothing of the app's mapped yet, attached to every requester ID of its
   group: 0 with *out set. Only with N_BOOT_UNISOLATED_DRIVERS, 0 with *out NULL when no unit
   translates, no unit sees one of its requester IDs, its group holds a function the kernel
   drives (not a guest) or that sends as the platform, or the bus was not all enumerated. Without that explicit
   opt-in these are errors, not a downgrade. Never when the units block a function of its group (an earlier
   claim's): -N_ERR_UNSUPPORTED (a unit does not see an ID, the group is unknown) or -N_ERR_BUSY
   (the kernel's or the platform's function) then. -N_ERR_BUSY when a function of the group is
   claimed or still in a domain (also one whose block was not confirmed), or no domain id is
   free; -N_ERR_NO_MEMORY; -N_ERR_IO when the units did not confirm it; -N_ERR_NOT_READY with
   *resetting set when forming the group has to read a function being reset (pci_device.reset),
   which only its reset may access: the group is formed once that is done. ACS controls the
   attempt turned on are put back as they were when no domain comes of it. */
int iommu_attach(struct pci_device *d, struct iommu_domain **out, struct pci_device **resetting);
/* The guests among dom's members: stopped before a claim of them ends (iommu_attach stops them before
   the domain is attached); started with dom once the claim is formed (formed), or NULL. */
void iommu_guests_stop(const struct iommu_domain *dom);
void iommu_guests_start(struct iommu_domain *dom, bool formed);
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
   confirm that. Any CPU, in a thread that may sleep: the domain's tables change under a mutex
   of its own. attach, block and domain_free sleep too (dev/iommu.c's attach_mutex). */
bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);
bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);

/* Interrupt messages (dev/pci.c). The address and data that make message `index` of d (the
   first of `count` for MSI, whose message n arrives as the irq's vector + n, arch/irq.h; an MSI-X
   entry with count 1) arrive where irq is delivered (irq_target): through the units' remapping
   entries where they remap that function's interrupts, which the message keeps while it is
   written again (a move, a sleep), the plain form otherwise (an 8-bit destination, SDM 13.11.1).
   False when it cannot be delivered.
   clear: the function's messages lead nowhere any more (its MSI and MSI-X are off), its entries
   given back; false when the units did not confirm that, so that its vectors may still be reached.
   reach: the highest APIC ID a message of d can name (irq_request_msi's reach). */
struct irq;
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, const struct irq *irq,
                       uint32_t *address, uint32_t *data);
bool iommu_msi_clear(struct pci_device *d);
uint32_t iommu_msi_reach(const struct pci_device *d);
/* An IOAPIC redirection entry (arch/ioapic.c) for pin `pin` of the `pins` of the IOAPIC with this
   MADT id to the CPU with APIC ID dest, given in the plain format (an 8-bit destination there, 0
   above 255) and made what to write: remappable where interrupt remapping covers the IOAPICs.
   False when it cannot be delivered (the pin then stays masked). reach: the highest APIC ID its
   entries can name. */
bool iommu_ioapic_entry(uint8_t ioapic_id, unsigned pin, unsigned pins, uint32_t dest, uint64_t *rte);
uint32_t iommu_ioapic_reach(uint8_t ioapic_id);
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

/* ---- for the back ends ---- */
/* An interrupt remapping table of the back end's (one a VT-d segment shares, one an AMD-Vi
   requester ID set has): size entries (a power of two) of entry_bytes, zeroed, at phys, and which
   of them are given out (dev/iommu.c, under a lock of its own). make: false without memory. take: count (a power of two) entries aligned to count, the
   first of them, or -1 when no such run is free. */
struct iommu_irt {
    uint64_t phys;
    unsigned size, entry_bytes;
    uint64_t *used;
};
bool iommu_irt_make(struct iommu_irt *t, unsigned size, unsigned entry_bytes);
void iommu_irt_free(struct iommu_irt *t);
int iommu_irt_take(struct iommu_irt *t, unsigned count);
void iommu_irt_give(struct iommu_irt *t, unsigned first, unsigned count);
#define IOMMU_READ  1u
#define IOMMU_WRITE 2u
struct iommu_domain {
    struct iommu_domain *next;   /* attached, or blocked without confirmation (dev/iommu.c) */
    struct pci_device *d;        /* the claimed function; NULL: a back end's own domain */
    struct pci_device **members; /* d and the rest of its group */
    unsigned nmembers;
    struct iommu_id *ids; /* every requester ID the members' requests may arrive with */
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
    struct iommu_irt irt; /* AMD-Vi: the claim's own interrupt remapping table (VT-d shares one a segment) */
};
/* The calls below come from any CPU: attach and block with dev/iommu.c's attach_mutex held,
   flush with the domain's map mutex, ioapic under arch/ioapic.c's lock, msi and msi_clear from
   a driver's thread or the sleep path, start, resume and shutdown from the power paths. Each back
   end puts each unit's queue and registers under a spinlock of that unit's, taken after all of
   those and in its fault or event interrupt handler (on whichever CPU that is) too, and waits for
   the unit's completion of its invalidations outside it: asleep where the caller may sleep and the
   unit interrupts at completion, else spinning on the unit's status write. */
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
       shutdown before treating hardware-owned tables/queues as stopped. translate false (N_BOOT_IOMMU off, or the firmware's
       descriptions not all kept): only a unit that must remap interrupts is started, remapping
       them alone, and false when there is none. */
    bool (*start)(bool translate);
    bool translating; /* set by start: DMA passes the units' page tables, claims can be isolated */
    /* The ID the firmware says requests with this one arrive as (AMD-Vi IVRS aliases), or NULL */
    struct iommu_id (*alias)(struct iommu_id id);
    /* attach: true once the units confirmed the domain. dom->written says whether anything of the
       units changed for its requester IDs; after a false the common part then blocks it. */
    bool (*attach)(struct iommu_domain *dom);
    bool (*block)(struct iommu_domain *dom);
    bool (*flush)(struct iommu_domain *dom, bool unmapped); /* after its page tables changed */
    /* Interrupt remapping of messages, whose entries dev/iommu.c gives out of the back end's tables.
       msi_table: *t the table d's messages go through (made where d has none yet, AMD-Vi), NULL
       when they go plain; false when they cannot be delivered. msi_write: entries first .. first + count - 1 of t, message n of d to vector
       + n at dest; the address and data of message 0; false when the units did not confirm it.
       msi_erase: entries[0 .. count - 1] of t, d's, not present (IOMMU_NO_IRTE skipped), true once the
       units confirmed it. msi_reach: the
       highest APIC ID d's messages can name. */
    bool (*msi_table)(struct pci_device *d, struct iommu_irt **t);
    bool (*msi_write)(struct pci_device *d, struct iommu_irt *t, unsigned first, unsigned count, uint8_t vector,
                      uint32_t dest, uint32_t *address, uint32_t *data);
    bool (*msi_erase)(struct pci_device *d, struct iommu_irt *t, const uint32_t *entries, unsigned count);
    uint32_t (*msi_reach)(const struct pci_device *d);
    bool (*ioapic)(uint8_t ioapic_id, unsigned pin, unsigned pins, uint32_t dest, uint64_t *rte);
    uint32_t (*ioapic_reach)(uint8_t ioapic_id);
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
/* What the back ends find in the firmware's tables while they probe.
   reserved: memory the functions of segment with requester IDs first..last reach by DMA as it is (AMD-Vi
   IVMD, VT-d RMRR) with IOMMU_READ / IOMMU_WRITE access, mapped in their domains when claimed.
   Page aligned, end exclusive. Access zero forbids mapping the range (AMD IVMD); overlapping
   restrictions intersect, and app DMA allocations cannot enlarge those permissions.
   cover: requests of segment with IDs first..last pass a unit (IVHD device entries, DRHD scopes); a function
   whose requests no unit sees cannot be isolated.
   kernel_id: requests with this ID are the platform's (IOAPIC, HPET): never in a claim's domain. */
void iommu_reserved(uint16_t segment, uint16_t first, uint16_t last, uint64_t start, uint64_t end, unsigned access);
unsigned iommu_reserved_count(void);
void iommu_cover(uint16_t segment, uint16_t first, uint16_t last);
void iommu_kernel_id(struct iommu_id id);
/* The back end's own domain, id 1 (the VT-d identity map): filled with iommu_map before a unit
   uses it, which then flushes nothing. */
struct iommu_domain *iommu_domain_new(unsigned levels, unsigned leaf_levels);
struct iommu_domain *iommu_domain_of(const struct pci_device *d); /* the claim's attached domain, or NULL */
/* The requester IDs d's requests may arrive with, *n of them, in memory the caller frees with
   kfree; NULL without memory. */
struct iommu_id *iommu_requester_ids(const struct pci_device *d, unsigned *n);
/* Every requester ID of d passes a unit (iommu_cover): its interrupts can be remapped. False
   without memory too. */
bool iommu_seen(const struct pci_device *d);
/* Writes a table entry the units may read, flushing it from the caches when they do not snoop. */
void iommu_entry_write(volatile uint64_t *e, uint64_t v);
/* A 16-byte aligned 128-bit entry (an interrupt remapping entry) in one store a unit cannot see
   half done, as both formats ask of an entry in use (VT-d 5.1.4, AMD-Vi 2.3.2), then flushed the
   same way. */
void iommu_entry_write16(volatile uint64_t *e, uint64_t lo, uint64_t hi);
void iommu_cache_flush(const volatile void *p, size_t bytes);
/* Zeroed physically contiguous pages for hardware tables/queues. Addresses fit every unit's
   physical width and the page-table format; freed with pmm_free_contig. */
uint64_t iommu_alloc_pages(size_t count);
/* Whether physical start..end (exclusive) may hold RAM the OS owns (the allocator's, the
   bootloader's, the modules'), where a structure the firmware left live may have been reused. */
bool iommu_os_ram(uint64_t start, uint64_t end);
/* The bits an identity-mapped DMA address may have; 64 for an unisolated claim, 0 for none. */
unsigned iommu_dma_bits(const struct iommu_domain *dom);
/* Whether to log one more blocked request: a few a second, so a device that keeps sending
   them cannot drown the log. */
bool iommu_fault_report(void);
/* The plain MSI address for a physical APIC id (bits 19:12, no redirection hint; SDM 13.11.1). */
static inline uint32_t iommu_msi_plain(uint32_t dest) { return 0xFEE00000u | dest << 12; }
#define IOMMU_NO_IRTE UINT32_MAX

