#include <stdlib.h>
#include "nocturne.h"
int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 0;
    if (seconds > 0) n_sleep_until(n_clock_after((uint64_t)(seconds * N_NS_PER_S)));
    return 0;
}
