/* NVM Express SSDs (NVM Express Base Specification 2.x, PCIe transport): the driver of every PCI
   function of class 01h, subclass 08h, programming interface 02h.

   Queues: one admin queue pair, and I/O queue pairs as many as the controller gives up to one a
   CPU. Each CPU submits to its queue (the CPUs split over the queues in their order, which keeps
   the CPUs of a cache and a node together), and each queue's completion queue interrupts on a
   CPU it serves, through a message of its own where the function has as many (MSI-X), else
   sharing the messages it has, else its INTx line. A queue holds as many commands at once as its
   entries but one.

   A request chain (dev/blk.h) is put into the calling CPU's queue whole, one command after the
   other, and the doorbell is rung once. The device moves the data between itself and the caller's
   memory, described by the request's segments in PRPs, or in SGL descriptors where the controller
   has them (Base 4.1, 4.3); the completion interrupt ends each request there. A command that fails
   where trying again may help, and each one a reset ended, goes to the controller's thread, which
   gives it again (at most three times), resets a controller that has stopped answering, polls the
   completion queues where there is no interrupt, and watches every command's time.

   Every active namespace with plain blocks of any size a command can move (no metadata, no
   protection information) becomes a block device "nvmeC.N".

   眠り、休止、電源断は nvme_suspend と nvme_resume（hal/hal.h）：I/O の queue を消し、電源が去るなら正常の shutdown を
   知らせ（3.6.1）、戻りは Controller Reset から立ち上げ直す。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "arch/topology.h"
#include "dev/blk.h"
#include "dev/pci.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define ADMIN_ENTRIES 32
#define IO_ENTRIES 64            /* an I/O queue's entries: its submission queue fills one page */
#define MOST_BYTES (256u * 1024) /* one command: 65 pages with the offset in the first, a list of 64 entries */
#define LIST_BYTES 512           /* a command's PRP list (64 entries) or SGL segment (32 descriptors) */
#define SGL_SEGMENTS (LIST_BYTES / 16)
#define TRIES 3 /* a command is given at most this many times */
#define ADMIN_NS (5 * NS_PER_S)
#define IO_NS (30 * NS_PER_S)
#define TICK_NS (250 * NS_PER_MS)
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
#define PSDT_SGL (1u << 14) /* CDW0 bits 15:14 = 01b: the data pointer is an SGL (Figure 92) */
#define SGL_DATA_BLOCK 0x00u
#define SGL_LAST_SEGMENT 0x30u

#define FAILED (-1) /* timeout or a dead controller; command errors return the status field */

/* A command's place in its queue: its command identifier is the slot's index. An admin command's
   submitter waits in wq for seq to move on; an I/O command's request is ended where its
   completion is read. */
struct slot {
    struct wait_queue wq;
    uint64_t seq;                /* moves on when its command completes, or a reset fails it */
    int status;                  /* 0, the 15-bit status field, or FAILED: written before seq moves on */
    uint32_t result;             /* the completion's DW0 */
    bool busy;                   /* submitted and not completed: a reset fails it */
    struct blk_request *request; /* an I/O command's, else NULL */
    uint64_t deadline;           /* an I/O command's, on the clock */
};

struct nvme;
struct ns;
struct queue {
    struct nvme *c;
    uint16_t qid, size, depth; /* depth: the slots, size - 1 (so the submission queue never fills) */
    uint8_t *sq;
    uint64_t sq_bus;
    volatile uint32_t *cq;
    uint64_t cq_bus;
    uint8_t *lists; /* an I/O queue's: LIST_BYTES a slot, for the PRPs or the SGL segment of its command */
    uint64_t lists_bus;
    /* The submission side: the tail and the slots taken. */
    struct spinlock sq_lock;
    uint16_t tail;
    uint64_t *free;         /* a bit a free slot (atomic: a completion gives one back) */
    uint64_t freed;         /* moves on as a slot is given back (room's word) */
    struct wait_queue room; /* submitters waiting for a free slot */
    /* The completion side, taken in the interrupt handler too: the head and phase. */
    struct spinlock cq_lock;
    uint16_t head;
    uint8_t phase;
    struct slot *slots;
};

/* A message's handler context: it reaps the queues whose completion queues it is the vector of. */
struct vector {
    struct nvme *c;
    unsigned index;
};

struct nvme {
    int index;
    struct hal_device *hal;
    struct pci_device *pci;
    volatile uint8_t *regs;
    uint64_t cap;
    unsigned stride;
    unsigned max_xfer; /* bytes a command moves at most: MDTS and MOST_BYTES, as the first enable found */
    bool sgl;          /* Identify Controller SGLS: SGLs describe the data (as the first enable found) */
    uint64_t ready_ns; /* CC.EN to CSTS.RDY at most (CAP.TO, CRTO) */
    uint32_t rtd3e;    /* Identify Controller: the shutdown's microseconds, 0 not reported */
    uint32_t namespaces; /* Identify Controller: NN */
    char model[41];
    /* Which device this is (4.7.1.2): its VID, SSVID, SN and MN, and its NQN, of Identify Controller. */
    uint8_t identity[64];
    uint8_t nqn[256];
    bool stranger;     /* cut for being another device, which still answers */
    uint16_t mqes;     /* the largest queue's entries, 0-based (CAP.MQES) */
    struct queue admin;
    struct queue *io;  /* I/O queue i is queue ID i + 1 */
    unsigned nio;      /* of them with memory, as the first enable made them */
    unsigned nvectors; /* its messages (or its one INTx line); 0: polled */
    struct irq **irqs;
    struct vector *vectors;
    struct ns *found;  /* its usable namespaces, as the first enable found them */
    bool discovered;   /* found is made */
    /* live: the admin queue takes commands; ready: the I/O queues too, looked at under a queue's
       submission lock. A timeout or a fatal status clears them until the reset that follows
       (recover). generation: moves on at each enable, so that of the callers whose commands failed
       only the first resets the controller. */
    bool live, ready, vwc;
    uint64_t generation;
    /* One stop at a time (controller_stop), taken inside reset_lock and alone by a command that
       ran out of time. stops moves on when one has ended; stopped: the last one's end, its bus
       mastering off if false; cut: the controller does not stop, and it stays out of use. */
    struct mutex stop_lock;
    uint64_t stops;
    bool stopped;
    bool called_off; /* the last stop only gave up, its thread being killed (atomic) */
    bool cut;
    struct mutex reset_lock; /* enable: at start, after a sleep, after a timeout */
    /* 眠り（nvme_suspend から nvme_resume まで）：データの道は閉じ、誰も立ち上げない。reset_lock の下で書き、錠なしで
       読む（atomic）。間に来た要求は retry の列で待ち、resume の work_wake で thread が出す。 */
    bool asleep;
    uint8_t *ident;          /* a page for identify, at start */
    uint64_t ident_bus;
    /* The commands to give again (a failed try, a stop that ended them, a chain the queue could not
       take): the thread takes them. work moves on when there is something for it to look at. */
    struct spinlock retry_lock;
    struct blk_request *retry, **retry_tail;
    uint64_t work;
    struct wait_queue work_wq;
    unsigned inflight; /* I/O commands given and not ended (atomic) */
    bool stopping;     /* detach: the thread ends (atomic) */
};

