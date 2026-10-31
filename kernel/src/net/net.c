/* Nocturne network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and a small TCP client.

   Any CPU. Received frames wait in the NIC's own ring (poll_rx) or in the receive queue below,
   filled from interrupt handlers and driver threads; the "net" kernel thread takes them in.
   System calls run the protocol themselves in the calling task, and sleep on their socket's
   progress (or shared_progress for link, ARP, DNS and UDP state) until a change or their
   deadline. Every time here is on the clock's axis (sys/clock.h): a moment is clock_now_ns, a
   deadline the moment it ends (CLOCK_NEVER: none). Nothing polls: the net thread sleeps until a
   frame, a driver's notice or one of its timers (struct net_timer) wakes it.

   Locks, the first before the later ones:
   - net_lock (a mutex): every piece of protocol state (sockets and their rings, ARP, DHCP, DNS,
     pings, the output frame, the timers' moments, the counters) and every call into the adapter
     (struct netif). Held by a system call from entry to return, given back while it sleeps; held
     by the net thread for one batch at a time. Allocations, copies of the app's memory and the
     adapter's own locks happen under it. Never taken by an interrupt handler or an alarm's fire.
   - a NIC driver's own lock (its rings and its interrupt state; see the driver).
   - rxq_lock, config_lock (spinlocks): leaves, nothing but a wait queue's lock under them, so an
     interrupt handler, an alarm's fire or any driver context may take them. */
#include "kernel.h"
#include "abi.h"
#include "net.h"
#include "../dev/entropy.h"
#include "../arch/cpu.h"
#include "../arch/smp.h"
#include "../arch/spin.h"
#include "../mm/heap.h"
#include "../sys/alarm.h"
#include "../sys/clock.h"
#include "../sys/process.h"
#include "../sys/sched.h"
#include "../sys/syscall.h"
#include "../sys/object.h"
#include "../sys/thread.h"
#include "../sys/wait.h"
#include "../vm/vm.h"
#include "../sys/channel.h"
#include "dns_name.h"

static struct mutex net_lock;

/* A condition the stack changes under net_lock, which its waiters sleep on without the lock:
   whoever changes it moves generation on before it wakes wq, and a waiter that read generation
   under the lock sleeps only while it still reads that (wq_wait_unless), so a change between its
   look and its sleep is never missed. */
struct net_progress {
    struct wait_queue wq;
    uint64_t generation;
};
static void progress_wake(struct net_progress *p) {
    __atomic_add_fetch(&p->generation, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&p->wq);
}

/* The adapter, its address and the IPv4 configuration: written under both net_lock and
   config_lock, read under either (net_config, from any context, takes config_lock). */
static struct spinlock config_lock;
static struct netif *nif;
static uint8_t link_mac[6];
static const char *link_driver;
static uint32_t my_ip, my_mask, my_gw, my_dns;
static bool configured;
static struct n_sockaddr dns_boot_resolver; /* set once by net_init, before the stack starts */
static int dns_boot_setting; /* 0=DHCP, 1=explicit, negative=rejected boot setting */
static struct n_sockaddr dns_default_resolver(void);

static uint64_t rx_packets, tx_packets;
static uint64_t net_service_bytes; /* wire bytes in and out: the net thread's service window */
static struct net_progress shared_progress; /* link/ARP/DNS/UDP progress */
static bool receive_batch; /* the net thread is taking in a batch: socket wakes wait for its end */

/* Tasks other than the net thread that wait for net_lock, and the net thread's wish for one of
   them to have it before its next batch (net_thread). */
static unsigned net_callers_waiting;
static bool net_turn_wanted;
static struct net_progress net_turn;

/* A system call (or another task) enters the stack. */
static void net_enter(void) {
    __atomic_add_fetch(&net_callers_waiting, 1, __ATOMIC_SEQ_CST);
    mutex_lock(&net_lock);
    __atomic_sub_fetch(&net_callers_waiting, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&net_turn_wanted, false, __ATOMIC_SEQ_CST)) progress_wake(&net_turn);
}
static void net_leave(void) { mutex_unlock(&net_lock); }

/* The retained handle identifies a channel end, independently of handle number
   reuse; its peer closing cancels. Only setup paths consult it: a cancelled HTTP/2
   stream must never abort a shared established TCP connection while pumping
   another stream's records. The handle is the calling thread's own. */
static bool net_setup_cancelled(void) {
    struct handle *h=thread_current()->net_setup_cancel;
    return h && (handle_ready(h)&N_EVENT_CLOSED);
}

/* A setup wait's watch of its cancel channel's end (net_sleep), a callback item on the end's
   object's waiters: a change to the end (its peer's close among them) has the sleepers on progress
   look again. Called under the end's waiters' lock, in any context (sys/wait.h): progress_wake
   takes only progress's wait queue's lock after it. */
struct setup_cancel_watch {
    struct wait_item item;
    struct net_progress *progress;
};
static void setup_cancel_told(struct wait_item *item) {
    progress_wake(((struct setup_cancel_watch *)item)->progress);
}

/* net_lock held once: sleep on p until deadline without the lock, which is held again on return.
   False when the deadline came first. Every reference into the stack's state is looked at again
   after: the socket may have closed and its slot may hold another connection. A kill wakes the
   sleeper (sys/wait.h), so a wait without a deadline needs no limit of its own. setup: a setup
   wait, which the thread's cancel channel's close (net_setup_cancelled) ends as well: the channel's
   end is watched from the generation's reading on, and looked at once it is, so a close before
   the watch is seen and one after it moves the generation on. */
static bool net_sleep(struct net_progress *p, uint64_t deadline, bool setup) {
    ASSERT(mutex_held(&net_lock) && net_lock.depth == 1);
    uint64_t seen = __atomic_load_n(&p->generation, __ATOMIC_SEQ_CST);
    struct handle *cancel = setup ? thread_current()->net_setup_cancel : NULL;
    struct wait_queue *told = cancel ? &cancel->object->waiters : NULL;
    struct setup_cancel_watch watch = {.item = {.callback = setup_cancel_told}, .progress = p};
    bool ready = true;
    uint64_t f;
    if (told) {
        f = spin_lock(&told->lock);
        wq_add(told, &watch.item);
        spin_unlock(&told->lock, f);
    }
    if (!told || !net_setup_cancelled()) {
        net_leave();
        ready = wq_wait_unless(&p->wq, &p->generation, seen, deadline);
        net_enter();
    }
    if (told) {
        f = spin_lock(&told->lock);
        wq_remove(told, &watch.item);
        spin_unlock(&told->lock, f);
    }
    return ready;
}
/* The time from then to now, two moments read on any CPUs (which may disagree by a few ns). */
static uint64_t ns_between(uint64_t then, uint64_t now) { return now>then?now-then:0; }

static uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
#define ntohs htons
static uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
#define ntohl htonl
static uint32_t ip_get(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }
static void ip_put(uint8_t *p, uint32_t ip) {
    p[0] = (uint8_t)(ip >> 24);
    p[1] = (uint8_t)(ip >> 16);
    p[2] = (uint8_t)(ip >> 8);
    p[3] = (uint8_t)ip;
}
static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint64_t rng_state;
static uint32_t rnd(void) {
    if (!rng_state) entropy_get(&rng_state, sizeof rng_state);
    rng_state |= 1;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)rng_state;
}

static uint32_t csum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = data;
    uint64_t wide = sum;
    /* RFC 1071: associative one's-complement addition. Builtin memcpy permits an
       unaligned, alias-safe machine-word load; network order is restored
       before adding independent pairs. Explicit builtins retain native loads
       under -ffreestanding. No per-word carry-fold is needed. */
    while (len >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, p, sizeof word);
        word = __builtin_bswap64(word);
        const uint64_t pair_mask = 0x0000ffff0000ffffULL;
        uint64_t pairs = (word & pair_mask) + ((word >> 16) & pair_mask);
        wide += (uint32_t)pairs + (pairs >> 32);
        p += sizeof word;
        len -= sizeof word;
    }
    if (len >= sizeof(uint32_t)) {
        uint32_t word;
        __builtin_memcpy(&word, p, sizeof word);
        word = __builtin_bswap32(word);
        wide += (word & UINT16_MAX) + (word >> 16);
        p += sizeof word;
        len -= sizeof word;
    }
    if (len >= sizeof(uint16_t)) { wide += (uint32_t)p[0] << 8 | p[1]; p += 2; len -= 2; }
    if (len) wide += (uint32_t)p[0] << 8;
    while (wide >> 32) wide = (wide & UINT32_MAX) + (wide >> 32);
    return (uint32_t)wide;
}
static uint16_t csum_fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static uint16_t l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *data, size_t len) {
    uint8_t ph[12];
    ip_put(ph, src);
    ip_put(ph + 4, dst);
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = (uint8_t)(len >> 8);
    ph[11] = (uint8_t)len;
    return csum_fold(csum_add(csum_add(0, ph, 12), data, len));
}

struct eth_hdr {
    uint8_t dst[6], src[6];
    uint16_t type;
} PACKED;
struct ip_hdr {
    uint8_t vihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint8_t src[4], dst[4];
} PACKED;
struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6], spa[4], tha[6], tpa[4];
} PACKED;
struct icmp_hdr {
    uint8_t type, code;
    uint16_t csum, id, seq;
} PACKED;
struct udp_hdr {
    uint16_t sport, dport, len, csum;
} PACKED;
struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
} PACKED;

/* ---------------------------------------------------------------- receive queue (drivers -> thread) */

/* Frames copied in by interrupt handlers, driver threads and the loopback output, taken in by
   the net thread, which reads the slot at the tail in place (a producer never writes it) and
   gives it back after. rxq_lock guards head, tail, the counters, the fired timers' queue below
   and net_work_pending, the net thread's wake latch (the conditions it sleeps on, rxq_wq). */
#define RXQ 64
static uint8_t rxq_buf[RXQ][1536];
static uint16_t rxq_len[RXQ];
static int rxq_head, rxq_tail;
static struct spinlock rxq_lock;
static struct wait_queue rxq_wq;
static bool net_work_pending;
static uint64_t rxq_dropped, rxq_invalid, rxq_high_water;

/* Work the net thread does at a moment of the clock: a TCP connection's next look, DHCP's next
   message, a held-back test segment. Its alarm (sys/alarm.h) fires in a timer interrupt, where
   net_lock, a mutex, cannot be taken: the fire queues it and wakes the thread, which runs it under
   net_lock. A timer is armed for no more than the earliest moment it is wanted by and only ever
   moved earlier (net_timer_at); one that fires before its work is due makes the thread look and
   arm it again for the moment it now has, so a moment that keeps moving later (a retransmission
   timer each ACK restarts) costs one look per period, not an alarm move per change. A timer's
   memory is never freed; one whose work has gone is cancelled (alarm_cancel) under net_lock,
   which no fire takes. One cancelled after it fired may still be queued: its run finds no work. */
struct net_timer {
    struct alarm alarm;                             /* first: the fire finds its timer from it */
    void (*run)(struct net_timer *nt, uint64_t now); /* the net thread, net_lock held */
    struct net_timer *due_next;                     /* rxq_lock: the queue of fired ones */
    bool due;
};
static struct net_timer *net_due_first, **net_due_last = &net_due_first;

static void net_timer_fire(struct alarm *a, struct regs *r) {
    (void)r;
    struct net_timer *nt = (struct net_timer *)a;
    spin_take(&rxq_lock);
    if (!nt->due) {
        nt->due = true;
        nt->due_next = NULL;
        *net_due_last = nt;
        net_due_last = &nt->due_next;
    }
    net_work_pending = true;
    spin_give(&rxq_lock);
    wq_wake_all(&rxq_wq);
}

static void net_timer_init(struct net_timer *nt, void (*run)(struct net_timer *, uint64_t)) {
    alarm_init(&nt->alarm, net_timer_fire);
    nt->run = run;
    nt->due_next = NULL;
    nt->due = false;
}

/* net_lock held: nt runs by at (in the past: at once; CLOCK_NEVER: not for this). net_lock
   orders every arming, so the alarm's moment is read here without the alarm's lock. */
static void net_timer_at(struct net_timer *nt, uint64_t at) {
    if (at == CLOCK_NEVER || (alarm_armed(&nt->alarm) && nt->alarm.at <= at)) return;
    alarm_set(&nt->alarm, at);
}

/* The net thread, net_lock held: every timer that has fired, those that fire meanwhile too. */
static void net_timers_run(void) {
    for (;;) {
        uint64_t flags = spin_lock(&rxq_lock);
        struct net_timer *nt = net_due_first;
        if (nt) {
            net_due_first = nt->due_next;
            if (!net_due_first) net_due_last = &net_due_first;
            nt->due = false; /* a fire from here on queues it again */
        }
        spin_unlock(&rxq_lock, flags);
        if (!nt) return;
        nt->run(nt, clock_now_ns());
    }
}

void net_rx_notify(void) {
    uint64_t flags = spin_lock(&rxq_lock);
    net_work_pending = true;
    spin_unlock(&rxq_lock, flags);
    wq_wake_all(&rxq_wq);
}

/* rxq_lock held: the frame into the queue, and the net thread told; false when it is full. */
static bool rxq_put_locked(const void *frame, size_t len) {
    int next = (rxq_head + 1) % RXQ;
    if (next == rxq_tail) return false;
    memcpy(rxq_buf[rxq_head], frame, len);
    rxq_len[rxq_head] = (uint16_t)len;
    rxq_head = next;
    rxq_high_water = MAX(rxq_high_water, (uint64_t)((rxq_head - rxq_tail + RXQ) % RXQ));
    net_work_pending = true;
    return true;
}

void net_rx(const void *frame, size_t len) {
    uint64_t flags = spin_lock(&rxq_lock);
    bool queued = false;
    if (len < 14 || len > 1536) rxq_invalid++;
    else if (!(queued = rxq_put_locked(frame, len))) rxq_dropped++;
    spin_unlock(&rxq_lock, flags);
    if (queued) wq_wake_all(&rxq_wq);
}

/* The net thread, net_lock held: every queued frame, including those queued meanwhile. */
static void eth_input(const void *packet, size_t len);
static void rxq_drain(void) {
    for (;;) {
        uint64_t flags = spin_lock(&rxq_lock);
        int at = rxq_tail;
        bool empty = at == rxq_head;
        spin_unlock(&rxq_lock, flags);
        if (empty) return;
        eth_input(rxq_buf[at], rxq_len[at]);
        flags = spin_lock(&rxq_lock);
        rxq_tail = (at + 1) % RXQ;
        spin_unlock(&rxq_lock, flags);
    }
}

/* ---------------------------------------------------------------- output */

/* The one frame every output is built in, under net_lock. */
static uint8_t frame[1600];
#define L4 (frame + 14 + 20) /* transport payload area */
static uint16_t ip_ident;

static int eth_output(const uint8_t dst[6], uint16_t type, size_t len) {
    if (!nif) return -N_ERR_NETWORK_DOWN;
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, link_mac, 6);
    e->type = htons(type);
    size_t total = 14 + len;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    int result = nif->send(frame, total);
    if (!result) {tx_packets++;net_service_bytes+=total;}
    return result;
}

/* This system's own addresses: the loopback network 127/8 and the interface
   address. Their datagrams never reach the wire. */
static bool ip_is_local(uint32_t dst) { return (dst >> 24) == 127 || (configured && dst == my_ip); }
/* A datagram to 127/8 comes from that same loopback address, so both ends of
   a local connection name each other alike; everything else is from my_ip. */
static uint32_t ip_source(uint32_t dst) { return (dst >> 24) == 127 ? dst : my_ip; }

/* Enter a local datagram in the receive queue as if the card had delivered it.
   A full queue is backpressure (the caller retries), not a lost packet. */
static int ip_loopback(size_t len) {
    if (!nif) return -N_ERR_NETWORK_DOWN;
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, link_mac, 6);
    memcpy(e->src, link_mac, 6);
    e->type = htons(0x0800);
    uint64_t flags = spin_lock(&rxq_lock);
    bool queued = rxq_put_locked(frame, 14 + len);
    spin_unlock(&rxq_lock, flags);
    if (!queued) return -N_ERR_BUSY;
    wq_wake_all(&rxq_wq);
    tx_packets++;
    return 0;
}

