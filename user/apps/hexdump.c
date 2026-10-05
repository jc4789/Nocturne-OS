#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    int fd = argc > 1 ? open(argv[1], O_RDONLY) : 0;
    if (fd < 0) {
        fprintf(stderr, "hexdump: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    unsigned char buf[16];
    unsigned long off = 0;
    for (;;) {
        int n = 0;
        while (n < 16) {
            ssize_t r = read(fd, buf + n, 16 - n);
            if (r <= 0) break;
            n += r;
        }
        if (n <= 0) break;
        printf("\x1b[90m%08lx\x1b[0m  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", buf[i]);
            else printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" \x1b[36m|");
        for (int i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("|\x1b[0m\n");
        off += n;
        if (n < 16) break;
    }
    return 0;
}
