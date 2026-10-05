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
    if (*s == '?') snprintf(u->path, sizeof u->path, "/%s", s);
    else snprintf(u->path, sizeof u->path, "%s", *s ? s : "/");
    return true;
}

/* buffered reader on top of a stream */
struct rd {
    net_stream *s;
    char buf[4096];
    size_t pos, len;
    bool eof, err;
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
    for (;;) {
        if (!rd_fill(r)) {
            out[k] = 0;
            return k > 0;
        }
        char c = r->buf[r->pos++];
        if (c == '\n') break;
        if (c != '\r' && k + 1 < n) out[k++] = c;
    }
    out[k] = 0;
    return true;
}

struct sink {
    const struct http_req *rq;
    struct http_resp *rs;
    size_t cap;
    bool aborted;
};

static bool sink_put(struct sink *k, const char *data, size_t n) {
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
        while (cap < k->rs->body_len + n + 1) cap *= 2;
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
        if (!rd_fill(r)) return n == (size_t)-1;
        size_t take = r->len - r->pos;
        if (take > n) take = n;
        if (!sink_put(k, r->buf + r->pos, take)) return false;
        r->pos += take;
        if (n != (size_t)-1) n -= take;
    }
    return true;
}

char *http_header(const struct http_resp *resp, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    const char *p = resp->headers;
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
    char hostport[140], head[2048];
    if (std_port) snprintf(hostport, sizeof hostport, "%s", u.host);
    else snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);
    int hn = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Nocturne/1.0\r\nConnection: close\r\n",
                      method, u.path, hostport);
    if (rq->body || strcmp(method, "GET"))
        hn += snprintf(head + hn, sizeof head - hn, "Content-Length: %zu\r\n", rq->body_len);
    if (hn >= (int)sizeof head - 2) {
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
    r->s = s;
    char line[1024];
    int status = 0;
    /* status line, skipping any 100 Continue */
    do {
        if (!rd_line(r, line, sizeof line) || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
            snprintf(rs->error, sizeof rs->error, "no HTTP response (%s)", ns_error(s)[0] ? ns_error(s) : "connection closed");
            free(r);
            ns_close(s);
            return -1;
        }
        if (status == 100)
            while (rd_line(r, line, sizeof line) && line[0]) {
            }
    } while (status == 100);
    rs->status = status;
    size_t hl = 0;
    while (rd_line(r, line, sizeof line) && line[0]) {
        size_t l = strlen(line);
        if (hl + l + 3 < sizeof rs->headers) {
            memcpy(rs->headers + hl, line, l);
            memcpy(rs->headers + hl + l, "\r\n", 3);
            hl += l + 2;
        }
    }

    struct sink k = {rq, rs, 0, false};
    char v[64];
    bool body_ok;
    if (!strcmp(method, "HEAD") || status == 204 || status == 304) {
        body_ok = true;
    } else if (http_header(rs, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) {
        body_ok = false;
        for (;;) {
            if (!rd_line(r, line, sizeof line)) break;
            size_t n = strtoul(line, NULL, 16);
            if (n == 0) {
                body_ok = true;
                break;
            }
            if (!copy_body(r, &k, n)) break;
            rd_line(r, line, sizeof line); /* CRLF after the chunk */
        }
    } else if (http_header(rs, "Content-Length", v, sizeof v)) {
        body_ok = copy_body(r, &k, strtoul(v, NULL, 10));
    } else {
        body_ok = copy_body(r, &k, (size_t)-1);
    }
    if (!rs->body && !rq->on_body) {
        rs->body = calloc(1, 1);
    }
    int ret = 0;
    if (!body_ok && !k.aborted) {
        snprintf(rs->error, sizeof rs->error, "response cut short (%s)", ns_error(s)[0] ? ns_error(s) : "connection closed");
        ret = -1;
    } else if (k.aborted) {
        if (!rs->error[0]) snprintf(rs->error, sizeof rs->error, "aborted");
        ret = -1;
    }
    free(r);
    ns_close(s);
    return ret;
}

void http_resp_free(struct http_resp *resp) {
    free(resp->body);
    resp->body = NULL;
}
