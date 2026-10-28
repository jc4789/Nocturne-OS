/* Browser DevTools: the panel F12 docks under the page, with Elements (DOM
   tree, Styles, Computed, Layout), Console and Network tabs. Included once by
   browser.c.

   The tree, the side pane and the highlight boxes are cached while the
   document is consistent (after a layout, or on input routed by the main
   loop). A long author task's checkpoint and a synchronous module load may
   draw the panel and scroll it, but only from these caches: nothing there
   reads the DOM, the cascade or layout. Node handles stay valid for their
   document's lifetime; a document change resets every cached handle. */
#include "web_inspect.h"

enum { DT_ELEMENTS, DT_CONSOLE, DT_NETWORK, DT_TRACE, DT_NTABS };
enum { DT_STYLES, DT_COMPUTED, DT_LAYOUT, DT_NSUB };
#define DT_TABH 24
#define DT_ROWH 16
#define DT_SUBH 22
#define DT_NET_HEAD 20
#define DT_BG     RGB(32, 33, 36)
#define DT_BAR    RGB(41, 42, 45)
#define DT_RULE   RGB(60, 64, 67)
#define DT_SELECT RGB(38, 65, 105)
#define DT_HOVER  RGB(50, 54, 60)
#define DT_BLUE   RGB(138, 180, 248)

/* Colour switches inside cached line text (bytes below 0x20 never survive
   sanitising, so they cannot collide with page text). */
enum { C_TAG = 1, C_ATTR, C_VALUE, C_TEXT, C_DIM, C_PROP, C_ACCENT, C_ERROR, C_COUNT };
static const uint32_t dt_palette[C_COUNT] = {
    0, RGB(93, 176, 215), RGB(155, 187, 220), RGB(242, 151, 102), RGB(232, 234, 237),
    RGB(137, 137, 137), RGB(92, 213, 251), RGB(253, 214, 99), RGB(242, 139, 130)
};

/* The panel's divider and a press it took, the window's like the panel's height. */
static bool dt_swallow_up, dt_resizing;

/* ---------------------------------------------------------------- lines */
enum { DT_EXPANDABLE = 1, DT_OPEN = 2, DT_CLOSING = 4, DT_STRIKE = 8, DT_HEADER = 16 };
struct dt_line { web_node *node; char *text, *right; int depth; unsigned flags; };
struct dt_lines { struct dt_line *v; int n, capacity, top; };

struct dt_request {
    uint64_t serial, generation, resource, start, end;
    char *method, *url, *request_headers, *response_headers, *error, *mime;
    char *sent_headers; /* the fields the final hop sent (webnet's report_sent) */
    size_t size;
    int kind, status;
    bool navigation, done, streamed;
};

/* Each tab has DevTools of its own: the panel it shows, the cached tree and boxes of its
   document, and the requests it recorded. */
struct devtools {
    int panel, sub; /* DT_ELEMENTS..., DT_STYLES... */
    bool picking;
    struct dt_lines tree, side, detail;
    /* tree state */
    web_doc *doc;
    uint64_t generation, tree_at, side_at;
    web_node *selected, *hovered;
    bool auto_open;
    struct web_inspect_box overlay, sel_box;
    bool overlay_valid, sel_valid;
    char overlay_label[160];
    uintptr_t *open;              /* sorted expanded-node set */
    size_t open_n, open_capacity;
    /* network */
    struct dt_request *net;
    int net_n, net_capacity, net_top, net_selected;
    uint64_t net_serial, nav_serial;
};

static void dt_lines_clear(struct dt_lines *l) {
    for (int i = 0; i < l->n; i++) { free(l->v[i].text); free(l->v[i].right); }
    l->n = 0;
}
/* Takes the built text; the builder starts empty again either way. */
static void dt_line_add(struct dt_lines *l, web_node *node, int depth, unsigned flags, struct sb *text, const char *right) {
    char *owned = text->b ? text->b : strdup("");
    *text = (struct sb){0};
    if (!owned) return;
    if (l->n == l->capacity) {
        int capacity = l->capacity ? l->capacity * 2 : 128;
        void *grown = realloc(l->v, (size_t)capacity * sizeof *l->v);
        if (!grown) { free(owned); return; }
        l->v = grown; l->capacity = capacity;
    }
    l->v[l->n++] = (struct dt_line){node, owned, right && *right ? strdup(right) : NULL, depth, flags};
}
static void dt_put(struct sb *s, int color, const char *p, size_t n) {
    char marker = (char)color;
    sb_add(s, &marker, 1);
    size_t start = 0;
    for (size_t i = 0; i <= n; i++)
        if (i == n || (unsigned char)p[i] < 0x20) {
            if (i > start) sb_add(s, p + start, i - start);
            if (i < n) sb_add(s, " ", 1);
            start = i + 1;
        }
}
static void dt_puts(struct sb *s, int color, const char *p) { dt_put(s, color, p, strlen(p)); }
/* Long values end with an ellipsis at a UTF-8 boundary. */
static void dt_put_limited(struct sb *s, int color, const char *p, size_t n, size_t limit) {
    if (n <= limit) { dt_put(s, color, p, n); return; }
    while (limit && ((unsigned char)p[limit] & 0xc0) == 0x80) limit--;
    dt_put(s, color, p, limit);
    sb_str(s, "\xe2\x80\xa6");
}
/* Draws marked text from x; returns the end x. Stops at the clip's right edge. */
static int dt_draw_text(canvas_t *c, int x, int y, const char *p, uint32_t fg) {
    char chunk[256];
    while (*p && x < c->cx1) {
        if ((unsigned char)*p < 0x20) {
            if ((unsigned char)*p < C_COUNT) fg = dt_palette[(unsigned char)*p];
            p++; continue;
        }
        size_t n = 0;
        while (p[n] && (unsigned char)p[n] >= 0x20 && n < sizeof chunk - 1) n++;
        if (p[n] && (unsigned char)p[n] >= 0x20)
            while (n > 1 && ((unsigned char)p[n] & 0xc0) == 0x80) n--;
        memcpy(chunk, p, n); chunk[n] = 0;
        x += gfx_text(c, x, y, chunk, fg, TRANSPARENT, FONT_SMALL);
        p += n;
    }
    return x;
}
static int dt_rows_in(int height) { return MAX(1, height / DT_ROWH); }
static void dt_clamp_top(struct dt_lines *l, int rows) { l->top = MAX(0, MIN(l->top, l->n - rows)); }
static void dt_draw_lines(canvas_t *c, struct dt_lines *l, int x, int y, int width, int height, bool tree);

/* ---------------------------------------------------------------- tree state */

static size_t dt_open_slot(web_node *n, bool *found) {
    uintptr_t key = (uintptr_t)n;
    size_t lo = 0, hi = tab->dt->open_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tab->dt->open[mid] < key) lo = mid + 1; else hi = mid;
    }
    *found = lo < tab->dt->open_n && tab->dt->open[lo] == key;
    return lo;
}
static bool dt_is_open(web_node *n) { bool found; (void)dt_open_slot(n, &found); return found; }
static void dt_set_open(web_node *n, bool open) {
    bool found; size_t i = dt_open_slot(n, &found);
    if (open == found) return;
    if (!open) { memmove(tab->dt->open + i, tab->dt->open + i + 1, (tab->dt->open_n - i - 1) * sizeof *tab->dt->open); tab->dt->open_n--; return; }
    if (tab->dt->open_n == tab->dt->open_capacity) {
        size_t capacity = tab->dt->open_capacity ? tab->dt->open_capacity * 2 : 64;
        void *grown = realloc(tab->dt->open, capacity * sizeof *tab->dt->open);
        if (!grown) return;
        tab->dt->open = grown; tab->dt->open_capacity = capacity;
    }
    memmove(tab->dt->open + i + 1, tab->dt->open + i, (tab->dt->open_n - i) * sizeof *tab->dt->open);
    tab->dt->open[i] = (uintptr_t)n; tab->dt->open_n++;
}
static void dt_sync_doc(void) {
    if (tab->dt->doc == tab->doc && tab->dt->generation == tab->generation) return;
    tab->dt->doc = tab->doc; tab->dt->generation = tab->generation;
    tab->dt->selected = tab->dt->hovered = NULL;
    tab->dt->overlay_valid = tab->dt->sel_valid = false;
    tab->dt->open_n = 0; tab->dt->auto_open = true;
    dt_lines_clear(&tab->dt->tree); dt_lines_clear(&tab->dt->side);
    tab->dt->tree.top = tab->dt->side.top = 0;
    tab->dt->tree_at = tab->dt->side_at = 0;
}

