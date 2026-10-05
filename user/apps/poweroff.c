#include <stdio.h>
#include "nocturne.h"
int main(void) {
    printf("Good night!\n");
    fflush(stdout);
    msleep(300);
    poweroff();
    return 1;
}
