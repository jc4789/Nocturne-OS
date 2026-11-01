/* Networking system call wrappers and address helpers. Each returns its
   result or a negative n_result, and leaves errno alone. */
#include <stdio.h>
#include "nocturne.h"

#define NET(n, a, b, c, d, e) __syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))

n_result net_info(struct n_netinfo *ni) { return NET(SYS_NET_INFO, ni, 0, 0, 0, 0); }
n_result net_ping(uint32_t ip, int seq, int timeout_ms) { return NET(SYS_NET_PING, ip, seq, timeout_ms, 0, 0); }
n_result net_resolve(const char *name, uint32_t *ip) { return NET(SYS_NET_DNS, name, ip, 0, 0, 0); }
n_result net_resolve_all(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result) {
    return NET(SYS_NET_DNS_ALL, name, ips, capacity, result, 0);
}
n_result net_resolve_at(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result,
                        const struct n_sockaddr *resolver) {
    return NET(SYS_NET_DNS_ALL, name, ips, capacity, result, resolver);
}
n_result net_stats(struct n_netstats *stats) { return NET(SYS_NET_STATS, stats, 0, 0, 0, 0); }
n_result net_set_setup_cancel_channel(n_handle channel) { return NET(SYS_NET_SETUP_CANCEL_CHANNEL, channel, 0, 0, 0, 0); }

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
n_result udp_recvfrom(n_handle s, void *buf, size_t len, int timeout_ms, struct n_sockaddr *from) {
    return NET(SYS_UDP_RECV, s, buf, len, timeout_ms, from);
}
n_result udp_close(n_handle s) { return NET(SYS_UDP_CLOSE, s, 0, 0, 0, 0); }
n_result tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) { return NET(SYS_TCP_CONNECT, ip, port, timeout_ms, 0, 0); }
n_result tcp_send(n_handle s, const void *buf, size_t len) { return NET(SYS_TCP_SEND, s, buf, len, 0, 0); }
n_result tcp_send_until(n_handle s, const void *buf, size_t len, uint64_t deadline_ms) {
    return NET(SYS_TCP_SEND_UNTIL, s, buf, len, deadline_ms, 0);
}
n_result tcp_send_some(n_handle s, const void *buf, size_t len) {
    return NET(SYS_TCP_SEND_UNTIL, s, buf, len, 0, N_TCP_SEND_NONBLOCK);
}
n_result tcp_cancel(n_handle s) { return NET(SYS_TCP_CANCEL, s, 0, 0, 0, 0); }
n_result tcp_recv(n_handle s, void *buf, size_t len, int timeout_ms) { return NET(SYS_TCP_RECV, s, buf, len, timeout_ms, 0); }
n_result tcp_close(n_handle s) { return NET(SYS_TCP_CLOSE, s, 0, 0, 0, 0); }
n_result tcp_listen(uint32_t ip, uint16_t port, int backlog) { return NET(SYS_TCP_LISTEN, ip, port, backlog, 0, 0); }
n_result tcp_accept(n_handle listener, int timeout_ms, struct n_sockaddr *remote) {
    return NET(SYS_TCP_ACCEPT, listener, timeout_ms, remote, 0, 0);
}
n_result tcp_address(n_handle s, struct n_sockaddr *local, struct n_sockaddr *remote) {
    return NET(SYS_TCP_ADDRESS, s, local, remote, 0, 0);
}
n_result tcp_finish(n_handle s) { return NET(SYS_TCP_FINISH, s, 0, 0, 0, 0); }
n_result net_test(const struct n_netfault *set, struct n_tcpstats *stats) { return NET(SYS_NET_TEST, set, stats, 0, 0, 0); }

/* wait for the interface to get an address (DHCP runs in the background after boot) */
bool net_wait_up(int timeout_ms) {
    struct n_netinfo ni;
    uint64_t until = uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (net_info(&ni) < 0 || !ni.present) return false;
        if (ni.up) return true;
        if (uptime_ms() >= until) return false;
        n_sleep_ms(100);
    }
}
