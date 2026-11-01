/* NVM Express SSDs (NVM Express Base Specification 2.x, PCIe transport).

   Queues: one admin queue pair, and I/O queue pairs as many as the controller gives up to one a
   CPU. Each CPU submits to its queue (the CPUs split over the queues in their order, which keeps
   the CPUs of a cache and a node together), and each queue's completion queue interrupts on a
   CPU it serves, through a message of its own where the function has as many (MSI-X), else
   sharing the messages it has. A queue holds as many commands at once as its entries but one;
   every command waits in a slot of its own, woken by the interrupt that completes it.

   Data moves between the device and the caller's memory itself, described page by page in PRPs
   (the kernel's own functions pass the IOMMU untranslated, dev/iommu.h, so a bus address is the
   physical one): a bounce buffer only for blocks a request covers in part, or memory that is not
   dword aligned. Every active namespace with plain 512..4096-byte blocks (no metadata, no
   protection information) becomes a block device "nvmeN". Completions are polled when the
   platform offers no message interrupt. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "dev/blk.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define ADMIN_ENTRIES 32
#define IO_ENTRIES 64             /* an I/O queue's entries: its submission queue fills one page */
#define PRPS_PER_PAGE 512         /* one PRP list page: a command of up to 2 MiB */
#define ADMIN_NS (5 * NS_PER_S)
#define IO_NS (30 * NS_PER_S)

/* controller registers */
#define CAP 0x00
#define VS 0x08
#define CC 0x14
#define CSTS 0x1C
#define AQA 0x24
#define ASQ 0x28
#define ACQ 0x30
#define CRTO 0x68
#define CSTS_RDY 1u
#define CSTS_CFS 2u

/* admin and NVM command opcodes */
#define OP_CREATE_SQ 0x01
#define OP_CREATE_CQ 0x05
#define OP_IDENTIFY 0x06
#define OP_SET_FEATURES 0x09
#define OP_FLUSH 0x00
#define OP_WRITE 0x01
#define OP_READ 0x02
#define FEATURE_QUEUES 0x07

#define FAILED (-1) /* timeout or a dead controller; command errors return the status field */

/* A command's place in its queue: its command identifier is the slot's index. */
struct slot {
    struct wait_queue wq; /* its submitter, woken when seq moves on */
    uint64_t seq;         /* moves on when its command completes, or a reset fails it */
    int status;           /* 0, the 15-bit status field, or FAILED: written before seq moves on */
    uint32_t result;      /* the completion's DW0 */
    bool busy;            /* submitted and not completed: a reset fails it */
};

struct nvme;
struct queue {
    struct nvme *c;
    uint16_t qid, size, depth; /* depth: the slots, size - 1 (so the submission queue never fills) */
    uint8_t *sq;
    uint64_t sq_phys;
    volatile uint32_t *cq;
    uint64_t cq_phys;
    /* The submission side: the tail, the free slots, taking a slot. */
    struct spinlock sq_lock;
    uint16_t tail;
    uint64_t *free;          /* a bit a free slot */
    uint64_t freed;          /* moves on as a slot is given back (room's word) */
    struct wait_queue room;  /* submitters waiting for a free slot */
    /* The completion side, taken in the interrupt handler too: the head and phase. */
    struct spinlock cq_lock;
    uint16_t head;
    uint8_t phase;
    struct slot *slots;
    unsigned node; /* of the CPUs it serves: where its memory comes from */
};

/* A message's handler context: it reaps the queues whose completion queues it is the vector of. */
struct vector {
    struct nvme *c;
    unsigned index;
};

