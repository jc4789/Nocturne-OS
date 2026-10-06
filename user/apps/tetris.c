/* Tetris-style falling blocks */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define BW 10
#define BH 20
#define CS 26
#define SIDE 170
#define W (BW * CS + SIDE)
#define H (BH * CS)

static const uint32_t colors[8] = {0, RGB(100, 220, 240), RGB(250, 210, 80), RGB(190, 120, 250), RGB(110, 220, 120),
                                   RGB(240, 90, 110), RGB(100, 140, 250), RGB(250, 160, 80)};
/* 7 pieces x 4 rotations, each 4 cells packed as 16-bit masks over a 4x4 grid */
static const uint16_t shapes[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, /* I */
    {0x6600, 0x6600, 0x6600, 0x6600}, /* O */
    {0x0E40, 0x4C40, 0x4E00, 0x4640}, /* T */
    {0x06C0, 0x8C40, 0x6C00, 0x4620}, /* S */
    {0x0C60, 0x4C80, 0xC600, 0x2640}, /* Z */
    {0x0E20, 0x44C0, 0x8E00, 0x6440}, /* J */
    {0x0E80, 0xC440, 0x2E00, 0x4460}, /* L */
};

static uint8_t board[BH][BW];
static int cur, rot, px, py, nextp, score, lines, level, best;
static bool over, paused;

static bool cell(int p, int r, int x, int y) { return shapes[p][r] & (0x8000 >> (y * 4 + x)); }

static bool fits(int p, int r, int x0, int y0) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell(p, r, x, y)) continue;
            int bx = x0 + x, by = y0 + y;
            if (bx < 0 || bx >= BW || by >= BH) return false;
            if (by >= 0 && board[by][bx]) return false;
        }
    return true;
}

static void spawn_piece(void) {
    cur = nextp;
    nextp = rand() % 7;
    rot = 0;
    px = 3;
    py = -1;
    if (!fits(cur, rot, px, py)) {
        over = true;
        if (score > best) best = score;
        sound_effect(330, 55, 160, SND_SAW);
    }
}

static void reset(void) {
    memset(board, 0, sizeof board);
    score = lines = 0;
    level = 1;
    over = paused = false;
    nextp = rand() % 7;
    spawn_piece();
}

static void lock_piece(void) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(cur, rot, x, y) && py + y >= 0) board[py + y][px + x] = (uint8_t)(cur + 1);
    int cleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BW; x++)
            if (!board[y][x]) full = false;
        if (full) {
            memmove(board[1], board[0], sizeof(board[0]) * y);
            memset(board[0], 0, sizeof board[0]);
            cleared++;
            y++;
        }
    }
    if (cleared == 4) { /* a rising fanfare */
        sound_effect(523, 784, 60, SND_SQUARE);
        sound_effect(784, 1568, 90, SND_SQUARE);
    } else if (cleared) {
        sound_effect(440 + 110 * cleared, 880 + 220 * cleared, 50 + 20 * cleared, SND_SQUARE);
    } else {
        sound_effect(140, 70, 30, SND_TRIANGLE); /* a thud */
    }
    static const int pts[] = {0, 100, 300, 500, 800};
    score += pts[cleared] * level;
    lines += cleared;
    level = 1 + lines / 10;
    spawn_piece();
}

