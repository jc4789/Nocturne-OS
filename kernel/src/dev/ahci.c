/* SATA disks behind AHCI host bus adapters (Serial ATA AHCI 1.3.1), the way nearly every PC
   attaches them: one command at a time per port, in its first command slot, with 48-bit (or, for
   an old disk, 28-bit) DMA commands and the data through a bounce buffer. Every port with an ATA
   disk becomes a block device "sataN"; ATAPI drives and port multipliers are skipped. Completions
   arrive by MSI, or are polled when the platform offers none. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "dev/blk.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/sched.h"

#define MAX_HBA 4
#define MAX_PORTS 32
#define XFER_PAGES 32  /* bounce buffer: 128 KiB, one PRD entry */
#define SPINUP_NS (50 * NS_PER_MS) /* links come up after the spin-up bit is set */
#define LINK_NS NS_PER_S            /* after a COMRESET the link is up within this */
#define READY_NS (10 * NS_PER_S)    /* a disk that was asleep or spinning up answers within this */
#define STOP_NS (500 * NS_PER_MS)   /* a port's engines stop within this (10.1.2) */
#define IO_NS (30 * NS_PER_S)

/* HBA registers (3.1) */
#define CAP 0x00
#define GHC 0x04
#define IS 0x08
#define PI 0x0C
#define VS 0x10
#define CAP2 0x24
#define BOHC 0x28
#define CAP_S64A (1u << 31)
#define CAP_SSS (1u << 27)
#define CAP2_BOH 1u
#define GHC_IE 2u
#define GHC_AE (1u << 31)
#define BOHC_BOS 1u
#define BOHC_OOS 2u
#define BOHC_BB 0x10u

/* port registers (3.3), 0x80 bytes each from 0x100 */
#define PxCLB 0x00
#define PxFB 0x08
#define PxIS 0x10
#define PxIE 0x14
#define PxCMD 0x18
#define PxTFD 0x20
#define PxSIG 0x24
#define PxSSTS 0x28
#define PxSCTL 0x2C
#define PxSERR 0x30
#define PxCI 0x38
#define CMD_ST 1u
#define CMD_SUD 2u
#define CMD_POD 4u
#define CMD_FRE 0x10u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define TFD_ERR 1u
#define TFD_DRQ 8u
#define TFD_BSY 0x80u
#define IS_DHRS 1u            /* a Device to Host Register FIS: how a command ends */
#define IS_ERRORS 0x78000000u /* interface fatal, host bus data, host bus fatal, task file */
#define DET_UP 3u             /* PxSSTS.DET: a device, and the link established */
#define SIG_ATA 0x00000101u

/* the port's page: its command list, the FISes it receives and the one command table */
#define LIST_AT 0x000
#define FIS_AT 0x400
#define TABLE_AT 0x800
#define PRDT_AT 0x80 /* in the command table, after the command FIS */

/* ATA commands (ACS-3) */
#define ATA_READ_DMA 0xC8
#define ATA_WRITE_DMA 0xCA
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_CACHE 0xE7
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_STANDBY_IMMEDIATE 0xE0
#define ATA_IDENTIFY 0xEC

struct hba;

struct port {
    struct blkdev blk;
    struct hba *h;
    unsigned no;
    volatile uint8_t *regs;
    uint8_t *mem; /* LIST_AT, FIS_AT, TABLE_AT */
    uint64_t mem_phys;
    uint8_t *data;
    uint64_t data_phys;
    unsigned block;   /* logical sector in bytes */
    bool lba48, cache; /* 48-bit commands; a write cache that is on */
    bool live; /* set up: commands may be given. Cleared by a command that went wrong and by a sleep:
                  the next command sets the port up again first */
    struct mutex lock; /* one command at a time, from any CPU: the slot, the bounce buffer, live */
    /* The interrupt handler (CPU 0) and the command waiting on another CPU: it adds to events and
       counts kicks up, then wakes wq. A waiter reads kicks before it looks at the port and sleeps
       only while kicks still reads that (wq_wait_unless). */
    struct wait_queue wq;
    uint64_t kicks;
    uint32_t events; /* PxIS bits the interrupt handler cleared since the command began */
};

struct hba {
    int index;
    struct pci_device *pci;
    volatile uint8_t *regs;
    bool s64;
    uint8_t vector;
    struct port *ports[MAX_PORTS];
};

static struct hba hbas[MAX_HBA];
static int nhbas, ndisks;

