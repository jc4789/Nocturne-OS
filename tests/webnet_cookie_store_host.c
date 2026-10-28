/* The cookie store of user/libc/webnet.c (cookie_sync and the functions around it), cut from the
   product source by check_webnet_cookie_store.py and built with the product user/libc/webcookie.c.
   Several jars stand for several window processes sharing the document "cookies" of the profile
   folder; the document calls (n_document_*) are one document in memory with its claim, and the
   watch is absent (N_ERR_UNSUPPORTED), so a process reads the store at its own reads and saves
   (cookie_sync), as it does when another process's save is reported. A bounded proof: not the
   kernel's documents and watches, not a guest browser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <nocturne.h>
#include <webcookie.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef struct webnet webnet;
struct webnet {
    webcookie_jar *cookies;
    bool debug;
    char cookie_path[1024];
    n_handle cookie_watch, cookie_queue;
    bool cookie_stale;
    uint64_t cookie_due, cookie_rules;
};
static uint64_t clock_ms = 1000;
uint64_t uptime_ms(void) { return clock_ms; }

static unsigned char *doc;
static size_t doc_n;
static bool doc_exists;
static int opened, claims, saves;
const char *web_profile_folder(bool temporary) { return temporary ? "C:\\Temp\\Browser" : "C:\\Users\\User\\AppData\\Browser"; }
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s\\%s", folder, name);
    return n < 0 || (size_t)n >= cap ? -N_ERR_TOO_LARGE : n;
}
n_handle n_document_open(const char *path, int access) {
    (void)path;
    if (access == N_DOCUMENT_CREATE) {
        if (doc_exists) return -N_ERR_EXISTS;
        doc_exists = true;
    } else if (!doc_exists) return -N_ERR_NOT_FOUND;
    opened++;
    return 9;
}
n_result n_document_size(n_handle h) { (void)h; return (n_result)doc_n; }
n_result n_document_read_at(n_handle h, uint64_t at, void *buf, size_t n) {
    (void)h;
    if (at >= doc_n) return 0;
    if (n > doc_n - at) n = doc_n - (size_t)at;
    memcpy(buf, doc + at, n);
    return (n_result)n;
}
n_result n_document_claim(n_handle h, bool wait) { (void)h; (void)wait; claims++; return 0; }
n_result n_document_release(n_handle h) { (void)h; claims--; return 0; }
n_result n_document_save(const char *path, const void *data, size_t size) {
    (void)path;
    free(doc);
    doc = malloc(size ? size : 1);
    memcpy(doc, data, size);
    doc_n = size;
    doc_exists = true;
    saves++;
    return 0;
}
n_result n_handle_close(n_handle h) { (void)h; opened--; return 0; }
n_result n_folder_create(const char *path) { (void)path; return 0; }
n_handle n_entry_watch(const char *path, uint32_t what) { (void)path; (void)what; return -N_ERR_UNSUPPORTED; }
n_result n_events_watch(n_handle q, n_handle h, uint32_t events, uint64_t token) { (void)q; (void)h; (void)events; (void)token; return -N_ERR_WRONG_KIND; }
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 0; }
n_result n_watch_take(n_handle w) { (void)w; return 0; }

/* The user's choices: temp.test's data is not kept, blocked.test's cookies are refused. */
static uint64_t rules_generation = 1;
static bool k_transient;
uint64_t webprefs_site_generation(void) { return rules_generation; }
static bool host_in(const char *host, const char *site) {
    size_t h = strlen(host), s = strlen(site);
    return h == s ? !strcmp(host, site) : h > s && host[h - s - 1] == '.' && !strcmp(host + h - s, site);
}
static unsigned test_rule(void *opaque, const char *host) {
    (void)opaque;
    if (host_in(host, "blocked.test")) return WEBCOOKIE_BLOCK;
    if (host_in(host, "temp.test") || (k_transient && host_in(host, "k.test"))) return WEBCOOKIE_TRANSIENT;
    return WEBCOOKIE_ALLOW;
}

