/* Dedicated QuickJS realm in a native Nocturne child task. The only author
 * capabilities are data-only IPC to the browser; no libc/native modules. */
#include <nocturne.h>
#include <quickjs.h>
#include <js_worker_wire.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <webnet.h>
#include "../libc/web/js_encoding.h"
#include "../libc/web/js_url.h"
#include "../libc/web/js_crypto.h"
#include "../libc/web/js_compression.h"
#include "../libc/web/js_wasm.h"
#include "../libc/web/js_webgl.h"
#include "../libc/web/js_atomics.h"
#include "../libc/web/js_memory.h"
#include "../libc/web/js_opfs.h"
#include "../libc/web/js_indexeddb.h"
#include "../libc/web/js_cachestorage.h"
#include "../libc/web/js_worker_transfer.h"
#include "../libc/web/js_worker_port_lifetime.h"
#include "../libc/web/js_worker_runtime.inc"
#include "../libc/web/js_webidl.h"
#include "../libc/web/js_navigator_native.h"
#include "../libc/web/html_policy.h"
#include "../libc/web/js_html_safety_native.h"
/* Records travel as channel messages, one record each. A dedicated worker's
 * page channel is handle CHANNEL; a shared server's CHANNEL is its listener
 * and every owner page is a client with a channel of its own. All are watched
 * in one queue with the Atomics.waitAsync memory words. */
#define CHANNEL 0
#define INPUT_TOKEN 1u
#define CLIENT_TOKEN (UINT64_C(0x43) << 56)
struct client {struct client *next;n_handle channel;uint64_t token;uint32_t generation,watched;size_t references,in_flight;struct web_inbox inbox;bool established,active,blocked,wants_output,readable;};
struct pending_load {struct pending_load*next;uint32_t id;struct client*client;};
struct deferred {struct deferred *next;struct njw_header h;uint8_t *data;struct client*client;struct web_inbox inbox;bool normalized;};
struct outgoing {struct outgoing *next;struct njw_header h;uint8_t *data;struct client*client;struct web_attachments attach;size_t attach_sent;};
struct policy_violation {struct policy_violation *next;JSValue init;struct web_html_policy_rule *rule;};
struct worker {JSRuntime *rt;JSContext *ctx;JSValue hooks;uint32_t generation,request,interrupt_ticks,next_shared_port;uint64_t until;bool closed,established,output_failed,policy_initialized,shared,had_client,need_start;struct deferred *head,*tail,*before_start,*before_start_tail;struct outgoing *output,*last_output;size_t queued,output_bytes,in_flight;uint32_t next_lease;struct broker_lease *leases;struct broker_pair *ports;struct client*clients,*current_client,*network_client;struct pending_load*loads;n_handle queue,probe;struct web_inbox inbox;uint64_t next_client;uint32_t input_watched;const char*script_url;char*security_url;web_doc policy_doc;struct policy_violation *violations,*last_violation;int policy_status;uint64_t languages_seen;struct web_idb indexed_db;bool indexed_db_turn;};
struct output_publication {struct worker *worker;struct outgoing *packet;struct broker_pair *ports,*close_pair,*ack_pair;};
static void output_link(void *opaque,void *packet){struct worker*w=opaque;struct outgoing*p=packet;size_t charge=sizeof p->h+p->h.bytes;if(p->client)p->client->in_flight+=charge;else w->in_flight+=charge;if(w->last_output)w->last_output->next=p;else w->output=p;w->last_output=p;}
static void output_packet_free(struct worker*w,void *opaque){struct outgoing*p=opaque;if(p){if(p->client)p->client->references--;web_attachments_release(&p->attach);js_free(w->ctx,p->data);free(p);}}
static void output_ports_free(struct worker*w,struct broker_pair*p){while(p){struct broker_pair*n=p->next;output_packet_free(w,p->held_close);output_packet_free(w,p->held_ack);if(p->client)((struct client*)p->client)->references--;free(p);p=n;}}
static void output_ports_retire(struct worker*w){struct broker_pair **at=&w->ports;while(*at){struct broker_pair*p=*at;if(worker_port_retirable(p)){*at=p->next;if(p->client)((struct client*)p->client)->references--;free(p);}else at=&p->next;}}
static void deferred_free(struct deferred*d){if(d->client)d->client->references--;web_inbox_close(&d->inbox);free(d->data);free(d);}
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
/* Whether one record can be sent to h now without waiting (or h has gone):
 * a slow page never holds the worker or another owner's records back. */
static bool sendable(struct worker*w,n_handle h){
    if(w->probe<0&&(w->probe=n_events_create())<0)return false;
    if(n_events_watch(w->probe,h,N_EVENT_SENDABLE|N_EVENT_CLOSED,1)<0)return false;
    struct n_event e;n_result ready=n_events_wait(w->probe,&e,1,0);n_events_unwatch(w->probe,1);return ready>0;
}
/* Change a watch in the worker's queue only when its interest changes. */
static bool watch(struct worker*w,n_handle h,uint64_t token,uint32_t*watched,uint32_t interest){
    if(*watched==interest)return true;
    n_result r=interest?n_events_watch(w->queue,h,interest,token):n_events_unwatch(w->queue,token);
    if(r<0)return false;*watched=interest;return true;
}
/* Send whole records up to a copy budget (at least one), in order per target. */
static void output_flush(struct worker*w){size_t budget=16384;struct outgoing**at=&w->output;for(struct client*c=w->clients;c;c=c->next)c->blocked=false;
    while(*at&&budget){struct outgoing*p=*at;struct client*c=p->client;if(w->shared&&c&&c->blocked){at=&p->next;continue;}
        if(w->shared&&(!c||!c->active)){output_unlink(w,at,p);continue;}
        n_handle h=w->shared?c->channel:CHANNEL;if(!sendable(w,h)){if(!w->shared)break;c->blocked=true;at=&p->next;continue;}
        /* A shared region goes first as a bare carrier message, one per
           attachment, each step checked for room; the payload follows. */
        if(p->attach_sent<p->attach.count){struct njw_header carrier={NJW_MAGIC,NJW_ATTACH,p->h.generation,0,0,0};
            if(n_channel_send(h,&carrier,sizeof carrier,web_attachments_handle(&p->attach,p->attach_sent))<0){if(!w->shared){w->closed=w->output_failed=true;break;}shared_disconnect(w,c);continue;}
            p->attach_sent++;budget-=sizeof carrier<budget?sizeof carrier:budget;continue;}
        size_t size=sizeof p->h+p->h.bytes;uint8_t*record=malloc(size);if(!record){w->closed=w->output_failed=true;break;}
        memcpy(record,&p->h,sizeof p->h);memcpy(record+sizeof p->h,p->data,p->h.bytes);n_result sent=n_channel_send(h,record,size,-1);free(record);
        if(sent<0){if(!w->shared){w->closed=w->output_failed=true;break;}shared_disconnect(w,c);continue;}
        budget-=size<budget?size:budget;output_unlink(w,at,p);
    }w->last_output=NULL;for(struct outgoing*p=w->output;p;p=p->next)w->last_output=p;
}
/* The next whole record on h, or NULL: none waits yet, or (*failed) h has
 * gone or sent a malformed or unrepresentable record. A bare header in front
 * of it is an attachment carrier: its handle waits in inbox and goes to the
 * record that follows. */
static struct deferred *record_receive(struct worker*w,n_handle h,struct web_inbox*inbox,bool*failed){
    for(;;){
    n_result size=n_channel_receive(h,NULL,0,N_CHANNEL_PEEK,NULL);if(size==-N_ERR_NOT_READY)return NULL;
    if(size==(n_result)sizeof(struct njw_header)){struct njw_header carrier;n_handle attached=-1;
        if(n_channel_receive(h,&carrier,sizeof carrier,0,&attached)!=size||carrier.magic!=NJW_MAGIC||carrier.op!=NJW_ATTACH||carrier.bytes||attached<0||!web_inbox_add(inbox,attached)){if(attached>=0)n_handle_close(attached);*failed=true;return NULL;}
        continue;}
    struct deferred*d=NULL;uint8_t*bytes=NULL;size_t payload=size>0?(size_t)size-sizeof d->h:0;
    if(size<0||(size_t)size<=sizeof d->h||payload>NJW_MAX_BYTES||payload>SIZE_MAX-w->queued)goto bad;
    if(!(d=calloc(1,sizeof*d))||!(bytes=malloc((size_t)size))||n_channel_receive(h,bytes,(size_t)size,0,NULL)!=size)goto bad;
    memcpy(&d->h,bytes,sizeof d->h);if(d->h.bytes!=payload)goto bad;
    memmove(bytes,bytes+sizeof d->h,payload);d->data=bytes;d->inbox=*inbox;*inbox=(struct web_inbox){0};return d;
bad:free(bytes);free(d);*failed=true;return NULL;
    }
}
/* Wait on the page channel and Atomics.waitAsync words: a record, room for
 * output, the page leaving or the timeout. False when the wait failed. */
