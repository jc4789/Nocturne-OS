/* Nocturne network stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and a small TCP client.
   Received frames are queued from the NIC interrupt and processed by the "net" kernel thread.
   System calls run in the calling task and block on net_wq until the thread reports progress.
   The kernel is non-preemptive, so stack state needs no locks besides the IRQ-fed queue. */
#include "kernel.h"
#include "abi.h"
#include "net.h"
#include "../dev/entropy.h"
#include "../arch/cpu.h"
#include "../mm/heap.h"
#include "../sys/sched.h"
#include "../sys/syscall.h"

static struct netif *nif;
static uint32_t my_ip, my_mask, my_gw, my_dns;
static bool configured;
static uint64_t rx_packets, tx_packets;
static struct wait_queue net_wq; /* progress for blocked system calls */
static struct wait_queue rxq_wq; /* the net thread sleeps here */

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
    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
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

void net_rx(const void *frame, size_t len) {
    if (len < 14 || len > 1536) return;
    int next = (rxq_head + 1) % RXQ;
    if (next == rxq_tail) return; /* full: drop */
    memcpy(rxq_buf[rxq_head], frame, len);
    rxq_len[rxq_head] = (uint16_t)len;
    rxq_head = next;
    wq_wake_all(&rxq_wq);
}

/* ---------------------------------------------------------------- output */

static uint8_t frame[1600];
#define L4 (frame + 14 + 20) /* transport payload area */
static uint16_t ip_ident;

static int eth_output(const uint8_t dst[6], uint16_t type, size_t len) {
    if (!nif) return -ENETDOWN;
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, nif->mac, 6);
    e->type = htons(type);
    size_t total = 14 + len;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    tx_packets++;
    return nif->send(frame, total);
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

/* resolve the link address for dst (system call context: may block) */
static int arp_resolve(uint32_t dst, uint8_t mac[6]) {
    if (!nif) return -ENETDOWN;
    if (dst == 0xFFFFFFFF || dst == (my_ip | ~my_mask)) {
        memcpy(mac, bcast_mac, 6);
        return 0;
    }
    if (!configured) return -ENETDOWN;
    uint32_t hop = next_hop(dst);
    if (!hop) return -EHOSTUNREACH;
    for (int tries = 0; tries < 3; tries++) {
        if (arp_lookup(hop, mac)) return 0;
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", hop, bcast_mac);
        uint64_t until = uptime_ms() + 1000;
        while (uptime_ms() < until) {
            if (arp_lookup(hop, mac)) return 0;
            if (current_task->killed) return -EINTR;
            wq_wait_timeout(&net_wq, until - uptime_ms());
        }
    }
    return -EHOSTUNREACH;
}

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
    if (slot < 0) return -EBUSY;
    struct ping_wait *w = &pings[slot];
    w->used = true;
    w->done = false;
    w->id = (uint16_t)(0x4E00 + current_task->pid);
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
    int64_t ret = -ETIMEDOUT;
    while (uptime_ms() < until) {
        if (w->done) {
            ret = (int64_t)(uptime_ms() - t0);
            break;
        }
        if (current_task->killed) {
            ret = -EINTR;
            break;
        }
        wq_wait_timeout(&net_wq, until - uptime_ms());
    }
    if (w->done) ret = (int64_t)(uptime_ms() - t0);
    w->used = false;
    return ret;
}

/* ---------------------------------------------------------------- UDP */

#define UDP_N 8
#define UDP_Q 8
struct udp_dgram {
    uint32_t src;
    uint16_t sport, len;
    uint8_t data[1472];
};
static struct udp_sock {
    bool used;
    int pid;
    uint16_t port;
    struct udp_dgram q[UDP_Q];
    int qh, qt;
} udp_socks[UDP_N];
static uint16_t next_port = 49152;

static uint16_t ephemeral_port(void) {
    if (next_port < 49152) next_port = 49152;
    return next_port++;
}

