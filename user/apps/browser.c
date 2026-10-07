/* Nocturne Web: native DOM, layout and painting, with a document-owned JavaScript runtime.
   HTTP requests run in isolated Nocturne child processes, never on the GUI/JS task. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include "nocturne.h"
#include "http.h"
#include "web.h"
#include "webnet.h"
#include "webstorage.h"

#define TB 40   /* toolbar */
#define SB 24   /* status / find bar */
#define SCRW 10 /* scrollbar */
#define MAXHIST 64
#define LINE 48
#define HOME "about:home"
#define SEARCH "https://lite.duckduckgo.com/lite/?q="

static window_t *w;
static web_doc *doc;
static int scroll_y, doc_h;
static char cur_url[2048], status[256], hover[512];
static bool quit, loading;
static bool debug_js;
static webnet *network;
static webstorage *storage; /* browser-window lifetime, not document lifetime */
static uint64_t generation = 1, next_generation = 1, navigation_generation, navigation_id;
static int navigation_mode;
static char navigation_url[2048];
static bool native_wait, pending_navigation;
static char pending_url[2048];
static char *pending_post;
static int pending_mode;
static bool navigation_ready, sync_stopped;

struct transfer {
    struct transfer *next;
    uint64_t network_id, resource_id, generation;
};
static struct transfer *transfers;
static struct web_response navigation_response;

#define CONSOLE_LINES 128
#define CONSOLE_LINE 768
static char console_lines[CONSOLE_LINES][CONSOLE_LINE];
static int console_n, console_next;
static bool console_open, console_dirty;

static struct {
    char *url;
    int y;
    uint64_t document, entry;
    void *state;
    size_t state_len;
    bool manual;
} hist[MAXHIST];
static int nhist, hpos = -1;
static uint64_t history_serial;
static struct { int delta; uint64_t generation; } history_queue[64];
static int history_queue_head, history_queue_count;

enum { F_PAGE, F_ADDR, F_FIND };
static int focus = F_PAGE;
static char addr[2048];
static int acur;
static bool addr_all;

static bool find_open;
static char find_buf[128];
static int find_y = -1;

static web_node *sel_node;
static web_node *pressed_node, *hover_node;
static char *focus_value;
static const char *sel_labels[128];
static int sel_n, sel_cur, sel_top, sel_x, sel_y, sel_w;
#define SEL_H 20
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool need_layout, need_paint;
static char refresh_url[2048];
static uint64_t refresh_at;
static bool refresh_seen, initial_scroll_pending, initial_scroll_anchor;
static int initial_scroll_y;

static int hot_btn = -1;
static bool drag_sb;
static int drag_off;

/* events that arrived while an image was downloading and could not be handled there */
static struct gui_event evq[64];
static int evq_n;
static bool scroll_event_pending;

static int page_w(void) { return MAX(w->w - SCRW, 50); }
static int console_h(void) { return console_open ? MIN(220, MAX(80, (w->h - TB - SB) / 3)) : 0; }
static int page_h(void) { return MAX(w->h - TB - SB - console_h(), 20); }
static int max_scroll(void) { return MAX(doc_h - page_h(), 0); }
static void clamp_scroll(void) {
    int y = MAX(0, MIN(scroll_y, max_scroll()));
    if (y != scroll_y) scroll_event_pending = true;
    scroll_y = y;
}

/* ---------------------------------------------------------------- strings */
struct sb {
    char *b;
    size_t n, cap;
};
static void sb_add(struct sb *s, const char *p, size_t n) {
    if (s->n + n + 1 > s->cap) {
        while (s->n + n + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 1024;
        s->b = realloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, p, n);
    s->n += n;
    s->b[s->n] = 0;
}
static void sb_str(struct sb *s, const char *p) { sb_add(s, p, strlen(p)); }
static void sb_esc(struct sb *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char *r = p[i] == '<' ? "&lt;" : p[i] == '>' ? "&gt;" : p[i] == '&' ? "&amp;" : p[i] == '"' ? "&quot;" : NULL;
        if (r) sb_str(s, r);
        else sb_add(s, p + i, 1);
    }
}

static bool has_prefix(const char *s, const char *p) { return !strncasecmp(s, p, strlen(p)); }

/* ---------------------------------------------------------------- drawing */
static void field(canvas_t *c, int x, int y, int fw, int h, const char *text, bool foc, int caret, bool all) {
    gfx_fill_round(c, x, y, fw, h, 5, foc ? UI_ACCENT : RGB(70, 66, 110));
    gfx_fill_round(c, x + 1, y + 1, fw - 2, h - 2, 4, RGB(18, 16, 32));
    canvas_t cl = *c;
    gfx_clip(&cl, x + 4, y, fw - 8, h);
    int ty = y + (h - 16) / 2;
    char pre[2048];
    snprintf(pre, sizeof pre, "%.*s", caret, text);
    int cpx = gfx_text_width(pre, FONT_SMALL);
    int shift = foc && cpx > fw - 20 ? cpx - (fw - 20) : 0;
    int tx = x + 7 - shift;
    if (foc && all && *text) gfx_fill(&cl, tx - 1, ty, gfx_text_width(text, FONT_SMALL) + 2, 16, RGB(70, 60, 140));
    gfx_text(&cl, tx, ty, text, foc ? UI_FG : RGB(200, 200, 225), TRANSPARENT, FONT_SMALL);
    if (foc && !all) gfx_fill(&cl, tx + cpx, ty, 2, 16, UI_ACCENT2);
}

enum { BTN_BACK, BTN_FWD, BTN_RELOAD, BTN_HOME, NBTN };
static int btn_x(int i) { return 6 + i * 32; }
#define ADDR_X (btn_x(NBTN) + 6)

static void draw_toolbar(void) {
    canvas_t *c = &w->c;
    int W = w->w;
    gfx_gradient_v(c, 0, 0, W, TB, RGB(46, 42, 80), RGB(36, 32, 64));
    for (int i = 0; i < NBTN; i++) {
        int x = btn_x(i), y = 6;
        bool on = i == BTN_BACK ? hpos > 0 : i == BTN_FWD ? hpos < nhist - 1 : true;
        ui_button(c, x, y, 28, 28, "", on && hot_btn == i, false);
        uint32_t col = on ? UI_FG : RGB(100, 98, 140);
        int cx = x + 14, cy = y + 14;
        if (i == BTN_BACK) gfx_triangle(c, cx - 6, cy, cx + 4, cy - 6, cx + 4, cy + 6, col);
        else if (i == BTN_FWD) gfx_triangle(c, cx + 6, cy, cx - 4, cy - 6, cx - 4, cy + 6, col);
        else if (i == BTN_RELOAD) {
            if (loading) {
                gfx_line(c, cx - 5, cy - 5, cx + 5, cy + 5, col);
                gfx_line(c, cx - 5, cy + 5, cx + 5, cy - 5, col);
            } else {
                gfx_circle(c, cx, cy, 7, col);
                gfx_circle(c, cx, cy, 6, col);
                gfx_fill(c, cx + 1, cy - 8, 8, 8, UI_BTN);
                gfx_triangle(c, cx + 1, cy - 11, cx + 1, cy - 1, cx + 7, cy - 6, col);
            }
        } else {
            gfx_triangle(c, cx - 8, cy - 1, cx, cy - 8, cx + 8, cy - 1, col);
            gfx_fill(c, cx - 5, cy - 1, 10, 8, col);
            gfx_fill(c, cx - 1, cy + 3, 3, 4, UI_BTN);
        }
    }
    field(c, ADDR_X, 8, W - ADDR_X - 8, 24, focus == F_ADDR ? addr : cur_url, focus == F_ADDR,
          focus == F_ADDR ? acur : 0, addr_all);
}

static void draw_status(void) {
    canvas_t *c = &w->c;
    int W = w->w, y = w->h - SB;
    gfx_fill(c, 0, y, W, SB, RGB(28, 26, 46));
    gfx_hline(c, 0, y, W, RGB(60, 56, 100));
    if (find_open) {
        gfx_text(c, 8, y + 4, "Find:", UI_ACCENT2, TRANSPARENT, FONT_SMALL);
        field(c, 56, y + 2, 240, 20, find_buf, focus == F_FIND, (int)strlen(find_buf), false);
        const char *msg = !find_buf[0] ? "Enter: next   Esc: close" : find_y < 0 ? "not found" : "Enter: next match";
        gfx_text(c, 306, y + 4, msg, find_y < 0 && find_buf[0] ? RGB(240, 120, 120) : UI_DIM, TRANSPARENT, FONT_SMALL);
        return;
    }
    const char *s = hover[0] ? hover : status;
    canvas_t cl = *c;
    gfx_clip(&cl, 0, y, W - 8, SB);
    gfx_text(&cl, 8, y + 4, s, hover[0] ? RGB(160, 200, 255) : UI_DIM, TRANSPARENT, FONT_SMALL);
}

