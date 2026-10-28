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
   the window waits for the one asked of it (ui_window_frame, answered by UI_EVENT_FRAME).
   picture_pending from a picture of the page to the frame that shows it: the next picture of the
   document waits for that frame, so that the page changes at most once a frame however many tasks
   change it in between. */
static bool frame_asked, picture_pending;
static web_node *pip_node;
static window_t *pip_window;
static uint64_t pip_revision;
struct url_text { char *p; size_t capacity; };
#define cur_url (tab->current_text.p ? tab->current_text.p : "")
#define navigation_url (tab->navigation_text.p ? tab->navigation_text.p : "")
#define pending_url (tab->pending_text.p ? tab->pending_text.p : "")
#define addr (tab->address_text.p ? tab->address_text.p : "")
#define refresh_url (tab->refresh_text.p ? tab->refresh_text.p : "")
static bool quit;
static bool debug_js;
/* --net-log: requests in the console; --log-console: the console in the system log.
   --debug-js turns both on. --net-body <text>: --net-log plus the bodies of
   requests whose URL contains text. */
static bool net_log, log_console;
static const char *net_body;
/* Debug-only inclusive phase totals, with no page data or per-frame logging. */
static struct {
    uint64_t since, tick_ms, tick_max_ms, layout_ms, draw_ms, update_ms, net_ms, wait_ms;
    uint64_t partial_ms, partial_update_ms, video_present_ms, console_ms, console_update_ms, partial_pixels;
    unsigned ticks, layouts, draws;
    unsigned partial_draws, video_patches, console_draws;
    unsigned partial_empty, partial_whole; /* damage off the page, or not a known rectangle */
    unsigned waits[7], engine_waits, woken; /* which source set each wait's timeout; waits an event ended */
} loop_profile;
enum { WAIT_NONE, WAIT_NET, WAIT_STORAGE, WAIT_DOCUMENT, WAIT_NOW, WAIT_REFRESH, WAIT_POINTER };
/* Browser pages need more time than small custom web_live hosts. The native
   command line can select a finite limit, never an unlimited watchdog. */
static uint32_t js_task_budget_ms = WEB_JS_TASK_DEFAULT_MS;
static webnet *network;
static webstorage *storage; /* browser-window lifetime, not document lifetime */
/* Every document of every tab has a generation of its own: a late answer names the one it is for. */
static uint64_t next_generation = 1;
/* HTML document state: how the navigation that fetched a document asked for
   it. Its history entry keeps it, so reload and traversal fetch the same way.
   initiator: the source document's URL ("" for an opaque origin), NULL when
   the browser itself asked (address bar, home, its buttons). referrer: the
   referrer source, NULL for no-referrer. */
struct document_state { char *initiator, *referrer; int referrer_policy; };
/* One navigation of a tab, as the browser or a page asks for it. */
struct navigation_request {
    const char *url;
    const void *body; size_t body_length; const char *content_type; /* a POST */
    int mode; /* NAV_* */
    const char *initiator, *referrer; int referrer_policy; /* as in document_state */
    bool user_activation, browser_ui;
};
/* The same request, owned, while a script or a native wait delays it; its URL
   is pending_text. */
struct pending_request {
    void *body; size_t body_length; char *content_type, *initiator, *referrer;
    int mode, referrer_policy; bool user_activation, browser_ui;
};

/* A request of a document. The window keeps them all: a keepalive request outlives its
   document and even its tab (owner NULL, detached). */
struct transfer {
    struct transfer *next;
    struct tab *owner;
    uint64_t network_id, resource_id, generation;
    uint64_t devtools; /* Network panel entry, 0 when not recorded */
    int kind;
    bool keepalive, detached;
};
static struct transfer *transfers;
_Static_assert(WEB_RESPONSE_HEADERS_MAX >= WEBNET_RESPONSE_HEADERS_MAX, "Browser must retain complete native response headers");

#define CONSOLE_LINE 1024
/* The page's console, and the engine's own measurement (WEB_CONSOLE_TRACE:
   profiles, requests, media timing) in a panel of its own, so that a trace
   every two seconds never buries what the page itself reported. */
struct console_log { char (*lines)[CONSOLE_LINE]; int n, capacity, skip; };
/* Traces arrive for as long as a page plays: the panel keeps the newer half of a long session. */
#define CONSOLE_TRACE_MAX 8192
static unsigned long console_sequence;
static char *console_startup;
static size_t console_startup_length;
static uint64_t console_due, console_startup_generation;
static bool console_each_document;

struct browser_history_entry {
    char *url;
    int x, y;
    uint64_t document, entry;
    void *state;
    size_t state_len;
    bool manual;
    struct document_state source;
};
static uint64_t history_serial;
struct browser_history_task { struct browser_history_task *next; int delta; uint64_t generation; };

/* Where keys go. The engine keeps the fields' own focus and acts on a field's "focus" only
   when it changes, so each field keeps its own value (focus_asked) and only a move the
   browser makes changes them: focus_shown is where the view last put the keyboard, and
   focus_renew asks again for the field that has it (select all of it). What the engine does
   with the keyboard comes back in order with the other input (CMD_FOCUS) and moves
   focus_shown with it, so the views never ask for what the engine did on its own.
   Each tab keeps where its keys go (its focus); the fields are the window's. */
enum { F_PAGE, F_ADDR, F_FIND, F_CONSOLE, F_COUNT };
static int focus_shown = F_PAGE, focus_serial, focus_asked[F_COUNT];
static bool focus_renew;
static int field_focus = F_PAGE; /* the field the engine has given the keyboard to, as it said so */

#define SEL_H 20
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

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

/* A tab: one top-level traversable, with its document, its session history, its
   navigation, its find bar and its DevTools. The window shows one of them (front); the
   main loop and the engine's callbacks serve one at a time (tab), a callback the one its
   document belongs to (the host's opaque). */
struct tab {
    uint32_t id;     /* the strip's name for it, never given to another tab of this window */
    web_doc *doc;
    int scroll_x, scroll_y, doc_w, doc_h;
    struct url_text current_text, navigation_text, pending_text, address_text, refresh_text;
    char status[256], hover[512];
    char title[256]; /* as the strip and the window's title show it */
    char *name;      /* a page's target name that opened it, found again by that name */
    /* New tabs its document asked for without the user's activation and did not get: how many,
       and the last one's URL (NULL for a form's POST), which the user may still open. */
    unsigned popups;
    char *popup_url;
    uint64_t storage_session; /* its sessionStorage (webstorage_session_open), 0 until a page asks */
    bool loading;
    /* What its document last saw of the window: visible only while it is the front tab. */
    bool window_state_known;
    unsigned window_previous_state;
    uint64_t generation, navigation_generation, navigation_id;
    int navigation_mode;
    struct web_navigation_timing navigation_timing;
    bool native_wait, pending_navigation;
    struct pending_request pending;
    struct document_state navigation_state; /* the navigation in flight */
    bool navigation_ready, sync_stopped;
    struct web_response navigation_response;
    struct console_log console_page, console_trace;
    bool console_open, console_dirty;
    char *console_input;
    size_t console_input_capacity;
    struct browser_history_entry *hist;
    int nhist, hist_capacity, hpos;
    struct browser_history_task *history_queue, *history_queue_tail;
    unsigned history_queue_count;
    int focus;
    bool find_open, find_error;
    struct url_text find_text;
    int find_y;
    web_node *sel_node;
    bool sel_datalist, sel_autocomplete;
    web_node *pressed_node, *hover_node;
    char *focus_value;
    const char **sel_labels;
    int sel_capacity;
    int sel_n, sel_cur, sel_top, sel_x, sel_y, sel_w;
    bool need_layout, need_paint;
    uint64_t refresh_at;
    bool refresh_seen, initial_scroll_pending, initial_scroll_anchor;
    int initial_scroll_x, initial_scroll_y;
    struct deferred_input *evq;
    int evq_first, evq_n, evq_capacity, evq_scroll_at;
    bool evq_failure_logged;
    bool evq_hover_tail_valid;
    bool scroll_event_pending;
    bool native_wait_snapshot;
    bool native_scroll_active;
    struct devtools *dt;
    struct settings *settings; /* the browser's settings page (about:settings) instead of a document */
};
static struct tab *tab;   /* the tab being served */
static struct tab *front; /* the tab the window shows */
static struct tab **tabs; /* in the strip's order */
static int ntabs, tabs_capacity;
static uint32_t tab_serial; /* the last tab's id */
static struct tab *tab_by_id(uint32_t id) {
    for (int i = 0; i < ntabs; i++) if (tabs[i]->id == id) return tabs[i];
    return NULL;
}
/* A tab to bring to the front, asked for while a script runs: the main loop shows it between
   tasks. */
static struct tab *front_wanted;
/* An engine's callback serves the tab its document belongs to (the host's opaque, or the
   one being served when it has none), and the one it interrupted again however it returns. */
static void tab_leave(struct tab **served) { tab = *served; }
#define SERVE(opaque) struct tab *served __attribute__((cleanup(tab_leave))) = tab; \
    if (opaque) tab = (struct tab *)(opaque)
#define find_buf (tab->find_text.p ? tab->find_text.p : "")
struct navigation_request;
static struct tab *tab_new(struct tab *after);
static struct tab *tab_open(const struct navigation_request *r, struct tab *after, bool foreground);
static void tab_select(struct tab *t);
static void tab_close(struct tab *t);
static void tab_move(struct tab *t, int to);

/* An explicit native debug file is not a page-injected script. Preserve the
   old once-per-window default, and never re-run it on same-document history. */
static bool console_startup_pending(void) {
    return console_startup && (!console_each_document || console_startup_generation != tab->generation);
}

static int console_h(void);
static int console_visible(void) { return MAX(1, (console_h() - 50) / 16); } /* below the DevTools tab bar (24) and the heading (26) */
static void console_scroll(struct console_log *log, int delta) {
    int64_t desired = (int64_t)log->skip + delta;
    log->skip = (int)MAX(0, MIN(desired, MAX(log->n - console_visible(), 0)));
    tab->console_dirty = true;
}
static bool console_copy_log(const struct console_log *log) {
    if ((size_t)log->n > SIZE_MAX / (CONSOLE_LINE + 1)) return false;
    size_t capacity = (size_t)log->n * (CONSOLE_LINE + 1) + 1, used = 0;
    char *text = malloc(capacity);
    if (!text) return false;
    for (int i = 0; i < log->n; i++) {
        size_t n = strlen(log->lines[i]);
        memcpy(text + used, log->lines[i], n); used += n;
        text[used++] = '\n';
    }
    int result=clipboard_set(text, used); free(text);
    return result>=0;
}

#include "browser_events.h"
#include "browser_automation.h"

static int page_w(void) { return MAX(view.w - SCRW, 50); }
static int devtools_height; /* DevTools panel, dragged by its top edge; 0: default share */
static int console_h(void) {
    if (!tab->console_open || browser_fullscreen) return 0;
    int room = view.h, h = devtools_height ? devtools_height : room * 2 / 5;
    return MAX(MIN(h, room - 60), MIN(120, room / 2));
}
static int page_h(void) { return MAX(view.h - console_h(), 20); }
static int max_scroll(void) { return MAX(tab->doc_h - page_h(), 0); }
static int max_scroll_x(void) { return MAX(tab->doc_w - page_w(), 0); }
static int document_x(int client_x) { return client_x + tab->scroll_x; }
static void clamp_scroll(void) {
    int x = MAX(0, MIN(tab->scroll_x, max_scroll_x()));
    int y = MAX(0, MIN(tab->scroll_y, max_scroll()));
    if (x != tab->scroll_x || y != tab->scroll_y) tab->scroll_event_pending = true;
    tab->scroll_x = x; tab->scroll_y = y;
    web_viewport_position(tab->doc,tab->scroll_x,tab->scroll_y);
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
    free(tab->current_text.p); free(tab->address_text.p);
    tab->current_text = (struct url_text){current, strlen(current) + 1};
    tab->address_text = (struct url_text){address, strlen(address) + 1};
}

/* ---------------------------------------------------------------- drawing */
static void show_chrome(void);
#include "browser_devtools.h"
#include "browser_settings.h"

/* The widgets around the surface. Their events are browser commands that wait their turn
   with the rest of the input (EV_COMMAND), except a field's text, which only the field uses. */
enum { CMD_BACK = 1, CMD_FORWARD, CMD_RELOAD, CMD_HOME, CMD_SETTINGS, CMD_GO, CMD_FIND, CMD_FIND_NEXT,
       CMD_EVAL, CMD_FOCUS, CMD_NEW_TAB, CMD_CLOSE_TAB, CMD_SELECT_TAB, CMD_MOVE_TAB, CMD_NEW_WINDOW,
       CMD_DEVTOOLS, CMD_QUIT, CMD_SETTINGS_DO, CMD_FIND_OPEN, CMD_RELOAD_TAB, CMD_CLOSE_OTHER_TABS,
       CMD_POPUP_OPEN, CMD_POPUP_DISMISS, CMD_AUTOMATION };
#define EV_COMMAND 0x100u /* key = CMD_*; CMD_FOCUS: x = F_*, y = got (1) or lost (0);
                             CMD_AUTOMATION: x = a batch's marker, or y = 1: the page takes the keys;
                             CMD_SETTINGS_DO: x, y = the settings action, pressed = a toggle's state;
                             CMD_CLOSE_TAB, CMD_SELECT_TAB, CMD_RELOAD_TAB, CMD_CLOSE_OTHER_TABS: x = the
                             tab's id; CMD_MOVE_TAB: x = its id, y = the place it goes to in the strip */
/* A field of the settings page has the keyboard: the keys it does not take are not the page's. */
static bool settings_typing;
static bool menu_open; /* the ≡ button's menu is shown, until the engine dismisses it */
/* The right button's menu of a tab: the tab's id (0: none) and where in it the button went down. */
static uint32_t tab_menu_id;
static double tab_menu_x, tab_menu_y;
static uint32_t views; /* views sent: an event names the one it refers to */
static struct chrome {
    uint32_t view, back, forward, reload, home, menu, address, find, console, page;
    uint32_t strip, menu_list, item_new_tab, item_new_window, item_find, item_devtools, item_settings, item_quit;
    uint32_t tab_menu, tab_new, tab_reload, tab_close, tab_close_others;
    uint32_t popup_open, popup_dismiss;
    uint32_t *tab_ids, *tab_nodes; /* the strip's tabs in this view's order and their nodes: an event names
                                      one by its node and its place */
    int tab_n;
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

static uint32_t menu_item(struct ui_msg *m, const char *text, const char *icon, const char *shortcut) {
    uint32_t place = ui_node(m, "item");
    ui_str(m, "text", text);
    if (icon) ui_str(m, "icon", icon);
    if (shortcut) ui_str(m, "shortcut", shortcut);
    ui_end(m);
    return place;
}

/* The strip of the window's tabs, in their order, with the front one chosen. */
static void tab_strip(struct ui_msg *m, struct chrome *c) {
    const struct ui_theme *t = &ui_theme;
    c->tab_ids = malloc(2 * (size_t)ntabs * sizeof *c->tab_ids);
    c->tab_nodes = c->tab_ids ? c->tab_ids + ntabs : NULL;
    c->strip = ui_node(m, "tabs"); ui_str(m, "key", "tabs"); ui_str(m, "width", "fill");
    ui_num(m, "background", t->background); ui_num(m, "selectedBackground", t->sidebar);
    for (int i = 0; i < ntabs; i++) {
        const struct tab *s = tabs[i];
        char key[24];
        snprintf(key, sizeof key, "tab%u", (unsigned)s->id);
        uint32_t node = ui_node(m, "tab"); ui_str(m, "key", key); ui_num(m, "buttons", 2);
        ui_str(m, "text", s->title[0] ? s->title : "新しいタブ");
        if (s->settings) ui_str(m, "icon", "settings");
        if (s->loading) ui_num(m, "loading", 1);
        if (s == front) ui_num(m, "selected", 1);
        if (s->id == tab_menu_id) {
            c->tab_menu = ui_node(m, "menu"); ui_str(m, "key", "tab-menu"); ui_nums(m, "at", (double[]){tab_menu_x, tab_menu_y}, 2);
                c->tab_new = menu_item(m, "新しいタブ", "plus", "Ctrl+T");
                ui_node(m, "divider"); ui_end(m);
                c->tab_reload = menu_item(m, "再読み込み", "refresh", NULL);
                ui_node(m, "divider"); ui_end(m);
                c->tab_close = menu_item(m, "タブを閉じる", "close", "Ctrl+W");
                c->tab_close_others = menu_item(m, "ほかのタブを閉じる", NULL, NULL);
            ui_end(m);
        }
        ui_end(m);
        if (c->tab_ids) { c->tab_ids[i] = s->id; c->tab_nodes[i] = node; }
    }
    ui_end(m);
    c->tab_n = c->tab_ids ? ntabs : 0;
}

/* The ≡ button and, while it is open, its menu: what the browser does that the toolbar does
   not show. */
static void main_menu(struct ui_msg *m, struct chrome *c) {
    c->menu = ui_node(m, "button");
    ui_str(m, "icon", "menu"); ui_str(m, "variant", "ghost"); ui_num(m, "compact", 1);
    if (menu_open) {
        c->menu_list = ui_node(m, "menu"); ui_str(m, "key", "main-menu"); ui_str(m, "edge", "end");
            c->item_new_tab = menu_item(m, "新しいタブ", "plus", "Ctrl+T");
            c->item_new_window = menu_item(m, "新しいウインドウ", NULL, "Ctrl+N");
            ui_node(m, "divider"); ui_end(m);
            c->item_find = menu_item(m, "ページ内を検索", "search", "Ctrl+F");
            c->item_devtools = menu_item(m, "開発ツール", NULL, "F12");
            ui_node(m, "divider"); ui_end(m);
            c->item_settings = menu_item(m, "設定", "settings", NULL);
            ui_node(m, "divider"); ui_end(m);
            c->item_quit = menu_item(m, "終了", "close", NULL);
        ui_end(m);
    }
    ui_end(m);
}

/* A view's record leaves the ring of recent views. */
static void chrome_release(struct chrome *c) {
    free(c->tab_ids);
    c->tab_ids = c->tab_nodes = NULL; c->tab_n = 0;
}
/* The id of the tab whose node an event of view c came from, at its place there; 0 for none. */
static uint32_t chrome_tab(const struct chrome *c, uint32_t node, double place) {
    if (!(place >= 0 && place < c->tab_n)) return 0;
    int i = (int)place;
    return c->tab_nodes[i] == node ? c->tab_ids[i] : 0;
}

static void show_chrome(void) {
    if (!ui || !front) return;
    SERVE(front); /* the window's widgets are the front tab's */
    if (tab->focus != focus_shown || focus_renew) {
        if (focus_shown != F_PAGE) focus_asked[focus_shown] = -++focus_serial;
        if (tab->focus != F_PAGE) focus_asked[tab->focus] = ++focus_serial;
        focus_shown = tab->focus;
        focus_renew = false;
    }
    const struct ui_theme *t = &ui_theme;
    struct chrome c = {.view = views + 1};
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "column");
    if (!browser_fullscreen) {
        tab_strip(&m, &c);
        ui_node(&m, "row"); ui_str(&m, "key", "toolbar"); ui_nums(&m, "padding", (double[]){5, 8}, 2); ui_num(&m, "gap", 2);
        ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
            c.back = tool(&m, "chevron-left", tab->hpos > 0);
            c.forward = tool(&m, "chevron-right", tab->hpos < tab->nhist - 1);
            c.reload = tool(&m, tab->loading ? "close" : "refresh", tab->loading || cur_url[0]);
            c.home = tool(&m, "home", true);
            ui_node(&m, "spacer"); ui_num(&m, "size", 4); ui_end(&m);
            c.address = ui_node(&m, "field");
            ui_str(&m, "value", field_focus == F_ADDR ? addr : cur_url);
            ui_str(&m, "placeholder", "検索するか、アドレスを入力");
            ui_str(&m, "width", "fill"); ui_num(&m, "height", 30); ui_num(&m, "selectOnFocus", 1);
            field_focus_prop(&m, F_ADDR);
            ui_end(&m);
            ui_node(&m, "spacer"); ui_num(&m, "size", 4); ui_end(&m);
            main_menu(&m, &c);
        ui_end(&m);
        ui_node(&m, "divider"); ui_end(&m);
        if (tab->popups) {
            char said[96];
            snprintf(said, sizeof said, "このページが開こうとした新しいタブを止めました（%u 件）", tab->popups);
            ui_node(&m, "row"); ui_str(&m, "key", "popups"); ui_nums(&m, "padding", (double[]){4, 12}, 2); ui_num(&m, "gap", 8);
            ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
                ui_node(&m, "icon"); ui_str(&m, "icon", "info"); ui_num(&m, "size", 16); ui_num(&m, "colour", t->accent); ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", said); ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1); ui_end(&m);
                if (tab->popup_url) {
                    c.popup_open = ui_node(&m, "button"); ui_str(&m, "text", "最後のものを開く"); ui_num(&m, "compact", 1); ui_end(&m);
                }
                c.popup_dismiss = tool(&m, "close", true);
            ui_end(&m);
            ui_node(&m, "divider"); ui_end(&m);
        }
    }
    if (tab->settings) settings_view(&m, tab->settings, views + 1, w ? (float)w->w : 1024);
    else {
        c.page = ui_node(&m, "surface"); ui_str(&m, "key", "page"); ui_num(&m, "surface", VIEW); ui_num(&m, "pointer", 1);
        ui_str(&m, "width", "fill"); ui_str(&m, "height", "fill"); ui_end(&m);
    }
    if (!browser_fullscreen && !tab->settings && tab->console_open && tab->dt->panel == DT_CONSOLE) {
        ui_node(&m, "row"); ui_str(&m, "key", "console"); ui_nums(&m, "padding", (double[]){3, 8}, 2); ui_num(&m, "gap", 6);
        ui_str(&m, "align", "center"); ui_num(&m, "background", DT_BG);
            ui_node(&m, "text"); ui_str(&m, "text", ">"); ui_num(&m, "mono", 1); ui_num(&m, "colour", t->accent);
            ui_end(&m);
            c.console = ui_node(&m, "field");
            ui_str(&m, "value", tab->console_input); ui_str(&m, "placeholder", "JavaScript");
            ui_num(&m, "mono", 1); ui_str(&m, "width", "fill"); ui_num(&m, "height", 28);
            field_focus_prop(&m, F_CONSOLE);
            ui_end(&m);
        ui_end(&m);
    }
    if (!browser_fullscreen) {
        ui_node(&m, "divider"); ui_end(&m);
        ui_node(&m, "row"); ui_str(&m, "key", "status"); ui_nums(&m, "padding", (double[]){0, 10}, 2); ui_num(&m, "height", 30);
        ui_num(&m, "gap", 8); ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
        if (tab->find_open) {
            ui_node(&m, "icon"); ui_str(&m, "icon", "search"); ui_num(&m, "size", 16); ui_num(&m, "colour", t->textDim);
            ui_end(&m);
            c.find = ui_node(&m, "field");
            ui_str(&m, "value", find_buf); ui_str(&m, "placeholder", "ページ内を検索");
            ui_num(&m, "length", 240); ui_num(&m, "height", 24);
            field_focus_prop(&m, F_FIND);
            ui_end(&m);
            bool bad = tab->find_error || (tab->find_y < 0 && find_buf[0]);
            ui_node(&m, "text");
            ui_str(&m, "text", tab->find_error ? "記憶を確保できませんでした" : !find_buf[0] ? "Enter：次へ　Esc：閉じる"
                               : tab->find_y < 0 ? "見つかりません" : "Enter：次の一致へ");
            ui_str(&m, "style", "caption"); ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1);
            if (bad) ui_num(&m, "colour", t->danger);
            ui_end(&m);
        } else {
            ui_node(&m, "text"); ui_str(&m, "text", tab->hover[0] ? tab->hover : tab->status); ui_str(&m, "style", "caption");
            ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1);
            if (tab->hover[0]) ui_num(&m, "colour", t->accentHover);
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
        chrome_release(&c);
        settings_commit(0); /* the view that went last stands */
        return;
    }
    uint8_t *copy = malloc(m.b.n ? m.b.n : 1);
    if (copy) memcpy(copy, m.b.p, m.b.n);
    size_t n = m.b.n;
    if (ui_send(ui, &m) < 0) { free(copy); chrome_release(&c); settings_commit(0); return; }
    free(chrome_sent);
    chrome_sent = copy;
    chrome_sent_n = copy ? n : 0;
    chrome_release(&chrome[++views % 32]);
    chrome[views % 32] = c;
    settings_commit(views);
}

