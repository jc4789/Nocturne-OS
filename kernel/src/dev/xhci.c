/* xHCI host controllers (eXtensible Host Controller Interface for USB, revision 1.2), the USB device
 * lifecycle on them, and the single "usb" thread that drives every controller and class driver. */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/pci.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/sched.h"

#define MAX_HC 8
#define RING_TRBS 256 /* one page; the last entry links back to the first */
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

/* operational registers */
#define USBCMD 0x00
#define USBSTS 0x04
#define CRCR 0x18
#define DCBAAP 0x30
#define CONFIG 0x38
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define STS_HCH 1u
#define STS_EINT (1u << 3)
#define STS_CNR (1u << 11)
#define PS_CCS 1u
#define PS_PED (1u << 1)
#define PS_PR (1u << 4)
#define PS_PP (1u << 9)
#define PS_PRC (1u << 21)
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

struct endpoint {
    struct ring ring;
    uint8_t dci;
    uint8_t *buf;
    uint64_t buf_phys;
    unsigned size, count; /* packet buffer size and how many stay queued */
    usb_report_fn fn;
    void *ctx;
    bool halted;
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
    unsigned ev_idx;
    uint32_t ev_cycle;
    volatile bool cmd_done;
    uint64_t cmd_trb;
    uint8_t cmd_cc, cmd_slot;
    uint8_t vector; /* 0: polled */
    bool running, recover, irq_seen;
    volatile uint32_t interrupts;
    struct xdev *slots[256];
    struct xdev *root[256]; /* device on each root port */
    uint8_t port_major[256]; /* USB major revision of each root port, from the protocol capabilities */
    uint32_t port_pending[8];
};

static struct usb_hc controllers[MAX_HC];
static int ncontrollers;
static struct wait_queue usb_wq;
static volatile bool usb_kick;

static inline uint32_t rd32(volatile uint8_t *b, unsigned off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, unsigned off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, unsigned off, uint64_t v) {
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}
#define barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

void usb_note_work(void) { usb_kick = true; }

static void *dma_page(uint64_t *phys) {
    uint64_t p = pmm_alloc_zeroed();
    if (!p) return NULL;
    *phys = p;
    return phys_to_virt(p);
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
    hc->db[slot] = target;
}

/* ---- events ---- */