struct ns {
    struct blkdev blk;
    struct nvme *c;
    struct ns *next;
    uint32_t nsid;
    unsigned block;
};

static int nctrls; /* atomic: the controllers' numbers */

static void nvme_submit(struct blkdev *d, struct blk_request *chain);
static const struct blk_ops nvme_ops = {.submit = nvme_submit};
static void health_check(struct nvme *c);

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

static struct ns *ns_of(const struct blk_request *r) { return r->device->priv; }

static void work_wake(struct nvme *c) {
    __atomic_add_fetch(&c->work, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&c->work_wq);
}

/* ---- the commands to give again ---- */
/* The chain's commands go to the thread, which gives them again, or ends them. Any context. */
static void retry_push(struct nvme *c, struct blk_request *chain) {
    struct blk_request **tail = &chain;
    while (*tail) tail = &(*tail)->next;
    uint64_t f = spin_lock(&c->retry_lock);
    *c->retry_tail = chain;
    c->retry_tail = tail;
    spin_unlock(&c->retry_lock, f);
    work_wake(c);
}

/* ---- queues ---- */
static bool queue_up(const struct queue *q) {
    return __atomic_load_n(q == &q->c->admin ? &q->c->live : &q->c->ready, __ATOMIC_ACQUIRE);
}

/* A slot given back, free to be taken again at once. Any context. */
static void slot_give(struct queue *q, unsigned id) {
    __atomic_fetch_or(&q->free[id / 64], 1ull << (id % 64), __ATOMIC_RELEASE);
    __atomic_add_fetch(&q->freed, 1, __ATOMIC_RELEASE);
    wq_wake_all(&q->room);
}

/* sq_lock held: a free slot taken, -1 when none. */
static int slot_take(struct queue *q) {
    for (unsigned w = 0; w < (q->depth + 63u) / 64; w++) {
        uint64_t bits = __atomic_load_n(&q->free[w], __ATOMIC_ACQUIRE);
        if (!bits) continue;
        uint64_t bit = bits & -bits;
        __atomic_fetch_and(&q->free[w], ~bit, __ATOMIC_ACQ_REL);
        return (int)(w * 64 + (unsigned)__builtin_ctzll(bit));
    }
    return -1;
}

/* A failed command that no other try can mend: DNR (Do Not Retry) or a media and data integrity error. */
static bool retriable(unsigned status) { return !(status >> 14 & 1) && (status >> 8 & 7) != 2; }

static const char *op_name(enum blk_op op) { return op == BLK_WRITE ? "write" : op == BLK_READ ? "read" : "flush"; }

/* cq_lock held: every new completion entry ends its slot's wait, or an I/O request: one that did
   its work, or has had its tries, goes on *done; one that failed where it may be given again goes
   to the controller's thread. True when an entry was there. */
static bool reap(struct queue *q, struct blk_request **done) {
    struct nvme *c = q->c;
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
        unsigned status = dw3 >> 17 & 0x7FFF;
        s->busy = false;
        struct blk_request *r = s->request;
        if (!r) {
            s->status = (int)status;
            s->result = dw0;
            __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
            wq_wake_all(&s->wq);
            continue;
        }
        s->request = NULL;
        if (r->op == BLK_WRITE) blk_write_ended(&ns_of(r)->blk);
        __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE);
        slot_give(q, id);
        if (!status) {
            r->result = 0;
            r->next = *done;
            *done = r;
            continue;
        }
        bool again = retriable(status) && r->tries < TRIES;
        kprintf("nvme%d: %s of block %lu failed (type %u code %02x)%s\n", c->index, op_name(r->op), r->lba,
                status >> 8 & 7, status & 0xFF, again ? ", again" : "");
        if (again) {
            r->next = NULL;
            retry_push(c, r);
        } else {
            r->result = -N_ERR_IO;
            r->next = *done;
            *done = r;
        }
    }
    if (moved && queue_up(q)) doorbell(q->c, q->qid, true, q->head); /* else the stop's queue_clear empties it */
    return moved;
}

/* The requests reap gathered, ended with the lock given back. */
static void done_run(struct blk_request *done) {
    while (done) {
        struct blk_request *next = done->next;
        done->next = NULL;
        blk_complete(done, done->result);
        done = next;
    }
}

/* 割り込みの無い controller の完了を読む。reap は記憶だけを読み、doorbell は queue が上がっているときだけ書くので、
   下りた queue（止める途中、眠りの前の io_drain）も読んでよい。 */
static void poll_queue(struct queue *q) {
    struct blk_request *done = NULL;
    uint64_t f = spin_lock(&q->cq_lock);
    reap(q, &done);
    spin_unlock(&q->cq_lock, f);
    done_run(done);
}