struct nvme {
    struct nvme *next;
    int index;
    struct pci_device *pci;
    volatile uint8_t *regs;
    unsigned stride, max_xfer; /* max_xfer: bytes a command moves at most */
    uint64_t ready_ns;         /* CC.EN to CSTS.RDY at most (CAP.TO, CRTO) */
    uint32_t rtd3e;
    uint16_t mqes;             /* the largest queue's entries, 0-based (CAP.MQES) */
    struct queue admin;
    struct queue *io;          /* I/O queue i is queue ID i + 1 */
    unsigned nio;              /* of them with memory, as the first enable made them */
    unsigned nvectors;         /* its messages; 0: polled */
    struct irq **irqs;
    struct vector *vectors;
    /* live: the admin queue takes commands; ready: the I/O queues too, looked at under a queue's
       submission lock. A timeout or a fatal status clears them until the reset that follows
       (recover). generation: moves on at each enable, so that of the callers whose commands failed
       only the first resets the controller. */
    bool live, ready, vwc;
    uint64_t generation;
    /* back from a sleep: enabled again before the next command. Set by resume while the disks are
       held (blk_hold). */
    bool woke;
    struct mutex reset_lock; /* enable: at start, after a sleep, after a timeout */
    uint8_t *ident;          /* a page for identify, at start */
    uint64_t ident_phys;
};

struct ns {
    struct blkdev blk;
    struct nvme *c;
    uint32_t nsid;
    unsigned block;
};

static struct nvme *ctrls;
static int nctrls, ndisks;

static uint32_t rd32(struct nvme *c, unsigned off) { return mmio_read32(c->regs + off); }
static void wr32(struct nvme *c, unsigned off, uint32_t v) { mmio_write32(c->regs + off, v); }
static uint64_t rd64(struct nvme *c, unsigned off) { return rd32(c, off) | (uint64_t)rd32(c, off + 4) << 32; }
static void wr64(struct nvme *c, unsigned off, uint64_t v) { /* low half first */
    wr32(c, off, (uint32_t)v);
    wr32(c, off + 4, (uint32_t)(v >> 32));
}
static void doorbell(struct nvme *c, uint16_t qid, bool cq, uint16_t v) {
    wr32(c, 0x1000 + (2u * qid + (cq ? 1 : 0)) * c->stride, v);
}

/* ---- queues ---- */
/* cq_lock held: every new completion entry ends its slot's wait. */
static void reap(struct queue *q) {
    bool moved = false;
    for (;;) {
        volatile uint32_t *e = q->cq + q->head * 4u;
        uint32_t dw3 = e[3];
        if (((dw3 >> 16) & 1) != q->phase) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* the entry's other words after its phase */
        uint32_t dw0 = e[0];
        unsigned id = dw3 & 0xFFFF;
        if (++q->head == q->size) {
            q->head = 0;
            q->phase ^= 1;
        }
        moved = true;
        if (id >= q->depth || !q->slots[id].busy) continue; /* an answer to a command a reset failed */
        struct slot *s = &q->slots[id];
        s->status = (int)(dw3 >> 17 & 0x7FFF);
        s->result = dw0;
        s->busy = false;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
        wq_wake_all(&s->wq);
    }
    if (moved) doorbell(q->c, q->qid, true, q->head);
}

static enum irq_result nvme_isr(void *context) {
    struct vector *v = context;
    struct nvme *c = v->c;
    if (!v->index) {
        spin_take(&c->admin.cq_lock);
        reap(&c->admin);
        spin_give(&c->admin.cq_lock);
    }
    unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE), step = MAX(c->nvectors, 1u);
    for (unsigned i = v->index; i < nio; i += step) {
        spin_take(&c->io[i].cq_lock);
        reap(&c->io[i]);
        spin_give(&c->io[i].cq_lock);
    }
    return IRQ_HANDLED;
}

/* The controller off (enable), or a new queue: the queue empty again, every command in it failed.
   The submission lock first: a submitter is either through it (its slot busy, failed here) or
   finds the controller down. */
static void queue_clear(struct queue *q) {
    uint64_t f = spin_lock(&q->sq_lock);
    spin_take(&q->cq_lock);
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE); /* phase 0: the controller's first pass writes 1 */
    q->tail = q->head = 0;
    q->phase = 1;
    for (unsigned i = 0; i < q->depth; i++) {
        struct slot *s = &q->slots[i];
        q->free[i / 64] |= 1ull << (i % 64);
        if (!s->busy) continue;
        s->status = FAILED;
        s->busy = false;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
        wq_wake_all(&s->wq);
    }
    spin_give(&q->cq_lock);
    spin_unlock(&q->sq_lock, f);
}