static void draw_page(void) {
    canvas_t *c = &view;
    int pw = page_w(), ph = page_h();
    if (tab->doc) web_paint(tab->doc, c, 0, 0, pw, ph, tab->scroll_x, tab->scroll_y);
    else gfx_fill(c, 0, 0, pw, ph, RGB(255, 255, 255));
    devtools_draw_overlay(c);
    /* scrollbar */
    int x = view.w - SCRW;
    gfx_fill(c, x, 0, SCRW, ph, ui_theme.sidebar);
    if (tab->doc_h > ph) {
        int th = MAX(24, (int)((long)ph * ph / tab->doc_h));
        int ty = (int)((long)tab->scroll_y * (ph - th) / max_scroll());
        gfx_fill_round(c, x + 2, ty + 1, SCRW - 4, th - 2, 3, drag_sb ? ui_theme.accent : ui_theme.textFaint);
    }
    /* <select> popup */
    if (tab->sel_node) {
        int vis = MIN(tab->sel_n - tab->sel_top, (view.h - tab->sel_y) / SEL_H);
        gfx_fill(c, tab->sel_x - 1, tab->sel_y - 1, tab->sel_w + 2, vis * SEL_H + 2, RGB(90, 90, 120));
        for (int i = 0; i < vis; i++) {
            int k = tab->sel_top + i, y = tab->sel_y + i * SEL_H;
            bool on = k == tab->sel_cur;
            gfx_fill(c, tab->sel_x, y, tab->sel_w, SEL_H, on ? RGB(60, 110, 220) : RGB(255, 255, 255));
            canvas_t cl = *c;
            gfx_clip(&cl, tab->sel_x, y, tab->sel_w - 4, SEL_H);
            gfx_text(&cl, tab->sel_x + 6, y + 2, tab->sel_labels[k], on ? RGB(255, 255, 255) : RGB(20, 20, 30), TRANSPARENT, FONT_SMALL);
        }
    }
}

/* The DevTools panel under the page, inside the surface. The console's input
   line is an engine field below the surface (show_chrome). */
static void draw_console(void) {
    if (browser_fullscreen) return;
    if (tab->console_open) {
        canvas_t *c = &view;
        int top = page_h(), h = console_h();
        devtools_draw(c, top, h);
        tab->console_dirty = false;
        if (tab->dt->panel != DT_CONSOLE && tab->dt->panel != DT_TRACE) return;
        struct console_log *log = tab->dt->panel == DT_TRACE ? &tab->console_trace : &tab->console_page;
        top += DT_TABH; h -= DT_TABH;
        gfx_fill(c, 0, top, view.w, h, DT_BG);
        char heading[192];
        snprintf(heading, sizeof heading, "%s  [PgUp/PgDn: history] [Ctrl+Shift+C: copy log] [Ctrl+K: clear]  %d/%d%s",
                 log == &tab->console_trace ? "Engine trace: profiles, requests, media timing" : "JavaScript console",
                 log->n - log->skip, log->n, log->skip ? " (paused)" : "");
        gfx_text(c, 8, top + 4, heading, ui_theme.textDim, TRANSPARENT, FONT_SMALL);
        canvas_t clip = *c;
        gfx_clip(&clip, 4, top + 24, view.w - 8, h - 24);
        log->skip = MIN(log->skip, MAX(log->n - console_visible(), 0));
        int count = MIN(console_visible(), log->n - log->skip);
        for (int i = 0; i < count; i++) {
            int n = log->n - log->skip - count + i;
            gfx_text(&clip, 8, top + 25 + i * 16, log->lines[n], ui_theme.text, TRANSPARENT, FONT_SMALL);
        }
    }
    tab->console_dirty = false;
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
    struct console_log *log = level && !strcmp(level, "trace") ? &tab->console_trace : &tab->console_page;
    if (log == &tab->console_trace && log->n >= CONSOLE_TRACE_MAX) {
        int drop = log->n / 2;
        memmove(log->lines, log->lines + drop, (size_t)(log->n - drop) * sizeof *log->lines);
        log->n -= drop; log->skip = MAX(0, log->skip - drop);
    }
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
            if (log->n == log->capacity) {
                if (log->n == INT_MAX) return;
                int capacity = log->capacity ? (log->capacity > INT_MAX / 2 ? INT_MAX : log->capacity * 2) : 64;
                if ((size_t)capacity > SIZE_MAX / sizeof *log->lines) return;
                void *grown = realloc(log->lines, (size_t)capacity * sizeof *log->lines);
                if (!grown) return; /* Keep existing log; host serial logging remains available. */
                log->lines = grown; log->capacity = capacity;
            }
            snprintf(log->lines[log->n], CONSOLE_LINE, "[%s] #%lu @%lu.%03lu%s %.*s",
                     level ? level : "log", sequence, (unsigned long)(now / 1000),
                     (unsigned long)(now % 1000), offset ? " +" : "", (int)part, p + offset);
            log->n++;
            if (log->skip && log->skip < INT_MAX) log->skip = MIN(log->skip + 1, MAX(log->n - console_visible(), 0));
            offset += part;
        } while (offset < n);
        tab->console_dirty = true;
        p += n;
        while (*p == '\r' || *p == '\n') p++;
    } while (*p);
}

/* The page is nothing but a fullscreen WebGL canvas (web_direct_open): its frames go to the display
   instead of through the window, which asks for that only meanwhile. true: the display shows the
   newest frame, and the window's surface (hidden) is not painted. */
static bool direct_frame(void) {
    /* The page waits for a script: the display keeps the frame it shows. */
    if (tab->native_wait) return direct && direct_shown;
    bool allowed = w && tab->doc && browser_fullscreen && fullscreen_doc == tab->doc && !tab->sel_node &&
                   !devtools_overlay_visible() && view.px && view.w == w->w && view.h == w->h;
    if (allowed && !direct) allowed = (direct = web_direct_open(tab->doc, view.w, view.h)) != NULL;
    int shown = allowed ? web_direct_present(direct, tab->doc, view.w, view.h, direct_shown) : -2;
    /* Not shown although the display shows the window directly: either it just stopped (EV_DIRECT key 0
       follows) or the context can no longer (-2 now), when nothing else would ask again. */
    if (shown == -1 && direct_shown) shown = web_direct_present(direct, tab->doc, view.w, view.h, false);
    if (shown == -2) {
        allowed = false;
        /* The images are given back once the display no longer shows them. */
        if (direct && !direct_shown) { web_direct_close(direct); direct = NULL; }
    }
    if (allowed != direct_wanted && win_direct(w, allowed) >= 0) direct_wanted = allowed;
    return shown >= 0;
}

/* After a picture of the page is sent: the frame that shows it is asked for. */
static void picture_sent(void) {
    if (!frame_asked) frame_asked = ui_window_frame(ui, WINDOW) == 0;
    picture_pending = frame_asked;
}
/* Only the front tab paints the surface; another one paints when it comes to the front. Its
   title and progress are the strip's. */
static void redraw(void) {
    if (tab != front) { tab->need_paint = true; show_chrome(); return; }
    if (tab->settings) { tab->need_paint = false; show_chrome(); return; }
    if(tab->doc){int x,y,width,height;(void)web_take_paint_damage(tab->doc,&x,&y,&width,&height);}
    if (!view.px) { tab->need_paint = false; show_chrome(); return; }
    if (direct_frame()) { tab->need_paint = false; show_chrome(); return; }
    uint64_t start = debug_js ? n_clock_uptime_ms() : 0;
    draw();
    uint64_t drawn = debug_js ? n_clock_uptime_ms() : 0;
    ui_surface_changed(ui, VIEW);
    picture_sent();
    show_chrome();
    if (debug_js) {
        loop_profile.draw_ms += drawn - start;
        loop_profile.update_ms += n_clock_uptime_ms() - drawn;
        loop_profile.draws++;
    }
    tab->need_paint = false;
}
static void redraw_damage(void){
    int x,y,width,height;
    if(tab!=front)return;
    if(direct_frame()){if(tab->doc)(void)web_take_paint_damage(tab->doc,&x,&y,&width,&height);return;}
    if(!tab->doc||!view.px||tab->need_paint||tab->sel_node||devtools_overlay_visible()||!web_take_paint_damage(tab->doc,&x,&y,&width,&height)){
        if(debug_js)loop_profile.partial_whole++;
        redraw();return;
    }
    if(width<=0||height<=0){if(debug_js)loop_profile.partial_empty++;return;}
    uint64_t start=debug_js?n_clock_uptime_ms():0;
    canvas_t original=view;
    gfx_clip(&view,x,y,width,height);
    draw_page();view=original;
    uint64_t drawn=debug_js?n_clock_uptime_ms():0;
    ui_surface_changed_rect(ui,VIEW,x,y,width,height);
    picture_sent();
    if(debug_js){loop_profile.partial_draws++;loop_profile.partial_ms+=drawn-start;loop_profile.partial_pixels+=(uint64_t)width*(uint64_t)height;
        loop_profile.partial_update_ms+=n_clock_uptime_ms()-drawn;}
}

/* Log changes do not invalidate author content. In particular the
   profiling console must not make its own next sample rasterize the page.
   Opening/closing or resizing it still follows the layout/full-paint path. */
static void redraw_console(void) {
    if (tab != front || tab->settings || !view.px || !tab->console_dirty || !tab->console_open || browser_fullscreen) return;
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
    strlcpy(tab->status, s, sizeof tab->status);
    show_chrome(); /* the same view is not sent again */
}

/* ---------------------------------------------------------------- scrolling */
static void scroll_to_xy(int x, int y) {
    int old_x = tab->scroll_x, old_y = tab->scroll_y;
    tab->scroll_x = x; tab->scroll_y = y;
    clamp_scroll();
    if (old_x != tab->scroll_x || old_y != tab->scroll_y) {
        tab->scroll_event_pending = true;
        tab->sel_node = NULL;
        tab->hover[0] = 0;
    }
}
static void scroll_to(int y) { scroll_to_xy(tab->scroll_x,y); }

/* Native presentation changes only: no author callback or forced layout while
   a script/native wait is active. Residual input chains to the viewport. */
static void scroll_input(web_node *target,int dx,int dy,int edge){
    int old_x=tab->scroll_x,old_y=tab->scroll_y,input_dx=dx,input_dy=dy;
    bool handled=tab->doc&&web_scroll_input(tab->doc,target,&dx,&dy,edge);
    if(edge){if(!handled)scroll_to(edge<0?0:max_scroll());}
    else if(dx||dy){
        int64_t x=(int64_t)tab->scroll_x+dx,y=(int64_t)tab->scroll_y+dy;
        scroll_to_xy((int)MAX((int64_t)0,MIN(x,(int64_t)max_scroll_x())),
                     (int)MAX((int64_t)0,MIN(y,(int64_t)max_scroll())));
    }
    if(debug_js)fprintf(stderr,"[web-input:scroll] input=%d,%d edge=%d handled=%d residual=%d,%d viewport=%d,%d->%d,%d max=%d,%d layout=%d\n",
        input_dx,input_dy,edge,handled,dx,dy,old_x,old_y,tab->scroll_x,tab->scroll_y,max_scroll_x(),max_scroll(),tab->need_layout);
}
static web_node *key_scroll_target(void){
    web_node *target=tab->doc?web_focused(tab->doc):NULL;
    return target?target:tab->hover_node;
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
        if (!tab->doc || tab->sel_node || tab->need_layout || e->x<0 || e->x>=page_w() ||
            e->y<0 || e->y>=page_h() || !web_native_input_ready(tab->doc,tab->native_wait&&tab->native_wait_snapshot)) return false;
        web_node *target=web_node_at(tab->doc,document_x(e->x),e->y+tab->scroll_y);
        /* Hit-index construction can run a cancellation checkpoint. Chrome
           may have resized the viewport or queued navigation in that call. */
        if(quit || tab->pending_navigation || tab->need_layout)return false;
        if(!web_native_wheel_target(tab->doc,target,target_out,tab->native_wait&&tab->native_wait_snapshot))return false;
        tab->doc_w=web_doc_width(tab->doc);tab->doc_h=web_doc_height(tab->doc);
        if (e->mods & NMOD_SHIFT) scroll_input(target,e->wheel*LINE,0,0);
        else scroll_input(target,0,e->wheel*LINE,0);
        return true;
    }
    /* Key defaults remain behind cancelable keydown delivery. Unlike a wheel
       proven passive, a queued PageDown cannot silently bypass preventDefault. */
    return false;
}

static bool defer_native_event(const struct gui_event *e) {
    if(tab->evq_n && browser_coalesce_hover(&tab->evq[tab->evq_first+tab->evq_n-1].event,1,e,tab->evq_hover_tail_valid))return true;
    if(tab->evq_first+tab->evq_n==tab->evq_capacity) {
        if(tab->evq_first) {
            tab->evq_scroll_at=MAX(0,tab->evq_scroll_at-tab->evq_first);
            memmove(tab->evq,tab->evq+tab->evq_first,(size_t)tab->evq_n*sizeof *tab->evq);
            tab->evq_first=0;
        }else {
            int capacity=tab->evq_capacity?(tab->evq_capacity>INT_MAX/2?INT_MAX:tab->evq_capacity*2):64;
            void *grown=tab->evq_n<INT_MAX && (size_t)capacity<=SIZE_MAX/sizeof *tab->evq?
                realloc(tab->evq,(size_t)capacity*sizeof *tab->evq):NULL;
            if(!grown) {
                if(!tab->evq_failure_logged){console_add("error","Deferred native event storage allocation failed");tab->evq_failure_logged=true;}
                return false;
            }
            tab->evq=grown;tab->evq_capacity=capacity;
        }
    }
    struct deferred_input *entry=&tab->evq[tab->evq_first+tab->evq_n++];
    *entry=(struct deferred_input){.event=*e,.generation=tab->generation};
    (void)web_input_capture(tab->doc,NULL,&entry->document);
    tab->evq_hover_tail_valid=browser_hover_tail(e);
    return true;
}

/* This is also called at a pure-JS checkpoint after a C operation unwinds.
   Never re-layout a dirty document here. Blocking wheels and earlier keys/
   buttons remain FIFO-owned by the ordinary cancelable event dispatcher. */
static void native_scroll_queue(void) {
    if(tab->native_scroll_active || !tab->doc || !tab->evq_n || !web_native_input_ready(tab->doc,tab->native_wait&&tab->native_wait_snapshot))return;
    tab->native_scroll_active=true;
    bool painted=false;
    tab->evq_scroll_at=MAX(tab->evq_scroll_at,tab->evq_first);
    while(tab->evq_scroll_at<tab->evq_first+tab->evq_n) {
        int offset=tab->evq_scroll_at-tab->evq_first;
        struct deferred_input entry=tab->evq[tab->evq_scroll_at];
        if(entry.event.type==EV_MOUSE_MOVE && !entry.event.buttons){tab->evq_scroll_at++;continue;}
        if(entry.event.type!=EV_WHEEL)break;
        if(entry.scrolled){tab->evq_scroll_at++;continue;}
        if(entry.generation!=tab->generation || !web_input_target_valid(tab->doc,&entry.document))break;
        if(!try_scroll_event(&entry.event,&entry.target))break;
        /* A nested hit-test/paint checkpoint can append input, grow/compact
           the FIFO, or repaint chrome. Keep no queue-storage pointer across
           native calls; its logical offset survives those tail-only changes. */
        int index=tab->evq_first+offset;
        tab->evq[index].target=entry.target;tab->evq[index].scrolled=true;
        painted=true;tab->evq_scroll_at=index+1;
        /* Painting is itself native work; one presentation per drained batch
           avoids a full viewport raster for each wheel packet. */
    }
    if(painted && !quit)redraw();
    tab->native_scroll_active=false;
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
        {".html", "text/html"}, {".htm", "text/html"}, {".xhtml", "application/xhtml+xml"},
        {".xht", "application/xhtml+xml"}, {".xml", "text/xml"}, {".css", "text/css"}, {".png", "image/png"},
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
    n_handle file = n_document_open(path, N_DOCUMENT_READ);
    if (file < 0) {
        snprintf(f->err, sizeof f->err, "cannot open %s", path);
        return false;
    }
    n_result size = n_document_size(file);
    if (size < 0 || (uint64_t)size >= SIZE_MAX) {
        n_handle_close(file);
        strlcpy(f->err, "file length is not representable", sizeof f->err);
        return false;
    }
    size_t n = (size_t)size;
    f->body = malloc(n + 1);
    if (!f->body) { n_handle_close(file); strlcpy(f->err, "out of memory", sizeof f->err); return false; }
    size_t got = 0;
    n_result r;
    while (got < n && (r = n_document_read_at(file, got, f->body + got, n - got)) > 0) got += (size_t)r;
    n_handle_close(file);
    f->status = 200;
    f->body[got] = 0;
    f->len = got;
    strlcpy(f->ctype, guess_type(path), sizeof f->ctype);
    return true;
}

/* ---------------------------------------------------------------- built-in pages */
static const char home_page_head[] =
    "<!doctype html><html lang=ja><head><meta charset=utf-8><title>Nocturne Web</title><style>"
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
    "<h1>Nocturne Web</h1><p class=sub>Nocturne 自身のウェブエンジンで動く HTML・CSS・JavaScript。</p>"
    "<form action=\"https://lite.duckduckgo.com/lite/\"><input type=text name=q placeholder=\"DuckDuckGo で検索\">"
    "<button type=submit>検索</button></form>"
    "<div class=grid>"
    "<a href=\"https://en.wikipedia.org/wiki/Special:Random\">Wikipedia<span>おまかせの記事</span></a>"
    "<a href=\"https://news.ycombinator.com/\">Hacker News<span>news.ycombinator.com</span></a>"
    "<a href=\"https://lite.cnn.com/\">CNN Lite<span>文字だけのニュース</span></a>"
    "<a href=\"https://text.npr.org/\">NPR Text<span>text.npr.org</span></a>"
    "<a href=\"https://www.python.org/\">Python<span>python.org</span></a>"
    "<a href=\"https://lobste.rs/\">Lobsters<span>lobste.rs</span></a>"
    "<a href=\"https://developer.mozilla.org/en-US/docs/Web/CSS\">MDN CSS<span>developer.mozilla.org</span></a>"
    "<a href=\"https://example.com/\">Example<span>example.com</span></a>"
    ;
