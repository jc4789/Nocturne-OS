/* SATA disks behind AHCI host bus adapters (Serial ATA AHCI 1.3.1), the way nearly every PC
   attaches them. Every port with an ATA disk becomes a block device "sataN"; ATAPI drives and port
   multipliers are skipped.

   A port has as many command slots as its adapter gives (CAP.NCS). A disk with native command
   queuing (CAP.SNCQ and the disk's IDENTIFY word 76) takes its reads and writes as READ/WRITE
   FPDMA QUEUED, as many at once as both have slots and the disk has tags (word 75), from any CPU;
   the rest of its commands (flush, identify, standby) have the port to themselves, since a
   non-queued command may not meet a queued one (SATA 3.3, 13.6). A disk without it takes one
   command at a time. The data goes straight between the disk and the caller's memory, a PRD for
   each physically contiguous run; a bounce buffer only for a request that does not cover whole
   blocks of word aligned memory, or for an adapter that reaches only the first 4 GiB.

   Completions arrive by MSI, or are polled when the platform offers none. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/topology.h"
#include "dev/blk.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define PORTS 32          /* PI has a bit for each */
#define XFER_PAGES 128    /* the most a command moves: 512 KiB */
#define PRDS (XFER_PAGES + 1) /* any 512 KiB of memory, aligned anyhow, in runs of a page at worst */
#define PRD_MOST (4u << 20) /* one PRD's bytes (4.2.3.3) */
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
#define CAP_SNCQ (1u << 30)
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
#define PxSACT 0x34
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
#define IS_DHRS 1u            /* a Device to Host Register FIS: how a non-queued command ends */
#define IS_SDBS 8u            /* a Set Device Bits FIS: how queued commands end */
#define IS_ERRORS 0x78000000u /* interface fatal, host bus data, host bus fatal, task file */
#define DET_UP 3u             /* PxSSTS.DET: a device, and the link established */
#define SIG_ATA 0x00000101u

/* The port's memory, one run: the command list (32 headers), the FISes it receives, then a command
   table for each slot: the command FIS and its PRDs, 128-byte aligned (4.2). */
#define LIST_AT 0x000
#define FIS_AT 0x400
#define TABLES_AT 0x800
#define PRDT_AT 0x80 /* in a command table, after the command FIS */
#define TABLE_BYTES ALIGN_UP(PRDT_AT + PRDS * 16, 128)

/* ATA commands (ACS-3) */
#define ATA_READ_DMA 0xC8
#define ATA_WRITE_DMA 0xCA
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_READ_FPDMA_QUEUED 0x60
#define ATA_WRITE_FPDMA_QUEUED 0x61
#define ATA_FLUSH_CACHE 0xE7
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_STANDBY_IMMEDIATE 0xE0
#define ATA_IDENTIFY 0xEC

enum { PENDING, DONE, FAILED };

struct hba;

/* A command slot's end: reap sets status and moves seq on, under the port's lock, then wakes wq.
   The waiter reads seq before it gives the command and sleeps while seq still reads that. */
struct slot {
    struct wait_queue wq;
    uint64_t seq;
    int status;
};

struct port {
    struct blkdev blk;
    struct hba *h;
    unsigned no;
    volatile uint8_t *regs;
    uint8_t *mem; /* LIST_AT, FIS_AT, TABLES_AT */
    uint64_t mem_phys;
    unsigned pages; /* of mem */
    unsigned block; /* logical sector in bytes */
    unsigned most;  /* blocks a command moves at most */
    bool lba48, cache, ncq; /* 48-bit commands; a write cache that is on; queued reads and writes */
    uint32_t all;   /* the slots the port uses: with ncq as many as the disk has tags, else one */
    /* An adapter that reaches only the first 4 GiB: every transfer through this buffer below it,
       one at a time under low_lock. NULL for every other. */
    uint8_t *low;
    uint64_t low_phys;
    struct mutex low_lock;

