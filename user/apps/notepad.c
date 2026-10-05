/* Text editor */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nocturne.h"

#define TB 40
#define SB 22
#define GUT 44 /* line-number gutter */
#define CW 8
#define LH 16

static window_t *w;
static char *text;
static size_t len, cap, cur;
static long anc = -1; /* selection anchor, -1 = none */
static int top, left;
static bool modified, prompt_open, dragging;
static char path[256], prompt_buf[256], status[128];
static int hot_btn = -1;
static int want_col = -1;

enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, NBTN };
static const char *btn_label[NBTN] = {"New", "Open", "Save", "Save as"};
static int btn_x[NBTN], btn_w[NBTN];
static int prompt_kind; /* B_OPEN or B_SAVEAS */

static void reserve(size_t n) {
    if (n <= cap) return;
    while (cap < n) cap = cap ? cap * 2 : 4096;
    text = realloc(text, cap);
}

static void insert(const char *s, size_t n) {
    reserve(len + n + 1);
    memmove(text + cur + n, text + cur, len - cur);
    memcpy(text + cur, s, n);
    len += n;
    cur += n;
    modified = true;
}

static void erase(size_t a, size_t b) {
    if (b <= a) return;
    memmove(text + a, text + b, len - b);
    len -= b - a;
    cur = a;
    modified = true;
}

static bool sel_range(size_t *a, size_t *b) {
    if (anc < 0 || (size_t)anc == cur) return false;
    *a = MIN((size_t)anc, cur);
    *b = MAX((size_t)anc, cur);
    return true;
}

static bool delete_sel(void) {
    size_t a, b;
    if (!sel_range(&a, &b)) return false;
    erase(a, b);
    anc = -1;
    return true;
}

static size_t line_start(size_t p) {
    while (p > 0 && text[p - 1] != '\n') p--;
    return p;
}
static size_t line_end(size_t p) {
    while (p < len && text[p] != '\n') p++;
    return p;
}
static int line_of(size_t p) {
    int n = 0;
    for (size_t i = 0; i < p; i++)
        if (text[i] == '\n') n++;
    return n;
}
static int total_lines(void) { return line_of(len) + 1; }
static size_t pos_of(int line, int col) {
    size_t p = 0;
    for (int l = 0; l < line && p < len; p++)
        if (text[p] == '\n') l++;
    size_t e = line_end(p);
    return MIN(p + (size_t)MAX(col, 0), e);
}

static int text_rows(void) { return (w->h - TB - SB - 8) / LH; }
static int text_cols(void) { return (w->w - GUT - 12) / CW; }

static void ensure_cursor_visible(void) {
    int line = line_of(cur), col = (int)(cur - line_start(cur));
    int rows = text_rows(), cols = text_cols();
    if (line < top) top = line;
    if (line >= top + rows) top = line - rows + 1;
    if (col < left) left = col;
    if (col >= left + cols) left = col - cols + 1;
}

static void set_title(void) {
    char t[300];
    const char *name = path[0] ? strrchr(path, '/') ? strrchr(path, '/') + 1 : path : "untitled";
    snprintf(t, sizeof t, "%s%s - Text Editor", modified ? "*" : "", name);
    win_set_title(w, t);
}

static bool load_file(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot open %s", p);
        return false;
    }
    len = cur = 0;
    char buf[4096];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        reserve(len + n + 1);
        memcpy(text + len, buf, n);
        len += n;
    }
    close(fd);
    strlcpy(path, p, sizeof path);
    modified = false;
    anc = -1;
    top = left = 0;
    snprintf(status, sizeof status, "opened %s (%lu bytes)", p, (unsigned long)len);
    return true;
}

static bool save_file(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot write %s", p);
        return false;
    }
    size_t off = 0;
    while (off < len) {
        long n = write(fd, text + off, len - off);
        if (n <= 0) break;
        off += n;
    }
    close(fd);
    strlcpy(path, p, sizeof path);
    modified = false;
    snprintf(status, sizeof status, "saved %s (%lu bytes)", p, (unsigned long)len);
    return true;
}

static void open_prompt(int kind) {
    prompt_open = true;
    prompt_kind = kind;
    strlcpy(prompt_buf, path[0] ? path : "/home/", sizeof prompt_buf);
}

