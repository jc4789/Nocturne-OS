/* USB hubs: USB 2.0 hubs (USB 2.0 chapter 11) and Enhanced SuperSpeed hubs (USB 3.2 chapter 10):
 * port power, connect, reset and the devices behind them. Keyboards often carry a hub, and boards
 * wire front panels and some rear ports through one. A USB 3 hub is two hubs in one package; its
 * SuperSpeed half shows up on a USB 3 root port and its USB 2.0 half on a USB 2 one. Everything
 * here runs on the hub's controller worker but hub_attach, which runs in its interface's attach
 * thread, and hub_report, which runs in the drain (dev/usb.h). */
#include "kernel.h"
#include "dev/usb.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/wait.h"

/* feature selectors (USB 2.0 Table 11-17, USB 3.2 Table 10-9) */
#define C_HUB_LOCAL_POWER 0
#define C_HUB_OVER_CURRENT 1
#define PORT_ENABLE 1
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE 17
#define C_PORT_SUSPEND 18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET 20
#define C_PORT_LINK_STATE 25
#define C_PORT_CONFIG_ERROR 26
#define BH_PORT_RESET 28
#define C_BH_PORT_RESET 29
#define SET_HUB_DEPTH 12
#define DESC_BOS 0x0F
#define DEVICE_CAPABILITY 0x10
#define CAP_SSP 0x0A       /* SuperSpeedPlus USB device capability (USB 3.2 9.6.2.5) */
#define PORT_STATUS_EXT 2  /* Get Port Status's wValue: with dwExtPortStatus (USB 3.2 10.16.2.6) */

/* wPortStatus / wPortChange (USB 2.0 Tables 11-21, 11-22; USB 3.2 Tables 10-13, 10-14) */
#define PS_CONNECTION (1u << 0)
#define PS_ENABLE (1u << 1)
#define PS_OVER_CURRENT (1u << 3)
#define PS_RESET (1u << 4)
#define PS_USB2_POWER (1u << 8)  /* USB 2.0 only */
#define PS_SS_POWER (1u << 9)    /* SuperSpeed only */
#define PS_LOW_SPEED (1u << 9)   /* USB 2.0 only */
#define PS_HIGH_SPEED (1u << 10) /* USB 2.0 only */
#define PS_LINK(s) (((s) >> 5) & 15) /* SuperSpeed only */
#define LINK_INACTIVE 6
#define LINK_COMPLIANCE 10
#define CH_CONNECTION (1u << 0)
#define CH_ENABLE (1u << 1) /* USB 2.0: disabled by an error */
#define CH_OVER_CURRENT (1u << 3)
#define CH_RESET (1u << 4)
#define CH_BH_RESET (1u << 5) /* SuperSpeed */

struct hub {
    struct usb_device *dev;
    unsigned ports, power_ms;
    bool ss;         /* Enhanced SuperSpeed hub */
    bool hub_change; /* atomic: bit 0 of the status change report, the hub's own status changed */
    bool bound;      /* atomic: an interface of the device took the hub: a device is one hub */
    bool reporting;  /* its status change endpoint runs: a port change comes as a report */
    /* it does not, or a change acknowledged waits for it to answer again: its ports are read each
       second, by no document's time */
    bool asking;
    uint64_t ask_at;
    unsigned ask_gap; /* seconds: doubled while it does not answer, to 64 */
    uint8_t report_ep;
    bool local;      /* it has had local power: a hub without any says "lost" and runs on the bus's */
    bool off;        /* local power lost or over-current (wHubStatus): every port is off and stays so */
    bool hub_held;   /* a change of its own acknowledged, not yet acted on */
    /* a SuperSpeedPlus hub's Sublink Speed Attributes (USB 3.2 9.6.2.5), NULL for any other */
    uint32_t *sublinks;
    unsigned nsublinks;
    struct usb_device **child; /* by port, from 1 */
    struct usb_port_tries *tries; /* by port, from 1 */
    struct usb_port_tries *power; /* by port, from 1: its power turned on again after an over-current */
    uint16_t *held; /* by port, from 1: changes acknowledged, not yet acted on */
    uint32_t *pending; /* atomic words: bit p of the words, the ports whose status changed */
};

