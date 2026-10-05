#include <stdio.h>
#include "nocturne.h"
int main(void) {
    struct n_sysinfo si;
    sysinfo(&si);
    unsigned long t = si.total_mem / 1024, f = si.free_mem / 1024;
    printf("\x1b[1m          total       used       free\x1b[0m\n");
    printf("Mem:  %9luK %9luK %9luK\n", t, t - f, f);
    printf("Kernel heap in use: %luK\n", (unsigned long)(si.heap_used / 1024));
    return 0;
}
