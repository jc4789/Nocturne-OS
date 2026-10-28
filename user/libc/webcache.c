/* Cache Storage's storage (webcache.h): Service Workers, "Caches",
 * https://w3c.github.io/ServiceWorker/#cache-objects (consulted 2026-10-28): Query Cache, Request
 * Matches Cached Item and Batch Cache Operations run here, under the origin's lease, on the lists
 * as the disk has them. */
#include <webcache.h>
#include <webstorage.h>
#include <weburl.h>
#include <nocturne.h>
#include <bearssl_hash.h>
#include <stdio.h>

#define CACHE_PATH 1024
#define NAMES_MAGIC 0x314e434eu /* "NCN1" */
#define LIST_MAGIC 0x314c434eu  /* "NCL1" */
#define FORMAT 1u
#define READ_LIMIT (1u << 20)

struct span { const uint8_t *p; size_t n; };
struct stage {
    struct stage *next;
    uint32_t token;
    n_handle h;
    uint64_t size;
    int error; /* a write that failed: the batch that names it fails so */
    char digest[65], id[33], path[CACHE_PATH];
};
struct body {
    struct body *next;
    uint32_t token;
    n_handle h;
    uint64_t at, size;
};
struct webcache {
    struct stage *stages;
    struct body *bodies;
    uint32_t serial;
};

static int failure(n_result r) {
    switch (-r) {
    case N_ERR_NO_SPACE: return WEB_CACHE_QUOTA;
    case N_ERR_NO_MEMORY: return WEB_CACHE_MEMORY;
    case N_ERR_NOT_READY: case N_ERR_BUSY: return WEB_CACHE_BUSY;
    default: return WEB_CACHE_IO;
    }
}

static uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(0x100000001b3); }
    return h;
}

static void hex(const uint8_t *bytes, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15]; }
    out[n * 2] = 0;
}
static bool hex_name(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}
static bool fresh_id(char out[33]) {
    uint8_t bytes[16];
    if (n_random(bytes, sizeof bytes) < 0) return false;
    hex(bytes, sizeof bytes, out);
    return true;
}

/* caches in the browser's profile folder; false when it cannot be found. */
static bool caches_folder(char *out, size_t size) {
    const char *profile = web_profile_folder(false);
    return profile && n_path_join(profile, "caches", out, size) >= 0;
}
/* An origin's folder is the SHA-256 of its length (8 bytes, big endian) and its serialization. */
static void origin_digest(const char *origin, char out[65]) {
    size_t n = strlen(origin);
    uint8_t length[8], digest[32];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)((uint64_t)n >> (56 - i * 8));
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, length, sizeof length);
    br_sha256_update(&h, origin, n);
    br_sha256_out(&h, digest);
    hex(digest, sizeof digest, out);
}
static bool join(const char *folder, const char *name, char *out) {
    return n_path_join(folder, name, out, CACHE_PATH) >= 0;
}
static bool ensure_folder(const char *path) {
    struct n_entry e;
    if (n_entry_info(path, &e) >= 0) return e.kind == N_KIND_FOLDER;
    return n_folder_create(path) >= 0 || (n_entry_info(path, &e) >= 0 && e.kind == N_KIND_FOLDER);
}

/* The whole document at path in malloc storage; -N_ERR_NOT_FOUND when there is none. */
static n_result read_all(const char *path, uint8_t **out, size_t *length) {
    *out = NULL;
    *length = 0;
    n_handle h = n_document_open(path, N_DOCUMENT_READ);
    if (h < 0) return h;
    n_result size = n_document_size(h);
    uint8_t *bytes = size >= 0 && (uint64_t)size < SIZE_MAX ? malloc((size_t)size + 1) : NULL;
    n_result r = size < 0 ? size : bytes ? 0 : -N_ERR_NO_MEMORY;
    size_t at = 0;
    while (r >= 0 && at < (size_t)size) {
        n_result got = n_document_read_at(h, at, bytes + at, (size_t)size - at);
        if (got < 0) r = got;
        else if (!got) break;
        else at += (size_t)got;
    }
    n_handle_close(h);
    if (r < 0) { free(bytes); return r; }
    *out = bytes;
    *length = at;
    return 0;
}

/* The origin's folder; with make, made with its origin note and bodies folder. */
static int origin_folder(const char *origin, bool make, char *out, char digest[65]) {
    char root[CACHE_PATH], name[65];
    origin_digest(origin, name);
    if (digest) memcpy(digest, name, 65);
    if (!caches_folder(root, sizeof root) || !join(root, name, out)) return WEB_CACHE_IO;
    if (!make) return WEB_CACHE_OK;
    char note[CACHE_PATH], bodies[CACHE_PATH];
    if (!ensure_folder(root) || !ensure_folder(out) || !join(out, "origin", note) || !join(out, "bodies", bodies) ||
        !ensure_folder(bodies)) return WEB_CACHE_IO;
    struct n_entry e;
    n_result found = n_entry_info(note, &e);
    if (found == -N_ERR_NOT_FOUND) found = n_document_save(note, origin, strlen(origin));
    return found < 0 ? failure(found) : WEB_CACHE_OK;
}

/* The origin's lease, the claim of its lock document; WEB_CACHE_BUSY when another holds it and
   wait is false. *h is -1 (and the result OK) when the folder is not there and make is false: the
   origin has nothing stored. A lock document a clear removed while this waited for it is not the
   lease: the folder is looked up again. */
