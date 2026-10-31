/* USB Ethernet adapters of the standard communications classes: CDC ECM (one frame per bulk
 * transfer) and CDC NCM (frames inside Network Transfer Blocks), per CDC 1.2, ECM 1.2 and
 * NCM 1.1. The protocol stack uses one adapter; the first one that arrives becomes it, also
 * after boot, and a replugged adapter takes its place again. */
#include "kernel.h"
#include "dev/usb.h"
#include "mm/heap.h"
#include "net/net.h"
#include "sys/wait.h"

#define CDC_ECM 0x06
#define CDC_NCM 0x0D
#define CS_INTERFACE 0x24
#define FD_UNION 0x06
#define FD_ETHERNET 0x0F
#define FD_NCM 0x1A
#define SET_ETHERNET_PACKET_FILTER 0x43
#define GET_NTB_PARAMETERS 0x80
#define SET_NTB_INPUT_SIZE 0x86
#define NOTIFY_NETWORK_CONNECTION 0x00
#define NOTIFY_SPEED_CHANGE 0x2A
#define NTH16_SIG 0x484D434Eu /* "NCMH" */
#define NDP16_SIG 0x304D434Eu /* "NCM0": no CRC */
#define NDP16_CRC 0x314D434Eu /* "NCM1": datagrams end in a CRC */
#define TX_SLOTS 16

struct usbnet {
    struct usb_device *dev; /* NULL once unplugged */
    bool ncm, link;
    uint8_t comm, data, alt; /* interface numbers and the data interface's working setting */
    uint8_t in, out;
    unsigned max_segment;
    /* NCM */
    uint32_t ntb_in, ntb_out;
    uint16_t out_divisor, out_remainder, out_align, seq;
    unsigned header; /* NTH16, padding, NDP16 and padding before the datagram */
};

/* The adapter the stack uses. Sends come from the stack on any CPU (under its lock, one at a
   time), arrival and removal from the usb thread: usbnet_lock (a mutex, as usb_send is a call
   that may take the host controller's locks) guards adapter and its dev, so a removed device is
   never sent to. Order: the stack's net_lock, then usbnet_lock, then the host controller's
   locks; net_register is called without usbnet_lock. */
static struct mutex usbnet_lock;
static struct usbnet *adapter;
static int usbnet_send(const void *frame, size_t len);
static struct netif usb_netif = {.send = usbnet_send};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

bool usbnet_claims(const struct usb_interface_desc *intf) {
    return intf->cls == 2 && (intf->subcls == CDC_ECM || intf->subcls == CDC_NCM) && intf->alternate == 0;
}

/* ---- data path ---- */

/* usbnet_lock held, n->dev there. */
static int usbnet_queue(struct usbnet *n, const void *frame, size_t len) {
    int r;
    if (!n->ncm) {
        r = usb_send(n->dev, n->out, frame, (unsigned)len, NULL, 0);
    } else {
        /* one datagram per NTB (NCM 3): NTH16, NDP16 with its null entry, the datagram */
        uint8_t h[1024]; /* ncm_setup keeps the header within this */
        memset(h, 0, n->header);
        unsigned ndp = (12 + n->out_align - 1) & ~(n->out_align - 1u);
        put32(h, NTH16_SIG);
        put16(h + 4, 12);
        put16(h + 6, n->seq++);
        put16(h + 8, (uint16_t)(n->header + len));
        put16(h + 10, (uint16_t)ndp);
        put32(h + ndp, NDP16_SIG);
        put16(h + ndp + 4, 16);
        put16(h + ndp + 8, (uint16_t)n->header);
        put16(h + ndp + 10, (uint16_t)len);
        r = usb_send(n->dev, n->out, h, n->header, frame, (unsigned)len);
    }
    return r > 0 ? 0 : r == 0 ? -N_ERR_NOT_READY : -N_ERR_IO;
}

static int usbnet_send(const void *frame, size_t len) {
    mutex_lock(&usbnet_lock);
    struct usbnet *n = adapter;
    int r;
    if (!n || !n->dev) r = -N_ERR_NETWORK_DOWN;
    else if (len > n->max_segment) r = -N_ERR_INVALID;
    else r = usbnet_queue(n, frame, len);
    mutex_unlock(&usbnet_lock);
    return r;
}

