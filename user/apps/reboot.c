#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Rebooting...\n");
    fflush(stdout);
    n_sleep_ms(300);
    reboot();
    return 1;
}
