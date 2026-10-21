/* Intel VT-d, per the Intel Virtualization Technology for Directed I/O Architecture
   Specification: the DMAR table, legacy-mode root and context tables, second-stage page tables,
   the invalidation queue, fault recording and interrupt remapping.
   Every unit gets the same picture. Its root table points every bus at one context table whose
   entries let a function through (pass-through, or the identity domain on a unit without it);
   a bus with a claimed function gets a context table of its own in each unit, where the entries
   of the claim's group point at the claim's domain and, once blocked, are not present.
   Interrupt remapping, when every unit has it, takes every interrupt: compatibility-format
   interrupts are refused (they would let any device send any vector), so each MSI and IOAPIC
   pin is an entry of one shared table, checked against the requester ID of its source.
   Entries 0..255 are the IOAPIC pins by GSI, then 32 for each function with MSI or MSI-X on.
   Units that require it get the x2APIC entry format (Extended Interrupt Mode). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* registers */
#define R_VER     0x00
#define R_CAP     0x08
#define R_ECAP    0x10
#define R_GCMD    0x18
#define R_GSTS    0x1C
#define R_RTADDR  0x20
#define R_FSTS    0x34
#define R_FECTL   0x38
#define R_FEDATA  0x3C
#define R_FEADDR  0x40
#define R_FEUADDR 0x44
#define R_PMEN    0x64
#define R_IQH     0x80
#define R_IQT     0x88
#define R_IQA     0x90
#define R_IQERCD  0xB0
#define R_IRTA    0xB8

/* global command bits, and the status bits at the same positions */
#define GCMD_TE      (1u << 31)
#define GCMD_SRTP    (1u << 30)
#define GCMD_WBF     (1u << 27)
#define GCMD_QIE     (1u << 26)
#define GCMD_IRE     (1u << 25)
#define GCMD_SIRTP   (1u << 24)
#define GCMD_CFI     (1u << 23)
#define GSTS_ONESHOT (GCMD_SRTP | 1u << 29 | GCMD_WBF | GCMD_SIRTP) /* commands, not states */
#define FSTS_PFO (1u << 0)
#define FSTS_IQE (1u << 4)
#define FECTL_IM (1u << 31)
#define PMEN_EPM (1u << 31)
#define PMEN_PRS (1u << 0)
#define IRTA_EIME (1ull << 11)

#define CAP_RWBF     (1ull << 4)
#define CAP_PLMR     (1ull << 5)
#define CAP_PHMR     (1ull << 6)
#define CAP_CM       (1ull << 7)
#define CAP_ND(c)    ((unsigned)(c) & 7)
#define CAP_SAGAW(c) ((unsigned)((c) >> 8) & 0x1F)
#define CAP_FRO(c)   (((unsigned)((c) >> 24) & 0x3FF) * 16)
#define CAP_SSLPS(c) ((unsigned)((c) >> 34) & 0xF)
#define CAP_NFR(c)   (((unsigned)((c) >> 40) & 0xFF) + 1)
#define CAP_DWD      (1ull << 54)
#define CAP_DRD      (1ull << 55)
#define ECAP_C     (1ull << 0)
#define ECAP_QI    (1ull << 1)
#define ECAP_IR    (1ull << 3)
#define ECAP_EIM   (1ull << 4)
#define ECAP_PT    (1ull << 6)
#define ECAP_MHMV(e) ((unsigned)((e) >> 20) & 0xF)
#define ECAP_EIMER (1ull << 61) /* interrupt remapping in Extended Interrupt Mode only */
#define ECAP_IRREQ (1ull << 62) /* interrupts blocked while interrupt remapping is off */

#define ADDR ((uint64_t)0x000FFFFFFFFFF000)
/* root and context entries: two qwords, present bit 0 of the first */
#define CTX_P    1ull
#define CTX_PASS (2ull << 2) /* translation type 10b: pass-through */
#define PASS_DOMAIN 1
/* second-stage entries */
#define SL_R  (1ull << 0)
#define SL_W  (1ull << 1)
#define SL_PS (1ull << 7)

/* invalidation descriptors, 128 bits: the first qword (the second is 0 but for a wait) */
#define INV_CONTEXT_GLOBAL   (1ull | 1ull << 4)
#define INV_IOTLB_GLOBAL     (2ull | 1ull << 4)
#define INV_IOTLB_DOMAIN(id) (2ull | 2ull << 4 | (uint64_t)(id) << 16)
#define INV_IOTLB_DW         (1ull << 6) /* drain writes, reads: what a version 1 unit needs asked */
#define INV_IOTLB_DR         (1ull << 7)
#define INV_IEC_GLOBAL       4ull
#define INV_IEC(index, log2) (4ull | 1ull << 4 | (uint64_t)(log2) << 27 | (uint64_t)(index) << 32)
#define INV_WAIT(token)      (5ull | 1ull << 5 | 1ull << 6 | (uint64_t)(token) << 32) /* status write, fence */
#define INV_NOTHING          5ull /* a wait that writes nothing: what a rejected descriptor becomes */

/* interrupt remapping table */
#define IRT_ENTRIES    4096
#define IRT_S          11 /* IRTA.S: 2^(S+1) entries */
#define IOAPIC_ENTRIES 256
#define MSI_BLOCK_LOG2 5
#define MSI_BLOCK      (1u << MSI_BLOCK_LOG2)
#define MSI_BLOCKS     ((int)((IRT_ENTRIES - IOAPIC_ENTRIES) / MSI_BLOCK))
#define IRTE_P         1ull
#define IRTE_DM        (1ull << 2) /* logical destination */
#define IRTE_TM        (1ull << 4) /* level */
#define IRTE_DLM(m)    ((uint64_t)(m) << 5)
#define IRTE_VECTOR(v) ((uint64_t)(uint8_t)(v) << 16)
#define IRTE_SID(sid, svt) ((uint64_t)(sid) | (uint64_t)(svt) << 18) /* SVT 1: the ID, 2: a bus range */
/* an IOAPIC entry in remappable format */
#define RTE_KEPT (0xFFull | 1ull << 13 | 1ull << 15 | 1ull << 16) /* vector, polarity, trigger, mask */
#define RTE_REMAPPABLE (1ull << 48)