static unsigned pending_words(const struct hub *h) { return (h->ports >> 5) + 1; }

/* The reads of a status until no change is left, by no document's count: a change left after them is
   not acknowledged, and is reported again. */
#define CHANGE_READS 8

static void pending_mark(struct hub *h, unsigned port) {
    __atomic_fetch_or(&h->pending[port >> 5], 1u << (port & 31), __ATOMIC_RELEASE);
}

static void hub_ask(struct hub *h) {
    if (h->asking) return;
    h->asking = true;
    h->ask_gap = 1;
}

static bool removed_hub(const struct hub *h) { return __atomic_load_n(&h->dev->removed, __ATOMIC_ACQUIRE); }

static int port_feature(struct usb_device *d, uint8_t request, uint16_t feature, unsigned port) {
    return usb_control(d, 0x23, request, feature, (uint16_t)port, NULL, 0);
}

static bool port_status(struct usb_device *d, unsigned port, uint16_t *status, uint16_t *change) {
    uint8_t st[4];
    if (usb_control(d, 0xA3, USB_REQ_GET_STATUS, 0, (uint16_t)port, st, 4) < 4) return false;
    *status = st[0] | st[1] << 8;
    *change = st[2] | st[3] << 8;
    return true;
}

/* Acknowledge every change bit set: each has its own feature selector, and they differ between
 * USB 2.0 hubs (bits 0-4) and SuperSpeed hubs (bits 0, 3-7). */
static void clear_changes(struct hub *h, unsigned port, uint16_t change) {
    static const uint8_t usb2[5] = {C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND, C_PORT_OVER_CURRENT,
                                    C_PORT_RESET};
    static const uint8_t ss[8] = {C_PORT_CONNECTION, 0, 0, C_PORT_OVER_CURRENT, C_PORT_RESET, C_BH_PORT_RESET,
                                  C_PORT_LINK_STATE, C_PORT_CONFIG_ERROR};
    const uint8_t *map = h->ss ? ss : usb2;
    unsigned n = h->ss ? 8 : 5;
    for (unsigned b = 0; b < n; b++)
        if ((change & (1u << b)) && map[b]) port_feature(h->dev, USB_REQ_CLEAR_FEATURE, map[b], port);
}

/* The port's status with its changes acknowledged: read again after that, as what happened between
   the read and that is told no more (USB 2.0 Figure 11-21, USB 3.2 Figure 10-23). *change: all it
   read, with what the last call could not finish with. False if the hub stops answering: what it
   acknowledged waits for the next read. */
static bool port_changes(struct hub *h, unsigned port, uint16_t *status, uint16_t *change) {
    *change = h->held[port];
    for (int i = 0;; i++) {
        uint16_t now;
        if (!port_status(h->dev, port, status, &now)) {
            h->held[port] = *change;
            if (*change) hub_ask(h);
            return false;
        }
        *change |= now;
        if (!now || i == CHANGE_READS - 1) {
            h->held[port] = 0;
            return true;
        }
        clear_changes(h, port, now);
    }
}

/* A SuperSpeedPlus hub says so in its BOS descriptor (USB 3.2 9.6.2): its Sublink Speed Attributes, by
   which the speed in a port's extended status is read. */
