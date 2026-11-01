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
   dword aligned. Every active namespace with plain blocks of any size a command can move (no
   metadata, no protection information) becomes a block device "nvmeN". Completions are polled when
   the platform offers no message interrupt, and until the controller has said how many queues it
   gives. */
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
#include "sys/thread.h"
#include "sys/wait.h"

#define ADMIN_ENTRIES 32
#define IO_ENTRIES 64             /* an I/O queue's entries: its submission queue fills one page */
#define PRPS_PER_PAGE 512         /* one PRP list page: a command of up to 2 MiB */
#define ADMIN_NS (5 * NS_PER_S)
#define IO_NS (30 * NS_PER_S)
#define SHUTDOWN_MOST_NS (60 * NS_PER_S) /* our limit for a shutdown under way: 3.6.1 gives none */

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
#define CSTS_SHST(v) ((v) >> 2 & 3)
#define SHST_NONE 0u
#define SHST_DONE 2u
#define SHST_RESERVED 3u
/* CAP.CSS (Figure 36), and what each makes of CC.CSS (3.5.1 step 3) */
#define CAP_CSS_NVM (1ull << 37)
#define CAP_CSS_SETS (1ull << 43)
#define CAP_CSS_NOIO (1ull << 44)
#define CC_EN 1u
#define CC_SHN 0xC000u
#define CC_SHN_NORMAL 0x4000u
#define CC_CSS_NVM (0u << 4)
#define CC_CSS_SETS (6u << 4)
#define CC_IOSQES(n) ((uint32_t)(n) << 16)
#define CC_IOCQES(n) ((uint32_t)(n) << 20)
#define SQES 6u /* 64-byte submission queue entries */
#define CQES 4u /* 16-byte completion queue entries */
#define CNS_NAMESPACE 0x00
#define CNS_CONTROLLER 0x01
#define CNS_ACTIVE 0x02
#define CNS_NAMESPACE_OF_SET 0x05
#define CNS_CONTROLLER_OF_SET 0x06
#define CNS_ACTIVE_OF_SET 0x07
#define CNS_NAMESPACE_ANY_SET 0x08
#define CNS_COMMAND_SETS 0x1C
#define CNTID_THIS 0xFFFFu

/* admin and NVM command opcodes */
#define OP_DELETE_SQ 0x00
#define OP_CREATE_SQ 0x01
#define OP_DELETE_CQ 0x04
#define OP_CREATE_CQ 0x05
#define OP_IDENTIFY 0x06
#define OP_SET_FEATURES 0x09
#define OP_FLUSH 0x00
#define OP_WRITE 0x01
#define OP_READ 0x02
#define FEATURE_QUEUES 0x07
#define FEATURE_COMMAND_SETS 0x19

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
struct ns;
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
    uint64_t cap;
    unsigned stride, max_xfer; /* max_xfer: bytes a command moves at most */
    uint64_t ready_ns;         /* CC.EN to CSTS.RDY at most (CAP.TO, CRTO) */
    uint32_t rtd3e;            /* Identify Controller: the shutdown's microseconds, 0 not reported */
    uint32_t namespaces;       /* Identify Controller: NN */
    char model[41];
    /* Which device this is (4.7.1.2): its VID, SSVID, SN and MN, and its NQN, of Identify Controller. */
    uint8_t identity[64];
    uint8_t nqn[256];
    bool stranger;             /* cut for being another device, which still answers */
    uint16_t mqes;             /* the largest queue's entries, 0-based (CAP.MQES) */
    struct queue admin;
    struct queue *io;          /* I/O queue i is queue ID i + 1 */
    unsigned nio;              /* of them with memory, as the first enable made them */
    unsigned nvectors;         /* its messages; 0: polled */
    struct irq **irqs;
    struct vector *vectors;
    struct ns *found;          /* its usable namespaces, as the first enable found them */
    bool discovered;           /* found is made */
    bool in_use;               /* they are block devices: what they were given is to be kept */
    /* live: the admin queue takes commands; ready: the I/O queues too, looked at under a queue's
       submission lock. A timeout or a fatal status clears them until the reset that follows
       (recover). generation: moves on at each enable, so that of the callers whose commands failed
       only the first resets the controller. */
    bool live, ready, vwc;
    uint64_t generation;
    /* back from a sleep: enabled again before the next command. Set by resume while the disks are
       held (blk_hold). */
    bool woke;
    /* One stop at a time (controller_stop), taken inside reset_lock and alone by a command that
       ran out of time. stops moves on when one has ended; stopped: the last one's end, its bus
       mastering off if false; cut: the controller does not stop, and it stays out of use. */
    struct mutex stop_lock;
    uint64_t stops;
    bool stopped;
    bool called_off; /* the last stop only gave up, its thread being killed (atomic) */
    bool cut;
    struct mutex reset_lock; /* enable: at start, after a sleep, after a timeout */
    uint8_t *ident;          /* a page for identify, at start */
    uint64_t ident_phys;
};

