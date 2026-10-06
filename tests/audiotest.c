/* audiotest: /dev/audio and /dev/volume, beep and play. Prints "audiotest: N failed".
   The sound goes to the card; scripts/test.py records it (QEMU's wav backend) and checks that
   these tones came out, in this order, with silence between them:
     1 s    1000 Hz left, 1500 Hz right   (written here, straight to /dev/audio)
     0.5 s  600 Hz        8-bit mono, 22050 Hz           (play)
     0.5 s  700/900 Hz    16-bit stereo, 44100 Hz        (play)
     0.5 s  800 Hz        24-bit mono, 96000 Hz          (play)
     0.5 s  500/1100 Hz   32-bit float stereo, 48000 Hz, WAVE_FORMAT_EXTENSIBLE (play)
     0.4 s  440 Hz        beep -l 400
   and then a beep that is killed early. Everything else written here is silence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include "nocturne.h"

#define PI 3.14159265358979323846

static int failed;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF), put16(p + 2, v >> 16); }

/* a WAV file of `secs` seconds: tone fl on the left (or the only) channel, fr on the right */
static void make_wav(const char *path, int tag, int bits, int ch, uint32_t rate, double fl, double fr, double secs,
                     bool extensible) {
    uint32_t frames = (uint32_t)(rate * secs), blk = (uint32_t)(ch * bits / 8), data = frames * blk;
    int fmtlen = extensible ? 40 : 16;
    uint8_t h[12 + 8 + 40 + 8 + 8];
    size_t n = 0;
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVE", 4);
    n = 12;
    memcpy(h + n, "LIST", 4), put32(h + n + 4, 0), n += 8; /* an empty chunk to skip */
    memcpy(h + n, "fmt ", 4), put32(h + n + 4, (uint32_t)fmtlen), n += 8;
    put16(h + n, extensible ? 0xFFFE : (unsigned)tag), put16(h + n + 2, (unsigned)ch), put32(h + n + 4, rate);
    put32(h + n + 8, rate * blk), put16(h + n + 12, blk), put16(h + n + 14, (unsigned)bits);
    if (extensible) {
        memset(h + n + 16, 0, 24);
        put16(h + n + 16, 22), put16(h + n + 18, (unsigned)bits), put32(h + n + 20, 3);
        put16(h + n + 24, (unsigned)tag); /* the subformat GUID starts with the tag */
    }
    n += (size_t)fmtlen;
    memcpy(h + n, "data", 4), put32(h + n + 4, data), n += 8;
    put32(h + 4, (uint32_t)(n - 8 + data));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    write_all(fd, h, n);
    uint8_t *buf = malloc(data);
    for (uint32_t i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) {
            double v = 0.7 * sin(2 * PI * (c ? fr : fl) * i / rate);
            uint8_t *p = buf + i * blk + c * bits / 8;
            if (tag == 3) {
                float f = (float)v;
                memcpy(p, &f, 4);
            } else if (bits == 8) {
                p[0] = (uint8_t)(128 + (int)(v * 127));
            } else {
                int32_t s = (int32_t)(v * 2147483647.0);
                for (int b = 0; b < bits / 8; b++) p[b] = (uint8_t)(s >> (32 - bits + 8 * b));
            }
        }
    write_all(fd, buf, data);
    free(buf);
    close(fd);
}

static int run_quiet(char *const argv[]) {
    int nul = open("/dev/null", O_RDWR);
    int fdmap[3] = {nul, nul, nul};
    int pid = spawn(argv[0], argv, fdmap, 0);
    close(nul);
    if (pid < 0) return -1;
    int st = -1;
    waitpid(pid, &st, 0);
    return st;
}

static int volume_get(void) {
    int fd = open("/dev/volume", O_RDONLY);
    char b[8] = {0};
    read(fd, b, sizeof b - 1);
    close(fd);
    return atoi(b);
}
static int volume_put(const char *s) {
    int fd = open("/dev/volume", O_WRONLY);
    int r = (int)write(fd, s, strlen(s));
    close(fd);
    return r;
}

