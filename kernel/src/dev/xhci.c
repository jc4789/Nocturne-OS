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
 * vector, where the controller and the PCI layer give that many; with fewer they share.
 *
 * 電源（hal/hal.h の契約、下の「眠り」の節）。眠りと休止の写しでは装置を残す形で止め（4.15、4.23.2）、状態を保った装置は
 * 戻りの後も同じ装置として残る。状態を失った装置は去り、つながっていれば数え直される。 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/pci.h"
#include "dev/usb.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
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
#define TRB_SP (1u << 23) /* Stop Endpoint: 中断する装置の端点（6.4.3.8） */
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
#define CMD_HCRST (1u << 1)
#define CMD_INTE (1u << 2)
#define CMD_CSS (1u << 8) /* Controller Save State (5.4.1) */
#define CMD_CRS (1u << 9) /* Controller Restore State */
#define STS_HCH 1u
#define STS_HSE (1u << 2)
#define STS_EINT (1u << 3)
#define STS_SSS (1u << 8)  /* Save State Status (5.4.2) */
#define STS_RSS (1u << 9)  /* Restore State Status */
#define STS_SRE (1u << 10) /* Save/Restore Error, RW1C */
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
#define PS_LWS (1u << 16) /* PLS を書くときに立てる（Table 5-27） */
#define PS_CSC (1u << 17)
#define PS_WRC (1u << 19)
#define PS_PRC (1u << 21)
#define PS_PLC (1u << 22)
#define PS_CAS (1u << 24) /* Cold Attach Status (4.19.8) */
#define PS_WPR (1u << 31)
#define PLS_U0 0
#define PLS_U3 3
#define PLS_RXDETECT 5
#define PLS_INACTIVE 6
#define PLS_COMPLIANCE 10
#define PLS_RESUME 15
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
#define SAVE_NS NS_PER_S /* Save State と Restore State の終わり（SSS、RSS）：文書は時間を決めない */

/* The kinds of transfer events, each on an interrupter of its own when there are enough. */
enum ring_kind { RING_CONTROL, RING_PERIODIC, RING_BULK, RING_KINDS };

struct ring {
    struct trb *trb;
    uint64_t phys, bus; /* where the CPU reaches it, and what the controller is told (bus_of) */
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

/* An endpoint. lock guards its ring, meta, busy, gen, trbs, inflight, next, halted, dead, asleep and the
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
    uint64_t buf_phys, buf_bus;
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
    /* 眠りの間（device_sleep から device_wake まで）：輪に何も積まず doorbell も鳴らさない（4.23.2：止めた制御器が指す
       構造は変えない）。起きなかった装置の端点は、装置が外されるまでこのまま */
    bool asleep;
    unsigned fails;    /* recoveries since the last good transfer, the end of it at 8 (the worker's and a drain's) */
    uint64_t retry_at; /* when the next recovery may run (clock_now_ns; the worker's) */
};

struct xdev {
    struct usb_device dev; /* first: class drivers only see this part */
    struct usb_interface *interfaces; /* its nodes, from when it is configured to usb_remove (the worker's) */
    unsigned ninterfaces;
    bool published; /* dev.hal is in the tree: release_slot removes it */
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
    bool asleep;     /* 端点は止まり、link は中断している：device_sleep から device_wake まで。ctl_mutex の下で書き、
                        control と電源の walk が読む */
};

struct interrupter {
    struct usb_hc *hc;
    unsigned index;
    volatile uint8_t *regs;
    struct spinlock lock; /* the ring's dequeue side: one drain at a time */
    struct trb *ev;
    uint64_t ev_phys, ev_bus;
    uint64_t *erst;
    uint64_t erst_phys, erst_bus;
    unsigned ev_idx;
    uint32_t ev_cycle;
    struct irq *irq;     /* its MSI or MSI-X message, or the INTx line; NULL when polled */
    uint32_t interrupts; /* under lock */
};

