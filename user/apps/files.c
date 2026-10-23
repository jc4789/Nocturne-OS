/* File manager: the toolbar, path field, column header, status line and every row
   of the listing are widgets of the interface engine (libc/ui.c); the app keeps
   the folder, the selection and what opening a file means.
   Click selects, a second click on the same row (or Enter) opens it; arrows, Home,
   End, PageUp/PageDown move the selection, Backspace goes up, Delete removes the
   selected entry, F5 reads the folder again. The path field takes a folder to go to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "media.h"
#include "ui.h"

#define WINDOW 1
#define PAD 12
#define ROW_H 32       /* a row of the listing: 6 + 20 + 6 */
#define CHROME 150     /* window height that is not the listing: toolbar, card header, status line, padding */
#define COL_W 84       /* the Size and Type columns */
#define ROW_NODES 5    /* a row and its four children, counted in the view */
#define DOUBLE_MS 400  /* two clicks on a row this close are a double click */

static struct ui_session *sess;
#define PATH_BYTES 1024
static char cwd[PATH_BYTES];     /* the folder shown, a complete path (set by main) */
static bool in_programs;         /* it holds programs: the system's folder, or an app's folder in C:\Programs */
static char pathbuf[PATH_BYTES]; /* what the path field holds */
static struct n_entry *ents;     /* the folder's entries (n_folder_entries) */
static int nent, sel = -1, win_w = 620, win_h = 440;
static char status[128];
static uint64_t last_click;
static int last_click_idx = -1;
static int reveal = 1;           /* counts up when the keyboard moves the selection: the list shows it */

/* where the widgets of the last view are (events name them by place) */
enum { B_UP, B_HOME, B_ROOT, B_NEWDIR, B_NEWFILE, B_DELETE, NBTN };
static uint32_t btn_node[NBTN], first_row;

