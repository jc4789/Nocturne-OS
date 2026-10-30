/* Hyper-V synthetic network adapter (netvsc). Two protocols stacked on a VMBus channel: NVSP sets
   up two big shared buffers (receive and send), and RNDIS, the USB-style remote NDIS protocol,
   runs over them. Every frame and every RNDIS control message travels as an RNDIS message: the
   host writes received ones into the receive buffer and tells us where (a transfer-page packet,
   which we must complete so it can reuse the space); we copy ones we send into a section of the
   send buffer and tell the host which section (it completes that packet when done with it). */
#include "kernel.h"
#include "vm/devices.h"
#include "abi.h"
#include "arch/cpu.h"
#include "vm/hv/vmbus.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "net/net.h"
#include "sys/sched.h"

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
/* These limit one turn of the net thread, not the accepted transfer size.
   A cursor and host-owned receive credit survive a budget boundary. */
#define RX_MESSAGE_BUDGET 128
#define RX_BYTE_BUDGET (128 * 1024)
#define RX_TIME_BUDGET_MS 2

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

struct setup_frame {
    struct setup_frame *next;
    uint32_t len;
    uint8_t data[];
};

struct keepalive_reply {
    struct keepalive_reply *next;
    uint32_t request;
};

static struct {
    struct vmbus_channel *ch;
    uint32_t nvsp_version;
    uint8_t *recv_buf, *send_buf;
    uint32_t recv_gpadl, send_gpadl;
    bool recv_shared, send_shared;
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
    uint64_t tx_backpressure;
    /* The channel callback and the rescind (VMBus's interrupt handler, CPU 0) only set these
       latches, mask the host's signal and wake the net thread. Everything else here (both rings,
       the send sections, the receive borrow, the queues below) belongs to the stack's calls into
       the adapter, which come one at a time from any CPU under the stack's lock, and before
       registration to the probe alone: the net thread (or the probe's synchronous wait) is the
       sole ring reader. */
    volatile bool event_pending, detaching;
    uint8_t *rx_packet;
    uint32_t rx_capacity, rx_len, rx_range, rx_position;
    bool rx_borrowed, completion_wait;
    struct setup_frame *setup_first, *setup_last;
    struct keepalive_reply *keepalive_first, *keepalive_last;
    size_t deferred_bytes;
    uint64_t rx_dropped, rx_invalid, rx_budget_yields;
    uint64_t rx_completion_retries, rx_completion_cancelled, rx_ring_high_water;
} nv;

static struct netif nif = {.driver = "netvsc"};

/* ---- send-buffer sections ---- */

static uint32_t section_alloc(void) {
    if (!nv.section_used) return INVALID_SECTION;
    for (uint32_t i = 0; i < nv.sections; i++) {
        if (!(nv.section_used[i / 64] & (1ull << (i % 64)))) {
            nv.section_used[i / 64] |= 1ull << (i % 64);
            return i;
        }
    }
    return INVALID_SECTION;
}

static bool section_free(uint32_t i) {
    if (i >= nv.sections || !nv.section_used) return false;
    bool used = (nv.section_used[i / 64] & (1ull << (i % 64))) != 0;
    nv.section_used[i / 64] &= ~(1ull << (i % 64));
    return used;
}

/* Hand the RNDIS message in section `sec` to the host. */
static int send_section(uint32_t sec, uint32_t len, bool control) {
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT, control ? 1 : 0, sec, len};
    return vmbus_send(nv.ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, TX_TID_BASE + sec);
}

/* Copy an RNDIS message into a free section and send it. */
static int send_rndis(const void *msg, uint32_t len, bool control) {
    if (len > nv.section_size) return -N_ERR_INVALID;
    uint32_t sec = section_alloc();
    if (sec == INVALID_SECTION) return -N_ERR_NOT_READY;
    memcpy(nv.send_buf + (size_t)sec * nv.section_size, msg, len);
    int r = send_section(sec, len, control);
    if (r) section_free(sec);
    return r;
}

/* ---- receive path (the net thread, or the probe before registration) ---- */

