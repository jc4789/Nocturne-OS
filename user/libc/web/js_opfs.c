/* Origin private file system: the native half of js_opfs.js.
 * https://fs.spec.whatwg.org/ (2026-10-15) */
#include "js_opfs.h"
#include "http.h"
#include "nocturne.h"
#include <bearssl_hash.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPFS_ROOT "/data/browser/opfs"
#define OPFS_PATH 512 /* the kernel's path limit, including the terminator */
#define OPFS_NAME 255 /* a FAT long name */

/* Results shared with js_opfs.js, which maps them to DOMException names. */
enum {
    OPFS_NOT_FOUND = -1, OPFS_TYPE_MISMATCH = -2, OPFS_NOT_EMPTY = -3, OPFS_LOCKED = -4,
    OPFS_SECURITY = -5, OPFS_STATE = -6, OPFS_QUOTA = -7, OPFS_BAD_NAME = -8, OPFS_IO = -9,
    OPFS_MEMORY = -10,
};

struct opfs_access {
    struct opfs_access *next;
    JSContext *ctx;
    n_handle document; /* holds the file's exclusive claim */
};
static struct opfs_access *accesses;
static const char *(*context_url)(JSContext *ctx);

static int opfs_error(n_result error) {
    switch (-error) {
    case N_ERR_NOT_FOUND: return OPFS_NOT_FOUND;
    case N_ERR_NOT_FOLDER: case N_ERR_IS_FOLDER: case N_ERR_WRONG_KIND: return OPFS_TYPE_MISMATCH;
    case N_ERR_NOT_EMPTY: return OPFS_NOT_EMPTY;
    case N_ERR_NOT_READY: case N_ERR_BUSY: return OPFS_LOCKED;
    case N_ERR_NO_SPACE: return OPFS_QUOTA;
    case N_ERR_NO_MEMORY: return OPFS_MEMORY;
    default: return OPFS_IO;
    }
}

static bool ensure_directory(const char *path) {
    struct n_entry e;
    if (n_entry_info(path, &e) >= 0) return e.kind == N_KIND_FOLDER;
    return n_folder_create(path) >= 0 || (n_entry_info(path, &e) >= 0 && e.kind == N_KIND_FOLDER);
}

/* The origin's directory. Storage keys are tuple origins of HTTP(S) URLs;
 * an opaque origin (data:, sandboxed, about:blank without a creator) has
 * none. A length prefix keeps the digest input unambiguous. */
static int opfs_root(JSContext *ctx, char *out, size_t size) {
    const char *url = context_url ? context_url(ctx) : NULL;
    char *origin = NULL;
    enum http_url_result r = url ? http_origin_owned(url, &origin) : HTTP_URL_INVALID;
    if (r == HTTP_URL_OOM) return OPFS_MEMORY;
    if (r != HTTP_URL_TUPLE || (strncmp(origin, "http://", 7) && strncmp(origin, "https://", 8))) {
        free(origin);
        return OPFS_SECURITY;
    }
    size_t n = strlen(origin);
    uint8_t length[8], digest[32];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)((uint64_t)n >> (56 - i * 8));
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, length, sizeof length);
    br_sha256_update(&h, origin, n);
    br_sha256_out(&h, digest);
    free(origin);
    int used = snprintf(out, size, OPFS_ROOT "/");
    for (int i = 0; i < 32; i++) used += snprintf(out + used, size - (size_t)used, "%02x", digest[i]);
    if (!ensure_directory("/data/browser") || !ensure_directory(OPFS_ROOT) || !ensure_directory(out))
        return OPFS_IO;
    return 0;
}

/* Web names are any non-empty USVString except "." and ".." without '/'
 * or '\\'. FAT folds case and strips trailing dots, so only [a-z0-9_.-]
 * keep their spelling: an uppercase letter becomes '~' and its lowercase,
 * every other byte (and a final '.') becomes %xx of its UTF-8 bytes. */
