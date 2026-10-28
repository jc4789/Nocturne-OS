/* Cache Storage: the storage of CacheStorage and Cache (Service Workers, "Caches",
 * https://w3c.github.io/ServiceWorker/#cache-objects, consulted 2026-10-28).
 *
 * An origin's name to cache map and its caches' request response lists live in the browser's
 * profile folder, as the other stores' do (webidb.h, web/js_opfs.h):
 *   caches\<sha256 of the origin>\origin      the origin, for the Settings window's site data
 *   caches\<...>\names                         the name to cache map, in its order: name, cache id
 *   caches\<...>\<cache id>.cache              a cache's request response list, in its order
 *   caches\<...>\bodies\<body id>              a stored response's body, written once
 *   caches\<...>\lock                          the origin's lease: one command at a time
 * Every command reads what is on the disk under the lease and saves whole documents
 * (n_document_save), so the brokers of every window and worker of the origin see one state and a
 * failed batch leaves the cache as it was. A body is streamed into its document before the batch
 * that stores it and read out a piece at a time, never held whole. A deleted cache (CacheStorage
 * delete) leaves the map at once, but its list stays for the Cache objects that still represent
 * it; it goes, with the bodies nothing names, at the first window of the next session
 * (webcache_purge). Quota is the volume: a write the volume has no room for is WEB_CACHE_QUOTA.
 *
 * The engine runs in the storage broker (browserstorageworker, webidb_async.h); the realm side is
 * web/js_cachestorage.c and web/js_cache_storage.js. The usage, clear and purge calls serve the
 * Settings window and the session's end from any browser process (websitedata.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Results. The first five are the storage broker's own (webidb.h): a command it refused, or one
   whose broker died, has WEB_CACHE_SECURITY or WEB_CACHE_IO. */
enum {
    WEB_CACHE_OK, WEB_CACHE_BUSY, WEB_CACHE_SECURITY, WEB_CACHE_IO, WEB_CACHE_MEMORY,
    WEB_CACHE_QUOTA,         /* QuotaExceededError */
    WEB_CACHE_TYPE,          /* TypeError: Batch Cache Operations' type checks */
    WEB_CACHE_INVALID_STATE, /* InvalidStateError: a batch puts the same request twice */
};

/* The broker's commands (webidb_async.h carries them). Each names the origin the realm's owner
   gave, except those on a token of the broker's own. */
enum webcache_operation {
    WEB_CACHE_OPEN = 32, /* name: the cache name. bytes: the cache id */
    WEB_CACHE_HAS,       /* name: the cache name. count: 1 when the map has it */
    WEB_CACHE_DELETE,    /* name: the cache name. count: 1 when it was removed */
    WEB_CACHE_NAMES,     /* count, bytes: the names, in the map's order, each a field */
    /* name: the cache id, or none for the whole map (CacheStorage match). argument: the query
       flags (WEB_CACHE_IGNORE_*, WEB_CACHE_QUERY, WEB_CACHE_NAMED, WEB_CACHE_FIRST). payload:
       the cache name when WEB_CACHE_NAMED, then the query when WEB_CACHE_QUERY. count, bytes:
       the responses (webcache_response), each body opened for WEB_CACHE_READ */
    WEB_CACHE_MATCH,
    WEB_CACHE_KEYS,      /* as WEB_CACHE_MATCH on a cache: the requests (webcache_request) */
    WEB_CACHE_STAGE,     /* token: a new body, empty, to be written */
    WEB_CACHE_WRITE,     /* token: the body. payload: bytes appended to it */
    WEB_CACHE_DISCARD,   /* token: the body, removed */
    /* name: the cache id. payload: the operations (webcache_batch). count: the entries the
       delete operations removed. Every body the operations name is used up, stored or removed. */
    WEB_CACHE_BATCH,
    WEB_CACHE_READ,      /* token: an opened body. argument: at most this many bytes; none at the end */
    WEB_CACHE_CLOSE,     /* token: an opened body, no longer read */
    WEB_CACHE_LAST = WEB_CACHE_CLOSE
};

/* CacheQueryOptions, and what else a command's argument says. */
enum {
    WEB_CACHE_IGNORE_SEARCH = 1u << 0, WEB_CACHE_IGNORE_METHOD = 1u << 1, WEB_CACHE_IGNORE_VARY = 1u << 2,
    WEB_CACHE_OPTIONS = 1u << 3, /* a batch operation's options are not null */
    WEB_CACHE_QUERY = 1u << 4,   /* the command has a request query */
    WEB_CACHE_NAMED = 1u << 5,   /* MultiCacheQueryOptions' cacheName is present */
    WEB_CACHE_FIRST = 1u << 6,   /* match: the first response only */
};
enum { WEB_CACHE_PUT = 1, WEB_CACHE_REMOVE = 2 }; /* a batch operation's type */