static int lease_take(const char *origin, bool make, bool wait, char *folder, char digest[65], n_handle *h) {
    *h = -1;
    for (int attempt = 0; attempt < 8; attempt++) {
        int r = origin_folder(origin, make, folder, digest);
        if (r) return r;
        char path[CACHE_PATH];
        if (!join(folder, "lock", path)) return WEB_CACHE_IO;
        struct n_entry e;
        if (!make && n_entry_info(folder, &e) < 0) return WEB_CACHE_OK;
        n_handle lock = n_document_open(path, N_DOCUMENT_EDIT);
        if (lock == -N_ERR_NOT_FOUND) {
            lock = n_document_open(path, N_DOCUMENT_CREATE);
            if (lock == -N_ERR_EXISTS) lock = n_document_open(path, N_DOCUMENT_EDIT);
        }
        if (lock == -N_ERR_NOT_FOUND && !make) return WEB_CACHE_OK;
        if (lock < 0) return failure(lock);
        n_result claimed = n_document_claim(lock, wait);
        if (claimed < 0) { n_handle_close(lock); return failure(claimed); }
        struct n_entry held, named;
        if (n_document_info(lock, &held) >= 0 && n_entry_info(path, &named) >= 0 && held.id == named.id) {
            *h = lock;
            return WEB_CACHE_OK;
        }
        n_document_release(lock);
        n_handle_close(lock);
    }
    return WEB_CACHE_BUSY;
}
static void lease_drop(n_handle h) {
    if (h < 0) return;
    n_document_release(h);
    n_handle_close(h);
}

/* ---- The name to cache map ---- */
struct name_item { struct span name; char id[33]; };
struct names { uint8_t *data; struct name_item *items; size_t count, capacity; };

static void names_free(struct names *m) { free(m->data); free(m->items); memset(m, 0, sizeof *m); }
static bool names_add(struct names *m, struct span name, const char *id) {
    if (m->count == m->capacity) {
        size_t c = m->capacity ? m->capacity * 2 : 8;
        struct name_item *grown = realloc(m->items, c * sizeof *grown);
        if (!grown) return false;
        m->items = grown;
        m->capacity = c;
    }
    m->items[m->count].name = name;
    memcpy(m->items[m->count].id, id, 33);
    m->count++;
    return true;
}
/* A document that does not hold together is lost storage: WEB_CACHE_IO, never an empty map. */
static int names_load(const char *folder, struct names *m) {
    memset(m, 0, sizeof *m);
    char path[CACHE_PATH];
    if (!join(folder, "names", path)) return WEB_CACHE_IO;
    size_t n;
    n_result r = read_all(path, &m->data, &n);
    if (r == -N_ERR_NOT_FOUND) return WEB_CACHE_OK;
    if (r < 0) return failure(r);
    struct webcache_in tail = {m->data, n, n >= 8 ? n - 8 : 0, false};
    if (n < 8 || checksum(m->data, n - 8) != webcache_get_u64(&tail)) { names_free(m); return WEB_CACHE_IO; }
    struct webcache_in in = {m->data, n - 8, 0, false};
    const uint8_t *p;
    size_t length;
    uint32_t magic = webcache_get_u32(&in), format = webcache_get_u32(&in);
    webcache_get_field(&in, &p, &length); /* the origin, for the Settings window */
    uint32_t count = webcache_get_u32(&in);
    if (in.failed || magic != NAMES_MAGIC || format != FORMAT) { names_free(m); return WEB_CACHE_IO; }
    for (uint32_t i = 0; i < count; i++) {
        struct span name;
        const uint8_t *id;
        size_t id_length;
        webcache_get_field(&in, &name.p, &name.n);
        webcache_get_field(&in, &id, &id_length);
        char text[33];
        if (in.failed || id_length != 32) { names_free(m); return WEB_CACHE_IO; }
        memcpy(text, id, 32);
        text[32] = 0;
        if (!hex_name(text, 32)) { names_free(m); return WEB_CACHE_IO; }
        if (!names_add(m, name, text)) { names_free(m); return WEB_CACHE_MEMORY; }
    }
    if (in.at != in.length) { names_free(m); return WEB_CACHE_IO; }
    return WEB_CACHE_OK;
}
static int names_save(const char *folder, const char *origin, const struct names *m) {
    struct webcache_out o = {0};
    webcache_put_u32(&o, NAMES_MAGIC);
    webcache_put_u32(&o, FORMAT);
    webcache_put_field(&o, origin, strlen(origin));
    webcache_put_u32(&o, (uint32_t)m->count);
    for (size_t i = 0; i < m->count; i++) {
        webcache_put_field(&o, m->items[i].name.p, m->items[i].name.n);
        webcache_put_field(&o, m->items[i].id, 32);
    }
    if (!o.failed) webcache_put_u64(&o, checksum(o.bytes, o.length));
    char path[CACHE_PATH];
    int r = o.failed ? WEB_CACHE_MEMORY : !join(folder, "names", path) ? WEB_CACHE_IO : WEB_CACHE_OK;
    if (!r) { n_result saved = n_document_save(path, o.bytes, o.length); if (saved < 0) r = failure(saved); }
    free(o.bytes);
    return r;
}
static long names_find(const struct names *m, const uint8_t *name, size_t n) {
    for (size_t i = 0; i < m->count; i++)
        if (m->items[i].name.n == n && (!n || !memcmp(m->items[i].name.p, name, n))) return (long)i;
    return -1;
}

/* ---- A cache's request response list ---- */
struct entry {
    struct span url, method, request_headers, request_extra, response_headers, response_extra;
    char body[33];      /* "" when the response's body is null */
    uint64_t size;
    struct stage *stage; /* an entry this batch adds: its staged body */
};
struct list { uint8_t *data; struct entry *items; size_t count, capacity; };

