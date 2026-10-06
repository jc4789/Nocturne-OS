/* jstest: the real web_live/QuickJS path, compiled by TinyCC inside Nocturne.
   The embedder serves deterministic offline resources through the public host
   contract. HTTP workers and CORS are covered separately by webnettest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"

#define BASE "http://fixture.test/dir/index.html"
#define VW 800
#define VH 600
#define SLOTS 32
#define MARKS 192
#define PRELUDE "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}" \
                "function mark(n){console.log('OK '+n);}"
#define START "<!doctype html><html><head><script>" PRELUDE "</script>"

struct asset { const char *path, *body; int delay_ms; };
static const struct asset assets[] = {
    {"/dir/style.css", "#target {height:7px;color:rgb(255,0,0)}", 15},
    {"/dir/classic.js", "check('external-classic',document.getElementById('future')!==null);"
        "check('blocking-css',getComputedStyle(document.getElementById('target')).height==='7px');"
        "globalThis.classicRan=true;", 0},
    {"/dir/defer.js", "globalThis.deferRan=true;check('defer-after-parser',document.getElementById('future')!==null);", 0},
    {"/dir/async.js", "globalThis.asyncRan=true;mark('async-loaded');", 0},
    {"/dir/main.mjs", "import {cycle} from './a.mjs';check('module-cycle',cycle()==='AB');"
        "check('module-current-script',document.currentScript===null);"
        "check('module-import-meta',import.meta.url==='http://fixture.test/dir/main.mjs');"
        "const value=await import('./dynamic.mjs');check('dynamic-import',value.answer===42);"
        "await new Promise(resolve=>setTimeout(resolve,4));globalThis.moduleRan=true;mark('module-top-level-await');", 0},
    {"/dir/a.mjs", "import {b} from './b.mjs';export function a(){return 'A';}export function cycle(){return a()+b();}", 0},
    {"/dir/b.mjs", "import {a} from './a.mjs';export function b(){return a()==='A'?'B':'?';}", 0},
    {"/dir/dynamic.mjs", "globalThis.dynamicEvaluations=(globalThis.dynamicEvaluations||0)+1;export const answer=42;", 0},
    {"/dir/mime.mjs", "import {value} from './mimedep.mjs';check('mime-import',value===42);mark('mime-module');", 0},
    {"/dir/mimedep.mjs", "export const value=42;", 0},
    {"/dir/order-one.js", "globalThis.dynamicOrder.push(1);", 15},
    {"/dir/order-two.js", "globalThis.dynamicOrder.push(2);", 0},
    {"/dir/api/json", "{\"ok\":true,\"value\":42}", 0},
    {"/dir/api/post", "Nocturne request body", 0},
    {"/dir/api/slow", "{\"late\":true}", 60000},
};

struct queued {
    bool used;
    uint64_t id, due;
    int kind;
    char url[2048], method[16], headers[1024], body[256];
    size_t body_len;
};
struct fixture {
    web_doc *doc;
    struct queued queue[SLOTS];
    char marks[MARKS][96];
    int nmarks, js_failures, errors, requests, cancellations, completions, sync_loads;
    int outside_running, navigations;
    int scroll_x, scroll_y;
    bool expected_errors;
    const char *module_mime_headers;
    struct { char *url; void *data; size_t len; uint64_t entry; bool manual; } history[16];
    int history_pos, history_length, history_delta;
    uint64_t history_serial;
    bool history_pending;
    char navigation[2048], post[256], last_error[512];
};
static struct fixture fixture;
static int total, failed;
static uint32_t pixels[VW * VH];

static void test_check(const char *name, bool ok) {
    total++;
    if (!ok) { printf("FAIL %s\n", name); fflush(stdout); failed++; }
}
static const struct asset *lookup(const char *url) {
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : url;
    if (!path) path = "/";
    for (size_t i = 0; i < sizeof assets / sizeof *assets; i++)
        if (!strcmp(path, assets[i].path)) return &assets[i];
    return NULL;
}
static bool has_mark(const struct fixture *f, const char *name) {
    for (int i = 0; i < f->nmarks; i++) if (!strcmp(f->marks[i], name)) return true;
    return false;
}
static int queued_count(const struct fixture *f) {
    int count = 0;
    for (int i = 0; i < SLOTS; i++) count += f->queue[i].used;
    return count;
}
static void receive_log(void *opaque, int level, const char *message) {
    struct fixture *f = opaque;
    if (!strncmp(message, "OK ", 3)) {
        if (f->nmarks < MARKS) snprintf(f->marks[f->nmarks++], sizeof f->marks[0], "%s", message + 3);
    } else if (!strncmp(message, "FAIL ", 5)) {
        printf("%s\n", message); fflush(stdout); f->js_failures++; failed++; total++;
    } else if (level == 2) {
        f->errors++;
        snprintf(f->last_error, sizeof f->last_error, "%s", message);
        if (!f->expected_errors) printf("jstest: unexpected JavaScript error: %s\n", message);
    }
}
static bool request(void *opaque, const struct web_request *r) {
    struct fixture *f = opaque;
    for (int i = 0; i < SLOTS; i++) if (!f->queue[i].used) {
        struct queued *q = &f->queue[i];
        memset(q, 0, sizeof *q);
        q->used = true; q->id = r->id; q->kind = r->kind;
        snprintf(q->url, sizeof q->url, "%s", r->url);
        snprintf(q->method, sizeof q->method, "%s", r->method ? r->method : "GET");
        snprintf(q->headers, sizeof q->headers, "%s", r->headers ? r->headers : "");
        q->body_len = r->body_len;
        size_t n = r->body_len < sizeof q->body - 1 ? r->body_len : sizeof q->body - 1;
        if (n) memcpy(q->body, r->body, n);
        q->body[n] = 0;
        const struct asset *a = lookup(q->url);
        q->due = uptime_ms() + (a ? a->delay_ms : 0);
        f->requests++;
        return true;
    }
    return false;
}
static void cancel(void *opaque, uint64_t id) {
    struct fixture *f = opaque;
    for (int i = 0; i < SLOTS; i++) if (f->queue[i].used && f->queue[i].id == id) {
        f->queue[i].used = false; f->cancellations++; break;
    }
}
static bool sync_load(void *opaque, const char *url, int kind, struct web_response *r) {
    struct fixture *f = opaque;
    f->sync_loads++;
    if (!f->doc || !web_script_running(f->doc) || kind != WEB_RESOURCE_MODULE) f->outside_running++;
    const struct asset *a = lookup(url);
    memset(r, 0, sizeof *r);
    snprintf(r->url, sizeof r->url, "%s", url);
    snprintf(r->headers, sizeof r->headers, "HTTP/1.1 200 OK\r\n%s\r\n", f->module_mime_headers ? f->module_mime_headers : "Content-Type: text/javascript\r\n");
    r->status = a ? 200 : 404;
    r->body = strdup(a ? a->body : "not found");
    r->body_len = strlen(r->body ? r->body : "");
    return r->body != NULL;
}
static void navigate(void *opaque, const char *url, const char *post) {
    struct fixture *f = opaque;
    if (!f->doc || !web_script_running(f->doc)) f->outside_running++;
    f->navigations++;
    snprintf(f->navigation, sizeof f->navigation, "%s", url);
    snprintf(f->post, sizeof f->post, "%s", post ? post : "");
    /* Like the browser, defer document replacement until the JS task returns. */
}
static void scroll_position(void *opaque, int *x, int *y) {
    struct fixture *f = opaque; *x = f->scroll_x; *y = f->scroll_y;
}
static bool history_host(void *opaque, int op, const char *url, const void *data, size_t len, int delta, struct web_history *out) {
    struct fixture *f = opaque; int p = f->history_pos;
    if (op == WEB_HISTORY_PUSH || op == WEB_HISTORY_REPLACE) {
        if (op == WEB_HISTORY_PUSH && p == 15) return false;
        char *copy = strdup(url); void *state = len ? malloc(len) : NULL;
        if (!copy || (len && !state)) { free(copy); free(state); return false; }
        if (len) memcpy(state, data, len);
        if (!web_set_url(f->doc, url)) { free(copy); free(state); return false; }
        bool manual = f->history[p].manual;
        if (op == WEB_HISTORY_PUSH) {
            p++;
            for (int i = p; i < f->history_length; i++) { free(f->history[i].url); free(f->history[i].data); memset(&f->history[i],0,sizeof f->history[i]); }
            f->history_length = p + 1; f->history_pos = p;
        } else { free(f->history[p].url); free(f->history[p].data); }
        f->history[p].url = copy; f->history[p].data = state; f->history[p].len = len;
        f->history[p].entry = ++f->history_serial; f->history[p].manual = manual;
    } else if (op == WEB_HISTORY_GO) { f->history_delta = delta; f->history_pending = true; }
    else if (op == WEB_HISTORY_SCROLL) f->history[p].manual = delta != 0;
    *out = (struct web_history){.entry=f->history[p].entry,.length=f->history_length,.manual_scroll=f->history[p].manual,.state=f->history[p].data,.state_len=f->history[p].len};
    return true;
}
static void deliver(struct fixture *f) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < SLOTS; i++) if (f->queue[i].used && f->queue[i].due <= now) {
        struct queued q = f->queue[i]; f->queue[i].used = false;
        const struct asset *a = lookup(q.url);
        struct web_response r; memset(&r, 0, sizeof r);
        r.status = a ? 200 : 404;
        snprintf(r.url, sizeof r.url, "%s", q.url);
        const char *mime = q.kind == WEB_RESOURCE_MODULE || q.kind == WEB_RESOURCE_SCRIPT ? "text/javascript" :
                           q.kind == WEB_RESOURCE_CSS ? "text/css" : "application/json";
        snprintf(r.headers, sizeof r.headers, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nX-Fixture: yes\r\n\r\n", mime);
        if (q.kind == WEB_RESOURCE_MODULE && f->module_mime_headers)
            snprintf(r.headers, sizeof r.headers, "HTTP/1.1 200 OK\r\n%s\r\n", f->module_mime_headers);
        r.body = strdup(a ? a->body : "not found"); r.body_len = strlen(r.body ? r.body : "");
        if (!strcmp(q.url, "http://fixture.test/dir/api/post") &&
            (strcmp(q.method, "POST") || strcmp(q.body, "Nocturne request body") || q.body_len != 21 || !strstr(q.headers, "x-fixture: yes"))) {
            snprintf(r.error, sizeof r.error, "Invalid POST request in the fixture");
        }
        int marks = f->nmarks;
        if (web_script_running(f->doc)) f->outside_running++;
        web_resource_loaded(f->doc, q.id, &r);
        if (f->nmarks != marks || web_script_running(f->doc)) f->outside_running++;
        /* Completion has to copy: its borrowed body is destroyed immediately. */
        if (r.body) memset(r.body, '!', r.body_len);
        free(r.body); f->completions++;
    }
}
static web_doc *open_case_url(const char *html, bool expected_errors, const char *url) {
    memset(&fixture, 0, sizeof fixture); fixture.expected_errors = expected_errors;
    struct web_host host = {.opaque=&fixture, .request=request, .cancel=cancel, .sync_load=sync_load,
                            .navigate=navigate, .console=receive_log, .scroll=scroll_position,.history=history_host};
    fixture.history_length = 1; fixture.history_serial = 1;
    fixture.history[0].url = strdup(url); fixture.history[0].entry = 1;
    fixture.doc = web_live(html, strlen(html), url, "utf-8", &host);
    test_check("web-live-allocation", fixture.doc != NULL);
    return fixture.doc;
}
static web_doc *open_case(const char *html, bool expected_errors) { return open_case_url(html, expected_errors, BASE); }
static void step(struct fixture *f) {
    if (f->history_pending) {
        f->history_pending = false;
        int64_t p = (int64_t)f->history_pos + f->history_delta;
        if (f->history_delta && p >= 0 && p < f->history_length) {
            char old[2048]; snprintf(old,sizeof old,"%s",web_url(f->doc)); f->history_pos = p;
            web_set_url(f->doc,f->history[p].url); web_history_event(f->doc,old,true);
        }
    }
    deliver(f);
    web_tick(f->doc, uptime_ms());
    if (web_dirty(f->doc)) {
        web_layout(f->doc, VW, VH);
        canvas_t c; gfx_init(&c, pixels, VW, VH, VW);
        web_paint(f->doc, &c, 0, 0, VW, VH, 0, f->scroll_y);
    }
}
static bool pump(const char *marker, unsigned timeout_ms) {
    uint64_t end = uptime_ms() + timeout_ms;
    while (uptime_ms() < end) {
        step(&fixture);
        if (marker && has_mark(&fixture, marker)) return true;
        if (!marker && !queued_count(&fixture) && web_deadline(fixture.doc) < 0) return true;
        msleep(1);
    }
    return false;
}
static void require_marks(const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) test_check(names[i], has_mark(&fixture, names[i]));
}
static void close_case(void) {
    if (fixture.doc) { web_free(fixture.doc); fixture.doc = NULL; }
    for (int i = 0; i < fixture.history_length; i++) { free(fixture.history[i].url); free(fixture.history[i].data); }
    fixture.history_length = 0;
}
static char *script_page(const char *source) {
    const char *a = START "</head><body style='margin:0'><div id=target style='height:20px'></div><div id=sentinel></div><script>";
    const char *b = "</script></body></html>";
    size_t n = strlen(a) + strlen(source) + strlen(b) + 1;
    char *page = malloc(n);
    if (page) snprintf(page, n, "%s%s%s", a, source, b);
    return page;
}