static void draw_page(void) {
    canvas_t *c = &w->c;
    int pw = page_w(), ph = page_h();
    if (doc) web_paint(doc, c, 0, TB, pw, ph, 0, scroll_y);
    else gfx_fill(c, 0, TB, pw, ph, RGB(255, 255, 255));
    /* scrollbar */
    int x = w->w - SCRW;
    gfx_fill(c, x, TB, SCRW, ph, RGB(36, 34, 60));
    if (doc_h > ph) {
        int th = MAX(24, (int)((long)ph * ph / doc_h));
        int ty = TB + (int)((long)scroll_y * (ph - th) / max_scroll());
        gfx_fill_round(c, x + 2, ty + 1, SCRW - 4, th - 2, 3, drag_sb ? UI_ACCENT : RGB(100, 96, 150));
    }
    /* <select> popup */
    if (sel_node) {
        int vis = MIN(sel_n - sel_top, (w->h - SB - sel_y) / SEL_H);
        gfx_fill(c, sel_x - 1, sel_y - 1, sel_w + 2, vis * SEL_H + 2, RGB(90, 90, 120));
        for (int i = 0; i < vis; i++) {
            int k = sel_top + i, y = sel_y + i * SEL_H;
            bool on = k == sel_cur;
            gfx_fill(c, sel_x, y, sel_w, SEL_H, on ? RGB(60, 110, 220) : RGB(255, 255, 255));
            canvas_t cl = *c;
            gfx_clip(&cl, sel_x, y, sel_w - 4, SEL_H);
            gfx_text(&cl, sel_x + 6, y + 2, sel_labels[k], on ? RGB(255, 255, 255) : RGB(20, 20, 30), TRANSPARENT, FONT_SMALL);
        }
    }
}

static void draw(void) {
    draw_toolbar();
    draw_page();
    draw_status();
    if (console_open) {
        canvas_t *c = &w->c;
        int top = TB + page_h(), h = console_h();
        gfx_fill(c, 0, top, w->w, h, RGB(18, 16, 30));
        gfx_hline(c, 0, top, w->w, UI_ACCENT);
        gfx_text(c, 8, top + 4, "JavaScript console  [F12: close]  [Ctrl+K: clear]", UI_ACCENT2, TRANSPARENT, FONT_SMALL);
        canvas_t clip = *c;
        gfx_clip(&clip, 4, top + 24, w->w - 8, h - 24);
        int visible = MAX(0, (h - 26) / 16), count = MIN(visible, console_n);
        for (int i = 0; i < count; i++) {
            int n = (console_next - count + i + CONSOLE_LINES) % CONSOLE_LINES;
            gfx_text(&clip, 8, top + 25 + i * 16, console_lines[n], UI_FG, TRANSPARENT, FONT_SMALL);
        }
    }
    console_dirty = false;
}

static void console_add(const char *level, const char *message) {
    const char *p = message ? message : "";
    do {
        size_t n = strcspn(p, "\r\n");
        snprintf(console_lines[console_next], CONSOLE_LINE, "[%s] %.*s", level ? level : "log",
                 (int)MIN(n, CONSOLE_LINE - 32), p);
        console_next = (console_next + 1) % CONSOLE_LINES;
        if (console_n < CONSOLE_LINES) console_n++;
        console_dirty = true;
        p += n;
        while (*p == '\r' || *p == '\n') p++;
    } while (*p);
}

static void redraw(void) {
    draw();
    win_update(w);
    need_paint = false;
}

static void redraw_status(void) {
    draw_status();
    win_update_rect(w, 0, w->h - SB, w->w, SB);
}

static void set_status(const char *s) {
    strlcpy(status, s, sizeof status);
    if (!hover[0] && !find_open) redraw_status();
}

/* ---------------------------------------------------------------- scrolling */
static void scroll_to(int y) {
    int old = scroll_y;
    scroll_y = y;
    clamp_scroll();
    if (old != scroll_y) {
        scroll_event_pending = true;
        sel_node = NULL;
        hover[0] = 0;
    }
}

/* scrolling keys for the page; true if the event was one */
static bool scroll_key(const struct gui_event *e) {
    int ph = page_h();
    switch (e->key) {
    case NKEY_UP: scroll_to(scroll_y - LINE); return true;
    case NKEY_DOWN: scroll_to(scroll_y + LINE); return true;
    case NKEY_PGUP: scroll_to(scroll_y - (ph - LINE)); return true;
    case NKEY_PGDN: scroll_to(scroll_y + (ph - LINE)); return true;
    case NKEY_HOME: scroll_to(0); return true;
    case NKEY_END: scroll_to(max_scroll()); return true;
    case ' ': scroll_to(scroll_y + (e->mods & NMOD_SHIFT ? -1 : 1) * (ph - LINE)); return true;
    }
    return false;
}

/* an event that only scrolls the page can be handled even while something is loading */
static bool try_scroll_event(const struct gui_event *e) {
    if (e->type == EV_WHEEL) {
        if (sel_node) return false;
        scroll_to(scroll_y + e->wheel * LINE);
        return true;
    }
    if (e->type == EV_KEY && e->pressed && focus == F_PAGE && !sel_node && !(e->mods & (NMOD_CTRL | NMOD_ALT)) &&
        !(doc && web_focused(doc)))
        return scroll_key(e);
    return false;
}

/* ---------------------------------------------------------------- fetching */
struct fetched {
    int status;
    char *body;
    size_t len;
    char ctype[128];
    char url[2048];
    char err[200];
    bool stopped;
};

static void pct_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
}

static const char *guess_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return "";
    static const char *map[][2] = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".svg", "image/svg+xml"}, {".bmp", "image/bmp"}, {".txt", "text/plain"}, {".c", "text/plain"},
        {".h", "text/plain"}, {".sh", "text/plain"}, {".md", "text/plain"}, {".json", "text/plain"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(map); i++)
        if (!strcasecmp(dot, map[i][0])) return map[i][1];
    return "";
}

static bool read_local(const char *url, struct fetched *f) {
    char path[1024];
    const char *p = url + 7;
    if (has_prefix(p, "localhost/")) p += 9;
    strlcpy(path, p, sizeof path);
    char *q = strpbrk(path, "?#");
    if (q) *q = 0;
    pct_decode(path);
    if (!path[0]) strcpy(path, "/");
    struct n_stat st;
    if (stat(path, &st) < 0) {
        snprintf(f->err, sizeof f->err, "%s: no such file or folder", path);
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(f->err, sizeof f->err, "cannot open %s", path);
        return false;
    }
    f->status = 200;
    if (st.type == N_FT_DIR) { /* a folder listing */
        struct sb s = {0};
        sb_str(&s, "<!doctype html><title>");
        sb_esc(&s, path, strlen(path));
        sb_str(&s, "</title><style>body{font-family:sans-serif;margin:24px}td{padding:2px 16px 2px 0}"
                   "a{text-decoration:none}</style><h2>Index of ");
        sb_esc(&s, path, strlen(path));
        sb_str(&s, "</h2><table>");
        if (strcmp(path, "/")) sb_str(&s, "<tr><td><a href=\"../\">../</a><td>");
        struct n_dirent d;
        for (int i = 0; readdir(fd, i, &d) > 0; i++) {
            if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
            bool dir = d.type == N_FT_DIR;
            char sz[32] = "";
            if (!dir) snprintf(sz, sizeof sz, "%lu bytes", (unsigned long)d.size);
            sb_str(&s, "<tr><td><a href=\"");
            sb_esc(&s, d.name, strlen(d.name));
            sb_str(&s, dir ? "/\">" : "\">");
            sb_esc(&s, d.name, strlen(d.name));
            sb_str(&s, dir ? "/</a><td>" : "</a><td>");
            sb_str(&s, sz);
        }
        sb_str(&s, "</table>");
        close(fd);
        f->body = s.b;
        f->len = s.n;
        strcpy(f->ctype, "text/html");
        if (path[strlen(path) - 1] != '/' && strlen(f->url) + 1 < sizeof f->url) strcat(f->url, "/");
        return true;
    }
    if (st.size > (16u << 20)) {
        close(fd);
        strlcpy(f->err, "file exceeds the 16 MiB limit", sizeof f->err);
        return false;
    }
    size_t n = (size_t)st.size;
    f->body = malloc(n + 1);
    if (!f->body) { close(fd); strlcpy(f->err, "out of memory", sizeof f->err); return false; }
    size_t got = 0;
    long r;
    while (got < n && (r = read(fd, f->body + got, n - got)) > 0) got += (size_t)r;
    close(fd);
    f->body[got] = 0;
    f->len = got;
    strlcpy(f->ctype, guess_type(path), sizeof f->ctype);
    return true;
}

