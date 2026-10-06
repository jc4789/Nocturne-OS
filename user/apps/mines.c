/* Minesweeper */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define GW 16
#define GH 16
#define MINES 40
#define CS 24
#define TOP 48
#define W (GW * CS)
#define H (GH * CS + TOP)

static bool mine[GH][GW], open_[GH][GW], flag[GH][GW];
static int nflags, nopen;
static bool lost, won, started;
static uint64_t t0, t_end;

static int count(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < GW && ny < GH && mine[ny][nx]) n++;
        }
    return n;
}

static void reset(void) {
    memset(mine, 0, sizeof mine);
    memset(open_, 0, sizeof open_);
    memset(flag, 0, sizeof flag);
    nflags = nopen = 0;
    lost = won = started = false;
}

static void plant(int sx, int sy) {
    int n = 0;
    while (n < MINES) {
        int x = rand() % GW, y = rand() % GH;
        if (mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        mine[y][x] = true;
        n++;
    }
    started = true;
    t0 = uptime_ms();
}

static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= GW || y >= GH || open_[y][x] || flag[y][x]) return;
    open_[y][x] = true;
    nopen++;
    if (mine[y][x]) {
        lost = true;
        t_end = uptime_ms();
        sound_effect(220, 30, 160, SND_NOISE);
        return;
    }
    if (count(x, y) == 0)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) reveal(x + dx, y + dy);
    if (nopen == GW * GH - MINES && !lost) {
        won = true;
        t_end = uptime_ms();
        sound_effect(523, 523, 50, SND_TRIANGLE);
        sound_effect(659, 659, 50, SND_TRIANGLE);
        sound_effect(784, 1047, 60, SND_TRIANGLE);
    }
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    gfx_gradient_v(c, 0, 0, W, TOP, RGB(40, 34, 76), RGB(28, 24, 54));
    char buf[32];
    snprintf(buf, sizeof buf, "%03d", MINES - nflags);
    gfx_fill_round(c, 10, 10, 64, 28, 6, RGB(10, 8, 20));
    gfx_text(c, 18, 16, buf, RGB(250, 90, 100), TRANSPARENT, FONT_SMALL);
    int secs = started ? (int)(((lost || won) ? t_end : uptime_ms()) - t0) / 1000 : 0;
    snprintf(buf, sizeof buf, "%03d", secs > 999 ? 999 : secs);
    gfx_fill_round(c, W - 74, 10, 64, 28, 6, RGB(10, 8, 20));
    gfx_text(c, W - 66, 16, buf, RGB(250, 90, 100), TRANSPARENT, FONT_SMALL);
    /* face button */
    int fx = W / 2;
    gfx_fill_circle(c, fx, 24, 15, RGB(250, 210, 80));
    gfx_circle(c, fx, 24, 15, RGB(120, 90, 20));
    if (lost) {
        gfx_line(c, fx - 8, 17, fx - 4, 21, RGB(40, 30, 10));
        gfx_line(c, fx - 8, 21, fx - 4, 17, RGB(40, 30, 10));
        gfx_line(c, fx + 4, 17, fx + 8, 21, RGB(40, 30, 10));
        gfx_line(c, fx + 4, 21, fx + 8, 17, RGB(40, 30, 10));
        gfx_hline(c, fx - 5, 31, 10, RGB(40, 30, 10));
    } else {
        gfx_fill(c, fx - 6, 18, 3, 3, RGB(40, 30, 10));
        gfx_fill(c, fx + 4, 18, 3, 3, RGB(40, 30, 10));
        if (won) gfx_fill(c, fx - 9, 17, 18, 4, RGB(20, 20, 30));
        for (int i = -6; i <= 6; i++) gfx_pixel(c, fx + i, 31 - (i * i) / 12, RGB(40, 30, 10));
    }
    static const uint32_t numc[9] = {0, RGB(90, 150, 255), RGB(90, 200, 110), RGB(240, 90, 100), RGB(150, 110, 250),
                                     RGB(220, 120, 60), RGB(70, 200, 210), RGB(230, 230, 230), RGB(150, 150, 150)};
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int px = x * CS, py = TOP + y * CS;
            if (open_[y][x] || (lost && mine[y][x])) {
                gfx_fill(c, px, py, CS, CS, RGB(36, 34, 54));
                gfx_rect(c, px, py, CS, CS, RGB(28, 26, 42));
                if (mine[y][x]) {
                    if (open_[y][x]) gfx_fill(c, px + 1, py + 1, CS - 2, CS - 2, RGB(170, 40, 60));
                    gfx_fill_circle(c, px + CS / 2, py + CS / 2, 6, RGB(15, 15, 20));
                    gfx_pixel(c, px + CS / 2 - 2, py + CS / 2 - 2, RGB(255, 255, 255));
                } else {
                    int n = count(x, y);
                    if (n) {
                        snprintf(buf, sizeof buf, "%d", n);
                        gfx_text(c, px + 8, py + 4, buf, numc[n], TRANSPARENT, FONT_SMALL);
                    }
                }
            } else {
                gfx_fill(c, px, py, CS, CS, RGB(84, 80, 130));
                gfx_hline(c, px, py, CS, RGB(130, 126, 180));
                gfx_vline(c, px, py, CS, RGB(130, 126, 180));
                gfx_hline(c, px, py + CS - 1, CS, RGB(50, 46, 80));
                gfx_vline(c, px + CS - 1, py, CS, RGB(50, 46, 80));
                if (flag[y][x]) {
                    gfx_vline(c, px + 9, py + 5, 14, RGB(30, 30, 30));
                    gfx_triangle(c, px + 10, py + 5, px + 10, py + 13, px + 18, py + 9, RGB(240, 70, 90));
                    gfx_hline(c, px + 6, py + 19, 8, RGB(30, 30, 30));
                }
            }
        }
    if (won) {
        gfx_fill_blend(c, 0, TOP + H / 2 - 50, W, 60, ARGB(170, 10, 10, 30));
        ui_text_center(c, 0, TOP + H / 2 - 44, W, "You win!", RGB(255, 230, 120), FONT_LARGE);
    }
}

