/* xHCI host controllers (eXtensible Host Controller Interface for USB, revision 1.2) and the USB
 * device lifecycle on them.
 *
 * Two flows, apart. The completions: each interrupter's event ring is drained in its interrupt
 * (or a polled controller's alarm), which never sleeps: a transfer that finished goes from there
 * straight to its class driver's report function, so a key or a pointer move reaches the input
 * layer from the interrupt. The long work: each controller has a worker thread that brings it up,
 * looks at its ports and hubs, enumerates, recovers halted endpoints and restarts it after an
 * error or a sleep; those steps sleep (debounce, reset, power-on) without holding the events up.
 * The class drivers' own transfers (usb_control, usb_bulk) wait for their completion in whatever
 * thread calls them.
 *
 * Interrupters (4.17): commands, port changes and control transfers go to interrupter 0, the
 * interrupt endpoints to a second one and the bulk endpoints to a third, each with its own MSI-X
 * vector, where the controller and the PCI layer give that many; with fewer they share. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define RING_TRBS 256 /* one page; the last entry links back to the first */
#define EV_PAGES 4    /* each event ring: one contiguous segment */
#define EV_TRBS (EV_PAGES * 256)

struct trb {
    uint64_t param;
    uint32_t status, control;
};

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_NOOP = 8,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS = 11, TRB_CONFIGURE = 12, TRB_EVALUATE = 13,
    TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_DEQUEUE = 16, TRB_RESET_DEVICE = 17, TRB_NOOP_CMD = 23,
    TRB_EV_TRANSFER = 32, TRB_EV_COMMAND = 33, TRB_EV_PORT = 34,
};
#define TRB_TC (1u << 1) /* link: toggle the cycle */
#define TRB_ISP (1u << 2)
#define TRB_CH (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_TARGET(i) ((uint32_t)(i) << 22) /* a transfer TRB's Interrupter Target (6.4.1) */
#define TRB_DC (1u << 9) /* Configure Endpoint: Deconfigure (4.6.6) */
#define CC_SUCCESS 1
#define CC_TRANSACTION_ERROR 4
#define CC_TRB_ERROR 5
#define CC_STALL 6
#define CC_SHORT 13
#define CC_CONTEXT_STATE 19
#define CC_CMD_RING_STOPPED 24
#define CC_STOPPED 26
#define CC_STOPPED_LENGTH 27
#define CC_STOPPED_SHORT 28
/* endpoint states (6.2.3): the output context's are good only right after a command that fails
   on them (4.8.3), so the driver follows each endpoint itself: a doorbell runs it, a Transfer Event's
   error halts it, and its commands stop it */
enum { EP_DISABLED, EP_RUNNING, EP_HALTED, EP_STOPPED, EP_ERROR };

/* operational registers */
#define USBCMD 0x00
#define USBSTS 0x04
#define PAGESIZE 0x08
#define CRCR 0x18
#define DCBAAP 0x30
#define CONFIG 0x38
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define PORTLI(p) (PORTSC(p) + 8)
#define CMD_INTE (1u << 2)
#define STS_HCH 1u
#define STS_HSE (1u << 2)
#define STS_EINT (1u << 3)
#define STS_CNR (1u << 11)
#define STS_HCE (1u << 12)
#define CRCR_CA (1u << 2)
#define CRCR_CRR (1u << 3)
#define CRCR_RSVDP 0x30u
#define HCC2_LEC (1u << 4)
#define PS_CCS 1u
#define PS_PED (1u << 1)
#define PS_OCA (1u << 3)
#define PS_PR (1u << 4)
#define PS_PLS(sc) (((sc) >> 5) & 15)
#define PS_PP (1u << 9)
#define PS_CSC (1u << 17)
#define PS_WRC (1u << 19)
#define PS_PRC (1u << 21)
#define PS_PLC (1u << 22)
#define PS_WPR (1u << 31)
#define PLS_RXDETECT 5
#define PLS_INACTIVE 6
#define PLS_COMPLIANCE 10
#define PS_CHANGES (0x7Fu << 17)
#define PS_PIC (3u << 14)
#define SLOT_CONFIGURED 3u /* the Slot Context's Slot State (Table 6-7) */
#define PS_KEEP (PS_PP | PS_PIC | 7u << 25) /* bits a write must carry over; the rest are RW1C or actions */
/* an interrupter's register set in the runtime registers (5.5.2): 0x20 + 32 * n */
#define IR_SET(n) (0x20 + 32 * (n))
#define IMAN 0x00
#define IMOD 0x04
#define ERSTSZ 0x08
#define ERSTBA 0x10
#define ERDP 0x18
#define IMAN_IP 1u
#define IMAN_IE 2u
#define ERDP_EHB 8u
/* IMODI of an interrupter without interrupt endpoints: at most one interrupt per 250 us (in 250 ns
   units, 5.5.2.2) gathers the completions of bulk streams and command bursts. The one that carries
   the interrupt endpoints has none: each of those completes at most once per packet of its own
   service interval, so it cannot storm, and a report waits for nothing. */
#define IMODI_GATHER 1000
#define HALT_NS (16 * NS_PER_MS)      /* USBSTS.HCH follows USBCMD.R/S within this (5.4.2) */
#define COMMAND_ABORT_NS (5 * NS_PER_S) /* an aborted command's ring stops within this (4.6.1.2) */
#define CONTROL_NS (5 * NS_PER_S)     /* a control request ends within this (USB 2.0 9.2.6.1) */

/* The kinds of transfer events, each on an interrupter of its own when there are enough. */
enum ring_kind { RING_CONTROL, RING_PERIODIC, RING_BULK, RING_KINDS };

struct ring {
    struct trb *trb;
    uint64_t phys;
    unsigned enq;
    uint32_t cycle;
};

#define EP_XFERS 16        /* transfers one endpoint keeps in flight */
#define SYNC_BYTES 0x10000 /* usb_bulk's bounce buffer */

/* What a ring entry belongs to: transfer td-1 of the endpoint, queued in generation gen. An
 * event names a ring entry, and the generation tells a late event of a finished transfer from
 * the transfer that reuses its buffer. */
struct trb_meta {
    uint8_t td, last; /* last: the entry whose completion finishes the transfer */
    uint16_t gen;
    uint32_t before; /* bytes of the transfer in the entries before this one */
};

enum { EP_STREAM = 1, EP_SYNC, EP_SEND };

/* An endpoint. lock guards its ring, meta, busy, gen, trbs, inflight, next, halted, dead and the
   sync result, between the drain that completes its transfers, the thread that queues them
   (usb_bulk, usb_send) and the worker that recovers it. The rest is set before mode is, and stays. */
struct endpoint {
    struct spinlock lock;
    struct wait_queue wq; /* usb_bulk's caller, for sync_done or the device's removal */
    struct ring ring;
    uint8_t dci, address, type, mode; /* type: bmAttributes & 3 */
    uint8_t state;                    /* EP_RUNNING, ... under lock */
    uint8_t target;                   /* its interrupter */
    uint8_t interval;                 /* periodic: the exponent of its service interval; a high-speed bulk
                                         OUT: its NAK rate (6.2.3.6) */
    unsigned mps, esit;
    uint8_t *buf; /* contiguous DMA area: `count` transfers `stride` bytes apart */
    uint64_t buf_phys;
    unsigned pages;
    unsigned len;           /* bytes per transfer (streams: as usb_stream_start's caller says) */
    unsigned stride, count;
    uint32_t busy;          /* transfers in flight */
    uint16_t gen[EP_XFERS];
    uint8_t trbs[EP_XFERS];
    unsigned inflight;      /* ring entries in use */
    unsigned next;          /* send: the slot to try first */
    struct trb_meta meta[RING_TRBS];
    uint32_t short_seen;    /* transfers that went short before their last entry: the bytes that came */
    unsigned short_got[EP_XFERS];
    bool sync_done;
    uint8_t sync_cc;
    unsigned sync_got;
    usb_report_fn fn;
    void *ctx;
    bool halted, dead;
    unsigned fails;    /* recoveries since the last good transfer, the end of it at 8 (the worker's and a drain's) */
    uint64_t retry_at; /* when the next recovery may run (clock_now_ns; the worker's) */
};

struct xdev {
    struct usb_device dev; /* first: class drivers only see this part */
    uint8_t *out_ctx, *in_ctx;
    uint64_t out_phys, in_phys;
    struct ring ep0;
    uint8_t ep0_state;  /* EP_RUNNING, ... under ctl_lock */
    uint8_t speed_id;   /* the Slot Context's Speed: the port's own Protocol Speed ID (7.2.2.1) */
    uint16_t ep0_mps;
    uint8_t *xfer; /* control transfer bounce page */
    uint64_t xfer_phys;
    struct mutex in_mutex;    /* in_ctx, from the first field written until the command that reads it ends;
                                 before cmd_mutex */
    struct mutex ctl_mutex;   /* one control transfer at a time: ep0, xfer, ctl_broken */
    struct spinlock ctl_lock; /* the result below, against the drain */
    struct wait_queue ctl_wq;
    bool ctl_done;
    uint8_t ctl_cc;
    uint32_t ctl_residual;
    uint64_t ctl_status_trb;
    struct endpoint *ep[32]; /* by device context index; set by the worker, read by the drain */
    unsigned hub_ports, think_time;
    bool hub, multi_tt;
    bool ctl_broken; /* the control ring could not be restarted: nothing more may be queued on it */
    bool work;       /* atomic: a class driver asked for its work functions (usb_note_work) */
};

struct interrupter {
    struct usb_hc *hc;
    unsigned index;
    volatile uint8_t *regs;
    struct spinlock lock; /* the ring's dequeue side: one drain at a time */
    struct trb *ev;
    uint64_t ev_phys;
    uint64_t *erst;
    uint64_t erst_phys;
    unsigned ev_idx;
    uint32_t ev_cycle;
    struct irq *irq;     /* its MSI or MSI-X message, NULL when polled */
    uint32_t interrupts; /* under lock */
};

struct usb_hc {
    int index;
    struct pci_device *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    unsigned max_slots, max_ports, max_intrs, ctx_size;
    bool lec;      /* Large ESIT Payload (HCCPARAMS2, 5.3.9) */
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;
    uint64_t bar_size;
    uint64_t scratch_phys; /* kept across a restart, like the rings */
    struct ring cmd;
    struct mutex cmd_mutex;   /* one command at a time: the command ring and CRCR */
    struct spinlock cmd_lock; /* the result below, against the drain */
    struct wait_queue cmd_wq;
    uint64_t cmd_trb;
    bool cmd_done, cmd_stopped;
    uint8_t cmd_cc, cmd_slot;
    struct interrupter *intr;
    unsigned nintr;
    uint8_t target[RING_KINDS]; /* the interrupter each kind of event goes to */
    bool running; /* atomic; written under every interrupter's lock (hc_set_running), or by resume, where no drain runs */
    bool polled;  /* atomic: no interrupt comes, the poll alarm drains the rings */
    struct alarm poll;
    uint32_t periodic[16]; /* open interrupt endpoints by service interval exponent (atomic) */
    /* the worker: kicks counts the notes of work (wq_wait_unless), the rest is the worker's but
       port_pending, recover and woke, which drains and resume set */
    struct thread *worker;
    struct wait_queue work_wq;
    uint64_t kicks;
    uint32_t port_pending[8];
    bool recover, woke;
    bool wedged; /* atomic: an aborted command's ring did not stop (4.6.1.2) */
    bool failed, settled;
    unsigned restarts;
    uint64_t retry_at; /* the earliest endpoint recovery retry (CLOCK_NEVER = none) */
    struct xdev *slots[256]; /* set and cleared by the worker, read by the drains */
    struct xdev *root[256];  /* device on each root port */
    uint8_t port_major[256]; /* USB major revision of each root port, from the protocol capabilities */
    uint8_t port_slot_type[256];
    uint8_t port_hubs[256];  /* the most hubs before a root port's devices, 0: USB's own limit (Table 7-14) */
    struct usb_port_tries port_tries[256]; /* the worker's */
    struct usb_port_tries port_power[256]; /* the worker's: its power turned on again after an over-current */
    const uint32_t *port_psi[256]; /* its protocol's speed table (7.2.1), once for good */
    uint8_t port_psic[256];
};

static struct usb_hc **controllers;
static size_t ncontrollers, controllers_room;
static unsigned unsettled; /* controllers whose first look at every port and hub is not done (atomic) */

static inline uint32_t rd32(volatile uint8_t *b, unsigned off) { return mmio_read32(b + off); }
static inline void wr32(volatile uint8_t *b, unsigned off, uint32_t v) { mmio_write32(b + off, v); }
static inline void wr64(volatile uint8_t *b, unsigned off, uint64_t v) { mmio_write64(b + off, v); } /* 5.1.1 */
#define barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

static bool hc_running(const struct usb_hc *hc) { return __atomic_load_n(&hc->running, __ATOMIC_ACQUIRE); }
static bool removed(const struct usb_device *d) { return __atomic_load_n(&d->removed, __ATOMIC_ACQUIRE); }

/* Work for the controller's worker: its wait returns at once. Any context. */
static void hc_kick(struct usb_hc *hc) {
    __atomic_fetch_add(&hc->kicks, 1, __ATOMIC_RELEASE);
    wq_wake_all(&hc->work_wq);
}

void usb_note_work(struct usb_device *d) {
    __atomic_store_n(&((struct xdev *)d)->work, true, __ATOMIC_RELEASE);
    hc_kick(d->hc);
}

uint64_t usb_kicks(struct usb_device *d) { return __atomic_load_n(&d->hc->kicks, __ATOMIC_ACQUIRE); }

void usb_wait_kick(struct usb_device *d, uint64_t seen, uint64_t deadline) {
    wq_wait_unless(&d->hc->work_wq, &d->hc->kicks, seen, deadline);
}

/* Every drain of the controller sees running from here on, and none that saw the old value runs. */
static void hc_set_running(struct usb_hc *hc, bool running) {
    for (unsigned i = 0; i < hc->nintr; i++) {
        uint64_t f = spin_lock(&hc->intr[i].lock);
        __atomic_store_n(&hc->running, running, __ATOMIC_RELEASE);
        spin_unlock(&hc->intr[i].lock, f);
    }
    if (running) return;
    uint64_t f = spin_lock(&hc->cmd_lock); /* a command waiter gives up */
    wq_wake_all(&hc->cmd_wq);
    spin_unlock(&hc->cmd_lock, f);
}

/* No drain still works with what it read before: the caller published a removal first. */
static void sync_drains(struct usb_hc *hc) {
    for (unsigned i = 0; i < hc->nintr; i++) {
        uint64_t f = spin_lock(&hc->intr[i].lock);
        spin_unlock(&hc->intr[i].lock, f);
    }
}

/* Interrupts off, lock taken: asleep on wq until woken or the deadline; returns with lock taken
   again, false when the deadline came. */
