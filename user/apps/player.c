/* Sound Player: plays WAV files. Opened with a file (from Files) it plays it, and lists the other
   sounds in the same folder; opened on its own it shows the Music folder. The sound is read and
   resampled here and kept about 100 ms ahead of the speaker, so pausing and seeking answer at
   once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define W      520
#define H      424
#define MUSIC  "/home/Music"
#define AHEAD  4800 /* frames kept queued: 100 ms */
#define CHUNK  960  /* frames made at a time: 20 ms */
#define MAXT   200
#define LIST_Y 248
#define ROW    24
#define ROWS   ((H - LIST_Y - 28) / ROW)
#define SEEK_X 74
#define SEEK_W (W - 2 * SEEK_X)
#define SEEK_Y 152

static window_t *win;
static char dir[256], names[MAXT][128], error[160];
static int ntracks, cur = -1, scroll, hover = -1, fd = -1;
static struct wav wav;
static bool loaded, playing, seeking;
static uint64_t sent;     /* frames written since the track (or the last seek) started */
static uint32_t base;     /* the file frame that `sent` counts from */
static int16_t scope[CHUNK * 2];

static void join(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

static void load_dir(const char *path) {
    strlcpy(dir, path, sizeof dir);
    ntracks = 0;
    int d = open(dir, O_RDONLY);
    if (d < 0) return;
    struct n_dirent e;
    for (int i = 0; ntracks < MAXT && readdir(d, i, &e) > 0; i++) {
        const char *dot = strrchr(e.name, '.');
        if (e.type != N_FT_DIR && dot && !strcasecmp(dot, ".wav")) strlcpy(names[ntracks++], e.name, 128);
    }
    close(d);
    qsort(names, ntracks, sizeof names[0], by_name);
}

/* where the speaker is now, in the file's frames: what was sent, less what is still queued */
static uint32_t now_frame(void) {
    if (!loaded) return 0;
    uint32_t queued = 0;
    if (fd >= 0 && playing) read(fd, &queued, 4);
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
    if (cur < scroll) scroll = cur;
    if (cur >= scroll + ROWS) scroll = cur - ROWS + 1;
    char path[512];
    join(path, sizeof path, names[i]);
    const char *err = wav_open(&wav, path);
    if (err) {
        snprintf(error, sizeof error, "%s: %s", names[i], err);
        return;
    }
    loaded = true;
    playing = fd >= 0;
}

static void seek_to(uint32_t frame) {
    if (!loaded) return;
    wav_seek(&wav, frame);
    base = frame;
    sent = 0;
}

/* keep the stream AHEAD frames ahead of the speaker; at the end, go on to the next sound */
static void pump(void) {
    if (!playing || fd < 0) return;
    uint32_t queued = 0;
    if (read(fd, &queued, 4) != 4) return;
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
        if (write(fd, scope, (size_t)n * 4) <= 0) return;
        sent += (uint64_t)n;
        queued += (uint32_t)n;
    }
}

/* ---- drawing ---- */

static void clock_text(char *s, uint32_t ms) { sprintf(s, "%u:%02u", ms / 60000, ms / 1000 % 60); }

static void draw_scope(canvas_t *c) {
    int x = 20, y = 72, w = W - 40, h = 58;
    gfx_fill_round(c, x, y, w, h, 6, RGB(16, 14, 30));
    int prev = 0;
    for (int i = 0; i < w - 8; i++) {
        int f = i * CHUNK / (w - 8);
        int v = (scope[2 * f] + scope[2 * f + 1]) / 2, py = y + h / 2 - v * (h / 2 - 4) / 32768;
        if (i) gfx_line(c, x + 3 + i, prev, x + 4 + i, py, RGB(120, 230, 170));
        prev = py;
    }
}

static void draw_seek(canvas_t *c) {
    gfx_fill(c, 0, SEEK_Y - 12, W, 26, UI_BG);
    uint32_t at = now_frame(), total = loaded ? wav.frames : 0;
    char t1[16], t2[16];
    clock_text(t1, loaded ? wav_ms(&wav, at) : 0);
    clock_text(t2, loaded ? wav_ms(&wav, total) : 0);
    gfx_text(c, SEEK_X - 12 - gfx_text_width(t1, FONT_SMALL), SEEK_Y - 6, t1, UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, SEEK_X + SEEK_W + 12, SEEK_Y - 6, t2, UI_DIM, TRANSPARENT, FONT_SMALL);
    int fill = total ? (int)((uint64_t)SEEK_W * at / total) : 0;
    gfx_fill_round(c, SEEK_X, SEEK_Y, SEEK_W, 4, 2, RGB(60, 56, 100));
    if (fill) gfx_fill_round(c, SEEK_X, SEEK_Y, MAX(fill, 4), 4, 2, UI_ACCENT);
    if (loaded) gfx_fill_circle(c, SEEK_X + fill, SEEK_Y + 2, 7, RGB(235, 230, 255));
}

