/* Intel VT-d, per the Intel Virtualization Technology for Directed I/O Architecture
   Specification: the DMAR table, legacy-mode root and context tables, second-stage page tables,
   the invalidation queue, fault recording and interrupt remapping.
   Every unit gets the same picture. Its root table points every bus at one context table whose
   entries let a function through (pass-through, or the identity domain on a unit without it);
   a bus with a claimed function gets a context table of its own in each unit, where the entries
   of the claim's group point at the claim's domain and, once blocked, are not present.
   Interrupt remapping, when every unit has it, takes every interrupt: compatibility-format
   interrupts are refused (they would let any device send any vector), so each MSI and IOAPIC
   pin is an entry of the segment's table, the largest the format has (IRTA.S 15: 64K entries),
   checked against the requester ID of its source. Its entries are given out as the sources come
   (dev/iommu.c): a run for each IOAPIC's pins, one for each MSI block, one for each MSI-X entry.
   The units remap in Extended Interrupt Mode (IRTA.EIME, 32-bit destinations, each unit's EIM,
   9.10) where the CPUs are in x2APIC mode or move to it once the units remap so (arch/apic.h,
   apic_x2_waits: SDM 13.12.7 asks for EIME before x2APIC mode), else with 8-bit xAPIC
   destinations. EIME is set with the table's pointer and never changed while remapping is on
   (11.4.10). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/apic.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"

/* registers */
#define R_VER     0x00
#define R_CAP     0x08
#define R_ECAP    0x10
#define R_GCMD    0x18
#define R_GSTS    0x1C
#define R_RTADDR  0x20
#define R_CCMD    0x28
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
#define R_ECRSP   0x410

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
#define FSTS_ICE (1u << 5)
#define FSTS_ITE (1u << 6)
#define FSTS_QUEUE_ERRORS (FSTS_IQE | FSTS_ICE | FSTS_ITE)
/* IQERCD (11.4.9.9): the IQE's cause, and the requester of an ITE/ICE (0: not recorded) */
#define IQEI(r)   ((unsigned)(r) & 0xF)
#define ITESID(r) ((unsigned)((r) >> 32) & 0xFFFF)
#define ICESID(r) ((unsigned)((r) >> 48) & 0xFFFF)
#define FECTL_IM (1u << 31)
#define PMEN_EPM (1u << 31)
#define PMEN_PRS (1u << 0)
#define IRTA_EIME (1ull << 11)

#define VER_MAJOR(v) (((unsigned)(v) >> 4) & 0xF)
#define CAP_RWBF     (1ull << 4)
#define CAP_PLMR     (1ull << 5)
#define CAP_PHMR     (1ull << 6)
#define CAP_CM       (1ull << 7)
#define CAP_ND(c)    ((unsigned)(c) & 7)
#define CAP_SAGAW(c) ((unsigned)((c) >> 8) & 0x1F)
#define CAP_MGAW(c)  (((unsigned)((c) >> 16) & 0x3F) + 1)
#define CAP_FRO(c)   (((unsigned)((c) >> 24) & 0x3FF) * 16)
#define CAP_SSLPS(c) ((unsigned)((c) >> 34) & 0xF)
#define CAP_NFR(c)   (((unsigned)((c) >> 40) & 0xFF) + 1)
#define CAP_DWD      (1ull << 54)
#define CAP_DRD      (1ull << 55)
#define CAP_ECMDS    (1ull << 61)
#define ECAP_C     (1ull << 0)
#define ECAP_QI    (1ull << 1)
#define ECAP_IR    (1ull << 3)
#define ECAP_EIM   (1ull << 4)
#define ECAP_PT    (1ull << 6)
#define ECAP_MHMV(e) ((unsigned)((e) >> 20) & 0xF)
#define ECAP_IRO(e) (((unsigned)((e) >> 8) & 0x3FF) * 16)
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
#define INV_FENCE            (5ull | 1ull << 6) /* a wait with neither status write nor interrupt (6.5.2.9) */
#define IQ_PTR_MASK          0x7FFF0ull /* IQH/IQT bits 18:4, independent of queue size */
#define IQA_DW               (1ull << 11)
#define IQA_QS               7u
#define INV_BYTES            16u
#define DRHD_SIZE_MASK       0x0Fu /* 8.3: log2 of 4KiB register pages; upper four bits reserved */

/* the DMAR's fixed fields (8.1): Host Address Width, Flags, then the remapping structures */
#define DMAR_HAW               36
#define DMAR_FLAGS             37
#define DMAR_HEADER            48
#define DMAR_INTR_REMAP        (1u << 0)
#define DMAR_DMA_REMAP_OPT_OUT (1u << 3)

/* interrupt remapping table: 2^(S+1) entries of 16 bytes */
#define IRT_S          15
#define IRT_ENTRIES    (1u << (IRT_S + 1))
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
    uint16_t segment;   /* the PCI segment group it serves (DRHD) */
    volatile uint8_t *regs;
    uint64_t reg_bytes; /* the DRHD's complete register aperture, not a license to map beyond it */
    bool readable;      /* mapped, and its registers describe a VT-d unit inside that aperture */
    uint64_t cap, ecap;
    uint64_t root;  /* root table */
    uint64_t pass;  /* the context table of every bus without a claimed function */
    uint64_t queue; /* invalidation queue: 256 descriptors */
    uint64_t handoff; /* owned replacement for an inherited queue, followed by its status page */
    unsigned handoff_config; /* inherited IQA.QS/DW: offsets stay valid during the live IQA change */
    uint32_t handoff_tail, handoff_token;
    /* The unit's queue, registers, fault records, the flags below and its root and context tables:
       a leaf, taken in the fault handler too. A caller's descriptors and the wait after them go
       in under one hold; the wait for its status write is outside it, except in the power paths
       (units_hold) and a recovery, which hold it. */
    struct spinlock lock;
    uint32_t tail;
    /* The status data of the waits: each sync's wait writes the next token into the unit's word
       (sems), the unit completing them in order, so a word at or past a sync's token says its
       descriptors are done. restarted: the last token given before a recovery, which voids the
       waits up to it that had not completed. */
    uint32_t token, restarted;
    bool queue_on;  /* the queue on is ours (queue_start), not one the firmware left on */
    bool recovering; /* re-synchronizing after a queue error: another error is final */
    bool failed;   /* completion is uncertain, and no recovery confirmed it: never reuse its semaphore or
                      report a later sync successful */
    bool command_failed; /* a timed-out register command may still own the command interface */
};
static struct unit *units;
static size_t nunits, units_room;
static struct ioapic_scope {
    uint8_t id;       /* the MADT's */
    uint16_t segment; /* of the unit it is under */
    uint16_t sid;     /* the requester ID it sends with */
    int first;        /* the remapping entry of its pin 0, its pins' following it; -1 before the first */
} *ioapics;
static size_t nioapics, ioapics_room;
static uint8_t dmar_flags;
static unsigned host_bits;
/* remap: interrupts are remapped; eime: in Extended Interrupt Mode, with x2APIC destinations
   (11.4.10: "x2APIC mode is active"), where the CPUs are in x2APIC mode or wait for it */
static bool remap, eime, caching_mode, write_buffers, parse_lost;
static bool units_incomplete; /* separate from unsupported/malformed device-scope policy */
static bool irreq; /* a unit requires interrupt remapping (ECAP.IRREQ) */
static unsigned iec_mask = 15; /* the largest IEC invalidation mask every unit takes (MHMV) */
static struct iommu_domain *identity; /* units without pass-through */
static uint64_t sems;
static size_t sem_pages;
static struct irq *fault_irq; /* the fault events' (fault_isr) */
/* An interrupt remapping table, one per segment, its units sharing it: an entry checks its sender
   by a 16-bit ID, which names a function only within its segment. Made at start, all of them, and
   never moved after: the functions keep pointers to t (dev/iommu.c). */
struct irt_table {
    uint16_t segment;
    struct iommu_irt t;
};
static struct irt_table *irts;
static size_t nirts, irts_room;
/* The tables' entries: a leaf, never held while a unit's lock is taken. An entry's units are told
   by a sync on every unit after it changed. Taken after the common part's locks and
   arch/ioapic.c's. */
static struct spinlock tables_lock;

/* NULL: no unit serves the segment, so nothing remaps its interrupts. */
static struct irt_table *irt_of(uint16_t segment) {
    for (size_t i = 0; i < nirts; i++)
        if (irts[i].segment == segment) return &irts[i];
    return NULL;
}

static struct iommu_hw intel;
static bool queue_check(struct unit *u);
static bool queue_recover(struct unit *u, uint32_t errors, uint64_t record);
static bool units_off(void);

static uint32_t rd32(const struct unit *u, unsigned off) { return mmio_read32(u->regs + off); }
static void wr32(const struct unit *u, unsigned off, uint32_t v) { mmio_write32(u->regs + off, v); }
static uint64_t rd64(const struct unit *u, unsigned off) { return mmio_read64(u->regs + off); }
static void wr64(const struct unit *u, unsigned off, uint64_t v) { mmio_write64(u->regs + off, v); }

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- DMAR ---- */
/* A device scope's requester ID: its path from the start bus, through the bridges on the way;
   -1 when a bridge on the way is not there. */
