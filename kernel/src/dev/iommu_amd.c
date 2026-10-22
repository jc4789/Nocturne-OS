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
   their SMI filter can, which is the firmware's and stays as it set it): a claimed device that
   sends an SMI message the filter lets through reaches the firmware's handler as on a machine
   without an IOMMU. */
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
#define R_EXCL_BASE 0x0020
#define R_EFR       0x0030
#define R_PERF_OPT  0x016C
#define R_VSTATUS   0x0190
#define R_CMD_HEAD  0x2000
#define R_CMD_TAIL  0x2008
#define R_EVT_HEAD  0x2010
#define R_EVT_TAIL  0x2018
#define R_STATUS    0x2020
#define MMIO_BYTES  0x4000

#define CTL_IOMMU_EN    (1ull << 0)
#define CTL_EVENTLOG_EN (1ull << 2)
#define CTL_EVENTINT_EN (1ull << 3)
#define CTL_COMWAITINT_EN (1ull << 4)
#define CTL_CMDBUF_EN   (1ull << 12)
#define CTL_PPRLOG_EN   (1ull << 13)
#define CTL_PPRINT_EN   (1ull << 14)
#define CTL_PPR_EN      (1ull << 15)
#define CTL_GA_EN       (1ull << 17)
#define CTL_SMIF_EN     (1ull << 22)
#define CTL_SMIF_LOG_EN (1ull << 24)
#define CTL_GALOG_EN    (1ull << 28)
#define CTL_GAINT_EN    (1ull << 29)
#define CTL_MARC_EN     (1ull << 40)
#define CTL_BLK_STOP_MRK_EN (1ull << 41)
#define CTL_XT_EN       (1ull << 50)
#define CTL_VCMD_EN     (1ull << 52)
#define CTL_VIOMMU_EN   (1ull << 53)
/* The firmware's, not engines: its SMI filter, which 2.3.1 sets up only where the firmware did
   not, and StopMark blocking, on from reset (2.6.4.5). Every write of Control keeps them. */
#define CTL_FIRMWARE    (CTL_SMIF_EN | CTL_SMIF_LOG_EN | CTL_BLK_STOP_MRK_EN)
#define EXCL_ENABLE    (1ull << 0)
#define EXCL_ALLOW_ALL (1ull << 1)
#define ST_EVENT_OVERFLOW (1u << 0)
#define ST_EVENT_INT      (1u << 1)
#define ST_COMWAIT_INT    (1u << 2)
#define ST_EVENTLOG_RUN   (1u << 3)
#define ST_CMDBUF_RUN     (1u << 4)
#define ST_PPRLOG_RUN     (1u << 7)
#define ST_GALOG_RUN      (1u << 8)
#define VST_IOMMU_RUN     (1u << 0)
#define VST_CMDBUF_RUN    (1u << 1)
#define EFR_PPR      (1ull << 1)
#define EFR_XT       (1ull << 2)
#define EFR_GA       (1ull << 7)
#define EFR_VIOMMU   (1ull << 55)
#define EFR_IA       (1ull << 6)  /* INVALIDATE_IOMMU_ALL */
#define EFR_MARC_MASK (3ull << 42) /* MarcSup: 00 is unsupported; 01/10 specify aperture counts */
#define EFR_PERF_OPT (1ull << 45) /* the performance optimization register (3.4.9) */
#define PERF_OPT_EN  (1u << 13)
#define CAP_NP_CACHE (1u << 26) /* caches not-present entries: a map needs an invalidation too */
#define CAP_EFR      (1u << 27)

#define RING_BYTES 4096           /* command buffer and event log: 256 entries of 16 bytes */
#define RING_LEN   (8ull << 56)   /* log2 of their entry count */
#define DT_BYTES   (65536 * 32)
#define IRT_LOG2   5              /* a claim's 32 vectors */
/* Every blocked entry's DomainID: iommu.c keeps ID 1 for the back end. Entries that share an ID
   must agree on Mode and root (Table 7), and a released claim's ID is soon another's. */
#define BLOCK_DOMAIN 1

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
#define PTE_NEXT_LEVEL (7ull << 9)
/* 2.2.3: 4-KiB tables, 8-byte entries and a 52-bit physical address field;
   DTE.Mode (Table 7) and EFR.HATS allow at most six host levels. */
