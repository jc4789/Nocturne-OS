/* About Nocturne */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

#define W 460
#define H 330

static void crescent(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d1 = x * x + y * y, ex = x - r * 45 / 100, ey = y + r * 25 / 100;
            if (d1 <= r * r && ex * ex + ey * ey > r * r * 85 / 100) gfx_pixel(c, cx + x, cy + y, col);
        }
}

static void draw(window_t *w, bool hot) {
    canvas_t *c = &w->c;
    gfx_gradient_v(c, 0, 0, W, H, RGB(20, 18, 46), RGB(56, 34, 86));
    for (int i = 0; i < 60; i++) {
        int x = (i * 7919) % W, y = (i * 104729) % 150;
        gfx_pixel(c, x, y, RGB(200, 200, 230));
    }
    for (int r = 60; r > 34; r -= 2) gfx_fill_blend(c, 70 - r, 80 - r, r * 2, r * 2, ARGB(6, 200, 190, 255));
    crescent(c, 70, 80, 34, RGB(246, 236, 196));
    gfx_text(c, 130, 46, "Nocturne", RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    gfx_text(c, 132, 84, "version 0.9 \"Moonrise\"", UI_ACCENT2, TRANSPARENT, FONT_SMALL);

    struct n_sysinfo si;
    sysinfo(&si);
    char buf[160];
    int y = 140;
    unsigned long up = si.uptime_ms / 1000;
    const char *labels[] = {"Kernel", "CPU", "Memory", "Display", "Uptime", "Tasks"};
    char vals[6][96];
    snprintf(vals[0], 96, "x86_64, preemptive, written from scratch");
    snprintf(vals[1], 96, "%.40s", si.cpu);
    snprintf(vals[2], 96, "%lu MiB free of %lu MiB", (unsigned long)(si.free_mem >> 20),
             (unsigned long)(si.total_mem >> 20));
    snprintf(vals[3], 96, "%ux%u, 32-bit colour", si.fb_w, si.fb_h);
    snprintf(vals[4], 96, "%lu:%02lu:%02lu", up / 3600, up / 60 % 60, up % 60);
    snprintf(vals[5], 96, "%d running", si.ntasks);
    for (int i = 0; i < 6; i++) {
        gfx_text(c, 30, y, labels[i], UI_DIM, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 110, y, vals[i], UI_FG, TRANSPARENT, FONT_SMALL);
        y += 20;
    }
    snprintf(buf, sizeof buf, "Made with care on a quiet night.");
    gfx_text(c, 30, H - 38, buf, RGB(170, 160, 210), TRANSPARENT, FONT_SMALL);
    ui_button(c, W - 110, H - 46, 90, 30, "Close", hot, false);
}

int main(void) {
    window_t *w = win_open(W, H, "About Nocturne", WIN_CENTER);
    if (!w) return 1;
    bool hot = false;
    draw(w, hot);
    win_update(w);
    for (;;) {
        struct gui_event e;
        int r = win_event(w, &e, 1000);
        if (r < 0 || e.type == EV_CLOSE) break;
        if (r == 0) {
            draw(w, hot);
            win_update(w);
            continue;
        }
        if (e.type == EV_MOUSE_MOVE) {
            bool h = ui_hit(e.x, e.y, W - 110, H - 46, 90, 30);
            if (h != hot) {
                hot = h;
                draw(w, hot);
                win_update(w);
            }
        } else if (e.type == EV_MOUSE_UP && ui_hit(e.x, e.y, W - 110, H - 46, 90, 30)) {
            break;
        } else if (e.type == EV_KEY && e.pressed && (e.key == NKEY_ENTER || e.key == NKEY_ESC)) {
            break;
        }
    }
    win_close(w);
    return 0;
}
