/* The browser's Settings window: the languages web pages are asked for, and the search history,
   all by mouse. It keeps no setting of its own. Each click changes the browser's preferences
   (webprefs.h) at once; every browser window and worker hears of it through its own watch on
   them, and a change made elsewhere (another of these windows) shows here at once through this
   window's watch, in the queue its loop waits on. The gear on the browser's toolbar starts it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <wctype.h>
#include <nocturne.h>
#include <ui.h>
#include <web.h>
#include <webprefs.h>
#include <webstorage.h>

#define WINDOW 1
#define SIDEBAR 216 /* the list of pages */
#define PAGE_PAD 30 /* the page's padding left and right */
#define CARD_PAD 16 /* a card's own (ui.c) */

enum { PAGE_LANGUAGES, PAGE_PRIVACY, PAGES };
static const struct { const char *label, *icon; } pages[PAGES] = {
    [PAGE_LANGUAGES] = {"Languages", "language"},
    [PAGE_PRIVACY] = {"Privacy", "clock"},
};

/* What a widget of a view does when it is used; events name a widget by its place in a view. A
   language's controls carry the language (its place in the catalog), not its row, so that a
   click on a view whose list changed since acts on the language that was clicked. */
enum { ACT_NONE, ACT_PAGE, ACT_UP, ACT_DOWN, ACT_REMOVE, ACT_ADD, ACT_RESTORE, ACT_SEARCH, ACT_HISTORY,
       ACT_CLEAR };
struct action { uint32_t act, arg; };
/* The views sent lately: an event names the view it came from, and more may have been sent
   before it is read. */
#define VIEWS 8
static struct sent { uint32_t view, count, cap; struct action *at; } sent[VIEWS];
static struct sent *building;
static uint32_t views;

static struct ui_session *session;
static int win_w = 760, win_h = 560;
static int page = PAGE_LANGUAGES;
static const struct webprefs_language *catalog;
static size_t catalog_count;
static size_t *chosen, chosen_count; /* the browser's languages now, most wanted first: places in the catalog */
static const char **tags;             /* room for the chosen tags and one more */
static char search[512];              /* what the search field holds */
static bool search_focused, watched;
static char status[256];              /* what the last change did, under the page; empty: the usual note */
static bool status_bad;

static void act(uint32_t place, uint32_t what, uint32_t arg) {
    struct sent *v = building;
    if (place >= v->cap) {
        uint32_t cap = v->cap ? v->cap : 64;
        while (cap <= place) cap *= 2;
        struct action *grown = realloc(v->at, cap * sizeof *grown);
        if (!grown) return; /* the widget does nothing */
        v->at = grown;
        v->cap = cap;
    }
    if (place >= v->count) {
        memset(v->at + v->count, 0, (place - v->count) * sizeof *v->at);
        v->count = place + 1;
    }
    v->at[place] = (struct action){what, arg};
}

static struct action action_of(const struct ui_event *e) {
    const struct sent *v = &sent[e->view % VIEWS];
    if (!e->view || v->view != e->view || e->node >= v->count) return (struct action){ACT_NONE, 0};
    return v->at[e->node];
}

/* ---- the browser's preferences ---- */

/* Reads the browser's languages again into chosen. */
static void read_languages(void) {
    char list[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1]; /* the list is shorter than its Accept-Language */
    chosen_count = 0;
    n_result r = webprefs_languages(list, sizeof list);
    if (r < 0) {
        snprintf(status, sizeof status, "The browser's languages could not be read: %s.", n_result_text(r));
        status_bad = true;
        return;
    }
    for (char *p = list, *end; *p; p = *end ? end + 1 : end) {
        end = p + strcspn(p, ",");
        size_t n = (size_t)(end - p);
        for (size_t i = 0; i < catalog_count; i++)
            if (strlen(catalog[i].tag) == n && !strncasecmp(catalog[i].tag, p, n)) {
                chosen[chosen_count++] = i;
                break;
            }
    }
}

/* The chosen tags in tags; their count. */
static size_t chosen_tags(void) {
    for (size_t i = 0; i < chosen_count; i++) tags[i] = catalog[chosen[i]].tag;
    return chosen_count;
}

static bool is_chosen(size_t language, size_t *at) {
    for (size_t i = 0; i < chosen_count; i++)
        if (chosen[i] == language) {
            if (at) *at = i;
            return true;
        }
    return false;
}

/* After a change: what it did, and the preferences as they are now (the window shows those). */
static void saved(n_result r) {
    if (r >= 0) {
        status[0] = 0;
        status_bad = false;
    } else {
        snprintf(status, sizeof status, "The browser's preferences could not be saved: %s.", n_result_text(r));
        status_bad = true;
    }
    webprefs_sync();
}