/* Memory for a queue of `entries`, from node: false without it. Once; enable clears it each time. */
static bool queue_alloc(struct queue *q, struct nvme *c, uint16_t qid, uint16_t entries, unsigned node) {
    q->c = c;
    q->qid = qid;
    q->size = entries;
    q->depth = (uint16_t)(entries - 1);
    q->node = node;
    if (!(q->sq_phys = pmm_alloc_node(node)) || !(q->cq_phys = pmm_alloc_node(node))) return false;
    q->sq = phys_to_virt(q->sq_phys);
    q->cq = phys_to_virt(q->cq_phys);
    q->slots = kzalloc(q->depth * sizeof *q->slots);
    q->free = kzalloc((q->depth + 63) / 64 * sizeof *q->free);
    if (!q->slots || !q->free) return false;
    queue_clear(q);
    return true;
}

/* sq_lock held: a free slot taken, -1 when none. */
static int slot_take(struct queue *q) {
    for (unsigned w = 0; w < (q->depth + 63u) / 64; w++)
        if (q->free[w]) {
            unsigned id = w * 64 + (unsigned)__builtin_ctzll(q->free[w]);
            q->free[w] &= q->free[w] - 1;
            return (int)id;
        }
    return -1;
}

static void slot_give(struct queue *q, unsigned id) {
    uint64_t f = spin_lock(&q->sq_lock);
    q->free[id / 64] |= 1ull << (id % 64);
    __atomic_add_fetch(&q->freed, 1, __ATOMIC_RELEASE);
    spin_unlock(&q->sq_lock, f);
    wq_wake_all(&q->room);
}

/* The controller can take no command until the next reset (recover): a command timed out, or the
   controller said it failed. */
static void controller_down(struct nvme *c) {
    __atomic_store_n(&c->ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&c->live, false, __ATOMIC_RELEASE);
}

/* Submit a command on q and wait for its completion. Returns 0, the 15-bit status field of a failed
   command, or FAILED when the controller stopped answering (it is then down until a reset). */
static int run(struct queue *q, uint32_t cmd[16], uint64_t timeout_ns, uint32_t *result) {
    struct nvme *c = q->c;
    uint64_t deadline = clock_after(timeout_ns);
    int id;
    uint64_t f;
    for (;;) {
        f = spin_lock(&q->sq_lock);
        bool up = __atomic_load_n(q == &c->admin ? &c->live : &c->ready, __ATOMIC_ACQUIRE);
        if (!up) {
            spin_unlock(&q->sq_lock, f);
            return FAILED;
        }
        if ((id = slot_take(q)) >= 0) break;
        uint64_t seen = __atomic_load_n(&q->freed, __ATOMIC_ACQUIRE);
        spin_unlock(&q->sq_lock, f);
        if (clock_now_ns() >= deadline) return FAILED;
        wq_wait_unless(&q->room, &q->freed, seen, deadline);
    }
    struct slot *s = &q->slots[id];
    uint64_t start = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    s->busy = true;
    cmd[0] = (cmd[0] & 0xFFFF) | (uint32_t)id << 16; /* PRPs, not fused */
    memcpy(q->sq + q->tail * 64u, cmd, 64);
    q->tail = (uint16_t)((q->tail + 1) % q->size);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* the entry before the doorbell */
    doorbell(c, q->qid, false, q->tail);
    spin_unlock(&q->sq_lock, f);

    int status = FAILED;
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seen != start) {
            status = s->status;
            if (result) *result = s->result;
            break;
        }
        if (!c->nvectors) {
            f = spin_lock(&q->cq_lock);
            reap(q);
            spin_unlock(&q->cq_lock, f);
            if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != start) continue;
        }
        uint32_t csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu || (csts & CSTS_CFS)) {
            kprintf("nvme%d: controller %s\n", c->index, csts == 0xFFFFFFFFu ? "vanished" : "reported a fatal error");
            controller_down(c);
            break;
        }
        if (clock_now_ns() >= deadline) {
            kprintf("nvme%d: command %02x on queue %u timed out\n", c->index, cmd[0] & 0xFF, q->qid);
            controller_down(c);
            break;
        }
        /* the completion's interrupt wakes it; polled, it looks again each millisecond */
        wq_wait_unless(&s->wq, &s->seq, seen, c->nvectors ? deadline : MIN(deadline, clock_after(NS_PER_MS)));
    }
    slot_give(q, (unsigned)id);
    return status;
}