/* Isolated API regressions supplement, never replace, public-site GUI checks. */
static void external_case_expected(const char *file, const char *tail, const char *url, int expected_errors) {
    char path[160]; snprintf(path, sizeof path, "/data/tests/%s", file);
    FILE *f = fopen(path, "rb"); test_check(file, f != NULL); if (!f) return;
    if (fseek(f, 0, SEEK_END)) { fclose(f); test_check("api-seek", false); return; }
    long len = ftell(f); rewind(f);
    if (len < 0 || len > 512 * 1024) { fclose(f); test_check("api-size", false); return; }
    char *source = malloc((size_t)len + strlen(tail) + 1);
    test_check("api-source-allocation", source != NULL); if (!source) { fclose(f); return; }
    size_t read = fread(source, 1, (size_t)len, f); fclose(f);
    source[read] = 0; strcat(source, tail);
    char *page = script_page(source); free(source);
    test_check("api-page-allocation", page != NULL); if (!page) return;
    if (open_case_url(page, expected_errors != 0, url)) {
        test_check(file, pump("api-done", 15000));
        test_check("api-expected-exceptions", fixture.errors == expected_errors && fixture.js_failures == 0);
        close_case();
    }
    free(page);
}
static void external_case(const char *file, const char *tail, const char *url) { external_case_expected(file,tail,url,0); }
static void test_platform(void) {
    external_case("js_url_cases.js", ";for(const f of __urlTestResults.failures)console.log('FAIL url '+f.name+' '+f.error);check('url-count',__urlTestResults.total===32);mark('api-done');", BASE);
    external_case("js_crypto_cases.js", ";runCryptoCases().then(n=>{check('crypto-count',n>100);mark('api-done');},e=>{console.log('FAIL crypto '+e);mark('api-done');});", "https://fixture.test/dir/index.html");
    external_case("js_crypto_cases.js", ";runCryptoCases().then(n=>{check('insecure-crypto-count',n>30);mark('api-done');},e=>{console.log('FAIL crypto '+e);mark('api-done');});", BASE);
    external_case("js_clone_cases.js", ";mark('api-done');", BASE);
    external_case("js_encoding_cases.js", ";check('encoding-count',runEncodingCases()>170);mark('api-done');", BASE);
    external_case("js_intl_cases.js", ";runIntlCases().then(n=>{check('intl-count',n===106);mark('api-done');},e=>{console.log('FAIL intl '+e);mark('api-done');});", BASE);
    external_case("js_history_cases.js", ";runHistoryCases().then(()=>mark('api-done'),e=>{console.log('FAIL history '+e);mark('api-done');});", BASE);
    external_case("js_geometry_cases.js", ";check('geometry-count',runGeometryCases()===15);mark('api-done');", BASE);
    external_case("js_dom_compare_cases.js", ";check('node-compare-count',runDOMCompareCases()===14);mark('api-done');", BASE);
    external_case("js_event_handler_cases.js", ";check('event-handler-count',runEventHandlerCases()===18);mark('api-done');", BASE);
    external_case("js_observer_cases.js", ";runObserverCases().then(n=>{check('observer-count',n>=40);mark('api-done');},e=>{console.log('FAIL observer '+e);mark('api-done');});", BASE);
    external_case("js_dom_runtime_cases.js", ";runDOMRuntimeCases().then(n=>{check('dom-runtime-count',n>=40);mark('api-done');},e=>{console.log('FAIL dom-runtime '+e);mark('api-done');});", BASE);
    external_case("js_selection_cases.js", ";runSelectionCases().then(n=>{check('selection-count',n>300);mark('api-done');},e=>{console.log('FAIL selection '+e);mark('api-done');});", BASE);
    external_case("js_performance_cases.js", ";runPerformanceCases().then(n=>{check('performance-count',n>=160);mark('api-done');},e=>{console.log('FAIL performance '+e);mark('api-done');});", BASE);
    external_case("js_performance_cases.js", ";runPerformanceBoundaryCases().then(n=>{check('performance-boundary-count',n>=75);mark('api-done');},e=>{console.log('FAIL performance-boundary '+e);mark('api-done');});", BASE);
    external_case_expected("js_performance_cases.js", ";runPerformanceObserverExceptionCases().then(n=>{check('performance-exception-count',n>=2);mark('api-done');},e=>{console.log('FAIL performance-exception '+e);mark('api-done');});", BASE,1);
    external_case("js_custom_elements_cases.js", ";check('global-events',runGlobalEventTargetCases()===9);check('iframe-types',runHTMLIFrameElementCases()===19);runCustomElementCases().then(n=>{check('custom-elements-count',n>30);mark('api-done');},e=>{console.log('FAIL custom-elements '+e);mark('api-done');});", BASE);
    external_case_expected("js_custom_elements_cases.js", ";check('custom-elements-failures-count',runCustomElementFailureCases()===9);mark('api-done');", BASE, 4);
}

