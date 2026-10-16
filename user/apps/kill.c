/* kill: stop running apps by the identifiers ps shows */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill ID...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        n_handle app = n_app_open(atoll(argv[i]));
        n_result r = app < 0 ? app : n_app_stop(app, 0);
        if (app >= 0) n_handle_close(app);
        if (r < 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], n_result_text(r));
            rc = 1;
        }
    }
    return rc;
}
