/* Procedural desktop art: night-sky wallpaper, app icons and the mouse cursor. */
#include "kernel.h"
#include "gui/wm.h"

static uint32_t seed = 0x5EED1234;
static uint32_t rnd(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static uint32_t sky_at(int y, int h) {
    /* piecewise gradient: deep navy -> indigo -> violet glow at the horizon */
    int t = y * 1000 / h;
    if (t < 550) return gfx_mix(RGB(6, 8, 26), RGB(24, 20, 64), t * 255 / 550);
    if (t < 850) return gfx_mix(RGB(24, 20, 64), RGB(84, 44, 108), (t - 550) * 255 / 300);
    return gfx_mix(RGB(84, 44, 108), RGB(140, 70, 120), (t - 850) * 255 / 150);
}

static void ridge(int *hgt, int w, int base, int amp, uint32_t s) {
    /* midpoint displacement over a power-of-two grid, then sampled */
    enum { N = 256 };
    int pts[N + 1];
    seed = s;
    pts[0] = base + (int)(rnd() % (amp + 1)) - amp / 2;
    pts[N] = base + (int)(rnd() % (amp + 1)) - amp / 2;
    for (int step = N; step > 1; step /= 2) {
        for (int i = 0; i < N; i += step) {
            int mid = i + step / 2;
            int disp = amp * step / N;
            pts[mid] = (pts[i] + pts[i + step]) / 2 + (disp ? (int)(rnd() % (2 * disp + 1)) - disp : 0);
        }
    }
    for (int x = 0; x < w; x++) {
        int fx = x * N / w;
        int frac = (x * N) % w;
        hgt[x] = pts[fx] + (pts[fx + 1] - pts[fx]) * frac / w;
    }
}

void desktop_draw_wallpaper(canvas_t *c) {
    int w = c->w, h = c->h;
    for (int y = 0; y < h; y++) gfx_hline(c, 0, y, w, sky_at(y, h));

    /* moon glow + crescent */
    int mx = w * 78 / 100, my = h * 22 / 100, mr = h / 14;
    int gr = mr * 4;
    for (int y = my - gr; y < my + gr; y++) {
        if (y < 0 || y >= h) continue;
        for (int x = mx - gr; x < mx + gr; x++) {
            if (x < 0 || x >= w) continue;
            int dx = x - mx, dy = y - my;
            uint32_t d = isqrt(dx * dx + dy * dy);
            if (d < (uint32_t)gr) {
                int t = (int)(gr - d) * 255 / gr;
                t = t * t / 255 * 70 / 255;
                uint32_t *p = &c->px[y * c->pitch + x];
                *p = gfx_mix(*p, RGB(200, 190, 255), t);
            }
        }
    }

    /* stars */
    seed = 0xC0DEF00D;
    int nstars = w * h / 2200;
    for (int i = 0; i < nstars; i++) {
        int x = rnd() % w, y = rnd() % (h * 80 / 100);
        int dx = x - mx, dy = y - my;
        if (dx * dx + dy * dy < (mr + 6) * (mr + 6)) continue;
        int b = 70 + rnd() % 186;
        uint32_t tint = rnd() % 3;
        uint32_t col = tint == 0 ? RGB(b, b, b) : tint == 1 ? RGB(b * 9 / 10, b * 9 / 10, b) : RGB(b, b * 95 / 100, b * 8 / 10);
        uint32_t *p = &c->px[y * c->pitch + x];
        *p = gfx_mix(*p, col, 230);
        if (b > 240 && rnd() % 3 == 0) {
            uint32_t dim = gfx_mix(sky_at(y, h), col, 110);
            gfx_pixel(c, x - 1, y, dim);
            gfx_pixel(c, x + 1, y, dim);
            gfx_pixel(c, x, y - 1, dim);
            gfx_pixel(c, x, y + 1, dim);
        }
    }

    /* the crescent itself */
    int ox = mr * 45 / 100, oy = -mr * 20 / 100;
    for (int y = my - mr - 1; y <= my + mr + 1; y++) {
        for (int x = mx - mr - 1; x <= mx + mr + 1; x++) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            int dx = x - mx, dy = y - my;
            int d2 = dx * dx + dy * dy;
            int ex = x - (mx + ox), ey = y - (my + oy);
            int e2 = ex * ex + ey * ey;
            int r2 = mr * mr;
            if (d2 <= r2 && e2 > r2 * 92 / 100) {
                /* soft edge on the inner side */
                int edge = e2 - r2 * 92 / 100;
                int a = edge * 255 / (mr * 6 + 1);
                if (a > 255) a = 255;
                int outer = r2 - d2;
                int a2 = outer * 255 / (mr * 2 + 1);
                if (a2 < a) a = a2;
                if (a > 255) a = 255;
                uint32_t *p = &c->px[y * c->pitch + x];
                *p = gfx_mix(*p, RGB(246, 236, 196), a);
            }
        }
    }

    /* mountains: two layers */
    int *hgt = __builtin_alloca(sizeof(int) * w);
    ridge(hgt, w, h * 76 / 100, h * 16 / 100, 0x1234567);
    for (int x = 0; x < w; x++)
        for (int y = hgt[x]; y < h; y++) {
            uint32_t *p = &c->px[y * c->pitch + x];
            *p = gfx_mix(RGB(38, 26, 70), RGB(22, 16, 46), (y - hgt[x]) * 255 / (h - hgt[x] + 1));
        }
    ridge(hgt, w, h * 87 / 100, h * 10 / 100, 0x7654321);
    for (int x = 0; x < w; x++)
        for (int y = hgt[x]; y < h; y++) c->px[y * c->pitch + x] = RGB(12, 10, 26);

    /* a few pine trees on the front ridge */
    seed = 0xBADA55;
    for (int i = 0; i < w / 40; i++) {
        int x = rnd() % w;
        int th = 10 + rnd() % 18;
        int base = hgt[x] + 2;
        gfx_triangle(c, x, base - th, x - th / 3, base, x + th / 3, base, RGB(10, 8, 20));
    }
}

