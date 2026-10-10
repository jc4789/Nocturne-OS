/* A live iframe stream must be idle without input, not merely slow to rescan. */
#include "webi.h"
#include "frame.h"
#include <nocturne.h>
#include <stdio.h>

static int checks,failures,errors;
static void check(const char *name,bool value){
    checks++;if(!value)failures++;printf("%s %s\n",value?"OK":"FAIL",name);
}
static void log_line(void *unused,int level,const char *text){
    if(!strncmp(text,"OK ",3))check(text+3,true);
    else if(!strncmp(text,"FAIL ",5))check(text+5,false);
    else if(!strncmp(text,"STREAM-CSS ",11))printf("%s\n",text);
    else if(level>=2){errors++;printf("ERROR %s\n",text);}
}
static bool eval(web_doc *doc,const char *source){return web_console_eval(doc,source,strlen(source));}
static void settle(web_doc *top,unsigned ticks){for(unsigned i=0;i<ticks;i++){web_tick(top,uptime_ms());msleep(1);}}
int main(void){
    const char page[]="<!doctype html><iframe srcdoc='<p>initial frame</p>'></iframe>";
    struct web_host host={.console=log_line,.js_task_budget_ms=5000};
    web_doc *top=web_live(page,sizeof page-1,"https://stream.test/","utf-8",&host);
    check("live-document",top!=NULL);if(!top)goto done;
    uint64_t deadline=uptime_ms()+10000;web_doc *child=NULL;
    do{settle(top,1);if(top->frames&&!top->frames->detached)child=top->frames->document;}while(!child&&uptime_ms()<deadline);
    check("live-child-context",child!=NULL);if(!child)goto cleanup;
    settle(top,20);
    /* The embedder establishes a viewport before browser geometry/style reads.
       web_tick alone does not enter the frame paint/layout service. */
    web_layout(child,300,150);
    printf("STREAM-VIEWPORT width=%d height=%d\n",child->width,child->height);
    check("initialized-child-viewport",child->width==300&&child->height==150);
    check("open-and-write",eval(child,
        "document.open();function check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
        "function colour(n,wanted){const actual=getComputedStyle(savedTarget).color;console.log('STREAM-CSS '+n+' actual='+JSON.stringify(actual)+' expected='+JSON.stringify(wanted));check(n,actual===wanted);}"
        "document.write('<!doctype html><style>#target{color:rgb(1,2,3)}</style><body><div id=target>A</div><div id=tail>B');"
        "var savedTarget=document.getElementById('target');"
        "check('initial-visible-text',savedTarget.textContent==='A');"
        "colour('initial-css','rgba(1, 2, 3, 1)');"));
    settle(top,10);
    check("open-stream-has-no-input",child->parser&&!html_pending_input(child->parser));
    check("native-edit-during-wait",eval(child,
        "savedTarget.textContent='native';document.body.setAttribute('data-native','kept');"));
    settle(top,10);
    uint64_t rescans=child->profile.rescans,revision=child->dom_revision;
    uint64_t body_revision=child->body?child->body->resource_revision:0;
    settle(top,100);
    check("waiting-ticks-no-css-rescan",child->profile.rescans==rescans);
    check("waiting-ticks-no-dom-republish",child->dom_revision==revision);
    check("waiting-ticks-no-link-revision",child->body&&child->body->resource_revision==body_revision);
    check("empty-write",eval(child,"document.write('');"));settle(top,10);
    check("empty-write-no-css-rescan",child->profile.rescans==rescans);
    check("actual-write",eval(child,
        "document.write('C</div><body data-late=from-parser><style>#target{color:rgb(4,5,6)}</style><p id=new>new</p>');"
        "check('native-identity-preserved',document.getElementById('target')===savedTarget);"
        "check('native-attribute-exported',document.body.getAttribute('data-native')==='kept');"
        "check('native-text-exported',savedTarget.textContent==='native');"
        "check('parser-attribute-imported',document.body.getAttribute('data-late')==='from-parser');"
        "colour('new-css-imported','rgba(4, 5, 6, 1)');"
        "check('new-element-visible',document.getElementById('new').textContent==='new');"
        "check('suspended-token-text-imported',document.getElementById('tail').textContent==='BC');"));
    settle(top,10);check("actual-write-rescans",child->profile.rescans>rescans);
    check("close-eof",eval(child,"document.close();"));settle(top,20);
    check("close-eof-finishes-parser",child->parser==NULL);
    check("closed-visible-dom",eval(child,
        "check('closed-ready-state',document.readyState==='complete');"
        "check('closed-visible-text',savedTarget.textContent==='native'&&document.getElementById('tail').textContent==='BC');"
        "colour('closed-css','rgba(4, 5, 6, 1)');"));
cleanup:web_free(top);
done:check("no-unexpected-js-errors",errors==0);
    printf("waitingstreamtest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
