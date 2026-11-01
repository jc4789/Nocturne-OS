/* SATA disks behind AHCI host bus adapters (Serial ATA AHCI 1.3.1), the way nearly every PC
   attaches them. Every implemented port is set up, with a disk or without (10.1.2); every ATA disk
   that is or comes on one becomes a block device "sataN"; ATAPI drives and port multipliers are
   skipped.

   A port has as many command slots as its adapter gives (CAP.NCS). A disk with native command
   queuing (CAP.SNCQ and the disk's IDENTIFY word 76) takes its reads and writes as READ/WRITE
   FPDMA QUEUED, as many at once as both have slots and the disk has tags (word 75), from any CPU;
   the rest of its commands (flush, identify, standby) have the port to themselves, since a
   non-queued command may not meet a queued one (SATA 3.3, 13.6). A disk without it takes one
   command at a time. The data goes straight between the disk and the caller's memory, a PRD for
   each physically contiguous run; a bounce buffer only for a request that does not cover whole
   blocks of word aligned memory, or for an adapter that reaches only the first 4 GiB.

   Completions arrive by MSI, or are polled when the platform offers none. A thread for each adapter
   handles what may wait: a device that came or went (7.3, 6.2.2.3), and what the adapter lost in
   a sleep.

   A failure goes down the steps of 10.4: the command list engine stopped and started again, a
   COMRESET when the device stays busy or a queued command was in it, an HBA reset when the
   engines do not stop, and bus mastering turned off when even that does not end. A caller
   gets its failure only once one of these has ended the port's DMA, so it may take its memory
   back; or at a task file error, after which the adapter issues no new command and acknowledges
   no DMA Setup until PxCMD.ST is cleared (6.2.2). */
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
#include "sys/thread.h"
#include "sys/wait.h"

#define PORTS 32          /* PI has a bit for each */
#define XFER_PAGES 128    /* the most a command moves: 512 KiB */
#define PRDS (XFER_PAGES + 1) /* any 512 KiB of memory, aligned anyhow, in runs of a page at worst */
#define PRD_MOST (4u << 20) /* one PRD's bytes (4.2.3.3) */
#define LINK_NS NS_PER_S            /* after a COMRESET or a spin-up the link is up within this */
#define READY_NS (10 * NS_PER_S)    /* a disk that was asleep or spinning up answers within this */
#define STOP_NS (500 * NS_PER_MS)   /* a port's engines stop within this (10.1.2) */
#define HBA_RESET_NS NS_PER_S       /* GHC.HR falls within this, or the adapter is hung (10.4.3) */
#define OWNER_NS (25 * NS_PER_MS)   /* the firmware shows it is busy within this (10.6.3) */
#define OWNER_BUSY_NS (2 * NS_PER_S) /* and then has at least this to finish (10.6.3) */
#define IO_NS (30 * NS_PER_S)
#define RETRY_NS NS_PER_S           /* a port with a device that does not answer is looked at again after this */
#define RETRY_MOST_NS (60 * NS_PER_S) /* and twice as long each time, up to this */

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
#define CAP_SSC (1u << 14)
#define CAP2_BOH 1u
#define CAP2_SDS (1u << 3)
#define CAP2_SADM (1u << 4)
#define GHC_HR 1u
#define GHC_IE 2u
#define GHC_AE (1u << 31)
#define BOHC_BOS 1u
#define BOHC_OOS 2u
#define BOHC_OOC 8u
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
#define SERR_LINK ((1u << 16) | (1u << 26)) /* DIAG.N, DIAG.X (3.3.12) */
#define PxDEVSLP 0x44
#define DEVSLP_ADSE 1u
#define CMD_ST 1u
#define CMD_SUD 2u
#define CMD_POD 4u
#define CMD_FRE 0x10u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define CMD_MPSP (1u << 19)
#define CMD_CPD (1u << 20)
#define CMD_ALPE (1u << 26)
#define CMD_ASP (1u << 27)
#define CMD_ICC (0xFu << 28)
#define CMD_ICC_SLUMBER (6u << 28)
#define TFD_ERR 1u
#define TFD_DRQ 8u
#define TFD_BSY 0x80u
#define IS_DHRS 1u            /* a Device to Host Register FIS: how a non-queued command ends */
#define IS_PSS 2u             /* a PIO Setup FIS: how a command that reads by PIO ends (5.3.9) */
#define IS_SDBS 8u            /* a Set Device Bits FIS: how queued commands end */
#define IS_PCS (1u << 6)      /* PxSERR.DIAG.X, cleared there: a device spoke up (7.3) */
#define IS_PRCS (1u << 22)    /* PxSERR.DIAG.N, cleared there: the link came or went (7.3) */
#define IS_OFS (1u << 24)     /* overflow: the HBA goes on with the command, its data cut (6.1.5) */
#define IS_IFS (1u << 27)     /* interface fatal */
#define IS_HBDS (1u << 28)    /* host bus data */
#define IS_HBFS (1u << 29)    /* host bus fatal */
#define IS_TFES (1u << 30)    /* task file */
#define IS_ERRORS (IS_IFS | IS_HBDS | IS_HBFS | IS_TFES) /* the port stopped on these (6.1) */
#define IS_FATAL (IS_OFS | IS_IFS | IS_HBDS | IS_HBFS) /* for these the registers end no command (settle) */
#define IS_LINK (IS_PCS | IS_PRCS) /* the port goes on: someone must stop it */
#define IS_ALL (IS_DHRS | IS_PSS | IS_SDBS | IS_ERRORS | IS_LINK | IS_OFS)
#define DET_FOUND 1u          /* PxSSTS.DET: bit 0, in 1h (a device) and 3h (and the link up) alone */
#define DET_UP 3u             /* PxSSTS.DET: a device, and the link established */
#define IPM_MASK 0xF00u       /* PxSSTS.IPM (3.3.10) */
#define IPM_ACTIVE 0x100u
#define IPM_SLUMBER 0x600u
/* PxSCTL.IPM: neither Partial nor Slumber, nor DevSleep on an adapter that has it (3.3.11, 7.3.1.1) */
#define SCTL_NO_POWER_STATES(h) ((h)->sds ? 0x700u : 0x300u)
/* PxSCTL.IPM: Slumber alone allowed (3.3.11) */
#define SCTL_SLUMBER_ALONE(h) ((h)->sds ? 0x500u : 0x100u)
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

struct port;

/* A disk the port has or had: its block device stays for good (blk.c), the same one if the same
   disk comes back to the port. blk.gone: it is not there, its requests fail at once. */