static void list_free(struct list *l) { free(l->data); free(l->items); memset(l, 0, sizeof *l); }
static bool list_add(struct list *l, const struct entry *e) {
    if (l->count == l->capacity) {
        size_t c = l->capacity ? l->capacity * 2 : 16;
        struct entry *grown = realloc(l->items, c * sizeof *grown);
        if (!grown) return false;
        l->items = grown;
        l->capacity = c;
    }
    l->items[l->count++] = *e;
    return true;
}
static void list_remove(struct list *l, size_t index) {
    memmove(&l->items[index], &l->items[index + 1], (l->count - index - 1) * sizeof *l->items);
    l->count--;
}
/* A cache id names its list; one the client made up is only ever hex. */
static bool list_path(const char *folder, const uint8_t *id, size_t n, char *out) {
    char leaf[48];
    if (n != 32) return false;
    memcpy(leaf, id, 32);
    leaf[32] = 0;
    if (!hex_name(leaf, 32)) return false;
    memcpy(leaf + 32, ".cache", 7);
    return join(folder, leaf, out);
}
/* The headers fields from where the reader is (u32 count, then the names and values). */
static bool get_headers(struct webcache_in *in, struct span *out) {
    size_t start = in->at;
    uint32_t count = webcache_get_u32(in);
    for (uint32_t i = 0; i < count && !in->failed; i++) {
        const uint8_t *p;
        size_t n;
        webcache_get_field(in, &p, &n);
        webcache_get_field(in, &p, &n);
    }
    out->p = in->bytes + start;
    out->n = in->at - start;
    return !in->failed;
}
static int list_load(const char *folder, const uint8_t *id, size_t id_length, struct list *l) {
    memset(l, 0, sizeof *l);
    char path[CACHE_PATH];
    if (!list_path(folder, id, id_length, path)) return WEB_CACHE_TYPE;
    size_t n;
    n_result r = read_all(path, &l->data, &n);
    if (r == -N_ERR_NOT_FOUND) return WEB_CACHE_OK;
    if (r < 0) return failure(r);
    struct webcache_in tail = {l->data, n, n >= 8 ? n - 8 : 0, false};
    if (n < 8 || checksum(l->data, n - 8) != webcache_get_u64(&tail)) { list_free(l); return WEB_CACHE_IO; }
    struct webcache_in in = {l->data, n - 8, 0, false};
    uint32_t magic = webcache_get_u32(&in), format = webcache_get_u32(&in), count = webcache_get_u32(&in);
    if (in.failed || magic != LIST_MAGIC || format != FORMAT) { list_free(l); return WEB_CACHE_IO; }
    for (uint32_t i = 0; i < count; i++) {
        struct entry e = {0};
        const uint8_t *p;
        size_t k;
        webcache_get_field(&in, &e.url.p, &e.url.n);
        webcache_get_field(&in, &e.method.p, &e.method.n);
        webcache_get_field(&in, &e.request_headers.p, &e.request_headers.n);
        webcache_get_field(&in, &e.request_extra.p, &e.request_extra.n);
        webcache_get_field(&in, &e.response_headers.p, &e.response_headers.n);
        webcache_get_field(&in, &e.response_extra.p, &e.response_extra.n);
        webcache_get_field(&in, &p, &k);
        e.size = webcache_get_u64(&in);
        if (in.failed || (k && k != 32)) { list_free(l); return WEB_CACHE_IO; }
        memcpy(e.body, p, k);
        e.body[k] = 0;
        if (k && !hex_name(e.body, 32)) { list_free(l); return WEB_CACHE_IO; }
        if (!list_add(l, &e)) { list_free(l); return WEB_CACHE_MEMORY; }
    }
    if (in.at != in.length) { list_free(l); return WEB_CACHE_IO; }
    return WEB_CACHE_OK;
}
static int list_save(const char *folder, const uint8_t *id, size_t id_length, const struct list *l) {
    struct webcache_out o = {0};
    webcache_put_u32(&o, LIST_MAGIC);
    webcache_put_u32(&o, FORMAT);
    webcache_put_u32(&o, (uint32_t)l->count);
    for (size_t i = 0; i < l->count; i++) {
        const struct entry *e = &l->items[i];
        webcache_put_field(&o, e->url.p, e->url.n);
        webcache_put_field(&o, e->method.p, e->method.n);
        webcache_put_field(&o, e->request_headers.p, e->request_headers.n);
        webcache_put_field(&o, e->request_extra.p, e->request_extra.n);
        webcache_put_field(&o, e->response_headers.p, e->response_headers.n);
        webcache_put_field(&o, e->response_extra.p, e->response_extra.n);
        webcache_put_field(&o, e->body, strlen(e->body));
        webcache_put_u64(&o, e->size);
    }
    if (!o.failed) webcache_put_u64(&o, checksum(o.bytes, o.length));
    char path[CACHE_PATH];
    int r = o.failed ? WEB_CACHE_MEMORY : !list_path(folder, id, id_length, path) ? WEB_CACHE_TYPE : WEB_CACHE_OK;
    if (!r) { n_result saved = n_document_save(path, o.bytes, o.length); if (saved < 0) r = failure(saved); }
    free(o.bytes);
    return r;
}