struct ns {
    struct blkdev blk;
    struct nvme *c;
    struct ns *next;
    uint32_t nsid;
    unsigned block;
};

static struct nvme *ctrls;
static int nctrls, ndisks;

static int blk_nvme_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf);
static int blk_nvme_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf);
static int blk_nvme_persist(struct blkdev *b);

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
static bool queue_up(const struct queue *q) {
    return __atomic_load_n(q == &q->c->admin ? &q->c->live : &q->c->ready, __ATOMIC_ACQUIRE);
}

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
    if (moved && queue_up(q)) doorbell(q->c, q->qid, true, q->head); /* else the stop's queue_clear empties it */
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

/* The controller off (controller_stop), or a new queue: the queue empty again, every command in it
   failed, each one's slot given back by its waiter. The submission lock first: a submitter is either
   through it (its slot busy, failed here) or finds the controller down. */
static void queue_clear(struct queue *q) {
    uint64_t f = spin_lock(&q->sq_lock);
    spin_take(&q->cq_lock);
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE); /* phase 0: the controller's first pass writes 1 */
    q->tail = q->head = 0;
    q->phase = 1;
    for (unsigned i = 0; i < q->depth; i++) {
        struct slot *s = &q->slots[i];
        if (!s->busy) continue;
        s->status = FAILED;
        s->busy = false;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
        wq_wake_all(&s->wq);
    }
    spin_give(&q->cq_lock);
    spin_unlock(&q->sq_lock, f);
}

/* Whoever was in the queue's locks has gone. */
static void queue_pass(struct queue *q) {
    spin_unlock(&q->sq_lock, spin_lock(&q->sq_lock));
    spin_unlock(&q->cq_lock, spin_lock(&q->cq_lock));
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
    for (unsigned i = 0; i < q->depth; i++) q->free[i / 64] |= 1ull << (i % 64);
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

/* CSTS.RDY to follow CC.EN, within CAP.TO or CRTO; a fatal status does not keep it from falling, and
   ends the wait for it to rise only for one that will not use it (past_fatal false). */
static bool wait_ready(struct nvme *c, bool ready, bool past_fatal) {
    uint64_t deadline = clock_after(c->ready_ns);
    for (;;) {
        uint32_t csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu) return false;
        if (ready && !past_fatal && (csts & CSTS_CFS)) return false;
        if (!!(csts & CSTS_RDY) == ready) return true;
        if (clock_now_ns() >= deadline) return false;
        sleep_until(clock_after(NS_PER_MS));
    }
}

/* CSTS.RDY fallen, and no fatal status left that the reset was to clear (9.5). */
static bool controller_idle(struct nvme *c) {
    return wait_ready(c, false, false) && !(rd32(c, CSTS) & CSTS_CFS);
}

/* The controller stopped by a Controller Reset (CC.EN to 0 and CSTS.RDY following, 3.7.2): nothing
   it held moves any more, so every command in it fails and its caller may take the memory back.
   A shutdown it may have notified ends with this write: SHN is written back as 00b, or the
   same write would begin another (Figure 41). EN does not fall while RDY reads 0, which is undefined
   (Figure 41): RDY is given its time to rise, whatever else the status says. A controller where this
   cannot be done, or does not end, or stays fatal, gets the other Controller Level Reset, the
   Function Level Reset (3.7.2). The reset of a healthy controller loses nothing of what it completed
   (Figure 41, EN); nothing says that of a fatal status, of one gone from the bus, of a Function
   Level Reset or of EN found 0 while it was in use, a reset no one here asked for (3.7.2), and its
   cache may be gone. One that still does not stop has its bus mastering turned
   off, and stays out of use with its namespaces. If the reset was only called off, its thread being
   killed (dev/pci.h), the next request goes on with it. A Function Level Reset left unfinished is not
   to be reached by anything but its next call, and has its bus mastering off already (dev/pci.h, 6.6.2):
   neither the registers nor the configuration are touched before that; nor does anyone that read the
   queue up go on past its locks (run, reap). `seen`: c->stops as the caller
   read it; a stop that ended since has done this for it. What the last stop came to is returned. */
