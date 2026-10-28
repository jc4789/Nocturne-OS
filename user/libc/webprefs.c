/* The browser's preferences (webprefs.h): the document "preferences" in the browser's profile
   folder, read into each browser process and kept current through a watch on it.

   Two locks. io keeps the document's reads, its changes and the watch in one order: a read puts
   what it read in place before another begins, and a change is claimed, read, changed and saved
   whole under it. state guards what this process holds now, for its readers on any thread, which
   wait for the document only until it has been read. io is taken before state, never after. */
#include <webprefs.h>
#include <webstorage.h>
#include <webcookie.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define HEADER "Nocturne browser preferences 1"
#define DOCUMENT_MAX (64u * 1024u)
#define DEFAULT_LANGUAGES WEBPREFS_DEFAULT_LANGUAGES
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
    memcpy(p->languages, DEFAULT_LANGUAGES, sizeof DEFAULT_LANGUAGES);
    p->languages_length = sizeof DEFAULT_LANGUAGES - 1;
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

/* The length of the first line when the document is this build's (HEADER, then a line feed or
   its end); 0 when it is not. */
static size_t header_of(const char *bytes, size_t n) {
    size_t h = sizeof HEADER - 1;
    return n >= h && !memcmp(bytes, HEADER, h) && (n == h || bytes[h] == '\n') ? h : 0;
}

/* The lines of a document from at on, each without its line feed (the last may have none). */
struct lines {
    const char *bytes;
    size_t n, at;
};

static bool line_next(struct lines *it, const char **line, size_t *length) {
    if (it->at >= it->n) return false;
    const char *start = it->bytes + it->at, *lf = memchr(start, '\n', it->n - it->at);
    *line = start;
    *length = lf ? (size_t)(lf - start) : it->n - it->at;
    it->at += *length + 1;
    return true;
}

/* Whether the line is "name=value", and its value. */
static bool line_value(const char *line, size_t length, const char *name, const char **value, size_t *n) {
    size_t k = strlen(name);
    if (length <= k || line[k] != '=' || memcmp(line, name, k)) return false;
    *value = line + k + 1;
    *n = length - k - 1;
    return true;
}

/* What a document says. Another first line: every default. Of the lines of a name, the first
   counts; a bad value leaves that one's default. false: no memory (*p holds nothing to free). */
static bool prefs_parse(const char *bytes, size_t n, struct prefs *p) {
    prefs_default(p);
    size_t h = header_of(bytes, n);
    if (!h) return true;
    bool languages = false, history = false, close = false, sites = false;
    struct lines it = {bytes, n, h + 1};
    const char *line, *value;
    size_t length, vn;
    while (line_next(&it, &line, &length)) {
        if (!languages && line_value(line, length, "languages", &value, &vn)) {
            languages = true;
            languages_read(value, vn, p);
        } else if (!history && line_value(line, length, "search-history", &value, &vn)) {
            history = true;
            p->search_history = vn == 2 && !memcmp(value, "on", 2); /* "off", or the default (off) */
        } else if (!close && line_value(line, length, "clear-site-data-on-close", &value, &vn)) {
            close = true;
            p->clear_on_close = vn == 2 && !memcmp(value, "on", 2);
        } else if (!sites && line_value(line, length, "site-data-exceptions", &value, &vn)) {
            sites = true;
            if (!sites_read(value, vn, p)) return false;
        }
    }
    return true;
}

/* The document whose line of name is "name=value" (value NULL: it has none), in *out (free it) and
   *n: the first line, then the other lines of old as they were, the new line where the first of
   name was or at the end. When old is not this build's document, the new one starts again. 0;
   -N_ERR_NO_MEMORY; -N_ERR_TOO_LARGE past DOCUMENT_MAX. */
