#include <stdio.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char path[1024];
        n_result r = n_path_complete(argv[i], path, sizeof path);
        n_handle h = r < 0 ? (n_handle)r : n_document_open(path, N_DOCUMENT_CREATE);
        if (h >= 0) n_handle_close(h);
        else if (h != -N_ERR_EXISTS) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], n_result_text(h));
            rc = 1;
        }
    }
    return rc;
}