    /* Under lock, with the interrupt handler: the slots taken (a command being put together or in
       the port) and issued (in the port: PxCI, and PxSACT for a queued one, written under the lock,
       so reap never takes a slot as done before the port had it); sole: one command, or a reset,
       has the port to itself; wanting: callers waiting for sole, before whom no queued command is
       let in; live: set up, commands may be given (cleared by a command that went wrong and by a
       sleep: the next caller sets the port up again first); halting: a caller whose command ran
       out of time is stopping the engines. A caller waiting for room sleeps in room while freed
       still reads what it read before it looked. */
    struct spinlock lock;
    uint32_t taken, issued;
    bool sole, live, halting;
    unsigned wanting;
    uint32_t error_is, error_tfd; /* what the last failure showed, for its log line */
    struct wait_queue room;
    uint64_t freed;
    struct slot slots[PORTS];
};

struct hba {
    struct hba *next;
    int index;
    struct pci_device *pci;
    volatile uint8_t *regs;
    bool s64, sncq;
    unsigned nslots;
    struct irq *irq; /* its message (pci_irq_enable), NULL: polled */
    struct port *ports[PORTS];
};

static struct hba *hbas; /* set up at boot, before anything else looks */
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

/* lock held: the issued commands in `which` end with status; returns the slots to wake. */
static uint32_t finish(struct port *p, uint32_t which, int status) {
    for (uint32_t m = which; m; m &= m - 1) {
        struct slot *s = &p->slots[__builtin_ctz(m)];
        s->status = status;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
    }
    p->issued &= ~which;
    return which;
}

static void wake_slots(struct port *p, uint32_t which) {
    for (; which; which &= which - 1) wq_wake_all(&p->slots[__builtin_ctz(which)].wq);
}

/* The port's events: the commands it finished, or every command in it failed when it reported an
   error (a queued command's error aborts the others in the disk too, SATA 3.3 13.6.3.2; they are
   given again after the reset). The interrupt handler, and a waiter that polls. */
static void reap(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    uint32_t is = rd(p, PxIS), woken = 0;
    if (is) wr(p, PxIS, is);
    if (p->issued) {
        uint32_t out = rd(p, PxSACT) | rd(p, PxCI), tfd = rd(p, PxTFD);
        bool gone = is == 0xFFFFFFFFu, broke = gone || (is & IS_ERRORS);
        uint32_t done = broke ? 0 : p->issued & ~out;
        /* a non-queued command ends with the status in the task file */
        if (done && p->sole && (tfd & TFD_ERR)) broke = true;
        if (broke) {
            p->error_is = is;
            p->error_tfd = tfd;
            p->live = false;
            woken = finish(p, p->issued, FAILED);
        } else if (done) {
            woken = finish(p, done, DONE);
        }
    }
    spin_unlock(&p->lock, f);
    wake_slots(p, woken);
}

static enum irq_result ahci_isr(void *context) {
    struct hba *h = context;
    uint32_t is = hba_rd(h, IS);
    if (is == 0xFFFFFFFFu) return IRQ_NONE;
    for (uint32_t m = is; m; m &= m - 1) {
        unsigned n = (unsigned)__builtin_ctz(m);
        struct port *p = h->ports[n];
        if (p) {
            reap(p);
        } else {
            volatile uint8_t *regs = h->regs + 0x100 + 0x80 * n;
            mmio_write32(regs + PxIS, mmio_read32(regs + PxIS));
        }
    }
    hba_wr(h, IS, is);
    return IRQ_HANDLED;
}

/* The port's command list and FIS receive engines stopped (10.1.2), so its memory may change and
   a COMRESET may be sent. Stopping the command list engine clears PxCI and PxSACT: nothing in
   the port moves data after this. */
static bool port_stop(struct port *p) {
    uint32_t cmd = rd(p, PxCMD);
    if (cmd & CMD_ST) wr(p, PxCMD, cmd & ~CMD_ST);
    if (!wait_reg(p, PxCMD, CMD_CR, 0, STOP_NS)) return false;
    cmd = rd(p, PxCMD);
    if (cmd & CMD_FRE) wr(p, PxCMD, cmd & ~CMD_FRE);
    return wait_reg(p, PxCMD, CMD_FR, 0, STOP_NS);
}

/* COMRESET (10.4.2): the link set up again and the device reset; it then sends its signature in a
   register FIS. Its power states are kept to active (IPM 3). */
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
   engines to stop and start again; the reset also clears a disk that stays busy). The caller has
   the port to itself (sole), no command in it. */