/* src is the datagram's source address: ip_source(dst), or 0 for a DHCP client's broadcast. */
static int ip_output(uint32_t src, uint32_t dst, const uint8_t mac[6], uint8_t proto, size_t len) {
    struct ip_hdr *ip = (struct ip_hdr *)(frame + 14);
    ip->vihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(20 + len));
    ip->id = htons(ip_ident++);
    ip->frag = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip_put(ip->src, src);
    ip_put(ip->dst, dst);
    ip->csum = htons(csum_fold(csum_add(0, ip, 20)));
    if (ip_is_local(dst)) return ip_loopback(20 + len);
    return eth_output(mac, 0x0800, 20 + len);
}

/* ---------------------------------------------------------------- ARP */

#define ARP_N 16
#define ARP_LIFETIME (600 * NS_PER_S) /* a learnt address is asked for again after this */
#define ARP_RETRY    NS_PER_S         /* a request unanswered this long is sent again */
static struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t time; /* when it was learnt */
    bool valid;
} arp_tab[ARP_N];

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    uint64_t now = clock_now_ns();
    for (int i = 0; i < ARP_N; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip && now < arp_tab[i].time + ARP_LIFETIME) {
            memcpy(mac, arp_tab[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_store(uint32_t ip, const uint8_t mac[6]) {
    int slot = 0;
    for (int i = 0; i < ARP_N; i++) {
        if (arp_tab[i].valid && arp_tab[i].ip == ip) {
            slot = i;
            break;
        }
        if (!arp_tab[i].valid || arp_tab[i].time < arp_tab[slot].time) slot = i;
    }
    arp_tab[slot].ip = ip;
    memcpy(arp_tab[slot].mac, mac, 6);
    arp_tab[slot].time = clock_now_ns();
    arp_tab[slot].valid = true;
    progress_wake(&shared_progress);
}

static void arp_send(uint16_t op, const uint8_t tha[6], uint32_t tpa, const uint8_t dst[6]) {
    struct arp_pkt *a = (struct arp_pkt *)(frame + 14);
    a->htype = htons(1);
    a->ptype = htons(0x0800);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, link_mac, 6);
    ip_put(a->spa, my_ip);
    memcpy(a->tha, tha, 6);
    ip_put(a->tpa, tpa);
    eth_output(dst, 0x0806, sizeof *a);
}

static void arp_input(const uint8_t *p, size_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)p;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != 0x0800 || a->hlen != 6 || a->plen != 4) return;
    uint32_t spa = ip_get(a->spa), tpa = ip_get(a->tpa);
    if (spa) arp_store(spa, a->sha);
    if (ntohs(a->op) == 1 && configured && tpa == my_ip) {
        uint8_t sha[6];
        memcpy(sha, a->sha, 6);
        arp_send(2, sha, spa, sha);
    }
}

static uint32_t next_hop(uint32_t dst) { return ((dst ^ my_ip) & my_mask) == 0 ? dst : my_gw; }

/* Resolve the link address (system call context: may block): three requests ARP_RETRY apart,
   all of it by deadline (the caller's budget; CLOCK_NEVER: the requests alone bound it). */
static int arp_resolve_until(uint32_t dst, uint8_t mac[6], uint64_t deadline) {
    if (!nif) return -N_ERR_NETWORK_DOWN;
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    if (ip_is_local(dst)) { /* delivered by ip_loopback */
        memcpy(mac, link_mac, 6);
        return 0;
    }
    if (dst == 0xFFFFFFFF || dst == (my_ip | ~my_mask)) {
        memcpy(mac, bcast_mac, 6);
        return 0;
    }
    if (!configured) return -N_ERR_NETWORK_DOWN;
    uint32_t hop = next_hop(dst);
    if (!hop) return -N_ERR_UNREACHABLE;
    for (int tries = 0; tries < 3; tries++) {
        if (thread_current()->killed) return -N_ERR_STOPPED;
        if(net_setup_cancelled())return -N_ERR_CANCELLED;
        if (clock_now_ns() >= deadline) return -N_ERR_TIMEOUT;
        if (arp_lookup(hop, mac)) return 0;
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", hop, bcast_mac);
        uint64_t until = MIN(clock_after(ARP_RETRY), deadline);
        for (;;) {
            if (thread_current()->killed) return -N_ERR_STOPPED;
            if(net_setup_cancelled())return -N_ERR_CANCELLED;
            uint64_t now = clock_now_ns();
            if (now >= deadline) return -N_ERR_TIMEOUT;
            if (arp_lookup(hop, mac)) return 0;
            if (now >= until) break;
            net_sleep(&shared_progress, until, true);
        }
    }
    return -N_ERR_UNREACHABLE;
}
static int arp_resolve(uint32_t dst, uint8_t mac[6]) { return arp_resolve_until(dst, mac, CLOCK_NEVER); }

/* ---------------------------------------------------------------- ICMP */

#define PING_N 8
#define PING_TIMEOUT_DEFAULT 1000 /* ms: SYS_NET_PING's timeout_ms of 0 or less */
static struct ping_wait {
    bool used, done;
    uint16_t id, seq;
    uint64_t replied; /* the moment the echo reply came in */
} pings[PING_N];

static void icmp_input(const uint8_t *src_mac, uint32_t src, const uint8_t *p, size_t len) {
    if (len < 8 || csum_fold(csum_add(0, p, len)) != 0) return;
    const struct icmp_hdr *h = (const struct icmp_hdr *)p;
    if (h->type == 8 && configured && len <= 1400) {
        uint8_t mac[6];
        memcpy(mac, src_mac, 6);
        memcpy(L4, p, len);
        struct icmp_hdr *r = (struct icmp_hdr *)L4;
        r->type = 0;
        r->csum = 0;
        r->csum = htons(csum_fold(csum_add(0, L4, len)));
        ip_output(ip_source(src), src, mac, 1, len);
    } else if (h->type == 0) {
        uint64_t now = clock_now_ns();
        for (int i = 0; i < PING_N; i++)
            if (pings[i].used && !pings[i].done && pings[i].id == ntohs(h->id) && pings[i].seq == ntohs(h->seq)) {
                pings[i].done = true;
                pings[i].replied = now;
            }
        progress_wake(&shared_progress);
    }
}

/* SYS_NET_PING: the round trip in ms, the address resolution and the echo both by deadline. */
static int64_t sys_ping(uint32_t dst, int seq, uint64_t deadline) {
    if (dst == my_ip || (dst >> 24) == 127) return 0;
    uint8_t mac[6];
    int r = arp_resolve_until(dst, mac, deadline);
    if (r < 0) return r;
    int slot = -1;
    for (int i = 0; i < PING_N; i++)
        if (!pings[i].used) slot = i;
    if (slot < 0) return -N_ERR_BUSY;
    struct ping_wait *w = &pings[slot];
    w->used = true;
    w->done = false;
    w->id = (uint16_t)(0x4E00 + process_current()->pid);
    w->seq = (uint16_t)seq;
    struct icmp_hdr *h = (struct icmp_hdr *)L4;
    h->type = 8;
    h->code = 0;
    h->id = htons(w->id);
    h->seq = htons(w->seq);
    h->csum = 0;
    memcpy(L4 + 8, "Nocturne says hello to the night.", 32);
    h->csum = htons(csum_fold(csum_add(0, L4, 40)));
    uint64_t sent = clock_now_ns();
    ip_output(ip_source(dst), dst, mac, 1, 40);
    int64_t ret = -N_ERR_TIMEOUT;
    while (!w->done && clock_now_ns() < deadline) {
        if (thread_current()->killed) {
            ret = -N_ERR_STOPPED;
            break;
        }
        net_sleep(&shared_progress, deadline, false);
    }
    if (w->done) ret = (int64_t)(ns_between(sent, w->replied) / NS_PER_MS);
    w->used = false;
    return ret;
}

/* ---------------------------------------------------------------- UDP */

#define UDP_INITIAL 8
#define UDP_Q 8
struct udp_dgram {
    uint32_t src;
    uint16_t sport, len;
    uint8_t data[1472];
};
struct udp_sock {
    bool used, internal;
    int pid;
    uint16_t port;
    uint64_t generation;
    struct udp_dgram *q;
    int qh, qt;
};
/* Socket metadata must stay at a stable address across a blocking receive.
   The pointer table grows, while queues exist only for live sockets. The
   initial capacity is not a global concurrency limit for DNS or user UDP. */
static struct udp_sock udp_initial[UDP_INITIAL];
static struct udp_sock *udp_initial_slots[UDP_INITIAL]={
    &udp_initial[0],&udp_initial[1],&udp_initial[2],&udp_initial[3],
    &udp_initial[4],&udp_initial[5],&udp_initial[6],&udp_initial[7]
};
static struct udp_sock **udp_socks=udp_initial_slots;
static size_t udp_capacity=UDP_INITIAL;
static uint64_t udp_generation;
static uint16_t next_port = 49152;

static uint16_t ephemeral_port(void) {
    if (next_port < 49152) next_port = 49152;
    return next_port++;
}

static int udp_output(uint16_t sport, uint32_t src, uint32_t dst, uint16_t dport, const uint8_t mac[6],
                      const void *data, size_t len) {
    if (len > 1472) return -N_ERR_INVALID;
    struct udp_hdr *u = (struct udp_hdr *)L4;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(8 + len));
    u->csum = 0;
    memcpy(L4 + 8, data, len);
    uint16_t c = l4_csum(src, dst, 17, L4, 8 + len);
    u->csum = htons(c ? c : 0xFFFF);
    return ip_output(src, dst, mac, 17, 8 + len);
}

static void dhcp_input(const uint8_t *p, size_t len);

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t len) {
    if (len < 8) return;
    const struct udp_hdr *u = (const struct udp_hdr *)p;
    size_t ulen = ntohs(u->len);
    if (ulen < 8 || ulen > len) return;
    if (u->csum && l4_csum(src, dst, 17, p, ulen) != 0) return;
    uint16_t dport = ntohs(u->dport);
    if (dport == 68) {
        dhcp_input(p + 8, ulen - 8);
        return;
    }
    for (size_t i = 0; i < udp_capacity; i++) {
        struct udp_sock *s = udp_socks[i];
        if (!s || !s->used || s->port != dport) continue;
        int next = (s->qh + 1) % UDP_Q;
        if (next == s->qt || ulen - 8 > sizeof s->q[0].data) return;
        struct udp_dgram *d = &s->q[s->qh];
        d->src = src;
        d->sport = ntohs(u->sport);
        d->len = (uint16_t)(ulen - 8);
        memcpy(d->data, p + 8, ulen - 8);
        s->qh = next;
        progress_wake(&shared_progress);
        return;
    }
}

static bool udp_port_used(uint16_t port) {
    for(size_t i=0;i<udp_capacity;i++)
        if(udp_socks[i] && udp_socks[i]->used && udp_socks[i]->port==port)return true;
    return false;
}
static void udp_release(struct udp_sock *s) {
    if(!s || !s->used)return;
    s->used=false;kfree(s->q);s->q=NULL;s->qh=s->qt=0;
    progress_wake(&shared_progress);
}
static int udp_alloc(uint16_t port, int pid) {
    if(port) {if(udp_port_used(port))return -N_ERR_ADDRESS_IN_USE;}
    else {
        /* There are exactly 16384 ephemeral UDP ports, not eight sockets. */
        unsigned attempts=0;
        do {port=ephemeral_port();if(!udp_port_used(port))break;}while(++attempts<16384);
        if(attempts==16384)return -N_ERR_ADDRESS_IN_USE;
    }
    size_t slot=0;
    while(slot<udp_capacity && udp_socks[slot] && udp_socks[slot]->used)slot++;
    if(slot==udp_capacity) {
        /* Handles use the syscall signed-int ABI. Reject only arithmetic or
           allocation failure, never a small scheduling-derived count. */
        size_t next=udp_capacity<=((size_t)0x7fffffff+1)/2?udp_capacity*2:(size_t)0x7fffffff+1;
        if(next<=udp_capacity || next>SIZE_MAX/sizeof *udp_socks)return -N_ERR_NO_MEMORY;
        struct udp_sock **grown=kmalloc(next*sizeof *grown);
        if(!grown)return -N_ERR_NO_MEMORY;
        memcpy(grown,udp_socks,udp_capacity*sizeof *grown);
        memset(grown+udp_capacity,0,(next-udp_capacity)*sizeof *grown);
        if(udp_socks!=udp_initial_slots)kfree(udp_socks);
        udp_socks=grown;udp_capacity=next;
    }
    struct udp_sock *s=udp_socks[slot];
    if(!s) {
        s=kmalloc(sizeof *s);if(!s)return -N_ERR_NO_MEMORY;
        memset(s,0,sizeof *s);udp_socks[slot]=s;
    }
    struct udp_dgram *queue=kmalloc(UDP_Q*sizeof *queue);
    if(!queue)return -N_ERR_NO_MEMORY;
    memset(s,0,sizeof *s);s->q=queue;s->used=true;s->pid=pid;s->port=port;
    if(++udp_generation==0)++udp_generation;
    s->generation=udp_generation;
    return (int)slot;
}

/* Wait for a datagram on socket i until deadline: the datagram, or NULL with *err set
   (-N_ERR_NOT_READY once the deadline has come). */
static struct udp_dgram *udp_wait(int i, uint64_t deadline, int *err) {
    struct udp_sock *s = udp_socks[i];
    uint64_t generation=s->generation;
    for (;;) {
        /* A sibling thread can close/reuse this handle while we sleep. Never
           consume another socket generation or touch its freed queue. */
        if(!s->used || s->generation!=generation){*err=-N_ERR_NO_HANDLE;return NULL;}
        if(s->internal && net_setup_cancelled()){*err=-N_ERR_CANCELLED;return NULL;}
        if (s->qt != s->qh) {
            struct udp_dgram *d = &s->q[s->qt];
            s->qt = (s->qt + 1) % UDP_Q;
            return d;
        }
        if (thread_current()->killed) {
            *err = -N_ERR_STOPPED;
            return NULL;
        }
        if (clock_now_ns() >= deadline) {
            *err = -N_ERR_NOT_READY;
            return NULL;
        }
        net_sleep(&shared_progress, deadline, s->internal);
    }
}

static struct udp_sock *user_udp(int h) {
    if(h<0 || (size_t)h>=udp_capacity)return NULL;
    struct udp_sock *s=udp_socks[h];
    return s && s->used && !s->internal && s->pid==process_current()->pid?s:NULL;
}

/* ---------------------------------------------------------------- DHCP */

enum { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };
static int dhcp_state;
static uint32_t dhcp_xid, dhcp_offer, dhcp_server;
static uint64_t dhcp_next; /* the moment of the next message (a retry, or the renewal) */
static int dhcp_tries;
static struct net_timer dhcp_timer;

static void dhcp_at(uint64_t at) {
    dhcp_next = at;
    net_timer_at(&dhcp_timer, at);
}

static void dhcp_send(int type) {
    uint8_t b[300];
    memset(b, 0, sizeof b);
    b[0] = 1; /* BOOTREQUEST */
    b[1] = 1;
    b[2] = 6;
    b[4] = (uint8_t)(dhcp_xid >> 24);
    b[5] = (uint8_t)(dhcp_xid >> 16);
    b[6] = (uint8_t)(dhcp_xid >> 8);
    b[7] = (uint8_t)dhcp_xid;
    b[10] = 0x80; /* broadcast replies please */
    memcpy(b + 28, link_mac, 6);
    static const uint8_t magic[4] = {99, 130, 83, 99};
    memcpy(b + 236, magic, 4);
    int o = 240;
    b[o++] = 53;
    b[o++] = 1;
    b[o++] = (uint8_t)type;
    b[o++] = 61; /* client id */
    b[o++] = 7;
    b[o++] = 1;
    memcpy(b + o, link_mac, 6);
    o += 6;
    if (type == 3) {
        b[o++] = 50;
        b[o++] = 4;
        ip_put(b + o, dhcp_offer);
        o += 4;
        if (dhcp_server) {
            b[o++] = 54;
            b[o++] = 4;
            ip_put(b + o, dhcp_server);
            o += 4;
        }
    }
    b[o++] = 12; /* host name */
    b[o++] = 8;
    memcpy(b + o, "nocturne", 8);
    o += 8;
    b[o++] = 55; /* parameter request list */
    b[o++] = 4;
    b[o++] = 1;
    b[o++] = 3;
    b[o++] = 6;
    b[o++] = 51;
    b[o++] = 255;
    udp_output(68, 0, 0xFFFFFFFF, 67, bcast_mac, b, sizeof b); /* from 0.0.0.0, also while bound */
}