static int scope_id(uint16_t segment, const uint8_t *s) {
    unsigned bus = s[5], n = (s[1] - 6u) / 2;
    for (unsigned i = 0; i < n; i++) {
        unsigned dev = s[6 + 2 * i] & 31, func = s[7 + 2 * i] & 7;
        if (i + 1 == n) return (int)(bus << 8 | dev << 3 | func);
        uint32_t r = pci_read32((struct pci_address){segment, (uint8_t)bus, (uint8_t)dev, (uint8_t)func}, 0x18);
        if (r == 0xFFFFFFFF) return -1;
        bus = r >> 8 & 0xFF;
    }
    return -1;
}

/* Device scopes of a segment from `at` to `end`, each passed with its requester ID. */
static void scopes(uint16_t segment, const uint8_t *b, unsigned at, unsigned end,
                   void (*each)(uint16_t segment, const uint8_t *s, int id, void *arg), void *arg) {
    for (; at < end; at += b[at + 1]) {
        if (end - at < 6 || b[at + 1] < 8 || b[at + 1] > end - at || (b[at + 1] - 6) % 2) {
            parse_lost = true;
            return;
        }
        int id = scope_id(segment, b + at);
        if (id >= 0) each(segment, b + at, id, arg);
        else kprintf("iommu: a DMAR device scope leads through a bridge that is not there; ignored\n");
    }
}

/* The buses below a bridge with this ID, as its registers say. */
static void bridge_buses(uint16_t segment, int id, uint16_t *first, uint16_t *last) {
    uint32_t r = pci_read32(pci_address_of(segment, (uint16_t)id), 0x18);
    *first = (uint16_t)((r >> 8 & 0xFF) << 8);
    *last = (uint16_t)((r >> 16 & 0xFF) << 8 | 0xFF);
}

/* DRHD scopes (8.3.1): an endpoint, a bridge with everything below it, an IOAPIC (its
   enumeration ID is the MADT's), an HPET that sends MSIs, an ACPI device. */
static void drhd_scope(uint16_t segment, const uint8_t *s, int id, void *arg) {
    (void)arg;
    uint16_t first, last;
    struct iommu_id kernel = {segment, (uint16_t)id};
    switch (s[0]) {
    case 1: iommu_cover(segment, (uint16_t)id, (uint16_t)id); break;
    case 2:
        iommu_cover(segment, (uint16_t)id, (uint16_t)id);
        bridge_buses(segment, id, &first, &last);
        iommu_cover(segment, first, last);
        break;
    case 3: {
        iommu_kernel_id(kernel);
        struct ioapic_scope *a = kappend(&ioapics, &nioapics, &ioapics_room, sizeof *a);
        if (a) *a = (struct ioapic_scope){s[4], segment, (uint16_t)id, -1};
        else parse_lost = true;
        break;
    }
    case 4: iommu_kernel_id(kernel); break;
    }
}

/* RMRR: memory the functions in its scopes reach by DMA from the firmware's start (a USB
   controller's legacy emulation, a graphics device's stolen memory), to be mapped for reading
   and writing. A bridge's scope is everything below it too. */
static void rmrr_scope(uint16_t segment, const uint8_t *s, int id, void *arg) {
    const uint64_t *range = arg;
    uint16_t first, last;
    if (s[0] != 1 && s[0] != 2) return;
    iommu_reserved(segment, (uint16_t)id, (uint16_t)id, range[0], range[1], IOMMU_READ | IOMMU_WRITE);
    if (s[0] == 2) {
        bridge_buses(segment, id, &first, &last);
        iommu_reserved(segment, first, last, range[0], range[1], IOMMU_READ | IOMMU_WRITE);
    }
}

/* The DMAR, when it holds its fixed fields: NULL otherwise. */
static const struct acpi_sdt *dmar_table(void) {
    const struct acpi_sdt *h = acpi_find("DMAR", 0);
    return h && h->length >= DMAR_HEADER ? h : NULL;
}

/* The register set of the DRHD at s (8.3): at *base, *bytes long (Size: log2 of its 4 KiB pages,
   upper four bits reserved). False where it is not aligned to that whole set, as 8.3 requires: a
   shifted base can make unrelated registers look like a valid, stopped unit, so nothing of it
   may be mapped or read then. */
static bool drhd_registers(const uint8_t *s, uint64_t *base, uint64_t *bytes) {
    unsigned size = s[5];
    memcpy(base, s + 8, sizeof *base);
    *bytes = (uint64_t)PAGE_SIZE << (size & DRHD_SIZE_MASK);
    return !(size & ~DRHD_SIZE_MASK) && *base && !(*base & (*bytes - 1)) && *base <= UINT64_MAX - *bytes;
}

static bool parse_dmar(void) {
    const struct acpi_sdt *h = dmar_table();
    if (!h) return false;
    const uint8_t *b = (const uint8_t *)h;
    host_bits = (unsigned)b[DMAR_HAW] + 1;
    dmar_flags = b[DMAR_FLAGS];
    uint32_t off = DMAR_HEADER;
    for (; h->length - off >= 4;) {
        uint16_t type = u16(b + off), len = u16(b + off + 2);
        if (len < 4 || len > h->length - off) { parse_lost = units_incomplete = true; break; }
        uint64_t base;
        if (type == 0) { /* DRHD */
            if (len < 16) { parse_lost = units_incomplete = true; break; }
            uint64_t reg_bytes;
            bool aligned = drhd_registers(b + off, &base, &reg_bytes);
            uint16_t segment = u16(b + off + 6);
            /* rejected before any mapping or read, also for a plain-safe observation */
            if (!aligned) {
                parse_lost = units_incomplete = true;
                kprintf("iommu: the DMAR describes an invalid VT-d register range at %lx (size %u); not used\n",
                        base, b[off + 5]);
            } else {
                struct unit *u = kappend(&units, &nunits, &units_room, sizeof *u);
                if (!u) {
                    parse_lost = units_incomplete = true;
                } else {
                    *u = (struct unit){.base = base, .segment = segment, .reg_bytes = reg_bytes};
                    if (b[off + 4] & 1) iommu_cover(segment, 0, 0xFFFF); /* INCLUDE_PCI_ALL: what no other unit has */
                    scopes(segment, b, off + 16, off + len, drhd_scope, NULL);
                }
            }
        } else if (type == 1) { /* RMRR, 8.4: fixed header followed by one or more scopes */
            if (len <= 24) parse_lost = true;
            else {
                uint64_t limit;
                memcpy(&base, b + off + 8, 8);
                memcpy(&limit, b + off + 16, 8);
                uint64_t end = limit + 1;
                /* Base and size must be page aligned. Do not round malformed bounds
                   into a wider reservation, or let either addition wrap it to empty.
                   Losing a mandatory reservation prevents translation, not independent
                   observation of every DRHD's stopped/plain-safe state. */
                if (limit <= base || !end || (base | end) & (PAGE_SIZE - 1)) parse_lost = true;
                else {
                    uint64_t range[2] = {base, end};
                    scopes(u16(b + off + 6), b, off + 24, off + len, rmrr_scope, range);
                }
            }
        } else if (type > 6) units_incomplete = true; /* skip per 8.2, but do not attest unknown state as plain-safe */
        off += len;
    }
    if (off != h->length) parse_lost = units_incomplete = true;
    return nunits != 0;
}

/* Every IOAPIC of the MADT in the scope of exactly one unit (8.3.1.1): the source check of its
   pins' entries needs the ID it sends with. */
