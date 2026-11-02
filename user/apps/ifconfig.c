/* ifconfig: show the network adapters */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
#include "nocturne_services.h"

/* The kernel's list, and what the service network says of each adapter. */
int main(void) {
    n_result count = n_net_adapters(NULL, 0);
    struct n_net_adapter *list = count > 0 ? malloc((size_t)count * sizeof *list) : NULL;
    if (list) count = n_net_adapters(list, (size_t)count);
    if (!list || count <= 0) {
        printf("no network adapter\n");
        return 1;
    }
    struct n_network_link *links = NULL;
    n_result link_count = n_network_links(&links);
    for (n_result i = 0; i < count; i++) {
        const struct n_net_adapter *a = &list[i];
        char driver[64] = "";
        n_result named = n_net_adapter_name(a->id, driver, sizeof driver - 1);
        if (named > 0) driver[named < (n_result)sizeof driver ? named : (n_result)sizeof driver - 1] = 0;
        printf("\033[1;35madapter %u\033[0m: %s  driver %s\n", (unsigned)a->id,
               !a->present ? "\033[31mabsent\033[0m" : a->ip ? "\033[32mUP\033[0m" : "\033[33mno address\033[0m", driver);
        printf("  ether     %02x:%02x:%02x:%02x:%02x:%02x\n", a->mac[0], a->mac[1], a->mac[2], a->mac[3], a->mac[4], a->mac[5]);
        char text[16];
        if (a->ip) printf("  inet      %s/%d\n", ip_format(a->ip, text), __builtin_popcount(a->mask));
        struct n_network_link *link = links;
        for (n_result l = 0; l < link_count; l++, link = n_network_link_next(link)) {
            if (link->adapter != a->id) continue;
            if (link->gateway) printf("  gateway   %s\n", ip_format(link->gateway, text));
            const uint32_t *addresses = n_network_link_addresses(link);
            for (uint32_t d = 0; d < link->dns_count; d++) printf("  dns       %s\n", ip_format(addresses[d], text));
            for (uint32_t d = 0; d < link->ntp_count; d++) printf("  ntp       %s\n", ip_format(addresses[link->dns_count + d], text));
        }
        printf("  packets   rx %lu  tx %lu\n", (unsigned long)a->rx_packets, (unsigned long)a->tx_packets);
    }
    free(links);
    free(list);
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