static void dhcp_start(void) {
    dhcp_state = DHCP_DISCOVER;
    dhcp_xid = rnd();
    dhcp_tries = 0;
    dhcp_at(clock_now_ns());
}

static void dhcp_input(const uint8_t *p, size_t len) {
    if (len < 240 || p[0] != 2 || ip_get(p + 4) != dhcp_xid) return;
    if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return;
    int type = 0;
    uint32_t mask = 0xFFFFFF00, router = 0, dns = 0, server = 0, lease = 86400;
    for (size_t i = 240; i < len;) {
        uint8_t opt = p[i];
        if (opt == 255) break;
        if (opt == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len) break;
        uint8_t l = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + l > len) break;
        if (opt == 53 && l >= 1) type = v[0];
        else if (opt == 1 && l >= 4) mask = ip_get(v);
        else if (opt == 3 && l >= 4) router = ip_get(v);
        else if (opt == 6 && l >= 4) dns = ip_get(v);
        else if (opt == 54 && l >= 4) server = ip_get(v);
        else if (opt == 51 && l >= 4) lease = ip_get(v);
        i += 2 + l;
    }
    uint32_t yiaddr = ip_get(p + 16);
    if (dhcp_state == DHCP_DISCOVER && type == 2) {
        dhcp_offer = yiaddr;
        dhcp_server = server;
        dhcp_state = DHCP_REQUEST;
        dhcp_tries = 0;
        dhcp_send(3);
        dhcp_at(clock_after(2 * NS_PER_S));
    } else if (dhcp_state == DHCP_REQUEST && type == 5) {
        uint64_t flags = spin_lock(&config_lock);
        my_ip = yiaddr;
        my_mask = mask;
        my_gw = router;
        my_dns = dns ? dns : router;
        configured = true;
        spin_unlock(&config_lock, flags);
        dhcp_state = DHCP_BOUND;
        if (lease < 60) lease = 60;
        dhcp_at(clock_after((uint64_t)lease * (NS_PER_S / 2))); /* renew at half the lease */
        struct n_sockaddr resolver=dns_default_resolver();
        kprintf("net: %u.%u.%u.%u/%d gateway %u.%u.%u.%u dns %u.%u.%u.%u:%u (DHCP)\n", my_ip >> 24, (my_ip >> 16) & 255,
                (my_ip >> 8) & 255, my_ip & 255, __builtin_popcount(my_mask), my_gw >> 24, (my_gw >> 16) & 255,
                (my_gw >> 8) & 255, my_gw & 255, resolver.ip >> 24, (resolver.ip >> 16) & 255, (resolver.ip >> 8) & 255,
                resolver.ip & 255, resolver.port);
        progress_wake(&shared_progress);
    } else if (type == 6) {
        dhcp_start();
    }
}

/* dhcp_timer's work: the message whose moment has come, and the moment of the next. */
static void dhcp_timer_run(struct net_timer *nt, uint64_t now) {
    if (dhcp_state == DHCP_IDLE) return;
    if (now < dhcp_next) { /* fired for a moment that has moved later since */
        net_timer_at(nt, dhcp_next);
        return;
    }
    if (dhcp_state == DHCP_BOUND) {
        /* renew: re-request the same address */
        dhcp_offer = my_ip;
        dhcp_state = DHCP_REQUEST;
        dhcp_tries = 0;
    }
    if (dhcp_state == DHCP_REQUEST && dhcp_tries >= 4) {
        dhcp_start();
        if (configured) {
            dhcp_offer = my_ip;
            dhcp_state = DHCP_REQUEST;
        }
    }
    dhcp_send(dhcp_state == DHCP_DISCOVER ? 1 : 3);
    dhcp_tries++;
    dhcp_at(now + (dhcp_tries < 4 ? 2 : dhcp_tries < 10 ? 5 : 30) * NS_PER_S);
}

/* ---------------------------------------------------------------- DNS */

static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (v > 255) return false;
        }
        ip = ip << 8 | v;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *out = ip;
    return true;
}

static bool dns_cmd_space(char c) {return c==' ' || c=='\t' || c=='\r' || c=='\n';}
static bool dns_parse_resolver_endpoint(const char *at,const char *end,struct n_sockaddr *out) {
    uint32_t ip=0;
    for(unsigned part=0;part<4;part++) {
        if(at==end || *at<'0' || *at>'9')return false;
        unsigned octet=0;
        do {
            octet=octet*10+(unsigned)(*at++-'0');
            if(octet>255)return false;
        } while(at!=end && *at>='0' && *at<='9');
        ip=(ip<<8)|octet;
        if(at==end || *at++!=(part==3?':':'.'))return false;
    }
    if(at==end || *at<'0' || *at>'9')return false;
    unsigned port=0;
    do {
        port=port*10+(unsigned)(*at++-'0');
        if(port>UINT16_MAX)return false;
    } while(at!=end && *at>='0' && *at<='9');
    if(at!=end || !port)return false;
    *out=(struct n_sockaddr){.ip=ip,.port=(uint16_t)port};
    return true;
}

/* Parse one native dns-server=<decimal IPv4>:<decimal port> token. Absence
   retains DHCP; malformed or repeated settings disable the default resolver
   rather than silently sending queries to a different DHCP-supplied server.
   An explicit per-call resolver is independent of this boot preference. */
static int dns_parse_boot_resolver(const char *command,struct n_sockaddr *out) {
    const char key[]="dns-server";
    struct n_sockaddr selected={0};bool seen=false;
    *out=selected;
    for(const char *at=command;*at;) {
        while(dns_cmd_space(*at))at++;
        const char *begin=at;
        while(*at && !dns_cmd_space(*at))at++;
        size_t length=(size_t)(at-begin),key_length=sizeof key-1;
        if(length<key_length || strncmp(begin,key,key_length) ||
           (length>key_length && begin[key_length]!='='))continue;
        if(seen || length==key_length || !dns_parse_resolver_endpoint(begin+key_length+1,at,&selected))return -N_ERR_INVALID;
        seen=true;
    }
    *out=selected;
    return seen?1:0;
}
static struct n_sockaddr dns_default_resolver(void) {
    if(dns_boot_setting<0)return (struct n_sockaddr){0};
    if(dns_boot_setting)return dns_boot_resolver;
    return (struct n_sockaddr){.ip=my_dns,.port=53};
}
static void dns_configure(void) {
    extern const char *kernel_cmdline;
    dns_boot_setting=dns_parse_boot_resolver(kernel_cmdline,&dns_boot_resolver);
    if(dns_boot_setting<0)kprintf("net: invalid or duplicate dns-server setting; default DNS disabled\n");
    else if(dns_boot_setting)kprintf("net: default DNS %u.%u.%u.%u:%u (boot setting)\n",
        dns_boot_resolver.ip>>24,(dns_boot_resolver.ip>>16)&255,(dns_boot_resolver.ip>>8)&255,
        dns_boot_resolver.ip&255,dns_boot_resolver.port);
}

/* The cache has a byte budget and exact-size names/candidate vectors. Its
   capacity is not a hostname length or answer-count restriction. */
#define DNS_CACHE_BYTES (256u*1024u)
#define DNS_ATTEMPT (2 * NS_PER_S) /* a query unanswered this long is sent again, three times in all */
struct dns_cache_entry {
    struct dns_cache_entry *next;
    uint64_t expires, used; /* moments: its TTL's end, its last use */
    size_t bytes;
    uint32_t count, resolver;
    uint16_t port;
    char *name;
    uint32_t *ips;
};
static struct dns_cache_entry *dns_cache;
static size_t dns_cache_bytes;
struct dns_answer { uint32_t *ips, count, ttl, status; };
static int dns_tcp_exchange(uint32_t resolver,uint16_t port,uint64_t deadline,
                            const uint8_t *query,size_t query_len,uint8_t **reply,size_t *reply_len);

static uint32_t dns_hash(const char *s) {
    uint32_t hash=2166136261u;
    while (*s) { hash^=(uint8_t)*s++; hash*=16777619u; }
    return hash;
}
static int dns_failure(struct dns_answer *answer, unsigned status) {
    answer->status=status;
    answer->count=answer->ttl=0;
    switch(status) {
    case N_DNS_NXDOMAIN: case N_DNS_NODATA: return -N_ERR_NOT_FOUND;
    case N_DNS_SERVFAIL: return -N_ERR_NOT_READY;
    case N_DNS_REFUSED: return -N_ERR_CONNECTION_REFUSED;
    case N_DNS_TIMEOUT: return -N_ERR_TIMEOUT;
    case N_DNS_CANCELLED: return -N_ERR_STOPPED;
    case N_DNS_UNREACHABLE: return -N_ERR_UNREACHABLE;
    case N_DNS_RESOURCE: return -N_ERR_NO_MEMORY;
    default: return -N_ERR_PROTOCOL;
    }
}
static unsigned dns_error_status(int error) {
    return error==-N_ERR_NO_MEMORY?N_DNS_RESOURCE:error==-N_ERR_TIMEOUT||error==-N_ERR_NOT_READY?N_DNS_TIMEOUT:
           error==-N_ERR_STOPPED||error==-N_ERR_CANCELLED?N_DNS_CANCELLED:error==-N_ERR_PROTOCOL?N_DNS_FORMAT:N_DNS_UNREACHABLE;
}
static void dns_cache_store(const char *name,uint32_t resolver,uint16_t port,const struct dns_answer *answer) {
    size_t namesz=strlen(name)+1, name_storage=(size_t)ALIGN_UP(namesz,sizeof(uint32_t));
    size_t bytes=sizeof(struct dns_cache_entry)+name_storage+(size_t)answer->count*4;
    if (!answer->ttl || bytes>DNS_CACHE_BYTES) return;
    while (dns_cache && dns_cache_bytes+bytes>DNS_CACHE_BYTES) {
        struct dns_cache_entry **oldest=&dns_cache;
        for (struct dns_cache_entry **at=&dns_cache;*at;at=&(*at)->next)
            if ((*at)->used<(*oldest)->used) oldest=at;
        struct dns_cache_entry *old=*oldest;
        *oldest=old->next; dns_cache_bytes-=old->bytes; kfree(old);
    }
    struct dns_cache_entry *entry=kmalloc(bytes);
    if (!entry) return; /* cache allocation never fails an already valid answer */
    entry->name=(char *)(entry+1);
    entry->ips=(uint32_t *)((uint8_t *)(entry+1)+name_storage);
    memcpy(entry->name,name,namesz); memcpy(entry->ips,answer->ips,(size_t)answer->count*4);
    entry->count=answer->count; entry->bytes=bytes; entry->used=clock_now_ns();
    entry->resolver=resolver;entry->port=port;
    uint64_t duration=(uint64_t)answer->ttl*NS_PER_S;
    entry->expires=duration>UINT64_MAX-entry->used?UINT64_MAX:entry->used+duration;
    entry->next=dns_cache; dns_cache=entry; dns_cache_bytes+=bytes;
}

/* Hash each answer owner once. Following a CNAME chain then visits only the
   matching owners; unrelated additional A records cannot redirect a query. */
static int dns_parse_reply(const uint8_t *p,size_t len,const char *name,struct dns_answer *answer) {
    if (len<12 || !(p[2]&0x80) || (p[2]&0x7a) || p[4] || p[5]!=1) return dns_failure(answer,N_DNS_FORMAT);
    char decoded[DNS_NAME_TEXT_BYTES+1];
    size_t offset=dns_name_decode(p,len,12,decoded);
    if (!offset || offset+4>len || strcmp(decoded,name) || p[offset] || p[offset+1]!=1 || p[offset+2] || p[offset+3]!=1)
        return dns_failure(answer,N_DNS_FORMAT);
    offset+=4;
    switch (p[3]&15) {
    case 0: break;
    case 2: return dns_failure(answer,N_DNS_SERVFAIL);
    case 3: return dns_failure(answer,N_DNS_NXDOMAIN);
    case 5: return dns_failure(answer,N_DNS_REFUSED);
    default: return dns_failure(answer,N_DNS_FORMAT);
    }
    size_t an=((size_t)p[6]<<8)|p[7];
    if (an>(len-offset)/11) return dns_failure(answer,N_DNS_FORMAT);
    if (!an) return dns_failure(answer,N_DNS_NODATA);
    struct dns_record { char owner[DNS_NAME_TEXT_BYTES+1], target[DNS_NAME_TEXT_BYTES+1]; uint32_t ip,ttl,next; uint16_t type; };
    struct dns_record *records=kmalloc(MAX(an,1)*sizeof *records);
    size_t buckets=1;
    while (buckets<an*2+1) buckets*=2;
    uint32_t *heads=kmalloc(buckets*sizeof *heads);
    uint32_t *ips=kmalloc(MAX(an,1)*sizeof *ips);
    if (!records || !heads || !ips) { kfree(records);kfree(heads);kfree(ips);return dns_failure(answer,N_DNS_RESOURCE); }
    memset(heads,0,buckets*sizeof *heads);
    size_t count=0;
    int result=-N_ERR_PROTOCOL;
    for (size_t i=0;i<an;i++) {
        offset=dns_name_decode(p,len,offset,decoded);
        if (!offset || offset+10>len) goto malformed;
        unsigned type=(unsigned)p[offset]<<8|p[offset+1], cls=(unsigned)p[offset+2]<<8|p[offset+3];
        uint32_t ttl=ip_get(p+offset+4);
        size_t bytes=(size_t)p[offset+8]<<8|p[offset+9]; offset+=10;
        if (bytes>len-offset) goto malformed;
        if (cls==1 && (type==1 || type==5)) {
            struct dns_record *rr=&records[count];
            strcpy(rr->owner,decoded); rr->type=(uint16_t)type; rr->ttl=ttl;
            if (type==1) { if (bytes!=4) goto malformed; rr->ip=ip_get(p+offset); }
            else if (dns_name_decode(p,len,offset,rr->target)!=offset+bytes) goto malformed;
            uint32_t bucket=dns_hash(rr->owner)&(buckets-1);
            rr->next=heads[bucket]; heads[bucket]=(uint32_t)++count;
        }
        offset+=bytes;
    }
    strcpy(decoded,name);
    answer->ttl=UINT32_MAX;
    for (size_t steps=0;steps<=count;steps++) {
        const char *target=NULL;
        for (uint32_t index=heads[dns_hash(decoded)&(buckets-1)];index;index=records[index-1].next) {
            struct dns_record *rr=&records[index-1];
            if (strcmp(rr->owner,decoded)) continue;
            answer->ttl=MIN(answer->ttl,rr->ttl);
            if (rr->type==1) {
                bool duplicate=false;
                for (uint32_t i=0;i<answer->count;i++) if (ips[i]==rr->ip) {duplicate=true;break;}
                if (!duplicate) ips[answer->count++]=rr->ip;
            } else {
                if (target && strcmp(target,rr->target)) goto malformed;
                target=rr->target;
            }
        }
        if (answer->count) {
            if (target) goto malformed; /* CNAME cannot coexist with A */
            answer->ips=ips;ips=NULL;answer->status=N_DNS_OK;result=0;goto done;
        }
        if (!target) { result=dns_failure(answer,N_DNS_NODATA);goto done; }
        strcpy(decoded,target);
    }
malformed:
    result=dns_failure(answer,N_DNS_FORMAT);
done:
    kfree(records);kfree(heads);kfree(ips);return result;
}