struct usb_hc {
    int index;
    struct hal_device *hal; /* its PCI function */
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
    bool running; /* atomic; written under every interrupter's lock (hc_set_running) */
    bool polled;  /* atomic: no interrupt comes, the poll alarm drains the rings */
    bool messages; /* the interrupters raise MSI or MSI-X messages, not the INTx pin; set before running */
    struct alarm poll;
    uint32_t periodic[16]; /* open interrupt endpoints by service interval exponent (atomic) */
    /* the worker: kicks counts the notes of work (wq_wait_unless), the rest is the worker's but
       port_pending, port_lost, recover and lost, which drains and resume set */
    struct thread *worker;
    struct wait_queue work_wq;
    uint64_t kicks;
    uint32_t port_pending[8];
    uint32_t port_lost[8]; /* atomic：眠りから戻らなかった装置の root port。root_port が装置を外し、reset から数え直す */
    bool recover;
    bool lost; /* atomic：xhci_resume が制御器の状態を戻せなかった。worker が立て直し、全部を数え直す */
    /* 眠り：xhci_suspend と xhci_resume だけが書く（worker は電源の門で待っている） */
    bool slept;        /* suspend が制御器を止めた：resume が戻す */
    bool saved;        /* Save State が済んだ（4.23.2） */
    uint64_t quiet_at; /* 端点と root port が止まった時：10 ms は戻さない（6.4.3.8） */
    bool wedged; /* atomic: an aborted command's ring did not stop (4.6.1.2) */
    bool failed, settled;
    bool stopping; /* atomic: detach asks the worker to remove the devices, halt the controller and end */
    bool ended;    /* atomic: the worker has ended */
    bool gone; /* its registers read all ones: not on the bus */
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

static unsigned controllers; /* atomic: the indexes given */

static inline uint32_t rd32(volatile uint8_t *b, unsigned off) { return mmio_read32(b + off); }
static inline void wr32(volatile uint8_t *b, unsigned off, uint32_t v) { mmio_write32(b + off, v); }
static inline void wr64(volatile uint8_t *b, unsigned off, uint64_t v) { mmio_write64(b + off, v); } /* 5.1.1 */
#define barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* The address the controller is told for memory at a physical address, and back (hal_dma_address). */
static uint64_t bus_of(const struct usb_hc *hc, uint64_t phys) { return hal_dma_address(hc->hal, phys); }
static uint64_t phys_of(const struct usb_hc *hc, uint64_t bus) { return bus - hc->hal->dma_offset; }

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
    r->trb[RING_TRBS - 1].param = r->bus;
    r->trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

static bool ring_init(const struct usb_hc *hc, struct ring *r) {
    if (!(r->trb = dma_page(&r->phys))) return false;
    r->bus = bus_of(hc, r->phys);
    ring_reset(r);
    return true;
}

/* One TRB at the enqueue point. held: its cycle bit is left wrong until ring_release, so the
   controller does not start on a TD whose other TRBs are not written yet (4.11.7.1 keeps a TD Fragment
   from being seen in part). */
static uint64_t ring_write(struct ring *r, uint64_t param, uint32_t status, uint32_t control, bool held) {
    struct trb *t = &r->trb[r->enq];
    uint64_t at = r->bus + r->enq * sizeof(struct trb);
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
    uint64_t at = ep->buf_bus + (uint64_t)b * ep->stride;
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
    if (!ep->mode || ep->dead || param < ep->ring.bus || param >= ep->ring.bus + 4096) {
        spin_give(&ep->lock);
        return;
    }
    unsigned idx = (unsigned)((param - ep->ring.bus) / sizeof(struct trb));
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
    /* a recovery on the worker may have queued the whole stream again meanwhile. 眠りの間は device_wake が積む */
    if (!removed(&xd->dev) && !ep->halted && !ep->dead && !ep->asleep && !(ep->busy & (1u << b))) {
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
    if (any || in_interrupt) wr64(ir->regs, ERDP, (ir->ev_bus + ir->ev_idx * sizeof(struct trb)) | ERDP_EHB);
}

/* The INTx pin's assertion is the interrupter's when it may interrupt and has one pending (IMAN,
   5.5.2.1); the line may be another function's too. Then EINT is cleared, before IP (5.4.2), and
   clearing IP deasserts the pin (4.17.3). Otherwise nothing is written. ir->lock held. */
static bool pin_take(struct interrupter *ir) {
    uint32_t iman = rd32(ir->regs, IMAN);
    if (iman == 0xFFFFFFFFu || (iman & (IMAN_IP | IMAN_IE)) != (IMAN_IP | IMAN_IE)) return false;
    wr32(ir->hc->op, USBSTS, STS_EINT);
    wr32(ir->regs, IMAN, iman); /* IP written 1 clears it; IE and RsvdP kept */
    return true;
}

/* An interrupter's interrupt, on the CPU its irq is delivered to. A message is always its own, and
   its IP cleared itself as it was sent (4.17.5): nothing to acknowledge but the ring (drain). */
static enum irq_result xhci_isr(void *context) {
    struct interrupter *ir = context;
    uint64_t at = clock_now_ns();
    spin_take(&ir->lock);
    bool ours = hc_running(ir->hc) && (ir->hc->messages || pin_take(ir));
    if (ours) {
        ir->interrupts++;
        drain(ir, at, true);
    }
    spin_give(&ir->lock);
    return ours ? IRQ_HANDLED : IRQ_NONE;
}

/* 全部の interrupter の輪をいま排水する（割り込みの外から）。 */
static void hc_drain(struct usb_hc *hc) {
    uint64_t now = clock_now_ns();
    for (unsigned i = 0; i < hc->nintr; i++) {
        uint64_t f = spin_lock(&hc->intr[i].lock);
        drain(&hc->intr[i], now, false);
        spin_unlock(&hc->intr[i].lock, f);
    }
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

/* A running endpoint stopped by Stop Endpoint; returns the state it is in, -1 when the command failed. A command
   refused with a Context State Error says the state followed here is not the endpoint's, whose output context
   then tells (4.6.9, 4.8.3). sp：装置を中断する前の Stop Endpoint（6.4.3.8、4.15.1.1）。 */
static int endpoint_stop(struct xdev *xd, unsigned dci, bool sp) {
    unsigned state = state_get(xd, dci);
    if (state != EP_RUNNING) return (int)state;
    int cc = hc_command(xd->dev.hc, 0, TRB_TYPE(TRB_STOP_EP) | (sp ? TRB_SP : 0) | dci << 16 | (uint32_t)xd->dev.slot << 24, NULL);
    if (cc == CC_SUCCESS) return EP_STOPPED;
    if (cc == CC_CONTEXT_STATE) return (int)(out_ctx(xd, dci)[0] & 7);
    return -1;
}

/* The endpoint to where Set TR Dequeue Pointer takes it: a running one stopped, a halted one reset
   (TSP 0: its data toggle too, and *halted says so, if asked). Stopped or in error is where it was to go. */
static bool endpoint_quiet(struct xdev *xd, unsigned dci, bool *halted) {
    struct usb_hc *hc = xd->dev.hc;
    unsigned slot = xd->dev.slot;
    if (halted) *halted = false;
    int stopped = endpoint_stop(xd, dci, false);
    if (stopped < 0) return false;
    unsigned state = (unsigned)stopped;
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
    int cc = hc_command(xd->dev.hc, (r->bus + r->enq * sizeof(struct trb)) | r->cycle,
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
    if (removed(d) || xd->ctl_broken || xd->asleep || hc_dead(hc)) return -1;
    bool in = type & 0x80;
    if (!in && length) memcpy(mem, buffer, length);
    uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)length << 48;
    uint64_t f = spin_lock(&xd->ctl_lock);
    xd->ctl_done = false;
    xd->ctl_residual = 0;
    struct trb *first = ring_hold(&xd->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (length ? (in ? 3u : 2u) << 16 : 0), NULL);
    if (length) ring_push(&xd->ep0, bus_of(hc, phys), length, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
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
static bool endpoint_buffer(const struct usb_hc *hc, struct endpoint *ep, unsigned len, unsigned count) {
    if (ep->buf) return ep->len >= len && ep->count >= count;
    if (!len || !count || count > EP_XFERS) return false;
    unsigned stride = (len + 63) & ~63u, pages = (stride * count + 4095) / 4096;
    uint64_t phys = pmm_alloc_contig(pages, 64);
    if (!phys) return false;
    ep->buf = phys_to_virt(phys);
    memset(ep->buf, 0, pages * 4096ull);
    ep->buf_phys = phys;
    ep->buf_bus = bus_of(hc, phys);
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
    kfree(xd->dev.product);
    kfree((void *)xd->dev.config);
    kfree(xd->interfaces);
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
    hc_drain(hc);
}

/* The controller owns the device context and rings until Disable Slot completes; if it never
 * does, the memory stays allocated rather than handed back while the controller may still use it.
 * No drain looks at the slot once it is cleared here. */
static void release_slot(struct usb_hc *hc, unsigned slot, struct xdev *xd) {
    if (xd && xd->published) hal_remove(&xd->dev.hal);
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
    c[2] = (uint32_t)(ep->ring.bus + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.bus + ep->ring.enq * sizeof(struct trb)) >> 32);
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
        if (!ep || !ring_init(hc, &ep->ring)) {
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
        int cc = hc_command(hc, bus_of(hc, xd->in_phys), TRB_TYPE(TRB_CONFIGURE) | (uint32_t)xd->dev.slot << 24, NULL);
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
    if (!ep || ep->mode || !(address & 0x80) || !endpoint_buffer(d->hc, ep, len, count)) return false;
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
    c[2] = (uint32_t)(ep->ring.bus + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.bus + ep->ring.enq * sizeof(struct trb)) >> 32);
    slot_context(xd, out_ctx(xd, 0)[0] >> 27, true);
    int cc = hc_command(d->hc, bus_of(d->hc, xd->in_phys), TRB_TYPE(TRB_CONFIGURE) | slot << 24, NULL);
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
    uint64_t f = spin_lock(&ep->lock);
    bool asleep = ep->asleep; /* 眠りの間は端点の文脈と輪を変えない（4.23.2） */
    spin_unlock(&ep->lock, f);
    if (asleep) return -1;
    endpoint_restart(ep, true);
    if (!pipe_reset((struct xdev *)d, ep)) return -1;
    f = spin_lock(&ep->lock);
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
        if (!endpoint_buffer(d->hc, ep, SYNC_BYTES, 1)) return -1;
        uint64_t f = spin_lock(&ep->lock);
        ep->mode = EP_SYNC;
        spin_unlock(&ep->lock, f);
    }
    bool in = address & 0x80;
    if (!in && len) memcpy(ep->buf, buf, len);
    uint64_t f = spin_lock(&ep->lock);
    if (ep->asleep) {
        spin_unlock(&ep->lock, f);
        return -1;
    }
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
    if (!ep || ep->mode || (address & 0x80) || ep->type != 2 || !endpoint_buffer(d->hc, ep, size, slots)) return false;
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
    if (ep->dead || ep->asleep) {
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

/* The string descriptor index as a new UTF-8 string, NULL without one. USB 2.0 9.6.7: UTF-16LE of at most bLength's 253
   bytes, in language 0x0409 or the first the device offers. Each unit takes at most three bytes in UTF-8. */
char *usb_string(struct usb_device *d, uint8_t index) {
    if (!index) return NULL;
    uint8_t *raw = kmalloc(255);
    if (!raw) return NULL;
    int got = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_STRING << 8, 0, raw, 4);
    uint16_t language = got >= 4 ? (uint16_t)(raw[2] | raw[3] << 8) : 0x0409;
    got = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_STRING << 8 | index), language, raw, 255);
    char *out = NULL;
    if (got >= 2 && raw[1] == USB_DESC_STRING) {
        unsigned end = MIN((unsigned)got, (unsigned)raw[0]);
        out = kmalloc((end / 2) * 3 + 1);
        size_t at = 0;
        for (unsigned i = 2; out && i + 1 < end; i += 2) {
            uint32_t c = (uint32_t)raw[i] | (uint32_t)raw[i + 1] << 8;
            if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) continue;
            if (c < 0x80) {
                out[at++] = (char)c;
            } else if (c < 0x800) {
                out[at++] = (char)(0xC0 | c >> 6);
                out[at++] = (char)(0x80 | (c & 63));
            } else {
                out[at++] = (char)(0xE0 | c >> 12);
                out[at++] = (char)(0x80 | (c >> 6 & 63));
                out[at++] = (char)(0x80 | (c & 63));
            }
        }
        if (out) out[at] = 0;
    }
    kfree(raw);
    return out;
}

const char *usb_product(struct usb_device *d) {
    if (!d->product_read) {
        d->product_read = true;
        d->product = usb_string(d, d->desc.product_name);
        if (d->product) hal_name(&d->hal, d->product);
    }
    return d->product ? d->product : "";
}

/* The next interface of alternate 0 from *at that has an interrupt IN endpoint, with the descriptors between
   it and that endpoint. *at moves past the interface. False when there is no other. */
static bool next_interrupt_interface(const uint8_t *cfg, unsigned total, unsigned *at, const struct usb_interface_desc **intf_out,
                                     const uint8_t **extra_out, unsigned *extra_len_out, const struct usb_endpoint_desc **ep_out) {
    for (unsigned off = *at; off + 2 <= total;) {
        unsigned len = cfg[off];
        if (len < 2 || off + len > total) break;
        const struct usb_interface_desc *intf = (const void *)(cfg + off);
        off += len;
        if (cfg[off - len + 1] != USB_DESC_INTERFACE || len < 9 || intf->alternate) continue;
        const uint8_t *extra = cfg + off;
        unsigned extra_len = 0;
        const struct usb_endpoint_desc *ep = NULL;
        for (unsigned e = off; e + 2 <= total && cfg[e] >= 2 && e + cfg[e] <= total && !ep; e += cfg[e]) {
            if (cfg[e + 1] == USB_DESC_INTERFACE) break;
            if (cfg[e + 1] == USB_DESC_ENDPOINT && cfg[e] >= 7) {
                const struct usb_endpoint_desc *found = (const void *)(cfg + e);
                if ((found->address & 0x80) && (found->attributes & 3) == 3) ep = found;
            } else {
                extra_len = (unsigned)(cfg + e + cfg[e] - extra);
            }
        }
        if (!ep) continue;
        *at = off;
        *intf_out = intf;
        *extra_out = extra;
        *extra_len_out = extra_len;
        *ep_out = ep;
        return true;
    }
    return false;
}

bool usb_interface_interrupt(const struct usb_interface *i, const uint8_t **extra, unsigned *extra_len,
                             const struct usb_endpoint_desc **ep) {
    const struct usb_interface_desc *found;
    unsigned at = i->offset;
    return next_interrupt_interface(i->dev->config, i->dev->config_bytes, &at, &found, extra, extra_len, ep) &&
           found == i->desc;
}

void usb_work_set(struct usb_interface *i, void (*work)(struct usb_interface *)) {
    __atomic_store_n(&i->work, work, __ATOMIC_RELEASE);
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

/* The properties of an interface node (interfaces_publish), as the HAL's rules see them. */
static void interface_props(const struct usb_device *d, const struct usb_interface_desc *intf, struct n_hal_prop props[5]) {
    props[0] = (struct n_hal_prop){.key = N_HAL_PROP_BUS, .value = N_BUS_USB_INTERFACE};
    props[1] = (struct n_hal_prop){.key = N_PROP_USB_CLASS, .value = N_USB_CLASS_CODE(intf->cls, intf->subcls, intf->protocol)};
    props[2] = (struct n_hal_prop){.key = N_PROP_USB_INTERFACE, .value = intf->number};
    props[3] = (struct n_hal_prop){.key = N_PROP_USB_VENDOR, .value = d->desc.vendor};
    props[4] = (struct n_hal_prop){.key = N_PROP_USB_PRODUCT, .value = d->desc.product};
}

/* Does an interface of this configuration have a driver? */
static bool configuration_supported(const struct usb_device *d, const uint8_t *cfg, unsigned total) {
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] != USB_DESC_INTERFACE || cfg[off] < 9 || off + 9 > total) continue;
        const struct usb_interface_desc *intf = (const void *)(cfg + off);
        if (intf->alternate) continue;
        struct n_hal_prop props[5];
        interface_props(d, intf, props);
        if (hal_driver_exists(props, ARRAY_SIZE(props), NULL)) return true;
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
    ep0[2] = (uint32_t)(xd->ep0.bus + xd->ep0.enq * sizeof(struct trb)) | xd->ep0.cycle;
    ep0[3] = (uint32_t)((xd->ep0.bus + xd->ep0.enq * sizeof(struct trb)) >> 32);
    ep0[4] = 8;
    int cc = hc_command(xd->dev.hc, bus_of(xd->dev.hc, xd->in_phys), TRB_TYPE(TRB_ADDRESS) | (uint32_t)xd->dev.slot << 24, NULL);
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

/* The device in the tree, once its descriptor is read: the child of its hub's node, or of the controller's PCI
   function, named by where it is plugged in (the controller, the root port, the port of each hub on the way). */
static void device_publish(struct usb_hc *hc, struct xdev *xd) {
    struct usb_device *d = &xd->dev;
    hal_device_init(&d->hal, N_BUS_USB, NULL);
    hal_prop_number(&d->hal, N_PROP_USB_VENDOR, d->desc.vendor);
    hal_prop_number(&d->hal, N_PROP_USB_PRODUCT, d->desc.product);
    hal_prop_number(&d->hal, N_PROP_USB_RELEASE, d->desc.release);
    hal_prop_number(&d->hal, N_PROP_USB_CLASS, N_USB_CLASS_CODE(d->desc.cls, d->desc.subcls, d->desc.protocol));
    char where[48];
    if (d->parent) ksnprintf(where, sizeof where, "%s.%u", d->parent->hal.location, d->port);
    else ksnprintf(where, sizeof where, "USB %d-%u", hc->index + 1, d->root_port);
    hal_location(&d->hal, where);
    hal_publish(d->parent ? &d->parent->hal : hc->hal, &d->hal);
    xd->published = true;
}

/* Each interface of alternate 0 of the configuration as a node under the device (usb.h): the class drivers'
   rules bind to them. The worker's, after the configuration is in use. */
static void interfaces_publish(struct xdev *xd) {
    struct usb_device *d = &xd->dev;
    const uint8_t *cfg = d->config;
    unsigned total = d->config_bytes, count = 0;
    for (unsigned off = 0; off + 9 <= total && cfg[off] >= 2; off += cfg[off])
        if (cfg[off + 1] == USB_DESC_INTERFACE && cfg[off] >= 9 && !((const struct usb_interface_desc *)(cfg + off))->alternate) count++;
    if (!count || !(xd->interfaces = kzalloc(count * sizeof *xd->interfaces))) return;
    for (unsigned off = 0; off + 9 <= total && cfg[off] >= 2 && xd->ninterfaces < count; off += cfg[off]) {
        const struct usb_interface_desc *intf = (const void *)(cfg + off);
        if (cfg[off + 1] != USB_DESC_INTERFACE || cfg[off] < 9 || intf->alternate) continue;
        struct usb_interface *i = &xd->interfaces[xd->ninterfaces];
        i->dev = d;
        i->desc = intf;
        i->offset = off;
        hal_device_init(&i->hal, N_BUS_USB_INTERFACE, NULL);
        hal_prop_number(&i->hal, N_PROP_USB_CLASS, N_USB_CLASS_CODE(intf->cls, intf->subcls, intf->protocol));
        hal_prop_number(&i->hal, N_PROP_USB_INTERFACE, intf->number);
        hal_prop_number(&i->hal, N_PROP_USB_VENDOR, d->desc.vendor);
        hal_prop_number(&i->hal, N_PROP_USB_PRODUCT, d->desc.product);
        char where[64];
        ksnprintf(where, sizeof where, "%s:%u", d->hal.location, intf->number);
        hal_location(&i->hal, where);
        xd->ninterfaces++;
        hal_publish(&d->hal, &i->hal);
    }
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
        !ring_init(hc, &xd->ep0) || !(xd->xfer = dma_page(&xd->xfer_phys)))
        goto fail;
    hc->dcbaa[slot] = bus_of(hc, xd->out_phys);
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
        cc = hc_command(hc, bus_of(hc, xd->in_phys), TRB_TYPE(TRB_EVALUATE) | (uint32_t)slot << 24, NULL);
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
    device_publish(hc, xd);
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
        hal_failed(&d->hal, -N_ERR_IO);
        return xd; /* stays addressed so a disconnect still frees it */
    }
    /* A hub's descriptor may only be asked of it configured (USB 2.0 11.24.2.5, USB 3.2 10.16.2.3), and
       what it says goes into the Slot Context by the first Configure Endpoint after the slot's Default
       state (xHCI 4.5.2, 6.2.2.2): so a hub is configured as a function, asked, and made anew. */
    if (configuration_has_hub(cfg, (unsigned)got) &&
        (!usbhub_prepare(d) || !device_readdress(xd) || !configure(xd, cfg, (unsigned)got) || !usbhub_set_depth(d))) {
        kprintf("usb: slot %u could not be configured as a hub\n", slot);
        kfree(cfg);
        usbhub_detach(d);
        goto fail;
    }
    d->config = cfg;
    d->config_bytes = (unsigned)got;
    interfaces_publish(xd);
    /* 文字に答えない装置は制御の道を壊すことがある。だからバスが読むのは、その道を使う実装の無い装置の名だけである。
       実装を持つ装置の名は実装が決める（usbhid と usbnet は文字を読み、usbmsc は INQUIRY の名を使う）。hub は読まない：
       下の装置を巻き添えにする。 */
    if (!d->hub && !configuration_supported(d, cfg, (unsigned)got)) usb_product(d);
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
   synchronized with), and every transfer waited for on it ends: the class drivers then drop it
   (hal_remove of each interface, then of the device), and the slot goes. */
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
    /* the class drivers, each after its own users are out; the hub's takes its devices with it */
    for (unsigned i = xd->ninterfaces; i-- > 0;) hal_remove(&xd->interfaces[i].hal);
    xd->ninterfaces = 0;
    usbhub_detach(d); /* a hub that no driver took */
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

