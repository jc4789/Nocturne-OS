/* IOMMU, the part AMD-Vi and VT-d share: domains and their page tables (512 entries a level, 9
   bits of the address each, in both formats), the requester IDs a function's requests carry, the
   group of functions a claim isolates together (ACS on the way, PCI Express Base specification
   6.12 and 7.7.7), the firmware's descriptions the back ends found, and the calls the rest of
   the kernel makes, passed to the back end that found its units. The address and ID rules are
   the PCI Express Base specification's (requester IDs, ACS) and the bridge specifications'.
   Any CPU calls in. Domains come and go one at a time under attach_mutex (attach, block, free,
   the retried blocks), which comes after dev/pciuser.c's claim_mutex and before the back end's
   lock; the lists of them are under domains_lock, taken alone; a domain's page tables change
   under its own mutex (iommu_map, iommu_unmap), before the back end's lock. The power calls
   (iommu_init on CPU 0 at boot, iommu_resume and iommu_shutdown with every other CPU stopped)
   change hw, restore_failed and lost_in_sleep, which the others only read. */
#include "kernel.h"
#include "abi.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/ioapic.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "fs/bootrecord.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define PTE_ADDR 0x000FFFFFFFFFF000ull

static const struct iommu_hw *hw;
static bool restore_failed; /* never downgrade an active domain after an unconfirmed power transition */
/* the units did not come back from a sleep (iommu_resume): no claim and no other sleep until a
   restart, however far iommu_init's way down took them */
static bool lost_in_sleep;
/* attached: the claims' domains; held: blocked without the units' confirmation (iommu_block).
   Both lists and domain_used are under domains_lock; they change with attach_mutex held too. */
static struct iommu_domain *attached, *held;
static uint8_t domain_used[65536 / 8];
static struct spinlock domains_lock;
/* attach, block and free, and each domain's written, blocked and unwanted and its members'
   iommu_blocked: the back end's attach and block wait for the units, so this sleeps. */
static struct mutex attach_mutex;

/* A domain as this part allocates it: its page tables change under map (iommu_map, iommu_unmap),
   which may allocate a table or wait for the units' invalidation. */
struct domain_rec {
    struct iommu_domain dom; /* first: the back ends see this */
    struct mutex map;
};
static struct mutex *map_lock(struct iommu_domain *dom) { return &((struct domain_rec *)dom)->map; }

/* ---- what the back ends found in the firmware's tables ---- */
static struct reserved {
    uint16_t segment, first, last;
    uint64_t start, end;
    unsigned access;
} *reserved;
static size_t nreserved, reserved_room;
static struct span {
    uint16_t segment, first, last;
} *covered;
static size_t ncovered, covered_room;
static struct iommu_id *kernel_ids;
static size_t nkernel_ids, kernel_ids_room;
static bool described_lost; /* no memory kept one of them: the units are not used */

static void lost(const char *what) {
    if (!described_lost) kprintf("iommu: no memory to keep the firmware's %s\n", what);
    described_lost = true;
}

static bool id_same(struct iommu_id a, struct iommu_id b) { return a.segment == b.segment && a.requester == b.requester; }

void iommu_reserved(uint16_t segment, uint16_t first, uint16_t last, uint64_t start, uint64_t end, unsigned access) {
    access &= IOMMU_READ | IOMMU_WRITE;
    if (first > last || start >= end) return;
    for (size_t i = 0; i < nreserved; i++) /* firmware repeats them (one per IVHD type, or scope) */
        if (reserved[i].segment == segment && reserved[i].first == first && reserved[i].last == last &&
            reserved[i].start == start && reserved[i].end == end) {
            reserved[i].access &= access;
            return;
        }
    struct reserved *r = kappend(&reserved, &nreserved, &reserved_room, sizeof *r);
    if (r) *r = (struct reserved){segment, first, last, start, end, access};
    else lost("reserved memory ranges");
}

unsigned iommu_reserved_count(void) { return (unsigned)nreserved; }

void iommu_cover(uint16_t segment, uint16_t first, uint16_t last) {
    if (first > last) return;
    struct span *s = kappend(&covered, &ncovered, &covered_room, sizeof *s);
    if (s) *s = (struct span){segment, first, last};
    else lost("device scopes");
}

static bool is_covered(struct iommu_id id) {
    for (size_t i = 0; i < ncovered; i++)
        if (id.segment == covered[i].segment && id.requester >= covered[i].first && id.requester <= covered[i].last)
            return true;
    return false;
}

void iommu_kernel_id(struct iommu_id id) {
    for (size_t i = 0; i < nkernel_ids; i++)
        if (id_same(kernel_ids[i], id)) return;
    struct iommu_id *at = kappend(&kernel_ids, &nkernel_ids, &kernel_ids_room, sizeof *at);
    if (at) *at = id;
    else lost("IOAPIC and HPET IDs");
}

static bool is_kernel_id(struct iommu_id id) {
    for (size_t i = 0; i < nkernel_ids; i++)
        if (id_same(kernel_ids[i], id)) return true;
    return false;
}

/* Units that did not start (iommu_init) or come back from a sleep (iommu_resume), with the
   IOAPICs masked: off where they confirm it. Interrupts then go in the plain format
   unless a unit must remap them (VT-d ECAP.IRREQ: every interrupt is blocked while it is off,
   5.1.4); else no interrupt route is given out (restore_failed). True: plain. */
static bool units_abandon(void) {
    /* false may leave a live firmware queue or an unconfirmed command, not merely an unused
       unit. Do not later approve overwriting its tables during hibernate restore. */
    bool plain = hw->shutdown() && !hw->interrupt_remapping_required;
    if (!plain) restore_failed = true;
    hw = NULL;
    return plain;
}

/* Units are found also with the boot setting off, or when the firmware's descriptions could not be
   kept: the boot setting is DMA translation's, and units that remap interrupts apart from it
   (VT-d) are started remapping them alone, which keeps a device from sending any vector, lets a
   unit that requires remapping (ECAP.IRREQ, 5.1.4) deliver at all, and is what x2APIC mode waits
   for on Intel's CPUs (SDM 13.12.7, arch/apic.h). */
