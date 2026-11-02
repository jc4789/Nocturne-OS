/* ブラウザーの設定（webprefs.h）：プログラムの scope の設定として settings が持つ。各プロセスが読んで持ち、
   見張りで変更を聞く。

   二つの錠。io は読みと変更と見張りを一つの順にする。state は今持っているものを守り、どのスレッドの読み手も
   取る。io を先に取り、後では取らない。 */
#include <webprefs.h>
#include <nocturne_language.h>
#include <nocturne_settings.h>
#include <webcookie.h>
#include <weburl.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define ACCEPT_MAX WEBPREFS_ACCEPT_LANGUAGE_MAX
/* What each tag after the first adds to the Accept-Language: a comma and ";q=0.N". */
#define WEIGHT_BYTES (sizeof ",;q=0.1" - 1)
/* The most tags a list holds: its Accept-Language fits, and the shortest tag has two letters. */
#define LIST_TAGS ((ACCEPT_MAX - 2) / (2 + WEIGHT_BYTES) + 1)

/* What the browser's settings page offers: tags of registered subtags (RFC 5646 §2.2.9's valid
   tags) in their usual case, alphabetical. The table the system's Settings window had (a51c4398). */
static const struct webprefs_language catalog[] = {
    {"af", "Afrikaans", "Afrikaans"},
    {"am", "አማርኛ", "Amharic"},
    {"ar", "العربية", "Arabic"},
    {"az", "azərbaycan", "Azerbaijani"},
    {"be", "беларуская", "Belarusian"},
    {"bg", "български", "Bulgarian"},
    {"bn", "বাংলা", "Bangla"},
    {"br", "brezhoneg", "Breton"},
    {"bs", "bosanski", "Bosnian"},
    {"ca", "català", "Catalan"},
    {"cs", "čeština", "Czech"},
    {"cy", "Cymraeg", "Welsh"},
    {"da", "dansk", "Danish"},
    {"de", "Deutsch", "German"},
    {"de-AT", "Deutsch (Österreich)", "German (Austria)"},
    {"de-CH", "Deutsch (Schweiz)", "German (Switzerland)"},
    {"de-DE", "Deutsch (Deutschland)", "German (Germany)"},
    {"el", "Ελληνικά", "Greek"},
    {"en", "English", "English"},
    {"en-AU", "English (Australia)", "English (Australia)"},
    {"en-CA", "English (Canada)", "English (Canada)"},
    {"en-GB", "English (United Kingdom)", "English (United Kingdom)"},
    {"en-IN", "English (India)", "English (India)"},
    {"en-US", "English (United States)", "English (United States)"},
    {"eo", "esperanto", "Esperanto"},
    {"es", "español", "Spanish"},
    {"es-419", "español (Latinoamérica)", "Spanish (Latin America)"},
    {"es-ES", "español (España)", "Spanish (Spain)"},
    {"es-MX", "español (México)", "Spanish (Mexico)"},
    {"et", "eesti", "Estonian"},
    {"eu", "euskara", "Basque"},
    {"fa", "فارسی", "Persian"},
    {"fi", "suomi", "Finnish"},
    {"fil", "Filipino", "Filipino"},
    {"fr", "français", "French"},
    {"fr-CA", "français (Canada)", "French (Canada)"},
    {"fr-FR", "français (France)", "French (France)"},
    {"fy", "Frysk", "Western Frisian"},
    {"ga", "Gaeilge", "Irish"},
    {"gd", "Gàidhlig", "Scottish Gaelic"},
    {"gl", "galego", "Galician"},
    {"gu", "ગુજરાતી", "Gujarati"},
    {"ha", "Hausa", "Hausa"},
    {"he", "עברית", "Hebrew"},
    {"hi", "हिन्दी", "Hindi"},
    {"hr", "hrvatski", "Croatian"},
    {"hu", "magyar", "Hungarian"},
    {"hy", "հայերեն", "Armenian"},
    {"id", "Bahasa Indonesia", "Indonesian"},
    {"ig", "Igbo", "Igbo"},
    {"is", "íslenska", "Icelandic"},
    {"it", "italiano", "Italian"},
    {"ja", "日本語", "Japanese"},
    {"ka", "ქართული", "Georgian"},
    {"kk", "қазақ тілі", "Kazakh"},
    {"km", "ខ្មែរ", "Khmer"},
    {"kn", "ಕನ್ನಡ", "Kannada"},
    {"ko", "한국어", "Korean"},
    {"ku", "kurdî", "Kurdish"},
    {"ky", "кыргызча", "Kyrgyz"},
    {"la", "Latina", "Latin"},
    {"lb", "Lëtzebuergesch", "Luxembourgish"},
    {"lo", "ລາວ", "Lao"},
    {"lt", "lietuvių", "Lithuanian"},
    {"lv", "latviešu", "Latvian"},
    {"mk", "македонски", "Macedonian"},
    {"ml", "മലയാളം", "Malayalam"},
    {"mn", "монгол", "Mongolian"},
    {"mr", "मराठी", "Marathi"},
    {"ms", "Bahasa Melayu", "Malay"},
    {"mt", "Malti", "Maltese"},
    {"my", "မြန်မာ", "Burmese"},
    {"nb", "norsk bokmål", "Norwegian Bokmål"},
    {"ne", "नेपाली", "Nepali"},
    {"nl", "Nederlands", "Dutch"},
    {"nn", "norsk nynorsk", "Norwegian Nynorsk"},
    {"oc", "occitan", "Occitan"},
    {"pa", "ਪੰਜਾਬੀ", "Punjabi"},
    {"pl", "polski", "Polish"},
    {"ps", "پښتو", "Pashto"},
    {"pt", "português", "Portuguese"},
    {"pt-BR", "português (Brasil)", "Portuguese (Brazil)"},
    {"pt-PT", "português (Portugal)", "Portuguese (Portugal)"},
    {"ro", "română", "Romanian"},
    {"ru", "русский", "Russian"},
    {"si", "සිංහල", "Sinhala"},
    {"sk", "slovenčina", "Slovak"},
    {"sl", "slovenščina", "Slovenian"},
    {"so", "Soomaali", "Somali"},
    {"sq", "shqip", "Albanian"},
    {"sr", "српски", "Serbian"},
    {"sv", "svenska", "Swedish"},
    {"sw", "Kiswahili", "Swahili"},
    {"ta", "தமிழ்", "Tamil"},
    {"te", "తెలుగు", "Telugu"},
    {"tg", "тоҷикӣ", "Tajik"},
    {"th", "ไทย", "Thai"},
    {"tk", "türkmen dili", "Turkmen"},
    {"tr", "Türkçe", "Turkish"},
    {"tt", "татар", "Tatar"},
    {"uk", "українська", "Ukrainian"},
    {"ur", "اردو", "Urdu"},
    {"uz", "oʻzbek", "Uzbek"},
    {"vi", "Tiếng Việt", "Vietnamese"},
    {"xh", "isiXhosa", "Xhosa"},
    {"yo", "Èdè Yorùbá", "Yoruba"},
    {"yue", "粵語", "Cantonese"},
    {"zh", "中文", "Chinese"},
    {"zh-CN", "中文（中国）", "Chinese (China)"},
    {"zh-HK", "中文（香港）", "Chinese (Hong Kong)"},
    {"zh-TW", "中文（台灣）", "Chinese (Taiwan)"},
    {"zu", "isiZulu", "Zulu"},
};
#define CATALOG (sizeof catalog / sizeof *catalog)