static void test_language(void) {
    const char *source =
        "check('language-basic',JSON.parse(JSON.stringify({n:42})).n===42&&[1,2,3].map(x=>x*x).join(',')==='1,4,9');"
        "check('regexp-unicode',/\\p{Script=Hiragana}/u.test('\\u304c')&&'\\u304b\\u3099'.normalize('NFC')==='\\u304c'&&Array.from('A\\u{1f63a}B').length===3);"
        "const n=1606938044258990275541962092341286059311215339461694069869266n;"
        "const d64=18446744073709551557n,d128=170141183460469231731687303715884118073n;"
        "check('bigint-div64',n/d64===87112285931760246925243521991848423237878n&&n%d64===14083848168753593220n);"
        "check('bigint-div128',n/d128===9444732965739290427392n&&n%d128===123340193783883627360908413650n);"
        "check('bigint-signed',(-n)/d64===-87112285931760246925243521991848423237878n&&(-n)%d64===-14083848168753593220n);"
        "check('date-clock',Number.isFinite(Date.now())&&Date.now()>1700000000000&&performance.now()>=0);"
        "check('math-boundaries',Math.log1p(-1)===-Infinity&&Number.isNaN(Math.log1p(-2))&&Math.expm1(-Infinity)===-1&&Math.expm1(Infinity)===Infinity&&Math.acosh(1)===0&&Number.isNaN(Math.acosh(0))&&Math.atanh(1)===Infinity&&Math.atanh(-1)===-Infinity&&Object.is(Math.asinh(-0),-0));"
        "check('math-small',Math.abs(Math.log1p(1e-12)-(1e-12-5e-25))<1e-25&&Math.abs(Math.expm1(1e-12)-(1e-12+5e-25))<1e-25);"
        "check('math-inverses',Math.abs(Math.asinh(Math.sinh(.5))-.5)<1e-12&&Math.abs(Math.acosh(Math.cosh(1.2))-1.2)<1e-12&&Math.abs(Math.atanh(Math.tanh(.25))-.25)<1e-12);"
        "check('no-os-exposure',typeof std==='undefined'&&typeof os==='undefined'&&typeof Atomics==='undefined'&&typeof SharedArrayBuffer==='undefined');"
        "try{function recurse(){return recurse()+1;}recurse();check('stack-limit',false);}catch(e){check('stack-limit',true);}mark('language-done');";
    char *page = script_page(source);
    test_check("language-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, false)) {
        test_check("language-finished", pump("language-done", 5000));
        static const char *const names[] = {"language-basic","regexp-unicode","bigint-div64","bigint-div128","bigint-signed","date-clock","math-boundaries","math-small","math-inverses","no-os-exposure","stack-limit"};
        require_marks(names, sizeof names / sizeof *names);
        test_check("language-no-exceptions", fixture.errors == 0);
        close_case();
    }
    free(page);
}

