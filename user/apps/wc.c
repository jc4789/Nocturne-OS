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
static bool want_l, want_w, want_c;
static void show(long l, long w, long c, const char *name) {
    if (want_l) printf("%7ld", l);
    if (want_w) printf(" %7ld", w);
    if (want_c) printf(" %7ld", c);
    if (name) printf(" %s", name);
    printf("\n");
}
int main(int argc, char **argv) {
    long tl = 0, tw = 0, tc = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l') want_l = true;
            else if (*p == 'w') want_w = true;
            else if (*p == 'c') want_c = true;
            else {
                fprintf(stderr, "usage: wc [-lwc] [file...]\n");
                return 2;
            }
        }
    }
    if (!want_l && !want_w && !want_c) want_l = want_w = want_c = true;
    if (first >= argc) {
        count(0, &tl, &tw, &tc);
        /* a lone count from stdin is printed bare, so scripts can use it */
        if (want_l + want_w + want_c == 1) printf("%ld\n", want_l ? tl : want_w ? tw : tc);
        else show(tl, tw, tc, NULL);
        return 0;
    }
    int nfiles = argc - first;
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        long l = 0, w = 0, c = 0;
        count(fd, &l, &w, &c);
        close(fd);
        show(l, w, c, argv[i]);
        tl += l, tw += w, tc += c;
    }
    if (nfiles > 1) show(tl, tw, tc, "total");
    return 0;
}