const struct webprefs_language *webprefs_catalog(size_t *count) {
    if (count) *count = CATALOG;
    return catalog;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* The catalog's language whose tag is the n bytes at tag, ASCII case aside; NULL when none is. */
static const struct webprefs_language *language_find(const char *tag, size_t n) {
    for (size_t i = 0; i < CATALOG; i++) {
        const char *t = catalog[i].tag;
        size_t k = 0;
        while (k < n && t[k] && lower(t[k]) == lower(tag[k])) k++;
        if (k == n && !t[k]) return &catalog[i];
    }
    return NULL;
}

/* A list of languages: catalog tags in the catalog's form, each once, commas between, whose
   Accept-Language fits (so the list itself is never longer than ACCEPT_MAX). */
struct list {
    const struct webprefs_language *tags[LIST_TAGS];
    size_t count, length, accept; /* its tags, the bytes of its text and of its Accept-Language */
    char text[ACCEPT_MAX + 1];
};

/* Adds lang at the end: 1; 0 when the list has it already; -1 when the Accept-Language would not
   fit any more. */
static int list_add(struct list *l, const struct webprefs_language *lang) {
    for (size_t i = 0; i < l->count; i++)
        if (l->tags[i] == lang) return 0;
    size_t n = strlen(lang->tag), cost = n + (l->count ? WEIGHT_BYTES : 0);
    if (l->count == LIST_TAGS || l->accept + cost > ACCEPT_MAX) return -1;
    if (l->count) l->text[l->length++] = ',';
    memcpy(l->text + l->length, lang->tag, n);
    l->length += n;
    l->text[l->length] = 0;
    l->accept += cost;
    l->tags[l->count++] = lang;
    return 1;
}

/* The list count tags make, in *l: 0, or -N_ERR_INVALID when there is none, one is not in the
   catalog (ASCII case aside), one is there twice, or its Accept-Language does not fit. */
static n_result list_make(const char *const *tags, size_t count, struct list *l) {
    *l = (struct list){0};
    if (!tags || !count) return -N_ERR_INVALID;
    for (size_t i = 0; i < count; i++) {
        const struct webprefs_language *lang = tags[i] ? language_find(tags[i], strlen(tags[i])) : NULL;
        if (!lang || list_add(l, lang) <= 0) return -N_ERR_INVALID;
    }
    return 0;
}

bool webprefs_languages_fit(const char *const *tags, size_t count) {
    struct list l;
    return list_make(tags, count, &l) >= 0;
}

/* What the user chose, as a process holds it. */
struct prefs {
    char languages[ACCEPT_MAX + 1]; /* a list's text, never empty */
    size_t languages_length;
    bool search_history, clear_on_close;
    struct webprefs_site_exception *sites; /* malloc'd, sorted by host, each host once */
    size_t site_count;
};

static void prefs_default(struct prefs *p) {
    p->languages_length = n_locale_languages(p->languages, sizeof p->languages);
    p->search_history = p->clear_on_close = false;
    p->sites = NULL;
    p->site_count = 0;
}

/* The site exceptions. A host: lowercase LDH labels of 1 to 63 bytes between dots (A-labels for
   Unicode names), at most WEBPREFS_HOST_MAX bytes; or an [IPv6] address. */
static bool host_valid(const char *h, size_t n) {
    if (!n || n > WEBPREFS_HOST_MAX) return false;
    if (h[0] == '[') {
        if (n < 3 || h[n - 1] != ']') return false;
        for (size_t i = 1; i + 1 < n; i++)
            if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f') || h[i] == ':' || h[i] == '.')) return false;
        return true;
    }
    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (c == '.') {
            if (!label || h[i - 1] == '-') return false;
            label = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && label)) {
            if (++label > 63) return false;
        } else {
            return false;
        }
    }
    return label && h[n - 1] != '-';
}

