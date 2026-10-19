/* Sound Player: plays WAV files. Opened with a file (from Files) it plays it, and lists the other
   sounds in the same folder; opened on its own it shows the Music folder. The sound is read and
   resampled here and kept about 100 ms ahead of the speaker, so pausing and seeking answer at
   once. The title, seek slider, buttons and sound list are widgets of the interface engine
   (libc/ui.c); the waveform is a surface this program draws itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define W        520
#define H        500
#define MUSIC    "/home/Music"
#define AHEAD    4800 /* frames kept queued: 100 ms */
#define CHUNK    960  /* frames made at a time: 20 ms */
#define MAXT     200
#define PAD      16
#define SCOPE_W  (W - 2 * PAD)
#define SCOPE_H  58
#define SEEK_PX  400  /* the slider moves on screen in about this many steps */
#define WINDOW   1
#define SCOPE    1

static char dir[256], names[MAXT][128], error[160];
static int ntracks, cur = -1;
static n_handle stream = -1;
static struct wav wav;
static bool loaded, playing;
static uint64_t sent;     /* frames written since the track (or the last seek) started */
static uint32_t base;     /* the file frame that `sent` counts from */
static int16_t scope[CHUNK * 2];
/* the places of the widgets in the last view, and what that view showed of the time */
static uint32_t n_prev, n_play, n_stop, n_next, n_seek, n_items;
static uint32_t shown_sec = UINT32_MAX;
static int shown_px = -1;

static void join(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

static void load_dir(const char *path) {
    strlcpy(dir, path, sizeof dir);
    ntracks = 0;
    struct n_entry *e;
    n_result n = n_folder_entries(dir, &e);
    for (n_result i = 0; i < n && ntracks < MAXT; i++) {
        const char *dot = strrchr(e[i].name, '.');
        if (e[i].kind == N_KIND_DOCUMENT && dot && !strcasecmp(dot, ".wav")) strlcpy(names[ntracks++], e[i].name, 128);
    }
    if (n >= 0) free(e);
    qsort(names, ntracks, sizeof names[0], by_name);
}

/* where the speaker is now, in the file's frames: what was sent, less what is still queued */
static uint32_t now_frame(void) {
    if (!loaded) return 0;
    uint32_t queued = 0;
    struct n_audio_status st;
    if (stream >= 0 && playing && n_audio_status(stream, &st) == 0) queued = st.queued_frames;
    uint64_t heard = sent > queued ? sent - queued : 0;
    uint64_t f = base + heard * wav.rate / SOUND_RATE;
    return (uint32_t)MIN(f, (uint64_t)wav.frames);
}

static void stop(void) {
    playing = false;
    if (loaded) wav_close(&wav);
    loaded = false;
    sent = base = 0;
    memset(scope, 0, sizeof scope);
}

static void play_track(int i) {
    stop();
    error[0] = 0;
    if (i < 0 || i >= ntracks) return;
    cur = i;
    char path[512];
    join(path, sizeof path, names[i]);
    const char *err = wav_open(&wav, path);
    if (err) {
        snprintf(error, sizeof error, "%s: %s", names[i], err);
        return;
    }
    loaded = true;
    playing = stream >= 0;
}

static void seek_to(uint32_t frame) {
    if (!loaded) return;
    wav_seek(&wav, frame);
    base = frame;
    sent = 0;
}

/* keep the stream AHEAD frames ahead of the speaker; at the end, go on to the next sound */
static void pump(void) {
    if (!playing || stream < 0) return;
    uint32_t queued = 0;
    struct n_audio_status st;
    if (n_audio_status(stream, &st) < 0) return;
    queued = st.queued_frames;
    while (queued < AHEAD) {
        int n = wav_read(&wav, scope, CHUNK);
        if (n <= 0) {
            if (queued == 0) { /* the last of it has been heard */
                if (cur + 1 < ntracks) play_track(cur + 1);
                else {
                    stop();
                    cur = -1;
                }
            }
            return;
        }
        if (n < CHUNK) memset(scope + 2 * n, 0, (size_t)(CHUNK - n) * 4);
        if (n_audio_submit(stream, scope, (size_t)n, 0) <= 0) return;
        sent += (uint64_t)n;
        queued += (uint32_t)n;
    }
}

/* ---- the waveform (a surface) ---- */

static void clock_text(char *s, uint32_t ms) { sprintf(s, "%u:%02u", ms / 60000, ms / 1000 % 60); }

static void draw_scope(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, SCOPE_W, SCOPE_H, t->background);
    ui_round_fill(c, 0, 0, SCOPE_W, SCOPE_H, t->controlRadius, t->surface);
    float px = 0, py = 0;
    for (int i = 0; i < SCOPE_W - 8; i += 3) {
        int f = i * CHUNK / (SCOPE_W - 8);
        int v = (scope[2 * f] + scope[2 * f + 1]) / 2;
        float x = 4 + (float)i, y = SCOPE_H / 2 + (float)(-v * (SCOPE_H / 2 - 6)) / 32768;
        if (i) ui_line(c, px, py, x, y, 1.6f, t->success);
        px = x;
        py = y;
    }
}

