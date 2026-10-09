/* Actual native functions; clock/task/profile/origin fixtures only. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <math.h>
#include "quickjs.h"
#define JS_TIMERS_INITIAL 32u
struct web_js_state;
struct web_profile {unsigned unused;};
typedef struct web_doc {struct web_js_state *js;bool live;uint64_t dom_revision;struct web_profile profile;const char *url;} web_doc;
struct js_timer {uint32_t id;int kind;uint64_t due,interval;JSValue fn,args;};
struct js_posted_task {struct js_posted_task *next;JSValue fn;uint32_t id;};
struct js_idle_task {struct js_idle_task *next;JSValue fn;uint32_t id;uint64_t due;};
struct js_rejection {JSValue promise,reason;struct js_rejection *next;};
struct js_pending {struct js_pending *next;uint64_t id,deadline;int kind;char *url;JSValue resolve,reject;};
struct web_js_state {
    JSContext *ctx;JSRuntime *rt;web_doc *doc;struct web_js_state *runtime_owner;
    struct js_timer *timers;unsigned timer_capacity;uint32_t next_timer;
    struct js_posted_task *posted,*last_posted;unsigned posted_count;uint32_t next_posted;bool posted_id_wrapped;
    struct js_idle_task *idle_pending,*last_idle_pending,*idle_runnable,*last_idle_runnable;
    unsigned idle_count,idle_period_callbacks;uint64_t idle_period_end;bool idle_period_stopped;uint32_t next_idle;
    struct js_rejection *rejections,*last_rejection;bool rejection_oom;
    struct js_pending *pending;unsigned pending_count;uint64_t next_request;
    bool disabled;uint64_t task_layout_ms;unsigned task_layout_flushes;
};
typedef struct {char *p;size_t n;} sbuf;
static unsigned checks,failed,reported,callback_count,expected_args;static bool args_ok;
/* Saved, unexecuted repair: small cached blocks need allocation-site failure
 * injection; JS_SetMemoryLimit alone does not force their malloc to fail. */