static bool controller_stop(struct nvme *c, uint64_t seen) {
    mutex_lock(&c->stop_lock);
    if (__atomic_load_n(&c->stops, __ATOMIC_ACQUIRE) == seen) {
        bool was_live = __atomic_load_n(&c->live, __ATOMIC_ACQUIRE);
        controller_down(c);
        bool resetting = pci_reset_left(c->pci);
        uint32_t cc = resetting ? 0xFFFFFFFFu : rd32(c, CC), csts = resetting ? 0xFFFFFFFFu : rd32(c, CSTS);
        bool stopped = false, lost = csts == 0xFFFFFFFFu || csts & CSTS_CFS;
        if (cc != 0xFFFFFFFFu && cc & CC_EN) {
            wait_ready(c, true, true);
            if (rd32(c, CSTS) & CSTS_RDY) {
                wr32(c, CC, cc & ~(CC_EN | CC_SHN));
                stopped = controller_idle(c);
            }
        } else if (cc != 0xFFFFFFFFu) {
            lost = lost || was_live;
            stopped = controller_idle(c);
        }
        if (!stopped) {
            lost = true;
            unsigned queues = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE);
            queue_pass(&c->admin);
            for (unsigned i = 0; i < queues; i++) queue_pass(&c->io[i]);
            if (pci_function_reset(c->pci)) stopped = controller_idle(c);
        }
        if (lost)
            for (struct ns *s = c->found; s; s = s->next) blk_cache_lost(&s->blk);
        bool called_off = false;
        if (!stopped) {
            if (!pci_reset_left(c->pci)) pci_disable_busmaster(c->pci, NULL);
            called_off = thread_current() && thread_current()->killed;
            if (called_off) {
                kprintf("nvme%d: its reset was called off with its thread\n", c->index);
            } else {
                kprintf("nvme%d: controller did not reset, turned off\n", c->index);
                __atomic_store_n(&c->cut, true, __ATOMIC_RELEASE);
                for (struct ns *s = c->found; s; s = s->next) __atomic_store_n(&s->blk.gone, true, __ATOMIC_RELEASE);
            }
        }
        c->stopped = stopped;
        __atomic_store_n(&c->called_off, called_off, __ATOMIC_RELEASE);
        queue_clear(&c->admin);
        unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < nio; i++) queue_clear(&c->io[i]);
        __atomic_add_fetch(&c->stops, 1, __ATOMIC_RELEASE);
    }
    bool stopped = c->stopped;
    mutex_unlock(&c->stop_lock);
    return stopped;
}

/* Enters a command on q in the free slot `id` (sq_lock held): the entry, then its doorbell. */
static void submit(struct queue *q, int id, uint32_t cmd[16]) {
    q->slots[id].busy = true;
    cmd[0] = (cmd[0] & 0xFFFF) | (uint32_t)id << 16; /* PRPs, not fused */
    memcpy(q->sq + q->tail * 64u, cmd, 64);
    q->tail = (uint16_t)((q->tail + 1) % q->size);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* the entry before the doorbell */
    doorbell(q->c, q->qid, false, q->tail);
}

/* Submit a command on q and wait for its completion. Returns 0, the 15-bit status field of a failed
   command, or FAILED when the controller stopped answering. Then it has been stopped, and nothing
   of the command moves any more: the caller may take its memory back. It is down until a reset.
   Waiting for a free slot takes no time from the command: every command in the queue ends, or
   stops the controller, within its own time. `writing`: the device whose write this is. */
static int run(struct queue *q, uint32_t cmd[16], uint64_t timeout_ns, uint32_t *result, struct blkdev *writing) {
    struct nvme *c = q->c;
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
        wq_wait_unless(&q->room, &q->freed, seen, clock_after(timeout_ns));
    }
    uint64_t deadline = clock_after(timeout_ns);
    struct slot *s = &q->slots[id];
    uint64_t start = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    uint64_t stops = __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE);
    if (writing) blk_write_given(writing);
    submit(q, id, cmd);
    spin_unlock(&q->sq_lock, f);

    int status = FAILED;
    bool waited = false;
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (seen != start) {
            status = s->status;
            if (result) *result = s->result;
            break;
        }
        /* Down: a stop is under way, which ends this command, and the function may be in its Function
           Level Reset, which nothing else is to reach (PCIe 6.6.2). Up: read under the lock the stop
           passes through before that reset, by a command that has not ended within its first wait. */
        f = spin_lock(&q->cq_lock);
        bool up = queue_up(q);
        if (up && !c->nvectors) reap(q);
        uint32_t csts = up && waited ? rd32(c, CSTS) : 0;
        spin_unlock(&q->cq_lock, f);
        if (!up) {
            wq_wait_unless(&s->wq, &s->seq, seen, clock_after(NS_PER_MS));
            continue;
        }
        if (!c->nvectors && __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != start) continue;
        if (csts == 0xFFFFFFFFu || (csts & CSTS_CFS) || clock_now_ns() >= deadline) {
            if (csts == 0xFFFFFFFFu || (csts & CSTS_CFS))
                kprintf("nvme%d: controller %s\n", c->index, csts == 0xFFFFFFFFu ? "vanished" : "reported a fatal error");
            else
                kprintf("nvme%d: command %02x on queue %u timed out\n", c->index, cmd[0] & 0xFF, q->qid);
            controller_stop(c, stops); /* fails the command, seq moves on */
            continue;
        }
        /* the completion's interrupt wakes it; polled, it looks again each millisecond */
        wq_wait_unless(&s->wq, &s->seq, seen, c->nvectors && waited ? deadline : MIN(deadline, clock_after(NS_PER_MS)));
        waited = true;
    }
    if (writing) blk_write_ended(writing);
    slot_give(q, (unsigned)id);
    return status;
}