/* then two links into the user's folder, which the user's folder decides (home_html) */
static const char home_page_tail[] =
    "</div>"
    "<h2>キー</h2><table>"
    "<tr><td><kbd>Ctrl+T</kbd><td>新しいタブ"
    "<tr><td><kbd>Ctrl+W</kbd> <kbd>Ctrl+F4</kbd><td>タブを閉じる"
    "<tr><td><kbd>Ctrl+Tab</kbd> <kbd>Ctrl+Shift+Tab</kbd><td>次のタブ・前のタブ"
    "<tr><td><kbd>Ctrl+1</kbd>〜<kbd>Ctrl+8</kbd> <kbd>Ctrl+9</kbd><td>その番のタブ・最後のタブ"
    "<tr><td><kbd>Ctrl+N</kbd><td>新しいウインドウ"
    "<tr><td><kbd>Ctrl</kbd>＋クリック<td>リンクを後ろのタブで開く"
    "<tr><td><kbd>Ctrl+L</kbd><td>アドレス欄（言葉を打てば検索）"
    "<tr><td><kbd>Ctrl+F</kbd><td>ページ内を検索"
    "<tr><td><kbd>Alt+Left</kbd> <kbd>Backspace</kbd><td>戻る"
    "<tr><td><kbd>Alt+Right</kbd><td>進む"
    "<tr><td><kbd>F5</kbd> <kbd>Ctrl+R</kbd><td>再読み込み"
    "<tr><td><kbd>F12</kbd><td>開発ツール"
    "<tr><td><kbd>Space</kbd> <kbd>PgDn</kbd> <kbd>PgUp</kbd><td>頁を送る"
    "<tr><td><kbd>Esc</kbd><td>読み込みを止める"
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
        sb_str(&s, "\">JavaScript の見本<span>DOM・事象・タイマー・モジュール</span></a>");
        free(url);
    }
    if ((url = file_url(n_user_folder()))) {
        sb_str(&s, "<a href=\"");
        sb_str(&s, url);
        sb_str(&s, "\">あなたのファイル<span>");
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
static int anchor(const char *url) {
    const char *h = strchr(url, '#');
    if (!h || !tab->doc) return -1;
    /* Native fragment selection owns decoding. Pass the full fragment once:
       neither a fixed scratch truncation nor a second percent-decode may
       select a different element. */
    return web_anchor_y(tab->doc, h + 1);
}

/* A new document's position (its fragment's, or where history left it) is taken by the first
   layout that reaches it, whichever path laid the page out. */
static void initial_scroll(void) {
    if (!tab->initial_scroll_pending) return;
    if (tab->initial_scroll_anchor) {
        int y = anchor(cur_url);
        if (y >= 0) { scroll_to(y); tab->initial_scroll_pending = false; }
    } else if (max_scroll() >= tab->initial_scroll_y && max_scroll_x() >= tab->initial_scroll_x) {
        scroll_to_xy(tab->initial_scroll_x,tab->initial_scroll_y); tab->initial_scroll_pending = false;
    }
}

static uint64_t page_layouts; /* layouts of any tab's page: the pointer looks again after one */
static void relayout(void) {
    if (!tab->doc) return;
    page_layouts++;
    uint64_t start = debug_js ? n_clock_uptime_ms() : 0;
    tab->doc_h = web_layout(tab->doc, page_w(), page_h());
    tab->doc_w = web_doc_width(tab->doc);
    if (debug_js) { loop_profile.layout_ms += n_clock_uptime_ms() - start; loop_profile.layouts++; }
    clamp_scroll();
    tab->need_layout = false;
    initial_scroll();
    devtools_refresh(false);
}

/* The tab's title, as the strip shows it; the front tab's names the window too. */
static void set_title(void) {
    static char shown[256 + 8];
    const char *s = tab->doc ? web_title(tab->doc) : tab->settings ? "設定" : "";
    char t[sizeof tab->title], window_title[sizeof shown];
    snprintf(t, sizeof t, "%s", *s ? s : cur_url);
    if (strcmp(t, tab->title)) { strcpy(tab->title, t); show_chrome(); }
    if (tab != front) return;
    snprintf(window_title, sizeof window_title, "%s - Web", t);
    if (!ui || !strcmp(window_title, shown)) return;
    strcpy(shown, window_title);
    ui_window_title(ui, WINDOW, window_title);
}

/* A finished JS task may have retired stylesheet/SVG arenas. No native hit-test,
   control popup, or repaint may use the old boxes after that point. */
static void flush_dom_layout(void) {
    if (!tab->doc || tab->native_wait || web_script_running(tab->doc)) return;
    if(tab->sel_node && web_node_inert(tab->doc,tab->sel_node))tab->sel_node=NULL;
    /* Animation samples wait for the document's rendering turn, which bounds their repaint. */
    if (!web_restyle_only(tab->doc) && web_dirty(tab->doc)) tab->need_layout = true;
    if (tab->need_layout) {
        relayout();
        set_title();
        tab->need_paint = true;
    }
    web_node *invalid=NULL; const char *message=NULL; size_t length=0;
    if (web_take_validation_report(tab->doc,&invalid,&message,&length)) {
        tab->focus=F_PAGE;
        free(tab->focus_value); tab->focus_value=strdup(web_control_value(invalid));
        char visible[sizeof tab->status]; size_t n=MIN(length,sizeof visible-1);
        for(size_t i=0;i<n;i++) visible[i]=message[i]?message[i]:' ';
        visible[n]=0; tab->hover[0]=0; set_status(visible);
        int x,y,width,height;
        if(web_node_rect(tab->doc,invalid,&x,&y,&width,&height) &&
           (x<tab->scroll_x || x+width>tab->scroll_x+page_w() || y<tab->scroll_y || y+height>tab->scroll_y+page_h()))
            scroll_to_xy(x<tab->scroll_x || x+width>tab->scroll_x+page_w()?MAX(x-16,0):tab->scroll_x,
                         y<tab->scroll_y || y+height>tab->scroll_y+page_h()?MAX(y-16,0):tab->scroll_y);
        tab->need_paint=true;
    }
}

/* Module loading can suspend an executing JS task after a DOM mutation has
   retired its SVG snapshots. Rebuild native boxes before pumping any UI.
   This deliberately bypasses web_script_running(), but never enters JS or
   rescans script/resource work: web_layout and painting are native-only. */
static void prepare_native_snapshot(bool force) {
    if (!tab->doc) return;
    bool dirty = web_dirty(tab->doc);
    if (force || dirty || tab->need_layout) {
        relayout();
        tab->need_paint = true;
    }
}

static void document_state_free(struct document_state *s) {
    free(s->initiator); free(s->referrer);
    *s = (struct document_state){0};
}
static bool document_state_set(struct document_state *to, const char *initiator, const char *referrer, int policy) {
    struct document_state s = {initiator ? strdup(initiator) : NULL, referrer && *referrer ? strdup(referrer) : NULL, policy};
    if ((initiator && !s.initiator) || (referrer && *referrer && !s.referrer)) { document_state_free(&s); return false; }
    document_state_free(to); *to = s;
    return true;
}
static void hist_discard(int index) {
    free(tab->hist[index].url); free(tab->hist[index].state); document_state_free(&tab->hist[index].source);
    memset(&tab->hist[index], 0, sizeof tab->hist[index]);
}
static bool hist_reserve(size_t count) {
    if (count > INT_MAX || count > SIZE_MAX / sizeof *tab->hist) return false;
    if (count <= (size_t)tab->hist_capacity) return true;
    size_t capacity = tab->hist_capacity ? (size_t)tab->hist_capacity : 64;
    while (capacity < count) { if (capacity > INT_MAX / 2) { capacity = count; break; } capacity *= 2; }
    void *grown = realloc(tab->hist, capacity * sizeof *tab->hist);
    if (!grown) return false;
    tab->hist = grown; tab->hist_capacity = (int)capacity;
    return true;
}
/* A new entry of the current document shares its document state. */
static bool hist_append(char *copy) {
    if (!hist_reserve((size_t)(tab->hpos + 1) + 1)) return false;
    struct document_state source = {0};
    if (tab->hpos >= 0 && !document_state_set(&source, tab->hist[tab->hpos].source.initiator, tab->hist[tab->hpos].source.referrer,
                                         tab->hist[tab->hpos].source.referrer_policy)) return false;
    for (int i = tab->hpos + 1; i < tab->nhist; i++) hist_discard(i);
    tab->nhist = tab->hpos + 1;
    memset(&tab->hist[tab->nhist], 0, sizeof tab->hist[tab->nhist]);
    tab->hist[tab->nhist].url = copy;
    tab->hist[tab->nhist].source = source;
    tab->hist[tab->nhist].document = tab->generation;
    tab->hist[tab->nhist].entry = ++history_serial;
    tab->hpos = tab->nhist++;
    return true;
}
static void hist_push(const char *url) {
    char *copy = strdup(url);
    if (!copy) { console_add("error", "History allocation failed"); return; }
    if (!hist_append(copy)) { free(copy); console_add("error", "History allocation failed"); }
}
static void history_scroll_save(void) {
    if (tab->hpos >= 0) { tab->hist[tab->hpos].x = tab->scroll_x; tab->hist[tab->hpos].y = tab->scroll_y; }
}

static bool same_doc(const char *a, const char *b) {
    size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
    return la == lb && !strncmp(a, b, la);
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
    /* A network error has its timing record too (an entry with its start and end). */
    if (in->timing && !http_fetch_timing_copy(&out->timing, in->timing)) {
        strlcpy(out->error, "out of memory copying response timing", sizeof out->error); return;
    }
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
    SERVE(t->owner);
    if (t->owner) dt_net_update(t->devtools, result);
    if (!t->detached && t->owner && tab->doc && gen == tab->generation && t->generation == tab->generation) {
        debug_image_result(t, result);
        if (debug_js && t->kind == WEB_RESOURCE_CSS)
            printf("[web-net:css] status=%d bytes=%lu error=%s url=%s\n", result->status,
                   (unsigned long)result->body_len, result->error ? result->error : "",
                   result->final_url ? result->final_url : "");
        struct web_response *r = malloc(sizeof *r);
        if (r) {
            response_copy(r, result, network_kind(t->kind));
            r->event = result->event; r->uploaded = result->uploaded;
            web_resource_loaded(tab->doc, t->resource_id, r);
            web_response_free(r); free(r);
        } else {
            static const struct web_response failure = {.error="resource response allocation failed"};
            web_resource_loaded(tab->doc, t->resource_id, &failure);
        }
    }
    if (result->event == WEBNET_COMPLETE || result->event == WEBNET_END) remove_transfer(t);
    else if (t->detached) webnet_resume(net, id);
}

static const char *request_refusal;
static const char *host_request_refusal(void *opaque) { (void)opaque; return request_refusal; }
static size_t host_keepalive_inflight(void *opaque, uint64_t group) { (void)opaque; return webnet_keepalive_inflight(network, group); }
static bool host_request(void *opaque, const struct web_request *request) {
    SERVE(opaque);
    if (!tab->doc || quit) { request_refusal = "the tab is closing"; return false; }
    struct transfer *t = calloc(1, sizeof *t);
    if (!t) { request_refusal = "out of memory"; return false; }
    t->owner = tab;
    t->resource_id = request->id;
    t->generation = tab->generation;
    t->kind = request->kind;
    t->keepalive = request->keepalive;
    struct webnet_request r = {
        .kind = network_kind(request->kind), .generation = tab->generation,
        .url = request->url, .origin = request->origin ? request->origin : web_url(tab->doc), .method = request->method,
        .headers = request->headers, .body = request->body, .body_len = request->body_len,
        .credentials = request->credentials, .force_preflight = request->force_preflight,
        .redirect_error = request->redirect_error, .same_origin = request->same_origin,
        .no_cors = request->no_cors, .no_referrer = request->no_referrer,
        .referrer=request->referrer,.referrer_policy=request->referrer_policy,
        .image_upgrade = request->kind == WEB_RESOURCE_IMAGE && request->image_upgrade,
        .cache_mode = request->cache_mode, .keepalive = request->keepalive,
        .fetch_group = request->fetch_group
        , .stream_response = request->stream_response, .top_level_url=web_url(tab->doc)
        , .require_corp = request->require_corp
        , .destination = (enum webnet_destination)request->destination
        , .report_sent = tab->console_open /* DevTools records it: show the fields it sent */
    };
    t->network_id = webnet_submit(network, &r, resource_completed, t);
    if (!t->network_id) { request_refusal = webnet_refusal(network); free(t); return false; }
    t->devtools = dt_net_begin(false, tab->generation, request->id, request->kind, request->method, request->url, request->headers);
    t->next = transfers;
    transfers = t;
    return true;
}

static void host_cancel(void *opaque, uint64_t id) {
    SERVE(opaque);
    for (struct transfer *t = transfers; t; t = t->next) {
        if (t->generation == tab->generation && t->resource_id == id) {
            webnet_cancel(network, t->network_id);
            remove_transfer(t);
            return;
        }
    }
}
static void host_release_request(void *opaque, uint64_t id) {
    SERVE(opaque);
    for (struct transfer *t = transfers; t; t = t->next)
        if (t->generation == tab->generation && t->resource_id == id) {
            if (t->keepalive) { t->detached = true; webnet_resume(network, t->network_id); }
            else { webnet_cancel(network, t->network_id); remove_transfer(t); }
            return;
        }
}
static void host_resume_request(void *opaque, uint64_t id) {
    SERVE(opaque);
    for (struct transfer *t = transfers; t; t = t->next)
        if (t->generation == tab->generation && t->resource_id == id) { webnet_resume(network, t->network_id); return; }
}

/* The tab whose document has this generation now, if any. */
static struct tab *tab_of_generation(uint64_t of) {
    for (int i = 0; i < ntabs; i++) if (tabs[i]->generation == of) return tabs[i];
    return NULL;
}

/* WebSocket: the webnet socket id is the document-visible id. Delivery stops
   with the document generation; webnet_cancel_generation releases the rest. */
static void socket_message(webnet *net, uint64_t id, const struct webnet_socket_message *m, void *opaque) {
    (void)net;
    struct tab *owner = tab_of_generation((uint64_t)(uintptr_t)opaque);
    if (!owner) return;
    SERVE(owner);
    if (!tab->doc) return;
    struct web_socket_event e = { .code = m->code, .data = m->data, .length = m->length, .protocol = m->protocol };
    switch (m->event) {
    case WEBNET_SOCKET_OPENED: e.type = WEB_SOCKET_OPENED; break;
    case WEBNET_SOCKET_RECEIVED: e.type = m->code == WEBNET_SOCKET_BINARY ? WEB_SOCKET_MESSAGE_BINARY : WEB_SOCKET_MESSAGE_TEXT; e.code = 0; break;
    case WEBNET_SOCKET_CLOSED: e.type = WEB_SOCKET_CLOSED; break;
    default: e.type = WEB_SOCKET_FAILED; break;
    }
    web_socket_event(tab->doc, id, &e);
}
static uint64_t host_socket_open(void *opaque, const char *url, const char *protocols, const char *origin) {
    SERVE(opaque);
    if (!tab->doc || quit || !url) return 0;
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
        .kind = WEBNET_WEBSOCKET, .generation = tab->generation, .url = url,
        .origin = origin ? origin : web_url(tab->doc), .method = "GET", .headers = headers,
        .credentials = WEBNET_CREDENTIALS_INCLUDE, .top_level_url = web_url(tab->doc)
    };
    uint64_t id = webnet_socket_open(network, &r, socket_message, (void *)(uintptr_t)tab->generation);
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
    webnet_cancel_generation(network, tab->generation);
    struct transfer **at = &transfers;
    while (*at) {
        struct transfer *t = *at;
        /* UA/process shutdown has no independent network owner. Navigation
           retains native requests but permanently removes old JS delivery. */
        if (quit) { webnet_cancel(network,t->network_id); *at=t->next; free(t); continue; }
        if (t->generation != tab->generation) { at=&t->next; continue; }
        if (t->keepalive) { t->detached=true; webnet_resume(network,t->network_id); at=&t->next; continue; }
        *at=t->next; free(t);
    }
}

static void pending_free(struct pending_request *p) {
    free(p->body); free(p->content_type); free(p->initiator); free(p->referrer);
    *p = (struct pending_request){0};
}
static void queue_navigation(const struct navigation_request *r) {
    if (!r->url || !*r->url) return;
    if (r->body_length > WEBNET_BODY_LIMIT ||
        (r->body && (!r->content_type || strlen(r->content_type) > UINT32_MAX - 16u || strpbrk(r->content_type,"\r\n")))) {
        console_add("error", "Form navigation exceeds the native request bounds"); return;
    }
    struct pending_request p = {.body_length = r->body ? r->body_length : 0, .mode = r->mode,
        .referrer_policy = r->referrer_policy, .user_activation = r->user_activation, .browser_ui = r->browser_ui};
    char *url = strdup(r->url);
    p.body = r->body ? malloc(r->body_length + 1) : NULL;
    p.content_type = r->body ? strdup(r->content_type) : NULL;
    p.initiator = r->initiator ? strdup(r->initiator) : NULL;
    p.referrer = r->referrer ? strdup(r->referrer) : NULL;
    if (!url || (r->body && (!p.body || !p.content_type)) || (r->initiator && !p.initiator) || (r->referrer && !p.referrer)) {
        free(url); pending_free(&p); console_add("error", "Navigation allocation failed"); return;
    }
    if (p.body) { if (r->body_length) memcpy(p.body, r->body, r->body_length); ((char *)p.body)[r->body_length] = 0; }
    free(tab->pending_text.p); tab->pending_text = (struct url_text){url, strlen(url) + 1};
    pending_free(&tab->pending); tab->pending = p;
    tab->pending_navigation = true;
}
/* The browser's own navigation (address bar, home, its buttons, the window's
   first page): no source document, so no referrer unless reload and
   traversal reuse their entry's, and the user's own activation. */
static struct navigation_request browser_navigation(const char *url, int mode) {
    struct navigation_request r = {.url = url, .mode = mode, .user_activation = true, .browser_ui = true};
    if ((mode == NAV_RELOAD || mode == NAV_HISTORY) && tab->hpos >= 0) {
        r.referrer = tab->hist[tab->hpos].source.referrer; r.referrer_policy = tab->hist[tab->hpos].source.referrer_policy;
    }
    return r;
}

static void host_navigate(void *opaque, const struct web_navigation *n) {
    SERVE(opaque);
    if (!n || !n->url) return;
    if (tab->doc && !has_prefix(web_url(tab->doc), "file://") && !has_prefix(web_url(tab->doc), "about:") &&
        has_prefix(n->url, "file://")) {
        console_add("error", "A remote document cannot navigate to a local file.");
        return;
    }
    const char *target = n->target;
    if (!target || !*target || !strcasecmp(target,"_self") || !strcasecmp(target,"_top") || !strcasecmp(target,"_parent")) {
        struct navigation_request r = {.url = n->url, .body = n->body, .body_length = n->body_length,
            .content_type = n->content_type,
            .mode = n->history == WEB_NAVIGATE_REPLACE ? NAV_REPLACE : n->history == WEB_NAVIGATE_RELOAD ? NAV_RELOAD : NAV_PUSH,
            .initiator = n->initiator ? n->initiator : "", .referrer = n->referrer,
            .referrer_policy = n->referrer_policy, .user_activation = n->user_activation};
        /* Reload fetches with its entry's request referrer and policy. */
        if (r.mode == NAV_RELOAD && tab->hpos >= 0) { r.referrer = tab->hist[tab->hpos].source.referrer; r.referrer_policy = tab->hist[tab->hpos].source.referrer_policy; }
        queue_navigation(&r); return;
    }
    /* Another top-level traversable: a tab after this one, at the front. One the page named is
       found again by that name; a new one opens only on the user's activation, as a popup
       would. */
    struct navigation_request r = {.url = n->url, .body = n->body, .body_length = n->body_length,
        .content_type = n->content_type, .mode = NAV_PUSH,
        .initiator = n->initiator ? n->initiator : "", .referrer = n->referrer,
        .referrer_policy = n->referrer_policy, .user_activation = n->user_activation};
    bool blank = !strcasecmp(target, "_blank");
    for (int i = 0; !blank && i < ntabs; i++)
        if (tabs[i]->name && !strcmp(tabs[i]->name, target)) {
            struct tab *source = tab;
            tab = tabs[i]; queue_navigation(&r); tab = source;
            front_wanted = tabs[i];
            return;
        }
    if (!n->user_activation) {
        console_add("warn", "A new tab was not opened: the page asked without the user's activation.");
        if (tab->popups < UINT_MAX) tab->popups++;
        free(tab->popup_url);
        tab->popup_url = n->body ? NULL : strdup(n->url);
        show_chrome();
        return;
    }
    struct tab *t = tab_open(&r, tab, true);
    if (t && !blank && !(t->name = strdup(target))) console_add("error", "Tab name allocation failed");
}

static void host_console(void *opaque, int level, const char *message) {
    SERVE(opaque);
    const char *name = level == WEB_CONSOLE_TRACE ? "trace" : level >= WEB_CONSOLE_ERROR ? "error" : level == WEB_CONSOLE_WARN ? "warn" : "log";
    console_add(name, message);
    if (debug_js) fprintf(stderr, "[web-js:%s] %s\n", name, message);
}