void iommu_init(void) {
    const char *off = !boot_setting(N_BOOT_IOMMU) ? "off (the boot record says so)" : NULL; /* why DMA is not translated */
    bool amd_plain = false, intel_plain = false;
    const struct iommu_hw *found = iommu_amd_probe(&amd_plain);
    if (!found) found = iommu_intel_probe(&intel_plain);
    if (!found) {
        /* Parsing support and hardware state are separate. Rejecting an isolation table
           must not disable plain interrupts when every unit is independently confirmed off.
           Conversely, a malformed/unreadable description cannot establish that fact. */
        if (!amd_plain || !intel_plain) restore_failed = true;
        if (restore_failed)
            kprintf("iommu: firmware description unusable; hardware stop unconfirmed; app claims and new interrupt routes disabled\n");
        else kprintf("iommu: %s: app drivers run without isolation only by the boot setting\n",
                     off ? off : acpi_find("IVRS", 0) || acpi_find("DMAR", 0)
                         ? "not used (rejected tables, all units confirmed off)"
                         : "none described (no IVRS or DMAR)");
        apic_x2_follow("no IOMMU remaps interrupts");
        return;
    }
    if (!off && described_lost) off = "not used (the firmware's descriptions were not all kept)";
    hw = found; /* table writes need it from here on */
    domain_used[0] |= 3; /* 0 is never used, 1 is the back end's */
    /* No IOAPIC pin delivers while the units and the entries change format: masked, their routes
       kept and written again in the new format (ioapic_remap). This boot task's interrupts off
       meanwhile; the devices' messages wait for their drivers, which come after. */
    uint64_t irq = irq_save();
    ioapic_mask_all();
    bool started = hw->start(!off);
    /* x2APIC mode after the remapping with 32-bit destinations it waits for (SDM 13.12.7), before
       an IOAPIC pin or a driver's message goes through the units */
    apic_x2_follow(!started             ? "the IOMMU did not start"
                   : !hw->x2apic_ready ? "the IOMMU does not remap interrupts with 32-bit destinations"
                                       : NULL);
    if (started) started = ioapic_remap();
    if (!started) {
        /* No table remains active once they are off: the original plain requests go back,
           including a route whose remapping failed. Never rewrite routes under uncertain units. */
        if (!units_abandon()) ioapic_mask_all();
        else if (!ioapic_remap()) restore_failed = true;
        if (restore_failed)
            kprintf("iommu: initialization/interrupt delivery unconfirmed; app claims and new interrupt routes disabled\n");
        else kprintf("iommu: %s: app drivers run without isolation only by the boot setting\n", off ? off : "not used");
    } else if (!hw->translating) {
        kprintf("iommu: %s remaps interrupts only: app drivers need the boot setting to run without isolation\n", hw->name);
    }
    irq_restore(irq);
}

bool iommu_active(void) { return hw && hw->translating && !restore_failed; }
bool iommu_power_ready(void) { return !restore_failed && !lost_in_sleep; }

/* The back ends' fault handlers and their waits (any CPU) count against one second's
   limit, under report_lock, taken last. */
bool iommu_fault_report(void) {
    static struct spinlock report_lock;
    static uint64_t second;
    static unsigned shown;
    uint64_t now = clock_now_ns() / NS_PER_S;
    uint64_t f = spin_lock(&report_lock);
    if (now != second) {
        second = now;
        shown = 0;
    }
    bool more = shown == 8, show = shown < 8;
    if (shown <= 8) shown++;
    spin_unlock(&report_lock, f);
    if (more) kprintf("iommu: more blocked requests this second are not shown\n");
    return show;
}

