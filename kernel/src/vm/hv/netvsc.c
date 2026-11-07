/* Hyper-V synthetic network adapter (netvsc). Two protocols stacked on a VMBus channel: NVSP sets
   up two big shared buffers (receive and send), and RNDIS, the USB-style remote NDIS protocol,
   runs over them. Every frame and every RNDIS control message travels as an RNDIS message: the
   host writes received ones into the receive buffer and tells us where (a transfer-page packet,
   which we must complete so it can reuse the space); we copy ones we send into a section of the
   send buffer and tell the host which section (it completes that packet when done with it). */
#include "kernel.h"
#include "hal/hal.h"
#include "abi.h"
#include "arch/cpu.h"
#include "vm/hv/vmbus.h"
#include "mm/heap.h"
#include "net/net.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "sys/wait.h"

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
#define RX_TIME_BUDGET (2 * NS_PER_MS)
#define SETUP_REPLY_WAIT (5 * NS_PER_S) /* how long the probe waits for each NVSP or RNDIS reply */

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

/* One synthetic adapter: the state of one channel, from attach to detach. */
struct netvsc {
    struct netif nif; /* first: self is the device */
    struct netvsc *next; /* the adapters that are up (netvsc_ip) */
    struct vmbus_channel *ch;
    struct hal_device *hal;
    uint32_t nvsp_version;
    /* host と分ける緩衝。HAL の DMA の口で取り、手放さない：装置が外れた後、バスが host はもう持たないと答えた時に
       HAL が返す（vmbus.h の vmbus_gpadl_create） */
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
    uint64_t tx_backpressure;
    /* The channel callback and the rescind (VMBus's interrupt handler, CPU 0) only set these
       latches, mask the host's signal, count the signal and wake the net thread and the probe's
       wait (netvsc_wait). Everything else here (both rings,
       the send sections, the receive borrow, the queues below) belongs to the stack's calls into
       the adapter, which come one at a time from any CPU under the stack's lock, and before
       registration to the probe alone: the net thread (or the probe's synchronous wait) is the
       sole ring reader. */
    volatile bool event_pending, detaching;
    uint64_t signals; /* every callback and rescind; the probe's wait sleeps until it moves */
    struct wait_queue signal_wq;
    uint8_t *rx_packet;
    uint32_t rx_capacity, rx_len, rx_range, rx_position;
    bool rx_borrowed, completion_wait;
    struct setup_frame *setup_first, *setup_last;
    struct keepalive_reply *keepalive_first, *keepalive_last;
    size_t deferred_bytes;
    uint64_t rx_dropped, rx_invalid, rx_budget_yields;
    uint64_t rx_completion_retries, rx_completion_cancelled, rx_ring_high_water;
};

/* ---- send-buffer sections ---- */

static uint32_t section_alloc(struct netvsc *c) {
    if (!c->section_used) return INVALID_SECTION;
    for (uint32_t i = 0; i < c->sections; i++) {
        if (!(c->section_used[i / 64] & (1ull << (i % 64)))) {
            c->section_used[i / 64] |= 1ull << (i % 64);
            return i;
        }
    }
    return INVALID_SECTION;
}

static bool section_free(struct netvsc *c, uint32_t i) {
    if (i >= c->sections || !c->section_used) return false;
    bool used = (c->section_used[i / 64] & (1ull << (i % 64))) != 0;
    c->section_used[i / 64] &= ~(1ull << (i % 64));
    return used;
}

/* Hand the RNDIS message in section `sec` to the host. */
static int send_section(struct netvsc *c, uint32_t sec, uint32_t len, bool control) {
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT, control ? 1 : 0, sec, len};
    return vmbus_send(c->ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, TX_TID_BASE + sec);
}

/* Copy an RNDIS message into a free section and send it. */
static int send_rndis(struct netvsc *c, const void *msg, uint32_t len, bool control) {
    if (len > c->section_size) return -N_ERR_INVALID;
    uint32_t sec = section_alloc(c);
    if (sec == INVALID_SECTION) return -N_ERR_NOT_READY;
    memcpy(c->send_buf + (size_t)sec * c->section_size, msg, len);
    int r = send_section(c, sec, len, control);
    if (r) section_free(c, sec);
    return r;
}

