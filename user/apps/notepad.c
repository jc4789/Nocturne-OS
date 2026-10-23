/* Text editor: the toolbar (New, Open, Save, Save as, the file name or the name
   prompt) and the status line are widgets of the interface engine (libc/ui.c); the
   text area is a surface the editor draws itself - line numbers, the selection, the
   caret - and a pointer surface, so the mouse and the wheel come to the app.
   Text is UTF-8 in Noto Sans (CJK included); Tab is four spaces when typed and a
   tab stop when read. Ctrl+S saves, Ctrl+O opens, Ctrl+N starts a new text,
   Ctrl+A/C/X/V select all and use the clipboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "nocturne.h"
#include "ui.h"

#define WINDOW 1
#define EDITOR 1
#define PAD 12
#define TAB_COLS 4 /* a tab stop is this many spaces wide */

static struct ui_session *sess;
static canvas_t *ed;     /* the surface the text area is drawn in */
static int win_w = 640, win_h = 460;

static char *text;
static size_t len, cap, cur;
static long anc = -1; /* selection anchor, -1 = none */
static int top;       /* the first line shown */
static float left;    /* how far the text is scrolled sideways, in pixels */
static bool modified, prompt_open, dragging;
static char path[1024], prompt_buf[1024], status[128];
static float want_x = -1; /* where Up/Down keep the caret sideways, -1 = not set */
static int cur_line, total;

enum { B_NEW, B_OPEN, B_SAVE, B_SAVEAS, NBTN };
static const char *btn_label[NBTN] = {"New", "Open", "Save", "Save as"};
static uint32_t btn_node[NBTN]; /* where the buttons are in the last view */
static int prompt_kind;         /* B_OPEN or B_SAVEAS */
static int prompt_focus;        /* the field's "focus": counts up each time the prompt opens */
static uint32_t prompt_field;   /* its place in the last view */
/* Closing with unsaved changes (the close button, or the system shutting down) asks first. */
enum { A_SAVE, A_DISCARD, A_CANCEL, NASK };
static const char *ask_label[NASK] = {"Save", "Don't save", "Cancel"};
static uint32_t ask_node[NASK];
static bool closing;

/* the look of the text area */
static float fs, small_size, space_w;
static int lh;

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