static bool port_start(struct port *p) {
    struct hba *h = p->h;
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
    wr(p, PxIE, h->irq ? IS_DHRS | IS_SDBS | IS_ERRORS : 0);
    wr(p, PxCMD, rd(p, PxCMD) | CMD_ST);
    uint64_t f = spin_lock(&p->lock);
    p->live = true;
    spin_unlock(&p->lock, f);
    return true;
}

/* Wait for leave to free room: it wakes the waiter; in case a wake is lost, the waiter looks again
   after 10 ms (polled: each ms). With interrupts off it spins. */
static void wait_room(struct port *p, uint64_t seen) {
    if (!may_sleep()) pause();
    else wq_wait_unless(&p->room, &p->freed, seen, clock_after(p->h->irq ? 10 * NS_PER_MS : NS_PER_MS));
}

/* A slot of the live port for a command: a queued one (alone false) beside the other queued ones,
   or the port to itself (no command in it, no other let in). A port that is not live is set up
   first, by the caller that finds it so once it is empty. -1 when it cannot be. */
static int enter(struct port *p, bool alone) {
    bool wanted = false;
    for (;;) {
        uint64_t seen = __atomic_load_n(&p->freed, __ATOMIC_ACQUIRE);
        uint64_t f = spin_lock(&p->lock);
        if (!p->sole) {
            if (alone || !p->live) {
                if (!p->taken) {
                    p->sole = true;
                    if (wanted) p->wanting--;
                    wanted = false;
                    bool live = p->live;
                    if (alone) p->taken = 1;
                    spin_unlock(&p->lock, f);
                    if (live || port_start(p)) {
                        if (alone) return 0;
                        f = spin_lock(&p->lock); /* set up: now its turn as a queued command */
                        p->sole = false;
                        spin_unlock(&p->lock, f);
                        __atomic_add_fetch(&p->freed, 1, __ATOMIC_RELEASE);
                        wq_wake_all(&p->room);
                        continue;
                    }
                    f = spin_lock(&p->lock);
                    p->sole = false;
                    if (alone) p->taken = 0;
                    spin_unlock(&p->lock, f);
                    __atomic_add_fetch(&p->freed, 1, __ATOMIC_RELEASE);
                    wq_wake_all(&p->room);
                    return -1;
                }
                if (!wanted) {
                    p->wanting++;
                    wanted = true;
                }
            } else if (!p->wanting && (p->taken & p->all) != p->all) {
                int slot = __builtin_ctz(~p->taken & p->all);
                p->taken |= 1u << slot;
                spin_unlock(&p->lock, f);
                return slot;
            }
        }
        spin_unlock(&p->lock, f);
        wait_room(p, seen);
    }
}

static void leave(struct port *p, int slot, bool alone) {
    uint64_t f = spin_lock(&p->lock);
    p->taken &= ~(1u << slot);
    if (alone) p->sole = false;
    spin_unlock(&p->lock, f);
    __atomic_add_fetch(&p->freed, 1, __ATOMIC_RELEASE);
    wq_wake_all(&p->room);
}

/* A command of the caller's ran out of time: the engines stopped, so nothing in the port moves
   data into memory its callers are about to give back, and every command in it failed. The first
   caller to find it so does it; the others wait for their own commands to fail. */
static void halt(struct port *p, uint32_t bit) {
    uint64_t f = spin_lock(&p->lock);
    if (p->halting || !(p->issued & bit)) {
        spin_unlock(&p->lock, f);
        return;
    }
    p->halting = true;
    p->live = false;
    spin_unlock(&p->lock, f);
    port_stop(p);
    f = spin_lock(&p->lock);
    p->error_is = 0;
    p->error_tfd = rd(p, PxTFD);
    uint32_t woken = finish(p, p->issued, FAILED);
    p->halting = false;
    spin_unlock(&p->lock, f);
    wake_slots(p, woken);
}

/* The PRDs for `bytes` at buf in the slot's table (4.2.3.3): one for each physically contiguous
   run. 0 when a page is not mapped or lies beyond the adapter's reach. */