static bool ioapics_described(void) {
    const struct acpi_sdt *madt = acpi_find("APIC", 0);
    for (const struct acpi_entry *e = madt ? acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL) : NULL; e;
         e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e)) {
        const struct acpi_madt_ioapic *a = (const void *)e;
        if (e->type != ACPI_MADT_IOAPIC || e->length < sizeof *a || !a->address) continue;
        unsigned found = 0;
        for (size_t k = 0; k < nioapics; k++) found += ioapics[k].id == a->id;
        if (found != 1) {
            kprintf("iommu: the DMAR names IOAPIC %u %s\n", a->id, found ? "more than once" : "under no unit");
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

static bool command_idle(struct unit *u) {
    if (u->command_failed) return false;
    /* Do not overlap firmware's register invalidation/write-buffer/protected-range/enhanced command (6.9,
       11.4.4.1). A timed-out interface remains hardware-owned, not free for cleanup writes. */
    for (uint64_t t = clock_after(100 * NS_PER_MS);; pause()) {
        uint32_t pmen = rd32(u, R_PMEN);
        bool busy = (rd64(u, R_CCMD) >> 63) || /* ICC */
                    (rd64(u, ECAP_IRO(u->ecap) + 8) >> 63) || /* IVT */
                    (rd32(u, R_GSTS) & GCMD_WBF) ||
                    (u->cap & CAP_ECMDS && rd64(u, R_ECRSP) & 1) ||
                    (u->cap & (CAP_PLMR | CAP_PHMR) && !!(pmen & PMEN_EPM) != !!(pmen & PMEN_PRS));
        if (!busy) break;
        if (clock_now_ns() >= t) {
            u->command_failed = true;
            return false;
        }
    }
    return true;
}

static bool command(struct unit *u, uint32_t bit, bool on) {
    if (!command_idle(u)) return false;
    uint32_t sts = rd32(u, R_GSTS) & ~GSTS_ONESHOT;
    wr32(u, R_GCMD, on ? sts | bit : sts & ~bit);
    bool completed = bit == GCMD_WBF ? false : on; /* WBFS clears on completion, unlike RTPS/IRTPS */
    for (uint64_t t = clock_after(100 * NS_PER_MS); !!(rd32(u, R_GSTS) & bit) != completed; pause())
        if (clock_now_ns() >= t) {
            kprintf("iommu: VT-d unit at %lx did not %s %s\n", u->base,
                    bit & GSTS_ONESHOT ? "take" : on ? "turn on" : "turn off", command_name(bit));
            u->command_failed = true;
            return false;
        }
    return true;
}

/* Units that buffer their writes (CAP.RWBF) write them, for a change no invalidation follows:
   the invalidations flush them first themselves (6.8). False when one did not confirm it. */
static bool write_buffers_flush(void) {
    bool ok = true;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        uint64_t f = spin_lock(&u->lock);
        if (!queue_check(u)) ok = false;
        else if (write_buffers && u->cap & CAP_RWBF && !command(u, GCMD_WBF, true)) {
            u->failed = true;
            ok = false;
        }
        spin_unlock(&u->lock, f);
    }
    return ok;
}

static bool queue_start(struct unit *u) {
    /* An IQE/ITE an earlier queue left keeps this one from being fetched (6.5.2): queue_check
       clears it while no queue is on. */
    if (rd32(u, R_GSTS) & GCMD_QIE || !queue_check(u)) return false;
    /* QI must not be enabled over register-based invalidations (6.5.2). */
    if (!command_idle(u)) {
        u->failed = true;
        return false;
    }
    wr64(u, R_IQT, 0);
    u->tail = 0;
    wr64(u, R_IQA, u->queue); /* 128-bit descriptors, one page */
    u->queue_on = command(u, GCMD_QIE, true);
    return u->queue_on;
}

static void queue_errors_show(const struct unit *u, uint32_t errors, uint64_t record) {
    if (errors & FSTS_IQE)
        kprintf("iommu: VT-d unit at %lx: invalidation queue error %u (IQEI) at head %lx\n", u->base, IQEI(record),
                rd64(u, R_IQH));
    if (errors & FSTS_ITE)
        kprintf("iommu: VT-d unit at %lx: a Device-TLB invalidation of requester %04x timed out (ITE)\n", u->base,
                ITESID(record));
    if (errors & FSTS_ICE && iommu_fault_report()) /* a device sends these at will */
        kprintf("iommu: VT-d unit at %lx: requester %04x sent an invalid Device-TLB invalidation completion (ICE)\n",
                u->base, ICESID(record));
}

/* Queue errors (6.5.2.11). An ICE is a Device-TLB invalidation completion that matches no ITag
   (4.3), and the unit goes on with the queue: this kernel sends no Device-TLB invalidation and
   its context entries block Translation Requests (TT 00b/10b, 9.3), so no completion it waits
   for is lost. It is recorded and cleared. IQE and ITE stop the queue until cleared (6.5.2): on
   the OS's own queue, queue_recover; with no queue on, they are what an earlier one left (sticky,
   11.3), cleared for the next. On an inherited queue an ITE is cleared too: the unit fetches
   from its head again, and queue_quiet's wait confirms the queue. An IQE there is not: it names
   a descriptor the OS must not rewrite. Uncertain completion stays latched (failed), with the
   evidence; callers retain DMA memory/IR entries rather than turn a later wait into a false
   recovery. Not for the fault interrupt: recovery submits and waits. */
static bool queue_check(struct unit *u) {
    if (u->failed || u->command_failed) {
        wr32(u, R_FECTL, rd32(u, R_FECTL) | FECTL_IM);
        return false;
    }
    uint32_t errors = rd32(u, R_FSTS) & FSTS_QUEUE_ERRORS;
    bool live = (rd32(u, R_GSTS) & GCMD_QIE) != 0, ours = live && u->queue_on && rd64(u, R_IQA) == u->queue;
    bool stopped = live && errors & (FSTS_IQE | FSTS_ITE);
    uint64_t record = errors ? rd64(u, R_IQERCD) : 0;
    if (errors) queue_errors_show(u, errors, record);
    if (u->queue_on && !ours) {
        /* the unit lost QIES or IQA of the queue the OS still owns */
    } else if (!stopped || (!ours && !(errors & FSTS_IQE))) {
        if (errors) wr32(u, R_FSTS, errors);
        return true;
    } else if (ours && !u->recovering && queue_recover(u, errors, record)) {
        return true;
    }
    kprintf("iommu: VT-d unit at %lx: invalidation failed, status %x, head %lx, record %lx; completion unknown\n",
            u->base, errors, rd64(u, R_IQH), record);
    u->failed = true;
    wr32(u, R_FECTL, rd32(u, R_FECTL) | FECTL_IM); /* retain the error without an interrupt storm */
    return false;
}

/* A descriptor for one unit, u->lock held: false, and failed, when it had no room for it in time.
   IOTLB invalidations ask for the draining a version 1 unit does only when asked (6.5.4). */
static bool submit(struct unit *u, uint64_t lo, uint64_t hi) {
    if (!u->queue_on || !queue_check(u)) return false;
    if ((lo & 0xF) == 2) lo |= (u->cap & CAP_DRD ? INV_IOTLB_DR : 0) | (u->cap & CAP_DWD ? INV_IOTLB_DW : 0);
    for (uint64_t t = clock_after(100 * NS_PER_MS); (rd64(u, R_IQH) & (PAGE_SIZE - 16)) == (u->tail + 16) % PAGE_SIZE;
         pause()) {
        if (!queue_check(u)) return false;
        if (clock_now_ns() >= t) {
            u->failed = true;
            return false;
        }
    }
    volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)phys_to_virt(u->queue) + u->tail);
    slot[0] = lo;
    slot[1] = hi;
    if (!intel.coherent) cpu_cache_flush(slot, 16);
    __asm__ volatile("sfence" ::: "memory"); /* descriptor contents before the MMIO tail publication */
    u->tail = (u->tail + 16) % PAGE_SIZE;
    wr64(u, R_IQT, u->tail);
    return true;
}

/* Every unit's lock held (units_hold): a descriptor to each. */
static void send(uint64_t lo) {
    for (size_t i = 0; i < nunits; i++) submit(&units[i], lo, 0);
}

/* A change of root or context entries: the context caches, then the IOTLBs, both whole (6.5.2.1:
   a global context invalidation is followed by a global IOTLB one). u->lock held. */
static void context_flush(struct unit *u) {
    submit(u, INV_CONTEXT_GLOBAL, 0);
    submit(u, INV_IOTLB_GLOBAL, 0);
}

static void fault_show(const struct unit *u, uint64_t lo, uint64_t hi) {
    unsigned reason = hi >> 32 & 0xFF;
    struct pci_address a = pci_address_of(u->segment, (uint16_t)hi);
    if (!iommu_fault_report()) return;
    if (reason >= 0x20)
        kprintf("iommu: %04x:%02x:%02x.%x: interrupt (entry %u) blocked, reason %02xh\n", PCI_ADDRESS_ARGS(a),
                (unsigned)(lo >> 48), reason);
    else
        kprintf("iommu: %04x:%02x:%02x.%x: %s at %lx blocked, reason %02xh\n", PCI_ADDRESS_ARGS(a),
                hi >> 62 & 1 ? "DMA read" : "DMA write", lo & ADDR, reason);
}

static void faults_drain(struct unit *u) {
    unsigned fro = CAP_FRO(u->cap), nfr = CAP_NFR(u->cap);
    for (unsigned i = 0; i < nfr; i++) {
        unsigned r = fro + i * 16;
        uint64_t hi = rd64(u, r + 8);
        if (!(hi >> 63)) continue;
        fault_show(u, rd64(u, r), hi);
        wr32(u, r + 12, 1u << 31); /* F: the record is free again */
    }
    if (rd32(u, R_FSTS) & FSTS_PFO) {
        wr32(u, R_FSTS, FSTS_PFO);
        kprintf("iommu: VT-d unit at %lx: more faults than its records\n", u->base);
    }
}

/* An ICE is cleared here. IQE and ITE wait for the queue's next user (queue_check), whose
   recovery submits and waits; no fault event comes while they are set (7.3). A unit whose lock
   another CPU holds is drained when it is let go. */
static enum irq_result fault_isr(void *context) {
    (void)context;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        spin_take(&u->lock);
        if (rd32(u, R_FSTS) & FSTS_ICE) {
            queue_errors_show(u, FSTS_ICE, rd64(u, R_IQERCD));
            wr32(u, R_FSTS, FSTS_ICE);
        }
        faults_drain(u);
        spin_give(&u->lock);
    }
    return IRQ_HANDLED;
}