/* the transport buttons: previous, play/pause, stop, next */
static const int bx[4] = {W / 2 - 110, W / 2 - 28, W / 2 + 40, W / 2 + 84};
static const int bw[4] = {40, 56, 40, 40};
#define BTN_Y 178

static void draw_buttons(canvas_t *c) {
    gfx_fill(c, 0, BTN_Y - 4, W, 66, UI_BG);
    uint32_t fg = UI_FG;
    for (int i = 0; i < 4; i++) {
        int x = bx[i], y = BTN_Y + (i == 1 ? 0 : 8), w = bw[i], h = i == 1 ? 56 : 40;
        int cx = x + w / 2, cy = y + h / 2;
        if (i == 1) gfx_fill_circle(c, cx, cy, 26, UI_ACCENT);
        else gfx_fill_circle(c, cx, cy, 18, UI_BTN);
        switch (i) {
        case 0: /* previous: a bar and a triangle pointing left */
            gfx_fill(c, cx - 7, cy - 6, 2, 13, fg);
            gfx_triangle(c, cx + 6, cy - 6, cx + 6, cy + 6, cx - 4, cy, fg);
            break;
        case 1:
            if (playing) {
                gfx_fill(c, cx - 8, cy - 10, 6, 21, fg);
                gfx_fill(c, cx + 3, cy - 10, 6, 21, fg);
            } else {
                gfx_triangle(c, cx - 6, cy - 11, cx - 6, cy + 11, cx + 11, cy, fg);
            }
            break;
        case 2: gfx_fill(c, cx - 6, cy - 6, 13, 13, fg); break;
        case 3:
            gfx_triangle(c, cx - 6, cy - 6, cx - 6, cy + 6, cx + 4, cy, fg);
            gfx_fill(c, cx + 5, cy - 6, 2, 13, fg);
            break;
        }
    }
}

static void draw_list(canvas_t *c) {
    gfx_fill(c, 0, LIST_Y - 6, W, H - LIST_Y + 6, UI_BG);
    char head[300];
    snprintf(head, sizeof head, "%s  (%d sound%s)", dir, ntracks, ntracks == 1 ? "" : "s");
    gfx_text(c, 20, LIST_Y - 2, head, UI_DIM, TRANSPARENT, FONT_SMALL);
    int y0 = LIST_Y + 20;
    gfx_fill_round(c, 14, y0 - 4, W - 28, ROWS * ROW + 8, 6, UI_PANEL);
    if (!ntracks) gfx_text(c, 28, y0 + 4, "No WAV files here.", UI_DIM, TRANSPARENT, FONT_SMALL);
    for (int r = 0; r < ROWS && scroll + r < ntracks; r++) {
        int i = scroll + r, y = y0 + r * ROW;
        if (i == cur) gfx_fill_round(c, 18, y, W - 36, ROW - 2, 4, RGB(76, 64, 150));
        else if (i == hover) gfx_fill_round(c, 18, y, W - 36, ROW - 2, 4, RGB(54, 50, 94));
        if (i == cur) gfx_triangle(c, 28, y + 6, 28, y + 16, 36, y + 11, UI_ACCENT2);
        gfx_text(c, 46, y + 4, names[i], UI_FG, TRANSPARENT, FONT_SMALL);
    }
}

static void draw(void) {
    canvas_t *c = &win->c;
    gfx_fill(c, 0, 0, W, H, UI_BG);
    char title[160], info[160];
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
    gfx_text(c, 20, 16, title, UI_FG, TRANSPARENT, FONT_LARGE);
    if (error[0]) gfx_text(c, 20, 46, error, UI_ACCENT2, TRANSPARENT, FONT_SMALL);
    else if (fd < 0) gfx_text(c, 20, 46, "No sound: too many programs are playing.", UI_ACCENT2, TRANSPARENT, FONT_SMALL);
    else gfx_text(c, 20, 46, info, UI_DIM, TRANSPARENT, FONT_SMALL);
    draw_scope(c);
    draw_seek(c);
    draw_buttons(c);
    draw_list(c);
    win_update(win);
}

/* ---- input ---- */