/* ---- receive path (the net thread, or the probe before registration) ---- */

static void receive_frame(struct netvsc *c, const void *frame, uint32_t len, net_input_fn input) {
    if (input) {
        /* The stack consumes this borrow before returning. Do not return the
           transfer's host credit until every frame in it has been consumed. */
        input(&c->nif, frame, len, 0);
        return;
    }
    /* The packet filter can become active just before its setup reply. Keep
       those early frames until the net thread starts, rather than putting a
       startup burst through the old 63-frame IRQ staging queue. */
    size_t allocation = sizeof(struct setup_frame) + len;
    /* Probe-only staging and control replies share a memory budget backed by
       the actual receive buffer, rather than growing an unbounded heap queue.
       Normal data reception allocates nothing and retains the host credit. */
    struct setup_frame *saved = allocation <= c->recv_size - c->deferred_bytes ? kmalloc(allocation) : NULL;
    if (!saved) { c->rx_dropped++; return; }
    c->deferred_bytes += allocation;
    saved->next = NULL;
    saved->len = len;
    memcpy(saved->data, frame, len);
    if (c->setup_last) c->setup_last->next = saved;
    else c->setup_first = saved;
    c->setup_last = saved;
}

static void rndis_receive(struct netvsc *c, const uint8_t *p, uint32_t len, net_input_fn input) {
    if (len < sizeof(struct rndis_hdr)) { c->rx_invalid++; return; }
    const struct rndis_hdr *h = (const void *)p;
    if (h->len < sizeof *h || h->len > len) { c->rx_invalid++; return; }
    switch (h->type) {
    case RNDIS_PACKET: {
        const struct rndis_packet *rp = (const void *)p;
        if (h->len < sizeof *rp) { c->rx_invalid++; return; }
        uint64_t end = 8ull + rp->data_offset + rp->data_len;
        if (rp->data_offset < sizeof *rp - 8 || end > h->len ||
            rp->data_len < 14 || rp->data_len > 1536) { c->rx_invalid++; return; }
        receive_frame(c, p + 8 + rp->data_offset, rp->data_len, input);
        break;
    }
    case RNDIS_INDICATE: {
        uint32_t status = h->len >= 12 ? ((const uint32_t *)p)[2] : 0;
        if (status == RNDIS_STATUS_MEDIA_CONNECT) c->link_up = true;
        if (status == RNDIS_STATUS_MEDIA_DISCONNECT) c->link_up = false;
        if (status == RNDIS_STATUS_MEDIA_CONNECT || status == RNDIS_STATUS_MEDIA_DISCONNECT)
            kprintf("netvsc: link %s\n", c->link_up ? "up" : "down");
        break;
    }
    case RNDIS_KEEPALIVE: {
        if (h->len < 12) { c->rx_invalid++; return; }
        struct keepalive_reply *reply = sizeof *reply <= c->recv_size - c->deferred_bytes ?
            kmalloc(sizeof *reply) : NULL;
        if (!reply) { c->rx_dropped++; return; }
        c->deferred_bytes += sizeof *reply;
        reply->next = NULL;
        reply->request = ((const uint32_t *)p)[2];
        if (c->keepalive_last) c->keepalive_last->next = reply;
        else c->keepalive_first = reply;
        c->keepalive_last = reply;
        break;
    }
    default:
        if ((h->type & RNDIS_COMPLETION) && h->len >= 16 && ((const uint32_t *)p)[2] == c->rndis_wait_id &&
            !c->rndis_done) {
            memset(c->rndis_reply, 0, sizeof c->rndis_reply);
            memcpy(c->rndis_reply, p, MIN(h->len, sizeof c->rndis_reply));
            c->rndis_done = true;
        }
        break;
    }
}