#define HOST_PAGE_SHIFT 12u
#define HOST_INDEX_BITS 9u
#define HOST_PHYS_BITS  52u
#define HOST_MAX_LEVELS 6u

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
    uint64_t state_efr; /* engine/bypass停止確認専用: 実MMIOとfirmware imageの和 */
    uint8_t msi_num;       /* event-log MSI-X entry, IVHD IOMMU info[4:0] */
    bool hat_dis, event_irq;
    uint64_t cmd, events;  /* rings */
    uint32_t cmd_tail;
    bool lost;             /* a command since the last sync was not done: that sync fails */
    bool stalled;          /* it took no command in time: none more are sent until that sync */
    bool overflow_told;    /* its event log overflowed and is still writing: said once */
    struct span *listed;   /* the IDs its IVHD names: those it may have cached entries of */
    size_t nlisted, listed_room;
};
static struct unit *units;
static size_t nunits, units_room;
static bool np_cache; /* a unit caches not-present entries */
/* The CPUs in x2APIC mode take 32-bit destinations, which only the 128-bit remapping entry holds:
   XTEn and GAEn select it on every unit, all units using one entry size (2.2.5.2-3). */
static bool wide;
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
static bool parse_lost; /* incomplete allocation or malformed IVRS: the units are not used */
static uint32_t ivinfo;

static struct iommu_hw amd;

#ifndef IOMMU_AMD_HOSTTEST
static uint64_t rd(const struct unit *u, unsigned off) { return mmio_read64(u->regs + off); }
static void wr(const struct unit *u, unsigned off, uint64_t v) { mmio_write64(u->regs + off, v); }
static uint32_t rd32(const struct unit *u, unsigned off) { return mmio_read32(u->regs + off); }
static void wr32(const struct unit *u, unsigned off, uint32_t v) { mmio_write32(u->regs + off, v); }
#endif

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static bool engines_off(const struct unit *u, bool wait);

/* ---- IVRS ---- */
static void alias_add(uint16_t first, uint16_t last, uint16_t as) {
    if (first > last) { parse_lost = true; return; }
    struct alias *a = kappend(&aliases, &naliases, &aliases_room, sizeof *a);
    if (a) *a = (struct alias){first, last, as};
    else parse_lost = true;
}

/* IDs the unit sees requests with: covered (a claim may isolate them), and named, so that what
   it cached of them is invalidated where it cannot invalidate everything at once */
static void listed_add(struct unit *u, uint16_t first, uint16_t last, bool cover) {
    if (first > last) { parse_lost = true; return; }
    if (cover) iommu_cover(first, last);
    struct span *s = kappend(&u->listed, &u->nlisted, &u->listed_room, sizeof *s);
    if (s) *s = (struct span){first, last};
    else parse_lost = true;
}

/* Device entries (Tables 101-108): 4 bytes below type 40h, 8 below 80h, ACPI HID entries (F0h) 22
   plus their UID. Select (02h, 42h, 46h), all (01h), and ranges from a start (03h, 43h, 47h) to
   an end (04h); 42h and 43h add the ID the requests arrive as. Special devices (48h) are the
   IOAPICs and HPETs, the platform's. Their DTE settings (byte 3: the LINT, NMI, INIT and ExtInt
   passes) are for the entries the firmware writes; a claim's entries pass none. */
static void parse_devices(struct unit *u, const uint8_t *b, uint32_t at, uint32_t end) {
    uint16_t first = 0, as = 0;
    bool range = false, alias_range = false;
    while (end - at >= 4) {
        uint8_t type = b[at];
        uint32_t len = type < 0x40 ? 4 : type < 0x80 ? 8 : type == 0xF0 && end - at >= 22 ? 22u + b[at + 21] : 0;
        if (!len || len > end - at) { parse_lost = true; return; }
        uint16_t id = u16(b + at + 1);
        switch (type) {
        case 0x01: listed_add(u, 0, 0xFFFF, true); break;
        case 0x02: case 0x46: listed_add(u, id, id, true); break;
        case 0x03: case 0x47:
            if (range) parse_lost = true;
            first = id, range = true, alias_range = false;
            break;
        case 0x42:
            listed_add(u, id, id, true);
            listed_add(u, u16(b + at + 5), u16(b + at + 5), true);
            alias_add(id, id, u16(b + at + 5));
            break;
        case 0x43:
            if (range) parse_lost = true;
            first = id, as = u16(b + at + 5), range = alias_range = true;
            break;
        case 0x04:
            if (!range) parse_lost = true;
            if (range) listed_add(u, first, id, true);
            if (range && alias_range) {
                listed_add(u, as, as, true);
                alias_add(first, id, as);
            }
            range = alias_range = false;
            break;
        case 0x48:
            iommu_kernel_id(u16(b + at + 5));
            listed_add(u, u16(b + at + 5), u16(b + at + 5), false);
            break;
        case 0xF0:
            iommu_kernel_id(id); /* ACPI-named platform device: not a PCI claim's requester */
            listed_add(u, id, id, false);
            break;
        }
        at += len;
    }
    if (at != end || range) parse_lost = true;
}

