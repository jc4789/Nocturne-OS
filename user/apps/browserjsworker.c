/* Dedicated QuickJS realm in a native Nocturne child task. The only author
 * capabilities are data-only IPC to the browser; no libc/native modules. */
#include <nocturne.h>
#include <quickjs.h>
#include <js_worker_wire.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <webnet.h>
#include "../libc/web/js_encoding.h"
#include "../libc/web/js_crypto.h"
#include "../libc/web/js_worker_transfer.h"
#include "../libc/web/js_worker_port_lifetime.h"
#include "../libc/web/js_worker_runtime.inc"
#include "../libc/web/js_navigator_native.h"
#include "../libc/web/html_policy.h"
#include "../libc/web/js_html_safety_native.h"
struct client {struct client *next;int fd;uint32_t generation;size_t references,in_flight;bool established,active,blocked,wants_output;struct njw_header rx;size_t head_at,data_at;uint8_t*data;};
struct pending_load {struct pending_load*next;uint32_t id;struct client*client;};
struct deferred {struct deferred *next;struct njw_header h;uint8_t *data;struct client*client;bool normalized;};
struct outgoing {struct outgoing *next;struct njw_header h;size_t at;uint8_t *data;struct client*client;};
struct policy_violation {struct policy_violation *next;JSValue init;struct web_html_policy_rule *rule;};
struct worker {JSRuntime *rt;JSContext *ctx;JSValue hooks;uint32_t generation,request,interrupt_ticks,next_shared_port;uint64_t until;bool closed,established,output_failed,policy_initialized,shared,had_client,need_start;struct deferred *head,*tail,*before_start,*before_start_tail;struct outgoing *output,*last_output;size_t queued,output_bytes,in_flight;uint32_t next_lease;struct broker_lease *leases;struct broker_pair *ports;struct client*clients,*current_client,*network_client;struct pending_load*loads;struct n_pollfd*pollfds;struct client**pollclients;size_t pollcapacity;const char*script_url;char*security_url;web_doc policy_doc;struct policy_violation *violations,*last_violation;int policy_status;};
struct output_publication {struct worker *worker;struct outgoing *packet;struct broker_pair *ports,*close_pair,*ack_pair;};
static void output_link(void *opaque,void *packet){struct worker*w=opaque;struct outgoing*p=packet;size_t charge=sizeof p->h+p->h.bytes;if(p->client)p->client->in_flight+=charge;else w->in_flight+=charge;if(w->last_output)w->last_output->next=p;else w->output=p;w->last_output=p;}
static void output_packet_free(struct worker*w,void *opaque){struct outgoing*p=opaque;if(p){if(p->client)p->client->references--;js_free(w->ctx,p->data);free(p);}}
static void output_ports_free(struct worker*w,struct broker_pair*p){while(p){struct broker_pair*n=p->next;output_packet_free(w,p->held_close);output_packet_free(w,p->held_ack);if(p->client)((struct client*)p->client)->references--;free(p);p=n;}}
static void output_ports_retire(struct worker*w){struct broker_pair **at=&w->ports;while(*at){struct broker_pair*p=*at;if(worker_port_retirable(p)){*at=p->next;if(p->client)((struct client*)p->client)->references--;free(p);}else at=&p->next;}}
static void deferred_free(struct deferred*d){if(d->client)d->client->references--;free(d->data);free(d);}
static void output_publish(void *opaque){
    struct output_publication*t=opaque;struct worker*w=t->worker;struct outgoing*p=t->packet;
    while(t->ports){struct broker_pair*pair=t->ports;t->ports=pair->next;pair->next=w->ports;w->ports=pair;}
    size_t charge=sizeof p->h+p->h.bytes;w->output_bytes+=charge;if(p->client)p->client->references++;
    if(t->close_pair){t->close_pair->closed=true;t->close_pair->held_close=p;worker_port_flush(t->close_pair,output_link,w);}
    else if(t->ack_pair){t->ack_pair->held_ack=p;worker_port_flush(t->ack_pair,output_link,w);}
    else output_link(w,p);
}
static void shared_disconnect(struct worker*,struct client*);
static void output_unlink(struct worker*w,struct outgoing**at,struct outgoing*p){*at=p->next;if(w->last_output==p){w->last_output=NULL;for(struct outgoing*q=w->output;q;q=q->next)w->last_output=q;}w->output_bytes-=sizeof p->h+p->h.bytes;output_packet_free(w,p);}
static void output_flush(struct worker*w){size_t budget=16384;struct outgoing**at=&w->output;for(struct client*c=w->clients;c;c=c->next)c->blocked=false;
    while(*at&&budget){struct outgoing*p=*at;struct client*c=p->client;if(w->shared&&c&&c->blocked){at=&p->next;continue;}
        if(w->shared&&(!c||!c->active)){output_unlink(w,at,p);continue;}
        bool header=p->at<sizeof p->h;size_t left=header?sizeof p->h-p->at:sizeof p->h+p->h.bytes-p->at,take=left<budget?left:budget;const uint8_t*src=header?(uint8_t*)&p->h+p->at:p->data+p->at-sizeof p->h;ssize_t n=write(w->shared?c->fd:1,src,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR)){if(!w->shared)break;c->blocked=true;at=&p->next;continue;}if(n<=0){if(!w->shared){w->closed=w->output_failed=true;break;}shared_disconnect(w,c);continue;}
        p->at+=n;budget-=n;if(p->at==sizeof p->h+p->h.bytes)output_unlink(w,at,p);
    }w->last_output=NULL;for(struct outgoing*p=w->output;p;p=p->next)w->last_output=p;
}
static bool exact(int fd,void *p,size_t n,bool writing){size_t at=0;while(at<n){ssize_t got=writing?write(fd,(uint8_t *)p+at,n-at):read(fd,(uint8_t *)p+at,n-at);if(got<0&&errno==EINTR)continue;if(got<=0)return false;at+=(size_t)got;}return true;}
static bool send_client(struct worker*w,struct client*c,unsigned op,unsigned request,unsigned kind,JSValueConst value){size_t n=0;uint8_t *data=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!data)return false;if(n>NJW_MAX_BYTES||n>SIZE_MAX-sizeof(struct njw_header)||n+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(w->ctx,data);return false;}struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(w->ctx,data);return false;}p->data=data;p->client=c;p->h=(struct njw_header){NJW_MAGIC,op,c?c->generation:w->generation,request,kind,(uint32_t)n};struct output_publication t={.worker=w,.packet=p};output_publish(&t);return true;}
static bool send_value(struct worker*w,unsigned op,unsigned request,unsigned kind,JSValueConst value){
    if(!w->shared)return send_client(w,NULL,op,request,kind,value);
    if(op==NJW_ERROR||op==NJW_CONSOLE||op==NJW_CLOSE){bool ok=true;for(struct client*c=w->clients;c;c=c->next)if(c->active&&c->established&&!send_client(w,c,op,request,kind,value))ok=false;return ok;}
    struct client*c=w->network_client;if(!c||!c->active)return false;
    if(op==NJW_LOAD&&request){struct pending_load*p=malloc(sizeof*p);if(!p)return false;*p=(struct pending_load){w->loads,request,c};w->loads=p;c->references++;if(!send_client(w,c,op,request,kind,value)){w->loads=p->next;c->references--;free(p);return false;}return true;}
    if(op==NJW_CANCEL){struct pending_load**at=&w->loads;while(*at&&(*at)->id!=request)at=&(*at)->next;if(*at){struct pending_load*p=*at;c=p->client;*at=p->next;c->references--;free(p);}}
    return send_client(w,c,op,request,kind,value);
}
static struct deferred *receive(struct worker *w){
    struct deferred *d=calloc(1,sizeof *d);if(!d)return NULL;
    if(!exact(0,&d->h,sizeof d->h,false)||d->h.magic!=NJW_MAGIC||!d->h.bytes||d->h.bytes>NJW_MAX_BYTES||
       ((d->h.op==NJW_PORT_MESSAGE||d->h.op==NJW_PORT_CLOSE||d->h.op==NJW_PORT_DRAINED)?d->h.kind>1:d->h.op==NJW_CONNECT?d->h.kind!=NJW_WITH_PORTS:d->h.op==NJW_MESSAGE?(d->h.kind!=0&&d->h.kind!=NJW_WITH_PORTS):d->h.op==NJW_LOADED?!njw_loaded_kind_valid(d->h.kind):d->h.kind!=0)||
       (d->h.op!=NJW_START&&d->h.op!=NJW_MESSAGE&&d->h.op!=NJW_CONNECT&&d->h.op!=NJW_LOADED&&d->h.op!=NJW_CREDIT&&d->h.op!=NJW_PORT_MESSAGE&&d->h.op!=NJW_PORT_CLOSE&&d->h.op!=NJW_PORT_DRAINED)||(w->established&&d->h.generation!=w->generation)||(!w->established&&d->h.op!=NJW_START)){free(d);return NULL;}
    if(!w->established){w->generation=d->h.generation;w->established=true;}else if(d->h.op==NJW_START){free(d);return NULL;}
    d->data=malloc(d->h.bytes);if(!d->data||!exact(0,d->data,d->h.bytes,false)){free(d->data);free(d);return NULL;}return d;
}
static void defer_link(struct worker*w,struct deferred*d,bool before){
    d->next=NULL;struct deferred**head=before?&w->before_start:&w->head,**tail=before?&w->before_start_tail:&w->tail;
    if(*tail)(*tail)->next=d;else *head=d;*tail=d;
}
static bool defer_value(struct worker*w,unsigned op,uint32_t id,uint32_t kind,JSValueConst value){
    size_t n=0;uint8_t*bytes=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!bytes)return false;
    if(n>NJW_MAX_BYTES||n>SIZE_MAX-w->queued){js_free(w->ctx,bytes);return false;}
    struct deferred*d=calloc(1,sizeof*d);if(!d){js_free(w->ctx,bytes);return false;}d->data=malloc(n);if(!d->data){js_free(w->ctx,bytes);free(d);return false;}memcpy(d->data,bytes,n);js_free(w->ctx,bytes);d->h=(struct njw_header){NJW_MAGIC,op,w->generation,id,kind,(uint32_t)n};w->queued+=n;defer_link(w,d,false);return true;
}
static void shared_restart_source(struct worker*w){
    if(!w->need_start||w->policy_initialized||!w->network_client||!w->network_client->active)return;
    JSValue url=JS_NewString(w->ctx,w->script_url?w->script_url:"");if(!send_value(w,NJW_LOAD,0,0,url))w->closed=w->output_failed=true;else w->need_start=false;JS_FreeValue(w->ctx,url);
}
static void shared_disconnect(struct worker*w,struct client*c){
    if(!c||!c->active)return;c->active=false;close(c->fd);c->fd=-1;if(c->data){w->queued-=c->rx.bytes;free(c->data);c->data=NULL;}c->head_at=c->data_at=0;
    if(w->network_client==c){w->network_client=NULL;for(struct client*p=w->clients;p;p=p->next)if(p->active&&p->established){w->network_client=p;break;}}
    struct pending_load**at=&w->loads;while(*at){struct pending_load*p=*at;if(p->client!=c){at=&p->next;continue;}*at=p->next;
        JSValue a=JS_NewArray(w->ctx);bool ok=!JS_IsException(a);
        JSValue fields[]={JS_NewInt32(w->ctx,0),JS_NewString(w->ctx,""),JS_NewString(w->ctx,""),JS_NewArrayBufferCopy(w->ctx,NULL,0),JS_NewString(w->ctx,"SharedWorker network owner disconnected")};
        for(unsigned i=0;i<5;i++)if(JS_IsException(fields[i])||JS_DefinePropertyValueUint32(w->ctx,a,i,fields[i],JS_PROP_C_W_E)<0)ok=false;
        if(!ok||!defer_value(w,NJW_LOADED,p->id,0,a))w->closed=w->output_failed=true;JS_FreeValue(w->ctx,a);c->references--;free(p);
    }
    for(struct broker_pair*p=w->ports;p;p=p->next)if(p->client==c&&!p->sealed){p->closed=true;if(!defer_value(w,NJW_PORT_CLOSE,p->id,p->creator,JS_NULL))w->closed=w->output_failed=true;}
    bool active=false;for(struct client*p=w->clients;p;p=p->next)if(p->active){active=true;break;}
    if(!active&&w->had_client)w->closed=true;
    else if(!w->policy_initialized){w->need_start=true;shared_restart_source(w);}
}
static void shared_collect(struct worker*w){
    struct client**at=&w->clients;while(*at){struct client*c=*at;if(c->active||c->references||w->current_client==c||w->network_client==c){at=&c->next;continue;}*at=c->next;free(c);}
}
static bool shared_header_valid(const struct njw_header*h){
    bool port=h->op==NJW_PORT_MESSAGE||h->op==NJW_PORT_CLOSE||h->op==NJW_PORT_DRAINED;
    bool kind=port?h->kind<=1:h->op==NJW_CONNECT?h->kind==NJW_WITH_PORTS:h->op==NJW_LOADED?njw_loaded_kind_valid(h->kind):h->kind==0;
    return h->magic==NJW_MAGIC&&h->bytes&&kind&&(port||h->op==NJW_CONNECT||h->op==NJW_START||h->op==NJW_LOADED||(h->op==NJW_CREDIT&&!h->request));
}
static bool shared_connect_url(struct worker*w,struct client*c,const struct deferred*d){
    JSValue value=JS_ReadObject(w->ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE),url=JS_IsException(value)?JS_UNDEFINED:JS_GetPropertyUint32(w->ctx,value,2);const char*address=JS_IsString(url)?JS_ToCString(w->ctx,url):NULL;bool ok=address&&!strcmp(address,w->script_url?w->script_url:"");JS_FreeCString(w->ctx,address);JS_FreeValue(w->ctx,url);JS_FreeValue(w->ctx,value);
    if(ok)return true;if(JS_HasException(w->ctx))JS_FreeValue(w->ctx,JS_GetException(w->ctx));JSValue error=JS_NewObjectProto(w->ctx,JS_NULL);JS_DefinePropertyValueStr(w->ctx,error,"message",JS_NewString(w->ctx,"SharedWorker name is associated with a different script URL"),JS_PROP_C_W_E);JS_DefinePropertyValueStr(w->ctx,error,"terminal",JS_TRUE,JS_PROP_C_W_E);bool sent=send_client(w,c,NJW_ERROR,0,0,error);JS_FreeValue(w->ctx,error);if(sent)output_flush(w);return false;
}
/* Native-only endpoint ownership. The interrupt path may use this helper:
 * never deserialize bytes, allocate JS values or dispatch an author task. */
