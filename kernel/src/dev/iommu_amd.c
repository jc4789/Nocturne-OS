/* AMD-Vi, per the AMD I/O Virtualization Technology (IOMMU) specification (48882): its MMIO
   registers, device table, I/O page tables, interrupt remapping, commands, event log and the
   IVRS table that describes the units.
   One device table (64K entries of 32 bytes) serves every unit; each unit reads only the
   entries of the functions behind it, and commands go to all of them. An entry left zero lets
   a function through: DMA untranslated, interrupts as sent. A claimed function's group has
   entries pointing at its domain's page tables and at an interrupt remapping table of the
   claim's own whose entries are the claim's vectors; once blocked they let nothing through. The
   IOAPICs, the HPET and the kernel's devices keep zero entries, so their interrupts never depend
   on the units. The units cannot block system management interrupts by device table entry (only
   their SMI filter can, which the firmware's own SMIs may need): a claimed device that sends an
   SMI message reaches the firmware's handler as on a machine without an IOMMU. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "dev/acpi.h"
#include "dev/iommu.h"
#include "dev/pci.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* MMIO registers */
#define R_DEVTAB    0x0000
#define R_CMDBUF    0x0008
#define R_EVENTLOG  0x0010
#define R_CONTROL   0x0018
#define R_EFR       0x0030
#define R_PERF_OPT  0x016C
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
#define EFR_IA       (1ull << 6)  /* INVALIDATE_IOMMU_ALL */
#define EFR_PERF_OPT (1ull << 45) /* the performance optimization register (3.4.9) */
#define PERF_OPT_EN  (1u << 13)
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
/* I/O page table entry: what a request may do is what every entry on its way allows */
#define PTE_PR (1ull << 0)
#define PTE_IR (1ull << 61)
#define PTE_IW (1ull << 62)

enum { CMD_COMPLETION_WAIT = 1, CMD_INV_DEVTAB = 2, CMD_INV_PAGES = 3, CMD_INV_IRT = 5, CMD_INV_ALL = 8 };
enum { EVENT_ILLEGAL_DTE = 1, EVENT_PAGE_FAULT = 2 };

/* IVRS: device IDs first..last */
struct span {
    uint16_t first, last;
};

struct unit {
    uint64_t base;
    volatile uint8_t *regs;
    struct pci_device *fn; /* its PCI function: the event interrupt is its MSI */
    uint8_t type, flags;   /* the IVHD the unit's description came from */
    uint16_t cap;          /* its capability in the function's config space */
    uint64_t efr;
    uint64_t cmd, events;  /* rings */
    uint32_t cmd_tail;
    bool lost;             /* a command since the last sync was not done: that sync fails */
    bool overflow_told;    /* its event log overflowed and is still writing: said once */
    struct span *listed;   /* the IDs its IVHD names: those it may have cached entries of */
    size_t nlisted, listed_room;
};
static struct unit *units;
static size_t nunits, units_room;
static bool np_cache; /* a unit caches not-present entries */
static uint64_t devtab, sems; /* device table; one completion word a unit */
static size_t sem_pages;
static uint64_t token;
static int event_vector = -1;

/* IVRS: requests of first..last arrive as `as` */
struct alias {
    uint16_t first, last, as;
};
static struct alias *aliases;
static size_t naliases, aliases_room;
static bool parse_lost; /* no memory kept part of the IVRS: the units are not used */

static struct iommu_hw amd;

static uint64_t rd(const struct unit *u, unsigned off) { return *(volatile uint64_t *)(u->regs + off); }
static void wr(const struct unit *u, unsigned off, uint64_t v) { *(volatile uint64_t *)(u->regs + off) = v; }

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- IVRS ---- */
static void alias_add(uint16_t first, uint16_t last, uint16_t as) {
    if (first > last) return;
    struct alias *a = kappend(&aliases, &naliases, &aliases_room, sizeof *a);
    if (a) *a = (struct alias){first, last, as};
    else parse_lost = true;
}

