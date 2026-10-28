/* The browser's settings: a page of the browser's own in a tab (about:settings), not a web
   document. It has no realm and no script; only the browser's UI opens it (the menu, the
   address field, the session history). Sections on the left, a search over every section at
   the top, cards with what each setting does. It keeps no setting of its own: each click
   changes the browser's preferences (webprefs.h) at once, which every window and worker of
   the browser hears of through its own watch; a change made elsewhere shows here at once.
   Included once by browser.c. */
#include <uchar.h>
#include <wctype.h>
#include <webprefs.h>
#include <websitedata.h>

#define SETTINGS_URL "about:settings"
#define SETTINGS_SIDEBAR 240
#define SETTINGS_PAD 32

enum { SECTION_LANGUAGES, SECTION_PRIVACY, SECTIONS };
static const struct { const char *label, *icon; } settings_sections[SECTIONS] = {
    [SECTION_LANGUAGES] = {"言語", "language"},
    [SECTION_PRIVACY] = {"プライバシーとセキュリティ", "lock"},
};

/* A settings page: the section it shows, what its search fields hold, what the last change did. */
struct settings {
    int section;
    char find[256];   /* the search over every section */
    char search[512]; /* the search for a language to add */
    char site_find[256]; /* the search among the sites with data */
    char exception[512]; /* the site typed for a new exception */
    char status[256]; /* empty: no change made yet */
    bool status_bad;
    bool confirm_clear; /* "clear everything" was pressed once: the card asks again */
    /* The names a view's controls carry (a site, an exception's host): a control names its own by
       its place here, which a later view does not give to another name. */
    char **names;
    uint32_t names_n, names_cap;
};

/* What a widget of a view does when it is used; events name a widget by its place in a view.
   A language's controls carry the language (its place in the catalog), not its row, so that a
   click on a view whose list changed since acts on the language that was clicked. */
enum { SET_NONE, SET_SECTION, SET_FIND, SET_UP, SET_DOWN, SET_REMOVE, SET_ADD, SET_RESTORE, SET_SEARCH,
       SET_HISTORY, SET_CLEAR_HISTORY, SET_SITE_FIND, SET_CLEAR_SITE, SET_CLEAR_ALL, SET_CLEAR_ALL_SURE,
       SET_CLEAR_ALL_CANCEL, SET_CLEAR_ON_CLOSE, SET_EXCEPTION_HOST, SET_EXCEPTION_ADD, SET_EXCEPTION_REMOVE };
struct settings_action { uint32_t act, arg; };
/* The actions of the views sent lately, by view (the ring of the window's views). */
#define SETTINGS_VIEWS 32
static struct settings_sent { uint32_t view, count, cap; struct settings_action *at; } settings_sent[SETTINGS_VIEWS];
static struct settings_sent *settings_building;

static void settings_act_at(uint32_t place, uint32_t what, uint32_t arg) {
    struct settings_sent *v = settings_building;
    if (!v) return;
    if (place >= v->cap) {
        uint32_t cap = v->cap ? v->cap : 64;
        while (cap <= place) cap *= 2;
        struct settings_action *grown = realloc(v->at, cap * sizeof *grown);
        if (!grown) return; /* the widget does nothing */
        v->at = grown;
        v->cap = cap;
    }
    if (place >= v->count) {
        memset(v->at + v->count, 0, (place - v->count) * sizeof *v->at);
        v->count = place + 1;
    }
    v->at[place] = (struct settings_action){what, arg};
}
/* The view went to the window: its widgets' events can be read now. */
static void settings_commit(uint32_t view) {
    struct settings_sent *v = &settings_sent[view % SETTINGS_VIEWS];
    if (v == settings_building) v->view = view;
    settings_building = NULL;
}
static struct settings_action settings_action_of(uint32_t view, uint32_t node) {
    const struct settings_sent *v = &settings_sent[view % SETTINGS_VIEWS];
    if (!view || v->view != view || node >= v->count) return (struct settings_action){SET_NONE, 0};
    return v->at[node];
}

static struct settings *settings_new(void) { return calloc(1, sizeof(struct settings)); }
static void settings_free(struct settings *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->names_n; i++) free(s->names[i]);
    free(s->names);
    free(s);
}
/* The place of name in the page's names, added if new; UINT32_MAX without memory. */
static uint32_t settings_name(struct settings *s, const char *name) {
    for (uint32_t i = 0; i < s->names_n; i++) if (!strcmp(s->names[i], name)) return i;
    if (s->names_n == s->names_cap) {
        uint32_t cap = s->names_cap ? s->names_cap * 2 : 32;
        char **grown = cap > s->names_cap ? realloc(s->names, cap * sizeof *grown) : NULL;
        if (!grown) return UINT32_MAX;
        s->names = grown; s->names_cap = cap;
    }
    if (!(s->names[s->names_n] = strdup(name))) return UINT32_MAX;
    return s->names_n++;
}
static const char *settings_name_at(const struct settings *s, uint32_t at) { return at < s->names_n ? s->names[at] : NULL; }
static bool settings_url(const char *url) { return url && !strcasecmp(url, SETTINGS_URL); }

