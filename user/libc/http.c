/* HTTP/1.1 persistent connections and ALPN HTTP/2. HTTP semantics and content
   decoding are shared across both wire protocols. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <threads.h>
#include "../include/http.h"
#include "http2.h"
#include "browser_identity.h"
extern uint64_t uptime_ms(void);
extern void n_sleep_ms(uint64_t);

/* The pool is process-local. It never coalesces certificate names or origins.
   Keys include the exact caller partition and credential header bytes; no
   cookies, authorization values or HPACK state cross that boundary. */
struct pooled_connection {
    struct pooled_connection *next;
    net_stream *stream;
    struct http2_connection *h2;
    char *host,*key;
    uint16_t port;
    bool tls,prior,opening,retire;
    unsigned users;
    uint64_t idle,opened_at;
    uint64_t identity;
};
/* A same-key connection still opening usually becomes shareable (HTTP/2) or
   idle within a second, so later requests wait for it briefly. A stalled
   TCP/TLS open must not hold every request to that host for its whole
   allowance: after the patience expires another open starts, and at most
   POOL_OPEN_PER_KEY opens race per key. The pool never needs more
   connections than concurrent requests; idle ones are evicted first. */
#define POOL_OPEN_PATIENCE_MS 2000u
#define POOL_OPEN_PER_KEY 6u
#define POOL_MAX 32u
static struct pooled_connection *pool;
static mtx_t pool_lock;
static unsigned pool_count;
static uint64_t connection_identity;
static void pool_enter(void){mtx_lock(&pool_lock);}
static void pool_leave(void){mtx_unlock(&pool_lock);}
static void pool_destroy(struct pooled_connection *p) {
    http2_free(p->h2);ns_abort(p->stream);free(p->host);free(p->key);free(p);
}
static void pool_unlink(struct pooled_connection *p) {
    struct pooled_connection **at=&pool;while(*at&&*at!=p)at=&(*at)->next;
    if(*at){*at=p->next;pool_count--;}
}
static char *pool_key(const struct http_req *rq,const char *partition) {
    const char *headers=rq->headers?rq->headers:"";size_t n=strlen(partition?partition:"");
    size_t credentials=0;
    for(const char *p=headers;*p;) {
        const char *end=strstr(p,"\r\n");if(!end)return NULL;
        if(!strncasecmp(p,"Authorization:",14)||!strncasecmp(p,"Proxy-Authorization:",20)||!strncasecmp(p,"Cookie:",7))
            credentials+=(size_t)(end-p)+2;
        p=end+2;
    }
    if(n>SIZE_MAX-credentials-48)return NULL;
    char *key=malloc(n+credentials+48);if(!key)return NULL;
    int prefix=snprintf(key,n+credentials+48,"%zu:",n);size_t at=(size_t)prefix;
    memcpy(key+at,partition?partition:"",n);at+=n;
    for(const char *p=headers;*p;) {
        const char *end=strstr(p,"\r\n");
        if(!strncasecmp(p,"Authorization:",14)||!strncasecmp(p,"Proxy-Authorization:",20)||!strncasecmp(p,"Cookie:",7)) {
            size_t bytes=(size_t)(end-p)+2;memcpy(key+at,p,bytes);at+=bytes;
        }
        p=end+2;
    }
    key[at]=0;return key;
}
/* An open raced against a slower same-key open gives up once any same-key
   HTTP/2 connection can take the stream: the request then joins it instead
   of waiting for its own handshake, which may stall for its whole allowance. */
