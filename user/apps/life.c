/* Conway's Game of Life: the generation, the buttons and the hint are widgets of the
   interface engine (libc/ui.c); the grid is a surface the game draws itself, and the
   mouse paints cells in it. Keys: space pauses, s steps, r random, g glider gun,
   c clears, + and - change the speed, Escape closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define GW 96
#define GH 64
#define CS 7
#define W (GW * CS)
#define H (GH * CS)
#define PAD 12
#define WINDOW 1
#define BOARD 1
#define STATUS_MS 100 /* the generation counter is shown at most this often */

static uint8_t grid[GH][GW], nxt[GH][GW], age[GH][GW];
static bool running = true;
static int gen, speed = 80, paint_val = -1;
/* the buttons' places in the view */
static uint32_t b_play, b_step, b_random, b_gun, b_clear, b_slower, b_faster;

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

static void clear(void) {
    memset(grid, 0, sizeof grid);
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

static int population(void) {
    int pop = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) pop += grid[y][x];
    return pop;
}

static void draw(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    uint32_t dead = t->background, line = t->sidebar;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int px = x * CS, py = y * CS;
            if (grid[y][x]) {
                gfx_fill(c, px, py, CS - 1, CS - 1, gfx_mix(t->success, t->accent, age[y][x] * 4));
                gfx_pixel(c, px, py, t->text);
            } else {
                gfx_fill(c, px, py, CS, CS, dead);
                gfx_pixel(c, px + CS - 1, py + CS - 1, line);
            }
        }
}

/* A compact button of the toolbar; its place in the view is returned. */
static uint32_t tool(struct ui_msg *m, const char *text, const char *icon, const char *variant) {
    uint32_t at = ui_node(m, "button");
    if (text) ui_str(m, "text", text);
    if (icon) ui_str(m, "icon", icon);
    ui_str(m, "variant", variant);
    ui_num(m, "compact", 1);
    ui_end(m);
    return at;
}

static void show(struct ui_session *s) {
    char generation[32], pop[32], rate[32];
    snprintf(generation, sizeof generation, "Generation %d", gen);
    snprintf(pop, sizeof pop, "Population %d", population());
    snprintf(rate, sizeof rate, "Every %d ms", speed);
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", 8);
        ui_node(&m, "row"); ui_num(&m, "gap", 12);
            ui_node(&m, "text"); ui_str(&m, "text", generation); ui_num(&m, "size", 16); ui_num(&m, "weight", 1); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", pop); ui_str(&m, "style", "caption"); ui_end(&m);
            if (!running) {
                ui_node(&m, "text"); ui_str(&m, "text", "Paused"); ui_str(&m, "style", "caption");
                ui_num(&m, "colour", ui_theme.warning); ui_end(&m);
            }
            ui_node(&m, "spacer"); ui_end(&m);
            b_slower = tool(&m, NULL, "minus", "ghost");
            ui_node(&m, "text"); ui_str(&m, "text", rate); ui_str(&m, "style", "caption"); ui_end(&m);
            b_faster = tool(&m, NULL, "plus", "ghost");
        ui_end(&m);
        ui_node(&m, "row"); ui_num(&m, "gap", 8);
            b_play = tool(&m, running ? "Pause" : "Play", NULL, "primary");
            b_step = tool(&m, "Step", NULL, "secondary");
            b_random = tool(&m, "Random", "refresh", "secondary");
            b_gun = tool(&m, "Glider gun", NULL, "secondary");
            b_clear = tool(&m, "Clear", NULL, "secondary");
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", BOARD); ui_num(&m, "pointer", 1);
        ui_num(&m, "width", W); ui_num(&m, "height", H); ui_end(&m);
        ui_node(&m, "text");
        ui_str(&m, "text", "Space: pause   S: step   R: random   G: glider gun   C: clear   + / -: speed   Mouse: paint");
        ui_str(&m, "style", "caption"); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

/* Paints (or erases) the cell under the pointer; whether the grid changed. */
static bool paint(double px, double py, bool first) {
    if (px < 0 || py < 0) return false;
    int x = (int)px / CS, y = (int)py / CS;
    if (x >= GW || y >= GH) return false;
    if (first) paint_val = !grid[y][x];
    if (paint_val < 0) return false;
    bool changed = grid[y][x] != paint_val;
    grid[y][x] = (uint8_t)paint_val;
    age[y][x] = 0;
    return changed;
}

int main(void) {
    srand((unsigned)n_clock_uptime_ms());
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W + 2 * PAD, H + 2 * PAD + 110, 0, "Game of Life", NULL, NULL) < 0) return 1;
    canvas_t *board = ui_surface(s, BOARD, W, H, 0);
    if (!board) return 1;
    gun();
    draw(board);
    show(s);
    uint64_t next = n_clock_uptime_ms(), shown_at = next;
    bool stale = false; /* the generation counter shown is out of date */
    for (bool alive = true; alive;) {
        uint64_t now = n_clock_uptime_ms();
        int wait = -1;
        if (running) wait = next > now ? (int)(next - now) : 0;
        if (stale) {
            int until_status = shown_at + STATUS_MS > now ? (int)(shown_at + STATUS_MS - now) : 0;
            if (wait < 0 || until_status < wait) wait = until_status;
        }
        ui_session_wait(s, wait);
        bool redraw = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            bool view = false; /* the toolbar or the counters changed at once */
            switch (e.type) {
            case UI_EVENT_CLICK:
                if (e.node == b_play) { running = !running; view = true; }
                else if (e.node == b_step) { step(); redraw = view = true; }
                else if (e.node == b_random) { randomize(); redraw = view = true; }
                else if (e.node == b_gun) { gun(); redraw = view = true; }
                else if (e.node == b_clear) { clear(); redraw = view = true; }
                else if (e.node == b_slower) { speed = MIN(1000, speed + 20); view = true; }
                else if (e.node == b_faster) { speed = MAX(10, speed - 20); view = true; }
                break;
            case UI_EVENT_POINTER:
                if (e.flags & UI_UP) paint_val = -1;
                else if (UI_BUTTONS(e.flags) & 1) {
                    if (paint(e.a, e.b, (e.flags & UI_DOWN) != 0)) redraw = stale = true;
                }
                break;
            case UI_EVENT_KEY:
                if (e.flags & UI_UP) break;
                switch ((int)e.a) {
                case ' ': running = !running; view = true; break;
                case 'r': case 'R': randomize(); redraw = view = true; break;
                case 'g': case 'G': gun(); redraw = view = true; break;
                case 'c': case 'C': clear(); redraw = view = true; break;
                case 's': case 'S': step(); redraw = view = true; break;
                case '+': case '=': speed = MAX(10, speed - 20); view = true; break;
                case '-': speed = MIN(1000, speed + 20); view = true; break;
                case NKEY_ESC: alive = false; break;
                }
                break;
            case UI_EVENT_CLOSE:
                alive = false;
                break;
            }
            if (view) stale = true;
            if (view && e.type != UI_EVENT_POINTER) shown_at = 0; /* show it now */
        }
        if (!alive || !ui_session_open(s)) break;
        now = n_clock_uptime_ms();
        if (running && now >= next) {
            next = now + speed;
            step();
            redraw = stale = true;
        }
        if (redraw) {
            draw(board);
            ui_surface_changed(s, BOARD);
        }
        if (stale && now >= shown_at + STATUS_MS) {
            show(s);
            shown_at = now;
            stale = false;
        }
    }
    ui_session_free(s);
    return 0;
}