/* A change acts on the list as it is now, a change another process made included. */
static void languages_now(void) {
    webprefs_sync();
    read_languages();
}

static void move_language(size_t language, int by) {
    languages_now();
    size_t i, n = chosen_tags();
    if (!is_chosen(language, &i) || (by < 0 && !i) || (by > 0 && i + 1 >= n)) return;
    const char *t = tags[i];
    tags[i] = tags[i + by];
    tags[i + by] = t;
    saved(webprefs_set_languages(tags, n));
}

static void remove_language(size_t language) {
    languages_now();
    size_t i, n = chosen_tags();
    if (n < 2 || !is_chosen(language, &i)) return; /* the last one stays */
    memmove(tags + i, tags + i + 1, (n - i - 1) * sizeof *tags);
    saved(webprefs_set_languages(tags, n - 1));
}

static void add_language(size_t language) {
    languages_now();
    size_t n = chosen_tags();
    if (language >= catalog_count || is_chosen(language, NULL)) return;
    tags[n] = catalog[language].tag;
    if (!webprefs_languages_fit(tags, n + 1)) {
        snprintf(status, sizeof status, "Websites accept at most %u bytes of languages.", WEBPREFS_ACCEPT_LANGUAGE_MAX);
        status_bad = true;
        return;
    }
    saved(webprefs_set_languages(tags, n + 1));
}

static void set_history(bool on) {
    n_result r = webprefs_set_search_history(on);
    saved(r);
    if (r >= 0)
        strlcpy(status, on ? "Enabled. Past values are offered only on explicit selection."
                           : "Disabled. Stored history is kept until Clear is selected.", sizeof status);
}

static void clear_history(void) {
    bool ok = web_autocomplete_clear();
    webprefs_sync();
    strlcpy(status, ok ? "History removed and completion disabled."
                       : "Storage operation failed; verify the browser's data folder is writable.", sizeof status);
    status_bad = !ok;
}

/* ---- the search for a language to add ---- */

/* The next character of *s in lower case (Unicode's simple mapping), past it; 0 at the end or at
   bytes that are not UTF-8 (then *s stays). */
static char32_t next_folded(const char **s) {
    mbstate_t state = {0};
    char32_t c;
    size_t n = mbrtoc32(&c, *s, strnlen(*s, 4), &state);
    if (!n || n > 4) return 0;
    *s += n;
    return (char32_t)towlower((wint_t)c);
}

/* Whether s begins with q, case aside. */
static bool begins(const char *s, const char *q) {
    while (*q) {
        char32_t b = next_folded(&q), a = next_folded(&s);
        if (!b || a != b) return false;
    }
    return true;
}

/* Whether q begins name or one of its words. */
static bool name_matches(const char *name, const char *q) {
    bool start = true;
    for (const char *p = name; *p;) {
        if (start && begins(p, q)) return true;
        char32_t c = next_folded(&p);
        if (!c) return false;
        start = !iswalnum((wint_t)c);
    }
    return false;
}

/* q, without the spaces around it: the start of the language's own name, of its English name, of
   a word of either, or of its tag. */
static bool language_matches(const struct webprefs_language *l, const char *q) {
    return !*q || name_matches(l->own, q) || name_matches(l->english, q) || !strncasecmp(l->tag, q, strlen(q));
}

/* ---- the view ---- */

static void say(struct ui_msg *m, const char *text, const char *style, uint32_t colour) {
    ui_node(m, "text");
    ui_str(m, "text", text);
    if (style) ui_str(m, "style", style);
    if (colour) ui_num(m, "colour", colour);
    ui_num(m, "wrap", 1);
    ui_end(m);
}

static void card_head(struct ui_msg *m, const char *heading, const char *about) {
    ui_node(m, "column"); ui_num(m, "gap", 4);
        say(m, heading, "heading", 0);
        say(m, about, "caption", 0);
    ui_end(m);
}

static void tool(struct ui_msg *m, const char *icon, bool enabled, uint32_t what, size_t language) {
    uint32_t place = ui_node(m, "button");
    ui_str(m, "icon", icon); ui_str(m, "variant", "ghost"); ui_num(m, "compact", 1);
    if (!enabled) ui_num(m, "disabled", 1);
    ui_end(m);
    act(place, what, (uint32_t)language);
}

static uint32_t button(struct ui_msg *m, const char *text) {
    uint32_t place = ui_node(m, "button");
    ui_str(m, "text", text); ui_str(m, "variant", "secondary");
    ui_end(m);
    return place;
}

