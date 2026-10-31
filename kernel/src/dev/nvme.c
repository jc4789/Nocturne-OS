/* NVM Express SSDs (NVMe base specification 1.4c and 2.x, PCIe transport): one admin queue pair
   and one I/O queue pair per controller, one command in flight at a time, data through a bounce
   buffer. Every active namespace with plain 512..4096-byte blocks (no metadata, no protection
   information) becomes a block device "nvmeN". Completions arrive by MSI/MSI-X, or are polled
   when the platform offers neither. */
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
#include "sys/wait.h"

#define MAX_CTRL 4
#define MAX_NS 8
#define ADMIN_ENTRIES 32
#define IO_ENTRIES 64
#define XFER_PAGES 32 /* bounce buffer: 128 KiB, described by PRP1 and one PRP list page */
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

#define FAILED (-1) /* timeout or a dead controller; command errors return the status field */

struct queue {
    uint8_t *sq;
    uint64_t sq_phys;
    volatile uint32_t *cq;
    uint64_t cq_phys;
    uint16_t qid, size, tail, head;
    uint8_t phase;
};

struct nvme {
    int index;
    struct pci_device *pci;
    volatile uint8_t *regs;
    unsigned stride, max_xfer;
    uint64_t ready_ns; /* CC.EN to CSTS.RDY at most (CAP.TO, CRTO) */
    uint32_t rtd3e;
    struct queue admin, io;
    struct irq *irq; /* its message 0 (pci_irq_enable), NULL: polled */
    /* The interrupt handler counts kicks up, then wakes wq; the command waiting on another
       CPU reads kicks before it looks at its completion queue and sleeps only while kicks still
       reads that (wq_wait_unless). */
    uint64_t kicks;
    struct wait_queue wq;
    bool live, vwc;
    /* back from a sleep: enable again before the next command. Set by resume while the disks are
       held (blk_hold), read under the lock. */
    bool woke;
    struct mutex lock; /* one command at a time, from any CPU: the queues, cid, the bounce buffer */
    uint16_t cid;
    uint8_t *data;
    uint64_t data_phys;
    uint64_t *prp;
    uint64_t prp_phys;
};

struct ns {
    struct blkdev blk;
    struct nvme *c;
    uint32_t nsid;
    unsigned block;
};

static struct nvme ctrls[MAX_CTRL];
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

static enum irq_result nvme_isr(void *context) {
    struct nvme *c = context;
    __atomic_add_fetch(&c->kicks, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&c->wq);
    return IRQ_HANDLED;
}

static bool queue_init(struct queue *q, uint16_t qid, uint16_t size) {
    q->qid = qid;
    q->size = size;
    if (!q->sq) {
        if (!(q->sq_phys = pmm_alloc_contig(1, 64)) || !(q->cq_phys = pmm_alloc_contig(1, 64))) return false;
        q->sq = phys_to_virt(q->sq_phys);
        q->cq = phys_to_virt(q->cq_phys);
    }
    /* completion entries start with phase 0, the controller's first pass writes 1 */
    memset(q->sq, 0, 4096);
    memset((void *)q->cq, 0, 4096);
    q->tail = q->head = 0;
    q->phase = 1;
    return true;
}

/* Submit one command and wait for its completion. Returns 0, the 15-bit status field of a failed
   command, or FAILED when the controller stopped answering. */