static bool sleep_locked(struct wait_queue *wq, struct spinlock *lock, uint64_t deadline) {
    bool woke = wq_wait_locked_until(wq, lock, deadline);
    spin_take(lock);
    return woke;
}

static void *dma_page(uint64_t *phys) {
    uint64_t p = pmm_alloc_zeroed();
    if (!p) return NULL;
    *phys = p;
    return phys_to_virt(p);
}

/* `pages` (at most 16) contiguous zeroed pages inside one 64 KiB block: no xHCI structure of up
 * to 64 KiB may span such a boundary (6.0, Table 6-1). A run starts at a multiple of the power of
 * two at or above its size, so one of at most 16 pages never crosses one. */
static void *dma_block(unsigned pages, uint64_t *phys) {
    ASSERT(pages <= 16);
    uint64_t base = pmm_alloc_contig(pages, 64);
    if (!base) return NULL;
    memset(phys_to_virt(base), 0, pages * 4096ull);
    *phys = base;
    return phys_to_virt(base);
}

static void dma_free(uint64_t phys, unsigned pages) {
    for (unsigned i = 0; i < pages; i++) pmm_free(phys + i * 4096ull);
}

static void ring_reset(struct ring *r) {
    memset(r->trb, 0, 4096);
    r->enq = 0;
    r->cycle = 1;
    r->trb[RING_TRBS - 1].param = r->phys;
    r->trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

static bool ring_init(struct ring *r) {
    if (!(r->trb = dma_page(&r->phys))) return false;
    ring_reset(r);
    return true;
}

/* One TRB at the enqueue point. held: its cycle bit is left wrong until ring_release, so the
   controller does not start on a TD whose other TRBs are not written yet (4.11.7.1 keeps a TD Fragment
   from being seen in part). */
static uint64_t ring_write(struct ring *r, uint64_t param, uint32_t status, uint32_t control, bool held) {
    struct trb *t = &r->trb[r->enq];
    uint64_t at = r->phys + r->enq * sizeof(struct trb);
    t->param = param;
    t->status = status;
    barrier();
    t->control = (control & ~1u) | (held ? r->cycle ^ 1 : r->cycle);
    if (++r->enq == RING_TRBS - 1) {
        struct trb *link = &r->trb[RING_TRBS - 1];
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    return ring_write(r, param, status, control, false);
}

/* The first TRB of a TD: its cycle bit is flipped by ring_release after the rest are written. */
static struct trb *ring_hold(struct ring *r, uint64_t param, uint32_t status, uint32_t control, uint64_t *at) {
    struct trb *t = &r->trb[r->enq];
    uint64_t where = ring_write(r, param, status, control, true);
    if (at) *at = where;
    return t;
}

static void ring_release(struct trb *first) {
    barrier();
    first->control ^= 1;
}

/* A TD of `trbs` on a Transfer Ring lies in one segment before the Link TRB (4.11.7.1, 4.9.2): what does
   not fit starts after the link, the entries left filled with No Op TDs, which end at once and report
   nothing (6.4.1.4, 4.11.7). Returns how many; meta marks them as no transfer's. */
static unsigned ring_fit(struct ring *r, unsigned trbs, uint32_t target, struct trb_meta *meta) {
    unsigned left = RING_TRBS - 1 - r->enq;
    if (trbs <= left) return 0;
    for (unsigned i = 0; i < left; i++) {
        meta[r->enq] = (struct trb_meta){0};
        ring_push(r, 0, TRB_TARGET(target), TRB_TYPE(TRB_NOOP));
    }
    return left;
}

static void doorbell(struct usb_hc *hc, unsigned slot, unsigned target) {
    barrier();
    mmio_write32(hc->db + slot, target);
}

/* ---- transfers on an endpoint ---- */

/* Room on the ring for `trbs` more entries? One stays free so a full ring is never mistaken
 * for an empty one, and the link entry is not usable. The TD may start after a fill as long as itself. */
static bool ring_room(const struct endpoint *ep, unsigned trbs) { return ep->inflight + 2 * trbs <= RING_TRBS - 2; }

/* Queue transfer b: `len` bytes of its buffer as one TD (xHCI 4.11.2), chained wherever a 64 KiB
 * boundary falls (no TRB buffer may span one), and if `zlp`, a zero-length TD after it that
 * ends a transfer of whole packets. A transfer ends at a short packet or after `len` bytes.
 * Every TRB of it names the endpoint's interrupter (4.11.2: the same in all TRBs of a TD).
 * ep->lock held. */
static void endpoint_push(struct endpoint *ep, unsigned b, unsigned len, bool zlp) {
    uint64_t at = ep->buf_phys + (uint64_t)b * ep->stride;
    unsigned done = 0, packets = (len + ep->mps - 1) / ep->mps, need = 0;
    bool in = ep->address & 0x80;
    for (unsigned off = 0;;) {
        off += MIN(len - off, 0x10000 - (unsigned)((at + off) & 0xFFFF));
        need++;
        if (off == len) break;
    }
    unsigned trbs = ring_fit(&ep->ring, need, ep->target, ep->meta) + need;
    struct trb *first = NULL;
    for (;;) {
        unsigned chunk = len - done, room = 0x10000 - (unsigned)((at + done) & 0xFFFF);
        if (chunk > room) chunk = room;
        bool last = done + chunk == len;
        unsigned left = last ? 0 : packets - (done + chunk) / ep->mps; /* TD Size, 4.11.2.4 */
        ep->meta[ep->ring.enq] = (struct trb_meta){(uint8_t)(b + 1), last && !zlp, ep->gen[b], done};
        uint32_t control = TRB_TYPE(TRB_NORMAL) | (in ? TRB_ISP : 0) | (!last ? TRB_CH : zlp ? 0 : TRB_IOC);
        uint32_t status = chunk | (left > 31 ? 31u : left) << 17 | TRB_TARGET(ep->target);
        if (!first) first = ring_hold(&ep->ring, at + done, status, control, NULL);
        else ring_push(&ep->ring, at + done, status, control);
        done += chunk;
        if (last) break;
    }
    ring_release(first);
    if (zlp) {
        ep->meta[ep->ring.enq] = (struct trb_meta){(uint8_t)(b + 1), 1, ep->gen[b], len};
        ring_push(&ep->ring, 0, TRB_TARGET(ep->target), TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        trbs++;
    }
    ep->trbs[b] = (uint8_t)trbs;
    ep->inflight += trbs;
    ep->busy |= 1u << b;
    ep->state = EP_RUNNING;
}

/* Every transfer of the endpoint is gone: after a halt or an abort moved the ring past them.
 * ep->lock held. */
static void endpoint_forget(struct endpoint *ep) {
    for (unsigned b = 0; b < EP_XFERS; b++)
        if (ep->busy & (1u << b)) ep->gen[b]++;
    ep->busy = 0;
    ep->inflight = 0;
    ep->short_seen = 0;
}

/* The endpoint's transfers forgotten and its halt set or cleared, in one step. */
static void endpoint_restart(struct endpoint *ep, bool halted) {
    uint64_t f = spin_lock(&ep->lock);
    endpoint_forget(ep);
    ep->halted = halted;
    spin_unlock(&ep->lock, f);
}

/* A stream's every transfer queued again; ep->lock held. */
static void endpoint_requeue_all(struct usb_hc *hc, struct xdev *xd, struct endpoint *ep) {
    for (unsigned b = 0; b < ep->count; b++) endpoint_push(ep, b, ep->len, false);
    doorbell(hc, xd->dev.slot, ep->dci);
}

/* ---- the events: the drains, in interrupts and the poll alarm, never sleeping ---- */

/* The state a Transfer Event's completion code leaves its endpoint in (4.8.3, 4.10.2). */
static unsigned state_after(unsigned cc) { return cc == CC_TRB_ERROR ? EP_ERROR : EP_HALTED; }

static bool stopped_code(unsigned cc) { return cc == CC_STOPPED || cc == CC_STOPPED_LENGTH || cc == CC_STOPPED_SHORT; }

static void control_event(struct xdev *xd, uint64_t param, uint32_t status) {
    unsigned cc = status >> 24;
    spin_take(&xd->ctl_lock);
    if (cc != CC_SUCCESS && cc != CC_SHORT && !stopped_code(cc)) xd->ep0_state = (uint8_t)state_after(cc);
    if (param == xd->ctl_status_trb || (cc != CC_SUCCESS && cc != CC_SHORT)) {
        xd->ctl_cc = (uint8_t)cc;
        xd->ctl_done = true;
        wq_wake_all(&xd->ctl_wq);
    } else if (cc == CC_SHORT) {
        xd->ctl_residual = status & 0xFFFFFF;
    }
    spin_give(&xd->ctl_lock);
}

static void transfer_event(struct usb_hc *hc, uint64_t param, uint32_t status, uint32_t control, uint64_t at) {
    unsigned slot = control >> 24, dci = (control >> 16) & 31, cc = status >> 24;
    struct xdev *xd = slot && slot <= hc->max_slots ? __atomic_load_n(&hc->slots[slot], __ATOMIC_ACQUIRE) : NULL;
    if (!xd || removed(&xd->dev)) return;
    if (dci == 1) {
        control_event(xd, param, status);
        return;
    }
    struct endpoint *ep = __atomic_load_n(&xd->ep[dci], __ATOMIC_ACQUIRE);
    if (!ep) return;
    if (stopped_code(cc)) return; /* our own Stop Endpoint */
    spin_take(&ep->lock);
    if (!ep->mode || ep->dead || param < ep->ring.phys || param >= ep->ring.phys + 4096) {
        spin_give(&ep->lock);
        return;
    }
    unsigned idx = (unsigned)((param - ep->ring.phys) / sizeof(struct trb));
    struct trb_meta m = idx < RING_TRBS - 1 ? ep->meta[idx] : (struct trb_meta){0};
    unsigned b = m.td - 1u;
    bool live = m.td && b < EP_XFERS && (ep->busy & (1u << b)) && m.gen == ep->gen[b];
    /* A TD that is gone was given up by a recovery, which has dealt with the endpoint since. An error
       on an entry of no TD (the No Op of a wrap, a Link TRB) is the endpoint's as it is now. */
    if (!live && (m.td || cc == CC_SUCCESS || cc == CC_SHORT)) {
        spin_give(&ep->lock);
        return;
    }
    /* what reached memory or the device before this event's TRB ended, the bytes of its TD */
    unsigned trb_len = idx < RING_TRBS - 1 ? ep->ring.trb[idx].status & 0x1FFFF : 0, residual = status & 0xFFFFFF;
    unsigned got = m.before + (residual < trb_len ? trb_len - residual : 0);
    if (got > ep->len) got = ep->len;
    if (cc != CC_SUCCESS && cc != CC_SHORT) {
        /* the endpoint halted on this TD (xHCI 4.10.2): the ring stays put until recovered */
        ep->state = (uint8_t)state_after(cc);
        if (ep->mode == EP_SYNC) {
            endpoint_forget(ep);
            ep->halted = true;
            ep->sync_cc = (uint8_t)cc;
            ep->sync_got = got;
            ep->sync_done = true;
            wq_wake_all(&ep->wq);
            spin_give(&ep->lock);
            return;
        }
        bool first = !ep->halted && !ep->fails;
        ep->halted = true;
        spin_give(&ep->lock);
        if (first) kprintf("usb: slot %u endpoint %u failed (completion %u), resetting it\n", slot, dci, cc);
        __atomic_store_n(&hc->recover, true, __ATOMIC_RELEASE);
        hc_kick(hc);
        return;
    }
    /* Only the event of the last entry completes the TD (4.10.1.1). A short packet on an entry before
     * it gives two: this one, whose length counts, and then the last entry's, whose should be
     * the same, so the first is kept. */
    if (ep->halted || (cc == CC_SUCCESS && !m.last)) {
        spin_give(&ep->lock);
        return;
    }
    if (!m.last) {
        ep->short_seen |= 1u << b;
        ep->short_got[b] = got;
        spin_give(&ep->lock);
        return;
    }
    if (ep->short_seen & (1u << b)) {
        got = ep->short_got[b];
        ep->short_seen &= ~(1u << b);
    }
    ep->busy &= ~(1u << b);
    ep->gen[b]++;
    ep->inflight -= ep->trbs[b];
    ep->fails = 0;
    if (ep->mode == EP_SYNC) {
        ep->sync_cc = (uint8_t)cc;
        ep->sync_got = got;
        ep->sync_done = true;
        wq_wake_all(&ep->wq);
        spin_give(&ep->lock);
        return;
    }
    spin_give(&ep->lock);
    if (ep->fn) ep->fn(&xd->dev, ep->ctx, ep->buf + (size_t)b * ep->stride, got, at);
    if (ep->mode != EP_STREAM) return;
    spin_take(&ep->lock);
    /* a recovery on the worker may have queued the whole stream again meanwhile */
    if (!removed(&xd->dev) && !ep->halted && !ep->dead && !(ep->busy & (1u << b))) {
        endpoint_push(ep, b, ep->len, false);
        doorbell(hc, slot, dci);
    }
    spin_give(&ep->lock);
}

static void command_event(struct usb_hc *hc, uint64_t param, uint32_t status, uint32_t control) {
    unsigned cc = status >> 24;
    spin_take(&hc->cmd_lock);
    if (cc == CC_CMD_RING_STOPPED) {
        hc->cmd_stopped = true;
    } else if (param == hc->cmd_trb) {
        hc->cmd_cc = (uint8_t)cc;
        hc->cmd_slot = (uint8_t)(control >> 24);
        hc->cmd_done = true;
    }
    wq_wake_all(&hc->cmd_wq);
    spin_give(&hc->cmd_lock);
}

/* Every event on the ring, ir->lock held, interrupts off. `at` is the moment the completions
   were taken (the interrupt, or the poll), which the reports carry. In an interrupt the Event
   Handler Busy flag is cleared even when nothing came (4.17.2: no interrupt follows while set). */
static void drain(struct interrupter *ir, uint64_t at, bool in_interrupt) {
    struct usb_hc *hc = ir->hc;
    if (!hc_running(hc)) return;
    bool any = false;
    for (;;) {
        struct trb *e = &ir->ev[ir->ev_idx];
        uint32_t control = e->control;
        if ((control & 1) != ir->ev_cycle) break;
        barrier();
        uint64_t param = e->param;
        uint32_t status = e->status;
        switch ((control >> 10) & 63) {
        case TRB_EV_TRANSFER: transfer_event(hc, param, status, control, at); break;
        case TRB_EV_COMMAND: command_event(hc, param, status, control); break;
        case TRB_EV_PORT: {
            unsigned port = (param >> 24) & 0xFF;
            if (port) __atomic_fetch_or(&hc->port_pending[port >> 5], 1u << (port & 31), __ATOMIC_RELEASE);
            hc_kick(hc);
            break;
        }
        default: break;
        }
        if (++ir->ev_idx == EV_TRBS) {
            ir->ev_idx = 0;
            ir->ev_cycle ^= 1;
        }
        any = true;
    }
    if (any || in_interrupt) wr64(ir->regs, ERDP, (ir->ev_phys + ir->ev_idx * sizeof(struct trb)) | ERDP_EHB);
}

/* An interrupter's message, on the CPU its irq is delivered to. */
static enum irq_result xhci_isr(void *context) {
    struct interrupter *ir = context;
    uint64_t at = clock_now_ns();
    spin_take(&ir->lock);
    if (hc_running(ir->hc)) {
        wr32(ir->hc->op, USBSTS, STS_EINT);
        wr32(ir->regs, IMAN, IMAN_IP | IMAN_IE);
        ir->interrupts++;
        drain(ir, at, true);
    }
    spin_give(&ir->lock);
    return IRQ_HANDLED;
}

/* A polled controller's rings looked at: as often as its fastest interrupt endpoint completes,
   and at least each millisecond for commands and control transfers. */
static uint64_t poll_period(struct usb_hc *hc) {
    for (unsigned i = 0; i < 16; i++)
        if (__atomic_load_n(&hc->periodic[i], __ATOMIC_RELAXED)) return MIN(125000ull << i, NS_PER_MS);
    return NS_PER_MS;
}

static void poll_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct usb_hc *hc = (struct usb_hc *)((char *)a - offsetof(struct usb_hc, poll));
    if (!hc_running(hc) || !__atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE)) return;
    uint64_t now = clock_now_ns();
    for (unsigned i = 0; i < hc->nintr; i++) {
        spin_take(&hc->intr[i].lock);
        drain(&hc->intr[i], now, false);
        spin_give(&hc->intr[i].lock);
    }
    alarm_set(a, now + poll_period(hc));
}

/* No interrupt comes: the controller interrupts no more, and the poll alarm drains its rings. */
static void hc_poll(struct usb_hc *hc) {
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~CMD_INTE);
    for (unsigned i = 0; i < hc->nintr; i++) wr32(hc->intr[i].regs, IMAN, IMAN_IP);
    __atomic_store_n(&hc->polled, true, __ATOMIC_RELEASE);
    alarm_set(&hc->poll, clock_now_ns());
}

