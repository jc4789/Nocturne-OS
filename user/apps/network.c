/* サービス network：網の方針の持ち主。カーネルは装置、IP、TCP、UDP と、住所と経路を適用する口を持つ。
   ここが住所の決め方（DHCP か固定）、経路の表、名前引きとその cache を持つ。

   一本の主のループが全部の接続と cache を持つ。錠なしで触れるのは主のループだけ。
     - 接続ごとにスレッドを作らない。接続が数百でも事象の行列の項目が数百あるだけで、待機中は回らない。
     - 応答を送る前に SENDABLE を待つので、読まない相手が主のループを止めない。
     - UDP のソケットは待ち合わせられないので、名前引きは名前ごとの短命のスレッドが行う。同じ名前の
       要求は一つのスレッドの答えを待つ。待つ者が全部消えたら主のループが止めの channel を閉じる。
     - DHCP は adapter ごとに、DHCP を使う間だけスレッドが一本。次の期限まで udp_recvfrom で眠る。
   links_lock は adapter の構成を守る。DHCP のスレッドと主のループが取る。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"
#include "dns_name.h"

#define NS_PER_S 1000000000ull
#define BROADCAST 0xffffffffu

static n_handle network_cap;
static n_handle queue;

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t random32(void) {
    uint32_t v = 0;
    n_random(&v, sizeof v);
    return v;
}

/* ---- adapter の構成 ---- */

struct link {
    struct link *next;
    uint32_t adapter;
    uint8_t mac[6];
    uint64_t device;   /* N_NETWORK_FIXED_ADDRESS の装置の番号 */
    /* 主のループだけ */
    bool present;
    struct n_setting_ipv4 fixed;
    int thread;        /* DHCP のスレッド。無ければ -1。 */
    n_handle stop;     /* 閉じるとそのスレッドが終わる */
    /* links_lock */
    uint32_t state;    /* enum n_link_state */
    uint32_t gateway;
    uint64_t lease_end;
    uint32_t *addresses; /* DNS の dns_count 個、NTP の ntp_count 個 */
    uint32_t dns_count, ntp_count;
    struct n_net_route *routes;
    size_t route_count;
};
static struct link *links, **links_end = &links;
static mtx_t links_lock;
static uint64_t dns_generation; /* links_lock：DHCP の言う DNS が変わるたびに増える */

/* links_lock の下で。全部の adapter の経路をカーネルへ渡す。 */
static void routes_apply(void) {
    size_t count = 0;
    for (struct link *l = links; l; l = l->next) count += l->route_count;
    struct n_net_route *table = count ? malloc(count * sizeof *table) : NULL;
    if (count && !table) {
        printf("network: no memory for the route table\n");
        return;
    }
    size_t at = 0;
    for (struct link *l = links; l; l = l->next)
        for (size_t i = 0; i < l->route_count; i++) table[at++] = l->routes[i];
    n_result r = n_net_routes_set(network_cap, table, count);
    if (r < 0) printf("network: the routes were refused (%s)\n", n_result_text(r));
    free(table);
}

/* 構成を置き換える。addresses と routes は link のものになる。 */
static void link_config(struct link *l, uint32_t state, uint32_t gateway, uint64_t lease_end, uint32_t *addresses, uint32_t dns_count,
                        uint32_t ntp_count, struct n_net_route *routes, size_t route_count) {
    mtx_lock(&links_lock);
    if (l->dns_count != dns_count || (dns_count && memcmp(l->addresses, addresses, dns_count * sizeof *addresses))) dns_generation++;
    free(l->addresses);
    free(l->routes);
    l->state = state;
    l->gateway = gateway;
    l->lease_end = lease_end;
    l->addresses = addresses;
    l->dns_count = dns_count;
    l->ntp_count = ntp_count;
    l->routes = routes;
    l->route_count = route_count;
    routes_apply();
    mtx_unlock(&links_lock);
}

/* 既定の経路一つの表。gateway 0 は経路なし。 */
static struct n_net_route *default_route(const struct link *l, uint32_t gateway, size_t *count) {
    *count = 0;
    if (!gateway) return NULL;
    struct n_net_route *r = malloc(sizeof *r);
    if (!r) return NULL;
    *r = (struct n_net_route){.gateway = gateway, .adapter = l->adapter, .metric = l->adapter};
    *count = 1;
    return r;
}

/* ---- DHCP（RFC 2131）---- */

enum { DHCP_DISCOVER = 1, DHCP_OFFER, DHCP_REQUEST, DHCP_DECLINE, DHCP_ACK, DHCP_NAK };
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_FIXED_BYTES 236
#define DHCP_MESSAGE_MIN 300 /* RFC 1542 2.1：BOOTP の中継が受ける最小の長さ */
#define DHCP_MESSAGE_MAX 548 /* RFC 2132 9.10：option 57 を言わない相手が送る最大（576 から IP と UDP の 28） */
#define DHCP_RETRY_MAX (64 * NS_PER_S)          /* RFC 2131 4.1：4, 8, 16, 32, 64 秒 */
#define DHCP_LEASE_RETRY_MIN (60 * NS_PER_S)    /* RFC 2131 4.4.5：延長の再送の間隔の下限 */
#define DHCP_REST (10 * NS_PER_S)               /* RFC 2131 3.1：やり直す前に置く間 */
#define DHCP_INFINITE 0xffffffffu               /* RFC 2131 3.3 */

struct dhcp_reply {
    int type;
    uint32_t yiaddr, server, mask, lease, t1, t2, router;
    uint32_t *dns, *ntp;
    uint32_t dns_count, ntp_count;
    struct n_net_route *classless;
    size_t classless_count;
};
struct dhcp {
    struct link *link;
    n_handle socket;
    uint32_t xid;
};

static void dhcp_reply_free(struct dhcp_reply *r) {
    free(r->dns);
    free(r->ntp);
    free(r->classless);
    memset(r, 0, sizeof *r);
}

static uint32_t *address_list(const uint8_t *v, size_t bytes, uint32_t *count) {
    size_t n = bytes / 4;
    uint32_t *list = n ? malloc(n * sizeof *list) : NULL;
    *count = list ? (uint32_t)n : 0;
    for (size_t i = 0; list && i < n; i++) list[i] = get32(v + i * 4);
    return list;
}

/* RFC 3442：幅、宛先の意味のあるバイト、ルーター。 */
static void classless_routes(struct dhcp_reply *r, const uint8_t *v, size_t bytes, uint32_t adapter) {
    size_t count = 0;
    for (size_t at = 0; at < bytes;) {
        if (v[at] > 32) return;
        at += 1 + (size_t)(v[at] + 7) / 8 + 4;
        if (at > bytes) return;
        count++;
    }
    struct n_net_route *table = count ? malloc(count * sizeof *table) : NULL;
    if (!table) return;
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned width = v[at++], octets = (width + 7) / 8;
        uint8_t destination[4] = {0};
        memcpy(destination, v + at, octets);
        at += octets;
        table[i] = (struct n_net_route){
            .destination = get32(destination),
            .mask = width ? 0xffffffffu << (32 - width) : 0,
            .gateway = get32(v + at),
            .adapter = adapter,
            .metric = adapter,
        };
        at += 4;
    }
    r->classless = table;
    r->classless_count = count;
}

