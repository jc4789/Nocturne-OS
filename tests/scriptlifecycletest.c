/* Product web_live parsing, script execution and lifecycle boundaries. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <string.h>

static int checks, failures, errors;
static bool done;
static uint64_t blocking_request, deferred_request, following_request, module_request;

static void console(void *unused, int level, const char *text) {
    if (!strncmp(text,"OK ",3)) { checks++; printf("%s\n",text); }
    else if (!strncmp(text,"FAIL ",5)) { checks++; failures++; printf("%s\n",text); }
    else if (!strcmp(text,"SCRIPT-LIFECYCLE-DONE")) done=true;
    else if (level>=2) { errors++; printf("ERROR %s\n",text); }
}
static bool request(void *unused, const struct web_request *r) {
    if (r->kind!=WEB_RESOURCE_SCRIPT && r->kind!=WEB_RESOURCE_MODULE) return false;
    if (!strcmp(r->url,"https://lifecycle.test/blocking.js")) blocking_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/deferred.js")) deferred_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/following.js")) following_request=r->id;
    else if (!strcmp(r->url,"https://lifecycle.test/load-await.js")) module_request=r->id;
    else return false;
    return true;
}
static const char blocking[]=
    "document.write('<meta id=blocking-written>');"
    "check('blocking-write-preserved',!!document.getElementById('blocking-written'));"
    "Promise.resolve().then(()=>{document.write('<meta id=blocking-microtask>');"
    "check('blocking-checkpoint-write-preserved',!!document.getElementById('blocking-microtask'));});";
static const char deferred[]=
    "{const original=document.getElementById('original'),script=document.currentScript;"
    "document.write('<p id=bad-defer>replacement</p>');"
    "check('defer-write-preserves-document',document.getElementById('original')===original&&!document.getElementById('bad-defer'));"
    "const inner=document.createElement('script');"
    "inner.textContent=\"document.write('<p id=bad-nested>replacement</p>');check('nested-write-guard',!!document.getElementById('original')&&!document.getElementById('bad-nested'));\";"
    "document.head.appendChild(inner);"
    "check('nested-restores-currentScript',document.currentScript===script);"
    "Promise.resolve().then(()=>{document.write('<p id=bad-microtask>replacement</p>');"
    "check('defer-checkpoint-write-guard',document.getElementById('original')===original&&!document.getElementById('bad-microtask'));"
    "check('defer-checkpoint-currentScript',document.currentScript===script);});}";
static const char following[]=
    "check('following-defer-runs-during-await',!moduleDone&&document.readyState==='interactive');following=true;";
static const char load_module[]=
    "check('external-module-starts-before-load',!windowLoaded);"
    "await new Promise(resolve=>window.addEventListener('load',resolve,{once:true}));"
    "check('await-window-load-resumes',windowLoaded&&domReady&&moduleDone&&moduleElementLoaded);"
    "console.log('SCRIPT-LIFECYCLE-DONE');";
static const char page[]=
    "<!doctype html><head><script>"
    "function check(name,pass){console.log((pass?'OK ':'FAIL ')+name);}"
    "let moduleDone=false,following=false,domReady=false,windowLoaded=false,moduleElementLoaded=false;"
    "document.addEventListener('DOMContentLoaded',()=>{check('dom-ready-before-await-completion',following&&!moduleDone);domReady=true;});"
    "window.addEventListener('load',()=>{check('window-load-does-not-await-module-evaluation',domReady&&moduleDone&&moduleElementLoaded);windowLoaded=true;});"
    "</script><script src='/blocking.js'></script>"
    "<script defer src='/deferred.js'></script>"
    "<script type='module'>"
    "document.write('<p id=bad-module>replacement</p>');"
    "check('module-initial-write-guard',!!document.getElementById('original')&&!document.getElementById('bad-module'));"
    "await new Promise(resolve=>document.addEventListener('DOMContentLoaded',resolve,{once:true}));"
    "check('await-dom-ready-resumes',domReady&&following);moduleDone=true;"
    "</script><script defer src='/following.js'></script>"
    "<script type='module' src='/load-await.js' onload=\"check('module-element-load-before-await-completion',!windowLoaded);moduleElementLoaded=true;\"></script>"
    "</head><body><p id='original'>original document</p></body>";
static void deliver(web_doc *doc, uint64_t *pending, const char *source, const char *url) {
    if (!*pending) return;
    struct web_response response={.status=200,.body=(char *)source,.body_len=strlen(source)};
    strcpy(response.headers,"Content-Type: text/javascript\r\n");
    strcpy(response.url,url);
    uint64_t id=*pending; *pending=0;
    web_resource_loaded(doc,id,&response);
}
int main(void) {
    struct web_host host={.request=request,.console=console,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://lifecycle.test/","utf-8",&host);
    if (!doc) return 1;
    uint64_t deadline=uptime_ms()+10000;
    while (!done && uptime_ms()<deadline) {
        deliver(doc,&blocking_request,blocking,"https://lifecycle.test/blocking.js");
        deliver(doc,&deferred_request,deferred,"https://lifecycle.test/deferred.js");
        deliver(doc,&following_request,following,"https://lifecycle.test/following.js");
        deliver(doc,&module_request,load_module,"https://lifecycle.test/load-await.js");
        web_tick(doc,uptime_ms()); msleep(1);
    }
    if (!done || checks!=15 || errors) {
        printf("FAIL script lifecycle completion done=%d checks=%d errors=%d\n",done,checks,errors);
        failures++;
    }
    web_free(doc);
    printf("scriptlifecycletest: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
