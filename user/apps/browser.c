/* Nocturne Web: native DOM, layout and painting, with a document-owned JavaScript runtime.
   HTTP requests run in isolated Nocturne child processes, never on the GUI/JS task. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include "nocturne.h"
#include "http.h"
#include "web.h"
#include "webnet.h"
#include "webstorage.h"
#include <parallel.h>

#define TB 40   /* toolbar */
#define SB 24   /* status / find bar */
#define SCRW 10 /* scrollbar */
#define LINE 48
#define HOME "about:home"
#define SEARCH "https://lite.duckduckgo.com/lite/?q="

static window_t *w;
static web_doc *doc;
static int scroll_x, scroll_y, doc_w, doc_h;
struct url_text { char *p; size_t capacity; };
static struct url_text current_text, navigation_text, pending_text, address_text, refresh_text;
#define cur_url (current_text.p ? current_text.p : "")
#define navigation_url (navigation_text.p ? navigation_text.p : "")
#define pending_url (pending_text.p ? pending_text.p : "")
#define addr (address_text.p ? address_text.p : "")
#define refresh_url (refresh_text.p ? refresh_text.p : "")
static char status[256], hover[512];
static bool quit, loading;
static bool window_state_known;
static unsigned window_previous_state;
static bool debug_js;
/* Debug-only inclusive phase totals, with no page data or per-frame logging. */
static struct {
    uint64_t since, tick_ms, tick_max_ms, layout_ms, draw_ms, update_ms, net_ms, wait_ms;
    unsigned ticks, layouts, draws;
} loop_profile;
/* Browser pages need more time than small custom web_live hosts. The native
   command line can select a finite limit, never an unlimited watchdog. */
static uint32_t js_task_budget_ms = WEB_JS_TASK_DEFAULT_MS;
static webnet *network;
static webstorage *storage; /* browser-window lifetime, not document lifetime */
static uint64_t generation = 1, next_generation = 1, navigation_generation, navigation_id;
static int navigation_mode;
static struct web_navigation_timing navigation_timing;
static bool native_wait, pending_navigation;
static char *pending_post;
static size_t pending_post_len;
static char *pending_content_type;
static int pending_mode;
static bool navigation_ready, sync_stopped;

struct transfer {
    struct transfer *next;
    uint64_t network_id, resource_id, generation;
    int kind;
    bool keepalive, detached;
};
static struct transfer *transfers;
static struct web_response navigation_response;
_Static_assert(WEB_RESPONSE_HEADERS_MAX >= WEBNET_RESPONSE_HEADERS_MAX, "Browser must retain complete native response headers");

#define CONSOLE_LINE 1024
static char (*console_lines)[CONSOLE_LINE];
static int console_n, console_capacity, console_skip;
static unsigned long console_sequence;
static bool console_open, console_dirty;
static char *console_input, *console_startup;
static size_t console_input_capacity, console_startup_length;
static size_t console_cursor;
static bool console_selected;
static uint64_t console_due, console_startup_generation;
static bool console_each_document;
/* An explicit native debug file is not a page-injected script. Preserve the
   old once-per-window default, and never re-run it on same-document history. */
static bool console_startup_pending(void) {
    return console_startup && (!console_each_document || console_startup_generation != generation);
}

static int console_h(void);
static int console_visible(void) { return MAX(1, (console_h() - 48) / 16); }
static void console_scroll(int delta) {
    int64_t desired = (int64_t)console_skip + delta;
    console_skip = (int)MAX(0, MIN(desired, MAX(console_n - console_visible(), 0)));
    console_dirty = true;
}
static bool console_copy_log(void) {
    if ((size_t)console_n > SIZE_MAX / (CONSOLE_LINE + 1)) return false;
    size_t capacity = (size_t)console_n * (CONSOLE_LINE + 1) + 1, used = 0;
    char *text = malloc(capacity);
    if (!text) return false;
    for (int i = 0; i < console_n; i++) {
        int index = i;
        size_t n = strlen(console_lines[index]);
        memcpy(text + used, console_lines[index], n); used += n;
        text[used++] = '\n';
    }
    int result=clipboard_set(text, used); free(text);
    return result>=0;
}

static struct browser_history_entry {
    char *url;
    int x, y;
    uint64_t document, entry;
    void *state;
    size_t state_len;
    bool manual;
} *hist;
static int nhist, hist_capacity, hpos = -1;
static uint64_t history_serial;
static struct browser_history_task { struct browser_history_task *next; int delta; uint64_t generation; }
    *history_queue, *history_queue_tail;
static unsigned history_queue_count;

enum { F_PAGE, F_ADDR, F_FIND, F_CONSOLE };
static int focus = F_PAGE;
static size_t acur;
static bool addr_all;

static bool find_open;
static struct url_text find_text;
#define find_buf (find_text.p ? find_text.p : "")
static bool find_all, find_error;
static int find_y = -1;

static web_node *sel_node;
static bool sel_datalist, sel_autocomplete;
static web_node *pressed_node, *hover_node;
static char *focus_value;
static const char **sel_labels;
static int sel_capacity;
static int sel_n, sel_cur, sel_top, sel_x, sel_y, sel_w;
#define SEL_H 20
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool need_layout, need_paint;
static uint64_t refresh_at;
static bool refresh_seen, initial_scroll_pending, initial_scroll_anchor;
static int initial_scroll_x, initial_scroll_y;

static int hot_btn = -1;
static bool drag_sb;
static int drag_off;

/* events that arrived while an image was downloading and could not be handled there */
static struct gui_event *evq;
static int evq_n, evq_capacity;
static bool evq_failure_logged;
static bool evq_hover_tail_valid;
static bool scroll_event_pending;
#include "browser_events.h"

static int page_w(void) { return MAX(w->w - SCRW, 50); }
static int console_h(void) { return console_open ? MIN(220, MAX(80, (w->h - TB - SB) / 3)) : 0; }
static int page_h(void) { return MAX(w->h - TB - SB - console_h(), 20); }
static int max_scroll(void) { return MAX(doc_h - page_h(), 0); }
static int max_scroll_x(void) { return MAX(doc_w - page_w(), 0); }
static int document_x(int client_x) { return client_x + scroll_x; }
static void clamp_scroll(void) {
    int x = MAX(0, MIN(scroll_x, max_scroll_x()));
    int y = MAX(0, MIN(scroll_y, max_scroll()));
    if (x != scroll_x || y != scroll_y) scroll_event_pending = true;
    scroll_x = x; scroll_y = y;
    web_viewport_position(doc,scroll_x,scroll_y);
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
static bool url_text_reserve(struct url_text *text, size_t needed) {
    if (needed <= text->capacity) return true;
    size_t capacity = text->capacity ? text->capacity : 64;
    while (capacity < needed) capacity = capacity > SIZE_MAX / 2 ? needed : capacity * 2;
    char *grown = realloc(text->p, capacity);
    if (!grown) return false;
    text->p = grown; text->capacity = capacity; return true;
}
static bool url_text_set(struct url_text *text, const char *source) {
    char *copy = strdup(source ? source : "");
    if (!copy) return false;
    free(text->p); text->p = copy; text->capacity = strlen(copy) + 1; return true;
}
static bool url_pair_prepare(const char *url, char **current, char **address) {
    *current = strdup(url); *address = *current ? strdup(url) : NULL;
    if (*current && *address) return true;
    free(*current); free(*address); *current = *address = NULL; return false;
}
static void url_pair_commit(char *current, char *address) {
    free(current_text.p); free(address_text.p);
    current_text = (struct url_text){current, strlen(current) + 1};
    address_text = (struct url_text){address, strlen(address) + 1};
    acur = strlen(address);
}
static int address_width(const char *text, size_t bytes) {
    int width = 0;
    while (*text && bytes) {
        uint32_t cp; int n = gfx_utf8_decode(text, &cp);
        if (n <= 0 || (size_t)n > bytes) break;
        int add = MAX(gfx_codepoint_width(cp, FONT_SMALL), 0);
        if (add > INT_MAX - width) return INT_MAX;
        width += add; text += n; bytes -= (size_t)n;
    }
    return width;
}

/* ---------------------------------------------------------------- drawing */
static void field(canvas_t *c, int x, int y, int fw, int h, const char *text, bool foc, size_t caret, bool all) {
    gfx_fill_round(c, x, y, fw, h, 5, foc ? UI_ACCENT : RGB(70, 66, 110));
    gfx_fill_round(c, x + 1, y + 1, fw - 2, h - 2, 4, RGB(18, 16, 32));
    canvas_t cl = *c;
    gfx_clip(&cl, x + 4, y, fw - 8, h);
    int ty = y + (h - 16) / 2;
    int cpx = foc ? address_width(text, caret) : 0;
    int shift = foc && cpx > fw - 20 ? cpx - (fw - 20) : 0;
    int tx = x + 7 - shift;
    if (foc && all && *text) gfx_fill(&cl, tx - 1, ty, MIN(address_width(text, strlen(text)), INT_MAX - 2) + 2, 16, RGB(70, 60, 140));
    gfx_text(&cl, tx, ty, text, foc ? UI_FG : RGB(200, 200, 225), TRANSPARENT, FONT_SMALL);
    if (foc && !all) gfx_fill(&cl, tx + cpx, ty, 2, 16, UI_ACCENT2);
}

enum { BTN_BACK, BTN_FWD, BTN_RELOAD, BTN_HOME, BTN_HISTORY, NBTN };
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
        } else if (i == BTN_HOME) {
            gfx_triangle(c, cx - 8, cy - 1, cx, cy - 8, cx + 8, cy - 1, col);
            gfx_fill(c, cx - 5, cy - 1, 10, 8, col);
            gfx_fill(c, cx - 1, cy + 3, 3, 4, UI_BTN);
        } else {
            gfx_text(c, cx - 4, cy - 7, "H", col, TRANSPARENT, FONT_SMALL);
        }
    }
    int room = W - ADDR_X - 8;
    field(c, ADDR_X, 8, room, 24, focus == F_ADDR ? addr : cur_url, focus == F_ADDR,
                focus == F_ADDR ? acur : 0, addr_all);
}

