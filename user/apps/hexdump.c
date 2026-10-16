#include <stdio.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc > 1) {
        in = fopen(argv[1], "rb");
        if (!in) {
            fprintf(stderr, "hexdump: %s: %s\n", argv[1], n_result_text(-errno));
            return 1;
        }
    }
    unsigned char buf[16];
    unsigned long off = 0;
    for (;;) {
        size_t n = fread(buf, 1, 16, in);
        if (!n) break;
        printf("\x1b[90m%08lx\x1b[0m  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", buf[i]);
            else printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" \x1b[36m|");
        for (size_t i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("|\x1b[0m\n");
        off += n;
        if (n < 16) break;
    }
    return 0;
}