static bool dedicated_wait(struct worker*w,int timeout){
    uint32_t interest=(w->closed?N_EVENT_CLOSED:N_EVENT_RECEIVABLE)|(w->output?N_EVENT_SENDABLE:0);
    if(!watch(w,CHANNEL,INPUT_TOKEN,&w->input_watched,interest))return false;
    struct n_event events[64];n_result n=n_events_wait(w->queue,events,64,timeout);if(n<0)return false;
    web_atomics_events(events,(size_t)n);webprefs_events(events,(size_t)n);return true;
}
static bool send_client(struct worker*w,struct client*c,unsigned op,unsigned request,unsigned kind,JSValueConst value){size_t n=0;uint8_t *data=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!data)return false;if(n>NJW_MAX_BYTES||n>SIZE_MAX-sizeof(struct njw_header)||n+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(w->ctx,data);return false;}struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(w->ctx,data);return false;}p->data=data;p->client=c;p->h=(struct njw_header){NJW_MAGIC,op,c?c->generation:w->generation,request,kind,(uint32_t)n};struct output_publication t={.worker=w,.packet=p};output_publish(&t);return true;}
static bool send_value(struct worker*w,unsigned op,unsigned request,unsigned kind,JSValueConst value){
    if(!w->shared)return send_client(w,NULL,op,request,kind,value);
    if(op==NJW_ERROR||op==NJW_CONSOLE||op==NJW_CLOSE){bool ok=true;for(struct client*c=w->clients;c;c=c->next)if(c->active&&c->established&&!send_client(w,c,op,request,kind,value))ok=false;return ok;}
    /* A resume goes to the owner of its fetch, which stays pending; an ended one has none. */
    if(op==NJW_RESUME){struct pending_load*p=w->loads;while(p&&p->id!=request)p=p->next;return !p||!p->client||!p->client->active||send_client(w,p->client,op,request,kind,value);}
    struct client*c=w->network_client;if(!c||!c->active)return false;
    if(op==NJW_LOAD&&request){struct pending_load*p=malloc(sizeof*p);if(!p)return false;*p=(struct pending_load){w->loads,request,c};w->loads=p;c->references++;if(!send_client(w,c,op,request,kind,value)){w->loads=p->next;c->references--;free(p);return false;}return true;}
    if(op==NJW_CANCEL){struct pending_load**at=&w->loads;while(*at&&(*at)->id!=request)at=&(*at)->next;if(*at){struct pending_load*p=*at;c=p->client;*at=p->next;c->references--;free(p);}}
    return send_client(w,c,op,request,kind,value);
}
static struct deferred *receive(struct worker *w,bool*failed){
    struct deferred *d=record_receive(w,CHANNEL,&w->inbox,failed);if(!d)return NULL;
    if(d->h.magic!=NJW_MAGIC||
       ((d->h.op==NJW_PORT_MESSAGE||d->h.op==NJW_PORT_CLOSE||d->h.op==NJW_PORT_DRAINED)?d->h.kind>1:d->h.op==NJW_CONNECT?d->h.kind!=NJW_WITH_PORTS:d->h.op==NJW_MESSAGE?(d->h.kind!=0&&d->h.kind!=NJW_WITH_PORTS):d->h.op==NJW_LOADED?!njw_loaded_kind_valid(d->h.kind):d->h.op==NJW_FRAME?!njw_frame_kind_valid(d->h.kind):d->h.kind!=0)||
       (d->h.op!=NJW_START&&d->h.op!=NJW_MESSAGE&&d->h.op!=NJW_CONNECT&&d->h.op!=NJW_LOADED&&d->h.op!=NJW_FRAME&&d->h.op!=NJW_CREDIT&&d->h.op!=NJW_PORT_MESSAGE&&d->h.op!=NJW_PORT_CLOSE&&d->h.op!=NJW_PORT_DRAINED)||(w->established&&d->h.generation!=w->generation)||(!w->established&&d->h.op!=NJW_START)){deferred_free(d);*failed=true;return NULL;}
    if(!w->established){w->generation=d->h.generation;w->established=true;}else if(d->h.op==NJW_START){deferred_free(d);*failed=true;return NULL;}
    return d;
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
    if(!c||!c->active)return;c->active=false;n_handle_close(c->channel);c->channel=-1;c->watched=0;web_inbox_close(&c->inbox);
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
    struct client**at=&w->clients;while(*at){struct client*c=*at;if(c->active||c->references||w->current_client==c||w->network_client==c){at=&c->next;continue;}*at=c->next;web_inbox_close(&c->inbox);free(c);}
}
static bool shared_header_valid(const struct njw_header*h){
    bool port=h->op==NJW_PORT_MESSAGE||h->op==NJW_PORT_CLOSE||h->op==NJW_PORT_DRAINED;
    bool kind=port?h->kind<=1:h->op==NJW_CONNECT?h->kind==NJW_WITH_PORTS:h->op==NJW_LOADED?njw_loaded_kind_valid(h->kind):h->op==NJW_FRAME?njw_frame_kind_valid(h->kind):h->kind==0;
    return h->magic==NJW_MAGIC&&h->bytes&&kind&&(port||h->op==NJW_CONNECT||h->op==NJW_START||h->op==NJW_LOADED||h->op==NJW_FRAME||(h->op==NJW_CREDIT&&!h->request));
}
static bool shared_connect_url(struct worker*w,struct client*c,const struct deferred*d){
    JSValue value=JS_ReadObject(w->ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE),url=JS_IsException(value)?JS_UNDEFINED:JS_GetPropertyUint32(w->ctx,value,2);const char*address=JS_IsString(url)?JS_ToCString(w->ctx,url):NULL;bool ok=address&&!strcmp(address,w->script_url?w->script_url:"");JS_FreeCString(w->ctx,address);JS_FreeValue(w->ctx,url);JS_FreeValue(w->ctx,value);
    if(ok)return true;if(JS_HasException(w->ctx))JS_FreeValue(w->ctx,JS_GetException(w->ctx));JSValue error=JS_NewObjectProto(w->ctx,JS_NULL);JS_DefinePropertyValueStr(w->ctx,error,"message",JS_NewString(w->ctx,"SharedWorker name is associated with a different script URL"),JS_PROP_C_W_E);JS_DefinePropertyValueStr(w->ctx,error,"terminal",JS_TRUE,JS_PROP_C_W_E);bool sent=send_client(w,c,NJW_ERROR,0,0,error);JS_FreeValue(w->ctx,error);if(sent)output_flush(w);return false;
}
/* Native-only endpoint ownership. The interrupt path may use this helper:
 * never deserialize bytes, allocate JS values or dispatch an author task.
 * The count of owners accepted, or -1 when the listener failed. */
static int shared_accept(struct worker*w,unsigned quantum){
    int accepted=0;
    for(unsigned i=0;i<quantum;i++){n_handle h=n_channel_accept(CHANNEL);if(h==-N_ERR_NOT_READY)break;if(h<0){w->closed=w->output_failed=true;return -1;}
        struct client*c=calloc(1,sizeof*c);if(!c){n_handle_close(h);w->closed=w->output_failed=true;return -1;}
        c->channel=h;c->token=CLIENT_TOKEN|++w->next_client;c->active=true;c->next=w->clients;w->clients=c;w->had_client=true;accepted++;
    }return accepted;
}
static bool shared_without_owners(struct worker*w){
    if(!w->shared||!w->had_client)return false;
    /* Accept a finite batch so dead, never-parsed pending peers do not pin a
     * detached CPU-only task forever. Live new peers keep their ownership. */
    if(shared_accept(w,16)<0)return true;
    /* An owner whose page has gone and whose records were all taken. A record
     * still waiting is a live task for this server either way. */
    for(struct client*c=w->clients;c;c=c->next)if(c->active&&n_channel_receive(c->channel,NULL,0,N_CHANNEL_PEEK,NULL)!=-N_ERR_CLOSED)return false;
    /* A connection may have arrived during the bounded scan. An accepted
     * owner is protected; the next interrupt will accept the next quantum. */
    return shared_accept(w,1)==0;
}
/* One wait for the whole shared server, whole records, and a finite copy
 * quantum. A slow client never blocks another client's input or output. */
