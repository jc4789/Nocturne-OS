/* Actual webcache.c (Cache Storage's storage, webcache.h) and BearSSL's hash; only the native
 * document, folder, entropy and lease calls are adapted to isolated host files under
 * build/cache-native-<serial> (C:\Users\User\AppData\Browser there). Documents are opened with
 * delete sharing, so a removed document's open handles read on, as Nocturne's do (POSIX delete on
 * NTFS: the name goes at once). Two engines stand for the
 * brokers of two browser processes. A bounded proof (check_cache_storage_native.py): not the
 * broker's pipe, not a realm, not a guest browser.
 * Build with -idirafter user/include -idirafter common -Ithird_party/bearssl/inc
 * -Ithird_party/bearssl/src and BearSSL's sha2small.c, enc32be.c and dec32be.c. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include <nocturne.h>
#include <webstorage.h>
#include <webcache.h>
#include <bearssl_hash.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define PROFILE_FOLDER "C:\\Users\\User\\AppData\\Browser"

/* ---- Host files for the native calls ---- */
struct file_entry { char path[400]; int leased; uint64_t id; };
static struct file_entry files[2048];
static unsigned fixture_serial;
static int fail_write_space, fail_save_space; /* the next write or save finds the volume full */
/* Each document's identity: its path and how many times a document was made there. */
struct generation { char path[400]; uint64_t made; };
static struct generation generations[4096];
static size_t generation_count;
static uint64_t generation_of(const char *host, bool make) {
    for (size_t i = 0; i < generation_count; i++)
        if (!strcmp(generations[i].path, host)) { if (make) generations[i].made++; return generations[i].made; }
    if (generation_count == 4096) return 0;
    snprintf(generations[generation_count].path, 400, "%s", host);
    generations[generation_count].made = 1;
    return generations[generation_count++].made;
}
static uint64_t identity(const char *host, uint64_t made) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (const char *p = host; *p; p++) { h ^= (uint8_t)*p; h *= UINT64_C(0x100000001b3); }
    return h ^ made * UINT64_C(0x9e3779b97f4a7c15);
}
static void path(const char *guest, char host[400]) {
    snprintf(host, 400, "build/cache-native-%u", fixture_serial);
    size_t at = strlen(host);
    if (guest[0] && guest[1] == ':') guest += 2;
    for (const char *p = guest; *p && at < 399; p++) host[at++] = *p == '\\' ? '/' : *p;
    host[at] = 0;
}
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s\\%s", folder, name);
    return n < 0 || (size_t)n >= cap ? -N_ERR_TOO_LARGE : n;
}
static const char *leaf_of(const char *guest) { const char *name = guest; for (const char *p = guest; *p; p++) if (*p == '\\' || *p == '/') name = p + 1; return name; }
n_result n_entry_info(const char *guest, struct n_entry *out) {
    char host[400];
    path(guest, host);
    struct _stat64 st;
    if (_stat64(host, &st) < 0) return -N_ERR_NOT_FOUND;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", leaf_of(guest));
    out->kind = (st.st_mode & _S_IFDIR) ? N_KIND_FOLDER : N_KIND_DOCUMENT;
    out->size = (uint64_t)st.st_size;
    out->id = identity(host, generation_of(host, false));
    return 0;
}
n_result n_entry_remove(const char *guest) {
    char host[400];
    path(guest, host);
    struct _stat64 st;
    if (_stat64(host, &st) < 0) return -N_ERR_NOT_FOUND;
    if (st.st_mode & _S_IFDIR) return _rmdir(host) == 0 ? 0 : errno == ENOTEMPTY ? -N_ERR_NOT_EMPTY : -N_ERR_IO;
    return _unlink(host) == 0 ? 0 : -N_ERR_IO;
}
n_result n_folder_create(const char *guest) {
    char host[400];
    path(guest, host);
    if (_mkdir(host) == 0) return 0;
    return errno == EEXIST ? -N_ERR_EXISTS : errno == ENOENT ? -N_ERR_NOT_FOUND : -N_ERR_IO;
}
n_result n_folder_entries(const char *guest, struct n_entry **out) {
    char host[400], query[420];
    path(guest, host);
    *out = NULL;
    struct _stat64 st;
    if (_stat64(host, &st) < 0) return -N_ERR_NOT_FOUND;
    if (!(st.st_mode & _S_IFDIR)) return -N_ERR_NOT_FOLDER;
    snprintf(query, sizeof query, "%s/*", host);
    struct __finddata64_t info;
    intptr_t search = _findfirst64(query, &info);
    struct n_entry *list = NULL;
    size_t count = 0;
    if (search != -1) {
        do {
            if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) continue;
            struct n_entry *more = realloc(list, (count + 1) * sizeof *list);
            if (!more) { free(list); _findclose(search); return -N_ERR_NO_MEMORY; }
            list = more;
            memset(&list[count], 0, sizeof *list);
            snprintf(list[count].name, sizeof list[count].name, "%s", info.name);
            list[count].kind = (info.attrib & _A_SUBDIR) ? N_KIND_FOLDER : N_KIND_DOCUMENT;
            list[count].size = (uint64_t)info.size;
            count++;
        } while (_findnext64(search, &info) == 0);
        _findclose(search);
    }
    *out = list;
    return (n_result)count;
}
n_handle n_document_open(const char *guest, int access) {
    char host[400];
    path(guest, host);
    struct _stat64 st;
    int exists = _stat64(host, &st) == 0;
    if (exists && (st.st_mode & _S_IFDIR)) return -N_ERR_IS_FOLDER;
    if (access == N_DOCUMENT_CREATE && exists) return -N_ERR_EXISTS;
    if (access != N_DOCUMENT_CREATE && !exists) return -N_ERR_NOT_FOUND;
    if (access != N_DOCUMENT_READ && access != N_DOCUMENT_EDIT && access != N_DOCUMENT_CREATE) return -N_ERR_UNSUPPORTED;
    HANDLE file = CreateFileA(host, access == N_DOCUMENT_READ ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              access == N_DOCUMENT_CREATE ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD why = GetLastError();
        return why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND ? -N_ERR_NOT_FOUND : why == ERROR_FILE_EXISTS ? -N_ERR_EXISTS : -N_ERR_IO;
    }
    int fd = _open_osfhandle((intptr_t)file, (access == N_DOCUMENT_READ ? _O_RDONLY : _O_RDWR) | _O_BINARY);
    if (fd < 0) { CloseHandle(file); return -N_ERR_IO; }
    if (fd >= 2048) { _close(fd); return -N_ERR_NO_MEMORY; }
    snprintf(files[fd].path, sizeof files[fd].path, "%s", host);
    files[fd].leased = 0;
    files[fd].id = identity(host, generation_of(host, access == N_DOCUMENT_CREATE));
    return fd;
}
static bool open_handle(n_handle h) { return h >= 0 && h < 2048 && files[h].path[0]; }
n_result n_document_info(n_handle h, struct n_entry *out) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", leaf_of(files[h].path));
    out->kind = N_KIND_DOCUMENT;
    out->id = files[h].id;
    return 0;
}
n_result n_document_read_at(n_handle h, uint64_t at, void *p, size_t n) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    if (_lseeki64(h, (long long)at, SEEK_SET) < 0) return -N_ERR_IO;
    int r = _read(h, p, (unsigned)n);
    return r < 0 ? -N_ERR_IO : r;
}
n_result n_document_write_at(n_handle h, uint64_t at, const void *p, size_t n) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    if (fail_write_space) { fail_write_space--; return -N_ERR_NO_SPACE; }
    if (_lseeki64(h, (long long)at, SEEK_SET) < 0) return -N_ERR_IO;
    int r = _write(h, p, (unsigned)n);
    return r < 0 ? -N_ERR_IO : r;
}
n_result n_document_size(n_handle h) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    long long size = _filelengthi64(h);
    return size < 0 ? -N_ERR_IO : (n_result)size;
}
/* A claim belongs to one opening: another opening of the same document is not ready. The host
   has one process, so a claim that would wait answers so too. */
