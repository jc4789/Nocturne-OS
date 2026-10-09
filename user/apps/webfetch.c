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
#include <limits.h>

struct header_buffer { char *p; size_t length, capacity; };
struct job {
    struct webnet_wire_request wire;
    char *url, *document, *method, *headers, *request_body;
    char *origin, *target_origin;
    char *final_url;
    char error[160];
    struct header_buffer outgoing;
    char *non_simple;
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
    if ((uint64_t)len + 1 > SIZE_MAX) return NULL;
    char *p = malloc((size_t)len + 1);
    if (!p) return NULL;
    if (!read_all(0, p, len) || (!binary && memchr(p, 0, len))) { free(p); return NULL; }
    p[len] = 0; return p;
}
static bool fail(struct job *j, const char *message) {
    snprintf(j->error, sizeof j->error, "%s", message); return false;
}
static bool header_reserve(struct job *j, struct header_buffer *b, size_t extra) {
    if (b->length == SIZE_MAX || extra > SIZE_MAX - b->length - 1)
        return fail(j, "Request header allocation size overflow");
    size_t need = b->length + extra + 1;
    if (need <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 256;
    while (capacity < need) capacity = capacity > SIZE_MAX / 2 ? need : capacity * 2;
    char *p = realloc(b->p, capacity);
    if (!p) return fail(j, "Out of memory preparing request headers");
    b->p = p; b->capacity = capacity; b->p[b->length] = 0;
    return true;
}
static bool header_append(struct job *j, struct header_buffer *b, const char *p, size_t n) {
    if (!header_reserve(j, b, n)) return false;
    if (n) memcpy(b->p + b->length, p, n);
    b->length += n; b->p[b->length] = 0; return true;
}
static bool header_text(struct job *j, struct header_buffer *b, const char *p) {
    return header_append(j, b, p, strlen(p));
}
static int remaining(struct job *j) {
    uint64_t now = uptime_ms();
    if (now >= j->wire.deadline) { fail(j, "Request deadline exceeded"); return 0; }
    uint64_t left = j->wire.deadline - now;
    /* TCP/read timeout parameters are signed int. Never turn a long positive
       request allowance into a negative/infinite wait by narrowing it. */
    return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}
static bool token_char(unsigned char c) {
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
/* Only publish a replacement after complete allocation and buffer checks.
   Private slot may already own an origin; OOM never becomes opaque/null. */
static enum http_url_result http_url(const char *raw, char **canonical, char **origin) {
    /* Preserve this transport caller's stricter wire URL policy. Origin-only
       users may have non-ASCII paths or underscores: do not impose it there. */
    char *next = NULL, *next_origin = NULL;
    enum http_url_result result = http_canonical_owned_n(raw, raw ? strlen(raw) : 0, canonical ? &next : NULL, &next_origin);
    if (result != HTTP_URL_TUPLE) return result;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++)
        if (*p >= 127) { free(next); free(next_origin); return HTTP_URL_INVALID; }
    if (strchr(next_origin, '_')) { free(next); free(next_origin); return HTTP_URL_INVALID; }
    if(canonical){free(*canonical);*canonical=next;}
    free(*origin); *origin = next_origin;
    return HTTP_URL_TUPLE;
}
static bool url_failure(struct job *j, enum http_url_result result, const char *invalid) {
    return fail(j, result == HTTP_URL_OOM ? "Out of memory preparing canonical URL or origin" : invalid);
}
/* WHATWG host parsing uses the final nonempty label to detect possible IPv4:
   decimal, shortened/integer, hexadecimal and octal variants must not be
   treated as DNS domains just because this native stack lacks that parser.
   Conservatively leave them to the existing mixed-content rejection. */
static bool image_host_potential_ip(const char *host, size_t length) {
    if (length && host[length - 1] == '.') length--;
    size_t start = length;
    while (start && host[start - 1] != '.') start--;
    if (start == length) return true;
    bool decimal = true;
    for (size_t i = start; i < length; i++)
        if (host[i] < '0' || host[i] > '9') { decimal = false; break; }
    if (decimal) return true;
    if (length - start >= 2 && host[start] == '0' && (host[start + 1] == 'x' || host[start + 1] == 'X')) {
        for (size_t i = start + 2; i < length; i++)
            if (!((host[i] >= '0' && host[i] <= '9') ||
                  (host[i] >= 'a' && host[i] <= 'f') || (host[i] >= 'A' && host[i] <= 'F'))) return false;
        return true;
    }
    return false;
}
/* Prepare before publishing either canonical URL or origin. No retry path
   ever falls back to HTTP after a TLS error. Each redirect calls this too. */
static enum http_url_result image_http_url(struct job *j, const char *raw,
                                          char **canonical, char **origin) {
    char *next_origin = NULL,*prepared=NULL;
    enum http_url_result result = http_url(raw, &prepared, &next_origin);
    if (result != HTTP_URL_TUPLE) return result;
    char *upgraded = NULL;
    bool eligible = j->wire.kind == WEBNET_RESOURCE &&
        (j->wire.user_navigation & WEBNET_WIRE_IMAGE_UPGRADE) &&
        j->origin && !strncmp(j->origin, "https://", 8) && !strncmp(prepared, "http://", 7);
    if (eligible) {
        const char *host = prepared + 7;
        size_t hl = strcspn(host, ":/?#");
        if (!image_host_potential_ip(host, hl)) {
            /* http_url already removes default port80. Explicit nondefault
               ports remain in this spelling and survive the scheme change. */
            size_t length = strlen(prepared);
            if (length > SIZE_MAX - 2 || length >= UINT32_MAX) {
                free(prepared);free(next_origin); return HTTP_URL_INVALID;
            }
            upgraded = malloc(length + 2);
            if (!upgraded) { free(prepared);free(next_origin); return HTTP_URL_OOM; }
            memcpy(upgraded, "https://", 8); memcpy(upgraded + 8, prepared + 7, length - 7 + 1);
            result = http_url(upgraded, &prepared, &next_origin);
            free(upgraded);
            if (result != HTTP_URL_TUPLE) { free(prepared);free(next_origin); return result; }
        }
    }
    size_t length = strlen(prepared);
    if (!canonical || length > webnet_wire_url_limit(j->wire.user_navigation) || length == SIZE_MAX) { free(prepared);free(next_origin); return HTTP_URL_INVALID; }
    free(*canonical);*canonical=prepared;
    free(*origin); *origin = next_origin; return HTTP_URL_TUPLE;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int file_path(const char *url,char **out) {
    *out=NULL;if(!url||strncmp(url,"file:///",8))return 0;
    size_t length=strlen(url);if(length>UINT32_MAX||length>SIZE_MAX-2)return 0;
    char *decoded=malloc(length+1);if(!decoded)return -1;
    const char *p=url+7;size_t n=0;
    while(*p&&*p!='?'&&*p!='#'){
        unsigned char c=(unsigned char)*p++;
        if(c=='%'){int a=hex((unsigned char)p[0]),b=p[0]?hex((unsigned char)p[1]):-1;if(a<0||b<0){free(decoded);return 0;}c=(unsigned char)((a<<4)|b);p+=2;}
        if(!c||c<32||c==127||c=='\\'){free(decoded);return 0;}decoded[n++]=(char)c;
    }
    decoded[n]=0;char *path=malloc(n+2);if(!path){free(decoded);return -1;}
    size_t at=1;path[0]='/';char *s=decoded;
    while(*s){
        while(*s=='/')s++;if(!*s)break;
        char *end=strchr(s,'/');size_t l=end?(size_t)(end-s):strlen(s);
        if(l==1&&s[0]=='.'){}
        else if(l==2&&s[0]=='.'&&s[1]=='.'){
            if(at==1){free(path);free(decoded);return 0;}
            while(at>1&&path[at-1]!='/')at--;if(at>1)at--;
        }else{if(at>1)path[at++]='/';memcpy(path+at,s,l);at+=l;}
        s+=l;
    }
    path[at]=0;free(decoded);*out=path;return 1;
}
static bool load_file_open(struct job *j,const char *path) {
    if(strcmp(j->method,"GET")||j->wire.body_len)return fail(j,"Local resources support GET only");
    int fd=open(path,O_RDONLY);if(fd<0)return fail(j,"Cannot open local resource");
    struct n_stat st;
    if(fstat(fd,&st)<0||st.type!=N_FT_FILE||st.size>webnet_wire_response_limit(j->wire.kind,j->wire.user_navigation)||(uint64_t)st.size+1>SIZE_MAX){close(fd);return fail(j,"Invalid or oversized local resource");}
    j->body=malloc((size_t)st.size+1);if(!j->body){close(fd);return fail(j,"Out of memory");}
    size_t left=(size_t)st.size;
    while(left){
        if(!remaining(j)){close(fd);return false;}
        ssize_t k=read(fd,j->body+j->body_len,MIN(left,65536));
        if(k<=0){close(fd);return fail(j,"Local resource ended early");}
        j->body_len+=(size_t)k;left-=(size_t)k;
    }
    close(fd);j->body[j->body_len]=0;
    char *url=strdup(j->url);if(!url)return fail(j,"Out of memory copying local URL");
    free(j->final_url);j->final_url=url;j->status=200;return true;
}
static bool load_file(struct job *j) {
    if(j->wire.kind==WEBNET_FETCH)return fail(j,"fetch accepts only HTTP(S)");
    char *path=NULL,*document_path=NULL;int parsed=file_path(j->url,&path);bool ok=false;
    if(parsed!=1)return fail(j,parsed<0?"Out of memory preparing local URL":"Invalid local URL");
    if(j->wire.kind==WEBNET_NAVIGATION){
        if(!(j->wire.user_navigation&WEBNET_WIRE_USER_NAVIGATION)){fail(j,"Only a user navigation may open a local file");goto out;}
    }else{
        parsed=file_path(j->document,&document_path);
        if(parsed!=1){fail(j,parsed<0?"Out of memory preparing local document URL":"Remote pages cannot open local files");goto out;}
        char *slash=strrchr(document_path,'/');if(!slash){fail(j,"Invalid local document URL");goto out;}
        size_t dl=(size_t)(slash-document_path)+1;
        if(strncmp(path,document_path,dl)){fail(j,"Local resource escapes document directory");goto out;}
    }
    ok=load_file_open(j,path);
out:
    free(path);free(document_path);return ok;
}
static bool cors_kind(const struct job *j) { return j->wire.kind == WEBNET_FETCH || j->wire.kind == WEBNET_MODULE; }
/* Redirect-taint changes the serialized Origin, not the document origin used
   for same-origin mode and mixed-content checks. It can never be cleared. */
static const char *request_origin(const struct job *j) { return j->origin_tainted || !j->origin ? "null" : j->origin; }
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
static bool field_slice(const char *headers, const char *name, const char **out, size_t *length) {
    size_t nl = strlen(name); bool found = false;
    *out = NULL; *length = 0;
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            if (found) { *out = NULL; *length = 0; return false; }
            const char *v = p + nl + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
            const char *ve = end; while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            *out = v; *length = (size_t)(ve - v); found = true;
        }
        p = *end ? end + 2 : end;
    }
    return found;
}
static bool field(const char *headers, const char *name, char *out, size_t cap) {
    const char *value; size_t length;
    if (!cap) return false;
    out[0] = 0;
    if (!field_slice(headers, name, &value, &length) || length >= cap) return false;
    memcpy(out, value, length); out[length] = 0; return true;
}
static bool validate_headers(struct job *j) {
    struct header_buffer seen = {0}, names = {0};
    char *name = NULL; bool ok = false;
    if (!header_reserve(j, &seen, 0) || !header_reserve(j, &names, 0)) goto out;
    const char *p = j->headers;
    while (*p) {
        const char *end = strstr(p, "\r\n");
        if (!end) { fail(j, "Request headers must end in CRLF"); goto out; }
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || colon == p) { fail(j, "Invalid request header name"); goto out; }
        size_t nl = colon - p;
        name = malloc(nl + 1);
        if (!name) { fail(j, "Out of memory validating request header name"); goto out; }
        for (size_t i = 0; i < nl; i++) {
            if (!token_char((unsigned char)p[i])) { fail(j, "Invalid request header name"); goto out; }
            name[i] = (char)tolower((unsigned char)p[i]);
        }
        name[nl] = 0;
        if (forbidden_header(name)) { fail(j, "Prohibited request header"); goto out; }
        if (list_has(seen.p, name, true)) { fail(j, "Duplicate request header"); goto out; }
        if ((seen.length && !header_text(j, &seen, ",")) || !header_append(j, &seen, name, nl)) goto out;
        const char *v = colon + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
        size_t vl = end - v;
        for (size_t i = 0; i < vl; i++) if (((unsigned char)v[i] < 32 && v[i] != '\t') || (unsigned char)v[i] == 127) { fail(j, "Invalid request header value"); goto out; }
        /* Fetch's 128-byte safelist condition is a protocol rule, not a
           request capacity: longer values remain valid and need preflight. */
        bool safe = false;
        if (vl <= 128) { char value[129]; memcpy(value, v, vl); value[vl] = 0; safe = safelisted(name, value); }
        if (!safe) {
            if ((names.length && !header_text(j, &names, ", ")) || !header_append(j, &names, name, nl)) goto out;
        }
        free(name); name = NULL;
        p = end + 2;
    }
    free(j->non_simple); j->non_simple = names.p; names.p = NULL; ok = true;