static struct deferred*shared_receive(struct worker*w,int timeout){
    shared_collect(w);
    for(struct client*c=w->clients;c;c=c->next)c->wants_output=false;for(struct outgoing*p=w->output;p;p=p->next)if(p->client)p->client->wants_output=true;
    bool watched=watch(w,CHANNEL,INPUT_TOKEN,&w->input_watched,w->closed?0:N_EVENT_RECEIVABLE);
    for(struct client*c=w->clients;c;c=c->next)if(c->active&&!watch(w,c->channel,c->token,&c->watched,N_EVENT_RECEIVABLE|(c->wants_output?N_EVENT_SENDABLE:0)))watched=false;
    /* Atomics.waitAsync notifications wake the agent like an incoming task. */
    struct n_event events[64];n_result ready=n_events_wait(w->queue,events,64,watched?timeout:0);
    if(ready<0){w->closed=w->output_failed=true;return NULL;}
    web_atomics_events(events,(size_t)ready);webprefs_events(events,(size_t)ready);
    /* A full report (or a failed watch) may leave owners unreported: try all. */
    bool all=ready==64||!watched,accept=!w->closed&&!watched,output=false;
    for(n_result i=0;i<ready;i++){uint64_t token=events[i].token;
        if(token==INPUT_TOKEN)accept=!w->closed;
        else if((token&~UINT64_C(0xffffffff))==CLIENT_TOKEN)for(struct client*c=w->clients;c;c=c->next)if(c->token==token){
            if(events[i].events&N_EVENT_SENDABLE)output=true;if(events[i].events&(N_EVENT_RECEIVABLE|N_EVENT_CLOSED))c->readable=true;break;}
    }
    if(accept&&shared_accept(w,16)<0)return NULL;
    if(output||(all&&w->output))output_flush(w);
    size_t budget=65536;
    for(struct client*c=w->clients;c&&budget&&!w->closed;c=c->next){if(!c->active||!(c->readable||all))continue;
        while(c->active&&budget){bool failed=false;struct deferred*d=record_receive(w,c->channel,&c->inbox,&failed);
            if(!d){if(failed)shared_disconnect(w,c);else c->readable=false;break;}
            budget-=sizeof d->h+d->h.bytes<budget?sizeof d->h+d->h.bytes:budget;
            if(!shared_header_valid(&d->h)||(c->established&&d->h.generation!=c->generation)){deferred_free(d);shared_disconnect(w,c);break;}
            if(!c->established){c->generation=d->h.generation;c->established=true;if(!w->network_client)w->network_client=c;}
            d->client=c;c->references++;w->queued+=d->h.bytes;
            if(d->h.op==NJW_CONNECT&&!shared_connect_url(w,c,d)){w->queued-=d->h.bytes;deferred_free(d);shared_disconnect(w,c);break;}
            if(d->h.op==NJW_CONNECT)shared_restart_source(w);
            if(!w->policy_initialized&&d->h.op!=NJW_START)defer_link(w,d,true);else{w->queued-=d->h.bytes;return d;}
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
    if(d->h.op==NJW_LOADED||d->h.op==NJW_FRAME){struct pending_load**at=&w->loads;while(*at&&((*at)->id!=d->h.request||(*at)->client!=c))at=&(*at)->next;if(!*at)return false;
        if(d->h.op==NJW_FRAME&&(d->h.kind&NJW_FRAME_EVENT)!=NJW_FRAME_END)return true;struct pending_load*p=*at;*at=p->next;c->references--;free(p);return true;}
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
        uint64_t now=n_clock_uptime_ms();if(w->until!=UINT64_MAX&&now>=w->until)return false;uint64_t remaining=w->until==UINT64_MAX?UINT64_MAX:w->until-now;int wait=remaining>(uint64_t)INT_MAX?INT_MAX:(int)remaining;struct deferred*d=NULL;
        /* An import wait may already have retained a private credit record. */
        struct deferred**at=&w->head;while(*at&&(*at)->h.op!=NJW_CREDIT)at=&(*at)->next;
        if(*at){d=*at;*at=d->next;w->queued-=d->h.bytes;w->tail=NULL;for(struct deferred*p=w->head;p;p=p->next)w->tail=p;d->next=NULL;}
        else if(w->shared)d=shared_receive(w,wait);
        else{if(!dedicated_wait(w,wait)){w->closed=true;return false;}if(w->output)output_flush(w);bool failed=false;d=receive(w,&failed);if(failed){w->closed=true;return false;}}
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
/* OPFS storage key: a blob Worker's creator snapshot, else its policy URL. */
static const char *opfs_storage_url(JSContext *ctx){struct worker*w=state(ctx);return w&&w->policy_initialized?(w->security_url?w->security_url:w->policy_doc.url):NULL;}
/* IndexedDB (js_indexeddb.c): the Window's storage owner, reached from this realm the same way.
   Its storage key is made from the same URL as OPFS's, by the Window's algorithm (js_url.js's
   storageOrigin, called directly: hook() would cancel the running task's port captures). */
static struct web_idb *worker_idb_state(JSContext *ctx){struct worker*w=state(ctx);return w?&w->indexed_db:NULL;}
static JSValue worker_idb_authority(JSContext *ctx){
    struct worker*w=state(ctx);
    if(!w||!w->policy_initialized)return JS_ThrowTypeError(ctx,"IndexedDB before the Worker started");
    if(w->closed)return JS_ThrowTypeError(ctx,"Worker closed");
    return JS_UNDEFINED;
}
static const char *worker_idb_origin(JSContext *ctx){
    struct worker*w=state(ctx);const char *url=opfs_storage_url(ctx);
    if(!url||!JS_IsObject(w->hooks))return NULL;
    JSValue fn=JS_GetPropertyStr(ctx,w->hooks,"storageOrigin");
    if(JS_IsException(fn))return NULL;
    if(!JS_IsFunction(ctx,fn)){JS_FreeValue(ctx,fn);JS_ThrowInternalError(ctx,"Missing private origin parser");return NULL;}
    JSValue address=JS_NewString(ctx,url);
    JSValue value=JS_IsException(address)?JS_EXCEPTION:JS_Call(ctx,fn,w->hooks,1,&address);
    JS_FreeValue(ctx,fn);JS_FreeValue(ctx,address);
    const char *origin=JS_IsException(value)||JS_IsNull(value)?NULL:JS_ToCString(ctx,value);
    JS_FreeValue(ctx,value);return origin;
}
static int worker_embedder_policy(JSContext *ctx){struct worker*w=state(ctx);return w&&w->policy_initialized?(int)w->policy_doc.embedder_policy:HTML_COEP_UNSAFE_NONE;}
static const struct web_idb_realm worker_idb_realm={worker_idb_state,worker_idb_authority,worker_idb_origin,worker_embedder_policy};
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
static void worker_csp_report(void *opaque,const struct html_csp_violation *violation);
static JSValue native_safety(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"Worker safety operation required");
    const char *op=JS_ToCString(ctx,argv[0]);if(!op)return JS_EXCEPTION;
    bool policy=!strcmp(op,"state"),violation=!strcmp(op,"violation"),timer=!strcmp(op,"cspTimer");JS_FreeCString(ctx,op);
    if(policy)return worker_policy_state(ctx,w);
    if(timer){
        /* HTML timer initialization: a string handler CSP blocks (§4.4.1,
           "timer") starts no timer (null); otherwise its compiled function. */
        size_t length=0;const char *text=argc>2?JS_ToCStringLen(ctx,&length,argv[2]):NULL;
        if(!text)return JS_EXCEPTION;
        JSValue result=JS_NULL;
        if(html_policy_eval_allowed(&w->policy_doc,text,length,worker_csp_report,w)){
            size_t n=length+16;char *source=length<SIZE_MAX-16?malloc(n):NULL;
            if(!source)result=JS_ThrowOutOfMemory(ctx);
            else{
                memcpy(source,"(function(){\n",13);memcpy(source+13,text,length);memcpy(source+13+length,"\n})",3);
                result=JS_Eval(ctx,source,13+length+3,w->script_url?w->script_url:"",JS_EVAL_TYPE_GLOBAL);free(source);
            }
        }
        JS_FreeCString(ctx,text);return result;
    }
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
    v->rule=rule->from_meta?NULL:rule;
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
        else{
            if(JS_ToBool(ctx,inherit)){
                JSValue policy=JS_GetPropertyUint32(ctx,start,NJW_START_CREATOR_REFERRER_POLICY);int32_t value=0;
                bool valid=JS_IsUndefined(policy)||(JS_IsNumber(policy)&&JS_ToInt32(ctx,&value,policy)>=0&&value>=0&&value<=WEBNET_REFERRER_UNSAFE);JS_FreeValue(ctx,policy);
                if(!valid){free(copy);free(security);w->policy_doc.url=NULL;html_policy_free(&w->policy_doc);JS_ThrowTypeError(ctx,"Invalid Worker referrer policy");goto done;}
                w->policy_doc.referrer_policy=value;
            }else w->policy_doc.referrer_policy=webnet_referrer_policy_header(text,WEBNET_REFERRER_DEFAULT);
            /* A dedicated Worker joins its creator's isolated agent cluster;
               a fetched script must carry its own embedder policy too. */
            JSValue isolation=JS_GetPropertyUint32(ctx,start,NJW_START_CROSS_ORIGIN_ISOLATED);
            bool isolated=JS_ToBool(ctx,isolation)>0&&(JS_ToBool(ctx,inherit)||html_policy_embedder(text)!=HTML_COEP_UNSAFE_NONE);
            JS_FreeValue(ctx,isolation);w->policy_doc.cross_origin_isolated=isolated;
            if(web_atomics_isolation(ctx,isolated)<0){free(copy);free(security);w->policy_doc.url=NULL;html_policy_free(&w->policy_doc);goto done;}
            w->security_url=security;w->policy_initialized=ok=true;
        }
    }
    JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);JS_FreeCString(ctx,creator);
done:JS_FreeValue(ctx,inherit);JS_FreeValue(ctx,headers);JS_FreeValue(ctx,url);JS_FreeValue(ctx,status);JS_FreeValue(ctx,origin);return ok;
}
/* CSP3 §5.5 for the WorkerGlobalScope: the record is made now (the running
   script's location), the event and the reports are policy_task's. */