static void endpoint_queue(struct xdev *xd, struct endpoint *ep, unsigned i) {
    ring_push(&ep->ring, ep->buf_phys + i * ep->size, ep->size, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    (void)xd;
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
    if (!ep || !ep->fn || param < ep->ring.phys || param >= ep->ring.phys + 4096) return;
    if (cc != CC_SUCCESS && cc != CC_SHORT) {
        if (!ep->halted) kprintf("usb: slot %u endpoint %u failed (completion %u), resetting it\n", slot, dci, cc);
        ep->halted = true;
        hc->recover = true;
        usb_kick = true;
        return;
    }
    uint64_t buf = ep->ring.trb[(param - ep->ring.phys) / sizeof(struct trb)].param;
    if (buf < ep->buf_phys || buf >= ep->buf_phys + 4096) return;
    unsigned residual = status & 0xFFFFFF, len = residual < ep->size ? ep->size - residual : 0;
    ep->fn(&xd->dev, ep->ctx, phys_to_virt(buf), len);
    if (xd->dev.removed || ep->halted) return;
    endpoint_queue(xd, ep, (unsigned)((buf - ep->buf_phys) / ep->size));
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
            usb_kick = true;
            break;
        }
        default: break;
        }
        if (++hc->ev_idx == RING_TRBS) {
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

/* Wait for a completion flag, handling every controller's events meanwhile. */
static bool hc_wait(volatile bool *flag, unsigned ms) {
    uint64_t end = uptime_ms() + ms;
    for (;;) {
        poll_all();
        if (*flag) return true;
        if (uptime_ms() >= end) return false;
        uint64_t f = irq_save();
        if (!usb_kick) wq_wait_timeout(&usb_wq, any_polled() ? 1 : 10);
        usb_kick = false;
        irq_restore(f);
    }
}

static void xhci_isr(struct regs *r) {
    for (int i = 0; i < ncontrollers; i++) {
        struct usb_hc *hc = &controllers[i];
        if (!hc->running || hc->vector != r->vector) continue;
        wr32(hc->op, USBSTS, STS_EINT);
        wr32(hc->rt, IMAN, 3);
        hc->interrupts++;
    }
    usb_kick = true;
    wq_wake_all(&usb_wq);
}

static int hc_command(struct usb_hc *hc, uint64_t param, uint32_t control, uint8_t *slot) {
    hc->cmd_done = false;
    hc->cmd_trb = ring_push(&hc->cmd, param, 0, control);
    doorbell(hc, 0, 0);
    if (!hc_wait(&hc->cmd_done, 3000)) {
        kprintf("usb: controller %d command %u timed out\n", hc->index, (control >> 10) & 63);
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

/* Bring a halted or stuck endpoint back to its ring's enqueue point. */
static void endpoint_recover(struct xdev *xd, unsigned dci, struct ring *r, bool halted) {
    struct usb_hc *hc = xd->dev.hc;
    unsigned slot = xd->dev.slot;
    hc_command(hc, 0, TRB_TYPE(halted ? TRB_RESET_EP : TRB_STOP_EP) | dci << 16 | slot << 24, NULL);
    hc_command(hc, (r->phys + r->enq * sizeof(struct trb)) | r->cycle,
               TRB_TYPE(TRB_SET_DEQUEUE) | dci << 16 | slot << 24, NULL);
}

int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length) {
    struct xdev *xd = (struct xdev *)d;
    struct usb_hc *hc = d->hc;
    if (d->removed || length > 4096) return -1;
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
    if (!hc_wait(&xd->ctl_done, 2000)) {
        kprintf("usb: slot %u request %02x:%02x timed out\n", d->slot, type, request);
        endpoint_recover(xd, 1, &xd->ep0, false);
        return -1;
    }
    if (xd->ctl_cc != CC_SUCCESS) {
        endpoint_recover(xd, 1, &xd->ep0, true);
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

static unsigned endpoint_dci(const struct usb_endpoint_desc *e) {
    return (e->address & 15) * 2 + ((e->address & 0x80) ? 1 : 0);
}

static void free_endpoint(struct endpoint *ep) {
    if (!ep) return;
    if (ep->ring.trb) pmm_free(ep->ring.phys);
    if (ep->buf) pmm_free(ep->buf_phys);
    kfree(ep);
}

static void free_device(struct xdev *xd) {
    for (int i = 0; i < 32; i++) free_endpoint(xd->ep[i]);
    if (xd->out_ctx) pmm_free(xd->out_phys);
    if (xd->in_ctx) pmm_free(xd->in_phys);
    if (xd->ep0.trb) pmm_free(xd->ep0.phys);
    if (xd->xfer) pmm_free(xd->xfer_phys);
    kfree(xd);
}

void usb_hub_info(struct usb_device *d, unsigned ports, unsigned think_time, bool multi_tt) {
    struct xdev *xd = (struct xdev *)d;
    xd->hub = true;
    xd->hub_ports = ports;
    xd->think_time = think_time;
    xd->multi_tt = multi_tt;
}

/* Interrupt IN endpoints of the claimed interfaces become device contexts. */
static bool configure_endpoints(struct xdev *xd, const struct usb_endpoint_desc **eps, int n) {
    struct usb_hc *hc = xd->dev.hc;
    memset(xd->in_ctx, 0, 4096);
    uint32_t *icc = in_ctx(xd, 0);
    unsigned top = 1;
    icc[1] = 1;
    for (int i = 0; i < n; i++) {
        const struct usb_endpoint_desc *e = eps[i];
        unsigned dci = endpoint_dci(e);
        if (xd->ep[dci]) continue;
        struct endpoint *ep = kzalloc(sizeof *ep);
        if (!ep || !ring_init(&ep->ring) || !(ep->buf = dma_page(&ep->buf_phys))) {
            free_endpoint(ep);
            return false;
        }
        unsigned mps = e->max_packet & 0x7FF, burst = (e->max_packet >> 11) & 3;
        if (!mps) mps = 8;
        ep->dci = dci;
        ep->size = (mps + 63) & ~63u;
        ep->count = 4096 / ep->size < 8 ? 4096 / ep->size : 8;
        xd->ep[dci] = ep;
        uint32_t esit = mps * (burst + 1);
        uint32_t *c = in_ctx(xd, dci + 1);
        c[0] = endpoint_interval(xd->dev.speed, e->interval) << 16 | (esit >> 16) << 24;
        c[1] = 3u << 1 | 7u << 3 | burst << 8 | mps << 16; /* error count 3, interrupt IN */
        c[2] = (uint32_t)ep->ring.phys | 1;
        c[3] = (uint32_t)(ep->ring.phys >> 32);
        c[4] = mps | (esit & 0xFFFF) << 16;
        icc[1] |= 1u << dci;
        if (dci > top) top = dci;
    }
    unsigned entries = (out_ctx(xd, 0)[0] >> 27);
    slot_context(xd, top > entries ? top : entries);
    int cc = hc_command(hc, xd->in_phys, TRB_TYPE(TRB_CONFIGURE) | (uint32_t)xd->dev.slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: slot %u endpoint configuration failed (%d)\n", xd->dev.slot, cc);
        return false;
    }
    return true;
}

bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *e, usb_report_fn fn, void *ctx) {
    struct xdev *xd = (struct xdev *)d;
    unsigned dci = endpoint_dci(e);
    struct endpoint *ep = xd->ep[dci];
    if (!ep || d->removed) return false;
    ep->fn = fn;
    ep->ctx = ctx;
    for (unsigned i = 0; i < ep->count; i++) endpoint_queue(xd, ep, i);
    doorbell(d->hc, d->slot, dci);
    return true;
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

struct claim {
    const struct usb_interface_desc *intf;
    const uint8_t *extra;
    unsigned extra_len;
    const struct usb_endpoint_desc *ep;
};

static void bind_classes(struct xdev *xd, uint8_t *cfg, unsigned total) {
    struct usb_device *d = &xd->dev;
    struct claim claims[8];
    int n = 0;
    struct claim cur = {0};
    for (unsigned off = 0; off + 2 <= total;) {
        unsigned len = cfg[off], type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == USB_DESC_INTERFACE && len >= 9) {
            if (cur.intf && cur.ep && n < 8) claims[n++] = cur;
            const struct usb_interface_desc *intf = (const void *)(cfg + off);
            bool want = intf->alternate == 0 &&
                        ((intf->cls == 9 && d->speed < USB_SPEED_SUPER) || usbhid_claims(intf));
            cur = (struct claim){want ? intf : NULL, cfg + off + len, 0, NULL};
        } else if (type == USB_DESC_ENDPOINT && len >= 7 && cur.intf && !cur.ep) {
            const struct usb_endpoint_desc *e = (const void *)(cfg + off);
            if ((e->address & 0x80) && (e->attributes & 3) == 3) cur.ep = e;
        } else if (cur.intf && !cur.ep) {
            cur.extra_len = (unsigned)(cfg + off + len - cur.extra);
        }
        off += len;
    }
    if (cur.intf && cur.ep && n < 8) claims[n++] = cur;
    if (!n) return;
    for (int i = 0; i < n; i++)
        if (claims[i].intf->cls == 9 && !usbhub_prepare(d)) claims[i].intf = NULL;
    const struct usb_endpoint_desc *eps[8];
    int neps = 0;
    for (int i = 0; i < n; i++)
        if (claims[i].intf) eps[neps++] = claims[i].ep;
    if (!neps || !configure_endpoints(xd, eps, neps)) return;
    for (int i = 0; i < n && !d->removed; i++) {
        if (!claims[i].intf) continue;
        if (claims[i].intf->cls == 9) usbhub_attach(d, claims[i].ep);
        else usbhid_attach(d, claims[i].intf, claims[i].extra, claims[i].extra_len, claims[i].ep);
    }
}

static struct xdev *device_attach(struct usb_hc *hc, struct usb_device *parent, unsigned port, unsigned root_port,
                                  uint32_t route, unsigned speed, unsigned depth, unsigned tt_slot, unsigned tt_port) {
    uint8_t slot = 0;
    int cc = hc_command(hc, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
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
    sleep_ms(2);
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &d->desc, 8) < 8) {
        kprintf("usb: port %u device descriptor unreadable\n", port);
        goto fail;
    }
    if (speed < USB_SPEED_SUPER && d->desc.max_packet0 && d->desc.max_packet0 != mps) {
        mps = d->desc.max_packet0;
        icc[0] = 0;
        icc[1] = 2;
        ep0[1] = 3u << 1 | 4u << 3 | mps << 16;
        hc_command(hc, xd->in_phys, TRB_TYPE(TRB_EVALUATE) | (uint32_t)slot << 24, NULL);
    }
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, &d->desc, 18) < 18) goto fail;
    uint8_t head[9];
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, head, 9) < 9) goto fail;
    unsigned total = head[2] | head[3] << 8;
    if (total > 4096) total = 4096;
    uint8_t *cfg = kmalloc(total);
    if (!cfg) goto fail;
    int got = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, cfg, total);
    char via[24] = "";
    if (parent) ksnprintf(via, sizeof via, " (hub port %u)", port);
    kprintf("usb: %04x:%04x class %02x on controller %d port %u%s, %s speed, slot %u\n", d->desc.vendor,
            d->desc.product, d->desc.cls, hc->index, root_port, via, speed_name(speed), slot);
    if (got < 9 || usb_control(d, 0, USB_REQ_SET_CONFIG, head[5], 0, NULL, 0) < 0) {
        kfree(cfg);
        kprintf("usb: slot %u could not be configured\n", slot);
        return xd; /* stays addressed so a disconnect still frees it */
    }
    bind_classes(xd, cfg, (unsigned)got);
    kfree(cfg);
    return xd;