void iommu_cache_flush(const volatile void *p, size_t bytes) {
    unsigned line = cpu_features.clflush_bytes;
    uintptr_t at = (uintptr_t)p / line * line, end = (uintptr_t)p + bytes;
    for (; at < end; at += line) __asm__ volatile("clflush (%0)" ::"r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

void iommu_entry_write(volatile uint64_t *e, uint64_t v) {
    *e = v;
    if (!hw->coherent) iommu_cache_flush(e, sizeof *e);
}

void iommu_entry_write16(volatile uint64_t *e, uint64_t lo, uint64_t hi) {
    ASSERT(!((uintptr_t)e & 15));
    uint64_t old_lo = e[0], old_hi = e[1];
    bool stored;
    do __asm__ volatile("lock cmpxchg16b %1"
                        : "=@ccz"(stored), "+m"(*(volatile unsigned __int128 *)e), "+a"(old_lo), "+d"(old_hi)
                        : "b"(lo), "c"(hi)
                        : "memory");
    while (!stored);
    if (!hw->coherent) iommu_cache_flush(e, 16);
}

/* ---- interrupt remapping tables ---- */
/* The tables' given-out maps and the functions' entries (irte, irt): a leaf, never held over a
   back end's call. */
static struct spinlock irt_lock;

bool iommu_irt_make(struct iommu_irt *t, unsigned size, unsigned entry_bytes) {
    ASSERT(size && !(size & (size - 1)));
    size_t bytes = (size_t)size * entry_bytes;
    uint64_t *used = kzalloc(ALIGN_UP(size, 64) / 8);
    uint64_t phys = used ? iommu_alloc_pages(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE) : 0;
    if (!phys) {
        kfree(used);
        return false;
    }
    if (!hw->coherent) iommu_cache_flush(phys_to_virt(phys), bytes);
    *t = (struct iommu_irt){.phys = phys, .size = size, .entry_bytes = entry_bytes, .used = used};
    return true;
}

void iommu_irt_free(struct iommu_irt *t) {
    pmm_free_contig(t->phys, ALIGN_UP((size_t)t->size * t->entry_bytes, PAGE_SIZE) / PAGE_SIZE);
    kfree(t->used);
    *t = (struct iommu_irt){0};
}

static bool irt_run_free(const struct iommu_irt *t, unsigned first, unsigned count) {
    for (unsigned i = first; i < first + count; i++)
        if (t->used[i / 64] >> (i % 64) & 1) return false;
    return true;
}

int iommu_irt_take(struct iommu_irt *t, unsigned count) {
    ASSERT(count && !(count & (count - 1)) && count <= t->size);
    uint64_t f = spin_lock(&irt_lock);
    int found = -1;
    for (unsigned first = 0; first < t->size && found < 0; first += count) {
        if (count < 64 && t->used[first / 64] == UINT64_MAX) {
            first = (first | 63) + 1 - count; /* a full word: on to the next */
            continue;
        }
        if (!irt_run_free(t, first, count)) continue;
        for (unsigned i = first; i < first + count; i++) t->used[i / 64] |= 1ull << (i % 64);
        found = (int)first;
    }
    spin_unlock(&irt_lock, f);
    return found;
}

void iommu_irt_give(struct iommu_irt *t, unsigned first, unsigned count) {
    uint64_t f = spin_lock(&irt_lock);
    for (unsigned i = first; i < first + count; i++) t->used[i / 64] &= ~(1ull << (i % 64));
    spin_unlock(&irt_lock, f);
}

/* d's entries room for index + count messages, IOMMU_NO_IRTE where none. irt_lock held. */
static bool messages_room(struct pci_device *d, unsigned index, unsigned count) {
    if (index + count <= d->irte_room) return true;
    unsigned room = MAX(index + count, MAX(32u, (unsigned)d->msix_entries));
    uint32_t *grown = kmalloc(room * sizeof *grown);
    if (!grown) return false;
    for (unsigned i = 0; i < room; i++) grown[i] = i < d->irte_room ? d->irte[i] : IOMMU_NO_IRTE;
    kfree(d->irte);
    d->irte = grown;
    d->irte_room = room;
    return true;
}

/* The entries of d's messages index .. index + count - 1 in t: those it has (a message written
   again), else count taken for them. -1 when t has no run free, d's are in another table, or
   without memory. */
static int messages_entries(struct pci_device *d, struct iommu_irt *t, unsigned index, unsigned count) {
    uint64_t f = spin_lock(&irt_lock);
    bool ok = (!d->irt || d->irt == t) && messages_room(d, index, count);
    uint32_t first = ok ? d->irte[index] : IOMMU_NO_IRTE;
    spin_unlock(&irt_lock, f);
    if (!ok) return -1;
    if (first != IOMMU_NO_IRTE) return (int)first;
    int taken = iommu_irt_take(t, count);
    if (taken < 0) {
        kprintf("iommu: %04x:%02x:%02x.%x: no interrupt remapping entries left for %u message%s\n",
                PCI_ADDRESS_ARGS(d->address), count, count == 1 ? "" : "s");
        return -1;
    }
    f = spin_lock(&irt_lock);
    d->irt = t;
    for (unsigned i = 0; i < count; i++) d->irte[index + i] = (uint32_t)taken + i;
    spin_unlock(&irt_lock, f);
    return taken;
}

/* d's entries given back to their table and forgotten. */
static void messages_forget(struct pci_device *d) {
    uint64_t f = spin_lock(&irt_lock);
    for (unsigned i = 0; i < d->irte_room; i++)
        if (d->irte[i] != IOMMU_NO_IRTE) d->irt->used[d->irte[i] / 64] &= ~(1ull << (d->irte[i] % 64));
    uint32_t *gone = d->irte;
    d->irte = NULL;
    d->irte_room = 0;
    d->irt = NULL;
    spin_unlock(&irt_lock, f);
    kfree(gone);
}

/* ---- page tables ---- */
static unsigned shift_of(unsigned level) { return 12 + 9 * (level - 1); }

/* Reject high bits before indexing or encoding a PTE. Masking an address would instead map a
   different page, and a wrapped range could silently modify unrelated mappings. */
static bool range_fits(uint64_t base, size_t count, unsigned bits) {
    if (!bits || bits > 64 || (base & (PAGE_SIZE - 1))) return false;
    uint64_t last = bits == 64 ? UINT64_MAX : (1ull << bits) - 1;
    if (last < PAGE_SIZE - 1) return false;
    last -= PAGE_SIZE - 1;
    return base <= last && (!count || count - 1 <= (last - base) / PAGE_SIZE);
}

uint64_t iommu_alloc_pages(size_t count) {
    if (!count || count > SIZE_MAX / PAGE_SIZE) return 0;
    unsigned bits = hw->phys_bits;
    /* Both back ends encode table addresses in bits 51:12. */
    if (bits > 52) bits = 52;
    if (!bits) return 0;
    uint64_t base = pmm_alloc_contig(count, bits);
    if (base) memset(phys_to_virt(base), 0, count * PAGE_SIZE);
    return base;
}

unsigned iommu_dma_bits(const struct iommu_domain *dom) {
    if (!dom) return 64;
    unsigned bits = MIN(hw->iova_bits, hw->phys_bits);
    unsigned indexed = shift_of(dom->levels) + 9;
    if (bits > indexed) bits = indexed;
    if (bits > 52) bits = 52; /* the leaf's physical address field */
    return bits;
}

static bool pages_fit(const struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    unsigned bits = iommu_dma_bits(dom);
    if (!bits || count > SIZE_MAX / PAGE_SIZE) return false;
    if (!frames) return range_fits(base, count, bits);
    for (size_t i = 0; i < count; i++)
        if (!range_fits(frames[i], 1, bits)) return false;
    return true;
}

static uint64_t table_new(void) {
    uint64_t t = iommu_alloc_pages(1);
    if (t && !hw->coherent) iommu_cache_flush(phys_to_virt(t), PAGE_SIZE);
    return t;
}

static void tables_free(uint64_t table, unsigned level) {
    const uint64_t *e = phys_to_virt(table);
    for (unsigned i = 0; level > 1 && i < 512; i++)
        if ((e[i] & 1) && !hw->is_leaf(e[i], level)) tables_free(e[i] & PTE_ADDR, level - 1);
    pmm_free(table);
}

/* The entry for iova at `level`, the tables above it made when create. */
static volatile uint64_t *entry(struct iommu_domain *dom, uint64_t iova, unsigned level, bool create) {
    uint64_t table = dom->root;
    for (unsigned l = dom->levels;; l--) {
        volatile uint64_t *e = (volatile uint64_t *)phys_to_virt(table) + (iova >> shift_of(l) & 511);
        if (l == level) return e;
        if (!(*e & 1)) {
            uint64_t t = create ? table_new() : 0;
            if (!t) return NULL;
            iommu_entry_write(e, hw->table(t, l));
        } else if (dom->leaf_levels > 1 && hw->is_leaf(*e, l)) return NULL;
        table = *e & PTE_ADDR;
    }
}

/* The entries only: a domain no unit uses yet needs no flush. */
static bool map_range(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count,
                      unsigned access) {
    if (!pages_fit(dom, frames, base, count)) return false;
    for (size_t i = 0; i < count;) {
        uint64_t pa = frames ? frames[i] : base + i * PAGE_SIZE;
        unsigned level = 1;
        while (!frames && level < dom->leaf_levels) {
            uint64_t span = 1ull << shift_of(level + 1);
            if ((pa & (span - 1)) || (count - i) * PAGE_SIZE < span) break;
            level++;
        }
        volatile uint64_t *e = entry(dom, pa, level, true);
        if (!e) return false;
        iommu_entry_write(e, hw->leaf(pa, level, access));
        i += 1ull << (shift_of(level) - 12);
    }
    return true;
}

static bool reserved_for(const struct reserved *r, const struct iommu_domain *dom);

/* Firmware restrictions are maximum access, not permission to allocate such a page to a driver
   and overwrite its protected PTE. All DMA objects are read/write; restricted pages cannot be
   used for them. The allocator normally excludes firmware-owned RAM independently. */
static bool dma_pages_allowed(const struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    if (!dom->d) return true; /* the kernel's identity domain, not an app allocation */
    if (!pages_fit(dom, frames, base, count)) return false;
    for (size_t r = 0; r < nreserved; r++) {
        const struct reserved *s = &reserved[r];
        if (!reserved_for(s, dom)) continue;
        if (!frames) {
            if (count && base < s->end && s->start < base + count * PAGE_SIZE) return false;
        } else for (size_t i = 0; i < count; i++)
            if (frames[i] < s->end && s->start < frames[i] + PAGE_SIZE) return false;
    }
    return true;
}

bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    if (!dma_pages_allowed(dom, frames, base, count)) return false;
    mutex_lock(map_lock(dom));
    bool ok = map_range(dom, frames, base, count, IOMMU_READ | IOMMU_WRITE) && (!dom->d || hw->flush(dom, false));
    mutex_unlock(map_lock(dom));
    return ok;
}

static bool unmap_range(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count);

bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    ASSERT(dom->leaf_levels == 1);
    /* map_range preflights the complete input before changing any PTE. Its rejected input must
       not be truncated here either, but it has no mapping to invalidate or DMA pages to pin. */
    if (!pages_fit(dom, frames, base, count) || !dma_pages_allowed(dom, frames, base, count)) return true;
    mutex_lock(map_lock(dom));
    bool ok = unmap_range(dom, frames, base, count);
    mutex_unlock(map_lock(dom));
    return ok;
}

