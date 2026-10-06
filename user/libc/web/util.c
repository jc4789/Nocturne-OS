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
        if ((a->limit && (cap > a->limit || a->allocated > a->limit - cap)) ||
            cap > SIZE_MAX - sizeof(struct achunk)) {
            if (a->trap) longjmp(*a->trap, 1);
            abort();
        }
        c = malloc(sizeof(struct achunk) + cap);
        if (!c) {
            if (a->trap) longjmp(*a->trap, 1);
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
    char path[2048];
    char query[2048];
    bool has_query;
    char frag[512];
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

static void copy_until(char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* split a URL that may lack a scheme or authority */
static void url_split(const char *s, struct urlparts *u) {
    memset(u, 0, sizeof *u);
    size_t sl = scheme_len(s);
    if (sl) {
        for (size_t i = 0; i < sl; i++) u->scheme[i] = (char)lower((unsigned char)s[i]);
        s += sl + 1;
    }
    if (s[0] == '/' && s[1] == '/') {
        s += 2;
        size_t n = strcspn(s, "/?#");
        copy_until(u->auth, sizeof u->auth, s, n);
        for (char *p = u->auth; *p; p++) *p = (char)lower((unsigned char)*p);
        u->has_auth = true;
        s += n;
    }
    size_t n = strcspn(s, "?#");
    copy_until(u->path, sizeof u->path, s, n);
    s += n;
    if (*s == '?') {
        s++;
        n = strcspn(s, "#");
        copy_until(u->query, sizeof u->query, s, n);
        u->has_query = true;
        s += n;
    }
    if (*s == '#') {
        copy_until(u->frag, sizeof u->frag, s + 1, strlen(s + 1));
        u->has_frag = true;
    }
}

/* RFC 3986 5.2.4 */
static void remove_dots(char *path) {
    char out[2048];
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
                if (on < sizeof out - 1) out[on++] = *in;
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

bool url_resolve(const char *base, const char *rel, char *out, size_t n) {
    /* strip surrounding whitespace and remove tabs and newlines, as browsers do */
    char clean[4096];
    size_t cn = 0;
    while (is_space((unsigned char)*rel)) rel++;
    for (const char *p = rel; *p && cn < sizeof clean - 1; p++)
        if (*p != '\t' && *p != '\n' && *p != '\r') clean[cn++] = *p == '\\' ? '/' : *p;
    while (cn > 0 && is_space((unsigned char)clean[cn - 1])) cn--;
    clean[cn] = 0;

    static struct urlparts b, r, t; /* large; the engine is single-threaded */
    url_split(clean, &r);
    if (r.scheme[0] && !special_scheme(r.scheme)) { /* data:, mailto:, javascript:, about: ... */
        snprintf(out, n, "%s", clean);
        return true;
    }
    url_split(base ? base : "", &b);
    if (r.scheme[0] && (r.has_auth || strcmp(r.scheme, b.scheme))) {
        t = r;
        remove_dots(t.path);
    } else {
        if (!b.scheme[0]) return false;
        memset(&t, 0, sizeof t);
        strcpy(t.scheme, b.scheme);
        if (r.has_auth) {
            strcpy(t.auth, r.auth);
            t.has_auth = true;
            strcpy(t.path, r.path);
            remove_dots(t.path);
            strcpy(t.query, r.query);
            t.has_query = r.has_query;
        } else {
            strcpy(t.auth, b.auth);
            t.has_auth = b.has_auth;
            if (!r.path[0]) {
                strcpy(t.path, b.path);
                if (r.has_query) {
                    strcpy(t.query, r.query);
                    t.has_query = true;
                } else {
                    strcpy(t.query, b.query);
                    t.has_query = b.has_query;
                }
            } else {
                if (r.path[0] == '/') strcpy(t.path, r.path);
                else {
                    /* merge with the base path up to its last slash */
                    char *slash = strrchr(b.path, '/');
                    size_t keep = slash ? (size_t)(slash - b.path + 1) : 0;
                    if (!keep && b.has_auth) snprintf(t.path, sizeof t.path, "/%s", r.path);
                    else snprintf(t.path, sizeof t.path, "%.*s%s", (int)keep, b.path, r.path);
                }
                remove_dots(t.path);
                strcpy(t.query, r.query);
                t.has_query = r.has_query;
            }
        }
        strcpy(t.frag, r.frag);
        t.has_frag = r.has_frag;
    }
    if (!t.path[0] && t.has_auth) strcpy(t.path, "/");

    /* reassemble, percent-encoding what may not appear in a URL */
    sbuf o = {0};
    sb_puts(&o, t.scheme);
    sb_putc(&o, ':');
    if (t.has_auth) {
        sb_puts(&o, "//");
        sb_puts(&o, t.auth);
    }
    const char *parts[3] = {t.path, t.has_query ? t.query : NULL, t.has_frag ? t.frag : NULL};
    for (int k = 0; k < 3; k++) {
        if (!parts[k]) continue;
        if (k == 1) sb_putc(&o, '?');
        if (k == 2) sb_putc(&o, '#');
        for (const unsigned char *p = (const unsigned char *)parts[k]; *p; p++) {
            if (*p <= ' ' || *p >= 0x7F || *p == '"' || *p == '<' || *p == '>' || *p == '`' ||
                (k == 0 && *p == '{') || (k == 0 && *p == '}')) {
                char hx[4];
                snprintf(hx, sizeof hx, "%%%02X", *p);
                sb_puts(&o, hx);
            } else {
                sb_putc(&o, (char)*p);
            }
        }
    }
    bool ok = o.n < n;
    snprintf(out, n, "%s", sb_cstr(&o));
    sb_free(&o);
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