/* IVMD: memory a function reaches by DMA from the firmware's start: unity mapped with the
   access its IR and IW flags give (none: not mapped), or excluded from translation (then
   reached as it is, the flags ignored). A claimed function's domain maps it as it is. */
static void parse_memory(const uint8_t *b) {
    if (b[0] != 0x20 && u16(b + 8)) return; /* PCI segment, Table 110 */
    unsigned flags = b[1], access = flags & 8 ? IOMMU_READ | IOMMU_WRITE
                                  : (flags & 2 ? IOMMU_READ : 0) | (flags & 4 ? IOMMU_WRITE : 0);
    uint64_t start, length;
    memcpy(&start, b + 16, 8);
    memcpy(&length, b + 24, 8);
    uint16_t first = b[0] == 0x20 ? 0 : u16(b + 4), last = b[0] == 0x20 ? 0xFFFF : b[0] == 0x22 ? u16(b + 6) : first;
    if (first > last || length > UINT64_MAX - start || start + length > UINT64_MAX - (PAGE_SIZE - 1)) {
        parse_lost = true;
        return;
    }
    if (length) iommu_reserved(first, last, ALIGN_DOWN(start, PAGE_SIZE), ALIGN_UP(start + length, PAGE_SIZE), access);
}

static struct unit *unit_at(uint64_t base) {
    for (size_t i = 0; i < nunits; i++)
        if (units[i].base == base) return &units[i];
    return NULL;
}

/* Types 10h, 11h and 40h describe the same unit in more detail each: the most detailed one
   this code reads is used. */
static bool parse_ivrs(const struct acpi_sdt *h) {
    if (h->length < 48 || (h->revision != 1 && h->revision != 2)) return false;
    const uint8_t *b = (const uint8_t *)h;
    memcpy(&ivinfo, b + 36, sizeof ivinfo);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t off = 48;
        for (; h->length - off >= 4;) {
            uint8_t type = b[off];
            uint16_t len = u16(b + off + 2);
            if (len < 4 || len > h->length - off) { parse_lost = true; break; }
            if (type == 0x10 || ((ivinfo & 1) && (type == 0x11 || (h->revision >= 2 && type == 0x40)))) {
                if (len < (type == 0x10 ? 24 : 40)) { parse_lost = true; break; }
                if (u16(b + off + 16)) { off += len; continue; } /* this PCI layer addresses segment zero */
                uint64_t base;
                memcpy(&base, b + off + 8, 8);
                struct unit *u = unit_at(base);
                if (pass == 0 && (!base || (base & (MMIO_BYTES - 1)) || base > UINT64_MAX - MMIO_BYTES)) {
                    kprintf("iommu: the IVRS describes an AMD-Vi unit with an invalid register address; not used\n");
                    parse_lost = true;
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
                    /* The capability and its +10h register must fit conventional PCI
                       configuration space (3.2); no wrap into another function/register. */
                    if (u->cap < 0x40 || u->cap > 0xEC || (u->cap & 3)) parse_lost = true;
                    u->msi_num = u16(b + off + 18) & 31;
                    if (type != 0x10) {
                        u->hat_dis = (b[off + 20] & 1) != 0;
                        memcpy(&u->efr, b + off + 24, sizeof u->efr);
                    }
                    u->fn = pci_find_function(id >> 8, (id >> 3) & 31, id & 7);
                    iommu_kernel_id(id);
                    parse_devices(u, b, off + (type == 0x10 ? 24 : 40), off + len);
                }
            } else if (pass == 1 && type >= 0x20 && type <= 0x22) {
                if (len != 32) parse_lost = true;
                else parse_memory(b + off);
            }
            off += len;
        }
        if (off != h->length) parse_lost = true;
    }
    return nunits != 0;
}

/* Isolation metadata may be unusable while the hardware is already safely off. Audit only
   unit identity and live engines, independently of device entries, IVMD and usable widths.
   No registers are written and no firmware queue is taken over on this path. */