/* The wire and the documents are fields: a u32 length (little endian) and that many bytes.
   A request query:   url, method, headers.
   A request:         url, method, headers, extra.
   A response:        headers, extra, then the body: u32 token (0: none) and u64 size.
   headers:           u32 count, then each header's name and value fields, in the list's order.
   extra:             what only the realm reads back (mode, status, type, ...), kept as given.
   A batch:           u32 count, then each operation: u32 type, u32 flags, the request, and for a
                      put the response, its body token the staged body's (WEB_CACHE_STAGE).
   The url is a URL serialization, the method and headers byte strings. */
struct webcache_out { uint8_t *bytes; size_t length, capacity; bool failed; };
struct webcache_in { const uint8_t *bytes; size_t length, at; bool failed; };

static inline void webcache_put(struct webcache_out *o, const void *p, size_t n) {
    if (o->failed || !n) return;
    if (n > SIZE_MAX - o->length) { o->failed = true; return; }
    if (o->length + n > o->capacity) {
        size_t c = o->capacity ? o->capacity : 256;
        while (c < o->length + n) { if (c > SIZE_MAX / 2) { c = o->length + n; break; } c *= 2; }
        uint8_t *grown = realloc(o->bytes, c);
        if (!grown) { o->failed = true; return; }
        o->bytes = grown;
        o->capacity = c;
    }
    memcpy(o->bytes + o->length, p, n);
    o->length += n;
}
static inline void webcache_put_u32(struct webcache_out *o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    webcache_put(o, b, 4);
}
static inline void webcache_put_u64(struct webcache_out *o, uint64_t v) {
    webcache_put_u32(o, (uint32_t)v);
    webcache_put_u32(o, (uint32_t)(v >> 32));
}
static inline void webcache_put_field(struct webcache_out *o, const void *p, size_t n) {
    if (n > UINT32_MAX) { o->failed = true; return; }
    webcache_put_u32(o, (uint32_t)n);
    webcache_put(o, p, n);
}
static inline uint32_t webcache_get_u32(struct webcache_in *i) {
    if (i->failed || i->length - i->at < 4) { i->failed = true; return 0; }
    const uint8_t *b = i->bytes + i->at;
    i->at += 4;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline uint64_t webcache_get_u64(struct webcache_in *i) {
    uint64_t low = webcache_get_u32(i);
    return low | (uint64_t)webcache_get_u32(i) << 32;
}
static inline bool webcache_get_field(struct webcache_in *i, const uint8_t **p, size_t *n) {
    uint32_t length = webcache_get_u32(i);
    if (i->failed || i->length - i->at < length) { i->failed = true; *p = NULL; *n = 0; return false; }
    *p = i->bytes + i->at;
    *n = length;
    i->at += length;
    return true;
}

typedef struct webcache webcache;
webcache *webcache_create(void);
/* Closes the bodies still open and removes those still staged. */
void webcache_free(webcache *c);
struct webcache_call {
    unsigned operation;      /* enum webcache_operation */
    const char *origin;      /* the serialized origin of the realm's storage key */
    const uint8_t *name;     /* the cache name or id, as the operation says */
    size_t name_length;
    uint32_t token;
    uint64_t argument;
    const uint8_t *payload;
    size_t length;
};
struct webcache_answer {
    uint32_t token;
    uint64_t count;
    uint8_t *bytes; /* malloc storage the caller frees */
    size_t length;
};
/* One command; a WEB_CACHE_* result. */
int webcache_run(webcache *c, const struct webcache_call *call, struct webcache_answer *out);

/* The Settings window's site data (websitedata.c), from any browser process: usage gives each
   origin's bytes (its maps, lists and bodies); clear removes every origin match accepts (NULL: all)
   whole, and leaves one a command holds now: WEB_CACHE_BUSY once the rest is removed. */
int webcache_usage(void (*each)(void *opaque, const char *origin, uint64_t bytes), void *opaque);
int webcache_clear(bool (*match)(void *opaque, const char *origin), void *opaque);
/* The first window of a session: no Cache object of an earlier one is left, so the caches the map
   no longer names go, with the bodies no list names (a broker that died while writing one). */
int webcache_purge(void);