/* Stopped, halted by a fatal error, or gone from the bus (reads all ones): nothing it is given
 * would complete, so nobody should wait for it. */
static bool hc_dead(struct usb_hc *hc) {
    if (!hc_running(hc)) return true;
    uint32_t sts = rd32(hc->op, USBSTS);
    return sts == 0xFFFFFFFFu || (sts & (STS_HSE | STS_HCE));
}

/* A command waited for and nothing arrived, while an event waits on interrupter 0's ring and no
   interrupt ever came: its message goes nowhere. True when the controller is polled from now on. */
static bool interrupts_lost(struct usb_hc *hc) {
    struct interrupter *ir = &hc->intr[0];
    if (__atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE)) return false;
    uint64_t f = spin_lock(&ir->lock);
    bool lost = !ir->interrupts && (ir->ev[ir->ev_idx].control & 1) == ir->ev_cycle;
    spin_unlock(&ir->lock, f);
    if (!lost) return false;
    kprintf("usb: controller %d interrupts never arrive, polling instead\n", hc->index);
    hc_poll(hc);
    return true;
}

/* Under cmd_lock: until the flag, the controller stopped, or the deadline. */
static bool command_wait(struct usb_hc *hc, const bool *flag, uint64_t deadline) {
    uint64_t f = spin_lock(&hc->cmd_lock);
    while (!*flag && hc_running(hc) && sleep_locked(&hc->cmd_wq, &hc->cmd_lock, deadline)) {}
    bool done = *flag;
    spin_unlock(&hc->cmd_lock, f);
    return done;
}

static int hc_command(struct usb_hc *hc, uint64_t param, uint32_t control, uint8_t *slot) {
    mutex_lock(&hc->cmd_mutex);
    int r = -1;
    /* wedged: where the ring would go on from is not known, until the reset */
    if (!hc_dead(hc) && !__atomic_load_n(&hc->wedged, __ATOMIC_ACQUIRE)) {
        uint64_t f = spin_lock(&hc->cmd_lock);
        hc->cmd_done = false;
        hc->cmd_trb = ring_push(&hc->cmd, param, 0, control);
        spin_unlock(&hc->cmd_lock, f);
        doorbell(hc, 0, 0);
        bool done = command_wait(hc, &hc->cmd_done, clock_after(3 * NS_PER_S));
        if (!done && interrupts_lost(hc)) done = command_wait(hc, &hc->cmd_done, clock_after(3 * NS_PER_S));
        if (done) {
            if (slot) *slot = hc->cmd_slot;
            r = hc->cmd_cc;
        } else if (hc_running(hc)) {
            kprintf("usb: controller %d command %u timed out\n", hc->index, (control >> 10) & 63);
            /* abort it so the commands queued after it can run: the ring stops with a Command Ring
               Stopped completion (4.6.1.2), and goes on from there at the next doorbell. The one
               64-bit write CRCR takes (5.1.1): bits 5:4 kept, its pointer and cycle ignored while
               the ring runs (5.4.5). A ring not running has nothing to abort, and a command it
               never answered; one that does not stop neither: the controller is reset (4.6.1.2). */
            f = spin_lock(&hc->cmd_lock);
            hc->cmd_stopped = false;
            spin_unlock(&hc->cmd_lock, f);
            uint32_t crcr = rd32(hc->op, CRCR);
            if (crcr & CRCR_CRR) wr64(hc->op, CRCR, (crcr & CRCR_RSVDP) | CRCR_CA);
            if (!(crcr & CRCR_CRR) || !command_wait(hc, &hc->cmd_stopped, clock_after(COMMAND_ABORT_NS))) {
                kprintf("usb: controller %d command ring does not stop\n", hc->index);
                __atomic_store_n(&hc->wedged, true, __ATOMIC_RELEASE);
                hc_kick(hc);
            }
        }
    }
    mutex_unlock(&hc->cmd_mutex);
    return r;
}

/* ---- device contexts ---- */

static uint32_t *in_ctx(struct xdev *xd, unsigned index) {
    return (uint32_t *)(xd->in_ctx + index * xd->dev.hc->ctx_size);
}

static uint32_t *out_ctx(struct xdev *xd, unsigned index) {
    return (uint32_t *)(xd->out_ctx + index * xd->dev.hc->ctx_size);
}

/* The input Slot Context. hub_fields: for Configure Endpoint (6.2.2.2); Address Device takes none. */
static void slot_context(struct xdev *xd, unsigned entries, bool hub_fields) {
    struct usb_device *d = &xd->dev;
    uint32_t *s = in_ctx(xd, 1);
    bool hub = hub_fields && xd->hub;
    s[0] = (d->route & 0xFFFFF) | (uint32_t)xd->speed_id << 20 | (hub && xd->multi_tt ? 1u << 25 : 0) |
           (hub ? 1u << 26 : 0) | (uint32_t)entries << 27;
    s[1] = (uint32_t)d->root_port << 16 | (hub ? (uint32_t)xd->hub_ports << 24 : 0);
    /* Interrupter Target 0: the events no transfer TRB names (4.17.4) go with the control ones */
    s[2] = d->tt_slot | (uint32_t)d->tt_port << 8 | (hub ? (uint32_t)(xd->think_time & 3) << 16 : 0);
    s[3] = 0;
}

/* An endpoint's state: its spinlock, and for the control one ctl_lock. */
static unsigned state_get(struct xdev *xd, unsigned dci) {
    struct endpoint *ep = dci == 1 ? NULL : xd->ep[dci];
    uint64_t f = spin_lock(ep ? &ep->lock : &xd->ctl_lock);
    unsigned state = ep ? ep->state : xd->ep0_state;
    spin_unlock(ep ? &ep->lock : &xd->ctl_lock, f);
    return state;
}

static void state_set(struct xdev *xd, unsigned dci, unsigned state) {
    struct endpoint *ep = dci == 1 ? NULL : xd->ep[dci];
    uint64_t f = spin_lock(ep ? &ep->lock : &xd->ctl_lock);
    if (ep) ep->state = (uint8_t)state;
    else xd->ep0_state = (uint8_t)state;
    spin_unlock(ep ? &ep->lock : &xd->ctl_lock, f);
}

/* The endpoint to where Set TR Dequeue Pointer takes it: a running one stopped, a halted one reset
   (TSP 0: its data toggle too, and *halted says so, if asked). A command refused with a Context State Error
   says the state followed here is not the endpoint's, whose output context then tells (4.6.9, 4.8.3):
   stopped or in error is where it was to go. */
static bool endpoint_quiet(struct xdev *xd, unsigned dci, bool *halted) {
    struct usb_hc *hc = xd->dev.hc;
    unsigned slot = xd->dev.slot, state = state_get(xd, dci);
    if (halted) *halted = false;
    if (state == EP_RUNNING) {
        int cc = hc_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | dci << 16 | slot << 24, NULL);
        if (cc == CC_SUCCESS) state = EP_STOPPED;
        else if (cc == CC_CONTEXT_STATE) state = out_ctx(xd, dci)[0] & 7;
        else return false;
    }
    if (state == EP_HALTED) {
        int cc = hc_command(hc, 0, TRB_TYPE(TRB_RESET_EP) | dci << 16 | slot << 24, NULL);
        if (cc == CC_SUCCESS) {
            if (halted) *halted = true;
            state = EP_STOPPED;
        } else if (cc == CC_CONTEXT_STATE) {
            state = out_ctx(xd, dci)[0] & 7;
        } else {
            return false;
        }
    }
    state_set(xd, dci, state);
    return state == EP_STOPPED || state == EP_ERROR;
}

/* What a transaction translator kept of a failed split transaction on a control or bulk endpoint of
   a low- or full-speed device behind a high-speed hub: cleared by CLEAR_TT_BUFFER (USB 2.0 11.17.5,
   11.24.2.3; xHCI 4.6.8). Direction is that of the split transaction: a control pipe has both, and
   a hub that holds no such buffer answers all the same. No TT per port is ever selected, so wIndex is 1.
   False: the buffer may stay busy for every device of that TT, the hub being there still. */
static bool tt_clear(struct xdev *xd, unsigned type, unsigned address, unsigned device) {
    struct usb_device *d = &xd->dev;
    if (d->speed >= USB_SPEED_SUPER || !d->tt_slot || (type != 0 && type != 2)) return true;
    struct usb_device *hub = d->parent;
    while (hub && hub->slot != d->tt_slot) hub = hub->parent;
    if (!hub) return true;
    uint16_t value = (uint16_t)((address & 15) | device << 4 | type << 11);
    bool ok = true;
    if (type == 0 || !(address & 0x80)) ok = usb_control(hub, 0x23, 8 /* CLEAR_TT_BUFFER */, value, 1, NULL, 0) >= 0;
    if (type == 0 || address & 0x80) ok = usb_control(hub, 0x23, 8, (uint16_t)(value | 1u << 15), 1, NULL, 0) >= 0 && ok;
    if (ok || removed(hub)) return true;
    kprintf("usb: slot %u: its hub's TT buffer was not cleared\n", d->slot);
    return false;
}

static bool set_dequeue(struct xdev *xd, unsigned dci, struct ring *r) {
    unsigned slot = xd->dev.slot;
    int cc = hc_command(xd->dev.hc, (r->phys + r->enq * sizeof(struct trb)) | r->cycle,
                        TRB_TYPE(TRB_SET_DEQUEUE) | dci << 16 | slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u endpoint %u could not be restarted (%d)\n", slot, dci, cc);
        return false;
    }
    state_set(xd, dci, EP_STOPPED);
    return true;
}

/* Bring a halted or stuck endpoint back to its ring's enqueue point on the controller's side alone: a
   control endpoint, or a transfer given up (pipe_reset is for the device's side too). A split transaction
   that failed can leave its TT's buffer busy (USB 2.0 11.17.5), one given up as much as one that halted.
   False leaves the old TRBs in place, so nothing may be queued after them. */
static bool endpoint_recover(struct xdev *xd, unsigned dci, struct ring *r, unsigned type, unsigned address) {
    if (!endpoint_quiet(xd, dci, NULL)) return false;
    tt_clear(xd, type, address, out_ctx(xd, 0)[3] & 0xFF);
    return set_dequeue(xd, dci, r);
}

/* After a failed control transfer the ring's dequeue point is wherever the controller stopped;
 * if it cannot be moved past the dead TD, a new request would run behind it or never at all.
 * ctl_mutex held. True when the TD is off the ring. */
static bool control_recover(struct xdev *xd) {
    if (removed(&xd->dev)) return false;
    if (endpoint_recover(xd, 1, &xd->ep0, 0, 0)) return true;
    xd->ctl_broken = true;
    kprintf("usb: slot %u control pipe is stuck, the device needs replugging\n", xd->dev.slot);
    return false;
}

/* ctl_mutex held. mem, phys: the data stage's memory, not the controller's any more if *over is set. */
static int control(struct xdev *xd, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *buffer,
                   uint16_t length, uint8_t *mem, uint64_t phys, bool *over) {
    struct usb_device *d = &xd->dev;
    struct usb_hc *hc = d->hc;
    *over = true;
    if (removed(d) || xd->ctl_broken || hc_dead(hc)) return -1;
    bool in = type & 0x80;
    if (!in && length) memcpy(mem, buffer, length);
    uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)length << 48;
    uint64_t f = spin_lock(&xd->ctl_lock);
    xd->ctl_done = false;
    xd->ctl_residual = 0;
    struct trb *first = ring_hold(&xd->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (length ? (in ? 3u : 2u) << 16 : 0), NULL);
    if (length) ring_push(&xd->ep0, phys, length, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    xd->ctl_status_trb = ring_push(&xd->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (length && in ? 0 : TRB_DIR_IN));
    ring_release(first);
    xd->ep0_state = EP_RUNNING;
    doorbell(hc, d->slot, 1);
    uint64_t deadline = clock_after(CONTROL_NS);
    while (!xd->ctl_done && !removed(d) && hc_running(hc) && sleep_locked(&xd->ctl_wq, &xd->ctl_lock, deadline)) {}
    bool done = xd->ctl_done;
    uint8_t cc = xd->ctl_cc;
    uint32_t residual = xd->ctl_residual;
    spin_unlock(&xd->ctl_lock, f);
    if (!done) {
        *over = false;
        if (removed(d) || !hc_running(hc)) return -1;
        kprintf("usb: slot %u request %02x:%02x timed out\n", d->slot, type, request);
        *over = control_recover(xd);
        return -1;
    }
    if (cc != CC_SUCCESS) {
        control_recover(xd); /* halted, it moves no more */
        return -1;
    }
    int got = length - (int)(residual < length ? residual : length);
    if (in && got > 0) memcpy(buffer, mem, got);
    return got;
}

