/* Intel VT-d, per the Intel Virtualization Technology for Directed I/O Architecture
   Specification: the DMAR table, legacy-mode root and context tables, second-stage page tables,
   the invalidation queue, fault recording and interrupt remapping.
   Every unit gets the same picture. Its root table points every bus at one context table whose
   entries let a function through (pass-through, or the identity domain on a unit without it);
   a bus with a claimed function gets a context table of its own in each unit, where that
   function's entry points at the claim's domain and, once blocked, is not present.
   Interrupt remapping, when every unit has it, takes every interrupt: compatibility-format
   interrupts are refused (they would let any device send any vector), so each MSI and IOAPIC
   pin is an entry of one shared table, checked against the requester ID of its source.
   Entries 0..255 are the IOAPIC pins by GSI, then 32 for each function with MSI or MSI-X on. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define MAX_UNITS   8
#define MAX_IOAPICS 8

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

#define CAP_RWBF     (1ull << 4)
#define CAP_PLMR     (1ull << 5)
#define CAP_PHMR     (1ull << 6)
#define CAP_CM       (1ull << 7)
#define CAP_ND(c)    ((unsigned)(c) & 7)
#define CAP_SAGAW(c) ((unsigned)((c) >> 8) & 0x1F)
#define CAP_FRO(c)   (((unsigned)((c) >> 24) & 0x3FF) * 16)
#define CAP_SSLPS(c) ((unsigned)((c) >> 34) & 0xF)
#define CAP_NFR(c)   (((unsigned)((c) >> 40) & 0xFF) + 1)
#define ECAP_C  (1ull << 0)
#define ECAP_QI (1ull << 1)
#define ECAP_IR (1ull << 3)
#define ECAP_PT (1ull << 6)

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
#define INV_IEC_GLOBAL       4ull
#define INV_IEC(index, log2) (4ull | 1ull << 4 | (uint64_t)(log2) << 27 | (uint64_t)(index) << 32)
#define INV_WAIT(token)      (5ull | 1ull << 5 | 1ull << 6 | (uint64_t)(token) << 32) /* status write, fence */

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
#define IRTE_DEST(d)   ((uint64_t)(d) << 40) /* xAPIC: 8 bits */
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
};
static struct unit units[MAX_UNITS];
static unsigned nunits;
static struct {
    uint8_t id;
    uint16_t sid;
} ioapics[MAX_IOAPICS]; /* the requester IDs the IOAPICs send with */
static unsigned nioapics;
static uint8_t dmar_flags;
static bool remap, caching_mode, write_buffers;
static struct iommu_domain *identity; /* units without pass-through */
static uint64_t sems, irt;
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
    }
}

static void ioapic_scope(const uint8_t *s, int id, void *arg) {
    (void)arg;
    if (s[0] == 3 && nioapics < MAX_IOAPICS) {
        ioapics[nioapics].id = s[4];
        ioapics[nioapics++].sid = (uint16_t)id;
    }
}

/* RMRR: memory the functions in its scopes reach by DMA from the firmware's start (a USB
   controller's legacy emulation, a graphics device's stolen memory). A bridge's scope is
   everything below it too. */
