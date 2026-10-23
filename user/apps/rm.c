#include <stdio.h>
#include <string.h>
#include "nocturne.h"
static bool recursive, force;
static int rm(const char *path, const char *shown) {
    struct n_entry e;
    n_result r = n_entry_info(path, &e);
    if (r < 0) {
        if (force && r == -N_ERR_NOT_FOUND) return 0;
        fprintf(stderr, "rm: %s: %s\n", shown, n_result_text(r));
        return 1;
    }
    if (e.kind == N_KIND_FOLDER && recursive) {
        /* the folder shrinks as entries go, so always take the first */
        struct n_entry first;
        while (n_folder_list(path, &first, 1, 0) == 1) {
            char sub[1024];
            if (n_path_join(path, first.name, sub, sizeof sub) < 0 || rm(sub, sub)) return 1;
        }
    } else if (e.kind == N_KIND_FOLDER) {
        fprintf(stderr, "rm: %s: is a directory (use -r)\n", shown);
        return 1;
    }
    r = n_entry_remove(path);
    if (r < 0) {
        fprintf(stderr, "rm: %s: %s\n", shown, n_result_text(r));
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
        char path[1024];
        n_result r = n_path_complete(argv[i], path, sizeof path);
        if (r < 0) {
            fprintf(stderr, "rm: %s: %s\n", argv[i], n_result_text(r));
            rc = 1;
        } else {
            rc |= rm(path, argv[i]);
        }
    }
    if (!files) fprintf(stderr, "usage: rm [-rf] FILE...\n");
    return rc;
}