static void worker_csp_report(void *opaque,const struct html_csp_violation *violation){
    struct worker *w=opaque;JSContext *ctx=w->ctx;
    struct policy_violation *v=js_mallocz(ctx,sizeof *v);
    if(v)v->init=csp_violation_init(ctx,&w->policy_doc,violation,w->policy_status);
    if(!v||JS_IsException(v->init)){js_free(ctx,v);if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));return;}
    v->rule=violation->policy&&!violation->policy->from_meta?violation->policy:NULL;
    if(w->last_violation)w->last_violation->next=v;else w->violations=v;w->last_violation=v;
}
/* CSP3 §4.1.1 and §4.1.2 for a request the WorkerGlobalScope makes: the
   worker's own CSP list decides (its creator's only fetched the worker's
   script). url is the request's current URL, original its URL; redirects 1
   for the URL a redirect reached, whose path is then not matched. */
static bool worker_csp_allowed(struct worker *w,const char *url,const char *original,int destination,unsigned redirects){
    if(!w->policy_initialized)return true;
    struct html_csp_request r={.url=url,.original_url=original,.destination=destination,.redirect_count=redirects};
    return html_policy_request_allowed(&w->policy_doc,&r,worker_csp_report,w);
}
/* Whether two URL serializations differ apart from their fragments. */
static bool worker_url_moved(const char *a,const char *b){
    size_t an=strcspn(a,"#"),bn=strcspn(b,"#");
    return an!=bn || strncmp(a,b,an);
}
static JSValue worker_code_check(JSContext *ctx,JSValueConst value,int kind,int argc,JSValueConst *argv,void *opaque){
    struct worker *w=opaque;
    if(!html_policy_requires_script(&w->policy_doc) && (kind==JS_DYNAMIC_FUNCTION || !JS_IsObject(value)))
        return kind==JS_DYNAMIC_FUNCTION_ARGUMENT?JS_DupValue(ctx,value):
            csp_compile_check(ctx,&w->policy_doc,JS_DupValue(ctx,value),kind==JS_DYNAMIC_FUNCTION,worker_csp_report,w);
    JSValue args_array=JS_NewArray(ctx);if(JS_IsException(args_array))return args_array;
    for(int i=0;i<argc;i++)if(JS_DefinePropertyValueUint32(ctx,args_array,(uint32_t)i,JS_DupValue(ctx,argv[i]),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,args_array);return JS_EXCEPTION;}
    JSValue args[]={JS_DupValue(ctx,value),JS_NewInt32(ctx,kind),args_array};
    /* A nested dynamic-code check must not cancel the outer task's leases. */
    JSValue fn=JS_GetPropertyStr(ctx,w->hooks,"safetyDynamicCode"),result=JS_EXCEPTION;
    if(!JS_IsException(fn))result=JS_Call(ctx,fn,JS_UNDEFINED,3,args);
    JS_FreeValue(ctx,fn);for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,args[i]);
    /* §4.4.1 after Trusted Types: a Function's parts are checked assembled. */
    return kind==JS_DYNAMIC_FUNCTION_ARGUMENT?result:csp_compile_check(ctx,&w->policy_doc,result,kind==JS_DYNAMIC_FUNCTION,worker_csp_report,w);
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
    /* Shared regions of the message travel as attachments ahead of it. */
    struct web_attachments attached={0};if(op==NJW_MESSAGE&&!web_attachments_extract(ctx,argv[3],&attached)){output_ports_free(w,ports);return JS_EXCEPTION;}
    size_t length=0;uint8_t*data=JS_WriteObject(ctx,&length,argv[3],JS_WRITE_OBJ_REFERENCE);if(!data){web_attachments_release(&attached);output_ports_free(w,ports);return JS_EXCEPTION;}
    if(length>NJW_MAX_BYTES||length>SIZE_MAX-sizeof(struct njw_header)||length+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(ctx,data);web_attachments_release(&attached);output_ports_free(w,ports);return JS_ThrowRangeError(ctx,"Worker broker output accounting overflow");}
    struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(ctx,data);web_attachments_release(&attached);output_ports_free(w,ports);return JS_ThrowOutOfMemory(ctx);}p->data=data;p->attach=attached;p->client=w->shared?pair->client:NULL;p->h=(struct njw_header){NJW_MAGIC,op,p->client?p->client->generation:w->generation,w->shared?pair->client_id:id,op==NJW_MESSAGE?NJW_WITH_PORTS:creator,(uint32_t)length};
    if(!output_wait(w,p->client,sizeof p->h+length)){js_free(ctx,data);web_attachments_release(&p->attach);free(p);output_ports_free(w,ports);return JS_ThrowTypeError(ctx,"Worker transport ended or its task deadline expired before publication");}
    struct output_publication t={w,p,ports,op==NJW_PORT_CLOSE?pair:NULL,NULL};JSValue r=worker_transfer_commit_publish(ctx,argv[4],!w->closed,output_publish,&t);
    if(JS_IsException(r)){js_free(ctx,data);web_attachments_release(&p->attach);free(p);output_ports_free(w,ports);}else{
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
static JSValue native_detach(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(argc&&!JS_IsArrayBufferTransferable(ctx,argv[0]))return JS_ThrowTypeError(ctx,"ArrayBuffer cannot be transferred");if(argc)JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;}
static JSValue native_buffer_transferable(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;return JS_NewBool(ctx,argc&&JS_IsArrayBufferTransferable(ctx,argv[0]));}
/* The private structured-clone graph IndexedDB stores (as js.c's pack and unpack). */
static JSValue native_pack(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;size_t size=0;uint8_t *data=JS_WriteObject(ctx,&size,argc?argv[0]:JS_UNDEFINED,JS_WRITE_OBJ_REFERENCE);if(!data)return JS_EXCEPTION;JSValue out=JS_NewArrayBufferCopy(ctx,data,size);js_free(ctx,data);return out;}
static JSValue native_unpack(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;size_t size=0;uint8_t *data=argc?JS_GetArrayBuffer(ctx,&size,argv[0]):NULL;if(!data)return JS_EXCEPTION;return JS_ReadObject(ctx,data,size,JS_READ_OBJ_REFERENCE);}
static JSValue native_now(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;return JS_NewFloat64(ctx,(double)n_clock_uptime_ms());}
/* Web IDL promise reactions: no page Promise constructor or @@species lookup. */
static JSValue native_promise_then(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;if(!argc)return JS_ThrowTypeError(ctx,"promiseThen requires a promise");
    return JS_PromiseThenIntrinsic(ctx,argv[0],argc>1?argv[1]:JS_UNDEFINED,argc>2?argv[2]:JS_UNDEFINED);
}
static JSValue native_promise_is_handled(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;int handled=argc?JS_PromiseIsHandled(ctx,argv[0]):-1;
    if(handled<0)return JS_ThrowTypeError(ctx,"promiseIsHandled requires a promise");
    return JS_NewBool(ctx,handled);
}
static JSValue native_close(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;struct worker *w=state(ctx);send_value(w,NJW_CLOSE,0,0,JS_NULL);w->closed=true;if(w->shared){n_handle_close(CHANNEL);w->input_watched=0;}return JS_UNDEFINED;}
static JSValue native_eval(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(argc<2)return JS_ThrowTypeError(ctx,"Worker source/URL required");size_t n;const char *source=JS_ToCStringLen(ctx,&n,argv[0]),*url=JS_ToCString(ctx,argv[1]);if(!source||!url){JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);return JS_EXCEPTION;}
    uint64_t saved=w->until;w->until=n_clock_uptime_ms()+NJW_TASK_MS;JSValue code=JS_Eval(ctx,source,n,url,JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);JS_FreeCString(ctx,source);JS_FreeCString(ctx,url);w->until=saved;
    if(JS_IsException(code))return code;return JS_EvalFunction(ctx,code);
}
/* request(query, stream): a fetch, from js_fetch.js's transport query (the runtime's own record,
 * without a prototype). Its modes become the NJW_LOAD kind's flags, the rest the packet that
 * js_worker.c reads: [url, method, headers, body, referrer, referrer policy, destination]. */