/* A data stage that does not fit the device's page has 64 KiB of its own, all of wLength (USB 2.0
   9.3.5), aligned as they are, so one TRB may cover it (6.4.1); kept if the controller may still
   be using it. */
#define CONTROL_LONG_PAGES 16

int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length) {
    struct xdev *xd = (struct xdev *)d;
    bool lng = length > 4096, over;
    uint64_t phys = xd->xfer_phys;
    uint8_t *mem = lng ? dma_block(CONTROL_LONG_PAGES, &phys) : xd->xfer;
    if (!mem) return -1;
    mutex_lock(&xd->ctl_mutex);
    int r = control(xd, type, request, value, index, buffer, length, mem, phys, &over);
    mutex_unlock(&xd->ctl_mutex);
    if (lng && over) dma_free(phys, CONTROL_LONG_PAGES);
    return r;
}

/* The Endpoint Context's Interval, an exponent of 125 us microframes, from bInterval (Table 6-12):
   full-speed isochronous 2^(bInterval-1) frames; high and SuperSpeed 2^(bInterval-1) microframes;
   low- and full-speed interrupt bInterval frames, rounded down to a power of two. */
static unsigned endpoint_interval(unsigned speed, unsigned type, unsigned interval) {
    if (speed == USB_SPEED_HIGH || speed >= USB_SPEED_SUPER) {
        if (interval < 1) interval = 1;
        return interval > 16 ? 15 : interval - 1;
    }
    if (type == 1) {
        if (interval < 1) interval = 1;
        return (interval > 16 ? 16 : interval) + 2;
    }
    unsigned frames = (interval ? interval : 1) * 8, log = 0; /* in 125 us microframes */
    while ((2u << log) <= frames) log++;
    return log < 3 ? 3 : log > 10 ? 10 : log;
}

static unsigned address_dci(uint8_t address) { return (address & 15) * 2 + ((address & 0x80) ? 1 : 0); }

static void free_endpoint(struct usb_hc *hc, struct endpoint *ep) {
    if (!ep) return;
    if (ep->type == 3) __atomic_fetch_sub(&hc->periodic[ep->interval], 1, __ATOMIC_RELAXED);
    if (ep->ring.trb) pmm_free(ep->ring.phys);
    if (ep->buf) dma_free(ep->buf_phys, ep->pages);
    kfree(ep);
}

/* The DMA area for `count` transfers of `len` bytes, 64-byte aligned each. Before the endpoint
   gets its mode, by the one thread that sets it. */
static bool endpoint_buffer(struct endpoint *ep, unsigned len, unsigned count) {
    if (ep->buf) return ep->len >= len && ep->count >= count;
    if (!len || !count || count > EP_XFERS) return false;
    unsigned stride = (len + 63) & ~63u, pages = (stride * count + 4095) / 4096;
    uint64_t phys = pmm_alloc_contig(pages, 64);
    if (!phys) return false;
    ep->buf = phys_to_virt(phys);
    memset(ep->buf, 0, pages * 4096ull);
    ep->buf_phys = phys;
    ep->pages = pages;
    ep->len = len;
    ep->stride = stride;
    ep->count = count;
    return true;
}

static struct endpoint *find_endpoint(struct usb_device *d, uint8_t address) {
    return removed(d) ? NULL : ((struct xdev *)d)->ep[address_dci(address)];
}

static void free_device(struct xdev *xd) {
    for (int i = 0; i < 32; i++) free_endpoint(xd->dev.hc, xd->ep[i]);
    if (xd->out_ctx) pmm_free(xd->out_phys);
    if (xd->in_ctx) pmm_free(xd->in_phys);
    if (xd->ep0.trb) pmm_free(xd->ep0.phys);
    if (xd->xfer) pmm_free(xd->xfer_phys);
    kfree(xd);
}

static bool set_dequeue(struct xdev *xd, unsigned dci, struct ring *r);

/* What Disable Slot asks first (4.6.4's note): each endpoint stopped, one halted or in error brought to
   stopped its own way, and the events of what was in them received; no driver waits for them now. */
static void slot_quiet(struct usb_hc *hc, unsigned slot, struct xdev *xd) {
    /* an Enabled slot, whose Slot State reads 0, has no endpoint to stop (4.6.9) */
    bool addressed = xd && xd->out_ctx && out_ctx(xd, 0)[3] >> 27;
    for (unsigned dci = 1; addressed && dci < 32; dci++) {
        struct endpoint *ep = dci == 1 ? NULL : xd->ep[dci];
        if (dci != 1 && !ep) continue;
        if (hc_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | dci << 16 | slot << 24, NULL) != CC_CONTEXT_STATE) continue;
        unsigned state = out_ctx(xd, dci)[0] & 7;
        if (state == EP_HALTED) hc_command(hc, 0, TRB_TYPE(TRB_RESET_EP) | dci << 16 | slot << 24, NULL);
        else if (state == EP_ERROR) set_dequeue(xd, dci, ep ? &ep->ring : &xd->ep0);
    }
    uint64_t now = clock_now_ns();
    for (unsigned i = 0; i < hc->nintr; i++) {
        uint64_t f = spin_lock(&hc->intr[i].lock);
        drain(&hc->intr[i], now, false);
        spin_unlock(&hc->intr[i].lock, f);
    }
}

/* The controller owns the device context and rings until Disable Slot completes; if it never
 * does, the memory stays allocated rather than handed back while the controller may still use it.
 * No drain looks at the slot once it is cleared here. */
static void release_slot(struct usb_hc *hc, unsigned slot, struct xdev *xd) {
    __atomic_store_n(&hc->slots[slot], NULL, __ATOMIC_RELEASE);
    sync_drains(hc);
    /* a controller stopped for a restart holds nothing of its old slots any more */
    int cc = CC_SUCCESS;
    if (hc_running(hc)) {
        slot_quiet(hc, slot, xd);
        cc = hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
    }
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u could not be disabled (%d), its memory is kept\n", slot, cc);
        return;
    }
    hc->dcbaa[slot] = 0;
    if (xd) free_device(xd);
}

void usb_hub_info(struct usb_device *d, unsigned ports, unsigned think_time, bool multi_tt) {
    struct xdev *xd = (struct xdev *)d;
    xd->hub = true;
    xd->hub_ports = ports;
    xd->think_time = d->speed == USB_SPEED_HIGH ? think_time : 0; /* TTT is a high-speed hub's (Table 6-6) */
    xd->multi_tt = multi_tt;
}

static void endpoint_info(const struct usb_endpoint_desc *e, const uint8_t *companion, const uint8_t *ssp,
                          struct usb_ep_info *out) {
    *out = (struct usb_ep_info){.address = e->address, .attributes = e->attributes, .interval = e->interval,
                                .max_packet = e->max_packet};
    if (companion) {
        out->burst = companion[2];
        out->companion_attributes = companion[3];
        out->esit = (uint16_t)(companion[4] | companion[5] << 8);
    }
    if (ssp) out->esit = ssp[4] | ssp[5] << 8 | ssp[6] << 16 | (uint32_t)ssp[7] << 24; /* dwBytesPerInterval */
}

bool usb_endpoint_info(const uint8_t *cfg, unsigned total, unsigned off, struct usb_ep_info *ep) {
    if (off + 7 > total || cfg[off] < 7 || cfg[off + 1] != USB_DESC_ENDPOINT) return false;
    unsigned next = off + cfg[off];
    const uint8_t *companion = next + 6 <= total && cfg[next] >= 6 && cfg[next + 1] == USB_DESC_COMPANION
                                   ? cfg + next : NULL, *ssp = NULL;
    /* an isochronous endpoint with its companion's bit 7 has a SuperSpeedPlus one after it (USB 3.2 9.6.7) */
    if (companion && (cfg[off + 3] & 3) == 1 && companion[3] & 0x80) {
        unsigned after = next + companion[0];
        if (after + 8 <= total && cfg[after] >= 8 && cfg[after + 1] == USB_DESC_SSP_COMPANION) ssp = cfg + after;
    }
    endpoint_info((const void *)(cfg + off), companion, ssp, ep);
    return true;
}

/* Fill endpoint context dci for `ep` in the input context (6.2.3, Table 6-12). */
static void endpoint_context(struct xdev *xd, const struct endpoint *ep, unsigned burst, unsigned mult) {
    uint32_t *c = in_ctx(xd, ep->dci + 1);
    bool in = ep->address & 0x80, timed = ep->type == 1 || ep->type == 3;
    unsigned ep_type = ep->type + (in ? 4 : 0); /* isochronous 1/5, bulk 2/6, interrupt 3/7 */
    c[0] = mult << 8 | (uint32_t)ep->interval << 16 | (timed && xd->dev.hc->lec ? (ep->esit >> 16) << 24 : 0);
    /* errors counted to 3; an isochronous endpoint does not count them (4.3.6, 6.2.3) */
    c[1] = (ep->type == 1 ? 0u : 3u << 1) | ep_type << 3 | burst << 8 | ep->mps << 16;
    c[2] = (uint32_t)(ep->ring.phys + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.phys + ep->ring.enq * sizeof(struct trb)) >> 32);
    /* Average TRB Length, to matter most for a periodic endpoint (4.14.1.1): an interrupt endpoint's
       transfers are a packet or a few; 3 KiB for bulk and isochronous. Max ESIT Payload. */
    c[4] = (ep->type == 3 ? (uint32_t)ep->mps : 3072u) | (timed ? (ep->esit & 0xFFFF) << 16 : 0);
}

/* Every endpoint but the control one is gone from the controller: nothing refers to them. */
static void endpoints_free(struct xdev *xd) {
    struct usb_hc *hc = xd->dev.hc;
    struct endpoint *gone[32];
    for (int i = 2; i < 32; i++) {
        gone[i] = xd->ep[i];
        __atomic_store_n(&xd->ep[i], NULL, __ATOMIC_RELEASE);
    }
    sync_drains(hc);
    for (int i = 2; i < 32; i++) free_endpoint(hc, gone[i]);
}

/* Contexts for the endpoints not open yet, all in one Configure Endpoint (4.6.6). If it fails the
   endpoints it would have added are not there (4.8.3: their contexts stay Disabled). */
static bool open_endpoints(struct xdev *xd, const struct usb_ep_info *eps, int n) {
    struct usb_hc *hc = xd->dev.hc;
    mutex_lock(&xd->in_mutex);
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0);
    unsigned top = 1;
    uint32_t added = 0;
    struct endpoint *made[32] = {0};
    icc[1] = 1;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        const struct usb_ep_info *e = &eps[i];
        unsigned dci = address_dci(e->address), type = e->attributes & 3, mps = e->max_packet & 0x7FF;
        if (dci < 2 || xd->ep[dci] || made[dci] || type == 0 || !mps) continue; /* mps 0: carries nothing */
        struct endpoint *ep = kzalloc(sizeof *ep);
        if (!ep || !ring_init(&ep->ring)) {
            if (ep) kfree(ep);
            ok = false;
            break;
        }
        unsigned burst = 0, mult = 0;
        uint32_t esit = 0;
        if (type == 1 || type == 3) {
            /* high speed: extra transactions per microframe; low and full speed have none (6.2.3.4) */
            burst = xd->dev.speed == USB_SPEED_HIGH ? (e->max_packet >> 11) & 3 : 0;
            esit = mps * (burst + 1);
        }
        if (xd->dev.speed >= USB_SPEED_SUPER) { /* burst and payload come from the companion */
            burst = e->burst;
            /* Mult is ignored with a SuperSpeedPlus companion (USB 3.2 9.6.7), and reserved in the
               context of a controller with Large ESIT Payload (Table 6-8) */
            if (type == 1 && !(e->companion_attributes & 0x80)) mult = e->companion_attributes & 3;
            if (type == 1 || type == 3) esit = e->esit ? e->esit : mps * (burst + 1) * (mult + 1);
            if (hc->lec) mult = 0;
        }
        ep->dci = (uint8_t)dci;
        ep->address = e->address;
        ep->type = (uint8_t)type;
        ep->state = EP_RUNNING; /* after Configure Endpoint (4.8.3) */
        ep->mps = mps;
        ep->esit = esit;
        ep->target = hc->target[type == 3 ? RING_PERIODIC : RING_BULK];
        if (type == 1 || type == 3) ep->interval = (uint8_t)endpoint_interval(xd->dev.speed, type, e->interval);
        /* bulk OUT: its NAK rate, of 0 to 255 in USB 2.0 Table 9-13 and 0 to 15 in the field (6.2.3.6) */
        else if (xd->dev.speed == USB_SPEED_HIGH && !(e->address & 0x80)) ep->interval = MIN(e->interval, 15);
        if (type == 3) __atomic_fetch_add(&hc->periodic[ep->interval], 1, __ATOMIC_RELAXED);
        endpoint_context(xd, ep, burst, mult);
        made[dci] = ep;
        added |= 1u << dci;
        icc[1] |= 1u << dci;
        if (dci > top) top = dci;
    }
    if (ok && !added) {
        mutex_unlock(&xd->in_mutex);
        return true;
    }
    if (ok) {
        unsigned entries = out_ctx(xd, 0)[0] >> 27;
        slot_context(xd, top > entries ? top : entries, true);
        for (uint32_t m = added; m; m &= m - 1)
            __atomic_store_n(&xd->ep[__builtin_ctz(m)], made[__builtin_ctz(m)], __ATOMIC_RELEASE);
        int cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_CONFIGURE) | (uint32_t)xd->dev.slot << 24, NULL);
        if (cc == CC_SUCCESS) {
            mutex_unlock(&xd->in_mutex);
            return true;
        }
        kprintf("usb: slot %u endpoint configuration failed (%d)\n", xd->dev.slot, cc);
        for (uint32_t m = added; m; m &= m - 1) __atomic_store_n(&xd->ep[__builtin_ctz(m)], NULL, __ATOMIC_RELEASE);
        sync_drains(hc);
    }
    mutex_unlock(&xd->in_mutex);
    for (uint32_t m = added; m; m &= m - 1) free_endpoint(hc, made[__builtin_ctz(m)]);
    return false;
}

bool usb_open_endpoints(struct usb_device *d, const struct usb_ep_info *eps, int n) {
    return !removed(d) && open_endpoints((struct xdev *)d, eps, n);
}

bool usb_stream_start(struct usb_device *d, uint8_t address, unsigned len, unsigned count, usb_report_fn fn, void *ctx) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep || ep->mode || !(address & 0x80) || !endpoint_buffer(ep, len, count)) return false;
    unsigned per = len / 0x10000 + 2; /* entries one transfer can take */
    if (!ring_room(ep, per * count)) return false;
    uint64_t f = spin_lock(&ep->lock);
    ep->fn = fn;
    ep->ctx = ctx;
    ep->mode = EP_STREAM;
    endpoint_requeue_all(d->hc, (struct xdev *)d, ep);
    spin_unlock(&ep->lock, f);
    return true;
}