static bool dhcp_parse(const struct dhcp *d, const uint8_t *p, size_t len, struct dhcp_reply *r) {
    static const uint8_t magic[4] = {99, 130, 83, 99};
    memset(r, 0, sizeof *r);
    if (len < DHCP_FIXED_BYTES + 4 || p[0] != 2 || get32(p + 4) != d->xid || memcmp(p + 28, d->link->mac, 6) ||
        memcmp(p + DHCP_FIXED_BYTES, magic, 4))
        return false;
    r->yiaddr = get32(p + 16);
    for (size_t i = DHCP_FIXED_BYTES + 4; i < len;) {
        uint8_t option = p[i];
        if (option == 255) break;
        if (option == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + p[i + 1] > len) break;
        size_t l = p[i + 1];
        const uint8_t *v = p + i + 2;
        switch (option) {
        case 1: if (l >= 4) r->mask = get32(v); break;
        case 3: if (l >= 4) r->router = get32(v); break;
        case 6: if (!r->dns) r->dns = address_list(v, l, &r->dns_count); break;
        case 42: if (!r->ntp) r->ntp = address_list(v, l, &r->ntp_count); break;
        case 51: if (l >= 4) r->lease = get32(v); break;
        case 53: if (l >= 1) r->type = v[0]; break;
        case 54: if (l >= 4) r->server = get32(v); break;
        case 58: if (l >= 4) r->t1 = get32(v); break;
        case 59: if (l >= 4) r->t2 = get32(v); break;
        case 121: if (!r->classless) classless_routes(r, v, l, d->link->adapter); break;
        default: break;
        }
        i += 2 + l;
    }
    return true;
}

static n_result dhcp_send(const struct dhcp *d, int type, uint32_t ciaddr, uint32_t requested, uint32_t server, uint32_t to) {
    uint8_t b[DHCP_MESSAGE_MIN] = {0};
    const uint8_t *mac = d->link->mac;
    b[0] = 1; /* BOOTREQUEST */
    b[1] = 1; /* Ethernet */
    b[2] = 6;
    put32(b + 4, d->xid);
    b[10] = ciaddr ? 0 : 0x80; /* 住所が無い間は broadcast で答えてもらう */
    put32(b + 12, ciaddr);
    memcpy(b + 28, mac, 6);
    static const uint8_t magic[4] = {99, 130, 83, 99};
    memcpy(b + DHCP_FIXED_BYTES, magic, 4);
    size_t o = DHCP_FIXED_BYTES + 4;
    b[o++] = 53;
    b[o++] = 1;
    b[o++] = (uint8_t)type;
    b[o++] = 61; /* client id */
    b[o++] = 7;
    b[o++] = 1;
    memcpy(b + o, mac, 6);
    o += 6;
    if (requested) {
        b[o++] = 50;
        b[o++] = 4;
        put32(b + o, requested);
        o += 4;
    }
    if (server) {
        b[o++] = 54;
        b[o++] = 4;
        put32(b + o, server);
        o += 4;
    }
    b[o++] = 12; /* host name */
    b[o++] = 8;
    memcpy(b + o, "nocturne", 8);
    o += 8;
    static const uint8_t wanted[] = {1, 3, 6, 42, 51, 54, 58, 59, 121};
    b[o++] = 55;
    b[o++] = sizeof wanted;
    memcpy(b + o, wanted, sizeof wanted);
    o += sizeof wanted;
    b[o++] = 255;
    return udp_sendto(d->socket, to, DHCP_SERVER_PORT, b, sizeof b);
}

/* until まで、want の種類の答えを待つ。1 は答え（r は呼ぶ側が放す）、0 は期限、負は止められた。 */
static n_result dhcp_wait(const struct dhcp *d, uint64_t until, unsigned want, struct dhcp_reply *r) {
    for (;;) {
        uint8_t message[DHCP_MESSAGE_MAX];
        n_result n = udp_recvfrom(d->socket, message, sizeof message, until, NULL);
        if (n == -N_ERR_NOT_READY) return 0;
        if (n < 0) return n;
        if (!dhcp_parse(d, message, (size_t)n, r)) continue;
        if (r->type > 0 && r->type < 32 && (want & 1u << r->type)) return 1;
        dhcp_reply_free(r);
    }
}

/* 送って待つ。doubling は RFC 2131 4.1 の倍々で ±1 秒。そうでなければ 4.4.5 の残りの半分（下限は 60 秒）。 */
static n_result dhcp_ask(const struct dhcp *d, int type, uint32_t ciaddr, uint32_t requested, uint32_t server, uint32_t to, uint64_t until,
                         bool doubling, unsigned want, struct dhcp_reply *r) {
    uint64_t delay = 4 * NS_PER_S;
    for (;;) {
        uint64_t now = n_clock_monotonic();
        if (now >= until) return 0;
        n_result sent = dhcp_send(d, type, ciaddr, requested, server, to);
        if (sent == -N_ERR_CANCELLED || sent == -N_ERR_STOPPED) return sent;
        uint64_t wait;
        if (doubling) {
            wait = delay - NS_PER_S + random32() % (2 * NS_PER_S);
            if (delay < DHCP_RETRY_MAX) delay *= 2;
        } else {
            wait = (until - now) / 2;
            if (wait < DHCP_LEASE_RETRY_MIN) wait = DHCP_LEASE_RETRY_MIN;
        }
        uint64_t next = until - now < wait ? until : now + wait;
        n_result got = dhcp_wait(d, next, want, r);
        if (got) return got;
    }
}

static uint32_t mask_of(uint32_t ip) { /* option 1 が無いときは住所の級（RFC 791）のマスク */
    return ip >> 31 == 0 ? 0xff000000u : ip >> 30 == 2 ? 0xffff0000u : 0xffffff00u;
}

