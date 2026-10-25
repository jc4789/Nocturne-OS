/* volume: show or set the master volume (0-100); "+N" and "-N" change it by N. */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"

static void show(const struct n_audio_output *o) {
    printf("volume: %d%%%s%s%s\n", (int)o->volume, o->muted ? ", muted" : "", o->device[0] ? ", " : ", no sound device",
           o->device);
}

int main(int argc, char **argv) {
    struct n_audio_output o;
    n_result r = n_audio_output(&o, 0);
    if (r < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(r));
        return 1;
    }
    if (argc < 2) {
        show(&o);
        return 0;
    }
    const char *a = argv[1];
    char *end;
    long v = strtol(a, &end, 10);
    if (end == a || *end) {
        fprintf(stderr, "usage: volume [0-100 | +N | -N]\n");
        return 2;
    }
    if (a[0] == '+' || a[0] == '-') v += o.volume;
    o.volume = (int32_t)MAX(0, MIN(100, v));
    r = n_audio_output(&o, N_AUDIO_SET_VOLUME);
    if (r < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(r));
        return 1;
    }
    show(&o);
    return 0;
}
