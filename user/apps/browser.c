/* Web browser: the web engine (libc/web, no JavaScript) with an address bar, history, forms and
   find in page. Pages and stylesheets load before the first paint; images load afterwards, one at
   a time while the window is idle, and the page is laid out again as they arrive. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include "nocturne.h"
#include "http.h"
#include "web.h"

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

static struct {
    char *url;
    int y;
} hist[MAXHIST];
static int nhist, hpos = -1;

enum { F_PAGE, F_ADDR, F_FIND };
static int focus = F_PAGE;
static char addr[2048];
static int acur;
static bool addr_all;

static bool find_open;
static char find_buf[128];
static int find_y = -1;

static web_node *sel_node;
static const char *sel_labels[128];
static int sel_n, sel_cur, sel_top, sel_x, sel_y, sel_w;
#define SEL_H 20
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int img_i;
static bool img_dirty, need_layout;
static uint64_t last_layout;
static char refresh_url[2048];
static uint64_t refresh_at;

static int hot_btn = -1;
static bool drag_sb;
static int drag_off;

/* events that arrived while an image was downloading and could not be handled there */
static struct gui_event evq[64];
static int evq_n;

static int page_w(void) { return MAX(w->w - SCRW, 50); }
static int page_h(void) { return MAX(w->h - TB - SB, 20); }
static int max_scroll(void) { return MAX(doc_h - page_h(), 0); }
static void clamp_scroll(void) { scroll_y = MAX(0, MIN(scroll_y, max_scroll())); }

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
}

static void redraw(void) {
    draw();
    win_update(w);
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
struct sink {
    char *b;
    size_t n, cap, max;
    bool page, stopped;
    uint64_t last;
    const char *what;
};

static int on_body(void *ctx, const char *data, size_t n) {
    struct sink *s = ctx;
    if (s->n + n > s->max) return -1;
    if (s->n + n + 1 > s->cap) {
        while (s->n + n + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 65536;
        char *nb = realloc(s->b, s->cap);
        if (!nb) return -1;
        s->b = nb;
    }
    memcpy(s->b + s->n, data, n);
    s->n += n;
    s->b[s->n] = 0;
    /* keep the window alive: scroll if asked to, Esc stops a page load, closing quits */
    struct gui_event e;
    bool moved = false;
    while (win_event(w, &e, 0) > 0) {
        if (e.type == EV_CLOSE) {
            quit = true;
            return -1;
        }
        if (e.type == EV_RESIZE) {
            need_layout = true;
            moved = true;
            continue;
        }
        if (s->page && e.type == EV_KEY && e.pressed && e.key == NKEY_ESC) {
            s->stopped = true;
            return -1;
        }
        if (try_scroll_event(&e)) moved = true;
        else if (!s->page && evq_n < (int)ARRAY_SIZE(evq)) evq[evq_n++] = e;
    }
    if (moved) redraw();
    uint64_t now = uptime_ms();
    if (s->what && now - s->last > 150) {
        s->last = now;
        char m[300];
        snprintf(m, sizeof m, "%s (%lu KB)", s->what, (unsigned long)(s->n / 1024));
        set_status(m);
    }
    return 0;
}

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
    size_t n = (size_t)st.size;
    f->body = malloc(n + 1);
    size_t got = 0;
    long r;
    while (got < n && (r = read(fd, f->body + got, n - got)) > 0) got += (size_t)r;
    close(fd);
    f->body[got] = 0;
    f->len = got;
    strlcpy(f->ctype, guess_type(path), sizeof f->ctype);
    return true;
}

/* GET (or POST when post is not NULL) a URL, following redirects. page: the main document (Esc
   stops it; anything else is an image or a stylesheet). */
static bool fetch(const char *url, const char *post, bool page, const char *what, struct fetched *f) {
    memset(f, 0, sizeof *f);
    strlcpy(f->url, url, sizeof f->url);
    const char *hash = strchr(url, '#');
    char frag[512] = "";
    if (hash) strlcpy(frag, hash, sizeof frag);
    for (int hop = 0; hop < 10; hop++) {
        if (has_prefix(f->url, "file://")) return read_local(f->url, f);
        if (!has_prefix(f->url, "http://") && !has_prefix(f->url, "https://")) {
            snprintf(f->err, sizeof f->err, "this browser cannot open %.40s links", f->url);
            return false;
        }
        char req_url[2048];
        strlcpy(req_url, f->url, sizeof req_url);
        char *h = strchr(req_url, '#');
        if (h) *h = 0;
        char hdrs[256];
        snprintf(hdrs, sizeof hdrs, "%s%s",
                 page ? "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\nAccept-Language: en\r\n"
                      : "Accept: image/*,text/css,*/*;q=0.5\r\n",
                 post ? "Content-Type: application/x-www-form-urlencoded\r\n" : "");
        struct sink s = {.max = page ? 24u << 20 : 12u << 20, .page = page, .what = what};
        struct http_req rq = {.method = post ? "POST" : "GET", .url = req_url, .headers = hdrs, .body = post,
                              .body_len = post ? strlen(post) : 0, .timeout_ms = 20000, .on_body = on_body, .ctx = &s};
        struct http_resp rs;
        memset(&rs, 0, sizeof rs);
        int r = http_request(&rq, &rs);
        if (r < 0 || quit || s.stopped) {
            f->stopped = s.stopped;
            snprintf(f->err, sizeof f->err, "%s", s.stopped ? "stopped" : rs.error[0] ? rs.error : "the transfer failed");
            free(s.b);
            http_resp_free(&rs);
            return false;
        }
        char loc[2048];
        int st = rs.status;
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && http_header(&rs, "Location", loc, sizeof loc)) {
            char next[2048];
            free(s.b);
            http_resp_free(&rs);
            if (!web_resolve_url(f->url, loc, next, sizeof next)) {
                snprintf(f->err, sizeof f->err, "bad redirect to %.100s", loc);
                return false;
            }
            if (frag[0] && !strchr(next, '#') && strlen(next) + strlen(frag) < sizeof next) strcat(next, frag);
            strlcpy(f->url, next, sizeof f->url);
            if (st == 303 || ((st == 301 || st == 302) && post)) post = NULL;
            continue;
        }
        f->status = st;
        f->body = s.b ? s.b : calloc(1, 1);
        f->len = s.n;
        if (!http_header(&rs, "Content-Type", f->ctype, sizeof f->ctype)) f->ctype[0] = 0;
        http_resp_free(&rs);
        return true;
    }
    snprintf(f->err, sizeof f->err, "too many redirects");
    return false;
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
    "<h1>Nocturne Web</h1><p class=sub>HTML and CSS, laid out and painted by Nocturne itself. No JavaScript.</p>"
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
    "<a href=\"file:///home/\">Your files<span>file:///home/</span></a>"
    "</div>"
    "<h2>Keys</h2><table>"
    "<tr><td><kbd>Ctrl+L</kbd><td>address bar (type words to search)"
    "<tr><td><kbd>Ctrl+F</kbd><td>find in page"
    "<tr><td><kbd>Alt+Left</kbd> <kbd>Backspace</kbd><td>back"
    "<tr><td><kbd>Alt+Right</kbd><td>forward"
    "<tr><td><kbd>F5</kbd> <kbd>Ctrl+R</kbd><td>reload"
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
    last_layout = uptime_ms();
    need_layout = false;
}

