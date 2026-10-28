/* The user's site data across the browser's stores (websitedata.h): what the Settings window
   lists and clears, and the clearing that ends a browser session. Each store keeps and clears its
   own data (webnet.c, webstorage.c, webidb.c, web/js_opfs.c, webcache.c); this puts them together
   by site. */
#include "websitedata.h"
#include "webcache.h"
#include "webcookie.h"
#include "webidb.h"
#include "webprefs.h"
#include "web/js_opfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The host of a serialized origin ("https://www.example.com:8443": www.example.com), as its URL
   record has it (weburl.h). */
static bool origin_host(const char *origin, char out[256]) {
    struct web_url url;
    if (!origin || web_url_read(origin, strlen(origin), &url) != 1) return false;
    size_t n;
    const char *host = web_url_host_text(&url, &n);
    bool ok = n && n < 256;
    if (ok) {
        memcpy(out, host, n);
        out[n] = 0;
    }
    web_url_free(&url);
    return ok;
}

/* Whether host is site or inside it ("" holds every host). */
static bool within(const char *host, const char *site) {
    if (!*site) return true;
    size_t h = strlen(host), s = strlen(site);
    if (h == s) return !strcmp(host, site);
    return h > s && host[h - s - 1] == '.' && !strcmp(host + h - s, site);
}

struct gather {
    struct websitedata_site *items;
    size_t count, cap;
    webnet *net;
    n_result failed;
};

/* A new entry for site (merged with the others of its site at the end). */
static struct websitedata_site *entry(struct gather *g, const char *site) {
    if (g->failed) return NULL;
    if (g->count == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 64;
        struct websitedata_site *grown = realloc(g->items, cap * sizeof *grown);
        if (!grown) { g->failed = -N_ERR_NO_MEMORY; return NULL; }
        g->items = grown;
        g->cap = cap;
    }
    struct websitedata_site *e = &g->items[g->count++];
    memset(e, 0, sizeof *e);
    snprintf(e->site, sizeof e->site, "%s", site);
    return e;
}

static struct websitedata_site *origin_entry(struct gather *g, const char *origin) {
    char host[256], site[254] = "";
    if (origin_host(origin, host) && !webnet_site(g->net, host, site, sizeof site)) snprintf(site, sizeof site, "%s", host);
    return entry(g, site);
}

static void idb_each(void *opaque, const char *origin, uint64_t bytes) {
    struct websitedata_site *e = origin_entry(opaque, origin);
    if (e) e->indexeddb_bytes = bytes;
}

static void opfs_each(void *opaque, const char *origin, uint64_t bytes) {
    struct websitedata_site *e = origin_entry(opaque, origin);
    if (e) e->opfs_bytes = bytes;
}

static void cache_each(void *opaque, const char *origin, uint64_t bytes) {
    struct websitedata_site *e = origin_entry(opaque, origin);
    if (e) e->cache_bytes = bytes;
}

static int site_order(const void *a, const void *b) {
    return strcmp(((const struct websitedata_site *)a)->site, ((const struct websitedata_site *)b)->site);
}

