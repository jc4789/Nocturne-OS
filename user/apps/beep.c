/* beep: play tones on /dev/audio.
   beep [-f hz] [-l ms] [-w sine|square|triangle|saw] [-v 0-100] [note[:ms]...]
   Notes are names like C4, F#5, Bb3 or R (a rest); a melody is a list of them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include "nocturne.h"

#define RATE 48000
#define PI 3.14159265358979323846

static int wave = 0; /* 0 sine, 1 square, 2 triangle, 3 saw */
static double amp = 0.5;
static int fd;
static int16_t buf[2 * 960];

static double shape(double ph) { /* ph in [0, 1) */
    switch (wave) {
    case 1: return ph < 0.5 ? 1 : -1;
    case 2: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
    case 3: return 2 * ph - 1;
    default: return sin(2 * PI * ph);
    }
}

static bool tone(double hz, int ms) {
    long frames = (long)ms * RATE / 1000;
    long fade = MIN(frames / 4, RATE / 200); /* 5 ms ramps: no clicks */
    double ph = 0, step = hz / RATE;
    for (long done = 0; done < frames;) {
        int n = (int)MIN(frames - done, 960L);
        for (int i = 0; i < n; i++) {
            long k = done + i;
            double env = 1;
            if (k < fade) env = (double)k / fade;
            else if (frames - k < fade) env = (double)(frames - k) / fade;
            double v = hz > 0 ? shape(ph) * amp * env : 0;
            ph += step;
            if (ph >= 1) ph -= 1;
            buf[2 * i] = buf[2 * i + 1] = (int16_t)(v * (wave == 1 || wave == 3 ? 0.5 : 1) * 32767);
        }
        const char *p = (const char *)buf;
        size_t left = (size_t)n * 4;
        while (left) {
            ssize_t w = write(fd, p, left);
            if (w < 0) {
                if (errno != EINTR) fprintf(stderr, "beep: /dev/audio: %s\n", strerror(errno));
                return false;
            }
            p += w;
            left -= (size_t)w;
        }
        done += n;
    }
    return true;
}

/* "A4" -> 440; "R" -> 0; -1 if it is not a note */
static double note_hz(const char *s) {
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G from C */
    char c = (char)(s[0] & ~0x20);
    if (c == 'R' && (!s[1] || s[1] == ':')) return 0;
    if (c < 'A' || c > 'G') return -1;
    int semi = semis[c - 'A'];
    s++;
    if (*s == '#') semi++, s++;
    else if (*s == 'b') semi--, s++;
    int oct = 4;
    if (*s >= '0' && *s <= '9') oct = *s++ - '0';
    if (*s && *s != ':') return -1;
    int midi = (oct + 1) * 12 + semi;
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

static void usage(void) {
    fprintf(stderr, "usage: beep [-f hz] [-l ms] [-w sine|square|triangle|saw] [-v 0-100] [note[:ms] ...]\n"
                    "  notes: C4 F#5 Bb3 ... and R for a rest, e.g. beep -l 150 C4 E4 G4 C5:400\n");
}

int main(int argc, char **argv) {
    double hz = 440;
    int ms = 200, first_note = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc) hz = atof(argv[++i]);
        else if (!strcmp(a, "-l") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!strcmp(a, "-v") && i + 1 < argc) amp = MAX(0, MIN(100, atoi(argv[++i]))) / 100.0;
        else if (!strcmp(a, "-w") && i + 1 < argc) {
            const char *w = argv[++i];
            static const char *names[] = {"sine", "square", "triangle", "saw"};
            wave = -1;
            for (int k = 0; k < 4; k++)
                if (!strcmp(w, names[k])) wave = k;
            if (wave < 0) {
                usage();
                return 2;
            }
        } else if (a[0] == '-' && a[1]) {
            usage();
            return 2;
        } else {
            first_note = i;
            break;
        }
    }
    if (hz < 0 || hz > 20000 || ms < 0 || ms > 60000) {
        usage();
        return 2;
    }
    for (int i = first_note; i < argc; i++)
        if (note_hz(argv[i]) < 0) {
            fprintf(stderr, "beep: %s: not a note\n", argv[i]);
            return 2;
        }
    fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "beep: /dev/audio: %s\n", strerror(errno));
        return 1;
    }
    bool ok = true;
    if (first_note == argc) ok = tone(hz, ms);
    for (int i = first_note; i < argc && ok; i++) {
        const char *colon = strchr(argv[i], ':');
        ok = tone(note_hz(argv[i]), colon ? atoi(colon + 1) : ms);
    }
    close(fd); /* waits for the sound to finish */
    return ok ? 0 : 1;
}
