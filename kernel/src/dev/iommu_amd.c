/* AMD-Vi, per the AMD I/O Virtualization Technology (IOMMU) specification (48882): its MMIO
   registers, device table, I/O page tables, interrupt remapping, commands, event log and the
   IVRS table that describes the units.
   One device table (64K entries of 32 bytes) serves every unit; each unit reads only the
   entries of the functions behind it, and commands go to all of them. An entry left zero lets
   a function through: DMA untranslated, interrupts as sent. A claimed function's entries point
   at its domain's page tables and at an interrupt remapping table of its own whose entries are
   the claim's vectors; once blocked they let nothing through. The IOAPICs, the HPET and the
   kernel's devices keep zero entries, so their interrupts never depend on the units. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define MAX_UNITS   8
#define MAX_ALIASES 64

/* MMIO registers */
#define R_DEVTAB    0x0000
#define R_CMDBUF    0x0008
#define R_EVENTLOG  0x0010
#define R_CONTROL   0x0018
#define R_EFR       0x0030
#define R_CMD_HEAD  0x2000
#define R_CMD_TAIL  0x2008
#define R_EVT_HEAD  0x2010
#define R_EVT_TAIL  0x2018
#define R_STATUS    0x2020
#define MMIO_BYTES  0x4000

#define CTL_IOMMU_EN    (1ull << 0)
#define CTL_EVENTLOG_EN (1ull << 2)
#define CTL_EVENTINT_EN (1ull << 3)
#define CTL_CMDBUF_EN   (1ull << 12)
#define ST_EVENT_OVERFLOW (1u << 0)
#define ST_EVENT_INT      (1u << 1)
#define ST_COMWAIT_INT    (1u << 2)
#define ST_EVENTLOG_RUN   (1u << 3)
#define ST_CMDBUF_RUN     (1u << 4)
#define EFR_IA (1ull << 6) /* INVALIDATE_IOMMU_ALL */
#define CAP_NP_CACHE (1u << 26) /* caches not-present entries: a map needs an invalidation too */
#define CAP_EFR      (1u << 27)

#define RING_BYTES 4096           /* command buffer and event log: 256 entries of 16 bytes */
#define RING_LEN   (8ull << 56)   /* log2 of their entry count */
#define DT_BYTES   (65536 * 32)
#define IRT_LOG2   5              /* a claim's 32 vectors */

/* device table entry: qword 0 DMA, 1 domain, 2 interrupts */
#define DTE_V        (1ull << 0)
#define DTE_TV       (1ull << 1)
#define DTE_MODE(l)  ((uint64_t)(l) << 9)
#define DTE_IR       (1ull << 61)
#define DTE_IW       (1ull << 62)
#define DTE_IV       (1ull << 0)
#define DTE_IRT_LEN(n) ((uint64_t)(n) << 1)
#define DTE_INT_REMAP (2ull << 60) /* IntCtl: fixed and arbitrated interrupts remapped; 00 aborts */
/* I/O page table entry */
#define PTE_PR (1ull << 0)
#define PTE_IR (1ull << 61)
#define PTE_IW (1ull << 62)

enum { CMD_COMPLETION_WAIT = 1, CMD_INV_DEVTAB = 2, CMD_INV_PAGES = 3, CMD_INV_IRT = 5, CMD_INV_ALL = 8 };
enum { EVENT_ILLEGAL_DTE = 1, EVENT_PAGE_FAULT = 2 };

struct unit {
    uint64_t base;
    volatile uint8_t *regs;
    struct pci_device *fn; /* its PCI function: the event interrupt is its MSI */
    uint8_t type, flags;   /* the IVHD the unit's description came from */
    uint16_t cap;          /* its capability in the function's config space */
    uint64_t efr;
    uint64_t cmd, events;  /* rings */
    uint32_t cmd_tail;
};
static struct unit units[MAX_UNITS];
static unsigned nunits;
static bool np_cache; /* a unit caches not-present entries */
static uint64_t devtab, sems; /* device table; one completion word a unit */
static uint64_t token;
static int event_vector = -1;

