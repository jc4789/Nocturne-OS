/* Tetris-style falling blocks: the score panel (next piece, score, lines, level, best
   and the keys) is made of widgets of the interface engine (libc/ui.c); the well and
   the next piece are surfaces the game draws itself. Arrows move, Up or X rotates,
   space drops, P pauses, Enter starts again after the game is over, Escape closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define BW 10
#define BH 20
#define CS 26
#define SIDE 170
#define W (BW * CS)
#define H (BH * CS)
#define PAD 12
#define NCS 22 /* the size of a block in the next-piece preview */
#define NW (4 * NCS)
#define NH (2 * NCS)
#define WINDOW 1
#define WELL 1
#define PREVIEW 2

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

/* One block of size cs at x, y: a rounded tile that is lighter at the top. */
static void block(canvas_t *c, float x, float y, float cs, uint32_t col) {
    ui_round_gradient(c, x + 0.5f, y + 0.5f, cs - 2, cs - 2, 4, gfx_mix(col, RGB(255, 255, 255), 70),
                      gfx_mix(col, RGB(0, 0, 0), 70));
}

static void draw_well(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, W, H, t->sidebar);
    for (int x = 1; x < BW; x++) gfx_vline(c, x * CS - 1, 0, H, t->surface);
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            if (board[y][x]) block(c, (float)(x * CS), (float)(y * CS), CS, colors[board[y][x]]);
    if (!over) {
        int gy = py;
        while (fits(cur, rot, px, gy + 1)) gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (cell(cur, rot, x, y)) {
                    if (gy + y >= 0)
                        ui_round_stroke(c, (float)((px + x) * CS) + 1, (float)((gy + y) * CS) + 1, CS - 3, CS - 3, 4, 1.5f,
                                        gfx_mix(t->sidebar, colors[cur + 1], 110));
                    if (py + y >= 0) block(c, (float)((px + x) * CS), (float)((py + y) * CS), CS, colors[cur + 1]);
                }
    }
    if (over || paused) {
        gfx_fill_blend(c, 0, 0, W, H, ARGB(160, 8, 6, 16));
        const char *big = over ? "GAME OVER" : "PAUSED";
        const char *small = over ? "Enter: new game" : "P: resume";
        float bs = 28, ss = 14, bw = ui_text_width(big, strlen(big), bs, 1), sw = ui_text_width(small, strlen(small), ss, 0);
        float bl = ui_line_height(bs, 1, NULL), sl = ui_line_height(ss, 0, NULL);
        float top = ((float)H - bl - 8 - sl) / 2;
        ui_text(c, ((float)W - bw) / 2, top, big, strlen(big), bs, 1, t->text);
        ui_text(c, ((float)W - sw) / 2, top + bl + 8, small, strlen(small), ss, 0, t->textDim);
    }
}

/* The next piece, centred in its small surface (which lies on a card). */
static void draw_next(canvas_t *c) {
    gfx_fill(c, 0, 0, NW, NH, ui_theme.surface);
    int x0 = 4, x1 = -1, y0 = 4, y1 = -1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(nextp, 0, x, y)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    float ox = ((float)NW - (float)((x1 - x0 + 1) * NCS)) / 2, oy = ((float)NH - (float)((y1 - y0 + 1) * NCS)) / 2;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (cell(nextp, 0, x, y)) block(c, ox + (float)((x - x0) * NCS), oy + (float)((y - y0) * NCS), NCS, colors[nextp + 1]);
}

static void caption(struct ui_msg *m, const char *text) {
    ui_node(m, "text"); ui_str(m, "text", text); ui_str(m, "style", "caption"); ui_end(m);
}

static void stat(struct ui_msg *m, const char *label, int value) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    ui_node(m, "column"); ui_num(m, "gap", 2);
        caption(m, label);
        ui_node(m, "text"); ui_str(m, "text", buf); ui_num(m, "size", 22); ui_num(m, "weight", 1); ui_end(m);
    ui_end(m);
}

static void show(struct ui_session *s) {
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "row"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", PAD); ui_str(&m, "align", "stretch");
        ui_node(&m, "surface"); ui_num(&m, "surface", WELL); ui_num(&m, "width", W); ui_num(&m, "height", H); ui_end(&m);
        ui_node(&m, "card"); ui_num(&m, "width", SIDE); ui_num(&m, "gap", 12);
            caption(&m, "NEXT");
            ui_node(&m, "surface"); ui_num(&m, "surface", PREVIEW); ui_num(&m, "width", NW); ui_num(&m, "height", NH); ui_end(&m);
            ui_node(&m, "divider"); ui_end(&m);
            stat(&m, "SCORE", score);
            stat(&m, "LINES", lines);
            stat(&m, "LEVEL", level);
            stat(&m, "BEST", best);
            ui_node(&m, "spacer"); ui_end(&m);
            caption(&m, "Arrows: move");
            caption(&m, "Up or X: rotate");
            caption(&m, "Space: drop");
            caption(&m, "P: pause");
        ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

int main(void) {
    srand((unsigned)n_clock_uptime_ms());
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W + SIDE + 3 * PAD, H + 2 * PAD, 0, "Tetris", NULL, NULL) < 0) return 1;
    canvas_t *well = ui_surface(s, WELL, W, H, 0), *preview = ui_surface(s, PREVIEW, NW, NH, 0);
    if (!well || !preview) return 1;
    reset();
    draw_well(well);
    draw_next(preview);
    show(s);
    int shown_score = score, shown_lines = lines, shown_level = level, shown_best = best, shown_next = nextp;
    uint64_t next = n_clock_uptime_ms() + 800;
    for (bool running = true; running;) {
        int delay = 800 - (level - 1) * 70;
        if (delay < 90) delay = 90;
        int64_t wait = (int64_t)(next - n_clock_uptime_ms());
        ui_session_wait(s, wait > 0 ? (int)wait : 0);
        bool redraw = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            if (e.type == UI_EVENT_CLOSE) {
                running = false;
                continue;
            }
            if (e.type != UI_EVENT_KEY || (e.flags & UI_UP)) continue;
            int k = (int)e.a;
            if (k == NKEY_ESC) {
                running = false;
                break;
            }
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
                    next = n_clock_uptime_ms() + delay;
                }
            }
            redraw = true;
        }
        if (!running || !ui_session_open(s)) break;
        if ((int64_t)(next - n_clock_uptime_ms()) <= 0) {
            next = n_clock_uptime_ms() + delay;
            if (!over && !paused) {
                if (fits(cur, rot, px, py + 1)) py++;
                else lock_piece();
            }
            redraw = true;
        }
        if (redraw) {
            draw_well(well);
            ui_surface_changed(s, WELL);
        }
        if (nextp != shown_next) { /* the next piece changed */
            shown_next = nextp;
            draw_next(preview);
            ui_surface_changed(s, PREVIEW);
        }
        if (score != shown_score || lines != shown_lines || level != shown_level || best != shown_best) {
            shown_score = score;
            shown_lines = lines;
            shown_level = level;
            shown_best = best;
            show(s); /* the numbers in the panel */
        }
    }
    ui_session_free(s);
    return 0;
}