/* One try at a port with something at it and no device yet. lost：居た装置が眠りから戻らず（xhci_resume）、つながったまま：
   接続の debounce をせず、すぐ reset する。 */
static void root_port_attach(struct usb_hc *hc, unsigned p, bool lost) {
    bool usb3 = hc->port_major[p] >= 3, retry = hc->port_tries[p].count > 1 || lost;
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
    uint32_t sc = rd32(hc->op, PORTSC(p)), bit = 1u << (p & 31);
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    bool lost = __atomic_fetch_and(&hc->port_lost[p >> 5], ~bit, __ATOMIC_ACQ_REL) & bit;
    if (hc->root[p] && (lost || !(sc & PS_CCS) || (sc & PS_CSC) || !(sc & PS_PED))) {
        /* unplugged, replugged between two looks, or disabled by an error: start over. 眠りから戻らなかった装置も */
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
    root_port_attach(hc, p, lost);
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
            /* usb_bulk's caller recovers. 眠りから起きなかった装置の端点は、その装置が外されるまで眠ったまま */
            if (!ep || !ep->halted || ep->dead || ep->asleep || ep->mode == EP_SYNC) continue;
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
                usbhub_reports_lost(&xd->dev, ep->address);
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
        for (unsigned i = 0; i < xd->ninterfaces && hc->slots[s] == xd && !removed(&xd->dev); i++) {
            void (*work)(struct usb_interface *) = __atomic_load_n(&xd->interfaces[i].work, __ATOMIC_ACQUIRE);
            if (work) work(&xd->interfaces[i]);
        }
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

/* R/S を落とし、HCH を待つ（5.4.2：16 ms のうちに）。割り込みも切る。 */
static bool hc_halt(struct usb_hc *hc) {
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~(1u | CMD_INTE));
    return wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, HALT_NS);
}

