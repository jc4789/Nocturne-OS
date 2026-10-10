/* HTTP/1.1 client: one request per connection ("Connection: close"), Content-Length, chunked and
   read-until-close bodies, over plain TCP or TLS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "http.h"
#include "browser_identity.h"

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
    char buf[4096];
    size_t pos, len;
    char *line;
    size_t line_capacity;
    bool eof, err, line_invalid, line_oom;
};

static bool rd_fill(struct rd *r) {
    if (r->pos < r->len) return true;
    if (r->eof || r->err) return false;
    long n = ns_read(r->s, r->buf, sizeof r->buf);
    if (n < 0) r->err = true;
    if (n <= 0) {
        r->eof = true;
        return false;
    }
    r->pos = 0;
    r->len = (size_t)n;
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
    struct http_gzip *gzip;
};

static bool sink_plain_put(struct sink *k, const char *data, size_t n) {
    if (n >= (size_t)-1 - k->rs->body_len ||
        (k->limit && (k->rs->body_len > k->limit || n > k->limit - k->rs->body_len))) {
        snprintf(k->rs->error, sizeof k->rs->error, "response exceeds size limit");
        k->aborted = true;
        return false;
    }
    if (k->rq->on_body) {
        if (k->rq->on_body(k->rq->ctx, data, n) < 0) {
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
#include "http_gzip.h"
static bool sink_put(struct sink *k,const char *data,size_t n){
    return k->gzip?gzip_feed(k->gzip,k,data,n,false):sink_plain_put(k,data,n);
}

/* copy exactly n bytes (or until the end of stream when n == SIZE_MAX) to the sink */
static bool copy_body(struct rd *r, struct sink *k, size_t n) {
    while (n) {
        if (!rd_fill(r)) return n == (size_t)-1 && !r->err;
        size_t take = r->len - r->pos;
        if (take > n) take = n;
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

#define HTTP_GZIP_LIMIT ((size_t)UINT32_MAX)
/* Private adapter to the existing image codec's bounded DEFLATE implementation. */
extern int http_inflate(char *out, int cap, const char *in, int len, int *consumed);

static uint32_t gzip_crc(const unsigned char *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Validate each gzip member, including optional headers, checksum and size.
   Concatenated members are permitted; unrelated trailing bytes are not. */
static bool gunzip_body(struct http_resp *rs, size_t limit) {
    const unsigned char *in = (const unsigned char *)rs->body;
    size_t pos = 0, used = 0, cap = limit < 65536 ? limit : 65536;
    char *out = malloc(cap + 1);
    if (!out) { snprintf(rs->error, sizeof rs->error, "out of memory"); return false; }
    do {
        size_t start = pos, n = rs->body_len;
        if (n - pos < 18 || in[pos] != 0x1f || in[pos+1] != 0x8b || in[pos+2] != 8 || (in[pos+3] & 0xe0)) goto invalid;
        unsigned flags = in[pos+3];
        pos += 10;
        if (flags & 4) {
            if (n - pos < 2) goto invalid;
            size_t extra = (size_t)in[pos] | (size_t)in[pos+1] << 8;
            pos += 2;
            if (extra > n - pos) goto invalid;
            pos += extra;
        }
        for (unsigned flag = 8; flag <= 16; flag <<= 1) {
            if (!(flags & flag)) continue;
            while (pos < n && in[pos]) pos++;
            if (pos == n) goto invalid;
            pos++;
        }
        if (flags & 2) {
            if (n - pos < 2 || (gzip_crc(in + start, pos - start) & 0xffff) !=
                ((unsigned)in[pos] | (unsigned)in[pos+1] << 8)) goto invalid;
            pos += 2;
        }
        if (n - pos < 8) goto invalid;
        int took = 0, got;
        for (;;) {
            size_t available = cap - used, encoded = n - pos;
            got = http_inflate(out + used, available > INT_MAX ? INT_MAX : (int)available,
                (const char *)in + pos, encoded > INT_MAX ? INT_MAX : (int)encoded, &took);
            if (got != -2) break;
            if (cap - used >= (size_t)INT_MAX) {
                snprintf(rs->error, sizeof rs->error, "gzip member length cannot be represented by the inflater API");
                free(out); return false;
            }
            if (cap == limit) {
                if (!(limit % (1024u * 1024u)))
                    snprintf(rs->error, sizeof rs->error, "gzip response exceeds %lu MiB", (unsigned long)(limit / (1024u * 1024u)));
                else snprintf(rs->error, sizeof rs->error, "gzip response exceeds %lu bytes", (unsigned long)limit);
                free(out); return false;
            }
            cap = cap * 2 < limit ? cap * 2 : limit;
            char *larger = realloc(out, cap + 1);
            if (!larger) { snprintf(rs->error, sizeof rs->error, "out of memory"); free(out); return false; }
            out = larger;
        }
        if (got < 0 || took <= 0 || (size_t)took > n - pos - 8) goto invalid;
        pos += (size_t)took;
        if (le32(in + pos) != gzip_crc((const unsigned char *)out + used, (size_t)got) ||
            le32(in + pos + 4) != (uint32_t)got) goto invalid;
        used += (size_t)got;
        pos += 8;
    } while (pos < rs->body_len);
    out[used] = 0;
    free(rs->body);
    rs->body = out;
    rs->body_len = used;
    return true;
invalid:
    snprintf(rs->error, sizeof rs->error, "invalid or truncated gzip response");
    free(out);
    return false;
}

struct http_scratch {
    struct url_owned url;
    char *head;
    size_t head_length;
};
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
    if (rq->body || strcmp(method, "GET"))
        snprintf(content_length, sizeof content_length, "Content-Length: %zu\r\n", rq->body_len);
    const char *parts[] = {method, " ", u->path, " HTTP/1.1\r\nHost: ", u->host, port,
        "\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\nConnection: close\r\n", content_length};
    size_t length = 0;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        size_t n = strlen(parts[i]);
        if (n > SIZE_MAX - length - 1) return "HTTP request-line allocation size overflow";
        length += n;
    }
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
                         const struct http_stream_hooks *hooks) {
    memset(rs, 0, sizeof *rs);
    struct url_owned *u = &work->url;
    if (!url_parse_owned(rq->url, u)) {
        snprintf(rs->error, sizeof rs->error, "invalid URL or URL allocation failure");
        return -1;
    }
    const char *method = rq->method ? rq->method : "GET";
    const char *head_error = request_head(rq, work);
    if (head_error) { snprintf(rs->error, sizeof rs->error, "%s", head_error); return -1; }
    /* This legacy API accepts signed int durations. Its default is the
       representation maximum, not an independent 60-second resource quota. */
    int timeout = rq->timeout_ms ? rq->timeout_ms : INT_MAX;
    net_stream *s = ns_open(u->host, u->port, u->tls, timeout, rs->error, sizeof rs->error);
    if (!s) return -1;

    bool ok = ns_write(s, work->head, work->head_length) >= 0 && (!rq->headers || ns_write(s, rq->headers, strlen(rq->headers)) >= 0) &&
              ns_write(s, "\r\n", 2) >= 0;
    for (size_t sent = 0; ok && sent < rq->body_len;) {
        size_t count = rq->body_len - sent;
        if (count > 65536u) count = 65536u;
        ok = ns_write(s, (const char *)rq->body + sent, count) >= 0;
        sent += count;
        if (ok && hooks && hooks->upload) ok = hooks->upload(rq->ctx, sent, rq->body_len) >= 0;
    }
    if (ok && !rq->body_len && hooks && hooks->upload) ok = hooks->upload(rq->ctx, 0, 0) >= 0;
    if (!ok) {
        snprintf(rs->error, sizeof rs->error, "send: %s", ns_error(s));
        ns_close(s);
        return -1;
    }

    struct rd *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(rs->error, sizeof rs->error, "out of memory");
        ns_close(s);
        return -1;
    }
    r->s = s;
    char *line = NULL;
    int status = 0;
    /* status line, skipping any 100 Continue */
    do {
        if (!rd_line(r, &line) || r->line_invalid || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
            snprintf(rs->error, sizeof rs->error, "no HTTP response (%s)", r->line_oom ? "out of memory reading line" : ns_error(s)[0] ? ns_error(s) : "connection closed");
            rd_free(r);
            ns_close(s);
            return -1;
        }
        if (status == 100) {
            size_t interim_len = 0;
            for (;;) {
                if (!rd_line(r, &line) || r->line_invalid) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response headers");
                    rd_free(r); ns_close(s); return -1;
                }
                if (!line[0]) break;
                if (!valid_header_line(line)) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response header field");
                    rd_free(r); ns_close(s); return -1;
                }
                size_t l = strlen(line);
                if (interim_len > SIZE_MAX - 2 || l > SIZE_MAX - interim_len - 2) {
                    snprintf(rs->error, sizeof rs->error, "response header length overflow");
                    rd_free(r); ns_close(s); return -1;
                }
                interim_len += l + 2;
            }
        }
    } while (status == 100);
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
                rd_free(r); ns_close(s); return -1;
            }
            memcpy(rs->status_text_full, reason, reason_len + 1);
        } else {
            memcpy(rs->status_text, reason, reason_len + 1);
        }
    }
    size_t hl = 0, headers_cap = sizeof rs->headers, length = 0;
    bool chunked = false, have_length = false, gzip = false, have_encoding = false;
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
            if (have_encoding || (strcasecmp(v, "gzip") && strcasecmp(v, "identity")))
                header_error = "unsupported or invalid Content-Encoding";
            gzip = !strcasecmp(v, "gzip");
            have_encoding = true;
        }
        if (header_error) break;
    }
    if (header_error) {
        snprintf(rs->error, sizeof rs->error, "%s", header_error);
        free(rs->headers_full); rs->headers_full = NULL; rs->headers[0] = 0;
        rd_free(r); ns_close(s); return -1;
    }
    rs->headers_len = hl;
    if (rs->headers_full) rs->headers[0] = 0;
    if (hooks && hooks->headers) {
        int decision = hooks->headers(rq->ctx, rs);
        if (decision) {
            if (decision < 0 && !rs->error[0]) snprintf(rs->error, sizeof rs->error, "response headers rejected");
            rd_free(r); ns_close(s); return decision < 0 ? -1 : 0;
        }
    }

    struct http_req buffered = *rq;
    buffered.on_body = NULL;
    bool no_body = !strcmp(method, "HEAD") || status == 204 || status == 304;
    size_t decoded_limit = body_limit ? body_limit : HTTP_GZIP_LIMIT;
    bool stream_gzip=gzip&&hooks&&rq->on_body&&!no_body;
    struct sink k = {gzip&&!stream_gzip ? &buffered : rq, rs, 0, false, gzip ? decoded_limit : body_limit,NULL};
    if(stream_gzip){k.gzip=gzip_create();if(!k.gzip){snprintf(rs->error,sizeof rs->error,"out of memory decoding gzip");rd_free(r);ns_close(s);return -1;}}
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
    if(body_ok&&stream_gzip)body_ok=gzip_feed(k.gzip,&k,NULL,0,true);
    free(k.gzip);k.gzip=NULL;
    if (body_ok && gzip && !stream_gzip && !no_body) {
        body_ok = gunzip_body(rs, decoded_limit);
        if (body_ok && rq->on_body) {
            char *body = rs->body;
            size_t len = rs->body_len;
            rs->body = NULL; rs->body_len = 0;
            k.rq = rq;
            body_ok = sink_put(&k, body, len);
            free(body);
        }
    }
    if (!rs->body && !rq->on_body) {
        rs->body = calloc(1, 1);
    }
    int ret = 0;
    if (!body_ok && !k.aborted) {
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "response cut short (%s)", r->line_oom ? "out of memory reading line" : ns_error(s)[0] ? ns_error(s) : "connection closed");
        ret = -1;
    } else if (k.aborted) {
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "aborted");
        ret = -1;
    }
    rd_free(r);
    ns_close(s);
    if (ret < 0) {
        free(rs->headers_full); rs->headers_full = NULL;
        rs->headers[0] = 0; rs->headers_len = 0;
    }
    return ret;
}

static int request_with_limit(const struct http_req *rq, struct http_resp *rs, size_t body_limit,
                              const struct http_stream_hooks *hooks) {
    struct http_scratch *work = calloc(1, sizeof *work);
    if (!work) {
        memset(rs, 0, sizeof *rs);
        snprintf(rs->error, sizeof rs->error, "out of memory");
        return -1;
    }
    int result = request_inner(rq, rs, work, body_limit, hooks);
    free(work->head);
    url_owned_free(&work->url);
    free(work);
    return result;
}
int http_request(const struct http_req *rq, struct http_resp *rs) {
    return request_with_limit(rq, rs, 0, NULL);
}
int http_request_limited(const struct http_req *rq, struct http_resp *rs, size_t body_limit) {
    if (!body_limit || body_limit > HTTP_RESPONSE_BODY_MAX) {
        memset(rs, 0, sizeof *rs);
        snprintf(rs->error, sizeof rs->error, "invalid finite response size limit");
        return -1;
    }
    return request_with_limit(rq, rs, body_limit, NULL);
}
int http_request_stream(const struct http_req *rq, struct http_resp *rs, size_t limit,
                        const struct http_stream_hooks *hooks) {
    return request_with_limit(rq, rs, limit, hooks);
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
