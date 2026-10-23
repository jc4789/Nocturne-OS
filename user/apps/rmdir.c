/* rmdir: remove empty folders */
#include <stdio.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir dir...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[1024];
        struct n_entry e;
        n_result r = n_path_complete(argv[i], path, sizeof path);
        if (r >= 0) r = n_entry_info(path, &e);
        if (r >= 0 && e.kind != N_KIND_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r >= 0) r = n_entry_remove(path);
        if (r < 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], n_result_text(r));
            rc = 1;
        }
    }
    return rc;
}
