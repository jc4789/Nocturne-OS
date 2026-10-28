/* HTML Page visibility (interaction.html 6.2) in one live document with iframes: the host's
 * window_state and web_visibility_event, the queued "update the visibility state" tasks,
 * visibilitychange at each document, VisibilityStateEntry, a frame created while hidden, and
 * what a hidden document stops: animation frames, CSS animation samples and marquee steps (and
 * their deadlines), and its timers' wakeups (padded to whole seconds). The CSS animation's time
 * keeps running while hidden. Not a site acceptance test. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include "webi.h"
#include "frame.h"

static int checks,failures,errors;static bool ready,late_ready;
static unsigned window_bits=WEB_WINDOW_VISIBLE|WEB_WINDOW_FOCUSED;
static unsigned window_state(void *opaque){return window_bits;}
static void check(const char *name,bool pass){checks++;if(!pass)failures++;printf("%s %s\n",pass?"OK":"FAIL",name);}
static void log_line(void *opaque,int level,const char *text){
    if(!strcmp(text,"VISIBILITY-READY"))ready=true;
    else if(!strcmp(text,"LATE-READY"))late_ready=true;
    else if(!strncmp(text,"OK ",3)){checks++;puts(text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;puts(text);}
    else if(level==2){errors++;printf("ERROR %s\n",text);}
}
static void eval(web_doc *d,const char *script){
    if(!web_console_eval(d,script,strlen(script)))check("console eval",false);
}
static void layout(web_doc *d){if(web_dirty(d))web_layout(d,400,300);}
/* A host's loop until limit: sleep until the document's deadline, tick, lay out. Returns the
   number of times it woke before limit. stop, when given, ends the loop early once true. */
static int run_until(web_doc *d,uint64_t limit,const bool *stop){
    int wakes=0;n_handle queue=web_wait_queue();struct n_event events[32];
    while(!(stop&&*stop)){
        uint64_t now=uptime_ms();if(now>=limit)break;
        int64_t due=web_deadline(d);
        uint64_t until=due<0||(uint64_t)due>limit?limit:(uint64_t)due;
        int ms=until>now?(int)(until-now):0;
        n_result got=0;
        if(queue>=0)got=n_events_wait(queue,events,32,ms);else if(ms)n_sleep_ms(ms);
        if(got>0)web_wait_events(events,(size_t)got);
        if(got<=0&&uptime_ms()>=limit&&until==limit)break;
        wakes++;web_tick(d,uptime_ms());layout(d);
    }
    return wakes;
}
static node_t *by_id(web_doc *d,const char *name){
    char selector[64];snprintf(selector,sizeof selector,"#%s",name);
    pvec found={0};node_t *n=css_select(d,d->root,selector,&found)&&found.n?found.v[0]:NULL;
    pv_free(&found);return n;
}
static float opacity(node_t *n){return n&&n->style?n->style->opacity:-1;}
static bool all_frames(web_doc *d,bool visible,unsigned updates){
    int count=0;
    for(struct web_frame *f=d->frames;f;f=web_frame_walk_next(d,f,true)){
        if(!f->document||f->detached)continue;
        count++;if(f->document->visible!=visible||f->document->visibility_updates!=updates)return false;
    }
    return count>0;
}
static const char page[]=
    "<!doctype html><style>@keyframes fade{from{opacity:0}to{opacity:1}}#spin{animation:fade 100s linear infinite}</style>"
    "<body><div id=spin>animated</div><marquee id=moving scrolldelay=60 truespeed style='display:block;width:100px'>moving text</marquee>"
    "<iframe id=child srcdoc=\"<script>parent.childEvents=[];document.addEventListener('visibilitychange',()=>parent.childEvents.push(document.visibilityState));</script>\"></iframe>"
    "<script>"
    "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
    "let events=[],windowEvents=0,handlerEvents=0,rafs=0,rafHidden=0,timersHidden=0,late=null,observed=[],shapes=true;"
    /* The late frame sets these as Window properties (parent.lateState): var, not let, so the
       page's own reads see them. */
    "var lateState=null,lateEntry=null;"
    "function lateReady(){console.log('LATE-READY');}"
    "check('initial-visible',document.visibilityState==='visible'&&document.hidden===false);"
    "const first=performance.getEntriesByType('visibility-state');"
    "check('initial-entry',first.length===1&&first[0].name==='visible'&&first[0].startTime===0&&first[0].duration===0&&first[0] instanceof VisibilityStateEntry);"
    "new PerformanceObserver(list=>{for(const e of list.getEntries())observed.push(e.name);}).observe({type:'visibility-state'});"
    "document.onvisibilitychange=()=>handlerEvents++;"
    "addEventListener('visibilitychange',e=>{windowEvents++;if(e.eventPhase!==3||e.target!==document)shapes=false;});"
    "document.addEventListener('visibilitychange',e=>{"
    "events.push(document.visibilityState);"
    "if(!e.isTrusted||!e.bubbles||e.cancelable||e.target!==document||document.hidden!==(document.visibilityState==='hidden'))shapes=false;"
    "if(document.hidden&&!late){late=document.createElement('iframe');"
    "late.srcdoc='<script>parent.lateState=document.visibilityState;'+"
    "'parent.lateEntry=performance.getEntriesByType(\"visibility-state\").map(e=>e.name+\"@\"+e.startTime).join();parent.lateReady();<\\/script>';"
    "document.body.appendChild(late);}});"
    "(function frame(){rafs++;if(document.hidden)rafHidden++;requestAnimationFrame(frame);})();"
    "setInterval(()=>{if(document.hidden)timersHidden++;},10);"
    "addEventListener('load',()=>console.log('VISIBILITY-READY'));"
    "</script></body>";
