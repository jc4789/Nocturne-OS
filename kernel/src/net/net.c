/* Nocturne network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and a small TCP client.
   Received frames are queued from the NIC interrupt and processed by the "net" kernel thread.
   System calls run in the calling task and block on their socket's progress queue
   (or net_wq for shared link/UDP state) until the thread reports progress.
   Protocol control, timers, allocation and NIC output are BSP-owned. APs may
   copy bounded TCP ring prefixes while the BSP gate is open. */
#include "kernel.h"
#include "abi.h"
#include "net.h"
#include "../dev/entropy.h"
#include "../arch/cpu.h"
#include "../mm/heap.h"
#include "../sys/sched.h"
#include "../sys/syscall.h"
#include "../mm/vmm.h"
#include "../fs/vfs.h"
#include "dns_name.h"

static struct netif *nif;
static uint32_t my_ip, my_mask, my_gw, my_dns;
static struct n_sockaddr dns_boot_resolver;
static int dns_boot_setting; /* 0=DHCP, 1=explicit, negative=rejected boot setting */
static struct n_sockaddr dns_default_resolver(void);
static bool configured;
static uint64_t rx_packets, tx_packets;
/* BSP-owned wire work, used only at cooperative batch boundaries. */
static uint64_t net_service_bytes;
static struct wait_queue net_wq; /* shared link/ARP/DNS/UDP progress */
static struct wait_queue rxq_wq; /* the net thread sleeps here */
static volatile bool net_work_pending; /* IRQ wake latch, including TX resource returns */
static bool receive_batch;
/* BSP closes the gate before touching socket metadata. AP sections are at
   most 64 KiB and never wait or allocate. The active count pins the registry,
   rings and generations until BSP close has joined all preceding readers. */
/* Keep the ownership gate, AP join count, producer mailbox and consumer wait
   arm on separate cache lines. Their independent BSP/AP writers must not
   invalidate the other controls merely by updating an adjacent word. The
   full-size objects also exclude unrelated globals from their cache lines. */
struct net_shared_word { unsigned value; } __attribute__((aligned(64)));
_Static_assert(sizeof(struct net_shared_word)==64,"network control cache line");
static struct net_shared_word net_gate_closed, net_ap_active, net_ap_pending;
static struct net_shared_word net_ap_sleeping; /* consumer has armed its BSP wait boundary */
static unsigned net_guard_depth; /* BSP-only nesting */
static struct n_netstats net_counters;
/* Temporary handoff diagnosis; disable after the controlled comparison. No
   per-packet clock reads or automatic logs. SYS_NET_STATS emits one snapshot. */
#define NET_HANDOFF_DIAG 0
#if NET_HANDOFF_DIAG
enum { ND_GATE,ND_GATE_RACE,ND_OWNER,ND_LEGACY_LARGE,ND_LEGACY_SPACE,ND_TX_FULL,ND_TX_MAP,ND_RX_WAIT,ND_RX_MAP,ND_REASONS };
static struct {
    uint64_t fallback[CPU_MAX_COUNT][ND_REASONS];
    uint64_t gate_joins,gate_ticks,gate_max;
    uint64_t handoffs,handoff_ticks,handoff_max,handoff_no_output;
    uint64_t yields,yield_resched,yield_pending,yield_idle,yield_level[4];
    uint64_t pushes[3],push_bytes[3]; /* caller, net thread, other kernel */
} net_diag;
static struct task *net_diag_thread;
static void net_diag_fallback(unsigned cpu,unsigned reason) {
    /* One writer per CPU; atomic load/store avoids adding a locked RMW to
       the path being measured. Snapshot readers use the same atomic width. */
    uint64_t *value=&net_diag.fallback[cpu][reason];
    __atomic_store_n(value,__atomic_load_n(value,__ATOMIC_RELAXED)+1,__ATOMIC_RELAXED);
}
#define ND_FALLBACK(cpu,reason) net_diag_fallback(cpu,reason)
#else
#define ND_FALLBACK(cpu,reason) ((void)0)
#endif
static void net_bsp_enter(void) {
    if (net_guard_depth++) return;
    __atomic_store_n(&net_gate_closed.value, 1, __ATOMIC_SEQ_CST);
#if NET_HANDOFF_DIAG
    uint64_t began=0;
    while (__atomic_load_n(&net_ap_active.value, __ATOMIC_SEQ_CST)) {
        if(!began)began=rdtsc();
        pause();
    }
    if(began) {
        uint64_t ticks=rdtsc()-began;
        net_diag.gate_joins++;net_diag.gate_ticks+=ticks;net_diag.gate_max=MAX(net_diag.gate_max,ticks);
    }
#else
    while (__atomic_load_n(&net_ap_active.value, __ATOMIC_SEQ_CST)) pause();
#endif
}
static void net_bsp_leave(void) {
    ASSERT(net_guard_depth);
    if (!--net_guard_depth) __atomic_store_n(&net_gate_closed.value, 0, __ATOMIC_RELEASE);
}
/* Wait queues and allocations remain BSP-only. Release the data-plane gate
   while sleeping so another CPU can use an unrelated ready connection. */
static bool net_wait_timeout(struct wait_queue *queue, uint64_t ms) {
    unsigned depth = net_guard_depth;
    while (net_guard_depth) net_bsp_leave();
    bool ready = wq_wait_timeout(queue, ms);
    for (unsigned i=0;i<depth;i++) net_bsp_enter();
    return ready;
}
/* The retained file identifies an endpoint, independently of descriptor reuse.
   Only setup paths consult it: a cancelled HTTP/2 stream must never abort a
   shared established TCP connection while pumping another stream's records. */
static bool net_setup_cancelled(void) {
    struct file *file=current_task->net_setup_cancel_file;
    if(!file)return false;
    uint64_t flags=irq_save();
    bool closed=file->vn->ops && file->vn->ops->hangup && file->vn->ops->hangup(file->vn,file);
    irq_restore(flags);
    return closed;
}
static bool net_setup_wait_timeout(struct wait_queue *queue,uint64_t ms) {
    /* Task wait queues remain BSP-owned. Bound only the setup context's
       sleep; normal TCP/UDP waits retain their existing progress semantics. */
    if(current_task->net_setup_cancel_file && ms>50)ms=50;
    return net_wait_timeout(queue,ms);
}
void net_task_exit(struct task *task) {
    struct file *file=task->net_setup_cancel_file;
    task->net_setup_cancel_file=NULL;
    if(file)vfs_close(file);
}

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

void net_delay_ms(uint64_t ms) {
    uint64_t until = uptime_ms() + ms + 1;
    while (uptime_ms() < until) pause();
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

/* ---------------------------------------------------------------- receive queue (IRQ -> thread) */

#define RXQ 64
static uint8_t rxq_buf[RXQ][1536];
static uint16_t rxq_len[RXQ];
static volatile int rxq_head, rxq_tail;

void net_rx_notify(void) {
    net_work_pending = true;
    wq_wake_all(&rxq_wq);
}

void net_rx(const void *frame, size_t len) {
    if (len < 14 || len > 1536) {net_counters.rx_invalid++;return;}
    int next = (rxq_head + 1) % RXQ;
    if (next == rxq_tail) {net_counters.rx_dropped++;return;}
    memcpy(rxq_buf[rxq_head], frame, len);
    rxq_len[rxq_head] = (uint16_t)len;
    rxq_head = next;
    uint64_t queued=(uint64_t)((rxq_head-rxq_tail+RXQ)%RXQ);
    net_counters.rx_ring_high_water=MAX(net_counters.rx_ring_high_water,queued);
    net_rx_notify();
}

/* ---------------------------------------------------------------- output */

static uint8_t frame[1600];
#define L4 (frame + 14 + 20) /* transport payload area */
static uint16_t ip_ident;

static int eth_output(const uint8_t dst[6], uint16_t type, size_t len) {
    if (!nif) return -N_ERR_NETWORK_DOWN;
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, nif->mac, 6);
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

static int ip_output(uint32_t dst, const uint8_t mac[6], uint8_t proto, size_t len) {
    struct ip_hdr *ip = (struct ip_hdr *)(frame + 14);
    ip->vihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(20 + len));
    ip->id = htons(ip_ident++);
    ip->frag = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip_put(ip->src, my_ip);
    ip_put(ip->dst, dst);
    ip->csum = htons(csum_fold(csum_add(0, ip, 20)));
    return eth_output(mac, 0x0800, 20 + len);
}