fail:
    hc->slots[slot] = NULL;
    hc->dcbaa[slot] = 0;
    if (xd) free_device(xd);
fail_slot:
    hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
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
    if (d->class_detach) d->class_detach(d);
    hc_command(hc, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)d->slot << 24, NULL);
    hc->slots[d->slot] = NULL;
    hc->dcbaa[d->slot] = 0;
    free_device(xd);
}

/* ---- root ports ---- */

static void root_port(struct usb_hc *hc, unsigned p) {
    uint32_t sc = rd32(hc->op, PORTSC(p));
    wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
    if (!(sc & PS_CCS)) {
        if (hc->root[p]) {
            usb_remove(&hc->root[p]->dev);
            hc->root[p] = NULL;
        }
        return;
    }
    if (hc->root[p]) return;
    if (hc->port_major[p] < 3) {
        sleep_ms(50); /* connect debounce */
        sc = rd32(hc->op, PORTSC(p));
        if (!(sc & PS_CCS)) return;
        wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PR);
        for (int t = 0; t < 500 && !(rd32(hc->op, PORTSC(p)) & PS_PRC); t++) sleep_ms(1);
        sc = rd32(hc->op, PORTSC(p));
        wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | (sc & PS_CHANGES));
        sleep_ms(10); /* reset recovery */
    } else {
        for (int t = 0; t < 1000 && !(rd32(hc->op, PORTSC(p)) & PS_PED); t++) sleep_ms(1);
    }
    sc = rd32(hc->op, PORTSC(p));
    if (!(sc & PS_PED)) {
        kprintf("usb: controller %d port %u connected but not enabled (%08x)\n", hc->index, p, sc);
        return;
    }
    hc->root[p] = device_attach(hc, NULL, p, p, 0, (sc >> 10) & 15, 0, 0, 0);
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
    for (unsigned s = 1; s <= hc->max_slots; s++) {
        struct xdev *xd = hc->slots[s];
        for (int i = 2; xd && i < 32; i++) {
            struct endpoint *ep = xd->ep[i];
            if (!ep || !ep->halted) continue;
            endpoint_recover(xd, i, &ep->ring, true);
            ep->halted = false;
            for (unsigned k = 0; k < ep->count; k++) endpoint_queue(xd, ep, k);
            doorbell(hc, s, i);
        }
    }
}