static void test_script_mime(void) {
    static const char *const valid[] = {
        "application/ecmascript", "application/javascript", "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0", "text/javascript1.1", "text/javascript1.2",
        "text/javascript1.3", "text/javascript1.4", "text/javascript1.5", "text/jscript", "text/livescript",
        "text/x-ecmascript", "text/x-javascript", " TeXT/JavaScript "
    };
    for (unsigned i=0;i<sizeof valid/sizeof *valid;i++) {
        char page[1024],headers[256];
        snprintf(page,sizeof page,START "<script type='%s'>mark('mime-classic');</script>"
            "<script type=module src=mime.mjs></script></head><body>native</body>",valid[i]);
        snprintf(headers,sizeof headers,"Content-Type: %s; charset=UTF-8\r\n",valid[i]);
        if (open_case(page,false)) {
            fixture.module_mime_headers=headers;
            test_check("mime-module-accepted",pump("mime-module",4000));
            require_marks((const char *const[]){"mime-classic","mime-import","mime-module"},3);
            test_check("mime-import-loader",fixture.sync_loads==1);
            test_check("mime-no-errors",fixture.errors==0);
            close_case();
        }
    }
    static const char *const extra_valid[] = {
        "Content-Type: text/plain, application/x-javascript\r\n",
        "Content-Type: text/javascript; charset=\"UTF-8,x\"\r\n",
        "Content-Type: text/javascript\r\nContent-Type: not a type\r\n",
        "Content-Type: text/javascript, */*\r\n"
    };
    for (unsigned i=0;i<sizeof extra_valid/sizeof *extra_valid;i++) {
        if (open_case(START "<script type=module src=mime.mjs></script></head><body>native</body>",false)) {
            fixture.module_mime_headers=extra_valid[i];
            test_check("mime-list-accepted",pump("mime-module",4000));
            require_marks((const char *const[]){"mime-import","mime-module"},2);
            test_check("mime-list-no-errors",fixture.errors==0);
            close_case();
        }
    }
    static const char *const invalid[] = {
        "", "Content-Type: text/plain\r\n", "Content-Type: text/html\r\n",
        "Content-Type: application/json\r\n", "Content-Type: text/java\r\n",
        "Content-Type: text/javascriptXYZ\r\n", "Content-Type: text/javascript extra\r\n",
        "Content-Type: text/javascript, text/html\r\n",
        "Content-Type: text/javascript; charset=utf-8, text/html\r\n",
        "Content-Type: text/javascript\r\nContent-Type: text/plain\r\n"
    };
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        if (open_case(START "<script type=module src=mime.mjs onerror=\"mark('mime-blocked')\"></script>"
                          "<script type='text/javascript; charset=utf-8'>mark('mime-wrong-type');</script></head><body>native</body>",true)) {
            fixture.module_mime_headers=invalid[i];
            test_check("mime-module-rejected",pump("mime-blocked",4000));
            test_check("mime-wrong-not-executed",!has_mark(&fixture,"mime-module")&&!has_mark(&fixture,"mime-wrong-type"));
            test_check("mime-rejection-reported",fixture.errors==1&&fixture.sync_loads==0);
            close_case();
        }
    }
}

static void test_dom_and_scripts(void) {
    const char *html = START
        "<link rel=stylesheet href=style.css><script defer src=defer.js></script><script async src=async.js></script>"
        "<script type=module src=main.mjs></script></head><body style='margin:0'>"
        "<div id=target></div><div id=sentinel></div><script>"
        "check('parser-boundary',document.getElementById('future')===null);"
        "check('classic-current-script',document.currentScript&&document.currentScript.localName==='script');"
        "document.write('<span id=written>one</span>');document.write('<span id=written2>two</span>');"
        "document.addEventListener('DOMContentLoaded',()=>{check('dom-content-order',deferRan&&moduleRan&&classicRan);"
        "check('document-write-order',document.getElementById('written').nextSibling===document.getElementById('written2'));"
        "try{document.write('late');check('late-write-rejected',false);}catch(e){check('late-write-rejected',true);}mark('dom-ready');});"
        "window.addEventListener('load',()=>{check('window-load-order',document.readyState==='complete');mark('page-loaded');});"
        "</script><div id=future></div><script src=classic.js></script><script>"
        "const target=document.getElementById('target');target.style.height='37px';target.className='one two';"
        "check('dom-node-identity',document.querySelector('#target')===target);"
        "check('dom-prototype-hierarchy',Object.getPrototypeOf(target)===HTMLElement.prototype&&Object.getPrototypeOf(HTMLElement.prototype)===Element.prototype&&Object.getPrototypeOf(Element.prototype)===Node.prototype&&target instanceof HTMLElement&&target instanceof Element&&target instanceof Node);"
        "check('dom-document-interface',Object.getPrototypeOf(document)===Document.prototype&&document instanceof Document&&document instanceof Node&&!(document instanceof Element)&&document.className===undefined&&document.getAttribute===undefined&&document.matches===undefined&&document.href===undefined);"
        "check('dom-element-prototype-method',typeof Element.prototype.matches==='function'&&Element.prototype.matches.call(target,'#target')&&Node.prototype.matches===undefined);"
        "class DerivedElement extends Element{};check('dom-no-fake-instanceof',!(target instanceof DerivedElement));"
        "check('dom-selector',target.matches('.one.two')&&document.querySelector('body > #target')===target);"
        "target.classList.remove('two');check('dom-class-cache',!target.matches('.two'));"
        "target.setAttribute('DATA-VALUE','yes');check('dom-attribute-case',target.getAttribute('data-value')==='yes'&&target.getAttribute('DATA-VALUE')==='yes');"
        "const child=document.createElement('span');child.textContent='native text';target.appendChild(child);child.remove();"
        "check('dom-created-interface',Object.getPrototypeOf(child)===HTMLElement.prototype&&child instanceof Node);"
        "check('dom-detached',!child.isConnected&&child.textContent==='native text');target.appendChild(child);"
        "check('dom-reattach',target.firstChild===child&&child.parentNode===target);"
        "const fragment=document.createDocumentFragment();fragment.append('a',document.createElement('strong'));fragment.lastChild.textContent='b';target.appendChild(fragment);"
        "check('dom-fragment',fragment.childNodes.length===0&&target.lastChild.textContent==='b');"
        "check('dom-fragment-interface',Object.getPrototypeOf(fragment)===DocumentFragment.prototype&&fragment instanceof Node&&!(fragment instanceof Element)&&fragment.className===undefined&&typeof fragment.querySelector==='function');"
        "const textNode=document.createTextNode('text'),commentNode=document.createComment('comment');"
        "check('dom-character-interfaces',Object.getPrototypeOf(textNode)===Text.prototype&&textNode instanceof CharacterData&&textNode instanceof Node&&commentNode instanceof Comment&&commentNode instanceof CharacterData&&textNode.className===undefined&&textNode.getAttribute===undefined);"
        "const foreign=document.createElementNS('http://www.w3.org/2000/svg','svg');check('dom-foreign-interface',foreign instanceof Element&&!(foreign instanceof HTMLElement)&&Object.getPrototypeOf(foreign)===Element.prototype);"
        "const clone=child.cloneNode(true);check('dom-clone',clone!==child&&clone.textContent===child.textContent);"
        "check('dom-clone-interface',Object.getPrototypeOf(clone)===HTMLElement.prototype&&clone.firstChild instanceof Text);"
        "const inert=document.createElement('div');globalThis.inertRan=false;inert.innerHTML='<span class=inner>parsed</span><script>inertRan=true;<\\/script>';target.appendChild(inert);"
        "check('inner-html-inert',!inertRan&&inert.querySelector('.inner').textContent==='parsed');"
        "const live=document.createElement('script');live.text='globalThis.inlineDynamicRan=true;';document.body.appendChild(live);"
        "check('dynamic-inline',globalThis.inlineDynamicRan===true);"
        "const empty=document.createElement('script');document.body.appendChild(empty);empty.text='globalThis.lateDynamicRan=true;';"
        "check('dynamic-empty-then-text',globalThis.lateDynamicRan===true);"
        "const cloneScript=live.cloneNode(true);globalThis.inlineDynamicRan=false;document.body.appendChild(cloneScript);"
        "check('cloned-script-inert',globalThis.inlineDynamicRan===false);"
        "check('dom-geometry',target.getBoundingClientRect().height===37&&getComputedStyle(target).height==='37px');"
        "document.title='QuickJS native DOM';"
        "globalThis.dynamicOrder=[];const one=document.createElement('script'),two=document.createElement('script');one.async=two.async=false;one.src='order-one.js';two.src='order-two.js';"
        "two.onload=()=>check('dynamic-external-order',dynamicOrder.join(',')==='1,2');document.body.append(one,two);"
        "import('./dynamic.mjs').then(()=>import('./dynamic.mjs')).then(()=>check('module-cache',dynamicEvaluations===1));"
        "mark('dom-script-done');</script></body></html>";
    if (!open_case(html, false)) return;
    test_check("dom-script-finished", pump("page-loaded", 8000));
    test_check("dom-jobs-drained", pump(NULL, 4000));
    static const char *const names[] = {"parser-boundary","classic-current-script","external-classic","blocking-css","defer-after-parser","async-loaded","module-cycle","module-current-script","module-import-meta","dynamic-import","module-top-level-await","dom-content-order","document-write-order","late-write-rejected","window-load-order","dom-node-identity","dom-selector","dom-class-cache","dom-attribute-case","dom-detached","dom-reattach","dom-fragment","dom-clone","inner-html-inert","dynamic-inline","dynamic-empty-then-text","cloned-script-inert","dom-geometry","dynamic-external-order","module-cache"};
    require_marks(names, sizeof names / sizeof *names);
    static const char *const interfaces[] = {"dom-prototype-hierarchy","dom-document-interface","dom-element-prototype-method","dom-no-fake-instanceof","dom-created-interface","dom-fragment-interface","dom-character-interfaces","dom-foreign-interface","dom-clone-interface"};
    require_marks(interfaces, sizeof interfaces / sizeof *interfaces);
    test_check("native-title", !strcmp(web_title(fixture.doc), "QuickJS native DOM"));
    int y = web_anchor_y(fixture.doc, "target"), sentinel = web_anchor_y(fixture.doc, "sentinel");
    test_check("native-layout-after-js", y >= 0 && sentinel - y == 37);
    test_check("host-completions-copy-only", fixture.completions >= 7 && fixture.outside_running == 0);
    test_check("sync-module-loader-used", fixture.sync_loads >= 3);
    test_check("dom-no-exceptions", fixture.errors == 0);
    close_case();
}