static bool wait_ready(struct nvme *c, bool ready) {
    uint64_t deadline = clock_after(c->ready_ns);
    for (;;) {
        uint32_t csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu) return false;
        if (ready && (csts & CSTS_CFS)) return false;
        if (!!(csts & CSTS_RDY) == ready) return true;
        if (clock_now_ns() >= deadline) return false;
        sleep_until(clock_after(NS_PER_MS));
    }
}

/* The I/O queues the controller gives, at most one a CPU: their memory, once. */
static bool io_alloc(struct nvme *c, unsigned want) {
    struct queue *io = kzalloc(want * sizeof *io);
    if (!io) return false;
    uint16_t entries = (uint16_t)MIN((unsigned)IO_ENTRIES, c->mqes + 1u);
    for (unsigned i = 0; i < want; i++) {
        unsigned first = (unsigned)(((uint64_t)i * topology.cpus + want - 1) / want); /* its first CPU */
        if (!queue_alloc(&io[i], c, (uint16_t)(i + 1), entries, topology.cpu[first].node)) return false;
    }
    c->io = io;
    __atomic_store_n(&c->nio, want, __ATOMIC_RELEASE);
    return true;
}

/* Reset the controller and bring up the admin and I/O queues (section 3.5.1). Also the way back
   after a command timed out: the reset aborts whatever was outstanding, and every command still
   waiting fails. reset_lock held. */
static bool enable(struct nvme *c) {
    controller_down(c);
    __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELEASE);
    uint32_t cc = rd32(c, CC);
    if (cc & 1) {
        wait_ready(c, true); /* EN may only fall once RDY followed it up */
        wr32(c, CC, cc & ~1u);
    }
    if (!wait_ready(c, false)) {
        kprintf("nvme%d: controller did not reset\n", c->index);
        return false;
    }
    queue_clear(&c->admin);
    for (unsigned i = 0; i < c->nio; i++) queue_clear(&c->io[i]);
    wr32(c, AQA, (ADMIN_ENTRIES - 1u) << 16 | (ADMIN_ENTRIES - 1u));
    wr64(c, ASQ, c->admin.sq_phys);
    wr64(c, ACQ, c->admin.cq_phys);
    /* NVM command set, 4 KiB pages, round robin, 64-byte SQ and 16-byte CQ entries */
    wr32(c, CC, 1u | 6u << 16 | 4u << 20);
    if (!wait_ready(c, true)) {
        kprintf("nvme%d: controller did not become ready (status %x)\n", c->index, rd32(c, CSTS));
        return false;
    }
    __atomic_store_n(&c->live, true, __ATOMIC_RELEASE);
    /* Number of Queues (5.27.1.5): as many submission and completion queues as CPUs, 0-based; the
       controller says how many it gives. After a reset the same count again, the memory made. */
    unsigned want = c->nio ? c->nio : MIN(topology.cpus, 65535u);
    uint32_t cmd[16] = {OP_SET_FEATURES}, got = 0;
    cmd[10] = FEATURE_QUEUES;
    cmd[11] = (want - 1) << 16 | (want - 1);
    int r = run(&c->admin, cmd, ADMIN_NS, &got);
    if (r) goto fail;
    unsigned given = MIN((got & 0xFFFF) + 1, (got >> 16) + 1);
    if (!c->nio && !io_alloc(c, MIN(want, given))) {
        kprintf("nvme%d: no memory for its I/O queues\n", c->index);
        return false;
    }
    if (given < c->nio) {
        kprintf("nvme%d: gives %u I/O queues now, %u before\n", c->index, given, c->nio);
        r = FAILED;
        goto fail;
    }
    for (unsigned i = 0; i < c->nio; i++) {
        struct queue *q = &c->io[i];
        memset(cmd, 0, sizeof cmd);
        cmd[0] = OP_CREATE_CQ;
        *(uint64_t *)&cmd[6] = q->cq_phys;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        /* its vector, interrupts on when there are any, contiguous */
        cmd[11] = (c->nvectors ? (i % c->nvectors) << 16 | 2u : 0u) | 1u;
        if ((r = run(&c->admin, cmd, ADMIN_NS, NULL))) goto fail;
        memset(cmd, 0, sizeof cmd);
        cmd[0] = OP_CREATE_SQ;
        *(uint64_t *)&cmd[6] = q->sq_phys;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        cmd[11] = (uint32_t)q->qid << 16 | 1u; /* its completion queue, contiguous */
        if ((r = run(&c->admin, cmd, ADMIN_NS, NULL))) goto fail;
    }
    __atomic_store_n(&c->ready, true, __ATOMIC_RELEASE);
    return true;
