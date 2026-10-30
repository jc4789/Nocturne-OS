/* xHCI host controllers (eXtensible Host Controller Interface for USB, revision 1.2), the USB device
 * lifecycle on them, and the single "usb" thread that drives every controller and class driver. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/sched.h"

#define MAX_HC 8
#define RING_TRBS 256 /* one page; the last entry links back to the first */
#define EV_PAGES 4    /* the event ring: one contiguous segment */
#define EV_TRBS (EV_PAGES * 256)
#define VEC_XHCI 0x40 /* MSI vectors 0x40.. one per controller */

struct trb {
    uint64_t param;
    uint32_t status, control;
};

enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS = 11, TRB_CONFIGURE = 12, TRB_EVALUATE = 13,
    TRB_RESET_EP = 14, TRB_STOP_EP = 15, TRB_SET_DEQUEUE = 16,
    TRB_EV_TRANSFER = 32, TRB_EV_COMMAND = 33, TRB_EV_PORT = 34,
};
#define TRB_TC (1u << 1) /* link: toggle the cycle */
#define TRB_ISP (1u << 2)
#define TRB_CH (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT 13
#define CC_STOPPED 26
#define CC_STOPPED_LENGTH 27
#define CC_STOPPED_SHORT 28
/* endpoint states in the output endpoint context */
#define EP_RUNNING 1
#define EP_HALTED 2

/* operational registers */
#define USBCMD 0x00
#define USBSTS 0x04
#define CRCR 0x18
#define DCBAAP 0x30
#define CONFIG 0x38
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define CMD_INTE (1u << 2)
#define STS_HCH 1u
#define STS_HSE (1u << 2)
#define STS_EINT (1u << 3)
#define STS_CNR (1u << 11)
#define STS_HCE (1u << 12)
#define CRCR_CA (1u << 2)
#define CRCR_CRR (1u << 3)
#define PS_CCS 1u
#define PS_PED (1u << 1)
#define PS_PR (1u << 4)
#define PS_PLS(sc) (((sc) >> 5) & 15)
#define PS_PP (1u << 9)
#define PS_CSC (1u << 17)
#define PS_WRC (1u << 19)
#define PS_PRC (1u << 21)
#define PS_WPR (1u << 31)
#define PLS_INACTIVE 6
#define PLS_COMPLIANCE 10
#define PS_CHANGES (0x7Fu << 17)
#define PS_KEEP (PS_PP | 7u << 25) /* bits a write must carry over; the rest are RW1C or actions */
/* interrupter 0 in the runtime registers */
#define IMAN 0x20
#define IMOD 0x24
#define ERSTSZ 0x28
#define ERSTBA 0x30
#define ERDP 0x38

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

/* An endpoint is the usb thread's, except one that sends (EP_SEND): usb_send queues on it from
   other tasks, so its ring, meta, busy, gen, trbs, inflight, next, halted and dead change only
   under send_lock, on the usb thread too. */
struct endpoint {
    struct ring ring;
    uint8_t dci, address, type, mode; /* type: bmAttributes & 3 */
    unsigned mps, esit;
    uint8_t *buf; /* contiguous DMA area: `count` transfers `stride` bytes apart */
    uint64_t buf_phys;
    unsigned pages;
    unsigned len;           /* bytes per transfer (streams: one interrupt service interval's payload) */
    unsigned stride, count;
    uint32_t busy;          /* transfers in flight */
    uint16_t gen[EP_XFERS];
    uint8_t trbs[EP_XFERS];
    unsigned inflight;      /* ring entries in use */
    unsigned next;          /* send: the slot to try first */
    struct trb_meta meta[RING_TRBS];
    volatile bool sync_done;
    uint8_t sync_cc;
    unsigned sync_got;
    usb_report_fn fn;
    void *ctx;
    bool halted, dead;
    unsigned fails;   /* failed recoveries since the last good transfer */
    uint64_t retry_at; /* when the next recovery may run (clock_now_ns) */
};

struct xdev {
    struct usb_device dev; /* first: class drivers only see this part */
    uint8_t *out_ctx, *in_ctx;
    uint64_t out_phys, in_phys;
    struct ring ep0;
    uint8_t *xfer; /* control transfer bounce page */
    uint64_t xfer_phys;
    volatile bool ctl_done;
    volatile uint8_t ctl_cc;
    volatile uint32_t ctl_residual;
    uint64_t ctl_status_trb;
    struct endpoint *ep[32]; /* by device context index */
    unsigned hub_ports, think_time;
    bool hub, multi_tt;
    bool ctl_broken; /* the control ring could not be restarted: nothing more may be queued on it */
};

struct usb_hc {
    int index;
    struct pci_device *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    unsigned max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;
    struct ring cmd;
    struct trb *ev;
    uint64_t ev_phys;
    uint64_t *erst;
    uint64_t erst_phys, scratch_phys; /* kept across a restart, like the rings */
    unsigned restarts;
    unsigned ev_idx;
    uint32_t ev_cycle;
    volatile bool cmd_done;
    uint64_t cmd_trb;
    uint8_t cmd_cc, cmd_slot;
    uint8_t vector; /* 0: polled */
    bool running, recover, irq_seen, failed;
    volatile bool woke; /* back from a sleep, which reset it: start over on the usb thread */
    volatile uint32_t interrupts;
    uint32_t events;
    struct xdev *slots[256];
    struct xdev *root[256]; /* device on each root port */
    uint8_t port_major[256]; /* USB major revision of each root port, from the protocol capabilities */
    uint8_t port_slot_type[256];
    uint32_t port_pending[8];
};

static struct usb_hc controllers[MAX_HC];
static int ncontrollers;
/* Between the usb thread and the others: usb_kick, usb_more, the calls and each call's done, and
   what CPU 0's interrupt handler reads of a controller (running, vector). The usb thread's waits
   look at usb_kick under it (wq_wait_locked). Taken alone: only the wait queues' locks under it. */
static struct spinlock usb_lock;
static struct wait_queue usb_wq;
static bool usb_kick; /* wakes whichever wait runs; nested waits consume it */
static bool usb_more; /* work for the main loop; only the main loop clears it */
static uint64_t usb_deadline;  /* earliest endpoint recovery retry (CLOCK_NEVER = none) */
static struct task *usb_task;
static bool usb_settled; /* the first pass over every port and hub is done (atomic: read by others) */
/* The endpoints that send (struct endpoint): usb_send on any task, the usb thread's completions and
   recoveries. Taken alone, never across a wait. */
static struct spinlock send_lock;

/* work other tasks hand to the usb thread (usb_run) */
struct usb_call {
    void (*fn)(void *arg);
    void *arg;
    bool done;
    struct usb_call *next;
};
static struct usb_call *calls, **calls_tail = &calls;
static struct wait_queue calls_wq;

static inline uint32_t rd32(volatile uint8_t *b, unsigned off) { return mmio_read32(b + off); }
static inline void wr32(volatile uint8_t *b, unsigned off, uint32_t v) { mmio_write32(b + off, v); }
static inline void wr64(volatile uint8_t *b, unsigned off, uint64_t v) {
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}
#define barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Work for the usb thread: its wait returns at once, and with more, the main loop runs once again.
   Any task or interrupt handler, holding no other lock of this file. */
static void usb_wake(bool more) {
    uint64_t f = spin_lock(&usb_lock);
    if (more) usb_more = true;
    usb_kick = true;
    wq_wake_all(&usb_wq);
    spin_unlock(&usb_lock, f);
}

void usb_note_work(void) { usb_wake(true); }