/* An admin command where interrupts are off and nothing may sleep (a power change: no other command
   is in the controller): its completion looked for by this CPU. 0, the status field, or FAILED. */
static int run_polled(struct nvme *c, uint32_t cmd[16], uint64_t timeout_ns) {
    struct queue *q = &c->admin;
    uint64_t f = spin_lock(&q->sq_lock);
    int id = slot_take(q);
    if (id < 0) {
        spin_unlock(&q->sq_lock, f);
        return FAILED;
    }
    struct slot *s = &q->slots[id];
    uint64_t start = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    submit(q, id, cmd);
    spin_unlock(&q->sq_lock, f);
    int status = FAILED;
    for (uint64_t deadline = clock_after(timeout_ns); clock_now_ns() < deadline; pause()) {
        f = spin_lock(&q->cq_lock);
        reap(q);
        spin_unlock(&q->cq_lock, f);
        if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != start) {
            status = s->status;
            break;
        }
        if (rd32(c, CSTS) & CSTS_CFS) break;
    }
    /* never answered: a late answer finds no command, and the slot is free for another */
    f = spin_lock(&q->cq_lock);
    s->busy = false;
    spin_unlock(&q->cq_lock, f);
    slot_give(q, (unsigned)id);
    return status;
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

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid) {
    uint32_t cmd[16] = {OP_IDENTIFY};
    cmd[1] = nsid;
    *(uint64_t *)&cmd[6] = c->ident_phys;
    cmd[10] = cns;
    memset(c->ident, 0, PAGE_SIZE);
    return run(&c->admin, cmd, ADMIN_NS, NULL, NULL);
}

/* Identify Controller (3.5.1 step 7), with what the host sets from it: the I/O queue entry sizes,
   which the data structure's SQES and CQES bound (Figure 338), written while no I/O queue exists
   (Figure 41). False when this controller cannot take the entries we build. */
static bool read_controller(struct nvme *c) {
    int r = identify(c, CNS_CONTROLLER, 0);
    if (r) {
        kprintf("nvme%d: identify failed (%x)\n", c->index, r);
        return false;
    }
    const uint8_t *id = c->ident;
    /* Not the device it was: its namespaces are not the ones here, and nothing is written to it. */
    if (c->discovered && (memcmp(c->identity, id, sizeof c->identity) || memcmp(c->nqn, id + 768, sizeof c->nqn))) {
        kprintf("nvme%d: another device is here than before, turned off\n", c->index);
        pci_disable_busmaster(c->pci, NULL);
        c->stranger = true;
        __atomic_store_n(&c->cut, true, __ATOMIC_RELEASE);
        for (struct ns *s = c->found; s; s = s->next) __atomic_store_n(&s->blk.gone, true, __ATOMIC_RELEASE);
        return false;
    }
    memcpy(c->identity, id, sizeof c->identity);
    memcpy(c->nqn, id + 768, sizeof c->nqn);
    unsigned n = 0;
    for (int i = 24; i < 64 && n < sizeof c->model - 1; i++) {
        char ch = id[i] >= 0x20 && id[i] < 0x7F ? (char)id[i] : ' ';
        if (ch == ' ' && (!n || c->model[n - 1] == ' ')) continue;
        c->model[n++] = ch;
    }
    while (n && c->model[n - 1] == ' ') n--;
    c->model[n] = 0;
    unsigned mdts = id[77], most = PRPS_PER_PAGE * PAGE_SIZE;
    if (mdts && mdts < 20 && (PAGE_SIZE << mdts) < most) most = PAGE_SIZE << mdts;
    __atomic_store_n(&c->max_xfer, most, __ATOMIC_RELEASE); /* a reset's read beside the callers' */
    c->vwc = id[525] & 1;
    c->rtd3e = *(const uint32_t *)(id + 88);
    c->namespaces = *(const uint32_t *)(id + 516);
    unsigned sqes = id[512], cqes = id[513];
    if ((sqes & 0xF) > SQES || (sqes >> 4) < SQES || (cqes & 0xF) > CQES || (cqes >> 4) < CQES) {
        kprintf("nvme%d: takes no 64-byte and 16-byte queue entries (%02x, %02x), skipped\n", c->index, sqes, cqes);
        return false;
    }
    wr32(c, CC, rd32(c, CC) | CC_IOSQES(SQES) | CC_IOCQES(CQES));
    return true;
}

