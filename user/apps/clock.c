/* Analog clock */
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "nocturne.h"

static void thick_line(canvas_t *c, double x0, double y0, double x1, double y1, int t, uint32_t col) {
    for (int dy = -t / 2; dy <= t / 2; dy++)
        for (int dx = -t / 2; dx <= t / 2; dx++)
            gfx_line(c, (int)(x0 + dx), (int)(y0 + dy), (int)(x1 + dx), (int)(y1 + dy), col);
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    int W = w->w, H = w->h;
    gfx_gradient_v(c, 0, 0, W, H, RGB(22, 20, 44), RGB(40, 30, 70));
    int R = (W < H - 50 ? W : H - 50) / 2 - 14;
    int cx = W / 2, cy = R + 16;
    gfx_fill_circle(c, cx, cy + 3, R + 6, RGB(12, 10, 24));
    gfx_fill_circle(c, cx, cy, R + 6, RGB(110, 96, 200));
    gfx_fill_circle(c, cx, cy, R + 2, RGB(30, 28, 56));
    gfx_fill_circle(c, cx, cy, R, RGB(244, 240, 228));
    for (int i = 0; i < 60; i++) {
        double a = i * M_PI / 30;
        double s = sin(a), co = cos(a);
        int len = i % 5 == 0 ? R / 8 : R / 20;
        thick_line(c, cx + s * (R - 4), cy - co * (R - 4), cx + s * (R - 4 - len), cy - co * (R - 4 - len),
                   i % 5 == 0 ? 3 : 1, i % 15 == 0 ? RGB(110, 80, 200) : RGB(60, 60, 80));
    }
    for (int h = 1; h <= 12; h++) {
        char b[4];
        snprintf(b, sizeof b, "%d", h);
        double a = h * M_PI / 6;
        int tx = cx + (int)(sin(a) * (R * 0.72)) - gfx_text_width(b, FONT_SMALL) / 2;
        int ty = cy - (int)(cos(a) * (R * 0.72)) - 8;
        gfx_text(c, tx, ty, b, RGB(50, 44, 80), TRANSPARENT, FONT_SMALL);
    }
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    double sec = tm->tm_sec, min = tm->tm_min + sec / 60.0, hr = (tm->tm_hour % 12) + min / 60.0;
    double ha = hr * M_PI / 6, ma = min * M_PI / 30, sa = sec * M_PI / 30;
    thick_line(c, cx, cy, cx + sin(ha) * R * 0.5, cy - cos(ha) * R * 0.5, 5, RGB(40, 36, 70));
    thick_line(c, cx, cy, cx + sin(ma) * R * 0.75, cy - cos(ma) * R * 0.75, 3, RGB(40, 36, 70));
    thick_line(c, cx - sin(sa) * R * 0.15, cy + cos(sa) * R * 0.15, cx + sin(sa) * R * 0.85, cy - cos(sa) * R * 0.85, 1,
               RGB(230, 80, 100));
    gfx_fill_circle(c, cx, cy, 5, RGB(230, 80, 100));
    char buf[64];
    strftime(buf, sizeof buf, "%H:%M:%S", tm);
    ui_text_center(c, 0, H - 34, W, buf, RGB(240, 240, 255), FONT_SMALL);
    strftime(buf, sizeof buf, "%A, %d %B %Y", tm);
    ui_text_center(c, 0, H - 18, W, buf, UI_DIM, FONT_SMALL);
}

int main(void) {
    window_t *w = win_open(280, 330, "Clock", WIN_RESIZABLE);
    if (!w) return 1;
    for (;;) {
        draw(w);
        win_update(w);
        struct gui_event e;
        int r = win_event(w, &e, 1000 - (int)(uptime_ms() % 1000));
        if (r < 0 || (r > 0 && e.type == EV_CLOSE)) break;
        if (r > 0 && e.type == EV_KEY && e.pressed && e.key == NKEY_ESC) break;
    }
    win_close(w);
    return 0;
}