struct disk {
    struct blkdev blk;
    struct port *p;
    struct disk *older; /* the disk the port knew before this one */
    unsigned block; /* logical sector in bytes */
    unsigned most;  /* blocks a command moves at most */
    bool lba48, cache, ncq; /* 48-bit commands; a write cache that is on; queued reads and writes */
    uint32_t all;   /* the slots it uses: with ncq as many as the disk has tags, else one */
    uint16_t serial[10]; /* IDENTIFY words 10-19: with its size, how a disk is known again */
};

struct port {
    struct hba *h;
    unsigned no;
    volatile uint8_t *regs;
    uint8_t *mem; /* LIST_AT, FIS_AT, TABLES_AT */
    uint64_t mem_phys;
    unsigned pages; /* of mem */
    struct disk *disk; /* the last one seen; written with the port to itself */
    struct disk *disks; /* atomic: every one it has seen, newest first */
    uint64_t retry_at, retry_ns; /* a device that did not answer: its next look (the examiner's) */
    /* An adapter that reaches only the first 4 GiB: every transfer through this buffer below it,
       one at a time under low_lock. NULL for every other. */
    uint8_t *low;
    uint64_t low_phys;
    struct mutex low_lock;

    /* Under lock, with the interrupt handler: the slots taken (a command being put together or in
       the port) and issued (in the port: PxCI, and PxSACT for a queued one, written under the lock,
       so reap never takes a slot as done before the port had it), of them the queued ones; all: the
       slots the disk uses; sole: one command, or a reset, has the port to itself; wanting: callers
       waiting for sole, before whom no queued command is let in; live: set up, commands may be
       given (cleared by a command that went wrong and by a sleep: the next caller sets the port up
       again first); need_reset: that set up must send a COMRESET; resetting: it is, and the link
       changes that shows are its own; halting: a caller is stopping the engines; attention: the
       port reported what it goes on through, and whoever waits must stop it; fatal: an error was
       reported of which no command is known to have ended well, until the engines stop; noted: the
       adapter's thread has this port's link change to see to; unverified: who is on the port is not
       known since a link change, an adapter reset or a sleep, and the adapter's thread alone may
       give it commands until it knows (port_examine). A caller waiting for room sleeps in room
       while freed still reads what it read before it looked. */
    struct spinlock lock;
    uint32_t taken, issued, queued, all;
    bool sole, live, need_reset, resetting, halting, attention, fatal, noted, unverified;
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
    bool s64, sncq, sss, ssc, sds, sadm;
    unsigned nslots;
    uint32_t implemented; /* PI */
    struct irq *irq; /* its message (pci_irq_enable), NULL: polled */
    struct port *ports[PORTS]; /* every implemented one, unless there was no memory for it */
    /* reset: one thread at a time takes the adapter through its setup or an HBA reset (not taken
       where nothing may sleep: there is no setup there). lost: its registers need setting up
       (after a sleep, after an HBA reset). dead: it does not recover, its bus mastering is off. */
    struct mutex reset;
    bool lost, dead;
    /* The adapter's thread: the ports whose links changed, whether to look at all of them (a
       sleep), a word that moves on before wq wakes. All atomic. */
    uint32_t changed;
    bool look_all;
    uint64_t kicks;
    struct wait_queue wq;
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
static bool wait_mmio(volatile uint8_t *reg, uint32_t mask, uint32_t want, uint64_t ns) {
    bool sleeps = may_sleep();
    uint64_t deadline = clock_after(ns);
    for (;;) {
        uint32_t v = mmio_read32(reg);
        if (v == 0xFFFFFFFFu) return false; /* the adapter is gone */
        if ((v & mask) == want) return true;
        if (clock_now_ns() >= deadline) return false;
        if (sleeps) sleep_until(clock_after(NS_PER_MS));
        else pause();
    }
}

static bool wait_reg(struct port *p, unsigned off, uint32_t mask, uint32_t want, uint64_t ns) {
    return wait_mmio(p->regs + off, mask, want, ns);
}

/* lock held: the issued commands in `which` end with status; returns the slots to wake. */
static uint32_t finish(struct port *p, uint32_t which, int status) {
    for (uint32_t m = which; m; m &= m - 1) {
        struct slot *s = &p->slots[__builtin_ctz(m)];
        s->status = status;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
    }
    p->issued &= ~which;
    p->queued &= ~which;
    return which;
}

static void wake_slots(struct port *p, uint32_t which) {
    for (; which; which &= which - 1) wq_wake_all(&p->slots[__builtin_ctz(which)].wq);
}

/* lock held, the engines not stopped by us (that clears PxCI and PxSACT): what the port's registers
   say of the commands in it. A non-queued command ends with the status in the task file, and the
   adapter's part with it. On a task file error, a queued command whose PxSACT bit is clear has
   ended well and the others have not, the one that failed and those the disk aborted with it
   (6.2.2.2; SATA 3.3 13.6.3.2): they are given again after the reset. A fault of the adapter's or
   the bus's says neither that its DMA ended nor which command's data it hit (6.1.1, 5.3.16.6), and
   an overflow asks for the recovery of a fatal error (6.1.5): from that report until the engines
   stop (halt), the registers end no command, and every one in the port fails and is given again.
   Such a report is looked for after PxSACT and PxCI are read, and not cleared: what they show
   ended before it. Returns the slots to wake. */
static uint32_t settle(struct port *p, uint32_t is) {
    if (p->fatal) return 0;
    uint32_t sact = rd(p, PxSACT), out = sact | rd(p, PxCI), tfd = rd(p, PxTFD);
    uint32_t done = p->issued & ~out;
    is |= rd(p, PxIS) & IS_FATAL;
    if (is == 0xFFFFFFFFu || is & IS_FATAL) { /* all ones: the adapter is gone */
        p->error_is = is;
        p->error_tfd = tfd;
        p->live = false;
        p->need_reset = true;
        p->attention = true;
        p->fatal = true;
        return p->issued; /* each waiter looks at attention */
    }
    if (!(is & IS_TFES) && !(done & ~p->queued && tfd & TFD_ERR)) return done ? finish(p, done, DONE) : 0;
    p->error_is = is;
    p->error_tfd = tfd;
    p->live = false;
    /* A queued command's error is cleared by a reset: its log page is the other way (6.2.2.2). */
    if (p->queued) p->need_reset = true;
    uint32_t woken = finish(p, p->issued & p->queued & ~sact, DONE);
    return woken | finish(p, p->issued, FAILED);
}

/* lock held: the port reported something for whoever waits to stop, and nobody waits any more: the
   next caller sets it up again. */
static void attention_over(struct port *p) {
    if (!p->attention || p->issued) return;
    p->attention = false;
    p->live = false;
    p->need_reset = true;
}

/* Port no's link change is for the adapter's thread to see to. */
static void hba_note(struct hba *h, unsigned no) {
    __atomic_or_fetch(&h->changed, 1u << no, __ATOMIC_RELEASE);
    __atomic_add_fetch(&h->kicks, 1, __ATOMIC_RELEASE);
    wq_wake_all(&h->wq);
}

/* The port's events. A link change (PCS, PRCS) stays in PxIS until PxSERR is cleared (3.3.5), which
   is the adapter's thread's to do: its interrupts stay closed until then. What the port goes on
   through is for a waiter to end by stopping it, in thread context. An error with no command in the
   port leaves it stopped until PxCMD.ST is cleared (6.2.2, 5.3.16.6): the next caller sets it up.
   The interrupt handler, and a waiter that polls. */
static void reap(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    uint32_t is = rd(p, PxIS), woken = 0;
    bool gone = is == 0xFFFFFFFFu, noted = false;
    if (is && !gone) wr(p, PxIS, is);
    if (!gone && is & IS_LINK && !p->resetting) {
        wr(p, PxIE, rd(p, PxIE) & ~IS_LINK);
        if (!p->noted) {
            p->noted = noted = true;
            p->unverified = true;
        }
    }
    if (p->issued && !p->halting) {
        woken = settle(p, is);
        if (!gone && p->issued && is & IS_LINK) {
            p->attention = true;
            woken |= p->issued;
        }
        attention_over(p);
    } else if (!gone && !p->halting && !p->resetting && is & (IS_ERRORS | IS_OFS)) {
        p->live = false;
        p->need_reset = true;
    }
    spin_unlock(&p->lock, f);
    wake_slots(p, woken);
    if (noted) hba_note(p->h, p->no);
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

/* The command list engine stopped: nothing in the port moves data after this; it clears PxCI and
   PxSACT. FIS receive goes on. */
static bool list_stop(struct port *p) {
    uint32_t cmd = rd(p, PxCMD);
    if (cmd & CMD_ST) wr(p, PxCMD, cmd & ~CMD_ST);
    return wait_reg(p, PxCMD, CMD_CR, 0, STOP_NS);
}

/* The port idle: its command list and FIS receive engines stopped (10.1.2), so its memory may
   change. */
static bool port_stop(struct port *p) {
    if (!list_stop(p)) return false;
    uint32_t cmd = rd(p, PxCMD);
    if (cmd & CMD_FRE) wr(p, PxCMD, cmd & ~CMD_FRE);
    return wait_reg(p, PxCMD, CMD_FR, 0, STOP_NS);
}

/* COMRESET (10.4.2): the link set up again and the device reset; it then sends its signature in a
   register FIS. Its power states are kept to active. DET 1h asks for SUD set (10.10.1), which an
   adapter of no staggered spin-up has always. */
static bool comreset(struct port *p) {
    wr(p, PxCMD, rd(p, PxCMD) | CMD_SUD);
    uint32_t sctl = (rd(p, PxSCTL) & ~0xF0Fu) | SCTL_NO_POWER_STATES(p->h);
    wr(p, PxSCTL, sctl | 1u);
    clock_spin(2 * NS_PER_MS); /* at least 1 ms, also where nothing may sleep */
    wr(p, PxSCTL, sctl);
    bool up = wait_reg(p, PxSSTS, 0xF, DET_UP, LINK_NS);
    wr(p, PxSERR, 0xFFFFFFFFu); /* coming up sets the link's diagnostics; PxTFD waits for DIAG.X (10.1.2) */
    return up;
}

/* Every command in the port failed, which no longer moves data: the caller knows. The adapter's reset
   took the link with it: who comes back is for the adapter's thread to look at. */
static void port_fail_all(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    p->live = false;
    p->need_reset = true;
    p->attention = false;
    p->unverified = true;
    uint32_t woken = finish(p, p->issued, FAILED);
    spin_unlock(&p->lock, f);
    wake_slots(p, woken);
}

static bool hba_lock(struct hba *h) {
    bool sleeps = may_sleep();
    if (sleeps) mutex_lock(&h->reset);
    return sleeps;
}

static void hba_unlock(struct hba *h, bool held) {
    if (held) mutex_unlock(&h->reset);
}

/* An adapter that does not recover: its bus mastering off ends its DMA, and it stays out of use,
   its disks with it. */
static void hba_kill(struct hba *h) {
    kprintf("ahci%d: the adapter is hung, turned off\n", h->index);
    __atomic_store_n(&h->dead, true, __ATOMIC_RELEASE);
    pci_disable_busmaster(h->pci, NULL);
    for (unsigned n = 0; n < PORTS; n++)
        for (struct disk *d = h->ports[n] ? __atomic_load_n(&h->ports[n]->disks, __ATOMIC_ACQUIRE) : NULL; d; d = d->older)
            __atomic_store_n(&d->blk.gone, true, __ATOMIC_RELEASE);
}

/* The adapter's thread looks at every port. */
static void hba_look_all(struct hba *h) {
    __atomic_store_n(&h->look_all, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&h->kicks, 1, __ATOMIC_RELEASE);
    wq_wake_all(&h->wq);
}

/* The last step (10.4.3): the whole adapter reset, which clears GHC.AE too, the first thing to set
   again (3.1.2); every port lost its commands, and is set up again before the next. PxCI and
   PxSACT read 0 from the reset on (10.4.3), which ends no command: the ports are marked first. h->reset
   held where a thread may sleep. */
static void hba_reset(struct hba *h) {
    kprintf("ahci%d: resetting the adapter\n", h->index);
    __atomic_store_n(&h->lost, true, __ATOMIC_RELEASE);
    for (unsigned n = 0; n < PORTS; n++) {
        struct port *p = h->ports[n];
        if (!p) continue;
        uint64_t f = spin_lock(&p->lock);
        p->fatal = true;
        p->live = false;
        spin_unlock(&p->lock, f);
    }
    hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE);
    hba_wr(h, GHC, hba_rd(h, GHC) | GHC_HR);
    if (wait_mmio(h->regs + GHC, GHC_HR, 0, HBA_RESET_NS)) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE);
    else hba_kill(h);
    for (unsigned n = 0; n < PORTS; n++)
        if (h->ports[n]) port_fail_all(h->ports[n]);
    hba_look_all(h);
}