static bool shared_accept(struct worker*w,unsigned quantum){
    for(unsigned i=0;i<quantum;i++){int fd=n_channel_accept(0);if(fd<0){if(errno==EAGAIN||errno==EINTR)return true;w->closed=w->output_failed=true;return false;}
        struct client*c=calloc(1,sizeof*c);if(!c){close(fd);w->closed=w->output_failed=true;return false;}c->fd=fd;c->active=true;c->next=w->clients;w->clients=c;w->had_client=true;
    }return true;
}
static bool shared_without_owners(struct worker*w){
    if(!w->shared||!w->had_client)return false;
    struct n_pollfd listener={0,N_POLLIN,0};if(poll(&listener,1,0)<0)return false;
    /* Accept a finite batch so dead, never-parsed pending peers do not pin a
     * detached CPU-only task forever. Live new peers keep their FD ownership. */
    if((listener.revents&N_POLLIN)&&!shared_accept(w,16))return true;
    struct n_pollfd batch[64];unsigned count=0;
    for(struct client*c=w->clients;c;c=c->next)if(c->active){batch[count++]=(struct n_pollfd){c->fd,0,0};if(count==64){if(poll(batch,(int)count,0)<0)return false;for(unsigned i=0;i<count;i++)if(!(batch[i].revents&N_POLLHUP))return false;count=0;}}
    if(count){if(poll(batch,(int)count,0)<0)return false;for(unsigned i=0;i<count;i++)if(!(batch[i].revents&N_POLLHUP))return false;}
    /* A connection may have arrived during the bounded scan. An unaccepted
     * owner is protected; the next interrupt will accept the next quantum. */
    listener.revents=0;if(poll(&listener,1,0)<0)return false;return !(listener.revents&N_POLLIN);
}
/* One poll for the whole shared server, incremental nonblocking packets, and
 * a finite copy quantum. A slow client never blocks another client's input. */