/* ---------------------------------------------------------------- built-in pages */
static const char home_page[] =
    "<!doctype html><html><head><title>Nocturne Web</title><style>"
    "body{margin:0;font-family:sans-serif;background:#1e1c34;color:#e8e8f5}"
    ".wrap{max-width:640px;margin:0 auto;padding:56px 24px}"
    "h1{font-size:40px;margin:0 0 4px;color:#f6c86e;font-weight:600}"
    ".sub{color:#9696b9;margin:0 0 32px}"
    "form{display:flex;gap:8px;margin-bottom:36px}"
    "input[type=text]{flex:1;padding:8px 10px;font-size:16px;border:1px solid #5a5490;border-radius:6px;"
    "background:#121020;color:#e8e8f5}"
    "button{padding:8px 16px;font-size:15px;border:0;border-radius:6px;background:#826ef0;color:white}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:12px}"
    ".grid a{display:block;padding:14px 16px;border-radius:8px;background:#2a2748;color:#e8e8f5;"
    "text-decoration:none}"
    ".grid a span{display:block;color:#9696b9;font-size:13px;margin-top:2px}"
    "h2{font-size:18px;color:#c8c4f0;margin:40px 0 12px}"
    "table{border-collapse:collapse;font-size:14px}td{padding:3px 18px 3px 0}"
    "kbd{font-family:monospace;background:#2a2748;border-radius:4px;padding:1px 6px;color:#f6c86e}"
    "</style></head><body><div class=wrap>"
    "<h1>Nocturne Web</h1><p class=sub>HTML, CSS and JavaScript, on Nocturne's own web engine.</p>"
    "<form action=\"https://lite.duckduckgo.com/lite/\"><input type=text name=q placeholder=\"Search DuckDuckGo\">"
    "<button type=submit>Search</button></form>"
    "<div class=grid>"
    "<a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia<span>a random article</span></a>"
    "<a href=\"https://news.ycombinator.com/\">Hacker News<span>news.ycombinator.com</span></a>"
    "<a href=\"https://lite.cnn.com/\">CNN Lite<span>text-only news</span></a>"
    "<a href=\"https://text.npr.org/\">NPR Text<span>text.npr.org</span></a>"
    "<a href=\"https://www.python.org/\">Python<span>python.org</span></a>"
    "<a href=\"https://lobste.rs/\">Lobsters<span>lobste.rs</span></a>"
    "<a href=\"https://developer.mozilla.org/en-US/docs/Web/CSS\">MDN CSS<span>developer.mozilla.org</span></a>"
    "<a href=\"https://example.com/\">Example<span>example.com</span></a>"
    "<a href=\"file:///home/JavaScript/index.html\">JavaScript demo<span>DOM, events, timers and modules</span></a>"
    "<a href=\"file:///home/\">Your files<span>file:///home/</span></a>"
    "</div>"
    "<h2>Keys</h2><table>"
    "<tr><td><kbd>Ctrl+L</kbd><td>address bar (type words to search)"
    "<tr><td><kbd>Ctrl+F</kbd><td>find in page"
    "<tr><td><kbd>Alt+Left</kbd> <kbd>Backspace</kbd><td>back"
    "<tr><td><kbd>Alt+Right</kbd><td>forward"
    "<tr><td><kbd>F5</kbd> <kbd>Ctrl+R</kbd><td>reload"
    "<tr><td><kbd>F12</kbd><td>JavaScript console"
    "<tr><td><kbd>Space</kbd> <kbd>PgDn</kbd> <kbd>PgUp</kbd><td>scroll a page"
    "<tr><td><kbd>Esc</kbd><td>stop loading"
    "</table></div></body></html>";

static char *error_page(const char *url, const char *msg) {
    struct sb s = {0};
    sb_str(&s, "<!doctype html><title>Problem loading page</title><style>body{font-family:sans-serif;"
               "background:#1e1c34;color:#e8e8f5;margin:0}div{max-width:600px;margin:80px auto;padding:0 24px}"
               "h1{color:#f6c86e;font-weight:600}p{color:#b4b4d2;line-height:1.5}a{color:#a898ff}"
               "code{color:#e8e8f5;word-break:break-all}</style><div><h1>Could not load the page</h1><p><code>");
    sb_esc(&s, url, strlen(url));
    sb_str(&s, "</code></p><p>");
    sb_esc(&s, msg, strlen(msg));
    sb_str(&s, "</p><p><a href=\"");
    sb_esc(&s, url, strlen(url));
    sb_str(&s, "\">Try again</a></p></div>");
    return s.b;
}

static char *text_page(const char *url, const char *text, size_t n) {
    struct sb s = {0};
    const char *name = strrchr(url, '/');
    sb_str(&s, "<!doctype html><title>");
    sb_esc(&s, name && name[1] ? name + 1 : url, strlen(name && name[1] ? name + 1 : url));
    sb_str(&s, "</title><pre style=\"white-space:pre-wrap;margin:8px\">");
    sb_esc(&s, text, n);
    sb_str(&s, "</pre>");
    return s.b;
}

static char *image_page(const char *url) {
    struct sb s = {0};
    const char *name = strrchr(url, '/');
    sb_str(&s, "<!doctype html><title>");
    sb_esc(&s, name && name[1] ? name + 1 : url, strlen(name && name[1] ? name + 1 : url));
    sb_str(&s, "</title><body style=\"margin:0;background:#222;text-align:center\"><img style=\"max-width:100%\" src=\"");
    sb_esc(&s, url, strlen(url));
    sb_str(&s, "\">");
    return s.b;
}

/* ---------------------------------------------------------------- navigation */
static void relayout(void) {
    if (!doc) return;
    doc_h = web_layout(doc, page_w(), page_h());
    clamp_scroll();
    need_layout = false;
}

static void set_title(void) {
    char t[256];
    const char *s = doc ? web_title(doc) : "";
    snprintf(t, sizeof t, "%s - Web", *s ? s : cur_url);
    win_set_title(w, t);
}

/* A finished JS task may have retired stylesheet/SVG arenas. No native hit-test,
   control popup, or repaint may use the old boxes after that point. */
static void flush_dom_layout(void) {
    if (!doc || native_wait || web_script_running(doc)) return;
    if (web_dirty(doc)) need_layout = true;
    if (need_layout) {
        relayout();
        set_title();
        need_paint = true;
    }
}

/* Module loading can suspend an executing JS task after a DOM mutation has
   retired its SVG snapshots. Rebuild native boxes before pumping any UI.
   This deliberately bypasses web_script_running(), but never enters JS or
   rescans script/resource work: web_layout and painting are native-only. */
static void prepare_native_snapshot(bool force) {
    if (!doc) return;
    bool dirty = web_dirty(doc);
    if (force || dirty || need_layout) {
        relayout();
        need_paint = true;
    }
}

static void hist_discard(int index) {
    free(hist[index].url); free(hist[index].state);
    memset(&hist[index], 0, sizeof hist[index]);
}
static void hist_append(char *copy) {
    for (int i = hpos + 1; i < nhist; i++) hist_discard(i);
    nhist = hpos + 1;
    if (nhist == MAXHIST) {
        hist_discard(0);
        memmove(hist, hist + 1, sizeof hist[0] * (MAXHIST - 1));
        nhist--;
    }
    memset(&hist[nhist], 0, sizeof hist[nhist]);
    hist[nhist].url = copy;
    hist[nhist].document = generation;
    hist[nhist].entry = ++history_serial;
    hpos = nhist++;
}
static void hist_push(const char *url) {
    char *copy = strdup(url);
    if (!copy) { console_add("error", "History allocation failed"); return; }
    hist_append(copy);
}

static bool same_doc(const char *a, const char *b) {
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
    return la == lb && !strncmp(a, b, la);
}

static int anchor(const char *url) {
    const char *h = strchr(url, '#');
    if (!h || !doc) return -1;
    char frag[512];
    strlcpy(frag, h + 1, sizeof frag);
    int y = web_anchor_y(doc, frag);
    if (y < 0) {
        pct_decode(frag);
        y = web_anchor_y(doc, frag);
    }
    return y;
}

enum { NAV_PUSH, NAV_RELOAD, NAV_HISTORY, NAV_REPLACE };

/* Resource callbacks only queue results in the document; QuickJS runs in web_tick. */
static enum webnet_kind network_kind(int kind) {
    switch (kind) {
    case WEB_RESOURCE_SCRIPT: return WEBNET_CLASSIC;
    case WEB_RESOURCE_MODULE: return WEBNET_MODULE;
    case WEB_RESOURCE_FETCH: return WEBNET_FETCH;
    default: return WEBNET_RESOURCE;
    }
}

static void response_copy(struct web_response *out, const struct webnet_response *in) {
    memset(out, 0, sizeof *out);
    out->status = in->status;
    strlcpy(out->url, in->final_url ? in->final_url : "", sizeof out->url);
    strlcpy(out->headers, in->headers ? in->headers : "", sizeof out->headers);
    strlcpy(out->error, in->error ? in->error : "", sizeof out->error);
    if (in->body_len > WEBNET_BODY_LIMIT) {
        strlcpy(out->error, "response exceeds the 16 MiB limit", sizeof out->error);
        return;
    }
    out->body = malloc(in->body_len + 1);
    if (!out->body) {
        strlcpy(out->error, "out of memory copying response", sizeof out->error);
        return;
    }
    if (in->body_len) memcpy(out->body, in->body, in->body_len);
    out->body[in->body_len] = 0;
    out->body_len = in->body_len;
}

