/* Nocturne Web: native DOM, layout and painting, with a document-owned JavaScript runtime.
   HTTP requests run in isolated Nocturne child processes, never on the GUI/JS task. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include "nocturne.h"
#include "ui.h"
#include "http.h"
#include "web.h"
#include "webnet.h"
#include "webstorage.h"
#include <parallel.h>

#define SCRW (browser_fullscreen?0:10) /* native fullscreen has no chrome */
#define LINE 48
#define HOME "about:home"
#define SEARCH "https://lite.duckduckgo.com/lite/?q="

/* The window is the interface engine's (libc/ui.c): the toolbar, the status line, the find bar
   and the console's input are its widgets, and the page, its scroll bar and DevTools are drawn
   into one surface between them, in the surface's coordinates. */
enum { WINDOW = 1, VIEW = 1 };
static struct ui_session *ui;
static window_t *w;   /* behind the engine's window: its state, fullscreen and handle */
static canvas_t view; /* the surface */
static bool browser_fullscreen;
static web_doc *fullscreen_doc,*pip_doc;
/* Direct scanout (win_direct, web_direct_open): the display shows a fullscreen WebGL canvas's frames
   itself. direct holds its context and the display's images from the first frame that can until the
   display shows the window again (EV_DIRECT key 0); direct_shown is the display's last word. */
static web_direct *direct;
static bool direct_wanted, direct_shown;
/* The page's rendering opportunities are the display's frames (host.display_frames): frame_asked while
   the window waits for the one asked of it (ui_window_frame, answered by UI_EVENT_FRAME). */
static bool frame_asked;
static web_node *pip_node;
static window_t *pip_window;
static uint64_t pip_revision;
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
/* --net-log: requests in the console; --log-console: the console in the system log.
   --debug-js turns both on. */
static bool net_log, log_console;
/* Debug-only inclusive phase totals, with no page data or per-frame logging. */
static struct {
    uint64_t since, tick_ms, tick_max_ms, layout_ms, draw_ms, update_ms, net_ms, wait_ms;
    uint64_t partial_ms, partial_update_ms, video_present_ms, console_ms, console_update_ms;
    unsigned ticks, layouts, draws;
    unsigned partial_draws, video_patches, console_draws;
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
    uint64_t devtools; /* Network panel entry, 0 when not recorded */
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
static uint64_t console_due, console_startup_generation;
static bool console_each_document;
/* An explicit native debug file is not a page-injected script. Preserve the
   old once-per-window default, and never re-run it on same-document history. */
static bool console_startup_pending(void) {
    return console_startup && (!console_each_document || console_startup_generation != generation);
}

static int console_h(void);
static int console_visible(void) { return MAX(1, (console_h() - 50) / 16); } /* below the DevTools tab bar (24) and the heading (26) */
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

/* Where keys go. The engine keeps the fields' own focus and acts on a field's "focus" only
   when it changes, so each field keeps its own value (focus_asked) and only a move the
   browser makes changes them: focus_shown is where the view last put the keyboard, and
   focus_renew asks again for the field that has it (select all of it). What the engine does
   with the keyboard comes back in order with the other input (CMD_FOCUS) and moves
   focus_shown with it, so the views never ask for what the engine did on its own. */
enum { F_PAGE, F_ADDR, F_FIND, F_CONSOLE, F_COUNT };
static int focus = F_PAGE, focus_shown = F_PAGE, focus_serial, focus_asked[F_COUNT];
static bool focus_renew;
static int field_focus = F_PAGE; /* the field the engine has given the keyboard to, as it said so */

static bool find_open;
static struct url_text find_text;
#define find_buf (find_text.p ? find_text.p : "")
static bool find_error;
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

static bool drag_sb;
static int drag_off;

/* Native scroll and later author notification are separate phases. The
   document epochs keep queued input out of a replacement page/frame. */
struct deferred_input {
    struct gui_event event;
    struct web_input_target document, target;
    uint64_t generation;
    bool scrolled;
};
static struct deferred_input *evq;
static int evq_first, evq_n, evq_capacity, evq_scroll_at;
static bool evq_failure_logged;
static bool evq_hover_tail_valid;
static bool scroll_event_pending;
static bool native_wait_snapshot;
static bool native_scroll_active;
#include "browser_events.h"

static int page_w(void) { return MAX(view.w - SCRW, 50); }
static int devtools_height; /* DevTools panel, dragged by its top edge; 0: default share */
static int console_h(void) {
    if (!console_open || browser_fullscreen) return 0;
    int room = view.h, h = devtools_height ? devtools_height : room * 2 / 5;
    return MAX(MIN(h, room - 60), MIN(120, room / 2));
}
static int page_h(void) { return MAX(view.h - console_h(), 20); }
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
}

/* ---------------------------------------------------------------- drawing */
static void show_chrome(void);
#include "browser_devtools.h"

/* The widgets around the surface. Their events are browser commands that wait their turn
   with the rest of the input (EV_COMMAND), except a field's text, which only the field uses. */
enum { CMD_BACK = 1, CMD_FORWARD, CMD_RELOAD, CMD_HOME, CMD_HISTORY, CMD_GO, CMD_FIND, CMD_FIND_NEXT,
       CMD_EVAL, CMD_FOCUS };
#define EV_COMMAND 0x100u /* key = CMD_*; CMD_FOCUS: x = F_*, y = got (1) or lost (0) */
static uint32_t views; /* views sent: an event names the one it refers to */
static struct chrome {
    uint32_t view, back, forward, reload, home, history, address, find, console, page;
} chrome[32]; /* the recent views: an event names the view it came from, and a handler may send many before it is read */
static uint8_t *chrome_sent; /* the last view as it went, not sent again */
static size_t chrome_sent_n;

static uint32_t tool(struct ui_msg *m, const char *icon, bool enabled) {
    uint32_t place = ui_node(m, "button");
    ui_str(m, "icon", icon); ui_str(m, "variant", "ghost"); ui_num(m, "compact", 1);
    if (!enabled) ui_num(m, "disabled", 1);
    ui_end(m);
    return place;
}

static void field_focus_prop(struct ui_msg *m, int which) {
    ui_num(m, "focus", focus_asked[which]);
}

static void show_chrome(void) {
    if (!ui) return;
    if (focus != focus_shown || focus_renew) {
        if (focus_shown != F_PAGE) focus_asked[focus_shown] = -++focus_serial;
        if (focus != F_PAGE) focus_asked[focus] = ++focus_serial;
        focus_shown = focus;
        focus_renew = false;
    }
    const struct ui_theme *t = &ui_theme;
    struct chrome c = {.view = views + 1};
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column");
    if (!browser_fullscreen) {
        ui_node(&m, "row"); ui_str(&m, "key", "toolbar"); ui_nums(&m, "padding", (double[]){5, 8}, 2); ui_num(&m, "gap", 2);
        ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
            c.back = tool(&m, "chevron-left", hpos > 0);
            c.forward = tool(&m, "chevron-right", hpos < nhist - 1);
            c.reload = tool(&m, loading ? "close" : "refresh", loading || cur_url[0]);
            c.home = tool(&m, "home", true);
            c.history = tool(&m, "clock", true);
            ui_node(&m, "spacer"); ui_num(&m, "size", 4); ui_end(&m);
            c.address = ui_node(&m, "field");
            ui_str(&m, "value", field_focus == F_ADDR ? addr : cur_url);
            ui_str(&m, "placeholder", "Search or enter an address");
            ui_str(&m, "width", "fill"); ui_num(&m, "height", 30); ui_num(&m, "selectOnFocus", 1);
            field_focus_prop(&m, F_ADDR);
            ui_end(&m);
        ui_end(&m);
        ui_node(&m, "divider"); ui_end(&m);
    }
    c.page = ui_node(&m, "surface"); ui_str(&m, "key", "page"); ui_num(&m, "surface", VIEW); ui_num(&m, "pointer", 1);
    ui_str(&m, "width", "fill"); ui_str(&m, "height", "fill"); ui_end(&m);
    if (!browser_fullscreen && console_open && dt_tab == DT_CONSOLE) {
        ui_node(&m, "row"); ui_str(&m, "key", "console"); ui_nums(&m, "padding", (double[]){3, 8}, 2); ui_num(&m, "gap", 6);
        ui_str(&m, "align", "center"); ui_num(&m, "background", DT_BG);
            ui_node(&m, "text"); ui_str(&m, "text", ">"); ui_num(&m, "mono", 1); ui_num(&m, "colour", t->accent);
            ui_end(&m);
            c.console = ui_node(&m, "field");
            ui_str(&m, "value", console_input); ui_str(&m, "placeholder", "JavaScript");
            ui_num(&m, "mono", 1); ui_str(&m, "width", "fill"); ui_num(&m, "height", 28);
            field_focus_prop(&m, F_CONSOLE);
            ui_end(&m);
        ui_end(&m);
    }
    if (!browser_fullscreen) {
        ui_node(&m, "divider"); ui_end(&m);
        ui_node(&m, "row"); ui_str(&m, "key", "status"); ui_nums(&m, "padding", (double[]){0, 10}, 2); ui_num(&m, "height", 30);
        ui_num(&m, "gap", 8); ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
        if (find_open) {
            ui_node(&m, "icon"); ui_str(&m, "icon", "search"); ui_num(&m, "size", 16); ui_num(&m, "colour", t->textDim);
            ui_end(&m);
            c.find = ui_node(&m, "field");
            ui_str(&m, "value", find_buf); ui_str(&m, "placeholder", "Find in page");
            ui_num(&m, "length", 240); ui_num(&m, "height", 24);
            field_focus_prop(&m, F_FIND);
            ui_end(&m);
            bool bad = find_error || (find_y < 0 && find_buf[0]);
            ui_node(&m, "text");
            ui_str(&m, "text", find_error ? "allocation failed" : !find_buf[0] ? "Enter: next   Esc: close"
                               : find_y < 0 ? "not found" : "Enter: next match");
            ui_str(&m, "style", "caption"); ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1);
            if (bad) ui_num(&m, "colour", t->danger);
            ui_end(&m);
        } else {
            ui_node(&m, "text"); ui_str(&m, "text", hover[0] ? hover : status); ui_str(&m, "style", "caption");
            ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1);
            if (hover[0]) ui_num(&m, "colour", t->accentHover);
            ui_end(&m);
        }
        ui_end(&m);
    }
    ui_end(&m);
    /* a field left out of the view gives the keyboard up without an event and comes back new:
       one that still has the browser's keys (the console's, behind another DevTools tab) is
       asked for again when it is back */
    const bool shown[F_COUNT] = {[F_ADDR] = c.address, [F_FIND] = c.find, [F_CONSOLE] = c.console};
    for (int f = F_ADDR; f < F_COUNT; f++) {
        if (shown[f]) continue;
        focus_asked[f] = 0;
        if (field_focus == f) field_focus = F_PAGE;
        if (focus_shown == f) focus_shown = F_PAGE;
    }
    if (m.b.bad || (chrome_sent && m.b.n == chrome_sent_n && !memcmp(m.b.p, chrome_sent, m.b.n))) {
        free(m.b.p); free(m.open);
        return;
    }
    uint8_t *copy = malloc(m.b.n ? m.b.n : 1);
    if (copy) memcpy(copy, m.b.p, m.b.n);
    size_t n = m.b.n;
    if (ui_send(ui, &m) < 0) { free(copy); return; }
    free(chrome_sent);
    chrome_sent = copy;
    chrome_sent_n = copy ? n : 0;
    chrome[++views % 32] = c;
}