static JSValue native_request(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);
    if(w->closed)return JS_ThrowTypeError(ctx,"Worker closed");
    if(!argc||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"Fetch request record required");
    if(w->request==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker request identifier space exhausted");
    static const struct {const char *name;uint32_t flag;} modes[]={{"noCors",NJW_LOAD_NO_CORS},{"noReferrer",NJW_LOAD_NO_REFERRER},
        {"sameOrigin",NJW_LOAD_SAME_ORIGIN},{"redirectError",NJW_LOAD_REDIRECT_ERROR},{"forcePreflight",NJW_LOAD_PREFLIGHT},{"keepalive",NJW_LOAD_KEEPALIVE}};
    uint32_t kind=2u;
    for(unsigned i=0;i<sizeof modes/sizeof *modes;i++){
        JSValue value=JS_GetPropertyStr(ctx,argv[0],modes[i].name);if(JS_IsException(value))return value;
        int on=JS_ToBool(ctx,value);JS_FreeValue(ctx,value);if(on<0)return JS_EXCEPTION;if(on)kind|=modes[i].flag;
    }
    if(argc>1){int on=JS_ToBool(ctx,argv[1]);if(on<0)return JS_EXCEPTION;if(on)kind|=NJW_LOAD_STREAM;}
    /* Fetch's main fetch step 7 against the worker's CSP list: a blocked
       request is a network error (the fetch rejects with a TypeError). */
    {
        JSValue address=JS_GetPropertyStr(ctx,argv[0],"url"),target=JS_GetPropertyStr(ctx,argv[0],"destination");
        const char *url=JS_IsString(address)?JS_ToCString(ctx,address):NULL,*name=JS_IsString(target)?JS_ToCString(ctx,target):NULL;
        JS_FreeValue(ctx,address);JS_FreeValue(ctx,target);
        int destination=WEBNET_DEST_EMPTY;
        for(int i=0;name&&i<WEBNET_DEST_COUNT;i++)if(!strcmp(webnet_destination_name((enum webnet_destination)i),name)){destination=i;break;}
        bool allowed=!url||worker_csp_allowed(w,url,url,destination,0);
        JS_FreeCString(ctx,url);JS_FreeCString(ctx,name);
        if(JS_HasException(ctx))return JS_EXCEPTION;
        if(!allowed)return JS_ThrowTypeError(ctx,"Blocked by Content Security Policy");
    }
    static const struct {const char *name;int32_t max;unsigned shift;} numbers[]={{"credentials",2,NJW_LOAD_CREDENTIALS_SHIFT},{"cache",5,NJW_LOAD_CACHE_SHIFT}};
    for(unsigned i=0;i<2;i++){
        JSValue value=JS_GetPropertyStr(ctx,argv[0],numbers[i].name);if(JS_IsException(value))return value;
        int32_t n=-1;bool valid=JS_IsNumber(value)&&!JS_ToInt32(ctx,&n,value)&&n>=0&&n<=numbers[i].max;JS_FreeValue(ctx,value);
        if(!valid)return JS_ThrowTypeError(ctx,"Invalid Worker fetch mode");kind|=(uint32_t)n<<numbers[i].shift;
    }
    static const char *const fields[]={"url","method","headers","body","referrer","referrerPolicy","destination"};
    JSValue packet=JS_NewArray(ctx);if(JS_IsException(packet))return packet;
    bool ok=true;
    for(unsigned i=0;i<7&&ok;i++){
        JSValue value=JS_GetPropertyStr(ctx,argv[0],fields[i]);
        if(JS_IsException(value)){ok=false;break;}
        size_t bytes=0;
        bool typed=i==3?JS_IsNull(value)||JS_GetArrayBuffer(ctx,&bytes,value)!=NULL||(!JS_HasException(ctx)&&JS_IsObject(value)&&!bytes):i==5?JS_IsNumber(value):JS_IsString(value);
        if(!typed){JS_FreeValue(ctx,value);if(!JS_HasException(ctx))JS_ThrowTypeError(ctx,"Invalid Worker fetch request field");ok=false;break;}
        if(JS_DefinePropertyValueUint32(ctx,packet,i,value,JS_PROP_C_W_E)<0)ok=false;
    }
    uint32_t id=w->request+1;
    if(ok)ok=send_value(w,NJW_LOAD,id,kind,packet);JS_FreeValue(ctx,packet);
    if(!ok)return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowTypeError(ctx,"Worker fetch IPC failed");
    w->request=id;output_flush(w);
    return JS_NewUint32(ctx,id);
}
/* resume(id): the runtime took the last frame of a streamed fetch (NJW_RESUME). */
static JSValue native_resume(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);uint32_t id;
    if(!argc)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(JS_ToUint32(ctx,&id,argv[0]))return JS_EXCEPTION;
    if(!id)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(w->closed)return JS_UNDEFINED;
    if(!send_value(w,NJW_RESUME,id,0,JS_NULL)){w->closed=w->output_failed=true;return JS_ThrowTypeError(ctx,"Worker stream IPC failed");}
    output_flush(w);
    return JS_UNDEFINED;
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
static JSValue native_referrer_policy(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;(void)argc;(void)argv;return JS_NewInt32(ctx,state(ctx)->policy_doc.referrer_policy);}
/* The NJW_LOADED that answers request id, while this worker's event loop pauses: every other
 * record waits in its task queue (transport credit is applied). importScripts waits so, and a
 * synchronous XMLHttpRequest's send() (https://xhr.spec.whatwg.org/#the-send()-method, "pause").
 * At deadline (an uptime; UINT64_MAX for none) the parent is told to cancel and *timed_out is set.
 * NULL with an exception when the worker closed or its channel failed. The paused time does not
 * count against the running task's deadline. */
