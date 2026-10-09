/* One browser request per process. DNS, TCP and verified TLS never run on the GUI task. */
#include "nocturne.h"
#include "http.h"
#include "web.h"
#include "webnet_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

struct job {
    struct webnet_wire_request wire;
    char *url, *document, *method, *headers, *request_body;
    char origin[160], final_url[WEBNET_URL_MAX], error[160];
    /* User processes have a 64 KiB stack. Keep bounded URL/header work on the
       already heap-owned job; do not grow six local arrays on every hop. */
    char current[WEBNET_URL_MAX], document_url[WEBNET_URL_MAX];
    char location[WEBNET_URL_MAX], resolved[WEBNET_URL_MAX];
    char decoded[WEBNET_URL_MAX], cookie_url[WEBNET_URL_MAX];
    char path[WEBNET_URL_MAX], document_path[WEBNET_URL_MAX];
    char outgoing[WEBNET_REQUEST_HEADERS_MAX + WEBCOOKIE_OUTPUT_MAX + 288];
    char *response_headers;
    size_t response_headers_len;
    char *body;
    size_t body_len, body_cap;
    int status;
    webcookie_jar *cookies;
    unsigned char *cookie_events;
    size_t cookie_len, cookie_cap;
    bool hop_cookies, redirect_cross_site;
    bool cors_tainted, origin_tainted, redirected;
};
static bool read_all(int fd, void *p, size_t n) {
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p = (char *)p + k; n -= k;
    }
    return true;
}
static bool write_all(int fd, const void *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p = (const char *)p + k; n -= k;
    }
    return true;
}
static char *read_string(uint32_t len, bool binary) {
    char *p = malloc((size_t)len + 1);
    if (!p) return NULL;
    if (!read_all(0, p, len) || (!binary && memchr(p, 0, len))) { free(p); return NULL; }
    p[len] = 0; return p;
}
static bool fail(struct job *j, const char *message) {
    snprintf(j->error, sizeof j->error, "%s", message); return false;
}
static int remaining(struct job *j) {
    uint64_t now = uptime_ms();
    if (now >= j->wire.deadline) { fail(j, "Request deadline exceeded"); return 0; }
    uint64_t left = j->wire.deadline - now;
    return (int)MIN(left, WEBNET_TIMEOUT_MS);
}
static bool token_char(unsigned char c) {
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
/* Validate before calling the permissive underlying URL parser; never allow truncation. */
static bool http_url(const char *raw, char *canonical, size_t cap, char *origin, size_t origin_cap) {
    size_t len = strlen(raw);
    if (!len || len >= WEBNET_URL_MAX) return false;
    for (size_t i = 0; i < len; i++) if ((unsigned char)raw[i] <= 32 || raw[i] == '\\' || (unsigned char)raw[i] >= 127) return false;
    bool tls;
    const char *authority;
    if (!strncasecmp(raw, "https://", 8)) { tls = true; authority = raw + 8; }
    else if (!strncasecmp(raw, "http://", 7)) { tls = false; authority = raw + 7; }
    else return false;
    size_t al = strcspn(authority, "/?#");
    if (!al || al >= 140 || memchr(authority, '@', al)) return false;
    const char *colon = memchr(authority, ':', al);
    size_t hl = colon ? (size_t)(colon - authority) : al;
    if (!hl || hl >= 128) return false;
    char host[128];
    for (size_t i = 0; i < hl; i++) {
        unsigned char c = authority[i];
        if (!isalnum(c) && c != '-' && c != '.') return false;
        host[i] = (char)tolower(c);
    }
    host[hl] = 0;
    unsigned port = tls ? 443 : 80;
    if (colon) {
        const char *p = colon + 1, *end = authority + al;
        if (p == end) return false;
        port = 0;
        for (; p < end; p++) {
            if (!isdigit((unsigned char)*p)) return false;
            port = port * 10 + (unsigned)(*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    char normalized_origin[160];
    int n = port == (tls ? 443u : 80u)
        ? snprintf(normalized_origin, sizeof normalized_origin, "%s://%s", tls ? "https" : "http", host)
        : snprintf(normalized_origin, sizeof normalized_origin, "%s://%s:%u", tls ? "https" : "http", host, port);
    if (n < 0 || (size_t)n >= sizeof normalized_origin) return false;
    const char *path = authority + al;
    size_t pl = strcspn(path, "#");
    if (pl >= HTTP_URL_PATH_MAX || (pl && *path != '/' && *path != '?')) return false;
    size_t ol = strlen(normalized_origin);
    size_t extra = !pl || *path == '?' ? 1 : 0;
    if (ol + extra + pl >= cap || ol >= origin_cap) return false;
    memcpy(canonical, normalized_origin, ol);
    size_t at = ol;
    if (extra) canonical[at++] = '/';
    memcpy(canonical + at, path, pl); canonical[at + pl] = 0;
    if (origin) memcpy(origin, normalized_origin, ol + 1);
    return true;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool file_path(const char *url, char *out, size_t cap, char *decoded) {
    if (strncmp(url, "file:///", 8)) return false;
    const char *p = url + 7;
    size_t n = 0;
    while (*p && *p != '?' && *p != '#') {
        unsigned char c = (unsigned char)*p++;
        if (c == '%') {
            int a = hex((unsigned char)p[0]), b = p[0] ? hex((unsigned char)p[1]) : -1;
            if (a < 0 || b < 0) return false;
            c = (unsigned char)((a << 4) | b); p += 2;
        }
        if (!c || c < 32 || c == 127 || c == '\\' || n + 1 >= WEBNET_URL_MAX) return false;
        decoded[n++] = (char)c;
    }
    decoded[n] = 0;
    size_t at = 1; out[0] = '/';
    char *s = decoded;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char *end = strchr(s, '/');
        size_t l = end ? (size_t)(end - s) : strlen(s);
        if (l == 1 && s[0] == '.') { /* no-op */ }
        else if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (at == 1) return false;
            while (at > 1 && out[at - 1] != '/') at--;
            if (at > 1) at--;
        } else {
            if (at > 1) { if (at + 1 >= cap) return false; out[at++] = '/'; }
            if (at + l >= cap) return false;
            memcpy(out + at, s, l); at += l;
        }
        s += l;
    }
    out[at] = 0; return true;
}
static bool load_file(struct job *j) {
    if (j->wire.kind == WEBNET_FETCH) return fail(j, "fetch accepts only HTTP(S)");
    char *path = j->path, *document_path = j->document_path;
    if (!file_path(j->url, path, sizeof j->path, j->decoded)) return fail(j, "Invalid local URL");
    if (j->wire.kind == WEBNET_NAVIGATION) {
        if (!(j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION)) return fail(j, "Only a user navigation may open a local file");
    } else {
        if (!file_path(j->document, document_path, sizeof j->document_path, j->decoded)) return fail(j, "Remote pages cannot open local files");
        char *slash = strrchr(document_path, '/');
        if (!slash) return fail(j, "Invalid local document URL");
        size_t dl = (size_t)(slash - document_path) + 1;
        if (strncmp(path, document_path, dl)) return fail(j, "Local resource escapes document directory");
    }
    if (strcmp(j->method, "GET") || j->wire.body_len) return fail(j, "Local resources support GET only");
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail(j, "Cannot open local resource");
    struct n_stat st;
    if (fstat(fd, &st) < 0 || st.type != N_FT_FILE || st.size > webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation)) {
        close(fd); return fail(j, "Invalid or oversized local resource");
    }
    j->body = malloc((size_t)st.size + 1);
    if (!j->body) { close(fd); return fail(j, "Out of memory"); }
    size_t left = (size_t)st.size;
    while (left) {
        if (!remaining(j)) { close(fd); return false; }
        ssize_t k = read(fd, j->body + j->body_len, MIN(left, 65536));
        if (k <= 0) { close(fd); return fail(j, "Local resource ended early"); }
        j->body_len += k; left -= k;
    }
    close(fd); j->body[j->body_len] = 0; j->status = 200;
    snprintf(j->final_url, sizeof j->final_url, "%s", j->url);
    return true;
}
static bool cors_kind(const struct job *j) { return j->wire.kind == WEBNET_FETCH || j->wire.kind == WEBNET_MODULE; }
/* Redirect-taint changes the serialized Origin, not the document origin used
   for same-origin mode and mixed-content checks. It can never be cleared. */
static const char *request_origin(const struct job *j) { return j->origin_tainted ? "null" : j->origin; }
static bool forbidden_header(const char *name) {
    static const char *const forbidden[] = {"host", "origin", "referer", "cookie", "cookie2",
        "proxy-authorization", "connection", "content-length", "transfer-encoding", "accept-encoding", "te", "trailer",
        "upgrade", "expect", "user-agent", "via", "date", "dnt", "permissions-policy", "access-control-request-method",
        "access-control-request-headers", "accept-charset", "keep-alive", NULL};
    if (!strncasecmp(name, "sec-", 4) || !strncasecmp(name, "proxy-", 6)) return true;
    for (int i = 0; forbidden[i]; i++) if (!strcasecmp(name, forbidden[i])) return true;
    return false;
}
static bool unsafe_value(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 32 && c != 9) || c == 127 || strchr("\"():<>?@[\\]{}", c)) return true;
    }
    return false;
}
static bool safelisted(const char *name, const char *value) {
    if (strlen(value) > 128) return false;
    if (!strcasecmp(name, "accept")) return !unsafe_value(value);
    if (!strcasecmp(name, "accept-language") || !strcasecmp(name, "content-language")) {
        for (const char *s = value; *s; s++) if (!isalnum((unsigned char)*s) && !strchr(" *,-.;=", *s)) return false;
        return true;
    }
    if (!strcasecmp(name, "content-type")) {
        if (unsafe_value(value)) return false;
        size_t l = strcspn(value, ";");
        while (l && value[l - 1] == ' ') l--;
        return (l == 33 && !strncasecmp(value, "application/x-www-form-urlencoded", l)) ||
               (l == 19 && !strncasecmp(value, "multipart/form-data", l)) ||
               (l == 10 && !strncasecmp(value, "text/plain", l));
    }
    return false;
}
static bool list_has_slice(const char *list, const char *limit, const char *token, size_t tl, bool insensitive) {
    for (const char *p = list; p < limit;) {
        while (p < limit && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        const char *e = memchr(p, ',', (size_t)(limit - p)); if (!e) e = limit;
        const char *trim = e; while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        if ((size_t)(trim - p) == tl && (insensitive ? !strncasecmp(p, token, tl) : !strncmp(p, token, tl))) return true;
        p = e < limit ? e + 1 : e;
    }
    return false;
}
static bool list_has(const char *list, const char *token, bool insensitive) {
    return list_has_slice(list, list + strlen(list), token, strlen(token), insensitive);
}
static bool field_name_is(const char *name, size_t len, const char *expected) {
    return len == strlen(expected) && !strncasecmp(name, expected, len);
}
static bool field_exists(const char *headers, const char *name) {
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && field_name_is(p, (size_t)(colon - p), name)) return true;
        p = *end ? end + 2 : end;
    }
    return false;
}
/* Strict response field lookup: security-critical singleton fields cannot be duplicated. */
static bool field(const char *headers, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name); bool found = false;
    out[0] = 0;
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            if (found) { out[0] = 0; return false; }
            const char *v = p + nl + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
            const char *ve = end; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            size_t l = ve - v; if (l >= cap) return false;
            memcpy(out, v, l); out[l] = 0; found = true;
        }
        p = *end ? end + 2 : end;
    }
    return found;
}
static bool validate_headers(struct job *j, char *non_simple, size_t cap) {
    non_simple[0] = 0; size_t used = 0;
    char seen[WEBNET_REQUEST_HEADERS_MAX] = ""; size_t seen_len = 0;
    const char *p = j->headers;
    while (*p) {
        const char *end = strstr(p, "\r\n");
        if (!end) return fail(j, "Request headers must end in CRLF");
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || colon == p || (size_t)(colon - p) >= 128) return fail(j, "Invalid request header name");
        char name[128], value[WEBNET_REQUEST_HEADERS_MAX];
        size_t nl = colon - p;
        for (size_t i = 0; i < nl; i++) {
            if (!token_char((unsigned char)p[i])) return fail(j, "Invalid request header name");
            name[i] = (char)tolower((unsigned char)p[i]);
        }
        name[nl] = 0;
        if (forbidden_header(name)) return fail(j, "Prohibited request header");
        if (list_has(seen, name, true)) return fail(j, "Duplicate request header");
        if (seen_len + nl + (seen_len ? 1 : 0) >= sizeof seen) return fail(j, "Too many request headers");
        if (seen_len) seen[seen_len++] = ',';
        memcpy(seen + seen_len, name, nl + 1); seen_len += nl;
        const char *v = colon + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
        size_t vl = end - v;
        if (vl >= sizeof value) return fail(j, "Request header too large");
        for (size_t i = 0; i < vl; i++) if (((unsigned char)v[i] < 32 && v[i] != '\t') || (unsigned char)v[i] == 127) return fail(j, "Invalid request header value");
        memcpy(value, v, vl); value[vl] = 0;
        if (!safelisted(name, value) && !list_has(non_simple, name, true)) {
            if (used + nl + (used ? 2 : 0) >= cap) return fail(j, "Too many request header names");
            if (used) { memcpy(non_simple + used, ", ", 2); used += 2; }
            memcpy(non_simple + used, name, nl + 1); used += nl;
        }
        p = end + 2;
    }
    return true;
}
/* Fetch HTTP-network-or-cache steps 16-18 (WHATWG, consulted 2026-10-07).
   This transport has no HTTP response cache: default/force-cache/no-cache
   have genuine cache-miss network semantics, reload/no-store bypass storage,
   and only-if-cached fails before network/preflight. Generate these fields
   after classifying AUTHOR headers for CORS, independently on every hop. */