/* one character (a UTF-8 sequence) at a time */
static size_t cp_next(size_t p) {
    if (p >= len) return len;
    p++;
    while (p < len && ((uint8_t)text[p] & 0xC0) == 0x80) p++;
    return p;
}
static size_t cp_prev(size_t p) {
    if (p == 0) return 0;
    p--;
    while (p > 0 && ((uint8_t)text[p] & 0xC0) == 0x80) p--;
    return p;
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
/* where line number `line` starts (the end of the text when it is beyond it) */
static size_t line_pos(int line) {
    size_t p = 0;
    for (int l = 0; l < line && p < len; p++)
        if (text[p] == '\n') l++;
    return p;
}

/* ---- the geometry of the text area ---- */
static float tab_stop(float x) {
    float w = space_w * TAB_COLS;
    return (floorf(x / w) + 1) * w;
}

/* How far from the start of the line (ls) the character boundary p is. */
static float line_x(size_t ls, size_t p) {
    float x = 0;
    size_t run = ls;
    for (size_t i = ls; i < p; i++) {
        if (text[i] != '\t') continue;
        x += ui_text_width(text + run, i - run, fs, UI_TEXT_MONO);
        x = tab_stop(x);
        run = i + 1;
    }
    return x + (p > run ? ui_text_width(text + run, p - run, fs, UI_TEXT_MONO) : 0);
}

/* The character boundary of the line starting at ls that is nearest to x. */
static size_t pos_at_x(size_t ls, float target) {
    float x = 0;
    size_t i = ls, le = line_end(ls);
    while (i < le) {
        size_t nx = cp_next(i);
        float cw = text[i] == '\t' ? tab_stop(x) - x : ui_text_width(text + i, nx - i, fs, UI_TEXT_MONO);
        if (target < x + cw / 2) return i;
        x += cw;
        i = nx;
    }
    return le;
}

static int gutter(int lines) {
    int digits = 1;
    for (int n = lines; n >= 10; n /= 10) digits++;
    return (int)ceilf(ui_text_width("0", 1, small_size, UI_TEXT_MONO) * (float)MAX(digits, 3)) + 26;
}
static float text_x(void) { return (float)gutter(total_lines()) + 10; }
static int text_rows(void) { return ed ? MAX(1, (ed->h - 8) / lh) : 1; }
static float text_w(void) { return ed ? (float)ed->w - text_x() - 8 : 0; }

static void ensure_cursor_visible(void) {
    int rows = text_rows();
    cur_line = line_of(cur);
    if (cur_line < top) top = cur_line;
    if (cur_line >= top + rows) top = cur_line - rows + 1;
    float cx = line_x(line_start(cur), cur), tw = text_w();
    if (cx < tw - 12) left = 0;
    else if (cx - left > tw - 12) left = cx - tw + 12;
    else if (cx < left) left = cx;
}

static void set_title(void) {
    static char last[300];
    char t[300];
    const char *name = path[0] ? n_path_name(path) : "untitled";
    snprintf(t, sizeof t, "%s%s - Text Editor", modified ? "*" : "", name);
    if (!strcmp(t, last)) return;
    strlcpy(last, t, sizeof last);
    ui_window_title(sess, WINDOW, t);
}

static bool load_file(const char *name) {
    char p[sizeof path];
    n_result r = n_path_complete(name, p, sizeof p);
    if (r < 0) {
        snprintf(status, sizeof status, "cannot open %s: %s", name, n_result_text(r));
        return false;
    }
    void *data;
    size_t size;
    r = n_document_load(p, &data, &size);
    if (r < 0) {
        snprintf(status, sizeof status, "cannot open %s: %s", p, n_result_text(r));
        return false;
    }
    cur = 0;
    reserve(size + 1);
    memcpy(text, data, size);
    len = size;
    n_memory_release(data, size);
    strlcpy(path, p, sizeof path);
    modified = false;
    anc = -1;
    top = 0;
    left = 0;
    want_x = -1;
    snprintf(status, sizeof status, "opened %s (%lu bytes)", p, (unsigned long)len);
    return true;
}

static bool save_file(const char *name) {
    char p[sizeof path];
    n_result r = n_path_complete(name, p, sizeof p);
    if (r < 0) {
        snprintf(status, sizeof status, "cannot write %s: %s", name, n_result_text(r));
        return false;
    }
    r = n_document_save(p, text, len); /* whole: a failed save leaves the old text */
    if (r < 0) {
        snprintf(status, sizeof status, "cannot write %s: %s", p, n_result_text(r));
        return false;
    }
    strlcpy(path, p, sizeof path);
    modified = false;
    snprintf(status, sizeof status, "saved %s (%lu bytes)", p, (unsigned long)len);
    return true;
}

static void open_prompt(int kind) {
    prompt_open = true;
    prompt_kind = kind;
    prompt_focus++;
    if (path[0]) strlcpy(prompt_buf, path, sizeof prompt_buf);
    else if (n_path_join(n_user_folder(), "Documents", prompt_buf, sizeof prompt_buf) >= 0) strlcat(prompt_buf, "\\", sizeof prompt_buf);
}

static void new_text(void) {
    len = cur = 0;
    path[0] = 0;
    modified = false;
    anc = -1;
    top = 0;
    left = 0;
    want_x = -1;
}

static void save(void) {
    if (path[0]) save_file(path);
    else open_prompt(B_SAVEAS);
}

/* ---- drawing the text area ---- */
/* The text of one line (ls..le) from x origin ox, only what shows between vis_l and vis_r. */
static void draw_line(canvas_t *c, size_t ls, size_t le, float ox, float y, float vis_l, float vis_r) {
    float x = 0, runx = 0;
    size_t run = ls, i = ls;
    while (i < le) {
        size_t nx = cp_next(i);
        if (text[i] == '\t') {
            if (i > run) ui_text(c, ox + runx, y, text + run, i - run, fs, UI_TEXT_MONO, ui_theme.text);
            x = tab_stop(x);
            run = nx;
            runx = x;
            i = nx;
            continue;
        }
        float cw = ui_text_width(text + i, nx - i, fs, UI_TEXT_MONO);
        if (ox + x + cw < vis_l) { /* still left of the window: not drawn */
            x += cw;
            run = nx;
            runx = x;
        } else if (ox + x > vis_r) break;
        else x += cw;
        i = nx;
    }
    if (i > run) ui_text(c, ox + runx, y, text + run, i - run, fs, UI_TEXT_MONO, ui_theme.text);
}

static void draw_editor(void) {
    if (!ed) return;
    const struct ui_theme *t = &ui_theme;
    int sw = ed->w, sh = ed->h;
    total = total_lines();
    cur_line = line_of(cur);
    int gut = gutter(total);
    float tx = (float)gut + 10;
    gfx_noclip(ed);
    gfx_fill(ed, 0, 0, sw, sh, t->background);
    ui_round_fill(ed, 0, 0, (float)sw, (float)sh, t->radius, t->surface);
    ui_round_stroke(ed, 0, 0, (float)sw, (float)sh, t->radius, 1, t->border);
    ui_line(ed, (float)gut, 10, (float)gut, (float)sh - 10, 1, t->divider);
    size_t sa = 0, sb = 0;
    bool has_sel = sel_range(&sa, &sb);
    canvas_t clip = *ed;
    gfx_clip(&clip, gut + 1, 2, sw - gut - 4, sh - 4);
    size_t p = line_pos(top);
    float ty_off = (float)(lh - (int)ui_line_height(fs, UI_TEXT_MONO, NULL)) / 2;
    uint32_t pick = (t->accent & 0x00FFFFFFu) | 0x55000000u;
    for (int r = 0, rows = text_rows(); r < rows && top + r < total; r++) {
        int ln = top + r;
        float y = 4 + (float)(r * lh);
        size_t ls = p, le = line_end(p);
        if (ln == cur_line) ui_round_fill(ed, 4, y, (float)sw - 8, (float)lh, 6, t->surfaceHover);
        char num[12];
        int nn = snprintf(num, sizeof num, "%d", ln + 1);
        float nw = ui_text_width(num, (size_t)nn, small_size, UI_TEXT_MONO);
        ui_text(ed, (float)gut - 12 - nw, y + (float)(lh - (int)ui_line_height(small_size, UI_TEXT_MONO, NULL)) / 2, num,
                (size_t)nn, small_size, UI_TEXT_MONO, ln == cur_line ? t->accent : t->textFaint);
        if (has_sel && sb > ls && sa <= le) {
            float x0 = line_x(ls, MAX(sa, ls)), x1 = line_x(ls, MIN(sb, le));
            if (sb > le) x1 += space_w; /* the line break is selected too */
            ui_round_fill(&clip, tx + x0 - left, y + 1, x1 - x0, (float)lh - 2, 3, pick);
        }
        draw_line(&clip, ls, le, tx - left, y + ty_off, (float)gut + 1, (float)sw);
        if (cur >= ls && cur <= le && !prompt_open && ln == cur_line)
            ui_round_fill(&clip, tx + line_x(ls, cur) - left - 1, y + 3, 2, (float)lh - 6, 1, t->accent);
        p = le + 1;
    }
    ui_surface_changed(sess, EDITOR);
}

/* ---- the toolbar and status line ---- */
static void right_status(char *out, size_t n) {
    size_t ls = line_start(cur);
    int col = 1;
    for (size_t i = ls; i < cur; i = cp_next(i)) col++;
    snprintf(out, n, "Ln %d, Col %d  |  %d lines%s", cur_line + 1, col, total, modified ? "  |  modified" : "");
}

static void show(void) {
    char right[96];
    right_status(right, sizeof right);
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", 10);
        ui_node(&m, "row"); ui_num(&m, "gap", 8); ui_num(&m, "height", 36);
            for (int i = 0; i < NBTN; i++) {
                btn_node[i] = ui_node(&m, "button");
                ui_str(&m, "text", btn_label[i]);
                ui_num(&m, "compact", 1);
                ui_end(&m);
            }
            if (closing && !prompt_open) {
                ui_node(&m, "text"); ui_str(&m, "text", "Save changes?");
                ui_num(&m, "colour", ui_theme.accent); ui_num(&m, "weight", 1); ui_num(&m, "ellipsis", 1);
                ui_num(&m, "grow", 1); ui_nums(&m, "padding", (double[]){0, 8}, 2); ui_end(&m);
                for (int i = 0; i < NASK; i++) {
                    ask_node[i] = ui_node(&m, "button");
                    ui_str(&m, "text", ask_label[i]);
                    ui_num(&m, "compact", 1);
                    ui_end(&m);
                }
            } else if (prompt_open) {
                ui_node(&m, "text"); ui_str(&m, "text", prompt_kind == B_OPEN ? "Open:" : "Save as:");
                ui_num(&m, "colour", ui_theme.accent); ui_num(&m, "weight", 1); ui_nums(&m, "padding", (double[]){0, 4}, 2);
                ui_end(&m);
                prompt_field = ui_node(&m, "field"); ui_str(&m, "value", prompt_buf); ui_num(&m, "focus", prompt_focus);
                ui_str(&m, "placeholder", "Documents\\file.txt");
                ui_str(&m, "width", "fill"); ui_num(&m, "minWidth", 80); ui_end(&m);
            } else {
                ui_node(&m, "text"); ui_str(&m, "text", path[0] ? path : "(not saved yet)"); ui_num(&m, "ellipsis", 1);
                ui_num(&m, "colour", ui_theme.textDim);
                ui_nums(&m, "padding", (double[]){0, 8}, 2); ui_end(&m);
            }
        ui_end(&m);
        ui_node(&m, "surface"); ui_num(&m, "surface", EDITOR); ui_num(&m, "pointer", 1);
        ui_str(&m, "width", "fill"); ui_str(&m, "height", "fill"); ui_end(&m);
        ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){0, 4}, 2); ui_num(&m, "gap", 12);
            ui_node(&m, "text"); ui_str(&m, "text", status); ui_str(&m, "style", "caption"); ui_num(&m, "grow", 1);
            ui_num(&m, "ellipsis", 1); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", right); ui_str(&m, "style", "caption"); ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    ui_send(sess, &m);
}