/* A controller of several I/O command sets (CAP.CSS.IOCSS, CC.CSS 110b): the first combination it
   has with the NVM command set selected (3.5.1 step 8.a, I/O Command Set Profile). The ones we do
   not drive are not enabled by any namespace we use. A refusal leaves the one it had. */
static bool select_command_set(struct nvme *c) {
    uint32_t cmd[16] = {OP_IDENTIFY};
    *(uint64_t *)&cmd[6] = c->ident_phys;
    cmd[10] = (uint32_t)CNTID_THIS << 16 | CNS_COMMAND_SETS;
    memset(c->ident, 0, PAGE_SIZE);
    int r = run(&c->admin, cmd, ADMIN_NS, NULL, NULL);
    if (r) {
        kprintf("nvme%d: identify of its command sets failed (%x)\n", c->index, r);
        return false;
    }
    const uint64_t *vectors = (const uint64_t *)c->ident;
    unsigned combination = 0;
    while (combination < PAGE_SIZE / 8 && !(vectors[combination] & 1)) combination++;
    if (combination == PAGE_SIZE / 8) {
        kprintf("nvme%d: no combination of its command sets has the NVM one, skipped\n", c->index);
        return false;
    }
    memset(cmd, 0, sizeof cmd);
    cmd[0] = OP_SET_FEATURES;
    cmd[10] = FEATURE_COMMAND_SETS;
    cmd[11] = combination;
    if ((r = run(&c->admin, cmd, ADMIN_NS, NULL, NULL)) == FAILED) return false;
    if (r) kprintf("nvme%d: refused command set combination %u (%x)\n", c->index, combination, r);
    return true;
}

/* A namespace from its Identify Namespace (CNS 00h) noted, a block device once the controller is up.
   FLBAS's Format Index has its upper bits meant only with more than 16 formats (NVM Command Set Figure
   114; NLBAF is 0-based, NULBAF 1-based, 5.5). A block a command cannot move is out, none other. */
static void add_namespace(struct nvme *c, uint32_t nsid) {
    if (identify(c, CNS_NAMESPACE, nsid)) return;
    const uint8_t *id = c->ident;
    uint64_t blocks = *(const uint64_t *)id;
    if (!blocks) return; /* inactive */
    unsigned formats = id[25] + 1u + id[83];
    unsigned format = (id[26] & 0xF) | (formats > 16 ? (id[26] >> 5 & 3u) << 4 : 0);
    uint32_t lbaf = *(const uint32_t *)(id + 128 + 4 * format);
    unsigned shift = lbaf >> 16 & 0xFF, metadata = lbaf & 0xFFFF, protection = id[29] & 7;
    if (metadata || protection || shift < 9 || shift >= 32 || (1u << shift) > c->max_xfer) {
        kprintf("nvme%d: namespace %u uses %u-byte blocks with %u bytes of metadata, skipped\n", c->index, nsid,
                shift < 32 ? 1u << shift : 0, metadata);
        return;
    }
    struct ns *s = kzalloc(sizeof *s);
    if (!s) return;
    s->c = c;
    s->nsid = nsid;
    s->block = 1u << shift;
    s->blk.sectors = blocks * (s->block / 512);
    s->blk.read = blk_nvme_read;
    s->blk.write = blk_nvme_write;
    if (c->vwc) s->blk.persist = blk_nvme_persist;
    s->blk.block = s->block;
    s->blk.priv = s;
    struct ns **tail = &c->found;
    while (*tail) tail = &(*tail)->next;
    *tail = s;
}

/* The rest of step 8.b.ii.2 for a namespace of the NVM command set (CSI 00h): its Identify Namespace of
   the command set, the command set's Identify Controller, and the Identify Namespace of any command
   set. They add nothing to a plain namespace but whether it is ready, which a later command
   would find too (its status says to try again). A controller that has not these fails them
   (Invalid Field) and it means nothing. */
static void identify_command_set(struct nvme *c, uint32_t nsid) {
    identify(c, CNS_NAMESPACE_OF_SET, nsid);
    identify(c, CNS_CONTROLLER_OF_SET, 0);
    if (!identify(c, CNS_NAMESPACE_ANY_SET, nsid) && !(c->ident[14] & 1))
        kprintf("nvme%d: namespace %u is not ready yet\n", c->index, nsid);
}

/* The active namespaces of the NVM command set (Identify CNS 07h with CSI 00h, 3.5.1 step 8.b, or CNS
   02h where it is not known: up to 1024 IDs above the one given, in order), each noted; a controller
   without the list (NVMe 1.0) has namespaces 1 to its count. */