struct unit {
    uint64_t base;
    volatile uint8_t *regs;
    unsigned pages; /* of registers, as the DMAR says */
    uint64_t cap, ecap;
    uint64_t root;  /* root table */
    uint64_t pass;  /* the context table of every bus without a claimed function */
    uint64_t queue; /* invalidation queue: 256 descriptors */
    uint32_t tail;
    bool queue_on;  /* the queue on is ours (queue_start), not one the firmware left on */
    bool waiting;   /* a sync's wait descriptor went in */
    bool lost;      /* a descriptor since the last sync was not done: that sync fails */
};
static struct unit *units;
static size_t nunits, units_room;
static struct ioapic_scope {
    uint8_t id;   /* the MADT's */
    uint16_t sid; /* the requester ID it sends with */
} *ioapics;
static size_t nioapics, ioapics_room;
static uint8_t dmar_flags;
static bool remap, x2, caching_mode, write_buffers, parse_lost;
static bool irreq; /* a unit requires interrupt remapping (ECAP.IRREQ) */
static unsigned iec_mask = 15; /* the largest IEC invalidation mask every unit takes (MHMV) */
static struct iommu_domain *identity; /* units without pass-through */
static uint64_t sems, irt;
static size_t sem_pages;
static uint32_t token;
static int fault_vector = -1;
static const struct pci_device *msi_owner[MSI_BLOCKS];

static struct iommu_hw intel;

static uint32_t rd32(const struct unit *u, unsigned off) { return *(volatile uint32_t *)(u->regs + off); }
static void wr32(const struct unit *u, unsigned off, uint32_t v) { *(volatile uint32_t *)(u->regs + off) = v; }
static uint64_t rd64(const struct unit *u, unsigned off) { return *(volatile uint64_t *)(u->regs + off); }
static void wr64(const struct unit *u, unsigned off, uint64_t v) { *(volatile uint64_t *)(u->regs + off) = v; }

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- DMAR ---- */
/* A device scope's requester ID: its path from the start bus, through the bridges on the way;
   -1 when a bridge on the way is not there. */
static int scope_id(const uint8_t *s) {
    unsigned bus = s[5], n = (s[1] - 6u) / 2;
    for (unsigned i = 0; i < n; i++) {
        unsigned dev = s[6 + 2 * i] & 31, func = s[7 + 2 * i] & 7;
        if (i + 1 == n) return (int)(bus << 8 | dev << 3 | func);
        uint32_t r = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)func, 0x18);
        if (r == 0xFFFFFFFF) return -1;
        bus = r >> 8 & 0xFF;
    }
    return -1;
}

/* Device scopes from `at` to `end`, each passed with its requester ID. */
static void scopes(const uint8_t *b, unsigned at, unsigned end, void (*each)(const uint8_t *s, int id, void *arg),
                   void *arg) {
    for (; at + 6 <= end && b[at + 1] >= 6 && at + b[at + 1] <= end; at += b[at + 1]) {
        int id = scope_id(b + at);
        if (id >= 0) each(b + at, id, arg);
        else kprintf("iommu: a DMAR device scope leads through a bridge that is not there; ignored\n");
    }
}

/* The buses below a bridge with this ID, as its registers say. */
static void bridge_buses(int id, uint16_t *first, uint16_t *last) {
    uint32_t r = pci_read32((uint8_t)(id >> 8), (id >> 3) & 31, id & 7, 0x18);
    *first = (uint16_t)((r >> 8 & 0xFF) << 8);
    *last = (uint16_t)((r >> 16 & 0xFF) << 8 | 0xFF);
}

/* DRHD scopes (8.3.1): an endpoint, a bridge with everything below it, an IOAPIC (its
   enumeration ID is the MADT's), an HPET that sends MSIs, an ACPI device. */
static void drhd_scope(const uint8_t *s, int id, void *arg) {
    (void)arg;
    uint16_t first, last;
    switch (s[0]) {
    case 1: iommu_cover((uint16_t)id, (uint16_t)id); break;
    case 2:
        iommu_cover((uint16_t)id, (uint16_t)id);
        bridge_buses(id, &first, &last);
        iommu_cover(first, last);
        break;
    case 3: {
        iommu_kernel_id((uint16_t)id);
        struct ioapic_scope *a = kappend(&ioapics, &nioapics, &ioapics_room, sizeof *a);
        if (a) *a = (struct ioapic_scope){s[4], (uint16_t)id};
        else parse_lost = true;
        break;
    }
    case 4: iommu_kernel_id((uint16_t)id); break;
    }
}

/* RMRR: memory the functions in its scopes reach by DMA from the firmware's start (a USB
   controller's legacy emulation, a graphics device's stolen memory), to be mapped for reading
   and writing. A bridge's scope is everything below it too. */
static void rmrr_scope(const uint8_t *s, int id, void *arg) {
    const uint64_t *range = arg;
    uint16_t first, last;
    if (s[0] != 1 && s[0] != 2) return;
    iommu_reserved((uint16_t)id, (uint16_t)id, range[0], range[1], IOMMU_READ | IOMMU_WRITE);
    if (s[0] == 2) {
        bridge_buses(id, &first, &last);
        iommu_reserved(first, last, range[0], range[1], IOMMU_READ | IOMMU_WRITE);
    }
}