static void receive_frame(const void *frame, uint32_t len, void (*input)(const void *, size_t)) {
    if (input) {
        /* The stack consumes this borrow before returning. Do not return the
           transfer's host credit until every frame in it has been consumed. */
        input(frame, len);
        return;
    }
    /* The packet filter can become active just before its setup reply. Keep
       those early frames until the net thread starts, rather than putting a
       startup burst through the old 63-frame IRQ staging queue. */
    size_t allocation = sizeof(struct setup_frame) + len;
    /* Probe-only staging and control replies share a memory budget backed by
       the actual receive buffer, rather than growing an unbounded heap queue.
       Normal data reception allocates nothing and retains the host credit. */
    struct setup_frame *saved = allocation <= nv.recv_size - nv.deferred_bytes ? kmalloc(allocation) : NULL;
    if (!saved) { nv.rx_dropped++; return; }
    nv.deferred_bytes += allocation;
    saved->next = NULL;
    saved->len = len;
    memcpy(saved->data, frame, len);
    if (nv.setup_last) nv.setup_last->next = saved;
    else nv.setup_first = saved;
    nv.setup_last = saved;
}

static void rndis_receive(const uint8_t *p, uint32_t len, void (*input)(const void *, size_t)) {
    if (len < sizeof(struct rndis_hdr)) { nv.rx_invalid++; return; }
    const struct rndis_hdr *h = (const void *)p;
    if (h->len < sizeof *h || h->len > len) { nv.rx_invalid++; return; }
    switch (h->type) {
    case RNDIS_PACKET: {
        const struct rndis_packet *rp = (const void *)p;
        if (h->len < sizeof *rp) { nv.rx_invalid++; return; }
        uint64_t end = 8ull + rp->data_offset + rp->data_len;
        if (rp->data_offset < sizeof *rp - 8 || end > h->len ||
            rp->data_len < 14 || rp->data_len > 1536) { nv.rx_invalid++; return; }
        receive_frame(p + 8 + rp->data_offset, rp->data_len, input);
        break;
    }
    case RNDIS_INDICATE: {
        uint32_t status = h->len >= 12 ? ((const uint32_t *)p)[2] : 0;
        if (status == RNDIS_STATUS_MEDIA_CONNECT) nv.link_up = true;
        if (status == RNDIS_STATUS_MEDIA_DISCONNECT) nv.link_up = false;
        if (status == RNDIS_STATUS_MEDIA_CONNECT || status == RNDIS_STATUS_MEDIA_DISCONNECT)
            kprintf("netvsc: link %s\n", nv.link_up ? "up" : "down");
        break;
    }
    case RNDIS_KEEPALIVE: {
        if (h->len < 12) { nv.rx_invalid++; return; }
        struct keepalive_reply *reply = sizeof *reply <= nv.recv_size - nv.deferred_bytes ?
            kmalloc(sizeof *reply) : NULL;
        if (!reply) { nv.rx_dropped++; return; }
        nv.deferred_bytes += sizeof *reply;
        reply->next = NULL;
        reply->request = ((const uint32_t *)p)[2];
        if (nv.keepalive_last) nv.keepalive_last->next = reply;
        else nv.keepalive_first = reply;
        nv.keepalive_last = reply;
        break;
    }
    default:
        if ((h->type & RNDIS_COMPLETION) && h->len >= 16 && ((const uint32_t *)p)[2] == nv.rndis_wait_id &&
            !nv.rndis_done) {
            memset(nv.rndis_reply, 0, sizeof nv.rndis_reply);
            memcpy(nv.rndis_reply, p, MIN(h->len, sizeof nv.rndis_reply));
            nv.rndis_done = true;
        }
        break;
    }
}

static bool complete_receive(void) {
    if (!nv.rx_borrowed || !nv.completion_wait) return true;
    const struct vmpacket_desc *d = (const void *)nv.rx_packet;
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT_COMPLETE, NVSP_STATUS_SUCCESS};
    int result = vmbus_send(nv.ch, VM_PKT_COMP, 0, m, sizeof m, d->trans_id);
    if (result) {
        if (result == -N_ERR_NOT_READY) nv.rx_completion_retries++;
        /* Keep both the exact transaction ID and ownership on every failure.
           Only successful enqueue, or channel teardown, retires this borrow. */
        return false;
    }
    nv.rx_borrowed = nv.completion_wait = false;
    return true;
}