int main(void) {
    srand((unsigned)uptime_ms());
    window_t *w = win_open(W, H, "Minesweeper", 0);
    if (!w) return 1;
    reset();
    draw(w);
    win_update(w);
    for (;;) {
        struct gui_event e;
        int r = win_event(w, &e, started && !lost && !won ? 1000 : -1);
        if (r < 0) break;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_KEY && e.pressed && e.key == NKEY_ESC) break;
            if (e.type == EV_KEY && e.pressed && (e.key == 'n' || e.key == NKEY_F2)) reset();
            if (e.type == EV_MOUSE_DOWN) {
                if (e.y < TOP) {
                    if (abs(e.x - W / 2) < 16) reset();
                } else if (!lost && !won) {
                    int x = e.x / CS, y = (e.y - TOP) / CS;
                    if (x >= 0 && x < GW && y >= 0 && y < GH) {
                        if (e.buttons & 2) {
                            if (!open_[y][x]) {
                                flag[y][x] = !flag[y][x];
                                nflags += flag[y][x] ? 1 : -1;
                                sound_effect(flag[y][x] ? 900 : 600, flag[y][x] ? 1200 : 450, 25, SND_TRIANGLE);
                            }
                        } else if (e.buttons & 1) {
                            if (!started) plant(x, y);
                            bool was_open = open_[y][x] || flag[y][x];
                            reveal(x, y);
                            if (!was_open && !lost && !won) sound_effect(1800, 1400, 12, SND_TRIANGLE); /* a click */
                        }
                    }
                }
            }
        }
        draw(w);
        win_update(w);
    }
    win_close(w);
    return 0;
}
