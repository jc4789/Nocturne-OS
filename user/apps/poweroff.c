#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Good night!\n");
    fflush(stdout);
    n_sleep_ms(300);
    poweroff();
    return 1;
}
