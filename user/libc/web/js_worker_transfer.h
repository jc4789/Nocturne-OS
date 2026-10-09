/* Private Worker transfer: preflight may allocate; publication cannot. */
#ifndef WEB_JS_WORKER_TRANSFER_H
#define WEB_JS_WORKER_TRANSFER_H
#include <stdint.h>
#include <stdbool.h>
#include <quickjs.h>
struct worker_transfer_write {JSValue object,value,old;JSAtom key;};
static JSValue worker_transfer_field(JSContext *ctx,JSValueConst object,JSAtom atom,JSClassID ordinary) {
    if(JS_GetClassID(object)!=ordinary)return JS_ThrowTypeError(ctx,"Invalid private worker transfer record");
    JSPropertyDescriptor d;int found=JS_GetOwnProperty(ctx,&d,object,atom);
    if(found<0)return JS_EXCEPTION;
    if(!found)return JS_ThrowTypeError(ctx,"Missing private worker transfer field");
    if(d.flags&JS_PROP_GETSET){JS_FreeValue(ctx,d.getter);JS_FreeValue(ctx,d.setter);return JS_ThrowTypeError(ctx,"Private transfer accessor forbidden");}
    return d.value;
}
static JSValue worker_transfer_index(JSContext *ctx,JSValueConst array,uint32_t index) {
    JSAtom atom=JS_NewAtomUInt32(ctx,index);if(atom==JS_ATOM_NULL)return JS_EXCEPTION;
    JSPropertyDescriptor d;int found=JS_GetOwnProperty(ctx,&d,array,atom);JS_FreeAtom(ctx,atom);
    if(found<0)return JS_EXCEPTION;
    if(!found)return JS_ThrowTypeError(ctx,"Missing private worker transfer item");
    if(d.flags&JS_PROP_GETSET){JS_FreeValue(ctx,d.getter);JS_FreeValue(ctx,d.setter);return JS_ThrowTypeError(ctx,"Private transfer accessor forbidden");}
    return d.value;
}
static bool worker_transfer_length(JSContext *ctx,JSValueConst array,JSClassID array_class,uint32_t *length) {
    if(JS_GetClassID(array)!=array_class){JS_ThrowTypeError(ctx,"Invalid private worker transfer sequence");return false;}
    JSValue n=JS_GetPropertyStr(ctx,array,"length");
    bool ok=JS_IsNumber(n)&&!JS_ToUint32(ctx,length,n);JS_FreeValue(ctx,n);
    if(!ok)JS_ThrowTypeError(ctx,"Invalid private worker transfer length");return ok;
}
static JSValue worker_transfer_commit_publish(JSContext *ctx,JSValueConst plan,bool active,void (*publish)(void *),void *opaque) {
    JSValue result=JS_EXCEPTION,sample=JS_NewObject(ctx),array=JS_NewArray(ctx),buffer=JS_NewArrayBufferCopy(ctx,NULL,0);
    JSValue bs=JS_UNDEFINED,ws=JS_UNDEFINED,cs=JS_UNDEFINED;
    JSAtom atoms[6]={0};JSValue *buffers=NULL;struct worker_transfer_write *writes=NULL;
    uint32_t nb=0,nw=0,nc=0,built=0,held=0;
    if(JS_IsException(sample)||JS_IsException(array)||JS_IsException(buffer))goto out;
    if(!active){JS_ThrowTypeError(ctx,"Worker transfer destination is inactive");goto out;}
    JSClassID ordinary=JS_GetClassID(sample),array_class=JS_GetClassID(array),buffer_class=JS_GetClassID(buffer);
    const char *names[]={"buffers","writes","cancels","object","key","value"};
    for(unsigned i=0;i<6;i++){atoms[i]=JS_NewAtom(ctx,names[i]);if(atoms[i]==JS_ATOM_NULL)goto out;}
    bs=worker_transfer_field(ctx,plan,atoms[0],ordinary);ws=worker_transfer_field(ctx,plan,atoms[1],ordinary);cs=worker_transfer_field(ctx,plan,atoms[2],ordinary);
    if(JS_IsException(bs)||JS_IsException(ws)||JS_IsException(cs))goto out;
    if(!worker_transfer_length(ctx,bs,array_class,&nb)||!worker_transfer_length(ctx,ws,array_class,&nw)||!worker_transfer_length(ctx,cs,array_class,&nc))goto out;
    if(nc){JS_ThrowTypeError(ctx,"External worker transfer cancellation unsupported");goto out;}
    if((uint64_t)nb*sizeof *buffers>SIZE_MAX||(uint64_t)nw*sizeof *writes>SIZE_MAX){JS_ThrowRangeError(ctx,"Worker transfer allocation overflow");goto out;}
    if(nb){buffers=js_malloc(ctx,(size_t)nb*sizeof *buffers);if(!buffers)goto out;}
    if(nw){writes=js_mallocz(ctx,(size_t)nw*sizeof *writes);if(!writes)goto out;}
    for(uint32_t i=0;i<nb;i++){
        JSValue value=worker_transfer_index(ctx,bs,i);size_t size=0;
        if(JS_IsException(value))goto out;
        if(JS_GetClassID(value)!=buffer_class){JS_FreeValue(ctx,value);JS_ThrowTypeError(ctx,"Invalid worker transfer buffer");goto out;}
        JS_GetArrayBuffer(ctx,&size,value);if(JS_HasException(ctx)){JS_FreeValue(ctx,value);goto out;}
        buffers[held++]=value;
    }
    for(uint32_t i=0;i<nw;i++){
        JSValue entry=worker_transfer_index(ctx,ws,i);if(JS_IsException(entry))goto out;
        JSValue object=worker_transfer_field(ctx,entry,atoms[3],ordinary),key=worker_transfer_field(ctx,entry,atoms[4],ordinary),value=worker_transfer_field(ctx,entry,atoms[5],ordinary);JS_FreeValue(ctx,entry);
        if(JS_IsException(object)||JS_IsException(key)||JS_IsException(value)){JS_FreeValue(ctx,object);JS_FreeValue(ctx,key);JS_FreeValue(ctx,value);goto out;}
        if(JS_GetClassID(object)!=ordinary||!JS_IsString(key)){JS_FreeValue(ctx,object);JS_FreeValue(ctx,key);JS_FreeValue(ctx,value);JS_ThrowTypeError(ctx,"Invalid worker transfer write");goto out;}
        JSAtom atom=JS_ValueToAtom(ctx,key);JS_FreeValue(ctx,key);
        if(atom==JS_ATOM_NULL){JS_FreeValue(ctx,object);JS_FreeValue(ctx,value);goto out;}
        JSPropertyDescriptor d;int found=JS_GetOwnProperty(ctx,&d,object,atom);
        if(found!=1||(d.flags&(JS_PROP_TMASK|JS_PROP_WRITABLE|JS_PROP_LENGTH))!=JS_PROP_WRITABLE){
            if(found==1){JS_FreeValue(ctx,d.value);JS_FreeValue(ctx,d.getter);JS_FreeValue(ctx,d.setter);}
            JS_FreeAtom(ctx,atom);JS_FreeValue(ctx,object);JS_FreeValue(ctx,value);
            if(found>=0)JS_ThrowTypeError(ctx,"Worker transfer requires an existing writable own slot");goto out;
        }
        writes[built++]=(struct worker_transfer_write){object,value,d.value,atom};
    }
    /* No allocation, JS call, conversion or property creation through commit.
     * Old values are retained until every detach/write has been published. */
    for(uint32_t i=0;i<nb;i++)JS_DetachArrayBuffer(ctx,buffers[i]);
    for(uint32_t i=0;i<nw;i++)JS_SetProperty(ctx,writes[i].object,writes[i].key,JS_DupValue(ctx,writes[i].value));
    if(publish)publish(opaque);
    result=JS_UNDEFINED;
out:
    for(uint32_t i=0;i<built;i++){JS_FreeValue(ctx,writes[i].object);JS_FreeValue(ctx,writes[i].value);JS_FreeValue(ctx,writes[i].old);JS_FreeAtom(ctx,writes[i].key);}
    for(uint32_t i=0;i<held;i++)JS_FreeValue(ctx,buffers[i]);
    js_free(ctx,writes);js_free(ctx,buffers);for(unsigned i=0;i<6;i++)JS_FreeAtom(ctx,atoms[i]);
    JS_FreeValue(ctx,bs);JS_FreeValue(ctx,ws);JS_FreeValue(ctx,cs);JS_FreeValue(ctx,sample);JS_FreeValue(ctx,array);JS_FreeValue(ctx,buffer);return result;
}
static JSValue worker_transfer_commit(JSContext *ctx,JSValueConst plan,bool active){return worker_transfer_commit_publish(ctx,plan,active,NULL,NULL);}
#endif
