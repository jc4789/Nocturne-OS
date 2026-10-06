/* Reading WAV files as 48 kHz stereo: see nocturne.h. */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool rd(int fd, void *p, size_t n) {
    uint8_t *d = p;
    while (n) {
        ssize_t r = read(fd, d, n);
        if (r <= 0) return false;
        d += r;
        n -= (size_t)r;
    }
    return true;
}

const char *wav_open(struct wav *w, const char *path) {
    memset(w, 0, sizeof *w);
    w->fd = open(path, O_RDONLY);
    if (w->fd < 0) return "cannot open the file";
    uint8_t h[12], ch[8], fmt[40];
    const char *err = "not a WAV file";
    if (!rd(w->fd, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto fail;
    uint32_t off = 12;
    for (;;) { /* walk the chunks up to the data */
        err = "no sound in the file";
        if (!rd(w->fd, ch, 8)) goto fail;
        uint32_t len = le32(ch + 4);
        off += 8;
        if (!memcmp(ch, "data", 4)) {
            w->data_off = off;
            w->data_len = len;
            break;
        }
        uint32_t keep = !memcmp(ch, "fmt ", 4) ? MIN(len, (uint32_t)sizeof fmt) : 0;
        if (keep && !rd(w->fd, fmt, keep)) goto fail;
        if (keep >= 16) {
            w->tag = le16(fmt);
            w->channels = le16(fmt + 2);
            w->rate = le32(fmt + 4);
            w->block = le16(fmt + 12);
            w->bits = le16(fmt + 14);
            if (w->tag == 0xFFFE && keep >= 26) w->tag = le16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE */
        }
        off += len + (len & 1); /* chunks are padded to even sizes */
        if (lseek(w->fd, (long)off, SEEK_SET) < 0) goto fail;
    }
    bool ok_bits = w->tag == 1 ? (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)
                               : w->tag == 3 && w->bits == 32;
    err = "a kind of WAV this cannot play";
    if (!ok_bits || w->channels < 1 || w->rate < 1000 || w->rate > 384000 ||
        w->block < w->channels * w->bits / 8)
        goto fail;
    struct n_stat st;
    if (fstat(w->fd, &st) == 0 && st.size > w->data_off && w->data_len > st.size - w->data_off)
        w->data_len = (uint32_t)(st.size - w->data_off); /* a recording that was cut short */
    w->frames = w->data_len / (uint32_t)w->block;
    w->step = ((uint64_t)w->rate << 32) / SOUND_RATE;
    wav_seek(w, 0);
    return NULL;
fail:
    close(w->fd);
    w->fd = -1;
    return err;
}

void wav_close(struct wav *w) {
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
}

uint32_t wav_ms(const struct wav *w, uint32_t frame) { return (uint32_t)((uint64_t)frame * 1000 / w->rate); }

/* one sample, as a 16-bit value */
static int32_t sample(const struct wav *w, const uint8_t *p) {
    switch (w->bits) {
    case 8: return ((int32_t)p[0] - 128) << 8;
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)le16(p + 1);
    default:
        if (w->tag == 3) {
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
static bool next_frame(struct wav *w, int32_t *l, int32_t *r) {
    if (w->pos >= w->frames) return false;
    if (w->len - w->at < (size_t)w->block) {
        memmove(w->buf, w->buf + w->at, w->len - w->at);
        w->len -= w->at;
        w->at = 0;
        size_t want = MIN(sizeof w->buf - w->len, (size_t)(w->frames - w->pos) * (size_t)w->block);
        while (w->len < (size_t)w->block && want) {
            ssize_t got = read(w->fd, w->buf + w->len, want);
            if (got <= 0) {
                w->frames = w->pos; /* the file ended early */
                return false;
            }
            w->len += (size_t)got;
            want -= (size_t)got;
        }
        if (w->len < (size_t)w->block) return false;
    }
    const uint8_t *p = w->buf + w->at;
    w->at += (size_t)w->block;
    w->pos++;
    *l = sample(w, p);
    *r = w->channels > 1 ? sample(w, p + w->bits / 8) : *l;
    return true;
}

void wav_seek(struct wav *w, uint32_t frame) {
    w->pos = MIN(frame, w->frames);
    w->len = w->at = 0;
    w->frac = 0;
    w->eof = false;
    w->src = w->pos;
    lseek(w->fd, (long)(w->data_off + (uint64_t)w->pos * (uint32_t)w->block), SEEK_SET);
    if (!next_frame(w, &w->al, &w->ar)) {
        w->eof = true;
        return;
    }
    if (!next_frame(w, &w->bl, &w->br)) w->bl = w->al, w->br = w->ar;
}

/* linear resampling: each output frame lies `frac` of the way from source frame a to b */
int wav_read(struct wav *w, int16_t *out, int frames) {
    int n = 0;
    while (n < frames && !w->eof) {
        int64_t f = (int64_t)(w->frac >> 16);
        out[2 * n] = (int16_t)(w->al + (((w->bl - w->al) * f) >> 16));
        out[2 * n + 1] = (int16_t)(w->ar + (((w->br - w->ar) * f) >> 16));
        n++;
        for (w->frac += w->step; w->frac >> 32; w->frac -= 1ull << 32) {
            w->al = w->bl, w->ar = w->br;
            w->src++;
            if (!next_frame(w, &w->bl, &w->br)) {
                w->eof = true;
                break;
            }
        }
    }
    return n;
}
