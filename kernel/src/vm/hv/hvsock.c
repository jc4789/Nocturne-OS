/* Hyper-V sockets (the guest side of AF_HYPERV / Linux's hv_sock). When the host connects to a
   service in the guest, it offers a new VMBus channel whose type GUID is the service ID; the guest
   accepts by opening the channel. Each packet on it carries a small pipe header and a piece of
   the byte stream; a packet with no data marks the end of the stream. When either side is done,
   the guest closes the channel and the host rescinds it. Only host-to-guest connections exist
   here: that is all a server needs. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "vm/hv/hv.h"
#include "vm/hv/hvsock.h"
#include "vm/hv/vmbus.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* vsock-style service IDs: <port>-facb-11e6-bd58-64006a7986d3 */
static const struct guid port_template =
    GUID_INIT(0x00000000, 0xfacb, 0x11e6, 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3);

#define SEND_RING (256 * 1024)
#define RECV_RING (64 * 1024)
#define MAX_SEND_DATA (4096 - 8) /* data per packet, as Linux sends: a page with the pipe header */
#define SEND_GIVE_UP (10 * NS_PER_S) /* a host that makes no room for a packet this long is gone */

struct pipe_header {
    uint32_t type; /* 1: data */
    uint32_t size;
} PACKED;

/* A socket is its user's task's; the channel's callbacks (CPU 0's handler) only move signals on
   and wake wq, and a rescind sets rescinded first. A waiter reads signals, looks, and sleeps
   unless it moved (wq_wait_unless). */
struct hvsock {
    struct vmbus_channel *ch;
    struct wait_queue wq;
    uint64_t signals;
    bool rescinded; /* atomic */
    bool eof;
    uint8_t *pkt; /* the packet being read */
    uint32_t pkt_cap, off, end;
};

/* The listeners and the offers waiting for the hvsock thread: hvsock_listen, the offer hook (CPU
   0's handler, under the VMBus channel lock) and the thread. Only a wait queue's lock under it. */
static struct spinlock hvsock_lock;

#define MAX_LISTEN 4
static struct {
    uint32_t port;
    void (*accept)(struct hvsock *s, void *arg);
    void *arg;
} listeners[MAX_LISTEN];
static int nlisteners;

/* offers waiting for the hvsock thread to open them */
#define PENDING 16
static struct vmbus_channel *pending[PENDING];
static int pend_head, pend_tail;
static struct wait_queue thread_wq;

static bool socket_rescinded(struct hvsock *s) { return __atomic_load_n(&s->rescinded, __ATOMIC_ACQUIRE); }

/* hvsock_lock held. */
static int listener_for(const struct guid *service) {
    if (memcmp(service->b + 4, port_template.b + 4, 12)) return -1;
    uint32_t port;
    memcpy(&port, service->b, 4);
    for (int i = 0; i < nlisteners; i++)
        if (listeners[i].port == port) return i;
    return -1;
}

/* CPU 0's handler, under the VMBus channel lock: a socket channel was offered. True keeps it. */
static bool socket_offer(struct vmbus_channel *ch) {
    spin_take(&hvsock_lock);
    int next = (pend_head + 1) % PENDING;
    /* nobody listens: the host rescinds it after a while */
    bool keep = listener_for(&ch->type) >= 0 && next != pend_tail;
    if (keep) {
        pending[pend_head] = ch;
        pend_head = next;
        wq_wake_all(&thread_wq);
    }
    spin_give(&hvsock_lock);
    return keep;
}

/* CPU 0's handler: the host wrote, read, or withdrew the channel. */
static void sock_signal(struct hvsock *s) {
    __atomic_fetch_add(&s->signals, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&s->wq);
}

static void sock_callback(struct vmbus_channel *ch) { sock_signal(ch->priv); }

/* Set before the open, which hands the channel its priv: a rescind before it has no socket. */
static void sock_rescind(struct vmbus_channel *ch) {
    struct hvsock *s = ch->priv;
    if (!s) return;
    __atomic_store_n(&s->rescinded, true, __ATOMIC_RELEASE);
    sock_signal(s);
}

static void accept_channel(struct vmbus_channel *ch) {
    uint64_t f = spin_lock(&hvsock_lock);
    int l = listener_for(&ch->type);
    void (*accept)(struct hvsock *s, void *arg) = l >= 0 ? listeners[l].accept : NULL;
    void *accept_arg = l >= 0 ? listeners[l].arg : NULL;
    spin_unlock(&hvsock_lock, f);
    struct hvsock *s = kzalloc(sizeof *s);
    if (s) s->pkt = kmalloc(RECV_RING);
    if (!accept || !s || !s->pkt || __atomic_load_n(&ch->rescinded, __ATOMIC_ACQUIRE)) goto fail;
    s->ch = ch;
    s->pkt_cap = RECV_RING;
    vmbus_set_rescind(ch, sock_rescind);
    if (vmbus_open(ch, SEND_RING, RECV_RING, sock_callback, s)) goto fail;
    accept(s, accept_arg);
    return;
fail:
    vmbus_release(ch); /* from here on neither sock_rescind nor sock_callback runs for s */
    if (s) kfree(s->pkt);
    kfree(s);
}

