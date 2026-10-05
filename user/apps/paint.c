/* Paint */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "nocturne.h"

#define W 900
#define H 640
#define TOOLW 76
#define PALH 56
#define CX TOOLW
#define CY 0
#define CWID (W - TOOLW)
#define CHGT (H - PALH)

enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_FRECT, T_ELLIPSE, T_FILL, T_PICK, T_SPRAY, NTOOL };
static const char *tool_name[NTOOL] = {"Pencil", "Brush", "Eraser", "Line", "Rect", "Filled", "Ellipse", "Fill",
                                       "Picker", "Spray"};

static const uint32_t palette[] = {
    RGB(0, 0, 0),       RGB(64, 64, 64),    RGB(128, 128, 128), RGB(192, 192, 192), RGB(255, 255, 255),
    RGB(128, 0, 0),     RGB(230, 40, 50),   RGB(255, 140, 0),   RGB(255, 220, 0),   RGB(140, 220, 40),
    RGB(20, 140, 60),   RGB(0, 200, 200),   RGB(30, 110, 230),  RGB(20, 30, 140),   RGB(130, 60, 220),
    RGB(230, 90, 200),  RGB(140, 90, 50),   RGB(250, 200, 160), RGB(30, 28, 60),    RGB(246, 236, 196),
};
#define NPAL (int)(sizeof palette / sizeof palette[0])

static window_t *win;
static canvas_t img, snap_c;
static uint32_t *pix, *snap, *undo;
static int tool = T_BRUSH, size = 4;
static uint32_t fg = RGB(30, 110, 230), bg = RGB(255, 255, 255);
static bool drawing;
static int sx0, sy0, lx, ly;
static uint32_t dcol;
static char status[160];

static void push_undo(void) { memcpy(undo, pix, CWID * CHGT * 4); }

static void stamp(int x, int y, int r, uint32_t col) {
    if (r <= 1) gfx_pixel(&img, x, y, col);
    else gfx_fill_circle(&img, x, y, r / 2, col);
}

static void stroke(int x0, int y0, int x1, int y1, int r, uint32_t col) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = MAX(dx, dy);
    if (n == 0) {
        stamp(x0, y0, r, col);
        return;
    }
    for (int i = 0; i <= n; i++) stamp(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r, col);
}

static void ellipse(int x0, int y0, int x1, int y1, uint32_t col, int r) {
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double a = abs(x1 - x0) / 2.0, b = abs(y1 - y0) / 2.0;
    if (a < 1 || b < 1) {
        stroke(x0, y0, x1, y1, r, col);
        return;
    }
    int steps = (int)(4 * (a + b)) + 16;
    int px = cx + (int)a, py = cy;
    for (int i = 1; i <= steps; i++) {
        double t = i * 2 * M_PI / steps;
        int nx = cx + (int)round(a * cos(t)), ny = cy + (int)round(b * sin(t));
        stroke(px, py, nx, ny, r, col);
        px = nx;
        py = ny;
    }
}

static void flood(int x, int y, uint32_t col) {
    if (x < 0 || y < 0 || x >= CWID || y >= CHGT) return;
    uint32_t target = pix[y * CWID + x];
    if (target == col) return;
    int cap = 65536, n = 0;
    int *st = malloc(cap * sizeof(int) * 2);
    st[n++] = x;
    st[n++] = y;
    while (n) {
        int py = st[--n], px = st[--n];
        int l = px, r = px;
        uint32_t *row = pix + py * CWID;
        if (row[px] != target) continue;
        while (l > 0 && row[l - 1] == target) l--;
        while (r < CWID - 1 && row[r + 1] == target) r++;
        for (int i = l; i <= r; i++) row[i] = col;
        for (int d = -1; d <= 1; d += 2) {
            int ny = py + d;
            if (ny < 0 || ny >= CHGT) continue;
            uint32_t *nr = pix + ny * CWID;
            for (int i = l; i <= r; i++)
                if (nr[i] == target && (i == l || nr[i - 1] != target)) {
                    if (n + 2 > cap) {
                        cap *= 2;
                        st = realloc(st, cap * sizeof(int) * 2);
                    }
                    st[n++] = i;
                    st[n++] = ny;
                }
        }
    }
    free(st);
}