static void test_events_and_forms(void) {
    const char *html = START "</head><body style='margin:0'>"
        "<div id=target style='height:20px'></div><div id=sentinel></div>"
        "<form id=theform action=accepted method=post><input id=value name=value value=initial>"
        "<input id=checkbox type=checkbox><input id=radio1 type=radio name=group checked>"
        "<input id=radio2 type=radio name=group disabled><select id=selection name=choice>"
        "<optgroup label=Group><option value=one>One</option><option value=two>Two</option></optgroup></select>"
        "<button id=submitter name=button value=send>Submit</button></form>"
        "<input id=outside form=theform name=outside value=owned>"
        "<button id=outsidebutton form=theform>Outside submit</button>"
        "<a id=link href=blocked onclick='globalThis.inlineEvent=true;return false;'>link</a><script>"
        "const target=document.getElementById('target'),form=document.getElementById('theform');"
        "let order=[];document.addEventListener('probe',()=>order.push('capture'),true);"
        "target.addEventListener('probe',e=>{order.push('target');e.preventDefault();});"
        "document.addEventListener('probe',()=>order.push('bubble'));"
        "check('event-capture-bubble-cancel',target.dispatchEvent(new Event('probe',{bubbles:true,cancelable:true}))===false&&order.join(',')==='capture,target,bubble');"
        "let once=0,removed=0;target.addEventListener('once',()=>once++,{once:true});"
        "const removedHandler=()=>removed++;target.addEventListener('once',removedHandler);target.removeEventListener('once',removedHandler);"
        "target.dispatchEvent(new Event('once'));target.dispatchEvent(new Event('once'));check('event-once-remove',once===1&&removed===0);"
        "let readded=0;const readd=()=>readded++;target.addEventListener('readd',readd);target.removeEventListener('readd',readd);target.addEventListener('readd',readd);target.dispatchEvent(new Event('readd'));check('event-remove-readd',readded===1);"
        "let onceAgain=0;const onceHandler=()=>{onceAgain++;target.addEventListener('once-again',onceHandler,{once:true});};target.addEventListener('once-again',onceHandler,{once:true});target.dispatchEvent(new Event('once-again'));target.dispatchEvent(new Event('once-again'));check('event-once-readd',onceAgain===2);target.removeEventListener('once-again',onceHandler);"
        "const oldSignal=new AbortController();let signalCount=0;const signalHandler=()=>signalCount++;target.addEventListener('signal-old',signalHandler,{signal:oldSignal.signal});target.removeEventListener('signal-old',signalHandler);target.addEventListener('signal-old',signalHandler);oldSignal.abort();target.dispatchEvent(new Event('signal-old'));check('event-old-signal-isolated',signalCount===1);"
        "const onceSignal=new AbortController();let onceSignalCount=0;const onceSignalHandler=()=>onceSignalCount++;target.addEventListener('signal-once',onceSignalHandler,{once:true,signal:onceSignal.signal});target.dispatchEvent(new Event('signal-once'));target.addEventListener('signal-once',onceSignalHandler);onceSignal.abort();target.dispatchEvent(new Event('signal-once'));check('event-once-signal-isolated',onceSignalCount===2);"
        "const activeSignal=new AbortController();let abortedCount=0;const abortedHandler=()=>abortedCount++;target.addEventListener('signal-active',abortedHandler,{signal:activeSignal.signal});activeSignal.abort();target.dispatchEvent(new Event('signal-active'));target.addEventListener('signal-active',abortedHandler,{signal:activeSignal.signal});target.dispatchEvent(new Event('signal-active'));check('event-active-signal-removes',abortedCount===0);"
        "let snapshot=[];const snapshotSecond=()=>snapshot.push(2);target.addEventListener('snapshot',()=>{snapshot.push(1);target.removeEventListener('snapshot',snapshotSecond);target.addEventListener('snapshot',snapshotSecond);},{once:true});target.addEventListener('snapshot',snapshotSecond);target.dispatchEvent(new Event('snapshot'));const firstSnapshot=snapshot.join(',');target.dispatchEvent(new Event('snapshot'));check('event-mutation-snapshot',firstSnapshot==='1'&&snapshot.join(',')==='1,2');"
        "let stopped=[];target.addEventListener('stop',e=>{stopped.push(1);e.stopImmediatePropagation();});"
        "target.addEventListener('stop',()=>stopped.push(2));document.addEventListener('stop',()=>stopped.push(3));"
        "target.dispatchEvent(new Event('stop',{bubbles:true}));check('event-stop-immediate',stopped.join(',')==='1');"
        "target.addEventListener('passive',e=>e.preventDefault(),{passive:true});"
        "check('event-passive',target.dispatchEvent(new Event('passive',{cancelable:true})));"
        "let custom;target.addEventListener('custom',e=>custom=e.detail);target.dispatchEvent(new CustomEvent('custom',{detail:42}));check('event-custom',custom===42);"
        "target.addEventListener('native-probe',e=>{check('native-event-fields',e.isTrusted&&e.target===target&&e.clientX===3&&e.clientY===4&&e.pageY===15&&e.button===1);"
        "e.preventDefault();Promise.resolve().then(()=>{target.style.height='41px';mark('native-event-microtask');});});"
        "target.addEventListener('keydown',e=>{check('native-key-fields',e.key==='Enter'&&e.keyCode===13&&e.ctrlKey&&e.shiftKey&&!e.altKey);mark('native-key-done');});"
        "document.addEventListener('scroll',e=>check('native-scroll-document',e.target===document&&e.isTrusted&&!e.cancelable));"
        "window.addEventListener('scroll',e=>{check('native-scroll-window',e.target===document&&e.bubbles);queueMicrotask(()=>mark('native-scroll-microtask'));});"
        "let submits=0;form.addEventListener('submit',e=>{submits++;e.preventDefault();});"
        "document.getElementById('submitter').click();form.requestSubmit();document.getElementById('outsidebutton').click();"
        "check('form-submit-canceled',submits===3);"
        "check('form-external-owner',document.getElementById('outside').form===form&&document.getElementById('outsidebutton').form===form);"
        "document.getElementById('link').click();check('inline-handler-canceled',globalThis.inlineEvent===true);"
        "const input=document.getElementById('value');input.setAttribute('value','attribute');check('control-pristine-value',input.value==='attribute');"
        "input.value='current';input.setAttribute('value','default');check('control-dirty-value',input.value==='current');"
        "input.focus();check('control-focus',document.activeElement===input);input.blur();check('control-blur',document.activeElement===document.body);"
        "const checkbox=document.getElementById('checkbox');let changes=[];checkbox.addEventListener('input',()=>changes.push('input'));checkbox.addEventListener('change',()=>changes.push('change'));checkbox.click();"
        "check('control-checkbox-click',checkbox.checked&&changes.join(',')==='input,change');checkbox.removeAttribute('checked');check('control-dirty-checked',checkbox.checked);"
        "const r1=document.getElementById('radio1'),r2=document.getElementById('radio2');r2.checked=true;check('control-radio-exclusion',r2.checked&&!r1.checked);"
        "const select=document.getElementById('selection'),two=select.querySelectorAll('option')[1];"
        "select.value='two';check('control-select-optgroup',select.value==='two'&&select.selectedIndex===1&&two.selected);"
        "select.selectedIndex=-1;check('control-select-none',select.value===''&&!two.selected);two.selected=true;check('control-option-selected',select.selectedIndex===1&&select.value==='two');"
        "mark('events-ready');</script></body></html>";
    if (!open_case(html, false)) return;
    test_check("events-ready", pump("events-ready", 5000));
    fixture.scroll_y = 11;
    web_node *target = web_node_at(fixture.doc, 3, 4);
    test_check("native-event-target", target != NULL);
    if (target) {
        struct web_event e = {.type="native-probe",.x=3,.y=4,.button=1,.bubbles=true,.cancelable=true};
        test_check("native-event-default-canceled", !web_dispatch(fixture.doc, target, &e));
        test_check("native-dispatch-checkpoint", has_mark(&fixture, "native-event-microtask"));
        e.type="keydown";e.key="Enter";e.key_code=13;e.ctrl=true;e.shift=true;e.alt=false;
        test_check("native-key-default-allowed", web_dispatch(fixture.doc, target, &e));
        step(&fixture);
    }
    web_document_scroll(fixture.doc);
    test_check("native-scroll-document", has_mark(&fixture, "native-scroll-document"));
    test_check("native-scroll-window", has_mark(&fixture, "native-scroll-window"));
    test_check("native-scroll-checkpoint", has_mark(&fixture, "native-scroll-microtask"));
    static const char *const names[] = {"event-capture-bubble-cancel","event-once-remove","event-stop-immediate","event-passive","event-custom","native-event-fields","native-event-microtask","native-key-fields","native-key-done","form-submit-canceled","form-external-owner","inline-handler-canceled","control-pristine-value","control-dirty-value","control-focus","control-blur","control-checkbox-click","control-dirty-checked","control-radio-exclusion","control-select-optgroup","control-select-none","control-option-selected"};
    require_marks(names, sizeof names / sizeof *names);
    static const char *const lifecycle[] = {"event-remove-readd","event-once-readd","event-old-signal-isolated","event-once-signal-isolated","event-active-signal-removes","event-mutation-snapshot"};
    require_marks(lifecycle, sizeof lifecycle / sizeof *lifecycle);
    test_check("canceled-native-navigation", fixture.navigations == 0);
    test_check("native-event-layout", web_anchor_y(fixture.doc, "sentinel") - web_anchor_y(fixture.doc, "target") == 41);
    test_check("events-no-exceptions", fixture.errors == 0);
    close_case();
}