static int encode_name(const char *name, size_t n, char *out, size_t *used, size_t size) {
    if (!n || (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) return OPFS_BAD_NAME;
    size_t start = *used, at = start;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\') return OPFS_BAD_NAME;
        char piece[4];
        size_t k;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (c == '.' && i + 1 < n)) {
            piece[0] = (char)c;
            k = 1;
        } else if (c >= 'A' && c <= 'Z') {
            piece[0] = '~';
            piece[1] = (char)(c - 'A' + 'a');
            k = 2;
        } else {
            static const char hex[] = "0123456789abcdef";
            piece[0] = '%';
            piece[1] = hex[c >> 4];
            piece[2] = hex[c & 15];
            k = 3;
        }
        if (at - start + k > OPFS_NAME || at + k >= size) return OPFS_BAD_NAME;
        memcpy(out + at, piece, k);
        at += k;
    }
    out[at] = 0;
    *used = at;
    return 0;
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Inverse of encode_name; false for a spelling encode_name never makes. */
static bool decode_name(const char *in, char *out, size_t *n) {
    size_t at = 0;
    for (size_t i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '~') {
            if (in[i + 1] < 'a' || in[i + 1] > 'z') return false;
            out[at++] = (char)(in[++i] - 'a' + 'A');
        } else if (c == '%') {
            int hi = hex_digit(in[i + 1]), lo = hi < 0 ? -1 : hex_digit(in[i + 2]);
            if (lo < 0) return false;
            out[at++] = (char)(hi << 4 | lo);
            i += 2;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[at++] = c;
        } else {
            return false;
        }
    }
    if (!at || (at == 1 && out[0] == '.') || (at == 2 && out[0] == '.' && out[1] == '.')) return false;
    out[at] = 0;
    *n = at;
    return true;
}

/* root + "/" + each encoded name of the JS array parts. */
static int opfs_path(JSContext *ctx, JSValueConst parts, char *out, size_t size) {
    int r = opfs_root(ctx, out, size);
    if (r) return r;
    uint32_t count = 0;
    JSValue length = JS_GetPropertyStr(ctx, parts, "length");
    if (JS_IsException(length) || JS_ToUint32(ctx, &count, length)) { JS_FreeValue(ctx, length); return OPFS_STATE; }
    JS_FreeValue(ctx, length);
    size_t used = strlen(out);
    for (uint32_t i = 0; i < count; i++) {
        JSValue part = JS_GetPropertyUint32(ctx, parts, i);
        size_t n = 0;
        const char *name = JS_IsString(part) ? JS_ToCStringLen(ctx, &n, part) : NULL;
        JS_FreeValue(ctx, part);
        if (!name) return OPFS_STATE;
        if (used + 1 >= size) r = OPFS_BAD_NAME;
        else {
            out[used++] = '/';
            r = encode_name(name, n, out, &used, size);
        }
        JS_FreeCString(ctx, name);
        if (r) return r;
    }
    return 0;
}

static int kind_of(const char *path) {
    struct n_entry e;
    n_result r = n_entry_info(path, &e);
    if (r < 0) return r == -N_ERR_NOT_FOUND ? 0 : opfs_error(r);
    return e.kind == N_KIND_FOLDER ? 2 : e.kind == N_KIND_DOCUMENT ? 1 : OPFS_TYPE_MISMATCH;
}

/* Entry names of a directory, encoded, one after another with terminators. */
static int list_raw(const char *path, char **names, size_t *bytes) {
    *names = NULL;
    *bytes = 0;
    struct n_entry *entries = NULL;
    n_result count = n_folder_entries(path, &entries);
    if (count < 0) return opfs_error(count);
    size_t capacity = 0;
    int r = 0;
    for (n_result index = 0; index < count; index++) {
        const struct n_entry *d = &entries[index];
        size_t n = strlen(d->name) + 1;
        if (*bytes + n > capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            while (next < *bytes + n) next *= 2;
            char *grown = realloc(*names, next);
            if (!grown) { r = OPFS_MEMORY; break; }
            *names = grown;
            capacity = next;
        }
        memcpy(*names + *bytes, d->name, n);
        *bytes += n;
    }
    free(entries);
    if (r) { free(*names); *names = NULL; *bytes = 0; }
    return r;
}

/* Exclusive-claim probe: a sync access handle of any app holds it. */
static int file_free(const char *path) {
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h < 0) return opfs_error(h);
    n_result claimed = n_document_claim(h, false);
    if (claimed >= 0) n_document_release(h);
    n_handle_close(h);
    return claimed < 0 ? opfs_error(claimed) : 0;
}

