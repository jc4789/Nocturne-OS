/* Cookie policy follows RFC 6265bis section 5 (HTTPWG, September 2026).
   Limits: WEBCOOKIE_DOMAIN_MAX per domain field and WEBCOOKIE_TOTAL_MAX in all (webcookie.h), a
   400-day lifetime; no Lax-allowing-unsafe grace, no partitioned-cookie emulation.
   A URL is read by the URL parser (weburl.h), which gives the canonicalized host name (a domain
   in A-labels, an IPv4 address, an IPv6 address in brackets), the path and the scheme; the jar
   reads no URL text itself. Same site is HTML's (the origins' sites), with this jar's PSL.

   The window process's jar tracks its changes for the saved store (webnet.c keeps the document):
   every set or removal of a cookie the store would hold, and every clear, goes into an ordered
   list of changes, one entry per cookie (the later replaces the earlier), until webcookie_saved.
   Merging a store another process wrote (webcookie_load) takes the store's cookies, applies that
   list on top, and keeps the jar's own session cookies, so neither process loses the other's
   writes. A cookie's saved flag says the store or the list holds it. */
#include "webcookie.h"
#include "weburl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DOMAIN_MAX 253
#define COOKIE_PATH_MAX 1024
#define COOKIE_DAYS (400LL * 86400LL)
#define RECORD_SIZE 40u
#define SNAP_HEADER 24u
#define STORE_HEADER 48u
#define NEVER INT64_MAX
_Static_assert(RECORD_SIZE + 4096u + DOMAIN_MAX + COOKIE_PATH_MAX == WEBCOOKIE_RECORD_MAX, "record bound");
_Static_assert(WEBCOOKIE_TOTAL_MAX <= 0xffffu, "snapshot count is 16 bits");
enum { S_LAX, S_STRICT, S_NONE };
enum { F_HOST = 1, F_SECURE = 2, F_HTTP = 4, F_PERSIST = 8 };
struct cookie {
    char *name, *value, *domain, *path;
    uint16_t nl, vl, dl, pl;
    unsigned flags, same;
    bool saved, doomed;
    int64_t expires;
    uint64_t created, access;
    char data[];
};
enum { CHANGE_SET, CHANGE_DELETE, CHANGE_CLEAR };
/* SET: an owned copy of the cookie. DELETE: an owned copy giving the key. CLEAR: site ("" all). */
struct change { unsigned kind; struct cookie *cookie; char site[DOMAIN_MAX + 1]; };
struct clear { uint64_t serial; char site[DOMAIN_MAX + 1]; };
struct webcookie_jar {
    struct cookie **cookies;
    size_t count, cap, bytes;
    uint64_t serial;
    char *psl_text, **rules;
    size_t rule_count;
    webcookie_rule rule;
    void *rule_opaque;
    bool tracking, overflow; /* overflow: a change could not be listed; every kept cookie is one */
    struct change *changes;
    size_t change_count, change_cap;
    struct clear *clears; /* the store's, oldest first */
    size_t clear_count;
    uint64_t clear_top, clear_seen;
};
/* What a cookie is set or sent for: the request URL's record (the URL parser's, weburl.h), its
   host as a string of its own (the canonicalized host name: a domain in A-labels, or an IP
   address), its path, whether its scheme is secure and whether its host is an IP address. */
struct address { struct web_url url; char *host; const char *path; size_t path_len; bool secure, ip; };