static void tx_done(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len) {
    (void)d, (void)ctx, (void)data, (void)len;
    net_rx_notify(); /* a sender held back by full slots may go on */
}

/* NCM 3.2-3.3: every offset and length in a received NTB is checked against what arrived. */
static void ncm_input(struct usbnet *n, const uint8_t *b, unsigned len) {
    if (len < 12 || get32(b) != NTH16_SIG || get16(b + 4) < 12) return;
    unsigned header = get16(b + 4), limit = get16(b + 8) ? get16(b + 8) : len;
    if (limit > len) return;
    unsigned ndp = get16(b + 10);
    for (int guard = 0; ndp && guard < 8; guard++) {
        if ((ndp & 3) || ndp < header || ndp + 16 > limit) return;
        uint32_t sig = get32(b + ndp);
        unsigned ndp_len = get16(b + ndp + 4);
        if ((sig != NDP16_SIG && sig != NDP16_CRC) || ndp_len < 16 || (ndp_len & 3) || ndp + ndp_len > limit) return;
        unsigned crc = sig == NDP16_CRC ? 4 : 0;
        for (unsigned e = ndp + 8; e + 4 <= ndp + ndp_len; e += 4) {
            unsigned at = get16(b + e), size = get16(b + e + 2);
            if (!at || !size) break; /* the first null entry ends the list (3.7) */
            if (at < header || at + size > limit || size < 14 + crc) continue;
            if (size - crc <= n->max_segment) net_rx(b + at, size - crc);
        }
        unsigned next = get16(b + ndp + 6); /* reserved in NCM 1.0, a link in Figure 3-1 */
        if (next <= ndp) return;
        ndp = next;
    }
}

static void rx(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len) {
    (void)d;
    struct usbnet *n = ctx;
    if (!len) return; /* the zero-length packet after a frame of whole packets */
    if (n->ncm) ncm_input(n, data, len);
    else if (len >= 14) net_rx(data, len); /* ECM 3.4: no CRC */
}

static void notify(struct usb_device *d, void *ctx, const uint8_t *data, unsigned len) {
    (void)d;
    struct usbnet *n = ctx;
    if (len < 8 || data[0] != 0xA1) return; /* the start of a notification (CDC 6.3) */
    if (data[1] == NOTIFY_NETWORK_CONNECTION) {
        bool up = get16(data + 2) != 0;
        if (up != n->link) kprintf("usbnet: link %s\n", up ? "up" : "down");
        n->link = up;
    } else if (data[1] == NOTIFY_SPEED_CHANGE && len >= 16) {
        kprintf("usbnet: %u Mbit/s\n", get32(data + 8) / 1000000);
    }
}

/* ---- setup ---- */

/* iMACAddress: 12 hexadecimal digits, the first one the high nibble of the first byte (ECM 5.4). */
static bool read_mac(struct usb_device *d, unsigned index, uint8_t mac[6]) {
    uint8_t s[64], langs[4];
    uint16_t lang = 0x0409;
    if (usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_STRING << 8, 0, langs, 4) >= 4) lang = get16(langs + 2);
    int got = usb_control(d, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DESC_STRING << 8 | index), lang, s, sizeof s);
    if (got < 26 || s[0] < 26) return false;
    for (int i = 0; i < 12; i++) {
        unsigned c = s[2 + 2 * i], v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        if (i & 1) mac[i / 2] |= (uint8_t)v;
        else mac[i / 2] = (uint8_t)(v << 4);
    }
    return !(mac[0] & 1);
}

