#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
static int cat_fd(int fd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write(1, buf, n) < 0) return 1;
    return n < 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return cat_fd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = !strcmp(argv[i], "-") ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        struct n_stat st;
        if (fstat(fd, &st) == 0 && st.type == N_FT_DIR) {
            fprintf(stderr, "cat: %s: is a directory\n", argv[i]);
            rc = 1;
        } else {
            rc |= cat_fd(fd);
        }
        if (fd) close(fd);
    }
    return rc;
}
