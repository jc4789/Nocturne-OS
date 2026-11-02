/* volume: show the master volume (0-100). The Settings window and the shell change it. */
#include <stdio.h>
#include <stdlib.h>
#include "nocturne.h"
#include "nocturne_settings.h"

int main(void) {
    int32_t volume = 0, muted = 0;
    n_result r = n_setting_get_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_VOLUME), &volume);
    if (r >= 0) r = n_setting_get_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_MUTED), &muted);
    if (r < 0) {
        fprintf(stderr, "volume: %s\n", n_result_text(r));
        return 1;
    }
    char *device = n_audio_output_name();
    printf("volume: %d%%%s, %s\n", (int)volume, muted ? ", muted" : "", device ? device : "no sound device");
    free(device);
    return 0;
}