static uint32_t hba_rd(struct hba *h, unsigned off) { return mmio_read32(h->regs + off); }
static void hba_wr(struct hba *h, unsigned off, uint32_t v) { mmio_write32(h->regs + off, v); }
static uint32_t rd(struct port *p, unsigned off) { return mmio_read32(p->regs + off); }
static void wr(struct port *p, unsigned off, uint32_t v) { mmio_write32(p->regs + off, v); }

/* With interrupts off (a power change) nothing may sleep: waits spin on the clock instead. */
static bool may_sleep(void) { return read_rflags() & 0x200; }

/* Until (register & mask) == want, for up to ns: looked at each millisecond, or spinning. */
static bool wait_reg(struct port *p, unsigned off, uint32_t mask, uint32_t want, uint64_t ns) {
    bool sleeps = may_sleep();
    uint64_t deadline = clock_after(ns);
    for (;;) {
        uint32_t v = rd(p, off);
        if (v == 0xFFFFFFFFu) return false; /* the adapter is gone */
        if ((v & mask) == want) return true;
        if (clock_now_ns() >= deadline) return false;
        if (sleeps) sleep_until(clock_after(NS_PER_MS));
        else pause();
    }
}

static void ahci_isr(struct regs *r) {
    for (int i = 0; i < nhbas; i++) {
        struct hba *h = &hbas[i];
        if (h->vector != r->vector) continue;
        uint32_t is = hba_rd(h, IS);
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            if (!(is & 1u << n)) continue;
            struct port *p = h->ports[n];
            volatile uint8_t *regs = h->regs + 0x100 + 0x80 * n;
            uint32_t events = mmio_read32(regs + PxIS);
            mmio_write32(regs + PxIS, events);
            if (!p) continue;
            __atomic_fetch_or(&p->events, events, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&p->kicks, 1, __ATOMIC_SEQ_CST);
            wq_wake_all(&p->wq);
        }
        hba_wr(h, IS, is);
    }
}

/* The port's command list and FIS receive engines stopped (10.1.2), so its memory may change and
   a COMRESET may be sent. */
static bool port_stop(struct port *p) {
    uint32_t cmd = rd(p, PxCMD);
    if (cmd & CMD_ST) wr(p, PxCMD, cmd & ~CMD_ST);
    if (!wait_reg(p, PxCMD, CMD_CR, 0, STOP_NS)) return false;
    cmd = rd(p, PxCMD);
    if (cmd & CMD_FRE) wr(p, PxCMD, cmd & ~CMD_FRE);
    return wait_reg(p, PxCMD, CMD_FR, 0, STOP_NS);
}

/* COMRESET (10.4.2): the link set up again and the device reset; it then sends its signature in a
   register FIS. Its power states are kept to active (IPM 3): the commands are few and far apart. */
static bool comreset(struct port *p) {
    uint32_t sctl = rd(p, PxSCTL) & ~0xF0Fu;
    wr(p, PxSCTL, sctl | 0x300u | 1u);
    clock_spin(2 * NS_PER_MS); /* at least 1 ms, also where nothing may sleep */
    wr(p, PxSCTL, sctl | 0x300u);
    bool up = wait_reg(p, PxSSTS, 0xF, DET_UP, LINK_NS);
    wr(p, PxSERR, 0xFFFFFFFFu); /* coming up sets the link's diagnostics */
    return up;
}

/* The port running with its disk ready for commands (10.1.2, 10.3.1): its memory given, a fresh
   reset with FIS receive on (so the signature and status are the disk's own), then the command
   list engine. At the start, and after a sleep or a command that went wrong (6.2.2.1 asks for the
   engines to stop and start again; the reset also clears a disk that stays busy). */
static bool port_start(struct port *p) {
    struct hba *h = p->h;
    p->live = false;
    if (!port_stop(p)) {
        kprintf("ahci%d: port %u does not stop\n", h->index, p->no);
        return false;
    }
    wr(p, PxCLB, (uint32_t)(p->mem_phys + LIST_AT));
    wr(p, PxCLB + 4, (uint32_t)((p->mem_phys + LIST_AT) >> 32));
    wr(p, PxFB, (uint32_t)(p->mem_phys + FIS_AT));
    wr(p, PxFB + 4, (uint32_t)((p->mem_phys + FIS_AT) >> 32));
    wr(p, PxCMD, rd(p, PxCMD) | CMD_SUD | CMD_POD | CMD_FRE);
    if (!comreset(p)) return false;
    if (!wait_reg(p, PxTFD, TFD_BSY | TFD_DRQ, 0, READY_NS)) {
        kprintf("ahci%d: port %u: the disk does not become ready (status %x)\n", h->index, p->no, rd(p, PxTFD));
        return false;
    }
    wr(p, PxSERR, 0xFFFFFFFFu);
    wr(p, PxIS, 0xFFFFFFFFu);
    wr(p, PxIE, h->vector ? IS_DHRS | IS_ERRORS : 0);
    wr(p, PxCMD, rd(p, PxCMD) | CMD_ST);
    p->live = true;
    return true;
}