static int dns_query(const char *source,uint32_t resolver,uint16_t port,struct dns_answer *answer) {
    memset(answer,0,sizeof *answer);
    answer->status=N_DNS_FORMAT;
    if(net_setup_cancelled()){dns_failure(answer,N_DNS_CANCELLED);return -N_ERR_CANCELLED;}
    uint32_t literal;
    char name[DNS_NAME_TEXT_BYTES+1];
    size_t textlen=strlen(source);
    if (textlen>DNS_NAME_TEXT_BYTES) return -N_ERR_NAME_TOO_LONG;
    uint8_t validation[DNS_NAME_WIRE_BYTES];size_t encoded=0;
    if(!dns_name_encode(source,textlen,validation,sizeof validation,&encoded)) return dns_failure(answer,N_DNS_FORMAT);
    for (size_t i=0;i<=textlen;i++) name[i]=(char)tolower((uint8_t)source[i]);
    if (textlen && name[textlen-1]=='.') name[--textlen]=0;
    if (parse_ip(name,&literal) || (!strcmp(name,"localhost") && (literal=0x7f000001))) {
        answer->ips=kmalloc(4);if (!answer->ips) return dns_failure(answer,N_DNS_RESOURCE);
        answer->ips[0]=literal;answer->count=1;answer->status=N_DNS_OK;return 0;
    }
    uint64_t now=clock_now_ns();
    for (struct dns_cache_entry **at=&dns_cache;*at;) {
        struct dns_cache_entry *entry=*at;
        if (entry->expires<=now) { *at=entry->next;dns_cache_bytes-=entry->bytes;kfree(entry);continue; }
        if (entry->resolver==resolver && entry->port==port && !strcmp(entry->name,name)) {
            answer->ips=kmalloc((size_t)entry->count*4);if (!answer->ips) return dns_failure(answer,N_DNS_RESOURCE);
            memcpy(answer->ips,entry->ips,(size_t)entry->count*4);answer->count=entry->count;
            uint64_t left=entry->expires-now; /* whole seconds, rounded up */
            answer->ttl=(uint32_t)MIN(left/NS_PER_S+(left%NS_PER_S!=0),(uint64_t)UINT32_MAX);
            answer->status=N_DNS_OK;entry->used=now;return 0;
        }
        at=&entry->next;
    }
    if (!configured || !resolver || !port) return dns_failure(answer,N_DNS_UNREACHABLE);
    uint8_t query[12+DNS_NAME_WIRE_BYTES+4];
    memset(query,0,12);uint16_t id=(uint16_t)rnd();
    query[0]=(uint8_t)(id>>8);query[1]=(uint8_t)id;query[2]=1;query[5]=1;
    size_t bytes=0;
    if (!dns_name_encode(name,textlen,query+12,sizeof query-16,&bytes)) return dns_failure(answer,N_DNS_FORMAT);
    size_t query_len=12+bytes;query[query_len++]=0;query[query_len++]=1;query[query_len++]=0;query[query_len++]=1;
    uint8_t mac[6];int result=arp_resolve(resolver,mac);
    if (result<0) {dns_failure(answer,dns_error_status(result));return result;}
    int sock=udp_alloc(0,process_current()->pid);if (sock<0) {dns_failure(answer,sock==-N_ERR_NO_MEMORY?N_DNS_RESOURCE:N_DNS_UNREACHABLE);return sock;}
    udp_socks[sock]->internal=true;uint64_t generation=udp_socks[sock]->generation;
    result=dns_failure(answer,N_DNS_TIMEOUT);
    for (unsigned attempt=0;attempt<3 && result==-N_ERR_TIMEOUT;attempt++) {
        if(net_setup_cancelled()){result=-N_ERR_CANCELLED;dns_failure(answer,N_DNS_CANCELLED);break;}
        int sent=udp_output(udp_socks[sock]->port,ip_source(resolver),resolver,port,mac,query,query_len);
        if (sent<0) {result=sent;dns_failure(answer,N_DNS_UNREACHABLE);break;}
        uint64_t until=clock_after(DNS_ATTEMPT);
        while (clock_now_ns()<until) {
            int err=0;
            struct udp_dgram *packet=udp_wait(sock,until,&err);
            if (!packet) {if (err && err!=-N_ERR_NOT_READY) {result=err;dns_failure(answer,dns_error_status(err));}break;}
            const uint8_t *p=packet->data;size_t length=packet->len;
            if (packet->src!=resolver || packet->sport!=port || length<12 || !(p[2]&0x80) || p[0]!=query[0] || p[1]!=query[1]) continue;
            uint8_t *tcp_reply=NULL;
            if (p[2]&2) {
                result=dns_tcp_exchange(resolver,port,until,query,query_len,&tcp_reply,&length);
                if (result<0) {dns_failure(answer,dns_error_status(result));break;}
                p=tcp_reply;
                if (length<12 || p[0]!=query[0] || p[1]!=query[1] || (p[2]&2)) {kfree(tcp_reply);result=dns_failure(answer,N_DNS_FORMAT);break;}
            }
            if(net_setup_cancelled()){result=-N_ERR_CANCELLED;dns_failure(answer,N_DNS_CANCELLED);}
            else result=dns_parse_reply(p,length,name,answer);
            kfree(tcp_reply);
            goto finished;
        }
    }
finished:
    if (udp_socks[sock]->generation==generation) udp_release(udp_socks[sock]);
    if (!result) dns_cache_store(name,resolver,port,answer);
    return result;
}

/* ---------------------------------------------------------------- TCP */

#define TCP_INITIAL 16
/* Requested default storage target, NOT a limit on a transfer or connection
   count. Actual capacity is allocated per socket, can be configured at boot,
   and falls back under memory pressure before negotiation. */
#define TCP_BUFFER_DEFAULT (1u << 20)
#define TCP_WINDOW_SHIFT_MAX 14 /* RFC 7323 sequence/window arithmetic bound */
#define TCP_WINDOW_MAX ((uint32_t)UINT16_MAX << TCP_WINDOW_SHIFT_MAX)
#define TCP_MSS (1500u - 20u - 20u) /* interface IPv4 MTU minus IP/TCP headers */
/* The most a system call copies between the app and a ring under one hold of net_lock (a
   receive returns at most this; a send gives the lock back between chunks). */
#define TCP_COPY_CHUNK (64u * 1024u)
static uint32_t tcp_rx_target = TCP_BUFFER_DEFAULT, tcp_tx_target = TCP_BUFFER_DEFAULT;
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { T_FREE, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK,
       T_TIME_WAIT, T_CLOSED, T_LISTEN, T_SYN_RECEIVED };
#define TCP_BACKLOG_DEFAULT 128 /* listen() without an application backlog */

#define TCP_OOO   8    /* out-of-order ranges remembered per connection */
/* The retransmission timeout (RFC 6298), in clock ns: 1 s before any sample, then from the RTT
   estimate, held between these. G, the clock's granularity in RFC 6298 2.3: the clock and the
   alarms that time retransmissions resolve finer than this. */
#define RTO_INITIAL NS_PER_S
#define RTO_MIN   (200 * NS_PER_MS)
#define RTO_MAX   (16 * NS_PER_S)
#define RTO_G     NS_PER_US
/* TIME-WAIT, kept short of RFC 9293's 2 MSL (3.6), and how long a connection no app holds any
   more may take to finish before it is reset. */
#define TCP_TIME_WAIT     (2 * NS_PER_S)
#define TCP_ORPHAN_LINGER (20 * NS_PER_S)
#define TCP_CONNECT_DEFAULT 15000 /* ms: SYS_TCP_CONNECT's timeout_ms of 0 or less */

struct tcb {
    int state, err, pid;
    bool user_open, fin_pending, fin_sent, rx_fin;
    bool window_scaled, ack_pending, progress_pending, tx_retry, probe_pending, resend_pending;
    uint8_t rx_shift, tx_shift;
    unsigned ack_segments;
    uint32_t rip;
    uint16_t lport, rport, mss;
    uint8_t mac[6];
    /* send side. tx[tx_head] is the byte at snd_una; snd_nxt is the next byte to (re)send, snd_max the
       highest ever sent (they differ after a timeout, when we go back to snd_una) */
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2, fin_seq;
    /* congestion control: NewReno (RFC 5681 / 6582) */
    uint32_t cwnd, ssthresh, recover;
    int dupacks;
    bool in_recovery;
    /* RTT estimate (RFC 6298, clock ns; srtt 0: no sample yet); one segment is timed at a time,
       never a retransmitted one: rtt_at is the moment it was sent (0: none is) */
    uint64_t srtt, rttvar;
    uint32_t rtt_seq;
    uint64_t rtt_at;
    /* receive side. rx is a ring: rx_len bytes are in order and readable; segments that arrive
       ahead of rcv_nxt are stored at their place behind them, and ooo[] remembers which ranges */
    uint32_t rcv_nxt, adv_wnd;
    struct tcp_range { uint32_t start, end; } *ooo;
    size_t nooo, ooo_capacity;
    uint8_t *rx, *tx;
    uint32_t rx_head, rx_len, tx_head, tx_len, rx_capacity, tx_capacity;
    /* Moments: the retransmission (or SYN, or zero-window probe) is due (0: not timed), and
       TIME-WAIT ends or, once no app holds the connection, it is reset (tcp_timer_run). */
    uint64_t rto_at, linger_until;
    uint64_t generation;
    uint64_t rto; /* ns */
    int retries;
    /* Passive open. A listener (T_LISTEN) keeps only lip (0: every local
       address), lport and backlog. Each connection it spawned names it by
       generation and stays owned by the stack (accept_pending, not user_open)
       until SYS_TCP_ACCEPT hands it out, oldest established first (accept_order). */
    int backlog;
    uint32_t lip;
    bool accept_pending;
    uint64_t listener, accept_order;
    /* Last, and kept by tcb_clear: the slot's progress outlives each connection it holds, so a
       task that read its generation for an earlier one and sleeps without net_lock is woken,
       not lost. Stable metadata owns its progress waiters: one socket's ACK/data must not
       reschedule every DNS/TCP helper that is waiting on another socket. So does its timer,
       which the alarm and the thread's queue may still hold for the connection before. */
    struct net_progress progress;
    struct net_timer timer; /* the connection's next look at rto_at and linger_until */
};
/* The slot empty again, its progress and timer kept. */
static void tcb_clear(struct tcb *t) { memset(t, 0, offsetof(struct tcb, progress)); }
/* Closed-but-lingering tuples remain valid until TCP releases them. They
   must not occupy a fixed sixteen-slot pool and serialize unrelated fetches. */
static struct tcb *tcp_initial_slots[TCP_INITIAL];
static struct tcb **tcbs=tcp_initial_slots;
static size_t tcp_capacity=TCP_INITIAL;
static uint64_t tcp_generation, tcp_accept_counter;

static struct n_tcpstats tcp_stats;

/* fault injection for tests (SYS_NET_TEST): applies only to fault_pid's connections */
static struct n_netfault fault;
static int fault_pid;
static uint8_t fault_held[1600];
static size_t fault_held_len;
static uint32_t fault_held_src;
#define FAULT_HOLD (200 * NS_PER_MS) /* a held-back segment whose successor does not come goes in after this */
static uint64_t fault_held_until;
static struct net_timer fault_timer;

static bool fault_hit(uint16_t per_mille) { return per_mille && rnd() % 1000 < per_mille; }

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)

static void tcp_free(struct tcb *t) {
    /* Wake sleeping tasks before clearing the stable slot; their generation
       checks reject a closed/reused connection when they next run. */
    progress_wake(&t->progress);
    alarm_cancel(&t->timer.alarm);
    kfree(t->rx);
    kfree(t->tx);
    kfree(t->ooo);
    tcb_clear(t);
}

static void tcp_progress(struct tcb *t) {
    if (receive_batch) t->progress_pending = true;
    else progress_wake(&t->progress);
}

/* The connection is over: CLOSED with err (0: an orderly end), nothing more retransmitted, its
   waiters told. One no app holds goes at the net thread's next look (tcp_timer_run). */
static void tcp_end(struct tcb *t, int err) {
    t->state = T_CLOSED;
    t->err = err;
    t->rto_at = 0;
    tcp_progress(t);
    if (!t->user_open) net_timer_at(&t->timer, 0);
}

/* RFC 9293 3.6: the side that closed first waits out the peer's last segments, then ends. */
static void tcp_time_wait(struct tcb *t) {
    t->state = T_TIME_WAIT;
    t->linger_until = clock_after(TCP_TIME_WAIT);
    net_timer_at(&t->timer, t->linger_until);
}

static void tcp_ring_write(uint8_t *ring, uint32_t capacity, uint32_t at, const void *src, uint32_t n) {
    at %= capacity;
    uint32_t first = MIN(n, capacity-at);
    memcpy(ring+at, src, first);
    if (n>first) memcpy(ring, (const uint8_t *)src+first, n-first);
}

static void tcp_ring_read(void *dst, const uint8_t *ring, uint32_t capacity, uint32_t at, uint32_t n) {
    at %= capacity;
    uint32_t first = MIN(n, capacity-at);
    memcpy(dst, ring+at, first);
    if (n>first) memcpy((uint8_t *)dst+first, ring, n-first);
}

/* The app's n bytes at usrc into the ring from at: 0, or -N_ERR_BAD_ADDRESS (whatever arrived
   lies past the ring's data, where nothing reads it). */
static int tcp_ring_write_user(uint8_t *ring, uint32_t capacity, uint32_t at, const void *usrc, uint32_t n) {
    at %= capacity;
    uint32_t first = MIN(n, capacity-at);
    if (copy_from_user(ring+at, usrc, first) < 0) return -N_ERR_BAD_ADDRESS;
    if (n>first && copy_from_user(ring, (const uint8_t *)usrc+first, n-first) < 0) return -N_ERR_BAD_ADDRESS;
    return 0;
}

/* The ring's n bytes from at into the app's memory at udst: how many arrived (the first piece
   when the second's page went), or -N_ERR_BAD_ADDRESS when none did. */
static int64_t tcp_ring_read_user(void *udst, const uint8_t *ring, uint32_t capacity, uint32_t at, uint32_t n) {
    at %= capacity;
    uint32_t first = MIN(n, capacity-at);
    if (copy_to_user(udst, ring+at, first) < 0) return -N_ERR_BAD_ADDRESS;
    if (n>first && copy_to_user((uint8_t *)udst+first, ring, n-first) < 0) return first;
    return n;
}

static uint32_t tcp_window(struct tcb *t) {
    return MIN(t->rx_capacity-t->rx_len, (uint32_t)UINT16_MAX << (t->window_scaled?t->rx_shift:0));
}

static uint32_t tcp_peer_window(struct tcb *t, const struct tcp_hdr *h) {
    return (uint32_t)ntohs(h->win) << ((t->window_scaled && !(h->flags&F_SYN))?t->tx_shift:0);
}

/* Payload, if any, is borrowed directly from the send ring at seq-snd_una. */
static int tcp_output(struct tcb *t, uint8_t flags, uint32_t seq, size_t len) {
    if (len && t->pid == fault_pid && fault_hit(fault.tx_drop)) {
        tcp_stats.segs_out++;
        tcp_stats.faults_dropped++;
        return 0; /* accepted, then intentionally "lost" on the test wire */
    }
    struct tcp_hdr *h = (struct tcp_hdr *)L4;
    /* RFC 7323: a <SYN,ACK> carries Window Scale only when the <SYN> did */
    bool ws = (flags & F_SYN) && (!(flags & F_ACK) || t->window_scaled);
    size_t hl = (flags & F_SYN) ? (ws ? 28 : 24) : 20; /* MSS, NOP and 3-byte WS, word aligned */
    h->sport = htons(t->lport);
    h->dport = htons(t->rport);
    h->seq = htonl(seq);
    h->ack = htonl((flags & F_ACK) ? t->rcv_nxt : 0);
    h->off = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    unsigned shift = t->window_scaled && !(flags&F_SYN) ? t->rx_shift : 0;
    uint16_t advertised = (uint16_t)MIN(tcp_window(t)>>shift, (uint32_t)UINT16_MAX);
    h->win = htons(advertised); /* SYN windows are never scaled */
    h->csum = 0;
    h->urg = 0;
    if (flags & F_SYN) {
        L4[20] = 2; /* MSS option */
        L4[21] = 4;
        L4[22] = TCP_MSS >> 8;
        L4[23] = TCP_MSS & 255;
        if (ws) {
            L4[24] = 1; /* NOP aligns the following option */
            L4[25] = 3; /* RFC 7323 Window Scale */
            L4[26] = 3;
            L4[27] = t->rx_shift;
        }
    }
    if (len) tcp_ring_read(L4+hl,t->tx,t->tx_capacity,t->tx_head+(seq-t->snd_una),(uint32_t)len);
    h->csum = htons(l4_csum(ip_source(t->rip), t->rip, 6, L4, hl + len));
    int result = ip_output(ip_source(t->rip), t->rip, t->mac, 6, hl+len);
    if (result == -N_ERR_NOT_READY || result == -N_ERR_BUSY) {
        t->tx_retry = true;
        if (flags&F_ACK) t->ack_pending = true;
        return result; /* no sequence advance, retransmit timer or cwnd loss */
    }
    if (result < 0) {
        tcp_end(t, result);
        return result;
    }
    tcp_stats.segs_out++;
    t->adv_wnd = (uint32_t)advertised << shift;
    if (flags&F_ACK) { t->ack_pending = false; t->ack_segments = 0; }
    return 0;
}