/* The port's DMA ended by the first of 10.4's steps that works, when its engines do not stop by
   themselves: a COMRESET (10.4.2), then the adapter's reset (10.4.3). h->reset held. */
static void port_force_stop(struct port *p) {
    if (__atomic_load_n(&p->h->dead, __ATOMIC_ACQUIRE)) return;
    kprintf("ahci%d: port %u does not stop\n", p->h->index, p->no);
    comreset(p);
    if (!port_stop(p)) hba_reset(p->h);
}

/* Ready for events: the link's diagnostics and the port's events clear (PCS and PRCS go with
   PxSERR), PxIS first and IS second (10.1.2, 10.7.2.1), then its interrupts. */
static void port_listen(struct port *p) {
    wr(p, PxSERR, 0xFFFFFFFFu);
    wr(p, PxIS, 0xFFFFFFFFu);
    hba_wr(p->h, IS, 1u << p->no);
    wr(p, PxIE, p->h->irq ? IS_ALL : 0);
    uint64_t f = spin_lock(&p->lock);
    p->resetting = false;
    spin_unlock(&p->lock, f);
}

static void port_give_memory(struct port *p) {
    wr(p, PxCLB, (uint32_t)(p->mem_phys + LIST_AT));
    wr(p, PxCLB + 4, (uint32_t)((p->mem_phys + LIST_AT) >> 32));
    wr(p, PxFB, (uint32_t)(p->mem_phys + FIS_AT));
    wr(p, PxFB + 4, (uint32_t)((p->mem_phys + FIS_AT) >> 32));
    wr(p, PxCMD, rd(p, PxCMD) | CMD_FRE);
}

