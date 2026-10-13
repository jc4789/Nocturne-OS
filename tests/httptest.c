#if defined(TLS_DEADLINE_HOST_TEST)
/* Actual net_stream callbacks/deadlines over substituted native transport and
   BearSSL I/O driving. This does not test TLS authentication or a website. */
#include "../user/libc/tls.c"
int *__errno_location(void){static int value;return &value;}
const br_x509_trust_anchor tls_trust_anchors[1]={{0}};
const size_t tls_trust_anchors_num=1;
static uint64_t clock_ms;
static unsigned connect_delay,read_delay,handshake_reads=1,application_reads=1,close_reads;
static int failure_mode,closed,read_count,checks,failed,last_timeouts[32];
static bool engine_closed;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL TLS deadline:%d %s\n",__LINE__,#c);}}while(0)
uint64_t uptime_ms(void){return clock_ms;}
bool net_wait_up(int timeout){(void)timeout;return true;}
int net_resolve(const char *host,uint32_t *ip){(void)host;*ip=0x0a000202;return 0;}
int tcp_connect(uint32_t ip,uint16_t port,int timeout){(void)ip;(void)port;CHECK(timeout<=20000);
    if(connect_delay>=(unsigned)timeout){clock_ms+=timeout;errno=ETIMEDOUT;return -1;}
    clock_ms+=connect_delay;return 7;}
int tcp_close(int socket){CHECK(socket==7);closed++;return 0;}
long tcp_send(int socket,const void *data,size_t n){(void)socket;(void)data;return (long)n;}
long tcp_recv(int socket,void *data,size_t n,int timeout){(void)socket;(void)n;
    if(read_count<32)last_timeouts[read_count]=timeout;read_count++;
    if(failure_mode==1)return 0;
    if(failure_mode==2){errno=ECONNRESET;return -1;}
    if(timeout>=0&&read_delay&&(unsigned)timeout<=read_delay){clock_ms+=(unsigned)timeout;errno=EAGAIN;return -1;}
    clock_ms+=read_delay;*(unsigned char*)data=1;return 1;}
void br_ssl_client_init_full(br_ssl_client_context *cc,br_x509_minimal_context *xc,
    const br_x509_trust_anchor *anchors,size_t count){(void)xc;(void)anchors;(void)count;memset(cc,0,sizeof *cc);}
void br_ssl_engine_set_buffer(br_ssl_engine_context *cc,void *buf,size_t len,int bidi){(void)cc;(void)buf;(void)len;(void)bidi;}
int br_ssl_client_reset(br_ssl_client_context *cc,const char *host,int resume){(void)cc;(void)host;(void)resume;engine_closed=false;return 1;}
unsigned br_ssl_engine_current_state(const br_ssl_engine_context *cc){return cc->err||engine_closed?BR_SSL_CLOSED:BR_SSL_SENDAPP;}
void br_sslio_init(br_sslio_context *ctx,br_ssl_engine_context *engine,
    int(*read_fn)(void*,unsigned char*,size_t),void *read_ctx,
    int(*write_fn)(void*,const unsigned char*,size_t),void *write_ctx){
    ctx->engine=engine;ctx->low_read=read_fn;ctx->read_context=read_ctx;
    ctx->low_write=write_fn;ctx->write_context=write_ctx;}
static int mock_records(br_sslio_context *ctx,unsigned count){unsigned char byte;
    for(unsigned i=0;i<count;i++)if(ctx->low_read(ctx->read_context,&byte,1)<0){ctx->engine->err=BR_ERR_IO;return -1;}
    return 0;}
int br_sslio_flush(br_sslio_context *ctx){net_stream *s=ctx->read_context;return mock_records(ctx,s->handshaking?handshake_reads:0);}
int br_sslio_write_all(br_sslio_context *ctx,const void *data,size_t n){return ctx->low_write(ctx->write_context,data,n)<0?-1:0;}
int br_sslio_read(br_sslio_context *ctx,void *data,size_t n){if(!n)return 0;
    if(mock_records(ctx,application_reads)<0)return -1;*(char*)data='x';return 1;}
