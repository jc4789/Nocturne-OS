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
#include "../libc/web/js_worker_transfer.h"
#include "../libc/web/js_worker_port_lifetime.h"
#include "../libc/web/js_worker_runtime.inc"
#include "../libc/web/js_navigator_native.h"
#include "../libc/web/html_policy.h"
#include "../libc/web/js_html_safety_native.h"
struct deferred {struct deferred *next;struct njw_header h;uint8_t *data;};
struct outgoing {struct outgoing *next;struct njw_header h;size_t at;uint8_t *data;};
struct policy_violation {struct policy_violation *next;JSValue init;struct web_html_policy_rule *rule;};
struct worker {JSRuntime *rt;JSContext *ctx;JSValue hooks;uint32_t generation,request;uint64_t until;bool closed,established,output_failed,policy_initialized;struct deferred *head,*tail;struct outgoing *output,*last_output;size_t queued,output_bytes;uint32_t next_lease;struct broker_lease *leases;struct broker_pair *ports;web_doc policy_doc;struct policy_violation *violations,*last_violation;int policy_status;};
struct output_publication {struct worker *worker;struct outgoing *packet;struct broker_pair *ports,*close_pair,*ack_pair;};
static void output_link(void *opaque,void *packet){struct worker*w=opaque;struct outgoing*p=packet;if(w->last_output)w->last_output->next=p;else w->output=p;w->last_output=p;}
static void output_packet_free(struct worker*w,void *opaque){struct outgoing*p=opaque;if(p){js_free(w->ctx,p->data);free(p);}}
static void output_ports_free(struct worker*w,struct broker_pair*p){while(p){struct broker_pair*n=p->next;output_packet_free(w,p->held_close);output_packet_free(w,p->held_ack);free(p);p=n;}}
static void output_ports_retire(struct worker*w){struct broker_pair **at=&w->ports;while(*at){struct broker_pair*p=*at;if(worker_port_retirable(p)){*at=p->next;free(p);}else at=&p->next;}}
static void output_publish(void *opaque){
    struct output_publication*t=opaque;struct worker*w=t->worker;struct outgoing*p=t->packet;
    while(t->ports){struct broker_pair*pair=t->ports;t->ports=pair->next;pair->next=w->ports;w->ports=pair;}
    w->output_bytes+=sizeof p->h+p->h.bytes;
    if(t->close_pair){t->close_pair->closed=true;t->close_pair->held_close=p;worker_port_flush(t->close_pair,output_link,w);}
    else if(t->ack_pair){t->ack_pair->held_ack=p;worker_port_flush(t->ack_pair,output_link,w);}
    else output_link(w,p);
}
static void output_flush(struct worker*w){size_t budget=16384;while(w->output&&budget){struct outgoing*p=w->output;bool header=p->at<sizeof p->h;size_t left=header?sizeof p->h-p->at:sizeof p->h+p->h.bytes-p->at,take=left<budget?left:budget;const uint8_t*src=header?(uint8_t*)&p->h+p->at:p->data+p->at-sizeof p->h;ssize_t n=write(1,src,take);if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){w->closed=w->output_failed=true;break;}p->at+=n;budget-=n;if(p->at==sizeof p->h+p->h.bytes){w->output=p->next;if(!w->output)w->last_output=NULL;w->output_bytes-=sizeof p->h+p->h.bytes;js_free(w->ctx,p->data);free(p);}}}
static bool exact(int fd,void *p,size_t n,bool writing){size_t at=0;while(at<n){ssize_t got=writing?write(fd,(uint8_t *)p+at,n-at):read(fd,(uint8_t *)p+at,n-at);if(got<0&&errno==EINTR)continue;if(got<=0)return false;at+=(size_t)got;}return true;}
static bool send_value(struct worker *w,unsigned op,unsigned request,unsigned kind,JSValueConst value){size_t n=0;uint8_t *data=JS_WriteObject(w->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);if(!data)return false;if(n>NJW_MAX_BYTES||n>SIZE_MAX-sizeof(struct njw_header)||n+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(w->ctx,data);return false;}struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(w->ctx,data);return false;}p->data=data;p->h=(struct njw_header){NJW_MAGIC,op,w->generation,request,kind,(uint32_t)n};struct output_publication t={.worker=w,.packet=p};output_publish(&t);return true;}
static struct deferred *receive(struct worker *w){
    struct deferred *d=calloc(1,sizeof *d);if(!d)return NULL;
    if(!exact(0,&d->h,sizeof d->h,false)||d->h.magic!=NJW_MAGIC||!d->h.bytes||d->h.bytes>NJW_MAX_BYTES||
       ((d->h.op==NJW_PORT_MESSAGE||d->h.op==NJW_PORT_CLOSE||d->h.op==NJW_PORT_DRAINED)?d->h.kind>1:d->h.op==NJW_MESSAGE?(d->h.kind!=0&&d->h.kind!=NJW_WITH_PORTS):d->h.op==NJW_LOADED?!njw_loaded_kind_valid(d->h.kind):d->h.kind!=0)||
       (d->h.op!=NJW_START&&d->h.op!=NJW_MESSAGE&&d->h.op!=NJW_LOADED&&d->h.op!=NJW_PORT_MESSAGE&&d->h.op!=NJW_PORT_CLOSE&&d->h.op!=NJW_PORT_DRAINED)||(w->established&&d->h.generation!=w->generation)||(!w->established&&d->h.op!=NJW_START)){free(d);return NULL;}
    if(!w->established){w->generation=d->h.generation;w->established=true;}else if(d->h.op==NJW_START){free(d);return NULL;}
    d->data=malloc(d->h.bytes);if(!d->data||!exact(0,d->data,d->h.bytes,false)){free(d->data);free(d);return NULL;}return d;
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
    JSValue inherit=JS_GetPropertyUint32(ctx,start,NJW_START_INHERIT_POLICY),headers=JS_UNDEFINED,url=JS_UNDEFINED,status=JS_UNDEFINED;
    bool ok=false;if(!JS_IsBool(inherit)){JS_ThrowTypeError(ctx,"Missing native Worker policy metadata");goto done;}
    headers=JS_GetPropertyUint32(ctx,start,JS_ToBool(ctx,inherit)?NJW_START_CREATOR_POLICY:2);
    url=JS_GetPropertyUint32(ctx,start,1);status=JS_GetPropertyUint32(ctx,start,0);
    if(!JS_IsString(headers)||!JS_IsString(url)){JS_ThrowTypeError(ctx,"Invalid native Worker policy metadata");goto done;}
    if(JS_ToInt32(ctx,&w->policy_status,status)<0)goto done;
    const char *text=JS_ToCString(ctx,headers),*address=JS_ToCString(ctx,url);
    if(!text||!address){JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);goto done;}
    size_t n=strlen(address);char *copy=n==SIZE_MAX?NULL:malloc(n+1);if(copy)memcpy(copy,address,n+1);
    if(!copy)JS_ThrowOutOfMemory(ctx);
    else{
        w->policy_doc.url=copy;
        bool loaded=JS_ToBool(ctx,inherit)?html_policy_snapshot_init(&w->policy_doc,text):html_policy_init(&w->policy_doc,text,NULL);
        if(!loaded){free(copy);w->policy_doc.url=NULL;html_policy_free(&w->policy_doc);JS_ThrowOutOfMemory(ctx);}
        else w->policy_initialized=ok=true;
    }
    JS_FreeCString(ctx,text);JS_FreeCString(ctx,address);
