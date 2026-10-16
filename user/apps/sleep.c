#include <stdlib.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    if (argc > 1) n_sleep_ms((unsigned)(atof(argv[1]) * 1000));
    return 0;
}
