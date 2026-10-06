/* play: play WAV files (any kind wav_open reads) on /dev/audio. The Sound Player is the same
   from the desktop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

static int out_fd;
static struct wav wav;

static bool out(const int16_t *buf, int frames) {
    const char *p = (const char *)buf;
    size_t left = (size_t)frames * 4;
    while (left) {
        ssize_t w = write(out_fd, p, left);
        if (w < 0) {
            if (errno != EINTR) fprintf(stderr, "play: /dev/audio: %s\n", strerror(errno));
            return false;
        }
        p += w;
        left -= (size_t)w;
    }
    return true;
}

static void clock_text(char *s, uint64_t ms) {
    sprintf(s, "%d:%02d", (int)(ms / 60000), (int)(ms / 1000 % 60));
}

static int play(const char *path, bool quiet) {
    const char *err = wav_open(&wav, path);
    if (err) {
        fprintf(stderr, "play: %s: %s\n", path, err);
        return 1;
    }
    bool show = !quiet && isatty(1);
    char t1[16], t2[16];
    clock_text(t2, wav_ms(&wav, wav.frames));
    if (!quiet)
        printf("%s: %u Hz, %d-bit%s, %s, %s\n", path, wav.rate, wav.bits, wav.tag == 3 ? " float" : "",
               wav.channels == 1 ? "mono" : wav.channels == 2 ? "stereo" : "multichannel", t2);
    static int16_t obuf[2 * 1024];
    uint64_t played = 0, shown = ~0ull;
    int rc = 0, n;
    while ((n = wav_read(&wav, obuf, 1024)) > 0) {
        if (!out(obuf, n)) {
            rc = 1;
            break;
        }
        played += (uint64_t)n;
        uint64_t now = played * 1000 / SOUND_RATE;
        if (show && now / 250 != shown) {
            shown = now / 250;
            clock_text(t1, now);
            printf("\r  %s / %s ", t1, t2);
            fflush(stdout);
        }
    }
    if (show) printf("\n");
    wav_close(&wav);
    return rc;
}

int main(int argc, char **argv) {
    bool quiet = false;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-q")) quiet = true, first = 2;
    if (first >= argc) {
        fprintf(stderr, "usage: play [-q] file.wav ...\n");
        return 2;
    }
    out_fd = open("/dev/audio", O_WRONLY);
    if (out_fd < 0) {
        fprintf(stderr, "play: /dev/audio: %s\n", strerror(errno));
        return 1;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) rc |= play(argv[i], quiet);
    close(out_fd);
    return rc;
}
