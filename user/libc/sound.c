/* Sound effects for programs that must not wait (games): see nocturne.h. */
#include <math.h>
#include "nocturne.h"

#define PI 3.14159265358979323846

static int fd = -2;
static uint32_t noise_state = 0x9E3779B9;
static int16_t buf[2 * SOUND_QUEUE];

bool sound_effect(int hz, int hz2, int ms, int wave) {
    if (fd == -2) fd = open("/dev/audio", O_RDWR | O_NONBLOCK);
    if (fd < 0 || hz <= 0 || ms <= 0) return false;
    int frames = ms * (SOUND_RATE / 1000);
    uint32_t queued = 0;
    if (frames > SOUND_QUEUE || read(fd, &queued, 4) != 4 || queued + (uint32_t)frames > SOUND_QUEUE) return false;
    if (hz2 <= 0) hz2 = hz;
    double ph = 0, ratio = log((double)hz2 / hz), held = 0;
    int attack = SOUND_RATE / 400, release = frames * 3 / 10; /* 2.5 ms in, 30% out: no clicks */
    for (int i = 0; i < frames; i++) {
        double t = (double)i / frames, f = hz * exp(ratio * t);
        ph += f / SOUND_RATE;
        ph -= (int)ph;
        double v;
        switch (wave) {
        case SND_SQUARE: v = ph < 0.5 ? 0.5 : -0.5; break;
        case SND_TRIANGLE: v = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; break;
        case SND_SAW: v = ph - 0.5; break;
        case SND_NOISE: /* a new random level every period: the pitch colours the noise */
            if (ph < f / SOUND_RATE) {
                noise_state = noise_state * 1664525 + 1013904223;
                held = (int32_t)noise_state / 2147483648.0 * 0.6;
            }
            v = held;
            break;
        default: v = sin(2 * PI * ph);
        }
        double env = 1;
        if (i < attack) env = (double)i / attack;
        else if (frames - i < release) env = (double)(frames - i) / release;
        int16_t s = (int16_t)(v * env * 0.4 * 32767);
        buf[2 * i] = buf[2 * i + 1] = s;
    }
    return write(fd, buf, (size_t)frames * 4) == frames * 4;
}