/* The header as websites receive it, broken after a comma wherever the next part would pass
   width: the value has no spaces, where alone the engine breaks a line. */
static void header_lines(const char *value, float width, char *out, size_t cap) {
    static const char name[] = "Accept-Language: ";
    float size = ui_theme.fontSize;
    size_t at = strlcpy(out, name, cap);
    if (at >= cap) return;
    float line = ui_text_width(name, sizeof name - 1, size, UI_TEXT_MONO);
    for (const char *p = value; *p;) {
        size_t n = strcspn(p, ",");
        if (p[n] == ',') n++;
        float w = ui_text_width(p, n, size, UI_TEXT_MONO);
        if (line > 0 && line + w > width && at + 1 < cap) {
            out[at++] = '\n';
            line = 0;
        }
        if (at + n >= cap) break;
        memcpy(out + at, p, n);
        at += n;
        line += w;
        p += n;
    }
    out[at] = 0;
}

static void languages_page(struct ui_msg *m) {
    const struct ui_theme *t = &ui_theme;
    read_languages();
    size_t n = chosen_tags();

    ui_node(m, "card"); ui_num(m, "gap", 12);
        card_head(m, "Preferred languages",
                  "Web pages are asked for their content in these languages, in this order. The first is the one "
                  "you want most.");
        for (size_t i = 0; i < chosen_count; i++) {
            const struct webprefs_language *l = &catalog[chosen[i]];
            char number[24], about[256];
            snprintf(number, sizeof number, "%zu", i + 1);
            snprintf(about, sizeof about, "%s · %s", l->english, l->tag);
            if (i) { ui_node(m, "divider"); ui_end(m); }
            ui_node(m, "row"); ui_num(m, "gap", 8); ui_str(m, "key", l->tag);
                ui_node(m, "text"); ui_str(m, "text", number); ui_num(m, "width", 22); ui_num(m, "colour", t->textDim);
                ui_end(m);
                ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 2);
                    ui_node(m, "text"); ui_str(m, "text", l->own); ui_num(m, "weight", 1); ui_end(m);
                    say(m, about, "caption", 0);
                ui_end(m);
                tool(m, "chevron-up", i > 0, ACT_UP, chosen[i]);
                tool(m, "chevron-down", i + 1 < chosen_count, ACT_DOWN, chosen[i]);
                tool(m, "close", chosen_count > 1, ACT_REMOVE, chosen[i]);
            ui_end(m);
        }
        ui_node(m, "row");
            act(button(m, "Restore default"), ACT_RESTORE, 0);
        ui_end(m);
    ui_end(m);

    char q[sizeof search];
    const char *from = search;
    while (*from == ' ') from++;
    size_t qn = strlcpy(q, from, sizeof q);
    while (qn && q[qn - 1] == ' ') q[--qn] = 0;
    bool unfit = false, any = false;
    for (size_t i = 0; i < catalog_count && !unfit; i++) {
        if (is_chosen(i, NULL) || !language_matches(&catalog[i], q)) continue;
        tags[n] = catalog[i].tag;
        unfit = !webprefs_languages_fit(tags, n + 1);
    }
    ui_node(m, "card"); ui_num(m, "gap", 12);
        card_head(m, "Add a language", "It is added at the end of the list.");
        act(ui_node(m, "field"), ACT_SEARCH, 0);
            ui_str(m, "value", search); ui_str(m, "placeholder", "Search languages"); ui_str(m, "width", "fill");
        ui_end(m);
        if (unfit) {
            char why[96];
            snprintf(why, sizeof why, "Websites accept at most %u bytes of languages.", WEBPREFS_ACCEPT_LANGUAGE_MAX);
            say(m, why, "caption", t->warning);
        }
        ui_node(m, "scroll"); ui_num(m, "maxHeight", 260); ui_num(m, "gap", 2); ui_str(m, "key", "choices");
        for (size_t i = 0; i < catalog_count; i++) {
            const struct webprefs_language *l = &catalog[i];
            if (is_chosen(i, NULL) || !language_matches(l, q)) continue;
            char label[256];
            if (!strcmp(l->own, l->english)) strlcpy(label, l->own, sizeof label);
            else snprintf(label, sizeof label, "%s — %s", l->own, l->english);
            tags[n] = l->tag;
            any = true;
            if (webprefs_languages_fit(tags, n + 1)) {
                act(ui_node(m, "item"), ACT_ADD, (uint32_t)i);
                    ui_str(m, "text", label); ui_str(m, "key", l->tag);
                ui_end(m);
            } else { /* an item looks the same disabled: this one says it cannot be chosen */
                ui_node(m, "row"); ui_num(m, "height", 38); ui_nums(m, "padding", (double[]){0, 14}, 2);
                ui_str(m, "align", "center"); ui_str(m, "key", l->tag);
                    ui_node(m, "text"); ui_str(m, "text", label); ui_num(m, "colour", t->textFaint); ui_end(m);
                ui_end(m);
            }
        }
        ui_end(m);
        if (!any) say(m, *q ? "No language matches." : "Every language listed is already chosen.", "caption", 0);
    ui_end(m);

    char value[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1], header[2 * WEBPREFS_ACCEPT_LANGUAGE_MAX + 64];
    n_result r = webprefs_accept_language(value, sizeof value);
    float width = (float)(win_w - SIDEBAR - 2 * PAGE_PAD - 2 * CARD_PAD - 8);
    if (r >= 0) header_lines(value, width > 120 ? width : 120, header, sizeof header);
    else snprintf(header, sizeof header, "It could not be made: %s.", n_result_text(r));
    ui_node(m, "card"); ui_num(m, "gap", 12);
        card_head(m, "What websites receive",
                  "Sent with every request, unless the page sets its own. Scripts read the same list in "
                  "navigator.languages.");
        ui_node(m, "text"); ui_str(m, "text", header); ui_num(m, "mono", 1); ui_num(m, "wrap", 1);
        if (r < 0) ui_num(m, "colour", t->warning);
        ui_end(m);
    ui_end(m);
}