/* The retransmission timer runs (from now, at the current RTO) unless it already does. */
static void tcp_arm(struct tcb *t) {
    if (t->rto_at) return;
    t->rto_at = clock_after(t->rto);
    net_timer_at(&t->timer, t->rto_at);
}

static void tcp_sent(struct tcb *t, uint32_t n) {
    if (!t->rtt_at && t->snd_nxt == t->snd_max) { /* time new data only (Karn) */
        t->rtt_at = clock_now_ns();
        t->rtt_seq = t->snd_nxt + n;
    }
    t->snd_nxt += n;
    if (SEQ_LT(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt;
    tcp_arm(t);
}

/* send whatever data (and FIN) the send and congestion windows allow, from snd_nxt on;
   force sends one byte into a zero window (a window probe) */
static void tcp_push(struct tcb *t, bool force) {
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT && t->state != T_FIN_WAIT1 &&
        t->state != T_LAST_ACK && t->state != T_CLOSING)
        return;
    uint32_t wnd = MIN(t->snd_wnd, t->cwnd);
    if (force && wnd == 0) wnd = 1;
    t->tx_retry = false;
    t->probe_pending = force;
    /* net_lock keeps tx_len, snd_una and wnd stable during this call.
       Every successful data output advances snd_nxt by a nonzero MSS-bounded
       prefix; storage/window exhaustion or NIC backpressure ends the loop.
       There are no waits here, and at most one FIN follows the finite data. */
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una; /* = bytes in flight, and the offset into tx */
        if (off < t->tx_len) {
            if (off >= wnd) break;
            uint32_t n = MIN(t->tx_len - off, MIN((uint32_t)t->mss, wnd - off));
            if (tcp_output(t, F_ACK | F_PSH, t->snd_nxt, n)<0) break;
            tcp_sent(t, n);
            continue;
        }
        if (t->fin_pending && off == t->tx_len && (!t->fin_sent || t->snd_nxt == t->fin_seq)) {
            if (tcp_output(t, F_FIN | F_ACK, t->snd_nxt, 0)<0) break;
            if (!t->fin_sent) {
                t->fin_sent = true;
                t->fin_seq = t->snd_nxt;
                if (t->state == T_ESTABLISHED) t->state = T_FIN_WAIT1;
                else if (t->state == T_CLOSE_WAIT) t->state = T_LAST_ACK;
            }
            tcp_sent(t, 1);
        }
        break;
    }
    /* data waiting behind a zero window: probe it from the retransmit timer */
    if (t->tx_len && t->snd_una == t->snd_max && t->snd_wnd == 0 && !t->tx_retry) tcp_arm(t);
    if (!t->tx_retry) t->probe_pending = false;
}

/* retransmit the oldest unacknowledged segment */
static void tcp_resend_first(struct tcb *t) {
    uint32_t n = MIN(t->tx_len, (uint32_t)t->mss);
    int result = 0;
    if (n) result = tcp_output(t, F_ACK | F_PSH, t->snd_una, n);
    else if (t->fin_sent && t->snd_una == t->fin_seq) result = tcp_output(t, F_FIN | F_ACK, t->fin_seq, 0);
    t->rtt_at = 0;
    t->resend_pending = result<0;
    if (!result && (n || (t->fin_sent && t->snd_una==t->fin_seq))) tcp_stats.retransmits++;
}

/* RFC 6298 2.2-2.4 on a round trip of r ns: RTO = SRTT + max(G, 4 RTTVAR), held to its bounds. */
static void tcp_rtt_sample(struct tcb *t, uint64_t r) {
    if (!r) r = 1; /* srtt 0 means no sample yet */
    if (!t->srtt) {
        t->srtt = r;
        t->rttvar = r / 2;
    } else {
        uint64_t d = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3 * t->rttvar + d) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
    }
    t->rto = MIN(MAX(t->srtt + MAX(4 * t->rttvar, RTO_G), RTO_MIN), RTO_MAX);
}

/* the congestion window starts at RFC 3390's initial window */
static void tcp_cc_init(struct tcb *t) {
    t->cwnd = MIN(4u * t->mss, MAX(2u * t->mss, 4380u));
    t->ssthresh = t->tx_capacity;
    t->recover = t->snd_una;
}

/* Grow metadata with actual gaps, not a fixed eight-range rejection. If memory
   is exhausted, do not advance rcv_nxt for unremembered data; the peer retries. */
static void tcp_ooo_add(struct tcb *t, uint32_t s, uint32_t e) {
    for (size_t i = 0; i < t->nooo;) {
        if (SEQ_LEQ(t->ooo[i].start, e) && SEQ_LEQ(s, t->ooo[i].end)) { /* overlaps or touches */
            if (SEQ_LT(t->ooo[i].start, s)) s = t->ooo[i].start;
            if (SEQ_LT(e, t->ooo[i].end)) e = t->ooo[i].end;
            t->ooo[i] = t->ooo[--t->nooo];
            i = 0;
        } else {
            i++;
        }
    }
    if (t->nooo==t->ooo_capacity) {
        size_t next = t->ooo_capacity ? t->ooo_capacity*2 : TCP_OOO;
        if (next<=t->ooo_capacity || next>SIZE_MAX/sizeof *t->ooo) return;
        struct tcp_range *grown = krealloc(t->ooo,next*sizeof *grown);
        if (!grown) return;
        t->ooo = grown;
        t->ooo_capacity = next;
    }
    t->ooo[t->nooo++] = (struct tcp_range){s,e};
}

/* after rcv_nxt moved: take in every stored range that now continues the in-order data */
static void tcp_ooo_pull(struct tcb *t) {
    for (size_t i = 0; i < t->nooo;) {
        if (SEQ_LEQ(t->ooo[i].start, t->rcv_nxt)) {
            if (SEQ_LT(t->rcv_nxt, t->ooo[i].end)) {
                t->rx_len += t->ooo[i].end - t->rcv_nxt;
                t->rcv_nxt = t->ooo[i].end;
            }
            t->ooo[i] = t->ooo[--t->nooo];
            i = 0;
        } else {
            i++;
        }
    }
}

static void tcp_reset(struct tcb *t, int err);

/* A listener that ends refuses (RST) every connection it has not handed out;
   tcp_timer_run then releases them, since nobody owns them. */
static void tcp_refuse_pending(struct tcb *listener) {
    for (size_t i=0;i<tcp_capacity;i++) {
        struct tcb *c=tcbs[i];
        if (!c || c->state==T_FREE || !c->accept_pending || c->listener!=listener->generation) continue;
        c->accept_pending=false;
        c->linger_until=0;
        tcp_reset(c,-N_ERR_CONNECTION_RESET);
    }
}

static void tcp_reset(struct tcb *t, int err) {
    if (t->state == T_LISTEN) tcp_refuse_pending(t);
    else if (t->state != T_SYN_SENT && t->state != T_CLOSED && t->state != T_FREE)
        tcp_output(t, F_RST | F_ACK, t->snd_nxt, 0);
    tcp_end(t, err);
}

static void tcp_segment(struct tcb *t, const uint8_t *p, size_t len);
static void tcp_input_held(size_t n);
static int tcp_alloc_slot(void);

/* A reset that belongs to no connection: replies to segments for a port with
   no listener (RFC 9293 3.10.7.1) and to ACKs a listener cannot accept. */
static void tcp_output_reset(uint32_t dst, const uint8_t mac[6], uint16_t sport, uint16_t dport,
                             uint32_t seq, uint32_t ack, uint8_t flags) {
    struct tcp_hdr *h = (struct tcp_hdr *)L4;
    h->sport = htons(sport);
    h->dport = htons(dport);
    h->seq = htonl(seq);
    h->ack = htonl(ack);
    h->off = 5 << 4;
    h->flags = flags;
    h->win = 0;
    h->csum = 0;
    h->urg = 0;
    h->csum = htons(l4_csum(ip_source(dst), dst, 6, L4, 20));
    if (!ip_output(ip_source(dst), dst, mac, 6, 20)) tcp_stats.segs_out++;
}

static void tcp_reset_unknown(uint32_t src, const uint8_t mac[6], const struct tcp_hdr *h, size_t len) {
    if (h->flags & F_RST) return; /* never answer a reset */
    size_t hl = (size_t)(h->off >> 4) * 4;
    uint32_t seg_len = (uint32_t)(len - hl) + ((h->flags & F_SYN) ? 1 : 0) + ((h->flags & F_FIN) ? 1 : 0);
    if (h->flags & F_ACK) tcp_output_reset(src, mac, ntohs(h->dport), ntohs(h->sport), ntohl(h->ack), 0, F_RST);
    else tcp_output_reset(src, mac, ntohs(h->dport), ntohs(h->sport), 0, ntohl(h->seq) + seg_len, F_RST | F_ACK);
}

/* MSS and Window Scale of a <SYN> or <SYN,ACK> (RFC 9293 3.7.1, RFC 7323 2) */
static void tcp_syn_options(struct tcb *t, const uint8_t *p, size_t hl) {
    t->mss = 536;
    for (size_t o = 20; o < hl;) {
        uint8_t kind = p[o];
        if (kind == 0) break;
        if (kind == 1) {
            o++;
            continue;
        }
        if (hl-o < 2) break;
        uint8_t l = p[o + 1];
        if (l < 2 || l > hl-o) break;
        if (kind == 2 && l == 4) {
            unsigned mss = (unsigned)p[o+2] << 8 | p[o+3];
            if (mss) t->mss = (uint16_t)MIN(mss,TCP_MSS);
        }
        if (kind == 3 && l == 3 && !t->window_scaled) {
            t->window_scaled = true;
            t->tx_shift = MIN(p[o+2],TCP_WINDOW_SHIFT_MAX);
        }
        o += l;
    }
}

static size_t tcp_listener_pending(const struct tcb *listener) {
    size_t n=0;
    for (size_t i=0;i<tcp_capacity;i++)
        if (tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->accept_pending && tcbs[i]->listener==listener->generation) n++;
    return n;
}

static struct tcb *tcp_listener_of(const struct tcb *t) {
    for (size_t i=0;i<tcp_capacity;i++)
        if (tcbs[i] && tcbs[i]->state==T_LISTEN && tcbs[i]->generation==t->listener) return tcbs[i];
    return NULL;
}

/* RFC 9293 3.10.7.2, LISTEN: a <SYN> makes a connection in SYN-RECEIVED that
   answers <SYN,ACK>; it waits for SYS_TCP_ACCEPT once its handshake ends. */
static void tcp_listen_input(struct tcb *l, uint32_t src, const uint8_t mac[6], const uint8_t *p, size_t len) {
    const struct tcp_hdr *h = (const struct tcp_hdr *)p;
    size_t hl = (size_t)(h->off >> 4) * 4;
    if (h->flags & F_RST) return;
    if (h->flags & F_ACK) {
        tcp_output_reset(src, mac, l->lport, ntohs(h->sport), ntohl(h->ack), 0, F_RST);
        return;
    }
    if (!(h->flags & F_SYN)) return;
    /* A full queue drops the <SYN>; the peer's retransmission tries again
       once the application has accepted. Storage is not reserved for it. */
    if (tcp_listener_pending(l) >= (size_t)l->backlog) return;
    int slot = tcp_alloc_slot();
    if (slot < 0) return;
    struct tcb *t = tcbs[slot];
    tcb_clear(t);
    t->rx_capacity = tcp_rx_target;
    t->tx_capacity = tcp_tx_target;
    while (!(t->rx=kmalloc(t->rx_capacity)) && t->rx_capacity>PAGE_SIZE) t->rx_capacity/=2;
    while (!(t->tx=kmalloc(t->tx_capacity)) && t->tx_capacity>PAGE_SIZE) t->tx_capacity/=2;
    if (!t->rx || !t->tx) {
        tcp_free(t);
        return;
    }
    t->pid = l->pid;
    if(++tcp_generation==0)++tcp_generation;
    t->generation = tcp_generation;
    t->listener = l->generation;
    t->accept_pending = true;
    while ((t->rx_capacity-1)>>t->rx_shift > UINT16_MAX) t->rx_shift++;
    t->rip = src;
    t->rport = ntohs(h->sport);
    t->lport = l->lport;
    memcpy(t->mac, mac, 6);
    tcp_syn_options(t, p, hl);
    uint32_t irs = ntohl(h->seq);
    t->rcv_nxt = irs + 1; /* data carried by the <SYN> is not kept; the peer resends it */
    t->snd_wnd = ntohs(h->win); /* a <SYN> window is never scaled */
    t->snd_wl1 = irs;
    t->iss = rnd();
    t->snd_una = t->snd_wl2 = t->iss;
    t->snd_nxt = t->snd_max = t->iss;
    t->rto = RTO_INITIAL;
    t->state = T_SYN_RECEIVED;
    if (!tcp_output(t, F_SYN | F_ACK, t->iss, 0)) {
        t->snd_nxt = t->snd_max = t->iss + 1;
        tcp_arm(t);
    }
}

static void tcp_input(uint32_t src, uint32_t dst, const uint8_t *src_mac, const uint8_t *p, size_t len) {
    if (len < 20 || l4_csum(src, dst, 6, p, len) != 0) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)p;
    size_t hl = (size_t)(h->off >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    struct tcb *t = NULL, *listener = NULL;
    for(size_t i=0;i<tcp_capacity;i++) {
        struct tcb *c=tcbs[i];
        if(!c || c->state==T_FREE) continue;
        if(c->state==T_LISTEN) { /* a listener on this very address before one on every address */
            if(c->lport==dport && (c->lip==dst || (!c->lip && !listener))) listener=c;
        }
        /* a reused tuple may still have its closed predecessor lingering */
        else if(c->rip==src && c->rport==sport && c->lport==dport && (!t || t->state==T_CLOSED)) t=c;
    }
    if (!t || (t->state == T_CLOSED && listener && (h->flags & (F_SYN|F_ACK|F_RST)) == F_SYN)) {
        uint8_t mac[6];
        memcpy(mac, src_mac, 6); /* the frame's sender is the next hop back */
        tcp_stats.segs_in++;
        if (listener) tcp_listen_input(listener, src, mac, p, len);
        else if (!t) tcp_reset_unknown(src, mac, h, len);
        return;
    }
    if (t->state == T_CLOSED) return;
    tcp_stats.segs_in++;
    if (t->pid == fault_pid && len > hl && t->state != T_SYN_SENT) {
        if (fault_hit(fault.rx_drop)) {
            tcp_stats.faults_dropped++;
            return;
        }
        if (!fault_held_len && len <= sizeof fault_held && fault_hit(fault.rx_reorder)) {
            /* hold this segment back and deliver it after the next one */
            memcpy(fault_held, p, len);
            fault_held_len = len;
            fault_held_src = src;
            fault_held_until = clock_after(FAULT_HOLD);
            net_timer_at(&fault_timer, fault_held_until);
            tcp_stats.faults_reordered++;
            return;
        }
    }
    tcp_segment(t, p, len);
    if (fault_held_len && src == fault_held_src) {
        size_t n = fault_held_len;
        fault_held_len = 0;
        tcp_input_held(n);
    }
}