static int cmp(const void *a, const void *b) {
    const struct n_entry *x = a, *y = b;
    if ((x->kind == N_KIND_FOLDER) != (y->kind == N_KIND_FOLDER)) return x->kind == N_KIND_FOLDER ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* the complete path of a name in the shown folder; false (and a status) when it does not fit */
static bool entry_path(char *out, const char *name) {
    if (n_path_join(cwd, name, out, PATH_BYTES) >= 0) return true;
    snprintf(status, sizeof status, "the path of %s is too long", name);
    return false;
}

static void load(void) {
    nent = 0;
    sel = -1;
    free(ents);
    ents = NULL;
    strlcpy(pathbuf, cwd, sizeof pathbuf);
    in_programs = program_folder(cwd);
    n_result n = n_folder_entries(cwd, &ents);
    if (n < 0) {
        ents = NULL;
        snprintf(status, sizeof status, "cannot open %s: %s", cwd, n_result_text(n));
        return;
    }
    nent = (int)n;
    qsort(ents, nent, sizeof ents[0], cmp);
    snprintf(status, sizeof status, "%d item%s", nent, nent == 1 ? "" : "s");
    char t[PATH_BYTES + 16];
    snprintf(t, sizeof t, "Files - %s", cwd);
    ui_window_title(sess, WINDOW, t);
}

static void go(const char *path) {
    strlcpy(cwd, path, sizeof cwd);
    load();
}

static void go_up(void) {
    char up[PATH_BYTES];
    if (n_path_join(cwd, "..", up, sizeof up) >= 0) go(up); /* the root stays the root */
}

static const char *type_name(const struct n_entry *e) {
    if (e->kind == N_KIND_FOLDER) return "Folder";
    const char *dot = strrchr(e->name, '.');
    if (in_programs) return "Program";
    if (dot && !strcasecmp(dot, ".txt")) return "Text";
    if (dot && !strcasecmp(dot, ".ppm")) return "Picture";
    if (dot && !strcasecmp(dot, ".wav")) return "Sound";
    if (dot && (nmedia_audio_extension(e->name) || !strcasecmp(dot, ".mp4") ||
                !strcasecmp(dot, ".avi") || !strcasecmp(dot, ".mov"))) return "Media";
    if (dot && (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm"))) return "Web page";
    if (dot && !strcasecmp(dot, ".sh")) return "Script";
    return "File";
}

/* The icon of an entry and its colour. */
static const char *icon_of(const struct n_entry *e, uint32_t *colour) {
    const char *t = type_name(e);
    *colour = ui_theme.textDim;
    if (e->kind == N_KIND_FOLDER) { *colour = ui_theme.warning; return "folder"; }
    if (!strcmp(t, "Sound") || !strcmp(t, "Media")) { *colour = ui_theme.accent; return "sound"; }
    if (!strcmp(t, "Program")) { *colour = ui_theme.success; return "chevron-right"; }
    if (!strcmp(t, "Picture")) { *colour = ui_theme.accentHover; return "display"; }
    if (!strcmp(t, "Web page")) { *colour = ui_theme.accentHover; return "info"; }
    return "file";
}

static int rows_visible(void) { return MAX(1, (win_h - CHROME) / ROW_H); }

static void human(uint64_t n, char *out) {
    if (n < 1024) snprintf(out, 24, "%lu B", (unsigned long)n);
    else if (n < 1024 * 1024) snprintf(out, 24, "%.1f KiB", n / 1024.0);
    else snprintf(out, 24, "%.1f MiB", n / 1048576.0);
}

static void toolbar_button(struct ui_msg *m, int i, const char *text, const char *icon, bool disabled) {
    btn_node[i] = ui_node(m, "button");
    ui_str(m, "text", text);
    if (icon) ui_str(m, "icon", icon);
    ui_num(m, "compact", 1);
    if (disabled) ui_num(m, "disabled", 1);
    ui_end(m);
}

static void caption(struct ui_msg *m, const char *text, double width, bool end) {
    ui_node(m, "text");
    ui_str(m, "text", text);
    ui_str(m, "style", "caption");
    ui_num(m, "ellipsis", 1);
    if (width > 0) ui_num(m, "width", width);
    else ui_num(m, "grow", 1);
    if (end) ui_str(m, "textAlign", "end");
    ui_end(m);
}

static void show_row(struct ui_msg *m, int idx) {
    const struct n_entry *e = &ents[idx];
    uint32_t col;
    const char *icon = icon_of(e, &col);
    uint32_t pick = (ui_theme.accent & 0x00FFFFFFu) | 0x40000000u;
    char size[24] = "";
    if (e->kind != N_KIND_FOLDER) human(e->size, size);
    uint32_t row = ui_node(m, "row");
    if (idx == 0) first_row = row;
    ui_num(m, "clickable", 1);
    ui_nums(m, "padding", (double[]){6, 12}, 2);
    ui_num(m, "gap", 10);
    ui_num(m, "radius", ui_theme.controlRadius);
    if (idx == sel) {
        ui_num(m, "reveal", reveal);
        ui_num(m, "background", pick);
        ui_num(m, "hoverBackground", (ui_theme.accent & 0x00FFFFFFu) | 0x50000000u);
    }
        ui_node(m, "icon"); ui_str(m, "icon", icon); ui_num(m, "size", 18); ui_num(m, "colour", col); ui_end(m);
        ui_node(m, "text"); ui_str(m, "text", e->name); ui_num(m, "grow", 1); ui_num(m, "ellipsis", 1);
        ui_num(m, "weight", idx == sel ? 1 : 0); ui_end(m);
        ui_node(m, "text"); ui_str(m, "text", size); ui_num(m, "width", COL_W); ui_str(m, "textAlign", "end");
        ui_num(m, "colour", ui_theme.textDim); ui_end(m);
        ui_node(m, "text"); ui_str(m, "text", type_name(e)); ui_num(m, "width", COL_W);
        ui_num(m, "colour", ui_theme.textDim); ui_end(m);
    ui_end(m);
}

static void show(void) {
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column"); ui_num(&m, "padding", PAD); ui_num(&m, "gap", 10);
        ui_node(&m, "row"); ui_num(&m, "gap", 8);
            toolbar_button(&m, B_UP, "Up", NULL, false);
            toolbar_button(&m, B_HOME, "Home", NULL, false);
            toolbar_button(&m, B_ROOT, "C:\\", NULL, false);
            ui_node(&m, "field"); ui_str(&m, "value", pathbuf); ui_str(&m, "placeholder", "Folder");
            ui_str(&m, "width", "fill"); ui_num(&m, "minWidth", 80); ui_end(&m);
            toolbar_button(&m, B_NEWDIR, "New folder", "plus", false);
            toolbar_button(&m, B_NEWFILE, "New file", "plus", false);
            toolbar_button(&m, B_DELETE, "Delete", NULL, sel < 0);
        ui_end(&m);
        ui_node(&m, "card"); ui_num(&m, "padding", 0); ui_num(&m, "gap", 0); ui_str(&m, "height", "fill");
            ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){8, 26, 8, 20}, 4); ui_num(&m, "gap", 10);
                ui_node(&m, "spacer"); ui_num(&m, "size", 18); ui_end(&m);
                caption(&m, "Name", 0, false);
                caption(&m, "Size", COL_W, true);
                caption(&m, "Type", COL_W, false);
            ui_end(&m);
            ui_node(&m, "divider"); ui_end(&m);
            ui_node(&m, "scroll"); ui_str(&m, "height", "fill"); ui_str(&m, "key", cwd);
            ui_nums(&m, "padding", (double[]){4, 14, 4, 8}, 4); ui_num(&m, "gap", 2);
                for (int i = 0; i < nent; i++) show_row(&m, i);
                if (!nent) {
                    ui_node(&m, "text"); ui_str(&m, "text", "This folder is empty"); ui_str(&m, "style", "caption");
                    ui_nums(&m, "padding", (double[]){16, 12}, 2); ui_end(&m);
                }
            ui_end(&m);
        ui_end(&m);
        ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){0, 4}, 2); ui_num(&m, "gap", 12);
            caption(&m, status, 0, false);
            caption(&m, "Enter: open  Del: delete", 0, true);
        ui_end(&m);
    ui_end(&m);
    ui_send(sess, &m);
}

