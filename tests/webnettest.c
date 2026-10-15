/* Real Nocturne worker/pipe/network tests against the runner's two HTTP fixture origins. */
#include "nocturne.h"
#include "webnet.h"
#include "webnet_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct result {
    int calls, status;
    uint64_t id, generation;
    char *body;
    size_t length;
    size_t final_url_length, response_headers_length;
    char url[WEBNET_URL_MAX], headers[WEBNET_RESPONSE_HEADERS_MAX], error[160];
};
static webnet *net;
static int ports[2], failed;
static char document[128];
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}
static void release(struct result *r) { free(r->body); memset(r, 0, sizeof *r); }
static void completed(webnet *n, uint64_t id, uint64_t generation,
                      const struct webnet_response *response, void *opaque) {
    (void)n;
    struct result *r = opaque;
    r->calls++; r->id = id; r->generation = generation; r->status = response->status;
    r->final_url_length=strlen(response->final_url);r->response_headers_length=strlen(response->headers);
    snprintf(r->url, sizeof r->url, "%s", response->final_url);
    snprintf(r->headers, sizeof r->headers, "%s", response->headers);
    snprintf(r->error, sizeof r->error, "%s", response->error);
    r->body = malloc(response->body_len + 1);
    if (!r->body) { strcpy(r->error, "test response allocation failed"); return; }
    if (response->body_len) memcpy(r->body, response->body, response->body_len);
    r->body[response->body_len] = 0; r->length = response->body_len;
}
static void pump_for(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    do { webnet_pump(net, uptime_ms()); msleep(5); } while (uptime_ms() < until);
}
static bool wait_result(struct result *r, unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    while (!r->calls && uptime_ms() < until) { webnet_pump(net, uptime_ms()); msleep(5); }
    return r->calls == 1;
}
static bool wait_idle(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    while (webnet_busy(net) && uptime_ms() < until) { webnet_pump(net, uptime_ms()); msleep(5); }
    return !webnet_busy(net);
}
static int children(void) {
    struct n_procinfo p[128]; int n = proclist(p, 128), count = 0;
    for (int i = 0; i < n; i++) if (p[i].ppid == getpid() && strstr(p[i].name, "webfetch")) count++;
    return count;
}
static unsigned transports(void){return webnet_inflight(net);}
static uint64_t request_options(struct result *r, int port, const char *path, enum webnet_kind kind,
                        const char *method, const char *headers, const void *body, size_t length,
                        uint64_t generation, bool force_preflight, bool redirect_error, bool same_origin) {
    char url[WEBNET_URL_MAX]; snprintf(url, sizeof url, "http://10.0.2.2:%d%s", ports[port], path);
    struct webnet_request q = {.kind=kind, .generation=generation, .url=url, .origin=document,
        .method=method, .headers=headers, .body=body, .body_len=length,
        .credentials=WEBNET_CREDENTIALS_OMIT, .force_preflight=force_preflight,
        .redirect_error=redirect_error, .same_origin=same_origin};
    return webnet_submit(net, &q, completed, r);
}
static uint64_t request_flags(struct result *r, int port, const char *path, enum webnet_kind kind,
                        const char *method, const char *headers, const void *body, size_t length,
                        uint64_t generation, bool force_preflight) {
    return request_options(r,port,path,kind,method,headers,body,length,generation,force_preflight,false,false);
}
static uint64_t request(struct result *r, int port, const char *path, enum webnet_kind kind,
                        const char *method, const char *headers, const void *body, size_t length,
                        uint64_t generation) {
    return request_flags(r, port, path, kind, method, headers, body, length, generation, false);
}
static bool success(const struct result *r) { return r->calls == 1 && r->status == 200 && !r->error[0]; }
/* Compare every byte and the exact field length, not just a prefix. This also
   detects truncation in the worker pipe, native transport or test callback. */
static bool filled_header(const char *headers, const char *name, size_t length, char fill) {
    size_t nl=strlen(name);
    for(const char *p=headers;*p;) {
        const char *end=strstr(p,"\r\n");if(!end)return false;
        if((size_t)(end-p)>nl && p[nl]==':' && !strncasecmp(p,name,nl)) {
            const char *value=p+nl+1;while(value<end&&(*value==' '||*value=='\t'))value++;
            if((size_t)(end-value)!=length)return false;
            for(size_t i=0;i<length;i++)if(value[i]!=fill)return false;
            return true;
        }
        p=end+2;
    }
    return false;
}
static bool complete_long_cookie(const char *cookies) {
    const char *value=strstr(cookies,"longcookie=");if(!value)return false;
    value+=strlen("longcookie=");
    return strspn(value,"v")==2400 && (!value[2400]||value[2400]==';');
}
static void expect_request(int port, const char *path, enum webnet_kind kind, const char *headers,
                           bool want_success, const char *what) {
    struct result r = {0};
    uint64_t id = request(&r, port, path, kind, "GET", headers, NULL, 0, 11);
    bool done = id && wait_result(&r, 40000);
    check(done && (want_success ? success(&r) : r.error[0] != 0) && r.id == id && r.generation == 11, what);
    if (!done) webnet_cancel_generation(net, 11);
    if (!want_success && r.error[0]) printf("  rejection: %s\n", r.error);
    release(&r);
}
static void http_tests(void) {
    struct result r = {0};
    uint64_t id = request(&r, 0, "/api/json", WEBNET_FETCH, "GET", NULL, NULL, 0, 1);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"value\": 42") &&
          r.id == id && r.generation == 1, "GET callback id/generation/body");
    release(&r);
    id = request(&r, 0, "/api/echo?fixture=1", WEBNET_FETCH, "POST", "Content-Type: text/plain\r\n", "Nocturne", 8, 2);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"method\": \"POST\"") &&
          strstr(r.body, "\"body\": \"Nocturne\""), "POST string body");
    release(&r);

    const size_t length = 196613; /* not aligned with TCP blocks, pipe capacity, or parent pump budget */
    id = request(&r, 0, "/api/pattern?n=196613", WEBNET_RESOURCE, "GET", NULL, NULL, 0, 3);
    bool same = id && wait_result(&r, 40000) && success(&r) && r.length == length;
    for (size_t i = 0; same && i < length; i++) same = (unsigned char)r.body[i] == (unsigned char)(i * 37 + 11);
    check(same, "fragmented HTTP and worker response pipe preserve every byte");
    release(&r);
    unsigned char *upload = malloc(length);
    if (!upload) { check(false, "upload allocation"); return; }
    for (size_t i = 0; i < length; i++) upload[i] = (unsigned char)(i * 37 + 11);
    id = request(&r, 0, "/api/echo-raw", WEBNET_FETCH, "POST", "Content-Type: application/octet-stream\r\n", upload, length, 4);
    memset(upload, 0, length); free(upload); /* Submit must copy, not retain caller storage. */
    same = id && wait_result(&r, 40000) && success(&r) && r.length == length;
    for (size_t i = 0; same && i < length; i++) same = (unsigned char)r.body[i] == (unsigned char)(i * 37 + 11);
    check(same, "fragmented request pipe and binary POST ownership");
    release(&r);
}
static void debug_transport_tests(void) {
    /* 製品workerのprivate診断除去を確認。サイト受入ではない。
       隔離jarで、任意本文・Cookie・header値は試験ログへ出力しない。 */
    webnet *saved = net;
    net = webnet_create();
    check(net != NULL, "数値診断の隔離通信jar確保");
    if (!net) { net = saved; return; }
    webnet_debug(net, true);
    struct result r = {0};
    const size_t length = 196613;
    uint64_t id = request(&r, 0, "/api/pattern?n=196613", WEBNET_RESOURCE, "GET", NULL, NULL, 0, 309);
    bool done = id && wait_result(&r, 40000);
    bool same = done && success(&r) && r.length == length;
    for (size_t i = 0; same && i < length; i++) same = (unsigned char)r.body[i] == (unsigned char)(i * 37 + 11);
    check(same && !strstr(r.headers, "HTTP/Nocturne-Request") && !strstr(r.headers,"HTTP/Nocturne-Transport"),
          "数値診断除去後も分割binary body全byteとHTTP状態を保全");
    check(done && success(&r) && r.id == id && r.generation == 309 &&
          strstr(r.headers, "HTTP/1.1 200 ") && strstr(r.headers, "HTTP/Nocturne-Meta cors=0 redirected=0") &&
          strstr(r.headers, "Content-Type: application/octet-stream\r\n"),
          "数値診断だけをcallback前に除去し既存statusとheaderを保全");
    if (!done) webnet_cancel_generation(net, 309);
    release(&r);

    char url[WEBNET_URL_MAX], visible[256];
    snprintf(url, sizeof url,
        "http://10.0.2.2:%d/api/cookie?set=diagnosticcookie%%3Dreceived%%3B%%20Path%%3D%%2F%%3B%%20HttpOnly", ports[0]);
    struct webnet_request q = {.kind = WEBNET_RESOURCE, .generation = 310, .url = url, .origin = document,
        .method = "GET", .credentials = WEBNET_CREDENTIALS_INCLUDE};
    id = webnet_submit(net, &q, completed, &r); done = id && wait_result(&r, 40000);
    static const char empty_cookie_body[] =
        "{\"cookie\": \"\", \"origin\": \"\", \"preflight_cookie\": \"\", \"preflight_count\": 0}";
    check(done && success(&r) && r.length == sizeof empty_cookie_body - 1 &&
          !memcmp(r.body, empty_cookie_body, sizeof empty_cookie_body - 1) &&
          !strstr(r.headers, "HTTP/Nocturne-Request") && !strstr(r.headers, "Set-Cookie") &&
          !strstr(r.headers, "set-cookie") && webnet_cookie_get(net, document, visible, sizeof visible) == 0,
          "数値診断除去で本文offsetと特権Cookie処理およびHttpOnly非露出を保全");
    if (!done) webnet_cancel_generation(net, 310);
    release(&r);
    webnet_debug(net, false);
    snprintf(url, sizeof url, "http://10.0.2.2:%d/api/cookie", ports[0]);
    id = webnet_submit(net, &q, completed, &r); done = id && wait_result(&r, 40000);
    check(done && success(&r) && strstr(r.body, "\"cookie\": \"diagnosticcookie=received\"") &&
          !strstr(r.headers, "HTTP/Nocturne-Request") &&
          webnet_cookie_get(net, document, visible, sizeof visible) == 0,
          "元のCookie record offsetを保持し診断無効化後の要求へ正しく引継ぐ");
    if (!done) webnet_cancel_generation(net, 310);
    release(&r);
    webnet_free(net); net = saved;
}
static void body_presence_tests(void) {
    /* 固定fixtureが実際に受信したCLの数値/有無を確認する。
       NULLをmalloc(1)の空bodyへ変えず、空の実bodyも消さない。 */
    static const char *const methods[] = {"GET", "PATCH", "DELETE", "OPTIONS", "POST", "PUT"};
    struct result r = {0};
    char path[512], label[160], expected_method[64];
    for (size_t i = 0; i < sizeof methods / sizeof *methods; i++) {
        snprintf(path, sizeof path, "/api/request-info?probe=null-body-%s", methods[i]);
        uint64_t id = request(&r, 0, path, i ? WEBNET_FETCH : WEBNET_RESOURCE, methods[i], NULL, NULL, 0, 311);
        bool done = id && wait_result(&r, 40000);
        snprintf(expected_method, sizeof expected_method, "\"method\": \"%s\"", methods[i]);
        snprintf(label, sizeof label, "NULL %s bodyの実CL生成規則と空本文を保全", methods[i]);
        check(done && success(&r) && strstr(r.body, expected_method) && strstr(r.body, "\"body_length\": 0") &&
              strstr(r.body, i >= 4 ? "\"content_length\": 0" : "\"content_length\": null"), label);
        if (!done) webnet_cancel_generation(net, 311);
        release(&r);
    }
    static const char *const empty_methods[] = {"PATCH", "DELETE"};
    for (size_t i = 0; i < sizeof empty_methods / sizeof *empty_methods; i++) {
        snprintf(path, sizeof path, "/api/request-info?probe=empty-body-%s", empty_methods[i]);
        uint64_t id = request(&r, 0, path, WEBNET_FETCH, empty_methods[i], NULL, "", 0, 312);
        bool done = id && wait_result(&r, 40000);
        snprintf(expected_method, sizeof expected_method, "\"method\": \"%s\"", empty_methods[i]);
        snprintf(label, sizeof label, "空の実%s bodyをNULLへ消さずCL0で転送", empty_methods[i]);
        check(done && success(&r) && strstr(r.body, expected_method) &&
              strstr(r.body, "\"body_length\": 0") && strstr(r.body, "\"content_length\": 0"), label);
        if (!done) webnet_cancel_generation(net, 312);
        release(&r);
    }
    static const unsigned statuses[] = {303, 307};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        snprintf(path, sizeof path,
            "/api/redirect?status=%u&url=/api/request-info%%3Fprobe%%3Dempty-redirect-%u", statuses[i], statuses[i]);
        uint64_t id = request(&r, 0, path, WEBNET_FETCH, "POST", "Content-Type: text/plain\r\n", "", 0, 313);
        bool done = id && wait_result(&r, 40000);
        snprintf(label, sizeof label, "HTTP%uで空bodyと関連headerのdrop/retainを保全", statuses[i]);
        check(done && success(&r) && strstr(r.body, "\"body_length\": 0") &&
              strstr(r.body, i ? "\"method\": \"POST\"" : "\"method\": \"GET\"") &&
              strstr(r.body, i ? "\"content_length\": 0" : "\"content_length\": null") &&
              strstr(r.body, i ? "\"content_type\": \"text/plain\"" : "\"content_type\": \"\""), label);
        if (!done) webnet_cancel_generation(net, 313);
        release(&r);
    }
}
/* URL-encode the exact target path, including its query. The fixture records
   actual requests separately from OPTIONS, so an error alone is not evidence
   that a rejected operation refrained from contacting its target endpoint. */