int br_sslio_close(br_sslio_context *ctx){int result=mock_records(ctx,close_reads);engine_closed=true;return result;}
static void setup(unsigned connect,unsigned delay,unsigned records){
    clock_ms=100;connect_delay=connect;read_delay=delay;handshake_reads=records;
    application_reads=1;close_reads=0;failure_mode=0;closed=read_count=0;engine_closed=false;
    memset(last_timeouts,0,sizeof last_timeouts);}
int main(void){char error[160],byte;net_stream *s;
    setup(500,6000,4);s=ns_open("transport.invalid",443,true,INT_MAX,error,sizeof error);
    CHECK(!s&&clock_ms==20100&&closed==1);
    CHECK(read_count==4&&last_timeouts[0]==19500&&last_timeouts[1]==13500&&last_timeouts[2]==7500&&last_timeouts[3]==1500);
    CHECK(strstr(error,"TLS handshake")&&strstr(error,"timed out")&&strstr(error,"transport=")&&strstr(error,"TLS="));
    setup(100,100,2);s=ns_open("transport.invalid",443,true,250,error,sizeof error);
    CHECK(!s&&clock_ms==350&&last_timeouts[0]==150&&last_timeouts[1]==50&&closed==1);
    setup(250,1000,3);s=ns_open("transport.invalid",443,true,180000,error,sizeof error);CHECK(s!=NULL);
    if(s){read_delay=90000;CHECK(ns_read(s,&byte,1)==1&&last_timeouts[3]==180000);
        CHECK(ns_read(s,&byte,1)==1&&last_timeouts[4]==180000);
        ns_set_timeout(s,-1);read_delay=200000;CHECK(ns_read(s,&byte,1)==1&&last_timeouts[5]==-1);
        ns_set_timeout(s,0);read_delay=0;CHECK(ns_read(s,&byte,1)==1&&last_timeouts[6]==0);
        read_delay=300;close_reads=4;uint64_t start=clock_ms;ns_close(s);
        CHECK(clock_ms-start==1000&&closed==1&&last_timeouts[7]==1000&&last_timeouts[10]==100);}
    setup(0,1,1);s=ns_open("transport.invalid",443,true,1000,error,sizeof error);CHECK(s!=NULL);
    if(s){read_delay=600;application_reads=2;uint64_t start=clock_ms;
        CHECK(ns_read(s,&byte,1)==-1&&clock_ms-start==1000&&last_timeouts[1]==1000&&last_timeouts[2]==400);ns_close(s);CHECK(closed==1);}
    setup(0,1,1);failure_mode=1;s=ns_open("transport.invalid",443,true,1000,error,sizeof error);
    CHECK(!s&&closed==1&&strstr(error,"peer closed")&&strstr(error,"eof=1")&&strstr(error,"transport=0"));
    setup(0,1,1);failure_mode=2;s=ns_open("transport.invalid",443,true,1000,error,sizeof error);
    char expected[40];snprintf(expected,sizeof expected,"transport=%d",ECONNRESET);
    CHECK(!s&&closed==1&&strstr(error,expected)&&strstr(error,"eof=0"));
    setup(0,1,1);s=ns_open("transport.invalid",80,false,180000,error,sizeof error);CHECK(s!=NULL);
    if(s){read_delay=90000;CHECK(ns_read(s,&byte,1)==1&&last_timeouts[0]==180000);ns_close(s);CHECK(closed==1);}
    setup(0,1,1);s=ns_open("transport.invalid",443,true,-1,error,sizeof error);CHECK(s!=NULL);
    if(s){read_delay=200000;CHECK(ns_read(s,&byte,1)==1&&last_timeouts[1]==-1);ns_close(s);CHECK(closed==1);}
    printf("tls-stream-deadline: %d checks, %d failed\n",checks,failed);return failed!=0;}
#else
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
static char sent_request[1024];
static size_t sent_request_len;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL httptest:%d: %s\n", __LINE__, #c); failed++; } } while (0)