static bool force_rejection_oom;
static void *test_rejection_allocate(JSRuntime *rt,size_t n){if(force_rejection_oom){force_rejection_oom=false;return NULL;}return js_malloc_rt(rt,n);}
static struct web_js_state *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static uint64_t uptime_ms(void){return 0;}
static uint64_t relative_time(struct web_js_state *s,uint64_t now){return now;}
static const char *web_effective_url(web_doc *d){return d->url;}
static bool make_origin(const char *url,char *out,size_t n){if(strlen(url)>=n)return false;strcpy(out,url);return true;}
static void sb_puts(sbuf *b,const char *s){size_t n=strlen(s);b->p=realloc(b->p,b->n+n+1);memcpy(b->p+b->n,s,n+1);b->n+=n;}
static void sb_putc(sbuf *b,char c){char s[]={c,0};sb_puts(b,s);}
static const char *sb_cstr(sbuf *b){return b->p?b->p:"";}
static void sb_free(sbuf *b){free(b->p);}
static void log_text(struct web_js_state *s,int level,const char *text){reported++;}
static void diagnostic_context(struct web_js_state *s){}
static void diagnostic_excerpt(struct web_js_state *s,const char *text){}
static void exception(struct web_js_state *s){JS_FreeValue(s->ctx,JS_GetException(s->ctx));}
static void begin_named_task(struct web_js_state *s,const char *name,uint64_t id){}
static void report_rejections(struct web_js_state *s);
static void end_task(struct web_js_state *s){report_rejections(s);}
/* ACTUAL_QUEUE_FUNCTIONS */
static JSValue callback(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    callback_count++;args_ok=argc==(int)expected_args;
    if(argc==100){int32_t first,last;JS_ToInt32(ctx,&first,argv[0]);JS_ToInt32(ctx,&last,argv[99]);args_ok&=first==0&&last==99;}
    return JS_UNDEFINED;
}
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("失敗 %s\n",name);}}
static unsigned timers_count(struct web_js_state *s){unsigned n=0;for(unsigned i=0;i<s->timer_capacity;i++)n+=s->timers[i].id!=0;return n;}
static unsigned rejection_count(struct web_js_state *s){unsigned n=0;for(struct js_rejection *p=s->rejections;p;p=p->next)n++;return n;}
static JSValue timer(struct web_js_state *s,JSValue fn,JSValue args,double delay){JSValue q[]={JS_NewInt32(s->ctx,0),fn,JS_NewFloat64(s->ctx,delay),args};return native_timer(s->ctx,JS_UNDEFINED,4,q);}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);JSRuntime *rt=JS_NewRuntime();JS_SetMaxStackSize(rt,1u<<20);JSContext *ctx=JS_NewContext(rt);
    struct web_js_state s={.ctx=ctx,.rt=rt};web_doc doc={.js=&s,.live=true,.url="https://fixture.invalid"};s.doc=&doc;s.runtime_owner=&s;JS_SetContextOpaque(ctx,&s);
    JSValue fn=JS_NewCFunction(ctx,callback,"callback",0),empty=JS_NewArray(ctx);
    bool all=true;for(unsigned i=0;i<1500;i++){JSValue id=timer(&s,fn,empty,1000);all&=!JS_IsException(id);JS_FreeValue(ctx,id);}
    check(all&&timers_count(&s)==1500&&s.timer_capacity>=1500,"旧1024を越えるtimer grow");
    JSValue hundred=JS_NewArray(ctx);for(unsigned i=0;i<100;i++)JS_SetPropertyUint32(ctx,hundred,i,JS_NewUint32(ctx,i));
    JSValue id=timer(&s,fn,hundred,0);expected_args=100;run_timer(&s,0);check(args_ok&&callback_count==1&&timers_count(&s)==1500,"timer追加引数100件を非切捨て配送");JS_FreeValue(ctx,id);
    while(timers_count(&s)<s.timer_capacity){JSValue v=timer(&s,fn,empty,1000);JS_FreeValue(ctx,v);}
    unsigned capacity=s.timer_capacity;struct js_timer *before=s.timers;JS_SetMemoryLimit(rt,1);id=timer(&s,fn,empty,1000);
    check(JS_IsException(id)&&before==s.timers&&s.timer_capacity==capacity&&timers_count(&s)==capacity,"grow OOMは既存tableを保持");exception(&s);JS_SetMemoryLimit(rt,SIZE_MAX);
    JSValue q[]={fn,JS_NewUint32(ctx,1000)};all=true;for(unsigned i=0;i<1400;i++){JSValue v=native_idle(ctx,JS_UNDEFINED,2,q);all&=!JS_IsException(v);JS_FreeValue(ctx,v);}
    check(all&&s.idle_count==1400,"旧1024を越えるidle queue");
    for(unsigned i=0;i<5000;i++){JSValue v=native_post_task(ctx,JS_UNDEFINED,1,&fn);all&=!JS_IsException(v);JS_FreeValue(ctx,v);}
    check(all&&s.posted_count==5000,"旧4096を越えるposted queue");
    s.next_posted=UINT_MAX;id=native_post_task(ctx,JS_UNDEFINED,1,&fn);uint32_t posted_id;JS_ToUint32(ctx,&posted_id,id);
    check(posted_id==5001&&s.posted_count==5001,"post ID wrapは生存handleと衝突しない");JS_FreeValue(ctx,id);
    release_document_tasks(&s);check(!s.timers&&!s.posted&&!s.idle_pending&&!s.timer_capacity&&!s.posted_count&&!s.idle_count,"dynamic pending task roots解放");
    for(unsigned i=0;i<160;i++){JSValue p=JS_NewObject(ctx);promise_rejection(ctx,p,JS_NewUint32(ctx,i),false,NULL);JS_FreeValue(ctx,p);}
    check(rejection_count(&s)==160,"旧32を越えるrejection保存");reported=0;report_rejections(&s);check(reported==32&&rejection_count(&s)==128,"report batchは残りを黙落ちさせない");
    JSValue p=JS_DupValue(ctx,s.rejections->promise);promise_rejection(ctx,p,JS_UNDEFINED,true,NULL);JS_FreeValue(ctx,p);check(rejection_count(&s)==127,"dynamic handled rejection unlink");
    p=JS_NewObject(ctx);force_rejection_oom=true;promise_rejection(ctx,p,JS_UNDEFINED,false,NULL);check(s.rejection_oom&&rejection_count(&s)==127,"tracker allocation失敗は記録保持");JS_FreeValue(ctx,p);release_document_tasks(&s);
    all=true;for(unsigned i=0;i<160;i++)all&=pending_new(&s,0,"https://fixture.invalid/resource")!=NULL;
    check(all&&s.pending_count==160,"旧64を越えるJS request pending");
    while(s.pending){struct js_pending *p=s.pending;s.pending=p->next;js_free(ctx,p->url);js_free(ctx,p);}s.pending_count=0;
    char name_text[5001];memset(name_text,'x',5000);name_text[5000]=0;JSValue name=JS_NewString(ctx,name_text),ids[300];
    JSValue open[]={JS_NewInt32(ctx,0),name,fn};all=true;for(unsigned i=0;i<300;i++){ids[i]=native_broadcast(ctx,JS_UNDEFINED,3,open);all&=!JS_IsException(ids[i]);}
    check(all&&broadcast_count==300,"旧256/4096byteを越えるchannel登録");
    JSValue send[]={JS_NewInt32(ctx,2),ids[0],JS_NewString(ctx,"payload")};JSValue sent=native_broadcast(ctx,JS_UNDEFINED,3,send);
    check(!JS_IsException(sent)&&s.posted_count==299,"broadcast dynamic atomic fanout299");JS_FreeValue(ctx,sent);JS_FreeValue(ctx,send[2]);
    expected_args=1;callback_count=0;while(s.posted)run_posted_task(&s);check(callback_count==299,"dynamic fanoutの全target配送");
    for(unsigned i=0;i<300;i++){JSValue close[]={JS_NewInt32(ctx,1),ids[i]};JSValue v=native_broadcast(ctx,JS_UNDEFINED,2,close);JS_FreeValue(ctx,v);JS_FreeValue(ctx,ids[i]);}JS_FreeValue(ctx,name);
    name=JS_NewString(ctx,"large-message");open[1]=name;JSValue a=native_broadcast(ctx,JS_UNDEFINED,3,open),b=native_broadcast(ctx,JS_UNDEFINED,3,open);
    char *large=malloc((1u<<20)+2);memset(large,'y',(1u<<20)+1);large[(1u<<20)+1]=0;send[1]=a;send[2]=JS_NewString(ctx,large);free(large);
    sent=native_broadcast(ctx,JS_UNDEFINED,3,send);check(!JS_IsException(sent)&&s.posted_count==1,"旧1MiBを越えるchannel message");JS_FreeValue(ctx,sent);JS_FreeValue(ctx,send[2]);release_document_tasks(&s);broadcast_free(&s);JS_FreeValue(ctx,a);JS_FreeValue(ctx,b);JS_FreeValue(ctx,name);
    printf("dynamic native queues: %u new checks, %u failed\n",checks,failed);
    JS_FreeValue(ctx,hundred);JS_FreeValue(ctx,empty);JS_FreeValue(ctx,fn);JS_FreeContext(ctx);JS_FreeRuntime(rt);return failed!=0;
}