static n_result dhcp_bind(const struct dhcp *d, struct dhcp_reply *a) {
    struct link *l = d->link;
    uint32_t mask = a->mask ? a->mask : mask_of(a->yiaddr);
    n_result r = n_net_address_set(network_cap, l->adapter, a->yiaddr, mask);
    if (r < 0) {
        printf("network: adapter %u: the address was refused (%s)\n", (unsigned)l->adapter, n_result_text(r));
        return r;
    }
    uint32_t total = a->dns_count + a->ntp_count;
    uint32_t *addresses = total ? malloc(total * sizeof *addresses) : NULL;
    uint32_t dns = addresses ? a->dns_count : 0, ntp = addresses ? a->ntp_count : 0;
    if (dns) memcpy(addresses, a->dns, dns * sizeof *addresses);
    if (ntp) memcpy(addresses + dns, a->ntp, ntp * sizeof *addresses);
    size_t route_count = a->classless_count;
    struct n_net_route *routes = a->classless; /* RFC 3442：あれば option 3 を見ない */
    a->classless = NULL;
    uint32_t gateway = a->router;
    if (routes) {
        gateway = 0;
        for (size_t i = 0; i < route_count; i++)
            if (!routes[i].mask) gateway = routes[i].gateway;
    } else {
        routes = default_route(l, gateway, &route_count);
    }
    uint64_t end = a->lease == DHCP_INFINITE ? N_CLOCK_NEVER : n_clock_after((uint64_t)a->lease * NS_PER_S);
    link_config(l, N_LINK_LEASED, gateway, end, addresses, dns, ntp, routes, route_count);
    char ip[16], via[16];
    printf("network: adapter %u: %s/%d gateway %s DHCP %u seconds\n", (unsigned)l->adapter, ip_format(a->yiaddr, ip), __builtin_popcount(mask),
           ip_format(gateway, via), (unsigned)a->lease);
    return 0;
}

/* 住所を失った。 */
static void dhcp_unbind(const struct dhcp *d) {
    n_net_address_set(network_cap, d->link->adapter, 0, 0);
    link_config(d->link, N_LINK_SEARCHING, 0, 0, NULL, 0, 0, NULL, 0);
}

/* 借りた住所を保つ。止められたら負、失ったら 0。 */
static n_result dhcp_keep(struct dhcp *d, struct dhcp_reply *a) {
    for (;;) {
        uint32_t ip = a->yiaddr, server = a->server, lease = a->lease;
        if (lease == DHCP_INFINITE) { /* 期限なし：止められるまで眠る */
            struct dhcp_reply none;
            n_result got;
            while ((got = dhcp_wait(d, N_CLOCK_NEVER, 0, &none)) >= 0) {}
            return got;
        }
        uint32_t t1 = a->t1, t2 = a->t2; /* RFC 2131 4.4.5：既定は 0.5 と 0.875 */
        if (!t2 || t2 >= lease) t2 = (uint32_t)((uint64_t)lease * 7 / 8);
        if (!t1 || t1 >= t2) t1 = lease / 2;
        uint64_t start = n_clock_monotonic();
        uint64_t renew = start + t1 * NS_PER_S, rebind = start + t2 * NS_PER_S, end = start + lease * NS_PER_S;
        struct dhcp_reply none, next;
        n_result got = dhcp_wait(d, renew, 0, &none);
        if (got < 0) return got;
        d->xid = random32();
        got = dhcp_ask(d, DHCP_REQUEST, ip, 0, 0, server ? server : BROADCAST, rebind, false, 1u << DHCP_ACK | 1u << DHCP_NAK, &next);
        if (got == 0) got = dhcp_ask(d, DHCP_REQUEST, ip, 0, 0, BROADCAST, end, false, 1u << DHCP_ACK | 1u << DHCP_NAK, &next);
        if (got < 0) return got;
        if (got == 0) return 0;
        if (next.type != DHCP_ACK || !next.lease || next.yiaddr != ip) {
            dhcp_reply_free(&next);
            return 0;
        }
        if (!next.server) next.server = server;
        if (dhcp_bind(d, &next) < 0) {
            dhcp_reply_free(&next);
            return 0;
        }
        dhcp_reply_free(a);
        *a = next;
    }
}

struct dhcp_start {
    struct link *link;
    n_handle stop;
    uint32_t requested; /* 前から持っている住所（RFC 2131 の INIT-REBOOT）。0 は無し。 */
};

static void dhcp_thread(void *arg) {
    struct dhcp_start start = *(struct dhcp_start *)arg;
    free(arg);
    struct link *l = start.link;
    net_set_setup_cancel_channel(start.stop);
    struct dhcp d = {.link = l, .socket = udp_socket_on(network_cap, l->adapter, DHCP_CLIENT_PORT)};
    if (d.socket < 0) {
        printf("network: adapter %u: no DHCP port (%s)\n", (unsigned)l->adapter, n_result_text(d.socket));
        n_handle_close(start.stop);
        return;
    }
    uint32_t requested = start.requested;
    bool rest = false;
    for (;;) {
        n_result got;
        struct dhcp_reply offer, ack;
        memset(&ack, 0, sizeof ack);
        d.xid = random32();
        if (requested) {
            got = dhcp_ask(&d, DHCP_REQUEST, 0, requested, 0, BROADCAST, n_clock_after(DHCP_RETRY_MAX), true, 1u << DHCP_ACK | 1u << DHCP_NAK, &ack);
            if (got == 0) ack.type = DHCP_NAK;
        } else {
            if (rest) {
                got = dhcp_wait(&d, n_clock_after(DHCP_REST), 0, &offer);
                if (got < 0) break;
            }
            got = dhcp_ask(&d, DHCP_DISCOVER, 0, 0, 0, BROADCAST, N_CLOCK_NEVER, true, 1u << DHCP_OFFER, &offer);
            if (got < 0) break;
            uint32_t server = offer.server;
            requested = offer.yiaddr;
            dhcp_reply_free(&offer);
            got = dhcp_ask(&d, DHCP_REQUEST, 0, requested, server, BROADCAST, n_clock_after(DHCP_RETRY_MAX), true, 1u << DHCP_ACK | 1u << DHCP_NAK, &ack);
            if (got == 0) ack.type = DHCP_NAK;
            if (got > 0 && !ack.server) ack.server = server;
        }
        if (got < 0) break;
        if (ack.type == DHCP_ACK && ack.lease && dhcp_bind(&d, &ack) >= 0) {
            got = dhcp_keep(&d, &ack);
            if (got < 0) {
                dhcp_reply_free(&ack);
                break;
            }
        }
        dhcp_reply_free(&ack);
        dhcp_unbind(&d);
        requested = 0;
        rest = true;
    }
    udp_close(d.socket);
    n_handle_close(start.stop);
}

/* 主のループ。設定のとおりに adapter を動かす。 */
static void link_stop(struct link *l) {
    if (l->thread < 0) return;
    n_handle_close(l->stop);
    thread_join(l->thread);
    l->thread = -1;
}