static int udp_output(uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t mac[6], const void *data,
                      size_t len) {
    if (len > 1472) return -EINVAL;
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
    for (int i = 0; i < UDP_N; i++) {
        struct udp_sock *s = &udp_socks[i];
        if (!s->used || s->port != dport) continue;
        int next = (s->qh + 1) % UDP_Q;
        if (next == s->qt || ulen - 8 > sizeof s->q[0].data) return;
        struct udp_dgram *d = &s->q[s->qh];
        d->src = src;
        d->sport = ntohs(u->sport);
        d->len = (uint16_t)(ulen - 8);
        memcpy(d->data, p + 8, ulen - 8);
        s->qh = next;
        wq_wake_all(&net_wq);
        poll_notify();
        return;
    }
}

static int udp_alloc(uint16_t port, int pid) {
    if (!port) port = ephemeral_port();
    for (int i = 0; i < UDP_N; i++)
        if (udp_socks[i].used && udp_socks[i].port == port) return -EADDRINUSE;
    for (int i = 0; i < UDP_N; i++)
        if (!udp_socks[i].used) {
            memset(&udp_socks[i], 0, sizeof udp_socks[i]);
            udp_socks[i].used = true;
            udp_socks[i].pid = pid;
            udp_socks[i].port = port;
            return i;
        }
    return -EMFILE;
}

/* wait for a datagram on socket i; returns pointer or NULL on timeout (and sets *err) */
static struct udp_dgram *udp_wait(int i, int timeout_ms, int *err) {
    struct udp_sock *s = &udp_socks[i];
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (s->qt != s->qh) {
            struct udp_dgram *d = &s->q[s->qt];
            s->qt = (s->qt + 1) % UDP_Q;
            return d;
        }
        if (current_task->killed) {
            *err = -EINTR;
            return NULL;
        }
        if (timeout_ms >= 0 && uptime_ms() >= until) {
            *err = -EAGAIN;
            return NULL;
        }
        wq_wait_timeout(&net_wq, timeout_ms < 0 ? 1000 : until - uptime_ms());
    }
}

