/* audiotest: sound streams and the master volume, beep, play and the Sound Player. Prints
   "audiotest: N failed". The sound goes to the card; scripts/test.py records it (QEMU's wav
   backend) and checks that these tones came out, in this order, with silence between them:
     1 s    1000 Hz left, 1500 Hz right   (submitted here, straight to a stream)
     0.5 s  600 Hz        8-bit mono, 22050 Hz           (play)
     0.5 s  700/900 Hz    16-bit stereo, 44100 Hz        (play)
     0.5 s  800 Hz        24-bit mono, 96000 Hz          (play)
     0.5 s  500/1100 Hz   32-bit float stereo, 48000 Hz, WAVE_FORMAT_EXTENSIBLE (play)
     0.4 s  440 Hz        beep -l 400
     0.6 s  1200/750 Hz   16-bit stereo, 32000 Hz: the Sound Player, started as Files starts it
   and then a beep that is stopped early. Everything else submitted here is silence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

/* the system's programs this test runs: complete paths, as n_app_launch takes them */
#define VOLUME N_FOLDER_SYSTEM "\\volume"
#define BEEP N_FOLDER_SYSTEM "\\beep"
#define PLAY N_FOLDER_SYSTEM "\\play"

/* the complete path of a document or folder of this test, inside folder (the temporary folder when it is
   NULL); empty when it does not fit, which fails whatever is done with it */
static const char *in_temp(const char *folder, const char *leaf, char *out, size_t cap) {
    if (n_path_join(folder ? folder : n_temp_folder(), leaf, out, cap) < 0) out[0] = 0;
    return out;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF), put16(p + 2, v >> 16); }

/* a WAV file of `secs` seconds: tone fl on the left (or the only) channel, fr on the right */
static void make_wav(const char *path, int tag, int bits, int ch, uint32_t rate, double fl, double fr, double secs,
                     bool extensible) {
    uint32_t frames = (uint32_t)(rate * secs), blk = (uint32_t)(ch * bits / 8), data = frames * blk;
    int fmtlen = extensible ? 40 : 16;
    uint8_t *h = malloc(12 + 8 + 40 + 8 + 8 + data);
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
    uint8_t *buf = h + n;
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
    CHECK(n_document_save(path, h, n + data) >= 0, "saving %s", path);
    free(h);
}

/* an app with no input or output */
static n_handle start_quiet(const char *path, const char *const *args, uint32_t argc) {
    int32_t handles[3] = {-1, -1, -1};
    struct n_launch l = {.arguments = args, .handles = handles, .argument_count = argc, .handle_count = 3};
    return n_app_launch(path, &l);
}

static int run_quiet(const char *const *args, uint32_t argc) {
    n_handle app = start_quiet(args[0], args, argc);
    if (app < 0) return -1;
    int st = -1;
    n_app_result(app, -1, &st);
    n_handle_close(app);
    return st;
}

/* submit all of frames, waiting for room */
static bool submit_all(n_handle a, const int16_t *frames, size_t count) {
    while (count) {
        n_result r = n_audio_submit(a, frames, count, N_AUDIO_WAIT);
        if (r <= 0) return false;
        frames += 2 * r;
        count -= (size_t)r;
    }
    return true;
}

