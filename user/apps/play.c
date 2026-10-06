/* play: play WAV files on /dev/audio.
   Integer PCM of 8, 16, 24 or 32 bits and 32-bit float, any number of channels (mono is sent to
   both speakers; past two, the first two are played) and any rate (resampled to 48 kHz). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

#define OUT_RATE 48000

static int in_fd, out_fd;
static uint8_t ibuf[16384];
static size_t ilen, ipos;
static uint32_t data_left; /* bytes of the data chunk still unread */
static int fmt_tag, channels, bits, block;
static uint32_t rate;

static bool rd(void *p, size_t n) {
    uint8_t *d = p;
    while (n) {
        ssize_t r = read(in_fd, d, n);
        if (r <= 0) return false;
        d += r;
        n -= (size_t)r;
    }
    return true;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* one sample, as a 16-bit value */
static int32_t sample(const uint8_t *p) {
    switch (bits) {
    case 8: return ((int32_t)p[0] - 128) << 8;
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)le16(p + 1);
    default:
        if (fmt_tag == 3) {
            uint32_t u = le32(p);
            float f;
            memcpy(&f, &u, 4);
            if (!(f == f)) f = 0;
            f = f > 1 ? 1 : f < -1 ? -1 : f;
            return (int32_t)(f * 32767);
        }
        return (int16_t)le16(p + 2);
    }
}

/* the next source frame; false at the end of the data */
static bool next_frame(int32_t *l, int32_t *r) {
    if (ilen - ipos < (size_t)block) {
        memmove(ibuf, ibuf + ipos, ilen - ipos);
        ilen -= ipos;
        ipos = 0;
        size_t want = MIN(sizeof ibuf - ilen, (size_t)data_left);
        while (want && ilen < (size_t)block) {
            ssize_t got = read(in_fd, ibuf + ilen, want);
            if (got <= 0) {
                data_left = 0;
                break;
            }
            ilen += (size_t)got;
            data_left -= (uint32_t)got;
            want -= (size_t)got;
        }
        if (ilen < (size_t)block) return false;
    }
    const uint8_t *p = ibuf + ipos;
    ipos += (size_t)block;
    *l = sample(p);
    *r = channels > 1 ? sample(p + bits / 8) : *l;
    return true;
}

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
    in_fd = !strcmp(path, "-") ? 0 : open(path, O_RDONLY);
    if (in_fd < 0) {
        fprintf(stderr, "play: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int rc = 1;
    uint8_t h[12], ch[8], fmt[40];
    if (!rd(h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "play: %s: not a WAV file\n", path);
        goto done;
    }
    fmt_tag = 0;
    for (;;) { /* walk the chunks up to the data */
        if (!rd(ch, 8)) {
            fprintf(stderr, "play: %s: no sound data\n", path);
            goto done;
        }
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "data", 4)) {
            data_left = len;
            break;
        }
        uint32_t keep = !memcmp(ch, "fmt ", 4) ? MIN(len, (uint32_t)sizeof fmt) : 0;
        if (keep && !rd(fmt, keep)) goto done;
        if (keep >= 16) {
            fmt_tag = le16(fmt);
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            block = le16(fmt + 12);
            bits = le16(fmt + 14);
            if (fmt_tag == 0xFFFE && keep >= 26) fmt_tag = le16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE */
        }
        for (uint32_t skip = len - keep + (len & 1); skip;) { /* chunks are padded to even sizes */
            uint8_t junk[256];
            uint32_t n = MIN(skip, (uint32_t)sizeof junk);
            if (!rd(junk, n)) goto done;
            skip -= n;
        }
    }
    bool ok_bits = fmt_tag == 1 ? (bits == 8 || bits == 16 || bits == 24 || bits == 32) : fmt_tag == 3 && bits == 32;
    if (!ok_bits || channels < 1 || rate < 1000 || rate > 384000 || block < channels * bits / 8) {
        fprintf(stderr, "play: %s: unsupported format (tag %d, %d-bit, %d channels, %u Hz)\n", path, fmt_tag, bits,
                channels, rate);
        goto done;
    }
    uint64_t total_ms = (uint64_t)data_left / (uint32_t)block * 1000 / rate;
    bool show = !quiet && isatty(1);
    char t1[16], t2[16];
    clock_text(t2, total_ms);
    if (!quiet)
        printf("%s: %u Hz, %d-bit%s, %s, %s\n", path, rate, bits, fmt_tag == 3 ? " float" : "",
               channels == 1 ? "mono" : channels == 2 ? "stereo" : "multichannel", t2);
    ilen = ipos = 0;

    /* linear resampling: each output frame lies `frac` of the way from source frame a to b */
    int32_t al, ar, bl, br;
    if (!next_frame(&al, &ar)) {
        rc = 0;
        goto done;
    }
    if (!next_frame(&bl, &br)) bl = al, br = ar;
    uint64_t step = ((uint64_t)rate << 32) / OUT_RATE, frac = 0, played = 0, shown = ~0ull;
    bool eof = false;
    static int16_t obuf[2 * 1024];
    while (!eof) {
        int n = 0;
        while (n < 1024 && !eof) {
            int64_t f = (int64_t)(frac >> 16);
            obuf[2 * n] = (int16_t)(al + (((bl - al) * f) >> 16));
            obuf[2 * n + 1] = (int16_t)(ar + (((br - ar) * f) >> 16));
            n++;
            for (frac += step; frac >> 32; frac -= 1ull << 32) {
                al = bl, ar = br;
                if (!next_frame(&bl, &br)) {
                    eof = true;
                    break;
                }
            }
        }
        if (!out(obuf, n)) goto done;
        played += (uint64_t)n;
        uint64_t now = played * 1000 / OUT_RATE;
        if (show && now / 250 != shown) {
            shown = now / 250;
            clock_text(t1, now);
            printf("\r  %s / %s ", t1, t2);
            fflush(stdout);
        }
    }
    if (show) printf("\n");
    rc = 0;
done:
    if (in_fd > 0) close(in_fd);
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
