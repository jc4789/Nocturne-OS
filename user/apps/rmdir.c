/* rmdir: remove empty directories */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir dir...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct n_stat st;
        if (stat(argv[i], &st) < 0 || st.type != N_FT_DIR) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], st.type != N_FT_DIR && errno != ENOENT ? "not a directory" : strerror(errno));
            rc = 1;
        } else if (unlink(argv[i]) < 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
