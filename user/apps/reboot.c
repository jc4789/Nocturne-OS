#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Rebooting...\n");
    fflush(stdout);
    msleep(300);
    reboot();
    return 1;
}