static void add_namespaces(struct nvme *c) {
    uint32_t cns = CNS_ACTIVE_OF_SET;
    if (identify(c, cns, 0) && identify(c, cns = CNS_ACTIVE, 0)) {
        for (uint32_t nsid = 1; nsid && nsid <= c->namespaces; nsid++) add_namespace(c, nsid);
        return;
    }
    uint32_t *ids = kmalloc(PAGE_SIZE);
    for (bool more = ids != NULL; more;) {
        uint32_t n = 0;
        memcpy(ids, c->ident, PAGE_SIZE);
        while (n < PAGE_SIZE / 4 && ids[n]) n++;
        for (uint32_t i = 0; i < n; i++) {
            add_namespace(c, ids[i]);
            if (cns == CNS_ACTIVE_OF_SET) identify_command_set(c, ids[i]);
        }
        more = n == PAGE_SIZE / 4 && !identify(c, cns, ids[n - 1]);
    }
    if (!ids) kprintf("nvme%d: no memory for its namespace list\n", c->index);
    kfree(ids);
}

/* Its messages, as many as there are I/O queues where the function has them; each queue's completion
   queue takes one in turn. */
static void vectors_enable(struct nvme *c) {
    unsigned most = pci_msix_max_vectors(c->pci);
    if (!most) most = pci_msi_max_vectors(c->pci);
    unsigned want = MIN(MAX(most, 1u), c->nio);
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

/* Reset the controller and bring up the admin and I/O queues, in the order of 3.5.1. Also the way
   back after a command timed out: the stop aborts whatever was outstanding, and every command still
   waiting fails. reset_lock held. */
static bool enable(struct nvme *c) {
    if (__atomic_load_n(&c->cut, __ATOMIC_ACQUIRE)) return false;
    __atomic_add_fetch(&c->generation, 1, __ATOMIC_RELEASE);
    /* a stop whose thread gave up, ended just as this came, is not this thread's answer */
    if (!controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE)) &&
        !(__atomic_load_n(&c->called_off, __ATOMIC_ACQUIRE) && controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE))))
        return false;
    pci_enable_busmaster(c->pci); /* a Function Level Reset's configuration comes back without it */
    wr32(c, AQA, (ADMIN_ENTRIES - 1u) << 16 | (ADMIN_ENTRIES - 1u));
    wr64(c, ASQ, c->admin.sq_phys);
    wr64(c, ACQ, c->admin.cq_phys);
    /* its command sets, 4 KiB pages, round robin */
    bool sets = c->cap & CAP_CSS_SETS;
    wr32(c, CC, CC_EN | (sets ? CC_CSS_SETS : CC_CSS_NVM));
    if (!wait_ready(c, true, false)) {
        kprintf("nvme%d: controller did not become ready (status %x)\n", c->index, rd32(c, CSTS));
        return false;
    }
    __atomic_store_n(&c->live, true, __ATOMIC_RELEASE);
    int r = FAILED;
    if (!read_controller(c) || (sets && !select_command_set(c))) goto down;
    if (!c->discovered) { /* step 8.b, once: the block devices stay what they were */
        add_namespaces(c);
        c->discovered = true;
    }
    /* Number of Queues (5.2.30.1.5): as many submission and completion queues as CPUs, 0-based; the
       controller says how many it gives. After a reset the same count again, the memory made. */
    unsigned want = c->nio ? c->nio : MIN(topology.cpus, 65535u);
    uint32_t cmd[16] = {OP_SET_FEATURES}, got = 0;
    cmd[10] = FEATURE_QUEUES;
    cmd[11] = (want - 1) << 16 | (want - 1);
    r = run(&c->admin, cmd, ADMIN_NS, &got, NULL);
    if (r) goto fail;
    unsigned given = MIN((got & 0xFFFF) + 1, (got >> 16) + 1);
    if (!c->nio) {
        if (!io_alloc(c, MIN(want, given))) {
            kprintf("nvme%d: no memory for its I/O queues\n", c->index);
            goto down;
        }
        vectors_enable(c); /* step 9, now the queues are known (Transport 3.1); the admin queue was polled */
    }
    if (given < c->nio) {
        kprintf("nvme%d: gives %u I/O queues now, %u before\n", c->index, given, c->nio);
        r = FAILED;
        goto fail;
    }
    for (unsigned i = 0; i < c->nio; i++) { /* step 10: the completion queues */
        struct queue *q = &c->io[i];
        memset(cmd, 0, sizeof cmd);
        cmd[0] = OP_CREATE_CQ;
        *(uint64_t *)&cmd[6] = q->cq_phys;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        /* its vector, interrupts on when there are any, contiguous */
        cmd[11] = (c->nvectors ? (i % c->nvectors) << 16 | 2u : 0u) | 1u;
        if ((r = run(&c->admin, cmd, ADMIN_NS, NULL, NULL))) goto fail;
    }
    for (unsigned i = 0; i < c->nio; i++) { /* step 11: the submission queues */
        struct queue *q = &c->io[i];
        memset(cmd, 0, sizeof cmd);
        cmd[0] = OP_CREATE_SQ;
        *(uint64_t *)&cmd[6] = q->sq_phys;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        cmd[11] = (uint32_t)q->qid << 16 | 1u; /* its completion queue, contiguous */
        if ((r = run(&c->admin, cmd, ADMIN_NS, NULL, NULL))) goto fail;
    }
    __atomic_store_n(&c->ready, true, __ATOMIC_RELEASE);
    return true;