/* Remove an entry; 0 or an OPFS result. */
static int remove_entry(const char *path) {
    n_result r = n_entry_remove(path);
    return r < 0 ? opfs_error(r) : 0;
}

static int remove_tree(char *path, size_t size, bool recursive) {
    int kind = kind_of(path);
    if (kind < 0) return kind;
    if (!kind) return OPFS_NOT_FOUND;
    if (kind == 1) {
        int r = file_free(path);
        if (r) return r;
        return remove_entry(path);
    }
    char *names;
    size_t bytes;
    int r = list_raw(path, &names, &bytes);
    if (r) return r;
    if (bytes && !recursive) { free(names); return OPFS_NOT_EMPTY; }
    size_t base = strlen(path);
    for (size_t at = 0; at < bytes && !r; at += strlen(names + at) + 1) {
        size_t n = strlen(names + at);
        if (base + 1 + n >= size) { r = OPFS_IO; break; }
        path[base] = '/';
        memcpy(path + base + 1, names + at, n + 1);
        r = remove_tree(path, size, true);
        path[base] = 0;
    }
    free(names);
    if (r) return r;
    return remove_entry(path);
}

/* Move a file or directory tree, one document at a time: a folder move
 * across volumes is not offered by n_entry_move, which never replaces. */
static int move_tree(char *from, char *to, size_t size) {
    int kind = kind_of(from);
    if (kind < 0) return kind;
    if (!kind) return OPFS_NOT_FOUND;
    if (kind == 1) {
        int r = file_free(from);
        if (r) return r;
        int existing = kind_of(to);
        if (existing < 0) return existing;
        if (existing == 2) return OPFS_TYPE_MISMATCH;
        if (existing == 1 && ((r = file_free(to)) || (r = remove_entry(to)))) return r;
        n_result moved = n_entry_move(from, to);
        return moved < 0 ? opfs_error(moved) : 0;
    }
    int existing = kind_of(to);
    if (existing < 0) return existing;
    if (existing == 1) return OPFS_TYPE_MISMATCH;
    if (!existing) { n_result made = n_folder_create(to); if (made < 0) return opfs_error(made); }
    char *names;
    size_t bytes;
    int r = list_raw(from, &names, &bytes);
    if (r) return r;
    size_t a = strlen(from), b = strlen(to);
    for (size_t at = 0; at < bytes && !r; at += strlen(names + at) + 1) {
        size_t n = strlen(names + at);
        if (a + 1 + n >= size || b + 1 + n >= size) { r = OPFS_IO; break; }
        from[a] = '/';
        memcpy(from + a + 1, names + at, n + 1);
        to[b] = '/';
        memcpy(to + b + 1, names + at, n + 1);
        r = move_tree(from, to, size);
        from[a] = 0;
        to[b] = 0;
    }
    free(names);
    if (r) return r;
    return remove_entry(from);
}

static int64_t tree_usage(char *path, size_t size) {
    struct n_entry e;
    n_result found = n_entry_info(path, &e);
    if (found < 0) return opfs_error(found);
    if (e.kind != N_KIND_FOLDER) return (int64_t)e.size;
    char *names;
    size_t bytes;
    int r = list_raw(path, &names, &bytes);
    if (r) return r;
    int64_t total = 0;
    size_t base = strlen(path);
    for (size_t at = 0; at < bytes; at += strlen(names + at) + 1) {
        size_t n = strlen(names + at);
        if (base + 1 + n >= size) continue;
        path[base] = '/';
        memcpy(path + base + 1, names + at, n + 1);
        int64_t child = tree_usage(path, size);
        path[base] = 0;
        if (child < 0) { total = child; break; }
        total += child;
    }
    free(names);
    return total;
}

static int full_io(n_handle h, uint64_t at, void *data, size_t n, bool write_mode, size_t *done) {
    *done = 0;
    while (*done < n) {
        n_result r = write_mode ? n_document_write_at(h, at + *done, (const uint8_t *)data + *done, n - *done) :
                                  n_document_read_at(h, at + *done, (uint8_t *)data + *done, n - *done);
        if (r < 0) return opfs_error(r);
        if (!r) break;
        *done += (size_t)r;
    }
    return 0;
}

