/* Private native captured-target lifetime. Packets are prepared before commit;
 * releasing a pin only links already-owned packets, never allocates/calls JS. */
#ifndef WEB_JS_WORKER_PORT_LIFETIME_H
#define WEB_JS_WORKER_PORT_LIFETIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <quickjs.h>
struct broker_pair {
    struct broker_pair *next;
    uint32_t id,creator;
    size_t pins;
    bool closed,sealed,close_sent,ack_sent;
    void *held_close,*held_ack;
};
struct broker_lease {struct broker_lease *next;uint32_t id,child,port,creator;};
static uint32_t worker_port_next_lease(struct broker_lease *list,uint32_t cursor){
    uint32_t candidate=cursor==UINT32_MAX?1:cursor+1,first=candidate;
    do{bool used=false;for(struct broker_lease*p=list;p;p=p->next)if(p->id==candidate){used=true;break;}if(!used)return candidate;
        candidate=candidate==UINT32_MAX?1:candidate+1;
    }while(candidate!=first);return 0;
}
static struct broker_pair *worker_port_find(struct broker_pair *list,uint32_t id,uint32_t creator){for(;list;list=list->next)if(list->id==id&&list->creator==creator)return list;return NULL;}
static void worker_port_flush(struct broker_pair *pair,void (*publish)(void *,void *),void *owner){
    if(pair->pins)return;
    if(pair->held_close){void *packet=pair->held_close;pair->held_close=NULL;pair->close_sent=true;publish(owner,packet);}
    if(pair->held_ack){void *packet=pair->held_ack;pair->held_ack=NULL;pair->ack_sent=true;publish(owner,packet);}
}
static bool worker_port_retirable(const struct broker_pair *pair){return pair->closed&&pair->sealed&&(pair->close_sent||pair->ack_sent)&&!pair->pins&&!pair->held_close&&!pair->held_ack;}
static bool worker_port_metadata(JSContext *ctx,JSValueConst value,unsigned creator,struct broker_pair *existing,uint32_t maximum,struct broker_pair **result){
    JSValue sample=JS_NewArray(ctx);if(JS_IsException(sample))return false;JSClassID array=JS_GetClassID(sample);JS_FreeValue(ctx,sample);
    if(JS_GetClassID(value)!=array){JS_ThrowTypeError(ctx,"Invalid Worker port envelope");return false;}
    JSValue list=JS_GetPropertyUint32(ctx,value,1);if(JS_IsException(list))return false;
    if(JS_GetClassID(list)!=array){JS_FreeValue(ctx,list);JS_ThrowTypeError(ctx,"Invalid Worker port metadata");return false;}
    JSValue length=JS_GetPropertyStr(ctx,list,"length");if(JS_IsException(length)){JS_FreeValue(ctx,list);return false;}
    uint32_t count=0;bool ok=JS_IsNumber(length)&&!JS_ToUint32(ctx,&count,length);JS_FreeValue(ctx,length);
    if(!ok){JS_FreeValue(ctx,list);JS_ThrowTypeError(ctx,"Invalid Worker port metadata length");return false;}
    struct broker_pair *head=NULL;
    for(uint32_t i=0;i<count;i++){
        JSValue entry=JS_GetPropertyUint32(ctx,list,i);if(JS_IsException(entry))goto fail;
        if(JS_IsNull(entry)){JS_FreeValue(ctx,entry);continue;}
        if(JS_GetClassID(entry)!=array){JS_FreeValue(ctx,entry);JS_ThrowTypeError(ctx,"Invalid Worker port ownership entry");goto fail;}
        JSValue id=JS_GetPropertyUint32(ctx,entry,0);if(JS_IsException(id)){JS_FreeValue(ctx,entry);goto fail;}
        JSValue side=JS_GetPropertyUint32(ctx,entry,1);if(JS_IsException(side)){JS_FreeValue(ctx,id);JS_FreeValue(ctx,entry);goto fail;}
        uint32_t number=0,tag=2;double exact=0,exact_tag=0;
        ok=JS_IsNumber(id)&&JS_IsNumber(side)&&!JS_ToUint32(ctx,&number,id)&&!JS_ToUint32(ctx,&tag,side)&&
           !JS_ToFloat64(ctx,&exact,id)&&!JS_ToFloat64(ctx,&exact_tag,side)&&exact==number&&exact_tag==tag&&number&&number<=maximum&&tag==creator;
        JS_FreeValue(ctx,id);JS_FreeValue(ctx,side);JS_FreeValue(ctx,entry);
        if(ok&&worker_port_find(existing,number,tag))ok=false;
        for(struct broker_pair *p=head;ok&&p;p=p->next)if(p->id==number)ok=false;
        if(!ok){JS_ThrowTypeError(ctx,"Invalid or repeated Worker port ownership");goto fail;}
        struct broker_pair *p=calloc(1,sizeof *p);if(!p){JS_ThrowOutOfMemory(ctx);goto fail;}p->id=number;p->creator=tag;p->next=head;head=p;
    }
    JS_FreeValue(ctx,list);*result=head;return true;
fail:JS_FreeValue(ctx,list);while(head){struct broker_pair *p=head;head=p->next;free(p);}return false;
}
#endif
