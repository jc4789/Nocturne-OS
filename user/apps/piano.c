/* Piano: play two octaves with the mouse or the keyboard (Z S X D C ... for the lower one,
   Q 2 W 3 E ... for the upper one), in three voices. The voices and the octave are widgets
   of the interface engine (libc/ui.c); the keyboard and the oscilloscope are surfaces this
   app draws itself. The sound is synthesised here and kept about 30 ms ahead of the
   speaker, by asking how much its sound stream has queued. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define PI 3.14159265358979323846
#define W 720
#define KEYS_H 200
#define SCOPE_W 236
#define SCOPE_H 64
#define H 330
#define NWHITE 15 /* C to C, two octaves */
#define KW (W / NWHITE)
#define NNOTES 25
#define NVOICES 10
#define AHEAD 1440 /* frames to keep queued: 30 ms */
#define CHUNK 240  /* frames made at a time: 5 ms */
#define WINDOW 1
#define S_KEYS 1
#define S_SCOPE 2

static const char *voice_names[] = {"Piano", "Organ", "Chip"};
static const char lower_keys[] = "zsxdcvgbhnjm,";  /* C .. C, white and black in turn */
static const char upper_keys[] = "q2w3er5t6y7ui";

struct voice {
    int note; /* 0..NNOTES-1, -1 when free */
    bool held;
    double ph, amp, env;
};
static struct voice voices[NVOICES];
static int sound = 0, octave = 4, mouse_note = -1;
static n_handle stream = -1;
static bool down[NNOTES];
static int16_t scope[CHUNK * 2];
static canvas_t *keys_c, *scope_c;
static uint32_t voice_seg, octave_down, octave_up; /* their places in the view */
static int dirty_x0 = W, dirty_y0 = KEYS_H, dirty_x1, dirty_y1; /* keys changed since they were drawn */

static double note_hz(int n) { return 440.0 * pow(2.0, ((octave + 1) * 12 + n - 69) / 12.0); }
static bool is_black(int n) { return (0x54A >> (n % 12)) & 1; } /* C# D# F# G# A# */

/* ---- keys on the screen ---- */

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
    *y = 0;
}

static void mark_rect(int x, int y, int w, int h) {
    if (x < dirty_x0) dirty_x0 = x;
    if (y < dirty_y0) dirty_y0 = y;
    if (x + w > dirty_x1) dirty_x1 = x + w;
    if (y + h > dirty_y1) dirty_y1 = y + h;
}

static void mark_note(int n) {
    int x, y, w, h;
    key_rect(n, &x, &y, &w, &h);
    mark_rect(x, y, w, h);
}

static int key_at(int px, int py) {
    if (py < 0 || py >= KEYS_H) return -1;
    for (int pass = 0; pass < 2; pass++) /* black keys lie on top */
        for (int n = 0; n < NNOTES; n++) {
            if (is_black(n) != (pass == 0)) continue;
            int x, y, w, h;
            key_rect(n, &x, &y, &w, &h);
            if (px >= x && px < x + w && py >= y && py < y + h) return n;
        }
    return -1;
}

static char key_label(int n) { return n < 12 ? lower_keys[n] : upper_keys[n - 12]; }

/* ---- the notes ---- */

static void note_on(int n) {
    if (n < 0 || n >= NNOTES) return;
    down[n] = true;
    mark_note(n);
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
    mark_note(n);
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
    if (stream < 0) return;
    struct n_audio_status st;
    if (n_audio_status(stream, &st) < 0) return;
    uint32_t queued = st.queued_frames;
    while (queued < AHEAD && any_voice()) {
        synth(scope, CHUNK);
        if (n_audio_submit(stream, scope, CHUNK, 0) <= 0) return;
        queued += CHUNK;
    }
}

/* ---- drawing ---- */