fail:
    kprintf("nvme%d: setting up the I/O queues failed (%x)\n", c->index, r);
    controller_down(c);
    return false;
}

/* After a command failed with FAILED: the controller reset once by whichever caller comes first
   (generation still `seen`), the others then finding it up again. */
static bool recover(struct nvme *c, uint64_t seen) {
    mutex_lock(&c->reset_lock);
    bool ok = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) != seen ? __atomic_load_n(&c->ready, __ATOMIC_ACQUIRE)
                                                                         : enable(c);
    mutex_unlock(&c->reset_lock);
    return ok;
}

/* Back from a sleep, the controller enabled again first. */
static void awake(struct nvme *c) {
    if (!__atomic_load_n(&c->woke, __ATOMIC_ACQUIRE)) return;
    mutex_lock(&c->reset_lock);
    if (c->woke) {
        c->woke = false;
        if (enable(c)) kprintf("nvme%d: ready again after sleep\n", c->index);
        else kprintf("nvme%d: did not come back after sleep\n", c->index);
    }
    mutex_unlock(&c->reset_lock);
}

/* The calling CPU's I/O queue. */
static struct queue *io_queue(struct nvme *c) {
    unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE);
    return &c->io[(uint64_t)cpu_current_index() * nio / topology.cpus];
}

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid) {
    uint32_t cmd[16] = {OP_IDENTIFY};
    cmd[1] = nsid;
    *(uint64_t *)&cmd[6] = c->ident_phys;
    cmd[10] = cns;
    memset(c->ident, 0, PAGE_SIZE);
    return run(&c->admin, cmd, ADMIN_NS, NULL);
}

/* ---- transfers ---- */
/* PRP1 and PRP2 for `bytes` at buf (dword aligned, 513 pages at most): the first page in PRP1 at
   its offset, then the second page, or a list of the pages after the first in *list (a page the
   caller frees). False when a page is not mapped, or no page for the list. */