static struct deferred *loaded_wait(JSContext *ctx,struct worker *w,uint32_t id,uint64_t deadline,bool *timed_out){
    uint64_t wait_start=n_clock_uptime_ms();*timed_out=false;
    while(!w->closed){uint64_t now=n_clock_uptime_ms();if(now>=deadline){send_value(w,NJW_CANCEL,id,0,JS_NULL);*timed_out=true;return NULL;}uint64_t left=deadline-now;int timeout=left>(uint64_t)INT_MAX?INT_MAX:(int)left;struct deferred*d=NULL;
        if(w->shared){
            struct deferred**at=&w->head;while(*at&&((*at)->h.op!=NJW_LOADED||(*at)->h.request!=id))at=&(*at)->next;
            if(*at){d=*at;*at=d->next;w->queued-=d->h.bytes;w->tail=NULL;for(struct deferred*p=w->head;p;p=p->next)w->tail=p;d->next=NULL;}
            else d=shared_receive(w,timeout);
            if(!d)continue;if(!shared_incoming(w,d)){deferred_free(d);continue;}
        }else{if(!dedicated_wait(w,timeout)){JS_ThrowTypeError(ctx,"Worker wait failed");return NULL;}if(w->output)output_flush(w);if(w->closed)break;bool failed=false;d=receive(w,&failed);if(failed){w->closed=true;JS_ThrowTypeError(ctx,"Worker channel closed");return NULL;}if(!d)continue;}
        if(d->h.op==NJW_CREDIT){transport_credit(w,d);deferred_free(d);continue;}
        if(d->h.op==NJW_LOADED&&d->h.request==id){if(w->until!=UINT64_MAX)w->until+=n_clock_uptime_ms()-wait_start;return d;}
        if(d->h.bytes>SIZE_MAX-w->queued){deferred_free(d);JS_ThrowRangeError(ctx,"Worker deferred message accounting overflow");return NULL;}w->queued+=d->h.bytes;if(w->tail)w->tail->next=d;else w->head=d;w->tail=d;
    }JS_ThrowTypeError(ctx,"Worker closed");return NULL;
}
static JSValue import_load(JSContext *ctx,JSValueConst url){
    struct worker *w=state(ctx);if(w->request==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker request identifier space exhausted");uint32_t id=++w->request;if(!send_value(w,NJW_LOAD,id,1,url))return JS_ThrowTypeError(ctx,"Worker import IPC failed");
    bool timed_out;struct deferred*d=loaded_wait(ctx,w,id,n_clock_uptime_ms()+WEBNET_TIMEOUT_MS,&timed_out);
    if(!d)return timed_out?JS_ThrowTypeError(ctx,"Worker import deadline exceeded"):JS_EXCEPTION;
    JSValue result=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);deferred_free(d);return result;
}
/* requestWait(id, timeout): the answer to request(query, false)'s fetch id, for a synchronous
 * XMLHttpRequest: [NJW_LOADED's record, its NJW_LOADED_* flags], or null when timeout ms (0: none)
 * passed first and the parent was told to cancel. */
static JSValue native_request_wait(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);uint32_t id;double timeout=0;
    if(argc<2||JS_ToUint32(ctx,&id,argv[0])<0||JS_ToFloat64(ctx,&timeout,argv[1])<0)return JS_EXCEPTION;
    if(!id||id>w->request)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(!(timeout>=0))return JS_ThrowRangeError(ctx,"Invalid synchronous request timeout");
    uint64_t now=n_clock_uptime_ms(),deadline=timeout<1?UINT64_MAX:timeout>=(double)(UINT64_MAX-now)?UINT64_MAX:now+(uint64_t)timeout;
    bool timed_out;struct deferred*d=loaded_wait(ctx,w,id,deadline,&timed_out);
    if(!d)return timed_out?JS_NULL:JS_EXCEPTION;
    JSValue record=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE),out=JS_IsException(record)?JS_EXCEPTION:JS_NewArray(ctx);uint32_t flags=d->h.kind;deferred_free(d);
    if(JS_IsException(out)){JS_FreeValue(ctx,record);return out;}
    if(JS_DefinePropertyValueUint32(ctx,out,0,record,JS_PROP_C_W_E)<0||JS_DefinePropertyValueUint32(ctx,out,1,JS_NewUint32(ctx,flags),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,out);return JS_EXCEPTION;}
    return out;
}
/* What importScripts reads for a network error: no status, the URL, the reason. */
static JSValue import_blocked(JSContext *ctx,const char *url){
    JSValue r=JS_NewArray(ctx);if(JS_IsException(r))return r;
    if(JS_DefinePropertyValueUint32(ctx,r,0,JS_NewInt32(ctx,0),JS_PROP_C_W_E)<0||
       JS_DefinePropertyValueUint32(ctx,r,1,JS_NewString(ctx,url),JS_PROP_C_W_E)<0||
       JS_DefinePropertyValueUint32(ctx,r,4,JS_NewString(ctx,"Blocked by Content Security Policy"),JS_PROP_C_W_E)<0){
        JS_FreeValue(ctx,r);return JS_EXCEPTION;
    }
    return r;
}
/* importScripts' fetch (destination "script"): the worker's CSP list before
   the request and, for the URL a redirect reached, after it. A blocked fetch
   is a network error, which importScripts throws as a NetworkError. */
static JSValue native_import(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"URL required");
    const char *url=JS_ToCString(ctx,argv[0]);if(!url)return JS_EXCEPTION;
    JSValue result=worker_csp_allowed(w,url,url,WEBNET_DEST_SCRIPT,0)?import_load(ctx,argv[0]):import_blocked(ctx,url);
    if(JS_IsObject(result)){
        JSValue final=JS_GetPropertyUint32(ctx,result,1);
        const char *reached=JS_IsString(final)?JS_ToCString(ctx,final):NULL;JS_FreeValue(ctx,final);
        if(!reached&&JS_HasException(ctx)){JS_FreeValue(ctx,result);result=JS_EXCEPTION;}
        else if(reached&&*reached&&worker_url_moved(url,reached)&&!worker_csp_allowed(w,reached,url,WEBNET_DEST_SCRIPT,1)){
            JS_FreeValue(ctx,result);result=import_blocked(ctx,url);
        }
        JS_FreeCString(ctx,reached);
    }
    JS_FreeCString(ctx,url);return result;
}
static int interrupt(JSRuntime *rt,void *opaque){
    (void)rt;struct worker*w=opaque;if(w->closed)return 1;
    /* QuickJS already amortizes bytecode checks. Further amortize the native
     * clock syscall, not cancellation: process kill and closed remain live.
     * Import waits are charged separately and preserve the same deadline. */
    if(++w->interrupt_ticks&15u)return 0;
    /* The storage broker's transport moves on during a long author task (native I/O only). */
    web_idb_pump(&w->indexed_db);
    if(w->shared&&shared_without_owners(w)){w->closed=true;return 1;}
    if(w->until==UINT64_MAX)return 0;
    return n_clock_uptime_ms()>=w->until;
}
static JSValue hook(struct worker *w,const char *name,int argc,JSValueConst *argv){JSValue fn=JS_GetPropertyStr(w->ctx,w->hooks,name);JSValue r=JS_Call(w->ctx,fn,JS_UNDEFINED,argc,argv);JS_FreeValue(w->ctx,fn);port_capture_cancel(w);return r;}
static int wasm_poll(JSContext *ctx){struct worker*w=JS_GetContextOpaque(ctx);return !w||interrupt(JS_GetRuntime(ctx),w);}
static JSValue native_data_url(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;return web_js_data_url(ctx,argc?argv[0]:JS_UNDEFINED,wasm_poll,false);}
static int wasm_can_compile(JSContext *ctx){
    struct worker *w=state(ctx);if(!w||w->closed)return 0;
    bool allowed=true;
    for(struct web_html_policy_rule *rule=w->policy_doc.html_policy?w->policy_doc.html_policy->first:NULL;rule;rule=rule->next){
        if(html_policy_wasm_allowed(rule))continue;
        if(!rule->report_only)allowed=false;
        struct policy_violation *v=js_mallocz(ctx,sizeof *v);if(!v)return -1;
        v->init=wasm_policy_violation_init(ctx,&w->policy_doc,rule,w->policy_status);
        if(JS_IsException(v->init)){js_free(ctx,v);return -1;}
        v->rule=rule->from_meta?NULL:rule;
        if(w->last_violation)w->last_violation->next=v;else w->violations=v;w->last_violation=v;
    }
    return allowed;
}
static void exception(struct worker *w){JSValue ex=JS_GetException(w->ctx);w->until=n_clock_uptime_ms()+NJW_TASK_MS;JSValue r=hook(w,"report",1,&ex);if(JS_IsException(r)){JSValue ignored=JS_GetException(w->ctx);JS_FreeValue(w->ctx,ignored);}JS_FreeValue(w->ctx,r);JS_FreeValue(w->ctx,ex);}
/* HTML "notify about rejected promises": rejections still unhandled after a
   microtask checkpoint fire unhandledrejection; a later handler on a notified
   promise fires rejectionhandled. The tracker cannot run script itself. */