static enum irq_result nvme_isr(void *context) {
    struct vector *v = context;
    struct nvme *c = v->c;
    struct blk_request *done = NULL;
    bool moved = false;
    if (!v->index) {
        spin_take(&c->admin.cq_lock);
        moved |= reap(&c->admin, &done);
        spin_give(&c->admin.cq_lock);
    }
    unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE), step = MAX(c->nvectors, 1u);
    for (unsigned i = v->index; i < nio; i += step) {
        spin_take(&c->io[i].cq_lock);
        moved |= reap(&c->io[i], &done);
        spin_give(&c->io[i].cq_lock);
    }
    done_run(done);
    return moved ? IRQ_HANDLED : IRQ_NONE;
}

/* The controller off (controller_stop), or a new queue: the queue empty again, every command in it
   over. A caller waiting on one has it failed, each one's slot given back by its waiter; an I/O
   request is to be given again. The submission lock first: a submitter is either through it (its
   slot busy, failed here) or finds the controller down. */
static void queue_clear(struct queue *q) {
    struct nvme *c = q->c;
    struct blk_request *again = NULL, **tail = &again;
    uint64_t f = spin_lock(&q->sq_lock);
    spin_take(&q->cq_lock);
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE); /* phase 0: the controller's first pass writes 1 */
    q->tail = q->head = 0;
    q->phase = 1;
    for (unsigned i = 0; i < q->depth; i++) {
        struct slot *s = &q->slots[i];
        if (!s->busy) continue;
        s->busy = false;
        if (s->request) {
            struct blk_request *r = s->request;
            s->request = NULL;
            if (r->op == BLK_WRITE) blk_write_ended(&ns_of(r)->blk); /* nothing of it moves any more */
            __atomic_sub_fetch(&c->inflight, 1, __ATOMIC_RELEASE);
            *tail = r;
            tail = &r->next;
            continue;
        }
        s->status = FAILED;
        __atomic_add_fetch(&s->seq, 1, __ATOMIC_RELEASE);
        wq_wake_all(&s->wq);
    }
    for (unsigned w = 0; w < (q->depth + 63u) / 64; w++) q->free[w] = ~0ull;
    if (q->depth % 64) q->free[q->depth / 64] = (1ull << (q->depth % 64)) - 1;
    spin_give(&q->cq_lock);
    spin_unlock(&q->sq_lock, f);
    *tail = NULL;
    if (again) retry_push(c, again);
    __atomic_add_fetch(&q->freed, 1, __ATOMIC_RELEASE);
    wq_wake_all(&q->room);
}

/* Whoever was in the queue's locks has gone. */
static void queue_pass(struct queue *q) {
    spin_unlock(&q->sq_lock, spin_lock(&q->sq_lock));
    spin_unlock(&q->cq_lock, spin_lock(&q->cq_lock));
}

/* Memory for a queue of `entries`, once; enable clears it each time. An I/O queue has the lists its
   commands' data pointers are built in. False without memory. */