static bool parse_dmar(void) {
    const struct acpi_sdt *h = acpi_find("DMAR", 0);
    if (!h || h->length < 48) return false;
    const uint8_t *b = (const uint8_t *)h;
    dmar_flags = b[37];
    for (uint32_t off = 48; off + 4 <= h->length;) {
        uint16_t type = u16(b + off), len = u16(b + off + 2);
        if (len < 4 || off + len > h->length) break;
        uint64_t base;
        if (type == 0 && len >= 16) { /* DRHD */
            memcpy(&base, b + off + 8, 8);
            if (u16(b + off + 6) != 0) {
                kprintf("iommu: VT-d unit at %lx is on PCI segment %u, which Nocturne does not reach\n", base,
                        u16(b + off + 6));
            } else if (!base) {
                kprintf("iommu: the DMAR describes a VT-d unit without an address; not used\n");
            } else {
                struct unit *u = kappend(&units, &nunits, &units_room, sizeof *u);
                if (!u) {
                    parse_lost = true;
                } else {
                    *u = (struct unit){.base = base, .pages = 1u << (b[off + 5] & 15)};
                    if (b[off + 4] & 1) iommu_cover(0, 0xFFFF); /* INCLUDE_PCI_ALL: what no other unit has */
                    scopes(b, off + 16, off + len, drhd_scope, NULL);
                }
            }
        } else if (type == 1 && len >= 24 && u16(b + off + 6) == 0) { /* RMRR */
            uint64_t limit;
            memcpy(&base, b + off + 8, 8);
            memcpy(&limit, b + off + 16, 8);
            uint64_t range[2] = {ALIGN_DOWN(base, PAGE_SIZE), ALIGN_UP(limit + 1, PAGE_SIZE)};
            if (limit >= base) scopes(b, off + 24, off + len, rmrr_scope, range);
        }
        off += len;
    }
    return nunits != 0;
}

/* Every IOAPIC of the MADT in the scope of exactly one unit (8.3.1.1): the source check of its
   pins' entries needs the ID it sends with. */
static bool ioapics_described(void) {
    unsigned n;
    const struct acpi_ioapic *a = acpi_ioapics(&n);
    for (unsigned i = 0; i < n; i++) {
        unsigned found = 0;
        for (size_t k = 0; k < nioapics; k++) found += ioapics[k].id == a[i].id;
        if (found != 1) {
            kprintf("iommu: the DMAR names IOAPIC %u %s\n", a[i].id, found ? "more than once" : "under no unit");
            return false;
        }
    }
    return true;
}

/* ---- commands ---- */
static const char *command_name(uint32_t bit) {
    switch (bit) {
    case GCMD_TE: return "translation (TE)";
    case GCMD_SRTP: return "its root table pointer (SRTP)";
    case GCMD_QIE: return "its invalidation queue (QIE)";
    case GCMD_IRE: return "interrupt remapping (IRE)";
    case GCMD_SIRTP: return "its remapping table pointer (SIRTP)";
    case GCMD_CFI: return "compatibility-format interrupts (CFI)";
    default: return "a global command";
    }
}

static bool command(struct unit *u, uint32_t bit, bool on) {
    uint32_t sts = rd32(u, R_GSTS) & ~GSTS_ONESHOT;
    wr32(u, R_GCMD, on ? sts | bit : sts & ~bit);
    for (uint64_t t = iommu_deadline(100); !(rd32(u, R_GSTS) & bit) == on; pause())
        if (iommu_expired(t)) {
            kprintf("iommu: VT-d unit at %lx did not %s %s\n", u->base,
                    bit & GSTS_ONESHOT ? "take" : on ? "turn on" : "turn off", command_name(bit));
            return false;
        }
    return true;
}

/* Units that buffer their writes (CAP.RWBF) write them, for a change no invalidation follows:
   the invalidations flush them first themselves (6.8). False when one did not confirm it. */
static bool write_buffers_flush(void) {
    bool ok = true;
    for (size_t i = 0; write_buffers && i < nunits; i++) {
        struct unit *u = &units[i];
        if (!(u->cap & CAP_RWBF)) continue;
        wr32(u, R_GCMD, (rd32(u, R_GSTS) & ~GSTS_ONESHOT) | GCMD_WBF);
        for (uint64_t t = iommu_deadline(100); rd32(u, R_GSTS) & GCMD_WBF; pause())
            if (iommu_expired(t)) {
                kprintf("iommu: VT-d unit at %lx did not flush its write buffers\n", u->base);
                ok = false;
                break;
            }
    }
    return ok;
}

static bool queue_start(struct unit *u) {
    wr64(u, R_IQT, 0);
    u->tail = 0;
    wr64(u, R_IQA, u->queue); /* 128-bit descriptors, one page */
    u->queue_on = command(u, GCMD_QIE, true);
    return u->queue_on;
}

/* A unit that rejected a descriptor fetches none until IQE is cleared, its head on that
   descriptor; the ones before it complete as normal (6.5.2.11). It becomes a wait that writes
   nothing, the tail is written again (in case the tail was what it rejected) and the queue goes
   on, never restarted under descriptors in flight: what the rejected one asked is lost. */
static void queue_recover(struct unit *u) {
    uint32_t head = (uint32_t)rd64(u, R_IQH) & (PAGE_SIZE - 16);
    kprintf("iommu: VT-d unit at %lx rejected the command at %x (reason %u); it is skipped\n", u->base, head,
            (unsigned)(rd64(u, R_IQERCD) & 15));
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)phys_to_virt(u->queue) + head);
    slot[0] = INV_NOTHING;
    slot[1] = 0;
    if (!intel.coherent) iommu_cache_flush(slot, 16);
    wr64(u, R_IQT, u->tail);
    wr32(u, R_FSTS, FSTS_IQE);
    u->lost = true;
}