static bool complete_receive(struct netvsc *c) {
    if (!c->rx_borrowed || !c->completion_wait) return true;
    const struct vmpacket_desc *d = (const void *)c->rx_packet;
    uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RNDIS_PKT_COMPLETE, NVSP_STATUS_SUCCESS};
    int result = vmbus_send(c->ch, VM_PKT_COMP, 0, m, sizeof m, d->trans_id);
    if (result) {
        if (result == -N_ERR_NOT_READY) c->rx_completion_retries++;
        /* Keep both the exact transaction ID and ownership on every failure.
           Only successful enqueue, or channel teardown, retires this borrow. */
        return false;
    }
    c->rx_borrowed = c->completion_wait = false;
    return true;
}

/* The host moved something: a packet came, room came in the outbound ring (pending_send_sz), or
   the channel went. Interrupt context. */
static void netvsc_signal(struct netvsc *c) {
    __atomic_fetch_add(&c->signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&c->signal_wq);
    net_rx_notify();
}

static void netvsc_callback(struct vmbus_channel *ch) {
    struct netvsc *c = ch->priv;
    if (ch != __atomic_load_n(&c->ch, __ATOMIC_ACQUIRE) || ch->rescinded) return;
    ch->in->interrupt_mask = 1;
    c->event_pending = true;
    netvsc_signal(c);
}

static void netvsc_rescind(struct vmbus_channel *ch) {
    struct netvsc *c = ch->priv;
    if (ch != __atomic_load_n(&c->ch, __ATOMIC_ACQUIRE)) return;
    c->detaching = true;
    c->link_up = false;
    netvsc_signal(c);
}

static void netvsc_shutdown(struct netvsc *c) {
    struct vmbus_channel *ch = c->ch;
    if (!ch) return;
    /* No callback can consume a descriptor or free memory. The net thread (or
       the failed attach) closes this channel, after any input borrow
       has returned. From here on the callback and the rescind find c->ch no
       longer theirs and touch nothing; the rest uses ch. A rescind invalidates
       pending IDs; they are never sent to a later channel with a potentially
       reused relid. */
    __atomic_store_n(&c->ch, NULL, __ATOMIC_SEQ_CST);
    c->link_up = false;
    if (c->rx_borrowed) c->rx_completion_cancelled++;
    c->rx_borrowed = c->completion_wait = false;
    vmbus_close(ch);
    /* 緩衝の一覧はバスの stop が外し、緩衝は HAL が返す（hal_release） */
    c->recv_buf = c->send_buf = NULL;
    kfree(c->section_used);
    c->section_used = NULL;
    c->sections = 0;
    kfree(c->rx_packet);
    c->rx_packet = NULL;
    while (c->setup_first) {
        struct setup_frame *saved = c->setup_first;
        c->setup_first = saved->next;
        c->rx_dropped++;
        kfree(saved);
    }
    c->setup_last = NULL;
    while (c->keepalive_first) {
        struct keepalive_reply *reply = c->keepalive_first;
        c->keepalive_first = reply->next;
        kfree(reply);
    }
    c->keepalive_last = NULL;
    c->deferred_bytes = 0;
    c->event_pending = c->detaching = false;
    vmbus_release(ch);
}

static bool netvsc_rx_pending(struct netif *self, unsigned queue) {
    (void)queue;
    struct netvsc *c = (struct netvsc *)self;
    /* A channel that is going has nothing left to do: its detach shuts it down. */
    if (!c->ch || c->detaching || c->ch->rescinded) return false;
    if (c->event_pending || c->setup_first) return true;
    if (c->completion_wait)
        return vmbus_write_ready(c->ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE);
    if (c->rx_borrowed || vmbus_rx_pending(c->ch)) return true;
    /* A keepalive can also wait for a send-buffer section, whose host
       completion arrives on the input ring. Do not busy-poll a full ring. */
    if (c->keepalive_first && vmbus_write_ready(c->ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE)) {
        for (uint32_t i = 0; i < c->sections; i++)
            if (!(c->section_used[i / 64] & (1ull << (i % 64)))) return true;
    }
    return false;
}