/* map_lock held. */
static bool unmap_range(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t pa = frames ? frames[i] : base + i * PAGE_SIZE;
        volatile uint64_t *e = entry(dom, pa, 1, false);
        /* A DMA object's lifetime does not end the firmware's reservation. */
        unsigned access = IOMMU_READ | IOMMU_WRITE;
        bool present = false;
        for (size_t r = 0; r < nreserved; r++)
            if (reserved[r].start <= pa && pa < reserved[r].end && reserved_for(&reserved[r], dom)) {
                access &= reserved[r].access;
                present = true;
            }
        if (e && *e) iommu_entry_write(e, present && access ? hw->leaf(pa, 1, access) : 0);
    }
    return hw->flush(dom, true);
}

static struct iommu_domain *domain_alloc(uint16_t id, unsigned levels, unsigned leaf_levels) {
    struct domain_rec *rec = kzalloc(sizeof *rec);
    struct iommu_domain *dom = rec ? &rec->dom : NULL;
    if (dom && !(dom->root = table_new())) {
        kfree(rec);
        dom = NULL;
    }
    if (!dom) return NULL;
    dom->id = id;
    dom->levels = levels;
    dom->leaf_levels = leaf_levels;
    return dom;
}

struct iommu_domain *iommu_domain_new(unsigned levels, unsigned leaf_levels) {
    return domain_alloc(1, levels, leaf_levels);
}

/* The domain stays while its claim does: the callers ask for a claim's own function. */
struct iommu_domain *iommu_domain_of(const struct pci_device *d) {
    uint64_t f = spin_lock(&domains_lock);
    struct iommu_domain *found = NULL;
    for (struct iommu_domain *dom = attached; dom && !found; dom = dom->next)
        if (dom->d == d) found = dom;
    spin_unlock(&domains_lock, f);
    return found;
}

bool iommu_member(const struct iommu_domain *dom, const struct pci_device *d) {
    for (unsigned i = 0; i < dom->nmembers; i++)
        if (dom->members[i] == d) return true;
    return false;
}

bool iommu_in_use(const struct pci_device *d) {
    uint64_t f = spin_lock(&domains_lock);
    bool in = false;
    for (struct iommu_domain *dom = attached; dom && !in; dom = dom->next) in = iommu_member(dom, d);
    for (struct iommu_domain *dom = held; dom && !in; dom = dom->next) in = iommu_member(dom, d);
    spin_unlock(&domains_lock, f);
    return in;
}

/* ---- requester IDs ---- */
struct ids {
    struct iommu_id *at;
    size_t n, room;
};

static struct iommu_id id_of(const struct pci_device *d) {
    return (struct iommu_id){d->address.segment, pci_requester_id(d->address)};
}

static bool id_has(const struct ids *l, struct iommu_id id) {
    for (size_t i = 0; i < l->n; i++)
        if (id_same(l->at[i], id)) return true;
    return false;
}

static bool id_add(struct ids *l, struct iommu_id id) {
    if (id_has(l, id)) return true;
    struct iommu_id *at = kappend(&l->at, &l->n, &l->room, sizeof *at);
    if (at) *at = id;
    return at != NULL;
}

/* Adds the IDs d's requests may arrive with: its own, and for each bridge to conventional PCI or
   PCI-X above it, which issues the requests from below as its own, the bridge's ID and its
   secondary bus with device and function 0; then, when aliases, the firmware's aliases of
   those. False without memory. */
static bool ids_of(const struct pci_device *d, struct ids *l, bool aliases) {
    size_t from = l->n;
    if (!id_add(l, id_of(d))) return false;
    for (const struct pci_device *b = d->upstream; b; b = b->upstream) {
        if (b->pcie_cap && b->pcie_type != 7) continue; /* 7: PCI Express to PCI/PCI-X bridge */
        if (!id_add(l, (struct iommu_id){d->address.segment, (uint16_t)(b->secondary << 8)}) || !id_add(l, id_of(b)))
            return false;
    }
    for (size_t i = from, own = l->n; aliases && hw->alias && i < own; i++)
        if (!id_add(l, hw->alias(l->at[i]))) return false;
    return true;
}

struct iommu_id *iommu_requester_ids(const struct pci_device *d, unsigned *n) {
    struct ids l = {0};
    if (!ids_of(d, &l, true)) {
        kfree(l.at);
        return NULL;
    }
    *n = (unsigned)l.n;
    return l.at;
}

bool iommu_seen(const struct pci_device *d) {
    unsigned n;
    struct iommu_id *ids = iommu_requester_ids(d, &n);
    bool seen = ids != NULL;
    for (unsigned i = 0; seen && i < n; i++) seen = is_covered(ids[i]);
    kfree(ids);
    return seen;
}

/* ---- groups ----
   The functions a claimed one reaches, or that reach it, without their requests passing the
   units: those sending with one of its requester IDs; the other functions of its device unless
   its ACS sends what it addresses to them upstream; below a switch whose downstream port above
   it does not redirect every request upstream, everything below that switch; and everything
   when a root port above it has no ACS, or its ACS does not turn on. Such a root port validates
   no requester ID (Source Validation is ACS's, 6.12.1.1), so what is below it may send with any
   function's ID, and whether it passes requests to the other root ports is the root complex's
   own choice (1.3.1). A function integrated in the root complex has no port: its requests reach
   the units with the ID the root complex gives them, those to its peers too (VT-d 3.17; AMD-Vi
   2.1, the IOMMU sees every request of the devices it serves), except that its device's other
   functions may be reached inside the device, ACS or not (where an integrated function's ACS
   redirects to is the root complex's choice, 6.12.1.2). The group closes over its members: what
   a member reaches, it reaches too.
   An attempt changes ACS controls on the way: written back when no domain comes of it (a
   refused or unisolated claim leaves the bus as it was), kept while the domain lives and after
   (its group stays blocked, dev/iommu.h). */