n_result n_document_claim(n_handle h, bool wait) {
    (void)wait;
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    if (files[h].leased) return 0;
    for (int i = 0; i < 2048; i++) if (i != h && files[i].leased && !strcmp(files[i].path, files[h].path)) return -N_ERR_NOT_READY;
    files[h].leased = 1;
    return 0;
}
n_result n_document_release(n_handle h) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    if (!files[h].leased) return -N_ERR_INVALID;
    files[h].leased = 0;
    return 0;
}
n_result n_handle_close(n_handle h) {
    if (!open_handle(h)) return -N_ERR_NO_HANDLE;
    files[h].leased = 0;
    files[h].path[0] = 0;
    return _close(h) == 0 ? 0 : -N_ERR_IO;
}
/* Whole and atomic, as the volume's save: a full volume leaves the document as it was. */
n_result n_document_save(const char *guest, const void *data, size_t n) {
    char host[400];
    path(guest, host);
    if (fail_save_space) { fail_save_space--; return -N_ERR_NO_SPACE; }
    struct _stat64 st;
    bool exists = _stat64(host, &st) == 0;
    if (exists && (st.st_mode & _S_IFDIR)) return -N_ERR_IS_FOLDER;
    int fd = _open(host, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return errno == ENOENT ? -N_ERR_NOT_FOUND : -N_ERR_IO;
    if (!exists) generation_of(host, true);
    int r = n ? _write(fd, data, (unsigned)n) : 0;
    if (_close(fd) != 0 || r < 0 || (size_t)r != n) return -N_ERR_IO;
    return 0;
}
static uint64_t entropy = UINT64_C(0x243f6a8885a308d3);
n_result n_random(void *buf, size_t n) {
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        entropy ^= entropy << 13; entropy ^= entropy >> 7; entropy ^= entropy << 17;
        p[i] = (uint8_t)(entropy >> 24);
    }
    return 0;
}
static void make_folders(const char *guest) {
    char partial[400];
    size_t length = strlen(guest);
    for (size_t at = 3; at <= length; at++)
        if (at == length || guest[at] == '\\') { memcpy(partial, guest, at); partial[at] = 0; n_folder_create(partial); }
}
const char *web_profile_folder(bool temporary) { (void)temporary; make_folders(PROFILE_FOLDER); return PROFILE_FOLDER; }

