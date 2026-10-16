#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

static bool opt_l, opt_a, tty;

static int cmp(const void *a, const void *b) {
    const struct n_entry *x = a, *y = b;
    if ((x->kind == N_KIND_FOLDER) != (y->kind == N_KIND_FOLDER)) return x->kind == N_KIND_FOLDER ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void human(uint64_t n, char *out) {
    if (n < 1024) sprintf(out, "%luB", (unsigned long)n);
    else if (n < 1024 * 1024) sprintf(out, "%.1fK", n / 1024.0);
    else sprintf(out, "%.1fM", n / 1048576.0);
}

static const char *color(const struct n_entry *d, const char *dir) {
    if (!tty) return "";
    if (d->kind == N_KIND_FOLDER) return "\x1b[1;34m";
    if (d->kind == N_KIND_DEVICE) return "\x1b[33m";
    if (!strcmp(dir, "/bin")) return "\x1b[1;32m";
    const char *dot = strrchr(d->name, '.');
    if (dot && !strcmp(dot, ".sh")) return "\x1b[32m";
    return "";
}

static int list(const char *name, bool header) {
    char path[512];
    struct n_entry st;
    n_result r = n_path_complete(name, path, sizeof path);
    if (r >= 0) r = n_entry_info(path, &st);
    if (r < 0) {
        fprintf(stderr, "ls: %s: %s\n", name, n_result_text(r));
        return 1;
    }
    if (st.kind != N_KIND_FOLDER) {
        printf("%s\n", name);
        return 0;
    }
    struct n_entry *all;
    r = n_folder_entries(path, &all);
    if (r < 0) {
        fprintf(stderr, "ls: %s: %s\n", name, n_result_text(r));
        return 1;
    }
    int n = 0;
    for (int i = 0; i < (int)r; i++)
        if (opt_a || all[i].name[0] != '.') all[n++] = all[i];
    qsort(all, n, sizeof *all, cmp);
    if (header) printf("%s:\n", name);
    if (opt_l) {
        for (int i = 0; i < n; i++) {
            char sz[16];
            human(all[i].size, sz);
            bool folder = all[i].kind == N_KIND_FOLDER;
            const char *t = folder ? "dir " : all[i].kind == N_KIND_DEVICE ? "dev " : "file";
            printf("%s %8s  %s%s%s%s\n", t, folder ? "-" : sz, color(&all[i], path), all[i].name, tty ? "\x1b[0m" : "",
                   folder ? "/" : "");
        }
    } else if (!tty) {
        /* piped or redirected: one name per line, so `ls | wc -l` and scripts work */
        for (int i = 0; i < n; i++) printf("%s%s\n", all[i].name, all[i].kind == N_KIND_FOLDER ? "/" : "");
    } else {
        int width = 0;
        for (int i = 0; i < n; i++)
            if ((int)strlen(all[i].name) + 2 > width) width = (int)strlen(all[i].name) + 2;
        int percol = width ? 78 / width : 1;
        if (percol < 1) percol = 1;
        for (int i = 0; i < n; i++) {
            printf("%s%s\x1b[0m%*s", color(&all[i], path), all[i].name, width - (int)strlen(all[i].name), "");
            if ((i + 1) % percol == 0 || i == n - 1) printf("\n");
        }
    }
    free(all);
    return 0;
}

int main(int argc, char **argv) {
    int first = 1;
    tty = n_terminal(1);
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
