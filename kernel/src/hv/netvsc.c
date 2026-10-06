/* Hyper-V synthetic network adapter (netvsc). Two protocols stacked on a VMBus channel: NVSP sets
   up two big shared buffers (receive and send), and RNDIS, the USB-style remote NDIS protocol,
   runs over them. Every frame and every RNDIS control message travels as an RNDIS message: the
   host writes received ones into the receive buffer and tells us where (a transfer-page packet,
   which we must complete so it can reuse the space); we copy ones we send into a section of the
   send buffer and tell the host which section (it completes that packet when done with it). */
#include "kernel.h"
#include "arch/cpu.h"
#include "hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "net/net.h"

static const struct guid net_guid =
    GUID_INIT(0xf8615163, 0xdf3e, 0x46c5, 0x91, 0x3f, 0xf2, 0xd2, 0xf9, 0x65, 0xed, 0x0e);

enum {
    NVSP_INIT = 1,
    NVSP_INIT_COMPLETE = 2,
    NVSP_SEND_NDIS_VERSION = 100,
    NVSP_SEND_RECV_BUF = 101,
    NVSP_SEND_RECV_BUF_COMPLETE = 102,
    NVSP_SEND_SEND_BUF = 104,
    NVSP_SEND_SEND_BUF_COMPLETE = 105,
    NVSP_SEND_RNDIS_PKT = 107,
    NVSP_SEND_RNDIS_PKT_COMPLETE = 108,
    NVSP_SEND_NDIS_CONFIG = 125,
};
#define NVSP_STATUS_SUCCESS 1
#define NVSP_MSG_SIZE 40 /* the host wants messages at least as big as its own union */
#define RECV_BUF_ID 0xCAFE
#define SEND_BUF_ID 0
#define INVALID_SECTION 0xFFFFFFFFu

#define RECV_BUF_PAGES 256 /* 1 MiB */
#define SEND_BUF_PAGES 256
#define TX_TID_BASE 0x5E000000ull /* trans_id of a data send = base + its section */

enum {
    RNDIS_PACKET = 1,
    RNDIS_INIT = 2,
    RNDIS_QUERY = 4,
    RNDIS_SET = 5,
    RNDIS_INDICATE = 7,
    RNDIS_KEEPALIVE = 8,
    RNDIS_COMPLETION = 0x80000000u,
};
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define OID_GEN_MEDIA_CONNECT_STATUS  0x00010114
#define OID_802_3_PERMANENT_ADDRESS   0x01010101
#define PACKET_FILTER (1 | 4 | 8) /* directed, all multicast, broadcast */
#define RNDIS_STATUS_MEDIA_CONNECT    0x4001000B
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000C

struct rndis_hdr {
    uint32_t type, len;
} PACKED;

/* a data packet: offsets count from the byte after rndis_hdr */
struct rndis_packet {
    struct rndis_hdr h;
    uint32_t data_offset, data_len;
    uint32_t oob_offset, oob_len, oob_count;
    uint32_t ppi_offset, ppi_len;
    uint32_t vc_handle, reserved;
} PACKED;

/* QUERY and SET requests share a layout; the buffer offset counts from req_id */
struct rndis_oid_req {
    struct rndis_hdr h;
    uint32_t req_id, oid, buflen, bufoff, vc_handle;
} PACKED;

struct xfer_range {
    uint32_t len, offset;
} PACKED;

struct xfer_page_header {
    struct vmpacket_desc d;
    uint16_t pageset_id;
    uint8_t sender_owns, reserved;
    uint32_t range_count;
    struct xfer_range range[];
} PACKED;

static struct {
    struct vmbus_channel *ch;
    uint32_t nvsp_version;
    uint8_t *recv_buf, *send_buf;
    uint32_t recv_size;
    uint32_t section_size, sections;
    uint64_t *section_used; /* bitmap */
    /* waiting for one NVSP completion during setup */
    volatile bool nvsp_done;
    uint32_t nvsp_wait_type;
    uint8_t nvsp_reply[64];
    /* waiting for one RNDIS completion */
    volatile bool rndis_done;
    uint32_t rndis_wait_id, next_req_id;
    uint8_t rndis_reply[128];
    bool link_up;
    uint64_t tx_drops;
} nv;

static struct netif nif = {.driver = "netvsc"};

/* ---- send-buffer sections ---- */

static uint32_t section_alloc(void) {
    uint64_t f = irq_save();
    for (uint32_t i = 0; i < nv.sections; i++) {
        if (!(nv.section_used[i / 64] & (1ull << (i % 64)))) {
            nv.section_used[i / 64] |= 1ull << (i % 64);
            irq_restore(f);
            return i;
        }
    }
    irq_restore(f);
    return INVALID_SECTION;
}

