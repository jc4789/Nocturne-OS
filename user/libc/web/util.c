/* Arenas, string buffers and URL resolution for the web engine. */
#include <stdio.h>
#include "webi.h"

/* ---------------------------------------------------------------- arena */
struct achunk {
    struct achunk *next;
    size_t used, cap;
    char data[];
};

void *ar_alloc(arena_t *a, size_t n) {
    if (n > SIZE_MAX - 7) {
        if (a->trap) longjmp(*a->trap, 1);
        abort();
    }
    n = (n + 7) & ~(size_t)7;
    struct achunk *c = a->head;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 60000 ? n : 65536 - sizeof(struct achunk);
        bool quota=a->limit && (cap>a->limit || a->allocated>a->limit-cap);
        if (quota || cap > SIZE_MAX - sizeof(struct achunk)) {
            /* Every existing arena trap tests nonzero; retain the failure
               semantics while distinguishing quota from allocator failure. */
            if (a->trap) longjmp(*a->trap, quota ? 2 : 1);
            abort();
        }
        c = malloc(sizeof(struct achunk) + cap);
        if (!c) {
            if (a->trap) longjmp(*a->trap, 3);
            abort();
        }
        a->allocated += cap;
        c->used = 0;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *ar_strndup(arena_t *a, const char *s, size_t n) {
    char *p = ar_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *ar_strdup(arena_t *a, const char *s) { return ar_strndup(a, s, strlen(s)); }

void ar_free(arena_t *a) {
    for (struct achunk *c = a->head, *nx; c; c = nx) {
        nx = c->next;
        free(c);
    }
    a->head = NULL;
    a->allocated = 0;
}

/* ---------------------------------------------------------------- buffers */
static void sb_grow(sbuf *b, size_t need) {
    if (b->n + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->n + need + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) abort();
    b->p = p;
    b->cap = cap;
}

void sb_put(sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

void sb_putc(sbuf *b, char c) {
    sb_grow(b, 1);
    b->p[b->n++] = c;
}

int utf8_put(char *o, uint32_t cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 63));
        o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    if (cp > 0x10FFFF) return utf8_put(o, 0xFFFD);
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63));
    o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

void sb_utf8(sbuf *b, uint32_t cp) {
    char t[4];
    sb_put(b, t, (size_t)utf8_put(t, cp));
}

char *sb_cstr(sbuf *b) {
    sb_grow(b, 0);
    b->p[b->n] = 0;
    return b->p;
}

void sb_free(sbuf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void pv_push(pvec *p, void *x) {
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        void **v = realloc(p->v, sizeof(void *) * (size_t)cap);
        if (!v) abort();
        p->v = v;
        p->cap = cap;
    }
    p->v[p->n++] = x;
}

void pv_free(pvec *p) {
    free(p->v);
    p->v = NULL;
    p->n = p->cap = 0;
}

bool str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
    return *a == *b;
}

bool strn_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++, b++)
        if (!*b || lower((unsigned char)a[i]) != lower((unsigned char)*b)) return false;
    return *b == 0;
}

/* ---------------------------------------------------------------- URLs */
struct urlparts {
    char scheme[16];
    char auth[256]; /* host[:port], empty for file: */
    bool has_auth;
    char path[HTTP_URL_MAX];
    char query[HTTP_URL_MAX];
    bool has_query;
    char frag[HTTP_URL_MAX];
    bool has_frag;
};

static size_t scheme_len(const char *s) {
    size_t i = 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return 0;
    while (s[i] && (((s[i] | 32) >= 'a' && (s[i] | 32) <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '+' ||
                    s[i] == '-' || s[i] == '.'))
        i++;
    return s[i] == ':' && i < 15 ? i : 0;
}

static bool copy_until(char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) return false;
    memcpy(dst, s, n);
    dst[n] = 0;
    return true;
}

