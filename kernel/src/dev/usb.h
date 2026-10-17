#pragma once
/* USB devices as the host controller driver (xhci.c) hands them to the class drivers
 * (usbhid.c, usbhub.c). Everything here runs on the single "usb" kernel thread. */
#include <stdint.h>
#include <stdbool.h>

enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };

/* standard requests and descriptor types (USB 2.0 chapter 9) */
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09
#define USB_DESC_DEVICE    1
#define USB_DESC_CONFIG    2
#define USB_DESC_STRING    3
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT  5

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
    /* the class driver bound to the device or one of its interfaces */
    void *class_data;
    void (*class_detach)(struct usb_device *d);
};

typedef void (*usb_report_fn)(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len);

/* A control transfer; returns the bytes moved, or -1 (stall, error, timeout). */
int usb_control(struct usb_device *d, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                void *buffer, uint16_t length);
/* Keep the interrupt IN endpoint polled, handing each completed packet to `fn`. The endpoint must
 * have been passed to the configuration (see usb_class_endpoint below). */
bool usb_interrupt_start(struct usb_device *d, const struct usb_endpoint_desc *ep, usb_report_fn fn, void *ctx);

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
uint64_t usbhid_tick(uint64_t now);        /* key repeat; returns the next deadline (0 = none) */

void usb_init(void);