/* IVRS: requests of first..last arrive as `as` */
struct alias {
    uint16_t first, last, as;
};
static struct alias aliases[MAX_ALIASES];
static unsigned naliases;

static struct iommu_hw amd;

static uint64_t rd(const struct unit *u, unsigned off) { return *(volatile uint64_t *)(u->regs + off); }
static void wr(const struct unit *u, unsigned off, uint64_t v) { *(volatile uint64_t *)(u->regs + off) = v; }

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- IVRS ---- */
static void alias_add(uint16_t first, uint16_t last, uint16_t as) {
    if (naliases < MAX_ALIASES && first <= last) aliases[naliases++] = (struct alias){first, last, as};
}

/* Device entries: 4 bytes below type 40h, 8 below 80h, ACPI HID entries (F0h) 22 plus their
   UID. Only aliases matter here: zero entries already let every function through. */
static void parse_devices(const uint8_t *b, uint32_t at, uint32_t end) {
    uint16_t first = 0, as = 0;
    bool alias_range = false;
    while (at + 4 <= end) {
        uint8_t type = b[at];
        uint32_t len = type < 0x40 ? 4 : type < 0x80 ? 8 : type == 0xF0 && at + 22 <= end ? 22u + b[at + 21] : 0;
        if (!len || at + len > end) break;
        uint16_t id = u16(b + at + 1);
        if (type == 0x42) alias_add(id, id, u16(b + at + 5));
        else if (type == 0x43) first = id, as = u16(b + at + 5), alias_range = true;
        else if (type == 0x03 || type == 0x47) alias_range = false;
        else if (type == 0x04 && alias_range) alias_add(first, id, as), alias_range = false;
        at += len;
    }
}

/* IVMD: memory a function reaches by DMA from the firmware's start (unity mapped, or excluded
   from translation): a claimed function's domain maps it as it is. */
static void parse_memory(const uint8_t *b) {
    if (!(b[1] & (1u << 0 | 1u << 3))) return;
    uint64_t start, length;
    memcpy(&start, b + 16, 8);
    memcpy(&length, b + 24, 8);
    uint16_t first = b[0] == 0x20 ? 0 : u16(b + 4), last = b[0] == 0x20 ? 0xFFFF : b[0] == 0x22 ? u16(b + 6) : first;
    if (length) iommu_reserved(first, last, ALIGN_DOWN(start, PAGE_SIZE), ALIGN_UP(start + length, PAGE_SIZE));
}

static struct unit *unit_at(uint64_t base) {
    for (unsigned i = 0; i < nunits; i++)
        if (units[i].base == base) return &units[i];
    return NULL;
}

/* Types 10h, 11h and 40h describe the same unit in more detail each: the most detailed one
   this code reads is used. */
static bool parse_ivrs(void) {
    const struct acpi_sdt *h = acpi_find("IVRS", 0);
    if (!h || h->length < 48) return false;
    const uint8_t *b = (const uint8_t *)h;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t off = 48; off + 4 <= h->length;) {
            uint8_t type = b[off];
            uint16_t len = u16(b + off + 2);
            if (len < 4 || off + len > h->length) break;
            if ((type == 0x10 || type == 0x11 || type == 0x40) && len >= 24) {
                uint64_t base;
                memcpy(&base, b + off + 8, 8);
                struct unit *u = unit_at(base);
                if (pass == 0 && u16(b + off + 16) != 0) {
                    kprintf("iommu: AMD-Vi unit at %lx is on PCI segment %u, which Nocturne does not reach\n",
                            base, u16(b + off + 16));
                } else if (pass == 0 && base && !u && nunits < MAX_UNITS) {
                    u = &units[nunits++];
                    u->base = base;
                    u->type = type;
                } else if (pass == 0 && u && type > u->type) {
                    u->type = type;
                } else if (pass == 1 && u && u->type == type) {
                    uint16_t id = u16(b + off + 4);
                    u->flags = b[off + 1];
                    u->cap = u16(b + off + 6);
                    u->fn = pci_find_function(id >> 8, (id >> 3) & 31, id & 7);
                    parse_devices(b, off + (type == 0x10 ? 24 : 40), off + len);
                }
            } else if (pass == 1 && type >= 0x20 && type <= 0x22 && len >= 32) {
                parse_memory(b + off);
            }
            off += len;
        }
    }
    return nunits != 0;
}