/* A TD ends at a short packet or at its length: `longest` in whole packets, so a shorter transfer is
 * not held waiting for more. */
unsigned usb_interrupt_length(struct usb_device *d, const struct usb_endpoint_desc *e, unsigned longest) {
    struct endpoint *ep = find_endpoint(d, e->address);
    if (!ep) return 0;
    return (longest + ep->mps - 1) / ep->mps * ep->mps;
}

bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *e, unsigned longest, usb_report_fn fn,
                         void *ctx) {
    unsigned len = usb_interrupt_length(d, e, longest);
    if (!len) return false;
    unsigned count = 4096 / ((len + 63) & ~63u);
    return usb_stream_start(d, e->address, len, count < 1 ? 1 : count > 8 ? 8 : count, fn, ctx);
}

/* Reset a pipe on both sides. Halted, Reset Endpoint clears the controller's data toggle; otherwise
 * stopped (one in error by Set TR Dequeue, 4.8.3) and Dropped and Added in one Configure Endpoint
 * (4.6.8's last note). The device's side is CLEAR_FEATURE(ENDPOINT_HALT), in between for a halted one
 * (4.6.8's note). Its ring goes on from the enqueue point. False: a part did not go through, and
 * the caller keeps the pipe halted. */
static bool pipe_reset(struct xdev *xd, struct endpoint *ep) {
    struct usb_device *d = &xd->dev;
    unsigned slot = d->slot, dci = ep->dci;
    bool halted;
    if (!endpoint_quiet(xd, dci, &halted)) return false;
    if (halted) {
        bool cleared = tt_clear(xd, ep->type, ep->address, out_ctx(xd, 0)[3] & 0xFF);
        return usb_control(d, 0x02, USB_REQ_CLEAR_FEATURE, 0, ep->address, NULL, 0) >= 0 &&
               set_dequeue(xd, dci, &ep->ring) && cleared;
    }
    if (state_get(xd, dci) == EP_ERROR && !set_dequeue(xd, dci, &ep->ring)) return false;
    mutex_lock(&xd->in_mutex);
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0), *c = in_ctx(xd, dci + 1);
    icc[0] = 1u << dci;
    icc[1] = 1u | 1u << dci;
    memcpy(c, out_ctx(xd, dci), 5 * sizeof *c); /* what follows is the controller's (6.2.3) */
    c[0] &= ~7u;
    c[2] = (uint32_t)(ep->ring.phys + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.phys + ep->ring.enq * sizeof(struct trb)) >> 32);
    slot_context(xd, out_ctx(xd, 0)[0] >> 27, true);
    int cc = hc_command(d->hc, xd->in_phys, TRB_TYPE(TRB_CONFIGURE) | slot << 24, NULL);
    mutex_unlock(&xd->in_mutex);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u endpoint %u could not be reset (%d)\n", slot, dci, cc);
        return false;
    }
    state_set(xd, dci, EP_RUNNING);
    bool cleared = tt_clear(xd, ep->type, ep->address, out_ctx(xd, 0)[3] & 0xFF);
    return usb_control(d, 0x02, USB_REQ_CLEAR_FEATURE, 0, ep->address, NULL, 0) >= 0 && cleared;
}

/* From here until both sides are done the pipe is halted and transfers are refused, whatever it was. */
int usb_clear_halt(struct usb_device *d, uint8_t address) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep) return -1;
    endpoint_restart(ep, true);
    if (!pipe_reset((struct xdev *)d, ep)) return -1;
    uint64_t f = spin_lock(&ep->lock);
    ep->halted = false;
    spin_unlock(&ep->lock, f);
    return 0;
}

int usb_bulk(struct usb_device *d, uint8_t address, void *buf, unsigned len, unsigned timeout_ms, unsigned *moved) {
    struct xdev *xd = (struct xdev *)d;
    struct endpoint *ep = find_endpoint(d, address);
    *moved = 0;
    if (!ep || ep->type != 2 || (ep->mode && ep->mode != EP_SYNC) || len > SYNC_BYTES || hc_dead(d->hc)) return -1;
    if (!ep->mode) {
        if (!endpoint_buffer(ep, SYNC_BYTES, 1)) return -1;
        uint64_t f = spin_lock(&ep->lock);
        ep->mode = EP_SYNC;
        spin_unlock(&ep->lock, f);
    }
    bool in = address & 0x80;
    if (!in && len) memcpy(ep->buf, buf, len);
    uint64_t f = spin_lock(&ep->lock);
    if (ep->halted) {
        spin_unlock(&ep->lock, f);
        return USB_STALL;
    }
    ep->sync_done = false;
    endpoint_push(ep, 0, len, false);
    doorbell(d->hc, d->slot, ep->dci);
    uint64_t deadline = clock_after(timeout_ms * NS_PER_MS);
    while (!ep->sync_done && !removed(d) && hc_running(d->hc) && sleep_locked(&ep->wq, &ep->lock, deadline)) {}
    bool done = ep->sync_done;
    uint8_t cc = ep->sync_cc;
    unsigned got = ep->sync_got;
    spin_unlock(&ep->lock, f);
    if (!done) {
        if (removed(d) || !hc_running(d->hc)) return -1;
        kprintf("usb: slot %u endpoint %02x transfer timed out\n", d->slot, address);
        bool stuck = !endpoint_recover(xd, ep->dci, &ep->ring, ep->type, address); /* stop it and skip the TD */
        endpoint_restart(ep, stuck);
        return USB_TIMEOUT;
    }
    /* what arrived before a stall or an error is the caller's too (BOT 6.7.2) */
    *moved = got;
    if (in && got) memcpy(buf, ep->buf, got);
    if (cc == CC_STALL) return USB_STALL;
    if (cc != CC_SUCCESS && cc != CC_SHORT) return -1;
    return 0;
}

bool usb_send_open(struct usb_device *d, uint8_t address, unsigned size, unsigned slots, usb_report_fn done, void *ctx) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep || ep->mode || (address & 0x80) || ep->type != 2 || !endpoint_buffer(ep, size, slots)) return false;
    uint64_t f = spin_lock(&ep->lock);
    ep->fn = done;
    ep->ctx = ctx;
    ep->mode = EP_SEND;
    spin_unlock(&ep->lock, f);
    return true;
}

int usb_send(struct usb_device *d, uint8_t address, const void *a, unsigned alen, const void *b, unsigned blen) {
    struct endpoint *ep = find_endpoint(d, address);
    unsigned len = alen + blen;
    if (!ep || ep->mode != EP_SEND || len > ep->len || !len) return -1;
    uint64_t f = spin_lock(&ep->lock);
    if (ep->dead) {
        spin_unlock(&ep->lock, f);
        return -1;
    }
    int r = 0;
    for (unsigned i = 0; i < ep->count && !ep->halted; i++) {
        unsigned k = (ep->next + i) % ep->count;
        if (ep->busy & (1u << k)) continue;
        if (!ring_room(ep, len / 0x10000 + 3)) break;
        uint8_t *slot = ep->buf + (size_t)k * ep->stride;
        memcpy(slot, a, alen);
        if (blen) memcpy(slot + alen, b, blen);
        endpoint_push(ep, k, len, len % ep->mps == 0);
        doorbell(d->hc, d->slot, ep->dci);
        ep->next = k + 1;
        r = 1;
        break;
    }
    spin_unlock(&ep->lock, f);
    return r;
}

static const char *speed_name(unsigned s) {
    switch (s) {
    case USB_SPEED_LOW: return "low";
    case USB_SPEED_FULL: return "full";
    case USB_SPEED_HIGH: return "high";
    case USB_SPEED_SUPER: return "super";
    default: return "super+";
    }
}

/* Each interface of alternate 0 that takes an interrupt IN endpoint's reports, to its class driver,
   with the descriptors between it and that endpoint. A device is one hub: a second hub interface
   would share its state. */
static void bind_classes(struct xdev *xd, uint8_t *cfg, unsigned total) {
    struct usb_device *d = &xd->dev;
    bool hub_taken = false;
    for (unsigned off = 0; off + 2 <= total && !removed(d);) {
        unsigned len = cfg[off];
        if (len < 2 || off + len > total) break;
        const struct usb_interface_desc *intf = (const void *)(cfg + off);
        off += len;
        if (cfg[off - len + 1] != USB_DESC_INTERFACE || len < 9 || intf->alternate) continue;
        bool hub = intf->cls == 9;
        if (!hub && !usbhid_claims(intf)) continue;
        const uint8_t *extra = cfg + off;
        unsigned extra_len = 0;
        const struct usb_endpoint_desc *ep = NULL;
        for (unsigned at = off; at + 2 <= total && cfg[at] >= 2 && at + cfg[at] <= total && !ep; at += cfg[at]) {
            if (cfg[at + 1] == USB_DESC_INTERFACE) break;
            if (cfg[at + 1] == USB_DESC_ENDPOINT && cfg[at] >= 7) {
                const struct usb_endpoint_desc *e = (const void *)(cfg + at);
                if ((e->address & 0x80) && (e->attributes & 3) == 3) ep = e;
            } else {
                extra_len = (unsigned)(cfg + at + cfg[at] - extra);
            }
        }
        if (!ep) continue;
        if (hub) {
            if (!hub_taken && d->hub) usbhub_attach(d, ep);
            hub_taken = true;
        } else {
            usbhid_attach(d, intf, extra, extra_len, ep);
        }
    }
}

/* Every endpoint the configuration enables: those of each interface's alternate 0 (xHCI 3.3.5), whatever
   the interface is for. A kmalloc'ed list, NULL with no memory. */
static struct usb_ep_info *configuration_endpoints(const uint8_t *cfg, unsigned total, int *count) {
    unsigned room = 0;
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off])
        if (cfg[off + 1] == USB_DESC_ENDPOINT) room++;
    struct usb_ep_info *eps = kmalloc((room ? room : 1) * sizeof *eps);
    if (!eps) return NULL;
    int n = 0;
    bool first = false;
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total; off += cfg[off]) {
        if (cfg[off + 1] == USB_DESC_INTERFACE && cfg[off] >= 9) first = !((const struct usb_interface_desc *)(cfg + off))->alternate;
        else if (first && usb_endpoint_info(cfg, total, off, &eps[n])) n++;
    }
    *count = n;
    return eps;
}

/* Out of the configuration it was to have: the slot's endpoints go and the device is told, together
   (4.6.6). A controller that keeps them keeps their rings too. */
static void deconfigure(struct xdev *xd) {
    struct usb_device *d = &xd->dev;
    int cc = hc_command(d->hc, 0, TRB_TYPE(TRB_CONFIGURE) | TRB_DC | (uint32_t)d->slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u could not be deconfigured (%d)\n", d->slot, cc);
        return;
    }
    endpoints_free(xd);
    usb_control(d, 0, USB_REQ_SET_CONFIG, 0, 0, NULL, 0);
}

/* The configuration's endpoints in the controller, and only then the device told to use it (3.3.5,
   4.3.5). A controller that refuses them (Resource, Bandwidth Error) leaves the device addressed. */
static bool configure(struct xdev *xd, const uint8_t *cfg, unsigned total) {
    struct usb_device *d = &xd->dev;
    int n = 0;
    struct usb_ep_info *eps = configuration_endpoints(cfg, total, &n);
    if (!eps) return false;
    bool opened = open_endpoints(xd, eps, n);
    kfree(eps);
    if (!opened) return false;
    if (usb_control(d, 0, USB_REQ_SET_CONFIG, cfg[5], 0, NULL, 0) >= 0) return true;
    if (out_ctx(xd, 0)[3] >> 27 == SLOT_CONFIGURED) deconfigure(xd); /* no endpoint added: it never was */
    return false;
}

/* Does an interface of this configuration have a driver? */
static bool configuration_supported(const struct usb_device *d, const uint8_t *cfg, unsigned total) {
    (void)d;
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] != USB_DESC_INTERFACE || cfg[off] < 9 || off + 9 > total) continue;
        const struct usb_interface_desc *intf = (const void *)(cfg + off);
        if (intf->cls == 9 || usbhid_claims(intf) || usbmsc_claims(intf) ||
            usbnet_claims(intf))
            return true;
    }
    return false;
}

/* The first configuration a driver can use, else the first one (USB 2.0 9.4.3: descriptor
 * index, not bConfigurationValue). Network adapters often put their standard (CDC) function
 * in a second or third configuration after a vendor-specific first one. */
static uint8_t *choose_configuration(struct usb_device *d, int *got) {
    unsigned count = d->desc.configurations ? d->desc.configurations : 1;
    uint8_t *first = NULL;
    int first_got = 0;
    for (unsigned i = 0; i < count && i < 8; i++) {
        uint8_t head[9];
        if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8 | i, 0, head, 9) < 9) continue;
        unsigned total = head[2] | head[3] << 8;
        if (total < 9) continue;
        uint8_t *cfg = kmalloc(total);
        if (!cfg) break;
        int n = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8 | i, 0, cfg, total);
        if (n < 9) {
            kfree(cfg);
            continue;
        }
        if (configuration_supported(d, cfg, (unsigned)n)) {
            if (i) kprintf("usb: slot %u uses configuration %u of %u\n", d->slot, cfg[5], count);
            kfree(first);
            *got = n;
            return cfg;
        }
        if (first) kfree(cfg);
        else {
            first = cfg;
            first_got = n;
        }
    }
    *got = first_got;
    return first;
}

/* Address Device from the slot's Default state: the slot's route and the control endpoint at its ring's
   enqueue point. A split transaction of its SET_ADDRESS may stay in a translator (4.6.5's note; address 0,
   control). Its result as hc_command's. */
static int address_device(struct xdev *xd) {
    mutex_lock(&xd->in_mutex);
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0), *ep0 = in_ctx(xd, 2);
    icc[1] = 3;
    slot_context(xd, 1, false);
    ep0[1] = 3u << 1 | 4u << 3 | (uint32_t)xd->ep0_mps << 16; /* error count 3, control */
    ep0[2] = (uint32_t)(xd->ep0.phys + xd->ep0.enq * sizeof(struct trb)) | xd->ep0.cycle;
    ep0[3] = (uint32_t)((xd->ep0.phys + xd->ep0.enq * sizeof(struct trb)) >> 32);
    ep0[4] = 8;
    int cc = hc_command(xd->dev.hc, xd->in_phys, TRB_TYPE(TRB_ADDRESS) | (uint32_t)xd->dev.slot << 24, NULL);
    mutex_unlock(&xd->in_mutex);
    if (cc == CC_TRANSACTION_ERROR) tt_clear(xd, 0, 0, 0);
    return cc;
}

static bool port_reset(struct usb_hc *hc, unsigned p, bool warm);

/* The slot through its Default state again, the one way its hub fields change after their first
   Configure Endpoint (4.5.2): the device reset at its port, Reset Device (4.6.11), Address Device,
   whose control ring starts anew with its Dequeue Cycle State 1 (6.2.3.1). */
