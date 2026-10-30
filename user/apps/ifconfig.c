/* ifconfig: show the network interface */
#include <stdio.h>
#include "nocturne.h"

static void addr(const char *label, const uint8_t *a) { printf("  %-9s %u.%u.%u.%u\n", label, a[0], a[1], a[2], a[3]); }

int main(void) {
    struct n_netinfo ni;
    if (net_info(&ni) < 0 || !ni.present) {
        printf("no network interface\n");
        return 1;
    }
    int prefix = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--) prefix += (ni.mask[i] >> b) & 1;
    printf("\033[1;35meth0\033[0m: %s  driver %s\n", ni.up ? "\033[32mUP\033[0m" : "\033[33mwaiting for DHCP\033[0m",
           ni.driver);
    printf("  ether     %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4],
           ni.mac[5]);
    if (ni.up) {
        printf("  inet      %u.%u.%u.%u/%d\n", ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3], prefix);
        addr("gateway", ni.gateway);
        addr("dns", ni.dns);
    }
    printf("  packets   rx %lu  tx %lu\n", (unsigned long)ni.rx_packets, (unsigned long)ni.tx_packets);
    struct n_netstats stats;
    if (net_stats(&stats)==0) {
        printf("  receive   dropped %lu  invalid %lu  budget yields %lu\n",
               (unsigned long)stats.rx_dropped,(unsigned long)stats.rx_invalid,(unsigned long)stats.rx_budget_yields);
        printf("  host ack  pending %lu  retries %lu  cancelled %lu\n",
               (unsigned long)stats.rx_completion_pending,(unsigned long)stats.rx_completion_retries,
               (unsigned long)stats.rx_completion_cancelled);
    }
    struct n_tcpstats tcp;
    if (net_test(NULL, &tcp)==0) {
        printf("  tcp       segments in %lu  out %lu  out-of-order %lu  duplicate acks %lu\n",
               (unsigned long)tcp.segs_in,(unsigned long)tcp.segs_out,(unsigned long)tcp.ooo_in,(unsigned long)tcp.dupacks_in);
        printf("  resend    timeouts %lu  retransmits %lu  fast %lu\n",
               (unsigned long)tcp.timeouts,(unsigned long)tcp.retransmits,(unsigned long)tcp.fast_retransmits);
    }
    return 0;
}