static n_result document_make(const char *old, size_t on, const char *name, const char *value, char **out, size_t *n) {
    size_t h = header_of(old, on), k = strlen(name), vn = value ? strlen(value) : 0;
    size_t room = sizeof HEADER + (h ? on - h : 0) + k + vn + 2;
    char *b = malloc(room);
    if (!b) return -N_ERR_NO_MEMORY;
    memcpy(b, HEADER "\n", sizeof HEADER);
    size_t at = sizeof HEADER;
    bool placed = !value;
    struct lines it = {old, h ? on : 0, h + 1};
    const char *line, *v;
    size_t length, vlength;
    while (line_next(&it, &line, &length)) {
        bool named = line_value(line, length, name, &v, &vlength);
        if (named && placed) continue; /* a later line of name, or the line removed */
        if (named) {
            memcpy(b + at, name, k);
            b[at + k] = '=';
            memcpy(b + at + k + 1, value, vn);
            at += k + 1 + vn;
            placed = true;
        } else {
            memcpy(b + at, line, length);
            at += length;
        }
        b[at++] = '\n';
    }
    if (!placed) {
        memcpy(b + at, name, k);
        b[at + k] = '=';
        memcpy(b + at + k + 1, value, vn);
        at += k + 1 + vn;
        b[at++] = '\n';
    }
    if (at > DOCUMENT_MAX) {
        free(b);
        return -N_ERR_TOO_LARGE;
    }
    *out = b;
    *n = at;
    return 0;
}

/* The whole content of an open document of at most DOCUMENT_MAX bytes, in *bytes (free it; NULL
   when it is empty) and *n: 1. 0 when it is longer (not a document of this build: nothing read).
   Or the failure. */
static n_result read_all(n_handle h, char **bytes, size_t *n) {
    *bytes = NULL;
    *n = 0;
    n_result size = n_document_size(h);
    if (size < 0) return size;
    if ((uint64_t)size > DOCUMENT_MAX) return 0;
    if (!size) return 1;
    char *b = malloc((size_t)size);
    if (!b) return -N_ERR_NO_MEMORY;
    size_t at = 0;
    while (at < (size_t)size) {
        n_result got = n_document_read_at(h, at, b + at, (size_t)size - at);
        if (got < 0) {
            free(b);
            return got;
        }
        if (!got) break; /* shorter than its size said */
        at += (size_t)got;
    }
    *bytes = b;
    *n = at;
    return 1;
}

static mtx_t io, state;

/* Under io. queue: where the watch is wanted (-1: nowhere, the document is read once). */
static char path[1024];
static n_handle watch = -1, queue = -1;

/* Under state; valid and generation are read without it too. seen: a reader was given the list.
   valid: what the process holds is the document's, and no watch is wanted and missing. */
static struct prefs current = {.languages = DEFAULT_LANGUAGES, .languages_length = sizeof DEFAULT_LANGUAGES - 1};
static bool seen, valid;
static uint64_t generation, site_generation;

static bool path_ready(void) {
    if (path[0]) return true;
    const char *folder = web_profile_folder(false);
    if (folder && n_path_join(folder, "preferences", path, sizeof path) >= 0) return true;
    path[0] = 0;
    return false;
}

/* Makes p what the process holds; its exceptions become the process's (p keeps none). The list's
   generation grows when it differs from a list a reader was given: before any was given, no
   change can be seen. The sites' generation grows with any change of theirs. Under io. */
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

/* The document as it is now, in *p (every default without it): 0, or the failure that kept it
   from being read. Under io. */
static n_result document_read(struct prefs *p) {
    prefs_default(p);
    if (!path_ready()) return 0; /* no profile folder: no document */
    n_handle h = n_document_open(path, N_DOCUMENT_READ);
    if (h == -N_ERR_NOT_FOUND || h == -N_ERR_NOT_FOLDER || h == -N_ERR_IS_FOLDER) return 0;
    if (h < 0) return h;
    char *bytes;
    size_t n;
    n_result r = read_all(h, &bytes, &n);
    n_handle_close(h);
    if (r > 0 && !prefs_parse(bytes, n, p)) r = -N_ERR_NO_MEMORY;
    free(bytes);
    return r < 0 ? r : 0;
}

/* Reads the document into the process. After a failed read the process keeps what it held and
   the next use reads again, as it does while a watch is wanted and missing. Under io. */
