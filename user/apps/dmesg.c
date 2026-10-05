#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
int main(void) {
    char *buf = malloc(65536);
    int n = dmesg(buf, 65536);
    if (n > 0) write(1, buf, n);
    return 0;
}