static int command(struct nvme *c, struct queue *q, uint32_t cmd[16], uint64_t timeout_ns, uint32_t *result) {
    if (!c->live) return FAILED;
    c->cid = (uint16_t)((c->cid + 1) & 0x7FFF);
    cmd[0] = (cmd[0] & 0xFFFF) | (uint32_t)c->cid << 16; /* PRPs, not fused */
    memcpy(q->sq + q->tail * 64u, cmd, 64);
    q->tail = (uint16_t)((q->tail + 1) % q->size);
    __sync_synchronize();
    doorbell(c, q->qid, false, q->tail);
    uint64_t deadline = clock_after(timeout_ns);
    for (;;) {
        uint64_t seen = __atomic_load_n(&c->kicks, __ATOMIC_SEQ_CST); /* before the look */
        volatile uint32_t *e = q->cq + q->head * 4u;
        uint32_t dw3 = e[3];
        if (((dw3 >> 16) & 1) == q->phase) {
            __sync_synchronize();
            uint32_t dw0 = e[0];
            if (++q->head == q->size) {
                q->head = 0;
                q->phase ^= 1;
            }
            doorbell(c, q->qid, true, q->head);
            if ((dw3 & 0xFFFF) != c->cid) continue; /* a late answer to a command given up on */
            if (result) *result = dw0;
            return (int)(dw3 >> 17 & 0x7FFF);
        }
        uint32_t csts = rd32(c, CSTS);
        if (csts == 0xFFFFFFFFu || (csts & CSTS_CFS)) {
            kprintf("nvme%d: controller %s\n", c->index, csts == 0xFFFFFFFFu ? "vanished" : "reported a fatal error");
            c->live = false;
            return FAILED;
        }
        if (clock_now_ns() >= deadline) {
            kprintf("nvme%d: command %02x on queue %u timed out\n", c->index, cmd[0] & 0xFF, q->qid);
            return FAILED;
        }
        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
        wq_wait_unless(&c->wq, &c->kicks, seen, clock_after(c->irq ? 10 * NS_PER_MS : NS_PER_MS));
    }
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

/* Reset the controller and bring up the admin and I/O queue pairs (section 7.6.1). Also the way
   back after a command timed out: the reset aborts whatever was outstanding. */
static bool enable(struct nvme *c) {
    c->live = false;
    uint32_t cc = rd32(c, CC);
    if (cc & 1) {
        wait_ready(c, true); /* EN may only fall once RDY followed it up */
        wr32(c, CC, cc & ~1u);
    }
    if (!wait_ready(c, false)) {
        kprintf("nvme%d: controller did not reset\n", c->index);
        return false;
    }
    uint64_t cap = rd64(c, CAP);
    unsigned entries = (unsigned)(cap & 0xFFFF) + 1;
    if (!queue_init(&c->admin, 0, ADMIN_ENTRIES) || !queue_init(&c->io, 1, MIN(IO_ENTRIES, entries))) return false;
    wr32(c, AQA, (ADMIN_ENTRIES - 1u) << 16 | (ADMIN_ENTRIES - 1u));
    wr64(c, ASQ, c->admin.sq_phys);
    wr64(c, ACQ, c->admin.cq_phys);
    /* NVM command set, 4 KiB pages, round robin, 64-byte SQ and 16-byte CQ entries */
    wr32(c, CC, 1u | 6u << 16 | 4u << 20);
    if (!wait_ready(c, true)) {
        kprintf("nvme%d: controller did not become ready (status %x)\n", c->index, rd32(c, CSTS));
        return false;
    }
    c->live = true;
    uint32_t cmd[16] = {OP_SET_FEATURES}, got;
    cmd[10] = 0x07; /* Number of Queues: one submission and one completion queue (0-based) */
    int r = command(c, &c->admin, cmd, ADMIN_NS, &got);
    if (r) goto fail;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = OP_CREATE_CQ;
    *(uint64_t *)&cmd[6] = c->io.cq_phys;
    cmd[10] = (uint32_t)(c->io.size - 1) << 16 | 1;
    cmd[11] = (c->irq ? 2u : 0u) | 1u; /* vector 0, interrupts on when we have one, contiguous */
    if ((r = command(c, &c->admin, cmd, ADMIN_NS, NULL))) goto fail;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = OP_CREATE_SQ;
    *(uint64_t *)&cmd[6] = c->io.sq_phys;
    cmd[10] = (uint32_t)(c->io.size - 1) << 16 | 1;
    cmd[11] = 1u << 16 | 1u; /* completion queue 1, contiguous */
    if ((r = command(c, &c->admin, cmd, ADMIN_NS, NULL))) goto fail;
    return true;
fail:
    kprintf("nvme%d: setting up the I/O queues failed (%x)\n", c->index, r);
    c->live = false;
    return false;
}

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid) {
    uint32_t cmd[16] = {OP_IDENTIFY};
    cmd[1] = nsid;
    *(uint64_t *)&cmd[6] = c->data_phys;
    cmd[10] = cns;
    memset(c->data, 0, 4096);
    return command(c, &c->admin, cmd, ADMIN_NS, NULL);
}

