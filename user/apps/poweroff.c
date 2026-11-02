#include <stdio.h>
#include "nocturne.h"
#include "nocturne_services.h"
int main(void) {
    printf("Good night!\n");
    fflush(stdout);
    n_sleep_until(n_clock_after(300 * N_NS_PER_MS));
    n_result r = n_machine_power(N_MACHINE_POWER_OFF);
    fprintf(stderr, "poweroff: %s\n", n_result_text(r));
    return 1;
}
