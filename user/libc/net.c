/* Networking system call wrappers, address helpers and the lookup of names at the service network.
   Each returns its result or a negative n_result, and leaves errno alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne_services.h"
#include "nocturne_settings.h"

#define NET(n, a, b, c, d, e) __syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))

n_result n_net_adapters(struct n_net_adapter *out, size_t max) { return NET(SYS_NET_ADAPTERS, out, max, 0, 0, 0); }
n_result n_net_adapter_name(uint32_t adapter, char *out, size_t cap) { return NET(SYS_NET_ADAPTER_NAME, adapter, out, cap, 0, 0); }
n_result n_net_routes(struct n_net_route *out, size_t max) { return NET(SYS_NET_ROUTES, out, max, 0, 0, 0); }
n_handle n_net_watch(void) { return (n_handle)NET(SYS_NET_WATCH, 0, 0, 0, 0, 0); }
n_result n_net_address_set(n_handle cap, uint32_t adapter, uint32_t ip, uint32_t mask) { return NET(SYS_NET_ADDRESS_SET, cap, adapter, ip, mask, 0); }
n_result n_net_routes_set(n_handle cap, const struct n_net_route *routes, size_t count) { return NET(SYS_NET_ROUTES_SET, cap, routes, count, 0, 0); }
n_result udp_socket_on(n_handle cap, uint32_t adapter, uint16_t port) { return NET(SYS_UDP_SOCKET_ON, cap, adapter, port, 0, 0); }
n_result net_ping(uint32_t ip, int seq, uint64_t deadline) { return NET(SYS_NET_PING, ip, seq, deadline, 0, 0); }
n_result net_stats(struct n_netstats *stats) { return NET(SYS_NET_STATS, stats, 0, 0, 0, 0); }
n_result net_set_setup_cancel_channel(n_handle channel) {
    n_result r = NET(SYS_NET_SETUP_CANCEL_CHANNEL, channel, 0, 0, 0, 0);
    if (r >= 0) thread_local_set(N_TLS_NET_CANCEL, (void *)(intptr_t)(channel + 1));
    return r;
}

bool ip_parse(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (uint32_t)(*s++ - '0');
            if (n > 255) return false;
        }
        v = v << 8 | n;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *ip = v;
    return true;
}

char *ip_format(uint32_t ip, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

n_result udp_socket(uint16_t port) { return NET(SYS_UDP_SOCKET, port, 0, 0, 0, 0); }
n_result udp_sendto(n_handle s, uint32_t ip, uint16_t port, const void *buf, size_t len) {
    return NET(SYS_UDP_SEND, s, ip, port, buf, len);
}
n_result udp_recvfrom(n_handle s, void *buf, size_t len, uint64_t deadline, struct n_sockaddr *from) {
    return NET(SYS_UDP_RECV, s, buf, len, deadline, from);
}
n_result udp_close(n_handle s) { return NET(SYS_UDP_CLOSE, s, 0, 0, 0, 0); }
n_result tcp_connect(uint32_t ip, uint16_t port, uint64_t deadline) { return NET(SYS_TCP_CONNECT, ip, port, deadline, 0, 0); }
n_result tcp_send(n_handle s, const void *buf, size_t len) { return NET(SYS_TCP_SEND, s, buf, len, 0, 0); }
n_result tcp_send_until(n_handle s, const void *buf, size_t len, uint64_t deadline) {
    return NET(SYS_TCP_SEND_UNTIL, s, buf, len, deadline, 0);
}
n_result tcp_send_some(n_handle s, const void *buf, size_t len) {
    return NET(SYS_TCP_SEND_UNTIL, s, buf, len, N_CLOCK_NEVER, N_TCP_SEND_NONBLOCK);
}
n_result tcp_cancel(n_handle s) { return NET(SYS_TCP_CANCEL, s, 0, 0, 0, 0); }
n_result tcp_recv(n_handle s, void *buf, size_t len, uint64_t deadline) { return NET(SYS_TCP_RECV, s, buf, len, deadline, 0); }
n_result tcp_close(n_handle s) { return NET(SYS_TCP_CLOSE, s, 0, 0, 0, 0); }
n_result tcp_listen(uint32_t ip, uint16_t port, int backlog) { return NET(SYS_TCP_LISTEN, ip, port, backlog, 0, 0); }
n_result tcp_accept(n_handle listener, uint64_t deadline, struct n_sockaddr *remote) {
    return NET(SYS_TCP_ACCEPT, listener, deadline, remote, 0, 0);
}
n_result tcp_address(n_handle s, struct n_sockaddr *local, struct n_sockaddr *remote) {
    return NET(SYS_TCP_ADDRESS, s, local, remote, 0, 0);
}
n_result tcp_finish(n_handle s) { return NET(SYS_TCP_FINISH, s, 0, 0, 0, 0); }
n_result net_test(const struct n_netfault *set, struct n_tcpstats *stats) { return NET(SYS_NET_TEST, set, stats, 0, 0, 0); }

/* Is some adapter there, and does one have an address? The stack says when either changes. */
static bool any_address(bool *present) {
    n_result count = n_net_adapters(NULL, 0);
    *present = false;
    if (count <= 0) return false;
    struct n_net_adapter *list = malloc((size_t)count * sizeof *list);
    if (!list) return false;
    count = n_net_adapters(list, (size_t)count);
    bool up = false;
    for (n_result i = 0; i < count; i++) {
        *present = *present || list[i].present;
        up = up || list[i].ip;
    }
    free(list);
    return up;
}