static void link_start(struct link *l, uint32_t held) {
    char ip[16], via[16];
    if (l->fixed.ip) {
        n_result r = n_net_address_set(network_cap, l->adapter, l->fixed.ip, l->fixed.mask);
        if (r < 0) {
            printf("network: adapter %u: the fixed address was refused (%s)\n", (unsigned)l->adapter, n_result_text(r));
            link_config(l, N_LINK_SEARCHING, 0, 0, NULL, 0, 0, NULL, 0);
            return;
        }
        size_t count;
        struct n_net_route *routes = default_route(l, l->fixed.gateway, &count);
        link_config(l, N_LINK_FIXED, l->fixed.gateway, 0, NULL, 0, 0, routes, count);
        printf("network: adapter %u: %s/%d gateway %s fixed\n", (unsigned)l->adapter, ip_format(l->fixed.ip, ip),
               __builtin_popcount(l->fixed.mask), ip_format(l->fixed.gateway, via));
        return;
    }
    if (held) { /* 起こし直された：答えが来るまで今の住所と経路で動く */
        mtx_lock(&links_lock);
        l->state = N_LINK_SEARCHING;
        mtx_unlock(&links_lock);
    } else {
        link_config(l, N_LINK_SEARCHING, 0, 0, NULL, 0, 0, NULL, 0);
    }
    n_handle stop[2];
    struct dhcp_start *start = malloc(sizeof *start);
    if (!start || n_channel_pair(stop) < 0) {
        free(start);
        printf("network: adapter %u: no room for DHCP\n", (unsigned)l->adapter);
        return;
    }
    *start = (struct dhcp_start){.link = l, .stop = stop[1], .requested = held};
    n_result tid = thread_create(dhcp_thread, start);
    if (tid < 0) {
        printf("network: adapter %u: no thread for DHCP (%s)\n", (unsigned)l->adapter, n_result_text(tid));
        n_handle_close(stop[0]);
        n_handle_close(stop[1]);
        free(start);
        return;
    }
    l->stop = stop[0];
    l->thread = (int)tid;
}

static void link_read_fixed(struct link *l) {
    struct n_setting_id id = n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_FIXED_ADDRESS);
    id.device = l->device;
    struct n_setting_ipv4 fixed = {0};
    if (n_setting_get(id, &fixed, sizeof fixed) != sizeof fixed) memset(&fixed, 0, sizeof fixed);
    l->fixed = fixed;
}

/* カーネルの一覧に合わせる。 */
static void links_refresh(void) {
    n_result count = n_net_adapters(NULL, 0);
    if (count <= 0) return;
    struct n_net_adapter *list = malloc((size_t)count * sizeof *list);
    if (!list) return;
    count = n_net_adapters(list, (size_t)count);
    for (n_result i = 0; i < count; i++) {
        const struct n_net_adapter *a = &list[i];
        struct link *l = links;
        while (l && l->adapter != a->id) l = l->next;
        if (!l) {
            if (!(l = calloc(1, sizeof *l))) continue;
            l->adapter = a->id;
            memcpy(l->mac, a->mac, 6);
            char text[18];
            snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);
            l->device = n_setting_device(text);
            l->thread = -1;
            mtx_lock(&links_lock);
            *links_end = l;
            links_end = &l->next;
            mtx_unlock(&links_lock);
            link_read_fixed(l);
        }
        if (a->present && !l->present) {
            l->present = true;
            link_start(l, a->ip);
        } else if (!a->present && l->present) {
            l->present = false;
            link_stop(l);
            link_config(l, N_LINK_ABSENT, 0, 0, NULL, 0, 0, NULL, 0);
        }
    }
    free(list);
}

static void link_fixed_changed(struct link *l) {
    struct n_setting_ipv4 before = l->fixed;
    link_read_fixed(l);
    if (!memcmp(&before, &l->fixed, sizeof before) || !l->present) return;
    link_stop(l);
    if (before.ip && !l->fixed.ip) n_net_address_set(network_cap, l->adapter, 0, 0);
    link_start(l, 0);
}

/* ---- 名前引き ---- */

#define DNS_PORT 53
/* 再送の回数と間隔。RFC 1035 4.2.1 は再送を求め、値は決めない。答えの無いサーバーの列一周あたり
   2 秒で次へ進み、三周で呼ぶ側へ返す。 */
#define DNS_ATTEMPTS 3
#define DNS_ATTEMPT (2 * NS_PER_S)
#define DNS_UDP_BYTES 1472u           /* Ethernet の 1500 から IP と UDP の 28。スタックは断片を受けない。 */

struct lookup {
    struct name_entry *entry; /* 主のループ。待つ者が全部消えたら NULL。 */
    n_handle stop, stop_peer;
    n_handle done;
    struct n_sockaddr *servers;
    size_t server_count;
    /* スレッドが埋める */
    uint32_t status, ttl, count;
    n_result result;
    uint32_t *ips;
    char name[];
};

static n_result dns_failure(struct lookup *l, uint32_t status) {
    l->status = status;
    l->count = l->ttl = 0;
    switch (status) {
    case N_DNS_NXDOMAIN: case N_DNS_NODATA: return l->result = -N_ERR_NOT_FOUND;
    case N_DNS_SERVFAIL: return l->result = -N_ERR_NOT_READY;
    case N_DNS_REFUSED: return l->result = -N_ERR_CONNECTION_REFUSED;
    case N_DNS_TIMEOUT: return l->result = -N_ERR_TIMEOUT;
    case N_DNS_CANCELLED: return l->result = -N_ERR_CANCELLED;
    case N_DNS_UNREACHABLE: return l->result = -N_ERR_UNREACHABLE;
    case N_DNS_RESOURCE: return l->result = -N_ERR_NO_MEMORY;
    default: return l->result = -N_ERR_PROTOCOL;
    }
}
static n_result dns_error(struct lookup *l, n_result error) {
    return dns_failure(l, error == -N_ERR_NO_MEMORY ? N_DNS_RESOURCE
                        : error == -N_ERR_TIMEOUT || error == -N_ERR_NOT_READY ? N_DNS_TIMEOUT
                        : error == -N_ERR_STOPPED || error == -N_ERR_CANCELLED ? N_DNS_CANCELLED
                        : error == -N_ERR_PROTOCOL ? N_DNS_FORMAT : N_DNS_UNREACHABLE);
}

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* 答えを読む。各持ち主を一度ハッシュに入れ、CNAME の鎖は一致する持ち主だけをたどる：関係のない A
   の記録は問いを曲げられない。 */