/* Take in up to budget frames (without input, the probe: the frames are kept for the stack). */
static size_t receive(struct netvsc *c, size_t budget, net_input_fn input) {
    if (c->detaching || !c->ch || c->ch->rescinded || !c->rx_packet) return 0;
    uint64_t started = clock_now_ns();
    uint32_t messages = 0, bytes = 0;
    size_t frames = 0;
    bool completion_blocked = false;
    bool keepalive_blocked = false;
    bool budget_exhausted = false;
    c->ch->in->interrupt_mask = 1;
    c->event_pending = false;
    while (messages < RX_MESSAGE_BUDGET && bytes < RX_BYTE_BUDGET && frames < budget) {
        if ((messages & 7) == 0 && clock_now_ns() >= started + RX_TIME_BUDGET) {
            budget_exhausted = true;
            break;
        }
        if (c->detaching || c->ch->rescinded) break;
        if (c->completion_wait && !complete_receive(c)) { completion_blocked = true; break; }
        if (input && c->setup_first) {
            struct setup_frame *saved = c->setup_first;
            c->setup_first = saved->next;
            if (!c->setup_first) c->setup_last = NULL;
            input(&c->nif, saved->data, saved->len, 0);
            bytes += saved->len;
            frames++;
            messages++;
            c->deferred_bytes -= sizeof *saved + saved->len;
            kfree(saved);
            continue;
        }
        if (c->keepalive_first && !keepalive_blocked) {
            struct keepalive_reply *reply = c->keepalive_first;
            uint32_t m[4] = {RNDIS_KEEPALIVE | RNDIS_COMPLETION, 16, reply->request, 0};
            if (!send_rndis(c, m, sizeof m, true)) {
                c->keepalive_first = reply->next;
                if (!c->keepalive_first) c->keepalive_last = NULL;
                c->deferred_bytes -= sizeof *reply;
                kfree(reply);
                messages++;
                continue;
            }
            keepalive_blocked = true;
        }
        if (c->rx_borrowed) {
            struct xfer_page_header *x = (void *)c->rx_packet;
            if (c->rx_range == x->range_count) { c->completion_wait = true; continue; }
            const struct xfer_range *r = &x->range[c->rx_range];
            if ((uint64_t)r->offset + r->len > c->recv_size || !c->recv_buf) {
                c->rx_invalid++;
                c->rx_range++;
                c->rx_position = 0;
                messages++;
                continue;
            }
            if (c->rx_position == r->len) { c->rx_range++; c->rx_position = 0; messages++; continue; }
            uint32_t remaining = r->len - c->rx_position;
            const uint8_t *p = c->recv_buf + r->offset + c->rx_position;
            const struct rndis_hdr *h = (const void *)p;
            if (remaining < sizeof *h || h->len < sizeof *h || h->len > remaining) {
                c->rx_invalid++;
                c->rx_range++;
                c->rx_position = 0;
                messages++;
                continue;
            }
            uint32_t len = h->len;
            if (h->type == RNDIS_PACKET) frames++;
            rndis_receive(c, p, len, input);
            c->rx_position += len;
            bytes += len;
            messages++;
            continue;
        }
        uint32_t r = c->ch->in->read_index, w = c->ch->in->write_index;
        uint32_t used = w >= r ? w - r : c->ch->in_size - r + w;
        c->rx_ring_high_water = MAX(c->rx_ring_high_water, used);
        int n = vmbus_recv(c->ch, c->rx_packet, c->rx_capacity);
        if (!n) break;
        messages++;
        if (n < 0) { c->rx_invalid++; continue; }
        struct vmpacket_desc *d = (void *)c->rx_packet;
        if (d->type == VM_PKT_DATA_XFER_PAGES) {
            /* The descriptor allocation is as large as the entire input
               ring. No accepted transfer ID is lost to a 4 KiB scratch cap. */
            c->rx_len = (uint32_t)n;
            c->rx_range = c->rx_position = 0;
            c->rx_borrowed = true;
            struct xfer_page_header *x = (void *)c->rx_packet;
            c->completion_wait = c->rx_len < sizeof *x || x->pageset_id != RECV_BUF_ID ||
                d->offset8 > d->len8 ||
                sizeof *x + (uint64_t)x->range_count * sizeof(struct xfer_range) > (uint32_t)d->offset8 * 8;
            if (c->completion_wait) c->rx_invalid++;
            continue;
        }
        if (d->type != VM_PKT_COMP) continue;
        if (d->offset8 < sizeof *d / 8 || d->offset8 > d->len8) { c->rx_invalid++; continue; }
        uint32_t dlen = vmpacket_datalen(c->rx_packet);
        uint32_t *m = vmpacket_data(c->rx_packet);
        if (dlen < 4) { c->rx_invalid++; continue; }
        if (m[0] == NVSP_SEND_RNDIS_PKT_COMPLETE) {
            if (d->trans_id >= TX_TID_BASE && d->trans_id - TX_TID_BASE < c->sections) {
                if (section_free(c, (uint32_t)(d->trans_id - TX_TID_BASE))) {
                    keepalive_blocked = false;
                    net_rx_notify();
                }
            }
        } else if (m[0] == c->nvsp_wait_type && !c->nvsp_done) {
            memset(c->nvsp_reply, 0, sizeof c->nvsp_reply);
            memcpy(c->nvsp_reply, m, MIN(dlen, sizeof c->nvsp_reply));
            c->nvsp_done = true;
        }
    }
    budget_exhausted |= messages >= RX_MESSAGE_BUDGET || bytes >= RX_BYTE_BUDGET || frames >= budget;
    if (c->detaching || c->ch->rescinded) return frames;
    /* Return the last transfer promptly, even when its final packet exactly
       exhausted the processing budget. A full ring keeps the borrow intact. */
    if (c->rx_borrowed && !c->completion_wait) {
        struct xfer_page_header *x = (void *)c->rx_packet;
        if (c->rx_range == x->range_count ||
            (c->rx_range + 1 == x->range_count && c->rx_position == x->range[c->rx_range].len))
            c->completion_wait = true;
    }
    if (c->completion_wait && !completion_blocked) complete_receive(c);
    c->ch->in->interrupt_mask = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Rearm before the final readiness check; an arrival crossing the empty
       check cannot be lost: the callback, on CPU 0 at any point of this, sets
       event_pending before it wakes the net thread, so either the check below
       sees the arrival or the callback's wake comes after it. A blocked
       completion waits for host progress. */
    bool pending = netvsc_rx_pending(&c->nif, 0);
    if (pending) {
        if (budget_exhausted) c->rx_budget_yields++;
        net_rx_notify();
    }
    return frames;
}

