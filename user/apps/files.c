/* File manager */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define TB 40     /* toolbar height */
#define HDR 22    /* column header */
#define ROW 24
#define SB 22     /* status bar */
#define MAXE 512

struct entry {
    char name[256];
    uint32_t type;
    uint64_t size;
};

static window_t *w;
static char cwd[512] = "/home";
static struct entry ents[MAXE];
static int nent, sel = -1, scroll, hot_btn = -1;
static char status[128];
static uint64_t last_click;
static int last_click_idx = -1;

enum { B_UP, B_HOME, B_ROOT, B_NEWDIR, B_NEWFILE, B_DELETE, NBTN };
static const char *btn_label[NBTN] = {"Up", "Home", "/", "New folder", "New file", "Delete"};
static int btn_x[NBTN], btn_w[NBTN];

static int cmp(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if ((x->type == N_FT_DIR) != (y->type == N_FT_DIR)) return x->type == N_FT_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void join(char *out, size_t n, const char *dir, const char *name) {
    if (!strcmp(dir, "/")) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", dir, name);
}

static void load(void) {
    nent = 0;
    sel = -1;
    scroll = 0;
    int fd = open(cwd, O_RDONLY);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot open %s", cwd);
        return;
    }
    struct n_dirent d;
    for (int i = 0; nent < MAXE && readdir(fd, i, &d) > 0; i++) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        strlcpy(ents[nent].name, d.name, sizeof ents[nent].name);
        ents[nent].type = d.type;
        ents[nent].size = d.size;
        nent++;
    }
    close(fd);
    qsort(ents, nent, sizeof ents[0], cmp);
    snprintf(status, sizeof status, "%d item%s", nent, nent == 1 ? "" : "s");
    char t[300];
    snprintf(t, sizeof t, "Files - %s", cwd);
    win_set_title(w, t);
}

static void go(const char *path) {
    strlcpy(cwd, path, sizeof cwd);
    load();
}

static void go_up(void) {
    char *s = strrchr(cwd, '/');
    if (!s) return;
    if (s == cwd) cwd[1] = 0;
    else *s = 0;
    load();
}

static const char *type_name(const struct entry *e) {
    if (e->type == N_FT_DIR) return "Folder";
    if (e->type == N_FT_CHAR) return "Device";
    const char *dot = strrchr(e->name, '.');
    if (!strcmp(cwd, "/bin")) return "Program";
    if (dot && !strcasecmp(dot, ".txt")) return "Text";
    if (dot && !strcasecmp(dot, ".ppm")) return "Picture";
    if (dot && !strcasecmp(dot, ".wav")) return "Sound";
    if (dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))) return "Web page";
    if (dot && !strcasecmp(dot, ".sh")) return "Script";
    return "File";
}

static void icon(canvas_t *c, int x, int y, const struct entry *e) {
    if (e->type == N_FT_DIR) {
        gfx_fill_round(c, x, y + 2, 8, 4, 1, RGB(230, 170, 60));
        gfx_fill_round(c, x, y + 4, 16, 12, 2, RGB(250, 196, 80));
        gfx_hline(c, x + 1, y + 6, 14, RGB(255, 220, 130));
    } else if (e->type == N_FT_CHAR) {
        gfx_fill_round(c, x + 1, y + 2, 14, 14, 3, RGB(100, 110, 140));
        gfx_fill(c, x + 4, y + 6, 8, 6, RGB(40, 46, 60));
    } else if (!strcmp(type_name(e), "Sound")) { /* a quaver */
        gfx_fill_round(c, x, y + 1, 16, 16, 3, RGB(64, 44, 130));
        gfx_fill_circle(c, x + 6, y + 12, 3, RGB(246, 236, 196));
        gfx_fill(c, x + 8, y + 3, 2, 9, RGB(246, 236, 196));
        gfx_line(c, x + 9, y + 3, x + 13, y + 7, RGB(246, 236, 196));
    } else if (!strcmp(type_name(e), "Program")) {
        gfx_fill_round(c, x, y + 1, 16, 15, 2, RGB(70, 66, 120));
        gfx_fill(c, x + 1, y + 4, 14, 11, RGB(20, 18, 34));
        gfx_text(c, x + 3, y + 1, ">", RGB(110, 230, 140), TRANSPARENT, FONT_SMALL);
    } else {
        uint32_t col = !strcmp(type_name(e), "Picture") ? RGB(120, 200, 250) : RGB(230, 230, 240);
        gfx_fill(c, x + 2, y, 10, 17, col);
        gfx_fill(c, x + 2, y + 4, 13, 13, col);
        gfx_triangle(c, x + 12, y, x + 12, y + 4, x + 15, y + 4, RGB(160, 160, 180));
        for (int i = 0; i < 3; i++) gfx_hline(c, x + 4, y + 7 + i * 3, 9, RGB(150, 150, 170));
    }
}