/* A descriptor for one unit: false, and lost (u->lost), when it had no room for it in time.
   IOTLB invalidations ask for the draining a version 1 unit does only when asked (6.5.4). */
static bool submit(struct unit *u, uint64_t lo, uint64_t hi) {
    if ((lo & 0xF) == 2) lo |= (u->cap & CAP_DRD ? INV_IOTLB_DR : 0) | (u->cap & CAP_DWD ? INV_IOTLB_DW : 0);
    for (uint64_t t = iommu_deadline(100); (rd64(u, R_IQH) & (PAGE_SIZE - 16)) == (u->tail + 16) % PAGE_SIZE; pause()) {
        if (rd32(u, R_FSTS) & FSTS_IQE) queue_recover(u);
        if (iommu_expired(t)) {
            u->lost = true;
            return false;
        }
    }
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)phys_to_virt(u->queue) + u->tail);
    slot[0] = lo;
    slot[1] = hi;
    if (!intel.coherent) iommu_cache_flush(slot, 16);
    u->tail = (u->tail + 16) % PAGE_SIZE;
    wr64(u, R_IQT, u->tail);
    return true;
}

static void send(uint64_t lo) {
    for (size_t i = 0; i < nunits; i++) submit(&units[i], lo, 0);
}

/* Interrupt entries first..first + 2^log2 - 1 (first aligned to that), in descriptors whose
   mask every unit takes. */
static void send_iec(unsigned first, unsigned log2) {
    unsigned mask = log2 < iec_mask ? log2 : iec_mask;
    for (unsigned at = first; at < first + (1u << log2); at += 1u << mask) send(INV_IEC(at, mask));
}

/* A change of root or context entries: the context caches, then the IOTLBs, both whole (6.5.2.1:
   a global context invalidation is followed by a global IOTLB one). */
static void send_context_flush(void) {
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_GLOBAL);
}

static void fault_show(const struct unit *u, uint64_t lo, uint64_t hi) {
    (void)u;
    unsigned sid = hi & 0xFFFF, reason = hi >> 32 & 0xFF;
    if (!iommu_fault_report()) return;
    if (reason >= 0x20)
        kprintf("iommu: %02x:%02x.%u: interrupt (entry %u) blocked, reason %02xh\n", sid >> 8, (sid >> 3) & 31,
                sid & 7, (unsigned)(lo >> 48), reason);
    else
        kprintf("iommu: %02x:%02x.%u: %s at %lx blocked, reason %02xh\n", sid >> 8, (sid >> 3) & 31, sid & 7,
                hi >> 62 & 1 ? "DMA read" : "DMA write", lo & ADDR, reason);
}

static void faults_drain(struct unit *u) {
    unsigned fro = CAP_FRO(u->cap), nfr = CAP_NFR(u->cap);
    for (unsigned i = 0; i < nfr; i++) {
        volatile uint64_t *r = (volatile uint64_t *)(u->regs + fro + i * 16);
        uint64_t hi = r[1];
        if (!(hi >> 63)) continue;
        fault_show(u, r[0], hi);
        *(volatile uint32_t *)(u->regs + fro + i * 16 + 12) = 1u << 31; /* F: the record is free again */
    }
    if (rd32(u, R_FSTS) & FSTS_PFO) {
        wr32(u, R_FSTS, FSTS_PFO);
        kprintf("iommu: VT-d unit at %lx: more faults than its records\n", u->base);
    }
}

static void fault_isr(struct regs *r) {
    (void)r;
    for (size_t i = 0; i < nunits; i++) faults_drain(&units[i]);
}

/* Waits until units first..first + n - 1 have done every descriptor sent since their last sync,
   the last one a wait that completed: false when one was lost or rejected, or a unit did not
   finish them in time. */
static bool sync_units(size_t first, size_t n) {
    volatile uint32_t *word = phys_to_virt(sems);
    if (!++token) token++;
    for (size_t i = first; i < first + n; i++) {
        word[i] = 0;
        units[i].waiting = submit(&units[i], INV_WAIT(token), sems + i * 4);
    }
    for (size_t i = first; i < first + n; i++) {
        struct unit *u = &units[i];
        for (uint64_t t = iommu_deadline(100); u->waiting && word[i] != token; pause()) {
            if (rd32(u, R_FSTS) & FSTS_IQE) queue_recover(u); /* the wait behind it still comes */
            if (iommu_expired(t)) {
                kprintf("iommu: VT-d unit at %lx did not finish its commands\n", u->base);
                u->lost = true;
                break;
            }
        }
    }
    bool ok = true;
    for (size_t i = first; i < first + n; i++) {
        faults_drain(&units[i]);
        ok &= !units[i].lost;
        units[i].lost = false;
    }
    return ok;
}

static bool sync(void) { return sync_units(0, nunits); }

/* ---- context entries ---- */
static uint64_t pass_lo(const struct unit *u) {
    return u->ecap & ECAP_PT ? CTX_P | CTX_PASS : CTX_P | identity->root;
}
/* Pass-through takes the unit's widest address width; the identity domain has the common one. */
static uint64_t pass_hi(const struct unit *u) {
    unsigned aw = u->ecap & ECAP_PT ? 31 - __builtin_clz(CAP_SAGAW(u->cap)) : intel.levels - 2;
    return aw | (uint64_t)PASS_DOMAIN << 8;
}

/* The entry of the function with this requester ID in u. create: its bus gets a context table
   of its own (the same entries) when it still has the shared one; NULL without memory. */
static volatile uint64_t *context_entry(struct unit *u, uint16_t id, bool create) {
    volatile uint64_t *root = (volatile uint64_t *)phys_to_virt(u->root) + (id >> 8) * 2;
    uint64_t table = *root & ADDR;
    if (table == u->pass && create) {
        if (!(table = pmm_alloc())) return NULL;
        memcpy(phys_to_virt(table), phys_to_virt(u->pass), PAGE_SIZE);
        if (!intel.coherent) iommu_cache_flush(phys_to_virt(table), PAGE_SIZE);
        iommu_entry_write(root, table | CTX_P);
    }
    return (volatile uint64_t *)phys_to_virt(table) + (id & 0xFF) * 2;
}