static void shape(int x0, int y0, int x1, int y1) {
    switch (tool) {
    case T_LINE: stroke(x0, y0, x1, y1, size, dcol); break;
    case T_RECT:
        stroke(x0, y0, x1, y0, size, dcol);
        stroke(x1, y0, x1, y1, size, dcol);
        stroke(x1, y1, x0, y1, size, dcol);
        stroke(x0, y1, x0, y0, size, dcol);
        break;
    case T_FRECT: gfx_fill(&img, MIN(x0, x1), MIN(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1, dcol); break;
    case T_ELLIPSE: ellipse(x0, y0, x1, y1, dcol, size); break;
    }
}

static void tool_icon(canvas_t *c, int x, int y, int t) {
    uint32_t k = RGB(230, 230, 245);
    switch (t) {
    case T_PENCIL:
        gfx_line(c, x + 4, y + 18, x + 18, y + 4, k);
        gfx_line(c, x + 5, y + 18, x + 19, y + 4, k);
        gfx_fill(c, x + 3, y + 18, 3, 3, RGB(250, 200, 110));
        break;
    case T_BRUSH:
        gfx_line(c, x + 18, y + 3, x + 9, y + 12, RGB(170, 120, 70));
        gfx_line(c, x + 19, y + 4, x + 10, y + 13, RGB(170, 120, 70));
        gfx_fill_circle(c, x + 7, y + 16, 4, RGB(120, 160, 250));
        break;
    case T_ERASER:
        gfx_fill_round(c, x + 4, y + 7, 16, 10, 2, RGB(250, 150, 170));
        gfx_fill(c, x + 4, y + 13, 16, 4, RGB(230, 230, 240));
        break;
    case T_LINE: gfx_line(c, x + 3, y + 19, x + 20, y + 3, k); break;
    case T_RECT: gfx_rect(c, x + 3, y + 5, 18, 14, k); break;
    case T_FRECT: gfx_fill(c, x + 3, y + 5, 18, 14, k); break;
    case T_ELLIPSE: gfx_circle(c, x + 12, y + 12, 8, k); break;
    case T_FILL:
        gfx_triangle(c, x + 4, y + 10, x + 12, y + 3, x + 18, y + 12, k);
        gfx_fill_circle(c, x + 19, y + 18, 3, RGB(120, 160, 250));
        break;
    case T_PICK:
        gfx_line(c, x + 5, y + 19, x + 15, y + 9, k);
        gfx_fill_circle(c, x + 17, y + 7, 4, k);
        break;
    case T_SPRAY:
        gfx_fill_round(c, x + 6, y + 9, 9, 12, 2, k);
        for (int i = 0; i < 6; i++) gfx_pixel(c, x + 16 + (i * 7) % 6, y + 2 + (i * 5) % 7, k);
        break;
    }
}

static void draw(void) {
    canvas_t *c = &win->c;
    gfx_fill(c, 0, 0, TOOLW, H, RGB(34, 30, 60));
    for (int i = 0; i < NTOOL; i++) {
        int x = 8 + (i % 2) * 32, y = 8 + (i / 2) * 32;
        gfx_fill_round(c, x, y, 28, 28, 5, i == tool ? UI_ACCENT : RGB(52, 48, 90));
        tool_icon(c, x + 2, y + 2, i);
    }
    int y = 8 + ((NTOOL + 1) / 2) * 32 + 8;
    gfx_text(c, 8, y, "Size", UI_DIM, TRANSPARENT, FONT_SMALL);
    y += 20;
    static const int sizes[] = {1, 4, 8, 16};
    for (int i = 0; i < 4; i++) {
        int by = y + i * 30;
        gfx_fill_round(c, 8, by, 60, 26, 5, sizes[i] == size ? UI_ACCENT : RGB(52, 48, 90));
        gfx_fill_circle(c, 38, by + 13, MAX(1, sizes[i] / 2), RGB(230, 230, 245));
    }
    y += 4 * 30 + 10;
    ui_button(c, 8, y, 60, 26, "Undo", false, false);
    ui_button(c, 8, y + 32, 60, 26, "Clear", false, false);
    ui_button(c, 8, y + 64, 60, 26, "Save", false, false);
    /* canvas */
    gfx_blit(c, CX, CY, &img, 0, 0, CWID, CHGT);
    /* palette */
    gfx_fill(c, 0, H - PALH, W, PALH, RGB(34, 30, 60));
    gfx_fill(c, 18, H - PALH + 12, 30, 30, bg);
    gfx_rect(c, 18, H - PALH + 12, 30, 30, RGB(0, 0, 0));
    gfx_fill(c, 8, H - PALH + 6, 28, 28, fg);
    gfx_rect(c, 8, H - PALH + 6, 28, 28, RGB(255, 255, 255));
    for (int i = 0; i < NPAL; i++) {
        int px = CX + 10 + (i % 10) * 26, py = H - PALH + 4 + (i / 10) * 25;
        gfx_fill_round(c, px, py, 23, 22, 4, palette[i]);
    }
    gfx_text(c, CX + 290, H - PALH + 10, status, UI_DIM, TRANSPARENT, FONT_SMALL);
    char hint[96];
    snprintf(hint, sizeof hint, "%s, size %d   right click: background", tool_name[tool], size);
    gfx_text(c, CX + 290, H - PALH + 30, hint, RGB(110, 108, 150), TRANSPARENT, FONT_SMALL);
}

static void save(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot write %s", path);
        return;
    }
    char hdr[64];
    int n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", CWID, CHGT);
    write(fd, hdr, n);
    uint8_t *row = malloc(CWID * 3);
    for (int y = 0; y < CHGT; y++) {
        for (int x = 0; x < CWID; x++) {
            uint32_t p = pix[y * CWID + x];
            row[x * 3] = p >> 16;
            row[x * 3 + 1] = p >> 8;
            row[x * 3 + 2] = p;
        }
        write(fd, row, CWID * 3);
    }
    free(row);
    close(fd);
    snprintf(status, sizeof status, "saved %s", path);
}