/* ---- The cases ---- */
static int checks, failures;
static void check(const char *label, bool yes) { checks++; if (!yes) { failures++; printf("FAIL %s\n", label); } }
static const char *const ORIGIN = "https://cache.test";

static int call(webcache *c, unsigned op, const char *origin, const void *name, size_t name_length, uint32_t token,
                uint64_t argument, const struct webcache_out *payload, struct webcache_answer *out) {
    struct webcache_call k = {op, origin, name, name_length, token, argument, payload ? payload->bytes : NULL, payload ? payload->length : 0};
    return webcache_run(c, &k, out);
}
static void field(struct webcache_out *o, const char *s) { webcache_put_field(o, s, strlen(s)); }
/* A header list: count, then name and value pairs (NULL ends them). */
static void header_list(struct webcache_out *o, const char *const *pairs) {
    uint32_t n = 0;
    while (pairs && pairs[n * 2]) n++;
    webcache_put_u32(o, n);
    for (uint32_t i = 0; i < n; i++) { field(o, pairs[i * 2]); field(o, pairs[i * 2 + 1]); }
}
static void query(struct webcache_out *o, const char *url, const char *method, const char *const *headers) {
    field(o, url); field(o, method); header_list(o, headers);
}
static void op_put(struct webcache_out *o, const char *url, const char *const *request_headers,
                   const char *const *response_headers, const char *extra, uint32_t token) {
    webcache_put_u32(o, WEB_CACHE_PUT); webcache_put_u32(o, 0);
    query(o, url, "GET", request_headers); field(o, "{\"m\":\"cors\"}");
    header_list(o, response_headers); field(o, extra); webcache_put_u32(o, token); webcache_put_u64(o, 0);
}
static void op_remove(struct webcache_out *o, const char *url, uint32_t flags, const char *const *request_headers) {
    webcache_put_u32(o, WEB_CACHE_REMOVE); webcache_put_u32(o, flags | WEB_CACHE_OPTIONS);
    query(o, url, "GET", request_headers); field(o, "{}");
}
static char *open_id(webcache *c, const char *name, char out[33]) {
    struct webcache_answer a;
    out[0] = 0;
    if (call(c, WEB_CACHE_OPEN, ORIGIN, name, strlen(name), 0, 0, NULL, &a) || a.length != 32) { free(a.bytes); return NULL; }
    memcpy(out, a.bytes, 32);
    out[32] = 0;
    free(a.bytes);
    return out;
}
/* A staged body of n bytes of value v, written in pieces of piece bytes. */
static uint32_t stage(webcache *c, size_t n, uint8_t v, size_t piece) {
    struct webcache_answer a;
    if (call(c, WEB_CACHE_STAGE, ORIGIN, NULL, 0, 0, 0, NULL, &a)) return 0;
    uint32_t token = a.token;
    uint8_t *bytes = malloc(n ? n : 1);
    memset(bytes, v, n);
    for (size_t at = 0; at < n; at += piece) {
        struct webcache_out o = {0};
        webcache_put(&o, bytes + at, n - at < piece ? n - at : piece);
        int r = call(c, WEB_CACHE_WRITE, NULL, NULL, 0, token, 0, &o, &a);
        free(o.bytes);
        if (r) { free(bytes); return 0; }
    }
    free(bytes);
    return token;
}
static int batch(webcache *c, const char *id, const struct webcache_out *ops, uint32_t count, uint64_t *deleted) {
    struct webcache_out o = {0};
    webcache_put_u32(&o, count);
    webcache_put(&o, ops->bytes, ops->length);
    struct webcache_answer a;
    int r = call(c, WEB_CACHE_BATCH, ORIGIN, id, 32, 0, 0, &o, &a);
    free(o.bytes);
    if (deleted) *deleted = a.count;
    return r;
}
static int put_one(webcache *c, const char *id, const char *url, const char *const *request_headers,
                   const char *const *response_headers, uint32_t token) {
    struct webcache_out ops = {0};
    op_put(&ops, url, request_headers, response_headers, "{\"t\":\"basic\",\"s\":200}", token);
    int r = batch(c, id, &ops, 1, NULL);
    free(ops.bytes);
    return r;
}
/* match (or keys) on a cache, or on the whole map when id is NULL: the count. */
struct found { uint64_t count; uint32_t tokens[16]; uint64_t sizes[16]; char urls[16][96]; char extra[16][64]; };
static int match(webcache *c, unsigned op, const char *id, unsigned flags, const char *cache_name, const char *url,
                 const char *const *headers, struct found *f) {
    struct webcache_out o = {0};
    if (cache_name) { flags |= WEB_CACHE_NAMED; field(&o, cache_name); }
    if (url) { flags |= WEB_CACHE_QUERY; query(&o, url, "GET", headers); }
    struct webcache_answer a;
    int r = call(c, op, ORIGIN, id, id ? 32 : 0, 0, flags, &o, &a);
    free(o.bytes);
    memset(f, 0, sizeof *f);
    if (r) return r;
    f->count = a.count;
    struct webcache_in in = {a.bytes, a.length, 0, false};
    for (uint64_t i = 0; i < a.count && i < 16; i++) {
        const uint8_t *p;
        size_t n;
        if (op == WEB_CACHE_KEYS) {
            webcache_get_field(&in, &p, &n);
            snprintf(f->urls[i], sizeof f->urls[i], "%.*s", (int)n, (const char *)p);
            webcache_get_field(&in, &p, &n); /* method */
            uint32_t h = webcache_get_u32(&in);
            for (uint32_t k = 0; k < h * 2; k++) webcache_get_field(&in, &p, &n);
            webcache_get_field(&in, &p, &n);
        } else {
            uint32_t h = webcache_get_u32(&in);
            for (uint32_t k = 0; k < h * 2; k++) webcache_get_field(&in, &p, &n);
            webcache_get_field(&in, &p, &n);
            snprintf(f->extra[i], sizeof f->extra[i], "%.*s", (int)n, (const char *)p);
            f->tokens[i] = webcache_get_u32(&in);
            f->sizes[i] = webcache_get_u64(&in);
        }
    }
    if (in.failed || in.at != in.length) r = WEB_CACHE_IO;
    free(a.bytes);
    return r;
}
/* A body read to its end in pieces of at most max: its length, every byte v, or -1. */
static long long read_body(webcache *c, uint32_t token, size_t max, uint8_t v, int *reads) {
    long long total = 0;
    *reads = 0;
    for (;;) {
        struct webcache_answer a;
        if (call(c, WEB_CACHE_READ, NULL, NULL, 0, token, max, NULL, &a)) return -1;
        ++*reads;
        if (!a.length) return total;
        if (a.length > max) { free(a.bytes); return -1; }
        for (size_t i = 0; i < a.length; i++) if (a.bytes[i] != v) { free(a.bytes); return -1; }
        total += (long long)a.length;
        free(a.bytes);
    }
}
static size_t count_in(const char *guest) {
    struct n_entry *entries = NULL;
    n_result n = n_folder_entries(guest, &entries);
    free(entries);
    return n < 0 ? 0 : (size_t)n;
}
static void origin_folder_of(const char *origin, char *out, size_t cap) {
    size_t n = strlen(origin);
    uint8_t length[8], digest[32];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)((uint64_t)n >> (56 - i * 8));
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, length, 8);
    br_sha256_update(&h, origin, n);
    br_sha256_out(&h, digest);
    char name[65];
    for (int i = 0; i < 32; i++) snprintf(name + i * 2, 3, "%02x", digest[i]);
    snprintf(out, cap, "%s\\caches\\%s", PROFILE_FOLDER, name);
}
struct seen { uint64_t bytes; size_t count; };
static void usage_each(void *opaque, const char *origin, uint64_t bytes) {
    struct seen *s = opaque;
    if (!strcmp(origin, ORIGIN)) s->bytes = bytes;
    s->count++;
}
static bool origin_is(void *opaque, const char *origin) { return !strcmp(origin, opaque); }