/* ---- controller bring-up ---- */

static bool wait_reg(volatile uint8_t *b, unsigned off, uint32_t mask, uint32_t want, unsigned ms) {
    for (unsigned t = 0; t <= ms; t++) {
        if ((rd32(b, off) & mask) == want) return true;
        sleep_ms(1);
    }
    return false;
}

static bool hc_start(struct usb_hc *hc) {
    struct pci_device *pci = hc->pci;
    uint64_t base = pci_bar_address(pci, 0), size = pci_bar_size(pci, 0);
    if (!base || size < 0x1000) {
        kprintf("usb: controller %d has no register window\n", hc->index);
        return false;
    }
    if (size > (16u << 20)) size = 16u << 20;
    pci_enable_busmaster(pci);
    hc->cap = vmm_map_mmio(base, size);
    hc->op = hc->cap + hc->cap[0];
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
    for (unsigned x = (hcc1 >> 16) << 2, guard = 0; x && x + 12 <= size && guard < 64; guard++) {
        uint32_t v = rd32(hc->cap, x);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                wr32(hc->cap, x, v | 1u << 24);
                if (!wait_reg(hc->cap, x, 1u << 16, 0, 1000))
                    kprintf("usb: controller %d firmware kept ownership, taking it anyway\n", hc->index);
            }
            uint32_t legacy = rd32(hc->cap, x + 4);
            wr32(hc->cap, x + 4, (legacy & ~0xE011u) | 0xE0000000u); /* SMIs off, stale events acked */
        } else if ((v & 0xFF) == 2) {
            uint32_t ports = rd32(hc->cap, x + 8);
            unsigned first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (unsigned p = first; p < first + count && p < 256; p++) hc->port_major[p] = v >> 24;
        }
        unsigned next = (v >> 8) & 0xFF;
        if (!next) break;
        x += next << 2;
    }
    wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~1u);
    wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, 50);
    wr32(hc->op, USBCMD, 2);
    sleep_ms(2);
    if (!wait_reg(hc->op, USBCMD, 2, 0, 1000) || !wait_reg(hc->op, USBSTS, STS_CNR, 0, 1000)) {
        kprintf("usb: controller %d did not come out of reset\n", hc->index);
        return false;
    }
    if (!(hc->dcbaa = dma_page(&hc->dcbaa_phys)) || !ring_init(&hc->cmd) || !(hc->ev = dma_page(&hc->ev_phys)))
        return false;
    unsigned scratch = ((hcs2 >> 21) & 0x1F) << 5 | hcs2 >> 27;
    if (scratch) {
        uint64_t array_phys, page;
        uint64_t *array = dma_page(&array_phys);
        if (!array) return false;
        for (unsigned i = 0; i < scratch; i++) {
            if (!dma_page(&page)) return false;
            array[i] = page;
        }
        hc->dcbaa[0] = array_phys;
    }
    uint64_t erst_phys;
    uint64_t *erst = dma_page(&erst_phys);
    if (!erst) return false;
    erst[0] = hc->ev_phys;
    erst[1] = RING_TRBS;
    hc->ev_cycle = 1;
    wr32(hc->op, CONFIG, (rd32(hc->op, CONFIG) & ~0xFFu) | hc->max_slots);
    wr64(hc->op, DCBAAP, hc->dcbaa_phys);
    wr64(hc->op, CRCR, hc->cmd.phys | 1);
    wr32(hc->rt, ERSTSZ, 1);
    wr64(hc->rt, ERDP, hc->ev_phys);
    wr64(hc->rt, ERSTBA, erst_phys);
    wr32(hc->rt, IMOD, 1000); /* at most one interrupt per 250 us */
    hc->vector = VEC_XHCI + hc->index;
    vector_register(hc->vector, xhci_isr);
    if (!pci_msi_enable(pci, hc->vector)) hc->vector = 0;
    wr32(hc->rt, IMAN, 3);
    hc->running = true;
    wr32(hc->op, USBCMD, 1 | 4);
    if (!wait_reg(hc->op, USBSTS, STS_HCH, 0, 100)) {
        kprintf("usb: controller %d did not start\n", hc->index);
        hc->running = false;
        return false;
    }
    if (hcc1 & 8) /* port power control */
        for (unsigned p = 1; p <= hc->max_ports; p++) {
            uint32_t sc = rd32(hc->op, PORTSC(p));
            if (!(sc & PS_PP)) wr32(hc->op, PORTSC(p), (sc & PS_KEEP) | PS_PP);
        }
    sleep_ms(20);
    for (unsigned p = 1; p <= hc->max_ports; p++) hc->port_pending[p >> 5] |= 1u << (p & 31);
    kprintf("usb: xHCI %d (%04x:%04x) version %x, %u ports, %u slots, %s\n", hc->index, pci->vendor, pci->device,
            rd32(hc->cap, 0) >> 16, hc->max_ports, hc->max_slots, hc->vector ? "MSI" : "polled");
    return true;
}