static void rmrr_scope(const uint8_t *s, int id, void *arg) {
    const uint64_t *range = arg;
    if (s[0] != 1 && s[0] != 2) return;
    iommu_reserved((uint16_t)id, (uint16_t)id, range[0], range[1]);
    if (s[0] == 2) {
        uint32_t r = pci_read32((uint8_t)(id >> 8), (id >> 3) & 31, id & 7, 0x18);
        iommu_reserved((uint16_t)((r >> 8 & 0xFF) << 8), (uint16_t)((r >> 16 & 0xFF) << 8 | 0xFF), range[0], range[1]);
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
            } else if (base && nunits < MAX_UNITS) {
                units[nunits].base = base;
                units[nunits++].pages = 1u << (b[off + 5] & 15);
                scopes(b, off + 16, off + len, ioapic_scope, NULL);
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

/* ---- commands ---- */
static bool command(struct unit *u, uint32_t bit, bool on) {
    uint32_t sts = rd32(u, R_GSTS) & ~GSTS_ONESHOT;
    wr32(u, R_GCMD, on ? sts | bit : sts & ~bit);
    for (uint64_t t = iommu_deadline(100); !(rd32(u, R_GSTS) & bit) == on; pause())
        if (iommu_expired(t)) {
            kprintf("iommu: VT-d unit at %lx did not %s %08x\n", u->base, on ? "set" : "clear", bit);
            return false;
        }
    return true;
}

static void write_buffers_flush(void) {
    if (!write_buffers) return;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (!(u->cap & CAP_RWBF)) continue;
        wr32(u, R_GCMD, (rd32(u, R_GSTS) & ~GSTS_ONESHOT) | GCMD_WBF);
        for (uint64_t t = iommu_deadline(100); rd32(u, R_GSTS) & GCMD_WBF && !iommu_expired(t);) pause();
    }
}

static bool queue_start(struct unit *u) {
    wr64(u, R_IQT, 0);
    u->tail = 0;
    wr64(u, R_IQA, u->queue); /* 128-bit descriptors, one page */
    return command(u, GCMD_QIE, true);
}

static bool submit(struct unit *u, uint64_t lo, uint64_t hi) {
    uint32_t next = (u->tail + 16) % PAGE_SIZE;
    for (uint64_t t = iommu_deadline(100); (rd64(u, R_IQH) & 0x7FFF0) == next; pause())
        if (iommu_expired(t)) return false;
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)phys_to_virt(u->queue) + u->tail);
    slot[0] = lo;
    slot[1] = hi;
    if (!intel.coherent) iommu_cache_flush(slot, 16);
    u->tail = next;
    wr64(u, R_IQT, next);
    return true;
}

static void send(uint64_t lo) {
    for (unsigned i = 0; i < nunits; i++) submit(&units[i], lo, 0);
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
    for (unsigned i = 0; i < nunits; i++) faults_drain(&units[i]);
}

/* Waits until every unit has done every command sent before. A unit that rejected one has its
   queue started again, empty. */
static bool sync(void) {
    volatile uint32_t *word = phys_to_virt(sems);
    if (!++token) token++;
    bool ok = true;
    for (unsigned i = 0; i < nunits; i++) {
        word[i] = 0;
        ok &= submit(&units[i], INV_WAIT(token), sems + i * 4);
    }
    for (unsigned i = 0; ok && i < nunits; i++) {
        struct unit *u = &units[i];
        for (uint64_t t = iommu_deadline(100); word[i] != token; pause()) {
            if (rd32(u, R_FSTS) & FSTS_IQE) {
                kprintf("iommu: VT-d unit at %lx rejected a command\n", u->base);
                command(u, GCMD_QIE, false);
                wr32(u, R_FSTS, FSTS_IQE);
                queue_start(u);
                ok = false;
                break;
            }
            if (iommu_expired(t)) {
                kprintf("iommu: VT-d unit at %lx did not finish its commands\n", u->base);
                ok = false;
                break;
            }
        }
    }
    for (unsigned i = 0; i < nunits; i++) faults_drain(&units[i]);
    return ok;
}

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
    for (unsigned i = 0; i < nunits; i++)
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

/* What an entry checks the sender against: d's requester ID, or the buses of its IDs when a
   bridge sends its requests as its own. */