#include "store.inc"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL webnet_cookie_store:%d: %s\n", __LINE__, #c); failed++; } } while (0)

static char *psl;
static size_t psl_n;
/* A window process's network as webnet_create makes it, with the test's rule. */
static webnet *window(void) {
    webnet *n = calloc(1, sizeof *n);
    n->cookie_watch = n->cookie_queue = -1;
    n->cookies = webcookie_create();
    if (psl) webcookie_psl_load(n->cookies, psl, psl_n);
    webcookie_track(n->cookies);
    webcookie_set_rule(n->cookies, test_rule, NULL);
    n->cookie_rules = webprefs_site_generation();
    (void)cookie_sync(n, false);
    webcookie_rule_changed(n->cookies);
    cookie_note(n, uptime_ms());
    return n;
}
static void window_free(webnet *n) {
    if (n->cookie_rules != webprefs_site_generation()) webcookie_rule_changed(n->cookies);
    (void)cookie_sync(n, true);
    cookie_watch_close(n);
    webcookie_free(n->cookies);
    free(n);
}
static int set(webnet *n, const char *url, const char *value) {
    struct webcookie_context c = {url, url, "GET", true, true, false};
    int r = webcookie_set(n->cookies, &c, value, strlen(value), time(NULL));
    cookie_note(n, uptime_ms());
    return r;
}
static bool sends(webnet *n, const char *url, const char *pair) {
    struct webcookie_context c = {url, url, "GET", true, true, false};
    char out[4096];
    if (webcookie_get(n->cookies, &c, out, sizeof out, time(NULL)) < 0) return false;
    size_t k = strlen(pair);
    for (const char *p = out; (p = strstr(p, pair)); p++)
        if ((p == out || p[-1] == ' ') && (p[k] == ';' || !p[k])) return true;
    return false;
}
static bool settled(void) { return !opened && !claims; }
/* The pump a second later: what waits is saved. */
static void later(webnet *n) { clock_ms += 1000; cookie_pump(n, uptime_ms()); }

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "rootfs/Nocturne/Web/public_suffix_list.dat", "rb");
    if (f) {
        psl = malloc(WEBCOOKIE_PSL_MAX + 1);
        psl_n = fread(psl, 1, WEBCOOKIE_PSL_MAX, f);
        fclose(f);
    }
    CHECK(psl && psl_n);

    /* Saved a second after the first change, persistent cookies only. */
    webnet *a = window(), *b = window();
    CHECK(!doc_exists && settled());
    CHECK(set(a, "https://www.a.test/", "p=1; Max-Age=3600") == 1 && set(a, "https://www.a.test/", "s=1") == 1);
    clock_ms += 999;
    cookie_pump(a, uptime_ms());
    CHECK(!saves && a->cookie_due);
    clock_ms += 1;
    cookie_pump(a, uptime_ms());
    CHECK(saves == 1 && !a->cookie_due && !webcookie_changed(a->cookies) && settled());

    /* Another window reads it: the persistent cookie, never the session one. */
    CHECK(cookie_sync(b, false) == 0 && settled());
    CHECK(sends(b, "https://www.a.test/", "p=1") && !sends(b, "https://www.a.test/", "s=1"));

    /* Two windows change the store meanwhile: each save reads what is there first. */
    CHECK(set(b, "https://www.a.test/", "q=2; Max-Age=3600") == 1 && set(a, "https://www.a.test/", "r=3; Max-Age=3600") == 1);
    later(b);
    later(a);
    CHECK(sends(a, "https://www.a.test/", "q=2") && sends(a, "https://www.a.test/", "r=3"));
    CHECK(cookie_sync(b, false) == 0 && sends(b, "https://www.a.test/", "r=3") && sends(b, "https://www.a.test/", "p=1"));
    CHECK(settled());
    /* The URL Standard keeps '\' in a special URL's query and fragment; before them it is not
       the parser's serialization. */
    CHECK(sends(a, "https://www.a.test/log?x\\y|z#f\\g", "r=3"));
    {
        struct webcookie_context c = {"https://www.a.test/p\\q", "https://www.a.test/p\\q", "GET", true, true, false};
        char out[64];
        CHECK(webcookie_get(a->cookies, &c, out, sizeof out, time(NULL)) < 0);
    }

    /* What the Settings window lists, and a site's key. */
    struct webcookie_site *sites = NULL;
    long n = webnet_cookie_sites(a, &sites);
    CHECK(n == 1 && !strcmp(sites[0].site, "a.test") && sites[0].cookies == 4);
    free(sites);
    char site[254];
    CHECK(webnet_site(a, "WWW.Shop.Example.CO.UK.", site, sizeof site) && !strcmp(site, "example.co.uk"));
    CHECK(webnet_site(a, "192.168.1.1", site, sizeof site) && !strcmp(site, "192.168.1.1"));
    CHECK(!webnet_site(a, "example.com", site, 5));

    /* Clearing a site in one window: saved at once; the other removes its own, session ones too. */
    CHECK(set(b, "https://b.test/", "keep=1; Max-Age=3600") == 1);
    int before = saves;
    CHECK(webnet_cookie_clear(b, "a.test") == 3 && saves == before + 1 && !b->cookie_due);
    CHECK(cookie_sync(a, false) == 0);
    CHECK(!sends(a, "https://www.a.test/", "p=1") && !sends(a, "https://www.a.test/", "r=3"));
    CHECK(!sends(a, "https://www.a.test/", "s=1")); /* a's session cookie: the clear reached it */
    CHECK(sends(a, "https://b.test/", "keep=1"));
    CHECK(settled());

    /* A host whose data is not kept: its persistent cookie lives in the window only. */
    CHECK(set(a, "https://temp.test/", "t=1; Max-Age=3600") == 1);
    later(a);
    webnet *c = window();
    CHECK(sends(a, "https://temp.test/", "t=1") && !sends(c, "https://temp.test/", "t=1"));

    /* The user's rule changes: a cookie no longer kept leaves the store, stays in its window. */
    CHECK(set(a, "https://k.test/", "k=1; Max-Age=3600") == 1);
    later(a);
    CHECK(cookie_sync(c, false) == 0 && sends(c, "https://k.test/", "k=1"));
    k_transient = true;
    rules_generation++;
    later(a); /* the pump sees the rule, then saves what changed */
    later(a);
    webnet *d = window();
    CHECK(!sends(d, "https://k.test/", "k=1") && sends(a, "https://k.test/", "k=1"));
    /* c read k=1 from the store while it was kept: the store no longer has it, so c's goes too. */
    CHECK(cookie_sync(c, false) == 0 && !sends(c, "https://k.test/", "k=1"));
    k_transient = false;
    rules_generation++;

    /* A blocked host: no cookie set, none sent. */
    CHECK(set(a, "https://blocked.test/", "x=1; Max-Age=3600") == 0 && !sends(a, "https://blocked.test/", "x=1"));

    /* A store this build cannot read: nothing is merged, and the next save replaces it. */
    n_document_save(NULL, "garbage", 7);
    webnet *e = window();
    CHECK(e && !sends(e, "https://b.test/", "keep=1") && settled());
    CHECK(set(e, "https://e.test/", "e=1; Max-Age=3600") == 1);
    later(e);
    webnet *g = window();
    CHECK(sends(g, "https://e.test/", "e=1") && doc_n > 7 && !memcmp(doc, "NCS1", 4));

    /* A window closing saves what waits. */
    CHECK(set(g, "https://g.test/", "late=1; Max-Age=3600") == 1 && g->cookie_due);
    window_free(g);
    webnet *h = window();
    CHECK(sends(h, "https://g.test/", "late=1") && settled());

    window_free(a); window_free(b); window_free(c); window_free(d); window_free(e); window_free(h);
    CHECK(settled());
    free(psl);
    free(doc);
    printf("webnet_cookie_store: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