static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
/* A rule of the Public Suffix List: ASCII LDH labels (the list is loaded already converted). */
static bool ascii_host(char *out, const char *src, size_t n) {
    if (!n || n > DOMAIN_MAX) return false;
    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = lower((unsigned char)src[i]);
        if (c == '.') {
            if (!label || src[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!(digit(c) || (c >= 'a' && c <= 'z') || c == '-') || (!label && c == '-') || ++label > 63) return false;
        }
        out[i] = (char)c;
    }
    if (!label || src[n-1] == '-') return false;
    out[n] = 0; return true;
}
/* url read by the URL parser: 1 an http or https URL (address_free a); 0 not one; -1 no memory. */
static int address(const char *url, struct address *a) {
    memset(a, 0, sizeof *a);
    int parsed = web_url_read(url, url ? strlen(url) : 0, &a->url);
    if (parsed != 1) return parsed;
    if (!web_url_http_scheme(&a->url)) { web_url_free(&a->url); return 0; }
    a->secure = web_url_scheme_is(&a->url, "https");
    size_t n;
    const char *host = web_url_host_text(&a->url, &n);
    if (!(a->host = malloc(n + 1))) { web_url_free(&a->url); return -1; }
    memcpy(a->host, host, n); a->host[n] = 0;
    a->ip = a->url.host == WEB_URL_HOST_IPV4 || a->url.host == WEB_URL_HOST_IPV6;
    /* uri-path: a special URL's path always begins with '/'. */
    a->path = web_url_span(&a->url, WEB_URL_PATH_START, WEB_URL_QUERY_START, &a->path_len);
    return 1;
}
static void address_free(struct address *a) { free(a->host); web_url_free(&a->url); }
static bool domain_match(const char *host, const char *domain) {
    size_t h = strlen(host), d = strlen(domain);
    return h == d ? !strcmp(host, domain) : h > d && host[h-d-1] == '.' && !strcmp(host+h-d, domain);
}
static bool path_match_n(const char *path,size_t p,const char *cookie_path) {
    size_t n = strlen(cookie_path);
    return p >= n && !memcmp(path, cookie_path, n) && (p == n || cookie_path[n-1] == '/' || path[n] == '/');
}
static bool path_match(const char *path,const char *cookie_path){return path_match_n(path,strlen(path),cookie_path);}
static int rule_compare(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static bool rule(const webcookie_jar *j, const char *key) {
    size_t lo = 0, hi = j->rule_count;
    while (lo < hi) { size_t mid = lo + (hi-lo)/2; int c = strcmp(key, j->rules[mid]); if (!c) return true; if (c < 0) hi = mid; else lo = mid + 1; }
    return false;
}
static unsigned labels(const char *host) { unsigned n = 1; for (; *host; host++) n += *host == '.'; return n; }
static unsigned suffix_labels(const webcookie_jar *j, const char *host) {
    unsigned best = 1, count = labels(host);
    char key[DOMAIN_MAX + 3];
    for (const char *p = host; p; count--) {
        key[0] = '!'; strcpy(key+1, p);
        if (rule(j, key)) return count - 1;
        if (rule(j, p) && count > best) best = count;
        const char *dot = strchr(p, '.');
        if (dot) { key[0] = '*'; strcpy(key+1, dot); if (rule(j, key) && count > best) best = count; }
        p = dot ? dot+1 : NULL;
    }
    return best;
}
const char *webcookie_site_of(const webcookie_jar *j, const char *host) {
    if (!host) return host;
    size_t n = strlen(host);
    /* A host the URL parser wrote: an IPv6 address is in brackets, and a host of digits and
       dots alone is an IPv4 address (the parser takes any other such text for one, or fails). */
    bool ip = *host == '[';
    if (!ip) { ip = true; for (const char *p = host; *p; p++) if (!digit((unsigned char)*p) && *p != '.') ip = false; }
    if (!j || !j->rule_count || ip || !n || n > DOMAIN_MAX) return host;
    unsigned total = labels(host), suffix = suffix_labels(j, host);
    if (total <= suffix) return host;
    const char *p = host;
    for (unsigned i = total; i > suffix + 1; i--) p = strchr(p, '.') + 1;
    return p;
}
/* HTML's "obtain a site" for the origin of url, serialized (the serialization of a site):
   1 with *site malloc'd for a tuple origin, 0 for an opaque one or no URL, -1 no memory. */
static int url_site(const webcookie_jar *j, const char *url, char **site) {
    *site = NULL;
    char *origin = NULL;
    enum web_url_result kind = web_url_origin(url, &origin);
    if (kind != WEB_URL_TUPLE) return kind == WEB_URL_OOM ? -1 : 0;
    struct web_url u;
    int parsed = web_url_read(origin, strlen(origin), &u);
    free(origin);
    if (parsed != 1) return parsed;
    size_t n, scheme = u.mark[WEB_URL_PROTOCOL_END];
    const char *at = web_url_host_text(&u, &n);
    char *host = malloc(n + 1);
    if (!host) { web_url_free(&u); return -1; }
    memcpy(host, at, n); host[n] = 0;
    const char *registrable = u.host == WEB_URL_HOST_DOMAIN ? webcookie_site_of(j, host) : host;
    size_t r = strlen(registrable);
    *site = malloc(scheme + 2 + r + 1);
    if (*site) { memcpy(*site, u.href, scheme); memcpy(*site + scheme, "//", 2); memcpy(*site + scheme + 2, registrable, r + 1); }
    free(host);
    web_url_free(&u);
    return *site ? 1 : -1;
}
/* HTML's same site for the origins of two URLs: both tuple origins whose sites are the same
   (scheme and registrable domain, or the host itself when it has none). */
bool webcookie_same_site(const webcookie_jar *j, const char *x, const char *y) {
    char *a = NULL, *b = NULL;
    bool same = url_site(j, x, &a) == 1 && url_site(j, y, &b) == 1 && !strcmp(a, b);
    free(a); free(b);
    return same;
}
static bool same_context(const webcookie_jar *j, const struct webcookie_context *c) {
    return !c->redirect_cross_site && (c->site_url ? webcookie_same_site(j, c->url, c->site_url) : c->http && c->top_level);
}
static unsigned policy(const webcookie_jar *j, const char *host) {
    return j->rule ? j->rule(j->rule_opaque, host) : WEBCOOKIE_ALLOW;
}
/* Whether the store holds c: persistent, of a host the rule keeps. */
static bool kept(const webcookie_jar *j, const struct cookie *c) {
    return (c->flags & F_PERSIST) && !(policy(j, c->domain) & WEBCOOKIE_TRANSIENT);
}
/* A creation or access time: the UTC second in the high bits, so that jars of different
   processes order their cookies alike, and a count within the second. */
static uint64_t stamp(webcookie_jar *j, int64_t now) {
    uint64_t base = now > 0 && now < (INT64_C(1) << 43) ? (uint64_t)now << 20 : 0;
    j->serial = j->serial + 1 > base ? j->serial + 1 : base;
    return j->serial;
}
static bool reserve(struct cookie ***list, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t n = *cap ? *cap : 16;
    while (n < need) { if (n > SIZE_MAX / 2 / sizeof **list) return false; n *= 2; }
    struct cookie **grown = realloc(*list, n * sizeof *grown);
    if (!grown) return false;
    *list = grown; *cap = n; return true;
}
webcookie_jar *webcookie_create(void) { return calloc(1, sizeof(webcookie_jar)); }
static void changes_free(webcookie_jar *j) {
    for (size_t i = 0; i < j->change_count; i++) free(j->changes[i].cookie);
    free(j->changes); j->changes = NULL; j->change_count = j->change_cap = 0;
}
void webcookie_free(webcookie_jar *j) {
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) free(j->cookies[i]);
    changes_free(j);
    free(j->cookies); free(j->clears); free(j->rules); free(j->psl_text); free(j);
}
void webcookie_set_rule(webcookie_jar *j, webcookie_rule r, void *opaque) { if (j) { j->rule = r; j->rule_opaque = opaque; } }
void webcookie_track(webcookie_jar *j) { if (j) j->tracking = true; }
bool webcookie_psl_load(webcookie_jar *j, const char *text, size_t len) {
    if (!j || !text || !len || len > WEBCOOKIE_PSL_MAX || memchr(text, 0, len)) return false;
    char *copy = malloc(len+1); char **rules = malloc(20000 * sizeof *rules);
    if (!copy || !rules) { free(copy); free(rules); return false; }
    memcpy(copy, text, len); copy[len] = 0; size_t count = 0;
    for (char *p = copy; *p;) {
        char *end = strchr(p, '\n'); if (end) *end = 0;
        char *tail = p + strlen(p); while (tail > p && (tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && strncmp(p, "//", 2)) {
            char *host = p + (*p == '!' ? 1 : !strncmp(p, "*.", 2) ? 2 : 0);
            char canonical[DOMAIN_MAX+1];
            if (count == 20000 || !ascii_host(canonical, host, strlen(host))) { free(copy); free(rules); return false; }
            strcpy(host, canonical); rules[count++] = p;
        }
        p = end ? end + 1 : tail;
    }
    if (!count) { free(copy); free(rules); return false; }
    qsort(rules, count, sizeof *rules, rule_compare);
    free(j->rules); free(j->psl_text); j->rules = rules; j->psl_text = copy; j->rule_count = count;
    return true;
}
static size_t record_bytes(const struct cookie *c) { return RECORD_SIZE + c->nl + c->vl + c->dl + c->pl; }
static void remove_cookie(webcookie_jar *j, size_t i) {
    j->bytes -= record_bytes(j->cookies[i]); free(j->cookies[i]);
    memmove(j->cookies+i, j->cookies+i+1, (j->count-i-1) * sizeof *j->cookies); j->count--;
}
static void purge(webcookie_jar *j, int64_t now) {
    for (size_t i = 0; i < j->count;) if (j->cookies[i]->expires <= now) remove_cookie(j, i); else i++;
}
static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *end = s + strlen(s); while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0; return s; }
static bool clean(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127) return false;
    return true;
}
static bool name_ok(const char *s) {
    for (; *s; s++) if ((unsigned char)*s <= 32 || (unsigned char)*s >= 127 || strchr("()<>@,;:\\\"/[]?={}", *s)) return false;
    return true;
}
static bool date_delim(unsigned char c) { return c == 9 || (c >= 32 && c <= 47) || (c >= 59 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
static bool number(const char *p, size_t n, int min, int max, int *out, size_t *used) {
    size_t k = 0; int v = 0;
    while (k < n && digit((unsigned char)p[k]) && k <= (size_t)max) v = v*10 + p[k++] - '0';
    if (k < (size_t)min || k > (size_t)max) return false;
    *out = v; if (used) *used = k; return true;
}
static bool cookie_date(const char *p, int64_t *out) {
    int year = -1, month = -1, day = -1, hour = -1, minute = -1, second = -1;
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    while (*p) {
        while (*p && date_delim((unsigned char)*p)) p++;
        const char *end = p; while (*end && !date_delim((unsigned char)*end)) end++;
        size_t n = (size_t)(end-p), k, l, m; int h, mi, sec, v;
        if (!n) break;
        if (hour < 0 && number(p,n,1,2,&h,&k) && k < n && p[k] == ':' &&
            number(p+k+1,n-k-1,1,2,&mi,&l) && k+l+1 < n && p[k+l+1] == ':' &&
            number(p+k+l+2,n-k-l-2,1,2,&sec,&m)) { hour=h; minute=mi; second=sec; }
        else if (day < 0 && number(p,n,1,2,&v,NULL)) day=v;
        else if (month < 0 && n >= 3) {
            for (int i = 0; i < 12; i++) if (!strncasecmp(p, months+i*3, 3)) { month=i+1; break; }
            if (month < 0 && year < 0 && number(p,n,2,4,&v,NULL)) year=v;
        } else if (year < 0 && number(p,n,2,4,&v,NULL)) year=v;
        p=end;
    }
    if (year >= 0 && year <= 69) year += 2000; else if (year >= 70 && year <= 99) year += 1900;
    if (year < 1601 || month < 1 || day < 1 || hour < 0 || hour > 23 || minute > 59 || second > 59) return false;
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
    if (day > days[month-1] + (month == 2 && leap)) return false;
    int64_t y = year-1;
    int64_t d = 365*(year-1970LL) + (y/4-1969/4) - (y/100-1969/100) + (y/400-1969/400);
    for (int i=1;i<month;i++) d += days[i-1] + (i == 2 && leap);
    *out = (d+day-1)*86400 + hour*3600 + minute*60 + second; return true;
}
static bool max_age(const char *p, int64_t now, int64_t *expiry) {
    bool neg = *p == '-'; if (neg) p++;
    if (!*p) return false;
    int64_t n = 0;
    for (; *p; p++) { if (!digit((unsigned char)*p)) return false; if (n < COOKIE_DAYS) n = n*10 + *p-'0'; }
    if (neg || !n) *expiry = 0;
    else *expiry = now + (n > COOKIE_DAYS ? COOKIE_DAYS : n);
    return true;
}
static struct cookie *make_cookie(const char *name, const char *value, const char *domain, const char *path) {
    size_t sizes[4] = {strlen(name), strlen(value), strlen(domain), strlen(path)}, total = 4;
    for (int i=0;i<4;i++) total += sizes[i];
    struct cookie *c = calloc(1, sizeof *c + total); if (!c) return NULL;
    char *dst=c->data; char **fields[4] = {&c->name,&c->value,&c->domain,&c->path};
    const char *src[4] = {name,value,domain,path};
    for (int i=0;i<4;i++) { *fields[i]=dst; memcpy(dst,src[i],sizes[i]+1); dst+=sizes[i]+1; }
    c->nl=sizes[0]; c->vl=sizes[1]; c->dl=sizes[2]; c->pl=sizes[3]; return c;
}
static struct cookie *copy_cookie(const struct cookie *from) {
    struct cookie *c = make_cookie(from->name, from->value, from->domain, from->path);
    if (!c) return NULL;
    c->flags = from->flags; c->same = from->same; c->expires = from->expires;
    c->created = from->created; c->access = from->access;
    return c;
}

/* The identity of a cookie (RFC 6265bis §5.7 step 23): name, domain, path, host-only. */
static bool same_key(const struct cookie *a, const struct cookie *b) {
    return a->nl == b->nl && a->dl == b->dl && a->pl == b->pl && !((a->flags ^ b->flags) & F_HOST) &&
           !memcmp(a->name, b->name, a->nl) && !memcmp(a->domain, b->domain, a->dl) && !memcmp(a->path, b->path, a->pl);
}
static uint64_t key_hash(const struct cookie *c) {
    uint64_t h = UINT64_C(1469598103934665603) ^ (c->flags & F_HOST);
    const char *parts[3] = {c->name, c->domain, c->path};
    size_t sizes[3] = {c->nl, c->dl, c->pl};
    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < sizes[k]; i++) { h ^= (unsigned char)parts[k][i]; h *= UINT64_C(1099511628211); }
        h ^= 0x100; h *= UINT64_C(1099511628211);
    }
    return h;
}
/* Positions of a list's cookies by key: a slot holds 0 (empty), TOMB, or the position + 1. It is
   made for at most limit insertions, so a probe always ends at an empty slot. */