/* What CPU 0's handler (xhci_isr) reads of a controller. */
static void hc_set_irq(struct usb_hc *hc, bool running, uint8_t vector) {
    uint64_t f = spin_lock(&usb_lock);
    hc->running = running;
    hc->vector = vector;
    spin_unlock(&usb_lock, f);
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

static bool ring_init(struct ring *r) {
    if (!(r->trb = dma_page(&r->phys))) return false;
    r->enq = 0;
    r->cycle = 1;
    r->trb[RING_TRBS - 1].param = r->phys;
    r->trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TC;
    return true;
}

static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    struct trb *t = &r->trb[r->enq];
    uint64_t at = r->phys + r->enq * sizeof(struct trb);
    t->param = param;
    t->status = status;
    barrier();
    t->control = (control & ~1u) | r->cycle;
    if (++r->enq == RING_TRBS - 1) {
        struct trb *link = &r->trb[RING_TRBS - 1];
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void doorbell(struct usb_hc *hc, unsigned slot, unsigned target) {
    barrier();
    mmio_write32(hc->db + slot, target);
}

/* ---- events ---- */

/* Room on the ring for `trbs` more entries? One stays free so a full ring is never mistaken
 * for an empty one, and the link entry is not usable. */
static bool ring_room(const struct endpoint *ep, unsigned trbs) { return ep->inflight + trbs <= RING_TRBS - 2; }

/* Queue transfer b: `len` bytes of its buffer as one TD (xHCI 4.11.2), chained wherever a 64 KiB
 * boundary falls (no TRB buffer may span one), and if `zlp`, a zero-length TD after it that
 * ends a transfer of whole packets. A transfer ends at a short packet or after `len` bytes.
 * send_lock held when the endpoint sends. */
static void endpoint_push(struct endpoint *ep, unsigned b, unsigned len, bool zlp) {
    uint64_t at = ep->buf_phys + (uint64_t)b * ep->stride;
    unsigned done = 0, packets = (len + ep->mps - 1) / ep->mps, trbs = 0;
    bool in = ep->address & 0x80;
    for (;;) {
        unsigned chunk = len - done, room = 0x10000 - (unsigned)((at + done) & 0xFFFF);
        if (chunk > room) chunk = room;
        bool last = done + chunk == len;
        unsigned left = last ? 0 : packets - (done + chunk) / ep->mps; /* TD Size, 4.11.2.4 */
        ep->meta[ep->ring.enq] = (struct trb_meta){(uint8_t)(b + 1), last && !zlp, ep->gen[b], done};
        uint32_t control = TRB_TYPE(TRB_NORMAL) | (in ? TRB_ISP : 0) | (!last ? TRB_CH : zlp ? 0 : TRB_IOC);
        ring_push(&ep->ring, at + done, chunk | (left > 31 ? 31u : left) << 17, control);
        trbs++;
        done += chunk;
        if (last) break;
    }
    if (zlp) {
        ep->meta[ep->ring.enq] = (struct trb_meta){(uint8_t)(b + 1), 1, ep->gen[b], len};
        ring_push(&ep->ring, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        trbs++;
    }
    ep->trbs[b] = (uint8_t)trbs;
    ep->inflight += trbs;
    ep->busy |= 1u << b;
}

/* Every transfer of the endpoint is gone: after a halt or an abort moved the ring past them.
 * send_lock held. */
static void endpoint_forget(struct endpoint *ep) {
    for (unsigned b = 0; b < EP_XFERS; b++)
        if (ep->busy & (1u << b)) ep->gen[b]++;
    ep->busy = 0;
    ep->inflight = 0;
}

/* The endpoint's transfers forgotten and its halt set or cleared, in one step for usb_send. */
static void endpoint_restart(struct endpoint *ep, bool halted) {
    uint64_t f = spin_lock(&send_lock);
    endpoint_forget(ep);
    ep->halted = halted;
    spin_unlock(&send_lock, f);
}

/* A stream endpoint: the usb thread's alone. */
static void endpoint_requeue_all(struct usb_hc *hc, struct xdev *xd, struct endpoint *ep) {
    for (unsigned b = 0; b < ep->count; b++) endpoint_push(ep, b, ep->len, false);
    doorbell(hc, xd->dev.slot, ep->dci);
}

static void transfer_event(struct usb_hc *hc, uint64_t param, uint32_t status, uint32_t control) {
    unsigned slot = control >> 24, dci = (control >> 16) & 31, cc = status >> 24;
    struct xdev *xd = hc->slots[slot];
    if (!xd || xd->dev.removed) return;
    if (dci == 1) {
        if (param == xd->ctl_status_trb || (cc != CC_SUCCESS && cc != CC_SHORT)) {
            xd->ctl_cc = cc;
            xd->ctl_done = true;
        } else if (cc == CC_SHORT) {
            xd->ctl_residual = status & 0xFFFFFF;
        }
        return;
    }
    struct endpoint *ep = xd->ep[dci];
    if (!ep || !ep->mode || ep->dead || param < ep->ring.phys || param >= ep->ring.phys + 4096) return;
    if (cc == CC_STOPPED || cc == CC_STOPPED_LENGTH || cc == CC_STOPPED_SHORT) return; /* our own Stop Endpoint */
    unsigned idx = (unsigned)((param - ep->ring.phys) / sizeof(struct trb));
    uint64_t f = spin_lock(&send_lock);
    struct trb_meta m = idx < RING_TRBS - 1 ? ep->meta[idx] : (struct trb_meta){0};
    unsigned b = m.td - 1u;
    bool live = m.td && b < EP_XFERS && (ep->busy & (1u << b)) && m.gen == ep->gen[b];
    if (cc != CC_SUCCESS && cc != CC_SHORT) {
        /* the endpoint halted on this TD (xHCI 4.10.2): the ring stays put until recovered */
        if (ep->mode == EP_SYNC) {
            if (live) {
                endpoint_forget(ep);
                ep->halted = true;
            }
            spin_unlock(&send_lock, f);
            if (!live) return;
            ep->sync_cc = (uint8_t)cc;
            ep->sync_got = 0;
            ep->sync_done = true;
            return;
        }
        bool first = !ep->halted && !ep->fails;
        ep->halted = true;
        spin_unlock(&send_lock, f);
        if (first) kprintf("usb: slot %u endpoint %u failed (completion %u), resetting it\n", slot, dci, cc);
        hc->recover = true;
        usb_wake(true);
        return;
    }
    /* a short packet ends the TD on any entry (and the controller skips the rest of it);
     * otherwise only the last entry's completion counts */
    if (!live || ep->halted || (cc == CC_SUCCESS && !m.last)) {
        spin_unlock(&send_lock, f);
        return;
    }
    unsigned trb_len = ep->ring.trb[idx].status & 0x1FFFF, residual = status & 0xFFFFFF;
    unsigned got = m.before + (residual < trb_len ? trb_len - residual : 0);
    if (got > ep->len) got = ep->len;
    ep->busy &= ~(1u << b);
    ep->gen[b]++;
    ep->inflight -= ep->trbs[b];
    ep->fails = 0;
    spin_unlock(&send_lock, f);
    if (ep->mode == EP_SYNC) {
        ep->sync_cc = (uint8_t)cc;
        ep->sync_got = got;
        ep->sync_done = true;
        return;
    }
    if (ep->fn) ep->fn(&xd->dev, ep->ctx, ep->buf + (size_t)b * ep->stride, got);
    if (ep->mode != EP_STREAM || xd->dev.removed || ep->halted) return;
    endpoint_push(ep, b, ep->len, false);
    doorbell(hc, slot, dci);
}

static void hc_events(struct usb_hc *hc) {
    if (!hc->running) return;
    bool any = false;
    for (;;) {
        struct trb *e = &hc->ev[hc->ev_idx];
        uint32_t control = e->control;
        if ((control & 1) != hc->ev_cycle) break;
        barrier();
        uint64_t param = e->param;
        uint32_t status = e->status;
        hc->events++;
        switch ((control >> 10) & 63) {
        case TRB_EV_TRANSFER: transfer_event(hc, param, status, control); break;
        case TRB_EV_COMMAND:
            if (param == hc->cmd_trb) {
                hc->cmd_cc = status >> 24;
                hc->cmd_slot = control >> 24;
                hc->cmd_done = true;
            }
            break;
        case TRB_EV_PORT: {
            unsigned port = (param >> 24) & 0xFF;
            if (port) hc->port_pending[port >> 5] |= 1u << (port & 31);
            usb_wake(true);
            break;
        }
        default: break;
        }
        if (++hc->ev_idx == EV_TRBS) {
            hc->ev_idx = 0;
            hc->ev_cycle ^= 1;
        }
        any = true;
    }
    if (any) wr64(hc->rt, ERDP, (hc->ev_phys + hc->ev_idx * sizeof(struct trb)) | 8);
}

static void poll_all(void) {
    for (int i = 0; i < ncontrollers; i++) hc_events(&controllers[i]);
}

static bool any_polled(void) {
    for (int i = 0; i < ncontrollers; i++)
        if (controllers[i].running && !controllers[i].vector) return true;
    return false;
}

/* Wait for a completion flag until the deadline, handling every controller's events meanwhile:
   looked at again each millisecond while a controller is polled, else each 10 ms. */
static bool hc_wait(volatile bool *flag, uint64_t deadline) {
    for (;;) {
        poll_all();
        if (*flag) return true;
        uint64_t now = clock_now_ns();
        if (now >= deadline) return false;
        uint64_t look = now + (any_polled() ? NS_PER_MS : 10 * NS_PER_MS);
        uint64_t f = spin_lock(&usb_lock);
        if (!usb_kick) {
            wq_wait_locked_until(&usb_wq, &usb_lock, MIN(look, deadline));
            spin_take(&usb_lock);
        }
        usb_kick = false;
        spin_unlock(&usb_lock, f);
    }
}

/* CPU 0. */
static void xhci_isr(struct regs *r) {
    uint64_t f = spin_lock(&usb_lock);
    for (int i = 0; i < ncontrollers; i++) {
        struct usb_hc *hc = &controllers[i];
        if (!hc->running || hc->vector != r->vector) continue;
        wr32(hc->op, USBSTS, STS_EINT);
        wr32(hc->rt, IMAN, 3);
        __atomic_fetch_add(&hc->interrupts, 1, __ATOMIC_RELAXED);
    }
    usb_kick = true;
    wq_wake_all(&usb_wq);
    spin_unlock(&usb_lock, f);
}

/* Stopped, halted by a fatal error, or gone from the bus (reads all ones): nothing it is given
 * would complete, so nobody should wait for it. */
static bool hc_dead(struct usb_hc *hc) {
    if (!hc->running) return true;
    uint32_t sts = rd32(hc->op, USBSTS);
    return sts == 0xFFFFFFFFu || (sts & (STS_HSE | STS_HCE));
}

static int hc_command(struct usb_hc *hc, uint64_t param, uint32_t control, uint8_t *slot) {
    if (hc_dead(hc)) return -1;
    hc->cmd_done = false;
    hc->cmd_trb = ring_push(&hc->cmd, param, 0, control);
    doorbell(hc, 0, 0);
    if (!hc_wait(&hc->cmd_done, clock_after(3 * NS_PER_S))) {
        kprintf("usb: controller %d command %u timed out\n", hc->index, (control >> 10) & 63);
        /* abort it so the commands queued after it can run; the ring stops with a completion event */
        wr32(hc->op, CRCR, CRCR_CA);
        wr32(hc->op, CRCR + 4, 0);
        uint64_t deadline = clock_after(5 * NS_PER_S);
        while ((rd32(hc->op, CRCR) & CRCR_CRR) && clock_now_ns() < deadline) {
            poll_all();
            sleep_until(clock_after(NS_PER_MS));
        }
        poll_all();
        return -1;
    }
    if (slot) *slot = hc->cmd_slot;
    return hc->cmd_cc;
}

/* ---- device contexts ---- */

static uint32_t *in_ctx(struct xdev *xd, unsigned index) {
    return (uint32_t *)(xd->in_ctx + index * xd->dev.hc->ctx_size);
}

static uint32_t *out_ctx(struct xdev *xd, unsigned index) {
    return (uint32_t *)(xd->out_ctx + index * xd->dev.hc->ctx_size);
}

static void slot_context(struct xdev *xd, unsigned entries) {
    struct usb_device *d = &xd->dev;
    uint32_t *s = in_ctx(xd, 1);
    s[0] = (d->route & 0xFFFFF) | (uint32_t)d->speed << 20 | (xd->multi_tt ? 1u << 25 : 0) |
           (xd->hub ? 1u << 26 : 0) | (uint32_t)entries << 27;
    s[1] = (uint32_t)d->root_port << 16 | (uint32_t)xd->hub_ports << 24;
    s[2] = d->tt_slot | (uint32_t)d->tt_port << 8 | (uint32_t)(xd->think_time & 3) << 16;
    s[3] = 0;
}

/* Bring a halted or stuck endpoint back to its ring's enqueue point. The endpoint context says
 * whether it halted (Reset Endpoint) or still runs (Stop Endpoint); Set TR Dequeue then needs it
 * stopped. False when the controller refused, so nothing may be queued after the old TRBs. */
static bool endpoint_recover(struct xdev *xd, unsigned dci, struct ring *r) {
    struct usb_hc *hc = xd->dev.hc;
    unsigned slot = xd->dev.slot;
    unsigned state = out_ctx(xd, dci)[0] & 7;
    if (state == EP_HALTED) hc_command(hc, 0, TRB_TYPE(TRB_RESET_EP) | dci << 16 | slot << 24, NULL);
    else if (state == EP_RUNNING) hc_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | dci << 16 | slot << 24, NULL);
    int cc = hc_command(hc, (r->phys + r->enq * sizeof(struct trb)) | r->cycle,
                        TRB_TYPE(TRB_SET_DEQUEUE) | dci << 16 | slot << 24, NULL);
    if (cc != CC_SUCCESS) kprintf("usb: slot %u endpoint %u could not be restarted (%d)\n", slot, dci, cc);
    return cc == CC_SUCCESS;
}

/* After a failed control transfer the ring's dequeue point is wherever the controller stopped;
 * if it cannot be moved past the dead TD, a new request would run behind it or never at all. */
static void control_recover(struct xdev *xd) {
    if (xd->dev.removed || endpoint_recover(xd, 1, &xd->ep0)) return;
    xd->ctl_broken = true;
    kprintf("usb: slot %u control pipe is stuck, the device needs replugging\n", xd->dev.slot);
}

int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length) {
    struct xdev *xd = (struct xdev *)d;
    struct usb_hc *hc = d->hc;
    if (d->removed || xd->ctl_broken || length > 4096 || hc_dead(hc)) return -1;
    bool in = type & 0x80;
    if (!in && length) memcpy(xd->xfer, buffer, length);
    uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)length << 48;
    xd->ctl_done = false;
    xd->ctl_residual = 0;
    ring_push(&xd->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (length ? (in ? 3u : 2u) << 16 : 0));
    if (length) ring_push(&xd->ep0, xd->xfer_phys, length, TRB_TYPE(TRB_DATA) | TRB_ISP | (in ? TRB_DIR_IN : 0));
    xd->ctl_status_trb = ring_push(&xd->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (length && in ? 0 : TRB_DIR_IN));
    doorbell(hc, d->slot, 1);
    if (!hc_wait(&xd->ctl_done, clock_after(2 * NS_PER_S))) {
        kprintf("usb: slot %u request %02x:%02x timed out\n", d->slot, type, request);
        control_recover(xd);
        return -1;
    }
    if (xd->ctl_cc != CC_SUCCESS) {
        control_recover(xd);
        return -1;
    }
    int got = length - (int)(xd->ctl_residual < length ? xd->ctl_residual : length);
    if (in && got > 0) memcpy(buffer, xd->xfer, got);
    return got;
}