bool net_wait_up(uint64_t deadline) {
    bool present;
    if (any_address(&present)) return true;
    if (!present) return false;
    n_handle watch = n_net_watch(), queue = watch >= 0 ? n_events_create() : -1;
    bool up = false;
    if (queue >= 0 && n_events_watch(queue, watch, N_EVENT_RECEIVABLE, 1) >= 0) {
        struct n_event event;
        while (!(up = any_address(&present)) && present) {
            n_result n = n_events_wait(queue, &event, 1, deadline);
            if (n <= 0) break; /* the deadline, or the queue went wrong */
            n_watch_take(watch);
        }
    }
    if (queue >= 0) n_handle_close(queue);
    if (watch >= 0) n_handle_close(watch);
    return up;
}

/* ---- names ---- */

static n_handle setup_cancel(void) { return (n_handle)(intptr_t)thread_local_get(N_TLS_NET_CANCEL) - 1; }

/* What net_resolve_at gives for a status the service could not be asked for. */
static n_result lookup_failed(struct n_dns_result *result, uint32_t status, n_result code) {
    *result = (struct n_dns_result){.version = 1, .status = status};
    return code;
}

/* The answer's message, new, waiting for it or for the thread's cancel channel's peer to go. */
static n_result lookup_answer(n_handle channel, n_handle cancel, struct n_network_reply **out) {
    if (cancel >= 0) {
        n_handle queue = n_events_create();
        if (queue < 0) return queue;
        n_result r = n_events_watch(queue, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1);
        if (r >= 0) r = n_events_watch(queue, cancel, N_EVENT_CLOSED, 2);
        bool cancelled = false, ready = false;
        while (r >= 0 && !ready && !cancelled) {
            struct n_event events[2];
            r = n_events_wait(queue, events, 2, N_CLOCK_NEVER);
            for (n_result i = 0; i < r; i++) {
                if (events[i].token == 2) cancelled = true;
                else ready = true;
            }
        }
        n_handle_close(queue);
        if (r < 0) return r;
        if (cancelled) return -N_ERR_CANCELLED;
    }
    n_result size = n_channel_receive(channel, NULL, 0, N_CHANNEL_WAIT | N_CHANNEL_PEEK, NULL);
    if (size < 0) return size;
    if ((size_t)size < sizeof **out) return -N_ERR_PROTOCOL;
    struct n_network_reply *reply = malloc((size_t)size);
    if (!reply) return -N_ERR_NO_MEMORY;
    n_result got = n_channel_receive(channel, reply, (size_t)size, 0, NULL);
    if (got != size || (size_t)size != sizeof *reply + (size_t)reply->count * sizeof(uint32_t)) {
        free(reply);
        return got < 0 ? got : -N_ERR_PROTOCOL;
    }
    *out = reply;
    return 0;
}