static void draw(void) {
    canvas_t *c = &w->c;
    int W = w->w, H = w->h;
    gfx_gradient_v(c, 0, 0, W, TB, RGB(46, 42, 80), RGB(36, 32, 64));
    int x = 8;
    for (int i = 0; i < NBTN; i++) {
        btn_w[i] = gfx_text_width(btn_label[i], FONT_SMALL) + 22;
        btn_x[i] = x;
        ui_button(c, x, 6, btn_w[i], 28, btn_label[i], hot_btn == i, false);
        x += btn_w[i] + 6;
    }
    if (prompt_open) {
        const char *lab = prompt_kind == B_OPEN ? "Open:" : "Save as:";
        gfx_text(c, x + 8, 12, lab, UI_ACCENT2, TRANSPARENT, FONT_SMALL);
        int fx = x + 16 + gfx_text_width(lab, FONT_SMALL);
        ui_textfield(c, fx, 8, W - fx - 10, prompt_buf, true);
    } else {
        gfx_fill(c, x, 0, W - x, TB - 1, RGB(40, 36, 70));
        gfx_gradient_v(c, x, 0, W - x, TB, RGB(46, 42, 80), RGB(36, 32, 64));
        gfx_text(c, x + 10, 12, path[0] ? path : "(not saved yet)", UI_DIM, TRANSPARENT, FONT_SMALL);
    }
    gfx_fill(c, 0, TB, W, H - TB - SB, RGB(20, 19, 34));
    gfx_fill(c, 0, TB, GUT, H - TB - SB, RGB(28, 26, 46));
    size_t sa = 0, sb = 0;
    bool has_sel = sel_range(&sa, &sb);
    int rows = text_rows(), cols = text_cols();
    size_t p = pos_of(top, 0);
    if (top > 0 && p == len && line_of(len) < top) p = len;
    int cur_line = line_of(cur);
    canvas_t clip = *c;
    gfx_clip(&clip, GUT, TB, W - GUT, H - TB - SB);
    for (int r = 0; r < rows; r++) {
        int ln = top + r;
        int y = TB + 4 + r * LH;
        if (ln > 0 && p >= len && (len == 0 || text[len - 1] != '\n' || ln > line_of(len))) break;
        if (ln == cur_line) gfx_fill(c, GUT, y, W - GUT, LH, RGB(30, 28, 52));
        char num[12];
        snprintf(num, sizeof num, "%d", ln + 1);
        gfx_text(c, GUT - 8 - gfx_text_width(num, FONT_SMALL), y, num,
                 ln == cur_line ? UI_ACCENT2 : RGB(90, 88, 130), TRANSPARENT, FONT_SMALL);
        size_t e = line_end(p);
        for (size_t i = p; i <= e; i++) {
            int col = (int)(i - p) - left;
            int cx = GUT + 6 + col * CW;
            if (col >= 0 && col <= cols) {
                if (has_sel && i >= sa && i < sb) gfx_fill(&clip, cx, y, CW, LH, RGB(80, 70, 160));
                if (i < e) {
                    char ch = text[i];
                    gfx_char(&clip, cx, y, ch == '\t' ? ' ' : (uint8_t)ch, UI_FG, TRANSPARENT, FONT_SMALL);
                }
                if (i == cur && !prompt_open) gfx_fill(&clip, cx, y, 2, LH, UI_ACCENT2);
            }
        }
        p = e + 1;
    }
    gfx_fill(c, 0, H - SB, W, SB, RGB(36, 32, 62));
    char st[96];
    snprintf(st, sizeof st, "Ln %d, Col %d  |  %d lines%s", cur_line + 1, (int)(cur - line_start(cur)) + 1,
             total_lines(), modified ? "  |  modified" : "");
    gfx_text(c, W - 8 - gfx_text_width(st, FONT_SMALL), H - SB + 3, st, UI_DIM, TRANSPARENT, FONT_SMALL);
    gfx_text(c, 8, H - SB + 3, status, RGB(170, 160, 220), TRANSPARENT, FONT_SMALL);
}

static size_t hit_pos(int mx, int my) {
    int line = top + (my - TB - 4) / LH;
    int col = left + (mx - GUT - 6 + CW / 2) / CW;
    if (line < 0) line = 0;
    int tl = total_lines() - 1;
    if (line > tl) return len;
    return pos_of(line, col);
}

static void copy_sel(void) {
    size_t a, b;
    if (sel_range(&a, &b)) clipboard_set(text + a, b - a);
    else {
        size_t s = line_start(cur), e = line_end(cur);
        clipboard_set(text + s, e - s);
    }
}

