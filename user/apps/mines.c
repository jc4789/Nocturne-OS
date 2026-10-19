/* Minesweeper: the counters and New game are widgets of the interface engine
   (libc/ui.c); the board is a surface the game draws itself. Left click opens a
   cell, right click flags it; N or F2 starts again, Escape closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define GW 16
#define GH 16
#define MINES 40
#define CS 24
#define BW (GW * CS)
#define BH (GH * CS)
#define PAD 12
#define WINDOW 1
#define BOARD 1

static bool mine[GH][GW], open_[GH][GW], flag[GH][GW];
static int nflags, nopen, hot_x = -1, hot_y = -1;
static bool lost, won, started;
static uint64_t t0, t_end;
static uint32_t new_game; /* the button's place in the view */

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
    t0 = n_clock_uptime_ms();
}

static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= GW || y >= GH || open_[y][x] || flag[y][x]) return;
    open_[y][x] = true;
    nopen++;
    if (mine[y][x]) {
        lost = true;
        t_end = n_clock_uptime_ms();
        sound_effect(220, 30, 160, SND_NOISE);
        return;
    }
    if (count(x, y) == 0)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) reveal(x + dx, y + dy);
    if (nopen == GW * GH - MINES && !lost) {
        won = true;
        t_end = n_clock_uptime_ms();
        sound_effect(523, 523, 50, SND_TRIANGLE);
        sound_effect(659, 659, 50, SND_TRIANGLE);
        sound_effect(784, 1047, 60, SND_TRIANGLE);
    }
}

static int seconds(void) {
    int secs = started ? (int)((((lost || won) ? t_end : n_clock_uptime_ms()) - t0) / 1000) : 0;
    return secs > 999 ? 999 : secs;
}

static void draw_board(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    static const uint32_t numc[9] = {0, RGB(110, 160, 255), RGB(111, 214, 160), RGB(240, 122, 122), RGB(170, 140, 250),
                                     RGB(242, 160, 90), RGB(90, 210, 220), RGB(230, 230, 240), RGB(160, 160, 175)};
    gfx_fill(c, 0, 0, BW, BH, t->background);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            float px = (float)(x * CS) + 1, py = (float)(y * CS) + 1, s = CS - 2;
            if (open_[y][x] || (lost && mine[y][x])) {
                ui_round_fill(c, px, py, s, s, 4, mine[y][x] && open_[y][x] ? RGB(150, 52, 70) : t->surface);
                if (mine[y][x]) {
                    ui_round_fill(c, px + 6, py + 6, s - 12, s - 12, (s - 12) / 2, RGB(20, 18, 28));
                    ui_round_fill(c, px + 8, py + 8, 3, 3, 1.5f, RGB(230, 230, 240));
                } else {
                    int n = count(x, y);
                    if (n) {
                        char d = (char)('0' + n);
                        float tw = ui_text_width(&d, 1, 14, 1), lh = ui_line_height(14, 1, NULL);
                        ui_text(c, px + (s - tw) / 2, py + (s - lh) / 2, &d, 1, 14, 1, numc[n]);
                    }
                }
            } else {
                bool hot = x == hot_x && y == hot_y && !lost && !won;
                ui_round_gradient(c, px, py, s, s, 4, hot ? t->controlHover : RGB(60, 56, 84), hot ? t->control : RGB(48, 45, 68));
                if (flag[y][x]) {
                    ui_line(c, px + 8, py + 5, px + 8, py + s - 5, 2, t->textDim);
                    gfx_triangle(c, (int)px + 9, (int)py + 5, (int)px + 9, (int)py + 13, (int)px + 17, (int)py + 9, t->danger);
                }
            }
        }
    if (won || lost) {
        const char *msg = won ? "You win!" : "Boom! Press New game";
        float size = 22, lh = ui_line_height(size, 1, NULL), tw = ui_text_width(msg, strlen(msg), size, 1);
        ui_round_fill(c, (BW - tw) / 2 - 20, (BH - lh) / 2 - 12, tw + 40, lh + 24, 12, ARGB(210, 22, 21, 29));
        ui_text(c, (BW - tw) / 2, (BH - lh) / 2, msg, strlen(msg), size, 1, won ? t->warning : t->text);
    }
}