static void tcp_segment(struct tcb *t, const uint8_t *p, size_t len) {
    const struct tcp_hdr *h = (const struct tcp_hdr *)p;
    size_t hl = (size_t)(h->off >> 4) * 4;
    uint8_t fl = h->flags;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    const uint8_t *data = p + hl;
    uint32_t dlen = (uint32_t)(len - hl);

    /* RFC 9293 3.10.7.3-4: a reset is acceptable when it acknowledges our <SYN> (SYN-SENT) or
       lies in the receive window; any other one is dropped, the connection and its timers as
       they were. */
    if (fl & F_RST) {
        if (t->state == T_SYN_SENT) {
            if ((fl & F_ACK) && ack == t->iss + 1) tcp_end(t, -N_ERR_CONNECTION_REFUSED);
        } else if (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + MAX(t->adv_wnd, 1u))) {
            tcp_end(t, -N_ERR_CONNECTION_RESET);
        }
        return;
    }

    if (t->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != t->iss + 1) return;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = ntohs(h->win); /* SYN-ACK is NEVER shifted */
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        tcp_syn_options(t, p, hl);
        t->state = T_ESTABLISHED;
        t->rto_at = 0;
        t->retries = 0;
        t->rto = RTO_INITIAL;
        t->snd_max = t->snd_nxt;
        tcp_cc_init(t);
        tcp_output(t, F_ACK, t->snd_nxt, 0);
        tcp_progress(t);
        tcp_push(t, false);
        return;
    }

    /* RFC 9293 3.10.7.4, SYN-RECEIVED: the ACK of our <SYN,ACK> ends the
       passive open; the same segment may already carry data or a FIN. */
    if (t->state == T_SYN_RECEIVED) {
        if (fl & F_SYN) {
            /* the peer did not hear our <SYN,ACK>: answer its <SYN> again */
            if (!(fl & F_ACK) && seq == t->rcv_nxt - 1) tcp_output(t, F_SYN | F_ACK, t->iss, 0);
            return;
        }
        if (!(fl & F_ACK)) return;
        if (!SEQ_LT(t->snd_una, ack) || !SEQ_LEQ(ack, t->snd_max)) {
            tcp_output_reset(t->rip, t->mac, t->lport, t->rport, ack, 0, F_RST);
            return;
        }
        t->state = T_ESTABLISHED;
        t->snd_una = ack;
        t->snd_wnd = tcp_peer_window(t, h);
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        t->rto_at = 0;
        t->retries = 0;
        t->rto = RTO_INITIAL;
        tcp_cc_init(t);
        t->accept_order = ++tcp_accept_counter;
        struct tcb *listener = tcp_listener_of(t);
        if (listener) tcp_progress(listener);
    }

    bool need_ack = false, immediate_ack = false;
    uint32_t peer_window = tcp_peer_window(t,h);
    if (fl & F_ACK) {
        if (SEQ_LT(t->snd_max,ack)) {
            tcp_output(t,F_ACK,t->snd_nxt,0);
            return; /* a peer must not acknowledge bytes never submitted */
        }
        if (SEQ_LT(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_max)) {
            uint32_t acked = ack - t->snd_una;
            bool fin_acked = t->fin_sent && ack == t->fin_seq + 1;
            uint32_t data_acked = acked - (fin_acked ? 1 : 0);
            if (data_acked > t->tx_len) data_acked = t->tx_len;
            t->tx_head = (t->tx_head+data_acked)%t->tx_capacity;
            t->tx_len -= data_acked;
            t->snd_una = ack;
            if (SEQ_LT(t->snd_nxt, ack)) t->snd_nxt = ack; /* the peer had kept what we were resending */
            if (t->rtt_at && SEQ_LEQ(t->rtt_seq, ack)) {
                tcp_rtt_sample(t, ns_between(t->rtt_at, clock_now_ns()));
                t->rtt_at = 0;
            }
            t->retries = 0;
            t->rto_at = 0;
            /* congestion control */
            if (t->in_recovery) {
                if (SEQ_LEQ(t->recover, ack)) { /* everything sent before the loss is acked */
                    t->in_recovery = false;
                    t->cwnd = t->ssthresh;
                } else { /* partial ack: the next hole was lost too */
                    tcp_resend_first(t);
                    t->cwnd = (t->cwnd > acked ? t->cwnd - acked : 0) + t->mss;
                }
            } else if (t->cwnd < t->ssthresh) {
                t->cwnd += MIN(acked, (uint32_t)t->mss); /* slow start */
            } else {
                t->cwnd += MAX((uint32_t)t->mss * t->mss / t->cwnd, 1u); /* congestion avoidance */
            }
            t->cwnd = MIN(t->cwnd, t->tx_capacity);
            t->dupacks = 0;
            if (t->snd_una != t->snd_max) tcp_arm(t);
            if (fin_acked) {
                if (t->state == T_FIN_WAIT1) t->state = T_FIN_WAIT2;
                else if (t->state == T_CLOSING) tcp_time_wait(t);
                else if (t->state == T_LAST_ACK) tcp_end(t, 0);
            }
            tcp_progress(t);
        } else if (ack == t->snd_una && t->snd_una != t->snd_max && dlen == 0 && !(fl & (F_SYN | F_FIN)) &&
                   peer_window == t->snd_wnd) {
            /* duplicate ack: a segment after snd_una arrived, so (probably) snd_una's was lost */
            tcp_stats.dupacks_in++;
            if (++t->dupacks == 3 && !t->in_recovery && SEQ_LT(t->recover, ack)) {
                t->ssthresh = MAX((t->snd_max - t->snd_una) / 2, 2u * t->mss);
                t->recover = t->snd_max;
                tcp_resend_first(t);
                tcp_stats.fast_retransmits++;
                t->cwnd = t->ssthresh + 3u * t->mss;
                t->in_recovery = true;
            } else if (t->in_recovery) {
                t->cwnd = MIN(t->cwnd + t->mss, t->tx_capacity); /* storage-backed flight window */
            }
        }
        if (SEQ_LEQ(t->snd_una,ack) && (SEQ_LT(t->snd_wl1,seq) ||
            (t->snd_wl1==seq && SEQ_LEQ(t->snd_wl2,ack)))) {
            t->snd_wnd = peer_window;
            t->snd_wl1 = seq;
            t->snd_wl2 = ack;
        }
    }

    /* data: trim what we already have, store the rest at its place in the ring (it may be ahead of
       rcv_nxt, after a lost or reordered segment), then move rcv_nxt over everything contiguous */
    if (dlen) {
        need_ack = true;
        immediate_ack = seq!=t->rcv_nxt || t->nooo!=0;
        if (SEQ_LT(seq, t->rcv_nxt)) {
            uint32_t old = t->rcv_nxt - seq;
            if (old >= dlen) old = dlen;
            data += old;
            dlen -= old;
            seq += old;
        }
    }
    bool in_order = seq == t->rcv_nxt;
    if (dlen) {
        if (!t->rx_fin && (t->state == T_ESTABLISHED || t->state == T_FIN_WAIT1 || t->state == T_FIN_WAIT2)) {
            uint32_t space = t->rx_capacity - t->rx_len, off = seq - t->rcv_nxt;
            if (off < space) {
                uint32_t n = MIN(dlen, space - off), at = t->rx_head + t->rx_len + off;
                tcp_ring_write(t->rx,t->rx_capacity,at,data,n);
                dlen = n;
                if (off == 0) {
                    t->rx_len += n;
                    t->rcv_nxt += n;
                    tcp_ooo_pull(t);
                    in_order = true;
                    if (n>=TCP_MSS) t->ack_segments++;
                    tcp_progress(t);
                } else {
                    tcp_ooo_add(t, seq, seq + n);
                    tcp_stats.ooo_in++;
                }
            }
        }
    }
    if ((fl & F_FIN) && in_order && seq + dlen == t->rcv_nxt && !t->rx_fin) {
        t->rcv_nxt++;
        t->rx_fin = true;
        need_ack = immediate_ack = true;
        if (t->state == T_ESTABLISHED) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1) t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT2) tcp_time_wait(t);
        tcp_progress(t);
    }
    if (need_ack) {
        t->ack_pending = true;
        /* RFC 5681: at least every second full-sized segment, immediately
           for gaps/duplicates/FIN. The remaining ACK is flushed before the
           drain ends, not held behind an arbitrary delayed-ACK timer. */
        if (immediate_ack || t->ack_segments>=2 || !receive_batch)
            tcp_output(t, F_ACK, t->snd_nxt, 0);
    }
    tcp_push(t, false);
}

/* A connection's timer runs (struct net_timer): TIME-WAIT's end, the end of one no app holds, the
   retransmission (a <SYN>'s, data's, or a zero-window probe) due by now; then it is armed for the
   next of them. */
static void tcp_timer_run(struct net_timer *nt, uint64_t now) {
    struct tcb *t = (struct tcb *)((uint8_t *)nt - offsetof(struct tcb, timer));
    if (t->state == T_FREE) return; /* fired for a connection that has gone since */
    if (t->state == T_TIME_WAIT && now >= t->linger_until) tcp_end(t, 0);
    /* a connection waiting for accept belongs to its listener, not to a linger timer */
    if (!t->user_open && (t->state == T_CLOSED || (!t->accept_pending && now >= t->linger_until))) {
        if (t->state != T_CLOSED) tcp_reset(t, -N_ERR_CONNECTION_RESET);
        tcp_free(t);
        return;
    }
    if (t->rto_at && now >= t->rto_at) {
        t->rto_at = 0;
        bool syn = t->state == T_SYN_SENT || t->state == T_SYN_RECEIVED;
        bool probe = !syn && t->snd_wnd == 0; /* persist: the peer is just not reading */
        if (!probe && ++t->retries > (syn ? 5 : 8)) {
            tcp_reset(t, -N_ERR_TIMEOUT);
        } else {
            t->rto = MIN(t->rto * 2, RTO_MAX);
            if (syn) {
                if (!tcp_output(t, t->state == T_SYN_SENT ? F_SYN : F_SYN | F_ACK, t->iss, 0)) {
                    t->snd_nxt = t->snd_max = t->iss+1;
                    tcp_arm(t);
                }
            } else {
                if (!probe) {
                    /* a timeout is a strong congestion signal: back to one segment (RFC 5681) */
                    t->ssthresh = MAX((t->snd_max - t->snd_una) / 2, 2u * t->mss);
                    t->cwnd = t->mss;
                    t->in_recovery = false;
                    t->dupacks = 0;
                    t->recover = t->snd_max;
                    t->rtt_at = 0;
                    tcp_stats.retransmits++;
                    tcp_stats.timeouts++;
                }
                /* go back to the oldest unacknowledged byte */
                t->snd_nxt = t->snd_una;
                t->resend_pending = false;
                tcp_push(t, true);
                if (t->snd_una != t->snd_max && !t->tx_retry) tcp_arm(t);
            }
        }
    }
    /* The next look: the retransmission, TIME-WAIT's end, or the end of one no app holds (at
       once when it is over already). */
    uint64_t at = t->rto_at ? t->rto_at : CLOCK_NEVER;
    if (t->state == T_TIME_WAIT || (!t->user_open && !t->accept_pending)) at = MIN(at, t->linger_until);
    if (!t->user_open && t->state == T_CLOSED) at = 0;
    net_timer_at(nt, at);
}

/* fault_timer's work: a held-back (reordered) segment whose successor never came goes in. */
static void fault_timer_run(struct net_timer *nt, uint64_t now) {
    if (!fault_held_len) return;
    if (now < fault_held_until) { /* fired for an earlier hold */
        net_timer_at(nt, fault_held_until);
        return;
    }
    size_t n = fault_held_len;
    fault_held_len = 0;
    tcp_input_held(n);
}

static void tcp_input_held(size_t n) {
    static uint8_t seg[sizeof fault_held];
    memcpy(seg, fault_held, n);
    const struct tcp_hdr *h = (const struct tcp_hdr *)seg;
    for(size_t i=0;i<tcp_capacity;i++)
        if(tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->state!=T_CLOSED && tcbs[i]->rip==fault_held_src &&
           tcbs[i]->rport==ntohs(h->sport) && tcbs[i]->lport==ntohs(h->dport)) {
            tcp_segment(tcbs[i],seg,n);
            return;
        }
}

static struct tcb *user_tcb(int h) {
    if(h<0 || (size_t)h>=tcp_capacity)return NULL;
    struct tcb *t=tcbs[h];
    return t && t->state!=T_FREE && t->user_open && t->pid==process_current()->pid?t:NULL;
}

static int tcp_alloc_slot(void) {
    size_t slot=0;
    while(slot<tcp_capacity && tcbs[slot] && tcbs[slot]->state!=T_FREE)slot++;
    if(slot==tcp_capacity) {
        size_t next=tcp_capacity<=((size_t)0x7fffffff+1)/2?tcp_capacity*2:(size_t)0x7fffffff+1;
        if(next<=tcp_capacity || next>SIZE_MAX/sizeof *tcbs)return -N_ERR_NO_MEMORY;
        struct tcb **grown=kmalloc(next*sizeof *grown);if(!grown)return -N_ERR_NO_MEMORY;
        memcpy(grown,tcbs,tcp_capacity*sizeof *grown);
        memset(grown+tcp_capacity,0,(next-tcp_capacity)*sizeof *grown);
        if(tcbs!=tcp_initial_slots)kfree(tcbs);
        tcbs=grown;tcp_capacity=next;
    }
    if(!tcbs[slot]) {
        struct tcb *t=kmalloc(sizeof *t);if(!t)return -N_ERR_NO_MEMORY;
        memset(t,0,sizeof *t);net_timer_init(&t->timer,tcp_timer_run);tcbs[slot]=t;
    }
    return (int)slot;
}
static uint16_t tcp_local_port(uint32_t ip,uint16_t remote) {
    for(unsigned attempt=0;attempt<16384;attempt++) {
        uint16_t port=ephemeral_port();bool occupied=false;
        for(size_t i=0;i<tcp_capacity;i++)
            if(tcbs[i] && tcbs[i]->state!=T_FREE && ((tcbs[i]->rip==ip && tcbs[i]->rport==remote) ||
               tcbs[i]->state==T_LISTEN) && tcbs[i]->lport==port){occupied=true;break;}
        if(!occupied)return port;
    }
    return 0;
}

static bool tcp_port_listening(uint32_t ip, uint16_t port) {
    for(size_t i=0;i<tcp_capacity;i++)
        if(tcbs[i] && tcbs[i]->state==T_LISTEN && tcbs[i]->lport==port &&
           (!ip || !tcbs[i]->lip || tcbs[i]->lip==ip))return true;
    return false;
}

static int64_t sys_tcp_listen(uint32_t ip, uint16_t port, int backlog) {
    if (ip && !ip_is_local(ip)) return -N_ERR_INVALID;
    if (!port) {
        /* an ephemeral port no listener or connection is using */
        for (unsigned attempt=0;attempt<16384 && !port;attempt++) {
            uint16_t candidate=ephemeral_port();
            bool used=false;
            for (size_t i=0;i<tcp_capacity && !used;i++)
                used=tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->lport==candidate;
            if (!used) port=candidate;
        }
        if (!port) return -N_ERR_ADDRESS_IN_USE;
    } else if (tcp_port_listening(ip, port)) return -N_ERR_ADDRESS_IN_USE;
    int h=tcp_alloc_slot();if(h<0)return h;
    struct tcb *t=tcbs[h];
    tcb_clear(t);
    t->pid=process_current()->pid;
    if(++tcp_generation==0)++tcp_generation;
    t->generation=tcp_generation;
    t->user_open=true;
    t->lip=ip;
    t->lport=port;
    t->backlog=backlog>0?backlog:TCP_BACKLOG_DEFAULT;
    t->state=T_LISTEN;
    return h;
}

static struct tcb *tcp_next_accept(const struct tcb *listener) {
    struct tcb *best=NULL;
    for (size_t i=0;i<tcp_capacity;i++) {
        struct tcb *c=tcbs[i];
        if (!c || !c->accept_pending || c->listener!=listener->generation) continue;
        if (c->state!=T_ESTABLISHED && c->state!=T_CLOSE_WAIT) continue;
        if (!best || c->accept_order<best->accept_order) best=c;
    }
    return best;
}

static int64_t sys_tcp_accept(int h, uint64_t deadline, struct n_sockaddr *remote) {
    struct tcb *l=user_tcb(h);
    if (!l) return -N_ERR_NO_HANDLE;
    if (l->state!=T_LISTEN && l->state!=T_CLOSED) return -N_ERR_INVALID;
    if (remote && !user_ok_w(remote,sizeof *remote)) return -N_ERR_BAD_ADDRESS; /* before the wait */
    uint64_t generation=l->generation;
    for (;;) {
        if (l->generation!=generation || !l->user_open) return -N_ERR_NO_HANDLE;
        if (l->state!=T_LISTEN) return l->err?l->err:-N_ERR_CLOSED;
        struct tcb *c=tcp_next_accept(l);
        if (c) {
            /* a sibling thread may have revoked the page while this one slept:
               the connection then stays for a later accept */
            struct n_sockaddr peer={.ip=c->rip,.port=c->rport};
            if (remote && copy_to_user(remote,&peer,sizeof peer)<0) return -N_ERR_BAD_ADDRESS;
            c->accept_pending=false;
            c->user_open=true;
            for (size_t i=0;i<tcp_capacity;i++) if (tcbs[i]==c) return (int64_t)i;
        }
        if (thread_current()->killed) return -N_ERR_STOPPED;
        if (clock_now_ns()>=deadline) return -N_ERR_NOT_READY;
        net_sleep(&l->progress, deadline, false);
    }
}