static void section_free(uint32_t i) {
    if (i >= nv.sections) return;
    uint64_t f = irq_save();
    nv.section_used[i / 64] &= ~(1ull << (i % 64));
    irq_restore(f);
}

/* Hand the RNDIS message in section `sec` to the host. */
static int send_section(uint32_t sec, uint32_t len, bool control) {
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT, control ? 1 : 0, sec, len};
    return vmbus_send(nv.ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, TX_TID_BASE + sec);
}

/* Copy an RNDIS message into a free section and send it. */
static int send_rndis(const void *msg, uint32_t len, bool control) {
    if (len > nv.section_size) return -EINVAL;
    uint32_t sec = section_alloc();
    if (sec == INVALID_SECTION) return -EAGAIN;
    memcpy(nv.send_buf + (size_t)sec * nv.section_size, msg, len);
    int r = send_section(sec, len, control);
    if (r) section_free(sec);
    return r;
}

/* ---- receive path (interrupt context) ---- */

static void rndis_receive(const uint8_t *p, uint32_t len) {
    if (len < sizeof(struct rndis_hdr)) return;
    const struct rndis_hdr *h = (const void *)p;
    if (h->len > len) return;
    switch (h->type) {
    case RNDIS_PACKET: {
        const struct rndis_packet *rp = (const void *)p;
        if (h->len < sizeof *rp) return;
        uint64_t end = 8ull + rp->data_offset + rp->data_len;
        if (end > h->len) return;
        net_rx(p + 8 + rp->data_offset, rp->data_len);
        break;
    }
    case RNDIS_INDICATE: {
        uint32_t status = len >= 12 ? ((const uint32_t *)p)[2] : 0;
        if (status == RNDIS_STATUS_MEDIA_CONNECT) nv.link_up = true;
        if (status == RNDIS_STATUS_MEDIA_DISCONNECT) nv.link_up = false;
        if (status == RNDIS_STATUS_MEDIA_CONNECT || status == RNDIS_STATUS_MEDIA_DISCONNECT)
            kprintf("netvsc: link %s\n", nv.link_up ? "up" : "down");
        break;
    }
    case RNDIS_KEEPALIVE: {
        if (len < 12) return;
        uint32_t reply[4] = {RNDIS_KEEPALIVE | RNDIS_COMPLETION, 16, ((const uint32_t *)p)[2], 0};
        send_rndis(reply, sizeof reply, true);
        break;
    }
    default:
        if ((h->type & RNDIS_COMPLETION) && len >= 12 && ((const uint32_t *)p)[2] == nv.rndis_wait_id &&
            !nv.rndis_done) {
            memcpy(nv.rndis_reply, p, MIN(len, sizeof nv.rndis_reply));
            nv.rndis_done = true;
        }
        break;
    }
}

static void receive_xfer(uint8_t *pkt, uint32_t plen) {
    struct xfer_page_header *x = (void *)pkt;
    if (plen < sizeof *x || x->pageset_id != RECV_BUF_ID ||
        sizeof *x + (uint64_t)x->range_count * sizeof(struct xfer_range) > plen)
        goto done;
    for (uint32_t i = 0; i < x->range_count; i++) {
        struct xfer_range *r = &x->range[i];
        if ((uint64_t)r->offset + r->len > nv.recv_size) continue;
        rndis_receive(nv.recv_buf + r->offset, r->len);
    }
done:;
    /* give the space back to the host */
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT_COMPLETE, NVSP_STATUS_SUCCESS};
    for (int tries = 0; vmbus_send(nv.ch, VM_PKT_COMP, 0, m, sizeof m, x->d.trans_id) == -EAGAIN && tries < 1000;
         tries++)
        pause();
}

static void netvsc_callback(struct vmbus_channel *ch) {
    static uint8_t pkt[4096]; /* interrupt context, one channel: a static buffer is fine */
    int n;
    while ((n = vmbus_recv(ch, pkt, sizeof pkt)) != 0) {
        if (n < 0) continue;
        struct vmpacket_desc *d = (void *)pkt;
        if (d->type == VM_PKT_DATA_XFER_PAGES) {
            receive_xfer(pkt, (uint32_t)n);
            continue;
        }
        if (d->type != VM_PKT_COMP) continue;
        uint32_t dlen = vmpacket_datalen(pkt);
        uint32_t *m = vmpacket_data(pkt);
        if (dlen < 4) continue;
        if (m[0] == NVSP_SEND_RNDIS_PKT_COMPLETE) {
            if (d->trans_id >= TX_TID_BASE) section_free((uint32_t)(d->trans_id - TX_TID_BASE));
        } else if (m[0] == nv.nvsp_wait_type && !nv.nvsp_done) {
            memset(nv.nvsp_reply, 0, sizeof nv.nvsp_reply);
            memcpy(nv.nvsp_reply, m, MIN(dlen, sizeof nv.nvsp_reply));
            nv.nvsp_done = true;
        }
    }
}