static bool queue_alloc(struct queue *q, struct nvme *c, uint16_t qid, uint16_t entries, bool lists) {
    q->c = c;
    q->qid = qid;
    q->size = entries;
    q->depth = (uint16_t)(entries - 1);
    if (!(q->sq = hal_dma_alloc(c->hal, PAGE_SIZE, 0, &q->sq_bus)) ||
        !(q->cq = hal_dma_alloc(c->hal, PAGE_SIZE, 0, &q->cq_bus)))
        return false;
    if (lists && !(q->lists = hal_dma_alloc(c->hal, (size_t)q->depth * LIST_BYTES, 0, &q->lists_bus))) return false;
    q->slots = kzalloc(q->depth * sizeof *q->slots);
    q->free = kzalloc((q->depth + 63) / 64 * sizeof *q->free);
    if (!q->slots || !q->free) return false;
    queue_clear(q);
    return true;
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
   it held moves any more, so every command in it is over and its memory may be taken back; the I/O
   requests among them go to be given again. A shutdown it may have notified ends with this
   write: SHN is written back as 00b, or the same write would begin another (Figure 41). EN does not
   fall while RDY reads 0, which is undefined (Figure 41): RDY is given its time to rise, whatever
   else the status says. A controller where this cannot be done, or does not end, or stays fatal,
   gets the other Controller Level Reset, the Function Level Reset (3.7.2). The reset of a healthy
   controller loses nothing of what it completed (Figure 41, EN); nothing says that of a fatal
   status, of one gone from the bus, of a Function Level Reset or of EN found 0 while it was in use,
   a reset no one here asked for (3.7.2), and its cache may be gone. One that still does not stop
   has its bus mastering turned off, and stays out of use with its namespaces. If the reset was only
   called off, its thread being killed (dev/pci.h), the next request goes on with it. A Function
   Level Reset left unfinished is not to be reached by anything but its next call, and has its bus
   mastering off already (dev/pci.h, 6.6.2): neither the registers nor the configuration are
   touched before that; nor does anyone that read the queue up go on past its locks (run, reap).
   `seen`: c->stops as the caller read it; a stop that ended since has done this for it. What the
   last stop came to is returned. */
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
                hal_failed(c->hal, -N_ERR_IO);
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

/* Enters a command on q in the free slot `id` (sq_lock held): the entry only, the doorbell is the
   caller's. */
static void enter(struct queue *q, int id, uint32_t cmd[16]) {
    q->slots[id].busy = true;
    cmd[0] = (cmd[0] & 0xFFFF) | (uint32_t)id << 16; /* the command identifier; PSDT stays, not fused */
    memcpy(q->sq + q->tail * 64u, cmd, 64);
    q->tail = (uint16_t)((q->tail + 1) % q->size);
}

/* ---- admin commands ---- */
/* Submit a command on the admin queue and wait for its completion. Returns 0, the 15-bit status
   field of a failed command, or FAILED when the controller stopped answering. Then it has been
   stopped, and nothing of the command moves any more: the caller may take its memory back. It is
   down until a reset. Waiting for a free slot takes no time from the command: every command in the
   queue ends, or stops the controller, within its own time. */
static int run(struct nvme *c, uint32_t cmd[16], uint64_t timeout_ns, uint32_t *result) {
    struct queue *q = &c->admin;
    int id;
    uint64_t f;
    for (;;) {
        f = spin_lock(&q->sq_lock);
        if (!__atomic_load_n(&c->live, __ATOMIC_ACQUIRE)) {
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
    enter(q, id, cmd);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* the entry before the doorbell */
    doorbell(c, q->qid, false, q->tail);
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
        struct blk_request *none = NULL;
        f = spin_lock(&q->cq_lock);
        bool up = queue_up(q);
        if (up && !c->nvectors) reap(q, &none);
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
    for (unsigned i = 0; i < want; i++)
        if (!queue_alloc(&io[i], c, (uint16_t)(i + 1), entries, true)) return false;
    c->io = io;
    __atomic_store_n(&c->nio, want, __ATOMIC_RELEASE);
    return true;
}

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid) {
    uint32_t cmd[16] = {OP_IDENTIFY};
    cmd[1] = nsid;
    *(uint64_t *)&cmd[6] = c->ident_bus;
    cmd[10] = cns;
    memset(c->ident, 0, PAGE_SIZE);
    return run(c, cmd, ADMIN_NS, NULL);
}

/* A controller that is not what it was, or takes less: cut, and nothing is written to it. */
static void cut_off(struct nvme *c, const char *why) {
    kprintf("nvme%d: %s, turned off\n", c->index, why);
    pci_disable_busmaster(c->pci, NULL);
    c->stranger = true;
    __atomic_store_n(&c->cut, true, __ATOMIC_RELEASE);
    for (struct ns *s = c->found; s; s = s->next) __atomic_store_n(&s->blk.gone, true, __ATOMIC_RELEASE);
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
    c->rtd3e = *(const uint32_t *)(id + 88); /* what its shutdown takes, whichever device it is */
    /* Not the device it was: its namespaces are not the ones here, and nothing is written to it. */
    if (c->discovered && (memcmp(c->identity, id, sizeof c->identity) || memcmp(c->nqn, id + 768, sizeof c->nqn))) {
        cut_off(c, "another device is here than before");
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
    unsigned mdts = id[77], most = MOST_BYTES;
    if (mdts && mdts < 20 && (PAGE_SIZE << mdts) < most) most = PAGE_SIZE << mdts;
    bool sgl = id[536] & 3; /* SGLS: supported, with or without dword alignment (Figure 338) */
    /* the block devices were made from what the first enable found: a controller that now takes less
       or describes data otherwise is not the one they are for */
    if (c->discovered && (most < c->max_xfer || sgl != c->sgl)) {
        cut_off(c, "its transfer limits changed");
        return false;
    }
    if (!c->discovered) {
        c->max_xfer = most;
        c->sgl = sgl;
    }
    c->vwc = id[525] & 1;
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
    *(uint64_t *)&cmd[6] = c->ident_bus;
    cmd[10] = (uint32_t)CNTID_THIS << 16 | CNS_COMMAND_SETS;
    memset(c->ident, 0, PAGE_SIZE);
    int r = run(c, cmd, ADMIN_NS, NULL);
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
    if ((r = run(c, cmd, ADMIN_NS, NULL)) == FAILED) return false;
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
    s->blk.block = s->block;
    s->blk.max_sectors = c->max_xfer / 512;
    s->blk.max_segments = c->sgl ? SGL_SEGMENTS : 0; /* PRPs: the pages of the transfer, which its size bounds */
    s->blk.segment_align = 4;                        /* a data pointer is dword aligned */
    /* PRPs: every entry after the first starts a page, every one before the last ends one (4.3.1) */
    s->blk.segment_boundary = c->sgl ? 0 : PAGE_SIZE;
    s->blk.ops = &nvme_ops;
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
   queue takes one in turn. A function with none has its INTx line, one vector for all the queues. */
static void vectors_enable(struct nvme *c) {
    unsigned want = MIN(MAX(hal_irq_max(c->hal), 1u), c->nio);
    struct irq_spec *specs = kzalloc(want * sizeof *specs);
    c->irqs = kzalloc(want * sizeof *c->irqs);
    c->vectors = kzalloc(want * sizeof *c->vectors);
    if (!specs || !c->irqs || !c->vectors) {
        kfree(specs);
        return;
    }
    for (unsigned i = 0; i < want; i++) {
        c->vectors[i] = (struct vector){c, i};
        specs[i] = (struct irq_spec){.name = "NVMe", .handler = nvme_isr, .context = &c->vectors[i]};
    }
    c->nvectors = hal_irq_enable(c->hal, specs, want, c->irqs);
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
        !(__atomic_load_n(&c->called_off, __ATOMIC_ACQUIRE) &&
          controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE))))
        return false;
    hal_enable(c->hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA); /* a Function Level Reset's configuration comes back without it */
    wr32(c, AQA, (ADMIN_ENTRIES - 1u) << 16 | (ADMIN_ENTRIES - 1u));
    wr64(c, ASQ, c->admin.sq_bus);
    wr64(c, ACQ, c->admin.cq_bus);
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
    r = run(c, cmd, ADMIN_NS, &got);
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
        *(uint64_t *)&cmd[6] = q->cq_bus;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        /* its vector, interrupts on when there are any, contiguous */
        cmd[11] = (c->nvectors ? (i % c->nvectors) << 16 | 2u : 0u) | 1u;
        if ((r = run(c, cmd, ADMIN_NS, NULL))) goto fail;
    }
    for (unsigned i = 0; i < c->nio; i++) { /* step 11: the submission queues */
        struct queue *q = &c->io[i];
        memset(cmd, 0, sizeof cmd);
        cmd[0] = OP_CREATE_SQ;
        *(uint64_t *)&cmd[6] = q->sq_bus;
        cmd[10] = (uint32_t)(q->size - 1) << 16 | q->qid;
        cmd[11] = (uint32_t)q->qid << 16 | 1u; /* its completion queue, contiguous */
        if ((r = run(c, cmd, ADMIN_NS, NULL))) goto fail;
    }
    __atomic_store_n(&c->ready, true, __ATOMIC_RELEASE);
    return true;
fail:
    kprintf("nvme%d: setting up the I/O queues failed (%x)\n", c->index, r);
down:
    controller_down(c);
    return false;
}