static void refresh_locked(void) {
    struct prefs p;
    bool read = document_read(&p) >= 0;
    if (read) apply(&p);
    __atomic_store_n(&valid, read && (queue < 0 || watch >= 0), __ATOMIC_RELEASE);
}

/* Watches the document in queue (before it is read: then no change between is missed). Without
   watches on C: or without that queue the document is read once; after other failures (the
   profile folder removed, no memory) each use tries again. Under io. */
static n_result watch_open_locked(void) {
    n_handle w = path_ready() ? n_entry_watch(path, N_WATCH_NAME) : -N_ERR_NOT_FOUND;
    if (w == -N_ERR_UNSUPPORTED) queue = -1;
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

/* Takes the watch's changes, and reads the document again when there were some, when the watch
   ended (its folder was removed: watched again once there is one) or when the last read failed.
   Under io. */
static void take_locked(bool closed) {
    n_result n = watch >= 0 ? n_watch_take(watch) : 0;
    if (n == -N_ERR_CLOSED) closed = true;
    if (closed && watch >= 0) {
        watch_drop_locked();
        (void)watch_open_locked();
    }
    if (n || closed || !__atomic_load_n(&valid, __ATOMIC_ACQUIRE)) refresh_locked();
}

/* Before a reader: the document read once, and again while a read or a wanted watch failed. */
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
        refresh_locked(); /* without a watch nothing tells: read */
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
    mtx_lock(&state);
    bool same = current.languages_length == sizeof DEFAULT_LANGUAGES - 1 &&
                !memcmp(current.languages, DEFAULT_LANGUAGES, sizeof DEFAULT_LANGUAGES - 1);
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

/* The new value of a line, made from the document as it is when claimed (old: its on bytes, NULL
   when there is none): malloc'd in *value, or NULL to remove the line. 0 or the failure. */
typedef n_result (*value_of)(const char *old, size_t on, const void *arg, char **value);

static n_result fixed_value(const char *old, size_t on, const void *arg, char **value) {
    (void)old;
    (void)on;
    *value = NULL;
    if (!arg) return 0;
    size_t n = strlen(arg) + 1;
    if (!(*value = malloc(n))) return -N_ERR_NO_MEMORY;
    memcpy(*value, arg, n);
    return 0;
}

/* One change, written at once: the document is opened (made when missing), claimed, read again,
   given the line of name and saved whole, and what it now says is the process's. As
   n_document_save keeps the document the name had, a writer waiting for the claim reads this
   change before its own. Under io. */
static n_result edit_locked(const char *name, value_of make, const void *arg) {
    if (!path_ready()) return -N_ERR_NOT_FOUND;
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h == -N_ERR_NOT_FOUND) {
        char *value = NULL;
        n_result r = make(NULL, 0, arg, &value);
        bool none = !value;
        free(value);
        if (r < 0) return r;
        if (none) {
            refresh_locked(); /* no line to remove: the default applies */
            return 0;
        }
        h = n_document_open(path, N_DOCUMENT_CREATE);
        if (h == -N_ERR_NOT_FOUND) { /* the profile folder was removed: only it is made again */
            const char *folder = web_profile_folder(false);
            n_result made = folder ? n_folder_create(folder) : -N_ERR_NOT_FOUND;
            h = made >= 0 || made == -N_ERR_EXISTS ? n_document_open(path, N_DOCUMENT_CREATE) : made;
        }
        if (h == -N_ERR_EXISTS) h = n_document_open(path, N_DOCUMENT_EDIT); /* made meanwhile */
    }
    if (h < 0) return h;
    n_result r = n_document_claim(h, true);
    struct prefs now;
    bool parsed = false;
    if (r >= 0) {
        char *old = NULL, *next = NULL, *value = NULL;
        size_t on = 0, nn = 0;
        r = read_all(h, &old, &on);
        if (r >= 0) r = make(old, on, arg, &value);
        if (r >= 0) r = document_make(old, on, name, value, &next, &nn);
        if (r >= 0 && (nn != on || memcmp(next, old, nn))) r = n_document_save(path, next, nn);
        if (r >= 0) parsed = prefs_parse(next, nn, &now);
        free(old);
        free(next);
        free(value);
        n_document_release(h);
    }
    n_handle_close(h);
    if (r < 0) return r;
    if (queue >= 0 && watch < 0) (void)watch_open_locked(); /* its folder may be there again */
    if (!parsed) { /* saved, but no memory to hold it: the next use reads it */
        __atomic_store_n(&valid, false, __ATOMIC_RELEASE);
        return 0;
    }
    apply(&now);
    __atomic_store_n(&valid, queue < 0 || watch >= 0, __ATOMIC_RELEASE);
    return 0;
}