long websitedata_sites(webnet *net, webstorage *store, struct websitedata_site **out) {
    if (!out) return -N_ERR_INVALID;
    *out = NULL;
    if (!net) return -N_ERR_INVALID;
    struct gather g = {.net = net};
    struct webcookie_site *cookies = NULL;
    long n = webnet_cookie_sites(net, &cookies);
    if (n < 0) return -N_ERR_NO_MEMORY;
    for (long i = 0; i < n; i++) {
        struct websitedata_site *e = entry(&g, cookies[i].site);
        if (e) { e->cookies = cookies[i].cookies; e->cookie_bytes = cookies[i].bytes; }
    }
    free(cookies);
    if (store && !g.failed) {
        struct web_storage_usage *usage = NULL;
        long u = webstorage_usage(store, &usage);
        if (u < 0) g.failed = -N_ERR_IO;
        for (long i = 0; i < u; i++) {
            struct websitedata_site *e = origin_entry(&g, usage[i].origin);
            if (e) e->storage_bytes = usage[i].local + usage[i].session;
        }
        if (u > 0) webstorage_usage_free(usage, (size_t)u);
    }
    if (!g.failed) {
        int r = webidb_usage(idb_each, &g);
        if (r && !g.failed) g.failed = r == WEB_IDB_MEMORY ? -N_ERR_NO_MEMORY : -N_ERR_IO;
    }
    if (!g.failed) {
        n_result r = web_opfs_usage(opfs_each, &g);
        if (r < 0 && !g.failed) g.failed = r;
    }
    if (!g.failed) {
        int r = webcache_usage(cache_each, &g);
        if (r && !g.failed) g.failed = r == WEB_CACHE_MEMORY ? -N_ERR_NO_MEMORY : -N_ERR_IO;
    }
    if (g.failed) { free(g.items); return g.failed; }
    if (!g.count) { free(g.items); return 0; }
    qsort(g.items, g.count, sizeof *g.items, site_order);
    size_t k = 0;
    for (size_t i = 0; i < g.count; i++) {
        struct websitedata_site *e = &g.items[i];
        if (k && !strcmp(g.items[k - 1].site, e->site)) {
            struct websitedata_site *t = &g.items[k - 1];
            t->cookies += e->cookies;
            t->cookie_bytes += e->cookie_bytes;
            t->storage_bytes += e->storage_bytes;
            t->indexeddb_bytes += e->indexeddb_bytes;
            t->opfs_bytes += e->opfs_bytes;
            t->cache_bytes += e->cache_bytes;
        } else {
            g.items[k++] = *e;
        }
    }
    *out = g.items;
    return (long)k;
}

uint64_t websitedata_bytes(const struct websitedata_site *s) {
    return s ? s->cookie_bytes + s->storage_bytes + s->indexeddb_bytes + s->opfs_bytes + s->cache_bytes : 0;
}

static bool origin_in_site(void *opaque, const char *origin) {
    const char *site = opaque;
    char host[256];
    if (!*site) return true;
    return origin_host(origin, host) && within(host, site);
}

n_result websitedata_clear(webnet *net, webstorage *store, const char *site) {
    if (!site) site = "";
    n_result first = 0;
    bool busy = false;
    if (net) {
        long c = webnet_cookie_clear(net, site);
        if (c < 0) first = (n_result)c;
    }
    if (store) {
        int w = *site ? webstorage_clear_site(store, site) : webstorage_clear_all(store);
        if (w != WEB_STORAGE_OK && !first) first = -N_ERR_IO;
    }
    int i = webidb_clear(origin_in_site, (void *)site);
    if (i == WEB_IDB_BUSY) busy = true;
    else if (i && !first) first = i == WEB_IDB_MEMORY ? -N_ERR_NO_MEMORY : -N_ERR_IO;
    n_result o = web_opfs_clear(origin_in_site, (void *)site);
    if (o == -N_ERR_BUSY) busy = true;
    else if (o < 0 && o != -N_ERR_NOT_FOUND && !first) first = o;
    int c = webcache_clear(origin_in_site, (void *)site);
    if (c == WEB_CACHE_BUSY) busy = true;
    else if (c && !first) first = c == WEB_CACHE_MEMORY ? -N_ERR_NO_MEMORY : c == WEB_CACHE_QUOTA ? -N_ERR_NO_SPACE : -N_ERR_IO;
    return first ? first : busy ? -N_ERR_BUSY : 0;
}

/* The end of a session: the storage of every origin whose host the user's choices do not keep.
   An OPFS directory without a noted origin goes when the browser clears all site data on close. */
static bool origin_transient(void *opaque, const char *origin) {
    (void)opaque;
    char host[256];
    return origin_host(origin, host) ? !webprefs_site_kept(host) : webprefs_clear_on_close();
}