#define TOMB SIZE_MAX
struct index { size_t *slots, mask; };
static bool index_make(struct index *x, size_t limit) {
    size_t size = 16;
    while (size < limit * 2 + 2) { if (size > SIZE_MAX / 4 / sizeof *x->slots) return false; size *= 2; }
    x->slots = calloc(size, sizeof *x->slots); x->mask = size - 1;
    return x->slots != NULL;
}
/* The slot holding c's key, or the one to put it in; *found says which. */
static size_t *index_slot(const struct index *x, struct cookie *const *list, const struct cookie *c, bool *found) {
    size_t *tomb = NULL;
    for (size_t i = (size_t)key_hash(c) & x->mask;; i = (i + 1) & x->mask) {
        size_t *s = &x->slots[i];
        if (!*s) { *found = false; return tomb ? tomb : s; }
        if (*s == TOMB) { if (!tomb) tomb = s; continue; }
        if (same_key(list[*s - 1], c)) { *found = true; return s; }
    }
}

/* The list of changes (tracking jars only). Each cookie's latest change replaces its earlier one
   wherever that was: applied in order, the result is the same. */
static void change_drop(webcookie_jar *j, size_t i) {
    free(j->changes[i].cookie);
    memmove(j->changes + i, j->changes + i + 1, (j->change_count - i - 1) * sizeof *j->changes);
    j->change_count--;
}
static struct change *change_add(webcookie_jar *j) {
    if (j->change_count == j->change_cap) {
        size_t n = j->change_cap ? j->change_cap * 2 : 16;
        struct change *grown = n <= SIZE_MAX / sizeof *grown ? realloc(j->changes, n * sizeof *grown) : NULL;
        if (!grown) { j->overflow = true; return NULL; }
        j->changes = grown; j->change_cap = n;
    }
    struct change *ch = &j->changes[j->change_count++];
    memset(ch, 0, sizeof *ch);
    return ch;
}
static void note(webcookie_jar *j, unsigned kind, const struct cookie *c) {
    if (!j->tracking) return;
    for (size_t i = 0; i < j->change_count;)
        if (j->changes[i].kind != CHANGE_CLEAR && same_key(j->changes[i].cookie, c)) change_drop(j, i);
        else i++;
    struct cookie *copy = copy_cookie(c);
    if (!copy) { j->overflow = true; return; }
    struct change *ch = change_add(j);
    if (!ch) { free(copy); return; }
    ch->kind = kind; ch->cookie = copy;
}
static void note_clear(webcookie_jar *j, const char *site) {
    if (!j->tracking) return;
    for (size_t i = 0; i < j->change_count;) {
        const struct change *ch = &j->changes[i];
        bool covered = !*site || (ch->kind == CHANGE_CLEAR ? *ch->site && domain_match(ch->site, site) : domain_match(ch->cookie->domain, site));
        if (covered) change_drop(j, i); else i++;
    }
    struct change *ch = change_add(j);
    if (ch) { ch->kind = CHANGE_CLEAR; strcpy(ch->site, site); }
}
/* A cookie leaves the jar: the store forgets it too. */
static void forget(webcookie_jar *j, const struct cookie *c) { if (c->saved) note(j, CHANGE_DELETE, c); }
static void evict(webcookie_jar *j, size_t i) { forget(j, j->cookies[i]); remove_cookie(j, i); }
/* §5.7: before c joins, a domain field at its bound loses its least recently used non-Secure
   cookie (then Secure), and a full jar its least recently used. */
