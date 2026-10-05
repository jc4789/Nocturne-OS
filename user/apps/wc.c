#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "nocturne.h"
static void count(int fd, long *l, long *w, long *c) {
    char buf[4096];
    ssize_t n;
    bool inword = false;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') (*l)++;
            if (isspace((unsigned char)buf[i])) inword = false;
            else if (!inword) {
                inword = true;
                (*w)++;
            }
        }
        *c += n;
    }
}
int main(int argc, char **argv) {
    long tl = 0, tw = 0, tc = 0;
    if (argc < 2) {
        count(0, &tl, &tw, &tc);
        printf("%7ld %7ld %7ld\n", tl, tw, tc);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        long l = 0, w = 0, c = 0;
        count(fd, &l, &w, &c);
        close(fd);
        printf("%7ld %7ld %7ld %s\n", l, w, c, argv[i]);
        tl += l, tw += w, tc += c;
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tc);
    return 0;
}