static bool prps(uint32_t cmd[16], const uint8_t *buf, unsigned bytes, uint64_t *list) {
    uint64_t va = (uint64_t)buf, first = vmm_kernel_translate(va);
    unsigned pages = (unsigned)(((va & (PAGE_SIZE - 1)) + bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t prp2 = 0, page = va & ~(uint64_t)(PAGE_SIZE - 1);
    *list = 0;
    if (!first) return false;
    if (pages == 2) {
        if (!(prp2 = vmm_kernel_translate(page + PAGE_SIZE))) return false;
    } else if (pages > 2) {
        if (!(*list = pmm_alloc())) return false;
        uint64_t *l = phys_to_virt(*list);
        for (unsigned i = 1; i < pages; i++)
            if (!(l[i - 1] = vmm_kernel_translate(page + i * PAGE_SIZE))) return false;
        prp2 = *list;
    }
    *(uint64_t *)&cmd[6] = first;
    *(uint64_t *)&cmd[8] = prp2;
    return true;
}

/* Read or write `n` logical blocks between the device and buf (blk_by_blocks): the caller's memory,
   or the request's bounce buffer. */
static int transfer(void *ctx, bool write, uint64_t lba, unsigned n, uint8_t *buf) {
    struct ns *s = ctx;
    struct nvme *c = s->c;
    for (int attempt = 0;; attempt++) {
        uint64_t gen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE), list;
        uint32_t cmd[16] = {write ? OP_WRITE : OP_READ};
        cmd[1] = s->nsid;
        bool described = prps(cmd, buf, n * s->block, &list);
        cmd[10] = (uint32_t)lba;
        cmd[11] = (uint32_t)(lba >> 32);
        cmd[12] = n - 1;
        int r = described ? run(io_queue(c), cmd, IO_NS, NULL) : -2;
        if (list) pmm_free(list);
        if (!r) return 0;
        if (r == -2) {
            kprintf("nvme%d: a buffer of %s block %lu is not mapped, or no page for its list\n", c->index,
                    write ? "write" : "read", lba);
            return -1;
        }
        if (attempt == 2) return -1;
        if (r == FAILED) {
            if (!recover(c, gen)) return -1; /* a timeout: reset and reissue */
            continue;
        }
        unsigned sct = (unsigned)r >> 8 & 7, sc = (unsigned)r & 0xFF;
        kprintf("nvme%d: %s of block %lu failed (type %u code %02x)\n", c->index, write ? "write" : "read", lba, sct, sc);
        if (r & 0x4000 || sct == 2) return -1; /* do not retry, or a media error */
    }
}

/* 512-byte sectors on top of the namespace's logical blocks: the caller's memory itself where the
   request covers whole blocks of dword aligned memory, else a bounce buffer of its own. */
static int rw(struct ns *s, bool write, uint64_t lba, uint32_t count, uint8_t *p) {
    struct nvme *c = s->c;
    awake(c);
    unsigned per = s->block / 512, most = c->max_xfer / s->block;
    bool direct = !((uint64_t)p & 3), whole = !(lba % per) && !(count % per);
    uint8_t *bounce = NULL;
    if (!direct || !whole) {
        size_t bytes = (size_t)MIN((uint64_t)most, (count + 2ull * per - 1) / per) * s->block;
        if (!(bounce = kmalloc(bytes))) return -N_ERR_NO_MEMORY;
        most = (unsigned)(bytes / s->block);
    }
    int r = blk_by_blocks(&s->blk, write, lba, count, p, bounce, most, direct, transfer, s);
    kfree(bounce);
    return r;
}

static int blk_nvme_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, false, lba, n, buf); }
static int blk_nvme_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, true, lba, n, (uint8_t *)buf);
}

/* Flush (NVM Command Set 3.2.2): the volatile write cache reaches the medium, the writes every
   queue completed before it with it. */
static int blk_nvme_persist(struct blkdev *b) {
    struct ns *s = b->priv;
    struct nvme *c = s->c;
    awake(c);
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t gen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
        uint32_t cmd[16] = {OP_FLUSH};
        cmd[1] = s->nsid;
        int r = run(io_queue(c), cmd, IO_NS, NULL);
        if (!r) return 0;
        if (r != FAILED || !recover(c, gen)) break;
    }
    return -N_ERR_IO;
}

static void add_namespace(struct nvme *c, uint32_t nsid, const char *model) {
    if (identify(c, 0, nsid)) return;
    const uint8_t *id = c->ident;
    uint64_t blocks = *(const uint64_t *)id;
    if (!blocks) return; /* inactive */
    unsigned format = id[26] & 0xF;
    uint32_t lbaf = *(const uint32_t *)(id + 128 + 4 * format);
    unsigned shift = lbaf >> 16 & 0xFF, metadata = lbaf & 0xFFFF, protection = id[29] & 7;
    if (metadata || protection || shift < 9 || shift > 12) {
        kprintf("nvme%d: namespace %u uses %u-byte blocks with %u bytes of metadata, skipped\n", c->index, nsid,
                shift < 32 ? 1u << shift : 0, metadata);
        return;
    }
    struct ns *s = kzalloc(sizeof *s);
    if (!s) return;
    s->c = c;
    s->nsid = nsid;
    s->block = 1u << shift;
    ksnprintf(s->blk.name, sizeof s->blk.name, "nvme%d", __atomic_fetch_add(&ndisks, 1, __ATOMIC_RELAXED));
    strlcpy(s->blk.model, model, sizeof s->blk.model);
    s->blk.sectors = blocks * (s->block / 512);
    s->blk.read = blk_nvme_read;
    s->blk.write = blk_nvme_write;
    if (c->vwc) s->blk.persist = blk_nvme_persist;
    s->blk.block = s->block;
    s->blk.priv = s;
    blk_register(&s->blk);
}