static unsigned endpoint_interval(unsigned speed, unsigned interval) {
    if (speed == USB_SPEED_HIGH || speed >= USB_SPEED_SUPER) {
        if (interval < 1) interval = 1;
        return interval > 16 ? 15 : interval - 1;
    }
    unsigned frames = (interval ? interval : 1) * 8, log = 0; /* in 125 us microframes */
    while ((2u << log) <= frames) log++;
    return log < 3 ? 3 : log > 10 ? 10 : log;
}

static unsigned address_dci(uint8_t address) { return (address & 15) * 2 + ((address & 0x80) ? 1 : 0); }

static void free_endpoint(struct endpoint *ep) {
    if (!ep) return;
    if (ep->ring.trb) pmm_free(ep->ring.phys);
    for (unsigned i = 0; ep->buf && i < ep->pages; i++) pmm_free(ep->buf_phys + i * 4096ull);
    kfree(ep);
}

/* The DMA area for `count` transfers of `len` bytes, 64-byte aligned each. */
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
    return d->removed ? NULL : ((struct xdev *)d)->ep[address_dci(address)];
}

static void free_device(struct xdev *xd) {
    for (int i = 0; i < 32; i++) free_endpoint(xd->ep[i]);
    if (xd->out_ctx) pmm_free(xd->out_phys);
    if (xd->in_ctx) pmm_free(xd->in_phys);
    if (xd->ep0.trb) pmm_free(xd->ep0.phys);
    if (xd->xfer) pmm_free(xd->xfer_phys);
    kfree(xd);
}