static bool cache_headers(struct job *j, char *headers, size_t cap, int *used) {
    unsigned mode = WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation);
    if (mode == WEBNET_CACHE_DEFAULT &&
        (field_exists(j->headers, "If-Modified-Since") || field_exists(j->headers, "If-None-Match") ||
         field_exists(j->headers, "If-Unmodified-Since") || field_exists(j->headers, "If-Match") ||
         field_exists(j->headers, "If-Range"))) mode = WEBNET_CACHE_NO_STORE;
    const char *pragma = "", *control = "";
    if (mode == WEBNET_CACHE_NO_STORE || mode == WEBNET_CACHE_RELOAD) {
        if (!field_exists(j->headers, "Pragma")) pragma = "Pragma: no-cache\r\n";
        if (!field_exists(j->headers, "Cache-Control")) control = "Cache-Control: no-cache\r\n";
    } else if (mode == WEBNET_CACHE_NO_CACHE && !field_exists(j->headers, "Cache-Control"))
        control = "Cache-Control: max-age=0\r\n";
    int n = snprintf(headers + *used, cap - (size_t)*used, "%s%s", pragma, control);
    if (n < 0 || (size_t)n >= cap - (size_t)*used) return fail(j, "Cache request headers exceed limit");
    *used += n;
    return true;
}
/* Headers have already been validated. Rewrites can only shorten the owned buffer. */
static void remove_request_header(char *headers, const char *name) {
    size_t nl = strlen(name);
    char *src = headers, *dst = headers;
    while (*src) {
        char *end = strstr(src, "\r\n");
        if (!end) break; /* Unreachable for validated request headers. */
        size_t len = (size_t)(end - src) + 2;
        bool remove = (size_t)(end - src) > nl && src[nl] == ':' && !strncasecmp(src, name, nl);
        if (!remove) { memmove(dst, src, len); dst += len; }
        src = end + 2;
    }
    *dst = 0;
}
static void remove_body_headers(char *headers) {
    remove_request_header(headers, "Content-Encoding");
    remove_request_header(headers, "Content-Language");
    remove_request_header(headers, "Content-Location");
    remove_request_header(headers, "Content-Type");
}
static bool cors_allowed(struct job *j, const struct http_resp *r) {
    char allow[160]; /* serialized supported origin is <=159 bytes */
    const char *headers = http_response_headers(r);
    if (!field(headers, "Access-Control-Allow-Origin", allow, sizeof allow) ||
        (strcmp(allow, "*") && strcmp(allow, request_origin(j)))) return fail(j, "Cross-origin response is not allowed");
    if (j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE) {
        char credentials[16];
        if (!strcmp(allow,"*") || !field(headers,"Access-Control-Allow-Credentials",credentials,sizeof credentials) ||
            strcmp(credentials,"true")) return fail(j,"Credentialed CORS requires exact origin and credentials permission");
    }
    return true;
}
static int cookie_header(void *opaque, const char *line, size_t length) {
    struct job *j = opaque;
    if (!j->hop_cookies || length < 11 || strncasecmp(line,"Set-Cookie:",11)) return 0;
    const char *value = line+11, *end=line+length;
    while (value<end && (*value==' ' || *value=='\t')) value++;
    size_t vl=(size_t)(end-value), ul=strlen(j->final_url), need=sizeof(struct webnet_wire_cookie)+ul+vl;
    if (vl>WEBCOOKIE_FIELD_MAX || need>WEBNET_COOKIE_EVENTS_MAX-j->cookie_len) {
        fail(j,"Cookie response fields exceed bounded storage"); return -1;
    }
    if (j->cookie_len+need>j->cookie_cap) {
        size_t cap=j->cookie_cap?j->cookie_cap:8192;
        while(cap<j->cookie_len+need)cap*=2;
        unsigned char *p=realloc(j->cookie_events,cap);
        if(!p){fail(j,"Out of memory receiving cookies");return -1;}
        j->cookie_events=p;j->cookie_cap=cap;
    }
    struct webnet_wire_cookie e={(uint32_t)ul,(uint32_t)vl,j->redirect_cross_site,0,time(NULL)};
    unsigned char *p=j->cookie_events+j->cookie_len;memcpy(p,&e,sizeof e);p+=sizeof e;
    memcpy(p,j->final_url,ul);memcpy(p+ul,value,vl);j->cookie_len+=need;return 0;
}
static bool apply_cookie_events(struct job *j,size_t start,const char *method) {
    if(start==j->cookie_len)return true;
    const unsigned char *p=j->cookie_events+start,*end=j->cookie_events+j->cookie_len;
    while(p<end){
        struct webnet_wire_cookie e;memcpy(&e,p,sizeof e);p+=sizeof e;
        char *url=j->cookie_url;memcpy(url,p,e.url_len);url[e.url_len]=0;p+=e.url_len;
        struct webcookie_context c={url,*j->document?j->document:j->wire.kind==WEBNET_NAVIGATION?NULL:"",method,
            j->wire.kind==WEBNET_NAVIGATION && (j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION),true,e.redirect_cross_site!=0};
        if(webcookie_set(j->cookies,&c,(const char*)p,e.value_len,e.received)<0)return fail(j,"Out of memory applying cookies");
        p+=e.value_len;
    }
    return true;
}
static int body_cb(void *opaque, const char *data, size_t n) {
    struct job *j = opaque;
    if (!remaining(j)) return -1;
    size_t limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
    if (j->body_len > limit || n > limit - j->body_len) {
        fail(j, limit == WEBNET_BODY_LIMIT ? "Response exceeds 16 MiB" : "Script response exceeds 32 MiB"); return -1;
    }
    size_t need = j->body_len + n + 1;
    if (need > j->body_cap) {
        size_t cap = j->body_cap ? j->body_cap : 8192;
        while (cap < need) cap = MIN(cap * 2, limit + 1);
        char *p = realloc(j->body, cap);
        if (!p) { fail(j, "Out of memory receiving response"); return -1; }
        j->body = p; j->body_cap = cap;
    }
    memcpy(j->body + j->body_len, data, n); j->body_len += n; j->body[j->body_len] = 0;
    return 0;
}
static int discard_cb(void *opaque, const char *data, size_t n) {
    (void)data;
    struct job *j = opaque;
    if (!remaining(j)) return -1;
    if (n > WEBNET_BODY_LIMIT - j->body_len) { fail(j, "Preflight response exceeds 16 MiB"); return -1; }
    j->body_len += n; return 0;
}
static bool preflight(struct job *j, const char *url, const char *method, const char *names) {
    char head[WEBNET_REQUEST_HEADERS_MAX];
    int n = snprintf(head, sizeof head, "Origin: %s\r\nAccess-Control-Request-Method: %s\r\n", request_origin(j), method);
    if (*names) n += snprintf(head + n, sizeof head - (size_t)n, "Access-Control-Request-Headers: %s\r\n", names);
    if (n < 0 || (size_t)n >= sizeof head) return fail(j, "Preflight headers too large");
    int left = remaining(j); if (!left) return false;
    struct http_req q = {"OPTIONS", url, head, NULL, 0, left, discard_cb, j, NULL};
    struct http_resp r;
    j->body_len = 0;
    int result = http_request(&q, &r);
    j->body_len = 0;
    if (result < 0) { if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
    if (r.status < 200 || r.status >= 300) { http_resp_free(&r); return fail(j, "CORS preflight failed"); }
    if (!cors_allowed(j, &r)) { http_resp_free(&r); return false; }
    char methods[1024], allowed[4096];
    /* Safelisted methods do not need an Allow-Methods entry. XHR upload
       listeners can force OPTIONS even for a simple GET/POST request. */
    bool ok = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST") ||
              (field(http_response_headers(&r), "Access-Control-Allow-Methods", methods, sizeof methods) &&
               (list_has(methods, method, false) || (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && list_has(methods, "*", false))));
    if (*names) {
        ok = ok && field(http_response_headers(&r), "Access-Control-Allow-Headers", allowed, sizeof allowed);
        for (const char *p = names; ok && *p;) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = strchr(p, ','); if (!e) e = p + strlen(p);
            size_t l = e - p; while (l && p[l - 1] == ' ') l--;
            char name[128]; if (l >= sizeof name) { ok = false; break; }
            memcpy(name, p, l); name[l] = 0;
            /* Authorization is never covered by the CORS header wildcard. */
            ok = list_has(allowed, name, true) ||
                 (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && strcasecmp(name, "authorization") && list_has(allowed, "*", false));
            p = *e ? e + 1 : e;
        }
    }
    http_resp_free(&r);
    return ok || fail(j, "CORS preflight did not allow method or headers");
}
static bool js_mime(const char *headers) {
    char mime[256];
    if (!field(headers, "Content-Type", mime, sizeof mime)) return false;
    char *semi = strchr(mime, ';'); if (semi) *semi = 0;
    size_t n = strlen(mime); while (n && mime[n - 1] == ' ') mime[--n] = 0;
    return !strcasecmp(mime, "text/javascript") || !strcasecmp(mime, "application/javascript") ||
           !strcasecmp(mime, "text/ecmascript") || !strcasecmp(mime, "application/ecmascript") ||
           !strcasecmp(mime, "application/x-javascript");
}
/* List-valued exposure fields may repeat. Scan every complete value without
   copying it into a smaller temporary array or dropping long field names. */