/* split a URL that may lack a scheme or authority */
static bool url_split(const char *s, struct urlparts *u) {
    memset(u, 0, sizeof *u);
    size_t sl = scheme_len(s);
    if (sl) {
        for (size_t i = 0; i < sl; i++) u->scheme[i] = (char)lower((unsigned char)s[i]);
        s += sl + 1;
    }
    if (s[0] == '/' && s[1] == '/') {
        s += 2;
        size_t n = strcspn(s, "/?#");
        if (!copy_until(u->auth, sizeof u->auth, s, n)) return false;
        for (char *p = u->auth; *p; p++) *p = (char)lower((unsigned char)*p);
        u->has_auth = true;
        s += n;
    }
    size_t n = strcspn(s, "?#");
    if (!copy_until(u->path, sizeof u->path, s, n)) return false;
    s += n;
    if (*s == '?') {
        s++;
        n = strcspn(s, "#");
        if (!copy_until(u->query, sizeof u->query, s, n)) return false;
        u->has_query = true;
        s += n;
    }
    if (*s == '#') {
        if (!copy_until(u->frag, sizeof u->frag, s + 1, strlen(s + 1))) return false;
        u->has_frag = true;
    }
    return true;
}

/* RFC 3986 5.2.4 */
static void remove_dots(char *path, char *out) {
    size_t on = 0;
    const char *in = path;
    while (*in) {
        if (!strncmp(in, "../", 3)) in += 3;
        else if (!strncmp(in, "./", 2)) in += 2;
        else if (!strncmp(in, "/./", 3)) in += 2;
        else if (!strcmp(in, "/.")) in = "/";
        else if (!strncmp(in, "/../", 4) || !strcmp(in, "/..")) {
            in = in[3] ? in + 3 : "/";
            while (on > 0 && out[on - 1] != '/') on--;
            if (on > 0) on--;
        } else if (!strcmp(in, ".") || !strcmp(in, "..")) in += strlen(in);
        else {
            do {
                out[on++] = *in; /* removal never grows a checked input path */
                in++;
            } while (*in && *in != '/');
        }
    }
    out[on] = 0;
    strcpy(path, out);
}

static bool special_scheme(const char *s) {
    return !strcmp(s, "http") || !strcmp(s, "https") || !strcmp(s, "file") || !strcmp(s, "ftp");
}