/* reset_lock held. */
static bool enable_recorded(struct nvme *c) {
    bool up = enable(c);
    if (up) hal_recovered(c->hal);
    else if (!__atomic_load_n(&c->cut, __ATOMIC_ACQUIRE)) hal_failed(c->hal, -N_ERR_IO);
    return up;
}

/* 命令が FAILED で終わった後、または controller が下りているとき。もう上がっていれば true（その間に reset が済んだ）。
   でなければ最初に来た者（generation がまだ seen）が一度だけ立ち上げ直し、後の者は上がらなかったと知る。立ち上げ
   直す者の thread が kill で諦めたなら、後の者が続ける。眠りの間（asleep）は立ち上げず false。 */
static bool recover(struct nvme *c, uint64_t seen) {
    mutex_lock(&c->reset_lock);
    bool moved = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE) != seen;
    bool ready = __atomic_load_n(&c->ready, __ATOMIC_ACQUIRE);
    bool ok = !c->asleep &&
              (ready || (moved && !__atomic_load_n(&c->called_off, __ATOMIC_ACQUIRE) ? false : enable_recorded(c)));
    mutex_unlock(&c->reset_lock);
    return ok;
}

/* The calling CPU's I/O queue. */
static struct queue *io_queue(struct nvme *c) {
    unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE);
    return &c->io[(uint64_t)cpu_current_index() * nio / topology.cpus];
}

/* ---- requests ---- */
/* The data pointer of r in cmd (DW6 to DW9), built in `list` (at list_bus) where it needs one: the
   PRPs of its pages (4.3.1: PRP1 at its offset in the first page, PRP2 the second page, or the list
   of the rest), or its SGL (4.3.2: one Data Block descriptor, or a Last Segment descriptor over a
   list of them). The segments are physical; the bus address of each adds the device's offset. */
static void data_pointer(const struct nvme *c, const struct blk_request *r, uint32_t cmd[16], uint64_t *list,
                         uint64_t list_bus) {
    const struct hal_device *h = c->hal;
    if (c->sgl) {
        cmd[0] |= PSDT_SGL;
        if (r->segment_count == 1) {
            *(uint64_t *)&cmd[6] = hal_dma_address(h, r->segments[0].phys);
            cmd[8] = r->segments[0].bytes;
            cmd[9] = SGL_DATA_BLOCK << 24;
            return;
        }
        for (uint32_t i = 0; i < r->segment_count; i++) {
            list[2 * i] = hal_dma_address(h, r->segments[i].phys);
            list[2 * i + 1] = r->segments[i].bytes | (uint64_t)SGL_DATA_BLOCK << 56;
        }
        *(uint64_t *)&cmd[6] = list_bus;
        cmd[8] = r->segment_count * 16;
        cmd[9] = SGL_LAST_SEGMENT << 24;
        return;
    }
    *(uint64_t *)&cmd[6] = hal_dma_address(h, r->segments[0].phys);
    unsigned pages = 0;
    uint64_t second = 0;
    for (uint32_t i = 0; i < r->segment_count; i++) {
        uint64_t at = r->segments[i].phys & ~(uint64_t)(PAGE_SIZE - 1), end = r->segments[i].phys + r->segments[i].bytes;
        for (; at < end; at += PAGE_SIZE, pages++) {
            if (pages == 1) second = hal_dma_address(h, at);
            if (pages >= 1) list[pages - 1] = hal_dma_address(h, at);
        }
    }
    if (pages == 2) *(uint64_t *)&cmd[8] = second;
    else if (pages > 2) *(uint64_t *)&cmd[8] = list_bus;
}

/* The command of r in the free slot `id` of q (sq_lock held), entered; no doorbell. */
static void command_enter(struct queue *q, int id, struct blk_request *r) {
    struct nvme *c = q->c;
    struct ns *n = ns_of(r);
    uint32_t cmd[16] = {r->op == BLK_READ ? OP_READ : r->op == BLK_WRITE ? OP_WRITE : OP_FLUSH};
    cmd[1] = n->nsid;
    if (r->op != BLK_PERSIST) {
        unsigned per = n->block / 512;
        uint64_t lba = r->lba / per;
        cmd[10] = (uint32_t)lba;
        cmd[11] = (uint32_t)(lba >> 32);
        cmd[12] = r->sectors / per - 1;
        data_pointer(c, r, cmd, (uint64_t *)(q->lists + (size_t)id * LIST_BYTES), q->lists_bus + (uint64_t)id * LIST_BYTES);
    }
    q->slots[id].request = r;
    q->slots[id].deadline = clock_after(IO_NS);
    r->tries++;
    if (r->op == BLK_WRITE) blk_write_given(&n->blk);
    __atomic_add_fetch(&c->inflight, 1, __ATOMIC_ACQ_REL);
    enter(q, id, cmd);
}

/* The commands for the requests of `chain` in the calling CPU's queue, one doorbell for as many as
   fit. A request waits for a free slot, never for another to end; once the controller is down the
   rest of the chain is left to the thread, which brings it up. Where nothing interrupts, the wait
   for a slot reads the completions itself. */