static void dt_trimmed(web_node *n, const char **text, size_t *length) {
    const char *p = web_inspect_text(n, length);
    while (*length && (unsigned char)*p <= ' ') { p++; (*length)--; }
    while (*length && (unsigned char)p[*length - 1] <= ' ') (*length)--;
    *text = p;
}
static bool dt_void_element(const char *name) {
    static const char *const names[] = {"area","base","br","col","embed","hr","img","input","link","meta","source","track","wbr"};
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) if (!strcasecmp(name, names[i])) return true;
    return false;
}
static void dt_open_tag(struct sb *s, web_node *n) {
    dt_puts(s, C_TAG, "<"); dt_puts(s, C_TAG, web_inspect_name(n));
    for (int i = 0, count = web_inspect_attr_count(n); i < count; i++) {
        const char *name, *value;
        if (!web_inspect_attr(n, i, &name, &value)) continue;
        dt_puts(s, C_ATTR, " "); dt_puts(s, C_ATTR, name);
        dt_puts(s, C_ATTR, "=");
        dt_puts(s, C_VALUE, "\""); dt_put_limited(s, C_VALUE, value, strlen(value), 160); dt_puts(s, C_VALUE, "\"");
    }
    dt_puts(s, C_TAG, ">");
}
static void dt_close_tag(struct sb *s, web_node *n) {
    dt_puts(s, C_TAG, "</"); dt_puts(s, C_TAG, web_inspect_name(n)); dt_puts(s, C_TAG, ">");
}
/* One short text child is shown inline, as the tree does in Chrome. */
static bool dt_inline_text(web_node *first, const char **text, size_t *length) {
    if (!first || web_inspect_kind(first) != WEB_INSPECT_TEXT || web_inspect_next(first)) return false;
    dt_trimmed(first, text, length);
    return *length <= 80 && !memchr(*text, '\n', *length);
}
static void dt_add_node(web_node *n, int depth) {
    struct sb s = {0};
    const char *text; size_t length;
    switch (web_inspect_kind(n)) {
    case WEB_INSPECT_ELEMENT: {
        web_node *first = web_inspect_first(n);
        bool short_text = dt_inline_text(first, &text, &length);
        bool expandable = first && !short_text, open = expandable && dt_is_open(n);
        dt_open_tag(&s, n);
        if (short_text) { dt_put(&s, C_TEXT, text, length); dt_close_tag(&s, n); }
        else if (expandable && !open) { dt_puts(&s, C_DIM, "\xe2\x80\xa6"); dt_close_tag(&s, n); }
        else if (!first && !dt_void_element(web_inspect_name(n))) dt_close_tag(&s, n);
        dt_line_add(&tab->dt->tree, n, depth, (expandable ? DT_EXPANDABLE : 0) | (open ? DT_OPEN : 0), &s, NULL);
        if (!open) return;
        for (web_node *c = first; c; c = web_inspect_next(c)) dt_add_node(c, depth + 1);
        dt_close_tag(&s, n);
        dt_line_add(&tab->dt->tree, n, depth, DT_CLOSING, &s, NULL);
        return;
    }
    case WEB_INSPECT_TEXT:
        dt_trimmed(n, &text, &length);
        if (!length) return;
        dt_puts(&s, C_TEXT, "\""); dt_put_limited(&s, C_TEXT, text, length, 240); dt_puts(&s, C_TEXT, "\"");
        break;
    case WEB_INSPECT_COMMENT:
        text = web_inspect_text(n, &length);
        dt_puts(&s, C_DIM, "<!--"); dt_put_limited(&s, C_DIM, text, length, 240); dt_puts(&s, C_DIM, "-->");
        break;
    case WEB_INSPECT_DOCTYPE:
        dt_puts(&s, C_DIM, "<!DOCTYPE "); dt_puts(&s, C_DIM, web_inspect_name(n)); dt_puts(&s, C_DIM, ">");
        break;
    case WEB_INSPECT_SHADOW_ROOT: {
        web_node *first = web_inspect_first(n);
        bool open = first && dt_is_open(n);
        dt_puts(&s, C_DIM, "#shadow-root ("); dt_puts(&s, C_DIM, web_inspect_text(n, NULL)); dt_puts(&s, C_DIM, ")");
        dt_line_add(&tab->dt->tree, n, depth, (first ? DT_EXPANDABLE : 0) | (open ? DT_OPEN : 0), &s, NULL);
        if (open) for (web_node *c = first; c; c = web_inspect_next(c)) dt_add_node(c, depth + 1);
        return;
    }
    default: return;
    }
    dt_line_add(&tab->dt->tree, n, depth, 0, &s, NULL);
}
static web_node *dt_child_named(web_node *parent, const char *name) {
    for (web_node *c = web_inspect_first(parent); c; c = web_inspect_next(c))
        if (web_inspect_kind(c) == WEB_INSPECT_ELEMENT && !strcasecmp(web_inspect_name(c), name)) return c;
    return NULL;
}
static void dt_build_tree(void) {
    web_node *root = web_inspect_root(tab->doc);
    if (tab->dt->auto_open && root) {     /* <html> and <body> start expanded */
        web_node *html = dt_child_named(root, "html"), *body = html ? dt_child_named(html, "body") : NULL;
        if (html) dt_set_open(html, true);
        if (body) { dt_set_open(body, true); tab->dt->auto_open = false; }
    }
    dt_lines_clear(&tab->dt->tree);
    for (web_node *c = web_inspect_first(root); c; c = web_inspect_next(c)) dt_add_node(c, 0);
}
static int dt_row_of(web_node *n) {
    for (int i = 0; i < tab->dt->tree.n; i++) if (tab->dt->tree.v[i].node == n && !(tab->dt->tree.v[i].flags & DT_CLOSING)) return i;
    return -1;
}
static int dt_panel_rows(void) { return dt_rows_in(console_h() - DT_TABH); }
static void dt_scroll_to_row(struct dt_lines *l, int row, int rows) {
    if (row < 0) return;
    if (row < l->top) l->top = row;
    else if (row >= l->top + rows) l->top = row - rows + 1;
}