/* 止まった制御器だけを reset する（5.4.1）。戻ると全部のレジスターは既定で、制御器はメモリーを手放している。 */
static bool hc_reset(struct usb_hc *hc) {
    wr32(hc->op, USBCMD, CMD_HCRST);
    sleep_until(clock_after(2 * NS_PER_MS));
    return wait_reg(hc->op, USBCMD, CMD_HCRST, 0, NS_PER_S) && wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S);
}

/* 止まらない制御器は bus master を切る：その DMA はもうメモリーに届かない。 */
static bool hc_quiesce(struct usb_hc *hc) {
    if (hc_halt(hc)) return true;
    kprintf("usb: controller %d does not halt, turned off\n", hc->index);
    pci_disable_busmaster(hc->pci, NULL);
    hal_failed(hc->hal, -N_ERR_IO);
    return false;
}

/* 拡張機能の一覧（7.0）：HCCPARAMS1 の xECP から、各項の Next で辿る。0 で終わる。 */
static unsigned cap_first(const struct usb_hc *hc) { return (rd32(hc->cap, 0x10) >> 16) << 2; }
static unsigned cap_next(const struct usb_hc *hc, unsigned x) {
    unsigned next = (rd32(hc->cap, x) >> 8) & 0xFF;
    return next ? x + (next << 2) : 0;
}

/* Ownership (4.22.1): the OS semaphore set and the BIOS's clear (Table 7-4), whoever had it, or the
 * firmware may take it later. Only the OS semaphore's byte: a whole-register write could put back a
 * BIOS bit the firmware cleared in between. 印は Aux の電源の中にあり、それを失えば既定に戻る：Save State から
 * 戻す時にもう一度受け取る。 */
static void hc_take_ownership(struct usb_hc *hc) {
    for (unsigned x = cap_first(hc); x && x + 16 <= hc->bar_size; x = cap_next(hc, x)) {
        uint32_t v = rd32(hc->cap, x);
        if ((v & 0xFF) != 1) continue;
        mmio_write8(hc->cap + x + 3, (uint8_t)(v >> 24) | 1);
        if (!wait_reg(hc->cap, x, 1u << 16, 0, NS_PER_S))
            kprintf("usb: controller %d firmware kept ownership, taking it anyway\n", hc->index);
        uint32_t legacy = rd32(hc->cap, x + 4);
        wr32(hc->cap, x + 4, (legacy & ~0xE011u) | 0xE0000000u); /* SMIs off, stale events acked */
        return;
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
        specs[i] = (struct irq_spec){.name = "xHCI", .handler = xhci_isr, .context = ir};
        /* the event ring segment must not cross a 64 KiB boundary (6.0, Table 6-1) */
        if ((ir->ev = dma_block(EV_PAGES, &ir->ev_phys)) && (ir->erst = dma_page(&ir->erst_phys))) {
            ir->ev_bus = bus_of(hc, ir->ev_phys);
            ir->erst_bus = bus_of(hc, ir->erst_phys);
            continue;
        }
        for (unsigned j = 0; j <= i; j++) interrupter_free(&intr[j]);
        kfree(intr);
        return false;
    }
    hc->intr = intr;
    unsigned got = hal_irq_enable(hc->hal, specs, want, irqs);
    hc->nintr = got ? got : 1;
    __atomic_store_n(&hc->polled, !got, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < got; i++) intr[i].irq = irqs[i];
    hc->messages = got && irqs[0] != hc->pci->intx;
    for (unsigned i = hc->nintr; i < want; i++) interrupter_free(&intr[i]);
    hc->target[RING_CONTROL] = 0;
    hc->target[RING_PERIODIC] = hc->nintr > 1 ? 1 : 0;
    hc->target[RING_BULK] = hc->nintr > 2 ? 2 : 0;
    return true;
}