/* IDs the unit sees requests with: covered (a claim may isolate them), and named, so that what
   it cached of them is invalidated where it cannot invalidate everything at once */
static void listed_add(struct unit *u, uint16_t first, uint16_t last, bool cover) {
    if (first > last) return;
    if (cover) iommu_cover(first, last);
    struct span *s = kappend(&u->listed, &u->nlisted, &u->listed_room, sizeof *s);
    if (s) *s = (struct span){first, last};
    else parse_lost = true;
}

/* Device entries (Table 95 on): 4 bytes below type 40h, 8 below 80h, ACPI HID entries (F0h) 22
   plus their UID. Select (02h, 42h, 46h), all (01h), and ranges from a start (03h, 43h, 47h) to
   an end (04h); 42h and 43h add the ID the requests arrive as. Special devices (48h) are the
   IOAPICs and HPETs, the platform's. Their DTE settings (byte 3: the LINT, NMI, INIT and ExtInt
   passes) are for the entries the firmware writes; a claim's entries pass none. */
static void parse_devices(struct unit *u, const uint8_t *b, uint32_t at, uint32_t end) {
    uint16_t first = 0, as = 0;
    bool range = false, alias_range = false;
    while (at + 4 <= end) {
        uint8_t type = b[at];
        uint32_t len = type < 0x40 ? 4 : type < 0x80 ? 8 : type == 0xF0 && at + 22 <= end ? 22u + b[at + 21] : 0;
        if (!len || at + len > end) break;
        uint16_t id = u16(b + at + 1);
        switch (type) {
        case 0x01: listed_add(u, 0, 0xFFFF, true); break;
        case 0x02: case 0x46: listed_add(u, id, id, true); break;
        case 0x03: case 0x47: first = id, range = true, alias_range = false; break;
        case 0x42:
            listed_add(u, id, id, true);
            listed_add(u, u16(b + at + 5), u16(b + at + 5), false);
            alias_add(id, id, u16(b + at + 5));
            break;
        case 0x43: first = id, as = u16(b + at + 5), range = alias_range = true; break;
        case 0x04:
            if (range) listed_add(u, first, id, true);
            if (range && alias_range) {
                listed_add(u, as, as, false);
                alias_add(first, id, as);
            }
            range = alias_range = false;
            break;
        case 0x48:
            iommu_kernel_id(u16(b + at + 5));
            listed_add(u, u16(b + at + 5), u16(b + at + 5), false);
            break;
        case 0xF0: listed_add(u, id, id, false); break;
        }
        at += len;
    }
}

/* IVMD: memory a function reaches by DMA from the firmware's start: unity mapped with the
   access its IR and IW flags give (none: not mapped), or excluded from translation (then
   reached as it is, the flags ignored). A claimed function's domain maps it as it is. */
static void parse_memory(const uint8_t *b) {
    unsigned flags = b[1], access = flags & 8 ? IOMMU_READ | IOMMU_WRITE
                                  : flags & 1 ? (flags & 2 ? IOMMU_READ : 0) | (flags & 4 ? IOMMU_WRITE : 0) : 0;
    if (!access) return;
    uint64_t start, length;
    memcpy(&start, b + 16, 8);
    memcpy(&length, b + 24, 8);
    uint16_t first = b[0] == 0x20 ? 0 : u16(b + 4), last = b[0] == 0x20 ? 0xFFFF : b[0] == 0x22 ? u16(b + 6) : first;
    if (length) iommu_reserved(first, last, ALIGN_DOWN(start, PAGE_SIZE), ALIGN_UP(start + length, PAGE_SIZE), access);
}