static void make_room(webcookie_jar *j, const struct cookie *c) {
    size_t same = 0;
    for (size_t i = 0; i < j->count; i++) same += !strcmp(j->cookies[i]->domain, c->domain);
    while (same >= WEBCOOKIE_DOMAIN_MAX) {
        size_t victim = j->count;
        for (size_t i = 0; i < j->count; i++) {
            const struct cookie *x = j->cookies[i];
            if (strcmp(x->domain, c->domain)) continue;
            if (victim == j->count) { victim = i; continue; }
            const struct cookie *v = j->cookies[victim];
            bool xs = x->flags & F_SECURE, vs = v->flags & F_SECURE;
            if ((vs && !xs) || (xs == vs && x->access < v->access)) victim = i;
        }
        if (victim == j->count) break;
        evict(j, victim); same--;
    }
    while (j->count >= WEBCOOKIE_TOTAL_MAX) {
        size_t victim = 0;
        for (size_t i = 1; i < j->count; i++) if (j->cookies[i]->access < j->cookies[victim]->access) victim = i;
        evict(j, victim);
    }
}
/* The Domain attribute's value (§5.6.3: without one leading '.', lowercase) for the storage
   model to domain-match against the canonicalized request host: ASCII without controls, at
   most DOMAIN_MAX bytes. Only a suffix of the host the URL parser gave can match it. */