/* The controller owns the device context and rings until Disable Slot completes; if it never
 * does, the memory stays allocated rather than handed back while the controller may still use it. */
static void release_slot(struct usb_hc *hc, unsigned slot, struct xdev *xd) {
    /* a controller stopped for a restart holds nothing of its old slots any more */
    int cc = hc->running ? hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL) : CC_SUCCESS;
    hc->slots[slot] = NULL;
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
    xd->think_time = think_time;
    xd->multi_tt = multi_tt;
}

struct claim {
    const struct usb_interface_desc *intf;
    const uint8_t *extra;
    unsigned extra_len;
    const struct usb_endpoint_desc *ep;
    const uint8_t *companion; /* SuperSpeed endpoint companion descriptor, if any */
};

static void endpoint_info(const struct usb_endpoint_desc *e, const uint8_t *companion, struct usb_ep_info *out) {
    *out = (struct usb_ep_info){.address = e->address, .attributes = e->attributes, .interval = e->interval,
                                .max_packet = e->max_packet};
    if (companion) {
        out->burst = companion[2];
        out->esit = (uint16_t)(companion[4] | companion[5] << 8);
    }
}

bool usb_endpoint_info(const uint8_t *cfg, unsigned total, unsigned off, struct usb_ep_info *ep) {
    if (off + 7 > total || cfg[off] < 7 || cfg[off + 1] != USB_DESC_ENDPOINT) return false;
    unsigned next = off + cfg[off];
    const uint8_t *companion = next + 6 <= total && cfg[next] >= 6 && cfg[next + 1] == USB_DESC_COMPANION
                                   ? cfg + next : NULL;
    endpoint_info((const void *)(cfg + off), companion, ep);
    return true;
}

/* Fill endpoint context dci for `ep` in the input context. */
static void endpoint_context(struct xdev *xd, const struct endpoint *ep, unsigned interval, unsigned burst) {
    uint32_t *c = in_ctx(xd, ep->dci + 1);
    bool in = ep->address & 0x80, periodic = ep->type == 3;
    unsigned ep_type = (periodic ? 3 : 2) + (in ? 4 : 0); /* xHCI 6.2.3: bulk 2/6, interrupt 3/7 */
    c[0] = periodic ? interval << 16 | (ep->esit >> 16) << 24 : 0;
    c[1] = 3u << 1 | ep_type << 3 | burst << 8 | ep->mps << 16; /* error count 3 */
    c[2] = (uint32_t)(ep->ring.phys + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.phys + ep->ring.enq * sizeof(struct trb)) >> 32);
    /* average TRB length (4.14.1.1: about 1 KiB interrupt, 3 KiB bulk) and Max ESIT Payload */
    c[4] = (periodic ? ep->mps : 3072) | (periodic ? (ep->esit & 0xFFFF) << 16 : 0);
}

static bool open_endpoints(struct xdev *xd, const struct usb_ep_info *eps, int n) {
    struct usb_hc *hc = xd->dev.hc;
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0);
    unsigned top = 1, added = 0;
    icc[1] = 1;
    for (int i = 0; i < n; i++) {
        const struct usb_ep_info *e = &eps[i];
        unsigned dci = address_dci(e->address), type = e->attributes & 3;
        if (xd->ep[dci] || dci < 2 || (type != 2 && type != 3)) continue; /* no isochronous yet */
        struct endpoint *ep = kzalloc(sizeof *ep);
        if (!ep || !ring_init(&ep->ring)) {
            free_endpoint(ep);
            return false;
        }
        unsigned mps = e->max_packet & 0x7FF, burst = 0;
        if (!mps) mps = 8;
        uint32_t esit = 0;
        if (type == 3) {
            burst = (e->max_packet >> 11) & 3; /* high speed: extra transactions per microframe */
            esit = mps * (burst + 1);
        }
        if (xd->dev.speed >= USB_SPEED_SUPER) { /* burst and payload come from the companion */
            burst = e->burst;
            if (type == 3) esit = e->esit ? e->esit : mps * (burst + 1);
        }
        ep->dci = (uint8_t)dci;
        ep->address = e->address;
        ep->type = (uint8_t)type;
        ep->mps = mps;
        ep->esit = esit;
        xd->ep[dci] = ep;
        endpoint_context(xd, ep, endpoint_interval(xd->dev.speed, e->interval), burst);
        icc[1] |= 1u << dci;
        if (dci > top) top = dci;
        added++;
    }
    if (!added) return true;
    unsigned entries = (out_ctx(xd, 0)[0] >> 27);
    slot_context(xd, top > entries ? top : entries);
    int cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_CONFIGURE) | (uint32_t)xd->dev.slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u endpoint configuration failed (%d)\n", xd->dev.slot, cc);
        return false;
    }
    return true;
}

bool usb_open_endpoints(struct usb_device *d, const struct usb_ep_info *eps, int n) {
    return !d->removed && open_endpoints((struct xdev *)d, eps, n);
}

bool usb_stream_start(struct usb_device *d, uint8_t address, unsigned len, unsigned count, usb_report_fn fn, void *ctx) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep || ep->mode || !(address & 0x80) || !endpoint_buffer(ep, len, count)) return false;
    unsigned per = len / 0x10000 + 2; /* entries one transfer can take */
    if (!ring_room(ep, per * count)) return false;
    ep->mode = EP_STREAM;
    ep->fn = fn;
    ep->ctx = ctx;
    endpoint_requeue_all(d->hc, (struct xdev *)d, ep);
    return true;
}

/* An interrupt transfer must be one service interval's payload: a longer one would gather
 * several full-size reports into one completion. */
bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *e, usb_report_fn fn, void *ctx) {
    struct endpoint *ep = find_endpoint(d, e->address);
    if (!ep) return false;
    unsigned len = ep->esit ? ep->esit : ep->mps;
    if (len > 4096) len = 4096;
    unsigned count = 4096 / ((len + 63) & ~63u);
    return usb_stream_start(d, e->address, len, count > 8 ? 8 : count, fn, ctx);
}

/* Reset the controller's side of a pipe: halted endpoints by Reset Endpoint (which clears the
 * data toggle), others by Drop and Add in one Configure Endpoint (xHCI 4.6.8), restarting at
 * the ring's enqueue point either way. */