/* The active namespaces (Identify CNS 02h: up to 1024 IDs above the one given, in order), each
   added; a controller without the list (NVMe 1.0) has namespaces 1 to its count. */
static void add_namespaces(struct nvme *c, uint32_t count, const char *model) {
    uint32_t after = 0;
    if (identify(c, 2, 0)) {
        for (uint32_t nsid = 1; nsid && nsid <= count; nsid++) add_namespace(c, nsid, model);
        return;
    }
    uint32_t *ids = kmalloc(PAGE_SIZE);
    for (bool more = ids != NULL; more;) {
        uint32_t n = 0;
        memcpy(ids, c->ident, PAGE_SIZE);
        while (n < PAGE_SIZE / 4 && ids[n]) n++;
        for (uint32_t i = 0; i < n; i++) add_namespace(c, ids[i], model);
        more = n == PAGE_SIZE / 4 && !identify(c, 2, after = ids[n - 1]);
    }
    if (!ids) kprintf("nvme%d: no memory for its namespace list\n", c->index);
    kfree(ids);
}

/* Each message's vector on the CPU of the first queue it serves (the queues' CPUs are io_queue's). */
static void vectors_place(struct nvme *c) {
    for (unsigned v = 0; v < c->nvectors && v < c->nio; v++) {
        unsigned cpu = (unsigned)(((uint64_t)v * topology.cpus + c->nio - 1) / c->nio);
        if (cpu < topology.cpus) irq_set_cpu(c->irqs[v], cpu);
    }
}

/* Its messages, as many as there may be I/O queues (one a CPU) where the function has them. */
static void vectors_enable(struct nvme *c) {
    unsigned most = pci_msix_max_vectors(c->pci);
    if (!most) most = pci_msi_max_vectors(c->pci);
    unsigned want = MIN(MAX(most, 1u), topology.cpus);
    struct irq_spec *specs = kzalloc(want * sizeof *specs);
    c->irqs = kzalloc(want * sizeof *c->irqs);
    c->vectors = kzalloc(want * sizeof *c->vectors);
    if (!specs || !c->irqs || !c->vectors) {
        kfree(specs);
        return;
    }
    for (unsigned i = 0; i < want; i++) {
        c->vectors[i] = (struct vector){c, i};
        specs[i] = (struct irq_spec){.name = "NVMe", .handler = nvme_isr, .context = &c->vectors[i],
                                     .node = c->pci->node};
    }
    c->nvectors = pci_irq_enable(c->pci, specs, want, c->irqs);
    kfree(specs);
}

