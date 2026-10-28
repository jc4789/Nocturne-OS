/* Browser-side, nonblocking Nocturne native dedicated-worker transport. */
#include "js_worker.h"
#include "js_worker_wire.h"
#include "js_atomics.h"
#include <nocturne.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* One record is one channel message: data keeps room for the header in front
 * of the payload, so a send is a single message without another copy. */
struct packet {struct packet *next;struct njw_header h;uint8_t *data;struct web_attachments attach;size_t attach_sent;};
struct resource {struct resource *next;uint32_t id;};
#include "js_worker_port_lifetime.h"
/* channel carries both directions; app is a dedicated worker's own task (a
 * shared server outlives its creator and is not held). watched: the channel
 * is in the page's wait queue, so a record wakes the page without a timer. */
struct child {struct child *next,*shared_next;web_workers *owner;char *name,*creator_policy,*url,*origin;size_t name_len;uint32_t id,generation,parent_port;n_handle app,channel;int64_t task;int creator_referrer_policy;bool started,stopped,closing,inherit_policy,shared,isolated,watched;struct web_inbox inbox;struct packet *tx,*tail;struct broker_pair *ports;struct resource *resources;};
#define WEB_WORKER_TOKEN (UINT64_C(0x4a) << 56)
#define WORKER_PROGRAM N_FOLDER_SYSTEM "\\browserjsworker"
struct shared_ref {struct shared_ref *next;struct child *child;};
struct event {struct event *next;uint32_t id;struct njw_header h;uint8_t *data;struct web_inbox inbox;bool from_child;};
struct web_workers {struct web_workers *next;JSContext *ctx;JSValue callback;uint32_t generation;web_worker_loader load;web_worker_requester request;web_worker_resumer resume;web_worker_canceler cancel;void *opaque;char *creator_policy,*origin;int creator_referrer_policy;bool isolated;web_worker_reporter reporter;void *report_opaque;struct child *children;struct shared_ref *shared;struct event *events,*last;size_t queued;uint32_t next_lease;struct broker_lease *leases;};
static struct child *shared_children;
static web_workers *managers;
static uint32_t next_worker_id;
struct web_worker_publication {web_workers *owner;struct child *child;struct packet *packet;struct broker_pair *ports,*close_pair,*ack_pair;};
static struct broker_pair *broker_find(struct child *c,uint32_t id,uint32_t creator){return worker_port_find(c->ports,id,creator);}
static void broker_packet_free(void *opaque){struct packet *p=opaque;if(p){web_attachments_release(&p->attach);free(p->data);free(p);}}
static size_t broker_free(struct broker_pair *p){size_t released=0;while(p){struct broker_pair *n=p->next;struct packet*a=p->held_close,*b=p->held_ack;if(a)released+=sizeof a->h+a->h.bytes;if(b)released+=sizeof b->h+b->h.bytes;broker_packet_free(a);broker_packet_free(b);free(p);p=n;}return released;}
static bool broker_metadata(JSContext *ctx,struct child *c,JSValueConst value,unsigned creator,struct broker_pair **result){return worker_port_metadata(ctx,value,creator,c->ports,creator?UINT32_MAX:c->parent_port,result);}
static char *policy_copy(const char *headers){size_t n=strlen(headers?headers:"");if(n==SIZE_MAX)return NULL;char *copy=malloc(n+1);if(copy)memcpy(copy,headers?headers:"",n+1);return copy;}
static void child_free(struct child *c){if(c){struct child **at=&shared_children;while(*at&&*at!=c)at=&(*at)->shared_next;if(*at)*at=c->shared_next;for(web_workers*w=managers;w;w=w->next){struct shared_ref**p=&w->shared;while(*p){if((*p)->child==c){struct shared_ref*r=*p;*p=r->next;free(r);}else p=&(*p)->next;}}while(c->resources){struct resource*r=c->resources;c->resources=r->next;free(r);}if(c->app>=0)n_handle_close(c->app);web_inbox_close(&c->inbox);free(c->creator_policy);free(c->name);free(c->url);free(c->origin);free(c);}}
static bool resource_field(JSContext *ctx,JSValueConst record,uint32_t index,JSValue value){
    if(JS_IsException(value))return false;
    /* Resource completion is not an author-JS task. Bypass prototype setters. */
    return JS_DefinePropertyValueUint32(ctx,record,index,value,JS_PROP_C_W_E)>=0;
}
static struct child *retired;
static unsigned active_children;
/* True once a dedicated worker's task has ended; its handle is then released. */
static bool child_ended(struct child *c,int *status){
    if(c->app<0)return true;if(n_app_result(c->app,0,status)==-N_ERR_NOT_READY)return false;
    n_handle_close(c->app);c->app=-1;if(active_children)active_children--;return true;
}
void web_worker_background(void){struct child **p=&retired;while(*p){struct child *r=*p;if(child_ended(r,NULL)){*p=r->next;child_free(r);}else p=&r->next;}}
/* Whether a record sent now would be queued at once (or find the peer gone):
 * the page task never waits on a busy worker. One private probe queue. */
