/* IndexedDB: the native half of js_indexeddb.js (js_indexeddb.h). Origin and lifetime come from
 * the realm's owner, never from a caller-selected path; busy leases are retried by JS tasks. */
#include "js_indexeddb.h"
#include <webprefs.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct web_idb_pending {
    struct web_idb_pending *next;
    uint32_t sequence;
    unsigned operation;
    JSValue resolve, reject;
};
static const struct web_idb_realm *owner;

static JSValue idb_result(JSContext *ctx,int status,JSValue first,JSValue second,int count) {
    JSValue result=JS_NewArray(ctx);
    if(JS_IsException(result) || JS_IsException(first) || JS_IsException(second))goto failed;
    if(JS_DefinePropertyValueUint32(ctx,result,0,JS_NewInt32(ctx,status),JS_PROP_C_W_E)<0)goto failed;
    JSValue value=first;first=JS_UNDEFINED;
    if(JS_DefinePropertyValueUint32(ctx,result,1,value,JS_PROP_C_W_E)<0)goto failed;
    if(count==3){value=second;second=JS_UNDEFINED;
        if(JS_DefinePropertyValueUint32(ctx,result,2,value,JS_PROP_C_W_E)<0)goto failed;}
    JS_FreeValue(ctx,second);return result;
failed:
    JS_FreeValue(ctx,result);JS_FreeValue(ctx,first);JS_FreeValue(ctx,second);return JS_EXCEPTION;
}
static bool idb_buffer(JSContext *ctx,JSValueConst value,uint8_t **bytes,size_t *length) {
    /* GetArrayBuffer reports wrong brands/detachment, but an empty owned
     * buffer may legitimately have a NULL backing pointer. */
    *bytes=JS_GetArrayBuffer(ctx,length,value);
    if(*bytes)return true;
    return !JS_HasException(ctx);
}
static void idb_buffer_free(JSRuntime *runtime,void *opaque,void *bytes) {
    (void)runtime;(void)opaque;free(bytes);
}
static JSValue idb_take_buffer(JSContext *ctx,struct webidb_async_result *out) {
    /* The broker result owns malloc storage, not QuickJS allocator storage.
       Transfer it only after successful wrapper construction; on OOM the
       ordinary result destructor still owns it. No page-size memcpy here. */
    JSValue value=JS_NewArrayBuffer(ctx,out->bytes,out->length,idb_buffer_free,NULL,false);
    if(!JS_IsException(value)){out->bytes=NULL;out->length=0;}
    return value;
}
/* The result as js_indexeddb.js reads it: [status, ...]. */
static JSValue idb_completion(JSContext *ctx,struct webidb_async_result *out) {
    switch(out->operation){
    case WEB_IDB_DATABASES: {
        JSValue list=JS_NewArray(ctx);
        if(JS_IsException(list))return list;
        for(size_t i=0;i<out->item_count;i++){
            if(i>=UINT32_MAX){JS_FreeValue(ctx,list);return JS_ThrowOutOfMemory(ctx);}
            JSValue item=JS_NewObject(ctx);
            if(JS_IsException(item)){JS_FreeValue(ctx,list);return JS_EXCEPTION;}
            JSValue name=JS_NewStringLen(ctx,out->items[i].name,out->items[i].name_length);
            if(JS_IsException(name)||
               JS_DefinePropertyValueStr(ctx,item,"name",name,JS_PROP_C_W_E)<0||
               JS_DefinePropertyValueStr(ctx,item,"version",JS_NewFloat64(ctx,(double)out->items[i].version),JS_PROP_C_W_E)<0){
                JS_FreeValue(ctx,item);JS_FreeValue(ctx,list);return JS_EXCEPTION;
            }
            if(JS_DefinePropertyValueUint32(ctx,list,(uint32_t)i,item,JS_PROP_C_W_E)<0){JS_FreeValue(ctx,list);return JS_EXCEPTION;}
        }
        return idb_result(ctx,out->status,list,JS_UNDEFINED,2);
    }
    case WEB_IDB_BEGIN:
        return idb_result(ctx,out->status,JS_NewUint32(ctx,out->token),
            out->bytes?idb_take_buffer(ctx,out):JS_NULL,3);
    case WEB_IDB_STAGE:
        return idb_result(ctx,out->status,JS_NewString(ctx,out->reference),JS_UNDEFINED,2);
    case WEB_IDB_VALUE:
        return idb_result(ctx,out->status,out->status==WEB_IDB_OK?
            idb_take_buffer(ctx,out):JS_NULL,JS_UNDEFINED,2);
    case WEB_IDB_VALUES: {
        JSValue list=JS_NewArray(ctx);
        if(JS_IsException(list))return list;
        size_t offset=0;
        if(out->status==WEB_IDB_OK)for(size_t i=0;i<out->count;i++){
            if(i>=UINT32_MAX||offset>out->length||out->length-offset<sizeof(uint64_t))goto malformed;
            uint64_t length;memcpy(&length,out->bytes+offset,sizeof length);offset+=sizeof length;
            if(length>SIZE_MAX||length>out->length-offset)goto malformed;
            JSValue value=JS_NewArrayBufferCopy(ctx,out->bytes+offset,(size_t)length);
            if(JS_IsException(value)||JS_DefinePropertyValueUint32(ctx,list,(uint32_t)i,value,JS_PROP_C_W_E)<0){
                JS_FreeValue(ctx,list);return JS_EXCEPTION;
            }
            offset+=(size_t)length;
        }
        if(out->status==WEB_IDB_OK&&offset!=out->length)goto malformed;
        return idb_result(ctx,out->status,list,JS_UNDEFINED,2);
malformed:
        JS_FreeValue(ctx,list);return JS_ThrowInternalError(ctx,"Invalid IndexedDB value batch wire");
    }
    case WEB_IDB_CONNECT:
        return idb_result(ctx,out->status,JS_NewUint32(ctx,out->token),JS_UNDEFINED,2);
    case WEB_IDB_CONNECTIONS:
        /* [status, connections still open, those of them whose versionchange has not been fired] */
        return idb_result(ctx,out->status,JS_NewFloat64(ctx,(double)out->count),JS_NewFloat64(ctx,(double)(uint32_t)out->pending),3);
    case WEB_IDB_POLL:
        /* [status, the versionchange's version (null: none), whether the connection is closed forcibly] */
        return idb_result(ctx,out->status,out->pending==1?JS_NewFloat64(ctx,(double)out->version):JS_NULL,JS_NewBool(ctx,out->pending==2),3);
    default:return JS_NewInt32(ctx,out->status);
    }
}
static void idb_broker_failed(struct web_idb *idb) {
    webidb_async_free(idb->broker);idb->broker=NULL;
    idb->failed=true;
}
void web_idb_pump(struct web_idb *idb) {
    if(idb&&idb->broker)webidb_async_pump(idb->broker);
}
bool web_idb_used(const struct web_idb *idb) {return idb&&idb->used;}
bool web_idb_waiting(const struct web_idb *idb) {return idb&&idb->pending;}
static struct web_idb_pending **idb_find(struct web_idb *idb,uint32_t sequence,struct web_idb_pending **previous) {
    struct web_idb_pending **link=&idb->pending;*previous=NULL;
    while(*link&&(*link)->sequence!=sequence){*previous=*link;link=&(*link)->next;}
    return link;
}
uint32_t web_idb_next(struct web_idb *idb) {
    if(!idb)return 0;
    for(;;){
        if(idb->has_taken)return idb->taken.sequence;
        if(!idb->broker&&!idb->failed)return 0;
        if(idb->broker){
            webidb_async_pump(idb->broker);
            if(webidb_async_failed(idb->broker))idb_broker_failed(idb);
        }
        struct webidb_async_result out={0};
        if(idb->failed){
            /* An old JS connection must never alias a restarted helper's handles. */
            if(!idb->pending)return 0;
            out.sequence=idb->pending->sequence;
            out.operation=idb->pending->operation;out.status=WEB_IDB_IO;
        }else if(!webidb_async_take(idb->broker,&out))return 0;
        struct web_idb_pending *previous;
        /* A cleanup's result (below) has no request waiting for it. */
        if(!*idb_find(idb,out.sequence,&previous)){webidb_async_result_free(&out);continue;}
        idb->taken=out;idb->has_taken=true;
    }
}
void web_idb_complete(JSContext *ctx,struct web_idb *idb) {
    if(!idb||!idb->has_taken)return;
    struct webidb_async_result out=idb->taken;
    memset(&idb->taken,0,sizeof idb->taken);idb->has_taken=false;
    struct web_idb_pending *previous,**link=idb_find(idb,out.sequence,&previous),*p=*link;
    if(!p){webidb_async_result_free(&out);return;}
    *link=p->next;
    if(idb->pending_last==p)idb->pending_last=previous;
    JSValue value=idb_completion(ctx,&out);
    JSValue fn=p->resolve;
    if(JS_IsException(value)){
        value=JS_GetException(ctx);fn=p->reject;
        if(out.token&&(out.operation==WEB_IDB_BEGIN||out.operation==WEB_IDB_CONNECT)&&idb->broker){
            struct webidb_async_request cleanup={.operation=out.operation==WEB_IDB_BEGIN?WEB_IDB_ABORT:WEB_IDB_FORGET,.token=out.token};
            if(!webidb_async_submit(idb->broker,&cleanup))idb_broker_failed(idb);
        }
    }
    JSValue called=JS_Call(ctx,fn,JS_UNDEFINED,1,&value);
    /* An exception settling the promise stays pending for the caller's task to report. */
    JS_FreeValue(ctx,called);JS_FreeValue(ctx,value);
    JS_FreeValue(ctx,p->resolve);JS_FreeValue(ctx,p->reject);js_free(ctx,p);
    webidb_async_result_free(&out);
}
void web_idb_release(JSContext *ctx,struct web_idb *idb) {
    if(!idb)return;
    webidb_async_free(idb->broker);idb->broker=NULL;
    if(idb->has_taken){webidb_async_result_free(&idb->taken);idb->has_taken=false;}
    while(idb->pending){
        struct web_idb_pending *p=idb->pending;
        idb->pending=p->next;
        JS_FreeValue(ctx,p->resolve);JS_FreeValue(ctx,p->reject);js_free(ctx,p);
    }
    idb->pending_last=NULL;
    idb->failed=false;idb->used=false;
}
/* host.indexedDBStorageKey(): open and deleteDatabase throw SecurityError at once when it is
   false: an opaque origin (a sandbox without allow-same-origin, a data: URL), or a site whose
   storage the user blocked (webprefs.h). */