static n_result dns_parse_reply(struct lookup *l, const uint8_t *p, size_t len) {
    if (len < 12 || !(p[2] & 0x80) || (p[2] & 0x7a) || p[4] || p[5] != 1) return dns_failure(l, N_DNS_FORMAT);
    char decoded[DNS_NAME_TEXT_BYTES + 1];
    size_t offset = dns_name_decode(p, len, 12, decoded);
    if (!offset || offset + 4 > len || strcmp(decoded, l->name) || p[offset] || p[offset + 1] != 1 || p[offset + 2] || p[offset + 3] != 1)
        return dns_failure(l, N_DNS_FORMAT);
    offset += 4;
    switch (p[3] & 15) {
    case 0: break;
    case 2: return dns_failure(l, N_DNS_SERVFAIL);
    case 3: return dns_failure(l, N_DNS_NXDOMAIN);
    case 5: return dns_failure(l, N_DNS_REFUSED);
    default: return dns_failure(l, N_DNS_FORMAT);
    }
    size_t an = (size_t)p[6] << 8 | p[7];
    if (an > (len - offset) / 11) return dns_failure(l, N_DNS_FORMAT);
    if (!an) return dns_failure(l, N_DNS_NODATA);
    struct dns_record {
        char owner[DNS_NAME_TEXT_BYTES + 1], target[DNS_NAME_TEXT_BYTES + 1];
        uint32_t ip, ttl, next;
        uint16_t type;
    };
    struct dns_record *records = malloc(an * sizeof *records);
    size_t buckets = 1;
    while (buckets < an * 2 + 1) buckets *= 2;
    uint32_t *heads = calloc(buckets, sizeof *heads);
    uint32_t *ips = malloc(an * sizeof *ips);
    n_result result;
    if (!records || !heads || !ips) {
        result = dns_failure(l, N_DNS_RESOURCE);
        goto done;
    }
    size_t count = 0;
    for (size_t i = 0; i < an; i++) {
        offset = dns_name_decode(p, len, offset, decoded);
        if (!offset || offset + 10 > len) goto malformed;
        unsigned type = (unsigned)p[offset] << 8 | p[offset + 1], cls = (unsigned)p[offset + 2] << 8 | p[offset + 3];
        uint32_t ttl = get32(p + offset + 4);
        size_t bytes = (size_t)p[offset + 8] << 8 | p[offset + 9];
        offset += 10;
        if (bytes > len - offset) goto malformed;
        if (cls == 1 && (type == 1 || type == 5)) {
            struct dns_record *rr = &records[count];
            strcpy(rr->owner, decoded);
            rr->type = (uint16_t)type;
            rr->ttl = ttl > 0x7fffffffu ? 0 : ttl; /* RFC 2181 8：上の位が立つ TTL は 0 */
            if (type == 1) {
                if (bytes != 4) goto malformed;
                rr->ip = get32(p + offset);
            } else if (dns_name_decode(p, len, offset, rr->target) != offset + bytes) {
                goto malformed;
            }
            uint32_t bucket = name_hash(rr->owner) & (uint32_t)(buckets - 1);
            rr->next = heads[bucket];
            heads[bucket] = (uint32_t)++count;
        }
        offset += bytes;
    }
    strcpy(decoded, l->name);
    uint32_t ttl = UINT32_MAX, found = 0;
    for (size_t steps = 0; steps <= count; steps++) {
        const char *target = NULL;
        for (uint32_t index = heads[name_hash(decoded) & (uint32_t)(buckets - 1)]; index; index = records[index - 1].next) {
            struct dns_record *rr = &records[index - 1];
            if (strcmp(rr->owner, decoded)) continue;
            if (rr->ttl < ttl) ttl = rr->ttl;
            if (rr->type == 1) {
                bool duplicate = false;
                for (uint32_t i = 0; i < found; i++)
                    if (ips[i] == rr->ip) duplicate = true;
                if (!duplicate) ips[found++] = rr->ip;
            } else {
                if (target && strcmp(target, rr->target)) goto malformed;
                target = rr->target;
            }
        }
        if (found) {
            if (target) goto malformed; /* CNAME は A と並ばない */
            l->ips = ips;
            ips = NULL;
            l->count = found;
            l->ttl = ttl;
            l->status = N_DNS_OK;
            result = l->result = 0;
            goto done;
        }
        if (!target) {
            result = dns_failure(l, N_DNS_NODATA);
            goto done;
        }
        strcpy(decoded, target);
    }
malformed:
    result = dns_failure(l, N_DNS_FORMAT);
done:
    free(records);
    free(heads);
    free(ips);
    return result;
}

/* RFC 1035 4.2.2：長さの 2 バイトが前につく。 */
static n_result dns_tcp_exchange(const struct n_sockaddr *server, uint64_t until, const uint8_t *query, size_t query_len, uint8_t **reply, size_t *reply_len) {
    n_handle s = tcp_connect(server->ip, server->port, until);
    if (s < 0) return s;
    uint8_t prefix[2] = {(uint8_t)(query_len >> 8), (uint8_t)query_len};
    uint8_t *response = NULL;
    n_result r = tcp_send_until(s, prefix, 2, until);
    if (r == 2) r = tcp_send_until(s, query, query_len, until);
    if (r < 0) goto close;
    if (r != (n_result)query_len) {
        r = -N_ERR_TIMEOUT;
        goto close;
    }
    size_t done = 0;
    while (done < 2) {
        r = tcp_recv(s, prefix + done, 2 - done, until);
        if (r <= 0) {
            r = r < 0 ? r : -N_ERR_PROTOCOL;
            goto close;
        }
        done += (size_t)r;
    }
    size_t length = (size_t)prefix[0] << 8 | prefix[1];
    if (length < 12) {
        r = -N_ERR_PROTOCOL;
        goto close;
    }
    if (!(response = malloc(length))) {
        r = -N_ERR_NO_MEMORY;
        goto close;
    }
    done = 0;
    while (done < length) {
        r = tcp_recv(s, response + done, length - done, until);
        if (r <= 0) {
            r = r < 0 ? r : -N_ERR_PROTOCOL;
            free(response);
            goto close;
        }
        done += (size_t)r;
    }
    *reply = response;
    *reply_len = length;
    r = 0;
close:
    tcp_close(s);
    return r;
}

static bool listed(const struct lookup *l, const struct n_sockaddr *from) {
    for (size_t i = 0; i < l->server_count; i++)
        if (l->servers[i].ip == from->ip && l->servers[i].port == from->port) return true;
    return false;
}

