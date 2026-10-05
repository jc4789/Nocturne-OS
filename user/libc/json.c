/* JSON parser (to a malloc'd tree) and builder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

/* ---------------------------------------------------------------- parser */

struct parser {
    const char *p, *end;
    const char *err;
    int depth;
};

static void ws(struct parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static struct json *node(int type) {
    struct json *j = calloc(1, sizeof *j);
    if (j) j->type = type;
    return j;
}

static bool push(struct json *j, char *key, struct json *v) {
    if (j->n == j->cap) {
        int nc = j->cap ? j->cap * 2 : 4;
        struct json **ni = realloc(j->items, sizeof *ni * nc);
        if (!ni) return false;
        j->items = ni;
        if (j->type == J_OBJ) {
            char **nk = realloc(j->keys, sizeof *nk * nc);
            if (!nk) return false;
            j->keys = nk;
        }
        j->cap = nc;
    }
    if (j->type == J_OBJ) j->keys[j->n] = key;
    j->items[j->n++] = v;
    return true;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t utf8_put(char *o, unsigned cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* parse a string starting at the opening quote; returns malloc'd text */
static char *pstring(struct parser *ps, size_t *outlen) {
    ps->p++; /* " */
    const char *s = ps->p;
    size_t raw = 0;
    while (s + raw < ps->end && s[raw] != '"') raw += s[raw] == '\\' ? 2 : 1;
    if (s + raw >= ps->end) {
        ps->err = "unterminated string";
        return NULL;
    }
    char *out = malloc(raw + 1); /* decoded text is never longer than the escaped form */
    if (!out) {
        ps->err = "out of memory";
        return NULL;
    }
    size_t o = 0;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') {
            out[o++] = c;
            continue;
        }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'u': {
            if (ps->end - ps->p < 4 || hex4(ps->p) < 0) {
                free(out);
                ps->err = "bad \\u escape";
                return NULL;
            }
            unsigned cp = (unsigned)hex4(ps->p);
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                int lo = hex4(ps->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                    ps->p += 6;
                }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD; /* lone surrogate */
            o += utf8_put(out + o, cp);
            break;
        }
        default: out[o++] = e; break; /* \" \\ \/ */
        }
    }
    ps->p++; /* closing quote */
    out[o] = 0;
    *outlen = o;
    return out;
}

static struct json *pvalue(struct parser *ps);

static struct json *pcontainer(struct parser *ps, bool obj) {
    struct json *j = node(obj ? J_OBJ : J_ARR);
    if (!j) return NULL;
    ps->p++;
    ws(ps);
    if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
        ps->p++;
        return j;
    }
    for (;;) {
        char *key = NULL;
        if (obj) {
            ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') {
                ps->err = "expected a key";
                break;
            }
            size_t kl;
            key = pstring(ps, &kl);
            if (!key) break;
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                ps->err = "expected ':'";
                break;
            }
            ps->p++;
        }
        struct json *v = pvalue(ps);
        if (!v || !push(j, key, v)) {
            free(key);
            json_free(v);
            if (!ps->err) ps->err = "out of memory";
            break;
        }
        ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            return j;
        }
        ps->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
        break;
    }
    json_free(j);
    return NULL;
}