static JSValue idb_storage_key(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    (void)this_val;(void)argc;(void)argv;
    const char *origin=owner&&owner->state(ctx)?owner->origin(ctx):NULL;
    if(JS_HasException(ctx)){if(origin)JS_FreeCString(ctx,origin);return JS_EXCEPTION;}
    bool allowed=origin&&webprefs_storage_allowed(origin);
    if(origin)JS_FreeCString(ctx,origin);
    return JS_NewBool(ctx,allowed);
}
/* host.indexedDB(operation, ...): a promise of the broker's result. */
static JSValue idb_native(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    (void)this_val;
    struct web_idb *idb=owner?owner->state(ctx):NULL;
    if(!idb)return JS_ThrowTypeError(ctx,"Retired IndexedDB realm");
    JSValue refused=owner->authority(ctx);
    if(JS_IsException(refused))return refused;
    if(!argc)return JS_ThrowTypeError(ctx,"IndexedDB operation required");
    const char *op=JS_ToCString(ctx,argv[0]);
    if(!op)return JS_EXCEPTION;
    static const struct {const char *name;unsigned operation;} operations[]={
        {"begin",WEB_IDB_BEGIN},{"stage",WEB_IDB_STAGE},{"value",WEB_IDB_VALUE},{"commit",WEB_IDB_COMMIT},
        {"abort",WEB_IDB_ABORT},{"databases",WEB_IDB_DATABASES},{"connections",WEB_IDB_CONNECTIONS},
        {"connect",WEB_IDB_CONNECT},{"poll",WEB_IDB_POLL},{"disconnect",WEB_IDB_DISCONNECT},{"forget",WEB_IDB_FORGET},
        {"values",WEB_IDB_VALUES},{"fired",WEB_IDB_FIRED}};
    struct webidb_async_request request={0};
    for(unsigned i=0;i<sizeof operations/sizeof *operations;i++)if(!strcmp(op,operations[i].name)){request.operation=operations[i].operation;break;}
    JS_FreeCString(ctx,op);
    if(!request.operation)return JS_ThrowTypeError(ctx,"Invalid IndexedDB operation");
    JSValue result=JS_EXCEPTION;
    const char *origin=NULL,*name=NULL,*reference=NULL;
    const char **references=NULL;
    size_t acquired=0;
    if(request.operation==WEB_IDB_BEGIN||request.operation==WEB_IDB_DATABASES){
        origin=owner->origin(ctx);if(JS_HasException(ctx))goto done;
        request.origin=origin;
        if(request.operation==WEB_IDB_BEGIN){
            if(argc<2){JS_ThrowTypeError(ctx,"IndexedDB name required");goto done;}
            name=JS_ToCStringLen(ctx,&request.name_length,argv[1]);if(!name)goto done;
            request.name=name;
        }
    }else{
        if(argc<2){JS_ThrowTypeError(ctx,"IndexedDB token required");goto done;}
        if(JS_ToUint32(ctx,&request.token,argv[1])<0)goto done;
    }
    if(request.operation==WEB_IDB_CONNECT||request.operation==WEB_IDB_CONNECTIONS||request.operation==WEB_IDB_COMMIT){
        int version_index=request.operation==WEB_IDB_COMMIT?4:2;
        double version;
        if(argc<=version_index){JS_ThrowTypeError(ctx,"IndexedDB version required");goto done;}
        if(JS_ToFloat64(ctx,&version,argv[version_index])<0)goto done;
        if(!isfinite(version)||version<0||version>9007199254740991.0||floor(version)!=version){JS_ThrowTypeError(ctx,"Invalid IndexedDB version");goto done;}
        request.version=(uint64_t)version;
    }
    if(request.operation==WEB_IDB_STAGE||request.operation==WEB_IDB_COMMIT){
        if(argc<3){JS_ThrowTypeError(ctx,"IndexedDB payload required");goto done;}
        uint8_t *bytes=NULL;
        if(!idb_buffer(ctx,argv[2],&bytes,&request.length))goto done;
        request.bytes=bytes;
    }
    if(request.operation==WEB_IDB_VALUE){
        if(argc<3){JS_ThrowTypeError(ctx,"IndexedDB reference required");goto done;}
        reference=JS_ToCString(ctx,argv[2]);if(!reference)goto done;
        request.reference=reference;
    }
    if(request.operation==WEB_IDB_COMMIT||request.operation==WEB_IDB_VALUES){
        int index=request.operation==WEB_IDB_COMMIT?3:2;
        if(argc<=index){JS_ThrowTypeError(ctx,"IndexedDB references required");goto done;}
        JSValue count_value=JS_GetPropertyStr(ctx,argv[index],"length");uint32_t count=0;
        int failed=JS_IsException(count_value)?-1:JS_ToUint32(ctx,&count,count_value);
        JS_FreeValue(ctx,count_value);if(failed<0)goto done;
        if((size_t)count>SIZE_MAX/sizeof *references){JS_ThrowOutOfMemory(ctx);goto done;}
        references=count?js_mallocz(ctx,(size_t)count*sizeof *references):NULL;
        if(count&&!references)goto done;
        for(;acquired<count;acquired++){
            JSValue value=JS_GetPropertyUint32(ctx,argv[index],(uint32_t)acquired);
            if(JS_IsException(value)){JS_FreeValue(ctx,value);goto done;}
            references[acquired]=JS_ToCString(ctx,value);JS_FreeValue(ctx,value);
            if(!references[acquired])goto done;
        }
        request.references=references;request.reference_count=count;
    }
    /* The conversions above ran author code: the realm may have gone meanwhile. */
    if(owner->state(ctx)!=idb){JS_ThrowTypeError(ctx,"Retired IndexedDB realm");goto done;}
    refused=owner->authority(ctx);
    if(JS_IsException(refused))goto done;
    JSValue callbacks[2];
    result=JS_NewPromiseCapability(ctx,callbacks);
    if(JS_IsException(result))goto done;
    struct web_idb_pending *pending=js_mallocz(ctx,sizeof *pending);
    if(!pending){JS_FreeValue(ctx,result);result=JS_EXCEPTION;JS_FreeValue(ctx,callbacks[0]);JS_FreeValue(ctx,callbacks[1]);goto done;}
    if(!idb->broker&&!idb->failed)idb->broker=webidb_async_create();
    idb->used=true;
    uint32_t sequence=0;
    if(idb->broker && (origin||(request.operation!=WEB_IDB_BEGIN&&request.operation!=WEB_IDB_DATABASES)))
        sequence=webidb_async_submit(idb->broker,&request);
    if(webidb_async_failed(idb->broker))idb_broker_failed(idb);
    if(!sequence){
        struct webidb_async_result out={.operation=request.operation,.status=!origin&&(request.operation==WEB_IDB_BEGIN||request.operation==WEB_IDB_DATABASES)?WEB_IDB_SECURITY:idb->failed?WEB_IDB_IO:WEB_IDB_MEMORY};
        JSValue value=idb_completion(ctx,&out);
        JSValue fn=callbacks[0];
        if(JS_IsException(value)){value=JS_GetException(ctx);fn=callbacks[1];}
        JSValue called=JS_Call(ctx,fn,JS_UNDEFINED,1,&value);
        JS_FreeValue(ctx,value);JS_FreeValue(ctx,callbacks[0]);JS_FreeValue(ctx,callbacks[1]);js_free(ctx,pending);
        if(JS_IsException(called)){JS_FreeValue(ctx,result);result=JS_EXCEPTION;}JS_FreeValue(ctx,called);
    }else{
        pending->sequence=sequence;pending->operation=request.operation;
        pending->resolve=callbacks[0];pending->reject=callbacks[1];
        if(idb->pending_last)idb->pending_last->next=pending;
        else idb->pending=pending;
        idb->pending_last=pending;
    }
done:
    for(size_t i=0;i<acquired;i++)JS_FreeCString(ctx,references[i]);js_free(ctx,references);
    JS_FreeCString(ctx,reference);JS_FreeCString(ctx,name);JS_FreeCString(ctx,origin);return result;
}
void web_idb_host_init(JSContext *ctx,JSValueConst host,const struct web_idb_realm *realm) {
    static const JSCFunctionListEntry functions[]={
        JS_CFUNC_DEF("indexedDB",5,idb_native),
        JS_CFUNC_DEF("indexedDBStorageKey",0,idb_storage_key),
    };
    owner=realm;
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof functions[0]);
}