static size_t netvsc_poll(struct netif *self, unsigned queue, size_t budget, net_input_fn input) {
    (void)queue;
    return receive((struct netvsc *)self, budget, input);
}

static void netvsc_get_stats(struct netif *self, struct n_netstats *stats) {
    struct netvsc *c = (struct netvsc *)self;
    stats->rx_dropped = c->rx_dropped;
    stats->rx_invalid = c->rx_invalid;
    stats->rx_budget_yields = c->rx_budget_yields;
    stats->rx_completion_pending = c->rx_borrowed ? 1 : 0;
    stats->rx_completion_retries = c->rx_completion_retries;
    stats->rx_completion_cancelled = c->rx_completion_cancelled;
    stats->rx_ring_high_water = c->rx_ring_high_water;
}

/* The probe, before registration: read the channel until *flag, or until the deadline (false).
   Between reads it sleeps until the host's next signal (netvsc_signal): a packet, or room for the
   completion a full outbound ring refused (vmbus asks the host for that through pending_send_sz). */
static bool netvsc_wait(struct netvsc *c, volatile bool *flag, uint64_t deadline) {
    while (!*flag) {
        if (!c->ch || c->ch->rescinded || clock_now_ns() >= deadline) return false;
        uint64_t seen = __atomic_load_n(&c->signals, __ATOMIC_SEQ_CST);
        receive(c, SIZE_MAX, NULL);
        if (*flag || !c->ch) break;
        if ((c->rx_borrowed && !c->completion_wait) || vmbus_rx_pending(c->ch) ||
            (c->completion_wait && vmbus_write_ready(c->ch, sizeof(struct vmpacket_desc) + NVSP_MSG_SIZE)))
            sched_yield();
        else wq_wait_unless(&c->signal_wq, &c->signals, seen, deadline);
    }
    return *flag && c->ch && !c->detaching && !c->ch->rescinded;
}

/* ---- setup ---- */