static void clear_transient(webstorage *store) {
    webprefs_sync(); /* the choice as it is now, not as this window last heard it */
    if (!webprefs_site_data_transient()) return;
    (void)webidb_clear(origin_transient, NULL);
    (void)web_opfs_clear(origin_transient, NULL);
    (void)webcache_clear(origin_transient, NULL);
    if (!store) return;
    /* Web Storage clears a host with the hosts inside it: a host whose data goes is cleared unless
       a host inside it keeps its own (an "allow" exception for a subdomain); that one's stays. */
    struct web_storage_usage *usage = NULL;
    long n = webstorage_usage(store, &usage);
    for (long i = 0; i < n; i++) {
        char host[256], inner[256];
        if (!origin_host(usage[i].origin, host) || webprefs_site_kept(host)) continue;
        bool keeps = false;
        for (long k = 0; k < n && !keeps; k++)
            keeps = k != i && origin_host(usage[k].origin, inner) && strcmp(inner, host) && within(inner, host) && webprefs_site_kept(inner);
        if (!keeps) (void)webstorage_clear_site(store, host);
    }
    if (n > 0) webstorage_usage_free(usage, (size_t)n);
}

/* The live windows' rows (process id, start time: 8 bytes each), in the temporary folder. */
#define WINDOW_ROW 16u

static uint64_t row64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }
static void put_row64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int processes(struct n_procinfo **out) {
    for (int capacity = 64;; capacity *= 2) {
        struct n_procinfo *p = malloc((size_t)capacity * sizeof *p);
        if (!p) return -N_ERR_NO_MEMORY;
        n_result n = proclist(p, capacity);
        if (n < 0) { free(p); return (int)n; }
        if (n < capacity || capacity > (1 << 20)) { *out = p; return (int)n; }
        free(p);
    }
}

/* Under the document's claim: drops the rows of ended processes and this process's, adds this
   process's with join, and saves. How many other windows live, or the failure. */
static n_result windows_update(bool join) {
    const char *folder = web_profile_folder(true);
    char path[1024];
    if (!folder || n_path_join(folder, "windows", path, sizeof path) < 0) return -N_ERR_NOT_FOUND;
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h == -N_ERR_NOT_FOUND) {
        h = n_document_open(path, N_DOCUMENT_CREATE);
        if (h == -N_ERR_EXISTS) h = n_document_open(path, N_DOCUMENT_EDIT);
    }
    if (h < 0) return h;
    n_result r = n_document_claim(h, true);
    if (r < 0) { n_handle_close(h); return r; }
    struct n_procinfo *procs = NULL;
    int count = processes(&procs);
    n_result size = n_document_size(h);
    uint8_t *rows = NULL;
    size_t n = size > 0 && size % WINDOW_ROW == 0 ? (size_t)size : 0, kept = 0;
    if (count < 0) r = count;
    else if (size < 0) r = size;
    else if (!(rows = malloc(n + WINDOW_ROW))) r = -N_ERR_NO_MEMORY;
    if (r >= 0) {
        size_t at = 0;
        while (at < n) {
            n_result got = n_document_read_at(h, at, rows + at, n - at);
            if (got <= 0) { n = at - at % WINDOW_ROW; break; }
            at += (size_t)got;
        }
        uint64_t self = (uint32_t)n_app_self(), start = 0;
        for (int i = 0; i < count; i++) if ((uint64_t)(uint32_t)procs[i].pid == self) start = procs[i].start_ms;
        for (size_t i = 0; i + WINDOW_ROW <= n; i += WINDOW_ROW) {
            uint64_t pid = row64(rows + i), when = row64(rows + i + 8);
            if (pid == self && when == start) continue;
            bool alive = false;
            for (int k = 0; k < count && !alive; k++)
                alive = (uint64_t)(uint32_t)procs[k].pid == pid && (!when || procs[k].start_ms == when) && procs[k].state != 3;
            if (alive) { memmove(rows + kept, rows + i, WINDOW_ROW); kept += WINDOW_ROW; }
        }
        r = (n_result)(kept / WINDOW_ROW);
        if (join) { put_row64(rows + kept, self); put_row64(rows + kept + 8, start); kept += WINDOW_ROW; }
        n_result saved = n_document_save(path, rows, kept);
        if (saved < 0) r = saved;
    }
    free(rows);
    free(procs);
    n_document_release(h);
    n_handle_close(h);
    return r;
}

void websitedata_session_begin(webstorage *store) {
    if (windows_update(true)) return;
    clear_transient(store);
    /* No window of an earlier session is left to hold a Cache object of a deleted cache. */
    (void)webcache_purge();
}

void websitedata_session_end(webstorage *store) {
    if (!windows_update(false)) clear_transient(store);
}