static bool endpoint_reset(struct xdev *xd, struct endpoint *ep) {
    struct usb_hc *hc = xd->dev.hc;
    unsigned slot = xd->dev.slot, dci = ep->dci;
    unsigned state = out_ctx(xd, dci)[0] & 7;
    if (state == EP_HALTED) {
        endpoint_restart(ep, ep->halted);
        return endpoint_recover(xd, dci, &ep->ring);
    }
    if (state == EP_RUNNING) hc_command(hc, 0, TRB_TYPE(TRB_STOP_EP) | dci << 16 | slot << 24, NULL);
    endpoint_restart(ep, ep->halted);
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0), *c = in_ctx(xd, dci + 1);
    icc[0] = 1u << dci;
    icc[1] = 1u | 1u << dci;
    memcpy(c, out_ctx(xd, dci), hc->ctx_size);
    c[0] &= ~7u;
    c[2] = (uint32_t)(ep->ring.phys + ep->ring.enq * sizeof(struct trb)) | ep->ring.cycle;
    c[3] = (uint32_t)((ep->ring.phys + ep->ring.enq * sizeof(struct trb)) >> 32);
    slot_context(xd, out_ctx(xd, 0)[0] >> 27);
    int cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_CONFIGURE) | slot << 24, NULL);
    if (cc != CC_SUCCESS) kprintf("usb: slot %u endpoint %u could not be reset (%d)\n", slot, dci, cc);
    return cc == CC_SUCCESS;
}

int usb_clear_halt(struct usb_device *d, uint8_t address) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep) return -1;
    bool ok = endpoint_reset((struct xdev *)d, ep);
    /* the device keeps its halt (and data toggle) until CLEAR_FEATURE gets through; until both
     * sides are reset the pipe stays halted and transfers are refused */
    ok = usb_control(d, 0x02, USB_REQ_CLEAR_FEATURE, 0, address, NULL, 0) >= 0 && ok;
    if (ok) {
        uint64_t f = spin_lock(&send_lock);
        ep->halted = false;
        spin_unlock(&send_lock, f);
    }
    return ok ? 0 : -1;
}

int usb_bulk(struct usb_device *d, uint8_t address, void *buf, unsigned len, unsigned timeout_ms) {
    struct xdev *xd = (struct xdev *)d;
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep || current_task != usb_task || ep->type != 2 || (ep->mode && ep->mode != EP_SYNC) || len > SYNC_BYTES ||
        hc_dead(d->hc))
        return -1;
    if (!ep->mode) {
        if (!endpoint_buffer(ep, SYNC_BYTES, 1)) return -1;
        ep->mode = EP_SYNC;
    }
    if (ep->halted) return USB_STALL;
    bool in = address & 0x80;
    if (!in && len) memcpy(ep->buf, buf, len);
    ep->sync_done = false;
    endpoint_push(ep, 0, len, false);
    doorbell(d->hc, d->slot, ep->dci);
    if (!hc_wait(&ep->sync_done, clock_after(timeout_ms * NS_PER_MS))) {
        if (d->removed) return -1;
        kprintf("usb: slot %u endpoint %02x transfer timed out\n", d->slot, address);
        endpoint_restart(ep, ep->halted);
        bool stuck = !endpoint_recover(xd, ep->dci, &ep->ring); /* stop it and skip the TD */
        endpoint_restart(ep, stuck);
        return -1;
    }
    if (ep->sync_cc == CC_STALL) return USB_STALL;
    if (ep->sync_cc != CC_SUCCESS && ep->sync_cc != CC_SHORT) return -1;
    if (in && ep->sync_got) memcpy(buf, ep->buf, ep->sync_got);
    return (int)ep->sync_got;
}

bool usb_send_open(struct usb_device *d, uint8_t address, unsigned size, unsigned slots, usb_report_fn done, void *ctx) {
    struct endpoint *ep = find_endpoint(d, address);
    if (!ep || ep->mode || (address & 0x80) || ep->type != 2 || !endpoint_buffer(ep, size, slots)) return false;
    ep->fn = done;
    ep->ctx = ctx;
    ep->mode = EP_SEND;
    return true;
}

int usb_send(struct usb_device *d, uint8_t address, const void *a, unsigned alen, const void *b, unsigned blen) {
    struct endpoint *ep = find_endpoint(d, address);
    unsigned len = alen + blen;
    if (!ep || ep->mode != EP_SEND || len > ep->len || !len) return -1;
    uint64_t f = spin_lock(&send_lock);
    if (ep->dead) {
        spin_unlock(&send_lock, f);
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
    spin_unlock(&send_lock, f);
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

static void bind_classes(struct xdev *xd, uint8_t *cfg, unsigned total) {
    struct usb_device *d = &xd->dev;
    struct claim claims[8];
    int n = 0;
    struct claim cur = {0};
    const uint8_t *last_ep = NULL;
    for (unsigned off = 0; off + 2 <= total;) {
        unsigned len = cfg[off], type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (cur.intf && cur.ep && n < 8) claims[n++] = cur;
            const struct usb_interface_desc *intf = (const void *)(cfg + off);
            bool want = intf->alternate == 0 &&
                        (intf->cls == 9 || usbhid_claims(intf));
            cur = (struct claim){want ? intf : NULL, cfg + off + len, 0, NULL, NULL};
            last_ep = NULL;
        } else if (type == USB_DESC_ENDPOINT && len >= 7 && cur.intf && !cur.ep) {
            const struct usb_endpoint_desc *e = (const void *)(cfg + off);
            if ((e->address & 0x80) && (e->attributes & 3) == 3) cur.ep = e;
            last_ep = cfg + off;
        } else if (type == 0x30 && len >= 6 && cur.ep && last_ep == (const uint8_t *)cur.ep) {
            cur.companion = cfg + off; /* follows its endpoint directly */
        } else if (cur.intf && !cur.ep) {
            cur.extra_len = (unsigned)(cfg + off + len - cur.extra);
        }
        if (type != USB_DESC_ENDPOINT && type != 0x30) last_ep = NULL;
        off += len;
    }
    if (cur.intf && cur.ep && n < 8) claims[n++] = cur;
    if (!n) return;
    bool hub = false; /* a device is one hub: a second hub interface would share its state */
    for (int i = 0; i < n; i++) {
        if (claims[i].intf->cls != 9) continue;
        if (hub || !usbhub_prepare(d)) claims[i].intf = NULL;
        else hub = true;
    }
    struct usb_ep_info eps[8];
    int neps = 0;
    for (int i = 0; i < n; i++)
        if (claims[i].intf) endpoint_info(claims[i].ep, claims[i].companion, &eps[neps++]);
    if (!neps) return;
    if (!open_endpoints(xd, eps, neps)) {
        kprintf("usb: slot %u endpoints could not be opened, its hub and input functions stay off\n", d->slot);
        return;
    }
    for (int i = 0; i < n && !d->removed; i++) {
        if (!claims[i].intf) continue;
        if (claims[i].intf->cls == 9) usbhub_attach(d, claims[i].ep);
        else usbhid_attach(d, claims[i].intf, claims[i].extra, claims[i].extra_len, claims[i].ep);
    }
}

/* Does an interface of this configuration have a driver? */
static bool configuration_supported(const struct usb_device *d, const uint8_t *cfg, unsigned total) {
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
        if (total > 4096) total = 4096;
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

static struct xdev *device_attach(struct usb_hc *hc, struct usb_device *parent, unsigned port, unsigned root_port,
                                  uint32_t route, unsigned speed, unsigned depth, unsigned tt_slot, unsigned tt_port) {
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
                             .root_port = root_port, .depth = depth, .route = route,
                             .tt_slot = tt_slot, .tt_port = tt_port};
    if (!(xd->out_ctx = dma_page(&xd->out_phys)) || !(xd->in_ctx = dma_page(&xd->in_phys)) ||
        !ring_init(&xd->ep0) || !(xd->xfer = dma_page(&xd->xfer_phys)))
        goto fail;
    hc->dcbaa[slot] = xd->out_phys;
    hc->slots[slot] = xd;

    unsigned mps = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    uint32_t *icc = in_ctx(xd, 0), *ep0 = in_ctx(xd, 2);
    icc[1] = 3;
    slot_context(xd, 1);
    ep0[1] = 3u << 1 | 4u << 3 | mps << 16; /* error count 3, control */
    ep0[2] = (uint32_t)xd->ep0.phys | 1;
    ep0[3] = (uint32_t)(xd->ep0.phys >> 32);
    ep0[4] = 8;
    cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_ADDRESS) | (uint32_t)slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: port %u address assignment failed (%d)\n", port, cc);
        goto fail;
    }
    sleep_until(clock_after(2 * NS_PER_MS));
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &d->desc, 8) < 8) {
        kprintf("usb: port %u device descriptor unreadable\n", port);
        goto fail;
    }
    if (speed < USB_SPEED_SUPER && d->desc.max_packet0 && d->desc.max_packet0 != mps) {
        mps = d->desc.max_packet0;
        icc[0] = 0;
        icc[1] = 2;
        ep0[1] = 3u << 1 | 4u << 3 | mps << 16;
        cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_EVALUATE) | (uint32_t)slot << 24, NULL);
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
    for (int attempt = 0; attempt < 3 && !d->removed && !xd->ctl_broken; attempt++) {
        if (attempt) sleep_until(clock_after((100 * NS_PER_MS) << attempt));
        if ((cfg = choose_configuration(d, &got)) && usb_control(d, 0, USB_REQ_SET_CONFIG, cfg[5], 0, NULL, 0) >= 0)
            break;
        kfree(cfg);
        cfg = NULL;
    }
    if (!cfg) {
        kprintf("usb: slot %u could not be configured\n", slot);
        return xd; /* stays addressed so a disconnect still frees it */
    }
    bind_classes(xd, cfg, (unsigned)got);
    if (!d->removed) usbmsc_attach(d, cfg, (unsigned)got);
    if (!d->removed) usbnet_attach(d, cfg, (unsigned)got);
    kfree(cfg);
    return xd;
