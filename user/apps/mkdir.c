#include <stdio.h>
#include <string.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir DIR...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) continue;
        char path[512];
        n_result r = n_path_complete(argv[i], path, sizeof path);
        if (r >= 0) r = n_folder_create(path);
        if (r < 0 && r != -N_ERR_EXISTS) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], n_result_text(r));
            rc = 1;
        }
    }
    return rc;
}