fail:
    kprintf("nvme%d: setting up the I/O queues failed (%x)\n", c->index, r);
down:
    controller_down(c);
    return false;
}

/* After a command failed with FAILED: up now, a reset has been done since. Else the controller reset
   once by whichever caller comes first (generation still `seen`), the others finding that it did not
   come up; or, if that caller's thread was killed and gave up on it, going on with it themselves. */
static bool recover(struct nvme *c, uint64_t seen) {
    mutex_lock(&c->reset_lock);
    bool moved = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) != seen;
    bool ready = __atomic_load_n(&c->ready, __ATOMIC_ACQUIRE);
    bool ok = ready || (moved && !__atomic_load_n(&c->called_off, __ATOMIC_ACQUIRE) ? false : enable(c));
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
        int r = described ? run(io_queue(c), cmd, IO_NS, NULL, write ? &s->blk : NULL) : -2;
        if (list) pmm_free(list);
        if (!r) return 0;
        if (r == -2) {
            kprintf("nvme%d: a buffer of %s block %lu is not mapped, or no page for its list\n", c->index,
                    write ? "write" : "read", lba);
            return -1;
        }
        if (r == FAILED) { /* a timeout: reset, whether it is given again or not, so others can go on */
            if (!recover(c, gen) || attempt == 2) return -1;
            continue;
        }
        if (attempt == 2) return -1;
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
    unsigned per = s->block / 512, most = __atomic_load_n(&c->max_xfer, __ATOMIC_ACQUIRE) / s->block;
    if (!most) return -N_ERR_IO; /* a reset found it takes less than a block now */
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

/* Flush (Base 7.2): the volatile write cache reaches the medium, the writes every queue completed
   before it with it. */
static int blk_nvme_persist(struct blkdev *b) {
    struct ns *s = b->priv;
    struct nvme *c = s->c;
    awake(c);
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t gen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
        uint32_t cmd[16] = {OP_FLUSH};
        cmd[1] = s->nsid;
        int r = run(io_queue(c), cmd, IO_NS, NULL, NULL);
        if (!r) return 0;
        if (r != FAILED || !recover(c, gen)) break;
    }
    return -N_ERR_IO;
}

/* Each message's vector on the CPU of the first queue it serves (the queues' CPUs are io_queue's). */
static void vectors_place(struct nvme *c) {
    for (unsigned v = 0; v < c->nvectors && v < c->nio; v++) {
        unsigned cpu = (unsigned)(((uint64_t)v * topology.cpus + c->nio - 1) / c->nio);
        if (cpu < topology.cpus) irq_set_cpu(c->irqs[v], cpu);
    }
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
    uint64_t cap = c->cap = rd64(c, CAP);
    c->stride = 4u << (cap >> 32 & 0xF);
    c->mqes = (uint16_t)(cap & 0xFFFF);
    unsigned to = (unsigned)(cap >> 24 & 0xFF);
    if (cap >> 59 & 3) { /* 2.x ready modes: CRTO may be longer than CAP.TO can say */
        unsigned crwmt = rd32(c, CRTO) & 0xFFFF;
        if (crwmt > to) to = crwmt;
    }
    c->ready_ns = MAX(to, 1u) * 500 * NS_PER_MS; /* 500 ms units, the field rounding down */
    /* one with no I/O command set has nothing to read or write (3.5.1 step 3.a) */
    if (cap & CAP_CSS_NOIO || !(cap & (CAP_CSS_NVM | CAP_CSS_SETS)) || (cap >> 48 & 0xF)) {
        kprintf("nvme%d: no NVM command set or no 4 KiB pages (capabilities %lx), skipped\n", c->index, cap);
        return;
    }
    if (!(c->ident_phys = pmm_alloc_node(pci->node < 0 ? 0 : (unsigned)pci->node)) ||
        !queue_alloc(&c->admin, c, 0, ADMIN_ENTRIES, pci->node < 0 ? 0 : (unsigned)pci->node))
        return;
    c->ident = phys_to_virt(c->ident_phys);
    mutex_lock(&c->reset_lock);
    bool up = enable(c);
    mutex_unlock(&c->reset_lock);
    if (!up) {
        while (c->found) {
            struct ns *s = c->found;
            c->found = s->next;
            kfree(s);
        }
        return;
    }
    vectors_place(c);

    kprintf("nvme%d: %s (%04x:%04x), version %x, %u namespaces, %u I/O queues of %u, %u %s%s\n", c->index, c->model,
            pci->vendor, pci->device, rd32(c, VS), c->namespaces, c->nio, c->io[0].depth, c->nvectors,
            !c->nvectors ? "vectors: polled" : pci_msix_max_vectors(pci) ? "MSI-X vectors" : "MSI vectors",
            c->vwc ? ", write cache" : "");
    __atomic_store_n(&c->in_use, true, __ATOMIC_RELEASE);
    for (struct ns *s = c->found; s; s = s->next) {
        ksnprintf(s->blk.name, sizeof s->blk.name, "nvme%d", __atomic_fetch_add(&ndisks, 1, __ATOMIC_RELAXED));
        strlcpy(s->blk.model, c->model, sizeof s->blk.model);
        blk_register(&s->blk);
    }
}