static bool device_readdress(struct xdev *xd) {
    struct usb_device *d = &xd->dev;
    struct usb_hc *hc = d->hc;
    if (!(d->parent ? usbhub_reset_port(d->parent, d->port) : port_reset(hc, d->port, false))) return false;
    int cc = hc_command(hc, 0, TRB_TYPE(TRB_RESET_DEVICE) | (uint32_t)d->slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u could not be reset (%d)\n", d->slot, cc);
        return false;
    }
    endpoints_free(xd);
    uint64_t f = spin_lock(&xd->ctl_lock);
    ring_reset(&xd->ep0);
    xd->ep0_state = EP_RUNNING;
    spin_unlock(&xd->ctl_lock, f);
    if ((cc = address_device(xd)) != CC_SUCCESS) {
        kprintf("usb: slot %u address assignment failed (%d)\n", d->slot, cc);
        return false;
    }
    sleep_until(clock_after(2 * NS_PER_MS)); /* SetAddress recovery (USB 2.0 9.2.6.3) */
    return true;
}

/* A device with a hub interface in the configuration. */
static bool configuration_has_hub(const uint8_t *cfg, unsigned total) {
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off])
        if (cfg[off + 1] == USB_DESC_INTERFACE && cfg[off] >= 9 && off + 9 <= total &&
            cfg[off + 5] == 9 && !cfg[off + 3])
            return true;
    return false;
}

/* The Protocol Speed ID for a kind of speed on root port p, as the port's protocol lists it (7.2.1),
   or by default (7.2.2.1.1). For a device behind a hub, whose speed the hub reports by kind. */
static uint64_t psi_rate(uint32_t psi) { /* bits a second */
    uint64_t rate = psi >> 16;
    for (unsigned e = (psi >> 4) & 3; e; e--) rate *= 1000;
    return rate;
}

static unsigned speed_kind_of_psi(unsigned major, uint32_t psi) {
    uint64_t rate = psi_rate(psi);
    if (major >= 3) return (psi >> 14 & 3) == 1 ? USB_SPEED_SUPER_PLUS : USB_SPEED_SUPER;
    return rate <= 1500000 ? USB_SPEED_LOW : rate <= 12000000 ? USB_SPEED_FULL : USB_SPEED_HIGH;
}

/* The Port Speed of a device of this kind and rank (USB 3.2 7.5.4.5.1) behind root port p: the listed one
   of its rate, 20 Gb/s for Gen2x2 and 10 for the two below (Table 7-13), the ID of Table 7-13 if that is
   among them, since a PSI does not say the lanes of Gen1x2 and Gen2x1; by default that ID. */
static unsigned port_speed_id(const struct usb_hc *hc, unsigned p, unsigned kind, unsigned rank) {
    uint64_t rate = rank == 3 ? 20000000000ull : 10000000000ull;
    unsigned own = kind != USB_SPEED_SUPER_PLUS ? kind : rank == 1 ? 6 : rank == 3 ? 7 : 5;
    unsigned of_rate = 0, of_kind = 0;
    for (unsigned i = 0; i < hc->port_psic[p]; i++) {
        uint32_t psi = hc->port_psi[p][i];
        if (speed_kind_of_psi(hc->port_major[p], psi) != kind) continue;
        if (kind != USB_SPEED_SUPER_PLUS) return psi & 15;
        if (psi_rate(psi) == rate) {
            if ((psi & 15) == own) return own;
            if (!of_rate) of_rate = psi & 15;
        }
        if (!of_kind) of_kind = psi & 15;
    }
    return of_rate ? of_rate : of_kind ? of_kind : own;
}

/* The kind of a Port Speed that root port p reads. */
static unsigned port_speed_kind(const struct usb_hc *hc, unsigned p, unsigned id) {
    for (unsigned i = 0; i < hc->port_psic[p]; i++)
        if ((hc->port_psi[p][i] & 15) == id) return speed_kind_of_psi(hc->port_major[p], hc->port_psi[p][i]);
    return id >= 5 ? USB_SPEED_SUPER_PLUS : id;
}

/* The rank of the link root port p reads (USB 3.2 7.5.4.5.1). By default its ID says (Table 7-13); a listed
   one says the rate of all its lanes, of which PORTLI.RLC has the number (5.4.10.1, 7.2.1). */
static unsigned port_speed_rank(const struct usb_hc *hc, unsigned p, unsigned id) {
    for (unsigned i = 0; i < hc->port_psic[p]; i++) {
        uint32_t psi = hc->port_psi[p][i];
        if ((psi & 15) != id) continue;
        if (speed_kind_of_psi(hc->port_major[p], psi) != USB_SPEED_SUPER_PLUS) return 0;
        unsigned lanes = (rd32(hc->op, PORTLI(p)) >> 16 & 15) + 1;
        bool gen2 = psi_rate(psi) / lanes >= 10000000000ull;
        return lanes > 1 ? (gen2 ? 3 : 1) : (gen2 ? 2 : 0);
    }
    return id == 5 ? 2 : id == 6 ? 1 : id == 7 ? 3 : 0;
}

static struct xdev *device_attach(struct usb_hc *hc, struct usb_device *parent, unsigned port, unsigned root_port,
                                  uint32_t route, unsigned speed, unsigned speed_id, unsigned rank, unsigned depth,
                                  unsigned tt_slot, unsigned tt_port) {
    uint8_t slot = 0;
    int cc = hc_command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT) | (uint32_t)hc->port_slot_type[root_port] << 16, &slot);
    if (cc != CC_SUCCESS || !slot || slot > hc->max_slots) {
        kprintf("usb: no device slot for port %u (%d)\n", port, cc);
        return NULL;
    }
    struct xdev *xd = kzalloc(sizeof *xd);
    if (!xd) goto fail_slot;
    struct usb_device *d = &xd->dev;
    *d = (struct usb_device){.hc = hc, .parent = parent, .slot = slot, .speed = speed, .port = port,
                             .root_port = root_port, .depth = depth, .rank = rank, .route = route,
                             .tt_slot = tt_slot, .tt_port = tt_port};
    xd->speed_id = (uint8_t)speed_id;
    xd->ep0_state = EP_RUNNING;
    if (!(xd->out_ctx = dma_page(&xd->out_phys)) || !(xd->in_ctx = dma_page(&xd->in_phys)) ||
        !ring_init(&xd->ep0) || !(xd->xfer = dma_page(&xd->xfer_phys)))
        goto fail;
    hc->dcbaa[slot] = xd->out_phys;
    __atomic_store_n(&hc->slots[slot], xd, __ATOMIC_RELEASE);

    unsigned mps = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    uint32_t *icc = in_ctx(xd, 0), *ep0 = in_ctx(xd, 2);
    xd->ep0_mps = (uint16_t)mps;
    cc = address_device(xd);
    if (cc != CC_SUCCESS) {
        kprintf("usb: port %u address assignment failed (%d)\n", port, cc);
        goto fail;
    }
    sleep_until(clock_after(2 * NS_PER_MS)); /* SetAddress recovery (USB 2.0 9.2.6.3) */
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &d->desc, 8) < 8) {
        kprintf("usb: port %u device descriptor unreadable\n", port);
        goto fail;
    }
    if (speed < USB_SPEED_SUPER && d->desc.max_packet0 && d->desc.max_packet0 != mps) {
        mps = d->desc.max_packet0;
        xd->ep0_mps = (uint16_t)mps;
        mutex_lock(&xd->in_mutex);
        icc[0] = 0;
        icc[1] = 2;
        ep0[1] = 3u << 1 | 4u << 3 | mps << 16;
        cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_EVALUATE) | (uint32_t)slot << 24, NULL);
        mutex_unlock(&xd->in_mutex);
        if (cc != CC_SUCCESS) {
            kprintf("usb: port %u control packet size %u refused (%d)\n", port, mps, cc);
            goto fail;
        }
    }
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &d->desc, 18) < 18) goto fail;
    char via[24] = "";
    if (parent) ksnprintf(via, sizeof via, " (hub port %u)", port);
    kprintf("usb: %04x:%04x class %02x on controller %d port %u%s, %s speed, slot %u\n", d->desc.vendor,
            d->desc.product, d->desc.cls, hc->index, root_port, via, speed_name(speed), slot);
    int got = 0;
    uint8_t *cfg = NULL;
    /* devices still busy after reset (or slow to fetch their descriptors) answer a moment later */
    for (int attempt = 0; attempt < 3 && !removed(d) && !xd->ctl_broken; attempt++) {
        if (attempt) sleep_until(clock_after((100 * NS_PER_MS) << attempt));
        if ((cfg = choose_configuration(d, &got)) && configure(xd, cfg, (unsigned)got)) break;
        kfree(cfg);
        cfg = NULL;
    }
    if (!cfg) {
        kprintf("usb: slot %u could not be configured\n", slot);
        return xd; /* stays addressed so a disconnect still frees it */
    }
    /* A hub's descriptor may only be asked of it configured (USB 2.0 11.24.2.5, USB 3.2 10.16.2.3), and
       what it says goes into the Slot Context by the first Configure Endpoint after the slot's Default
       state (xHCI 4.5.2, 6.2.2.2): so a hub is configured as a function, asked, and made anew. */
    if (configuration_has_hub(cfg, (unsigned)got) && usbhub_prepare(d) &&
        (!device_readdress(xd) || !configure(xd, cfg, (unsigned)got))) {
        kprintf("usb: slot %u could not be configured as a hub\n", slot);
        kfree(cfg);
        usbhub_detach(d);
        goto fail;
    }
    bind_classes(xd, cfg, (unsigned)got);
    if (!removed(d)) usbmsc_attach(d, cfg, (unsigned)got);
    if (!removed(d)) usbnet_attach(d, cfg, (unsigned)got);
    kfree(cfg);
    return xd;
fail:
    __atomic_store_n(&xd->dev.removed, true, __ATOMIC_RELEASE); /* late events for the slot are dropped */
fail_slot:
    release_slot(hc, slot, xd);
    return NULL;
}

struct usb_device *usb_attach_child(struct usb_device *hub, unsigned port, unsigned speed, unsigned rank) {
    unsigned hubs = hub->hc->port_hubs[hub->root_port];
    if (hub->depth >= (hubs ? hubs : 5) || removed(hub)) return NULL;
    /* an HS or FS hub of more than 14 ports has 15 as its tier of the route string (6.2.2) */
    unsigned tier = hub->speed < USB_SPEED_SUPER && ((struct xdev *)hub)->hub_ports > 14 ? 15 : port;
    uint32_t route = hub->route | (uint32_t)tier << (4 * hub->depth);
    unsigned tt_slot = hub->tt_slot, tt_port = hub->tt_port;
    if ((hub->speed == USB_SPEED_HIGH && (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW)) ||
        (hub->speed >= USB_SPEED_SUPER && rank < hub->rank)) { /* Table 6-6 */
        tt_slot = hub->slot;
        tt_port = port;
    }
    struct xdev *xd = device_attach(hub->hc, hub, port, hub->root_port, route, speed,
                                    port_speed_id(hub->hc, hub->root_port, speed, rank), rank, hub->depth + 1, tt_slot, tt_port);
    return xd ? &xd->dev : NULL;
}

/* On its controller's worker. Once marked removed, no drain hands it another completion (they are
   synchronized with), and every transfer waited for on it ends: the class drivers then drop it,
   each after its own users are out, and the slot goes. */
void usb_remove(struct usb_device *d) {
    if (removed(d)) return;
    struct xdev *xd = (struct xdev *)d;
    struct usb_hc *hc = d->hc;
    __atomic_store_n(&d->removed, true, __ATOMIC_RELEASE);
    sync_drains(hc);
    uint64_t f = spin_lock(&xd->ctl_lock);
    wq_wake_all(&xd->ctl_wq);
    spin_unlock(&xd->ctl_lock, f);
    for (int i = 2; i < 32; i++) {
        struct endpoint *ep = xd->ep[i];
        if (!ep) continue;
        f = spin_lock(&ep->lock);
        wq_wake_all(&ep->wq);
        spin_unlock(&ep->lock, f);
    }
    kprintf("usb: slot %u (%04x:%04x) removed\n", d->slot, d->desc.vendor, d->desc.product);
    usbhub_detach(d);
    usbhid_detach(d);
    usbmsc_detach(d);
    usbnet_detach(d);
    release_slot(hc, d->slot, xd);
}

/* ---- root ports, on the worker ---- */

/* Reset a root port (warm reset: the USB3 link reset); its end comes as a port status change
   event (PRC, WRC). True once enabled. */
static bool port_reset(struct usb_hc *hc, unsigned p, bool warm) {
    uint32_t sc = rd32(hc->op, PORTSC(p)), done = warm ? PS_WRC | PS_PRC : PS_PRC;
    uint64_t seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (warm ? PS_WPR : PS_PR));
    for (uint64_t deadline = clock_after(500 * NS_PER_MS);
         !(rd32(hc->op, PORTSC(p)) & done) && clock_now_ns() < deadline && hc_running(hc);) {
        wq_wait_unless(&hc->work_wq, &hc->kicks, seen, deadline);
        seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
    }
    sc = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery (USB 2.0 7.1.7.5, TRSTRCY) */
    return rd32(hc->op, PORTSC(p)) & PS_PED;
}

/* PLS means something: not during a reset (4.19.1.2.5), not without power (Table 5-27). */
static bool link_known(uint32_t sc) { return (sc & PS_PP) && !(sc & PS_PR); }

/* A USB3 root port's link trains by itself and is enabled then, which comes as a port status
   change; a stuck link (inactive, compliance) does not, and asks for a warm reset. False at the deadline. */
static bool link_trained(struct usb_hc *hc, unsigned p) {
    uint64_t seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
    for (uint64_t deadline = clock_after(NS_PER_S); hc_running(hc);) {
        uint32_t sc = rd32(hc->op, PORTSC(p));
        if (sc & PS_PED) return true;
        unsigned pls = PS_PLS(sc);
        if ((link_known(sc) && (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE)) || clock_now_ns() >= deadline) return false;
        wq_wait_unless(&hc->work_wq, &hc->kicks, seen, deadline);
        seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
    }
    return false;
}

/* In Error (Inactive) or Compliance a USB3 port reads no connection (Figure 4-27), and a device that
   stays there has only a reset to take it out (4.19.1.2.5). A device that leaves passes through
   Inactive by itself, which alone sets no PLC (Table 5-27, footnote 94); so Inactive is taken from a
   read with PLC, or from a port being tried. */
static bool port_stuck(const struct usb_hc *hc, unsigned p, uint32_t sc) {
    unsigned pls = PS_PLS(sc);
    if (!link_known(sc)) return false;
    return pls == PLS_COMPLIANCE || (pls == PLS_INACTIVE && (sc & PS_PLC || hc->port_tries[p].at));
}

/* Nothing is at the port: a USB3 port that is back in RxDetect, a USB2 port with no connection, a
   port without power. */
