/* USB hubs: USB 2.0 hubs (USB 2.0 chapter 11) and Enhanced SuperSpeed hubs (USB 3.2 chapter 10):
 * port power, connect, reset and the devices behind them. Keyboards often carry a hub, and boards
 * wire front panels and some rear ports through one. A USB 3 hub is two hubs in one package; its
 * SuperSpeed half shows up on a USB 3 root port and its USB 2.0 half on a USB 2 one. Everything
 * here runs on the hub's controller worker but hub_report, which runs in the drain (dev/usb.h). */
#include "kernel.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define HUB_PORTS 15 /* a route string tier has four bits */

/* feature selectors (USB 2.0 Table 11-17, USB 3.2 Table 10-9) */
#define C_HUB_LOCAL_POWER 0
#define C_HUB_OVER_CURRENT 1
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

/* wPortStatus / wPortChange (USB 2.0 Tables 11-21, 11-22; USB 3.2 Tables 10-13, 10-14) */
#define PS_CONNECTION (1u << 0)
#define PS_ENABLE (1u << 1)
#define PS_OVER_CURRENT (1u << 3)
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
    bool reporting;  /* its status change endpoint runs: a port change comes as a report */
    struct usb_device *child[HUB_PORTS + 1];
    uint32_t pending; /* atomic: ports whose status changed */
};

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
    h->ports = desc[2] > HUB_PORTS ? HUB_PORTS : desc[2];
    h->power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;
    d->hub = h; /* usbhub_detach frees it even if the hub never gets configured */
    usb_hub_info(d, desc[2], ss ? 0 : (desc[3] >> 5) & 3, false);
    return true;
}

static void hub_report(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len, uint64_t at) {
    (void)at;
    struct hub *h = ctx;
    uint32_t bits = 0;
    for (unsigned i = 0; i < len && i < 4; i++) bits |= (uint32_t)data[i] << (8 * i);
    __atomic_fetch_or(&h->pending, bits & ~1u, __ATOMIC_RELEASE);
    if (bits & 1) __atomic_store_n(&h->hub_change, true, __ATOMIC_RELEASE); /* bit 0 is the hub itself */
    usb_note_work(d);
}

void usbhub_detach(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return;
    for (unsigned p = 1; p <= h->ports; p++)
        if (h->child[p]) usb_remove(h->child[p]);
    d->hub = NULL;
    kfree(h);
}

/* Power on to power good (bPwrOn2PwrGood, USB 2.0 11.23.2.1), then every port looked at. */
static void power_ports(struct hub *h) {
    for (unsigned p = 1; p <= h->ports; p++) port_feature(h->dev, USB_REQ_SET_FEATURE, PORT_POWER, p);
    sleep_until(clock_after(h->power_ms * NS_PER_MS));
    uint32_t all = 0;
    for (unsigned p = 1; p <= h->ports; p++) all |= 1u << p;
    __atomic_fetch_or(&h->pending, all, __ATOMIC_RELEASE);
    usb_note_work(h->dev);
}

void usbhub_attach(struct usb_device *d, const struct usb_endpoint_desc *ep) {
    struct hub *h = d->hub;
    if (!h) return;
    /* a SuperSpeed hub routes packets by its tier of the route string, which it learns here;
     * until then it takes every packet as its own (USB 3.2 10.1.3.1) */
    if (h->ss && usb_control(d, 0x20, SET_HUB_DEPTH, d->depth, 0, NULL, 0) < 0) {
        kprintf("usbhub: slot %u refused its hub depth\n", d->slot);
        return;
    }
    power_ports(h);
    kprintf("usbhub: slot %u %shub with %u ports\n", d->slot, h->ss ? "SuperSpeed " : "", h->ports);
    h->reporting = usb_interrupt_start(d, ep, hub_report, h);
}

/* The hub's own status: its local power supply, or an over-current across all ports, which
 * switches every port off until software powers them again. */
static void hub_status(struct hub *h) {
    struct usb_device *d = h->dev;
    uint8_t st[4];
    if (usb_control(d, 0xA0, USB_REQ_GET_STATUS, 0, 0, st, 4) < 4) return;
    uint16_t status = st[0] | st[1] << 8, change = st[2] | st[3] << 8;
    if (change & 1) usb_control(d, 0x20, USB_REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, NULL, 0);
    if (!(change & 2)) return;
    usb_control(d, 0x20, USB_REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, NULL, 0);
    if (status & 2) {
        kprintf("usbhub: slot %u over-current, its ports are switched off\n", d->slot);
    } else {
        kprintf("usbhub: slot %u over-current ended, powering its ports again\n", d->slot);
        power_ports(h);
    }
}

/* Reset a port (warm: the SuperSpeed link reset) and wait until the hub reports it done: the
   port's bit in the status change report (USB 2.0 11.12.4), then C_PORT_RESET (or C_BH_PORT_RESET)
   in its status. A hub whose report does not run is asked at the deadline. */