static struct unit *unit_at(uint64_t base) {
    for (size_t i = 0; i < nunits; i++)
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
                } else if (pass == 0 && !base) {
                    kprintf("iommu: the IVRS describes an AMD-Vi unit without an address; not used\n");
                } else if (pass == 0 && !u) {
                    if ((u = kappend(&units, &nunits, &units_room, sizeof *u)))
                        *u = (struct unit){.base = base, .type = type};
                    else
                        parse_lost = true;
                } else if (pass == 0 && type > u->type) {
                    u->type = type;
                } else if (pass == 1 && u && u->type == type) {
                    uint16_t id = u16(b + off + 4);
                    u->flags = b[off + 1];
                    u->cap = u16(b + off + 6);
                    u->fn = pci_find_function(id >> 8, (id >> 3) & 31, id & 7);
                    parse_devices(u, b, off + (type == 0x10 ? 24 : 40), off + len);
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
/* Waits until the status bits `bits` read `want`: false when they did not in time. */
static bool status_wait(const struct unit *u, uint32_t bits, uint32_t want) {
    for (uint64_t t = iommu_deadline(10); (rd(u, R_STATUS) & bits) != want; pause())
        if (iommu_expired(t)) return false;
    return true;
}

/* CmdBufRun is a level: set once CmdBufEn is written with 1 and the unit fetches commands, clear
   only when it halts them for cause, after an illegal or failed command (events 5 and 6), and
   then clear until CmdBufEn is written with 1 again (3.4.16). The unit is waited for after every
   such write, so reading it clear after that says the unit halted. It starts again as 2.4 says:
   with CmdBufRun clear, CmdBufEn off, the pointers set, CmdBufEn on; its buffer empty. What it
   had not done is lost, and the next sync says so. */
static bool commands_started(const struct unit *u) {
    if (status_wait(u, ST_CMDBUF_RUN, ST_CMDBUF_RUN)) return true;
    kprintf("iommu: unit at %lx does not start fetching commands\n", u->base);
    return false;
}

static void commands_restart(struct unit *u) {
    kprintf("iommu: unit at %lx stopped taking commands; started again\n", u->base);
    uint64_t ctl = rd(u, R_CONTROL);
    wr(u, R_CONTROL, ctl & ~CTL_CMDBUF_EN);
    wr(u, R_CMD_HEAD, 0);
    wr(u, R_CMD_TAIL, 0);
    u->cmd_tail = 0;
    u->lost = true;
    wr(u, R_CONTROL, ctl | CTL_CMDBUF_EN);
    commands_started(u);
}

/* A command for one unit: one it has no room for in time is lost (u->lost). */
static void command(struct unit *u, unsigned op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint64_t t = iommu_deadline(100); (rd(u, R_CMD_HEAD) & 0x7FFF0) == (u->cmd_tail + 16) % RING_BYTES; pause()) {
        if (!(rd(u, R_STATUS) & ST_CMDBUF_RUN)) {
            commands_restart(u);
            break;
        }
        if (iommu_expired(t)) {
            u->lost = true;
            return;
        }
    }
    volatile uint32_t *slot = (volatile uint32_t *)((uint8_t *)phys_to_virt(u->cmd) + u->cmd_tail);
    slot[0] = a;
    slot[1] = b | (uint32_t)op << 28;
    slot[2] = c;
    slot[3] = d;
    if (!amd.coherent) iommu_cache_flush(slot, 16);
    u->cmd_tail = (u->cmd_tail + 16) % RING_BYTES;
    wr(u, R_CMD_TAIL, u->cmd_tail);
}