/* The unit's next status data (never 0, the word's value before any wait). u->lock held. */
static uint32_t token_next(struct unit *u) {
    if (!++u->token) u->token++;
    return u->token;
}

/* u->lock held: the wait that ends the caller's descriptors. Its token, 0 when the queue failed or
   took no more. */
static uint32_t wait_post(struct unit *u) {
    if (!queue_check(u)) return 0;
    uint32_t data = token_next(u);
    return submit(u, INV_WAIT(data), sems + (size_t)(u - units) * sizeof(uint32_t)) ? data : 0;
}

/* Waits for the unit to write a sync's token: held, with u->lock (the power paths, a recovery),
   else outside it, the queue's errors looked at under it. False when a recovery voided the wait,
   the queue failed, or the unit did not finish in time. The records it logged are said after. */
static bool wait_done(struct unit *u, uint32_t data, bool held) {
    volatile uint32_t *word = (volatile uint32_t *)phys_to_virt(sems) + (u - units);
    bool ok = data != 0;
    for (uint64_t t = clock_after(100 * NS_PER_MS); ok && (int32_t)(*word - data) < 0; pause()) {
        uint64_t f = held ? 0 : spin_lock(&u->lock);
        if (!queue_check(u) || (int32_t)(u->restarted - data) >= 0) ok = false;
        else if (clock_now_ns() >= t) {
            kprintf("iommu: VT-d unit at %lx did not finish its commands\n", u->base);
            u->failed = true;
            ok = false;
        }
        if (!held) spin_unlock(&u->lock, f);
    }
    uint64_t f = held ? 0 : spin_lock(&u->lock);
    faults_drain(u);
    ok = ok && queue_check(u);
    if (!held) spin_unlock(&u->lock, f);
    return ok;
}

/* Units first..first + n - 1 have done every descriptor sent before this sync: each given its
   wait, a group at a time before the group is waited for. held: the caller holds their locks. */
#define UNITS_AT_ONCE 16
static bool sync_units(size_t first, size_t n, bool held) {
    bool ok = true;
    for (size_t from = first; from < first + n; from += UNITS_AT_ONCE) {
        size_t count = MIN(first + n - from, (size_t)UNITS_AT_ONCE);
        uint32_t data[UNITS_AT_ONCE];
        for (size_t i = 0; i < count; i++) {
            struct unit *u = &units[from + i];
            uint64_t f = held ? 0 : spin_lock(&u->lock);
            data[i] = wait_post(u);
            if (!held) spin_unlock(&u->lock, f);
        }
        for (size_t i = 0; i < count; i++) ok &= wait_done(&units[from + i], data[i], held);
    }
    return ok;
}

/* Every unit's lock held (units_hold). */
static bool sync_held(void) { return sync_units(0, nunits, true); }

/* fill's descriptors to every unit under its lock, then a sync on every unit. */
static bool on_every_unit(void (*fill)(struct unit *u, const void *arg), const void *arg) {
    for (size_t i = 0; i < nunits; i++) {
        uint64_t f = spin_lock(&units[i].lock);
        fill(&units[i], arg);
        spin_unlock(&units[i].lock, f);
    }
    return sync_units(0, nunits, false);
}

/* Interrupt entries first..first + 2^log2 - 1 (first aligned to that), in descriptors whose
   mask every unit takes. */
struct iec_range {
    uint16_t segment;
    unsigned first, log2;
};
static void iec_flush(struct unit *u, const void *arg) {
    const struct iec_range *r = arg;
    if (u->segment != r->segment) return;
    unsigned mask = r->log2 < iec_mask ? r->log2 : iec_mask;
    for (unsigned at = r->first; at < r->first + (1u << r->log2); at += 1u << mask) submit(u, INV_IEC(at, mask), 0);
}

/* The power paths' (start, resume, shutdown): every unit's lock, in their order. */
static uint64_t units_hold(void) {
    uint64_t f = irq_save();
    for (size_t i = 0; i < nunits; i++) spin_take(&units[i].lock);
    return f;
}
static void units_release(uint64_t f) {
    for (size_t i = nunits; i--;) spin_give(&units[i].lock);
    irq_restore(f);
}

/* The OS's own queue after an IQE/ITE (6.5.2.11): what is ahead of the head completes as normal,
   nothing from the head on is fetched until the error is cleared, then the unit fetches from the
   head again. IQE: the head references the descriptor in error, and software corrects the cause
   first. The OS corrects what it wrote: the tail (IQEI 1) from its own record, a descriptor
   (3: its type, Table 26; 4: a reserved field) by turning every one from the head on into a
   fence, whose invalidations the global ones below subsume; without a record (0) both. A fetch
   error (2), the width or root table mode its registers hold (5..7) or an undefined code is
   nothing it can correct. ITE: the pending waits were aborted, and nothing behind a timed-out
   Device-TLB invalidation is guaranteed (this kernel sends none). Either way the waits already
   sent no longer confirm what preceded them: once the error is cleared, every cache is
   invalidated whole and a wait of its own confirms it (6.5.2.12). */
static bool queue_recover(struct unit *u, uint32_t errors, uint64_t record) {
    uint64_t head = rd64(u, R_IQH);
    if (!sems || head & ~IQ_PTR_MASK || head >= PAGE_SIZE) return false;
    if (errors & FSTS_IQE) {
        unsigned code = IQEI(record);
        if (code == 2 || code > 4) return false;
        for (uint32_t at = (uint32_t)head; at != u->tail; at = (at + INV_BYTES) % PAGE_SIZE) {
            volatile uint64_t *slot = (volatile uint64_t *)((uint8_t *)phys_to_virt(u->queue) + at);
            slot[0] = INV_FENCE;
            slot[1] = 0;
        }
        if (!intel.coherent) cpu_cache_flush(phys_to_virt(u->queue), PAGE_SIZE);
        __asm__ volatile("sfence" ::: "memory"); /* the fences before the unit fetches them */
        wr64(u, R_IQT, u->tail); /* no descriptor beyond the ones above */
    }
    wr32(u, R_FSTS, errors); /* only now may the unit fetch again */
    u->recovering = true;
    u->restarted = u->token; /* the waits already sent confirm nothing now */
    bool ok = submit(u, INV_CONTEXT_GLOBAL, 0) && submit(u, INV_IOTLB_GLOBAL, 0) &&
              (!remap || submit(u, INV_IEC_GLOBAL, 0)) && sync_units((size_t)(u - units), 1, true);
    u->recovering = false;
    if (ok) kprintf("iommu: VT-d unit at %lx: its invalidation queue recovered, every cache invalidated\n", u->base);
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
        if (!(table = iommu_alloc_pages(1))) return NULL;
        memcpy(phys_to_virt(table), phys_to_virt(u->pass), PAGE_SIZE);
        if (!intel.coherent) cpu_cache_flush(phys_to_virt(table), PAGE_SIZE);
        iommu_entry_write(root, table | CTX_P);
    }
    return (volatile uint64_t *)phys_to_virt(table) + (id & 0xFF) * 2;
}

static void context_write(volatile uint64_t *e, uint64_t lo, uint64_t hi) {
    e[0] = 0; /* present last */
    e[1] = hi;
    e[0] = lo;
    if (!intel.coherent) cpu_cache_flush(e, 16);
}

/* u->lock held: the domain's entries in the unit, then the unit's context caches. */
struct contexts {
    const struct iommu_domain *dom;
    uint64_t lo, hi;
};
static void contexts_flush(struct unit *u, const void *arg) {
    const struct contexts *c = arg;
    for (unsigned n = 0; n < c->dom->nids; n++)
        if (c->dom->ids[n].segment == u->segment)
            context_write(context_entry(u, c->dom->ids[n].requester, false), c->lo, c->hi);
    context_flush(u);
}

/* ---- interrupts ---- */
/* One store: an entry in use moves to its new CPU without a moment not present, in which a
   message on its way would be dropped as a fault (5.1.4). */
static void irte_write(struct irt_table *t, unsigned index, uint64_t lo, uint64_t hi) {
    iommu_entry_write16((volatile uint64_t *)phys_to_virt(t->t.phys) + index * 2, lo, hi);
}

/* The destination field (9.10): an x2APIC ID in bits 63:32 in Extended Interrupt Mode, an xAPIC
   ID in bits 47:40 otherwise. */
static uint64_t irte_dest(uint32_t dest) { return eime ? (uint64_t)dest << 32 : (uint64_t)(dest & 0xFF) << 40; }

/* The highest APIC ID an entry names: 32 bits in Extended Interrupt Mode, 8 otherwise. */
static uint32_t remapped_reach(void) { return eime ? UINT32_MAX : IOMMU_PLAIN_REACH; }

/* What an entry checks the sender against: d's requester ID, or the buses of its IDs when a
   bridge sends its requests as its own. 0 without memory. */
