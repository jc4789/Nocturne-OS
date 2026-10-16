#include <stdio.h>
#include <string.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cp SRC DST\n");
        return 1;
    }
    char src[512], dst[512], named[512];
    struct n_entry e;
    n_result r = n_path_complete(argv[1], src, sizeof src);
    if (r >= 0) r = n_path_complete(argv[2], named, sizeof named);
    if (r >= 0 && n_entry_info(named, &e) >= 0 && e.kind == N_KIND_FOLDER) r = n_path_join(named, strrchr(src, '/') + 1, dst, sizeof dst);
    else if (r >= 0) memcpy(dst, named, sizeof dst);
    if (r < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[2], n_result_text(r));
        return 1;
    }
    n_handle in = n_document_open(src, N_DOCUMENT_READ);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", argv[1], n_result_text(in));
        return 1;
    }
    /* the copy appears whole when it is closed, replacing what dst held */
    n_handle out = n_document_open(dst, N_DOCUMENT_REPLACE);
    if (out < 0) {
        n_handle_close(in);
        fprintf(stderr, "cp: %s: %s\n", dst, n_result_text(out));
        return 1;
    }
    static char buf[65536];
    uint64_t at = 0;
    while ((r = n_document_read_at(in, at, buf, sizeof buf)) > 0) {
        n_result w = n_document_write_at(out, at, buf, (size_t)r);
        if (w < 0) {
            r = w;
            break;
        }
        at += (uint64_t)r;
    }
    n_handle_close(in);
    if (r < 0) {
        /* ending without closing keeps dst as it was */
        fprintf(stderr, "cp: %s: %s\n", dst, n_result_text(r));
        return 1;
    }
    r = n_handle_close(out);
    if (r < 0) {
        fprintf(stderr, "cp: %s: %s\n", dst, n_result_text(r));
        return 1;
    }
    return 0;
}