static struct udp_sock *user_udp(int h) {
    if (h < 0 || h >= UDP_N || !udp_socks[h].used || udp_socks[h].pid != current_task->pid) return NULL;
    return &udp_socks[h];
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
        kprintf("net: %u.%u.%u.%u/%d gateway %u.%u.%u.%u dns %u.%u.%u.%u (DHCP)\n", my_ip >> 24, (my_ip >> 16) & 255,
                (my_ip >> 8) & 255, my_ip & 255, __builtin_popcount(my_mask), my_gw >> 24, (my_gw >> 16) & 255,
                (my_gw >> 8) & 255, my_gw & 255, my_dns >> 24, (my_dns >> 16) & 255, (my_dns >> 8) & 255,
                my_dns & 255);
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

#define DNS_CACHE 8
static struct {
    char name[64];
    uint32_t ip;
    uint64_t expires;
} dns_cache[DNS_CACHE];
static int dns_cache_next;

/* skip a (possibly compressed) DNS name; returns new offset or 0 on error */
static size_t dns_skip_name(const uint8_t *p, size_t len, size_t o) {
    while (o < len) {
        uint8_t l = p[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2;
        o += 1 + l;
    }
    return 0;
}

static int64_t dns_query(const char *name, uint32_t *out) {
    if (parse_ip(name, out)) return 0;
    if (!strcmp(name, "localhost")) {
        *out = 0x7F000001;
        return 0;
    }
    for (int i = 0; i < DNS_CACHE; i++)
        if (dns_cache[i].expires > uptime_ms() && !strcmp(dns_cache[i].name, name)) {
            *out = dns_cache[i].ip;
            return 0;
        }
    if (!configured || !my_dns) return -ENETDOWN;
    uint8_t q[300];
    size_t n = 12;
    uint16_t id = (uint16_t)rnd();
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01; /* recursion desired */
    q[5] = 1;    /* one question */
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || n + l + 6 > sizeof q) return -EINVAL;
        q[n++] = (uint8_t)l;
        memcpy(q + n, s, l);
        n += l;
        s += l;
        if (*s == '.') s++;
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1; /* A */
    q[n++] = 0;
    q[n++] = 1; /* IN */
    uint8_t mac[6];
    int r = arp_resolve(my_dns, mac);
    if (r < 0) return r;
    int sock = udp_alloc(0, 0);
    if (sock < 0) return sock;
    int64_t ret = -ETIMEDOUT;
    for (int tries = 0; tries < 3 && ret == -ETIMEDOUT; tries++) {
        udp_output(udp_socks[sock].port, my_dns, 53, mac, q, n);
        uint64_t until = uptime_ms() + 2000;
        while (uptime_ms() < until) {
            int err = 0;
            struct udp_dgram *d = udp_wait(sock, (int)(until - uptime_ms()), &err);
            if (!d) {
                if (err == -EINTR) ret = -EINTR;
                break;
            }
            const uint8_t *p = d->data;
            size_t len = d->len;
            if (len < 12 || p[0] != q[0] || p[1] != q[1] || !(p[2] & 0x80)) continue;
            if ((p[3] & 15) != 0) {
                ret = -ENOENT;
                break;
            }
            int qd = p[4] << 8 | p[5], an = p[6] << 8 | p[7];
            size_t o = 12;
            for (int i = 0; i < qd && o; i++) {
                o = dns_skip_name(p, len, o);
                if (o) o += 4;
            }
            ret = -ENOENT;
            for (int i = 0; i < an && o && o < len; i++) {
                o = dns_skip_name(p, len, o);
                if (!o || o + 10 > len) break;
                int type = p[o] << 8 | p[o + 1];
                uint32_t ttl = ip_get(p + o + 4);
                int rdlen = p[o + 8] << 8 | p[o + 9];
                o += 10;
                if (type == 1 && rdlen == 4 && o + 4 <= len) {
                    *out = ip_get(p + o);
                    int c = dns_cache_next++ % DNS_CACHE;
                    strlcpy(dns_cache[c].name, name, sizeof dns_cache[c].name);
                    dns_cache[c].ip = *out;
                    dns_cache[c].expires = uptime_ms() + (uint64_t)MIN(ttl, 3600u) * 1000;
                    ret = 0;
                    break;
                }
                o += rdlen;
            }
            break;
        }
    }
    udp_socks[sock].used = false;
    return ret;
}

/* ---------------------------------------------------------------- TCP */

#define TCP_N     16
#define TCP_RXBUF 65536
#define TCP_TXBUF 32768
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
#define CWND_MAX  (1u << 20)

static struct tcb {
    int state, err, pid;
    bool user_open, fin_pending, fin_sent, rx_fin;
    uint32_t rip;
    uint16_t lport, rport, mss;
    uint8_t mac[6];
    /* send side. tx[0] is the byte at snd_una; snd_nxt is the next byte to (re)send, snd_max the
       highest ever sent (they differ after a timeout, when we go back to snd_una) */
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, fin_seq;
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
    struct { uint32_t start, end; } ooo[TCP_OOO];
    int nooo;
    uint8_t *rx, *tx;
    uint32_t rx_head, rx_len, tx_len;
    uint64_t rto_at, linger_until;
    int rto, retries;
} tcbs[TCP_N];

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
    kfree(t->rx);
    kfree(t->tx);
    memset(t, 0, sizeof *t);
}

static uint32_t tcp_window(struct tcb *t) { return MIN(TCP_RXBUF - t->rx_len, 65535u); }