static uint64_t irte_source(const struct pci_device *d) {
    unsigned n;
    struct iommu_id *ids = iommu_requester_ids(d, &n);
    if (!ids) return 0;
    uint64_t source = IRTE_SID(ids[0].requester, 1);
    if (n > 1) {
        uint8_t first = 0xFF, last = 0;
        for (unsigned i = 0; i < n; i++) {
            if (ids[i].requester >> 8 < first) first = ids[i].requester >> 8;
            if (ids[i].requester >> 8 > last) last = ids[i].requester >> 8;
        }
        source = IRTE_SID((uint16_t)(first << 8 | last), 2);
    }
    kfree(ids);
    return source;
}

static void irte_write_locked(struct irt_table *t, unsigned index, uint64_t lo, uint64_t hi) {
    uint64_t f = spin_lock(&tables_lock);
    irte_write(t, index, lo, hi);
    spin_unlock(&tables_lock, f);
}

static struct irt_table *irt_holding(const struct iommu_irt *t) {
    return (struct irt_table *)((uint8_t *)t - __builtin_offsetof(struct irt_table, t));
}

static bool intel_msi_table(struct pci_device *d, struct iommu_irt **t) {
    struct irt_table *irt = remap && iommu_seen(d) ? irt_of(d->address.segment) : NULL;
    *t = irt ? &irt->t : NULL;
    return true;
}

static uint32_t intel_msi_reach(const struct pci_device *d) {
    return remap && irt_of(d->address.segment) && iommu_seen(d) ? remapped_reach() : IOMMU_PLAIN_REACH;
}

/* Message n of d arrives through entry first + n: the address carries the handle first with
   SHV, the data the subhandle 0 the function adds the message number to (5.1.5.2). */
static bool intel_msi_write(struct pci_device *d, struct iommu_irt *it, unsigned first, unsigned count, uint8_t vector,
                            uint32_t dest, uint32_t *address, uint32_t *data) {
    struct irt_table *t = irt_holding(it);
    uint64_t source = irte_source(d);
    if (!source) return false;
    uint64_t f = spin_lock(&tables_lock);
    for (unsigned i = 0; i < count; i++) irte_write(t, first + i, IRTE_P | IRTE_VECTOR(vector + i) | irte_dest(dest), source);
    spin_unlock(&tables_lock, f);
    bool ok = on_every_unit(iec_flush, &(struct iec_range){t->segment, first, (unsigned)__builtin_ctz(count)});
    *address = 0xFEE00000u | (first & 0x7FFF) << 5 | 1u << 4 | 1u << 3 | (first >> 15 & 1) << 2;
    *data = 0;
    return ok;
}

/* entries[0 .. count - 1] of a table, IOMMU_NO_IRTE skipped, one descriptor each. */
struct iec_list {
    uint16_t segment;
    const uint32_t *entries;
    unsigned count;
};
static void iec_entries(struct unit *u, const void *arg) {
    const struct iec_list *l = arg;
    if (u->segment != l->segment) return;
    for (unsigned i = 0; i < l->count; i++)
        if (l->entries[i] != IOMMU_NO_IRTE) submit(u, INV_IEC(l->entries[i], 0), 0);
}

static bool intel_msi_erase(struct pci_device *d, struct iommu_irt *it, const uint32_t *entries, unsigned count) {
    (void)d;
    struct irt_table *t = irt_holding(it);
    uint64_t f = spin_lock(&tables_lock);
    for (unsigned i = 0; i < count; i++)
        if (entries[i] != IOMMU_NO_IRTE) irte_write(t, entries[i], 0, 0);
    spin_unlock(&tables_lock, f);
    return on_every_unit(iec_entries, &(struct iec_list){t->segment, entries, count});
}

static struct ioapic_scope *ioapic_of(uint8_t ioapic_id) {
    for (size_t i = 0; i < nioapics; i++)
        if (ioapics[i].id == ioapic_id) return &ioapics[i];
    return NULL;
}

static uint32_t intel_ioapic_reach(uint8_t ioapic_id) {
    return remap && ioapic_of(ioapic_id) ? remapped_reach() : IOMMU_PLAIN_REACH;
}

/* The pins of an IOAPIC are a run of entries, taken at its first pin's route. The IRTE takes
   what the plain entry said (delivery mode, logical destination, trigger, vector) and the
   destination; the pin keeps polarity, trigger, vector (for the EOI of a level pin, 5.1.5.1) and
   mask, with delivery mode 000. arch/ioapic.c's lock held: one route of a pin at a time. */
static bool intel_ioapic(uint8_t ioapic_id, unsigned pin, unsigned pins, uint32_t dest, uint64_t *rte) {
    if (!remap) return dest <= IOMMU_PLAIN_REACH; /* the plain entry's 8-bit destination */
    struct ioapic_scope *a = ioapic_of(ioapic_id);
    struct irt_table *t = a ? irt_of(a->segment) : NULL;
    if (!t) {
        kprintf("iommu: IOAPIC %u is under no VT-d unit: its pin %u left masked\n", ioapic_id, pin);
        return false;
    }
    if (a->first < 0) {
        unsigned run = 1;
        while (run < pins) run <<= 1;
        a->first = iommu_irt_take(&t->t, run);
        if (a->first < 0) {
            kprintf("iommu: no interrupt remapping entries left for IOAPIC %u: its pins left masked\n", ioapic_id);
            return false;
        }
    }
    unsigned index = (unsigned)a->first + pin;
    uint64_t v = *rte;
    irte_write_locked(t, index, IRTE_P | (v >> 11 & 1 ? IRTE_DM : 0) | (v >> 15 & 1 ? IRTE_TM : 0) | IRTE_DLM(v >> 8 & 7) |
                                    IRTE_VECTOR(v) | irte_dest(dest), IRTE_SID(a->sid, 1));
    if (!on_every_unit(iec_flush, &(struct iec_range){t->segment, index, 0})) {
        kprintf("iommu: the units did not confirm the entry of IOAPIC %u pin %u: left masked\n", ioapic_id, pin);
        return false;
    }
    *rte = (v & RTE_KEPT) | RTE_REMAPPABLE | (uint64_t)(index & 0x7FFF) << 49 | (uint64_t)(index >> 15 & 1) << 11;
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
static bool intel_is_leaf(uint64_t entry, unsigned level) { return level == 1 || (entry & SL_PS) != 0; }

/* Invalidation completion alone is not DMA lifetime completion. Version 1 needs both
   optional drain capabilities; version 2 and later must perform the required drains
   (6.5.4, 11.4.2). PCI BME/Transactions Pending does not replace this guarantee. */
static bool dma_draining(void) {
    for (size_t i = 0; i < nunits; i++)
        if (VER_MAJOR(rd32(&units[i], R_VER)) < 2 &&
            (units[i].cap & (CAP_DRD | CAP_DWD)) != (CAP_DRD | CAP_DWD)) return false;
    return true;
}

/* A present context entry does not change in place: it goes not present, the caches drop it,
   then it comes back pointing at the domain. */
static bool intel_attach(struct iommu_domain *dom) {
    if (!dma_draining()) {
        kprintf("iommu: VT-d lacks DMA read/write draining: an app domain cannot be isolated safely\n");
        return false; /* no context published: the common layer can release this unused domain */
    }
    for (size_t i = 0; i < nunits; i++) { /* memory first: nothing changes for the group when there is none */
        uint64_t f = spin_lock(&units[i].lock);
        bool made = true;
        for (unsigned n = 0; made && n < dom->nids; n++)
            if (dom->ids[n].segment == units[i].segment) made = context_entry(&units[i], dom->ids[n].requester, true) != NULL;
        spin_unlock(&units[i].lock, f);
        if (!made) {
            kprintf("iommu: no memory for a context table\n");
            return false;
        }
    }
    dom->written = true;
    if (!on_every_unit(contexts_flush, &(struct contexts){dom, 0, 0})) return false;
    return on_every_unit(contexts_flush,
                         &(struct contexts){dom, CTX_P | dom->root, (uint64_t)(dom->levels - 2) | (uint64_t)dom->id << 8});
}

static bool intel_msi_erase(struct pci_device *d, struct iommu_irt *it, const uint32_t *entries, unsigned count);

/* The claimed function's remapping entries not present: they stay given until its messages are
   cleared (iommu_msi_clear), which finds them gone already. */
static bool intel_block(struct iommu_domain *dom) {
    struct pci_device *d = dom->d;
    bool ok = !d->irt || intel_msi_erase(d, d->irt, d->irte, d->irte_room);
    ok &= on_every_unit(contexts_flush, &(struct contexts){dom, 0, 0});
    return ok && dma_draining(); /* otherwise retain the domain and its DMA pages in quarantine */
}

static void iotlb_flush(struct unit *u, const void *arg) {
    submit(u, INV_IOTLB_DOMAIN(((const struct iommu_domain *)arg)->id), 0);
}

/* New entries need no invalidation but where the units cache not-present ones (CAP.CM); then
   only the write buffers are flushed, which an invalidation does itself (6.8). */
static bool intel_flush(struct iommu_domain *dom, bool unmapped) {
    bool ok = !unmapped && !caching_mode ? write_buffers_flush() : on_every_unit(iotlb_flush, dom);
    return ok && (!unmapped || dma_draining());
}

/* ---- the units ---- */
/* A mapping or PMM used bit is not ownership. Unknown firmware queues may only be
   inherited outside ALL allocator/bootloader/module RAM, whose contents could already
   have been reused before probe. Do not read or write their descriptors. */
static bool queue_address_safe(const struct unit *u, uint64_t iqa) {
    if (u->queue && iqa == u->queue) return true;
    if (u->handoff && iqa == (u->handoff | u->handoff_config)) return true;
    if (iqa & ~(ADDR | IQA_DW | IQA_QS) || !intel.phys_bits) return false;
    uint64_t base = iqa & ADDR, bytes = (uint64_t)PAGE_SIZE << (iqa & IQA_QS);
    if (!base || base + bytes < base || (base + bytes - 1) >> intel.phys_bits) return false;
    return !iommu_os_ram(base, base + bytes);
}

/* Run for every unit before allocating/zeroing any translation or queue metadata. */
static bool queues_address_safe(void) {
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (rd32(u, R_GSTS) & GCMD_QIE && !queue_address_safe(u, rd64(u, R_IQA))) return false;
    }
    return true;
}