int main(void) {
    /* the marker tone, with the time it takes to play */
    int fd = open("/dev/audio", O_RDWR);
    CHECK(fd >= 0, "open /dev/audio: %d", errno);
    static int16_t tone[48000 * 2];
    for (int i = 0; i < 48000; i++) {
        tone[2 * i] = (int16_t)(20000 * sin(2 * PI * 1000 * i / 48000));
        tone[2 * i + 1] = (int16_t)(20000 * sin(2 * PI * 1500 * i / 48000));
    }
    uint64_t t0 = uptime_ms();
    CHECK(write_all(fd, tone, sizeof tone), "writing the tone: %d", errno);
    uint32_t q = 0;
    CHECK(read(fd, &q, 4) == 4 && q > 0 && q <= 8192, "queued frames after writing: %u", q);
    close(fd);
    uint64_t took = uptime_ms() - t0;
    CHECK(took >= 950 && took <= 1400, "1 s of sound took %lu ms to play", (unsigned long)took);
    printf("1 s of sound played in %lu ms\n", (unsigned long)took);
    msleep(300);

    /* a writer that does not block: the stream takes 8192 frames at most */
    fd = open("/dev/audio", O_WRONLY | O_NONBLOCK);
    static uint8_t zero[65536];
    ssize_t w = write(fd, zero, sizeof zero);
    CHECK(w > 0 && w <= 32768, "a non-blocking write took %ld bytes", (long)w);
    ssize_t w2 = write(fd, zero, sizeof zero);
    CHECK(w2 < 0 && errno == EAGAIN, "the full stream's write: %ld (errno %d)", (long)w2, errno);
    CHECK(read(fd, &q, 4) < 0 && errno == EBADF, "reading a write-only stream");
    struct n_pollfd pf = {fd, N_POLLOUT, 0};
    t0 = uptime_ms();
    int pr = poll(&pf, 1, 1000);
    CHECK(pr == 1 && (pf.revents & N_POLLOUT) && uptime_ms() - t0 < 500, "poll for room: %d after %lu ms", pr,
          (unsigned long)(uptime_ms() - t0));
    close(fd);

    /* eight streams at once, then no more */
    int fds[9];
    for (int i = 0; i < 9; i++) fds[i] = open("/dev/audio", O_WRONLY);
    for (int i = 0; i < 8; i++) CHECK(write(fds[i], zero, 4800) == 4800, "stream %d", i);
    CHECK(write(fds[8], zero, 4800) < 0 && errno == EBUSY, "a ninth stream: errno %d", errno);
    t0 = uptime_ms();
    for (int i = 0; i < 9; i++) close(fds[i]);
    CHECK(uptime_ms() - t0 < 400, "closing eight streams took %lu ms", (unsigned long)(uptime_ms() - t0));

    /* the master volume */
    int vol = volume_get();
    CHECK(vol == 80, "the volume starts at %d", vol);
    volume_put("35\n");
    CHECK(volume_get() == 35, "volume after writing 35: %d", volume_get());
    volume_put("250");
    CHECK(volume_get() == 100, "volume after writing 250: %d", volume_get());
    CHECK(volume_put("loud") < 0 && errno == EINVAL, "writing a word to /dev/volume");
    char *vargs[] = {"/bin/volume", "-30", NULL};
    CHECK(run_quiet(vargs) == 0 && volume_get() == 70, "volume -30: %d", volume_get());

    /* a melody, at volume 0 so that the recording stays as described above */
    volume_put("0");
    char *margs[] = {"/bin/beep", "-l", "50", "C4", "E4", "G4", "C5:100", "R", "Bb3", NULL};
    t0 = uptime_ms();
    CHECK(run_quiet(margs) == 0, "beep with a melody failed");
    took = uptime_ms() - t0;
    CHECK(took >= 330 && took <= 700, "a 350 ms melody took %lu ms", (unsigned long)took);
    volume_put("80");

    /* play, in every format it knows */
    static const struct {
        const char *name;
        int tag, bits, ch;
        uint32_t rate;
        double fl, fr;
        bool ext;
    } wavs[] = {
        {"/home/t8.wav", 1, 8, 1, 22050, 600, 600, false},
        {"/home/t16.wav", 1, 16, 2, 44100, 700, 900, false},
        {"/home/t24.wav", 1, 24, 1, 96000, 800, 800, false},
        {"/home/tf.wav", 3, 32, 2, 48000, 500, 1100, true},
    };
    for (int i = 0; i < 4; i++) {
        make_wav(wavs[i].name, wavs[i].tag, wavs[i].bits, wavs[i].ch, wavs[i].rate, wavs[i].fl, wavs[i].fr, 0.5,
                 wavs[i].ext);
        char *pargs[] = {"/bin/play", "-q", (char *)wavs[i].name, NULL};
        t0 = uptime_ms();
        int st = run_quiet(pargs);
        took = uptime_ms() - t0;
        CHECK(st == 0, "play %s: exit %d", wavs[i].name, st);
        CHECK(took >= 450 && took <= 900, "play %s took %lu ms", wavs[i].name, (unsigned long)took);
        msleep(300);
    }
    int bad = open("/home/bad.wav", O_WRONLY | O_CREAT | O_TRUNC);
    write_all(bad, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x55\0\x02\0", 22);
    close(bad);
    char *badargs[] = {"/bin/play", "/home/bad.wav", NULL};
    CHECK(run_quiet(badargs) != 0, "play accepted a broken file");

    /* beep: a plain tone, bad arguments, and being killed while it plays */
    char *bargs[] = {"/bin/beep", "-l", "400", NULL};
    t0 = uptime_ms();
    CHECK(run_quiet(bargs) == 0, "beep failed");
    took = uptime_ms() - t0;
    CHECK(took >= 380 && took <= 800, "beep -l 400 took %lu ms", (unsigned long)took);
    char *nargs[] = {"/bin/beep", "H9", NULL};
    CHECK(run_quiet(nargs) == 2, "beep H9 should be refused");
    msleep(300);
    int nul = open("/dev/null", O_RDWR);
    int fdmap[3] = {nul, nul, nul};
    char *largs[] = {"/bin/beep", "-l", "10000", "-f", "300", NULL};
    int pid = spawn("/bin/beep", largs, fdmap, 0);
    close(nul);
    msleep(300);
    t0 = uptime_ms();
    kill(pid);
    int st;
    waitpid(pid, &st, 0);
    CHECK(uptime_ms() - t0 < 300, "a killed beep took %lu ms to go", (unsigned long)(uptime_ms() - t0));

    printf("audiotest: %d failed\n", failed);
    return failed != 0;
}