/* ---------------------------------------------------------------- ARP */

#define ARP_N 16
static struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t time;
    bool valid;
} arp_tab[ARP_N];

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_N; i++)
        if (arp_tab[i].valid && arp_tab[i].ip == ip && uptime_ms() - arp_tab[i].time < 600000) {
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
    arp_tab[slot].time = uptime_ms();
    arp_tab[slot].valid = true;
    wq_wake_all(&net_wq);
}

static void arp_send(uint16_t op, const uint8_t tha[6], uint32_t tpa, const uint8_t dst[6]) {
    struct arp_pkt *a = (struct arp_pkt *)(frame + 14);
    a->htype = htons(1);
    a->ptype = htons(0x0800);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, nif->mac, 6);
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

/* Resolve the link address (system call context: may block). A nonzero
   deadline shares TCP's connect budget; zero preserves ordinary ARP retries. */
static int arp_resolve_until(uint32_t dst, uint8_t mac[6], uint64_t deadline) {
    if (!nif) return -N_ERR_NETWORK_DOWN;
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    if (dst == 0xFFFFFFFF || dst == (my_ip | ~my_mask)) {
        memcpy(mac, bcast_mac, 6);
        return 0;
    }
    if (!configured) return -N_ERR_NETWORK_DOWN;
    uint32_t hop = next_hop(dst);
    if (!hop) return -N_ERR_UNREACHABLE;
    for (int tries = 0; tries < 3; tries++) {
        if (current_task->killed) return -N_ERR_STOPPED;
        if(net_setup_cancelled())return -N_ERR_CANCELLED;
        uint64_t now = uptime_ms();
        if (deadline && now >= deadline) return -N_ERR_TIMEOUT;
        if (arp_lookup(hop, mac)) return 0;
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", hop, bcast_mac);
        uint64_t until = now + 1000;
        if (deadline && deadline < until) until = deadline;
        for (;;) {
            if (current_task->killed) return -N_ERR_STOPPED;
            if(net_setup_cancelled())return -N_ERR_CANCELLED;
            now = uptime_ms();
            if (deadline && now >= deadline) return -N_ERR_TIMEOUT;
            if (arp_lookup(hop, mac)) return 0;
            if (now >= until) break;
            net_setup_wait_timeout(&net_wq, until - now);
        }
    }
    return -N_ERR_UNREACHABLE;
}
static int arp_resolve(uint32_t dst, uint8_t mac[6]) { return arp_resolve_until(dst, mac, 0); }

/* ---------------------------------------------------------------- ICMP */

#define PING_N 8
static struct ping_wait {
    bool used, done;
    uint16_t id, seq;
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
        ip_output(src, mac, 1, len);
    } else if (h->type == 0) {
        for (int i = 0; i < PING_N; i++)
            if (pings[i].used && pings[i].id == ntohs(h->id) && pings[i].seq == ntohs(h->seq)) pings[i].done = true;
        wq_wake_all(&net_wq);
    }
}

static int64_t sys_ping(uint32_t dst, int seq, int timeout_ms) {
    if (dst == my_ip || (dst >> 24) == 127) return 0;
    uint8_t mac[6];
    int r = arp_resolve(dst, mac);
    if (r < 0) return r;
    int slot = -1;
    for (int i = 0; i < PING_N; i++)
        if (!pings[i].used) slot = i;
    if (slot < 0) return -N_ERR_BUSY;
    struct ping_wait *w = &pings[slot];
    w->used = true;
    w->done = false;
    w->id = (uint16_t)(0x4E00 + task_process()->pid);
    w->seq = (uint16_t)seq;
    struct icmp_hdr *h = (struct icmp_hdr *)L4;
    h->type = 8;
    h->code = 0;
    h->id = htons(w->id);
    h->seq = htons(w->seq);
    h->csum = 0;
    memcpy(L4 + 8, "Nocturne says hello to the night.", 32);
    h->csum = htons(csum_fold(csum_add(0, L4, 40)));
    uint64_t t0 = uptime_ms(), until = t0 + (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000);
    ip_output(dst, mac, 1, 40);
    int64_t ret = -N_ERR_TIMEOUT;
    for (;;) {
        uint64_t now=uptime_ms();
        if(now>=until)break;
        if (w->done) {
            ret = (int64_t)(now - t0);
            break;
        }
        if (current_task->killed) {
            ret = -N_ERR_STOPPED;
            break;
        }
        net_wait_timeout(&net_wq, until - now);
    }
    if (w->done) ret = (int64_t)(uptime_ms() - t0);
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

static int udp_output(uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t mac[6], const void *data,
                      size_t len) {
    if (len > 1472) return -N_ERR_INVALID;
    struct udp_hdr *u = (struct udp_hdr *)L4;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(8 + len));
    u->csum = 0;
    memcpy(L4 + 8, data, len);
    uint16_t c = l4_csum(my_ip, dst, 17, L4, 8 + len);
    u->csum = htons(c ? c : 0xFFFF);
    return ip_output(dst, mac, 17, 8 + len);
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
        wq_wake_all(&net_wq);
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
    wq_wake_all(&net_wq);
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

/* wait for a datagram on socket i; returns pointer or NULL on timeout (and sets *err) */
static struct udp_dgram *udp_wait(int i, int timeout_ms, int *err) {
    struct udp_sock *s = udp_socks[i];
    uint64_t generation=s->generation;
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
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
        if (current_task->killed) {
            *err = -N_ERR_STOPPED;
            return NULL;
        }
        uint64_t now=uptime_ms();
        if (timeout_ms >= 0 && now >= until) {
            *err = -N_ERR_NOT_READY;
            return NULL;
        }
        if(s->internal)net_setup_wait_timeout(&net_wq, timeout_ms < 0 ? 1000 : until - now);
        else net_wait_timeout(&net_wq, timeout_ms < 0 ? 1000 : until - now);
    }
}

static struct udp_sock *user_udp(int h) {
    if(h<0 || (size_t)h>=udp_capacity)return NULL;
    struct udp_sock *s=udp_socks[h];
    return s && s->used && !s->internal && s->pid==task_process()->pid?s:NULL;
}

/* ---------------------------------------------------------------- DHCP */

enum { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };
static int dhcp_state;
static uint32_t dhcp_xid, dhcp_offer, dhcp_server;
static uint64_t dhcp_next;
static int dhcp_tries;

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
    memcpy(b + 28, nif->mac, 6);
    static const uint8_t magic[4] = {99, 130, 83, 99};
    memcpy(b + 236, magic, 4);
    int o = 240;
    b[o++] = 53;
    b[o++] = 1;
    b[o++] = (uint8_t)type;
    b[o++] = 61; /* client id */
    b[o++] = 7;
    b[o++] = 1;
    memcpy(b + o, nif->mac, 6);
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
    uint32_t saved = my_ip;
    my_ip = 0;
    udp_output(68, 0xFFFFFFFF, 67, bcast_mac, b, sizeof b);
    my_ip = saved;
}