static struct deferred*shared_receive(struct worker*w,int timeout){
    shared_collect(w);size_t count=1;for(struct client*c=w->clients;c;c=c->next)if(c->active)count++;
    if(count>(size_t)INT_MAX||count>SIZE_MAX/sizeof*w->pollfds||count>SIZE_MAX/sizeof*w->pollclients){w->closed=w->output_failed=true;return NULL;}
    if(count>w->pollcapacity){size_t capacity=count>SIZE_MAX/2?count:count*2;struct n_pollfd*p=realloc(w->pollfds,capacity*sizeof*p);if(!p){w->closed=w->output_failed=true;return NULL;}w->pollfds=p;struct client**clients=realloc(w->pollclients,capacity*sizeof*clients);if(!clients){w->closed=w->output_failed=true;return NULL;}w->pollclients=clients;w->pollcapacity=capacity;}
    for(struct client*c=w->clients;c;c=c->next)c->wants_output=false;for(struct outgoing*p=w->output;p;p=p->next)if(p->client)p->client->wants_output=true;
    w->pollfds[0]=(struct n_pollfd){w->closed?-1:0,N_POLLIN,0};w->pollclients[0]=NULL;size_t i=1;for(struct client*c=w->clients;c;c=c->next)if(c->active){w->pollfds[i]=(struct n_pollfd){c->fd,N_POLLIN|(c->wants_output?N_POLLOUT:0),0};w->pollclients[i++]=c;}
    int ready=poll(w->pollfds,(int)count,timeout);if(ready<0){if(errno!=EINTR)w->closed=w->output_failed=true;return NULL;}if(!ready)return NULL;
    if((w->pollfds[0].revents&N_POLLIN)&&!shared_accept(w,16))return NULL;
    size_t budget=65536;
    for(i=1;i<count&&budget&&!w->closed;i++){struct client*c=w->pollclients[i];short flags=w->pollfds[i].revents;if(!c->active)continue;if(flags&N_POLLOUT)output_flush(w);
        if(!(flags&(N_POLLIN|N_POLLHUP)))continue;
        while(c->active&&budget){bool header=c->head_at<sizeof c->rx;size_t left=header?sizeof c->rx-c->head_at:c->rx.bytes-c->data_at,take=left<budget?left:budget;ssize_t n=read(c->fd,header?(void*)((uint8_t*)&c->rx+c->head_at):(void*)(c->data+c->data_at),take);
            if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){shared_disconnect(w,c);break;}budget-=n;
            if(header){c->head_at+=n;if(c->head_at==sizeof c->rx){if(!shared_header_valid(&c->rx)||(c->established&&c->rx.generation!=c->generation)||c->rx.bytes>SIZE_MAX-w->queued){shared_disconnect(w,c);break;}if(!c->established){c->generation=c->rx.generation;c->established=true;if(!w->network_client)w->network_client=c;}c->data=malloc(c->rx.bytes);if(!c->data){w->closed=w->output_failed=true;break;}w->queued+=c->rx.bytes;}}else c->data_at+=n;
            if(c->head_at==sizeof c->rx&&c->data_at==c->rx.bytes){struct deferred*d=calloc(1,sizeof*d);if(!d){w->closed=w->output_failed=true;break;}d->h=c->rx;d->data=c->data;d->client=c;c->references++;c->data=NULL;c->head_at=c->data_at=0;
                if(d->h.op==NJW_CONNECT&&!shared_connect_url(w,c,d)){w->queued-=d->h.bytes;deferred_free(d);shared_disconnect(w,c);break;}
                if(d->h.op==NJW_CONNECT)shared_restart_source(w);
                if(!w->policy_initialized&&d->h.op!=NJW_START)defer_link(w,d,true);else{w->queued-=d->h.bytes;return d;}
            }
        }
    }return NULL;
}
static struct broker_pair*shared_pair(struct worker*w,struct client*c,uint32_t local,uint32_t creator){for(struct broker_pair*p=w->ports;p;p=p->next)if(p->client==c&&p->client_id==local&&p->creator==creator)return p;return NULL;}
static bool shared_metadata(struct worker*w,JSValueConst value){
    struct broker_pair*ports=NULL;if(!worker_port_metadata(w->ctx,value,0,NULL,UINT32_MAX,&ports))return false;
    for(struct broker_pair*p=ports;p;p=p->next){if(shared_pair(w,w->current_client,p->id,p->creator)||w->next_shared_port==UINT32_MAX){output_ports_free(w,ports);JS_ThrowRangeError(w->ctx,"SharedWorker port identity is invalid or exhausted");return false;}p->client=w->current_client;w->current_client->references++;p->client_id=p->id;p->id=++w->next_shared_port;}
    JSValue list=JS_GetPropertyUint32(w->ctx,value,1),length=JS_GetPropertyStr(w->ctx,list,"length");uint32_t count=0;bool ok=!JS_IsException(list)&&!JS_IsException(length)&&!JS_ToUint32(w->ctx,&count,length);JS_FreeValue(w->ctx,length);
    for(uint32_t i=0;i<count&&ok;i++){JSValue m=JS_GetPropertyUint32(w->ctx,list,i);if(JS_IsNull(m)){JS_FreeValue(w->ctx,m);continue;}JSValue id=JS_GetPropertyUint32(w->ctx,m,0);uint32_t local=0;ok=!JS_IsException(id)&&!JS_ToUint32(w->ctx,&local,id);JS_FreeValue(w->ctx,id);struct broker_pair*p=ports;while(p&&p->client_id!=local)p=p->next;if(!p)ok=false;if(ok)ok=JS_DefinePropertyValueUint32(w->ctx,m,0,JS_NewUint32(w->ctx,p->id),JS_PROP_C_W_E)>=0;JS_FreeValue(w->ctx,m);}
    JS_FreeValue(w->ctx,list);if(!ok){output_ports_free(w,ports);return false;}while(ports){struct broker_pair*p=ports;ports=p->next;p->next=w->ports;w->ports=p;}return true;
}
static bool shared_incoming(struct worker*w,struct deferred*d){
    if(!d->client)return true;struct client*c=d->client;if(!c->active)return false;
    if(d->normalized)return true;d->normalized=true;
    if(d->h.op==NJW_START){if(w->policy_initialized)return false;w->network_client=c;w->generation=c->generation;return true;}
    if(d->h.op==NJW_LOADED){struct pending_load**at=&w->loads;while(*at&&((*at)->id!=d->h.request||(*at)->client!=c))at=&(*at)->next;if(!*at)return false;struct pending_load*p=*at;*at=p->next;c->references--;free(p);return true;}
    if(d->h.op==NJW_PORT_MESSAGE||d->h.op==NJW_PORT_CLOSE||d->h.op==NJW_PORT_DRAINED){struct broker_pair*p=shared_pair(w,c,d->h.request,d->h.kind);if(!p)return false;d->h.request=p->id;}
    return true;
}
static bool transport_credit(struct worker*w,const struct deferred*d){
    JSValue value=JS_ReadObject(w->ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);double amount=0;bool valid=JS_IsNumber(value)&&!JS_ToFloat64(w->ctx,&amount,value);JS_FreeValue(w->ctx,value);
    size_t*flight=w->shared?(d->client?&d->client->in_flight:NULL):&w->in_flight;
    if(!valid||!flight||!(amount>=0&&amount<=(double)NJW_MAX_BYTES+sizeof d->h)||amount>(double)*flight||amount!=(double)(size_t)amount){if(JS_HasException(w->ctx))JS_FreeValue(w->ctx,JS_GetException(w->ctx));if(w->shared)shared_disconnect(w,d->client);else w->closed=true;return false;}
    *flight-=(size_t)amount;return true;
}
/* The native producer can block for transport credit while its JS task is
 * running. Only trusted credit is consumed here; author messages, imports and
 * connect events are deferred until the ordinary task/microtask checkpoint.
 * All shared clients' transport is pumped, never author-JS reentered. */