static unsigned prds(struct port *p, uint8_t *table, const uint8_t *buf, unsigned bytes) {
    uint32_t *prd = (uint32_t *)(table + PRDT_AT);
    unsigned n = 0;
    uint64_t va = (uint64_t)buf, end = 0;
    while (bytes) {
        unsigned run = (unsigned)MIN((uint64_t)bytes, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        uint64_t pa = vmm_kernel_translate(va);
        if (!pa || (!p->h->s64 && pa + run > 1ull << 32)) return 0;
        uint32_t *last = &prd[4 * (n - 1)];
        if (n && pa == end && (last[3] & 0x3FFFFF) + 1 + run <= PRD_MOST) {
            last[3] += run;
        } else {
            if (n == PRDS) return 0;
            uint32_t *e = &prd[4 * n++];
            e[0] = (uint32_t)pa;
            e[1] = (uint32_t)(pa >> 32);
            e[2] = 0;
            e[3] = run - 1; /* no interrupt of its own: the command's FIS ends it */
        }
        end = pa + run;
        va += run;
        bytes -= run;
    }
    return n;
}

/* An H2D register FIS (SATA 3.3, 10.5.6) for `command` on `count` sectors from `lba`. A queued
   one (13.6.4) takes its count in the features and its tag in the count, given at issue. */
static void fis_make(uint8_t f[20], uint8_t command, uint64_t lba, unsigned count, bool lba48, bool queued) {
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
    } else {
        f[7] |= (uint8_t)(lba >> 24 & 0xF);
    }
    if (queued) {
        f[3] = (uint8_t)count;
        f[11] = (uint8_t)(count >> 8);
    } else {
        f[12] = (uint8_t)count;
        if (lba48) f[13] = (uint8_t)(count >> 8);
    }
}

/* Give one command with `bytes` at buf and wait for its end: queued beside the others, or with
   the port to itself. 0; -1 when the disk reported an error, the adapter one of its own or the
   time ran out; -2 when buf cannot be described to the adapter. */
static int command(struct port *p, const uint8_t fis[20], bool queued, bool write, const uint8_t *buf,
                   unsigned bytes, uint64_t ns) {
    int slot = enter(p, !queued);
    if (slot < 0) return -1;
    uint32_t bit = 1u << slot;
    uint64_t table_phys = p->mem_phys + TABLES_AT + (uint64_t)slot * TABLE_BYTES;
    uint8_t *table = p->mem + TABLES_AT + (size_t)slot * TABLE_BYTES;
    memset(table, 0, PRDT_AT);
    memcpy(table, fis, 20);
    if (queued) table[12] = (uint8_t)(slot << 3); /* the tag is the slot (5.3.1) */
    unsigned n = bytes ? prds(p, table, buf, bytes) : 0;
    if (bytes && !n) {
        leave(p, slot, !queued);
        kprintf("ahci%d: port %u: a buffer for command %02x is not mapped, or beyond the adapter's reach\n",
                p->h->index, p->no, fis[2]);
        return -2;
    }
    uint32_t *hdr = (uint32_t *)(p->mem + LIST_AT) + 8 * slot;
    hdr[0] = 5u | (write ? 1u << 6 : 0) | n << 16; /* a 5-dword FIS, its direction, its PRDs */
    hdr[1] = 0;
    hdr[2] = (uint32_t)table_phys;
    hdr[3] = (uint32_t)(table_phys >> 32);

    struct slot *s = &p->slots[slot];
    uint64_t start = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    uint64_t f = spin_lock(&p->lock);
    bool live = p->live;
    if (live) {
        s->status = PENDING;
        p->issued |= bit;
        __atomic_thread_fence(__ATOMIC_RELEASE); /* the table and header before the port sees them */
        if (queued) wr(p, PxSACT, bit); /* before PxCI (5.3.1) */
        wr(p, PxCI, bit);
    }
    spin_unlock(&p->lock, f);
    if (!live) { /* an error since enter: the next caller sets it up again */
        leave(p, slot, !queued);
        return -1;
    }

    bool sleeps = may_sleep(), timed_out = false, looked = false;
    uint64_t deadline = clock_after(ns);
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seen != start) break;
        if (!p->h->irq || !sleeps || looked) reap(p);
        looked = false;
        if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != start) break;
        if (clock_now_ns() >= deadline) {
            if (!timed_out) kprintf("ahci%d: port %u: command %02x timed out\n", p->h->index, p->no, fis[2]);
            timed_out = true;
            halt(p, bit); /* fails it, or another caller is doing so */
            deadline = clock_after(NS_PER_MS);
            continue;
        }
        if (!sleeps) {
            pause();
            continue;
        }
        /* the interrupt wakes it; 10 ms in case one is lost, then it looks itself; polled each ms */
        wq_wait_unless(&s->wq, &s->seq, seen, MIN(deadline, clock_after(p->h->irq ? 10 * NS_PER_MS : NS_PER_MS)));
        looked = true;
    }
    f = spin_lock(&p->lock);
    int status = s->status;
    uint32_t is = p->error_is, tfd = p->error_tfd;
    spin_unlock(&p->lock, f);
    leave(p, slot, !queued);
    if (status == DONE) return 0;
    if (!timed_out)
        kprintf("ahci%d: port %u: command %02x failed (status %x, error %x, events %x)\n", p->h->index, p->no, fis[2],
                tfd & 0xFF, tfd >> 8 & 0xFF, is);
    return -1;
}