static void draw_page(void) {
    canvas_t *c = &view;
    int pw = page_w(), ph = page_h();
    if (doc) web_paint(doc, c, 0, 0, pw, ph, scroll_x, scroll_y);
    else gfx_fill(c, 0, 0, pw, ph, RGB(255, 255, 255));
    devtools_draw_overlay(c);
    /* scrollbar */
    int x = view.w - SCRW;
    gfx_fill(c, x, 0, SCRW, ph, ui_theme.sidebar);
    if (doc_h > ph) {
        int th = MAX(24, (int)((long)ph * ph / doc_h));
        int ty = (int)((long)scroll_y * (ph - th) / max_scroll());
        gfx_fill_round(c, x + 2, ty + 1, SCRW - 4, th - 2, 3, drag_sb ? ui_theme.accent : ui_theme.textFaint);
    }
    /* <select> popup */
    if (sel_node) {
        int vis = MIN(sel_n - sel_top, (view.h - sel_y) / SEL_H);
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

/* The DevTools panel under the page, inside the surface. The console's input
   line is an engine field below the surface (show_chrome). */
static void draw_console(void) {
    if (browser_fullscreen) return;
    if (console_open) {
        canvas_t *c = &view;
        int top = page_h(), h = console_h();
        devtools_draw(c, top, h);
        console_dirty = false;
        if (dt_tab != DT_CONSOLE) return;
        top += DT_TABH; h -= DT_TABH;
        gfx_fill(c, 0, top, view.w, h, DT_BG);
        char heading[192];
        snprintf(heading, sizeof heading, "JavaScript console  [PgUp/PgDn: history] [Ctrl+Shift+C: copy log] [Ctrl+K: clear]  %d/%d%s",
                 console_n - console_skip, console_n, console_skip ? " (paused)" : "");
        gfx_text(c, 8, top + 4, heading, ui_theme.textDim, TRANSPARENT, FONT_SMALL);
        canvas_t clip = *c;
        gfx_clip(&clip, 4, top + 24, view.w - 8, h - 24);
        console_skip = MIN(console_skip, MAX(console_n - console_visible(), 0));
        int count = MIN(console_visible(), console_n - console_skip);
        for (int i = 0; i < count; i++) {
            int n = console_n - console_skip - count + i;
            gfx_text(&clip, 8, top + 25 + i * 16, console_lines[n], ui_theme.text, TRANSPARENT, FONT_SMALL);
        }
    }
    console_dirty = false;
}
static void draw(void) {
    draw_page(); draw_console();
}

/* --log-console keeps the console in the system log (the serial port), so a
   diagnosis survives a page that floods, clears or closes its console. The
   uptime matches the console's own @seconds column. */
static void console_system_log(const char *level, const char *message) {
    static n_handle log = -2;
    if (log == -2) log = n_system_log_open();
    if (log < 0) return;
    uint64_t now = n_clock_uptime_ms();
    /* A long result continues on "+"-marked lines instead of being cut. */
    size_t len = strlen(message), off = 0;
    do {
        char line[2048];
        size_t part = MIN(len - off, (size_t)1900);
        int n = snprintf(line, sizeof line, "[browser %s%s @%lu.%03lu] %.*s\n", level ? level : "log", off ? "+" : "",
                         (unsigned long)(now / 1000), (unsigned long)(now % 1000), (int)part, message + off);
        if (n > 0) n_channel_send(log, line, (size_t)MIN(n, (int)sizeof line - 1), -1);
        off += part;
    } while (off < len);
}
static void console_add(const char *level, const char *message) {
    const char *p = message ? message : "";
    if (log_console) console_system_log(level, p);
    uint64_t now = n_clock_uptime_ms();
    unsigned long sequence = ++console_sequence;
    int glyph = w ? MAX(gfx_text_width("M", FONT_SMALL), 1) : 8;
    size_t wrap = (size_t)MAX(16, MIN(CONSOLE_LINE - 64, (view.px ? view.w : 1024) / glyph - 48));
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

/* The page is nothing but a fullscreen WebGL canvas (web_direct_open): its frames go to the display
   instead of through the window, which asks for that only meanwhile. true: the display shows the
   newest frame, and the window's surface (hidden) is not painted. */
static bool direct_frame(void) {
    /* The page waits for a script: the display keeps the frame it shows. */
    if (native_wait) return direct && direct_shown;
    bool allowed = w && doc && browser_fullscreen && fullscreen_doc == doc && !sel_node &&
                   !devtools_overlay_visible() && view.px && view.w == w->w && view.h == w->h;
    if (allowed && !direct) allowed = (direct = web_direct_open(doc, view.w, view.h)) != NULL;
    int shown = allowed ? web_direct_present(direct, doc, view.w, view.h, direct_shown) : -2;
    /* Not shown although the display shows the window directly: either it just stopped (EV_DIRECT key 0
       follows) or the context can no longer (-2 now), when nothing else would ask again. */
    if (shown == -1 && direct_shown) shown = web_direct_present(direct, doc, view.w, view.h, false);
    if (shown == -2) {
        allowed = false;
        /* The images are given back once the display no longer shows them. */
        if (direct && !direct_shown) { web_direct_close(direct); direct = NULL; }
    }
    if (allowed != direct_wanted && win_direct(w, allowed) >= 0) direct_wanted = allowed;
    return shown >= 0;
}

static void redraw(void) {
    if(doc){int x,y,width,height;(void)web_take_paint_damage(doc,&x,&y,&width,&height);}
    if (!view.px) { need_paint = false; show_chrome(); return; }
    if (direct_frame()) { need_paint = false; show_chrome(); return; }
    uint64_t start = debug_js ? n_clock_uptime_ms() : 0;
    draw();
    uint64_t drawn = debug_js ? n_clock_uptime_ms() : 0;
    ui_surface_changed(ui, VIEW);
    show_chrome();
    if (debug_js) {
        loop_profile.draw_ms += drawn - start;
        loop_profile.update_ms += n_clock_uptime_ms() - drawn;
        loop_profile.draws++;
    }
    need_paint = false;
}
static void redraw_damage(void){
    int x,y,width,height;
    if(direct_frame()){if(doc)(void)web_take_paint_damage(doc,&x,&y,&width,&height);return;}
    if(!doc||!view.px||need_paint||sel_node||devtools_overlay_visible()||!web_take_paint_damage(doc,&x,&y,&width,&height)){redraw();return;}
    if(width<=0||height<=0)return;
    uint64_t start=debug_js?n_clock_uptime_ms():0;
    canvas_t original=view;
    gfx_clip(&view,x,y,width,height);
    draw_page();view=original;
    uint64_t drawn=debug_js?n_clock_uptime_ms():0;
    ui_surface_changed_rect(ui,VIEW,x,y,width,height);
    if(debug_js){loop_profile.partial_draws++;loop_profile.partial_ms+=drawn-start;
        loop_profile.partial_update_ms+=n_clock_uptime_ms()-drawn;}
}

/* Log changes do not invalidate author content. In particular the
   profiling console must not make its own next sample rasterize the page.
   Opening/closing or resizing it still follows the layout/full-paint path. */
static void redraw_console(void) {
    if (!view.px || !console_dirty || !console_open || browser_fullscreen) return;
    uint64_t start = debug_js ? n_clock_uptime_ms() : 0;
    int top = page_h(), height = console_h();
    draw_console();
    uint64_t drawn = debug_js ? n_clock_uptime_ms() : 0;
    ui_surface_changed_rect(ui, VIEW, 0, top, view.w, height);
    if (debug_js) {
        loop_profile.console_draws++;
        loop_profile.console_ms += drawn - start;
        loop_profile.console_update_ms += n_clock_uptime_ms() - drawn;
    }
}

static void set_status(const char *s) {
    strlcpy(status, s, sizeof status);
    show_chrome(); /* the same view is not sent again */
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

/* Native presentation changes only: no author callback or forced layout while
   a script/native wait is active. Residual input chains to the viewport. */
static void scroll_input(web_node *target,int dx,int dy,int edge){
    int old_x=scroll_x,old_y=scroll_y,input_dx=dx,input_dy=dy;
    bool handled=doc&&web_scroll_input(doc,target,&dx,&dy,edge);
    if(edge){if(!handled)scroll_to(edge<0?0:max_scroll());}
    else if(dx||dy){
        int64_t x=(int64_t)scroll_x+dx,y=(int64_t)scroll_y+dy;
        scroll_to_xy((int)MAX((int64_t)0,MIN(x,(int64_t)max_scroll_x())),
                     (int)MAX((int64_t)0,MIN(y,(int64_t)max_scroll())));
    }
    if(debug_js)fprintf(stderr,"[web-input:scroll] input=%d,%d edge=%d handled=%d residual=%d,%d viewport=%d,%d->%d,%d max=%d,%d layout=%d\n",
        input_dx,input_dy,edge,handled,dx,dy,old_x,old_y,scroll_x,scroll_y,max_scroll_x(),max_scroll(),need_layout);
}
static web_node *key_scroll_target(void){
    web_node *target=doc?web_focused(doc):NULL;
    return target?target:hover_node;
}

/* scrolling keys for the page; true if the event was one */
static bool scroll_key(const struct gui_event *e) {
    int ph = page_h();
    web_node *target=key_scroll_target();
    switch (e->key) {
    case NKEY_LEFT: scroll_input(target,-LINE,0,0); return true;
    case NKEY_RIGHT: scroll_input(target,LINE,0,0); return true;
    case NKEY_UP: scroll_input(target,0,-LINE,0); return true;
    case NKEY_DOWN: scroll_input(target,0,LINE,0); return true;
    case NKEY_PGUP: scroll_input(target,0,-(ph-LINE),0); return true;
    case NKEY_PGDN: scroll_input(target,0,ph-LINE,0); return true;
    case NKEY_HOME: scroll_input(target,0,0,-1); return true;
    case NKEY_END: scroll_input(target,0,0,1); return true;
    case ' ': scroll_input(target,0,(e->mods & NMOD_SHIFT?-1:1)*(ph-LINE),0); return true;
    }
    return false;
}

/* an event that only scrolls the page can be handled even while something is loading */
static bool try_scroll_event(const struct gui_event *e,struct web_input_target *target_out) {
    if (e->type == EV_WHEEL) {
        if (!doc || sel_node || need_layout || e->x<0 || e->x>=page_w() ||
            e->y<0 || e->y>=page_h() || !web_native_input_ready(doc,native_wait&&native_wait_snapshot)) return false;
        web_node *target=web_node_at(doc,document_x(e->x),e->y+scroll_y);
        /* Hit-index construction can run a cancellation checkpoint. Chrome
           may have resized the viewport or queued navigation in that call. */
        if(quit || pending_navigation || need_layout)return false;
        if(!web_native_wheel_target(doc,target,target_out,native_wait&&native_wait_snapshot))return false;
        doc_w=web_doc_width(doc);doc_h=web_doc_height(doc);
        if (e->mods & NMOD_SHIFT) scroll_input(target,e->wheel*LINE,0,0);
        else scroll_input(target,0,e->wheel*LINE,0);
        return true;
    }
    /* Key defaults remain behind cancelable keydown delivery. Unlike a wheel
       proven passive, a queued PageDown cannot silently bypass preventDefault. */
    return false;
}

static bool defer_native_event(const struct gui_event *e) {
    if(evq_n && browser_coalesce_hover(&evq[evq_first+evq_n-1].event,1,e,evq_hover_tail_valid))return true;
    if(evq_first+evq_n==evq_capacity) {
        if(evq_first) {
            evq_scroll_at=MAX(0,evq_scroll_at-evq_first);
            memmove(evq,evq+evq_first,(size_t)evq_n*sizeof *evq);
            evq_first=0;
        }else {
            int capacity=evq_capacity?(evq_capacity>INT_MAX/2?INT_MAX:evq_capacity*2):64;
            void *grown=evq_n<INT_MAX && (size_t)capacity<=SIZE_MAX/sizeof *evq?
                realloc(evq,(size_t)capacity*sizeof *evq):NULL;
            if(!grown) {
                if(!evq_failure_logged){console_add("error","Deferred native event storage allocation failed");evq_failure_logged=true;}
                return false;
            }
            evq=grown;evq_capacity=capacity;
        }
    }
    struct deferred_input *entry=&evq[evq_first+evq_n++];
    *entry=(struct deferred_input){.event=*e,.generation=generation};
    (void)web_input_capture(doc,NULL,&entry->document);
    evq_hover_tail_valid=browser_hover_tail(e);
    return true;
}

/* This is also called at a pure-JS checkpoint after a C operation unwinds.
   Never re-layout a dirty document here. Blocking wheels and earlier keys/
   buttons remain FIFO-owned by the ordinary cancelable event dispatcher. */
static void native_scroll_queue(void) {
    if(native_scroll_active || !doc || !evq_n || !web_native_input_ready(doc,native_wait&&native_wait_snapshot))return;
    native_scroll_active=true;
    bool painted=false;
    evq_scroll_at=MAX(evq_scroll_at,evq_first);
    while(evq_scroll_at<evq_first+evq_n) {
        int offset=evq_scroll_at-evq_first;
        struct deferred_input entry=evq[evq_scroll_at];
        if(entry.event.type==EV_MOUSE_MOVE && !entry.event.buttons){evq_scroll_at++;continue;}
        if(entry.event.type!=EV_WHEEL)break;
        if(entry.scrolled){evq_scroll_at++;continue;}
        if(entry.generation!=generation || !web_input_target_valid(doc,&entry.document))break;
        if(!try_scroll_event(&entry.event,&entry.target))break;
        /* A nested hit-test/paint checkpoint can append input, grow/compact
           the FIFO, or repaint chrome. Keep no queue-storage pointer across
           native calls; its logical offset survives those tail-only changes. */
        int index=evq_first+offset;
        evq[index].target=entry.target;evq[index].scrolled=true;
        painted=true;evq_scroll_at=index+1;
        /* Painting is itself native work; one presentation per drained batch
           avoids a full viewport raster for each wheel packet. */
    }
    if(painted && !quit)redraw();
    native_scroll_active=false;
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

static const char *guess_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strpbrk(dot, "\\/")) return "";
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

/* The file: URL of a path (n_path_to_url), malloc'd; NULL if it is too long or memory runs out. */
static char *file_url(const char *path) {
    size_t cap = 3 * (strlen(path) + 3) + 9; /* each byte of the complete path as %XX, after "file:///" */
    char *url = malloc(cap);
    if (url && n_path_to_url(path, url, cap) < 0) { free(url); url = NULL; }
    return url;
}

/* A file: URL is read as the path it names (n_path_from_url). */
static bool read_local(const char *url, struct fetched *f) {
    char path[4096];
    n_result named = n_path_from_url(url, path, sizeof path);
    if (named < 0) {
        strlcpy(f->err, named == -N_ERR_TOO_LARGE ? "local path exceeds filesystem limit" : "invalid local path", sizeof f->err);
        return false;
    }
    size_t end = strcspn(url, "?#");
    bool slash = end && url[end - 1] == '/'; /* the URL of a folder ends with '/' */
    struct n_entry info;
    if (n_entry_info(path, &info) < 0) {
        snprintf(f->err, sizeof f->err, "%s: no such file or folder", path);
        return false;
    }
    if (info.kind == N_KIND_FOLDER) { /* a folder listing */
        struct n_entry *entries = NULL;
        n_result count = n_folder_entries(path, &entries);
        if (count < 0) {
            snprintf(f->err, sizeof f->err, "cannot open %s", path);
            return false;
        }
        f->status = 200;
        struct sb s = {0};
        sb_str(&s, "<!doctype html><title>");
        sb_esc(&s, path, strlen(path));
        sb_str(&s, "</title><style>body{font-family:sans-serif;margin:24px}td{padding:2px 16px 2px 0}"
                   "a{text-decoration:none}</style><h2>Index of ");
        sb_esc(&s, path, strlen(path));
        sb_str(&s, "</h2><table>");
        if (strcmp(path, "C:\\")) sb_str(&s, "<tr><td><a href=\"../\">../</a><td>");
        for (n_result i = 0; i < count; i++) {
            const struct n_entry *d = &entries[i];
            bool dir = d->kind == N_KIND_FOLDER;
            char sz[32] = "", child[4096];
            char *href = n_path_join(path, d->name, child, sizeof child) >= 0 ? file_url(child) : NULL;
            if (!href) continue;
            if (!dir) snprintf(sz, sizeof sz, "%llu bytes", (unsigned long long)d->size);
            sb_str(&s, "<tr><td><a href=\"");
            sb_str(&s, href);
            free(href);
            sb_str(&s, dir ? "/\">" : "\">");
            sb_esc(&s, d->name, strlen(d->name));
            sb_str(&s, dir ? "/</a><td>" : "</a><td>");
            sb_str(&s, sz);
        }
        free(entries);
        sb_str(&s, "</table>");
        f->body = s.b;
        f->len = s.n;
        strcpy(f->ctype, "text/html");
        if (!slash) {
            size_t n = strlen(f->url);
            char *url = n <= SIZE_MAX - 2 ? realloc(f->url, n + 2) : NULL;
            if (!url) { strlcpy(f->err, "directory URL allocation failed", sizeof f->err); return false; }
            f->url = url; url[n] = '/'; url[n + 1] = 0;
        }
        return true;
    }
    n_handle doc = n_document_open(path, N_DOCUMENT_READ);
    if (doc < 0) {
        snprintf(f->err, sizeof f->err, "cannot open %s", path);
        return false;
    }
    n_result size = n_document_size(doc);
    if (size < 0 || (uint64_t)size >= SIZE_MAX) {
        n_handle_close(doc);
        strlcpy(f->err, "file length is not representable", sizeof f->err);
        return false;
    }
    size_t n = (size_t)size;
    f->body = malloc(n + 1);
    if (!f->body) { n_handle_close(doc); strlcpy(f->err, "out of memory", sizeof f->err); return false; }
    size_t got = 0;
    n_result r;
    while (got < n && (r = n_document_read_at(doc, got, f->body + got, n - got)) > 0) got += (size_t)r;
    n_handle_close(doc);
    f->status = 200;
    f->body[got] = 0;
    f->len = got;
    strlcpy(f->ctype, guess_type(path), sizeof f->ctype);
    return true;
}

/* ---------------------------------------------------------------- built-in pages */
static const char home_page_head[] =
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
    ;
/* then two links into the user's folder, which the user's folder decides (home_html) */
static const char home_page_tail[] =
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

/* The start page: the links to the web are fixed, those to the user's folder follow it. */
static char *home_html(void) {
    struct sb s = {0};
    char demo[1024];
    sb_str(&s, home_page_head);
    char *url = n_path_join(n_user_folder(), "Documents\\JavaScript\\index.html", demo, sizeof demo) >= 0 ? file_url(demo) : NULL;
    if (url) {
        sb_str(&s, "<a href=\"");
        sb_str(&s, url);
        sb_str(&s, "\">JavaScript demo<span>DOM, events, timers and modules</span></a>");
        free(url);
    }
    if ((url = file_url(n_user_folder()))) {
        sb_str(&s, "<a href=\"");
        sb_str(&s, url);
        sb_str(&s, "\">Your files<span>");
        sb_esc(&s, n_user_folder(), strlen(n_user_folder()));
        sb_str(&s, "</span></a>");
        free(url);
    }
    sb_str(&s, home_page_tail);
    return s.b;
}

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
    uint64_t start = debug_js ? n_clock_uptime_ms() : 0;
    doc_h = web_layout(doc, page_w(), page_h());
    doc_w = web_doc_width(doc);
    if (debug_js) { loop_profile.layout_ms += n_clock_uptime_ms() - start; loop_profile.layouts++; }
    clamp_scroll();
    need_layout = false;
    devtools_refresh(false);
}

static void set_title(void) {
    static char shown[256];
    char t[256];
    const char *s = doc ? web_title(doc) : "";
    snprintf(t, sizeof t, "%s - Web", *s ? s : cur_url);
    if (!ui || !strcmp(t, shown)) return;
    strcpy(shown, t);
    ui_window_title(ui, WINDOW, t);
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
    dt_net_update(t->devtools, result);
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
        .referrer=request->referrer,.referrer_policy=request->referrer_policy,
        .image_upgrade = request->kind == WEB_RESOURCE_IMAGE && request->image_upgrade,
        .cache_mode = request->cache_mode, .keepalive = request->keepalive,
        .fetch_group = request->fetch_group
        , .stream_response = request->stream_response, .top_level_url=web_url(doc)
        , .require_corp = request->require_corp
    };
    t->network_id = webnet_submit(network, &r, resource_completed, t);
    if (!t->network_id) { free(t); return false; }
    t->devtools = dt_net_begin(false, generation, request->id, request->kind, request->method, request->url, request->headers);
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

/* WebSocket: the webnet socket id is the document-visible id. Delivery stops
   with the document generation; webnet_cancel_generation releases the rest. */
static void socket_message(webnet *net, uint64_t id, const struct webnet_socket_message *m, void *opaque) {
    (void)net;
    uint64_t gen = (uint64_t)(uintptr_t)opaque;
    if (!doc || gen != generation) return;
    struct web_socket_event e = { .code = m->code, .data = m->data, .length = m->length, .protocol = m->protocol };
    switch (m->event) {
    case WEBNET_SOCKET_OPENED: e.type = WEB_SOCKET_OPENED; break;
    case WEBNET_SOCKET_RECEIVED: e.type = m->code == WEBNET_SOCKET_BINARY ? WEB_SOCKET_MESSAGE_BINARY : WEB_SOCKET_MESSAGE_TEXT; e.code = 0; break;
    case WEBNET_SOCKET_CLOSED: e.type = WEB_SOCKET_CLOSED; break;
    default: e.type = WEB_SOCKET_FAILED; break;
    }
    web_socket_event(doc, id, &e);
}
static uint64_t host_socket_open(void *opaque, const char *url, const char *protocols, const char *origin) {
    (void)opaque;
    if (!doc || quit || !url) return 0;
    char *headers = NULL;
    if (protocols && *protocols) {
        size_t n = strlen(protocols);
        static const char name[] = "Sec-WebSocket-Protocol: ";
        if (n > SIZE_MAX - sizeof name - 2 || !(headers = malloc(sizeof name - 1 + n + 3))) return 0;
        memcpy(headers, name, sizeof name - 1);
        memcpy(headers + sizeof name - 1, protocols, n);
        memcpy(headers + sizeof name - 1 + n, "\r\n", 3);
    }
    struct webnet_request r = {
        .kind = WEBNET_WEBSOCKET, .generation = generation, .url = url,
        .origin = origin ? origin : web_url(doc), .method = "GET", .headers = headers,
        .credentials = WEBNET_CREDENTIALS_INCLUDE, .top_level_url = web_url(doc)
    };
    uint64_t id = webnet_socket_open(network, &r, socket_message, (void *)(uintptr_t)generation);
    free(headers);
    return id;
}
static bool host_socket_send(void *opaque, uint64_t id, bool binary, const void *data, size_t length) {
    (void)opaque;
    return webnet_socket_send(network, id, binary ? WEBNET_SOCKET_BINARY : WEBNET_SOCKET_TEXT, data, length);
}
static bool host_socket_close(void *opaque, uint64_t id, unsigned code, const char *reason, size_t length) {
    (void)opaque;
    return webnet_socket_close(network, id, code, reason, length);
}
static size_t host_socket_buffered(void *opaque, uint64_t id) {
    (void)opaque;
    return webnet_socket_buffered(network, id);
}
static void host_socket_release(void *opaque, uint64_t id) {
    (void)opaque;
    webnet_socket_release(network, id);
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
        /* The new window shares this one's input, output and log, if it has them. */
        const char *args[]={"browser",url};int32_t handles[]={0,1,2};
        struct n_launch launch={.arguments=args,.argument_count=2,.handles=handles,.handle_count=3};
        char self[1024];
        n_handle app=find_program("browser",self,sizeof self)?n_app_launch(self,&launch):-N_ERR_NOT_FOUND;
        if (app==-N_ERR_NO_HANDLE) { launch.handle_count=0; app=n_app_launch(self,&launch); }
        if (app<0) console_add("error","Unable to open the form destination window");
        else n_handle_close(app);
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
static void presentation_service(void){
    if(!pip_window)return;
    struct gui_event event;bool resized=false;
    while(win_event(pip_window,&event,0)>0){
        if(event.type==EV_CLOSE){web_presentation_exit(pip_doc,2);return;}
        if(event.type==EV_RESIZE){resized=true;web_presentation_resize(pip_doc,pip_window->w,pip_window->h);}
    }
    if(!pip_doc||web_presentation_element(pip_doc,2)!=pip_node){web_presentation_exit(pip_doc,2);return;}
    uint64_t revision=web_avmedia_pip_revision(pip_doc,pip_node);
    if(resized||revision!=pip_revision){if(web_avmedia_pip_paint(pip_doc,pip_node,&pip_window->c)){win_update(pip_window);pip_revision=revision;}}
}
static bool host_presentation(void *opaque,web_doc *document,web_node *element,int mode){
    (void)opaque;
    if(mode==0||mode==4){if(fullscreen_doc!=document)return true;bool restored=w&&win_fullscreen(w,false)>=0;
        if(!restored&&mode==0)return false;
        browser_fullscreen=false;fullscreen_doc=NULL;need_layout=need_paint=true;return restored;}
    if(mode==3){if(pip_doc!=document)return true;if(pip_window)win_close(pip_window);pip_window=NULL;pip_doc=NULL;pip_node=NULL;pip_revision=0;return true;}
    web_doc *top=web_presentation_root(document);
    if(!w||!document||top!=doc||quit||loading||pending_navigation)return false;
    if(mode==1){if(win_fullscreen(w,true)<0)return false;browser_fullscreen=true;fullscreen_doc=document;focus=F_PAGE;need_layout=need_paint=true;return true;}
    if(mode==2){
        if(pip_window)return false;
        window_t *window;if(win_open(420,236,"Nocturne — Picture in Picture",WIN_RESIZABLE|WIN_ALWAYS_ON_TOP,&window)<0)return false;
        pip_window=window;pip_doc=document;pip_node=element;pip_revision=UINT64_MAX;
        web_presentation_resize(document,window->w,window->h);
        web_avmedia_pip_paint(document,element,&window->c);win_update(window);return true;
    }return false;
}
static bool host_video_present(void *opaque, web_doc *document, const struct web_video_patch *patch) {
    (void)opaque;
    presentation_service();
    /* Pending DOM/layout work does not replace the committed window raster.
       A snapshot owns that old surface's geometry and overlay pixels, not
       mutable layout boxes. Keep it moving until real paint replaces it;
       document, canvas, viewport, scroll and native-overlay checks below
       reject an obsolete surface without re-entering DOM or layout here. */
    if (!view.px || !patch || document != doc || quit || native_wait || pending_navigation ||
        sel_node || devtools_overlay_visible() || !patch->draw || patch->w <= 0 || patch->h <= 0 ||
        patch->canvas.px != view.px || patch->canvas.w != view.w ||
        patch->canvas.h != view.h || patch->canvas.pitch != view.pitch ||
        patch->scroll_x != scroll_x || patch->scroll_y != scroll_y ||
        patch->x < 0 || patch->y < 0 || patch->x > page_w() || patch->y > page_h() ||
        patch->w > page_w() - patch->x || patch->h > page_h() - patch->y ||
        view.pitch < view.w || view.h <= 0 ||
        (size_t)view.pitch > SIZE_MAX / sizeof(uint32_t) / (size_t)view.h) return false;
    uint64_t start=debug_js?n_clock_uptime_ms():0;
    patch->draw(patch->context, view.px + (size_t)patch->y * view.pitch + patch->x, view.pitch);
    __sync_synchronize();
    ui_surface_changed_rect(ui,VIEW,patch->x,patch->y,patch->w,patch->h);
    if(debug_js){loop_profile.video_patches++;loop_profile.video_present_ms+=n_clock_uptime_ms()-start;}
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
/* The engine's events become the browser's input, in the order they came. A field's
   text goes straight into what it edits (nothing else reads it); everything else waits
   in the inbox, then in evq while a page task runs, like the window's own input did. */
static struct gui_event *inbox;
static size_t inbox_first, inbox_n, inbox_capacity;
static int pointer_x = -1, pointer_y = -1; /* the last place the pointer was reported on the surface */
static void inbox_put(struct gui_event e) {
    if (inbox_first + inbox_n == inbox_capacity) {
        if (inbox_first) {
            memmove(inbox, inbox + inbox_first, inbox_n * sizeof *inbox);
            inbox_first = 0;
        } else {
            size_t capacity = inbox_capacity ? inbox_capacity * 2 : 64;
            void *grown = capacity <= SIZE_MAX / sizeof *inbox ? realloc(inbox, capacity * sizeof *inbox) : NULL;
            if (!grown) {
                if (!evq_failure_logged) { console_add("error", "Native event storage allocation failed"); evq_failure_logged = true; }
                return;
            }
            inbox = grown; inbox_capacity = capacity;
        }
    }
    inbox[inbox_first + inbox_n++] = e;
}
static void inbox_command(uint32_t command, int x, int y, uint32_t pressed) {
    inbox_put((struct gui_event){.type = EV_COMMAND, .key = command, .x = x, .y = y, .pressed = pressed});
}
static bool console_set(const char *text, size_t n) {
    if (n >= console_input_capacity) {
        char *grown = n < SIZE_MAX ? realloc(console_input, n + 1) : NULL;
        if (!grown) { console_add("error", "Console input allocation failed"); return false; }
        console_input = grown; console_input_capacity = n + 1;
    }
    memcpy(console_input, text, n); console_input[n] = 0;
    return true;
}
static void take_engine_events(void) {
    static char *text;
    static size_t text_capacity;
    size_t n;
    const void *p;
    bool echo = false;
    while ((p = ui_session_peek(ui, &n))) {
        struct ui_event e;
        bool ok = ui_event_read(p, n, &e) && e.window == WINDOW;
        if (ok && e.text_n >= text_capacity) {
            char *grown = e.text_n < SIZE_MAX ? realloc(text, e.text_n + 1) : NULL;
            if (grown) { text = grown; text_capacity = e.text_n + 1; }
            else ok = false;
        }
        if (ok) { memcpy(text, e.text, e.text_n); text[e.text_n] = 0; }
        ui_session_pop(ui);
        if (!ok) continue;
        const struct chrome *c = &chrome[e.view % 32];
        bool known = c->view == e.view && e.node != 0xFFFFFFFFu;
        int which = !known ? F_PAGE : e.node == c->address ? F_ADDR : e.node == c->find ? F_FIND
                    : e.node == c->console ? F_CONSOLE : F_PAGE;
        switch (e.type) {
        case UI_EVENT_CLICK:
            if (!known) break;
            if (e.node == c->back) inbox_command(CMD_BACK, 0, 0, 0);
            else if (e.node == c->forward) inbox_command(CMD_FORWARD, 0, 0, 0);
            else if (e.node == c->reload) inbox_command(CMD_RELOAD, 0, 0, 0);
            else if (e.node == c->home) inbox_command(CMD_HOME, 0, 0, 0);
            else if (e.node == c->history) inbox_command(CMD_HISTORY, 0, 0, 0);
            break;
        case UI_EVENT_TEXT:
        case UI_EVENT_SUBMIT:
            echo = true;
            if (which == F_ADDR) {
                if (!url_text_set(&address_text, text)) console_add("error", "Address allocation failed");
                else if (e.type == UI_EVENT_SUBMIT) inbox_command(CMD_GO, 0, 0, 0);
            } else if (which == F_FIND) {
                if (!url_text_set(&find_text, text)) { find_error = true; console_add("error", "Search input allocation failed"); }
                else inbox_command(e.type == UI_EVENT_SUBMIT ? CMD_FIND_NEXT : CMD_FIND, 0, 0, 0);
            } else if (which == F_CONSOLE) {
                if (console_set(text, e.text_n) && e.type == UI_EVENT_SUBMIT) inbox_command(CMD_EVAL, 0, 0, 0);
            }
            break;
        case UI_EVENT_FOCUS:
            if (e.node == 0xFFFFFFFFu) { inbox_put((struct gui_event){.type = e.a ? EV_FOCUS : EV_UNFOCUS}); break; }
            if (which == F_PAGE) break;
            if (e.a) {
                if (which == F_ADDR && !url_text_set(&address_text, cur_url)) console_add("error", "Address allocation failed");
                field_focus = which;
            } else if (field_focus == which) field_focus = F_PAGE;
            inbox_command(CMD_FOCUS, which, e.a ? 1 : 0, e.flags & UI_CANCEL);
            break;
        case UI_EVENT_KEY:
            inbox_put((struct gui_event){.type = EV_KEY, .key = (uint32_t)e.a, .mods = e.mods,
                                         .pressed = !(e.flags & UI_UP), .x = pointer_x, .y = pointer_y});
            break;
        case UI_EVENT_CLOSE:
            if (e.flags & UI_GONE) w = NULL;
            inbox_put((struct gui_event){.type = EV_CLOSE});
            break;
        case UI_EVENT_CANVAS: {
            canvas_t *surface = ui_surface(ui, VIEW, (int)e.a, (int)e.b, 0);
            if (!surface) { console_add("error", "Page surface allocation failed"); break; }
            view = *surface;
            need_paint = true;
            inbox_put((struct gui_event){.type = EV_RESIZE, .x = view.w, .y = view.h});
            break;
        }
        case UI_EVENT_POINTER: {
            pointer_x = (int)floor(e.a); pointer_y = (int)floor(e.b);
            uint32_t type = e.flags & UI_DOWN ? EV_MOUSE_DOWN : e.flags & UI_UP ? EV_MOUSE_UP : EV_MOUSE_MOVE;
            inbox_put((struct gui_event){.type = type, .x = pointer_x, .y = pointer_y,
                                         .buttons = UI_BUTTONS(e.flags), .mods = e.mods});
            break;
        }
        case UI_EVENT_WHEEL:
            inbox_put((struct gui_event){.type = EV_WHEEL, .x = pointer_x, .y = pointer_y,
                                         .wheel = (int32_t)e.a, .mods = e.mods});
            break;
        case UI_EVENT_DIRECT:
            inbox_put((struct gui_event){.type = EV_DIRECT, .key = e.a ? 1 : 0});
            break;
        case UI_EVENT_FRAME: /* only a time: safe while a script waits on the C stack */
            frame_asked = false;
            if (doc) web_rendering_now(doc, e.a);
            break;
        default: break; /* the window's own size: the surface's comes as UI_EVENT_CANVAS */
        }
    }
    /* A field's new value goes back in the next view: the engine takes a value it sent and
       the app only returned as no change, so a value the browser sets later (a cleared
       console line) is never mistaken for an old one of its own. */
    if (echo) show_chrome();
}
static int browser_event(struct gui_event *event, int timeout) {
    if (!ui) return -1;
    ui_session_pump(ui);
    take_engine_events();
    if (!inbox_n && timeout != 0 && w) {
        /* One wait for the window, transports and Atomics.waitAsync waiters. */
        static bool window_watched;
        n_handle queue=web_wait_queue();
        if(queue<0)return -1;
        if(!window_watched)window_watched=n_events_watch(queue,w->handle,N_EVENT_RECEIVABLE,UINT64_C(0x42)<<56)>=0;
        int wait=window_watched||(timeout>=0&&timeout<10)?timeout:10;
        int engine=ui_session_timeout(ui); /* the caret, animations, a picture to finish */
        if(engine>=0&&(wait<0||engine<wait))wait=engine;
        struct n_event events[64];
        int ready=webnet_wait(network,queue,wait,events,64);
        if(ready<0)return -1;
        web_wait_events(events,(size_t)ready);
        ui_session_pump(ui);
        take_engine_events();
    }
    if (!inbox_n) return w ? 0 : -1;
    *event = inbox[inbox_first++];
    if (!--inbox_n) inbox_first = 0;
    /* Release/capture cancellation is native state, even while author JS is
       running. Never hit-test, dispatch, or lay out mutable DOM at dequeue. */
    native_active_event(event);browser_hover_boundary(&evq_hover_tail_valid,event);
    if(debug_js && (event->type==EV_MOUSE_MOVE ||
       event->type==EV_MOUSE_DOWN || event->type==EV_MOUSE_UP)) {
        static uint64_t sequence;
        fprintf(stderr,"[web-input] NATIVE_POINTER %lu %d %d\n",
                (unsigned long)++sequence,event->x,event->y);
    }
    return 1;
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
    if(event->type==EV_COMMAND && !sel_node) {
        switch(event->key) {
        case CMD_BACK: return hpos>0;
        case CMD_FORWARD: return hpos<nhist-1;
        case CMD_RELOAD: return cur_url[0]!=0;
        case CMD_HOME: case CMD_GO: return true;
        }
    }
    return false;
}
/* Keep an explicit user stop available during long author tasks without
   touching a half-mutated document or running another page callback. All
   other native events retain FIFO ownership until the outer JS task ends. */
static bool host_script_checkpoint(void *opaque) {
    (void)opaque;
    static uint64_t last;
    uint64_t now=n_clock_uptime_ms();
    if(quit)return false;
    if(now>=last && now-last<10)return true;
    last=now;
    webnet_service(network,now);
    uint64_t until=now+2;
    struct gui_event e;
    while(w && n_clock_uptime_ms()<until) {
        int event=browser_event(&e,0);
        if(event<0){quit=true;return false;}
        if(!event)break;
        if(e.type==EV_CLOSE){quit=true;return false;}
        if(e.type==EV_WHEEL && console_open && e.y>=page_h() && e.y<view.h) {
            devtools_wheel(e.x,e.y,e.wheel);redraw_console();continue;
        }
        /* Escape in native chrome/fullscreen is an ordinary UI command, not
           permission to permanently abort the page's author execution. Keep
           it queued for the normal owner rather than swallowing it here. */
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_ESC && !browser_fullscreen &&
           focus==F_PAGE && !sel_node && !find_open)return false;
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_F12) {
            int old_top=page_h(),old_height=console_h();
            console_open=!console_open;
            if(console_open)focus=F_CONSOLE;else if(focus==F_CONSOLE)focus=F_PAGE;
            need_layout=need_paint=true;
            /* Chrome only: never read the document's half-built layout here.
               The exposed page area is restored after the task unwinds. */
            if(view.px){
                if(console_open)draw_console();
                else gfx_fill(&view,0,old_top,view.w,old_height,ui_theme.background);
                ui_surface_changed(ui,VIEW);
            }
            show_chrome();
            continue;
        }
        if(!defer_native_event(&e))return false;
        if(native_navigation_command(&e))return false;
    }
    native_scroll_queue();
    return !quit;
}
static unsigned host_window_state(void *opaque) {
    (void)opaque;int state=win_state(w);
    if(state<0)return 0;
    return ((state&WIN_STATE_VISIBLE)?WEB_WINDOW_VISIBLE:0) |
           ((state&WIN_STATE_FOCUSED) && focus==F_PAGE?WEB_WINDOW_FOCUSED:0);
}
static void host_window_size(void *opaque, int *width, int *height) {
    (void)opaque;*width=w?w->w:0;*height=w?w->h:0;
}
static bool host_cookie_enabled(void *opaque) {
    (void)opaque;
    /* webnet_create succeeds only with an actual session cookie jar. */
    return network != NULL;
}
static char *host_cookie_get(void *opaque, const char *url) {
    (void)opaque;
    const char *top_level_url=doc?web_url(doc):"";
    long n = webnet_cookie_get_site(network, url, top_level_url, NULL, 0);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (out && webnet_cookie_get_site(network, url, top_level_url, out, (size_t)n + 1) < 0) { free(out); out = NULL; }
    return out;
}
static void host_cookie_set(void *opaque, const char *url, const char *value) {
    (void)opaque;
    if (webnet_cookie_set_site(network, url, doc?web_url(doc):"", value) < 0) console_add("error", "Cookie storage allocation failed");
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
/* Join a request already owned by the document (modulepreload), without a
   duplicate fetch or re-entering author tasks. The caller owns completed
   throughout this native pump and postpones document/pending retirement. */
static bool host_wait_request(void *opaque, uint64_t resource_id, const bool *completed) {
    (void)opaque;
    if (!completed || !doc || native_wait || quit || pending_navigation) return false;
    web_doc *document = doc;
    uint64_t owner_generation = generation;
    native_wait = true;
    native_wait_snapshot=false;
    sync_stopped = false;
    prepare_native_snapshot(false);
    native_wait_snapshot=web_native_input_ready(doc,true);
    if(native_wait_snapshot && need_paint && !quit)redraw();
    while (!*completed && !quit && !sync_stopped && !pending_navigation &&
           doc == document && generation == owner_generation) {
        bool present = false;
        for (struct transfer *t = transfers; t; t = t->next)
            if (!t->detached && t->generation == owner_generation &&
                t->resource_id == resource_id) { present = true; break; }
        if (!present) break;
        uint64_t now = n_clock_uptime_ms();
        webnet_pump(network, now);
        if (*completed) break;
        struct gui_event e;
        int timeout = webnet_timeout(network, now);
        int event = browser_event(&e, timeout);
        if (event < 0) { quit = true; break; }
        if (event) handle_native_wait(&e);
        redraw_console();
    }
    native_wait = false;
    native_wait_snapshot=false;
    return *completed && !quit && !sync_stopped && !pending_navigation &&
           doc == document && generation == owner_generation;
}
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
        .cache_mode = request->cache_mode, .top_level_url=web_url(doc)
    };
    uint64_t id = webnet_submit(network, &r, synchronous_completed, s);
    if (!id) { free(s); strcpy(response->error, "module request could not be started"); return false; }
    native_wait = true;
    native_wait_snapshot=false;
    sync_stopped = false;
    prepare_native_snapshot(false);
    native_wait_snapshot=web_native_input_ready(doc,true);
    if (native_wait_snapshot && need_paint && !quit) redraw();
    uint64_t started = n_clock_uptime_ms();
    while (!s->ready && !quit && !sync_stopped && !pending_navigation &&
           doc == s->document && generation == s->generation) {
        uint64_t now = n_clock_uptime_ms();
        if (now - started >= WEBNET_TIMEOUT_MS) { sync_stopped = true; break; }
        webnet_pump(network, now);
        if (s->ready) break;
        struct gui_event e;
        int timeout = webnet_timeout(network, now);
        int event = browser_event(&e, timeout);
        if (event < 0) { quit = true; break; }
        if (event) handle_native_wait(&e);
        redraw_console();
    }
    native_wait = false;
    native_wait_snapshot=false;
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
    .socket_open = host_socket_open, .socket_send = host_socket_send, .socket_close = host_socket_close,
    .socket_buffered = host_socket_buffered, .socket_release = host_socket_release,
    .sync_request = host_sync_request,
    .wait_request = host_wait_request,
    .navigate = host_navigate, .console = host_console, .scroll = host_scroll,
    .navigate_form = host_navigate_form,
    .scroll_to = host_scroll_to, .history = host_history,
    .window_state=host_window_state,.window_size=host_window_size,
    .navigation_pending=host_navigation_pending,
    .script_checkpoint=host_script_checkpoint,
    .video_present=host_video_present,
    .presentation=host_presentation,
    .cookie_get = host_cookie_get, .cookie_set = host_cookie_set,
    .cookie_enabled = host_cookie_enabled
    , .navigate_mode = host_navigate_mode
    , .storage = host_storage, .storage_events = true
    , .media_range = true
    , .display_frames = true
};

static void navigation_completed(webnet *net, uint64_t id, uint64_t gen,
                                 const struct webnet_response *result, void *opaque) {
    (void)net; (void)opaque;
    if (id != navigation_id || gen != navigation_generation) return;
    navigation_id = 0;
    dt_net_update(dt_nav_serial, result);
    /* This callback observes the complete response, never its first byte.
       Generation/id checks above prevent a canceled request changing timing. */
    if (!result->error[0] && result->status > 0) {
        navigation_timing.response_end_ms = n_clock_uptime_ms();
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
        html = !strcasecmp(web_response_url(f), HOME) ? home_html() : strdup("<!doctype html><title>blank</title>");
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
    document_host.net_log = net_log;
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
    evq_first=evq_n=evq_scroll_at=0;evq_hover_tail_valid=false;
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
    navigation_timing.navigation_start_ms = n_clock_uptime_ms();
    navigation_mode = mode;
    loading = true;
    char what[256]; snprintf(what, sizeof what, "Loading %.240s", url); set_status(what);
    show_chrome();
    if (has_prefix(url, "about:") || has_prefix(url, "file://")) {
        navigation_timing.fetch_start_ms = n_clock_uptime_ms();
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
            navigation_timing.response_end_ms = n_clock_uptime_ms();
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
        /* No document is a browser-initiated navigation (no client), not an
           opaque one: SameSite=Strict cookies remain eligible. */
        .credentials = WEBNET_CREDENTIALS_INCLUDE, .top_level_url=doc?web_url(doc):NULL
    };
    navigation_timing.fetch_start_ms = n_clock_uptime_ms();
    navigation_timing.fetch_valid = true;
    dt_net_clear(); /* the Network panel follows the top-level document */
    navigation_id = webnet_submit(network, &rq, navigation_completed, NULL);
    if (navigation_id) dt_nav_serial = dt_net_begin(true, navigation_generation, 0, -1, rq.method, url, rq.headers);
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
    if (n_path_is_complete(text)) return file_url(text); /* C:\Users\User\a.html, \Users\User\a.html or /Users/User/a.html */
    if (strstr(text, "://") || has_prefix(text, "about:")) {}
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
/* Enter in a field: the default button is clicked, so it is the submitter. */
static void implicit_submit(web_node *field) {
    web_node *submitter = web_form_implicit_submission(doc, field);
    if (submitter && submitter != field) page_click(submitter, 0, 0, true);
    else if (submitter) submit(field);
}
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
        .x = e ? e->x : 0, .y = e ? e->y : 0,
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
    struct web_event event={.x=e->x,.y=e->y,.button=e->type==EV_MOUSE_MOVE?-1:0,.buttons=e->buttons,
        .ctrl=(e->mods&NMOD_CTRL)!=0,.shift=(e->mods&NMOD_SHIFT)!=0,.alt=(e->mods&NMOD_ALT)!=0};
    return web_pointer_capture_target(doc,&event);
}

static void page_focus(web_node *node) {
    if (!doc) return;
    web_node *old = web_focused(doc);
    if (node && old == node) return;
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
    if (!needed) { needed = web_autocomplete_options(doc, n, NULL, 0); sel_autocomplete = needed > 0; }
    int capacity = MAX(needed, 1);
    if (capacity > sel_capacity) {
        if ((size_t)capacity > SIZE_MAX / sizeof *sel_labels) return;
        void *grown = realloc(sel_labels, (size_t)capacity * sizeof *sel_labels);
        if (!grown) { console_add("error", "Control option storage allocation failed"); return; }
        sel_labels = grown; sel_capacity = capacity;
    }
    if (sel_autocomplete) {
        sel_n = web_autocomplete_options(doc, n, sel_labels, sel_capacity);
        sel_datalist = sel_autocomplete = sel_n > 0;
    } else sel_n = sel_datalist ? web_datalist_options(doc, n, sel_labels, sel_capacity) :
        web_select_options(doc, n, sel_labels, sel_capacity, &sel);
    if (sel_n <= 0 || sel_n != needed) return;
    int x, y, ww, h;
    if (!web_node_rect(doc, n, &x, &y, &ww, &h)) return;
    sel_node = n;
    sel_cur = sel < 0 ? 0 : sel;
    sel_x = x - scroll_x;
    sel_w = ww;
    for (int i = 0; i < sel_n; i++) sel_w = MAX(sel_w, gfx_text_width(sel_labels[i], FONT_SMALL) + 16);
    sel_x = MAX(0, MIN(sel_x, page_w() - sel_w));
    sel_y = y + h - scroll_y;
    int room = (view.h - sel_y) / SEL_H;
    if (room < MIN(sel_n, 6)) { /* not enough room below: open upwards */
        int up = MIN(sel_n, (y - scroll_y) / SEL_H);
        if (up > room) sel_y = y - scroll_y - up * SEL_H;
    }
    room = (view.h - sel_y) / SEL_H;
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

/* The fields edit themselves in the engine; these give them the keyboard and act on what they hold. */
static void addr_focus(void) {
    if (!url_text_set(&address_text, cur_url)) { console_add("error", "Address allocation failed"); return; }
    focus = F_ADDR;
    focus_renew = true; /* again, even when it has the keyboard already: select all of it */
}

static void find_next(int from) {
    if (!doc) return;
    int result = web_find(doc, find_buf, from);
    find_error = result == -2;
    if (find_error) { console_add("error", "Search query allocation failed"); return; }
    find_y = result;
    if (find_y >= 0 && (find_y < scroll_y || find_y > scroll_y + page_h() - 40)) scroll_to(find_y - page_h() / 3);
}

static void find_close(void) {
    find_open = false;
    if (focus == F_FIND) focus = F_PAGE;
    if (doc) web_find(doc, "", 0);
}

/* Keys for the DevTools panel while the console has the keyboard; the line itself is the field's. */
static void console_key(const struct gui_event *e) {
    if(dt_tab!=DT_CONSOLE){devtools_key(e);return;}
    uint32_t k=e->key;bool ctrl=e->mods&NMOD_CTRL;
    if(ctrl&&k>='A'&&k<='Z')k+=32;
    if(ctrl&&(k=='['||k==']')){devtools_set_tab(k==']'?DT_NETWORK:DT_ELEMENTS);return;}
    if(k==NKEY_PGUP){console_scroll(console_visible());return;}
    if(k==NKEY_PGDN){console_scroll(-console_visible());return;}
    if(ctrl&&k==NKEY_HOME){console_scroll(INT_MAX);return;}
    if(ctrl&&k==NKEY_END){console_scroll(-INT_MAX);return;}
    if(ctrl&&k=='c'&&(e->mods&NMOD_SHIFT)){
        bool copied=console_copy_log();console_skip=0;
        console_add(copied?"log":"error",copied?"Retained console log copied to Nocturne clipboard.":"Console log copy failed.");return;
    }
}

static void console_eval(void) {
    size_t length=strlen(console_input);
    if(!length)return;
    console_skip=0;
    console_add("input",console_input);
    if(!web_console_eval(doc,console_input,length))console_add("error","Console execution failed or the page runtime is unavailable.");
    console_input[0]=0;
    need_layout=true;
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
    if(e->pressed&&browser_fullscreen&&(e->key==NKEY_ESC||((e->mods&NMOD_CTRL)&&(e->key=='l'||e->key=='L')))){
        web_presentation_exit(fullscreen_doc,1);if(e->key==NKEY_ESC)return;
    }
    bool ctrl = e->mods & NMOD_CTRL, alt = e->mods & NMOD_ALT;
    uint32_t k = e->key;
    if (k >= 'A' && k <= 'Z' && (ctrl || alt)) k += 32;
    if (ctrl && (e->mods & NMOD_SHIFT) && k == 'h') {
        sel_node = NULL; web_autocomplete_settings(); need_paint = true; return;
    }
    if (k == NKEY_F12) {
        console_open = !console_open;
        if(console_open)focus=F_CONSOLE;else if(focus==F_CONSOLE)focus=F_PAGE;
        if(!console_open){dt_picking=false;dt_set_hover(NULL);}
        need_layout = true;
        return;
    }
    /* Ctrl+Shift+C copies the console log while typing in the console. */
    if (ctrl && (e->mods & NMOD_SHIFT) && (k == 'i' || k == 'j' ||
        (k == 'c' && !(console_open && focus == F_CONSOLE && dt_tab == DT_CONSOLE)))) {
        devtools_shortcut(k);
        return;
    }
    if (console_open && ctrl && k == 'k') {
        devtools_clear();
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
        focus_renew = true; /* again, when it has the keyboard already */
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
        int room = MAX((view.h - sel_y) / SEL_H, 1);
        if (k == NKEY_ESC) sel_node = NULL;
        else if (k == NKEY_ENTER || k == ' ') choose_select(sel_cur);
        else if (k == NKEY_UP || k == NKEY_DOWN) {
            sel_cur = MAX(0, MIN(sel_n - 1, sel_cur + (k == NKEY_UP ? -1 : 1)));
            if (sel_cur < sel_top) sel_top = sel_cur;
            if (sel_cur >= sel_top + room) sel_top = sel_cur - room + 1;
        }
        return;
    }
    if (focus == F_ADDR || focus == F_FIND) return; /* a key the field had no use for */
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
        if (r == 2) implicit_submit(web_focused(doc));
        if (r) return;
        if (e->key >= 32 && e->key < 127) return; /* typed into a control that ignored it */
    }
    if (k == NKEY_BACKSPACE && !ctrl) return go_history(-1);
    if (k == NKEY_ESC && find_open) {
        find_close();
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
    if (!anchor && web_media_activate(doc,target)) { flush_dom_layout(); return; }
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
        if (!web_hit_test(doc,document_x(x),y+scroll_y,&pointer) || pointer.kind!=WEB_HIT_DETAILS || pointer.node!=hit.node) return;
    }
    switch (hit.kind) {
    case WEB_HIT_LINK:
        if (hit.href) {
            if (has_prefix(hit.href, "file://") && !has_prefix(web_url(doc), "file://") &&
                !has_prefix(web_url(doc), "about:")) {
                set_status("A remote page cannot open a local file");
                return;
            }
            if (!web_frame_navigate(doc, anchor, hit.href)) queue_navigation(hit.href, NULL, NAV_PUSH);
        }
        return;
    case WEB_HIT_TEXT_INPUT:
    case WEB_HIT_TEXTAREA: break; /* focusing belongs to the preceding mousedown */
    case WEB_HIT_FILE:
        if (web_input_choose_files(doc, hit.node)) {
            dispatch_native("input", hit.node, &native, false);
            dispatch_native("change", hit.node, &native, false);
            if (web_focused(doc) == hit.node) { free(focus_value); focus_value = strdup(web_control_value(hit.node)); }
            flush_dom_layout();
        }
        break;
    case WEB_HIT_CHECKBOX:
    case WEB_HIT_RADIO:
        if (input_events) { dispatch_native("input", hit.node, &native, false); dispatch_native("change", hit.node, &native, false); }
        break;
    case WEB_HIT_SUBMIT: submit(hit.node); return;
    case WEB_HIT_BUTTON: web_reset(doc,hit.node); flush_dom_layout(); break;
    case WEB_HIT_DETAILS:
        web_toggle(doc,hit.node); flush_dom_layout(); break;
    case WEB_HIT_SELECT:
        open_select(hit.node);
        break;
    default: break;
    }
}

static void scrollbar_drag(int my) {
    int ph = page_h();
    if (doc_h <= ph) return;
    int th = MAX(24, (int)((long)ph * ph / doc_h));
    int range = ph - th;
    if (range <= 0) return;
    scroll_to((int)((long)(my - drag_off) * max_scroll() / range));
}

static void mouse_down(const struct gui_event *e) {
    if (!(e->buttons & 1)) return;
    if(pressed_node)return; /* an additional button is not a new primary press */
    web_active_release(doc);pressed_node=NULL;
    int x = e->x, y = e->y;
    if (devtools_pointer_down(x, y)) return;
    if (sel_node) {
        int vis = MIN(sel_n - sel_top, (view.h - sel_y) / SEL_H);
        if (x >= sel_x && x < sel_x + sel_w && y >= sel_y && y < sel_y + vis * SEL_H) choose_select(sel_top + (y - sel_y) / SEL_H);
        else sel_node = NULL;
        return;
    }
    if (x >= view.w - SCRW) {
        int ph = page_h();
        if (doc_h <= ph) return;
        int th = MAX(24, (int)((long)ph * ph / doc_h));
        int ty = (int)((long)scroll_y * (ph - th) / max_scroll());
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
        pressed_node = web_node_at(doc, document_x(x), y + scroll_y);
        web_active_press(doc,pressed_node);
        web_frame_address_select(doc, pressed_node); need_paint = true;
        /* Dispatch may cancel the compatibility mouse event, remove the
           control or change its frame. Resolve the current focus area after
           it, before mouseup/click handlers observe activeElement. */
        if (dispatch_native("mousedown", pressed_node, e, true)) {
            web_node *area = NULL;
            if (web_pointer_focus_target(doc, pressed_node, &area)) page_focus(area);
        }
    }
}

/* the status bar shows the link under the mouse */
static void mouse_move(const struct gui_event *e) {
    if (drag_sb) {
        scrollbar_drag(e->y);
        redraw();
        return;
    }
    char h[sizeof hover] = "";
    web_node *node = NULL;
    if (doc && !sel_node && e->y >= 0 && e->y < page_h() && e->x >= 0 && e->x < page_w()) {
        struct web_hit hit;
        if (web_hit_test(doc, document_x(e->x), e->y + scroll_y, &hit) && hit.href) strlcpy(h, hit.href, sizeof h);
        node = web_node_at(doc, document_x(e->x), e->y + scroll_y);
    }
    if (doc && !native_wait && !web_script_running(doc)) {
        web_node *captured=native_capture_target(e);
        if(captured)node=captured;
        struct web_event event = {
            .x=e->x, .y=e->y, .buttons=e->buttons,
            .ctrl=(e->mods & NMOD_CTRL)!=0, .shift=(e->mods & NMOD_SHIFT)!=0,
            .alt=(e->mods & NMOD_ALT)!=0
        };
        web_hover(doc, node, &event);
        hover_node = node;
        flush_dom_layout();
    }
    if (strcmp(h, hover)) {
        strlcpy(hover, h, sizeof hover);
        show_chrome();
    }
}

/* What the widgets around the page asked for, in its turn with the rest of the input. */
static void command(const struct gui_event *e) {
    switch (e->key) {
    case CMD_BACK: go_history(-1); break;
    case CMD_FORWARD: go_history(1); break;
    case CMD_RELOAD:
        if (loading && !native_wait) stop_navigation();
        else if (cur_url[0]) navigate(cur_url, NULL, NAV_RELOAD);
        break;
    case CMD_HOME: navigate(HOME, NULL, NAV_PUSH); break;
    case CMD_HISTORY: sel_node = NULL; web_autocomplete_settings(); need_paint = true; break;
    case CMD_GO: focus = F_PAGE; sel_node = NULL; go_addr(addr); break;
    case CMD_FIND: find_next(find_y >= 0 ? find_y : scroll_y); break;
    case CMD_FIND_NEXT: if (find_buf[0]) find_next(find_y >= 0 ? find_y + 1 : scroll_y); break;
    case CMD_EVAL: console_eval(); break;
    case CMD_FOCUS: /* x: the field; y: it got the keyboard (1) or lost it; pressed: Escape gave it up */
        if (e->y) { focus = focus_shown = e->x; sel_node = NULL; }
        else if (focus == e->x) {
            focus = focus_shown = F_PAGE;
            if (e->x == F_FIND && e->pressed) find_close();
        }
        break;
    }
}

static void handle(const struct gui_event *e) {
    bool console_only=console_open && !dt_resizing &&
        ((e->type==EV_KEY && focus==F_CONSOLE) ||
         (e->type==EV_WHEEL && e->y>=page_h() && e->y<view.h));
    switch (e->type) {
    case EV_CLOSE: quit = true; return;
    case EV_COMMAND:
        command(e);
        /* the keyboard moving between fields changes only the widgets */
        if (e->key == CMD_FOCUS && !(e->x == F_FIND && e->pressed) && !need_layout && !need_paint) { show_chrome(); return; }
        break;
    case EV_UNFOCUS: web_pointer_cancel(doc);web_active_release(doc);pressed_node=NULL;break;
    case EV_DIRECT: direct_shown = e->key; need_paint = true; break; /* key 0: the window shows again */
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
        if (dt_resizing || dt_swallow_up) { dt_resizing = dt_swallow_up = false; pressed_node = NULL; break; }
        if (doc) {
            web_node *capture=native_capture_target(e);
            bool inside=e->y >= 0 && e->y<page_h() && e->x>=0 && e->x<page_w();
            web_node *target=capture?capture:inside?web_node_at(doc,document_x(e->x),e->y+scroll_y):NULL;
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
        if(!devtools_pointer_move(e->x,e->y))mouse_move(e);
        flush_dom_layout();
        if (need_paint && !quit) redraw();
        else if (!quit) redraw_console();
        return;
    case EV_WHEEL:
        if (console_open && e->y >= page_h() && e->y < view.h) {
            devtools_wheel(e->x, e->y, e->wheel);
        } else if (sel_node) {
            int room = MAX((view.h - sel_y) / SEL_H, 1);
            sel_top = MAX(0, MIN(sel_n - room, sel_top + e->wheel));
        } else {
            web_node *target = doc && e->y >= 0 && e->y < page_h() ?
                               web_node_at(doc, document_x(e->x), e->y + scroll_y) : NULL;
            if (dispatch_native("wheel", target, e, true)) {
                if (e->mods & NMOD_SHIFT) scroll_input(target,e->wheel*LINE,0,0);
                else scroll_input(target,0,e->wheel*LINE,0);
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
    if(console_only && !need_layout && !need_paint &&
       (e->type==EV_WHEEL || focus==F_CONSOLE)) {
        if(!quit)redraw_console();
        return;
    }
    if (need_layout) relayout();
    if (!quit) redraw();
}

/* This pump intentionally does not dispatch DOM events, run JS, or destroy a document.
   QuickJS's synchronous module loader can be on the C stack while it is used. */
static void handle_native_wait(const struct gui_event *e) {
    if (e->type == EV_CLOSE) { quit = true; sync_stopped = true; return; }
    if (e->type == EV_DIRECT) {
        direct_shown = e->key;
        if (!direct_shown) { need_paint = true; if (native_wait_snapshot) redraw(); }
        return;
    }
    if (e->type == EV_RESIZE) {
        prepare_native_snapshot(true);native_wait_snapshot=web_native_input_ready(doc,true);
        if(native_wait_snapshot)redraw();return;
    }
    if (e->type == EV_KEY && e->pressed) {
        uint32_t k = e->key;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (e->key == NKEY_ESC) { if(browser_fullscreen)web_presentation_exit(fullscreen_doc,1);else sync_stopped = true; return; }
        if (e->key == NKEY_F12) {
            console_open = !console_open;
            if (console_open) focus = F_CONSOLE; else if (focus == F_CONSOLE) focus = F_PAGE;
            prepare_native_snapshot(true);native_wait_snapshot=web_native_input_ready(doc,true);
            if(native_wait_snapshot)redraw();
            else if(console_open&&view.px){draw_console();ui_surface_changed_rect(ui,VIEW,0,page_h(),view.w,console_h());}
            show_chrome();
            return;
        }
        if ((e->mods & NMOD_CTRL) && k == 'l') { addr_focus(); show_chrome(); return; }
        if (e->key == NKEY_F5 || ((e->mods & NMOD_CTRL) && k == 'r')) {
            queue_navigation(cur_url, NULL, NAV_RELOAD); return;
        }
        if (e->mods & NMOD_ALT) {
            if (e->key == NKEY_LEFT) { go_history(-1); return; }
            if (e->key == NKEY_RIGHT) { go_history(1); return; }
        }
    }
    /* The toolbar's commands act now; what needs the document (find, the console) waits. */
    if (e->type == EV_COMMAND && e->key != CMD_FIND && e->key != CMD_FIND_NEXT && e->key != CMD_EVAL &&
        !(e->key == CMD_FOCUS && e->x == F_FIND && e->pressed)) {
        command(e); show_chrome(); return;
    }
    if (e->type == EV_MOUSE_DOWN && e->x >= view.w - SCRW) {
        mouse_down(e); redraw(); return;
    }
    if (e->type == EV_MOUSE_MOVE && drag_sb) { scrollbar_drag(e->y); redraw(); return; }
    if (e->type == EV_MOUSE_UP && drag_sb) { drag_sb = false; redraw(); return; }
    if(e->type==EV_WHEEL && console_open && e->y>=page_h() && e->y<view.h) {
        devtools_wheel(e->x,e->y,e->wheel);redraw_console();return;
    }
    if(!defer_native_event(e)){sync_stopped=true;return;}
    native_scroll_queue();
}

static void handle_deferred_input(void) {
    struct deferred_input entry=evq[evq_first++];
    if(!--evq_n){evq_first=evq_scroll_at=0;evq_failure_logged=false;evq_hover_tail_valid=false;}
    if(entry.event.type==EV_WHEEL && entry.document.document &&
       (entry.generation!=generation || !web_input_target_valid(doc,&entry.document)))return;
    if(!entry.scrolled){handle(&entry.event);return;}
    if(!web_input_target_valid(doc,&entry.target))return;
    /* The absence of non-passive listeners was established at native scroll
       commit. Later listeners cannot retroactively cancel that physical input.
       Notification still runs once, on the original surviving target. */
    dispatch_native("wheel",entry.target.node,&entry.event,false);
    if(need_layout)relayout();
    if(need_paint && !quit)redraw();
}

static void document_step(uint64_t now) {
    presentation_service();
    if (!doc || quit) { web_media_background(now); return; }
    if (scroll_event_pending && !native_wait && !web_script_running(doc)) {
        scroll_event_pending = false;
        web_document_scroll(doc);
    }
    uint64_t tick_start = debug_js ? n_clock_uptime_ms() : 0;
    web_tick(doc, now);
    if (debug_js) {
        uint64_t elapsed = n_clock_uptime_ms() - tick_start;
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
    } else if (need_paint) redraw();
    else if(media_paint)redraw_damage();
    redraw_console();
    flush_dom_layout();
    if (!refresh_seen && !loading) {
        int delay;
        const char *url = web_refresh_url(doc, &delay);
        if (url) {
            refresh_seen = true;
            if (delay <= 10 && strcmp(url, cur_url)) {
                if (url_text_set(&refresh_text, url)) refresh_at = n_clock_uptime_ms() + (uint64_t)MAX(delay, 0) * 1000 + 1;
                else console_add("error", "Refresh URL allocation failed");
            }
        }
    }
}

static void report_loop_profile(uint64_t now) {
    if (!debug_js) return;
    if (!loop_profile.since) { loop_profile.since = now; return; }
    if (now - loop_profile.since < 2000) return;
    char message[1280];
    snprintf(message, sizeof message,
             "Browser loop profile: wall %lu ms, ticks %u/%lu ms/max %lu, host layout %u/%lu ms, draw %u/%lu ms, update %lu ms, net %lu ms, wait %lu ms, parallel jobs %lu (inclusive)",
             (unsigned long)(now-loop_profile.since), loop_profile.ticks,
             (unsigned long)loop_profile.tick_ms, (unsigned long)loop_profile.tick_max_ms,
             loop_profile.layouts, (unsigned long)loop_profile.layout_ms,
             loop_profile.draws, (unsigned long)loop_profile.draw_ms,
             (unsigned long)loop_profile.update_ms, (unsigned long)loop_profile.net_ms,
             (unsigned long)loop_profile.wait_ms, (unsigned long)parallel_jobs());
    unsigned partial=loop_profile.partial_draws,patches=loop_profile.video_patches;
    unsigned consoles=loop_profile.console_draws;
    uint64_t console_ms=loop_profile.console_ms,console_update=loop_profile.console_update_ms;
    uint64_t partial_ms=loop_profile.partial_ms,partial_update=loop_profile.partial_update_ms,video_ms=loop_profile.video_present_ms;
    memset(&loop_profile, 0, sizeof loop_profile);
    loop_profile.since = now;
    host_console(NULL, 0, message);
    {
        /* Private pages of this process, against what malloc holds and uses:
           the difference between process and heap is mapped memory. */
        static struct n_procinfo processes[256];
        size_t heap = 0, used = 0; unsigned long process_kb = 0;
        int count = proclist(processes, 256), self = (int)n_app_self();
        for (int i = 0; i < count; i++) if (processes[i].pid == self) process_kb = processes[i].mem_kb;
        malloc_usage(&heap, &used);
        snprintf(message, sizeof message, "Browser memory: process %lu KiB, malloc heap %lu KiB, in use %lu KiB",
                 process_kb, (unsigned long)(heap / 1024), (unsigned long)(used / 1024));
        host_console(NULL, 0, message);
    }
    if (web_wait_report(message, sizeof message) > 0) host_console(NULL, 0, message);
    if (web_paint_report(message, sizeof message) > 0) host_console(NULL, 0, message);
    if (web_style_report(message, sizeof message) > 0) host_console(NULL, 0, message);
    if (web_box_report(message, sizeof message) > 0) host_console(NULL, 0, message);
    if(consoles){snprintf(message,sizeof message,
        "Browser console profile: rect draw %u/%lu ms, rect present %lu ms (no author page paint)",
        consoles,(unsigned long)console_ms,(unsigned long)console_update);host_console(NULL,0,message);}
    if(partial||patches){snprintf(message,sizeof message,
        "Browser damage profile: partial draw %u/%lu ms, partial present %lu ms, video patch %u/%lu ms (banded compose into the view and present)",
        partial,(unsigned long)partial_ms,(unsigned long)partial_update,patches,(unsigned long)video_ms);host_console(NULL,0,message);}
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
        if (!strcmp(argv[1], "--debug-js")) { debug_js = net_log = log_console = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--net-log")) { net_log = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--log-console")) { log_console = true; argc--; argv++; }
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
            else {source[length]=0;free(console_startup);console_startup=source;console_startup_length=length;console_due=n_clock_uptime_ms()+3000;}
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
    /* The window is the UI engine's: the toolbar, the address and find fields and the status
       line are its widgets; the page, its scrollbar and DevTools are one surface between them. */
    ui = ui_session_new();
    if (!ui) return 1;
    if (ui_window(ui, WINDOW, MIN(1100, sw - 40), MIN(800, sh - 70),
                  WIN_RESIZABLE | (start_maximized ? WIN_START_MAXIMIZED : 0), "Web", NULL, NULL) < 0 ||
        !(w = ui_session_window(ui, WINDOW))) { ui_session_free(ui); return 1; }
    network = webnet_create();
    webnet_debug(network, debug_js);
    if (!network) { ui_session_free(ui); return 1; }
    storage = webstorage_create(); /* lazily touches local disk only on storage access */
    show_chrome();
    ui_session_pump(ui); /* the surface gets its size */
    take_engine_events();
    redraw();
    if (argc > 1) go_addr(argv[1]); /* a URL, a path (as Files opens one) or words to search */
    else navigate(HOME, NULL, NAV_PUSH);
    while (!quit) {
        uint64_t now = n_clock_uptime_ms();
        webnet_pump(network, now);
        if (debug_js) loop_profile.net_ms += n_clock_uptime_ms() - now;
        apply_pending_history();
        apply_pending_navigation();
        finish_navigation();
        /* Give input already collected by a long task its turn before starting
           the next timer/resource task, rather than repeatedly starving it. */
        if(evq_n){
            /* A long C mutation can leave every collected wheel uncommitted.
               Publish its completed layout once, then batch passive defaults;
               do not rasterize/replay one default for each queued packet. */
            flush_dom_layout();native_scroll_queue();handle_deferred_input();continue;
        }
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
        report_loop_profile(n_clock_uptime_ms());
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
        now = n_clock_uptime_ms();
        struct gui_event e;
        if (evq_n) {
            flush_dom_layout();native_scroll_queue();handle_deferred_input();
            continue;
        }
        int timeout = webnet_timeout(network, now);
        if (webstorage_pending(storage)) {
            struct web_storage_result result;
            if (webstorage_flush(storage, false, &result) != WEB_STORAGE_OK)
                console_add("error", "Local storage flush failed; retrying later.");
            if (webstorage_pending(storage) && (timeout < 0 || timeout > 100)) timeout = 100;
        }
        if (doc && w && !frame_asked && web_rendering_wanted(doc))
            frame_asked = ui_window_frame(ui, WINDOW) == 0;
        int64_t deadline = web_deadline(doc); /* retired media children also wake a document-less window */
        if (deadline >= 0) {
            int due = deadline <= (int64_t)now ? 0 : (int)MIN(deadline - (int64_t)now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        if (navigation_ready || pending_navigation || scroll_event_pending || (history_queue_count && !loading)) timeout = 0;
        if(console_startup_pending()&&doc&&!loading){int due=now>=console_due?0:(int)MIN(console_due-now,0x7fffffff);
            if(timeout<0||due<timeout)timeout=due;}
        if (refresh_at) {
            uint64_t now = n_clock_uptime_ms();
            if (now >= refresh_at) {
                refresh_at = 0;
                navigate(refresh_url, NULL, NAV_PUSH);
                continue;
            }
            int due = (int)MIN(refresh_at - now, 0x7fffffff);
            if (timeout < 0 || due < timeout) timeout = due;
        }
        uint64_t wait_start = debug_js ? n_clock_uptime_ms() : 0;
        int r = browser_event(&e, timeout);
        if (debug_js) loop_profile.wait_ms += n_clock_uptime_ms() - wait_start;
        if (r < 0) break;
        if (r > 0) {
            handle(&e);
            continue;
        }
    }
    stop_navigation();
    cancel_document_requests();
    if (doc) web_free(doc);
    web_media_background(n_clock_uptime_ms()); /* cancel already killed children; never block the closing GUI */
    webnet_free(network);
    webstorage_free(storage);
    parallel_shutdown();
    free(pending_post); free(pending_content_type);
    free(current_text.p); free(navigation_text.p); free(pending_text.p); free(address_text.p); free(refresh_text.p);
    free(focus_value); free(find_text.p);
    for (int i = 0; i < nhist; i++) hist_discard(i);
    free(hist); free(console_lines); free(sel_labels); free(evq); free(console_input); free(console_startup);
    while (history_queue) { struct browser_history_task *next=history_queue->next; free(history_queue); history_queue=next; }
    ui_session_free(ui); free(chrome_sent); free(inbox);
    return 0;
}