static bool ivrs_plain_safe(const struct acpi_sdt *h) {
    if (h->length < 48 || (h->revision != 1 && h->revision != 2)) return false;
    const uint8_t *b = (const uint8_t *)h;
    uint32_t summary;
    memcpy(&summary, b + 36, sizeof summary);
    bool identified = false;
    uint32_t at = 48;
    while (h->length - at >= 4) {
        uint8_t type = b[at];
        uint16_t len = u16(b + at + 2);
        if (len < 4 || len > h->length - at) return false;
        if (type == 0x10 || type == 0x11 || type == 0x40) {
            if (len < (type == 0x10 ? 24 : 40)) return false;
            uint64_t base;
            memcpy(&base, b + at + 8, sizeof base);
            if (!base || (base & (MMIO_BYTES - 1)) || base > UINT64_MAX - MMIO_BYTES) return false;
            struct unit *known = unit_at(base);
            struct unit u = {.base = base, .regs = known ? known->regs : NULL};
            if (!u.regs && !(u.regs = vmm_map_mmio(base, MMIO_BYTES))) return false;
            /* Include nonzero PCI segments: their physical register addresses are still
               known even though this PCI layer cannot configure their functions. */
            uint16_t id = u16(b + at + 4), cap = u16(b + at + 6);
            struct pci_device *fn = !u16(b + at + 16) && cap >= 0x40 && cap <= 0xEC && !(cap & 3)
                ? pci_find_function(id >> 8, (id >> 3) & 31, id & 7) : NULL;
            uint32_t header = fn ? pci_read32(fn->bus, fn->dev, fn->func, cap) : 0;
            uint64_t efr = ((summary & 1) || (header & CAP_EFR)) ? rd(&u, R_EFR) : 0;
            if (type != 0x10) {
                uint64_t image;
                memcpy(&image, b + at + 24, sizeof image);
                /* The image overrides supported features for normal use. For an off
                   audit, neither it nor actual hardware may hide an existing engine. */
                efr |= image;
            }
            u.state_efr = efr;
            if (!engines_off(&u, false)) return false;
            identified = true;
        } else if (type < 0x20 || type > 0x22) {
            return false; /* unknown IVDBs might describe an unenumerated hardware unit */
        }
        at += len;
    }
    return at == h->length && identified;
}

/* ---- commands and events ---- */
/* Waits until the status bits `bits` read `want`: false when they did not in time. */
static bool register_wait(const struct unit *u, unsigned off, uint32_t bits, uint32_t want) {
    for (uint64_t t = iommu_deadline(10); (rd(u, off) & bits) != want; pause())
        if (iommu_expired(t)) return false;
    return true;
}

static bool status_wait(const struct unit *u, uint32_t bits, uint32_t want) {
    return register_wait(u, R_STATUS, bits, want);
}

/* Control/Status (3.4, pp212-216, 249-250), vIOMMU Status (3.4.11).
   Test only architected bits of supported features, not reserved/RO policy bits. Probe
   observes once without touching firmware; shutdown/program wait after disabling engines. */