static void read_sublinks(struct hub *h) {
    struct usb_device *d = h->dev;
    uint8_t head[5];
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, DESC_BOS << 8, 0, head, sizeof head) < 5) return;
    unsigned total = head[2] | (unsigned)head[3] << 8;
    if (total < 5 || total > 4096) return;
    uint8_t *bos = kmalloc(total);
    if (!bos) return;
    int got = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, DESC_BOS << 8, 0, bos, (uint16_t)total);
    for (unsigned off = 5; got > 0 && off + 3 <= (unsigned)got && bos[off] >= 3 && off + bos[off] <= (unsigned)got;
         off += bos[off]) {
        if (bos[off + 1] != DEVICE_CAPABILITY || bos[off + 2] != CAP_SSP || bos[off] < 12) continue;
        unsigned attrs = bos[off + 4] & 31;
        unsigned count = attrs + 1u;
        if (12 + count * 4 > bos[off] || !(h->sublinks = kmalloc(count * sizeof *h->sublinks))) break;
        for (unsigned i = 0; i < count; i++) {
            const uint8_t *a = bos + off + 12 + i * 4;
            h->sublinks[i] = a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24;
        }
        h->nsublinks = count;
        break;
    }
    kfree(bos);
}

bool usbhub_prepare(struct usb_device *d) {
    bool ss = d->speed >= USB_SPEED_SUPER;
    uint8_t desc[16];
    /* the SuperSpeed hub descriptor (type 2Ah) keeps the port count and power-on time where the
     * USB 2.0 one (29h) has them */
    if (usb_control(d, 0xA0, USB_REQ_GET_DESCRIPTOR, ss ? 0x2A00 : 0x2900, 0, desc, sizeof desc) < 7) {
        kprintf("usbhub: slot %u hub descriptor unreadable\n", d->slot);
        return false;
    }
    struct hub *h = kzalloc(sizeof *h);
    if (!h) return false;
    h->dev = d;
    h->ss = ss;
    h->ports = desc[2];
    h->power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;
    h->child = kzalloc((h->ports + 1u) * sizeof *h->child);
    h->tries = kzalloc((h->ports + 1u) * sizeof *h->tries);
    h->power = kzalloc((h->ports + 1u) * sizeof *h->power);
    h->held = kzalloc((h->ports + 1u) * sizeof *h->held);
    h->pending = kzalloc(pending_words(h) * sizeof *h->pending);
    if (!h->child || !h->tries || !h->power || !h->held || !h->pending) {
        kfree(h->child);
        kfree(h->tries);
        kfree(h->power);
        kfree(h->held);
        kfree(h->pending);
        kfree(h);
        return false;
    }
    if (ss) read_sublinks(h);
    d->hub = h; /* usbhub_detach frees it even if the hub never gets configured */
    usb_hub_info(d, h->ports, ss ? 0 : (desc[3] >> 5) & 3, false);
    return true;
}

/* The status change report: bit 0 the hub, bit p port p (USB 2.0 11.12.4). */
static void hub_report(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len, uint64_t at) {
    (void)at;
    struct hub *h = ctx;
    /* len 0 also after a recovery dropped its transfers: a port change in them is reported again */
    for (unsigned i = 0; i < len && i <= h->ports >> 3; i++) {
        uint32_t bits = data[i];
        if (!i && bits & 1) __atomic_store_n(&h->hub_change, true, __ATOMIC_RELEASE);
        if (!i) bits &= ~1u;
        if (bits) __atomic_fetch_or(&h->pending[i >> 2], bits << (8 * (i & 3)), __ATOMIC_RELEASE);
    }
    usb_note_work(d);
}

void usbhub_detach(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return;
    for (unsigned p = 1; p <= h->ports; p++)
        if (h->child[p]) usb_remove(h->child[p]);
    d->hub = NULL;
    kfree(h->sublinks);
    kfree(h->child);
    kfree(h->tries);
    kfree(h->power);
    kfree(h->held);
    kfree(h->pending);
    kfree(h);
}

/* Power on to power good (bPwrOn2PwrGood, USB 2.0 11.23.2.1), then every port looked at. */
static void power_ports(struct hub *h) {
    for (unsigned p = 1; p <= h->ports; p++) port_feature(h->dev, USB_REQ_SET_FEATURE, PORT_POWER, p);
    sleep_until(clock_after(h->power_ms * NS_PER_MS));
    for (unsigned p = 1; p <= h->ports; p++) pending_mark(h, p);
    usb_note_work(h->dev);
}

static bool hub_status_read(struct hub *h);