/* lock not held: the next set up of the port sends a COMRESET. */
static void port_want_reset(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    p->need_reset = true;
    spin_unlock(&p->lock, f);
}

/* The link may have changed while port_up's own COMRESET hid it (7.3, 7.4), or does not come back:
   unless the adapter's thread is already finding out who is there, it is asked to. */
static void port_note(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    bool asked = p->unverified;
    p->unverified = true;
    bool noted = !asked && !p->noted;
    if (noted) p->noted = true;
    spin_unlock(&p->lock, f);
    if (noted) hba_note(p->h, p->no);
}

/* port_up could not bring the device up. */
static bool port_up_failed(struct port *p) {
    port_want_reset(p);
    port_listen(p);
    port_note(p);
    return false;
}

/* PxCMD.ST set, which needs a working device (10.1.2). After a non-queued command's error the
   engine is stopped alone (6.2.2.1, 10.4.1); a COMRESET is for a device that stays busy or has no
   link, a queued command's error, and the start. h->reset held where a thread may sleep, the port
   to the caller (sole), no command in it. A reset asked for meanwhile (a link change) is left asked
   for, the port not live. False leaves it not live. */
static bool port_up(struct port *p) {
    struct hba *h = p->h;
    uint64_t f = spin_lock(&p->lock);
    p->resetting = true;
    p->attention = false;
    p->fatal = false;
    bool reset = p->need_reset;
    p->need_reset = false;
    spin_unlock(&p->lock, f);
    wr(p, PxIE, 0);
    if (!(reset ? port_stop(p) : list_stop(p))) {
        port_force_stop(p);
        reset = true;
        if (__atomic_load_n(&h->lost, __ATOMIC_ACQUIRE)) { /* the next try sets the adapter up */
            port_want_reset(p);
            return false;
        }
    }
    if (reset) {
        if (!(rd(p, PxCMD) & CMD_FR)) port_give_memory(p);
    } else {
        wr(p, PxSERR, 0xFFFFFFFFu);
        wr(p, PxIS, 0xFFFFFFFFu);
        if (rd(p, PxTFD) & (TFD_BSY | TFD_DRQ) || (rd(p, PxSSTS) & 0xF) != DET_UP) reset = true;
    }
    wr(p, PxCMD, rd(p, PxCMD) | CMD_SUD | CMD_POD | CMD_FRE);
    if (reset && !comreset(p)) return port_up_failed(p);
    if (!wait_reg(p, PxTFD, TFD_BSY | TFD_DRQ, 0, READY_NS)) {
        kprintf("ahci%d: port %u: the disk does not become ready (status %x)\n", h->index, p->no, rd(p, PxTFD));
        return port_up_failed(p);
    }
    bool changed = rd(p, PxSERR) & SERR_LINK; /* since its diagnostics were cleared */
    port_listen(p);
    if (changed && !__atomic_load_n(&p->unverified, __ATOMIC_ACQUIRE)) { /* no command for whoever that is */
        port_note(p);
        return false;
    }
    wr(p, PxCMD, rd(p, PxCMD) | CMD_ST);
    f = spin_lock(&p->lock);
    bool live = p->live = !p->need_reset;
    spin_unlock(&p->lock, f);
    return live;
}

/* The firmware gives the adapter up (10.6.3), and the adapter's registers are the AHCI ones (3.1.2:
   before anything else is read or written). The firmware clears BB before BOS (10.6.4): BOS falling
   ends the wait, whether it was busy or not. If it never does, the adapter is taken anyway. */
static void hba_take(struct hba *h) {
    hba_wr(h, GHC, hba_rd(h, GHC) | GHC_AE);
    if (!(hba_rd(h, CAP2) & CAP2_BOH)) return;
    hba_wr(h, BOHC, (hba_rd(h, BOHC) & ~BOHC_OOC) | BOHC_OOS);
    bool busy = false;
    uint64_t deadline = clock_after(OWNER_NS);
    for (;;) {
        uint32_t bohc = hba_rd(h, BOHC);
        if (!(bohc & BOHC_BOS)) return;
        if (bohc & BOHC_BB && !busy) {
            busy = true;
            deadline = clock_after(OWNER_BUSY_NS);
        }
        if (clock_now_ns() >= deadline) break;
        if (may_sleep()) sleep_until(clock_after(NS_PER_MS));
        else pause();
    }
    kprintf("ahci%d: the firmware does not give the adapter up, taken anyway\n", h->index);
}

/* A staggered spin-up (10.1.1 steps 4-7, 10.10.2): one port's disk at a time, each brought to ready
   before the next starts, so they do not all draw their spin-up current at once. A port where
   nothing answers stays spun up, listening: a device that comes later is seen by PCS. */
static void port_spin_up(struct port *p) {
    uint32_t cmd = rd(p, PxCMD);
    if (cmd & CMD_SUD) return;
    wr(p, PxCMD, cmd | CMD_SUD | CMD_POD);
    if (!wait_reg(p, PxSSTS, DET_FOUND, DET_FOUND, LINK_NS)) return;
    wr(p, PxSERR, 0xFFFFFFFFu);
    if (!wait_reg(p, PxTFD, TFD_BSY | TFD_DRQ | TFD_ERR, 0, READY_NS))
        kprintf("ahci%d: port %u: the disk does not become ready as it spins up (status %x)\n", p->h->index, p->no,
                rd(p, PxTFD));
}

/* The adapter from nothing to minimally set up (10.1.2), every implemented port in it: idle, its
   memory given, FIS receive on, nothing pending, its interrupts on; a disk on it spun up. Before
   its first use, after a sleep (10.6: the firmware may own it again), after an HBA reset. h->reset
   held where a thread may sleep; nothing is in any port. */
