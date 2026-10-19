/* Analog clock: the digital time and the date are widgets of the interface engine
   (libc/ui.c); the dial is a surface the clock draws itself and fills the resizable
   window. It is redrawn once a second. Escape closes. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "nocturne.h"
#include "ui.h"

#define WINDOW 1
#define FACE 1
#define PAD 14
#define GAP 6
#define DIGITAL 30 /* the height of the digital time */
#define CAPTION 18 /* the height of the date */

static int cw, ch; /* the size of the dial's surface */
static char clock_text[16], date_text[64];

/* Reads the time into the texts and returns it as angles of the three hands (in radians). */
static void read_time(double *ha, double *ma, double *sa) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    double sec = tm->tm_sec, min = tm->tm_min + sec / 60.0, hr = (tm->tm_hour % 12) + min / 60.0;
    *ha = hr * M_PI / 6;
    *ma = min * M_PI / 30;
    *sa = sec * M_PI / 30;
    strftime(clock_text, sizeof clock_text, "%H:%M:%S", tm);
    strftime(date_text, sizeof date_text, "%A, %d %B %Y", tm);
}

static void hand(canvas_t *c, float cx, float cy, double a, double from, double to, float width, uint32_t col) {
    float s = (float)sin(a), co = (float)cos(a);
    ui_line(c, cx + s * (float)from, cy - co * (float)from, cx + s * (float)to, cy - co * (float)to, width, col);
}

static void draw_face(canvas_t *c, double ha, double ma, double sa) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, cw, ch, t->background);
    float cx = (float)cw / 2, cy = (float)ch / 2;
    float out = (float)MIN(cw, ch) / 2 - 8, R = out - 6;
    if (R < 20) return;
    ui_round_shadow(c, cx - out, cy - out + 3, 2 * out, 2 * out, out, 14, t->shadow);
    ui_round_fill(c, cx - out, cy - out, 2 * out, 2 * out, out, t->accent);
    ui_round_fill(c, cx - out + 3, cy - out + 3, 2 * out - 6, 2 * out - 6, out - 3, t->border);
    ui_round_fill(c, cx - R, cy - R, 2 * R, 2 * R, R, t->surface);
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30;
        double len = i % 5 == 0 ? R / 8 : R / 20;
        bool hour = i % 5 == 0;
        hand(c, cx, cy, a, R - 4, R - 4 - len, hour ? 2.6f : 1.2f,
             i % 15 == 0 ? t->accent : hour ? t->textDim : t->textFaint);
    }
    float size = fmaxf(11, R * 0.13f), lh = ui_line_height(size, 1, NULL);
    for (int h = 1; h <= 12; h++) {
        char b[4];
        snprintf(b, sizeof b, "%d", h);
        double a = h * M_PI / 6;
        float tw = ui_text_width(b, strlen(b), size, 1);
        float tx = cx + (float)(sin(a) * (R * 0.72)) - tw / 2, ty = cy - (float)(cos(a) * (R * 0.72)) - lh / 2;
        ui_text(c, tx, ty, b, strlen(b), size, 1, t->text);
    }
    hand(c, cx, cy, ha, 0, R * 0.5, fmaxf(3, R * 0.045f), t->text);
    hand(c, cx, cy, ma, 0, R * 0.75, fmaxf(2.5f, R * 0.03f), t->text);
    hand(c, cx, cy, sa, -R * 0.15, R * 0.85, 1.6f, t->danger);
    ui_round_fill(c, cx - 5, cy - 5, 10, 10, 5, t->danger);
    ui_round_fill(c, cx - 2, cy - 2, 4, 4, 2, t->surface);
}

static void show(struct ui_session *s) {
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", GAP);
        ui_node(&m, "surface"); ui_num(&m, "surface", FACE);
        ui_str(&m, "width", "fill"); ui_str(&m, "height", "fill"); ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", clock_text); ui_num(&m, "size", 26); ui_num(&m, "weight", 1);
        ui_str(&m, "textAlign", "center"); ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", date_text); ui_str(&m, "style", "caption");
        ui_str(&m, "textAlign", "center"); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

int main(void) {
    struct ui_session *s = ui_session_new();
    int ww, wh;
    if (!s || ui_window(s, WINDOW, 300, 370, WIN_RESIZABLE, "Clock", &ww, &wh) < 0) return 1;
    /* a first guess; the engine reports the real size with UI_EVENT_CANVAS */
    cw = ww - 2 * PAD;
    ch = wh - 2 * PAD - 2 * GAP - DIGITAL - CAPTION;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    canvas_t *face = ui_surface(s, FACE, cw, ch, 0);
    if (!face) return 1;
    double ha, ma, sa;
    read_time(&ha, &ma, &sa);
    draw_face(face, ha, ma, sa);
    show(s);
    uint64_t now = n_clock_uptime_ms(), next = now + 1000 - now % 1000;
    for (bool running = true; running;) {
        now = n_clock_uptime_ms();
        ui_session_wait(s, next > now ? (int)(next - now) : 0);
        bool redraw = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CANVAS:
                cw = (int)e.a;
                ch = (int)e.b;
                if (cw < 1) cw = 1;
                if (ch < 1) ch = 1;
                face = ui_surface(s, FACE, cw, ch, 0);
                if (!face) return 1;
                redraw = true;
                break;
            case UI_EVENT_KEY:
                if (!(e.flags & UI_UP) && !strcmp(e.text, "Escape")) running = false;
                break;
            case UI_EVENT_CLOSE:
                running = false;
                break;
            }
        }
        if (!running || !ui_session_open(s)) break;
        now = n_clock_uptime_ms();
        bool tick = now >= next;
        if (tick) next = now + 1000 - now % 1000;
        if (tick || redraw) {
            read_time(&ha, &ma, &sa);
            draw_face(face, ha, ma, sa);
            ui_surface_changed(s, FACE);
            if (tick) show(s); /* the digital time and the date */
        }
    }
    ui_session_free(s);
    return 0;
}