/* A SuperSpeed hub routes packets by its tier of the route string, which it learns here; until then
   it takes every packet as its own (USB 3.2 10.1.3.1). */
bool usbhub_set_depth(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h->ss || usb_control(d, 0x20, SET_HUB_DEPTH, d->depth, 0, NULL, 0) >= 0) return true;
    kprintf("usbhub: slot %u refused its hub depth\n", d->slot);
    return false;
}

/* The hub interface: its device was configured as a hub (usbhub_prepare). A second hub interface of
   the device would share its state, so the first to come takes it. */
static int hub_attach(struct hal_device *hal) {
    struct usb_interface *i = usb_interface_of(hal);
    struct usb_device *d = i->dev;
    struct hub *h = d->hub;
    const uint8_t *extra;
    unsigned extra_len;
    const struct usb_endpoint_desc *ep;
    if (!h || !usb_interface_interrupt(i, &extra, &extra_len, &ep)) return HAL_DECLINE;
    if (__atomic_exchange_n(&h->bound, true, __ATOMIC_ACQ_REL)) return HAL_DECLINE;
    h->report_ep = ep->address;
    if (hub_status_read(h)) h->hub_held = false; /* whether it has local power, and its ports can have any */
    if (!h->off) power_ports(h);
    kprintf("usbhub: slot %u %shub with %u ports\n", d->slot, h->ss ? "SuperSpeed " : "", h->ports);
    h->reporting = usb_interrupt_start(d, ep, (h->ports + 8) / 8, hub_report, h); /* a bit a port, and the hub's */
    if (!h->reporting) usbhub_reports_lost(d, ep->address);
    return 0;
}

/* The devices behind the hub go with it (usb_remove), each by its own hub's worker. */
static void hub_detach(struct hal_device *hal) { usbhub_detach(usb_interface_of(hal)->dev); }

/* A sleep removes every device and enumerates them again (the controller's resume): the hub has nothing
   of its own to bring back. */
static bool hub_resume(struct hal_device *hal) {
    (void)hal;
    return true;
}

static const struct n_hal_rule hub_rules[] = {N_USB_INTERFACE_CLASS(9)};
static const struct hal_driver hub_driver = {
    .name = "usbhub",
    .rules = hub_rules,
    .rule_count = ARRAY_SIZE(hub_rules),
    .attach = hub_attach,
    .detach = hub_detach,
    .resume = hub_resume,
};
HAL_DRIVER(hub_driver);

/* A change is known by the status change endpoint alone (USB 2.0 11.12.1): without it, the ports are asked. */
void usbhub_reports_lost(struct usb_device *d, uint8_t address) {
    struct hub *h = d->hub;
    if (!h || address != h->report_ep) return;
    kprintf("usbhub: slot %u reports no port changes: its ports are read each second\n", d->slot);
    h->reporting = false;
    hub_ask(h);
}

/* The hub's own status: the loss of the local power it had, or an over-current across all ports,
 * either of which switches every port off until software powers them again (USB 2.0 11.5.1.2,
 * 11.12.5; USB 3.2 10.3.1.1, 10.13.5). Read again after a change is acknowledged, as what ended between
 * the read and that is told no more (USB 2.0 Figure 11-21, USB 3.2 Figure 10-23). False if it does
 * not answer: what it acknowledged waits in hub_held for the next read. */
static bool hub_status_read(struct hub *h) {
    struct usb_device *d = h->dev;
    uint16_t status, change;
    for (int i = 0;; i++) {
        uint8_t st[4];
        if (usb_control(d, 0xA0, USB_REQ_GET_STATUS, 0, 0, st, 4) < 4) {
            if (h->hub_held) hub_ask(h);
            return false;
        }
        status = st[0] | st[1] << 8;
        change = st[2] | st[3] << 8;
        if (!(status & 1)) h->local = true;
        if (!(change & 3) || i == CHANGE_READS - 1) break;
        h->hub_held = true;
        if (change & 1) usb_control(d, 0x20, USB_REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, NULL, 0);
        if (change & 2) usb_control(d, 0x20, USB_REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, NULL, 0);
    }
    h->off = (status & 2) || (h->local && (status & 1));
    return true;
}