static int read_num(const uint8_t *d, size_t n, size_t *i) {
    while (*i < n && (isspace(d[*i]) || d[*i] == '#')) {
        if (d[*i] == '#')
            while (*i < n && d[*i] != '\n') (*i)++;
        else (*i)++;
    }
    int v = 0;
    while (*i < n && isdigit(d[*i])) v = v * 10 + (d[(*i)++] - '0');
    return v;
}

static void load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot open %s", path);
        return;
    }
    struct n_stat st;
    fstat(fd, &st);
    uint8_t *d = malloc(st.size + 1);
    size_t got = 0;
    long r;
    while (got < st.size && (r = read(fd, d + got, st.size - got)) > 0) got += r;
    close(fd);
    size_t i = 2;
    if (got < 2 || d[0] != 'P' || d[1] != '6') {
        snprintf(status, sizeof status, "%s is not a P6 picture", path);
        free(d);
        return;
    }
    int iw = read_num(d, got, &i), ih = read_num(d, got, &i);
    read_num(d, got, &i);
    i++;
    for (int y = 0; y < MIN(ih, CHGT); y++)
        for (int x = 0; x < MIN(iw, CWID); x++) {
            size_t o = i + ((size_t)y * iw + x) * 3;
            if (o + 2 < got) pix[y * CWID + x] = RGB(d[o], d[o + 1], d[o + 2]);
        }
    free(d);
    snprintf(status, sizeof status, "opened %s", path);
}

static void click_tools(int x, int y) {
    for (int i = 0; i < NTOOL; i++)
        if (ui_hit(x, y, 8 + (i % 2) * 32, 8 + (i / 2) * 32, 28, 28)) tool = i;
    int by = 8 + ((NTOOL + 1) / 2) * 32 + 28;
    static const int sizes[] = {1, 4, 8, 16};
    for (int i = 0; i < 4; i++)
        if (ui_hit(x, y, 8, by + i * 30, 60, 26)) size = sizes[i];
    by += 4 * 30 + 10;
    if (ui_hit(x, y, 8, by, 60, 26)) {
        uint32_t *t = malloc(CWID * CHGT * 4);
        memcpy(t, pix, CWID * CHGT * 4);
        memcpy(pix, undo, CWID * CHGT * 4);
        memcpy(undo, t, CWID * CHGT * 4);
        free(t);
    }
    if (ui_hit(x, y, 8, by + 32, 60, 26)) {
        push_undo();
        gfx_fill(&img, 0, 0, CWID, CHGT, bg);
    }
    if (ui_hit(x, y, 8, by + 64, 60, 26)) save("/home/painting.ppm");
}