static void remove_transfer(struct transfer *t) {
    struct transfer **p = &transfers;
    while (*p && *p != t) p = &(*p)->next;
    if (*p) *p = t->next;
    free(t);
}

static void resource_completed(webnet *net, uint64_t id, uint64_t gen,
                               const struct webnet_response *result, void *opaque) {
    (void)net; (void)id;
    struct transfer *t = opaque;
    if (doc && gen == generation && t->generation == generation) {
        struct web_response r;
        response_copy(&r, result);
        web_resource_loaded(doc, t->resource_id, &r);
        free(r.body);
    }
    remove_transfer(t);
}

static bool host_request(void *opaque, const struct web_request *request) {
    (void)opaque;
    if (!doc || quit) return false;
    struct transfer *t = calloc(1, sizeof *t);
    if (!t) return false;
    t->resource_id = request->id;
    t->generation = generation;
    struct webnet_request r = {
        .kind = network_kind(request->kind), .generation = generation,
        .url = request->url, .origin = web_url(doc), .method = request->method,
        .headers = request->headers, .body = request->body, .body_len = request->body_len,
        .credentials = request->credentials, .force_preflight = request->force_preflight
    };
    t->network_id = webnet_submit(network, &r, resource_completed, t);
    if (!t->network_id) { free(t); return false; }
    t->next = transfers;
    transfers = t;
    return true;
}

static void host_cancel(void *opaque, uint64_t id) {
    (void)opaque;
    for (struct transfer *t = transfers; t; t = t->next) {
        if (t->generation == generation && t->resource_id == id) {
            webnet_cancel(network, t->network_id);
            remove_transfer(t);
            return;
        }
    }
}

static void cancel_document_requests(void) {
    webnet_cancel_generation(network, generation);
    while (transfers) {
        struct transfer *t = transfers;
        transfers = t->next;
        free(t);
    }
}

static void queue_navigation(const char *url, const char *post, int mode) {
    if (!url || !*url) return;
    char *copy = post ? strdup(post) : NULL;
    if (post && !copy) { console_add("error", "Navigation allocation failed"); return; }
    strlcpy(pending_url, url, sizeof pending_url);
    free(pending_post);
    pending_post = copy;
    pending_mode = mode;
    pending_navigation = true;
}

static void host_navigate(void *opaque, const char *url, const char *post) {
    (void)opaque;
    if (doc && !has_prefix(web_url(doc), "file://") && !has_prefix(web_url(doc), "about:") &&
        has_prefix(url, "file://")) {
        console_add("error", "A remote document cannot navigate to a local file.");
        return;
    }
    queue_navigation(url, post, NAV_PUSH);
}
static void host_navigate_mode(void *opaque, const char *url, int mode) {
    host_navigate(opaque, url, NULL);
    if (pending_navigation) pending_mode = mode == 2 ? NAV_REPLACE : mode == 1 ? NAV_RELOAD : NAV_PUSH;
}

static void host_console(void *opaque, int level, const char *message) {
    (void)opaque;
    console_add(level >= 2 ? "error" : level == 1 ? "warn" : "log", message);
    if (debug_js) fprintf(stderr, "[web-js:%s] %s\n", level >= 2 ? "error" : level == 1 ? "warn" : "log", message);
}

static void host_scroll(void *opaque, int *x, int *y) {
    (void)opaque;
    *x = 0;
    *y = scroll_y;
}
static void host_scroll_to(void *opaque, int x, int y) {
    (void)opaque; (void)x;
    if (need_layout) relayout();
    scroll_to(y);
    need_paint = true;
}
static char *host_cookie_get(void *opaque, const char *url) {
    (void)opaque;
    long n = webnet_cookie_get(network, url, NULL, 0);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (out && webnet_cookie_get(network, url, out, (size_t)n + 1) < 0) { free(out); out = NULL; }
    return out;
}
static void host_cookie_set(void *opaque, const char *url, const char *value) {
    (void)opaque;
    if (webnet_cookie_set(network, url, value) < 0) console_add("error", "Cookie storage allocation failed");
}
static int host_storage(void *opaque, const char *origin, const struct web_storage_request *request,
                        struct web_storage_result *out) {
    (void)opaque;
    return webstorage_access(storage, origin, request, out);
}
static bool host_history(void *opaque, int operation, const char *url, const void *data,
                         size_t len, int value, struct web_history *out) {
    (void)opaque;
    if (hpos < 0 || !doc) return false;
    if (operation == WEB_HISTORY_PUSH || operation == WEB_HISTORY_REPLACE) {
        if (!url || strlen(url) >= sizeof cur_url || len > 1024u * 1024u) return false;
        char *copy = strdup(url); void *saved = len ? malloc(len) : NULL;
        if (!copy || (len && !saved)) { free(copy); free(saved); return false; }
        if (len) memcpy(saved, data, len);
        if (!web_set_url(doc, url)) { free(copy); free(saved); return false; }
        bool manual = hist[hpos].manual;
        hist[hpos].y = scroll_y;
        if (operation == WEB_HISTORY_PUSH) hist_append(copy);
        else { free(hist[hpos].url); free(hist[hpos].state); hist[hpos].url = copy; hist[hpos].entry = ++history_serial; }
        hist[hpos].state = saved; hist[hpos].state_len = len; hist[hpos].manual = manual;
        hist[hpos].y = scroll_y;
        strlcpy(cur_url, url, sizeof cur_url); strlcpy(addr, url, sizeof addr); acur = strlen(addr);
        need_paint = true;
    } else if (operation == WEB_HISTORY_GO) {
        if (history_queue_count == 64) return false;
        int at = (history_queue_head + history_queue_count++) % 64;
        history_queue[at].delta = value; history_queue[at].generation = generation;
    }
    else if (operation == WEB_HISTORY_SCROLL) hist[hpos].manual = value != 0;
    else if (operation != WEB_HISTORY_INFO) return false;
    *out = (struct web_history){.entry=hist[hpos].entry, .length=nhist, .manual_scroll=hist[hpos].manual,
        .state=hist[hpos].state, .state_len=hist[hpos].state_len};
    return true;
}

static void handle_native_wait(const struct gui_event *e);
struct synchronous_load { bool ready; struct web_response response; };
static void synchronous_completed(webnet *net, uint64_t id, uint64_t gen,
                                  const struct webnet_response *r, void *opaque) {
    (void)net; (void)id;
    struct synchronous_load *s = opaque;
    if (gen == generation) response_copy(&s->response, r);
    else { memset(&s->response, 0, sizeof s->response); strcpy(s->response.error, "document was replaced"); }
    s->ready = true;
}

static bool host_sync_load(void *opaque, const char *url, int kind, struct web_response *response) {
    (void)opaque;
    struct synchronous_load s = {0};
    struct webnet_request r = {
        .kind = network_kind(kind), .generation = generation, .url = url,
        .origin = doc ? web_url(doc) : "", .method = "GET",
        .credentials = kind == WEB_RESOURCE_MODULE ? WEBNET_CREDENTIALS_SAME_ORIGIN : WEBNET_CREDENTIALS_INCLUDE
    };
    uint64_t id = webnet_submit(network, &r, synchronous_completed, &s);
    memset(response, 0, sizeof *response);
    if (!id) { strcpy(response->error, "module request could not be started"); return false; }
    prepare_native_snapshot(false);
    if (need_paint && !quit) redraw();
    native_wait = true;
    sync_stopped = false;
    uint64_t started = uptime_ms();
    while (!s.ready && !quit && !sync_stopped && !pending_navigation) {
        uint64_t now = uptime_ms();
        if (now - started >= WEBNET_TIMEOUT_MS) { sync_stopped = true; break; }
        webnet_pump(network, now);
        if (s.ready) break;
        struct gui_event e;
        int timeout = webnet_timeout(network, now);
        int event = win_event(w, &e, timeout < 0 ? 10 : MIN(timeout, 10));
        if (event < 0) { quit = true; break; }
        if (event) handle_native_wait(&e);
        if (console_dirty && console_open) redraw();
    }
    native_wait = false;
    if (!s.ready) {
        webnet_cancel(network, id);
        strcpy(response->error, quit ? "window closed" : pending_navigation ? "navigation canceled module load" : "module load canceled");
        return false;
    }
    *response = s.response;
    return !response->error[0] && response->status >= 200 && response->status < 300;
}

static const struct web_host browser_host = {
    .request = host_request, .cancel = host_cancel, .sync_load = host_sync_load,
    .navigate = host_navigate, .console = host_console, .scroll = host_scroll,
    .scroll_to = host_scroll_to, .history = host_history,
    .cookie_get = host_cookie_get, .cookie_set = host_cookie_set
    , .navigate_mode = host_navigate_mode
    , .storage = host_storage
};

static void navigation_completed(webnet *net, uint64_t id, uint64_t gen,
                                 const struct webnet_response *result, void *opaque) {
    (void)net; (void)opaque;
    if (id != navigation_id || gen != navigation_generation) return;
    navigation_id = 0;
    free(navigation_response.body);
    response_copy(&navigation_response, result);
    navigation_ready = true;
}