static void key(const struct gui_event *e) {
    bool ctrl = e->mods & NMOD_CTRL, shift = e->mods & NMOD_SHIFT;
    int k = e->key;
    if (ctrl && k < 128) k = tolower(k);
    if (ctrl) {
        if (k == 's') {
            if (path[0]) save_file(path);
            else open_prompt(B_SAVEAS);
        } else if (k == 'o') open_prompt(B_OPEN);
        else if (k == 'n') {
            len = cur = 0;
            path[0] = 0;
            modified = false;
            anc = -1;
        } else if (k == 'a') {
            anc = 0;
            cur = len;
        } else if (k == 'c') copy_sel();
        else if (k == 'x') {
            copy_sel();
            if (!delete_sel()) {
                size_t s = line_start(cur), en = line_end(cur);
                erase(s, en < len ? en + 1 : en);
            }
        } else if (k == 'v') {
            char *buf = malloc(65536);
            int n = clipboard_get(buf, 65536);
            if (n > 0) {
                delete_sel();
                insert(buf, n);
            }
            free(buf);
        } else if (k == NKEY_HOME) cur = 0;
        else if (k == NKEY_END) cur = len;
        return;
    }
    bool move = k == NKEY_LEFT || k == NKEY_RIGHT || k == NKEY_UP || k == NKEY_DOWN || k == NKEY_HOME ||
                k == NKEY_END || k == NKEY_PGUP || k == NKEY_PGDN;
    if (move) {
        if (shift && anc < 0) anc = (long)cur;
        if (!shift) anc = -1;
        int line = line_of(cur), col = (int)(cur - line_start(cur));
        if (k != NKEY_UP && k != NKEY_DOWN && k != NKEY_PGUP && k != NKEY_PGDN) want_col = -1;
        else if (want_col < 0) want_col = col;
        switch (k) {
        case NKEY_LEFT: if (cur) cur--; break;
        case NKEY_RIGHT: if (cur < len) cur++; break;
        case NKEY_UP: if (line) cur = pos_of(line - 1, want_col); else cur = 0; break;
        case NKEY_DOWN: if (line < total_lines() - 1) cur = pos_of(line + 1, want_col); else cur = len; break;
        case NKEY_PGUP: cur = pos_of(MAX(0, line - text_rows()), want_col); break;
        case NKEY_PGDN: cur = pos_of(MIN(total_lines() - 1, line + text_rows()), want_col); break;
        case NKEY_HOME: cur = line_start(cur); break;
        case NKEY_END: cur = line_end(cur); break;
        }
        return;
    }
    want_col = -1;
    if (k == NKEY_BACKSPACE) {
        if (!delete_sel() && cur) erase(cur - 1, cur);
    } else if (k == NKEY_DELETE) {
        if (!delete_sel() && cur < len) erase(cur, cur + 1);
    } else if (k == NKEY_ENTER) {
        delete_sel();
        /* keep indentation */
        size_t s = line_start(cur), i = s;
        while (i < cur && (text[i] == ' ' || text[i] == '\t')) i++;
        char ind[128];
        size_t n = MIN(i - s, sizeof ind - 1);
        memcpy(ind, text + s, n);
        insert("\n", 1);
        insert(ind, n);
    } else if (k == NKEY_TAB) {
        delete_sel();
        insert("    ", 4);
    } else if (k >= 32 && k < 127) {
        delete_sel();
        char ch = (char)k;
        insert(&ch, 1);
    }
}

static void button(int i) {
    if (i == B_NEW) {
        struct gui_event e = {.type = EV_KEY, .key = 'n', .mods = NMOD_CTRL, .pressed = 1};
        key(&e);
    } else if (i == B_OPEN) open_prompt(B_OPEN);
    else if (i == B_SAVE) {
        if (path[0]) save_file(path);
        else open_prompt(B_SAVEAS);
    } else if (i == B_SAVEAS) open_prompt(B_SAVEAS);
}

int main(int argc, char **argv) {
    w = win_open(640, 460, "Text Editor", WIN_RESIZABLE);
    if (!w) return 1;
    reserve(4096);
    if (argc > 1) {
        if (!load_file(argv[1])) {
            strlcpy(path, argv[1], sizeof path);
            snprintf(status, sizeof status, "new file %s", argv[1]);
        }
    } else snprintf(status, sizeof status, "Ctrl+S save, Ctrl+O open");
    set_title();
    draw();
    win_update(w);
    bool was_modified = modified;
    for (;;) {
        struct gui_event e;
        if (win_event(w, &e, -1) < 0) break;
        if (e.type == EV_CLOSE) break;
        switch (e.type) {
        case EV_KEY:
            if (!e.pressed) continue;
            if (prompt_open) {
                int r = ui_edit_key(prompt_buf, sizeof prompt_buf, &e);
                if (r == 1) {
                    prompt_open = false;
                    if (prompt_kind == B_OPEN) load_file(prompt_buf);
                    else save_file(prompt_buf);
                    set_title();
                } else if (r == -1) prompt_open = false;
                break;
            }
            key(&e);
            ensure_cursor_visible();
            break;
        case EV_MOUSE_MOVE: {
            if (dragging && e.y >= TB) {
                cur = hit_pos(e.x, e.y);
                ensure_cursor_visible();
                break;
            }
            int h = -1;
            for (int i = 0; i < NBTN; i++)
                if (ui_hit(e.x, e.y, btn_x[i], 6, btn_w[i], 28)) h = i;
            if (h == hot_btn) continue;
            hot_btn = h;
            break;
        }
        case EV_MOUSE_DOWN:
            if (e.y >= TB && e.y < w->h - SB && (e.buttons & 1)) {
                prompt_open = false;
                cur = hit_pos(e.x, e.y);
                anc = (long)cur;
                dragging = true;
            }
            break;
        case EV_MOUSE_UP:
            if (dragging) {
                dragging = false;
                if (anc == (long)cur) anc = -1;
            }
            for (int i = 0; i < NBTN; i++)
                if (e.y < TB && ui_hit(e.x, e.y, btn_x[i], 6, btn_w[i], 28)) button(i);
            break;
        case EV_WHEEL:
            top = MAX(0, MIN(total_lines() - 1, top + e.wheel * 3));
            break;
        case EV_RESIZE: break;
        default: continue;
        }
        if (modified != was_modified) {
            set_title();
            was_modified = modified;
        }
        draw();
        win_update(w);
    }
    win_close(w);
    return 0;
}