static void block(canvas_t *c, int x, int y, uint32_t col) {
    gfx_fill(c, x, y, CS - 1, CS - 1, col);
    gfx_hline(c, x, y, CS - 1, gfx_mix(col, RGB(255, 255, 255), 110));
    gfx_vline(c, x, y, CS - 1, gfx_mix(col, RGB(255, 255, 255), 70));
    gfx_hline(c, x, y + CS - 2, CS - 1, gfx_mix(col, RGB(0, 0, 0), 90));
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    gfx_fill(c, 0, 0, BW * CS, H, RGB(14, 12, 26));
    for (int x = 1; x < BW; x++) gfx_vline(c, x * CS - 1, 0, H, RGB(22, 20, 38));
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            if (board[y][x]) block(c, x * CS, y * CS, colors[board[y][x]]);
    if (!over) {
        int gy = py;
        while (fits(cur, rot, px, gy + 1)) gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (cell(cur, rot, x, y)) {
                    if (gy + y >= 0) gfx_rect(c, (px + x) * CS, (gy + y) * CS, CS - 1, CS - 1, RGB(90, 86, 130));
                    if (py + y >= 0) block(c, (px + x) * CS, (py + y) * CS, colors[cur + 1]);
                }
    }
    int sx = BW * CS;
    gfx_gradient_v(c, sx, 0, SIDE, H, RGB(34, 30, 62), RGB(24, 20, 44));
    gfx_text(c, sx + 16, 16, "NEXT", UI_DIM, TRANSPARENT, FONT_SMALL);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(nextp, 0, x, y)) block(c, sx + 30 + x * CS, 44 + y * CS, colors[nextp + 1]);
    char buf[32];
    const char *lab[] = {"SCORE", "LINES", "LEVEL", "BEST"};
    int val[] = {score, lines, level, best};
    for (int i = 0; i < 4; i++) {
        gfx_text(c, sx + 16, 160 + i * 56, lab[i], UI_DIM, TRANSPARENT, FONT_SMALL);
        snprintf(buf, sizeof buf, "%d", val[i]);
        gfx_text(c, sx + 16, 178 + i * 56, buf, UI_FG, TRANSPARENT, FONT_SMALL);
    }
    gfx_text(c, sx + 12, H - 70, "arrows move", UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, sx + 12, H - 52, "up rotates", UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, sx + 12, H - 34, "space drops", UI_DIM, TRANSPARENT, FONT_SMALL);
    if (over || paused) {
        gfx_fill_blend(c, 0, 0, BW * CS, H, ARGB(160, 8, 6, 16));
        ui_text_center(c, 0, H / 2 - 30, BW * CS, over ? "GAME OVER" : "PAUSED", RGB(255, 255, 255), FONT_LARGE);
        ui_text_center(c, 0, H / 2 + 10, BW * CS, over ? "Enter: new game" : "P: resume", UI_DIM, FONT_SMALL);
    }
}

int main(void) {
    srand((unsigned)uptime_ms());
    window_t *w = win_open(W, H, "Tetris", 0);
    if (!w) return 1;
    reset();
    uint64_t next = uptime_ms() + 800;
    for (;;) {
        int delay = 800 - (level - 1) * 70;
        if (delay < 90) delay = 90;
        int64_t wait = (int64_t)(next - uptime_ms());
        struct gui_event e;
        int r = win_event(w, &e, wait > 0 ? (int)wait : 0);
        if (r < 0) break;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type != EV_KEY || !e.pressed) continue;
            int k = e.key;
            if (k == NKEY_ESC) break;
            if (over) {
                if (k == NKEY_ENTER) reset();
            } else if (k == 'p' || k == 'P') {
                paused = !paused;
            } else if (!paused) {
                if (k == NKEY_LEFT && fits(cur, rot, px - 1, py)) px--;
                else if (k == NKEY_RIGHT && fits(cur, rot, px + 1, py)) px++;
                else if (k == NKEY_DOWN) {
                    if (fits(cur, rot, px, py + 1)) {
                        py++;
                        score++;
                    }
                } else if (k == NKEY_UP || k == 'x') {
                    int nr = (rot + 1) % 4;
                    for (int kick = 0; kick < 3; kick++) {
                        int dx = kick == 0 ? 0 : kick == 1 ? -1 : 1;
                        if (fits(cur, nr, px + dx, py)) {
                            rot = nr;
                            px += dx;
                            sound_effect(1200, 1500, 15, SND_TRIANGLE);
                            break;
                        }
                    }
                } else if (k == ' ') {
                    while (fits(cur, rot, px, py + 1)) {
                        py++;
                        score += 2;
                    }
                    lock_piece();
                    next = uptime_ms() + delay;
                }
            }
            draw(w);
            win_update(w);
            continue;
        }
        next = uptime_ms() + delay;
        if (!over && !paused) {
            if (fits(cur, rot, px, py + 1)) py++;
            else lock_piece();
        }
        draw(w);
        win_update(w);
    }
    win_close(w);
    return 0;
}