static void test_timers_and_promises(void) {
    const char *source =
        "let sequence=['script'],intervalCount=0,canceledTimeout=false,canceledFrame=false,frameDone=false;"
        "Promise.resolve().then(()=>sequence.push('promise'));queueMicrotask(()=>sequence.push('microtask'));"
        "setTimeout(()=>{sequence.push('timer');check('task-microtask-order',sequence.join(',')==='script,promise,microtask,timer');},0);"
        "const interval=setInterval(()=>{if(++intervalCount===3){clearInterval(interval);mark('interval-three');}},2);"
        "const canceled=setTimeout(()=>canceledTimeout=true,0);clearTimeout(canceled);"
        "const frame=requestAnimationFrame(t=>{check('raf-monotonic',Number.isFinite(t)&&t>=0&&t<=performance.now());frameDone=true;mark('raf-fired');});"
        "const other=requestAnimationFrame(()=>canceledFrame=true);cancelAnimationFrame(other);"
        "const args=Array.from({length:96},(_,i)=>i);setTimeout(function(...received){check('timer-many-arguments',received.length===96&&received[95]===95&&this===window);},0,...args);"
        "Promise.reject(new Error('handled')).catch(e=>check('handled-promise-rejection',e.message==='handled'));"
        "function finish(){if(intervalCount<3||!frameDone){setTimeout(finish,4);return;}check('timers-canceled',!canceledTimeout&&!canceledFrame&&intervalCount===3);mark('timers-done');}setTimeout(finish,80);";
    char *page = script_page(source);
    test_check("timers-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, false)) {
        test_check("timers-finished", pump("timers-done", 5000));
        test_check("timers-idle", pump(NULL, 3000));
        static const char *const names[] = {"task-microtask-order","interval-three","raf-monotonic","raf-fired","timer-many-arguments","handled-promise-rejection","timers-canceled"};
        require_marks(names, sizeof names / sizeof *names);
        test_check("timers-no-exceptions", fixture.errors == 0);
        close_case();
    }
    free(page);
}

