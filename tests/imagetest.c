/* Supplementary real web_live/QuickJS/native image-decoder test. The transport
 * is deterministic, including late responses. This is not website acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "web.h"
struct queued { uint64_t id,due; char url[2048]; bool used; };
static struct { web_doc *doc; struct queued pending[16]; int errors,failures,requests,images,completions; bool done; } state;
static const char one[]="<svg xmlns='http://www.w3.org/2000/svg' width='2' height='3'><rect width='2' height='3' fill='red'/></svg>";
static const char two[]="<svg xmlns='http://www.w3.org/2000/svg' width='5' height='7'><rect width='5' height='7' fill='blue'/></svg>";
static const char slow[]="<svg xmlns='http://www.w3.org/2000/svg' width='4' height='6'><rect width='4' height='6' fill='green'/></svg>";
static bool request(void *opaque,const struct web_request *r){
    (void)opaque;state.requests++;if(r->kind==WEB_RESOURCE_IMAGE)state.images++;
    for(int i=0;i<16;i++)if(!state.pending[i].used){
        struct queued *q=&state.pending[i];q->used=true;q->id=r->id;snprintf(q->url,sizeof q->url,"%s",r->url);
        q->due=uptime_ms()+((strstr(r->url,"slow.svg")||strstr(r->url,"old.svg"))?80:2);return true;
    }
    return false;
}
static void cancel(void *opaque,uint64_t id){(void)opaque;for(int i=0;i<16;i++)if(state.pending[i].id==id)state.pending[i].used=false;}
static void log_message(void *opaque,int level,const char *message){
    (void)opaque;if(level==2){state.errors++;printf("ERROR %s\n",message);}
    if(!strncmp(message,"FAIL ",5)){state.failures++;printf("%s\n",message);}
    if(!strncmp(message,"IMAGE-DONE ",11)){state.done=true;printf("%s\n",message);}
}
static void deliver(void){
    for(int i=0;i<16;i++)if(state.pending[i].used&&state.pending[i].due<=uptime_ms()){
        struct queued q=state.pending[i];state.pending[i].used=false;
        const char *body=strstr(q.url,"one.svg")?one:strstr(q.url,"two.svg")?two:
            (strstr(q.url,"slow.svg")||strstr(q.url,"old.svg"))?slow:"not an image";
        struct web_response r={0};r.status=200;snprintf(r.url,sizeof r.url,"%s",q.url);
        snprintf(r.headers,sizeof r.headers,"Content-Type: image/svg+xml\r\n");r.body=strdup(body);r.body_len=strlen(body);
        web_resource_loaded(state.doc,q.id,&r);memset(r.body,'!',r.body_len);free(r.body);state.completions++;
    }
}
int main(void){
    FILE *file=fopen("/data/tests/js_image_cases.js","rb");if(!file){puts("FAIL Image test source missing");return 1;}
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
    if(length<0||length>65536){fclose(file);puts("FAIL Image test source length");return 1;}
    const char *head="<!doctype html><html><head></head><body><script>";
    const char *tail=";Promise.resolve().then(()=>runFormInterfaceCases()).then(forms=>runImageCases().then(images=>console.log('IMAGE-DONE '+forms+' forms '+images+' images'))).catch(e=>{console.log('FAIL image '+e+' '+e.stack);console.log('IMAGE-DONE failure');});</script></body></html>";
    size_t size=strlen(head)+(size_t)length+strlen(tail)+1;char *html=malloc(size);
    if(!html){fclose(file);puts("FAIL Image source allocation");return 1;}
    strcpy(html,head);size_t got=fread(html+strlen(head),1,(size_t)length,file);fclose(file);
    html[strlen(head)+got]=0;strcat(html,tail);
    struct web_host host={.request=request,.cancel=cancel,.console=log_message};
    state.doc=web_live(html,strlen(html),"http://image.fixture/index.html","utf-8",&host);free(html);
    if(!state.doc){puts("FAIL Image document allocation");return 1;}
    uint64_t deadline=uptime_ms()+15000;
    while(!state.done&&uptime_ms()<deadline){deliver();web_tick(state.doc,uptime_ms());web_layout(state.doc,800,600);msleep(1);}
    if(!state.done){state.failures++;puts("FAIL Image test timeout");}
    if(state.images<5||state.completions<5){state.failures++;puts("FAIL detached image transport was not exercised");}
    web_free(state.doc);
    printf("imagetest: %d failures, %d unexpected errors, %d image requests, %d completions\n",state.failures,state.errors,state.images,state.completions);
    return state.failures||state.errors;
}