/* An H2D register FIS (SATA 3.3, 10.5.6) for `command` on `count` sectors from `lba`. */
static void fis_make(uint8_t f[20], uint8_t command, uint64_t lba, unsigned count, bool lba48) {
    memset(f, 0, 20);
    f[0] = 0x27;
    f[1] = 0x80; /* a command, not a control update */
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40; /* LBA addressing */
    if (lba48) {
        f[8] = (uint8_t)(lba >> 24);
        f[9] = (uint8_t)(lba >> 32);
        f[10] = (uint8_t)(lba >> 40);
        f[13] = (uint8_t)(count >> 8);
    } else {
        f[7] |= (uint8_t)(lba >> 24 & 0xF);
    }
    f[12] = (uint8_t)count;
}

/* Give one command in slot 0, with `bytes` of the bounce buffer, and wait for its end. 0, or -1
   when the disk reported an error, the adapter one of its own or the time ran out. */
static int command(struct port *p, const uint8_t fis[20], bool write, unsigned bytes, uint64_t ns) {
    if (!p->live && !port_start(p)) return -1;
    uint32_t *hdr = (uint32_t *)(p->mem + LIST_AT);
    uint8_t *table = p->mem + TABLE_AT;
    memset(table, 0, PRDT_AT + 16);
    memcpy(table, fis, 20);
    hdr[0] = 5u | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0); /* a 5-dword FIS, its direction, PRDs */
    hdr[1] = 0;
    hdr[2] = (uint32_t)(p->mem_phys + TABLE_AT);
    hdr[3] = (uint32_t)((p->mem_phys + TABLE_AT) >> 32);
    if (bytes) {
        uint32_t *prd = (uint32_t *)(table + PRDT_AT);
        prd[0] = (uint32_t)p->data_phys;
        prd[1] = (uint32_t)(p->data_phys >> 32);
        prd[3] = bytes - 1;
    }
    __atomic_store_n(&p->events, 0, __ATOMIC_SEQ_CST);
    wr(p, PxIS, 0xFFFFFFFFu);
    __sync_synchronize();
    wr(p, PxCI, 1);
    bool sleeps = may_sleep();
    uint64_t deadline = clock_after(ns);
    for (;;) {
        uint64_t seen = __atomic_load_n(&p->kicks, __ATOMIC_SEQ_CST); /* before the look */
        uint32_t events = rd(p, PxIS) | __atomic_load_n(&p->events, __ATOMIC_SEQ_CST);
        bool done = !(rd(p, PxCI) & 1);
        uint32_t tfd = rd(p, PxTFD); /* after CI: the status the command ended with */
        if (!(events & IS_ERRORS) && done && !(tfd & TFD_ERR)) return 0;
        if ((events & IS_ERRORS) || done) {
            kprintf("ahci%d: port %u: command %02x failed (status %x, error %x, events %x)\n", p->h->index, p->no,
                    fis[2], tfd & 0xFF, tfd >> 8 & 0xFF, events);
            p->live = false;
            return -1;
        }
        if (clock_now_ns() >= deadline) {
            kprintf("ahci%d: port %u: command %02x timed out\n", p->h->index, p->no, fis[2]);
            p->live = false;
            return -1;
        }
        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
        if (!sleeps) pause();
        else wq_wait_unless(&p->wq, &p->kicks, seen, clock_after(p->h->vector ? 10 * NS_PER_MS : NS_PER_MS));
    }
}

/* Read or write `n` logical blocks between the disk and the bounce buffer (buf, blk_by_blocks),
   given up to three times. */
static int transfer(void *ctx, bool write, uint64_t lba, unsigned n, uint8_t *buf) {
    struct port *p = ctx;
    (void)buf;
    uint8_t fis[20];
    uint8_t op = p->lba48 ? (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT) : (write ? ATA_WRITE_DMA : ATA_READ_DMA);
    fis_make(fis, op, lba, n, p->lba48);
    for (int attempt = 0; attempt < 3; attempt++)
        if (!command(p, fis, write, n * p->block, IO_NS)) return 0;
    return -1;
}

static int flush(struct port *p) {
    uint8_t fis[20];
    fis_make(fis, p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, false);
    return command(p, fis, false, 0, IO_NS);
}

