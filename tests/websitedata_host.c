/* user/libc/websitedata.c as it is, with each store's functions (webnet, webstorage, webidb,
   js_opfs, webcache), the user's choices (webprefs), the process list and the temporary folder's document
   "windows" replaced by small fakes: the sites put together, a site's clear reaching every store,
   and the browser session's first and last window. A bounded proof (check_websitedata.py): not
   the stores themselves, not a guest browser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <websitedata.h>
#include <webcache.h>
#include <webcookie.h>
#include <webidb.h>
#include <webprefs.h>

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL websitedata:%d: %s\n", __LINE__, #c); failed++; } } while (0)

static bool within(const char *host, const char *site) {
    size_t h = strlen(host), s = strlen(site);
    return h == s ? !strcmp(host, site) : h > s && host[h - s - 1] == '.' && !strcmp(host + h - s, site);
}
static char cleared[24][64];
static int cleared_n;
static void note(const char *what, const char *arg) { if (cleared_n < 24) snprintf(cleared[cleared_n++], 64, "%s:%s", what, arg); }
static bool was(const char *what) { for (int i = 0; i < cleared_n; i++) if (!strcmp(cleared[i], what)) return true; return false; }

/* Cookies. */
long webnet_cookie_sites(webnet *n, struct webcookie_site **out) {
    (void)n;
    *out = calloc(2, sizeof **out);
    strcpy((*out)[0].site, "a.test"); (*out)[0].cookies = 2; (*out)[0].bytes = 100;
    strcpy((*out)[1].site, "b.test"); (*out)[1].cookies = 1; (*out)[1].bytes = 50;
    return 2;
}
long webnet_cookie_clear(webnet *n, const char *site) { (void)n; note("cookies", site); return 1; }
bool webnet_site(const webnet *n, const char *host, char *out, size_t cap) {
    (void)n;
    const char *dot = strrchr(host, '.'), *site = host;
    for (const char *p = host; dot && p < dot; p++) if (*p == '.') site = p + 1;
    if (strlen(site) >= cap) return false;
    strcpy(out, site);
    return true;
}
/* Web Storage. */
static const char *const storage_origins[] = {"https://www.a.test", "https://keep.c.test", "https://c.test", "https://d.c.test"};
long webstorage_usage(webstorage *store, struct web_storage_usage **out) {
    (void)store;
    size_t n = sizeof storage_origins / sizeof *storage_origins;
    *out = calloc(n, sizeof **out);
    for (size_t i = 0; i < n; i++) { (*out)[i].origin = strdup(storage_origins[i]); (*out)[i].local = 10; (*out)[i].session = 5; (*out)[i].disk = 64; }
    return (long)n;
}
void webstorage_usage_free(struct web_storage_usage *usage, size_t count) { for (size_t i = 0; i < count; i++) free(usage[i].origin); free(usage); }
int webstorage_clear_site(webstorage *store, const char *domain) { (void)store; note("storage", domain); return WEB_STORAGE_OK; }
int webstorage_clear_all(webstorage *store) { (void)store; note("storage", "*"); return WEB_STORAGE_OK; }
/* IndexedDB and OPFS. */
static const char *const idb_origins[] = {"https://a.test", "http://x.b.test:8080", "https://c.test"};
int webidb_usage(void (*each)(void *, const char *, uint64_t), void *opaque) {
    each(opaque, idb_origins[0], 1000); each(opaque, idb_origins[1], 300); each(opaque, idb_origins[2], 1);
    return WEB_IDB_OK;
}
int webidb_clear(bool (*match)(void *, const char *), void *opaque) {
    for (size_t i = 0; i < 3; i++) if (!match || match(opaque, idb_origins[i])) note("idb", idb_origins[i]);
    return WEB_IDB_OK;
}
static const char *const opfs_origins[] = {"", "https://a.test"};
n_result web_opfs_usage(void (*each)(void *, const char *, uint64_t), void *opaque) {
    each(opaque, opfs_origins[0], 7); each(opaque, opfs_origins[1], 20);
    return 0;
}
n_result web_opfs_clear(bool (*match)(void *, const char *), void *opaque) {
    for (size_t i = 0; i < 2; i++) if (!match || match(opaque, opfs_origins[i])) note("opfs", *opfs_origins[i] ? opfs_origins[i] : "?");
    return match && match(opaque, "https://a.test") ? -N_ERR_BUSY : 0; /* a.test has a file open for sync access */
}
/* Cache Storage: purge is the first window's. */
static const char *const cache_origins[] = {"https://a.test", "https://c.test:444"};
int webcache_usage(void (*each)(void *, const char *, uint64_t), void *opaque) {
    each(opaque, cache_origins[0], 50); each(opaque, cache_origins[1], 9);
    return WEB_CACHE_OK;
}
int webcache_clear(bool (*match)(void *, const char *), void *opaque) {
    for (size_t i = 0; i < 2; i++) if (!match || match(opaque, cache_origins[i])) note("cache", cache_origins[i]);
    return WEB_CACHE_OK;
}
int webcache_purge(void) { note("purge", ""); return WEB_CACHE_OK; }
/* The user's choices: c.test's data goes when the browser closes, keep.c.test's stays. */
static bool clear_on_close;
void webprefs_sync(void) {}
bool webprefs_site_data_transient(void) { return true; }
bool webprefs_clear_on_close(void) { return clear_on_close; }
bool webprefs_site_kept(const char *host) {
    if (within(host, "keep.c.test")) return true;
    return !within(host, "c.test") && !clear_on_close;
}
/* Processes and the temporary folder's "windows". */
static struct n_procinfo procs[8];
static int proc_count, self = 10;
n_result proclist(struct n_procinfo *out, int max) { int n = proc_count < max ? proc_count : max; memcpy(out, procs, (size_t)n * sizeof *out); return n; }
n_result n_app_self(void) { return self; }
static void run(int pid, uint64_t start) { procs[proc_count].pid = pid; procs[proc_count].start_ms = start; procs[proc_count++].state = 1; }
static void end(int pid) { for (int i = 0; i < proc_count; i++) if (procs[i].pid == pid) procs[i] = procs[--proc_count]; }
static unsigned char windows[256];
static size_t windows_n;
static bool windows_exist;
static int opened, claimed;
const char *web_profile_folder(bool temporary) { return temporary ? "C:\\Temp\\Browser" : NULL; }
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) { return snprintf(out, cap, "%s\\%s", folder, name); }
n_handle n_document_open(const char *path, int access) {
    if (strcmp(path, "C:\\Temp\\Browser\\windows")) return -N_ERR_NOT_FOUND;
    if (access == N_DOCUMENT_CREATE) { if (windows_exist) return -N_ERR_EXISTS; windows_exist = true; }
    else if (!windows_exist) return -N_ERR_NOT_FOUND;
    opened++;
    return 5;
}
n_result n_document_claim(n_handle h, bool wait) { (void)h; (void)wait; claimed++; return 0; }
n_result n_document_release(n_handle h) { (void)h; claimed--; return 0; }
n_result n_document_size(n_handle h) { (void)h; return (n_result)windows_n; }
n_result n_document_read_at(n_handle h, uint64_t at, void *buf, size_t n) {
    (void)h;
    if (at >= windows_n) return 0;
    if (n > windows_n - at) n = windows_n - (size_t)at;
    memcpy(buf, windows + at, n);
    return (n_result)n;
}
n_result n_document_save(const char *path, const void *data, size_t size) { (void)path; memcpy(windows, data, size); windows_n = size; return 0; }
n_result n_handle_close(n_handle h) { (void)h; opened--; return 0; }