/* Read or write `n` logical blocks between the disk and buf (blk_by_blocks), given up to three
   times. */
static int transfer(void *ctx, bool write, uint64_t lba, unsigned n, uint8_t *buf) {
    struct port *p = ctx;
    uint8_t fis[20];
    uint8_t op = p->ncq     ? (write ? ATA_WRITE_FPDMA_QUEUED : ATA_READ_FPDMA_QUEUED)
                 : p->lba48 ? (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT)
                            : (write ? ATA_WRITE_DMA : ATA_READ_DMA);
    fis_make(fis, op, lba, n, p->lba48, p->ncq);
    for (int attempt = 0; attempt < 3; attempt++) {
        int r = command(p, fis, p->ncq, write, buf, n * p->block, IO_NS);
        if (r != -1) return r ? -1 : 0;
    }
    return -1;
}

static int flush(struct port *p) {
    uint8_t fis[20];
    fis_make(fis, p->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, false, false);
    return command(p, fis, false, false, NULL, 0, IO_NS);
}

/* 512-byte sectors on top of the disk's logical blocks: the caller's memory itself where the
   request covers whole blocks of word aligned memory (a PRD's address is), else a bounce buffer of
   its own; for an adapter that reaches only 4 GiB the port's buffer below it. */
static int rw(struct port *p, bool write, uint64_t lba, uint32_t count, uint8_t *buf) {
    if (p->low) {
        mutex_lock(&p->low_lock);
        int r = blk_by_blocks(&p->blk, write, lba, count, buf, p->low, p->most, false, transfer, p);
        mutex_unlock(&p->low_lock);
        return r;
    }
    unsigned per = p->block / 512, most = p->most;
    bool direct = !((uint64_t)buf & 1), whole = !(lba % per) && !(count % per);
    uint8_t *bounce = NULL;
    if (!direct || !whole) {
        size_t bytes = (size_t)MIN((uint64_t)most, (count + 2ull * per - 1) / per) * p->block;
        if (!(bounce = kmalloc(bytes))) return -N_ERR_NO_MEMORY;
        most = (unsigned)(bytes / p->block);
    }
    int r = blk_by_blocks(&p->blk, write, lba, count, buf, bounce, most, direct, transfer, p);
    kfree(bounce);
    return r;
}

static int blk_ahci_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, false, lba, n, buf); }
static int blk_ahci_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, true, lba, n, (uint8_t *)buf);
}

/* FLUSH CACHE (EXT): the write cache reaches the medium, with every write that ended before it. */
static int blk_ahci_persist(struct blkdev *b) { return flush(b->priv) < 0 ? -N_ERR_IO : 0; }

/* count contiguous pages the adapter reaches, on its node first. */
static uint64_t alloc_dma(struct hba *h, unsigned count) {
    if (!h->s64) return pmm_alloc_contig(count, 32);
    int node = h->pci->node >= 0 ? h->pci->node : (int)topology.cpu[cpu_current_index()].node;
    uint64_t phys = pmm_alloc_run(count, (unsigned)node);
    return phys ? phys : pmm_alloc_contig(count, 64);
}