static void test_fetch_and_cancel(void) {
    const char *source =
        "async function exercise(){"
        "const r=await fetch('api/json');check('fetch-response',r.status===200&&r.ok&&r.url==='http://fixture.test/dir/api/json'&&r.headers.get('x-fixture')==='yes');"
        "const copy=r.clone(),bytes=await copy.arrayBuffer();check('fetch-arraybuffer',bytes instanceof ArrayBuffer&&bytes.byteLength===22&&new Uint8Array(bytes)[0]===123);"
        "const json=await r.json();check('fetch-json',json.ok&&json.value===42&&r.bodyUsed);"
        "try{await r.text();check('fetch-body-once',false);}catch(e){check('fetch-body-once',e instanceof TypeError);}"
        "const post=await fetch('api/post',{method:'POST',headers:{'X-Fixture':'yes'},body:'Nocturne request body'});check('fetch-post',await post.text()==='Nocturne request body');"
        "const controller=new AbortController(),promise=fetch('api/slow',{signal:controller.signal});controller.abort();"
        "try{await promise;check('fetch-aborted',false);}catch(e){check('fetch-aborted',e.name==='AbortError');}"
        "try{await fetch('file:///data/secret');check('fetch-local-rejected',false);}catch(e){check('fetch-local-rejected',e instanceof TypeError);}"
        "try{await fetch('api/json',{method:'PUT'});check('fetch-method-rejected',false);}catch(e){check('fetch-method-rejected',true);}"
        "try{await fetch('api/json',{body:'not allowed'});check('fetch-get-body-rejected',false);}catch(e){check('fetch-get-body-rejected',true);}"
        "document.getElementById('target').textContent='received '+json.value;document.title='Fetched 42';mark('fetch-done');}exercise();";
    char *page = script_page(source);
    test_check("fetch-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, false)) {
        test_check("fetch-finished", pump("fetch-done", 5000));
        test_check("fetch-idle", pump(NULL, 3000));
        static const char *const names[] = {"fetch-response","fetch-arraybuffer","fetch-json","fetch-body-once","fetch-post","fetch-aborted","fetch-local-rejected","fetch-method-rejected","fetch-get-body-rejected"};
        require_marks(names, sizeof names / sizeof *names);
        test_check("fetch-canceled-request", fixture.cancellations == 1 && fixture.requests == 3 && queued_count(&fixture) == 0);
        test_check("fetch-native-title", !strcmp(web_title(fixture.doc), "Fetched 42"));
        test_check("fetch-copy-checkpoint", fixture.outside_running == 0);
        test_check("fetch-no-exceptions", fixture.errors == 0);
        close_case();
    }
    free(page);
}

static void test_execution_budget(const char *name, const char *source) {
    const char *tail = "</script><div id=afterbudget style='height:18px'>Native UI survives</div><script>mark('must-not-run');";
    size_t n = strlen(source) + strlen(tail) + 1;
    char *combined = malloc(n);
    test_check("budget-source-allocation", combined != NULL);
    if (!combined) return;
    snprintf(combined, n, "%s%s", source, tail);
    char *page = script_page(combined); free(combined);
    test_check("budget-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, true)) {
        uint64_t start = uptime_ms();
        test_check(name, pump(NULL, 10000));
        uint64_t elapsed = uptime_ms() - start;
        test_check("budget-stopped-within-bound", elapsed >= 4900 && elapsed < 9000);
        test_check("budget-reported", fixture.errors > 0 && strstr(fixture.last_error, "5 second") != NULL);
        test_check("budget-not-running", !web_script_running(fixture.doc));
        test_check("budget-further-js-disabled", !has_mark(&fixture, "must-not-run"));
        test_check("budget-parser-native-survives", web_anchor_y(fixture.doc, "afterbudget") >= 0);
        struct web_event e = {.type="click",.cancelable=true};
        test_check("budget-native-ui-survives", web_dispatch(fixture.doc, NULL, &e));
        close_case();
    }
    free(page);
}

static void test_allocation_limit(const char *kind, const char *source, const char *marker) {
    char *page = script_page(source);
    test_check("limit-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, false)) {
        test_check(kind, pump(marker, 12000));
        test_check("allocation-limit-caught", has_mark(&fixture, marker));
        test_check("allocation-limit-not-running", !web_script_running(fixture.doc));
        test_check("allocation-limit-no-uncaught", fixture.errors == 0);
        test_check("allocation-limit-native-survives", web_anchor_y(fixture.doc, "sentinel") >= 0);
        close_case();
    }
    free(page);
}

static void test_limits(void) {
    test_execution_budget("loop-budget", "mark('loop-start');for(;;){}");
    test_execution_budget("microtask-budget", "mark('microtask-start');function forever(){Promise.resolve().then(forever);}forever();");
    /* Split allocations into browser tasks: a time-limit failure cannot stand in
       for enforcing either allocation quota. No private allocator APIs are used. */
    test_allocation_limit("heap-128m-limit",
        "let buffers=[],allocated=0;function consume(){try{for(let i=0;i<4;i++){buffers.push(new ArrayBuffer(1048576));allocated++;}setTimeout(consume,0);}"
        "catch(e){buffers.length=0;check('heap-bound',allocated>96&&allocated<128);mark('heap-limit');}}consume();", "heap-limit");
    require_marks((const char *const[]){"heap-bound"}, 1);
    test_allocation_limit("dom-32m-limit",
        "const payload='x'.repeat(262144);let nodes=0;function consume(){try{for(let i=0;i<4;i++){document.createTextNode(payload);nodes++;}setTimeout(consume,0);}"
        "catch(e){check('dom-bound',nodes>64&&nodes<128);mark('dom-limit');}}consume();", "dom-limit");
    require_marks((const char *const[]){"dom-bound"}, 1);
    test_allocation_limit("timer-count-limit",
        "let handles=[];try{for(let i=0;i<130;i++)handles.push(setTimeout(()=>{},60000));check('timer-bound',false);}"
        "catch(e){check('timer-bound',handles.length===128&&e instanceof RangeError);}for(const h of handles)clearTimeout(h);mark('timer-limit');", "timer-limit");
    require_marks((const char *const[]){"timer-bound"}, 1);

    char *page = script_page("document.getElementById('target').addEventListener('runaway',e=>{e.preventDefault();for(;;){}});mark('runaway-ready');");
    test_check("runaway-event-page-allocation", page != NULL);
    if (page && open_case(page, true)) {
        test_check("runaway-event-ready", pump("runaway-ready", 4000));
        web_node *target = web_node_at(fixture.doc, 3, 4);
        struct web_event e = {.type="runaway",.cancelable=true};
        test_check("runaway-event-target", target != NULL);
        if (target) {
            test_check("runaway-event-default-remains-canceled", !web_dispatch(fixture.doc,target,&e));
            test_check("disabled-runtime-next-native-event-allowed", web_dispatch(fixture.doc,target,&e));
        }
        test_check("runaway-event-budget-reported", fixture.errors > 0 && strstr(fixture.last_error,"5 second") != NULL);
        test_check("runaway-event-not-running", !web_script_running(fixture.doc));
        close_case();
    }
    free(page);
}