/* Describe `bytes` of the bounce buffer: the first page in PRP1, then the second page or a list. */
static void set_prps(struct nvme *c, uint32_t cmd[16], unsigned bytes) {
    unsigned pages = (bytes + 4095) / 4096;
    uint64_t prp2 = 0;
    if (pages == 2) prp2 = c->data_phys + 4096;
    else if (pages > 2) {
        for (unsigned i = 1; i < pages; i++) c->prp[i - 1] = c->data_phys + i * 4096ull;
        prp2 = c->prp_phys;
    }
    *(uint64_t *)&cmd[6] = c->data_phys;
    *(uint64_t *)&cmd[8] = prp2;
}

/* Read or write `n` logical blocks between the device and the bounce buffer (buf, blk_by_blocks). */
static int transfer(void *ctx, bool write, uint64_t lba, unsigned n, uint8_t *buf) {
    struct ns *s = ctx;
    struct nvme *c = s->c;
    (void)buf;
    for (int attempt = 0;; attempt++) {
        uint32_t cmd[16] = {write ? OP_WRITE : OP_READ};
        cmd[1] = s->nsid;
        set_prps(c, cmd, n * s->block);
        cmd[10] = (uint32_t)lba;
        cmd[11] = (uint32_t)(lba >> 32);
        cmd[12] = n - 1;
        int r = command(c, &c->io, cmd, IO_NS, NULL);
        if (!r) return 0;
        if (attempt == 2) return -1;
        if (r == FAILED) {
            if (!c->live || !enable(c)) return -1; /* a timeout: reset and reissue */
            continue;
        }
        unsigned sct = (unsigned)r >> 8 & 7, sc = (unsigned)r & 0xFF;
        kprintf("nvme%d: %s of block %lu failed (type %u code %02x)\n", c->index, write ? "write" : "read", lba, sct, sc);
        if (r & 0x4000 || sct == 2) return -1; /* do not retry, or a media error */
    }
}

static int flush(struct ns *s) {
    uint32_t cmd[16] = {OP_FLUSH};
    cmd[1] = s->nsid;
    return command(s->c, &s->c->io, cmd, IO_NS, NULL) ? -1 : 0;
}

/* The controller for one request: back from a sleep, it is enabled again first. */
static void lock_awake(struct nvme *c) {
    mutex_lock(&c->lock);
    if (c->woke) {
        c->woke = false;
        if (enable(c)) kprintf("nvme%d: ready again after sleep\n", c->index);
        else kprintf("nvme%d: did not come back after sleep\n", c->index);
    }
}

/* 512-byte sectors on top of the namespace's logical blocks, through the bounce buffer. */
static int rw(struct ns *s, bool write, uint64_t lba, uint32_t count, uint8_t *p) {
    struct nvme *c = s->c;
    lock_awake(c);
    int r = blk_by_blocks(&s->blk, write, lba, count, p, c->data, c->max_xfer / s->block, false, transfer, s);
    mutex_unlock(&c->lock);
    return r;
}

static int blk_nvme_read(struct blkdev *b, uint64_t lba, uint32_t n, void *buf) { return rw(b->priv, false, lba, n, buf); }
static int blk_nvme_write(struct blkdev *b, uint64_t lba, uint32_t n, const void *buf) {
    return rw(b->priv, true, lba, n, (uint8_t *)buf);
}

/* Flush (NVMe base 7.1): the volatile write cache reaches the medium. */
static int blk_nvme_persist(struct blkdev *b) {
    struct ns *s = b->priv;
    lock_awake(s->c);
    int r = flush(s) < 0 ? -N_ERR_IO : 0;
    mutex_unlock(&s->c->lock);
    return r;
}

static void add_namespace(struct nvme *c, uint32_t nsid, const char *model) {
    if (identify(c, 0, nsid)) return;
    const uint8_t *id = c->data;
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
    ksnprintf(s->blk.name, sizeof s->blk.name, "nvme%d", ndisks++);
    strlcpy(s->blk.model, model, sizeof s->blk.model);
    s->blk.sectors = blocks * (s->block / 512);
    s->blk.read = blk_nvme_read;
    s->blk.write = blk_nvme_write;
    if (c->vwc) s->blk.persist = blk_nvme_persist;
    s->blk.block = s->block;
    s->blk.priv = s;
    blk_register(&s->blk);
}

