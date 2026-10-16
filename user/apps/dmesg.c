#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
int main(void) {
    char *buf = malloc(65536);
    n_result n = buf ? n_system_log(buf, 65536) : -N_ERR_NO_MEMORY;
    if (n < 0) {
        fprintf(stderr, "dmesg: %s\n", n_result_text(n));
        return 1;
    }
    fwrite(buf, 1, (size_t)n, stdout);
    return 0;
}
