/* Real web_live/QuickJS/native image-decoder ownership regressions.
 * Deterministic transport, not real-site acceptance. No image getters are
 * used to make the detached request or to promote the replacement before paint. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"

struct queued { uint64_t id, due; char url[2048]; bool used; };
static struct {
    web_doc *doc;
    struct queued pending[16];
    int checks, failures, errors, images, completions, phase, paint;
    int first, replacement, detached, broken, old, latest, clone;
    bool done;
} state;
static uint32_t pixels[64 * 64];
static const char red[] = "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='3'><rect width='2' height='3' fill='red'/></svg>";
static const char blue[] = "<svg xmlns='http://www.w3.org/2000/svg' width='5' height='7'><rect width='5' height='7' fill='blue'/></svg>";

static void check(bool ok, const char *name) {
    state.checks++;
    if (!ok) { state.failures++; printf("FAIL image ownership %s\n", name); }
}
static bool request(void *opaque, const struct web_request *r) {
    (void)opaque;
    if (r->kind != WEB_RESOURCE_IMAGE) { check(false, "unexpected resource kind"); return false; }
    state.images++;
    if (strstr(r->url, "parser-inert.svg")) check(false, "initial parser template fetched");
    if (strstr(r->url, "first.svg")) { state.first++; check(state.phase >= 1, "inert first requested before live insertion"); }
    else if (strstr(r->url, "replacement.svg")) state.replacement++;
    else if (strstr(r->url, "detached.svg")) { state.detached++; check(state.phase >= 3, "inert detached requested before adoption"); }
    else if (strstr(r->url, "broken.svg")) state.broken++;
    else if (strstr(r->url, "oldlate.svg")) state.old++;
    else if (strstr(r->url, "latest.svg")) state.latest++;
    else if (strstr(r->url, "clone.svg")) { state.clone++; check(state.phase >= 6, "inert clone requested before live insertion"); }
    else check(false, "unknown image URL");
    printf("IMAGE-OWN-REQUEST phase=%d %s\n", state.phase, r->url);
    for (int i=0; i<16; i++) if (!state.pending[i].used) {
        struct queued *q = &state.pending[i]; q->used=true; q->id=r->id;
        snprintf(q->url, sizeof q->url, "%s", r->url);
        q->due=uptime_ms()+(strstr(r->url,"oldlate.svg")?200:strstr(r->url,"replacement.svg")?40:2);
        return true;
    }
    check(false, "transport queue capacity"); return false;
}
static void cancel(void *opaque, uint64_t id) {
    (void)opaque;
    for (int i=0; i<16; i++) if (state.pending[i].used && state.pending[i].id==id) state.pending[i].used=false;
}
static void log_message(void *opaque, int level, const char *message) {
    (void)opaque;
    if (level==2) { state.errors++; printf("ERROR %s\n", message); }
    if (!strncmp(message,"FAIL ",5)) { state.failures++; printf("%s\n", message); }
    if (!strncmp(message,"OWN-CHECK ",10)) state.checks++;
    if (!strncmp(message,"OWN-PHASE ",10)) { state.phase=atoi(message+10); printf("%s\n",message); }
    if (!strcmp(message,"OWN-PAINT RED")) state.paint=1;
    if (!strcmp(message,"OWN-PAINT BLUE")) state.paint=2;
    if (!strcmp(message,"OWN-DONE")) state.done=true;
}
static void deliver(void) {
    for (int i=0; i<16; i++) if (state.pending[i].used && state.pending[i].due<=uptime_ms()) {
        struct queued q=state.pending[i]; state.pending[i].used=false;
        const char *body=strstr(q.url,"broken.svg")?"not an image":
            (strstr(q.url,"replacement.svg")||strstr(q.url,"latest.svg"))?blue:red;
        struct web_response r={0}; r.status=200;
        snprintf(r.url,sizeof r.url,"%s",q.url);
        snprintf(r.headers,sizeof r.headers,"Content-Type: image/svg+xml\r\n");
        r.body=strdup(body); r.body_len=strlen(body);
        web_resource_loaded(state.doc,q.id,&r);
        memset(r.body,'!',r.body_len); free(r.body); state.completions++;
    }
}
static const char html[] =
"<!doctype html><html><head><style>body{margin:0;font-size:0;line-height:0}img{display:block;width:8px;height:8px}</style></head><body>"
"<template id='parsed'><img src='http://image.fixture/parser-inert.svg'></template><script>"
"const wait=ms=>new Promise(r=>setTimeout(r,ms));"
"function ck(v,n){console.log(v?'OWN-CHECK '+n:'FAIL '+n)}"
"function phase(n){console.log('OWN-PHASE '+n)}"
"function watch(img){const c={load:0,error:0,trusted:true,target:true};img.onload=e=>{c.load++;c.trusted=c.trusted&&e.isTrusted;c.target=c.target&&e.target===img};img.onerror=e=>{c.error++;c.trusted=c.trusted&&e.isTrusted;c.target=c.target&&e.target===img};return c}"
"function inert(src){const t=document.createElement('template');t.innerHTML='<img src=\"http://image.fixture/'+src+'\">';return {t,img:t.content.firstChild}}"
"(async()=>{"
"const parsed=document.getElementById('parsed').content.firstChild, pc=watch(parsed);"
"const first=inert('first.svg'), img=first.img, events=watch(img);"
"ck(img.ownerDocument!==document,'dynamic image inert owner');"
"await wait(100);ck(pc.load===0&&pc.error===0&&events.load===0&&events.error===0,'inert asynchronous no load/error');"
"phase(1);document.body.appendChild(img);await wait(100);"
"ck(img.ownerDocument===document&&events.load===1&&events.error===0&&events.trusted&&events.target,'inert to live insertion load');"
"console.log('OWN-PAINT RED');await wait(15);"
"phase(2);img.src='http://image.fixture/replacement.svg';await wait(120);"
"ck(events.load===2&&events.error===0,'replacement load exactly once');"
"console.log('OWN-PAINT BLUE');await wait(15);"
"ck(img.naturalWidth===5&&img.naturalHeight===7&&img.currentSrc.indexOf('replacement.svg')>=0,'replacement dimensions after paint');img.remove();"
"const detached=inert('detached.svg'), dc=watch(detached.img);await wait(40);"
"ck(dc.load===0&&dc.error===0,'detached initially inert');phase(3);document.adoptNode(detached.img);await wait(100);"
"ck(dc.load===1&&dc.error===0&&dc.trusted&&dc.target&&!detached.img.isConnected&&detached.img.ownerDocument===document,'detached adopt request and trusted load');"
"const broken=inert('broken.svg'), bc=watch(broken.img);phase(4);document.body.appendChild(broken.img);await wait(100);"
"ck(bc.error===1&&bc.load===0&&bc.trusted&&bc.target,'adopted broken error exactly once');broken.img.remove();"
"const late=inert('oldlate.svg'), lc=watch(late.img);phase(5);document.body.appendChild(late.img);await wait(15);late.img.src='http://image.fixture/latest.svg';await wait(260);"
"ck(lc.load===1&&lc.error===0,'stale response does not dispatch load');"
"console.log('OWN-PAINT BLUE');await wait(15);"
"ck(late.img.currentSrc.indexOf('latest.svg')>=0&&late.img.naturalWidth===5,'stale response does not replace current');late.img.remove();"
"const original=inert('clone.svg'), copy=original.t.cloneNode(true), ci=copy.content.firstChild, cc=watch(ci);"
"ck(ci.ownerDocument!==document,'deep clone image inert owner');await wait(40);ck(cc.load===0&&cc.error===0,'deep clone inert no events');"
"phase(6);document.body.appendChild(ci);await wait(100);ck(cc.load===1&&cc.error===0&&cc.trusted&&cc.target,'deep clone to live load');ci.remove();"
"await wait(40);ck(pc.load===0&&pc.error===0,'parser template remains inert');console.log('OWN-DONE');"
"})().catch(e=>{console.log('FAIL image ownership '+e+' '+e.stack);console.log('OWN-DONE')});"
"</script></body></html>";

int main(void) {
    struct web_host host={.request=request,.cancel=cancel,.console=log_message};
    state.doc=web_live(html,strlen(html),"http://image.fixture/index.html","utf-8",&host);
    if (!state.doc) { puts("FAIL image ownership allocation"); return 1; }
    uint64_t deadline=uptime_ms()+15000;
    while (!state.done && uptime_ms()<deadline) {
        deliver(); web_tick(state.doc,uptime_ms()); web_layout(state.doc,64,64);
        if (state.paint) {
            canvas_t canvas; gfx_init(&canvas,pixels,64,64,64);
            web_paint(state.doc,&canvas,0,0,64,64,0,0);
            int got=pixels[4*64+4]&0xFFFFFF, want=state.paint==1?0xFF0000:0x0000FF;
            check(got==want,"current image painted before property getter");
            printf("IMAGE-OWN-PAINT rgb=%06x expected=%06x\n",got,want); state.paint=0;
        }
        msleep(1);
    }
    check(state.done,"asynchronous cases completed");
    check(state.first==1&&state.replacement==1&&state.detached==1&&state.broken==1&&state.old==1&&state.latest==1&&state.clone==1,"unique resource counts");
    check(state.completions==7,"all responses including stale completed");
    web_free(state.doc);
    printf("imageownershiptest: %d checks, %d failures, %d unexpected errors, %d image requests, %d completions\n",
           state.checks,state.failures,state.errors,state.images,state.completions);
    return state.failures||state.errors;
}
