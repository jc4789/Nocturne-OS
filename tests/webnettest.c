/* Real Nocturne worker/pipe/network tests against the runner's two HTTP fixture origins. */
#include "nocturne.h"
#include "webnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct result {
    int calls, status;
    uint64_t id, generation;
    char *body;
    size_t length;
    char url[WEBNET_URL_MAX], headers[WEBNET_HEADERS_MAX], error[160];
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
static uint64_t request(struct result *r, int port, const char *path, enum webnet_kind kind,
                        const char *method, const char *headers, const void *body, size_t length,
                        uint64_t generation) {
    char url[WEBNET_URL_MAX]; snprintf(url, sizeof url, "http://10.0.2.2:%d%s", ports[port], path);
    struct webnet_request q = {kind, generation, url, document, method, headers, body, length, false, WEBNET_CREDENTIALS_OMIT};
    return webnet_submit(net, &q, completed, r);
}
static bool success(const struct result *r) { return r->calls == 1 && r->status == 200 && !r->error[0]; }
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
static void concurrency_tests(void) {
    struct result r[3] = {{0}};
    uint64_t ids[3];
    for (int i = 0; i < 3; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=800", WEBNET_FETCH, "GET", NULL, NULL, 0, 20);
    webnet_pump(net, uptime_ms());
    check(ids[0] && ids[1] && ids[2] && children() == 2, "at most two workers for three queued requests");
    bool good = true; uint64_t until = uptime_ms() + 6000;
    while (webnet_busy(net) && uptime_ms() < until) {
        webnet_pump(net, uptime_ms());
        if (children() > 2) good = false;
        msleep(5);
    }
    for (int i = 0; i < 3; i++) good = good && success(&r[i]);
    check(good && !webnet_busy(net) && children() == 0, "queued requests complete and workers are reaped");
    webnet_cancel_generation(net, 20);
    for (int i = 0; i < 3; i++) release(&r[i]);

    ids[0] = request(&r[0], 0, "/api/slow?ms=5000", WEBNET_FETCH, "GET", NULL, NULL, 0, 21);
    pump_for(100); webnet_cancel(net, ids[0]);
    check(ids[0] && wait_idle(3000) && !r[0].calls && children() == 0, "single cancellation suppresses callback and asynchronously reaps");
    release(&r[0]);
    for (int i = 0; i < 2; i++) ids[i] = request(&r[i], 0, "/api/slow?ms=5000", WEBNET_FETCH, "GET", NULL, NULL, 0, 22);
    pump_for(100); webnet_cancel_generation(net, 22);
    check(ids[0] && ids[1] && !webnet_busy(net) && !r[0].calls && !r[1].calls && children() == 0,
          "document cancellation synchronously kills and reaps both workers");
    release(&r[0]); release(&r[1]);
}
static void cors_tests(void) {
    struct result r = {0};
    uint64_t id = request(&r, 1, "/api/cors", WEBNET_FETCH, "GET", NULL, NULL, 0, 30);
    check(id && wait_result(&r, 40000) && success(&r) && strstr(r.headers, "X-Exposed: visible") &&
          !strstr(r.headers, "X-Private"), "cross-origin CORS and response-header exposure");
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
    check(done && strstr(r.error, "16 MiB") && r.length == 0, "response larger than 16 MiB is rejected before delivery");
    if (!done) webnet_cancel_generation(net, 59);
    release(&r);
    uint64_t started = uptime_ms();
    id = request(&r, 0, "/api/slow?ms=35000", WEBNET_FETCH, "GET", NULL, NULL, 0, 60);
    done = id && wait_result(&r, 35000);
    uint64_t elapsed = uptime_ms() - started;
    check(done && r.error[0] && strstr(r.error, "deadline") && elapsed >= 29500 && elapsed < 34000,
          "30-second total deadline kills and reaps a stalled request");
    check(!webnet_busy(net) && children() == 0, "deadline leaves no worker or pending request");
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
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?padding=yes&long=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    long visible_len=webnet_cookie_get(net,document,visible,sizeof visible);
    check(done&&success(&r)&&visible_len>2400&&strstr(visible,"longcookie="),"complete Set-Cookie survives 4KiB headers and 1023-byte legacy line limit");release(&r);
    check(webnet_cookie_set(net,document,"longcookie=; Path=/; Max-Age=0")==1,"long test cookie deletion");
    snprintf(url,sizeof url,"http://10.0.2.2:%d/api/cookie?overlong=yes",ports[0]);
    id=webnet_submit(net,&q,completed,&r);done=id&&wait_result(&r,40000);
    check(done&&strstr(r.error,"Cookie response fields")&&webnet_cookie_get(net,document,visible,sizeof visible)>=0&&!strstr(visible,"overlong="),"oversized cookie field fails without storing a truncated value");release(&r);
}
int main(int argc, char **argv) {
    bool tls = argc == 2 && !strcmp(argv[1], "--tls");
    if (argc > 1 && !tls) { puts("usage: webnettest [--tls]"); return 1; }
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
    snprintf(document, sizeof document, "http://10.0.2.2:%d/index.html", ports[0]);
    net = webnet_create();
    if (!net) { puts("FAIL webnet allocation"); return 1; }
    http_tests(); concurrency_tests(); cors_tests(); redirect_tests(); file_tests(); cookie_tests(); limit_tests();
    webnet_free(net);
    printf("webnettest: %d failed\n", failed);
    return failed != 0;
}