#define ACS_V (1u << 0) /* source validation */
#define ACS_B (1u << 1) /* translation blocking */
#define ACS_R (1u << 2) /* P2P request redirect */
#define ACS_C (1u << 3) /* P2P completion redirect */
#define ACS_U (1u << 4) /* upstream forwarding */
#define ACS_E (1u << 5) /* P2P egress control */
#define ACS_T (1u << 6) /* direct translated P2P */
#define ACS_PORT (ACS_V | ACS_R | ACS_C | ACS_U) /* what a switch downstream port with ACS has */

struct fns {
    struct pci_device **at;
    size_t n, room;
};

static bool fn_has(const struct fns *g, const struct pci_device *o) {
    for (size_t i = 0; i < g->n; i++)
        if (g->at[i] == o) return true;
    return false;
}

static bool fn_add(struct fns *g, struct pci_device *o) {
    if (fn_has(g, o)) return true;
    struct pci_device **at = kappend(&g->at, &g->n, &g->room, sizeof *at);
    if (at) *at = o;
    return at != NULL;
}

/* Bridges send no requests of their own that matter, unless a kernel driver runs them. */
static bool sends(const struct pci_device *o) { return o->cls != 0x06 || o->kernel_owned; }

/* ACS Control registers an attempt changed, as they were before */
struct acs_change {
    const struct pci_device *f;
    uint16_t at, ctl;
};
struct acs_log {
    struct acs_change *at;
    size_t n, room;
    struct pci_device *resetting; /* the function under reset that stopped the attempt, or NULL */
};

static void acs_undo(const struct acs_log *log) {
    for (size_t i = log->n; i--;)
        pci_write16(log->at[i].f->address, log->at[i].at, log->at[i].ctl);
}

/* Turns on the ACS controls of `want` that f has, and off egress control and direct translated
   P2P (both let requests past the redirect), noting what the register was in log: 1; 0 without
   ACS; -N_ERR_NO_MEMORY without memory to note it, nothing written; -N_ERR_NOT_READY, f noted,
   while f is being reset, as no other configuration access may reach it (dev/pci.h). A function
   that did not come back from its reset is not read either, and is taken as without ACS: its
   ACS is not known. *has: the controls of `want` it has; *set: those of them that read back on. */
static int acs_set(struct acs_log *log, struct pci_device *f, uint16_t want, uint16_t *has, uint16_t *set) {
    *has = *set = 0;
    if (pci_reset_stuck(f)) {
        kprintf("iommu: %04x:%02x:%02x.%x does not answer after its reset: taken as without ACS\n", PCI_ADDRESS_ARGS(f->address));
        return 0;
    }
    if (f->reset) {
        kprintf("iommu: %04x:%02x:%02x.%x is being reset: its group is formed once it is ready\n", PCI_ADDRESS_ARGS(f->address));
        log->resetting = f;
        return -N_ERR_NOT_READY;
    }
    uint16_t at = pci_find_ext_cap(f, 0x0D);
    if (!at) return 0;
    *has = pci_read16(f->address, at + 4) & want;
    uint16_t ctl = pci_read16(f->address, at + 6);
    uint16_t now = (uint16_t)((ctl | *has) & ~(ACS_E | ACS_T));
    if (now != ctl) {
        struct acs_change *c = kappend(&log->at, &log->n, &log->room, sizeof *c);
        if (!c) return -N_ERR_NO_MEMORY;
        *c = (struct acs_change){f, (uint16_t)(at + 6), ctl};
        pci_write16(f->address, at + 6, now);
    }
    *set = pci_read16(f->address, at + 6) & *has;
    return 1;
}

static bool add_below(struct fns *g, const struct pci_device *up) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *o = pci_at(i);
        if (sends(o) && pci_below(up, o) && !fn_add(g, o)) return false;
    }
    return true;
}

/* Adds what m reaches, or what reaches m, past the units; *all when that is every function.
   0, or an error from acs_set (or -N_ERR_NO_MEMORY). */
static int reach(struct fns *g, struct acs_log *log, struct pci_device *m, bool *all) {
    uint16_t has, set;
    bool alone = true;
    for (int i = 0; i < pci_count() && alone; i++)
        alone = pci_at(i) == m || !sends(pci_at(i)) || !pci_same_device(m, pci_at(i));
    if (!alone) {
        /* A function that has no P2P Request Redirect has no peer-to-peer with the other
           functions (6.12.1.2); one without ACS may, as its device implements it. A device
           integrated in the root complex has no port above it that the redirected requests are
           sure to pass. */
        int acs = acs_set(log, m, ACS_PORT | ACS_B, &has, &set);
        if (acs < 0) return acs;
        if (!acs || !m->upstream || (set & (ACS_R | ACS_C)) != (has & (ACS_R | ACS_C))) {
            kprintf("iommu: %04x:%02x:%02x.%x: %s; the device's other functions are in its group\n", PCI_ADDRESS_ARGS(m->address), !acs ? "no ACS" : !m->upstream ? "integrated in the root complex" : "its ACS did not turn on");
            for (int i = 0; i < pci_count(); i++) {
                struct pci_device *o = pci_at(i);
                if (sends(o) && pci_same_device(m, o) && !fn_add(g, o)) return -N_ERR_NO_MEMORY;
            }
        }
    }
    for (struct pci_device *b = m->upstream; b; b = b->upstream) {
        /* conventional bridges: what is below shares their IDs; switch upstream ports: what
           passes them passed a downstream port or root port of their own */
        if (!b->pcie_cap || (b->pcie_type != 4 && b->pcie_type != 6)) continue;
        int acs = acs_set(log, b, ACS_PORT | ACS_B, &has, &set);
        if (acs < 0) return acs;
        if (b->pcie_type == 6) { /* a switch's downstream port: has all of ACS_PORT, or no ACS */
            if (acs && (has & ACS_PORT) == ACS_PORT && (set & ACS_PORT) == ACS_PORT) continue;
            kprintf("iommu: switch port %04x:%02x:%02x.%x above %04x:%02x:%02x.%x does not redirect requests between the "
                    "devices below the switch: they are in its group\n", PCI_ADDRESS_ARGS(b->address), PCI_ADDRESS_ARGS(m->address));
            if (!b->upstream) {
                *all = true;
                return 0;
            }
            if (!add_below(g, b->upstream)) return -N_ERR_NO_MEMORY;
        } else {
            /* a root port: Source Validation is required of one with ACS, P2P Request Redirect
               too when it passes requests to other root ports (6.12.1.1); what it has must turn
               on */
            if (acs && (has & ACS_V) && (set & ACS_PORT) == (has & ACS_PORT)) continue;
            kprintf("iommu: root port %04x:%02x:%02x.%x above %04x:%02x:%02x.%x %s: what is below it may send with any ID "
                    "and reach the other root ports; every function is in the group\n", PCI_ADDRESS_ARGS(b->address),
                    PCI_ADDRESS_ARGS(m->address),
                    !acs ? "has no ACS" : !(has & ACS_V) ? "cannot validate requester IDs" : "did not turn its ACS on");
            *all = true;
            return 0;
        }
    }
    /* the functions that send with one of m's IDs */
    struct ids mine = {0}, theirs = {0};
    bool ok = ids_of(m, &mine, true);
    for (int i = 0; ok && i < pci_count(); i++) {
        struct pci_device *o = pci_at(i);
        if (!sends(o) || fn_has(g, o)) continue;
        theirs.n = 0;
        ok = ids_of(o, &theirs, true);
        for (size_t k = 0; ok && k < theirs.n; k++)
            if (id_has(&mine, theirs.at[k])) {
                ok = fn_add(g, o);
                break;
            }
    }
    kfree(mine.at);
    kfree(theirs.at);
    return ok ? 0 : -N_ERR_NO_MEMORY;
}

