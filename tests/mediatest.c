/* Native viewport -> CSS evaluator -> QuickJS MQL notification integration.
   Synthetic API regression, not acceptance of any real website. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"
static int failed,checks,ready,events;
#define CHECK(x) do{checks++;if(!(x)){printf("FAIL mediatest:%d: %s\n",__LINE__,#x);failed++;}}while(0)
static void log_message(void *opaque,int level,const char *message){
    (void)opaque;
    if(!strcmp(message,"MEDIA_READY"))ready=1;
    else if(!strncmp(message,"MEDIA_EVENT ",12))events++;
    else if(level==2||!strncmp(message,"FAIL",4)){printf("%s\n",message);failed++;}
}
static void ticks(web_doc *doc,int count){for(int i=0;i<count;i++)web_tick(doc,uptime_ms());}
int main(void){
    const char *prefix="<!doctype html><style>#target{color:red}"
        "@media (scripting:enabled){#target{color:blue}}"
        "@media (min-width:500px){#target{color:lime}}"
        "@media not (unknown-feature){#target{color:red}}"
        "</style><body><div id=target>media</div><script>";
    const char *tail=";if(runMediaCases()<40)throw Error('media API count');"
        "var m=matchMedia('(min-width:500px)'),seen=0;"
        "if(m.matches)throw Error('initial viewport');"
        "var target=document.getElementById('target');if(getComputedStyle(target).color!=='rgba(0, 0, 255, 1)')throw Error('CSS scripting media');"
        "function legacy(e){if(e.matches!==m.matches||e.target!==m||this!==m||!e.isTrusted||e.media!==m.media)throw Error('event fields');"
        "if(getComputedStyle(target).color!==(m.matches?'rgba(0, 255, 0, 1)':'rgba(0, 0, 255, 1)'))throw Error('CSS and JS media disagree');"
        "seen++;console.log('MEDIA_EVENT legacy');}"
        "m.addListener(legacy);m.addEventListener('change',legacy);"
        "m.addEventListener('change',()=>console.log('MEDIA_EVENT once'),{once:true});"
        "m.onchange=()=>console.log('MEDIA_EVENT handler');"
        "var abort=new AbortController();m.addEventListener('change',()=>{throw Error('aborted listener');},{signal:abort.signal});abort.abort();"
        "addEventListener('media-check',()=>{if(seen!==2||m.matches)throw Error('final state');m.removeListener(legacy);m.onchange=null;});"
        "console.log('MEDIA_READY');</script></body>";
    FILE *f=fopen("/data/tests/js_media_cases.js","rb");CHECK(f!=NULL);if(!f)return 1;
    char *script=malloc(65536);CHECK(script!=NULL);if(!script){fclose(f);return 1;}
    size_t n=fread(script,1,65535,f);fclose(f);script[n]=0;
    size_t size=strlen(prefix)+n+strlen(tail)+1;char *page=malloc(size);CHECK(page!=NULL);if(!page){free(script);return 1;}
    snprintf(page,size,"%s%s%s",prefix,script,tail);free(script);
    struct web_host host={.console=log_message};web_doc *doc=web_live(page,strlen(page),"https://media.test/","utf-8",&host);free(page);
    CHECK(doc!=NULL);if(!doc)return 1;
    web_layout(doc,400,600);ticks(doc,30);CHECK(ready==1);CHECK(events==0);
    web_layout(doc,800,600);CHECK(web_deadline(doc)>=0);ticks(doc,10);CHECK(events==3);
    web_layout(doc,801,600);ticks(doc,10);CHECK(events==3);
    web_layout(doc,400,600);ticks(doc,10);CHECK(events==5);
    struct web_event e={.type="media-check"};CHECK(web_dispatch(doc,NULL,&e));
    web_layout(doc,800,600);ticks(doc,10);CHECK(events==5);
    web_free(doc);printf("mediatest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