fail:
    xd->dev.removed = true; /* late events for the slot are dropped */
fail_slot:
    release_slot(hc, slot, xd);
    return NULL;
}

struct usb_device *usb_attach_child(struct usb_device *hub, unsigned port, unsigned speed) {
    if (hub->depth >= 5 || hub->removed) return NULL;
    uint32_t route = hub->route | (uint32_t)(port > 15 ? 15 : port) << (4 * hub->depth);
    unsigned tt_slot = hub->tt_slot, tt_port = hub->tt_port;
    if (hub->speed == USB_SPEED_HIGH && (speed == USB_SPEED_FULL || speed == USB_SPEED_LOW)) {
        tt_slot = hub->slot;
        tt_port = port;
    }
    struct xdev *xd = device_attach(hub->hc, hub, port, hub->root_port, route, speed, hub->depth + 1, tt_slot, tt_port);
    return xd ? &xd->dev : NULL;
}

void usb_remove(struct usb_device *d) {
    if (d->removed) return;
    struct xdev *xd = (struct xdev *)d;
    struct usb_hc *hc = d->hc;
    d->removed = true;
    kprintf("usb: slot %u (%04x:%04x) removed\n", d->slot, d->desc.vendor, d->desc.product);
    usbhub_detach(d);
    usbhid_detach(d);
    usbmsc_detach(d);
    usbnet_detach(d);
    release_slot(hc, d->slot, xd);
}

/* ---- root ports ---- */

/* Reset a root port (warm reset: the USB3 link reset) and wait for it to finish; true once enabled. */
static bool port_reset(struct usb_hc *hc, unsigned p, bool warm) {
    uint32_t sc = rd32(hc->op, PORTSC(p)), done = warm ? PS_WRC | PS_PRC : PS_PRC;
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (warm ? PS_WPR : PS_PR));
    for (uint64_t deadline = clock_after(500 * NS_PER_MS);
         !(rd32(hc->op, PORTSC(p)) & done) && clock_now_ns() < deadline;)
        sleep_until(clock_after(NS_PER_MS));
    sc = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
    return rd32(hc->op, PORTSC(p)) & PS_PED;
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
    if (!(sc & PS_CCS) || hc->root[p]) return;
    bool usb3 = hc->port_major[p] >= 3;
    if (!usb3) {
        sleep_until(clock_after(100 * NS_PER_MS)); /* connect debounce */
        if (!(rd32(hc->op, PORTSC(p)) & PS_CCS)) return;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (usb3) {
            /* USB3 links train on their own; a stuck link (inactive, compliance) needs a warm reset */
            for (uint64_t deadline = clock_after(NS_PER_S);
                 !(rd32(hc->op, PORTSC(p)) & PS_PED) && clock_now_ns() < deadline;) {
                unsigned pls = PS_PLS(rd32(hc->op, PORTSC(p)));
                if (pls == PLS_INACTIVE || pls == PLS_COMPLIANCE) break;
                sleep_until(clock_after(NS_PER_MS));
            }
            if (attempt || !(rd32(hc->op, PORTSC(p)) & PS_PED)) port_reset(hc, p, true);
        } else {
            port_reset(hc, p, false);
        }
        sc = rd32(hc->op, PORTSC(p));
        if (!(sc & PS_CCS)) return;
        if (!(sc & PS_PED)) {
            kprintf("usb: controller %d port %u connected but not enabled (%08x)\n", hc->index, p, sc);
            continue;
        }
        if ((hc->root[p] = device_attach(hc, NULL, p, p, 0, (sc >> 10) & 15, 0, 0, 0))) break;
        sleep_until(clock_after(100 * NS_PER_MS));
    }
}