/* A byte range of an ArrayBuffer or SharedArrayBuffer, checked by bounds. */
static uint8_t *buffer_range(JSContext *ctx, JSValueConst buffer, JSValueConst offset, JSValueConst length, size_t *n) {
    size_t total = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &total, buffer);
    if (!data) return NULL;
    int64_t o = 0, l = 0;
    if (JS_ToInt64(ctx, &o, offset) || JS_ToInt64(ctx, &l, length)) return NULL;
    if (o < 0 || l < 0 || (uint64_t)o > total || (uint64_t)l > total - (uint64_t)o) {
        JS_ThrowRangeError(ctx, "Buffer range is out of bounds");
        return NULL;
    }
    *n = (size_t)l;
    return data + o;
}

static JSValue opfs_result(JSContext *ctx, int64_t r) {
    if (r == OPFS_MEMORY) return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt64(ctx, r);
}

/* host.opfsKind(parts) -> 0 missing, 1 file, 2 directory, or an error. */
static JSValue opfs_kind(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    return opfs_result(ctx, r ? r : kind_of(path));
}

/* host.opfsEnsure(parts, kind, create): the get*Handle lookup algorithm. */
static JSValue opfs_ensure(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    if (r) return opfs_result(ctx, r);
    int32_t want = 0;
    if (JS_ToInt32(ctx, &want, argv[1])) return JS_EXCEPTION;
    int kind = kind_of(path);
    if (kind < 0) return opfs_result(ctx, kind);
    if (kind) return opfs_result(ctx, kind == want ? 0 : OPFS_TYPE_MISMATCH);
    if (!JS_ToBool(ctx, argv[2])) return opfs_result(ctx, OPFS_NOT_FOUND);
    n_result made = want == 2 ? n_folder_create(path) : n_document_open(path, N_DOCUMENT_CREATE);
    if (want != 2 && made >= 0) n_handle_close((n_handle)made);
    r = made < 0 ? opfs_error(made) : 0;
    return opfs_result(ctx, r);
}

/* host.opfsList(parts) -> [[name, kind], ...] in directory order. */
static JSValue opfs_list(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    if (r) return opfs_result(ctx, r);
    if ((r = kind_of(path)) != 2) return opfs_result(ctx, r < 0 ? r : r ? OPFS_TYPE_MISMATCH : OPFS_NOT_FOUND);
    char *names;
    size_t bytes;
    if ((r = list_raw(path, &names, &bytes))) return opfs_result(ctx, r);
    JSValue list = JS_NewArray(ctx);
    uint32_t count = 0;
    size_t base = strlen(path);
    for (size_t at = 0; at < bytes && !JS_IsException(list); at += strlen(names + at) + 1) {
        char decoded[OPFS_NAME + 1];
        size_t n;
        size_t raw = strlen(names + at);
        if (raw > OPFS_NAME || !decode_name(names + at, decoded, &n) || base + 1 + raw >= sizeof path) continue;
        path[base] = '/';
        memcpy(path + base + 1, names + at, raw + 1);
        int kind = kind_of(path);
        path[base] = 0;
        if (kind != 1 && kind != 2) continue;
        JSValue entry = JS_NewArray(ctx);
        if (JS_IsException(entry) ||
            JS_SetPropertyUint32(ctx, entry, 0, JS_NewStringLen(ctx, decoded, n)) < 0 ||
            JS_SetPropertyUint32(ctx, entry, 1, JS_NewInt32(ctx, kind)) < 0 ||
            JS_SetPropertyUint32(ctx, list, count++, entry) < 0) {
            JS_FreeValue(ctx, list);
            list = JS_EXCEPTION;
        }
    }
    free(names);
    return list;
}

/* host.opfsRemove(parts, recursive) */
static JSValue opfs_remove(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    return opfs_result(ctx, r ? r : remove_tree(path, sizeof path, JS_ToBool(ctx, argv[1])));
}

