/* Browser-side, nonblocking Nocturne native dedicated-worker transport. */
#include "js_worker.h"
#include "js_worker_wire.h"
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
struct packet {struct packet *next;struct njw_header h;size_t at;uint8_t *data;};
struct child {struct child *next;char *name;uint32_t id;int pid,in,out;bool started,stopped,closing;struct packet *tx,*tail;struct njw_header rx;size_t head_at,data_at,rx_charge;uint8_t *data;};
struct event {struct event *next;uint32_t id;struct njw_header h;uint8_t *data;};
struct web_workers {JSContext *ctx;JSValue callback;uint32_t generation,next_id;web_worker_loader load;void *opaque;struct child *children;struct event *events,*last;size_t queued;};
static struct child *retired;
static unsigned active_children;
void web_worker_background(void){struct child **p=&retired;while(*p){struct child *r=*p;int n=waitpid(r->pid,NULL,WNOHANG);if(n==r->pid||(n<0&&errno==ECHILD)){*p=r->next;free(r->name);free(r);if(active_children)active_children--;}else p=&r->next;}}
int64_t web_worker_background_deadline(uint64_t now){return retired?(int64_t)now+10:-1;}
static struct child *find(web_workers *w,uint32_t id){for(struct child *c=w->children;c;c=c->next)if(c->id==id)return c;return NULL;}
static void release_packet(web_workers *w,struct packet *p){w->queued-=sizeof p->h+p->h.bytes;free(p->data);free(p);}
static void stop(web_workers *w,struct child *c){
    if(c->stopped)return;c->stopped=true;
    if(c->in>=0)close(c->in);if(c->out>=0)close(c->out);c->in=c->out=-1;
    if(c->pid>0){kill(c->pid);int n=waitpid(c->pid,NULL,WNOHANG);if(n==c->pid||(n<0&&errno==ECHILD)){if(active_children)active_children--;c->pid=0;}}
    while(c->tx){struct packet *p=c->tx;c->tx=p->next;release_packet(w,p);}c->tail=NULL;free(c->data);c->data=NULL;w->queued-=c->rx_charge;c->rx_charge=0;c->head_at=c->data_at=0;
}
static bool queue(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,const void *data,size_t length,bool first){
    if(length>NJW_MAX_BYTES||sizeof(struct njw_header)+length>NJW_MAX_QUEUE-w->queued||c->stopped)return false;
    struct packet *p=calloc(1,sizeof *p);if(!p)return false;if(length){p->data=malloc(length);if(!p->data){free(p);return false;}memcpy(p->data,data,length);}
    p->h=(struct njw_header){NJW_MAGIC,op,w->generation,request,kind,(uint32_t)length};
    if(first){p->next=c->tx;c->tx=p;if(!c->tail)c->tail=p;}else{if(c->tail)c->tail->next=p;else c->tx=p;c->tail=p;}w->queued+=sizeof p->h+length;return true;
}
static bool serialized(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,JSValueConst value,bool first){size_t n=0;uint8_t *p=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!p)return false;bool ok=queue(w,c,op,request,kind,p,n,first);js_free(w->ctx,p);return ok;}
static void event(web_workers *w,struct child *c,uint32_t op,JSValueConst value){
    size_t n;uint8_t *bytes=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)return;
    struct event *e=calloc(1,sizeof *e);if(!e){js_free(w->ctx,bytes);return;}e->data=malloc(n);if(!e->data){free(e);js_free(w->ctx,bytes);return;}memcpy(e->data,bytes,n);js_free(w->ctx,bytes);
    e->id=c->id;e->h=(struct njw_header){NJW_MAGIC,op,w->generation,0,0,(uint32_t)n};if(w->last)w->last->next=e;else w->events=e;w->last=e;w->queued+=sizeof e->h+n;
}
static void fail(web_workers *w,struct child *c,const char *message){stop(w,c);JSValue v=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,v,"message",JS_NewString(w->ctx,message));event(w,c,NJW_ERROR,v);JS_FreeValue(w->ctx,v);}
static bool spawn_child(web_workers *w,struct child *c){
    web_worker_background();if(active_children>=NJW_MAX_WORKERS){fail(w,c,"Native Worker process quota exceeded");return false;}
    int ip[2]={-1,-1},op[2]={-1,-1},nullfd=-1;
    if(pipe(ip)<0||pipe(op)<0||(nullfd=open("/dev/null",O_WRONLY))<0)goto bad;
    int map[3]={ip[0],op[1],nullfd};char *argv[]={"browserjsworker",NULL};c->pid=spawn("/bin/browserjsworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(nullfd);nullfd=-1;if(c->pid<0){c->pid=0;goto bad;}active_children++;
    c->in=ip[1];ip[1]=-1;c->out=op[0];op[0]=-1;if(fcntl(c->in,F_SETFL,O_NONBLOCK)<0||fcntl(c->out,F_SETFL,O_NONBLOCK)<0){fail(w,c,"Worker nonblocking pipe setup failed");return false;}
    char info[160];snprintf(info,sizeof info,"native dedicated task; heap ceiling %u MiB (lazy)",NJW_HEAP_BYTES/(1024u*1024u));JSValue log=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,log,"message",JS_NewString(w->ctx,info));event(w,c,NJW_CONSOLE,log);JS_FreeValue(w->ctx,log);return true;
bad:for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);}if(nullfd>=0)close(nullfd);fail(w,c,"Cannot spawn native dedicated Worker");return false;
}
web_workers *web_worker_new(JSContext *ctx,uint32_t generation,web_worker_loader load,void *opaque){web_workers *w=calloc(1,sizeof *w);if(!w)return NULL;w->ctx=ctx;w->generation=generation;w->callback=JS_UNDEFINED;w->load=load;w->opaque=opaque;return w;}
void web_worker_set_callback(web_workers *w,JSValueConst fn){if(!w)return;JS_FreeValue(w->ctx,w->callback);w->callback=JS_DupValue(w->ctx,fn);}
JSValue web_worker_native(web_workers *w,int argc,JSValueConst *argv){
    if(!w)return JS_UNDEFINED;JSContext *ctx=w->ctx;uint32_t op=0,id=0;if(argc<2||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0)return JS_EXCEPTION;
    if(op==0){unsigned live=0;for(struct child *c=w->children;c;c=c->next)if(!c->stopped)live++;if(live>=NJW_MAX_WORKERS)return JS_ThrowRangeError(ctx,"Dedicated Worker quota exceeded");
        const char *url=argc>2?JS_ToCString(ctx,argv[2]):NULL;if(!url)return JS_EXCEPTION;struct child *c=calloc(1,sizeof *c);if(!c){JS_FreeCString(ctx,url);return JS_ThrowOutOfMemory(ctx);}const char *name=argc>3?JS_ToCString(ctx,argv[3]):"";if(!name||strlen(name)>65536){if(argc>3)JS_FreeCString(ctx,name);JS_FreeCString(ctx,url);free(c);return JS_ThrowRangeError(ctx,"Worker name quota exceeded");}c->name=strdup(name);if(argc>3)JS_FreeCString(ctx,name);if(!c->name){JS_FreeCString(ctx,url);free(c);return JS_ThrowOutOfMemory(ctx);}c->id=++w->next_id;c->in=c->out=-1;c->next=w->children;w->children=c;
        bool ok=w->load&&w->load(w->opaque,c->id,0,url,0);JS_FreeCString(ctx,url);if(!ok){w->children=c->next;free(c->name);free(c);return JS_ThrowTypeError(ctx,"Worker source URL rejected or request unavailable");}return JS_NewUint32(ctx,c->id);
    }
    struct child *c=find(w,id);if(!c||c->stopped)return JS_UNDEFINED;
    if(op==2){stop(w,c);if(!c->pid){struct child **at=&w->children;while(*at&&*at!=c)at=&(*at)->next;if(*at)*at=c->next;free(c->name);free(c);}return JS_UNDEFINED;}
    if(op==1){if(argc<3)return JS_ThrowTypeError(ctx,"Worker message required");if(!serialized(w,c,NJW_MESSAGE,0,0,argv[2],false))return JS_ThrowRangeError(ctx,"Worker message queue quota exceeded");return JS_UNDEFINED;}
    return JS_ThrowTypeError(ctx,"Invalid Worker operation");
}
void web_worker_loaded(web_workers *w,uint32_t id,uint32_t request,int status,const char *url,const char *headers,const void *bytes,size_t n,const char *error){
    if(!w)return;struct child *c=find(w,id);if(!c||c->stopped)return;
    if(n>NJW_MAX_BYTES){fail(w,c,"Worker resource exceeds byte quota");return;}
    JSContext *ctx=w->ctx;JSValue a=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,a,0,JS_NewInt32(ctx,status));JS_SetPropertyUint32(ctx,a,1,JS_NewString(ctx,url?url:""));JS_SetPropertyUint32(ctx,a,2,JS_NewString(ctx,headers?headers:""));JS_SetPropertyUint32(ctx,a,3,JS_NewArrayBufferCopy(ctx,bytes,n));JS_SetPropertyUint32(ctx,a,4,JS_NewString(ctx,error?error:""));JS_SetPropertyUint32(ctx,a,5,JS_NewString(ctx,c->name?c->name:""));
    bool first=request==0;bool ok=serialized(w,c,first?NJW_START:NJW_LOADED,request,0,a,first);JS_FreeValue(ctx,a);if(!ok){fail(w,c,"Worker resource queue quota exceeded");return;}if(first)c->started=true;
}
void web_worker_pump(web_workers *w,uint64_t now){
    (void)now;web_worker_background();if(!w)return;
    for(struct child *c=w->children;c;c=c->next){size_t budget=16384;if(c->stopped){if(c->pid){int n=waitpid(c->pid,NULL,WNOHANG);if(n==c->pid||(n<0&&errno==ECHILD)){c->pid=0;if(active_children)active_children--;}}continue;}if(!c->started||c->closing)continue;if(!c->pid&&!spawn_child(w,c))continue;
        struct n_pollfd p[2]={{c->in,N_POLLOUT,0},{c->out,N_POLLIN,0}};if(poll(p,2,0)<0){fail(w,c,"Worker poll failed");continue;}
        while(c->tx&&budget&&(p[0].revents&N_POLLOUT)){struct packet *a=c->tx;bool header=a->at<sizeof a->h;size_t left=header?sizeof a->h-a->at:sizeof a->h+a->h.bytes-a->at;size_t take=left<budget?left:budget;const uint8_t *src=header?(uint8_t *)&a->h+a->at:a->data+a->at-sizeof a->h;ssize_t n=write(c->in,src,take);if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){fail(w,c,"Worker command pipe closed");break;}a->at+=n;budget-=n;if(a->at==sizeof a->h+a->h.bytes){c->tx=a->next;if(!c->tx)c->tail=NULL;release_packet(w,a);}}
        while(!c->stopped&&budget&&(p[1].revents&(N_POLLIN|N_POLLHUP))){bool header=c->head_at<sizeof c->rx;
            /* Leave room for the largest valid packet before starting or
             * completing its header. A fast producer must be backpressured
             * by the native pipe, not killed because the page consumes its
             * messages more slowly. No read/write here may block the GUI. */
            if(header&&w->queued>NJW_MAX_QUEUE-NJW_MAX_BYTES-sizeof c->rx)break;
            size_t left=header?sizeof c->rx-c->head_at:c->rx.bytes-c->data_at;size_t take=left<budget?left:budget;ssize_t n=read(c->out,header?(void *)((uint8_t *)&c->rx+c->head_at):(void *)(c->data+c->data_at),take);if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){fail(w,c,"Worker process exited");break;}budget-=n;
            if(header){c->head_at+=n;if(c->head_at==sizeof c->rx){if(c->rx.magic!=NJW_MAGIC||c->rx.generation!=w->generation||c->rx.bytes>NJW_MAX_BYTES||!c->rx.bytes||(c->rx.op!=NJW_MESSAGE&&c->rx.op!=NJW_ERROR&&c->rx.op!=NJW_LOAD&&c->rx.op!=NJW_CLOSE&&c->rx.op!=NJW_CONSOLE)||c->rx.kind>2||c->rx.bytes+sizeof c->rx>NJW_MAX_QUEUE-w->queued){fail(w,c,"Invalid Worker response or receive quota");break;}c->data=malloc(c->rx.bytes);if(!c->data){fail(w,c,"Worker receive allocation failed");break;}c->rx_charge=sizeof c->rx+c->rx.bytes;w->queued+=c->rx_charge;}}else c->data_at+=n;
            if(c->head_at==sizeof c->rx&&c->data_at==c->rx.bytes){struct event *e=calloc(1,sizeof *e);if(!e){fail(w,c,"Worker event allocation failed");break;}e->id=c->id;e->h=c->rx;e->data=c->data;c->data=NULL;c->head_at=c->data_at=0;c->rx_charge=0;if(w->last)w->last->next=e;else w->events=e;w->last=e;if(e->h.op==NJW_CLOSE){c->closing=true;break;}}
        }
    }
    struct child **at=&w->children;while(*at){struct child *c=*at;if(c->stopped&&!c->pid){*at=c->next;free(c->name);free(c);}else at=&c->next;}
}
bool web_worker_runnable(const web_workers *w){return w&&w->events;}
bool web_worker_run_one(web_workers *w){
    if(!w||!w->events)return false;struct event *e=w->events;w->events=e->next;if(!w->events)w->last=NULL;w->queued-=sizeof e->h+e->h.bytes;struct child *c=find(w,e->id);
    if((c&&!c->stopped)||e->h.op==NJW_ERROR){JSContext *ctx=w->ctx;JSValue v=JS_ReadObject(ctx,e->data,e->h.bytes,JS_READ_OBJ_REFERENCE);if(!JS_IsException(v)){
        if(e->h.op==NJW_LOAD){const char *url=JS_ToCString(ctx,v);bool ok=url&&w->load&&w->load(w->opaque,c->id,e->h.request,url,e->h.kind);JS_FreeCString(ctx,url);if(!ok)web_worker_loaded(w,c->id,e->h.request,0,"","",NULL,0,"Worker resource URL rejected");}
        else {if(e->h.op==NJW_CONSOLE&&c){JSValue message=JS_GetPropertyStr(ctx,v,"message");const char *text=JS_ToCString(ctx,message);char log[1024];snprintf(log,sizeof log,"[Worker %u PID %d] %s",c->id,c->pid,text?text:"");JS_FreeCString(ctx,text);JS_FreeValue(ctx,message);JS_SetPropertyStr(ctx,v,"message",JS_NewString(ctx,log));}JSValue args[]={JS_NewUint32(ctx,e->id),JS_NewUint32(ctx,e->h.op),v};JSValue r=JS_Call(ctx,w->callback,JS_UNDEFINED,3,args);JS_FreeValue(ctx,r);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);if(e->h.op==NJW_CLOSE&&c)stop(w,c);}
        JS_FreeValue(ctx,v);
    }}free(e->data);free(e);return true;
}
int64_t web_worker_deadline(const web_workers *w,uint64_t now){if(!w)return -1;if(w->events)return (int64_t)now;for(struct child *c=w->children;c;c=c->next)if(c->pid||(!c->stopped&&c->started))return (int64_t)now+10;return -1;}
void web_worker_free(web_workers *w){if(!w)return;while(w->children){struct child *c=w->children;w->children=c->next;stop(w,c);if(c->pid){c->next=retired;retired=c;}else {free(c->name);free(c);}}while(w->events){struct event *e=w->events;w->events=e->next;free(e->data);free(e);}JS_FreeValue(w->ctx,w->callback);free(w);}
