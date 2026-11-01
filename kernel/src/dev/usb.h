#pragma once
/* USB devices as the host controller driver (xhci.c) hands them to the class drivers
 * (usbhid.c, usbhub.c, usbmsc.c, usbnet.c).
 *
 * Contexts. Each controller has a worker thread: it enumerates, so a class driver's attach,
 * detach and work functions run there, one device of that controller at a time, and may sleep.
 * A report function (usb_report_fn) runs in the drain of the event ring that took the completion:
 * an interrupt, or a polled controller's alarm, with interrupts off. It never sleeps, takes no
 * mutex and keeps short; whatever needs the worker it asks for with usb_note_work. usb_control,
 * usb_bulk and usb_clear_halt sleep until their transfer completes and run in any thread, on a
 * device that is not removed meanwhile (its class driver's detach waits for its users). */
#include <stdint.h>
#include <stdbool.h>
#include "sys/clock.h"

/* A port whose device does not come up, or that stays stuck, is tried one reset at a time, with the
   worker's other work in between, and less and less often: 1, 2, 4 ... 64 s apart. No document sets
   these times. One with a device connected is left after USB_PORT_TRIES, until its connection
   changes; a stuck one reads no connection, and may have no change to tell of a good device coming,
   so it goes on at the longest. The port's own, on its controller's worker. */
#define USB_PORT_TRIES 7
#define USB_DEBOUNCES 5 /* the connect debounce begun again for a connection that came and went */
struct usb_port_tries {
    uint64_t at;   /* when the next try may be; 0: none has failed; CLOCK_NEVER: left */
    uint8_t count; /* tries in a row, this one with them: how long the next waits */
    uint8_t failed; /* of them, those with a device connected */
    bool stuck;
};
/* Whether to try now; a try is counted if so. */
static inline bool usb_port_try(struct usb_port_tries *t) {
    if (clock_now_ns() < t->at) return false;
    if (t->count < USB_PORT_TRIES) t->count++;
    return true;
}
/* Nothing being tried is at the port any more, or what is there came up. */
static inline void usb_port_free(struct usb_port_tries *t) {
    t->at = 0;
    t->count = 0;
    t->failed = 0;
    t->stuck = false;
}
/* A try that failed. */
static inline void usb_port_failed(struct usb_port_tries *t, bool stuck) {
    if (t->stuck && !stuck) t->count = 1; /* a device has come: the waits begin again */
    t->stuck = stuck;
    if (!stuck && t->failed < USB_PORT_TRIES) t->failed++;
    t->at = t->failed >= USB_PORT_TRIES && !stuck ? CLOCK_NEVER : clock_after(NS_PER_S << (t->count ? t->count - 1 : 0));
}
/* A device is there, and another try is to come. */
static inline bool usb_port_trying(const struct usb_port_tries *t) {
    return t->at && t->at != CLOCK_NEVER && !t->stuck;
}

/* The kind of a device's speed. The numbers a Slot Context takes are the root port's own (xhci.c). */
enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4, USB_SPEED_SUPER_PLUS = 5 };

/* standard requests and descriptor types (USB 2.0 chapter 9) */
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09
#define USB_REQ_SET_INTERFACE  0x0B
#define USB_DESC_DEVICE    1
#define USB_DESC_CONFIG    2
#define USB_DESC_STRING    3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT  5
#define USB_DESC_COMPANION 0x30 /* SuperSpeed endpoint companion */
#define USB_DESC_SSP_COMPANION 0x31 /* SuperSpeedPlus isochronous endpoint companion */
#define USB_STALL (-2)          /* a transfer the endpoint refused (it is halted until usb_clear_halt) */
#define USB_TIMEOUT (-3)        /* the device did not answer in the time given */

struct usb_device_desc {
    uint8_t length, type;
    uint16_t usb_version;
    uint8_t cls, subcls, protocol, max_packet0;
    uint16_t vendor, product, release;
    uint8_t manufacturer, product_name, serial, configurations;
} __attribute__((packed));