static void seek_from_mouse(int x) {
    if (!loaded) return;
    int64_t f = (int64_t)(x - SEEK_X) * wav.frames / SEEK_W;
    seek_to((uint32_t)MAX(0, MIN((int64_t)wav.frames, f)));
}

static void toggle(void) {
    if (!loaded) play_track(cur >= 0 ? cur : 0);
    else playing = !playing && fd >= 0;
    if (playing && wav.eof) seek_to(0);
}

static int row_at(int x, int y) {
    int y0 = LIST_Y + 20;
    if (x < 18 || x > W - 18 || y < y0) return -1;
    int i = scroll + (y - y0) / ROW;
    return (y - y0) / ROW < ROWS && i < ntracks ? i : -1;
}

int main(int argc, char **argv) {
    const char *start = argc > 1 ? argv[1] : NULL;
    struct n_stat st;
    if (start && stat(start, &st) == 0 && st.type == N_FT_DIR) {
        load_dir(start);
        start = NULL;
    } else if (start) {
        char d[256];
        strlcpy(d, start, sizeof d);
        char *slash = strrchr(d, '/');
        if (slash == d) d[1] = 0;
        else if (slash) *slash = 0;
        else strlcpy(d, ".", sizeof d);
        load_dir(d);
    } else {
        load_dir(MUSIC);
    }
    win = win_open(W, H, "Sound Player", WIN_CENTER);
    if (!win) return 1;
    fd = open("/dev/audio", O_RDWR | O_NONBLOCK);
    if (start) {
        const char *base_name = strrchr(start, '/') ? strrchr(start, '/') + 1 : start;
        for (int i = 0; i < ntracks; i++)
            if (!strcmp(names[i], base_name)) play_track(i);
        if (cur < 0) snprintf(error, sizeof error, "%s: not a WAV file", base_name);
    }
    draw();
    uint64_t last = 0;
    bool was_playing = playing;
    int was_cur = cur; /* the next sound starts by itself, so watch for that too */
    for (;;) {
        struct gui_event e;
        int r = win_event(win, &e, playing ? 10 : -1);
        if (r < 0) break;
        bool redraw = false;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_KEY && e.pressed) {
                if (e.key == ' ') toggle();
                else if (e.key == NKEY_LEFT && loaded) seek_to(now_frame() > 5 * wav.rate ? now_frame() - 5 * wav.rate : 0);
                else if (e.key == NKEY_RIGHT && loaded) seek_to(MIN(wav.frames, now_frame() + 5 * wav.rate));
                else if (e.key == NKEY_UP && cur > 0) play_track(cur - 1);
                else if (e.key == NKEY_DOWN && cur + 1 < ntracks) play_track(cur + 1);
                else if (e.key == NKEY_ENTER) play_track(cur >= 0 ? cur : 0);
                else if (e.key == NKEY_ESC) stop();
                redraw = true;
            } else if (e.type == EV_MOUSE_DOWN && (e.buttons & 1)) {
                if (loaded && ui_hit(e.x, e.y, SEEK_X - 8, SEEK_Y - 10, SEEK_W + 16, 24)) {
                    seeking = true;
                    seek_from_mouse(e.x);
                }
                for (int i = 0; i < 4; i++)
                    if (ui_hit(e.x, e.y, bx[i], BTN_Y, bw[i], 56)) {
                        if (i == 0) play_track(cur > 0 ? cur - 1 : 0);
                        if (i == 1) toggle();
                        if (i == 2) stop();
                        if (i == 3 && cur + 1 < ntracks) play_track(cur + 1);
                    }
                int row = row_at(e.x, e.y);
                if (row >= 0) play_track(row);
                redraw = true;
            } else if (e.type == EV_MOUSE_MOVE) {
                if (seeking) seek_from_mouse(e.x), redraw = true;
                int row = row_at(e.x, e.y);
                if (row != hover) hover = row, redraw = true;
            } else if (e.type == EV_MOUSE_UP) {
                seeking = false;
            } else if (e.type == EV_WHEEL) {
                scroll = MAX(0, MIN(MAX(0, ntracks - ROWS), scroll + e.wheel));
                redraw = true;
            }
        }
        pump();
        if (redraw || playing != was_playing || cur != was_cur) {
            draw();
        } else if (playing && uptime_ms() - last >= 40) {
            last = uptime_ms();
            draw_scope(&win->c);
            draw_seek(&win->c);
            win_update_rect(win, 0, 70, W, SEEK_Y + 16 - 70);
        }
        was_playing = playing;
        was_cur = cur;
    }
    stop();
    win_close(win);
    return 0;
}
