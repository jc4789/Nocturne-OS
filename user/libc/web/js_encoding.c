#include "js_encoding.h"
#include "js_memory.h"
#include "webi.h"
#include <limits.h>

/* Validate WHATWG scalar UTF-8 before using QuickJS's string constructor:
   JS_NewStringLen alone accepts encoded surrogates and groups invalid bytes
   differently. A counting pass keeps allocation linear and exact. */
static size_t decode_utf8(const uint8_t *bytes, size_t length, bool fatal,
                          char *output, bool *malformed) {
    size_t written=0;
    unsigned needed=0,seen=0,low=0x80,high=0xbf;
    uint32_t cp=0;
    for (size_t i=0;i<length;i++) {
        unsigned byte=bytes[i];
        if (!needed) {
            if (byte<=0x7f) cp=byte;
            else if (byte>=0xc2&&byte<=0xdf) {needed=1;cp=byte&31;continue;}
            else if (byte>=0xe0&&byte<=0xef) {
                needed=2;cp=byte&15;
                if(byte==0xe0)low=0xa0;
                if(byte==0xed)high=0x9f;
                continue;
            } else if (byte>=0xf0&&byte<=0xf4) {
                needed=3;cp=byte&7;
                if(byte==0xf0)low=0x90;
                if(byte==0xf4)high=0x8f;
                continue;
            } else { *malformed=true;if(fatal)return SIZE_MAX;cp=0xfffd; }
        } else if (byte<low||byte>high) {
            needed=seen=0;low=0x80;high=0xbf;
            *malformed=true;if(fatal)return SIZE_MAX;cp=0xfffd;i--;
        } else {
            low=0x80;high=0xbf;cp=(cp<<6)|(byte&63);
            if(++seen!=needed)continue;
            needed=seen=0;
        }
        char encoded[4];int n=utf8_put(encoded,cp);
        if(output)memcpy(output+written,encoded,(size_t)n);
        written+=(size_t)n;cp=0;
    }
    if(needed) {
        *malformed=true;if(fatal)return SIZE_MAX;
        if(output)memcpy(output+written,"\xef\xbf\xbd",3);
        written+=3;
    }
    return written;
}

static JSValue decode_utf8_call(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, bool document) {
    (void)self;
    uint64_t offset,length;
    if(argc<5)return JS_ThrowTypeError(ctx,"UTF-8 decode requires a buffer view");
    /* Convert before borrowing data: even private calls must not retain a
       native pointer across an observable conversion or detached buffer. */
    if(JS_ToIndex(ctx,&offset,argv[1])<0||JS_ToIndex(ctx,&length,argv[2])<0)return JS_EXCEPTION;
    bool fatal=JS_ToBool(ctx,argv[3])>0,ignore_bom=JS_ToBool(ctx,argv[4])>0;
    size_t size=0;
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,argv[0]);
    if(!data&&JS_HasException(ctx))return JS_EXCEPTION;
    if(offset>size||length>size-(size_t)offset)return JS_ThrowRangeError(ctx,"UTF-8 view exceeds its buffer");
    if(length>(size_t)INT_MAX/3u)return JS_ThrowRangeError(ctx,"UTF-8 input is too large");
    const uint8_t *bytes=data?data+(size_t)offset:(const uint8_t *)"";
    size_t len=(size_t)length;
    if(!ignore_bom&&len>=3&&bytes[0]==0xef&&bytes[1]==0xbb&&bytes[2]==0xbf){bytes+=3;len-=3;}
    bool malformed=false;
    size_t written=decode_utf8(bytes,len,fatal,NULL,&malformed);
    if(written==SIZE_MAX)return JS_ThrowTypeError(ctx,"Invalid UTF-8");
    if(!malformed) {
        /* ASCII goes straight to one final string allocation. Valid Unicode
           never creates a per-character JS array or a copied source buffer. */
        if(document)web_js_prepare_bytes(ctx,len*2u+64u);
        return JS_NewStringLen(ctx,(const char *)bytes,len);
    }
    if(document)web_js_prepare_bytes(ctx,written*3u+64u);
    char *output=js_malloc(ctx,written?written:1);
    if(!output)return JS_EXCEPTION;
    decode_utf8(bytes,len,false,output,&malformed);
    JSValue result=JS_NewStringLen(ctx,output,written);
    js_free(ctx,output);
    return result;
}
static JSValue native_decode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    return decode_utf8_call(ctx,self,argc,argv,true);
}
static JSValue isolated_decode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    return decode_utf8_call(ctx,self,argc,argv,false);
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
void web_js_encoding_init_isolated(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[]={JS_CFUNC_DEF("decodeUTF8",5,isolated_decode_utf8)};
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof *functions);
}