/* ---- setup ---- */

/* Send an NVSP message and wait for the completion of type `reply`. */
static bool nvsp_request(const void *msg, uint32_t reply) {
    uint8_t m[NVSP_MSG_SIZE];
    memcpy(m, msg, sizeof m);
    nv.nvsp_done = false;
    nv.nvsp_wait_type = reply;
    if (vmbus_send(nv.ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, 1)) return false;
    if (!vmbus_wait(&nv.nvsp_done, 5000)) {
        kprintf("netvsc: no reply to NVSP message %u\n", *(uint32_t *)m);
        return false;
    }
    return true;
}

static bool nvsp_negotiate(void) {
    static const uint32_t versions[] = {0x60001, 0x60000, 0x50000, 0x40000, 0x30002, 0x2};
    for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
        uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_INIT, versions[i], versions[i]};
        if (!nvsp_request(m, NVSP_INIT_COMPLETE)) return false;
        uint32_t *r = (uint32_t *)nv.nvsp_reply;
        if (r[3] == NVSP_STATUS_SUCCESS) {
            nv.nvsp_version = versions[i];
            return true;
        }
    }
    kprintf("netvsc: no NVSP version accepted\n");
    return false;
}

static bool setup_buffers(void) {
    if (nv.nvsp_version >= 0x2) {
        /* mtu + ethernet header, then the capability bits: just 802.1q, like Linux */
        uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_NDIS_CONFIG, 1514, 0, 1u << 3, 0};
        if (vmbus_send(nv.ch, VM_PKT_DATA_INBAND, 0, m, sizeof m, 0)) return false;
    }
    uint32_t ndis = nv.nvsp_version > 0x40000 ? 0x6001E : 0x60001; /* NDIS 6.30 or 6.1 */
    uint32_t v[NVSP_MSG_SIZE / 4] = {NVSP_SEND_NDIS_VERSION, ndis >> 16, ndis & 0xFFFF};
    if (vmbus_send(nv.ch, VM_PKT_DATA_INBAND, 0, v, sizeof v, 0)) return false;

    uint64_t pa = pmm_alloc_contig(RECV_BUF_PAGES);
    if (!pa) return false;
    nv.recv_buf = phys_to_virt(pa);
    nv.recv_size = RECV_BUF_PAGES * PAGE_SIZE;
    memset(nv.recv_buf, 0, nv.recv_size);
    uint32_t g = vmbus_gpadl_create(nv.ch, pa, RECV_BUF_PAGES);
    if (!g) return false;
    uint32_t rb[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RECV_BUF, g, RECV_BUF_ID};
    if (!nvsp_request(rb, NVSP_SEND_RECV_BUF_COMPLETE)) return false;
    if (((uint32_t *)nv.nvsp_reply)[1] != NVSP_STATUS_SUCCESS) {
        kprintf("netvsc: receive buffer refused (%u)\n", ((uint32_t *)nv.nvsp_reply)[1]);
        return false;
    }

    pa = pmm_alloc_contig(SEND_BUF_PAGES);
    if (!pa) return false;
    nv.send_buf = phys_to_virt(pa);
    memset(nv.send_buf, 0, SEND_BUF_PAGES * PAGE_SIZE);
    g = vmbus_gpadl_create(nv.ch, pa, SEND_BUF_PAGES);
    if (!g) return false;
    uint32_t sb[NVSP_MSG_SIZE / 4] = {NVSP_SEND_SEND_BUF, g, SEND_BUF_ID};
    if (!nvsp_request(sb, NVSP_SEND_SEND_BUF_COMPLETE)) return false;
    uint32_t *r = (uint32_t *)nv.nvsp_reply;
    if (r[1] != NVSP_STATUS_SUCCESS || r[2] < 2048) {
        kprintf("netvsc: send buffer refused (%u, section %u)\n", r[1], r[2]);
        return false;
    }
    nv.section_size = r[2];
    nv.sections = SEND_BUF_PAGES * PAGE_SIZE / nv.section_size;
    nv.section_used = kzalloc((nv.sections + 63) / 64 * 8);
    return true;
}

