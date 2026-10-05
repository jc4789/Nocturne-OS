#include <stdio.h>
#include <string.h>
#include "nocturne.h"
static int dirs, files;
static void tree(const char *path, const char *prefix) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    static struct n_dirent ents_buf[8][128];
    static int depth;
    if (depth >= 8) {
        close(fd);
        return;
    }
    struct n_dirent *ents = ents_buf[depth];
    int n = 0;
    while (n < 128 && readdir(fd, n, &ents[n]) > 0) n++;
    close(fd);
    depth++;
    for (int i = 0; i < n; i++) {
        bool last = i == n - 1;
        printf("%s%s%s%s\x1b[0m\n", prefix, last ? "└── " : "├── ", ents[i].type == N_FT_DIR ? "\x1b[1;34m" : "",
               ents[i].name);
        if (ents[i].type == N_FT_DIR) {
            dirs++;
            char sub[512], np[256];
            snprintf(sub, sizeof sub, "%s/%s", strcmp(path, "/") ? path : "", ents[i].name);
            snprintf(np, sizeof np, "%s%s", prefix, last ? "    " : "│   ");
            tree(sub, np);
        } else {
            files++;
        }
    }
    depth--;
}
int main(int argc, char **argv) {
    const char *p = argc > 1 ? argv[1] : ".";
    printf("\x1b[1;34m%s\x1b[0m\n", p);
    tree(p, "");
    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}