/* NCM 7.2.1 and 9.2: learn the NTB limits and alignment, and set the IN NTB size we buffer. */
static bool ncm_setup(struct usbnet *n) {
    struct usb_device *d = n->dev;
    uint8_t p[28];
    if (usb_control(d, 0xA1, GET_NTB_PARAMETERS, 0, n->comm, p, sizeof p) < 28 || get16(p) < 28 || !(get16(p + 2) & 1)) {
        kprintf("usbnet: slot %u NTB parameters unreadable\n", d->slot);
        return false;
    }
    uint32_t in_max = get32(p + 4);
    n->ntb_out = get32(p + 16);
    n->out_divisor = get16(p + 20) ? get16(p + 20) : 1;
    n->out_remainder = (uint16_t)(get16(p + 22) % n->out_divisor);
    n->out_align = get16(p + 24);
    if (n->out_align < 4 || (n->out_align & (n->out_align - 1))) n->out_align = 4;
    /* 3.3.4 asks "Offset % wNdpOutDivisor == wNdpOutPayloadRemainder" of each datagram */
    unsigned ndp = (12 + n->out_align - 1) & ~(n->out_align - 1u), at = ndp + 16;
    at += (n->out_remainder + n->out_divisor - at % n->out_divisor) % n->out_divisor;
    n->header = at;
    if (n->header + n->max_segment > n->ntb_out || n->header > 1024) {
        kprintf("usbnet: slot %u OUT NTBs too small (%u bytes)\n", d->slot, n->ntb_out);
        return false;
    }
    n->ntb_in = in_max > 16384 ? 16384 : in_max;
    if (n->ntb_in < 2048) n->ntb_in = 2048;
    uint8_t size[4];
    put32(size, n->ntb_in);
    if (usb_control(d, 0x21, SET_NTB_INPUT_SIZE, 0, n->comm, size, 4) < 0) {
        n->ntb_in = in_max > 65536 ? 65536 : in_max; /* the device keeps its own maximum */
        if (n->ntb_in < 2048) return false;
    }
    return true;
}