static void tcp_output(struct tcb *t, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len) {
    tcp_stats.segs_out++;
    if (len && t->pid == fault_pid && fault_hit(fault.tx_drop)) {
        tcp_stats.faults_dropped++;
        return; /* "lost" on the wire */
    }
    struct tcp_hdr *h = (struct tcp_hdr *)L4;
    size_t hl = (flags & F_SYN) ? 24 : 20;
    h->sport = htons(t->lport);
    h->dport = htons(t->rport);
    h->seq = htonl(seq);
    h->ack = htonl((flags & F_ACK) ? t->rcv_nxt : 0);
    h->off = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    t->adv_wnd = tcp_window(t);
    h->win = htons((uint16_t)t->adv_wnd);
    h->csum = 0;
    h->urg = 0;
    if (flags & F_SYN) {
        L4[20] = 2; /* MSS option */
        L4[21] = 4;
        L4[22] = 1460 >> 8;
        L4[23] = 1460 & 255;
    }
    if (len) memcpy(L4 + hl, data, len);
    h->csum = htons(l4_csum(my_ip, t->rip, 6, L4, hl + len));
    ip_output(t->rip, t->mac, 6, hl + len);
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

/* send whatever data (and FIN) the send and congestion windows allow, from snd_nxt on;
   force sends one byte into a zero window (a window probe) */
static void tcp_push(struct tcb *t, bool force) {
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT && t->state != T_FIN_WAIT1 &&
        t->state != T_LAST_ACK && t->state != T_CLOSING)
        return;
    uint32_t wnd = MIN(t->snd_wnd, t->cwnd);
    if (force && wnd == 0) wnd = 1;
    for (;;) {
        uint32_t off = t->snd_nxt - t->snd_una; /* = bytes in flight, and the offset into tx */
        if (off < t->tx_len) {
            if (off >= wnd) break;
            uint32_t n = MIN(t->tx_len - off, MIN((uint32_t)t->mss, wnd - off));
            tcp_output(t, F_ACK | F_PSH, t->snd_nxt, t->tx + off, n);
            tcp_sent(t, n);
            continue;
        }
        if (t->fin_pending && off == t->tx_len && (!t->fin_sent || t->snd_nxt == t->fin_seq)) {
            if (!t->fin_sent) {
                t->fin_sent = true;
                t->fin_seq = t->snd_nxt;
                if (t->state == T_ESTABLISHED) t->state = T_FIN_WAIT1;
                else if (t->state == T_CLOSE_WAIT) t->state = T_LAST_ACK;
            }
            tcp_output(t, F_FIN | F_ACK, t->snd_nxt, NULL, 0);
            tcp_sent(t, 1);
        }
        break;
    }
    /* data waiting behind a zero window: probe it from the retransmit timer */
    if (t->tx_len && t->snd_una == t->snd_max && t->snd_wnd == 0) tcp_arm(t);
}

/* retransmit the oldest unacknowledged segment */
static void tcp_resend_first(struct tcb *t) {
    uint32_t n = MIN(t->tx_len, (uint32_t)t->mss);
    if (n) tcp_output(t, F_ACK | F_PSH, t->snd_una, t->tx, n);
    else if (t->fin_sent && t->snd_una == t->fin_seq) tcp_output(t, F_FIN | F_ACK, t->fin_seq, NULL, 0);
    t->rtt_at = 0;
    tcp_stats.retransmits++;
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
    t->ssthresh = 65535;
    t->recover = t->snd_una;
}

/* merge [s, e) into the out-of-order list; if it is full, the data is simply not remembered
   (the sender will retransmit it) */
static void tcp_ooo_add(struct tcb *t, uint32_t s, uint32_t e) {
    for (int i = 0; i < t->nooo;) {
        if (SEQ_LEQ(t->ooo[i].start, e) && SEQ_LEQ(s, t->ooo[i].end)) { /* overlaps or touches */
            if (SEQ_LT(t->ooo[i].start, s)) s = t->ooo[i].start;
            if (SEQ_LT(e, t->ooo[i].end)) e = t->ooo[i].end;
            t->ooo[i] = t->ooo[--t->nooo];
            i = 0;
        } else {
            i++;
        }
    }
    if (t->nooo < TCP_OOO) {
        t->ooo[t->nooo].start = s;
        t->ooo[t->nooo].end = e;
        t->nooo++;
    }
}

