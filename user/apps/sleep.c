#include <stdlib.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc > 1) msleep((unsigned)(atof(argv[1]) * 1000));
    return 0;
}