static void stop_navigation(void) {
    if (navigation_id) webnet_cancel(network, navigation_id);
    navigation_id = 0;
    navigation_ready = false;
    free(navigation_response.body);
    memset(&navigation_response, 0, sizeof navigation_response);
    loading = false;
}

static void finish_navigation(void) {
    if (!navigation_ready || native_wait || quit) return;
    navigation_ready = false;
    struct web_response f = navigation_response;
    memset(&navigation_response, 0, sizeof navigation_response);
    char ctype[128] = "", charset_buf[64];
    const char *charset = NULL, *image = NULL;
    struct http_resp headers = {0};
    strlcpy(headers.headers, f.headers, sizeof headers.headers);
    http_header(&headers, "Content-Type", ctype, sizeof ctype);
    char *html = NULL;
    size_t hlen = 0;
    if (f.error[0]) html = error_page(navigation_url, f.error);
    else if (has_prefix(f.url, "about:"))
        html = strdup(!strcasecmp(f.url, HOME) ? home_page : "<!doctype html><title>blank</title>");
    else {
        bool sniff_html = !ctype[0] && f.body_len && memchr(f.body, '<', MIN(f.body_len, 512));
        if (has_prefix(ctype, "text/html") || has_prefix(ctype, "application/xhtml") || sniff_html) {
            html = f.body; hlen = f.body_len; f.body = NULL;
            const char *cs = strstr(ctype, "charset=");
            if (cs) {
                cs += 8; if (*cs == '"') cs++;
                size_t n = MIN(strcspn(cs, "\"; "), sizeof charset_buf - 1);
                memcpy(charset_buf, cs, n); charset_buf[n] = 0; charset = charset_buf;
            }
        } else if (has_prefix(ctype, "image/")) { html = image_page(f.url); image = f.url; }
        else if (has_prefix(ctype, "text/") || !ctype[0] || strstr(ctype, "json") ||
                 strstr(ctype, "javascript") || strstr(ctype, "xml")) html = text_page(f.url, f.body, f.body_len);
        else {
            char message[256];
            snprintf(message, sizeof message, "Cannot display %s (%lu bytes).", ctype, (unsigned long)f.body_len);
            html = error_page(f.url, message);
        }
    }
    if (!html) { free(f.body); loading = false; set_status("Out of memory loading page"); return; }
    if (!hlen) hlen = strlen(html);
    const char *final_url = f.url[0] ? f.url : navigation_url;
    web_doc *nd = web_live(html, hlen, final_url, charset, &browser_host);
    free(html);
    if (!nd) { free(f.body); loading = false; set_status("Out of memory creating document"); return; }

    cancel_document_requests();
    if (doc) web_free(doc);
    doc = nd;
    generation = navigation_generation;
    evq_n = 0;
    strlcpy(cur_url, web_url(doc), sizeof cur_url);
    if (navigation_mode == NAV_PUSH) hist_push(cur_url);
    else if (hpos >= 0) {
        char *copy = strdup(cur_url);
        if (copy) { free(hist[hpos].url); hist[hpos].url = copy; }
        if (navigation_mode == NAV_REPLACE) {
            free(hist[hpos].state); hist[hpos].state = NULL; hist[hpos].state_len = 0;
            hist[hpos].document = generation; hist[hpos].entry = ++history_serial; hist[hpos].y = 0;
        } else {
            uint64_t previous = hist[hpos].document;
            for (int i = 0; i < nhist; i++) if (hist[i].document == previous) hist[i].document = generation;
        }
    }
    int keep = navigation_mode == NAV_PUSH || hpos < 0 || hist[hpos].manual ? 0 : hist[hpos].y;
    scroll_y = 0; scroll_event_pending = false;
    find_y = -1; sel_node = pressed_node = hover_node = NULL; hover[0] = 0;
    free(focus_value); focus_value = NULL;
    refresh_at = 0; refresh_seen = false;
    initial_scroll_pending = (hpos < 0 || !hist[hpos].manual) && (navigation_mode != NAV_PUSH || strchr(cur_url, '#') != NULL);
    initial_scroll_anchor = navigation_mode == NAV_PUSH || navigation_mode == NAV_REPLACE;
    initial_scroll_y = keep;
    relayout();
    if (image) {
        for (int i = 0; i < web_image_count(doc); i++)
            if (!strcmp(web_image_url(doc, i), image)) web_image_loaded(doc, i, f.body, f.body_len);
        relayout();
    }
    free(f.body);
    int ay = anchor(cur_url);
    scroll_to(navigation_mode != NAV_PUSH ? keep : ay >= 0 ? ay : 0);
    loading = false;
    status[0] = 0;
    set_title();
    if (navigation_mode == NAV_HISTORY) web_history_event(doc, cur_url, true);
    redraw();
}

static void navigate(const char *url_in, const char *post, int mode) {
    if (native_wait || (doc && web_script_running(doc))) { queue_navigation(url_in, post, mode); return; }
    char url[2048]; strlcpy(url, url_in, sizeof url);
    if (hpos >= 0 && mode != NAV_HISTORY) hist[hpos].y = scroll_y;
    sel_node = NULL; hover[0] = 0; refresh_at = 0;
    if (focus == F_ADDR) focus = F_PAGE;
    if (!post && doc && mode != NAV_RELOAD &&
        ((mode == NAV_HISTORY && hpos >= 0 && hist[hpos].document == generation) ||
         ((strchr(url, '#') || strchr(cur_url, '#')) && same_doc(url, cur_url)))) {
        char old_url[sizeof cur_url]; strlcpy(old_url, cur_url, sizeof old_url);
        if (!web_set_url(doc, url)) return;
        int y = anchor(url);
        if (mode == NAV_PUSH) hist_push(url);
        else if (mode == NAV_REPLACE && hpos >= 0) {
            char *copy = strdup(url);
            if (copy) { free(hist[hpos].url); hist[hpos].url = copy; hist[hpos].entry = ++history_serial; }
        }
        strlcpy(cur_url, url, sizeof cur_url);
        strlcpy(addr, url, sizeof addr); acur = strlen(addr);
        if (mode == NAV_PUSH || mode == NAV_REPLACE) { if (y >= 0) scroll_to(y); else if (!strchr(url, '#') || !strchr(url, '#')[1]) scroll_to(0); }
        else if (mode == NAV_HISTORY && hpos >= 0 && !hist[hpos].manual) scroll_to(hist[hpos].y);
        web_history_event(doc, old_url, mode == NAV_HISTORY);
        redraw(); return;
    }
    stop_navigation();
    navigation_generation = ++next_generation;
    navigation_mode = mode;
    strlcpy(navigation_url, url, sizeof navigation_url);
    loading = true;
    char what[256]; snprintf(what, sizeof what, "Loading %.240s", url); set_status(what);
    draw_toolbar(); win_update_rect(w, 0, 0, w->w, TB);
    if (has_prefix(url, "about:") || has_prefix(url, "file://")) {
        struct fetched local = {0}; strlcpy(local.url, url, sizeof local.url);
        if (has_prefix(url, "file://") && !read_local(url, &local)) {
            strlcpy(navigation_response.error, local.err, sizeof navigation_response.error);
        } else {
            navigation_response.status = local.status ? local.status : 200;
            navigation_response.body = local.body;
            navigation_response.body_len = local.len;
            snprintf(navigation_response.headers, sizeof navigation_response.headers, "Content-Type: %s\r\n", local.ctype);
        }
        strlcpy(navigation_response.url, local.url, sizeof navigation_response.url);
        navigation_ready = true; return;
    }
    struct webnet_request rq = {
        .kind = WEBNET_NAVIGATION, .generation = navigation_generation, .url = url,
        .origin = cur_url, .method = post ? "POST" : "GET",
        .headers = post ? "Content-Type: application/x-www-form-urlencoded\r\n" : NULL,
        .body = post, .body_len = post ? strlen(post) : 0, .user_navigation = true,
        .credentials = WEBNET_CREDENTIALS_INCLUDE
    };
    navigation_id = webnet_submit(network, &rq, navigation_completed, NULL);
    if (!navigation_id) {
        strcpy(navigation_response.error, "request could not be started");
        strlcpy(navigation_response.url, url, sizeof navigation_response.url);
        navigation_ready = true;
    }
}

static void apply_pending_navigation(void) {
    if (!pending_navigation || native_wait || quit || (doc && web_script_running(doc))) return;
    char url[2048]; strlcpy(url, pending_url, sizeof url);
    char *post = pending_post; int mode = pending_mode;
    pending_post = NULL; pending_navigation = false;
    navigate(url, post, mode);
    free(post);
}


