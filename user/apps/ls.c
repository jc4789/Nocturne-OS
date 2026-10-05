#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

static bool opt_l, opt_a;

static int cmp(const void *a, const void *b) {
    const struct n_dirent *x = a, *y = b;
    if ((x->type == N_FT_DIR) != (y->type == N_FT_DIR)) return x->type == N_FT_DIR ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void human(uint64_t n, char *out) {
    if (n < 1024) sprintf(out, "%luB", (unsigned long)n);
    else if (n < 1024 * 1024) sprintf(out, "%.1fK", n / 1024.0);
    else sprintf(out, "%.1fM", n / 1048576.0);
}

static const char *color(const struct n_dirent *d, const char *dir) {
    if (d->type == N_FT_DIR) return "\x1b[1;34m";
    if (d->type == N_FT_CHAR) return "\x1b[33m";
    if (!strcmp(dir, "/bin")) return "\x1b[1;32m";
    const char *dot = strrchr(d->name, '.');
    if (dot && !strcmp(dot, ".sh")) return "\x1b[32m";
    return "";
}

static int list(const char *path, bool header) {
    struct n_stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (st.type != N_FT_DIR) {
        printf("%s\n", path);
        return 0;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int cap = 64, n = 0;
    struct n_dirent *ents = malloc(sizeof *ents * cap);
    struct n_dirent d;
    for (int i = 0; readdir(fd, i, &d) > 0; i++) {
        if (!opt_a && d.name[0] == '.') continue;
        if (n == cap) ents = realloc(ents, sizeof *ents * (cap *= 2));
        ents[n++] = d;
    }
    close(fd);
    qsort(ents, n, sizeof *ents, cmp);
    if (header) printf("%s:\n", path);
    if (opt_l) {
        for (int i = 0; i < n; i++) {
            char sz[16];
            human(ents[i].size, sz);
            const char *t = ents[i].type == N_FT_DIR ? "dir " : ents[i].type == N_FT_CHAR ? "dev " : "file";
            printf("%s %8s  %s%s\x1b[0m%s\n", t, ents[i].type == N_FT_DIR ? "-" : sz, color(&ents[i], path),
                   ents[i].name, ents[i].type == N_FT_DIR ? "/" : "");
        }
    } else {
        int width = 0;
        for (int i = 0; i < n; i++)
            if ((int)strlen(ents[i].name) + 2 > width) width = (int)strlen(ents[i].name) + 2;
        int percol = width ? 78 / width : 1;
        if (percol < 1) percol = 1;
        for (int i = 0; i < n; i++) {
            printf("%s%s\x1b[0m%*s", color(&ents[i], path), ents[i].name, width - (int)strlen(ents[i].name), "");
            if ((i + 1) % percol == 0 || i == n - 1) printf("\n");
        }
    }
    free(ents);
    return 0;
}

int main(int argc, char **argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        for (char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l') opt_l = true;
            else if (*p == 'a') opt_a = true;
        }
    }
    if (first >= argc) return list(".", false);
    int rc = 0;
    for (int i = first; i < argc; i++) {
        rc |= list(argv[i], argc - first > 1);
        if (argc - first > 1 && i < argc - 1) printf("\n");
    }
    return rc;
}