static void draw_scope(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, SCOPE_W, SCOPE_H, t->background);
    ui_round_fill(c, 0, 0, SCOPE_W, SCOPE_H, t->radius, t->surface);
    ui_round_stroke(c, 0, 0, SCOPE_W, SCOPE_H, t->radius, 1, t->border);
    ui_line(c, 8, SCOPE_H / 2, SCOPE_W - 8, SCOPE_H / 2, 1, t->divider);
    int span = SCOPE_W - 16;
    float px = 0, py = 0;
    for (int i = 0; i <= span; i += 2) {
        int f = MIN(CHUNK - 1, i * CHUNK / span);
        float x = 8.0f + (float)i, y = SCOPE_H / 2.0f - (float)scope[2 * f] * (SCOPE_H / 2 - 6) / 32768.0f;
        if (i) ui_line(c, px, py, x, y, 1.6f, t->success);
        px = x;
        py = y;
    }
}

static void draw_keys(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, W, KEYS_H, t->background);
    for (int pass = 0; pass < 2; pass++)
        for (int n = 0; n < NNOTES; n++) {
            if (is_black(n) != (pass == 1)) continue;
            int x, y, w, h;
            key_rect(n, &x, &y, &w, &h);
            char lab[2] = {key_label(n), 0};
            if (lab[0] >= 'a' && lab[0] <= 'z') lab[0] -= 32;
            /* keys reach above the surface so only their lower corners are round */
            if (is_black(n)) {
                ui_round_gradient(c, (float)x, -10, (float)w, (float)h + 10, 5, down[n] ? t->accentHover : RGB(52, 50, 70),
                                  down[n] ? t->accent : RGB(22, 21, 30));
                float lw = ui_text_width(lab, 1, 12, 1);
                ui_text(c, (float)x + ((float)w - lw) / 2, (float)(y + h) - 24, lab, 1, 12, 1, RGB(170, 170, 200));
            } else {
                ui_round_gradient(c, (float)x + 1, -10, (float)w - 2, (float)h + 10, 7,
                                  down[n] ? t->accentHover : RGB(250, 249, 252), down[n] ? t->accent : RGB(226, 224, 236));
                float lw = ui_text_width(lab, 1, 12, 1);
                ui_text(c, (float)x + ((float)w - lw) / 2, (float)(y + h) - 26, lab, 1, 12, 1, down[n] ? t->accentText : RGB(120, 118, 140));
                if (n % 12 == 0) {
                    char buf[8];
                    int len = snprintf(buf, sizeof buf, "C%d", octave + n / 12);
                    float cw = ui_text_width(buf, (size_t)len, 12, 0);
                    ui_text(c, (float)x + ((float)w - cw) / 2, (float)(y + h) - 48, buf, (size_t)len, 12, 0,
                            down[n] ? t->accentText : RGB(130, 120, 190));
                }
            }
        }
}

/* Draw the keys, telling the engine only about the part that changed. */
static void flush_keys(struct ui_session *s, bool whole) {
    if (whole) mark_rect(0, 0, W, KEYS_H);
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) return;
    draw_keys(keys_c);
    ui_surface_changed_rect(s, S_KEYS, dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    dirty_x0 = W;
    dirty_y0 = KEYS_H;
    dirty_x1 = dirty_y1 = 0;
}

