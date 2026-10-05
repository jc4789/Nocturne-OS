#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mv SRC DST\n");
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
    if (rename(argv[1], dst) < 0) {
        fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}