/* 0, or why the group could not be formed (reach) */
static int group_of(struct pci_device *d, struct fns *g, struct acs_log *log) {
    bool all = false;
    if (!fn_add(g, d)) return -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < g->n && !all; i++) {
        int r = reach(g, log, g->at[i], &all);
        if (r) return r;
    }
    for (int i = 0; all && i < pci_count(); i++)
        if (sends(pci_at(i)) && !fn_add(g, pci_at(i))) return -N_ERR_NO_MEMORY;
    return 0;
}

static void cannot_isolate(const struct pci_device *d, const struct pci_device *m, const char *why) {
    kprintf("iommu: %04x:%02x:%02x.%x cannot be isolated: %04x:%02x:%02x.%x is in its group and %s\n", PCI_ADDRESS_ARGS(d->address),
            PCI_ADDRESS_ARGS(m->address), why);
}

/* Whether the group can join a domain, and its IDs. What the platform decides first: no member
   the kernel drives, every ID seen by a unit and none the platform's own; an error then with
   *fixed set, as no later attempt finds it otherwise. Then the members' claims: -N_ERR_BUSY
   while one is claimed or still in a domain. */
static int group_check(const struct pci_device *d, const struct fns *g, struct ids *ids, bool *fixed) {
    *fixed = true;
    for (size_t i = 0; i < g->n; i++) {
        const struct pci_device *m = g->at[i];
        if (m->kernel_owned && !m->guest) {
            cannot_isolate(d, m, "a kernel driver runs it");
            return -N_ERR_BUSY;
        }
        struct ids own = {0};
        bool ok = ids_of(m, &own, false);
        for (size_t k = 0; ok && k < own.n; k++)
            if (!is_covered(own.at[k])) {
                struct pci_address sender = pci_address_of(own.at[k].segment, own.at[k].requester);
                kprintf("iommu: %04x:%02x:%02x.%x cannot be isolated: no unit sees requests with ID %04x:%02x:%02x.%x\n",
                        PCI_ADDRESS_ARGS(d->address), PCI_ADDRESS_ARGS(sender));
                kfree(own.at);
                return -N_ERR_UNSUPPORTED;
            }
        kfree(own.at);
        if (!ok || !ids_of(m, ids, true)) {
            *fixed = false;
            return -N_ERR_NO_MEMORY;
        }
    }
    for (size_t k = 0; k < ids->n; k++)
        if (is_kernel_id(ids->at[k])) {
            struct pci_address sender = pci_address_of(ids->at[k].segment, ids->at[k].requester);
            kprintf("iommu: %04x:%02x:%02x.%x cannot be isolated: its group sends with ID %04x:%02x:%02x.%x, which the "
                    "platform's interrupt controllers or timers use\n", PCI_ADDRESS_ARGS(d->address), PCI_ADDRESS_ARGS(sender));
            return -N_ERR_BUSY;
        }
    *fixed = false;
    for (size_t i = 0; i < g->n; i++) {
        const struct pci_device *m = g->at[i];
        if (m != d && m->claimed) {
            cannot_isolate(d, m, "it is claimed");
            return -N_ERR_BUSY;
        }
        if (iommu_in_use(m)) {
            cannot_isolate(d, m, "it is in another claim's domain, or one whose block the units did not confirm");
            return -N_ERR_BUSY;
        }
    }
    return 0;
}

/* ---- the firmware's reserved memory in a domain ----
   The ranges for the domain's IDs, cut where any starts or ends. A piece never gains a
   permission a covering firmware restriction forbids, including an explicit no-access range. */
static bool reserved_for(const struct reserved *r, const struct iommu_domain *dom) {
    for (unsigned i = 0; i < dom->nids; i++)
        if (dom->ids[i].segment == r->segment && dom->ids[i].requester >= r->first && dom->ids[i].requester <= r->last)
            return true;
    return false;
}

bool iommu_os_ram(uint64_t start, uint64_t end) {
    const struct pmm_range *ram;
    unsigned count = pmm_ram_ranges(&ram);
    for (unsigned i = 0; i < count; i++)
        if (start < ram[i].end && ram[i].base < end) return true;
    return false;
}

/* A permanent firmware mapping must never expose allocator-owned RAM. Intel VT-d 3.16
   requires OS-visible DMA addresses not to overlap RMRRs. IVMD permissions likewise do not
   transfer ownership of kernel/app RAM to firmware. A malformed/incomplete memory map is not
   evidence of exclusion; refuse the domain rather than pin or leak arbitrary live OS pages. */
static bool reserved_piece_safe(uint64_t start, uint64_t end) {
    if (!iommu_os_ram(start, end)) return true;
    kprintf("iommu: firmware DMA reservation %lx..%lx may overlap OS-owned RAM; claim refused\n", start, end);
    return false;
}

static int reserved_map(struct iommu_domain *dom) {
    if (!nreserved) return 0;
    if (nreserved > SIZE_MAX / (2 * sizeof(uint64_t))) return -N_ERR_NO_MEMORY;
    uint64_t *cut = kmalloc(nreserved * 2 * sizeof *cut);
    if (!cut) return -N_ERR_NO_MEMORY;
    size_t n = 0;
    for (size_t r = 0; r < nreserved; r++)
        if (reserved_for(&reserved[r], dom)) {
            cut[n++] = reserved[r].start;
            cut[n++] = reserved[r].end;
        }
    for (size_t i = 1; i < n; i++) /* a few: sorted in place */
        for (size_t k = i; k && cut[k - 1] > cut[k]; k--) {
            uint64_t t = cut[k];
            cut[k] = cut[k - 1];
            cut[k - 1] = t;
        }
    int result = 0;
    for (size_t i = 0; !result && i + 1 < n; i++) {
        unsigned access = IOMMU_READ | IOMMU_WRITE;
        bool present = false;
        for (size_t r = 0; r < nreserved; r++)
            if (reserved[r].start <= cut[i] && cut[i + 1] <= reserved[r].end && reserved_for(&reserved[r], dom))
            {
                access &= reserved[r].access;
                present = true;
            }
        if (present && access && cut[i] < cut[i + 1]) {
            /* Check effective pieces: a no-access overlap can legitimately exclude OS RAM. */
            if (!reserved_piece_safe(cut[i], cut[i + 1])) result = -N_ERR_UNSUPPORTED;
            else if (!map_range(dom, NULL, cut[i], (cut[i + 1] - cut[i]) / PAGE_SIZE, access)) result = -N_ERR_NO_MEMORY;
        }
    }
    kfree(cut);
    return result;
}