static bool encode_query(const char *source, char *encoded, size_t capacity) {
    static const char hex[]="0123456789ABCDEF";
    size_t n=0;
    for(size_t i=0;source[i];i++) {
        unsigned char c=(unsigned char)source[i];
        if(n+4>capacity)return false;
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')encoded[n++]=(char)c;
        else {encoded[n++]='%';encoded[n++]=hex[c>>4];encoded[n++]=hex[c&15];}
    }
    if(!capacity)return false;
    encoded[n]=0;return true;
}
static void observe_no_actual(int port, const char *target, int preflights, const char *what) {
    char encoded[1536],path[1600];
    if(!encode_query(target,encoded,sizeof encoded)){check(false,"observation URL capacity");return;}
    snprintf(path,sizeof path,"/api/observations?path=%s",encoded);
    struct result r={0};uint64_t id=request(&r,port,path,WEBNET_FETCH,"GET",NULL,NULL,0,91);
    char count[64];snprintf(count,sizeof count,"\"preflight_count\": %d",preflights);
    bool done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"actual_count\": 0")&&strstr(r.body,count),what);
    if(!done)webnet_cancel_generation(net,91);
    release(&r);
}
static void fetch_contract_tests(void) {
    struct result r={0};uint64_t id;char label[128];
    static const char *const methods[]={"PUT","PATCH","DELETE","OPTIONS"};
    const unsigned char bytes[]={0,0x80,0xff,'A',0,7};
    for(unsigned i=0;i<sizeof methods/sizeof methods[0];i++) {
        id=request(&r,0,"/api/echo-raw",WEBNET_FETCH,methods[i],"Content-Type: application/octet-stream\r\n",bytes,sizeof bytes,90);
        bool done=id&&wait_result(&r,40000);
        snprintf(label,sizeof label,"%s method transports exact NUL/non-UTF8 binary body",methods[i]);
        check(done&&success(&r)&&r.length==sizeof bytes&&!memcmp(r.body,bytes,sizeof bytes),label);
        if(!done)webnet_cancel_generation(net,90);
        release(&r);
    }
    id=request(&r,0,"/api/pattern?n=196613",WEBNET_FETCH,"HEAD",NULL,NULL,0,90);
    bool done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&r.length==0&&strstr(r.headers,"Content-Length: 196613"),
          "HEAD accepts a representation Content-Length without waiting for a response body");
    if(!done)webnet_cancel_generation(net,90);
    release(&r);
    id=request(&r,0,"/api/status",WEBNET_FETCH,"GET",NULL,NULL,0,90);done=id&&wait_result(&r,40000);
    const char *status_line="HTTP/1.1 202 Fixture Accepted\r\n";
    check(done&&r.status==202&&!r.error[0]&&!strncmp(r.headers,status_line,strlen(status_line))&&r.length==8&&!memcmp(r.body,"accepted",8),
          "native response metadata preserves the server's real status reason phrase");
    if(!done)webnet_cancel_generation(net,90);
    release(&r);
    const char *const bad_methods[]={"TRACE","connect","TRACK","bad method","GET\r\nX-Fixture: injected"};
    bool rejected=true;
    for(unsigned i=0;i<sizeof bad_methods/sizeof bad_methods[0];i++) {
        id=request(&r,0,"/api/json",WEBNET_FETCH,bad_methods[i],NULL,NULL,0,90);
        if(id){rejected=false;webnet_cancel(net,id);}
    }
    check(rejected&&!r.calls,"native boundary rejects forbidden methods and request-line injection");
    uint64_t bad_get=request(&r,0,"/api/json",WEBNET_FETCH,"GET",NULL,"x",1,90);
    uint64_t bad_head=request(&r,0,"/api/json",WEBNET_FETCH,"HEAD",NULL,"x",1,90);
    check(!bad_get&&!bad_head,"native GET and HEAD body validation");
    if(bad_get)webnet_cancel(net,bad_get);
    if(bad_head)webnet_cancel(net,bad_head);

    unsigned long probe=(unsigned long)uptime_ms();char allowed[256],denied[256];
    snprintf(allowed,sizeof allowed,"/api/cors?batch=put-allowed-%lu",probe);
    id=request(&r,1,allowed,WEBNET_FETCH,"PUT","Content-Type: text/plain\r\n","method upload",13,92);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"method\": \"PUT\"")&&strstr(r.body,"\"body\": \"method upload\"")&&
          strstr(r.body,"\"preflight_count\": 1"),"cross-origin PUT preflights even with exclusively safelisted headers");
    if(!done)webnet_cancel_generation(net,92);
    release(&r);
    snprintf(denied,sizeof denied,"/api/cors?batch=put-denied-%lu&allow_methods=GET,POST",probe);
    id=request(&r,1,denied,WEBNET_FETCH,"PUT","Content-Type: text/plain\r\n","not sent",8,92);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"allow method or headers"),"cross-origin PUT rejects preflight without method permission");
    if(!done)webnet_cancel_generation(net,92);
    release(&r);
    observe_no_actual(1,denied,1,"denied PUT sends OPTIONS but never its actual target request");

    const char *body_headers="Content-Type: application/json\r\nContent-Encoding: identity\r\nContent-Language: en\r\nContent-Location: /body\r\n";
    id=request(&r,0,"/api/redirect?status=303&url=%2Fapi%2Frequest-info",WEBNET_FETCH,"PUT",body_headers,"{}",2,93);
    done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"method\": \"GET\"")&&strstr(r.body,"\"body_length\": 0")&&
          strstr(r.body,"\"content_type\": \"\"")&&strstr(r.body,"\"content_encoding\": \"\"")&&
          strstr(r.body,"\"content_language\": \"\"")&&strstr(r.body,"\"content_location\": \"\""),
          "303 PUT becomes GET and removes every request-body header");
    if(!done)webnet_cancel_generation(net,93);
    release(&r);
    id=request(&r,0,"/api/redirect?status=307&url=%2Fapi%2Fecho-raw",WEBNET_FETCH,"PUT","Content-Type: application/octet-stream\r\n",bytes,sizeof bytes,93);
    done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&r.length==sizeof bytes&&!memcmp(r.body,bytes,sizeof bytes),"307 PUT preserves method and exact binary body");
    if(!done)webnet_cancel_generation(net,93);
    release(&r);
    id=request(&r,0,"/api/redirect?status=303&url=%2Fapi%2Frequest-info",WEBNET_FETCH,"HEAD",NULL,NULL,0,93);
    done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&r.length==0&&strstr(r.url,"/api/request-info"),"303 leaves HEAD unchanged and bodyless");
    if(!done)webnet_cancel_generation(net,93);
    release(&r);

    char path[512],target[256];
    snprintf(target,sizeof target,"/api/request-info?probe=redirect-error-%lu",probe);
    snprintf(path,sizeof path,"/api/redirect?url=%%2Fapi%%2Frequest-info%%3Fprobe%%3Dredirect-error-%lu",probe);
    id=request_options(&r,0,path,WEBNET_FETCH,"GET",NULL,NULL,0,94,false,true,false);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"Redirect forbidden"),"redirect:error rejects a redirect before following it");
    if(!done)webnet_cancel_generation(net,94);
    release(&r);
    observe_no_actual(0,target,0,"redirect:error never contacts the redirect target");
    id=request_options(&r,0,"/api/json",WEBNET_FETCH,"GET",NULL,NULL,0,94,false,true,false);done=id&&wait_result(&r,40000);
    check(done&&success(&r),"redirect:error allows nonredirect responses");
    if(!done)webnet_cancel_generation(net,94);
    release(&r);
    snprintf(target,sizeof target,"/api/request-info?probe=same-origin-redirect-%lu",probe);
    snprintf(path,sizeof path,"/api/redirect?url=http%%3A%%2F%%2F10.0.2.2%%3A%d%%2Fapi%%2Frequest-info%%3Fprobe%%3Dsame-origin-redirect-%lu",ports[1],probe);
    id=request_options(&r,0,path,WEBNET_FETCH,"GET",NULL,NULL,0,95,false,false,true);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"same-origin mode"),"same-origin policy is enforced after a same-origin URL redirects cross-origin");
    if(!done)webnet_cancel_generation(net,95);
    release(&r);
    observe_no_actual(1,target,0,"same-origin redirect rejection never contacts its CORS-permitted cross-origin target");
    id=request_options(&r,0,"/api/redirect?url=%2Fapi%2Fjson",WEBNET_FETCH,"GET",NULL,NULL,0,95,false,false,true);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.url,"/api/json"),"same-origin mode permits same-origin redirect hops");
    if(!done)webnet_cancel_generation(net,95);
    release(&r);
    snprintf(target,sizeof target,"/api/cors?probe=same-origin-initial-%lu",probe);
    id=request_options(&r,1,target,WEBNET_FETCH,"GET",NULL,NULL,0,95,false,false,true);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"same-origin mode"),"same-origin mode rejects the initial cross-origin URL");
    if(!done)webnet_cancel_generation(net,95);
    release(&r);
    observe_no_actual(1,target,0,"initial same-origin rejection occurs before any network target request");
}
static uint64_t cache_request(struct result *r, int port, const char *path,
                              enum webnet_cache mode, const char *headers, bool same_origin) {
    char url[WEBNET_URL_MAX]; snprintf(url,sizeof url,"http://10.0.2.2:%d%s",ports[port],path);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=98,.url=url,.origin=document,
        .method="GET",.headers=headers,.credentials=WEBNET_CREDENTIALS_OMIT,
        .same_origin=same_origin,.cache_mode=mode};
    return webnet_submit(net,&q,completed,r);
}
static void top_partition_tests(void){
    static const char *const tops[]={"http://top-a.cache.test/document","http://top-b.cache.test/document","http://top-a.cache.test/document"};
    unsigned long probe=(unsigned long)uptime_ms();char url[512],field[64];static struct result r;
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=118,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_OMIT};
    uint64_t id;bool done;
    static const char *const partition_labels[]={
        "same child origin URL and credentials use separate top A/B cache entries and restore top A's original hit",
        "iframe navigation retains enclosing top A/B cache partitions and restores top A's hit",
        "true top navigation uses the destination top partition rather than the previous enclosing top"};
    for(unsigned destination=0;destination<3;destination++){
        q.kind=destination?WEBNET_NAVIGATION:WEBNET_FETCH;q.user_navigation=destination==2;
        q.referrer=destination?NULL:document;q.referrer_policy=destination?WEBNET_REFERRER_DEFAULT:WEBNET_REFERRER_ORIGIN;
        snprintf(url,sizeof url,"http://10.0.2.2:%d/api/http-cache?key=top-partition-%lu-%u&policy=fresh",ports[0],probe,destination);
        bool isolated=true;
        for(unsigned i=0;i<3;i++){
            q.top_level_url=tops[i];id=webnet_submit(net,&q,completed,&r);
            done=id&&wait_result(&r,5000);snprintf(field,sizeof field,"\"count\": %u",destination==2?1u:i==1?2u:1u);
            isolated&=done&&success(&r)&&strstr(r.body,field)!=NULL;
            if(!done)webnet_cancel_generation(net,118);release(&r);
        }
        check(isolated,partition_labels[destination]);
        snprintf(url,sizeof url,"http://10.0.2.2:%d/api/http-cache-count?key=top-partition-%lu-%u",ports[0],probe,destination);
        q.top_level_url=tops[0];id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,5000);
        snprintf(field,sizeof field,"\"count\": %u",destination==2?1u:2u);
        check(done&&success(&r)&&strstr(r.body,field),"top partition selection preserves the exact real network fetch count");
        if(!done)webnet_cancel_generation(net,118);release(&r);
    }
    /* Cache bypass exposes the same security boundary in the native pool. */
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/net8-http?kind=plain",ports[0]);q.kind=WEBNET_FETCH;q.user_navigation=false;q.cache_mode=WEBNET_CACHE_NO_STORE;
    unsigned connections[3]={0};bool pooled=true;
    for(unsigned i=0;i<3;i++){
        q.top_level_url=tops[i];id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,5000);
        const char *connection=done?strstr(r.headers,"X-Connection-ID: "):NULL;
        if(connection)sscanf(connection,"X-Connection-ID: %u",&connections[i]);
        pooled&=done&&success(&r)&&connections[i]!=0;
        if(!done)webnet_cancel_generation(net,118);release(&r);
    }
    check(pooled&&connections[0]!=connections[1]&&connections[0]==connections[2],
          "same child requests isolate top A/B pooled connections and reuse top A's own connection");
    bool cookie_set=webnet_cookie_set_site(net,document,document,"top_partition_strict=present; SameSite=Strict; Path=/")==1;
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/http-cache?key=top-cookie-%lu&policy=fresh",ports[0],probe);
    q.credentials=WEBNET_CREDENTIALS_INCLUDE;bool cookie_isolated=cookie_set;
    for(unsigned i=0;i<2;i++){
        q.top_level_url=i?tops[0]:document;id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,5000);
        bool sent=done&&r.body&&strstr(r.body,"top_partition_strict=present")!=NULL;
        cookie_isolated&=done&&success(&r)&&(sent==(i==0));
        if(!done)webnet_cancel_generation(net,118);release(&r);
    }
    check(cookie_isolated,"native Cookie site context uses the trusted top document rather than the same child initiator");
    check(webnet_cookie_set_site(net,document,document,"top_partition_strict=; Max-Age=0; Path=/")==1,"remove top partition Cookie control");
    webnet_cancel_generation(net,118);
}
static void cache_preflight_tests(void){
    static struct result r;char path[512],observation[512],key[128];
    unsigned long probe=(unsigned long)uptime_ms();
    for(unsigned denied=0;denied<2;denied++){
        snprintf(key,sizeof key,"cache-preflight-%lu-%u",probe,denied);
        snprintf(path,sizeof path,"/api/http-cache?key=%s&policy=fresh&preflight=%s",key,denied?"deny":"allow");
        uint64_t id=cache_request(&r,1,path,WEBNET_CACHE_DEFAULT,NULL,false);bool done=id&&wait_result(&r,5000);
        check(done&&success(&r)&&strstr(r.body,"\"count\": 1"),"seed a fresh cross-origin simple GET cache entry before an unsafe-header request");
        if(!done)webnet_cancel_generation(net,98);release(&r);
        id=cache_request(&r,1,path,WEBNET_CACHE_DEFAULT,"X-Cache-Guard: required\r\n",false);done=id&&wait_result(&r,5000);
        if(denied)check(done&&r.status==0&&r.length==0&&r.response_headers_length==0&&r.error[0],
            "a denied preflight publishes neither cached status nor headers nor body");
        else check(done&&success(&r)&&strstr(r.body,"\"count\": 1"),
            "an allowed preflight retains and serves the original fresh cached representation");
        if(!done)webnet_cancel_generation(net,98);release(&r);
        id=cache_request(&r,1,path,WEBNET_CACHE_DEFAULT,NULL,false);done=id&&wait_result(&r,5000);
        check(done&&success(&r)&&strstr(r.body,"\"count\": 1"),
              "preflight permission or rejection leaves the complete cached simple response intact");
        if(!done)webnet_cancel_generation(net,98);release(&r);
        snprintf(observation,sizeof observation,"/api/http-cache-count?key=%s",key);
        id=request(&r,1,observation,WEBNET_FETCH,"GET",NULL,NULL,0,98);done=id&&wait_result(&r,5000);
        check(done&&success(&r)&&strstr(r.body,"\"count\": 1")&&strstr(r.body,"\"preflight_count\": 1"),
              "the cached unsafe-header request performs one real OPTIONS and never refetches GET");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
}
static void cache_tests(void) {
    top_partition_tests();
    cache_preflight_tests();
    check(sizeof(struct webnet_wire_request)==64 && WEBNET_WIRE_CACHE_MODE(0)==WEBNET_CACHE_DEFAULT &&
          WEBNET_WIRE_CACHE_MODE(WEBNET_WIRE_USER_NAVIGATION)==WEBNET_CACHE_DEFAULT,
          "cache policy preserves 64-byte framing and legacy zero/one flags");
    struct result r={0};char path[512],label[160],field[96];
    unsigned long probe=(unsigned long)uptime_ms();
    static const char *const names[]={"default","no-store","reload","no-cache","force-cache"};
    for(unsigned port=0;port<2;port++)for(unsigned mode=0;mode<5;mode++) {
        snprintf(path,sizeof path,"/api/cache?probe=%lu&port=%u&mode=%u",probe,port,mode);
        uint64_t id=cache_request(&r,port,path,(enum webnet_cache)mode,NULL,false);
        bool done=id&&wait_result(&r,40000);
        const char *control=mode==WEBNET_CACHE_NO_CACHE?"max-age=0":
            mode==WEBNET_CACHE_NO_STORE||mode==WEBNET_CACHE_RELOAD?"no-cache":"";
        snprintf(field,sizeof field,"\"cache_control\": \"%s\"",control);
        bool expected=done&&success(&r)&&strstr(r.body,field)&&
            strstr(r.body,mode==WEBNET_CACHE_NO_STORE||mode==WEBNET_CACHE_RELOAD?
                   "\"pragma\": \"no-cache\"":"\"pragma\": \"\"")&&
            strstr(r.body,*control?"\"cache_control_fields\": 1":"\"cache_control_fields\": 0")&&
            strstr(r.body,mode==WEBNET_CACHE_NO_STORE||mode==WEBNET_CACHE_RELOAD?
                   "\"pragma_fields\": 1":"\"pragma_fields\": 0")&&
            strstr(r.body,"\"if_none_match\": \"\"")&&strstr(r.body,"\"preflight_count\": 0");
        snprintf(label,sizeof label,"%s %s uses exact network cache headers without automatic-header CORS preflight",port?"cross-origin":"same-origin",names[mode]);
        check(expected,label);
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
    }
    /* An HTTP cache hit must reuse bytes from the complete previous response. */
    snprintf(path,sizeof path,"/api/cache?probe=%lu&repeat=force",probe);
    for(unsigned attempt=1;attempt<=2;attempt++) {
        uint64_t id=cache_request(&r,0,path,WEBNET_CACHE_FORCE_CACHE,NULL,false);
        bool done=id&&wait_result(&r,40000);
        snprintf(field,sizeof field,"\"actual_count\": %u",1u);
        check(done&&success(&r)&&strstr(r.body,field)&&strstr(r.headers,"Cache-Control: public, max-age=3600"),
              "force-cache stores a complete miss and reuses its original response on the next request");
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
    }
    uint64_t id=cache_request(&r,0,path,WEBNET_CACHE_NO_CACHE,NULL,false);
    bool done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"actual_count\": 2")&&strstr(r.body,"fixture-cache"),
          "no-cache revalidates the stored ETag through a browser-managed conditional field");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    id=cache_request(&r,0,path,WEBNET_CACHE_ONLY_IF_CACHED,NULL,true);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"actual_count\": 2"),"only-if-cached returns the previously completed representation");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    char encoded[1536],observation[1600];
    bool encoded_ok=encode_query(path,encoded,sizeof encoded);
    if(encoded_ok)snprintf(observation,sizeof observation,"/api/observations?path=%s",encoded);
    id=encoded_ok?request(&r,0,observation,WEBNET_FETCH,"GET",NULL,NULL,0,98):0;done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"actual_count\": 2")&&strstr(r.body,"\"preflight_count\": 0"),
          "cache hits and cache-only reuse add no actual or OPTIONS request");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    snprintf(path,sizeof path,"/api/http-cache?key=fresh-%lu&policy=fresh",probe);
    static const enum webnet_cache sequence[]={WEBNET_CACHE_DEFAULT,WEBNET_CACHE_DEFAULT,WEBNET_CACHE_ONLY_IF_CACHED,
        WEBNET_CACHE_RELOAD,WEBNET_CACHE_NO_STORE,WEBNET_CACHE_DEFAULT,WEBNET_CACHE_NO_CACHE,WEBNET_CACHE_ONLY_IF_CACHED};
    static const unsigned sequence_count[]={1,1,1,2,3,2,2,2};
    for(unsigned step=0;step<sizeof sequence/sizeof sequence[0];step++){
        id=cache_request(&r,0,path,sequence[step],NULL,sequence[step]==WEBNET_CACHE_ONLY_IF_CACHED);
        done=id&&wait_result(&r,40000);snprintf(field,sizeof field,"\"count\": %u",sequence_count[step]);
        snprintf(label,sizeof label,"persistent HTTP cache mode sequence step %u keeps exact stored representation",step);
        check(done&&success(&r)&&strstr(r.body,field),label);
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    snprintf(observation,sizeof observation,"/api/http-cache-count?key=fresh-%lu",probe);
    id=request(&r,0,observation,WEBNET_FETCH,"GET",NULL,NULL,0,98);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"count\": 4"),
          "fresh hits and only-if-cached perform no network; reload no-store and 304 validation perform one request each");
    if(!done)webnet_cancel_generation(net,98);release(&r);
    /* Credential material and initiating origin are part of the private key. */
    static const char *const auth[]={"Authorization: cache-fixture-a\r\n","Authorization: cache-fixture-a\r\n","Authorization: cache-fixture-b\r\n"};
    for(unsigned step=0;step<3;step++){
        id=cache_request(&r,0,path,WEBNET_CACHE_DEFAULT,auth[step],false);done=id&&wait_result(&r,40000);
        snprintf(field,sizeof field,"\"count\": %u",step==2?6u:5u);
        check(done&&success(&r)&&strstr(r.body,field),"Authorization partitions reuse the same identity and isolate a changed identity");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    char private_url[768];snprintf(private_url,sizeof private_url,"http://10.0.2.2:%d%s",ports[0],path);
    struct webnet_request private_request={.kind=WEBNET_FETCH,.generation=98,.url=private_url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_INCLUDE};
    id=webnet_submit(net,&private_request,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"count\": 7"),"include credentials cannot consume an omit-credentials cache entry");
    if(!done)webnet_cancel_generation(net,98);release(&r);
    check(webnet_cookie_set(net,document,"cachefixture=changed; Path=/")==1,"prepare changed Cookie cache partition");
    for(unsigned step=0;step<2;step++){
        id=webnet_submit(net,&private_request,completed,&r);done=id&&wait_result(&r,40000);
        check(done&&success(&r)&&strstr(r.body,"\"count\": 8")&&strstr(r.body,"cachefixture=changed"),
              "changed Cookie fetches a separate representation and then reuses only that partition");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    check(webnet_cookie_set(net,document,"cachefixture=; Path=/; Max-Age=0")==1,"remove cache partition Cookie control");
    snprintf(path,sizeof path,"/api/http-cache?key=vary-%lu&policy=vary",probe);
    for(unsigned step=0;step<3;step++){
        const char *variant=step==1?"X-Variant: two\r\n":"X-Variant: one\r\n";
        id=cache_request(&r,0,path,WEBNET_CACHE_DEFAULT,variant,false);done=id&&wait_result(&r,40000);
        snprintf(field,sizeof field,"\"count\": %u",step==1?2u:1u);
        check(done&&success(&r)&&strstr(r.body,field)&&strstr(r.body,step==1?"\"variant\": \"two\"":"\"variant\": \"one\""),
              "Vary stores both complete request-header variants without mixing their bodies");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    snprintf(path,sizeof path,"/api/http-cache?key=stale-%lu&policy=stale",probe);
    for(unsigned step=0;step<2;step++){
        id=cache_request(&r,0,path,WEBNET_CACHE_DEFAULT,NULL,false);done=id&&wait_result(&r,40000);
        check(done&&success(&r)&&strstr(r.body,"\"count\": 1"),"stale ETag validation merges 304 metadata and retains the original 200 body");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    id=cache_request(&r,0,path,WEBNET_CACHE_ONLY_IF_CACHED,NULL,true);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"No usable cached response"),"must-revalidate cannot be served stale by a cache-only request");
    if(!done)webnet_cancel_generation(net,98);release(&r);
    snprintf(observation,sizeof observation,"/api/http-cache-count?key=stale-%lu",probe);
    id=request(&r,0,observation,WEBNET_FETCH,"GET",NULL,NULL,0,98);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"count\": 2"),"304 validation is one network request and a stale cache-only failure adds none");
    if(!done)webnet_cancel_generation(net,98);release(&r);
    snprintf(path,sizeof path,"/api/http-cache?key=nostore-%lu&policy=nostore",probe);
    for(unsigned step=1;step<=2;step++){
        id=cache_request(&r,0,path,WEBNET_CACHE_DEFAULT,NULL,false);done=id&&wait_result(&r,40000);
        snprintf(field,sizeof field,"\"count\": %u",step);
        check(done&&success(&r)&&strstr(r.body,field),"response no-store never becomes a reusable representation");
        if(!done)webnet_cancel_generation(net,98);release(&r);
    }
    id=cache_request(&r,0,path,WEBNET_CACHE_ONLY_IF_CACHED,NULL,true);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"No usable cached response"),"no-store response leaves only-if-cached as a network-free miss");
    if(!done)webnet_cancel_generation(net,98);release(&r);
    for(unsigned mode=WEBNET_CACHE_NO_STORE;mode<=WEBNET_CACHE_NO_CACHE;mode++) {
        snprintf(path,sizeof path,"/api/cache?probe=%lu&author=%u",probe,mode);
        uint64_t id=cache_request(&r,1,path,(enum webnet_cache)mode,
            "Cache-Control: custom-cache\r\nPragma: custom-pragma\r\n",false);
        bool done=id&&wait_result(&r,40000);
        check(done&&success(&r)&&strstr(r.body,"\"cache_control\": \"custom-cache\"")&&
              strstr(r.body,"\"pragma\": \"custom-pragma\"")&&strstr(r.body,"\"cache_control_fields\": 1")&&
              strstr(r.body,"\"pragma_fields\": 1")&&strstr(r.body,"\"preflight_count\": 1")&&
              strstr(r.body,"\"preflight_headers\": \"cache-control, pragma\""),
              "author cache fields retain values, avoid duplicates and still require CORS permission");
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
    }
    for(unsigned author=0;author<2;author++) {
        snprintf(path,sizeof path,"/api/cache?probe=%lu&partial=%u",probe,author);
        id=cache_request(&r,1,path,WEBNET_CACHE_NO_STORE,
            author?"Pragma: author-pragma\r\n":"Cache-Control: author-cache\r\n",false);
        done=id&&wait_result(&r,40000);
        check(done&&success(&r)&&strstr(r.body,author?"\"pragma\": \"author-pragma\"":"\"pragma\": \"no-cache\"")&&
              strstr(r.body,author?"\"cache_control\": \"no-cache\"":"\"cache_control\": \"author-cache\"")&&
              strstr(r.body,author?"\"preflight_headers\": \"pragma\"":"\"preflight_headers\": \"cache-control\"")&&
              strstr(r.body,"\"cache_control_fields\": 1")&&strstr(r.body,"\"pragma_fields\": 1"),
              "each absent automatic cache field is generated independently without author-field duplication");
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
    }
    snprintf(path,sizeof path,"/api/cache?probe=%lu&unsafe=one",probe);
    id=cache_request(&r,1,path,WEBNET_CACHE_NO_STORE,"X-Fixture: nocturne\r\n",false);
    done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"preflight_headers\": \"x-fixture\"")&&
          strstr(r.body,"\"cache_control\": \"no-cache\"")&&strstr(r.body,"\"pragma\": \"no-cache\""),
          "generated cache headers never contaminate the author-header preflight list");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    static const char *const conditions[]={"If-Modified-Since","If-None-Match","If-Unmodified-Since","If-Match","If-Range"};
    for(unsigned i=0;i<5;i++) {
        char headers[128];snprintf(headers,sizeof headers,"%s: fixture-condition\r\n",conditions[i]);
        snprintf(path,sizeof path,"/api/cache?probe=%lu&condition=%u",probe,i);
        id=cache_request(&r,0,path,WEBNET_CACHE_DEFAULT,headers,false);done=id&&wait_result(&r,40000);
        check(done&&success(&r)&&strstr(r.body,"\"cache_control\": \"no-cache\"")&&
              strstr(r.body,"\"pragma\": \"no-cache\""),
              "default with an author condition uses internal no-store network semantics");
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
    }
    snprintf(path,sizeof path,"/api/redirect?probe=%lu&url=%%2Fapi%%2Fcache%%3Fprobe%%3D%lu%%26hop%%3Dfinal",probe,probe);
    id=cache_request(&r,0,path,WEBNET_CACHE_RELOAD,NULL,false);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"cache_control\": \"no-cache\"")&&strstr(r.body,"\"pragma\": \"no-cache\""),
          "reload generates cache headers on the actual redirected request too");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    for(unsigned port=0;port<2;port++) {
        snprintf(path,sizeof path,"/api/cache?probe=%lu&only=%u",probe,port);
        id=cache_request(&r,port,path,WEBNET_CACHE_ONLY_IF_CACHED,NULL,true);done=id&&wait_result(&r,40000);
        check(done&&r.status==0&&strstr(r.error,port?"Cross-origin fetch":"No usable cached response")&&r.length==0,
              "native cache-only miss is a network error, never a synthetic HTTP response");
        if(!done)webnet_cancel_generation(net,98);
        release(&r);
        observe_no_actual(port,path,0,"cache-only worker sends neither OPTIONS nor an actual target request");
    }
    snprintf(path,sizeof path,"/api/cache?probe=%lu&only=invalid-mode",probe);
    id=cache_request(&r,1,path,WEBNET_CACHE_ONLY_IF_CACHED,NULL,false);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"requires same-origin mode"),"native cache-only request still requires same-origin mode");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    observe_no_actual(1,path,0,"invalid cache-only mode also cannot contact the target");
    snprintf(path,sizeof path,"/api/no-cors?probe=%lu&cache=no-store",probe);
    id=cache_request(&r,1,path,WEBNET_CACHE_NO_STORE,NULL,false);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"Cross-origin response"),"cache policy never bypasses native response CORS checks");
    if(!done)webnet_cancel_generation(net,98);
    release(&r);
    check(!cache_request(&r,0,"/api/cache",(enum webnet_cache)6,NULL,false)&&
          !cache_request(&r,0,"/api/cache",(enum webnet_cache)-1,NULL,false),
          "native submission validates cache metadata before enqueueing");
}
static void concurrency_tests(void) {
    /* Different HTTP origins can use the adaptive budget concurrently; the
       old four-slot ceiling is not an expectation of the transport contract. */
    static struct result burst[8];
    bool submitted=true;
    for(int i=0;i<8;i++)submitted&=request(&burst[i],i%2,"/api/slow?ms=1200",WEBNET_FETCH,"GET",NULL,NULL,0,19)!=0;
    webnet_pump(net,uptime_ms());
    int started=(int)transports();
    struct n_sysinfo memory={0};sysinfo(&memory);
    uint64_t affordable=memory.free_mem/(64u*1024u*1024u);
    int expected=affordable>1?(int)MIN(7u,affordable-1):1;
    /* Admission itself now yields on the GUI time budget. Observe the burst
       before any 1200ms reply can finish, without demanding one long pump. */
    uint64_t admitted_until=uptime_ms()+500;
    while(started<expected&&uptime_ms()<admitted_until){
        webnet_pump(net,uptime_ms());started=MAX(started,(int)transports());
        if(started<expected)webnet_wait(net,-1,10);
    }
    check(submitted&&started>=expected,
          "adaptive worker budget admits a multi-origin burst beyond four when native memory permits");
    bool drained=wait_idle(12000);
    for(int i=0;i<8;i++)drained&=success(&burst[i]);
    check(drained&&transports()==0&&children()==1,"adaptive burst preserves every response and retains exactly one pooled service");
    webnet_cancel_generation(net,19);for(int i=0;i<8;i++)release(&burst[i]);
    static struct result r[3]; /* large bounded response storage is not stack scratch */
    uint64_t ids[3];
    for (int i = 0; i < 3; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=800", WEBNET_FETCH, "GET", NULL, NULL, 0, 20);
    webnet_pump(net, uptime_ms());
    admitted_until=uptime_ms()+300;
    while(transports()<3&&uptime_ms()<admitted_until){webnet_pump(net,uptime_ms());webnet_wait(net,-1,10);}
    check(ids[0] && ids[1] && ids[2] && transports() == 3 && children()==1, "three Fetch channels share one service and leave document dependency admission reserved");
    bool good = true; uint64_t until = uptime_ms() + 6000;
    while (webnet_busy(net) && uptime_ms() < until) {
        webnet_pump(net, uptime_ms());
        if (transports() > 4 || children()!=1) good = false;
        msleep(5);
    }
    for (int i = 0; i < 3; i++) good = good && success(&r[i]);
    check(good && !webnet_busy(net) && transports() == 0 && children()==1, "queued requests complete and release all request transports in the pooled service");
    webnet_cancel_generation(net, 20);
    for (int i = 0; i < 3; i++) release(&r[i]);

    /* A real production worker regression: don't count Fetch as a document
       load dependency, or let its long-poll channels occupy the reserved slot. */
    for (int i = 0; i < 3; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=5000", WEBNET_FETCH, "GET", NULL, NULL, 0, 23);
    pump_for(100);
    static struct result dependency;
    uint64_t dependency_id = request(&dependency, 0, "/api/json", WEBNET_RESOURCE, "GET", NULL, NULL, 0, 24);
    check(ids[0] && ids[1] && ids[2] && dependency_id && wait_result(&dependency, 2500) && success(&dependency) &&
          !r[0].calls && !r[1].calls && !r[2].calls && transports() <= 4 && children()==1,
          "later document resource completes while three long-lived Fetch channels remain active");
    webnet_cancel_generation(net, 23); webnet_cancel_generation(net, 24);
    for (int i = 0; i < 3; i++) release(&r[i]);
    release(&dependency);

    for (int i = 0; i < 3; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=800", WEBNET_RESOURCE, "GET", NULL, NULL, 0, 25);
    dependency_id = request(&dependency, 0, "/api/json", WEBNET_FETCH, "GET", NULL, NULL, 0, 26);
    check(dependency_id && wait_result(&dependency, 2500) && success(&dependency),
          "Fetch still progresses alongside a finite batch of document resources");
    webnet_cancel_generation(net, 25); webnet_cancel_generation(net, 26);
    for (int i = 0; i < 3; i++) release(&r[i]);
    release(&dependency);

    ids[0] = request(&r[0], 0, "/api/slow?ms=5000", WEBNET_FETCH, "GET", NULL, NULL, 0, 21);
    pump_for(100); webnet_cancel(net, ids[0]);
    check(ids[0] && wait_idle(3000) && !r[0].calls && transports() == 0 && children()==1, "single cancellation suppresses callback and retires its channel while preserving the pooled service");
    release(&r[0]);
    for (int i = 0; i < 2; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=5000", WEBNET_FETCH, "GET", NULL, NULL, 0, 22);
    pump_for(100); webnet_cancel_generation(net, 22);
    check(ids[0] && ids[1] && !webnet_busy(net) && !r[0].calls && !r[1].calls && transports() == 0 && children()==1,
          "document cancellation synchronously kills and reaps both workers");
    release(&r[0]); release(&r[1]);
}
static void cors_tests(void) {
    struct result r = {0};
    uint64_t id = request(&r, 1, "/api/cors", WEBNET_FETCH, "GET", NULL, NULL, 0, 30);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.headers, "X-Exposed: visible") &&
          !strstr(r.headers, "X-Private"), "cross-origin CORS and response-header exposure");
    release(&r);
    id = request_flags(&r, 1, "/api/cors?force=post&allow_methods=omit&allow_headers=omit", WEBNET_FETCH,
                       "POST", "Content-Type: text/plain\r\n", "upload", 6, 33, true);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"preflight_count\": 1") &&
          strstr(r.body, "\"body\": \"upload\""), "upload listener forces simple POST OPTIONS without allow-methods/headers");
    release(&r);
    id = request_flags(&r, 1, "/api/cors?force=get", WEBNET_FETCH, "GET", NULL, NULL, 0, 34, true);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"preflight_count\": 1"),
          "upload listener forces cross-origin GET preflight even without a request body");
    release(&r);
    id = request_flags(&r, 0, "/api/cors?force=same-origin", WEBNET_FETCH, "POST",
                       "Content-Type: text/plain\r\n", "upload", 6, 35, true);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"preflight_count\": 0"),
          "forced preflight does not create same-origin OPTIONS");
    release(&r);
    id = request(&r, 1, "/api/cors?force=none", WEBNET_FETCH, "POST",
                 "Content-Type: text/plain\r\n", "upload", 6, 36);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"preflight_count\": 0"),
          "simple cross-origin POST without upload listener does not preflight");
    release(&r);
    id = request_flags(&r, 1, "/api/no-cors?force=denied", WEBNET_FETCH, "POST",
                       "Content-Type: text/plain\r\n", "upload", 6, 37, true);
    check(id && wait_result(&r, 40000) && r.status == 0 && strstr(r.error, "preflight failed"),
          "forced preflight rejection stops the actual request");
    release(&r);
    id = request_flags(&r, 1, "/api/cors?force=unsafe&allow_headers=omit", WEBNET_FETCH, "POST",
                       "X-Fixture: nocturne\r\n", "upload", 6, 38, true);
    check(id && wait_result(&r, 40000) && r.status == 0 && strstr(r.error, "allow method or headers"),
          "forced preflight still requires explicit permission for unsafe headers");
    release(&r);
    id = request(&r, 1, "/api/cors?preflight=network", WEBNET_FETCH, "POST",
                 "Content-Type: application/json\r\nX-Fixture: nocturne\r\n", "{}", 2, 31);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"preflight_count\": 1") &&
          strstr(r.body, "\"x_fixture\": \"nocturne\""), "OPTIONS preflight permits non-simple POST headers");
    release(&r);
    expect_request(1, "/api/no-cors", WEBNET_FETCH, NULL, false, "cross-origin response without CORS is rejected");
    expect_request(1, "/api/cors-wildcard", WEBNET_FETCH, "Authorization: Bearer nocturne-test\r\n", false,
                   "authorization requires explicit allow-header, not wildcard");
    id = request(&r, 1, "/api/cors?authorization=explicit", WEBNET_FETCH, "GET",
                 "Authorization: Bearer nocturne-test\r\n", NULL, 0, 32);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"authorization\": true") &&
          strstr(r.body, "\"preflight_count\": 1"), "explicit authorization and preflight support");
    release(&r);
    expect_request(1, "/api/module", WEBNET_MODULE, NULL, true, "module CORS and JavaScript MIME acceptance");
    expect_request(1, "/api/cors", WEBNET_MODULE, NULL, false, "module rejects JSON MIME");
    expect_request(0, "/api/json", WEBNET_FETCH, "Cookie: forbidden\r\n", false, "page Cookie header is rejected");
    expect_request(0, "/api/json", WEBNET_FETCH, "X-Fixture: a\r\nx-fixture: b\r\n", false, "duplicate request headers are rejected");
}
static void redirect_tests(void) {
    struct result r = {0};
    uint64_t id = request(&r, 0, "/api/redirect?url=%2Fapi%2Frequest-info", WEBNET_FETCH, "POST",
                         "Content-Type: application/json\r\n", "{}", 2, 40);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"method\": \"GET\"") &&
          strstr(r.body, "\"content_type\": \"\"") && strstr(r.url, "/api/request-info"),
          "POST redirect rewrites GET and removes body headers");
    release(&r);
    char path[256];
    snprintf(path, sizeof path, "/api/redirect?url=http%%3A%%2F%%2F10.0.2.2%%3A%d%%2Fapi%%2Frequest-info", ports[1]);
    id = request(&r, 0, path, WEBNET_FETCH, "GET", "Authorization: Bearer nocturne-test\r\n", NULL, 0, 41);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.body, "\"authorization\": false"),
          "cross-origin redirect strips authorization");
    release(&r);
    expect_request(0, "/api/infinite-redirect", WEBNET_FETCH, NULL, false, "redirect loop is bounded");
    expect_request(0, "/api/redirect?url=file%3A%2F%2F%2Fhome%2Fwebnet-outside.js", WEBNET_RESOURCE, NULL, false,
                   "HTTP redirect cannot cross into local files");
}
static void long_url_tests(void) {
    static struct result r;
    static char path[WEBNET_URL_MAX], initial[WEBNET_URL_MAX], encoded[WEBNET_URL_MAX], url[WEBNET_URL_MAX+1];
    strcpy(path,"/api/json?pad=");size_t prefix=strlen(path);
    memset(path+prefix,'x',6000);path[prefix+6000]=0;
    uint64_t id=request(&r,0,path,WEBNET_RESOURCE,"GET",NULL,NULL,0,43);
    snprintf(url,sizeof url,"http://10.0.2.2:%d%s",ports[0],path);
    check(id&&wait_result(&r,40000)&&success(&r)&&!strcmp(r.url,url),
          "6000-byte real worker URL survives request, HTTP path, response framing and final URL without truncation");
    release(&r);
    bool encoded_ok=encode_query(path,encoded,sizeof encoded);
    if(encoded_ok)snprintf(initial,sizeof initial,"/api/redirect?url=%s",encoded);
    id=encoded_ok?request(&r,0,initial,WEBNET_RESOURCE,"GET",NULL,NULL,0,43):0;
    check(id&&wait_result(&r,40000)&&success(&r)&&!strcmp(r.url,url),
          "6000-byte Location survives the production redirect resolver with the complete final URL");
    release(&r);
    snprintf(url,WEBNET_URL_MAX+1,"http://10.0.2.2:%d/api/json?pad=",ports[0]);prefix=strlen(url);
    memset(url+prefix,'x',WEBNET_URL_MAX-prefix);url[WEBNET_URL_MAX]=0;
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=43,.url=url,.origin=document,.method="GET",.credentials=WEBNET_CREDENTIALS_OMIT};
    id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&r.final_url_length==WEBNET_URL_MAX,
          "owned URL at the old inline bound reaches the actual worker without truncation");
    release(&r);
    webnet_cancel_generation(net,43);
}
static void file_request(const char *url, const char *origin, enum webnet_kind kind, bool user, bool allow,
                         const char *what) {
    struct result r = {0};
    struct webnet_request q = {kind, 50, url, origin, "GET", NULL, NULL, 0, user, WEBNET_CREDENTIALS_OMIT};
    uint64_t id = webnet_submit(net, &q, completed, &r);
    bool done = id && wait_result(&r, 3000);
    check(done && (allow ? success(&r) && strstr(r.body, "local-fixture") : r.error[0] != 0), what);
    if (!done) webnet_cancel_generation(net, 50);
    release(&r);
}
static void file_tests(void) {
    mkdir("/home/webnet-fixture");
    const char *paths[2] = {"/home/webnet-fixture/inside.js", "/home/webnet-outside.js"};
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY);
        check(fd >= 0 && write(fd, "// local-fixture\n", 17) == 17, "create local resource fixture");
        if (fd >= 0) close(fd);
    }
    const char *origin = "file:///home/webnet-fixture/index.html";
    file_request("file:///home/webnet-fixture/%69nside.js", origin, WEBNET_MODULE, false, true, "local module within decoded document directory");
    file_request("file:///home/webnet-fixture/%2e%2e/webnet-outside.js", origin, WEBNET_RESOURCE, false, false, "encoded parent traversal is rejected");
    file_request("file:///home/webnet-fixture/%00.js", origin, WEBNET_RESOURCE, false, false, "encoded NUL filename is rejected");
    file_request("file:///home/webnet-fixture/inside.js", document, WEBNET_CLASSIC, false, false, "remote document cannot read a local script");
    file_request("file:///home/webnet-fixture/inside.js", origin, WEBNET_FETCH, false, false, "fetch cannot read local files");
    file_request("file:///home/webnet-fixture/inside.js", origin, WEBNET_NAVIGATION, false, false, "local navigation requires user intent");
    file_request("file:///home/webnet-fixture/inside.js", origin, WEBNET_NAVIGATION, true, true, "user local navigation is allowed");
}
static void limit_tests(void) {
    struct result r = {0};
    uint64_t id = request(&r, 0, "/api/oversize", WEBNET_FETCH, "GET", NULL, NULL, 0, 59);
    bool done = id && wait_result(&r, 35000);
    bool intact=done&&r.status==200&&!r.error[0]&&r.length==16u*1024u*1024u+1;
    if(intact)for(size_t i=0;i<r.length;i++)if(r.body[i]!='x'){intact=false;break;}
    check(intact, "response beyond the removed 16 MiB quota is delivered completely");
    if (!done) webnet_cancel_generation(net, 59);
    release(&r);
    uint64_t started = uptime_ms();
    id = request(&r, 0, "/api/slow?ms=35000", WEBNET_FETCH, "GET", NULL, NULL, 0, 60);
    done = id && wait_result(&r, 45000);
    uint64_t elapsed = uptime_ms() - started;
    check(done && success(&r) && elapsed >= 34500 && elapsed < 44000 && WEBNET_TIMEOUT_MS==UINT32_MAX,
          "a legitimate 35-second response survives the removed 30-second deadline");
    check(!webnet_busy(net) && transports() == 0 && children()==1, "long response completion leaves no request transport and retains one pooled service");
    if (!done) webnet_cancel_generation(net, 60);
    release(&r);
}
static void tls_tests(void) {
    struct result r = {0};
    struct webnet_request q = {WEBNET_NAVIGATION, 70, "https://example.com/", "", "GET", NULL, NULL, 0, true, WEBNET_CREDENTIALS_OMIT};
    uint64_t id = webnet_submit(net, &q, completed, &r);
    check(id && wait_result(&r, 35000) && success(&r) && strstr(r.body, "Example Domain"),
          "real webfetch verified HTTPS succeeds");
    if (r.error[0]) printf("  HTTPS result: %s\n", r.error);
    if (!r.calls) webnet_cancel_generation(net, 70);
    release(&r);
    q.generation = 71; q.url = "https://expired.badssl.com/";
    id = webnet_submit(net, &q, completed, &r);
    check(id && wait_result(&r, 35000) && r.status == 0 && strstr(r.error, "TLS: certificate expired"),
          "real webfetch rejects an expired certificate (not a connection failure)");
    printf("  expired-certificate result: %s\n", r.error);
    if (!r.calls) webnet_cancel_generation(net, 71);
    release(&r);
}
static void cookie_tests(void) {
    /* Real Nocturne worker/pipe path, but these endpoints are transport tests,
       not evidence that a public website renders or executes correctly. */
    struct result r={0};char url[WEBNET_URL_MAX],visible[8192];
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie",ports[0]);
    check(webnet_cookie_set(net,document,"scriptcookie=visible; Path=/")==1,"document cookie reaches the browser jar");
    struct webnet_request q={WEBNET_FETCH,80,url,document,"GET",NULL,NULL,0,false,WEBNET_CREDENTIALS_SAME_ORIGIN};
    uint64_t id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&strstr(r.body,"scriptcookie=visible"),"same-origin request sends script cookie");release(&r);
    q.credentials=WEBNET_CREDENTIALS_OMIT;id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&strstr(r.body,"\"cookie\": \"\""),"omit sends no Cookie header");release(&r);
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?set=ignored%%3D1%%3B%%20Path%%3D%%2F",ports[0]);
    id=webnet_submit(net,&q,completed,&r);bool done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&webnet_cookie_get(net,document,visible,sizeof visible)>=0&&!strstr(visible,"ignored="),"omit ignores response cookies");release(&r);
    q.credentials=WEBNET_CREDENTIALS_INCLUDE;
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie-redirect?set=redirectcookie%%3Dreceived%%3B%%20Path%%3D%%2F&set=secretcookie%%3Dserver%%3B%%20HttpOnly%%3B%%20Path%%3D%%2F",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"redirectcookie=received")&&strstr(r.body,"secretcookie=server"),"redirect applies multiple response cookies before the next hop");
    check(done&&webnet_cookie_get(net,document,visible,sizeof visible)>=0&&strstr(visible,"redirectcookie=received")&&!strstr(visible,"secretcookie"),"response event merge preserves HttpOnly isolation");
    check(done&&!strstr(r.headers,"Set-Cookie")&&!strstr(r.headers,"set-cookie"),"Set-Cookie never appears in JS response headers");release(&r);
    check(webnet_cookie_set(net,document,"secretcookie=script; Path=/")==0,"script cannot overwrite the HTTP-only cookie");
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie",ports[1]);
    q.credentials=WEBNET_CREDENTIALS_SAME_ORIGIN;id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&strstr(r.body,"\"cookie\": \"\""),"same-origin credentials exclude cross-port origin");release(&r);
    q.credentials=WEBNET_CREDENTIALS_INCLUDE;id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&strstr(r.body,"secretcookie=server"),"include permits credentialed exact-origin CORS");release(&r);
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?cors=wildcard",ports[1]);
    id=webnet_submit(net,&q,completed,&r);check(id&&wait_result(&r,40000)&&strstr(r.error,"Credentialed CORS"),"credentialed CORS rejects wildcard ACAO");release(&r);
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?credentials=no",ports[1]);
    id=webnet_submit(net,&q,completed,&r);check(id&&wait_result(&r,40000)&&strstr(r.error,"Credentialed CORS"),"credentialed CORS requires ACAC true");release(&r);
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?preflight=cookies",ports[1]);
    q.headers="X-Fixture: cookie-preflight\r\n";id=webnet_submit(net,&q,completed,&r);
    check(id&&wait_result(&r,40000)&&success(&r)&&strstr(r.body,"\"preflight_count\": 1")&&strstr(r.body,"\"preflight_cookie\": \"\"")&&strstr(r.body,"secretcookie=server"),"preflight remains credentialless but actual request includes cookies");release(&r);q.headers=NULL;
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?long=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    long visible_len=webnet_cookie_get(net,document,visible,sizeof visible);
    check(done&&success(&r)&&visible_len>2400&&complete_long_cookie(visible),"complete 2400-byte Set-Cookie survives the 1023-byte legacy line limit");release(&r);
    check(webnet_cookie_set(net,document,"longcookie=; Path=/; Max-Age=0")==1,"long test cookie deletion");
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?padding=yes&long=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&filled_header(r.headers,"X-Cookie-Test-Padding",6000,'p')&&
          webnet_cookie_get(net,document,visible,sizeof visible)>=0&&complete_long_cookie(visible),
          "6 KiB response field and trailing 2400-byte Set-Cookie both remain complete");
    check(done&&success(&r)&&!strstr(r.headers,"Set-Cookie")&&!strstr(r.headers,"set-cookie"),
          "large cookie response keeps Set-Cookie out of JS-visible headers");release(&r);
    check(webnet_cookie_set(net,document,"longcookie=; Path=/; Max-Age=0")==1,"large-header test cookie deletion");
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?overlong=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&strstr(r.error,"Cookie response fields")&&webnet_cookie_get(net,document,visible,sizeof visible)>=0&&!strstr(visible,"overlong="),"oversized cookie field fails without storing a truncated value");release(&r);
}
static void header_block_tests(void) {
    struct result r={0};char path[128],label[160],url[512],visible[8192];
    static const unsigned sizes[]={6000,20000};
    for(unsigned size=0;size<2;size++)for(unsigned kind=0;kind<2;kind++) {
        snprintf(path,sizeof path,"/api/headers?bytes=%u",sizes[size]);
        uint64_t id=request(&r,0,path,kind?WEBNET_RESOURCE:WEBNET_FETCH,"GET",NULL,NULL,0,95);
        bool done=id&&wait_result(&r,40000);
        bool complete=done&&success(&r)&&r.length==10&&!memcmp(r.body,"headers-ok",10)&&
            strlen(r.headers)>sizes[size]&&strstr(r.headers,"X-Header-End: complete-after-padding\r\n");
        for(unsigned offset=0,index=0;complete&&offset<sizes[size];offset+=6000,index++) {
            char name[64];snprintf(name,sizeof name,"X-Header-Chunk-%02u",index);
            complete=filled_header(r.headers,name,MIN(6000u,sizes[size]-offset),(char)('a'+index%26));
        }
        snprintf(label,sizeof label,"%u-byte complete response headers preserve every value and late field through %s",sizes[size],kind?"resource":"Fetch");
        check(complete,label);
        check(done&&success(&r)&&!strstr(r.headers,"Set-Cookie")&&!strstr(r.headers,"set-cookie"),
              "large complete response does not expose Set-Cookie or Set-Cookie2");
        if(!done)webnet_cancel_generation(net,95);
        release(&r);
    }
    /* 65537 value bytes in several 6000-byte lines exceed the 65536-byte raw
       block bound without exceeding the independent 16383-byte line bound. */
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/headers?bytes=65537",ports[0]);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=95,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_INCLUDE};
    uint64_t id=webnet_submit(net,&q,completed,&r);bool done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&r.response_headers_length>65537,
          "complete valid headers beyond the removed inline block bound reach the native receiver");
    release(&r);
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/headers?bytes=65537&cookie=yes&invalid=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&strstr(r.error,"invalid Content-Length")&&r.length==0&&!r.headers[0],
          "a malformed late field in a large response rejects the entire response");
    check(done&&webnet_cookie_get(net,document,visible,sizeof visible)>=0&&!strstr(visible,"headeroverflow="),
          "malformed late response rolls back the earlier privileged Set-Cookie event");
    if(!done)webnet_cancel_generation(net,95);
    release(&r);
}
static uint64_t returning_request(struct result *r, const char *probe, enum webnet_credentials credentials,
                                 bool original_origin_permission, char *final_path, size_t final_capacity,
                                 char *middle_path, size_t middle_capacity) {
    char final_url[512],encoded_final[1024],middle_url[1400],encoded_middle[1800],initial_url[WEBNET_URL_MAX];
    if(original_origin_permission)
        snprintf(final_path,final_capacity,"/api/cookie?probe=%s&allow_origin=http%%3A%%2F%%2F10.0.2.2%%3A%d",probe,ports[0]);
    else snprintf(final_path,final_capacity,"/api/cookie?probe=%s&set=returnnotallowed%%3D1%%3B%%20Path%%3D%%2F",probe);
    snprintf(final_url,sizeof final_url,"http://10.0.2.2:%d%s",ports[0],final_path);
    if(!encode_query(final_url,encoded_final,sizeof encoded_final))return 0;
    snprintf(middle_path,middle_capacity,"/api/redirect?%surl=%s",
        credentials==WEBNET_CREDENTIALS_INCLUDE?"credentials=yes&":"",encoded_final);
    snprintf(middle_url,sizeof middle_url,"http://10.0.2.2:%d%s",ports[1],middle_path);
    if(!encode_query(middle_url,encoded_middle,sizeof encoded_middle))return 0;
    snprintf(initial_url,sizeof initial_url,"http://10.0.2.2:%d/api/redirect?url=%s",ports[0],encoded_middle);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=97,.url=initial_url,.origin=document,
        .method="GET",.credentials=credentials};
    return webnet_submit(net,&q,completed,r);
}
static void cors_boundary_regression_tests(void) {
    struct result r={0};char url[512],label[160],visible[8192];
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=96,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_INCLUDE};
    static const char *const duplicates[]={"origin","credentials"};
    static const char *const paddings[]={"small","large","twenty"};
    for(unsigned d=0;d<2;d++)for(unsigned p=0;p<3;p++) {
        snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cors-overflow?padding=%s&duplicate=%s",ports[1],paddings[p],duplicates[d]);
        uint64_t id=webnet_submit(net,&q,completed,&r);bool done=id&&wait_result(&r,40000);
        snprintf(label,sizeof label,"conflicting CORS %s singleton cannot hide behind %s snapshot padding",duplicates[d],paddings[p]);
        check(done&&r.status==0&&r.length==0&&strstr(r.error,d?"Credentialed CORS":"Cross-origin response"),label);
        if(!done)webnet_cancel_generation(net,96);
        release(&r);
    }
    uint64_t id;bool done;
    for(unsigned p=0;p<3;p++) {
        snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cors-overflow?padding=%s",ports[1],paddings[p]);
        id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
        snprintf(label,sizeof label,"complete credentialed CORS block with %s padding keeps late exposure and private-field filtering",paddings[p]);
        check(done&&success(&r)&&r.length==11&&!memcmp(r.body,"{\"ok\":true}",11)&&
              strstr(r.headers,"X-Exposed: visible-after-padding\r\n")&&!strstr(r.headers,"X-Private")&&
              !strstr(r.headers,"X-Snapshot-Padding"),label);
        if(!done)webnet_cancel_generation(net,96);
        release(&r);
    }

    id=request(&r,0,"/api/cookie?probe=direct-header-control",WEBNET_FETCH,"GET",NULL,NULL,0,97);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.headers,"X-Private: must-not-be-visible-after-cors-redirect")&&
          strstr(r.headers,"HTTP/Nocturne-Meta cors=0 redirected=0"),
          "an untainted same-origin control keeps basic response headers and native metadata");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    check(webnet_cookie_set(net,document,"returnprobe=original; Path=/")==1,"create redirect-return cookie control");
    char probe[128],final_path[512],middle_path[1200];unsigned long token=(unsigned long)uptime_ms();
    snprintf(probe,sizeof probe,"return-same-%lu",token);
    id=returning_request(&r,probe,WEBNET_CREDENTIALS_SAME_ORIGIN,false,final_path,sizeof final_path,middle_path,sizeof middle_path);
    done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"origin\": \"null\"")&&strstr(r.body,"\"cookie\": \"\""),
          "A to B to A retains CORS taint, serializes Origin null and does not revive same-origin Cookie");
    check(done&&success(&r)&&strstr(r.headers,"X-Exposed: visible")&&!strstr(r.headers,"X-Private"),
          "returning to document origin retains CORS-filtered response headers");
    check(done&&success(&r)&&strstr(r.headers,"HTTP/Nocturne-Meta cors=1 redirected=1"),
          "native return redirect metadata retains cors response type and redirected state");
    long length=webnet_cookie_get(net,document,visible,sizeof visible);
    check(done&&success(&r)&&length>=0&&strstr(visible,"returnprobe=original")&&!strstr(visible,"returnnotallowed="),
          "tainted same-origin credentials also refuse the returning response Set-Cookie");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    char encoded[1536],path[1600],expected_origin[128];
    bool encoded_ok=encode_query(middle_path,encoded,sizeof encoded);
    if(encoded_ok)snprintf(path,sizeof path,"/api/observations?path=%s",encoded);
    id=encoded_ok?request(&r,1,path,WEBNET_FETCH,"GET",NULL,NULL,0,97):0;done=id&&wait_result(&r,40000);
    snprintf(expected_origin,sizeof expected_origin,"\"origins\": [\"http://10.0.2.2:%d\"]",ports[0]);
    check(done&&success(&r)&&strstr(r.body,expected_origin)&&strstr(r.body,"\"cookies\": [\"\"]"),
          "the first A to B redirect still uses the document origin, not a prematurely null origin");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    snprintf(probe,sizeof probe,"return-denied-%lu",token);
    id=returning_request(&r,probe,WEBNET_CREDENTIALS_SAME_ORIGIN,true,final_path,sizeof final_path,middle_path,sizeof middle_path);
    done=id&&wait_result(&r,40000);
    check(done&&r.status==0&&r.error[0]&&r.length==0,
          "returning CORS response cannot allow the original origin instead of the required null origin");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    // The response is rejected, but the already-issued final request must also
    // have used the tainted origin rather than the original document origin.
    encoded_ok=encode_query(final_path,encoded,sizeof encoded);
    if(encoded_ok)snprintf(path,sizeof path,"/api/observations?path=%s",encoded);
    id=encoded_ok?request(&r,0,path,WEBNET_FETCH,"GET",NULL,NULL,0,97):0;done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"origins\": [\"null\"]")&&strstr(r.body,"\"cookies\": [\"\"]"),
          "rejected returning response still observes a null-Origin cookie-free final request");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    snprintf(probe,sizeof probe,"return-include-%lu",token);
    id=returning_request(&r,probe,WEBNET_CREDENTIALS_INCLUDE,false,final_path,sizeof final_path,middle_path,sizeof middle_path);done=id&&wait_result(&r,40000);
    check(done&&success(&r)&&strstr(r.body,"\"origin\": \"null\"")&&strstr(r.body,"returnprobe=original"),
          "include credentials accepts explicit null-origin permission plus ACAC after the return redirect");
    if(!done)webnet_cancel_generation(net,97);
    release(&r);
    check(webnet_cookie_set(net,document,"returnprobe=; Path=/; Max-Age=0")==1&&
          webnet_cookie_set(net,document,"returnnotallowed=; Path=/; Max-Age=0")==1,"remove redirect-return cookie controls");
}
static unsigned callback_cancel_calls;
static bool callback_cancel_borrow;
static void cancel_from_callback(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *response,void *opaque){
    (void)id;(void)opaque;callback_cancel_calls++;
    char original[512];strlcpy(original,response->final_url,sizeof original);
    webnet_cancel_generation(n,generation);
    webnet_pump(n,uptime_ms()); /* reentry must not free the borrowed response */
    callback_cancel_borrow=!strcmp(original,response->final_url)&&response->headers!=NULL;
}
static void callback_cancel_test(void){
    char url[256];snprintf(url,sizeof url,"http://10.0.2.2:%d/api/json",ports[0]);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=111,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_OMIT,.stream_response=true};
    uint64_t id=webnet_submit(net,&q,cancel_from_callback,NULL);
    bool drained=id&&wait_idle(5000);
    check(drained&&callback_cancel_calls==1&&callback_cancel_borrow&&transports()==0&&children()==1,
          "stream callback generation cancellation retains its borrowed response and reaps without reentry");
}
struct paused_stream_result {unsigned headers,chunks,ends;unsigned char *body;size_t length;bool failed;};
static void paused_stream_received(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *response,void *opaque){
    (void)generation;struct paused_stream_result *r=opaque;
    if(response->error&&*response->error)r->failed=true;
    if(response->event==WEBNET_UPLOAD){
        /* The existing transport reports the bodyless request's 0/0 upload
           before HEADERS. This test pauses received bodies, not that phase. */
        if(r->headers || r->chunks || r->ends || response->uploaded)r->failed=true;
        webnet_resume(n,id);
    }else if(response->event==WEBNET_HEADERS){
        if(r->headers++ || r->chunks || r->ends || response->status!=200)r->failed=true;
    }else if(response->event==WEBNET_CHUNK){
        if(r->headers!=1 || r->ends || response->body_len>SIZE_MAX-r->length-1){r->failed=true;return;}
        unsigned char *body=realloc(r->body,r->length+response->body_len+1);
        if(!body){r->failed=true;return;}r->body=body;
        memcpy(body+r->length,response->body,response->body_len);r->length+=response->body_len;body[r->length]=0;r->chunks++;
    }else if(response->event==WEBNET_END){if(r->headers!=1 || r->ends++)r->failed=true;}
    else r->failed=true;
}
static void paused_stream_transport_test(void){
    struct paused_stream_result results[6]={0};uint64_t ids[6]={0};
    /* Use the existing immutable response. /api/json intentionally includes
       an actual-request counter and cannot be compared byte-for-byte across
       separate requests. Keep the full-byte comparison, not a JSON subset. */
    char url[256];snprintf(url,sizeof url,"http://10.0.2.2:%d/index.html",ports[0]);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=112,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_OMIT,.stream_response=true};
    bool submitted=true;for(unsigned i=0;i<5;i++){ids[i]=webnet_submit(net,&q,paused_stream_received,&results[i]);submitted&=ids[i]!=0;}
    uint64_t until=uptime_ms()+5000;unsigned headers=0;
    do{webnet_pump(net,uptime_ms());headers=0;for(unsigned i=0;i<5;i++)headers+=results[i].headers;
        if(headers==5&&!transports())break;msleep(5);
    }while(uptime_ms()<until);
    check(submitted&&headers==5&&transports()==0&&children()==1,"native small paused response writers retire while their unread bounded channels remain owned");
    bool unread=webnet_busy(net);for(unsigned i=0;i<5;i++)unread&=!results[i].chunks&&!results[i].ends&&!results[i].failed;
    check(unread,"native transport retirement does not publish unread chunks or a fabricated END");
    ids[5]=webnet_submit(net,&q,paused_stream_received,&results[5]);until=uptime_ms()+5000;
    while(!results[5].headers&&uptime_ms()<until){webnet_pump(net,uptime_ms());msleep(5);}
    check(ids[5]&&results[5].headers==1&&!results[5].failed,"sixth same-origin Fetch progresses while five response bodies remain paused");
    until=uptime_ms()+5000;
    while(webnet_busy(net)&&uptime_ms()<until){for(unsigned i=0;i<6;i++)if(ids[i])webnet_resume(net,ids[i]);webnet_pump(net,uptime_ms());msleep(5);}
    bool drained=!webnet_busy(net)&&transports()==0&&children()==1;
    for(unsigned i=0;i<6;i++)drained&=results[i].headers==1&&results[i].chunks>0&&results[i].ends==1&&!results[i].failed;
    check(drained,"native resume preserves ordered headers/chunks/END and reaps all transport owners");
    static struct result reference;
    uint64_t reference_id=request(&reference,0,"/index.html",WEBNET_FETCH,"GET",NULL,NULL,0,113);
    bool exact=reference_id&&wait_result(&reference,5000)&&success(&reference);
    for(unsigned i=0;i<6;i++)exact&=results[i].length==reference.length&&results[i].body&&reference.body&&
        !memcmp(results[i].body,reference.body,results[i].length);
    check(exact,"every byte of a native retained paused response survives transport reaping");
    webnet_cancel_generation(net,112);webnet_cancel_generation(net,113);
    for(unsigned i=0;i<6;i++)free(results[i].body);release(&reference);
    check(!webnet_busy(net)&&transports()==0&&children()==1,"native paused response teardown leaves only the reusable service owner");
}
struct paced_stream {size_t bytes;unsigned headers,chunks,ends;bool automatic,failed;};
static void paced_received(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *response,void *opaque){
    (void)generation;struct paced_stream *r=opaque;
    if(response->error&&*response->error)r->failed=true;
    if(response->event==WEBNET_HEADERS){if(r->headers++||response->status!=200)r->failed=true;}
    else if(response->event==WEBNET_CHUNK){
        if(r->headers!=1||r->ends)r->failed=true;
        const unsigned char *p=response->body;
        for(size_t i=0;i<response->body_len;i++)if(p[i]!=(unsigned char)((r->bytes+i)*37u+11u))r->failed=true;
        r->bytes+=response->body_len;r->chunks++;
    }else if(response->event==WEBNET_END){if(r->headers!=1||r->ends++)r->failed=true;}
    else if(response->event!=WEBNET_UPLOAD)r->failed=true;
    if(r->automatic||response->event==WEBNET_UPLOAD)webnet_resume(n,id);
}
static int setup_service_pid(void){
    struct n_procinfo tasks[256];int count=proclist(tasks,256);
    for(int i=0;i<count;i++)if(tasks[i].ppid==getpid()&&strstr(tasks[i].name,"webfetch"))return tasks[i].pid;
    return -1;
}
static int setup_service_threads(int service_pid,bool blocked_only){
    struct n_procinfo tasks[256];int count=proclist(tasks,256),live=0;
    if(count<0)return -1;
    for(int i=0;i<count;i++)if(tasks[i].ppid==service_pid&&tasks[i].state!=3&&(!blocked_only||tasks[i].state==2))live++;
    return live;
}
static void setup_unexpected_response(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *response,void *opaque){
    (void)n;(void)id;(void)generation;(void)response;(*(unsigned *)opaque)++;
}
static void setup_cancel_service_test(void){
    /* Forty-eight entered setups exceed the service's thirty-two slots if
       closing a request channel cannot interrupt native ARP/connect waits.
       Distinct ports avoid the per-origin request admission limit. */
    int service_pid=setup_service_pid();unsigned submitted=0,entered=0,callbacks=0;
    bool admitted=service_pid>0,reclaimed=true,quiet=true;
    uint64_t settled=uptime_ms()+150;
    while(setup_service_threads(service_pid,false)>0&&uptime_ms()<settled)msleep(2);
    admitted&=setup_service_threads(service_pid,false)==0;
    for(unsigned batch=0;batch<8;batch++){
        uint64_t ids[6]={0};int previous=setup_service_threads(service_pid,true);
        for(unsigned i=0;i<6;i++){
            char url[128];snprintf(url,sizeof url,"http://10.0.2.250:%u/setup-cancel",49152u+batch*6u+i);
            struct webnet_request q={.kind=WEBNET_RESOURCE,.generation=116,.url=url,.origin=document,
                .method="GET",.credentials=WEBNET_CREDENTIALS_OMIT};
            ids[i]=webnet_submit(net,&q,setup_unexpected_response,&callbacks);submitted+=ids[i]!=0;
        }
        uint64_t until=uptime_ms()+500;
        while(transports()<6&&uptime_ms()<until){webnet_pump(net,uptime_ms());msleep(2);}
        /* Keep the requests live after sending their prefixes, so cancellation
           exercises the worker's setup syscall, rather than an unread queue. */
        pump_for(60);
        int active=setup_service_threads(service_pid,true);until=uptime_ms()+500;
        while(active<previous+6&&uptime_ms()<until){webnet_pump(net,uptime_ms());msleep(2);active=setup_service_threads(service_pid,true);}
        if(previous>=0&&active>=previous+6&&transports()==6)entered+=6;else admitted=false;
        quiet&=callbacks==0;
        for(unsigned i=0;i<6;i++)if(ids[i])webnet_cancel(net,ids[i]);
        until=uptime_ms()+150;
        do{webnet_pump(net,uptime_ms());active=setup_service_threads(service_pid,false);
            if(!webnet_busy(net)&&active==0)break;msleep(2);
        }while(uptime_ms()<until);
        reclaimed&=!webnet_busy(net)&&transports()==0&&active==0;
    }
    check(submitted==48&&entered==48&&admitted&&quiet,
          "forty-eight unresponsive setup requests enter the real shared service before channel cancellation");
    check(reclaimed&&callbacks==0,
          "cancelled native setups promptly retire service threads and never publish a response callback");
    static struct result healthy;
    uint64_t begin=uptime_ms();
    uint64_t id=request(&healthy,0,"/api/json",WEBNET_RESOURCE,"GET",NULL,NULL,0,117);
    bool done=id&&wait_result(&healthy,2000);uint64_t elapsed=uptime_ms()-begin;
    check(done&&success(&healthy)&&elapsed<2000&&children()==1&&setup_service_pid()==service_pid,
          "normal fixture fetch succeeds promptly in the same service after more than thirty-two cancelled setups");
    printf("network-setup-cancel: submitted=%u entered=%u callbacks=%u live-threads=%d local-ms=%lu service-pid=%d\n",
        submitted,entered,callbacks,setup_service_threads(service_pid,false),(unsigned long)elapsed,service_pid);
    webnet_cancel_generation(net,116);webnet_cancel_generation(net,117);release(&healthy);
}
static void pacing_tests(void){
    char url[256];snprintf(url,sizeof url,"http://10.0.2.2:%d/api/pattern?n=1048576",ports[0]);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=114,.url=url,.origin=document,
        .method="GET",.credentials=WEBNET_CREDENTIALS_OMIT,.stream_response=true};
    struct paced_stream stream={0};uint64_t id=webnet_submit(net,&q,paced_received,&stream);
    uint64_t until=uptime_ms()+5000;
    while(id&&!stream.headers&&uptime_ms()<until){webnet_pump(net,uptime_ms());webnet_wait(net,-1,20);}
    check(id&&stream.headers==1&&!stream.chunks&&!stream.ends&&!stream.failed,
          "real streaming response pauses after headers without publishing body bytes");
    /* Let the worker fill its bounded ahead window. A different request must
       still use the shared process and deliver its terminal response. */
    pump_for(150);
    static struct result dependency;
    uint64_t other=request(&dependency,0,"/api/json",WEBNET_RESOURCE,"GET",NULL,NULL,0,115);
    check(other&&wait_result(&dependency,5000)&&success(&dependency)&&!stream.chunks&&children()==1,
          "a paused large stream cannot block another shared worker thread's complete response");
    release(&dependency);
    stream.automatic=true;webnet_resume(net,id);unsigned maximum=0,pumps=0;
    uint64_t begin=uptime_ms();until=begin+12000;
    while(webnet_busy(net)&&uptime_ms()<until){
        unsigned before=stream.chunks;webnet_pump(net,uptime_ms());pumps++;
        maximum=MAX(maximum,stream.chunks-before);
        if(webnet_busy(net))webnet_wait(net,-1,100);
    }
    check(!webnet_busy(net)&&stream.bytes==1048576&&stream.headers==1&&stream.ends==1&&!stream.failed,
          "real streamed byte credit preserves every pattern byte through pause resume and END");
    check(maximum>1,"one native pump drains multiple ready 4KiB stream frames without per-frame sleep");
    printf("network-pacing: bytes=%lu chunks=%u pumps=%u max-chunks-per-pump=%u drain-ms=%lu\n",
        (unsigned long)stream.bytes,stream.chunks,pumps,maximum,(unsigned long)(uptime_ms()-begin));
    webnet_cancel_generation(net,114);webnet_cancel_generation(net,115);
    check(!webnet_busy(net)&&transports()==0&&children()==1,"stream drain retires all channels while retaining one reusable service");
    setup_cancel_service_test();
}
static volatile bool udp_wait_started;
static int udp_wait_handle,udp_wait_result,udp_wait_errno;
static void udp_wait_closed(void *unused) {
    (void)unused;char byte;udp_wait_started=true;
    udp_wait_result=udp_recvfrom(udp_wait_handle,&byte,1,3000,NULL);
    udp_wait_errno=errno;
}
static int udp_capacity_test(void) {
    if(!net_wait_up(20000)){puts("FAIL UDP network did not come up");return 1;}
    int sockets[64];for(size_t i=0;i<64;i++)sockets[i]=-1;
    unsigned opened=0;
    for(size_t i=0;i<64;i++){sockets[i]=udp_socket(0);if(sockets[i]<0)break;opened++;}
    check(opened==64,"UDP concurrent live sockets are not rejected at the former global eight-slot limit");
    uint32_t ip=0;
    check(net_resolve("example.com",&ip)==0&&ip!=0,"DNS has its own live queue while all user sockets remain open");
    for(size_t i=0;i<64;i++)if(sockets[i]>=0)check(udp_close(sockets[i])==0,"release live UDP queue");
    int first=udp_socket(49152);
    check(first>=0,"explicit UDP port allocates after queue release");
    int duplicate=udp_socket(49152);
    check(duplicate<0,"explicit live UDP port still excludes duplicate binding");
    if(duplicate>=0)udp_close(duplicate);
    if(first>=0){check(udp_close(first)==0,"close explicit UDP port");check(udp_close(first)<0,"closed UDP handle is invalid");}
    first=udp_socket(49152);check(first>=0,"released UDP port is reusable");if(first>=0)udp_close(first);
    udp_wait_handle=udp_socket(0);
    int tid=udp_wait_handle>=0?thread_create(udp_wait_closed,NULL):-1;
    check(tid>=0,"native UDP blocked receive thread starts");
    if(tid>=0) {
        while(!udp_wait_started)msleep(1);
        msleep(30);udp_close(udp_wait_handle);
        int replacement=udp_socket(0);
        check(replacement==udp_wait_handle,"closed slot can be reused while its old receive is sleeping");
        thread_join(tid);
        check(udp_wait_result==-1&&udp_wait_errno==EBADF,"blocked receive never consumes a replacement socket generation");
        if(replacement>=0)udp_close(replacement);
    } else if(udp_wait_handle>=0)udp_close(udp_wait_handle);
    printf("webnet-udp-capacity: %d failed\n",failed);return failed!=0;
}
static volatile bool tcp_wait_started,tcp_wait_finished;
static int tcp_wait_handle,tcp_wait_result,tcp_wait_errno;
static void tcp_wait_closed(void *unused) {
    (void)unused;char byte;tcp_wait_started=true;
    tcp_wait_result=tcp_recv(tcp_wait_handle,&byte,1,3000);
    tcp_wait_errno=errno;tcp_wait_finished=true;
}
static int tcp_capacity_test(void) {
    int sockets[24];
    for(int round=0;round<2;round++) {
        unsigned opened=0;
        for(size_t i=0;i<24;i++)sockets[i]=-1;
        for(size_t i=0;i<24;i++) {
            sockets[i]=tcp_connect(0x0a000202,(uint16_t)ports[0],5000);
            if(sockets[i]<0)break;
            opened++;
        }
        check(opened==24,round?"TCP closing tuples do not block the next concurrent group":"TCP live connections are not limited to sixteen global slots");
        for(size_t i=0;i<24;i++)if(sockets[i]>=0)check(tcp_close(sockets[i])==0,"release TCP ownership without stealing its closing tuple");
    }
    /* A socket-local wait queue must wake its blocked sibling on close,
       without publishing bytes from a fresh connection into the old receive. */
    tcp_wait_handle=tcp_connect(0x0a000202,(uint16_t)ports[0],5000);
    tcp_wait_started=tcp_wait_finished=false;
    int tid=tcp_wait_handle>=0?thread_create(tcp_wait_closed,NULL):-1;
    check(tid>=0,"native TCP socket-local blocked receive starts");
    if(tid>=0) {
        while(!tcp_wait_started)msleep(1);
        msleep(30);
        check(!tcp_wait_finished,"idle TCP receive remains pending before close");
        check(tcp_close(tcp_wait_handle)==0,"close wakes the socket-local receive queue");
        int replacement=tcp_connect(0x0a000202,(uint16_t)ports[0],5000);
        check(replacement>=0,"fresh TCP connection progresses while old receive resumes");
        thread_join(tid);
        check(tcp_wait_finished&&tcp_wait_result==-1&&tcp_wait_errno==EBADF,
              "blocked TCP receive retains its closed connection generation");
        if(replacement>=0)tcp_close(replacement);
    } else if(tcp_wait_handle>=0)tcp_close(tcp_wait_handle);
    printf("webnet-tcp-capacity: %d failed\n",failed);return failed!=0;
}
int main(int argc, char **argv) {
    if(argc==2&&!strcmp(argv[1],"--udp-capacity"))return udp_capacity_test();
    bool tls = argc == 2 && !strcmp(argv[1], "--tls");
    bool boundaries=argc==2&&!strcmp(argv[1],"--performance-boundaries");
    bool tcp_capacity=argc==2&&!strcmp(argv[1],"--tcp-capacity");
    bool pacing=argc==2&&!strcmp(argv[1],"--network-pacing");
    if (argc > 1 && !tls&&!boundaries&&!tcp_capacity&&!pacing) { puts("usage: webnettest [--tls|--performance-boundaries|--udp-capacity|--tcp-capacity|--network-pacing]"); return 1; }
    if (tls) {
        if (!net_wait_up(20000)) { puts("FAIL webnet network did not come up"); return 1; }
        net = webnet_create();
        if (!net) { puts("FAIL webnet allocation"); return 1; }
        tls_tests(); webnet_free(net);
        printf("webnettest: %d failed\n", failed);
        return failed != 0;
    }
    FILE *f = fopen("/data/tests/webports", "r");
    char text[64] = "";
    if (f) { size_t n = fread(text, 1, sizeof text - 1, f); text[n] = 0; }
    if (!f || sscanf(text, "%d %d", &ports[0], &ports[1]) != 2 ||
        ports[0] < 1 || ports[0] > 65535 || ports[1] < 1 || ports[1] > 65535) {
        if (f) fclose(f);
        puts("FAIL webnet fixture ports missing"); return 1;
    }
    fclose(f);
    if (!net_wait_up(20000)) { puts("FAIL webnet network did not come up"); return 1; }
    if(tcp_capacity)return tcp_capacity_test();
    snprintf(document, sizeof document, "http://10.0.2.2:%d/index.html", ports[0]);
    net = webnet_create();
    if (!net) { puts("FAIL webnet allocation"); return 1; }
    if(pacing){pacing_tests();top_partition_tests();cache_preflight_tests();}
    else if(boundaries){concurrency_tests();callback_cancel_test();paused_stream_transport_test();}
    else {http_tests(); debug_transport_tests(); body_presence_tests(); fetch_contract_tests(); cache_tests(); concurrency_tests(); cors_tests(); redirect_tests(); long_url_tests(); file_tests(); cookie_tests(); header_block_tests(); cors_boundary_regression_tests(); limit_tests();}
    webnet_free(net);net=NULL;
    check(children()==0,"destroying webnet reaps its persistent service and all request threads");
    printf("webnettest: %d failed\n", failed);
    return failed != 0;
}