static void netvsc_callback(struct vmbus_channel *ch) {
    if (ch != __atomic_load_n(&nv.ch, __ATOMIC_ACQUIRE) || ch->rescinded) return;
    ch->in->interrupt_mask = 1;
    nv.event_pending = true;
    net_rx_notify();
}

static void netvsc_rescind(struct vmbus_channel *ch) {
    if (ch != __atomic_load_n(&nv.ch, __ATOMIC_ACQUIRE)) return;
    nv.detaching = true;
    nv.link_up = false;
    net_rx_notify();
}

static void free_buffer(uint8_t *buffer, uint32_t pages) {
    if (!buffer) return;
    uint64_t pa = hhdm_virt_to_phys(buffer);
    for (uint32_t i = 0; i < pages; i++) pmm_free(pa + (uint64_t)i * PAGE_SIZE);
}

static void netvsc_shutdown(void) {
    struct vmbus_channel *ch = nv.ch;
    if (!ch) return;
    /* No callback can consume a descriptor or free memory. The net thread (or
       the failed probe) closes this claimed channel, after any input borrow
       has returned. From here on the callback and the rescind find nv.ch no
       longer theirs and touch nothing; the rest uses ch. A rescind invalidates
       pending IDs; they are never sent to a later channel with a potentially
       reused relid. */
    __atomic_store_n(&nv.ch, NULL, __ATOMIC_SEQ_CST);
    nv.link_up = false;
    if (nv.rx_borrowed) nv.rx_completion_cancelled++;
    nv.rx_borrowed = nv.completion_wait = false;
    vmbus_close_confirmed(ch);
    /* Rescind retires channel transactions, but is not a GPADL teardown ACK.
       Preserve shared backing on that path: a late host write must never hit
       reallocated memory. Normal shutdown can reclaim after explicit ACKs. */
    bool recv_safe = !nv.recv_shared;
    bool send_safe = !nv.send_shared;
    if (!ch->rescinded) {
        if (!recv_safe && nv.recv_gpadl) recv_safe = vmbus_gpadl_teardown(ch, nv.recv_gpadl);
        if (!send_safe && nv.send_gpadl) send_safe = vmbus_gpadl_teardown(ch, nv.send_gpadl);
    }
    if (recv_safe) free_buffer(nv.recv_buf, RECV_BUF_PAGES);
    else kprintf("netvsc: preserving receive pages: host mapping not retired\n");
    if (send_safe) free_buffer(nv.send_buf, SEND_BUF_PAGES);
    else kprintf("netvsc: preserving send pages: host mapping not retired\n");
    nv.recv_buf = nv.send_buf = NULL;
    nv.recv_gpadl = nv.send_gpadl = 0;
    nv.recv_shared = nv.send_shared = false;
    kfree(nv.section_used);
    nv.section_used = NULL;
    nv.sections = 0;
    kfree(nv.rx_packet);
    nv.rx_packet = NULL;
    while (nv.setup_first) {
        struct setup_frame *saved = nv.setup_first;
        nv.setup_first = saved->next;
        nv.rx_dropped++;
        kfree(saved);
    }
    nv.setup_last = NULL;
    while (nv.keepalive_first) {
        struct keepalive_reply *reply = nv.keepalive_first;
        nv.keepalive_first = reply->next;
        kfree(reply);
    }
    nv.keepalive_last = NULL;
    nv.deferred_bytes = 0;
    nv.event_pending = nv.detaching = false;
    vmbus_release(ch);
}

static bool netvsc_rx_pending(void) {
    /* Once shut down nothing is left to do, also when a rescind racing the shutdown on CPU 0
       set detaching again. */
    if (!nv.ch) return false;
    if (nv.detaching) return true;
    if (nv.ch->rescinded) return false;
    if (nv.event_pending || nv.setup_first) return true;
    if (nv.completion_wait)
        return vmbus_write_ready(nv.ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE);
    if (nv.rx_borrowed || vmbus_rx_pending(nv.ch)) return true;
    /* A keepalive can also wait for a send-buffer section, whose host
       completion arrives on the input ring. Do not busy-poll a full ring. */
    if (nv.keepalive_first && vmbus_write_ready(nv.ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE)) {
        for (uint32_t i = 0; i < nv.sections; i++)
            if (!(nv.section_used[i / 64] & (1ull << (i % 64)))) return true;
    }
    return false;
}