/* The I/O queues deleted, submission queues before completion queues (3.6.1 steps 1b, 1c; 5.3.4,
   5.3.3): a queue that does not go does not keep the shutdown from being notified. */
static void delete_queues(struct nvme *c) {
    for (int op = OP_DELETE_SQ; op >= 0; op = op == OP_DELETE_SQ ? OP_DELETE_CQ : -1)
        for (unsigned i = 0; i < c->nio; i++) {
            uint32_t cmd[16] = {(uint32_t)op};
            cmd[10] = c->io[i].qid;
            int r = run_polled(c, cmd, ADMIN_NS);
            if (r) {
                kprintf("nvme%d: deleting I/O queue %u failed (%x)\n", c->index, c->io[i].qid, r);
                return;
            }
        }
}

/* CC.SHN set: until CSTS.SHST reads complete, which is when the controller may lose power (3.6.1).
   With CSTS.ST set the whole subsystem is shutting down, by another controller's hand, and its
   complete says as much (3.6.3). The specification gives the least to wait (RTD3E, or one second when
   that is not reported) and no limit: a controller that has begun is waited for up to our own
   limit, its cache being worth more than the time. One that has not begun by the least, reads the
   reserved value (Figure 42), reported a fatal status or vanished will not finish. */
static bool shutdown_done(struct nvme *c) {
    uint64_t least = c->rtd3e ? c->rtd3e * NS_PER_US : NS_PER_S, deadline = clock_after(least);
    uint64_t last = clock_after(MAX(least, (uint64_t)SHUTDOWN_MOST_NS));
    bool told = false;
    for (;;) {
        uint32_t csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu || csts & CSTS_CFS) return false;
        unsigned shst = CSTS_SHST(csts);
        if (shst == SHST_DONE) return true;
        if (shst == SHST_RESERVED) return false;
        uint64_t now = clock_now_ns();
        if (now >= last) return false;
        if (now >= deadline) {
            if (shst == SHST_NONE) return false;
            if (!told) {
                kprintf("nvme%d: its shutdown takes longer than the %lu us it reports\n", c->index,
                        (unsigned long)(least / NS_PER_US));
                told = true;
            }
        }
        pause();
    }
}

/* Normal shutdown before power goes away (3.6.1), for every controller of the machine: SHN is to be
   written before any power down (Figure 41), whoever used it, and the drive then does not count an
   unsafe shutdown. Its I/O queues go first if it still takes commands; the notification has no such
   condition: a reset that failed left its cache as it was. One that reports complete stays ready
   to lose power (3.6.1), and one that does not stop can keep nothing; another device that came in
   its place has been given nothing, and is told as well. Only one holding block devices, given
   commands since it last lost power, keeps a power change from going on. Interrupts are already
   off, the disks held (no command in flight). */
static bool nvme_shutdown(void *arg) {
    (void)arg;
    bool all = true;
    for (struct nvme *c = ctrls; c; c = c->next) {
        /* a Function Level Reset left unfinished is not to be reached, and counted its cache lost */
        bool stranger = __atomic_load_n(&c->cut, __ATOMIC_ACQUIRE) && c->stranger;
        if (!c->regs || (__atomic_load_n(&c->cut, __ATOMIC_ACQUIRE) && !stranger) || pci_reset_left(c->pci) ||
            CSTS_SHST(rd32(c, CSTS)) == SHST_DONE)
            continue;
        if (__atomic_load_n(&c->live, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&c->ready, false, __ATOMIC_RELEASE);
            delete_queues(c);
        }
        wr32(c, CC, (rd32(c, CC) & ~CC_SHN) | CC_SHN_NORMAL);
        /* its cache may not be on the flash */
        if (!shutdown_done(c) && !stranger && __atomic_load_n(&c->in_use, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&c->woke, __ATOMIC_ACQUIRE))
            all = false;
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