done:JS_FreeValue(ctx,inherit);JS_FreeValue(ctx,headers);JS_FreeValue(ctx,url);JS_FreeValue(ctx,status);return ok;
}
static JSValue worker_code_check(JSContext *ctx,JSValueConst value,int kind,int argc,JSValueConst *argv,void *opaque){
    struct worker *w=opaque;JSValue args_array=JS_NewArray(ctx);if(JS_IsException(args_array))return args_array;
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
    if(action||w->closed||!p||p->closed)return JS_ThrowTypeError(ctx,"Worker capture target is inactive");
    uint32_t identifier=worker_port_next_lease(w->leases,w->next_lease);
    if(!identifier||p->pins==SIZE_MAX)return JS_ThrowRangeError(ctx,"Worker capture representation overflow");
    struct broker_lease*l=calloc(1,sizeof*l);if(!l)return JS_ThrowOutOfMemory(ctx);w->next_lease=l->id=identifier;l->port=id;l->creator=creator;l->next=w->leases;w->leases=l;p->pins++;return JS_NewUint32(ctx,l->id);
}
static JSValue native_port_send(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;struct worker*w=state(ctx);uint32_t op=0,id=0,creator=0,capture=0;
    if(argc<5||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&id,argv[1])<0||JS_ToUint32(ctx,&creator,argv[2])<0||(argc>5&&JS_ToUint32(ctx,&capture,argv[5])<0))return JS_EXCEPTION;
    if(w->closed||(op!=NJW_MESSAGE&&op!=NJW_PORT_MESSAGE&&op!=NJW_PORT_CLOSE)||(op!=NJW_MESSAGE&&(!id||creator>1)))return JS_ThrowTypeError(ctx,"Worker broker destination inactive or invalid");
    struct broker_pair*pair=NULL,*ports=NULL;
    if(op==NJW_MESSAGE){if(capture)return JS_ThrowTypeError(ctx,"Capture requires a Port message");if(!worker_port_metadata(ctx,argv[3],1,w->ports,UINT32_MAX,&ports))return JS_EXCEPTION;}
    else {pair=worker_port_find(w->ports,id,creator);struct broker_lease*l=w->leases;while(l&&l->id!=capture)l=l->next;bool pinned=capture&&l&&l->port==id&&l->creator==creator;
        if(!pair||(capture&&(!pinned||op!=NJW_PORT_MESSAGE))||(op==NJW_PORT_MESSAGE?(!pinned&&pair->closed):pair->closed))return JS_ThrowTypeError(ctx,"Worker port is closed, unknown or capture is invalid");}
    size_t length=0;uint8_t*data=JS_WriteObject(ctx,&length,argv[3],JS_WRITE_OBJ_REFERENCE);if(!data){output_ports_free(w,ports);return JS_EXCEPTION;}
    if(length>NJW_MAX_BYTES||length>SIZE_MAX-sizeof(struct njw_header)||length+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(ctx,data);output_ports_free(w,ports);return JS_ThrowRangeError(ctx,"Worker broker output accounting overflow");}
    struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(ctx,data);output_ports_free(w,ports);return JS_ThrowOutOfMemory(ctx);}p->data=data;p->h=(struct njw_header){NJW_MAGIC,op,w->generation,id,op==NJW_MESSAGE?NJW_WITH_PORTS:creator,(uint32_t)length};
    struct output_publication t={w,p,ports,op==NJW_PORT_CLOSE?pair:NULL,NULL};JSValue r=worker_transfer_commit_publish(ctx,argv[4],!w->closed,output_publish,&t);
    if(JS_IsException(r)){js_free(ctx,data);free(p);output_ports_free(w,ports);}return r;
}
static bool native_port_receive(struct worker*w,const struct njw_header*h,JSValueConst value){
    if(h->op==NJW_MESSAGE&&h->kind==NJW_WITH_PORTS){struct broker_pair*ports=NULL;if(!worker_port_metadata(w->ctx,value,0,w->ports,UINT32_MAX,&ports))return false;while(ports){struct broker_pair*p=ports;ports=p->next;p->next=w->ports;w->ports=p;}return true;}
    if(h->op!=NJW_PORT_MESSAGE&&h->op!=NJW_PORT_CLOSE&&h->op!=NJW_PORT_DRAINED)return true;
    struct broker_pair*pair=worker_port_find(w->ports,h->request,h->kind);if(!pair||(h->op==NJW_PORT_MESSAGE&&pair->sealed))return false;
    if(h->op!=NJW_PORT_MESSAGE){pair->closed=pair->sealed=true;
        if(h->op==NJW_PORT_CLOSE){size_t size=0;uint8_t*data=JS_WriteObject(w->ctx,&size,JS_NULL,JS_WRITE_OBJ_REFERENCE);if(!data){w->closed=true;return false;}
            if(size>SIZE_MAX-sizeof(struct njw_header)||size+sizeof(struct njw_header)>SIZE_MAX-w->output_bytes){js_free(w->ctx,data);w->closed=true;JS_ThrowRangeError(w->ctx,"Worker acknowledgement accounting overflow");return false;}
            struct outgoing*p=calloc(1,sizeof*p);if(!p){js_free(w->ctx,data);w->closed=true;JS_ThrowOutOfMemory(w->ctx);return false;}p->data=data;p->h=(struct njw_header){NJW_MAGIC,NJW_PORT_DRAINED,w->generation,h->request,h->kind,(uint32_t)size};struct output_publication t={w,p,NULL,NULL,pair};output_publish(&t);
        }output_ports_retire(w);
    }return true;
}
static JSValue native_transfer_commit(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(!argc)return JS_ThrowTypeError(ctx,"Worker transfer plan required");return worker_transfer_commit(ctx,argv[0],!state(ctx)->closed);}
static JSValue native_send(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;struct worker *w=state(ctx);uint32_t op=0,request=0;if(argc<3||JS_ToUint32(ctx,&op,argv[0])<0||JS_ToUint32(ctx,&request,argv[1])<0)return JS_EXCEPTION;if(op!=NJW_MESSAGE&&op!=NJW_ERROR&&op!=NJW_CONSOLE)return JS_ThrowTypeError(ctx,"Invalid worker send");if(!send_value(w,op,request,0,argv[2]))return JS_ThrowRangeError(ctx,"Worker send quota/transport failed");return JS_UNDEFINED;}
static JSValue native_class(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;return JS_NewUint32(ctx,argc?JS_GetClassID(argv[0]):0);}
static JSValue native_detach(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;if(argc)JS_DetachArrayBuffer(ctx,argv[0]);return JS_UNDEFINED;}
static JSValue native_now(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;return JS_NewFloat64(ctx,(double)uptime_ms());}
static JSValue native_close(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){(void)thisv;(void)argc;(void)argv;struct worker *w=state(ctx);send_value(w,NJW_CLOSE,0,0,JS_NULL);w->closed=true;return JS_UNDEFINED;}
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
    if(!send_value(w,NJW_LOAD,id,kind,argv[0]))return JS_ThrowTypeError(ctx,"Worker fetch IPC failed");
    return JS_NewUint32(ctx,id);
}
static JSValue native_cancel(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);uint32_t id;
    if(!argc)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(JS_ToUint32(ctx,&id,argv[0]))return JS_EXCEPTION;
    if(!id)return JS_ThrowTypeError(ctx,"Worker request identifier required");
    if(w->closed)return JS_UNDEFINED;
    if(!send_value(w,NJW_CANCEL,id,0,JS_NULL)){w->closed=w->output_failed=true;return JS_ThrowTypeError(ctx,"Worker cancellation IPC failed");}
    return JS_UNDEFINED;
}
static JSValue native_import(JSContext *ctx,JSValueConst thisv,int argc,JSValueConst *argv){
    (void)thisv;struct worker *w=state(ctx);if(!argc)return JS_ThrowTypeError(ctx,"URL required");if(w->request==UINT32_MAX)return JS_ThrowRangeError(ctx,"Worker request identifier space exhausted");uint32_t id=++w->request;if(!send_value(w,NJW_LOAD,id,1,argv[0]))return JS_ThrowTypeError(ctx,"Worker import IPC failed");
    uint64_t wait_start=uptime_ms(),deadline=wait_start+WEBNET_TIMEOUT_MS;
    while(!w->closed){uint64_t now=uptime_ms();if(now>=deadline)return JS_ThrowTypeError(ctx,"Worker import deadline exceeded");struct n_pollfd p[2]={{0,N_POLLIN,0},{1,w->output?N_POLLOUT:0,0}};uint64_t left=deadline-now;int ready=poll(p,2,left>(uint64_t)INT_MAX?INT_MAX:(int)left);if(ready<0&&errno==EINTR)continue;if(ready<0)return JS_ThrowTypeError(ctx,"Worker import poll failed");if(!ready)continue;if(p[1].revents&N_POLLOUT)output_flush(w);if(w->closed)break;if(!(p[0].revents&(N_POLLIN|N_POLLHUP)))continue;struct deferred *d=receive(w);if(!d){w->closed=true;return JS_ThrowTypeError(ctx,"Worker import pipe closed");}
        if(d->h.op==NJW_LOADED&&d->h.request==id){if(w->until!=UINT64_MAX)w->until+=uptime_ms()-wait_start;JSValue result=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);free(d->data);free(d);return result;}
        if(d->h.bytes>SIZE_MAX-w->queued){free(d->data);free(d);return JS_ThrowRangeError(ctx,"Worker deferred message accounting overflow");}w->queued+=d->h.bytes;if(w->tail)w->tail->next=d;else w->head=d;w->tail=d;
    }return JS_ThrowTypeError(ctx,"Worker closed");
}
static int interrupt(JSRuntime *rt,void *opaque){(void)rt;struct worker *w=opaque;return w->closed||uptime_ms()>=w->until;}
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
int main(void){
    struct worker w={0};w.hooks=JS_UNDEFINED;w.rt=JS_NewRuntime();if(!w.rt)return 1;JS_SetMemoryLimit(w.rt,NJW_HEAP_BYTES);JS_SetMaxStackSize(w.rt,512u*1024u);JS_SetCanBlock(w.rt,false);JS_SetInterruptHandler(w.rt,interrupt,&w);w.ctx=JS_NewContext(w.rt);if(!w.ctx){JS_FreeRuntime(w.rt);return 1;}JS_SetContextOpaque(w.ctx,&w);w.until=uptime_ms()+10000;
    if(trusted_native_init(w.ctx)<0)goto shutdown;
    JSValue global=JS_GetGlobalObject(w.ctx),host=JS_NewObject(w.ctx);const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("portSend",6,native_port_send),JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("request",3,native_request),JS_CFUNC_DEF("cancel",1,native_cancel),JS_CFUNC_DEF("import",1,native_import),JS_CFUNC_DEF("eval",2,native_eval)};JS_SetPropertyFunctionList(w.ctx,host,funcs,sizeof funcs/sizeof funcs[0]);web_js_encoding_init_isolated(w.ctx,host);web_js_navigator_init(w.ctx,host);JS_SetPropertyStr(w.ctx,global,"__workerHost",host);JS_FreeValue(w.ctx,global);
    w.hooks=JS_Eval(w.ctx,js_worker_runtime,sizeof js_worker_runtime-1,"<worker-bootstrap>",JS_EVAL_TYPE_GLOBAL);if(JS_IsException(w.hooks))goto shutdown;
    JS_SetDynamicCodeCheck(w.ctx,worker_code_check,&w);
    if(fcntl(1,F_SETFL,O_NONBLOCK)<0)goto shutdown;
    while(!w.closed||w.output){
        if(w.output_failed)break;
        if(w.closed){struct n_pollfd p={1,N_POLLOUT,0};if(poll(&p,1,-1)<0){if(errno==EINTR)continue;break;}if(p.revents&N_POLLOUT)output_flush(&w);else break;continue;}
        if(w.output)output_flush(&w);
        if(w.output_failed)break;
        if(JS_IsJobPending(w.rt)){w.until=uptime_ms()+NJW_TASK_MS;jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        /* CSP events are separate tasks, after the originating microtasks. */
        if(w.violations){w.until=uptime_ms()+NJW_TASK_MS;policy_task(&w);jobs(&w);continue;}
        struct deferred *d=NULL;if(w.head){d=w.head;w.head=d->next;if(!w.head)w.tail=NULL;w.queued-=d->h.bytes;}
        else{
            w.until=UINT64_MAX;JSValue deadline=hook(&w,"deadline",0,NULL);int64_t due=-1;JS_ToInt64(w.ctx,&due,deadline);JS_FreeValue(w.ctx,deadline);uint64_t now=uptime_ms();int wait=JS_IsJobPending(w.rt)?0:due<0?-1:due<=(int64_t)now?0:(int)(due-now);struct n_pollfd p[2]={{0,N_POLLIN,0},{1,w.output?N_POLLOUT:0,0}};int n=poll(p,2,wait);if(n<0&&errno==EINTR)continue;if(n<0)break;if(p[1].revents&N_POLLOUT)output_flush(&w);if(w.output_failed)break;if(p[0].revents&(N_POLLIN|N_POLLHUP)){d=receive(&w);if(!d)break;}
        }
        w.until=uptime_ms()+NJW_TASK_MS;
        bool received_task=d!=NULL;
        if(d){JSValue value=JS_ReadObject(w.ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE);if(JS_IsException(value))exception(&w);else if(!native_port_receive(&w,&d->h,value)){JS_FreeValue(w.ctx,value);if(JS_HasException(w.ctx))exception(&w);}else{JSValue result;if(d->h.op==NJW_LOADED){JSValue args[]={JS_NewUint32(w.ctx,d->h.request),value,JS_NewUint32(w.ctx,d->h.kind)};result=hook(&w,"loaded",3,args);JS_FreeValue(w.ctx,args[0]);JS_FreeValue(w.ctx,args[2]);}else if(d->h.op==NJW_START)result=worker_policy_start(&w,value)?hook(&w,"start",1,&value):JS_EXCEPTION;else{JSValue args[]={value,JS_NewUint32(w.ctx,d->h.op),JS_NewUint32(w.ctx,d->h.request),JS_NewUint32(w.ctx,d->h.kind)};result=hook(&w,"receive",4,args);for(unsigned i=1;i<4;i++)JS_FreeValue(w.ctx,args[i]);}if(JS_IsException(result)){exception(&w);if(d->h.op==NJW_START){send_value(&w,NJW_CLOSE,0,0,JS_NULL);w.closed=true;}}JS_FreeValue(w.ctx,result);JS_FreeValue(w.ctx,value);}free(d->data);free(d);}
        /* Finish the received task's microtasks before a local port/timer task. */
        if(received_task){jobs(&w);if(w.closed)continue;if(JS_IsJobPending(w.rt))continue;}
        JSValue now=JS_NewFloat64(w.ctx,(double)uptime_ms());JSValue tick=hook(&w,"tick",1,&now);if(JS_IsException(tick))exception(&w);JS_FreeValue(w.ctx,tick);JS_FreeValue(w.ctx,now);jobs(&w);
    }
shutdown:
    while(w.violations){struct policy_violation *v=w.violations;w.violations=v->next;JS_FreeValue(w.ctx,v->init);js_free(w.ctx,v);}
    html_policy_free(&w.policy_doc);free(w.policy_doc.url);
    while(w.output){struct outgoing*p=w.output;w.output=p->next;js_free(w.ctx,p->data);free(p);}
    while(w.head){struct deferred *d=w.head;w.head=d->next;free(d->data);free(d);}output_ports_free(&w,w.ports);while(w.leases){struct broker_lease*l=w.leases;w.leases=l->next;free(l);}JS_FreeValue(w.ctx,w.hooks);JS_FreeContext(w.ctx);JS_FreeRuntime(w.rt);return 0;
}