/* An active open, the address resolution and the handshake both by deadline: the connection's
   number, or an error. */
static int64_t tcp_connect_until(uint32_t ip, uint16_t port, uint64_t deadline) {
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    uint8_t mac[6];
    int r = arp_resolve_until(ip, mac, deadline);
    if (r < 0) return r;
    if(thread_current()->killed)return -N_ERR_STOPPED;
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    if(clock_now_ns()>=deadline)return -N_ERR_TIMEOUT;
    uint16_t local=tcp_local_port(ip,port);
    if(!local)return -N_ERR_ADDRESS_IN_USE;
    int h=tcp_alloc_slot();if(h<0)return h;
    struct tcb *t=tcbs[h];
    tcb_clear(t);
    t->rx_capacity = tcp_rx_target;
    t->tx_capacity = tcp_tx_target;
    /* Buffer size is real backing storage, not a transfer rejection. Allow
       a connection under memory pressure, negotiating its actual capacity. */
    while (!(t->rx=kmalloc(t->rx_capacity)) && t->rx_capacity>PAGE_SIZE) t->rx_capacity/=2;
    while (!(t->tx=kmalloc(t->tx_capacity)) && t->tx_capacity>PAGE_SIZE) t->tx_capacity/=2;
    if (!t->rx || !t->tx) {
        tcp_free(t);
        return -N_ERR_NO_MEMORY;
    }
    t->pid = process_current()->pid;
    if(++tcp_generation==0)++tcp_generation;
    t->generation=tcp_generation;
    t->user_open = true;
    while ((t->rx_capacity-1)>>t->rx_shift > UINT16_MAX) t->rx_shift++;
    t->rip = ip;
    t->rport = port;
    t->lport=local;
    memcpy(t->mac, mac, 6);
    t->iss = rnd();
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss;
    t->mss = 536;
    t->rto = RTO_INITIAL;
    t->state = T_SYN_SENT;
    if (!tcp_output(t, F_SYN, t->iss, 0)) {
        t->snd_nxt = t->snd_max = t->iss+1;
        tcp_arm(t);
    }
    uint64_t generation=t->generation;
    while (t->state == T_SYN_SENT) {
        if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
        bool cancelled=net_setup_cancelled();
        if (thread_current()->killed || cancelled || clock_now_ns() >= deadline) {
            t->state = T_CLOSED;
            t->err = thread_current()->killed ? -N_ERR_STOPPED : cancelled ? -N_ERR_CANCELLED : -N_ERR_TIMEOUT;
            break;
        }
        net_sleep(&t->progress, deadline, true);
    }
    if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
    if(net_setup_cancelled()) {
        tcp_reset(t,-N_ERR_CANCELLED);tcp_free(t);return -N_ERR_CANCELLED;
    }
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT) {
        int err = t->err ? t->err : -N_ERR_CONNECTION_REFUSED;
        tcp_free(t);
        return err;
    }
    return h;
}

/* Queue len bytes for sending, waiting for room until deadline (CLOCK_NEVER: no limit): how many
   were queued, or an error when none were. */
static int64_t tcp_send_data(int h, const uint8_t *buf, size_t len, uint64_t deadline, bool user, bool nonblock) {
    struct tcb *t = user_tcb(h);
    if (!t) return -N_ERR_NO_HANDLE;
    uint64_t generation=t->generation;
    size_t done = 0;
    while (done < len) {
        if(t->generation!=generation || !t->user_open)return done?(int64_t)done:-N_ERR_NO_HANDLE;
        if(!user && net_setup_cancelled())return done?(int64_t)done:-N_ERR_CANCELLED;
        if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT) return done ? (int64_t)done : (t->err ? t->err : -N_ERR_CLOSED);
        if (t->fin_pending) return done ? (int64_t)done : -N_ERR_CLOSED;
        if (thread_current()->killed) return done ? (int64_t)done : -N_ERR_STOPPED;
        if (clock_now_ns() >= deadline) return done ? (int64_t)done : -N_ERR_TIMEOUT;
        uint32_t space = t->tx_capacity - t->tx_len;
        if (!space) {
            if(nonblock)return done?(int64_t)done:-N_ERR_NOT_READY;
            net_sleep(&t->progress, deadline, !user);
            continue;
        }
        /* A nonblocking call enqueues one chunk at once; a blocking one goes on chunk by chunk. */
        uint32_t n = (uint32_t)MIN(MIN(len - done, (size_t)space), (size_t)TCP_COPY_CHUNK);
        /* Copy exactly the chunk we will enqueue, not the entire request
           on every iteration. A late fault preserves the completed prefix. */
        if ((uint64_t)buf>UINT64_MAX-done) return done?(int64_t)done:-N_ERR_BAD_ADDRESS;
        uint32_t at = t->tx_head+t->tx_len;
        if (!user) tcp_ring_write(t->tx,t->tx_capacity,at,buf+done,n);
        else if (tcp_ring_write_user(t->tx,t->tx_capacity,at,buf+done,n)<0) return done?(int64_t)done:-N_ERR_BAD_ADDRESS;
        t->tx_len += n;
        done += n;
        tcp_push(t, false);
        if(nonblock)return (int64_t)done;
    }
    return (int64_t)done;
}

/* Take up to len received bytes, waiting for some until deadline (-N_ERR_NOT_READY once it has
   come): how many, 0 at the end of the stream. */
static int64_t tcp_recv_data(int h, uint8_t *buf, size_t len, uint64_t deadline, bool user) {
    struct tcb *t = user_tcb(h);
    if (!t) return -N_ERR_NO_HANDLE;
    if (t->state == T_LISTEN) return -N_ERR_INVALID; /* a listener carries no data */
    if (!len) return 0;
    uint64_t generation=t->generation;
    if(!user && net_setup_cancelled())return -N_ERR_CANCELLED;
    while (t->rx_len == 0) {
        if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
        if(!user && net_setup_cancelled())return -N_ERR_CANCELLED;
        if (t->rx_fin) return 0;
        if (t->state == T_CLOSED) return t->err ? t->err : 0;
        if (thread_current()->killed) return -N_ERR_STOPPED;
        if (clock_now_ns() >= deadline) return -N_ERR_NOT_READY;
        net_sleep(&t->progress, deadline, !user);
    }
    if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
    /* At most one chunk a call (a short read is ordinary TCP), so net_lock never stays held
       across a long copy. A page that went keeps the bytes it did not take for a later read. */
    uint32_t n = (uint32_t)MIN(MIN(len, (size_t)t->rx_len), (size_t)TCP_COPY_CHUNK);
    if (!user) tcp_ring_read(buf,t->rx,t->rx_capacity,t->rx_head,n);
    else {
        int64_t got = tcp_ring_read_user(buf,t->rx,t->rx_capacity,t->rx_head,n);
        if (got < 0) return got;
        n = (uint32_t)got;
    }
    t->rx_head = (t->rx_head + n) % t->rx_capacity;
    t->rx_len -= n;
    /* window update if we had been squeezing the sender */
    uint32_t available = tcp_window(t);
    /* RFC 1122 receiver SWS avoidance: reopen at half the actual receive
       storage or one full segment, whichever is smaller. */
    uint32_t threshold = MIN(t->rx_capacity/2,TCP_MSS);
    if (t->state != T_CLOSED && available>t->adv_wnd &&
        (t->adv_wnd==0 || available-t->adv_wnd>=threshold))
        tcp_output(t, F_ACK, t->snd_nxt, 0);
    return n;
}

static void tcp_close(struct tcb *t) {
    t->user_open = false;
    /* Native TCP handles are not VFS descriptors. The helper's subsequent
       pipe write/close publishes poll readiness; a network packet itself
       must not wake every GUI/pipe poller before any fd can have changed. */
    progress_wake(&t->progress);
    t->linger_until = clock_after(TCP_ORPHAN_LINGER);
    if (t->state == T_LISTEN) {
        tcp_refuse_pending(t);
        tcp_free(t);
    } else if (t->state == T_SYN_SENT || t->state == T_CLOSED) {
        tcp_free(t);
    } else {
        if (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) {
            t->fin_pending = true;
            tcp_push(t, false);
        }
        net_timer_at(&t->timer, t->linger_until);
    }
}

/* RFC 1035 TCP framing: two-byte length followed by exactly that many bytes.
   Both prefix and payload share one budget; short reads are ordinary TCP. */
static int dns_tcp_exchange(uint32_t resolver,uint16_t port,uint64_t deadline,
                            const uint8_t *query,size_t query_len,uint8_t **reply,size_t *reply_len) {
    *reply=NULL;*reply_len=0;
    if(clock_now_ns()>=deadline)return -N_ERR_TIMEOUT;
    int handle=(int)tcp_connect_until(resolver,port,deadline);
    if (handle<0) return handle;
    uint8_t prefix[2]={(uint8_t)(query_len>>8),(uint8_t)query_len};
    int result=0;
    long n=tcp_send_data(handle,prefix,2,deadline,false,false);
    if (n!=2) {result=n<0?(int)n:net_setup_cancelled()?-N_ERR_CANCELLED:-N_ERR_TIMEOUT;goto close;}
    n=tcp_send_data(handle,query,query_len,deadline,false,false);
    if (n!=(long)query_len) {result=n<0?(int)n:net_setup_cancelled()?-N_ERR_CANCELLED:-N_ERR_TIMEOUT;goto close;}
    size_t done=0;
    while (done<2) {
        if (clock_now_ns()>=deadline) {result=-N_ERR_TIMEOUT;goto close;}
        n=tcp_recv_data(handle,prefix+done,2-done,deadline,false);
        if (n<=0) {result=n<0?(int)n:-N_ERR_PROTOCOL;goto close;} done+=(size_t)n;
    }
    size_t length=(size_t)prefix[0]<<8|prefix[1];
    if (length<12) {result=-N_ERR_PROTOCOL;goto close;}
    uint8_t *response=kmalloc(length);if (!response) {result=-N_ERR_NO_MEMORY;goto close;}
    done=0;
    while (done<length) {
        if (clock_now_ns()>=deadline) {result=-N_ERR_TIMEOUT;break;}
        n=tcp_recv_data(handle,response+done,length-done,deadline,false);
        if (n<=0) {result=n<0?(int)n:-N_ERR_PROTOCOL;break;} done+=(size_t)n;
    }
    if (!result) {*reply=response;*reply_len=length;} else kfree(response);
close:
    if (result) tcp_reset(tcbs[handle],result);
    tcp_close(tcbs[handle]);
    return result;
}

/* ---------------------------------------------------------------- input dispatch, thread, syscalls */

static void eth_input(const void *packet, size_t len) {
    const uint8_t *p = packet;
    if (len<sizeof(struct eth_hdr)) return;
    const struct eth_hdr *e = (const struct eth_hdr *)p;
    if (memcmp(e->dst, link_mac, 6) != 0 && !(e->dst[0] & 1)) return; /* not for us (promiscuous NICs) */
    rx_packets++;
    net_service_bytes+=len;
    uint16_t type = ntohs(e->type);
    if (type == 0x0806) arp_input(p + 14, len - 14);
    else if (type == 0x0800) {
        const uint8_t *ipp = p + 14;
        size_t iplen = len - 14;
        const struct ip_hdr *ip = (const struct ip_hdr *)ipp;
        if (iplen < 20 || (ip->vihl >> 4) != 4) return;
        size_t hl = (size_t)(ip->vihl & 15) * 4, tl = ntohs(ip->len);
        if (hl < 20 || tl < hl || tl > iplen) return;
        if (csum_fold(csum_add(0, ip, hl)) != 0) return;
        if (ntohs(ip->frag) & 0x3FFF) return; /* fragments are not supported */
        uint32_t src = ip_get(ip->src), dst = ip_get(ip->dst);
        /* 127/8 is only ever this system talking to itself (ip_loopback), never the wire */
        bool loopback = (dst >> 24) == 127;
        if (loopback && memcmp(e->src, link_mac, 6) != 0) return;
        if (configured && dst != my_ip && !loopback && dst != 0xFFFFFFFF && dst != (my_ip | ~my_mask)) return;
        if (ip->proto == 1) icmp_input(e->src, src, ipp + hl, tl - hl);
        else if (ip->proto == 17) udp_input(src, dst, ipp + hl, tl - hl);
        else if (ip->proto == 6 && (configured || loopback)) tcp_input(src, dst, e->src, ipp + hl, tl - hl);
    }
}

/* The output that waited: ACKs held for the end of a batch, segments the adapter had no room
   for, retransmissions it refused. */
static void tcp_flush_pending(void) {
    for (size_t i=0;i<tcp_capacity;i++) {
        struct tcb *t = tcbs[i];
        if (!t || t->state==T_FREE || t->state==T_CLOSED) continue;
        if ((t->state==T_SYN_SENT || t->state==T_SYN_RECEIVED) && t->tx_retry) {
            uint8_t flags=t->state==T_SYN_SENT?F_SYN:F_SYN|F_ACK;
            if (!tcp_output(t,flags,t->iss,0)) {
                t->snd_nxt = t->snd_max = t->iss+1;
                t->tx_retry = false;
                tcp_arm(t);
            }
        } else {
            if (t->ack_pending) tcp_output(t,F_ACK,t->snd_nxt,0);
            if (t->resend_pending) tcp_resend_first(t);
            if (t->tx_retry && !t->resend_pending) tcp_push(t,t->probe_pending);
        }
    }
}

/* Several short NIC batches may share one service window: at its end (or once this many bytes
   went through it) the system calls waiting for net_lock go first, the thread waiting at most
   NET_TURN_WAIT for one of them to take it. */
#define NET_SERVICE_WINDOW (2 * NS_PER_MS)
#define NET_SERVICE_BYTES  (256u * 1024u)
#define NET_TURN_WAIT      (2 * NS_PER_MS)

/* The stack's own thread: frames in, the output that waited, the timers that fired. One batch at
   a time under net_lock; without it, it sleeps on rxq_wq until there is work: a frame queued, a
   driver's notice (net_rx_notify: an interrupt, room to send again), a timer's alarm. It never
   wakes only to look. */
static void net_thread(void *arg) {
    (void)arg;
    /* A no-work wait is already a scheduler boundary; do not first requeue the net task only to
       run it again and block immediately afterwards. */
    uint64_t service_started=0,service_start_bytes=0;
    bool serviced=false;
    mutex_lock(&net_lock);
    for (;;) {
        bool device_ready = nif->rx_pending && nif->rx_pending();
        uint64_t service_bytes = net_service_bytes;
        mutex_unlock(&net_lock);
        /* Every producer, and every timer's fire, sets net_work_pending under rxq_lock before it
           wakes rxq_wq: looked at under that lock, an arrival after the look finds this thread
           enrolled. */
        uint64_t f = spin_lock(&rxq_lock);
        bool ready = device_ready || rxq_tail != rxq_head || net_work_pending || net_due_first;
        if (!ready) {
            wq_wait_locked(&rxq_wq, &rxq_lock);
            spin_take(&rxq_lock);
        }
        net_work_pending = false;
        spin_unlock(&rxq_lock, f);
        uint64_t now=clock_now_ns();
        if(!ready || !serviced) {
            service_started=now;service_start_bytes=service_bytes;serviced=false;
        } else if(now>=service_started+NET_SERVICE_WINDOW || service_bytes-service_start_bytes>=NET_SERVICE_BYTES) {
            /* One of the system calls waiting for net_lock takes it before the next batch
               (net_enter wakes this thread when one has). Without them, honor priority
               rescheduling. Each NIC poll is bounded. */
            if (__atomic_load_n(&net_callers_waiting, __ATOMIC_SEQ_CST)) {
                uint64_t seen = __atomic_load_n(&net_turn.generation, __ATOMIC_SEQ_CST);
                __atomic_store_n(&net_turn_wanted, true, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&net_callers_waiting, __ATOMIC_SEQ_CST))
                    wq_wait_unless(&net_turn.wq, &net_turn.generation, seen, clock_after(NET_TURN_WAIT));
                __atomic_store_n(&net_turn_wanted, false, __ATOMIC_SEQ_CST);
            } else {
                sched_yield();
            }
            service_started=clock_now_ns();service_start_bytes=service_bytes;serviced=false;
        }
        mutex_lock(&net_lock);
        receive_batch = true;
        if (nif->poll_rx) nif->poll_rx(eth_input);
        rxq_drain();
        /* End-of-drain ACK and per-socket readiness, once per connection.
           Resource-blocked sends retry on NIC completion, not a fictitious
           packet loss/retransmission timeout. */
        tcp_flush_pending();
        receive_batch = false;
        for (size_t i=0;i<tcp_capacity;i++) {
            struct tcb *t = tcbs[i];
            if (t && t->progress_pending) { t->progress_pending=false; progress_wake(&t->progress); }
        }
        net_timers_run();
        serviced=true;
    }
}