static bool channel_sendable(n_handle h){
    static n_handle probe=-1;if(probe<0&&(probe=n_events_create())<0)return false;
    if(n_events_watch(probe,h,N_EVENT_SENDABLE|N_EVENT_CLOSED,1)<0)return false;
    struct n_event e;n_result ready=n_events_wait(probe,&e,1,0);n_events_unwatch(probe,1);return ready>0;
}
int64_t web_worker_background_deadline(uint64_t now){return retired?(int64_t)now+10:-1;}
static struct child *find(web_workers *w,uint32_t id){for(struct child *c=w->children;c;c=c->next)if(c->id==id)return c;for(struct shared_ref*r=w->shared;r;r=r->next)if(r->child->id==id)return r->child;return NULL;}
static bool queue_room(const web_workers *w,size_t length){return length<=SIZE_MAX-sizeof(struct njw_header)&&length+sizeof(struct njw_header)<=SIZE_MAX-w->queued;}
static void release_packet(web_workers *w,struct packet *p){w->queued-=sizeof p->h+p->h.bytes;web_attachments_release(&p->attach);free(p->data);free(p);}
static void stop(web_workers *w,struct child *c){
    w=c->owner;
    if(c->stopped)return;c->stopped=true;
    if(w->cancel)w->cancel(w->opaque,c->id,0);
    if(c->channel>=0)n_handle_close(c->channel);c->channel=-1;c->watched=false;web_inbox_close(&c->inbox);
    if(c->app>=0){n_app_stop(c->app,0);child_ended(c,NULL);}
    while(c->tx){struct packet *p=c->tx;c->tx=p->next;release_packet(w,p);}c->tail=NULL;
    w->queued-=broker_free(c->ports);c->ports=NULL;
}
static bool queue(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,const void *data,size_t length,bool first){
    w=c->owner;
    if(length>NJW_MAX_BYTES||!queue_room(w,length)||c->stopped)return false;
    struct packet *p=calloc(1,sizeof *p);if(!p)return false;p->data=malloc(sizeof p->h+length);if(!p->data){free(p);return false;}if(length)memcpy(p->data+sizeof p->h,data,length);
    p->h=(struct njw_header){NJW_MAGIC,op,c->generation,request,kind,(uint32_t)length};
    if(first){p->next=c->tx;c->tx=p;if(!c->tail)c->tail=p;}else{if(c->tail)c->tail->next=p;else c->tx=p;c->tail=p;}w->queued+=sizeof p->h+length;return true;
}
static bool serialized(web_workers *w,struct child *c,uint32_t op,uint32_t request,uint32_t kind,JSValueConst value,bool first){size_t n=0;uint8_t *p=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!p)return false;bool ok=queue(w,c,op,request,kind,p,n,first);js_free(w->ctx,p);return ok;}
static void broker_retire(struct child *c){struct broker_pair **at=&c->ports;while(*at){struct broker_pair *p=*at;if(worker_port_retirable(p)){*at=p->next;free(p);}else at=&p->next;}}
struct broker_publish_owner {web_workers *w;struct child *c;};
static void broker_packet_publish(void *opaque,void *packet){struct broker_publish_owner *owner=opaque;struct packet *p=packet;struct child *c=owner->c;if(c->tail)c->tail->next=p;else c->tx=p;c->tail=p;}
static struct broker_lease *broker_lease_find(web_workers *w,uint32_t id){for(struct broker_lease *l=w->leases;l;l=l->next)if(l->id==id)return l;return NULL;}
JSValue web_worker_capture(web_workers *w,uint32_t child,uint32_t port,unsigned creator){
    if(!w)return JS_EXCEPTION;struct child *c=find(w,child);struct broker_pair *pair=c?broker_find(c,port,creator):NULL;
    if(!c||c->stopped||c->closing||!pair||pair->closed||(pair->client&&pair->client!=w))return JS_ThrowTypeError(w->ctx,"Worker capture target is inactive");
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
    if((op==NJW_MESSAGE||op==NJW_CONNECT)&&kind==NJW_WITH_PORTS){if(capture){JS_ThrowTypeError(ctx,"Capture requires a Port message");goto fail;}if((op==NJW_CONNECT)!=c->shared){JS_ThrowTypeError(ctx,"Worker connection type is invalid");goto fail;}if(!broker_metadata(ctx,c,value,0,&t->ports))goto fail;
        /* Shared regions of the message travel as attachments ahead of it. */
        if(!web_attachments_extract(ctx,value,&p->attach))goto fail;
        for(struct broker_pair*p=t->ports;p;p=p->next)p->client=w;if(op==NJW_CONNECT&&JS_DefinePropertyValueUint32(ctx,value,2,JS_NewString(ctx,c->url),JS_PROP_C_W_E)<0)goto fail;}
    else if(op==NJW_PORT_MESSAGE||op==NJW_PORT_CLOSE||op==NJW_PORT_DRAINED){
        struct broker_pair *pair=kind<=1?broker_find(c,request,kind):NULL;struct broker_lease *lease=capture?broker_lease_find(w,capture):NULL;
        bool pinned=lease&&lease->child==id&&lease->port==request&&lease->creator==kind;
        if(!pair||(pair->client&&pair->client!=w)||(capture&&(!pinned||op!=NJW_PORT_MESSAGE))||(op==NJW_PORT_MESSAGE?(!pinned&&pair->closed):op==NJW_PORT_CLOSE?pair->closed:false)){
            JS_ThrowTypeError(ctx,"Worker port is closed, unknown or capture is invalid");goto fail;}
        if(op==NJW_PORT_CLOSE)t->close_pair=pair;if(op==NJW_PORT_DRAINED)t->ack_pair=pair;
    }else{JS_ThrowTypeError(ctx,"Invalid Worker port publication");goto fail;}
    size_t length=0;uint8_t *bytes=JS_WriteObject(ctx,&length,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)goto fail;
    if(length>NJW_MAX_BYTES||!queue_room(c->owner,length)){js_free(ctx,bytes);JS_ThrowRangeError(ctx,"Worker port wire accounting overflow");goto fail;}
    p->data=malloc(sizeof p->h+length);if(!p->data){js_free(ctx,bytes);JS_ThrowOutOfMemory(ctx);goto fail;}memcpy(p->data+sizeof p->h,bytes,length);js_free(ctx,bytes);
    p->h=(struct njw_header){NJW_MAGIC,op,c->generation,request,kind,(uint32_t)length};return t;
fail:web_worker_abort(t);return NULL;
}
web_worker_publication *web_worker_prepare(web_workers *w,uint32_t id,unsigned op,uint32_t request,unsigned kind,JSValueConst value){return web_worker_prepare_captured(w,id,op,request,kind,value,0);}
void web_worker_abort(web_worker_publication *t){if(!t)return;broker_free(t->ports);broker_packet_free(t->packet);free(t);}
void web_worker_publish(web_worker_publication *t){
    struct child *c=t->child;struct packet *p=t->packet;struct broker_publish_owner owner={t->owner,c};
    c->owner->queued+=sizeof p->h+p->h.bytes;
    while(t->ports){struct broker_pair *pair=t->ports;t->ports=pair->next;pair->next=c->ports;c->ports=pair;}
    if(t->close_pair){t->close_pair->closed=true;t->close_pair->held_close=p;worker_port_flush(t->close_pair,broker_packet_publish,&owner);}
    else if(t->ack_pair){t->ack_pair->held_ack=p;worker_port_flush(t->ack_pair,broker_packet_publish,&owner);}
    else broker_packet_publish(&owner,p);free(t);
}
static void route_event(web_workers*,struct child*,struct event*);
static void event(web_workers *w,struct child *c,uint32_t op,JSValueConst value){
    size_t n;uint8_t *bytes=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)return;
    if(n>NJW_MAX_BYTES||!queue_room(w,n)){js_free(w->ctx,bytes);return;}
    struct event *e=calloc(1,sizeof *e);if(!e){js_free(w->ctx,bytes);return;}e->data=malloc(n);if(!e->data){free(e);js_free(w->ctx,bytes);return;}memcpy(e->data,bytes,n);js_free(w->ctx,bytes);
    e->id=c->id;e->h=(struct njw_header){NJW_MAGIC,op,c->generation,0,0,(uint32_t)n};w->queued+=sizeof e->h+n;route_event(w,c,e);
}
static void append_event(web_workers*w,struct event*e){if(w->last)w->last->next=e;else w->events=e;w->last=e;w->queued+=sizeof e->h+e->h.bytes;}
/* Route raw clone bytes to the receiving realm before deserialization. No
 * author callbacks execute during the transport pump. */