static void queue_chain(struct nvme *c, struct blk_request *chain) {
    struct queue *q = io_queue(c);
    while (chain) {
        uint64_t f = spin_lock(&q->sq_lock);
        bool up = __atomic_load_n(&c->ready, __ATOMIC_ACQUIRE);
        unsigned count = 0;
        while (chain && up) {
            int id = slot_take(q);
            if (id < 0) break;
            struct blk_request *r = chain;
            chain = r->next;
            r->next = NULL;
            command_enter(q, id, r);
            count++;
        }
        if (count) {
            __atomic_thread_fence(__ATOMIC_RELEASE); /* the entries before the doorbell */
            doorbell(c, q->qid, false, q->tail);
        }
        uint64_t seen = __atomic_load_n(&q->freed, __ATOMIC_ACQUIRE);
        spin_unlock(&q->sq_lock, f);
        if (!chain) return;
        if (!up) {
            retry_push(c, chain);
            return;
        }
        if (!c->nvectors) poll_queue(q);
        wq_wait_unless(&q->room, &q->freed, seen, clock_after(c->nvectors ? TICK_NS : NS_PER_MS));
        health_check(c); /* a controller that stopped answering frees the queue by its reset */
    }
}

static void nvme_submit(struct blkdev *d, struct blk_request *chain) {
    struct nvme *c = ((struct ns *)d->priv)->c;
    if (__atomic_load_n(&c->ready, __ATOMIC_ACQUIRE) || recover(c, __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE))) {
        queue_chain(c, chain);
        return;
    }
    if (__atomic_load_n(&c->asleep, __ATOMIC_ACQUIRE)) { /* resume の後に thread が出す */
        retry_push(c, chain);
        return;
    }
    while (chain) { /* the controller does not come up */
        struct blk_request *next = chain->next;
        chain->next = NULL;
        blk_complete(chain, -N_ERR_IO);
        chain = next;
    }
}

/* ---- the controller's thread ---- */
/* 出し直しを待つ命令。試しを使い切ったもの、使えない controller のものは終わり、残りは queue へ（下りた controller は
   立ち上げ直してから）。眠りの間は待たせ、resume の work_wake で呼び直される。 */
static void retries_run(struct nvme *c) {
    if (__atomic_load_n(&c->asleep, __ATOMIC_ACQUIRE)) return;
    uint64_t f = spin_lock(&c->retry_lock);
    struct blk_request *chain = c->retry;
    c->retry = NULL;
    c->retry_tail = &c->retry;
    spin_unlock(&c->retry_lock, f);
    if (!chain) return;
    bool cut = __atomic_load_n(&c->cut, __ATOMIC_ACQUIRE);
    if (!cut && !__atomic_load_n(&c->ready, __ATOMIC_ACQUIRE) &&
        !recover(c, __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE))) {
        if (__atomic_load_n(&c->asleep, __ATOMIC_ACQUIRE)) { /* 取った後に眠った */
            retry_push(c, chain);
            return;
        }
        cut = true;
    }
    struct blk_request *again = NULL, **tail = &again;
    for (struct blk_request *r = chain, *next; r; r = next) {
        next = r->next;
        r->next = NULL;
        if (cut || r->tries >= TRIES || __atomic_load_n(&ns_of(r)->blk.gone, __ATOMIC_ACQUIRE)) {
            blk_complete(r, -N_ERR_IO);
        } else {
            *tail = r;
            tail = &r->next;
        }
    }
    if (again) queue_chain(c, again);
}

/* A command that has had its time, or a controller that says it failed: reset, the commands in it
   given again. Only where commands are in the controller. */
static void health_check(struct nvme *c) {
    if (!__atomic_load_n(&c->ready, __ATOMIC_ACQUIRE) || !__atomic_load_n(&c->inflight, __ATOMIC_ACQUIRE)) return;
    bool late = false;
    uint64_t now = clock_now_ns();
    unsigned nio = __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < nio && !late; i++) {
        struct queue *q = &c->io[i];
        uint64_t f = spin_lock(&q->cq_lock);
        for (unsigned k = 0; k < q->depth && !late; k++)
            late = q->slots[k].busy && q->slots[k].request && q->slots[k].deadline <= now;
        spin_unlock(&q->cq_lock, f);
    }
    uint32_t csts = pci_reset_left(c->pci) ? 0 : rd32(c, CSTS);
    if (!late && csts != 0xFFFFFFFFu && !(csts & CSTS_CFS)) return;
    if (late) kprintf("nvme%d: a command timed out\n", c->index);
    else kprintf("nvme%d: controller %s\n", c->index, csts == 0xFFFFFFFFu ? "vanished" : "reported a fatal error");
    uint64_t gen = __atomic_load_n(&c->generation, __ATOMIC_ACQUIRE);
    controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE)); /* every command in it goes to retries */
    recover(c, gen);
}

static void nvme_thread(void *arg) {
    struct nvme *c = arg;
    while (!__atomic_load_n(&c->stopping, __ATOMIC_ACQUIRE)) {
        uint64_t seen = __atomic_load_n(&c->work, __ATOMIC_SEQ_CST);
        hal_busy_begin();
        if (!c->nvectors)
            for (unsigned i = 0; i < __atomic_load_n(&c->nio, __ATOMIC_ACQUIRE); i++) poll_queue(&c->io[i]);
        retries_run(c);
        health_check(c);
        hal_busy_end();
        bool polled = !c->nvectors && __atomic_load_n(&c->inflight, __ATOMIC_ACQUIRE);
        wq_wait_unless(&c->work_wq, &c->work, seen, clock_after(polled ? NS_PER_MS : TICK_NS));
    }
}

/* Each message's vector on the CPU of the first queue it serves (the queues' CPUs are io_queue's). */
static void vectors_place(struct nvme *c) {
    for (unsigned v = 0; v < c->nvectors && v < c->nio; v++) {
        unsigned cpu = (unsigned)(((uint64_t)v * topology.cpus + c->nio - 1) / c->nio);
        if (cpu < topology.cpus) irq_set_cpu(c->irqs[v], cpu);
    }
}