/* ---- the browser's languages ---- */

static const struct webprefs_language *settings_catalog;
static size_t settings_catalog_count;
static size_t *settings_chosen, settings_chosen_count; /* the languages now, most wanted first: places in the catalog */
static const char **settings_tags;                     /* room for the chosen tags and one more */

static bool settings_languages_ready(void) {
    if (settings_tags) return true;
    settings_catalog = webprefs_catalog(&settings_catalog_count);
    settings_chosen = calloc(settings_catalog_count + 1, sizeof *settings_chosen);
    settings_tags = calloc(settings_catalog_count + 1, sizeof *settings_tags);
    if (settings_chosen && settings_tags) return true;
    free(settings_chosen); free(settings_tags); settings_chosen = NULL; settings_tags = NULL;
    return false;
}

/* Reads the browser's languages again into settings_chosen. */
static void settings_read_languages(struct settings *s) {
    char list[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1]; /* the list is shorter than its Accept-Language */
    settings_chosen_count = 0;
    n_result r = webprefs_languages(list, sizeof list);
    if (r < 0) {
        snprintf(s->status, sizeof s->status, "ブラウザーの言語を読めませんでした：%s", n_result_text(r));
        s->status_bad = true;
        return;
    }
    for (char *p = list, *end; *p; p = *end ? end + 1 : end) {
        end = p + strcspn(p, ",");
        size_t n = (size_t)(end - p);
        for (size_t i = 0; i < settings_catalog_count; i++)
            if (strlen(settings_catalog[i].tag) == n && !strncasecmp(settings_catalog[i].tag, p, n)) {
                settings_chosen[settings_chosen_count++] = i;
                break;
            }
    }
}
/* The chosen tags in settings_tags; their count. */
static size_t settings_chosen_tags(void) {
    for (size_t i = 0; i < settings_chosen_count; i++) settings_tags[i] = settings_catalog[settings_chosen[i]].tag;
    return settings_chosen_count;
}
static bool settings_is_chosen(size_t language, size_t *at) {
    for (size_t i = 0; i < settings_chosen_count; i++)
        if (settings_chosen[i] == language) {
            if (at) *at = i;
            return true;
        }
    return false;
}
/* After a change: what it did, and the preferences as they are now (the page shows those). */
static void settings_saved(struct settings *s, n_result r) {
    if (r >= 0) { s->status[0] = 0; s->status_bad = false; }
    else {
        snprintf(s->status, sizeof s->status, "ブラウザーの設定を保存できませんでした：%s", n_result_text(r));
        s->status_bad = true;
    }
    webprefs_sync();
}
/* A change acts on the list as it is now, a change another process made included. */
static void settings_languages_now(struct settings *s) {
    webprefs_sync();
    settings_read_languages(s);
}
static void settings_move_language(struct settings *s, size_t language, int by) {
    settings_languages_now(s);
    size_t i, n = settings_chosen_tags();
    if (!settings_is_chosen(language, &i) || (by < 0 && !i) || (by > 0 && i + 1 >= n)) return;
    const char *t = settings_tags[i];
    settings_tags[i] = settings_tags[i + by];
    settings_tags[i + by] = t;
    settings_saved(s, webprefs_set_languages(settings_tags, n));
}
static void settings_remove_language(struct settings *s, size_t language) {
    settings_languages_now(s);
    size_t i, n = settings_chosen_tags();
    if (n < 2 || !settings_is_chosen(language, &i)) return; /* the last one stays */
    memmove(settings_tags + i, settings_tags + i + 1, (n - i - 1) * sizeof *settings_tags);
    settings_saved(s, webprefs_set_languages(settings_tags, n - 1));
}
static void settings_add_language(struct settings *s, size_t language) {
    settings_languages_now(s);
    size_t n = settings_chosen_tags();
    if (language >= settings_catalog_count || settings_is_chosen(language, NULL)) return;
    settings_tags[n] = settings_catalog[language].tag;
    if (!webprefs_languages_fit(settings_tags, n + 1)) {
        snprintf(s->status, sizeof s->status, "ウェブサイトが受け取る言語の一覧は %u バイトまでです。", WEBPREFS_ACCEPT_LANGUAGE_MAX);
        s->status_bad = true;
        return;
    }
    settings_saved(s, webprefs_set_languages(settings_tags, n + 1));
}

/* ---- the sites' data ---- */
static void settings_trimmed(const char *text, char *out, size_t cap);

/* The sites with data (websitedata.h) as they were last read, and when: reading walks the
   profile folder, so a view reads them again only when they may have changed (after the page's
   own change) or a second later. */
static struct websitedata_site *settings_sites_list;
static size_t settings_sites_count;
static uint64_t settings_sites_at;
static bool settings_sites_stale = true;
static n_result settings_sites_failure;