/* IDENTIFY DEVICE (ACS-3 7.12): its size, sector size, commands, queue and write cache. */
static bool identify(struct port *p, char model[41]) {
    uint64_t page = alloc_dma(p->h, 1);
    if (!page) return false;
    const uint16_t *id = phys_to_virt(page);
    uint8_t fis[20];
    fis_make(fis, ATA_IDENTIFY, 0, 0, false, false);
    fis[7] = 0;
    memset((void *)id, 0, 512);
    if (command(p, fis, false, false, (const uint8_t *)id, 512, IO_NS)) {
        pmm_free(page);
        return false;
    }
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
    /* queued commands need the adapter's and the disk's word (76 bit 8), and 48-bit addresses */
    p->ncq = p->h->sncq && p->lba48 && id[76] != 0xFFFF && (id[76] >> 8 & 1);
    unsigned depth = p->ncq ? MIN(p->h->nslots, (id[75] & 0x1Fu) + 1) : 1;
    p->all = depth == 32 ? 0xFFFFFFFFu : (1u << depth) - 1;
    pmm_free(page);
    if (p->block < 512 || p->block > 4096 || (p->block & (p->block - 1)) || !sectors) {
        kprintf("ahci%d: port %u: %s has %u-byte sectors, skipped\n", p->h->index, p->no, model, p->block);
        return false;
    }
    p->most = (p->lba48 ? XFER_PAGES * PAGE_SIZE : MIN(XFER_PAGES * PAGE_SIZE, 256 * p->block)) / p->block;
    p->blk.sectors = sectors * (p->block / 512);
    return true;
}

static void port_free(struct port *p) {
    if (p->low_phys) pmm_free_contig(p->low_phys, XFER_PAGES);
    pmm_free_contig(p->mem_phys, p->pages);
    kfree(p);
}

static void add_port(struct hba *h, unsigned no) {
    struct port *p = kzalloc(sizeof *p);
    if (!p) return;
    p->h = h;
    p->no = no;
    p->regs = h->regs + 0x100 + 0x80 * no;
    p->all = 1;
    p->pages = (unsigned)(ALIGN_UP(TABLES_AT + (uint64_t)h->nslots * TABLE_BYTES, PAGE_SIZE) / PAGE_SIZE);
    if (!(p->mem_phys = alloc_dma(h, p->pages))) {
        kprintf("ahci%d: port %u: no memory %sfor its commands\n", h->index, no, h->s64 ? "" : "below 4 GiB ");
        kfree(p);
        return;
    }
    p->mem = phys_to_virt(p->mem_phys);
    memset(p->mem, 0, (size_t)p->pages * PAGE_SIZE);
    if (!h->s64) {
        if (!(p->low_phys = alloc_dma(h, XFER_PAGES))) {
            kprintf("ahci%d: port %u: no memory below 4 GiB for its transfers\n", h->index, no);
            port_free(p);
            return;
        }
        p->low = phys_to_virt(p->low_phys);
    }
    h->ports[no] = p;
    char model[41];
    uint32_t sig = 0;
    int slot = enter(p, true); /* sets it up */
    if (slot >= 0) {
        sig = rd(p, PxSIG);
        leave(p, slot, true);
    }
    if (slot < 0 || sig != SIG_ATA || !identify(p, model)) {
        if (sig && sig != SIG_ATA)
            kprintf("ahci%d: port %u: %s, skipped\n", h->index, no,
                    sig == 0xEB140101u ? "an ATAPI drive" : sig == 0x96690101u ? "a port multiplier" : "not a disk");
        port_stop(p);
        h->ports[no] = NULL;
        port_free(p);
        return;
    }
    ksnprintf(p->blk.name, sizeof p->blk.name, "sata%d", ndisks++);
    strlcpy(p->blk.model, model, sizeof p->blk.model);
    p->blk.block = p->block;
    p->blk.read = blk_ahci_read;
    p->blk.write = blk_ahci_write;
    if (p->cache) p->blk.persist = blk_ahci_persist;
    p->blk.priv = p;
    kprintf("ahci%d: port %u: %s, %s\n", h->index, no, model,
            p->ncq ? "queued commands" : "one command at a time");
    if (p->ncq) kprintf("ahci%d: port %u: %u commands at once\n", h->index, no, (unsigned)__builtin_popcount(p->all));
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
    h->regs = vmm_map_mmio(base, 0x100 + 0x80 * PORTS); /* the adapter's registers and those of every port */
    if (!h->regs) {
        kprintf("ahci%d: no memory to map its registers\n", h->index);
        return;
    }
    pci_enable_busmaster(pci);
    hba_take(h);
    uint32_t cap = hba_rd(h, CAP), implemented = hba_rd(h, PI);
    h->s64 = cap & CAP_S64A;
    h->sncq = cap & CAP_SNCQ;
    h->nslots = (cap >> 8 & 0x1F) + 1;
    pci_irq_enable(pci, &(struct irq_spec){.name = "AHCI", .handler = ahci_isr, .context = h, .node = pci->node}, 1, &h->irq);
    kprintf("ahci%d: %04x:%04x, version %x, ports %x, %u slots%s, %s\n", h->index, pci->vendor, pci->device,
            hba_rd(h, VS), implemented, h->nslots, h->sncq ? ", queued commands" : "", h->irq ? "MSI" : "polled");
    /* staggered spin-up: every port's link first, so the disks spin up together */
    bool spun = false;
    for (uint32_t m = implemented; m; m &= m - 1) {
        volatile uint8_t *cmd = h->regs + 0x100 + 0x80 * __builtin_ctz(m) + PxCMD;
        if (!(mmio_read32(cmd) & CMD_SUD)) {
            mmio_write32(cmd, mmio_read32(cmd) | CMD_SUD | CMD_POD);
            spun = true;
        }
    }
    if (spun && (cap & CAP_SSS)) sleep_until(clock_after(SPINUP_NS));
    if (h->irq) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_IE);
    for (uint32_t m = implemented; m; m &= m - 1) {
        unsigned n = (unsigned)__builtin_ctz(m);
        uint32_t det = mmio_read32(h->regs + 0x100 + 0x80 * n + PxSSTS) & 0xF;
        if (det) add_port(h, n); /* 0: nothing there */
    }
}