static void go_addr(const char *text) {
    while (*text == ' ') text++;
    char url[2048];
    if (!*text) return;
    if (strstr(text, "://") || has_prefix(text, "about:")) strlcpy(url, text, sizeof url);
    else if (text[0] == '/') snprintf(url, sizeof url, "file://%s", text);
    else if (strchr(text, ' ') || !strchr(text, '.')) {
        struct sb s = {0};
        sb_str(&s, SEARCH);
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            char e[4];
            if (isalnum(*p) || strchr("-_.~", *p)) sb_add(&s, (const char *)p, 1);
            else if (*p == ' ') sb_str(&s, "+");
            else {
                snprintf(e, sizeof e, "%%%02X", *p);
                sb_str(&s, e);
            }
        }
        strlcpy(url, s.b, sizeof url);
        free(s.b);
    } else snprintf(url, sizeof url, "https://%s", text);
    navigate(url, NULL, NAV_PUSH);
}

static void go_history(int d) {
    int64_t p = (int64_t)hpos + d;
    if (p < 0 || p >= nhist) return;
    hist[hpos].y = scroll_y;
    hpos = (int)p;
    navigate(hist[hpos].url, NULL, NAV_HISTORY);
}
static void apply_pending_history(void) {
    if (!history_queue_count || loading || native_wait || quit || (doc && web_script_running(doc))) return;
    int at = history_queue_head; history_queue_head = (at + 1) % 64; history_queue_count--;
    if (history_queue[at].generation != generation) return;
    int delta = history_queue[at].delta;
    if (!delta) navigate(cur_url, NULL, NAV_RELOAD);
    else go_history(delta);
}

static void submit(web_node *n) {
    web_node *form = web_form_owner(doc, n);
    struct web_event event = {.type = "submit", .bubbles = true, .cancelable = true};
    bool allowed = form && web_dispatch(doc, form, &event);
    flush_dom_layout();
    if (!allowed) return;
    char *url = NULL, *body = NULL;
    if (!web_submit(doc, n, &url, &body)) return;
    queue_navigation(url, body, NAV_PUSH);
    free(url);
    free(body);
}

/* ---------------------------------------------------------------- input */
static const char *event_key(uint32_t key, char text[8]) {
    switch (key) {
    case NKEY_ENTER: return "Enter";
    case NKEY_ESC: return "Escape";
    case NKEY_BACKSPACE: return "Backspace";
    case NKEY_TAB: return "Tab";
    case NKEY_DELETE: return "Delete";
    case NKEY_UP: return "ArrowUp";
    case NKEY_DOWN: return "ArrowDown";
    case NKEY_LEFT: return "ArrowLeft";
    case NKEY_RIGHT: return "ArrowRight";
    case NKEY_HOME: return "Home";
    case NKEY_END: return "End";
    case NKEY_PGUP: return "PageUp";
    case NKEY_PGDN: return "PageDown";
    default:
        if (key >= NKEY_F1 && key <= NKEY_F12) { snprintf(text, 8, "F%u", key - NKEY_F1 + 1); return text; }
        if (key < 32 || (key >= 0x100 && key < 0x200) || key > 0x10ffff ||
            (key >= 0xd800 && key <= 0xdfff)) { text[0] = 0; return text; }
        if (key < 0x80) { text[0] = (char)key; text[1] = 0; }
        else if (key < 0x800) { text[0] = 0xc0 | (key >> 6); text[1] = 0x80 | (key & 63); text[2] = 0; }
        else if (key < 0x10000) {
            text[0] = 0xe0 | (key >> 12); text[1] = 0x80 | ((key >> 6) & 63);
            text[2] = 0x80 | (key & 63); text[3] = 0;
        } else {
            text[0] = 0xf0 | (key >> 18); text[1] = 0x80 | ((key >> 12) & 63);
            text[2] = 0x80 | ((key >> 6) & 63); text[3] = 0x80 | (key & 63); text[4] = 0;
        }
        return text;
    }
}

static int event_key_code(uint32_t key) {
    switch (key) {
    case NKEY_ENTER: return 13;
    case NKEY_DELETE: return 46;
    case NKEY_INSERT: return 45;
    case NKEY_UP: return 38;
    case NKEY_DOWN: return 40;
    case NKEY_LEFT: return 37;
    case NKEY_RIGHT: return 39;
    case NKEY_HOME: return 36;
    case NKEY_END: return 35;
    case NKEY_PGUP: return 33;
    case NKEY_PGDN: return 34;
    default:
        if (key >= NKEY_F1 && key <= NKEY_F12) return 112 + key - NKEY_F1;
        if (key >= 'a' && key <= 'z') return key - 'a' + 'A';
        return (int)key;
    }
}

static bool dispatch_native(const char *type, web_node *target, const struct gui_event *e, bool cancelable) {
    if (!doc || native_wait) return true;
    char key_text[8];
    struct web_event event = {
        .type = type, .bubbles = true, .cancelable = cancelable,
        .x = e ? e->x : 0, .y = e ? e->y - TB : 0,
        .button = e && (e->buttons & 2) ? 2 : 0,
        .buttons = e ? e->buttons : 0,
        .key = e ? event_key(e->key, key_text) : "",
        .key_code = e ? event_key_code(e->key) : 0,
        .ctrl = e && (e->mods & NMOD_CTRL), .shift = e && (e->mods & NMOD_SHIFT),
        .alt = e && (e->mods & NMOD_ALT)
    };
    bool allowed = web_dispatch(doc, target, &event);
    flush_dom_layout();
    return allowed;
}

static void page_focus(web_node *node) {
    if (!doc) return;
    web_node *old = web_focused(doc);
    if (old == node) return;
    if (old) {
        const char *value = web_control_value(old);
        if (focus_value && value && strcmp(focus_value, value)) dispatch_native("change", old, NULL, false);
        struct web_event blur = {.type = "blur"};
        web_dispatch(doc, old, &blur);
        flush_dom_layout();
        dispatch_native("focusout", old, NULL, false);
    }
    web_focus(doc, node);
    free(focus_value);
    const char *value = node ? web_control_value(node) : NULL;
    focus_value = value ? strdup(value) : NULL;
    if (node) {
        struct web_event event = {.type = "focus"};
        web_dispatch(doc, node, &event);
        flush_dom_layout();
        dispatch_native("focusin", node, NULL, false);
    }
}

static void open_select(web_node *n) {
    int sel = 0;
    sel_n = web_select_options(doc, n, sel_labels, (int)ARRAY_SIZE(sel_labels), &sel);
    if (sel_n <= 0) return;
    int x, y, ww, h;
    if (!web_node_rect(doc, n, &x, &y, &ww, &h)) return;
    sel_node = n;
    sel_cur = sel < 0 ? 0 : sel;
    sel_x = x;
    sel_w = ww;
    for (int i = 0; i < sel_n; i++) sel_w = MAX(sel_w, gfx_text_width(sel_labels[i], FONT_SMALL) + 16);
    sel_x = MAX(0, MIN(sel_x, page_w() - sel_w));
    sel_y = TB + y + h - scroll_y;
    int room = (w->h - SB - sel_y) / SEL_H;
    if (room < MIN(sel_n, 6)) { /* not enough room below: open upwards */
        int up = MIN(sel_n, (TB + y - scroll_y - TB) / SEL_H);
        if (up > room) sel_y = TB + y - scroll_y - up * SEL_H;
    }
    room = (w->h - SB - sel_y) / SEL_H;
    sel_top = sel_cur >= room ? sel_cur - room + 1 : 0;
}

static void choose_select(int i) {
    if (sel_node && i >= 0 && i < sel_n) {
        web_select_set(doc, sel_node, i);
        dispatch_native("input", sel_node, NULL, false);
        dispatch_native("change", sel_node, NULL, false);
    }
    sel_node = NULL;
    relayout();
}

static void addr_focus(void) {
    focus = F_ADDR;
    strlcpy(addr, cur_url, sizeof addr);
    acur = (int)strlen(addr);
    addr_all = true;
}

static void addr_insert(const char *s) {
    if (addr_all) {
        addr[0] = 0;
        acur = 0;
        addr_all = false;
    }
    size_t n = strlen(s), len = strlen(addr);
    if (len + n + 1 > sizeof addr) return;
    memmove(addr + acur + n, addr + acur, len - (size_t)acur + 1);
    memcpy(addr + acur, s, n);
    acur += (int)n;
}

