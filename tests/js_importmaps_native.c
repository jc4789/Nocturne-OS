/* Supplemental native web_live module/import-map checks. No HTTP policy mock
   can prove network CORS: webnettest remains the network-policy boundary. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main

static const struct asset map_assets[] = {
    {"/map/shared.mjs", "globalThis.__mapEvaluations=(globalThis.__mapEvaluations||0)+1;export const value='shared';", 0},
    {"/map/scoped.mjs", "export const value='scoped';", 0},
    {"/map/specific.mjs", "export const value='specific';", 0},
    {"/map/wrong.mjs", "throw Error('must not load wrong import-map target');", 0},
    {"/map/a.mjs", "import {b} from 'pkg/b.mjs';export function a(){return 'A';}export function cycle(){return a()+b();}", 0},
    {"/map/b.mjs", "import {a} from 'pkg/a.mjs';export function b(){return a()==='A'?'B':'?';}", 0},
    {"/map/sub/main.mjs", "import {value as f} from 'flavor';import {value as d} from 'fallback';import {value as e} from 'exactScope';import {cycle as c} from 'pkg/a.mjs';"
        "export const flavor=f,fallback=d,exact=e,cycle=c,meta=import.meta.url,current=document.currentScript;", 0},
    {"/map/null-scope.mjs", "export async function blocked(){try{await import('scopeBlock');return 'loaded';}catch(e){return e.name;}}"
        "export async function fallback(){return (await import('fallback')).value;}", 0},
    {"/map/late.mjs", "import {value} from 'pkg/long/flavor.mjs';export const flavor=value;", 0},
    {"/map/sub/flavor.mjs", "import {value} from 'flavor';export {value};", 0},
    {"/map/snapshot/value.mjs", "export const value='snapshot';", 0},
    {"/redirect/start.mjs", "import {value} from './dep.mjs';export {value};export const meta=import.meta.url;", 0},
    {"/redirect/final/dep.mjs", "export const value='redirect';", 0},
    {"/map/mime.mjs", "export const value='must not run';", 0},
    {"/map/cors.mjs", "export const value='must not run';", 0},
};
static bool map_sync_load(void *opaque, const char *url, int kind, struct web_response *response) {
    const struct asset *asset = NULL;
    const char *path = strstr(url, "://"); path = path ? strchr(path + 3, '/') : url;
    for (size_t i = 0; path && i < sizeof map_assets / sizeof *map_assets; i++)
        if (!strcmp(path, map_assets[i].path)) { asset = &map_assets[i]; break; }
    if (!asset) return sync_load(opaque, url, kind, response);
    struct fixture *f = opaque; f->sync_loads++;
    if (!f->doc || !web_script_running(f->doc) || kind != WEB_RESOURCE_MODULE) f->outside_running++;
    memset(response, 0, sizeof *response); response->status = 200;
    snprintf(response->url, sizeof response->url, "%s", !strcmp(path, "/redirect/start.mjs") ? "http://fixture.test/redirect/final/main.mjs" : url);
    snprintf(response->headers, sizeof response->headers, "Content-Type: %s\r\n", !strcmp(path, "/map/mime.mjs") ? "text/plain" : "text/javascript");
    if (!strcmp(path, "/map/cors.mjs")) snprintf(response->error, sizeof response->error, "Fixture host refused module CORS");
    response->body = strdup(asset->body); response->body_len = strlen(response->body ? response->body : "");
    return response->body != NULL;
}
static bool map_request(void *opaque, const struct web_request *r) {
    if (!request(opaque, r)) return false;
    /* Make readiness inversion explicit, even on a slow native/QEMU runner. */
    struct fixture *f = opaque;
    if (!strcmp(r->url, "http://fixture.test/dir/order-one.js"))
        for (int i=0;i<SLOTS;i++) if (f->queue[i].used && f->queue[i].id==r->id)
            f->queue[i].due=uptime_ms()+1000;
    return true;
}
static bool map_open(const char *html, bool expected_errors) {
    memset(&fixture, 0, sizeof fixture); fixture.expected_errors = expected_errors;
    struct web_host host = {.opaque=&fixture, .request=map_request, .cancel=cancel, .sync_load=map_sync_load, .console=receive_log};
    fixture.doc = web_live(html, strlen(html), BASE, "utf-8", &host);
    test_check("import-map-document", fixture.doc != NULL); return fixture.doc != NULL;
}
static void source_case(const char *html, const char *marker, int errors) {
    if (!map_open(html, errors != 0)) return;
    test_check(marker, pump(marker, 15000)); test_check("import-map-idle", pump(NULL, 5000));
    test_check("import-map-expected-errors", fixture.errors == errors && fixture.js_failures == 0);
    test_check("import-map-host-boundary", fixture.outside_running == 0); close_case();
}
static void regression(const char *file) {
    FILE *fp = fopen(file, "rb"); test_check("import-map-case-source", fp != NULL); if (!fp) return;
    fseek(fp, 0, SEEK_END); long len = ftell(fp); rewind(fp);
    if (len < 0 || len > 512 * 1024) { fclose(fp); test_check("import-map-case-size", false); return; }
    const char *tail = "runImportMapCases().then(n=>{check('import-map-cases-count',n>=38);mark('import-map-cases-done');},e=>{console.error(e);mark('import-map-cases-done');});";
    char *text = malloc((size_t)len + strlen(tail) + 1);
    test_check("import-map-case-allocation", text != NULL); if (!text) { fclose(fp); return; }
    size_t read = fread(text, 1, (size_t)len, fp); fclose(fp); text[read] = 0; strcat(text, tail);
    char *page = script_page(text); free(text); test_check("import-map-page-allocation", page != NULL);
    if (page) source_case(page, "import-map-cases-done", 0); free(page);
}
int main(int argc, char **argv) {
    regression(argc > 1 ? argv[1] : "/data/tests/js_importmaps_cases.js");
    source_case(START "<script id='parser-plain' type='application/json'>{}</script>"
        "<script id='parser-async' type='application/json' async>{}</script><script>"
        "(()=>{const s=document.createElement('script');"
        "check('async-created-force-default',s.async===true&&!s.hasAttribute('async'));"
        "s.removeAttribute('async');check('async-remove-absent-preserves-force',s.async===true);"
        "s.async=false;check('async-idl-false-clears-force',s.async===false&&!s.hasAttribute('async'));"
        "s.setAttribute('ASYNC','false');check('async-attribute-presence-after-idl',s.async===true);"
        "s.removeAttribute('async');check('async-remove-after-idl',s.async===false);"
        "s.async=true;check('async-idl-true-reflects',s.async===true&&s.getAttribute('async')==='');"
        "s.removeAttribute('async');check('async-idl-true-not-force',s.async===false);"
        "const t=document.createElement('script');t.setAttribute('async','');t.removeAttribute('async');"
        "check('async-add-attribute-clears-force',t.async===false);"
        "check('async-parser-default',document.getElementById('parser-plain').async===false);"
        "const p=document.getElementById('parser-async');check('async-parser-attribute',p.async===true);"
        "p.removeAttribute('async');check('async-parser-remove',p.async===false);mark('async-idl-done');})();"
        "</script></head><body></body>","async-idl-done",0);
    source_case(START "<script>globalThis.dynamicOrder=[];"
        "const one=document.createElement('script'),two=document.createElement('script');"
        "one.src='order-one.js';one.async=false;two.src='order-two.js';two.async=false;"
        "two.onload=()=>{check('async-preparation-ordered-snapshot',dynamicOrder.join(',')==='1,2');mark('async-ordered-done');};"
        "document.head.append(one,two);two.async=true;check('async-idl-after-preparation',two.async===true);"
        "</script></head><body></body>","async-ordered-done",0);
    source_case(START "<script>globalThis.dynamicOrder=[];"
        "const one=document.createElement('script'),two=document.createElement('script');"
        "one.src='order-one.js';one.async=false;two.src='order-two.js';two.async=false;two.setAttribute('async','');"
        "one.onload=()=>{check('async-attribute-preparation-snapshot',dynamicOrder.join(',')==='2,1');mark('async-attribute-done');};"
        "document.head.append(one,two);two.async=false;check('async-attribute-after-preparation',two.async===false);"
        "</script></head><body></body>","async-attribute-done",0);
    source_case(START "<script>globalThis.dynamicOrder=[];"
        "const one=document.createElement('script');one.src='order-one.js';one.async=false;document.head.append(one);"
        "const ordered=document.createElement('script');ordered.type='module';ordered.async=false;"
        "ordered.textContent=\"check('async-dynamic-module-ordered',dynamicOrder.join(',')==='free,1');mark('async-module-done');\";"
        "document.head.append(ordered);const free=document.createElement('script');free.type='module';"
        "free.textContent=\"dynamicOrder.push('free');\";document.head.append(free);"
        "</script></head><body></body>","async-module-done",0);
    source_case(START "<base href='/map/snapshot/'><script type='importmap'>{\"imports\":{\"snapshot\":\"./value.mjs\"}}</script>"
        "<script type='module'>import {value} from 'snapshot';import {value as relative} from './value.mjs';"
        "check('map-base-captured',value==='snapshot'&&relative==='snapshot');check('inline-meta-base',import.meta.url==='http://fixture.test/map/snapshot/');mark('snapshot-done');</script>"
        "<script>document.querySelector('base').href='/other/';</script></head><body></body>", "snapshot-done", 0);
    source_case(START "<script type='importmap'>{\"imports\":{\"./main.mjs\":\"./missing.mjs\"}}</script>"
        "<script type='module' src='main.mjs'></script></head><body><div id=future></div></body>", "module-top-level-await", 0);
    source_case(START "<script type='importmap'>{\"imports\":{\"redirect\":\"/redirect/start.mjs\"}}</script><script>"
        "Promise.all([import('redirect'),import('/redirect/start.mjs')]).then(([a,b])=>{check('redirect-module-identity',a===b);"
        "check('redirect-relative-base',a.value==='redirect');check('redirect-meta-url',a.meta==='http://fixture.test/redirect/final/main.mjs');mark('redirect-done');});"
        "</script></head><body></body>", "redirect-done", 0);
    source_case(START "<script type='module'>import {answer} from './dynamic.mjs';check('module-before-map',answer===42);mark('module-before-map-done');</script>"
        "<script type='importmap'>{\"imports\":{\"./dynamic.mjs\":\"/map/wrong.mjs\"}}</script></head><body></body>", "module-before-map-done", 0);
    source_case(START "<script type='importmap'>{\"imports\":{\"stable\":\"/map/shared.mjs\"}}</script>"
        "<script type='module'>import {value} from 'stable';check('atomic-map-original',value==='shared');mark('atomic-map-done');</script>"
        "<script type='importmap'>{\"imports\":{\"atomic\":\"/map/wrong.mjs\"},\"scopes\":{\"/bad/\":[]}}</script>"
        "<script>import('atomic').then(()=>check('invalid-map-not-partially-merged',false),e=>check('invalid-map-not-partially-merged',e instanceof TypeError));</script>"
        "</head><body></body>", "atomic-map-done", 1);
    static const char *const bad[] = {"{", "[]", "{\"imports\":[]}", "{\"scopes\":null}", "{\"scopes\":{\"/map/\":[]}}", "{\"integrity\":{\"/map/shared.mjs\":\"sha256-invalid\"}}"};
    for (size_t i=0;i<sizeof bad/sizeof *bad;i++) {
        char page[2048]; snprintf(page,sizeof page,START "<script type='importmap'>%s</script><script>mark('invalid-map-survives');</script></head><body></body>",bad[i]);
        source_case(page,"invalid-map-survives",1);
    }
    source_case(START "<script type='importmap' src='never-fetch.json'>{}</script><script type='importmap' src=''>{}</script>"
        "<script>mark('external-map-survives');</script></head><body></body>","external-map-survives",2);
    test_check("external-import-maps-no-fetch", fixture.requests == 0);
    printf("js_importmaps_native: %d checks, %d failed\n",total,failed);return failed!=0;
}