static void route_event(web_workers*w,struct child*c,struct event*e){
    if(c->shared&&(e->h.op==NJW_PORT_MESSAGE||e->h.op==NJW_PORT_CLOSE||e->h.op==NJW_PORT_DRAINED)){
        struct broker_pair*p=broker_find(c,e->h.request,e->h.kind);web_workers*client=p?p->client:NULL;
        if(client&&client!=w){w->queued-=sizeof e->h+e->h.bytes;append_event(client,e);return;}
    }else if(c->shared&&(e->h.op==NJW_ERROR||e->h.op==NJW_CLOSE)){
        for(web_workers*client=managers;client;client=client->next){if(client==w)continue;bool uses=false;for(struct shared_ref*r=client->shared;r;r=r->next)if(r->child==c){uses=true;break;}if(!uses)continue;
            struct event*copy=calloc(1,sizeof*copy);if(!copy)continue;copy->data=malloc(e->h.bytes);if(!copy->data){free(copy);continue;}copy->id=e->id;copy->h=e->h;memcpy(copy->data,e->data,e->h.bytes);append_event(client,copy);
        }
    }
    if(w->last)w->last->next=e;else w->events=e;w->last=e;
}
static void fail(web_workers *w,struct child *c,const char *message){stop(w,c);JSValue v=JS_NewObject(w->ctx);JS_DefinePropertyValueStr(w->ctx,v,"message",JS_NewString(w->ctx,message),JS_PROP_C_W_E);JS_DefinePropertyValueStr(w->ctx,v,"terminal",JS_TRUE,JS_PROP_C_W_E);event(w,c,NJW_ERROR,v);JS_FreeValue(w->ctx,v);}
/* A dedicated task closed its channel: take its result now when it has ended
   so the error names its exit status (an abort, a fault or a plain exit). */
static void child_exited(web_workers *w,struct child *c){
    int status=0;char message[64]="Worker process exited";
    if(c->app>=0&&child_ended(c,&status))snprintf(message,sizeof message,"Worker process exited (status %d)",status);
    fail(w,c,message);
}
static bool spawn_child(web_workers *w,struct child *c){
    web_worker_background();if(active_children>=NJW_MAX_WORKERS){fail(w,c,"Native Worker process count is not representable");return false;}
    n_handle pair[2]={-1,-1};if(n_channel_pair(pair)<0){fail(w,c,"Cannot create native dedicated Worker channel");return false;}
    /* Records travel both ways on one channel. The worker reports a runtime
       abort on the page owner's diagnostic stream, else in the system log. */
    const char *args[]={"browserjsworker"};int32_t handles[]={pair[1],pair[1],2};
    struct n_launch launch={.arguments=args,.argument_count=1,.handles=handles,.handle_count=3};
    n_handle app=n_app_launch(WORKER_PROGRAM,&launch);
    if(app==-N_ERR_NO_HANDLE){handles[2]=-1;app=n_app_launch(WORKER_PROGRAM,&launch);}
    n_handle_close(pair[1]);
    if(app<0){n_handle_close(pair[0]);fail(w,c,"Cannot spawn native dedicated Worker");return false;}
    c->app=app;c->task=n_app_id(app);c->channel=pair[0];active_children++;
    JSValue log=JS_NewObject(w->ctx);JS_SetPropertyStr(w->ctx,log,"message",JS_NewString(w->ctx,"native dedicated task; heap grows on demand (no configured ceiling)"));event(w,c,NJW_CONSOLE,log);JS_FreeValue(w->ctx,log);return true;
}
/* Native full-key rendezvous. No filesystem packet persistence or hash-only
 * authority. Every returned duplex endpoint has its own queue. */