/* after rcv_nxt moved: take in every stored range that now continues the in-order data */
static void tcp_ooo_pull(struct tcb *t) {
    for (int i = 0; i < t->nooo;) {
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
        tcp_output(t, F_RST | F_ACK, t->snd_nxt, NULL, 0);
    t->state = T_CLOSED;
    t->err = err;
    t->rto_at = 0;
    wq_wake_all(&net_wq);
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
    for (int i = 0; i < TCP_N; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].rip == src && tcbs[i].rport == sport && tcbs[i].lport == dport)
            t = &tcbs[i];
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
                t->err = -ECONNREFUSED;
            }
        } else if (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + MAX(t->adv_wnd, 1u))) {
            t->state = T_CLOSED;
            t->err = -ECONNRESET;
        }
        t->rto_at = 0;
        wq_wake_all(&net_wq);
        return;
    }

    if (t->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != t->iss + 1) return;
        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_wnd = ntohs(h->win);
        t->mss = 536;
        for (size_t o = 20; o + 1 < hl;) {
            uint8_t kind = p[o];
            if (kind == 0) break;
            if (kind == 1) {
                o++;
                continue;
            }
            uint8_t l = p[o + 1];
            if (l < 2) break;
            if (kind == 2 && l == 4 && o + 4 <= hl) t->mss = (uint16_t)MIN(p[o + 2] << 8 | p[o + 3], 1460);
            o += l;
        }
        t->state = T_ESTABLISHED;
        t->rto_at = 0;
        t->retries = 0;
        t->rto = 1000;
        t->snd_max = t->snd_nxt;
        tcp_cc_init(t);
        tcp_output(t, F_ACK, t->snd_nxt, NULL, 0);
        wq_wake_all(&net_wq);
        tcp_push(t, false);
        return;
    }

    bool need_ack = false;
    if (fl & F_ACK) {
        if (SEQ_LT(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_max)) {
            uint32_t acked = ack - t->snd_una;
            bool fin_acked = t->fin_sent && ack == t->fin_seq + 1;
            uint32_t data_acked = acked - (fin_acked ? 1 : 0);
            if (data_acked > t->tx_len) data_acked = t->tx_len;
            memmove(t->tx, t->tx + data_acked, t->tx_len - data_acked);
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
            t->cwnd = MIN(t->cwnd, CWND_MAX);
            t->dupacks = 0;
            if (t->snd_una != t->snd_max) tcp_arm(t);
            if (fin_acked) {
                if (t->state == T_FIN_WAIT1) t->state = T_FIN_WAIT2;
                else if (t->state == T_CLOSING) {
                    t->state = T_TIME_WAIT;
                    t->linger_until = uptime_ms() + 2000;
                } else if (t->state == T_LAST_ACK) t->state = T_CLOSED;
            }
            wq_wake_all(&net_wq);
        } else if (ack == t->snd_una && t->snd_una != t->snd_max && dlen == 0 && !(fl & (F_SYN | F_FIN)) &&
                   ntohs(h->win) == t->snd_wnd) {
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
                t->cwnd = MIN(t->cwnd + t->mss, CWND_MAX); /* each dup ack means one segment left the network */
            }
        }
        if (SEQ_LEQ(t->snd_una, ack)) t->snd_wnd = ntohs(h->win);
    }

    /* data: trim what we already have, store the rest at its place in the ring (it may be ahead of
       rcv_nxt, after a lost or reordered segment), then move rcv_nxt over everything contiguous */
    if (dlen) {
        need_ack = true; /* an out-of-order segment gets an immediate duplicate ack */
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
            uint32_t space = TCP_RXBUF - t->rx_len, off = seq - t->rcv_nxt;
            if (off < space) {
                uint32_t n = MIN(dlen, space - off), at = t->rx_head + t->rx_len + off;
                for (uint32_t i = 0; i < n; i++) t->rx[(at + i) % TCP_RXBUF] = data[i];
                dlen = n;
                if (off == 0) {
                    t->rx_len += n;
                    t->rcv_nxt += n;
                    tcp_ooo_pull(t);
                    in_order = true;
                    wq_wake_all(&net_wq);
                    poll_notify();
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
        need_ack = true;
        if (t->state == T_ESTABLISHED) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1) t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT2) {
            t->state = T_TIME_WAIT;
            t->linger_until = uptime_ms() + 2000;
        }
        wq_wake_all(&net_wq);
        poll_notify();
    }
    if (need_ack) tcp_output(t, F_ACK, t->snd_nxt, NULL, 0);
    tcp_push(t, false);
}