static void settings_read_sites(void) {
    uint64_t now = n_clock_uptime_ms();
    if (!settings_sites_stale && now - settings_sites_at < 1000) return;
    settings_sites_stale = false; settings_sites_at = now;
    struct websitedata_site *rows = NULL;
    long n = websitedata_sites(network, storage, &rows);
    settings_sites_failure = n < 0 ? (n_result)n : 0;
    if (n < 0) return; /* the last list stays, with the failure said */
    free(settings_sites_list);
    settings_sites_list = rows; settings_sites_count = (size_t)n;
}
/* bytes as the page shows them. */
static void settings_bytes(uint64_t bytes, char *out, size_t cap) {
    if (bytes < 1024) snprintf(out, cap, "%u バイト", (unsigned)bytes);
    else if (bytes < 1024 * 1024) snprintf(out, cap, "%.1f KB", bytes / 1024.0);
    else if (bytes < UINT64_C(1024) * 1024 * 1024) snprintf(out, cap, "%.1f MB", bytes / (1024.0 * 1024));
    else snprintf(out, cap, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
}
/* The data of site (NULL: of every site) leaves every store. */
static void settings_clear(struct settings *s, const char *site) {
    n_result r = websitedata_clear(network, storage, site);
    settings_sites_stale = true;
    s->status_bad = r < 0;
    if (r == -N_ERR_BUSY)
        strlcpy(s->status, "使用中のデータベースかファイルが残りました。頁を閉じてから、もう一度消してください。", sizeof s->status);
    else if (r < 0)
        snprintf(s->status, sizeof s->status, "消し切れませんでした：%s", n_result_text(r));
    else if (site) snprintf(s->status, sizeof s->status, "%s の Cookie とサイトデータを消しました。", *site ? site : "持ち主の分からないデータ");
    else strlcpy(s->status, "すべての Cookie とサイトデータを消しました。", sizeof s->status);
}
static const char *settings_rule_label(enum webprefs_site_rule rule) {
    switch (rule) {
    case WEBPREFS_SITE_ALLOW: return "許可（閉じても残す）";
    case WEBPREFS_SITE_SESSION: return "閉じたら消す";
    case WEBPREFS_SITE_BLOCK: return "ブロック";
    default: return "";
    }
}
static void settings_add_exception(struct settings *s, enum webprefs_site_rule rule) {
    char host[WEBPREFS_HOST_MAX + 1];
    char typed[sizeof s->exception];
    settings_trimmed(s->exception, typed, sizeof typed);
    if (webprefs_site_host(typed, host) < 0) {
        strlcpy(s->status, "サイトのアドレス（例 example.com や https://www.example.com/）を入れてください。", sizeof s->status);
        s->status_bad = true;
        return;
    }
    n_result r = webprefs_set_site_rule(host, rule);
    settings_saved(s, r);
    if (r >= 0) {
        snprintf(s->status, sizeof s->status, "%s：%s", host, settings_rule_label(rule));
        s->exception[0] = 0;
    }
}

/* ---- matching what is typed ---- */

/* The next character of *p in lower case (Unicode's simple mapping), past it; 0 at the end or
   at bytes that are not UTF-8 (then *p stays). */
static char32_t settings_next_folded(const char **p) {
    mbstate_t state = {0};
    char32_t c;
    size_t n = mbrtoc32(&c, *p, strnlen(*p, 4), &state);
    if (!n || n > 4) return 0;
    *p += n;
    return (char32_t)towlower((wint_t)c);
}
/* Whether text begins with q, case aside. */
static bool settings_begins(const char *text, const char *q) {
    while (*q) {
        char32_t b = settings_next_folded(&q), a = settings_next_folded(&text);
        if (!b || a != b) return false;
    }
    return true;
}
/* Whether q begins name or one of its words. */
static bool settings_word_matches(const char *name, const char *q) {
    bool start = true;
    for (const char *p = name; *p;) {
        if (start && settings_begins(p, q)) return true;
        char32_t c = settings_next_folded(&p);
        if (!c) return false;
        start = !iswalnum((wint_t)c);
    }
    return false;
}
/* Whether q is found anywhere in text, case aside (Japanese has no spaces between words). */
static bool settings_contains(const char *text, const char *q) {
    for (const char *p = text; *p;) {
        if (settings_begins(p, q)) return true;
        if (!settings_next_folded(&p)) return false;
    }
    return !*q;
}
/* q, without the spaces around it: the start of the language's own name, of its English name,
   of a word of either, or of its tag. */
static bool settings_language_matches(const struct webprefs_language *l, const char *q) {
    return !*q || settings_word_matches(l->own, q) || settings_word_matches(l->english, q) ||
           !strncasecmp(l->tag, q, strlen(q));
}
/* text without the spaces around it, in out. */
static void settings_trimmed(const char *text, char *out, size_t cap) {
    while (*text == ' ') text++;
    size_t n = strlcpy(out, text, cap);
    if (n >= cap) n = cap - 1;
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

/* ---- the view ---- */

static void settings_say(struct ui_msg *m, const char *text, const char *style, uint32_t colour) {
    ui_node(m, "text");
    ui_str(m, "text", text);
    if (style) ui_str(m, "style", style);
    if (colour) ui_num(m, "colour", colour);
    ui_num(m, "wrap", 1);
    ui_end(m);
}
static void settings_card_head(struct ui_msg *m, const char *heading, const char *about) {
    ui_node(m, "column"); ui_num(m, "gap", 4);
        settings_say(m, heading, "heading", 0);
        if (about) settings_say(m, about, "caption", 0);
    ui_end(m);
}
static void settings_tool(struct ui_msg *m, const char *icon, bool enabled, uint32_t what, size_t arg) {
    uint32_t place = ui_node(m, "button");
    ui_str(m, "icon", icon); ui_str(m, "variant", "ghost"); ui_num(m, "compact", 1);
    if (!enabled) ui_num(m, "disabled", 1);
    ui_end(m);
    settings_act_at(place, what, (uint32_t)arg);
}
static void settings_button(struct ui_msg *m, const char *text, const char *icon, bool enabled, uint32_t what, uint32_t arg) {
    uint32_t place = ui_node(m, "button");
    ui_str(m, "text", text); ui_str(m, "variant", "secondary");
    if (icon) ui_str(m, "icon", icon);
    if (!enabled) ui_num(m, "disabled", 1);
    ui_end(m);
    settings_act_at(place, what, arg);
}

/* The header as websites receive it, broken after a comma wherever the next part would pass
   width: the value has no spaces, where alone the engine breaks a line. */
static void settings_header_lines(const char *value, float width, char *out, size_t cap) {
    static const char name[] = "Accept-Language: ";
    float size = ui_theme.fontSize;
    size_t at = strlcpy(out, name, cap);
    if (at >= cap) return;
    float line = ui_text_width(name, sizeof name - 1, size, UI_TEXT_MONO);
    for (const char *p = value; *p;) {
        size_t n = strcspn(p, ",");
        if (p[n] == ',') n++;
        float w = ui_text_width(p, n, size, UI_TEXT_MONO);
        if (line > 0 && line + w > width && at + 1 < cap) { out[at++] = '\n'; line = 0; }
        if (at + n >= cap) break;
        memcpy(out + at, p, n);
        at += n;
        line += w;
        p += n;
    }
    out[at] = 0;
}

/* The cards: each in a section, with the words the search over every section looks in. */
static void settings_card_languages(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)width;
    ui_node(m, "card"); ui_num(m, "gap", 12); ui_str(m, "key", "languages");
        settings_card_head(m, "優先する言語",
                           "ウェブページの内容を、この順で求めます。一番上が最も望む言語です。");
        for (size_t i = 0; i < settings_chosen_count; i++) {
            const struct webprefs_language *l = &settings_catalog[settings_chosen[i]];
            char number[24], about[256];
            snprintf(number, sizeof number, "%zu", i + 1);
            snprintf(about, sizeof about, "%s · %s", l->english, l->tag);
            if (i) { ui_node(m, "divider"); ui_end(m); }
            ui_node(m, "row"); ui_num(m, "gap", 8); ui_str(m, "key", l->tag);
                ui_node(m, "text"); ui_str(m, "text", number); ui_num(m, "width", 22); ui_num(m, "colour", t->textDim);
                ui_end(m);
                ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 2);
                    ui_node(m, "text"); ui_str(m, "text", l->own); ui_num(m, "weight", 1); ui_end(m);
                    settings_say(m, about, "caption", 0);
                ui_end(m);
                settings_tool(m, "chevron-up", i > 0, SET_UP, settings_chosen[i]);
                settings_tool(m, "chevron-down", i + 1 < settings_chosen_count, SET_DOWN, settings_chosen[i]);
                settings_tool(m, "close", settings_chosen_count > 1, SET_REMOVE, settings_chosen[i]);
            ui_end(m);
        }
        ui_node(m, "row");
            settings_button(m, "既定に戻す", NULL, !webprefs_languages_default(), SET_RESTORE, 0);
        ui_end(m);
    ui_end(m);
}
static void settings_card_add_language(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)width;
    size_t n = settings_chosen_tags();
    char q[sizeof s->search];
    settings_trimmed(s->search, q, sizeof q);
    bool unfit = false, any = false;
    for (size_t i = 0; i < settings_catalog_count && !unfit; i++) {
        if (settings_is_chosen(i, NULL) || !settings_language_matches(&settings_catalog[i], q)) continue;
        settings_tags[n] = settings_catalog[i].tag;
        unfit = !webprefs_languages_fit(settings_tags, n + 1);
    }
    ui_node(m, "card"); ui_num(m, "gap", 12); ui_str(m, "key", "add-language");
        settings_card_head(m, "言語を追加", "一覧の最後に加わります。");
        settings_act_at(ui_node(m, "field"), SET_SEARCH, 0);
            ui_str(m, "value", s->search); ui_str(m, "placeholder", "言語を探す"); ui_str(m, "width", "fill");
        ui_end(m);
        if (unfit) {
            char why[128];
            snprintf(why, sizeof why, "ウェブサイトが受け取る言語の一覧は %u バイトまでです。", WEBPREFS_ACCEPT_LANGUAGE_MAX);
            settings_say(m, why, "caption", t->warning);
        }
        ui_node(m, "scroll"); ui_num(m, "maxHeight", 260); ui_num(m, "gap", 2); ui_str(m, "key", "choices");
        for (size_t i = 0; i < settings_catalog_count; i++) {
            const struct webprefs_language *l = &settings_catalog[i];
            if (settings_is_chosen(i, NULL) || !settings_language_matches(l, q)) continue;
            char label[256];
            if (!strcmp(l->own, l->english)) strlcpy(label, l->own, sizeof label);
            else snprintf(label, sizeof label, "%s — %s", l->own, l->english);
            settings_tags[n] = l->tag;
            any = true;
            if (webprefs_languages_fit(settings_tags, n + 1)) {
                settings_act_at(ui_node(m, "item"), SET_ADD, (uint32_t)i);
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
        if (!any) settings_say(m, *q ? "一致する言語はありません。" : "一覧の言語はすべて選ばれています。", "caption", 0);
    ui_end(m);
}
static void settings_card_sent_languages(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)s;
    char value[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1], header[2 * WEBPREFS_ACCEPT_LANGUAGE_MAX + 64];
    n_result r = webprefs_accept_language(value, sizeof value);
    if (r >= 0) settings_header_lines(value, width > 120 ? width : 120, header, sizeof header);
    else snprintf(header, sizeof header, "作れませんでした：%s", n_result_text(r));
    ui_node(m, "card"); ui_num(m, "gap", 12); ui_str(m, "key", "sent-languages");
        settings_card_head(m, "ウェブサイトが受け取るもの",
                           "頁が自分で決めない限り、すべての要求に付きます。スクリプトは同じ一覧を "
                           "navigator.languages で読みます。");
        ui_node(m, "text"); ui_str(m, "text", header); ui_num(m, "mono", 1); ui_num(m, "wrap", 1);
        if (r < 0) ui_num(m, "colour", t->warning);
        ui_end(m);
    ui_end(m);
}
static void settings_card_search_history(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)s; (void)width;
    bool on = webprefs_search_history();
    char stored[1200];
    const char *profile = web_profile_folder(false);
    snprintf(stored, sizeof stored, "保存先は %s だけです。「消去して止める」で一つ残らず消えます。",
             profile ? profile : "ブラウザーの profile のフォルダー");
    const char *const lines[] = {
        "検索欄に打った語を、サイトと欄の名ごとに分けて残します。",
        stored,
        "検索欄で ↓ を押すと候補が開き、Enter で選びます。",
        "秘密の語は検索欄に打たないでください。消去すると履歴も止まります。",
    };
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", "search-history");
        ui_node(m, "row"); ui_num(m, "gap", 16);
            ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 4);
                settings_say(m, "検索の履歴", "heading", 0);
                settings_say(m, on ? "検索の履歴を残しています。" : "検索の履歴は残していません（既定）。秘密でない検索のときだけ有効にしてください。",
                             "caption", on ? t->success : 0);
            ui_end(m);
            settings_act_at(ui_node(m, "toggle"), SET_HISTORY, 0);
                ui_num(m, "on", on);
            ui_end(m);
        ui_end(m);
        for (size_t i = 0; i < sizeof lines / sizeof *lines; i++) settings_say(m, lines[i], NULL, t->textDim);
        ui_node(m, "row");
            settings_button(m, "消去して止める", NULL, true, SET_CLEAR_HISTORY, 0);
        ui_end(m);
    ui_end(m);
}

