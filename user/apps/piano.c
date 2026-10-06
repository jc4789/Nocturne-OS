/* Piano: play two octaves with the mouse or the keyboard (Z S X D C ... for the lower one,
   Q 2 W 3 E ... for the upper one), in three voices. The sound is synthesised here and kept
   about 30 ms ahead of the speaker, by reading how much the /dev/audio stream has queued. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nocturne.h"

#define PI 3.14159265358979323846
#define W 720
#define TOP 92
#define KEYS_H 200
#define H (TOP + KEYS_H)
#define NWHITE 15 /* C to C, two octaves */
#define KW (W / NWHITE)
#define NNOTES 25
#define NVOICES 10
#define AHEAD 1440 /* frames to keep queued: 30 ms */
#define CHUNK 240  /* frames made at a time: 5 ms */

static const char *voice_names[] = {"Piano", "Organ", "Chip"};
static const char lower_keys[] = "zsxdcvgbhnjm,";  /* C .. C, white and black in turn */
static const char upper_keys[] = "q2w3er5t6y7ui";

struct voice {
    int note; /* 0..NNOTES-1, -1 when free */
    bool held;
    double ph, amp, env;
};
static struct voice voices[NVOICES];
static int sound = 0, octave = 4, mouse_note = -1, fd = -1;
static bool down[NNOTES];
static int16_t scope[CHUNK * 2];

static double note_hz(int n) { return 440.0 * pow(2.0, ((octave + 1) * 12 + n - 69) / 12.0); }
static bool is_black(int n) { return (0x54A >> (n % 12)) & 1; } /* C# D# F# G# A# */

static void note_on(int n) {
    if (n < 0 || n >= NNOTES) return;
    down[n] = true;
    struct voice *v = NULL;
    for (int i = 0; i < NVOICES && !v; i++)
        if (voices[i].note == n) v = &voices[i];
    for (int i = 0; i < NVOICES && !v; i++)
        if (voices[i].note < 0) v = &voices[i];
    if (!v) { /* steal the quietest */
        v = &voices[0];
        for (int i = 1; i < NVOICES; i++)
            if (voices[i].amp < v->amp) v = &voices[i];
    }
    v->note = n;
    v->held = true;
    v->amp = 1;
    v->env = 0;
    v->ph = 0;
}

static void note_off(int n) {
    if (n < 0 || n >= NNOTES) return;
    down[n] = false;
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].note == n) voices[i].held = false;
}

static bool any_voice(void) {
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].note >= 0) return true;
    return false;
}

/* how fast a voice fades, as a time constant in seconds */
static double fade(const struct voice *v) {
    static const double held[] = {1.1, 1e9, 2.5}, released[] = {0.12, 0.04, 0.06};
    return v->held ? held[sound] : released[sound];
}

static void synth(int16_t *out, int frames) {
    memset(out, 0, (size_t)frames * 4);
    for (int i = 0; i < NVOICES; i++) {
        struct voice *v = &voices[i];
        if (v->note < 0) continue;
        double step = note_hz(v->note) / SOUND_RATE, k = exp(-1.0 / (fade(v) * SOUND_RATE));
        for (int f = 0; f < frames; f++) {
            double p = 2 * PI * v->ph, s;
            if (sound == 0) s = sin(p) + 0.45 * sin(2 * p) + 0.2 * sin(3 * p) + 0.08 * sin(5 * p);
            else if (sound == 1) s = 0.8 * sin(p) + 0.5 * sin(2 * p) + 0.35 * sin(4 * p) + 0.15 * sin(8 * p);
            else s = v->ph < 0.5 ? 0.7 : -0.7;
            v->ph += step;
            if (v->ph >= 1) v->ph -= 1;
            if (v->env < 1) v->env = MIN(1.0, v->env + 1.0 / 144); /* 3 ms attack */
            v->amp *= k;
            int32_t add = (int32_t)(s * v->amp * v->env * 6000);
            int32_t l = out[2 * f] + add;
            out[2 * f] = out[2 * f + 1] = (int16_t)MAX(-32767, MIN(32767, l));
        }
        if (v->amp < 0.001) v->note = -1;
    }
}

/* keep the stream AHEAD frames ahead of the speaker while anything sounds */
static void pump(void) {
    if (fd < 0) return;
    uint32_t queued = 0;
    if (read(fd, &queued, 4) != 4) return;
    while (queued < AHEAD && any_voice()) {
        synth(scope, CHUNK);
        if (write(fd, scope, sizeof scope) <= 0) return;
        queued += CHUNK;
    }
}

/* ---- drawing ---- */