static void send(unsigned op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (size_t i = 0; i < nunits; i++) command(&units[i], op, a, b, c, d);
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
    wr(u, R_STATUS, status & ST_EVENT_INT);
    uint32_t head = rd(u, R_EVT_HEAD) & 0x7FFF0, tail = rd(u, R_EVT_TAIL) & 0x7FFF0;
    for (; head != tail; head = (head + 16) % RING_BYTES)
        event_show(u, (const volatile uint32_t *)((uint8_t *)phys_to_virt(u->events) + head));
    wr(u, R_EVT_HEAD, head);
    /* The log overflowed: once it stopped writing (EventLogRun clear, the only time its registers
       may change) it starts again, empty (2.5.1). Until then it is tried again at the next drain. */
    if (status & ST_EVENT_OVERFLOW) {
        if (!status_wait(u, ST_EVENTLOG_RUN, 0)) {
            if (!u->overflow_told)
                kprintf("iommu: unit at %lx: its event log overflowed and has not stopped yet\n", u->base);
            u->overflow_told = true;
            return;
        }
        u->overflow_told = false;
        /* CmdBufEn written with 1 restarts a halted command buffer where it halted: one that
           halted is left to commands_restart */
        uint64_t ctl = rd(u, R_CONTROL);
        if (!(rd(u, R_STATUS) & ST_CMDBUF_RUN)) ctl &= ~CTL_CMDBUF_EN;
        wr(u, R_CONTROL, ctl & ~CTL_EVENTLOG_EN);
        wr(u, R_EVT_HEAD, 0);
        wr(u, R_EVT_TAIL, 0);
        wr(u, R_STATUS, ST_EVENT_OVERFLOW);
        wr(u, R_CONTROL, ctl | CTL_EVENTLOG_EN);
        kprintf("iommu: unit at %lx: its event log overflowed\n", u->base);
    }
}

static void event_isr(struct regs *r) {
    (void)r;
    for (size_t i = 0; i < nunits; i++) events_drain(&units[i]);
}

/* Waits until every unit has done every command sent since the last sync: false when one was
   lost or a unit did not finish them in time. */
static bool sync(void) {
    volatile uint64_t *word = phys_to_virt(sems);
    token++;
    for (size_t i = 0; i < nunits; i++) {
        uint64_t at = sems + i * 8;
        word[i] = 0;
        command(&units[i], CMD_COMPLETION_WAIT, (uint32_t)at | 1, (uint32_t)(at >> 32) & 0xFFFFF, (uint32_t)token,
                (uint32_t)(token >> 32));
    }
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        for (uint64_t t = iommu_deadline(100); !u->lost && word[i] != token; pause()) {
            if (!(rd(u, R_STATUS) & ST_CMDBUF_RUN)) {
                commands_restart(u);
            } else if (iommu_expired(t)) {
                kprintf("iommu: unit at %lx did not finish its commands\n", u->base);
                u->lost = true;
            }
        }
    }
    bool ok = true;
    for (size_t i = 0; i < nunits; i++) {
        events_drain(&units[i]);
        ok &= !units[i].lost;
        units[i].lost = false;
    }
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

/* The device table and interrupt remapping caches keep the IDs' entries (2.3.2): both go. */
static void send_entry_flush(const struct iommu_domain *dom) {
    for (unsigned i = 0; i < dom->nids; i++) {
        send(CMD_INV_DEVTAB, dom->ids[i], 0, 0, 0);
        send(CMD_INV_IRT, dom->ids[i], 0, 0, 0);
    }
}

