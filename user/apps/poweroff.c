#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Good night!\n");
    fflush(stdout);
    n_sleep_until(n_clock_after(300 * N_NS_PER_MS));
    poweroff(n_cap_find(N_CAP_POWER));
    fprintf(stderr, "poweroff: this app holds no power capability\n");
    return 1;
}