static void settings_card_site_data(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)width;
    settings_read_sites();
    size_t cookies = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < settings_sites_count; i++) { cookies += settings_sites_list[i].cookies; total += websitedata_bytes(&settings_sites_list[i]); }
    char a[32], said[256];
    settings_bytes(total, a, sizeof a);
    snprintf(said, sizeof said, "%zu のサイトが、Cookie %zu 件を含めて %s を保存しています"
             "（Cookie・Web Storage・IndexedDB・OPFS・Cache Storage。HTTP のキャッシュは含まない）。", settings_sites_count, cookies, a);
    bool on_close = webprefs_clear_on_close();
    ui_node(m, "card"); ui_num(m, "gap", 14); ui_str(m, "key", "site-data");
        settings_card_head(m, "Cookie とサイトデータ", said);
        if (settings_sites_failure < 0) {
            char why[160];
            snprintf(why, sizeof why, "サイトのデータを読めませんでした：%s", n_result_text(settings_sites_failure));
            settings_say(m, why, "caption", t->warning);
        }
        if (!s->confirm_clear) {
            ui_node(m, "row");
                settings_button(m, "すべて消去…", "trash", settings_sites_count != 0, SET_CLEAR_ALL, 0);
            ui_end(m);
        } else {
            settings_say(m, "すべてのサイトの Cookie・Web Storage・IndexedDB・OPFS・Cache Storage を消します。ログインしているサイトからは"
                            "ログアウトし、開いている頁は空の保存を読みます。元に戻せません。", NULL, t->warning);
            ui_node(m, "row"); ui_num(m, "gap", 8);
                settings_button(m, "すべて消去する", "trash", true, SET_CLEAR_ALL_SURE, 0);
                settings_button(m, "やめる", NULL, true, SET_CLEAR_ALL_CANCEL, 0);
            ui_end(m);
        }
        ui_node(m, "divider"); ui_end(m);
        ui_node(m, "row"); ui_num(m, "gap", 16);
            ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 4);
                settings_say(m, "ブラウザーを閉じたら Cookie とサイトデータを消す", NULL, 0);
                settings_say(m, "最後の窓を閉じたとき、「許可」の例外のサイトを除いて消えます。", "caption", 0);
            ui_end(m);
            settings_act_at(ui_node(m, "toggle"), SET_CLEAR_ON_CLOSE, 0);
                ui_num(m, "on", on_close);
            ui_end(m);
        ui_end(m);
    ui_end(m);
}
static void settings_card_sites(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)width;
    settings_read_sites();
    char q[sizeof s->site_find];
    settings_trimmed(s->site_find, q, sizeof q);
    ui_node(m, "card"); ui_num(m, "gap", 12); ui_str(m, "key", "sites");
        settings_card_head(m, "サイトごとのデータ", "サイト（登録できるドメイン）ごとに、そのサブドメインの分も合わせて数えます。");
        settings_act_at(ui_node(m, "field"), SET_SITE_FIND, 0);
            ui_str(m, "value", s->site_find); ui_str(m, "placeholder", "サイトを探す"); ui_str(m, "width", "fill");
        ui_end(m);
        bool any = false;
        ui_node(m, "scroll"); ui_num(m, "maxHeight", 360); ui_num(m, "gap", 2); ui_str(m, "key", "site-rows");
        for (size_t i = 0; i < settings_sites_count; i++) {
            const struct websitedata_site *r = &settings_sites_list[i];
            const char *name = r->site[0] ? r->site : "持ち主の分からないデータ";
            if (*q && !settings_contains(name, q)) continue;
            char about[320], part[5][32];
            settings_bytes(r->cookie_bytes, part[0], sizeof part[0]);
            settings_bytes(r->storage_bytes, part[1], sizeof part[1]);
            settings_bytes(r->indexeddb_bytes, part[2], sizeof part[2]);
            settings_bytes(r->opfs_bytes, part[3], sizeof part[3]);
            settings_bytes(r->cache_bytes, part[4], sizeof part[4]);
            snprintf(about, sizeof about, "Cookie %zu 件（%s） · Web Storage %s · IndexedDB %s · OPFS %s · Cache Storage %s",
                     r->cookies, part[0], part[1], part[2], part[3], part[4]);
            if (any) { ui_node(m, "divider"); ui_end(m); }
            any = true;
            ui_node(m, "row"); ui_num(m, "gap", 8); ui_str(m, "align", "center"); ui_str(m, "key", r->site[0] ? r->site : "-");
            ui_nums(m, "padding", (double[]){4, 4}, 2);
                ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 2);
                    ui_node(m, "text"); ui_str(m, "text", name); ui_num(m, "weight", 1); ui_end(m);
                    settings_say(m, about, "caption", 0);
                ui_end(m);
                /* The data of no known origin (OPFS, Cache Storage) goes only with everything ("" clears every site). */
                uint32_t place = r->site[0] ? settings_name(s, r->site) : UINT32_MAX;
                settings_tool(m, "trash", place != UINT32_MAX, SET_CLEAR_SITE, place);
            ui_end(m);
        }
        ui_end(m);
        if (!any) settings_say(m, *q ? "一致するサイトはありません。" : "データを保存しているサイトはありません。", "caption", t->textDim);
    ui_end(m);
}
static void settings_card_exceptions(struct ui_msg *m, struct settings *s, float width) {
    const struct ui_theme *t = &ui_theme;
    (void)width;
    size_t count = webprefs_site_exceptions(NULL, 0);
    struct webprefs_site_exception *list = count ? calloc(count, sizeof *list) : NULL;
    if (list) count = MIN(count, webprefs_site_exceptions(list, count));
    else count = 0;
    ui_node(m, "card"); ui_num(m, "gap", 12); ui_str(m, "key", "exceptions");
        settings_card_head(m, "例外", "サイトごとに、Cookie とサイトデータを許すか、閉じたら消すか、"
                                      "ブロックするかを決めます。サブドメインにも効き、より詳しい方が勝ちます。");
        settings_act_at(ui_node(m, "field"), SET_EXCEPTION_HOST, 0);
            ui_str(m, "value", s->exception); ui_str(m, "placeholder", "サイトのアドレス（example.com）"); ui_str(m, "width", "fill");
        ui_end(m);
        ui_node(m, "row"); ui_num(m, "gap", 8);
            settings_button(m, "許可", NULL, s->exception[0] != 0, SET_EXCEPTION_ADD, WEBPREFS_SITE_ALLOW);
            settings_button(m, "閉じたら消す", NULL, s->exception[0] != 0, SET_EXCEPTION_ADD, WEBPREFS_SITE_SESSION);
            settings_button(m, "ブロック", NULL, s->exception[0] != 0, SET_EXCEPTION_ADD, WEBPREFS_SITE_BLOCK);
        ui_end(m);
        for (size_t i = 0; i < count; i++) {
            ui_node(m, "divider"); ui_end(m);
            ui_node(m, "row"); ui_num(m, "gap", 8); ui_str(m, "align", "center"); ui_str(m, "key", list[i].host);
                ui_node(m, "column"); ui_num(m, "grow", 1); ui_num(m, "gap", 2);
                    ui_node(m, "text"); ui_str(m, "text", list[i].host); ui_num(m, "weight", 1); ui_end(m);
                    settings_say(m, settings_rule_label(list[i].rule), "caption",
                                 list[i].rule == WEBPREFS_SITE_BLOCK ? t->warning : 0);
                ui_end(m);
                uint32_t name = settings_name(s, list[i].host);
                settings_tool(m, "close", name != UINT32_MAX, SET_EXCEPTION_REMOVE, name);
            ui_end(m);
        }
        if (!count) settings_say(m, "例外はありません。", "caption", t->textDim);
    ui_end(m);
    free(list);
}

