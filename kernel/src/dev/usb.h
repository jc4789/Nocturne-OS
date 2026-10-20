#pragma once
/* USB devices as the host controller driver (xhci.c) hands them to the class drivers
 * (usbhid.c, usbhub.c, usbmsc.c, usbnet.c). Everything here runs on the single "usb" kernel
 * thread, except usb_run and usb_send, which other tasks use to reach it. */
#include <stdint.h>
#include <stdbool.h>

enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };

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
#define USB_STALL (-2)          /* a transfer the endpoint refused (it is halted until usb_clear_halt) */

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
    uint32_t route;
    uint8_t tt_slot, tt_port; /* transaction translator for full/low speed behind a high-speed hub */
    struct usb_device_desc desc;
    bool removed;
    void *hub; /* usbhub.c state when the device is a hub */
    void *msc; /* usbmsc.c state */
    void *net; /* usbnet.c state */
};

/* An endpoint as a class driver found it in the configuration descriptor. */
struct usb_ep_info {
    uint8_t address, attributes, interval, burst; /* burst: SuperSpeed companion */
    uint16_t max_packet;                         /* bits 12:11: extra high-speed transactions */
    uint16_t esit;                               /* SuperSpeed bytes per service interval, 0 if none */
};

typedef void (*usb_report_fn)(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len);

/* A control transfer; returns the bytes moved, or -1 (stall, error, timeout). */
int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length);
/* Keep the interrupt IN endpoint polled, handing each service interval's data to `fn`. The
 * endpoint must belong to an interface the class driver claimed. */
bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *ep, usb_report_fn fn, void *ctx);

/* The endpoint descriptor at cfg[off], with the SuperSpeed companion that may follow it. */
bool usb_endpoint_info(const uint8_t *cfg, unsigned total, unsigned off, struct usb_ep_info *ep);
/* Give endpoints device contexts. An alternate setting's endpoints are opened before
 * SET_INTERFACE selects it (xHCI 4.6.6). Endpoints already open are left as they are. */
bool usb_open_endpoints(struct usb_device *d, const struct usb_ep_info *eps, int n);
/* Keep `count` (at most 16) transfers of `len` bytes queued on a bulk or interrupt IN endpoint,
 * handing each one that finishes to `fn` with the bytes that arrived. */
bool usb_stream_start(struct usb_device *d, uint8_t address, unsigned len, unsigned count, usb_report_fn fn, void *ctx);
/* One bulk transfer of up to 64 KiB, waited for; usb thread only. Returns the bytes moved,
 * USB_STALL, or -1 (error or timeout: the endpoint then needs usb_clear_halt too). */
int usb_bulk(struct usb_device *d, uint8_t address, void *buf, unsigned len, unsigned timeout_ms);
/* Reset a pipe on both sides: the controller's endpoint state and data toggle, then
 * CLEAR_FEATURE(ENDPOINT_HALT) on the device. */
int usb_clear_halt(struct usb_device *d, uint8_t address);
/* A bulk OUT endpoint for sending from other tasks: `slots` copies of up to `size` bytes in
 * flight; `done` runs on the usb thread whenever one finishes. */
bool usb_send_open(struct usb_device *d, uint8_t address, unsigned size, unsigned slots, usb_report_fn done, void *ctx);
/* Queue `a` followed by `b` as one transfer (with the zero-length packet that ends a transfer
 * of whole packets). 1: queued, 0: every slot busy, -1: cannot send. Not from interrupts. */
int usb_send(struct usb_device *d, uint8_t address, const void *a, unsigned alen, const void *b, unsigned blen);
/* Run fn(arg) on the usb thread and wait for it: how other tasks reach a device. */
void usb_run(void (*fn)(void *arg), void *arg);
/* Wait (at most `ms`) until the devices present at boot are enumerated. */
void usb_wait_settled(unsigned ms);

/* hub support */
struct usb_device *usb_attach_child(struct usb_device *hub, unsigned port, unsigned speed);
void usb_remove(struct usb_device *d);
void usb_hub_info(struct usb_device *d, unsigned ports, unsigned think_time, bool multi_tt);
void usb_note_work(void); /* a report callback queued work: run the class work functions soon */

/* class drivers: does the interface belong to us (and which interrupt IN endpoint does it need)? */
bool usbhid_claims(const struct usb_interface_desc *intf);
void usbhid_attach(struct usb_device *d, const struct usb_interface_desc *intf, const uint8_t *extra,
                   unsigned extra_len, const struct usb_endpoint_desc *ep);
bool usbhub_prepare(struct usb_device *d); /* reads the hub descriptor and calls usb_hub_info */
void usbhub_attach(struct usb_device *d, const struct usb_endpoint_desc *ep);
void usbhub_work(void);                    /* port changes noticed by the hub interrupt endpoints */
bool usbhub_pending(void);                 /* port changes not handled yet */
uint64_t usbhid_tick(uint64_t now);        /* key repeat; returns the next deadline (0 = none) */
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