static uint64_t irte_source(const struct pci_device *d) {
    uint16_t ids[IOMMU_IDS];
    unsigned n = iommu_requester_ids(d, ids);
    if (n == 1) return IRTE_SID(ids[0], 1);
    uint8_t first = 0xFF, last = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ids[i] >> 8 < first) first = ids[i] >> 8;
        if (ids[i] >> 8 > last) last = ids[i] >> 8;
    }
    return n ? IRTE_SID((uint16_t)(first << 8 | last), 2) : 0;
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
    send(INV_IEC(first, MSI_BLOCK_LOG2));
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
    if (b < 0 || index + count > MSI_BLOCK) return false;
    unsigned first = IOAPIC_ENTRIES + (unsigned)b * MSI_BLOCK, at = first + index;
    uint64_t source = irte_source(d);
    for (unsigned i = 0; i < count; i++) irte_write(at + i, IRTE_P | IRTE_VECTOR(vector + i) | IRTE_DEST(dest), source);
    uint64_t f = irq_save();
    send(INV_IEC(first, MSI_BLOCK_LOG2));
    bool ok = sync();
    irq_restore(f);
    *address = 0xFEE00000u | (at & 0x7FFF) << 5 | 1u << 4 | 1u << 3 | (at >> 15 & 1) << 2; /* remappable, SHV */
    *data = 0;
    return ok;
}

static void intel_msi_clear(struct pci_device *d) {
    if (remap) msi_release(d);
}

/* Pin `gsi` is entry gsi; the IRTE takes what the plain entry said (delivery mode, logical
   destination, trigger, vector, 8-bit destination), the pin keeps polarity, trigger, vector
   (for the EOI of a level pin) and mask, with delivery mode 000. */
static uint64_t intel_ioapic(uint8_t ioapic_id, uint32_t gsi, uint64_t rte) {
    if (!remap) return rte;
    if (gsi >= IOAPIC_ENTRIES) {
        kprintf("iommu: GSI %u is beyond the IOAPIC entries: left masked\n", gsi);
        return rte | 1ull << 16;
    }
    uint64_t source = 0;
    for (unsigned i = 0; i < nioapics; i++)
        if (ioapics[i].id == ioapic_id) source = IRTE_SID(ioapics[i].sid, 1);
    irte_write(gsi, IRTE_P | (rte >> 11 & 1 ? IRTE_DM : 0) | (rte >> 15 & 1 ? IRTE_TM : 0) |
                        IRTE_DLM(rte >> 8 & 7) | IRTE_VECTOR(rte) | IRTE_DEST(rte >> 56), source);
    uint64_t f = irq_save();
    send(INV_IEC(gsi, 0));
    sync();
    irq_restore(f);
    return (rte & RTE_KEPT) | RTE_REMAPPABLE | (uint64_t)(gsi & 0x7FFF) << 49 | (uint64_t)(gsi >> 15 & 1) << 11;
}

/* ---- domains ---- */
static uint64_t intel_table(uint64_t phys, unsigned level) {
    (void)level;
    return phys | SL_R | SL_W;
}
static uint64_t intel_leaf(uint64_t phys, unsigned level) { return phys | SL_R | SL_W | (level > 1 ? SL_PS : 0); }

/* A present context entry does not change in place: it goes not present, the caches drop it,
   then it comes back pointing at the domain. */
static bool intel_attach(struct iommu_domain *dom) {
    for (unsigned i = 0; i < nunits; i++) /* memory first: nothing changes when there is none */
        for (unsigned n = 0; n < dom->nids; n++)
            if (!context_entry(&units[i], dom->ids[n], true)) return false;
    uint64_t f = irq_save();
    contexts_write(dom, 0, 0);
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_DOMAIN(PASS_DOMAIN));
    sync();
    write_buffers_flush();
    contexts_write(dom, CTX_P | dom->root, (uint64_t)(dom->levels - 2) | (uint64_t)dom->id << 8);
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_DOMAIN(dom->id));
    sync();
    irq_restore(f);
    return true;
}

static bool intel_block(struct iommu_domain *dom) {
    bool ok = !remap || msi_release(dom->d);
    uint64_t f = irq_save();
    contexts_write(dom, 0, 0);
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_DOMAIN(dom->id));
    ok &= sync();
    irq_restore(f);
    return ok;
}