static const struct settings_card {
    int section;
    const char *words; /* what the search looks in besides what the card shows */
    void (*draw)(struct ui_msg *m, struct settings *s, float width);
} settings_cards[] = {
    {SECTION_LANGUAGES, "優先する言語 language languages Accept-Language 順", settings_card_languages},
    {SECTION_LANGUAGES, "言語を追加 language add 探す", settings_card_add_language},
    {SECTION_LANGUAGES, "ウェブサイトが受け取るもの Accept-Language navigator.languages header 要求", settings_card_sent_languages},
    {SECTION_PRIVACY, "Cookie cookies サイトデータ site data 消去 clear 使用量 閉じたら ブラウジングデータ localStorage sessionStorage", settings_card_site_data},
    {SECTION_PRIVACY, "サイトごとのデータ sites Cookie 消去 管理 manage localStorage", settings_card_sites},
    {SECTION_PRIVACY, "例外 exceptions 許可 allow ブロック block Cookie サイトデータ", settings_card_exceptions},
    {SECTION_PRIVACY, "検索の履歴 search history autocomplete 候補 消去", settings_card_search_history},
};

/* The page: the sections on the left, the search and the section's (or the search's) cards. */
static void settings_view(struct ui_msg *m, struct settings *s, uint32_t view, float width) {
    const struct ui_theme *t = &ui_theme;
    settings_building = &settings_sent[view % SETTINGS_VIEWS];
    settings_building->view = settings_building->count = 0;
    if (!settings_languages_ready()) { settings_say(m, "設定の頁のための記憶が足りません。", NULL, t->danger); return; }
    settings_read_languages(s);
    char q[sizeof s->find];
    settings_trimmed(s->find, q, sizeof q);
    float content = width - SETTINGS_SIDEBAR - 2 * SETTINGS_PAD - 2 * 16 - 8;
    ui_node(m, "row"); ui_num(m, "gap", 0); ui_str(m, "align", "stretch"); ui_num(m, "grow", 1);
    ui_str(m, "key", "settings");
        ui_node(m, "column"); ui_num(m, "width", SETTINGS_SIDEBAR); ui_nums(m, "padding", (double[]){22, 12}, 2);
        ui_num(m, "gap", 4); ui_num(m, "background", t->sidebar);
            ui_node(m, "row"); ui_num(m, "gap", 10); ui_nums(m, "padding", (double[]){0, 10, 18, 10}, 4);
                ui_node(m, "icon"); ui_str(m, "icon", "settings"); ui_num(m, "size", 22); ui_num(m, "colour", t->accent);
                ui_end(m);
                ui_node(m, "text"); ui_str(m, "text", "設定"); ui_str(m, "style", "title"); ui_num(m, "size", 19);
                ui_end(m);
            ui_end(m);
            for (int i = 0; i < SECTIONS; i++) {
                settings_act_at(ui_node(m, "item"), SET_SECTION, (uint32_t)i);
                    ui_str(m, "text", settings_sections[i].label); ui_str(m, "icon", settings_sections[i].icon);
                    ui_num(m, "selected", !*q && i == s->section); ui_str(m, "key", settings_sections[i].label);
                ui_end(m);
            }
        ui_end(m);
        ui_node(m, "scroll"); ui_num(m, "grow", 1); ui_nums(m, "padding", (double[]){24, SETTINGS_PAD}, 2);
        ui_num(m, "gap", 14); ui_str(m, "key", *q ? "found" : settings_sections[s->section].label);
            settings_act_at(ui_node(m, "field"), SET_FIND, 0);
                ui_str(m, "value", s->find); ui_str(m, "placeholder", "設定を検索"); ui_str(m, "width", "fill");
                ui_str(m, "key", "find");
            ui_end(m);
            ui_node(m, "text"); ui_str(m, "text", *q ? "検索の結果" : settings_sections[s->section].label);
            ui_str(m, "style", "title"); ui_end(m);
            bool any = false;
            for (size_t i = 0; i < sizeof settings_cards / sizeof *settings_cards; i++) {
                const struct settings_card *c = &settings_cards[i];
                if (*q ? !settings_contains(c->words, q) && !settings_contains(settings_sections[c->section].label, q)
                       : c->section != s->section) continue;
                c->draw(m, s, content);
                any = true;
            }
            if (!any) settings_say(m, "一致する設定はありません。", "caption", 0);
            if (s->status[0]) settings_say(m, s->status, "caption", s->status_bad ? t->warning : t->success);
        ui_end(m);
    ui_end(m);
}