static int rows_visible(void) { return (w->h - TB - HDR - SB) / ROW; }

static void human(uint64_t n, char *out) {
    if (n < 1024) snprintf(out, 24, "%lu B", (unsigned long)n);
    else if (n < 1024 * 1024) snprintf(out, 24, "%.1f KiB", n / 1024.0);
    else snprintf(out, 24, "%.1f MiB", n / 1048576.0);
}

static void draw(void) {
    canvas_t *c = &w->c;
    int W = w->w, H = w->h;
    gfx_fill(c, 0, 0, W, H, RGB(26, 24, 44));
    gfx_gradient_v(c, 0, 0, W, TB, RGB(46, 42, 80), RGB(36, 32, 64));
    int x = 8;
    for (int i = 0; i < NBTN; i++) {
        btn_w[i] = gfx_text_width(btn_label[i], FONT_SMALL) + 20;
        btn_x[i] = x;
        ui_button(c, x, 6, btn_w[i], 28, btn_label[i], hot_btn == i, false);
        x += btn_w[i] + 6;
        if (i == B_ROOT) x += 10;
    }
    if (x + 40 < W) ui_textfield(c, x + 6, 8, W - x - 14, cwd, false);
    int y = TB;
    gfx_fill(c, 0, y, W, HDR, RGB(36, 34, 60));
    int col_size = W - 200, col_type = W - 100;
    gfx_text(c, 34, y + 3, "Name", UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, col_size, y + 3, "Size", UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, col_type, y + 3, "Type", UI_DIM, TRANSPARENT, FONT_SMALL);
    y += HDR;
    int vis = rows_visible();
    for (int i = 0; i < vis && scroll + i < nent; i++) {
        int idx = scroll + i, ry = y + i * ROW;
        struct entry *e = &ents[idx];
        if (idx == sel) gfx_fill_round(c, 4, ry + 1, W - 8, ROW - 2, 4, RGB(84, 70, 170));
        else if (i & 1) gfx_fill(c, 0, ry, W, ROW, RGB(30, 28, 50));
        icon(c, 10, ry + 3, e);
        canvas_t clip = *c;
        gfx_clip(&clip, 0, ry, col_size - 12, ROW);
        gfx_text(&clip, 34, ry + 4, e->name, UI_FG, TRANSPARENT, FONT_SMALL);
        if (e->type != N_FT_DIR) {
            char b[24];
            human(e->size, b);
            gfx_text(c, col_size, ry + 4, b, UI_DIM, TRANSPARENT, FONT_SMALL);
        }
        gfx_text(c, col_type, ry + 4, type_name(e), UI_DIM, TRANSPARENT, FONT_SMALL);
    }
    if (nent > vis) {
        int th = MAX(20, vis * (H - TB - HDR - SB) / nent);
        int ty = TB + HDR + scroll * (H - TB - HDR - SB - th) / MAX(1, nent - vis);
        gfx_fill_round(c, W - 6, ty, 4, th, 2, RGB(110, 100, 170));
    }
    gfx_fill(c, 0, H - SB, W, SB, RGB(36, 32, 62));
    gfx_text(c, 8, H - SB + 3, status, UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, W - 8 - gfx_text_width("Enter: open  Del: delete", FONT_SMALL), H - SB + 3,
             "Enter: open  Del: delete", RGB(100, 100, 140), TRANSPARENT, FONT_SMALL);
}

static void open_entry(int i) {
    if (i < 0 || i >= nent) return;
    char path[800];
    join(path, sizeof path, cwd, ents[i].name);
    if (ents[i].type == N_FT_DIR) {
        go(path);
        return;
    }
    const char *t = type_name(&ents[i]);
    int r;
    if (!strcmp(t, "Program")) r = gui_launch(path, NULL);
    else if (!strcmp(t, "Picture")) r = gui_launch("/bin/paint", path);
    else if (!strcmp(t, "Sound")) r = gui_launch("/bin/player", path);
    else if (!strcmp(t, "Web page")) r = gui_launch("/bin/browser", path);
    else r = gui_launch("/bin/notepad", path);
    snprintf(status, sizeof status, r < 0 ? "could not open %s" : "opened %s", ents[i].name);
}