/* Send an NVSP message and wait for the completion of type `reply`. */
static bool nvsp_request(struct netvsc *c, const void *msg, uint32_t reply) {
    uint8_t m[NVSP_MSG_SIZE];
    memcpy(m, msg, sizeof m);
    c->nvsp_done = false;
    c->nvsp_wait_type = reply;
    if (vmbus_send(c->ch, VM_PKT_DATA_INBAND, VMBUS_FLAG_COMPLETION_REQUESTED, m, sizeof m, 1)) return false;
    if (!netvsc_wait(c, &c->nvsp_done, clock_after(SETUP_REPLY_WAIT))) {
        kprintf("netvsc: no reply to NVSP message %u\n", *(uint32_t *)m);
        return false;
    }
    return true;
}

static bool nvsp_negotiate(struct netvsc *c) {
    static const uint32_t versions[] = {0x60001, 0x60000, 0x50000, 0x40000, 0x30002, 0x2};
    for (size_t i = 0; i < ARRAY_SIZE(versions); i++) {
        uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_INIT, versions[i], versions[i]};
        if (!nvsp_request(c, m, NVSP_INIT_COMPLETE)) return false;
        uint32_t *r = (uint32_t *)c->nvsp_reply;
        if (r[3] == NVSP_STATUS_SUCCESS) {
            c->nvsp_version = versions[i];
            return true;
        }
    }
    kprintf("netvsc: no NVSP version accepted\n");
    return false;
}

static bool setup_buffers(struct netvsc *c) {
    if (c->nvsp_version >= 0x2) {
        /* mtu + ethernet header, then the capability bits: 802.1q */
        uint32_t m[NVSP_MSG_SIZE / 4] = {NVSP_SEND_NDIS_CONFIG, 1514, 0, 1u << 3, 0};
        if (vmbus_send(c->ch, VM_PKT_DATA_INBAND, 0, m, sizeof m, 0)) return false;
    }
    uint32_t ndis = c->nvsp_version > 0x40000 ? 0x6001E : 0x60001; /* NDIS 6.30 or 6.1 */
    uint32_t v[NVSP_MSG_SIZE / 4] = {NVSP_SEND_NDIS_VERSION, ndis >> 16, ndis & 0xFFFF};
    if (vmbus_send(c->ch, VM_PKT_DATA_INBAND, 0, v, sizeof v, 0)) return false;

    uint64_t gpa;
    c->recv_size = RECV_BUF_PAGES * PAGE_SIZE;
    c->recv_buf = hal_dma_alloc(c->hal, c->recv_size, &gpa);
    if (!c->recv_buf) return false;
    uint32_t g = vmbus_gpadl_create(c->ch, gpa, RECV_BUF_PAGES);
    if (!g) return false;
    uint32_t rb[NVSP_MSG_SIZE / 4] = {NVSP_SEND_RECV_BUF, g, RECV_BUF_ID};
    if (!nvsp_request(c, rb, NVSP_SEND_RECV_BUF_COMPLETE)) return false;
    if (((uint32_t *)c->nvsp_reply)[1] != NVSP_STATUS_SUCCESS) {
        kprintf("netvsc: receive buffer refused (%u)\n", ((uint32_t *)c->nvsp_reply)[1]);
        return false;
    }

    c->send_buf = hal_dma_alloc(c->hal, SEND_BUF_PAGES * PAGE_SIZE, &gpa);
    if (!c->send_buf) return false;
    g = vmbus_gpadl_create(c->ch, gpa, SEND_BUF_PAGES);
    if (!g) return false;
    uint32_t sb[NVSP_MSG_SIZE / 4] = {NVSP_SEND_SEND_BUF, g, SEND_BUF_ID};
    if (!nvsp_request(c, sb, NVSP_SEND_SEND_BUF_COMPLETE)) return false;
    uint32_t *r = (uint32_t *)c->nvsp_reply;
    if (r[1] != NVSP_STATUS_SUCCESS || r[2] < 2048) {
        kprintf("netvsc: send buffer refused (%u, section %u)\n", r[1], r[2]);
        return false;
    }
    c->section_size = r[2];
    c->sections = SEND_BUF_PAGES * PAGE_SIZE / c->section_size;
    if (!c->sections) return false;
    c->section_used = kzalloc((c->sections + 63) / 64 * 8);
    return c->section_used != NULL;
}

