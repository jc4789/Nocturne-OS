/* USB 2.0 hubs (USB 2.0 chapter 11): port power, connect, reset and the devices behind them.
 * Keyboards often carry a hub, and boards wire front panels and some rear ports through one. */
#include "kernel.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "sys/sched.h"

#define HUB_PORTS 15 /* a route string tier has four bits */
#define PORT_RESET 4
#define PORT_POWER 8
#define C_PORT_CONNECTION 16

struct hub {
    struct hub *next;
    struct usb_device *dev;
    unsigned ports, power_ms;
    struct usb_device *child[HUB_PORTS + 1];
    volatile uint32_t pending; /* ports whose status changed */
};

static struct hub *hubs;
static unsigned hubs_changed;

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

bool usbhub_prepare(struct usb_device *d) {
    uint8_t desc[16];
    if (usb_control(d, 0xA0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, desc, sizeof desc) < 7) {
        kprintf("usbhub: slot %u hub descriptor unreadable\n", d->slot);
        return false;
    }
    struct hub *h = kzalloc(sizeof *h);
    if (!h) return false;
    h->dev = d;
    h->ports = desc[2] > HUB_PORTS ? HUB_PORTS : desc[2];
    h->power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;
    d->hub = h; /* usbhub_detach frees it even if the hub never gets configured */
    usb_hub_info(d, desc[2], (desc[3] >> 5) & 3, false);
    return true;
}

static void hub_report(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len) {
    (void)d;
    struct hub *h = ctx;
    uint32_t bits = 0;
    for (unsigned i = 0; i < len && i < 4; i++) bits |= (uint32_t)data[i] << (8 * i);
    h->pending |= bits & ~1u; /* bit 0 is the hub itself */
    usb_note_work();
}

void usbhub_detach(struct usb_device *d) {
    struct hub *h = d->hub;
    if (!h) return;
    for (unsigned p = 1; p <= h->ports; p++)
        if (h->child[p]) usb_remove(h->child[p]);
    for (struct hub **pp = &hubs; *pp; pp = &(*pp)->next)
        if (*pp == h) {
            *pp = h->next;
            break;
        }
    d->hub = NULL;
    hubs_changed++;
    kfree(h);
}

void usbhub_attach(struct usb_device *d, const struct usb_endpoint_desc *ep) {
    struct hub *h = d->hub;
    if (!h) return;
    for (unsigned p = 1; p <= h->ports; p++) port_feature(d, USB_REQ_SET_FEATURE, PORT_POWER, p);
    sleep_ms(h->power_ms);
    for (unsigned p = 1; p <= h->ports; p++) h->pending |= 1u << p;
    h->next = hubs;
    hubs = h;
    hubs_changed++;
    kprintf("usbhub: slot %u hub with %u ports\n", d->slot, h->ports);
    usb_interrupt_start(d, ep, hub_report, h);
    usb_note_work();
}

static void hub_port(struct hub *h, unsigned port) {
    struct usb_device *d = h->dev;
    uint16_t status, change;
    if (!port_status(d, port, &status, &change)) return;
    for (unsigned b = 0; b < 5; b++)
        if (change & (1u << b)) port_feature(d, USB_REQ_CLEAR_FEATURE, C_PORT_CONNECTION + b, port);
    bool connected = status & 1;
    if (h->child[port] && (!connected || (change & 1))) {
        struct usb_device *gone = h->child[port];
        h->child[port] = NULL;
        usb_remove(gone);
    }
    if (!connected || h->child[port] || d->removed) return;
    sleep_ms(100); /* connect debounce */
    for (int attempt = 0; attempt < 3 && !h->child[port] && !d->removed; attempt++) {
        if (attempt) sleep_ms(100);
        if (!port_status(d, port, &status, &change) || !(status & 1)) return;
        port_feature(d, USB_REQ_SET_FEATURE, PORT_RESET, port);
        for (int t = 0; t < 50; t++) {
            sleep_ms(10);
            if (!port_status(d, port, &status, &change)) return;
            if (change & 0x10) break;
        }
        port_feature(d, USB_REQ_CLEAR_FEATURE, C_PORT_CONNECTION + 4, port);
        if (!(status & 2)) {
            kprintf("usbhub: slot %u port %u did not enable\n", d->slot, port);
            continue;
        }
        sleep_ms(10); /* reset recovery */
        unsigned speed = (status & 0x200) ? USB_SPEED_LOW : (status & 0x400) ? USB_SPEED_HIGH : USB_SPEED_FULL;
        h->child[port] = usb_attach_child(d, port, speed);
    }
}

void usbhub_work(void) {
again:
    for (struct hub *h = hubs; h; h = h->next) {
        while (h->pending && !h->dev->removed) {
            unsigned port = __builtin_ctz(h->pending);
            h->pending &= ~(1u << port);
            unsigned seen = hubs_changed;
            if (port >= 1 && port <= h->ports) hub_port(h, port);
            if (seen != hubs_changed) goto again; /* the list changed under us */
        }
    }
}