int main(void) {
    /* the marker tone, with the time it takes to play */
    n_handle a = n_audio_open();
    CHECK(a >= 0, "n_audio_open: %s", n_result_text(a));
    CHECK(n_handle_kind(a) == N_KIND_DEVICE, "a sound stream's kind: %d", (int)n_handle_kind(a));
    static int16_t tone[48000 * 2];
    for (int i = 0; i < 48000; i++) {
        tone[2 * i] = (int16_t)(20000 * sin(2 * PI * 1000 * i / 48000));
        tone[2 * i + 1] = (int16_t)(20000 * sin(2 * PI * 1500 * i / 48000));
    }
    uint64_t t0 = uptime_ms();
    CHECK(submit_all(a, tone, 48000), "submitting the tone");
    struct n_audio_status st;
    CHECK(n_audio_status(a, &st) == 0 && st.queued_frames > 0 && st.queued_frames <= 8192 &&
              st.committed_frames == 48000,
          "after submitting: %u queued, %llu committed", st.queued_frames, (unsigned long long)st.committed_frames);
    n_handle_close(a);
    uint64_t took = uptime_ms() - t0;
    CHECK(took >= 950 && took <= 1400, "1 s of sound took %lu ms to play", (unsigned long)took);
    printf("1 s of sound played in %lu ms\n", (unsigned long)took);
    n_sleep_ms(300);

    /* a submitter that does not wait: the stream takes 8192 frames at most */
    a = n_audio_open();
    static int16_t zero[16384 * 2];
    n_result w = n_audio_submit(a, zero, 16384, 0);
    CHECK(w > 0 && w <= 8192, "a submit without waiting took %ld frames", (long)w);
    n_result w2 = n_audio_submit(a, zero, 16384, 0);
    CHECK(w2 == -N_ERR_NOT_READY, "the full stream's submit: %ld", (long)w2);
    CHECK(n_audio_submit(a, zero, 1, 2) == -N_ERR_INVALID, "an unknown submit flag");
    n_handle q = n_events_create();
    n_events_watch(q, a, N_EVENT_SENDABLE, 7);
    struct n_event ev;
    t0 = uptime_ms();
    n_result got = n_events_wait(q, &ev, 1, 1000);
    CHECK(got == 1 && ev.token == 7 && (ev.events & N_EVENT_SENDABLE) && uptime_ms() - t0 < 500,
          "waiting for room: %ld after %lu ms", (long)got, (unsigned long)(uptime_ms() - t0));
    n_handle_close(q);
    CHECK(n_audio_flush(a) == 0 && n_audio_status(a, &st) == 0 && st.queued_frames == 0, "a flushed stream holds %u",
          st.queued_frames);
    n_handle_close(a);
    CHECK(n_audio_submit(0, zero, 1, 0) < 0, "a submit to a handle that is not a sound stream");

    /* eight streams at once, then no more */
    n_handle as[9];
    for (int i = 0; i < 9; i++) as[i] = n_audio_open();
    for (int i = 0; i < 8; i++) CHECK(n_audio_submit(as[i], zero, 1200, 0) == 1200, "stream %d", i);
    CHECK(as[8] == -N_ERR_BUSY, "a ninth stream: %s", n_result_text(as[8]));
    t0 = uptime_ms();
    for (int i = 0; i < 9; i++)
        if (as[i] >= 0) n_handle_close(as[i]);
    CHECK(uptime_ms() - t0 < 400, "closing eight streams took %lu ms", (unsigned long)(uptime_ms() - t0));

    /* the master volume */
    n_result vol = n_audio_volume();
    CHECK(vol == 80, "the volume starts at %ld", (long)vol);
    CHECK(n_audio_set_volume(35) == 35 && n_audio_volume() == 35, "volume after setting 35: %ld", (long)n_audio_volume());
    CHECK(n_audio_set_volume(250) == -N_ERR_INVALID && n_audio_volume() == 35, "setting 250: %ld",
          (long)n_audio_volume());
    CHECK(n_audio_set_volume(-1) == -N_ERR_INVALID, "setting -1");
    n_audio_set_volume(100);
    const char *vargs[] = {VOLUME, "-30"};
    CHECK(run_quiet(vargs, 2) == 0 && n_audio_volume() == 70, "volume -30: %ld", (long)n_audio_volume());

    /* a melody, at volume 0 so that the recording stays as described above */
    n_audio_set_volume(0);
    const char *margs[] = {BEEP, "-l", "50", "C4", "E4", "G4", "C5:100", "R", "Bb3"};
    t0 = uptime_ms();
    CHECK(run_quiet(margs, 9) == 0, "beep with a melody failed");
    took = uptime_ms() - t0;
    CHECK(took >= 330 && took <= 700, "a 350 ms melody took %lu ms", (unsigned long)took);
    n_audio_set_volume(80);

    /* play, in every format it knows */
    static const struct {
        const char *name;
        int tag, bits, ch;
        uint32_t rate;
        double fl, fr;
        bool ext;
    } wavs[] = {
        {"t8.wav", 1, 8, 1, 22050, 600, 600, false},
        {"t16.wav", 1, 16, 2, 44100, 700, 900, false},
        {"t24.wav", 1, 24, 1, 96000, 800, 800, false},
        {"tf.wav", 3, 32, 2, 48000, 500, 1100, true},
    };
    char wav[4096];
    for (int i = 0; i < 4; i++) {
        in_temp(NULL, wavs[i].name, wav, sizeof wav);
        make_wav(wav, wavs[i].tag, wavs[i].bits, wavs[i].ch, wavs[i].rate, wavs[i].fl, wavs[i].fr, 0.5,
                 wavs[i].ext);
        const char *pargs[] = {PLAY, "-q", wav};
        t0 = uptime_ms();
        int rc = run_quiet(pargs, 3);
        took = uptime_ms() - t0;
        CHECK(rc == 0, "play %s: exit %d", wavs[i].name, rc);
        CHECK(took >= 450 && took <= 900, "play %s took %lu ms", wavs[i].name, (unsigned long)took);
        n_sleep_ms(300);
        n_entry_remove(wav);
    }
    in_temp(NULL, "bad.wav", wav, sizeof wav);
    n_document_save(wav, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x55\0\x02\0", 22);
    const char *badargs[] = {PLAY, wav};
    CHECK(run_quiet(badargs, 2) != 0, "play accepted a broken file");
    n_entry_remove(wav);

    /* beep: a plain tone, bad arguments, and being stopped while it plays */
    const char *bargs[] = {BEEP, "-l", "400"};
    t0 = uptime_ms();
    CHECK(run_quiet(bargs, 3) == 0, "beep failed");
    took = uptime_ms() - t0;
    CHECK(took >= 380 && took <= 800, "beep -l 400 took %lu ms", (unsigned long)took);
    const char *nargs[] = {BEEP, "H9"};
    CHECK(run_quiet(nargs, 2) == 2, "beep H9 should be refused");
    n_sleep_ms(300);

    /* the Sound Player plays the file it is opened with (alone in its folder, so nothing follows) */
    char ptest[4096], tone_path[4096];
    in_temp(NULL, "ptest", ptest, sizeof ptest);
    n_folder_create(ptest);
    in_temp(ptest, "tone.wav", tone_path, sizeof tone_path);
    make_wav(tone_path, 1, 16, 2, 32000, 1200, 750, 0.6, false);
    int id = gui_launch("player", tone_path);
    CHECK(id > 0, "the Sound Player did not start (%d)", id);
    n_sleep_ms(1300);
    if (id > 0) {
        n_handle player = n_app_open(id);
        CHECK(player >= 0 && n_app_stop(player, 0) == 0, "the Sound Player quit by itself");
        if (player >= 0) {
            n_app_result(player, -1, NULL);
            n_handle_close(player);
        }
    }
    n_sleep_ms(300);
    const char *largs[] = {BEEP, "-l", "10000", "-f", "300"};
    n_handle beep = start_quiet(BEEP, largs, 5);
    n_sleep_ms(300);
    t0 = uptime_ms();
    n_app_stop(beep, 0);
    n_app_result(beep, -1, NULL);
    n_handle_close(beep);
    CHECK(uptime_ms() - t0 < 300, "a stopped beep took %lu ms to go", (unsigned long)(uptime_ms() - t0));

    printf("audiotest: %d failed\n", failed);
    return failed != 0;
}
