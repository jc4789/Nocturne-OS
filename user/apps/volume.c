/* volume: show or set the master volume (0-100); "+N" and "-N" change it by N. */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"

int main(int argc, char **argv) {
    n_result cur = n_audio_volume();
    if (cur < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(cur));
        return 1;
    }
    if (argc < 2) {
        printf("volume: %d%%\n", (int)cur);
        return 0;
    }
    const char *a = argv[1];
    char *end;
    long v = strtol(a, &end, 10);
    if (end == a || *end) {
        fprintf(stderr, "usage: volume [0-100 | +N | -N]\n");
        return 2;
    }
    if (a[0] == '+' || a[0] == '-') v += cur;
    v = MAX(0, MIN(100, v));
    n_result r = n_audio_set_volume((int)v);
    if (r < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(r));
        return 1;
    }
    printf("volume: %d%%\n", (int)r);
    return 0;
}
