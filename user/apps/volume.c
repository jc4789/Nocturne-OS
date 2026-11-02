/* volume: show the master volume (0-100). The Settings window and the shell change it. */
#include <stdio.h>
#include "nocturne.h"

int main(void) {
    struct n_audio_output o;
    n_result r = n_audio_output(&o);
    if (r < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(r));
        return 1;
    }
    printf("volume: %d%%%s%s%s\n", (int)o.volume, o.muted ? ", muted" : "", o.device[0] ? ", " : ", no sound device", o.device);
    return 0;
}