static void dhcp_start(void) {
    dhcp_state = DHCP_DISCOVER;
    dhcp_xid = rnd();
    dhcp_tries = 0;
    dhcp_next = uptime_ms();
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
        dhcp_next = uptime_ms() + 2000;
    } else if (dhcp_state == DHCP_REQUEST && type == 5) {
        my_ip = yiaddr;
        my_mask = mask;
        my_gw = router;
        my_dns = dns ? dns : router;
        configured = true;
        dhcp_state = DHCP_BOUND;
        if (lease < 60) lease = 60;
        dhcp_next = uptime_ms() + (uint64_t)lease * 500; /* renew at half the lease */
        struct n_sockaddr resolver=dns_default_resolver();
        kprintf("net: %u.%u.%u.%u/%d gateway %u.%u.%u.%u dns %u.%u.%u.%u:%u (DHCP)\n", my_ip >> 24, (my_ip >> 16) & 255,
                (my_ip >> 8) & 255, my_ip & 255, __builtin_popcount(my_mask), my_gw >> 24, (my_gw >> 16) & 255,
                (my_gw >> 8) & 255, my_gw & 255, resolver.ip >> 24, (resolver.ip >> 16) & 255, (resolver.ip >> 8) & 255,
                resolver.ip & 255, resolver.port);
        wq_wake_all(&net_wq);
    } else if (type == 6) {
        dhcp_start();
    }
}