/* An IPv4 or [IPv6] address: an exception for it covers only itself. */
static bool host_address(const char *h) {
    if (h[0] == '[') return true;
    for (; *h; h++)
        if (!(*h >= '0' && *h <= '9') && *h != '.') return false;
    return true;
}

static const char *const rule_names[] = {"", "allow", "session", "block"};

static int site_compare(const void *a, const void *b) {
    return strcmp(((const struct webprefs_site_exception *)a)->host, ((const struct webprefs_site_exception *)b)->host);
}

/* The value of site-data-exceptions=: "rule:host" items between spaces. An item that is not one
   is left out, as is a host there already (the first counts). false: no memory. */
static bool sites_read(const char *value, size_t n, struct prefs *p) {
    size_t items = 0;
    for (size_t i = 0; i < n; i++) items += value[i] == ':';
    if (!items) return true;
    struct webprefs_site_exception *sites = calloc(items, sizeof *sites);
    if (!sites) return false;
    size_t count = 0;
    for (size_t at = 0; at < n;) {
        while (at < n && (value[at] == ' ' || value[at] == '\t')) at++;
        size_t end = at;
        while (end < n && value[end] != ' ' && value[end] != '\t') end++;
        const char *item = value + at, *colon = memchr(item, ':', end - at);
        at = end;
        if (!colon) continue;
        size_t rn = (size_t)(colon - item), hn = (size_t)(value + end - colon - 1);
        enum webprefs_site_rule rule = WEBPREFS_SITE_DEFAULT;
        for (int r = WEBPREFS_SITE_ALLOW; r <= WEBPREFS_SITE_BLOCK; r++)
            if (strlen(rule_names[r]) == rn && !memcmp(item, rule_names[r], rn)) rule = (enum webprefs_site_rule)r;
        if (!rule || !host_valid(colon + 1, hn)) continue;
        memcpy(sites[count].host, colon + 1, hn);
        sites[count].host[hn] = 0;
        sites[count].rule = rule;
        bool twice = false;
        for (size_t k = 0; k < count && !twice; k++) twice = !strcmp(sites[k].host, sites[count].host);
        if (!twice) count++;
    }
    if (!count) {
        free(sites);
        return true;
    }
    qsort(sites, count, sizeof *sites, site_compare);
    p->sites = sites;
    p->site_count = count;
    return true;
}

/* The value of site-data-exceptions= for a list: malloc'd; NULL with *none for an empty list
   (the line is removed). */
