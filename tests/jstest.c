/* jstest: the real web_live/QuickJS path, compiled by TinyCC inside Nocturne.
   The embedder serves deterministic offline resources through the public host
   contract. HTTP workers and CORS are covered separately by webnettest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"
#include "webnet.h"

#define BASE "http://fixture.test/dir/index.html"
#define VW 800
#define VH 600
#define SLOTS 32
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
    {"/dir/main.mjs", "import {cycle} from './a.mjs';globalThis.moduleStarted=true;check('module-cycle',cycle()==='AB');"
        "check('module-current-script',document.currentScript===null);"
        "check('module-import-meta',import.meta.url==='http://fixture.test/dir/main.mjs');"
        "const value=await import('./dynamic.mjs');check('dynamic-import',value.answer===42);"
        "await globalThis.moduleGate;await new Promise(resolve=>setTimeout(resolve,4));globalThis.moduleRan=true;check('module-top-level-await',globalThis.domContentObserved===true);", 0},
    {"/dir/a.mjs", "import {b} from './b.mjs';export function a(){return 'A';}export function cycle(){return a()+b();}", 0},
    {"/dir/b.mjs", "import {a} from './a.mjs';export function b(){return a()==='A'?'B':'?';}", 0},
    {"/dir/dynamic.mjs", "globalThis.dynamicEvaluations=(globalThis.dynamicEvaluations||0)+1;export const answer=42;", 0},
    {"/dir/preload.mjs", "globalThis.preloadEvaluations=(globalThis.preloadEvaluations||0)+1;export const answer=42;", 10},
    {"/dir/preload.mjs?sri", "globalThis.preloadEvaluations=(globalThis.preloadEvaluations||0)+1;export const answer=42;", 10},
    {"/dir/preload-root.mjs", "import {value} from './preload-child.mjs';check('preload-joined-value',value===42);mark('preload-graph-done');", 0},
    {"/dir/preload-child.mjs", "export const value=42;", 1000},
    {"/dir/mime.mjs", "import {value} from './mimedep.mjs';check('mime-import',value===42);mark('mime-module');", 0},
    {"/dir/mimedep.mjs", "export const value=42;", 0},
    {"/dir/broken-parent.mjs", "import {value} from './missing-dependency.mjs';export {value};", 0},
    {"/dir/order-one.js", "globalThis.dynamicOrder.push(1);", 15},
    {"/dir/order-two.js", "globalThis.dynamicOrder.push(2);", 0},
    {"/dir/api/json", "{\"ok\":true,\"value\":42}", 0},
    {"/dir/api/long-headers", "complete headers", 0},
    {"/dir/long-headers.mjs", "export const completeHeaders = 42;", 0},
    {"/dir/api/post", "Nocturne request body", 0},
    {"/dir/api/slow", "{\"late\":true}", 60000},
    {"/dir/queued-wanted.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>", 1000},
};

struct queued {
    bool used, no_cors, no_referrer, body_present;
    uint64_t id, due, transport_id;
    int kind;
    char url[2048], method[16], headers[1024], body[4096],referrer[2048];
    size_t body_len;
    int cache_mode,referrer_policy;
};
struct fixture {
    web_doc *doc;
    webnet *net;
    struct queued queue[SLOTS];
    char **marks;
    size_t nmarks, mark_capacity;
    bool mark_allocation_failed;
    int js_failures, errors, requests, cancellations, completions, sync_loads;
    bool native_wait;
    int preload_requests, preload_joins;
    int fetch_requests;
    int policy_report_requests, policy_report_completed, policy_report_bad;
    int outside_running, navigations;
    int scroll_x, scroll_y;
    bool expected_errors, retirement_errors;
    int retirement_interrupts, retirement_unexpected;
    const char *module_mime_headers;
    /* A supplemental suite serving its own resources replaces the response
       deliver() built, on the same asynchronous path as every other request. */
    void (*respond)(const struct queued *q, struct web_response *r);
    struct { char *url; void *data; size_t len; uint64_t entry; bool manual; } history[16];
    int history_pos, history_length, history_delta;
    uint64_t history_serial;
    bool history_pending;
    char navigation[2048], post[256], last_error[512], error_trace[8192];
    int diagnostic_batches;
};
static struct fixture fixture;
static const char *case_response_headers;
static const char *case_meta_policy;
static bool case_real_network;
static bool case_input_checkpoint;
static unsigned input_ready_checkpoints, input_blocked_checkpoints;
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
    for (size_t i = 0; i < f->nmarks; i++) if (!strcmp(f->marks[i], name)) return true;
    return false;
}
static void free_marks(struct fixture *f) {
    for(size_t i=0;i<f->nmarks;i++)free(f->marks[i]);
    free(f->marks);f->marks=NULL;f->nmarks=f->mark_capacity=0;
    f->mark_allocation_failed=false;
}
static void mark_allocation_failure(struct fixture *f,const char *reason) {
    if(f->mark_allocation_failed)return;
    f->mark_allocation_failed=true;f->js_failures++;
    test_check(reason,false);
}
static void save_mark(struct fixture *f,const char *name) {
    if(f->mark_allocation_failed)return;
    if(f->nmarks==f->mark_capacity){
        if(f->mark_capacity>SIZE_MAX/2){mark_allocation_failure(f,"fixture-marker-capacity-overflow");return;}
        size_t capacity=f->mark_capacity?f->mark_capacity*2:32;
        if(capacity>SIZE_MAX/sizeof *f->marks){mark_allocation_failure(f,"fixture-marker-size-overflow");return;}
        char **marks=realloc(f->marks,capacity*sizeof *marks);
        if(!marks){mark_allocation_failure(f,"fixture-marker-allocation");return;}
        f->marks=marks;f->mark_capacity=capacity;
    }
    char *copy=strdup(name);
    if(!copy){mark_allocation_failure(f,"fixture-marker-name-allocation");return;}
    f->marks[f->nmarks++]=copy;
}
static int queued_count(const struct fixture *f) {
    int count = 0;
    for (int i = 0; i < SLOTS; i++) count += f->queue[i].used;
    return count;
}
static void receive_log(void *opaque, int level, const char *message) {
    struct fixture *f = opaque;
    if (!strncmp(message, "OK ", 3)) {
        save_mark(f,message+3);
    } else if (!strncmp(message, "FAIL ", 5)) {
        printf("%s\n", message); fflush(stdout); f->js_failures++; failed++; total++;
    } else if (!strncmp(message, "NATIVE-SELECTION ", 17) || !strncmp(message, "XHR checks ", 11) || !strncmp(message, "Document checks ", 16) || !strncmp(message, "XMLSerializer checks ", 21) || !strncmp(message, "Document limit mode ", 20) || !strncmp(message, "Lexbor ", 7) || !strncmp(message,"WebAssembly ",sizeof("WebAssembly ")-1) || strstr(message, " API checks ")) {
        printf("%s\n", message);
    } else if (level == 2) {
        /* One exception can have Diagnostic/Source continuation messages.
           Keep them in the trace, not in the semantic exception count. */
        bool detail=!strncmp(message,"Diagnostic #",12)||!strncmp(message,"Source ",7);
        if(!detail){f->errors++;snprintf(f->last_error, sizeof f->last_error, "%s", message);}
        size_t used = strlen(f->error_trace);
        snprintf(f->error_trace + used, sizeof f->error_trace - used, "%s\n", message);
        if (!strncmp(message, "Diagnostic #", 12)) f->diagnostic_batches++;
        if (f->retirement_errors) {
            if (!strncmp(message,"InternalError: interrupted",26)) f->retirement_interrupts++;
            else if (strncmp(message,"Diagnostic #",12) && strncmp(message,"Source ",7)) f->retirement_unexpected++;
        }
        if (!f->expected_errors) printf("jstest: unexpected JavaScript error: %s\n", message);
    }
}
static void network_complete(webnet *net,uint64_t id,uint64_t generation,
                             const struct webnet_response *response,void *opaque){
    (void)net;(void)generation;struct queued *q=opaque;
    if(!q->used||q->transport_id!=id||!fixture.doc)return;
    if(q->kind==WEB_RESOURCE_REPORT){fixture.policy_report_completed++;if(response->status!=204||*response->error)fixture.policy_report_bad++;}
    struct web_response r={0};r.status=response->status;
    web_response_set_url(&r,response->final_url);
    r.headers_full=strdup(response->headers?response->headers:"");
    snprintf(r.error,sizeof r.error,"%s",response->error?response->error:"");
    r.body=malloc(response->body_len+1);r.body_len=response->body_len;
    if(r.body){if(r.body_len)memcpy(r.body,response->body,r.body_len);r.body[r.body_len]=0;}
    if(!r.body||!r.headers_full)snprintf(r.error,sizeof r.error,"fixture response allocation failed");
    r.event=response->event;r.uploaded=response->uploaded;
    if(response->event==WEBNET_COMPLETE||response->event==WEBNET_END)q->used=false;
    web_resource_loaded(fixture.doc,q->id,&r);
    web_response_free(&r);fixture.completions++;
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
        q->body_present = r->body != NULL;
        q->cache_mode = r->cache_mode;
        q->no_cors = r->no_cors; q->no_referrer = r->no_referrer;
        q->referrer_policy=r->referrer_policy;
        snprintf(q->referrer,sizeof q->referrer,"%s",r->referrer?r->referrer:"");
        if (r->kind == WEB_RESOURCE_FETCH) f->fetch_requests++;
        if(r->kind==WEB_RESOURCE_MODULE&&strstr(r->url,"/preload"))f->preload_requests++;
        size_t n = r->body_len < sizeof q->body - 1 ? r->body_len : sizeof q->body - 1;
        if (n) memcpy(q->body, r->body, n);
        q->body[n] = 0;
        if(r->kind==WEB_RESOURCE_REPORT){
            f->policy_report_requests++;
            if(strcmp(q->method,"POST")||strcmp(q->headers,"Content-Type: application/csp-report\r\n")||
               r->credentials!=1||!r->redirect_error||r->no_cors||
               !strstr(q->body,"\"csp-report\"")||!strstr(q->body,"\"original-policy\"")||
               !strstr(q->body,"\"script-sample\"")||q->body_len>=sizeof q->body)f->policy_report_bad++;
        }
        const struct asset *a = lookup(q->url);
        q->due = uptime_ms() + (a ? a->delay_ms : 0);
        f->requests++;
        if(f->net&&!strncmp(r->url,"http://10.0.2.2:",16)){
            struct webnet_request native={.kind=r->kind==WEB_RESOURCE_REPORT?WEBNET_REPORT:r->kind==WEB_RESOURCE_FETCH?WEBNET_FETCH:
                r->kind==WEB_RESOURCE_MODULE?WEBNET_MODULE:r->kind==WEB_RESOURCE_SCRIPT?WEBNET_CLASSIC:WEBNET_RESOURCE,
                .generation=1,.url=r->url,.origin=r->origin,.method=r->method,.headers=r->headers,
                .body=r->body,.body_len=r->body_len,.credentials=(enum webnet_credentials)r->credentials,
                .same_origin=r->same_origin,.redirect_error=r->redirect_error,.redirect_manual=r->redirect_manual,
                .no_cors=r->no_cors,.no_referrer=r->no_referrer,.stream_response=r->stream_response};
            q->transport_id=webnet_submit(f->net,&native,network_complete,q);
            if(!q->transport_id){q->used=false;return false;}
        }
        return true;
    }
    return false;
}
static void cancel(void *opaque, uint64_t id) {
    struct fixture *f = opaque;
    for (int i = 0; i < SLOTS; i++) if (f->queue[i].used && f->queue[i].id == id) {
        if(f->queue[i].transport_id&&f->net)webnet_cancel(f->net,f->queue[i].transport_id);
        f->queue[i].used = false; f->cancellations++; break;
    }
}
static void resume_request(void *opaque,uint64_t id){
    struct fixture *f=opaque;
    for(int i=0;i<SLOTS;i++)if(f->queue[i].used&&f->queue[i].id==id&&f->queue[i].transport_id){
        webnet_resume(f->net,f->queue[i].transport_id);break;
    }
}
static void long_headers(struct web_response *r, const char *mime) {
    r->headers[0] = 0;
    r->headers_full = malloc(8192);
    if (!r->headers_full) { strcpy(r->error, "fixture header allocation failed"); return; }
    size_t n = (size_t)sprintf(r->headers_full, "HTTP/1.1 200 OK\r\nX-Long: ");
    memset(r->headers_full + n, 'L', 6000); n += 6000;
    sprintf(r->headers_full + n, "\r\nContent-Type: %s\r\nX-Tail: complete\r\n\r\n", mime);
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
    if (strstr(url, "/long-headers.mjs")) long_headers(r, "text/javascript");
    return r->body != NULL;
}
static void navigate(void *opaque, const struct web_navigation *n) {
    struct fixture *f = opaque;
    if (!f->doc || !web_script_running(f->doc)) f->outside_running++;
    f->navigations++;
    snprintf(f->navigation, sizeof f->navigation, "%s", n->url);
    snprintf(f->post, sizeof f->post, "%.*s", n->body ? (int)n->body_length : 0, n->body ? (const char *)n->body : "");
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
    for (int i = 0; i < SLOTS; i++) if (f->queue[i].used && !f->queue[i].transport_id && f->queue[i].due <= now) {
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
        if (strstr(q.url, "/api/long-headers")) long_headers(&r, "text/plain");
        if (strstr(q.url, "/api/header-allocation-failure")) {
            /* A real host failure, not an attempt to allocate the entire
               representable 4 GiB wire size after removing old quotas. */
            strcpy(r.error, "fixture header allocation failed");
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/echo")) {
            free(r.body); r.body = malloc(q.body_len + 1); r.body_len = q.body_len; r.status = 200;
            if (r.body) { memcpy(r.body, q.body, q.body_len); r.body[q.body_len] = 0; }
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/headers")) {
            free(r.body); r.body = strdup(q.headers); r.body_len = strlen(q.headers); r.status = 200;
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/cache-policy")) {
            char metadata[160];
            snprintf(metadata, sizeof metadata,
                "{\"cache_mode\":%d,\"fetch_requests\":%d,\"has_cache_control\":%s,\"has_pragma\":%s}",
                q.cache_mode, f->fetch_requests, strstr(q.headers, "cache-control:") ? "true" : "false",
                strstr(q.headers, "pragma:") ? "true" : "false");
            free(r.body); r.body = strdup(metadata); r.body_len = strlen(metadata); r.status = 200;
        }
        if(q.cache_mode==WEBNET_CACHE_ONLY_IF_CACHED){
            /* Bridge contract only: the real persistent HTTP cache is exercised
               by webnettest against the counted HTTP server. */
            if(!strcmp(q.url,"http://fixture.test/dir/api/cache-hit")){
                free(r.body);r.body=strdup("{\"cached\":true}");r.body_len=r.body?strlen(r.body):0;r.status=200;
            }else{r.status=0;snprintf(r.error,sizeof r.error,"No usable cached response is available");}
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/fetch-policy")) {
            char metadata[2300];
            snprintf(metadata,sizeof metadata,"{\"no_cors\":%s,\"no_referrer\":%s,\"referrer\":\"%s\",\"referrer_policy\":%d}",
                q.no_cors ? "true" : "false", q.no_referrer ? "true" : "false",q.referrer,q.referrer_policy);
            free(r.body); r.body=strdup(metadata); r.body_len=strlen(metadata); r.status=200;
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/body-presence")) {
            char metadata[96];
            snprintf(metadata,sizeof metadata,"{\"present\":%d,\"bytes\":%lu}",
                q.body_present?1:0,(unsigned long)q.body_len);
            free(r.body);r.body=strdup(metadata);r.body_len=strlen(metadata);r.status=200;
            snprintf(r.headers,sizeof r.headers,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nX-Fixture: yes\r\nX-Body-Present: %d\r\nX-Body-Bytes: %lu\r\n\r\n",
                q.body_present?1:0,(unsigned long)q.body_len);
        }
        if (f->respond) f->respond(&q, &r);
        if (q.no_cors) {
            /* This in-memory transport serves same-origin responses. Like the
               production HTTP host, affirm filtering through the private
               non-field metadata line; absence must remain a network error. */
            size_t n=strlen(r.headers);
            snprintf(r.headers+n,sizeof r.headers-n,"HTTP/Nocturne-Meta cors=0 redirected=0 opaque=0\r\n");
        }
        if (!strcmp(q.url, "http://fixture.test/dir/api/post") &&
            (strcmp(q.method, "POST") || strcmp(q.body, "Nocturne request body") || q.body_len != 21 || !strstr(q.headers, "x-fixture: yes"))) {
            snprintf(r.error, sizeof r.error, "Invalid POST request in the fixture");
        }
        size_t marks = f->nmarks;
        if (!f->native_wait&&web_script_running(f->doc)) f->outside_running++;
        web_resource_loaded(f->doc, q.id, &r);
        if (f->nmarks != marks || (!f->native_wait&&web_script_running(f->doc))) f->outside_running++;
        /* Completion has to copy: borrowed body and full headers die immediately. */
        if (r.body) memset(r.body, '!', r.body_len);
        if (r.headers_full) memset(r.headers_full, '!', strlen(r.headers_full));
        web_response_free(&r); f->completions++;
    }
}
static bool wait_request(void *opaque,uint64_t id,const bool *completed) {
    struct fixture *f=opaque;
    if(f->native_wait||!completed)return false;
    bool found=false;
    for(int i=0;i<SLOTS;i++)if(f->queue[i].used&&f->queue[i].id==id)found=true;
    if(!found)return *completed;
    uint64_t end=uptime_ms()+5000;
    f->native_wait=true;f->preload_joins++;
    while(!*completed&&uptime_ms()<end){deliver(f);n_sleep_ms(1);}
    f->native_wait=false;return *completed;
}
static void close_case(void);
static bool input_fixture_checkpoint(void *opaque) {
    struct fixture *f=opaque;
    if(f->doc) {
        if(web_native_input_ready(f->doc,false))input_ready_checkpoints++;
        else input_blocked_checkpoints++;
    }
    return true;
}
static web_doc *open_case_url_budget(const char *html, bool expected_errors, const char *url, uint32_t task_budget_ms) {
    close_case();free_marks(&fixture);
    memset(&fixture, 0, sizeof fixture); fixture.expected_errors = expected_errors;
    if(case_real_network)fixture.net=webnet_create();
    struct web_host host = {.opaque=&fixture, .request=request, .cancel=cancel, .sync_load=sync_load,.wait_request=wait_request,
                            .navigate=navigate, .console=receive_log, .scroll=scroll_position,.history=history_host,
                            .js_task_budget_ms=task_budget_ms,
                            .script_checkpoint=case_input_checkpoint?input_fixture_checkpoint:NULL};
    if(case_real_network)host.resume_request=resume_request;
    fixture.history_length = 1; fixture.history_serial = 1;
    fixture.history[0].url = strdup(url); fixture.history[0].entry = 1;
    fixture.doc = web_live_response(html, strlen(html), url, "utf-8",case_response_headers,NULL,&host);
    test_check("web-live-allocation", fixture.doc != NULL);
    return fixture.doc;
}
static web_doc *open_case_url(const char *html, bool expected_errors, const char *url) {
    return open_case_url_budget(html,expected_errors,url,0);
}
static web_doc *open_case(const char *html, bool expected_errors) { return open_case_url(html, expected_errors, BASE); }
/* Deliberate runaway fixtures opt in to a deadline. The product default stays
 * unlimited; normal finite feature cases retain the ordinary native host. */