static struct json *pvalue(struct parser *ps) {
    ws(ps);
    if (ps->p >= ps->end) {
        ps->err = "unexpected end of input";
        return NULL;
    }
    char c = *ps->p;
    if (c == '{' || c == '[') {
        if (++ps->depth > 200) {
            ps->err = "nested too deeply";
            return NULL;
        }
        struct json *j = pcontainer(ps, c == '{');
        ps->depth--;
        return j;
    }
    if (c == '"') {
        struct json *j = node(J_STR);
        if (!j) return NULL;
        j->str = pstring(ps, &j->len);
        if (!j->str) {
            free(j);
            return NULL;
        }
        return j;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (ps->p + n < ps->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", ps->p[n])) n++;
        memcpy(tmp, ps->p, n);
        tmp[n] = 0;
        ps->p += n;
        struct json *j = node(J_NUM);
        if (j) j->num = strtod(tmp, NULL);
        return j;
    }
    static const struct {
        const char *word;
        int type;
        bool b;
    } words[] = {{"true", J_BOOL, true}, {"false", J_BOOL, false}, {"null", J_NULL, false}};
    for (int i = 0; i < 3; i++) {
        size_t wl = strlen(words[i].word);
        if ((size_t)(ps->end - ps->p) >= wl && !memcmp(ps->p, words[i].word, wl)) {
            ps->p += wl;
            struct json *j = node(words[i].type);
            if (j) j->b = words[i].b;
            return j;
        }
    }
    ps->err = "unexpected character";
    return NULL;
}

struct json *json_parse(const char *s, size_t len, const char **err) {
    struct parser ps = {s, s + len, NULL, 0};
    struct json *j = pvalue(&ps);
    if (j) {
        ws(&ps);
        if (ps.p != ps.end) {
            json_free(j);
            j = NULL;
            ps.err = "trailing characters";
        }
    }
    if (!j && err) *err = ps.err ? ps.err : "out of memory";
    return j;
}

void json_free(struct json *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

struct json *json_get(const struct json *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

struct json *json_at(const struct json *arr, int i) {
    if (!arr || (arr->type != J_ARR && arr->type != J_OBJ) || i < 0 || i >= arr->n) return NULL;
    return arr->items[i];
}

const char *json_str(const struct json *j) { return j && j->type == J_STR ? j->str : NULL; }
const char *json_getstr(const struct json *obj, const char *key) { return json_str(json_get(obj, key)); }

double json_getnum(const struct json *obj, const char *key, double def) {
    struct json *j = json_get(obj, key);
    return j && j->type == J_NUM ? j->num : def;
}

/* ---------------------------------------------------------------- builder */

void jb_init(struct jbuf *b) { memset(b, 0, sizeof *b); }

void jb_free(struct jbuf *b) {
    free(b->s);
    jb_init(b);
}

static bool grow(struct jbuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    char *ns = realloc(b->s, nc);
    if (!ns) {
        b->oom = true;
        return false;
    }
    b->s = ns;
    b->cap = nc;
    return true;
}

void jb_raw(struct jbuf *b, const char *s, size_t n) {
    if (!grow(b, n)) return;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void jb_puts(struct jbuf *b, const char *s) { jb_raw(b, s, strlen(s)); }

void jb_vprintf(struct jbuf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0 || !grow(b, (size_t)n)) return;
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void jb_printf(struct jbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    jb_vprintf(b, fmt, ap);
    va_end(ap);
}

/* length of a valid UTF-8 sequence at s (0 if invalid) */
static size_t utf8_len(const unsigned char *s, size_t avail) {
    unsigned char c = s[0];
    size_t n;
    unsigned min;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) n = 2, min = 0x80;
    else if ((c & 0xF0) == 0xE0) n = 3, min = 0x800;
    else if ((c & 0xF8) == 0xF0) n = 4, min = 0x10000;
    else return 0;
    if (n > avail) return 0;
    unsigned cp = c & (0x3F >> (n - 1));
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0;
    return n;
}

void jb_strn(struct jbuf *b, const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    if (!grow(b, n + 2)) return;
    b->s[b->len++] = '"';
    size_t i = 0;
    while (i < n) {
        /* copy a run of plain characters at once */
        size_t run = 0;
        while (i + run < n && s[i + run] >= 0x20 && s[i + run] < 0x80 && s[i + run] != '"' && s[i + run] != '\\') run++;
        if (run) {
            jb_raw(b, (const char *)s + i, run);
            i += run;
            continue;
        }
        unsigned char c = s[i];
        char esc[8];
        if (c == '"') jb_raw(b, "\\\"", 2);
        else if (c == '\\') jb_raw(b, "\\\\", 2);
        else if (c == '\n') jb_raw(b, "\\n", 2);
        else if (c == '\r') jb_raw(b, "\\r", 2);
        else if (c == '\t') jb_raw(b, "\\t", 2);
        else if (c < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", c);
            jb_raw(b, esc, 6);
        } else {
            size_t l = utf8_len(s + i, n - i);
            if (l) {
                jb_raw(b, (const char *)s + i, l);
                i += l;
                continue;
            }
            jb_raw(b, "\\ufffd", 6);
        }
        i++;
    }
    jb_raw(b, "\"", 1);
}

void jb_str(struct jbuf *b, const char *s) { jb_strn(b, s ? s : "", s ? strlen(s) : 0); }

void json_write(struct jbuf *b, const struct json *j) {
    if (!j) {
        jb_puts(b, "null");
        return;
    }
    switch (j->type) {
    case J_NULL: jb_puts(b, "null"); break;
    case J_BOOL: jb_puts(b, j->b ? "true" : "false"); break;
    case J_NUM:
        if (j->num == (double)(long long)j->num && j->num > -1e15 && j->num < 1e15) jb_printf(b, "%lld", (long long)j->num);
        else jb_printf(b, "%.17g", j->num);
        break;
    case J_STR: jb_strn(b, j->str, j->len); break;
    case J_ARR:
    case J_OBJ:
        jb_raw(b, j->type == J_ARR ? "[" : "{", 1);
        for (int i = 0; i < j->n; i++) {
            if (i) jb_raw(b, ",", 1);
            if (j->type == J_OBJ) {
                jb_str(b, j->keys[i]);
                jb_raw(b, ":", 1);
            }
            json_write(b, j->items[i]);
        }
        jb_raw(b, j->type == J_ARR ? "]" : "}", 1);
        break;
    }
}

char *jb_take(struct jbuf *b) {
    if (!b->s) grow(b, 0);
    char *s = b->s;
    if (s) s[b->len] = 0;
    jb_init(b);
    return s;
}