/* interrupter の IMAN：polled でなければ割り込みを許す。IMOD：割り込みの端点を運ぶものは間引かない（IMODI_GATHER）。
   立ち上げ（interrupter_start）と Save State からの戻し（hc_restore）が同じ値を書く。 */
static uint32_t interrupter_iman(const struct usb_hc *hc) {
    return __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE) ? IMAN_IP : IMAN_IP | IMAN_IE;
}
static uint32_t interrupter_imod(const struct usb_hc *hc, const struct interrupter *ir) {
    return hc->target[RING_PERIODIC] == ir->index ? 0 : IMODI_GATHER;
}

/* R/S を立てて走らせる。割り込みは polled でなければ許し、polled なら見回りの alarm を始める。HCH が落ちなければ
   false で、走っていない。立ち上げ（hc_start）と眠りからの戻り（hc_wake）が呼ぶ。 */
static bool hc_run(struct usb_hc *hc) {
    hc_set_running(hc, true);
    bool polled = __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE);
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) | 1 | (polled ? 0 : CMD_INTE));
    if (!wait_reg(hc->op, USBSTS, STS_HCH, 0, 100 * NS_PER_MS)) {
        hc_set_running(hc, false);
        return false;
    }
    if (polled) alarm_set(&hc->poll, clock_now_ns());
    return true;
}

/* An interrupter's ring empty and in place (4.9.4: ERSTBA last). */
static void interrupter_start(struct usb_hc *hc, struct interrupter *ir) {
    memset(ir->ev, 0, EV_PAGES * 4096);
    ir->erst[0] = ir->ev_bus;
    ir->erst[1] = EV_TRBS;
    ir->ev_idx = 0;
    ir->ev_cycle = 1;
    ir->interrupts = 0;
    wr32(ir->regs, ERSTSZ, 1);
    wr64(ir->regs, ERDP, ir->ev_bus);
    wr64(ir->regs, ERSTBA, ir->erst_bus);
    wr32(ir->regs, IMOD, interrupter_imod(hc, ir));
    wr32(ir->regs, IMAN, interrupter_iman(hc));
}

static bool hc_start(struct usb_hc *hc) {
    struct pci_device *pci = hc->pci;
    /* the capability, operational, runtime, doorbell and extended registers, all in its BAR, mapped by attach */
    hal_enable(hc->hal, HAL_ENABLE_MEMORY | HAL_ENABLE_DMA);
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
    hc_take_ownership(hc);
    for (unsigned x = cap_first(hc); x && x + 16 <= hc->bar_size; x = cap_next(hc, x)) {
        uint32_t v = rd32(hc->cap, x);
        if ((v & 0xFF) != 2) continue;
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
    /* no operational register may be written before Controller Not Ready clears (4.2), and a
     * reset is only allowed once the controller has halted (5.4.1) */
    if (!wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
        kprintf("usb: controller %d never became ready\n", hc->index);
        return false;
    }
    if (!hc_halt(hc)) {
        kprintf("usb: controller %d did not halt\n", hc->index);
        return false;
    }
    if (!hc_reset(hc)) {
        kprintf("usb: controller %d did not come out of reset\n", hc->index);
        return false;
    }
    /* a restart after a fatal error reuses the memory: the reset took it back from the controller */
    if (hc->dcbaa) memset(hc->dcbaa, 0, 4096);
    else if (!(hc->dcbaa = dma_page(&hc->dcbaa_phys))) return false;
    if (hc->cmd.trb) ring_reset(&hc->cmd);
    else if (!ring_init(hc, &hc->cmd)) return false;
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
                while (i) dma_free(phys_of(hc, array[--i]), pages);
                dma_free(array_phys, 2);
                return false;
            }
            memset(phys_to_virt(page), 0, pages * 4096ull);
            array[i] = bus_of(hc, page);
        }
        hc->scratch_phys = array_phys;
    }
    hc->dcbaa[0] = hc->scratch_phys ? bus_of(hc, hc->scratch_phys) : 0;
    wr32(hc->op, CONFIG, (rd32(hc->op, CONFIG) & ~0xFFu) | hc->max_slots);
    wr64(hc->op, DCBAAP, bus_of(hc, hc->dcbaa_phys));
    wr64(hc->op, CRCR, hc->cmd.bus | 1);
    for (unsigned i = 0; i < hc->nintr; i++) interrupter_start(hc, &hc->intr[i]);
    if (!hc_run(hc)) {
        kprintf("usb: controller %d did not start\n", hc->index);
        return false;
    }
    /* a No Op command shows that interrupts arrive (or that they do not: polled from then on) */
    if (hc_command(hc, 0, TRB_TYPE(TRB_NOOP_CMD), NULL) != CC_SUCCESS) {
        kprintf("usb: controller %d does not answer commands\n", hc->index);
        hc_set_running(hc, false);
        return false;
    }
    memset(hc->port_tries, 0, sizeof hc->port_tries); /* the reset took every port back (4.19.1.2.3) */
    memset(hc->port_power, 0, sizeof hc->port_power);
    for (unsigned w = 0; w < 8; w++) __atomic_store_n(&hc->port_lost[w], 0, __ATOMIC_RELEASE);
    if (hcc1 & 8) /* port power control */
        for (unsigned p = 1; p <= hc->max_ports; p++) {
            if (!hc->port_major[p]) continue; /* no protocol defines it: not to be touched (7.2.2) */
            uint32_t sc = rd32(hc->op, PORTSC(p));
            if (!(sc & PS_PP)) wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PP);
        }
    sleep_until(clock_after(20 * NS_PER_MS));
    for (unsigned p = 1; p <= hc->max_ports; p++)
        if (hc->port_major[p]) __atomic_fetch_or(&hc->port_pending[p >> 5], 1u << (p & 31), __ATOMIC_RELEASE);
    bool polled = __atomic_load_n(&hc->polled, __ATOMIC_ACQUIRE);
    kprintf("usb: xHCI %d (%04x:%04x) version %x, %u ports, %u slots, %u interrupter%s of %u, %s\n", hc->index,
            pci->vendor, pci->device, rd32(hc->cap, 0) >> 16, hc->max_ports, hc->max_slots, hc->nintr,
            hc->nintr > 1 ? "s" : "", hc->max_intrs, polled ? "polled" : !hc->messages ? "INTx" : pci->msi_count ? "MSI" : "MSI-X");
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

/* The controller reset so it lets go of memory, and every device of it removed: the class drivers drop theirs.
   False when it will not halt: it keeps its DMA until bus mastering goes off, and stays off. */
static bool hc_stop(struct usb_hc *hc) {
    hc_set_running(hc, false);
    bool halted = hc_quiesce(hc);
    if (halted) hc_reset(hc);
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        struct xdev *gone = hc->root[p];
        hc->root[p] = NULL;
        if (gone) usb_remove(&gone->dev);
    }
    for (unsigned s = 1; s <= hc->max_slots; s++)
        if (hc->slots[s]) usb_remove(&hc->slots[s]->dev);
    __atomic_store_n(&hc->recover, false, __ATOMIC_RELEASE);
    __atomic_store_n(&hc->wedged, false, __ATOMIC_RELEASE);
    return halted;
}

/* A Host System Error or Host Controller Error stops the controller for good (4.24.1, 5.4.2) and
 * every device with it: reset it so it lets go of memory, have the drivers drop their devices,
 * then bring it up again and enumerate from scratch. A controller that keeps failing is left off.
 * 眠りの間に状態を失った制御器（xhci_resume の lost）も同じにするが、失敗には数えない。 */