static void test_native_selection(void) {
    const char *source =
        "const input=document.createElement('input');input.style.cssText='position:absolute;left:0;top:0;width:200px;height:30px';"
        "document.body.appendChild(input);input.value='abcdef';input.setSelectionRange(1,4);input.focus();let stage=0;"
        "input.addEventListener('selection-probe',()=>{"
        "if(stage===0){check('native-range-replacement',input.value==='aZef'&&input.selectionStart===2&&input.selectionEnd===2);input.value='a\\ud83d\\ude00b';input.setSelectionRange(3,3);}"
        "if(stage===1)check('native-left-scalar',input.selectionStart===1&&input.selectionEnd===1);"
        "if(stage===2)check('native-shift-selection',input.selectionStart===1&&input.selectionEnd===3&&input.selectionDirection==='forward');"
        "if(stage===3){check('native-delete-selection',input.value==='ab'&&input.selectionStart===1);input.setSelectionRange(1,1);}"
        "if(stage===4)check('native-backspace',input.value==='b'&&input.selectionStart===0);"
        "if(stage===5)check('native-home-shift',input.selectionStart===0&&input.selectionEnd===0);"
        "if(stage===6){check('native-end-shift',input.selectionStart===0&&input.selectionEnd===1);input.value='abcd';input.setSelectionRange(1,3);input.setAttribute('maxlength','4');}"
        "if(stage===7){check('native-maxlength-replacement',input.value==='aXd'&&input.selectionStart===2);input.value='abcd';input.setSelectionRange(2,2);}"
        "if(stage===8)check('native-maxlength-reject',input.value==='abcd'&&input.selectionStart===2);"
        "stage++;mark('native-selection-step-'+stage);});mark('native-selection-ready');";
    char *page=script_page(source);
    test_check("native-selection-page",page!=NULL);
    if(page && open_case(page,false)) {
        test_check("native-selection-ready",pump("native-selection-ready",5000));
        web_node *input=web_focused(fixture.doc);
        test_check("native-selection-focus",input!=NULL);
        step(&fixture);
        int selected_pixels=0;for(int i=0;i<VW*VH;i++)if((pixels[i]&0xffffff)==0x0075ff)selected_pixels++;
        test_check("native-selection-painted",selected_pixels>20);
        static const uint32_t keys[]={'Z',NKEY_LEFT,NKEY_RIGHT,NKEY_DELETE,NKEY_BACKSPACE,NKEY_HOME,NKEY_END,'X','Y'};
        for(unsigned i=0;input && i<sizeof keys/sizeof *keys;i++) {
            struct gui_event key={.key=keys[i],.mods=(i==2||i==5||i==6)?NMOD_SHIFT:0};
            int result=web_key(fixture.doc,&key);
            test_check("native-selection-key-handled",result==(i==8?0:1));
            struct web_event probe={.type="selection-probe"};web_dispatch(fixture.doc,input,&probe);
            char mark[48];snprintf(mark,sizeof mark,"native-selection-step-%u",i+1);
            test_check("native-selection-step",has_mark(&fixture,mark));
        }
        static const char *const names[]={"native-range-replacement","native-left-scalar","native-shift-selection","native-delete-selection","native-backspace","native-home-shift","native-end-shift","native-maxlength-replacement","native-maxlength-reject"};
        require_marks(names,sizeof names/sizeof *names);
        test_check("native-selection-no-errors",fixture.errors==0);
        close_case();
    }
    free(page);
}

static void test_lifetime(void) {
    const char *oldsource =
        "globalThis.oldDocumentValue=42;const saved=document.createElement('span');saved.textContent='retained';document.body.appendChild(saved);saved.remove();"
        "saved.addEventListener('custom',()=>mark('old-event'));globalThis.retained=saved;"
        "setTimeout(()=>mark('old-timer-must-not-fire'),200);fetch('api/slow').then(()=>mark('old-fetch-must-not-settle'));"
        "location.href='replacement.html';document.getElementById('target').style.height='33px';"
        "Promise.resolve().then(()=>{check('navigation-task-finished',document.getElementById('target').getBoundingClientRect().height===33);mark('lifetime-pending');});";
    char *old = script_page(oldsource);
    char *fresh = script_page("check('fresh-global',typeof oldDocumentValue==='undefined'&&typeof retained==='undefined');mark('fresh-done');");
    test_check("lifetime-pages-allocation", old && fresh);
    if (!old || !fresh) { free(old); free(fresh); return; }
    int cycles = 0;
    for (; cycles < 24; cycles++) {
        if (!open_case(old, false)) break;
        test_check("lifetime-task-checkpoint", pump("lifetime-pending", 4000));
        test_check("navigation-deferred-notification", fixture.navigations == 1 && !strcmp(fixture.navigation,"http://fixture.test/dir/replacement.html") && fixture.outside_running == 0);
        test_check("old-resource-pending", queued_count(&fixture) == 1);
        test_check("navigation-task-finished", has_mark(&fixture, "navigation-task-finished"));
        close_case();
        test_check("document-free-cancels-request", fixture.cancellations == 1 && queued_count(&fixture) == 0);
        if (!open_case(fresh, false)) break;
        test_check("fresh-document-runs", pump("fresh-done", 4000));
        test_check("fresh-document-no-state-leak", has_mark(&fixture,"fresh-global") && fixture.errors == 0);
        test_check("old-timer-never-runs", !has_mark(&fixture,"old-timer-must-not-fire") && !has_mark(&fixture,"old-fetch-must-not-settle"));
        close_case();
    }
    test_check("repeated-document-destruction", cycles == 24);
    free(old); free(fresh);
}

int main(int argc, char **argv) {
    uint64_t start = uptime_ms();
    /* Optional selectors make a failing feature reproducible in the real OS. */
#define RUN(name, fn) do { if (argc == 1 || !strcmp(argv[1], name)) { printf("jstest: %s\n", name); fflush(stdout); fn(); } } while (0)
    RUN("language", test_language);
    RUN("platform", test_platform);
    RUN("dom", test_dom_and_scripts);
    RUN("mime", test_script_mime);
    RUN("events", test_events_and_forms);
    RUN("selection-native", test_native_selection);
    RUN("timers", test_timers_and_promises);
    RUN("fetch", test_fetch_and_cancel);
    RUN("limits", test_limits);
    RUN("lifetime", test_lifetime);
#undef RUN
    test_check("at-least-one-case", total != 0);
    printf("jstest: %d checks, %d failed (%lu ms)\n", total, failed, (unsigned long)(uptime_ms()-start));
    return failed != 0;
}