static bool port_reset(struct hub *h, unsigned port, bool warm, uint16_t *status) {
    struct usb_device *d = h->dev;
    uint16_t change = 0, done = warm ? CH_BH_RESET : CH_RESET;
    uint32_t bit = 1u << port;
    port_feature(d, USB_REQ_SET_FEATURE, warm ? BH_PORT_RESET : PORT_RESET, port);
    for (uint64_t deadline = clock_after(500 * NS_PER_MS);;) {
        uint64_t seen = usb_kicks(d);
        bool told = __atomic_fetch_and(&h->pending, ~bit, __ATOMIC_ACQ_REL) & bit;
        bool late = clock_now_ns() >= deadline;
        if (told || late || !h->reporting || removed_hub(h)) {
            if (!port_status(d, port, status, &change)) return false;
            if ((change & done) || late || removed_hub(h)) break;
        }
        usb_wait_kick(d, seen, h->reporting ? deadline : MIN(deadline, clock_after(10 * NS_PER_MS)));
    }
    clear_changes(h, port, change);
    return true;
}

static void hub_port(struct hub *h, unsigned port) {
    struct usb_device *d = h->dev;
    uint16_t status, change;
    if (!port_status(d, port, &status, &change)) return;
    clear_changes(h, port, change);
    bool connected = status & PS_CONNECTION;
    /* unplugged, replugged between two looks, or disabled by an error (USB 2.0 11.24.2.7.2.2):
     * the device goes, and one still connected is enumerated afresh */
    if (h->child[port] && (!connected || (change & CH_CONNECTION) || !(status & PS_ENABLE))) {
        struct usb_device *gone = h->child[port];
        h->child[port] = NULL;
        usb_remove(gone);
    }
    if (status & PS_OVER_CURRENT) {
        if (change & CH_OVER_CURRENT) kprintf("usbhub: slot %u port %u over-current\n", d->slot, port);
        return;
    }
    if (change & CH_OVER_CURRENT) { /* over: the port was switched off and needs power again */
        kprintf("usbhub: slot %u port %u over-current ended, powering it again\n", d->slot, port);
        port_feature(d, USB_REQ_SET_FEATURE, PORT_POWER, port);
        sleep_until(clock_after(h->power_ms * NS_PER_MS));
        if (!port_status(d, port, &status, &change)) return;
        clear_changes(h, port, change);
        connected = status & PS_CONNECTION;
    }
    unsigned link = PS_LINK(status);
    bool stuck = h->ss && (link == LINK_INACTIVE || link == LINK_COMPLIANCE); /* needs a warm reset */
    if ((!connected && !stuck) || h->child[port] || removed_hub(h)) return;
    /* connect debounce; SuperSpeed links train first, then connect */
    if (!h->ss) sleep_until(clock_after(100 * NS_PER_MS));
    for (int attempt = 0; attempt < 3 && !h->child[port] && !removed_hub(h); attempt++) {
        if (attempt) sleep_until(clock_after(100 * NS_PER_MS));
        if (!port_status(d, port, &status, &change)) return;
        link = PS_LINK(status);
        stuck = h->ss && (link == LINK_INACTIVE || link == LINK_COMPLIANCE);
        if (!(status & PS_CONNECTION) && !stuck) return;
        /* a trained SuperSpeed link is already enabled (USB 3.2 10.3.1.5); everything else, and a
         * retry, takes a reset: a warm one for a SuperSpeed link that would not train */
        if (!h->ss || stuck || attempt || !(status & PS_ENABLE)) {
            if (!port_reset(h, port, h->ss && (stuck || attempt), &status)) return;
        }
        if (!(status & PS_ENABLE)) {
            kprintf("usbhub: slot %u port %u did not enable\n", d->slot, port);
            continue;
        }
        sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
        unsigned speed = h->ss ? USB_SPEED_SUPER
                         : (status & PS_LOW_SPEED) ? USB_SPEED_LOW
                         : (status & PS_HIGH_SPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
        h->child[port] = usb_attach_child(d, port, speed);
    }
}

bool usbhub_pending(struct usb_device *d) {
    struct hub *h = d->hub;
    return h && (__atomic_load_n(&h->pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&h->hub_change, __ATOMIC_ACQUIRE));
}

void usbhub_work(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return;
    if (__atomic_exchange_n(&h->hub_change, false, __ATOMIC_ACQ_REL) && !removed_hub(h)) hub_status(h);
    for (uint32_t bits; !removed_hub(h) && (bits = __atomic_exchange_n(&h->pending, 0, __ATOMIC_ACQ_REL));)
        while (bits && !removed_hub(h)) {
            unsigned port = __builtin_ctz(bits);
            bits &= bits - 1;
            if (port >= 1 && port <= h->ports) hub_port(h, port);
        }
}