static bool port_empty(const struct usb_hc *hc, unsigned p, uint32_t sc) {
    return !(sc & PS_CCS) && !(sc & PS_PR) && (!(sc & PS_PP) || hc->port_major[p] < 3 || PS_PLS(sc) == PLS_RXDETECT);
}

/* The connection stable for TATTDB, which a disconnection begins again (USB 2.0 7.1.7.3). */
static bool connect_debounced(struct usb_hc *hc, unsigned p) {
    for (int i = 0; i < USB_DEBOUNCES; i++) {
        sleep_until(clock_after(100 * NS_PER_MS));
        uint32_t sc = rd32(hc->op, PORTSC(p));
        if (!(sc & PS_CCS)) return false;
        if (!(sc & PS_CSC)) return true;
        wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_CSC);
    }
    return false;
}

/* One try at a port with something at it and no device yet. */
static void root_port_attach(struct usb_hc *hc, unsigned p) {
    bool usb3 = hc->port_major[p] >= 3, retry = hc->port_tries[p].count > 1;
    if (usb3) {
        if (!link_trained(hc, p) || retry) port_reset(hc, p, true);
    } else {
        if (!retry && !connect_debounced(hc, p)) return;
        port_reset(hc, p, false);
    }
    uint32_t sc = rd32(hc->op, PORTSC(p));
    if (!(sc & PS_CCS)) return;
    if (!(sc & PS_PED)) {
        kprintf("usb: controller %d port %u connected but not enabled (%08x)\n", hc->index, p, sc);
        return;
    }
    unsigned id = (sc >> 10) & 15;
    hc->root[p] = device_attach(hc, NULL, p, p, 0, port_speed_kind(hc, p, id), id, port_speed_rank(hc, p, id), 0, 0, 0);
    /* not left in its Default state (4.19.5's note), where it answers address 0 with the next device on
       that bus. A USB3 link has no such bus, and a USB3 port is not disabled: its way back is a write
       to PLS (Table 5-27), where a warm reset is to come first. */
    if (!hc->root[p] && !usb3) {
        sc = rd32(hc->op, PORTSC(p));
        wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PED);
    }
}

static void root_port(struct usb_hc *hc, unsigned p) {
    uint32_t sc = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    if (hc->root[p] && (!(sc & PS_CCS) || (sc & PS_CSC) || !(sc & PS_PED))) {
        /* unplugged, replugged between two looks, or disabled by an error: start over */
        struct xdev *gone = hc->root[p];
        hc->root[p] = NULL;
        usb_remove(&gone->dev);
    }
    struct usb_port_tries *t = &hc->port_tries[p];
    if (sc & PS_CSC) usb_port_free(t); /* what was being tried is not what comes now */
    if (!(sc & PS_PP) && !(sc & PS_OCA)) {
        /* an over-current took its power and is over: on again, its 20 ms (Table 5-27, 5.4.8; USB 2.0 11.12.5).
           A short takes it again at once, with its load gone: less and less often. */
        struct usb_port_tries *power = &hc->port_power[p];
        if (!usb_port_try(power)) return;
        usb_port_failed(power, true);
        if (power->count == 1) kprintf("usb: controller %d port %u over-current ended, powering it again\n", hc->index, p);
        wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PP);
        sleep_until(clock_after(20 * NS_PER_MS));
        sc = rd32(hc->op, PORTSC(p));
    }
    bool usb3 = hc->port_major[p] >= 3, stuck = usb3 && port_stuck(hc, p, sc);
    if ((!(sc & PS_CCS) && !stuck) || hc->root[p] || !usb_port_try(t)) return;
    root_port_attach(hc, p);
    sc = rd32(hc->op, PORTSC(p));
    if (hc->root[p] || port_empty(hc, p, sc)) usb_port_free(t);
    else usb_port_failed(t, usb3 && !(sc & PS_CCS));
}

/* What no event tells, each round of the worker, at the latest a second apart. A change flag that came between a read and its write
   back stays set, and no event comes while one does (4.19.2). A USB3 port entering Compliance sets no
   flag: neither the PLC table of Table 5-27 nor 4.19.2 has it, and a controller without CTC goes in by
   itself (5.3.9, 4.19.1.2.4.1). Leaving Inactive for RxDetect sets none either (4.19.1.2.6), nor need
   the end of an over-current (4.19.2 has OCC rise with it alone). And a port being tried has its time
   come. A port no Supported Protocol capability defines is not touched
   (7.2.2). All ones: the controller left the bus. */
static void hc_ports_again(struct usb_hc *hc) {
    uint64_t now = clock_now_ns();
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        if (!hc->port_major[p]) continue;
        uint32_t sc = rd32(hc->op, PORTSC(p));
        if (sc == 0xFFFFFFFFu) continue;
        bool stuck = hc->port_major[p] >= 3 && port_stuck(hc, p, sc);
        struct usb_port_tries *power = &hc->port_power[p];
        if (port_empty(hc, p, sc)) usb_port_free(&hc->port_tries[p]);
        if ((sc & PS_PP) && power->at && now >= power->at) usb_port_free(power); /* it has stayed on */
        if ((sc & PS_CHANGES) || (!hc->root[p] && now >= hc->port_tries[p].at && (stuck || sc & PS_CCS)) ||
            (!(sc & PS_PP) && !(sc & PS_OCA) && now >= power->at))
            __atomic_fetch_or(&hc->port_pending[p >> 5], 1u << (p & 31), __ATOMIC_RELEASE);
    }
}

/* Each port once: one that changes again, as its own resets make it, waits for the worker's next
   round, and the worker's other work comes in between. */
static void hc_ports(struct usb_hc *hc) {
    for (unsigned w = 0; w < 8 && hc_running(hc); w++) {
        uint32_t bits = __atomic_exchange_n(&hc->port_pending[w], 0, __ATOMIC_ACQ_REL);
        while (bits) {
            unsigned b = __builtin_ctz(bits);
            bits &= bits - 1;
            unsigned p = w * 32 + b;
            if (p >= 1 && p <= hc->max_ports && hc->port_major[p]) root_port(hc, p);
        }
    }
}

/* Halted streams and sends brought back, each failure waiting twice as long as the last. */
static void hc_recover(struct usb_hc *hc) {
    hc->retry_at = CLOCK_NEVER;
    if (!__atomic_exchange_n(&hc->recover, false, __ATOMIC_ACQ_REL)) return;
    uint64_t now = clock_now_ns();
    for (unsigned s = 1; s <= hc->max_slots; s++) {
        struct xdev *xd = hc->slots[s];
        for (int i = 2; xd && !removed(&xd->dev) && i < 32; i++) {
            struct endpoint *ep = xd->ep[i];
            if (!ep || !ep->halted || ep->dead || ep->mode == EP_SYNC) continue; /* usb_bulk's caller recovers */
            if (now < ep->retry_at) { /* backing off: come back later */
                __atomic_store_n(&hc->recover, true, __ATOMIC_RELEASE);
                hc->retry_at = MIN(hc->retry_at, ep->retry_at);
                continue;
            }
            /* the device keeps its own halt and data toggle until told; if that does not get through,
             * the pipe is still halted and is retried later */
            bool ok = pipe_reset(xd, ep);
            if (removed(&xd->dev)) break;
            if (++ep->fails >= 8) {
                kprintf("usb: slot %u endpoint %u keeps failing, given up until it is replugged\n", s, i);
                uint64_t f = spin_lock(&ep->lock);
                ep->dead = true;
                spin_unlock(&ep->lock, f);
                usbhub_reports_lost(&xd->dev);
                continue;
            }
            ep->retry_at = now + ((10 * NS_PER_MS) << ep->fails);
            if (!ok) {
                __atomic_store_n(&hc->recover, true, __ATOMIC_RELEASE);
                hc->retry_at = MIN(hc->retry_at, ep->retry_at);
                continue;
            }
            /* the ring restarts past everything that was queued, which a stream's driver hears first */
            if (ep->mode == EP_STREAM && ep->fn) ep->fn(&xd->dev, ep->ctx, NULL, 0, now);
            uint64_t f = spin_lock(&ep->lock);
            endpoint_forget(ep);
            ep->halted = false;
            if (ep->mode == EP_STREAM) endpoint_requeue_all(hc, xd, ep);
            spin_unlock(&ep->lock, f);
            if (ep->mode == EP_SEND && ep->fn) ep->fn(&xd->dev, ep->ctx, NULL, 0, now); /* sends were dropped: slots are free */
        }
    }
}

/* The class drivers' work for each device that asked for it (usb_note_work), or a hub whose time
   has come to look at a port. A hub's work may remove devices behind it: each slot is read afresh. */
static void hc_devices(struct usb_hc *hc) {
    for (unsigned s = 1; s <= hc->max_slots; s++) {
        struct xdev *xd = hc->slots[s];
        if (!xd || removed(&xd->dev)) continue;
        bool asked = __atomic_exchange_n(&xd->work, false, __ATOMIC_ACQ_REL);
        if (!asked && !usbhub_pending(&xd->dev)) continue;
        usbhub_work(&xd->dev);
        if (hc->slots[s] == xd && !removed(&xd->dev)) usbhid_work(&xd->dev);
    }
}

/* Its devices may yet appear. A controller that does not run has none coming. */
static bool hc_busy(struct usb_hc *hc) {
    if (!hc_running(hc)) return false;
    for (unsigned w = 0; w < 8; w++)
        if (__atomic_load_n(&hc->port_pending[w], __ATOMIC_ACQUIRE)) return true;
    for (unsigned p = 1; p <= hc->max_ports; p++)
        if (usb_port_trying(&hc->port_tries[p])) return true;
    for (unsigned s = 1; s <= hc->max_slots; s++) {
        struct xdev *xd = hc->slots[s];
        if (xd && !removed(&xd->dev) && (__atomic_load_n(&xd->work, __ATOMIC_ACQUIRE) || usbhub_pending(&xd->dev) ||
                                         usbhub_trying(&xd->dev)))
            return true;
    }
    return false;
}

/* ---- controller bring-up ---- */

static bool wait_reg(volatile uint8_t *b, unsigned off, uint32_t mask, uint32_t want, uint64_t ns) {
    uint64_t deadline = clock_after(ns);
    for (;;) {
        if ((rd32(b, off) & mask) == want) return true;
        if (clock_now_ns() >= deadline) return false;
        sleep_until(clock_after(NS_PER_MS));
    }
}

static void interrupter_free(struct interrupter *ir) {
    if (ir->ev) dma_free(ir->ev_phys, EV_PAGES);
    if (ir->erst) pmm_free(ir->erst_phys);
}

/* The interrupters, once: their memory, then as many as the kinds of events, the controller's MaxIntrs
   and the messages the PCI layer gives allow (MSI-X: interrupter n raises vector n; MSI: n modulo
   the messages enabled, 4.17), each with its event ring. None given: one, polled. */
static bool interrupters_init(struct usb_hc *hc) {
    unsigned want = MIN((unsigned)RING_KINDS, hc->max_intrs ? hc->max_intrs : 1);
    struct interrupter *intr = kzalloc(want * sizeof *intr);
    if (!intr) return false;
    struct irq_spec specs[RING_KINDS];
    struct irq *irqs[RING_KINDS] = {0};
    for (unsigned i = 0; i < want; i++) {
        struct interrupter *ir = &intr[i];
        ir->hc = hc;
        ir->index = i;
        ir->regs = hc->rt + IR_SET(i);
        specs[i] = (struct irq_spec){.name = "xHCI", .handler = xhci_isr, .context = ir, .node = -1};
        /* the event ring segment must not cross a 64 KiB boundary (6.0, Table 6-1) */
        if ((ir->ev = dma_block(EV_PAGES, &ir->ev_phys)) && (ir->erst = dma_page(&ir->erst_phys))) continue;
        for (unsigned j = 0; j <= i; j++) interrupter_free(&intr[j]);
        kfree(intr);
        return false;
    }
    hc->intr = intr;
    unsigned got = pci_irq_enable(hc->pci, specs, want, irqs);
    hc->nintr = got ? got : 1;
    __atomic_store_n(&hc->polled, !got, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < got; i++) intr[i].irq = irqs[i];
    for (unsigned i = hc->nintr; i < want; i++) interrupter_free(&intr[i]);
    hc->target[RING_CONTROL] = 0;
    hc->target[RING_PERIODIC] = hc->nintr > 1 ? 1 : 0;
    hc->target[RING_BULK] = hc->nintr > 2 ? 2 : 0;
    return true;
}

/* An interrupter's ring empty and in place (4.9.4: ERSTBA last). */
static void interrupter_start(struct usb_hc *hc, struct interrupter *ir) {
    memset(ir->ev, 0, EV_PAGES * 4096);
    ir->erst[0] = ir->ev_phys;
    ir->erst[1] = EV_TRBS;
    ir->ev_idx = 0;
    ir->ev_cycle = 1;
    ir->interrupts = 0;
    bool periodic = hc->target[RING_PERIODIC] == ir->index;
    wr32(ir->regs, ERSTSZ, 1);
    wr64(ir->regs, ERDP, ir->ev_phys);
    wr64(ir->regs, ERSTBA, ir->erst_phys);
    wr32(ir->regs, IMOD, periodic ? 0 : IMODI_GATHER);
    wr32(ir->regs, IMAN, __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE) ? IMAN_IP : IMAN_IP | IMAN_IE);
}