static char *sites_write(const struct webprefs_site_exception *sites, size_t count, bool *none) {
    *none = !count;
    if (!count) return NULL;
    size_t room = 1;
    for (size_t i = 0; i < count; i++) room += strlen(rule_names[sites[i].rule]) + 2 + strlen(sites[i].host);
    char *text = malloc(room);
    if (!text) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < count; i++) {
        size_t rn = strlen(rule_names[sites[i].rule]), hn = strlen(sites[i].host);
        if (i) text[at++] = ' ';
        memcpy(text + at, rule_names[sites[i].rule], rn);
        text[at + rn] = ':';
        memcpy(text + at + rn + 1, sites[i].host, hn);
        at += rn + 1 + hn;
    }
    text[at] = 0;
    return text;
}

/* The rule of host in a list: its own exception, else its nearest parent domain's. */
static enum webprefs_site_rule sites_rule(const struct webprefs_site_exception *sites, size_t count, const char *host) {
    bool address = host_address(host);
    for (const char *h = host; h && *h;) {
        size_t lo = 0, hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int c = strcmp(h, sites[mid].host);
            if (!c) return sites[mid].rule;
            if (c < 0) hi = mid;
            else lo = mid + 1;
        }
        if (address) break;
        const char *dot = strchr(h, '.');
        h = dot ? dot + 1 : NULL;
    }
    return WEBPREFS_SITE_DEFAULT;
}

/* The value of languages=: tags between commas, spaces and tabs around them allowed, each made the
   catalog's (ASCII case aside). One not in the catalog is left out, as is one there already, and
   the list ends before the first tag that would make its Accept-Language too long. None left:
   the default stays. */
static void languages_read(const char *value, size_t n, struct prefs *p) {
    struct list l = {0};
    for (size_t at = 0; at < n;) {
        const char *comma = memchr(value + at, ',', n - at);
        size_t end = comma ? (size_t)(comma - value) : n, a = at, b = end;
        at = end + 1;
        while (a < b && (value[a] == ' ' || value[a] == '\t')) a++;
        while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t')) b--;
        const struct webprefs_language *lang = language_find(value + a, b - a);
        if (lang && list_add(&l, lang) < 0) break;
    }
    if (!l.count) return;
    memcpy(p->languages, l.text, l.length + 1);
    p->languages_length = l.length;
}

/* プログラムの scope の鍵。 */
enum { KEY_LANGUAGES, KEY_SEARCH_HISTORY, KEY_CLEAR_ON_CLOSE, KEY_SITE_EXCEPTIONS };
static const struct n_setting_decl declarations[] = {
    {.domain = N_DOMAIN_PROGRAM, .key = KEY_LANGUAGES, .type = N_SETTING_STRING, .scope = N_SCOPE_PROGRAM,
     .apply = N_APPLY_NONE, .max = ACCEPT_MAX, .name = "browser.languages",
     .label = "優先する言語", .about = "タグをコンマで区切った一覧です。空ならシステムの言語に従います。"},
    {.domain = N_DOMAIN_PROGRAM, .key = KEY_SEARCH_HISTORY, .type = N_SETTING_BOOL, .scope = N_SCOPE_PROGRAM,
     .apply = N_APPLY_NONE, .max = 1, .name = "browser.search-history", .label = "検索の履歴",
     .about = "検索の入力を候補のために残します。"},
    {.domain = N_DOMAIN_PROGRAM, .key = KEY_CLEAR_ON_CLOSE, .type = N_SETTING_BOOL, .scope = N_SCOPE_PROGRAM,
     .apply = N_APPLY_NONE, .max = 1, .name = "browser.clear-on-close", .label = "閉じたらサイトのデータを消す",
     .about = "ブラウザーを閉じるときに Cookie とサイトのデータを消します。"},
    {.domain = N_DOMAIN_PROGRAM, .key = KEY_SITE_EXCEPTIONS, .type = N_SETTING_STRING, .scope = N_SCOPE_PROGRAM,
     .apply = N_APPLY_NONE, .max = N_SETTING_VALUE_MAX, .name = "browser.site-exceptions", .label = "サイトの例外",
     .about = "「規則:ホスト」を空白で区切った一覧です。"},
};
#define SITES_ATTEMPTS 8 /* 他のプロセスと競って書き直す回数 */

static mtx_t io, state;

/* io の下。queue：見張りを置きたい行列（-1 は無し。読むのは一度）。 */
static n_handle watch = -1, queue = -1;
static bool declared;

/* state の下。valid と generation は錠なしでも読む。seen：読み手に一覧を渡した。valid：持っているものが settings の
   値で、欠けた見張りが無い。 */
static struct prefs current;
static bool seen, valid;
static uint64_t generation, site_generation;

/* p を今持つものにする（例外は移す）。一覧の世代は読み手に渡した一覧と違うときに進む。サイトの世代はその変更で
   進む。io の下。 */