/* ---- commands and events ---- */
static bool command(struct unit *u, unsigned op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t next = (u->cmd_tail + 16) % RING_BYTES;
    for (uint64_t t = iommu_deadline(100); (rd(u, R_CMD_HEAD) & 0x7FFF0) == next; pause())
        if (iommu_expired(t)) return false;
    volatile uint32_t *slot = (volatile uint32_t *)((uint8_t *)phys_to_virt(u->cmd) + u->cmd_tail);
    slot[0] = a;
    slot[1] = b | (uint32_t)op << 28;
    slot[2] = c;
    slot[3] = d;
    if (!amd.coherent) iommu_cache_flush(slot, 16);
    u->cmd_tail = next;
    wr(u, R_CMD_TAIL, next);
    return true;
}

static void send(unsigned op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (unsigned i = 0; i < nunits; i++) command(&units[i], op, a, b, c, d);
}

/* Every page of a domain, the page directory caches with them. */
static void send_domain_flush(uint16_t id) { send(CMD_INV_PAGES, 0, id, 0xFFFFF000u | 3, 0x7FFFFFFFu); }

static void event_show(const struct unit *u, const volatile uint32_t *e) {
    unsigned code = e[1] >> 28;
    uint16_t id = e[0] & 0xFFFF;
    uint64_t address = e[2] | (uint64_t)e[3] << 32;
    if (!iommu_fault_report()) return;
    if (code == EVENT_PAGE_FAULT) {
        uint32_t flags = e[1] >> 16;
        kprintf("iommu: %02x:%02x.%u: %s at %lx blocked (domain %u)\n", id >> 8, (id >> 3) & 31, id & 7,
                flags & 8 ? "interrupt" : flags & 32 ? "DMA write" : "DMA read", address, e[1] & 0xFFFF);
    } else if (code == EVENT_ILLEGAL_DTE) {
        kprintf("iommu: %02x:%02x.%u: the unit rejected its device table entry\n", id >> 8, (id >> 3) & 31, id & 7);
    } else {
        kprintf("iommu: unit at %lx reports event %u (%08x %08x %08x %08x)\n", u->base, code, e[0], e[1], e[2], e[3]);
    }
}

static void events_drain(struct unit *u) {
    uint32_t status = (uint32_t)rd(u, R_STATUS);
    wr(u, R_STATUS, status & (ST_EVENT_INT | ST_EVENT_OVERFLOW));
    uint32_t head = rd(u, R_EVT_HEAD) & 0x7FFF0, tail = rd(u, R_EVT_TAIL) & 0x7FFF0;
    for (; head != tail; head = (head + 16) % RING_BYTES)
        event_show(u, (const volatile uint32_t *)((uint8_t *)phys_to_virt(u->events) + head));
    wr(u, R_EVT_HEAD, head);
    if (status & ST_EVENT_OVERFLOW) { /* the log stopped: start it again, empty */
        uint64_t ctl = rd(u, R_CONTROL);
        wr(u, R_CONTROL, ctl & ~CTL_EVENTLOG_EN);
        wr(u, R_EVT_HEAD, 0);
        wr(u, R_EVT_TAIL, 0);
        wr(u, R_CONTROL, ctl);
        kprintf("iommu: unit at %lx: its event log overflowed\n", u->base);
    }
}

static void event_isr(struct regs *r) {
    (void)r;
    for (unsigned i = 0; i < nunits; i++) events_drain(&units[i]);
}

