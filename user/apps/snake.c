/* Snake */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define GW 28
#define GH 20
#define CELL 20
#define TOP 32
#define W (GW * CELL)
#define H (GH * CELL + TOP)

static int sx[GW * GH], sy[GW * GH], len, dir, next_dir, fx, fy, score, best;
static bool dead, paused;

static void place_food(void) {
    for (;;) {
        fx = rand() % GW;
        fy = rand() % GH;
        bool ok = true;
        for (int i = 0; i < len; i++)
            if (sx[i] == fx && sy[i] == fy) ok = false;
        if (ok) return;
    }
}

static void reset(void) {
    len = 4;
    for (int i = 0; i < len; i++) {
        sx[i] = GW / 2 - i;
        sy[i] = GH / 2;
    }
    dir = next_dir = 0;
    score = 0;
    dead = false;
    paused = false;
    place_food();
}

static void step(void) {
    static const int dx[] = {1, 0, -1, 0}, dy[] = {0, 1, 0, -1};
    dir = next_dir;
    int nx = (sx[0] + dx[dir] + GW) % GW, ny = (sy[0] + dy[dir] + GH) % GH;
    for (int i = 0; i < len - 1; i++)
        if (sx[i] == nx && sy[i] == ny) {
            dead = true;
            if (score > best) best = score;
            sound_effect(300, 60, 160, SND_SAW);
            return;
        }
    bool grow = nx == fx && ny == fy;
    if (grow && len < GW * GH) len++;
    memmove(sx + 1, sx, sizeof(int) * (len - 1));
    memmove(sy + 1, sy, sizeof(int) * (len - 1));
    sx[0] = nx;
    sy[0] = ny;
    if (grow) {
        score += 10;
        place_food();
        sound_effect(660, 1320, 60, SND_SQUARE);
    }
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    gfx_fill(c, 0, 0, W, TOP, RGB(30, 26, 56));
    char buf[64];
    snprintf(buf, sizeof buf, "Score %d", score);
    gfx_text(c, 10, 8, buf, UI_FG, TRANSPARENT, FONT_SMALL);
    snprintf(buf, sizeof buf, "Best %d", best);
    gfx_text(c, W - 10 - gfx_text_width(buf, FONT_SMALL), 8, buf, UI_ACCENT2, TRANSPARENT, FONT_SMALL);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            gfx_fill(c, x * CELL, TOP + y * CELL, CELL, CELL, (x + y) & 1 ? RGB(18, 22, 30) : RGB(22, 26, 36));
    gfx_fill_circle(c, fx * CELL + CELL / 2, TOP + fy * CELL + CELL / 2, CELL / 2 - 2, RGB(240, 80, 100));
    gfx_fill(c, fx * CELL + CELL / 2, TOP + fy * CELL + 1, 2, 4, RGB(110, 200, 100));
    for (int i = len - 1; i >= 0; i--) {
        int t = len > 1 ? i * 120 / (len - 1) : 0;
        uint32_t col = gfx_mix(RGB(140, 255, 160), RGB(40, 140, 90), t);
        gfx_fill_round(c, sx[i] * CELL + 1, TOP + sy[i] * CELL + 1, CELL - 2, CELL - 2, 5, col);
    }
    int hx = sx[0] * CELL, hy = TOP + sy[0] * CELL;
    gfx_fill(c, hx + 5, hy + 5, 3, 3, RGB(20, 20, 30));
    gfx_fill(c, hx + 12, hy + 5, 3, 3, RGB(20, 20, 30));
    if (dead || paused) {
        gfx_fill_blend(c, 0, TOP, W, H - TOP, ARGB(150, 10, 8, 20));
        ui_text_center(c, 0, H / 2 - 20, W, dead ? "GAME OVER" : "PAUSED", RGB(255, 255, 255), FONT_LARGE);
        ui_text_center(c, 0, H / 2 + 20, W, dead ? "press Enter to play again" : "press P to continue", UI_DIM,
                       FONT_SMALL);
    }
}

int main(void) {
    srand((unsigned)uptime_ms());
    window_t *w = win_open(W, H, "Snake", 0);
    if (!w) return 1;
    reset();
    uint64_t next = uptime_ms();
    for (;;) {
        int speed = 130 - (score / 50) * 8;
        if (speed < 55) speed = 55;
        int64_t wait = (int64_t)(next - uptime_ms());
        struct gui_event e;
        int r = win_event(w, &e, wait > 0 ? (int)wait : 0);
        if (r < 0) break;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_KEY && e.pressed) {
                int k = e.key;
                if (k == NKEY_RIGHT && dir != 2) next_dir = 0;
                else if (k == NKEY_DOWN && dir != 3) next_dir = 1;
                else if (k == NKEY_LEFT && dir != 0) next_dir = 2;
                else if (k == NKEY_UP && dir != 1) next_dir = 3;
                else if ((k == 'p' || k == 'P' || k == ' ') && !dead) paused = !paused;
                else if (k == NKEY_ENTER && dead) reset();
                else if (k == NKEY_ESC) break;
                draw(w);
                win_update(w);
            }
            continue;
        }
        next = uptime_ms() + speed;
        if (!dead && !paused) step();
        draw(w);
        win_update(w);
    }
    win_close(w);
    return 0;
}