static n_result edit_with(const char *name, value_of make, const void *arg) {
    mtx_lock(&io);
    n_result r = edit_locked(name, make, arg);
    mtx_unlock(&io);
    return r;
}

static n_result edit(const char *name, const char *value) { return edit_with(name, fixed_value, value); }

n_result webprefs_set_languages(const char *const *tags, size_t count) {
    struct list l;
    n_result r = list_make(tags, count, &l);
    return r < 0 ? r : edit("languages", l.text);
}

n_result webprefs_reset_languages(void) { return edit("languages", NULL); }

n_result webprefs_set_search_history(bool on) { return edit("search-history", on ? "on" : "off"); }

n_result webprefs_set_clear_on_close(bool on) { return edit("clear-site-data-on-close", on ? "on" : "off"); }

/* The value of site-data-exceptions= once the document old has host's rule changed. */
struct site_change {
    const char *host;
    enum webprefs_site_rule rule;
};

static n_result sites_value(const char *old, size_t on, const void *arg, char **value) {
    const struct site_change *change = arg;
    struct prefs p;
    if (!prefs_parse(old, on, &p)) return -N_ERR_NO_MEMORY;
    struct webprefs_site_exception *sites = p.sites;
    size_t count = p.site_count, at = count;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(sites[i].host, change->host)) at = i;
    if (change->rule == WEBPREFS_SITE_DEFAULT) {
        if (at < count) {
            memmove(sites + at, sites + at + 1, (count - at - 1) * sizeof *sites);
            count--;
        }
    } else if (at < count) {
        sites[at].rule = change->rule;
    } else {
        struct webprefs_site_exception *grown = realloc(sites, (count + 1) * sizeof *grown);
        if (!grown) {
            free(sites);
            return -N_ERR_NO_MEMORY;
        }
        sites = grown;
        memset(&sites[count], 0, sizeof *sites);
        memcpy(sites[count].host, change->host, strlen(change->host) + 1);
        sites[count++].rule = change->rule;
        qsort(sites, count, sizeof *sites, site_compare);
    }
    bool none;
    *value = sites_write(sites, count, &none);
    free(sites);
    return *value || none ? 0 : -N_ERR_NO_MEMORY;
}

n_result webprefs_set_site_rule(const char *host, enum webprefs_site_rule rule) {
    if (!host || !host_valid(host, strlen(host)) || rule < WEBPREFS_SITE_DEFAULT || rule > WEBPREFS_SITE_BLOCK)
        return -N_ERR_INVALID;
    struct site_change change = {host, rule};
    return edit_with("site-data-exceptions", sites_value, &change);
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

/* The host of an http(s) or ws(s) URL in out. */
static bool url_host(const char *url, char out[WEBPREFS_HOST_MAX + 1]) {
    static const char *const schemes[] = {"http://", "https://", "ws://", "wss://"};
    for (size_t i = 0; url && i < sizeof schemes / sizeof *schemes; i++) {
        size_t k = strlen(schemes[i]), m = 0;
        while (m < k && lower(url[m]) == schemes[i][m]) m++;
        if (m == k) return authority_host(url + k, out);
    }
    return false;
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
    return url_host(url, key) ? site_of_key(key, &kept) : WEBPREFS_SITE_DEFAULT;
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
