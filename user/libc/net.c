/* Networking system call wrappers and address helpers. */
#include <stdio.h>
#include <errno.h>
#include "nocturne.h"

static long ret(long r) {
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int net_info(struct n_netinfo *ni) { return (int)ret(__syscall(SYS_NET_INFO, (long)ni, 0, 0, 0, 0)); }
int net_ping(uint32_t ip, int seq, int timeout_ms) {
    return (int)ret(__syscall(SYS_NET_PING, ip, seq, timeout_ms, 0, 0));
}
int net_resolve(const char *name, uint32_t *ip) { return (int)ret(__syscall(SYS_NET_DNS, (long)name, (long)ip, 0, 0, 0)); }

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

int udp_socket(uint16_t port) { return (int)ret(__syscall(SYS_UDP_SOCKET, port, 0, 0, 0, 0)); }
int udp_sendto(int s, uint32_t ip, uint16_t port, const void *buf, size_t len) {
    return (int)ret(__syscall(SYS_UDP_SEND, s, ip, port, (long)buf, (long)len));
}
int udp_recvfrom(int s, void *buf, size_t len, int timeout_ms, struct n_sockaddr *from) {
    return (int)ret(__syscall(SYS_UDP_RECV, s, (long)buf, (long)len, timeout_ms, (long)from));
}
int udp_close(int s) { return (int)ret(__syscall(SYS_UDP_CLOSE, s, 0, 0, 0, 0)); }
int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    return (int)ret(__syscall(SYS_TCP_CONNECT, ip, port, timeout_ms, 0, 0));
}
long tcp_send(int s, const void *buf, size_t len) { return ret(__syscall(SYS_TCP_SEND, s, (long)buf, (long)len, 0, 0)); }
long tcp_recv(int s, void *buf, size_t len, int timeout_ms) {
    return ret(__syscall(SYS_TCP_RECV, s, (long)buf, (long)len, timeout_ms, 0));
}
int tcp_close(int s) { return (int)ret(__syscall(SYS_TCP_CLOSE, s, 0, 0, 0, 0)); }

/* wait for the interface to get an address (DHCP runs in the background after boot) */
bool net_wait_up(int timeout_ms) {
    struct n_netinfo ni;
    uint64_t until = uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (net_info(&ni) < 0 || !ni.present) return false;
        if (ni.up) return true;
        if (uptime_ms() >= until) return false;
        msleep(100);
    }
}