/* Waits until every unit has done every command sent before. */
static bool sync(void) {
    volatile uint64_t *word = phys_to_virt(sems);
    token++;
    bool ok = true;
    for (unsigned i = 0; i < nunits; i++) {
        uint64_t at = sems + i * 8;
        word[i] = 0;
        ok &= command(&units[i], CMD_COMPLETION_WAIT, (uint32_t)at | 1, (uint32_t)(at >> 32) & 0xFFFFF,
                      (uint32_t)token, (uint32_t)(token >> 32));
    }
    for (unsigned i = 0; ok && i < nunits; i++)
        for (uint64_t t = iommu_deadline(100); word[i] != token; pause())
            if (iommu_expired(t)) {
                kprintf("iommu: unit at %lx did not finish its commands\n", units[i].base);
                ok = false;
                break;
            }
    for (unsigned i = 0; i < nunits; i++) events_drain(&units[i]);
    return ok;
}

/* ---- device table ---- */
static void dte_write(uint16_t id, uint64_t q0, uint64_t q1, uint64_t q2) {
    volatile uint64_t *e = (volatile uint64_t *)phys_to_virt(devtab) + id * 4;
    e[3] = 0;
    e[2] = q2;
    e[1] = q1;
    e[0] = q0; /* valid last */
    if (!amd.coherent) iommu_cache_flush(e, 32);
}

static void send_entry_flush(const struct iommu_domain *dom) {
    for (unsigned i = 0; i < dom->nids; i++) {
        send(CMD_INV_DEVTAB, dom->ids[i], 0, 0, 0);
        send(CMD_INV_IRT, dom->ids[i], 0, 0, 0);
    }
}

static bool amd_attach(struct iommu_domain *dom) {
    if (!(dom->irt = pmm_alloc_zeroed())) return false;
    if (!amd.coherent) iommu_cache_flush(phys_to_virt(dom->irt), PAGE_SIZE);
    uint64_t f = irq_save();
    send_domain_flush(dom->id); /* nothing the firmware cached under this id is used */
    sync();
    for (unsigned i = 0; i < dom->nids; i++)
        dte_write(dom->ids[i], DTE_V | DTE_TV | DTE_MODE(dom->levels) | dom->root | DTE_IR | DTE_IW, dom->id,
                  DTE_IV | DTE_IRT_LEN(IRT_LOG2) | dom->irt | DTE_INT_REMAP);
    send_entry_flush(dom);
    sync();
    irq_restore(f);
    return true;
}

/* Valid, translation on, mode 0 (no page tables) with neither read nor write allowed: every DMA
   aborts. Interrupts valid with IntCtl 00 and no pass bits: every interrupt aborts. */
static bool amd_block(struct iommu_domain *dom) {
    uint64_t f = irq_save();
    for (unsigned i = 0; i < dom->nids; i++) dte_write(dom->ids[i], DTE_V | DTE_TV, dom->id, DTE_IV);
    send_entry_flush(dom);
    send_domain_flush(dom->id);
    bool ok = sync();
    irq_restore(f);
    return ok;
}

static bool amd_flush(struct iommu_domain *dom, bool unmapped) {
    if (!unmapped && !np_cache) return true;
    uint64_t f = irq_save();
    send_domain_flush(dom->id);
    bool ok = sync();
    irq_restore(f);
    return ok;
}

static uint64_t amd_table(uint64_t phys, unsigned level) {
    return phys | (uint64_t)(level - 1) << 9 | PTE_IR | PTE_IW | PTE_PR; /* NextLevel: the table's */
}
static uint64_t amd_leaf(uint64_t phys, unsigned level) {
    (void)level; /* 4 KiB pages only: NextLevel 0 */
    return phys | PTE_IR | PTE_IW | PTE_PR;
}

static uint16_t amd_alias(uint16_t id) {
    for (unsigned i = 0; i < naliases; i++)
        if (id >= aliases[i].first && id <= aliases[i].last) return aliases[i].as;
    return id;
}

/* ---- interrupts ----
   A claimed function's message carries its remapping table index in data bits 10:0: entry n
   is message n, fixed delivery to a physical APIC id (the 32-bit entry format). */