static bool shared_transport(web_workers*w,struct child*c,bool*created){
    (void)w;const char*identity_url=c->name_len?"":c->url;size_t a=strlen(c->origin),b=strlen(identity_url),n=c->name_len;
    if(a>SIZE_MAX-b-96||a+b+96>SIZE_MAX-n||a+b+96+n>SIZE_MAX-n)return false;
    size_t capacity=a+b+96+n*2;char*key=malloc(capacity);if(!key)return false;
    int prefix=snprintf(key,capacity,"Nocturne.SharedWorker:%zu:%s:%zu:%s:%zu:",a,c->origin,b,identity_url,n);
    if(prefix<0||(size_t)prefix>=capacity){free(key);return false;}
    static const char hex[]="0123456789abcdef";size_t at=(size_t)prefix;for(size_t i=0;i<n;i++){unsigned v=(unsigned char)c->name[i];key[at++]=hex[v>>4];key[at++]=hex[v&15];}key[at]=0;
    n_handle listener=n_channel_listen(key);*created=listener>=0;
    if(listener<0&&listener!=-N_ERR_EXISTS){free(key);return false;}
    /* The server's only handle is its listener: no input, output or log. It
       ends itself once every connected owner has gone. */
    if(*created){const char*args[]={"browserjsworker","--shared-server",c->url};const int32_t handles[]={listener};
        struct n_launch launch={.arguments=args,.argument_count=3,.handles=handles,.handle_count=1};
        n_handle app=n_app_launch(WORKER_PROGRAM,&launch);if(app<0){n_handle_close(listener);free(key);return false;}n_handle_close(app);}
    n_handle channel=n_channel_connect(key);if(listener>=0)n_handle_close(listener);free(key);if(channel<0)return false;
    c->channel=channel;return true;
}
web_workers *web_worker_new(JSContext *ctx,uint32_t generation,web_worker_loader load,void *opaque){web_workers *w=calloc(1,sizeof *w);if(!w)return NULL;w->ctx=ctx;w->generation=generation;w->callback=JS_UNDEFINED;w->load=load;w->opaque=opaque;w->next=managers;managers=w;return w;}
bool web_worker_set_policy(web_workers *w,const char *headers,int referrer_policy){if(!w||referrer_policy<0||referrer_policy>8)return false;char *copy=policy_copy(headers);if(!copy)return false;free(w->creator_policy);w->creator_policy=copy;w->creator_referrer_policy=referrer_policy;return true;}
bool web_worker_set_origin(web_workers*w,const char*origin){if(!w)return false;char*copy=policy_copy(origin);if(!copy)return false;free(w->origin);w->origin=copy;return true;}
void web_worker_set_isolated(web_workers*w,bool isolated){if(w)w->isolated=isolated;}
void web_worker_set_reporter(web_workers *w,web_worker_reporter reporter,void *opaque){if(w){w->reporter=reporter;w->report_opaque=opaque;}}
void web_worker_set_canceler(web_workers *w,web_worker_canceler cancel){if(w)w->cancel=cancel;}
void web_worker_set_requester(web_workers *w,web_worker_requester request){if(w)w->request=request;}
void web_worker_set_resumer(web_workers *w,web_worker_resumer resume){if(w)w->resume=resume;}
void web_worker_set_callback(web_workers *w,JSValueConst fn){if(!w)return;JS_FreeValue(w->ctx,w->callback);w->callback=JS_DupValue(w->ctx,fn);}
JSValue web_worker_native(web_workers *w,int argc,JSValueConst *argv){
    if(!w)return JS_UNDEFINED;JSContext *ctx=w->ctx;uint32_t op=0,id=0;if(argc<2||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0)return JS_EXCEPTION;
    if(op==0||op==5){
        if(next_worker_id==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker identifier space exhausted");
        if(op==5&&(!w->origin||!w->origin[0]||!strcmp(w->origin,"null")))return JS_ThrowTypeError(ctx,"SecurityError: SharedWorker requires a non-opaque origin");
        const char *url=argc>2?JS_ToCString(ctx,argv[2]):NULL;if(!url)return JS_EXCEPTION;
        size_t name_len=0;const char *name=argc>3?JS_ToCStringLen(ctx,&name_len,argv[3]):"";if(!name){JS_FreeCString(ctx,url);return JS_EXCEPTION;}
        if(op==5)for(struct child*c=shared_children;c;c=c->shared_next){
            if(c->stopped||c->closing||strcmp(c->origin,w->origin)||c->name_len!=name_len||memcmp(c->name,name,name_len))continue;
            /* Empty names identify by URL; a reused nonempty name must not
             * silently create a second realm for a different script. */
            if(strcmp(c->url,url)){if(!name_len)continue;JS_FreeCString(ctx,url);if(argc>3)JS_FreeCString(ctx,name);return JS_ThrowTypeError(ctx,"SharedWorker name is already associated with a different script URL");}
            bool present=false;for(struct shared_ref*r=w->shared;r;r=r->next)if(r->child==c){present=true;break;}
            if(!present){struct shared_ref*r=malloc(sizeof*r);if(!r){JS_FreeCString(ctx,url);if(argc>3)JS_FreeCString(ctx,name);return JS_ThrowOutOfMemory(ctx);}*r=(struct shared_ref){w->shared,c};w->shared=r;}
            JS_FreeCString(ctx,url);if(argc>3)JS_FreeCString(ctx,name);return JS_NewUint32(ctx,c->id);
        }
        struct child*c=calloc(1,sizeof*c);struct shared_ref*ref=op==5?malloc(sizeof*ref):NULL;
        if(!c||(op==5&&!ref)){free(c);free(ref);JS_FreeCString(ctx,url);if(argc>3)JS_FreeCString(ctx,name);return JS_ThrowOutOfMemory(ctx);}
        c->app=c->channel=-1;if(name_len!=SIZE_MAX)c->name=malloc(name_len+1);
        if(c->name){memcpy(c->name,name,name_len);c->name[name_len]=0;c->name_len=name_len;}
        if(argc>3)JS_FreeCString(ctx,name);
        c->creator_policy=policy_copy(w->creator_policy);c->url=policy_copy(url);c->origin=policy_copy(w->origin);
        c->creator_referrer_policy=w->creator_referrer_policy;
        if(!c->name||!c->creator_policy||!c->url||!c->origin){JS_FreeCString(ctx,url);free(ref);child_free(c);return JS_ThrowOutOfMemory(ctx);}
        c->shared=op==5;c->owner=w;c->isolated=w->isolated&&op!=5;c->generation=w->generation;c->inherit_policy=!strncasecmp(url,"blob:",5)||!strncasecmp(url,"data:",5)||!strncasecmp(url,"about:",6);c->id=++next_worker_id;c->next=w->children;w->children=c;
        if(c->shared){*ref=(struct shared_ref){w->shared,c};w->shared=ref;c->shared_next=shared_children;shared_children=c;}
        bool created=true,ok=!c->shared||shared_transport(w,c,&created);
        if(ok&&created)ok=w->load&&w->load(w->opaque,c->id,0,url,0);else if(ok)c->started=true;
        JS_FreeCString(ctx,url);if(!ok){w->children=c->next;stop(w,c);child_free(c);return JS_ThrowTypeError(ctx,"Worker source URL rejected or native endpoint unavailable");}return JS_NewUint32(ctx,c->id);
    }
    struct child *c=find(w,id);if(!c||c->stopped)return JS_UNDEFINED;
    if(op==4){if(c->parent_port==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker port identifier space exhausted");return JS_NewUint32(ctx,++c->parent_port);}
    if(op==2){if(c->shared)return JS_ThrowTypeError(ctx,"SharedWorker has no terminate operation");stop(w,c);if(c->app<0){struct child **at=&w->children;while(*at&&*at!=c)at=&(*at)->next;if(*at)*at=c->next;child_free(c);}return JS_UNDEFINED;}
    if(op==1){if(argc<3)return JS_ThrowTypeError(ctx,"Worker message required");if(!serialized(w,c,NJW_MESSAGE,0,0,argv[2],false))return JS_ThrowRangeError(ctx,"Worker message allocation, representation or transport failed");return JS_UNDEFINED;}
    return JS_ThrowTypeError(ctx,"Invalid Worker operation");
}
static void resource_retire(struct child *c,uint32_t request){
    if(request){struct resource**at=&c->resources;while(*at&&(*at)->id!=request)at=&(*at)->next;if(*at){struct resource*r=*at;*at=r->next;free(r);}}
}
/* The NJW_LOADED flags of a response's header block, from the transport's own line. An opaque
 * response's internals are blanked here: never place them in the author's Worker realm, even if
 * its built-in prototypes have been replaced. */
static uint32_t response_kind(int *status,const char **url,const char **headers,const void **bytes,size_t *n){
    uint32_t kind=0;
    /* This second status line is emitted by the trusted HTTP host. Server
       fields contain a colon and cannot manufacture transport metadata. */
    const char *meta=*headers?strstr(*headers,"\r\nHTTP/Nocturne-Meta cors="):NULL;
    unsigned cors=0,redirected=0,opaque=0;
    if(meta&&sscanf(meta+2,"HTTP/Nocturne-Meta cors=%u redirected=%u opaque=%u",&cors,&redirected,&opaque)>=2&&cors<=1&&redirected<=1&&opaque<=1){
        if(cors)kind|=NJW_LOADED_CORS;
        if(redirected)kind|=NJW_LOADED_REDIRECTED;
        if(opaque){
            const char *end=strstr(meta+2,"\r\n"),*at=strstr(meta+2," resource=");unsigned bits=2;
            if(!at||!end||at>end||sscanf(at+10,"%u",&bits)!=1||bits>7)bits=2;
            kind=NJW_LOADED_OPAQUE|(bits<<NJW_LOADED_RESOURCE_SHIFT);
            *status=0;*url="";*headers="";*bytes=NULL;*n=0;
        }
    }
    return kind;
}
void web_worker_loaded(web_workers *w,uint32_t id,uint32_t request,int status,const char *url,const char *headers,const void *bytes,size_t n,const char *error,JSValueConst timing){
    if(!w)return;struct child *c=find(w,id);if(!c||c->stopped)return;
    resource_retire(c,request);
    uint32_t kind=0;
    if(request&&status>0&&(!error||!error[0])&&headers)kind=response_kind(&status,&url,&headers,&bytes,&n);
    if(n>NJW_MAX_BYTES){fail(w,c,"Worker resource length is not representable on the wire");return;}
    JSContext *ctx=w->ctx;JSValue a=JS_NewArray(ctx);
    bool filled=!JS_IsException(a) && resource_field(ctx,a,0,JS_NewInt32(ctx,status)) &&
        resource_field(ctx,a,1,JS_NewString(ctx,url?url:"")) && resource_field(ctx,a,2,JS_NewString(ctx,headers?headers:"")) &&
        resource_field(ctx,a,3,JS_NewArrayBufferCopy(ctx,bytes,n)) && resource_field(ctx,a,4,JS_NewString(ctx,error?error:"")) &&
        resource_field(ctx,a,5,JS_NewStringLen(ctx,c->name?c->name:"",c->name_len)) && resource_field(ctx,a,6,JS_NewBool(ctx,w->cancel!=NULL));
    bool first=request==0;
    if(!first&&JS_IsObject(timing))filled=filled&&resource_field(ctx,a,NJW_LOADED_TIMING,JS_DupValue(ctx,timing));
    if(first)filled=filled && resource_field(ctx,a,NJW_START_CREATOR_POLICY,JS_NewString(ctx,c->creator_policy?c->creator_policy:"")) &&
        resource_field(ctx,a,NJW_START_INHERIT_POLICY,JS_NewBool(ctx,c->inherit_policy)) && resource_field(ctx,a,NJW_START_SHARED,JS_NewBool(ctx,c->shared)) &&
        resource_field(ctx,a,NJW_START_CREATOR_ORIGIN,JS_NewString(ctx,c->origin?c->origin:"")) &&
        resource_field(ctx,a,NJW_START_CREATOR_REFERRER_POLICY,JS_NewInt32(ctx,c->creator_referrer_policy)) &&
        resource_field(ctx,a,NJW_START_CROSS_ORIGIN_ISOLATED,JS_NewBool(ctx,c->isolated)) &&
        resource_field(ctx,a,NJW_START_STREAMING,JS_NewBool(ctx,w->resume!=NULL));
    bool ok=filled && serialized(w,c,first?NJW_START:NJW_LOADED,request,kind,a,first);JS_FreeValue(ctx,a);if(!ok){fail(w,c,"Worker resource allocation, representation or transport failed");return;}if(first)c->started=true;
}
bool web_worker_frame(web_workers *w,uint32_t id,uint32_t request,int event,int status,const char *url,const char *headers,const void *bytes,size_t n,const char *error,JSValueConst timing){
    if(!w||!request||event<1||event>4)return false;struct child *c=find(w,id);if(!c||c->stopped)return false;
    JSContext *ctx=w->ctx;JSValue value;uint32_t kind=(uint32_t)event<<NJW_FRAME_SHIFT;
    if(event==1){
        kind|=response_kind(&status,&url,&headers,&bytes,&n);
        value=JS_NewArray(ctx);
        if(!JS_IsException(value)&&!(resource_field(ctx,value,0,JS_NewInt32(ctx,status))&&resource_field(ctx,value,1,JS_NewString(ctx,url?url:""))&&
            resource_field(ctx,value,2,JS_NewString(ctx,headers?headers:"")))){JS_FreeValue(ctx,value);value=JS_EXCEPTION;}
    } else if(event==2){
        if(n>NJW_MAX_BYTES){fail(w,c,"Worker resource length is not representable on the wire");return false;}
        value=JS_NewArrayBufferCopy(ctx,bytes,n);
    } else if(event==4)value=JS_NewFloat64(ctx,(double)n);
    else {
        /* The end: [error, report timing or null]. */
        value=JS_NewArray(ctx);resource_retire(c,request);
        if(!JS_IsException(value)&&!(resource_field(ctx,value,0,JS_NewString(ctx,error?error:""))&&
            resource_field(ctx,value,1,JS_IsObject(timing)?JS_DupValue(ctx,timing):JS_NULL))){JS_FreeValue(ctx,value);value=JS_EXCEPTION;}
    }
    bool ok=!JS_IsException(value)&&serialized(w,c,NJW_FRAME,request,kind,value,false);JS_FreeValue(ctx,value);
    if(!ok){if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));fail(w,c,"Worker resource allocation, representation or transport failed");}
    return ok;
}
void web_worker_pump(web_workers *w,uint64_t now){
    (void)now;web_worker_background();if(!w)return;
    for(struct child *c=w->children;c;c=c->next){size_t budget=16384;if(c->stopped){child_ended(c,NULL);continue;}if(!c->started||c->closing)continue;if(!c->shared&&c->app<0&&!spawn_child(w,c))continue;
        if(!c->watched){n_handle queue=web_atomics_queue();c->watched=queue>=0&&n_events_watch(queue,c->channel,N_EVENT_RECEIVABLE,WEB_WORKER_TOKEN|c->id)>=0;}
        while(c->tx&&budget&&channel_sendable(c->channel)){struct packet *a=c->tx;
            /* A shared region goes first as a bare carrier message, one per
               attachment, each step checked for room; the payload follows. */
            if(a->attach_sent<a->attach.count){struct njw_header carrier={NJW_MAGIC,NJW_ATTACH,a->h.generation,0,0,0};
                if(n_channel_send(c->channel,&carrier,sizeof carrier,web_attachments_handle(&a->attach,a->attach_sent))<0){fail(w,c,"Worker command channel closed");break;}
                a->attach_sent++;budget-=sizeof carrier<budget?sizeof carrier:budget;continue;}
            size_t size=sizeof a->h+a->h.bytes;memcpy(a->data,&a->h,sizeof a->h);
            if(n_channel_send(c->channel,a->data,size,-1)<0){fail(w,c,"Worker command channel closed");break;}
            budget-=size<budget?size:budget;c->tx=a->next;if(!c->tx)c->tail=NULL;release_packet(w,a);}
        /* Take records up to this tick's byte budget (at least one). Each is
           reserved at its actual size; never a maximum payload in advance. */
        while(!c->stopped&&budget){
            n_result size=n_channel_receive(c->channel,NULL,0,N_CHANNEL_PEEK,NULL);
            if(size==-N_ERR_NOT_READY)break;if(size<0){child_exited(w,c);break;}
            /* A bare header is a carrier: keep its handle for the record after it. */
            if(size==(n_result)sizeof(struct njw_header)){struct njw_header carrier;n_handle attached=-1;
                if(n_channel_receive(c->channel,&carrier,sizeof carrier,0,&attached)!=size||carrier.magic!=NJW_MAGIC||carrier.op!=NJW_ATTACH||carrier.generation!=c->generation||carrier.bytes||attached<0||!web_inbox_add(&c->inbox,attached)){
                    if(attached>=0)n_handle_close(attached);fail(w,c,"Invalid Worker attachment");break;}
                continue;}
            struct njw_header h;size_t bytes=(size_t)size-sizeof h;
            uint8_t *data=(size_t)size>sizeof h&&bytes<=NJW_MAX_BYTES&&queue_room(w,bytes)?malloc((size_t)size):NULL;
            if(!data){fail(w,c,"Invalid Worker response, receive size overflow or allocation failure");break;}
            if(n_channel_receive(c->channel,data,(size_t)size,0,NULL)!=size){free(data);child_exited(w,c);break;}
            memcpy(&h,data,sizeof h);bool port=h.op==NJW_PORT_MESSAGE||h.op==NJW_PORT_CLOSE||h.op==NJW_PORT_DRAINED;
            bool valid_kind=port?h.kind<=1:h.op==NJW_MESSAGE?(h.kind==0||h.kind==NJW_WITH_PORTS):h.op==NJW_LOAD?(njw_load_kind_valid(h.kind)||(c->shared&&!h.request&&!h.kind)):(h.op==NJW_CANCEL||h.op==NJW_RESUME)?h.kind==0&&h.request!=0:h.kind<=2;
            if(h.magic!=NJW_MAGIC||h.generation!=c->generation||h.bytes!=bytes||(h.op!=NJW_MESSAGE&&h.op!=NJW_ERROR&&h.op!=NJW_LOAD&&h.op!=NJW_CANCEL&&h.op!=NJW_RESUME&&h.op!=NJW_CLOSE&&h.op!=NJW_CONSOLE&&h.op!=NJW_CSP_REPORT&&!port)||!valid_kind){free(data);fail(w,c,"Invalid Worker response");break;}
            memmove(data,data+sizeof h,bytes);budget-=(size_t)size<budget?(size_t)size:budget;
            struct event *e=calloc(1,sizeof *e);if(!e){free(data);fail(w,c,"Worker event allocation failed");break;}
            e->id=c->id;e->h=h;e->data=data;e->inbox=c->inbox;c->inbox=(struct web_inbox){0};e->from_child=true;w->queued+=sizeof h+bytes;route_event(w,c,e);
            if(h.op==NJW_CLOSE){c->closing=true;if(c->watched)n_events_unwatch(web_atomics_queue(),WEB_WORKER_TOKEN|c->id);c->watched=false;break;}
        }
    }
    struct child **at=&w->children;while(*at){struct child *c=*at;if(c->stopped&&c->app<0){*at=c->next;child_free(c);}else at=&c->next;}
}
bool web_worker_runnable(const web_workers *w){return w&&w->events;}
static bool console_prefix(JSContext *ctx,JSValueConst value,const struct child *c){
    JSValue message=JS_GetPropertyStr(ctx,value,"message");
    if(JS_IsException(message))return false;
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,message);
    JS_FreeValue(ctx,message);if(!text)return false;
    /* Only the two representable numeric identifiers are bounded, not the
       author message. Retain the complete UTF-8 span (including embedded NUL). */
    char prefix[64];int n=c->shared?snprintf(prefix,sizeof prefix,"[SharedWorker %u native service] ",c->id):snprintf(prefix,sizeof prefix,"[Worker %u app %lld] ",c->id,(long long)c->task);
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
/* IPC recreates Object/Array prototypes in the receiving realm. Strip them
 * again before this private report dictionary reaches CSP field reads/JSON:
 * author prototype getters and toJSON are never report authority. */
static bool report_private(JSContext *ctx,JSValueConst value){
    if(!JS_IsObject(value))return false;
    if(JS_SetPrototype(ctx,value,JS_NULL)<0)return false;
    JSValue init=JS_GetPropertyStr(ctx,value,"init"),endpoints=JS_GetPropertyStr(ctx,value,"endpoints");
    bool ok=JS_IsObject(init)&&JS_IsArray(ctx,endpoints)>0;
    if(ok)ok=JS_SetPrototype(ctx,init,JS_NULL)>=0&&JS_SetPrototype(ctx,endpoints,JS_NULL)>=0;
    JS_FreeValue(ctx,init);JS_FreeValue(ctx,endpoints);return ok;
}
static bool event_credit(web_workers*w,struct child*c,struct event*e){
    /* Acknowledge transport consumption, not the author callback. Synthetic
     * reports and fanout copies must never manufacture extra sender credit. */
    if(!e->from_child||!c||c->stopped||c->closing)return true;
    JSValue credit=JS_NewFloat64(w->ctx,(double)sizeof e->h+e->h.bytes);bool ok=serialized(w,c,NJW_CREDIT,0,0,credit,false);JS_FreeValue(w->ctx,credit);if(ok)e->from_child=false;else fail(w,c,"Worker transport credit allocation failed");return ok;
}
bool web_worker_run_one(web_workers *w){
    if(!w||!w->events)return false;struct event *e=w->events;w->events=e->next;if(!w->events)w->last=NULL;w->queued-=sizeof e->h+e->h.bytes;struct child *c=find(w,e->id);
    if(!event_credit(w,c,e))goto done;
    if((c&&!c->stopped)||e->h.op==NJW_ERROR){JSContext *ctx=w->ctx;JSValue v=JS_ReadObject(ctx,e->data,e->h.bytes,JS_READ_OBJ_REFERENCE);if(!JS_IsException(v)){
        if(c&&e->h.op==NJW_MESSAGE&&e->h.kind==NJW_WITH_PORTS){struct broker_pair *ports=NULL;if(!broker_metadata(ctx,c,v,1,&ports)){JS_FreeValue(ctx,v);goto done;}while(ports){struct broker_pair*p=ports;ports=p->next;p->next=c->ports;c->ports=p;}}
        if(c&&(e->h.op==NJW_PORT_MESSAGE||e->h.op==NJW_PORT_CLOSE||e->h.op==NJW_PORT_DRAINED)){
            struct broker_pair*p=broker_find(c,e->h.request,e->h.kind);if(!p||(e->h.op==NJW_PORT_MESSAGE&&p->sealed)){JS_FreeValue(ctx,v);goto done;}
            if(e->h.op!=NJW_PORT_MESSAGE){p->closed=p->sealed=true;
                if(e->h.op==NJW_PORT_CLOSE){web_worker_publication *ack=web_worker_prepare(w,c->id,NJW_PORT_DRAINED,p->id,p->creator,JS_NULL);if(!ack){JS_FreeValue(ctx,v);fail(w,c,"Worker close acknowledgement allocation failed");goto done;}web_worker_publish(ack);}
                broker_retire(c);
            }
        }
        if(e->h.op==NJW_LOAD){
            struct resource*r=e->h.request?malloc(sizeof*r):NULL;if(r){*r=(struct resource){c->resources,e->h.request};c->resources=r;}
            bool initial=c->shared&&!e->h.request&&!e->h.kind,ok=false;
            if(JS_IsString(v)){const char *url=JS_ToCString(ctx,v);ok=(r||initial)&&url&&w->load&&w->load(w->opaque,c->id,e->h.request,url,e->h.kind);JS_FreeCString(ctx,url);}
            else if(r&&((e->h.kind&~NJW_LOAD_POLICY_FLAGS)==2)&&JS_IsArray(ctx,v)){
                /* [url, method, headers, body, referrer, referrer policy, destination]: browserjsworker.c's native_request. */
                JSValue fields[7];for(unsigned i=0;i<7;i++)fields[i]=JS_GetPropertyUint32(ctx,v,i);
                JSValue count_value=JS_GetPropertyStr(ctx,v,"length");uint32_t count=0;bool valid=JS_ToUint32(ctx,&count,count_value)>=0&&count==7;JS_FreeValue(ctx,count_value);
                static const unsigned text[]={0,1,2,4,6};size_t lengths[5]={0};const char *strings[5]={0};
                for(unsigned i=0;i<5&&valid;i++){valid=JS_IsString(fields[text[i]]);if(valid){strings[i]=JS_ToCStringLen(ctx,&lengths[i],fields[text[i]]);valid=strings[i]&&!memchr(strings[i],0,lengths[i]);}}
                uint8_t empty_body=0;size_t bytes=0;uint8_t *body=NULL;if(valid&&!JS_IsNull(fields[3])){body=JS_GetArrayBuffer(ctx,&bytes,fields[3]);valid=body||!JS_HasException(ctx);if(valid&&!body)body=&empty_body;}
                int32_t referrer_policy=0;
                if(valid)valid=JS_IsNumber(fields[5])&&JS_ToInt32(ctx,&referrer_policy,fields[5])>=0&&referrer_policy>=0&&referrer_policy<=8;
                struct web_worker_fetch fetch={.url=strings[0],.method=strings[1],.headers=strings[2],.referrer=strings[3],.destination=strings[4],
                    .body=body,.body_len=bytes,.referrer_policy=referrer_policy};
                if(valid)ok=w->request&&w->request(w->opaque,c->id,e->h.request,(int)e->h.kind,&fetch);
                for(unsigned i=0;i<5;i++)JS_FreeCString(ctx,strings[i]);for(unsigned i=0;i<7;i++)JS_FreeValue(ctx,fields[i]);
            }
            if(!ok){if(JS_HasException(ctx)){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}web_worker_loaded(w,c->id,e->h.request,0,"","",NULL,0,"Worker resource request rejected or allocation failed",JS_UNDEFINED);}
        }
        else if(e->h.op==NJW_CSP_REPORT){if(report_private(ctx,v)){if(w->reporter)w->reporter(w->report_opaque,v);}else fail(w,c,"Invalid private Worker CSP report");}
        else if(e->h.op==NJW_CANCEL){if(!JS_IsNull(v)||!w->cancel)fail(w,c,"Worker host cannot cancel this resource");else w->cancel(w->opaque,c->id,e->h.request);}
        else if(e->h.op==NJW_RESUME){if(!JS_IsNull(v)||!w->resume)fail(w,c,"Worker host cannot stream this resource");else w->resume(w->opaque,c->id,e->h.request);}
        else {if(e->h.op!=NJW_CONSOLE||!c||console_prefix(ctx,v,c)){JSValue args[]={JS_NewUint32(ctx,e->id),JS_NewUint32(ctx,e->h.op),v,JS_NewUint32(ctx,e->h.request),JS_NewUint32(ctx,e->h.kind)};web_atomics_import_begin(&e->inbox);JSValue r=JS_Call(ctx,w->callback,JS_UNDEFINED,5,args);web_atomics_import_end();JS_FreeValue(ctx,r);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);JS_FreeValue(ctx,args[3]);JS_FreeValue(ctx,args[4]);if(e->h.op==NJW_CLOSE&&c)stop(w,c);}}
        JS_FreeValue(ctx,v);
    }}done:web_inbox_close(&e->inbox);free(e->data);free(e);return true;
}
/* A watched channel wakes the page by itself. Only a record waiting for room,
 * an unwatched channel or a stopping task are checked again in 10 ms. */