/* ---- icons (32x32) ---- */

static void label(canvas_t *c, int x, int y, const char *s, uint32_t col) {
    gfx_text(c, x, y, s, col, TRANSPARENT, FONT_SMALL);
}

void desktop_draw_icon(canvas_t *c, int x, int y, int icon) {
    switch (icon) {
    case ICON_TERMINAL:
        gfx_fill_round(c, x + 1, y + 3, 30, 26, 4, RGB(150, 160, 200));
        gfx_fill_round(c, x + 2, y + 4, 28, 24, 3, RGB(18, 18, 32));
        gfx_fill(c, x + 2, y + 4, 28, 4, RGB(70, 70, 110));
        label(c, x + 5, y + 10, ">_", RGB(120, 240, 150));
        break;
    case ICON_FILES:
        gfx_fill_round(c, x + 2, y + 6, 12, 6, 2, RGB(230, 170, 60));
        gfx_fill_round(c, x + 2, y + 9, 28, 20, 3, RGB(245, 195, 80));
        gfx_hline(c, x + 3, y + 12, 26, RGB(255, 225, 140));
        break;
    case ICON_PAINT:
        gfx_fill_circle(c, x + 16, y + 16, 13, RGB(235, 215, 180));
        gfx_fill_circle(c, x + 21, y + 21, 4, RGB(20, 20, 40));
        gfx_fill_circle(c, x + 10, y + 11, 3, RGB(230, 60, 80));
        gfx_fill_circle(c, x + 17, y + 8, 3, RGB(70, 140, 240));
        gfx_fill_circle(c, x + 24, y + 12, 3, RGB(80, 200, 110));
        gfx_fill_circle(c, x + 9, y + 20, 3, RGB(250, 210, 60));
        break;
    case ICON_CLOCK:
        gfx_fill_circle(c, x + 16, y + 16, 14, RGB(110, 120, 190));
        gfx_fill_circle(c, x + 16, y + 16, 12, RGB(245, 245, 255));
        gfx_line(c, x + 16, y + 16, x + 16, y + 7, RGB(30, 30, 50));
        gfx_line(c, x + 16, y + 16, x + 22, y + 19, RGB(30, 30, 50));
        gfx_fill_circle(c, x + 16, y + 16, 1, RGB(220, 60, 80));
        break;
    case ICON_SNAKE:
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(24, 40, 30));
        for (int i = 0; i < 4; i++) gfx_fill(c, x + 4 + i * 6, y + 20, 5, 5, RGB(90, 220, 110));
        gfx_fill(c, x + 22, y + 14, 5, 5, RGB(90, 220, 110));
        gfx_fill(c, x + 22, y + 8, 5, 5, RGB(140, 255, 150));
        gfx_fill(c, x + 8, y + 7, 5, 5, RGB(240, 70, 80));
        break;
    case ICON_FRACTAL:
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(10, 8, 40));
        gfx_fill_circle(c, x + 18, y + 16, 8, RGB(30, 20, 90));
        gfx_fill_circle(c, x + 18, y + 16, 6, RGB(10, 5, 20));
        gfx_fill_circle(c, x + 9, y + 16, 4, RGB(10, 5, 20));
        gfx_circle(c, x + 18, y + 16, 9, RGB(240, 160, 60));
        gfx_circle(c, x + 9, y + 16, 5, RGB(240, 100, 160));
        break;
    case ICON_CUBE: {
        uint32_t col = RGB(120, 220, 255);
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(20, 24, 50));
        gfx_rect(c, x + 6, y + 11, 15, 15, col);
        gfx_rect(c, x + 12, y + 5, 15, 15, col);
        gfx_line(c, x + 6, y + 11, x + 12, y + 5, col);
        gfx_line(c, x + 20, y + 11, x + 26, y + 5, col);
        gfx_line(c, x + 6, y + 25, x + 12, y + 19, col);
        gfx_line(c, x + 20, y + 25, x + 26, y + 19, col);
        break;
    }
    case ICON_LIFE:
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(30, 30, 40));
        for (int j = 0; j < 5; j++)
            for (int i = 0; i < 5; i++)
                if ((i * 7 + j * 3 + i * j) % 3 == 0) gfx_fill(c, x + 4 + i * 5, y + 4 + j * 5, 4, 4, RGB(255, 200, 90));
        break;
    case ICON_MONITOR:
        gfx_fill_round(c, x + 1, y + 3, 30, 24, 4, RGB(40, 44, 70));
        gfx_fill(c, x + 6, y + 15, 4, 9, RGB(110, 220, 140));
        gfx_fill(c, x + 12, y + 9, 4, 15, RGB(110, 180, 250));
        gfx_fill(c, x + 18, y + 12, 4, 12, RGB(240, 180, 90));
        gfx_fill(c, x + 24, y + 7, 4, 17, RGB(230, 110, 140));
        gfx_fill(c, x + 12, y + 28, 8, 3, RGB(150, 160, 200));
        break;
    case ICON_EDITOR:
        gfx_fill(c, x + 6, y + 2, 20, 28, RGB(245, 245, 250));
        gfx_rect(c, x + 6, y + 2, 20, 28, RGB(150, 160, 190));
        for (int i = 0; i < 5; i++) gfx_hline(c, x + 9, y + 8 + i * 4, 14 - (i % 2) * 4, RGB(120, 130, 170));
        gfx_line(c, x + 28, y + 6, x + 18, y + 24, RGB(240, 150, 60));
        gfx_line(c, x + 29, y + 7, x + 19, y + 25, RGB(240, 150, 60));
        break;
    case ICON_ABOUT:
        gfx_fill_circle(c, x + 16, y + 16, 13, RGB(246, 236, 196));
        gfx_fill_circle(c, x + 22, y + 12, 12, RGB(40, 30, 80));
        gfx_pixel(c, x + 27, y + 24, RGB(255, 255, 255));
        gfx_pixel(c, x + 24, y + 28, RGB(255, 255, 255));
        break;
    case ICON_CALC:
        gfx_fill_round(c, x + 4, y + 1, 24, 30, 4, RGB(60, 64, 90));
        gfx_fill(c, x + 7, y + 4, 18, 7, RGB(170, 220, 180));
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) gfx_fill(c, x + 7 + i * 6, y + 14 + j * 5, 5, 4, i == 2 && j == 2 ? RGB(240, 150, 60) : RGB(200, 200, 220));
        break;
    case ICON_POWER:
        gfx_circle(c, x + 16, y + 17, 11, RGB(240, 90, 100));
        gfx_circle(c, x + 16, y + 17, 10, RGB(240, 90, 100));
        gfx_fill(c, x + 15, y + 3, 3, 13, RGB(240, 90, 100));
        break;
    case ICON_REBOOT:
        gfx_circle(c, x + 16, y + 16, 11, RGB(120, 200, 250));
        gfx_circle(c, x + 16, y + 16, 10, RGB(120, 200, 250));
        gfx_triangle(c, x + 22, y + 2, x + 22, y + 12, x + 30, y + 7, RGB(120, 200, 250));
        break;
    case ICON_PIANO:
        gfx_fill_round(c, x + 1, y + 4, 30, 24, 4, RGB(40, 34, 70));
        for (int i = 0; i < 5; i++) gfx_fill(c, x + 4 + i * 5, y + 7, 4, 19, RGB(246, 244, 236));
        for (int i = 0; i < 4; i++)
            if (i != 2) gfx_fill(c, x + 7 + i * 5, y + 7, 3, 11, RGB(24, 22, 36));
        gfx_fill(c, x + 4, y + 7, 24, 2, RGB(130, 110, 240));
        break;
    case ICON_TETRIS:
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(24, 24, 44));
        gfx_fill(c, x + 5, y + 19, 7, 7, RGB(240, 90, 110));
        gfx_fill(c, x + 12, y + 19, 7, 7, RGB(240, 90, 110));
        gfx_fill(c, x + 12, y + 12, 7, 7, RGB(240, 90, 110));
        gfx_fill(c, x + 19, y + 19, 7, 7, RGB(90, 180, 250));
        gfx_fill(c, x + 19, y + 12, 7, 7, RGB(90, 180, 250));
        gfx_fill(c, x + 19, y + 5, 7, 7, RGB(90, 180, 250));
        break;
    case ICON_MINES:
        gfx_fill_round(c, x + 1, y + 1, 30, 30, 5, RGB(180, 185, 200));
        gfx_line(c, x + 7, y + 7, x + 25, y + 25, RGB(20, 20, 30));
        gfx_line(c, x + 25, y + 7, x + 7, y + 25, RGB(20, 20, 30));
        gfx_hline(c, x + 5, y + 16, 22, RGB(20, 20, 30));
        gfx_vline(c, x + 16, y + 5, 22, RGB(20, 20, 30));
        gfx_fill_circle(c, x + 16, y + 16, 7, RGB(20, 20, 30));
        gfx_fill(c, x + 13, y + 13, 3, 3, RGB(255, 255, 255));
        break;
    case ICON_NET:
        gfx_fill_circle(c, x + 16, y + 16, 13, RGB(70, 140, 230));
        gfx_circle(c, x + 16, y + 16, 13, RGB(200, 230, 255));
        gfx_vline(c, x + 16, y + 3, 26, RGB(200, 230, 255));
        gfx_hline(c, x + 3, y + 16, 26, RGB(200, 230, 255));
        gfx_circle(c, x + 16, y + 16, 6, RGB(200, 230, 255));
        break;
    default:
        gfx_fill_round(c, x + 4, y + 4, 24, 24, 6, RGB(150, 120, 230));
        break;
    }
}