struct pool_open_watch {
    const struct http_stream_hooks *hooks;void *ctx;
    const struct pooled_connection *self;bool superseded;
};
static int pool_open_cancelled(void *opaque) {
    struct pool_open_watch *w=opaque;
    if(w->hooks&&w->hooks->cancelled&&w->hooks->cancelled(w->ctx))return 1;
    const struct pooled_connection *self=w->self;bool shared=false;
    pool_enter();
    for(struct pooled_connection *p=pool;p&&!shared;p=p->next)
        shared=p!=self&&!p->opening&&!p->retire&&p->h2&&p->port==self->port&&p->tls==self->tls&&
            p->prior==self->prior&&!strcmp(p->host,self->host)&&!strcmp(p->key,self->key)&&http2_available(p->h2);
    pool_leave();
    if(shared)w->superseded=true;
    return shared;
}
static struct pooled_connection *pool_acquire(const struct http_req *rq,const struct url_owned *u,
    int timeout,const char *partition,bool prior,const struct http_stream_hooks *hooks,
    struct http_transport_timing *timing,struct http_transport_result *result,char *error,size_t error_cap) {
    char *key=pool_key(rq,partition);if(!key){errno=ENOMEM;snprintf(error,error_cap,"out of memory preparing connection key");return NULL;}
    uint64_t started=uptime_ms();
    for(;;) {
        if(hooks&&hooks->cancelled&&hooks->cancelled(rq->ctx)){errno=ECANCELED;snprintf(error,error_cap,"request cancelled");break;}
        if(timeout>0&&uptime_ms()-started>=(uint64_t)timeout){errno=ETIMEDOUT;snprintf(error,error_cap,"connection pool wait timed out");break;}
        pool_enter();unsigned opening=0;uint64_t newest_open=0;
        struct pooled_connection *oldest=NULL;
        for(struct pooled_connection *p=pool;p;p=p->next) {
            if(!p->users&&(!oldest||p->idle<oldest->idle))oldest=p;
            if(p->retire||p->port!=u->port||p->tls!=u->tls||p->prior!=prior||strcmp(p->host,u->host)||strcmp(p->key,key))continue;
            if(p->opening){opening++;if(p->opened_at>newest_open)newest_open=p->opened_at;continue;}
            if(!p->users&&uptime_ms()-p->idle>=30000){p->retire=true;continue;}
            if(p->h2?http2_available(p->h2):!p->users) {
                p->users++;pool_leave();free(key);
                if(result){result->reused=true;result->connection_id=p->identity;result->http2=p->h2!=NULL;}
                if(timing)memset(timing,0,sizeof *timing);
                if(!p->h2)ns_set_timeout(p->stream,timeout);return p;
            }
        }
        if(opening&&(opening>=POOL_OPEN_PER_KEY||uptime_ms()-newest_open<POOL_OPEN_PATIENCE_MS)){pool_leave();n_sleep_ms(1);continue;}
        if(oldest&&(oldest->retire||pool_count>=POOL_MAX)) {
            pool_unlink(oldest);pool_leave();pool_destroy(oldest);continue;
        }
        if(pool_count>=POOL_MAX){pool_leave();n_sleep_ms(1);continue;}
        struct pooled_connection *p=calloc(1,sizeof *p);
        if(!p){pool_leave();errno=ENOMEM;break;}
        size_t host_len=strlen(u->host);p->host=malloc(host_len+1);
        if(!p->host){free(p);pool_leave();errno=ENOMEM;break;}
        memcpy(p->host,u->host,host_len+1);
        p->key=key;p->port=u->port;p->tls=u->tls;p->prior=prior;p->opening=true;p->users=1;p->opened_at=uptime_ms();
        p->identity=++connection_identity;
        bool raced=opening!=0;
        p->next=pool;pool=p;pool_count++;pool_leave();
        struct pool_open_watch watch={hooks,rq->ctx,p,false};
        p->stream=raced?ns_open_http_cancelled_timed(u->host,u->port,u->tls,timeout,error,error_cap,timing,key,
                pool_open_cancelled,&watch):
            ns_open_http_cancelled_timed(u->host,u->port,u->tls,timeout,error,error_cap,timing,key,
                hooks?hooks->cancelled:NULL,rq->ctx);
        if(watch.superseded&&!p->stream) {
            /* The request retries the pool and joins the shared connection. */
            pool_enter();pool_unlink(p);pool_leave();
            key=p->key;p->key=NULL;pool_destroy(p);error[0]=0;errno=0;continue;
        }
        /* The opener's hook belongs to one request (and maybe this frame).
           HTTP/2 cancels per stream; HTTP/1 installs each user's hook. */
        if(p->stream)ns_set_cancel(p->stream,NULL,NULL);
        if(p->stream&&(prior||!strcmp(ns_protocol(p->stream),"h2"))) {
            p->h2=http2_new(p->stream);if(!p->h2){errno=errno?errno:EIO;snprintf(error,error_cap,"HTTP/2 preface failed");}
        }
        pool_enter();p->opening=false;
        if(!p->stream||((prior||!strcmp(ns_protocol(p->stream),"h2"))&&!p->h2)) {
            pool_unlink(p);pool_leave();pool_destroy(p);return NULL;
        }
        if(result){result->connection_id=p->identity;result->http2=p->h2!=NULL;}
        pool_leave();return p;
    }
    free(key);return NULL;
}
static void pool_release(struct pooled_connection *p,bool reusable) {
    if(!p)return;
    /* h2 cancellation only resets one stream; protocol/transport failures
       make http2_available false and retire the connection when idle. */
    pool_enter();if(!reusable&&!p->h2)p->retire=true;
    if(p->h2&&!http2_reusable(p->h2))p->retire=true;
    if(!p->h2)ns_set_cancel(p->stream,NULL,NULL);
    if(p->users)p->users--;p->idle=uptime_ms();
    bool destroy=!p->users&&p->retire;if(destroy)pool_unlink(p);pool_leave();
    if(destroy)pool_destroy(p);
}
void http_pool_clear(void) {
    for(;;) {
        pool_enter();struct pooled_connection *idle=NULL;
        for(struct pooled_connection *p=pool;p;p=p->next){p->retire=true;if(!p->users){idle=p;break;}}
        if(idle)pool_unlink(idle);pool_leave();if(!idle)break;pool_destroy(idle);
    }
    ns_sessions_clear();
}

bool url_parse(const char *s, struct url *u) {
    if (!s || !u || strlen(s) >= HTTP_URL_MAX) return false;
    memset(u, 0, sizeof *u);
    if (!strncmp(s, "https://", 8)) {
        u->tls = true;
        u->port = 443;
        s += 8;
    } else {
        u->port = 80;
        if (!strncmp(s, "http://", 7)) s += 7;
    }
    size_t hl = strcspn(s, ":/?");
    if (hl == 0 || hl >= sizeof u->host) return false;
    memcpy(u->host, s, hl);
    s += hl;
    if (*s == ':') {
        long p = strtol(s + 1, (char **)&s, 10);
        if (p <= 0 || p > 65535) return false;
        u->port = (uint16_t)p;
    }
    /* "host:80abc" must not yield the relative request target "abc". */
    if (*s && *s != '/' && *s != '?') return false;
    int n = *s == '?' ? snprintf(u->path, sizeof u->path, "/%s", s)
                      : snprintf(u->path, sizeof u->path, "%s", *s ? s : "/");
    /* A prefix would request a different resource, not the original URL. */
    return n >= 0 && (size_t)n < sizeof u->path;
}

void url_owned_free(struct url_owned *u) {
    if (!u) return;
    free(u->host); free(u->path); memset(u, 0, sizeof *u);
}