static bool hc_start_again(struct usb_hc *hc) {
    bool up = hc_start(hc);
    if (up) {
        hc->failed = false;
        hal_recovered(hc->hal);
    } else {
        hal_failed(hc->hal, -N_ERR_IO);
    }
    return up;
}

static void hc_restart(struct usb_hc *hc, bool after_sleep) {
    if (!hc_stop(hc)) return;
    if (after_sleep) {
        kprintf("usb: controller %d starting again after sleep\n", hc->index);
        if (!hc_start_again(hc)) kprintf("usb: controller %d did not come back after sleep\n", hc->index);
        return;
    }
    if (++hc->restarts > 3) {
        kprintf("usb: controller %d keeps failing, left off\n", hc->index);
        hal_failed(hc->hal, -N_ERR_IO);
        return;
    }
    kprintf("usb: controller %d restarting (attempt %u)\n", hc->index, hc->restarts);
    hc_start_again(hc);
}

static void hc_settled(struct usb_hc *hc) {
    if (hc->settled) return;
    hc->settled = true;
    hal_looking_end(); /* its first look at every port and hub is done */
}

/* One round of the worker: restarts, ports, hubs, recoveries and the class drivers' work. */
static void hc_round(struct usb_hc *hc, uint64_t settle_at) {
    if (__atomic_exchange_n(&hc->lost, false, __ATOMIC_ACQ_REL)) hc_restart(hc, true);
    uint32_t sts = hc_running(hc) ? rd32(hc->op, USBSTS) : 0;
    if ((sts == 0xFFFFFFFFu) != hc->gone) {
        hc->gone = !hc->gone;
        kprintf("usb: controller %d %s the bus\n", hc->index, hc->gone ? "is gone from" : "is back on");
        if (hc->gone) hal_failed(hc->hal, -N_ERR_IO);
        else hal_recovered(hc->hal);
    }
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
    if (!hc->settled && clock_now_ns() >= settle_at && !hc_busy(hc)) hc_settled(hc);
}

/* A controller's worker: its bring-up, ports, hubs, enumeration, recoveries and restarts. A fair
   thread: none of this is periodic, and none of it holds a completion up. A round is hal_busy, so a
   change of power waits for it and it waits for the change. Detach ends it. */
static void hc_worker(void *arg) {
    struct usb_hc *hc = arg;
    hal_busy_begin();
    if (!hc_start(hc)) {
        hc->failed = true;
        hal_failed(hc->hal, -N_ERR_IO);
        hc_settled(hc);
    }
    hal_busy_end();
    uint64_t settle_at = clock_after(500 * NS_PER_MS);
    while (!__atomic_load_n(&hc->stopping, __ATOMIC_ACQUIRE)) {
        uint64_t seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
        hal_busy_begin();
        hc_round(hc, settle_at);
        hal_busy_end();
        /* the next look: a recovery retry, settling, else the second after which the controller's
           status is looked at again (a fatal error raises no event) */
        uint64_t now = clock_now_ns();
        uint64_t next = MIN(hc->retry_at, now + NS_PER_S);
        if (!hc->settled && now < settle_at) next = MIN(next, settle_at);
        if (next > now) wq_wait_unless(&hc->work_wq, &hc->kicks, seen, next);
    }
    hal_busy_begin();
    if (hc->op) hc_stop(hc);
    hc_settled(hc);
    hal_busy_end();
    __atomic_store_n(&hc->ended, true, __ATOMIC_RELEASE);
    hc_kick(hc);
}

/* ---- 眠り、休止の写し、電源断（hal.h の suspend と resume） ----
 *
 * 眠り（S3）と休止の写しでは装置は残る。中断した装置は address と構成を持つ（USB 2.0 9.1.1.6、USB 3.2 9.2.5.2）。
 * 止め方は 4.15 と 4.23.2：各装置の端点を SP つきの Stop Endpoint で止め（4.15.1.1）、link を U3 にし、R/S を落とし、
 * 眠りでは Save State で内の状態を写す。戻りは逆に、Restore State（電源を失った時）、R/S、link を U0、端点の doorbell。
 * 電源を失った・抜かれた装置は port が変わって戻り、worker が外して数え直す。制御器が状態を持たず Save State も無い時
 * （休止の写しから戻った：起動のカーネルの電源断が reset した）は、worker が制御器を立て直して全部を数え直す（lost）。
 * 電源断は端点を閉じて止め、reset する：resume は来ず、写しのカーネルは DCBAAP の 0 で状態の無いことを知る。
 *
 * USB の装置の node は動いていない（実装も claim も無い）ので、walk の段は木の順を持たない：同じ制御器の interface は
 * hub の下のものも全部同じ段で、制御器より先に止まり、後に戻る。木の順（深い装置から止め、浅い装置から戻す）はここで
 * 辿る。worker は電源の門で待っていて、hc->slots と装置の木は動かない。 */

/* 生きている装置の一番深い段。 */
static unsigned deepest(const struct usb_hc *hc) {
    unsigned depth = 0;
    for (unsigned s = 1; s <= hc->max_slots; s++)
        if (hc->slots[s] && hc->slots[s]->dev.depth > depth) depth = hc->slots[s]->dev.depth;
    return depth;
}

/* 装置のデータの端点に何も積ませない：usb_bulk、usb_send、usb_clear_halt と stream の積み直しが断る。 */
static void endpoints_close(struct xdev *xd) {
    for (unsigned dci = 2; dci < 32; dci++) {
        struct endpoint *ep = xd->ep[dci];
        if (!ep) continue;
        uint64_t f = spin_lock(&ep->lock);
        ep->asleep = true;
        spin_unlock(&ep->lock, f);
    }
}

/* 装置を中断の前の形に（4.15.1.1）：端点を閉じ、制御の端点は ctl_mutex で進行中の要求の終わりを待ってから閉じ
   （control が断る）、動いている端点を全部止める。止まらない端点は命令の時間切れが言い、止まらない命令の輪は
   Save State を断る（hc_save）。 */
static void device_sleep(struct xdev *xd) {
    endpoints_close(xd);
    mutex_lock(&xd->ctl_mutex);
    xd->asleep = true;
    mutex_unlock(&xd->ctl_mutex);
    for (unsigned dci = 1; dci < 32; dci++) {
        if (dci != 1 && !xd->ep[dci]) continue;
        int state = endpoint_stop(xd, dci, true);
        if (state >= 0) state_set(xd, dci, (unsigned)state);
        else kprintf("usb: slot %u endpoint %u did not stop for sleep\n", xd->dev.slot, dci);
    }
}

/* 深い装置から止める。SuperSpeed の hub の下の装置は、端点の後にその port を U3 にする（USB 3.2 10.16.2.10）：
   Enhanced SuperSpeed の hub は選んだ port の中断だけを持ち（10.10）、上り口が U3 になった hub には要求が届かないので、
   hub より先に下の装置。USB2 の hub の下は root port の中断で木の全部が中断する（USB 2.0 7.1.7.6）。 */
static void devices_sleep(struct usb_hc *hc) {
    for (unsigned depth = deepest(hc) + 1; depth-- > 0;)
        for (unsigned s = 1; s <= hc->max_slots; s++) {
            struct xdev *xd = hc->slots[s];
            if (!xd || xd->dev.depth != depth) continue;
            device_sleep(xd);
            struct usb_device *hub = xd->dev.parent;
            if (hub && hub->speed >= USB_SPEED_SUPER) usbhub_link_suspend(hub, xd->dev.port);
        }
}

/* root port の link を pls へ（Table 5-27：LWS を立てて PLS を書く）。PED と変化の旗は書かない。 */
static void port_link(struct usb_hc *hc, unsigned p, unsigned pls) {
    uint32_t sc = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_LWS | pls << 5);
}