static size_t netvsc_poll_rx(void (*input)(const void *, size_t)) {
    if (nv.detaching || (nv.ch && nv.ch->rescinded)) { netvsc_shutdown(); return 0; }
    if (!nv.ch || !nv.rx_packet) return 0;
    uint64_t started = uptime_ms();
    uint32_t messages = 0, bytes = 0;
    size_t frames = 0;
    bool completion_blocked = false;
    bool keepalive_blocked = false;
    bool budget_exhausted = false;
    nv.ch->in->interrupt_mask = 1;
    nv.event_pending = false;
    while (messages < RX_MESSAGE_BUDGET && bytes < RX_BYTE_BUDGET) {
        if ((messages & 7) == 0 && uptime_ms() - started >= RX_TIME_BUDGET_MS) {
            budget_exhausted = true;
            break;
        }
        if (nv.detaching || nv.ch->rescinded) break;
        if (nv.completion_wait && !complete_receive()) { completion_blocked = true; break; }
        if (input && nv.setup_first) {
            struct setup_frame *saved = nv.setup_first;
            nv.setup_first = saved->next;
            if (!nv.setup_first) nv.setup_last = NULL;
            input(saved->data, saved->len);
            bytes += saved->len;
            frames++;
            messages++;
            nv.deferred_bytes -= sizeof *saved + saved->len;
            kfree(saved);
            continue;
        }
        if (nv.keepalive_first && !keepalive_blocked) {
            struct keepalive_reply *reply = nv.keepalive_first;
            uint32_t m[4] = {RNDIS_KEEPALIVE | RNDIS_COMPLETION, 16, reply->request, 0};
            if (!send_rndis(m, sizeof m, true)) {
                nv.keepalive_first = reply->next;
                if (!nv.keepalive_first) nv.keepalive_last = NULL;
                nv.deferred_bytes -= sizeof *reply;
                kfree(reply);
                messages++;
                continue;
            }
            keepalive_blocked = true;
        }
        if (nv.rx_borrowed) {
            struct xfer_page_header *x = (void *)nv.rx_packet;
            if (nv.rx_range == x->range_count) { nv.completion_wait = true; continue; }
            const struct xfer_range *r = &x->range[nv.rx_range];
            if ((uint64_t)r->offset + r->len > nv.recv_size || !nv.recv_buf) {
                nv.rx_invalid++;
                nv.rx_range++;
                nv.rx_position = 0;
                messages++;
                continue;
            }
            if (nv.rx_position == r->len) { nv.rx_range++; nv.rx_position = 0; messages++; continue; }
            uint32_t remaining = r->len - nv.rx_position;
            const uint8_t *p = nv.recv_buf + r->offset + nv.rx_position;
            const struct rndis_hdr *h = (const void *)p;
            if (remaining < sizeof *h || h->len < sizeof *h || h->len > remaining) {
                nv.rx_invalid++;
                nv.rx_range++;
                nv.rx_position = 0;
                messages++;
                continue;
            }
            uint32_t len = h->len;
            if (h->type == RNDIS_PACKET) frames++;
            rndis_receive(p, len, input);
            nv.rx_position += len;
            bytes += len;
            messages++;
            continue;
        }
        uint32_t r = nv.ch->in->read_index, w = nv.ch->in->write_index;
        uint32_t used = w >= r ? w - r : nv.ch->in_size - r + w;
        nv.rx_ring_high_water = MAX(nv.rx_ring_high_water, used);
        int n = vmbus_recv(nv.ch, nv.rx_packet, nv.rx_capacity);
        if (!n) break;
        messages++;
        if (n < 0) { nv.rx_invalid++; continue; }
        struct vmpacket_desc *d = (void *)nv.rx_packet;
        if (d->type == VM_PKT_DATA_XFER_PAGES) {
            /* The descriptor allocation is as large as the entire input
               ring. No accepted transfer ID is lost to a 4 KiB scratch cap. */
            nv.rx_len = (uint32_t)n;
            nv.rx_range = nv.rx_position = 0;
            nv.rx_borrowed = true;
            struct xfer_page_header *x = (void *)nv.rx_packet;
            nv.completion_wait = nv.rx_len < sizeof *x || x->pageset_id != RECV_BUF_ID ||
                d->offset8 > d->len8 ||
                sizeof *x + (uint64_t)x->range_count * sizeof(struct xfer_range) > (uint32_t)d->offset8 * 8;
            if (nv.completion_wait) nv.rx_invalid++;
            continue;
        }
        if (d->type != VM_PKT_COMP) continue;
        if (d->offset8 < sizeof *d / 8 || d->offset8 > d->len8) { nv.rx_invalid++; continue; }
        uint32_t dlen = vmpacket_datalen(nv.rx_packet);
        uint32_t *m = vmpacket_data(nv.rx_packet);
        if (dlen < 4) { nv.rx_invalid++; continue; }
        if (m[0] == NVSP_SEND_RNDIS_PKT_COMPLETE) {
            if (d->trans_id >= TX_TID_BASE && d->trans_id - TX_TID_BASE < nv.sections) {
                if (section_free((uint32_t)(d->trans_id - TX_TID_BASE))) {
                    keepalive_blocked = false;
                    net_rx_notify();
                }
            }
        } else if (m[0] == nv.nvsp_wait_type && !nv.nvsp_done) {
            memset(nv.nvsp_reply, 0, sizeof nv.nvsp_reply);
            memcpy(nv.nvsp_reply, m, MIN(dlen, sizeof nv.nvsp_reply));
            nv.nvsp_done = true;
        }
    }
    budget_exhausted |= messages >= RX_MESSAGE_BUDGET || bytes >= RX_BYTE_BUDGET;
    if (nv.detaching || nv.ch->rescinded) { netvsc_shutdown(); return frames; }
    /* Return the last transfer promptly, even when its final packet exactly
       exhausted the processing budget. A full ring keeps the borrow intact. */
    if (nv.rx_borrowed && !nv.completion_wait) {
        struct xfer_page_header *x = (void *)nv.rx_packet;
        if (nv.rx_range == x->range_count ||
            (nv.rx_range + 1 == x->range_count && nv.rx_position == x->range[nv.rx_range].len))
            nv.completion_wait = true;
    }
    if (nv.completion_wait && !completion_blocked) complete_receive();
    nv.ch->in->interrupt_mask = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Rearm before the final readiness check; an arrival crossing the empty
       check cannot be lost: the callback, on CPU 0 at any point of this, sets
       event_pending before it wakes the net thread, so either the check below
       sees the arrival or the callback's wake comes after it. A blocked
       completion waits for host progress. */
    bool pending = netvsc_rx_pending();
    if (pending) {
        if (budget_exhausted) nv.rx_budget_yields++;
        net_rx_notify();
    }
    return frames;
}