/* ---- the window (widgets) ---- */

/* a transport button; `primary` makes it the accent one */
static uint32_t transport(struct ui_msg *m, const char *text, const char *icon, bool primary) {
    uint32_t n = ui_node(m, "button");
    if (text) ui_str(m, "text", text);
    if (icon) ui_str(m, "icon", icon);
    if (primary) { ui_str(m, "variant", "primary"); ui_num(m, "minWidth", 96); }
    ui_end(m);
    return n;
}

static void show(struct ui_session *s) {
    const struct ui_theme *t = &ui_theme;
    char title[160], info[160], head[300], t1[16], t2[16];
    if (loaded) {
        strlcpy(title, names[cur], sizeof title);
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
        char len[16];
        clock_text(len, wav_ms(&wav, wav.frames));
        char khz[16], *z;
        snprintf(khz, sizeof khz, "%u.%03u", wav.rate / 1000, wav.rate % 1000);
        for (z = khz + strlen(khz) - 1; *z == '0'; z--) *z = 0; /* 22.050 -> 22.05, 48.000 -> 48 */
        if (*z == '.') *z = 0;
        snprintf(info, sizeof info, "%s kHz, %d-bit%s, %s, %s", khz, wav.bits, wav.tag == 3 ? " float" : "",
                 wav.channels == 1 ? "mono" : wav.channels == 2 ? "stereo" : "surround", len);
    } else {
        strlcpy(title, "Sound Player", sizeof title);
        strlcpy(info, ntracks ? "Pick a sound below, or press Space." : "Open a WAV file from Files.", sizeof info);
    }
    const char *line = info;
    uint32_t line_colour = t->textDim;
    if (error[0]) { line = error; line_colour = t->danger; }
    else if (stream < 0) { line = "No sound: too many programs are playing."; line_colour = t->danger; }
    uint32_t at = now_frame(), at_ms = loaded ? wav_ms(&wav, at) : 0, total_ms = loaded ? wav_ms(&wav, wav.frames) : 0;
    clock_text(t1, at_ms);
    clock_text(t2, total_ms);
    snprintf(head, sizeof head, "%s  (%d sound%s)", dir, ntracks, ntracks == 1 ? "" : "s");
    shown_sec = at_ms / 1000;
    shown_px = total_ms ? (int)((uint64_t)at_ms * SEEK_PX / total_ms) : 0;
    static int revealed = -1, reveal;
    if (cur != revealed) { revealed = cur; reveal++; } /* the list scrolls to a new current track */

    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", 10);
        ui_node(&m, "column"); ui_num(&m, "gap", 2);
            ui_node(&m, "text"); ui_str(&m, "text", title); ui_str(&m, "style", "title"); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", line); ui_str(&m, "style", "caption");
            ui_num(&m, "colour", line_colour); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", SCOPE); ui_num(&m, "width", SCOPE_W);
        ui_num(&m, "height", SCOPE_H); ui_end(&m);
        ui_node(&m, "row"); ui_num(&m, "gap", 12);
            ui_node(&m, "text"); ui_str(&m, "text", t1); ui_str(&m, "style", "caption"); ui_num(&m, "minWidth", 34);
            ui_str(&m, "textAlign", "end"); ui_end(&m);
            n_seek = ui_node(&m, "slider"); ui_str(&m, "width", "fill"); ui_num(&m, "value", at_ms);
            ui_num(&m, "max", total_ms ? total_ms : 1); ui_num(&m, "wheelStep", 5000); ui_num(&m, "disabled", !loaded);
            ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", t2); ui_str(&m, "style", "caption"); ui_num(&m, "minWidth", 34); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "row"); ui_num(&m, "gap", 10); ui_str(&m, "justify", "center");
            n_prev = transport(&m, NULL, "chevron-left", false);
            n_play = transport(&m, playing ? "Pause" : "Play", NULL, true);
            n_stop = transport(&m, "Stop", NULL, false);
            n_next = transport(&m, NULL, "chevron-right", false);
        ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", head); ui_str(&m, "style", "caption"); ui_end(&m);
        ui_node(&m, "scroll"); ui_str(&m, "key", "tracks"); ui_str(&m, "height", "fill"); ui_num(&m, "padding", 4);
        ui_num(&m, "gap", 2); ui_num(&m, "background", t->surface); ui_num(&m, "border", t->border);
        ui_num(&m, "radius", t->radius);
            if (!ntracks) {
                ui_node(&m, "text"); ui_str(&m, "text", "No WAV files here."); ui_str(&m, "style", "caption");
                ui_num(&m, "padding", 10); ui_end(&m);
            }
            for (int i = 0; i < ntracks; i++) {
                uint32_t n = ui_node(&m, "item");
                if (!i) n_items = n; /* the rest follow it, one place each */
                ui_str(&m, "text", names[i]); ui_str(&m, "icon", "sound"); ui_num(&m, "selected", i == cur);
                if (i == cur) ui_num(&m, "reveal", reveal);
                ui_end(&m);
            }
        ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

/* ---- input ---- */

static void toggle(void) {
    if (!loaded) play_track(cur >= 0 ? cur : 0);
    else playing = !playing && stream >= 0;
    if (playing && wav.eof) seek_to(0);
}

int main(int argc, char **argv) {
    char full[256];
    const char *start = argc > 1 && n_path_complete(argv[1], full, sizeof full) > 0 ? full : NULL;
    struct n_entry st;
    if (start && n_entry_info(start, &st) == 0 && st.kind == N_KIND_FOLDER) {
        load_dir(start);
        start = NULL;
    } else if (start) {
        char d[256];
        strlcpy(d, start, sizeof d);
        char *slash = strrchr(d, '/'); /* a complete path: there is one */
        if (slash == d) d[1] = 0;
        else *slash = 0;
        load_dir(d);
    } else {
        load_dir(MUSIC);
    }
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W, H, WIN_CENTER, "Sound Player", NULL, NULL) < 0) return 1;
    canvas_t *wave = ui_surface(s, SCOPE, SCOPE_W, SCOPE_H, 0);
    if (!wave) return 1;
    stream = n_audio_open();
    if (start) {
        const char *base_name = strrchr(start, '/') ? strrchr(start, '/') + 1 : start;
        for (int i = 0; i < ntracks; i++)
            if (!strcmp(names[i], base_name)) play_track(i);
        if (cur < 0) snprintf(error, sizeof error, "%s: not a WAV file", base_name);
    }
    draw_scope(wave);
    ui_surface_changed(s, SCOPE);
    show(s);
    uint64_t last = 0;
    bool was_playing = playing, was_loaded = loaded;
    int was_cur = cur; /* the next sound starts by itself, so watch for that too */
    for (bool running = true; running;) {
        ui_session_wait(s, playing ? 10 : -1);
        bool changed = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLOSE:
                running = false;
                break;
            case UI_EVENT_KEY: {
                if (e.flags & UI_UP) break;
                uint32_t key = (uint32_t)e.a;
                if (key == ' ') toggle();
                else if (key == NKEY_LEFT && loaded) seek_to(now_frame() > 5 * wav.rate ? now_frame() - 5 * wav.rate : 0);
                else if (key == NKEY_RIGHT && loaded) seek_to(MIN(wav.frames, now_frame() + 5 * wav.rate));
                else if (key == NKEY_UP && cur > 0) play_track(cur - 1);
                else if (key == NKEY_DOWN && cur + 1 < ntracks) play_track(cur + 1);
                else if (key == NKEY_ENTER) play_track(cur >= 0 ? cur : 0);
                else if (key == NKEY_ESC) stop();
                changed = true;
                break;
            }
            case UI_EVENT_SLIDE:
                if (e.node == n_seek && loaded) {
                    uint64_t f = (uint64_t)(e.a < 0 ? 0 : e.a) * wav.rate / 1000;
                    seek_to((uint32_t)MIN(f, (uint64_t)wav.frames));
                    changed = true;
                }
                break;
            case UI_EVENT_CLICK:
                if (e.node == n_prev) play_track(cur > 0 ? cur - 1 : 0);
                else if (e.node == n_play) toggle();
                else if (e.node == n_stop) stop();
                else if (e.node == n_next) { if (cur + 1 < ntracks) play_track(cur + 1); }
                else if (ntracks && e.node >= n_items && e.node < n_items + (uint32_t)ntracks) play_track((int)(e.node - n_items));
                changed = true;
                break;
            }
        }
        if (!running || !ui_session_open(s)) break;
        pump();
        bool tick = playing && n_clock_uptime_ms() - last >= 40;
        if (changed || playing != was_playing || cur != was_cur || loaded != was_loaded) {
            last = n_clock_uptime_ms();
            draw_scope(wave);
            ui_surface_changed_rect(s, SCOPE, 0, 0, SCOPE_W, SCOPE_H);
            show(s);
        } else if (tick) {
            last = n_clock_uptime_ms();
            draw_scope(wave);
            ui_surface_changed_rect(s, SCOPE, 0, 0, SCOPE_W, SCOPE_H);
            /* the widgets change only when the clock ticks over or the slider moves a step */
            uint32_t ms = wav_ms(&wav, now_frame()), total = wav_ms(&wav, wav.frames);
            if (ms / 1000 != shown_sec || (total && (int)((uint64_t)ms * SEEK_PX / total) != shown_px)) show(s);
        }
        was_playing = playing;
        was_loaded = loaded;
        was_cur = cur;
    }
    stop();
    ui_session_free(s);
    return 0;
}
