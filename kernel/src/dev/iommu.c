/* IOMMU, the part AMD-Vi and VT-d share: domains and their page tables (512 entries a level, 9
   bits of the address each, in both formats), the requester IDs a function's requests carry,
   ACS on the ports above a claimed function, and the calls the rest of the kernel makes, passed
   to the back end that found its units. The address and ID rules are the PCI Express Base
   specification's (requester IDs, ACS) and the bridge specifications'. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/heap.h"
#include "mm/pmm.h"

#define PTE_ADDR 0x000FFFFFFFFFF000ull

static const struct iommu_hw *hw;
static struct iommu_domain *attached;
static uint8_t domain_used[65536 / 8];

void iommu_init(void) {
    if (cmdline_has("noiommu")) {
        kprintf("iommu: off (noiommu): a device given to an app reaches all of memory\n");
        return;
    }
    const struct iommu_hw *found = iommu_amd_probe();
    if (!found) found = iommu_intel_probe();
    if (!found) {
        kprintf("iommu: none described (no IVRS or DMAR): a device given to an app reaches all of memory\n");
        return;
    }
    hw = found; /* table writes need it from here on */
    domain_used[0] |= 3; /* 0 is never used, 1 is the back end's */
    if (!hw->start()) {
        hw = NULL;
        kprintf("iommu: not used: a device given to an app reaches all of memory\n");
    }
}

bool iommu_active(void) { return hw != NULL; }

uint64_t iommu_deadline(unsigned ms) { return rdtsc() + (tsc_hz ? tsc_hz / 1000 : 3000000ull) * ms; }
bool iommu_expired(uint64_t deadline) { return (int64_t)(rdtsc() - deadline) > 0; }

bool iommu_fault_report(void) {
    static uint64_t second;
    static unsigned shown;
    uint64_t now = uptime_ms() / 1000;
    if (now != second) {
        second = now;
        shown = 0;
    }
    if (shown == 8) kprintf("iommu: more blocked requests this second are not shown\n");
    return shown++ < 8;
}

void iommu_cache_flush(const volatile void *p, size_t bytes) {
    uintptr_t at = (uintptr_t)p & ~63ull, end = (uintptr_t)p + bytes;
    for (; at < end; at += 64) __asm__ volatile("clflush (%0)" ::"r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

void iommu_entry_write(volatile uint64_t *e, uint64_t v) {
    *e = v;
    if (!hw->coherent) iommu_cache_flush(e, sizeof *e);
}

/* ---- page tables ---- */
static unsigned shift_of(unsigned level) { return 12 + 9 * (level - 1); }

static uint64_t table_new(void) {
    uint64_t t = pmm_alloc_zeroed();
    if (t && !hw->coherent) iommu_cache_flush(phys_to_virt(t), PAGE_SIZE);
    return t;
}

static void tables_free(uint64_t table, unsigned level) {
    const uint64_t *e = phys_to_virt(table);
    for (unsigned i = 0; level > 1 && i < 512; i++)
        if (e[i] & 1) tables_free(e[i] & PTE_ADDR, level - 1);
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
        }
        table = *e & PTE_ADDR;
    }
}

bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
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
        iommu_entry_write(e, hw->leaf(pa, level));
        i += 1ull << (shift_of(level) - 12);
    }
    return !dom->d || hw->flush(dom, false);
}

bool iommu_unmap(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    ASSERT(dom->leaf_levels == 1);
    for (size_t i = 0; i < count; i++) {
        volatile uint64_t *e = entry(dom, frames ? frames[i] : base + i * PAGE_SIZE, 1, false);
        if (e && *e) iommu_entry_write(e, 0);
    }
    return hw->flush(dom, true);
}

