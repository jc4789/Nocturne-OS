/* Snake: the score and the best score are widgets of the interface engine
   (libc/ui.c); the field is a surface the game draws itself. Arrows steer, P or
   space pauses, Enter starts again after a crash, Escape closes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "ui.h"

#define GW 28
#define GH 20
#define CELL 20
#define W (GW * CELL)
#define H (GH * CELL)
#define PAD 12
#define GAP 8
#define HEADER 28 /* the height of the row with the score */
#define WINDOW 1
#define FIELD 1

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

static void draw(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            gfx_fill(c, x * CELL, y * CELL, CELL, CELL, (x + y) & 1 ? t->background : t->sidebar);
    float fcx = (float)(fx * CELL), fcy = (float)(fy * CELL);
    ui_round_fill(c, fcx + 3, fcy + 4, CELL - 6, CELL - 6, (CELL - 6) / 2.0f, t->danger);
    ui_line(c, fcx + CELL / 2.0f, fcy + 5, fcx + CELL / 2.0f + 2, fcy + 1.5f, 2, t->success);
    for (int i = len - 1; i >= 0; i--) {
        int mix = len > 1 ? i * 120 / (len - 1) : 0;
        uint32_t col = gfx_mix(t->success, t->accent, mix);
        ui_round_fill(c, (float)(sx[i] * CELL + 1), (float)(sy[i] * CELL + 1), CELL - 2, CELL - 2, 5, col);
    }
    float hx = (float)(sx[0] * CELL), hy = (float)(sy[0] * CELL);
    ui_round_fill(c, hx + 5, hy + 5, 3.5f, 3.5f, 1.75f, t->background);
    ui_round_fill(c, hx + 12, hy + 5, 3.5f, 3.5f, 1.75f, t->background);
    if (dead || paused) {
        gfx_fill_blend(c, 0, 0, W, H, ARGB(150, 10, 8, 20));
        const char *big = dead ? "GAME OVER" : "PAUSED";
        const char *small = dead ? "press Enter to play again" : "press P to continue";
        float bs = 30, ss = 14, bw = ui_text_width(big, strlen(big), bs, 1), sw = ui_text_width(small, strlen(small), ss, 0);
        float bl = ui_line_height(bs, 1, NULL), sl = ui_line_height(ss, 0, NULL);
        float top = ((float)H - bl - 8 - sl) / 2;
        ui_text(c, ((float)W - bw) / 2, top, big, strlen(big), bs, 1, t->text);
        ui_text(c, ((float)W - sw) / 2, top + bl + 8, small, strlen(small), ss, 0, t->textDim);
    }
}

static void show(struct ui_session *s) {
    char score_text[32], best_text[32];
    snprintf(score_text, sizeof score_text, "Score %d", score);
    snprintf(best_text, sizeof best_text, "Best %d", best);
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", GAP);
        ui_node(&m, "row");
            ui_node(&m, "text"); ui_str(&m, "text", score_text); ui_num(&m, "size", 18); ui_num(&m, "weight", 1); ui_end(&m);
            ui_node(&m, "spacer"); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", best_text); ui_num(&m, "size", 18); ui_num(&m, "weight", 1);
            ui_num(&m, "colour", ui_theme.accent); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", FIELD);
        ui_num(&m, "width", W); ui_num(&m, "height", H); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

int main(void) {
    srand((unsigned)n_clock_uptime_ms());
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W + 2 * PAD, H + 2 * PAD + GAP + HEADER, 0, "Snake", NULL, NULL) < 0) return 1;
    canvas_t *field = ui_surface(s, FIELD, W, H, 0);
    if (!field) return 1;
    reset();
    draw(field);
    ui_surface_changed(s, FIELD);
    show(s);
    int shown_score = score, shown_best = best;
    uint64_t next = n_clock_uptime_ms();
    for (bool running = true; running;) {
        int speed = 130 - (score / 50) * 8;
        if (speed < 55) speed = 55;
        int64_t wait = (int64_t)(next - n_clock_uptime_ms());
        ui_session_wait(s, wait > 0 ? (int)wait : 0);
        bool redraw = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            if (e.type == UI_EVENT_CLOSE) {
                running = false;
            } else if (e.type == UI_EVENT_KEY && !(e.flags & UI_UP)) {
                int k = (int)e.a;
                if (k == NKEY_RIGHT && dir != 2) next_dir = 0;
                else if (k == NKEY_DOWN && dir != 3) next_dir = 1;
                else if (k == NKEY_LEFT && dir != 0) next_dir = 2;
                else if (k == NKEY_UP && dir != 1) next_dir = 3;
                else if ((k == 'p' || k == 'P' || k == ' ') && !dead) paused = !paused;
                else if (k == NKEY_ENTER && dead) reset();
                else if (k == NKEY_ESC) running = false;
                redraw = true;
            }
        }
        if (!running || !ui_session_open(s)) break;
        if ((int64_t)(next - n_clock_uptime_ms()) <= 0) {
            next = n_clock_uptime_ms() + speed;
            if (!dead && !paused) step();
            redraw = true;
        }
        if (redraw) {
            draw(field);
            ui_surface_changed(s, FIELD);
        }
        if (score != shown_score || best != shown_best) { /* only when the numbers change */
            shown_score = score;
            shown_best = best;
            show(s);
        }
    }
    ui_session_free(s);
    return 0;
}
