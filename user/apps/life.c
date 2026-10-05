/* Conway's Game of Life */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define GW 96
#define GH 64
#define CS 7
#define TOP 30
#define W (GW * CS)
#define H (GH * CS + TOP)

static uint8_t grid[GH][GW], nxt[GH][GW], age[GH][GW];
static bool running = true;
static int gen, speed = 80, paint_val = -1;

static void randomize(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            grid[y][x] = rand() % 5 == 0;
            age[y][x] = 0;
        }
    gen = 0;
}

static void put(const char **pat, int ox, int oy) {
    for (int y = 0; pat[y]; y++)
        for (int x = 0; pat[y][x]; x++)
            if (pat[y][x] == 'O') grid[(oy + y) % GH][(ox + x) % GW] = 1;
}

static void gun(void) {
    memset(grid, 0, sizeof grid);
    memset(age, 0, sizeof age);
    static const char *g[] = {
        "........................O...........",
        "......................O.O...........",
        "............OO......OO............OO",
        "...........O...O....OO............OO",
        "OO........O.....O...OO..............",
        "OO........O...O.OO....O.O...........",
        "..........O.....O.......O...........",
        "...........O...O....................",
        "............OO......................",
        0};
    put(g, 4, 4);
    gen = 0;
}

static void step(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy) n += grid[(y + dy + GH) % GH][(x + dx + GW) % GW];
            nxt[y][x] = n == 3 || (n == 2 && grid[y][x]);
            if (nxt[y][x]) age[y][x] = grid[y][x] ? (uint8_t)MIN(age[y][x] + 1, 60) : 0;
        }
    memcpy(grid, nxt, sizeof grid);
    gen++;
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    gfx_fill(c, 0, 0, W, TOP, RGB(30, 26, 56));
    int pop = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) pop += grid[y][x];
    char b[128];
    snprintf(b, sizeof b, "gen %d  pop %d  %s", gen, pop, running ? "" : "[paused]");
    gfx_text(c, 8, 7, b, UI_FG, TRANSPARENT, FONT_SMALL);
    const char *help = "space pause  r random  g gun  c clear  +/- speed";
    gfx_text(c, W - 8 - gfx_text_width(help, FONT_SMALL), 7, help, UI_DIM, TRANSPARENT, FONT_SMALL);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int px = x * CS, py = TOP + y * CS;
            if (grid[y][x]) {
                uint32_t col = gfx_mix(RGB(150, 255, 200), RGB(120, 110, 250), age[y][x] * 4);
                gfx_fill(c, px, py, CS - 1, CS - 1, col);
                gfx_pixel(c, px, py, RGB(230, 255, 240));
            } else {
                gfx_fill(c, px, py, CS, CS, RGB(14, 12, 26));
                gfx_pixel(c, px + CS - 1, py + CS - 1, RGB(24, 22, 40));
            }
        }
}

int main(void) {
    srand((unsigned)uptime_ms());
    window_t *w = win_open(W, H, "Game of Life", 0);
    if (!w) return 1;
    gun();
    uint64_t next = uptime_ms();
    for (;;) {
        int64_t wait = (int64_t)(next - uptime_ms());
        struct gui_event e;
        int r = win_event(w, &e, running ? (wait > 0 ? (int)wait : 0) : -1);
        if (r < 0) break;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_KEY && e.pressed) {
                switch (e.key) {
                case ' ': running = !running; break;
                case 'r': case 'R': randomize(); break;
                case 'g': case 'G': gun(); break;
                case 'c': case 'C': memset(grid, 0, sizeof grid); gen = 0; break;
                case 's': case 'S': step(); break;
                case '+': case '=': speed = MAX(10, speed - 20); break;
                case '-': speed = MIN(1000, speed + 20); break;
                case NKEY_ESC: goto out;
                }
            } else if ((e.type == EV_MOUSE_DOWN || e.type == EV_MOUSE_MOVE) && (e.buttons & 1) && e.y >= TOP) {
                int x = e.x / CS, y = (e.y - TOP) / CS;
                if (x >= 0 && x < GW && y < GH) {
                    if (e.type == EV_MOUSE_DOWN) paint_val = !grid[y][x];
                    if (paint_val >= 0) {
                        grid[y][x] = (uint8_t)paint_val;
                        age[y][x] = 0;
                    }
                }
            } else if (e.type == EV_MOUSE_UP) {
                paint_val = -1;
                continue;
            } else continue;
            draw(w);
            win_update(w);
            continue;
        }
        next = uptime_ms() + speed;
        step();
        draw(w);
        win_update(w);
    }
out:
    win_close(w);
    return 0;
}