struct usb_interface_desc {
    uint8_t length, type, number, alternate, endpoints, cls, subcls, protocol, name;
} __attribute__((packed));

struct usb_endpoint_desc {
    uint8_t length, type, address, attributes;
    uint16_t max_packet;
    uint8_t interval;
} __attribute__((packed));

struct usb_device {
    struct usb_hc *hc;
    struct usb_device *parent; /* hub, or NULL on a root port */
    uint8_t slot, speed, port, root_port, depth;
    uint8_t rank; /* of an Enhanced SuperSpeed link (USB 3.2 7.5.4.5.1): 0 Gen1x1, 1 Gen1x2, 2 Gen2x1, 3 Gen2x2 */
    uint32_t route;
    /* The hub and its port where the device's signaling changes: the transaction translator for full
       or low speed behind a high-speed hub, or the hub of a higher rank (xHCI Table 6-6). */
    uint8_t tt_slot, tt_port;
    struct usb_device_desc desc;
    bool removed; /* atomic: set once by usb_remove, after which no report function runs for it */
    void *hub; /* usbhub.c state when the device is a hub */
    void *hid; /* usbhid.c state: its HID interfaces */
    void *msc; /* usbmsc.c state */
    void *net; /* usbnet.c state */
};

/* An endpoint as a class driver found it in the configuration descriptor. */
struct usb_ep_info {
    uint8_t address, attributes, interval, burst; /* burst: SuperSpeed companion */
    uint8_t companion_attributes;                /* its bmAttributes: Mult of an isochronous endpoint */
    uint16_t max_packet;                         /* bits 12:11: extra high-speed transactions */
    uint32_t esit;                               /* SuperSpeed bytes per service interval, 0 if none */
};

/* A transfer that finished, in the drain that took it (see above): its bytes, and `at`, the
   moment on the monotonic clock (clock_now_ns) its completion was taken from the event ring. */
typedef void (*usb_report_fn)(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len, uint64_t at);

/* A control transfer; returns the bytes moved, or -1 (stall, error, timeout, removal). */
int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length);
/* Keep the interrupt IN endpoint polled, handing each transfer of up to `longest` bytes, in whole
 * packets, to `fn` as it comes; usb_interrupt_length says how many bytes that is. */
unsigned usb_interrupt_length(struct usb_device *d, const struct usb_endpoint_desc *ep, unsigned longest);
bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *ep, unsigned longest, usb_report_fn fn,
                         void *ctx);

/* The endpoint descriptor at cfg[off], with the SuperSpeed companion that may follow it. */
bool usb_endpoint_info(const uint8_t *cfg, unsigned total, unsigned off, struct usb_ep_info *ep);
/* Give an alternate setting's endpoints device contexts, before SET_INTERFACE selects it (xHCI
 * 4.6.6): those of alternate 0 came with the configuration. Endpoints already open are left as they are. */
bool usb_open_endpoints(struct usb_device *d, const struct usb_ep_info *eps, int n);
/* Keep `count` (at most 16) transfers of `len` bytes queued on a bulk or interrupt IN endpoint,
 * handing each one that finishes to `fn` with the bytes that arrived; and on the worker, before they
 * are queued anew, after a recovery dropped them all (data NULL, len 0). */
bool usb_stream_start(struct usb_device *d, uint8_t address, unsigned len, unsigned count, usb_report_fn fn, void *ctx);
/* One bulk transfer of up to 64 KiB, waited for. One caller at a time on an endpoint (the class
 * driver's lock). Returns 0, USB_STALL, USB_TIMEOUT, or -1 (error or removal); after any but 0 the
 * endpoint needs usb_clear_halt; *moved is the bytes that did move, received ones in buf, when the
 * transfer's completion came, and 0 at USB_TIMEOUT and when the device or its controller went. */