static struct iommu_domain *domain_alloc(uint16_t id, unsigned levels, unsigned leaf_levels) {
    struct iommu_domain *dom = kzalloc(sizeof *dom);
    if (dom && !(dom->root = table_new())) {
        kfree(dom);
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

struct iommu_domain *iommu_domain_of(const struct pci_device *d) {
    for (struct iommu_domain *dom = attached; dom; dom = dom->next)
        if (dom->d == d) return dom;
    return NULL;
}

/* ---- requester IDs ---- */
static uint16_t requester_id(const struct pci_device *d) { return d->bus << 8 | d->dev << 3 | d->func; }

static bool id_add(uint16_t *ids, unsigned *n, uint16_t id) {
    for (unsigned i = 0; i < *n; i++)
        if (ids[i] == id) return true;
    if (*n == IOMMU_IDS) return false;
    ids[(*n)++] = id;
    return true;
}

/* The IDs d's requests may arrive with: its own, and for each bridge to conventional PCI or
   PCI-X above it, which issues the requests from below as its own, the bridge's ID and its
   secondary bus with device and function 0; then the firmware's aliases of those. 0 when there
   are more than IOMMU_IDS. */
unsigned iommu_requester_ids(const struct pci_device *d, uint16_t *ids) {
    unsigned n = 0;
    bool fits = id_add(ids, &n, requester_id(d));
    for (const struct pci_device *b = pci_upstream(d); fits && b; b = pci_upstream(b)) {
        if (b->pcie_cap && b->pcie_type != 7) continue; /* 7: PCI Express to PCI/PCI-X bridge */
        uint8_t secondary = pci_read32(b->bus, b->dev, b->func, 0x18) >> 8;
        fits = id_add(ids, &n, (uint16_t)secondary << 8) && id_add(ids, &n, requester_id(b));
    }
    for (unsigned i = 0, own = n; fits && hw->alias && i < own; i++) fits = id_add(ids, &n, hw->alias(ids[i]));
    return fits ? n : 0;
}

/* Another function that sends with one of these IDs: a domain for d would take its requests
   too. Bridges no driver runs send none that matter. */
static const struct pci_device *sharer(const struct pci_device *d, const uint16_t *ids, unsigned n) {
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_device *o = pci_at(i);
        if (o == d || (o->cls == 0x06 && !o->kernel_owned)) continue;
        uint16_t theirs[IOMMU_IDS];
        unsigned m = iommu_requester_ids(o, theirs);
        for (unsigned a = 0; a < n; a++)
            for (unsigned b = 0; b < m; b++)
                if (ids[a] == theirs[b]) return o;
    }
    return NULL;
}

/* ---- ACS ----
   A switch passes a request between the devices below it without the root complex, and so
   without the IOMMU, unless its downstream ports redirect it upstream; root ports likewise.
   Source validation, request and completion redirect and upstream forwarding, where a port
   has them. */
#define ACS_WANTED (1u << 0 | 1u << 2 | 1u << 3 | 1u << 4)
static void acs_enable(const struct pci_device *d) {
    for (const struct pci_device *b = pci_upstream(d); b; b = pci_upstream(b)) {
        if (!b->pcie_cap || (b->pcie_type != 4 && b->pcie_type != 6)) continue; /* root, downstream port */
        uint16_t acs = pci_find_ext_cap(b, 0x0D);
        if (!acs) {
            if (b->pcie_type == 6)
                kprintf("iommu: switch port %02x:%02x.%u above %02x:%02x.%u has no ACS: requests between "
                        "devices below that switch do not pass the IOMMU\n",
                        b->bus, b->dev, b->func, d->bus, d->dev, d->func);
            continue;
        }
        uint16_t want = pci_read16(b->bus, b->dev, b->func, acs + 4) & ACS_WANTED;
        uint16_t ctl = pci_read16(b->bus, b->dev, b->func, acs + 6);
        if ((ctl & want) != want) pci_write16(b->bus, b->dev, b->func, acs + 6, ctl | want);
    }
}

/* ---- the firmware's reserved memory ---- */
#define MAX_RESERVED 32
static struct reserved {
    uint16_t first, last;
    uint64_t start, end;
} reserved[MAX_RESERVED];
static unsigned nreserved;

void iommu_reserved(uint16_t first, uint16_t last, uint64_t start, uint64_t end) {
    if (first > last || start >= end) return;
    for (unsigned i = 0; i < nreserved; i++) /* firmware repeats them (one per IVHD type, or scope) */
        if (reserved[i].first == first && reserved[i].last == last && reserved[i].start == start &&
            reserved[i].end == end)
            return;
    if (nreserved == MAX_RESERVED) {
        kprintf("iommu: more than %u reserved ranges; %lx-%lx is not kept\n", MAX_RESERVED, start, end);
        return;
    }
    reserved[nreserved++] = (struct reserved){first, last, start, end};
}

unsigned iommu_reserved_count(void) { return nreserved; }

static bool reserved_map(struct iommu_domain *dom) {
    for (unsigned r = 0; r < nreserved; r++)
        for (unsigned i = 0; i < dom->nids; i++)
            if (dom->ids[i] >= reserved[r].first && dom->ids[i] <= reserved[r].last) {
                if (!iommu_map(dom, NULL, reserved[r].start, (reserved[r].end - reserved[r].start) / PAGE_SIZE))
                    return false;
                break;
            }
    return true;
}

/* ---- domains of claimed functions ---- */
static uint16_t domain_id_new(void) {
    for (unsigned id = 2; id < hw->domains; id++)
        if (!(domain_used[id / 8] & 1u << (id % 8))) {
            domain_used[id / 8] |= 1u << (id % 8);
            return (uint16_t)id;
        }
    return 0;
}

int iommu_attach(struct pci_device *d, struct iommu_domain **out) {
    if (!hw) return -N_ERR_UNSUPPORTED;
    uint16_t ids[IOMMU_IDS];
    unsigned n = iommu_requester_ids(d, ids);
    if (!n) return -N_ERR_UNSUPPORTED;
    const struct pci_device *o = sharer(d, ids, n);
    if (o) {
        kprintf("iommu: %02x:%02x.%u sends with the same requester ID as %02x:%02x.%u: they cannot be "
                "kept apart\n", d->bus, d->dev, d->func, o->bus, o->dev, o->func);
        return -N_ERR_BUSY;
    }
    uint16_t id = domain_id_new();
    if (!id) return -N_ERR_BUSY;
    struct iommu_domain *dom = domain_alloc(id, hw->levels, 1);
    if (!dom) {
        domain_used[id / 8] &= ~(1u << (id % 8));
        return -N_ERR_NO_MEMORY;
    }
    dom->d = d;
    memcpy(dom->ids, ids, sizeof ids);
    dom->nids = n;
    acs_enable(d);
    if (!reserved_map(dom) || !hw->attach(dom)) {
        dom->blocked = true; /* nothing changed: the function still passes through */
        iommu_domain_free(dom);
        return -N_ERR_NO_MEMORY;
    }
    dom->next = attached;
    attached = dom;
    kprintf("iommu: %02x:%02x.%u isolated in domain %u\n", d->bus, d->dev, d->func, id);
    *out = dom;
    return 0;
}

bool iommu_block(struct iommu_domain *dom) {
    for (struct iommu_domain **p = &attached; *p; p = &(*p)->next)
        if (*p == dom) {
            *p = dom->next;
            break;
        }
    if (!dom->blocked) dom->blocked = hw->block(dom);
    if (!dom->blocked)
        kprintf("iommu: %02x:%02x.%u: blocking it was not confirmed\n", dom->d->bus, dom->d->dev, dom->d->func);
    return dom->blocked;
}

void iommu_domain_free(struct iommu_domain *dom) {
    /* a unit may still walk the tables of a function it was not seen to block */
    if (!dom->blocked && !iommu_block(dom)) return;
    tables_free(dom->root, dom->levels);
    if (dom->irt) pmm_free(dom->irt);
    domain_used[dom->id / 8] &= ~(1u << (dom->id % 8));
    kfree(dom);
}

/* ---- interrupts and power ---- */
bool iommu_msi_message(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                       uint32_t *address, uint32_t *data) {
    if (hw) return hw->msi(d, index, count, vector, dest, address, data);
    if (dest > 0xFF) return false;
    *address = iommu_msi_plain(dest);
    *data = vector;
    return true;
}

void iommu_msi_clear(struct pci_device *d) {
    if (hw) hw->msi_clear(d);
}

uint64_t iommu_ioapic_entry(uint8_t ioapic_id, uint32_t gsi, uint64_t rte) {
    return hw ? hw->ioapic(ioapic_id, gsi, rte) : rte;
}

void iommu_resume(void) {
    if (hw) hw->resume();
}

void iommu_shutdown(void) {
    if (hw) hw->shutdown();
}