static bool hba_set_up(struct hba *h, bool after_reset) {
    hba_take(h);
    hba_wr(h, GHC, hba_rd(h, GHC) & ~GHC_IE);
    for (unsigned n = 0; n < PORTS; n++) {
        struct port *p = h->ports[n];
        if (!p) continue;
        wr(p, PxIE, 0);
        if (!port_stop(p)) {
            if (after_reset) { /* not even the adapter's reset stopped it */
                hba_kill(h);
                return false;
            }
            port_force_stop(p);
            if (__atomic_load_n(&h->dead, __ATOMIC_ACQUIRE)) return false;
            if (__atomic_exchange_n(&h->lost, false, __ATOMIC_ACQ_REL)) return hba_set_up(h, true);
        }
        wr(p, PxCMD, rd(p, PxCMD) & ~(CMD_ALPE | CMD_ASP));
        wr(p, PxSCTL, (rd(p, PxSCTL) & ~0xF0Fu) | SCTL_NO_POWER_STATES(h));
        if (h->sds && h->sadm) wr(p, PxDEVSLP, rd(p, PxDEVSLP) & ~DEVSLP_ADSE);
        port_give_memory(p);
        wr(p, PxSERR, 0xFFFFFFFFu);
        uint64_t f = spin_lock(&p->lock);
        p->live = false;
        p->need_reset = true;
        p->resetting = true;
        spin_unlock(&p->lock, f);
    }
    if (h->sss)
        for (unsigned n = 0; n < PORTS; n++)
            if (h->ports[n]) port_spin_up(h->ports[n]);
    for (unsigned n = 0; n < PORTS; n++)
        if (h->ports[n]) port_listen(h->ports[n]);
    if (h->irq) hba_wr(h, GHC, hba_rd(h, GHC) | GHC_IE);
    return true;
}

/* The port running with its disk ready for commands (10.1.2, 10.3.1): the adapter set up if it
   needs that, then the port. Where nothing may sleep (a power change, the disks held) it spins, and
   no other thread is at the adapter. */
static bool port_start(struct port *p) {
    struct hba *h = p->h;
    bool held = hba_lock(h);
    bool ok = !__atomic_load_n(&h->dead, __ATOMIC_ACQUIRE);
    if (ok && __atomic_exchange_n(&h->lost, false, __ATOMIC_ACQ_REL)) ok = hba_set_up(h, false);
    ok = ok && port_up(p);
    hba_unlock(h, held);
    return ok;
}

/* Wait for leave to free room: it wakes the waiter; in case a wake is lost, the waiter looks again
   after 10 ms (polled: each ms). With interrupts off it spins. */
static void wait_room(struct port *p, uint64_t seen) {
    if (!may_sleep()) pause();
    else wq_wait_unless(&p->room, &p->freed, seen, clock_after(p->h->irq ? 10 * NS_PER_MS : NS_PER_MS));
}

static void port_changed(struct port *p);

/* A slot of the live port for a command: a queued one (alone false) beside the other queued ones,
   or the port to itself (no command in it, no other let in). A port that is not live is set up
   first, by the caller that finds it so once it is empty. -1 when it cannot be. While who is on the
   port is not known a caller waits for the adapter's thread, which ends each look in its time. When
   that look found nothing to go by, or a power change has stopped the thread, it waits as long as
   a command may take, and then it is -2. The thread that holds the disks finds out itself, the
   adapter's thread waiting for it, and where nothing may sleep it is -2 at once. examining: the one
   finding out. */