static void context_write(volatile uint64_t *e, uint64_t lo, uint64_t hi) {
    e[0] = 0; /* present last */
    e[1] = hi;
    e[0] = lo;
    if (!intel.coherent) iommu_cache_flush(e, 16);
}

static void contexts_write(const struct iommu_domain *dom, uint64_t lo, uint64_t hi) {
    for (size_t i = 0; i < nunits; i++)
        for (unsigned n = 0; n < dom->nids; n++) context_write(context_entry(&units[i], dom->ids[n], false), lo, hi);
}

/* ---- interrupts ---- */
static void irte_write(unsigned index, uint64_t lo, uint64_t hi) {
    volatile uint64_t *e = (volatile uint64_t *)phys_to_virt(irt) + index * 2;
    e[0] = 0; /* present last */
    e[1] = hi;
    e[0] = lo;
    if (!intel.coherent) iommu_cache_flush(e, 16);
}

/* The destination field (9.9): an xAPIC id in bits 47:40, or an x2APIC one in 63:32. */
static uint64_t irte_dest(uint32_t dest) { return x2 ? (uint64_t)dest << 32 : (uint64_t)(dest & 0xFF) << 40; }

/* What an entry checks the sender against: d's requester ID, or the buses of its IDs when a
   bridge sends its requests as its own. 0 without memory. */
static uint64_t irte_source(const struct pci_device *d) {
    unsigned n;
    uint16_t *ids = iommu_requester_ids(d, &n);
    if (!ids) return 0;
    uint64_t source = IRTE_SID(ids[0], 1);
    if (n > 1) {
        uint8_t first = 0xFF, last = 0;
        for (unsigned i = 0; i < n; i++) {
            if (ids[i] >> 8 < first) first = ids[i] >> 8;
            if (ids[i] >> 8 > last) last = ids[i] >> 8;
        }
        source = IRTE_SID((uint16_t)(first << 8 | last), 2);
    }
    kfree(ids);
    return source;
}

static int msi_block(const struct pci_device *d, bool create) {
    int free = -1;
    for (int b = 0; b < MSI_BLOCKS; b++) {
        if (msi_owner[b] == d) return b;
        if (!msi_owner[b] && free < 0) free = b;
    }
    if (!create) return -1;
    if (free < 0) {
        kprintf("iommu: no interrupt remapping entries left for %02x:%02x.%u\n", d->bus, d->dev, d->func);
        return -1;
    }
    msi_owner[free] = d;
    return free;
}

static bool msi_release(const struct pci_device *d) {
    int b = msi_block(d, false);
    if (b < 0) return true;
    unsigned first = IOAPIC_ENTRIES + (unsigned)b * MSI_BLOCK;
    for (unsigned i = 0; i < MSI_BLOCK; i++) irte_write(first + i, 0, 0);
    uint64_t f = irq_save();
    send_iec(first, MSI_BLOCK_LOG2);
    bool ok = sync();
    irq_restore(f);
    if (ok) msi_owner[b] = NULL; /* else kept: an entry may still be cached */
    return ok;
}

/* Message n of d's block arrives through entry block + index + n: the address carries the
   block entry of message 0, the data the subhandle the function adds the message number to. */
static bool intel_msi(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                      uint32_t *address, uint32_t *data) {
    if (dest > 0xFF) return false;
    if (!remap) {
        *address = iommu_msi_plain(dest);
        *data = vector;
        return true;
    }
    int b = msi_block(d, true);
    uint64_t source = b < 0 ? 0 : irte_source(d);
    if (!source || index + count > MSI_BLOCK) return false;
    unsigned first = IOAPIC_ENTRIES + (unsigned)b * MSI_BLOCK, at = first + index;
    for (unsigned i = 0; i < count; i++) irte_write(at + i, IRTE_P | IRTE_VECTOR(vector + i) | irte_dest(dest), source);
    uint64_t f = irq_save();
    send_iec(first, MSI_BLOCK_LOG2);
    bool ok = sync();
    irq_restore(f);
    *address = 0xFEE00000u | (at & 0x7FFF) << 5 | 1u << 4 | 1u << 3 | (at >> 15 & 1) << 2; /* remappable, SHV */
    *data = 0;
    return ok;
}

static bool intel_msi_clear(struct pci_device *d) { return !remap || msi_release(d); }

/* Pin `gsi` is entry gsi; the IRTE takes what the plain entry said (delivery mode, logical
   destination, trigger, vector, 8-bit destination), the pin keeps polarity, trigger, vector
   (for the EOI of a level pin) and mask, with delivery mode 000. */
static bool intel_ioapic(uint8_t ioapic_id, uint32_t gsi, uint64_t *rte) {
    if (!remap) return true;
    if (gsi >= IOAPIC_ENTRIES) {
        kprintf("iommu: GSI %u is beyond the IOAPIC entries: left masked\n", gsi);
        return false;
    }
    uint64_t source = 0, v = *rte;
    for (size_t i = 0; i < nioapics; i++)
        if (ioapics[i].id == ioapic_id) source = IRTE_SID(ioapics[i].sid, 1);
    if (!source) {
        kprintf("iommu: IOAPIC %u is under no VT-d unit: GSI %u left masked\n", ioapic_id, gsi);
        return false;
    }
    irte_write(gsi, IRTE_P | (v >> 11 & 1 ? IRTE_DM : 0) | (v >> 15 & 1 ? IRTE_TM : 0) | IRTE_DLM(v >> 8 & 7) |
                        IRTE_VECTOR(v) | irte_dest((uint32_t)(v >> 56)), source);
    uint64_t f = irq_save();
    send_iec(gsi, 0);
    bool ok = sync();
    irq_restore(f);
    if (!ok) {
        kprintf("iommu: the units did not confirm the entry of GSI %u: left masked\n", gsi);
        return false;
    }
    *rte = (v & RTE_KEPT) | RTE_REMAPPABLE | (uint64_t)(gsi & 0x7FFF) << 49 | (uint64_t)(gsi >> 15 & 1) << 11;
    return true;
}