static void addr_key(const struct gui_event *e) {
    int len = (int)strlen(addr);
    bool ctrl = e->mods & NMOD_CTRL;
    if (e->key == NKEY_ENTER) {
        char t[2048];
        strlcpy(t, addr, sizeof t);
        focus = F_PAGE;
        go_addr(t);
        return;
    }
    if (e->key == NKEY_ESC) {
        focus = F_PAGE;
        return;
    }
    if (ctrl && (e->key == 'a' || e->key == 'A')) addr_all = true;
    else if (ctrl && (e->key == 'c' || e->key == 'C')) clipboard_set(addr, strlen(addr));
    else if (ctrl && (e->key == 'v' || e->key == 'V')) {
        char clip[2048];
        int n = clipboard_get(clip, sizeof clip);
        if (n > 0) {
            clip[MIN(n, (int)sizeof clip - 1)] = 0;
            for (char *p = clip; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            addr_insert(clip);
        }
    } else if (e->key == NKEY_BACKSPACE || e->key == NKEY_DELETE) {
        if (addr_all) {
            addr[0] = 0;
            acur = 0;
            addr_all = false;
        } else if (e->key == NKEY_BACKSPACE && acur > 0) {
            memmove(addr + acur - 1, addr + acur, (size_t)(len - acur + 1));
            acur--;
        } else if (e->key == NKEY_DELETE && acur < len)
            memmove(addr + acur, addr + acur + 1, (size_t)(len - acur));
    } else if (e->key == NKEY_LEFT || e->key == NKEY_HOME) {
        acur = addr_all || e->key == NKEY_HOME ? 0 : MAX(acur - 1, 0);
        addr_all = false;
    } else if (e->key == NKEY_RIGHT || e->key == NKEY_END) {
        acur = addr_all || e->key == NKEY_END ? len : MIN(acur + 1, len);
        addr_all = false;
    } else if (e->key >= 32 && e->key < 127 && !ctrl) {
        char s[2] = {(char)e->key, 0};
        addr_insert(s);
    }
}

static void find_next(int from) {
    if (!doc) return;
    find_y = web_find(doc, find_buf, from);
    if (find_y >= 0 && (find_y < scroll_y || find_y > scroll_y + page_h() - 40)) scroll_to(find_y - page_h() / 3);
}

static void find_key(const struct gui_event *e) {
    size_t len = strlen(find_buf);
    if (e->key == NKEY_ESC) {
        find_open = false;
        focus = F_PAGE;
        if (doc) web_find(doc, "", 0);
        return;
    }
    if (e->key == NKEY_ENTER) {
        if (find_buf[0]) find_next(find_y >= 0 ? find_y + 1 : scroll_y);
        return;
    }
    if (e->key == NKEY_BACKSPACE) {
        if (len) find_buf[len - 1] = 0;
    } else if (e->key >= 32 && e->key < 127 && !(e->mods & NMOD_CTRL) && len + 1 < sizeof find_buf) {
        find_buf[len] = (char)e->key;
        find_buf[len + 1] = 0;
    } else return;
    find_next(find_y >= 0 ? find_y : scroll_y);
}

static void key(const struct gui_event *e) {
    bool ctrl = e->mods & NMOD_CTRL, alt = e->mods & NMOD_ALT;
    uint32_t k = e->key;
    if (k >= 'A' && k <= 'Z' && (ctrl || alt)) k += 32;
    if (k == NKEY_F12) {
        console_open = !console_open;
        need_layout = true;
        return;
    }
    if (console_open && ctrl && k == 'k') {
        console_n = console_next = 0;
        console_dirty = true;
        return;
    }
    if (k == NKEY_ESC && loading) {
        stop_navigation();
        set_status("Stopped");
        return;
    }
    if (ctrl && k == 'l') {
        sel_node = NULL;
        addr_focus();
        return;
    }
    if (ctrl && k == 'f') {
        find_open = true;
        focus = F_FIND;
        sel_node = NULL;
        return;
    }
    if (k == NKEY_F5 || (ctrl && k == 'r')) {
        if (cur_url[0]) navigate(cur_url, NULL, NAV_RELOAD);
        return;
    }
    if (alt && k == NKEY_LEFT) return go_history(-1);
    if (alt && k == NKEY_RIGHT) return go_history(1);
    if (sel_node) {
        int room = MAX((w->h - SB - sel_y) / SEL_H, 1);
        if (k == NKEY_ESC) sel_node = NULL;
        else if (k == NKEY_ENTER || k == ' ') choose_select(sel_cur);
        else if (k == NKEY_UP || k == NKEY_DOWN) {
            sel_cur = MAX(0, MIN(sel_n - 1, sel_cur + (k == NKEY_UP ? -1 : 1)));
            if (sel_cur < sel_top) sel_top = sel_cur;
            if (sel_cur >= sel_top + room) sel_top = sel_cur - room + 1;
        }
        return;
    }
    if (focus == F_ADDR) return addr_key(e);
    if (focus == F_FIND) return find_key(e);
    web_node *target = doc ? web_focused(doc) : NULL;
    if (!dispatch_native("keydown", target, e, true)) return;
    if ((e->key >= 32 && !(e->key >= 0x100 && e->key < 0x200)) || e->key == NKEY_ENTER)
        if (!dispatch_native("keypress", target, e, true)) return;
    if (doc && web_focused(doc)) {
        if (k == NKEY_ESC) {
            page_focus(NULL);
            return;
        }
        web_node *control = web_focused(doc);
        const char *value = web_control_value(control);
        char *old_value = strdup(value ? value : "");
        int r = web_key(doc, e);
        if (r == 2) submit(web_focused(doc));
        value = web_control_value(control);
        if (r == 1 && value && old_value && strcmp(old_value, value)) dispatch_native("input", control, e, false);
        free(old_value);
        if (r) return;
        if (e->key >= 32 && e->key < 127) return; /* typed into a control that ignored it */
    }
    if (k == NKEY_BACKSPACE && !ctrl) return go_history(-1);
    if (k == NKEY_ESC && find_open) {
        find_open = false;
        if (doc) web_find(doc, "", 0);
        return;
    }
    scroll_key(e);
}

static void page_click(web_node *target, int x, int y) {
    focus = F_PAGE;
    if (!doc) return;
    struct gui_event native = {.x = x, .y = y, .buttons = 1};
    if (!dispatch_native("click", target, &native, true)) return;
    struct web_hit hit = {0};
    if (!web_node_action(doc, target, &hit)) return;
    switch (hit.kind) {
    case WEB_HIT_LINK:
        if (hit.href) {
            char u[2048];
            strlcpy(u, hit.href, sizeof u);
            if (has_prefix(u, "file://") && !has_prefix(web_url(doc), "file://") &&
                !has_prefix(web_url(doc), "about:")) {
                set_status("A remote page cannot open a local file");
                return;
            }
            page_focus(NULL);
            queue_navigation(u, NULL, NAV_PUSH);
        }
        return;
    case WEB_HIT_TEXT_INPUT:
    case WEB_HIT_TEXTAREA: page_focus(hit.node); break;
    case WEB_HIT_CHECKBOX:
    case WEB_HIT_RADIO:
        page_focus(NULL);
        web_toggle(doc, hit.node);
        dispatch_native("input", hit.node, &native, false);
        dispatch_native("change", hit.node, &native, false);
        break;
    case WEB_HIT_SUBMIT: submit(hit.node); return;
    case WEB_HIT_SELECT:
        page_focus(NULL);
        open_select(hit.node);
        break;
    default: page_focus(NULL); break;
    }
}

static void scrollbar_drag(int my) {
    int ph = page_h();
    if (doc_h <= ph) return;
    int th = MAX(24, (int)((long)ph * ph / doc_h));
    int range = ph - th;
    if (range <= 0) return;
    scroll_to((int)((long)(my - TB - drag_off) * max_scroll() / range));
}

static void mouse_down(const struct gui_event *e) {
    if (!(e->buttons & 1)) return;
    int x = e->x, y = e->y;
    if (console_open && y >= TB + page_h() && y < w->h - SB) return;
    if (sel_node) {
        int vis = MIN(sel_n - sel_top, (w->h - SB - sel_y) / SEL_H);
        if (ui_hit(x, y, sel_x, sel_y, sel_w, vis * SEL_H)) choose_select(sel_top + (y - sel_y) / SEL_H);
        else sel_node = NULL;
        return;
    }
    if (y < TB) {
        if (x >= ADDR_X) {
            if (focus != F_ADDR) addr_focus();
            else { /* place the caret */
                addr_all = false;
                int len = (int)strlen(addr), best = len;
                char pre[2048];
                for (int i = 0; i <= len; i++) {
                    snprintf(pre, sizeof pre, "%.*s", i, addr);
                    if (ADDR_X + 7 + gfx_text_width(pre, FONT_SMALL) >= x) {
                        best = i;
                        break;
                    }
                }
                acur = best;
            }
            return;
        }
        focus = F_PAGE;
        for (int i = 0; i < NBTN; i++)
            if (ui_hit(x, y, btn_x(i), 6, 28, 28)) {
                if (i == BTN_BACK) go_history(-1);
                else if (i == BTN_FWD) go_history(1);
                else if (i == BTN_RELOAD && loading && !native_wait) stop_navigation();
                else if (i == BTN_RELOAD && cur_url[0]) navigate(cur_url, NULL, NAV_RELOAD);
                else if (i == BTN_HOME) navigate(HOME, NULL, NAV_PUSH);
                return;
            }
        return;
    }
    if (y >= w->h - SB) {
        if (find_open) focus = F_FIND;
        return;
    }
    if (x >= w->w - SCRW) {
        int ph = page_h();
        if (doc_h <= ph) return;
        int th = MAX(24, (int)((long)ph * ph / doc_h));
        int ty = TB + (int)((long)scroll_y * (ph - th) / max_scroll());
        if (y < ty) scroll_to(scroll_y - (ph - LINE));
        else if (y >= ty + th) scroll_to(scroll_y + (ph - LINE));
        else {
            drag_sb = true;
            drag_off = y - ty;
        }
        return;
    }
    focus = F_PAGE;
    if (doc) {
        pressed_node = web_node_at(doc, x, y - TB + scroll_y);
        dispatch_native("mousedown", pressed_node, e, true);
    }
}

/* the status bar shows the link under the mouse */
static void mouse_move(const struct gui_event *e) {
    if (drag_sb) {
        scrollbar_drag(e->y);
        redraw();
        return;
    }
    int hb = -1;
    if (e->y < TB)
        for (int i = 0; i < NBTN; i++)
            if (ui_hit(e->x, e->y, btn_x(i), 6, 28, 28)) hb = i;
    if (hb != hot_btn) {
        hot_btn = hb;
        draw_toolbar();
        win_update_rect(w, 0, 0, w->w, TB);
    }
    char h[sizeof hover] = "";
    web_node *node = NULL;
    if (doc && !sel_node && e->y >= TB && e->y < TB + page_h() && e->x >= 0 && e->x < page_w()) {
        struct web_hit hit;
        if (web_hit_test(doc, e->x, e->y - TB + scroll_y, &hit) && hit.href) strlcpy(h, hit.href, sizeof h);
        node = web_node_at(doc, e->x, e->y - TB + scroll_y);
    }
    if (doc && !native_wait && !web_script_running(doc)) {
        struct web_event event = {
            .x=e->x, .y=e->y-TB, .buttons=e->buttons,
            .ctrl=(e->mods & NMOD_CTRL)!=0, .shift=(e->mods & NMOD_SHIFT)!=0,
            .alt=(e->mods & NMOD_ALT)!=0
        };
        web_hover(doc, node, &event);
        hover_node = node;
        flush_dom_layout();
    }
    if (strcmp(h, hover)) {
        strlcpy(hover, h, sizeof hover);
        redraw_status();
    }
}

static void handle(const struct gui_event *e) {
    switch (e->type) {
    case EV_CLOSE: quit = true; return;
    case EV_KEY:
        if (!e->pressed) {
            if (focus == F_PAGE) dispatch_native("keyup", doc ? web_focused(doc) : NULL, e, false);
            flush_dom_layout();
            if (need_paint && !quit) redraw();
            return;
        }
        key(e);
        break;
    case EV_MOUSE_DOWN: mouse_down(e); break;
    case EV_MOUSE_UP:
        if (drag_sb) { drag_sb = false; pressed_node = NULL; break; }
        if (doc && e->y >= TB && e->y < TB + page_h() && e->x < page_w()) {
            web_node *target = web_node_at(doc, e->x, e->y - TB + scroll_y);
            dispatch_native("mouseup", target, e, false);
            if (pressed_node && target == pressed_node) page_click(target, e->x, e->y);
        }
        pressed_node = NULL;
        break;
    case EV_MOUSE_MOVE:
        mouse_move(e);
        flush_dom_layout();
        if (need_paint && !quit) redraw();
        return;
    case EV_WHEEL:
        if (sel_node) {
            int room = MAX((w->h - SB - sel_y) / SEL_H, 1);
            sel_top = MAX(0, MIN(sel_n - room, sel_top + e->wheel));
        } else {
            web_node *target = doc && e->y >= TB && e->y < TB + page_h() ?
                               web_node_at(doc, e->x, e->y - TB + scroll_y) : NULL;
            if (dispatch_native("wheel", target, e, true)) {
                scroll_to(scroll_y + e->wheel * LINE);
            }
        }
        break;
    case EV_RESIZE:
        need_layout = true;
        flush_dom_layout();
        dispatch_native("resize", NULL, e, false);
        break;
    default: return;
    }
    flush_dom_layout();
    if (need_layout) relayout();
    if (!quit) redraw();
}

/* This pump intentionally does not dispatch DOM events, run JS, or destroy a document.
   QuickJS's synchronous module loader can be on the C stack while it is used. */
static void handle_native_wait(const struct gui_event *e) {
    bool moved = false;
    if (e->type == EV_CLOSE) { quit = true; sync_stopped = true; return; }
    if (e->type == EV_RESIZE) { prepare_native_snapshot(true); redraw(); return; }
    if (e->type == EV_KEY && e->pressed) {
        uint32_t k = e->key;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (e->key == NKEY_ESC) { sync_stopped = true; return; }
        if (e->key == NKEY_F12) { console_open = !console_open; prepare_native_snapshot(true); redraw(); return; }
        if ((e->mods & NMOD_CTRL) && k == 'l') { addr_focus(); redraw(); return; }
        if (focus == F_ADDR) { addr_key(e); redraw(); return; }
        if (e->key == NKEY_F5 || ((e->mods & NMOD_CTRL) && k == 'r')) {
            queue_navigation(cur_url, NULL, NAV_RELOAD); return;
        }
        if (e->mods & NMOD_ALT) {
            if (e->key == NKEY_LEFT) { go_history(-1); return; }
            if (e->key == NKEY_RIGHT) { go_history(1); return; }
        }
    }
    if (e->type == EV_MOUSE_DOWN && (e->y < TB || e->x >= w->w - SCRW)) {
        mouse_down(e); redraw(); return;
    }
    if (e->type == EV_MOUSE_MOVE && drag_sb) { scrollbar_drag(e->y); redraw(); return; }
    if (e->type == EV_MOUSE_UP && drag_sb) { drag_sb = false; redraw(); return; }
    if (try_scroll_event(e)) moved = true;
    else if (evq_n < (int)ARRAY_SIZE(evq)) evq[evq_n++] = *e;
    if (moved) redraw();
}

static void document_step(uint64_t now) {
    if (!doc || quit) return;
    if (scroll_event_pending && !native_wait && !web_script_running(doc)) {
        scroll_event_pending = false;
        web_document_scroll(doc);
    }
    web_tick(doc, now);
    if (quit) return;
    bool dirty = web_dirty(doc);
    if (dirty || need_layout) {
        relayout();
        if (initial_scroll_pending) {
            if (initial_scroll_anchor) {
                int y = anchor(cur_url);
                if (y >= 0) { scroll_to(y); initial_scroll_pending = false; }
            } else if (max_scroll() >= initial_scroll_y) {
                scroll_to(initial_scroll_y); initial_scroll_pending = false;
            }
        }
        set_title();
        redraw();
    } else if (need_paint || (console_dirty && console_open)) redraw();
    if (!refresh_seen && !loading) {
        int delay;
        const char *url = web_refresh_url(doc, &delay);
        if (url) {
            refresh_seen = true;
            if (delay <= 10 && strcmp(url, cur_url)) {
                strlcpy(refresh_url, url, sizeof refresh_url);
                refresh_at = uptime_ms() + (uint64_t)MAX(delay, 0) * 1000 + 1;
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--debug-js")) { debug_js = true; argc--; argv++; }
    int sw = 1024, sh = 768;
    screen_size(&sw, &sh);
    w = win_open(MIN(1100, sw - 40), MIN(800, sh - 70), "Web", WIN_RESIZABLE);
    if (!w) return 1;
    network = webnet_create();
    if (!network) { win_close(w); return 1; }
    storage = webstorage_create(); /* lazily touches local disk only on storage access */
    redraw();
    if (argc > 1) {
        if (argv[1][0] == '/') {
            char u[1100];
            snprintf(u, sizeof u, "file://%s", argv[1]);
            navigate(u, NULL, NAV_PUSH);
        } else go_addr(argv[1]);
    } else navigate(HOME, NULL, NAV_PUSH);
    while (!quit) {
        uint64_t now = uptime_ms();
        webnet_pump(network, now);
        apply_pending_history();
        apply_pending_navigation();
        finish_navigation();
        document_step(now);
        apply_pending_history();
        apply_pending_navigation();
        if (quit) break;
        now = uptime_ms();
        struct gui_event e;
        if (evq_n) {
            e = evq[0];
            memmove(evq, evq + 1, sizeof evq[0] * (size_t)--evq_n);
            handle(&e);
            continue;
        }
        int timeout = webnet_timeout(network, now);
        int64_t deadline = doc ? web_deadline(doc) : -1;
        if (deadline >= 0) {
            int due = deadline <= (int64_t)now ? 0 : (int)MIN(deadline - (int64_t)now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        if (navigation_ready || pending_navigation || scroll_event_pending || (history_queue_count && !loading)) timeout = 0;
        if (refresh_at) {
            uint64_t now = uptime_ms();
            if (now >= refresh_at) {
                refresh_at = 0;
                char u[2048];
                strlcpy(u, refresh_url, sizeof u);
                navigate(u, NULL, NAV_PUSH);
                continue;
            }
            int due = (int)MIN(refresh_at - now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        int r = win_event(w, &e, timeout);
        if (r < 0) break;
        if (r > 0) {
            handle(&e);
            continue;
        }
    }
    stop_navigation();
    cancel_document_requests();
    if (doc) web_free(doc);
    webnet_free(network);
    webstorage_free(storage);
    free(pending_post);
    free(focus_value);
    for (int i = 0; i < nhist; i++) hist_discard(i);
    win_close(w);
    return 0;
}
