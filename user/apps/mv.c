#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mv SRC DST\n");
        return 1;
    }
    char src[1024], dst[1024], named[1024];
    struct n_entry e;
    n_result r = n_path_complete(argv[1], src, sizeof src);
    if (r >= 0) r = n_path_complete(argv[2], named, sizeof named);
    if (r >= 0 && n_entry_info(named, &e) >= 0 && e.kind == N_KIND_FOLDER) r = n_path_join(named, n_path_name(src), dst, sizeof dst);
    else if (r >= 0) memcpy(dst, named, sizeof dst);
    /* ISO C rename: an existing document named dst is replaced whole */
    if (r < 0 || rename(src, dst) < 0) {
        fprintf(stderr, "mv: %s: %s\n", argv[1], n_result_text(r < 0 ? r : -errno));
        return 1;
    }
    return 0;
}