/* host.opfsRead(parts) -> [ArrayBuffer, lastModified ms]: a File snapshot. */
static JSValue opfs_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    if (r) return opfs_result(ctx, r);
    struct n_entry st;
    n_result found = n_entry_info(path, &st);
    if (found < 0) return opfs_result(ctx, opfs_error(found));
    if (st.kind != N_KIND_DOCUMENT) return opfs_result(ctx, OPFS_TYPE_MISMATCH);
    n_handle h = n_document_open(path, N_DOCUMENT_READ);
    if (h < 0) return opfs_result(ctx, opfs_error(h));
    n_result size = n_document_size(h);
    if (size < 0) { n_handle_close(h); return opfs_result(ctx, opfs_error(size)); }
    if ((uint64_t)size > SIZE_MAX) { n_handle_close(h); return opfs_result(ctx, OPFS_MEMORY); }
    uint8_t *data = malloc(size ? (size_t)size : 1);
    if (!data) { n_handle_close(h); return opfs_result(ctx, OPFS_MEMORY); }
    size_t done;
    r = full_io(h, 0, data, (size_t)size, false, &done);
    n_handle_close(h);
    if (r) { free(data); return opfs_result(ctx, r); }
    JSValue buffer = JS_NewArrayBufferCopy(ctx, data, done);
    free(data);
    if (JS_IsException(buffer)) return buffer;
    JSValue out = JS_NewArray(ctx);
    if (JS_IsException(out)) { JS_FreeValue(ctx, buffer); return out; }
    if (JS_SetPropertyUint32(ctx, out, 0, buffer) < 0 ||
        JS_SetPropertyUint32(ctx, out, 1, JS_NewFloat64(ctx, (double)st.modified * 1000.0)) < 0) {
        JS_FreeValue(ctx, out);
        return JS_EXCEPTION;
    }
    return out;
}

/* host.opfsReplace(parts, buffer, offset, length): commit a writable
 * stream's swap contents unless a sync access handle holds the file. */
static JSValue opfs_replace(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    if (r) return opfs_result(ctx, r);
    size_t n;
    uint8_t *data = buffer_range(ctx, argv[1], argv[2], argv[3], &n);
    if (!data) return JS_EXCEPTION;
    if ((r = kind_of(path)) != 1) return opfs_result(ctx, r < 0 ? r : r ? OPFS_TYPE_MISMATCH : OPFS_NOT_FOUND);
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h < 0) return opfs_result(ctx, opfs_error(h));
    n_result claimed = n_document_claim(h, false);
    if (claimed < 0) { n_handle_close(h); return opfs_result(ctx, opfs_error(claimed)); }
    size_t done = 0;
    n_result emptied = n_document_set_size(h, 0);
    if (emptied < 0) r = opfs_error(emptied);
    else if (!(r = full_io(h, 0, data, n, true, &done)) && done != n) r = OPFS_IO;
    n_document_release(h);
    n_handle_close(h);
    return opfs_result(ctx, r);
}

/* host.opfsLocked(parts) -> 0 when no sync access handle holds the file. */
static JSValue opfs_locked(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    return opfs_result(ctx, r ? r : file_free(path));
}

/* host.opfsMove(parts, destinationParts) */
static JSValue opfs_move(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char from[OPFS_PATH], to[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], from, sizeof from);
    if (!r) r = opfs_path(ctx, argv[1], to, sizeof to);
    if (r) return opfs_result(ctx, r);
    if (!strcmp(from, to)) return opfs_result(ctx, kind_of(from) > 0 ? 0 : OPFS_NOT_FOUND);
    size_t n = strlen(from);
    /* A directory cannot move into its own subtree. */
    if (!strncmp(from, to, n) && to[n] == '/') return opfs_result(ctx, OPFS_STATE);
    char parent[OPFS_PATH];
    memcpy(parent, to, sizeof parent);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = 0;
    if (kind_of(parent) != 2) return opfs_result(ctx, OPFS_NOT_FOUND);
    return opfs_result(ctx, move_tree(from, to, sizeof from));
}