static void counter(struct ui_msg *m, const char *icon, const char *text) {
    ui_node(m, "row");
    ui_num(m, "gap", 6);
    ui_node(m, "icon"); ui_str(m, "icon", icon); ui_end(m);
    ui_node(m, "text"); ui_str(m, "text", text); ui_num(m, "size", 18); ui_num(m, "weight", 1); ui_end(m);
    ui_end(m);
}

static void show(struct ui_session *s) {
    char mines_left[16], time[16];
    snprintf(mines_left, sizeof mines_left, "%d", MINES - nflags);
    snprintf(time, sizeof time, "%03d", seconds());
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", PAD);
        ui_node(&m, "row");
            counter(&m, "info", mines_left);
            ui_node(&m, "spacer"); ui_end(&m);
            new_game = ui_node(&m, "button");
            ui_str(&m, "text", "New game"); ui_str(&m, "icon", "refresh"); ui_end(&m);
            ui_node(&m, "spacer"); ui_end(&m);
            counter(&m, "clock", time);
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", BOARD); ui_num(&m, "pointer", 1);
        ui_num(&m, "width", BW); ui_num(&m, "height", BH); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

static void press(int x, int y, uint32_t buttons) {
    if (lost || won || x < 0 || x >= GW || y < 0 || y >= GH) return;
    if (buttons & 2) {
        if (!open_[y][x]) {
            flag[y][x] = !flag[y][x];
            nflags += flag[y][x] ? 1 : -1;
            sound_effect(flag[y][x] ? 900 : 600, flag[y][x] ? 1200 : 450, 25, SND_TRIANGLE);
        }
    } else if (buttons & 1) {
        if (!started) plant(x, y);
        bool was_open = open_[y][x] || flag[y][x];
        reveal(x, y);
        if (!was_open && !lost && !won) sound_effect(1800, 1400, 12, SND_TRIANGLE); /* a click */
    }
}

int main(void) {
    srand((unsigned)n_clock_uptime_ms());
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, BW + 2 * PAD, BH + 3 * PAD + 40, 0, "Minesweeper", NULL, NULL) < 0) return 1;
    canvas_t *board = ui_surface(s, BOARD, BW, BH, 0);
    if (!board) return 1;
    reset();
    draw_board(board);
    show(s);
    int shown = -1;
    for (bool running = true; running;) {
        ui_session_wait(s, started && !lost && !won ? 250 : -1);
        bool changed = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLICK:
                if (e.node == new_game) { reset(); changed = true; }
                break;
            case UI_EVENT_POINTER: {
                int x = (int)e.a / CS, y = (int)e.b / CS;
                if (e.a < 0 || e.b < 0) x = y = -1;
                if (e.flags & UI_DOWN) { press(x, y, UI_BUTTONS(e.flags)); changed = true; }
                if (x != hot_x || y != hot_y) { hot_x = x; hot_y = y; changed = true; }
                break;
            }
            case UI_EVENT_KEY:
                if (e.flags & UI_UP) break;
                if (!strcmp(e.text, "Escape")) running = false;
                else if (!strcmp(e.text, "n") || !strcmp(e.text, "F2")) { reset(); changed = true; }
                break;
            case UI_EVENT_CLOSE:
                running = false;
                break;
            }
        }
        if (!running || !ui_session_open(s)) break;
        if (changed) {
            draw_board(board);
            ui_surface_changed(s, BOARD);
        }
        if (changed || seconds() != shown) {
            shown = seconds();
            show(s);
        }
    }
    ui_session_free(s);
    return 0;
}