int main(void){
    struct web_host host={.console=log_line,.window_state=window_state,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://page-visibility.test/","utf-8",&host);
    if(!doc){puts("FAIL pagevisibility live document");return 1;}
    web_layout(doc,400,300);
    run_until(doc,uptime_ms()+10000,&ready);
    check("page ready",ready);
    run_until(doc,uptime_ms()+200,NULL);
    node_t *spin=by_id(doc,"spin");
    web_doc *child=doc->frames?doc->frames->document:NULL;
    check("visible top and frame",doc->visible&&doc->system_visible&&child&&child->visible);
    check("visible CSS animation deadline",web_animations_deadline(doc)>=0);
    check("visible marquee deadline",web_marquee_deadline(doc)>=0);
    check("visible animation frames wanted",web_js_rendering_wanted(doc));

    /* The window is minimized (or the tab goes behind another): one queued task per document. */
    float before=opacity(spin);uint64_t hidden_at=uptime_ms();
    window_bits=0;web_visibility_event(doc);web_visibility_event(child);
    check("hide queues one task per document",doc->visibility_updates==1&&all_frames(doc,true,1)&&doc->visible&&!doc->system_visible);
    int64_t due=web_deadline(doc);
    check("queued task is due now",due>=0&&(uint64_t)due<=uptime_ms());
    eval(doc,"check('state kept until the task runs',document.visibilityState==='visible'&&events.length===0)");
    web_tick(doc,uptime_ms());layout(doc);
    check("hidden after the task",!doc->visible&&!doc->visibility_updates&&all_frames(doc,false,0));
    eval(doc,"check('hidden event',events.join()==='hidden'&&handlerEvents===1&&windowEvents===1&&shapes&&document.hidden);"
             "check('frame hidden event',childEvents.join()==='hidden'&&document.getElementById('child').contentDocument.visibilityState==='hidden')");
    check("hidden CSS animation has no deadline",web_animations_deadline(doc)==-1);
    check("hidden marquee has no deadline",web_marquee_deadline(doc)==-1);
    check("hidden animation frames not wanted",!web_js_rendering_wanted(doc));
    run_until(doc,uptime_ms()+5000,&late_ready);
    check("frame created while hidden loaded",late_ready);
    /* A frame's first deadlines (its first-render fallback) pass, then the hidden page's wakes. */
    run_until(doc,hidden_at+3500,NULL);
    int wakes=run_until(doc,uptime_ms()+2000,NULL);
    printf("pagevisibility hidden wakes in 2000 ms: %d\n",wakes);
    check("hidden page wakes at most a few times",wakes<=6);

    /* Shown again: the same tasks, and the animation where its time puts it. */
    window_bits=WEB_WINDOW_VISIBLE|WEB_WINDOW_FOCUSED;web_visibility_event(doc);
    web_tick(doc,uptime_ms());web_layout(doc,400,300);web_tick(doc,uptime_ms());web_layout(doc,400,300);
    float after=opacity(spin);uint64_t elapsed=uptime_ms()-hidden_at;
    check("visible again",doc->visible&&all_frames(doc,true,0));
    printf("pagevisibility opacity %.4f -> %.4f over %lu ms\n",before,after,(unsigned long)elapsed);
    check("CSS animation time ran while hidden",before>=0&&fabsf((after-before)-(float)elapsed/100000.0f)<0.01f);
    check("visible CSS animation deadline again",web_animations_deadline(doc)>=0);
    check("visible marquee deadline again",web_marquee_deadline(doc)>=0);
    eval(doc,"check('visible event',events.join()==='hidden,visible'&&handlerEvents===2&&windowEvents===2&&shapes&&!document.hidden);"
             "check('frame visible event',childEvents.join()==='hidden,visible');"
             "check('no animation frame while hidden',rafHidden===0);"
             "check('hidden timers padded, not stopped',timersHidden>=1&&timersHidden<=10);"
             "check('frame created while hidden starts hidden (state '+lateState+', entries '+lateEntry+')',lateState==='hidden'&&lateEntry==='hidden@0');"
             "const v=performance.getEntriesByType('visibility-state');"
             "check('visibility-state entries',v.map(e=>e.name).join()==='visible,hidden,visible'&&v[1].startTime>0&&v[2].startTime>v[1].startTime&&v[2].duration===0);"
             "window.rafsShown=rafs");
    run_until(doc,uptime_ms()+1500,NULL);
    eval(doc,"check('animation frames resume',rafs>=window.rafsShown+5);"
             "check('observer gets the entries',observed.join()==='hidden,visible')");

    /* Hidden and shown again before a task runs: both tasks run, in order. */
    window_bits=0;web_visibility_event(doc);window_bits=WEB_WINDOW_VISIBLE|WEB_WINDOW_FOCUSED;web_visibility_event(doc);
    check("two queued tasks",doc->visibility_updates==2&&doc->visible);
    web_tick(doc,uptime_ms());check("first task hides",!doc->visible&&doc->visibility_updates==1);
    web_tick(doc,uptime_ms());check("second task shows",doc->visible&&!doc->visibility_updates);
    run_until(doc,uptime_ms()+100,NULL);
    eval(doc,"check('both events fire',events.join()==='hidden,visible,hidden,visible'&&childEvents.join()==='hidden,visible,hidden,visible');"
             "check('five entries',performance.getEntriesByType('visibility-state').length===5)");

    web_free(doc);
    /* 2 page checks at load, 5 visible, 11 hidden, 12 shown again, 5 for two queued tasks. */
    const int expected=35;
    if(checks!=expected||errors){printf("FAIL pagevisibility completion checks=%d expected=%d errors=%d\n",checks,expected,errors);failures++;}
    printf("pagevisibilitytest: %d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