static void handoff_free(struct unit *u) {
    if (!u->handoff) return;
    size_t pages = ((size_t)1 << (u->handoff_config & IQA_QS)) + 1;
    pmm_free_contig(u->handoff, pages);
    u->handoff = 0;
}

/* Empty alone does NOT prove completion (6.5.2): a queue is quiesced when its last completed
   descriptor is a wait. An inherited queue lies in memory the OS does not own, so its wait goes
   into owned memory: once empty, IQA may be changed (software only must not while it is not
   empty, 11.4.9.3), keeping QS/DW. Where the unit resumes fetching after that change is not
   specified, so every slot of the new queue is a wait: plain fences, and the one at the old
   head position writes the private status word on the extra page. From any head the unit
   reaches the tail through valid waits only, that one last. The status page is never reused
   after an uncertain publication. Firmware/SMM producer handoff remains a platform obligation. */
static bool queue_quiet(struct unit *u) {
    if (!queue_check(u)) return false;
    uint64_t iqa = rd64(u, R_IQA), head = rd64(u, R_IQH), tail = rd64(u, R_IQT);
    unsigned bytes = PAGE_SIZE << (iqa & IQA_QS), width = iqa & IQA_DW ? 2 * INV_BYTES : INV_BYTES;
    if (!queue_address_safe(u, iqa) || (head | tail) & ~IQ_PTR_MASK ||
        head >= bytes || tail >= bytes || (head | tail) & (width - 1)) {
        if (u->queue_on || u->handoff) u->failed = true;
        return false;
    }
    if (u->handoff && iqa == (u->handoff | u->handoff_config)) {
        volatile uint32_t *word = phys_to_virt(u->handoff + bytes);
        if (head != u->handoff_tail || tail != head || *word != u->handoff_token) {
            u->failed = true;
            return false;
        }
        return queue_check(u);
    }
    if (u->queue && iqa == u->queue) {
        if (!sems) return false;
        if (u->queue_on && tail != u->tail) { u->failed = true; return false; }
        u->queue_on = true; /* retained OS allocation, not an address/used-bit guess after S3 */
        u->tail = (uint32_t)tail;
        if (!sync_units((size_t)(u - units), 1, true)) return false;
        /* Status writes precede all effects being visible in the head register. */
        for (uint64_t t = clock_after(100 * NS_PER_MS); rd64(u, R_IQH) != u->tail; pause())
            if (!queue_check(u) || clock_now_ns() >= t) {
                u->failed = true;
                return false;
            }
        if (rd64(u, R_IQT) != u->tail) { u->failed = true; return false; }
        return queue_check(u);
    }
    for (uint64_t t = clock_after(100 * NS_PER_MS); rd64(u, R_IQH) != tail; pause())
        if (!queue_check(u) || clock_now_ns() >= t) return false;
    if (u->handoff) return false; /* never overwrite retained, possibly hardware-owned storage */
    u->handoff_config = (unsigned)(iqa & (IQA_QS | IQA_DW));
    u->handoff = iommu_alloc_pages(bytes / PAGE_SIZE + 1);
    if (!u->handoff) return false;
    /* Allocation may take time. No IQA replacement until the queue is still empty
       at the same producer position, with no error and QIES still set. */
    if (!(rd32(u, R_GSTS) & GCMD_QIE) || rd64(u, R_IQH) != tail ||
        rd64(u, R_IQT) != tail || rd64(u, R_IQA) != iqa || !queue_check(u)) {
        handoff_free(u); /* this allocation was never published */
        return false;
    }
    /* iommu_alloc_pages zeroed it all: the status word, and each slot's upper half */
    uint8_t *queue = phys_to_virt(u->handoff);
    for (unsigned at = 0; at < bytes; at += width) *(volatile uint64_t *)(queue + at) = INV_FENCE;
    volatile uint64_t *slot = (volatile uint64_t *)(queue + tail);
    volatile uint32_t *word = phys_to_virt(u->handoff + bytes);
    u->handoff_token = token_next(u);
    slot[0] = INV_WAIT(u->handoff_token);
    slot[1] = u->handoff + bytes;
    if (!intel.coherent) cpu_cache_flush(queue, bytes + sizeof *word);
    __asm__ volatile("sfence" ::: "memory");
    wr64(u, R_IQA, u->handoff | u->handoff_config);
    if (rd64(u, R_IQA) != (u->handoff | u->handoff_config)) {
        u->failed = true; /* the hardware may have taken the pointer: retain both pages */
        return false;
    }
    u->handoff_tail = (uint32_t)((tail + width) % bytes);
    wr64(u, R_IQT, u->handoff_tail);
    for (uint64_t t = clock_after(100 * NS_PER_MS); *word != u->handoff_token || rd64(u, R_IQH) != u->handoff_tail;
         pause()) {
        if (!queue_check(u)) return false;
        if (clock_now_ns() >= t) {
            u->failed = true;
            return false;
        }
    }
    if (rd64(u, R_IQA) != (u->handoff | u->handoff_config) ||
        rd64(u, R_IQT) != u->handoff_tail || !(rd32(u, R_GSTS) & GCMD_QIE)) {
        u->failed = true;
        return false;
    }
    return queue_check(u);
}

/* Keep protection unchanged until this unit's last wait and queue state are confirmed. */
static bool unit_quiet(struct unit *u) {
    /* Observe even a previously unreported queue error before changing protection. */
    if (!queue_check(u) || !command_idle(u)) return false;
    uint32_t sts = rd32(u, R_GSTS);
    if (sts & GCMD_QIE && !queue_quiet(u)) {
        kprintf("iommu: VT-d unit at %lx: its invalidation queue does not go quiet; protection stays on\n", u->base);
        /* queue_quiet latches uncertainty after publication. Validation/allocation
           failure before publication has no OS status word whose late write could alias. */
        wr32(u, R_FECTL, rd32(u, R_FECTL) | FECTL_IM);
        return false;
    }
    return queue_check(u);
}

/* Called only after every unit is quiet. Translation, interrupt remapping and the queue
   off, fault events masked: false when the unit did not confirm one. */
static bool unit_disable(struct unit *u) {
    if (!queue_check(u)) return false;
    uint32_t sts = rd32(u, R_GSTS);
    /* Never issue another command after an unconfirmed one (6.9). */
    if (sts & GCMD_TE && !command(u, GCMD_TE, false)) return false;
    if (sts & GCMD_IRE && !command(u, GCMD_IRE, false)) return false;
    if (sts & GCMD_QIE) {
        if (!command(u, GCMD_QIE, false)) return false;
        u->queue_on = false;
    }
    wr32(u, R_FECTL, FECTL_IM);
    return true;
}

/* The firmware's protected memory ranges off (11.4.8.1): PMEN is read-only without PLMR/PHMR;
   EPM changes only after the last command completed (6.9), and PRS tells when it took effect.
   Blocked requests leave no fault record, so ranges left on would silently refuse DMA. */
static bool pmr_release(struct unit *u) {
    if (!(u->cap & (CAP_PLMR | CAP_PHMR)) || !(rd32(u, R_PMEN) & (PMEN_EPM | PMEN_PRS))) return true;
    if (!command_idle(u)) return false; /* also an EPM change still in flight */
    uint32_t pmen = rd32(u, R_PMEN);
    if (pmen & PMEN_EPM) wr32(u, R_PMEN, pmen & ~PMEN_EPM);
    for (uint64_t t = clock_after(100 * NS_PER_MS); rd32(u, R_PMEN) & PMEN_PRS; pause())
        if (clock_now_ns() >= t) {
            kprintf("iommu: VT-d unit at %lx did not confirm its protected memory ranges off\n", u->base);
            u->command_failed = true;
            return false;
        }
    return true;
}

static bool pmrs_release(void) {
    for (size_t i = 0; i < nunits; i++)
        if (!pmr_release(&units[i])) return false;
    return true;
}