/* What is typed into one of its fields: whether the page changed. */
static bool settings_text(struct settings *s, struct settings_action a, const char *text) {
    char *to = a.act == SET_FIND ? s->find : a.act == SET_SEARCH ? s->search : a.act == SET_SITE_FIND ? s->site_find
             : a.act == SET_EXCEPTION_HOST ? s->exception : NULL;
    size_t cap = a.act == SET_FIND ? sizeof s->find : a.act == SET_SEARCH ? sizeof s->search
               : a.act == SET_SITE_FIND ? sizeof s->site_find : sizeof s->exception;
    if (!to) return false;
    size_t n = strlcpy(to, text, cap);
    if (n >= cap) { /* cut: not inside a character */
        n = cap - 1;
        while (n && ((unsigned char)to[n - 1] & 0xC0) == 0x80) n--;
        if (n && (unsigned char)to[n - 1] >= 0xC0) n--;
        to[n] = 0;
    }
    return true;
}

/* A click or a toggle, in its turn with the rest of the input. */
static void settings_do(struct settings *s, struct settings_action a, uint32_t value) {
    if (!settings_languages_ready()) return;
    switch (a.act) {
    case SET_SECTION:
        s->section = (int)MIN(a.arg, (uint32_t)SECTIONS - 1);
        s->find[0] = 0; s->status[0] = 0; s->status_bad = false; s->confirm_clear = false;
        settings_sites_stale = true;
        break;
    case SET_CLEAR_SITE: if (settings_name_at(s, a.arg) && *settings_name_at(s, a.arg)) settings_clear(s, settings_name_at(s, a.arg)); break;
    case SET_CLEAR_ALL: s->confirm_clear = true; break;
    case SET_CLEAR_ALL_CANCEL: s->confirm_clear = false; break;
    case SET_CLEAR_ALL_SURE: s->confirm_clear = false; settings_clear(s, NULL); break;
    case SET_CLEAR_ON_CLOSE: {
        n_result r = webprefs_set_clear_on_close(value != 0);
        settings_saved(s, r);
        if (r >= 0) strlcpy(s->status, value ? "ブラウザーを閉じたら、例外で許したサイトを除いて Cookie とサイトデータを消します。"
                                             : "閉じても Cookie とサイトデータを残します。", sizeof s->status);
        break;
    }
    case SET_EXCEPTION_ADD:
        if (a.arg == WEBPREFS_SITE_ALLOW || a.arg == WEBPREFS_SITE_SESSION || a.arg == WEBPREFS_SITE_BLOCK)
            settings_add_exception(s, (enum webprefs_site_rule)a.arg);
        break;
    case SET_EXCEPTION_REMOVE:
        if (settings_name_at(s, a.arg)) {
            n_result r = webprefs_set_site_rule(settings_name_at(s, a.arg), WEBPREFS_SITE_DEFAULT);
            settings_saved(s, r);
            if (r >= 0) snprintf(s->status, sizeof s->status, "%s の例外を外しました。", settings_name_at(s, a.arg));
        }
        break;
    case SET_UP: settings_move_language(s, a.arg, -1); break;
    case SET_DOWN: settings_move_language(s, a.arg, 1); break;
    case SET_REMOVE: settings_remove_language(s, a.arg); break;
    case SET_ADD: settings_add_language(s, a.arg); break;
    case SET_RESTORE: settings_saved(s, webprefs_reset_languages()); break;
    case SET_HISTORY: {
        n_result r = webprefs_set_search_history(value != 0);
        settings_saved(s, r);
        if (r >= 0) strlcpy(s->status, value ? "有効にしました。過去の値は、明示して選んだときだけ候補に出ます。"
                                             : "止めました。残っている履歴は「消去して止める」まで消えません。", sizeof s->status);
        break;
    }
    case SET_CLEAR_HISTORY: {
        bool ok = web_autocomplete_clear();
        webprefs_sync();
        strlcpy(s->status, ok ? "履歴を消し、候補を止めました。"
                              : "保存できませんでした。ブラウザーのデータのフォルダーに書けるかを確かめてください。", sizeof s->status);
        s->status_bad = !ok;
        break;
    }
    }
}