static void tcp_tick(void) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < TCP_N; i++) {
        struct tcb *t = &tcbs[i];
        if (t->state == T_FREE) continue;
        if (t->state == T_TIME_WAIT && now >= t->linger_until) t->state = T_CLOSED;
        if (!t->user_open && (t->state == T_CLOSED || now >= t->linger_until)) {
            if (t->state != T_CLOSED) tcp_reset(t, -ECONNRESET);
            tcp_free(t);
            continue;
        }
        if (t->rto_at && now >= t->rto_at) {
            t->rto_at = 0;
            bool probe = t->state != T_SYN_SENT && t->snd_wnd == 0; /* persist: the peer is just not reading */
            if (!probe && ++t->retries > (t->state == T_SYN_SENT ? 5 : 8)) {
                tcp_reset(t, -ETIMEDOUT);
                continue;
            }
            t->rto = MIN(t->rto * 2, RTO_MAX);
            if (t->state == T_SYN_SENT) {
                tcp_output(t, F_SYN, t->iss, NULL, 0);
                tcp_arm(t);
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
                tcp_push(t, true);
                if (t->snd_una != t->snd_max) tcp_arm(t);
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
    for (int i = 0; i < TCP_N; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].state != T_CLOSED && tcbs[i].rip == fault_held_src &&
            tcbs[i].rport == ntohs(h->sport) && tcbs[i].lport == ntohs(h->dport)) {
            tcp_segment(&tcbs[i], seg, n);
            return;
        }
}

static struct tcb *user_tcb(int h) {
    if (h < 0 || h >= TCP_N || tcbs[h].state == T_FREE || !tcbs[h].user_open || tcbs[h].pid != current_task->pid)
        return NULL;
    return &tcbs[h];
}

static int64_t sys_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    uint8_t mac[6];
    int r = arp_resolve(ip, mac);
    if (r < 0) return r;
    int h = -1;
    for (int i = 0; i < TCP_N; i++)
        if (tcbs[i].state == T_FREE) {
            h = i;
            break;
        }
    if (h < 0) return -EMFILE;
    struct tcb *t = &tcbs[h];
    memset(t, 0, sizeof *t);
    t->rx = kmalloc(TCP_RXBUF);
    t->tx = kmalloc(TCP_TXBUF);
    if (!t->rx || !t->tx) {
        tcp_free(t);
        return -ENOMEM;
    }
    t->pid = current_task->pid;
    t->user_open = true;
    t->rip = ip;
    t->rport = port;
    t->lport = ephemeral_port();
    memcpy(t->mac, mac, 6);
    t->iss = rnd();
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1;
    t->mss = 536;
    t->rto = 1000;
    t->state = T_SYN_SENT;
    tcp_output(t, F_SYN, t->iss, NULL, 0);
    tcp_arm(t);
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 15000);
    while (t->state == T_SYN_SENT) {
        if (current_task->killed || uptime_ms() >= until) {
            t->state = T_CLOSED;
            t->err = current_task->killed ? -EINTR : -ETIMEDOUT;
            break;
        }
        wq_wait_timeout(&net_wq, until - uptime_ms());
    }
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT) {
        int err = t->err ? t->err : -ECONNREFUSED;
        tcp_free(t);
        return err;
    }
    return h;
}