/* Version 2 and later block DMA to protected memory whether or not translation is on; an
   earlier unit may let remapped requests through to it under TE, and then no mapping may
   reach it (11.4.8.1). Which pre-version-2 units do block is not visible in the registers. */
static bool pmr_open_under_te(const struct unit *u) { return VER_MAJOR(rd32(u, R_VER)) < 2; }

/* TE on the units whose protected ranges hold under translation, or (open) on the others,
   each of whose ranges go off just before its TE. */
static bool translation_on(bool open) {
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (pmr_open_under_te(u) != open) continue;
        if (open && !pmr_release(u)) return false;
        if (!command(u, GCMD_TE, true)) return false;
    }
    return true;
}

static bool unit_tables(struct unit *u) {
    wr64(u, R_RTADDR, u->root); /* legacy mode */
    bool ok = command(u, GCMD_SRTP, true) && queue_start(u);
    if (ok && remap) {
        wr64(u, R_IRTA, irt_of(u->segment)->t.phys | (eime ? IRTA_EIME : 0) | IRT_S);
        ok = command(u, GCMD_SIRTP, true);
    }
    /* without EIME, compatibility-format interrupts bypass the table while CFIS is set (5.1.4) */
    if (ok && remap && !eime && rd32(u, R_GSTS) & GCMD_CFI) ok = command(u, GCMD_CFI, false);
    /* the fault event message (5.1.6), not remapped: in Extended Interrupt Mode the destination's
       bits 31:8 in FEUADDR, which a unit with EIM implements (11.4.7.5) and reads once the pointer
       above set EIME (5.1.6.2), so it is unmasked only then; else an 8-bit destination. */
    struct irq_target to = fault_irq ? irq_target(fault_irq) : (struct irq_target){0};
    if (ok && fault_irq && (eime || to.apic_id <= IOMMU_PLAIN_REACH)) {
        wr32(u, R_FEDATA, to.vector);
        wr32(u, R_FEADDR, iommu_msi_plain(to.apic_id & 0xFF));
        wr32(u, R_FEUADDR, eime ? to.apic_id & ~0xFFu : 0);
        wr32(u, R_FECTL, 0);
    }
    return ok;
}

/* Compatibility-format interrupts must not bypass the interrupt table (5.1.4): EIME, set with the
   table's pointer by SIRTP first (unit_tables), blocks them independently of CFIS, both at boot
   and after sleep. A plain IOAPIC entry may be blocked until its caller converts/restores it, not
   delivered through an isolation bypass.
   The global invalidations flush the table write buffers (6.8). Translation goes on only
   when intel.translating. The firmware's protected memory ranges stay on until every unit
   confirmed its remapping and translation, so a start that fails keeps them; only a unit that
   would leave them open under TE (pmr_open_under_te) drops its ranges earlier: after the TE of
   every unit that keeps them, just before its own. */
static bool units_on(void) {
    if (!units_off()) return false; /* also when firmware left any unit on */
    for (size_t i = 0; i < nunits; i++)
        if (!unit_tables(&units[i])) return false;
    send(INV_CONTEXT_GLOBAL);
    send(INV_IOTLB_GLOBAL);
    if (remap) send(INV_IEC_GLOBAL);
    bool ok = sync_held();
    for (size_t i = 0; ok && remap && i < nunits; i++)
        ok = command(&units[i], GCMD_IRE, true);
    if (ok && intel.translating) ok = translation_on(false) && translation_on(true);
    return ok && pmrs_release();
}

/* Every unit quiet, then off; the protected memory ranges as they were. */
static bool units_off(void) {
    bool ok = true;
    if (!queues_address_safe()) return false;
    /* An error in a later unit must not turn an earlier unit's protected DMA into bypass. */
    for (size_t i = 0; i < nunits; i++) ok &= queue_check(&units[i]);
    if (!ok) return false;
    for (size_t i = 0; i < nunits; i++) ok &= unit_quiet(&units[i]);
    if (!ok) return false;
    for (size_t i = 0; i < nunits; i++)
        if (!unit_disable(&units[i])) {
            kprintf("iommu: VT-d unit at %lx did not turn off\n", units[i].base);
            return false; /* no further command after an unconfirmed one */
        }
    for (size_t i = 0; i < nunits; i++) handoff_free(&units[i]);
    return true;
}

/* Off as on a machine without VT-d (what units_plain_safe takes as plain): the protected memory
   ranges too, once every unit confirmed it is off. */
static bool intel_shutdown(void) {
    uint64_t f = units_hold();
    bool ok = units_off() && pmrs_release();
    units_release(f);
    return ok;
}

static bool intel_resume(void) {
    /* Called only after the machine really slept. Registers may have reset or been
       reprogrammed by wake firmware: the old software producer state is not evidence
       that this is still our queue. Treat any active queue as inherited, retaining all
       uncertainty latches and the memory a late status write could still address. */
    uint64_t f = units_hold();
    for (size_t i = 0; i < nunits; i++) units[i].queue_on = false;
    bool ok = units_on();
    units_release(f);
    if (!ok) kprintf("iommu: VT-d did not come back after sleep\n");
    return ok;
}

static void tables_free(void) {
    if (sems) pmm_free_contig(sems, sem_pages);
    for (size_t i = 0; i < nirts; i++)
        if (irts[i].t.phys) iommu_irt_free(&irts[i].t);
    nirts = 0;
    for (size_t i = 0; i < nioapics; i++) ioapics[i].first = -1;
    for (size_t i = 0; i < nunits; i++) {
        if (units[i].root) pmm_free(units[i].root);
        if (units[i].pass) pmm_free(units[i].pass);
        if (units[i].queue) pmm_free(units[i].queue);
        units[i].root = units[i].pass = units[i].queue = 0;
    }
    sems = 0;
}

/* Every unit's segment gets its table, empty. */
static bool irts_alloc(void) {
    for (size_t i = 0; i < nunits; i++) {
        if (irt_of(units[i].segment)) continue;
        struct irt_table *t = kappend(&irts, &nirts, &irts_room, sizeof *t);
        if (!t) return false;
        memset(t, 0, sizeof *t);
        t->segment = units[i].segment;
        if (!iommu_irt_make(&t->t, IRT_ENTRIES, 16)) return false;
    }
    return true;
}

/* Backend start only: the common caller keeps sources masked until it publishes IOAPIC
   entries. Failure retains uncertain commands and their table storage, and the protected
   memory ranges until the caller's shutdown. */
static bool units_start(bool translate) {
    intel.translating = translate;
    bool ok = units_on();
    if (!ok) {
        intel.translating = false;
        units_off(); /* iommu_init writes the IOAPIC entries back plain */
    }
    return ok;
}

/* translate false, no page table format the units share, or the firmware's DMA_REMAP_OPT_OUT
   (8.1): interrupt remapping alone where the units remap (remap), which is not DMA translation:
   a unit that requires it blocks every interrupt message while it is off (ECAP.IRREQ, 5.1.4), and
   x2APIC mode on Intel's CPUs waits for it (SDM 13.12.7). DMA then passes the units untranslated. */
static bool intel_start(bool translate) {
    if (translate && dmar_flags & DMAR_DMA_REMAP_OPT_OUT) {
        kprintf("iommu: the firmware asks the OS not to enable VT-d DMA remapping (DMAR DMA_REMAP_OPT_OUT): "
                "DMA is not translated, a claimed device gets no DMA isolation\n");
        translate = false;
    }
    translate &= intel.levels != 0;
    if (!translate && !remap) return false;
    if (!queues_address_safe()) {
        kprintf("iommu: an inherited VT-d queue overlaps OS-managed or unclassified RAM; not touched\n");
        return false;
    }
    sem_pages = ALIGN_UP(nunits * sizeof(uint32_t), PAGE_SIZE) / PAGE_SIZE;
    sems = iommu_alloc_pages(sem_pages);
    bool ok = sems && (!remap || irts_alloc()), pass_all = true;
    for (size_t i = 0; i < nunits; i++) {
        units[i].root = iommu_alloc_pages(1);
        units[i].pass = iommu_alloc_pages(1);
        units[i].queue = iommu_alloc_pages(1);
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
        if (identity) {
            iommu_domain_free(identity); /* no unit has received this table yet */
            identity = NULL;
        }
        tables_free();
        return false;
    }
    memset(phys_to_virt(sems), 0, sem_pages * PAGE_SIZE);
    for (size_t i = 0; translate && i < nunits; i++) {
        struct unit *u = &units[i];
        uint64_t *pass = phys_to_virt(u->pass), *root = phys_to_virt(u->root);
        for (unsigned e = 0; e < 256; e++) {
            pass[e * 2] = pass_lo(u);
            pass[e * 2 + 1] = pass_hi(u);
            root[e * 2] = u->pass | CTX_P;
        }
        if (!intel.coherent) {
            cpu_cache_flush(pass, PAGE_SIZE);
            cpu_cache_flush(root, PAGE_SIZE);
        }
    }
    /* any CPU the unit's message can name: a 32-bit destination in Extended Interrupt Mode (unit_tables) */
    if (!fault_irq)
        fault_irq = irq_request_msi(&(struct irq_spec){.name = "VT-d faults", .handler = fault_isr, .node = -1}, IRQ_CPU_ANY,
                                    eime ? UINT32_MAX : IOMMU_PLAIN_REACH);
    uint64_t f = units_hold();
    ok = units_start(translate);
    if (!ok && translate && remap) {
        kprintf("iommu: VT-d did not start translating: trying interrupt remapping alone\n");
        ok = units_start(false);
    }
    units_release(f);
    if (!ok) {
        if (irreq) kprintf("iommu: a VT-d unit that requires interrupt remapping is off: it blocks every interrupt message\n");
        return false; /* the tables stay: a unit may still read them */
    }
    intel.x2apic_ready = eime;
    if (!intel.translating) {
        kprintf("iommu: VT-d remaps interrupts only (%s format), %lu unit%s\n", eime ? "x2APIC" : "xAPIC",
                (unsigned long)nunits, nunits == 1 ? "" : "s");
        return true;
    }
    kprintf("iommu: VT-d on, %lu unit%s, %u-level tables%s, interrupt remapping %s, %u reserved range%s: every "
            "function passes through until an app claims it\n", (unsigned long)nunits, nunits == 1 ? "" : "s",
            intel.levels, pass_all ? "" : " (identity domain for units without pass-through)",
            remap ? eime ? "on (x2APIC format)" : "on (xAPIC format)"
                  : dmar_flags & DMAR_INTR_REMAP ? "not possible on these units" : "not offered by the firmware",
            iommu_reserved_count(), iommu_reserved_count() == 1 ? "" : "s");
    if (!remap) kprintf("iommu: interrupts are not remapped: a device given to an app can send any vector\n");
    return true;
}

