/* Names and paths on the store (store/names.h). */
#include "store/volume.h"
#include "store/fold_table.h"

int fold_load(const struct store_env *env, const void *data, size_t n, struct fold *out) {
    const struct store_fold_header *h = data;
    if (n < sizeof *h || memcmp(h->magic, "NOCFOLD1", 8) || h->count > (n - sizeof *h) / 8) return -N_ERR_IO;
    out->unicode = h->unicode;
    out->count = h->count;
    out->pairs = env->alloc(h->count * sizeof *out->pairs + 1);
    if (!out->pairs) return -N_ERR_NO_MEMORY;
    memcpy(out->pairs, h + 1, h->count * sizeof *out->pairs);
    for (uint32_t i = 1; i < out->count; i++)
        if (out->pairs[i][0] <= out->pairs[i - 1][0]) {
            fold_free(env, out);
            return -N_ERR_IO;
        }
    return 0;
}

void fold_free(const struct store_env *env, struct fold *f) {
    env->free(f->pairs);
    f->pairs = NULL;
    f->count = 0;
}

int fold_document(const struct store_env *env, uint8_t **out, size_t *n) {
    struct store_fold_header h = {.unicode = FOLD_UNICODE, .count = FOLD_PAIRS};
    memcpy(h.magic, "NOCFOLD1", 8);
    *n = sizeof h + sizeof fold_pairs;
    if (!(*out = env->alloc(*n))) return -N_ERR_NO_MEMORY;
    memcpy(*out, &h, sizeof h);
    memcpy(*out + sizeof h, fold_pairs, sizeof fold_pairs);
    return 0;
}

/* the code point at *p (n bytes left), *p moved past it; -1 for bytes that are not UTF-8 */
static int32_t next_code(const uint8_t **p, size_t *n) {
    const uint8_t *s = *p;
    uint32_t c = s[0], need, min;
    if (c < 0x80) need = 0, min = 0;
    else if ((c & 0xE0) == 0xC0) c &= 0x1F, need = 1, min = 0x80;
    else if ((c & 0xF0) == 0xE0) c &= 0x0F, need = 2, min = 0x800;
    else if ((c & 0xF8) == 0xF0) c &= 0x07, need = 3, min = 0x10000;
    else return -1;
    if (need >= *n) return -1;
    for (uint32_t i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) return -1;
        c = c << 6 | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
    *p += need + 1;
    *n -= need + 1;
    return (int32_t)c;
}

int name_check(const char *name, size_t n) {
    if (!n) return -N_ERR_INVALID;
    if (n > 255) return -N_ERR_NAME_TOO_LONG;
    if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) return -N_ERR_INVALID;
    if (name[n - 1] == ' ' || name[n - 1] == '.') return -N_ERR_INVALID;
    const uint8_t *p = (const uint8_t *)name;
    size_t left = n;
    while (left) {
        int32_t c = next_code(&p, &left);
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0)) return -N_ERR_INVALID;
        if (c < 0x80 && strchr("\\/:*?\"<>|", c)) return -N_ERR_INVALID;
    }
    return 0;
}

static uint32_t fold_code(const struct fold *f, uint32_t c) {
    uint32_t lo = 0, hi = f->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (f->pairs[mid][0] < c) lo = mid + 1;
        else hi = mid;
    }
    return lo < f->count && f->pairs[lo][0] == c ? f->pairs[lo][1] : c;
}

int name_fold(const struct fold *f, const char *name, size_t n, uint8_t *out, size_t cap) {
    const uint8_t *p = (const uint8_t *)name;
    size_t at = 0;
    while (n) {
        int32_t got = next_code(&p, &n);
        if (got < 0) return -N_ERR_INVALID;
        uint32_t c = fold_code(f, (uint32_t)got);
        size_t k = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (at + k > cap) return -N_ERR_NAME_TOO_LONG;
        if (k == 1) out[at] = (uint8_t)c;
        else {
            out[at] = (uint8_t)((k == 2 ? 0xC0 : k == 3 ? 0xE0 : 0xF0) | c >> (6 * (k - 1)));
            for (size_t i = 1; i < k; i++) out[at + i] = (uint8_t)(0x80 | (c >> (6 * (k - 1 - i)) & 0x3F));
        }
        at += k;
    }
    return (int)at;
}

int path_parse(const struct store_env *env, const char *text, struct path *out) {
    out->count = 0;
    out->part = NULL;
    const char *p = text;
    if (p[0] && p[1] == ':') {
        if (p[0] != 'C' && p[0] != 'c') return (p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z' ? -N_ERR_NOT_FOUND : -N_ERR_INVALID;
        p += 2;
    }
    if (*p != '\\' && *p != '/') return -N_ERR_INVALID; /* no working folder: "C:a" and "a" name nothing */
    size_t most = 1;
    for (const char *s = p; *s; s++) most += *s == '\\' || *s == '/';
    if (!(out->part = env->alloc(most * sizeof *out->part))) return -N_ERR_NO_MEMORY;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        const char *name = p;
        while (*p && *p != '\\' && *p != '/') p++;
        size_t n = (size_t)(p - name);
        if (!n || (n == 1 && name[0] == '.')) continue;
        if (n == 2 && name[0] == '.' && name[1] == '.') {
            if (out->count) out->count--;
            continue;
        }
        int r = name_check(name, n);
        if (r < 0) {
            path_free(env, out);
            return r;
        }
        out->part[out->count].name = name;
        out->part[out->count++].n = n;
    }
    return 0;
}

void path_free(const struct store_env *env, struct path *p) {
    env->free(p->part);
    p->part = NULL;
    p->count = 0;
}