static int rw(struct port *p, bool write, uint64_t lba, uint32_t count, uint8_t *buf) {
    mutex_lock(&p->lock);
    unsigned most = (p->lba48 ? XFER_PAGES * 4096 : MIN(XFER_PAGES * 4096, 256 * p->block)) / p->block;
    int r = blk_by_blocks(&p->blk, write, lba, count, buf, p->data, most, false, transfer, p);
    mutex_unlock(&p->lock);
    return r;
}

static int blk_ahci_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, false, lba, n, buf); }
static int blk_ahci_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, true, lba, n, (uint8_t *)buf);
}

/* FLUSH CACHE (EXT): the write cache reaches the medium. */
static int blk_ahci_persist(struct blkdev *b) {
    struct port *p = b->priv;
    mutex_lock(&p->lock);
    int r = flush(p) < 0 ? -N_ERR_IO : 0;
    mutex_unlock(&p->lock);
    return r;
}

static uint64_t alloc_dma(struct hba *h, unsigned pages) {
    return pmm_alloc_contig(pages, h->s64 ? 64 : 32);
}

/* IDENTIFY DEVICE (ACS-3 7.12): its size, sector size, commands and write cache. */
static bool identify(struct port *p, char model[41]) {
    uint8_t fis[20];
    fis_make(fis, ATA_IDENTIFY, 0, 0, false);
    fis[7] = 0;
    memset(p->data, 0, 512);
    if (command(p, fis, false, 512, IO_NS)) return false;
    const uint16_t *id = (const uint16_t *)p->data;
    unsigned n = 0;
    for (int w = 27; w <= 46; w++)
        for (int b = 1; b >= 0; b--) { /* each word holds its two characters high byte first */
            char ch = (char)(id[w] >> (8 * b));
            model[n++] = ch >= 0x20 && ch < 0x7F ? ch : ' ';
        }
    while (n && model[n - 1] == ' ') n--;
    model[n] = 0;
    bool valid83 = (id[83] & 0xC000) == 0x4000, valid106 = (id[106] & 0xC000) == 0x4000;
    p->lba48 = valid83 && (id[83] >> 10 & 1);
    uint64_t sectors = p->lba48 ? id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
                                : id[60] | (uint64_t)id[61] << 16;
    p->block = valid106 && (id[106] >> 12 & 1) ? 2 * (id[117] | (unsigned)id[118] << 16) : 512;
    p->cache = (id[87] & 0xC000) == 0x4000 && (id[85] >> 5 & 1);
    if (p->block < 512 || p->block > 4096 || (p->block & (p->block - 1)) || !sectors) {
        kprintf("ahci%d: port %u: %s has %u-byte sectors, skipped\n", p->h->index, p->no, model, p->block);
        return false;
    }
    p->blk.sectors = sectors * (p->block / 512);
    return true;
}

static void add_port(struct hba *h, unsigned no) {
    struct port *p = kzalloc(sizeof *p);
    if (!p) return;
    p->h = h;
    p->no = no;
    p->regs = h->regs + 0x100 + 0x80 * no;
    if (!(p->mem_phys = alloc_dma(h, 1)) || !(p->data_phys = alloc_dma(h, XFER_PAGES))) {
        kprintf("ahci%d: port %u: no memory below 4 GiB for its commands\n", h->index, no);
        kfree(p);
        return;
    }
    p->mem = phys_to_virt(p->mem_phys);
    p->data = phys_to_virt(p->data_phys);
    memset(p->mem, 0, 4096);
    h->ports[no] = p;
    char model[41];
    uint32_t sig = 0;
    if (!port_start(p) || (sig = rd(p, PxSIG)) != SIG_ATA || !identify(p, model)) {
        if (sig && sig != SIG_ATA)
            kprintf("ahci%d: port %u: %s, skipped\n", h->index, no,
                    sig == 0xEB140101u ? "an ATAPI drive" : sig == 0x96690101u ? "a port multiplier" : "not a disk");
        port_stop(p);
        h->ports[no] = NULL;
        for (unsigned i = 0; i < XFER_PAGES; i++) pmm_free(p->data_phys + i * 4096ull);
        pmm_free(p->mem_phys);
        kfree(p);
        return;
    }
    ksnprintf(p->blk.name, sizeof p->blk.name, "sata%d", ndisks++);
    strlcpy(p->blk.model, model, sizeof p->blk.model);
    p->blk.block = p->block;
    p->blk.read = blk_ahci_read;
    p->blk.write = blk_ahci_write;
    if (p->cache) p->blk.persist = blk_ahci_persist;
    p->blk.priv = p;
    blk_register(&p->blk);
}