/* ---- Request Matches Cached Item ---- */
struct query { struct span url, method, headers; };

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static bool same_name(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
/* Fetch's "get" of a header list: the values named name, in order, joined by ", "; false when the
   list has none. */
static bool header_get(struct span list, const uint8_t *name, size_t n, struct webcache_out *value) {
    struct webcache_in in = {list.p, list.n, 0, false};
    uint32_t count = webcache_get_u32(&in);
    bool found = false;
    for (uint32_t i = 0; i < count && !in.failed; i++) {
        const uint8_t *k, *v;
        size_t kn, vn;
        webcache_get_field(&in, &k, &kn);
        webcache_get_field(&in, &v, &vn);
        if (in.failed || !same_name(k, kn, name, n)) continue;
        if (found) webcache_put(value, ", ", 2);
        webcache_put(value, v, vn);
        found = true;
    }
    return found;
}
/* The URL up to its fragment, or with ignoreSearch up to its query: a serialization percent-encodes
   '?' and '#' everywhere before them. */
static size_t url_key(struct span url, bool ignore_search) {
    for (size_t i = 0; i < url.n; i++) if (url.p[i] == '#' || (ignore_search && url.p[i] == '?')) return i;
    return url.n;
}
static bool is_ows(uint8_t c) { return c == ' ' || c == '\t'; }
/* requestQuery against the cached request (url, method, headers) and its response's headers (none:
   the response is null). A Vary field name's values must be the same in both requests' lists. */
static bool request_matches(const struct query *q, struct span url, struct span method, struct span headers,
                            const struct span *response_headers, unsigned options, bool *failed) {
    if (!(options & WEB_CACHE_IGNORE_METHOD) && !(method.n == 3 && !memcmp(method.p, "GET", 3))) return false;
    bool ignore = options & WEB_CACHE_IGNORE_SEARCH;
    size_t a = url_key(q->url, ignore), b = url_key(url, ignore);
    if (a != b || memcmp(q->url.p, url.p, a)) return false;
    if (!response_headers || (options & WEB_CACHE_IGNORE_VARY)) return true;
    struct webcache_out vary = {0};
    if (!header_get(*response_headers, (const uint8_t *)"vary", 4, &vary)) { free(vary.bytes); return !vary.failed || !(*failed = true); }
    bool same = !vary.failed;
    for (size_t at = 0; same && at < vary.length;) {
        size_t end = at;
        while (end < vary.length && vary.bytes[end] != ',') end++;
        size_t s = at, e = end;
        while (s < e && is_ows(vary.bytes[s])) s++;
        while (e > s && is_ows(vary.bytes[e - 1])) e--;
        at = end + 1;
        if (s == e) continue;
        if (e - s == 1 && vary.bytes[s] == '*') { same = false; break; }
        struct webcache_out x = {0}, y = {0};
        bool hx = header_get(headers, vary.bytes + s, e - s, &x), hy = header_get(q->headers, vary.bytes + s, e - s, &y);
        if (x.failed || y.failed) *failed = true;
        same = hx == hy && x.length == y.length && (!x.length || !memcmp(x.bytes, y.bytes, x.length));
        free(x.bytes);
        free(y.bytes);
    }
    if (vary.failed) *failed = true;
    free(vary.bytes);
    return same;
}
static bool entry_matches(const struct query *q, const struct entry *e, unsigned options, bool *failed) {
    return request_matches(q, e->url, e->method, e->request_headers, &e->response_headers, options, failed);
}
static bool get_query(struct webcache_in *in, struct query *q) {
    webcache_get_field(in, &q->url.p, &q->url.n);
    webcache_get_field(in, &q->method.p, &q->method.n);
    return get_headers(in, &q->headers);
}

/* ---- Tokens of the broker's own ---- */
static uint32_t next_token(webcache *c) {
    for (;;) {
        if (!++c->serial) c->serial = 1;
        bool used = false;
        for (struct stage *s = c->stages; s && !used; s = s->next) used = s->token == c->serial;
        for (struct body *b = c->bodies; b && !used; b = b->next) used = b->token == c->serial;
        if (!used) return c->serial;
    }
}
static struct stage **stage_find(webcache *c, uint32_t token) {
    struct stage **link = &c->stages;
    while (*link && (*link)->token != token) link = &(*link)->next;
    return link;
}
static struct body **body_find(webcache *c, uint32_t token) {
    struct body **link = &c->bodies;
    while (*link && (*link)->token != token) link = &(*link)->next;
    return link;
}
/* A staged body goes: written, its document too. */
static void stage_discard(struct stage *s) {
    if (s->h >= 0) n_handle_close(s->h);
    (void)n_entry_remove(s->path);
    free(s);
}
static void stage_unlink(webcache *c, struct stage *s) {
    struct stage **link = &c->stages;
    while (*link && *link != s) link = &(*link)->next;
    if (*link) *link = s->next;
}
static int body_open(webcache *c, const char *folder, const struct entry *e, struct webcache_out *o) {
    if (!e->body[0]) { webcache_put_u32(o, 0); webcache_put_u64(o, 0); return WEB_CACHE_OK; }
    char bodies[CACHE_PATH], path[CACHE_PATH];
    if (!join(folder, "bodies", bodies) || !join(bodies, e->body, path)) return WEB_CACHE_IO;
    struct body *b = calloc(1, sizeof *b);
    if (!b) return WEB_CACHE_MEMORY;
    b->h = n_document_open(path, N_DOCUMENT_READ);
    if (b->h < 0) { int r = failure(b->h); free(b); return r == WEB_CACHE_BUSY ? WEB_CACHE_IO : r; }
    n_result size = n_document_size(b->h);
    b->size = size >= 0 ? (uint64_t)size : e->size;
    b->token = next_token(c);
    b->next = c->bodies;
    c->bodies = b;
    webcache_put_u32(o, b->token);
    webcache_put_u64(o, b->size);
    return WEB_CACHE_OK;
}
/* The bodies a failed command opened are closed again: the realm never learns their tokens. */
static void bodies_close_since(webcache *c, struct body *mark) {
    while (c->bodies && c->bodies != mark) {
        struct body *b = c->bodies;
        c->bodies = b->next;
        n_handle_close(b->h);
        free(b);
    }
}
static void put_response(struct webcache_out *o, const struct entry *e) {
    webcache_put(o, e->response_headers.p, e->response_headers.n);
    webcache_put_field(o, e->response_extra.p, e->response_extra.n);
}
static void put_request(struct webcache_out *o, const struct entry *e) {
    webcache_put_field(o, e->url.p, e->url.n);
    webcache_put_field(o, e->method.p, e->method.n);
    webcache_put(o, e->request_headers.p, e->request_headers.n);
    webcache_put_field(o, e->request_extra.p, e->request_extra.n);
}

/* ---- The commands ---- */
static int open_cache(const struct webcache_call *call, struct webcache_answer *out) {
    char folder[CACHE_PATH];
    n_handle lease;
    int r = lease_take(call->origin, true, true, folder, NULL, &lease);
    if (r) return r;
    struct names m;
    r = names_load(folder, &m);
    if (!r) {
        long i = names_find(&m, call->name, call->name_length);
        char id[33];
        if (i >= 0) memcpy(id, m.items[i].id, 33);
        else if (!fresh_id(id)) r = WEB_CACHE_IO;
        else if (!names_add(&m, (struct span){call->name, call->name_length}, id)) r = WEB_CACHE_MEMORY;
        else r = names_save(folder, call->origin, &m);
        if (!r && !(out->bytes = malloc(32))) r = WEB_CACHE_MEMORY;
        if (!r) { memcpy(out->bytes, id, 32); out->length = 32; }
        names_free(&m);
    }
    lease_drop(lease);
    return r;
}
static int name_command(const struct webcache_call *call, struct webcache_answer *out) {
    char folder[CACHE_PATH];
    n_handle lease;
    bool removing = call->operation == WEB_CACHE_DELETE;
    int r = lease_take(call->origin, false, true, folder, NULL, &lease);
    if (r || lease < 0) return r;
    struct names m;
    r = names_load(folder, &m);
    if (!r && call->operation == WEB_CACHE_NAMES) {
        struct webcache_out o = {0};
        for (size_t i = 0; i < m.count; i++) webcache_put_field(&o, m.items[i].name.p, m.items[i].name.n);
        if (o.failed) { free(o.bytes); r = WEB_CACHE_MEMORY; }
        else { out->bytes = o.bytes; out->length = o.length; out->count = m.count; }
    } else if (!r) {
        long i = names_find(&m, call->name, call->name_length);
        out->count = i >= 0;
        /* The cache's list stays for its Cache objects (webcache_purge). */
        if (i >= 0 && removing) {
            memmove(&m.items[i], &m.items[i + 1], (m.count - (size_t)i - 1) * sizeof *m.items);
            m.count--;
            r = names_save(folder, call->origin, &m);
        }
    }
    names_free(&m);
    lease_drop(lease);
    return r;
}
/* Query Cache (on one list): the entries the query matches, or all without one. */
static int query_list(webcache *c, const char *folder, const struct list *l, const struct query *q, unsigned options,
                      bool requests, size_t limit, struct webcache_out *o, uint64_t *count) {
    for (size_t i = 0; i < l->count && *count < limit; i++) {
        bool failed = false;
        if (q && !entry_matches(q, &l->items[i], options, &failed)) { if (failed) return WEB_CACHE_MEMORY; continue; }
        if (failed) return WEB_CACHE_MEMORY;
        if (requests) put_request(o, &l->items[i]);
        else {
            put_response(o, &l->items[i]);
            int r = body_open(c, folder, &l->items[i], o);
            if (r) return r;
        }
        ++*count;
    }
    return o->failed ? WEB_CACHE_MEMORY : WEB_CACHE_OK;
}
static int match_command(webcache *c, const struct webcache_call *call, struct webcache_answer *out) {
    unsigned options = (unsigned)call->argument;
    bool requests = call->operation == WEB_CACHE_KEYS, whole = !call->name_length;
    struct webcache_in in = {call->payload, call->length, 0, false};
    struct span name = {0};
    struct query q;
    if (whole && (options & WEB_CACHE_NAMED)) webcache_get_field(&in, &name.p, &name.n);
    if ((options & WEB_CACHE_QUERY) && !get_query(&in, &q)) return WEB_CACHE_TYPE;
    if (in.failed || in.at != in.length || (whole && requests)) return WEB_CACHE_TYPE;
    char folder[CACHE_PATH];
    n_handle lease;
    int r = lease_take(call->origin, false, true, folder, NULL, &lease);
    if (r || lease < 0) return r;
    struct webcache_out o = {0};
    uint64_t count = 0;
    size_t limit = options & WEB_CACHE_FIRST ? 1 : SIZE_MAX;
    struct body *mark = c->bodies;
    const struct query *query = options & WEB_CACHE_QUERY ? &q : NULL;
    if (!whole) {
        struct list l;
        r = list_load(folder, call->name, call->name_length, &l);
        if (!r) r = query_list(c, folder, &l, query, options, requests, limit, &o, &count);
        list_free(&l);
    } else {
        /* CacheStorage match: the named cache, or each cache in the map's order until one has it. */
        struct names m;
        r = names_load(folder, &m);
        for (size_t i = 0; !r && i < m.count && !count; i++) {
            if ((options & WEB_CACHE_NAMED) && (m.items[i].name.n != name.n || (name.n && memcmp(m.items[i].name.p, name.p, name.n)))) continue;
            struct list l;
            r = list_load(folder, (const uint8_t *)m.items[i].id, 32, &l);
            if (!r) r = query_list(c, folder, &l, query, options, false, 1, &o, &count);
            list_free(&l);
            if (options & WEB_CACHE_NAMED) break;
        }
        names_free(&m);
    }
    lease_drop(lease);
    if (r) { bodies_close_since(c, mark); free(o.bytes); return r; }
    out->bytes = o.bytes;
    out->length = o.length;
    out->count = count;
    return WEB_CACHE_OK;
}
static int stage_command(webcache *c, const struct webcache_call *call, struct webcache_answer *out) {
    char folder[CACHE_PATH], bodies[CACHE_PATH];
    struct stage *s = calloc(1, sizeof *s);
    if (!s) return WEB_CACHE_MEMORY;
    s->h = -1;
    int r = origin_folder(call->origin, true, folder, s->digest);
    if (!r && (!join(folder, "bodies", bodies) || !fresh_id(s->id) || !join(bodies, s->id, s->path))) r = WEB_CACHE_IO;
    if (!r) {
        s->h = n_document_open(s->path, N_DOCUMENT_CREATE);
        if (s->h < 0) r = failure(s->h);
    }
    if (r) { free(s); return r == WEB_CACHE_BUSY ? WEB_CACHE_IO : r; }
    s->token = next_token(c);
    s->next = c->stages;
    c->stages = s;
    out->token = s->token;
    return WEB_CACHE_OK;
}
static int write_command(webcache *c, const struct webcache_call *call) {
    struct stage *s = *stage_find(c, call->token);
    if (!s) return WEB_CACHE_TYPE;
    if (s->error) return s->error;
    for (size_t at = 0; at < call->length;) {
        n_result wrote = n_document_write_at(s->h, s->size, call->payload + at, call->length - at);
        if (wrote <= 0) { s->error = wrote < 0 ? failure(wrote) : WEB_CACHE_IO; return s->error; }
        at += (size_t)wrote;
        s->size += (uint64_t)wrote;
    }
    return WEB_CACHE_OK;
}

/* Batch Cache Operations. */
struct operation { uint32_t type, flags; struct query q; struct entry e; uint32_t token; };

static bool http_scheme(struct span url) {
    return (url.n >= 5 && !memcmp(url.p, "http:", 5)) || (url.n >= 6 && !memcmp(url.p, "https:", 6));
}
static int batch_command(webcache *c, const struct webcache_call *call, struct webcache_answer *out) {
    struct webcache_in in = {call->payload, call->length, 0, false};
    uint32_t count = webcache_get_u32(&in);
    if (in.failed || count > in.length / 8) return WEB_CACHE_TYPE;
    struct operation *ops = calloc(count ? count : 1, sizeof *ops);
    if (!ops) return WEB_CACHE_MEMORY;
    int r = WEB_CACHE_OK;
    /* Every staged body the operations name, found before anything can fail: whatever happens they
       are used up (stored, or removed). */
    for (uint32_t i = 0; i < count && !in.failed; i++) {
        struct operation *op = &ops[i];
        op->type = webcache_get_u32(&in);
        op->flags = webcache_get_u32(&in);
        get_query(&in, &op->q);
        webcache_get_field(&in, &op->e.request_extra.p, &op->e.request_extra.n);
        op->e.url = op->q.url;
        op->e.method = op->q.method;
        op->e.request_headers = op->q.headers;
        if (op->type == WEB_CACHE_PUT) {
            get_headers(&in, &op->e.response_headers);
            webcache_get_field(&in, &op->e.response_extra.p, &op->e.response_extra.n);
            op->token = webcache_get_u32(&in);
            (void)webcache_get_u64(&in);
        }
    }
    if (in.failed || in.at != in.length) r = WEB_CACHE_TYPE;
    for (uint32_t i = 0; i < count; i++) {
        if (!ops[i].token) continue;
        struct stage *s = *stage_find(c, ops[i].token);
        for (uint32_t k = 0; s && k < i; k++) if (ops[k].e.stage == s) s = NULL;
        if (!s) { if (!r) r = WEB_CACHE_TYPE; continue; }
        ops[i].e.stage = s;
    }
    char folder[CACHE_PATH], digest[65];
    n_handle lease = -1;
    if (!r) r = lease_take(call->origin, true, true, folder, digest, &lease);
    for (uint32_t i = 0; !r && i < count; i++)
        if (ops[i].e.stage && (strcmp(ops[i].e.stage->digest, digest) || ops[i].e.stage->error))
            r = strcmp(ops[i].e.stage->digest, digest) ? WEB_CACHE_TYPE : ops[i].e.stage->error;
    struct list l = {0}, added = {0};
    if (!r) r = list_load(folder, call->name, call->name_length, &l);
    /* The bodies of the entries this batch removes (those it added too, whose staged bodies go). */
    struct list removed = {0};
    uint64_t deleted = 0;
    for (uint32_t i = 0; !r && i < count; i++) {
        struct operation *op = &ops[i];
        bool failed = false;
        if (op->type != WEB_CACHE_PUT && op->type != WEB_CACHE_REMOVE) { r = WEB_CACHE_TYPE; break; }
        /* The same request twice in one batch: Query Cache over the items it added. Request
           Matches Cached Item looks at the cached response's Vary only, so a put is also matched
           the other way, with its own response's Vary against each request added before it. */
        for (size_t k = 0; !r && k < added.count; k++) {
            const struct entry *a = &added.items[k];
            struct query back = {a->url, a->method, a->request_headers};
            if (entry_matches(&op->q, a, op->flags, &failed) ||
                (op->type == WEB_CACHE_PUT && request_matches(&back, op->e.url, op->e.method, op->e.request_headers, &op->e.response_headers, op->flags, &failed)))
                r = WEB_CACHE_INVALID_STATE;
        }
        if (!r && failed) r = WEB_CACHE_MEMORY;
        if (r) break;
        if (op->type == WEB_CACHE_PUT) {
            if (!http_scheme(op->e.url) || !(op->e.method.n == 3 && !memcmp(op->e.method.p, "GET", 3)) ||
                (op->flags & WEB_CACHE_OPTIONS)) { r = WEB_CACHE_TYPE; break; }
        }
        unsigned options = op->type == WEB_CACHE_PUT ? 0 : op->flags;
        for (size_t k = 0; k < l.count;) {
            if (!entry_matches(&op->q, &l.items[k], options, &failed)) { k++; continue; }
            if (!list_add(&removed, &l.items[k])) { r = WEB_CACHE_MEMORY; break; }
            list_remove(&l, k);
            if (op->type == WEB_CACHE_REMOVE) deleted++;
        }
        if (!r && failed) r = WEB_CACHE_MEMORY;
        if (r || op->type != WEB_CACHE_PUT) continue;
        struct entry e = op->e;
        if (e.stage) { memcpy(e.body, e.stage->id, 33); e.size = e.stage->size; }
        if (!list_add(&l, &e) || !list_add(&added, &e)) r = WEB_CACHE_MEMORY;
    }
    /* The staged bodies are on the medium before the list that names them. */
    for (uint32_t i = 0; !r && i < count; i++) {
        struct stage *s = ops[i].e.stage;
        if (!s || s->h < 0) continue;
        n_result closed = n_handle_close(s->h);
        s->h = -1;
        struct n_entry e;
        if (closed < 0) r = failure(closed);
        else if (n_entry_info(s->path, &e) < 0) r = WEB_CACHE_IO; /* a clear removed it meanwhile */
    }
    if (!r) r = list_save(folder, call->name, call->name_length, &l);
    if (r == WEB_CACHE_BUSY) r = WEB_CACHE_IO;
    /* Nothing names a removed entry's body now: it goes (an open reader keeps reading it). An added
       body goes with a failed batch, or when a later operation of the batch removed its entry. */
    if (!r) {
        char bodies[CACHE_PATH], path[CACHE_PATH];
        for (size_t k = 0; join(folder, "bodies", bodies) && k < removed.count; k++)
            if (removed.items[k].body[0] && !removed.items[k].stage && join(bodies, removed.items[k].body, path))
                (void)n_entry_remove(path);
    }
    for (uint32_t i = 0; i < count; i++) {
        struct stage *s = ops[i].e.stage;
        if (!s) continue;
        bool kept = !r;
        for (size_t k = 0; kept && k < removed.count; k++) if (removed.items[k].stage == s) kept = false;
        stage_unlink(c, s);
        if (kept) { if (s->h >= 0) n_handle_close(s->h); free(s); }
        else stage_discard(s);
    }
    lease_drop(lease);
    list_free(&l);
    free(added.items);
    free(removed.items);
    free(ops);
    out->count = deleted;
    return r;
}
static int read_command(webcache *c, const struct webcache_call *call, struct webcache_answer *out) {
    struct body **link = body_find(c, call->token), *b = *link;
    if (!b) return WEB_CACHE_TYPE;
    size_t want = call->argument && call->argument < READ_LIMIT ? (size_t)call->argument : READ_LIMIT;
    if ((uint64_t)want > b->size - b->at) want = (size_t)(b->size - b->at);
    if (want && !(out->bytes = malloc(want))) return WEB_CACHE_MEMORY;
    size_t got = 0;
    while (got < want) {
        n_result n = n_document_read_at(b->h, b->at, out->bytes + got, want - got);
        if (n < 0) { free(out->bytes); out->bytes = NULL; return failure(n) == WEB_CACHE_MEMORY ? WEB_CACHE_MEMORY : WEB_CACHE_IO; }
        if (!n) break;
        got += (size_t)n;
        b->at += (uint64_t)n;
    }
    out->length = got;
    if (!got) {
        /* The end: the realm asks no more of it. */
        free(out->bytes);
        out->bytes = NULL;
        *link = b->next;
        n_handle_close(b->h);
        free(b);
    }
    return WEB_CACHE_OK;
}

webcache *webcache_create(void) { return calloc(1, sizeof(webcache)); }
void webcache_free(webcache *c) {
    if (!c) return;
    while (c->stages) { struct stage *s = c->stages; c->stages = s->next; stage_discard(s); }
    while (c->bodies) { struct body *b = c->bodies; c->bodies = b->next; n_handle_close(b->h); free(b); }
    free(c);
}
int webcache_run(webcache *c, const struct webcache_call *call, struct webcache_answer *out) {
    memset(out, 0, sizeof *out);
    if (!c || !call) return WEB_CACHE_TYPE;
    bool origin = call->operation != WEB_CACHE_WRITE && call->operation != WEB_CACHE_DISCARD &&
        call->operation != WEB_CACHE_READ && call->operation != WEB_CACHE_CLOSE;
    if (origin && !web_origin_http(call->origin))
        return WEB_CACHE_SECURITY;
    switch (call->operation) {
    case WEB_CACHE_OPEN: return open_cache(call, out);
    case WEB_CACHE_HAS: case WEB_CACHE_DELETE: case WEB_CACHE_NAMES: return name_command(call, out);
    case WEB_CACHE_MATCH: case WEB_CACHE_KEYS: return match_command(c, call, out);
    case WEB_CACHE_STAGE: return stage_command(c, call, out);
    case WEB_CACHE_WRITE: return write_command(c, call);
    case WEB_CACHE_DISCARD: {
        struct stage **link = stage_find(c, call->token), *s = *link;
        if (!s) return WEB_CACHE_TYPE;
        *link = s->next;
        stage_discard(s);
        return WEB_CACHE_OK;
    }
    case WEB_CACHE_BATCH: return batch_command(c, call, out);
    case WEB_CACHE_READ: return read_command(c, call, out);
    case WEB_CACHE_CLOSE: {
        struct body **link = body_find(c, call->token), *b = *link;
        if (!b) return WEB_CACHE_TYPE;
        *link = b->next;
        n_handle_close(b->h);
        free(b);
        return WEB_CACHE_OK;
    }
    default: return WEB_CACHE_TYPE;
    }
}

/* ---- The Settings window and the session ---- */
static uint64_t tree_bytes(const char *path) {
    struct n_entry *entries = NULL;
    n_result count = n_folder_entries(path, &entries);
    uint64_t total = 0;
    for (n_result i = 0; i < count; i++) {
        char child[CACHE_PATH];
        if (!join(path, entries[i].name, child)) continue;
        total += entries[i].kind == N_KIND_FOLDER ? tree_bytes(child) : entries[i].size;
    }
    free(entries);
    return total;
}
/* Every document in path and its folders, then path; the lock document (still held) is left. */
static int tree_remove(const char *path, bool top) {
    struct n_entry *entries = NULL;
    n_result count = n_folder_entries(path, &entries);
    if (count < 0) return count == -N_ERR_NOT_FOUND ? WEB_CACHE_OK : failure(count);
    int r = WEB_CACHE_OK;
    for (n_result i = 0; i < count && !r; i++) {
        char child[CACHE_PATH];
        if (top && !strcmp(entries[i].name, "lock")) continue;
        if (!join(path, entries[i].name, child)) { r = WEB_CACHE_IO; break; }
        if (entries[i].kind == N_KIND_FOLDER) r = tree_remove(child, false);
        else { n_result removed = n_entry_remove(child); if (removed < 0 && removed != -N_ERR_NOT_FOUND) r = failure(removed); }
    }
    free(entries);
    if (!r && !top) { n_result removed = n_entry_remove(path); if (removed < 0 && removed != -N_ERR_NOT_FOUND) r = failure(removed); }
    return r;
}
/* Each origin folder of caches, with the origin its note names ("" when it cannot be read). */
static int each_origin(int (*visit)(void *opaque, const char *folder, const char *origin), void *opaque) {
    char root[CACHE_PATH];
    if (!caches_folder(root, sizeof root)) return WEB_CACHE_IO;
    struct n_entry *entries = NULL;
    n_result count = n_folder_entries(root, &entries);
    if (count < 0) return count == -N_ERR_NOT_FOUND ? WEB_CACHE_OK : failure(count);
    int r = WEB_CACHE_OK;
    bool busy = false;
    for (n_result i = 0; i < count && !r; i++) {
        if (entries[i].kind != N_KIND_FOLDER || !hex_name(entries[i].name, 64)) continue;
        char folder[CACHE_PATH], note[CACHE_PATH];
        if (!join(root, entries[i].name, folder) || !join(folder, "origin", note)) continue;
        uint8_t *bytes = NULL;
        size_t n = 0;
        (void)read_all(note, &bytes, &n);
        char *origin = malloc(n + 1);
        if (!origin) { free(bytes); r = WEB_CACHE_MEMORY; break; }
        if (n) memcpy(origin, bytes, n);
        origin[n] = 0;
        free(bytes);
        if (strlen(origin) != n) origin[0] = 0;
        int v = visit(opaque, folder, origin);
        free(origin);
        if (v == WEB_CACHE_BUSY) busy = true;
        else r = v;
    }
    free(entries);
    return r ? r : busy ? WEB_CACHE_BUSY : WEB_CACHE_OK;
}
struct usage_visit { void (*each)(void *, const char *, uint64_t); void *opaque; };
static int usage_one(void *opaque, const char *folder, const char *origin) {
    struct usage_visit *u = opaque;
    u->each(u->opaque, origin, tree_bytes(folder));
    return WEB_CACHE_OK;
}
int webcache_usage(void (*each)(void *opaque, const char *origin, uint64_t bytes), void *opaque) {
    if (!each) return WEB_CACHE_OK;
    struct usage_visit u = {each, opaque};
    return each_origin(usage_one, &u);
}
/* The lease of a folder each_origin found, without waiting. */
static int folder_lease(const char *folder, n_handle *h) {
    char path[CACHE_PATH];
    *h = -1;
    if (!join(folder, "lock", path)) return WEB_CACHE_IO;
    n_handle lock = n_document_open(path, N_DOCUMENT_EDIT);
    if (lock == -N_ERR_NOT_FOUND) {
        lock = n_document_open(path, N_DOCUMENT_CREATE);
        if (lock == -N_ERR_EXISTS) lock = n_document_open(path, N_DOCUMENT_EDIT);
    }
    if (lock < 0) return failure(lock);
    n_result claimed = n_document_claim(lock, false);
    if (claimed < 0) { n_handle_close(lock); return failure(claimed); }
    *h = lock;
    return WEB_CACHE_OK;
}
/* The whole origin goes; its lock document last, once it is let go. */
static int remove_origin(const char *folder, n_handle lease) {
    int r = tree_remove(folder, true);
    char path[CACHE_PATH];
    lease_drop(lease);
    if (!r && join(folder, "lock", path)) {
        (void)n_entry_remove(path);
        n_result removed = n_entry_remove(folder);
        if (removed < 0 && removed != -N_ERR_NOT_FOUND && removed != -N_ERR_NOT_EMPTY) r = failure(removed);
    }
    return r;
}
struct clear_visit { bool (*match)(void *, const char *); void *opaque; };
static int clear_one(void *opaque, const char *folder, const char *origin) {
    struct clear_visit *v = opaque;
    if (v->match && !v->match(v->opaque, origin)) return WEB_CACHE_OK;
    n_handle lease;
    int r = folder_lease(folder, &lease);
    return r ? r : remove_origin(folder, lease);
}
int webcache_clear(bool (*match)(void *opaque, const char *origin), void *opaque) {
    struct clear_visit v = {match, opaque};
    return each_origin(clear_one, &v);
}
static int purge_one(void *opaque, const char *folder, const char *origin) {
    (void)opaque;
    (void)origin;
    n_handle lease;
    int r = folder_lease(folder, &lease);
    if (r) return r;
    struct names m;
    r = names_load(folder, &m);
    struct n_entry *entries = NULL;
    n_result count = r ? 0 : n_folder_entries(folder, &entries);
    if (count < 0) r = failure(count);
    /* The lists the map names are kept, and the bodies they name; the rest goes. */
    char bodies[CACHE_PATH], path[CACHE_PATH];
    struct list keep = {0};
    size_t kept_lists = 0;
    for (n_result i = 0; !r && i < count; i++) {
        const char *leaf = entries[i].name;
        size_t n = strlen(leaf);
        if (entries[i].kind != N_KIND_DOCUMENT || n != 38 || strcmp(leaf + 32, ".cache")) continue;
        bool named = false;
        for (size_t k = 0; k < m.count && !named; k++) named = !memcmp(m.items[k].id, leaf, 32);
        if (!join(folder, leaf, path)) continue;
        if (!named) { (void)n_entry_remove(path); continue; }
        struct list l;
        r = list_load(folder, (const uint8_t *)leaf, 32, &l);
        if (r) break;
        kept_lists++;
        for (size_t k = 0; !r && k < l.count; k++) {
            struct entry e = {0};
            memcpy(e.body, l.items[k].body, 33);
            if (e.body[0] && !list_add(&keep, &e)) r = WEB_CACHE_MEMORY;
        }
        list_free(&l);
    }
    free(entries);
    entries = NULL;
    count = r || !join(folder, "bodies", bodies) ? 0 : n_folder_entries(bodies, &entries);
    for (n_result i = 0; !r && i < count; i++) {
        bool named = false;
        for (size_t k = 0; k < keep.count && !named; k++) named = !strcmp(keep.items[k].body, entries[i].name);
        if (!named && join(bodies, entries[i].name, path)) (void)n_entry_remove(path);
    }
    free(entries);
    free(keep.items);
    if (!r && !m.count && !kept_lists) { names_free(&m); return remove_origin(folder, lease); }
    names_free(&m);
    lease_drop(lease);
    return r;
}
int webcache_purge(void) { return each_origin(purge_one, NULL); }