static enum http_url_result url_parse_owned_status(const char *s, size_t length, struct url_owned *u) {
    if (!u) return HTTP_URL_INVALID;
    memset(u, 0, sizeof *u);
    if (!s || !length || length > UINT32_MAX || length == SIZE_MAX || memchr(s, 0, length)) return HTTP_URL_INVALID;
    const char *end = s + length, *authority = s;
    bool tls = length >= 8 && !strncasecmp(s, "https://", 8);
    if (tls) authority += 8;
    else if (length >= 7 && !strncasecmp(s, "http://", 7)) authority += 7;
    else {
        /* Keep the legacy bare-host entry, but never interpret another
           explicit scheme as a host/port or permit request-line controls. */
        for (const char *p = s; (size_t)(end - p) > 2; p++)
            if (p[0] == ':' && p[1] == '/' && p[2] == '/') return HTTP_URL_INVALID;
    }
    for (const unsigned char *p = (const unsigned char *)s; p < (const unsigned char *)end; p++)
        if (*p <= 32 || *p == 127 || *p == '\\') return HTTP_URL_INVALID;
    const char *tail = authority;
    while (tail < end && *tail != '/' && *tail != '?' && *tail != '#') tail++;
    const char *colon = NULL;
    for (const char *p = authority; p < tail; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ':') { if (colon) return HTTP_URL_INVALID; colon = p; }
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) return HTTP_URL_INVALID;
    }
    size_t host_length = (size_t)((colon ? colon : tail) - authority);
    if (!host_length || host_length == SIZE_MAX) return HTTP_URL_INVALID;
    unsigned port = tls ? 443 : 80;
    if (colon) {
        if (colon + 1 == tail) return HTTP_URL_INVALID;
        port = 0;
        for (const char *p = colon + 1; p < tail; p++) {
            if (*p < '0' || *p > '9' || port > (65535u - (unsigned)(*p - '0')) / 10u) return HTTP_URL_INVALID;
            port = port * 10u + (unsigned)(*p - '0');
        }
        if (!port) return HTTP_URL_INVALID;
    }
    const char *path_end = tail;
    while (path_end < end && *path_end != '#') path_end++;
    size_t supplied = (size_t)(path_end - tail);
    bool prepend = !supplied || *tail == '?';
    if (supplied > SIZE_MAX - (size_t)prepend - 1) return HTTP_URL_INVALID;
    size_t path_length = supplied + (size_t)prepend;
    char *host = malloc(host_length + 1), *path = malloc(path_length + 1);
    if (!host || !path) { free(host); free(path); return HTTP_URL_OOM; }
    for (size_t i = 0; i < host_length; i++) {
        unsigned char c = (unsigned char)authority[i];
        host[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
    }
    host[host_length] = 0;
    size_t at = 0; if (prepend) path[at++] = '/';
    memcpy(path + at, tail, supplied); path[path_length] = 0;
    u->tls = tls; u->port = (uint16_t)port; u->host = host; u->path = path;
    return HTTP_URL_TUPLE;
}

enum http_url_result url_parse_owned_result_n(const char *s, size_t length, struct url_owned *u) {
    return url_parse_owned_status(s, length, u);
}

bool url_parse_owned_n(const char *s, size_t length, struct url_owned *u) {
    return url_parse_owned_result_n(s, length, u) == HTTP_URL_TUPLE;
}

bool url_parse_owned(const char *s, struct url_owned *u) {
    return url_parse_owned_n(s, s ? strlen(s) : 0, u);
}

enum http_url_result http_canonical_owned_n(const char *raw, size_t length, char **canonical, char **origin) {
    if (canonical) *canonical = NULL;
    if (!origin) return HTTP_URL_INVALID;
    *origin = NULL;
    if (!raw || !length || length > UINT32_MAX || length == SIZE_MAX || memchr(raw, 0, length)) return HTTP_URL_INVALID;
    bool secure = length >= 8 && !strncasecmp(raw, "https://", 8);
    bool plain = length >= 7 && !strncasecmp(raw, "http://", 7);
    if (!secure && !plain) {
        if ((length >= 6 && !strncasecmp(raw, "https:", 6)) ||
            (length >= 5 && !strncasecmp(raw, "http:", 5))) return HTTP_URL_INVALID;
        /* Non-HTTP URL parsing/inherited blob origin belongs to its caller.
           No tuple, equality permission or empty serialization is invented. */
        return HTTP_URL_OPAQUE;
    }
    struct url_owned parsed;
    enum http_url_result result = url_parse_owned_status(raw, length, &parsed);
    if (result != HTTP_URL_TUPLE) return result;
    char port[8] = "";
    if (parsed.port != (parsed.tls ? 443 : 80)) snprintf(port, sizeof port, ":%u", parsed.port);
    const char *scheme = parsed.tls ? "https://" : "http://";
    size_t sl = strlen(scheme), hl = strlen(parsed.host), pl = strlen(port);
    if (hl > SIZE_MAX - sl - pl - 1) { url_owned_free(&parsed); return HTTP_URL_INVALID; }
    size_t ol = sl + hl + pl, path_length = strlen(parsed.path);
    if (canonical && (path_length > SIZE_MAX - ol - 1 ||
                      ol > UINT32_MAX || path_length > UINT32_MAX - ol)) { url_owned_free(&parsed); return HTTP_URL_INVALID; }
    char *o = malloc(ol + 1), *c = canonical ? malloc(ol + path_length + 1) : NULL;
    if (!o || (canonical && !c)) { free(o); free(c); url_owned_free(&parsed); return HTTP_URL_OOM; }
    memcpy(o, scheme, sl); memcpy(o + sl, parsed.host, hl); memcpy(o + sl + hl, port, pl + 1);
    if (c) { memcpy(c, o, ol); memcpy(c + ol, parsed.path, path_length + 1); }
    url_owned_free(&parsed); *origin = o; if (canonical) *canonical = c;
    return HTTP_URL_TUPLE;
}

enum http_url_result http_origin_owned_n(const char *raw, size_t length, char **origin) {
    return http_canonical_owned_n(raw, length, NULL, origin);
}

enum http_url_result http_origin_owned(const char *raw, char **origin) {
    return http_origin_owned_n(raw, raw ? strlen(raw) : 0, origin);
}

/* buffered reader on top of a stream */
struct rd {
    net_stream *s;
    struct http2_stream *h2;
    struct http_transport_result *result;
    char buf[4096];
    size_t pos, len;
    char *line;
    size_t line_capacity;
    uint64_t filled_at; /* when the bytes now buffered arrived */
    bool eof, err, line_invalid, line_oom;
};

static bool rd_fill(struct rd *r) {
    if (r->pos < r->len) return true;
    if (r->eof || r->err) return false;
    long n = r->h2?http2_read(r->h2,r->buf,sizeof r->buf):ns_read(r->s, r->buf, sizeof r->buf);
    if(n>0&&r->result&&!r->h2)r->result->response_bytes_received+=(size_t)n;
    if (n < 0) r->err = true;
    if (n <= 0) {
        r->eof = true;
        return false;
    }
    r->pos = 0;
    r->len = (size_t)n;
    r->filled_at = uptime_ms();
    return true;
}

