/* Hyper-V sockets (the guest side of AF_HYPERV / Linux's hv_sock). When the host connects to a
   service in the guest, it offers a new VMBus channel whose type GUID is the service ID; the guest
   accepts by opening the channel. Each packet on it carries a small pipe header and a piece of
   the byte stream; a packet with no data marks the end of the stream. When either side is done,
   the guest closes the channel and the host rescinds it. Only host-to-guest connections exist
   here: that is all a server needs. */
#include "kernel.h"
#include "arch/cpu.h"
#include "hv/hv.h"
#include "hv/hvsock.h"
#include "hv/vmbus.h"
#include "mm/heap.h"
#include "sys/sched.h"

/* vsock-style service IDs: <port>-facb-11e6-bd58-64006a7986d3 */
static const struct guid port_template =
    GUID_INIT(0x00000000, 0xfacb, 0x11e6, 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3);

#define SEND_RING (256 * 1024)
#define RECV_RING (64 * 1024)
#define MAX_SEND_DATA (4096 - 8) /* data per packet, as Linux sends: a page with the pipe header */

struct pipe_header {
    uint32_t type; /* 1: data */
    uint32_t size;
} PACKED;

struct hvsock {
    struct vmbus_channel *ch;
    struct wait_queue wq;
    volatile bool rescinded;
    bool eof;
    uint8_t *pkt; /* the packet being read */
    uint32_t pkt_cap, off, end;
};

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
static volatile int pend_head, pend_tail;
static struct wait_queue thread_wq;

static int listener_for(const struct guid *service) {
    if (memcmp(service->b + 4, port_template.b + 4, 12)) return -1;
    uint32_t port;
    memcpy(&port, service->b, 4);
    for (int i = 0; i < nlisteners; i++)
        if (listeners[i].port == port) return i;
    return -1;
}

/* interrupt context: a socket channel was offered */
static void socket_offer(struct vmbus_channel *ch) {
    if (listener_for(&ch->type) < 0) return; /* nobody listens: the host rescinds it after a while */
    int next = (pend_head + 1) % PENDING;
    if (next == pend_tail) return;
    ch->claimed = true;
    pending[pend_head] = ch;
    pend_head = next;
    wq_wake_all(&thread_wq);
}

static void sock_callback(struct vmbus_channel *ch) {
    struct hvsock *s = ch->priv;
    wq_wake_all(&s->wq);
}

static void sock_rescind(struct vmbus_channel *ch) {
    struct hvsock *s = ch->priv;
    s->rescinded = true;
    wq_wake_all(&s->wq);
}

static void accept_channel(struct vmbus_channel *ch) {
    int l = listener_for(&ch->type);
    struct hvsock *s = kzalloc(sizeof *s);
    if (s) s->pkt = kmalloc(RECV_RING);
    if (l < 0 || !s || !s->pkt || ch->rescinded) goto fail;
    s->ch = ch;
    s->pkt_cap = RECV_RING;
    ch->on_rescind = sock_rescind;
    if (vmbus_open(ch, SEND_RING, RECV_RING, sock_callback, s)) goto fail;
    listeners[l].accept(s, listeners[l].arg);
    return;
fail:
    if (s) kfree(s->pkt);
    kfree(s);
    vmbus_release(ch);
}

static void hvsock_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        if (pend_tail == pend_head) wq_wait(&thread_wq);
        irq_restore(f);
        while (pend_tail != pend_head) {
            struct vmbus_channel *ch = pending[pend_tail];
            pend_tail = (pend_tail + 1) % PENDING;
            accept_channel(ch);
        }
    }
}

int hvsock_listen(uint32_t port, void (*accept)(struct hvsock *s, void *arg), void *arg) {
    if (!vmbus_ready()) return -EIO;
    if (nlisteners == MAX_LISTEN) return -ENOMEM;
    listeners[nlisteners].accept = accept;
    listeners[nlisteners].arg = arg;
    __asm__ volatile("" ::: "memory");
    listeners[nlisteners++].port = port;
    return 0;
}

/* Make the next stream bytes available in pkt[off..end). False if there are none yet. */
static bool fill(struct hvsock *s) {
    while (s->off >= s->end && !s->eof) {
        int n = vmbus_recv(s->ch, s->pkt, s->pkt_cap);
        if (n == 0) return s->rescinded;
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

bool hvsock_wait(struct hvsock *s, uint64_t timeout_ms) {
    uint64_t end = uptime_ms() + timeout_ms;
    for (;;) {
        uint64_t f = irq_save();
        bool ready = fill(s);
        uint64_t now = uptime_ms();
        if (!ready && now < end) wq_wait_timeout(&s->wq, end - now);
        irq_restore(f);
        if (ready) return true;
        if (now >= end) return false;
    }
}

int hvsock_read(struct hvsock *s, void *buf, size_t len, uint64_t timeout_ms) {
    if (!hvsock_wait(s, timeout_ms)) return -ETIMEDOUT;
    if (s->off >= s->end) return 0; /* end of stream */
    uint32_t n = (uint32_t)MIN(len, (size_t)(s->end - s->off));
    memcpy(buf, s->pkt + s->off, n);
    s->off += n;
    return (int)n;
}

int hvsock_read_full(struct hvsock *s, void *buf, size_t len, uint64_t timeout_ms) {
    uint8_t *p = buf;
    while (len) {
        int n = hvsock_read(s, p, len, timeout_ms);
        if (n < 0) return n;
        if (n == 0) return -EPIPE;
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
    uint32_t need = (uint32_t)ALIGN_UP(sizeof hdr + n, 8) + 8;
    uint64_t give_up = uptime_ms() + 10000; /* a host that reads nothing for this long is gone */
    for (;;) {
        if (s->rescinded) return -EPIPE;
        if (uptime_ms() > give_up) return -ETIMEDOUT;
        int r = vmbus_send_raw(s->ch, &hdr, sizeof hdr, data, n);
        if (r != -EAGAIN) return r;
        /* full: ask the host to signal us once it has made room, then wait */
        uint64_t f = irq_save();
        s->ch->out->pending_send_sz = need;
        hv_mb();
        r = vmbus_send_raw(s->ch, &hdr, sizeof hdr, data, n);
        if (r == -EAGAIN) wq_wait_timeout(&s->wq, 100);
        s->ch->out->pending_send_sz = 0;
        irq_restore(f);
        if (r != -EAGAIN) return r;
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
    if (!s->rescinded) send_packet(s, NULL, 0); /* end of stream */
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