/* The window's widgets: the voices, the octave and a hint. */
static void show(struct ui_session *s) {
    char label[24];
    snprintf(label, sizeof label, "Octave %d", octave);
    const double pad[2] = {14, 16};
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column");
        ui_node(&m, "row"); ui_nums(&m, "padding", pad, 2); ui_num(&m, "gap", 16); ui_num(&m, "grow", 1);
            ui_node(&m, "column"); ui_num(&m, "gap", 10); ui_num(&m, "grow", 1); ui_str(&m, "align", "start");
                voice_seg = ui_node(&m, "segmented");
                ui_strs(&m, "options", voice_names, 3); ui_num(&m, "selected", sound); ui_end(&m);
                ui_node(&m, "row"); ui_num(&m, "gap", 8);
                    octave_down = ui_node(&m, "button");
                    ui_str(&m, "icon", "minus"); ui_num(&m, "compact", 1); ui_num(&m, "disabled", octave <= 2); ui_end(&m);
                    ui_node(&m, "text"); ui_str(&m, "text", label); ui_num(&m, "weight", 1); ui_num(&m, "width", 84);
                    ui_str(&m, "textAlign", "center"); ui_end(&m);
                    octave_up = ui_node(&m, "button");
                    ui_str(&m, "icon", "plus"); ui_num(&m, "compact", 1); ui_num(&m, "disabled", octave >= 6); ui_end(&m);
                ui_end(&m);
                ui_node(&m, "text");
                ui_str(&m, "text", stream < 0 ? "No sound: every sound stream is taken"
                                              : "Tab: next sound   Left / Right: octave");
                ui_str(&m, "style", "caption");
                if (stream < 0) ui_num(&m, "colour", ui_theme.warning);
                ui_end(&m);
            ui_end(&m);
            ui_node(&m, "surface"); ui_num(&m, "surface", S_SCOPE);
            ui_num(&m, "width", SCOPE_W); ui_num(&m, "height", SCOPE_H); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", S_KEYS); ui_num(&m, "pointer", 1);
        ui_num(&m, "width", W); ui_num(&m, "height", KEYS_H); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

static int note_for_key(int k) {
    if (k <= 0 || k > 0x7F) return -1; /* named keys have codes of their own */
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
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W, H, WIN_CENTER, "Piano", NULL, NULL) < 0) return 1;
    keys_c = ui_surface(s, S_KEYS, W, KEYS_H, 0);
    scope_c = ui_surface(s, S_SCOPE, SCOPE_W, SCOPE_H, 0);
    if (!keys_c || !scope_c) return 1;
    stream = n_audio_open();
    flush_keys(s, true);
    draw_scope(scope_c);
    ui_surface_changed(s, S_SCOPE);
    show(s);
    uint64_t last_scope = 0;
    bool was_active = false;
    for (bool running = true; running;) {
        ui_session_wait(s, any_voice() ? 4 : -1);
        bool view = false, labels = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLOSE:
                running = false;
                break;
            case UI_EVENT_KEY: {
                bool up = e.flags & UI_UP;
                int n = note_for_key((int)e.a);
                if (n >= 0) {
                    if (!up && !down[n]) note_on(n);
                    else if (up) note_off(n);
                } else if (!up) {
                    if (!strcmp(e.text, "Escape")) running = false;
                    else if (!strcmp(e.text, "Tab")) sound = (sound + 1) % 3, view = true;
                    else if (!strcmp(e.text, "ArrowLeft") && octave > 2) octave--, view = labels = true;
                    else if (!strcmp(e.text, "ArrowRight") && octave < 6) octave++, view = labels = true;
                }
                break;
            }
            case UI_EVENT_CHOOSE:
                if (e.node == voice_seg) sound = (int)e.a, view = true;
                break;
            case UI_EVENT_CLICK:
                if (e.node == octave_down && octave > 2) octave--, view = labels = true;
                else if (e.node == octave_up && octave < 6) octave++, view = labels = true;
                break;
            case UI_EVENT_POINTER: {
                int x = (int)floor(e.a), y = (int)floor(e.b);
                if (e.flags & UI_DOWN) {
                    if (UI_BUTTONS(e.flags) & 1) {
                        mouse_note = key_at(x, y);
                        note_on(mouse_note);
                    }
                } else if (e.flags & UI_UP) {
                    if (mouse_note >= 0) {
                        note_off(mouse_note);
                        mouse_note = -1;
                    }
                } else if (mouse_note >= 0) { /* a glissando */
                    int n = key_at(x, y);
                    if (n != mouse_note) {
                        note_off(mouse_note);
                        note_on(mouse_note = n);
                    }
                }
                break;
            }
            }
        }
        if (!running || !ui_session_open(s)) break;
        pump();
        flush_keys(s, labels);
        if (view) show(s);
        bool active = any_voice();
        if ((active && n_clock_uptime_ms() - last_scope >= 33) || active != was_active) {
            last_scope = n_clock_uptime_ms();
            if (!active) memset(scope, 0, sizeof scope);
            draw_scope(scope_c);
            ui_surface_changed(s, S_SCOPE);
        }
        was_active = active;
    }
    ui_session_free(s);
    return 0;
}