static void privacy_page(struct ui_msg *m) {
    const struct ui_theme *t = &ui_theme;
    bool on = webprefs_search_history();
    char stored[1200];
    const char *profile = web_profile_folder(false);
    snprintf(stored, sizeof stored, "Stored only in %s; Clear removes every entry.",
             profile ? profile : "the browser's profile folder");
    const char *const lines[] = {
        "Stores typed search fields, separated by site and field name.",
        stored,
        "On a search field: Down opens suggestions; Enter chooses.",
        "Never type secrets in a search. Clear also disables history.",
    };
    ui_node(m, "card"); ui_num(m, "gap", 14);
        ui_node(m, "row"); ui_num(m, "gap", 16);
            ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 4);
                say(m, "Search history", "heading", 0);
                say(m, on ? "Search history is on." : "Search history is off, the default. Enable only for non-secret "
                            "searches.", "caption", on ? t->success : 0);
            ui_end(m);
            act(ui_node(m, "toggle"), ACT_HISTORY, 0);
                ui_num(m, "on", on);
            ui_end(m);
        ui_end(m);
        for (size_t i = 0; i < sizeof lines / sizeof *lines; i++) say(m, lines[i], NULL, t->textDim);
        ui_node(m, "row");
            act(button(m, "Clear and disable"), ACT_CLEAR, 0);
        ui_end(m);
    ui_end(m);
}

static void show(void) {
    const struct ui_theme *t = &ui_theme;
    building = &sent[(views + 1) % VIEWS];
    building->view = building->count = 0;
    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m, "row"); ui_num(&m, "gap", 0); ui_str(&m, "align", "stretch");
        ui_node(&m, "column"); ui_num(&m, "width", SIDEBAR); ui_nums(&m, "padding", (double[]){22, 12}, 2);
        ui_num(&m, "gap", 4); ui_num(&m, "background", t->sidebar);
            ui_node(&m, "row"); ui_num(&m, "gap", 10); ui_nums(&m, "padding", (double[]){0, 10, 18, 10}, 4);
                ui_node(&m, "icon"); ui_str(&m, "icon", "settings"); ui_num(&m, "size", 22); ui_num(&m, "colour", t->accent);
                ui_end(&m);
                ui_node(&m, "text"); ui_str(&m, "text", "Browser"); ui_str(&m, "style", "title"); ui_num(&m, "size", 19);
                ui_end(&m);
            ui_end(&m);
            for (int p = 0; p < PAGES; p++) {
                act(ui_node(&m, "item"), ACT_PAGE, (uint32_t)p);
                    ui_str(&m, "text", pages[p].label); ui_str(&m, "icon", pages[p].icon);
                    ui_num(&m, "selected", p == page); ui_str(&m, "key", pages[p].label);
                ui_end(&m);
            }
        ui_end(&m);
        ui_node(&m, "column"); ui_num(&m, "grow", 1); ui_num(&m, "gap", 0);
            ui_node(&m, "scroll"); ui_num(&m, "grow", 1); ui_nums(&m, "padding", (double[]){26, PAGE_PAD}, 2);
            ui_num(&m, "gap", 14); ui_str(&m, "key", pages[page].label);
                ui_node(&m, "text"); ui_str(&m, "text", pages[page].label); ui_str(&m, "style", "title"); ui_end(&m);
                if (page == PAGE_LANGUAGES) languages_page(&m);
                else privacy_page(&m);
            ui_end(&m);
            ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){12, PAGE_PAD}, 2); ui_num(&m, "background", t->sidebar);
                say(&m, status[0] ? status : watched ? "Changes apply at once in every browser window and are kept."
                        : "Changes apply at once. This window cannot watch the browser's preferences, so a change "
                          "made elsewhere shows here when it is opened again.",
                    "caption", status_bad || (!status[0] && !watched) ? t->warning : 0);
            ui_end(&m);
        ui_end(&m);
    ui_end(&m);
    if (ui_send(session, &m) >= 0) building->view = ++views;
}