static bool hub_status(struct hub *h) {
    struct usb_device *d = h->dev;
    if (!hub_status_read(h)) return false;
    if (!h->hub_held) return true;
    h->hub_held = false;
    if (h->off) {
        kprintf("usbhub: slot %u lost its power or over-current, its ports are switched off\n", d->slot);
        for (unsigned p = 1; p <= h->ports; p++) pending_mark(h, p); /* their devices have gone */
    } else {
        kprintf("usbhub: slot %u power good again, powering its ports\n", d->slot);
        power_ports(h);
    }
    return true;
}

/* Reset a port (warm: the SuperSpeed link reset) and wait until the hub reports it done: the
   port's bit in the status change report (USB 2.0 11.12.4), then C_PORT_RESET (or C_BH_PORT_RESET)
   in its status. A hub whose report does not run is asked at the deadline. */
static bool port_reset(struct hub *h, unsigned port, bool warm, uint16_t *status) {
    struct usb_device *d = h->dev;
    uint16_t change = 0, done = warm ? CH_BH_RESET : CH_RESET;
    uint32_t bit = 1u << (port & 31), *word = &h->pending[port >> 5];
    port_feature(d, USB_REQ_SET_FEATURE, warm ? BH_PORT_RESET : PORT_RESET, port);
    for (uint64_t deadline = clock_after(500 * NS_PER_MS);;) {
        uint64_t seen = usb_kicks(d);
        bool told = __atomic_fetch_and(word, ~bit, __ATOMIC_ACQ_REL) & bit;
        bool late = clock_now_ns() >= deadline;
        if (told || late || !h->reporting || removed_hub(h)) {
            if (!port_status(d, port, status, &change)) return false;
            if ((change & done) || late || removed_hub(h)) break;
        }
        usb_wait_kick(d, seen, h->reporting ? deadline : MIN(deadline, clock_after(10 * NS_PER_MS)));
    }
    clear_changes(h, port, change & ~CH_OVER_CURRENT); /* that one is hub_port's, which comes next */
    return true;
}

bool usbhub_reset_port(struct usb_device *hub, unsigned port) {
    struct hub *h = hub->hub;
    uint16_t status = 0;
    if (!h || !port_reset(h, port, false, &status) || !(status & PS_ENABLE)) return false;
    sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
    return true;
}

/* A SuperSpeedPlus hub's port: the extended status's Rx Sublink Speed ID and Lane Count (USB 3.2 Table
   10-15), and that attribute's Link Protocol says which it is running, and its lane speed the rank
   (9.6.2.5, 7.5.4.5.1: Gen2 is 10 Gb/s a lane). */
static unsigned ss_speed(struct hub *h, unsigned port, unsigned *rank) {
    uint8_t st[8];
    *rank = 0;
    if (!h->sublinks || usb_control(h->dev, 0xA3, USB_REQ_GET_STATUS, PORT_STATUS_EXT, (uint16_t)port, st, 8) < 8)
        return USB_SPEED_SUPER;
    unsigned id = st[4] & 15, lanes = (st[5] & 15) + 1u;
    for (unsigned i = 0; i < h->nsublinks; i++) {
        uint32_t attribute = h->sublinks[i];
        if ((attribute & 15) != id || (attribute >> 14 & 3) != 1) continue;
        uint64_t rate = attribute >> 16;
        for (unsigned e = attribute >> 4 & 3; e; e--) rate *= 1000;
        *rank = (rate >= 10000000000ull ? 2u : 0u) + (lanes > 1 ? 1u : 0u);
        return USB_SPEED_SUPER_PLUS;
    }
    return USB_SPEED_SUPER;
}

/* A SuperSpeed link that needs a warm reset. During a reset the state may still be the Inactive it
   came from; a port without power has neither state (USB 3.2 10.3.1.1, 10.3.1.10, 10.3.1.11). */