static web_doc *open_watchdog_case(const char *html) { return open_case_url_budget(html,true,BASE,5000); }
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
    if(f->net)webnet_pump(f->net,uptime_ms());
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
        if(fixture.mark_allocation_failed)return false;
        if (marker && has_mark(&fixture, marker)) return true;
        if (!marker && !queued_count(&fixture) && web_deadline(fixture.doc) < 0) return true;
        n_sleep_ms(1);
    }
    return false;
}
static void require_marks(const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) test_check(names[i], has_mark(&fixture, names[i]));
}
static void close_case(void) {
    if (fixture.doc) { web_free(fixture.doc); fixture.doc = NULL; }
    if(fixture.net){webnet_free(fixture.net);fixture.net=NULL;}
    for (int i = 0; i < fixture.history_length; i++) { free(fixture.history[i].url); free(fixture.history[i].data); }
    fixture.history_length = 0;
    /* Some callers inspect assertion marks after closing the document. Keep
       them until the next fixture reset or final process cleanup. */
}
static char *script_page_after(const char *source,const char *after) {
    const char *a = START "</head><body style='margin:0'><div id=target style='height:20px'></div><div id=sentinel></div><script>";
    const char *b = "</script>",*end="</body></html>";
    /* API files contain literal HTML strings. An inline HTML script is not
       JavaScript-aware: escape its end-tag spelling without changing strings. */
    size_t escapes=0,length=strlen(source);
    for(size_t i=0;i+8<=length;i++)if(!strncasecmp(source+i,"</script",8))escapes++;
    char *safe=malloc(length+escapes+1);if(!safe)return NULL;
    size_t at=0;for(size_t i=0;i<length;i++){
        safe[at++]=source[i];if(source[i]=='<' && i+8<=length && !strncasecmp(source+i,"</script",8))safe[at++]='\\';
    }
    safe[at]=0;
    size_t n = strlen(a) + at + strlen(b) + strlen(after) + strlen(end) + 1+(case_meta_policy?strlen(case_meta_policy):0);
    char *page = malloc(n);
    if (page){if(case_meta_policy){const char *script=strstr(a,"<script>");snprintf(page,n,"%.*s%s%s%s%s%s%s",(int)(script-a),a,case_meta_policy,script,safe,b,after,end);}else snprintf(page, n, "%s%s%s%s%s", a, safe, b, after, end);}
    free(safe);
    return page;
}
static char *script_page(const char *source) { return script_page_after(source,""); }

