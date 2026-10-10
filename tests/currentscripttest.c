/* Real script-element execution and its stack-empty cleanup checkpoint. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <string.h>

static int checks,failures,errors;static bool done;
static uint64_t classic_request,defer_request;
static void log_line(void *unused,int level,const char *text){
    if(!strncmp(text,"OK ",3)){checks++;printf("%s\n",text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;printf("%s\n",text);}
    else if(!strcmp(text,"CURRENT-SCRIPT-DONE"))done=true;
    else if(level>=2){errors++;printf("ERROR %s\n",text);}
}
static bool request_script(void *unused,const struct web_request *request){
    if(request->kind!=WEB_RESOURCE_SCRIPT)return false;
    if(!strcmp(request->url,"https://current.test/classic.js")){classic_request=request->id;return true;}
    if(!strcmp(request->url,"https://current.test/defer.js")){defer_request=request->id;return true;}return false;
}
static const char classic[]=
    "{const script=document.currentScript;check('external-classic-element',script instanceof HTMLScriptElement&&script.id==='external');"
    "Promise.resolve().then(()=>{check('external-checkpoint-element',document.currentScript===script);"
    "chunkQueue.push(()=>{check('chunk-module-registration',document.currentScript===script);classicDone=true;});chunkQueue.pop()();});}";
static const char deferred[]=
    "{const script=document.currentScript;check('defer-classic-element',script instanceof HTMLScriptElement&&script.id==='deferred');"
    "Promise.resolve().then(()=>{check('defer-checkpoint-element',document.currentScript===script);deferDone=true;});}";
static const char page[]=
    "<!doctype html><head><script id='setup'>"
    "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}let classicDone=false,deferDone=false,moduleDone=false,timerDone=false,loadDone=false;const chunkQueue=[];"
    "window.addEventListener('load',()=>{check('window-load-null',document.currentScript===null);loadDone=true;});"
    "</script><script id='outer'>"
    "{const outer=document.currentScript;check('inline-classic-element',outer instanceof HTMLScriptElement&&outer.id==='outer');"
    "const inner=document.createElement('script');inner.id='inner';"
    "inner.textContent=\"check('nested-classic-element',document.currentScript.id==='inner');Promise.resolve().then(()=>check('nested-deferred-checkpoint',document.currentScript.id==='outer'));\";"
    "document.head.appendChild(inner);check('nested-restores-outer',document.currentScript===outer);"
    "Promise.resolve().then(()=>{check('inline-promise-element',document.currentScript===outer);Promise.resolve().then(()=>check('nested-promise-element',document.currentScript===outer));});"
    "queueMicrotask(()=>check('inline-queued-microtask-element',document.currentScript===outer));"
    "setTimeout(()=>{check('timer-null',document.currentScript===null);Promise.resolve().then(()=>{check('timer-checkpoint-null',document.currentScript===null);timerDone=true;});},0);"
    "}</script><script id='external' src='/classic.js' onload=\"check('script-load-null',document.currentScript===null)\"></script>"
    "<script id='deferred' src='/defer.js' defer></script>"
    "<script id='module' type='module'>check('module-entry-null',document.currentScript===null);Promise.resolve().then(()=>{check('module-checkpoint-null',document.currentScript===null);moduleDone=true;});</script>"
    "<script id='following'>{const script=document.currentScript;check('following-classic-element',script.id==='following');Promise.resolve().then(()=>check('following-checkpoint-element',document.currentScript===script));}"
    "function finish(){if(!classicDone||!deferDone||!moduleDone||!timerDone||!loadDone){setTimeout(finish,2);return;}check('finish-task-null',document.currentScript===null);console.log('CURRENT-SCRIPT-DONE');}setTimeout(finish,2);"
    "</script></head><body>currentScript checkpoint</body>";
static void deliver(web_doc *doc,uint64_t *id,const char *source,const char *url){
    if(!*id)return;struct web_response response={.status=200,.body=(char*)source,.body_len=strlen(source)};
    strcpy(response.headers,"Content-Type: text/javascript\r\n");strcpy(response.url,url);
    uint64_t request=*id;*id=0;web_resource_loaded(doc,request,&response);
}
int main(void){
    struct web_host host={.request=request_script,.console=log_line,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://current.test/","utf-8",&host);if(!doc)return 1;
    uint64_t deadline=uptime_ms()+10000;
    while(!done&&uptime_ms()<deadline){
        deliver(doc,&classic_request,classic,"https://current.test/classic.js");
        deliver(doc,&defer_request,deferred,"https://current.test/defer.js");
        web_tick(doc,uptime_ms());msleep(1);
    }
    if(!done||checks!=21||errors){printf("FAIL currentScript completion done=%d checks=%d errors=%d\n",done,checks,errors);failures++;}
    web_free(doc);printf("currentscripttest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
