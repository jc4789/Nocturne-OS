/* HTTP transport regressions, not website acceptance tests. ns_read is the
   narrow substituted boundary; the production HTTP parser/inflater are linked.
   Run inside Nocturne: tcc -o /home/httptest /data/tests/httptest.c && /home/httptest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"

struct net_stream { const unsigned char *wire; size_t length, position, split; bool fail; };
static struct net_stream stream;
static int checks, failed, callbacks;
static char callback_body[128];
static size_t callback_len;
static size_t cookie_header_len;
static int header_result;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL httptest:%d: %s\n", __LINE__, #c); failed++; } } while (0)

net_stream *ns_open(const char *host, uint16_t port, bool tls, int timeout, char *error, size_t n) {
    (void)host; (void)port; (void)tls; (void)timeout; (void)error; (void)n;
    stream.position = 0; return &stream;
}
long ns_write(net_stream *s, const void *data, size_t n) { (void)s; (void)data; return (long)n; }
long ns_read(net_stream *s, void *data, size_t n) {
    size_t available = s->length - s->position;
    if (!available) return s->fail ? -1 : 0;
    if (n > available) n = available;
    if (s->split && n > s->split) n = s->split;
    memcpy(data, s->wire + s->position, n); s->position += n; return (long)n;
}
const char *ns_error(net_stream *s) { return s->fail ? "test read error" : ""; }
void ns_close(net_stream *s) { (void)s; }

static int callback(void *ctx, const char *data, size_t n) {
    (void)ctx; callbacks++;
    if (n > sizeof callback_body - callback_len - 1) return -1;
    memcpy(callback_body + callback_len, data, n); callback_len += n; callback_body[callback_len] = 0;
    return 0;
}
static int on_header(void *ctx,const char *line,size_t n) {
    (void)ctx;
    if(n>11&&!strncasecmp(line,"Set-Cookie:",11))cookie_header_len=n;
    return header_result;
}

static void response(const void *wire, size_t n, size_t split, bool io_error,
                     bool cb, bool ok, const char *body) {
    stream = (struct net_stream){wire, n, 0, split, io_error};
    struct http_req q = {.url = "https://transport.test/", .on_body = cb ? callback : NULL};
    struct http_resp r;
    callbacks = 0; callback_len = 0; callback_body[0] = 0;
    int result = http_request(&q, &r);
    CHECK((result == 0) == ok);
    if (ok && body) {
        CHECK(r.body_len == strlen(body));
        CHECK(!strcmp(cb ? callback_body : r.body ? r.body : "<null>", body));
    }
    if (!ok) CHECK(r.error[0]);
    http_resp_free(&r);
}

static void text_response(const char *wire, bool ok, const char *body) {
    response(wire, strlen(wire), 1, false, false, ok, body);
}

static void encoded(const unsigned char *gzip, size_t n, bool chunked, bool cb, bool ok, const char *body) {
    char *wire = malloc(n + 256);
    int h = snprintf(wire, n + 256, "HTTP/1.1 200 OK\r\nContent-Encoding: GZip\r\n%s\r\n",
                     chunked ? "Transfer-Encoding: chunked\r\n" : "");
    if (chunked) h += snprintf(wire + h, n + 256 - (size_t)h, "%zx;test=value\r\n", n);
    memcpy(wire + h, gzip, n); size_t size = (size_t)h + n;
    if (chunked) { memcpy(wire + size, "\r\n0\r\n\r\n", 7); size += 7; }
    response(wire, size, chunked ? 3 : 0, false, cb, ok, body);
    free(wire);
}

static const unsigned char gzip_sample[] = {
    0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0xff,0xf3,0xcb,0x4f,0x2e,0x29,0x2d,0xca,0x4b,0x55,0x48,0x49,0x4d,0xce,0x4f,
    0x49,0x4d,0x51,0x28,0x4a,0x2d,0x2e,0xc8,0xcf,0x2b,0x4e,0xe5,0x02,0x00,0x4a,0x54,0x0d,0xd9,0x1a,0x00,0x00,0x00,
};

static const unsigned char gzip_optional[] = {
    0x1f,0x8b,0x08,0x1e,0x00,0x00,0x00,0x00,0x02,0xff,0x03,0x00,0x61,0x62,0x63,0x6e,0x61,0x6d,0x65,0x00,0x63,0x6f,0x6d,0x6d,
    0x65,0x6e,0x74,0x00,0xf4,0x6e,0xf3,0xcb,0x4f,0x2e,0x29,0x2d,0xca,0x4b,0x55,0x48,0x49,0x4d,0xce,0x4f,0x49,0x4d,0x51,0x28,
    0x4a,0x2d,0x2e,0xc8,0xcf,0x2b,0x4e,0xe5,0x02,0x00,0x4a,0x54,0x0d,0xd9,0x1a,0x00,0x00,0x00,
};

/* Run-length encoded gzip member expanding to 16 MiB + 1 byte. */
static const unsigned short gzip_limit_runs[][2] = {
    {1,31},{1,139},{1,8},{5,0},{1,2},{1,255},{1,236},{1,193},
    {1,129},{4,0},{1,128},{1,32},{1,205},{1,253},{1,73},{1,22},
    {1,169},{1,10},{4094,0},{1,128},{1,217},{1,131},{1,3},{1,1},
    {4,0},{1,32},{1,255},{1,215},{1,70},{1,80},{4095,85},{1,97},
    {1,15},{1,14},{1,4},{4,0},{1,128},{1,252},{1,95},{1,27},
    {1,65},{4095,85},{1,165},{1,61},{1,56},{1,32},{4,0},{1,16},
    {1,242},{1,255},{1,117},{1,67},{1,2},{3969,0},{1,192},{1,86},
    {1,37},{1,246},{1,115},{1,202},{1,1},{2,0},{1,1},
};