static int64_t sys_tcp_send(int h, const uint8_t *buf, size_t len) {
    struct tcb *t = user_tcb(h);
    if (!t) return -EBADF;
    if (!user_ok(buf, len)) return -EFAULT;
    size_t done = 0;
    while (done < len) {
        if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT) return done ? (int64_t)done : (t->err ? t->err : -EPIPE);
        if (t->fin_pending) return -EPIPE;
        uint32_t space = TCP_TXBUF - t->tx_len;
        if (!space) {
            if (current_task->killed) return done ? (int64_t)done : -EINTR;
            wq_wait_timeout(&net_wq, 1000);
            continue;
        }
        uint32_t n = (uint32_t)MIN(len - done, space);
        memcpy(t->tx + t->tx_len, buf + done, n);
        t->tx_len += n;
        done += n;
        tcp_push(t, false);
    }
    return (int64_t)done;
}

static int64_t sys_tcp_recv(int h, uint8_t *buf, size_t len, int timeout_ms) {
    struct tcb *t = user_tcb(h);
    if (!t) return -EBADF;
    if (!user_ok_w(buf, len)) return -EFAULT;
    uint64_t until = uptime_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    while (t->rx_len == 0) {
        if (t->rx_fin) return 0;
        if (t->state == T_CLOSED) return t->err ? t->err : 0;
        if (current_task->killed) return -EINTR;
        if (timeout_ms >= 0 && uptime_ms() >= until) return -EAGAIN;
        wq_wait_timeout(&net_wq, timeout_ms < 0 ? 1000 : until - uptime_ms());
    }
    uint32_t n = (uint32_t)MIN(len, t->rx_len);
    for (uint32_t i = 0; i < n; i++) buf[i] = t->rx[(t->rx_head + i) % TCP_RXBUF];
    t->rx_head = (t->rx_head + n) % TCP_RXBUF;
    t->rx_len -= n;
    /* window update if we had been squeezing the sender */
    if (t->adv_wnd < 16384 && tcp_window(t) >= 16384 && t->state != T_CLOSED) tcp_output(t, F_ACK, t->snd_nxt, NULL, 0);
    return n;
}

static void tcp_close(struct tcb *t) {
    t->user_open = false;
    t->linger_until = uptime_ms() + 20000;
    if (t->state == T_ESTABLISHED || t->state == T_CLOSE_WAIT) {
        t->fin_pending = true;
        tcp_push(t, false);
    } else if (t->state == T_SYN_SENT || t->state == T_CLOSED) {
        tcp_free(t);
    }
}

/* ---------------------------------------------------------------- input dispatch, thread, syscalls */

static void eth_input(const uint8_t *p, size_t len) {
    const struct eth_hdr *e = (const struct eth_hdr *)p;
    if (memcmp(e->dst, nif->mac, 6) != 0 && !(e->dst[0] & 1)) return; /* not for us (promiscuous NICs) */
    rx_packets++;
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

static void net_thread(void *arg) {
    (void)arg;
    uint64_t last_tick = 0;
    for (;;) {
        uint64_t f = irq_save();
        if (rxq_tail == rxq_head) wq_wait_timeout(&rxq_wq, 50);
        irq_restore(f);
        while (rxq_tail != rxq_head) {
            eth_input(rxq_buf[rxq_tail], rxq_len[rxq_tail]);
            rxq_tail = (rxq_tail + 1) % RXQ;
        }
        if (uptime_ms() - last_tick >= 50) {
            last_tick = uptime_ms();
            dhcp_tick();
            tcp_tick();
        }
    }
}

void net_register(struct netif *n) {
    if (!nif) nif = n;
}

void net_init(void) {
    if (!netvsc_probe() && !e1000_probe() && !tulip_probe()) {
        kprintf("net: no supported network card\n");
        return;
    }
    kthread_create("net", net_thread, NULL);
    dhcp_start();
}

void net_process_exit(int pid) {
    if (pid == fault_pid) fault_pid = 0;
    for (int i = 0; i < UDP_N; i++)
        if (udp_socks[i].used && udp_socks[i].pid == pid) udp_socks[i].used = false;
    for (int i = 0; i < TCP_N; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].user_open && tcbs[i].pid == pid) tcp_close(&tcbs[i]);
}