/* サーバーの列を順に、全体を DNS_ATTEMPTS 回。SERVFAIL と REFUSED は次のサーバーへ進む。 */
static void lookup_run(struct lookup *l) {
    uint8_t query[12 + DNS_NAME_WIRE_BYTES + 4];
    memset(query, 0, 12);
    uint32_t id = random32();
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)id;
    query[2] = 1; /* RD */
    query[5] = 1;
    size_t bytes = 0;
    if (!dns_name_encode(l->name, strlen(l->name), query + 12, sizeof query - 16, &bytes)) {
        dns_failure(l, N_DNS_FORMAT);
        return;
    }
    size_t query_len = 12 + bytes;
    query[query_len++] = 0;
    query[query_len++] = 1; /* A */
    query[query_len++] = 0;
    query[query_len++] = 1; /* IN */
    n_handle socket = udp_socket(0);
    if (socket < 0) {
        dns_error(l, socket);
        return;
    }
    uint8_t *packet = malloc(DNS_UDP_BYTES);
    if (!packet) {
        udp_close(socket);
        dns_failure(l, N_DNS_RESOURCE);
        return;
    }
    uint32_t refused = N_DNS_OK;
    bool reached = false;
    for (unsigned attempt = 0; attempt < DNS_ATTEMPTS && refused == N_DNS_OK; attempt++) {
        for (size_t i = 0; i < l->server_count; i++) {
            n_result sent = udp_sendto(socket, l->servers[i].ip, l->servers[i].port, query, query_len);
            if (sent == -N_ERR_CANCELLED || sent == -N_ERR_STOPPED) {
                dns_error(l, sent);
                goto finished;
            }
            if (sent < 0) continue;
            reached = true;
            uint64_t until = n_clock_after(DNS_ATTEMPT);
            for (;;) {
                struct n_sockaddr from;
                n_result n = udp_recvfrom(socket, packet, DNS_UDP_BYTES, until, &from);
                if (n == -N_ERR_NOT_READY) break;
                if (n < 0) {
                    dns_error(l, n);
                    goto finished;
                }
                if (!listed(l, &from) || n < 12 || !(packet[2] & 0x80) || packet[0] != query[0] || packet[1] != query[1]) continue;
                if (packet[2] & 2) { /* TC */
                    uint8_t *whole = NULL;
                    size_t length = 0;
                    n_result r = dns_tcp_exchange(&from, until, query, query_len, &whole, &length);
                    if (r < 0) {
                        dns_error(l, r);
                        goto finished;
                    }
                    if (length < 12 || whole[0] != query[0] || whole[1] != query[1] || (whole[2] & 2)) {
                        free(whole);
                        dns_failure(l, N_DNS_FORMAT);
                        goto finished;
                    }
                    dns_parse_reply(l, whole, length);
                    free(whole);
                } else {
                    dns_parse_reply(l, packet, (size_t)n);
                }
                if (l->status != N_DNS_SERVFAIL && l->status != N_DNS_REFUSED) goto finished;
                refused = l->status;
                break;
            }
        }
    }
    if (refused != N_DNS_OK) dns_failure(l, refused);
    else dns_failure(l, reached ? N_DNS_TIMEOUT : N_DNS_UNREACHABLE);
finished:
    free(packet);
    udp_close(socket);
}

static void lookup_thread(void *arg) {
    struct lookup *l = arg;
    net_set_setup_cancel_channel(l->stop_peer);
    lookup_run(l);
    net_set_setup_cancel_channel(-1);
    n_handle_close(l->stop_peer);
    n_channel_send(l->done, &l, sizeof l, -1); /* ここから l は主のループのもの */
}

/* ---- 接続と、名前の表 ---- */

enum conn_state { CONN_RECEIVING, CONN_WAITING, CONN_SENDING };
struct conn {
    n_handle channel;
    enum conn_state state;
    struct name_entry *entry;       /* CONN_WAITING：待っている名前 */
    struct conn *next, **prev;      /* その名前の待つ者の列 */
};

/* 名前ごとの記録。答えを持つものが cache で、流れているものは lookup を持つ。 */
struct name_entry {
    struct name_entry *next;
    uint32_t hash;
    bool linked;            /* 表にある。設定が変わった後の流れている問いは表から外れている。 */
    struct n_sockaddr resolver;
    struct lookup *lookup;
    struct conn *waiters;
    uint64_t expires;
    uint32_t *ips, count;
    char name[];
};
static struct name_entry **table;
static size_t table_buckets, table_count, table_sweep_at;

/* 機械の resolver。主のループだけ。 */
static struct n_sockaddr chosen_resolver;
static uint64_t seen_dns_generation;
static n_handle done_in, done_out;

static void table_unlink(struct name_entry *e) {
    struct name_entry **at = &table[e->hash & (table_buckets - 1)];
    while (*at != e) at = &(*at)->next;
    *at = e->next;
    e->linked = false;
    table_count--;
}

static void entry_free(struct name_entry *e) {
    free(e->ips);
    free(e);
}

static void table_grow(void) {
    size_t buckets = table_buckets ? table_buckets * 2 : 1;
    struct name_entry **grown = calloc(buckets, sizeof *grown);
    if (!grown) return; /* 列が長くなるだけ */
    for (size_t i = 0; i < table_buckets; i++) {
        for (struct name_entry *e = table[i], *next; e; e = next) {
            next = e->next;
            e->next = grown[e->hash & (buckets - 1)];
            grown[e->hash & (buckets - 1)] = e;
        }
    }
    free(table);
    table = grown;
    table_buckets = buckets;
}

/* 答えを持つ記録を捨てる。expired_only なら期限の切れたものだけ。流れている問いは、DNS の設定が変わった
   ときは表から外し、新しい要求が新しいサーバーへ行くようにする。 */
static void table_clean(bool expired_only, bool detach_flights) {
    uint64_t now = n_clock_monotonic();
    for (size_t i = 0; i < table_buckets; i++) {
        for (struct name_entry **at = &table[i]; *at;) {
            struct name_entry *e = *at;
            bool answered = !e->lookup;
            if ((answered && (!expired_only || e->expires <= now)) || (!answered && detach_flights)) {
                *at = e->next;
                e->linked = false;
                table_count--;
                if (answered) entry_free(e);
                continue;
            }
            at = &e->next;
        }
    }
    table_sweep_at = table_count * 2 + 1;
}

static struct name_entry *table_find(const char *name, uint32_t hash, const struct n_sockaddr *resolver) {
    if (!table_buckets) return NULL;
    for (struct name_entry *e = table[hash & (table_buckets - 1)]; e; e = e->next)
        if (e->hash == hash && e->resolver.ip == resolver->ip && e->resolver.port == resolver->port && !strcmp(e->name, name)) return e;
    return NULL;
}

static void conn_watch(struct conn *c, uint32_t interest) {
    n_events_unwatch(queue, (uint64_t)(uintptr_t)c);
    n_events_watch(queue, c->channel, interest | N_EVENT_CLOSED, (uint64_t)(uintptr_t)c);
}

static void conn_sent(struct conn *c) {
    c->state = CONN_SENDING;
    conn_watch(c, N_EVENT_SENDABLE);
}

static void reply_resolved(struct conn *c, n_result result, uint32_t status, const uint32_t *ips, uint32_t count, uint32_t ttl) {
    struct n_network_reply head = {.result = result, .status = status, .count = count, .ttl = ttl};
    size_t bytes = sizeof head + (size_t)count * sizeof *ips;
    uint8_t *message = malloc(bytes);
    if (message) {
        memcpy(message, &head, sizeof head);
        if (count) memcpy(message + sizeof head, ips, (size_t)count * sizeof *ips);
        n_channel_send(c->channel, message, bytes, -1);
        free(message);
    } else {
        head = (struct n_network_reply){.result = -N_ERR_NO_MEMORY, .status = N_DNS_RESOURCE};
        n_channel_send(c->channel, &head, sizeof head, -1);
    }
    conn_sent(c);
}

