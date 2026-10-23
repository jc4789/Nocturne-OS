/* Paint: the tools, the brush size and the buttons are widgets of the interface engine
   (libc/ui.c); the picture and the colour palette are surfaces this app draws itself.
   Left button paints with the colour, right button with the background colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "nocturne.h"
#include "ui.h"

#define CWID 824 /* the picture */
#define CHGT 584
#define SIDEW 172
#define BARH 92
#define W (SIDEW + CWID + 24)
#define H (CHGT + 24 + BARH)
#define SWATCH 32 /* a cell of the palette, and its columns and rows */
#define PCOLS 10
#define PROWS 2
#define WINDOW 1
#define S_CANVAS 1
#define S_PALETTE 2

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

static canvas_t img, snap_c;
static canvas_t *canvas_c, *palette_c; /* the surfaces */
static uint32_t *pix, *snap, *undo;
static int tool = T_BRUSH, size = 4;
static uint32_t fg = RGB(30, 110, 230), bg = RGB(255, 255, 255);
static bool drawing;
static int sx0, sy0, lx, ly;
static uint32_t dcol;
static char status[1100];
static char save_path[1024]; /* where Save writes: painting.ppm in the user's Pictures folder */
static int hot_swatch = -1;
/* places in the view that events name */
static uint32_t tool_node[NTOOL], size_node, undo_node, clear_node, save_node, canvas_node, palette_node;
/* the part of the picture that changed since the surface was updated, and where the shape being dragged lies */
static int dirty_x0 = CWID, dirty_y0 = CHGT, dirty_x1, dirty_y1;
static int box[4];
static bool box_valid;

static void push_undo(void) { memcpy(undo, pix, CWID * CHGT * 4); }

/* ---- what changed ---- */

static bool clamp_box(int *x0, int *y0, int *x1, int *y1) {
    *x0 = MAX(0, *x0);
    *y0 = MAX(0, *y0);
    *x1 = MIN(CWID, *x1);
    *y1 = MIN(CHGT, *y1);
    return *x1 > *x0 && *y1 > *y0;
}

static void touch(int x0, int y0, int x1, int y1) {
    if (!clamp_box(&x0, &y0, &x1, &y1)) return;
    dirty_x0 = MIN(dirty_x0, x0);
    dirty_y0 = MIN(dirty_y0, y0);
    dirty_x1 = MAX(dirty_x1, x1);
    dirty_y1 = MAX(dirty_y1, y1);
}

