#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cp SRC DST\n");
        return 1;
    }
    char dst[512];
    struct n_stat st;
    strlcpy(dst, argv[2], sizeof dst);
    if (stat(dst, &st) == 0 && st.type == N_FT_DIR) {
        const char *base = strrchr(argv[1], '/');
        base = base ? base + 1 : argv[1];
        snprintf(dst, sizeof dst, "%s/%s", argv[2], base);
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        return 1;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) write(out, buf, n);
    close(in);
    close(out);
    return 0;
}