/* bits の root port が pls に着くまで、ns まで待つ。着かなかった port が bits に残る。 */
static void ports_wait(struct usb_hc *hc, uint32_t bits[8], unsigned pls, uint64_t ns) {
    uint64_t deadline = clock_after(ns);
    for (;;) {
        bool left = false;
        for (unsigned w = 0; w < 8; w++)
            for (uint32_t b = bits[w]; b; b &= b - 1) {
                unsigned p = w * 32 + (unsigned)__builtin_ctz(b);
                uint32_t sc = rd32(hc->op, PORTSC(p));
                if (sc != 0xFFFFFFFFu && PS_PLS(sc) == pls) bits[w] &= ~(1u << (p & 31));
                else left = true;
            }
        if (!left || clock_now_ns() >= deadline) return;
        sleep_until(clock_after(NS_PER_MS));
    }
}

/* 有効な root port（PED、PLS が U3 より下）を全部 U3 にする（4.15、4.15.1）。PLS が U3 になるのは中断の信号が
   装置に届いてからで、10 ms までかかり、制御器を止める前に待つ（4.15.1）。 */
static void ports_sleep(struct usb_hc *hc) {
    uint32_t bits[8] = {0};
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        if (!hc->port_major[p]) continue;
        uint32_t sc = rd32(hc->op, PORTSC(p));
        if (sc == 0xFFFFFFFFu || !(sc & PS_PED) || PS_PLS(sc) >= PLS_U3) continue;
        port_link(hc, p, PLS_U3);
        bits[p >> 5] |= 1u << (p & 31);
    }
    ports_wait(hc, bits, PLS_U3, 10 * NS_PER_MS);
    for (unsigned w = 0; w < 8; w++)
        for (uint32_t b = bits[w]; b; b &= b - 1)
            kprintf("usb: controller %d port %u did not suspend\n", hc->index, w * 32 + (unsigned)__builtin_ctz(b));
}

/* 中断した root port を U0 へ（4.15.2.2）。D0 に戻った制御器は U3 と Resume の port を自分では戻さない（4.15）。
   U3 に入れてから 10 ms は戻さない（6.4.3.8）。R/S を立ててから（4.15.2.1 の注）。USB3 の port は U0 を書けば
   LFPS の握手で戻り、装置が起こした Resume も同じ（4.15.2.1）：U3 からの出口は USB_U3_EXIT_NS まで。USB2 の port は
   Resume を書いて TDRSMDN の 20 ms を送り、U0 を書く（4.15.2.2、USB 2.0 7.1.7.7）。その後の TRSMRCY の 10 ms は装置に
   話しかけない。 */
static void ports_wake(struct usb_hc *hc) {
    sleep_until(hc->quiet_at + 10 * NS_PER_MS);
    uint32_t bits[8] = {0}, usb2[8] = {0};
    bool any2 = false;
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        if (!hc->port_major[p]) continue;
        uint32_t sc = rd32(hc->op, PORTSC(p));
        unsigned pls = PS_PLS(sc);
        if (sc == 0xFFFFFFFFu || !(sc & PS_PED) || (pls != PLS_U3 && pls != PLS_RESUME)) continue;
        bits[p >> 5] |= 1u << (p & 31);
        if (hc->port_major[p] >= 3) {
            port_link(hc, p, PLS_U0);
            continue;
        }
        if (pls == PLS_U3) port_link(hc, p, PLS_RESUME);
        usb2[p >> 5] |= 1u << (p & 31);
        any2 = true;
    }
    if (any2) {
        sleep_until(clock_after(20 * NS_PER_MS));
        for (unsigned w = 0; w < 8; w++)
            for (uint32_t b = usb2[w]; b; b &= b - 1) port_link(hc, w * 32 + (unsigned)__builtin_ctz(b), PLS_U0);
    }
    ports_wait(hc, bits, PLS_U0, USB_U3_EXIT_NS);
    if (any2) sleep_until(clock_after(10 * NS_PER_MS));
}

/* 眠りの後も同じ装置がつながっている root port：つながったまま抜き差しが無く（CSC）、D3 の間に接続が変わらず
   （CAS：その時 PLS は当てにならない、4.15）、有効で U0 か省電力の U1、U2 にいる。 */
static bool port_kept(uint32_t sc) {
    return sc != 0xFFFFFFFFu && (sc & PS_CCS) && !(sc & (PS_CSC | PS_CAS)) && (sc & PS_PED) && PS_PLS(sc) <= 2;
}

/* 戻った装置の端点を開け、積んであるものの端点を鳴らす（4.15.1.1、4.23.2 の 10）。止める間に終わった stream の転送は
   transfer_event が積み直さなかったので、ここで積む。 */
static void device_wake(struct xdev *xd) {
    struct usb_hc *hc = xd->dev.hc;
    mutex_lock(&xd->ctl_mutex);
    xd->asleep = false;
    mutex_unlock(&xd->ctl_mutex);
    for (unsigned dci = 2; dci < 32; dci++) {
        struct endpoint *ep = xd->ep[dci];
        if (!ep) continue;
        uint64_t f = spin_lock(&ep->lock);
        ep->asleep = false;
        bool live = !ep->halted && !ep->dead;
        if (live && ep->mode == EP_STREAM)
            for (unsigned b = 0; b < ep->count; b++)
                if (!(ep->busy & (1u << b))) endpoint_push(ep, b, ep->len, false);
        if (live && ep->busy) {
            ep->state = EP_RUNNING;
            doorbell(hc, xd->dev.slot, dci);
        }
        spin_unlock(&ep->lock, f);
    }
}

/* 浅い装置から戻す。root port の装置は port が保ったか（port_kept）。hub の下の装置は hub が戻ってから：SuperSpeed の
   hub ではその port を U0 に戻せたか（usbhub_link_resume）、USB2 の hub の下は上の Resume が下へ伝わる（USB 2.0 11.9）。
   戻らない装置の端点は閉じたままで、worker が外して数え直す：root port の装置は port_lost で、hub の下の装置は hub が
   覚えて（usbhub_link_resume）、戻らない hub の下の装置はその hub と一緒に。 */
static void devices_wake(struct usb_hc *hc) {
    unsigned last = deepest(hc);
    for (unsigned depth = 0; depth <= last; depth++)
        for (unsigned s = 1; s <= hc->max_slots; s++) {
            struct xdev *xd = hc->slots[s];
            if (!xd || xd->dev.depth != depth) continue;
            struct usb_device *hub = xd->dev.parent;
            unsigned p = xd->dev.root_port;
            bool up;
            if (!hub) up = port_kept(rd32(hc->op, PORTSC(p)));
            else if (((struct xdev *)hub)->asleep) up = false;
            else up = hub->speed < USB_SPEED_SUPER || usbhub_link_resume(hub, xd->dev.port);
            if (up) {
                device_wake(xd);
            } else if (!hub) {
                kprintf("usb: controller %d port %u: its device did not stay through the sleep\n", hc->index, p);
                __atomic_fetch_or(&hc->port_lost[p >> 5], 1u << (p & 31), __ATOMIC_RELEASE);
            }
        }
}

/* Save State（4.23.2 の 5）：止めた制御器が内の状態を scratchpad か自分の中へ写す。SSS が落ちて終わり、SRE（RW1C）が
   失敗を言う。止まらなかった命令の輪（wedged）は reset のほかに戻し方が無い（4.6.1.2）。 */
static bool hc_save(struct usb_hc *hc) {
    if (__atomic_load_n(&hc->wedged, __ATOMIC_ACQUIRE)) return false;
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) | CMD_CSS);
    bool done = wait_reg(hc->op, USBSTS, STS_SSS, 0, SAVE_NS);
    bool error = rd32(hc->op, USBSTS) & STS_SRE;
    if (error) wr32(hc->op, USBSTS, STS_SRE);
    if (done && !error) return true;
    kprintf("usb: controller %d could not save its state (%s)\n", hc->index, done ? "error" : "timed out");
    return false;
}