static void open_entry(int i) {
    if (i < 0 || i >= nent) return;
    char path[PATH_BYTES];
    if (!entry_path(path, ents[i].name)) return;
    if (ents[i].kind == N_KIND_FOLDER) {
        go(path);
        return;
    }
    const char *t = type_name(&ents[i]);
    int r;
    if (!strcmp(t, "Program")) r = gui_launch(path, NULL);
    else if (!strcmp(t, "Picture")) r = gui_launch("paint", path);
    else if (!strcmp(t, "Sound")) r = gui_launch("player", path);
    else if (!strcmp(t, "Media")) r = gui_launch("mediaplayer", path);
    else if (!strcmp(t, "Web page")) r = gui_launch("browser", path);
    else r = gui_launch("notepad", path);
    snprintf(status, sizeof status, r < 0 ? "could not open %s" : "opened %s", ents[i].name);
}

static void make_new(bool dir) {
    char path[PATH_BYTES], name[256];
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(name, sizeof name, dir ? "New folder" : "untitled.txt");
        else snprintf(name, sizeof name, dir ? "New folder %d" : "untitled%d.txt", n);
        if (!entry_path(path, name)) return;
        struct n_entry st;
        if (n_entry_info(path, &st) < 0) break;
    }
    n_result r;
    if (dir) r = n_folder_create(path);
    else {
        r = n_document_open(path, N_DOCUMENT_CREATE);
        if (r >= 0) r = n_handle_close(r);
    }
    load();
    if (r < 0) snprintf(status, sizeof status, "could not create %s: %s", name, n_result_text(r));
    for (int i = 0; i < nent; i++)
        if (!strcmp(ents[i].name, name)) sel = i;
}

static void delete_sel(void) {
    if (sel < 0) return;
    char path[PATH_BYTES], name[256];
    strlcpy(name, ents[sel].name, sizeof name);
    if (!entry_path(path, name)) return;
    n_result r = n_entry_remove(path);
    int keep = sel;
    load();
    sel = MIN(keep, nent - 1);
    if (r < 0) snprintf(status, sizeof status, "could not delete %s: %s", name, n_result_text(r));
    else snprintf(status, sizeof status, "deleted %s", name);
}

