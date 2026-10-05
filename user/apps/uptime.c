#include <stdio.h>
#include "nocturne.h"
int main(void) {
    struct n_sysinfo si;
    sysinfo(&si);
    unsigned long s = si.uptime_ms / 1000;
    printf("up %lu day(s), %02lu:%02lu:%02lu, %d tasks\n", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60, si.ntasks);
    return 0;
}
