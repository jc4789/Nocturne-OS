/* Encoding bindings that charge a document realm's allocation budget, kept
   apart from js_encoding.c so realm-independent users (Workers, /bin/js) of
   the UTF-8 decoder do not depend on the document engine. */
#include "js_encoding.h"
#include "js_memory.h"
#include "webi.h"

static JSValue native_decode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;return web_js_decode_utf8(ctx,argc,argv,web_js_prepare_bytes);
}

struct data_url_checkpoint { JSContext *ctx; int (*poll)(JSContext *); };
static bool data_url_checkpoint(void *opaque) {
    struct data_url_checkpoint *checkpoint=opaque;
    return !checkpoint->poll || !checkpoint->poll(checkpoint->ctx);
}
JSValue web_js_data_url(JSContext *ctx,JSValueConst input,int (*poll)(JSContext *),bool document) {
    size_t length=0;const char *url=JS_ToCStringLen(ctx,&length,input);
    if(!url)return JS_EXCEPTION;
    if(memchr(url,0,length)){JS_FreeCString(ctx,url);return JS_ThrowTypeError(ctx,"Invalid serialized data URL");}
    struct web_data_url data;struct data_url_checkpoint checkpoint={ctx,poll};
    int result=web_data_url_process(url,&data,data_url_checkpoint,&checkpoint);
    JS_FreeCString(ctx,url);
    if(result!=1){
        if(result==-1)return JS_ThrowOutOfMemory(ctx);
        if(result==-2)return JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowInternalError(ctx,"Data URL conversion interrupted");
        return JS_ThrowTypeError(ctx,"Malformed data URL");
    }
    if(document)web_js_prepare_bytes(ctx,data.length);
    /* Copy into the runtime's charged ArrayBuffer allocation. The temporary
     * processor result remains exclusively native until this succeeds. */
    JSValue bytes=JS_NewArrayBufferCopy(ctx,data.body,data.length);
    JSValue mime=JS_UNDEFINED,tuple=JS_UNDEFINED;
    if(JS_IsException(bytes))goto done;
    mime=JS_NewString(ctx,data.mime);if(JS_IsException(mime))goto done;
    tuple=JS_NewArray(ctx);if(JS_IsException(tuple))goto done;
    if(JS_DefinePropertyValueUint32(ctx,tuple,0,bytes,JS_PROP_C_W_E)<0){bytes=JS_UNDEFINED;JS_FreeValue(ctx,tuple);tuple=JS_EXCEPTION;goto done;}bytes=JS_UNDEFINED;
    if(JS_DefinePropertyValueUint32(ctx,tuple,1,mime,JS_PROP_C_W_E)<0){mime=JS_UNDEFINED;JS_FreeValue(ctx,tuple);tuple=JS_EXCEPTION;goto done;}mime=JS_UNDEFINED;
done:
    web_data_url_free(&data);JS_FreeValue(ctx,bytes);JS_FreeValue(ctx,mime);
    return JS_IsUndefined(tuple)?JS_EXCEPTION:tuple;
}

void web_js_encoding_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[]={JS_CFUNC_DEF("decodeUTF8",5,native_decode_utf8)};
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof *functions);
}