/* ---- input ---- */
static size_t hit_pos(float mx, float my) {
    int line = top + (int)floorf((my - 4) / (float)lh);
    if (line < 0) line = 0;
    if (line > total_lines() - 1) return len;
    return pos_at_x(line_pos(line), mx - text_x() + left);
}

static void copy_sel(void) {
    size_t a, b;
    if (sel_range(&a, &b)) clipboard_set(text + a, b - a);
    else {
        size_t s = line_start(cur), e = line_end(cur);
        clipboard_set(text + s, e - s);
    }
}

static void paste(void) {
    char *buf = malloc(65536);
    if (!buf) return;
    int n = clipboard_get(buf, 65536);
    if (n > 0) {
        delete_sel();
        insert(buf, (size_t)n);
    }
    free(buf);
}

/* Whether the typed key is a character to insert (not a named key such as an arrow). */
static bool printable(const struct ui_event *e) {
    int k = (int)e->a;
    return e->text_n > 0 && ((k >= 32 && k < 0x100 && k != 127) || k > 0x1FF);
}

/* A key for the text. */
static void key(const struct ui_event *e) {
    bool ctrl = e->mods & NMOD_CTRL, shift = e->mods & NMOD_SHIFT;
    int k = (int)e->a;
    if (ctrl && k < 128) k = tolower(k);
    if (ctrl) {
        if (k == 's') save();
        else if (k == 'o') open_prompt(B_OPEN);
        else if (k == 'n') new_text();
        else if (k == 'a') {
            anc = 0;
            cur = len;
        } else if (k == 'c') copy_sel();
        else if (k == 'x') {
            copy_sel();
            if (!delete_sel()) {
                size_t s = line_start(cur), en = line_end(cur);
                erase(s, en < len ? en + 1 : en);
            }
        } else if (k == 'v') paste();
        else if (k == NKEY_HOME) cur = 0, anc = -1;
        else if (k == NKEY_END) cur = len, anc = -1;
        return;
    }
    bool move = k == NKEY_LEFT || k == NKEY_RIGHT || k == NKEY_UP || k == NKEY_DOWN || k == NKEY_HOME ||
                k == NKEY_END || k == NKEY_PGUP || k == NKEY_PGDN;
    if (move) {
        if (shift && anc < 0) anc = (long)cur;
        if (!shift) anc = -1;
        bool vertical = k == NKEY_UP || k == NKEY_DOWN || k == NKEY_PGUP || k == NKEY_PGDN;
        size_t ls = line_start(cur);
        if (!vertical) want_x = -1;
        else if (want_x < 0) want_x = line_x(ls, cur);
        switch (k) {
        case NKEY_LEFT: cur = cp_prev(cur); break;
        case NKEY_RIGHT: cur = cp_next(cur); break;
        case NKEY_UP: cur = ls ? pos_at_x(line_start(ls - 1), want_x) : 0; break;
        case NKEY_DOWN: {
            size_t le = line_end(cur);
            cur = le < len ? pos_at_x(le + 1, want_x) : len;
            break;
        }
        case NKEY_PGUP:
            for (int i = 0; i < text_rows() && ls > 0; i++) ls = line_start(ls - 1);
            cur = pos_at_x(ls, want_x);
            break;
        case NKEY_PGDN:
            for (int i = 0; i < text_rows(); i++) {
                size_t le = line_end(ls);
                if (le >= len) break;
                ls = le + 1;
            }
            cur = pos_at_x(ls, want_x);
            break;
        case NKEY_HOME: cur = ls; break;
        case NKEY_END: cur = line_end(cur); break;
        }
        return;
    }
    want_x = -1;
    if (k == NKEY_BACKSPACE) {
        if (!delete_sel() && cur) erase(cp_prev(cur), cur);
    } else if (k == NKEY_DELETE) {
        if (!delete_sel() && cur < len) erase(cur, cp_next(cur));
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
    } else if (printable(e)) {
        delete_sel();
        insert(e->text, e->text_n);
    }
}