static void hc_work(struct usb_hc *hc) {
    if (!hc->running) return;
    for (unsigned w = 0; w < 8; w++) {
        while (hc->port_pending[w]) {
            unsigned b = __builtin_ctz(hc->port_pending[w]);
            hc->port_pending[w] &= ~(1u << b);
            unsigned p = w * 32 + b;
            if (p >= 1 && p <= hc->max_ports) root_port(hc, p);
        }
    }
    if (!hc->recover) return;
    hc->recover = false;
    uint64_t now = clock_now_ns();
    for (unsigned s = 1; s <= hc->max_slots; s++) {
        struct xdev *xd = hc->slots[s];
        for (int i = 2; xd && !xd->dev.removed && i < 32; i++) {
            struct endpoint *ep = xd->ep[i];
            if (!ep || !ep->halted || ep->dead || ep->mode == EP_SYNC) continue; /* usb_bulk's caller recovers */
            if (now < ep->retry_at) { /* backing off: come back later */
                hc->recover = true;
                usb_deadline = MIN(usb_deadline, ep->retry_at);
                continue;
            }
            bool ok = endpoint_recover(xd, i, &ep->ring);
            /* the device keeps its own halt and data toggle until told: CLEAR_FEATURE(ENDPOINT_HALT);
             * if that does not get through, the pipe is still halted and is retried later */
            if (ok) ok = usb_control(&xd->dev, 0x02, USB_REQ_CLEAR_FEATURE, 0, ep->address, NULL, 0) >= 0;
            if (xd->dev.removed) break;
            /* fails counts recoveries until a transfer succeeds again; each one waits twice as long */
            if (++ep->fails >= 8) {
                kprintf("usb: slot %u endpoint %u keeps failing, given up until it is replugged\n", s, i);
                uint64_t f = spin_lock(&send_lock);
                ep->dead = true;
                spin_unlock(&send_lock, f);
                continue;
            }
            ep->retry_at = now + ((10 * NS_PER_MS) << ep->fails);
            if (!ok) {
                hc->recover = true;
                usb_deadline = MIN(usb_deadline, ep->retry_at);
                continue;
            }
            endpoint_restart(ep, false); /* the ring restarts past everything that was queued */
            if (ep->mode == EP_STREAM) endpoint_requeue_all(hc, xd, ep);
            else if (ep->fn) ep->fn(&xd->dev, ep->ctx, NULL, 0); /* sends were dropped: slots are free */
        }
    }
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

static bool hc_start(struct usb_hc *hc) {
    struct pci_device *pci = hc->pci;
    uint64_t base = pci_bar_address(pci, 0), size = 64u << 10;
    if (!base) {
        kprintf("usb: controller %d has no register window\n", hc->index);
        return false;
    }
    /* The BAR is sized only after the firmware let go: the sizing probe turns memory decoding off
     * for a moment, which firmware still driving the controller from SMM would trip over. Until
     * then a provisional window covers the capability, operational and extended registers. */
    if (!(hc->cap = vmm_map_mmio(base, size))) {
        kprintf("usb: controller %d: no memory to map its registers\n", hc->index);
        return false;
    }
    pci_enable_busmaster(pci);
    hc->op = hc->cap + (rd32(hc->cap, 0) & 0xFF);
    uint32_t hcs1 = rd32(hc->cap, 4), hcs2 = rd32(hc->cap, 8), hcc1 = rd32(hc->cap, 0x10);
    hc->max_slots = hcs1 & 0xFF;
    hc->max_ports = hcs1 >> 24;
    hc->ctx_size = (hcc1 & 4) ? 64 : 32;
    hc->db = (volatile uint32_t *)(hc->cap + (rd32(hc->cap, 0x14) & ~3u));
    hc->rt = hc->cap + (rd32(hc->cap, 0x18) & ~0x1Fu);
    if (!(hcc1 & 1)) {
        kprintf("usb: controller %d lacks 64-bit addressing, skipped\n", hc->index);
        return false;
    }
    /* extended capabilities: take the controller from the firmware, learn each port's USB revision */
    for (unsigned x = (hcc1 >> 16) << 2, guard = 0; x && x + 16 <= size && guard < 64; guard++) {
        uint32_t v = rd32(hc->cap, x);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                /* only the OS semaphore byte: a whole-register write could put back a BIOS bit
                 * the firmware cleared in between */
                mmio_write8(hc->cap + x + 3, 1);
                if (!wait_reg(hc->cap, x, 1u << 16, 0, NS_PER_S))
                    kprintf("usb: controller %d firmware kept ownership, taking it anyway\n", hc->index);
            }
            uint32_t legacy = rd32(hc->cap, x + 4);
            wr32(hc->cap, x + 4, (legacy & ~0xE011u) | 0xE0000000u); /* SMIs off, stale events acked */
        } else if ((v & 0xFF) == 2) {
            uint32_t ports = rd32(hc->cap, x + 8), slot_type = rd32(hc->cap, x + 12) & 0x1F;
            unsigned first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (unsigned p = first; p < first + count && p < 256; p++) {
                hc->port_major[p] = v >> 24;
                hc->port_slot_type[p] = (uint8_t)slot_type;
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
    if (!wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, 50 * NS_PER_MS)) {
        kprintf("usb: controller %d did not halt\n", hc->index);
        return false;
    }
    wr32(hc->op, USBCMD, 2);
    sleep_until(clock_after(2 * NS_PER_MS));
    if (!wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S) || !wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
        kprintf("usb: controller %d did not come out of reset\n", hc->index);
        return false;
    }
    uint64_t bar = pci_bar_size(pci, 0);
    if (bar < 0x1000) {
        kprintf("usb: controller %d register window unusable\n", hc->index);
        return false;
    }
    if (bar > size && !vmm_map_mmio(base, bar > (16u << 20) ? 16u << 20 : bar)) {
        kprintf("usb: controller %d: no memory to map its registers\n", hc->index);
        return false;
    }
    /* a restart after a fatal error reuses the memory: the reset took it back from the controller */
    if (hc->dcbaa) memset(hc->dcbaa, 0, 4096);
    else if (!(hc->dcbaa = dma_page(&hc->dcbaa_phys))) return false;
    if (hc->cmd.trb) {
        memset(hc->cmd.trb, 0, 4096);
        hc->cmd.enq = 0;
        hc->cmd.cycle = 1;
        hc->cmd.trb[RING_TRBS - 1].param = hc->cmd.phys;
        hc->cmd.trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TC;
    } else if (!ring_init(&hc->cmd)) {
        return false;
    }
    /* the event ring segment must not cross a 64 KiB boundary (6.0, Table 6-1) */
    if (hc->ev) memset(hc->ev, 0, EV_PAGES * 4096);
    else if (!(hc->ev = dma_block(EV_PAGES, &hc->ev_phys))) return false;
    unsigned scratch = ((hcs2 >> 21) & 0x1F) << 5 | hcs2 >> 27;
    if (scratch && !hc->scratch_phys) { /* up to 1023 entries: the pointer array may need two pages */
        uint64_t array_phys, page;
        uint64_t *array = dma_block(2, &array_phys);
        if (!array) return false;
        for (unsigned i = 0; i < scratch; i++) {
            if (!dma_page(&page)) return false;
            array[i] = page;
        }
        hc->scratch_phys = array_phys;
    }
    hc->dcbaa[0] = hc->scratch_phys;
    if (!hc->erst && !(hc->erst = dma_page(&hc->erst_phys))) return false;
    hc->erst[0] = hc->ev_phys;
    hc->erst[1] = EV_TRBS;
    hc->ev_idx = 0;
    hc->ev_cycle = 1;
    hc->events = 0;
    __atomic_store_n(&hc->interrupts, 0, __ATOMIC_RELAXED);
    hc->irq_seen = false;
    wr32(hc->op, CONFIG, (rd32(hc->op, CONFIG) & ~0xFFu) | hc->max_slots);
    wr64(hc->op, DCBAAP, hc->dcbaa_phys);
    wr64(hc->op, CRCR, hc->cmd.phys | 1);
    wr32(hc->rt, ERSTSZ, 1);
    wr64(hc->rt, ERDP, hc->ev_phys);
    wr64(hc->rt, ERSTBA, hc->erst_phys);
    wr32(hc->rt, IMOD, 1000); /* at most one interrupt per 250 us */
    uint8_t vector = (uint8_t)(VEC_XHCI + hc->index);
    vector_register(vector, xhci_isr);
    if (!pci_msi_enable(pci, vector)) vector = 0;
    /* a polled controller raises no interrupt at all: its legacy line may be shared and unhandled */
    wr32(hc->rt, IMAN, vector ? 3 : 1);
    hc_set_irq(hc, true, vector);
    wr32(hc->op, USBCMD, 1 | (vector ? CMD_INTE : 0));
    if (!wait_reg(hc->op, USBSTS, STS_HCH, 0, 100 * NS_PER_MS)) {
        kprintf("usb: controller %d did not start\n", hc->index);
        hc_set_irq(hc, false, vector);
        return false;
    }
    if (hcc1 & 8) /* port power control */
        for (unsigned p = 1; p <= hc->max_ports; p++) {
            uint32_t sc = rd32(hc->op, PORTSC(p));
            if (!(sc & PS_PP)) wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PP);
        }
    sleep_until(clock_after(20 * NS_PER_MS));
    for (unsigned p = 1; p <= hc->max_ports; p++) hc->port_pending[p >> 5] |= 1u << (p & 31);
    kprintf("usb: xHCI %d (%04x:%04x) version %x, %u ports, %u slots, %s\n", hc->index, pci->vendor, pci->device,
            rd32(hc->cap, 0) >> 16, hc->max_ports, hc->max_slots, hc->vector ? "MSI" : "polled");
    return true;
}

/* A Host System Error or Host Controller Error stops the controller for good (4.24.1, 5.4.2) and
 * every device with it: reset it so it lets go of memory, have the drivers drop their devices,
 * then bring it up again and enumerate from scratch. A controller that keeps failing is left off.
 * After a sleep the same happens, without counting as a failure: the devices lost their addresses. */
