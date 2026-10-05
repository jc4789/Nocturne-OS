#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill PID...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (kill(atoi(argv[i])) < 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