out:
    free(name); free(seen.p); free(names.p); return ok;
}
/* Fetch HTTP-network-or-cache steps 16-18 (WHATWG, consulted 2026-10-07).
   This transport has no HTTP response cache: default/force-cache/no-cache
   have genuine cache-miss network semantics, reload/no-store bypass storage,
   and only-if-cached fails before network/preflight. Generate these fields
   after classifying AUTHOR headers for CORS, independently on every hop. */
static bool cache_headers(struct job *j) {
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
    return header_text(j, &j->outgoing, pragma) && header_text(j, &j->outgoing, control);
}
static bool prepare_outgoing(struct job *j, const char *current, const char *method, bool cross) {
    j->outgoing.length = 0;
    if (!header_text(j, &j->outgoing, j->headers)) return false;
    if (cors_kind(j) && !(header_text(j, &j->outgoing, "Origin: ") &&
        header_text(j, &j->outgoing, request_origin(j)) && header_text(j, &j->outgoing, "\r\n"))) return false;
    if (!cache_headers(j)) return false;
    j->hop_cookies = j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE ||
        (j->wire.credentials == WEBNET_CREDENTIALS_SAME_ORIGIN && !cross && !j->cors_tainted);
    if (j->hop_cookies) {
        struct webcookie_context c = {current, *j->document ? j->document : j->wire.kind == WEBNET_NAVIGATION ? NULL : "", method,
            j->wire.kind == WEBNET_NAVIGATION && (j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION), true, j->redirect_cross_site};
        time_t now = time(NULL);
        long size = webcookie_get(j->cookies, &c, NULL, 0, now);
        if (size < 0) return fail(j, "Cannot measure request cookies");
        if (size) {
            if (!header_text(j, &j->outgoing, "Cookie: ") || !header_reserve(j, &j->outgoing, (size_t)size)) return false;
            long wrote = webcookie_get(j->cookies, &c, j->outgoing.p + j->outgoing.length, (size_t)size + 1, now);
            if (wrote != size) return fail(j, "Cannot serialize complete request cookies");
            j->outgoing.length += (size_t)wrote;
            if (!header_text(j, &j->outgoing, "\r\n")) return false;
        }
    }
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
    const char *headers = http_response_headers(r);
    const char *allow; size_t length;
    const char *origin = request_origin(j);
    if (!field_slice(headers, "Access-Control-Allow-Origin", &allow, &length))
        return fail(j, "Cross-origin response is not allowed");
    bool wildcard = length == 1 && allow[0] == '*';
    if (!wildcard && (length != strlen(origin) || memcmp(allow, origin, length)))
        return fail(j, "Cross-origin response is not allowed");
    if (j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE) {
        char credentials[16];
        if (wildcard || !field(headers,"Access-Control-Allow-Credentials",credentials,sizeof credentials) ||
            strcmp(credentials,"true")) return fail(j,"Credentialed CORS requires exact origin and credentials permission");
    }
    return true;
}
static int cookie_header(void *opaque, const char *line, size_t length) {
    struct job *j = opaque;
    if (!j->hop_cookies || length < 11 || strncasecmp(line,"Set-Cookie:",11)) return 0;
    const char *value = line+11, *end=line+length;
    while (value<end && (*value==' ' || *value=='\t')) value++;
    size_t vl=(size_t)(end-value), ul=strlen(j->final_url), limit=webnet_wire_cookie_limit(j->wire.user_navigation);
    uint64_t size=sizeof(struct webnet_wire_cookie)+(uint64_t)ul+vl;
    if (vl>WEBCOOKIE_FIELD_MAX || ul>UINT32_MAX || j->cookie_len>limit || size>limit-j->cookie_len || size>SIZE_MAX) {
        fail(j,"Cookie response fields exceed bounded storage"); return -1;
    }
    size_t need=(size_t)size;
    if (j->cookie_len+need>j->cookie_cap) {
        size_t cap=j->cookie_cap?j->cookie_cap:8192;
        while(cap<j->cookie_len+need){if(cap>limit/2){cap=j->cookie_len+need;break;}cap*=2;}
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
        char *url=malloc((size_t)e.url_len+1);if(!url)return fail(j,"Out of memory applying cookie URL");
        memcpy(url,p,e.url_len);url[e.url_len]=0;p+=e.url_len;
        struct webcookie_context c={url,*j->document?j->document:j->wire.kind==WEBNET_NAVIGATION?NULL:"",method,
            j->wire.kind==WEBNET_NAVIGATION && (j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION),true,e.redirect_cross_site!=0};
        int result=webcookie_set(j->cookies,&c,(const char*)p,e.value_len,e.received);free(url);
        if(result<0)return fail(j,"Out of memory applying cookies");
        p+=e.value_len;
    }
    return true;
}
static int body_cb(void *opaque, const char *data, size_t n) {
    struct job *j = opaque;
    if (!remaining(j)) return -1;
    size_t limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
    if (j->body_len > limit || n > limit - j->body_len) {
        fail(j, "Response body exceeds the negotiated wire representation"); return -1;
    }
    size_t need = j->body_len + n + 1;
    if (need > j->body_cap) {
        size_t cap = j->body_cap ? j->body_cap : 8192;
        while (cap < need) cap = cap > SIZE_MAX / 2 ? need : MIN(cap * 2, limit + 1);
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
    if (j->body_len > WEBNET_BODY_LIMIT || n > WEBNET_BODY_LIMIT - j->body_len) { fail(j, "Preflight response length is not representable"); return -1; }
    j->body_len += n; return 0;
}
static bool preflight(struct job *j, const char *url, const char *method, const char *names) {
    struct header_buffer head = {0};
    bool prepared = header_text(j, &head, "Origin: ") && header_text(j, &head, request_origin(j)) &&
        header_text(j, &head, "\r\nAccess-Control-Request-Method: ") && header_text(j, &head, method) && header_text(j, &head, "\r\n");
    if (prepared && *names) prepared = header_text(j, &head, "Access-Control-Request-Headers: ") &&
        header_text(j, &head, names) && header_text(j, &head, "\r\n");
    int left = prepared ? remaining(j) : 0;
    if (!left) { free(head.p); return false; }
    struct http_req q = {"OPTIONS", url, head.p, NULL, 0, left, discard_cb, j, NULL};
    struct http_resp r;
    j->body_len = 0;
    int result = http_request(&q, &r);
    free(head.p);
    j->body_len = 0;
    if (result < 0) { if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
    if (r.status < 200 || r.status >= 300) { http_resp_free(&r); return fail(j, "CORS preflight failed"); }
    if (!cors_allowed(j, &r)) { http_resp_free(&r); return false; }
    const char *methods = NULL, *allowed = NULL; size_t methods_len = 0, allowed_len = 0;
    /* Safelisted methods do not need an Allow-Methods entry. XHR upload
       listeners can force OPTIONS even for a simple GET/POST request. */
    bool ok = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST") ||
              (field_slice(http_response_headers(&r), "Access-Control-Allow-Methods", &methods, &methods_len) &&
               (list_has_slice(methods, methods + methods_len, method, strlen(method), false) ||
                (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && list_has_slice(methods, methods + methods_len, "*", 1, false))));
    if (*names) {
        ok = ok && field_slice(http_response_headers(&r), "Access-Control-Allow-Headers", &allowed, &allowed_len);
        for (const char *p = names; ok && *p;) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = strchr(p, ','); if (!e) e = p + strlen(p);
            size_t l = e - p; while (l && p[l - 1] == ' ') l--;
            /* Authorization is never covered by the CORS header wildcard. */
            ok = list_has_slice(allowed, allowed + allowed_len, p, l, true) ||
                 (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && !field_name_is(p, l, "authorization") &&
                  list_has_slice(allowed, allowed + allowed_len, "*", 1, false));
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
    char *prefix = NULL;
    size_t prefix_len = 0;
    /* Negotiated private metadata; old clients retain the header-only format.
       The metadata line has no colon and cannot pass the server-field filter
       below. The genuine status phrase cannot inject lines (http.c validates
       CR/LF before storing it). Keep this prefix ahead of all server fields. */
    if (j->wire.user_navigation & WEBNET_WIRE_STATUS_LINE) {
        const char *reason = http_response_status_text(r);
        size_t reason_len = strlen(reason);
        char start[32], end[96];
        int a = snprintf(start, sizeof start, "HTTP/1.1 %d ", r->status);
        int b = snprintf(end, sizeof end, "\r\nHTTP/Nocturne-Meta cors=%d redirected=%d\r\n", j->cors_tainted, j->redirected);
        if (a < 0 || b < 0 || (size_t)a >= sizeof start || (size_t)b >= sizeof end ||
            reason_len > SIZE_MAX - (size_t)a - (size_t)b - 1) return fail(j, "Response metadata length overflow");
        prefix_len = (size_t)a + reason_len + (size_t)b;
        if (prefix_len > webnet_wire_header_limit(j->wire.user_navigation)) return fail(j, "Response metadata exceeds negotiated representation");
        prefix = malloc(prefix_len + 1);
        if (!prefix) return fail(j, "Out of memory storing response metadata");
        memcpy(prefix, start, (size_t)a);
        memcpy(prefix + a, reason, reason_len);
        memcpy(prefix + a + reason_len, end, (size_t)b + 1);
    }
    size_t limit = webnet_wire_header_limit(j->wire.user_navigation);
    size_t required = prefix_len;
    /* Measure the complete filtered output first; capacity is never a reason
       to turn an included field into an omitted field. */
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if (include_response_field(j, headers, p, end, cross)) {
            size_t l = (size_t)(end - p);
            if (l > SIZE_MAX - 2 || required > limit || l + 2 > limit - required) {
                free(prefix); return fail(j, "Response headers exceed negotiated representation");
            }
            l += 2;
            required += l;
        }
        p = *end ? end + 2 : end;
    }
    if (required > limit || required == SIZE_MAX) { free(prefix); return fail(j, "Response header length overflow"); }
    char *out = malloc(required + 1);
    if (!out) { free(prefix); return fail(j, "Out of memory filtering response headers"); }
    if (prefix_len) memcpy(out, prefix, prefix_len);
    free(prefix);
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
    enum http_url_result parsed = *j->document ? http_url(j->document, NULL, &j->origin) : HTTP_URL_OPAQUE;
    if (parsed != HTTP_URL_TUPLE && parsed != HTTP_URL_OPAQUE)
        return url_failure(j, parsed, "Invalid initiating document URL");
    bool document_http = parsed == HTTP_URL_TUPLE;
    parsed = image_http_url(j, j->url, &j->final_url, &j->target_origin);
    if (parsed != HTTP_URL_TUPLE) return url_failure(j, parsed, "Invalid HTTP(S) URL");
    if (!validate_headers(j)) return false;
    if (WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation) == WEBNET_CACHE_ONLY_IF_CACHED) {
        if (!(j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "only-if-cached requires same-origin mode");
        return fail(j, "No cached response is available");
    }
    bool drop_body = false;
    for (unsigned hop = 0; hop <= 10; hop++) {
        if (hop == 10) return fail(j, "Too many redirects");
        /* Initial and every redirect hop were fully prepared before publish.
           The current final URL is the single canonical owner, not a copy. */
        const char *current=j->final_url;
        if ((j->wire.kind != WEBNET_NAVIGATION || !(j->wire.user_navigation & WEBNET_WIRE_USER_NAVIGATION)) && document_http && !strncmp(j->origin, "https://", 8) && !strncmp(current, "http://", 7)) return fail(j, "HTTPS page cannot load insecure content");
        bool cross = !j->origin || strcmp(j->origin, j->target_origin) != 0;
        if (cross && (j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "Cross-origin fetch is forbidden by same-origin mode");
        /* Main fetch never changes cors response taint back to basic, even
           when a redirect returns to the original document's origin. */
        if (cors_kind(j) && cross) j->cors_tainted = true;
        const char *method = drop_body ? "GET" : j->method;
        /* Redirects can remove authorization/body fields; rederive the preflight names. */
        if (!validate_headers(j)) return false;
        const char *names = j->non_simple;
        bool simple_method = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
        if (j->cors_tainted && (!simple_method || *names || (j->wire.user_navigation & WEBNET_WIRE_FORCE_PREFLIGHT)) && !preflight(j, current, method, names)) return false;
        if (!prepare_outgoing(j, current, method, cross)) return false;
        int left = remaining(j); if (!left) return false;
        free(j->body); j->body = NULL; j->body_len = j->body_cap = 0;
        struct http_req q = {method, current, j->outgoing.p,
                             drop_body ? NULL : j->request_body, drop_body ? 0 : j->wire.body_len,
                             left, body_cb, j, cookie_header};
        struct http_resp r;
        size_t cookie_start=j->cookie_len;
        size_t body_limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
        int result = http_request_limited(&q, &r, body_limit);
        if (result < 0) { j->cookie_len=cookie_start; if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
        if (!apply_cookie_events(j,cookie_start,method)) { http_resp_free(&r); return false; }
        if (j->cors_tainted && !cors_allowed(j, &r)) { http_resp_free(&r); return false; }
        bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
        if (redirect) {
            if (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_ERROR) {
                http_resp_free(&r); return fail(j, "Redirect forbidden by redirect mode");
            }
            const char *location_value;size_t location_len;
            if (!field_slice(http_response_headers(&r), "Location", &location_value, &location_len) || location_len==SIZE_MAX) {
                http_resp_free(&r); return fail(j, "Invalid or prohibited redirect target");
            }
            char *location=malloc(location_len+1),*resolved=NULL;
            if(!location){http_resp_free(&r);return fail(j,"Out of memory preparing redirect target");}
            memcpy(location,location_value,location_len);location[location_len]=0;
            int resolution=web_resolve_url_owned(current,location,&resolved);free(location);
            if(resolution!=1){http_resp_free(&r);return fail(j,resolution<0?"Out of memory resolving redirect target":"Invalid or prohibited redirect target");}
            char *next_origin = NULL,*next_url=NULL;
            parsed = image_http_url(j, resolved, &next_url, &next_origin);free(resolved);
            if (parsed != HTTP_URL_TUPLE) {
                http_resp_free(&r); return url_failure(j, parsed, "Invalid or prohibited redirect target");
            }
            if (!webcookie_same_site(j->cookies,j->final_url,next_url)) j->redirect_cross_site=true;
            if (strcmp(j->target_origin, next_origin)) {
                remove_request_header(j->headers, "Authorization");
                /* Fetch redirect-taint: an origin-changing redirect from a
                   URL already outside the request origin serializes to null. */
                if (!j->origin || strcmp(j->target_origin, j->origin)) j->origin_tainted = true;
            }
            free(j->target_origin); j->target_origin = next_origin;
            free(j->final_url);j->final_url=next_url;
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
    size_t payload_size;
    if (!read_all(0, w, sizeof *w) || w->magic != WEBNET_MAGIC || w->kind > WEBNET_FETCH ||
        !w->url_len || w->url_len > webnet_wire_url_limit(w->user_navigation) || w->origin_len > webnet_wire_url_limit(w->user_navigation) ||
        !w->method_len || w->headers_len > webnet_wire_request_header_limit(w->user_navigation) || w->body_len > WEBNET_BODY_LIMIT ||
        w->credentials>WEBNET_CREDENTIALS_INCLUDE || (w->user_navigation & ~WEBNET_WIRE_REQUEST_FLAGS) || w->cookie_len>WEBCOOKIE_SNAPSHOT_MAX ||
        !webnet_wire_script_flag_valid(w->kind, w->user_navigation) || !webnet_wire_image_flag_valid(w->kind, w->user_navigation) || !webnet_wire_request_payload_size(w, &payload_size) ||
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
    const char *final_url=j->final_url?j->final_url:"";
    out.url_len = strlen(final_url); out.headers_len = j->response_headers_len;
    out.body_len = j->body_len; out.error_len = strlen(j->error);
    out.cookie_len=(uint32_t)j->cookie_len;
    bool ok = write_all(1, &out, sizeof out) && write_all(1, final_url, out.url_len) &&
              write_all(1, j->response_headers, out.headers_len) && write_all(1, j->body, out.body_len) && write_all(1, j->error, out.error_len) &&
              write_all(1,j->cookie_events,out.cookie_len);
    free(j->origin); free(j->target_origin);free(j->final_url);
    free(j->url); free(j->document); free(j->method); free(j->headers); free(j->request_body); free(j->body); free(j->response_headers); free(j->cookie_events); free(j->non_simple); free(j->outgoing.p); webcookie_free(j->cookies); free(j);
    return ok ? 0 : 1;
}