int64_t web_worker_deadline(const web_workers *w,uint64_t now){
    if(!w)return -1;if(w->events)return (int64_t)now;int64_t due=-1;
    for(struct child *c=w->children;c;c=c->next){
        if(c->stopped){if(c->app>=0)due=(int64_t)now+10;continue;}
        if(!c->started||c->closing)continue;
        if(!c->shared&&c->app<0)return (int64_t)now;
        if(c->tx||!c->watched)due=(int64_t)now+10;
    }return due;
}
void web_worker_free(web_workers *w){
    if(!w)return;web_worker_cancel_captures(w);
    /* Detach only this document's connections; the other owners retain the
     * same child realm, globals, ports and native task. */
    for(struct shared_ref*r=w->shared;r;r=r->next){struct child*c=r->child;if(c->stopped)continue;
        for(struct broker_pair*p=c->ports;p;p=p->next)if(p->client==w){
            if(!p->closed){if(!serialized(w,c,NJW_PORT_CLOSE,p->id,p->creator,JS_NULL,false)){fail(c->owner,c,"SharedWorker connection cleanup allocation failed");break;}p->closed=true;p->close_sent=true;}
            p->client=NULL;
        }
    }
    while(w->children){struct child*c=w->children;web_workers*replacement=NULL;
        if(c->shared&&!c->stopped)for(web_workers*other=managers;other;other=other->next){if(other==w)continue;for(struct shared_ref*r=other->shared;r;r=r->next)if(r->child==c){replacement=other;break;}if(replacement)break;}
        if(replacement){
            if(w->cancel)w->cancel(w->opaque,c->id,0);
            while(c->resources&&!c->stopped){uint32_t id=c->resources->id;web_worker_loaded(w,c->id,id,0,"","",NULL,0,"Worker resource owner navigated",JS_UNDEFINED);}
            w->children=c->next;c->next=replacement->children;replacement->children=c;
            size_t charge=0;for(struct packet*p=c->tx;p;p=p->next)charge+=sizeof p->h+p->h.bytes;
            for(struct broker_pair*p=c->ports;p;p=p->next){struct packet*a=p->held_close,*b=p->held_ack;if(a)charge+=sizeof a->h+a->h.bytes;if(b)charge+=sizeof b->h+b->h.bytes;}
            w->queued-=charge;replacement->queued+=charge;c->owner=replacement;
            struct event**at=&w->events;while(*at){struct event*e=*at;if(e->id!=c->id){at=&e->next;continue;}*at=e->next;e->next=NULL;w->queued-=sizeof e->h+e->h.bytes;append_event(replacement,e);}w->last=NULL;for(struct event*e=w->events;e;e=e->next)w->last=e;
            if(!c->started&&(!replacement->load||!replacement->load(replacement->opaque,c->id,0,c->url,0)))fail(replacement,c,"SharedWorker source owner replacement failed");
        }else{w->children=c->next;stop(w,c);if(c->app>=0){c->next=retired;retired=c;}else child_free(c);}
    }
    /* A retiring non-owner document may hold actual received port packets.
     * Their shared transport survives: discard is consumption too. Return
     * one credit before removing the authority needed to find that child. */
    for(struct event*e=w->events;e;e=e->next){struct child*c=find(w,e->id);if(c&&c->shared&&c->owner!=w)event_credit(w,c,e);}
    while(w->shared){struct shared_ref*r=w->shared;w->shared=r->next;free(r);}
    while(w->events){struct event*e=w->events;w->events=e->next;web_inbox_close(&e->inbox);free(e->data);free(e);}
    web_workers**at=&managers;while(*at&&*at!=w)at=&(*at)->next;if(*at)*at=w->next;
    JS_FreeValue(w->ctx,w->callback);free(w->creator_policy);free(w->origin);free(w);
}