static bool amd_attach(struct iommu_domain *dom) {
    if (!(dom->irt = pmm_alloc_zeroed())) {
        kprintf("iommu: no memory for an interrupt remapping table\n");
        return false;
    }
    if (!amd.coherent) iommu_cache_flush(phys_to_virt(dom->irt), PAGE_SIZE);
    uint64_t f = irq_save();
    send_domain_flush(dom->id); /* nothing the firmware cached under this id is used */
    bool ok = sync();
    if (ok) {
        dom->written = true;
        for (unsigned i = 0; i < dom->nids; i++)
            dte_write(dom->ids[i], DTE_V | DTE_TV | DTE_MODE(dom->levels) | dom->root | DTE_IR | DTE_IW, dom->id,
                      DTE_IV | DTE_IRT_LEN(IRT_LOG2) | dom->irt | DTE_INT_REMAP);
        send_entry_flush(dom);
        ok = sync();
    }
    irq_restore(f);
    return ok;
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
static uint64_t amd_leaf(uint64_t phys, unsigned level, unsigned access) {
    (void)level; /* 4 KiB pages only: NextLevel 0 */
    return phys | (access & IOMMU_READ ? PTE_IR : 0) | (access & IOMMU_WRITE ? PTE_IW : 0) | PTE_PR;
}

static uint16_t amd_alias(uint16_t id) {
    for (size_t i = 0; i < naliases; i++)
        if (id >= aliases[i].first && id <= aliases[i].last) return aliases[i].as;
    return id;
}

/* ---- interrupts ----
   A claimed function's message carries its remapping table index in data bits 10:0: entry n
   is message n, fixed delivery to a physical APIC id (the 32-bit entry format). The table is
   cached: every change is invalidated (2.3.2). */
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

static bool amd_msi_clear(struct pci_device *d) {
    struct iommu_domain *dom = iommu_domain_of(d);
    return !dom || irt_write(dom, 0, 1u << IRT_LOG2, 0, 0, false);
}

static bool amd_ioapic(uint8_t ioapic_id, uint32_t gsi, uint64_t *rte) {
    (void)ioapic_id, (void)gsi, (void)rte;
    return true;
}

/* ---- the units ---- */
/* Off, also when the firmware left it on, then on with this code's tables. Two features let
   requests past the device table, and both stay off: MARC's apertures (2.9), whose global enable
   MarcEn is a control bit never set here, and the performance optimization (3.4.9), which lets
   integrated devices such as GPUs reach memory without the unit, in a register of its own. */
static bool program(struct unit *u) {
    wr(u, R_CONTROL, 0);
    if (!status_wait(u, ST_EVENTLOG_RUN | ST_CMDBUF_RUN, 0)) {
        kprintf("iommu: unit at %lx does not stop its command buffer and event log\n", u->base);
        return false;
    }
    if (u->efr & EFR_PERF_OPT) {
        volatile uint32_t *perf = (volatile uint32_t *)(u->regs + R_PERF_OPT);
        *perf &= ~PERF_OPT_EN;
    }
    wr(u, R_DEVTAB, devtab | (DT_BYTES / PAGE_SIZE - 1));
    wr(u, R_CMDBUF, u->cmd | RING_LEN);
    wr(u, R_EVENTLOG, u->events | RING_LEN);
    wr(u, R_CMD_HEAD, 0);
    wr(u, R_CMD_TAIL, 0);
    wr(u, R_EVT_HEAD, 0);
    wr(u, R_EVT_TAIL, 0);
    u->cmd_tail = 0;
    u->lost = false;
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
    return commands_started(u);
}

/* What a unit may have cached before it was ours, or before a sleep: everything at once where
   it can be told so, else the entries of every ID its IVHD names (a domain's pages are
   invalidated before the domain is first used). */
static void send_everything_flush(void) {
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (u->efr & EFR_IA) {
            command(u, CMD_INV_ALL, 0, 0, 0, 0);
            continue;
        }
        for (size_t s = 0; s < u->nlisted; s++)
            for (uint32_t id = u->listed[s].first; id <= u->listed[s].last; id++) {
                command(u, CMD_INV_DEVTAB, id, 0, 0, 0);
                command(u, CMD_INV_IRT, id, 0, 0, 0);
            }
    }
}

/* Every unit off; false when one did not stop reading its buffers. */
static bool units_off(void) {
    bool ok = true;
    for (size_t i = 0; i < nunits; i++) {
        wr(&units[i], R_CONTROL, 0);
        if (!status_wait(&units[i], ST_EVENTLOG_RUN | ST_CMDBUF_RUN, 0)) {
            kprintf("iommu: unit at %lx does not stop\n", units[i].base);
            ok = false;
        }
    }
    return ok;
}

static void amd_shutdown(void) { units_off(); }