static int enter(struct port *p, bool alone, bool examining) {
    bool wanted = false, looked = false;
    uint64_t deadline = 0;
    for (;;) {
        uint64_t seen = __atomic_load_n(&p->freed, __ATOMIC_ACQUIRE);
        uint64_t f = spin_lock(&p->lock);
        if (p->unverified && !examining) {
            if (wanted) p->wanting--;
            wanted = false;
            spin_unlock(&p->lock, f);
            if (!may_sleep()) return -2;
            struct thread *holder = blk_holder();
            if (holder == thread_current()) {
                if (looked) return -2;
                looked = true;
                port_changed(p);
                continue;
            }
            uint64_t now = clock_now_ns();
            if (!deadline && (holder || __atomic_load_n(&p->retry_ns, __ATOMIC_ACQUIRE))) deadline = now + IO_NS;
            if (deadline && now >= deadline) return -2;
            wait_room(p, seen);
            continue;
        }
        if (!p->sole) {
            if (wanted && !alone && p->live) { /* set up by another meanwhile: this one wants the port no more */
                p->wanting--;
                wanted = false;
            }
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

/* A command of the caller's (`bit`) ran out of time, or the port went on through something it should
   not: the engines stopped, so nothing in the port moves data into memory its callers are about to
   give back, and every command still in it failed. What had ended before the engines stopped ended
   well: the stop clears PxCI and PxSACT, so they are read first (5.3.2.10, 6.2.2.2), and reap reads
   them no more until it is over. The first caller to find it so does it; the others wait for
   their own commands to fail. */
static void halt(struct port *p, uint32_t bit) {
    uint64_t f = spin_lock(&p->lock);
    uint32_t woken = p->halting ? 0 : settle(p, 0);
    if (p->halting || !(p->issued & bit)) {
        attention_over(p);
        spin_unlock(&p->lock, f);
        wake_slots(p, woken);
        return;
    }
    p->halting = true;
    p->live = false;
    p->need_reset = true;
    if (!p->attention) {
        p->error_is = 0;
        p->error_tfd = rd(p, PxTFD);
    }
    p->attention = false;
    spin_unlock(&p->lock, f);
    wake_slots(p, woken);
    /* after an adapter reset its engines are stopped, and what it set up is not this one's to stop */
    if (!__atomic_load_n(&p->h->lost, __ATOMIC_ACQUIRE) && !port_stop(p)) {
        bool held = hba_lock(p->h);
        port_force_stop(p);
        hba_unlock(p->h, held);
    }
    f = spin_lock(&p->lock);
    woken = finish(p, p->issued, FAILED);
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

/* One command with `bytes` at buf, given in the slot the caller entered with, and its end waited
   for. 0; -1 when the disk reported an error, the adapter one of its own or the time ran out; -2
   when buf cannot be described to the adapter. */
static int command_in(struct port *p, int slot, const uint8_t fis[20], bool queued, bool write, const uint8_t *buf,
                      unsigned bytes, uint64_t ns) {
    uint32_t bit = 1u << slot;
    uint64_t table_phys = p->mem_phys + TABLES_AT + (uint64_t)slot * TABLE_BYTES;
    uint8_t *table = p->mem + TABLES_AT + (size_t)slot * TABLE_BYTES;
    memset(table, 0, PRDT_AT);
    memcpy(table, fis, 20);
    if (queued) table[12] = (uint8_t)(slot << 3); /* the tag is the slot (3.3.13) */
    unsigned n = bytes ? prds(p, table, buf, bytes) : 0;
    if (bytes && !n) {
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
        if (queued) p->queued |= bit;
        __atomic_thread_fence(__ATOMIC_RELEASE); /* the table and header before the port sees them */
        if (queued) wr(p, PxSACT, bit); /* before PxCI (3.3.13) */
        wr(p, PxCI, bit);
    }
    spin_unlock(&p->lock, f);
    if (!live) return -1; /* an error since enter: the next caller sets it up again */

    bool sleeps = may_sleep(), timed_out = false, looked = false;
    uint64_t deadline = clock_after(ns);
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seen != start) break;
        if (!p->h->irq || !sleeps || looked) reap(p);
        looked = false;
        if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != start) break;
        if (__atomic_load_n(&p->attention, __ATOMIC_ACQUIRE)) {
            halt(p, bit);
            continue;
        }
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
    if (status == DONE) return 0;
    if (!timed_out)
        kprintf("ahci%d: port %u: command %02x failed (status %x, error %x, events %x)\n", p->h->index, p->no, fis[2],
                tfd & 0xFF, tfd >> 8 & 0xFF, is);
    return -1;
}

/* A command for the disk: queued beside the others, or alone with the port to itself (a queued one
   alone: see transfer). As command_in; also -2 when the disk is not there, or not known to be. */
static int command(struct disk *d, const uint8_t fis[20], bool queued, bool alone, bool write, const uint8_t *buf,
                   unsigned bytes, uint64_t ns) {
    struct port *p = d->p;
    if (__atomic_load_n(&d->blk.gone, __ATOMIC_ACQUIRE)) return -2;
    int slot = enter(p, alone, false);
    if (slot < 0) return slot;
    /* with the slot held the adapter's thread cannot be looking at another disk there */
    int r = __atomic_load_n(&d->blk.gone, __ATOMIC_ACQUIRE)
                ? -2
                : command_in(p, slot, fis, queued, write, buf, bytes, ns);
    leave(p, slot, alone);
    return r;
}

/* Read or write `n` logical blocks between the disk and buf (blk_by_blocks). When a queued command
   fails, the disk may have aborted it for another's error, and which one failed would have to be
   read from a log page: so it is given again alone, where a failure is its own, up to three times. */
static int transfer(void *ctx, bool write, uint64_t lba, unsigned n, uint8_t *buf) {
    struct disk *d = ctx;
    uint8_t fis[20];
    uint8_t op = d->ncq     ? (write ? ATA_WRITE_FPDMA_QUEUED : ATA_READ_FPDMA_QUEUED)
                 : d->lba48 ? (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT)
                            : (write ? ATA_WRITE_DMA : ATA_READ_DMA);
    fis_make(fis, op, lba, n, d->lba48, d->ncq);
    bool alone = !d->ncq;
    for (int failed_alone = 0; failed_alone < 3;) {
        int r = command(d, fis, d->ncq, alone, write, buf, n * d->block, IO_NS);
        if (r != -1) return r ? -1 : 0;
        if (alone) failed_alone++;
        alone = true;
    }
    return -1;
}

static int flush(struct disk *d) {
    uint8_t fis[20];
    fis_make(fis, d->lba48 ? ATA_FLUSH_CACHE_EXT : ATA_FLUSH_CACHE, 0, 0, false, false);
    return command(d, fis, false, true, false, NULL, 0, IO_NS);
}

/* 512-byte sectors on top of the disk's logical blocks: the caller's memory itself where the
   request covers whole blocks of word aligned memory (a PRD's address is), else a bounce buffer of
   its own; for an adapter that reaches only 4 GiB the port's buffer below it. */
static int rw(struct disk *d, bool write, uint64_t lba, uint32_t count, uint8_t *buf) {
    struct port *p = d->p;
    if (p->low) {
        mutex_lock(&p->low_lock);
        int r = blk_by_blocks(&d->blk, write, lba, count, buf, p->low, d->most, false, transfer, d);
        mutex_unlock(&p->low_lock);
        return r;
    }
    unsigned per = d->block / 512, most = d->most;
    bool direct = !((uint64_t)buf & 1), whole = !(lba % per) && !(count % per);
    uint8_t *bounce = NULL;
    if (!direct || !whole) {
        size_t bytes = (size_t)MIN((uint64_t)most, (count + 2ull * per - 1) / per) * d->block;
        if (!(bounce = kmalloc(bytes))) return -N_ERR_NO_MEMORY;
        most = (unsigned)(bytes / d->block);
    }
    int r = blk_by_blocks(&d->blk, write, lba, count, buf, bounce, most, direct, transfer, d);
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

/* IDENTIFY DEVICE (ACS-3 7.12): its size, sector size, commands, queue and write cache, in d. The
   caller has the port to itself, slot its own. *again: it did not answer, or there was no memory,
   and it may yet be a disk. */
static bool identify(struct port *p, int slot, struct disk *d, char model[41], bool *again) {
    uint64_t page = alloc_dma(p->h, 1);
    if (!page) {
        *again = true;
        return false;
    }
    const uint16_t *id = phys_to_virt(page);
    uint8_t fis[20];
    fis_make(fis, ATA_IDENTIFY, 0, 0, false, false);
    fis[7] = 0;
    memset((void *)id, 0, 512);
    if (command_in(p, slot, fis, false, false, (const uint8_t *)id, 512, IO_NS)) {
        pmm_free(page);
        *again = true;
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
    memcpy(d->serial, &id[10], sizeof d->serial);
    bool valid83 = (id[83] & 0xC000) == 0x4000, valid106 = (id[106] & 0xC000) == 0x4000;
    d->lba48 = valid83 && (id[83] >> 10 & 1);
    uint64_t sectors = d->lba48 ? id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
                                : id[60] | (uint64_t)id[61] << 16;
    d->block = valid106 && (id[106] >> 12 & 1) ? 2 * (id[117] | (unsigned)id[118] << 16) : 512;
    d->cache = (id[87] & 0xC000) == 0x4000 && (id[85] >> 5 & 1);
    /* queued commands need the adapter's and the disk's word (76 bit 8), and 48-bit addresses */
    d->ncq = p->h->sncq && d->lba48 && id[76] != 0xFFFF && (id[76] >> 8 & 1);
    unsigned depth = d->ncq ? MIN(p->h->nslots, (id[75] & 0x1Fu) + 1) : 1;
    d->all = depth == 32 ? 0xFFFFFFFFu : (1u << depth) - 1;
    pmm_free(page);
    if (d->block < 512 || d->block > 4096 || (d->block & (d->block - 1)) || !sectors) {
        kprintf("ahci%d: port %u: %s has %u-byte sectors, skipped\n", p->h->index, p->no, model, d->block);
        return false;
    }
    d->most = (d->lba48 ? XFER_PAGES * PAGE_SIZE : MIN(XFER_PAGES * PAGE_SIZE, 256 * d->block)) / d->block;
    d->blk.sectors = sectors * (d->block / 512);
    return true;
}

static void port_add(struct hba *h, unsigned no) {
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
    h->ports[no] = p;
}

static bool same_disk(const struct disk *a, const struct disk *b) {
    return !memcmp(a->serial, b->serial, sizeof a->serial) && a->blk.sectors == b->blk.sectors && a->block == b->block;
}

/* Who is on the port now: reset, and asked. A disk it knew goes on if it is the one there; the one it
   had, if another or nothing is there, is gone until it comes back. A device that is there and does
   not answer yet says nothing of who it is: what the port had stays, its callers kept waiting
   (unverified), and the adapter's thread looks again (retry_at), as it does after no memory. One
   caller at a time: the start, then the adapter's thread. */
static void port_examine(struct port *p) {
    struct hba *h = p->h;
    struct disk *had = p->disk, *known = NULL, *found = kzalloc(sizeof *found);
    char model[41];
    bool is_disk = false, again = false;
    int slot = -1;
    uint64_t f = spin_lock(&p->lock);
    p->unverified = true;
    spin_unlock(&p->lock, f);
    if (!found) {
        kprintf("ahci%d: port %u: no memory to look at its disk\n", h->index, p->no);
        again = true;
    } else if ((slot = enter(p, true, true)) < 0) {
        /* no device is nothing there; a device and no ready disk, or an adapter being reset, is not known */
        again = !__atomic_load_n(&h->dead, __ATOMIC_ACQUIRE) &&
                (__atomic_load_n(&h->lost, __ATOMIC_ACQUIRE) || rd(p, PxSSTS) & DET_FOUND);
    } else if ((rd(p, PxSSTS) & 0xF) != DET_UP) {
        again = rd(p, PxSSTS) & DET_FOUND;
    } else {
        uint32_t sig = rd(p, PxSIG);
        if (sig == SIG_ATA) is_disk = identify(p, slot, found, model, &again);
        else if (!sig || sig == 0xFFFFFFFFu) again = true; /* no signature: its first FIS is to come (3.3.9) */
        else kprintf("ahci%d: port %u: %s, skipped\n", h->index, p->no,
                     sig == 0xEB140101u ? "an ATAPI drive" : sig == 0x96690101u ? "a port multiplier" : "not a disk");
    }
    if (is_disk && !h->s64 && !p->low) {
        if ((p->low_phys = alloc_dma(h, XFER_PAGES))) {
            p->low = phys_to_virt(p->low_phys);
        } else {
            kprintf("ahci%d: port %u: no memory below 4 GiB for its transfers\n", h->index, p->no);
            is_disk = false;
            again = true;
        }
    }
    if (again) {
        uint64_t wait = p->retry_ns ? MIN(2 * p->retry_ns, (uint64_t)RETRY_MOST_NS) : RETRY_NS;
        __atomic_store_n(&p->retry_ns, wait, __ATOMIC_RELEASE);
        p->retry_at = clock_after(wait);
    } else {
        __atomic_store_n(&p->retry_ns, 0, __ATOMIC_RELEASE);
        p->retry_at = 0;
        for (struct disk *d = p->disks; is_disk && d && !known; d = d->older)
            if (same_disk(d, found)) known = d;
        if (had && had != known && !__atomic_exchange_n(&had->blk.gone, true, __ATOMIC_ACQ_REL))
            kprintf("ahci%d: port %u: %s is gone\n", h->index, p->no, had->blk.name);
        if (known) {
            if (__atomic_exchange_n(&known->blk.gone, false, __ATOMIC_ACQ_REL))
                kprintf("ahci%d: port %u: %s is back\n", h->index, p->no, known->blk.name);
            blk_cache_lost(&known->blk); /* it says nothing of whether the power went with the link (7.3) */
            p->disk = known;
        } else if (is_disk) {
            found->p = p;
            found->older = p->disks;
            __atomic_store_n(&p->disks, found, __ATOMIC_RELEASE);
            p->disk = found;
        }
    }
    if (slot >= 0 && !is_disk) list_stop(p); /* no command is coming: PxCMD.ST only for a disk in use (10.1.2) */
    f = spin_lock(&p->lock);
    if (slot >= 0) {
        if (!is_disk) p->live = false;
        if (again) p->need_reset = true;
        if (!again) p->all = known ? known->all : is_disk ? found->all : 1;
    }
    if (!again && !p->noted) p->unverified = false; /* a link change since is to be looked at too */
    spin_unlock(&p->lock, f);
    if (slot >= 0) {
        leave(p, slot, true);
    } else {
        __atomic_add_fetch(&p->freed, 1, __ATOMIC_RELEASE);
        wq_wake_all(&p->room);
    }
    if (known || !is_disk) {
        kfree(found);
        return;
    }
    ksnprintf(found->blk.name, sizeof found->blk.name, "sata%d", __atomic_fetch_add(&ndisks, 1, __ATOMIC_RELAXED));
    strlcpy(found->blk.model, model, sizeof found->blk.model);
    found->blk.block = found->block;
    found->blk.read = blk_ahci_read;
    found->blk.write = blk_ahci_write;
    if (found->cache) found->blk.persist = blk_ahci_persist;
    found->blk.priv = found;
    if (found->ncq)
        kprintf("ahci%d: port %u: %s, %u queued commands at once\n", h->index, p->no, model,
                (unsigned)__builtin_popcount(found->all));
    else
        kprintf("ahci%d: port %u: %s, one command at a time\n", h->index, p->no, model);
    blk_register(&found->blk);
}

/* The port's link changed, or the adapter slept: its commands stopped, a COMRESET whichever way it
   came (6.2.2.3), its events cleared with PxSERR, and then whoever is there looked at. */
static void port_changed(struct port *p) {
    uint64_t f = spin_lock(&p->lock);
    p->live = false;
    p->need_reset = true;
    p->noted = false;
    p->unverified = true;
    spin_unlock(&p->lock, f);
    halt(p, 0xFFFFFFFFu);
    port_examine(p);
}

/* The adapter's thread. What it does counts as a request of the disks (blk_busy_begin): a power
   change waits for it, and it waits for that. It looks again at the ports whose devices did not
   answer, once their time comes. Polled, it looks at the links itself each second. */
static void ahci_worker(void *arg) {
    struct hba *h = arg;
    for (;;) {
        uint64_t seen = __atomic_load_n(&h->kicks, __ATOMIC_ACQUIRE);
        uint64_t now = clock_now_ns(), next = h->irq ? CLOCK_NEVER : now + NS_PER_S;
        uint32_t changed = __atomic_exchange_n(&h->changed, 0, __ATOMIC_ACQ_REL);
        if (__atomic_exchange_n(&h->look_all, false, __ATOMIC_ACQ_REL)) changed = h->implemented;
        for (unsigned n = 0; n < PORTS; n++) {
            struct port *p = h->ports[n];
            if (!p) continue;
            if (!h->irq) reap(p);
            if (!p->retry_at) continue;
            if (now >= p->retry_at) changed |= 1u << n;
            else next = MIN(next, p->retry_at);
        }
        changed &= h->implemented;
        if (!changed) {
            wq_wait_unless(&h->wq, &h->kicks, seen, next);
            continue;
        }
        blk_busy_begin();
        for (uint32_t m = changed; m; m &= m - 1)
            if (h->ports[__builtin_ctz(m)]) port_changed(h->ports[__builtin_ctz(m)]);
        blk_busy_end();
    }
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
    uint32_t cap = hba_rd(h, CAP);
    h->implemented = hba_rd(h, PI);
    h->s64 = cap & CAP_S64A;
    h->sncq = cap & CAP_SNCQ;
    h->sss = cap & CAP_SSS;
    h->ssc = cap & CAP_SSC;
    h->sds = hba_rd(h, CAP2) & CAP2_SDS;
    h->sadm = hba_rd(h, CAP2) & CAP2_SADM;
    h->nslots = (cap >> 8 & 0x1F) + 1;
    for (uint32_t m = h->implemented; m; m &= m - 1) port_add(h, (unsigned)__builtin_ctz(m));
    hba_wr(h, GHC, hba_rd(h, GHC) & ~GHC_IE); /* until PxIS and IS are clear */
    pci_irq_enable(pci, &(struct irq_spec){.name = "AHCI", .handler = ahci_isr, .context = h, .node = pci->node}, 1, &h->irq);
    kprintf("ahci%d: %04x:%04x, version %x, ports %x, %u slots%s, %s\n", h->index, pci->vendor, pci->device,
            hba_rd(h, VS), h->implemented, h->nslots, h->sncq ? ", queued commands" : "", h->irq ? "MSI" : "polled");
    mutex_lock(&h->reset);
    bool up = hba_set_up(h, false);
    mutex_unlock(&h->reset);
    if (!up) return;
    for (unsigned n = 0; n < PORTS; n++) {
        struct port *p = h->ports[n];
        if (p && rd(p, PxSSTS) & 0xF) port_examine(p); /* 0: nothing there yet */
    }
    char name[16];
    ksnprintf(name, sizeof name, "ahci%d", h->index);
    if (!kthread_create(name, ahci_worker, h)) { /* nothing would know who is on a port after a link change */
        kprintf("ahci%d: no thread for its link changes\n", h->index);
        hba_kill(h);
    }
}

/* On a board that spins its disks up in turn, before the power goes away (10.10.3): a disk told to
   stand by has its interface put in Slumber (step 2), by an adapter that may (3.1.1 SSC), with the
   engine still running, which is where it takes the request (5.3.2.11, 5.3.4.2). */
static void port_to_slumber(struct port *p) {
    if (!p->h->sss || !p->h->ssc || !__atomic_load_n(&p->live, __ATOMIC_ACQUIRE)) return;
    if ((rd(p, PxSSTS) & IPM_MASK) != IPM_ACTIVE) return;
    wr(p, PxSCTL, (rd(p, PxSCTL) & ~0xF0Fu) | SCTL_SLUMBER_ALONE(p->h));
    wr(p, PxCMD, (rd(p, PxCMD) & ~CMD_ICC) | CMD_ICC_SLUMBER);
    wait_reg(p, PxSSTS, IPM_MASK, IPM_SLUMBER, LINK_NS);
}

/* Then listen mode, for the disks to spin up in turn again when it comes back (step 3): on a port in
   Slumber or with no device, since SUD is cleared only there (10.10.1), and not one with a presence
   switch or cold presence detect (10.10.4). The engines are stopped. */
static void port_to_listen(struct port *p) {
    if (!p->h->sss || rd(p, PxCMD) & (CMD_MPSP | CMD_CPD)) return;
    uint32_t ssts = rd(p, PxSSTS);
    if ((ssts & IPM_MASK) != IPM_SLUMBER && ssts & 0xF) return;
    wr(p, PxSCTL, rd(p, PxSCTL) & ~0xFu);
    wr(p, PxCMD, rd(p, PxCMD) & ~CMD_SUD);
}

/* Before the power goes away: the cache on the medium and the disk spun down (STANDBY IMMEDIATE),
   so a hard disk parks its heads and does not count an emergency stop; a port an error left down is
   set up for that first, spinning; then the engines stopped, of every port (they all receive FISes),
   no DMA after this. Interrupts are already off, and blk_hold let no request in. A disk that is gone
   and a dead adapter hold nothing that could be kept. An adapter reset on the way leaves every port
   unverified, so no later port is set up again, or given a command. */
static bool ahci_quiesce(void *arg) {
    (void)arg;
    bool all = true;
    for (struct hba *h = hbas; h; h = h->next)
        for (unsigned n = 0; n < PORTS; n++) {
            struct port *p = h->ports[n];
            if (!p || __atomic_load_n(&h->dead, __ATOMIC_ACQUIRE)) continue;
            struct disk *d = p->disk;
            if (d && !__atomic_load_n(&d->blk.gone, __ATOMIC_ACQUIRE)) {
                uint8_t fis[20];
                fis_make(fis, ATA_STANDBY_IMMEDIATE, 0, 0, false, false);
                fis[7] = 0;
                if ((d->cache && flush(d)) || command(d, fis, false, true, false, NULL, 0, READY_NS)) all = false;
                else port_to_slumber(p);
            }
            if (port_stop(p)) {
                port_to_listen(p);
            } else {
                port_force_stop(p);
                all = false;
            }
            uint64_t f = spin_lock(&p->lock);
            p->live = false;
            p->need_reset = true;
            spin_unlock(&p->lock, f);
        }
    return all;
}

/* After a sleep the adapter has lost its state, the firmware may own it again, and the disks may have
   changed. All of that takes waiting, and resume must not wait: the next command, or the adapter's
   thread, sets it up again (hba_set_up in port_start). */
static bool ahci_resume(void *arg) {
    (void)arg;
    for (struct hba *h = hbas; h; h = h->next) {
        if (!h->regs) continue;
        __atomic_store_n(&h->lost, true, __ATOMIC_RELEASE);
        for (unsigned n = 0; n < PORTS; n++) {
            struct port *p = h->ports[n];
            if (!p) continue;
            uint64_t f = spin_lock(&p->lock);
            p->unverified = true;
            spin_unlock(&p->lock, f);
        }
        hba_look_all(h);
    }
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
