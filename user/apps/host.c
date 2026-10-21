/* host: DNS lookup */
#include <stdio.h>
#include "nocturne.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: host name...\n");
        return 2;
    }
    if (!net_wait_up(5000)) {
        fprintf(stderr, "host: network is down\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t ip;
        char buf[16];
        n_result r = net_resolve(argv[i], &ip);
        if (r < 0) {
            printf("%s: %s\n", argv[i], n_result_text(r));
            status = 1;
        } else {
            printf("%s has address %s\n", argv[i], ip_format(ip, buf));
        }
    }
    return status;
}