/* Send an RNDIS control message (its req_id at offset 8) and wait for the matching completion. */
static bool rndis_request(struct netvsc *c, void *msg, uint32_t len) {
    uint32_t *m = msg;
    m[2] = ++c->next_req_id;
    c->rndis_done = false;
    c->rndis_wait_id = m[2];
    if (send_rndis(c, msg, len, true)) return false;
    if (!netvsc_wait(c, &c->rndis_done, clock_after(SETUP_REPLY_WAIT))) {
        kprintf("netvsc: no reply to RNDIS message %u\n", m[0]);
        return false;
    }
    return ((uint32_t *)c->rndis_reply)[3] == 0; /* RNDIS_STATUS_SUCCESS */
}

/* Query an OID; returns the length copied into out, or -1. */
static int rndis_query(struct netvsc *c, uint32_t oid, void *out, uint32_t outlen) {
    struct rndis_oid_req q = {{RNDIS_QUERY, sizeof q}, 0, oid, 0, 20, 0};
    if (!rndis_request(c, &q, sizeof q)) return -1;
    uint32_t *r = (uint32_t *)c->rndis_reply;
    uint32_t blen = r[4], boff = r[5];
    if (8ull + boff + blen > sizeof c->rndis_reply || 8ull + boff + blen > r[1]) return -1;
    uint32_t n = MIN(blen, outlen);
    memcpy(out, c->rndis_reply + 8 + boff, n);
    return (int)n;
}

static bool rndis_set(struct netvsc *c, uint32_t oid, const void *val, uint32_t len) {
    uint8_t buf[sizeof(struct rndis_oid_req) + 16];
    struct rndis_oid_req *s = (void *)buf;
    if (len > 16) return false;
    *s = (struct rndis_oid_req){{RNDIS_SET, sizeof *s + len}, 0, oid, len, 20, 0};
    memcpy(buf + sizeof *s, val, len);
    return rndis_request(c, buf, sizeof *s + len);
}

/* A batch of frames into free send sections, each announced to the host; the rest waits for a
   section's completion (net_rx_notify). */
static unsigned netvsc_transmit(struct netif *self, unsigned queue, const struct net_tx_frame *frames,
                                unsigned count) {
    (void)queue;
    struct netvsc *c = (struct netvsc *)self;
    unsigned taken = 0;
    while (taken < count) {
        const struct net_tx_frame *f = &frames[taken];
        if (!c->ch || c->detaching || c->ch->rescinded || f->length > 1514 ||
            sizeof(struct rndis_packet) + f->length > c->section_size) {
            taken++; /* never sendable: dropped */
            continue;
        }
        uint32_t sec = section_alloc(c);
        if (sec == INVALID_SECTION) {
            c->tx_backpressure++;
            break;
        }
        uint8_t *p = c->send_buf + (size_t)sec * c->section_size;
        struct rndis_packet *rp = (void *)p;
        memset(rp, 0, sizeof *rp);
        rp->h.type = RNDIS_PACKET;
        rp->h.len = (uint32_t)(sizeof *rp + f->length);
        rp->data_offset = sizeof *rp - 8;
        rp->data_len = (uint32_t)f->length;
        size_t at = sizeof *rp;
        for (unsigned i = 0; i < f->part_count; i++) {
            memcpy(p + at, f->parts[i].data, f->parts[i].length);
            at += f->parts[i].length;
        }
        int r = send_section(c, sec, rp->h.len, false);
        if (r) {
            section_free(c, sec);
            if (r == -N_ERR_NOT_READY) {
                c->tx_backpressure++;
                break;
            }
        }
        taken++; /* sent, or refused for good */
    }
    return taken;
}

/* The adapters that are up, for netvsc_ip. */
static struct spinlock live_lock;
static struct netvsc *live;