/* ---- domains ---- */
static uint64_t intel_table(uint64_t phys, unsigned level) {
    (void)level;
    return phys | SL_R | SL_W;
}
static uint64_t intel_leaf(uint64_t phys, unsigned level, unsigned access) {
    return phys | (access & IOMMU_READ ? SL_R : 0) | (access & IOMMU_WRITE ? SL_W : 0) | (level > 1 ? SL_PS : 0);
}

/* A present context entry does not change in place: it goes not present, the caches drop it,
   then it comes back pointing at the domain. */
static bool intel_attach(struct iommu_domain *dom) {
    for (size_t i = 0; i < nunits; i++) /* memory first: nothing changes for the group when there is none */
        for (unsigned n = 0; n < dom->nids; n++)
            if (!context_entry(&units[i], dom->ids[n], true)) {
                kprintf("iommu: no memory for a context table\n");
                return false;
            }
    uint64_t f = irq_save();
    dom->written = true;
    contexts_write(dom, 0, 0);
    send_context_flush();
    bool ok = sync();
    if (ok) {
        contexts_write(dom, CTX_P | dom->root, (uint64_t)(dom->levels - 2) | (uint64_t)dom->id << 8);
        send_context_flush();
        ok = sync();
    }
    irq_restore(f);
    return ok;
}

static bool intel_block(struct iommu_domain *dom) {
    bool ok = !remap || msi_release(dom->d);
    uint64_t f = irq_save();
    contexts_write(dom, 0, 0);
    send_context_flush();
    ok &= sync();
    irq_restore(f);
    return ok;
}

/* New entries need no invalidation but where the units cache not-present ones (CAP.CM); then
   only the write buffers are flushed, which an invalidation does itself (6.8). */
static bool intel_flush(struct iommu_domain *dom, bool unmapped) {
    uint64_t f = irq_save();
    bool ok;
    if (!unmapped && !caching_mode) {
        ok = write_buffers_flush();
    } else {
        send(INV_IOTLB_DOMAIN(dom->id));
        ok = sync();
    }
    irq_restore(f);
    return ok;
}

/* ---- the units ---- */
/* The queue may go off once it is quiet (6.5.2): empty, the last descriptor done a wait. Ours
   gets one now; one the firmware left on has to be empty without an error, as nothing is known
   of what it holds. */
static bool queue_quiet(struct unit *u) {
    if (u->queue_on) return sync_units((size_t)(u - units), 1);
    for (uint64_t t = iommu_deadline(10); (rd64(u, R_IQH) & 0x7FFF0) != (rd64(u, R_IQT) & 0x7FFF0); pause())
        if (iommu_expired(t)) return false;
    return !(rd32(u, R_FSTS) & FSTS_IQE);
}

/* Translation, interrupt remapping and the queue off, fault events masked: false when the unit
   did not confirm one. A queue that does not go quiet stays on. */
static bool unit_off(struct unit *u) {
    uint32_t sts = rd32(u, R_GSTS);
    bool ok = true;
    if (sts & GCMD_TE) ok &= command(u, GCMD_TE, false);
    if (sts & GCMD_IRE) ok &= command(u, GCMD_IRE, false);
    if (sts & GCMD_QIE) {
        if (!queue_quiet(u)) {
            kprintf("iommu: VT-d unit at %lx: its invalidation queue does not go quiet; it stays on\n", u->base);
            ok = false;
        } else if (command(u, GCMD_QIE, false)) {
            u->queue_on = false;
        } else {
            ok = false;
        }
    }
    wr32(u, R_FECTL, FECTL_IM);
    return ok;
}

static bool unit_tables(struct unit *u, uint32_t bsp) {
    if (!unit_off(u)) return false; /* also when the firmware left it on */
    wr64(u, R_RTADDR, u->root); /* legacy mode */
    bool ok = command(u, GCMD_SRTP, true) && queue_start(u);
    if (fault_vector >= 0 && bsp <= 0xFF) {
        wr32(u, R_FEDATA, (uint32_t)fault_vector);
        wr32(u, R_FEADDR, iommu_msi_plain(bsp));
        wr32(u, R_FEUADDR, 0);
        wr32(u, R_FECTL, 0);
    }
    if (ok && remap) {
        wr64(u, R_IRTA, irt | (x2 ? IRTA_EIME : 0) | IRT_S);
        ok = command(u, GCMD_SIRTP, true);
    }
    return ok;
}

/* cfi: compatibility-format interrupts still pass, until the IOAPIC entries are remappable. A
   unit that does not take CFI blocks (and records) a pin that fires in between; remapping goes
   on. In Extended Interrupt Mode they are blocked from the start (5.1.4): at boot only the
   firmware's ExtINT pin is unmasked. The global invalidations flush the write buffers the tables
   went through (6.8). Translation goes on only when intel.translating; the firmware's protected
   memory ranges go off either way, as every function's DMA is then to reach memory as it is. */