/* One event; whether the view changed. */
static bool handle(const struct ui_event *e, bool *running) {
    struct action a = action_of(e);
    switch (e->type) {
    case UI_EVENT_CLOSE:
        *running = false;
        return false;
    case UI_EVENT_RESIZE:
        win_w = (int)e->a;
        win_h = (int)e->b;
        return true;
    case UI_EVENT_KEY:
        if (e->flags & UI_UP) return false;
        if (!strcmp(e->text, "Escape")) {
            *running = false;
            return false;
        }
        if (search_focused || (strcmp(e->text, "ArrowUp") && strcmp(e->text, "ArrowDown"))) return false;
        page = (page + (!strcmp(e->text, "ArrowDown") ? 1 : PAGES - 1)) % PAGES;
        status[0] = 0;
        status_bad = false;
        return true;
    case UI_EVENT_FOCUS:
        if (a.act == ACT_SEARCH) search_focused = e->a != 0;
        return false;
    case UI_EVENT_TEXT: {
        if (a.act != ACT_SEARCH) return false;
        size_t n = strlcpy(search, e->text, sizeof search);
        if (n >= sizeof search) { /* cut: not inside a character */
            n = sizeof search - 1;
            while (n && ((unsigned char)search[n - 1] & 0xC0) == 0x80) n--;
            if (n && (unsigned char)search[n - 1] >= 0xC0) n--;
            search[n] = 0;
        }
        return true;
    }
    case UI_EVENT_TOGGLE:
        if (a.act != ACT_HISTORY) return false;
        set_history(e->a != 0);
        return true;
    case UI_EVENT_CLICK:
        switch (a.act) {
        case ACT_PAGE:
            if ((int)a.arg == page) return false;
            page = (int)a.arg;
            search_focused = false;
            status[0] = 0;
            status_bad = false;
            return true;
        case ACT_UP: move_language(a.arg, -1); return true;
        case ACT_DOWN: move_language(a.arg, 1); return true;
        case ACT_REMOVE: remove_language(a.arg); return true;
        case ACT_ADD: add_language(a.arg); return true;
        case ACT_RESTORE: saved(webprefs_reset_languages()); return true;
        case ACT_CLEAR: clear_history(); return true;
        }
        return false;
    }
    return false;
}

int main(void) {
    catalog = webprefs_catalog(&catalog_count);
    chosen = calloc(catalog_count + 1, sizeof *chosen);
    tags = calloc(catalog_count + 1, sizeof *tags);
    session = ui_session_new();
    if (!chosen || !tags || !session ||
        ui_window(session, WINDOW, win_w, win_h, WIN_RESIZABLE | WIN_CENTER, "Browser settings", &win_w, &win_h) < 0) {
        fprintf(stderr, "browsersettings: the window could not be opened\n");
        ui_session_free(session);
        return 1;
    }
    n_handle queue = ui_session_queue(session);
    /* The watch first, then the first view reads the preferences: no change between is missed. */
    watched = webprefs_watch(queue) >= 0;
    show();
    for (bool running = true; running;) {
        struct n_event events[64];
        n_result n = n_events_wait(queue, events, 64, ui_session_timeout(session));
        if (n == -N_ERR_TIMEOUT) n = 0;
        if (n < 0) {
            fprintf(stderr, "browsersettings: waiting failed: %s\n", n_result_text(n));
            break;
        }
        bool changed = false;
        for (n_result i = 0; i < n; i++) changed |= events[i].token == WEBPREFS_TOKEN;
        webprefs_events(events, (size_t)n);
        ui_session_pump(session);
        struct ui_event e;
        char text[2 * sizeof search]; /* longer than search: a value too long for it is seen and cut there */
        while (ui_next_event(session, &e, text, sizeof text)) changed |= handle(&e, &running);
        if (!running || !ui_session_open(session)) break;
        if (changed) show();
    }
    ui_session_free(session);
    return 0;
}