static void amd_resume(void) {
    bool ok = true;
    for (size_t i = 0; i < nunits; i++) ok &= program(&units[i]);
    if (ok) {
        send_everything_flush();
        ok = sync();
    }
    if (!ok) kprintf("iommu: AMD-Vi did not come back after sleep\n");
}

static void tables_free(void) {
    for (uint64_t at = 0; devtab && at < DT_BYTES; at += PAGE_SIZE) pmm_free(devtab + at);
    for (size_t p = 0; sems && p < sem_pages; p++) pmm_free(sems + p * PAGE_SIZE);
    for (size_t i = 0; i < nunits; i++) {
        if (units[i].cmd) pmm_free(units[i].cmd);
        if (units[i].events) pmm_free(units[i].events);
        units[i].cmd = units[i].events = 0;
    }
    devtab = sems = 0;
}

/* Only with translation: interrupts pass a unit that is off as they are (an AMD-Vi unit has
   nothing like VT-d's ECAP.IRREQ), so there is no reason to start one without. */
static bool amd_start(bool translate) {
    if (!translate) return false;
    sem_pages = ALIGN_UP(nunits * 8, PAGE_SIZE) / PAGE_SIZE;
    devtab = pmm_alloc_contig(DT_BYTES / PAGE_SIZE);
    sems = pmm_alloc_contig(sem_pages);
    bool ok = devtab && sems;
    for (size_t i = 0; i < nunits; i++) {
        units[i].cmd = pmm_alloc_zeroed();
        units[i].events = pmm_alloc_zeroed();
        ok &= units[i].cmd && units[i].events;
    }
    if (!ok) {
        kprintf("iommu: no memory for the AMD-Vi tables\n");
        tables_free();
        return false;
    }
    memset(phys_to_virt(devtab), 0, DT_BYTES);
    memset(phys_to_virt(sems), 0, sem_pages * PAGE_SIZE);
    if (!amd.coherent) iommu_cache_flush(phys_to_virt(devtab), DT_BYTES);
    event_vector = irq_vector_alloc(1, 1);
    if (event_vector >= 0) vector_register(event_vector, event_isr);
    uint64_t f = irq_save();
    for (size_t i = 0; ok && i < nunits; i++) ok = program(&units[i]);
    if (ok) {
        send_everything_flush();
        ok = sync();
    }
    bool off = ok || units_off();
    irq_restore(f);
    if (!ok) {
        if (event_vector >= 0) irq_vector_free((unsigned)event_vector, 1);
        event_vector = -1;
        if (off) tables_free(); /* else a unit may still read them: they stay */
        return false;
    }
    for (size_t i = 0; i < nunits; i++) {
        struct pci_device *fn = units[i].fn;
        if (!fn) continue;
        pci_enable_busmaster(fn); /* the kernel's: no app may claim the unit */
        if (event_vector < 0 || !pci_msi_enable(fn, (uint8_t)event_vector))
            kprintf("iommu: unit at %lx has no event interrupt: blocked requests show after commands only\n",
                    units[i].base);
    }
    amd.translating = true;
    kprintf("iommu: AMD-Vi on, %lu unit%s; %lu alias%s, %u reserved range%s: every function passes through "
            "until an app claims it\n", (unsigned long)nunits, nunits == 1 ? "" : "s", (unsigned long)naliases,
            naliases == 1 ? "" : "es", iommu_reserved_count(), iommu_reserved_count() == 1 ? "" : "s");
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
    if (parse_lost) {
        kprintf("iommu: no memory to keep the IVRS: AMD-Vi not used\n");
        return NULL;
    }
    amd.coherent = true;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (!(u->regs = vmm_map_mmio(u->base, MMIO_BYTES))) {
            kprintf("iommu: the registers of the AMD-Vi unit at %lx cannot be mapped: AMD-Vi not used\n", u->base);
            return NULL;
        }
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
