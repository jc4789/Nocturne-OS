/* Spinning shaded solids. Space switches shape, mouse drag spins. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "nocturne.h"

typedef struct {
    double x, y, z;
} vec3;

static vec3 verts[64];
static int faces[64][4], nface, nvert, face_n[64]; /* face_n: vertex count (3 or 4) */
static uint32_t face_col[64];
static int shape = 0;
static const char *shape_name[] = {"Cube", "Octahedron", "Icosahedron", "Pyramid"};

static void add_face(int a, int b, int c, int d, uint32_t col) {
    faces[nface][0] = a;
    faces[nface][1] = b;
    faces[nface][2] = c;
    faces[nface][3] = d;
    face_n[nface] = d < 0 ? 3 : 4;
    face_col[nface] = col;
    nface++;
}

static uint32_t hue(int i, int n) {
    double h = (double)i / n * 6;
    int k = (int)h;
    double f = h - k;
    int q = (int)(255 * (1 - f)), t = (int)(255 * f);
    int r, g, b;
    switch (k % 6) {
    case 0: r = 255, g = t, b = 0; break;
    case 1: r = q, g = 255, b = 0; break;
    case 2: r = 0, g = 255, b = t; break;
    case 3: r = 0, g = q, b = 255; break;
    case 4: r = t, g = 0, b = 255; break;
    default: r = 255, g = 0, b = q; break;
    }
    return RGB((r + 2 * 180) / 3, (g + 2 * 160) / 3, (b + 2 * 240) / 3);
}