/* Send an RNDIS control message (its req_id at offset 8) and wait for the matching completion. */
static bool rndis_request(void *msg, uint32_t len) {
    uint32_t *m = msg;
    m[2] = ++nv.next_req_id;
    nv.rndis_done = false;
    nv.rndis_wait_id = m[2];
    if (send_rndis(msg, len, true)) return false;
    if (!vmbus_wait(&nv.rndis_done, 5000)) {
        kprintf("netvsc: no reply to RNDIS message %u\n", m[0]);
        return false;
    }
    return ((uint32_t *)nv.rndis_reply)[3] == 0; /* RNDIS_STATUS_SUCCESS */
}

/* Query an OID; returns the length copied into out, or -1. */
static int rndis_query(uint32_t oid, void *out, uint32_t outlen) {
    struct rndis_oid_req q = {{RNDIS_QUERY, sizeof q}, 0, oid, 0, 20, 0};
    if (!rndis_request(&q, sizeof q)) return -1;
    uint32_t *r = (uint32_t *)nv.rndis_reply;
    uint32_t blen = r[4], boff = r[5];
    if (8ull + boff + blen > sizeof nv.rndis_reply || 8ull + boff + blen > r[1]) return -1;
    uint32_t n = MIN(blen, outlen);
    memcpy(out, nv.rndis_reply + 8 + boff, n);
    return (int)n;
}

static bool rndis_set(uint32_t oid, const void *val, uint32_t len) {
    uint8_t buf[sizeof(struct rndis_oid_req) + 16];
    struct rndis_oid_req *s = (void *)buf;
    if (len > 16) return false;
    *s = (struct rndis_oid_req){{RNDIS_SET, sizeof *s + len}, 0, oid, len, 20, 0};
    memcpy(buf + sizeof *s, val, len);
    return rndis_request(buf, sizeof *s + len);
}

static int netvsc_send(const void *frame, size_t len) {
    if (len > 1514) return -EINVAL;
    uint32_t sec = section_alloc();
    if (sec == INVALID_SECTION) {
        nv.tx_drops++;
        return -EAGAIN;
    }
    uint8_t *p = nv.send_buf + (size_t)sec * nv.section_size;
    struct rndis_packet *rp = (void *)p;
    memset(rp, 0, sizeof *rp);
    rp->h.type = RNDIS_PACKET;
    rp->h.len = (uint32_t)(sizeof *rp + len);
    rp->data_offset = sizeof *rp - 8;
    rp->data_len = (uint32_t)len;
    memcpy(p + sizeof *rp, frame, len);
    int r = send_section(sec, rp->h.len, false);
    if (r) {
        section_free(sec);
        nv.tx_drops++;
    }
    return r;
}

bool netvsc_probe(void) {
    if (!vmbus_ready()) return false;
    struct vmbus_channel *ch = vmbus_find(&net_guid, 0);
    if (!ch) return false;
    nv.ch = ch;
    if (vmbus_open(ch, 128 * 1024, 128 * 1024, netvsc_callback, NULL)) return false;
    if (!nvsp_negotiate() || !setup_buffers()) return false;

    struct {
        struct rndis_hdr h;
        uint32_t req_id, major, minor, max_xfer;
    } PACKED init = {{RNDIS_INIT, sizeof init}, 0, 1, 0, 0x4000};
    if (!rndis_request(&init, sizeof init)) {
        kprintf("netvsc: RNDIS initialisation failed\n");
        return false;
    }
    if (rndis_query(OID_802_3_PERMANENT_ADDRESS, nif.mac, 6) != 6) {
        kprintf("netvsc: could not read the MAC address\n");
        return false;
    }
    uint32_t media = 1;
    if (rndis_query(OID_GEN_MEDIA_CONNECT_STATUS, &media, 4) == 4) nv.link_up = media == 0;
    uint32_t filter = PACKET_FILTER;
    if (!rndis_set(OID_GEN_CURRENT_PACKET_FILTER, &filter, 4)) {
        kprintf("netvsc: could not set the packet filter\n");
        return false;
    }
    nif.send = netvsc_send;
    kprintf("net: netvsc (NVSP %x.%x, %u send sections of %u) mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            nv.nvsp_version >> 16, nv.nvsp_version & 0xFFFF, nv.sections, nv.section_size, nif.mac[0],
            nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5], nv.link_up ? "up" : "down");
    net_register(&nif);
    return true;
}