static void hvsock_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock(&hvsock_lock);
        while (pend_tail == pend_head) {
            wq_wait_locked(&thread_wq, &hvsock_lock);
            spin_take(&hvsock_lock);
        }
        struct vmbus_channel *ch = pending[pend_tail];
        pend_tail = (pend_tail + 1) % PENDING;
        spin_unlock(&hvsock_lock, f);
        accept_channel(ch);
    }
}

int hvsock_listen(uint32_t port, void (*accept)(struct hvsock *s, void *arg), void *arg) {
    if (!vmbus_ready()) return -N_ERR_IO;
    uint64_t f = spin_lock(&hvsock_lock);
    int r = 0;
    if (nlisteners == MAX_LISTEN) {
        r = -N_ERR_NO_MEMORY;
    } else {
        listeners[nlisteners].accept = accept;
        listeners[nlisteners].arg = arg;
        listeners[nlisteners++].port = port;
    }
    spin_unlock(&hvsock_lock, f);
    return r;
}

/* Make the next stream bytes available in pkt[off..end). False if there are none yet. */
static bool fill(struct hvsock *s) {
    while (s->off >= s->end && !s->eof) {
        int n = vmbus_recv(s->ch, s->pkt, s->pkt_cap);
        if (n == 0) return socket_rescinded(s);
        if (n < 0) continue;
        struct vmpacket_desc *d = (void *)s->pkt;
        uint32_t dl = vmpacket_datalen(s->pkt);
        struct pipe_header *h = vmpacket_data(s->pkt);
        if (d->type != VM_PKT_DATA_INBAND || dl < sizeof *h) continue;
        if (h->size == 0) {
            s->eof = true;
            break;
        }
        s->off = (uint32_t)((uint8_t *)(h + 1) - s->pkt);
        s->end = s->off + MIN(h->size, dl - (uint32_t)sizeof *h);
    }
    return true;
}

bool hvsock_wait(struct hvsock *s, uint64_t deadline) {
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->signals, __ATOMIC_SEQ_CST);
        if (fill(s)) return true;
        if (clock_now_ns() >= deadline) return false;
        wq_wait_unless(&s->wq, &s->signals, seen, deadline);
    }
}

int hvsock_read(struct hvsock *s, void *buf, size_t len, uint64_t deadline) {
    if (!hvsock_wait(s, deadline)) return -N_ERR_TIMEOUT;
    if (s->off >= s->end) return 0; /* end of stream */
    uint32_t n = (uint32_t)MIN(len, (size_t)(s->end - s->off));
    memcpy(buf, s->pkt + s->off, n);
    s->off += n;
    return (int)n;
}

int hvsock_read_full(struct hvsock *s, void *buf, size_t len, uint64_t deadline) {
    uint8_t *p = buf;
    while (len) {
        int n = hvsock_read(s, p, len, deadline);
        if (n < 0) return n;
        if (n == 0) return -N_ERR_CLOSED;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_packet(struct hvsock *s, const void *data, uint32_t n) {
    struct {
        struct vmpacket_desc d;
        struct pipe_header h;
    } PACKED hdr = {{VM_PKT_DATA_INBAND, 2, (uint16_t)((sizeof hdr + n + 7) / 8), 0, 0}, {1, n}};
    uint64_t give_up = clock_after(SEND_GIVE_UP);
    for (;;) {
        uint64_t seen = __atomic_load_n(&s->signals, __ATOMIC_SEQ_CST);
        if (socket_rescinded(s)) return -N_ERR_CLOSED;
        int r = vmbus_send_raw(s->ch, &hdr, sizeof hdr, data, n);
        if (r != -N_ERR_NOT_READY) return r;
        if (clock_now_ns() >= give_up) return -N_ERR_TIMEOUT;
        /* full: the ring has asked the host (pending_send_sz) to signal once it has made room,
           and the signal moves s->signals */
        wq_wait_unless(&s->wq, &s->signals, seen, give_up);
    }
}

int hvsock_write(struct hvsock *s, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        uint32_t n = (uint32_t)MIN(len, (size_t)MAX_SEND_DATA);
        int r = send_packet(s, p, n);
        if (r) return r;
        p += n;
        len -= n;
    }
    return 0;
}

void hvsock_close(struct hvsock *s) {
    if (!socket_rescinded(s)) send_packet(s, NULL, 0); /* end of stream */
    vmbus_close(s->ch);
    vmbus_release(s->ch);
    kfree(s->pkt);
    kfree(s);
}

void hvsock_init(void) {
    if (!vmbus_ready()) return;
    vmbus_set_socket_hook(socket_offer);
    kthread_create("hvsock", hvsock_thread, NULL);
}
