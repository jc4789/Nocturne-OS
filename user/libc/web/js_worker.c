/* Browser-side, nonblocking Nocturne native dedicated-worker transport. */
#include "js_worker.h"
#include "js_worker_wire.h"
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
struct packet {struct packet *next;struct njw_header h;size_t at;uint8_t *data;};
#include "js_worker_port_lifetime.h"
struct child {struct child *next;char *name;size_t name_len;uint32_t id,parent_port;int pid,in,out;bool started,stopped,closing;struct packet *tx,*tail;struct broker_pair *ports;struct njw_header rx;size_t head_at,data_at,rx_charge;uint8_t *data;};
struct event {struct event *next;uint32_t id;struct njw_header h;uint8_t *data;};
struct web_workers {JSContext *ctx;JSValue callback;uint32_t generation,next_id;web_worker_loader load;web_worker_canceler cancel;void *opaque;struct child *children;struct event *events,*last;size_t queued;uint32_t next_lease;struct broker_lease *leases;};
struct web_worker_publication {web_workers *owner;struct child *child;struct packet *packet;struct broker_pair *ports,*close_pair,*ack_pair;};
static struct broker_pair *broker_find(struct child *c,uint32_t id,uint32_t creator){return worker_port_find(c->ports,id,creator);}
static void broker_packet_free(void *opaque){struct packet *p=opaque;if(p){free(p->data);free(p);}}
static size_t broker_free(struct broker_pair *p){size_t released=0;while(p){struct broker_pair *n=p->next;struct packet*a=p->held_close,*b=p->held_ack;if(a)released+=sizeof a->h+a->h.bytes;if(b)released+=sizeof b->h+b->h.bytes;broker_packet_free(a);broker_packet_free(b);free(p);p=n;}return released;}
static bool broker_metadata(JSContext *ctx,struct child *c,JSValueConst value,unsigned creator,struct broker_pair **result){return worker_port_metadata(ctx,value,creator,c->ports,creator?UINT32_MAX:c->parent_port,result);}
static struct child *retired;
static unsigned active_children;
void web_worker_background(void){struct child **p=&retired;while(*p){struct child *r=*p;int n=waitpid(r->pid,NULL,WNOHANG);if(n==r->pid||(n<0&&errno==ECHILD)){*p=r->next;free(r->name);free(r);if(active_children)active_children--;}else p=&r->next;}}
int64_t web_worker_background_deadline(uint64_t now){return retired?(int64_t)now+10:-1;}
static struct child *find(web_workers *w,uint32_t id){for(struct child *c=w->children;c;c=c->next)if(c->id==id)return c;return NULL;}
static bool queue_room(const web_workers *w,size_t length){return length<=SIZE_MAX-sizeof(struct njw_header)&&length+sizeof(struct njw_header)<=SIZE_MAX-w->queued;}
static void release_packet(web_workers *w,struct packet *p){w->queued-=sizeof p->h+p->h.bytes;free(p->data);free(p);}
static void stop(web_workers *w,struct child *c){
    if(c->stopped)return;c->stopped=true;
    if(w->cancel)w->cancel(w->opaque,c->id,0);
    if(c->in>=0)close(c->in);if(c->out>=0)close(c->out);c->in=c->out=-1;
    if(c->pid>0){kill(c->pid);int n=waitpid(c->pid,NULL,WNOHANG);if(n==c->pid||(n<0&&errno==ECHILD)){if(active_children)active_children--;c->pid=0;}}
    while(c->tx){struct packet *p=c->tx;c->tx=p->next;release_packet(w,p);}c->tail=NULL;free(c->data);c->data=NULL;w->queued-=c->rx_charge;c->rx_charge=0;c->head_at=c->data_at=0;
    w->queued-=broker_free(c->ports);c->ports=NULL;
}
static bool queue(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,const void *data,size_t length,bool first){
    if(length>NJW_MAX_BYTES||!queue_room(w,length)||c->stopped)return false;
    struct packet *p=calloc(1,sizeof *p);if(!p)return false;if(length){p->data=malloc(length);if(!p->data){free(p);return false;}memcpy(p->data,data,length);}
    p->h=(struct njw_header){NJW_MAGIC,op,w->generation,request,kind,(uint32_t)length};
    if(first){p->next=c->tx;c->tx=p;if(!c->tail)c->tail=p;}else{if(c->tail)c->tail->next=p;else c->tx=p;c->tail=p;}w->queued+=sizeof p->h+length;return true;
}
static bool serialized(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,JSValueConst value,bool first){size_t n=0;uint8_t *p=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!p)return false;bool ok=queue(w,c,op,request,kind,p,n,first);js_free(w->ctx,p);return ok;}
static void broker_retire(struct child *c){struct broker_pair **at=&c->ports;while(*at){struct broker_pair *p=*at;if(worker_port_retirable(p)){*at=p->next;free(p);}else at=&p->next;}}
struct broker_publish_owner {web_workers *w;struct child *c;};
static void broker_packet_publish(void *opaque,void *packet){struct broker_publish_owner *owner=opaque;struct packet *p=packet;struct child *c=owner->c;if(c->tail)c->tail->next=p;else c->tx=p;c->tail=p;}
static struct broker_lease *broker_lease_find(web_workers *w,uint32_t id){for(struct broker_lease *l=w->leases;l;l=l->next)if(l->id==id)return l;return NULL;}
JSValue web_worker_capture(web_workers *w,uint32_t child,uint32_t port,unsigned creator){
    if(!w)return JS_EXCEPTION;struct child *c=find(w,child);struct broker_pair *pair=c?broker_find(c,port,creator):NULL;
    if(!c||c->stopped||c->closing||!pair||pair->closed)return JS_ThrowTypeError(w->ctx,"Worker capture target is inactive");
    uint32_t identifier=worker_port_next_lease(w->leases,w->next_lease);
    if(!identifier||pair->pins==SIZE_MAX)return JS_ThrowRangeError(w->ctx,"Worker capture representation overflow");
    struct broker_lease *lease=calloc(1,sizeof *lease);if(!lease)return JS_ThrowOutOfMemory(w->ctx);
    w->next_lease=lease->id=identifier;lease->child=child;lease->port=port;lease->creator=creator;lease->next=w->leases;w->leases=lease;pair->pins++;return JS_NewUint32(w->ctx,lease->id);
}
void web_worker_release_capture(web_workers *w,uint32_t id){
    if(!w)return;struct broker_lease **at=&w->leases;while(*at&&(*at)->id!=id)at=&(*at)->next;if(!*at)return;
    struct broker_lease *lease=*at;*at=lease->next;struct child *c=find(w,lease->child);struct broker_pair *pair=c?broker_find(c,lease->port,lease->creator):NULL;
    if(pair){if(pair->pins)pair->pins--;struct broker_publish_owner owner={w,c};worker_port_flush(pair,broker_packet_publish,&owner);broker_retire(c);}free(lease);
}
void web_worker_cancel_captures(web_workers *w){if(w)while(w->leases)web_worker_release_capture(w,w->leases->id);}
web_worker_publication *web_worker_prepare_captured(web_workers *w,uint32_t id,unsigned op,uint32_t request,unsigned kind,JSValueConst value,uint32_t capture){
    if(!w)return NULL;JSContext *ctx=w->ctx;struct child *c=find(w,id);
    if(!c||c->stopped||c->closing){JS_ThrowTypeError(ctx,"Worker port destination is inactive");return NULL;}
    struct web_worker_publication *t=calloc(1,sizeof *t);struct packet *p=calloc(1,sizeof *p);
    if(!t||!p){free(t);free(p);JS_ThrowOutOfMemory(ctx);return NULL;}t->owner=w;t->child=c;t->packet=p;
    if(op==NJW_MESSAGE&&kind==NJW_WITH_PORTS){if(capture){JS_ThrowTypeError(ctx,"Capture requires a Port message");goto fail;}if(!broker_metadata(ctx,c,value,0,&t->ports))goto fail;}
    else if(op==NJW_PORT_MESSAGE||op==NJW_PORT_CLOSE||op==NJW_PORT_DRAINED){
        struct broker_pair *pair=kind<=1?broker_find(c,request,kind):NULL;struct broker_lease *lease=capture?broker_lease_find(w,capture):NULL;
        bool pinned=lease&&lease->child==id&&lease->port==request&&lease->creator==kind;
        if(!pair||(capture&&(!pinned||op!=NJW_PORT_MESSAGE))||(op==NJW_PORT_MESSAGE?(!pinned&&pair->closed):op==NJW_PORT_CLOSE?pair->closed:false)){
            JS_ThrowTypeError(ctx,"Worker port is closed, unknown or capture is invalid");goto fail;}
        if(op==NJW_PORT_CLOSE)t->close_pair=pair;if(op==NJW_PORT_DRAINED)t->ack_pair=pair;
    }else{JS_ThrowTypeError(ctx,"Invalid Worker port publication");goto fail;}
    size_t length=0;uint8_t *bytes=JS_WriteObject(ctx,&length,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)goto fail;
    if(length>NJW_MAX_BYTES||!queue_room(w,length)){js_free(ctx,bytes);JS_ThrowRangeError(ctx,"Worker port wire accounting overflow");goto fail;}
    p->data=malloc(length);if(!p->data){js_free(ctx,bytes);JS_ThrowOutOfMemory(ctx);goto fail;}memcpy(p->data,bytes,length);js_free(ctx,bytes);
    p->h=(struct njw_header){NJW_MAGIC,op,w->generation,request,kind,(uint32_t)length};return t;
fail:web_worker_abort(t);return NULL;
}
web_worker_publication *web_worker_prepare(web_workers *w,uint32_t id,unsigned op,uint32_t request,unsigned kind,JSValueConst value){return web_worker_prepare_captured(w,id,op,request,kind,value,0);}
void web_worker_abort(web_worker_publication *t){if(!t)return;broker_free(t->ports);broker_packet_free(t->packet);free(t);}
void web_worker_publish(web_worker_publication *t){
    struct child *c=t->child;struct packet *p=t->packet;struct broker_publish_owner owner={t->owner,c};
    t->owner->queued+=sizeof p->h+p->h.bytes;
    while(t->ports){struct broker_pair *pair=t->ports;t->ports=pair->next;pair->next=c->ports;c->ports=pair;}
    if(t->close_pair){t->close_pair->closed=true;t->close_pair->held_close=p;worker_port_flush(t->close_pair,broker_packet_publish,&owner);}
    else if(t->ack_pair){t->ack_pair->held_ack=p;worker_port_flush(t->ack_pair,broker_packet_publish,&owner);}
    else broker_packet_publish(&owner,p);free(t);
}
static void event(web_workers *w,struct child *c,uint32_t op,JSValueConst value){
    size_t n;uint8_t *bytes=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)return;
    if(n>NJW_MAX_BYTES||!queue_room(w,n)){js_free(w->ctx,bytes);return;}
    struct event *e=calloc(1,sizeof *e);if(!e){js_free(w->ctx,bytes);return;}e->data=malloc(n);if(!e->data){free(e);js_free(w->ctx,bytes);return;}memcpy(e->data,bytes,n);js_free(w->ctx,bytes);
    e->id=c->id;e->h=(struct njw_header){NJW_MAGIC,op,w->generation,0,0,(uint32_t)n};if(w->last)w->last->next=e;else w->events=e;w->last=e;w->queued+=sizeof e->h+n;
}
static void fail(web_workers *w,struct child *c,const char *message){stop(w,c);JSValue v=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,v,"message",JS_NewString(w->ctx,message));JS_SetPropertyStr(w->ctx,v,"terminal",JS_TRUE);event(w,c,NJW_ERROR,v);JS_FreeValue(w->ctx,v);}
static bool spawn_child(web_workers *w,struct child *c){
    web_worker_background();if(active_children>=NJW_MAX_WORKERS){fail(w,c,"Native Worker process count is not representable");return false;}
    int ip[2]={-1,-1},op[2]={-1,-1},nullfd=-1;
    if(pipe(ip)<0||pipe(op)<0||(nullfd=open("/dev/null",O_WRONLY))<0)goto bad;
    int map[3]={ip[0],op[1],nullfd};char *argv[]={"browserjsworker",NULL};c->pid=spawn("/bin/browserjsworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(nullfd);nullfd=-1;if(c->pid<0){c->pid=0;goto bad;}active_children++;
    c->in=ip[1];ip[1]=-1;c->out=op[0];op[0]=-1;if(fcntl(c->in,F_SETFL,O_NONBLOCK)<0||fcntl(c->out,F_SETFL,O_NONBLOCK)<0){fail(w,c,"Worker nonblocking pipe setup failed");return false;}
    JSValue log=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,log,"message",JS_NewString(w->ctx,"native dedicated task; heap grows on demand (no configured ceiling)"));event(w,c,NJW_CONSOLE,log);JS_FreeValue(w->ctx,log);return true;
bad:for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);}if(nullfd>=0)close(nullfd);fail(w,c,"Cannot spawn native dedicated Worker");return false;
}
web_workers *web_worker_new(JSContext *ctx,uint32_t generation,web_worker_loader load,void *opaque){web_workers *w=calloc(1,sizeof *w);if(!w)return NULL;w->ctx=ctx;w->generation=generation;w->callback=JS_UNDEFINED;w->load=load;w->opaque=opaque;return w;}
void web_worker_set_canceler(web_workers *w,web_worker_canceler cancel){if(w)w->cancel=cancel;}
void web_worker_set_callback(web_workers *w,JSValueConst fn){if(!w)return;JS_FreeValue(w->ctx,w->callback);w->callback=JS_DupValue(w->ctx,fn);}
JSValue web_worker_native(web_workers *w,int argc,JSValueConst *argv){
    if(!w)return JS_UNDEFINED;JSContext *ctx=w->ctx;uint32_t op=0,id=0;if(argc<2||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0)return JS_EXCEPTION;
    if(op==0){if(w->next_id==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker identifier space exhausted");
        const char *url=argc>2?JS_ToCString(ctx,argv[2]):NULL;if(!url)return JS_EXCEPTION;struct child *c=calloc(1,sizeof *c);if(!c){JS_FreeCString(ctx,url);return JS_ThrowOutOfMemory(ctx);}size_t name_len=0;const char *name=argc>3?JS_ToCStringLen(ctx,&name_len,argv[3]):"";if(!name){JS_FreeCString(ctx,url);free(c);return JS_EXCEPTION;}if(name_len!=SIZE_MAX)c->name=malloc(name_len+1);if(c->name){memcpy(c->name,name,name_len);c->name[name_len]=0;c->name_len=name_len;}if(argc>3)JS_FreeCString(ctx,name);if(!c->name){JS_FreeCString(ctx,url);free(c);return JS_ThrowOutOfMemory(ctx);}c->id=++w->next_id;c->in=c->out=-1;c->next=w->children;w->children=c;
        bool ok=w->load&&w->load(w->opaque,c->id,0,url,0);JS_FreeCString(ctx,url);if(!ok){w->children=c->next;free(c->name);free(c);return JS_ThrowTypeError(ctx,"Worker source URL rejected or request unavailable");}return JS_NewUint32(ctx,c->id);
    }
    struct child *c=find(w,id);if(!c||c->stopped)return JS_UNDEFINED;
    if(op==4){if(c->parent_port==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker port identifier space exhausted");return JS_NewUint32(ctx,++c->parent_port);}
    if(op==2){stop(w,c);if(!c->pid){struct child **at=&w->children;while(*at&&*at!=c)at=&(*at)->next;if(*at)*at=c->next;free(c->name);free(c);}return JS_UNDEFINED;}
    if(op==1){if(argc<3)return JS_ThrowTypeError(ctx,"Worker message required");if(!serialized(w,c,NJW_MESSAGE,0,0,argv[2],false))return JS_ThrowRangeError(ctx,"Worker message allocation, representation or transport failed");return JS_UNDEFINED;}
    return JS_ThrowTypeError(ctx,"Invalid Worker operation");
}
void web_worker_loaded(web_workers *w,uint32_t id,uint32_t request,int status,const char *url,const char *headers,const void *bytes,size_t n,const char *error){
    if(!w)return;struct child *c=find(w,id);if(!c||c->stopped)return;
    uint32_t kind=0;
    if(request&&status>0&&(!error||!error[0])&&headers){
        /* This second status line is emitted by the trusted HTTP host. Server
           fields contain a colon and cannot manufacture transport metadata. */
        const char *meta=strstr(headers,"\r\nHTTP/Nocturne-Meta cors=");
        unsigned cors=0,redirected=0,opaque=0;
        if(meta&&sscanf(meta+2,"HTTP/Nocturne-Meta cors=%u redirected=%u opaque=%u",&cors,&redirected,&opaque)>=2&&cors<=1&&redirected<=1&&opaque<=1){
            if(cors)kind|=NJW_LOADED_CORS;
            if(redirected)kind|=NJW_LOADED_REDIRECTED;
            if(opaque){
                kind=NJW_LOADED_OPAQUE;
                /* Never place opaque internals in the author's Worker realm,
                   even if its built-in prototypes have been replaced. */
                status=0;url="";headers="";bytes=NULL;n=0;
            }
        }
    }
    if(n>NJW_MAX_BYTES){fail(w,c,"Worker resource length is not representable on the wire");return;}
    JSContext *ctx=w->ctx;JSValue a=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,a,0,JS_NewInt32(ctx,status));JS_SetPropertyUint32(ctx,a,1,JS_NewString(ctx,url?url:""));JS_SetPropertyUint32(ctx,a,2,JS_NewString(ctx,headers?headers:""));JS_SetPropertyUint32(ctx,a,3,JS_NewArrayBufferCopy(ctx,bytes,n));JS_SetPropertyUint32(ctx,a,4,JS_NewString(ctx,error?error:""));JS_SetPropertyUint32(ctx,a,5,JS_NewStringLen(ctx,c->name?c->name:"",c->name_len));
    JS_SetPropertyUint32(ctx,a,6,JS_NewBool(ctx,w->cancel!=NULL));
    bool first=request==0;bool ok=serialized(w,c,first?NJW_START:NJW_LOADED,request,kind,a,first);JS_FreeValue(ctx,a);if(!ok){fail(w,c,"Worker resource allocation, representation or transport failed");return;}if(first)c->started=true;
}
void web_worker_pump(web_workers *w,uint64_t now){
    (void)now;web_worker_background();if(!w)return;
    for(struct child *c=w->children;c;c=c->next){size_t budget=16384;if(c->stopped){if(c->pid){int n=waitpid(c->pid,NULL,WNOHANG);if(n==c->pid||(n<0&&errno==ECHILD)){c->pid=0;if(active_children)active_children--;}}continue;}if(!c->started||c->closing)continue;if(!c->pid&&!spawn_child(w,c))continue;
        struct n_pollfd p[2]={{c->in,N_POLLOUT,0},{c->out,N_POLLIN,0}};if(poll(p,2,0)<0){fail(w,c,"Worker poll failed");continue;}
        while(c->tx&&budget&&(p[0].revents&N_POLLOUT)){struct packet *a=c->tx;bool header=a->at<sizeof a->h;size_t left=header?sizeof a->h-a->at:sizeof a->h+a->h.bytes-a->at;size_t take=left<budget?left:budget;const uint8_t *src=header?(uint8_t *)&a->h+a->at:a->data+a->at-sizeof a->h;ssize_t n=write(c->in,src,take);if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){fail(w,c,"Worker command pipe closed");break;}a->at+=n;budget-=n;if(a->at==sizeof a->h+a->h.bytes){c->tx=a->next;if(!c->tx)c->tail=NULL;release_packet(w,a);}}
        while(!c->stopped&&budget&&(p[1].revents&(N_POLLIN|N_POLLHUP))){bool header=c->head_at<sizeof c->rx;
            /* Read only this tick's finite byte budget. Once the header is
               complete, reserve its actual size; never allocate a maximum
               payload in advance or reject a count-based queue quota. */
            size_t left=header?sizeof c->rx-c->head_at:c->rx.bytes-c->data_at;size_t take=left<budget?left:budget;ssize_t n=read(c->out,header?(void *)((uint8_t *)&c->rx+c->head_at):(void *)(c->data+c->data_at),take);if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){fail(w,c,"Worker process exited");break;}budget-=n;
            if(header){c->head_at+=n;if(c->head_at==sizeof c->rx){bool port=c->rx.op==NJW_PORT_MESSAGE||c->rx.op==NJW_PORT_CLOSE||c->rx.op==NJW_PORT_DRAINED;
                bool valid_kind=port?c->rx.kind<=1:c->rx.op==NJW_MESSAGE?(c->rx.kind==0||c->rx.kind==NJW_WITH_PORTS):c->rx.op==NJW_LOAD?njw_load_kind_valid(c->rx.kind):c->rx.op==NJW_CANCEL?c->rx.kind==0&&c->rx.request!=0:c->rx.kind<=2;
                if(c->rx.magic!=NJW_MAGIC||c->rx.generation!=w->generation||!c->rx.bytes||(c->rx.op!=NJW_MESSAGE&&c->rx.op!=NJW_ERROR&&c->rx.op!=NJW_LOAD&&c->rx.op!=NJW_CANCEL&&c->rx.op!=NJW_CLOSE&&c->rx.op!=NJW_CONSOLE&&!port)||!valid_kind||!queue_room(w,c->rx.bytes)){fail(w,c,"Invalid Worker response or receive size overflow");break;}c->data=malloc(c->rx.bytes);if(!c->data){fail(w,c,"Worker receive allocation failed");break;}c->rx_charge=sizeof c->rx+c->rx.bytes;w->queued+=c->rx_charge;}}else c->data_at+=n;
            if(c->head_at==sizeof c->rx&&c->data_at==c->rx.bytes){struct event *e=calloc(1,sizeof *e);if(!e){fail(w,c,"Worker event allocation failed");break;}e->id=c->id;e->h=c->rx;e->data=c->data;c->data=NULL;c->head_at=c->data_at=0;c->rx_charge=0;if(w->last)w->last->next=e;else w->events=e;w->last=e;if(e->h.op==NJW_CLOSE){c->closing=true;break;}}
        }
    }
    struct child **at=&w->children;while(*at){struct child *c=*at;if(c->stopped&&!c->pid){*at=c->next;free(c->name);free(c);}else at=&c->next;}
}
bool web_worker_runnable(const web_workers *w){return w&&w->events;}
static bool console_prefix(JSContext *ctx,JSValueConst value,const struct child *c){
    JSValue message=JS_GetPropertyStr(ctx,value,"message");
    if(JS_IsException(message))return false;
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,message);
    JS_FreeValue(ctx,message);if(!text)return false;
    /* Only the two representable numeric identifiers are bounded, not the
       author message. Retain the complete UTF-8 span (including embedded NUL). */
    char prefix[64];int n=snprintf(prefix,sizeof prefix,"[Worker %u PID %d] ",c->id,c->pid);
    if(n<0||(size_t)n>=sizeof prefix||length>SIZE_MAX-(size_t)n-1){
        JS_FreeCString(ctx,text);JS_ThrowRangeError(ctx,"Worker console length is not representable");return false;
    }
    size_t total=(size_t)n+length;char *joined=malloc(total+1);
    if(!joined){JS_FreeCString(ctx,text);JS_ThrowOutOfMemory(ctx);return false;}
    memcpy(joined,prefix,(size_t)n);memcpy(joined+n,text,length);joined[total]=0;JS_FreeCString(ctx,text);
    JSValue result=JS_NewStringLen(ctx,joined,total);free(joined);
    if(JS_IsException(result))return false;
    return JS_SetPropertyStr(ctx,value,"message",result)>=0;
}
bool web_worker_run_one(web_workers *w){
    if(!w||!w->events)return false;struct event *e=w->events;w->events=e->next;if(!w->events)w->last=NULL;w->queued-=sizeof e->h+e->h.bytes;struct child *c=find(w,e->id);
    if((c&&!c->stopped)||e->h.op==NJW_ERROR){JSContext *ctx=w->ctx;JSValue v=JS_ReadObject(ctx,e->data,e->h.bytes,JS_READ_OBJ_REFERENCE);if(!JS_IsException(v)){
        if(c&&e->h.op==NJW_MESSAGE&&e->h.kind==NJW_WITH_PORTS){struct broker_pair *ports=NULL;if(!broker_metadata(ctx,c,v,1,&ports)){JS_FreeValue(ctx,v);goto done;}while(ports){struct broker_pair*p=ports;ports=p->next;p->next=c->ports;c->ports=p;}}
        if(c&&(e->h.op==NJW_PORT_MESSAGE||e->h.op==NJW_PORT_CLOSE||e->h.op==NJW_PORT_DRAINED)){
            struct broker_pair*p=broker_find(c,e->h.request,e->h.kind);if(!p||(e->h.op==NJW_PORT_MESSAGE&&p->sealed)){JS_FreeValue(ctx,v);goto done;}
            if(e->h.op!=NJW_PORT_MESSAGE){p->closed=p->sealed=true;
                if(e->h.op==NJW_PORT_CLOSE){web_worker_publication *ack=web_worker_prepare(w,c->id,NJW_PORT_DRAINED,p->id,p->creator,JS_NULL);if(!ack){JS_FreeValue(ctx,v);fail(w,c,"Worker close acknowledgement allocation failed");goto done;}web_worker_publish(ack);}
                broker_retire(c);
            }
        }
        if(e->h.op==NJW_LOAD){const char *url=JS_IsString(v)?JS_ToCString(ctx,v):NULL;bool ok=e->h.request&&url&&w->load&&w->load(w->opaque,c->id,e->h.request,url,e->h.kind);JS_FreeCString(ctx,url);if(!ok)web_worker_loaded(w,c->id,e->h.request,0,"","",NULL,0,"Worker resource URL rejected");}
        else if(e->h.op==NJW_CANCEL){if(!JS_IsNull(v)||!w->cancel)fail(w,c,"Worker host cannot cancel this resource");else w->cancel(w->opaque,c->id,e->h.request);}
        else {if(e->h.op!=NJW_CONSOLE||!c||console_prefix(ctx,v,c)){JSValue args[]={JS_NewUint32(ctx,e->id),JS_NewUint32(ctx,e->h.op),v,JS_NewUint32(ctx,e->h.request),JS_NewUint32(ctx,e->h.kind)};JSValue r=JS_Call(ctx,w->callback,JS_UNDEFINED,5,args);JS_FreeValue(ctx,r);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);JS_FreeValue(ctx,args[3]);JS_FreeValue(ctx,args[4]);if(e->h.op==NJW_CLOSE&&c)stop(w,c);}}
        JS_FreeValue(ctx,v);
    }}done:free(e->data);free(e);return true;
}
int64_t web_worker_deadline(const web_workers *w,uint64_t now){if(!w)return -1;if(w->events)return (int64_t)now;for(struct child *c=w->children;c;c=c->next)if(c->pid||(!c->stopped&&c->started))return (int64_t)now+10;return -1;}
void web_worker_free(web_workers *w){if(!w)return;while(w->children){struct child *c=w->children;w->children=c->next;stop(w,c);if(c->pid){c->next=retired;retired=c;}else {free(c->name);free(c);}}while(w->events){struct event *e=w->events;w->events=e->next;free(e->data);free(e);}while(w->leases){struct broker_lease*l=w->leases;w->leases=l->next;free(l);}JS_FreeValue(w->ctx,w->callback);free(w);}