static void netvsc_get_stats(struct n_netstats *stats) {
    stats->rx_dropped = nv.rx_dropped;
    stats->rx_invalid = nv.rx_invalid;
    stats->rx_budget_yields = nv.rx_budget_yields;
    stats->rx_completion_pending = nv.rx_borrowed ? 1 : 0;
    stats->rx_completion_retries = nv.rx_completion_retries;
    stats->rx_completion_cancelled = nv.rx_completion_cancelled;
    stats->rx_ring_high_water = nv.rx_ring_high_water;
}

static bool netvsc_wait(volatile bool *flag, uint64_t ms) {
    uint64_t started = uptime_ms();
    while (!*flag) {
        if (!nv.ch || nv.ch->rescinded || uptime_ms() - started >= ms) return false;
        netvsc_poll_rx(NULL);
        if (*flag) return nv.ch && !nv.detaching && !nv.ch->rescinded;
        /* No IRQ-side parsing, and no pause-loop on a blocked completion.
           The timer provides a fallback on hosts without ring-space events. */
        if ((nv.rx_borrowed && !nv.completion_wait) || vmbus_rx_pending(nv.ch) ||
            (nv.completion_wait && vmbus_write_ready(nv.ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE)))
            sched_yield();
        else sleep_ms(1);
    }
    return nv.ch && !nv.detaching && !nv.ch->rescinded;
}

/* ---- setup ---- */