static bool intel_flush(struct iommu_domain *dom, bool unmapped) {
    write_buffers_flush();
    if (!unmapped && !caching_mode) return true;
    uint64_t f = irq_save();
    send(INV_IOTLB_DOMAIN(dom->id));
    bool ok = sync();
    irq_restore(f);
    return ok;
}

/* ---- the units ---- */
static void unit_off(struct unit *u) {
    uint32_t sts = rd32(u, R_GSTS);
    if (sts & GCMD_TE) command(u, GCMD_TE, false);
    if (sts & GCMD_IRE) command(u, GCMD_IRE, false);
    if (sts & GCMD_QIE) command(u, GCMD_QIE, false);
    wr32(u, R_FECTL, FECTL_IM);
}

static bool unit_tables(struct unit *u, uint32_t bsp) {
    unit_off(u); /* also when the firmware left it on */
    wr64(u, R_RTADDR, u->root); /* legacy mode */
    bool ok = command(u, GCMD_SRTP, true) && queue_start(u);
    if (fault_vector >= 0 && bsp <= 0xFF) {
        wr32(u, R_FEDATA, (uint32_t)fault_vector);
        wr32(u, R_FEADDR, iommu_msi_plain(bsp));
        wr32(u, R_FEUADDR, 0);
        wr32(u, R_FECTL, 0);
    }
    if (ok && remap) {
        wr64(u, R_IRTA, irt | IRT_S); /* xAPIC destinations */
        ok = command(u, GCMD_SIRTP, true);
    }
    return ok;
}

/* cfi: compatibility-format interrupts still pass, until the IOAPIC entries are remappable. A
   unit that does not take CFI blocks (and records) a pin that fires in between; remapping goes on. */
static bool units_on(bool cfi) {
    bool ok = true;
    uint32_t bsp = lapic_current_id();
    for (unsigned i = 0; i < nunits; i++) ok &= unit_tables(&units[i], bsp);
    if (!ok) return false;
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_GLOBAL);
    if (remap) send(INV_IEC_GLOBAL);
    ok = sync();
    for (unsigned i = 0; ok && i < nunits; i++) {
        struct unit *u = &units[i];
        if (remap && cfi) command(u, GCMD_CFI, true);
        if (remap) ok = command(u, GCMD_IRE, true);
        ok = ok && command(u, GCMD_TE, true);
        if (ok && u->cap & (CAP_PLMR | CAP_PHMR) && rd32(u, R_PMEN) & PMEN_EPM) { /* the firmware's protected ranges */
            wr32(u, R_PMEN, 0);
            for (uint64_t t = iommu_deadline(100); rd32(u, R_PMEN) & PMEN_PRS && !iommu_expired(t);) pause();
        }
    }
    return ok;
}

static void intel_shutdown(void) {
    for (unsigned i = 0; i < nunits; i++) unit_off(&units[i]);
}

static void intel_resume(void) {
    if (!units_on(false)) kprintf("iommu: VT-d did not come back after sleep\n");
}

static void tables_free(void) {
    if (sems) pmm_free(sems);
    for (unsigned at = 0; irt && at < IRT_ENTRIES * 16; at += PAGE_SIZE) pmm_free(irt + at);
    for (unsigned i = 0; i < nunits; i++) {
        if (units[i].root) pmm_free(units[i].root);
        if (units[i].pass) pmm_free(units[i].pass);
        if (units[i].queue) pmm_free(units[i].queue);
    }
    sems = irt = 0;
}