static bool irt_write(struct iommu_domain *dom, unsigned first, unsigned count, uint8_t vector, uint32_t dest,
                      bool on) {
    volatile uint32_t *irt = phys_to_virt(dom->irt);
    for (unsigned i = 0; i < count; i++)
        irt[first + i] = on ? 1u | dest << 8 | (uint32_t)(uint8_t)(vector + i) << 16 : 0;
    if (!amd.coherent) iommu_cache_flush(irt + first, count * 4);
    uint64_t f = irq_save();
    for (unsigned i = 0; i < dom->nids; i++) send(CMD_INV_IRT, dom->ids[i], 0, 0, 0);
    bool ok = sync();
    irq_restore(f);
    return ok;
}

static bool amd_msi(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                    uint32_t *address, uint32_t *data) {
    if (dest > 0xFF) return false;
    struct iommu_domain *dom = iommu_domain_of(d);
    *address = iommu_msi_plain(dest);
    *data = dom ? index : vector;
    if (!dom) return true;
    return index + count <= 1u << IRT_LOG2 && irt_write(dom, index, count, vector, dest, true);
}

static void amd_msi_clear(struct pci_device *d) {
    struct iommu_domain *dom = iommu_domain_of(d);
    if (dom) irt_write(dom, 0, 1u << IRT_LOG2, 0, 0, false);
}

static uint64_t amd_ioapic(uint8_t ioapic_id, uint32_t gsi, uint64_t rte) {
    (void)ioapic_id, (void)gsi;
    return rte;
}

/* ---- the units ---- */
static void program(struct unit *u) {
    wr(u, R_CONTROL, 0); /* off, also when the firmware left it on */
    for (uint64_t t = iommu_deadline(10); rd(u, R_STATUS) & (ST_EVENTLOG_RUN | ST_CMDBUF_RUN) && !iommu_expired(t);)
        pause();
    wr(u, R_DEVTAB, devtab | (DT_BYTES / PAGE_SIZE - 1));
    wr(u, R_CMDBUF, u->cmd | RING_LEN);
    wr(u, R_EVENTLOG, u->events | RING_LEN);
    wr(u, R_CMD_HEAD, 0);
    wr(u, R_CMD_TAIL, 0);
    wr(u, R_EVT_HEAD, 0);
    wr(u, R_EVT_TAIL, 0);
    u->cmd_tail = 0;
    wr(u, R_STATUS, ST_EVENT_OVERFLOW | ST_EVENT_INT | ST_COMWAIT_INT);
    /* the IVHD flags HtTunEn, PassPW, ResPassPW, Isoc and Coherent are the firmware's settings
       for control bits 1, 8, 9, 11 and 10 */
    uint64_t ctl = CTL_IOMMU_EN | CTL_EVENTLOG_EN | CTL_CMDBUF_EN;
    if (u->flags & 1) ctl |= 1ull << 1;
    if (u->flags & 2) ctl |= 1ull << 8;
    if (u->flags & 4) ctl |= 1ull << 9;
    if (u->flags & 8) ctl |= 1ull << 11;
    if (u->flags & 32) ctl |= 1ull << 10;
    if (event_vector >= 0 && u->fn) ctl |= CTL_EVENTINT_EN;
    wr(u, R_CONTROL, ctl);
}

/* What a unit may have cached before it was ours, or before a sleep. */
static void send_everything_flush(void) {
    bool all = true;
    for (unsigned i = 0; i < nunits; i++) all &= (units[i].efr & EFR_IA) != 0;
    if (all) {
        send(CMD_INV_ALL, 0, 0, 0, 0);
        return;
    }
    for (uint32_t id = 0; id < 65536; id++) {
        send(CMD_INV_DEVTAB, id, 0, 0, 0);
        send(CMD_INV_IRT, id, 0, 0, 0);
    }
}

static void amd_shutdown(void) {
    for (unsigned i = 0; i < nunits; i++) wr(&units[i], R_CONTROL, 0);
}