static bool units_on(bool cfi) {
    bool ok = true;
    uint32_t bsp = lapic_current_id();
    for (size_t i = 0; i < nunits; i++) ok &= unit_tables(&units[i], bsp);
    if (!ok) return false;
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_GLOBAL);
    if (remap) send(INV_IEC_GLOBAL);
    ok = sync();
    for (size_t i = 0; ok && i < nunits; i++) {
        struct unit *u = &units[i];
        if (remap && cfi && !x2 && !command(u, GCMD_CFI, true))
            kprintf("iommu: VT-d unit at %lx blocks the IOAPIC pins until they are remappable\n", u->base);
        if (remap) ok = command(u, GCMD_IRE, true);
        if (intel.translating) ok = ok && command(u, GCMD_TE, true);
        uint32_t pmen = rd32(u, R_PMEN);
        if (ok && u->cap & (CAP_PLMR | CAP_PHMR) && pmen & PMEN_EPM) { /* the firmware's protected ranges */
            wr32(u, R_PMEN, pmen & ~PMEN_EPM);
            for (uint64_t t = iommu_deadline(100); rd32(u, R_PMEN) & PMEN_PRS; pause())
                if (iommu_expired(t)) {
                    kprintf("iommu: VT-d unit at %lx did not confirm its protected memory ranges off\n", u->base);
                    break;
                }
        }
    }
    return ok;
}

static void intel_shutdown(void) {
    for (size_t i = 0; i < nunits; i++)
        if (!unit_off(&units[i])) kprintf("iommu: VT-d unit at %lx did not turn off\n", units[i].base);
}

static void intel_resume(void) {
    if (!units_on(false)) kprintf("iommu: VT-d did not come back after sleep\n");
}

static void tables_free(void) {
    for (size_t p = 0; sems && p < sem_pages; p++) pmm_free(sems + p * PAGE_SIZE);
    for (unsigned at = 0; irt && at < IRT_ENTRIES * 16; at += PAGE_SIZE) pmm_free(irt + at);
    for (size_t i = 0; i < nunits; i++) {
        if (units[i].root) pmm_free(units[i].root);
        if (units[i].pass) pmm_free(units[i].pass);
        if (units[i].queue) pmm_free(units[i].queue);
    }
    sems = irt = 0;
}

/* The units on with the tables made, translating or not: false leaves them off. A unit that
   keeps passing compatibility-format interrupts lets any device send any vector past the
   remapping table (5.1.4); where translation isolates claims it is not used then, without
   translation every device reaches all of memory anyway. */
static bool units_start(bool translate) {
    intel.translating = translate;
    bool ok = units_on(true);
    if (ok && remap) {
        ioapic_remap();
        for (size_t i = 0; !x2 && i < nunits; i++)
            if (!command(&units[i], GCMD_CFI, false)) {
                kprintf("iommu: VT-d unit at %lx keeps passing compatibility-format interrupts\n", units[i].base);
                ok &= !translate;
            }
    }
    if (!ok) {
        intel.translating = false;
        intel_shutdown(); /* iommu_init writes the IOAPIC entries back plain */
    }
    return ok;
}

/* translate false, or no page table format the units share: interrupt remapping alone, for a
   unit that requires it (ECAP.IRREQ: every interrupt message is blocked while it is off, 5.1.4).
   DMA then passes the units untranslated. */
static bool intel_start(bool translate) {
    translate &= intel.levels != 0;
    if (!translate && !irreq) return false;
    sem_pages = ALIGN_UP(nunits * 4, PAGE_SIZE) / PAGE_SIZE;
    sems = pmm_alloc_contig(sem_pages);
    irt = remap ? pmm_alloc_contig(IRT_ENTRIES * 16 / PAGE_SIZE) : 0;
    bool ok = sems && (irt || !remap), pass_all = true;
    for (size_t i = 0; i < nunits; i++) {
        units[i].root = pmm_alloc_zeroed();
        units[i].pass = pmm_alloc_zeroed();
        units[i].queue = pmm_alloc_zeroed();
        ok &= units[i].root && units[i].pass && units[i].queue;
        pass_all &= (units[i].ecap & ECAP_PT) != 0;
    }
    if (ok && translate && !pass_all) { /* all of RAM and the space below 4 GiB, in the largest pages every unit has */
        unsigned sllps = 0xF;
        for (size_t i = 0; i < nunits; i++) sllps &= CAP_SSLPS(units[i].cap);
        identity = iommu_domain_new(intel.levels, sllps & 2 ? 3 : sllps & 1 ? 2 : 1);
        uint64_t top = ALIGN_UP(pmm_highest_usable > (4ull << 30) ? pmm_highest_usable : 4ull << 30, 1ull << 30);
        ok = identity && iommu_map(identity, NULL, 0, top / PAGE_SIZE);
    }
    if (!ok) {
        kprintf("iommu: no memory for the VT-d tables\n");
        tables_free();
        return false;
    }
    memset(phys_to_virt(sems), 0, sem_pages * PAGE_SIZE);
    if (remap) memset(phys_to_virt(irt), 0, IRT_ENTRIES * 16);
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        uint64_t *pass = phys_to_virt(u->pass), *root = phys_to_virt(u->root);
        for (unsigned e = 0; e < 256; e++) {
            pass[e * 2] = pass_lo(u);
            pass[e * 2 + 1] = pass_hi(u);
            root[e * 2] = u->pass | CTX_P;
        }
        if (!intel.coherent) {
            iommu_cache_flush(pass, PAGE_SIZE);
            iommu_cache_flush(root, PAGE_SIZE);
        }
    }
    if (remap && !intel.coherent) iommu_cache_flush(phys_to_virt(irt), IRT_ENTRIES * 16);
    if (fault_vector < 0 && (fault_vector = irq_vector_alloc(1, 1)) >= 0) vector_register(fault_vector, fault_isr);
    uint64_t f = irq_save();
    ok = units_start(translate);
    if (!ok && translate && irreq) {
        kprintf("iommu: VT-d did not start translating; a unit requires interrupt remapping: trying that alone\n");
        ok = units_start(false);
    }
    irq_restore(f);
    if (!ok) {
        if (irreq) kprintf("iommu: a VT-d unit that requires interrupt remapping is off: it blocks every interrupt message\n");
        return false; /* the tables stay: a unit may still read them */
    }
    if (!intel.translating) {
        kprintf("iommu: VT-d remaps interrupts only%s, %lu unit%s\n", x2 ? " (x2APIC format)" : "",
                (unsigned long)nunits, nunits == 1 ? "" : "s");
        return true;
    }
    kprintf("iommu: VT-d on, %lu unit%s, %u-level tables%s, interrupt remapping %s, %u reserved range%s: every "
            "function passes through until an app claims it\n", (unsigned long)nunits, nunits == 1 ? "" : "s",
            intel.levels, pass_all ? "" : " (identity domain for units without pass-through)",
            remap ? (x2 ? "on (x2APIC format)" : "on") : dmar_flags & 1 ? "not possible on these units" : "not offered by the firmware",
            iommu_reserved_count(), iommu_reserved_count() == 1 ? "" : "s");
    if (!remap) kprintf("iommu: interrupts are not remapped: a device given to an app can send any vector\n");
    return true;
}