static struct iommu_hw intel = {
    .name = "VT-d",
    .table = intel_table,
    .leaf = intel_leaf,
    .is_leaf = intel_is_leaf,
    .start = intel_start,
    .attach = intel_attach,
    .block = intel_block,
    .flush = intel_flush,
    .msi_table = intel_msi_table,
    .msi_write = intel_msi_write,
    .msi_erase = intel_msi_erase,
    .msi_reach = intel_msi_reach,
    .ioapic = intel_ioapic,
    .ioapic_reach = intel_ioapic_reach,
    .resume = intel_resume,
    .shutdown = intel_shutdown,
};

/* Rejection and observation are separate: unsupported queue/table formats do not
   themselves tell us whether firmware left translation or mandatory IR active.
   No command or fault acknowledgement is issued while making this observation. */
static bool units_plain_safe(void) {
    if (units_incomplete || !nunits) return false;
    for (size_t i = 0; i < nunits; i++) {
        const struct unit *u = &units[i];
        if (!u->readable || u->ecap & ECAP_IRREQ ||
            rd32(u, R_GSTS) & (GCMD_TE | GCMD_IRE | GCMD_QIE | GCMD_WBF) ||
            rd64(u, R_CCMD) >> 63 || rd64(u, ECAP_IRO(u->ecap) + 8) >> 63 ||
            (u->cap & CAP_ECMDS && rd64(u, R_ECRSP) & 1) ||
            (u->cap & (CAP_PLMR | CAP_PHMR) && rd32(u, R_PMEN) & (PMEN_EPM | PMEN_PRS))) return false;
    }
    return true;
}

/* Map a described unit and read what identifies it, nothing written. False (and the unit not
   readable) when it cannot be mapped, answers like no VT-d unit, or places registers outside
   its DRHD aperture: the minimum aperture of 4 KiB covers the fixed registers through ECRSP,
   and the capability-selected fault records and IOTLB registers must fit the same reported
   set; an untrusted aperture is never widened to reach them. */
static bool unit_read(struct unit *u) {
    if (!(u->regs = vmm_map_mmio(u->base, u->reg_bytes))) {
        kprintf("iommu: the registers of the VT-d unit at %lx cannot be mapped\n", u->base);
        return false;
    }
    u->cap = rd64(u, R_CAP);
    u->ecap = rd64(u, R_ECAP);
    uint32_t ver = rd32(u, R_VER);
    if (!VER_MAJOR(ver) || ver == UINT32_MAX || u->cap == UINT64_MAX || u->ecap == UINT64_MAX) {
        kprintf("iommu: the VT-d unit at %lx does not give usable register values\n", u->base);
        return false;
    }
    uint64_t fault_end = (uint64_t)CAP_FRO(u->cap) + (uint64_t)CAP_NFR(u->cap) * 16;
    uint64_t iotlb_end = (uint64_t)ECAP_IRO(u->ecap) + 16;
    if (fault_end > u->reg_bytes || iotlb_end > u->reg_bytes) {
        kprintf("iommu: VT-d unit at %lx reports registers outside its DRHD range\n", u->base);
        return false;
    }
    kprintf("iommu: VT-d unit at %lx, version %u.%u, capabilities %lx, extended %lx\n", u->base, VER_MAJOR(ver),
            ver & 15, u->cap, u->ecap);
    return u->readable = true;
}

const struct iommu_hw *iommu_intel_probe(bool *plain_safe) {
    *plain_safe = false;
    if (!acpi_find("DMAR", 0)) { *plain_safe = true; return NULL; }
    /* One DMAR describes every unit (8.1): a second one's units would go unobserved. */
    if (acpi_find("DMAR", 1)) {
        kprintf("iommu: more than one DMAR: VT-d not used\n");
        return NULL;
    }
    parse_dmar();
    /* Every described unit is read before anything is rejected: whether the firmware left one
       translating or remapping is observed (units_plain_safe) even when its description or a
       format is not taken. */
    bool readable = nunits != 0;
    for (size_t i = 0; i < nunits; i++) readable &= unit_read(&units[i]);
    *plain_safe = units_plain_safe();
    if (!readable) {
        kprintf("iommu: %s: VT-d not used\n", nunits ? "a VT-d unit cannot be read" : "the DMAR describes no unit");
        return NULL;
    }
    unsigned sagaw = 0xE, domains = 1u << 16, mgaw = host_bits;
    bool qi = true, eim = true, eimer = false;
    intel.coherent = true;
    remap = (dmar_flags & DMAR_INTR_REMAP) != 0;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        sagaw &= CAP_SAGAW(u->cap);
        if (CAP_MGAW(u->cap) < mgaw) mgaw = CAP_MGAW(u->cap);
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
    intel.iova_bits = intel.levels ? MIN(mgaw, 12 + 9 * intel.levels) : 0;
    /* DMAR HAW bounds physical DMA; the second-stage entry's address field bounds encoding. */
    intel.phys_bits = MIN(host_bits, 64u - (unsigned)__builtin_clzll(ADDR));
    intel.domains = domains > 0xFFFF ? 0xFFFF : (uint16_t)domains;
    if (remap && !ioapics_described()) {
        kprintf("iommu: the IOAPICs' requester IDs are not known: interrupts are not remapped\n");
        remap = false;
    }
    /* Every unit remaps in the mode the CPUs are in or wait for (arch/apic.c chose it, the DMAR's
       X2APIC_OPT_OUT among its reasons): Extended Interrupt Mode, "x2APIC mode is active"
       (11.4.10), needs each unit's EIM (5.1.6); CPUs in x2APIC mode cannot be named otherwise,
       CPUs that wait for it stay in xAPIC mode then. xAPIC destinations need no unit that remaps
       in Extended Interrupt Mode only (EIMER). */
    if (remap && apic_x2() && !eim) {
        kprintf("iommu: a VT-d unit remaps interrupts in xAPIC format only (no EIM), and the CPUs are in x2APIC "
                "mode: interrupts are not remapped\n");
        remap = false;
    }
    if (remap && apic_x2_waits() && !eim)
        kprintf("iommu: a VT-d unit remaps interrupts in xAPIC format only (no EIM): the CPUs stay in xAPIC mode\n");
    eime = remap && eim && (apic_x2() || apic_x2_waits());
    if (remap && !eime && eimer) {
        kprintf("iommu: a VT-d unit remaps interrupts in x2APIC format only (EIMER), and the CPUs are in xAPIC "
                "mode: interrupts are not remapped\n");
        remap = false;
    }
    intel.interrupt_remapping_required = irreq;
    if (parse_lost) {
        kprintf("iommu: the DMAR scope or structure is not supported: VT-d not used\n");
        return NULL;
    }
    /* Units that remap interrupts are started to remap them even where nothing else of VT-d is
       used (intel_start). Without the queue (which remapping needs for its invalidations), or
       without a page table format and the remapping, they are left as the firmware set them. */
    const char *unused = !qi ? "a VT-d unit has no invalidation queue"
                       : irreq && !remap ? "a VT-d unit requires interrupt remapping, which these units cannot give"
                       : !intel.levels && !remap ? "the VT-d units share no page table format" : NULL;
    if (unused) {
        kprintf("iommu: %s: VT-d not used%s\n", unused,
                irreq ? "; unless the firmware left remapping on, every interrupt message is blocked" : "");
        return NULL;
    }
    if (!intel.levels) kprintf("iommu: the VT-d units share no page table format: DMA is not translated\n");
    return &intel;
}
