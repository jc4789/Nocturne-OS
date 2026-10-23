#include <stdio.h>
#include <string.h>
#include "nocturne.h"
static char buf[65536];
static int cat_input(void) {
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
        if (fwrite(buf, 1, n, stdout) != n) return 1;
    return ferror(stdin) != 0;
}
static int cat_document(const char *name) {
    char path[1024];
    struct n_entry e;
    n_result r = n_path_complete(name, path, sizeof path);
    if (r >= 0) r = n_entry_info(path, &e);
    if (r >= 0 && e.kind == N_KIND_FOLDER) r = -N_ERR_IS_FOLDER;
    n_handle h = r < 0 ? (n_handle)r : n_document_open(path, N_DOCUMENT_READ);
    if (h < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, n_result_text(h));
        return 1;
    }
    uint64_t at = 0;
    while ((r = n_document_read_at(h, at, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, (size_t)r, stdout) != (size_t)r) break;
        at += (uint64_t)r;
    }
    n_handle_close(h);
    if (r < 0) fprintf(stderr, "cat: %s: %s\n", name, n_result_text(r));
    return r != 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return cat_input();
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= !strcmp(argv[i], "-") ? cat_input() : cat_document(argv[i]);
    return rc;
}