static bool domain_attribute(char *out, const char *v) {
    size_t n = strlen(v);
    if (!n || n > DOMAIN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c <= 32 || c >= 127) return false;
        out[i] = (char)lower(c);
    }
    out[n] = 0;
    return true;
}
static int set_for(webcookie_jar *j, const struct webcookie_context *ctx, const struct address *at, const char *input, size_t len, int64_t now);
int webcookie_set(webcookie_jar *j, const struct webcookie_context *ctx, const char *input, size_t len, int64_t now) {
    if (!j || !ctx || !input || now < 0 || now > NEVER-COOKIE_DAYS || len > WEBCOOKIE_FIELD_MAX) return -1;
    struct address a; int parsed = address(ctx->url, &a);
    if (parsed != 1) return parsed;
    int result = set_for(j, ctx, &a, input, len, now);
    address_free(&a);
    return result;
}
static int set_for(webcookie_jar *j, const struct webcookie_context *ctx, const struct address *at, const char *input, size_t len, int64_t now) {
    const struct address a = *at;
    /* A host past what a cookie's domain field holds (the URL parser bounds no domain). */
    if (strlen(a.host) > DOMAIN_MAX) return 0;
    if (policy(j, a.host) & WEBCOOKIE_BLOCK) return 0; /* the user's choice (§5.3, §7.2) */
    if (memchr(input,0,len) || !clean(input,len)) return 0;
    char *copy=malloc(len+1); if (!copy) return -1; memcpy(copy,input,len); copy[len]=0;
    char *attrs=strchr(copy,';'); if (attrs) *attrs++=0;
    char *eq=strchr(copy,'='), *name, *value;
    if (eq) { *eq=0; name=trim(copy); value=trim(eq+1); } else { name=""; value=trim(copy); }
    if ((!*name && !*value) || !name_ok(name) || strlen(name)+strlen(value)>4096) { free(copy); return 0; }
    char domain[DOMAIN_MAX+1], path[COOKIE_PATH_MAX+1], default_path[COOKIE_PATH_MAX+1]; strcpy(domain,a.host);
    size_t pl=a.path_len; while (pl>1 && a.path[pl-1]!='/') pl--; if (pl>1) pl--;
    if (pl > COOKIE_PATH_MAX) { free(copy); return 0; } memcpy(path,a.path,pl); path[pl]=0; strcpy(default_path,path);
    bool domain_attr=false, path_root=false, secure=false, http_only=false, age=false, persistent=false, bad=false, partitioned=false;
    unsigned same=S_LAX; int64_t expires=NEVER, age_time=NEVER;
    for (char *p=attrs;p;) {
        char *next=strchr(p,';'); if (next) *next++=0;
        char *v=strchr(p,'='); if (v) *v++=0; else v=p+strlen(p);
        p=trim(p); v=trim(v);
        if (!strcasecmp(p,"Domain")) {
            if (*v) { if (*v=='.') v++; domain_attr=true; if (!domain_attribute(domain,v)) bad=true; }
        } else if (!strcasecmp(p,"Path")) {
            size_t n=strlen(v); if (n<=COOKIE_PATH_MAX) { if (*v=='/') memcpy(path,v,n+1); else strcpy(path,default_path); path_root=n==1&&*v=='/'; }
        } else if (!strcasecmp(p,"Secure")) secure=true;
        else if (!strcasecmp(p,"HttpOnly")) http_only=true;
        else if (!strcasecmp(p,"Partitioned")) partitioned=true;
        else if (!strcasecmp(p,"SameSite")) same=!strcasecmp(v,"None") ? S_NONE : !strcasecmp(v,"Strict") ? S_STRICT : S_LAX;
        else if (!strcasecmp(p,"Max-Age")) { int64_t t; if (max_age(v,now,&t)) { age=true; age_time=t; } }
        else if (!strcasecmp(p,"Expires")) { int64_t t; if (cookie_date(v,&t)) { persistent=true; expires=t; } }
        p=next;
    }
    bool host_only=!domain_attr;
    if (domain_attr) {
        if (!domain_match(a.host,domain) || (a.ip && strcmp(a.host,domain))) bad=true;
        else if (!j->rule_count || a.ip || suffix_labels(j,domain)==labels(domain)) {
            if (strcmp(a.host,domain)) bad=true; else host_only=true;
        }
    }
    if (secure && !a.secure) bad=true;
    if (http_only && !ctx->http) bad=true;
    if (same==S_NONE && !secure) bad=true;
    if (same!=S_NONE && !same_context(j,ctx) && !(ctx->http && ctx->top_level)) bad=true;
    if (partitioned) bad=true; /* No partition key exists in this browser yet. */
    const char *prefix=*name ? name : value;
    if ((!strncasecmp(prefix,"__Secure-",9) || !strncasecmp(prefix,"__Host-",7)) && !secure) bad=true;
    if (!strncasecmp(prefix,"__Host-",7) && (domain_attr || !path_root)) bad=true;
    /* Also enforce the newer HTTP-only prefixes; never weaken them to JS cookies. */
    if ((!strncasecmp(prefix,"__Http-",7) || !strncasecmp(prefix,"__Host-Http-",12)) && (!secure || !http_only || !ctx->http)) bad=true;
    if (!*name && (!strncasecmp(value,"__Secure-",9) || !strncasecmp(value,"__Host-",7) || !strncasecmp(value,"__Http-",7))) bad=true;
    purge(j,now); size_t old=j->count;
    for (size_t i=0;i<j->count;i++) {
        struct cookie *c=j->cookies[i];
        if (strcmp(c->name,name)) continue;
        if (!a.secure && (c->flags&F_SECURE) && (domain_match(c->domain,domain) || domain_match(domain,c->domain)) && path_match(path,c->path)) bad=true;
        if (!strcmp(c->domain,domain) && !strcmp(c->path,path) && !!(c->flags&F_HOST)==host_only) {
            old=i; if (!ctx->http && (c->flags&F_HTTP)) bad=true;
        }
    }
    if (bad) { free(copy); return 0; }
    if (age) { expires=age_time; persistent=true; }
    if (persistent && expires>now+COOKIE_DAYS) expires=now+COOKIE_DAYS;
    if (expires<=now) { if (old<j->count) evict(j,old); free(copy); return 1; }
    struct cookie *c=make_cookie(name,value,domain,path); free(copy); if (!c) return -1;
    c->flags=(host_only?F_HOST:0)|(secure?F_SECURE:0)|(http_only?F_HTTP:0)|(persistent?F_PERSIST:0); c->same=same; c->expires=expires;
    c->created=old<j->count ? j->cookies[old]->created : stamp(j,now); c->access=stamp(j,now);
    bool was_saved = old<j->count && j->cookies[old]->saved;
    if (old<j->count) remove_cookie(j,old);
    make_room(j,c);
    if (!reserve(&j->cookies,&j->cap,j->count+1)) { if (was_saved) note(j,CHANGE_DELETE,c); free(c); return -1; }
    if (j->tracking && kept(j,c)) { c->saved=true; note(j,CHANGE_SET,c); }
    else if (was_saved) note(j,CHANGE_DELETE,c);
    j->cookies[j->count++]=c; j->bytes+=record_bytes(c); return 1;
}
static int send_order(const void *a,const void *b) {
    const struct cookie *x=*(const struct cookie *const *)a,*y=*(const struct cookie *const *)b;
    if (x->pl!=y->pl) return x->pl>y->pl?-1:1;
    return x->created<y->created?-1:x->created>y->created?1:0;
}
static long get_for(webcookie_jar *j,const struct webcookie_context *ctx,const struct address *at,char *out,size_t cap,int64_t now);
long webcookie_get(webcookie_jar *j,const struct webcookie_context *ctx,char *out,size_t cap,int64_t now) {
    if (out && cap) out[0]=0;
    struct address a; if (!j || !ctx || (out&&!cap) || address(ctx->url,&a)!=1) return -1;
    long result=get_for(j,ctx,&a,out,cap,now);
    address_free(&a);
    return result;
}
static long get_for(webcookie_jar *j,const struct webcookie_context *ctx,const struct address *at,char *out,size_t cap,int64_t now) {
    const struct address a=*at;
    purge(j,now);
    if (policy(j,a.host) & WEBCOOKIE_BLOCK) return 0; /* §5.8.1: the field is omitted */
    struct cookie **list=j->count?malloc(j->count*sizeof *list):NULL;
    if (j->count && !list) return -1;
    size_t count=0,len=0;
    bool same=same_context(j,ctx);
    const char *method=ctx->method?ctx->method:"GET";
    bool safe=!strcmp(method,"GET")||!strcmp(method,"HEAD")||!strcmp(method,"OPTIONS")||!strcmp(method,"TRACE");
    for (size_t i=0;i<j->count;i++) {
        struct cookie *c=j->cookies[i];
        if ((c->flags&F_HOST) ? strcmp(a.host,c->domain)!=0 : !domain_match(a.host,c->domain)) continue;
        if (!path_match_n(a.path,a.path_len,c->path) || ((c->flags&F_SECURE)&&!a.secure) || ((c->flags&F_HTTP)&&!ctx->http)) continue;
        if (!same && c->same!=S_NONE && !(c->same==S_LAX && ctx->http && ctx->top_level && safe)) continue;
        list[count++]=c; len+=c->nl+c->vl+(c->nl?1:0)+(count>1?2:0);
    }
    if (len>WEBCOOKIE_OUTPUT_MAX || (out && len>=cap)) { free(list); return -1; }
    if (!out) { free(list); return (long)len; }
    qsort(list,count,sizeof *list,send_order); size_t pos=0;
    for (size_t i=0;i<count;i++) {
        struct cookie *c=list[i]; if (i) { out[pos++]=';';out[pos++]=' '; }
        if (c->nl) { memcpy(out+pos,c->name,c->nl);pos+=c->nl;out[pos++]='='; }
        memcpy(out+pos,c->value,c->vl);pos+=c->vl;c->access=stamp(j,now);
    }
    free(list);
    out[pos]=0;return (long)pos;
}

