/* Mandelbrot explorer: left click zooms in, right click zooms out, R resets */
#include <stdio.h>
#include <math.h>
#include "nocturne.h"

static window_t *w;
static double cx = -0.6, cy = 0.0, scale = 3.2; /* width of view in complex units */
static int maxit = 200, row;
static uint32_t pal[256];
static uint64_t t_start;
static char info[96];

static void make_palette(void) {
    for (int i = 0; i < 256; i++) {
        double t = i / 255.0;
        int r = (int)(9 * (1 - t) * t * t * t * 255);
        int g = (int)(15 * (1 - t) * (1 - t) * t * t * 255);
        int b = (int)(8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255);
        r = MIN(255, r + 20);
        g = MIN(255, g + 10);
        b = MIN(255, b + 40);
        pal[i] = RGB(r, g, b);
    }
}

static void render_rows(int n) {
    int W = w->w, H = w->h;
    double px = scale / W;
    for (int k = 0; k < n && row < H; k++, row++) {
        double ci = cy + (row - H / 2) * px;
        uint32_t *out = w->c.px + row * w->c.pitch;
        for (int x = 0; x < W; x++) {
            double cr = cx + (x - W / 2) * px;
            double zr = 0, zi = 0, zr2 = 0, zi2 = 0;
            int i = 0;
            /* main cardioid / period-2 bulb test */
            double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
            if (q * (q + (cr - 0.25)) < 0.25 * ci * ci || (cr + 1) * (cr + 1) + ci * ci < 0.0625) i = maxit;
            while (i < maxit && zr2 + zi2 < 256) {
                zi = 2 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                i++;
            }
            if (i >= maxit) out[x] = RGB(4, 2, 12);
            else {
                double mu = i + 1 - log(log(sqrt(zr2 + zi2))) / log(2.0);
                int idx = (int)(mu * 6) & 511;
                if (idx > 255) idx = 511 - idx;
                out[x] = pal[idx];
            }
        }
    }
}

static void restart(void) {
    row = 0;
    t_start = uptime_ms();
    maxit = 200 + (int)(60 * log2(3.2 / scale + 1));
}

int main(void) {
    w = win_open(640, 480, "Mandelbrot", WIN_RESIZABLE);
    if (!w) return 1;
    make_palette();
    restart();
    for (;;) {
        bool busy = row < w->h;
        struct gui_event e;
        int r = win_event(w, &e, busy ? 0 : -1);
        if (r < 0) break;
        if (r > 0) {
            if (e.type == EV_CLOSE) break;
            if (e.type == EV_MOUSE_DOWN) {
                double mx = cx + (e.x - w->w / 2) * scale / w->w, my = cy + (e.y - w->h / 2) * scale / w->w;
                cx = mx;
                cy = my;
                scale *= (e.buttons & 2) ? 2.5 : 0.4;
                restart();
            } else if (e.type == EV_WHEEL) {
                scale *= e.wheel > 0 ? 1.25 : 0.8;
                restart();
            } else if (e.type == EV_KEY && e.pressed) {
                if (e.key == 'r' || e.key == 'R') {
                    cx = -0.6;
                    cy = 0;
                    scale = 3.2;
                } else if (e.key == NKEY_ESC) break;
                else if (e.key == NKEY_LEFT) cx -= scale * 0.1;
                else if (e.key == NKEY_RIGHT) cx += scale * 0.1;
                else if (e.key == NKEY_UP) cy -= scale * 0.1;
                else if (e.key == NKEY_DOWN) cy += scale * 0.1;
                else if (e.key == '+' || e.key == '=') scale *= 0.7;
                else if (e.key == '-') scale /= 0.7;
                else continue;
                restart();
            } else if (e.type == EV_RESIZE) {
                restart();
            }
            continue;
        }
        if (busy) {
            int y0 = row;
            render_rows(24);
            if (row >= w->h) {
                snprintf(info, sizeof info, " zoom %.3gx  %d iterations  %lu ms ", 3.2 / scale, maxit,
                         (unsigned long)(uptime_ms() - t_start));
                int tw = gfx_text_width(info, FONT_SMALL);
                gfx_fill_blend(&w->c, 6, w->h - 26, tw + 4, 20, ARGB(150, 0, 0, 0));
                gfx_text(&w->c, 8, w->h - 24, info, RGB(230, 230, 250), TRANSPARENT, FONT_SMALL);
                win_update(w);
            } else {
                win_update_rect(w, 0, y0, w->w, row - y0);
            }
        }
    }
    win_close(w);
    return 0;
}