int main(void) {
    fixture_serial = (unsigned)_getpid();
    char dir[100];
    snprintf(dir, sizeof dir, "build/cache-native-%u", fixture_serial);
    _mkdir(dir);
    webcache *a = webcache_create(), *b = webcache_create();
    struct webcache_answer ans;
    struct found f;
    char v1[33], v2[33], again[33], folder[400], bodies[420];
    origin_folder_of(ORIGIN, folder, sizeof folder);
    snprintf(bodies, sizeof bodies, "%s\\bodies", folder);

    /* An origin that has no storage: no command reaches the disk. */
    check("opaque-origin-refused", call(a, WEB_CACHE_OPEN, "null", "x", 1, 0, 0, NULL, &ans) == WEB_CACHE_SECURITY);
    check("file-origin-refused", call(a, WEB_CACHE_NAMES, "file://", NULL, 0, 0, 0, NULL, &ans) == WEB_CACHE_SECURITY);
    /* Nothing stored yet: the map is empty, and asking makes no folder. */
    check("names-empty", call(a, WEB_CACHE_NAMES, ORIGIN, NULL, 0, 0, 0, NULL, &ans) == 0 && ans.count == 0);
    check("has-empty", call(a, WEB_CACHE_HAS, ORIGIN, "v1", 2, 0, 0, NULL, &ans) == 0 && ans.count == 0);
    check("delete-empty", call(a, WEB_CACHE_DELETE, ORIGIN, "v1", 2, 0, 0, NULL, &ans) == 0 && ans.count == 0);
    struct n_entry e;
    check("asking-makes-nothing", n_entry_info(folder, &e) == -N_ERR_NOT_FOUND);

    /* open: a name's cache, the same one again, in the map's order. */
    check("open-new", open_id(a, "v1", v1) != NULL);
    check("open-same", open_id(b, "v1", again) && !strcmp(again, v1));
    check("open-second", open_id(a, "v2", v2) && strcmp(v1, v2));
    const char lone[] = "\xed\xa0\x80name"; /* a lone surrogate, as QuickJS gives it */
    char odd[33];
    check("open-any-domstring", open_id(a, lone, odd) && open_id(b, lone, again) && !strcmp(odd, again));
    check("names-in-order", call(b, WEB_CACHE_NAMES, ORIGIN, NULL, 0, 0, 0, NULL, &ans) == 0 && ans.count == 3);
    {
        struct webcache_in in = {ans.bytes, ans.length, 0, false};
        const uint8_t *p;
        size_t n;
        bool ok = webcache_get_field(&in, &p, &n) && n == 2 && !memcmp(p, "v1", 2);
        ok = ok && webcache_get_field(&in, &p, &n) && n == 2 && !memcmp(p, "v2", 2);
        ok = ok && webcache_get_field(&in, &p, &n) && n == sizeof lone - 1 && !memcmp(p, lone, n);
        check("names-bytes", ok && in.at == in.length);
        free(ans.bytes);
    }
    check("origin-isolated", call(a, WEB_CACHE_NAMES, "https://other.test", NULL, 0, 0, 0, NULL, &ans) == 0 && ans.count == 0);

    /* put: a body streamed in pieces, stored, matched and read back in pieces. */
    const char *const plain[] = {"content-type", "text/plain", NULL};
    uint32_t t = stage(a, 300000, 0x61, 65536);
    check("stage-written", t != 0);
    check("put-stored", put_one(a, v1, "https://cache.test/a", NULL, plain, t) == 0);
    check("staged-token-used-up", call(a, WEB_CACHE_WRITE, NULL, NULL, 0, t, 0, NULL, &ans) == WEB_CACHE_TYPE);
    check("match-other-broker", match(b, WEB_CACHE_MATCH, v1, WEB_CACHE_FIRST, NULL, "https://cache.test/a", NULL, &f) == 0 &&
          f.count == 1 && f.sizes[0] == 300000 && f.tokens[0] && !strcmp(f.extra[0], "{\"t\":\"basic\",\"s\":200}"));
    int reads = 0;
    check("read-in-pieces", read_body(b, f.tokens[0], 100000, 0x61, &reads) == 300000 && reads == 4);
    check("read-closed-at-end", call(b, WEB_CACHE_READ, NULL, NULL, 0, f.tokens[0], 1, NULL, &ans) == WEB_CACHE_TYPE);
    check("match-ignores-fragment", match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/a#x", NULL, &f) == 0 && f.count == 1);
    check("close-body", f.count == 1 && call(a, WEB_CACHE_CLOSE, NULL, NULL, 0, f.tokens[0], 0, NULL, &ans) == 0 &&
          call(a, WEB_CACHE_READ, NULL, NULL, 0, f.tokens[0], 1, NULL, &ans) == WEB_CACHE_TYPE);
    check("match-other-cache-empty", match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/a", NULL, &f) == 0 && f.count == 0);
    check("match-null-body", put_one(a, v1, "https://cache.test/empty", NULL, NULL, 0) == 0 &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/empty", NULL, &f) == 0 && f.count == 1 && !f.tokens[0]);

    /* ignoreSearch, ignoreMethod and Vary. */
    check("put-search", put_one(a, v1, "https://cache.test/q?x=1", NULL, NULL, 0) == 0);
    check("search-counts", match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/q", NULL, &f) == 0 && f.count == 0);
    check("ignore-search", match(a, WEB_CACHE_MATCH, v1, WEB_CACHE_IGNORE_SEARCH, NULL, "https://cache.test/q?y", NULL, &f) == 0 && f.count == 1);
    check("ignore-method-same", match(a, WEB_CACHE_MATCH, v1, WEB_CACHE_IGNORE_METHOD, NULL, "https://cache.test/q?x=1", NULL, &f) == 0 && f.count == 1);
    const char *const circle[] = {"x-shape", "circle", NULL}, *const square[] = {"X-Shape", "square", NULL};
    const char *const circle_upper[] = {"X-SHAPE", "circle", NULL};
    const char *const vary_shape[] = {"Vary", "X-Shape", NULL}, *const vary_star[] = {"vary", "x-a, *", NULL};
    check("put-vary", put_one(a, v2, "https://cache.test/v", circle, vary_shape, 0) == 0);
    check("vary-same", match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/v", circle_upper, &f) == 0 && f.count == 1);
    check("vary-differs", match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/v", square, &f) == 0 && f.count == 0);
    check("vary-missing", match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/v", NULL, &f) == 0 && f.count == 0);
    check("ignore-vary", match(a, WEB_CACHE_MATCH, v2, WEB_CACHE_IGNORE_VARY, NULL, "https://cache.test/v", square, &f) == 0 && f.count == 1);
    /* A put with other Vary values keeps the first entry (Query Cache with no options). */
    check("put-vary-other", put_one(a, v2, "https://cache.test/v", square, vary_shape, 0) == 0 &&
          match(a, WEB_CACHE_MATCH, v2, WEB_CACHE_IGNORE_VARY, NULL, "https://cache.test/v", NULL, &f) == 0 && f.count == 2);
    check("put-vary-star", put_one(a, v2, "https://cache.test/star", NULL, vary_star, 0) == 0 &&
          match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/star", NULL, &f) == 0 && f.count == 0 &&
          match(a, WEB_CACHE_MATCH, v2, WEB_CACHE_IGNORE_VARY, NULL, "https://cache.test/star", NULL, &f) == 0 && f.count == 1);

    /* keys: the requests in the list's order; a put of the same request goes to the end. */
    check("keys-order", match(a, WEB_CACHE_KEYS, v1, 0, NULL, NULL, NULL, &f) == 0 && f.count == 3 &&
          !strcmp(f.urls[0], "https://cache.test/a") && !strcmp(f.urls[1], "https://cache.test/empty") && !strcmp(f.urls[2], "https://cache.test/q?x=1"));
    size_t before = count_in(bodies);
    t = stage(a, 10, 0x62, 4);
    check("put-replaces", t && put_one(a, v1, "https://cache.test/a", NULL, plain, t) == 0 &&
          match(a, WEB_CACHE_KEYS, v1, 0, NULL, NULL, NULL, &f) == 0 && f.count == 3 && !strcmp(f.urls[2], "https://cache.test/a"));
    check("replaced-body-removed", count_in(bodies) == before);
    check("replaced-body-read", match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/a", NULL, &f) == 0 && f.count == 1 &&
          read_body(a, f.tokens[0], 1 << 20, 0x62, &reads) == 10);
    check("keys-query", match(a, WEB_CACHE_KEYS, v1, WEB_CACHE_IGNORE_SEARCH, NULL, "https://cache.test/q", NULL, &f) == 0 &&
          f.count == 1 && !strcmp(f.urls[0], "https://cache.test/q?x=1"));
    check("keys-on-map-refused", match(a, WEB_CACHE_KEYS, NULL, 0, NULL, NULL, NULL, &f) == WEB_CACHE_TYPE);

    /* CacheStorage match: each cache in the map's order, or the named one. */
    check("map-match-first-cache", match(a, WEB_CACHE_MATCH, NULL, WEB_CACHE_FIRST, NULL, "https://cache.test/v", circle, &f) == 0 && f.count == 1);
    check("map-match-named", match(a, WEB_CACHE_MATCH, NULL, WEB_CACHE_FIRST, "v1", "https://cache.test/v", circle, &f) == 0 && f.count == 0 &&
          match(a, WEB_CACHE_MATCH, NULL, WEB_CACHE_FIRST, "v2", "https://cache.test/v", circle, &f) == 0 && f.count == 1);
    check("map-match-unknown-name", match(a, WEB_CACHE_MATCH, NULL, WEB_CACHE_FIRST, "none", "https://cache.test/a", NULL, &f) == 0 && f.count == 0);

    /* delete: what Query Cache finds with the options, how many. */
    struct webcache_out ops = {0};
    uint64_t deleted = 0;
    op_remove(&ops, "https://cache.test/q", WEB_CACHE_IGNORE_SEARCH, NULL);
    check("delete-ignore-search", batch(a, v1, &ops, 1, &deleted) == 0 && deleted == 1);
    free(ops.bytes); ops = (struct webcache_out){0};
    op_remove(&ops, "https://cache.test/q?x=1", 0, NULL);
    check("delete-none", batch(a, v1, &ops, 1, &deleted) == 0 && deleted == 0);
    free(ops.bytes); ops = (struct webcache_out){0};

    /* Batch Cache Operations: the same request twice is InvalidStateError, the list as it was,
       the staged bodies removed; Vary is looked at both ways. */
    before = count_in(bodies);
    uint32_t t1 = stage(a, 5, 1, 5), t2 = stage(a, 5, 2, 5);
    check("two-staged", t1 && t2 && count_in(bodies) == before + 2);
    op_put(&ops, "https://cache.test/d#one", NULL, NULL, "{}", t1);
    op_put(&ops, "https://cache.test/d#two", NULL, NULL, "{}", t2);
    check("duplicate-by-fragment", batch(a, v1, &ops, 2, NULL) == WEB_CACHE_INVALID_STATE);
    check("duplicate-left-nothing", count_in(bodies) == before &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/d", NULL, &f) == 0 && f.count == 0);
    check("duplicate-tokens-used-up", call(a, WEB_CACHE_DISCARD, NULL, NULL, 0, t1, 0, NULL, &ans) == WEB_CACHE_TYPE);
    free(ops.bytes); ops = (struct webcache_out){0};
    const char *const shape_big[] = {"x-shape", "circle", "x-size", "big", NULL}, *const square_big[] = {"x-shape", "square", "x-size", "big", NULL};
    const char *const vary_size[] = {"vary", "x-size", NULL};
    op_put(&ops, "https://cache.test/w", shape_big, vary_shape, "{}", 0);
    op_put(&ops, "https://cache.test/w", square_big, vary_size, "{}", 0);
    check("duplicate-by-later-vary", batch(a, v1, &ops, 2, NULL) == WEB_CACHE_INVALID_STATE);
    free(ops.bytes); ops = (struct webcache_out){0};
    op_put(&ops, "https://cache.test/w", square_big, vary_size, "{}", 0);
    op_put(&ops, "https://cache.test/w", shape_big, vary_shape, "{}", 0);
    check("duplicate-by-earlier-vary", batch(a, v1, &ops, 2, NULL) == WEB_CACHE_INVALID_STATE);
    free(ops.bytes); ops = (struct webcache_out){0};
    op_put(&ops, "https://cache.test/w", circle, vary_shape, "{}", 0);
    op_put(&ops, "https://cache.test/w", square, vary_shape, "{}", 0);
    check("differ-by-vary", batch(a, v1, &ops, 2, NULL) == 0 &&
          match(a, WEB_CACHE_MATCH, v1, WEB_CACHE_IGNORE_VARY, NULL, "https://cache.test/w", NULL, &f) == 0 && f.count == 2);
    free(ops.bytes); ops = (struct webcache_out){0};
    /* A put of what is no HTTP(S) GET request, and a token the broker does not know. */
    op_put(&ops, "blob:https://cache.test/x", NULL, NULL, "{}", 0);
    check("put-scheme-refused", batch(a, v1, &ops, 1, NULL) == WEB_CACHE_TYPE);
    free(ops.bytes); ops = (struct webcache_out){0};
    t1 = stage(a, 3, 3, 3);
    before = count_in(bodies);
    op_put(&ops, "https://cache.test/k", NULL, NULL, "{}", t1);
    op_put(&ops, "https://cache.test/l", NULL, NULL, "{}", 777777);
    check("unknown-token-refused", batch(a, v1, &ops, 2, NULL) == WEB_CACHE_TYPE && count_in(bodies) == before - 1);
    free(ops.bytes); ops = (struct webcache_out){0};
    check("stage-discard", (t1 = stage(a, 3, 3, 3)) && count_in(bodies) == before &&
          call(a, WEB_CACHE_DISCARD, NULL, NULL, 0, t1, 0, NULL, &ans) == 0 && count_in(bodies) == before - 1);
    check("token-of-another-broker", (t1 = stage(a, 3, 3, 3)) && put_one(b, v1, "https://cache.test/k", NULL, NULL, t1) == WEB_CACHE_TYPE &&
          call(a, WEB_CACHE_DISCARD, NULL, NULL, 0, t1, 0, NULL, &ans) == 0);
    check("malformed-batch", call(a, WEB_CACHE_BATCH, ORIGIN, v1, 32, 0, 0, &(struct webcache_out){(uint8_t *)"\x05\0\0\0", 4, 4, false}, &ans) == WEB_CACHE_TYPE);
    check("made-up-id-refused", match(a, WEB_CACHE_MATCH, "../../../../../../xxxxxxxxxxxxxxx", 0, NULL, NULL, NULL, &f) == WEB_CACHE_TYPE);

    /* A full volume: QuotaExceededError's status, and the cache as it was. */
    t1 = stage(a, 0, 0, 1);
    fail_write_space = 1;
    struct webcache_out piece = {0};
    webcache_put(&piece, "abc", 3);
    check("write-quota", t1 && call(a, WEB_CACHE_WRITE, NULL, NULL, 0, t1, 0, &piece, &ans) == WEB_CACHE_QUOTA);
    check("batch-of-failed-body-quota", put_one(a, v1, "https://cache.test/full", NULL, NULL, t1) == WEB_CACHE_QUOTA &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/full", NULL, &f) == 0 && f.count == 0);
    free(piece.bytes);
    fail_save_space = 1;
    check("list-save-quota", put_one(a, v1, "https://cache.test/full", NULL, NULL, 0) == WEB_CACHE_QUOTA &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/full", NULL, &f) == 0 && f.count == 0);
    fail_save_space = 1;
    check("open-quota", call(a, WEB_CACHE_OPEN, ORIGIN, "v9", 2, 0, 0, NULL, &ans) == WEB_CACHE_QUOTA &&
          call(a, WEB_CACHE_HAS, ORIGIN, "v9", 2, 0, 0, NULL, &ans) == 0 && ans.count == 0);

    /* The origin's lease: a command finds it held (the host has one process, so it does not wait). */
    char lock_path[420];
    snprintf(lock_path, sizeof lock_path, "%s\\lock", folder);
    n_handle held = n_document_open(lock_path, N_DOCUMENT_EDIT);
    check("lease-held", held >= 0 && n_document_claim(held, false) == 0);
    check("command-busy", call(b, WEB_CACHE_HAS, ORIGIN, "v1", 2, 0, 0, NULL, &ans) == WEB_CACHE_BUSY);
    check("clear-busy", webcache_clear(origin_is, (void *)ORIGIN) == WEB_CACHE_BUSY && n_entry_info(folder, &e) == 0);
    n_document_release(held);
    n_handle_close(held);

    /* A body a reader has open goes from the disk when its entry is replaced; the reader reads on. */
    t1 = stage(a, 20000, 0x63, 7000);
    check("put-read-on", t1 && put_one(a, v2, "https://cache.test/r", NULL, NULL, t1) == 0 &&
          match(a, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/r", NULL, &f) == 0 && f.count == 1);
    uint32_t reader = f.tokens[0];
    before = count_in(bodies);
    uint32_t t3 = stage(a, 5, 0x64, 5);
    check("replace-under-reader", t3 && put_one(a, v2, "https://cache.test/r", NULL, NULL, t3) == 0 && count_in(bodies) == before);
    check("reader-reads-on", read_body(a, reader, 4096, 0x63, &reads) == 20000 && reads == 6);
    check("new-body-read", match(b, WEB_CACHE_MATCH, v2, 0, NULL, "https://cache.test/r", NULL, &f) == 0 && f.count == 1 &&
          read_body(b, f.tokens[0], 4096, 0x64, &reads) == 5);

    /* CacheStorage delete: the name goes at once; the Cache object's list stays until a session
       begins with no window left (purge), with the bodies no list names. */
    char old[33];
    t1 = stage(a, 7, 7, 7);
    check("open-old", open_id(a, "old", old) && put_one(a, old, "https://cache.test/o", NULL, plain, t1) == 0);
    check("delete-name", call(b, WEB_CACHE_DELETE, ORIGIN, "old", 3, 0, 0, NULL, &ans) == 0 && ans.count == 1 &&
          call(a, WEB_CACHE_HAS, ORIGIN, "old", 3, 0, 0, NULL, &ans) == 0 && ans.count == 0);
    check("doomed-still-used", match(a, WEB_CACHE_MATCH, old, 0, NULL, "https://cache.test/o", NULL, &f) == 0 && f.count == 1 &&
          call(a, WEB_CACHE_CLOSE, NULL, NULL, 0, f.tokens[0], 0, NULL, &ans) == 0);
    check("doomed-not-in-map", match(a, WEB_CACHE_MATCH, NULL, 0, NULL, "https://cache.test/o", NULL, &f) == 0 && f.count == 0);
    char reopened[33];
    check("reopen-is-new", open_id(a, "old", reopened) && strcmp(reopened, old));
    char orphan[460];
    snprintf(orphan, sizeof orphan, "%s\\0123456789abcdef0123456789abcdef", bodies);
    check("orphan-made", n_document_save(orphan, "zz", 2) == 0);
    before = count_in(bodies);
    check("purge", webcache_purge() == 0);
    check("purge-doomed-list", match(a, WEB_CACHE_MATCH, old, 0, NULL, "https://cache.test/o", NULL, &f) == 0 && f.count == 0);
    check("purge-bodies", count_in(bodies) == before - 2 && n_entry_info(orphan, &e) == -N_ERR_NOT_FOUND);
    check("purge-keeps-named", match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/a", NULL, &f) == 0 && f.count == 1 &&
          call(a, WEB_CACHE_CLOSE, NULL, NULL, 0, f.tokens[0], 0, NULL, &ans) == 0);

    /* The Settings window: usage, then a clear of the origin whole. */
    struct seen s = {0};
    check("usage", webcache_usage(usage_each, &s) == 0 && s.count == 1 && s.bytes > 300);
    check("clear-other-origin-keeps", webcache_clear(origin_is, (void *)"https://other.test") == 0 && n_entry_info(folder, &e) == 0);
    check("clear", webcache_clear(origin_is, (void *)ORIGIN) == 0 && n_entry_info(folder, &e) == -N_ERR_NOT_FOUND);
    check("after-clear-empty", call(a, WEB_CACHE_NAMES, ORIGIN, NULL, 0, 0, 0, NULL, &ans) == 0 && ans.count == 0 &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/a", NULL, &f) == 0 && f.count == 0);
    s = (struct seen){0};
    check("usage-after-clear", webcache_usage(usage_each, &s) == 0 && s.count == 0);
    /* A stage a clear removed under it: its batch fails, nothing names a missing body. */
    t1 = stage(a, 4, 4, 4);
    check("stage-before-clear", t1 && open_id(a, "v1", v1) && webcache_clear(NULL, NULL) == 0);
    check("batch-after-clear", put_one(a, v1, "https://cache.test/a", NULL, NULL, t1) != 0 &&
          match(a, WEB_CACHE_MATCH, v1, 0, NULL, "https://cache.test/a", NULL, &f) == 0 && f.count == 0);

    /* A names document that does not hold together is lost storage, never an empty map. */
    check("open-after-clear", open_id(a, "v1", v1) != NULL);
    char names_path[420];
    snprintf(names_path, sizeof names_path, "%s\\names", folder);
    check("names-corrupt", n_document_save(names_path, "NCN1garbage", 11) == 0 &&
          call(a, WEB_CACHE_NAMES, ORIGIN, NULL, 0, 0, 0, NULL, &ans) == WEB_CACHE_IO);

    webcache_free(a);
    webcache_free(b);
    (void)webcache_clear(NULL, NULL);
    printf("cache-storage-native: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