/* Isolated API regressions supplement, never replace, public-site GUI checks. */
static void external_case_expected(const char *file, const char *tail, const char *url, int expected_errors) {
    printf("jstest: API batch %s\n", file); fflush(stdout);
    char path[160]; snprintf(path, sizeof path, "C:\\Tests\\%s", file);
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
        fixture.retirement_errors=expected_errors==-1 && !strcmp(file,"js_retired_task_roots_cases.js");
        /* The collation oracle covers thousands of comparisons and yields
           between batches. This is a harness deadline, not the page watchdog. */
        test_check(file, pump("api-done", !strcmp(file,"js_collator_cases.js") ? 120000 : !strcmp(file,"js_parser_worker_safety_cases.js")?60000:15000));
        if(fixture.retirement_errors)
            test_check("api-retirement-interrupts",fixture.retirement_interrupts==3 && fixture.diagnostic_batches==3 && fixture.retirement_unexpected==0 && fixture.js_failures==0);
        else test_check("api-expected-exceptions", fixture.errors == expected_errors && fixture.js_failures == 0);
        if (!strcmp(file,"js_form_validation_cases.js")) test_check("form-direct-submit-navigation",fixture.navigations==1);
        if(!strcmp(file,"js_html_csp_cases.js")){
            test_check("csp-report-transport-settles",pump(NULL,10000));
            int expected=case_meta_policy?0:case_response_headers&&strstr(case_response_headers,"Content-Security-Policy:")?10:6;
            test_check("csp-report-native-request-count",fixture.policy_report_requests==expected);
            test_check("csp-report-real-http-completion",fixture.policy_report_completed==expected&&fixture.policy_report_bad==0);
        }
        close_case();
    }
    free(page);
}
static void external_case(const char *file, const char *tail, const char *url) { external_case_expected(file,tail,url,0); }
static void test_broadcast(void) {
    external_case("js_broadcast_cases.js", ";runBroadcastCases().then(n=>{console.log('Broadcast API checks '+n);check('broadcast-count',n===27);mark('api-done');},e=>{console.log('FAIL broadcast '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_retired_jobs(void) {
    external_case("js_retired_frame_jobs_cases.js", ";runRetiredFrameJobCases().then(n=>{console.log('Retired frame API checks '+n);check('retired-job-count',n===18);mark('api-done');},e=>{console.log('FAIL retired-jobs '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_retired_tasks(void) {
    external_case_expected("js_retired_task_roots_cases.js", ";runRetiredTaskRootCases().then(n=>{console.log('Retired task root checks '+n);check('retired-task-count',n===21);mark('api-done');},e=>{console.log('FAIL retired tasks '+e+' '+e.stack);mark('api-done');});", BASE,-1);
}
static void test_error_event(void) {
    external_case("js_error_event_cases.js", ";console.log('ErrorEvent API checks '+runErrorEventCases());mark('api-done');", BASE);
}
static void test_history_receiver(void) {
    external_case("js_history_receiver_cases.js", ";runHistoryReceiverCases().then(n=>{console.log('History receiver checks '+n);check('history-receiver-count',n>=30);mark('api-done');},e=>{console.log('FAIL history receiver '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_inner_text(void) {
    external_case("js_inner_text_cases.js", ";runInnerTextCases().then(n=>{console.log('Inner text checks '+n);check('inner-text-count',n>=30);mark('api-done');},e=>{console.log('FAIL inner text '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_collections(void) {
    external_case("js_collections_cases.js", ";runCollectionCases().then(n=>{console.log('Collection API checks '+n);check('collections-count',n>=100);mark('api-done');},e=>{console.log('FAIL collections '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_document_collections_cases.js", ";runDocumentCollectionCases().then(n=>{console.log('Document collection API checks '+n);check('document-collections-count',n>=120);mark('api-done');},e=>{console.log('FAIL document collections '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_adjacent(void) {
    external_case("js_adjacent_cases.js", ";runAdjacentCases().then(n=>{console.log('Adjacent API checks '+n);check('adjacent-count',n===67);mark('api-done');},e=>{console.log('FAIL adjacent '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_fragment_constructor(void) {
    external_case("js_fragment_constructor_cases.js", ";runFragmentConstructorCases().then(n=>{console.log('Fragment constructor API checks '+n);check('fragment-constructor-count',n>=45);mark('api-done');},e=>{console.log('FAIL fragment constructor '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_resource_structure(void) {
    external_case("js_resource_structure_cases.js", ";runResourceStructureCases().then(n=>{console.log('Resource structure API checks '+n);check('resource-structure-count',n===17);mark('api-done');},e=>{console.log('FAIL resource structure '+e+' '+e.stack);mark('api-done');});", BASE);
}
static void test_detached_invalidation(void) {
    const char *page=START "</head><body style='margin:0'><div id=target style='height:20px'></div><div id=sentinel></div><script>mark('detached-ready');</script></body>";
    if (!open_case(page,false)) return;
    test_check("detached-document-ready",pump("detached-ready",3000));
    (void)web_dirty(fixture.doc);
    const char *prepare="globalThis.detached=document.createElement('div');detached.setAttribute('class','prepared');detached.style.height='35px';detached.textContent='prepared';";
    test_check("detached-mutation-eval",web_console_eval(fixture.doc,prepare,strlen(prepare)));
    /* The developer console itself requests repaint. Verify the public layout
       boundary rather than confusing that notification with style dirtiness. */
    web_layout(fixture.doc,VW,VH);
    test_check("detached-preserves-live-layout",web_anchor_y(fixture.doc,"sentinel")==20);
    const char *insert="document.body.insertBefore(detached,document.getElementById('sentinel'));";
    test_check("detached-insert-eval",web_console_eval(fixture.doc,insert,strlen(insert)));
    test_check("detached-insert-dirties-live-tree",web_dirty(fixture.doc));
    web_layout(fixture.doc,VW,VH);
    test_check("detached-insert-height",web_anchor_y(fixture.doc,"sentinel")==55);
    const char *remove="globalThis.forest=document.createElement('section');forest.appendChild(document.getElementById('target'));";
    test_check("live-to-detached-eval",web_console_eval(fixture.doc,remove,strlen(remove)));
    test_check("live-to-detached-dirties-old-parent",web_dirty(fixture.doc));
    web_layout(fixture.doc,VW,VH);
    test_check("live-to-detached-height",web_anchor_y(fixture.doc,"sentinel")==35);
    const char *image="globalThis.detachedImage=new Image();detachedImage.src='missing-detached.png';";
    test_check("detached-image-eval",web_console_eval(fixture.doc,image,strlen(image)));
    step(&fixture); /* Resource requests are issued by the next web_tick. */
    test_check("detached-image-resource-path-kept",fixture.requests>0);
    test_check("detached-no-js-errors",fixture.errors==0);
    close_case();
}
static void test_image_invalidation(void) {
    const char *page=START "<style>img{display:block;width:20px;height:20px}img[data-original='two.svg']{height:40px}</style></head>"
        "<body style='margin:0'><img id=image data-original='one.svg'><div id=sentinel></div>"
        "<picture><source id=source srcset='picture-one.svg 1x'><img id=picture></picture>"
        "<script>mark('image-invalidation-ready');</script></body>";
    if (!open_case(page,false)) return;
    test_check("image-invalidation-ready",pump("image-invalidation-ready",3000));
    web_layout(fixture.doc,VW,VH);
    test_check("image-attribute-initial-layout",web_anchor_y(fixture.doc,"sentinel")==20);
    /* HTML §3.2.6.6: no behaviour derives from data-* attributes, so
       data-original is no image candidate; src and a picture's source are. */
    const char *change="document.getElementById('image').setAttribute('data-original','two.svg');"
        "document.getElementById('image').setAttribute('src','three.svg');"
        "document.getElementById('source').setAttribute('srcset','picture-two.svg 1x');";
    test_check("image-candidate-mutation-eval",web_console_eval(fixture.doc,change,strlen(change)));
    web_layout(fixture.doc,VW,VH);
    test_check("image-candidate-preserves-attribute-selector",web_anchor_y(fixture.doc,"sentinel")==40);
    bool image_url=false,data_url=false,picture_url=false;
    for(int i=0;i<web_image_count(fixture.doc);i++) {
        const char *url=web_image_url(fixture.doc,i);
        image_url|=url && strstr(url,"/three.svg")!=NULL;
        data_url|=url && (strstr(url,"/one.svg") || strstr(url,"/two.svg"));
        picture_url|=url && strstr(url,"/picture-two.svg")!=NULL;
    }
    test_check("image-src-candidate-updated",image_url);
    test_check("image-data-attribute-not-a-candidate",!data_url);
    test_check("image-picture-source-updated",picture_url);
    for(int i=0;i<30;i++){step(&fixture);n_sleep_ms(1);}
    int previous_requests=fixture.requests;
    const char *idle="requestIdleCallback(()=>{globalThis.lastIdleImage=new Image();lastIdleImage.src='last-idle-image.svg';mark('last-idle-image-set');});";
    test_check("last-idle-image-queued",web_console_eval(fixture.doc,idle,strlen(idle)));
    test_check("last-idle-image-callback",pump("last-idle-image-set",3000));
    test_check("last-idle-image-wake",web_deadline(fixture.doc)>=0);
    step(&fixture);
    test_check("last-idle-image-dispatch",fixture.requests>previous_requests);
    test_check("image-invalidation-no-js-errors",fixture.errors==0);
    close_case();

    /* Fetch is not a load blocker, but an image in flight still is. Neither
       waits for the other: the transport owns the connection queue, so the
       image goes out beside eight held fetches. Exercise the held state
       without the harness's 1ms pump hiding a deadline that wakes forever. */
    const char *full=START "</head><body><script>"
        "addEventListener('load',()=>mark('image-queue-load'));"
        "document.addEventListener('DOMContentLoaded',()=>mark('image-queue-dom-ready'));"
        "for(let i=0;i<8;i++)fetch('/dir/api/slow').catch(()=>{});"
        "globalThis.waitingImage=new Image();waitingImage.src='queued-wanted.svg';"
        "</script></body>";
    if (!open_case(full,false)) return;
    test_check("image-queue-dom-ready",pump("image-queue-dom-ready",3000));
    test_check("image-queue-beside-held-fetches",fixture.fetch_requests==8 && fixture.requests==9 &&
        queued_count(&fixture)==9 && fixture.completions==0);
    bool queued_image=false;
    for(int i=0;i<SLOTS;i++)if(fixture.queue[i].used && fixture.queue[i].kind==WEB_RESOURCE_IMAGE &&
        strstr(fixture.queue[i].url,"/queued-wanted.svg"))queued_image=true;
    test_check("image-queue-detached-image-dispatched",queued_image);
    int future=0;
    for(int i=0;i<16;i++) {
        step(&fixture);web_layout(fixture.doc,VW,VH);
        uint64_t now=uptime_ms();int64_t deadline=web_deadline(fixture.doc);
        if(i>=8 && deadline>(int64_t)now)future++;
        n_sleep_ms(1);
    }
    test_check("image-queue-held-deadline-waits",future==8);
    test_check("image-queue-image-blocks-load",!has_mark(&fixture,"image-queue-load"));
    test_check("image-queue-load-after-image",pump("image-queue-load",3000) && fixture.requests==9 && queued_count(&fixture)==8);
    test_check("image-queue-no-js-errors",fixture.errors==0 && fixture.js_failures==0);
    close_case();
    test_check("image-queue-close-cancels-held-fetch",queued_count(&fixture)==0 && fixture.cancellations==8);
}
static void test_lexbor(void) {
    external_case("js_lexbor_cases.js", ";const r=runLexborCases();check('lexbor-probe-count',r.checks===62&&r.legacyTotal===1&&r.unsupportedAPITotal===0);mark('api-done');", BASE);
}
static void test_cssom(void) {
    external_case("js_cssom_cases.js", ";check('cssom-count',runCSSOMCases()===54);mark('api-done');", BASE);
}
static void test_computed_declaration(void) {
    external_case("js_cssom_cases.js", ";check('computed-declaration-count',runComputedStyleDeclarationCases()===24);check('placeholder-computed-count',runPlaceholderComputedStyleCases()===20);mark('api-done');", BASE);
}
static void test_fetch_api(void) {
    const char *old_headers=case_response_headers,*old_meta=case_meta_policy;
    case_response_headers="Referrer-Policy: origin\r\n";
    case_meta_policy="<meta name=\"referrer\" content=\"same-origin\">";
    external_case("js_fetch_cases.js", ";runFetchCases().then(n=>{console.log('Fetch API checks '+n);check('fetch-api-count',n>100);mark('api-done');},e=>{console.log('FAIL fetch-api '+e+' '+e.stack);mark('api-done');});", BASE);
    case_response_headers=old_headers;case_meta_policy=old_meta;
}
static void test_platform(void) {
    test_cssom();
    test_lexbor();
    external_case("js_attributes_cases.js", ";runAttributeCases().then(n=>{console.log('Attribute API checks '+n);check('attributes-count',n>70);mark('api-done');},e=>{console.log('FAIL attributes '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_tokens_cases.js", ";runTokenListCases().then(n=>{console.log('Token API checks '+n);check('tokens-count',n>100);mark('api-done');},e=>{console.log('FAIL tokens '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_shadow_cases.js", ";runShadowCases().then(n=>{console.log('Shadow API checks '+n);check('shadow-count',n>120);mark('api-done');},e=>{console.log('FAIL shadow '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_web_legacy_cases.js", ";Promise.resolve().then(()=>runWebLegacyCases()).then(n=>{console.log('Web legacy API checks '+n);check('web-legacy-count',n>30);mark('api-done');},e=>{console.log('FAIL web-legacy '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_html_elements_cases.js", ";runHTMLElementCases().then(n=>{console.log('HTML element API checks '+n);check('html-elements-count',n>100);mark('api-done');},e=>{console.log('FAIL html-elements '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_html_interface_cases.js", ";try{const n=runHTMLInterfaceCases();console.log('HTML interface API checks '+n);check('html-interfaces-count',n>500);}catch(e){console.log('FAIL html-interfaces '+e+' '+e.stack);}mark('api-done');", BASE);
    external_case("js_form_controls_cases.js", ";Promise.resolve().then(()=>runFormControlCases()).then(n=>{console.log('Form control API checks '+n);check('form-controls-count',n>250);mark('api-done');},e=>{console.log('FAIL form-controls '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_form_validation_cases.js", ";Promise.resolve().then(()=>runFormValidationCases()).then(n=>{console.log('Form validation API checks '+n);check('form-validation-count',n>150);mark('api-done');},e=>{console.log('FAIL form-validation '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_semantic_elements_cases.js", ";Promise.resolve().then(()=>runSemanticElementCases()).then(n=>{console.log('Semantic element API checks '+n);check('semantic-elements-count',n>200);mark('api-done');},e=>{console.log('FAIL semantic-elements '+e+' '+e.stack);mark('api-done');});", BASE);
    test_fetch_api();
    external_case("js_css_supports_cases.js", ";mark('api-done');", BASE);
    external_case("js_svg_dom_cases.js", ";check('svg-dom-count',runSVGDOMCases()>=30);mark('api-done');", BASE);
    external_case("js_url_cases.js", ";for(const f of __urlTestResults.failures)console.log('FAIL url '+f.name+' '+f.error);check('url-count',__urlTestResults.total===32);mark('api-done');", BASE);
    external_case("js_url_wpt_cases.js", ";const r=runURLWPTCases();for(const f of r.failures)console.log('FAIL url-wpt '+f);console.log('URL WPT API checks '+r.total);check('url-wpt',r.total===1262&&r.failures.length===0);mark('api-done');", BASE);
    external_case("js_crypto_cases.js", ";runCryptoCases().then(n=>{check('crypto-count',n>100);mark('api-done');},e=>{console.log('FAIL crypto '+e);mark('api-done');});", "https://fixture.test/dir/index.html");
    external_case("js_crypto_cases.js", ";runCryptoCases().then(n=>{check('insecure-crypto-count',n>30);mark('api-done');},e=>{console.log('FAIL crypto '+e);mark('api-done');});", BASE);
    external_case("js_clone_cases.js", ";mark('api-done');", BASE);
    external_case("js_encoding_cases.js", ";check('encoding-count',runEncodingCases()>170);mark('api-done');", BASE);
    external_case("js_intl_cases.js", ";runIntlCases().then(n=>{check('intl-count',n===106);mark('api-done');},e=>{console.log('FAIL intl '+e);mark('api-done');});", BASE);
    external_case("js_collator_cases.js", ";runCollatorCases().then(n=>{console.log('Collator API checks '+n);check('collator-count',n>7000);mark('api-done');},e=>{console.log('FAIL collator '+e+' '+e.stack);mark('api-done');});", BASE);
    external_case("js_history_cases.js", ";runHistoryCases().then(()=>mark('api-done'),e=>{console.log('FAIL history '+e);mark('api-done');});", BASE);
    test_check("location-reentrant-native-navigation",!strcmp(fixture.navigation,"http://fixture.test/dir/index.html?cache=5#outside"));
    external_case("js_geometry_cases.js", ";check('geometry-count',runGeometryCases()===34);mark('api-done');", BASE);
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

static void test_xhr(void) {
    external_case("js_xhr_cases.js", ";runXHRCases().then(n=>{console.log('XHR checks '+n);check('xhr-count',n>=60);mark('api-done');},e=>{console.log('FAIL xhr '+e);mark('api-done');});", BASE);
    test_check("xhr-native-cancel-reached",fixture.cancellations>=3);
    test_check("xhr-native-request-reached",fixture.requests>=10);
    test_check("xhr-not-during-native-completion",fixture.outside_running==0);
}
static void test_documents(void) {
    external_case("js_document_cases.js", ";try{const n=runDocumentCases();console.log('Document checks '+n);check('document-count',n>=80);}catch(e){console.log('FAIL documents '+e+' '+e.stack);}mark('api-done');", BASE);
    test_check("inert-document-no-network",fixture.requests==0);
    test_check("inert-document-no-navigation",fixture.navigations==0);
    external_case("js_document_cases.js", ";try{check('document-count-capacity',runDocumentCapacityCases('count')>=10);}catch(e){console.log('FAIL document count capacity '+e+' '+e.stack);}mark('api-done');", BASE);
    external_case("js_document_cases.js", ";try{check('document-arena-capacity',runDocumentCapacityCases('arena')>=10);}catch(e){console.log('FAIL document arena capacity '+e+' '+e.stack);}mark('api-done');", BASE);
}
static void test_xml_serialization(void) {
    external_case("js_document_cases.js", ";try{const n=runXMLSerializerCases();console.log('XMLSerializer checks '+n);check('xml-serialization-count',n===53);}catch(e){console.log('FAIL XMLSerializer '+e+' '+e.stack);}mark('api-done');", BASE);
    test_check("xml-serialization-no-network",fixture.requests==0);
    test_check("xml-serialization-no-navigation",fixture.navigations==0);
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
        "check('no-os-exposure',typeof std==='undefined'&&typeof os==='undefined'&&typeof Atomics==='object'&&typeof SharedArrayBuffer==='undefined'&&crossOriginIsolated===false);"
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
            test_check("mime-import-fetched-with-graph",fixture.sync_loads==0); /* not by the synchronous loader */
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
/* A graph whose dependency failed fails again for every later importer:
   two scripts and two import() calls link the same parsed parent. */
static void test_failed_module_graph(void) {
    if (!open_case(START "<script type=module onerror=\"mark('failed-graph-first-script')\">import './broken-parent.mjs';</script>"
                   "<script type=module onerror=\"mark('failed-graph-second-script')\">import './broken-parent.mjs';</script>"
                   "<script>addEventListener('load',()=>import('./broken-parent.mjs').then(()=>check('failed-graph-import',false),"
                   "first=>import('./broken-parent.mjs').then(()=>check('failed-graph-import',false),"
                   "again=>{check('failed-graph-import',first instanceof Error&&again instanceof Error);mark('failed-graph-done');})));</script>"
                   "</head><body>native</body>", true)) return;
    test_check("failed-graph-imports-rejected", pump("failed-graph-done", 6000));
    require_marks((const char *const[]){"failed-graph-first-script","failed-graph-second-script"},2);
    test_check("failed-graph-checks", fixture.js_failures == 0);
    test_check("failed-graph-fetched-with-graph", fixture.sync_loads == 0);
    close_case();
}

static void test_dom_and_scripts(void) {
    const char *html = START
        "<script>globalThis.moduleStarted=globalThis.moduleRan=false;globalThis.moduleGate=new Promise(resolve=>globalThis.moduleRelease=resolve);</script>"
        "<link rel=stylesheet href=style.css><script defer src=defer.js></script><script async src=async.js></script>"
        "<script type=module src=main.mjs></script></head><body style='margin:0'>"
        "<div id=target></div><div id=sentinel></div><script>"
        "check('parser-boundary',document.getElementById('future')===null);"
        "check('classic-current-script',document.currentScript&&document.currentScript.localName==='script');"
        "document.write('<span id=written>one</span>');document.write('<span id=written2>two</span>');"
        "document.addEventListener('DOMContentLoaded',()=>{globalThis.domContentObserved=true;check('dom-content-order',deferRan&&moduleStarted&&!moduleRan&&classicRan);"
        "check('document-write-order',document.getElementById('written').nextSibling===document.getElementById('written2'));"
        "moduleRelease();mark('dom-ready');});"
        "window.addEventListener('load',()=>{check('window-load-order',document.readyState==='complete');mark('page-loaded');});"
        "</script><div id=future></div><script src=classic.js></script><script>"
        "const target=document.getElementById('target');target.style.height='37px';target.className='one two';"
        "check('dom-node-identity',document.querySelector('#target')===target);"
        "check('dom-prototype-hierarchy',Object.getPrototypeOf(target)===HTMLDivElement.prototype&&Object.getPrototypeOf(HTMLDivElement.prototype)===HTMLElement.prototype&&Object.getPrototypeOf(HTMLElement.prototype)===Element.prototype&&Object.getPrototypeOf(Element.prototype)===Node.prototype&&target instanceof HTMLDivElement&&target instanceof HTMLElement&&target instanceof Element&&target instanceof Node);"
        "check('dom-document-interface',Object.getPrototypeOf(document)===HTMLDocument.prototype&&Object.getPrototypeOf(HTMLDocument.prototype)===Document.prototype&&document instanceof Document&&document instanceof Node&&!(document instanceof Element)&&document.className===undefined&&document.getAttribute===undefined&&document.matches===undefined&&document.href===undefined);"
        "check('dom-element-prototype-method',typeof Element.prototype.matches==='function'&&Element.prototype.matches.call(target,'#target')&&Node.prototype.matches===undefined);"
        "class DerivedElement extends Element{};check('dom-no-fake-instanceof',!(target instanceof DerivedElement));"
        "check('dom-selector',target.matches('.one.two')&&document.querySelector('body > #target')===target);"
        "target.classList.remove('two');check('dom-class-cache',!target.matches('.two'));"
        "target.setAttribute('DATA-VALUE','yes');check('dom-attribute-case',target.getAttribute('data-value')==='yes'&&target.getAttribute('DATA-VALUE')==='yes');"
        "const child=document.createElement('span');child.textContent='native text';target.appendChild(child);child.remove();"
        "check('dom-created-interface',Object.getPrototypeOf(child)===HTMLSpanElement.prototype&&child instanceof HTMLSpanElement&&child instanceof HTMLElement&&child instanceof Node);"
        "check('dom-detached',!child.isConnected&&child.textContent==='native text');target.appendChild(child);"
        "check('dom-reattach',target.firstChild===child&&child.parentNode===target);"
        "const fragment=document.createDocumentFragment();fragment.append('a',document.createElement('strong'));fragment.lastChild.textContent='b';target.appendChild(fragment);"
        "check('dom-fragment',fragment.childNodes.length===0&&target.lastChild.textContent==='b');"
        "check('dom-fragment-interface',Object.getPrototypeOf(fragment)===DocumentFragment.prototype&&fragment instanceof Node&&!(fragment instanceof Element)&&fragment.className===undefined&&typeof fragment.querySelector==='function');"
        "const textNode=document.createTextNode('text'),commentNode=document.createComment('comment');"
        "check('dom-character-interfaces',Object.getPrototypeOf(textNode)===Text.prototype&&textNode instanceof CharacterData&&textNode instanceof Node&&commentNode instanceof Comment&&commentNode instanceof CharacterData&&textNode.className===undefined&&textNode.getAttribute===undefined);"
        "const foreign=document.createElementNS('http://www.w3.org/2000/svg','svg'),foreignGroup=document.createElementNS('http://www.w3.org/2000/svg','g');check('dom-foreign-interface',foreign instanceof SVGSVGElement&&foreign instanceof SVGElement&&foreign instanceof Element&&foreign instanceof Node&&!(foreign instanceof HTMLElement)&&Object.getPrototypeOf(foreign)===SVGSVGElement.prototype&&foreignGroup instanceof SVGGElement&&foreignGroup instanceof SVGGraphicsElement&&foreignGroup instanceof SVGElement&&foreignGroup instanceof Element&&!(foreignGroup instanceof SVGSVGElement)&&!(foreignGroup instanceof HTMLElement)&&Object.getPrototypeOf(foreignGroup)===SVGGElement.prototype);"
        "const clone=child.cloneNode(true);check('dom-clone',clone!==child&&clone.textContent===child.textContent);"
        "check('dom-clone-interface',Object.getPrototypeOf(clone)===HTMLSpanElement.prototype&&clone instanceof HTMLSpanElement&&clone instanceof HTMLElement&&clone.firstChild instanceof Text);"
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
        "const fakePolymer=Object.create(HTMLElement.prototype);fakePolymer.async=function(){return 42;};"
        "check('polymer-async-isolation',fakePolymer.async()===42&&Object.getOwnPropertyDescriptor(HTMLElement.prototype,'async')===undefined);"
        "const testLink=document.createElement('a');testLink.setAttribute('href','/dir/test?foo=bar#target');"
        "check('anchor-url-reflection',testLink instanceof HTMLAnchorElement&&testLink.pathname==='/dir/test'&&testLink.search==='?foo=bar'&&testLink.hash==='#target'&&testLink.origin==='http://fixture.test'&&testLink.host==='fixture.test'&&testLink.toString()===testLink.href);"
        "testLink.pathname='/dir/modified';check('anchor-pathname-setter',testLink.pathname==='/dir/modified'&&testLink.getAttribute('href')==='http://fixture.test/dir/modified?foo=bar#target');"
        "const testForm=document.createElement('form');const fInput=document.createElement('input');fInput.name='field';testForm.appendChild(fInput);"
        "check('form-interface-properties',testForm instanceof HTMLFormElement&&testForm.elements.length===1&&testForm.elements['field']===fInput&&testForm.length===1);"
        "testForm.method='INVALID';check('form-method-default',testForm.method==='get');"
        "check('form-elements-sameobject',testForm.elements===testForm.elements);check('form-action-document-url',testForm.action===document.URL);testForm.action='';check('form-action-empty-document-url',testForm.action===document.URL);"
        "const publicURL=URL,urlPath=Object.getOwnPropertyDescriptor(URL.prototype,'pathname');try{globalThis.URL=function(){throw Error('public URL');};Object.defineProperty(publicURL.prototype,'pathname',{configurable:true,get(){return 'poison';},set(){throw Error('public URL prototype');}});testLink.pathname='/private';check('anchor-private-url',testLink.pathname==='/private'&&testLink.href==='http://fixture.test/private?foo=bar#target');}finally{globalThis.URL=publicURL;Object.defineProperty(publicURL.prototype,'pathname',urlPath);}"
        "const conversionError=Error('conversion');let conversionThrew=false;try{testLink.pathname={toString(){throw conversionError;}};}catch(e){conversionThrew=e===conversionError;}check('anchor-conversion-error',conversionThrew);"
        "let anchorBrand=false;try{Object.getOwnPropertyDescriptor(HTMLAnchorElement.prototype,'pathname').get.call(document.createElement('div'));}catch(e){anchorBrand=e instanceof TypeError;}check('anchor-native-brand',anchorBrand);testLink.href='x\\ud800';check('anchor-usvstring',testLink.getAttribute('href')==='x\\ufffd');"
        "const apiText=document.createTextNode('x\\ud83d\\ude00'),apiComment=document.createComment('comment');check('characterdata-utf16',apiText.data==='x\\ud83d\\ude00'&&apiText.length===3);apiComment.data='updated';check('characterdata-comment',apiComment.nodeValue==='updated'&&apiComment.length===7);apiText.data=null;let badData=false;try{apiText.data=Symbol();}catch(e){badData=e instanceof TypeError;}check('characterdata-conversion',apiText.data===''&&apiText.length===0&&badData);let dataBrand=false;try{Object.getOwnPropertyDescriptor(CharacterData.prototype,'data').get.call(testForm);}catch(e){dataBrand=e instanceof TypeError;}check('characterdata-brand',dataBrand);"
        "const tmpl=document.createElement('template');tmpl.innerHTML='<img id=inert-tmpl-img src=\"classic.js\">';document.body.appendChild(tmpl);"
        "check('template-inert-image',tmpl.content.querySelector('#inert-tmpl-img').ownerDocument!==document);"
        "mark('dom-script-done');</script></body></html>";
    if (!open_case(html, false)) return;
    test_check("dom-script-finished", pump("page-loaded", 8000));
    test_check("dom-jobs-drained", pump(NULL, 4000));
    static const char *const names[] = {"parser-boundary","classic-current-script","external-classic","blocking-css","defer-after-parser","async-loaded","module-cycle","module-current-script","module-import-meta","dynamic-import","module-top-level-await","dom-content-order","document-write-order","window-load-order","dom-node-identity","dom-selector","dom-class-cache","dom-attribute-case","dom-detached","dom-reattach","dom-fragment","dom-clone","inner-html-inert","dynamic-inline","dynamic-empty-then-text","cloned-script-inert","dom-geometry","dynamic-external-order","module-cache","polymer-async-isolation","anchor-url-reflection","anchor-pathname-setter","form-interface-properties","form-method-default","form-elements-sameobject","form-action-document-url","form-action-empty-document-url","anchor-private-url","anchor-conversion-error","anchor-native-brand","anchor-usvstring","characterdata-utf16","characterdata-comment","characterdata-conversion","characterdata-brand","template-inert-image"};
    require_marks(names, sizeof names / sizeof *names);
    static const char *const interfaces[] = {"dom-prototype-hierarchy","dom-document-interface","dom-element-prototype-method","dom-no-fake-instanceof","dom-created-interface","dom-fragment-interface","dom-character-interfaces","dom-foreign-interface","dom-clone-interface"};
    require_marks(interfaces, sizeof interfaces / sizeof *interfaces);
    test_check("native-title", !strcmp(web_title(fixture.doc), "QuickJS native DOM"));
    int y = web_anchor_y(fixture.doc, "target"), sentinel = web_anchor_y(fixture.doc, "sentinel");
    test_check("native-layout-after-js", y >= 0 && sentinel - y == 37);
    test_check("host-completions-copy-only", fixture.completions >= 7 && fixture.outside_running == 0);
    test_check("module-graphs-fetched-without-sync-loader", fixture.sync_loads == 0);
    test_check("dom-no-exceptions", fixture.errors == 0);
    close_case();
    const char *late=START "</head><body><p id=previous>old</p><script>"
        "const savedDocument=document,oldRoot=document.documentElement,retained=document.createElement('span');"
        "document.addEventListener('DOMContentLoaded',()=>{document.write('<!doctype html><title>Replacement</title><p id=late>late</p>');"
        "check('late-write-opens',document===savedDocument&&document.documentElement!==oldRoot&&document.getElementById('previous')===null&&document.getElementById('late').textContent==='late'&&document.readyState==='loading');"
        "check('late-write-owner',retained.ownerDocument===document&&retained.isConnected===false&&oldRoot.ownerDocument===document);"
        "document.close();mark('late-write-done');});</script></body>";
    if(open_case(late,false)){
        test_check("late-write-finished",pump("late-write-done",5000));
        test_check("late-write-opens",has_mark(&fixture,"late-write-opens"));
        test_check("late-write-owner",has_mark(&fixture,"late-write-owner"));
        test_check("late-write-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();
    }
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
        "check('form-interface-brand',form instanceof HTMLFormElement&&Object.getPrototypeOf(form)===HTMLFormElement.prototype);"
        "const resetForm=document.createElement('form'),resetInput=document.createElement('input'),resetCheck=document.createElement('input'),resetArea=document.createElement('textarea');document.body.appendChild(resetForm);resetCheck.type='checkbox';resetForm.append(resetInput,resetCheck,resetArea);"
        "resetInput.defaultValue='default';resetInput.value='edited';resetCheck.defaultChecked=true;resetCheck.checked=false;resetArea.defaultValue='area-default';resetArea.value='area-edit';let resetEvents=0;const reenterReset=()=>{if(++resetEvents===1)resetForm.reset();};resetForm.addEventListener('reset',reenterReset);resetForm.reset();resetForm.removeEventListener('reset',reenterReset);check('form-reset-reentry',resetEvents===1);"
        "check('form-reset-native-values',resetInput.value==='default'&&resetCheck.checked&&resetArea.value==='area-default');resetInput.defaultValue='next';resetCheck.defaultChecked=false;resetArea.defaultValue='area-next';check('form-reset-pristine',resetInput.value==='next'&&!resetCheck.checked&&resetArea.value==='area-next');"
        "resetArea.firstChild.data='child-next';check('textarea-pristine-child-data',resetArea.value==='child-next');resetArea.replaceChildren(document.createTextNode('replaced'));check('textarea-pristine-replace',resetArea.value==='replaced');resetArea.value='keep';resetArea.firstChild.data='new-default';check('textarea-dirty-child-preserved',resetArea.value==='keep');"
        "const resetRadioOne=document.createElement('input'),resetRadioTwo=document.createElement('input');for(const r of [resetRadioOne,resetRadioTwo]){r.type='radio';r.name='reset-group';r.defaultChecked=true;resetForm.appendChild(r);}resetForm.reset();check('form-reset-radio-group',!resetRadioOne.checked&&resetRadioTwo.checked&&resetRadioOne.defaultChecked&&resetRadioTwo.defaultChecked);"
        "let badSubmit=false,foreignSubmit=false;try{resetForm.requestSubmit(resetInput);}catch(e){badSubmit=e instanceof TypeError;}try{resetForm.requestSubmit(document.createElement('button'));}catch(e){foreignSubmit=e instanceof DOMException&&e.name==='NotFoundError';}check('requestsubmit-invalid-controls',badSubmit&&foreignSubmit);resetForm.remove();"
        "const formDoc=document.implementation.createHTMLDocument('form-owner'),ownedForm=formDoc.createElement('form'),ownedControl=formDoc.createElement('input');ownedForm.id='external-form';ownedControl.setAttribute('form','external-form');formDoc.body.append(ownedForm,ownedControl);check('form-independent-document-owner',ownedControl.form===ownedForm&&ownedForm.elements[0]===ownedControl&&ownedForm.action===formDoc.URL);ownedControl.defaultValue='owned';ownedControl.value='edited';ownedForm.reset();check('form-independent-document-reset',ownedControl.value==='owned');"
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
    static const char *const names[] = {"event-capture-bubble-cancel","event-once-remove","event-stop-immediate","event-passive","event-custom","native-event-fields","native-event-microtask","native-key-fields","native-key-done","form-submit-canceled","form-external-owner","form-interface-brand","form-reset-reentry","form-reset-native-values","form-reset-pristine","textarea-pristine-child-data","textarea-pristine-replace","textarea-dirty-child-preserved","form-reset-radio-group","requestsubmit-invalid-controls","form-independent-document-owner","form-independent-document-reset","inline-handler-canceled","control-pristine-value","control-dirty-value","control-focus","control-blur","control-checkbox-click","control-dirty-checked","control-radio-exclusion","control-select-optgroup","control-select-none","control-option-selected"};
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

static void test_host_microtasks(void) {
    const char *source =
        "const P=Promise,resolve=P.resolve,then=P.prototype.then,catcher=P.prototype.catch;let calls=0,ran=false,order=[];"
        "globalThis.Promise={resolve(){calls++;throw Error('page Promise must not be used');}};"
        "try{check('microtask-return',queueMicrotask(()=>{ran=true;})===undefined);}catch(e){check('microtask-return',false);}"
        "globalThis.Promise=P;P.resolve=P.prototype.then=P.prototype.catch=function(){calls++;throw Error('page Promise methods must not be used');};"
        "try{queueMicrotask(function(){'use strict';check('microtask-callback',this===undefined&&arguments.length===0);});}catch(e){check('microtask-callback',false);}"
        "P.resolve=resolve;P.prototype.then=then;P.prototype.catch=catcher;"
        "check('microtask-async',!ran);check('microtask-no-page-promise',calls===0);"
        "for(const bad of [undefined,null,42,{},'callback']){let threw=false;try{queueMicrotask(bad);}catch(e){threw=e instanceof TypeError;}check('microtask-invalid-'+String(bad),threw);}"
        "queueMicrotask(()=>{order.push('q1');queueMicrotask(()=>order.push('nested'));});"
        "Promise.resolve().then(()=>order.push('p'));queueMicrotask(()=>order.push('q2'));"
        "let assimilated=false;queueMicrotask(()=>({get then(){assimilated=true;throw Error('return ignored');}}));"
        "setTimeout(()=>{check('microtask-ran',ran);check('microtask-fifo',order.join(',')==='q1,p,q2,nested');check('microtask-return-ignored',!assimilated);mark('host-microtasks-done');},0);";
    char *page=script_page(source);
    test_check("host-microtask-page",page!=NULL);
    if(page && open_case(page,false)) {
        test_check("host-microtasks-finished",pump("host-microtasks-done",5000));
        static const char *const names[]={"microtask-return","microtask-callback","microtask-async","microtask-no-page-promise",
            "microtask-invalid-undefined","microtask-invalid-null","microtask-invalid-42","microtask-invalid-[object Object]","microtask-invalid-callback",
            "microtask-ran","microtask-fifo","microtask-return-ignored"};
        require_marks(names,sizeof names/sizeof *names);
        test_check("host-microtasks-no-errors",fixture.errors==0);close_case();
    }
    free(page);
    page=script_page("let done=false;queueMicrotask(()=>{throw Error('host-microtask-failure');});queueMicrotask(()=>done=true);"
                     "setTimeout(()=>{check('microtasks-drain-after-error',done);mark('microtask-error-done');},0);");
    test_check("microtask-error-page",page!=NULL);
    if(page && open_case(page,true)) {
        test_check("microtask-error-finished",pump("microtask-error-done",5000));
        test_check("microtask-error-drained",has_mark(&fixture,"microtasks-drain-after-error"));
        /* One exception now has several console diagnostics. Retain the whole
           bounded report rather than overwriting its error with the excerpt. */
        test_check("microtask-error-reported",fixture.errors>0&&fixture.diagnostic_batches==1&&strstr(fixture.error_trace,"host-microtask-failure")!=NULL);
        test_check("microtask-error-not-rejection",strstr(fixture.error_trace,"Unhandled promise rejection")==NULL);
        close_case();
    }
    free(page);
    page=script_page("Promise.reject(new Error('module-diagnostic'));setTimeout(()=>mark('rejection-done'),0);");
    test_check("rejection-stack-page",page!=NULL);
    if(page && open_case(page,true)) {
        test_check("rejection-stack-finished",pump("rejection-done",5000));
        test_check("rejection-stack-reported",fixture.errors>0&&fixture.diagnostic_batches==1&&strstr(fixture.error_trace,"Unhandled promise rejection: Error: module-diagnostic")&&strstr(fixture.error_trace,BASE));
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
        "try{await fetch('file:///C:/Users/User/secret');check('fetch-local-rejected',false);}catch(e){check('fetch-local-rejected',e instanceof TypeError);}"
        "try{await fetch('api/json',{method:'TRACE'});check('fetch-method-rejected',false);}catch(e){check('fetch-method-rejected',e instanceof TypeError);}"
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
    const char *tail = "<div id=afterbudget style='height:18px'>Native UI survives</div><script>mark('must-not-run');</script>";
    size_t n = strlen(source) + 1;
    char *prepared = malloc(n);
    test_check("budget-source-allocation", prepared != NULL);
    if (!prepared) return;
    memcpy(prepared,source,n);
    /* Escape JavaScript's literal end tags, never the actual document boundary
       or native UI suffix. Otherwise the intended runaway is a syntax error. */
    char *page = script_page_after(prepared,tail); free(prepared);
    test_check("budget-page-allocation", page != NULL);
    if (!page) return;
    if (open_watchdog_case(page)) {
        uint64_t start = uptime_ms();
        bool stopped=pump(NULL,10000);
        uint64_t elapsed = uptime_ms() - start;
        int64_t deadline=web_deadline(fixture.doc);
        printf("BUDGET-TIME %s elapsed_ms=%lu settled=%d deadline=%lld\n",name,(unsigned long)elapsed,stopped?1:0,(long long)deadline);
        test_check(name,stopped);
        test_check("budget-stopped-within-bound", elapsed >= 4900 && elapsed < 9000);
        test_check("budget-reported", fixture.errors > 0 && strstr(fixture.last_error, "5 second") != NULL);
        test_check("budget-not-running", !web_script_running(fixture.doc));
        test_check("budget-further-js-disabled", !has_mark(&fixture, "must-not-run"));
        test_check("budget-parser-native-survives", web_anchor_y(fixture.doc, "afterbudget") >= 0);
        test_check("budget-native-idle",deadline<0&&queued_count(&fixture)==0);
        struct web_event e = {.type="click",.cancelable=true};
        test_check("budget-native-ui-survives", web_dispatch(fixture.doc, NULL, &e));
        close_case();
    }
    free(page);
}

static void test_allocation_contract(const char *kind, const char *source, const char *marker) {
    char *page = script_page(source);
    test_check("limit-page-allocation", page != NULL);
    if (!page) return;
    if (open_case(page, false)) {
        test_check(kind, pump(marker, 12000));
        test_check("allocation-contract-completed", has_mark(&fixture, marker));
        test_check("allocation-contract-not-running", !web_script_running(fixture.doc));
        test_check("allocation-contract-no-uncaught", fixture.errors == 0);
        test_check("allocation-contract-native-survives", web_anchor_y(fixture.doc, "sentinel") >= 0);
        close_case();
    }
    free(page);
}

static void test_limits(void) {
    test_execution_budget("loop-budget", "mark('loop-start');for(;;){}");
    test_execution_budget("microtask-budget", "mark('microtask-start');function forever(){Promise.resolve().then(forever);}forever();");
    test_execution_budget("host-microtask-budget", "mark('microtask-start');function forever(){queueMicrotask(forever);}forever();");
    /* The old 128MiB/32MiB/128-handle policy was removed from production.
       Exercise the real contract with finite allocations, not a loop that
       consumes all guest RAM waiting for a quota which no longer exists.
       Native allocation-failure/queue-growth tests separately inject OOM. */
    test_allocation_contract("heap-past-former-128m-cap",
        "let buffers=[];function consume(){for(let i=0;i<4&&buffers.length<129;i++)buffers.push(new ArrayBuffer(1048576));"
        "if(buffers.length<129){setTimeout(consume,0);return;}new Uint8Array(buffers[128])[1048575]=73;"
        "check('heap-past-old-bound',buffers.length===129&&new Uint8Array(buffers[128])[1048575]===73);"
        "let rejected=false;try{new ArrayBuffer(2**53);}catch(e){rejected=e instanceof RangeError;}check('heap-length-overflow',rejected);"
        "buffers.length=0;mark('heap-complete');}consume();", "heap-complete");
    require_marks((const char *const[]){"heap-past-old-bound","heap-length-overflow"}, 2);
    test_allocation_contract("dom-past-former-32m-cap",
        "const payload='x'.repeat(262144);let nodes=[];function consume(){for(let i=0;i<4&&nodes.length<136;i++)nodes.push(document.createTextNode(payload));"
        "if(nodes.length<136){setTimeout(consume,0);return;}check('dom-past-old-bound',nodes.length===136&&nodes[0].data===payload&&nodes[135].length===262144);"
        "nodes[135].data='survives';check('dom-after-allocation',nodes[135].data==='survives');mark('dom-complete');}consume();", "dom-complete");
    require_marks((const char *const[]){"dom-past-old-bound","dom-after-allocation"}, 2);
    test_allocation_contract("timers-past-former-128-cap",
        "let handles=[];for(let i=0;i<130;i++)handles.push(setTimeout(()=>{},60000));"
        "check('timer-past-old-bound',handles.length===130&&new Set(handles).size===130);for(const h of handles)clearTimeout(h);"
        "setTimeout(()=>mark('timer-complete'),0);", "timer-complete");
    require_marks((const char *const[]){"timer-past-old-bound"}, 1);

    char *page = script_page("document.getElementById('target').addEventListener('runaway',e=>{e.preventDefault();for(;;){}});mark('runaway-ready');");
    test_check("runaway-event-page-allocation", page != NULL);
    if (page && open_watchdog_case(page)) {
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

static void test_scripting_presentation(void) {
    const char *page="<!doctype html><style>body{margin:0}noscript{display:block!important;height:40px}"
        "#mode{height:10px}@media(scripting:enabled){#mode{height:20px}}</style><body>"
        "<noscript style='display:block!important'><b id=fallback>fallback</b></noscript><div id=mode></div><div id=sentinel></div><script>"
        PRELUDE "check('noscript-rawtext',document.getElementById('fallback')===null);"
        "check('noscript-hidden',getComputedStyle(document.querySelector('noscript')).display==='none');"
        "mark('scripting-ready');for(;;){}</script>";
    web_doc *static_doc=web_parse(page,strlen(page),BASE,"utf-8");
    test_check("scripting-disabled-document",static_doc!=NULL);
    if(static_doc){web_layout(static_doc,VW,VH);test_check("noscript-disabled-fallback",web_anchor_y(static_doc,"sentinel")==50);web_free(static_doc);}
    if(open_watchdog_case(page)) {
        /* Like the browser's navigation path, establish the viewport before
           the first script runs; this page has no blocking external resource. */
        web_layout(fixture.doc,VW,VH);
        test_check("scripting-document-executed",pump("scripting-ready",12000));
        test_check("noscript-live-rawtext",has_mark(&fixture,"noscript-rawtext"));
        test_check("noscript-ua-hides",has_mark(&fixture,"noscript-hidden"));
        test_check("scripting-watchdog-fired",strstr(fixture.last_error,"5 second")!=NULL);
        web_layout(fixture.doc,VW+1,VH);
        test_check("noscript-remains-hidden-after-watchdog",web_anchor_y(fixture.doc,"sentinel")==20);
        close_case();
    }
}

static void test_native_selection(void) {
    const char *source =
        "const input=document.createElement('input');input.style.cssText='position:absolute;left:0;top:0;width:200px;height:30px';"
        "document.body.appendChild(input);input.value='abcdef';input.setSelectionRange(1,4);input.focus();let stage=0;"
        "const log=(kind,e)=>console.log('NATIVE-SELECTION '+kind+' stage='+stage+' value='+JSON.stringify(input.value)+' range='+input.selectionStart+':'+input.selectionEnd+':'+input.selectionDirection+(e?' data='+JSON.stringify(e.data)+' type='+e.inputType+' cancelled='+e.defaultPrevented:''));"
        "input.addEventListener('beforeinput',e=>log('beforeinput',e));input.addEventListener('input',e=>log('input',e));log('ready');"
        "input.addEventListener('selection-probe',()=>{"
        "log('probe');"
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
            printf("NATIVE-SELECTION key stage=%u result=%d\n",i,result);fflush(stdout);
            /* Rejected maxlength growth still consumes the editing key. */
            test_check("native-selection-key-handled",result==1);
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

/* The editing commands (js_editing_cases.js), then the user's keys in an
   editing host through web_key: typing, Backspace, Left, Enter and Ctrl+Z go
   through the editing layer's beforeinput, command and input. */
static void test_editing(void) {
    external_case("js_editing_cases.js", ";try{const n=runEditingCases();console.log('Editing checks '+n);check('editing-count',n>=60);}catch(e){console.log('FAIL editing '+e+' '+e.stack);}mark('api-done');", BASE);
    const char *source =
        "const editor=document.createElement('div');editor.contentEditable='true';editor.style.cssText='position:absolute;left:0;top:0;width:200px;height:30px';"
        "editor.textContent='xy';document.body.appendChild(editor);let stage=0;const seen=[];"
        "for(const type of ['beforeinput','input'])editor.addEventListener(type,e=>seen.push(e.type+':'+e.inputType+':'+e.data+(e.isTrusted?'':':untrusted')));"
        "editor.focus();getSelection().collapse(editor.firstChild,1);"
        "editor.addEventListener('editing-probe',()=>{"
        "console.log('NATIVE-EDITING stage='+stage+' html='+JSON.stringify(editor.innerHTML)+' events='+seen.join(' '));"
        "if(stage===0)check('native-editing-type',editor.innerHTML==='xay'&&seen.join(' ')==='beforeinput:insertText:a input:insertText:a');"
        "if(stage===1)check('native-editing-type-again',editor.innerHTML==='xaby');"
        "if(stage===2)check('native-editing-backspace',editor.innerHTML==='xay');"
        "if(stage===3)check('native-editing-left',getSelection().focusOffset===1&&getSelection().isCollapsed);"
        "if(stage===4)check('native-editing-enter',editor.innerHTML==='<div>x</div><div>ay</div>');"
        "if(stage===5)check('native-editing-undo',editor.innerHTML==='xay');"
        "seen.length=0;stage++;mark('native-editing-step-'+stage);});mark('native-editing-ready');";
    char *page=script_page(source);
    test_check("native-editing-page",page!=NULL);
    if(page && open_case(page,false)) {
        test_check("native-editing-ready",pump("native-editing-ready",5000));
        test_check("native-editing-target",web_editing_target(fixture.doc));
        web_node *editor=web_focused(fixture.doc);
        test_check("native-editing-focus",editor!=NULL);
        static const struct {uint32_t key,mods;} keys[]={{'a',0},{'b',0},{NKEY_BACKSPACE,0},{NKEY_LEFT,0},{NKEY_ENTER,0},{'z',NMOD_CTRL}};
        for(unsigned i=0;editor && i<sizeof keys/sizeof *keys;i++) {
            struct gui_event key={.key=keys[i].key,.mods=keys[i].mods};
            test_check("native-editing-key-handled",web_key(fixture.doc,&key)==1);
            struct web_event probe={.type="editing-probe"};web_dispatch(fixture.doc,editor,&probe);
            char mark[48];snprintf(mark,sizeof mark,"native-editing-step-%u",i+1);
            test_check("native-editing-step",has_mark(&fixture,mark));
        }
        static const char *const names[]={"native-editing-type","native-editing-type-again","native-editing-backspace",
            "native-editing-left","native-editing-enter","native-editing-undo"};
        require_marks(names,sizeof names/sizeof *names);
        test_check("native-editing-no-errors",fixture.errors==0);
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

#include "js_frames_native.h"
#include "js_node_position_native.h"
static void test_html_parser(void){
    external_case("js_parser_custom_cases.js",";runParserCustomElementCases().then(n=>{console.log('Parser CE checks '+n);check('parser-ce-count',n>=60);mark('api-done');},e=>{console.log('FAIL parser-ce '+e+' '+e.stack);mark('api-done');});",BASE);
    external_case("js_html_safety_cases.js",";try{let n=runHTMLSafetyCases();console.log('HTML safety checks '+n);check('html-safety-count',n>=50);}catch(e){console.log('FAIL html-safety '+e+' '+e.stack);}mark('api-done');",BASE);
    external_case("js_pi_cases.js",";runProcessingInstructionCases().then(n=>{console.log('PI API checks '+n);check('pi-count',n>=30);mark('api-done');},e=>{console.log('FAIL PI '+e+' '+e.stack);mark('api-done');});",BASE);
    case_response_headers="Content-Security-Policy: require-trusted-types-for 'script'; trusted-types script-snapshot-test default\r\n";
    external_case_expected("js_html_script_safety_cases.js",";try{check('script-safety-count',runHTMLScriptSafetyCases()===10);}catch(e){console.log('FAIL script safety '+e+' '+e.stack);}mark('api-done');",BASE,3);
    external_case_expected("js_html_script_safety_cases.js",";runHTMLScriptGenerationCases().then(n=>{check('script-generation-count',n===6);mark('api-done');},e=>{console.log('FAIL script generation '+e+' '+e.stack);mark('api-done');});",BASE,0);
    external_case_expected("js_html_script_safety_cases.js",";runHTMLStreamEventCases().then(n=>{check('stream-event-count',n===6);mark('api-done');},e=>{console.log('FAIL stream events '+e+' '+e.stack);mark('api-done');});",BASE,0);
    case_response_headers=NULL;
    const char *source=START "</head><body><script>"
        "globalThis.order=[];document.write('<p id=written>now</p>');"
        "check('write-visible-immediately',document.getElementById('written').textContent==='now');"
        "check('network-suffix-not-consumed',document.getElementById('future')===null);"
        "document.write('<scr'+'ipt>order.push(1);document.write(\"<b id=nested>nested</b>\");</scr'+'ipt>');"
        "check('nested-inline-synchronous',order.join(',')==='1'&&document.getElementById('nested')!==null);"
        "order.push(2);</script><p id=future>end</p><svg><script>order.push(3);</script></svg>"
        "<script>check('svg-script-parser-order',order.join(',')==='1,2,3');mark('parser-done');</script></body>";
    if(open_case(source,false)){
        test_check("parser-write-and-svg",pump("parser-done",5000));
        test_check("parser-write-no-exceptions",fixture.errors==0 && fixture.js_failures==0);
        close_case();
    }
    const char *timing=START "<script>globalThis.ceLog=[];class ParserTiming extends HTMLElement{"
        "static observedAttributes=['data-token'];constructor(){super();ceLog.push('ctor');"
        "check('ce-parser-before-attrs',this.attributes.length===0);check('ce-parser-before-children',this.childNodes.length===0);"
        "check('ce-parser-before-connection',!this.isConnected);check('ce-parser-before-future',document.getElementById('ce-future')===null);"
        "let blocked=false;try{document.write('<p>forbidden</p>');}catch(e){blocked=e.name==='InvalidStateError';}check('ce-parser-write-guard',blocked);}"
        "attributeChangedCallback(n,o,v){ceLog.push('attr');check('ce-token-attr-reaction',n==='data-token'&&o===null&&v==='yes');}"
        "connectedCallback(){ceLog.push('connected');check('ce-parser-connected-before-children',this.childNodes.length===0);}}"
        "customElements.define('nocturne-parser-timing',ParserTiming);</script></head><body>"
        "<nocturne-parser-timing data-token=yes><i>child</i></nocturne-parser-timing><p id=ce-future>later</p><script>"
        "check('ce-token-reaction-order',ceLog.join(',')==='ctor,attr,connected');"
        "check('ce-parser-child-published',document.querySelector('nocturne-parser-timing').firstChild.localName==='i');mark('ce-timing-done');</script></body>";
    if(open_case(timing,false)){test_check("parser-ce-timing",pump("ce-timing-done",5000));test_check("parser-ce-timing-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}
    char *stream=script_page("const originalDoc=document;globalThis.openPreserved=42;addEventListener('load',()=>{document.open();"
        "check('open-top-level-identity',document===originalDoc&&openPreserved===42);document.write('<p id=stream-now>now</p>');"
        "check('open-write-immediate',document.getElementById('stream-now').textContent==='now');document.close();mark('stream-done');},{once:true});");
    if(stream&&open_case(stream,false)){test_check("top-level-open-stream",pump("stream-done",5000));test_check("top-level-stream-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}free(stream);
    char *fragment=script_page(
        "globalThis.dynamicOrder=[];globalThis.fragmentInline=0;const r=document.createRange();r.selectNodeContents(document.body);"
        "const f=r.createContextualFragment('<script async defer src=order-one.js></script><script async defer src=order-two.js></script>"
        "<script async defer>fragmentInline++;</script>');f.querySelectorAll('script')[1].onload=()=>{"
        "check('fragment-attributes-ignore-order',dynamicOrder.join(',')==='1,2');mark('fragment-order-done');};document.body.appendChild(f);"
        "check('fragment-inline-insertion',fragmentInline===1);const unfinished=r.createContextualFragment('<script>fragmentInline=99;');"
        "document.body.appendChild(unfinished);check('fragment-eof-script-inert',fragmentInline===1);"
        "document.body.appendChild(r.createContextualFragment('<svg><script>fragmentInline++;</script></svg>'));check('fragment-svg-script-insertion',fragmentInline===2);"
        "document.body.appendChild(r.createContextualFragment('<svg><script>fragmentInline=98;'));check('fragment-svg-eof-inert',fragmentInline===2);");
    if(fragment&&open_case(fragment,false)){test_check("fragment-script-order",pump("fragment-order-done",5000));test_check("fragment-script-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}free(fragment);
    char *unsafe=script_page(
        "globalThis.dynamicOrder=[];globalThis.unsafeInline=0;const host=document.createElement('div');document.body.appendChild(host);"
        "host.setHTMLUnsafe('<script async defer src=order-one.js></script><script async defer src=order-two.js></script>"
        "<script>unsafeInline++;</script>',{runScripts:true});check('unsafe-inline-insertion',unsafeInline===1);"
        "host.querySelectorAll('script')[1].onload=()=>{check('unsafe-fragment-external-order',dynamicOrder.join(',')==='1,2');mark('unsafe-order-done');};");
    if(unsafe&&open_case(unsafe,false)){test_check("unsafe-fragment-script-order",pump("unsafe-order-done",5000));test_check("unsafe-fragment-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}free(unsafe);
    char *written=script_page(
        "globalThis.writeSheetRan=false;document.getElementById('target').removeAttribute('style');document.write('<link rel=stylesheet href=style.css><script>writeSheetRan=true;"
        "check(\"write-script-sees-sheet\",getComputedStyle(document.getElementById(\"target\")).height===\"7px\");</script>');"
        "check('write-script-waits-for-sheet',!writeSheetRan);setTimeout(()=>{check('write-sheet-script-resumes',writeSheetRan);mark('write-sheet-done');},100);");
    if(written&&open_case(written,false)){test_check("write-stylesheet-wait",pump("write-sheet-done",5000));test_check("write-stylesheet-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}free(written);
    char *replaced=script_page(
        "globalThis.dynamicOrder=[];const retained=document.getElementById('target'),old=document.createElement('script');old.src='order-one.js';document.head.appendChild(old);"
        "setTimeout(()=>{const same=document;document.open();document.write('<!doctype html><p id=new-stream>replacement</p>');document.close();"
        "check('stream-document-and-node-retained',document===same&&retained.ownerDocument===same&&!retained.isConnected);"
        "setTimeout(()=>{check('stream-old-resource-cancelled',dynamicOrder.length===0&&!!document.getElementById('new-stream'));mark('stream-resource-done');},100);},0);");
    if(replaced&&open_case(replaced,false)){test_check("stream-resource-generation",pump("stream-resource-done",5000));test_check("stream-resource-no-errors",fixture.errors==0&&fixture.js_failures==0);close_case();}free(replaced);
}
static void test_network_stream(void){
    FILE *ports=fopen("C:\\Tests\\webports","r");unsigned first=0,second=0;char text[96],origin[128];
    test_check("stream-fixture-ports",ports&&fgets(text,sizeof text,ports)&&sscanf(text,"%u %u",&first,&second)==2&&first);
    if(ports)fclose(ports);if(!first)return;
    snprintf(origin,sizeof origin,"http://10.0.2.2:%u/",first);
    case_real_network=true;
    external_case("js_network_stream_cases.js",";runNetworkStreamCases().then(()=>mark('api-done'),e=>{check('stream-unexpected-error',false);console.log(String(e)+' '+e.stack);mark('api-done');});",origin);
    case_real_network=false;
}
static void test_html_worker(void){
    FILE *ports=fopen("C:\\Tests\\webports","r");unsigned first=0,second=0;char port_text[96];
    test_check("worker-fixture-ports",ports && fgets(port_text,sizeof port_text,ports) && sscanf(port_text,"%u %u",&first,&second)==2 && first);
    if(ports)fclose(ports);if(!first)return;
    char origin[128],tail[1600];snprintf(origin,sizeof origin,"http://10.0.2.2:%u/",first);
    snprintf(tail,sizeof tail,";runParserWorkerSafetyCases({plainURL:'%sworkers/parser-safety-plain.js',enforceURL:'%sworkers/parser-safety-enforce.js',reportURL:'%sworkers/parser-safety-report.js',importURL:'%sworkers/parser-safety-import.js'}).then(n=>{check('worker-safety-count',n>=100);mark('api-done');},e=>{console.log('FAIL Worker safety '+e+' '+e.stack);mark('api-done');});",origin,origin,origin,origin);
    case_real_network=true;external_case("js_parser_worker_safety_cases.js",tail,origin);case_real_network=false;
}
static void test_html_csp(void){
    FILE *ports=fopen("C:\\Tests\\webports","r");unsigned first=0,second=0;char port_text[96],origin[128];
    test_check("csp-fixture-ports",ports&&fgets(port_text,sizeof port_text,ports)&&sscanf(port_text,"%u %u",&first,&second)==2&&first);
    if(ports)fclose(ports);if(!first)return;snprintf(origin,sizeof origin,"http://10.0.2.2:%u/dir/index.html",first);
    const char *headers[]={
        "Content-Security-Policy: require-trusted-types-for 'script'; trusted-types allowed default duplicate 'allow-duplicates'; report-uri /__csp-report\r\nContent-Security-Policy-Report-Only: require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-report-only\r\n",
        "Content-Security-Policy-Report-Only: require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-report-only\r\n",NULL};
    const char *modes[]={"enforce","report","meta"};
    for(unsigned i=0;i<3;i++){
        case_response_headers=headers[i];case_meta_policy=i==2?"<meta http-equiv=Content-Security-Policy content=\"require-trusted-types-for 'script'; trusted-types allowed default duplicate; report-uri /__csp-meta-must-not-send\">":NULL;
        char tail[400];snprintf(tail,sizeof tail,";runHTMLCSPCases('%s').then(n=>{console.log('CSP API checks '+n);check('csp-count',n>=30);mark('api-done');},e=>{console.log('FAIL CSP '+e+' '+e.stack);mark('api-done');});",modes[i]);
        case_real_network=true;external_case("js_html_csp_cases.js",tail,origin);case_real_network=false;
        case_response_headers=case_meta_policy=NULL;
    }
}
static void test_modulepreload_api(void) {
    char *page=script_page(
        "const attach=(node)=>new Promise((resolve,reject)=>{node.onload=resolve;node.onerror=reject;document.head.appendChild(node);});"
        "const preload=(suffix)=>{const node=document.createElement('link');node.rel='modulepreload';node.href='preload.mjs'+suffix;return node;};"
        "const module=(suffix)=>{const node=document.createElement('script');node.type='module';node.src='preload.mjs'+suffix;return node;};"
        "(async()=>{check('modulepreload-native-token',document.createElement('link').relList.supports('modulepreload'));"
        "await Promise.all([attach(preload('')),attach(preload(''))]);check('preload-not-evaluated',globalThis.preloadEvaluations===undefined);"
        "await attach(module(''));await attach(module(''));check('preload-one-evaluation',preloadEvaluations===1);"
        "const changed=preload('');changed.rel='nofollow';const ready=attach(changed);changed.rel='modulepreload';await ready;"
        "check('preload-rel-mutation',preloadEvaluations===1);"
        "const failed=preload('?sri');failed.integrity='sha384-invalid';await new Promise((resolve,reject)=>{failed.onload=()=>reject(new Error('SRI unexpectedly loaded'));failed.onerror=resolve;document.head.appendChild(failed);});"
        "await attach(module('?sri'));check('preload-sri-failure-retry',preloadEvaluations===2);mark('preload-api-done');"
        "})().catch(e=>{check('preload-unexpected-error',false);console.log(String(e));mark('preload-api-done');});");
    if(page&&open_case(page,true)){
        test_check("preload-api-completed",pump("preload-api-done",6000));
        test_check("preload-api-no-assertion-failures",fixture.js_failures==0);
        test_check("preload-url-sharing-and-failed-retry",fixture.preload_requests==3);
        test_check("preload-completion-native-only",fixture.outside_running==0);
        close_case();
    }
    free(page);
}
static void test_modulepreload(void) {
    test_modulepreload_api();
    char *root_only=script_page(
        "const root=document.createElement('link');root.rel='modulepreload';root.href='preload-root.mjs';"
        "root.onload=()=>{check('preload-root-no-evaluation',globalThis.preloadEvaluations===undefined);mark('preload-root-only-done');};"
        "root.onerror=()=>{check('preload-root-error',false);mark('preload-root-only-done');};document.head.appendChild(root);");
    if(root_only&&open_case(root_only,false)){
        test_check("preload-root-parsed-without-graph",pump("preload-root-only-done",6000));
        test_check("preload-root-no-dependency-fetch",fixture.preload_requests==1&&fixture.sync_loads==0);
        test_check("preload-root-no-errors",fixture.errors==0&&fixture.js_failures==0);
        close_case();
    }
    free(root_only);
    const char *graph=START
        "<link rel=modulepreload href=preload-child.mjs><script type=module src=preload-root.mjs></script>"
        "</head><body><p>Module preload join</p></body></html>";
    if(open_case(graph,false)){
        test_check("preload-native-graph-completed",pump("preload-graph-done",6000));
        test_check("preload-graph-no-errors",fixture.errors==0&&fixture.js_failures==0);
        test_check("preload-graph-no-duplicate-fetch",fixture.preload_requests==2&&fixture.sync_loads==0);
        test_check("preload-graph-reused-without-native-join",fixture.preload_joins==0);
        test_check("preload-join-no-author-callback",fixture.outside_running==0);
        close_case();
    }
}
static void input_probe(web_node *target,const char *source,bool allowed,const char *name) {
    test_check("input-author-operation",web_console_eval(fixture.doc,source,strlen(source)));
    web_layout(fixture.doc,VW,VH);
    struct web_input_target captured={0};
    test_check(name,web_native_wheel_target(fixture.doc,target,&captured,false)==allowed);
}

static void test_input_snapshot(void) {
    case_input_checkpoint=true;
    const char *html=START "</head><body style='margin:0'><div id=wheel style='height:100px'>wheel</div>"
        "<script>globalThis.w=document.getElementById('wheel');globalThis.cancelWheel=e=>e.preventDefault();mark('input-ready');</script></body>";
    if(!open_case(html,false)){case_input_checkpoint=false;return;}
    test_check("input-initial-script",pump("input-ready",3000));
    web_layout(fixture.doc,VW,VH);
    web_node *target=web_node_at(fixture.doc,4,4);
    test_check("input-target",target!=NULL);
    input_probe(target,"void 0",true,"input-no-listener");
    input_probe(target,"w.addEventListener('wheel',cancelWheel,{passive:true})",true,"input-passive");
    input_probe(target,"w.addEventListener('wheel',cancelWheel,{passive:false})",true,"input-duplicate-keeps-passive");
    input_probe(target,"w.removeEventListener('wheel',cancelWheel);w.addEventListener('wheel',cancelWheel,{passive:false})",false,"input-nonpassive");
    struct web_event event={.type="wheel",.bubbles=true,.cancelable=true,.delta_y=30};
    test_check("input-author-prevent-default",!web_dispatch(fixture.doc,target,&event));
    input_probe(target,"w.removeEventListener('wheel',cancelWheel)",true,"input-remove");
    input_probe(target,"w.addEventListener('wheel',cancelWheel,{once:true,passive:false})",false,"input-once-before");
    test_check("input-once-cancels",!web_dispatch(fixture.doc,target,&event));
    input_probe(target,"void 0",true,"input-once-after");
    input_probe(target,"globalThis.wheelAbort=new AbortController();w.addEventListener('wheel',cancelWheel,{signal:wheelAbort.signal,passive:false})",false,"input-abort-before");
    input_probe(target,"wheelAbort.abort()",true,"input-abort-after");
    input_probe(target,"document.addEventListener('wheel',cancelWheel)",true,"input-document-default-passive");
    test_check("input-default-passive-cannot-cancel",web_dispatch(fixture.doc,target,&event));
    input_probe(target,"document.removeEventListener('wheel',cancelWheel);document.addEventListener('wheel',cancelWheel,{passive:false})",false,"input-document-explicit-nonpassive");
    input_probe(target,"document.removeEventListener('wheel',cancelWheel);document.body.addEventListener('wheel',cancelWheel);addEventListener('wheel',cancelWheel)",true,"input-body-window-default-passive");
    input_probe(target,"removeEventListener('wheel',cancelWheel);addEventListener('wheel',cancelWheel,{passive:false})",false,"input-window-explicit-nonpassive");
    input_probe(target,"removeEventListener('wheel',cancelWheel);document.body.removeEventListener('wheel',cancelWheel);w.onwheel=()=>false",false,"input-inline-property");
    test_check("input-inline-cancels",!web_dispatch(fixture.doc,target,&event));
    input_probe(target,"w.onwheel=null",true,"input-inline-property-remove");
    input_probe(target,"w.setAttribute('onwheel','return false')",false,"input-inline-attribute");
    input_probe(target,"w.removeAttribute('onwheel')",true,"input-inline-attribute-remove");
    input_ready_checkpoints=input_blocked_checkpoints=0;
    const char *loop="let inputSum=0;for(let i=0;i<1000000;i++)inputSum+=i;";
    test_check("input-pure-js-loop",web_console_eval(fixture.doc,loop,strlen(loop)));
    test_check("input-pure-js-committed-snapshot",input_ready_checkpoints>0);
    struct web_input_target old={0};test_check("input-capture-owner",web_input_capture(fixture.doc,target,&old));
    test_check("input-current-owner",web_input_target_valid(fixture.doc,&old));
    const char *replace="document.open();document.write('<p>replacement</p>');document.close();";
    test_check("input-open",web_console_eval(fixture.doc,replace,strlen(replace)));
    test_check("input-open-invalidates-old-epoch",!web_input_target_valid(fixture.doc,&old));
    test_check("input-no-errors",fixture.errors==0&&fixture.js_failures==0);
    close_case();case_input_checkpoint=false;
}

int main(int argc, char **argv) {
    uint64_t start = uptime_ms();
    /* Optional selectors make a failing feature reproducible in the real OS. */
#define RUN(name, fn) do { if (argc == 1 || !strcmp(argv[1], name)) { printf("jstest: %s\n", name); fflush(stdout); fn(); } } while (0)
    RUN("language", test_language);
    if(argc==1 || !strcmp(argv[1],"wasm")){
        printf("jstest: wasm\n");fflush(stdout);
        external_case("js_wasm_cases.js",";runWasmCases().then(n=>{console.log('WebAssembly checks '+n);mark('api-done');},e=>{check('wasm-unexpected-error',false);console.log('FAIL wasm '+e+' '+e.stack);mark('api-done');});",BASE);
    }
    if(argc>1 && !strcmp(argv[1],"wasm-boundary")){
        printf("jstest: wasm-boundary\n");fflush(stdout);
        external_case("js_wasm_cases.js",";runWasmNativeBoundaryCases().then(n=>{console.log('WebAssembly boundary checks '+n);mark('api-done');},e=>{check('wasm-boundary-unexpected-error',false);console.log('FAIL wasm-boundary '+e+' '+e.stack);mark('api-done');});",BASE);
    }
    if(argc>1 && !strcmp(argv[1],"wasm-jspi")){
        printf("jstest: wasm-jspi\n");fflush(stdout);
        external_case("js_wasm_cases.js",";runWasmJspiCases().then(n=>{console.log('WebAssembly JSPI checks '+n);mark('api-done');},e=>{check('wasm-jspi-unexpected-error',false);console.log('FAIL wasm-jspi '+e+' '+e.stack);mark('api-done');});",BASE);
    }
    if(argc>1 && !strcmp(argv[1],"wasm-worker")){
        FILE *ports=fopen("C:\\Tests\\webports","r");unsigned first=0,second=0;char port_text[96],origin[128],tail[512];
        test_check("wasm-worker-fixture-ports",ports&&fgets(port_text,sizeof port_text,ports)&&sscanf(port_text,"%u %u",&first,&second)==2&&first);
        if(ports)fclose(ports);
        if(first){
            snprintf(origin,sizeof origin,"http://10.0.2.2:%u/",first);
            snprintf(tail,sizeof tail,";runWasmWorkerCases('%sworkers/wasm.js').then(n=>{console.log('WebAssembly worker checks '+n);mark('api-done');},e=>{check('wasm-worker-unexpected-error',false);console.log('FAIL wasm-worker '+e+' '+e.stack);mark('api-done');});",origin);
            case_real_network=true;external_case("js_wasm_cases.js",tail,origin);case_real_network=false;
        }
    }
    if(argc>1 && !strcmp(argv[1],"wasm-policy")){
        const char *policies[]={"Content-Security-Policy: script-src 'self'\r\n",
            "Content-Security-Policy: default-src 'none';script-src 'wasm-unsafe-eval'\r\n",
            "Content-Security-Policy-Report-Only: default-src 'none'\r\n",NULL};
        const char *tails[]={";runWasmPolicyCases(false,5)",";runWasmPolicyCases(true,0)",";runWasmPolicyCases(true,5)",";runWasmPolicyCases(false,5)"};
        const char suffix[]=".then(n=>{console.log('WebAssembly policy checks '+n);mark('api-done');},e=>{check('wasm-policy-error',false);console.log('FAIL wasm-policy '+e+' '+e.stack);mark('api-done');});";
        for(size_t i=0;i<sizeof policies/sizeof *policies;i++){
            char tail[sizeof ";runWasmPolicyCases(false,5)"+sizeof suffix];snprintf(tail,sizeof tail,"%s%s",tails[i],suffix);
            case_response_headers=policies[i];
            case_meta_policy=policies[i]?NULL:"<meta http-equiv=Content-Security-Policy content=\"script-src 'none'\">";
            external_case("js_wasm_cases.js",tail,BASE);case_response_headers=case_meta_policy=NULL;
        }
    }
    if(argc>1 && !strcmp(argv[1],"wasm-worker-policy")){
        FILE *ports=fopen("C:\\Tests\\webports","r");unsigned first=0,second=0;
        char port_text[sizeof "65535 65535\n"],origin[sizeof "http://10.0.2.2:65535/"];
        const char suffix[]="').then(n=>{console.log('WebAssembly worker policy checks '+n);mark('api-done');},e=>{check('wasm-worker-policy-error',false);console.log('FAIL wasm-worker-policy '+e+' '+e.stack);mark('api-done');});";
        char tail[sizeof ";runWasmWorkerPolicyCases('"+sizeof origin+sizeof suffix];
        test_check("wasm-policy-worker-ports",ports&&fgets(port_text,sizeof port_text,ports)&&sscanf(port_text,"%u %u",&first,&second)==2&&first);
        if(ports)fclose(ports);
        if(first){
            snprintf(origin,sizeof origin,"http://10.0.2.2:%u/",first);
            snprintf(tail,sizeof tail,";runWasmWorkerPolicyCases('%s%s",origin,suffix);
            case_real_network=true;external_case("js_wasm_cases.js",tail,origin);case_real_network=false;
        }
    }
    if(argc>1&&!strcmp(argv[1],"input-snapshot")){
        printf("jstest: input-snapshot\n");fflush(stdout);test_input_snapshot();
    }
    RUN("modulepreload",test_modulepreload);
    if(argc>1&&!strcmp(argv[1],"modulepreload-api")){
        printf("jstest: modulepreload-api\n");fflush(stdout);
        test_modulepreload_api();
    }
    if(argc>1&&!strcmp(argv[1],"intersection-inline")){
        printf("jstest: intersection-inline\n");fflush(stdout);
        external_case("js_observer_cases.js",";runIntersectionInlineCases().then(n=>{check('intersection-inline-count',n===10);mark('api-done');},e=>{console.log('FAIL intersection-inline '+e);mark('api-done');});",BASE);
    }
    if(argc>1&&!strcmp(argv[1],"intersection-entry")){
        printf("jstest: intersection-entry\n");fflush(stdout);
        external_case("js_observer_cases.js",";runIntersectionEntryCases().then(n=>{check('intersection-entry-count',n===10);mark('api-done');},e=>{console.log('FAIL intersection-entry '+e);mark('api-done');});",BASE);
    }
    if(argc>1&&!strcmp(argv[1],"window-named")){
        printf("jstest: window-named\n");fflush(stdout);
        external_case("js_frames_cases.js",";try{check('window-named-count',runWindowNamedCases()===42);}catch(e){console.log('FAIL window-named '+e);}mark('api-done');",BASE);
    }
    RUN("html-parser", test_html_parser);
    RUN("html-worker", test_html_worker);
    RUN("html-csp", test_html_csp);
    RUN("network-stream", test_network_stream);
    RUN("platform", test_platform);
    /* An API-only selector avoids repeating the full platform batch. */
    if (argc > 1 && !strcmp(argv[1], "html-interfaces")) {
        printf("jstest: html-interfaces\n"); fflush(stdout);
        external_case("js_html_interface_cases.js", ";try{const n=runHTMLInterfaceCases();console.log('HTML interface API checks '+n);check('html-interfaces-count',n>500);}catch(e){console.log('FAIL html-interfaces '+e+' '+e.stack);}mark('api-done');", BASE);
    }
    if (argc > 1 && !strcmp(argv[1], "fetch-api")) { printf("jstest: fetch-api\n"); fflush(stdout); test_fetch_api(); }
    if (argc > 1 && !strcmp(argv[1], "dom-api")) {
        printf("jstest: dom-api\n"); fflush(stdout);
        external_case("js_event_handler_cases.js", ";check('event-handler-count',runEventHandlerCases()===18);mark('api-done');", BASE);
        external_case("js_observer_cases.js", ";runObserverCases().then(n=>{check('observer-count',n>=40);mark('api-done');},e=>{console.log('FAIL observer '+e);mark('api-done');});", BASE);
        external_case("js_dom_runtime_cases.js", ";runDOMRuntimeCases().then(n=>{check('dom-runtime-count',n>=40);mark('api-done');},e=>{console.log('FAIL dom-runtime '+e);mark('api-done');});", BASE);
        external_case("js_custom_elements_cases.js", ";runCustomElementCases().then(n=>{check('custom-elements-count',n>30);mark('api-done');},e=>{console.log('FAIL custom-elements '+e);mark('api-done');});", BASE);
    }
    RUN("broadcast", test_broadcast);
    RUN("retired-jobs", test_retired_jobs);
    RUN("retired-tasks", test_retired_tasks);
    RUN("error-event", test_error_event);
    RUN("history-receiver", test_history_receiver);
    RUN("inner-text", test_inner_text);
    RUN("collections", test_collections);
    RUN("adjacent", test_adjacent);
    RUN("fragment-constructors", test_fragment_constructor);
    RUN("resource-structure", test_resource_structure);
    RUN("detached", test_detached_invalidation);
    RUN("image-invalidation", test_image_invalidation);
    RUN("frames", test_frames);
    RUN("node-position", test_node_position);
    RUN("cssom", test_cssom);
    RUN("computed-style-declaration", test_computed_declaration);
    /* Already included in platform above; selecting lexbor runs only its probes. */
    if (argc > 1 && !strcmp(argv[1], "lexbor")) { printf("jstest: lexbor\n"); fflush(stdout); test_lexbor(); }
    RUN("xhr", test_xhr);
    RUN("documents", test_documents);
    RUN("xml-serialization", test_xml_serialization);
    RUN("dom", test_dom_and_scripts);
    RUN("mime", test_script_mime);
    RUN("failed-module-graph", test_failed_module_graph);
    RUN("events", test_events_and_forms);
    RUN("selection-native", test_native_selection);
    RUN("editing", test_editing);
    RUN("timers", test_timers_and_promises);
    RUN("microtasks", test_host_microtasks);
    RUN("fetch", test_fetch_and_cancel);
    RUN("limits", test_limits);
    RUN("scripting", test_scripting_presentation);
    RUN("lifetime", test_lifetime);
#undef RUN
    test_check("at-least-one-case", total != 0);
    close_case();free_marks(&fixture);
    printf("jstest: %d checks, %d failed (%lu ms)\n", total, failed, (unsigned long)(uptime_ms()-start));
    return failed != 0;
}