static void put16(unsigned char *p,unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static unsigned get16(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void put32(unsigned char *p,uint32_t n){for(int i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8));}
static uint32_t get32(const unsigned char *p){uint32_t n=0;for(int i=0;i<4;i++)n|=(uint32_t)p[i]<<(i*8);return n;}
static void put64(unsigned char *p,uint64_t n){for(int i=0;i<8;i++)p[i]=(unsigned char)(n>>(i*8));}
static uint64_t get64(const unsigned char *p){uint64_t n=0;for(int i=0;i<8;i++)n|=(uint64_t)p[i]<<(i*8);return n;}
static unsigned char *put_record(unsigned char *p,const struct cookie *c){
    memset(p,0,RECORD_SIZE);put16(p,c->nl);put16(p+2,c->vl);put16(p+4,c->dl);put16(p+6,c->pl);
    p[8]=(unsigned char)c->flags;p[9]=(unsigned char)c->same;put64(p+16,(uint64_t)c->expires);put64(p+24,c->created);put64(p+32,c->access);p+=RECORD_SIZE;
    memcpy(p,c->name,c->nl);p+=c->nl;memcpy(p,c->value,c->vl);p+=c->vl;memcpy(p,c->domain,c->dl);p+=c->dl;memcpy(p,c->path,c->pl);return p+c->pl;
}
/* One record at *at: 1 with a new cookie in *out; 0 a record the policy refuses (skipped); -1 the
   bytes end inside it; -2 no memory. */
static int parse_record(const webcookie_jar *j,const unsigned char **at,const unsigned char *end,uint64_t serial,struct cookie **out){
    const unsigned char *p=*at;*out=NULL;
    if((size_t)(end-p)<RECORD_SIZE)return -1;
    unsigned nl=get16(p),vl=get16(p+2),dl=get16(p+4),pl=get16(p+6),flags=p[8],same=p[9];
    int64_t expires=(int64_t)get64(p+16);uint64_t created=get64(p+24),access=get64(p+32);p+=RECORD_SIZE;
    size_t size=(size_t)nl+vl+dl+pl;
    if(size>(size_t)(end-p))return -1;
    *at=p+size;
    if(nl+vl>4096||!dl||dl>DOMAIN_MAX||!pl||pl>COOKIE_PATH_MAX||flags>15||same>S_NONE||created>serial||access>serial||expires<0||memchr(p,0,size)||!clean((const char*)p,size))return 0;
    char fields[4096+DOMAIN_MAX+COOKIE_PATH_MAX+4],*f=fields;const char *parts[4];unsigned sizes[4]={nl,vl,dl,pl};
    for(int k=0;k<4;k++){parts[k]=f;memcpy(f,p,sizes[k]);f[sizes[k]]=0;p+=sizes[k];f+=sizes[k]+1;}
    const char *domain=parts[2];if(!name_ok(parts[0])||strchr(parts[1],';')||!web_url_host_serialized(domain,dl)||parts[3][0]!='/')return 0;
    if(!(flags&F_HOST)&&(!j->rule_count||suffix_labels(j,domain)==labels(domain)))return 0;
    struct cookie *c=make_cookie(parts[0],parts[1],parts[2],parts[3]);if(!c)return -2;
    c->flags=flags;c->same=same;c->expires=expires;c->created=created;c->access=access;*out=c;return 1;
}
long webcookie_export(webcookie_jar *j,void *output,size_t cap,int64_t now){
    if(!j)return -1;purge(j,now);size_t n=SNAP_HEADER+j->bytes;if(!output)return (long)n;if(cap<n)return -1;
    unsigned char *p=output;memset(p,0,SNAP_HEADER);memcpy(p,"NCK1",4);put16(p+4,(unsigned)j->count);put64(p+8,j->serial);p+=SNAP_HEADER;
    for(size_t i=0;i<j->count;i++)p=put_record(p,j->cookies[i]);
    return (long)n;
}
bool webcookie_import(webcookie_jar *j,const void *input,size_t len,int64_t now){
    if(!j||j->count||!input||len<SNAP_HEADER||len>WEBCOOKIE_SNAPSHOT_MAX)return false;
    const unsigned char *p=input,*end=p+len;if(memcmp(p,"NCK1",4)||get16(p+4)>WEBCOOKIE_TOTAL_MAX)return false;
    size_t count=get16(p+4);uint64_t serial=get64(p+8);p+=SNAP_HEADER;
    struct cookie **list=count?calloc(count,sizeof *list):NULL;struct index x={0};
    if((count&&!list)||!index_make(&x,count)){free(list);return false;}
    size_t n=0,bytes=0;bool ok=false;
    for(size_t i=0;i<count;i++){
        struct cookie *c;if(parse_record(j,&p,end,serial,&c)!=1)goto done;
        bool found;size_t *slot=index_slot(&x,list,c,&found);
        if(found){free(c);goto done;}
        list[n++]=c;*slot=n;bytes+=record_bytes(c);
    }
    if(p!=end)goto done;
    free(j->cookies);j->cookies=list;j->cap=count;j->count=n;j->bytes=bytes;j->serial=serial;list=NULL;n=0;purge(j,now);ok=true;
done:
    for(size_t i=0;i<n;i++)free(list[i]);
    free(list);free(x.slots);return ok;
}

/* The saved store: a header, the clears (oldest first), the cookies.
   header: "NCS1", 4 zero bytes, the writer's stamp (8), the cookie count (4), the clear count (4),
   the newest clear's serial (8), 16 zero bytes. A clear: its serial (8), the site's length (2),
   the site. A cookie: as in a snapshot. Persistent cookies only. */
static int domain_order(const void *a,const void *b){
    const struct cookie *x=*(const struct cookie *const *)a,*y=*(const struct cookie *const *)b;
    int c=strcmp(x->domain,y->domain);if(c)return c;
    unsigned xs=x->flags&F_SECURE,ys=y->flags&F_SECURE;if(xs!=ys)return xs<ys?-1:1;
    return x->access<y->access?-1:x->access>y->access?1:0;
}
static int access_order(const void *a,const void *b){
    const struct cookie *x=*(const struct cookie *const *)a,*y=*(const struct cookie *const *)b;
    return x->access<y->access?-1:x->access>y->access?1:0;
}
/* §5.7's bounds over a merged list (NULL entries already gone): per domain field the least
   recently used non-Secure cookies first, then Secure, then in all the least recently used. */
static void enforce(webcookie_jar *j,struct cookie **m,size_t *mn){
    size_t n=*mn;
    if(n<=WEBCOOKIE_DOMAIN_MAX&&n<=WEBCOOKIE_TOTAL_MAX)return;
    struct cookie **order=malloc(n*sizeof *order);if(!order)return;
    memcpy(order,m,n*sizeof *m);qsort(order,n,sizeof *order,domain_order);
    size_t left=n;
    for(size_t a=0;a<n;){
        size_t b=a;while(b<n&&!strcmp(order[b]->domain,order[a]->domain))b++;
        for(size_t k=a;b-a>WEBCOOKIE_DOMAIN_MAX&&k<b-WEBCOOKIE_DOMAIN_MAX;k++){order[k]->doomed=true;left--;}
        a=b;
    }
    if(left>WEBCOOKIE_TOTAL_MAX){
        size_t k=0;for(size_t i=0;i<n;i++)if(!m[i]->doomed)order[k++]=m[i];
        qsort(order,k,sizeof *order,access_order);
        for(size_t i=0;i<k-WEBCOOKIE_TOTAL_MAX;i++)order[i]->doomed=true;
    }
    free(order);
    size_t k=0;
    for(size_t i=0;i<n;i++){if(m[i]->doomed){forget(j,m[i]);free(m[i]);}else m[k++]=m[i];}
    *mn=k;
}
/* Removes from the merged list the cookie with c's key. */
static void merged_remove(struct index *x,struct cookie **m,const struct cookie *c){
    bool found;size_t *slot=index_slot(x,m,c,&found);
    if(found){free(m[*slot-1]);m[*slot-1]=NULL;*slot=TOMB;}
}
static bool merged_add(struct index *x,struct cookie ***m,size_t *mn,size_t *mcap,struct cookie *c){
    if(!reserve(m,mcap,*mn+1))return false;
    bool found;size_t *slot=index_slot(x,*m,c,&found);
    if(found){free((*m)[*slot-1]);(*m)[*slot-1]=c;return true;}
    (*m)[(*mn)++]=c;*slot=*mn;return true;
}
static void merged_clear(struct index *x,struct cookie **m,size_t mn,const char *site){
    for(size_t i=0;i<mn;i++)if(m[i]&&(!*site||domain_match(m[i]->domain,site))){
        bool found;size_t *slot=index_slot(x,m,m[i],&found);
        if(found)*slot=TOMB;
        free(m[i]);m[i]=NULL;
    }
}
/* A clear's site: none (every cookie), or a host as the URL parser writes one. */
static bool clear_site_ok(const char *site,size_t n){return !n||(n<=DOMAIN_MAX&&web_url_host_serialized(site,n));}
int webcookie_load(webcookie_jar *j,const void *data,size_t len,int64_t now){
    if(!j||(len&&!data))return 0;
    const unsigned char *p=data,*end=p+len;
    size_t ncookies=0,nclears=0;uint64_t serial=0,top=0;
    if(len){
        if(len<STORE_HEADER||memcmp(p,"NCS1",4))return 0;
        serial=get64(p+8);ncookies=get32(p+16);nclears=get32(p+20);top=get64(p+24);
        if(nclears>WEBCOOKIE_CLEARS_KEPT||ncookies>(len-STORE_HEADER)/RECORD_SIZE)return 0;
        p+=STORE_HEADER;
    }
    struct clear *clears=nclears?calloc(nclears,sizeof *clears):NULL;
    /* Every insertion below: the store's cookies, the changes, and the jar's own twice at most. */
    size_t limit=ncookies+j->change_count+2*j->count+1,mn=0,mcap=0;
    struct cookie **m=NULL;struct index x={0};int result=-1; /* -1 no memory, 0 malformed */
    if((nclears&&!clears)||!index_make(&x,limit))goto fail;
    for(size_t k=0;k<nclears;k++){
        if((size_t)(end-p)<10)goto bad;
        uint64_t s=get64(p);unsigned n=get16(p+8);p+=10;
        if(n>DOMAIN_MAX||n>(size_t)(end-p)||s>top||!clear_site_ok((const char*)p,n)||(k&&s<=clears[k-1].serial))goto bad;
        clears[k].serial=s;memcpy(clears[k].site,p,n);clears[k].site[n]=0;p+=n;
    }
    for(size_t k=0;k<ncookies;k++){
        struct cookie *c;int r=parse_record(j,&p,end,serial,&c);
        if(r==-2)goto fail;
        if(r<0)goto bad;
        if(!r)continue;
        if(!(c->flags&F_PERSIST)||c->expires<=now){free(c);continue;}
        bool found;index_slot(&x,m,c,&found);
        if(found||!merged_add(&x,&m,&mn,&mcap,c)){free(c);if(found)continue;goto fail;}
        c->saved=true;
    }
    if(p!=end)goto bad;
    /* The jar's unsaved changes, in order, on top. */
    for(size_t i=0;i<j->change_count;i++){
        const struct change *ch=&j->changes[i];
        if(ch->kind==CHANGE_CLEAR){merged_clear(&x,m,mn,ch->site);continue;}
        merged_remove(&x,m,ch->cookie);
        if(ch->kind!=CHANGE_SET||ch->cookie->expires<=now)continue;
        struct cookie *c=copy_cookie(ch->cookie);
        if(!c||!merged_add(&x,&m,&mn,&mcap,c)){free(c);goto fail;}
        c->saved=true;
    }
    if(j->overflow)for(size_t i=0;i<j->count;i++){
        const struct cookie *mine=j->cookies[i];
        if(!kept(j,mine)||mine->expires<=now)continue;
        struct cookie *c=copy_cookie(mine);
        if(!c||!merged_add(&x,&m,&mn,&mcap,c)){free(c);goto fail;}
        c->saved=true;
    }
    if(!reserve(&m,&mcap,mn+j->count))goto fail;
    /* What the store holds replaces the jar's saved cookies (keeping the later use of the same
       cookie); the jar's session and transient cookies stay, unless a clear recorded since the
       jar last looked or the store's same cookie takes their place. */
    bool *moved=j->count?calloc(j->count,sizeof *moved):NULL;
    if(j->count&&!moved)goto fail;
    for(size_t i=0;i<j->count;i++){
        struct cookie *mine=j->cookies[i];bool found;size_t *slot=index_slot(&x,m,mine,&found);
        if(mine->saved){
            if(found&&m[*slot-1]->created==mine->created&&m[*slot-1]->access<mine->access)m[*slot-1]->access=mine->access;
            continue;
        }
        if(found||mine->expires<=now)continue;
        bool cleared=false;
        for(size_t k=0;k<nclears&&!cleared;k++)cleared=clears[k].serial>j->clear_seen&&(!*clears[k].site||domain_match(mine->domain,clears[k].site));
        if(cleared)continue;
        m[mn++]=mine;*slot=mn;moved[i]=true;
    }
    size_t k=0;for(size_t i=0;i<mn;i++)if(m[i])m[k++]=m[i];mn=k;
    enforce(j,m,&mn);
    for(size_t i=0;i<j->count;i++)if(!moved[i])free(j->cookies[i]);
    free(moved);free(j->cookies);free(x.slots);
    j->cookies=m;j->count=mn;j->cap=mcap;j->bytes=0;
    if(serial>j->serial)j->serial=serial;
    for(size_t i=0;i<mn;i++){
        j->bytes+=record_bytes(m[i]);
        if(m[i]->created>j->serial)j->serial=m[i]->created;
        if(m[i]->access>j->serial)j->serial=m[i]->access;
    }
    free(j->clears);j->clears=clears;j->clear_count=nclears;j->clear_top=top;
    if(top>j->clear_seen)j->clear_seen=top;
    return 1;
bad:
    result=0;
fail:
    for(size_t i=0;i<mn;i++)free(m[i]);
    free(m);free(x.slots);free(clears);return result;
}
/* The clears the store will hold: the newest WEBCOOKIE_CLEARS_KEPT of the store's and the jar's
   pending ones (serials after the store's newest). Their count and the newest serial. */
static size_t pending_clears(const webcookie_jar *j){size_t n=0;for(size_t i=0;i<j->change_count;i++)n+=j->changes[i].kind==CHANGE_CLEAR;return n;}
long webcookie_save(webcookie_jar *j,void *output,size_t cap,int64_t now){
    if(!j||!j->tracking)return -1;
    size_t pending=pending_clears(j),total=j->clear_count+pending,skip=total>WEBCOOKIE_CLEARS_KEPT?total-WEBCOOKIE_CLEARS_KEPT:0;
    size_t n=STORE_HEADER,count=0;
    for(size_t i=skip;i<j->clear_count;i++)n+=10+strlen(j->clears[i].site);
    size_t seen=0;
    for(size_t i=0;i<j->change_count;i++)if(j->changes[i].kind==CHANGE_CLEAR&&j->clear_count+seen++>=skip)n+=10+strlen(j->changes[i].site);
    for(size_t i=0;i<j->count;i++)if(kept(j,j->cookies[i])&&j->cookies[i]->expires>now){n+=record_bytes(j->cookies[i]);count++;}
    if(!output)return n>(size_t)INT32_MAX?-1:(long)n;
    if(cap<n)return -1;
    unsigned char *p=output;memset(p,0,STORE_HEADER);memcpy(p,"NCS1",4);put64(p+8,j->serial);put32(p+16,(uint32_t)count);
    put32(p+20,(uint32_t)(total-skip));put64(p+24,j->clear_top+pending);p+=STORE_HEADER;
    uint64_t serial=j->clear_top;
    for(size_t i=skip;i<j->clear_count;i++){size_t l=strlen(j->clears[i].site);put64(p,j->clears[i].serial);put16(p+8,(unsigned)l);memcpy(p+10,j->clears[i].site,l);p+=10+l;}
    seen=0;
    for(size_t i=0;i<j->change_count;i++){
        if(j->changes[i].kind!=CHANGE_CLEAR)continue;
        serial++;
        if(j->clear_count+seen++<skip)continue;
        size_t l=strlen(j->changes[i].site);put64(p,serial);put16(p+8,(unsigned)l);memcpy(p+10,j->changes[i].site,l);p+=10+l;
    }
    for(size_t i=0;i<j->count;i++)if(kept(j,j->cookies[i])&&j->cookies[i]->expires>now)p=put_record(p,j->cookies[i]);
    return (long)n;
}
void webcookie_saved(webcookie_jar *j){
    if(!j||!j->tracking)return;
    size_t pending=pending_clears(j),total=j->clear_count+pending,skip=total>WEBCOOKIE_CLEARS_KEPT?total-WEBCOOKIE_CLEARS_KEPT:0;
    struct clear *clears=total-skip?calloc(total-skip,sizeof *clears):NULL;
    if(clears){
        size_t k=0,at=0;uint64_t serial=j->clear_top;
        for(size_t i=0;i<j->clear_count;i++,at++)if(at>=skip)clears[k++]=j->clears[i];
        for(size_t i=0;i<j->change_count;i++){
            if(j->changes[i].kind!=CHANGE_CLEAR)continue;
            serial++;
            if(at++>=skip){clears[k].serial=serial;strcpy(clears[k].site,j->changes[i].site);k++;}
        }
        free(j->clears);j->clears=clears;j->clear_count=k;
    }
    j->clear_top+=pending;
    if(j->clear_top>j->clear_seen)j->clear_seen=j->clear_top;
    changes_free(j);j->overflow=false;
    for(size_t i=0;i<j->count;i++)j->cookies[i]->saved=kept(j,j->cookies[i]);
}
bool webcookie_changed(const webcookie_jar *j){return j&&(j->change_count||j->overflow);}
void webcookie_rule_changed(webcookie_jar *j){
    if(!j||!j->tracking)return;
    for(size_t i=0;i<j->count;i++){
        struct cookie *c=j->cookies[i];bool k=kept(j,c);
        if(k&&!c->saved){c->saved=true;note(j,CHANGE_SET,c);}
        else if(!k&&c->saved){note(j,CHANGE_DELETE,c);c->saved=false;}
    }
}
void webcookie_session_end(webcookie_jar *j){
    if(!j)return;
    for(size_t i=0;i<j->count;)if(!kept(j,j->cookies[i]))evict(j,i);else i++;
}
struct site_entry{const char *site;const struct cookie *c;};
static int site_order(const void *a,const void *b){return strcmp(((const struct site_entry*)a)->site,((const struct site_entry*)b)->site);}
long webcookie_sites(webcookie_jar *j,struct webcookie_site **out,int64_t now){
    if(!out)return -1;
    *out=NULL;if(!j)return -1;
    purge(j,now);if(!j->count)return 0;
    struct site_entry *e=malloc(j->count*sizeof *e);struct webcookie_site *s=calloc(j->count,sizeof *s);
    if(!e||!s){free(e);free(s);return -1;}
    for(size_t i=0;i<j->count;i++){e[i].c=j->cookies[i];e[i].site=webcookie_site_of(j,j->cookies[i]->domain);}
    qsort(e,j->count,sizeof *e,site_order);
    size_t n=0;
    for(size_t i=0;i<j->count;i++){
        if(!n||strcmp(s[n-1].site,e[i].site))snprintf(s[n++].site,sizeof s->site,"%s",e[i].site);
        s[n-1].cookies++;s[n-1].bytes+=e[i].c->nl+e[i].c->vl+e[i].c->dl+e[i].c->pl;
    }
    free(e);*out=s;return (long)n;
}
size_t webcookie_clear(webcookie_jar *j,const char *site){
    if(!j)return 0;
    char host[DOMAIN_MAX+1]="";
    if(site&&*site){
        size_t n=strlen(site);if(n>DOMAIN_MAX)return 0;
        for(size_t i=0;i<=n;i++)host[i]=(char)lower((unsigned char)site[i]);
        if(!clear_site_ok(host,n))return 0;
    }
    size_t removed=0;
    for(size_t i=0;i<j->count;)if(!*host||domain_match(j->cookies[i]->domain,host)){remove_cookie(j,i);removed++;}else i++;
    note_clear(j,host);
    return removed;
}