static bool hc_start(struct usb_hc *hc) {
    struct pci_device *pci = hc->pci;
    uint64_t base = pci_bar_address(pci, 0);
    if (!base) {
        kprintf("usb: controller %d has no register window\n", hc->index);
        return false;
    }
    /* the capability, operational, runtime, doorbell and extended registers, all in its BAR; once,
       a restart finds them there */
    hc->bar_size = pci_bar_size(pci, 0);
    if (hc->bar_size < 0x1000) {
        kprintf("usb: controller %d register window unusable\n", hc->index);
        return false;
    }
    if (!hc->cap && !(hc->cap = vmm_map_mmio(base, hc->bar_size))) {
        kprintf("usb: controller %d: no memory to map its registers\n", hc->index);
        return false;
    }
    pci_enable_busmaster(pci);
    hc->op = hc->cap + (rd32(hc->cap, 0) & 0xFF);
    uint32_t hcs1 = rd32(hc->cap, 4), hcs2 = rd32(hc->cap, 8), hcc1 = rd32(hc->cap, 0x10);
    hc->max_slots = hcs1 & 0xFF;
    hc->max_intrs = (hcs1 >> 8) & 0x7FF;
    hc->max_ports = hcs1 >> 24;
    hc->ctx_size = (hcc1 & 4) ? 64 : 32;
    hc->lec = rd32(hc->cap, 0x1C) & HCC2_LEC;
    hc->db = (volatile uint32_t *)(hc->cap + (rd32(hc->cap, 0x14) & ~3u));
    hc->rt = hc->cap + (rd32(hc->cap, 0x18) & ~0x1Fu);
    if (!(hcc1 & 1)) {
        kprintf("usb: controller %d lacks 64-bit addressing, skipped\n", hc->index);
        return false;
    }
    /* extended capabilities: take the controller from the firmware, learn each port's USB revision */
    for (unsigned x = (hcc1 >> 16) << 2; x && x + 16 <= hc->bar_size;) {
        uint32_t v = rd32(hc->cap, x);
        if ((v & 0xFF) == 1) {
            /* Ownership: the OS semaphore set and the BIOS's clear (Table 7-4), whoever had it, or the
             * firmware may take it later (4.22.1). Only the OS semaphore's byte: a whole-register write
             * could put back a BIOS bit the firmware cleared in between. */
            mmio_write8(hc->cap + x + 3, (uint8_t)(v >> 24) | 1);
            if (!wait_reg(hc->cap, x, 1u << 16, 0, NS_PER_S))
                kprintf("usb: controller %d firmware kept ownership, taking it anyway\n", hc->index);
            uint32_t legacy = rd32(hc->cap, x + 4);
            wr32(hc->cap, x + 4, (legacy & ~0xE011u) | 0xE0000000u); /* SMIs off, stale events acked */
        } else if ((v & 0xFF) == 2) {
            uint32_t ports = rd32(hc->cap, x + 8), slot_type = rd32(hc->cap, x + 12) & 0x1F;
            unsigned first = ports & 0xFF, count = (ports >> 8) & 0xFF, psic = ports >> 28;
            uint32_t *psi = NULL;
            if (psic && first < 256 && !hc->port_psi[first] && x + 0x10 + psic * 4 <= hc->bar_size &&
                (psi = kmalloc(psic * sizeof *psi)))
                for (unsigned i = 0; i < psic; i++) psi[i] = rd32(hc->cap, x + 0x10 + i * 4);
            for (unsigned p = first; p < first + count && p < 256; p++) {
                hc->port_major[p] = v >> 24;
                hc->port_slot_type[p] = (uint8_t)slot_type;
                hc->port_hubs[p] = (ports >> 25) & 7;
                if (psi) { /* the same each time the controller starts */
                    hc->port_psi[p] = psi;
                    hc->port_psic[p] = (uint8_t)psic;
                }
            }
        }
        unsigned next = (v >> 8) & 0xFF;
        if (!next) break;
        x += next << 2;
    }
    /* no operational register may be written before Controller Not Ready clears (4.2), and a
     * reset is only allowed once the controller has halted (5.4.1) */
    if (!wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
        kprintf("usb: controller %d never became ready\n", hc->index);
        return false;
    }
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~1u);
    if (!wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, HALT_NS)) {
        kprintf("usb: controller %d did not halt\n", hc->index);
        return false;
    }
    wr32(hc->op, USBCMD, 2);
    sleep_until(clock_after(2 * NS_PER_MS));
    if (!wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S) || !wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
        kprintf("usb: controller %d did not come out of reset\n", hc->index);
        return false;
    }
    /* a restart after a fatal error reuses the memory: the reset took it back from the controller */
    if (hc->dcbaa) memset(hc->dcbaa, 0, 4096);
    else if (!(hc->dcbaa = dma_page(&hc->dcbaa_phys))) return false;
    if (hc->cmd.trb) ring_reset(&hc->cmd);
    else if (!ring_init(&hc->cmd)) return false;
    if (!hc->intr && !interrupters_init(hc)) return false;
    unsigned scratch = ((hcs2 >> 21) & 0x1F) << 5 | hcs2 >> 27;
    if (scratch && !hc->scratch_phys) {
        /* buffers of its PAGESIZE, 2^(n+12) bytes on that boundary, zeroed (5.4.3, 4.20). The array has as
           many entries as Max Scratchpad Buffers says, Hi and Lo (5.3.4, 4.20, 6.6), up to 1023: two pages
           aligned as a pair. Table 6-1 gives it at most 248 bytes, and within a PAGESIZE; the two do not agree
           for the largest, and this holds them all, within a PAGESIZE up to 512. */
        uint32_t pagesize = rd32(hc->op, PAGESIZE) & 0xFFFF;
        unsigned pages = pagesize ? 1u << __builtin_ctz(pagesize) : 1;
        uint64_t array_phys;
        uint64_t *array = dma_block(2, &array_phys);
        if (!array) return false;
        for (unsigned i = 0; i < scratch; i++) {
            uint64_t page = pmm_alloc_contig(pages, 64);
            if (!page) {
                while (i) dma_free(array[--i], pages);
                dma_free(array_phys, 2);
                return false;
            }
            memset(phys_to_virt(page), 0, pages * 4096ull);
            array[i] = page;
        }
        hc->scratch_phys = array_phys;
    }
    hc->dcbaa[0] = hc->scratch_phys;
    wr32(hc->op, CONFIG, (rd32(hc->op, CONFIG) & ~0xFFu) | hc->max_slots);
    wr64(hc->op, DCBAAP, hc->dcbaa_phys);
    wr64(hc->op, CRCR, hc->cmd.phys | 1);
    for (unsigned i = 0; i < hc->nintr; i++) interrupter_start(hc, &hc->intr[i]);
    hc_set_running(hc, true);
    bool polled = __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE);
    wr32(hc->op, USBCMD, 1 | (polled ? 0 : CMD_INTE));
    if (!wait_reg(hc->op, USBSTS, STS_HCH, 0, 100 * NS_PER_MS)) {
        kprintf("usb: controller %d did not start\n", hc->index);
        hc_set_running(hc, false);
        return false;
    }
    if (polled) alarm_set(&hc->poll, clock_now_ns());
    /* a No Op command shows that interrupts arrive (or that they do not: polled from then on) */
    if (hc_command(hc, 0, TRB_TYPE(TRB_NOOP_CMD), NULL) != CC_SUCCESS) {
        kprintf("usb: controller %d does not answer commands\n", hc->index);
        hc_set_running(hc, false);
        return false;
    }
    memset(hc->port_tries, 0, sizeof hc->port_tries); /* the reset took every port back (4.19.1.2.3) */
    memset(hc->port_power, 0, sizeof hc->port_power);
    if (hcc1 & 8) /* port power control */
        for (unsigned p = 1; p <= hc->max_ports; p++) {
            if (!hc->port_major[p]) continue; /* no protocol defines it: not to be touched (7.2.2) */
            uint32_t sc = rd32(hc->op, PORTSC(p));
            if (!(sc & PS_PP)) wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PP);
        }
    sleep_until(clock_after(20 * NS_PER_MS));
    for (unsigned p = 1; p <= hc->max_ports; p++)
        if (hc->port_major[p]) __atomic_fetch_or(&hc->port_pending[p >> 5], 1u << (p & 31), __ATOMIC_RELEASE);
    polled = __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE);
    kprintf("usb: xHCI %d (%04x:%04x) version %x, %u ports, %u slots, %u interrupter%s of %u, %s\n", hc->index,
            pci->vendor, pci->device, rd32(hc->cap, 0) >> 16, hc->max_ports, hc->max_slots, hc->nintr,
            hc->nintr > 1 ? "s" : "", hc->max_intrs, polled ? "polled" : pci->msix_entries ? "MSI-X" : "MSI");
    for (unsigned i = 0; i < hc->nintr && !polled; i++) {
        struct irq_target to = irq_target(hc->intr[i].irq);
        kprintf("usb: controller %d interrupter %u (%s) on cpu %u vector 0x%x\n", hc->index, i,
                i == hc->target[RING_PERIODIC] && i ? "interrupt endpoints"
                : i == hc->target[RING_BULK] && i ? "bulk endpoints"
                : hc->nintr == 1 ? "all events" : "commands, ports, control",
                to.cpu, to.vector);
    }
    return true;
}

/* A Host System Error or Host Controller Error stops the controller for good (4.24.1, 5.4.2) and
 * every device with it: reset it so it lets go of memory, have the drivers drop their devices,
 * then bring it up again and enumerate from scratch. A controller that keeps failing is left off.
 * After a sleep the same happens, without counting as a failure: the devices lost their addresses. */
static void hc_restart(struct usb_hc *hc, bool after_sleep) {
    hc_set_running(hc, false);
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~(1u | CMD_INTE));
    /* a reset is only for a halted controller (5.4.1). One that does not halt keeps its DMA until bus
       mastering goes off; the drivers then drop their devices, and it stays off. */
    bool halted = wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, HALT_NS);
    if (halted) {
        wr32(hc->op, USBCMD, 2);
        sleep_until(clock_after(2 * NS_PER_MS));
        wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S);
    } else {
        kprintf("usb: controller %d does not halt, turned off\n", hc->index);
        pci_disable_busmaster(hc->pci, NULL);
    }
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        struct xdev *gone = hc->root[p];
        hc->root[p] = NULL;
        if (gone) usb_remove(&gone->dev);
    }
    for (unsigned s = 1; s <= hc->max_slots; s++)
        if (hc->slots[s]) usb_remove(&hc->slots[s]->dev);
    __atomic_store_n(&hc->recover, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hc->wedged, false, __ATOMIC_RELEASE);
    if (!halted) return;
    if (after_sleep) {
        kprintf("usb: controller %d starting again after sleep\n", hc->index);
        if (hc_start(hc)) hc->failed = false;
        else kprintf("usb: controller %d did not come back after sleep\n", hc->index);
        return;
    }
    if (++hc->restarts > 3) {
        kprintf("usb: controller %d keeps failing, left off\n", hc->index);
        return;
    }
    kprintf("usb: controller %d restarting (attempt %u)\n", hc->index, hc->restarts);
    if (hc_start(hc)) hc->failed = false;
}

bool usb_enumerating(void) { return __atomic_load_n(&unsettled, __ATOMIC_ACQUIRE) > 0; }

static void hc_settled(struct usb_hc *hc) {
    if (hc->settled) return;
    hc->settled = true;
    __atomic_fetch_sub(&unsettled, 1, __ATOMIC_RELEASE);
}

/* A controller's worker: its bring-up, ports, hubs, enumeration, recoveries and restarts. A fair
   thread: none of this is periodic, and none of it holds a completion up. */
static void hc_worker(void *arg) {
    struct usb_hc *hc = arg;
    if (!hc_start(hc)) {
        hc->failed = true;
        hc_settled(hc);
    }
    uint64_t settle_at = clock_after(500 * NS_PER_MS);
    for (;;) {
        uint64_t seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
        if (__atomic_exchange_n(&hc->woke, false, __ATOMIC_ACQ_REL)) hc_restart(hc, true);
        uint32_t sts = hc_running(hc) ? rd32(hc->op, USBSTS) : 0;
        if ((sts & (STS_HSE | STS_HCE)) && sts != 0xFFFFFFFFu && !hc->failed) { /* all ones: gone from the bus */
            hc->failed = true;
            kprintf("usb: controller %d stopped on a %s error (status %08x)\n", hc->index,
                    sts & STS_HCE ? "controller" : "host system", sts);
            hc_restart(hc, false);
        } else if (__atomic_load_n(&hc->wedged, __ATOMIC_ACQUIRE) && !hc->failed) {
            hc->failed = true;
            hc_restart(hc, false);
        }
        if (hc_running(hc)) hc_ports_again(hc);
        hc_ports(hc);
        hc_recover(hc);
        hc_devices(hc);
        /* boot waits for this: every port and hub looked at once, and USB 3 links (which train
         * by themselves, then report a change) given a moment to show up */
        uint64_t now = clock_now_ns();
        if (!hc->settled && now >= settle_at && !hc_busy(hc)) hc_settled(hc);
        /* the next look: a recovery retry, settling, else the second after which the controller's
           status is looked at again (a fatal error raises no event) */
        uint64_t next = MIN(hc->retry_at, now + NS_PER_S);
        if (!hc->settled && now < settle_at) next = MIN(next, settle_at);
        if (next > now) wq_wait_unless(&hc->work_wq, &hc->kicks, seen, next);
    }
}

/* Power change (dev/pm.h): halt every controller, which ends its DMA (5.4.2: within 16 ms).
   The disks behind it were flushed before. Interrupts are off: polled. */
static bool xhci_quiesce(void *arg) {
    (void)arg;
    bool all = true;
    for (size_t i = 0; i < ncontrollers; i++) {
        struct usb_hc *hc = controllers[i];
        if (!hc->op) continue;
        wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~(1u | CMD_INTE));
        for (uint64_t deadline = clock_after(HALT_NS);
             !(rd32(hc->op, USBSTS) & STS_HCH) && clock_now_ns() < deadline;)
            pause();
        if (!(rd32(hc->op, USBSTS) & STS_HCH)) all = false;
    }
    return all;
}

/* After a sleep: every controller is reset and enumerated again by its worker (resume must not
   wait, and the class drivers run there). Keyboards and mice come back; a USB disk would come back
   as a new device under a mounted filesystem, so there is no sleep while one is plugged in. */
static bool xhci_resume(void *arg) {
    (void)arg;
    for (size_t i = 0; i < ncontrollers; i++) {
        struct usb_hc *hc = controllers[i];
        if (!hc->op) continue;
        __atomic_store_n(&hc->running, false, __ATOMIC_RELEASE);
        __atomic_store_n(&hc->woke, true, __ATOMIC_RELEASE);
        hc_kick(hc);
    }
    return true; /* the worker logs a controller that does not come back (hc_restart) */
}

static const char *xhci_refuse(void *arg) {
    (void)arg;
    return usbmsc_attached() ? "A USB disk is plugged in" : NULL;
}

static struct pm_device xhci_pm = {.name = "xhci", .stage = PM_STAGE_BUS, .quiesce = xhci_quiesce,
                                   .resume = xhci_resume, .refuse = xhci_refuse};

void usb_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d->cls != 0x0C || d->subcls != 0x03 || d->progif != 0x30) continue;
        struct usb_hc *hc = kzalloc(sizeof *hc), **at;
        if (!hc || !(at = kappend(&controllers, &ncontrollers, &controllers_room, sizeof *controllers))) {
            kfree(hc);
            kprintf("usb: no memory for the controller at %02x:%02x.%u\n", d->bus, d->dev, d->func);
            continue;
        }
        hc->index = (int)(ncontrollers - 1);
        hc->pci = d;
        alarm_init(&hc->poll, poll_fire);
        *at = hc;
    }
    if (!ncontrollers) return;
    __atomic_store_n(&unsettled, (unsigned)ncontrollers, __ATOMIC_RELEASE);
    for (size_t i = 0; i < ncontrollers; i++) {
        char name[16];
        ksnprintf(name, sizeof name, "usb%d", controllers[i]->index);
        if (!(controllers[i]->worker = kthread_create(name, hc_worker, controllers[i]))) {
            kprintf("usb: no memory for controller %d's worker\n", controllers[i]->index);
            hc_settled(controllers[i]);
        }
    }
    pm_register(&xhci_pm);
}