int main(int argc, char **argv) {
    win = win_open(W, H, "Paint", 0);
    if (!win) return 1;
    pix = malloc(CWID * CHGT * 4);
    snap = malloc(CWID * CHGT * 4);
    undo = malloc(CWID * CHGT * 4);
    gfx_init(&img, pix, CWID, CHGT, CWID);
    gfx_init(&snap_c, snap, CWID, CHGT, CWID);
    gfx_fill(&img, 0, 0, CWID, CHGT, bg);
    snprintf(status, sizeof status, "Save writes /home/painting.ppm");
    if (argc > 1) load(argv[1]);
    push_undo();
    srand((unsigned)uptime_ms());
    draw();
    win_update(win);
    for (;;) {
        struct gui_event e;
        int r = win_event(win, &e, drawing && tool == T_SPRAY ? 30 : -1);
        if (r < 0 || (r > 0 && e.type == EV_CLOSE)) break;
        int x = r > 0 ? e.x - CX : lx, y = r > 0 ? e.y - CY : ly;
        if (r == 0 || e.type == EV_MOUSE_MOVE) {
            if (!drawing) continue;
            if (r > 0) {
                lx = x;
                ly = y;
            }
            switch (tool) {
            case T_PENCIL: stroke(sx0, sy0, x, y, 1, dcol); break;
            case T_BRUSH: stroke(sx0, sy0, x, y, size + 2, dcol); break;
            case T_ERASER: stroke(sx0, sy0, x, y, size * 2 + 6, bg); break;
            case T_SPRAY:
                for (int i = 0; i < 12 + size * 2; i++) {
                    int rad = size * 2 + 6;
                    int dx = rand() % (2 * rad + 1) - rad, dy = rand() % (2 * rad + 1) - rad;
                    if (dx * dx + dy * dy <= rad * rad) gfx_pixel(&img, x + dx, y + dy, dcol);
                }
                break;
            case T_LINE: case T_RECT: case T_FRECT: case T_ELLIPSE:
                memcpy(pix, snap, CWID * CHGT * 4);
                shape(sx0, sy0, x, y);
                break;
            }
            if (tool <= T_ERASER) {
                sx0 = x;
                sy0 = y;
            }
        } else if (e.type == EV_MOUSE_DOWN) {
            if (e.x < TOOLW) {
                click_tools(e.x, e.y);
            } else if (e.y >= H - PALH) {
                for (int i = 0; i < NPAL; i++)
                    if (ui_hit(e.x, e.y, CX + 10 + (i % 10) * 26, H - PALH + 4 + (i / 10) * 25, 23, 22)) {
                        if (e.buttons & 2) bg = palette[i];
                        else fg = palette[i];
                    }
            } else if (!drawing) {
                dcol = (e.buttons & 2) ? bg : fg;
                if (tool == T_PICK) {
                    if (x >= 0 && y >= 0 && x < CWID && y < CHGT) {
                        if (e.buttons & 2) bg = pix[y * CWID + x];
                        else fg = pix[y * CWID + x];
                    }
                } else if (tool == T_FILL) {
                    push_undo();
                    flood(x, y, dcol);
                } else {
                    push_undo();
                    memcpy(snap, pix, CWID * CHGT * 4);
                    drawing = true;
                    sx0 = x;
                    sy0 = y;
                    lx = x;
                    ly = y;
                    if (tool == T_PENCIL) stamp(x, y, 1, dcol);
                    else if (tool == T_BRUSH) stamp(x, y, size + 2, dcol);
                    else if (tool == T_ERASER) stamp(x, y, size * 2 + 6, bg);
                }
            }
        } else if (e.type == EV_MOUSE_UP) {
            if (drawing && !(e.buttons & 3)) drawing = false;
        } else if (e.type == EV_KEY && e.pressed) {
            int k = e.key;
            if ((e.mods & NMOD_CTRL) && (k == 'z' || k == 'Z')) click_tools(8, 8 + ((NTOOL + 1) / 2) * 32 + 28 + 130);
            else if ((e.mods & NMOD_CTRL) && (k == 's' || k == 'S')) save("/home/painting.ppm");
            else if (k == '[') size = MAX(1, size - 1);
            else if (k == ']') size = MIN(32, size + 1);
            else if (k >= '1' && k <= '9' && k - '1' < NTOOL) tool = k - '1';
            else if (k == '0') tool = T_SPRAY;
            else continue;
        } else continue;
        draw();
        win_update(win);
    }
    win_close(win);
    return 0;
}