/* サーバーの列。resolver の指定、設定、DHCP の順。 */
static struct n_sockaddr *servers_for(const struct n_sockaddr *resolver, size_t *count) {
    *count = 0;
    struct n_sockaddr pick = resolver->ip ? *resolver : chosen_resolver;
    if (pick.ip) {
        if (!pick.port) pick.port = resolver->ip ? 0 : DNS_PORT;
        if (!pick.port) return NULL;
        struct n_sockaddr *one = malloc(sizeof *one);
        if (one) {
            *one = pick;
            *count = 1;
        }
        return one;
    }
    mtx_lock(&links_lock);
    size_t total = 0;
    for (struct link *l = links; l; l = l->next) total += l->dns_count;
    struct n_sockaddr *list = total ? malloc(total * sizeof *list) : NULL;
    if (list) {
        for (struct link *l = links; l; l = l->next)
            for (uint32_t i = 0; i < l->dns_count; i++) list[(*count)++] = (struct n_sockaddr){.ip = l->addresses[i], .port = DNS_PORT};
    }
    mtx_unlock(&links_lock);
    return list;
}

static void waiter_add(struct name_entry *e, struct conn *c) {
    c->state = CONN_WAITING;
    c->entry = e;
    c->next = e->waiters;
    c->prev = &e->waiters;
    if (c->next) c->next->prev = &c->next;
    e->waiters = c;
    conn_watch(c, 0);
}

static struct lookup *lookup_start(const char *name, const struct n_sockaddr *resolver) {
    size_t server_count;
    struct n_sockaddr *servers = servers_for(resolver, &server_count);
    if (!server_count) return NULL;
    size_t length = strlen(name);
    struct lookup *l = calloc(1, sizeof *l + length + 1);
    n_handle stop[2];
    if (!l || n_channel_pair(stop) < 0) {
        free(l);
        free(servers);
        return NULL;
    }
    memcpy(l->name, name, length + 1);
    l->servers = servers;
    l->server_count = server_count;
    l->stop = stop[0];
    l->stop_peer = stop[1];
    l->done = done_out;
    n_result tid = thread_create(lookup_thread, l);
    if (tid < 0) {
        n_handle_close(stop[0]);
        n_handle_close(stop[1]);
        free(servers);
        free(l);
        return NULL;
    }
    thread_detach((int)tid);
    return l;
}

static void lookup_free(struct lookup *l) {
    free(l->servers);
    free(l->ips);
    free(l);
}

static void handle_resolve(struct conn *c, const struct n_network_request *q, const char *source) {
    char name[DNS_NAME_TEXT_BYTES + 1];
    size_t length = q->name_length;
    if (length > DNS_NAME_TEXT_BYTES) {
        reply_resolved(c, -N_ERR_NAME_TOO_LONG, N_DNS_FORMAT, NULL, 0, 0);
        return;
    }
    uint8_t wire[DNS_NAME_WIRE_BYTES];
    size_t encoded;
    if (!dns_name_encode(source, length, wire, sizeof wire, &encoded)) {
        reply_resolved(c, -N_ERR_PROTOCOL, N_DNS_FORMAT, NULL, 0, 0);
        return;
    }
    for (size_t i = 0; i < length; i++) name[i] = source[i] >= 'A' && source[i] <= 'Z' ? (char)(source[i] + ('a' - 'A')) : source[i];
    if (length && name[length - 1] == '.') length--;
    name[length] = 0;
    uint32_t hash = name_hash(name);
    struct name_entry *e = table_find(name, hash, &q->resolver);
    if (e && !e->lookup) {
        uint64_t now = n_clock_monotonic();
        if (e->expires > now) {
            uint64_t left = e->expires - now;
            uint64_t seconds = left / NS_PER_S + (left % NS_PER_S != 0); /* 切り上げ */
            reply_resolved(c, 0, N_DNS_OK, e->ips, e->count, seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds);
            return;
        }
        table_unlink(e);
        entry_free(e);
        e = NULL;
    }
    if (e) {
        waiter_add(e, c);
        return;
    }
    if (table_count >= table_sweep_at) table_clean(true, false);
    if (table_count >= table_buckets) table_grow();
    e = table_buckets ? calloc(1, sizeof *e + length + 1) : NULL;
    if (!e) {
        table_clean(false, false); /* 持っている答えを手放してやり直す */
        e = table_buckets ? calloc(1, sizeof *e + length + 1) : NULL;
    }
    if (!e) {
        reply_resolved(c, -N_ERR_NO_MEMORY, N_DNS_RESOURCE, NULL, 0, 0);
        return;
    }
    struct lookup *l = lookup_start(name, &q->resolver);
    if (!l) {
        free(e);
        reply_resolved(c, -N_ERR_UNREACHABLE, N_DNS_UNREACHABLE, NULL, 0, 0);
        return;
    }
    l->entry = e;
    e->lookup = l;
    e->hash = hash;
    e->resolver = q->resolver;
    memcpy(e->name, name, length + 1);
    size_t bucket = hash & (table_buckets - 1);
    e->next = table[bucket];
    table[bucket] = e;
    e->linked = true;
    table_count++;
    waiter_add(e, c);
}

/* 問いの答えが主のループへ戻った。 */
static void lookup_done(void) {
    struct lookup *l;
    if (n_channel_receive(done_in, &l, sizeof l, 0, NULL) != sizeof l) return;
    if (l->stop >= 0) n_handle_close(l->stop);
    struct name_entry *e = l->entry;
    if (e) {
        e->lookup = NULL;
        while (e->waiters) {
            struct conn *c = e->waiters;
            e->waiters = c->next;
            c->entry = NULL;
            reply_resolved(c, l->result, l->status, l->ips, l->count, l->ttl);
        }
        if (e->linked && !l->result && l->ttl) {
            e->ips = l->ips;
            l->ips = NULL;
            e->count = l->count;
            e->expires = n_clock_after((uint64_t)l->ttl * NS_PER_S);
        } else {
            if (e->linked) table_unlink(e);
            entry_free(e);
        }
    }
    lookup_free(l);
}

static void conn_free(struct conn *c) {
    struct name_entry *e = c->entry;
    if (e) {
        *c->prev = c->next;
        if (c->next) c->next->prev = c->prev;
        if (!e->waiters) { /* 誰も待たない：問いを止める */
            struct lookup *l = e->lookup;
            l->entry = NULL;
            n_handle_close(l->stop);
            l->stop = -1;
            if (e->linked) table_unlink(e);
            entry_free(e);
        }
    }
    n_handle_close(c->channel); /* 行列の項目も消える */
    free(c);
}