static bool output_wait_inner(struct worker*w,struct client*client,size_t next){
    if(next>SIZE_MAX-sizeof(struct njw_header))return false;
    for(;;){output_flush(w);if(w->closed)return false;if(w->shared&&(!client||!client->active))return true;
        size_t flight=client?client->in_flight:w->in_flight;
        if(!flight||(flight<=NJW_TRANSPORT_WINDOW&&next<=NJW_TRANSPORT_WINDOW-flight))return true;
        uint64_t now=uptime_ms();if(w->until!=UINT64_MAX&&now>=w->until)return false;uint64_t remaining=w->until==UINT64_MAX?UINT64_MAX:w->until-now;int wait=remaining>(uint64_t)INT_MAX?INT_MAX:(int)remaining;struct deferred*d=NULL;
        /* An import wait may already have retained a private credit record. */
        struct deferred**at=&w->head;while(*at&&(*at)->h.op!=NJW_CREDIT)at=&(*at)->next;
        if(*at){d=*at;*at=d->next;w->queued-=d->h.bytes;w->tail=NULL;for(struct deferred*p=w->head;p;p=p->next)w->tail=p;d->next=NULL;}
        else if(w->shared)d=shared_receive(w,wait);
        else{struct n_pollfd p[2]={{0,N_POLLIN,0},{1,w->output?N_POLLOUT:0,0}};int ready=poll(p,2,wait);if(ready<0){if(errno==EINTR)continue;w->closed=true;return false;}if(p[1].revents&N_POLLOUT)output_flush(w);if(p[0].revents&(N_POLLIN|N_POLLHUP)){d=receive(w);if(!d){w->closed=true;return false;}}}
        if(!d)continue;if(d->h.op==NJW_CREDIT){transport_credit(w,d);deferred_free(d);continue;}
        if(d->h.bytes>SIZE_MAX-w->queued){deferred_free(d);w->closed=true;return false;}w->queued+=d->h.bytes;defer_link(w,d,false);
    }
}
static bool output_wait(struct worker*w,struct client*client,size_t next){
    /* Broadcast targets need their own lifetime pin: unlike a port send they
     * may have no broker_pair/current_client keeping them alive during poll. */
    if(client)client->references++;bool ok=output_wait_inner(w,client,next);if(client)client->references--;return ok;
}
static struct worker *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
/* The child has no DOM, but owns the same native CSP rules as a document.
 * Neither writable Worker globals nor an author-supplied JS dictionary is
 * consulted for policy authority. These plain objects are private snapshots. */
static bool policy_field(JSContext *ctx,JSValueConst object,const char *key,JSValue value){
    if(JS_IsException(value))return false;return JS_DefinePropertyValueStr(ctx,object,key,value,JS_PROP_C_W_E)>=0;
}
static JSValue worker_policy_record(JSContext *ctx,struct web_html_policy_rule *rule,uint32_t index){
    JSValue record=JS_NewObject(ctx);if(JS_IsException(record))return record;
    if(!policy_field(ctx,record,"policyIndex",JS_NewUint32(ctx,index))||
       !policy_field(ctx,record,"reportOnly",JS_NewBool(ctx,rule->report_only))||
       !policy_field(ctx,record,"originalPolicy",JS_NewString(ctx,rule->original_policy?rule->original_policy:""))){JS_FreeValue(ctx,record);return JS_EXCEPTION;}
    return record;
}
static JSValue worker_policy_state(JSContext *ctx,struct worker *w){
    JSValue out=JS_NewObject(ctx),rules=JS_NewArray(ctx),requirements=JS_NewArray(ctx);
    if(JS_IsException(out)||JS_IsException(rules)||JS_IsException(requirements))goto fail;
    uint32_t at=0,required_at=0;size_t index=0;bool required=false,enforce=false;
    for(struct web_html_policy_rule *r=w->policy_doc.html_policy?w->policy_doc.html_policy->first:NULL;r;r=r->next,index++){
        if(index>UINT32_MAX){JS_ThrowRangeError(ctx,"Worker policy representation overflow");goto fail;}
        if(r->require_script){
            required=true;if(!r->report_only)enforce=true;
            JSValue record=worker_policy_record(ctx,r,(uint32_t)index);
            if(JS_IsException(record))goto fail;
            if(required_at==UINT32_MAX){JS_FreeValue(ctx,record);JS_ThrowRangeError(ctx,"Worker policy requirement overflow");goto fail;}
            if(JS_DefinePropertyValueUint32(ctx,requirements,required_at++,record,JS_PROP_C_W_E)<0)goto fail;
        }
        if(!r->has_names)continue;
        JSValue record=worker_policy_record(ctx,r,(uint32_t)index),names=JS_NewArray(ctx);
        if(JS_IsException(record)||JS_IsException(names)){JS_FreeValue(ctx,record);JS_FreeValue(ctx,names);goto fail;}
        for(size_t i=0;i<r->count;i++){
            if(i>UINT32_MAX){JS_ThrowRangeError(ctx,"Worker policy name representation overflow");JS_FreeValue(ctx,record);JS_FreeValue(ctx,names);goto fail;}
            JSValue name=JS_NewString(ctx,r->names[i]);
            if(JS_IsException(name)||JS_DefinePropertyValueUint32(ctx,names,(uint32_t)i,name,JS_PROP_C_W_E)<0){JS_FreeValue(ctx,record);JS_FreeValue(ctx,names);goto fail;}
        }
        if(!policy_field(ctx,record,"names",names)||!policy_field(ctx,record,"allowDuplicates",JS_NewBool(ctx,r->allow_duplicates))){JS_FreeValue(ctx,record);goto fail;}
        if(at==UINT32_MAX){JS_FreeValue(ctx,record);JS_ThrowRangeError(ctx,"Worker policy allowlist overflow");goto fail;}
        if(JS_DefinePropertyValueUint32(ctx,rules,at++,record,JS_PROP_C_W_E)<0)goto fail;
    }
    if(!policy_field(ctx,out,"required",JS_NewBool(ctx,required))||!policy_field(ctx,out,"enforce",JS_NewBool(ctx,enforce)))goto fail;
    bool ok=policy_field(ctx,out,"policies",rules);rules=JS_UNDEFINED;if(!ok)goto fail;
    ok=policy_field(ctx,out,"requirements",requirements);requirements=JS_UNDEFINED;if(!ok)goto fail;return out;
fail:JS_FreeValue(ctx,out);JS_FreeValue(ctx,rules);JS_FreeValue(ctx,requirements);return JS_EXCEPTION;
}
static JSValue native_safety_required(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;
    return JS_NewBool(ctx,html_policy_requires_script(&state(ctx)->policy_doc));
}
static JSValue native_safety(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"Worker safety operation required");
    const char *op=JS_ToCString(ctx,argv[0]);if(!op)return JS_EXCEPTION;
    bool policy=!strcmp(op,"state"),violation=!strcmp(op,"violation");JS_FreeCString(ctx,op);
    if(policy)return worker_policy_state(ctx,w);
    if(!violation||argc<7)return JS_ThrowTypeError(ctx,"Invalid Worker safety operation");
    uint32_t index;if(JS_ToUint32(ctx,&index,argv[6])<0)return JS_EXCEPTION;
    struct web_html_policy_rule *rule=w->policy_doc.html_policy?w->policy_doc.html_policy->first:NULL;
    for(uint32_t i=0;rule&&i<index;i++)rule=rule->next;
    if(!rule)return JS_ThrowTypeError(ctx,"Invalid native Worker policy identity");
    struct policy_violation *v=js_mallocz(ctx,sizeof *v);if(!v)return JS_EXCEPTION;v->init=JS_NewObjectProto(ctx,JS_NULL);
    const char *directive=JS_ToCString(ctx,argv[2]);if(!directive){JS_FreeValue(ctx,v->init);js_free(ctx,v);return JS_EXCEPTION;}
    bool ok=!JS_IsException(v->init)&&policy_field(ctx,v->init,"effectiveDirective",JS_DupValue(ctx,argv[2]))&&
        policy_field(ctx,v->init,"violatedDirective",JS_DupValue(ctx,argv[2]))&&policy_field(ctx,v->init,"sample",JS_DupValue(ctx,argv[4]))&&
        policy_field(ctx,v->init,"disposition",JS_NewString(ctx,rule->report_only?"report":"enforce"))&&
        policy_field(ctx,v->init,"documentURI",JS_NewString(ctx,w->policy_doc.url?w->policy_doc.url:""))&&
        policy_field(ctx,v->init,"sourceFile",JS_NewString(ctx,w->policy_doc.url?w->policy_doc.url:""))&&
        policy_field(ctx,v->init,"originalPolicy",JS_NewString(ctx,rule->original_policy?rule->original_policy:""))&&
        policy_field(ctx,v->init,"blockedURI",JS_NewString(ctx,!strcmp(directive,"trusted-types")?"trusted-types-policy":"trusted-types-sink"))&&
        policy_field(ctx,v->init,"statusCode",JS_NewInt32(ctx,w->policy_status));JS_FreeCString(ctx,directive);
    if(!ok){JS_FreeValue(ctx,v->init);js_free(ctx,v);return JS_EXCEPTION;}
    v->rule=rule;
    if(w->last_violation)w->last_violation->next=v;else w->violations=v;w->last_violation=v;return JS_UNDEFINED;
}
static bool worker_policy_start(struct worker *w,JSValueConst start){
    JSContext *ctx=w->ctx;if(w->policy_initialized){JS_ThrowTypeError(ctx,"Worker policy already initialized");return false;}
    JSValue inherit=JS_GetPropertyUint32(ctx,start,NJW_START_INHERIT_POLICY),headers=JS_UNDEFINED,url=JS_UNDEFINED,status=JS_UNDEFINED,origin=JS_UNDEFINED;
    bool ok=false;if(!JS_IsBool(inherit)){JS_ThrowTypeError(ctx,"Missing native Worker policy metadata");goto done;}
    headers=JS_GetPropertyUint32(ctx,start,JS_ToBool(ctx,inherit)?NJW_START_CREATOR_POLICY:2);
    url=JS_GetPropertyUint32(ctx,start,1);status=JS_GetPropertyUint32(ctx,start,0);origin=JS_GetPropertyUint32(ctx,start,NJW_START_CREATOR_ORIGIN);
    if(!JS_IsString(headers)||!JS_IsString(url)||!JS_IsString(origin)){JS_ThrowTypeError(ctx,"Invalid native Worker policy metadata");goto done;}
    if(JS_ToInt32(ctx,&w->policy_status,status)<0)goto done;
    size_t origin_size=0;const char *text=JS_ToCString(ctx,headers),*address=JS_ToCString(ctx,url),*creator=JS_ToCStringLen(ctx,&origin_size,origin);
    if(!text||!address||!creator){JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);JS_FreeCString(ctx,creator);goto done;}
    if(memchr(creator,0,origin_size)){JS_ThrowTypeError(ctx,"Invalid native Worker creator origin");JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);JS_FreeCString(ctx,creator);goto done;}
    /* Blob's authority is the parent's immutable creator-origin snapshot, not
       an origin parsed out of author URL text. Keep script/CSP URLs unchanged. */
    bool blob=!strncasecmp(address,"blob:",5);char *security=NULL;
    if(blob){security=origin_size==SIZE_MAX?NULL:malloc(origin_size+1);if(security)memcpy(security,creator,origin_size+1);}
    size_t n=strlen(address);char *copy=n==SIZE_MAX?NULL:malloc(n+1);if(copy)memcpy(copy,address,n+1);
    if(!copy||(blob&&!security)){free(copy);free(security);JS_ThrowOutOfMemory(ctx);}
    else{
        w->policy_doc.url=copy;
        bool loaded=JS_ToBool(ctx,inherit)?html_policy_snapshot_init(&w->policy_doc,text):html_policy_init(&w->policy_doc,text,NULL);
        if(!loaded){free(copy);free(security);w->policy_doc.url=NULL;html_policy_free(&w->policy_doc);JS_ThrowOutOfMemory(ctx);}
        else{w->security_url=security;w->policy_initialized=ok=true;}
    }
    JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);JS_FreeCString(ctx,creator);