/* which white key a note is on, or (for a black key) just right of */
static int white_index(int n) {
    static const int in_octave[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    return n / 12 * 7 + in_octave[n % 12];
}

static void key_rect(int n, int *x, int *y, int *w, int *h) {
    if (is_black(n)) {
        *x = (white_index(n) + 1) * KW - 15;
        *w = 30;
        *h = KEYS_H * 6 / 10;
    } else {
        *x = white_index(n) * KW;
        *w = KW;
        *h = KEYS_H;
    }
    *y = TOP;
}

static int key_at(int px, int py) {
    if (py < TOP) return -1;
    for (int pass = 0; pass < 2; pass++) /* black keys lie on top */
        for (int n = 0; n < NNOTES; n++) {
            if (is_black(n) != (pass == 0)) continue;
            int x, y, w, h;
            key_rect(n, &x, &y, &w, &h);
            if (ui_hit(px, py, x, y, w, h)) return n;
        }
    return -1;
}

static char key_label(int n) { return n < 12 ? lower_keys[n] : upper_keys[n - 12]; }

static void draw_scope(canvas_t *c) {
    int sx = W - 250, sy = 14, sw = 236, sh = 64;
    gfx_fill_round(c, sx, sy, sw, sh, 6, RGB(16, 14, 30));
    int prev = 0;
    for (int i = 0; i < sw - 8; i++) {
        int f = i * CHUNK / (sw - 8), y = sy + sh / 2 - scope[2 * f] * (sh / 2 - 4) / 32768;
        if (i) gfx_line(c, sx + 3 + i, prev, sx + 4 + i, y, RGB(120, 230, 170));
        prev = y;
    }
}

static void draw(window_t *win) {
    canvas_t *c = &win->c;
    gfx_fill(c, 0, 0, W, TOP, UI_BG);
    for (int i = 0; i < 3; i++) ui_button(c, 14 + i * 92, 14, 84, 28, voice_names[i], false, sound == i);
    char buf[80];
    snprintf(buf, sizeof buf, "octave %d   (Left/Right)", octave);
    gfx_text(c, 16, 54, buf, UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, 16, 70, fd < 0 ? "no sound: /dev/audio is busy" : "Tab: next sound", fd < 0 ? UI_ACCENT2 : UI_DIM,
             TRANSPARENT, FONT_SMALL);
    draw_scope(c);
    for (int pass = 0; pass < 2; pass++)
        for (int n = 0; n < NNOTES; n++) {
            if (is_black(n) != (pass == 1)) continue;
            int x, y, w, h;
            key_rect(n, &x, &y, &w, &h);
            char lab[2] = {key_label(n), 0};
            if (lab[0] >= 'a' && lab[0] <= 'z') lab[0] -= 32;
            if (is_black(n)) {
                gfx_fill_round(c, x, y - 4, w, h + 4, 4, down[n] ? UI_ACCENT : RGB(24, 22, 36));
                ui_text_center(c, x, y + h - 22, w, lab, RGB(170, 170, 200), FONT_SMALL);
            } else {
                gfx_fill(c, x, y, w - 1, h, down[n] ? RGB(200, 190, 255) : RGB(246, 244, 236));
                gfx_fill(c, x + w - 1, y, 1, h, RGB(120, 118, 140));
                ui_text_center(c, x, y + h - 24, w, lab, RGB(120, 118, 140), FONT_SMALL);
                if (n % 12 == 0) {
                    snprintf(buf, sizeof buf, "C%d", octave + n / 12);
                    ui_text_center(c, x, y + h - 44, w, buf, RGB(160, 150, 200), FONT_SMALL);
                }
            }
        }
    win_update(win);
}

static int note_for_key(int k) {
    if (k >= 'A' && k <= 'Z') k += 32;
    if (k == '<') k = ',';
    const char *p = strchr(lower_keys, k);
    if (k && p) return (int)(p - lower_keys);
    p = strchr(upper_keys, k);
    if (k && p) return 12 + (int)(p - upper_keys);
    return -1;
}

int main(void) {
    for (int i = 0; i < NVOICES; i++) voices[i].note = -1;
    window_t *win = win_open(W, H, "Piano", WIN_CENTER);
    if (!win) return 1;
    fd = open("/dev/audio", O_RDWR | O_NONBLOCK);
    draw(win);
    uint64_t last_scope = 0;
    bool was_active = false;
    for (;;) {
        struct gui_event e;
        int r = win_event(win, &e, any_voice() ? 4 : -1);
        if (r < 0) break;
        bool redraw = false;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_KEY) {
                int n = note_for_key((int)e.key);
                if (n >= 0) {
                    if (e.pressed && !down[n]) note_on(n), redraw = true;
                    else if (!e.pressed) note_off(n), redraw = true;
                } else if (e.pressed) {
                    if (e.key == NKEY_ESC) break;
                    if (e.key == NKEY_TAB) sound = (sound + 1) % 3, redraw = true;
                    if (e.key == NKEY_LEFT && octave > 2) octave--, redraw = true;
                    if (e.key == NKEY_RIGHT && octave < 6) octave++, redraw = true;
                }
            } else if (e.type == EV_MOUSE_DOWN && (e.buttons & 1)) {
                for (int i = 0; i < 3; i++)
                    if (ui_hit(e.x, e.y, 14 + i * 92, 14, 84, 28)) sound = i;
                mouse_note = key_at(e.x, e.y);
                note_on(mouse_note);
                redraw = true;
            } else if (e.type == EV_MOUSE_MOVE && mouse_note >= 0) { /* a glissando */
                int n = key_at(e.x, e.y);
                if (n != mouse_note) {
                    note_off(mouse_note);
                    note_on(mouse_note = n);
                    redraw = true;
                }
            } else if (e.type == EV_MOUSE_UP && mouse_note >= 0) {
                note_off(mouse_note);
                mouse_note = -1;
                redraw = true;
            }
        }
        pump();
        bool active = any_voice();
        if (redraw) {
            draw(win);
        } else if (uptime_ms() - last_scope >= 33 || active != was_active) {
            last_scope = uptime_ms();
            if (!active) memset(scope, 0, sizeof scope);
            draw_scope(&win->c);
            win_update_rect(win, W - 250, 14, 236, 64);
        }
        was_active = active;
    }
    win_close(win);
    return 0;
}