static void dhcp_tick(void) {
    if (dhcp_state == DHCP_IDLE || uptime_ms() < dhcp_next) return;
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
    dhcp_next = uptime_ms() + (dhcp_tries < 4 ? 2000 : dhcp_tries < 10 ? 5000 : 30000);
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
struct dns_cache_entry {
    struct dns_cache_entry *next;
    uint64_t expires, used;
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
    entry->count=answer->count; entry->bytes=bytes; entry->used=uptime_ms();
    entry->resolver=resolver;entry->port=port;
    uint64_t duration=(uint64_t)answer->ttl*1000;
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
    uint64_t now=uptime_ms();
    for (struct dns_cache_entry **at=&dns_cache;*at;) {
        struct dns_cache_entry *entry=*at;
        if (entry->expires<=now) { *at=entry->next;dns_cache_bytes-=entry->bytes;kfree(entry);continue; }
        if (entry->resolver==resolver && entry->port==port && !strcmp(entry->name,name)) {
            answer->ips=kmalloc((size_t)entry->count*4);if (!answer->ips) return dns_failure(answer,N_DNS_RESOURCE);
            memcpy(answer->ips,entry->ips,(size_t)entry->count*4);answer->count=entry->count;
            answer->ttl=(uint32_t)((entry->expires-now+999)/1000);answer->status=N_DNS_OK;entry->used=now;return 0;
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
    int sock=udp_alloc(0,task_process()->pid);if (sock<0) {dns_failure(answer,sock==-N_ERR_NO_MEMORY?N_DNS_RESOURCE:N_DNS_UNREACHABLE);return sock;}
    udp_socks[sock]->internal=true;uint64_t generation=udp_socks[sock]->generation;
    result=dns_failure(answer,N_DNS_TIMEOUT);
    for (unsigned attempt=0;attempt<3 && result==-N_ERR_TIMEOUT;attempt++) {
        if(net_setup_cancelled()){result=-N_ERR_CANCELLED;dns_failure(answer,N_DNS_CANCELLED);break;}
        int sent=udp_output(udp_socks[sock]->port,resolver,port,mac,query,query_len);
        if (sent<0) {result=sent;dns_failure(answer,N_DNS_UNREACHABLE);break;}
        uint64_t until=uptime_ms()+2000;
        while (uptime_ms()<until) {
            int err=0;uint64_t tick=uptime_ms();if(tick>=until)break;uint64_t remain=until-tick;
            struct udp_dgram *packet=udp_wait(sock,(int)remain,&err);
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
static uint32_t tcp_rx_target = TCP_BUFFER_DEFAULT, tcp_tx_target = TCP_BUFFER_DEFAULT;
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { T_FREE, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK,
       T_TIME_WAIT, T_CLOSED };

#define TCP_OOO   8    /* out-of-order ranges remembered per connection */
#define RTO_MIN   200  /* ms */
#define RTO_MAX   16000

struct tcb {
    int state, err, pid;
    unsigned io_owner; /* 0=idle, CPU index+1 owns a bounded AP ring operation */
    /* Stable metadata owns its progress waiters. One socket's ACK/data must
       not reschedule every DNS/TCP helper that is waiting on another socket. */
    struct wait_queue progress;
    bool user_open, fin_pending, fin_sent, rx_fin;
    bool window_scaled, ack_pending, progress_pending, tx_retry, probe_pending, resend_pending;
    bool ap_output_pending; /* AP copy/window update awaiting BSP protocol output */
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
    /* RTT estimate (RFC 6298, ms); one segment is timed at a time, never a retransmitted one */
    int srtt, rttvar;
    uint32_t rtt_seq;
    uint64_t rtt_at;
    /* receive side. rx is a ring: rx_len bytes are in order and readable; segments that arrive
       ahead of rcv_nxt are stored at their place behind them, and ooo[] remembers which ranges */
    uint32_t rcv_nxt, adv_wnd;
    struct tcp_range { uint32_t start, end; } *ooo;
    size_t nooo, ooo_capacity;
    uint8_t *rx, *tx;
    uint32_t rx_head, rx_len, tx_head, tx_len, rx_capacity, tx_capacity;
    uint64_t rto_at, linger_until;
    uint64_t generation;
    int rto, retries;
#if NET_HANDOFF_DIAG
    uint64_t ap_enqueue_at;
#endif
};
/* Closed-but-lingering tuples remain valid until TCP releases them. They
   must not occupy a fixed sixteen-slot pool and serialize unrelated fetches. */
static struct tcb *tcp_initial_slots[TCP_INITIAL];
static struct tcb **tcbs=tcp_initial_slots;
static size_t tcp_capacity=TCP_INITIAL;
static uint64_t tcp_generation;

static struct n_tcpstats tcp_stats;

/* fault injection for tests (SYS_NET_TEST): applies only to fault_pid's connections */
static struct n_netfault fault;
static int fault_pid;
static uint8_t fault_held[1600];
static size_t fault_held_len;
static uint32_t fault_held_src;
static uint64_t fault_held_at;

static bool fault_hit(uint16_t per_mille) { return per_mille && rnd() % 1000 < per_mille; }

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)

static void tcp_free(struct tcb *t) {
    /* Detach sleeping tasks before clearing the stable slot; their generation
       checks reject a closed/reused connection when they next run. */
    wq_wake_all(&t->progress);
    kfree(t->rx);
    kfree(t->tx);
    kfree(t->ooo);
    memset(t, 0, sizeof *t);
}

static void tcp_progress(struct tcb *t) {
    if (receive_batch) t->progress_pending = true;
    else wq_wake_all(&t->progress);
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
    size_t hl = (flags & F_SYN) ? 28 : 20; /* MSS, NOP and 3-byte WS, word aligned */
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
        L4[24] = 1; /* NOP aligns the following option */
        L4[25] = 3; /* RFC 7323 Window Scale */
        L4[26] = 3;
        L4[27] = t->rx_shift;
    }
    if (len) tcp_ring_read(L4+hl,t->tx,t->tx_capacity,t->tx_head+(seq-t->snd_una),(uint32_t)len);
    h->csum = htons(l4_csum(my_ip, t->rip, 6, L4, hl + len));
    int result = ip_output(t->rip, t->mac, 6, hl+len);
    if (result == -N_ERR_NOT_READY || result == -N_ERR_BUSY) {
        t->tx_retry = true;
        if (flags&F_ACK) t->ack_pending = true;
        return result; /* no sequence advance, retransmit timer or cwnd loss */
    }
    if (result < 0) {
        t->state = T_CLOSED;
        t->err = result;
        t->rto_at = 0;
        tcp_progress(t);
        return result;
    }
    tcp_stats.segs_out++;
    t->adv_wnd = (uint32_t)advertised << shift;
    if (flags&F_ACK) { t->ack_pending = false; t->ack_segments = 0; }
    return 0;
}

static void tcp_arm(struct tcb *t) {
    if (!t->rto_at) t->rto_at = uptime_ms() + (uint64_t)t->rto;
}

static void tcp_sent(struct tcb *t, uint32_t n) {
    if (!t->rtt_at && t->snd_nxt == t->snd_max) { /* time new data only (Karn) */
        t->rtt_at = uptime_ms();
        t->rtt_seq = t->snd_nxt + n;
    }
    t->snd_nxt += n;
    if (SEQ_LT(t->snd_max, t->snd_nxt)) t->snd_max = t->snd_nxt;
    tcp_arm(t);
}

/* Optional cooperative prefix budget, shared across the pre-RX output
   handoff. Charge attempts, including local backpressure, to bound work. */
struct tcp_output_budget { unsigned frames; size_t bytes; };
static bool tcp_take_output_budget(struct tcp_output_budget *budget,uint8_t flags,size_t payload) {
    if(!budget)return true;
    size_t bytes=MAX((size_t)60,14+20+((flags&F_SYN)?28:20)+payload);
    if(!budget->frames || bytes>budget->bytes)return false;
    budget->frames--;budget->bytes-=bytes;
    return true;
}

/* send whatever data (and FIN) the send and congestion windows allow, from snd_nxt on;
   force sends one byte into a zero window (a window probe) */
static void tcp_push_with_budget(struct tcb *t, bool force,struct tcp_output_budget *budget) {
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT && t->state != T_FIN_WAIT1 &&
        t->state != T_LAST_ACK && t->state != T_CLOSING)
        return;
#if NET_HANDOFF_DIAG
    bool handoff=t->ap_enqueue_at!=0;
    if(handoff) {
        uint64_t ticks=rdtsc()-t->ap_enqueue_at;t->ap_enqueue_at=0;
        net_diag.handoffs++;net_diag.handoff_ticks+=ticks;net_diag.handoff_max=MAX(net_diag.handoff_max,ticks);
    }
    unsigned owner=current_task==net_diag_thread?1:current_task->is_user?0:2;
    uint32_t submitted=0;
#endif
    uint32_t wnd = MIN(t->snd_wnd, t->cwnd);
    if (force && wnd == 0) wnd = 1;
    t->tx_retry = false;
    t->probe_pending = force;
    /* BSP ownership keeps tx_len, snd_una and wnd stable during this call.
       Every successful data output advances snd_nxt by a nonzero MSS-bounded
       prefix; storage/window exhaustion or NIC backpressure ends the loop.
       There are no waits here, and at most one FIN follows the finite data. */
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una; /* = bytes in flight, and the offset into tx */
        if (off < t->tx_len) {
            if (off >= wnd) break;
            uint32_t n = MIN(t->tx_len - off, MIN((uint32_t)t->mss, wnd - off));
            if(!tcp_take_output_budget(budget,F_ACK|F_PSH,n)) {t->tx_retry=true;break;}
            if (tcp_output(t, F_ACK | F_PSH, t->snd_nxt, n)<0) break;
            tcp_sent(t, n);
#if NET_HANDOFF_DIAG
            submitted+=n;
#endif
            continue;
        }
        if (t->fin_pending && off == t->tx_len && (!t->fin_sent || t->snd_nxt == t->fin_seq)) {
            if(!tcp_take_output_budget(budget,F_FIN|F_ACK,0)) {t->tx_retry=true;break;}
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
#if NET_HANDOFF_DIAG
    net_diag.pushes[owner]++;net_diag.push_bytes[owner]+=submitted;
    if(handoff && !submitted)net_diag.handoff_no_output++;
#endif
}
static void tcp_push(struct tcb *t,bool force) {tcp_push_with_budget(t,force,NULL);}

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

static void tcp_rtt_sample(struct tcb *t, int r) {
    if (r < 1) r = 1;
    if (!t->srtt) {
        t->srtt = r;
        t->rttvar = r / 2;
    } else {
        int d = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3 * t->rttvar + d) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
    }
    t->rto = MIN(MAX(t->srtt + MAX(4 * t->rttvar, 50), RTO_MIN), RTO_MAX);
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

static void tcp_reset(struct tcb *t, int err) {
    if (t->state != T_SYN_SENT && t->state != T_CLOSED && t->state != T_FREE)
        tcp_output(t, F_RST | F_ACK, t->snd_nxt, 0);
    t->state = T_CLOSED;
    t->err = err;
    t->rto_at = 0;
    tcp_progress(t);
}

static void tcp_segment(struct tcb *t, const uint8_t *p, size_t len);
static void tcp_input_held(size_t n);

static void tcp_input(uint32_t src, const uint8_t *p, size_t len) {
    if (len < 20 || l4_csum(src, my_ip, 6, p, len) != 0) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)p;
    size_t hl = (size_t)(h->off >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    struct tcb *t = NULL;
    for(size_t i=0;i<tcp_capacity;i++)
        if(tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->rip==src && tcbs[i]->rport==sport && tcbs[i]->lport==dport)
            t=tcbs[i];
    if (!t || t->state == T_CLOSED) return;
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
            fault_held_at = uptime_ms();
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

    if (fl & F_RST) {
        if (t->state == T_SYN_SENT) {
            if ((fl & F_ACK) && ack == t->iss + 1) {
                t->state = T_CLOSED;
                t->err = -N_ERR_CONNECTION_REFUSED;
            }
        } else if (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + MAX(t->adv_wnd, 1u))) {
            t->state = T_CLOSED;
            t->err = -N_ERR_CONNECTION_RESET;
        }
        t->rto_at = 0;
        tcp_progress(t);
        return;
    }

    if (t->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != t->iss + 1) return;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = ntohs(h->win); /* SYN-ACK is NEVER shifted */
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
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
        t->state = T_ESTABLISHED;
        t->rto_at = 0;
        t->retries = 0;
        t->rto = 1000;
        t->snd_max = t->snd_nxt;
        tcp_cc_init(t);
        tcp_output(t, F_ACK, t->snd_nxt, 0);
        tcp_progress(t);
        tcp_push(t, false);
        return;
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
                tcp_rtt_sample(t, (int)(uptime_ms() - t->rtt_at));
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
                else if (t->state == T_CLOSING) {
                    t->state = T_TIME_WAIT;
                    t->linger_until = uptime_ms() + 2000;
                } else if (t->state == T_LAST_ACK) t->state = T_CLOSED;
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
        else if (t->state == T_FIN_WAIT2) {
            t->state = T_TIME_WAIT;
            t->linger_until = uptime_ms() + 2000;
        }
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

static void tcp_tick(void) {
    uint64_t now = uptime_ms();
    for(size_t i=0;i<tcp_capacity;i++) {
        struct tcb *t=tcbs[i];
        if(!t || t->state==T_FREE)continue;
        if (t->state == T_TIME_WAIT && now >= t->linger_until) t->state = T_CLOSED;
        if (!t->user_open && (t->state == T_CLOSED || now >= t->linger_until)) {
            if (t->state != T_CLOSED) tcp_reset(t, -N_ERR_CONNECTION_RESET);
            tcp_free(t);
            continue;
        }
        if (t->rto_at && now >= t->rto_at) {
            t->rto_at = 0;
            bool probe = t->state != T_SYN_SENT && t->snd_wnd == 0; /* persist: the peer is just not reading */
            if (!probe && ++t->retries > (t->state == T_SYN_SENT ? 5 : 8)) {
                tcp_reset(t, -N_ERR_TIMEOUT);
                continue;
            }
            t->rto = MIN(t->rto * 2, RTO_MAX);
            if (t->state == T_SYN_SENT) {
                if (!tcp_output(t, F_SYN, t->iss, 0)) {
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
    /* a held-back (reordered) segment whose successor never came */
    if (fault_held_len && now - fault_held_at > 200) {
        size_t n = fault_held_len;
        fault_held_len = 0;
        tcp_input_held(n);
    }
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
    return t && t->state!=T_FREE && t->user_open && t->pid==task_process()->pid?t:NULL;
}

/* No stack growth or demand allocation on an AP. VMM revocation already
   retires that address space's AP runner before changing its page tables. */
static bool ap_user_mapped(const void *pointer, size_t length, bool write) {
    uint64_t address = (uint64_t)pointer;
    if (address >= USER_TOP || length > USER_TOP-address) return false;
    for (uint64_t at=ALIGN_DOWN(address,PAGE_SIZE); at<address+length; at+=PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(current_task->pml4, at);
        if ((pte & (PTE_P|PTE_U)) != (PTE_P|PTE_U) || (write && !(pte&PTE_W))) return false;
    }
    return true;
}

/* Each AP owns only its own CPU-indexed counters. The DPL3 interrupt gate
   enters with IRQs disabled; this bounded path neither enables them nor
   waits/yields, and runner retirement/preemption happens after it returns.
   No BSP/NMI path writes these slots. Atomic snapshot readers may race with
   this single writer, so aligned relaxed loads/stores suffice without a
   locked read-modify-write on every syscall, including early fallbacks. */
static void net_ap_counter_add(uint64_t *counter,uint64_t amount) {
    __atomic_store_n(counter,__atomic_load_n(counter,__ATOMIC_RELAXED)+amount,__ATOMIC_RELAXED);
}

bool net_ap_syscall(struct regs *regs) {
    unsigned cpu = cpu_current_index();
    bool send = regs->rax == SYS_TCP_SEND || regs->rax == SYS_TCP_SEND_UNTIL;
    if (!cpu || (!send && regs->rax != SYS_TCP_RECV)) return false;
    if (current_task->killed) return false; /* BSP performs process exit */
    net_ap_counter_add(&net_counters.ap_calls[cpu],1);
    if (__atomic_load_n(&net_gate_closed.value, __ATOMIC_SEQ_CST)) {ND_FALLBACK(cpu,ND_GATE);goto fallback;}
    __atomic_fetch_add(&net_ap_active.value, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&net_gate_closed.value, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&net_ap_active.value, 1, __ATOMIC_SEQ_CST);
        ND_FALLBACK(cpu,ND_GATE_RACE);
        goto fallback;
    }
    struct tcb *t = user_tcb((int)regs->rdi);
    int64_t result = -N_ERR_NO_HANDLE;
    bool handled = true, notify = false;
    if (!t) goto release_gate;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&t->io_owner, &expected, cpu+1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        ND_FALLBACK(cpu,ND_OWNER);
        handled = false; goto release_gate;
    }
    size_t wanted = regs->rdx;
    if (send && regs->rax==SYS_TCP_SEND_UNTIL && (regs->r8&~(uint64_t)N_TCP_SEND_NONBLOCK)) {
        result=-N_ERR_INVALID;goto release_socket;
    }
    if (!wanted) { result=0; goto release_socket; }
    if (send) {
        bool nonblock=regs->rax==SYS_TCP_SEND_UNTIL && (regs->r8&N_TCP_SEND_NONBLOCK);
        if (t->state!=T_ESTABLISHED && t->state!=T_CLOSE_WAIT) { result=t->err?t->err:-N_ERR_CLOSED; goto release_socket; }
        if (t->fin_pending) { result=-N_ERR_CLOSED; goto release_socket; }
        if (regs->rax==SYS_TCP_SEND_UNTIL && regs->r10 && uptime_ms()>=regs->r10) {
            result=-N_ERR_TIMEOUT; goto release_socket;
        }
        /* Preserve the legacy send syscall's full enqueue on success. */
        if (regs->rax==SYS_TCP_SEND && (wanted>65536 || wanted>t->tx_capacity-t->tx_len)) {
            ND_FALLBACK(cpu,wanted>65536?ND_LEGACY_LARGE:ND_LEGACY_SPACE);handled=false;goto release_socket;
        }
        uint32_t n = (uint32_t)MIN(MIN(wanted, (size_t)65536), t->tx_capacity-t->tx_len);
        if (!n) { if(nonblock)result=-N_ERR_NOT_READY;else {ND_FALLBACK(cpu,ND_TX_FULL);handled=false;} goto release_socket; }
        if (!ap_user_mapped((void *)regs->rsi,n,false)) { ND_FALLBACK(cpu,ND_TX_MAP);handled=false; goto release_socket; }
        tcp_ring_write(t->tx,t->tx_capacity,t->tx_head+t->tx_len,(void *)regs->rsi,n);
        t->tx_len+=n;
#if NET_HANDOFF_DIAG
        if(!t->ap_enqueue_at)t->ap_enqueue_at=rdtsc();
#endif
        t->tx_retry=true; /* BSP owns segment construction, timers and NIC */
        t->ap_output_pending=true;
        net_ap_counter_add(&net_counters.ap_send_bytes[cpu],n);
        result=n; notify=true;
    } else {
        if (!t->rx_len) {
            if (t->rx_fin) result=0;
            else if (t->state==T_CLOSED) result=t->err?t->err:0;
            else if ((int)regs->r10==0) result=-N_ERR_NOT_READY;
            else {ND_FALLBACK(cpu,ND_RX_WAIT);handled=false;}
            goto release_socket;
        }
        uint32_t n = (uint32_t)MIN(MIN(wanted,(size_t)65536),t->rx_len);
        if (!ap_user_mapped((void *)regs->rsi,n,true)) { ND_FALLBACK(cpu,ND_RX_MAP);handled=false; goto release_socket; }
        tcp_ring_read((void *)regs->rsi,t->rx,t->rx_capacity,t->rx_head,n);
        t->rx_head=(t->rx_head+n)%t->rx_capacity;
        t->rx_len-=n;
        uint32_t available=tcp_window(t), threshold=MIN(t->rx_capacity/2,TCP_MSS);
        if (t->state!=T_CLOSED && available>t->adv_wnd &&
            (!t->adv_wnd || available-t->adv_wnd>=threshold)) {
            t->ack_pending=true;
            t->ap_output_pending=true;
            notify=true;
        }
        net_ap_counter_add(&net_counters.ap_recv_bytes[cpu],n);
        result=n;
    }
release_socket:
    __atomic_store_n(&t->io_owner,0,__ATOMIC_RELEASE);
release_gate:
    __atomic_fetch_sub(&net_ap_active.value,1,__ATOMIC_SEQ_CST);
    if (!handled) goto fallback;
    regs->rax=(uint64_t)result;
    if (notify && !__atomic_fetch_or(&net_ap_pending.value,1u<<cpu,__ATOMIC_SEQ_CST) &&
        __atomic_load_n(&net_ap_sleeping.value,__ATOMIC_SEQ_CST)) cpu_bsp_wake();
    return true;
fallback:
    net_ap_counter_add(&net_counters.ap_fallbacks[cpu],1);
    return false;
}

void net_ap_notify_bsp(void) {
    if (__atomic_load_n(&net_ap_pending.value,__ATOMIC_ACQUIRE)) net_rx_notify();
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
        memset(t,0,sizeof *t);tcbs[slot]=t;
    }
    return (int)slot;
}
static uint16_t tcp_local_port(uint32_t ip,uint16_t remote) {
    for(unsigned attempt=0;attempt<16384;attempt++) {
        uint16_t port=ephemeral_port();bool occupied=false;
        for(size_t i=0;i<tcp_capacity;i++)
            if(tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->rip==ip &&
               tcbs[i]->rport==remote && tcbs[i]->lport==port){occupied=true;break;}
        if(!occupied)return port;
    }
    return 0;
}

static int64_t sys_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 15000);
    uint8_t mac[6];
    int r = arp_resolve_until(ip, mac, until);
    if (r < 0) return r;
    if(current_task->killed)return -N_ERR_STOPPED;
    if(net_setup_cancelled())return -N_ERR_CANCELLED;
    if(uptime_ms()>=until)return -N_ERR_TIMEOUT;
    uint16_t local=tcp_local_port(ip,port);
    if(!local)return -N_ERR_ADDRESS_IN_USE;
    int h=tcp_alloc_slot();if(h<0)return h;
    struct tcb *t=tcbs[h];
    memset(t, 0, sizeof *t);
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
    t->pid = task_process()->pid;
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
    t->rto = 1000;
    t->state = T_SYN_SENT;
    if (!tcp_output(t, F_SYN, t->iss, 0)) {
        t->snd_nxt = t->snd_max = t->iss+1;
        tcp_arm(t);
    }
    uint64_t generation=t->generation;
    while (t->state == T_SYN_SENT) {
        if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
        uint64_t now = uptime_ms();
        bool cancelled=net_setup_cancelled();
        if (current_task->killed || cancelled || now >= until) {
            t->state = T_CLOSED;
            t->err = current_task->killed ? -N_ERR_STOPPED : cancelled ? -N_ERR_CANCELLED : -N_ERR_TIMEOUT;
            break;
        }
        net_setup_wait_timeout(&t->progress, until - now);
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
        uint64_t now = uptime_ms();
        if (current_task->killed) return done ? (int64_t)done : -N_ERR_STOPPED;
        if (deadline && now >= deadline) return done ? (int64_t)done : -N_ERR_TIMEOUT;
        uint32_t space = t->tx_capacity - t->tx_len;
        if (!space) {
            if(nonblock)return done?(int64_t)done:-N_ERR_NOT_READY;
            if(!user)net_setup_wait_timeout(&t->progress, deadline ? deadline-now : 1000);
            else net_wait_timeout(&t->progress, deadline ? deadline-now : 1000);
            continue;
        }
        uint32_t n = (uint32_t)MIN(len - done, space);
        if(nonblock)n=MIN(n,65536u); /* bounded immediate ring enqueue */
        /* Validate exactly the chunk we will enqueue, not the entire request
           on every iteration. A late fault preserves the completed prefix. */
        if ((uint64_t)buf>UINT64_MAX-done || (user && !user_ok((const void *)((uint64_t)buf+done),n)))
            return done?(int64_t)done:-N_ERR_BAD_ADDRESS;
        tcp_ring_write(t->tx,t->tx_capacity,t->tx_head+t->tx_len,buf+done,n);
        t->tx_len += n;
        done += n;
        tcp_push(t, false);
        if(nonblock)return (int64_t)done;
    }
    return (int64_t)done;
}

static int64_t tcp_recv_data(int h, uint8_t *buf, size_t len, int timeout_ms, bool user) {
    struct tcb *t = user_tcb(h);
    if (!t) return -N_ERR_NO_HANDLE;
    if (!len) return 0;
    uint64_t generation=t->generation;
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    if(!user && net_setup_cancelled())return -N_ERR_CANCELLED;
    while (t->rx_len == 0) {
        if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
        if(!user && net_setup_cancelled())return -N_ERR_CANCELLED;
        if (t->rx_fin) return 0;
        if (t->state == T_CLOSED) return t->err ? t->err : 0;
        if (current_task->killed) return -N_ERR_STOPPED;
        uint64_t now=uptime_ms();
        if (timeout_ms >= 0 && now >= until) return -N_ERR_NOT_READY;
        if(!user)net_setup_wait_timeout(&t->progress, timeout_ms < 0 ? 1000 : until - now);
        else net_wait_timeout(&t->progress, timeout_ms < 0 ? 1000 : until - now);
    }
    if(t->generation!=generation || !t->user_open)return -N_ERR_NO_HANDLE;
    uint32_t n = (uint32_t)MIN(len, t->rx_len);
    if (user && !user_ok_w(buf,n)) return -N_ERR_BAD_ADDRESS;
    tcp_ring_read(buf,t->rx,t->rx_capacity,t->rx_head,n);
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
    wq_wake_all(&t->progress);
    t->linger_until = uptime_ms() + 20000;
    if (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) {
        t->fin_pending = true;
        tcp_push(t, false);
    } else if (t->state == T_SYN_SENT || t->state == T_CLOSED) {
        tcp_free(t);
    }
}

/* RFC 1035 TCP framing: two-byte length followed by exactly that many bytes.
   Both prefix and payload share one budget; short reads are ordinary TCP. */
static int dns_tcp_exchange(uint32_t resolver,uint16_t port,uint64_t deadline,
                            const uint8_t *query,size_t query_len,uint8_t **reply,size_t *reply_len) {
    *reply=NULL;*reply_len=0;
    uint64_t now=uptime_ms();if(now>=deadline)return -N_ERR_TIMEOUT;
    int handle=(int)sys_tcp_connect(resolver,port,(int)(deadline-now));
    if (handle<0) return handle;
    uint8_t prefix[2]={(uint8_t)(query_len>>8),(uint8_t)query_len};
    int result=0;
    long n=tcp_send_data(handle,prefix,2,deadline,false,false);
    if (n!=2) {result=n<0?(int)n:net_setup_cancelled()?-N_ERR_CANCELLED:-N_ERR_TIMEOUT;goto close;}
    n=tcp_send_data(handle,query,query_len,deadline,false,false);
    if (n!=(long)query_len) {result=n<0?(int)n:net_setup_cancelled()?-N_ERR_CANCELLED:-N_ERR_TIMEOUT;goto close;}
    size_t done=0;
    while (done<2) {
        uint64_t now=uptime_ms();if (now>=deadline) {result=-N_ERR_TIMEOUT;goto close;}
        n=tcp_recv_data(handle,prefix+done,2-done,(int)(deadline-now),false);
        if (n<=0) {result=n<0?(int)n:-N_ERR_PROTOCOL;goto close;} done+=(size_t)n;
    }
    size_t length=(size_t)prefix[0]<<8|prefix[1];
    if (length<12) {result=-N_ERR_PROTOCOL;goto close;}
    uint8_t *response=kmalloc(length);if (!response) {result=-N_ERR_NO_MEMORY;goto close;}
    done=0;
    while (done<length) {
        uint64_t now=uptime_ms();if (now>=deadline) {result=-N_ERR_TIMEOUT;break;}
        n=tcp_recv_data(handle,response+done,length-done,(int)(deadline-now),false);
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
    if (memcmp(e->dst, nif->mac, 6) != 0 && !(e->dst[0] & 1)) return; /* not for us (promiscuous NICs) */
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
        if (configured && dst != my_ip && dst != 0xFFFFFFFF && dst != (my_ip | ~my_mask)) return;
        if (ip->proto == 1) icmp_input(e->src, src, ipp + hl, tl - hl);
        else if (ip->proto == 17) udp_input(src, dst, ipp + hl, tl - hl);
        else if (ip->proto == 6 && configured) tcp_input(src, ipp + hl, tl - hl);
    }
}

/* BSP-owned protocol output. APs only mark pending state while holding the
   data-plane gate; they never call the NIC or mutate protocol timers. */
static void tcp_flush_pending(bool ap_only) {
    struct tcp_output_budget prefix={64,128u*1024u};
    struct tcp_output_budget *budget=ap_only?&prefix:NULL;
    for (size_t i=0;i<tcp_capacity;i++) {
        struct tcb *t = tcbs[i];
        if (!t || t->state==T_FREE || t->state==T_CLOSED) continue;
        if(ap_only && !t->ap_output_pending)continue;
        /* A prefix yield leaves all remaining state for the ordinary flush
           after RX; the optional early pass never drains every stream's
           whole storage/window before making receive progress. */
        if(budget && !budget->frames)break;
        t->ap_output_pending=false;
        if (t->state==T_SYN_SENT && t->tx_retry) {
            if(!tcp_take_output_budget(budget,F_SYN,0))break;
            if (!tcp_output(t,F_SYN,t->iss,0)) {
                t->snd_nxt = t->snd_max = t->iss+1;
                t->tx_retry = false;
                tcp_arm(t);
            }
        } else {
            if (t->ack_pending) {
                if(!tcp_take_output_budget(budget,F_ACK,0))break;
                tcp_output(t,F_ACK,t->snd_nxt,0);
            }
            if (t->resend_pending) {
                size_t n=MIN(t->tx_len,(uint32_t)t->mss);
                if(!tcp_take_output_budget(budget,F_ACK,n))break;
                tcp_resend_first(t);
            }
            if (t->tx_retry && !t->resend_pending) tcp_push_with_budget(t,t->probe_pending,budget);
        }
    }
}

static void net_thread(void *arg) {
    (void)arg;
    uint64_t last_tick = 0;
    /* Several short NIC batches may share one bounded service window. A
       no-work wait is already a scheduler boundary; do not first requeue the
       net task only to run it again and block immediately afterwards. */
    uint64_t service_started=0,service_start_bytes=0;
    bool serviced=false;
    for (;;) {
        uint64_t f = irq_save();
        /* SC arm -> pending recheck pairs with AP publish -> armed recheck:
           both sides cannot observe zero. IRQs stay off from this condition
           through wq enrollment, so an AP doorbell delivered after the check
           wakes an enrolled consumer. A runnable consumer needs no new IPI. */
        __atomic_store_n(&net_ap_sleeping.value,1,__ATOMIC_SEQ_CST);
        bool rx_ready=rxq_tail!=rxq_head || (nif->rx_pending && nif->rx_pending());
        bool ready=rx_ready || net_work_pending || __atomic_load_n(&net_ap_pending.value,__ATOMIC_SEQ_CST);
        bool waited=!ready;
        if (!ready)
            wq_wait_timeout(&rxq_wq, 50);
        __atomic_store_n(&net_ap_sleeping.value,0,__ATOMIC_SEQ_CST);
        net_work_pending = false;
        irq_restore(f);
        uint64_t now=uptime_ms(); /* one clock read per bounded NIC batch */
        if(waited || !serviced) {
            service_started=now;service_start_bytes=net_service_bytes;serviced=false;
        } else if(now-service_started>=2 || net_service_bytes-service_start_bytes>=256u*1024u) {
            /* Honor timer/priority rescheduling at this maximum 2ms service
               window (or earlier byte boundary). Each NIC poll is bounded,
               and the AP gate is open before scheduling another task. */
#if NET_HANDOFF_DIAG
            net_diag.yields++;if(need_resched)net_diag.yield_resched++;if(rx_ready)net_diag.yield_pending++;
            if(!ready)net_diag.yield_idle++;
            if(current_task->sched_level<4)net_diag.yield_level[current_task->sched_level]++;
#endif
            sched_yield();
            service_started=uptime_ms();service_start_bytes=net_service_bytes;serviced=false;
        }
        net_bsp_enter();
        uint32_t ap_pending=__atomic_exchange_n(&net_ap_pending.value, 0, __ATOMIC_SEQ_CST);
        receive_batch = true;
        /* A completed AP copy/window update should reach the BSP-owned wire
           before draining an unrelated RX batch. Ordinary NIC-only turns do
           not pay another connection scan. RX may create new pending output,
           so retain the shared end-of-drain flush below. */
        if(ap_pending)tcp_flush_pending(true);
        if (nif->poll_rx) nif->poll_rx(eth_input);
        while (rxq_tail != rxq_head) {
            eth_input(rxq_buf[rxq_tail], rxq_len[rxq_tail]);
            rxq_tail = (rxq_tail + 1) % RXQ;
        }
        /* End-of-drain ACK and per-socket readiness, once per connection.
           Resource-blocked sends retry on NIC completion, not a fictitious
           packet loss/retransmission timeout. */
        tcp_flush_pending(false);
        receive_batch = false;
        for (size_t i=0;i<tcp_capacity;i++) {
            struct tcb *t = tcbs[i];
            if (t && t->progress_pending) { t->progress_pending=false; wq_wake_all(&t->progress); }
        }
        if (uptime_ms() - last_tick >= 50) {
            last_tick = uptime_ms();
            dhcp_tick();
            tcp_tick();
        }
        net_bsp_leave();
        serviced=true;
    }
}

static bool net_initialized;

static void net_start(void) {
#if NET_HANDOFF_DIAG
    net_diag_thread=kthread_create("net", net_thread, NULL);
#else
    kthread_create("net", net_thread, NULL);
#endif
    dhcp_start();
}

/* Adapters found by net_init's probes register before it starts the stack; a USB adapter may
   arrive later, and when it is the first one the stack starts with it. */
void net_register(struct netif *n) {
    if (nif) return;
    nif = n;
    if (net_initialized) {
        kprintf("net: %s adapter arrived\n", n->driver);
        net_start();
    }
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
    dns_configure();
    tcp_buffer_configure();
    bool found = netvsc_probe() || e1000_probe() || rtl8125_probe() || tulip_probe();
    net_initialized = true;
    if (!found || !nif) {
        kprintf("net: no supported network card yet\n");
        return;
    }
    net_start();
}

void net_process_exit(int pid) {
    net_bsp_enter();
    if (pid == fault_pid) fault_pid = 0;
    for(size_t i=0;i<udp_capacity;i++)
        if(udp_socks[i] && udp_socks[i]->used && udp_socks[i]->pid==pid)udp_release(udp_socks[i]);
    for(size_t i=0;i<tcp_capacity;i++)
        if(tcbs[i] && tcbs[i]->state!=T_FREE && tcbs[i]->user_open && tcbs[i]->pid==pid)tcp_close(tcbs[i]);
    net_bsp_leave();
}

bool net_config(uint8_t mac[6], uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns) {
    if (!nif) return false;
    memcpy(mac, nif->mac, 6);
    *ip = my_ip;
    *mask = my_mask;
    *gw = my_gw;
    *dns = dns_default_resolver().ip;
    return configured;
}

static int64_t net_syscall_inner(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    switch (num) {
    case SYS_NET_SETUP_CANCEL_CHANNEL: {
        int fd=(int)a;
        if(fd < -1)return -N_ERR_INVALID;
        struct file *file=NULL;
        if(fd>=0) {
            uint64_t flags=irq_save();
            struct task *owner=task_process();
            if((size_t)fd<owner->fd_capacity)file=owner->fds[fd];
            if(!file){irq_restore(flags);return -N_ERR_NO_HANDLE;}
            if(file->vn->type!=VT_CHANNEL || !file->vn->ops || !file->vn->ops->hangup){irq_restore(flags);return -N_ERR_INVALID;}
            vfs_dup(file);irq_restore(flags);
        }
        struct file *old=current_task->net_setup_cancel_file;
        current_task->net_setup_cancel_file=file;
        if(old)vfs_close(old);
        return 0;
    }
    case SYS_NET_INFO: {
        struct n_netinfo *ni = (struct n_netinfo *)a;
        if (!user_ok_w(ni, sizeof *ni)) return -N_ERR_BAD_ADDRESS;
        memset(ni, 0, sizeof *ni);
        if (!nif) return 0;
        ni->present = 1;
        ni->up = configured;
        memcpy(ni->mac, nif->mac, 6);
        ip_put(ni->ip, my_ip);
        ip_put(ni->mask, my_mask);
        ip_put(ni->gateway, my_gw);
        ip_put(ni->dns, dns_default_resolver().ip);
        ni->rx_packets = rx_packets;
        ni->tx_packets = tx_packets;
        strlcpy(ni->driver, nif->driver, sizeof ni->driver);
        return 0;
    }
    case SYS_NET_PING:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        return sys_ping((uint32_t)a, (int)b, (int)c);
    case SYS_NET_DNS: {
        if (!user_ok_w((void *)b, 4)) return -N_ERR_BAD_ADDRESS;
        char *name = NULL;
        int r = dns_user_name_copy((const char *)a, &name);
        if (r < 0) return r;
        struct dns_answer answer;
        struct n_sockaddr resolver=dns_default_resolver();
        int64_t ret = dns_query(name, resolver.ip, resolver.port, &answer);
        kfree(name);
        if (ret == 0) {
            if (!user_ok_w((void *)b,4)) ret=-N_ERR_BAD_ADDRESS;
            else *(uint32_t *)b = answer.ips[0];
        }
        kfree(answer.ips);
        return ret;
    }
    case SYS_NET_DNS_ALL: {
        struct n_dns_result *out=(struct n_dns_result *)d;
        if (!user_ok_w(out,sizeof *out) || c>SIZE_MAX/sizeof(uint32_t) || !user_ok_w((void *)b,c*sizeof(uint32_t))) return -N_ERR_BAD_ADDRESS;
        *out=(struct n_dns_result){.version=1,.status=N_DNS_FORMAT};
        char *name=NULL;int result=dns_user_name_copy((const char *)a,&name);
        if (result<0) return result;
        struct n_sockaddr resolver=dns_default_resolver();
        if (e) {
            if (!user_ok((void *)e,sizeof resolver)) {kfree(name);return -N_ERR_BAD_ADDRESS;}
            resolver=*(const struct n_sockaddr *)e;
        }
        struct dns_answer answer;
        result=dns_query(name,resolver.ip,resolver.port,&answer);kfree(name);
        /* A sibling may revoke user pages while resolution blocks. */
        if (!user_ok_w(out,sizeof *out) || !user_ok_w((void *)b,MIN(c,(size_t)answer.count)*sizeof(uint32_t))) {kfree(answer.ips);return -N_ERR_BAD_ADDRESS;}
        *out=(struct n_dns_result){.version=1,.status=answer.status,.count=answer.count,.ttl=answer.ttl};
        if (!result && c) memcpy((void *)b,answer.ips,MIN(c,(size_t)answer.count)*sizeof(uint32_t));
        kfree(answer.ips);return result;
    }
    case SYS_UDP_SOCKET:
        if (!nif) return -N_ERR_NETWORK_DOWN;
        return udp_alloc((uint16_t)a, task_process()->pid);
    case SYS_UDP_SEND: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -N_ERR_NO_HANDLE;
        if (e > 1472) return -N_ERR_INVALID;
        if (!user_ok((void *)d, e)) return -N_ERR_BAD_ADDRESS;
        uint8_t mac[6];
        uint64_t generation=s->generation;
        int r = arp_resolve((uint32_t)b, mac);
        if (r < 0) return r;
        if(!s->used || s->generation!=generation)return -N_ERR_NO_HANDLE;
        uint8_t tmp[1472];
        memcpy(tmp, (void *)d, e);
        r = udp_output(s->port, (uint32_t)b, (uint16_t)c, mac, tmp, e);
        return r < 0 ? r : (int64_t)e;
    }
    case SYS_UDP_RECV: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -N_ERR_NO_HANDLE;
        if (!user_ok_w((void *)b, c)) return -N_ERR_BAD_ADDRESS;
        struct n_sockaddr *from = (struct n_sockaddr *)e;
        if (from && !user_ok_w(from, sizeof *from)) return -N_ERR_BAD_ADDRESS;
        int err = 0;
        struct udp_dgram *dg = udp_wait((int)a, (int)d, &err);
        if (!dg) return err;
        size_t n = MIN(c, dg->len);
        memcpy((void *)b, dg->data, n);
        if (from) {
            from->ip = dg->src;
            from->port = dg->sport;
        }
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
        return sys_tcp_connect((uint32_t)a, (uint16_t)b, (int)c);
    case SYS_TCP_SEND: return tcp_send_data((int)a, (const uint8_t *)b, c, 0, true, false);
    case SYS_TCP_SEND_UNTIL:
        if(e&~(uint64_t)N_TCP_SEND_NONBLOCK)return -N_ERR_INVALID;
        return tcp_send_data((int)a, (const uint8_t *)b, c, d, true, (e&N_TCP_SEND_NONBLOCK)!=0);
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
        struct n_netstats *out = (struct n_netstats *)a;
        if (!user_ok_w(out, sizeof *out)) return -N_ERR_BAD_ADDRESS;
        memset(out,0,sizeof *out);
        out->rx_dropped=net_counters.rx_dropped;
        out->rx_invalid=net_counters.rx_invalid;
        out->rx_ring_high_water=net_counters.rx_ring_high_water;
        for (unsigned cpu=1;cpu<CPU_MAX_COUNT;cpu++) {
            out->ap_send_bytes[cpu]=__atomic_load_n(&net_counters.ap_send_bytes[cpu],__ATOMIC_RELAXED);
            out->ap_recv_bytes[cpu]=__atomic_load_n(&net_counters.ap_recv_bytes[cpu],__ATOMIC_RELAXED);
            out->ap_calls[cpu]=__atomic_load_n(&net_counters.ap_calls[cpu],__ATOMIC_RELAXED);
            out->ap_fallbacks[cpu]=__atomic_load_n(&net_counters.ap_fallbacks[cpu],__ATOMIC_RELAXED);
        }
        if (nif && nif->get_stats) nif->get_stats(out);
#if NET_HANDOFF_DIAG
        uint64_t fallback[ND_REASONS]={0};
        for(unsigned cpu=1;cpu<CPU_MAX_COUNT;cpu++)for(unsigned reason=0;reason<ND_REASONS;reason++)
            fallback[reason]+=__atomic_load_n(&net_diag.fallback[cpu][reason],__ATOMIC_RELAXED);
        kprintf("NETDIAG hz=%lu fb=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu gate=%lu/%lu/%lu handoff=%lu/%lu/%lu/noout%lu yield=%lu/resched%lu/pending%lu/idle%lu level=%lu/%lu/%lu/%lu push=%lu/%lu/%lu bytes=%lu/%lu/%lu\n",
                tsc_hz,fallback[0],fallback[1],fallback[2],fallback[3],fallback[4],fallback[5],fallback[6],fallback[7],fallback[8],
                net_diag.gate_joins,net_diag.gate_ticks,net_diag.gate_max,
                net_diag.handoffs,net_diag.handoff_ticks,net_diag.handoff_max,net_diag.handoff_no_output,
                net_diag.yields,net_diag.yield_resched,net_diag.yield_pending,net_diag.yield_idle,
                net_diag.yield_level[0],net_diag.yield_level[1],net_diag.yield_level[2],net_diag.yield_level[3],
                net_diag.pushes[0],net_diag.pushes[1],net_diag.pushes[2],net_diag.push_bytes[0],net_diag.push_bytes[1],net_diag.push_bytes[2]);
#endif
        return 0;
    }
    case SYS_TCP_RECV: return tcp_recv_data((int)a, (uint8_t *)b, c, (int)d, true);
    case SYS_NET_TEST: {
        /* a: fault settings for this process's TCP connections (or NULL); b: counters out (or NULL) */
        if (a) {
            if (!user_ok((void *)a, sizeof(struct n_netfault))) return -N_ERR_BAD_ADDRESS;
            fault = *(const struct n_netfault *)a;
            fault_pid = fault.rx_drop || fault.rx_reorder || fault.tx_drop ? task_process()->pid : 0;
        }
        if (b) {
            if (!user_ok_w((void *)b, sizeof(struct n_tcpstats))) return -N_ERR_BAD_ADDRESS;
            *(struct n_tcpstats *)b = tcp_stats;
        }
        return 0;
    }
    case SYS_TCP_CLOSE: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -N_ERR_NO_HANDLE;
        tcp_close(t);
        return 0;
    }
    }
    return -N_ERR_UNSUPPORTED;
}

int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    net_bsp_enter();
    int64_t result = net_syscall_inner(num, a, b, c, d, e);
    net_bsp_leave();
    return result;
}