static void apply(struct prefs *p) {
    mtx_lock(&state);
    if (seen && (p->languages_length != current.languages_length ||
                 memcmp(p->languages, current.languages, p->languages_length)))
        __atomic_store_n(&generation, generation + 1, __ATOMIC_RELEASE);
    bool sites_same = p->clear_on_close == current.clear_on_close && p->site_count == current.site_count;
    for (size_t i = 0; sites_same && i < p->site_count; i++)
        sites_same = p->sites[i].rule == current.sites[i].rule && !strcmp(p->sites[i].host, current.sites[i].host);
    if (!sites_same) __atomic_store_n(&site_generation, site_generation + 1, __ATOMIC_RELEASE);
    free(current.sites);
    current = *p;
    p->sites = NULL;
    p->site_count = 0;
    mtx_unlock(&state);
}

/* 宣言は一度。 */
static n_result declare_locked(void) {
    if (declared) return 0;
    n_result r = n_setting_declare(declarations, sizeof declarations / sizeof *declarations, NULL);
    if (r >= 0) declared = true;
    return r;
}

/* 今の値を *p へ（読めなければ既定）。0 か失敗。io の下。 */
static n_result settings_read(struct prefs *p) {
    prefs_default(p);
    n_result r = declare_locked();
    if (r < 0) return r;
    char *text = malloc(N_SETTING_VALUE_MAX);
    if (!text) return -N_ERR_NO_MEMORY;
    n_result n = n_setting_get(n_setting_of_program(KEY_LANGUAGES), text, N_SETTING_VALUE_MAX);
    if (n >= 0) languages_read(text, (size_t)n, p);
    int32_t on = 0;
    if ((r = n_setting_get_int(n_setting_of_program(KEY_SEARCH_HISTORY), &on)) >= 0) p->search_history = on != 0;
    if (r >= 0 && (r = n_setting_get_int(n_setting_of_program(KEY_CLEAR_ON_CLOSE), &on)) >= 0) p->clear_on_close = on != 0;
    if (r >= 0 && (n = n_setting_get(n_setting_of_program(KEY_SITE_EXCEPTIONS), text, N_SETTING_VALUE_MAX)) >= 0 &&
        !sites_read(text, (size_t)n, p))
        r = -N_ERR_NO_MEMORY;
    free(text);
    return r < 0 ? r : n < 0 ? n : 0;
}

/* 読んでプロセスのものにする。サービスに届かなければ今の値を保ち、まだ値が無ければ既定で動く。サービスが無ければ読み直さない（見張りが戻れば読む）。ほかの失敗は次の
   使用で読み直す。io の下。 */
static void refresh_locked(void) {
    struct prefs p;
    n_result r = settings_read(&p);
    if (r >= 0 || r == -N_ERR_NOT_FOUND || !current.languages_length) apply(&p);
    else free(p.sites);
    __atomic_store_n(&valid, (r >= 0 || r == -N_ERR_NOT_FOUND) && (queue < 0 || watch >= 0), __ATOMIC_RELEASE);
}

/* queue に見張りを置く（読む前に：その間の変更を落とさない）。io の下。 */
static n_result watch_open_locked(void) {
    n_handle w = n_setting_watch(N_SETTING_ANY, N_SETTING_ANY);
    if (w == -N_ERR_NOT_FOUND) queue = -1;
    if (w < 0) return w;
    n_result r = n_events_watch(queue, w, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, WEBPREFS_TOKEN);
    if (r < 0) {
        n_handle_close(w);
        if (r == -N_ERR_NO_HANDLE || r == -N_ERR_WRONG_KIND) queue = -1;
        return r;
    }
    watch = w;
    return 0;
}

static void watch_drop_locked(void) {
    if (watch < 0) return;
    n_events_unwatch(queue, WEBPREFS_TOKEN);
    n_handle_close(watch);
    watch = -1;
}

/* 見張りの知らせを取り、あれば読み直す。サービスが起こし直されていれば見張り直す。io の下。 */
static void take_locked(bool closed) {
    bool any = false;
    if (watch >= 0) {
        struct n_settings_changed change;
        n_result r;
        while ((r = n_setting_watch_take(watch, &change)) >= 0)
            if (change.id.domain == N_DOMAIN_PROGRAM || change.id.domain == N_DOMAIN_LANGUAGE) any = true;
        if (r == -N_ERR_CLOSED) closed = true;
    }
    if (closed && watch >= 0) {
        watch_drop_locked();
        declared = false;
        (void)watch_open_locked();
    }
    if (any || closed || !__atomic_load_n(&valid, __ATOMIC_ACQUIRE)) refresh_locked();
}