static void usb_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < ncontrollers; i++) hc_start(&controllers[i]);
    for (;;) {
        poll_all();
        for (int i = 0; i < ncontrollers; i++) {
            struct usb_hc *hc = &controllers[i];
            if (hc->interrupts && !hc->irq_seen) { /* the boot log shows that MSI really arrives */
                hc->irq_seen = true;
                kprintf("usb: controller %d interrupts arriving (vector 0x%x)\n", hc->index, hc->vector);
            }
            hc_work(hc);
        }
        usbhub_work();
        uint64_t now = uptime_ms(), next = usbhid_tick(now);
        uint64_t wait = any_polled() ? 4 : 1000;
        if (next) wait = next <= now ? 0 : (next - now < wait ? next - now : wait);
        uint64_t f = irq_save();
        if (wait && !usb_kick) wq_wait_timeout(&usb_wq, wait);
        usb_kick = false;
        irq_restore(f);
    }
}

void usb_init(void) {
    for (int i = 0; i < pci_count() && ncontrollers < MAX_HC; i++) {
        struct pci_device *d = pci_at(i);
        if (d->cls == 0x0C && d->subcls == 0x03 && d->progif == 0x30) {
            controllers[ncontrollers].index = ncontrollers;
            controllers[ncontrollers].pci = d;
            ncontrollers++;
        }
    }
    if (ncontrollers) kthread_create("usb", usb_thread, NULL);
}