static int start(struct nvme *c) {
    struct hal_device *h = c->hal;
    /* the registers and the doorbells of every queue pair it has, all in its BAR */
    c->regs = hal_map(h, 0);
    if (!c->regs) {
        kprintf("nvme%d: no register window\n", c->index);
        return -N_ERR_NOT_FOUND;
    }
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
        return -N_ERR_UNSUPPORTED;
    }
    if (!(c->ident = hal_dma_alloc(h, PAGE_SIZE, 0, &c->ident_bus)) ||
        !queue_alloc(&c->admin, c, 0, ADMIN_ENTRIES, false))
        return -N_ERR_NO_MEMORY;
    mutex_lock(&c->reset_lock);
    bool up = enable(c);
    mutex_unlock(&c->reset_lock);
    if (!up) {
        while (c->found) {
            struct ns *s = c->found;
            c->found = s->next;
            kfree(s);
        }
        return -N_ERR_IO;
    }
    vectors_place(c);
    hal_name(h, c->model);

    kprintf("nvme%d: %s (%04x:%04x), version %x, %u namespaces, %u I/O queues of %u, %u %s%s, %s\n", c->index, c->model,
            c->pci->vendor, c->pci->device, rd32(c, VS), c->namespaces, c->nio, c->io[0].depth, c->nvectors,
            !c->nvectors ? "vectors: polled" : pci_msix_max_vectors(c->pci) ? "MSI-X vectors" : "vectors",
            c->vwc ? ", write cache" : "", c->sgl ? "SGL" : "PRP");
    for (struct ns *s = c->found; s; s = s->next) {
        ksnprintf(s->blk.name, sizeof s->blk.name, "nvme%d.%u", c->index, s->nsid);
        strlcpy(s->blk.model, c->model, sizeof s->blk.model);
        s->blk.has_cache = c->vwc;
        blk_register(h, &s->blk);
    }
    return 0;
}

static int nvme_attach(struct hal_device *d) {
    struct nvme *c = kzalloc(sizeof *c);
    if (!c) {
        kprintf("nvme: no memory for the controller at %s\n", d->location);
        return -N_ERR_NO_MEMORY;
    }
    c->index = __atomic_fetch_add(&nctrls, 1, __ATOMIC_ACQ_REL);
    c->hal = d;
    c->pci = pci_device_of(d);
    c->retry_tail = &c->retry;
    d->driver_data = c;
    char name[16];
    ksnprintf(name, sizeof name, "nvme%d", c->index);
    if (!kthread_create(name, nvme_thread, c)) {
        kprintf("nvme%d: no thread for it\n", c->index);
        return -N_ERR_NO_MEMORY;
    }
    hal_enable(d, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
    int r = start(c); /* CC.EN may take CAP.TO (127.5 s): this thread is the device's own */
    if (r < 0) {
        __atomic_store_n(&c->stopping, true, __ATOMIC_RELEASE);
        work_wake(c);
    }
    return r;
}

/* The function leaves, or is given to a program: the controller reset so it moves nothing, every
   command it had ended. The block devices stay listed, gone; the controller's memory stays too, for
   what still points to it. */
static void nvme_detach(struct hal_device *d) {
    struct nvme *c = d->driver_data;
    __atomic_store_n(&c->stopping, true, __ATOMIC_RELEASE);
    for (struct ns *s = c->found; s; s = s->next) __atomic_store_n(&s->blk.gone, true, __ATOMIC_RELEASE);
    work_wake(c);
    mutex_lock(&c->reset_lock);
    controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE));
    __atomic_store_n(&c->cut, true, __ATOMIC_RELEASE);
    mutex_unlock(&c->reset_lock);
    uint64_t f = spin_lock(&c->retry_lock);
    struct blk_request *chain = c->retry;
    c->retry = NULL;
    c->retry_tail = &c->retry;
    spin_unlock(&c->retry_lock, f);
    while (chain) {
        struct blk_request *next = chain->next;
        chain->next = NULL;
        blk_complete(chain, -N_ERR_IO);
        chain = next;
    }
}

/* ---- 眠り、休止、電源断（hal/hal.h の suspend と resume）----
   どちらも電源の walk の thread で、割り込みは開き、ほかの CPU も thread も動いている。controller の thread は門
   （hal_busy_begin）で待つので、完了を読むのも時間を見るのもここの仕事。データの道は asleep と ready で閉じる。 */

/* ready を下ろした後、出ている I/O の命令の終わり（3.6.1 の 1a）。割り込みの無い controller は自分で読む。時間の内に
   終わらない命令は、続く queue の削除か Controller Reset が終える。 */
static void io_drain(struct nvme *c) {
    uint64_t deadline = clock_after(IO_NS);
    while (__atomic_load_n(&c->inflight, __ATOMIC_ACQUIRE) && clock_now_ns() < deadline) {
        for (unsigned i = 0; !c->nvectors && i < c->nio; i++) poll_queue(&c->io[i]);
        sleep_until(clock_after(NS_PER_MS));
    }
}

/* I/O の queue を消す命令（op は OP_DELETE_SQ か OP_DELETE_CQ）を全部の queue に。一つでも成らなければ false。 */
static bool queues_delete(struct nvme *c, uint32_t op) {
    for (unsigned i = 0; i < c->nio; i++) {
        uint32_t cmd[16] = {op};
        cmd[10] = c->io[i].qid;
        int r = run(c, cmd, ADMIN_NS, NULL);
        if (!r) continue;
        kprintf("nvme%d: deleting I/O %s queue %u failed (%x)\n", c->index,
                op == OP_DELETE_SQ ? "submission" : "completion", c->io[i].qid, r);
        return false;
    }
    return true;
}

/* I/O の命令を止める（3.6.1 の 1）。ready を下ろして各 queue の提出の錠を通れば、新しい命令は入らない（queue_chain は
   retry の列へ回す）。出ている命令の終わりを待ち、提出の queue を全部、続けて完了の queue を全部消す（5.3.4、5.3.3：
   完了の queue は、それを使う提出の queue が消えてから）。消えた提出の queue の、完了の項の無い命令は暗に終わった
   （5.3.4）：queue_clear が retry の列へ回す。消せない queue があれば、Controller Reset が全部の命令を終える（3.7.2）。
   下りた controller（live でない）の queue は、下ろした stop か立ち上げの失敗の後で、命令を持たない。
   reset_lock の下。 */