static bool intel_start(void) {
    sems = pmm_alloc_zeroed();
    irt = remap ? pmm_alloc_contig(IRT_ENTRIES * 16 / PAGE_SIZE) : 0;
    bool ok = sems && (irt || !remap), pass_all = true;
    for (unsigned i = 0; i < nunits; i++) {
        units[i].root = pmm_alloc_zeroed();
        units[i].pass = pmm_alloc_zeroed();
        units[i].queue = pmm_alloc_zeroed();
        ok &= units[i].root && units[i].pass && units[i].queue;
        pass_all &= (units[i].ecap & ECAP_PT) != 0;
    }
    if (ok && !pass_all) { /* all of RAM and the space below 4 GiB, in the largest pages every unit has */
        unsigned sllps = 0xF;
        for (unsigned i = 0; i < nunits; i++) sllps &= CAP_SSLPS(units[i].cap);
        identity = iommu_domain_new(intel.levels, sllps & 2 ? 3 : sllps & 1 ? 2 : 1);
        uint64_t top = ALIGN_UP(pmm_highest_usable > (4ull << 30) ? pmm_highest_usable : 4ull << 30, 1ull << 30);
        ok = identity && iommu_map(identity, NULL, 0, top / PAGE_SIZE);
    }
    if (!ok) {
        kprintf("iommu: no memory for the VT-d tables\n");
        tables_free();
        return false;
    }
    if (remap) memset(phys_to_virt(irt), 0, IRT_ENTRIES * 16);
    for (unsigned i = 0; i < nunits; i++) {
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
    ok = units_on(true);
    if (ok && remap) {
        ioapic_remap();
        for (unsigned i = 0; i < nunits; i++) ok &= command(&units[i], GCMD_CFI, false);
    }
    irq_restore(f);
    if (!ok) {
        intel_shutdown();
        return false; /* the tables stay: a unit may still read them */
    }
    kprintf("iommu: VT-d on, %u unit%s, %u-level tables%s, interrupt remapping %s, %u reserved range%s: every "
            "function passes through until an app claims it\n", nunits, nunits == 1 ? "" : "s", intel.levels,
            pass_all ? "" : " (identity domain for units without pass-through)",
            remap ? "on" : dmar_flags & 1 ? "missing on a unit" : "not offered by the firmware",
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
    unsigned sagaw = 0x1F, domains = 1u << 16;
    bool qi = true;
    intel.coherent = true;
    remap = (dmar_flags & 1) != 0;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        u->regs = vmm_map_mmio(u->base, (uint64_t)u->pages * PAGE_SIZE);
        u->cap = rd64(u, R_CAP);
        u->ecap = rd64(u, R_ECAP);
        uint64_t need = ALIGN_UP(CAP_FRO(u->cap) + CAP_NFR(u->cap) * 16, PAGE_SIZE);
        if (need > (uint64_t)u->pages * PAGE_SIZE) u->regs = vmm_map_mmio(u->base, need);
        uint32_t ver = rd32(u, R_VER);
        kprintf("iommu: VT-d unit at %lx, version %u.%u, capabilities %lx, extended %lx\n", u->base, ver >> 4 & 15,
                ver & 15, u->cap, u->ecap);
        sagaw &= CAP_SAGAW(u->cap);
        if (1u << (4 + 2 * CAP_ND(u->cap)) < domains) domains = 1u << (4 + 2 * CAP_ND(u->cap));
        qi &= (u->ecap & ECAP_QI) != 0;
        remap &= (u->ecap & ECAP_IR) != 0;
        intel.coherent &= (u->ecap & ECAP_C) != 0;
        caching_mode |= (u->cap & CAP_CM) != 0;
        write_buffers |= (u->cap & CAP_RWBF) != 0;
    }
    /* 4 levels (48 bits); 3 when every address fits 39 bits and a unit lacks 4, else 5 */
    bool fits39 = pmm_highest_usable <= 1ull << 39;
    intel.levels = sagaw & 4 ? 4 : sagaw & 2 && fits39 ? 3 : sagaw & 8 ? 5 : 0;
    intel.domains = domains > 0xFFFF ? 0xFFFF : (uint16_t)domains;
    if (!qi) {
        kprintf("iommu: a VT-d unit has no invalidation queue: VT-d not used\n");
        return NULL;
    }
    if (!intel.levels) {
        kprintf("iommu: the VT-d units share no page table format: VT-d not used\n");
        return NULL;
    }
    return &intel;
}