bool net_config(uint8_t mac[6], uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns) {
    if (!nif) return false;
    memcpy(mac, nif->mac, 6);
    *ip = my_ip;
    *mask = my_mask;
    *gw = my_gw;
    *dns = my_dns;
    return configured;
}

int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    switch (num) {
    case SYS_NET_INFO: {
        struct n_netinfo *ni = (struct n_netinfo *)a;
        if (!user_ok_w(ni, sizeof *ni)) return -EFAULT;
        memset(ni, 0, sizeof *ni);
        if (!nif) return 0;
        ni->present = 1;
        ni->up = configured;
        memcpy(ni->mac, nif->mac, 6);
        ip_put(ni->ip, my_ip);
        ip_put(ni->mask, my_mask);
        ip_put(ni->gateway, my_gw);
        ip_put(ni->dns, my_dns);
        ni->rx_packets = rx_packets;
        ni->tx_packets = tx_packets;
        strlcpy(ni->driver, nif->driver, sizeof ni->driver);
        return 0;
    }
    case SYS_NET_PING:
        if (!nif) return -ENETDOWN;
        return sys_ping((uint32_t)a, (int)b, (int)c);
    case SYS_NET_DNS: {
        char name[128];
        int r = user_str(name, (const char *)a, sizeof name);
        if (r < 0) return r;
        if (!user_ok_w((void *)b, 4)) return -EFAULT;
        uint32_t ip = 0;
        int64_t ret = dns_query(name, &ip);
        if (ret == 0) *(uint32_t *)b = ip;
        return ret;
    }
    case SYS_UDP_SOCKET:
        if (!nif) return -ENETDOWN;
        return udp_alloc((uint16_t)a, current_task->pid);
    case SYS_UDP_SEND: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -EBADF;
        if (e > 1472) return -EINVAL;
        if (!user_ok((void *)d, e)) return -EFAULT;
        uint8_t mac[6];
        int r = arp_resolve((uint32_t)b, mac);
        if (r < 0) return r;
        uint8_t tmp[1472];
        memcpy(tmp, (void *)d, e);
        r = udp_output(s->port, (uint32_t)b, (uint16_t)c, mac, tmp, e);
        return r < 0 ? r : (int64_t)e;
    }
    case SYS_UDP_RECV: {
        struct udp_sock *s = user_udp((int)a);
        if (!s) return -EBADF;
        if (!user_ok_w((void *)b, c)) return -EFAULT;
        struct n_sockaddr *from = (struct n_sockaddr *)e;
        if (from && !user_ok_w(from, sizeof *from)) return -EFAULT;
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
        if (!s) return -EBADF;
        s->used = false;
        return 0;
    }
    case SYS_TCP_CONNECT:
        if (!nif) return -ENETDOWN;
        return sys_tcp_connect((uint32_t)a, (uint16_t)b, (int)c);
    case SYS_TCP_SEND: return sys_tcp_send((int)a, (const uint8_t *)b, c);
    case SYS_TCP_RECV: return sys_tcp_recv((int)a, (uint8_t *)b, c, (int)d);
    case SYS_NET_TEST: {
        /* a: fault settings for this process's TCP connections (or NULL); b: counters out (or NULL) */
        if (a) {
            if (!user_ok((void *)a, sizeof(struct n_netfault))) return -EFAULT;
            fault = *(const struct n_netfault *)a;
            fault_pid = fault.rx_drop || fault.rx_reorder || fault.tx_drop ? current_task->pid : 0;
        }
        if (b) {
            if (!user_ok_w((void *)b, sizeof(struct n_tcpstats))) return -EFAULT;
            *(struct n_tcpstats *)b = tcp_stats;
        }
        return 0;
    }
    case SYS_TCP_CLOSE: {
        struct tcb *t = user_tcb((int)a);
        if (!t) return -EBADF;
        tcp_close(t);
        return 0;
    }
    }
    return -ENOSYS;
}