/* Copy what changed to the surface and tell the engine about only that part. */
static void flush(struct ui_session *s) {
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) return;
    gfx_blit(canvas_c, dirty_x0, dirty_y0, &img, dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    ui_surface_changed_rect(s, S_CANVAS, dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
    dirty_x0 = CWID;
    dirty_y0 = CHGT;
    dirty_x1 = dirty_y1 = 0;
}

/* ---- painting ---- */

static void stamp(int x, int y, int r, uint32_t col) {
    if (r <= 1) gfx_pixel(&img, x, y, col);
    else gfx_fill_circle(&img, x, y, r / 2, col);
    touch(x - r / 2 - 1, y - r / 2 - 1, x + r / 2 + 2, y + r / 2 + 2);
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
    if (!st) return;
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
                        int *bigger = realloc(st, cap * 2 * sizeof(int) * 2);
                        if (!bigger) {
                            free(st);
                            return;
                        }
                        st = bigger;
                        cap *= 2;
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
    case T_FRECT:
        gfx_fill(&img, MIN(x0, x1), MIN(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1, dcol);
        touch(MIN(x0, x1), MIN(y0, y1), MAX(x0, x1) + 1, MAX(y0, y1) + 1);
        break;
    case T_ELLIPSE: ellipse(x0, y0, x1, y1, dcol, size); break;
    }
}

/* The pointer went to (x, y) with a button down: carry on with the tool. */
static void paint_move(int x, int y) {
    switch (tool) {
    case T_PENCIL: stroke(sx0, sy0, x, y, 1, dcol); break;
    case T_BRUSH: stroke(sx0, sy0, x, y, size + 2, dcol); break;
    case T_ERASER: stroke(sx0, sy0, x, y, size * 2 + 6, bg); break;
    case T_SPRAY: {
        int rad = size * 2 + 6;
        for (int i = 0; i < 12 + size * 2; i++) {
            int dx = rand() % (2 * rad + 1) - rad, dy = rand() % (2 * rad + 1) - rad;
            if (dx * dx + dy * dy <= rad * rad) gfx_pixel(&img, x + dx, y + dy, dcol);
        }
        touch(x - rad, y - rad, x + rad + 1, y + rad + 1);
        break;
    }
    case T_LINE: case T_RECT: case T_FRECT: case T_ELLIPSE: {
        /* put back what the shape covered before, then draw it again: only in the boxes involved */
        int pad = size + 2, b[4] = {MIN(sx0, x) - pad, MIN(sy0, y) - pad, MAX(sx0, x) + pad + 1, MAX(sy0, y) + pad + 1};
        if (box_valid) {
            int r[4] = {box[0], box[1], box[2], box[3]};
            if (clamp_box(&r[0], &r[1], &r[2], &r[3]))
                for (int row = r[1]; row < r[3]; row++)
                    memcpy(pix + row * CWID + r[0], snap + row * CWID + r[0], (size_t)(r[2] - r[0]) * 4);
            touch(box[0], box[1], box[2], box[3]);
        }
        shape(sx0, sy0, x, y);
        touch(b[0], b[1], b[2], b[3]);
        memcpy(box, b, sizeof box);
        box_valid = true;
        break;
    }
    }
    if (tool <= T_ERASER) {
        sx0 = x;
        sy0 = y;
    }
}

/* ---- files ---- */

static void save(const char *path) {
    char hdr[64];
    int n = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", CWID, CHGT);
    uint8_t *data = malloc((size_t)n + CWID * CHGT * 3);
    if (!data) {
        snprintf(status, sizeof status, "cannot write %s: %s", path, n_result_text(-N_ERR_NO_MEMORY));
        return;
    }
    memcpy(data, hdr, (size_t)n);
    uint8_t *out = data + n;
    for (int i = 0; i < CWID * CHGT; i++) {
        uint32_t p = pix[i];
        *out++ = p >> 16;
        *out++ = p >> 8;
        *out++ = p;
    }
    n_result r = n_document_save(path, data, (size_t)(out - data));
    free(data);
    if (r < 0) snprintf(status, sizeof status, "cannot write %s: %s", path, n_result_text(r));
    else snprintf(status, sizeof status, "saved %s", path);
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
    void *data;
    size_t got;
    n_result r = n_document_load(path, &data, &got);
    if (r < 0) {
        snprintf(status, sizeof status, "cannot open %s: %s", path, n_result_text(r));
        return;
    }
    const uint8_t *d = data;
    size_t i = 2;
    if (got < 2 || d[0] != 'P' || d[1] != '6') {
        snprintf(status, sizeof status, "%s is not a P6 picture", path);
        n_memory_release(data, got);
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
    n_memory_release(data, got);
    snprintf(status, sizeof status, "opened %s", path);
}

/* Undo swaps the picture with the one before the last change, so a second undo redoes. */
static void do_undo(void) {
    uint32_t *t = malloc(CWID * CHGT * 4);
    if (!t) return;
    memcpy(t, pix, CWID * CHGT * 4);
    memcpy(pix, undo, CWID * CHGT * 4);
    memcpy(undo, t, CWID * CHGT * 4);
    free(t);
    touch(0, 0, CWID, CHGT);
}

static void do_clear(void) {
    push_undo();
    gfx_fill(&img, 0, 0, CWID, CHGT, bg);
    touch(0, 0, CWID, CHGT);
}

/* ---- the palette ---- */

static void draw_palette(canvas_t *c) {
    const struct ui_theme *t = &ui_theme;
    gfx_fill(c, 0, 0, SWATCH * PCOLS, SWATCH * PROWS, t->sidebar);
    for (int i = 0; i < NPAL; i++) {
        float px = (float)(i % PCOLS * SWATCH), py = (float)(i / PCOLS * SWATCH);
        if (i == hot_swatch) ui_round_fill(c, px + 1, py + 1, SWATCH - 2, SWATCH - 2, 9, t->surfaceHover);
        ui_round_fill(c, px + 4, py + 4, SWATCH - 8, SWATCH - 8, 7, palette[i]);
        ui_round_stroke(c, px + 4, py + 4, SWATCH - 8, SWATCH - 8, 7, 1, ARGB(90, 128, 128, 140));
        if (palette[i] == bg) ui_round_stroke(c, px + 1.5f, py + 1.5f, SWATCH - 3, SWATCH - 3, 9, 1.5f, t->textDim);
        if (palette[i] == fg) ui_round_stroke(c, px + 1.5f, py + 1.5f, SWATCH - 3, SWATCH - 3, 9, 2, t->accent);
    }
}

static int swatch_at(double a, double b) {
    if (a < 0 || b < 0) return -1;
    int col = (int)a / SWATCH, row = (int)b / SWATCH;
    int i = row * PCOLS + col;
    return col < PCOLS && row < PROWS && i < NPAL ? i : -1;
}

/* ---- the window ---- */

static void colour_box(struct ui_msg *m, const char *name, uint32_t col) {
    ui_node(m, "column"); ui_num(m, "gap", 4); ui_str(m, "align", "center");
        ui_node(m, "row"); ui_num(m, "width", 38); ui_num(m, "height", 38); ui_num(m, "radius", 9);
        ui_num(m, "background", col); ui_num(m, "border", ui_theme.textFaint); ui_end(m);
        ui_node(m, "text"); ui_str(m, "text", name); ui_str(m, "style", "caption"); ui_end(m);
    ui_end(m);
}

static void show(struct ui_session *s) {
    const struct ui_theme *t = &ui_theme;
    char px[16], hint[96];
    snprintf(px, sizeof px, "%d px", size);
    snprintf(hint, sizeof hint, "%s, size %d   right click: background", tool_name[tool], size);
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "gap", 0);
        ui_node(&m, "row"); ui_num(&m, "grow", 1); ui_num(&m, "gap", 0); ui_str(&m, "align", "stretch");
            ui_node(&m, "column"); ui_num(&m, "width", SIDEW); ui_num(&m, "padding", 12); ui_num(&m, "gap", 8);
            ui_num(&m, "background", t->sidebar);
                ui_node(&m, "text"); ui_str(&m, "text", "Tools"); ui_str(&m, "style", "heading"); ui_end(&m);
                ui_node(&m, "column"); ui_num(&m, "gap", 4);
                    for (int i = 0; i < NTOOL; i += 2) {
                        ui_node(&m, "row"); ui_num(&m, "gap", 4);
                        for (int j = i; j < i + 2 && j < NTOOL; j++) {
                            tool_node[j] = ui_node(&m, "button");
                            ui_str(&m, "text", tool_name[j]); ui_num(&m, "compact", 1); ui_num(&m, "width", 72);
                            ui_str(&m, "variant", j == tool ? "primary" : "secondary"); ui_end(&m);
                        }
                        ui_end(&m);
                    }
                ui_end(&m);
                ui_node(&m, "row");
                    ui_node(&m, "text"); ui_str(&m, "text", "Size"); ui_str(&m, "style", "heading"); ui_end(&m);
                    ui_node(&m, "spacer"); ui_end(&m);
                    ui_node(&m, "text"); ui_str(&m, "text", px); ui_num(&m, "weight", 1); ui_end(&m);
                ui_end(&m);
                size_node = ui_node(&m, "slider");
                ui_num(&m, "value", size); ui_num(&m, "min", 1); ui_num(&m, "max", 32); ui_num(&m, "step", 1);
                ui_num(&m, "wheelStep", 1); ui_end(&m);
                ui_node(&m, "divider"); ui_end(&m);
                undo_node = ui_node(&m, "button"); ui_str(&m, "text", "Undo"); ui_str(&m, "icon", "chevron-left"); ui_end(&m);
                clear_node = ui_node(&m, "button"); ui_str(&m, "text", "Clear"); ui_str(&m, "icon", "close"); ui_end(&m);
                save_node = ui_node(&m, "button"); ui_str(&m, "text", "Save"); ui_str(&m, "variant", "primary");
                ui_str(&m, "icon", "folder"); ui_end(&m);
            ui_end(&m);
            ui_node(&m, "column"); ui_num(&m, "padding", 12);
                canvas_node = ui_node(&m, "surface"); ui_num(&m, "surface", S_CANVAS); ui_num(&m, "pointer", 1);
                ui_num(&m, "width", CWID); ui_num(&m, "height", CHGT); ui_end(&m);
            ui_end(&m);
        ui_end(&m);
        ui_node(&m, "row"); ui_nums(&m, "padding", (const double[]){0, 16}, 2); ui_num(&m, "gap", 18);
        ui_num(&m, "height", BARH); ui_num(&m, "background", t->sidebar);
            ui_node(&m, "row"); ui_num(&m, "gap", 12); ui_str(&m, "align", "start");
                colour_box(&m, "Colour", fg);
                colour_box(&m, "Back", bg);
            ui_end(&m);
            palette_node = ui_node(&m, "surface"); ui_num(&m, "surface", S_PALETTE); ui_num(&m, "pointer", 1);
            ui_num(&m, "width", SWATCH * PCOLS); ui_num(&m, "height", SWATCH * PROWS); ui_end(&m);
            ui_node(&m, "column"); ui_num(&m, "grow", 1); ui_num(&m, "gap", 4);
                ui_node(&m, "text"); ui_str(&m, "text", status); ui_num(&m, "wrap", 1); ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", hint); ui_str(&m, "style", "caption"); ui_num(&m, "wrap", 1); ui_end(&m);
            ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
}

/* The pointer on the picture. Returns whether the colours changed. */
static bool canvas_pointer(const struct ui_event *e, bool *moved) {
    int x = (int)floor(e->a), y = (int)floor(e->b);
    uint32_t buttons = UI_BUTTONS(e->flags);
    if (e->flags & UI_DOWN) {
        if (drawing) return false;
        dcol = (buttons & 2) ? bg : fg;
        if (tool == T_PICK) {
            if (x >= 0 && y >= 0 && x < CWID && y < CHGT) {
                if (buttons & 2) bg = pix[y * CWID + x];
                else fg = pix[y * CWID + x];
                return true;
            }
        } else if (tool == T_FILL) {
            push_undo();
            flood(x, y, dcol);
            touch(0, 0, CWID, CHGT);
        } else {
            push_undo();
            memcpy(snap, pix, CWID * CHGT * 4);
            drawing = true;
            box_valid = false;
            sx0 = lx = x;
            sy0 = ly = y;
            if (tool == T_PENCIL) stamp(x, y, 1, dcol);
            else if (tool == T_BRUSH) stamp(x, y, size + 2, dcol);
            else if (tool == T_ERASER) stamp(x, y, size * 2 + 6, bg);
        }
    } else if (e->flags & UI_UP) {
        if (drawing && !(buttons & 3)) drawing = false;
    } else if (drawing) {
        lx = x;
        ly = y;
        paint_move(x, y);
        *moved = true;
    }
    return false;
}

int main(int argc, char **argv) {
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, WINDOW, W, H, 0, "Paint", NULL, NULL) < 0) return 1;
    canvas_c = ui_surface(s, S_CANVAS, CWID, CHGT, 0);
    palette_c = ui_surface(s, S_PALETTE, SWATCH * PCOLS, SWATCH * PROWS, 0);
    pix = malloc(CWID * CHGT * 4);
    snap = malloc(CWID * CHGT * 4);
    undo = malloc(CWID * CHGT * 4);
    if (!canvas_c || !palette_c || !pix || !snap || !undo) return 1;
    gfx_init(&img, pix, CWID, CHGT, CWID);
    gfx_init(&snap_c, snap, CWID, CHGT, CWID);
    gfx_fill(&img, 0, 0, CWID, CHGT, bg);
    n_path_join(n_user_folder(), "Pictures\\painting.ppm", save_path, sizeof save_path);
    snprintf(status, sizeof status, "Save writes %s", save_path);
    char named[1024];
    if (argc > 1 && n_path_complete(argv[1], named, sizeof named) > 0) load(named);
    push_undo();
    srand((unsigned)n_clock_uptime_ms());
    touch(0, 0, CWID, CHGT);
    flush(s);
    draw_palette(palette_c);
    ui_surface_changed(s, S_PALETTE);
    show(s);
    for (bool running = true; running;) {
        ui_session_wait(s, drawing && tool == T_SPRAY ? 30 : -1);
        bool view = false, colours = false, moved = false;
        struct ui_event e;
        char text[16];
        while (ui_next_event(s, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLOSE:
                running = false;
                break;
            case UI_EVENT_CLICK:
                for (int i = 0; i < NTOOL; i++)
                    if (e.node == tool_node[i]) tool = i, view = true;
                if (e.node == undo_node) do_undo();
                else if (e.node == clear_node) do_clear();
                else if (e.node == save_node) save(save_path), view = true;
                break;
            case UI_EVENT_SLIDE:
                if (e.node == size_node) size = MAX(1, MIN(32, (int)lround(e.a))), view = true;
                break;
            case UI_EVENT_POINTER:
                if (e.node == canvas_node) {
                    if (canvas_pointer(&e, &moved)) view = colours = true;
                } else if (e.node == palette_node) {
                    int i = swatch_at(e.a, e.b);
                    if (e.flags & UI_DOWN) {
                        if (i >= 0) {
                            if (UI_BUTTONS(e.flags) & 2) bg = palette[i];
                            else fg = palette[i];
                            view = colours = true;
                        }
                    } else if (!(e.flags & UI_UP) && i != hot_swatch) {
                        hot_swatch = i;
                        colours = true;
                    }
                }
                break;
            case UI_EVENT_KEY: {
                if (e.flags & UI_UP) break;
                int k = (int)e.a;
                if ((e.mods & NMOD_CTRL) && (k == 'z' || k == 'Z')) do_undo();
                else if ((e.mods & NMOD_CTRL) && (k == 's' || k == 'S')) save(save_path), view = true;
                else if (k == '[') size = MAX(1, size - 1), view = true;
                else if (k == ']') size = MIN(32, size + 1), view = true;
                else if (k >= '1' && k <= '9' && k - '1' < NTOOL) tool = k - '1', view = true;
                else if (k == '0') tool = T_SPRAY, view = true;
                break;
            }
            }
        }
        if (!running || !ui_session_open(s)) break;
        if (drawing && tool == T_SPRAY && !moved) paint_move(lx, ly); /* the spray keeps going while held still */
        flush(s);
        if (colours) {
            draw_palette(palette_c);
            ui_surface_changed(s, S_PALETTE);
        }
        if (view) show(s);
    }
    ui_session_free(s);
    return 0;
}