static bool link_stuck(const struct hub *h, uint16_t status) {
    unsigned link = PS_LINK(status);
    return h->ss && !(status & PS_RESET) && (status & PS_SS_POWER) && (link == LINK_INACTIVE || link == LINK_COMPLIANCE);
}

/* The connection stable for TATTDB, which a disconnection begins again (USB 2.0 7.1.7.3). */
static bool connect_debounced(struct hub *h, unsigned port) {
    struct usb_device *d = h->dev;
    uint16_t status, change;
    for (int i = 0; i < USB_DEBOUNCES; i++) {
        sleep_until(clock_after(100 * NS_PER_MS));
        if (!port_status(d, port, &status, &change) || !(status & PS_CONNECTION)) return false;
        if (!(change & CH_CONNECTION)) return true;
        port_feature(d, USB_REQ_CLEAR_FEATURE, C_PORT_CONNECTION, port);
    }
    return false;
}

/* One try at a port with something at it and no device yet. */
static void hub_port_attach(struct hub *h, unsigned port) {
    struct usb_device *d = h->dev;
    uint16_t status, change;
    bool retry = h->tries[port].count > 1;
    if (!h->ss && !retry && !connect_debounced(h, port)) return;
    if (!port_status(d, port, &status, &change)) return;
    bool stuck = link_stuck(h, status);
    if (!(status & PS_CONNECTION) && !stuck) return;
    /* a trained SuperSpeed link is already enabled (USB 3.2 10.3.1.5); everything else, and a
     * retry, takes a reset: a warm one for a SuperSpeed link that would not train */
    if (!h->ss || stuck || retry || !(status & PS_ENABLE)) {
        if (!port_reset(h, port, h->ss && (stuck || retry), &status)) return;
    }
    if (!(status & PS_ENABLE)) {
        kprintf("usbhub: slot %u port %u did not enable\n", d->slot, port);
        return;
    }
    sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
    unsigned rank = 0;
    unsigned speed = h->ss ? ss_speed(h, port, &rank)
                     : (status & PS_LOW_SPEED) ? USB_SPEED_LOW
                     : (status & PS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    h->child[port] = usb_attach_child(d, port, speed, rank);
    /* a device left in its Default state answers address 0 with the next one reset behind this hub
       (USB 2.0 11.5.1.4, 9.1.2); a SuperSpeed port has no such request, and no such address */
    if (!h->child[port] && !h->ss) port_feature(d, USB_REQ_CLEAR_FEATURE, PORT_ENABLE, port);
}

static void hub_port(struct hub *h, unsigned port) {
    struct usb_device *d = h->dev;
    struct usb_port_tries *t = &h->tries[port], *power = &h->power[port];
    uint16_t status, change;
    if (!port_changes(h, port, &status, &change)) {
        /* a hub that does not answer: a port being tried is not read again at once */
        if (t->at && usb_port_try(t)) usb_port_failed(t, true);
        if (power->at && usb_port_try(power)) usb_port_failed(power, true);
        return;
    }
    bool connected = status & PS_CONNECTION;
    if (change & CH_CONNECTION) usb_port_free(t); /* what was being tried is not what comes now */
    /* unplugged, replugged between two looks, or disabled by an error (USB 2.0 11.24.2.7.2.2):
     * the device goes, and one still connected is enumerated afresh */
    if (h->child[port] && (!connected || (change & CH_CONNECTION) || !(status & PS_ENABLE))) {
        struct usb_device *gone = h->child[port];
        h->child[port] = NULL;
        usb_remove(gone);
    }
    if (status & PS_OVER_CURRENT) {
        if (change & CH_OVER_CURRENT) kprintf("usbhub: slot %u port %u over-current\n", d->slot, port);
        usb_port_free(t);
        if (power->at && clock_now_ns() >= power->at) power->at = clock_after(NS_PER_S); /* not due while it lasts */
        return;
    }
    if (status & (h->ss ? PS_SS_POWER : PS_USB2_POWER)) {
        if (power->at && clock_now_ns() >= power->at) usb_port_free(power); /* it has stayed on */
    } else { /* over: the port was switched off and needs power again (USB 2.0 11.12.5); a short takes it again */
        usb_port_free(t); /* what was tried has lost its power, and comes back as a new device (step 5) */
        if (h->off || !usb_port_try(power)) return;
        usb_port_failed(power, true);
        if (power->count == 1) kprintf("usbhub: slot %u port %u over-current ended, powering it again\n", d->slot, port);
        port_feature(d, USB_REQ_SET_FEATURE, PORT_POWER, port);
        sleep_until(clock_after(h->power_ms * NS_PER_MS));
        if (!port_changes(h, port, &status, &change)) return;
        connected = status & PS_CONNECTION;
    }
    bool stuck = link_stuck(h, status);
    if (!connected && !stuck) usb_port_free(t);
    if ((!connected && !stuck) || h->child[port] || removed_hub(h) || !usb_port_try(t)) return;
    hub_port_attach(h, port);
    if (h->child[port] || removed_hub(h)) {
        usb_port_free(t);
    } else if (port_status(d, port, &status, &change)) {
        connected = status & PS_CONNECTION;
        if (!connected && !link_stuck(h, status)) usb_port_free(t);
        else usb_port_failed(t, link_stuck(h, status));
    } else {
        usb_port_failed(t, true);
    }
}

/* A port being tried is tried again when its time comes. That read also finds a port with nothing
   at it any more: one that went into Inactive with no connection sets no change flag leaving it. */
static bool port_due(const struct hub *h, unsigned port) {
    uint64_t now = clock_now_ns();
    return (h->tries[port].at && now >= h->tries[port].at) || (!h->off && h->power[port].at && now >= h->power[port].at);
}

bool usbhub_pending(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return false;
    if (__atomic_load_n(&h->hub_change, __ATOMIC_ACQUIRE) || (h->asking && clock_now_ns() >= h->ask_at)) return true;
    for (unsigned w = 0; w < pending_words(h); w++)
        if (__atomic_load_n(&h->pending[w], __ATOMIC_ACQUIRE)) return true;
    for (unsigned p = 1; p <= h->ports; p++)
        if (port_due(h, p)) return true;
    return false;
}

bool usbhub_trying(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return false;
    for (unsigned p = 1; p <= h->ports; p++)
        if (usb_port_trying(&h->tries[p])) return true;
    return false;
}

void usbhub_work(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return;
    bool ask = h->asking && clock_now_ns() >= h->ask_at, answered = true;
    if ((__atomic_exchange_n(&h->hub_change, false, __ATOMIC_ACQ_REL) || ask) && !removed_hub(h)) answered = hub_status(h);
    if (ask) { /* each port waits for its control transfer's time: not for one that does not answer */
        h->ask_gap = answered ? 1 : MIN(h->ask_gap * 2, 64u);
        h->ask_at = clock_after(h->ask_gap * NS_PER_S);
    }
    for (unsigned p = 1; p <= h->ports; p++)
        if ((ask && answered) || port_due(h, p)) pending_mark(h, p);
    /* each port once: one that changes again waits for the worker's next round */
    for (unsigned w = 0; w < pending_words(h) && !removed_hub(h); w++) {
        uint32_t bits = __atomic_exchange_n(&h->pending[w], 0, __ATOMIC_ACQ_REL);
        while (bits && !removed_hub(h)) {
            unsigned port = w * 32 + (unsigned)__builtin_ctz(bits);
            bits &= bits - 1;
            if (port >= 1 && port <= h->ports) hub_port(h, port);
        }
    }
    /* its reports running, and nothing waiting for an answer any more: they tell it from here on */
    if (ask && answered && h->reporting && !h->hub_held) {
        bool held = false;
        for (unsigned p = 1; p <= h->ports; p++) held = held || h->held[p];
        h->asking = held;
    }
}