/* host.opfsOpen(parts) -> access id holding the file's exclusive lease. */
static JSValue opfs_open(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    char path[OPFS_PATH];
    int r = opfs_path(ctx, argv[0], path, sizeof path);
    if (r) return opfs_result(ctx, r);
    if ((r = kind_of(path)) != 1) return opfs_result(ctx, r < 0 ? r : r ? OPFS_TYPE_MISMATCH : OPFS_NOT_FOUND);
    struct opfs_access *a = calloc(1, sizeof *a);
    if (!a) return opfs_result(ctx, OPFS_MEMORY);
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h < 0) { free(a); return opfs_result(ctx, opfs_error(h)); }
    n_result claimed = n_document_claim(h, false);
    if (claimed < 0) { n_handle_close(h); free(a); return opfs_result(ctx, opfs_error(claimed)); }
    a->ctx = ctx;
    a->document = h;
    a->next = accesses;
    accesses = a;
    return JS_NewInt32(ctx, h);
}

/* host.opfsAccess(op, id, buffer, offset, length, at): 0 read, 1 write,
 * 2 truncate to at, 3 size, 4 flush, 5 close. */
static JSValue opfs_access(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op = 0, id = -1;
    if (JS_ToInt32(ctx, &op, argv[0]) || JS_ToInt32(ctx, &id, argv[1])) return JS_EXCEPTION;
    struct opfs_access **link = &accesses;
    while (*link && ((*link)->ctx != ctx || (*link)->document != id)) link = &(*link)->next;
    if (!*link) return opfs_result(ctx, OPFS_STATE);
    n_handle h = id;
    int64_t at = 0;
    if ((op <= 2) && argc > 5 && JS_ToInt64(ctx, &at, argv[5])) return JS_EXCEPTION;
    if (at < 0) return opfs_result(ctx, OPFS_STATE);
    switch (op) {
    case 0: case 1: {
        size_t n;
        uint8_t *data = buffer_range(ctx, argv[2], argv[3], argv[4], &n);
        if (!data) return JS_EXCEPTION;
        size_t done;
        int r = full_io(h, (uint64_t)at, data, n, op == 1, &done);
        return r && !done ? opfs_result(ctx, r) : JS_NewInt64(ctx, (int64_t)done);
    }
    case 2: {
        n_result sized = n_document_set_size(h, (uint64_t)at);
        return opfs_result(ctx, sized < 0 ? opfs_error(sized) : 0);
    }
    case 3: {
        n_result size = n_document_size(h);
        return opfs_result(ctx, size < 0 ? opfs_error(size) : size);
    }
    case 4:
        /* Every write reaches the volume before it returns. */
        return JS_NewInt32(ctx, 0);
    case 5: {
        struct opfs_access *a = *link;
        *link = a->next;
        n_document_release(h);
        n_handle_close(h);
        free(a);
        return JS_NewInt32(ctx, 0);
    }
    }
    return JS_ThrowRangeError(ctx, "Unknown access operation");
}

/* host.opfsUsage() -> bytes stored for the origin. */
static JSValue opfs_usage(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    char path[OPFS_PATH];
    int r = opfs_root(ctx, path, sizeof path);
    return opfs_result(ctx, r ? r : tree_usage(path, sizeof path));
}

void web_opfs_host_init(JSContext *ctx, JSValueConst host, const char *(*url)(JSContext *ctx)) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("opfsKind", 1, opfs_kind),
        JS_CFUNC_DEF("opfsEnsure", 3, opfs_ensure),
        JS_CFUNC_DEF("opfsList", 1, opfs_list),
        JS_CFUNC_DEF("opfsRemove", 2, opfs_remove),
        JS_CFUNC_DEF("opfsRead", 1, opfs_read),
        JS_CFUNC_DEF("opfsReplace", 4, opfs_replace),
        JS_CFUNC_DEF("opfsLocked", 1, opfs_locked),
        JS_CFUNC_DEF("opfsMove", 2, opfs_move),
        JS_CFUNC_DEF("opfsOpen", 1, opfs_open),
        JS_CFUNC_DEF("opfsAccess", 6, opfs_access),
        JS_CFUNC_DEF("opfsUsage", 0, opfs_usage),
    };
    context_url = url;
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}

void web_opfs_release(JSContext *ctx) {
    for (struct opfs_access **link = &accesses; *link;) {
        struct opfs_access *a = *link;
        if (a->ctx != ctx) { link = &a->next; continue; }
        *link = a->next;
        n_document_release(a->document);
        n_handle_close(a->document);
        free(a);
    }
}