void usbnet_attach(struct usb_device *d, const uint8_t *cfg, unsigned total) {
    const struct usb_interface_desc *comm = NULL;
    struct usb_ep_info eps[3], e; /* notification, bulk IN, bulk OUT */
    bool have_notify = false, have_in = false, have_out = false, found_data = false;
    int data_number = -1;
    unsigned mac_index = 0, max_segment = 1514, alt = 0;
    enum { OTHER, COMM, DATA } in = OTHER; /* whose descriptors these are */
    for (unsigned off = 0; off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total && !found_data;
         off += cfg[off]) {
        const uint8_t *x = cfg + off;
        if (x[1] == USB_DESC_INTERFACE && x[0] >= 9) {
            const struct usb_interface_desc *i = (const void *)x;
            in = OTHER;
            if (!comm) {
                if (usbnet_claims(i)) comm = i, in = COMM;
            } else if (i->number != comm->number) {
                if (data_number < 0) data_number = comm->number + 1; /* no Union: the next interface */
                /* the data interface: alternate 0 has no endpoints, the working one a bulk pair */
                if (i->cls == 0x0A && i->number == data_number && i->alternate) {
                    in = DATA;
                    alt = i->alternate;
                    have_in = have_out = false;
                }
            }
        } else if (in == COMM && x[1] == CS_INTERFACE && x[0] >= 3) {
            if (x[2] == FD_UNION && x[0] >= 5) data_number = x[4];
            if (x[2] == FD_ETHERNET && x[0] >= 13) {
                mac_index = x[3];
                max_segment = get16(x + 8);
            }
        } else if (in != OTHER && usb_endpoint_info(cfg, total, off, &e)) {
            if (in == COMM && !have_notify && (e.attributes & 3) == 3 && (e.address & 0x80)) {
                eps[0] = e;
                have_notify = true;
            } else if (in == DATA && (e.attributes & 3) == 2) {
                if ((e.address & 0x80) && !have_in) eps[1] = e, have_in = true;
                if (!(e.address & 0x80) && !have_out) eps[2] = e, have_out = true;
                found_data = have_in && have_out;
            }
        }
    }
    if (!comm) return;
    if (!found_data || !mac_index) {
        kprintf("usbnet: slot %u CDC function incomplete (data interface, MAC string)\n", d->slot);
        return;
    }
    mutex_lock(&usbnet_lock);
    bool busy = adapter && adapter->dev;
    mutex_unlock(&usbnet_lock);
    if (busy) {
        kprintf("usbnet: slot %u ignored, another USB adapter is in use\n", d->slot);
        return;
    }
    struct usbnet *n = kzalloc(sizeof *n);
    if (!n) return;
    n->dev = d;
    n->ncm = comm->subcls == CDC_NCM;
    n->comm = comm->number;
    n->data = (uint8_t)data_number;
    n->alt = (uint8_t)alt;
    n->in = eps[1].address;
    n->out = eps[2].address;
    n->max_segment = max_segment < 1514 ? 1514 : max_segment > 9014 ? 9014 : max_segment;
    uint8_t mac[6];
    if (!read_mac(d, mac_index, mac)) {
        kprintf("usbnet: slot %u MAC address string unreadable\n", d->slot);
        goto fail;
    }
    /* endpoints of the working alternate get contexts before SET_INTERFACE selects it; alternate
     * 0 first resets the function's network state (ECM 3.3, NCM 9.2), then the parameters,
     * which alternate 0 would reset again, then the working alternate */
    if (!usb_open_endpoints(d, have_notify ? eps : eps + 1, have_notify ? 3 : 2)) goto fail;
    usb_control(d, 0x01, USB_REQ_SET_INTERFACE, 0, n->data, NULL, 0);
    if (n->ncm && !ncm_setup(n)) goto fail;
    /* directed, broadcast, all multicast (ECM 6.2.4); NCM may lack the request (6.2.1 D0) */
    usb_control(d, 0x21, SET_ETHERNET_PACKET_FILTER, 0x000E, n->comm, NULL, 0);
    if (usb_control(d, 0x01, USB_REQ_SET_INTERFACE, n->alt, n->data, NULL, 0) < 0) {
        kprintf("usbnet: slot %u data interface refused alternate %u\n", d->slot, n->alt);
        goto fail;
    }
    unsigned mps = eps[1].max_packet & 0x7FF;
    if (!mps) mps = 512;
    /* an IN transfer ends at a short packet, so its buffer is whole packets */
    unsigned rx_len = n->ncm ? n->ntb_in : (n->max_segment + mps - 1) / mps * mps;
    unsigned tx_len = n->ncm ? n->header + n->max_segment : n->max_segment;
    d->net = n;
    if (!usb_send_open(d, n->out, tx_len, TX_SLOTS, tx_done, n) ||
        !usb_stream_start(d, n->in, rx_len, n->ncm ? 4 : 16, rx, n)) {
        kprintf("usbnet: slot %u transfers could not start\n", d->slot);
        d->net = NULL;
        goto fail;
    }
    if (have_notify) {
        unsigned nlen = eps[0].max_packet & 0x7FF;
        usb_stream_start(d, eps[0].address, nlen < 8 ? 8 : nlen > 64 ? 64 : nlen, 2, notify, n);
    } else {
        n->link = true;
    }
    kprintf("usbnet: slot %u CDC %s adapter %02x:%02x:%02x:%02x:%02x:%02x\n", d->slot, n->ncm ? "NCM" : "ECM", mac[0],
            mac[1], mac[2], mac[3], mac[4], mac[5]);
    mutex_lock(&usbnet_lock);
    struct usbnet *old = adapter;
    adapter = n;
    /* only net_register reads these, after this */
    memcpy(usb_netif.mac, mac, 6);
    usb_netif.driver = n->ncm ? "usb-ncm" : "usb-ecm";
    mutex_unlock(&usbnet_lock);
    kfree(old);
    /* the first adapter of the system, or this one again with another address */
    net_register(&usb_netif);
    return;
fail:
    kfree(n);
}

void usbnet_detach(struct usb_device *d) {
    struct usbnet *n = d->net;
    if (!n) return;
    d->net = NULL;
    mutex_lock(&usbnet_lock);
    n->dev = NULL; /* sends fail until an adapter comes back; none is inside usb_send now */
    mutex_unlock(&usbnet_lock);
    kprintf("usbnet: adapter unplugged\n");
}
