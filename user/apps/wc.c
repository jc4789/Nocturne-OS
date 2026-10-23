#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "nocturne.h"
struct counts {
    long l, w, c;
    bool inword;
};
static void count(struct counts *k, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') k->l++;
        if (isspace((unsigned char)buf[i])) k->inword = false;
        else if (!k->inword) {
            k->inword = true;
            k->w++;
        }
    }
    k->c += (long)n;
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
    static char buf[65536];
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
        struct counts k = {0};
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) count(&k, buf, n);
        /* a lone count from the input is printed bare, so scripts can use it */
        if (want_l + want_w + want_c == 1) printf("%ld\n", want_l ? k.l : want_w ? k.w : k.c);
        else show(k.l, k.w, k.c, NULL);
        return 0;
    }
    int nfiles = argc - first;
    for (int i = first; i < argc; i++) {
        char path[1024];
        n_result r = n_path_complete(argv[i], path, sizeof path);
        n_handle h = r < 0 ? (n_handle)r : n_document_open(path, N_DOCUMENT_READ);
        if (h < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], n_result_text(h));
            continue;
        }
        struct counts k = {0};
        uint64_t at = 0;
        while ((r = n_document_read_at(h, at, buf, sizeof buf)) > 0) {
            count(&k, buf, (size_t)r);
            at += (uint64_t)r;
        }
        n_handle_close(h);
        show(k.l, k.w, k.c, argv[i]);
        tl += k.l, tw += k.w, tc += k.c;
    }
    if (nfiles > 1) show(tl, tw, tc, "total");
    return 0;
}