static void amd_resume(void) {
    for (unsigned i = 0; i < nunits; i++) program(&units[i]);
    send_everything_flush();
    sync();
}

static bool amd_start(void) {
    devtab = pmm_alloc_contig(DT_BYTES / PAGE_SIZE);
    sems = pmm_alloc_zeroed();
    bool ok = devtab && sems;
    for (unsigned i = 0; i < nunits; i++) {
        units[i].cmd = pmm_alloc_zeroed();
        units[i].events = pmm_alloc_zeroed();
        ok &= units[i].cmd && units[i].events;
    }
    if (!ok) {
        kprintf("iommu: no memory for the AMD-Vi tables\n");
        for (uint64_t at = 0; devtab && at < DT_BYTES; at += PAGE_SIZE) pmm_free(devtab + at);
        if (sems) pmm_free(sems);
        for (unsigned i = 0; i < nunits; i++) {
            if (units[i].cmd) pmm_free(units[i].cmd);
            if (units[i].events) pmm_free(units[i].events);
        }
        return false;
    }
    memset(phys_to_virt(devtab), 0, DT_BYTES);
    if (!amd.coherent) iommu_cache_flush(phys_to_virt(devtab), DT_BYTES);
    event_vector = irq_vector_alloc(1, 1);
    if (event_vector >= 0) vector_register(event_vector, event_isr);
    uint64_t f = irq_save();
    for (unsigned i = 0; i < nunits; i++) program(&units[i]);
    send_everything_flush();
    ok = sync();
    irq_restore(f);
    if (!ok) {
        amd_shutdown();
        return false;
    }
    for (unsigned i = 0; i < nunits; i++) {
        struct pci_device *fn = units[i].fn;
        if (!fn) continue;
        pci_enable_busmaster(fn); /* the kernel's: no app may claim the unit */
        if (event_vector < 0 || !pci_msi_enable(fn, (uint8_t)event_vector))
            kprintf("iommu: unit at %lx has no event interrupt: blocked requests show after commands only\n",
                    units[i].base);
    }
    kprintf("iommu: AMD-Vi on, %u unit%s; %u alias%s, %u reserved range%s: every function passes through "
            "until an app claims it\n", nunits, nunits == 1 ? "" : "s", naliases, naliases == 1 ? "" : "es",
            iommu_reserved_count(), iommu_reserved_count() == 1 ? "" : "s");
    return true;
}

static struct iommu_hw amd = {
    .name = "AMD-Vi",
    .levels = 4,
    .domains = 0xFFFF,
    .table = amd_table,
    .leaf = amd_leaf,
    .start = amd_start,
    .alias = amd_alias,
    .attach = amd_attach,
    .block = amd_block,
    .flush = amd_flush,
    .msi = amd_msi,
    .msi_clear = amd_msi_clear,
    .ioapic = amd_ioapic,
    .resume = amd_resume,
    .shutdown = amd_shutdown,
};

const struct iommu_hw *iommu_amd_probe(void) {
    if (!parse_ivrs()) return NULL;
    amd.coherent = true;
    for (unsigned i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        u->regs = vmm_map_mmio(u->base, MMIO_BYTES);
        /* without its function the capability header is unknown: assume the cautious answers */
        uint32_t header = u->fn ? pci_read32(u->fn->bus, u->fn->dev, u->fn->func, u->cap) : CAP_NP_CACHE;
        u->efr = header & CAP_EFR ? rd(u, R_EFR) : 0;
        np_cache |= (header & CAP_NP_CACHE) != 0;
        amd.coherent &= (u->flags & 32) != 0;
        kprintf("iommu: AMD-Vi unit at %lx (IVHD %02xh, function %02x:%02x.%u), extended features %lx\n", u->base,
                u->type, u->fn ? u->fn->bus : 0, u->fn ? u->fn->dev : 0, u->fn ? u->fn->func : 0, u->efr);
    }
    return &amd;
}