/* ---------------------------------------------------------------- side pane */
static void dt_node_label(struct sb *s, web_node *n) {
    dt_puts(s, C_TAG, web_inspect_name(n));
    for (int i = 0, count = web_inspect_attr_count(n); i < count; i++) {
        const char *name, *value;
        if (!web_inspect_attr(n, i, &name, &value)) continue;
        if (!strcasecmp(name, "id") && *value) { dt_puts(s, C_VALUE, "#"); dt_put_limited(s, C_VALUE, value, strlen(value), 60); }
        else if (!strcasecmp(name, "class")) {
            for (const char *p = value; *p;) {
                p += strspn(p, " \t\r\n\f");
                size_t k = strcspn(p, " \t\r\n\f");
                if (k) { dt_puts(s, C_ATTR, "."); dt_put_limited(s, C_ATTR, p, k, 40); }
                p += k;
            }
        }
    }
}
/* "styles.css" for a URL, the host for a directory URL, the text otherwise. */
static void dt_basename(const char *url, char *out, size_t capacity) {
    out[0] = 0;
    if (!url || !*url) return;
    const char *scheme = strstr(url, "://");
    if (!scheme) { strlcpy(out, url, capacity); return; }
    const char *host = scheme + 3;
    size_t end = strcspn(url, "?#"), host_end = (size_t)(host - url) + strcspn(host, "/?#");
    const char *slash = NULL;
    for (const char *p = url + host_end; p < url + end; p++) if (*p == '/') slash = p;
    if (slash && slash + 1 < url + end) { size_t n = (size_t)(url + end - slash - 1); snprintf(out, capacity, "%.*s", (int)n, slash + 1); }
    else snprintf(out, capacity, "%.*s", (int)(host_end - (size_t)(host - url)), host);
}
static void dt_refresh_side(void) {
    dt_lines_clear(&tab->dt->side);
    tab->dt->side_at = n_clock_uptime_ms();
    struct sb s = {0};
    if (!tab->dt->selected || web_inspect_kind(tab->dt->selected) != WEB_INSPECT_ELEMENT) {
        dt_puts(&s, C_DIM, tab->dt->selected ? "Styles apply to elements only." : "Select an element in the tree, or pick one with Ctrl+Shift+C.");
        dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL);
        return;
    }
    if (tab->dt->sub == DT_STYLES) {
        struct web_inspect_styles styles;
        if (!web_inspect_styles(tab->doc, tab->dt->selected, &styles)) {
            dt_puts(&s, C_ERROR, "Styles are unavailable (allocation failed).");
            dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL);
            return;
        }
        web_node *section = NULL;
        for (int i = 0; i < styles.count; i++) {
            const struct web_inspect_rule *r = &styles.rules[i];
            if (r->inherited_from && r->inherited_from != section) {
                section = r->inherited_from;
                dt_puts(&s, C_DIM, "Inherited from "); dt_node_label(&s, section);
                dt_line_add(&tab->dt->side, section, 0, DT_HEADER, &s, NULL);
            }
            char source[96];
            dt_basename(r->source, source, sizeof source);
            dt_puts(&s, r->ua ? C_DIM : C_TEXT, r->selector); dt_puts(&s, C_TEXT, " {");
            dt_line_add(&tab->dt->side, NULL, 0, 0, &s, source);
            for (int k = 0; k < r->count; k++) {
                const struct web_inspect_decl *x = &r->decls[k];
                dt_puts(&s, C_PROP, x->name); dt_puts(&s, C_TEXT, ": ");
                dt_put_limited(&s, C_TEXT, x->value, strlen(x->value), 300);
                if (x->important) dt_puts(&s, C_ACCENT, " !important");
                dt_puts(&s, C_TEXT, ";");
                dt_line_add(&tab->dt->side, NULL, 1, x->overridden ? DT_STRIKE : 0, &s, NULL);
            }
            dt_puts(&s, C_TEXT, "}");
            dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL);
        }
        if (!styles.count) { dt_puts(&s, C_DIM, "No matching rules."); dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL); }
        web_inspect_styles_free(&styles);
    } else if (tab->dt->sub == DT_COMPUTED) {
        char *computed = web_inspect_computed(tab->doc, tab->dt->selected);
        if (!computed) {
            dt_puts(&s, C_DIM, "Computed style is unavailable while the page runtime is busy.");
            dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL);
            return;
        }
        for (char *p = computed; *p;) {
            size_t n = strcspn(p, "\n");
            char *colon = memchr(p, ':', n);
            if (colon) {
                dt_put(&s, C_PROP, p, (size_t)(colon - p)); dt_puts(&s, C_TEXT, ":");
                dt_put_limited(&s, C_TEXT, colon + 1, n - (size_t)(colon + 1 - p), 300);
                dt_line_add(&tab->dt->side, NULL, 0, 0, &s, NULL);
            }
            p += n; if (*p) p++;
        }
        free(computed);
    }
}
static void dt_measure(void) {
    tab->dt->overlay_valid = tab->dt->hovered && tab->doc && web_inspect_box(tab->doc, tab->dt->hovered, &tab->dt->overlay);
    if (tab->dt->overlay_valid) {
        struct sb s = {0};
        dt_node_label(&s, tab->dt->hovered);
        char size[48];
        snprintf(size, sizeof size, "  %d \xc3\x97 %d", tab->dt->overlay.w, tab->dt->overlay.h);
        dt_puts(&s, C_TEXT, size);
        strlcpy(tab->dt->overlay_label, s.b ? s.b : "", sizeof tab->dt->overlay_label);
        free(s.b);
    }
    tab->dt->sel_valid = tab->dt->selected && tab->doc && web_inspect_box(tab->doc, tab->dt->selected, &tab->dt->sel_box);
}

/* Main-loop entry: a layout just finished, or DevTools input needs fresh
   state. Layout-driven rebuilds are paced; input-driven ones are immediate. */
static void devtools_refresh(bool force) {
    if (!tab->console_open || browser_fullscreen || tab != front) return;
    dt_sync_doc();
    if (!tab->doc || tab->native_wait || web_script_running(tab->doc)) return;
    web_node *root = web_inspect_root(tab->doc);
    if (tab->dt->selected && !web_inspect_contains(root, tab->dt->selected)) { tab->dt->selected = NULL; tab->dt->side_at = 0; }
    if (tab->dt->hovered && !web_inspect_contains(root, tab->dt->hovered)) tab->dt->hovered = NULL;
    if (tab->dt->panel != DT_ELEMENTS) { dt_measure(); return; }
    uint64_t now = n_clock_uptime_ms();
    if (force || now - tab->dt->tree_at >= 250) { dt_build_tree(); tab->dt->tree_at = now; tab->console_dirty = true; }
    if (force || !tab->dt->side_at || now - tab->dt->side_at >= 1000) { dt_refresh_side(); tab->console_dirty = true; }
    dt_measure();
}
static void dt_select(web_node *n) {
    if (n && web_inspect_kind(n) == WEB_INSPECT_TEXT && web_inspect_parent(n)) n = web_inspect_parent(n);
    tab->dt->selected = n;
    tab->dt->side.top = 0;
    if (n) for (web_node *p = web_inspect_parent(n); p && web_inspect_kind(p) != WEB_INSPECT_DOCUMENT; p = web_inspect_parent(p))
        dt_set_open(p, true);
    devtools_refresh(true);
    dt_scroll_to_row(&tab->dt->tree, dt_row_of(n), dt_panel_rows());
}

/* ---------------------------------------------------------------- network */
#define DT_NET_MAX 20000