/* ---- cursor ---- */
static const char *arrow[] = {
    "X           ", "XX          ", "XoX         ", "XooX        ", "XoooX       ", "XooooX      ",
    "XoooooX     ", "XooooooX    ", "XoooooooX   ", "XooooooooX  ", "XoooooooooX ", "XooooooXXXXX",
    "XoooXooX    ", "XooXXooX    ", "XoX  XooX   ", "XX   XooX   ", "X     XooX  ", "      XooX  ",
    "       XX   ",
};
static const char *resize[] = {
    "XXXXXX      ", "XooooX      ", "XoooX       ", "XooooX      ", "XoXooX      ", "XX XooX     ",
    "    XooX XX ", "     XooXoX ", "      XooooX", "       XoooX", "      XooooX", "      XXXXXX",
};

void desktop_draw_cursor(canvas_t *c, int x, int y, int shape) {
    const char **img = shape == CURSOR_RESIZE ? resize : arrow;
    int rows = shape == CURSOR_RESIZE ? (int)ARRAY_SIZE(resize) : (int)ARRAY_SIZE(arrow);
    if (shape == CURSOR_RESIZE) { x -= 6; y -= 6; }
    for (int j = 0; j < rows; j++)
        for (int i = 0; img[j][i]; i++) {
            if (img[j][i] == 'X') gfx_pixel(c, x + i, y + j, RGB(10, 10, 20));
            else if (img[j][i] == 'o') gfx_pixel(c, x + i, y + j, RGB(255, 255, 255));
        }
}