static struct iommu_hw intel = {
    .name = "VT-d",
    .table = intel_table,
    .leaf = intel_leaf,
    .start = intel_start,
    .attach = intel_attach,
    .block = intel_block,
    .flush = intel_flush,
    .msi = intel_msi,
    .msi_clear = intel_msi_clear,
    .ioapic = intel_ioapic,
    .resume = intel_resume,
    .shutdown = intel_shutdown,
};

const struct iommu_hw *iommu_intel_probe(void) {
    if (!parse_dmar()) return NULL;
    if (parse_lost) {
        kprintf("iommu: no memory to keep the DMAR: VT-d not used\n");
        return NULL;
    }
    unsigned sagaw = 0x1F, domains = 1u << 16;
    bool qi = true, eim = true, eimer = false;
    intel.coherent = true;
    remap = (dmar_flags & 1) != 0;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (!(u->regs = vmm_map_mmio(u->base, (uint64_t)u->pages * PAGE_SIZE))) {
            kprintf("iommu: the registers of the VT-d unit at %lx cannot be mapped: VT-d not used\n", u->base);
            return NULL;
        }
        u->cap = rd64(u, R_CAP);
        u->ecap = rd64(u, R_ECAP);
        uint64_t need = ALIGN_UP(CAP_FRO(u->cap) + CAP_NFR(u->cap) * 16, PAGE_SIZE);
        if (need > (uint64_t)u->pages * PAGE_SIZE && !(u->regs = vmm_map_mmio(u->base, need))) {
            kprintf("iommu: the fault records of the VT-d unit at %lx cannot be mapped: VT-d not used\n", u->base);
            return NULL;
        }
        uint32_t ver = rd32(u, R_VER);
        kprintf("iommu: VT-d unit at %lx, version %u.%u, capabilities %lx, extended %lx\n", u->base, ver >> 4 & 15,
                ver & 15, u->cap, u->ecap);
        sagaw &= CAP_SAGAW(u->cap);
        if (1u << (4 + 2 * CAP_ND(u->cap)) < domains) domains = 1u << (4 + 2 * CAP_ND(u->cap));
        qi &= (u->ecap & ECAP_QI) != 0;
        remap &= (u->ecap & ECAP_IR) != 0;
        eim &= (u->ecap & ECAP_EIM) != 0;
        eimer |= (u->ecap & ECAP_EIMER) != 0;
        irreq |= (u->ecap & ECAP_IRREQ) != 0;
        if (ECAP_MHMV(u->ecap) < iec_mask) iec_mask = ECAP_MHMV(u->ecap);
        intel.coherent &= (u->ecap & ECAP_C) != 0;
        caching_mode |= (u->cap & CAP_CM) != 0;
        write_buffers |= (u->cap & CAP_RWBF) != 0;
    }
    /* 4 levels (48 bits); 3 when every address fits 39 bits and a unit lacks 4, else 5 */
    bool fits39 = pmm_highest_usable <= 1ull << 39;
    intel.levels = sagaw & 4 ? 4 : sagaw & 2 && fits39 ? 3 : sagaw & 8 ? 5 : 0;
    intel.domains = domains > 0xFFFF ? 0xFFFF : (uint16_t)domains;
    if (remap && !ioapics_described()) {
        kprintf("iommu: the IOAPICs' requester IDs are not known: interrupts are not remapped\n");
        remap = false;
    }
    /* A unit that requires Extended Interrupt Mode blocks every interrupt without it (5.1.4),
       and takes it with the CPUs in x2APIC mode only (5.1.6). */
    if (remap && eimer && (!eim || !lapic_x2apic())) {
        kprintf("iommu: a VT-d unit remaps interrupts in x2APIC format only, which %s: interrupts are not remapped\n",
                eim ? "the CPUs do not use" : "another unit lacks");
        remap = false;
    }
    x2 = remap && eimer;
    /* A unit that requires interrupt remapping blocks every interrupt message while it is off
       (5.1.4): it is started to remap them even where nothing else of VT-d is used. Without the
       queue (which remapping needs for its invalidations) or the remapping it is left as the
       firmware set it. */
    const char *unused = !qi ? "a VT-d unit has no invalidation queue"
                       : irreq && !remap ? "a VT-d unit requires interrupt remapping, which these units cannot give"
                       : !intel.levels && !irreq ? "the VT-d units share no page table format" : NULL;
    if (unused) {
        kprintf("iommu: %s: VT-d not used%s\n", unused,
                irreq ? "; unless the firmware left remapping on, every interrupt message is blocked" : "");
        return NULL;
    }
    if (!intel.levels) kprintf("iommu: the VT-d units share no page table format: DMA is not translated\n");
    return &intel;
}