static bool net_initialized, net_running;

/* net_lock held: once both net_init has run and an adapter is there, the stack starts (true:
   the caller makes its thread, without the lock). */
static bool net_start_locked(void) {
    if (!nif || !net_initialized || net_running) return false;
    net_running = true;
    dhcp_start();
    return true;
}

/* Adapters found by net_init's probes register before it starts the stack; a USB adapter may
   arrive later, and when it is the first one the stack starts with it. The adapter already in
   use registering again has a new address (another USB adapter in its place): the stack takes
   it, and asks DHCP again when it changed. */
void net_register(struct netif *n) {
    net_enter();
    bool arrived = false, start = false;
    if (!nif || nif == n) {
        bool changed = !nif || memcmp(link_mac, n->mac, 6) != 0;
        arrived = !nif && net_initialized;
        uint64_t flags = spin_lock(&config_lock);
        nif = n;
        memcpy(link_mac, n->mac, 6);
        spin_unlock(&config_lock, flags);
        link_driver = n->driver;
        if (changed && net_running) dhcp_start();
        start = net_start_locked();
    }
    net_leave();
    if (arrived) kprintf("net: %s adapter arrived\n", n->driver);
    if (start) kthread_create("net", net_thread, NULL);
}

static void tcp_buffer_configure(void) {
    extern const char *kernel_cmdline;
    const char *names[] = {"tcp-rx-bytes=","tcp-tx-bytes="};
    uint32_t *targets[] = {&tcp_rx_target,&tcp_tx_target};
    for (size_t option=0;option<ARRAY_SIZE(names);option++) {
        size_t prefix = strlen(names[option]);
        for (const char *p=kernel_cmdline;*p;p++) {
            if ((p!=kernel_cmdline && p[-1]!=' ') || strncmp(p,names[option],prefix)) continue;
            const char *q=p+prefix;
            uint64_t bytes=0;
            bool valid=*q>='0' && *q<='9';
            while (*q>='0' && *q<='9') {
                unsigned digit=*q++-'0';
                if (bytes>(UINT64_MAX-digit)/10) valid=false;
                if (valid) bytes=bytes*10+digit;
            }
            /* Physical allocation rounds to pages. RFC 7323's maximum window
               is also the sequence-space bound, not an invented fetch limit. */
            if (valid && (!*q || *q==' ') && bytes && bytes<=TCP_WINDOW_MAX) {
                *targets[option] = (uint32_t)ALIGN_UP(bytes,PAGE_SIZE);
            } else kprintf("net: invalid %s buffer configuration\n",names[option]);
        }
    }
    kprintf("net: TCP storage targets rx=%u tx=%u bytes (allocation-backed, WS negotiated)\n",tcp_rx_target,tcp_tx_target);
}

void net_init(void) {
    net_timer_init(&dhcp_timer, dhcp_timer_run);
    net_timer_init(&fault_timer, fault_timer_run);
    dns_configure();
    tcp_buffer_configure();
    if (!rtl8125_probe()) vm_net_probe();
    net_enter();
    net_initialized = true;
    bool none = !nif, start = net_start_locked();
    net_leave();
    if (none) kprintf("net: no supported network card yet\n");
    if (start) kthread_create("net", net_thread, NULL);
}

/* An app's sockets (its UDP sockets, its TCP connections and listeners, those of its name
   lookups) and its fault settings are its process's, found by its pid: they end when its handle
   table closes at its end, through this object, which the app holds with no number
   (sys/object.h). */
struct net_owner {
    int pid;
    bool held; /* the app's own: one a second thread made at the same moment owns nothing */
    struct object object;
};

static void owner_close(struct handle *h) {
    struct net_owner *o = h->object->priv;
    if (!o->held) return;
    net_enter();
    if (o->pid == fault_pid) fault_pid = 0;
    for(size_t i=0;i<udp_capacity;i++)
        if(udp_socks[i] && udp_socks[i]->used && udp_socks[i]->pid==o->pid)udp_release(udp_socks[i]);
    for(size_t i=0;i<tcp_capacity;i++)
        if(tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->user_open && tcbs[i]->pid==o->pid)tcp_close(tcbs[i]);
    net_leave();
}
static void owner_release(struct object *o) { kfree(o->priv); }
static const struct object_ops owner_ops = {.kind = N_KIND_CONNECTION, .close = owner_close, .release = owner_release};

/* Before a call that gives the calling app state of its own: its owner, made once. False without
   memory. */
static bool owner_hold(void) {
    if (handle_held(&owner_ops)) return true;
    struct net_owner *o = kzalloc(sizeof *o);
    if (!o) return false;
    o->pid = process_current()->pid;
    object_init(&o->object, &owner_ops, o);
    struct handle *h = handle_open(&o->object, 0);
    if (!h) {
        kfree(o);
        return false;
    }
    int r = handle_hold(h);
    if (r > 0) o->held = true; /* the table keeps it: this thread runs, so the app has not ended */
    return r >= 0;
}

/* The calls that give the app sockets of its own, or set its fault settings. */
static bool owner_needed(int num) {
    return num == SYS_UDP_SOCKET || num == SYS_TCP_CONNECT || num == SYS_TCP_LISTEN || num == SYS_NET_TEST ||
           num == SYS_NET_DNS || num == SYS_NET_DNS_ALL;
}

bool net_config(uint8_t mac[6], uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns) {
    uint64_t flags = spin_lock(&config_lock);
    bool present = nif != NULL, up = configured;
    memcpy(mac, link_mac, 6);
    *ip = my_ip;
    *mask = my_mask;
    *gw = my_gw;
    *dns = dns_default_resolver().ip;
    spin_unlock(&config_lock, flags);
    return present && up;
}

static int64_t net_syscall_inner(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    switch (num) {
    case SYS_NET_INFO: {
        struct n_netinfo ni = {0};
        if (nif) {
            ni.present = 1;
            ni.up = configured;
            memcpy(ni.mac, link_mac, 6);
            ip_put(ni.ip, my_ip);
            ip_put(ni.mask, my_mask);
            ip_put(ni.gateway, my_gw);
            ip_put(ni.dns, dns_default_resolver().ip);
            ni.rx_packets = rx_packets;
            ni.tx_packets = tx_packets;
            strlcpy(ni.driver, link_driver, sizeof ni.driver);
        }
        return copy_to_user((void *)a, &ni, sizeof ni);
    }
    case SYS_NET_PING:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        return sys_ping((uint32_t)a, (int)b,
                        clock_after((uint64_t)((int)c > 0 ? (int)c : PING_TIMEOUT_DEFAULT) * NS_PER_MS));
    case SYS_NET_DNS: {
        if (!user_ok_w((void *)b, 4)) return -N_ERR_BAD_ADDRESS;
        char name[DNS_NAME_TEXT_BYTES + 1];
        int r = user_str(name, (const char *)a, sizeof name);
        if (r < 0) return r;
        struct dns_answer answer;
        struct n_sockaddr resolver=dns_default_resolver();
        int64_t ret = dns_query(name, resolver.ip, resolver.port, &answer);
        if (ret == 0 && copy_to_user((void *)b, &answer.ips[0], sizeof answer.ips[0]) < 0) ret = -N_ERR_BAD_ADDRESS;
        kfree(answer.ips);
        return ret;
    }
    case SYS_NET_DNS_ALL: {
        struct n_dns_result *out=(struct n_dns_result *)d;
        if (!user_ok_w(out,sizeof *out) || c>SIZE_MAX/sizeof(uint32_t) || !user_ok_w((void *)b,c*sizeof(uint32_t))) return -N_ERR_BAD_ADDRESS;
        struct n_dns_result summary={.version=1,.status=N_DNS_FORMAT};
        if (copy_to_user(out,&summary,sizeof summary)<0) return -N_ERR_BAD_ADDRESS;
        char name[DNS_NAME_TEXT_BYTES + 1];
        int result=user_str(name,(const char *)a,sizeof name);
        if (result<0) return result;
        struct n_sockaddr resolver=dns_default_resolver();
        if (e && copy_from_user(&resolver,(const void *)e,sizeof resolver)<0) return -N_ERR_BAD_ADDRESS;
        struct dns_answer answer;
        result=dns_query(name,resolver.ip,resolver.port,&answer);
        summary=(struct n_dns_result){.version=1,.status=answer.status,.count=answer.count,.ttl=answer.ttl};
        size_t n=result?0:MIN(c,(size_t)answer.count);
        if (copy_to_user(out,&summary,sizeof summary)<0 ||
            (n && copy_to_user((void *)b,answer.ips,n*sizeof(uint32_t))<0)) result=-N_ERR_BAD_ADDRESS;
        kfree(answer.ips);return result;
    }
    case SYS_UDP_SOCKET:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        return udp_alloc((uint16_t)a, process_current()->pid);
    case SYS_UDP_SEND: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -N_ERR_NO_HANDLE;
        if (e > 1472) return -N_ERR_INVALID;
        uint8_t tmp[1472];
        if (copy_from_user(tmp, (const void *)d, e) < 0) return -N_ERR_BAD_ADDRESS;
        uint8_t mac[6];
        uint64_t generation=s->generation;
        int r = arp_resolve((uint32_t)b, mac);
        if (r < 0) return r;
        if(!s->used || s->generation!=generation)return -N_ERR_NO_HANDLE;
        r = udp_output(s->port, ip_source((uint32_t)b), (uint32_t)b, (uint16_t)c, mac, tmp, e);
        return r < 0 ? r : (int64_t)e;
    }
    case SYS_UDP_RECV: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -N_ERR_NO_HANDLE;
        if (!user_ok_w((void *)b, c)) return -N_ERR_BAD_ADDRESS;
        struct n_sockaddr *from = (struct n_sockaddr *)e;
        if (from && !user_ok_w(from, sizeof *from)) return -N_ERR_BAD_ADDRESS;
        int err = 0;
        struct udp_dgram *dg = udp_wait((int)a, syscall_deadline((int)d), &err);
        if (!dg) return err;
        /* The datagram is out of the queue: its slot stays as it is while net_lock is held. */
        size_t n = MIN(c, dg->len);
        struct n_sockaddr source = {.ip = dg->src, .port = dg->sport};
        if (copy_to_user((void *)b, dg->data, n) < 0 || (from && copy_to_user(from, &source, sizeof source) < 0))
            return -N_ERR_BAD_ADDRESS;
        return (int64_t)n;
    }
    case SYS_UDP_CLOSE: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -N_ERR_NO_HANDLE;
        udp_release(s);
        return 0;
    }
    case SYS_TCP_CONNECT:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        return tcp_connect_until((uint32_t)a, (uint16_t)b,
                                 clock_after((uint64_t)((int)c > 0 ? (int)c : TCP_CONNECT_DEFAULT) * NS_PER_MS));
    case SYS_TCP_SEND: return tcp_send_data((int)a, (const uint8_t *)b, c, CLOCK_NEVER, true, false);
    case SYS_TCP_SEND_UNTIL:
        /* d: the ms since boot it gives up at (on the clock's axis), 0 for none */
        if(e&~(uint64_t)N_TCP_SEND_NONBLOCK)return -N_ERR_INVALID;
        return tcp_send_data((int)a, (const uint8_t *)b, c,
                             !d || d > CLOCK_NEVER / NS_PER_MS ? CLOCK_NEVER : d * NS_PER_MS, true,
                             (e&N_TCP_SEND_NONBLOCK)!=0);
    case SYS_TCP_CANCEL: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -N_ERR_NO_HANDLE;
        tcp_reset(t, -N_ERR_CANCELLED);
        /* Aborted bytes are never exposed by a later receive and unsent
           prefixes are never emitted after cancellation. */
        t->rx_len=t->tx_len=0;t->nooo=0;
        return 0;
    }
    case SYS_NET_STATS: {
        struct n_netstats stats = {0};
        uint64_t flags = spin_lock(&rxq_lock);
        stats.rx_dropped = rxq_dropped;
        stats.rx_invalid = rxq_invalid;
        stats.rx_ring_high_water = rxq_high_water;
        spin_unlock(&rxq_lock, flags);
        if (nif && nif->get_stats) nif->get_stats(&stats);
        return copy_to_user((void *)a, &stats, sizeof stats);
    }
    case SYS_TCP_RECV: return tcp_recv_data((int)a, (uint8_t *)b, c, syscall_deadline((int)d), true);
    case SYS_NET_TEST: {
        /* a: fault settings for this process's TCP connections (or NULL); b: counters out (or NULL) */
        if (a) {
            struct n_netfault settings;
            if (copy_from_user(&settings, (const void *)a, sizeof settings) < 0) return -N_ERR_BAD_ADDRESS;
            fault = settings;
            fault_pid = fault.rx_drop || fault.rx_reorder || fault.tx_drop ? process_current()->pid : 0;
        }
        if (b) {
            struct n_tcpstats counters = tcp_stats;
            if (copy_to_user((void *)b, &counters, sizeof counters) < 0) return -N_ERR_BAD_ADDRESS;
        }
        return 0;
    }
    case SYS_TCP_CLOSE: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -N_ERR_NO_HANDLE;
        tcp_close(t);
        return 0;
    }
    case SYS_TCP_LISTEN:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        if (a > UINT32_MAX || b > UINT16_MAX) return -N_ERR_INVALID;
        return sys_tcp_listen((uint32_t)a, (uint16_t)b, (int)c);
    case SYS_TCP_ACCEPT: return sys_tcp_accept((int)a, syscall_deadline((int)b), (struct n_sockaddr *)c);
    case SYS_TCP_ADDRESS: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -N_ERR_NO_HANDLE;
        struct n_sockaddr local = {.ip = t->state == T_LISTEN ? t->lip : ip_source(t->rip), .port = t->lport};
        struct n_sockaddr remote = {.ip = t->rip, .port = t->rport};
        if ((b && copy_to_user((void *)b, &local, sizeof local) < 0) ||
            (c && copy_to_user((void *)c, &remote, sizeof remote) < 0))
            return -N_ERR_BAD_ADDRESS;
        return 0;
    }
    case SYS_TCP_FINISH: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -N_ERR_NO_HANDLE;
        if (t->fin_pending) return 0;
        if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT)
            return t->state == T_CLOSED && t->err ? t->err : -N_ERR_NOT_CONNECTED;
        t->fin_pending = true; /* tcp_push sends the FIN once the queued data is out */
        tcp_push(t, false);
        return 0;
    }
    }
    return -N_ERR_UNSUPPORTED;
}

/* The calling thread's cancel channel for setup waits: its own, so outside the stack's lock. */
static int64_t net_setup_cancel_channel(int index) {
    if (index < -1) return -N_ERR_INVALID;
    struct handle *h = NULL;
    if (index >= 0) {
        h = handle_get(index);
        if (!h) return -N_ERR_NO_HANDLE;
        if (!message_channel_is(h)) {
            handle_close(h);
            return -N_ERR_INVALID;
        }
    }
    struct thread *self = thread_current();
    struct handle *old = self->net_setup_cancel;
    self->net_setup_cancel = h;
    handle_close(old);
    return 0;
}

int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (num == SYS_NET_SETUP_CANCEL_CHANNEL) return net_setup_cancel_channel((int)a);
    if (owner_needed(num) && !owner_hold()) return -N_ERR_NO_MEMORY;
    net_enter();
    int64_t result = net_syscall_inner(num, a, b, c, d, e);
    net_leave();
    return result;
}
