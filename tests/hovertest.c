/* Real web_live -> native hit test -> web_hover -> QuickJS regression.
   Auxiliary offline contracts only; does not replace real-site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include <web.h>

static int ready, pending, outside, x, y, done, errors, checks;
static void receive(void *opaque, int level, const char *message) {
    (void)opaque;
    if (!strcmp(message,"HOVER_READY")) ready=1;
    else if (!strcmp(message,"HOVER_OUTSIDE")) {pending=1;outside=1;}
    else if (sscanf(message,"HOVER_POINT %d %d",&x,&y)==2) {pending=1;outside=0;}
    else if (sscanf(message,"HOVER_DONE %d",&checks)==1) done=1;
    else if (level==2) {errors++;printf("hovertest error: %s\n",message);}
}
static void scroll(void *opaque,int *sx,int *sy) {(void)opaque;*sx=5;*sy=7;}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"/src/tests/js_hover_cases.js";
    FILE *f=fopen(path,"rb"); if(!f){printf("hovertest: missing %s\n",path);return 1;}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    if(size<0||size>65536){fclose(f);return 1;}
    const char *start="<!doctype html><html><head><style>html,body{margin:0}</style></head><body><script>";
    const char *end="\nconst cases=runHoverCases();addEventListener('hovertest-next',()=>{"
        "const step=cases.next();if(step.done){console.log('HOVER_DONE '+step.value);return;}"
        "if(step.value===null){console.log('HOVER_OUTSIDE');return;}"
        "const r=step.value.getBoundingClientRect();console.log('HOVER_POINT '+Math.floor(r.left+6)+' '+Math.floor(r.top+6));"
        "});console.log('HOVER_READY');</script></body></html>";
    size_t length=strlen(start)+(size_t)size+strlen(end);
    char *html=malloc(length+1);if(!html){fclose(f);return 1;}
    strcpy(html,start);size_t read=fread(html+strlen(start),1,(size_t)size,f);fclose(f);
    if(read!=(size_t)size){free(html);return 1;}strcpy(html+strlen(start)+(size_t)size,end);
    struct web_host host={.console=receive,.scroll=scroll};
    web_doc *doc=web_live(html,length,"https://hover.invalid/",NULL,&host);free(html);
    if(!doc)return 1;
    for(int i=0;i<200&&!ready&&!errors;i++)web_tick(doc,uptime_ms());
    if(!ready){printf("hovertest: bootstrap not ready\n");web_free(doc);return 1;}
    web_layout(doc,800,600);
    for(int i=0;i<32&&!done&&!errors;i++) {
        pending=0;
        struct web_event next={.type="hovertest-next"};
        web_dispatch(doc,NULL,&next);
        if(done||errors)break;
        if(!pending){errors++;break;}
        web_layout(doc,800,600);
        web_node *target=outside?NULL:web_node_at(doc,x+5,y+7);
        if(!outside&&!target){printf("hovertest: no native hit %d,%d\n",x,y);errors++;break;}
        struct web_event e={.x=x,.y=y,.buttons=5,.ctrl=true,.shift=true};
        web_hover(doc,target,&e);
        if(web_script_running(doc)){errors++;break;}
    }
    web_free(doc);
    if(!done)errors++;
    printf("hovertest: %d checks, %d failed\n",checks,errors);
    return errors!=0;
}
