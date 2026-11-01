/* ping: ICMP echo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

int main(int argc, char **argv) {
    int count = 4;
    const char *host = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) count = atoi(argv[++i]);
        else host = argv[i];
    }
    if (!host) {
        fprintf(stderr, "usage: ping [-c count] host\n");
        return 2;
    }
    if (!net_wait_up(n_clock_after(5000 * N_NS_PER_MS))) {
        fprintf(stderr, "ping: network is down\n");
        return 1;
    }
    uint32_t ip;
    n_result resolved = net_resolve(host, &ip);
    if (resolved < 0) {
        fprintf(stderr, "ping: %s: %s\n", host, n_result_text(resolved));
        return 1;
    }
    char buf[16];
    printf("PING %s (%s): 40 data bytes\n", host, ip_format(ip, buf));
    int sent = 0, got = 0;
    uint64_t min = UINT64_MAX, max = 0, sum = 0;
    for (int seq = 1; count <= 0 || seq <= count; seq++) {
        uint64_t next = n_clock_after(N_NS_PER_S);
        n_result reply = net_ping(ip, seq, n_clock_after(2 * N_NS_PER_S));
        sent++;
        if (reply >= 0) {
            uint64_t rtt = (uint64_t)reply;
            got++;
            sum += rtt;
            if (rtt < min) min = rtt;
            if (rtt > max) max = rtt;
            printf("reply from %s: seq=%d time=%llu.%03llu ms\n", buf, seq, (unsigned long long)(rtt / N_NS_PER_MS),
                   (unsigned long long)(rtt % N_NS_PER_MS / N_NS_PER_US));
        } else if (reply == -N_ERR_STOPPED) {
            break;
        } else {
            printf("seq=%d: %s\n", seq, n_result_text(reply));
        }
        if (count <= 0 || seq < count) n_sleep_until(next);
    }
    printf("--- %s: %d sent, %d received, %d%% loss", host, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got)
        printf(", rtt min/avg/max %llu/%llu/%llu us", (unsigned long long)(min / N_NS_PER_US),
               (unsigned long long)(sum / got / N_NS_PER_US), (unsigned long long)(max / N_NS_PER_US));
    printf("\n");
    return got ? 0 : 1;
}