static void host_scroll(void *opaque, int *x, int *y) {
    SERVE(opaque);
    *x = tab->scroll_x;
    *y = tab->scroll_y;
}
static void host_scroll_to(void *opaque, int x, int y) {
    SERVE(opaque);
    if (tab->need_layout) relayout();
    /* Author geometry can flush layout before this host callback, without a
       browser turn setting need_layout. Clamp against that current snapshot. */
    if (tab->doc) { tab->doc_w = web_doc_width(tab->doc); tab->doc_h = web_doc_height(tab->doc); }
    scroll_to_xy(x,y);
    tab->need_paint = true;
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
    SERVE(opaque);
    if(mode==0||mode==4){if(fullscreen_doc!=document)return true;bool restored=w&&win_fullscreen(w,false)>=0;
        if(!restored&&mode==0)return false;
        browser_fullscreen=false;fullscreen_doc=NULL;tab->need_layout=tab->need_paint=true;return restored;}
    if(mode==3){if(pip_doc!=document)return true;if(pip_window)win_close(pip_window);pip_window=NULL;pip_doc=NULL;pip_node=NULL;pip_revision=0;return true;}
    web_doc *top=web_presentation_root(document);
    if(!w||!document||tab!=front||top!=tab->doc||quit||tab->loading||tab->pending_navigation)return false;
    if(mode==1){if(win_fullscreen(w,true)<0)return false;browser_fullscreen=true;fullscreen_doc=document;tab->focus=F_PAGE;tab->need_layout=tab->need_paint=true;return true;}
    if(mode==2){
        if(pip_window)return false;
        window_t *window;if(win_open(420,236,"Nocturne — Picture in Picture",WIN_RESIZABLE|WIN_ALWAYS_ON_TOP,&window)<0)return false;
        pip_window=window;pip_doc=document;pip_node=element;pip_revision=UINT64_MAX;
        web_presentation_resize(document,window->w,window->h);
        web_avmedia_pip_paint(document,element,&window->c);win_update(window);return true;
    }return false;
}
static bool host_video_present(void *opaque, web_doc *document, const struct web_video_patch *patch) {
    SERVE(opaque);
    presentation_service();
    /* Pending DOM/layout work does not replace the committed window raster.
       A snapshot owns that old surface's geometry and overlay pixels, not
       mutable layout boxes. Keep it moving until real paint replaces it;
       document, canvas, viewport, scroll and native-overlay checks below
       reject an obsolete surface without re-entering DOM or layout here. */
    if (!view.px || !patch || tab != front || document != tab->doc || quit || tab->native_wait || tab->pending_navigation ||
        tab->sel_node || devtools_overlay_visible() || !patch->draw || patch->w <= 0 || patch->h <= 0 ||
        patch->canvas.px != view.px || patch->canvas.w != view.w ||
        patch->canvas.h != view.h || patch->canvas.pitch != view.pitch ||
        patch->scroll_x != tab->scroll_x || patch->scroll_y != tab->scroll_y ||
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
    SERVE(opaque);
    return quit || tab->pending_navigation || tab->loading;
}
/* Report the real native pointer at dequeue, not a synthetic DOM callback.
   A long author task may defer hover delivery; diagnostics must not re-enter it. */
static void native_active_event(const struct gui_event *event) {
    if(tab->doc && (event->type==EV_CLOSE || event->type==EV_UNFOCUS ||
       ((event->type==EV_MOUSE_UP || event->type==EV_MOUSE_MOVE) && !(event->buttons&1))))
        web_active_release(tab->doc);
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
                if (!tab->evq_failure_logged) { console_add("error", "Native event storage allocation failed"); tab->evq_failure_logged = true; }
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
    if (n >= tab->console_input_capacity) {
        char *grown = n < SIZE_MAX ? realloc(tab->console_input, n + 1) : NULL;
        if (!grown) { console_add("error", "Console input allocation failed"); return false; }
        tab->console_input = grown; tab->console_input_capacity = n + 1;
    }
    memcpy(tab->console_input, text, n); tab->console_input[n] = 0;
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
        struct settings_action set = front && front->settings ? settings_action_of(e.view, e.node) : (struct settings_action){0};
        if (set.act) {
            /* What is typed is the page's own state; a click waits its turn with the rest of the input. */
            if (e.type == UI_EVENT_TEXT || e.type == UI_EVENT_SUBMIT) { if (settings_text(front->settings, set, text)) echo = true; }
            else if (e.type == UI_EVENT_FOCUS) settings_typing = e.a != 0;
            else if (e.type == UI_EVENT_CLICK || e.type == UI_EVENT_TOGGLE)
                inbox_put((struct gui_event){.type = EV_COMMAND, .key = CMD_SETTINGS_DO, .x = (int)set.act, .y = (int)set.arg,
                                             .pressed = e.type == UI_EVENT_TOGGLE && e.a != 0});
            continue;
        }
        int which = !known ? F_PAGE : e.node == c->address ? F_ADDR : e.node == c->find ? F_FIND
                    : e.node == c->console ? F_CONSOLE : F_PAGE;
        switch (e.type) {
        case UI_EVENT_CLICK:
            if (!known) break;
            if (e.node == c->back) inbox_command(CMD_BACK, 0, 0, 0);
            else if (e.node == c->forward) inbox_command(CMD_FORWARD, 0, 0, 0);
            else if (e.node == c->reload) inbox_command(CMD_RELOAD, 0, 0, 0);
            else if (e.node == c->home) inbox_command(CMD_HOME, 0, 0, 0);
            else if (e.node == c->menu) { menu_open = !menu_open; show_chrome(); }
            else if (e.node == c->item_new_tab) inbox_command(CMD_NEW_TAB, 0, 0, 0);
            else if (e.node == c->item_new_window) inbox_command(CMD_NEW_WINDOW, 0, 0, 0);
            else if (e.node == c->item_find) inbox_command(CMD_FIND_OPEN, 0, 0, 0);
            else if (e.node == c->item_devtools) inbox_command(CMD_DEVTOOLS, 0, 0, 0);
            else if (e.node == c->item_settings) inbox_command(CMD_SETTINGS, 0, 0, 0);
            else if (e.node == c->item_quit) inbox_command(CMD_QUIT, 0, 0, 0);
            else if (e.node == c->tab_new) inbox_command(CMD_NEW_TAB, 0, 0, 0);
            else if (e.node == c->tab_reload) inbox_command(CMD_RELOAD_TAB, (int)tab_menu_id, 0, 0);
            else if (e.node == c->tab_close) inbox_command(CMD_CLOSE_TAB, (int)tab_menu_id, 0, 0);
            else if (e.node == c->tab_close_others) inbox_command(CMD_CLOSE_OTHER_TABS, (int)tab_menu_id, 0, 0);
            else if (e.node == c->popup_open) inbox_command(CMD_POPUP_OPEN, 0, 0, 0);
            else if (e.node == c->popup_dismiss) inbox_command(CMD_POPUP_DISMISS, 0, 0, 0);
            else if (UI_BUTTONS(e.flags) == 2) /* a tab's right button: its menu, where it went down */
                for (int i = 0; i < c->tab_n; i++)
                    if (c->tab_nodes[i] == e.node) { tab_menu_id = c->tab_ids[i]; tab_menu_x = e.a; tab_menu_y = e.b; show_chrome(); }
            break;
        /* The strip names a tab by its place in the view the event came from. */
        case UI_EVENT_CHOOSE: {
            uint32_t id = known ? chrome_tab(c, e.node, e.a) : 0;
            if (id) inbox_command(CMD_SELECT_TAB, (int)id, 0, 0);
            break;
        }
        case UI_EVENT_DISMISS: { /* a menu's (the next view leaves it out), or a tab's close */
            if (!known) break;
            uint32_t id = chrome_tab(c, e.node, e.a);
            if (e.node == c->menu_list) { if (menu_open) { menu_open = false; show_chrome(); } }
            else if (e.node == c->tab_menu) { if (tab_menu_id) { tab_menu_id = 0; show_chrome(); } }
            else if (id) inbox_command(CMD_CLOSE_TAB, (int)id, 0, 0);
            break;
        }
        case UI_EVENT_ADD:
            if (known && e.node == c->strip) inbox_command(CMD_NEW_TAB, 0, 0, 0);
            break;
        case UI_EVENT_MOVE: {
            uint32_t id = known ? chrome_tab(c, e.node, e.a) : 0;
            if (id) inbox_command(CMD_MOVE_TAB, (int)id, (int)e.b, 0);
            break;
        }
        case UI_EVENT_TEXT:
        case UI_EVENT_SUBMIT:
            echo = true;
            if (which == F_ADDR) {
                if (!url_text_set(&tab->address_text, text)) console_add("error", "Address allocation failed");
                else if (e.type == UI_EVENT_SUBMIT) inbox_command(CMD_GO, 0, 0, 0);
            } else if (which == F_FIND) {
                if (!url_text_set(&tab->find_text, text)) { tab->find_error = true; console_add("error", "Search input allocation failed"); }
                else inbox_command(e.type == UI_EVENT_SUBMIT ? CMD_FIND_NEXT : CMD_FIND, 0, 0, 0);
            } else if (which == F_CONSOLE) {
                if (console_set(text, e.text_n) && e.type == UI_EVENT_SUBMIT) inbox_command(CMD_EVAL, 0, 0, 0);
            }
            break;
        case UI_EVENT_FOCUS:
            if (e.node == 0xFFFFFFFFu) { inbox_put((struct gui_event){.type = e.a ? EV_FOCUS : EV_UNFOCUS}); break; }
            if (which == F_PAGE) break;
            if (e.a) {
                if (which == F_ADDR && !url_text_set(&tab->address_text, cur_url)) console_add("error", "Address allocation failed");
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
            tab->need_paint = true;
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
            frame_asked = picture_pending = false;
            if (tab->doc) web_rendering_now(tab->doc, e.a);
            break;
        default: break; /* the window's own size: the surface's comes as UI_EVENT_CANVAS */
        }
    }
    /* A field's new value goes back in the next view: the engine takes a value it sent and
       the app only returned as no change, so a value the browser sets later (a cleared
       console line) is never mistaken for an old one of its own. */
    if (echo) show_chrome();
}
/* The window's input is the front tab's, whichever tab waits for it. */
static int browser_event(struct gui_event *event, int timeout) {
    if (!ui) return -1;
    SERVE(front);
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
        if(engine>=0&&(wait<0||engine<wait)){wait=engine;if(debug_js)loop_profile.engine_waits++;}
        struct n_event events[64];
        int ready=webnet_wait(network,queue,wait,events,64);
        if(ready<0)return -1;
        web_wait_events(events,(size_t)ready);
        webstorage_events(storage,events,(size_t)ready);
        /* The preferences changed (here or in another window): the settings page shows them. */
        for(int i=0;i<ready;i++)if(events[i].token==WEBPREFS_TOKEN && front->settings){show_chrome();break;}
        ui_session_pump(ui);
        take_engine_events();
    }
    if (!inbox_n) return w ? 0 : -1;
    *event = inbox[inbox_first++];
    if (!--inbox_n) inbox_first = 0;
    /* Release/capture cancellation is native state, even while author JS is
       running. Never hit-test, dispatch, or lay out mutable DOM at dequeue. */
    native_active_event(event);browser_hover_boundary(&tab->evq_hover_tail_valid,event);
    if(debug_js && (event->type==EV_MOUSE_MOVE ||
       event->type==EV_MOUSE_DOWN || event->type==EV_MOUSE_UP)) {
        static uint64_t sequence;
        fprintf(stderr,"[web-input] NATIVE_POINTER %lu %d %d\n",
                (unsigned long)++sequence,event->x,event->y);
    }
    return 1;
}
/* ---------------------------------------------------------------- automation */
/* A test tool, not a product feature: WPT's testdriver (tests/wpt/testdriver-vendor.js) drives
   the browser's own input through web_host.automation, which documents get only when the
   browser was started with --wpt-automation. Only tests/wptrunner.c gives it; a page can give
   neither the flag nor a new window with it (new_window passes an absolute URL or nothing).
   A run's steps go into the inbox at their times as the window's input does, so they meet
   everything that input meets: deferral while a page task runs, hit testing, focus, default
   actions. Runs go one after another, each while its tab is the front one and its document
   the one that asked. A marker command after each batch says when the browser has handled
   it; a run ends once the last batch is handled and its duration has passed. A run whose
   document was replaced or closed ends there, and nothing more of it is put. */
static bool automation_enabled;
struct automation_run {
    struct automation_run *next;
    uint32_t tab;      /* its tab's id: a closed tab is no longer found */
    web_doc *document; /* compared, never followed: its tab frees it when it replaces it */
    uint64_t generation, id;
    struct web_automation_step *steps;
    size_t count, fed;
    uint32_t duration;
    uint64_t started;  /* 0 until its tab was in front */
    unsigned marker;   /* the batch in the inbox, 0 for none */
};
static struct automation_run *automation_runs;
/* The automation's pointer and keyboard. A new document gets them with nothing held, where
   the pointer was (WebDriver's input state is the top-level traversable's). */
static struct automation_device automation_device;
static uint64_t automation_generation;
static unsigned automation_serial, automation_handled;
static const char *host_automation(void *opaque, web_doc *document, uint64_t id,
                                   const struct web_automation_step *steps, size_t count, uint32_t duration) {
    SERVE(opaque);
    if (!automation_enabled) return "Automation is not enabled";
    if (!document || document != tab->doc) return "Automation is only for the tab's own document";
    /* A run that cannot be done whole is refused before any of it reaches the page. */
    for (size_t i = 0; i < count; i++) {
        const char *why = automation_step_error(&steps[i]);
        if (why) return why;
    }
    struct automation_run *run = calloc(1, sizeof *run);
    struct web_automation_step *copy = count && count <= SIZE_MAX / sizeof *copy ? malloc(count * sizeof *copy) : NULL;
    if (!run || (count && !copy)) { free(run); free(copy); return "Out of memory for the automation steps"; }
    if (count) memcpy(copy, steps, count * sizeof *copy);
    *run = (struct automation_run){.tab = tab->id, .document = document, .generation = tab->generation, .id = id,
                                   .steps = copy, .count = count, .duration = duration};
    struct automation_run **link = &automation_runs;
    while (*link) link = &(*link)->next;
    *link = run;
    return NULL;
}
/* WebDriver's Cookies for the same testdriver (web_host.automation_cookies): the window's own jar,
   the one document.cookie and the requests use, for a document of the tab's own tree. Nothing of
   a cookie goes to the console or the serial log. */
static long host_automation_cookies(void *opaque, web_doc *document, const char *url, const char *name,
                                    struct webcookie_record **out) {
    SERVE(opaque);
    if (out) *out = NULL;
    if (!automation_enabled || !document || document != tab->doc) return -1;
    return webnet_cookie_associated(network, url, name, out);
}
static long host_automation_delete_cookies(void *opaque, web_doc *document, const char *url, const char *name) {
    SERVE(opaque);
    if (!automation_enabled || !document || document != tab->doc) return -1;
    return webnet_cookie_delete_associated(network, url, name);
}
static void automation_end(const char *error) {
    struct automation_run *run = automation_runs;
    automation_runs = run->next;
    struct tab *t = tab_by_id(run->tab);
    /* Only the document that asked, while it is still its tab's, hears how the run ended. */
    if (t && t->doc == run->document && t->generation == run->generation) web_automation_done(run->document, run->id, error);
    free(run->steps); free(run);
}
static bool automation_put(struct gui_event e) {
    size_t before = inbox_n;
    inbox_put(e);
    return inbox_n != before;
}
/* Called by the main loop between tasks: puts what is due and says in how many ms the next
   step is (-1: nothing to wait for; 0: the main loop brings the run's tab to the front). */
static int automation_service(uint64_t now) {
    while (automation_runs) {
        struct automation_run *run = automation_runs;
        struct tab *t = tab_by_id(run->tab);
        if (!t || t->doc != run->document || t->generation != run->generation) { automation_end(NULL); continue; }
        if (t != front) { front_wanted = t; return 0; }
        if (run->marker && automation_handled != run->marker) return -1; /* its batch waits its turn */
        run->marker = 0;
        if (!run->started) {
            run->started = now;
            if (automation_generation != run->generation) {
                automation_device.buttons = automation_device.modifiers = 0;
                automation_generation = run->generation;
            }
        }
        if (run->fed < run->count) {
            uint64_t due = run->started + run->steps[run->fed].at;
            if (due > now) return (int)MIN(due - now, 0x7fffffff);
            const char *error = NULL;
            for (size_t i = run->fed; i < run->count && run->started + run->steps[i].at <= now; i++)
                if (run->steps[i].kind == WEB_AUTOMATION_KEY_DOWN || run->steps[i].kind == WEB_AUTOMATION_KEY_UP) {
                    /* WebDriver's keys go to the page, not to a field around it. */
                    if (!automation_put((struct gui_event){.type = EV_COMMAND, .key = CMD_AUTOMATION, .y = 1})) error = "Out of memory for input";
                    break;
                }
            for (; !error && run->fed < run->count && run->started + run->steps[run->fed].at <= now; run->fed++) {
                struct gui_event events[3];
                int n = automation_events(&automation_device, &run->steps[run->fed], LINE, events, &error);
                for (int i = 0; i < n && !error; i++) if (!automation_put(events[i])) error = "Out of memory for input";
                if (n > 0 && events[n - 1].type != EV_KEY && events[n - 1].type != EV_WHEEL) {
                    pointer_x = automation_device.x; pointer_y = automation_device.y;
                }
            }
            automation_serial = automation_serial % 0x7fffffffu + 1;
            if (!error && !automation_put((struct gui_event){.type = EV_COMMAND, .key = CMD_AUTOMATION, .x = (int)automation_serial}))
                error = "Out of memory for input";
            if (error) { automation_end(error); continue; }
            run->marker = automation_serial;
            return -1; /* the inbox has the batch: the main loop takes it at once */
        }
        uint64_t end = run->started + run->duration;
        if (end > now) return (int)MIN(end - now, 0x7fffffff);
        automation_end(NULL);
    }
    return -1;
}
/* Only browser-owned navigation commands cancel a running page task. Keep
   the event in FIFO so the ordinary handler acts after QuickJS unwinds. */
static bool native_navigation_command(const struct gui_event *event) {
    if(event->type==EV_KEY && event->pressed) {
        uint32_t key=event->key;
        if(key>='A' && key<='Z')key+=32;
        if((key==NKEY_F5 || ((event->mods&NMOD_CTRL) && key=='r')) && cur_url[0])return true;
        if(event->mods&NMOD_ALT) {
            if(key==NKEY_LEFT && tab->hpos>0)return true;
            if(key==NKEY_RIGHT && tab->hpos<tab->nhist-1)return true;
        }
    }
    if(event->type==EV_COMMAND && !tab->sel_node) {
        switch(event->key) {
        case CMD_BACK: return tab->hpos>0;
        case CMD_FORWARD: return tab->hpos<tab->nhist-1;
        case CMD_RELOAD: return cur_url[0]!=0;
        case CMD_HOME: case CMD_GO: return true;
        case CMD_CLOSE_TAB: return tab_by_id((uint32_t)event->x)==tab;
        }
    }
    return false;
}
/* Keep an explicit user stop available during long author tasks without
   touching a half-mutated document or running another page callback. All
   other native events retain FIFO ownership until the outer JS task ends. */