struct rejection{struct rejection*next;JSValue promise,reason;bool handled;};
static struct rejection *rejections,**rejection_tail=&rejections;
static bool rejection_oom;
static void rejection_free(JSContext *ctx,struct rejection *r){JS_FreeValue(ctx,r->promise);JS_FreeValue(ctx,r->reason);free(r);}
static void rejection_track(JSContext *ctx,JSValueConst promise,JSValueConst reason,JS_BOOL handled,void *opaque){
    (void)opaque;
    for(struct rejection **link=&rejections;*link;link=&(*link)->next){
        struct rejection *r=*link;
        if(r->handled||!JS_StrictEq(ctx,r->promise,promise))continue;
        /* Handled before its notification: it was never reported. */
        if(handled){*link=r->next;if(rejection_tail==&r->next)rejection_tail=link;rejection_free(ctx,r);}
        return;
    }
    struct rejection *r=malloc(sizeof *r);
    if(!r){rejection_oom=true;return;}
    r->promise=JS_DupValue(ctx,promise);r->reason=JS_DupValue(ctx,reason);r->handled=handled;r->next=NULL;
    *rejection_tail=r;rejection_tail=&r->next;
}
static void drain(struct worker *w){for(unsigned i=0;i<256&&!w->closed&&JS_IsJobPending(w->rt);i++){JSContext *ctx=NULL;int n=JS_ExecutePendingJob(w->rt,&ctx);port_capture_cancel(w);if(n<0){exception(w);break;}if(!n)break;}}
static void jobs(struct worker *w){
    drain(w);
    /* HTML "perform a microtask checkpoint" empties the queue before it notifies about rejected
       promises: a handler a later microtask attaches still counts. A drain that stopped at its
       bound, so output can flush, leaves the rest to the main loop, which comes back here while
       a job is pending. */
    if(!w->closed&&JS_IsJobPending(w->rt))return;
    if(rejection_oom&&!w->closed){rejection_oom=false;JSValue error=JS_NewString(w->ctx,"Unhandled rejection tracking allocation failed");JSValue r=hook(w,"report",1,&error);JS_FreeValue(w->ctx,error);if(JS_IsException(r))JS_FreeValue(w->ctx,JS_GetException(w->ctx));JS_FreeValue(w->ctx,r);}
    /* Unpublish the batch before author listeners run; their rejections queue anew. */
    struct rejection *batch=rejections;rejections=NULL;rejection_tail=&rejections;
    while(batch){
        struct rejection *r=batch;batch=r->next;
        if(!w->closed&&JS_IsObject(w->hooks)){JSValue args[2]={r->promise,r->reason};JSValue out=hook(w,r->handled?"promiseRejectionHandled":"promiseRejection",2,args);if(JS_IsException(out))exception(w);JS_FreeValue(w->ctx,out);drain(w);}
        rejection_free(w->ctx,r);
    }
    web_js_wasm_collect(w->ctx);
    /* IndexedDB's active period is the task and its microtasks, never the next task. */
    if(!w->closed&&!JS_IsJobPending(w->rt)&&web_idb_used(&w->indexed_db)&&JS_IsObject(w->hooks)){
        JSValue r=hook(w,"indexedDBTaskEnd",0,NULL);if(JS_IsException(r))exception(w);JS_FreeValue(w->ctx,r);
    }
}
static void policy_task(struct worker *w){
    struct policy_violation *v=w->violations;w->violations=v->next;if(!w->violations)w->last_violation=NULL;
    struct web_html_policy_rule *rule=v->rule;
    if(rule&&!rule->from_meta&&(rule->report_uri_count||rule->report_to)){
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
    if(!JS_IsException(log)){JS_DefinePropertyValueStr(w->ctx,log,"message",JS_NewString(w->ctx,"Content Security Policy violation in Worker"),JS_PROP_C_W_E);send_value(w,NJW_CONSOLE,0,0,log);}
    else{JSValue ignored=JS_GetException(w->ctx);JS_FreeValue(w->ctx,ignored);}JS_FreeValue(w->ctx,log);
    JSValue r=hook(w,"safetyViolation",1,&v->init);if(JS_IsException(r))exception(w);JS_FreeValue(w->ctx,r);JS_FreeValue(w->ctx,v->init);js_free(w->ctx,v);
}
int main(int argc,char**argv){
    struct worker w={0};w.probe=-1;w.queue=web_atomics_queue();if(w.queue<0)return 1;w.shared=argc>1&&!strcmp(argv[1],"--shared-server");w.script_url=argc>2?argv[2]:"";w.hooks=JS_UNDEFINED;w.rt=web_js_new_runtime(NULL);if(!w.rt)return 1;JS_SetMemoryLimit(w.rt,NJW_HEAP_BYTES);JS_SetMaxStackSize(w.rt,512u*1024u);JS_SetCanBlock(w.rt,true);web_atomics_runtime_init(w.rt);JS_SetInterruptHandler(w.rt,interrupt,&w);JS_SetHostPromiseRejectionTracker(w.rt,rejection_track,NULL);w.ctx=JS_NewContext(w.rt);if(!w.ctx){JS_FreeRuntime(w.rt);return 1;}JS_SetContextOpaque(w.ctx,&w);w.until=n_clock_uptime_ms()+10000;
    /* Watch the browser's preferences in this queue before the realm first reads the languages
       (languagechange); without a watch the first read takes them once. */
    (void)webprefs_watch(w.queue);w.languages_seen=webprefs_languages_generation();
    if(trusted_native_init(w.ctx)<0)goto shutdown;
    if(web_js_wasm_init(w.ctx,wasm_poll,wasm_can_compile)<0)goto shutdown;
    JSValue global=JS_GetGlobalObject(w.ctx),host=JS_NewObject(w.ctx);
    const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),
        JS_CFUNC_DEF("safetyRequired",1,native_safety_required),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("classID",1,native_class),
        JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("portSend",6,native_port_send),
        JS_CFUNC_DEF("bufferTransferable",1,native_buffer_transferable),JS_CFUNC_DEF("pack",1,native_pack),JS_CFUNC_DEF("unpack",1,native_unpack),
        JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("promiseThen",3,native_promise_then),JS_CFUNC_DEF("promiseIsHandled",1,native_promise_is_handled),JS_CFUNC_DEF("url",0,native_context_url),JS_CFUNC_DEF("securityURL",0,native_security_url),JS_CFUNC_DEF("referrerPolicy",0,native_referrer_policy),
        JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("request",2,native_request),JS_CFUNC_DEF("requestWait",2,native_request_wait),JS_CFUNC_DEF("resume",1,native_resume),JS_CFUNC_DEF("dataURL",1,native_data_url),JS_CFUNC_DEF("cancel",1,native_cancel),
        JS_CFUNC_DEF("import",1,native_import),JS_CFUNC_DEF("eval",2,native_eval)};
    JS_SetPropertyFunctionList(w.ctx,host,funcs,sizeof funcs/sizeof funcs[0]);
    web_js_encoding_init_isolated(w.ctx,host);web_js_url_init(w.ctx,host);web_js_navigator_init(w.ctx,host);web_js_webidl_init(w.ctx,host);web_js_crypto_init(w.ctx,host);web_js_compression_init(w.ctx,host);web_atomics_host_init(w.ctx,host);web_opfs_host_init(w.ctx,host,opfs_storage_url);web_idb_host_init(w.ctx,host,&worker_idb_realm);web_cache_host_init(w.ctx,host,&worker_idb_realm);web_js_wasm_host_init(w.ctx,host);web_js_webgl_init(w.ctx,host,NULL);
    JS_SetPropertyStr(w.ctx,global,"__workerHost",host);JS_FreeValue(w.ctx,global);
    /* Worker platform interfaces are built-ins too: no implementation source. */
    {JSRuntime *rt=JS_GetRuntime(w.ctx);int strip=JS_GetStripInfo(rt);JS_SetStripInfo(rt,strip|JS_STRIP_SOURCE);
    JS_SetPlatformScriptPrefix(rt,"nocturne:");
    w.hooks=JS_Eval(w.ctx,js_worker_runtime,sizeof js_worker_runtime-1,"nocturne:worker-bindings",JS_EVAL_TYPE_GLOBAL);JS_SetStripInfo(rt,strip);}
    if(JS_IsException(w.hooks))goto shutdown;
    JS_SetDynamicCodeCheck(w.ctx,worker_code_check,&w);
    while(!w.closed||w.output){
        if(w.output_failed)break;
        if(w.closed){if(w.shared){output_flush(&w);if(w.output){struct deferred*d=shared_receive(&w,10);if(d)deferred_free(d);}continue;}if(!dedicated_wait(&w,-1))break;output_flush(&w);continue;}
        if(w.output)output_flush(&w);
        if(w.output_failed)break;
        if(JS_IsJobPending(w.rt)){w.until=n_clock_uptime_ms()+NJW_TASK_MS;jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        /* CSP events are separate tasks, after the originating microtasks. */
        if(w.violations){w.until=n_clock_uptime_ms()+NJW_TASK_MS;policy_task(&w);jobs(&w);continue;}
        struct deferred *d=NULL;if(w.head){d=w.head;w.head=d->next;if(!w.head)w.tail=NULL;w.queued-=d->h.bytes;}
        else{
            w.until=UINT64_MAX;JSValue deadline=hook(&w,"deadline",0,NULL);int64_t due=-1;JS_ToInt64(w.ctx,&due,deadline);JS_FreeValue(w.ctx,deadline);uint64_t now=n_clock_uptime_ms();int64_t atomics_due=web_atomics_deadline(w.ctx,now);if(atomics_due>=0&&(due<0||atomics_due<due))due=atomics_due;int wait=JS_IsJobPending(w.rt)?0:due<0?-1:due<=(int64_t)now?0:due-(int64_t)now>INT_MAX?INT_MAX:(int)(due-(int64_t)now);
            /* The storage broker's answer ends this wait (its channel is watched in the queue);
               only a watch that could not be made has the loop look again soon. */
            if(web_idb_next(&w.indexed_db))wait=0;else if(web_idb_needs_poll(&w.indexed_db)&&(wait<0||wait>8))wait=8;
            if(w.shared){d=shared_receive(&w,wait);if(w.closed&&!d)continue;}
            else{if(!dedicated_wait(&w,wait))break;if(w.output)output_flush(&w);if(w.output_failed)break;bool failed=false;d=receive(&w,&failed);if(failed)break;}
        }
        w.until=n_clock_uptime_ms()+NJW_TASK_MS;
        if(d&&d->h.op==NJW_CREDIT){if(!w.shared||shared_incoming(&w,d))transport_credit(&w,d);deferred_free(d);d=NULL;if(w.closed)continue;}
        bool received_task=d!=NULL;
        /* The running JS task holds current_client until its checkpoint; the
         * packet's ownership pin is released exactly once by deferred_free. */
        if(d){w.current_client=d->client;bool allowed=!w.shared||shared_incoming(&w,d);JSValue value=allowed?JS_ReadObject(w.ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE):JS_UNDEFINED;if(JS_IsException(value))exception(&w);else if(allowed&&!native_port_receive(&w,&d->h,value)){JS_FreeValue(w.ctx,value);if(JS_HasException(w.ctx))exception(&w);}else if(allowed){JSValue result;if(d->h.op==NJW_LOADED||d->h.op==NJW_FRAME){JSValue args[]={JS_NewUint32(w.ctx,d->h.request),value,JS_NewUint32(w.ctx,d->h.kind)};result=hook(&w,d->h.op==NJW_FRAME?"frame":"loaded",3,args);JS_FreeValue(w.ctx,args[0]);JS_FreeValue(w.ctx,args[2]);}else if(d->h.op==NJW_START)result=worker_policy_start(&w,value)?hook(&w,"start",1,&value):JS_EXCEPTION;else{JSValue args[]={value,JS_NewUint32(w.ctx,d->h.op),JS_NewUint32(w.ctx,d->h.request),JS_NewUint32(w.ctx,d->h.kind)};web_atomics_import_begin(&d->inbox);result=hook(&w,"receive",4,args);web_atomics_import_end();for(unsigned i=1;i<4;i++)JS_FreeValue(w.ctx,args[i]);}if(JS_IsException(result)){exception(&w);if(d->h.op==NJW_START){send_value(&w,NJW_CLOSE,0,0,JS_NULL);w.closed=true;}}else if(w.shared&&d->h.op==NJW_START){if(w.before_start){if(w.tail)w.tail->next=w.before_start;else w.head=w.before_start;w.tail=w.before_start_tail;w.before_start=w.before_start_tail=NULL;}}JS_FreeValue(w.ctx,result);JS_FreeValue(w.ctx,value);}deferred_free(d);w.current_client=NULL;}
        /* Finish the received task's microtasks before a local port/timer task. */
        if(received_task){jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        /* Atomics.waitAsync settlements are tasks of their own. */
        {uint64_t at=n_clock_uptime_ms();int64_t due=web_atomics_deadline(w.ctx,at);if(due>=0&&due<=(int64_t)at){w.until=at+NJW_TASK_MS;web_atomics_run(w.ctx,at);jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}}
        /* HTML NavigatorLanguage: a new set of the user's languages is one task (this worker's
           event loop) that switches navigator.languages and fires languagechange at the global. */
        if(w.languages_seen!=webprefs_languages_generation()){w.languages_seen=webprefs_languages_generation();w.until=n_clock_uptime_ms()+NJW_TASK_MS;JSValue r=hook(&w,"languageChange",0,NULL);if(JS_IsException(r))exception(&w);JS_FreeValue(w.ctx,r);jobs(&w);continue;}
        /* An IndexedDB completion is a task of its own (its request's promise settles, then its
           microtasks run); it takes turns with the timer and port tasks below. */
        if(w.indexed_db_turn&&web_idb_next(&w.indexed_db)){w.indexed_db_turn=false;w.until=n_clock_uptime_ms()+NJW_TASK_MS;web_idb_complete(w.ctx,&w.indexed_db);if(JS_HasException(w.ctx))exception(&w);jobs(&w);continue;}
        w.indexed_db_turn=true;
        JSValue now=JS_NewFloat64(w.ctx,(double)n_clock_uptime_ms());JSValue tick=hook(&w,"tick",1,&now);if(JS_IsException(tick))exception(&w);JS_FreeValue(w.ctx,tick);JS_FreeValue(w.ctx,now);jobs(&w);
    }
shutdown:
    while(w.violations){struct policy_violation *v=w.violations;w.violations=v->next;JS_FreeValue(w.ctx,v->init);js_free(w.ctx,v);}
    html_policy_free(&w.policy_doc);free(w.policy_doc.url);free(w.security_url);
    while(w.output){struct outgoing*p=w.output;w.output=p->next;output_packet_free(&w,p);}
    web_js_wasm_cancel(w.ctx);
    while(w.head){struct deferred*d=w.head;w.head=d->next;deferred_free(d);}while(w.before_start){struct deferred*d=w.before_start;w.before_start=d->next;deferred_free(d);}output_ports_free(&w,w.ports);while(w.leases){struct broker_lease*l=w.leases;w.leases=l->next;free(l);}while(w.loads){struct pending_load*p=w.loads;w.loads=p->next;if(p->client)p->client->references--;free(p);}while(w.clients){struct client*c=w.clients;w.clients=c->next;if(c->active)n_handle_close(c->channel);web_inbox_close(&c->inbox);free(c);}web_inbox_close(&w.inbox);if(w.probe>=0)n_handle_close(w.probe);while(rejections){struct rejection*r=rejections;rejections=r->next;rejection_free(w.ctx,r);}JS_FreeValue(w.ctx,w.hooks);web_idb_release(w.ctx,&w.indexed_db);web_atomics_release(w.ctx);web_opfs_release(w.ctx);JS_FreeContext(w.ctx);JS_FreeRuntime(w.rt);return 0;
}