static void hc_restart(struct usb_hc *hc, bool after_sleep) {
    hc_set_irq(hc, false, hc->vector);
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~(1u | CMD_INTE));
    wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, 50 * NS_PER_MS);
    wr32(hc->op, USBCMD, 2);
    sleep_until(clock_after(2 * NS_PER_MS));
    wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S);
    for (unsigned p = 1; p <= hc->max_ports; p++) {
        struct xdev *gone = hc->root[p];
        hc->root[p] = NULL;
        if (gone) usb_remove(&gone->dev);
    }
    for (unsigned s = 1; s <= hc->max_slots; s++)
        if (hc->slots[s]) usb_remove(&hc->slots[s]->dev);
    hc->recover = false;
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

/* A call lives on its caller's stack: once done is set the usb thread no longer looks at it. */
static void run_calls(void) {
    for (;;) {
        uint64_t f = spin_lock(&usb_lock);
        struct usb_call *c = calls;
        if (c) {
            calls = c->next;
            if (!calls) calls_tail = &calls;
        }
        spin_unlock(&usb_lock, f);
        if (!c) return;
        c->fn(c->arg);
        f = spin_lock(&usb_lock);
        c->done = true;
        wq_wake_all(&calls_wq);
        spin_unlock(&usb_lock, f);
    }
}

/* Until the usb thread has run the call. The boot task, which runs kmain_late and then becomes the
 * idle task, waits the same way: it lets the others run and halts in between (sys/sched.h). */
static void wait_call(struct usb_call *c) {
    uint64_t f = spin_lock(&usb_lock);
    while (!c->done) {
        wq_wait_locked(&calls_wq, &usb_lock);
        spin_take(&usb_lock);
    }
    spin_unlock(&usb_lock, f);
}

void usb_run(void (*fn)(void *arg), void *arg) {
    if (current_task == usb_task) {
        fn(arg);
        return;
    }
    struct usb_call c = {fn, arg, false, NULL};
    uint64_t f = spin_lock(&usb_lock);
    *calls_tail = &c;
    calls_tail = &c.next;
    usb_more = usb_kick = true;
    wq_wake_all(&usb_wq);
    spin_unlock(&usb_lock, f);
    wait_call(&c);
}

bool usb_enumerating(void) { return usb_task && !__atomic_load_n(&usb_settled, __ATOMIC_ACQUIRE); }

static bool any_port_pending(void) {
    for (int i = 0; i < ncontrollers; i++)
        for (unsigned w = 0; w < 8; w++)
            if (controllers[i].port_pending[w]) return true;
    return false;
}

static void usb_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < ncontrollers; i++) hc_start(&controllers[i]);
    uint64_t settle_at = clock_after(500 * NS_PER_MS);
    for (;;) {
        uint64_t f = spin_lock(&usb_lock);
        usb_more = false;
        spin_unlock(&usb_lock, f);
        usb_deadline = CLOCK_NEVER;
        poll_all();
        run_calls();
        for (int i = 0; i < ncontrollers; i++) {
            struct usb_hc *hc = &controllers[i];
            uint32_t interrupts = __atomic_load_n(&hc->interrupts, __ATOMIC_RELAXED);
            if (interrupts && !hc->irq_seen) { /* the boot log shows that MSI really arrives */
                hc->irq_seen = true;
                kprintf("usb: controller %d interrupts arriving (vector 0x%x)\n", hc->index, hc->vector);
            }
            if (hc->running && hc->vector && !interrupts && hc->events >= 16) {
                /* events keep coming but no interrupt ever did: the message goes nowhere */
                kprintf("usb: controller %d interrupts never arrive, polling instead\n", hc->index);
                wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~CMD_INTE);
                wr32(hc->rt, IMAN, 1);
                hc_set_irq(hc, hc->running, 0);
            }
            if (hc->woke) {
                hc->woke = false;
                hc_restart(hc, true);
            }
            uint32_t sts = hc->running ? rd32(hc->op, USBSTS) : 0;
            if ((sts & (STS_HSE | STS_HCE)) && sts != 0xFFFFFFFFu && !hc->failed) { /* all ones: gone from the bus */
                hc->failed = true;
                kprintf("usb: controller %d stopped on a %s error (status %08x)\n", hc->index,
                        sts & STS_HCE ? "controller" : "host system", sts);
                hc_restart(hc, false);
            }
            hc_work(hc);
        }
        usbhub_work();
        /* boot waits for this: every port and hub looked at once, and USB 3 links (which train
         * by themselves, then report a change) given a moment to show up */
        f = spin_lock(&usb_lock);
        bool more = usb_more;
        spin_unlock(&usb_lock, f);
        if (!usb_settled && !more && !any_port_pending() && !usbhub_pending() && clock_now_ns() >= settle_at)
            __atomic_store_n(&usb_settled, true, __ATOMIC_RELEASE);
        /* the next look: a key repeat, a recovery retry, settling, else a polled controller's 4 ms
           or the second after which the controllers' status is looked at again */
        uint64_t now = clock_now_ns(), next = usbhid_tick(now);
        next = MIN(next, usb_deadline);
        if (!usb_settled) next = MIN(next, now + 50 * NS_PER_MS);
        next = MIN(next, now + (any_polled() ? 4 * NS_PER_MS : NS_PER_S));
        f = spin_lock(&usb_lock);
        /* usb_more: work arrived during a nested wait, which used up the kick */
        if (!usb_more && !usb_kick && next > now) {
            wq_wait_locked_until(&usb_wq, &usb_lock, next);
            spin_take(&usb_lock);
        }
        usb_kick = false;
        spin_unlock(&usb_lock, f);
    }
}

/* Power change (dev/pm.h): halt every controller, which ends its DMA (xHCI 4.24: within 16 ms).
   The disks behind it were flushed before. Interrupts are off: polled. */
static bool xhci_quiesce(void *arg) {
    (void)arg;
    bool all = true;
    for (int i = 0; i < ncontrollers; i++) {
        struct usb_hc *hc = &controllers[i];
        if (!hc->op) continue;
        wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~(1u | CMD_INTE));
        for (uint64_t deadline = clock_after(20 * NS_PER_MS);
             !(rd32(hc->op, USBSTS) & STS_HCH) && clock_now_ns() < deadline;)
            pause();
        if (!(rd32(hc->op, USBSTS) & STS_HCH)) all = false;
    }
    return all;
}

/* After a sleep: every controller is reset and enumerated again by the usb thread (resume must
   not wait, and the class drivers run there). Keyboards and mice come back; a USB disk would come
   back as a new device under a mounted filesystem, so there is no sleep while one is plugged in. */
static bool xhci_resume(void *arg) {
    (void)arg;
    uint64_t f = spin_lock(&usb_lock);
    for (int i = 0; i < ncontrollers; i++)
        if (controllers[i].op) {
            controllers[i].running = false;
            controllers[i].woke = true;
        }
    usb_more = usb_kick = true;
    wq_wake_all(&usb_wq);
    spin_unlock(&usb_lock, f);
    return true; /* the usb thread logs a controller that does not come back (hc_restart) */
}

static const char *xhci_refuse(void *arg) {
    (void)arg;
    return usbmsc_attached() ? "A USB disk is plugged in" : NULL;
}

static struct pm_device xhci_pm = {.name = "xhci", .stage = PM_STAGE_BUS, .quiesce = xhci_quiesce,
                                   .resume = xhci_resume, .refuse = xhci_refuse};

void usb_init(void) {
    for (int i = 0; i < pci_count() && ncontrollers < MAX_HC; i++) {
        struct pci_device *d = pci_at(i);
        if (d->cls == 0x0C && d->subcls == 0x03 && d->progif == 0x30) {
            controllers[ncontrollers].index = ncontrollers;
            controllers[ncontrollers].pci = d;
            ncontrollers++;
        }
    }
    if (!ncontrollers) return;
    usb_task = kthread_create("usb", usb_thread, NULL);
    pm_register(&xhci_pm);
}