/* ---- domains of claimed functions ---- */
static uint16_t domain_id_new(void) {
    uint64_t f = spin_lock(&domains_lock);
    uint16_t found = 0;
    for (unsigned id = 2; id < hw->domains && !found; id++)
        if (!(domain_used[id / 8] & 1u << (id % 8))) {
            domain_used[id / 8] |= 1u << (id % 8);
            found = (uint16_t)id;
        }
    spin_unlock(&domains_lock, f);
    return found;
}

static void domain_id_free(uint16_t id) {
    uint64_t f = spin_lock(&domains_lock);
    domain_used[id / 8] &= ~(1u << (id % 8));
    spin_unlock(&domains_lock, f);
}

/* On no list, no unit walking its tables any more. */
static void domain_release(struct iommu_domain *dom) {
    tables_free(dom->root, dom->levels);
    if (dom->d && dom->d->irt == &dom->irt) messages_forget(dom->d); /* its clear went unconfirmed */
    if (dom->irt.phys) iommu_irt_free(&dom->irt);
    if (dom->id != 1) domain_id_free(dom->id); /* 1 is the back end's own, never given out */
    kfree(dom->members);
    kfree(dom->ids);
    kfree((struct domain_rec *)dom);
}

/* domains_lock held. */
static void list_remove(struct iommu_domain **list, struct iommu_domain *dom) {
    for (struct iommu_domain **p = list; *p; p = &(*p)->next)
        if (*p == dom) {
            *p = dom->next;
            return;
        }
}

static void unlink(struct iommu_domain **list, struct iommu_domain *dom) {
    uint64_t f = spin_lock(&domains_lock);
    list_remove(list, dom);
    spin_unlock(&domains_lock, f);
}

static void push(struct iommu_domain **list, struct iommu_domain *dom) {
    uint64_t f = spin_lock(&domains_lock);
    dom->next = *list;
    *list = dom;
    spin_unlock(&domains_lock, f);
}

/* Whether the units drop what dom's functions send (pci_device.iommu_blocked). */
static void members_blocked(const struct iommu_domain *dom, bool blocked) {
    for (unsigned i = 0; i < dom->nmembers; i++) dom->members[i]->iommu_blocked = blocked;
}

/* The blocks not confirmed yet, tried again: a group comes free once one is. attach_mutex held,
   so only this changes the held list meanwhile. */
static void held_retry(void) {
    uint64_t f = spin_lock(&domains_lock);
    struct iommu_domain *dom = held;
    spin_unlock(&domains_lock, f);
    for (struct iommu_domain *next; dom; dom = next) {
        next = dom->next;
        if (!hw->block(dom)) continue;
        dom->blocked = true;
        members_blocked(dom, true);
        unlink(&held, dom);
        kprintf("iommu: %04x:%02x:%02x.%x: blocking it is confirmed now\n", PCI_ADDRESS_ARGS(dom->d->address));
        if (dom->unwanted) domain_release(dom);
    }
}

/* No automatic loss of the security boundary. The boot opt-in permits only a platform's fixed
   inability to isolate, never a failed invalidation or an earlier claim's quarantine. */
static int unisolated(const struct pci_device *d, const struct fns *g, int r) {
    bool blocked = d->iommu_blocked;
    for (size_t i = 0; i < g->n; i++) blocked |= g->at[i]->iommu_blocked;
    if (!blocked && boot_setting(N_BOOT_UNISOLATED_DRIVERS)) return 0;
    if (!blocked) {
        kprintf("iommu: %04x:%02x:%02x.%x: refusing an unisolated app driver (the boot record does not allow it)\n",
                PCI_ADDRESS_ARGS(d->address));
        return r;
    }
    kprintf("iommu: %04x:%02x:%02x.%x: its group was blocked when an earlier claim ended, and it cannot be isolated "
            "now\n", PCI_ADDRESS_ARGS(d->address));
    return r;
}

static void guests_stop(const struct iommu_domain *dom) {
    for (unsigned i = 0; i < dom->nmembers; i++) {
        const struct iommu_guest *guest = dom->members[i]->guest;
        if (guest) guest->stop(guest->ctx);
    }
}

void iommu_guests_stop(const struct iommu_domain *dom) { guests_stop(dom); }

/* A guest in a formed domain sends no message: the domain's remapping entries are the claimed
   function's (AMD-Vi: one table for every ID of the group). */
void iommu_guests_start(struct iommu_domain *dom, bool formed) {
    for (unsigned i = 0; i < dom->nmembers; i++) {
        struct pci_device *m = dom->members[i];
        if (!m->guest) continue;
        m->no_messages = formed;
        m->guest->start(formed ? dom : NULL, m->guest->ctx);
    }
}

static int attach(struct pci_device *d, struct iommu_domain **out, struct pci_device **resetting);

int iommu_attach(struct pci_device *d, struct iommu_domain **out, struct pci_device **resetting) {
    mutex_lock(&attach_mutex);
    int r = attach(d, out, resetting);
    mutex_unlock(&attach_mutex);
    return r;
}