n_result net_resolve_at(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result,
                        const struct n_sockaddr *resolver) {
    n_handle cancel = setup_cancel();
    if (cancel >= 0) {
        n_handle queue = n_events_create();
        if (queue < 0) return lookup_failed(result, N_DNS_RESOURCE, queue);
        struct n_event event;
        n_result seen = n_events_watch(queue, cancel, N_EVENT_CLOSED, 1);
        if (seen >= 0) seen = n_events_wait(queue, &event, 1, 0);
        n_handle_close(queue);
        if (seen > 0) return lookup_failed(result, N_DNS_CANCELLED, -N_ERR_CANCELLED);
    }
    size_t length = strlen(name);
    if (length > DNS_NAME_TEXT_BYTES) return lookup_failed(result, N_DNS_FORMAT, -N_ERR_NAME_TOO_LONG);
    /* A name that is an address, or this system, asks nobody. */
    char plain[DNS_NAME_TEXT_BYTES + 1];
    memcpy(plain, name, length + 1);
    if (length && plain[length - 1] == '.') plain[length - 1] = 0;
    uint32_t literal = 0;
    if (ip_parse(plain, &literal) || !strcasecmp(plain, "localhost")) {
        if (!literal) literal = 0x7f000001;
        if (capacity) ips[0] = literal;
        *result = (struct n_dns_result){.version = 1, .status = N_DNS_OK, .count = 1};
        return 0;
    }
    n_handle channel = n_service_connect("network");
    if (channel < 0) return lookup_failed(result, N_DNS_UNREACHABLE, channel);
    size_t bytes = sizeof(struct n_network_request) + length;
    struct n_network_request *request = malloc(bytes);
    if (!request) {
        n_handle_close(channel);
        return lookup_failed(result, N_DNS_RESOURCE, -N_ERR_NO_MEMORY);
    }
    *request = (struct n_network_request){.op = N_NETWORK_RESOLVE, .name_length = (uint32_t)length};
    if (resolver) request->resolver = *resolver;
    memcpy(request + 1, name, length);
    struct n_network_reply *reply = NULL;
    n_result r = n_channel_send(channel, request, bytes, -1);
    free(request);
    if (r >= 0) r = lookup_answer(channel, cancel, &reply);
    n_handle_close(channel); /* an answer not taken: the service ends the lookup */
    if (r < 0) {
        return lookup_failed(result, r == -N_ERR_CANCELLED ? N_DNS_CANCELLED : r == -N_ERR_NO_MEMORY ? N_DNS_RESOURCE : N_DNS_UNREACHABLE, r);
    }
    *result = (struct n_dns_result){.version = 1, .status = reply->status, .count = reply->count, .ttl = reply->ttl};
    r = reply->result;
    if (r >= 0) {
        size_t n = reply->count < capacity ? reply->count : capacity;
        if (n) memcpy(ips, reply + 1, n * sizeof *ips);
    }
    free(reply);
    return r;
}

n_result net_resolve_all(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result) {
    return net_resolve_at(name, ips, capacity, result, NULL);
}

n_result net_resolve(const char *name, uint32_t *ip) {
    struct n_dns_result result;
    return net_resolve_at(name, ip, 1, &result, NULL);
}

n_result n_network_links(struct n_network_link **out) {
    n_handle channel = n_service_connect("network");
    if (channel < 0) return channel;
    struct n_network_request request = {.op = N_NETWORK_LINKS};
    struct n_network_reply *reply = NULL;
    n_result r = n_channel_send(channel, &request, sizeof request, -1);
    if (r >= 0) {
        r = n_channel_receive(channel, NULL, 0, N_CHANNEL_WAIT | N_CHANNEL_PEEK, NULL);
        if (r >= (n_result)sizeof *reply && (reply = malloc((size_t)r))) r = n_channel_receive(channel, reply, (size_t)r, 0, NULL);
        else if (r >= 0) r = -N_ERR_NO_MEMORY;
    }
    n_handle_close(channel);
    if (r < 0) {
        free(reply);
        return r;
    }
    /* The links come after the reply's head: moved to the front, the block is theirs. */
    n_result count = reply->result < 0 ? reply->result : (n_result)reply->count;
    if (count >= 0) memmove(reply, reply + 1, (size_t)r - sizeof *reply);
    *out = (struct n_network_link *)reply;
    if (count < 0) free(reply);
    return count;
}

bool net_proxy_for(const char *host, char *proxy, size_t cap, uint16_t *port) {
    uint32_t ip;
    /* This system is never behind the proxy. */
    if (!strcasecmp(host, "localhost") || (ip_parse(host, &ip) && ip >> 24 == 127)) return false;
    n_result length = n_setting_get(n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_PROXY_HOST), proxy, cap - 1);
    if (length <= 0) return false;
    proxy[length] = 0;
    int32_t number;
    if (n_setting_get_int(n_setting_at(N_DOMAIN_NETWORK, N_NETWORK_PROXY_PORT), &number) < 0) return false;
    *port = (uint16_t)number;
    return true;
}