static void dt_request_free(struct dt_request *r) {
    free(r->method); free(r->url); free(r->request_headers); free(r->response_headers); free(r->error); free(r->mime); free(r->sent_headers);
}
static struct devtools *devtools_new(void) {
    struct devtools *d = calloc(1, sizeof *d);
    if (d) { d->panel = DT_CONSOLE; d->sub = DT_STYLES; d->net_selected = -1; }
    return d;
}
static void devtools_free(struct devtools *d) {
    if (!d) return;
    struct dt_lines *lines[] = {&d->tree, &d->side, &d->detail};
    for (size_t i = 0; i < 3; i++) { dt_lines_clear(lines[i]); free(lines[i]->v); }
    for (int i = 0; i < d->net_n; i++) dt_request_free(&d->net[i]);
    free(d->net); free(d->open); free(d);
}
static void dt_net_clear(void) {
    for (int i = 0; i < tab->dt->net_n; i++) dt_request_free(&tab->dt->net[i]);
    tab->dt->net_n = tab->dt->net_top = 0; tab->dt->net_selected = -1; tab->dt->nav_serial = 0;
    dt_lines_clear(&tab->dt->detail);
    tab->console_dirty = true;
}
/* Requests are recorded while DevTools is open, as in other browsers. */
static uint64_t dt_net_begin(bool navigation, uint64_t gen, uint64_t resource, int kind,
                             const char *method, const char *url, const char *headers) {
    if (!tab->console_open) return 0;
    if (tab->dt->net_n == DT_NET_MAX) {      /* keep the newer half of a very long session */
        int drop = DT_NET_MAX / 2;
        for (int i = 0; i < drop; i++) dt_request_free(&tab->dt->net[i]);
        memmove(tab->dt->net, tab->dt->net + drop, (size_t)(tab->dt->net_n - drop) * sizeof *tab->dt->net);
        tab->dt->net_n -= drop;
        tab->dt->net_selected = tab->dt->net_selected >= drop ? tab->dt->net_selected - drop : -1;
        tab->dt->net_top = MAX(0, tab->dt->net_top - drop);
    }
    if (tab->dt->net_n == tab->dt->net_capacity) {
        int capacity = tab->dt->net_capacity ? tab->dt->net_capacity * 2 : 256;
        void *grown = realloc(tab->dt->net, (size_t)capacity * sizeof *tab->dt->net);
        if (!grown) return 0;
        tab->dt->net = grown; tab->dt->net_capacity = capacity;
    }
    struct dt_request *r = &tab->dt->net[tab->dt->net_n];
    memset(r, 0, sizeof *r);
    r->serial = ++tab->dt->net_serial; r->generation = gen; r->resource = resource; r->kind = kind;
    r->navigation = navigation; r->start = n_clock_uptime_ms();
    r->method = strdup(method ? method : "GET"); r->url = strdup(url ? url : "");
    r->request_headers = headers ? strdup(headers) : NULL;
    if (!r->method || !r->url) { dt_request_free(r); return 0; }
    tab->dt->net_n++;
    tab->console_dirty = true;
    return r->serial;
}
static struct dt_request *dt_net_find(uint64_t serial) {
    int lo = 0, hi = tab->dt->net_n;
    while (serial && lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (tab->dt->net[mid].serial < serial) lo = mid + 1; else hi = mid;
    }
    return serial && lo < tab->dt->net_n && tab->dt->net[lo].serial == serial ? &tab->dt->net[lo] : NULL;
}
static char *dt_header_value(const char *headers, const char *name) {
    size_t n = strlen(name);
    for (const char *p = headers; p && *p;) {
        size_t line = strcspn(p, "\r\n");
        if (line > n && !strncasecmp(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t k = strcspn(v, ";\r\n");
            while (k && (v[k - 1] == ' ' || v[k - 1] == '\t')) k--;
            char *out = malloc(k + 1);
            if (out) { memcpy(out, v, k); out[k] = 0; }
            return out;
        }
        p += line;
        while (*p == '\r' || *p == '\n') p++;
    }
    return NULL;
}
static void dt_net_detail_build(void);
static void dt_net_update(uint64_t serial, const struct webnet_response *result) {
    struct dt_request *r = dt_net_find(serial);
    if (!r || result->event == WEBNET_UPLOAD) return;
    if (result->event == WEBNET_HEADERS || result->event == WEBNET_COMPLETE) {
        r->status = result->status;
        if (result->headers && (!r->response_headers || strcmp(r->response_headers, result->headers))) {
            free(r->response_headers); r->response_headers = strdup(result->headers);
            free(r->mime); r->mime = dt_header_value(result->headers, "content-type");
        }
        if (result->sent_headers && *result->sent_headers && (!r->sent_headers || strcmp(r->sent_headers, result->sent_headers))) {
            free(r->sent_headers); r->sent_headers = strdup(result->sent_headers);
        }
    }
    if (result->event == WEBNET_CHUNK) { r->size += result->body_len; r->streamed = true; }
    if (result->event == WEBNET_COMPLETE) r->size = r->streamed ? r->size + result->body_len : result->body_len;
    if (result->event == WEBNET_COMPLETE || result->event == WEBNET_END) {
        r->done = true; r->end = n_clock_uptime_ms();
        if (result->error && result->error[0] && !r->error) r->error = strdup(result->error);
    }
    if (tab->dt->net_selected >= 0 && &tab->dt->net[tab->dt->net_selected] == r) dt_net_detail_build();
    if (tab->console_open && tab->dt->panel == DT_NETWORK) tab->console_dirty = true;
}
static const char *dt_request_type(const struct dt_request *r) {
    const char *mime = r->mime ? r->mime : "";
    if (r->navigation || r->kind == WEB_RESOURCE_FRAME) return "document";
    switch (r->kind) {
    case WEB_RESOURCE_SCRIPT: case WEB_RESOURCE_MODULE: return "script";
    case WEB_RESOURCE_CSS: return "stylesheet";
    case WEB_RESOURCE_IMAGE: return "img";
    case WEB_RESOURCE_REPORT: return "ping";
    }
    if (!strncasecmp(mime, "video/", 6) || !strncasecmp(mime, "audio/", 6)) return "media";
    if (!strncasecmp(mime, "font/", 5) || strstr(mime, "font")) return "font";
    if (!strncasecmp(mime, "image/", 6)) return "img";
    if (strstr(mime, "javascript")) return "script";
    if (!strcasecmp(mime, "text/css")) return "stylesheet";
    return "fetch";
}
static void dt_format_size(char *out, size_t capacity, size_t bytes) {
    if (bytes < 1000) snprintf(out, capacity, "%lu B", (unsigned long)bytes);
    else if (bytes < 1000000) snprintf(out, capacity, "%.1f kB", bytes / 1000.0);
    else snprintf(out, capacity, "%.1f MB", bytes / 1000000.0);
}
static void dt_format_time(char *out, size_t capacity, uint64_t ms) {
    if (ms < 1000) snprintf(out, capacity, "%lu ms", (unsigned long)ms);
    else snprintf(out, capacity, "%.2f s", ms / 1000.0);
}
/* Wraps a value at the pane width; continuation lines are indented. */
static void dt_detail_add(const char *name, const char *value, int width) {
    struct sb s = {0};
    if (name) { dt_puts(&s, C_PROP, name); dt_puts(&s, C_TEXT, ": "); }
    int used = name ? gfx_text_width(name, FONT_SMALL) + gfx_text_width(": ", FONT_SMALL) : 0;
    const char *p = value ? value : "";
    int room = MAX(width - 24, 80);
    do {
        size_t n = 0; int x = used;
        while (p[n]) {
            uint32_t cp; int k = gfx_utf8_decode(p + n, &cp);
            if (k <= 0) break;
            int advance = MAX(gfx_codepoint_width(cp, FONT_SMALL), 0);
            if (x + advance > room && n) break;
            x += advance; n += (size_t)k;
        }
        if (!n && *p) n = 1;
        dt_put(&s, C_TEXT, p, n);
        dt_line_add(&tab->dt->detail, NULL, used ? 0 : 1, 0, &s, NULL);
        p += n; used = 0;
    } while (*p);
}
static void dt_detail_headers(const char *title, const char *headers, int width) {
    struct sb s = {0};
    dt_puts(&s, C_ACCENT, title);
    dt_line_add(&tab->dt->detail, NULL, 0, DT_HEADER, &s, NULL);
    if (!headers || !*headers) { dt_puts(&s, C_DIM, "(none)"); dt_line_add(&tab->dt->detail, NULL, 0, 0, &s, NULL); return; }
    for (const char *p = headers; *p;) {
        size_t line = strcspn(p, "\r\n");
        const char *colon = memchr(p, ':', line);
        if (colon && colon > p) {
            char name[128]; size_t k = MIN((size_t)(colon - p), sizeof name - 1);
            memcpy(name, p, k); name[k] = 0;
            const char *v = colon + 1; while (*v == ' ' && v < p + line) v++;
            char *value = strndup(v, (size_t)(p + line - v));
            if (value) { dt_detail_add(name, value, width); free(value); }
        }
        p += line;
        while (*p == '\r' || *p == '\n') p++;
    }
}
static int dt_net_list_w(void) { return tab->dt->net_selected >= 0 ? view.w * 9 / 20 : view.w; }
static void dt_net_detail_build(void) {
    dt_lines_clear(&tab->dt->detail);
    if (tab->dt->net_selected < 0 || tab->dt->net_selected >= tab->dt->net_n) return;
    const struct dt_request *r = &tab->dt->net[tab->dt->net_selected];
    int width = view.w - dt_net_list_w();
    char text[64];
    struct sb s = {0};
    dt_puts(&s, C_ACCENT, "General");
    dt_line_add(&tab->dt->detail, NULL, 0, DT_HEADER, &s, NULL);
    dt_detail_add("Request URL", r->url, width);
    dt_detail_add("Request Method", r->method, width);
    if (r->status) { snprintf(text, sizeof text, "%d", r->status); dt_detail_add("Status Code", text, width); }
    else dt_detail_add("Status Code", r->done ? "(failed)" : "(pending)", width);
    if (r->error) dt_detail_add("Error", r->error, width);
    dt_detail_add("Type", dt_request_type(r), width);
    if (r->mime) dt_detail_add("Content-Type", r->mime, width);
    dt_format_size(text, sizeof text, r->size); dt_detail_add("Size", text, width);
    if (r->done) { dt_format_time(text, sizeof text, r->end - r->start); dt_detail_add("Time", text, width); }
    dt_detail_headers("Response Headers", r->response_headers, width);
    /* What went out, as other browsers show it; before the response says, what the page set. */
    if (r->sent_headers) dt_detail_headers("Request Headers", r->sent_headers, width);
    else dt_detail_headers(r->navigation ? "Request Headers" : "Request Headers (set by the page)", r->request_headers, width);
}
static void dt_net_select(int index) {
    tab->dt->net_selected = index >= 0 && index < tab->dt->net_n ? index : -1;
    tab->dt->detail.top = 0;
    dt_net_detail_build();
    if (tab->dt->net_selected >= 0) {
        int rows = dt_rows_in(console_h() - DT_TABH - DT_NET_HEAD);
        if (tab->dt->net_selected < tab->dt->net_top) tab->dt->net_top = tab->dt->net_selected;
        else if (tab->dt->net_selected >= tab->dt->net_top + rows) tab->dt->net_top = tab->dt->net_selected - rows + 1;
    }
    tab->console_dirty = true;
}

/* ---------------------------------------------------------------- drawing */
static const char *const dt_tab_names[DT_NTABS] = {"Elements", "Console", "Network", "Trace"};
static const char *const dt_sub_names[DT_NSUB] = {"Styles", "Computed", "Layout"};
static int dt_tab_x(int panel) {
    int x = 36;
    for (int i = 0; i < panel; i++) x += gfx_text_width(dt_tab_names[i], FONT_SMALL) + 28;
    return x;
}
static int dt_sub_x(int split, int sub) {
    int x = split + 10;
    for (int i = 0; i < sub; i++) x += gfx_text_width(dt_sub_names[i], FONT_SMALL) + 24;
    return x;
}
static int dt_split(void) { return view.w * 3 / 5; }

static void dt_draw_lines(canvas_t *c, struct dt_lines *l, int x, int y, int width, int height, bool tree) {
    int rows = dt_rows_in(height);
    dt_clamp_top(l, rows);
    canvas_t clip = *c;
    gfx_clip(&clip, x, y, width - 6, height);
    for (int i = 0; i < rows && l->top + i < l->n; i++) {
        const struct dt_line *line = &l->v[l->top + i];
        int ly = y + i * DT_ROWH;
        if (tree && line->node && line->node == tab->dt->selected) gfx_fill(&clip, x, ly, width, DT_ROWH, DT_SELECT);
        else if (tree && line->node && line->node == tab->dt->hovered) gfx_fill(&clip, x, ly, width, DT_ROWH, DT_HOVER);
        else if (line->flags & DT_HEADER) gfx_fill(&clip, x, ly, width, DT_ROWH, DT_BAR);
        int lx = x + 4 + line->depth * 14;
        if (line->flags & DT_EXPANDABLE) {
            int cx = lx + 4, cy = ly + 8;
            if (line->flags & DT_OPEN) gfx_triangle(&clip, cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3, dt_palette[C_DIM]);
            else gfx_triangle(&clip, cx - 2, cy - 4, cx - 2, cy + 4, cx + 3, cy, dt_palette[C_DIM]);
        }
        int tx = lx + 12;
        int end = dt_draw_text(&clip, tx, ly, line->text, dt_palette[C_TEXT]);
        if (line->flags & DT_STRIKE) gfx_hline(&clip, tx, ly + 8, MIN(end, clip.cx1) - tx, dt_palette[C_TEXT]);
        if (line->right) {
            int rw = gfx_text_width(line->right, FONT_SMALL), rx = x + width - rw - 10;
            if (rx > MIN(end, x + width) + 12) gfx_text(&clip, rx, ly, line->right, DT_BLUE, TRANSPARENT, FONT_SMALL);
        }
    }
    if (l->n > rows) {      /* position indicator */
        int th = MAX(16, height * rows / l->n), ty = y + (int)((int64_t)(height - th) * l->top / MAX(l->n - rows, 1));
        gfx_fill(c, x + width - 5, ty, 4, th, RGB(95, 99, 104));
    }
}
static void dt_draw_tabs(canvas_t *c, int top) {
    gfx_fill(c, 0, top, view.w, DT_TABH, DT_BAR);
    gfx_hline(c, 0, top, view.w, DT_RULE);
    gfx_hline(c, 0, top + DT_TABH - 1, view.w, DT_RULE);
    /* element picker: a box with a pointer */
    uint32_t picker = tab->dt->picking ? DT_BLUE : dt_palette[C_DIM];
    gfx_rect(c, 9, top + 6, 11, 11, picker);
    gfx_triangle(c, 15, top + 11, 24, top + 15, 18, top + 19, picker);
    gfx_vline(c, 30, top + 4, DT_TABH - 8, DT_RULE);
    for (int i = 0; i < DT_NTABS; i++) {
        int x = dt_tab_x(i), tw = gfx_text_width(dt_tab_names[i], FONT_SMALL);
        gfx_text(c, x, top + 4, dt_tab_names[i], i == tab->dt->panel ? RGB(232, 234, 237) : dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
        if (i == tab->dt->panel) gfx_fill(c, x - 6, top + DT_TABH - 3, tw + 12, 2, DT_BLUE);
    }
    char hint[128];
    if (tab->dt->panel == DT_ELEMENTS) snprintf(hint, sizeof hint, "%s", tab->dt->picking ? "Click an element on the page (Esc: cancel)" : "Ctrl+Shift+C: pick   \xe2\x86\x90\xe2\x86\x92: collapse/expand");
    else if (tab->dt->panel == DT_NETWORK) {
        size_t total = 0; for (int i = 0; i < tab->dt->net_n; i++) total += tab->dt->net[i].size;
        char size[32]; dt_format_size(size, sizeof size, total);
        snprintf(hint, sizeof hint, "%d requests   %s transferred   Ctrl+K: clear", tab->dt->net_n, size);
    } else if (tab->dt->panel == DT_TRACE) snprintf(hint, sizeof hint, "%d trace lines", tab->console_trace.n);
    else snprintf(hint, sizeof hint, "%d messages", tab->console_page.n);
    int hw = gfx_text_width(hint, FONT_SMALL), hx = view.w - hw - 12;
    if (hx > dt_tab_x(DT_NTABS)) gfx_text(c, hx, top + 4, hint, dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
}
static void dt_box_numbers(canvas_t *c, int x, int y, int width, int height, const float v[4]) {
    char text[24];
    for (int side = 0; side < 4; side++) {
        if (v[side] == 0) strcpy(text, "-"); else snprintf(text, sizeof text, "%g", (double)v[side]);
        int tw = gfx_text_width(text, FONT_SMALL), tx, ty;
        if (side == 0) { tx = x + (width - tw) / 2; ty = y + 3; }
        else if (side == 2) { tx = x + (width - tw) / 2; ty = y + height - 19; }
        else if (side == 3) { tx = x + 6; ty = y + (height - 16) / 2; }
        else { tx = x + width - tw - 6; ty = y + (height - 16) / 2; }
        gfx_text(c, tx, ty, text, RGB(20, 20, 20), TRANSPARENT, FONT_SMALL);
    }
}
static void dt_draw_box_model(canvas_t *c, int x, int y, int width, int height) {
    canvas_t clip = *c;
    gfx_clip(&clip, x, y, width, height);
    if (!tab->dt->sel_valid) {
        gfx_text(&clip, x + 12, y + 8, tab->dt->selected ? "This element generates no box (display: none, or not rendered)." :
                 "Select an element to see its box model.", dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
        return;
    }
    const struct web_inspect_box *b = &tab->dt->sel_box;
    int bw = MIN(width - 24, 420), bh = MIN(height - 60, 220);
    if (bw < 200 || bh < 140) { bw = MAX(bw, 200); bh = MAX(bh, 140); }
    int ox = x + (width - bw) / 2, oy = y + 10;
    static const char *const names[] = {"margin", "border", "padding"};
    const uint32_t colors[] = {RGB(196, 143, 96), RGB(227, 200, 128), RGB(142, 178, 117), RGB(111, 156, 199)};
    const float *values[] = {b->margin, b->border, b->padding};
    int lx = ox, ly = oy, lw = bw, lh = bh;
    for (int i = 0; i < 3; i++) {
        gfx_fill(&clip, lx, ly, lw, lh, colors[i]);
        gfx_rect(&clip, lx, ly, lw, lh, RGB(30, 30, 30));
        gfx_text(&clip, lx + 4, ly + 2, names[i], RGB(30, 30, 30), TRANSPARENT, FONT_SMALL);
        dt_box_numbers(&clip, lx, ly, lw, lh, values[i]);
        lx += 40; ly += 26; lw -= 80; lh -= 52;
    }
    gfx_fill(&clip, lx, ly, lw, lh, colors[3]);
    gfx_rect(&clip, lx, ly, lw, lh, RGB(30, 30, 30));
    float cw = b->w - b->border[1] - b->border[3] - b->padding[1] - b->padding[3];
    float ch = b->h - b->border[0] - b->border[2] - b->padding[0] - b->padding[2];
    char text[96];
    snprintf(text, sizeof text, "%g \xc3\x97 %g", (double)MAX(cw, 0), (double)MAX(ch, 0));
    gfx_text(&clip, lx + (lw - gfx_text_width(text, FONT_SMALL)) / 2, ly + (lh - 16) / 2, text, RGB(20, 20, 20), TRANSPARENT, FONT_SMALL);
    snprintf(text, sizeof text, "border box: %d \xc3\x97 %d at (%d, %d) in the document", b->w, b->h, b->x, b->y);
    gfx_text(&clip, ox, oy + bh + 12, text, dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
}
static void dt_draw_elements(canvas_t *c, int top, int h) {
    int split = dt_split();
    if (!tab->dt->tree.n) gfx_text(c, 12, top + 6, tab->doc ? "Waiting for the document layout..." : "No document.", dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
    else dt_draw_lines(c, &tab->dt->tree, 0, top, split, h, true);
    gfx_vline(c, split, top, h, DT_RULE);
    gfx_fill(c, split + 1, top, view.w - split - 1, DT_SUBH, DT_BAR);
    gfx_hline(c, split + 1, top + DT_SUBH - 1, view.w - split - 1, DT_RULE);
    for (int i = 0; i < DT_NSUB; i++) {
        int x = dt_sub_x(split, i), tw = gfx_text_width(dt_sub_names[i], FONT_SMALL);
        gfx_text(c, x, top + 3, dt_sub_names[i], i == tab->dt->sub ? RGB(232, 234, 237) : dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
        if (i == tab->dt->sub) gfx_fill(c, x - 4, top + DT_SUBH - 3, tw + 8, 2, DT_BLUE);
    }
    if (tab->dt->sub == DT_LAYOUT) dt_draw_box_model(c, split + 1, top + DT_SUBH, view.w - split - 1, h - DT_SUBH);
    else dt_draw_lines(c, &tab->dt->side, split + 1, top + DT_SUBH + 2, view.w - split - 1, h - DT_SUBH - 2, false);
}
static uint32_t dt_type_color(const char *type) {
    if (!strcmp(type, "document")) return RGB(138, 180, 248);
    if (!strcmp(type, "script")) return RGB(253, 214, 99);
    if (!strcmp(type, "stylesheet")) return RGB(197, 138, 249);
    if (!strcmp(type, "img")) return RGB(129, 201, 149);
    if (!strcmp(type, "media")) return RGB(242, 139, 130);
    return RGB(120, 186, 198);
}
static void dt_draw_network(canvas_t *c, int top, int h) {
    int list_w = dt_net_list_w();
    static const char *const heads[] = {"Name", "Status", "Type", "Size", "Time", "Waterfall"};
    int name_w = MAX(list_w * 2 / 5, 120);
    int cols[7] = {0, name_w, name_w + 64, name_w + 150, name_w + 230, name_w + 300, list_w};
    bool compact = tab->dt->net_selected >= 0;
    int ncols = compact ? 3 : 6;
    if (compact) cols[1] = list_w - 150, cols[2] = list_w - 80, cols[3] = list_w;
    gfx_fill(c, 0, top, list_w, DT_NET_HEAD, DT_BAR);
    gfx_hline(c, 0, top + DT_NET_HEAD - 1, list_w, DT_RULE);
    for (int i = 0; i < ncols; i++) {
        gfx_text(c, cols[i] + 6, top + 2, compact && i == 2 ? "Time" : heads[i], dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
        if (i) gfx_vline(c, cols[i], top, h, DT_RULE);
    }
    int y = top + DT_NET_HEAD, rows = dt_rows_in(h - DT_NET_HEAD);
    tab->dt->net_top = MAX(0, MIN(tab->dt->net_top, tab->dt->net_n - rows));
    if (!tab->dt->net_n) {
        gfx_text(c, 12, y + 8, "Recording network activity. Reload the page (F5) to record its requests.", dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
    }
    uint64_t now = n_clock_uptime_ms(), first = tab->dt->net_n ? tab->dt->net[0].start : now, last = first + 1;
    for (int i = 0; i < tab->dt->net_n; i++) last = MAX(last, tab->dt->net[i].done ? tab->dt->net[i].end : now);
    for (int i = 0; i < rows && tab->dt->net_top + i < tab->dt->net_n; i++) {
        const struct dt_request *r = &tab->dt->net[tab->dt->net_top + i];
        int ry = y + i * DT_ROWH;
        bool failed = r->error || r->status >= 400 || (r->done && !r->status);
        if (tab->dt->net_top + i == tab->dt->net_selected) gfx_fill(c, 0, ry, list_w, DT_ROWH, DT_SELECT);
        else if ((tab->dt->net_top + i) & 1) gfx_fill(c, 0, ry, list_w, DT_ROWH, RGB(36, 37, 40));
        char name[160], text[48];
        dt_basename(r->url, name, sizeof name);
        if (!name[0]) strlcpy(name, r->url, sizeof name);
        const char *type = dt_request_type(r);
        canvas_t cell = *c;
        gfx_clip(&cell, cols[0] + 4, ry, cols[1] - cols[0] - 8, DT_ROWH);
        gfx_text(&cell, cols[0] + 6, ry, name, failed ? dt_palette[C_ERROR] : dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
        if (compact) {
            if (r->done) dt_format_time(text, sizeof text, r->end - r->start); else strcpy(text, "(pending)");
            if (r->status) { char status_text[16]; snprintf(status_text, sizeof status_text, "%d", r->status);
                cell = *c; gfx_clip(&cell, cols[1], ry, cols[2] - cols[1], DT_ROWH);
                gfx_text(&cell, cols[1] + 6, ry, status_text, failed ? dt_palette[C_ERROR] : dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL); }
            cell = *c; gfx_clip(&cell, cols[2], ry, cols[3] - cols[2], DT_ROWH);
            gfx_text(&cell, cols[2] + 6, ry, text, dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
            continue;
        }
        if (r->status) snprintf(text, sizeof text, "%d", r->status);
        else strcpy(text, r->done ? "(failed)" : "(pending)");
        gfx_text(c, cols[1] + 6, ry, text, failed ? dt_palette[C_ERROR] : r->status ? dt_palette[C_TEXT] : dt_palette[C_DIM], TRANSPARENT, FONT_SMALL);
        gfx_text(c, cols[2] + 6, ry, type, dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
        dt_format_size(text, sizeof text, r->size);
        gfx_text(c, cols[3] + 6, ry, text, dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
        if (r->done) dt_format_time(text, sizeof text, r->end - r->start); else strcpy(text, "-");
        gfx_text(c, cols[4] + 6, ry, text, dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
        int wx = cols[5] + 4, ww = MAX(cols[6] - cols[5] - 10, 1);
        uint64_t span = last - first, end = r->done ? r->end : now;
        int bx = wx + (int)((r->start - first) * (uint64_t)ww / span);
        int bw = MAX(2, (int)((end - r->start) * (uint64_t)ww / span));
        gfx_fill(c, bx, ry + 4, MIN(bw, wx + ww - bx), 8, dt_type_color(type));
    }
    if (compact) {
        int dx = list_w + 1, dw = view.w - dx;
        gfx_vline(c, list_w, top, h, DT_RULE);
        gfx_fill(c, dx, top, dw, DT_NET_HEAD, DT_BAR);
        gfx_hline(c, dx, top + DT_NET_HEAD - 1, dw, DT_RULE);
        gfx_text(c, dx + 8, top + 2, "\xc3\x97", dt_palette[C_TEXT], TRANSPARENT, FONT_SMALL);
        gfx_text(c, dx + 30, top + 2, "Headers", RGB(232, 234, 237), TRANSPARENT, FONT_SMALL);
        gfx_fill(c, dx + 26, top + DT_NET_HEAD - 3, gfx_text_width("Headers", FONT_SMALL) + 8, 2, DT_BLUE);
        dt_draw_lines(c, &tab->dt->detail, dx, top + DT_NET_HEAD + 2, dw, h - DT_NET_HEAD - 2, false);
    }
}
static void devtools_draw(canvas_t *c, int top, int h) {
    dt_draw_tabs(c, top);
    canvas_t body = *c;
    gfx_clip(&body, 0, top + DT_TABH, view.w, h - DT_TABH);
    gfx_fill(&body, 0, top + DT_TABH, view.w, h - DT_TABH, DT_BG);
    if (tab->dt->panel == DT_ELEMENTS) dt_draw_elements(&body, top + DT_TABH, h - DT_TABH);
    else if (tab->dt->panel == DT_NETWORK) dt_draw_network(&body, top + DT_TABH, h - DT_TABH);
}

/* ---------------------------------------------------------------- page highlight */
static bool devtools_overlay_visible(void) { return tab->console_open && !browser_fullscreen && tab->dt->overlay_valid; }
static void dt_band(canvas_t *c, int ox, int oy, int ow, int oh, int ix, int iy, int iw, int ih, uint32_t argb) {
    if (iy > oy) gfx_fill_blend(c, ox, oy, ow, iy - oy, argb);
    if (oy + oh > iy + ih) gfx_fill_blend(c, ox, iy + ih, ow, oy + oh - iy - ih, argb);
    if (ix > ox) gfx_fill_blend(c, ox, iy, ix - ox, ih, argb);
    if (ox + ow > ix + iw) gfx_fill_blend(c, ix + iw, iy, ox + ow - ix - iw, ih, argb);
}
static int dt_px(float v) { return v > 0 ? (int)(v + 0.5f) : 0; }
static void devtools_draw_overlay(canvas_t *page) {
    if (!devtools_overlay_visible()) return;
    canvas_t c = *page;
    gfx_clip(&c, 0, 0, page_w(), page_h());
    const struct web_inspect_box *b = &tab->dt->overlay;
    int bx = b->x - tab->scroll_x, by = b->y - tab->scroll_y;
    int mt = dt_px(b->margin[0]), mr = dt_px(b->margin[1]), mb = dt_px(b->margin[2]), ml = dt_px(b->margin[3]);
    int bt = dt_px(b->border[0]), br = dt_px(b->border[1]), bb = dt_px(b->border[2]), bl = dt_px(b->border[3]);
    int pt = dt_px(b->padding[0]), pr = dt_px(b->padding[1]), pb = dt_px(b->padding[2]), pl = dt_px(b->padding[3]);
    int px = bx + bl, py = by + bt, pw = MAX(b->w - bl - br, 0), ph = MAX(b->h - bt - bb, 0);
    int cx = px + pl, cy = py + pt, cw = MAX(pw - pl - pr, 0), ch = MAX(ph - pt - pb, 0);
    dt_band(&c, bx - ml, by - mt, b->w + ml + mr, b->h + mt + mb, bx, by, b->w, b->h, ARGB(100, 246, 178, 107));
    dt_band(&c, bx, by, b->w, b->h, px, py, pw, ph, ARGB(110, 255, 229, 153));
    dt_band(&c, px, py, pw, ph, cx, cy, cw, ch, ARGB(130, 147, 196, 125));
    gfx_fill_blend(&c, cx, cy, cw, ch, ARGB(110, 111, 168, 220));
    /* tooltip under the box, or above it near the bottom of the page */
    int tw = 0;
    for (const char *p = tab->dt->overlay_label; *p;) {
        if ((unsigned char)*p < 0x20) { p++; continue; }
        uint32_t cp; int k = gfx_utf8_decode(p, &cp); if (k <= 0) break;
        tw += MAX(gfx_codepoint_width(cp, FONT_SMALL), 0); p += k;
    }
    int tx = MAX(4, MIN(bx, page_w() - tw - 20)), ty = by + b->h + mb + 6;
    if (ty + 22 > page_h()) ty = by - mt - 28;
    ty = MAX(2, ty);
    gfx_fill_round(&c, tx, ty, tw + 16, 22, 4, RGB(48, 50, 54));
    dt_draw_text(&c, tx + 8, ty + 3, tab->dt->overlay_label, dt_palette[C_TEXT]);
}

/* ---------------------------------------------------------------- input */
static void dt_set_hover(web_node *n) {
    if (n && web_inspect_kind(n) == WEB_INSPECT_TEXT) n = web_inspect_parent(n);
    if (n == tab->dt->hovered) return;
    bool had = tab->dt->overlay_valid;
    tab->dt->hovered = n;
    dt_measure();
    if (had || tab->dt->overlay_valid) tab->need_paint = true;
    tab->console_dirty = true;
}
static void devtools_set_tab(int panel) {
    if (panel == tab->dt->panel) return;
    tab->dt->panel = panel;
    if (panel != DT_ELEMENTS) tab->dt->picking = false;
    tab->focus = F_CONSOLE;
    devtools_refresh(true);
    tab->console_dirty = true;
    show_chrome(); /* the console's line is a field of the window, only under its own tab */
}
/* Global DevTools shortcuts: Ctrl+Shift+I toggles, J opens the console,
   C opens Elements with the picker. */
static void devtools_shortcut(uint32_t key) {
    if (key == 'i') { tab->console_open = !tab->console_open; }
    else {
        tab->console_open = true;
        tab->dt->panel = key == 'j' ? DT_CONSOLE : DT_ELEMENTS;
        tab->dt->picking = key == 'c' ? !tab->dt->picking : false;
    }
    if (tab->console_open) tab->focus = F_CONSOLE;
    else { if (tab->focus == F_CONSOLE) tab->focus = F_PAGE; tab->dt->picking = false; dt_set_hover(NULL); }
    tab->need_layout = tab->need_paint = true;
}
static void devtools_clear(void) {
    if (tab->dt->panel == DT_NETWORK) dt_net_clear();
    else if (tab->dt->panel == DT_TRACE) { tab->console_trace.n = tab->console_trace.skip = 0; tab->console_dirty = true; }
    else { tab->console_page.n = tab->console_page.skip = 0; tab->console_dirty = true; }
}
static void dt_toggle(int row) {
    if (row < 0 || row >= tab->dt->tree.n || !(tab->dt->tree.v[row].flags & DT_EXPANDABLE)) return;
    dt_set_open(tab->dt->tree.v[row].node, !(tab->dt->tree.v[row].flags & DT_OPEN));
    devtools_refresh(true);
}
/* Keys while DevTools has focus on a tab other than Console. */
static void devtools_key(const struct gui_event *e) {
    uint32_t k = e->key;
    bool ctrl = e->mods & NMOD_CTRL;
    if (ctrl && (k == '[' || k == ']')) { devtools_set_tab((tab->dt->panel + (k == ']' ? 1 : DT_NTABS - 1)) % DT_NTABS); return; }
    if (k == NKEY_ESC) {
        if (tab->dt->picking) { tab->dt->picking = false; dt_set_hover(NULL); }
        else if (tab->dt->panel == DT_NETWORK && tab->dt->net_selected >= 0) dt_net_select(-1);
        else tab->focus = F_PAGE;
        tab->console_dirty = true;
        return;
    }
    int rows = dt_panel_rows();
    if (tab->dt->panel == DT_NETWORK) {
        int list_rows = dt_rows_in(console_h() - DT_TABH - DT_NET_HEAD);
        if (k == NKEY_UP) dt_net_select(tab->dt->net_selected < 0 ? tab->dt->net_n - 1 : MAX(0, tab->dt->net_selected - 1));
        else if (k == NKEY_DOWN) dt_net_select(MIN(tab->dt->net_n - 1, tab->dt->net_selected + 1));
        else if (k == NKEY_PGUP) tab->dt->net_top = MAX(0, tab->dt->net_top - list_rows);
        else if (k == NKEY_PGDN) tab->dt->net_top += list_rows;
        tab->console_dirty = true;
        return;
    }
    if (k == NKEY_PGUP) { tab->dt->tree.top -= rows; tab->console_dirty = true; return; }
    if (k == NKEY_PGDN) { tab->dt->tree.top += rows; tab->console_dirty = true; return; }
    int row = dt_row_of(tab->dt->selected);
    web_node *next = NULL;
    if (k == NKEY_UP) next = row > 0 ? tab->dt->tree.v[row - 1].node : tab->dt->tree.n ? tab->dt->tree.v[0].node : NULL;
    else if (k == NKEY_DOWN) {
        int i = row + 1;
        while (i < tab->dt->tree.n && (tab->dt->tree.v[i].flags & DT_CLOSING)) i++;
        next = i < tab->dt->tree.n ? tab->dt->tree.v[i].node : tab->dt->selected;
    } else if (k == NKEY_LEFT && row >= 0) {
        if (tab->dt->tree.v[row].flags & DT_OPEN) { dt_toggle(row); return; }
        web_node *parent = web_inspect_parent(tab->dt->selected);
        if (parent && web_inspect_kind(parent) != WEB_INSPECT_DOCUMENT) next = parent;
    } else if (k == NKEY_RIGHT && row >= 0) {
        if ((tab->dt->tree.v[row].flags & DT_EXPANDABLE) && !(tab->dt->tree.v[row].flags & DT_OPEN)) { dt_toggle(row); return; }
        if (row + 1 < tab->dt->tree.n && tab->dt->tree.v[row + 1].depth > tab->dt->tree.v[row].depth) next = tab->dt->tree.v[row + 1].node;
    }
    if (next && next != tab->dt->selected) { dt_select(next); dt_set_hover(next); }
    tab->console_dirty = true;
}
/* Scrolls whichever DevTools list is under the pointer. Cache-only. */
static void devtools_wheel(int x, int y, int wheel) {
    (void)y;
    if (tab->dt->panel == DT_CONSOLE || tab->dt->panel == DT_TRACE) { console_scroll(tab->dt->panel == DT_TRACE ? &tab->console_trace : &tab->console_page, -wheel * 3); return; }
    if (tab->dt->panel == DT_ELEMENTS) { struct dt_lines *l = x < dt_split() ? &tab->dt->tree : &tab->dt->side; l->top = MAX(0, l->top - wheel * 3); }
    else if (x < dt_net_list_w()) tab->dt->net_top = MAX(0, tab->dt->net_top - wheel * 3);
    else tab->dt->detail.top = MAX(0, tab->dt->detail.top - wheel * 3);
    tab->console_dirty = true;
}
static bool dt_in_page(int x, int y) { return y >= 0 && y < page_h() && x >= 0 && x < page_w(); }
/* the band between the page and the panel that a press drags to resize it */
static bool dt_on_edge(int y) { int top = page_h(); return y >= top - 3 && y < top + 3 && y < view.h; }
/* The pointer DevTools wants at a point of the page's surface: the resize band's while over it
   or dragging it, the arrow over the panel and over the page while an element is being picked.
   false: the point is not DevTools' (the page says). */
static bool devtools_pointer_shape(int x, int y, uint32_t *shape) {
    if (!tab->console_open || browser_fullscreen) return false;
    if (dt_resizing || dt_on_edge(y)) { *shape = N_POINTER_NS_RESIZE; return true; }
    if ((tab->dt->picking && dt_in_page(x, y)) || (y >= page_h() && y < view.h)) { *shape = N_POINTER_DEFAULT; return true; }
    return false;
}
/* Primary press: the resize edge, a picked page element, or the panel.
   Returns true when DevTools consumed the press. */
static bool devtools_pointer_down(int x, int y) {
    if (!tab->console_open || browser_fullscreen) return false;
    int top = page_h();
    dt_sync_doc();
    if (dt_on_edge(y)) { dt_resizing = true; return true; }
    if (tab->dt->picking && tab->doc && dt_in_page(x, y)) {
        web_node *n = web_node_at(tab->doc, document_x(x), y + tab->scroll_y);
        tab->dt->picking = false;
        dt_swallow_up = true;
        tab->dt->panel = DT_ELEMENTS; tab->focus = F_CONSOLE;
        if (n) dt_select(n);
        dt_set_hover(NULL);
        tab->need_paint = true;
        return true;
    }
    if (y < top || y >= view.h) return false;
    tab->focus = F_CONSOLE;
    tab->console_dirty = true;
    if (y < top + DT_TABH) {
        if (x < 30) { tab->dt->picking = !tab->dt->picking; if (tab->dt->picking) devtools_set_tab(DT_ELEMENTS); else dt_set_hover(NULL); return true; }
        for (int i = 0; i < DT_NTABS; i++) {
            int tx = dt_tab_x(i), tw = gfx_text_width(dt_tab_names[i], FONT_SMALL);
            if (x >= tx - 12 && x < tx + tw + 12) { devtools_set_tab(i); break; }
        }
        return true;
    }
    int body = top + DT_TABH;
    if (tab->dt->panel == DT_ELEMENTS) {
        int split = dt_split();
        if (x < split) {
            int row = tab->dt->tree.top + (y - body) / DT_ROWH;
            if (row < 0 || row >= tab->dt->tree.n) return true;
            const struct dt_line *line = &tab->dt->tree.v[row];
            int lx = 4 + line->depth * 14;
            if ((line->flags & DT_EXPANDABLE) && x >= lx - 2 && x < lx + 12) dt_toggle(row);
            else dt_select(line->node);
        } else if (y < body + DT_SUBH) {
            for (int i = 0; i < DT_NSUB; i++) {
                int sx = dt_sub_x(split, i), sw = gfx_text_width(dt_sub_names[i], FONT_SMALL);
                if (x >= sx - 8 && x < sx + sw + 8 && i != tab->dt->sub) { tab->dt->sub = i; tab->dt->side.top = 0; devtools_refresh(true); dt_refresh_side(); }
            }
        }
        return true;
    }
    if (tab->dt->panel == DT_NETWORK) {
        int list_w = dt_net_list_w();
        if (x >= list_w) { if (y < body + DT_NET_HEAD && x < list_w + 24) dt_net_select(-1); return true; }
        if (y >= body + DT_NET_HEAD) {
            int row = tab->dt->net_top + (y - body - DT_NET_HEAD) / DT_ROWH;
            if (row >= 0 && row < tab->dt->net_n) dt_net_select(row);
        }
        return true;
    }
    return true; /* console: focus its input */
}
/* Pointer motion: resizing (consumed), row hover and picker hover. */
static bool devtools_pointer_move(int x, int y) {
    if (!tab->console_open || browser_fullscreen) return false;
    if (dt_resizing) {
        int room = view.h;
        devtools_height = MAX(MIN(view.h - y, room - 60), MIN(120, room / 2));
        tab->need_layout = tab->need_paint = true;
        return true;
    }
    dt_sync_doc();
    int top = page_h();
    if (tab->dt->picking && tab->doc && dt_in_page(x, y)) { dt_set_hover(web_node_at(tab->doc, document_x(x), y + tab->scroll_y)); return false; }
    if (tab->dt->panel == DT_ELEMENTS && y >= top + DT_TABH && y < view.h && x < dt_split()) {
        int row = tab->dt->tree.top + (y - top - DT_TABH) / DT_ROWH;
        dt_set_hover(row >= 0 && row < tab->dt->tree.n ? tab->dt->tree.v[row].node : NULL);
        return false;
    }
    if (tab->dt->hovered) dt_set_hover(NULL);
    return false;
}
