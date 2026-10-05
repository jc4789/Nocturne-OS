#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir DIR...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) continue;
        if (mkdir(argv[i]) < 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
