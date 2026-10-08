/* HTTP/1.1 client: one request per connection ("Connection: close"), Content-Length, chunked and
   read-until-close bodies, over plain TCP or TLS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "http.h"

bool url_parse(const char *s, struct url *u) {
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

/* buffered reader on top of a stream */
struct rd {
    net_stream *s;
    char buf[4096];
    size_t pos, len;
    bool eof, err, line_truncated, line_invalid;
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
static bool rd_line(struct rd *r, char *out, size_t n) {
    size_t k = 0;
    r->line_truncated = r->line_invalid = false;
    bool cr = false;
    for (;;) {
        if (!rd_fill(r)) {
            out[k] = 0;
            return false; /* a partial line is not a complete HTTP field */
        }
        char c = r->buf[r->pos++];
        if (c == '\n') break;
        if (cr || ((unsigned char)c < 32 && c != '\r' && c != '\t') || (unsigned char)c == 127) r->line_invalid = true;
        cr = c == '\r';
        if (c != '\r') {
            if (k + 1 < n) out[k++] = c;
            else r->line_truncated = true;
        }
    }
    out[k] = 0;
    return true;
}

struct sink {
    const struct http_req *rq;
    struct http_resp *rs;
    size_t cap;
    bool aborted;
    size_t limit; /* only buffered compressed responses need this additional bound */
};

static bool sink_put(struct sink *k, const char *data, size_t n) {
    if (n >= (size_t)-1 - k->rs->body_len ||
        (k->limit && n > k->limit - k->rs->body_len)) {
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

#define HTTP_GZIP_LIMIT (16u * 1024u * 1024u)
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
static bool gunzip_body(struct http_resp *rs) {
    const unsigned char *in = (const unsigned char *)rs->body;
    size_t pos = 0, used = 0, cap = 65536;
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
            got = http_inflate(out + used, (int)(cap - used), (const char *)in + pos, (int)(n - pos), &took);
            if (got != -2) break;
            if (cap == HTTP_GZIP_LIMIT) {
                snprintf(rs->error, sizeof rs->error, "gzip response exceeds 16 MiB");
                free(out); return false;
            }
            cap *= 2;
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

int http_request(const struct http_req *rq, struct http_resp *rs) {
    memset(rs, 0, sizeof *rs);
    struct url u;
    if (!url_parse(rq->url, &u)) {
        snprintf(rs->error, sizeof rs->error, "bad URL: %s", rq->url);
        return -1;
    }
    int timeout = rq->timeout_ms ? rq->timeout_ms : 60000;
    net_stream *s = ns_open(u.host, u.port, u.tls, timeout, rs->error, sizeof rs->error);
    if (!s) return -1;

    const char *method = rq->method ? rq->method : "GET";
    bool std_port = u.port == (u.tls ? 443 : 80);
    char hostport[140], head[HTTP_URL_PATH_MAX + 512u];
    if (std_port) snprintf(hostport, sizeof hostport, "%s", u.host);
    else snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);
    int hn = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Nocturne/1.0\r\nConnection: close\r\n",
                      method, u.path, hostport);
    if (hn < 0 || hn >= (int)sizeof head - 2) {
        snprintf(rs->error, sizeof rs->error, "request too large");
        ns_close(s);
        return -1;
    }
    if (rq->body || strcmp(method, "GET"))
        hn += snprintf(head + hn, sizeof head - hn, "Content-Length: %zu\r\n", rq->body_len);
    if (hn < 0 || hn >= (int)sizeof head - 2) {
        snprintf(rs->error, sizeof rs->error, "request too large");
        ns_close(s);
        return -1;
    }
    bool ok = ns_write(s, head, (size_t)hn) >= 0 && (!rq->headers || ns_write(s, rq->headers, strlen(rq->headers)) >= 0) &&
              ns_write(s, "\r\n", 2) >= 0 && (!rq->body_len || ns_write(s, rq->body, rq->body_len) >= 0);
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
    char line[HTTP_RESPONSE_HEADER_LINE_MAX];
    int status = 0;
    /* status line, skipping any 100 Continue */
    do {
        if (!rd_line(r, line, sizeof line) || r->line_invalid || r->line_truncated || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
            snprintf(rs->error, sizeof rs->error, "no HTTP response (%s)", ns_error(s)[0] ? ns_error(s) : "connection closed");
            free(r);
            ns_close(s);
            return -1;
        }
        if (status == 100) {
            size_t interim_len = 0;
            for (;;) {
                if (!rd_line(r, line, sizeof line) || r->line_invalid || r->line_truncated) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response headers");
                    free(r); ns_close(s); return -1;
                }
                if (!line[0]) break;
                if (!valid_header_line(line)) {
                    snprintf(rs->error, sizeof rs->error, "invalid interim response header field");
                    free(r); ns_close(s); return -1;
                }
                size_t l = strlen(line);
                if (l + 2 > HTTP_RESPONSE_HEADERS_LIMIT - interim_len) {
                    snprintf(rs->error, sizeof rs->error, "response headers exceed 64 KiB limit");
                    free(r); ns_close(s); return -1;
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
        if (strlen(reason) >= sizeof rs->status_text) {
            snprintf(rs->error, sizeof rs->error, "HTTP reason phrase exceeds limit");
            free(r); ns_close(s); return -1;
        }
        memcpy(rs->status_text, reason, strlen(reason) + 1);
    }
    size_t hl = 0, headers_cap = sizeof rs->headers, length = 0;
    bool chunked = false, have_length = false, gzip = false, have_encoding = false;
    const char *header_error = NULL;
    for (;;) {
        if (!rd_line(r, line, sizeof line)) { header_error = "incomplete response headers"; break; }
        if (r->line_invalid) { header_error = "invalid response header characters"; break; }
        if (!line[0]) break;
        if (r->line_truncated) { header_error = "response header exceeds 16 KiB"; break; }
        if (!valid_header_line(line)) { header_error = "invalid response header field"; break; }
        size_t l = strlen(line);
        /* Store every field or reject the response. Late security fields must
           never disappear behind the old inline capacity. */
        if (l + 2 > HTTP_RESPONSE_HEADERS_LIMIT - hl) {
            header_error = "response headers exceed 64 KiB limit"; break;
        }
        size_t need = hl + l + 3;
        if (need > headers_cap) {
            size_t cap = headers_cap;
            while (cap < need) {
                cap = cap > (HTTP_RESPONSE_HEADERS_LIMIT + 1u) / 2u ?
                    HTTP_RESPONSE_HEADERS_LIMIT + 1u : cap * 2u;
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
            if (r->line_truncated || chunked || strcasecmp(v, "chunked"))
                header_error = "unsupported or invalid Transfer-Encoding";
            chunked = true;
        } else if ((v = header_value(line, "Content-Length"))) {
            size_t parsed = 0;
            if (r->line_truncated || !body_size(v, 10, &parsed, false) || (have_length && parsed != length))
                header_error = "invalid Content-Length";
            length = parsed;
            have_length = true;
        } else if ((v = header_value(line, "Content-Encoding"))) {
            if (r->line_truncated || have_encoding || (strcasecmp(v, "gzip") && strcasecmp(v, "identity")))
                header_error = "unsupported or invalid Content-Encoding";
            gzip = !strcasecmp(v, "gzip");
            have_encoding = true;
        }
        if (header_error) break;
    }
    if (header_error) {
        snprintf(rs->error, sizeof rs->error, "%s", header_error);
        free(rs->headers_full); rs->headers_full = NULL; rs->headers[0] = 0;
        free(r); ns_close(s); return -1;
    }
    rs->headers_len = hl;
    if (rs->headers_full) rs->headers[0] = 0;

    struct http_req buffered = *rq;
    buffered.on_body = NULL;
    bool no_body = !strcmp(method, "HEAD") || status == 204 || status == 304;
    struct sink k = {gzip ? &buffered : rq, rs, 0, false, gzip ? HTTP_GZIP_LIMIT : 0};
    bool body_ok;
    if (no_body) {
        body_ok = true;
    } else if (chunked) {
        body_ok = false;
        for (;;) {
            size_t n;
            if (!rd_line(r, line, sizeof line) || r->line_truncated || r->line_invalid || !body_size(line, 16, &n, true)) break;
            if (n == 0) {
                while (rd_line(r, line, sizeof line)) {
                    if (r->line_invalid || r->line_truncated) break;
                    if (!line[0]) { body_ok = true; break; }
                }
                break;
            }
            if (!copy_body(r, &k, n)) break;
            if (!rd_line(r, line, sizeof line) || line[0] || r->line_truncated || r->line_invalid) break;
        }
    } else if (have_length) {
        body_ok = copy_body(r, &k, length);
    } else {
        body_ok = copy_body(r, &k, (size_t)-1);
    }
    if (body_ok && gzip && !no_body) {
        body_ok = gunzip_body(rs);
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
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "response cut short (%s)", ns_error(s)[0] ? ns_error(s) : "connection closed");
        ret = -1;
    } else if (k.aborted) {
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "aborted");
        ret = -1;
    }
    free(r);
    ns_close(s);
    if (ret < 0) {
        free(rs->headers_full); rs->headers_full = NULL;
        rs->headers[0] = 0; rs->headers_len = 0;
    }
    return ret;
}

void http_resp_free(struct http_resp *resp) {
    free(resp->headers_full);
    resp->headers_full = NULL;
    resp->headers[0] = 0;
    resp->headers_len = 0;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
