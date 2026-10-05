#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
static bool recursive, force;
static int rm(const char *p) {
    struct n_stat st;
    if (stat(p, &st) < 0) {
        if (force) return 0;
        fprintf(stderr, "rm: %s: %s\n", p, strerror(errno));
        return 1;
    }
    if (st.type == N_FT_DIR && recursive) {
        for (;;) {
            int fd = open(p, O_RDONLY | O_DIRECTORY);
            if (fd < 0) break;
            struct n_dirent d;
            int r = readdir(fd, 0, &d);
            close(fd);
            if (r <= 0) break;
            char sub[512];
            snprintf(sub, sizeof sub, "%s/%s", p, d.name);
            if (rm(sub)) return 1;
        }
    } else if (st.type == N_FT_DIR) {
        fprintf(stderr, "rm: %s: is a directory (use -r)\n", p);
        return 1;
    }
    if (unlink(p) < 0) {
        fprintf(stderr, "rm: %s: %s\n", p, strerror(errno));
        return 1;
    }
    return 0;
}
int main(int argc, char **argv) {
    int rc = 0, files = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strchr(argv[i], 'r')) recursive = true;
            if (strchr(argv[i], 'f')) force = true;
            continue;
        }
        files++;
        rc |= rm(argv[i]);
    }
    if (!files) fprintf(stderr, "usage: rm [-rf] FILE...\n");
    return rc;
}