net_stream *ns_open(const char *host, uint16_t port, bool tls, int timeout, char *error, size_t n) {
    (void)host; (void)port; (void)tls; (void)timeout; (void)error; (void)n;
    stream.position = 0; sent_request_len = 0; sent_request[0] = 0; return &stream;
}
net_stream *ns_open_timed(const char *host,uint16_t port,bool tls,int timeout,char *error,size_t n,
                          struct http_transport_timing *timing){
    memset(timing,0,sizeof *timing);timing->connect_ms=2;timing->tls_ms=tls?3:0;
    return ns_open(host,port,tls,timeout,error,n);}
uint64_t uptime_ms(void){return 0;}
long ns_write(net_stream *s, const void *data, size_t n) {
    (void)s;
    if (n > sizeof sent_request - sent_request_len - 1) return -1;
    memcpy(sent_request + sent_request_len, data, n); sent_request_len += n;
    sent_request[sent_request_len] = 0; return (long)n;
}
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

static size_t explicit_body_limit;
static void response(const void *wire, size_t n, size_t split, bool io_error,
                     bool cb, bool ok, const char *body) {
    stream = (struct net_stream){wire, n, 0, split, io_error};
    struct http_req q = {.url = "https://transport.test/", .on_body = cb ? callback : NULL};
    struct http_resp r;
    callbacks = 0; callback_len = 0; callback_body[0] = 0;
    int result = explicit_body_limit ? http_request_limited(&q, &r, explicit_body_limit) : http_request(&q, &r);
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
static void request_framing(const char *method, const void *body, size_t length, bool has_length) {
    static const unsigned char reply[] = "HTTP/1.1 204 No Content\r\n\r\n";
    stream = (struct net_stream){.wire = reply, .length = sizeof reply - 1};
    struct http_req q = {.url = "https://transport.test/request?source=fixture", .method = method,
        .headers = "X-Fixture: retained\r\n", .body = body, .body_len = length};
    struct http_resp r;
    CHECK(http_request(&q, &r) == 0 && r.status == 204);
    char start[128], field[64];
    snprintf(start, sizeof start, "%s /request?source=fixture HTTP/1.1\r\n", method);
    snprintf(field, sizeof field, "Content-Length: %zu\r\n", length);
    const char *found = strstr(sent_request, "\r\nContent-Length:");
    CHECK(!strncmp(sent_request, start, strlen(start)) &&
          (has_length ? found && !strncmp(found + 2, field, strlen(field)) &&
                        !strstr(found + 2 + strlen(field), "Content-Length:") : !found));
    const char *separator = strstr(sent_request, "X-Fixture: retained\r\n\r\n");
    size_t header_end = separator ? (size_t)(separator - sent_request) + strlen("X-Fixture: retained\r\n\r\n") : 0;
    CHECK(header_end && sent_request_len == header_end + length &&
          (!length || !memcmp(sent_request + header_end, body, length)));
    http_resp_free(&r);
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
    static const char *const absent_methods[] = {"GET", "HEAD", "PATCH", "DELETE", "OPTIONS", "POST", "PUT"};
    for (size_t i = 0; i < sizeof absent_methods / sizeof *absent_methods; i++)
        request_framing(absent_methods[i], NULL, 0, i >= 5);
    request_framing("PATCH", "", 0, true);
    request_framing("DELETE", "", 0, true);
    const unsigned char binary_body[] = {0, 0xff, '\r', '\n'};
    request_framing("POST", binary_body, sizeof binary_body, true);
    struct http_req missing = {.url = "https://transport.test/", .method = "POST", .body_len = 1};
    struct http_resp missing_response;
    sent_request_len = 0;
    CHECK(http_request(&missing, &missing_response) < 0 && missing_response.error[0] && !sent_request_len);
    http_resp_free(&missing_response);
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
    /* Defaults are unlimited now; fault injection uses an explicit caller budget. */
    explicit_body_limit = 16u << 20;
    encoded(gzip_limit, limit_len, false, true, false, NULL); CHECK(callbacks == 0);
    printf("httptest: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
#endif