static void start(struct nvme *c) {
    struct pci_device *pci = c->pci;
    uint64_t base = pci_bar_address(pci, 0);
    if (!base) {
        kprintf("nvme%d: no register window\n", c->index);
        return;
    }
    c->regs = vmm_map_mmio(base, 0x4000); /* registers and the doorbells of the first queue pairs */
    if (!c->regs) {
        kprintf("nvme%d: no memory to map its registers\n", c->index);
        return;
    }
    pci_enable_busmaster(pci);
    uint64_t cap = rd64(c, CAP);
    c->stride = 4u << (cap >> 32 & 0xF);
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
    if (!(c->data_phys = pmm_alloc_contig(XFER_PAGES, 64)) || !(c->prp_phys = pmm_alloc_contig(1, 64))) return;
    c->data = phys_to_virt(c->data_phys);
    c->prp = phys_to_virt(c->prp_phys);
    pci_irq_enable(pci, &(struct irq_spec){.name = "NVMe", .handler = nvme_isr, .context = c, .node = -1}, 1, &c->irq);
    if (!enable(c)) return;

    if (identify(c, 1, 0)) {
        kprintf("nvme%d: identify failed\n", c->index);
        return;
    }
    const uint8_t *id = c->data;
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
    c->max_xfer = XFER_PAGES * 4096;
    if (mdts && mdts < 6 && (4096u << mdts) < c->max_xfer) c->max_xfer = 4096u << mdts;
    c->vwc = id[525] & 1;
    c->rtd3e = *(const uint32_t *)(id + 88);
    uint32_t count = *(const uint32_t *)(id + 516), nsids[MAX_NS];
    unsigned found = 0;
    kprintf("nvme%d: %s (%04x:%04x), version %x, %u namespaces, %s%s\n", c->index, model, pci->vendor, pci->device,
            rd32(c, VS), count, c->irq ? "MSI" : "polled", c->vwc ? ", write cache" : "");
    if (!identify(c, 2, 0)) /* active namespace list */
        for (const uint32_t *l = (const uint32_t *)c->data; found < MAX_NS && found < 1024 && l[found]; found++)
            nsids[found] = l[found];
    else /* NVMe 1.0 lacks the list: try the first ones */
        for (uint32_t i = 1; i <= count && found < MAX_NS; i++) nsids[found++] = i;
    for (unsigned i = 0; i < found; i++) add_namespace(c, nsids[i], model);
}

/* Normal shutdown notification before power goes away (section 7.6.2): the controller writes its
   cache back and the drive does not count an unsafe shutdown. Interrupts are already off. */
static bool nvme_shutdown(void *arg) {
    (void)arg;
    bool all = true;
    for (int i = 0; i < nctrls; i++) {
        struct nvme *c = &ctrls[i];
        if (!c->live) continue;
        wr32(c, CC, (rd32(c, CC) & ~0xC000u) | 0x4000u);
        uint64_t us = c->rtd3e < 1000000 ? 1000000 : c->rtd3e > 10000000 ? 10000000 : c->rtd3e;
        for (uint64_t deadline = clock_after(us * NS_PER_US);
             (rd32(c, CSTS) >> 2 & 3) != 2 && clock_now_ns() < deadline;)
            pause();
        if ((rd32(c, CSTS) >> 2 & 3) != 2) all = false; /* its cache may not be on the flash */
        c->live = false;
    }
    return all;
}

/* After a sleep (the shutdown notification above went first): the next read or write resets the
   controller and sets the queues up again, the way it recovers from a timeout. Resume must not
   wait for the controller; the I/O path may (it reports a controller that does not come back). */
static bool nvme_resume(void *arg) {
    (void)arg;
    for (int i = 0; i < nctrls; i++)
        if (ctrls[i].regs && ctrls[i].data) ctrls[i].woke = true;
    return true;
}

static struct pm_device nvme_pm = {.name = "nvme", .stage = PM_STAGE_STORAGE, .quiesce = nvme_shutdown,
                                   .resume = nvme_resume};

void nvme_init(void) {
    for (int i = 0; i < pci_count() && nctrls < MAX_CTRL; i++) {
        struct pci_device *p = pci_at(i);
        if (p->cls != 0x01 || p->subcls != 0x08 || p->progif != 0x02) continue;
        struct nvme *c = &ctrls[nctrls];
        c->index = nctrls++;
        c->pci = p;
        start(c);
    }
    if (nctrls) pm_register(&nvme_pm);
}