/* Send an NVSP message and wait for the completion of type `reply`. */
static bool nvsp_request(const void *msg, uint32_t reply) {
    uint8_t m[NVSP_MSG_SIZE];
    memcpy(m, msg, sizeof m);
    nv.nvsp_done = false;
    nv.nvsp_wait_type = reply;
    if (vmbus_send(nv.ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, 1)) return false;
    if (!netvsc_wait(&nv.nvsp_done, 5000)) {
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
        /* mtu + ethernet header, then the capability bits: 802.1q */
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
    nv.recv_shared = true; /* a timeout must not make shared pages reusable */
    uint32_t g = nv.recv_gpadl = vmbus_gpadl_create(nv.ch, pa, RECV_BUF_PAGES);
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
    nv.send_shared = true;
    g = nv.send_gpadl = vmbus_gpadl_create(nv.ch, pa, SEND_BUF_PAGES);
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
    if (!nv.sections) return false;
    nv.section_used = kzalloc((nv.sections + 63) / 64 * 8);
    return nv.section_used != NULL;
}

/* Send an RNDIS control message (its req_id at offset 8) and wait for the matching completion. */
static bool rndis_request(void *msg, uint32_t len) {
    uint32_t *m = msg;
    m[2] = ++nv.next_req_id;
    nv.rndis_done = false;
    nv.rndis_wait_id = m[2];
    if (send_rndis(msg, len, true)) return false;
    if (!netvsc_wait(&nv.rndis_done, 5000)) {
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
    if (!nv.ch || nv.detaching || nv.ch->rescinded) return -N_ERR_NETWORK_DOWN;
    if (len > 1514 || sizeof(struct rndis_packet) + len > nv.section_size) return -N_ERR_INVALID;
    uint32_t sec = section_alloc();
    if (sec == INVALID_SECTION) {
        nv.tx_backpressure++;
        return -N_ERR_NOT_READY;
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
        if (r == -N_ERR_NOT_READY) nv.tx_backpressure++;
    }
    return r;
}

bool netvsc_probe(void) {
    if (!vmbus_ready()) return false;
    struct vmbus_channel *ch = vmbus_find(&net_guid, 0);
    if (!ch) return false;
    nv.ch = ch;
    ch->claimed = true;
    vmbus_set_rescind(ch, netvsc_rescind);
    if (vmbus_open(ch, 128 * 1024, 128 * 1024, netvsc_callback, NULL)) goto failed;
    nv.rx_capacity = ch->in_size;
    nv.rx_packet = kmalloc(nv.rx_capacity);
    if (!nv.rx_packet || !nvsp_negotiate() || !setup_buffers()) goto failed;

    struct {
        struct rndis_hdr h;
        uint32_t req_id, major, minor, max_xfer;
    } PACKED init = {{RNDIS_INIT, sizeof init}, 0, 1, 0, 0x4000};
    if (!rndis_request(&init, sizeof init)) {
        kprintf("netvsc: RNDIS initialisation failed\n");
        goto failed;
    }
    if (rndis_query(OID_802_3_PERMANENT_ADDRESS, nif.mac, 6) != 6) {
        kprintf("netvsc: could not read the MAC address\n");
        goto failed;
    }
    uint32_t media = 1;
    if (rndis_query(OID_GEN_MEDIA_CONNECT_STATUS, &media, 4) == 4) nv.link_up = media == 0;
    uint32_t filter = PACKET_FILTER;
    if (!rndis_set(OID_GEN_CURRENT_PACKET_FILTER, &filter, 4)) {
        kprintf("netvsc: could not set the packet filter\n");
        goto failed;
    }
    nif.send = netvsc_send;
    nif.rx_pending = netvsc_rx_pending;
    nif.poll_rx = netvsc_poll_rx;
    nif.get_stats = netvsc_get_stats;
    kprintf("net: netvsc (NVSP %x.%x, %u send sections of %u) mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            nv.nvsp_version >> 16, nv.nvsp_version & 0xFFFF, nv.sections, nv.section_size, nif.mac[0],
            nif.mac[1], nif.mac[2], nif.mac[3], nif.mac[4], nif.mac[5], nv.link_up ? "up" : "down");
    net_register(&nif);
    return true;
failed:
    netvsc_shutdown();
    return false;
}