bool netvsc_ip(uint32_t *ip) {
    uint8_t mac[6], other[6];
    uint32_t mask;
    uint64_t f = spin_lock(&live_lock);
    if (!live) {
        spin_unlock(&live_lock, f);
        return false;
    }
    memcpy(mac, live->nif.mac, 6);
    spin_unlock(&live_lock, f);
    for (size_t i = 0; net_adapter_info(i, other, ip, &mask); i++)
        if (!memcmp(mac, other, 6)) return true;
    return false;
}

static void live_add(struct netvsc *c) {
    uint64_t f = spin_lock(&live_lock);
    c->next = live;
    live = c;
    spin_unlock(&live_lock, f);
}

static void live_remove(struct netvsc *c) {
    uint64_t f = spin_lock(&live_lock);
    for (struct netvsc **pp = &live; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    spin_unlock(&live_lock, f);
}

static int netvsc_attach(struct hal_device *d) {
    struct vmbus_channel *ch = vmbus_channel_of(d);
    struct netvsc *c = kzalloc(sizeof *c);
    if (!c) return -N_ERR_NO_MEMORY;
    d->driver_data = c;
    c->nif.driver = "netvsc";
    c->ch = ch;
    c->hal = d;
    hal_dma_bits(d, 64); /* GPADL の頁の番号は 64 bit */
    if (vmbus_open(ch, 128 * 1024, 128 * 1024, netvsc_callback, c)) goto failed;
    vmbus_set_rescind(ch, netvsc_rescind);
    if (ch->rescinded) c->detaching = true;
    c->rx_capacity = ch->in_size;
    c->rx_packet = kmalloc(c->rx_capacity);
    if (!c->rx_packet || !nvsp_negotiate(c) || !setup_buffers(c)) goto failed;

    struct {
        struct rndis_hdr h;
        uint32_t req_id, major, minor, max_xfer;
    } PACKED init = {{RNDIS_INIT, sizeof init}, 0, 1, 0, 0x4000};
    if (!rndis_request(c, &init, sizeof init)) {
        kprintf("netvsc: RNDIS initialisation failed\n");
        goto failed;
    }
    if (rndis_query(c, OID_802_3_PERMANENT_ADDRESS, c->nif.mac, 6) != 6) {
        kprintf("netvsc: could not read the MAC address\n");
        goto failed;
    }
    uint32_t media = 1;
    if (rndis_query(c, OID_GEN_MEDIA_CONNECT_STATUS, &media, 4) == 4) c->link_up = media == 0;
    uint32_t filter = PACKET_FILTER;
    if (!rndis_set(c, OID_GEN_CURRENT_PACKET_FILTER, &filter, 4)) {
        kprintf("netvsc: could not set the packet filter\n");
        goto failed;
    }
    c->nif.tx_queues = 1;
    c->nif.rx_queues = 1;
    c->nif.transmit = netvsc_transmit;
    c->nif.poll = netvsc_poll;
    c->nif.rx_pending = netvsc_rx_pending;
    c->nif.get_stats = netvsc_get_stats;
    kprintf("net: netvsc (NVSP %x.%x, %u send sections of %u) mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            c->nvsp_version >> 16, c->nvsp_version & 0xFFFF, c->sections, c->section_size, c->nif.mac[0],
            c->nif.mac[1], c->nif.mac[2], c->nif.mac[3], c->nif.mac[4], c->nif.mac[5], c->link_up ? "up" : "down");
    live_add(c);
    net_register(d, &c->nif);
    return 0;
failed:
    netvsc_shutdown(c);
    d->driver_data = NULL;
    kfree(c);
    return -N_ERR_IO;
}

/* The host withdrew the channel (or the device is removed): the stack stops calling first, then the
   channel closes. */
static void netvsc_detach(struct hal_device *d) {
    struct netvsc *c = d->driver_data;
    live_remove(c);
    net_unregister(&c->nif);
    netvsc_shutdown(c);
    d->driver_data = NULL;
    kfree(c);
}

static const struct n_hal_rule netvsc_rules[] = {N_VMBUS_TYPE(net_guid.b)};
const struct hal_driver netvsc_driver = {
    .name = "netvsc",
    .rules = netvsc_rules,
    .rule_count = ARRAY_SIZE(netvsc_rules),
    .attach = netvsc_attach,
    .detach = netvsc_detach,
};