/* Before the power goes away: the cache on the medium and the disk spun down (STANDBY IMMEDIATE),
   so a hard disk parks its heads and does not count an emergency stop; then the engines stopped,
   no DMA after this. Interrupts are already off, and blk_hold let no request in. */
static bool ahci_quiesce(void *arg) {
    (void)arg;
    bool all = true;
    for (struct hba *h = hbas; h; h = h->next)
        for (unsigned n = 0; n < PORTS; n++) {
            struct port *p = h->ports[n];
            if (!p || !p->live) continue;
            uint8_t fis[20];
            fis_make(fis, ATA_STANDBY_IMMEDIATE, 0, 0, false, false);
            fis[7] = 0;
            if ((p->cache && flush(p)) || command(p, fis, false, false, NULL, 0, READY_NS)) all = false;
            if (!port_stop(p)) all = false;
            uint64_t f = spin_lock(&p->lock);
            p->live = false;
            spin_unlock(&p->lock, f);
        }
    return all;
}

/* After a sleep: the next command sets the port up again (the adapter lost its state with the
   power; quiesce left it not live), the way it recovers from a command that went wrong. Resume must
   not wait for the disk. */
static bool ahci_resume(void *arg) {
    (void)arg;
    for (struct hba *h = hbas; h; h = h->next)
        if (h->regs) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE | (h->irq ? GHC_IE : 0));
    return true;
}

static struct pm_device ahci_pm = {.name = "ahci", .stage = PM_STAGE_STORAGE, .quiesce = ahci_quiesce,
                                   .resume = ahci_resume};

void ahci_init(void) {
    struct hba **tail = &hbas;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *p = pci_at(i);
        if (p->cls != 0x01 || p->subcls != 0x06 || p->progif != 0x01) continue;
        struct hba *h = kzalloc(sizeof *h);
        if (!h) {
            kprintf("ahci: no memory for the adapter at %04x:%04x\n", p->vendor, p->device);
            continue;
        }
        h->index = nhbas++;
        h->pci = p;
        *tail = h;
        tail = &h->next;
        start(h);
    }
    if (hbas) pm_register(&ahci_pm);
}