struct resolve_scratch {
    struct urlparts b, r, t;
    char clean[HTTP_URL_MAX], dots[HTTP_URL_MAX];
};
static bool url_append(char *out, size_t *used, const char *text, size_t length) {
    if (length >= HTTP_URL_MAX - *used) return false;
    memcpy(out + *used, text, length); *used += length; out[*used] = 0;
    return true;
}
static bool resolve_inner(const char *base, const char *rel, char *out, size_t n,
                          struct resolve_scratch *work) {
    /* strip surrounding whitespace and remove tabs and newlines, as browsers do */
    char *clean = work->clean;
    size_t cn = 0;
    while (is_space((unsigned char)*rel)) rel++;
    for (const char *p = rel; *p; p++) {
        if (*p == '\t' || *p == '\n' || *p == '\r') continue;
        if (cn + 1 >= HTTP_URL_MAX) return false;
        clean[cn++] = *p == '\\' ? '/' : *p;
    }
    while (cn > 0 && is_space((unsigned char)clean[cn - 1])) cn--;
    clean[cn] = 0;

    /* Large, reentrant scratch is caller-owned heap storage, not GUI/JS stack. */
    struct urlparts *b = &work->b, *r = &work->r, *t = &work->t;
    if (!url_split(clean, r)) return false;
    if (r->scheme[0] && !special_scheme(r->scheme)) { /* data:, mailto:, javascript:, about: ... */
        if (cn >= n) return false;
        memcpy(out, clean, cn + 1);
        return true;
    }
    if (base && strlen(base) >= HTTP_URL_MAX) return false;
    if (!url_split(base ? base : "", b)) return false;
    if (r->scheme[0] && (r->has_auth || strcmp(r->scheme, b->scheme))) {
        *t = *r;
        remove_dots(t->path, work->dots);
    } else {
        if (!b->scheme[0]) return false;
        memset(t, 0, sizeof *t);
        strcpy(t->scheme, b->scheme);
        if (r->has_auth) {
            strcpy(t->auth, r->auth);
            t->has_auth = true;
            strcpy(t->path, r->path);
            remove_dots(t->path, work->dots);
            strcpy(t->query, r->query);
            t->has_query = r->has_query;
        } else {
            strcpy(t->auth, b->auth);
            t->has_auth = b->has_auth;
            if (!r->path[0]) {
                strcpy(t->path, b->path);
                if (r->has_query) {
                    strcpy(t->query, r->query);
                    t->has_query = true;
                } else {
                    strcpy(t->query, b->query);
                    t->has_query = b->has_query;
                }
            } else {
                if (r->path[0] == '/') strcpy(t->path, r->path);
                else {
                    /* merge with the base path up to its last slash */
                    char *slash = strrchr(b->path, '/');
                    size_t keep = slash ? (size_t)(slash - b->path + 1) : 0;
                    int length = !keep && b->has_auth
                        ? snprintf(t->path, sizeof t->path, "/%s", r->path)
                        : snprintf(t->path, sizeof t->path, "%.*s%s", (int)keep, b->path, r->path);
                    if (length < 0 || (size_t)length >= sizeof t->path) return false;
                }
                remove_dots(t->path, work->dots);
                strcpy(t->query, r->query);
                t->has_query = r->has_query;
            }
        }
        strcpy(t->frag, r->frag);
        t->has_frag = r->has_frag;
    }
    if (!t->path[0] && t->has_auth) strcpy(t->path, "/");

    /* reassemble, percent-encoding what may not appear in a URL */
    /* Reuse cleaned-input scratch after splitting. No abort-on-OOM sbuf or
       unbounded encoded expansion is necessary for a URL with a finite bound. */
    char *assembled = work->clean;
    size_t used = 0;
    if (!url_append(assembled, &used, t->scheme, strlen(t->scheme)) ||
        !url_append(assembled, &used, ":", 1)) return false;
    if (t->has_auth) {
        if (!url_append(assembled, &used, "//", 2) ||
            !url_append(assembled, &used, t->auth, strlen(t->auth))) return false;
    }
    const char *parts[3] = {t->path, t->has_query ? t->query : NULL, t->has_frag ? t->frag : NULL};
    for (int k = 0; k < 3; k++) {
        if (!parts[k]) continue;
        if (k && !url_append(assembled, &used, k == 1 ? "?" : "#", 1)) return false;
        for (const unsigned char *p = (const unsigned char *)parts[k]; *p; p++) {
            if (*p <= ' ' || *p >= 0x7F || *p == '"' || *p == '<' || *p == '>' || *p == '`' ||
                (k == 0 && *p == '{') || (k == 0 && *p == '}')) {
                char hx[4];
                snprintf(hx, sizeof hx, "%%%02X", *p);
                if (!url_append(assembled, &used, hx, 3)) return false;
            } else {
                if (!url_append(assembled, &used, (const char *)p, 1)) return false;
            }
        }
    }
    if (used >= n) return false;
    memcpy(out, assembled, used + 1);
    return true;
}

bool url_resolve(const char *base, const char *rel, char *out, size_t n) {
    if (!out || !n) return false;
    if (!rel) { out[0] = 0; return false; }
    struct resolve_scratch *work = calloc(1, sizeof *work);
    if (!work) { out[0] = 0; return false; }
    bool ok = resolve_inner(base, rel, out, n, work);
    free(work);
    if (!ok) out[0] = 0; /* never hand a caller a different, truncated URL */
    return ok;
}

bool web_resolve_url(const char *base, const char *rel, char *out, size_t n) { return url_resolve(base, rel, out, n); }

void url_encode_form(sbuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '*' ||
            *p == '-' || *p == '.' || *p == '_')
            sb_putc(b, (char)*p);
        else if (*p == ' ') sb_putc(b, '+');
        else {
            char hx[4];
            snprintf(hx, sizeof hx, "%%%02X", *p);
            sb_puts(b, hx);
        }
    }
}