/* 読み手の前に：一度読み、読みか見張りが欠けている間は読み直す。 */
static void ensure(void) {
    if (__atomic_load_n(&valid, __ATOMIC_ACQUIRE)) return;
    mtx_lock(&io);
    if (!__atomic_load_n(&valid, __ATOMIC_ACQUIRE)) {
        if (queue >= 0 && watch < 0) (void)watch_open_locked();
        refresh_locked();
    }
    mtx_unlock(&io);
}

n_result webprefs_watch(n_handle q) {
    mtx_lock(&io);
    n_result r = 0;
    if (watch < 0 || q != queue) {
        watch_drop_locked();
        queue = q;
        r = watch_open_locked();
        refresh_locked();
    }
    mtx_unlock(&io);
    return r;
}

void webprefs_events(const struct n_event *events, size_t count) {
    bool mine = false, closed = false;
    for (size_t i = 0; i < count; i++)
        if (events[i].token == WEBPREFS_TOKEN) {
            mine = true;
            if (events[i].events & N_EVENT_CLOSED) closed = true;
        }
    if (!mine) return;
    mtx_lock(&io);
    take_locked(closed);
    mtx_unlock(&io);
}

void webprefs_sync(void) {
    mtx_lock(&io);
    if (watch >= 0) {
        take_locked(false);
    } else {
        if (queue >= 0) (void)watch_open_locked();
        refresh_locked(); /* 見張りが無ければ何も知らせない：読む */
    }
    mtx_unlock(&io);
}

uint64_t webprefs_languages_generation(void) { return __atomic_load_n(&generation, __ATOMIC_ACQUIRE); }

n_result webprefs_languages(char *out, size_t cap) {
    ensure();
    mtx_lock(&state);
    size_t n = current.languages_length;
    n_result r = -N_ERR_TOO_LARGE;
    if (out && cap > n) {
        memcpy(out, current.languages, n + 1);
        seen = true;
        r = (n_result)n;
    }
    mtx_unlock(&state);
    return r;
}

/* A languages line with a bad value leaves the default too, so this asks what the process holds. */
bool webprefs_languages_default(void) {
    ensure();
    char system[sizeof current.languages];
    size_t n = n_locale_languages(system, sizeof system);
    mtx_lock(&state);
    bool same = current.languages_length == n && !memcmp(current.languages, system, n);
    mtx_unlock(&state);
    return same;
}

/* The Accept-Language of a list, in out (ACCEPT_MAX + 1 bytes): the first tag bare, then each
   with ";q=0.9" falling by a tenth to ";q=0.1" and staying there. The list fits (list_add). */
static size_t accept_of(const char *list, size_t n, char *out) {
    size_t length = 0;
    unsigned rank = 0;
    for (size_t at = 0; at < n; rank++) {
        const char *comma = memchr(list + at, ',', n - at);
        size_t k = comma ? (size_t)(comma - (list + at)) : n - at;
        if (length + k + (rank ? WEIGHT_BYTES : 0) > ACCEPT_MAX) break;
        if (rank) out[length++] = ',';
        memcpy(out + length, list + at, k);
        length += k;
        if (rank) {
            memcpy(out + length, ";q=0.", 5);
            length += 5;
            out[length++] = (char)('0' + (rank < 9 ? 10 - rank : 1));
        }
        at += k + 1;
    }
    out[length] = 0;
    return length;
}

n_result webprefs_accept_language(char *out, size_t cap) {
    ensure();
    char value[ACCEPT_MAX + 1];
    mtx_lock(&state);
    size_t n = accept_of(current.languages, current.languages_length, value);
    bool fits = out && cap > n;
    if (fits) seen = true;
    mtx_unlock(&state);
    if (!fits) return -N_ERR_TOO_LARGE;
    memcpy(out, value, n + 1);
    return (n_result)n;
}

bool webprefs_search_history(void) {
    ensure();
    mtx_lock(&state);
    bool on = current.search_history;
    mtx_unlock(&state);
    return on;
}

/* 一つの変更をその場で settings へ渡し、今の値を読み直す。 */
static n_result set_key(uint32_t key, const void *value, size_t length) {
    mtx_lock(&io);
    n_result r = declare_locked();
    if (r >= 0) r = n_setting_set(n_setting_of_program(key), value, length, NULL);
    if (r >= 0) refresh_locked();
    mtx_unlock(&io);
    return r;
}

n_result webprefs_set_languages(const char *const *tags, size_t count) {
    struct list l;
    n_result r = list_make(tags, count, &l);
    return r < 0 ? r : set_key(KEY_LANGUAGES, l.text, l.length);
}

n_result webprefs_reset_languages(void) { return set_key(KEY_LANGUAGES, "", 0); }

n_result webprefs_set_search_history(bool on) {
    int32_t v = on;
    return set_key(KEY_SEARCH_HISTORY, &v, sizeof v);
}

