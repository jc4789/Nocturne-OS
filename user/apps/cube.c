/* Spinning shaded solids. Space (or the button) switches shape, mouse drag spins. The
   header and the hint are widgets of the interface engine (libc/ui.c); the solid is
   drawn in a surface that fills the resizable window. Escape closes.
   A picture is drawn each time the screen took the last one (UI_EVENT_FRAME), so once a
   frame of the display at any refresh rate, and the solid turns by the time that passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nocturne.h"
#include "ui.h"

#define WINDOW 1
#define VIEW 1
#define PAD 12
#define GAP 8
#define HEADER 36 /* the height of the header row */
#define HINT 18   /* the height of the hint below the view */

static uint32_t next_shape; /* the button's place in the view */

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

/* The header (the shape's name, the frame rate and the button for the next shape) and the
   hint are widgets; the solid is drawn in the surface between them. */
static void show(struct ui_session *s, int fps) {
    char rate[24];
    snprintf(rate, sizeof rate, "%d fps", fps);
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", GAP);
        ui_node(&m, "row"); ui_num(&m, "gap", 10);
            ui_node(&m, "text"); ui_str(&m, "text", shape_name[shape]); ui_str(&m, "style", "heading"); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", rate); ui_str(&m, "style", "caption"); ui_end(&m);
            ui_node(&m, "spacer"); ui_end(&m);
            next_shape = ui_node(&m, "button");
            ui_str(&m, "text", "Next shape"); ui_str(&m, "icon", "chevron-right"); ui_str(&m, "variant", "secondary");
            ui_num(&m, "compact", 1); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", VIEW); ui_num(&m, "pointer", 1);
        ui_str(&m, "width", "fill"); ui_str(&m, "height", "fill"); ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", "Space: next shape    Drag: spin    Esc: close");
        ui_str(&m, "style", "caption"); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

/* The solid turned by ax and ay, drawn into c (W x H). */
static void paint(canvas_t *c, int W, int H, double ax, double ay) {
    const struct ui_theme *th = &ui_theme;
    gfx_fill(c, 0, 0, W, H, th->background);
    ui_round_gradient(c, 0, 0, W, H, th->radius, th->sidebar, th->surfaceActive);
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
    for (int r = 0; r < 6; r++) {
        float sw = 2 * (float)sc - r * 12, sh = 14 - r * 2;
        ui_round_fill(c, (float)W / 2 - sw / 2, (float)H - 30 + r, sw, sh, sh / 2, ARGB(26, 0, 0, 0));
    }
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
            ui_line(c, (float)px[p0], (float)py[p0], (float)px[p1], (float)py[p1], 1.2f, edge);
        }
    }
}

int main(void) {
    struct ui_session *s = ui_session_new();
    int ww, wh;
    if (!s || ui_window(s, WINDOW, 480, 440, WIN_RESIZABLE, "3D Shapes", &ww, &wh) < 0) return 1;
    /* a first guess; the engine reports the real size with UI_EVENT_CANVAS */
    int W = MAX(1, ww - 2 * PAD), H = MAX(1, wh - 2 * PAD - 2 * GAP - HEADER - HINT);
    canvas_t *c = ui_surface(s, VIEW, W, H, 0);
    if (!c) return 1;
    build();
    /* the spin in radians a millisecond; a drag turns by the pointer's moves and leaves the
       last one spinning, a move taken as one 16 ms step */
    double ax = 0.5, ay = 0.3, vx = 0.011 / 16, vy = 0.017 / 16;
    bool drag = false, draw = true, quit = false;
    int lmx = 0, lmy = 0;
    uint64_t frames = 0, t0 = n_clock_uptime_ms(), drawn = t0;
    int fps = 0;
    show(s, fps);
    while (!quit) {
        if (draw) {
            draw = false;
            uint64_t now = n_clock_uptime_ms();
            if (!drag) {
                ax += vx * (double)(now - drawn);
                ay += vy * (double)(now - drawn);
            }
            drawn = now;
            paint(c, W, H, ax, ay);
            ui_surface_changed(s, VIEW);
            frames++;
            if (now - t0 >= 1000) {
                bool same = (int)frames == fps;
                fps = (int)frames;
                frames = 0;
                t0 = now;
                if (!same) show(s, fps); /* the frame rate is a widget; send it only when it changed */
            }
        }
        ui_session_wait(s, -1);
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            bool next = false;
            switch (e.type) {
            case UI_EVENT_FRAME:
                draw = true;
                break;
            case UI_EVENT_CLOSE:
                quit = true;
                break;
            case UI_EVENT_KEY:
                if (e.flags & UI_UP) break;
                if (!strcmp(e.text, "Escape")) quit = true;
                else if (e.a == ' ') next = true;
                break;
            case UI_EVENT_CLICK:
                if (e.node == next_shape) next = true;
                break;
            case UI_EVENT_CANVAS:
                W = MAX(1, (int)e.a);
                H = MAX(1, (int)e.b);
                c = ui_surface(s, VIEW, W, H, 0);
                if (!c) quit = true;
                draw = true; /* the new surface is shown once drawn */
                break;
            case UI_EVENT_POINTER:
                if (e.flags & UI_DOWN) {
                    drag = true;
                    lmx = (int)e.a;
                    lmy = (int)e.b;
                } else if (e.flags & UI_UP) {
                    drag = false;
                } else if (drag) {
                    ay += ((int)e.a - lmx) * 0.01;
                    ax += ((int)e.b - lmy) * -0.01;
                    vy = ((int)e.a - lmx) * 0.01 / 16;
                    vx = ((int)e.b - lmy) * -0.01 / 16;
                    lmx = (int)e.a;
                    lmy = (int)e.b;
                }
                break;
            }
            if (next) {
                shape = (shape + 1) % 4;
                build();
                show(s, fps); /* the name of the shape */
            }
        }
        if (!ui_session_open(s)) quit = true;
    }
    ui_session_free(s);
    return 0;
}