/* read a line without the CRLF; false at end of stream */
static bool rd_line(struct rd *r, char **out) {
    size_t k = 0;
    r->line_invalid = r->line_oom = false;
    if (!r->line) {
        r->line = malloc(256);
        if (!r->line) { r->line_oom = true; *out = NULL; return false; }
        r->line_capacity = 256;
    }
    *out = r->line;
    bool cr = false;
    for (;;) {
        if (!rd_fill(r)) {
            r->line[k] = 0;
            return false; /* a partial line is not a complete HTTP field */
        }
        char c = r->buf[r->pos++];
        if (c == '\n') break;
        if (cr || ((unsigned char)c < 32 && c != '\r' && c != '\t') || (unsigned char)c == 127) r->line_invalid = true;
        cr = c == '\r';
        if (c != '\r') {
            if (k + 1 == r->line_capacity) {
                if (r->line_capacity == SIZE_MAX) { r->line_oom = true; return false; }
                size_t capacity = r->line_capacity > SIZE_MAX / 2 ? SIZE_MAX : r->line_capacity * 2;
                char *grown = realloc(r->line, capacity);
                if (!grown) { r->line_oom = true; return false; }
                r->line = *out = grown; r->line_capacity = capacity;
            }
            r->line[k++] = c;
        }
    }
    r->line[k] = 0;
    return true;
}
static void rd_free(struct rd *r) { free(r->line); free(r); }

struct sink {
    const struct http_req *rq;
    struct http_resp *rs;
    size_t cap;
    bool aborted;
    size_t limit; /* encoded/decoded bound, or zero for legacy uncompressed streaming */
    struct http_coding *coding; /* the content codings to undo, the last applied first */
    size_t codings;
};

static bool sink_plain_put(struct sink *k, const char *data, size_t n) {
    if (n >= (size_t)-1 - k->rs->body_len ||
        (k->limit && (k->rs->body_len > k->limit || n > k->limit - k->rs->body_len))) {
        snprintf(k->rs->error, sizeof k->rs->error, "response exceeds size limit");
        errno=EFBIG;
        k->aborted = true;
        return false;
    }
    if (k->rq->on_body) {
        if (k->rq->on_body(k->rq->ctx, data, n) < 0) {
            errno=ECANCELED;
            k->aborted = true;
            return false;
        }
        k->rs->body_len += n;
        return true;
    }
    if (k->rs->body_len + n + 1 > k->cap) {
        size_t cap = k->cap ? k->cap : 8192;
        while (cap < k->rs->body_len + n + 1) {
            if (cap > (size_t)-1 / 2) { cap = k->rs->body_len + n + 1; break; }
            cap *= 2;
        }
        if (k->limit && cap > k->limit + 1) cap = k->limit + 1;
        char *nb = realloc(k->rs->body, cap);
        if (!nb) {
            errno=ENOMEM;
            snprintf(k->rs->error, sizeof k->rs->error, "out of memory");
            k->aborted = true;
            return false;
        }
        k->rs->body = nb;
        k->cap = cap;
    }
    memcpy(k->rs->body + k->rs->body_len, data, n);
    k->rs->body_len += n;
    k->rs->body[k->rs->body_len] = 0;
    return true;
}
#include "http_coding.h"
static bool sink_put(struct sink *k,const char *data,size_t n){
    return coding_put(k,0,data,n);
}

/* copy exactly n bytes (or until the end of stream when n == SIZE_MAX) to the sink */
static bool copy_body(struct rd *r, struct sink *k, size_t n) {
    while (n) {
        if (!rd_fill(r)) return n == (size_t)-1 && !r->err;
        size_t take = r->len - r->pos;
        if (take > n) take = n;
        /* Fetch's encoded size: the body's bytes as transmitted. */
        if (r->result) r->result->encoded_body_size += take;
        if (!sink_put(k, r->buf + r->pos, take)) return false;
        r->pos += take;
        if (n != (size_t)-1) n -= take;
    }
    return true;
}

const char *http_response_headers(const struct http_resp *resp) {
    return resp->headers_full ? resp->headers_full : resp->headers;
}
const char *http_response_status_text(const struct http_resp *resp) {
    return resp->status_text_full ? resp->status_text_full : resp->status_text;
}

char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n) {
    if (!n) return NULL;
    size_t nl = strlen(name);
    const char *p = http_response_headers(resp);
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(eol - v);
            if (vl >= n) vl = n - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return out;
        }
        p = *eol ? eol + 2 : eol;
    }
    return NULL;
}

/* Framing is parsed from each complete line, independently of storage. */
static bool valid_header_line(const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon || colon == line) return false;
    for (const char *p = line; p < colon; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (!strchr("!#$%&'*+-.^_`|~", c)) return false;
    }
    return true;
}
static char *header_value(char *line, const char *name) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return NULL;
    char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    char *end = v + strlen(v);
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return v;
}

static bool body_size(const char *p, int base, size_t *out, bool chunk) {
    size_t n = 0;
    bool any = false;
    for (;;) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' :
                *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
                *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
        if (d < 0 || d >= base) break;
        if (n > ((size_t)-2 - (size_t)d) / (size_t)base) return false;
        n = n * (size_t)base + (size_t)d;
        any = true; p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!any || (*p && !(chunk && *p == ';'))) return false;
    *out = n;
    return true;
}

#define HTTP_DECODED_LIMIT ((size_t)UINT32_MAX)

/* A body read whole before its codings are undone: the coded bytes in rs->body
   become the decoded ones, or none reach the caller when a coding is bad. */
static bool decode_whole(struct sink *k, const struct http_req *buffered, size_t limit) {
    char *coded = k->rs->body;
    size_t coded_len = k->rs->body_len;
    k->rs->body = NULL;
    k->rs->body_len = 0;
    struct sink d = {buffered, k->rs, 0, false, limit, k->coding, k->codings};
    bool ok = coding_put(&d, 0, coded, coded_len) && coding_finish(&d);
    free(coded);
    return ok;
}