static int attach(struct pci_device *d, struct iommu_domain **out, struct pci_device **resetting) {
    *out = NULL;
    *resetting = NULL;
    if (restore_failed) return -N_ERR_IO;
    if (lost_in_sleep) { /* not the platform's fixed inability the boot opt-in permits (unisolated) */
        kprintf("iommu: %04x:%02x:%02x.%x: the units did not come back from a sleep: no app driver until a restart\n",
                PCI_ADDRESS_ARGS(d->address));
        return -N_ERR_UNSUPPORTED;
    }
    if (!iommu_active()) {
        if (boot_setting(N_BOOT_UNISOLATED_DRIVERS)) return 0;
        kprintf("iommu: %04x:%02x:%02x.%x: isolation is required but no translating IOMMU is active\n",
                PCI_ADDRESS_ARGS(d->address));
        return -N_ERR_UNSUPPORTED;
    }
    held_retry();
    if (iommu_in_use(d)) {
        kprintf("iommu: %04x:%02x:%02x.%x is in another claim's domain, or one whose block the units did not confirm\n",
                PCI_ADDRESS_ARGS(d->address));
        return -N_ERR_BUSY;
    }
    struct fns g = {0};
    struct ids ids = {0};
    struct acs_log acs = {0};
    struct iommu_domain *dom = NULL;
    bool fixed = !pci_all_known();
    int r = -N_ERR_NO_MEMORY;
    if (fixed) {
        kprintf("iommu: %04x:%02x:%02x.%x cannot be isolated: enumeration did not keep every function, so its group "
                "is unknown\n", PCI_ADDRESS_ARGS(d->address));
        r = -N_ERR_UNSUPPORTED;
    } else if (!(r = group_of(d, &g, &acs)) && !(r = group_check(d, &g, &ids, &fixed))) {
        /* group_check success is not attach success: allocation failure must not return an
           unisolated claim, even when the administrator did not permit any downgrade. */
        r = -N_ERR_NO_MEMORY;
        uint16_t id = domain_id_new();
        if (!id) r = -N_ERR_BUSY;
        else if (!(dom = domain_alloc(id, hw->levels, 1))) domain_id_free(id);
        else {
            dom->d = d;
            dom->members = g.at;
            dom->nmembers = (unsigned)g.n;
            dom->ids = ids.at;
            dom->nids = (unsigned)ids.n;
            g.at = NULL;
            ids.at = NULL;
            r = reserved_map(dom);
            if (r) {
                domain_release(dom);
                dom = NULL;
            }
        }
    }
    if (r && fixed) r = unisolated(d, &g, r);
    if (dom) guests_stop(dom);
    if (dom && !hw->attach(dom)) {
        kprintf("iommu: %04x:%02x:%02x.%x: the units did not confirm its domain\n", PCI_ADDRESS_ARGS(d->address));
        if (!dom->written) {
            acs_undo(&acs);
            iommu_guests_start(dom, false);
        }
        iommu_domain_free(dom); /* what was written is blocked first, its guests staying stopped */
        dom = NULL;
        r = -N_ERR_IO;
    } else if (!dom) {
        acs_undo(&acs);
    }
    *resetting = acs.resetting;
    kfree(acs.at);
    kfree(g.at);
    kfree(ids.at);
    if (!dom) return r;
    members_blocked(dom, false);
    push(&attached, dom);
    kprintf("iommu: %04x:%02x:%02x.%x isolated in domain %u\n", PCI_ADDRESS_ARGS(d->address), dom->id);
    for (unsigned i = 1; i < dom->nmembers; i++)
        kprintf("iommu: %04x:%02x:%02x.%x is in its group: its DMA reaches only that domain too\n", PCI_ADDRESS_ARGS(dom->members[i]->address));
    *out = dom;
    return 0;
}

bool iommu_block(struct iommu_domain *dom) {
    mutex_lock(&attach_mutex);
    uint64_t f = spin_lock(&domains_lock);
    list_remove(&attached, dom);
    list_remove(&held, dom);
    spin_unlock(&domains_lock, f);
    if (!dom->blocked) dom->blocked = hw->block(dom);
    if (dom->blocked) members_blocked(dom, true);
    else {
        kprintf("iommu: %04x:%02x:%02x.%x: blocking it was not confirmed; its group stays out of every claim until it "
                "is\n", PCI_ADDRESS_ARGS(dom->d->address));
        push(&held, dom);
    }
    bool blocked = dom->blocked;
    mutex_unlock(&attach_mutex);
    return blocked;
}

void iommu_domain_free(struct iommu_domain *dom) {
    mutex_lock(&attach_mutex);
    /* a unit may still walk the tables of a group it was not seen to block */
    bool keep = dom->written && !dom->blocked && !iommu_block(dom);
    if (keep) dom->unwanted = true;
    mutex_unlock(&attach_mutex);
    if (!keep) domain_release(dom);
}

/* ---- interrupts and power ---- */
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, const struct irq *irq,
                       uint32_t *address, uint32_t *data) {
    if (restore_failed) return false;
    struct irq_target to = irq_target(irq);
    struct iommu_irt *t = NULL;
    if (hw && !hw->msi_table(d, &t)) return false;
    if (!t) {
        if (to.apic_id > IOMMU_PLAIN_REACH) return false;
        *address = iommu_msi_plain(to.apic_id);
        *data = to.vector; /* fixed delivery, edge (SDM 13.11.2) */
        return true;
    }
    int first = messages_entries(d, t, index, count);
    return first >= 0 && hw->msi_write(d, t, (unsigned)first, count, to.vector, to.apic_id, address, data);
}

bool iommu_msi_clear(struct pci_device *d) {
    if (restore_failed) return false;
    if (!d->irt) return true;
    if (hw && !hw->msi_erase(d, d->irt, d->irte, d->irte_room)) return false; /* kept: a unit may still use them */
    messages_forget(d);
    return true;
}

uint32_t iommu_msi_reach(const struct pci_device *d) { return hw && !restore_failed ? hw->msi_reach(d) : IOMMU_PLAIN_REACH; }

bool iommu_ioapic_entry(uint8_t ioapic_id, unsigned pin, unsigned pins, uint32_t dest, uint64_t *rte) {
    if (restore_failed) return false;
    return hw ? hw->ioapic(ioapic_id, pin, pins, dest, rte) : dest <= IOMMU_PLAIN_REACH;
}

uint32_t iommu_ioapic_reach(uint8_t ioapic_id) { return hw && !restore_failed ? hw->ioapic_reach(ioapic_id) : IOMMU_PLAIN_REACH; }

const char *iommu_resume(void) {
    if (!hw || hw->resume()) return NULL;
    lost_in_sleep = true;
    if (units_abandon()) {
        kprintf("iommu: the units did not come back after sleep: off, interrupts go plain, DMA untranslated\n");
        return "Device isolation, app drivers and sleep are off until a restart";
    }
    kprintf("iommu: the units did not come back after sleep, and are not confirmed off: no interrupt routes\n");
    return "Device interrupts are off until a restart";
}

bool iommu_shutdown(void) {
    if (hw && !hw->shutdown()) restore_failed = true;
    return !restore_failed;
}

/* ---- the machine's devices that are this file's (hal/hal.h) ----
   The PCI functions the units are (class 08h, subclass 06h): iommu_init took their registers and
   interrupt from the firmware's tables before any driver binds, and they come back from a sleep
   in iommu_resume. */
static bool unit_mine(struct hal_device *d) {
    return hw && hw->unit_function && d->bus == N_BUS_PCI && hw->unit_function(pci_device_of(d));
}

static const struct n_hal_rule unit_rules[] = {N_PCI_CLASS(0x08, 0x06)};
static const struct hal_machine iommu_units = {
    .owner = "iommu", .rules = unit_rules, .rule_count = ARRAY_SIZE(unit_rules), .mine = unit_mine};
HAL_MACHINE(iommu_units);