static void draw_status(void) {
    canvas_t *c = &w->c;
    int W = w->w, y = w->h - SB;
    gfx_fill(c, 0, y, W, SB, RGB(28, 26, 46));
    gfx_hline(c, 0, y, W, RGB(60, 56, 100));
    if (find_open) {
        gfx_text(c, 8, y + 4, "Find:", UI_ACCENT2, TRANSPARENT, FONT_SMALL);
        field(c, 56, y + 2, 240, 20, find_buf, focus == F_FIND, strlen(find_buf), find_all);
        const char *msg = find_error ? "allocation failed" : !find_buf[0] ? "Enter: next   Esc: close" : find_y < 0 ? "not found" : "Enter: next match";
        gfx_text(c, 306, y + 4, msg, find_error || (find_y < 0 && find_buf[0]) ? RGB(240, 120, 120) : UI_DIM, TRANSPARENT, FONT_SMALL);
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
    if (doc) web_paint(doc, c, 0, TB, pw, ph, scroll_x, scroll_y);
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

static void draw_console(void) {
    if (console_open) {
        canvas_t *c = &w->c;
        int top = TB + page_h(), h = console_h();
        gfx_fill(c, 0, top, w->w, h, RGB(18, 16, 30));
        gfx_hline(c, 0, top, w->w, UI_ACCENT);
        char heading[192];
        snprintf(heading, sizeof heading, "JavaScript console  [PgUp/PgDn: history] [Ctrl+Shift+C: copy log] [Ctrl+K: clear]  %d/%d%s",
                 console_n - console_skip, console_n, console_skip ? " (paused)" : "");
        gfx_text(c, 8, top + 4, heading, UI_ACCENT2, TRANSPARENT, FONT_SMALL);
        canvas_t clip = *c;
        gfx_clip(&clip, 4, top + 24, w->w - 8, h - 46);
        console_skip = MIN(console_skip, MAX(console_n - console_visible(), 0));
        int visible = MAX(0, (h - 48) / 16), count = MIN(visible, console_n - console_skip);
        for (int i = 0; i < count; i++) {
            int n = console_n - console_skip - count + i;
            gfx_text(&clip, 8, top + 25 + i * 16, console_lines[n], UI_FG, TRANSPARENT, FONT_SMALL);
        }
        gfx_hline(c, 4, top+h-23, w->w-8, UI_DIM);
        canvas_t input=*c;gfx_clip(&input,8,top+h-20,w->w-16,20);
        gfx_text(&input,8,top+h-19,focus==F_CONSOLE?"> ":"  ",UI_ACCENT2,TRANSPARENT,FONT_SMALL);
        size_t first=console_cursor>(size_t)MAX((w->w-48)/8,1)?console_cursor-(size_t)MAX((w->w-48)/8,1):0;
        while(first&&((unsigned char)console_input[first]&0xc0)==0x80)first++;
        gfx_text(&input,24,top+h-19,console_input+first,UI_FG,TRANSPARENT,FONT_SMALL);
        if(focus==F_CONSOLE){char saved=console_input[console_cursor];console_input[console_cursor]=0;
            int x=24+gfx_text_width(console_input+first,FONT_SMALL);console_input[console_cursor]=saved;
            gfx_vline(&input,x,top+h-19,15,UI_ACCENT2);}
    }
    console_dirty = false;
}
static void draw(void) {
    draw_toolbar(); draw_page(); draw_status(); draw_console();
}

static void console_add(const char *level, const char *message) {
    const char *p = message ? message : "";
    uint64_t now = uptime_ms();
    unsigned long sequence = ++console_sequence;
    int glyph = w ? MAX(gfx_text_width("M", FONT_SMALL), 1) : 8;
    size_t wrap = (size_t)MAX(16, MIN(CONSOLE_LINE - 64, (w ? w->w : 1024) / glyph - 48));
    do {
        size_t n = strcspn(p, "\r\n");
        size_t offset = 0;
        do {
            size_t part = MIN(n - offset, wrap);
            /* Do not split a UTF-8 scalar at a console continuation boundary. */
            if (offset + part < n)
                while (part && ((unsigned char)p[offset + part] & 0xc0) == 0x80) part--;
            if (!part && offset < n) part = MIN(n - offset, 4);
            if (console_n == console_capacity) {
                if (console_n == INT_MAX) return;
                int capacity = console_capacity ? (console_capacity > INT_MAX / 2 ? INT_MAX : console_capacity * 2) : 64;
                if ((size_t)capacity > SIZE_MAX / sizeof *console_lines) return;
                void *grown = realloc(console_lines, (size_t)capacity * sizeof *console_lines);
                if (!grown) return; /* Keep existing log; host serial logging remains available. */
                console_lines = grown; console_capacity = capacity;
            }
            snprintf(console_lines[console_n], CONSOLE_LINE, "[%s] #%lu @%lu.%03lu%s %.*s",
                     level ? level : "log", sequence, (unsigned long)(now / 1000),
                     (unsigned long)(now % 1000), offset ? " +" : "", (int)part, p + offset);
            console_n++;
            if (console_skip && console_skip < INT_MAX) console_skip = MIN(console_skip + 1, MAX(console_n - console_visible(), 0));
            offset += part;
        } while (offset < n);
        console_dirty = true;
        p += n;
        while (*p == '\r' || *p == '\n') p++;
    } while (*p);
}

static void redraw(void) {
    uint64_t start = debug_js ? uptime_ms() : 0;
    draw();
    uint64_t drawn = debug_js ? uptime_ms() : 0;
    win_update(w);
    if (debug_js) {
        loop_profile.draw_ms += drawn - start;
        loop_profile.update_ms += uptime_ms() - drawn;
        loop_profile.draws++;
    }
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
static void scroll_to_xy(int x, int y) {
    int old_x = scroll_x, old_y = scroll_y;
    scroll_x = x; scroll_y = y;
    clamp_scroll();
    if (old_x != scroll_x || old_y != scroll_y) {
        scroll_event_pending = true;
        sel_node = NULL;
        hover[0] = 0;
    }
}
static void scroll_to(int y) { scroll_to_xy(scroll_x,y); }

/* scrolling keys for the page; true if the event was one */
static bool scroll_key(const struct gui_event *e) {
    int ph = page_h();
    switch (e->key) {
    case NKEY_LEFT: scroll_to_xy(scroll_x - LINE,scroll_y); return true;
    case NKEY_RIGHT: scroll_to_xy(scroll_x + LINE,scroll_y); return true;
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
        if (e->mods & NMOD_SHIFT) scroll_to_xy(scroll_x + e->wheel * LINE,scroll_y);
        else scroll_to(scroll_y + e->wheel * LINE);
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
    char *url;
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
    if (strcspn(p, "?#") >= sizeof path) {
        strlcpy(f->err, "local path exceeds filesystem limit", sizeof f->err);
        return false;
    }
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
        if (path[strlen(path) - 1] != '/') {
            size_t n = strlen(f->url);
            char *url = n <= SIZE_MAX - 2 ? realloc(f->url, n + 2) : NULL;
            if (!url) { strlcpy(f->err, "directory URL allocation failed", sizeof f->err); return false; }
            f->url = url; url[n] = '/'; url[n + 1] = 0;
        }
        return true;
    }
    if (st.size >= SIZE_MAX) {
        close(fd);
        strlcpy(f->err, "file length is not representable", sizeof f->err);
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
    uint64_t start = debug_js ? uptime_ms() : 0;
    doc_h = web_layout(doc, page_w(), page_h());
    doc_w = web_doc_width(doc);
    if (debug_js) { loop_profile.layout_ms += uptime_ms() - start; loop_profile.layouts++; }
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
    if(sel_node && web_node_inert(doc,sel_node))sel_node=NULL;
    if (web_dirty(doc)) need_layout = true;
    if (need_layout) {
        relayout();
        set_title();
        need_paint = true;
    }
    web_node *invalid=NULL; const char *message=NULL; size_t length=0;
    if (web_take_validation_report(doc,&invalid,&message,&length)) {
        focus=F_PAGE;
        free(focus_value); focus_value=strdup(web_control_value(invalid));
        char visible[sizeof status]; size_t n=MIN(length,sizeof visible-1);
        for(size_t i=0;i<n;i++) visible[i]=message[i]?message[i]:' ';
        visible[n]=0; hover[0]=0; set_status(visible);
        int x,y,width,height;
        if(web_node_rect(doc,invalid,&x,&y,&width,&height) &&
           (x<scroll_x || x+width>scroll_x+page_w() || y<scroll_y || y+height>scroll_y+page_h()))
            scroll_to_xy(x<scroll_x || x+width>scroll_x+page_w()?MAX(x-16,0):scroll_x,
                         y<scroll_y || y+height>scroll_y+page_h()?MAX(y-16,0):scroll_y);
        need_paint=true;
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
static bool hist_reserve(size_t count) {
    if (count > INT_MAX || count > SIZE_MAX / sizeof *hist) return false;
    if (count <= (size_t)hist_capacity) return true;
    size_t capacity = hist_capacity ? (size_t)hist_capacity : 64;
    while (capacity < count) { if (capacity > INT_MAX / 2) { capacity = count; break; } capacity *= 2; }
    void *grown = realloc(hist, capacity * sizeof *hist);
    if (!grown) return false;
    hist = grown; hist_capacity = (int)capacity;
    return true;
}
static bool hist_append(char *copy) {
    if (!hist_reserve((size_t)(hpos + 1) + 1)) return false;
    for (int i = hpos + 1; i < nhist; i++) hist_discard(i);
    nhist = hpos + 1;
    memset(&hist[nhist], 0, sizeof hist[nhist]);
    hist[nhist].url = copy;
    hist[nhist].document = generation;
    hist[nhist].entry = ++history_serial;
    hpos = nhist++;
    return true;
}
static void hist_push(const char *url) {
    char *copy = strdup(url);
    if (!copy) { console_add("error", "History allocation failed"); return; }
    if (!hist_append(copy)) { free(copy); console_add("error", "History allocation failed"); }
}
static void history_scroll_save(void) {
    if (hpos >= 0) { hist[hpos].x = scroll_x; hist[hpos].y = scroll_y; }
}

static bool same_doc(const char *a, const char *b) {
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
    return la == lb && !strncmp(a, b, la);
}

static int anchor(const char *url) {
    const char *h = strchr(url, '#');
    if (!h || !doc) return -1;
    /* Native fragment selection owns decoding. Pass the full fragment once:
       neither a fixed scratch truncation nor a second percent-decode may
       select a different element. */
    return web_anchor_y(doc, h + 1);
}

enum { NAV_PUSH, NAV_RELOAD, NAV_HISTORY, NAV_REPLACE };

/* Resource callbacks only queue results in the document; QuickJS runs in web_tick. */
static enum webnet_kind network_kind(int kind) {
    switch (kind) {
    case WEB_RESOURCE_SCRIPT: return WEBNET_CLASSIC;
    case WEB_RESOURCE_MODULE: return WEBNET_MODULE;
    case WEB_RESOURCE_FETCH: return WEBNET_FETCH;
    case WEB_RESOURCE_REPORT: return WEBNET_REPORT;
    case WEB_RESOURCE_FRAME: return WEBNET_NAVIGATION;
    default: return WEBNET_RESOURCE;
    }
}

static void response_copy(struct web_response *out, const struct webnet_response *in, enum webnet_kind kind) {
    memset(out, 0, sizeof *out);
    out->status = in->status;
    const char *url = in->final_url ? in->final_url : "";
    if (!web_response_set_url(out, url)) {
        strlcpy(out->error, "response URL allocation failed", sizeof out->error); return;
    }
    strlcpy(out->error, in->error ? in->error : "", sizeof out->error);
    const char *headers = in->headers ? in->headers : "";
    size_t hn = strlen(headers);
    if (hn > WEB_RESPONSE_HEADERS_MAX || hn == SIZE_MAX) {
        strlcpy(out->error, "response header block exceeds browser limit", sizeof out->error);
        return;
    }
    if (hn < sizeof out->headers) memcpy(out->headers, headers, hn + 1);
    else {
        out->headers_full = malloc(hn + 1);
        if (!out->headers_full) {
            strlcpy(out->error, "out of memory copying response headers", sizeof out->error);
            return;
        }
        memcpy(out->headers_full, headers, hn + 1);
    }
    size_t limit = webnet_response_limit(kind);
    if (in->body_len > limit || (in->body_len && !in->body)) {
        strlcpy(out->error, "response body is invalid or its length is not representable", sizeof out->error);
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

/* Debug-only identity of native image bytes. Never print URLs, headers or body
   text: authenticated paths and even an image URL's basename can contain secrets. */
static void debug_image_result(const struct transfer *t, const struct webnet_response *r) {
    if (!debug_js || t->kind != WEB_RESOURCE_IMAGE) return;
    const unsigned char *bytes = r->body;
    uint32_t hash = 2166136261u;
    char magic[17] = {0};
    if (bytes) {
        for (size_t i = 0; i < r->body_len; i++) hash = (hash ^ bytes[i]) * 16777619u;
        size_t n = MIN(r->body_len, 8);
        for (size_t i = 0; i < n; i++) snprintf(magic + 2*i, 3, "%02x", bytes[i]);
    }
    printf("[web-net:image] id=%lu status=%d bytes=%lu fnv=%08x magic=%s\n",
           (unsigned long)t->resource_id, r->status, (unsigned long)r->body_len, hash, magic);
}

static void resource_completed(webnet *net, uint64_t id, uint64_t gen,
                               const struct webnet_response *result, void *opaque) {
    (void)net; (void)id;
    struct transfer *t = opaque;
    if (!t->detached && doc && gen == generation && t->generation == generation) {
        debug_image_result(t, result);
        if (debug_js && t->kind == WEB_RESOURCE_CSS)
            printf("[web-net:css] status=%d bytes=%lu error=%s url=%s\n", result->status,
                   (unsigned long)result->body_len, result->error ? result->error : "",
                   result->final_url ? result->final_url : "");
        struct web_response *r = malloc(sizeof *r);
        if (r) {
            response_copy(r, result, network_kind(t->kind));
            r->event = result->event; r->uploaded = result->uploaded;
            web_resource_loaded(doc, t->resource_id, r);
            web_response_free(r); free(r);
        } else {
            static const struct web_response failure = {.error="resource response allocation failed"};
            web_resource_loaded(doc, t->resource_id, &failure);
        }
    }
    if (result->event == WEBNET_COMPLETE || result->event == WEBNET_END) remove_transfer(t);
    else if (t->detached) webnet_resume(net, id);
}

static bool host_request(void *opaque, const struct web_request *request) {
    (void)opaque;
    if (!doc || quit) return false;
    struct transfer *t = calloc(1, sizeof *t);
    if (!t) return false;
    t->resource_id = request->id;
    t->generation = generation;
    t->kind = request->kind;
    t->keepalive = request->keepalive;
    struct webnet_request r = {
        .kind = network_kind(request->kind), .generation = generation,
        .url = request->url, .origin = request->origin ? request->origin : web_url(doc), .method = request->method,
        .headers = request->headers, .body = request->body, .body_len = request->body_len,
        .credentials = request->credentials, .force_preflight = request->force_preflight,
        .redirect_error = request->redirect_error, .same_origin = request->same_origin,
        .no_cors = request->no_cors, .no_referrer = request->no_referrer,
        .image_upgrade = request->kind == WEB_RESOURCE_IMAGE && request->image_upgrade,
        .cache_mode = request->cache_mode, .keepalive = request->keepalive,
        .fetch_group = request->fetch_group
        , .stream_response = request->stream_response
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
static void host_release_request(void *opaque, uint64_t id) {
    (void)opaque;
    for (struct transfer *t = transfers; t; t = t->next)
        if (t->generation == generation && t->resource_id == id) {
            if (t->keepalive) { t->detached = true; webnet_resume(network, t->network_id); }
            else { webnet_cancel(network, t->network_id); remove_transfer(t); }
            return;
        }
}
static void host_resume_request(void *opaque, uint64_t id) {
    (void)opaque;
    for (struct transfer *t = transfers; t; t = t->next)
        if (t->generation == generation && t->resource_id == id) { webnet_resume(network, t->network_id); return; }
}

static void cancel_document_requests(void) {
    webnet_cancel_generation(network, generation);
    struct transfer **at = &transfers;
    while (*at) {
        struct transfer *t = *at;
        /* UA/process shutdown has no independent network owner. Navigation
           retains native requests but permanently removes old JS delivery. */
        if (quit) { webnet_cancel(network,t->network_id); *at=t->next; free(t); continue; }
        if (t->generation != generation) { at=&t->next; continue; }
        if (t->keepalive) { t->detached=true; webnet_resume(network,t->network_id); at=&t->next; continue; }
        *at=t->next; free(t);
    }
}

static void queue_navigation_body(const char *url, const void *post, size_t length, const char *content_type, int mode) {
    if (!url || !*url) return;
    if (length > WEBNET_BODY_LIMIT ||
        (post && (!content_type || strlen(content_type) > UINT32_MAX - 16u || strpbrk(content_type,"\r\n")))) {
        console_add("error", "Form navigation exceeds the native request bounds"); return;
    }
    char *copy = post ? malloc(length + 1) : NULL;
    char *url_copy = strdup(url), *type_copy = post ? strdup(content_type) : NULL;
    if (!url_copy || (post && (!copy || !type_copy))) { free(url_copy); free(copy); free(type_copy); console_add("error", "Navigation allocation failed"); return; }
    if (copy) { if(length)memcpy(copy,post,length);copy[length]=0; }
    free(pending_text.p); pending_text = (struct url_text){url_copy, strlen(url_copy) + 1};
    free(pending_post);
    pending_post = copy;
    pending_post_len = post ? length : 0;
    free(pending_content_type); pending_content_type = type_copy;
    pending_mode = mode;
    pending_navigation = true;
}
static void queue_navigation(const char *url, const char *post, int mode) {
    queue_navigation_body(url,post,post?strlen(post):0,"application/x-www-form-urlencoded",mode);
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
static void host_navigate_form(void *opaque, const char *url, const void *body, size_t length, const char *content_type, const char *target) {
    (void)opaque;
    if (!url || (!has_prefix(url,"https://") && !has_prefix(url,"http://"))) {
        console_add("error","Form navigation requires an HTTP(S) destination"); return;
    }
    if (!target || !*target || !strcasecmp(target,"_self") || !strcasecmp(target,"_top") || !strcasecmp(target,"_parent")) {
        queue_navigation_body(url,body,length,content_type,NAV_PUSH); return;
    }
    if (!strcasecmp(target,"_blank") && !body) {
        char *args[]={"browser",(char *)url,NULL};int fds[]={0,1,2};
        if (spawn("/bin/browser",args,fds,0)<0)console_add("error","Unable to open the form destination window");
        return;
    }
    console_add("error","Named form destinations and POST into a new window are not yet supported");
}

static void host_console(void *opaque, int level, const char *message) {
    (void)opaque;
    console_add(level >= 2 ? "error" : level == 1 ? "warn" : "log", message);
    if (debug_js) fprintf(stderr, "[web-js:%s] %s\n", level >= 2 ? "error" : level == 1 ? "warn" : "log", message);
}

static void host_scroll(void *opaque, int *x, int *y) {
    (void)opaque;
    *x = scroll_x;
    *y = scroll_y;
}
static void host_scroll_to(void *opaque, int x, int y) {
    (void)opaque;
    if (need_layout) relayout();
    /* Author geometry can flush layout before this host callback, without a
       browser turn setting need_layout. Clamp against that current snapshot. */
    if (doc) { doc_w = web_doc_width(doc); doc_h = web_doc_height(doc); }
    scroll_to_xy(x,y);
    need_paint = true;
}
static bool host_video_present(void *opaque, web_doc *document, const struct web_video_patch *patch) {
    (void)opaque;
    if (!w || !patch || document != doc || quit || native_wait || loading || pending_navigation ||
        need_layout || need_paint || sel_node || !patch->pixels || patch->w <= 0 || patch->h <= 0 ||
        patch->pitch < patch->w ||
        patch->canvas.px != w->c.px || patch->canvas.w != w->c.w ||
        patch->canvas.h != w->c.h || patch->canvas.pitch != w->c.pitch ||
        patch->scroll_x != scroll_x || patch->scroll_y != scroll_y ||
        patch->x < 0 || patch->y < TB || patch->x > page_w() || patch->y > TB + page_h() ||
        patch->w > page_w() - patch->x || patch->h > TB + page_h() - patch->y ||
        w->c.pitch < w->c.w || w->c.h <= 0 ||
        (size_t)patch->pitch > SIZE_MAX / sizeof(uint32_t) / (size_t)patch->h ||
        (size_t)w->c.pitch > SIZE_MAX / sizeof(uint32_t) / (size_t)w->c.h) return false;
    for (int row = 0; row < patch->h; row++)
        memcpy(w->c.px + (size_t)(patch->y + row) * w->c.pitch + patch->x,
               patch->pixels + (size_t)row * patch->pitch, (size_t)patch->w * sizeof(uint32_t));
    __sync_synchronize();
    win_update_rect(w,patch->x,patch->y,patch->w,patch->h);
    return true;
}
static bool host_navigation_pending(void *opaque) {
    (void)opaque;
    return quit || pending_navigation || loading;
}
/* Report the real native pointer at dequeue, not a synthetic DOM callback.
   A long author task may defer hover delivery; diagnostics must not re-enter it. */
static void native_active_event(const struct gui_event *event) {
    if(doc && (event->type==EV_CLOSE || event->type==EV_UNFOCUS ||
       ((event->type==EV_MOUSE_UP || event->type==EV_MOUSE_MOVE) && !(event->buttons&1))))
        web_active_release(doc);
}
static int browser_event(struct gui_event *event, int timeout) {
    int result=win_event(w,event,timeout);
    /* Release/capture cancellation is native state, even while author JS is
       running. Never hit-test, dispatch, or lay out mutable DOM at dequeue. */
    if(result>0){native_active_event(event);browser_hover_boundary(&evq_hover_tail_valid,event);}
    if(result>0 && debug_js && (event->type==EV_MOUSE_MOVE ||
       event->type==EV_MOUSE_DOWN || event->type==EV_MOUSE_UP)) {
        static uint64_t sequence;
        fprintf(stderr,"[web-input] NATIVE_POINTER %lu %d %d\n",
                (unsigned long)++sequence,event->x,event->y-TB);
    }
    return result;
}
/* Only browser-owned navigation commands cancel a running page task. Keep
   the event in FIFO so the ordinary handler acts after QuickJS unwinds. */
static bool native_navigation_command(const struct gui_event *event) {
    if(event->type==EV_KEY && event->pressed) {
        uint32_t key=event->key;
        if(key>='A' && key<='Z')key+=32;
        if((key==NKEY_F5 || ((event->mods&NMOD_CTRL) && key=='r')) && cur_url[0])return true;
        if(event->mods&NMOD_ALT) {
            if(key==NKEY_LEFT && hpos>0)return true;
            if(key==NKEY_RIGHT && hpos<nhist-1)return true;
        }
    }
    if(event->type==EV_MOUSE_DOWN && (event->buttons&1) &&
       event->y<TB && !sel_node) {
        for(int i=BTN_BACK;i<=BTN_HOME;i++)
            if(ui_hit(event->x,event->y,btn_x(i),6,28,28))
                return i==BTN_BACK?hpos>0:i==BTN_FWD?hpos<nhist-1:
                       i==BTN_RELOAD?cur_url[0]!=0:true;
    }
    return false;
}
/* Keep an explicit user stop available during long author tasks without
   touching a half-mutated document or running another page callback. All
   other native events retain FIFO ownership until the outer JS task ends. */
static bool host_script_checkpoint(void *opaque) {
    (void)opaque;
    static uint64_t last;
    uint64_t now=uptime_ms();
    if(quit)return false;
    if(now>=last && now-last<10)return true;
    last=now;
    uint64_t until=now+2;
    struct gui_event e;
    while(w && uptime_ms()<until) {
        int event=browser_event(&e,0);
        if(event<0){quit=true;return false;}
        if(!event)break;
        if(e.type==EV_CLOSE){quit=true;return false;}
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_ESC)return false;
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_F12) {
            int old_top=TB+page_h(),old_height=console_h();
            console_open=!console_open;
            if(console_open)focus=F_CONSOLE;else if(focus==F_CONSOLE)focus=F_PAGE;
            need_layout=need_paint=true;
            /* Chrome only: never read the document's half-built layout here.
               The exposed page area is restored after the task unwinds. */
            if(console_open)draw_console();
            else gfx_fill(&w->c,0,old_top,w->w,old_height,UI_BG);
            win_update(w);
            continue;
        }
        if(browser_coalesce_hover(evq,evq_n,&e,evq_hover_tail_valid))continue;
        if(evq_n==evq_capacity) {
            int capacity=evq_capacity?(evq_capacity>INT_MAX/2?INT_MAX:evq_capacity*2):64;
            void *grown=evq_n<INT_MAX && (size_t)capacity<=SIZE_MAX/sizeof *evq?
                realloc(evq,(size_t)capacity*sizeof *evq):NULL;
            if(!grown){
                if(!evq_failure_logged){console_add("error","Deferred native event storage allocation failed");evq_failure_logged=true;}
                return false;
            }
            evq=grown;evq_capacity=capacity;
        }
        evq[evq_n++]=e;evq_hover_tail_valid=browser_hover_tail(&e);
        if(native_navigation_command(&e))return false;
    }
    return !quit;
}
static unsigned host_window_state(void *opaque) {
    (void)opaque;int state=win_state(w);
    if(state<0)return 0;
    return ((state&WIN_STATE_VISIBLE)?WEB_WINDOW_VISIBLE:0) |
           ((state&WIN_STATE_FOCUSED) && focus==F_PAGE?WEB_WINDOW_FOCUSED:0);
}
static bool host_cookie_enabled(void *opaque) {
    (void)opaque;
    /* webnet_create succeeds only with an actual session cookie jar. */
    return network != NULL;
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
        if (!url || (len && !data)) return false;
        char *copy = strdup(url); void *saved = len ? malloc(len) : NULL;
        if (!copy || (len && !saved)) { free(copy); free(saved); return false; }
        char *current, *address;
        if (!url_pair_prepare(url, &current, &address)) { free(copy); free(saved); return false; }
        if (len) memcpy(saved, data, len);
        if ((operation == WEB_HISTORY_PUSH && !hist_reserve((size_t)(hpos + 1) + 1)) ||
            !web_set_url(doc, url)) { free(copy); free(saved); free(current); free(address); return false; }
        bool manual = hist[hpos].manual;
        history_scroll_save();
        if (operation == WEB_HISTORY_PUSH) { if (!hist_append(copy)) { free(copy); free(saved); free(current); free(address); return false; } }
        else { free(hist[hpos].url); free(hist[hpos].state); hist[hpos].url = copy; hist[hpos].entry = ++history_serial; }
        hist[hpos].state = saved; hist[hpos].state_len = len; hist[hpos].manual = manual;
        history_scroll_save();
        url_pair_commit(current, address);
        need_paint = true;
    } else if (operation == WEB_HISTORY_GO) {
        if (history_queue_count == UINT_MAX) return false;
        struct browser_history_task *task = malloc(sizeof *task); if (!task) return false;
        *task = (struct browser_history_task){ .delta=value, .generation=generation };
        if (history_queue_tail) history_queue_tail->next=task; else history_queue=task;
        history_queue_tail=task; history_queue_count++;
    }
    else if (operation == WEB_HISTORY_SCROLL) hist[hpos].manual = value != 0;
    else if (operation != WEB_HISTORY_INFO) return false;
    *out = (struct web_history){.entry=hist[hpos].entry, .length=nhist, .manual_scroll=hist[hpos].manual,
        .state=hist[hpos].state, .state_len=hist[hpos].state_len};
    return true;
}

static void handle_native_wait(const struct gui_event *e);
struct synchronous_load { bool ready; enum webnet_kind kind; web_doc *document; uint64_t generation; struct web_response response; };
static void synchronous_completed(webnet *net, uint64_t id, uint64_t gen,
                                  const struct webnet_response *r, void *opaque) {
    (void)net; (void)id;
    struct synchronous_load *s = opaque;
    if (gen == s->generation && gen == generation && doc == s->document && !pending_navigation && !quit)
        response_copy(&s->response, r, s->kind);
    else { memset(&s->response, 0, sizeof s->response); strcpy(s->response.error, "document was replaced"); }
    s->ready = true;
}

static bool host_sync_request(void *opaque, const struct web_request *request, struct web_response *response) {
    (void)opaque;
    memset(response, 0, sizeof *response);
    if (!request || request->kind != WEB_RESOURCE_MODULE || !request->origin || !*request->origin ||
        !request->url || !doc || native_wait || quit || pending_navigation) {
        strcpy(response->error, "module request has no active initiating document"); return false;
    }
    struct synchronous_load *s = calloc(1, sizeof *s);
    if (!s) { strcpy(response->error, "module response allocation failed"); return false; }
    s->kind = network_kind(request->kind); s->document = doc; s->generation = generation;
    struct webnet_request r = {
        .kind = s->kind, .generation = s->generation, .url = request->url,
        .origin = request->origin, .method = "GET", .credentials = request->credentials,
        .redirect_error = request->redirect_error, .same_origin = request->same_origin,
        .cache_mode = request->cache_mode
    };
    uint64_t id = webnet_submit(network, &r, synchronous_completed, s);
    if (!id) { free(s); strcpy(response->error, "module request could not be started"); return false; }
    native_wait = true;
    sync_stopped = false;
    prepare_native_snapshot(false);
    if (need_paint && !quit) redraw();
    uint64_t started = uptime_ms();
    while (!s->ready && !quit && !sync_stopped && !pending_navigation &&
           doc == s->document && generation == s->generation) {
        uint64_t now = uptime_ms();
        if (now - started >= WEBNET_TIMEOUT_MS) { sync_stopped = true; break; }
        webnet_pump(network, now);
        if (s->ready) break;
        struct gui_event e;
        int timeout = webnet_timeout(network, now);
        int event = browser_event(&e, timeout < 0 ? 10 : MIN(timeout, 10));
        if (event < 0) { quit = true; break; }
        if (event) handle_native_wait(&e);
        if (console_dirty && console_open) redraw();
    }
    native_wait = false;
    if (!s->ready || quit || sync_stopped || pending_navigation || doc != s->document || generation != s->generation) {
        webnet_cancel(network, id);
        web_response_free(&s->response);
        free(s);
        strcpy(response->error, quit ? "window closed" : pending_navigation ? "navigation canceled module load" : "module load canceled");
        return false;
    }
    *response = s->response;
    free(s);
    return !response->error[0] && response->status >= 200 && response->status < 300;
}
static bool host_sync_load(void *opaque, const char *url, int kind, struct web_response *response) {
    struct web_request request = {.kind=kind, .url=url, .origin=doc?web_url(doc):"", .credentials=WEBNET_CREDENTIALS_SAME_ORIGIN};
    return host_sync_request(opaque, &request, response);
}

static const struct web_host browser_host = {
    .request = host_request, .cancel = host_cancel, .release_request = host_release_request, .resume_request = host_resume_request, .sync_load = host_sync_load,
    .sync_request = host_sync_request,
    .navigate = host_navigate, .console = host_console, .scroll = host_scroll,
    .navigate_form = host_navigate_form,
    .scroll_to = host_scroll_to, .history = host_history,
    .window_state=host_window_state,
    .navigation_pending=host_navigation_pending,
    .script_checkpoint=host_script_checkpoint,
    .video_present=host_video_present,
    .cookie_get = host_cookie_get, .cookie_set = host_cookie_set,
    .cookie_enabled = host_cookie_enabled
    , .navigate_mode = host_navigate_mode
    , .storage = host_storage, .storage_events = true
    , .media_range = true
};

static void navigation_completed(webnet *net, uint64_t id, uint64_t gen,
                                 const struct webnet_response *result, void *opaque) {
    (void)net; (void)opaque;
    if (id != navigation_id || gen != navigation_generation) return;
    navigation_id = 0;
    /* This callback observes the complete response, never its first byte.
       Generation/id checks above prevent a canceled request changing timing. */
    if (!result->error[0] && result->status > 0) {
        navigation_timing.response_end_ms = uptime_ms();
        navigation_timing.response_end_valid = true;
    }
    web_response_free(&navigation_response);
    response_copy(&navigation_response, result, WEBNET_NAVIGATION);
    navigation_ready = true;
}

static void stop_navigation(void) {
    if (navigation_id) webnet_cancel(network, navigation_id);
    navigation_id = 0;
    navigation_ready = false;
    web_response_free(&navigation_response);
    memset(&navigation_response, 0, sizeof navigation_response);
    memset(&navigation_timing, 0, sizeof navigation_timing);
    loading = false;
}

static void finish_navigation(void) {
    if (!navigation_ready || native_wait || quit) return;
    navigation_ready = false;
    struct web_response *f = malloc(sizeof *f);
    if (!f) {
        web_response_free(&navigation_response);
        loading = false; set_status("Out of memory loading page"); return;
    }
    *f = navigation_response;
    memset(&navigation_response, 0, sizeof navigation_response);
    char ctype[128] = "", charset_buf[64];
    const char *charset = NULL, *image = NULL;
    struct http_resp headers = {0};
    /* Borrow the complete block solely for lookup; do not free this view. */
    headers.headers_full = (char *)web_response_headers(f);
    http_header(&headers, "Content-Type", ctype, sizeof ctype);
    char *html = NULL;
    size_t hlen = 0;
    if (f->error[0]) html = error_page(navigation_url, f->error);
    else if (has_prefix(web_response_url(f), "about:"))
        html = strdup(!strcasecmp(web_response_url(f), HOME) ? home_page : "<!doctype html><title>blank</title>");
    else {
        bool sniff_html = !ctype[0] && f->body_len && memchr(f->body, '<', MIN(f->body_len, 512));
        if (has_prefix(ctype, "text/html") || has_prefix(ctype, "application/xhtml") || sniff_html) {
            html = f->body; hlen = f->body_len; f->body = NULL;
            const char *cs = strstr(ctype, "charset=");
            if (cs) {
                cs += 8; if (*cs == '"') cs++;
                size_t n = MIN(strcspn(cs, "\"; "), sizeof charset_buf - 1);
                memcpy(charset_buf, cs, n); charset_buf[n] = 0; charset = charset_buf;
            }
        } else if (has_prefix(ctype, "image/")) { html = image_page(web_response_url(f)); image = web_response_url(f); }
        else if (has_prefix(ctype, "text/") || !ctype[0] || strstr(ctype, "json") ||
                 strstr(ctype, "javascript") || strstr(ctype, "xml")) html = text_page(web_response_url(f), f->body, f->body_len);
        else {
            char message[256];
            snprintf(message, sizeof message, "Cannot display %s (%lu bytes).", ctype, (unsigned long)f->body_len);
            html = error_page(web_response_url(f), message);
        }
    }
    if (!html) { web_response_free(f); free(f); loading = false; set_status("Out of memory loading page"); return; }
    if (!hlen) hlen = strlen(html);
    const char *final_url = web_response_url(f)[0] ? web_response_url(f) : navigation_url;
    struct web_host document_host = browser_host;
    document_host.debug_js = debug_js;
    document_host.navigation_timing = navigation_timing;
    document_host.js_task_budget_ms = js_task_budget_ms;
    web_doc *nd = web_live_response(html, hlen, final_url, charset, web_response_headers(f), &document_host);
    free(html);
    if (!nd) { web_response_free(f); free(f); loading = false; set_status("Out of memory creating document"); return; }

    char *current, *address;
    if (!url_pair_prepare(web_url(nd), &current, &address)) {
        web_free(nd); web_response_free(f); free(f); loading = false;
        set_status("Out of memory retaining document URL"); return;
    }
    cancel_document_requests();
    if (doc) web_free(doc);
    doc = nd;
    generation = navigation_generation;
    evq_n = 0;evq_hover_tail_valid=false;
    url_pair_commit(current, address);
    if (navigation_mode == NAV_PUSH) hist_push(cur_url);
    else if (hpos >= 0) {
        char *copy = strdup(cur_url);
        if (copy) { free(hist[hpos].url); hist[hpos].url = copy; }
        if (navigation_mode == NAV_REPLACE) {
            free(hist[hpos].state); hist[hpos].state = NULL; hist[hpos].state_len = 0;
            hist[hpos].document = generation; hist[hpos].entry = ++history_serial; hist[hpos].x = hist[hpos].y = 0;
        } else {
            uint64_t previous = hist[hpos].document;
            for (int i = 0; i < nhist; i++) if (hist[i].document == previous) hist[i].document = generation;
        }
    }
    int keep = navigation_mode == NAV_PUSH || hpos < 0 || hist[hpos].manual ? 0 : hist[hpos].y;
    int keep_x = navigation_mode == NAV_PUSH || hpos < 0 || hist[hpos].manual ? 0 : hist[hpos].x;
    scroll_x = scroll_y = 0; scroll_event_pending = false;
    find_y = -1; sel_node = pressed_node = hover_node = NULL; hover[0] = 0;
    free(focus_value); focus_value = NULL;
    refresh_at = 0; refresh_seen = false;
    initial_scroll_pending = (hpos < 0 || !hist[hpos].manual) && (navigation_mode != NAV_PUSH || strchr(cur_url, '#') != NULL);
    initial_scroll_anchor = navigation_mode == NAV_PUSH || navigation_mode == NAV_REPLACE;
    initial_scroll_x = keep_x; initial_scroll_y = keep;
    relayout();
    if (image) {
        for (int i = 0; i < web_image_count(doc); i++)
            if (!strcmp(web_image_url(doc, i), image)) web_image_loaded(doc, i, f->body, f->body_len);
        relayout();
    }
    web_response_free(f); free(f);
    int ay = anchor(cur_url);
    scroll_to_xy(keep_x,navigation_mode != NAV_PUSH ? keep : ay >= 0 ? ay : 0);
    loading = false;
    status[0] = 0;
    set_title();
    if (navigation_mode == NAV_HISTORY) web_history_event(doc, cur_url, true);
    /* A history listener can retire SVG snapshots even after relayout. */
    prepare_native_snapshot(false);
    redraw();
}

static void navigate_inner(const char *url, const void *post, size_t length, const char *headers, int mode) {
    if (storage && webstorage_pending(storage)) {
        struct web_storage_result result;
        if (webstorage_flush(storage, true, &result) != WEB_STORAGE_OK)
            console_add("error", "Local storage flush failed; changes remain in RAM.");
    }
    if (hpos >= 0 && mode != NAV_HISTORY) history_scroll_save();
    sel_node = NULL; hover[0] = 0; refresh_at = 0;
    if (focus == F_ADDR) focus = F_PAGE;
    if (!post && doc && mode != NAV_RELOAD &&
        ((mode == NAV_HISTORY && hpos >= 0 && hist[hpos].document == generation) ||
         ((strchr(url, '#') || strchr(cur_url, '#')) && same_doc(url, cur_url)))) {
        char *old_url = strdup(cur_url);
        char *current, *address;
        if (!old_url || !url_pair_prepare(url, &current, &address)) { free(old_url); console_add("error", "Navigation allocation failed"); return; }
        if (!web_set_url(doc, url)) { free(old_url); free(current); free(address); return; }
        int y = anchor(url);
        if (mode == NAV_PUSH) hist_push(url);
        else if (mode == NAV_REPLACE && hpos >= 0) {
            char *copy = strdup(url);
            if (copy) { free(hist[hpos].url); hist[hpos].url = copy; hist[hpos].entry = ++history_serial; }
        }
        url_pair_commit(current, address);
        if (mode == NAV_PUSH || mode == NAV_REPLACE) { if (y >= 0) scroll_to(y); else if (!strchr(url, '#') || !strchr(url, '#')[1]) scroll_to(0); }
        else if (mode == NAV_HISTORY && hpos >= 0 && !hist[hpos].manual) scroll_to_xy(hist[hpos].x,hist[hpos].y);
        web_history_event(doc, old_url, mode == NAV_HISTORY);
        free(old_url);
        /* popstate/hashchange run author code: never paint its retired boxes. */
        prepare_native_snapshot(false);
        redraw(); return;
    }
    if (!url_text_set(&navigation_text, url)) { console_add("error", "Navigation URL allocation failed"); return; }
    stop_navigation();
    navigation_generation = ++next_generation;
    navigation_timing.valid = true;
    navigation_timing.navigation_start_ms = uptime_ms();
    navigation_mode = mode;
    loading = true;
    char what[256]; snprintf(what, sizeof what, "Loading %.240s", url); set_status(what);
    draw_toolbar(); win_update_rect(w, 0, 0, w->w, TB);
    if (has_prefix(url, "about:") || has_prefix(url, "file://")) {
        navigation_timing.fetch_start_ms = uptime_ms();
        navigation_timing.fetch_valid = true;
        struct fetched *local = calloc(1, sizeof *local);
        if (!local) { strcpy(navigation_response.error, "navigation allocation failed"); navigation_ready = true; return; }
        local->url = strdup(url);
        if (!local->url) { free(local); strcpy(navigation_response.error, "navigation URL allocation failed"); navigation_ready = true; return; }
        if (has_prefix(url, "file://") && !read_local(url, local)) {
            strlcpy(navigation_response.error, local->err, sizeof navigation_response.error);
        } else {
            navigation_response.status = local->status ? local->status : 200;
            navigation_response.body = local->body;
            navigation_response.body_len = local->len;
            snprintf(navigation_response.headers, sizeof navigation_response.headers, "Content-Type: %s\r\n", local->ctype);
        }
        if (!navigation_response.error[0]) {
            navigation_timing.response_end_ms = uptime_ms();
            navigation_timing.response_end_valid = true;
        }
        if (!web_response_set_url(&navigation_response, local->url))
            strlcpy(navigation_response.error, "navigation URL allocation failed", sizeof navigation_response.error);
        if (local->body != navigation_response.body) free(local->body);
        free(local->url); free(local);
        navigation_ready = true; return;
    }
    struct webnet_request rq = {
        .kind = WEBNET_NAVIGATION, .generation = navigation_generation, .url = url,
        .origin = cur_url, .method = post ? "POST" : "GET",
        .headers = post ? headers : NULL,
        .body = post, .body_len = post ? length : 0, .user_navigation = true,
        .credentials = WEBNET_CREDENTIALS_INCLUDE
    };
    navigation_timing.fetch_start_ms = uptime_ms();
    navigation_timing.fetch_valid = true;
    navigation_id = webnet_submit(network, &rq, navigation_completed, NULL);
    if (!navigation_id) {
        strcpy(navigation_response.error, "request could not be started");
        (void)web_response_set_url(&navigation_response, url);
        navigation_ready = true;
    }
}
/* The wire length is uint32_t; no shorter browser metadata ceiling.
   Headers are copied synchronously by webnet_submit before this owner retires. */
static char *navigation_post_headers(const char *content_type) {
    if(!content_type || strpbrk(content_type,"\r\n"))return NULL;
    size_t n=strlen(content_type);
    if(n>UINT32_MAX-16u || n>SIZE_MAX-17u)return NULL;
    char *headers=malloc(n+17);if(!headers)return NULL;
    memcpy(headers,"Content-Type: ",14);memcpy(headers+14,content_type,n);
    memcpy(headers+14+n,"\r\n",3);return headers;
}
static void navigate_body(const char *url_in, const void *post, size_t length, const char *content_type, int mode) {
    if (!url_in) return;
    if (native_wait || (doc && web_script_running(doc))) { queue_navigation_body(url_in,post,length,content_type,mode); return; }
    char *url = strdup(url_in); /* input can alias current URL or history */
    if (!url) { console_add("error", "Navigation allocation failed"); return; }
    char *headers=post?navigation_post_headers(content_type):NULL;
    if(post&&!headers){free(url);console_add("error","Form request header is invalid or could not be allocated");return;}
    navigate_inner(url, post, length, headers, mode);
    free(headers); free(url);
}
static void navigate(const char *url_in, const char *post, int mode) {
    navigate_body(url_in,post,post?strlen(post):0,"application/x-www-form-urlencoded",mode);
}

static void apply_pending_navigation(void) {
    if (!pending_navigation || native_wait || quit || (doc && web_script_running(doc))) return;
    char *url = pending_text.p;
    pending_text = (struct url_text){0};
    char *post = pending_post; int mode = pending_mode; size_t length=pending_post_len;
    char *content_type=pending_content_type; pending_content_type=NULL;
    pending_post = NULL; pending_navigation = false;
    navigate_body(url, post, length, content_type, mode);
    free(url);
    free(post); free(content_type);
}


static char *address_destination(const char *text) {
    const char *prefix = ""; bool search = false;
    if (strstr(text, "://") || has_prefix(text, "about:")) {}
    else if (text[0] == '/') prefix = "file://";
    else if (strchr(text, ' ') || !strchr(text, '.')) { prefix = SEARCH; search = true; }
    else prefix = "https://";
    size_t n = strlen(prefix);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        size_t add = !search || isalnum(*p) || strchr("-_.~ ", *p) ? 1 : 3;
        if (n == SIZE_MAX || add > SIZE_MAX - n - 1) return NULL;
        n += add;
    }
    char *url = malloc(n + 1); if (!url) return NULL;
    size_t used = strlen(prefix); memcpy(url, prefix, used);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!search || isalnum(*p) || strchr("-_.~", *p)) url[used++] = (char)*p;
        else if (*p == ' ') url[used++] = '+';
        else { url[used++] = '%'; url[used++] = hex[*p >> 4]; url[used++] = hex[*p & 15]; }
    }
    url[used] = 0; return url;
}
static void go_addr(const char *text) {
    while (*text == ' ') text++;
    if (!*text) return;
    char *url = address_destination(text);
    if (!url) { console_add("error", "Navigation allocation failed"); return; }
    navigate(url, NULL, NAV_PUSH); free(url);
}

static void go_history(int d) {
    int64_t p = (int64_t)hpos + d;
    if (p < 0 || p >= nhist) return;
    history_scroll_save();
    hpos = (int)p;
    navigate(hist[hpos].url, NULL, NAV_HISTORY);
}
static void apply_pending_history(void) {
    if (!history_queue_count || loading || native_wait || quit || (doc && web_script_running(doc))) return;
    struct browser_history_task *task = history_queue;
    history_queue=task->next; if (!history_queue) history_queue_tail=NULL; history_queue_count--;
    int delta = task->delta; uint64_t document_generation = task->generation; free(task);
    if (document_generation != generation) return;
    if (!delta) navigate(cur_url, NULL, NAV_RELOAD);
    else go_history(delta);
}

static void submit(web_node *n) {
    if (!web_sandbox_form_submission_allowed(doc, n)) return;
    web_node *form = web_form_owner(doc, n);
    if(!form || !web_form_submission_validate(doc,n)) {flush_dom_layout();return;}
    struct web_event event = {.type = "submit", .bubbles = true, .cancelable = true};
    struct web_hit action={0};
    if(web_node_action(doc,n,&action) && action.kind==WEB_HIT_SUBMIT)event.submitter=n;
    bool allowed = form && web_dispatch(doc, form, &event);
    flush_dom_layout();
    if (!allowed) return;
    if (web_js_dialog_submit(doc,n) != 0) {flush_dom_layout();return;}
    struct web_form_request request;
    if (!web_submit_request(doc,n,&request)) return;
    web_autocomplete_record(doc,n,request.url);
    host_navigate_form(NULL,request.url,request.body,request.body_len,request.content_type,request.target);
    web_submit_request_free(&request);
}

/* ---------------------------------------------------------------- input */
static void page_click(web_node *target, int x, int y, bool keyboard);
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

static bool dispatch_native_mode(const char *type, web_node *target, const struct gui_event *e, bool cancelable, bool keyboard) {
    if (!doc || native_wait) return true;
    char key_text[8];
    struct web_event event = {
        .type = type, .bubbles = true, .cancelable = cancelable,
        .x = e ? e->x : 0, .y = e ? e->y - TB : 0,
        .button = e && (e->buttons & 2) ? 2 : 0,
        .buttons = e ? e->buttons : 0,
        .delta_y = e && !strcmp(type,"wheel") ? e->wheel * LINE : 0,
        .key = e ? event_key(e->key, key_text) : "",
        .key_code = e ? event_key_code(e->key) : 0,
        .ctrl = e && (e->mods & NMOD_CTRL), .shift = e && (e->mods & NMOD_SHIFT),
        .alt = e && (e->mods & NMOD_ALT), .keyboard_activation = keyboard
    };
    bool allowed = web_dispatch(doc, target, &event);
    flush_dom_layout();
    return allowed;
}
static bool dispatch_native(const char *type, web_node *target, const struct gui_event *e, bool cancelable) {
    return dispatch_native_mode(type,target,e,cancelable,false);
}
static web_node *native_capture_target(const struct gui_event *e) {
    if(!doc || native_wait || !e)return NULL;
    struct web_event event={.x=e->x,.y=e->y-TB,.button=e->type==EV_MOUSE_MOVE?-1:0,.buttons=e->buttons,
        .ctrl=(e->mods&NMOD_CTRL)!=0,.shift=(e->mods&NMOD_SHIFT)!=0,.alt=(e->mods&NMOD_ALT)!=0};
    return web_pointer_capture_target(doc,&event);
}

static void page_focus(web_node *node) {
    if (!doc) return;
    web_node *old = web_focused(doc);
    web_focus(doc,node);
    node=web_focused(doc);
    if (old == node) return;
    if (old) {
        const char *value = web_control_value(old);
        if (focus_value && value && strcmp(focus_value, value)) dispatch_native("change", old, NULL, false);
        struct web_event blur = {.type = "blur", .related_target=node};
        web_dispatch(doc, old, &blur);
        flush_dom_layout();
        blur.type="focusout";blur.bubbles=true;web_dispatch(doc,old,&blur);flush_dom_layout();
    }
    if (web_focused(doc)!=node) return; /* a blur/focusout listener chose another control */
    free(focus_value);
    const char *value = node ? web_control_value(node) : NULL;
    focus_value = value ? strdup(value) : NULL;
    if (node) {
        struct web_event event = {.type = "focus", .related_target=old};
        web_dispatch(doc, node, &event);
        flush_dom_layout();
        if(web_focused(doc)==node){event.type="focusin";event.bubbles=true;web_dispatch(doc,node,&event);flush_dom_layout();}
    }
}

static void open_select(web_node *n) {
    int sel = 0;
    sel_datalist = sel_autocomplete = false;
    int needed = web_select_options(doc, n, NULL, 0, &sel);
    if (!needed) { needed = web_datalist_options(doc, n, NULL, 0); sel_datalist = needed > 0; }
    if (needed < 0) return;
    int capacity = MAX(needed, 128); /* seed for native saved-value suggestions, not an option ceiling */
    if (capacity > sel_capacity) {
        if ((size_t)capacity > SIZE_MAX / sizeof *sel_labels) return;
        void *grown = realloc(sel_labels, (size_t)capacity * sizeof *sel_labels);
        if (!grown) { console_add("error", "Control option storage allocation failed"); return; }
        sel_labels = grown; sel_capacity = capacity;
    }
    sel_n = sel_datalist ? web_datalist_options(doc, n, sel_labels, sel_capacity) :
        web_select_options(doc, n, sel_labels, sel_capacity, &sel);
    if (!sel_n) {
        sel_n = web_autocomplete_options(doc, n, sel_labels, sel_capacity);
        sel_autocomplete = sel_datalist = sel_n > 0;
    }
    if (sel_n <= 0 || (needed && sel_n != needed)) return;
    int x, y, ww, h;
    if (!web_node_rect(doc, n, &x, &y, &ww, &h)) return;
    sel_node = n;
    sel_cur = sel < 0 ? 0 : sel;
    sel_x = x - scroll_x;
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
        if (!sel_datalist || (sel_autocomplete ? web_autocomplete_choose(doc, sel_node, i) : web_datalist_choose(doc, sel_node, i))) {
            if (!sel_datalist) web_select_set(doc, sel_node, i);
            dispatch_native("input", sel_node, NULL, false);
            if (!sel_datalist) dispatch_native("change", sel_node, NULL, false);
        }
    }
    sel_node = NULL;
    relayout();
}

static void addr_focus(void) {
    if (!url_text_set(&address_text, cur_url)) { console_add("error", "Address allocation failed"); return; }
    focus = F_ADDR;
    acur = strlen(addr);
    addr_all = true;
}

static void addr_insert(const char *s) {
    size_t n = strlen(s), len = addr_all ? 0 : strlen(addr);
    if (len == SIZE_MAX || n > SIZE_MAX - len - 1 || !url_text_reserve(&address_text, len + n + 1)) {
        console_add("error", "Address allocation failed"); return;
    }
    if (!len) addr[0] = 0;
    if (addr_all) { addr[0] = 0; acur = 0; addr_all = false; }
    memmove(addr + acur + n, addr + acur, len - (size_t)acur + 1);
    memcpy(addr + acur, s, n); acur += n;
}

static void addr_key(const struct gui_event *e) {
    if (!address_text.p && !url_text_set(&address_text, "")) { console_add("error", "Address allocation failed"); return; }
    size_t len = strlen(addr);
    bool ctrl = e->mods & NMOD_CTRL;
    if (e->key == NKEY_ENTER) {
        focus = F_PAGE;
        go_addr(addr);
        return;
    }
    if (e->key == NKEY_ESC) {
        focus = F_PAGE;
        return;
    }
    if (ctrl && (e->key == 'a' || e->key == 'A')) addr_all = true;
    else if (ctrl && (e->key == 'c' || e->key == 'C')) clipboard_set(addr, strlen(addr));
    else if (ctrl && (e->key == 'v' || e->key == 'V')) {
        int needed = clipboard_get(NULL, 0);
        if (needed < 0 || needed == INT_MAX) { console_add("error", "Clipboard address length is not representable"); return; }
        char *clip = malloc((size_t)needed + 1);
        if (!clip) { console_add("error", "Address allocation failed"); return; }
        int n = clipboard_get(clip, needed + 1);
        if (n >= 0 && n <= needed) {
            clip[n] = 0;
            for (char *p = clip; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            addr_insert(clip);
        } else console_add("error", "Clipboard changed while copying address");
        free(clip);
    } else if (e->key == NKEY_BACKSPACE || e->key == NKEY_DELETE) {
        if (addr_all) {
            addr[0] = 0;
            acur = 0;
            addr_all = false;
        } else {
            size_t start = acur, end = acur;
            if (e->key == NKEY_BACKSPACE && start > 0) {
                start--; while (start > 0 && ((unsigned char)addr[start] & 0xc0) == 0x80) start--;
            } else if (e->key == NKEY_DELETE && end < len) {
                end++; while (end < len && ((unsigned char)addr[end] & 0xc0) == 0x80) end++;
            }
            memmove(addr + start, addr + end, (size_t)(len - end + 1)); acur = start;
        }
    } else if (e->key == NKEY_LEFT || e->key == NKEY_HOME) {
        acur = addr_all || e->key == NKEY_HOME ? 0 : acur ? acur - 1 : 0;
        while (acur > 0 && ((unsigned char)addr[acur] & 0xc0) == 0x80) acur--;
        addr_all = false;
    } else if (e->key == NKEY_RIGHT || e->key == NKEY_END) {
        acur = addr_all || e->key == NKEY_END ? len : acur < len ? acur + 1 : len;
        while (acur < len && ((unsigned char)addr[acur] & 0xc0) == 0x80) acur++;
        addr_all = false;
    } else if (e->key >= 32 && e->key < 127 && !ctrl) {
        char s[2] = {(char)e->key, 0};
        addr_insert(s);
    }
}

static void find_next(int from) {
    if (!doc) return;
    int result = web_find(doc, find_buf, from);
    find_error = result == -2;
    if (find_error) { console_add("error", "Search query allocation failed"); return; }
    find_y = result;
    if (find_y >= 0 && (find_y < scroll_y || find_y > scroll_y + page_h() - 40)) scroll_to(find_y - page_h() / 3);
}

static bool find_insert(const char *text) {
    size_t n = strlen(text), len = find_all ? 0 : strlen(find_buf);
    if (len == SIZE_MAX || n > SIZE_MAX - len - 1 || !url_text_reserve(&find_text, len + n + 1)) {
        find_error = true; console_add("error", "Search input allocation failed"); return false;
    }
    memcpy(find_text.p + len, text, n + 1);
    find_all = false; return true;
}

static void find_key(const struct gui_event *e) {
    size_t len = strlen(find_buf);
    bool ctrl = e->mods & NMOD_CTRL;
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
    if (ctrl && (e->key == 'a' || e->key == 'A')) { find_all = true; return; }
    if (ctrl && (e->key == 'c' || e->key == 'C' || e->key == 'x' || e->key == 'X')) {
        if (clipboard_set(find_buf, len) < 0) { console_add("error", "Search clipboard copy failed"); return; }
        if (e->key == 'c' || e->key == 'C') return;
        if (find_text.p) find_text.p[0] = 0;
        find_all = false;
    } else if (ctrl && (e->key == 'v' || e->key == 'V')) {
        int needed = clipboard_get(NULL, 0);
        if (needed < 0 || needed == INT_MAX) { console_add("error", "Search clipboard length is not representable"); return; }
        char *clip = malloc((size_t)needed + 1);
        if (!clip) { find_error = true; console_add("error", "Search clipboard allocation failed"); return; }
        int n = clipboard_get(clip, needed + 1);
        if (n < 0 || n > needed) { console_add("error", "Clipboard changed while copying search text"); free(clip); return; }
        clip[n] = 0;
        bool inserted = find_insert(clip); free(clip);
        if (!inserted) return;
    } else if (e->key == NKEY_BACKSPACE || (e->key == NKEY_DELETE && find_all)) {
        if (len && find_text.p) {
            size_t start = find_all ? 0 : len - 1;
            while (start && ((unsigned char)find_text.p[start] & 0xc0) == 0x80) start--;
            find_text.p[start] = 0;
        }
        find_all = false;
    } else if (e->key >= 32 && e->key < 127 && !ctrl) {
        char text[2] = {(char)e->key, 0};
        if (!find_insert(text)) return;
    } else return;
    find_next(find_y >= 0 ? find_y : scroll_y);
}

static void console_insert(const char *text) {
    size_t n=strlen(text),length=console_selected?0:strlen(console_input);
    if(n>SIZE_MAX-length-1)return;
    size_t needed=length+n+1;
    if (needed > console_input_capacity) {
        size_t capacity=console_input_capacity > 64 ? console_input_capacity : 64;
        while(capacity<needed){if(capacity>SIZE_MAX/2){capacity=needed;break;}capacity*=2;}
        char *grown=realloc(console_input,capacity);
        if(!grown){console_add("error","Console input allocation failed");return;}
        console_input=grown;console_input_capacity=capacity;
    }
    if(console_selected){console_input[0]=0;console_cursor=0;console_selected=false;}
    memmove(console_input+console_cursor+n,console_input+console_cursor,length-console_cursor+1);
    memcpy(console_input+console_cursor,text,n);console_cursor+=n;console_dirty=true;
}
static void console_key(const struct gui_event *e) {
    uint32_t k=e->key;bool ctrl=e->mods&NMOD_CTRL;
    if(ctrl&&k>='A'&&k<='Z')k+=32;
    size_t length=strlen(console_input);
    if(k==NKEY_PGUP){console_scroll(console_visible());return;}
    if(k==NKEY_PGDN){console_scroll(-console_visible());return;}
    if(ctrl&&k==NKEY_HOME){console_scroll(INT_MAX);return;}
    if(ctrl&&k==NKEY_END){console_scroll(-INT_MAX);return;}
    if(ctrl&&k=='c'&&(e->mods&NMOD_SHIFT)){
        bool copied=console_copy_log();console_skip=0;
        console_add(copied?"log":"error",copied?"Retained console log copied to Nocturne clipboard.":"Console log copy failed.");return;
    }
    if(k==NKEY_ENTER){if(length){
        console_skip=0;
        console_add("input",console_input);
        if(!web_console_eval(doc,console_input,length))console_add("error","Console execution failed or the page runtime is unavailable.");
        console_input[0]=0;console_cursor=0;console_selected=false;
        need_layout=true;
    }}else if(k==NKEY_ESC){focus=F_PAGE;}
    else if(ctrl&&k=='a')console_selected=true;
    else if(ctrl&&k=='c')clipboard_set(console_input,length);
    else if(ctrl&&k=='v'){
        char dummy; int needed=clipboard_get(&dummy,0);
        char *text=needed>0?malloc((size_t)needed+1):NULL;
        if(text){int n=clipboard_get(text,(size_t)needed+1);
            if(n>0&&n<=needed){text[n]=0;for(char *p=text;*p;p++)if(*p=='\r'||*p=='\n')*p=' ';console_insert(text);}
            free(text);
        }
    }
    else if(k==NKEY_LEFT||k==NKEY_HOME){
        if(k==NKEY_HOME||console_selected)console_cursor=0;
        else if(console_cursor){console_cursor--;while(console_cursor&&((unsigned char)console_input[console_cursor]&0xc0)==0x80)console_cursor--;}
        console_selected=false;
    }else if(k==NKEY_RIGHT||k==NKEY_END){
        if(k==NKEY_END||console_selected)console_cursor=length;
        else if(console_cursor<length){console_cursor++;while(console_cursor<length&&((unsigned char)console_input[console_cursor]&0xc0)==0x80)console_cursor++;}
        console_selected=false;
    }else if(k==NKEY_BACKSPACE||k==NKEY_DELETE){
        if(console_selected){console_input[0]=0;console_cursor=0;console_selected=false;}
        else {size_t start=console_cursor,end=console_cursor;
            if(k==NKEY_BACKSPACE&&start){start--;while(start&&((unsigned char)console_input[start]&0xc0)==0x80)start--;}
            if(k==NKEY_DELETE&&end<length){end++;while(end<length&&((unsigned char)console_input[end]&0xc0)==0x80)end++;}
            memmove(console_input+start,console_input+end,length-end+1);console_cursor=start;}
    }else if(!ctrl&&!(e->mods&NMOD_ALT)&&!(k>=NKEY_F1&&k<=NKEY_F12)){char text[8];const char *p=event_key(k,text);if(p==text&&*p)console_insert(p);}
    console_dirty=true;
}

/* These are native user commands. Page scripts receive the ordinary editing
   events, not permission to read the system clipboard outside a user key. */
static bool page_clipboard_key(web_node *target, const struct gui_event *e) {
    if (!doc || !target || !(e->mods & NMOD_CTRL) || (e->mods & NMOD_ALT)) return false;
    uint32_t key = e->key;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key == 'c' || key == 'x') {
        size_t length = 0;
        char *selected = web_control_selected_text(doc,target,&length);
        if (!selected) return false;
        if (length && clipboard_set(selected,length) >= 0 && key == 'x')
            web_control_edit(doc,target,"deleteByCut",NULL,0);
        free(selected);
        return true;
    }
    if (key == 'v') {
        char dummy = 0;
        int capacity = clipboard_get(&dummy,0);
        if (capacity <= 0) return true;
        char *text = malloc((size_t)capacity + 1);
        if (!text) return true;
        int length = clipboard_get(text,(size_t)capacity);
        /* A concurrent clipboard replacement must not paste a truncated value. */
        if (length > 0 && length <= capacity && !memchr(text,0,(size_t)length)) {
            text[length] = 0;
            web_control_edit(doc,target,"insertFromPaste",text,(size_t)length);
        }
        free(text);
        return true;
    }
    return false;
}

static void key(const struct gui_event *e) {
    bool ctrl = e->mods & NMOD_CTRL, alt = e->mods & NMOD_ALT;
    uint32_t k = e->key;
    if (k >= 'A' && k <= 'Z' && (ctrl || alt)) k += 32;
    if (ctrl && (e->mods & NMOD_SHIFT) && k == 'h') {
        sel_node = NULL; web_autocomplete_settings(); need_paint = true; return;
    }
    if (k == NKEY_F12) {
        console_open = !console_open;
        if(console_open)focus=F_CONSOLE;else if(focus==F_CONSOLE)focus=F_PAGE;
        need_layout = true;
        return;
    }
    if (console_open && ctrl && k == 'k') {
        console_n = console_skip = 0;
        console_dirty = true;
        return;
    }
    if (k == NKEY_ESC && loading && !web_modal_active(doc)) {
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
    if(console_open&&focus==F_CONSOLE)return console_key(e);
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
    if (!(e->mods & (NMOD_CTRL | NMOD_ALT)) &&
        ((e->key >= 32 && !(e->key >= 0x100 && e->key < 0x200)) || e->key == NKEY_ENTER))
        if (!dispatch_native("keypress", target, e, true)) return;
    if (page_clipboard_key(doc ? web_focused(doc) : NULL,e)) return;
    if (target && k == NKEY_DOWN && !ctrl && !alt) { open_select(target); if (sel_node) return; }
    if(target && (k==NKEY_ENTER || k==' ') && !ctrl && !alt) {
        if (k==' ' && web_media_activate(doc,target)) { flush_dom_layout(); return; }
        struct web_hit action={0};
        if(web_node_action(doc,target,&action) &&
           (action.kind==WEB_HIT_DETAILS || action.kind==WEB_HIT_BUTTON || action.kind==WEB_HIT_SUBMIT ||
            action.kind==WEB_HIT_CHECKBOX || action.kind==WEB_HIT_RADIO || action.kind==WEB_HIT_FILE ||
            (k==NKEY_ENTER && action.kind==WEB_HIT_LINK))) {page_click(target,0,0,true);return;}
    }
    if (doc && web_focused(doc)) {
        if (k == NKEY_ESC) {
            page_focus(NULL);
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

static void page_click(web_node *target, int x, int y, bool keyboard) {
    focus = F_PAGE;
    if (!doc) return;
    if(web_control_disabled(target))return;
    /* Author click listeners may reparent the target. Capture its first
       native link activation now, without changing the event target. */
    web_node *anchor = web_link_activation_anchor(doc,target);
    struct gui_event native = {.x = x, .y = y, .buttons = 0};
    struct web_control_activation activation;
    web_control_activation_begin(doc, target, &activation);
    bool allowed = dispatch_native_mode("click", target, &native, true, keyboard);
    bool input_events = web_control_activation_end(doc, &activation, allowed);
    if (!allowed) { flush_dom_layout(); return; }
    if (!anchor && web_media_activate(doc,target)) { page_focus(target); flush_dom_layout(); return; }
    web_node *label_control = anchor ? NULL : web_label_activation(target);
    if (label_control) { page_focus(label_control); page_click(label_control, x, y, keyboard); return; }
    struct web_hit hit = {0};
    if (anchor) {
        /* Read this same anchor's latest href after dispatch. Removal of
           href or reparenting must not select another ancestor anchor. */
        if (!web_link_action(doc,anchor,&hit)) return;
    } else if (!web_node_action(doc,target,&hit) || hit.kind==WEB_HIT_LINK) return;
    if (!keyboard && hit.kind==WEB_HIT_DETAILS) {
        struct web_hit pointer={0};
        if (!web_hit_test(doc,document_x(x),y-TB+scroll_y,&pointer) || pointer.kind!=WEB_HIT_DETAILS || pointer.node!=hit.node) return;
    }
    switch (hit.kind) {
    case WEB_HIT_LINK:
        if (hit.href) {
            if (has_prefix(hit.href, "file://") && !has_prefix(web_url(doc), "file://") &&
                !has_prefix(web_url(doc), "about:")) {
                set_status("A remote page cannot open a local file");
                return;
            }
            page_focus(NULL);
            if (!web_frame_navigate(doc, anchor, hit.href)) queue_navigation(hit.href, NULL, NAV_PUSH);
        }
        return;
    case WEB_HIT_TEXT_INPUT:
    case WEB_HIT_TEXTAREA: page_focus(hit.node); break;
    case WEB_HIT_FILE:
        page_focus(hit.node);
        if (web_input_choose_files(doc, hit.node)) {
            dispatch_native("input", hit.node, &native, false);
            dispatch_native("change", hit.node, &native, false);
            if (web_focused(doc) == hit.node) { free(focus_value); focus_value = strdup(web_control_value(hit.node)); }
            flush_dom_layout();
        }
        break;
    case WEB_HIT_CHECKBOX:
    case WEB_HIT_RADIO:
        if (!keyboard) page_focus(hit.node);
        if (input_events) { dispatch_native("input", hit.node, &native, false); dispatch_native("change", hit.node, &native, false); }
        break;
    case WEB_HIT_SUBMIT: submit(hit.node); return;
    case WEB_HIT_BUTTON: web_reset(doc,hit.node); flush_dom_layout(); break;
    case WEB_HIT_DETAILS:
        page_focus(web_disclosure_focus(hit.node)); web_toggle(doc,hit.node); flush_dom_layout(); break;
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
    if(pressed_node)return; /* an additional button is not a new primary press */
    web_active_release(doc);pressed_node=NULL;
    int x = e->x, y = e->y;
    if (console_open && y >= TB + page_h() && y < w->h - SB) {focus=F_CONSOLE;console_dirty=true;return;}
    if (sel_node) {
        int vis = MIN(sel_n - sel_top, (w->h - SB - sel_y) / SEL_H);
        if (ui_hit(x, y, sel_x, sel_y, sel_w, vis * SEL_H)) choose_select(sel_top + (y - sel_y) / SEL_H);
        else sel_node = NULL;
        return;
    }
    if (y < TB) {
        if (x >= ADDR_X) {
            if (focus != F_ADDR) {
                addr_focus();
            }
            else { /* place the caret */
                addr_all = false;
                size_t len = strlen(addr), best = len;
                int width = 0, shift = address_width(addr, acur);
                shift = shift > w->w - ADDR_X - 28 ? shift - (w->w - ADDR_X - 28) : 0;
                for (size_t i = 0; i < len;) {
                    if ((int64_t)ADDR_X + 7 + width - shift >= x) { best = i; break; }
                    uint32_t cp; int bytes = gfx_utf8_decode(addr + i, &cp);
                    if (bytes <= 0 || (size_t)bytes > len - i) break;
                    int add = MAX(gfx_codepoint_width(cp, FONT_SMALL), 0);
                    width = add > INT_MAX - width ? INT_MAX : width + add; i += bytes;
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
                else if (i == BTN_HISTORY) { sel_node = NULL; web_autocomplete_settings(); need_paint = true; }
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
        pressed_node = web_node_at(doc, document_x(x), y - TB + scroll_y);
        web_active_press(doc,pressed_node);
        web_frame_address_select(doc, pressed_node); need_paint = true;
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
        if (web_hit_test(doc, document_x(e->x), e->y - TB + scroll_y, &hit) && hit.href) strlcpy(h, hit.href, sizeof h);
        node = web_node_at(doc, document_x(e->x), e->y - TB + scroll_y);
    }
    if (doc && !native_wait && !web_script_running(doc)) {
        web_node *captured=native_capture_target(e);
        if(captured)node=captured;
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
    case EV_UNFOCUS: web_pointer_cancel(doc);web_active_release(doc);pressed_node=NULL;break;
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
        /* A deferred release may follow a deferred down that was not active
           yet at native dequeue. Clear again before any author mouseup. */
        if(!(e->buttons&1))web_active_release(doc);
        else break; /* another button's release does not end the primary press */
        if (drag_sb) { drag_sb = false; pressed_node = NULL; break; }
        if (doc) {
            web_node *capture=native_capture_target(e);
            bool inside=e->y>=TB && e->y<TB+page_h() && e->x>=0 && e->x<page_w();
            web_node *target=capture?capture:inside?web_node_at(doc,document_x(e->x),e->y-TB+scroll_y):NULL;
            web_node *down = pressed_node;
            dispatch_native("mouseup", target, e, false);
            web_node *captured_click=web_pointer_capture_click_target(doc,target);
            web_node *click_target = pressed_node ? captured_click?captured_click:web_pointer_click_target(doc,down,target) : NULL;
            if(debug_js)fprintf(stderr,"[web-input:click] down=%p up=%p target=%p same=%d\n",
                                (void *)down,(void *)target,(void *)click_target,down==target);
            if (click_target) page_click(click_target, e->x, e->y,false);
        }
        pressed_node = NULL;
        break;
    case EV_MOUSE_MOVE:
        if(!(e->buttons&1)){web_active_release(doc);pressed_node=NULL;}
        mouse_move(e);
        flush_dom_layout();
        if (need_paint && !quit) redraw();
        return;
    case EV_WHEEL:
        if (console_open && e->y >= TB + page_h() && e->y < w->h - SB) {
            console_scroll(-e->wheel * 3);
        } else if (sel_node) {
            int room = MAX((w->h - SB - sel_y) / SEL_H, 1);
            sel_top = MAX(0, MIN(sel_n - room, sel_top + e->wheel));
        } else {
            web_node *target = doc && e->y >= TB && e->y < TB + page_h() ?
                               web_node_at(doc, document_x(e->x), e->y - TB + scroll_y) : NULL;
            if (dispatch_native("wheel", target, e, true)) {
                if (e->mods & NMOD_SHIFT) scroll_to_xy(scroll_x + e->wheel * LINE,scroll_y);
                else scroll_to(scroll_y + e->wheel * LINE);
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
        if (e->key == NKEY_F12) {
            console_open = !console_open;
            if (console_open) focus = F_CONSOLE; else if (focus == F_CONSOLE) focus = F_PAGE;
            prepare_native_snapshot(true); redraw(); return;
        }
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
    else if (!browser_coalesce_hover(evq,evq_n,e,evq_hover_tail_valid)) {
        if (evq_n == evq_capacity) {
            int capacity = evq_capacity ? (evq_capacity > INT_MAX / 2 ? INT_MAX : evq_capacity * 2) : 64;
            void *grown = evq_n < INT_MAX && (size_t)capacity <= SIZE_MAX / sizeof *evq ?
                realloc(evq, (size_t)capacity * sizeof *evq) : NULL;
            if (grown) { evq = grown; evq_capacity = capacity; }
            else if (!evq_failure_logged) { console_add("error", "Deferred native event storage allocation failed"); evq_failure_logged = true; }
        }
        if (evq_n < evq_capacity) {evq[evq_n++] = *e;evq_hover_tail_valid=browser_hover_tail(e);}
    }
    if (moved) redraw();
}

static void document_step(uint64_t now) {
    if (!doc || quit) { web_media_background(now); return; }
    if (scroll_event_pending && !native_wait && !web_script_running(doc)) {
        scroll_event_pending = false;
        web_document_scroll(doc);
    }
    uint64_t tick_start = debug_js ? uptime_ms() : 0;
    web_tick(doc, now);
    if (debug_js) {
        uint64_t elapsed = uptime_ms() - tick_start;
        loop_profile.tick_ms += elapsed;
        loop_profile.tick_max_ms = MAX(loop_profile.tick_max_ms, elapsed);
        loop_profile.ticks++;
    }
    if (quit) return;
    bool dirty = web_dirty(doc);
    bool media_paint = web_paint_dirty(doc);
    if (dirty || need_layout) {
        relayout();
        if (initial_scroll_pending) {
            if (initial_scroll_anchor) {
                int y = anchor(cur_url);
                if (y >= 0) { scroll_to(y); initial_scroll_pending = false; }
            } else if (max_scroll() >= initial_scroll_y && max_scroll_x() >= initial_scroll_x) {
                scroll_to_xy(initial_scroll_x,initial_scroll_y); initial_scroll_pending = false;
            }
        }
        set_title();
        redraw();
    } else if (media_paint || need_paint || (console_dirty && console_open)) redraw();
    flush_dom_layout();
    if (!refresh_seen && !loading) {
        int delay;
        const char *url = web_refresh_url(doc, &delay);
        if (url) {
            refresh_seen = true;
            if (delay <= 10 && strcmp(url, cur_url)) {
                if (url_text_set(&refresh_text, url)) refresh_at = uptime_ms() + (uint64_t)MAX(delay, 0) * 1000 + 1;
                else console_add("error", "Refresh URL allocation failed");
            }
        }
    }
}

static void report_loop_profile(uint64_t now) {
    if (!debug_js) return;
    if (!loop_profile.since) { loop_profile.since = now; return; }
    if (now - loop_profile.since < 2000) return;
    char message[512];
    snprintf(message, sizeof message,
             "Browser loop profile: wall %lu ms, ticks %u/%lu ms/max %lu, host layout %u/%lu ms, draw %u/%lu ms, update %lu ms, net %lu ms, wait %lu ms, parallel jobs %lu (inclusive)",
             (unsigned long)(now-loop_profile.since), loop_profile.ticks,
             (unsigned long)loop_profile.tick_ms, (unsigned long)loop_profile.tick_max_ms,
             loop_profile.layouts, (unsigned long)loop_profile.layout_ms,
             loop_profile.draws, (unsigned long)loop_profile.draw_ms,
             (unsigned long)loop_profile.update_ms, (unsigned long)loop_profile.net_ms,
             (unsigned long)loop_profile.wait_ms, (unsigned long)parallel_jobs());
    memset(&loop_profile, 0, sizeof loop_profile);
    loop_profile.since = now;
    host_console(NULL, 0, message);
    static uint64_t reported_batches[PARALLEL_STAGE_COUNT];
    for (int stage = 1; stage < PARALLEL_STAGE_COUNT; stage++) {
        struct parallel_stats s;
        if (!parallel_get_stats((enum parallel_stage)stage, &s) || s.batches == reported_batches[stage]) continue;
        reported_batches[stage] = s.batches;
        snprintf(message,sizeof message,
            "Native parallel %s: batches %lu, items %lu/AP %lu, work %lu/AP %lu, wall %lu ms, workers %lu ms/AP %lu ms (inclusive), sampled CPUs %lx/AP %lx (cumulative)",
            parallel_stage_name((enum parallel_stage)stage),(unsigned long)s.batches,
            (unsigned long)s.items,(unsigned long)s.helper_items,(unsigned long)s.work,(unsigned long)s.helper_work,
            (unsigned long)s.wall_ms,(unsigned long)s.worker_ms,(unsigned long)s.helper_ms,
            (unsigned long)s.sampled_cpu_mask,(unsigned long)s.sampled_helper_cpu_mask);
        host_console(NULL,0,message);
    }
}

int main(int argc, char **argv) {
    bool start_maximized = false;
    console_input=calloc(1,1);console_input_capacity=1;
    if(!console_input)return 1;
    while (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--debug-js")) { debug_js = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--maximized")) { start_maximized = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--console-file-each-document")) { console_each_document = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--js-console")) {
            console_open = true; focus = F_CONSOLE; argc--; argv++;
        }
        else if (!strcmp(argv[1], "--js-budget-ms")) {
            const char *value = argc > 2 ? argv[2] : "";
            if (!*value || strspn(value,"0123456789") != strlen(value)) {
                fprintf(stderr,"browser: --js-budget-ms requires 1..4294967295 milliseconds\n");return 2;
            }
            unsigned long long budget = strtoull(value,NULL,10);
            if (budget < WEB_JS_TASK_MIN_MS || budget > WEB_JS_TASK_MAX_MS) {
                fprintf(stderr,"browser: --js-budget-ms requires 1..4294967295 milliseconds\n");return 2;
            }
            js_task_budget_ms=(uint32_t)budget;argc-=2;argv+=2;
        } else if (!strcmp(argv[1], "--console-file") && argc > 2) {
            /* Keep diagnostics visible even if navigation never produces a
               document; the queued command still waits for a safe JS task. */
            console_open=true;focus=F_CONSOLE;
            FILE *file=fopen(argv[2],"rb");long end=-1;
            if(file&&fseek(file,0,SEEK_END)==0){end=ftell(file);if(fseek(file,0,SEEK_SET)!=0)end=-1;}
            char *source=end>0&&(uint64_t)end<SIZE_MAX?malloc((size_t)end+1):NULL;
            size_t length=source?fread(source,1,(size_t)end,file):0;
            if(file)fclose(file);
            if(!source||length!=(size_t)end){free(source);console_add("error","Console source file could not be read or allocated.");}
            else {source[length]=0;free(console_startup);console_startup=source;console_startup_length=length;console_due=uptime_ms()+3000;}
            argc-=2;argv+=2;
        } else {
            fprintf(stderr,"browser: unknown or incomplete option: %s\n",argv[1]);return 2;
        }
    }
    /* Ignoring extra positional arguments can silently open a console source
       file instead of the requested site. --js-console is only a UI switch;
       an explicit --console-file owns and consumes its source argument. */
    if (argc > 2) {
        fprintf(stderr,"browser: exactly one destination is accepted; use --console-file for JavaScript input\n");
        free(console_startup);
        return 2;
    }
    int sw = 1024, sh = 768;
    screen_size(&sw, &sh);
    web_avmedia_debug(debug_js);
    web_paint_debug(debug_js);
    parallel_profile_enable(debug_js);
    w = win_open(MIN(1100, sw - 40), MIN(800, sh - 70), "Web",
                 WIN_RESIZABLE | (start_maximized ? WIN_START_MAXIMIZED : 0));
    if (!w) return 1;
    network = webnet_create();
    if (!network) { win_close(w); return 1; }
    storage = webstorage_create(); /* lazily touches local disk only on storage access */
    redraw();
    if (argc > 1) {
        if (argv[1][0] == '/') {
            size_t length = strlen(argv[1]);
            char *u = length <= SIZE_MAX - 8 ? malloc(length + 8) : NULL;
            if (!u) { console_add("error", "Local destination could not be allocated."); }
            else {
                memcpy(u, "file://", 7);
                memcpy(u + 7, argv[1], length + 1);
                navigate(u, NULL, NAV_PUSH);
                free(u);
            }
        } else go_addr(argv[1]);
    } else navigate(HOME, NULL, NAV_PUSH);
    while (!quit) {
        uint64_t now = uptime_ms();
        webnet_pump(network, now);
        if (debug_js) loop_profile.net_ms += uptime_ms() - now;
        apply_pending_history();
        apply_pending_navigation();
        finish_navigation();
        /* Native window state is sampled only between script tasks. Queries
           remain live during scripts, but visibility/focus callbacks cannot
           re-enter a module loader or a page callback on the C stack. */
        if(!native_wait && !web_script_running(doc)) {
            unsigned state=host_window_state(NULL),changed=state^window_previous_state;
            if(window_state_known && doc) {
                if(changed&WEB_WINDOW_VISIBLE)web_visibility_event(doc);
                if(changed&WEB_WINDOW_FOCUSED) {
                    struct web_event event={.type=(state&WEB_WINDOW_FOCUSED)?"focus":"blur"};
                    web_dispatch(doc,NULL,&event);flush_dom_layout();
                }
            }
            window_previous_state=state;window_state_known=true;
        }
        document_step(now);
        report_loop_profile(uptime_ms());
        if(console_startup_pending()&&doc&&!loading&&!native_wait&&now>=console_due&&!web_script_running(doc)){
            console_open=true;focus=F_CONSOLE;
            console_startup_generation=generation;
            if(!web_console_eval(doc,console_startup,console_startup_length))console_add("error","Startup console command failed.");
            if(!console_each_document){free(console_startup);console_startup=NULL;console_startup_length=0;}
            need_layout=true;prepare_native_snapshot(false);redraw();
        }
        apply_pending_history();
        apply_pending_navigation();
        if (quit) break;
        now = uptime_ms();
        struct gui_event e;
        if (evq_n) {
            e = evq[0];
            memmove(evq, evq + 1, sizeof evq[0] * (size_t)--evq_n);
            if (!evq_n) {evq_failure_logged=false;evq_hover_tail_valid=false;}
            handle(&e);
            continue;
        }
        int timeout = webnet_timeout(network, now);
        if (webstorage_pending(storage)) {
            struct web_storage_result result;
            if (webstorage_flush(storage, false, &result) != WEB_STORAGE_OK)
                console_add("error", "Local storage flush failed; retrying later.");
            if (webstorage_pending(storage) && (timeout < 0 || timeout > 100)) timeout = 100;
        }
        int64_t deadline = web_deadline(doc); /* retired media children also wake a document-less window */
        if (deadline >= 0) {
            int due = deadline <= (int64_t)now ? 0 : (int)MIN(deadline - (int64_t)now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        if (navigation_ready || pending_navigation || scroll_event_pending || (history_queue_count && !loading)) timeout = 0;
        if(console_startup_pending()&&doc&&!loading){int due=now>=console_due?0:(int)MIN(console_due-now,0x7fffffff);
            if(timeout<0||due<timeout)timeout=due;}
        if (refresh_at) {
            uint64_t now = uptime_ms();
            if (now >= refresh_at) {
                refresh_at = 0;
                navigate(refresh_url, NULL, NAV_PUSH);
                continue;
            }
            int due = (int)MIN(refresh_at - now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        uint64_t wait_start = debug_js ? uptime_ms() : 0;
        int r = browser_event(&e, timeout);
        if (debug_js) loop_profile.wait_ms += uptime_ms() - wait_start;
        if (r < 0) break;
        if (r > 0) {
            handle(&e);
            continue;
        }
    }
    stop_navigation();
    cancel_document_requests();
    if (doc) web_free(doc);
    web_media_background(uptime_ms()); /* cancel already killed children; never block the closing GUI */
    webnet_free(network);
    webstorage_free(storage);
    parallel_shutdown();
    free(pending_post); free(pending_content_type);
    free(current_text.p); free(navigation_text.p); free(pending_text.p); free(address_text.p); free(refresh_text.p);
    free(focus_value); free(find_text.p);
    for (int i = 0; i < nhist; i++) hist_discard(i);
    free(hist); free(console_lines); free(sel_labels); free(evq); free(console_input); free(console_startup);
    while (history_queue) { struct browser_history_task *next=history_queue->next; free(history_queue); history_queue=next; }
    win_close(w);
    return 0;
}