n_result webprefs_set_clear_on_close(bool on) {
    int32_t v = on;
    return set_key(KEY_CLEAR_ON_CLOSE, &v, sizeof v);
}

/* old の一覧の host の規則を変えた新しい一覧（malloc）。空なら *none。 */
static n_result sites_changed(const char *old, size_t on, const char *host, enum webprefs_site_rule rule, char **value, bool *none) {
    struct prefs p;
    prefs_default(&p);
    if (!sites_read(old, on, &p)) return -N_ERR_NO_MEMORY;
    struct webprefs_site_exception *sites = p.sites;
    size_t count = p.site_count, at = count;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(sites[i].host, host)) at = i;
    if (rule == WEBPREFS_SITE_DEFAULT) {
        if (at < count) {
            memmove(sites + at, sites + at + 1, (count - at - 1) * sizeof *sites);
            count--;
        }
    } else if (at < count) {
        sites[at].rule = rule;
    } else {
        struct webprefs_site_exception *grown = realloc(sites, (count + 1) * sizeof *grown);
        if (!grown) {
            free(sites);
            return -N_ERR_NO_MEMORY;
        }
        sites = grown;
        memset(&sites[count], 0, sizeof *sites);
        memcpy(sites[count].host, host, strlen(host) + 1);
        sites[count++].rule = rule;
        qsort(sites, count, sizeof *sites, site_compare);
    }
    *value = sites_write(sites, count, none);
    free(sites);
    return *value || *none ? 0 : -N_ERR_NO_MEMORY;
}

/* 他のプロセスの変更を消さない：読んだ一覧のままなら書く（n_setting_set_if）。変わっていれば読み直す。 */
static n_result sites_edit(const char *host, enum webprefs_site_rule rule) {
    struct n_setting_id id = n_setting_of_program(KEY_SITE_EXCEPTIONS);
    char *old = malloc(N_SETTING_VALUE_MAX);
    if (!old) return -N_ERR_NO_MEMORY;
    mtx_lock(&io);
    n_result r = declare_locked();
    for (int attempt = 0; r >= 0 && attempt < SITES_ATTEMPTS; attempt++) {
        n_result on = n_setting_get(id, old, N_SETTING_VALUE_MAX);
        if (on < 0) {
            r = on;
            break;
        }
        char *next = NULL;
        bool none = false;
        r = sites_changed(old, (size_t)on, host, rule, &next, &none);
        if (r < 0) break;
        uint32_t reason = N_SETTING_OK;
        r = n_setting_set_if(id, old, (size_t)on, next ? next : "", next ? strlen(next) : 0, &reason);
        free(next);
        if (reason == N_SETTING_OUT_OF_RANGE) r = -N_ERR_TOO_LARGE;
        if (r != -N_ERR_BUSY) break;
        r = attempt + 1 < SITES_ATTEMPTS ? 0 : r;
    }
    if (r >= 0) refresh_locked();
    mtx_unlock(&io);
    free(old);
    return r;
}

n_result webprefs_set_site_rule(const char *host, enum webprefs_site_rule rule) {
    if (!host || !host_valid(host, strlen(host)) || rule < WEBPREFS_SITE_DEFAULT || rule > WEBPREFS_SITE_BLOCK)
        return -N_ERR_INVALID;
    return sites_edit(host, rule);
}

/* host as the exceptions hold hosts, in out: lowercase, without a final dot. */
static bool host_key(const char *host, size_t n, char out[WEBPREFS_HOST_MAX + 1]) {
    if (n && host[n - 1] == '.') n--;
    if (!n || n > WEBPREFS_HOST_MAX) return false;
    for (size_t i = 0; i < n; i++) out[i] = lower(host[i]);
    out[n] = 0;
    return true;
}

/* The host of an authority ("user@host:port", up to '/', '?', '#' or '\\') in out. */
static bool authority_host(const char *p, char out[WEBPREFS_HOST_MAX + 1]) {
    size_t end = strcspn(p, "/?#\\");
    for (size_t i = end; i-- > 0;)
        if (p[i] == '@') {
            p += i + 1;
            end -= i + 1;
            break;
        }
    size_t n = end;
    if (n && p[0] == '[') {
        const char *close = memchr(p, ']', end);
        if (!close) return false;
        n = (size_t)(close - p) + 1;
    } else {
        const char *colon = memchr(p, ':', end);
        if (colon) n = (size_t)(colon - p);
    }
    return host_key(p, n, out);
}

/* The host of an http(s) or ws(s) URL (or of an origin's serialization) in out, as its URL record
   has it (weburl.h): 1; 0 for any other URL or text that is not one; -1 out of memory. */