static void handle_links(struct conn *c) {
    mtx_lock(&links_lock);
    size_t bytes = sizeof(struct n_network_reply);
    uint32_t count = 0;
    for (struct link *l = links; l; l = l->next, count++)
        bytes += sizeof(struct n_network_link) + (size_t)(l->dns_count + l->ntp_count) * sizeof(uint32_t);
    uint8_t *message = malloc(bytes);
    if (!message) {
        mtx_unlock(&links_lock);
        struct n_network_reply head = {.result = -N_ERR_NO_MEMORY};
        n_channel_send(c->channel, &head, sizeof head, -1);
        conn_sent(c);
        return;
    }
    *(struct n_network_reply *)message = (struct n_network_reply){.count = count};
    uint8_t *at = message + sizeof(struct n_network_reply);
    uint64_t now = n_clock_monotonic();
    for (struct link *l = links; l; l = l->next) {
        uint64_t left = l->state == N_LINK_LEASED && l->lease_end > now ? (l->lease_end - now) / NS_PER_S : 0;
        struct n_network_link record = {
            .adapter = l->adapter,
            .state = l->state,
            .gateway = l->gateway,
            .lease_seconds = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left,
            .dns_count = l->dns_count,
            .ntp_count = l->ntp_count,
        };
        memcpy(at, &record, sizeof record);
        at += sizeof record;
        size_t addresses = (size_t)(l->dns_count + l->ntp_count) * sizeof(uint32_t);
        if (addresses) memcpy(at, l->addresses, addresses);
        at += addresses;
    }
    mtx_unlock(&links_lock);
    n_channel_send(c->channel, message, bytes, -1);
    free(message);
    conn_sent(c);
}

static void handle_resolvers(struct conn *c) {
    struct n_sockaddr none = {0};
    size_t count;
    struct n_sockaddr *list = servers_for(&none, &count);
    struct n_network_reply head = {.count = (uint32_t)count};
    size_t bytes = sizeof head + count * sizeof *list;
    uint8_t *message = malloc(bytes);
    if (message) {
        memcpy(message, &head, sizeof head);
        if (count) memcpy(message + sizeof head, list, count * sizeof *list);
        n_channel_send(c->channel, message, bytes, -1);
        free(message);
    } else {
        head = (struct n_network_reply){.result = -N_ERR_NO_MEMORY};
        n_channel_send(c->channel, &head, sizeof head, -1);
    }
    free(list);
    conn_sent(c);
}

static void conn_event(struct conn *c, uint32_t events) {
    if (events & N_EVENT_CLOSED) {
        conn_free(c);
        return;
    }
    if (c->state == CONN_SENDING) {
        c->state = CONN_RECEIVING;
        conn_watch(c, N_EVENT_RECEIVABLE);
        return;
    }
    if (c->state != CONN_RECEIVING) return;
    union {
        struct n_network_request request;
        uint8_t bytes[sizeof(struct n_network_request) + DNS_NAME_TEXT_BYTES + 1];
    } m;
    n_result size = n_channel_receive(c->channel, NULL, 0, N_CHANNEL_PEEK, NULL);
    if (size == -N_ERR_NOT_READY) return;
    if (size < (n_result)sizeof m.request || (size_t)size > sizeof m.bytes - 1 ||
        n_channel_receive(c->channel, &m, sizeof m, 0, NULL) != size) { /* 名前の長さの上限を超えるものは要求ではない */
        conn_free(c);
        return;
    }
    if (m.request.op == N_NETWORK_RESOLVE && m.request.name_length == (size_t)size - sizeof m.request) {
        m.bytes[size] = 0;
        handle_resolve(c, &m.request, (const char *)(&m.request + 1));
    } else if (m.request.op == N_NETWORK_LINKS) {
        handle_links(c);
    } else if (m.request.op == N_NETWORK_RESOLVERS) {
        handle_resolvers(c);
    } else {
        conn_free(c);
    }
}

/* ---- 主のループ ---- */

static n_handle settings_watch = -1;
enum { TOKEN_CONTROL = 1, TOKEN_WATCH, TOKEN_SETTINGS, TOKEN_DONE };

static void resolver_read(void) {
    struct n_sockaddr chosen = {0};
    if (n_setting_get(n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_DNS_SERVER), &chosen, sizeof chosen) != sizeof chosen) memset(&chosen, 0, sizeof chosen);
    if (chosen.ip == chosen_resolver.ip && chosen.port == chosen_resolver.port) return;
    chosen_resolver = chosen;
    table_clean(false, true);
}

static void settings_open(void) {
    settings_watch = n_setting_watch(N_DOMAIN_NETWORK, N_SETTING_ANY);
    if (settings_watch < 0) {
        printf("network: the settings cannot be watched (%s)\n", n_result_text(settings_watch));
        return;
    }
    n_events_watch(queue, settings_watch, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_SETTINGS);
}

static void settings_event(uint32_t events) {
    if (events & N_EVENT_CLOSED) { /* settings が起こし直された：開き直して読み直す */
        n_handle_close(settings_watch);
        settings_open();
        resolver_read();
        for (struct link *l = links; l; l = l->next) link_fixed_changed(l);
        return;
    }
    struct n_settings_changed changed;
    while (n_setting_watch_take(settings_watch, &changed) >= 0) {
        if (changed.id.key == N_NETWORK_DNS_SERVER) {
            resolver_read();
        } else if (changed.id.key == N_NETWORK_FIXED_ADDRESS) {
            for (struct link *l = links; l; l = l->next)
                if (l->device == changed.id.device) link_fixed_changed(l);
        }
    }
}

static void dns_config_check(void) {
    mtx_lock(&links_lock);
    uint64_t generation = dns_generation;
    mtx_unlock(&links_lock);
    if (generation == seen_dns_generation) return;
    seen_dns_generation = generation;
    if (!chosen_resolver.ip) table_clean(false, true);
}

int main(void) {
    network_cap = n_cap_find(N_CAP_NETWORK);
    if (network_cap < 0 || mtx_init(&links_lock, mtx_plain) != thrd_success) return 1;
    n_handle done[2], watch = n_net_watch();
    queue = n_events_create();
    if (watch < 0 || queue < 0 || n_channel_pair(done) < 0) return 1;
    done_in = done[0];
    done_out = done[1];
    n_events_watch(queue, N_SERVICE_HANDLE_CONTROL, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_CONTROL);
    n_events_watch(queue, watch, N_EVENT_RECEIVABLE, TOKEN_WATCH);
    n_events_watch(queue, done_in, N_EVENT_RECEIVABLE, TOKEN_DONE);
    settings_open();
    resolver_read();
    links_refresh();
    n_service_ready();
    for (;;) {
        struct n_event event;
        n_result got = n_events_wait(queue, &event, 1, N_CLOCK_NEVER);
        if (got < 0) return 1;
        if (got == 0) continue;
        switch (event.token) {
        case TOKEN_CONTROL: {
            n_handle channel = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, 0);
            if (channel == -N_ERR_CLOSED) return 0; /* 管理者が止める指示。借りた住所はカーネルに残る。 */
            if (channel < 0) break;
            struct conn *c = calloc(1, sizeof *c);
            if (!c) {
                n_handle_close(channel);
                break;
            }
            c->channel = channel;
            conn_watch(c, N_EVENT_RECEIVABLE);
            break;
        }
        case TOKEN_WATCH:
            n_watch_take(watch);
            links_refresh();
            dns_config_check();
            break;
        case TOKEN_SETTINGS:
            settings_event(event.events);
            break;
        case TOKEN_DONE:
            lookup_done();
            break;
        default:
            conn_event((struct conn *)(uintptr_t)event.token, event.events);
            break;
        }
    }
}