static void start(struct nvme *c) {
    struct pci_device *pci = c->pci;
    uint64_t base = pci_bar_address(pci, 0);
    if (!base) {
        kprintf("nvme%d: no register window\n", c->index);
        return;
    }
    /* the registers and the doorbells of every queue pair it has, all in its BAR */
    c->regs = vmm_map_mmio(base, ALIGN_UP(MAX(pci_bar_size(pci, 0), (uint64_t)0x2000), PAGE_SIZE));
    if (!c->regs) {
        kprintf("nvme%d: no memory to map its registers\n", c->index);
        return;
    }
    pci_enable_busmaster(pci);
    uint64_t cap = rd64(c, CAP);
    c->stride = 4u << (cap >> 32 & 0xF);
    c->mqes = (uint16_t)(cap & 0xFFFF);
    unsigned to = (unsigned)(cap >> 24 & 0xFF);
    if (cap >> 59 & 3) { /* 2.x ready modes: CRTO may be longer than CAP.TO can say */
        unsigned crwmt = rd32(c, CRTO) & 0xFFFF;
        if (crwmt > to) to = crwmt;
    }
    c->ready_ns = MAX(to, 4u) * 500 * NS_PER_MS; /* 500 ms units, at least 2 s */
    if (!(cap >> 37 & 1) || (cap >> 48 & 0xF)) {
        kprintf("nvme%d: no NVM command set or no 4 KiB pages (capabilities %lx), skipped\n", c->index, cap);
        return;
    }
    if (!(c->ident_phys = pmm_alloc_node(pci->node < 0 ? 0 : (unsigned)pci->node)) ||
        !queue_alloc(&c->admin, c, 0, ADMIN_ENTRIES, pci->node < 0 ? 0 : (unsigned)pci->node))
        return;
    c->ident = phys_to_virt(c->ident_phys);
    vectors_enable(c);
    mutex_lock(&c->reset_lock);
    bool up = enable(c);
    mutex_unlock(&c->reset_lock);
    if (!up) return;
    vectors_place(c);

    if (identify(c, 1, 0)) {
        kprintf("nvme%d: identify failed\n", c->index);
        return;
    }
    const uint8_t *id = c->ident;
    char model[41];
    unsigned n = 0;
    for (int i = 24; i < 64 && n < sizeof model - 1; i++) {
        char ch = id[i] >= 0x20 && id[i] < 0x7F ? (char)id[i] : ' ';
        if (ch == ' ' && (!n || model[n - 1] == ' ')) continue;
        model[n++] = ch;
    }
    while (n && model[n - 1] == ' ') n--;
    model[n] = 0;
    unsigned mdts = id[77];
    c->max_xfer = PRPS_PER_PAGE * PAGE_SIZE;
    if (mdts && mdts < 20 && (PAGE_SIZE << mdts) < c->max_xfer) c->max_xfer = PAGE_SIZE << mdts;
    c->vwc = id[525] & 1;
    c->rtd3e = *(const uint32_t *)(id + 88);
    uint32_t count = *(const uint32_t *)(id + 516);
    kprintf("nvme%d: %s (%04x:%04x), version %x, %u namespaces, %u I/O queues of %u, %u %s%s\n", c->index, model,
            pci->vendor, pci->device, rd32(c, VS), count, c->nio, c->io[0].depth, c->nvectors,
            !c->nvectors ? "vectors: polled" : pci_msix_max_vectors(pci) ? "MSI-X vectors" : "MSI vectors",
            c->vwc ? ", write cache" : "");
    add_namespaces(c, count, model);
}

/* Normal shutdown notification before power goes away (section 3.6.1): the controller writes its
   cache back and the drive does not count an unsafe shutdown. Interrupts are already off, the
   disks held (no command in flight). */
static bool nvme_shutdown(void *arg) {
    (void)arg;
    bool all = true;
    for (struct nvme *c = ctrls; c; c = c->next) {
        if (!c->live) continue;
        wr32(c, CC, (rd32(c, CC) & ~0xC000u) | 0x4000u);
        uint64_t us = c->rtd3e < 1000000 ? 1000000 : c->rtd3e > 10000000 ? 10000000 : c->rtd3e;
        for (uint64_t deadline = clock_after(us * NS_PER_US);
             (rd32(c, CSTS) >> 2 & 3) != 2 && clock_now_ns() < deadline;)
            pause();
        if ((rd32(c, CSTS) >> 2 & 3) != 2) all = false; /* its cache may not be on the flash */
        controller_down(c);
    }
    return all;
}

/* After a sleep (the shutdown notification above went first): the next read or write resets the
   controller and sets the queues up again, the way it recovers from a timeout. Resume must not
   wait for the controller; the I/O path may (it reports a controller that does not come back). */
static bool nvme_resume(void *arg) {
    (void)arg;
    for (struct nvme *c = ctrls; c; c = c->next)
        if (c->regs && c->ident) __atomic_store_n(&c->woke, true, __ATOMIC_RELEASE);
    return true;
}

static struct pm_device nvme_pm = {.name = "nvme", .stage = PM_STAGE_STORAGE, .quiesce = nvme_shutdown,
                                   .resume = nvme_resume};

void nvme_init(void) {
    struct nvme **tail = &ctrls;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *p = pci_at(i);
        if (p->cls != 0x01 || p->subcls != 0x08 || p->progif != 0x02) continue;
        struct nvme *c = kzalloc(sizeof *c);
        if (!c) {
            kprintf("nvme: no memory for the controller at %02x:%02x.%u\n", p->bus, p->dev, p->func);
            continue;
        }
        c->index = nctrls++;
        c->pci = p;
        *tail = c;
        tail = &c->next;
        start(c);
    }
    if (nctrls) pm_register(&nvme_pm);
}