static int url_host(const char *url, char out[WEBPREFS_HOST_MAX + 1]) {
    struct web_url record;
    int read = url ? web_url_read(url, strlen(url), &record) : 0;
    if (read != 1) return read;
    size_t n;
    const char *host = web_url_host_text(&record, &n);
    bool ok = (web_url_http_scheme(&record) || web_url_scheme_is(&record, "ws") || web_url_scheme_is(&record, "wss")) &&
              host_key(host, n, out);
    web_url_free(&record);
    return ok;
}

n_result webprefs_site_host(const char *input, char out[WEBPREFS_HOST_MAX + 1]) {
    if (!input || !out) return -N_ERR_INVALID;
    while (*input == ' ' || *input == '\t') input++;
    const char *scheme = strstr(input, "://");
    const char *p = scheme ? scheme + 3 : input;
    while (*p == '*' && p[1] == '.') p += 2; /* "*.example.com": the exception covers subdomains anyway */
    if (*p == '.') p++;
    char host[WEBPREFS_HOST_MAX + 1];
    size_t n = strcspn(p, " \t");
    char text[WEBPREFS_HOST_MAX * 4 + 64];
    if (n >= sizeof text) return -N_ERR_INVALID;
    memcpy(text, p, n);
    text[n] = 0;
    if (!authority_host(text, host) || !host_valid(host, strlen(host))) return -N_ERR_INVALID;
    memcpy(out, host, strlen(host) + 1);
    return 0;
}

/* Under state: the rule of host (a key) and whether its data outlives the session. */
static enum webprefs_site_rule site_locked(const char *key, bool *kept) {
    enum webprefs_site_rule r = sites_rule(current.sites, current.site_count, key);
    *kept = r == WEBPREFS_SITE_ALLOW || (r != WEBPREFS_SITE_SESSION && !current.clear_on_close);
    return r;
}

static enum webprefs_site_rule site_of_key(const char *key, bool *kept) {
    ensure();
    mtx_lock(&state);
    enum webprefs_site_rule r = site_locked(key, kept);
    mtx_unlock(&state);
    return r;
}

enum webprefs_site_rule webprefs_site_rule(const char *host) {
    char key[WEBPREFS_HOST_MAX + 1];
    bool kept;
    return host && host_key(host, strlen(host), key) ? site_of_key(key, &kept) : WEBPREFS_SITE_DEFAULT;
}

enum webprefs_site_rule webprefs_site_rule_url(const char *url) {
    char key[WEBPREFS_HOST_MAX + 1];
    bool kept;
    int host = url_host(url, key);
    /* Out of memory: the host cannot be told, so storage is refused rather than allowed. */
    return host < 0 ? WEBPREFS_SITE_BLOCK : host ? site_of_key(key, &kept) : WEBPREFS_SITE_DEFAULT;
}

bool webprefs_storage_allowed(const char *url) { return webprefs_site_rule_url(url) != WEBPREFS_SITE_BLOCK; }

bool webprefs_site_kept(const char *host) {
    char key[WEBPREFS_HOST_MAX + 1];
    bool kept = true;
    if (host && host_key(host, strlen(host), key)) site_of_key(key, &kept);
    else kept = !webprefs_clear_on_close();
    return kept;
}

unsigned webprefs_cookie_rule(void *opaque, const char *host) {
    (void)opaque;
    char key[WEBPREFS_HOST_MAX + 1];
    bool kept = true;
    enum webprefs_site_rule r = host && host_key(host, strlen(host), key) ? site_of_key(key, &kept) : WEBPREFS_SITE_DEFAULT;
    return (r == WEBPREFS_SITE_BLOCK ? WEBCOOKIE_BLOCK : 0u) | (kept ? 0u : WEBCOOKIE_TRANSIENT);
}

bool webprefs_clear_on_close(void) {
    ensure();
    mtx_lock(&state);
    bool on = current.clear_on_close;
    mtx_unlock(&state);
    return on;
}

bool webprefs_site_data_transient(void) {
    ensure();
    mtx_lock(&state);
    bool any = current.clear_on_close;
    for (size_t i = 0; !any && i < current.site_count; i++) any = current.sites[i].rule == WEBPREFS_SITE_SESSION;
    mtx_unlock(&state);
    return any;
}

uint64_t webprefs_site_generation(void) {
    ensure();
    return __atomic_load_n(&site_generation, __ATOMIC_ACQUIRE);
}

size_t webprefs_site_exceptions(struct webprefs_site_exception *out, size_t cap) {
    ensure();
    mtx_lock(&state);
    size_t count = current.site_count;
    if (out) memcpy(out, current.sites, (cap < count ? cap : count) * sizeof *out);
    mtx_unlock(&state);
    return count;
}