static bool engines_off(const struct unit *u, bool wait) {
    uint64_t efr = u->efr | u->state_efr;
    uint64_t enabled = CTL_IOMMU_EN | CTL_CMDBUF_EN | CTL_EVENTLOG_EN |
                       CTL_EVENTINT_EN | CTL_COMWAITINT_EN;
    uint32_t running = ST_CMDBUF_RUN | ST_EVENTLOG_RUN;
    if (efr & EFR_PPR) {
        enabled |= CTL_PPR_EN | CTL_PPRLOG_EN | CTL_PPRINT_EN;
        running |= ST_PPRLOG_RUN;
    }
    if (efr & EFR_GA) {
        enabled |= CTL_GA_EN | CTL_GALOG_EN | CTL_GAINT_EN;
        running |= ST_GALOG_RUN;
    }
    if (efr & EFR_MARC_MASK) enabled |= CTL_MARC_EN;
    if (efr & EFR_VIOMMU) enabled |= CTL_VIOMMU_EN | CTL_VCMD_EN;
    if (rd(u, R_CONTROL) & enabled) return false;
    if (wait ? !status_wait(u, running, 0) : (rd(u, R_STATUS) & running) != 0) return false;
    if (!(efr & EFR_VIOMMU)) return true;
    uint32_t virtual_running = VST_IOMMU_RUN | VST_CMDBUF_RUN;
    return wait ? register_wait(u, R_VSTATUS, virtual_running, 0) : !(rd(u, R_VSTATUS) & virtual_running);
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

/* A command for one unit: one it has no room for in time is lost (u->lost), and so are the ones
   after it until the sync, which would each wait as long with interrupts off. */
static void command(struct unit *u, unsigned op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (u->stalled) return;
    for (uint64_t t = iommu_deadline(100); (rd(u, R_CMD_HEAD) & 0x7FFF0) == (u->cmd_tail + 16) % RING_BYTES; pause()) {
        if (!(rd(u, R_STATUS) & ST_CMDBUF_RUN)) {
            commands_restart(u);
            break;
        }
        if (iommu_expired(t)) {
            u->lost = u->stalled = true;
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
static void domain_flush(struct unit *u, uint16_t id) { command(u, CMD_INV_PAGES, 0, id, 0xFFFFF000u | 3, 0x7FFFFFFFu); }
static void send_domain_flush(uint16_t id) {
    for (size_t i = 0; i < nunits; i++) domain_flush(&units[i], id);
}

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
    /* Zero is the pending word, never completion, including at the store field's wrap. */
    if (!++token) ++token;
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
        units[i].lost = units[i].stalled = false;
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
    if (!(dom->irt = iommu_alloc_pages(1))) {
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
   aborts. Interrupts valid with IntCtl 00 and no pass bits: every interrupt aborts. What the
   units cached under the domain's own ID goes (2.4.2) before that ID is released. */
static bool amd_block(struct iommu_domain *dom) {
    uint64_t f = irq_save();
    for (unsigned i = 0; i < dom->nids; i++) dte_write(dom->ids[i], DTE_V | DTE_TV, BLOCK_DOMAIN, DTE_IV);
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
static bool amd_is_leaf(uint64_t entry, unsigned level) {
    (void)level;
    return !(entry & PTE_NEXT_LEVEL) || (entry & PTE_NEXT_LEVEL) == PTE_NEXT_LEVEL;
}

static uint16_t amd_alias(uint16_t id) {
    for (size_t i = 0; i < naliases; i++)
        if (id >= aliases[i].first && id <= aliases[i].last) return aliases[i].as;
    return id;
}

/* ---- interrupts ----
   A claimed function's message carries its remapping table index in data bits 10:0 and no
   destination: entry n is message n, fixed delivery to a physical APIC id. The entries are the
   128-bit format with GuestMode 0 (Figure 19), whose destination has 32 bits, where the units
   are wide, else the 32-bit format (Figure 15) with 8. The table is cached: every change is
   invalidated (2.3.2). */

/* A 128-bit entry changes in one store the unit cannot see half done, as one that remaps must
   (2.3.2): cmpxchg16b on the 16-byte aligned entry. */
static void irte_store(volatile uint64_t *e, uint64_t lo, uint64_t hi) {
    uint64_t old_lo = e[0], old_hi = e[1];
    bool stored;
    do __asm__ volatile("lock cmpxchg16b %1"
                        : "=@ccz"(stored), "+m"(*(volatile unsigned __int128 *)e), "+a"(old_lo), "+d"(old_hi)
                        : "b"(lo), "c"(hi)
                        : "memory");
    while (!stored);
}

static bool irt_write(struct iommu_domain *dom, unsigned first, unsigned count, uint8_t vector, uint32_t dest,
                      bool on) {
    uint8_t *irt = phys_to_virt(dom->irt);
    size_t size = wide ? 16 : 4;
    for (unsigned i = 0; i < count; i++) {
        uint8_t v = (uint8_t)(vector + i);
        if (wide)
            irte_store((volatile uint64_t *)(irt + (first + i) * size), on ? 1u | (uint64_t)(dest & 0xFFFFFF) << 8 : 0,
                       on ? v | (uint64_t)(dest >> 24) << 56 : 0);
        else
            ((volatile uint32_t *)irt)[first + i] = on ? 1u | dest << 8 | (uint32_t)v << 16 : 0;
    }
    if (!amd.coherent) iommu_cache_flush(irt + first * size, count * size);
    uint64_t f = irq_save();
    for (unsigned i = 0; i < dom->nids; i++) send(CMD_INV_IRT, dom->ids[i], 0, 0, 0);
    bool ok = sync();
    irq_restore(f);
    return ok;
}

static bool amd_msi(struct pci_device *d, unsigned index, unsigned count, uint8_t vector, uint32_t dest,
                    uint32_t *address, uint32_t *data) {
    struct iommu_domain *dom = iommu_domain_of(d);
    if (dest > 0xFF && !(dom && wide)) return false;
    *address = iommu_msi_plain(dom ? 0 : dest);
    *data = dom ? index : vector;
    if (!dom) return true;
    return index < 1u << IRT_LOG2 && count && count <= (1u << IRT_LOG2) - index &&
           irt_write(dom, index, count, vector, dest, true);
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
/* Off, also when the firmware left it on, then on with this code's tables. Bypasses stay off:
   the exclusion range (3.4, offset 0020h), MARC's apertures (2.9), whose global enable
   MarcEn is a control bit never set here, and the performance optimization (3.4.9), which lets
   integrated devices such as GPUs reach memory without the unit, in a register of its own. */
static bool program(struct unit *u) {
    uint64_t kept = rd(u, R_CONTROL) & CTL_FIRMWARE;
    wr(u, R_CONTROL, kept);
    if (!engines_off(u, true)) {
        kprintf("iommu: unit at %lx does not stop its translation and queue engines\n", u->base);
        return false;
    }
    /* Control=0 does not reset this register. Firmware's Allow=1 would bypass every
       claimed DTE in its range, including blocked domains (3.4, offset 0020h). */
    wr(u, R_EXCL_BASE, 0); /* reserved bits must also be written zero (Chapter 3) */
    if (rd(u, R_EXCL_BASE) & (EXCL_ENABLE | EXCL_ALLOW_ALL)) {
        kprintf("iommu: unit at %lx retains its firmware exclusion bypass\n", u->base);
        return false;
    }
    /* 通常のfeature選択で実在bypassの解除を省かない。新tableの公開前に確認する。 */
    if ((u->efr | u->state_efr) & EFR_PERF_OPT) {
        wr32(u, R_PERF_OPT, 0); /* the other bits are reserved (3.4.9) */
        if (rd32(u, R_PERF_OPT) & PERF_OPT_EN) {
            kprintf("iommu: unit at %lx retains its performance-optimization bypass\n", u->base);
            return false;
        }
    }
    wr(u, R_DEVTAB, devtab | (DT_BYTES / PAGE_SIZE - 1));
    wr(u, R_CMDBUF, u->cmd | RING_LEN);
    wr(u, R_EVENTLOG, u->events | RING_LEN);
    wr(u, R_CMD_HEAD, 0);
    wr(u, R_CMD_TAIL, 0);
    wr(u, R_EVT_HEAD, 0);
    wr(u, R_EVT_TAIL, 0);
    u->cmd_tail = 0;
    u->lost = u->stalled = false;
    wr(u, R_STATUS, ST_EVENT_OVERFLOW | ST_EVENT_INT | ST_COMWAIT_INT);
    /* the IVHD flags HtTunEn, PassPW, ResPassPW, Isoc and Coherent are the firmware's settings
       for control bits 1, 8, 9, 11 and 10 */
    uint64_t ctl = kept | CTL_IOMMU_EN | CTL_EVENTLOG_EN | CTL_CMDBUF_EN;
    if (wide) ctl |= CTL_XT_EN | CTL_GA_EN; /* GAMEn 000b: remapping only, no guest APIC */
    if (u->flags & 1) ctl |= 1ull << 1;
    if (u->flags & 2) ctl |= 1ull << 8;
    if (u->flags & 4) ctl |= 1ull << 9;
    if (u->flags & 8) ctl |= 1ull << 11;
    if (u->flags & 32) ctl |= 1ull << 10;
    if (u->event_irq) ctl |= CTL_EVENTINT_EN;
    wr(u, R_CONTROL, ctl);
    return commands_started(u);
}

/* What a unit may have cached before it was ours, or before a sleep: everything at once where
   it can be told so, else the entries of every ID its IVHD names and the pages of the blocked
   entries' domain (a claim's are invalidated before the claim first uses them). */
static void send_everything_flush(void) {
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (u->efr & EFR_IA) {
            command(u, CMD_INV_ALL, 0, 0, 0, 0);
            continue;
        }
        domain_flush(u, BLOCK_DOMAIN);
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
        wr(&units[i], R_CONTROL, rd(&units[i], R_CONTROL) & CTL_FIRMWARE);
        if (!engines_off(&units[i], true)) {
            kprintf("iommu: unit at %lx does not stop\n", units[i].base);
            ok = false;
        }
    }
    return ok;
}

static bool amd_shutdown(void) { return units_off(); }

static bool amd_resume(void) {
    bool ok = true;
    for (size_t i = 0; i < nunits; i++) ok &= program(&units[i]);
    if (ok) {
        send_everything_flush();
        ok = sync();
    }
    if (!ok) kprintf("iommu: AMD-Vi did not come back after sleep\n");
    return ok;
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

/* One PCI function may represent several units and exposes MSI or MSI-X, not necessarily MSI
   (3.1, Capability+10h). Initialize its MSI-X table once so later units do not mask earlier
   entries. Every unit keeps its IVHD event message number (Table 91). */
static void event_interrupts_enable(void) {
    for (size_t i = 0; i < nunits; i++) {
        struct pci_device *fn = units[i].fn;
        if (!fn) continue;
        bool seen = false;
        for (size_t j = 0; j < i; j++) seen |= units[j].fn == fn;
        if (seen) continue;
        pci_enable_busmaster(fn);
        if (event_vector < 0) {
            kprintf("iommu: function %02x:%02x.%u has no event vector\n", fn->bus, fn->dev, fn->func);
            continue;
        }
        unsigned entries = pci_msix_init(fn);
        bool msi = !entries && pci_msi_enable(fn, (uint8_t)event_vector);
        for (size_t j = i; j < nunits; j++) {
            struct unit *u = &units[j];
            if (u->fn != fn) continue;
            u->event_irq = entries ? u->msi_num < entries &&
                pci_msix_route(fn, u->msi_num, (uint8_t)event_vector, lapic_current_id()) : msi && !u->msi_num;
            if (u->event_irq && entries) pci_msix_mask(fn, u->msi_num, false);
            if (u->event_irq) {
                uint64_t ctl = rd(u, R_CONTROL);
                if (!(rd(u, R_STATUS) & ST_CMDBUF_RUN)) ctl &= ~CTL_CMDBUF_EN;
                wr(u, R_CONTROL, ctl | CTL_EVENTINT_EN);
            } else {
                kprintf("iommu: unit at %lx has no event interrupt: blocked requests show after commands only\n",
                        u->base);
            }
        }
        if (entries) pci_msix_function_mask(fn, false);
    }
}

/* Only with translation: interrupts pass a unit that is off as they are (an AMD-Vi unit has
   nothing like VT-d's ECAP.IRREQ), so there is no reason to start one without. */
static bool amd_start(bool translate) {
    if (!translate) return false;
    sem_pages = ALIGN_UP(nunits * 8, PAGE_SIZE) / PAGE_SIZE;
    devtab = iommu_alloc_pages(DT_BYTES / PAGE_SIZE);
    sems = iommu_alloc_pages(sem_pages);
    bool ok = devtab && sems;
    for (size_t i = 0; i < nunits; i++) {
        units[i].cmd = iommu_alloc_pages(1);
        units[i].events = iommu_alloc_pages(1);
        ok &= units[i].cmd && units[i].events;
    }
    if (!ok) {
        kprintf("iommu: no memory for the AMD-Vi tables\n");
        tables_free();
        return false;
    }
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
    event_interrupts_enable();
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
    .is_leaf = amd_is_leaf,
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

/* Capability+10h, 3.2 (pp203-204); Table86 uses the same encodings in IVinfo. */
static bool va_width_valid(unsigned bits) { return bits == 32 || bits == 40 || bits == 48 || bits == 64; }
static bool pa_width_valid(unsigned bits) { return bits == 40 || bits == 48 || bits == 52; }

/* A unit the firmware left on reads its device table and command buffer and writes its event
   log (3.4, offsets 0000h-0010h) until program() stops it: whether one of them lies in RAM the
   OS owns, which the kernel may have reused before then. */
static bool left_in_os_ram(const struct unit *u, uint64_t control) {
    static const struct { unsigned reg; uint64_t enable; } live[] = {
        {R_DEVTAB, CTL_IOMMU_EN}, {R_CMDBUF, CTL_CMDBUF_EN}, {R_EVENTLOG, CTL_EVENTLOG_EN}};
    bool found = false;
    for (size_t i = 0; i < sizeof live / sizeof *live; i++) {
        if (!(control & CTL_IOMMU_EN) || !(control & live[i].enable)) continue;
        uint64_t v = rd(u, live[i].reg), base = v & 0x000FFFFFFFFFF000ull;
        uint64_t bytes = live[i].reg == R_DEVTAB ? ((v & 0x1FF) + 1) * PAGE_SIZE : 16ull << (v >> 56 & 15);
        found |= iommu_os_ram(base, base + bytes);
    }
    return found;
}

const struct iommu_hw *iommu_amd_probe(bool *plain_safe) {
    *plain_safe = false;
    const struct acpi_sdt *h = acpi_find("IVRS", 0);
    if (!h) {
        *plain_safe = true;
        return NULL;
    }
    if (!parse_ivrs(h)) goto rejected;
    if (parse_lost) {
        kprintf("iommu: incomplete or malformed IVRS: AMD-Vi not used\n");
        goto rejected;
    }
    amd.coherent = true;
    unsigned summary_va = (ivinfo >> 15) & 0x7F, summary_pa = (ivinfo >> 8) & 0x7F;
    /* IVinfo summarizes all units. A defined firmware width remains an upper bound;
       a missing/reserved summary field cannot describe one. Start at the largest
       architected width in that case, then require a defined width for EVERY unit.
       Without its PCI function a unit still needs both valid IVinfo widths below. */
    amd.iova_bits = va_width_valid(summary_va) ? summary_va : 64;
    amd.phys_bits = pa_width_valid(summary_pa) ? summary_pa : HOST_PHYS_BITS;
    unsigned max_levels = HOST_MAX_LEVELS;
    for (size_t i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (u->hat_dis) {
            kprintf("iommu: unit at %lx cannot translate host addresses\n", u->base);
            goto rejected;
        }
        if (!(u->regs = vmm_map_mmio(u->base, MMIO_BYTES))) {
            kprintf("iommu: the registers of the AMD-Vi unit at %lx cannot be mapped: AMD-Vi not used\n", u->base);
            goto rejected;
        }
        /* without its function the capability header is unknown: assume the cautious answers */
        uint32_t header = u->fn ? pci_read32(u->fn->bus, u->fn->dev, u->fn->func, u->cap)
                                : CAP_NP_CACHE | (ivinfo & 1 ? CAP_EFR : 0);
        u->state_efr = ((header & CAP_EFR) || (ivinfo & 1)) ? rd(u, R_EFR) : 0;
        if (u->type == 0x10) u->efr = header & CAP_EFR ? u->state_efr : 0;
        u->state_efr |= u->efr;
        uint32_t info = u->fn ? pci_read32(u->fn->bus, u->fn->dev, u->fn->func, u->cap + 0x10) : ivinfo;
        unsigned va = (info >> 15) & 0x7F, pa = (info >> 8) & 0x7F;
        /* Only these widths are defined by Capability+10h (3.2, pp203-204). No guessed
           width is safe: a high bit could be discarded or a table address truncated. */
        if (!va_width_valid(va) || !pa_width_valid(pa) || ((u->efr >> 10) & 3) == 3) {
            kprintf("iommu: unit at %lx reports reserved address widths\n", u->base);
            goto rejected;
        }
        amd.iova_bits = MIN(amd.iova_bits, va);
        amd.phys_bits = MIN(amd.phys_bits, pa);
        max_levels = MIN(max_levels, 4u + ((unsigned)(u->efr >> 10) & 3));
        np_cache |= (header & CAP_NP_CACHE) != 0;
        amd.coherent &= (u->flags & 32) != 0;
        uint64_t control = rd(u, R_CONTROL);
        kprintf("iommu: AMD-Vi unit at %lx (IVHD %02xh, function %02x:%02x.%u), extended features %lx; "
                "left with control %lx, status %lx\n", u->base, u->type, u->fn ? u->fn->bus : 0,
                u->fn ? u->fn->dev : 0, u->fn ? u->fn->func : 0, u->efr, control, rd(u, R_STATUS));
        if (left_in_os_ram(u, control))
            kprintf("iommu: unit at %lx was left on with its tables in RAM the OS owns: what it read and "
                    "wrote there until now is not known\n", u->base);
    }
    if (ivinfo & 2) kprintf("iommu: the firmware protected memory from DMA with AMD-Vi before boot (IVinfo %x)\n", ivinfo);
    /* 2.2.5.3: with the CPUs in x2APIC mode the units are wide where every unit can be. Else a
       claimed function's remapped interrupts reach the APIC IDs the 32-bit entry holds. */
    wide = lapic_x2apic();
    for (size_t i = 0; i < nunits; i++) wide &= (units[i].efr & (EFR_XT | EFR_GA)) == (EFR_XT | EFR_GA);
    if (lapic_x2apic() && !wide)
        kprintf("iommu: an AMD-Vi unit lacks x2APIC interrupt remapping (XTSup, GASup): claimed functions' "
                "interrupts go to APIC IDs below 256 only\n");
    amd.iova_bits = MIN(amd.iova_bits, HOST_PAGE_SHIFT + HOST_INDEX_BITS * max_levels);
    /* HATS is a maximum, not a required depth. Domains currently use identity DMA;
       iommu_dma_limit accepts only min(input, physical, PTE physical width). Retain
       the real input capability, but do not walk levels that those mappings never use. */
    unsigned bits = MIN(amd.iova_bits, MIN(amd.phys_bits, HOST_PHYS_BITS));
    amd.levels = 1;
    while (HOST_PAGE_SHIFT + HOST_INDEX_BITS * amd.levels < bits) amd.levels++;
    return &amd;
rejected:
    *plain_safe = ivrs_plain_safe(h);
    return NULL;
}