int main(int argc, char **argv) {
    sess = ui_session_new();
    if (!sess || ui_window(sess, WINDOW, win_w, win_h, WIN_RESIZABLE, "Text Editor", &win_w, &win_h) < 0) return 1;
    fs = ui_theme.fontSize;
    small_size = ui_theme.smallSize;
    space_w = ui_text_width(" ", 1, fs, UI_TEXT_MONO);
    lh = (int)ui_line_height(fs, UI_TEXT_MONO, NULL) + 6;
    reserve(4096);
    if (argc > 1) {
        if (!load_file(argv[1])) {
            if (n_path_complete(argv[1], path, sizeof path) < 0) path[0] = 0;
            snprintf(status, sizeof status, "new file %s", argv[1]);
        }
    } else snprintf(status, sizeof status, "Ctrl+S save, Ctrl+O open");
    /* the area the layout gives the surface; a CANVAS event corrects it */
    ed = ui_surface(sess, EDITOR, MAX(64, win_w - 2 * PAD), MAX(64, win_h - 2 * PAD - 36 - 22 - 20), 0);
    if (!ed) return 1;
    set_title();
    draw_editor();
    char shown[2400] = "";
    show();
    for (bool running = true; running;) {
        ui_session_wait(sess, -1);
        bool redraw = false, follow = false;
        struct ui_event e;
        char etext[512];
        while (ui_next_event(sess, &e, etext, sizeof etext)) {
            switch (e.type) {
            case UI_EVENT_KEY:
                if (e.flags & UI_UP) break;
                if (prompt_open) break; /* the name field has the keyboard; other keys wait for it to close */
                key(&e);
                redraw = follow = true;
                break;
            case UI_EVENT_FOCUS: /* the prompt lasts while its field has the keyboard (Escape or a click away ends it) */
                if (prompt_open && e.node == prompt_field && !e.a) {
                    prompt_open = false;
                    redraw = true;
                }
                break;
            case UI_EVENT_TEXT: /* the name field */
                strlcpy(prompt_buf, e.text, sizeof prompt_buf);
                break;
            case UI_EVENT_SUBMIT:
                strlcpy(prompt_buf, e.text, sizeof prompt_buf);
                prompt_open = false;
                if (prompt_kind == B_OPEN) load_file(prompt_buf);
                else if (save_file(prompt_buf) && closing) running = false;
                redraw = follow = true;
                break;
            case UI_EVENT_CLICK:
                if (closing && e.node == ask_node[A_SAVE]) {
                    if (!path[0]) open_prompt(B_SAVEAS);
                    else if (save_file(path)) running = false;
                } else if (closing && e.node == ask_node[A_DISCARD]) {
                    running = false;
                } else if (closing && e.node == ask_node[A_CANCEL]) {
                    closing = false;
                } else if (e.node == btn_node[B_NEW]) new_text();
                else if (e.node == btn_node[B_OPEN]) open_prompt(B_OPEN);
                else if (e.node == btn_node[B_SAVE]) save();
                else if (e.node == btn_node[B_SAVEAS]) open_prompt(B_SAVEAS);
                redraw = true;
                break;
            case UI_EVENT_POINTER:
                if (e.flags & UI_DOWN) {
                    if (!(UI_BUTTONS(e.flags) & 1)) break;
                    prompt_open = false;
                    cur = hit_pos((float)e.a, (float)e.b);
                    anc = (long)cur;
                    want_x = -1;
                    dragging = true;
                    redraw = follow = true;
                } else if (e.flags & UI_UP) {
                    if (dragging) {
                        dragging = false;
                        if (anc == (long)cur) anc = -1;
                        redraw = true;
                    }
                } else if (dragging) {
                    cur = hit_pos((float)e.a, (float)e.b);
                    want_x = -1;
                    redraw = follow = true;
                }
                break;
            case UI_EVENT_WHEEL:
                top = MAX(0, MIN(total_lines() - 1, top + (int)e.a * 3));
                redraw = true;
                break;
            case UI_EVENT_CANVAS:
                ed = ui_surface(sess, EDITOR, (int)e.a, (int)e.b, 0);
                redraw = true;
                break;
            case UI_EVENT_RESIZE:
                win_w = (int)e.a;
                win_h = (int)e.b;
                break;
            case UI_EVENT_CLOSE:
                if (!modified) running = false;
                closing = true;
                break;
            }
        }
        if (!running || !ui_session_open(sess)) break;
        if (follow) ensure_cursor_visible();
        if (redraw) draw_editor();
        /* a new view only when something it shows has changed */
        char now[2400];
        char right[96];
        right_status(right, sizeof right);
        snprintf(now, sizeof now, "%d|%d|%d|%d|%d|%d|%s|%s|%s|%s", win_w, win_h, closing, prompt_open, prompt_kind,
                 prompt_focus, prompt_buf, path, status, right);
        if (strcmp(now, shown)) {
            strlcpy(shown, now, sizeof shown);
            show();
        }
        set_title();
    }
    ui_session_free(sess);
    free(text);
    return 0;
}