static bool session_cleared(void) { return was("idb:https://c.test"); }

int main(void) {
    /* The sites, put together from every store. */
    webnet *net = (webnet *)1;
    webstorage *store = (webstorage *)1;
    struct websitedata_site *sites = NULL;
    long n = websitedata_sites(net, store, &sites);
    CHECK(n == 4);
    if (n == 4) {
        CHECK(!strcmp(sites[0].site, "") && sites[0].opfs_bytes == 7);
        CHECK(!strcmp(sites[1].site, "a.test") && sites[1].cookies == 2 && sites[1].cookie_bytes == 100 &&
              sites[1].storage_bytes == 15 && sites[1].indexeddb_bytes == 1000 && sites[1].opfs_bytes == 20 && sites[1].cache_bytes == 50);
        CHECK(websitedata_bytes(&sites[1]) == 1185);
        CHECK(!strcmp(sites[2].site, "b.test") && sites[2].cookies == 1 && sites[2].indexeddb_bytes == 300 && !sites[2].cache_bytes);
        CHECK(!strcmp(sites[3].site, "c.test") && sites[3].storage_bytes == 45 && sites[3].indexeddb_bytes == 1 && sites[3].cache_bytes == 9);
    }
    free(sites);

    /* One site's clear reaches each store, and only that site's origins; something in use: busy. */
    CHECK(websitedata_clear(net, store, "a.test") == -N_ERR_BUSY);
    CHECK(was("cookies:a.test") && was("storage:a.test") && was("idb:https://a.test") && was("opfs:https://a.test") && was("cache:https://a.test"));
    CHECK(!was("idb:http://x.b.test:8080") && !was("idb:https://c.test") && !was("opfs:?") && !was("cache:https://c.test:444"));
    cleared_n = 0;
    CHECK(websitedata_clear(net, store, "b.test") == 0 && was("idb:http://x.b.test:8080") && cleared_n == 3);
    cleared_n = 0;
    CHECK(websitedata_clear(net, store, NULL) == -N_ERR_BUSY && was("cookies:") && was("storage:*") && was("opfs:?") &&
          was("cache:https://a.test") && was("cache:https://c.test:444") && cleared_n == 9);

    /* The session: the first window clears what the last session left, the last clears on close. */
    cleared_n = 0;
    run(10, 100);
    self = 10;
    websitedata_session_begin(store);
    CHECK(session_cleared() && windows_n == 16 && !opened && !claimed);
    CHECK(was("storage:d.c.test") && !was("storage:c.test") && !was("storage:keep.c.test") && !was("idb:https://a.test"));
    CHECK(was("cache:https://c.test:444") && !was("cache:https://a.test") && was("purge:"));
    cleared_n = 0;
    run(11, 200);
    self = 11;
    websitedata_session_begin(store);
    CHECK(!session_cleared() && windows_n == 32 && !was("purge:"));
    self = 10;
    websitedata_session_end(store);
    end(10);
    CHECK(!session_cleared() && windows_n == 16);
    self = 11;
    websitedata_session_end(store);
    end(11);
    CHECK(session_cleared() && windows_n == 0 && was("cache:https://c.test:444") && !was("purge:"));

    /* A window that was killed leaves its row: an ended process's row counts for nothing. */
    cleared_n = 0;
    run(12, 300);
    self = 12;
    websitedata_session_begin(store);
    end(12);
    run(13, 400);
    self = 13;
    cleared_n = 0;
    websitedata_session_begin(store);
    CHECK(session_cleared() && windows_n == 16);
    /* The pid used again by another process: its start time tells them apart. */
    run(12, 999);
    self = 12;
    cleared_n = 0;
    websitedata_session_begin(store);
    CHECK(!session_cleared() && windows_n == 32 && !opened && !claimed);

    /* With everything cleared on close, an OPFS directory without an origin goes too. */
    clear_on_close = true;
    end(12); end(13);
    self = 14;
    run(14, 500);
    cleared_n = 0;
    websitedata_session_begin(store);
    CHECK(was("opfs:?") && was("idb:https://a.test") && was("storage:keep.c.test") == false);

    printf("websitedata: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