static bool host_script_checkpoint(void *opaque) {
    SERVE(opaque);
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
        if(tab!=front) {
            /* A tab behind the front one runs a long task: the front tab's input waits its turn,
               and closing the tab that runs ends the task. */
            struct tab *running=tab;tab=front;bool kept=defer_native_event(&e);tab=running;
            if(!kept)return false;
            if(e.type==EV_COMMAND && e.key==CMD_CLOSE_TAB && tab_by_id((uint32_t)e.x)==running)return false;
            continue;
        }
        if(e.type==EV_WHEEL && tab->console_open && e.y>=page_h() && e.y<view.h) {
            devtools_wheel(e.x,e.y,e.wheel);redraw_console();continue;
        }
        /* Escape in native chrome/fullscreen is an ordinary UI command, not
           permission to permanently abort the page's author execution. Keep
           it queued for the normal owner rather than swallowing it here. */
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_ESC && !browser_fullscreen &&
           tab->focus==F_PAGE && !tab->sel_node && !tab->find_open)return false;
        if(e.type==EV_KEY && e.pressed && e.key==NKEY_F12) {
            int old_top=page_h(),old_height=console_h();
            tab->console_open=!tab->console_open;
            if(tab->console_open)tab->focus=F_CONSOLE;else if(tab->focus==F_CONSOLE)tab->focus=F_PAGE;
            tab->need_layout=tab->need_paint=true;
            /* Chrome only: never read the document's half-built layout here.
               The exposed page area is restored after the task unwinds. */
            if(view.px){
                if(tab->console_open)draw_console();
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
/* A tab behind another is hidden, as is every tab of a window that is not shown. */
static unsigned host_window_state(void *opaque) {
    SERVE(opaque);int state=win_state(w);
    if(state<0||tab!=front)return 0;
    return ((state&WIN_STATE_VISIBLE)?WEB_WINDOW_VISIBLE:0) |
           ((state&WIN_STATE_FOCUSED) && tab->focus==F_PAGE?WEB_WINDOW_FOCUSED:0);
}
static void host_window_size(void *opaque, int *width, int *height) {
    (void)opaque;*width=w?w->w:0;*height=w?w->h:0;
}
static bool host_cookie_enabled(void *opaque, const char *url) {
    (void)opaque;
    /* webnet_create succeeds only with an actual session cookie jar; the user may block the
       site's cookies (webprefs.h). */
    return network != NULL && webprefs_storage_allowed(url ? url : "");
}
static char *host_cookie_get(void *opaque, const char *url) {
    SERVE(opaque);
    const char *top_level_url=tab->doc?web_url(tab->doc):"";
    long n = webnet_cookie_get_site(network, url, top_level_url, NULL, 0);
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (out && webnet_cookie_get_site(network, url, top_level_url, out, (size_t)n + 1) < 0) { free(out); out = NULL; }
    return out;
}
static void host_cookie_set(void *opaque, const char *url, const char *value) {
    SERVE(opaque);
    if (webnet_cookie_set_site(network, url, tab->doc?web_url(tab->doc):"", value) < 0) console_add("error", "Cookie storage allocation failed");
}
static int host_storage(void *opaque, const char *origin, const struct web_storage_request *request,
                        struct web_storage_result *out) {
    SERVE(opaque);
    /* A site the user blocked has no storage: the Storage Standard's "the user has disabled
       storage" (obtaining its storage key fails, SecurityError). A release always goes through. */
    if (request && request->operation != WEB_STORAGE_RELEASE && origin && !webprefs_storage_allowed(origin)) {
        if (out) memset(out, 0, sizeof *out);
        return WEB_STORAGE_SECURITY;
    }
    if (!tab->storage_session && storage) tab->storage_session = webstorage_session_open(storage);
    return webstorage_access(storage, tab->storage_session, origin, request, out);
}
static bool host_history(void *opaque, int operation, const char *url, const void *data,
                         size_t len, int value, struct web_history *out) {
    SERVE(opaque);
    if (tab->hpos < 0 || !tab->doc) return false;
    if (operation == WEB_HISTORY_PUSH || operation == WEB_HISTORY_REPLACE) {
        if (!url || (len && !data)) return false;
        char *copy = strdup(url); void *saved = len ? malloc(len) : NULL;
        if (!copy || (len && !saved)) { free(copy); free(saved); return false; }
        char *current, *address;
        if (!url_pair_prepare(url, &current, &address)) { free(copy); free(saved); return false; }
        if (len) memcpy(saved, data, len);
        if ((operation == WEB_HISTORY_PUSH && !hist_reserve((size_t)(tab->hpos + 1) + 1)) ||
            !web_set_url(tab->doc, url)) { free(copy); free(saved); free(current); free(address); return false; }
        bool manual = tab->hist[tab->hpos].manual;
        history_scroll_save();
        if (operation == WEB_HISTORY_PUSH) { if (!hist_append(copy)) { free(copy); free(saved); free(current); free(address); return false; } }
        else { free(tab->hist[tab->hpos].url); free(tab->hist[tab->hpos].state); tab->hist[tab->hpos].url = copy; tab->hist[tab->hpos].entry = ++history_serial; }
        tab->hist[tab->hpos].state = saved; tab->hist[tab->hpos].state_len = len; tab->hist[tab->hpos].manual = manual;
        history_scroll_save();
        url_pair_commit(current, address);
        tab->need_paint = true;
    } else if (operation == WEB_HISTORY_GO) {
        if (tab->history_queue_count == UINT_MAX) return false;
        struct browser_history_task *task = malloc(sizeof *task); if (!task) return false;
        *task = (struct browser_history_task){ .delta=value, .generation=tab->generation };
        if (tab->history_queue_tail) tab->history_queue_tail->next=task; else tab->history_queue=task;
        tab->history_queue_tail=task; tab->history_queue_count++;
    }
    else if (operation == WEB_HISTORY_SCROLL) tab->hist[tab->hpos].manual = value != 0;
    else if (operation != WEB_HISTORY_INFO) return false;
    *out = (struct web_history){.entry=tab->hist[tab->hpos].entry, .length=tab->nhist, .manual_scroll=tab->hist[tab->hpos].manual,
        .state=tab->hist[tab->hpos].state, .state_len=tab->hist[tab->hpos].state_len};
    return true;
}

static void handle_native_wait(const struct gui_event *e);
/* Join a request already owned by the document (modulepreload), without a
   duplicate fetch or re-entering author tasks. The caller owns completed
   throughout this native pump and postpones document/pending retirement. */
static bool host_wait_request(void *opaque, uint64_t resource_id, const bool *completed) {
    SERVE(opaque);
    if (!completed || !tab->doc || tab->native_wait || quit || tab->pending_navigation) return false;
    web_doc *document = tab->doc;
    uint64_t owner_generation = tab->generation;
    tab->native_wait = true;
    tab->native_wait_snapshot=false;
    tab->sync_stopped = false;
    prepare_native_snapshot(false);
    tab->native_wait_snapshot=web_native_input_ready(tab->doc,true);
    if(tab->native_wait_snapshot && tab->need_paint && !quit)redraw();
    while (!*completed && !quit && !tab->sync_stopped && !tab->pending_navigation &&
           tab->doc == document && tab->generation == owner_generation) {
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
    tab->native_wait = false;
    tab->native_wait_snapshot=false;
    return *completed && !quit && !tab->sync_stopped && !tab->pending_navigation &&
           tab->doc == document && tab->generation == owner_generation;
}
struct synchronous_load { bool ready; enum webnet_kind kind; struct tab *owner; web_doc *document; uint64_t generation; struct web_response response; };
static void synchronous_completed(webnet *net, uint64_t id, uint64_t gen,
                                  const struct webnet_response *r, void *opaque) {
    (void)net; (void)id;
    struct synchronous_load *s = opaque;
    SERVE(s->owner);
    if (gen == s->generation && gen == tab->generation && tab->doc == s->document && !tab->pending_navigation && !quit)
        response_copy(&s->response, r, s->kind);
    else { memset(&s->response, 0, sizeof s->response); strcpy(s->response.error, "document was replaced"); }
    s->ready = true;
}

static bool host_sync_request(void *opaque, const struct web_request *request, struct web_response *response) {
    SERVE(opaque);
    memset(response, 0, sizeof *response);
    if (!request || request->kind != WEB_RESOURCE_MODULE || !request->origin || !*request->origin ||
        !request->url || !tab->doc || tab->native_wait || quit || tab->pending_navigation) {
        strcpy(response->error, "module request has no active initiating document"); return false;
    }
    struct synchronous_load *s = calloc(1, sizeof *s);
    if (!s) { strcpy(response->error, "module response allocation failed"); return false; }
    s->kind = network_kind(request->kind); s->owner = tab; s->document = tab->doc; s->generation = tab->generation;
    struct webnet_request r = {
        .kind = s->kind, .generation = s->generation, .url = request->url,
        .origin = request->origin, .method = "GET", .credentials = request->credentials,
        .redirect_error = request->redirect_error, .same_origin = request->same_origin,
        .cache_mode = request->cache_mode, .top_level_url=web_url(tab->doc),
        .referrer = request->referrer, .referrer_policy = request->referrer_policy,
        .destination = (enum webnet_destination)request->destination
    };
    uint64_t id = webnet_submit(network, &r, synchronous_completed, s);
    if (!id) { free(s); strcpy(response->error, "module request could not be started"); return false; }
    tab->native_wait = true;
    tab->native_wait_snapshot=false;
    tab->sync_stopped = false;
    prepare_native_snapshot(false);
    tab->native_wait_snapshot=web_native_input_ready(tab->doc,true);
    if (tab->native_wait_snapshot && tab->need_paint && !quit) redraw();
    uint64_t started = n_clock_uptime_ms();
    while (!s->ready && !quit && !tab->sync_stopped && !tab->pending_navigation &&
           tab->doc == s->document && tab->generation == s->generation) {
        uint64_t now = n_clock_uptime_ms();
        if (now - started >= WEBNET_TIMEOUT_MS) { tab->sync_stopped = true; break; }
        webnet_pump(network, now);
        if (s->ready) break;
        struct gui_event e;
        int timeout = webnet_timeout(network, now);
        int event = browser_event(&e, timeout);
        if (event < 0) { quit = true; break; }
        if (event) handle_native_wait(&e);
        redraw_console();
    }
    tab->native_wait = false;
    tab->native_wait_snapshot=false;
    if (!s->ready || quit || tab->sync_stopped || tab->pending_navigation || tab->doc != s->document || tab->generation != s->generation) {
        webnet_cancel(network, id);
        web_response_free(&s->response);
        free(s);
        strcpy(response->error, quit ? "window closed" : tab->pending_navigation ? "navigation canceled module load" : "module load canceled");
        return false;
    }
    *response = s->response;
    free(s);
    return !response->error[0] && response->status >= 200 && response->status < 300;
}
static bool host_sync_load(void *opaque, const char *url, int kind, struct web_response *response) {
    SERVE(opaque);
    struct web_request request = {.kind=kind, .url=url, .origin=tab->doc?web_url(tab->doc):"", .credentials=WEBNET_CREDENTIALS_SAME_ORIGIN};
    return host_sync_request(opaque, &request, response);
}

static const struct web_host browser_host = {
    .request = host_request, .request_refusal = host_request_refusal, .keepalive_inflight = host_keepalive_inflight, .cancel = host_cancel, .release_request = host_release_request, .resume_request = host_resume_request, .sync_load = host_sync_load,
    .socket_open = host_socket_open, .socket_send = host_socket_send, .socket_close = host_socket_close,
    .socket_buffered = host_socket_buffered, .socket_release = host_socket_release,
    .sync_request = host_sync_request,
    .wait_request = host_wait_request,
    .navigate = host_navigate, .console = host_console, .scroll = host_scroll,
    .scroll_to = host_scroll_to, .history = host_history,
    .window_state=host_window_state,.window_size=host_window_size,
    .navigation_pending=host_navigation_pending,
    .script_checkpoint=host_script_checkpoint,
    .video_present=host_video_present,
    .presentation=host_presentation,
    .cookie_get = host_cookie_get, .cookie_set = host_cookie_set,
    .cookie_enabled = host_cookie_enabled
    , .storage = host_storage, .storage_events = true
    , .media_range = true
    , .display_frames = true
};

static void navigation_completed(webnet *net, uint64_t id, uint64_t gen,
                                 const struct webnet_response *result, void *opaque) {
    (void)net;
    SERVE(opaque);
    if (id != tab->navigation_id || gen != tab->navigation_generation) return;
    tab->navigation_id = 0;
    dt_net_update(tab->dt->nav_serial, result);
    /* The response carries the request's own timing record (webnet.c). */
    web_response_free(&tab->navigation_response);
    response_copy(&tab->navigation_response, result, WEBNET_NAVIGATION);
    tab->navigation_ready = true;
}

static void stop_navigation(void) {
    if (tab->navigation_id) webnet_cancel(network, tab->navigation_id);
    tab->navigation_id = 0;
    tab->navigation_ready = false;
    web_response_free(&tab->navigation_response);
    memset(&tab->navigation_response, 0, sizeof tab->navigation_response);
    memset(&tab->navigation_timing, 0, sizeof tab->navigation_timing);
    tab->loading = false;
}

static void finish_navigation(void) {
    if (!tab->navigation_ready || tab->native_wait || quit) return;
    tab->navigation_ready = false;
    struct web_response *f = malloc(sizeof *f);
    if (!f) {
        web_response_free(&tab->navigation_response);
        tab->loading = false; set_status("Out of memory loading page"); return;
    }
    *f = tab->navigation_response;
    memset(&tab->navigation_response, 0, sizeof tab->navigation_response);
    char ctype[128] = "", charset_buf[64];
    const char *charset = NULL, *image = NULL;
    struct http_resp headers = {0};
    /* Borrow the complete block solely for lookup; do not free this view. */
    headers.headers_full = (char *)web_response_headers(f);
    http_header(&headers, "Content-Type", ctype, sizeof ctype);
    char *html = NULL;
    size_t hlen = 0;
    /* The Content-Type of html when it is the response's own body; NULL for
       a page the browser writes (text/html). */
    const char *document_type = NULL;
    /* The settings are a page of the browser's own, with no document. */
    bool native_page = !f->error[0] && settings_url(web_response_url(f));
    struct settings *page = !native_page ? NULL : tab->settings ? tab->settings : settings_new();
    if (native_page && !page) { web_response_free(f); free(f); tab->loading = false; set_status("Out of memory loading page"); return; }
    /* A page the browser writes is UTF-8; HTML's sniffing would take an unlabeled one as
       windows-1252. A response's own body carries its Content-Type's charset. */
    if (native_page) {}
    else if (f->error[0]) { html = error_page(navigation_url, f->error); charset = "utf-8"; }
    else if (has_prefix(web_response_url(f), "about:")) {
        html = !strcasecmp(web_response_url(f), HOME) ? home_html() : strdup("<!doctype html><title>blank</title>");
        charset = "utf-8";
    } else {
        const char *cs = strstr(ctype, "charset=");
        if (cs) {
            cs += 8; if (*cs == '"') cs++;
            size_t n = MIN(strcspn(cs, "\"; "), sizeof charset_buf - 1);
            memcpy(charset_buf, cs, n); charset_buf[n] = 0; charset = charset_buf;
        }
        bool sniff_html = !ctype[0] && f->body_len && memchr(f->body, '<', MIN(f->body_len, 512));
        /* HTML "load a document": HTML and XML types are documents of their
           own kind (SVG too); the rest the browser displays its own way. */
        enum web_document_kind kind = web_document_kind(ctype);
        if (kind != WEB_DOCUMENT_OTHER || sniff_html) {
            html = f->body; hlen = f->body_len; f->body = NULL;
            if (kind == WEB_DOCUMENT_XML) document_type = ctype;
        } else if (has_prefix(ctype, "image/")) { html = image_page(web_response_url(f)); image = web_response_url(f); charset = "utf-8"; }
        /* The text page wraps the body's bytes: they are in the body's charset, else UTF-8. */
        else if (has_prefix(ctype, "text/") || !ctype[0] || strstr(ctype, "json") ||
                 strstr(ctype, "javascript") || strstr(ctype, "xml")) {
            html = text_page(web_response_url(f), f->body, f->body_len);
            if (!charset) charset = "utf-8";
        } else {
            char message[256];
            snprintf(message, sizeof message, "Cannot display %s (%lu bytes).", ctype, (unsigned long)f->body_len);
            html = error_page(web_response_url(f), message);
            charset = "utf-8";
        }
    }
    if (!html && !native_page) { web_response_free(f); free(f); tab->loading = false; set_status("Out of memory loading page"); return; }
    if (!hlen && html) hlen = strlen(html);
    const char *final_url = web_response_url(f)[0] ? web_response_url(f) : navigation_url;
    web_doc *nd = NULL;
    if (!native_page) {
    struct web_host document_host = browser_host;
    document_host.opaque = tab;
    document_host.debug_js = debug_js;
    document_host.net_log = net_log;
    document_host.net_body = net_body;
    document_host.navigation_timing = tab->navigation_timing;
    /* Borrowed until the document is made; it copies what it keeps. */
    document_host.navigation_timing.fetch = f->timing;
    document_host.navigation_timing.status = f->status;
    document_host.navigation_timing.headers = web_response_headers(f);
    document_host.js_task_budget_ms = js_task_budget_ms;
    document_host.automation = automation_enabled ? host_automation : NULL;
    document_host.automation_cookies = automation_enabled ? host_automation_cookies : NULL;
    document_host.automation_delete_cookies = automation_enabled ? host_automation_delete_cookies : NULL;
    nd = web_live_response(html, hlen, final_url, charset, web_response_headers(f), document_type, &document_host);
    free(html);
    if (!nd) { web_response_free(f); free(f); tab->loading = false; set_status("Out of memory creating document"); return; }
    }

    char *current, *address;
    if (!url_pair_prepare(nd ? web_url(nd) : SETTINGS_URL, &current, &address)) {
        if (nd) web_free(nd);
        if (page != tab->settings) settings_free(page);
        web_response_free(f); free(f); tab->loading = false;
        set_status("Out of memory retaining document URL"); return;
    }
    cancel_document_requests();
    if (tab->doc) web_free(tab->doc);
    if (page != tab->settings) { settings_free(tab->settings); tab->settings = page; }
    tab->popups = 0; free(tab->popup_url); tab->popup_url = NULL;
    tab->doc = nd;
    tab->generation = tab->navigation_generation;
    tab->evq_first=tab->evq_n=tab->evq_scroll_at=0;tab->evq_hover_tail_valid=false;
    url_pair_commit(current, address);
    if (tab->navigation_mode == NAV_PUSH) hist_push(cur_url);
    else if (tab->hpos >= 0) {
        char *copy = strdup(cur_url);
        if (copy) { free(tab->hist[tab->hpos].url); tab->hist[tab->hpos].url = copy; }
        if (tab->navigation_mode == NAV_REPLACE) {
            free(tab->hist[tab->hpos].state); tab->hist[tab->hpos].state = NULL; tab->hist[tab->hpos].state_len = 0;
            tab->hist[tab->hpos].document = tab->generation; tab->hist[tab->hpos].entry = ++history_serial; tab->hist[tab->hpos].x = tab->hist[tab->hpos].y = 0;
        } else {
            uint64_t previous = tab->hist[tab->hpos].document;
            for (int i = 0; i < tab->nhist; i++) if (tab->hist[i].document == previous) tab->hist[i].document = tab->generation;
        }
    }
    /* A new entry keeps how its document was asked for; reload and traversal
       keep the entry's own. */
    if (tab->hpos >= 0 && tab->hist[tab->hpos].document == tab->generation &&
        (tab->navigation_mode == NAV_PUSH || tab->navigation_mode == NAV_REPLACE)) {
        document_state_free(&tab->hist[tab->hpos].source);
        tab->hist[tab->hpos].source = tab->navigation_state; tab->navigation_state = (struct document_state){0};
    }
    int keep = tab->navigation_mode == NAV_PUSH || tab->hpos < 0 || tab->hist[tab->hpos].manual ? 0 : tab->hist[tab->hpos].y;
    int keep_x = tab->navigation_mode == NAV_PUSH || tab->hpos < 0 || tab->hist[tab->hpos].manual ? 0 : tab->hist[tab->hpos].x;
    tab->scroll_x = tab->scroll_y = 0; tab->scroll_event_pending = false;
    tab->find_y = -1; tab->sel_node = tab->pressed_node = tab->hover_node = NULL; tab->hover[0] = 0;
    free(tab->focus_value); tab->focus_value = NULL;
    tab->refresh_at = 0; tab->refresh_seen = false;
    tab->initial_scroll_pending = (tab->hpos < 0 || !tab->hist[tab->hpos].manual) && (tab->navigation_mode != NAV_PUSH || strchr(cur_url, '#') != NULL);
    tab->initial_scroll_anchor = tab->navigation_mode == NAV_PUSH || tab->navigation_mode == NAV_REPLACE;
    tab->initial_scroll_x = keep_x; tab->initial_scroll_y = keep;
    relayout();
    if (image) {
        for (int i = 0; i < web_image_count(tab->doc); i++)
            if (!strcmp(web_image_url(tab->doc, i), image)) web_image_loaded(tab->doc, i, f->body, f->body_len, true); /* the document itself */
        relayout();
    }
    web_response_free(f); free(f);
    int ay = anchor(cur_url);
    scroll_to_xy(keep_x,tab->navigation_mode != NAV_PUSH ? keep : ay >= 0 ? ay : 0);
    tab->loading = false;
    tab->status[0] = 0;
    set_title();
    if (tab->navigation_mode == NAV_HISTORY && tab->doc) web_history_event(tab->doc, cur_url, true);
    /* A history listener can retire SVG snapshots even after relayout. */
    prepare_native_snapshot(false);
    redraw();
}

static void navigate_inner(const struct navigation_request *r, const char *headers) {
    const char *url = r->url; const void *post = r->body; size_t length = r->body_length; int mode = r->mode;
    if (storage && webstorage_pending(storage)) {
        struct web_storage_result result;
        if (webstorage_flush(storage, true, &result) != WEB_STORAGE_OK)
            console_add("error", "Local storage flush failed; changes remain in RAM.");
    }
    /* The settings are the browser's: a page can neither open them nor send a form to them. */
    if (settings_url(url) && ((!r->browser_ui && mode != NAV_HISTORY) || post)) {
        console_add("error", "about:settings opens only from the browser's own controls.");
        return;
    }
    if (tab->hpos >= 0 && mode != NAV_HISTORY) history_scroll_save();
    tab->sel_node = NULL; tab->hover[0] = 0; tab->refresh_at = 0;
    if (tab->focus == F_ADDR) tab->focus = F_PAGE;
    if (!post && tab->doc && mode != NAV_RELOAD &&
        ((mode == NAV_HISTORY && tab->hpos >= 0 && tab->hist[tab->hpos].document == tab->generation) ||
         ((strchr(url, '#') || strchr(cur_url, '#')) && same_doc(url, cur_url)))) {
        char *old_url = strdup(cur_url);
        char *current, *address;
        if (!old_url || !url_pair_prepare(url, &current, &address)) { free(old_url); console_add("error", "Navigation allocation failed"); return; }
        if (!web_set_url(tab->doc, url)) { free(old_url); free(current); free(address); return; }
        int y = anchor(url);
        if (mode == NAV_PUSH) hist_push(url);
        else if (mode == NAV_REPLACE && tab->hpos >= 0) {
            char *copy = strdup(url);
            if (copy) { free(tab->hist[tab->hpos].url); tab->hist[tab->hpos].url = copy; tab->hist[tab->hpos].entry = ++history_serial; }
        }
        url_pair_commit(current, address);
        if (mode == NAV_PUSH || mode == NAV_REPLACE) { if (y >= 0) scroll_to(y); else if (!strchr(url, '#') || !strchr(url, '#')[1]) scroll_to(0); }
        else if (mode == NAV_HISTORY && tab->hpos >= 0 && !tab->hist[tab->hpos].manual) scroll_to_xy(tab->hist[tab->hpos].x,tab->hist[tab->hpos].y);
        web_history_event(tab->doc, old_url, mode == NAV_HISTORY);
        free(old_url);
        /* popstate/hashchange run author code: never paint its retired boxes. */
        prepare_native_snapshot(false);
        redraw(); return;
    }
    if (!url_text_set(&tab->navigation_text, url)) { console_add("error", "Navigation URL allocation failed"); return; }
    if (!document_state_set(&tab->navigation_state, r->initiator, r->referrer, r->referrer_policy)) {
        console_add("error", "Navigation allocation failed"); return;
    }
    stop_navigation();
    tab->navigation_generation = ++next_generation;
    tab->navigation_timing.valid = true;
    tab->navigation_timing.navigation_start_ms = n_clock_uptime_ms();
    /* Navigation Timing's type: a reload, a history traversal, otherwise navigate. */
    tab->navigation_timing.type = mode == NAV_RELOAD ? WEB_NAVIGATION_RELOAD :
        mode == NAV_HISTORY ? WEB_NAVIGATION_BACK_FORWARD : WEB_NAVIGATION_NAVIGATE;
    tab->navigation_mode = mode;
    tab->loading = true;
    char what[256]; snprintf(what, sizeof what, "Loading %.240s", url); set_status(what);
    show_chrome();
    if (has_prefix(url, "about:") || has_prefix(url, "file://")) {
        /* A local fetch: its record has the start and the end, nothing between. */
        struct http_fetch_timing *timing = &tab->navigation_response.timing;
        timing->valid = true; timing->redirect_same_origin = timing->timing_allow_passed = true;
        timing->start_time = timing->post_redirect_start = n_clock_uptime_ms();
        struct fetched *local = calloc(1, sizeof *local);
        if (!local) { strcpy(tab->navigation_response.error, "navigation allocation failed"); tab->navigation_ready = true; return; }
        local->url = strdup(url);
        if (!local->url) { free(local); strcpy(tab->navigation_response.error, "navigation URL allocation failed"); tab->navigation_ready = true; return; }
        if (has_prefix(url, "file://") && !read_local(url, local)) {
            strlcpy(tab->navigation_response.error, local->err, sizeof tab->navigation_response.error);
        } else {
            tab->navigation_response.status = local->status ? local->status : 200;
            tab->navigation_response.body = local->body;
            tab->navigation_response.body_len = local->len;
            snprintf(tab->navigation_response.headers, sizeof tab->navigation_response.headers, "Content-Type: %s\r\n", local->ctype);
        }
        timing->end_time = n_clock_uptime_ms();
        if (!web_response_set_url(&tab->navigation_response, local->url))
            strlcpy(tab->navigation_response.error, "navigation URL allocation failed", sizeof tab->navigation_response.error);
        if (local->body != tab->navigation_response.body) free(local->body);
        free(local->url); free(local);
        tab->navigation_ready = true; return;
    }
    /* The request's origin and its top-level navigation initiator are the
       source document's. The browser UI has none (no client): Sec-Fetch-Site
       none, and SameSite=Strict cookies remain eligible. */
    struct webnet_request rq = {
        .kind = WEBNET_NAVIGATION, .generation = tab->navigation_generation, .url = url,
        .origin = r->initiator, .method = post ? "POST" : "GET",
        .headers = post ? headers : NULL,
        .body = post, .body_len = post ? length : 0, .user_navigation = true,
        .credentials = WEBNET_CREDENTIALS_INCLUDE, .top_level_url = r->initiator,
        .referrer = r->referrer && *r->referrer ? r->referrer : NULL, .referrer_policy = r->referrer_policy,
        .destination = WEBNET_DEST_DOCUMENT,
        .user_activation = r->user_activation || r->browser_ui, .browser_ui = r->browser_ui,
        .report_sent = tab->console_open
    };
    dt_net_clear(); /* the Network panel follows the top-level document */
    tab->navigation_id = webnet_submit(network, &rq, navigation_completed, tab);
    if (tab->navigation_id) tab->dt->nav_serial = dt_net_begin(true, tab->navigation_generation, 0, -1, rq.method, url, rq.headers);
    if (!tab->navigation_id) {
        strcpy(tab->navigation_response.error, "request could not be started");
        (void)web_response_set_url(&tab->navigation_response, url);
        tab->navigation_ready = true;
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
static void navigate_request(const struct navigation_request *in) {
    if (!in->url) return;
    if (tab->native_wait || (tab->doc && web_script_running(tab->doc))) { queue_navigation(in); return; }
    /* The URL and the source can alias the current URL or a history entry. */
    struct navigation_request r = *in;
    char *url = strdup(in->url), *initiator = in->initiator ? strdup(in->initiator) : NULL;
    char *referrer = in->referrer ? strdup(in->referrer) : NULL;
    char *headers = in->body ? navigation_post_headers(in->content_type) : NULL;
    if (!url || (in->initiator && !initiator) || (in->referrer && !referrer)) console_add("error", "Navigation allocation failed");
    else if (in->body && !headers) console_add("error","Form request header is invalid or could not be allocated");
    else { r.url = url; r.initiator = initiator; r.referrer = referrer; navigate_inner(&r, headers); }
    free(headers); free(url); free(initiator); free(referrer);
}
/* The browser's own navigation: its chrome, keys and first page. */
static void navigate(const char *url, int mode) {
    struct navigation_request r = browser_navigation(url, mode);
    navigate_request(&r);
}
/* A navigation the page itself asks for (history.go, a refresh), or a link
   the user follows in it: the page is the source. Reload and traversal
   fetch with their entry's request referrer. */
static struct navigation_request page_navigation(const char *url, int mode, web_node *element) {
    struct web_navigation source = {0};
    web_navigation_source(tab->doc, element, &source);
    struct navigation_request r = {.url = url, .mode = mode, .initiator = source.initiator ? source.initiator : "",
        .referrer = source.referrer, .referrer_policy = source.referrer_policy, .user_activation = source.user_activation};
    if ((mode == NAV_RELOAD || mode == NAV_HISTORY) && tab->hpos >= 0) {
        r.referrer = tab->hist[tab->hpos].source.referrer; r.referrer_policy = tab->hist[tab->hpos].source.referrer_policy;
    }
    return r;
}

static void apply_pending_navigation(void) {
    if (!tab->pending_navigation || tab->native_wait || quit || (tab->doc && web_script_running(tab->doc))) return;
    char *url = tab->pending_text.p;
    tab->pending_text = (struct url_text){0};
    struct pending_request p = tab->pending;
    tab->pending = (struct pending_request){0}; tab->pending_navigation = false;
    struct navigation_request r = {.url = url, .body = p.body, .body_length = p.body_length,
        .content_type = p.content_type, .mode = p.mode, .initiator = p.initiator, .referrer = p.referrer,
        .referrer_policy = p.referrer_policy, .user_activation = p.user_activation, .browser_ui = p.browser_ui};
    navigate_request(&r);
    free(url); pending_free(&p);
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
    navigate(url, NAV_PUSH); free(url);
}

static void go_history(int d, bool browser_ui) {
    int64_t p = (int64_t)tab->hpos + d;
    if (p < 0 || p >= tab->nhist) return;
    history_scroll_save();
    tab->hpos = (int)p;
    if (browser_ui) navigate(tab->hist[tab->hpos].url, NAV_HISTORY);
    else { struct navigation_request r = page_navigation(tab->hist[tab->hpos].url, NAV_HISTORY, NULL); navigate_request(&r); }
}
static void apply_pending_history(void) {
    if (!tab->history_queue_count || tab->loading || tab->native_wait || quit || (tab->doc && web_script_running(tab->doc))) return;
    struct browser_history_task *task = tab->history_queue;
    tab->history_queue=task->next; if (!tab->history_queue) tab->history_queue_tail=NULL; tab->history_queue_count--;
    int delta = task->delta; uint64_t document_generation = task->generation; free(task);
    if (document_generation != tab->generation) return;
    if (!delta) { struct navigation_request r = page_navigation(cur_url, NAV_RELOAD, NULL); navigate_request(&r); }
    else go_history(delta, false);
}

static void submit(web_node *n) {
    if (!web_sandbox_form_submission_allowed(tab->doc, n)) return;
    web_node *form = web_form_owner(tab->doc, n);
    if(!form || !web_form_submission_validate(tab->doc,n)) {flush_dom_layout();return;}
    struct web_event event = {.type = "submit", .bubbles = true, .cancelable = true};
    struct web_hit action={0};
    if(web_node_action(tab->doc,n,&action) && action.kind==WEB_HIT_SUBMIT)event.submitter=n;
    bool allowed = form && web_dispatch(tab->doc, form, &event);
    flush_dom_layout();
    if (!allowed) return;
    web_form_navigate(tab->doc, n);
    flush_dom_layout();
}

/* ---------------------------------------------------------------- input */
static void page_click(web_node *target, int x, int y, bool keyboard, uint32_t mods);
/* Enter in a field: the default button is clicked, so it is the submitter. */
static void implicit_submit(web_node *field) {
    web_node *submitter = web_form_implicit_submission(tab->doc, field);
    if (submitter && submitter != field) page_click(submitter, 0, 0, true, 0);
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
    if (!tab->doc || tab->native_wait) return true;
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
    bool allowed = web_dispatch(tab->doc, target, &event);
    flush_dom_layout();
    return allowed;
}
static bool dispatch_native(const char *type, web_node *target, const struct gui_event *e, bool cancelable) {
    return dispatch_native_mode(type,target,e,cancelable,false);
}
static web_node *native_capture_target(const struct gui_event *e) {
    if(!tab->doc || tab->native_wait || !e)return NULL;
    struct web_event event={.x=e->x,.y=e->y,.button=e->type==EV_MOUSE_MOVE?-1:0,.buttons=e->buttons,
        .ctrl=(e->mods&NMOD_CTRL)!=0,.shift=(e->mods&NMOD_SHIFT)!=0,.alt=(e->mods&NMOD_ALT)!=0};
    return web_pointer_capture_target(tab->doc,&event);
}

static void page_focus(web_node *node) {
    if (!tab->doc) return;
    web_node *old = web_focused(tab->doc);
    if (node && old == node) return;
    web_focus(tab->doc,node);
    node=web_focused(tab->doc);
    if (old == node) return;
    if (old) {
        const char *value = web_control_value(old);
        if (tab->focus_value && value && strcmp(tab->focus_value, value)) dispatch_native("change", old, NULL, false);
        struct web_event blur = {.type = "blur", .related_target=node};
        web_dispatch(tab->doc, old, &blur);
        flush_dom_layout();
        blur.type="focusout";blur.bubbles=true;web_dispatch(tab->doc,old,&blur);flush_dom_layout();
    }
    if (web_focused(tab->doc)!=node) return; /* a blur/focusout listener chose another control */
    free(tab->focus_value);
    const char *value = node ? web_control_value(node) : NULL;
    tab->focus_value = value ? strdup(value) : NULL;
    if (node) {
        struct web_event event = {.type = "focus", .related_target=old};
        web_dispatch(tab->doc, node, &event);
        flush_dom_layout();
        if(web_focused(tab->doc)==node){event.type="focusin";event.bubbles=true;web_dispatch(tab->doc,node,&event);flush_dom_layout();}
    }
}

static void open_select(web_node *n) {
    int sel = 0;
    tab->sel_datalist = tab->sel_autocomplete = false;
    int needed = web_select_options(tab->doc, n, NULL, 0, &sel);
    if (!needed) { needed = web_datalist_options(tab->doc, n, NULL, 0); tab->sel_datalist = needed > 0; }
    if (needed < 0) return;
    if (!needed) { needed = web_autocomplete_options(tab->doc, n, NULL, 0); tab->sel_autocomplete = needed > 0; }
    int capacity = MAX(needed, 1);
    if (capacity > tab->sel_capacity) {
        if ((size_t)capacity > SIZE_MAX / sizeof *tab->sel_labels) return;
        void *grown = realloc(tab->sel_labels, (size_t)capacity * sizeof *tab->sel_labels);
        if (!grown) { console_add("error", "Control option storage allocation failed"); return; }
        tab->sel_labels = grown; tab->sel_capacity = capacity;
    }
    if (tab->sel_autocomplete) {
        tab->sel_n = web_autocomplete_options(tab->doc, n, tab->sel_labels, tab->sel_capacity);
        tab->sel_datalist = tab->sel_autocomplete = tab->sel_n > 0;
    } else tab->sel_n = tab->sel_datalist ? web_datalist_options(tab->doc, n, tab->sel_labels, tab->sel_capacity) :
        web_select_options(tab->doc, n, tab->sel_labels, tab->sel_capacity, &sel);
    if (tab->sel_n <= 0 || tab->sel_n != needed) return;
    int x, y, ww, h;
    if (!web_node_rect(tab->doc, n, &x, &y, &ww, &h)) return;
    tab->sel_node = n;
    tab->sel_cur = sel < 0 ? 0 : sel;
    tab->sel_x = x - tab->scroll_x;
    tab->sel_w = ww;
    for (int i = 0; i < tab->sel_n; i++) tab->sel_w = MAX(tab->sel_w, gfx_text_width(tab->sel_labels[i], FONT_SMALL) + 16);
    tab->sel_x = MAX(0, MIN(tab->sel_x, page_w() - tab->sel_w));
    tab->sel_y = y + h - tab->scroll_y;
    int room = (view.h - tab->sel_y) / SEL_H;
    if (room < MIN(tab->sel_n, 6)) { /* not enough room below: open upwards */
        int up = MIN(tab->sel_n, (y - tab->scroll_y) / SEL_H);
        if (up > room) tab->sel_y = y - tab->scroll_y - up * SEL_H;
    }
    room = (view.h - tab->sel_y) / SEL_H;
    tab->sel_top = tab->sel_cur >= room ? tab->sel_cur - room + 1 : 0;
}

static void choose_select(int i) {
    if (tab->sel_node && i >= 0 && i < tab->sel_n) {
        if (!tab->sel_datalist || (tab->sel_autocomplete ? web_autocomplete_choose(tab->doc, tab->sel_node, i) : web_datalist_choose(tab->doc, tab->sel_node, i))) {
            if (!tab->sel_datalist) web_select_set(tab->doc, tab->sel_node, i);
            dispatch_native("input", tab->sel_node, NULL, false);
            if (!tab->sel_datalist) dispatch_native("change", tab->sel_node, NULL, false);
        }
    }
    tab->sel_node = NULL;
    relayout();
}

/* The fields edit themselves in the engine; these give them the keyboard and act on what they hold. */
static void addr_focus(void) {
    if (!url_text_set(&tab->address_text, cur_url)) { console_add("error", "Address allocation failed"); return; }
    tab->focus = F_ADDR;
    focus_renew = true; /* again, even when it has the keyboard already: select all of it */
}

static void find_next(int from) {
    if (!tab->doc) return;
    int result = web_find(tab->doc, find_buf, from);
    tab->find_error = result == -2;
    if (tab->find_error) { console_add("error", "Search query allocation failed"); return; }
    tab->find_y = result;
    if (tab->find_y >= 0 && (tab->find_y < tab->scroll_y || tab->find_y > tab->scroll_y + page_h() - 40)) scroll_to(tab->find_y - page_h() / 3);
}

static void find_close(void) {
    tab->find_open = false;
    if (tab->focus == F_FIND) tab->focus = F_PAGE;
    if (tab->doc) web_find(tab->doc, "", 0);
}

/* Keys for the DevTools panel while the console has the keyboard; the line itself is the field's. */
static void console_key(const struct gui_event *e) {
    if(tab->dt->panel!=DT_CONSOLE&&tab->dt->panel!=DT_TRACE){devtools_key(e);return;}
    struct console_log *log=tab->dt->panel==DT_TRACE?&tab->console_trace:&tab->console_page;
    uint32_t k=e->key;bool ctrl=e->mods&NMOD_CTRL;
    if(ctrl&&k>='A'&&k<='Z')k+=32;
    if(ctrl&&(k=='['||k==']')){devtools_set_tab((tab->dt->panel+(k==']'?1:DT_NTABS-1))%DT_NTABS);return;}
    if(k==NKEY_PGUP){console_scroll(log,console_visible());return;}
    if(k==NKEY_PGDN){console_scroll(log,-console_visible());return;}
    if(ctrl&&k==NKEY_HOME){console_scroll(log,INT_MAX);return;}
    if(ctrl&&k==NKEY_END){console_scroll(log,-INT_MAX);return;}
    if(ctrl&&k=='c'&&(e->mods&NMOD_SHIFT)){
        bool copied=console_copy_log(log);log->skip=0;
        console_add(!copied?"error":log==&tab->console_trace?"trace":"log",copied?"Retained console log copied to Nocturne clipboard.":"Console log copy failed.");return;
    }
}

static void console_eval(void) {
    size_t length=strlen(tab->console_input);
    if(!length)return;
    tab->console_page.skip=0;
    console_add("input",tab->console_input);
    if(!web_console_eval(tab->doc,tab->console_input,length))console_add("error","Console execution failed or the page runtime is unavailable.");
    tab->console_input[0]=0;
    tab->need_layout=true;
}

/* These are native user commands. Page scripts receive the ordinary editing
   events, not permission to read the system clipboard outside a user key. */
static bool page_clipboard_key(web_node *target, const struct gui_event *e) {
    if (!tab->doc || !(e->mods & NMOD_CTRL) || (e->mods & NMOD_ALT)) return false;
    uint32_t key = e->key;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    /* A text control has its own selection; otherwise the document's selection,
       and an editing host takes the cut and the paste. */
    bool editing = web_editing_target(tab->doc);
    if (key == 'c' || key == 'x') {
        size_t length = 0;
        char *selected = target ? web_control_selected_text(tab->doc,target,&length) : NULL;
        if (selected) {
            if (length && clipboard_set(selected,length) >= 0 && key == 'x')
                web_control_edit(tab->doc,target,"deleteByCut",NULL,0);
            free(selected);
            return true;
        }
        selected = web_selection_text(tab->doc,&length);
        if (!selected) return editing;
        if (length && clipboard_set(selected,length) >= 0 && key == 'x' && editing)
            web_editing_edit(tab->doc,"deleteByCut",NULL,0);
        free(selected);
        return true;
    }
    if (key == 'v') {
        if (!target && !editing) return false;
        char dummy = 0;
        int capacity = clipboard_get(&dummy,0);
        if (capacity <= 0) return true;
        char *text = malloc((size_t)capacity + 1);
        if (!text) return true;
        int length = clipboard_get(text,(size_t)capacity);
        /* A concurrent clipboard replacement must not paste a truncated value. */
        if (length > 0 && length <= capacity && !memchr(text,0,(size_t)length)) {
            text[length] = 0;
            if (!target || !web_control_edit(tab->doc,target,"insertFromPaste",text,(size_t)length))
                if (editing) web_editing_edit(tab->doc,"insertFromPaste",text,(size_t)length);
        }
        free(text);
        return true;
    }
    return false;
}

/* A new tab at the end of the strip, at the front, with the start page and the keyboard in
   the address field. */
static void new_tab(void) {
    struct tab *t = tab_new(NULL);
    if (!t) { console_add("error", "Tab allocation failed"); return; }
    tab_select(t);
    tab = t;
    navigate(HOME, NAV_PUSH);
    addr_focus();
    show_chrome();
}
/* A new window is another browser, a program of its own. Its output and log go where this
   window's go, if it has them. */
static void new_window(const char *url) {
    const char *args[] = {"browser", url};
    int32_t handles[] = {0, 1, 2};
    struct n_launch launch = {.arguments = args, .argument_count = url ? 2 : 1, .handles = handles, .handle_count = 3};
    char self[1024];
    n_handle app = find_program("browser", self, sizeof self) ? n_app_launch(self, &launch) : -N_ERR_NOT_FOUND;
    if (app == -N_ERR_NO_HANDLE) { launch.handle_count = 0; app = n_app_launch(self, &launch); }
    if (app < 0) console_add("error", "Unable to open the new window");
    else n_handle_close(app);
}
static int tab_index(const struct tab *t) {
    for (int i = 0; i < ntabs; i++) if (tabs[i] == t) return i;
    return -1;
}
/* With Ctrl, before the page has them: T a new tab, W or F4 closes this one, Tab or PgDn the
   next one, Shift+Tab or PgUp the one before, 1..8 that one, 9 the last one, N a new window. */
static bool tab_key(uint32_t k, bool shift) {
    int at = tab_index(front);
    if (k == 't' && !shift) { new_tab(); return true; }
    if ((k == 'w' && !shift) || k == NKEY_F4) { tab_close(front); return true; }
    if (k == NKEY_TAB || k == NKEY_PGDN || k == NKEY_PGUP) {
        int step = k == NKEY_PGUP || (k == NKEY_TAB && shift) ? ntabs - 1 : 1;
        tab_select(tabs[(at + step) % ntabs]);
        return true;
    }
    if (k >= '1' && k <= '9' && !shift) {
        int to = k == '9' ? ntabs - 1 : (int)(k - '1');
        if (to < ntabs) tab_select(tabs[to]);
        return true;
    }
    if (k == 'n' && !shift) { new_window(NULL); return true; }
    return false;
}

/* F12: DevTools open below the page with the keyboard in the console, or close. */
static void devtools_toggle(void) {
    tab->console_open = !tab->console_open;
    if(tab->console_open)tab->focus=F_CONSOLE;else if(tab->focus==F_CONSOLE)tab->focus=F_PAGE;
    if(!tab->console_open){tab->dt->picking=false;dt_set_hover(NULL);}
    tab->need_layout = true;
}
/* Ctrl+F: the find bar, with the keyboard. */
static void find_bar_open(void) {
    tab->find_open = true;
    tab->focus = F_FIND;
    focus_renew = true; /* again, when it has the keyboard already */
    tab->sel_node = NULL;
}

static void key(const struct gui_event *e) {
    if(e->pressed&&browser_fullscreen&&(e->key==NKEY_ESC||((e->mods&NMOD_CTRL)&&(e->key=='l'||e->key=='L')))){
        web_presentation_exit(fullscreen_doc,1);if(e->key==NKEY_ESC)return;
    }
    bool ctrl = e->mods & NMOD_CTRL, alt = e->mods & NMOD_ALT;
    uint32_t k = e->key;
    if (k >= 'A' && k <= 'Z' && (ctrl || alt)) k += 32;
    if (k == NKEY_F12) { devtools_toggle(); return; }
    /* Ctrl+Shift+C copies the console log while typing in the console. */
    if (ctrl && (e->mods & NMOD_SHIFT) && (k == 'i' || k == 'j' ||
        (k == 'c' && !(tab->console_open && tab->focus == F_CONSOLE && (tab->dt->panel == DT_CONSOLE || tab->dt->panel == DT_TRACE))))) {
        devtools_shortcut(k);
        return;
    }
    if (tab->console_open && ctrl && k == 'k') {
        devtools_clear();
        return;
    }
    if (k == NKEY_ESC && tab->loading && !web_modal_active(tab->doc)) {
        stop_navigation();
        set_status("Stopped");
        return;
    }
    if (ctrl && !alt && tab_key(k, (e->mods & NMOD_SHIFT) != 0)) return;
    if (ctrl && k == 'l') {
        tab->sel_node = NULL;
        addr_focus();
        return;
    }
    if (ctrl && k == 'f') { find_bar_open(); return; }
    if (k == NKEY_F5 || (ctrl && k == 'r')) {
        if (cur_url[0]) navigate(cur_url, NAV_RELOAD);
        return;
    }
    if (alt && k == NKEY_LEFT) return go_history(-1, true);
    if (alt && k == NKEY_RIGHT) return go_history(1, true);
    if(tab->console_open&&tab->focus==F_CONSOLE)return console_key(e);
    if (tab->sel_node) {
        int room = MAX((view.h - tab->sel_y) / SEL_H, 1);
        if (k == NKEY_ESC) tab->sel_node = NULL;
        else if (k == NKEY_ENTER || k == ' ') choose_select(tab->sel_cur);
        else if (k == NKEY_UP || k == NKEY_DOWN) {
            tab->sel_cur = MAX(0, MIN(tab->sel_n - 1, tab->sel_cur + (k == NKEY_UP ? -1 : 1)));
            if (tab->sel_cur < tab->sel_top) tab->sel_top = tab->sel_cur;
            if (tab->sel_cur >= tab->sel_top + room) tab->sel_top = tab->sel_cur - room + 1;
        }
        return;
    }
    if (tab->focus == F_ADDR || tab->focus == F_FIND || (tab->settings && settings_typing)) return; /* a key the field had no use for */
    web_node *target = tab->doc ? web_focused(tab->doc) : NULL;
    if (!dispatch_native("keydown", target, e, true)) return;
    if (!(e->mods & (NMOD_CTRL | NMOD_ALT)) &&
        ((e->key >= 32 && !(e->key >= 0x100 && e->key < 0x200)) || e->key == NKEY_ENTER))
        if (!dispatch_native("keypress", target, e, true)) return;
    if (page_clipboard_key(tab->doc ? web_focused(tab->doc) : NULL,e)) return;
    if (target && k == NKEY_DOWN && !ctrl && !alt) { open_select(target); if (tab->sel_node) return; }
    if(target && (k==NKEY_ENTER || k==' ') && !ctrl && !alt) {
        if (k==' ' && web_media_activate(tab->doc,target)) { flush_dom_layout(); return; }
        struct web_hit action={0};
        if(web_node_action(tab->doc,target,&action) &&
           (action.kind==WEB_HIT_DETAILS || action.kind==WEB_HIT_BUTTON || action.kind==WEB_HIT_SUBMIT ||
            action.kind==WEB_HIT_CHECKBOX || action.kind==WEB_HIT_RADIO || action.kind==WEB_HIT_FILE ||
            (k==NKEY_ENTER && action.kind==WEB_HIT_LINK))) {page_click(target,0,0,true,e->mods);return;}
    }
    if (tab->doc && (web_focused(tab->doc) || web_editing_target(tab->doc))) {
        if (k == NKEY_ESC) {
            page_focus(NULL);
            return;
        }
        int r = web_key(tab->doc, e);
        if (r == 2) implicit_submit(web_focused(tab->doc));
        if (r) return;
        if (e->key >= 32 && e->key < 127) return; /* typed into a control that ignored it */
    }
    if (k == NKEY_BACKSPACE && !ctrl) return go_history(-1, true);
    if (k == NKEY_ESC && tab->find_open) {
        find_close();
        return;
    }
    scroll_key(e);
}

static void page_click(web_node *target, int x, int y, bool keyboard, uint32_t mods) {
    tab->focus = F_PAGE;
    if (!tab->doc) return;
    if(web_control_disabled(target))return;
    /* Author click listeners may reparent the target. Capture its first
       native link activation now, without changing the event target. */
    web_node *anchor = web_link_activation_anchor(tab->doc,target);
    struct gui_event native = {.x = x, .y = y, .buttons = 0, .mods = mods};
    struct web_control_activation activation;
    web_control_activation_begin(tab->doc, target, &activation);
    bool allowed = dispatch_native_mode("click", target, &native, true, keyboard);
    bool input_events = web_control_activation_end(tab->doc, &activation, allowed);
    if (!allowed) { flush_dom_layout(); return; }
    if (!anchor && web_media_activate(tab->doc,target)) { flush_dom_layout(); return; }
    web_node *label_control = anchor ? NULL : web_label_activation(target);
    if (label_control) { page_focus(label_control); page_click(label_control, x, y, keyboard, mods); return; }
    struct web_hit hit = {0};
    if (anchor) {
        /* Read this same anchor's latest href after dispatch. Removal of
           href or reparenting must not select another ancestor anchor. */
        if (!web_link_action(tab->doc,anchor,&hit)) return;
    } else if (!web_node_action(tab->doc,target,&hit) || hit.kind==WEB_HIT_LINK) return;
    if (!keyboard && hit.kind==WEB_HIT_DETAILS) {
        struct web_hit pointer={0};
        if (!web_hit_test(tab->doc,document_x(x),y+tab->scroll_y,&pointer) || pointer.kind!=WEB_HIT_DETAILS || pointer.node!=hit.node) return;
    }
    switch (hit.kind) {
    case WEB_HIT_LINK:
        if (hit.href) {
            if (has_prefix(hit.href, "file://") && !has_prefix(web_url(tab->doc), "file://") &&
                !has_prefix(web_url(tab->doc), "about:")) {
                set_status("A remote page cannot open a local file");
                return;
            }
            /* Ctrl: a new tab behind this one; Shift: a new window. */
            if (mods & (NMOD_CTRL | NMOD_SHIFT)) {
                struct navigation_request r = page_navigation(hit.href, NAV_PUSH, anchor);
                if (mods & NMOD_CTRL) tab_open(&r, tab, false);
                else new_window(hit.href);
                return;
            }
            if (!web_frame_navigate(tab->doc, anchor, hit.href, true)) {
                struct navigation_request r = page_navigation(hit.href, NAV_PUSH, anchor);
                queue_navigation(&r);
            }
        }
        return;
    case WEB_HIT_TEXT_INPUT:
    case WEB_HIT_TEXTAREA: break; /* focusing belongs to the preceding mousedown */
    case WEB_HIT_FILE:
        if (web_input_choose_files(tab->doc, hit.node)) {
            dispatch_native("input", hit.node, &native, false);
            dispatch_native("change", hit.node, &native, false);
            if (web_focused(tab->doc) == hit.node) { free(tab->focus_value); tab->focus_value = strdup(web_control_value(hit.node)); }
            flush_dom_layout();
        }
        break;
    case WEB_HIT_CHECKBOX:
    case WEB_HIT_RADIO:
        if (input_events) { dispatch_native("input", hit.node, &native, false); dispatch_native("change", hit.node, &native, false); }
        break;
    case WEB_HIT_SUBMIT: submit(hit.node); return;
    case WEB_HIT_BUTTON: web_reset(tab->doc,hit.node); flush_dom_layout(); break;
    case WEB_HIT_DETAILS:
        web_toggle(tab->doc,hit.node); flush_dom_layout(); break;
    case WEB_HIT_SELECT:
        open_select(hit.node);
        break;
    default: break;
    }
}

static void scrollbar_drag(int my) {
    int ph = page_h();
    if (tab->doc_h <= ph) return;
    int th = MAX(24, (int)((long)ph * ph / tab->doc_h));
    int range = ph - th;
    if (range <= 0) return;
    scroll_to((int)((long)(my - drag_off) * max_scroll() / range));
}

/* whether a point of the surface is on the open select list's options */
static bool select_list_at(int x, int y) {
    int vis = MIN(tab->sel_n - tab->sel_top, (view.h - tab->sel_y) / SEL_H);
    return x >= tab->sel_x && x < tab->sel_x + tab->sel_w && y >= tab->sel_y && y < tab->sel_y + vis * SEL_H;
}
static void mouse_down(const struct gui_event *e) {
    if (!(e->buttons & 1)) return;
    if(tab->pressed_node)return; /* an additional button is not a new primary press */
    web_active_release(tab->doc);tab->pressed_node=NULL;
    int x = e->x, y = e->y;
    if (devtools_pointer_down(x, y)) return;
    if (tab->sel_node) {
        if (select_list_at(x, y)) choose_select(tab->sel_top + (y - tab->sel_y) / SEL_H);
        else tab->sel_node = NULL;
        return;
    }
    if (x >= view.w - SCRW) {
        int ph = page_h();
        if (tab->doc_h <= ph) return;
        int th = MAX(24, (int)((long)ph * ph / tab->doc_h));
        int ty = (int)((long)tab->scroll_y * (ph - th) / max_scroll());
        if (y < ty) scroll_to(tab->scroll_y - (ph - LINE));
        else if (y >= ty + th) scroll_to(tab->scroll_y + (ph - LINE));
        else {
            drag_sb = true;
            drag_off = y - ty;
        }
        return;
    }
    tab->focus = F_PAGE;
    if (tab->doc) {
        tab->pressed_node = web_node_at(tab->doc, document_x(x), y + tab->scroll_y);
        web_active_press(tab->doc,tab->pressed_node);
        web_frame_address_select(tab->doc, tab->pressed_node); tab->need_paint = true;
        /* Dispatch may cancel the compatibility mouse event, remove the
           control or change its frame. Resolve the current focus area after
           it, before mouseup/click handlers observe activeElement. */
        if (dispatch_native("mousedown", tab->pressed_node, e, true)) {
            web_node *area = NULL;
            if (web_pointer_focus_target(tab->doc, tab->pressed_node, &area)) page_focus(area);
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
    char h[sizeof tab->hover] = "";
    web_node *node = NULL;
    if (tab->doc && !tab->sel_node && e->y >= 0 && e->y < page_h() && e->x >= 0 && e->x < page_w()) {
        struct web_hit hit;
        if (web_hit_test(tab->doc, document_x(e->x), e->y + tab->scroll_y, &hit) && hit.href) strlcpy(h, hit.href, sizeof h);
        node = web_node_at(tab->doc, document_x(e->x), e->y + tab->scroll_y);
    }
    if (tab->doc && !tab->native_wait && !web_script_running(tab->doc)) {
        web_node *captured=native_capture_target(e);
        if(captured)node=captured;
        struct web_event event = {
            .x=e->x, .y=e->y, .buttons=e->buttons,
            .ctrl=(e->mods & NMOD_CTRL)!=0, .shift=(e->mods & NMOD_SHIFT)!=0,
            .alt=(e->mods & NMOD_ALT)!=0
        };
        web_hover(tab->doc, node, &event);
        tab->hover_node = node;
        flush_dom_layout();
    }
    if (strcmp(h, tab->hover)) {
        strlcpy(tab->hover, h, sizeof tab->hover);
        show_chrome();
    }
}

/* ---- the pointer over the page's surface (css-ui-4 cursor). What the point wants (DevTools, the
   scroll bar, the select list, else the page's cursor) is told to the window when it changes: at
   once after input and after the first layout that follows it, otherwise at most every
   POINTER_SETTLE_MS for a change under a still pointer (a layout, a scroll, another document or
   tab, an image the cursor waits for). The window shows it only while the pointer is over the
   surface; the widgets around it (the tab strip, the toolbar, the fields) choose their own. */
#define POINTER_SETTLE_MS 100
static struct {
    struct web_cursor want;
    uint32_t pixels[N_POINTER_IMAGE_MAX * N_POINTER_IMAGE_MAX]; /* the image the window has */
    struct n_win_pointer sent;
    bool asked;   /* the window has one */
    bool input;   /* the pointer moved or pressed: look at once */
    bool settle;  /* the look after input came before the page's next layout: look again at it */
    bool again;   /* the last look could not finish (the page was busy, an image was loading, sending failed) */
    /* what the last look saw */
    const struct tab *tab;
    uint64_t generation, layouts;
    int scroll_x, scroll_y;
    uint64_t next; /* the earliest look for a change that is not input */
} pointer;

static bool pointer_want(int x, int y, struct web_cursor *want) {
    want->shape = N_POINTER_DEFAULT;
    want->width = want->height = want->hot_x = want->hot_y = 0;
    want->pending = false;
    uint32_t shape;
    if (devtools_pointer_shape(x, y, &shape)) { want->shape = shape; return true; }
    if (drag_sb || (tab->sel_node && select_list_at(x, y))) return true;
    /* the scroll bar, and the surface of a tab without a document */
    if (!tab->doc || x < 0 || y < 0 || x >= page_w() || y >= page_h()) return true;
    return web_cursor_at(tab->doc, document_x(x), y + tab->scroll_y, want);
}
static void pointer_send(const struct web_cursor *want) {
    size_t pixels = (size_t)want->width * (size_t)want->height;
    const struct n_win_pointer *sent = &pointer.sent;
    if (pointer.asked && sent->shape == want->shape && sent->width == (uint32_t)want->width &&
        sent->height == (uint32_t)want->height && sent->hot_x == (uint32_t)want->hot_x && sent->hot_y == (uint32_t)want->hot_y &&
        !memcmp(pointer.pixels, want->pixels, pixels * sizeof *want->pixels))
        return;
    struct n_win_pointer p = {.shape = want->shape};
    if (pixels) {
        p.width = (uint32_t)want->width; p.height = (uint32_t)want->height;
        p.hot_x = (uint32_t)want->hot_x; p.hot_y = (uint32_t)want->hot_y;
        p.pixels = want->pixels;
    }
    if (ui_surface_pointer(ui, VIEW, &p) < 0) { pointer.again = true; return; }
    memcpy(pointer.pixels, want->pixels, pixels * sizeof *want->pixels);
    p.pixels = pointer.pixels;
    pointer.sent = p;
    pointer.asked = true;
}
/* In the main loop with the front tab served, between script tasks; *timeout shortens to the
   next look a change waits for. */
static void pointer_service(uint64_t now, int *timeout, int *source) {
    if (!ui || quit || tab != front || tab->settings || tab->native_wait || web_script_running(tab->doc) || pointer_x < 0) return;
    bool laid_out = pointer.layouts != page_layouts;
    bool changed = pointer.input || pointer.again || laid_out || pointer.tab != tab || pointer.generation != tab->generation ||
                   pointer.scroll_x != tab->scroll_x || pointer.scroll_y != tab->scroll_y;
    if (!changed) return;
    bool now_due = pointer.input || pointer.tab != tab || (pointer.settle && laid_out);
    if (!now_due && now < pointer.next) {
        int due = (int)MIN(pointer.next - now, (uint64_t)POINTER_SETTLE_MS);
        if (*timeout < 0 || due < *timeout) { *timeout = due; *source = WAIT_POINTER; }
        return;
    }
    pointer.settle = pointer.input;
    pointer.input = false;
    pointer.next = now + POINTER_SETTLE_MS;
    pointer.tab = tab;
    pointer.generation = tab->generation;
    pointer.layouts = page_layouts;
    pointer.scroll_x = tab->scroll_x;
    pointer.scroll_y = tab->scroll_y;
    pointer.again = !pointer_want(pointer_x, pointer_y, &pointer.want);
    if (!pointer.again) {
        pointer.again = pointer.want.pending;
        pointer_send(&pointer.want);
    }
    if (pointer.again && (*timeout < 0 || *timeout > POINTER_SETTLE_MS)) {
        *timeout = POINTER_SETTLE_MS;
        *source = WAIT_POINTER;
    }
}

/* The settings come to the front: the tab that shows them, or a new one after this one. */
static void open_settings(void) {
    for (int i = 0; i < ntabs; i++) if (tabs[i]->settings) { tab_select(tabs[i]); return; }
    struct tab *t = tab_new(front);
    if (!t) { console_add("error", "Tab allocation failed"); return; }
    tab_select(t);
    tab = t;
    navigate(SETTINGS_URL, NAV_PUSH);
}

/* A command that a native wait leaves for after it: one that needs the document or touches
   another tab. */
static bool command_waits(uint32_t cmd) {
    switch (cmd) {
    case CMD_FIND: case CMD_FIND_NEXT: case CMD_EVAL: case CMD_FIND_OPEN: case CMD_DEVTOOLS:
    case CMD_NEW_TAB: case CMD_CLOSE_TAB: case CMD_SELECT_TAB: case CMD_MOVE_TAB: case CMD_SETTINGS:
    case CMD_RELOAD_TAB: case CMD_CLOSE_OTHER_TABS: case CMD_POPUP_OPEN: case CMD_AUTOMATION:
        return true;
    }
    return false;
}

/* What the widgets around the page asked for, in its turn with the rest of the input. */
static void command(const struct gui_event *e) {
    switch (e->key) {
    case CMD_BACK: go_history(-1, true); break;
    case CMD_FORWARD: go_history(1, true); break;
    case CMD_RELOAD:
        if (tab->loading && !tab->native_wait) stop_navigation();
        else if (cur_url[0]) navigate(cur_url, NAV_RELOAD);
        break;
    case CMD_HOME: navigate(HOME, NAV_PUSH); break;
    case CMD_SETTINGS: open_settings(); break;
    case CMD_SETTINGS_DO:
        if (tab->settings) settings_do(tab->settings, (struct settings_action){(uint32_t)e->x, (uint32_t)e->y}, e->pressed);
        show_chrome();
        break;
    case CMD_NEW_TAB: new_tab(); break;
    case CMD_CLOSE_TAB: if (tab_by_id((uint32_t)e->x)) tab_close(tab_by_id((uint32_t)e->x)); break;
    case CMD_SELECT_TAB: tab_select(tab_by_id((uint32_t)e->x)); break;
    case CMD_MOVE_TAB: tab_move(tab_by_id((uint32_t)e->x), e->y); show_chrome(); break;
    case CMD_NEW_WINDOW: new_window(NULL); break;
    case CMD_DEVTOOLS: devtools_toggle(); break;
    case CMD_FIND_OPEN: find_bar_open(); break;
    case CMD_QUIT: quit = true; break;
    case CMD_POPUP_OPEN: {
        char *url = tab->popup_url;
        tab->popup_url = NULL; tab->popups = 0;
        if (url) { struct navigation_request r = browser_navigation(url, NAV_PUSH); tab_open(&r, tab, true); }
        free(url);
        show_chrome();
        break;
    }
    case CMD_POPUP_DISMISS: tab->popups = 0; free(tab->popup_url); tab->popup_url = NULL; show_chrome(); break;
    case CMD_AUTOMATION:
        if (e->y) { tab->focus = F_PAGE; show_chrome(); }
        else automation_handled = (unsigned)e->x;
        break;
    case CMD_RELOAD_TAB: {
        struct tab *t = tab_by_id((uint32_t)e->x);
        if (!t) break;
        SERVE(t);
        if (cur_url[0]) navigate(cur_url, NAV_RELOAD);
        break;
    }
    case CMD_CLOSE_OTHER_TABS: {
        struct tab *keep = tab_by_id((uint32_t)e->x);
        if (!keep) break;
        tab_select(keep);
        for (int i = ntabs - 1; i >= 0; i--) if (tabs[i] != keep) tab_close(tabs[i]);
        break;
    }
    case CMD_GO: tab->focus = F_PAGE; tab->sel_node = NULL; go_addr(addr); break;
    case CMD_FIND: find_next(tab->find_y >= 0 ? tab->find_y : tab->scroll_y); break;
    case CMD_FIND_NEXT: if (find_buf[0]) find_next(tab->find_y >= 0 ? tab->find_y + 1 : tab->scroll_y); break;
    case CMD_EVAL: console_eval(); break;
    case CMD_FOCUS: /* x: the field; y: it got the keyboard (1) or lost it; pressed: Escape gave it up */
        if (e->y) { tab->focus = focus_shown = e->x; tab->sel_node = NULL; }
        else if (tab->focus == e->x) {
            tab->focus = focus_shown = F_PAGE;
            if (e->x == F_FIND && e->pressed) find_close();
        }
        break;
    }
}

static void handle(const struct gui_event *e) {
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_DOWN || e->type == EV_MOUSE_UP || e->type == EV_WHEEL) pointer.input = true;
    bool console_only=tab->console_open && !dt_resizing &&
        ((e->type==EV_KEY && tab->focus==F_CONSOLE) ||
         (e->type==EV_WHEEL && e->y>=page_h() && e->y<view.h));
    switch (e->type) {
    case EV_CLOSE: quit = true; return;
    case EV_COMMAND:
        command(e);
        if (e->key == CMD_AUTOMATION) return; /* only the automation's own state */
        /* the keyboard moving between fields changes only the widgets */
        if (e->key == CMD_FOCUS && !(e->x == F_FIND && e->pressed) && !tab->need_layout && !tab->need_paint) { show_chrome(); return; }
        break;
    case EV_UNFOCUS: web_pointer_cancel(tab->doc);web_active_release(tab->doc);tab->pressed_node=NULL;break;
    case EV_DIRECT: direct_shown = e->key; tab->need_paint = true; break; /* key 0: the window shows again */
    case EV_KEY:
        if (!e->pressed) {
            if (tab->focus == F_PAGE) dispatch_native("keyup", tab->doc ? web_focused(tab->doc) : NULL, e, false);
            flush_dom_layout();
            if (tab->need_paint && !quit) redraw();
            return;
        }
        key(e);
        break;
    case EV_MOUSE_DOWN: mouse_down(e); break;
    case EV_MOUSE_UP:
        /* A deferred release may follow a deferred down that was not active
           yet at native dequeue. Clear again before any author mouseup. */
        if(!(e->buttons&1))web_active_release(tab->doc);
        else break; /* another button's release does not end the primary press */
        if (drag_sb) { drag_sb = false; tab->pressed_node = NULL; break; }
        if (dt_resizing || dt_swallow_up) { dt_resizing = dt_swallow_up = false; tab->pressed_node = NULL; break; }
        if (tab->doc) {
            web_node *capture=native_capture_target(e);
            bool inside=e->y >= 0 && e->y<page_h() && e->x>=0 && e->x<page_w();
            web_node *target=capture?capture:inside?web_node_at(tab->doc,document_x(e->x),e->y+tab->scroll_y):NULL;
            web_node *down = tab->pressed_node;
            dispatch_native("mouseup", target, e, false);
            web_node *captured_click=web_pointer_capture_click_target(tab->doc,target);
            web_node *click_target = tab->pressed_node ? captured_click?captured_click:web_pointer_click_target(tab->doc,down,target) : NULL;
            if(debug_js)fprintf(stderr,"[web-input:click] down=%p up=%p target=%p same=%d\n",
                                (void *)down,(void *)target,(void *)click_target,down==target);
            if (click_target) page_click(click_target, e->x, e->y,false,e->mods);
        }
        tab->pressed_node = NULL;
        break;
    case EV_MOUSE_MOVE:
        if(!(e->buttons&1)){web_active_release(tab->doc);tab->pressed_node=NULL;}
        if(!devtools_pointer_move(e->x,e->y))mouse_move(e);
        flush_dom_layout();
        if (tab->need_paint && !quit) redraw();
        else if (!quit) redraw_console();
        return;
    case EV_WHEEL:
        if (tab->console_open && e->y >= page_h() && e->y < view.h) {
            devtools_wheel(e->x, e->y, e->wheel);
        } else if (tab->sel_node) {
            int room = MAX((view.h - tab->sel_y) / SEL_H, 1);
            tab->sel_top = MAX(0, MIN(tab->sel_n - room, tab->sel_top + e->wheel));
        } else {
            web_node *target = tab->doc && e->y >= 0 && e->y < page_h() ?
                               web_node_at(tab->doc, document_x(e->x), e->y + tab->scroll_y) : NULL;
            if (dispatch_native("wheel", target, e, true)) {
                if (e->mods & NMOD_SHIFT) scroll_input(target,e->wheel*LINE,0,0);
                else scroll_input(target,0,e->wheel*LINE,0);
            }
        }
        break;
    case EV_RESIZE:
        /* The new viewport size reaches the page at its next rendering update, whose resize
           steps fire resize at the Window and its VisualViewport (js.c run_resize_steps). */
        tab->need_layout = true;
        flush_dom_layout();
        break;
    default: return;
    }
    flush_dom_layout();
    if(console_only && !tab->need_layout && !tab->need_paint &&
       (e->type==EV_WHEEL || tab->focus==F_CONSOLE)) {
        if(!quit)redraw_console();
        return;
    }
    if (tab->need_layout) relayout();
    if (!quit) redraw();
}

/* This pump intentionally does not dispatch DOM events, run JS, or destroy a document.
   QuickJS's synchronous module loader can be on the C stack while it is used. */
static void handle_native_wait(const struct gui_event *e) {
    if (e->type == EV_CLOSE) { quit = true; tab->sync_stopped = true; return; }
    if (tab != front) {
        /* A tab behind the front one waits: the front tab's input waits its turn. */
        struct tab *waiting = tab; tab = front;
        bool kept = defer_native_event(e);
        tab = waiting;
        if (!kept || (e->type == EV_COMMAND && e->key == CMD_CLOSE_TAB && tab_by_id((uint32_t)e->x) == waiting))
            tab->sync_stopped = true;
        return;
    }
    if (e->type == EV_DIRECT) {
        direct_shown = e->key;
        if (!direct_shown) { tab->need_paint = true; if (tab->native_wait_snapshot) redraw(); }
        return;
    }
    if (e->type == EV_RESIZE) {
        prepare_native_snapshot(true);tab->native_wait_snapshot=web_native_input_ready(tab->doc,true);
        if(tab->native_wait_snapshot)redraw();return;
    }
    if (e->type == EV_KEY && e->pressed) {
        uint32_t k = e->key;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (e->key == NKEY_ESC) { if(browser_fullscreen)web_presentation_exit(fullscreen_doc,1);else tab->sync_stopped = true; return; }
        if (e->key == NKEY_F12) {
            tab->console_open = !tab->console_open;
            if (tab->console_open) tab->focus = F_CONSOLE; else if (tab->focus == F_CONSOLE) tab->focus = F_PAGE;
            prepare_native_snapshot(true);tab->native_wait_snapshot=web_native_input_ready(tab->doc,true);
            if(tab->native_wait_snapshot)redraw();
            else if(tab->console_open&&view.px){draw_console();ui_surface_changed_rect(ui,VIEW,0,page_h(),view.w,console_h());}
            show_chrome();
            return;
        }
        if ((e->mods & NMOD_CTRL) && k == 'l') { addr_focus(); show_chrome(); return; }
        if (e->key == NKEY_F5 || ((e->mods & NMOD_CTRL) && k == 'r')) {
            struct navigation_request r = browser_navigation(cur_url, NAV_RELOAD);
            queue_navigation(&r); return;
        }
        if (e->mods & NMOD_ALT) {
            if (e->key == NKEY_LEFT) { go_history(-1, true); return; }
            if (e->key == NKEY_RIGHT) { go_history(1, true); return; }
        }
    }
    /* The toolbar's commands act now; what needs the document (find, the console) or another
       tab waits, and closing this tab ends the wait first. */
    if (e->type == EV_COMMAND && e->key == CMD_CLOSE_TAB && tab_by_id((uint32_t)e->x) == tab) tab->sync_stopped = true;
    if (e->type == EV_COMMAND && !command_waits(e->key) &&
        !(e->key == CMD_FOCUS && e->x == F_FIND && e->pressed)) {
        command(e); show_chrome(); return;
    }
    if (e->type == EV_MOUSE_DOWN && e->x >= view.w - SCRW) {
        mouse_down(e); redraw(); return;
    }
    if (e->type == EV_MOUSE_MOVE && drag_sb) { scrollbar_drag(e->y); redraw(); return; }
    if (e->type == EV_MOUSE_UP && drag_sb) { drag_sb = false; redraw(); return; }
    if(e->type==EV_WHEEL && tab->console_open && e->y>=page_h() && e->y<view.h) {
        devtools_wheel(e->x,e->y,e->wheel);redraw_console();return;
    }
    if(!defer_native_event(e)){tab->sync_stopped=true;return;}
    native_scroll_queue();
}

static void handle_deferred_input(void) {
    struct deferred_input entry=tab->evq[tab->evq_first++];
    if(!--tab->evq_n){tab->evq_first=tab->evq_scroll_at=0;tab->evq_failure_logged=false;tab->evq_hover_tail_valid=false;}
    if(entry.event.type==EV_WHEEL && entry.document.document &&
       (entry.generation!=tab->generation || !web_input_target_valid(tab->doc,&entry.document)))return;
    if(!entry.scrolled){handle(&entry.event);return;}
    if(!web_input_target_valid(tab->doc,&entry.target))return;
    /* The absence of non-passive listeners was established at native scroll
       commit. Later listeners cannot retroactively cancel that physical input.
       Notification still runs once, on the original surviving target. */
    dispatch_native("wheel",entry.target.node,&entry.event,false);
    if(tab->need_layout)relayout();
    if(tab->need_paint && !quit)redraw();
}

static void document_step(uint64_t now) {
    if (!tab->doc || quit) { web_media_background(now); return; }
    if (tab->scroll_event_pending && !tab->native_wait && !web_script_running(tab->doc)) {
        tab->scroll_event_pending = false;
        web_document_scroll(tab->doc);
    }
    uint64_t tick_start = debug_js ? n_clock_uptime_ms() : 0;
    web_tick(tab->doc, now);
    if (debug_js) {
        uint64_t elapsed = n_clock_uptime_ms() - tick_start;
        loop_profile.tick_ms += elapsed;
        loop_profile.tick_max_ms = MAX(loop_profile.tick_max_ms, elapsed);
        loop_profile.ticks++;
    }
    if (quit) return;
    /* A tab behind the front one is hidden: it has no rendering opportunity, and lays its
       document out and paints it when it comes to the front. Its title is the strip's. */
    if (tab != front) set_title();
    /* The rendering waits for the frame that shows the last picture: the tasks before it change
       the page, the frame lays it out and paints it once. */
    else if (!picture_pending) {
        bool restyle_only = web_restyle_only(tab->doc) && !tab->need_layout;
        bool dirty = web_dirty(tab->doc);
        bool media_paint = web_paint_dirty(tab->doc);
        if (restyle_only) {
            /* Animation samples: the layout bounds what they repaint, often nothing on screen. */
            relayout();
            redraw_damage();
        } else if (dirty || tab->need_layout) {
            relayout();
            set_title();
            redraw();
        } else if (tab->need_paint) redraw();
        else if(media_paint)redraw_damage();
    }
    if (tab == front) { redraw_console(); flush_dom_layout(); }
    if (!tab->refresh_seen && !tab->loading) {
        int delay;
        const char *url = web_refresh_url(tab->doc, &delay);
        if (url) {
            tab->refresh_seen = true;
            if (delay <= 10 && strcmp(url, cur_url)) {
                if (url_text_set(&tab->refresh_text, url)) tab->refresh_at = n_clock_uptime_ms() + (uint64_t)MAX(delay, 0) * 1000 + 1;
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
    unsigned partial_empty=loop_profile.partial_empty,partial_whole=loop_profile.partial_whole;
    unsigned consoles=loop_profile.console_draws;
    uint64_t console_ms=loop_profile.console_ms,console_update=loop_profile.console_update_ms;
    uint64_t partial_ms=loop_profile.partial_ms,partial_update=loop_profile.partial_update_ms,video_ms=loop_profile.video_present_ms;
    uint64_t partial_pixels=loop_profile.partial_pixels;
    unsigned waits[7],engine_waits=loop_profile.engine_waits,woken=loop_profile.woken;memcpy(waits,loop_profile.waits,sizeof waits);
    memset(&loop_profile, 0, sizeof loop_profile);
    loop_profile.since = now;
    host_console(NULL, WEB_CONSOLE_TRACE, message);
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
        host_console(NULL, WEB_CONSOLE_TRACE, message);
        if (web_memory_report(tab->doc, message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    }
    snprintf(message, sizeof message,
             "Browser wait profile: timeout from none %u, network %u, storage %u, document %u, now %u, refresh %u, pointer %u; shortened by the window engine %u; ended by an event %u",
             waits[WAIT_NONE], waits[WAIT_NET], waits[WAIT_STORAGE], waits[WAIT_DOCUMENT], waits[WAIT_NOW], waits[WAIT_REFRESH], waits[WAIT_POINTER],
             engine_waits, woken);
    host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_wait_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_paint_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_style_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_box_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_tick_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if (web_js_deadline_report(message, sizeof message) > 0) host_console(NULL, WEB_CONSOLE_TRACE, message);
    if(consoles){snprintf(message,sizeof message,
        "Browser console profile: rect draw %u/%lu ms, rect present %lu ms (no author page paint)",
        consoles,(unsigned long)console_ms,(unsigned long)console_update);host_console(NULL, WEB_CONSOLE_TRACE,message);}
    if(partial||patches||partial_empty||partial_whole){snprintf(message,sizeof message,
        "Browser damage profile: partial draw %u/%lu ms over %lu kpx, partial present %lu ms, off the page %u, drawn whole %u, video patch %u/%lu ms (banded compose into the view and present)",
        partial,(unsigned long)partial_ms,(unsigned long)(partial_pixels/1000),(unsigned long)partial_update,partial_empty,partial_whole,patches,(unsigned long)video_ms);host_console(NULL, WEB_CONSOLE_TRACE,message);}
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
        host_console(NULL, WEB_CONSOLE_TRACE,message);
    }
}

/* ---------------------------------------------------------------- tabs */
/* A new tab, empty until it navigates: at the end of the strip, or after the tab it was
   opened from. */
static struct tab *tab_new(struct tab *after) {
    if (ntabs == tabs_capacity) {
        int capacity = tabs_capacity ? (tabs_capacity > INT_MAX / 2 ? INT_MAX : tabs_capacity * 2) : 8;
        void *grown = ntabs < INT_MAX && (size_t)capacity <= SIZE_MAX / sizeof *tabs ?
                      realloc(tabs, (size_t)capacity * sizeof *tabs) : NULL;
        if (!grown) return NULL;
        tabs = grown; tabs_capacity = capacity;
    }
    struct tab *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->dt = devtools_new();
    t->console_input = calloc(1, 1);
    if (!t->dt || !t->console_input) { devtools_free(t->dt); free(t->console_input); free(t); return NULL; }
    t->console_input_capacity = 1;
    t->generation = ++next_generation;
    t->id = ++tab_serial;
    t->hpos = -1; t->find_y = -1; t->focus = F_PAGE;
    int at = ntabs;
    for (int i = 0; after && i < ntabs; i++) if (tabs[i] == after) at = i + 1;
    memmove(tabs + at + 1, tabs + at, (size_t)(ntabs - at) * sizeof *tabs);
    tabs[at] = t; ntabs++;
    return t;
}

/* Everything the tab being served holds is freed: its navigation and its document end, its
   requests with it except a keepalive one, which goes on without an owner. */
static void tab_free(void) {
    struct tab *t = tab;
    stop_navigation();
    cancel_document_requests();
    for (struct transfer *x = transfers; x; x = x->next) if (x->owner == t) x->owner = NULL;
    if (t->doc) {
        if (pip_doc && web_presentation_root(pip_doc) == t->doc) web_presentation_exit(pip_doc, 2);
        if (fullscreen_doc && web_presentation_root(fullscreen_doc) == t->doc) web_presentation_exit(fullscreen_doc, 1);
        web_free(t->doc);
    }
    pending_free(&t->pending); document_state_free(&t->navigation_state);
    free(t->current_text.p); free(t->navigation_text.p); free(t->pending_text.p); free(t->address_text.p); free(t->refresh_text.p);
    free(t->focus_value); free(t->find_text.p);
    for (int i = 0; i < t->nhist; i++) hist_discard(i);
    free(t->hist); free(t->console_page.lines); free(t->console_trace.lines); free(t->sel_labels); free(t->evq); free(t->console_input);
    while (t->history_queue) { struct browser_history_task *next = t->history_queue->next; free(t->history_queue); t->history_queue = next; }
    devtools_free(t->dt);
    settings_free(t->settings);
    if (t->storage_session) webstorage_session_close(storage, t->storage_session);
    free(t->name);
    free(t->popup_url);
    free(t);
}

/* The window shows another tab. The one it showed lets go of the pointer and its popup; its
   document learns it is hidden, and the new one that it is visible, between script tasks
   (the main loop's window state). */
static void tab_select(struct tab *t) {
    if (!t || t == front) return;
    if (front) {
        struct tab *old = front;
        if (browser_fullscreen && fullscreen_doc) web_presentation_exit(fullscreen_doc, 1);
        if (old->doc && !old->native_wait && !web_script_running(old->doc)) {
            web_pointer_cancel(old->doc); web_active_release(old->doc);
        }
        old->sel_node = old->pressed_node = NULL;
        drag_sb = false;
    }
    front = t;
    t->need_layout = t->need_paint = true;
    t->console_dirty = true;
    SERVE(t);
    set_title();
    if (!t->native_wait && !(t->doc && web_script_running(t->doc))) { relayout(); redraw(); }
    else show_chrome();
}

/* Closing the last tab closes the window. Another one comes to the front: the one after it,
   or the one before. */
static void tab_close(struct tab *t) {
    if (ntabs <= 1) { quit = true; return; }
    int at = 0;
    while (at < ntabs && tabs[at] != t) at++;
    if (at == ntabs || t->native_wait || (t->doc && web_script_running(t->doc))) return;
    if (t == front) tab_select(tabs[at + 1 < ntabs ? at + 1 : at - 1]);
    if (front_wanted == t) front_wanted = NULL;
    struct tab *served = tab;
    tab = t;
    tab_free();
    memmove(tabs + at, tabs + at + 1, (size_t)(ntabs - at - 1) * sizeof *tabs);
    ntabs--;
    tab = served == t ? front : served;
    show_chrome();
}

/* A tab dragged along the strip goes to another place in it. */
static void tab_move(struct tab *t, int to) {
    int at = tab_index(t);
    if (at < 0 || to < 0) return;
    if (to >= ntabs) to = ntabs - 1;
    if (to > at) memmove(tabs + at, tabs + at + 1, (size_t)(to - at) * sizeof *tabs);
    else memmove(tabs + to + 1, tabs + to, (size_t)(at - to) * sizeof *tabs);
    tabs[to] = t;
}

/* A tab that opens at the front or behind it, and goes where it was asked. A page's link or
   window.open passes the page as its source; the browser's own commands pass none. */
static struct tab *tab_open(const struct navigation_request *r, struct tab *after, bool foreground) {
    struct tab *t = tab_new(after);
    if (!t) { console_add("error", "Tab allocation failed"); return NULL; }
    SERVE(t);
    queue_navigation(r);
    if (foreground) front_wanted = t;
    return t;
}

int main(int argc, char **argv) {
    bool start_maximized = false;
    if (!(tab = front = tab_new(NULL))) return 1;
    while (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--debug-js")) { debug_js = net_log = log_console = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--net-log")) { net_log = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--net-body") && argc > 2) { net_log = true; net_body = argv[2]; argc -= 2; argv += 2; }
        else if (!strcmp(argv[1], "--log-console")) { log_console = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--maximized")) { start_maximized = true; argc--; argv++; }
        /* A test tool, never a product setting: WPT's runner drives the input (see automation_run). */
        else if (!strcmp(argv[1], "--wpt-automation")) { automation_enabled = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--console-file-each-document")) { console_each_document = true; argc--; argv++; }
        else if (!strcmp(argv[1], "--js-console")) {
            tab->console_open = true; tab->focus = F_CONSOLE; argc--; argv++;
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
            tab->console_open=true;tab->focus=F_CONSOLE;
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
    web_js_prepare(); /* while the window opens and the first page is fetched */
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
    /* What another window clears reaches this one's sessionStorage, before any document runs. */
    if (storage && web_wait_queue() >= 0 && webstorage_watch(storage, web_wait_queue()) < 0)
        console_add("warn", "Storage clears from other windows will not be seen by this one.");
    /* The first window of the browser's session clears what the last one left to clear. */
    websitedata_session_begin(storage);
    show_chrome();
    ui_session_pump(ui); /* the surface gets its size */
    take_engine_events();
    redraw();
    if (argc > 1) go_addr(argv[1]); /* a URL, a path (as Files opens one) or words to search */
    else navigate(HOME, NAV_PUSH);
    while (!quit) {
        uint64_t now = n_clock_uptime_ms();
        tab = front;
        webnet_pump(network, now); /* each answer goes to the tab it is for */
        if (debug_js) loop_profile.net_ms += n_clock_uptime_ms() - now;
        for (int i = 0; i < ntabs && !quit; i++) {
            tab = tabs[i];
            apply_pending_history();
            apply_pending_navigation();
            finish_navigation();
        }
        tab = front;
        if (front_wanted) { tab_select(front_wanted); front_wanted = NULL; tab = front; }
        /* Give input already collected by a long task its turn before starting
           the next timer/resource task, rather than repeatedly starving it. */
        if(tab->evq_n){
            /* A long C mutation can leave every collected wheel uncommitted.
               Publish its completed layout once, then batch passive defaults;
               do not rasterize/replay one default for each queued packet. */
            flush_dom_layout();native_scroll_queue();handle_deferred_input();continue;
        }
        /* Native window state is sampled only between script tasks. Queries
           remain live during scripts, but visibility/focus callbacks cannot
           re-enter a module loader or a page callback on the C stack. */
        for (int i = 0; i < ntabs && !quit; i++) {
            tab = tabs[i];
            if(tab->native_wait || web_script_running(tab->doc))continue;
            unsigned state=host_window_state(NULL),changed=state^tab->window_previous_state;
            if(tab->window_state_known && tab->doc) {
                if(changed&WEB_WINDOW_VISIBLE)web_visibility_event(tab->doc);
                if(changed&WEB_WINDOW_FOCUSED) {
                    struct web_event event={.type=(state&WEB_WINDOW_FOCUSED)?"focus":"blur"};
                    web_dispatch(tab->doc,NULL,&event);flush_dom_layout();
                }
            }
            tab->window_previous_state=state;tab->window_state_known=true;
        }
        presentation_service();
        for (int i = 0; i < ntabs && !quit; i++) { tab = tabs[i]; document_step(now); }
        tab = front;
        report_loop_profile(n_clock_uptime_ms());
        if(console_startup_pending()&&tab->doc&&!tab->loading&&!tab->native_wait&&now>=console_due&&!web_script_running(tab->doc)){
            tab->console_open=true;tab->focus=F_CONSOLE;
            console_startup_generation=tab->generation;
            if(!web_console_eval(tab->doc,console_startup,console_startup_length))console_add("error","Startup console command failed.");
            if(!console_each_document){free(console_startup);console_startup=NULL;console_startup_length=0;}
            tab->need_layout=true;prepare_native_snapshot(false);redraw();
        }
        for (int i = 0; i < ntabs && !quit; i++) {
            tab = tabs[i];
            apply_pending_history();
            apply_pending_navigation();
        }
        tab = front;
        if (quit) break;
        now = n_clock_uptime_ms();
        struct gui_event e;
        if (tab->evq_n) {
            flush_dom_layout();native_scroll_queue();handle_deferred_input();
            continue;
        }
        int timeout = webnet_timeout(network, now), source = timeout < 0 ? WAIT_NONE : WAIT_NET;
        if (automation_runs) {
            int due = automation_service(now);
            if (due >= 0 && (timeout < 0 || due < timeout)) { timeout = due; source = WAIT_NOW; }
        }
        if (webstorage_pending(storage)) {
            struct web_storage_result result;
            if (webstorage_flush(storage, false, &result) != WEB_STORAGE_OK)
                console_add("error", "Local storage flush failed; retrying later.");
            if (webstorage_pending(storage) && (timeout < 0 || timeout > 100)) { timeout = 100; source = WAIT_STORAGE; }
        }
        if (tab->doc && w && !frame_asked && web_rendering_wanted(tab->doc))
            frame_asked = ui_window_frame(ui, WINDOW) == 0;
        pointer_service(now, &timeout, &source);
        if(console_startup_pending()&&tab->doc&&!tab->loading){int due=now>=console_due?0:(int)MIN(console_due-now,0x7fffffff);
            if(timeout<0||due<timeout){timeout=due;source=WAIT_NOW;}}
        bool refreshed = false;
        for (int i = 0; i < ntabs && !refreshed; i++) {
            tab = tabs[i];
            int64_t deadline = web_deadline(tab->doc); /* retired media children also wake a document-less window */
            if (deadline >= 0) {
                int due = deadline <= (int64_t)now ? 0 : (int)MIN(deadline - (int64_t)now, 0x7fffffff);
                if (timeout < 0 || due < timeout) { timeout = due; source = WAIT_DOCUMENT; }
            }
            if (tab->navigation_ready || tab->pending_navigation || tab->scroll_event_pending || (tab->history_queue_count && !tab->loading)) { timeout = 0; source = WAIT_NOW; }
            if (tab->refresh_at) {
                uint64_t now = n_clock_uptime_ms();
                if (now >= tab->refresh_at) {
                    tab->refresh_at = 0;
                    struct navigation_request r = page_navigation(refresh_url, NAV_PUSH, NULL);
                    navigate_request(&r);
                    refreshed = true;
                    continue;
                }
                int due = (int)MIN(tab->refresh_at - now, 0x7fffffff);
                if (timeout < 0 || due < timeout) { timeout = due; source = WAIT_REFRESH; }
            }
        }
        tab = front;
        if (refreshed || front_wanted) continue;
        uint64_t wait_start = debug_js ? n_clock_uptime_ms() : 0;
        int r = browser_event(&e, timeout);
        if (debug_js) { loop_profile.wait_ms += n_clock_uptime_ms() - wait_start; loop_profile.waits[source]++; loop_profile.woken += r > 0; }
        if (r < 0) break;
        if (r > 0) {
            handle(&e);
            continue;
        }
    }
    quit = true; /* every request ends with its window, keepalive ones too */
    for (int i = 0; i < ntabs; i++) { tab = tabs[i]; tab_free(); }
    free(tabs); tabs = NULL; ntabs = 0; tab = front = NULL;
    /* The last window of the session clears the site data the user's choices do not keep. */
    websitedata_session_end(storage);
    web_media_background(n_clock_uptime_ms()); /* cancel already killed children; never block the closing GUI */
    webnet_free(network);
    webstorage_free(storage);
    parallel_shutdown();
    free(console_startup);
    ui_session_free(ui); free(chrome_sent); free(inbox);
    for (int i = 0; i < 32; i++) chrome_release(&chrome[i]);
    return 0;
}