struct http_scratch {
    struct url_owned url;
    char *head;
    size_t head_length;
    struct pooled_connection *connection;
    struct http2_stream *h2;
    struct http_transport_result *result;
    bool reusable;
    bool request_close;
};
static bool has_token(const char *value,const char *token) {
    size_t n=strlen(token);
    while(*value) {
        while(*value==','||*value==' '||*value=='\t')value++;
        const char *end=value;while(*end&&*end!=',')end++;
        const char *last=end;while(last>value&&(last[-1]==' '||last[-1]=='\t'))last--;
        if((size_t)(last-value)==n&&!strncasecmp(value,token,n))return true;
        value=end;
    }
    return false;
}
static const char *request_head(const struct http_req *rq, struct http_scratch *work) {
    const char *method = rq->method ? rq->method : "GET";
    if (!*method) return "invalid HTTP method";
    /* HTTP method = token. Browser-only forbidden methods are enforced by
       WebNET; the general HTTP client still has to reject request-line bytes. */
    for (const unsigned char *p = (const unsigned char *)method; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              strchr("!#$%&'*+-.^_`|~", *p))) return "invalid HTTP method";
    struct url_owned *u = &work->url;
    char port[8] = "", content_length[64] = "";
    if (u->port != (u->tls ? 443 : 80)) snprintf(port, sizeof port, ":%u", u->port);
    if (rq->body_len && !rq->body) return "missing HTTP request body storage";
    if(rq->headers)for(const char *p=rq->headers;*p;) {
        const char *end=strstr(p,"\r\n"),*colon=strchr(p,':');
        if(!end||!colon||colon==p||colon>end)return "invalid request header field";
        for(const char *q=p;q<colon;q++)if(!isalnum((unsigned char)*q)&&!strchr("!#$%&'*+-.^_`|~",*q))return "invalid request header name";
        for(const char *q=colon+1;q<end;q++)if((*q<32&&*q!='\t')||(unsigned char)*q==127)return "invalid request header value";
        size_t name=(size_t)(colon-p);
        if((name==14&&!strncasecmp(p,"Content-Length",name))||
           (name==17&&!strncasecmp(p,"Transfer-Encoding",name))||
           (name==4&&!strncasecmp(p,"Host",name)))return "caller supplied transport framing header";
        if(name==10&&!strncasecmp(p,"Connection",name)) {
            const char *v=colon+1;
            while(v<end){while(v<end&&(*v==','||*v==' '||*v=='\t'))v++;
                const char *last=v;while(last<end&&*last!=',')last++;const char *trim=last;
                while(trim>v&&(trim[-1]==' '||trim[-1]=='\t'))trim--;
                if(trim-v==5&&!strncasecmp(v,"close",5))work->request_close=true;v=last;}
        }
        p=end+2;
    }
    /* Fetch HTTP-network-or-cache: absent POST/PUT body generates zero;
       other absent bodies must not become an allocated empty-body request. */
    if (rq->body || !strcmp(method, "POST") || !strcmp(method, "PUT"))
        snprintf(content_length, sizeof content_length, "Content-Length: %zu\r\n", rq->body_len);
    const char *parts[] = {method, " ", u->path, " HTTP/1.1\r\nHost: ", u->host, port,
        "\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\n", content_length,
        rq->headers ? rq->headers : "", "\r\n"};
    size_t length = 0;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        size_t n = strlen(parts[i]);
        if (n > SIZE_MAX - length - 1) return "HTTP request-line allocation size overflow";
        length += n;
    }
    if(length>(size_t)LONG_MAX)return "HTTP request header write size overflow";
    char *head = malloc(length + 1);
    if (!head) return "out of memory preparing HTTP request line";
    size_t at = 0;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        size_t n = strlen(parts[i]); memcpy(head + at, parts[i], n); at += n;
    }
    head[at] = 0; work->head = head; work->head_length = length;
    return NULL;
}
static int request_inner(const struct http_req *rq, struct http_resp *rs, struct http_scratch *work, size_t body_limit,
                         const struct http_stream_hooks *hooks,struct http_transport_timing *timing,
                         uint64_t *phase_started,const char *partition,bool prior) {
    memset(rs, 0, sizeof *rs);
    struct url_owned *u = &work->url;
    if (!url_parse_owned(rq->url, u)) {
        errno=EINVAL;
        snprintf(rs->error, sizeof rs->error, "invalid URL or URL allocation failure");
        return -1;
    }
    if(prior&&u->tls){errno=EINVAL;snprintf(rs->error,sizeof rs->error,"prior-knowledge entry requires an http URL");return -1;}
    const char *method = rq->method ? rq->method : "GET";
    const char *head_error = request_head(rq, work);
    if (head_error) { errno=EINVAL;snprintf(rs->error, sizeof rs->error, "%s", head_error); return -1; }
    /* This legacy API accepts signed int durations. Its default is the
       representation maximum, not an independent 60-second resource quota. */
    int timeout = rq->timeout_ms ? rq->timeout_ms : INT_MAX;
    work->connection=pool_acquire(rq,u,timeout,partition,prior,hooks,timing,work->result,rs->error,sizeof rs->error);
    if(!work->connection)return -1;
    net_stream *s=work->connection->stream;
    if(!work->connection->h2)ns_set_cancel(s,hooks?hooks->cancelled:NULL,rq->ctx);
    if(timing){timing->phase=HTTP_PHASE_HEADER;*phase_started=uptime_ms();}
    struct http_transport_result *result=work->result;
    if(result) {
        /* The connection's timing info as it opened (an earlier request's
           when reused). Without ALPN the exchange names its own protocol:
           cleartext prior knowledge is h2c, HTTP/1 its response's version. */
        ns_get_connection_timing(s,&result->connection);
        if(!result->connection.protocol[0])strcpy(result->connection.protocol,prior?"h2c":"http/1.1");
        result->request_start=uptime_ms();
    }

    /* One complete header block avoids a flush/TLS record and TCP send for
       each fragment. A short write must never be treated as a sent request. */
    bool ok;
    if(work->connection->h2) {
        work->h2=http2_start(work->connection->h2,rq,u,work->head,hooks,work->result,rs->error,sizeof rs->error);
        ok=work->h2!=NULL;
    }else {
        /* A failed TLS write might already have sent a prefix. Record the
           attempted header bytes conservatively, so POST is never replayed. */
        uint64_t before=ns_bytes_sent(s);
        ok = ns_write(s, work->head, work->head_length) == (long)work->head_length;
        if(work->result)work->result->request_bytes_sent+=(size_t)(ns_bytes_sent(s)-before);
    }
    for (size_t sent = 0; ok && !work->h2 && sent < rq->body_len;) {
        size_t count = rq->body_len - sent;
        if (count > 65536u) count = 65536u;
        uint64_t before=ns_bytes_sent(s);
        ok = ns_write(s, (const char *)rq->body + sent, count) == (long)count;
        if(work->result)work->result->request_bytes_sent+=(size_t)(ns_bytes_sent(s)-before);
        sent += count;
        if (ok && hooks && hooks->upload) ok = hooks->upload(rq->ctx, sent, rq->body_len) >= 0;
    }
    if (ok && !rq->body_len && hooks && hooks->upload) ok = hooks->upload(rq->ctx, 0, 0) >= 0;
    if (!ok) {
        snprintf(rs->error, sizeof rs->error, "send: %s", ns_error(s));
        return -1;
    }

    struct rd *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(rs->error, sizeof rs->error, "out of memory");
        return -1;
    }
    r->s = s;r->h2=work->h2;r->result=work->result;
    char *line = NULL;
    int status = 0;
    int major=0,minor=0;
    /* All informational responses precede the final response. 101 changes
       protocols and cannot be handed back as a reusable HTTP connection. */
    do {
        /* Fetch: each response starts as its first byte reaches the parser. */
        uint64_t response_started = rd_fill(r) ? r->filled_at : 0;
        if (!rd_line(r, &line) || r->line_invalid || sscanf(line, "HTTP/%d.%d %d", &major,&minor,&status) != 3 ||
            major!=1||(minor!=0&&minor!=1)||status<100||status>599||status==101) {
            if(r->eof&&!r->err)errno=ECONNRESET;else if(!r->err)errno=EPROTO;
            const char *detail=work->h2?http2_error(work->h2):ns_error(s);
            snprintf(rs->error, sizeof rs->error, "no HTTP response (%s)", r->line_oom ? "out of memory reading line" : detail[0] ? detail : "connection closed");
            rd_free(r);
            return -1;
        }
        if (result) {
            result->final_response_start = response_started;
            if (status < 200 && !result->first_interim_response_start) result->first_interim_response_start = response_started;
            if (!work->h2 && minor == 0 && !strcmp(result->connection.protocol, "http/1.1")) strcpy(result->connection.protocol, "http/1.0");
        }
        if (status < 200) {
            size_t interim_len = 0;
            for (;;) {
                if (!rd_line(r, &line) || r->line_invalid) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response headers");
                    errno=EPROTO;rd_free(r); return -1;
                }
                if (!line[0]) break;
                if (!valid_header_line(line)) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response header field");
                    errno=EPROTO;rd_free(r); return -1;
                }
                size_t l = strlen(line);
                if (interim_len > SIZE_MAX - 2 || l > SIZE_MAX - interim_len - 2) {
                    snprintf(rs->error, sizeof rs->error, "response header length overflow");
                    errno=EOVERFLOW;rd_free(r); return -1;
                }
                interim_len += l + 2;
            }
        }
    } while (status < 200);
    if(timing)timing->first_header_ms=uptime_ms()-*phase_started;
    rs->status = status;
    const char *reason = strchr(line, ' ');
    if (reason) {
        while (*reason == ' ') reason++;
        while (*reason >= '0' && *reason <= '9') reason++;
        if (*reason == ' ') reason++;
        size_t reason_len = strlen(reason);
        if (reason_len >= sizeof rs->status_text) {
            rs->status_text_full = malloc(reason_len + 1);
            if (!rs->status_text_full) {
                snprintf(rs->error, sizeof rs->error, "out of memory storing HTTP reason phrase");
                errno=ENOMEM;rd_free(r); return -1;
            }
            memcpy(rs->status_text_full, reason, reason_len + 1);
        } else {
            memcpy(rs->status_text, reason, reason_len + 1);
        }
    }
    size_t hl = 0, headers_cap = sizeof rs->headers, length = 0;
    bool chunked = false, have_length = false;
    bool connection_close=false,connection_keepalive=false;
    char *encoding = NULL; /* every Content-Encoding field's value, joined by commas */
    size_t encoding_len = 0;
    const char *header_error = NULL;
    for (;;) {
        if (!rd_line(r, &line)) { header_error = r->line_oom ? "out of memory reading response header" : "incomplete response headers"; break; }
        if (r->line_invalid) { header_error = "invalid response header characters"; break; }
        if (!line[0]) break;
        if (!valid_header_line(line)) { header_error = "invalid response header field"; break; }
        size_t l = strlen(line);
        /* Store every field or reject the response. Late security fields must
           never disappear behind the old inline capacity. */
        if (hl > SIZE_MAX - 3 || l > SIZE_MAX - hl - 3) {
            header_error = "response header length overflow"; break;
        }
        size_t need = hl + l + 3;
        if (need > headers_cap) {
            size_t cap = headers_cap;
            while (cap < need) {
                cap = cap > SIZE_MAX / 2 ? need : cap * 2u;
            }
            bool had_full = rs->headers_full != NULL;
            char *larger = realloc(rs->headers_full, cap);
            if (!larger) { header_error = "out of memory receiving response headers"; break; }
            if (!had_full) memcpy(larger, rs->headers, hl + 1);
            rs->headers_full = larger; headers_cap = cap;
        }
        char *stored = rs->headers_full ? rs->headers_full : rs->headers;
        memcpy(stored + hl, line, l);
        memcpy(stored + hl + l, "\r\n", 3);
        hl += l + 2;
        if (rq->on_header && rq->on_header(rq->ctx, line, l) < 0) { header_error = "response header callback aborted"; break; }
        char *v;
        if ((v = header_value(line, "Transfer-Encoding"))) {
            if (chunked || strcasecmp(v, "chunked"))
                header_error = "unsupported or invalid Transfer-Encoding";
            chunked = true;
        } else if ((v = header_value(line, "Content-Length"))) {
            size_t parsed = 0;
            if (!body_size(v, 10, &parsed, false) || (have_length && parsed != length))
                header_error = "invalid Content-Length";
            length = parsed;
            have_length = true;
        } else if ((v = header_value(line, "Content-Encoding"))) {
            size_t vl = strlen(v);
            char *joined = realloc(encoding, encoding_len + vl + 2);
            if (!joined) header_error = "out of memory receiving response headers";
            else {
                encoding = joined;
                if (encoding_len) encoding[encoding_len++] = ',';
                memcpy(encoding + encoding_len, v, vl + 1);
                encoding_len += vl;
            }
        } else if ((v = header_value(line,"Connection"))) {
            connection_close|=has_token(v,"close");connection_keepalive|=has_token(v,"keep-alive");
        }
        if (header_error) break;
    }
    if(chunked&&have_length)header_error="ambiguous Transfer-Encoding and Content-Length";
    if (header_error) {
        snprintf(rs->error, sizeof rs->error, "%s", header_error);
        free(rs->headers_full); rs->headers_full = NULL; rs->headers[0] = 0;
        free(encoding);errno=EPROTO;rd_free(r); return -1;
    }
    rs->headers_len = hl;
    if (rs->headers_full) rs->headers[0] = 0;
    if(timing){uint64_t now=uptime_ms();timing->header_ms=now-*phase_started;
        *phase_started=now;timing->phase=HTTP_PHASE_BODY;}
    if (hooks && hooks->headers) {
        int decision = hooks->headers(rq->ctx, rs);
        if (decision) {
            if (decision < 0 && !rs->error[0]) snprintf(rs->error, sizeof rs->error, "response headers rejected");
            if(decision<0)errno=ECANCELED;
            free(encoding);rd_free(r); return decision < 0 ? -1 : 0;
        }
    }

    struct http_req buffered = *rq;
    buffered.on_body = NULL;
    bool no_body = !strcmp(method, "HEAD") || status == 204 || status == 304;
    size_t decoded_limit = body_limit ? body_limit : HTTP_DECODED_LIMIT;
    struct sink k = {rq, rs, 0, false, body_limit, NULL, 0};
    char filtered_coding[HTTP_CONTENT_ENCODING_MAX];
    coding_filtered(encoding, filtered_coding);
    int coded = encoding && !no_body ? coding_create(&k, encoding) : 0;
    free(encoding);
    if (coded < 0) {
        coding_free(&k);errno=ENOMEM;snprintf(rs->error,sizeof rs->error,"out of memory decoding the response");
        rd_free(r);return -1;
    }
    /* With hooks the caller takes the body as it is decoded. Without them a coded
       body is read whole and decoded before on_body sees a byte, so a bad
       checksum or an overlong body delivers nothing. */
    bool whole = coded && !(hooks && rq->on_body);
    struct http_coding *codings = k.coding;
    size_t coding_count = k.codings;
    if (coded) k.limit = decoded_limit;
    if (whole) { k.rq = &buffered; k.coding = NULL; k.codings = 0; }
    bool body_ok;
    if (no_body) {
        body_ok = true;
    } else if (chunked) {
        body_ok = false;
        for (;;) {
            size_t n;
            if (!rd_line(r, &line) || r->line_invalid || !body_size(line, 16, &n, true)) break;
            if (n == 0) {
                while (rd_line(r, &line)) {
                    if (r->line_invalid || (line[0] && !valid_header_line(line))) break;
                    if (!line[0]) { body_ok = true; break; }
                }
                break;
            }
            if (!copy_body(r, &k, n)) break;
            if (!rd_line(r, &line) || line[0] || r->line_invalid) break;
        }
    } else if (have_length) {
        body_ok = copy_body(r, &k, length);
    } else {
        body_ok = copy_body(r, &k, (size_t)-1);
    }
    if(body_ok&&coded&&!whole)body_ok=coding_finish(&k);
    if(body_ok&&work->h2&&http2_finish(work->h2,true,hooks)<0) {
        body_ok=false;snprintf(rs->error,sizeof rs->error,"%s",http2_error(work->h2));
    }
    k.coding = codings; k.codings = coding_count;
    if (body_ok && whole) {
        body_ok = decode_whole(&k, &buffered, decoded_limit);
        if (body_ok && rq->on_body) {
            char *body = rs->body;
            size_t len = rs->body_len;
            rs->body = NULL; rs->body_len = 0;
            k.rq = rq;
            body_ok = sink_plain_put(&k, body, len);
            free(body);
        }
    }
    coding_free(&k);
    /* Set once a body byte was transmitted (HTTP-network fetch). */
    if (result && result->encoded_body_size) memcpy(result->content_encoding, filtered_coding, sizeof filtered_coding);
    if (!rs->body && !rq->on_body) {
        rs->body = calloc(1, 1);
    }
    int ret = 0;
    if (!body_ok && !k.aborted) {
        const char *detail = work->h2 ? http2_error(work->h2) : ns_error(s);
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "response cut short (%s)", r->line_oom ? "out of memory reading line" : detail[0] ? detail : "connection closed");
        ret = -1;
    } else if (k.aborted) {
        if(!errno)errno=ECANCELED;
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "aborted");
        ret = -1;
    }
    work->reusable=ret==0&&!work->request_close&&!connection_close&&(minor==1||connection_keepalive)&&
        (no_body||chunked||have_length)&&r->pos==r->len&&!r->err&&!r->eof;
    if(work->h2)work->reusable=ret==0;
    rd_free(r);
    if (ret < 0) {
        free(rs->headers_full); rs->headers_full = NULL;
        rs->headers[0] = 0; rs->headers_len = 0;
    }
    return ret;
}