static void build(void) {
    nface = nvert = 0;
    if (shape == 0) {
        for (int i = 0; i < 8; i++) verts[nvert++] = (vec3){i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1};
        int f[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (int i = 0; i < 6; i++) add_face(f[i][0], f[i][1], f[i][2], f[i][3], hue(i, 6));
    } else if (shape == 1) {
        vec3 v[6] = {{1.4, 0, 0}, {-1.4, 0, 0}, {0, 1.4, 0}, {0, -1.4, 0}, {0, 0, 1.4}, {0, 0, -1.4}};
        for (int i = 0; i < 6; i++) verts[nvert++] = v[i];
        int f[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
        for (int i = 0; i < 8; i++) add_face(f[i][0], f[i][1], f[i][2], -1, hue(i, 8));
    } else if (shape == 2) {
        double p = (1 + sqrt(5)) / 2, s = 0.85;
        vec3 v[12] = {{-1, p, 0}, {1, p, 0}, {-1, -p, 0}, {1, -p, 0}, {0, -1, p}, {0, 1, p},
                      {0, -1, -p}, {0, 1, -p}, {p, 0, -1}, {p, 0, 1}, {-p, 0, -1}, {-p, 0, 1}};
        for (int i = 0; i < 12; i++) verts[nvert++] = (vec3){v[i].x * s, v[i].y * s, v[i].z * s};
        int f[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                        {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                        {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        for (int i = 0; i < 20; i++) add_face(f[i][0], f[i][1], f[i][2], -1, hue(i, 20));
    } else {
        vec3 v[5] = {{-1.2, -1, -1.2}, {1.2, -1, -1.2}, {1.2, -1, 1.2}, {-1.2, -1, 1.2}, {0, 1.4, 0}};
        for (int i = 0; i < 5; i++) verts[nvert++] = v[i];
        add_face(0, 1, 2, 3, hue(0, 5));
        add_face(1, 0, 4, -1, hue(1, 5));
        add_face(2, 1, 4, -1, hue(2, 5));
        add_face(3, 2, 4, -1, hue(3, 5));
        add_face(0, 3, 4, -1, hue(4, 5));
    }
}

static vec3 rot(vec3 v, double ax, double ay) {
    double c = cos(ay), s = sin(ay);
    vec3 r = {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
    c = cos(ax);
    s = sin(ax);
    return (vec3){r.x, r.y * c - r.z * s, r.y * s + r.z * c};
}

struct drawface {
    double depth;
    int idx;
};
static int cmpf(const void *a, const void *b) {
    double d = ((const struct drawface *)a)->depth - ((const struct drawface *)b)->depth;
    return d < 0 ? 1 : d > 0 ? -1 : 0;
}

int main(void) {
    window_t *w = win_open(480, 420, "3D Shapes", WIN_RESIZABLE);
    if (!w) return 1;
    build();
    double ax = 0.5, ay = 0.3, vx = 0.011, vy = 0.017;
    bool drag = false;
    int lmx = 0, lmy = 0;
    uint64_t frames = 0, t0 = uptime_ms();
    int fps = 0;
    for (;;) {
        uint64_t frame_start = uptime_ms();
        canvas_t *c = &w->c;
        int W = w->w, H = w->h;
        gfx_gradient_v(c, 0, 0, W, H, RGB(16, 14, 34), RGB(44, 30, 70));
        double sc = MIN(W, H) * 0.27;
        vec3 tv[64];
        int px[64], py[64];
        for (int i = 0; i < nvert; i++) {
            tv[i] = rot(verts[i], ax, ay);
            double z = tv[i].z + 5;
            px[i] = W / 2 + (int)(tv[i].x * sc * 4 / z);
            py[i] = H / 2 - (int)(tv[i].y * sc * 4 / z);
        }
        /* shadow */
        for (int r = 0; r < 12; r++)
            gfx_fill_blend(c, W / 2 - (int)sc + r * 4, H - 50 + r, 2 * (int)sc - r * 8, 6, ARGB(14, 0, 0, 0));
        struct drawface df[64];
        for (int f = 0; f < nface; f++) {
            double z = 0;
            for (int k = 0; k < face_n[f]; k++) z += tv[faces[f][k]].z;
            df[f].depth = z / face_n[f];
            df[f].idx = f;
        }
        qsort(df, nface, sizeof df[0], cmpf);
        vec3 light = {-0.4, 0.6, -0.7};
        double ll = sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
        for (int i = 0; i < nface; i++) {
            int f = df[i].idx;
            vec3 a = tv[faces[f][0]], b = tv[faces[f][1]], cc = tv[faces[f][2]];
            vec3 u = {b.x - a.x, b.y - a.y, b.z - a.z}, v = {cc.x - a.x, cc.y - a.y, cc.z - a.z};
            vec3 n = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
            double nl = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (nl == 0) continue;
            int i0 = faces[f][0], i1 = faces[f][1], i2 = faces[f][2];
            double d = fabs(n.x * light.x + n.y * light.y + n.z * light.z) / (nl * ll);
            int shade = (int)(70 + 185 * d);
            uint32_t col = gfx_mix(RGB(10, 8, 30), face_col[f], shade);
            gfx_triangle(c, px[i0], py[i0], px[i1], py[i1], px[i2], py[i2], col);
            if (face_n[f] == 4) {
                int i3 = faces[f][3];
                gfx_triangle(c, px[i0], py[i0], px[i2], py[i2], px[i3], py[i3], col);
            }
            uint32_t edge = gfx_mix(col, RGB(255, 255, 255), 90);
            for (int k = 0; k < face_n[f]; k++) {
                int p0 = faces[f][k], p1 = faces[f][(k + 1) % face_n[f]];
                gfx_line(c, px[p0], py[p0], px[p1], py[p1], edge);
            }
        }
        char b[64];
        snprintf(b, sizeof b, "%s  %d fps", shape_name[shape], fps);
        gfx_text(c, 10, 8, b, UI_DIM, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 10, H - 24, "space: next shape   drag: spin", RGB(110, 100, 150), TRANSPARENT, FONT_SMALL);
        win_update(w);
        frames++;
        if (uptime_ms() - t0 >= 1000) {
            fps = (int)frames;
            frames = 0;
            t0 = uptime_ms();
        }
        if (!drag) {
            ax += vx;
            ay += vy;
        }
        int spent = (int)(uptime_ms() - frame_start);
        int wait = MAX(1, 16 - spent);
        uint64_t until = uptime_ms() + wait;
        bool quit = false;
        for (;;) {
            int64_t left = (int64_t)(until - uptime_ms());
            struct gui_event e;
            int r = win_event(w, &e, left > 0 ? (int)left : 0);
            if (r < 0) {
                quit = true;
                break;
            }
            if (r == 0) break;
            if (e.type == EV_CLOSE || (e.type == EV_KEY && e.pressed && e.key == NKEY_ESC)) {
                quit = true;
                break;
            }
            if (e.type == EV_KEY && e.pressed && e.key == ' ') {
                shape = (shape + 1) % 4;
                build();
            } else if (e.type == EV_MOUSE_DOWN) {
                drag = true;
                lmx = e.x;
                lmy = e.y;
            } else if (e.type == EV_MOUSE_UP) {
                drag = false;
            } else if (e.type == EV_MOUSE_MOVE && drag) {
                vy = (e.x - lmx) * 0.01;
                vx = (e.y - lmy) * -0.01;
                ay += vy;
                ax += vx;
                lmx = e.x;
                lmy = e.y;
            }
        }
        if (quit) break;
    }
    win_close(w);
    return 0;
}
