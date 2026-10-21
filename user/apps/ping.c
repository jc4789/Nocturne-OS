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
    if (!net_wait_up(5000)) {
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
    int sent = 0, got = 0, min = 1 << 30, max = 0, sum = 0;
    for (int seq = 1; count <= 0 || seq <= count; seq++) {
        uint64_t t0 = n_clock_uptime_ms();
        n_result reply = net_ping(ip, seq, 2000);
        sent++;
        if (reply >= 0) {
            int rtt = (int)reply;
            got++;
            sum += rtt;
            if (rtt < min) min = rtt;
            if (rtt > max) max = rtt;
            printf("reply from %s: seq=%d time=%d ms\n", buf, seq, rtt);
        } else if (reply == -N_ERR_STOPPED) {
            break;
        } else {
            printf("seq=%d: %s\n", seq, n_result_text(reply));
        }
        if (count <= 0 || seq < count) {
            uint64_t spent = n_clock_uptime_ms() - t0;
            if (spent < 1000) n_sleep_ms((unsigned)(1000 - spent));
        }
    }
    printf("--- %s: %d sent, %d received, %d%% loss", host, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) printf(", rtt min/avg/max %d/%d/%d ms", min, sum / got, max);
    printf("\n");
    return got ? 0 : 1;
}