static int request_with_limit(const struct http_req *rq, struct http_resp *rs, size_t body_limit,
                              const struct http_stream_hooks *hooks,struct http_transport_timing *timing,
                              const char *partition,struct http_transport_result *result,bool prior) {
    if(timing)memset(timing,0,sizeof *timing);
    struct http_transport_result local_result={0};
    if(!result)result=&local_result;else memset(result,0,sizeof *result);
    struct http_transport_timing local_timing={0};if(!timing)timing=&local_timing;
    struct http_scratch *work = calloc(1, sizeof *work);
    if (!work) {
        memset(rs, 0, sizeof *rs);
        snprintf(rs->error, sizeof rs->error, "out of memory");
        return -1;
    }
    uint64_t phase_started=0;
    work->result=result;errno=0;
    int code = request_inner(rq, rs, work, body_limit, hooks,timing,&phase_started,partition,prior);
    int cause=errno;
    if(work->h2)http2_release(work->h2);
    pool_release(work->connection,work->reusable);
    /* An idle peer can close between checkout and the first read. Retry only
       one idempotent request, before any response byte or callback exists. */
    const char *method=rq->method?rq->method:"GET";
    bool idempotent=!strcmp(method,"GET")||!strcmp(method,"HEAD")||!strcmp(method,"OPTIONS")||
        !strcmp(method,"PUT")||!strcmp(method,"DELETE")||!strcmp(method,"TRACE");
    if(code<0&&result->reused&&!result->response_bytes_received&&idempotent&&
        (cause==ECONNRESET||cause==EPIPE||cause==ENOTCONN||cause==EIO||
         (cause==EAGAIN&&result->request_unprocessed))) {
        free(work->head);url_owned_free(&work->url);http_resp_free(rs);
        memset(work,0,sizeof *work);memset(result,0,sizeof *result);work->result=result;errno=0;
        code=request_inner(rq,rs,work,body_limit,hooks,timing,&phase_started,partition,prior);cause=errno;
        if(work->h2)http2_release(work->h2);pool_release(work->connection,work->reusable);
    }
    if(timing){
        if(timing->phase==HTTP_PHASE_HEADER)timing->header_ms=uptime_ms()-phase_started;
        else if(timing->phase==HTTP_PHASE_BODY)timing->body_ms=uptime_ms()-phase_started;
        if(code==0)timing->phase=HTTP_PHASE_DONE;
    }
    result->phase=timing->phase;
    if(code<0) {
        if(cause==ECANCELED)result->error=HTTP_ERR_CANCELLED;
        else if(cause==ENOMEM)result->error=HTTP_ERR_NO_MEMORY;
        else if(cause==EAGAIN&&result->request_unprocessed)result->error=HTTP_ERR_UNPROCESSED;
        else if(cause==ETIMEDOUT||cause==EAGAIN)result->error=timing->phase==HTTP_PHASE_RESOLVE?HTTP_ERR_DNS_TEMP:HTTP_ERR_TIMEOUT;
        else if(cause==ENOENT)result->error=HTTP_ERR_DNS_NOT_FOUND;
        else if(cause==EACCES&&timing->phase==HTTP_PHASE_TLS)result->error=HTTP_ERR_TLS_VERIFY;
        else if(cause==ECONNRESET||cause==EPIPE||cause==ENOTCONN)result->error=HTTP_ERR_RESET;
        else if(cause==EPROTO||cause==EINVAL||cause==EOVERFLOW)result->error=HTTP_ERR_PROTOCOL;
        else if(timing->phase==HTTP_PHASE_CONNECT)result->error=HTTP_ERR_CONNECT;
        else if(timing->phase==HTTP_PHASE_RESOLVE)result->error=HTTP_ERR_DNS_TEMP;
        else result->error=HTTP_ERR_IO;
    }
    free(work->head);
    url_owned_free(&work->url);
    free(work);
    errno=cause;return code;
}
int http_request(const struct http_req *rq, struct http_resp *rs) {
    return request_with_limit(rq, rs, 0, NULL,NULL,NULL,NULL,false);
}
int http_request_limited(const struct http_req *rq, struct http_resp *rs, size_t body_limit) {
    if (!body_limit || body_limit > HTTP_RESPONSE_BODY_MAX) {
        memset(rs, 0, sizeof *rs);
        snprintf(rs->error, sizeof rs->error, "invalid finite response size limit");
        return -1;
    }
    return request_with_limit(rq, rs, body_limit, NULL,NULL,NULL,NULL,false);
}
int http_request_stream(const struct http_req *rq, struct http_resp *rs, size_t limit,
                        const struct http_stream_hooks *hooks) {
    return request_with_limit(rq, rs, limit, hooks,NULL,NULL,NULL,false);
}
int http_request_stream_timed(const struct http_req *rq,struct http_resp *rs,size_t limit,
                              const struct http_stream_hooks *hooks,struct http_transport_timing *timing) {
    return request_with_limit(rq,rs,limit,hooks,timing,NULL,NULL,false);
}
int http_request_stream_ex(const struct http_req *rq,struct http_resp *rs,size_t limit,
    const struct http_stream_hooks *hooks,struct http_transport_timing *timing,
    const char *partition,struct http_transport_result *result) {
    return request_with_limit(rq,rs,limit,hooks,timing,partition,result,false);
}
int http_request_h2_prior_knowledge_ex(const struct http_req *rq,struct http_resp *rs,size_t limit,
    const struct http_stream_hooks *hooks,const char *partition,struct http_transport_result *result) {
    return request_with_limit(rq,rs,limit,hooks,NULL,partition,result,true);
}

void http_resp_free(struct http_resp *resp) {
    free(resp->status_text_full); resp->status_text_full = NULL; resp->status_text[0] = 0;
    free(resp->headers_full);
    resp->headers_full = NULL;
    resp->headers[0] = 0;
    resp->headers_len = 0;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