/* The path field was submitted: go there when it is a folder. */
static void go_typed(void) {
    char full[sizeof cwd];
    struct n_entry st;
    if (n_path_complete(pathbuf, full, sizeof full) < 0 || n_entry_info(full, &st) < 0 || st.kind != N_KIND_FOLDER) {
        snprintf(status, sizeof status, "%s is not a folder", pathbuf);
        strlcpy(pathbuf, cwd, sizeof pathbuf);
        return;
    }
    go(full);
}

static void click_row(int idx) {
    uint64_t now = n_clock_uptime_ms();
    if (idx == last_click_idx && now - last_click < DOUBLE_MS) {
        open_entry(idx);
        last_click_idx = -1;
    } else {
        sel = idx;
        last_click = now;
        last_click_idx = idx;
    }
}

/* A key no widget took. Returns whether the view changed. */
static bool key(const struct ui_event *e) {
    const char *k = e->text;
    int vis = rows_visible();
    if (!strcmp(k, "ArrowDown")) sel = MIN(nent - 1, sel + 1);
    else if (!strcmp(k, "ArrowUp")) sel = MAX(0, sel - 1);
    else if (!strcmp(k, "Home")) sel = 0;
    else if (!strcmp(k, "End")) sel = nent - 1;
    else if (!strcmp(k, "PageDown")) sel = MIN(nent - 1, sel + vis);
    else if (!strcmp(k, "PageUp")) sel = MAX(0, sel - vis);
    else if (!strcmp(k, "Enter")) open_entry(sel);
    else if (!strcmp(k, "Backspace")) go_up();
    else if (!strcmp(k, "Delete")) delete_sel();
    else if (!strcmp(k, "F5")) load();
    else return false;
    reveal++;
    return true;
}

int main(int argc, char **argv) {
    sess = ui_session_new();
    if (!sess || ui_window(sess, WINDOW, win_w, win_h, WIN_RESIZABLE, "Files", &win_w, &win_h) < 0) return 1;
    char start[PATH_BYTES]; /* the named folder, else the one Files was started in */
    if (n_path_complete(argc > 1 ? argv[1] : ".", start, sizeof start) < 0) strlcpy(start, n_user_folder(), sizeof start);
    go(start);
    show();
    for (bool running = true; running;) {
        ui_session_wait(sess, -1);
        bool changed = false;
        struct ui_event e;
        char text[1024];
        while (ui_next_event(sess, &e, text, sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLICK:
                if (e.node == btn_node[B_UP]) go_up();
                else if (e.node == btn_node[B_HOME]) go(n_user_folder());
                else if (e.node == btn_node[B_ROOT]) go("C:\\");
                else if (e.node == btn_node[B_NEWDIR]) make_new(true);
                else if (e.node == btn_node[B_NEWFILE]) make_new(false);
                else if (e.node == btn_node[B_DELETE]) delete_sel();
                else if (nent > 0 && e.node >= first_row && (e.node - first_row) % ROW_NODES == 0 &&
                         (int)((e.node - first_row) / ROW_NODES) < nent)
                    click_row((int)((e.node - first_row) / ROW_NODES));
                else break;
                changed = true;
                break;
            case UI_EVENT_TEXT:
                strlcpy(pathbuf, e.text, sizeof pathbuf);
                changed = true;
                break;
            case UI_EVENT_SUBMIT:
                go_typed();
                changed = true;
                break;
            case UI_EVENT_KEY:
                if (!(e.flags & UI_UP) && key(&e)) changed = true;
                break;
            case UI_EVENT_RESIZE:
                win_w = (int)e.a;
                win_h = (int)e.b;
                changed = true;
                break;
            case UI_EVENT_CLOSE:
                running = false;
                break;
            }
        }
        if (!running || !ui_session_open(sess)) break;
        if (changed) show();
    }
    ui_session_free(sess);
    free(ents);
    return 0;
}