int main(void) {
    /* Reserved DEFLATE block: neither a NULL nor a stale stb reason is usable. */
    extern int http_inflate(char *, int, const char *, int, int *);
    const char reserved_block[9] = {7}; char inflated[128]; int consumed = 0;
    CHECK(http_inflate(inflated, sizeof inflated, reserved_block, sizeof reserved_block, &consumed) == -1);
    char *header_wire=malloc(20000);size_t h=(size_t)sprintf(header_wire,"HTTP/1.1 200 OK\r\nX-Padding: ");
    memset(header_wire+h,'p',6000);h+=6000;memcpy(header_wire+h,"\r\nSet-Cookie: long=",19);h+=19;
    memset(header_wire+h,'v',2400);h+=2400;memcpy(header_wire+h,"\r\nContent-Length: 0\r\n\r\n",23);h+=23;
    stream=(struct net_stream){(const unsigned char*)header_wire,h,0,17,false};
    struct http_req header_request={.url="https://transport.test/",.on_header=on_header};struct http_resp header_response;
    CHECK(http_request(&header_request,&header_response)==0);CHECK(cookie_header_len==2417);http_resp_free(&header_response);
    header_result=-1;CHECK(http_request(&header_request,&header_response)<0);http_resp_free(&header_response);header_result=0;
    free(header_wire);
    CHECK(http_inflate(inflated, 1, (const char *)gzip_sample + 10, sizeof gzip_sample - 10, &consumed) == -2);
    CHECK(http_inflate(inflated, sizeof inflated, reserved_block, sizeof reserved_block, &consumed) == -1);
    /* A YouTube response put Transfer-Encoding after more than 4 KiB of fields. */
    char large[8192]; size_t used = (size_t)snprintf(large, sizeof large, "HTTP/1.1 200 OK\r\n");
    for (int i = 0; i < 90; i++) used += (size_t)snprintf(large + used, sizeof large - used,
        "X-Header-%02d: abcdefghijklmnopqrstuvwxyz0123456789\r\n", i);
    used += (size_t)snprintf(large + used, sizeof large - used,
        "tRaNsFeR-EnCoDiNg: ChUnKeD\r\n\r\n5\r\nhello\r\n0\r\nX-Trailer: ignored\r\n\r\n");
    response(large, used, 1, false, false, true, "hello");
    response(large, used, 4096, false, true, true, "hello");
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloignored", true, "hello");
    text_response("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\nhello", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: notchunked\r\n\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello!\r\n0\r\n\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhell", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n", false, NULL);
    text_response("HTTP/1.1 200 OK\r\nContent-Encoding: br\r\n\r\n", false, NULL);
    const char bad_header[]="HTTP/1.1 200 OK\r\nSet-Cookie: x=abc\0hidden\r\n\r\n";
    response(bad_header,sizeof bad_header-1,1,false,false,false,NULL);
    const char *plain = "HTTP/1.1 200 OK\r\n\r\nhello";
    response(plain, strlen(plain), 1, true, false, false, NULL);
    response(plain, strlen(plain), 1, false, true, true, "hello");
    CHECK(callbacks == 5); /* Uncompressed streaming still delivers before EOF. */
    const char *payload = "Nocturne decoded response\n";
    encoded(gzip_sample, sizeof gzip_sample, false, false, true, payload);
    encoded(gzip_sample, sizeof gzip_sample, true, true, true, payload);
    encoded(gzip_optional, sizeof gzip_optional, false, false, true, payload);
    encoded(gzip_sample, sizeof gzip_sample - 1, false, false, false, NULL);
    unsigned char changed[256];
    memcpy(changed, gzip_sample, sizeof gzip_sample);
    changed[sizeof gzip_sample - 8] ^= 1;
    encoded(changed, sizeof gzip_sample, false, true, false, NULL); CHECK(callbacks == 0);
    memcpy(changed, gzip_sample, sizeof gzip_sample); changed[sizeof gzip_sample - 4] ^= 1;
    encoded(changed, sizeof gzip_sample, false, false, false, NULL);
    memcpy(changed, gzip_sample, sizeof gzip_sample); changed[3] |= 0x20;
    encoded(changed, sizeof gzip_sample, false, false, false, NULL);
    memcpy(changed, gzip_optional, sizeof gzip_optional); changed[12] ^= 1;
    encoded(changed, sizeof gzip_optional, false, false, false, NULL);
    memcpy(changed, gzip_sample, sizeof gzip_sample); changed[sizeof gzip_sample] = 'x';
    encoded(changed, sizeof gzip_sample + 1, false, false, false, NULL);
    memcpy(changed, gzip_sample, sizeof gzip_sample);
    memcpy(changed + sizeof gzip_sample, gzip_sample, sizeof gzip_sample);
    encoded(changed, sizeof gzip_sample * 2, true, false, true,
            "Nocturne decoded response\nNocturne decoded response\n");
    unsigned char gzip_limit[20000]; size_t limit_len = 0;
    for (size_t i = 0; i < sizeof gzip_limit_runs / sizeof *gzip_limit_runs; i++) {
        memset(gzip_limit + limit_len, gzip_limit_runs[i][1], gzip_limit_runs[i][0]);
        limit_len += gzip_limit_runs[i][0];
    }
    encoded(gzip_limit, limit_len, false, true, false, NULL); CHECK(callbacks == 0);
    printf("httptest: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
