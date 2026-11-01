#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Good night!\n");
    fflush(stdout);
    n_sleep_until(n_clock_after(300 * N_NS_PER_MS));
    poweroff();
    return 1;
}
