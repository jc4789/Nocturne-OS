/* Calculator */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "nocturne.h"

#define W 300
#define H 416
#define BW 66
#define BH 54
#define GAP 6
#define TOP 110

static const char *keys[5][4] = {
    {"C", "+/-", "%", "/"},
    {"7", "8", "9", "*"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"0", ".", "<", "="},
};

static char display[64] = "0";
static char expr_line[64] = "";
static double acc;
static char op;
static bool fresh = true;
static int hot_r = -1, hot_c = -1, down_r = -1, down_c = -1;

static void fmt(double v, char *out) {
    if (isnan(v) || isinf(v)) {
        strcpy(out, "Error");
        return;
    }
    snprintf(out, 32, "%.12g", v);
}

static double apply(double a, double b, char o) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? NAN : a / b;
    default: return b;
    }
}

static void press(const char *k) {
    if (isdigit((unsigned char)k[0]) || !strcmp(k, ".")) {
        if (fresh || !strcmp(display, "0") || !strcmp(display, "Error")) {
            strcpy(display, k[0] == '.' ? "0." : k);
            fresh = false;
        } else if (strlen(display) < 16 && !(k[0] == '.' && strchr(display, '.'))) {
            strcat(display, k);
        }
    } else if (!strcmp(k, "C")) {
        strcpy(display, "0");
        expr_line[0] = 0;
        acc = 0;
        op = 0;
        fresh = true;
    } else if (!strcmp(k, "<")) {
        size_t n = strlen(display);
        if (!fresh && n > 1) display[n - 1] = 0;
        else strcpy(display, "0");
    } else if (!strcmp(k, "+/-")) {
        double v = -atof(display);
        fmt(v, display);
    } else if (!strcmp(k, "%")) {
        double v = atof(display) / 100.0;
        if (op == '+' || op == '-') v *= acc;
        fmt(v, display);
    } else if (!strcmp(k, "=")) {
        if (op) {
            double v = apply(acc, atof(display), op);
            char a[32], b[32];
            fmt(acc, a);
            strcpy(b, display);
            snprintf(expr_line, sizeof expr_line, "%s %c %s =", a, op, b);
            fmt(v, display);
            op = 0;
            acc = v;
        }
        fresh = true;
    } else {
        double v = atof(display);
        acc = op && !fresh ? apply(acc, v, op) : (op ? acc : v);
        op = k[0];
        char a[32];
        fmt(acc, a);
        snprintf(expr_line, sizeof expr_line, "%s %c", a, op);
        fmt(acc, display);
        fresh = true;
    }
}

static void draw(window_t *w) {
    canvas_t *c = &w->c;
    gfx_fill(c, 0, 0, W, H, RGB(24, 22, 42));
    gfx_fill_round(c, 10, 10, W - 20, TOP - 20, 10, RGB(14, 12, 28));
    gfx_text(c, W - 24 - gfx_text_width(expr_line, FONT_SMALL), 22, expr_line, UI_DIM, TRANSPARENT, FONT_SMALL);
    int tw = gfx_text_width(display, FONT_LARGE);
    int font = tw > W - 50 ? FONT_SMALL : FONT_LARGE;
    tw = gfx_text_width(display, font);
    gfx_text(c, W - 24 - tw, font == FONT_LARGE ? 50 : 62, display, RGB(255, 255, 255), TRANSPARENT, font);
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 4; k++) {
            int x = 10 + k * (BW + GAP), y = TOP + r * (BH + GAP);
            const char *lab = keys[r][k];
            bool hot = r == hot_r && k == hot_c, dn = r == down_r && k == down_c;
            uint32_t base = k == 3 ? RGB(110, 84, 210) : r == 0 ? RGB(64, 60, 104) : RGB(44, 42, 76);
            if (!strcmp(lab, "=")) base = RGB(230, 150, 70);
            if (hot) base = gfx_mix(base, RGB(255, 255, 255), 40);
            if (dn) base = gfx_mix(base, RGB(0, 0, 0), 60);
            gfx_fill_round(c, x, y, BW, BH, 10, base);
            const char *shown = !strcmp(lab, "<") ? "DEL" : lab;
            ui_text_center(c, x, y + (BH - 16) / 2, BW, shown, RGB(250, 250, 255), FONT_SMALL);
        }
}

static bool key_at(int px, int py, int *r, int *k) {
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            if (ui_hit(px, py, 10 + j * (BW + GAP), TOP + i * (BH + GAP), BW, BH)) {
                *r = i;
                *k = j;
                return true;
            }
    return false;
}

int main(void) {
    window_t *w = win_open(W, H, "Calculator", 0);
    if (!w) return 1;
    draw(w);
    win_update(w);
    for (;;) {
        struct gui_event e;
        if (win_event(w, &e, -1) < 0 || e.type == EV_CLOSE) break;
        int r, k;
        switch (e.type) {
        case EV_MOUSE_MOVE:
            if (!key_at(e.x, e.y, &r, &k)) r = k = -1;
            if (r == hot_r && k == hot_c) continue;
            hot_r = r;
            hot_c = k;
            break;
        case EV_MOUSE_DOWN:
            if (key_at(e.x, e.y, &r, &k)) {
                down_r = r;
                down_c = k;
            }
            break;
        case EV_MOUSE_UP:
            if (key_at(e.x, e.y, &r, &k) && r == down_r && k == down_c) press(keys[r][k]);
            down_r = down_c = -1;
            break;
        case EV_KEY: {
            if (!e.pressed) continue;
            char s[2] = {(char)e.key, 0};
            if (e.key == NKEY_ENTER || e.key == '=') press("=");
            else if (e.key == NKEY_BACKSPACE) press("<");
            else if (e.key == NKEY_ESC || e.key == 'c' || e.key == 'C') press("C");
            else if (e.key < 128 && strchr("0123456789.+-*/%", (int)e.key)) press(s);
            else continue;
            break;
        }
        default: continue;
        }
        draw(w);
        win_update(w);
    }
    win_close(w);
    return 0;
}
