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
    return 0;
}
