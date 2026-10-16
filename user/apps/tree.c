#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
static int dirs, files;
static void tree(const char *path, const char *prefix) {
    struct n_entry *ents;
    n_result n = n_folder_entries(path, &ents);
    if (n < 0) return;
    for (int i = 0; i < (int)n; i++) {
        bool last = i == n - 1;
        bool folder = ents[i].kind == N_KIND_FOLDER;
        printf("%s%s%s%s\x1b[0m\n", prefix, last ? "└── " : "├── ", folder ? "\x1b[1;34m" : "", ents[i].name);
        if (folder) {
            dirs++;
            char sub[512], np[512];
            if (n_path_join(path, ents[i].name, sub, sizeof sub) < 0) continue;
            snprintf(np, sizeof np, "%s%s", prefix, last ? "    " : "│   ");
            tree(sub, np);
        } else {
            files++;
        }
    }
    free(ents);
}
int main(int argc, char **argv) {
    const char *p = argc > 1 ? argv[1] : ".";
    char path[512];
    n_result r = n_path_complete(p, path, sizeof path);
    if (r < 0) {
        fprintf(stderr, "tree: %s: %s\n", p, n_result_text(r));
        return 1;
    }
    printf("\x1b[1;34m%s\x1b[0m\n", p);
    tree(path, "");
    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}