static void make_new(bool dir) {
    char path[800], name[256];
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(name, sizeof name, dir ? "New folder" : "untitled.txt");
        else snprintf(name, sizeof name, dir ? "New folder %d" : "untitled%d.txt", n);
        join(path, sizeof path, cwd, name);
        struct n_stat st;
        if (stat(path, &st) < 0) break;
    }
    int r;
    if (dir) r = mkdir(path);
    else {
        r = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (r >= 0) close(r);
    }
    load();
    if (r < 0) snprintf(status, sizeof status, "could not create %s", name);
    for (int i = 0; i < nent; i++)
        if (!strcmp(ents[i].name, name)) sel = i;
}

static void delete_sel(void) {
    if (sel < 0) return;
    char path[800], name[256];
    strlcpy(name, ents[sel].name, sizeof name);
    join(path, sizeof path, cwd, name);
    int r = ents[sel].type == N_FT_DIR ? rmdir(path) : unlink(path);
    int keep = sel;
    load();
    sel = MIN(keep, nent - 1);
    if (r < 0) snprintf(status, sizeof status, "could not delete %s: %s", name, strerror(-r));
    else snprintf(status, sizeof status, "deleted %s", name);
}

static void ensure_visible(void) {
    int vis = rows_visible();
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + vis) scroll = sel - vis + 1;
    if (scroll < 0) scroll = 0;
}

int main(int argc, char **argv) {
    w = win_open(620, 440, "Files", WIN_RESIZABLE);
    if (!w) return 1;
    go(argc > 1 ? argv[1] : "/home");
    draw();
    win_update(w);
    for (;;) {
        struct gui_event e;
        if (win_event(w, &e, -1) < 0 || e.type == EV_CLOSE) break;
        switch (e.type) {
        case EV_MOUSE_MOVE: {
            int h = -1;
            for (int i = 0; i < NBTN; i++)
                if (ui_hit(e.x, e.y, btn_x[i], 6, btn_w[i], 28)) h = i;
            if (h == hot_btn) continue;
            hot_btn = h;
            break;
        }
        case EV_MOUSE_DOWN:
            if (e.y >= TB + HDR && e.y < w->h - SB) {
                int idx = scroll + (e.y - TB - HDR) / ROW;
                if (idx >= nent) {
                    sel = -1;
                    break;
                }
                uint64_t now = uptime_ms();
                if (idx == last_click_idx && now - last_click < 450) {
                    open_entry(idx);
                    last_click_idx = -1;
                } else {
                    sel = idx;
                    last_click = now;
                    last_click_idx = idx;
                }
            }
            break;
        case EV_MOUSE_UP:
            if (e.y < TB)
                for (int i = 0; i < NBTN; i++)
                    if (ui_hit(e.x, e.y, btn_x[i], 6, btn_w[i], 28)) {
                        if (i == B_UP) go_up();
                        else if (i == B_HOME) go("/home");
                        else if (i == B_ROOT) go("/");
                        else if (i == B_NEWDIR) make_new(true);
                        else if (i == B_NEWFILE) make_new(false);
                        else if (i == B_DELETE) delete_sel();
                    }
            break;
        case EV_WHEEL:
            scroll = MAX(0, MIN(nent - rows_visible(), scroll + e.wheel * 3));
            if (scroll < 0) scroll = 0;
            break;
        case EV_KEY:
            if (!e.pressed) continue;
            if (e.key == NKEY_DOWN) sel = MIN(nent - 1, sel + 1);
            else if (e.key == NKEY_UP) sel = MAX(0, sel - 1);
            else if (e.key == NKEY_HOME) sel = 0;
            else if (e.key == NKEY_END) sel = nent - 1;
            else if (e.key == NKEY_PGDN) sel = MIN(nent - 1, sel + rows_visible());
            else if (e.key == NKEY_PGUP) sel = MAX(0, sel - rows_visible());
            else if (e.key == NKEY_ENTER) open_entry(sel);
            else if (e.key == NKEY_BACKSPACE) go_up();
            else if (e.key == NKEY_DELETE) delete_sel();
            else if (e.key == NKEY_F5) load();
            else continue;
            ensure_visible();
            break;
        case EV_RESIZE: break;
        default: continue;
        }
        draw();
        win_update(w);
    }
    win_close(w);
    return 0;
}