static bool exposed_header(const char *headers, const char *name, size_t len, bool wildcard) {
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && field_name_is(p, (size_t)(colon - p), "Access-Control-Expose-Headers") &&
            (list_has_slice(colon + 1, end, name, len, true) ||
             (wildcard && list_has_slice(colon + 1, end, "*", 1, false)))) return true;
        p = *end ? end + 2 : end;
    }
    return false;
}
static bool include_response_field(const struct job *j, const char *headers, const char *p, const char *end, bool cross) {
    const char *colon = memchr(p, ':', (size_t)(end - p));
    if (!colon || colon == p) return false;
    size_t len = (size_t)(colon - p);
    if (field_name_is(p, len, "set-cookie") || field_name_is(p, len, "set-cookie2")) return false;
    if (j->wire.kind != WEBNET_FETCH || !cross) return true;
    const char *safe = "cache-control,content-language,content-length,content-type,expires,last-modified,pragma";
    return list_has_slice(safe, safe + strlen(safe), p, len, true) ||
        exposed_header(headers, p, len, j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE);
}
static bool response_headers(struct job *j, const struct http_resp *r, bool cross) {
    const char *headers = http_response_headers(r);
    char prefix[512] = "";
    size_t prefix_len = 0;
    /* Negotiated private metadata; old clients retain the header-only format.
       The metadata line has no colon and cannot pass the server-field filter
       below. The genuine status phrase cannot inject lines (http.c validates
       CR/LF before storing it). Keep this prefix ahead of all server fields. */
    if (j->wire.user_navigation & WEBNET_WIRE_STATUS_LINE) {
        int n = snprintf(prefix, sizeof prefix,
                               "HTTP/1.1 %d %s\r\nHTTP/Nocturne-Meta cors=%d redirected=%d\r\n",
                               r->status, r->status_text, j->cors_tainted, j->redirected);
        if (n < 0 || (size_t)n >= sizeof prefix) return fail(j, "Response metadata exceeds limit");
        prefix_len = (size_t)n;
    }
    size_t limit = (j->wire.user_navigation & WEBNET_WIRE_LARGE_HEADERS) ?
        WEBNET_RESPONSE_HEADERS_MAX : WEBNET_REQUEST_HEADERS_MAX;
    size_t required = prefix_len;
    /* Measure the complete filtered output first; capacity is never a reason
       to turn an included field into an omitted field. */
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if (include_response_field(j, headers, p, end, cross)) {
            size_t l = (size_t)(end - p) + 2;
            if (required >= limit || l >= limit - required) return fail(j, "Response headers exceed negotiated limit");
            required += l;
        }
        p = *end ? end + 2 : end;
    }
    if (required >= limit) return fail(j, "Response headers exceed negotiated limit");
    char *out = malloc(required + 1);
    if (!out) return fail(j, "Out of memory filtering response headers");
    memcpy(out, prefix, prefix_len);
    size_t used = prefix_len;
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if (include_response_field(j, headers, p, end, cross)) {
            size_t l = (size_t)(end - p);
            memcpy(out + used, p, l); used += l;
            memcpy(out + used, "\r\n", 2); used += 2;
        }
        p = *end ? end + 2 : end;
    }
    out[used] = 0;
    free(j->response_headers); j->response_headers = out; j->response_headers_len = used;
    return true;
}
static bool run_http(struct job *j) {
    char *current = j->current, *document_url = j->document_url;
    char target_origin[160];
    if (!http_url(j->url, current, sizeof j->current, target_origin, sizeof target_origin)) return fail(j, "Invalid HTTP(S) URL");
    bool document_http = http_url(j->document, document_url, sizeof j->document_url, j->origin, sizeof j->origin);
    if (!document_http) snprintf(j->origin, sizeof j->origin, "null");
    char names[WEBNET_REQUEST_HEADERS_MAX];
    if (!validate_headers(j, names, sizeof names)) return false;
    if (WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation) == WEBNET_CACHE_ONLY_IF_CACHED) {
        if (!(j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "only-if-cached requires same-origin mode");
        return fail(j, "No cached response is available");
    }
    bool drop_body = false;
    for (unsigned hop = 0; hop <= 10; hop++) {
        if (hop == 10) return fail(j, "Too many redirects");
        if (!http_url(current, j->final_url, sizeof j->final_url, target_origin, sizeof target_origin)) return fail(j, "Redirect target is not HTTP(S)");
        snprintf(current, sizeof j->current, "%s", j->final_url);
        if ((j->wire.kind != WEBNET_NAVIGATION || !(j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION)) && document_http && !strncmp(j->origin, "https://", 8) && !strncmp(current, "http://", 7)) return fail(j, "HTTPS page cannot load insecure content");
        bool cross = strcmp(j->origin, target_origin) != 0;
        if (cross && (j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "Cross-origin fetch is forbidden by same-origin mode");
        /* Main fetch never changes cors response taint back to basic, even
           when a redirect returns to the original document's origin. */
        if (cors_kind(j) && cross) j->cors_tainted = true;
        const char *method = drop_body ? "GET" : j->method;
        /* Redirects can remove authorization/body fields; rederive the preflight names. */
        if (!validate_headers(j, names, sizeof names)) return false;
        bool simple_method = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
        if (j->cors_tainted && (!simple_method || *names || (j->wire.user_navigation & WEBNET_WIRE_FORCE_PREFLIGHT)) && !preflight(j, current, method, names)) return false;
        char *headers = j->outgoing;
        int hn;
        if (cors_kind(j)) hn = snprintf(headers, sizeof j->outgoing, "%sOrigin: %s\r\n", j->headers, request_origin(j));
        else hn = snprintf(headers, sizeof j->outgoing, "%s", j->headers);
        if (hn < 0 || (size_t)hn >= sizeof j->outgoing) return fail(j, "Request headers too large");
        if (!cache_headers(j, headers, sizeof j->outgoing, &hn)) return false;
        j->hop_cookies=j->wire.credentials==WEBNET_CREDENTIALS_INCLUDE ||
            (j->wire.credentials==WEBNET_CREDENTIALS_SAME_ORIGIN && !cross && !j->cors_tainted);
        if(j->hop_cookies){
            struct webcookie_context c={current,*j->document?j->document:j->wire.kind==WEBNET_NAVIGATION?NULL:"",method,
                j->wire.kind==WEBNET_NAVIGATION && (j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION),true,j->redirect_cross_site};
            long size=webcookie_get(j->cookies,&c,NULL,0,time(NULL));
            if(size<0 || (size_t)size+12>=sizeof j->outgoing-(size_t)hn)return fail(j,"Cookie request header exceeds limit");
            if(size){
                memcpy(headers+hn,"Cookie: ",8);hn+=8;
                long wrote=webcookie_get(j->cookies,&c,headers+hn,sizeof j->outgoing-(size_t)hn-2,time(NULL));
                if(wrote<0)return fail(j,"Cannot serialize request cookies");
                hn+=(int)wrote;memcpy(headers+hn,"\r\n",3);hn+=2;
            }
        }
        int left = remaining(j); if (!left) return false;
        free(j->body); j->body = NULL; j->body_len = j->body_cap = 0;
        struct http_req q = {method, current, headers,
                             drop_body ? NULL : j->request_body, drop_body ? 0 : j->wire.body_len,
                             left, body_cb, j, cookie_header};
        struct http_resp r;
        size_t cookie_start=j->cookie_len;
        size_t body_limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
        /* Only the negotiated script response may expand past the legacy gzip
           cap. General resources retain their existing streaming/default path. */
        int result = body_limit > WEBNET_BODY_LIMIT ? http_request_limited(&q, &r, body_limit) : http_request(&q, &r);
        if (result < 0) { j->cookie_len=cookie_start; if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
        if (!apply_cookie_events(j,cookie_start,method)) { http_resp_free(&r); return false; }
        if (j->cors_tainted && !cors_allowed(j, &r)) { http_resp_free(&r); return false; }
        bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
        if (redirect) {
            if (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_ERROR) {
                http_resp_free(&r); return fail(j, "Redirect forbidden by redirect mode");
            }
            char *location = j->location, *resolved = j->resolved;
            char previous_origin[160];
            snprintf(previous_origin, sizeof previous_origin, "%s", target_origin);
            if (!field(http_response_headers(&r), "Location", location, sizeof j->location) ||
                !web_resolve_url(current, location, resolved, sizeof j->resolved) ||
                !http_url(resolved, current, sizeof j->current, target_origin, sizeof target_origin)) {
                http_resp_free(&r); return fail(j, "Invalid or prohibited redirect target");
            }
            if (!webcookie_same_site(j->cookies,j->final_url,current)) j->redirect_cross_site=true;
            if (strcmp(previous_origin, target_origin)) {
                remove_request_header(j->headers, "Authorization");
                /* Fetch redirect-taint: an origin-changing redirect from a
                   URL already outside the request origin serializes to null. */
                if (strcmp(previous_origin, j->origin)) j->origin_tainted = true;
            }
            j->redirected = true;
            if ((!strcmp(method, "POST") && (r.status == 301 || r.status == 302)) ||
                (r.status == 303 && strcmp(method, "GET") && strcmp(method, "HEAD"))) {
                drop_body = true;
                remove_body_headers(j->headers);
            }
            http_resp_free(&r); continue;
        }
        if (j->wire.kind == WEBNET_MODULE && (r.status < 200 || r.status >= 300 || !js_mime(http_response_headers(&r)))) {
            http_resp_free(&r); return fail(j, "Module response must be successful JavaScript");
        }
        if (j->wire.kind == WEBNET_CLASSIC) {
            char nosniff[64];
            const char *headers = http_response_headers(&r);
            bool have_nosniff = field_exists(headers, "X-Content-Type-Options");
            if (r.status < 200 || r.status >= 300 ||
                (have_nosniff && (!field(headers, "X-Content-Type-Options", nosniff, sizeof nosniff) ||
                    (!strcasecmp(nosniff, "nosniff") && !js_mime(headers))))) {
                http_resp_free(&r); return fail(j, "Script response is not executable");
            }
        }
        j->status = r.status;
        bool headers_ok = response_headers(j, &r, j->cors_tainted); http_resp_free(&r);
        if (!headers_ok) return false;
        if (!j->body) { j->body = calloc(1, 1); if (!j->body) return fail(j, "Out of memory"); }
        return true;
    }
    return false;
}
int main(void) {
    struct job *j = calloc(1, sizeof *j);
    if (!j) return 1;
    struct webnet_wire_request *w = &j->wire;
    if (!read_all(0, w, sizeof *w) || w->magic != WEBNET_MAGIC || w->kind > WEBNET_FETCH ||
        !w->url_len || w->url_len >= WEBNET_URL_MAX || w->origin_len >= WEBNET_URL_MAX ||
        !w->method_len || w->method_len >= WEBNET_METHOD_MAX || w->headers_len >= WEBNET_REQUEST_HEADERS_MAX || w->body_len > WEBNET_BODY_LIMIT ||
        w->credentials>WEBNET_CREDENTIALS_INCLUDE || (w->user_navigation & ~WEBNET_WIRE_REQUEST_FLAGS) || w->cookie_len>WEBCOOKIE_SNAPSHOT_MAX ||
        !webnet_wire_script_flag_valid(w->kind, w->user_navigation) ||
        (w->credentials==WEBNET_CREDENTIALS_OMIT && w->cookie_len) ||
        WEBNET_WIRE_CACHE_MODE(w->user_navigation) > WEBNET_CACHE_ONLY_IF_CACHED ||
        (w->kind != WEBNET_FETCH && WEBNET_WIRE_CACHE_MODE(w->user_navigation) != WEBNET_CACHE_DEFAULT)) { free(j); return 1; }
    j->url = read_string(w->url_len, false); j->document = read_string(w->origin_len, false);
    j->method = read_string(w->method_len, false); j->headers = read_string(w->headers_len, false);
    j->request_body = read_string(w->body_len, true);
    /* Private transport diagnostics contain no cookie fields or metadata. */
    const char *cookie_snapshot_error=NULL;
    char *cookie_snapshot=malloc((size_t)w->cookie_len+1);
    if(!cookie_snapshot)cookie_snapshot_error="Cookie snapshot allocation failed";
    else if(!read_all(0,cookie_snapshot,w->cookie_len))cookie_snapshot_error="Cookie snapshot read failed";
    else cookie_snapshot[w->cookie_len]=0;
    j->cookies=webcookie_create();
    const char *cookie_psl_error="PSL open failed";
    if(j->cookies){
        FILE *f=fopen("/usr/share/browser/public_suffix_list.dat","r");
        if(f){
            char *text=malloc(WEBCOOKIE_PSL_MAX+1);
            if(!text)cookie_psl_error="PSL buffer allocation failed";
            else{
                size_t n=fread(text,1,WEBCOOKIE_PSL_MAX+1,f);
                if(ferror(f))cookie_psl_error="PSL read failed";
                else if(n>WEBCOOKIE_PSL_MAX)cookie_psl_error="PSL size limit exceeded";
                else if(!webcookie_psl_load(j->cookies,text,n))cookie_psl_error="PSL parse or policy allocation failed";
                else cookie_psl_error=NULL;
                free(text);
            }
            fclose(f);
        }
    }
    if(!j->cookies)fail(j,"Cookie jar allocation failed");
    else if(cookie_snapshot_error)fail(j,cookie_snapshot_error);
    else if(w->cookie_len&&!webcookie_import(j->cookies,cookie_snapshot,w->cookie_len,time(NULL))){
        /* import's bool result deliberately does not claim parse rather than OOM. */
        snprintf(j->error,sizeof j->error,"Cookie snapshot rejected or allocation failed (%s)",cookie_psl_error?cookie_psl_error:"PSL ready");
    }
    free(cookie_snapshot);
    if (!j->url || !j->document || !j->method || !j->headers || !j->request_body) fail(j, "Invalid request or out of memory");
    else if (!webnet_method_valid(j->method) || ((!strcmp(j->method, "GET") || !strcmp(j->method, "HEAD")) && w->body_len)) fail(j, "Invalid HTTP method or body");
    else if (!j->error[0] && remaining(j)) {
        if (!strncmp(j->url, "file:", 5)) load_file(j);
        else run_http(j);
    }
    if (j->error[0]) {
        free(j->body); j->body = NULL; j->body_len = 0; j->status = 0;
        free(j->response_headers); j->response_headers = NULL; j->response_headers_len = 0;
    }
    struct webnet_wire_response out = {0};
    out.magic = WEBNET_MAGIC; out.id = w->id; out.generation = w->generation; out.status = j->status;
    out.url_len = strlen(j->final_url); out.headers_len = j->response_headers_len;
    out.body_len = j->body_len; out.error_len = strlen(j->error);
    out.cookie_len=(uint32_t)j->cookie_len;
    bool ok = write_all(1, &out, sizeof out) && write_all(1, j->final_url, out.url_len) &&
              write_all(1, j->response_headers, out.headers_len) && write_all(1, j->body, out.body_len) && write_all(1, j->error, out.error_len) &&
              write_all(1,j->cookie_events,out.cookie_len);
    free(j->url); free(j->document); free(j->method); free(j->headers); free(j->request_body); free(j->body); free(j->response_headers); free(j->cookie_events); webcookie_free(j->cookies); free(j);
    return ok ? 0 : 1;
}
