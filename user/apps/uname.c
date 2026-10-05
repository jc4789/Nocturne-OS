#include <stdio.h>
#include <string.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    struct n_sysinfo si;
    sysinfo(&si);
    if (argc > 1 && !strcmp(argv[1], "-a")) printf("%s x86_64 %s\n", si.os, si.cpu);
    else printf("Nocturne\n");
    return 0;
}