/* The OS takes the adapter from the firmware (10.6) and switches it to AHCI mode. */
static void hba_take(struct hba *h) {
    if (hba_rd(h, CAP2) & CAP2_BOH) {
        hba_wr(h, BOHC, hba_rd(h, BOHC) | BOHC_OOS);
        uint64_t deadline = clock_after(25 * NS_PER_MS);
        while (hba_rd(h, BOHC) & BOHC_BOS && clock_now_ns() < deadline) sleep_until(clock_after(NS_PER_MS));
        /* the firmware finishes what it was doing */
        if (hba_rd(h, BOHC) & BOHC_BB) sleep_until(clock_after(2 * NS_PER_S));
    }
    hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE);
}

static void start(struct hba *h) {
    struct pci_device *pci = h->pci;
    uint64_t base = pci_bar_address(pci, 5);
    if (!base) {
        kprintf("ahci%d: no register window\n", h->index);
        return;
    }
    h->regs = vmm_map_mmio(base, 0x1100); /* the adapter's registers and those of 32 ports */
    if (!h->regs) {
        kprintf("ahci%d: no memory to map its registers\n", h->index);
        return;
    }
    pci_enable_busmaster(pci);
    hba_take(h);
    uint32_t cap = hba_rd(h, CAP), implemented = hba_rd(h, PI);
    h->s64 = cap & CAP_S64A;
    int vector = irq_vector_alloc(1, 1);
    if (vector >= 0) {
        h->vector = (uint8_t)vector;
        vector_register(vector, ahci_isr);
        if (!pci_msi_enable(pci, h->vector)) {
            irq_vector_free(vector, 1);
            h->vector = 0;
        }
    }
    kprintf("ahci%d: %04x:%04x, version %x, ports %x, %s\n", h->index, pci->vendor, pci->device, hba_rd(h, VS),
            implemented, h->vector ? "MSI" : "polled");
    /* staggered spin-up: every port's link first, so the disks spin up together */
    bool spun = false;
    for (unsigned n = 0; n < MAX_PORTS; n++) {
        if (!(implemented & 1u << n)) continue;
        volatile uint8_t *cmd = h->regs + 0x100 + 0x80 * n + PxCMD;
        if (!(mmio_read32(cmd) & CMD_SUD)) {
            mmio_write32(cmd, mmio_read32(cmd) | CMD_SUD | CMD_POD);
            spun = true;
        }
    }
    if (spun && (cap & CAP_SSS)) sleep_until(clock_after(SPINUP_NS));
    if (h->vector) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_IE);
    for (unsigned n = 0; n < MAX_PORTS; n++) {
        if (!(implemented & 1u << n)) continue;
        uint32_t det = mmio_read32(h->regs + 0x100 + 0x80 * n + PxSSTS) & 0xF;
        if (det) add_port(h, n); /* 0: nothing there */
    }
}

/* Before the power goes away: the cache on the medium and the disk spun down (STANDBY IMMEDIATE),
   so a hard disk parks its heads and does not count an emergency stop; then the engines stopped,
   no DMA after this. Interrupts are already off. */
static bool ahci_quiesce(void *arg) {
    (void)arg;
    bool all = true;
    for (int i = 0; i < nhbas; i++)
        for (unsigned n = 0; n < MAX_PORTS; n++) {
            struct port *p = hbas[i].ports[n];
            if (!p || !p->live) continue;
            uint8_t fis[20];
            fis_make(fis, ATA_STANDBY_IMMEDIATE, 0, 0, false);
            fis[7] = 0;
            if ((p->cache && flush(p)) || command(p, fis, false, 0, READY_NS)) all = false;
            if (!port_stop(p)) all = false;
            p->live = false;
        }
    return all;
}

/* After a sleep: the next command sets the port up again (the adapter lost its state with the
   power; quiesce left it not live), the way it recovers from a command that went wrong. Resume must
   not wait for the disk. */
static bool ahci_resume(void *arg) {
    (void)arg;
    for (int i = 0; i < nhbas; i++) {
        struct hba *h = &hbas[i];
        if (h->regs) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE | (h->vector ? GHC_IE : 0));
    }
    return true;
}

static struct pm_device ahci_pm = {.name = "ahci", .stage = PM_STAGE_STORAGE, .quiesce = ahci_quiesce,
                                   .resume = ahci_resume};

void ahci_init(void) {
    for (int i = 0; i < pci_count() && nhbas < MAX_HBA; i++) {
        struct pci_device *p = pci_at(i);
        if (p->cls != 0x01 || p->subcls != 0x06 || p->progif != 0x01) continue;
        struct hba *h = &hbas[nhbas];
        h->index = nhbas++;
        h->pci = p;
        start(h);
    }
    if (nhbas) pm_register(&ahci_pm);
}