static void set_title(void) {
    char t[256];
    const char *s = doc ? web_title(doc) : "";
    snprintf(t, sizeof t, "%s - Web", *s ? s : cur_url);
    win_set_title(w, t);
}

static void hist_push(const char *url) {
    for (int i = hpos + 1; i < nhist; i++) free(hist[i].url);
    nhist = hpos + 1;
    if (nhist == MAXHIST) {
        free(hist[0].url);
        memmove(hist, hist + 1, sizeof hist[0] * (MAXHIST - 1));
        nhist--;
    }
    hist[nhist].url = strdup(url);
    hist[nhist].y = 0;
    hpos = nhist++;
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

enum { NAV_PUSH, NAV_RELOAD, NAV_HISTORY };

static void navigate(const char *url_in, const char *post, int mode) {
    char url[2048];
    strlcpy(url, url_in, sizeof url);
    if (hpos >= 0 && mode != NAV_HISTORY) hist[hpos].y = scroll_y;
    sel_node = NULL;
    hover[0] = 0;
    refresh_at = 0;
    if (focus == F_ADDR) focus = F_PAGE;
    /* a link to another part of this page */
    if (!post && doc && mode != NAV_RELOAD && strchr(url, '#') && same_doc(url, cur_url)) {
        int y = anchor(url);
        if (mode == NAV_PUSH) hist_push(url);
        strlcpy(cur_url, url, sizeof cur_url);
        if (y >= 0 && mode == NAV_PUSH) scroll_to(y);
        else if (mode == NAV_HISTORY) scroll_to(hist[hpos].y);
        redraw();
        return;
    }
    loading = true;
    char what[300];
    snprintf(what, sizeof what, "Loading %s", url);
    set_status(what);
    draw_toolbar();
    win_update_rect(w, 0, 0, w->w, TB);

    struct fetched f;
    char *html = NULL, *charset = NULL, cs[64];
    const char *image = NULL;
    size_t hlen = 0;
    if (has_prefix(url, "about:")) {
        memset(&f, 0, sizeof f);
        strlcpy(f.url, url, sizeof f.url);
        html = strdup(!strcasecmp(url, HOME) ? home_page : "<!doctype html><title>blank</title>");
    } else if (!fetch(url, post, true, what, &f)) {
        loading = false;
        if (quit) return;
        if (f.stopped) {
            set_status("Stopped");
            redraw();
            return;
        }
        html = error_page(url, f.err);
    } else {
        const char *ct = f.ctype;
        bool sniff_html = !*ct && f.len && memchr(f.body, '<', f.len < 512 ? f.len : 512);
        if (has_prefix(ct, "text/html") || has_prefix(ct, "application/xhtml") || sniff_html) {
            html = f.body;
            hlen = f.len;
            f.body = NULL;
            const char *c = strstr(ct, "charset=");
            if (c) {
                c += 8;
                if (*c == '"') c++;
                size_t k = strcspn(c, "\"; ");
                snprintf(cs, sizeof cs, "%.*s", (int)MIN(k, sizeof cs - 1), c);
                charset = cs;
            }
        } else if (has_prefix(ct, "image/")) {
            html = image_page(f.url);
            image = f.url;
        } else if (has_prefix(ct, "text/") || !*ct || strstr(ct, "json") || strstr(ct, "javascript") ||
                   strstr(ct, "xml")) {
            html = text_page(f.url, f.body, f.len);
        } else {
            char m[300];
            snprintf(m, sizeof m, "This is a %s file (%lu bytes), which the browser cannot show.", ct,
                     (unsigned long)f.len);
            html = error_page(f.url, m);
        }
    }
    if (!hlen) hlen = strlen(html);
    web_doc *nd = web_parse(html, hlen, f.url, charset);
    free(html);

    /* stylesheets, before anything is shown */
    const char *u;
    int nsheet = 0;
    while (!quit && (u = web_pending_stylesheet(nd))) {
        char su[2048];
        strlcpy(su, u, sizeof su);
        snprintf(what, sizeof what, "Loading stylesheet %d: %s", ++nsheet, su);
        struct fetched cf;
        if (fetch(su, NULL, false, what, &cf) && cf.status < 400) web_stylesheet_loaded(nd, cf.body, cf.len);
        else web_stylesheet_loaded(nd, NULL, 0);
        free(cf.body);
    }
    if (quit) {
        web_free(nd);
        free(f.body);
        return;
    }

    if (doc) web_free(doc);
    doc = nd;
    evq_n = 0;
    strlcpy(cur_url, web_url(doc), sizeof cur_url);
    if (mode == NAV_PUSH) hist_push(cur_url);
    else if (hpos >= 0) {
        free(hist[hpos].url);
        hist[hpos].url = strdup(cur_url);
    }
    int keep = mode == NAV_PUSH ? 0 : hist[hpos].y;
    scroll_y = 0;
    relayout();
    if (image)
        for (int i = 0; i < web_image_count(doc); i++)
            if (!strcmp(web_image_url(doc, i), image)) {
                web_image_loaded(doc, i, f.body, f.len);
                relayout();
            }
    free(f.body);
    int ay = anchor(cur_url);
    scroll_to(mode != NAV_PUSH ? keep : ay >= 0 ? ay : 0);
    img_i = 0;
    img_dirty = false;
    find_y = -1;
    int delay;
    const char *r = web_refresh_url(doc, &delay);
    if (r && delay <= 10 && strcmp(r, cur_url)) {
        strlcpy(refresh_url, r, sizeof refresh_url);
        refresh_at = uptime_ms() + (uint64_t)delay * 1000 + 1;
    }
    loading = false;
    set_title();
    status[0] = 0;
    redraw();
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
    int p = hpos + d;
    if (p < 0 || p >= nhist) return;
    hist[hpos].y = scroll_y;
    hpos = p;
    navigate(hist[hpos].url, NULL, NAV_HISTORY);
}

static void submit(web_node *n) {
    char *url = NULL, *body = NULL;
    if (!web_submit(doc, n, &url, &body)) return;
    navigate(url, body, NAV_PUSH);
    free(url);
    free(body);
}

/* load one more image; false when there is nothing left to do */
static bool load_image_step(void) {
    if (!doc) return false;
    int n = web_image_count(doc);
    while (img_i < n && !web_image_wanted(doc, img_i)) img_i++;
    if (img_i >= n) {
        if (!img_dirty) return false;
        img_dirty = false;
        relayout();
        if (!hover[0] && !find_open) status[0] = 0;
        redraw();
        return true; /* layout may have asked for more (CSS backgrounds) */
    }
    char u[2048], what[320];
    strlcpy(u, web_image_url(doc, img_i), sizeof u);
    int cnt = 0;
    for (int i = img_i; i < n; i++) cnt += web_image_wanted(doc, i);
    snprintf(what, sizeof what, "Loading images: %d left", cnt);
    set_status(what);
    struct fetched f;
    bool ok = fetch(u, NULL, false, what, &f);
    if (quit) {
        free(f.body);
        return false;
    }
    web_image_loaded(doc, img_i, ok && f.status < 400 ? f.body : NULL, ok ? f.len : 0);
    free(f.body);
    img_i++;
    img_dirty = true;
    if (uptime_ms() - last_layout > 700) {
        relayout();
        img_dirty = false;
        redraw();
    }
    return true;
}

/* ---------------------------------------------------------------- input */
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
    if (sel_node && i >= 0 && i < sel_n) web_select_set(doc, sel_node, i);
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
    if (doc && web_focused(doc)) {
        if (k == NKEY_ESC) {
            web_focus(doc, NULL);
            return;
        }
        int r = web_key(doc, e);
        if (r == 2) submit(web_focused(doc));
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

static void page_click(int x, int y) {
    focus = F_PAGE;
    if (!doc) return;
    struct web_hit hit;
    web_hit_test(doc, x, y - TB + scroll_y, &hit);
    switch (hit.kind) {
    case WEB_HIT_LINK:
        if (hit.href) {
            char u[2048];
            strlcpy(u, hit.href, sizeof u);
            web_focus(doc, NULL);
            navigate(u, NULL, NAV_PUSH);
        }
        return;
    case WEB_HIT_TEXT_INPUT:
    case WEB_HIT_TEXTAREA: web_focus(doc, hit.node); break;
    case WEB_HIT_CHECKBOX:
    case WEB_HIT_RADIO:
        web_focus(doc, NULL);
        web_toggle(doc, hit.node);
        break;
    case WEB_HIT_SUBMIT: submit(hit.node); return;
    case WEB_HIT_SELECT:
        web_focus(doc, NULL);
        open_select(hit.node);
        break;
    default: web_focus(doc, NULL); break;
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
    page_click(x, y);
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
    if (doc && !sel_node && e->y >= TB && e->y < w->h - SB && e->x < page_w()) {
        struct web_hit hit;
        if (web_hit_test(doc, e->x, e->y - TB + scroll_y, &hit) && hit.href) strlcpy(h, hit.href, sizeof h);
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
        if (!e->pressed) return;
        key(e);
        break;
    case EV_MOUSE_DOWN: mouse_down(e); break;
    case EV_MOUSE_UP:
        if (!drag_sb) return;
        drag_sb = false;
        break;
    case EV_MOUSE_MOVE: mouse_move(e); return;
    case EV_WHEEL:
        if (sel_node) {
            int room = MAX((w->h - SB - sel_y) / SEL_H, 1);
            sel_top = MAX(0, MIN(sel_n - room, sel_top + e->wheel));
        } else scroll_to(scroll_y + e->wheel * LINE);
        break;
    case EV_RESIZE: need_layout = true; break;
    default: return;
    }
    if (need_layout) relayout();
    if (!quit) redraw();
}

int main(int argc, char **argv) {
    int sw = 1024, sh = 768;
    screen_size(&sw, &sh);
    w = win_open(MIN(1100, sw - 40), MIN(800, sh - 70), "Web", WIN_RESIZABLE);
    if (!w) return 1;
    redraw();
    if (argc > 1) {
        if (argv[1][0] == '/') {
            char u[1100];
            snprintf(u, sizeof u, "file://%s", argv[1]);
            navigate(u, NULL, NAV_PUSH);
        } else go_addr(argv[1]);
    } else navigate(HOME, NULL, NAV_PUSH);
    while (!quit) {
        struct gui_event e;
        if (evq_n) {
            e = evq[0];
            memmove(evq, evq + 1, sizeof evq[0] * (size_t)--evq_n);
            handle(&e);
            continue;
        }
        bool busy = doc && (img_i < web_image_count(doc) || img_dirty);
        int timeout = busy ? 0 : -1;
        if (refresh_at) {
            uint64_t now = uptime_ms();
            if (now >= refresh_at) {
                refresh_at = 0;
                char u[2048];
                strlcpy(u, refresh_url, sizeof u);
                navigate(u, NULL, NAV_PUSH);
                continue;
            }
            if (!busy) timeout = (int)(refresh_at - now);
        }
        int r = win_event(w, &e, timeout);
        if (r < 0) break;
        if (r > 0) {
            handle(&e);
            continue;
        }
        if (busy) load_image_step();
    }
    if (doc) web_free(doc);
    win_close(w);
    return 0;
}