done:JS_FreeValue(ctx,inherit);JS_FreeValue(ctx,headers);JS_FreeValue(ctx,url);JS_FreeValue(ctx,status);JS_FreeValue(ctx,origin);return ok;
}
static JSValue worker_code_check(JSContext *ctx,JSValueConst value,int kind,int argc,JSValueConst *argv,void *opaque){
    struct worker *w=opaque;
    if(!html_policy_requires_script(&w->policy_doc) && (kind==JS_DYNAMIC_FUNCTION || !JS_IsObject(value)))return JS_DupValue(ctx,value);
    JSValue args_array=JS_NewArray(ctx);if(JS_IsException(args_array))return args_array;
    for(int i=0;i<argc;i++)if(JS_DefinePropertyValueUint32(ctx,args_array,(uint32_t)i,JS_DupValue(ctx,argv[i]),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,args_array);return JS_EXCEPTION;}
    JSValue args[]={JS_DupValue(ctx,value),JS_NewInt32(ctx,kind),args_array};
    /* A nested dynamic-code check must not cancel the outer task's leases. */
    JSValue fn=JS_GetPropertyStr(ctx,w->hooks,"safetyDynamicCode"),result=JS_EXCEPTION;
    if(!JS_IsException(fn))result=JS_Call(ctx,fn,JS_UNDEFINED,3,args);
    JS_FreeValue(ctx,fn);for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,args[i]);return result;
}
static void port_capture_release(struct worker*w,uint32_t id){
    struct broker_lease **at=&w->leases;while(*at&&(*at)->id!=id)at=&(*at)->next;if(!*at)return;
    struct broker_lease*l=*at;*at=l->next;struct broker_pair*p=worker_port_find(w->ports,l->port,l->creator);
    if(p){if(p->pins)p->pins--;worker_port_flush(p,output_link,w);output_ports_retire(w);}free(l);
}
static void port_capture_cancel(struct worker*w){while(w->leases)port_capture_release(w,w->leases->id);}
static JSValue native_port_lease(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;struct worker*w=state(ctx);uint32_t action=0,id=0,creator=0;
    if(argc<3||JS_ToUint32(ctx,&action,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0||JS_ToUint32(ctx,&creator,argv[2])<0)return JS_EXCEPTION;
    if(action==1){port_capture_release(w,id);return JS_UNDEFINED;}
    struct broker_pair*p=worker_port_find(w->ports,id,creator);
    /* An EOF may precede the queued port-close task. postMessage still
     * serializes/transfers, but a disappeared peer cannot be a destination. */
    if(!action&&w->shared&&p&&p->client&&!((struct client*)p->client)->active)return JS_NewInt32(ctx,0);
    if(action||w->closed||!p||p->closed)return JS_ThrowTypeError(ctx,"Worker capture target is inactive");
    uint32_t identifier=worker_port_next_lease(w->leases,w->next_lease);
    if(!identifier||p->pins==SIZE_MAX)return JS_ThrowRangeError(ctx,"Worker capture representation overflow");
    struct broker_lease*l=calloc(1,sizeof*l);if(!l)return JS_ThrowOutOfMemory(ctx);w->next_lease=l->id=identifier;l->port=id;l->creator=creator;l->next=w->leases;w->leases=l;p->pins++;return JS_NewUint32(ctx,l->id);
}
static JSValue native_port_send(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;struct worker*w=state(ctx);uint32_t op=0,id=0,creator=0,capture=0;
    if(argc<5||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0||JS_ToUint32(ctx,&creator,argv[2])<0||(argc>5&&JS_ToUint32(ctx,&capture,argv[5])<0))return JS_EXCEPTION;
    if(w->closed||(w->shared&&op==NJW_MESSAGE)||(op!=NJW_MESSAGE&&op!=NJW_PORT_MESSAGE&&op!=NJW_PORT_CLOSE)||(op!=NJW_MESSAGE&&(!id||creator>1)))return JS_ThrowTypeError(ctx,"Worker broker destination inactive or invalid");
    struct broker_pair*pair=NULL,*ports=NULL;
    if(op==NJW_MESSAGE){if(capture)return JS_ThrowTypeError(ctx,"Capture requires a Port message");if(!worker_port_metadata(ctx,argv[3],1,w->ports,UINT32_MAX,&ports))return JS_EXCEPTION;}
    else {pair=worker_port_find(w->ports,id,creator);struct broker_lease*l=w->leases;while(l&&l->id!=capture)l=l->next;bool pinned=capture&&l&&l->port==id&&l->creator==creator;
        if(!pair||(capture&&(!pinned||op!=NJW_PORT_MESSAGE))||(op==NJW_PORT_MESSAGE?(!pinned&&pair->closed):pair->closed))return JS_ThrowTypeError(ctx,"Worker port is closed, unknown or capture is invalid");}
    size_t length=0;uint8_t*data=JS_WriteObject(ctx,&length,argv[3],JS_WRITE_OBJ_REFERENCE);if(!data){output_ports_free(w,ports);return JS_EXCEPTION;}
    if(length>NJW_MAX_BYTES||length>SIZE_MAX-sizeof(struct njw_header)||length+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(ctx,data);output_ports_free(w,ports);return JS_ThrowRangeError(ctx,"Worker broker output accounting overflow");}
    struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(ctx,data);output_ports_free(w,ports);return JS_ThrowOutOfMemory(ctx);}p->data=data;p->client=w->shared?pair->client:NULL;p->h=(struct njw_header){NJW_MAGIC,op,p->client?p->client->generation:w->generation,w->shared?pair->client_id:id,op==NJW_MESSAGE?NJW_WITH_PORTS:creator,(uint32_t)length};
    if(!output_wait(w,p->client,sizeof p->h+length)){js_free(ctx,data);free(p);output_ports_free(w,ports);return JS_ThrowTypeError(ctx,"Worker transport ended or its task deadline expired before publication");}
    struct output_publication t={w,p,ports,op==NJW_PORT_CLOSE?pair:NULL,NULL};JSValue r=worker_transfer_commit_publish(ctx,argv[4],!w->closed,output_publish,&t);
    if(JS_IsException(r)){js_free(ctx,data);free(p);output_ports_free(w,ports);}else{
        output_flush(w);
        /* A record larger than one copy quantum must keep progressing even
         * when the sender next enters an endless pure-JS loop. This native
         * wait only pumps transport, after the one irreversible commit. */
        if((op==NJW_MESSAGE||op==NJW_PORT_MESSAGE)&&sizeof(struct njw_header)+length>16384)output_wait(w,w->shared?pair->client:NULL,NJW_TRANSPORT_WINDOW+1u);
    }return r;
}
static bool native_port_receive(struct worker*w,const struct njw_header*h,JSValueConst value){
    if((h->op==NJW_MESSAGE||h->op==NJW_CONNECT)&&h->kind==NJW_WITH_PORTS){if(w->shared)return shared_metadata(w,value);struct broker_pair*ports=NULL;if(!worker_port_metadata(w->ctx,value,0,w->ports,UINT32_MAX,&ports))return false;while(ports){struct broker_pair*p=ports;ports=p->next;p->next=w->ports;w->ports=p;}return true;}
    if(h->op!=NJW_PORT_MESSAGE&&h->op!=NJW_PORT_CLOSE&&h->op!=NJW_PORT_DRAINED)return true;
    struct broker_pair*pair=worker_port_find(w->ports,h->request,h->kind);if(!pair||(h->op==NJW_PORT_MESSAGE&&pair->sealed))return false;
    if(h->op!=NJW_PORT_MESSAGE){pair->closed=pair->sealed=true;
        if(h->op==NJW_PORT_CLOSE){size_t size=0;uint8_t*data=JS_WriteObject(w->ctx,&size,JS_NULL,JS_WRITE_OBJ_REFERENCE);if(!data){w->closed=true;return false;}
            if(size>SIZE_MAX-sizeof(struct njw_header)||size+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(w->ctx,data);w->closed=true;JS_ThrowRangeError(w->ctx,"Worker acknowledgement accounting overflow");return false;}
            struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(w->ctx,data);w->closed=true;JS_ThrowOutOfMemory(w->ctx);return false;}p->data=data;p->client=w->shared?pair->client:NULL;p->h=(struct njw_header){NJW_MAGIC,NJW_PORT_DRAINED,p->client?p->client->generation:w->generation,w->shared?pair->client_id:h->request,h->kind,(uint32_t)size};struct output_publication t={w,p,NULL,NULL,pair};output_publish(&t);
        }output_ports_retire(w);
    }return true;
}
static JSValue native_transfer_commit(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(!argc)return JS_ThrowTypeError(ctx,"Worker transfer plan required");return worker_transfer_commit(ctx,argv[0],!state(ctx)->closed);}
static JSValue native_send(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;struct worker *w=state(ctx);uint32_t op=0,request=0;if(argc<3||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&request,argv[1])<0)return JS_EXCEPTION;if(op!=NJW_MESSAGE&&op!=NJW_ERROR&&op!=NJW_CONSOLE)return JS_ThrowTypeError(ctx,"Invalid worker send");if(!send_value(w,op,request,0,argv[2]))return JS_ThrowRangeError(ctx,"Worker send quota/transport failed");bool ok=true;if(w->shared){for(struct client*c=w->clients;c&&ok;c=c->next)if(c->active&&c->established)ok=output_wait(w,c,0);}else ok=output_wait(w,NULL,0);if(!ok)return JS_ThrowTypeError(ctx,"Worker transport ended or its task deadline expired");return JS_UNDEFINED;}
static JSValue native_class(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;return JS_NewUint32(ctx,argc?JS_GetClassID(argv[0]):0);}
static JSValue native_detach(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(argc)JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;}
static JSValue native_now(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;return JS_NewFloat64(ctx,(double)uptime_ms());}
static JSValue native_close(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;struct worker *w=state(ctx);send_value(w,NJW_CLOSE,0,0,JS_NULL);w->closed=true;if(w->shared)close(0);return JS_UNDEFINED;}
static JSValue native_eval(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(argc<2)return JS_ThrowTypeError(ctx,"Worker source/URL required");size_t n;const char *source=JS_ToCStringLen(ctx,&n,argv[0]),*url=JS_ToCString(ctx,argv[1]);if(!source||!url){JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);return JS_EXCEPTION;}
    uint64_t saved=w->until;w->until=uptime_ms()+NJW_TASK_MS;JSValue code=JS_Eval(ctx,source,n,url,JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);w->until=saved;
    if(JS_IsException(code))return code;return JS_EvalFunction(ctx,code);
}
static JSValue native_request(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);
    if(w->closed)return JS_ThrowTypeError(ctx,"Worker closed");
    if(!argc||!JS_IsString(argv[0]))return JS_ThrowTypeError(ctx,"URL string required");
    if(w->request==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker request identifier space exhausted");
    uint32_t kind=2u;
    if(argc>1&&JS_ToBool(ctx,argv[1]))kind|=NJW_LOAD_NO_CORS;
    if(argc>2&&JS_ToBool(ctx,argv[2]))kind|=NJW_LOAD_NO_REFERRER;
    uint32_t id=++w->request;
    JSValue packet=JS_NewArray(ctx);if(JS_IsException(packet))return packet;
    bool ok=true;
    for(unsigned i=0;i<4&&ok;i++){
        JSValue value=i==0?JS_DupValue(ctx,argv[0]):i==1?argc>3?JS_DupValue(ctx,argv[3]):JS_NewString(ctx,"GET"):
            i==2?argc>4?JS_DupValue(ctx,argv[4]):JS_NewString(ctx,""):argc>5?JS_DupValue(ctx,argv[5]):JS_NULL;
        if(JS_IsException(value)||JS_DefinePropertyValueUint32(ctx,packet,i,value,JS_PROP_C_W_E)<0)ok=false;
    }
    if(ok)ok=send_value(w,NJW_LOAD,id,kind,packet);JS_FreeValue(ctx,packet);
    if(!ok)return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowTypeError(ctx,"Worker fetch IPC failed");
    output_flush(w);
    return JS_NewUint32(ctx,id);
}
static JSValue native_context_url(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;struct worker*w=state(ctx);return JS_NewString(ctx,w->policy_doc.url?w->policy_doc.url:"");
}
static JSValue native_security_url(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;struct worker*w=state(ctx);
    return JS_NewString(ctx,w->policy_initialized?(w->security_url?w->security_url:w->policy_doc.url):"");
}
static JSValue native_cancel(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);uint32_t id;
    if(!argc)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(JS_ToUint32(ctx,&id,argv[0]))return JS_EXCEPTION;
    if(!id)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(w->closed)return JS_UNDEFINED;
    if(!send_value(w,NJW_CANCEL,id,0,JS_NULL)){w->closed=w->output_failed=true;return JS_ThrowTypeError(ctx,"Worker cancellation IPC failed");}
    output_flush(w);
    return JS_UNDEFINED;
}
static JSValue native_import(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"URL required");if(w->request==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker request identifier space exhausted");uint32_t id=++w->request;if(!send_value(w,NJW_LOAD,id,1,argv[0]))return JS_ThrowTypeError(ctx,"Worker import IPC failed");
    uint64_t wait_start=uptime_ms(),deadline=wait_start+WEBNET_TIMEOUT_MS;
    while(!w->closed){uint64_t now=uptime_ms();if(now>=deadline){send_value(w,NJW_CANCEL,id,0,JS_NULL);return JS_ThrowTypeError(ctx,"Worker import deadline exceeded");}uint64_t left=deadline-now;int timeout=left>(uint64_t)INT_MAX?INT_MAX:(int)left;struct deferred*d=NULL;
        if(w->shared){
            struct deferred**at=&w->head;while(*at&&((*at)->h.op!=NJW_LOADED||(*at)->h.request!=id))at=&(*at)->next;
            if(*at){d=*at;*at=d->next;w->queued-=d->h.bytes;w->tail=NULL;for(struct deferred*p=w->head;p;p=p->next)w->tail=p;d->next=NULL;}
            else d=shared_receive(w,timeout);
            if(!d)continue;if(!shared_incoming(w,d)){deferred_free(d);continue;}
        }else{struct n_pollfd p[2]={{0,N_POLLIN,0},{1,w->output?N_POLLOUT:0,0}};int ready=poll(p,2,timeout);if(ready<0&&errno==EINTR)continue;if(ready<0)return JS_ThrowTypeError(ctx,"Worker import poll failed");if(!ready)continue;if(p[1].revents&N_POLLOUT)output_flush(w);if(w->closed)break;if(!(p[0].revents&(N_POLLIN|N_POLLHUP)))continue;d=receive(w);if(!d){w->closed=true;return JS_ThrowTypeError(ctx,"Worker import pipe closed");}}
        if(d->h.op==NJW_CREDIT){transport_credit(w,d);deferred_free(d);continue;}
        if(d->h.op==NJW_LOADED&&d->h.request==id){if(w->until!=UINT64_MAX)w->until+=uptime_ms()-wait_start;JSValue result=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);deferred_free(d);return result;}
        if(d->h.bytes>SIZE_MAX-w->queued){deferred_free(d);return JS_ThrowRangeError(ctx,"Worker deferred message accounting overflow");}w->queued+=d->h.bytes;if(w->tail)w->tail->next=d;else w->head=d;w->tail=d;
    }return JS_ThrowTypeError(ctx,"Worker closed");
}
static int interrupt(JSRuntime *rt,void *opaque){
    (void)rt;struct worker*w=opaque;if(w->closed)return 1;
    /* QuickJS already amortizes bytecode checks. Further amortize the native
     * clock syscall, not cancellation: process kill and closed remain live.
     * Import waits are charged separately and preserve the same deadline. */
    if(++w->interrupt_ticks&15u)return 0;
    if(w->shared&&shared_without_owners(w)){w->closed=true;return 1;}
    if(w->until==UINT64_MAX)return 0;
    return uptime_ms()>=w->until;
}
static JSValue hook(struct worker *w,const char *name,int argc,JSValueConst *argv){JSValue fn=JS_GetPropertyStr(w->ctx,w->hooks,name);JSValue r=JS_Call(w->ctx,fn,JS_UNDEFINED,argc,argv);JS_FreeValue(w->ctx,fn);port_capture_cancel(w);return r;}
static void exception(struct worker *w){JSValue ex=JS_GetException(w->ctx);w->until=uptime_ms()+NJW_TASK_MS;JSValue r=hook(w,"report",1,&ex);if(JS_IsException(r)){JSValue ignored=JS_GetException(w->ctx);JS_FreeValue(w->ctx,ignored);}JS_FreeValue(w->ctx,r);JS_FreeValue(w->ctx,ex);}
static void jobs(struct worker *w){for(unsigned i=0;i<256&&!w->closed&&JS_IsJobPending(w->rt);i++){JSContext *ctx=NULL;int n=JS_ExecutePendingJob(w->rt,&ctx);port_capture_cancel(w);if(n<0){exception(w);break;}if(!n)break;}}
static void policy_task(struct worker *w){
    struct policy_violation *v=w->violations;w->violations=v->next;if(!w->violations)w->last_violation=NULL;
    struct web_html_policy_rule *rule=v->rule;
    if(!rule->from_meta&&(rule->report_uri_count||rule->report_to)){
        JSContext *ctx=w->ctx;JSValue report=JS_NewObjectProto(ctx,JS_NULL),endpoints=JS_NewArray(ctx);
        bool ok=!JS_IsException(report)&&!JS_IsException(endpoints)&&JS_SetPrototype(ctx,endpoints,JS_NULL)>=0;
        /* report-to takes precedence. Reporting API delivery is not a legacy
         * report-uri POST; the trusted parent reports unsupported delivery. */
        for(size_t i=0;ok&&!rule->report_to&&i<rule->report_uri_count;i++){
            if(i>=UINT32_MAX){JS_ThrowRangeError(ctx,"Worker report endpoint representation overflow");ok=false;break;}
            JSValue uri=JS_NewString(ctx,rule->report_uris[i]);
            if(JS_IsException(uri)||JS_DefinePropertyValueUint32(ctx,endpoints,(uint32_t)i,uri,JS_PROP_C_W_E)<0)ok=false;
        }
        if(ok)ok=policy_field(ctx,report,"init",JS_DupValue(ctx,v->init));
        if(ok){ok=policy_field(ctx,report,"endpoints",endpoints);endpoints=JS_UNDEFINED;}
        if(ok)ok=policy_field(ctx,report,"reportTo",JS_NewString(ctx,rule->report_to?rule->report_to:""));
        if(ok)ok=send_value(w,NJW_CSP_REPORT,0,0,report);
        if(!ok&&JS_HasException(ctx)){JSValue ignored=JS_GetException(ctx);JS_FreeValue(ctx,ignored);}
        JS_FreeValue(ctx,endpoints);JS_FreeValue(ctx,report);
    }
    JSValue log=JS_NewObject(w->ctx);
    if(!JS_IsException(log)){JS_DefinePropertyValueStr(w->ctx,log,"message",JS_NewString(w->ctx,"Trusted Types CSP violation in Worker"),JS_PROP_C_W_E);send_value(w,NJW_CONSOLE,0,0,log);}
    else{JSValue ignored=JS_GetException(w->ctx);JS_FreeValue(w->ctx,ignored);}JS_FreeValue(w->ctx,log);
    JSValue r=hook(w,"safetyViolation",1,&v->init);if(JS_IsException(r))exception(w);JS_FreeValue(w->ctx,r);JS_FreeValue(w->ctx,v->init);js_free(w->ctx,v);
}
int main(int argc,char**argv){
    struct worker w={0};w.shared=argc>1&&!strcmp(argv[1],"--shared-server");w.script_url=argc>2?argv[2]:"";w.hooks=JS_UNDEFINED;w.rt=JS_NewRuntime();if(!w.rt)return 1;JS_SetMemoryLimit(w.rt,NJW_HEAP_BYTES);JS_SetMaxStackSize(w.rt,512u*1024u);JS_SetCanBlock(w.rt,false);JS_SetInterruptHandler(w.rt,interrupt,&w);w.ctx=JS_NewContext(w.rt);if(!w.ctx){JS_FreeRuntime(w.rt);return 1;}JS_SetContextOpaque(w.ctx,&w);w.until=uptime_ms()+10000;
    if(trusted_native_init(w.ctx)<0)goto shutdown;
    JSValue global=JS_GetGlobalObject(w.ctx),host=JS_NewObject(w.ctx);
    const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),
        JS_CFUNC_DEF("safetyRequired",1,native_safety_required),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("classID",1,native_class),
        JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("portSend",6,native_port_send),
        JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("url",0,native_context_url),JS_CFUNC_DEF("securityURL",0,native_security_url),
        JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("request",6,native_request),JS_CFUNC_DEF("cancel",1,native_cancel),
        JS_CFUNC_DEF("import",1,native_import),JS_CFUNC_DEF("eval",2,native_eval)};
    JS_SetPropertyFunctionList(w.ctx,host,funcs,sizeof funcs/sizeof funcs[0]);
    web_js_encoding_init_isolated(w.ctx,host);web_js_navigator_init(w.ctx,host);web_js_crypto_init(w.ctx,host);
    JS_SetPropertyStr(w.ctx,global,"__workerHost",host);JS_FreeValue(w.ctx,global);
    w.hooks=JS_Eval(w.ctx,js_worker_runtime,sizeof js_worker_runtime-1,"<worker-bootstrap>",JS_EVAL_TYPE_GLOBAL);if(JS_IsException(w.hooks))goto shutdown;
    JS_SetDynamicCodeCheck(w.ctx,worker_code_check,&w);
    if(!w.shared&&fcntl(1,F_SETFL,O_NONBLOCK)<0)goto shutdown;
    while(!w.closed||w.output){
        if(w.output_failed)break;
        if(w.closed){if(w.shared){output_flush(&w);if(w.output){struct deferred*d=shared_receive(&w,10);if(d)deferred_free(d);}continue;}struct n_pollfd p={1,N_POLLOUT,0};if(poll(&p,1,-1)<0){if(errno==EINTR)continue;break;}if(p.revents&N_POLLOUT)output_flush(&w);else break;continue;}
        if(w.output)output_flush(&w);
        if(w.output_failed)break;
        if(JS_IsJobPending(w.rt)){w.until=uptime_ms()+NJW_TASK_MS;jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        /* CSP events are separate tasks, after the originating microtasks. */
        if(w.violations){w.until=uptime_ms()+NJW_TASK_MS;policy_task(&w);jobs(&w);continue;}
        struct deferred *d=NULL;if(w.head){d=w.head;w.head=d->next;if(!w.head)w.tail=NULL;w.queued-=d->h.bytes;}
        else{
            w.until=UINT64_MAX;JSValue deadline=hook(&w,"deadline",0,NULL);int64_t due=-1;JS_ToInt64(w.ctx,&due,deadline);JS_FreeValue(w.ctx,deadline);uint64_t now=uptime_ms();int wait=JS_IsJobPending(w.rt)?0:due<0?-1:due<=(int64_t)now?0:(int)(due-now);
            if(w.shared){d=shared_receive(&w,wait);if(w.closed&&!d)continue;}
            else{struct n_pollfd p[2]={{0,N_POLLIN,0},{1,w.output?N_POLLOUT:0,0}};int n=poll(p,2,wait);if(n<0&&errno==EINTR)continue;if(n<0)break;if(p[1].revents&N_POLLOUT)output_flush(&w);if(w.output_failed)break;if(p[0].revents&(N_POLLIN|N_POLLHUP)){d=receive(&w);if(!d)break;}}
        }
        w.until=uptime_ms()+NJW_TASK_MS;
        if(d&&d->h.op==NJW_CREDIT){if(!w.shared||shared_incoming(&w,d))transport_credit(&w,d);deferred_free(d);d=NULL;if(w.closed)continue;}
        bool received_task=d!=NULL;
        /* The running JS task holds current_client until its checkpoint; the
         * packet's ownership pin is released exactly once by deferred_free. */
        if(d){w.current_client=d->client;bool allowed=!w.shared||shared_incoming(&w,d);JSValue value=allowed?JS_ReadObject(w.ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE):JS_UNDEFINED;if(JS_IsException(value))exception(&w);else if(allowed&&!native_port_receive(&w,&d->h,value)){JS_FreeValue(w.ctx,value);if(JS_HasException(w.ctx))exception(&w);}else if(allowed){JSValue result;if(d->h.op==NJW_LOADED){JSValue args[]={JS_NewUint32(w.ctx,d->h.request),value,JS_NewUint32(w.ctx,d->h.kind)};result=hook(&w,"loaded",3,args);JS_FreeValue(w.ctx,args[0]);JS_FreeValue(w.ctx,args[2]);}else if(d->h.op==NJW_START)result=worker_policy_start(&w,value)?hook(&w,"start",1,&value):JS_EXCEPTION;else{JSValue args[]={value,JS_NewUint32(w.ctx,d->h.op),JS_NewUint32(w.ctx,d->h.request),JS_NewUint32(w.ctx,d->h.kind)};result=hook(&w,"receive",4,args);for(unsigned i=1;i<4;i++)JS_FreeValue(w.ctx,args[i]);}if(JS_IsException(result)){exception(&w);if(d->h.op==NJW_START){send_value(&w,NJW_CLOSE,0,0,JS_NULL);w.closed=true;}}else if(w.shared&&d->h.op==NJW_START){if(w.before_start){if(w.tail)w.tail->next=w.before_start;else w.head=w.before_start;w.tail=w.before_start_tail;w.before_start=w.before_start_tail=NULL;}}JS_FreeValue(w.ctx,result);JS_FreeValue(w.ctx,value);}deferred_free(d);w.current_client=NULL;}
        /* Finish the received task's microtasks before a local port/timer task. */
        if(received_task){jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        JSValue now=JS_NewFloat64(w.ctx,(double)uptime_ms());JSValue tick=hook(&w,"tick",1,&now);if(JS_IsException(tick))exception(&w);JS_FreeValue(w.ctx,tick);JS_FreeValue(w.ctx,now);jobs(&w);
    }
shutdown:
    while(w.violations){struct policy_violation *v=w.violations;w.violations=v->next;JS_FreeValue(w.ctx,v->init);js_free(w.ctx,v);}
    html_policy_free(&w.policy_doc);free(w.policy_doc.url);free(w.security_url);
    while(w.output){struct outgoing*p=w.output;w.output=p->next;output_packet_free(&w,p);}
    while(w.head){struct deferred*d=w.head;w.head=d->next;deferred_free(d);}while(w.before_start){struct deferred*d=w.before_start;w.before_start=d->next;deferred_free(d);}output_ports_free(&w,w.ports);while(w.leases){struct broker_lease*l=w.leases;w.leases=l->next;free(l);}while(w.loads){struct pending_load*p=w.loads;w.loads=p->next;if(p->client)p->client->references--;free(p);}while(w.clients){struct client*c=w.clients;w.clients=c->next;if(c->active)close(c->fd);free(c->data);free(c);}free(w.pollfds);free(w.pollclients);JS_FreeValue(w.ctx,w.hooks);JS_FreeContext(w.ctx);JS_FreeRuntime(w.rt);return 0;
}