static void io_stop(struct nvme *c) {
    __atomic_store_n(&c->ready, false, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < c->nio; i++) queue_pass(&c->io[i]);
    io_drain(c);
    if (!__atomic_load_n(&c->live, __ATOMIC_ACQUIRE)) return;
    if (queues_delete(c, OP_DELETE_SQ) && queues_delete(c, OP_DELETE_CQ)) {
        for (unsigned i = 0; i < c->nio; i++) queue_clear(&c->io[i]);
        return;
    }
    /* run が時間切れで止めたなら、もう下りている */
    if (__atomic_load_n(&c->live, __ATOMIC_ACQUIRE)) controller_stop(c, __atomic_load_n(&c->stops, __ATOMIC_ACQUIRE));
}

/* 正常の shutdown（3.6.1 の 2）：CC.SHN を 01b にし（EN はそのまま。EN が 0 の controller も shutdown を処理する、
   Figure 85）、CSTS.SHST が完了（10b）を言うまで待つ。待つのは少なくとも RTD3E（0 なら 1 秒）で、上限は仕様に無い。
   始まった shutdown は我々の上限まで待つ（cache は時間より重い）。その時までに始まらない、予約の値（Figure 42）、
   致命の状態、消えた controller は終わらない。CSTS.ST が 1 なら NVM subsystem の shutdown で、その完了も電源を切って
   よいと言う（3.6.3）。 */
static bool shutdown_notify(struct nvme *c) {
    uint32_t csts = rd32(c, CSTS);
    if (csts == 0xFFFFFFFFu) {
        kprintf("nvme%d: vanished before its shutdown\n", c->index);
        return false;
    }
    if (CSTS_SHST(csts) == SHST_DONE) return true;
    wr32(c, CC, (rd32(c, CC) & ~CC_SHN) | CC_SHN_NORMAL);
    uint64_t least = c->rtd3e ? (uint64_t)c->rtd3e * NS_PER_US : NS_PER_S;
    uint64_t begun_by = clock_after(least), last = clock_after(MAX(least, (uint64_t)SHUTDOWN_MOST_NS));
    for (;;) {
        csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu || csts & CSTS_CFS) break;
        unsigned shst = CSTS_SHST(csts);
        if (shst == SHST_DONE) return true;
        uint64_t now = clock_now_ns();
        if (shst == SHST_RESERVED || now >= last || (shst == SHST_NONE && now >= begun_by)) break;
        sleep_until(clock_after(NS_PER_MS));
    }
    kprintf("nvme%d: its shutdown did not complete (status %x)\n", c->index, csts);
    return false;
}

/* データの道を閉じ（asleep）、I/O の queue を消す（io_stop）。電源が去る SLEEP と OFF では続けて正常の shutdown を
   知らせる（CC.SHN は電源が去る前と PCI の電源の状態が変わる前に書く、Figure 41。D3 に入る前も、3.6.1）。IMAGE では
   電源も PCI の状態も変わらず、I/O の queue の無い controller は何も動かさない。止めた controller（cut）は BME が
   切れて何も動かさないが、別の装置（stranger）には電源が去ることを知らせる。終わっていない FLR の機能には次の
   pci_function_reset のほかは触れない（dev/pci.h）。false は shutdown が完了しない（電源を切ってよいと言わない）。 */
static bool nvme_suspend(struct hal_device *d, enum hal_power_target target) {
    struct nvme *c = d->driver_data;
    bool power_goes = false;
    switch (target) {
    case HAL_POWER_SLEEP:
    case HAL_POWER_OFF: power_goes = true; break;
    case HAL_POWER_IMAGE: break;
    }
    mutex_lock(&c->reset_lock);
    __atomic_store_n(&c->asleep, true, __ATOMIC_RELEASE);
    bool cut = __atomic_load_n(&c->cut, __ATOMIC_ACQUIRE), stranger = cut && c->stranger, stopped = true;
    if (!pci_reset_left(c->pci) && (!cut || stranger)) {
        io_stop(c);
        controller_down(c);
        if (power_goes) stopped = shutdown_notify(c) || stranger;
    }
    mutex_unlock(&c->reset_lock);
    return stopped;
}

/* suspend の後。controller は電源を失って初めから始まるか（眠りの D3hot から D0 の内の reset、D3cold）、shutdown を
   終えたままか（休止の写しの戻り：起動したカーネルが電源断の walk で shutdown した）、I/O の queue を消したまま動いて
   いる（写し、取りやめ）。どれも enable の Controller Reset から 3.5.1 の順で立ち上げる（shutdown の後に命令を出すには
   CLR が要る、3.6.1）。Identify が同じ装置かを確かめ、別の装置なら使わない。眠りの間に来た要求は retry の列から出る。
   true なら I/O の queue は命令を受ける。 */
static bool nvme_resume(struct hal_device *d) {
    struct nvme *c = d->driver_data;
    mutex_lock(&c->reset_lock);
    __atomic_store_n(&c->asleep, false, __ATOMIC_RELEASE);
    /* 眠る前から使わない controller（cut。hal_failed で言ってある）は、そのまま：戻すものが無い */
    bool up = __atomic_load_n(&c->cut, __ATOMIC_ACQUIRE) || enable_recorded(c);
    mutex_unlock(&c->reset_lock);
    work_wake(c);
    if (!up) kprintf("nvme%d: did not come back after the sleep\n", c->index);
    return up;
}

static const struct n_hal_rule nvme_rules[] = {N_PCI_CLASS_PROGIF(0x01, 0x08, 0x02)};
static const struct hal_driver nvme_driver = {
    .name = "nvme",
    .rules = nvme_rules,
    .rule_count = ARRAY_SIZE(nvme_rules),
    .attach = nvme_attach,
    .detach = nvme_detach,
    .suspend = nvme_suspend,
    .resume = nvme_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
HAL_DRIVER(nvme_driver);