/* Restore State（4.23.2 の戻しの 4 から 7）：レジスターを DCBAAP、CONFIG、各 interrupter の ERSTSZ、ERSTBA、ERDP、
   IMAN、IMOD の順に書き戻し（DNCTRL は使わず既定のまま）、CRS で内の状態を戻し、命令の輪を空から始め直す（止める前に
   空にした）。所有の印は Aux の電源の中にあり、それを失えば既定に戻る（4.22.1）。 */
static bool hc_restore(struct usb_hc *hc) {
    if (!wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
        kprintf("usb: controller %d never became ready after sleep\n", hc->index);
        return false;
    }
    hc_take_ownership(hc);
    wr64(hc->op, DCBAAP, bus_of(hc, hc->dcbaa_phys));
    wr32(hc->op, CONFIG, (rd32(hc->op, CONFIG) & ~0xFFu) | hc->max_slots);
    for (unsigned i = 0; i < hc->nintr; i++) {
        struct interrupter *ir = &hc->intr[i];
        wr32(ir->regs, ERSTSZ, 1);
        wr64(ir->regs, ERSTBA, ir->erst_bus);
        wr64(ir->regs, ERDP, ir->ev_bus + ir->ev_idx * sizeof(struct trb));
        wr32(ir->regs, IMAN, interrupter_iman(hc));
        wr32(ir->regs, IMOD, interrupter_imod(hc, ir));
    }
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) | CMD_CRS);
    bool done = wait_reg(hc->op, USBSTS, STS_RSS, 0, SAVE_NS);
    bool error = rd32(hc->op, USBSTS) & STS_SRE;
    if (error) wr32(hc->op, USBSTS, STS_SRE);
    if (!done || error) {
        kprintf("usb: controller %d could not restore its state (%s)\n", hc->index, done ? "error" : "timed out");
        return false;
    }
    mutex_lock(&hc->cmd_mutex);
    ring_reset(&hc->cmd);
    wr64(hc->op, CRCR, hc->cmd.bus | 1);
    mutex_unlock(&hc->cmd_mutex);
    return true;
}

/* 止めた制御器を走らせ直す：止めたままの状態を持つ時と、Save State から戻せた時。D3cold を通らなければ内の状態は
   残り、Restore State は要らない（4.23.2）。D3hot から D0 への移りと HCRST は Core の電源の側のレジスターを既定に
   戻す（4.23.1）：DCBAAP が自分の DCBAA を指していなければ、レジスターは失われた。 */
static bool hc_wake(struct usb_hc *hc) {
    uint32_t sts = rd32(hc->op, USBSTS);
    if (sts == 0xFFFFFFFFu) return false;
    bool kept = (sts & STS_HCH) && mmio_read64(hc->op + DCBAAP) == bus_of(hc, hc->dcbaa_phys);
    if (!kept && !(hc->saved && hc_restore(hc))) return false;
    if (hc_run(hc)) return true;
    kprintf("usb: controller %d did not start after sleep\n", hc->index);
    return false;
}

/* 電源の段（hal.h）。interface の実装はもう止まり、worker は電源の門で待っている。 */
static bool xhci_suspend(struct hal_device *d, enum hal_power_target target) {
    struct usb_hc *hc = d->driver_data;
    hc->slept = hc->saved = false;
    /* 立ち上がらなかった、立て直しで止まったままの、バスから消えた制御器：止めるものが無い */
    if (!hc->op || !hc_running(hc) || rd32(hc->op, USBSTS) == 0xFFFFFFFFu) return true;
    switch (target) {
    case HAL_POWER_SLEEP:
    case HAL_POWER_IMAGE:
        devices_sleep(hc);
        ports_sleep(hc);
        hc->quiet_at = clock_now_ns();
        break;
    case HAL_POWER_OFF:
        for (unsigned s = 1; s <= hc->max_slots; s++)
            if (hc->slots[s]) endpoints_close(hc->slots[s]);
        break;
    }
    hc->slept = true;
    bool halted = hc_quiesce(hc);
    hc_drain(hc); /* 止まるまでに来た事象：命令の完了と port の変化（4.23.2 の 2） */
    hc_set_running(hc, false);
    if (!halted) return false;
    switch (target) {
    case HAL_POWER_SLEEP: hc->saved = hc_save(hc); break;
    case HAL_POWER_IMAGE: break; /* 写しから戻る時、制御器は起動のカーネルのもの：写す状態は無い */
    case HAL_POWER_OFF: hc_reset(hc); break;
    }
    return true;
}

/* 状態を失った制御器も true：worker が立て直す（hc_round の lost）。それも駄目なら worker が hal_failed で言う。 */
static bool xhci_resume(struct hal_device *d) {
    struct usb_hc *hc = d->driver_data;
    if (!hc->slept) return true;
    hc->slept = false;
    if (hc_wake(hc)) {
        ports_wake(hc);
        devices_wake(hc);
    } else {
        kprintf("usb: controller %d lost its state while asleep, starting it again\n", hc->index);
        __atomic_store_n(&hc->lost, true, __ATOMIC_RELEASE);
    }
    /* 眠りの間の接続の変化は事象にならないことがある：worker が全部の port を見る */
    for (unsigned p = 1; p <= hc->max_ports; p++)
        if (hc->port_major[p]) __atomic_fetch_or(&hc->port_pending[p >> 5], 1u << (p & 31), __ATOMIC_RELEASE);
    hc_kick(hc);
    return true;
}

static int xhci_attach(struct hal_device *d) {
    const struct n_hal_resource *bar = hal_resource(d, N_HAL_MEMORY, 0);
    if (!bar || bar->size < 0x1000) {
        kprintf("usb: the controller at %s has no usable register window\n", d->location);
        return -N_ERR_UNSUPPORTED;
    }
    struct usb_hc *hc = kzalloc(sizeof *hc);
    if (!hc) {
        kprintf("usb: no memory for the controller at %s\n", d->location);
        return -N_ERR_NO_MEMORY;
    }
    hc->index = (int)__atomic_fetch_add(&controllers, 1, __ATOMIC_ACQ_REL);
    hc->hal = d;
    hc->pci = pci_device_of(d);
    hc->bar_size = bar->size;
    if (!(hc->cap = hal_map(d, 0))) {
        kprintf("usb: controller %d: no memory to map its registers\n", hc->index);
        kfree(hc);
        return -N_ERR_NO_MEMORY;
    }
    d->driver_data = hc;
    alarm_init(&hc->poll, poll_fire);
    hal_looking_begin();
    char name[16];
    ksnprintf(name, sizeof name, "usb%d", hc->index);
    if (!(hc->worker = kthread_create(name, hc_worker, hc))) {
        kprintf("usb: no memory for controller %d's worker\n", hc->index);
        hc_settled(hc);
        d->driver_data = NULL;
        kfree(hc);
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}

/* The function leaves, or is given to a program: the worker removes every device, halts the controller and ends.
   Its structures stay, as the HAL turns its interrupts off only after this returns and a drain may still read them. */
static void xhci_detach(struct hal_device *d) {
    struct usb_hc *hc = d->driver_data;
    __atomic_store_n(&hc->stopping, true, __ATOMIC_RELEASE);
    hc_kick(hc);
    for (;;) {
        uint64_t seen = __atomic_load_n(&hc->kicks, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&hc->ended, __ATOMIC_ACQUIRE)) break;
        wq_wait_unless(&hc->work_wq, &hc->kicks, seen, clock_after(10 * NS_PER_MS));
    }
}

static const struct n_hal_rule xhci_rules[] = {N_PCI_CLASS_PROGIF(0x0C, 0x03, 0x30)};
static const struct hal_driver xhci_driver = {
    .name = "xhci",
    .rules = xhci_rules,
    .rule_count = ARRAY_SIZE(xhci_rules),
    .attach = xhci_attach,
    .detach = xhci_detach,
    .suspend = xhci_suspend,
    .resume = xhci_resume,
    .after_sleep = HAL_AFTER_SLEEP_RESUME,
};
HAL_DRIVER(xhci_driver);