int usb_bulk(struct usb_device *d, uint8_t address, void *buf, unsigned len, unsigned timeout_ms, unsigned *moved);
/* Reset a pipe on both sides: the controller's endpoint state and data toggle, then
 * CLEAR_FEATURE(ENDPOINT_HALT) on the device. */
int usb_clear_halt(struct usb_device *d, uint8_t address);
/* A bulk OUT endpoint for sending from other tasks: `slots` copies of up to `size` bytes in
 * flight; `done` runs whenever one finishes (as a report function), and on the worker after a
 * recovery dropped them all (data NULL). */
bool usb_send_open(struct usb_device *d, uint8_t address, unsigned size, unsigned slots, usb_report_fn done, void *ctx);
/* Queue `a` followed by `b` as one transfer (with the zero-length packet that ends a transfer
 * of whole packets). 1: queued, 0: every slot busy, -1: cannot send. Any task on any CPU,
 * holding no spinlock: it takes the endpoint's lock. The caller keeps the device from being
 * removed meanwhile (its class driver's detach waits for it). */
int usb_send(struct usb_device *d, uint8_t address, const void *a, unsigned alen, const void *b, unsigned blen);
/* The devices there at start are still being looked at: their disks may yet appear. */
bool usb_enumerating(void);

/* hub support, on the worker */
struct usb_device *usb_attach_child(struct usb_device *hub, unsigned port, unsigned speed, unsigned rank);
void usb_remove(struct usb_device *d);
void usb_hub_info(struct usb_device *d, unsigned ports, unsigned think_time, bool multi_tt);
/* Any context: run the class work functions (usbhub_work, usbhid_work) for d on its worker soon. */
void usb_note_work(struct usb_device *d);
/* On the worker, for an event a report function notes (usb_note_work): read usb_kicks, look at
   the condition, then usb_wait_kick sleeps unless a note came after that read, or until the
   deadline. */
uint64_t usb_kicks(struct usb_device *d);
void usb_wait_kick(struct usb_device *d, uint64_t seen, uint64_t deadline);

/* class drivers: does the interface belong to us (and which interrupt IN endpoint does it need)? */
bool usbhid_claims(const struct usb_interface_desc *intf);
void usbhid_attach(struct usb_device *d, const struct usb_interface_desc *intf, const uint8_t *extra,
                   unsigned extra_len, const struct usb_endpoint_desc *ep);
void usbhid_work(struct usb_device *d);       /* the keyboard lights asked for */
bool usbhub_prepare(struct usb_device *d);    /* configured: reads the hub descriptor and calls usb_hub_info */
bool usbhub_set_depth(struct usb_device *d);  /* configured as a hub: tells it its tier */
bool usbhub_reset_port(struct usb_device *hub, unsigned port); /* reset the device on it; true once enabled */
void usbhub_attach(struct usb_device *d, const struct usb_endpoint_desc *ep);
void usbhub_work(struct usb_device *d);       /* port changes noticed by its interrupt endpoint */
bool usbhub_pending(struct usb_device *d);    /* port changes not handled yet, or a port's next try */
bool usbhub_trying(struct usb_device *d);     /* a device at a port is yet to get another try */
void usbhub_reports_lost(struct usb_device *d, uint8_t address); /* endpoint `address` was given up */
/* class drivers that read the whole configuration: does one of its interfaces suit them, and
 * take the device once it is configured */
bool usbmsc_claims(const struct usb_interface_desc *intf);
void usbmsc_attach(struct usb_device *d, const uint8_t *cfg, unsigned total);
bool usbnet_claims(const struct usb_interface_desc *intf);
void usbnet_attach(struct usb_device *d, const uint8_t *cfg, unsigned total);
/* a device is going away: each class driver drops whatever it holds of it (any interface) */
void usbhub_detach(struct usb_device *d);
void usbhid_detach(struct usb_device *d);
void usbmsc_detach(struct usb_device *d);
bool usbmsc_attached(void); /* a USB disk is plugged in */
void usbnet_detach(struct usb_device *d);

void usb_init(void);
