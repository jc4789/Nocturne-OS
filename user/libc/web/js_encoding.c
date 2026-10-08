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

static JSValue native_decode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
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
        web_js_prepare_bytes(ctx,len*2u+64u);
        return JS_NewStringLen(ctx,(const char *)bytes,len);
    }
    web_js_prepare_bytes(ctx,written*3u+64u);
    char *output=js_malloc(ctx,written?written:1);
    if(!output)return JS_EXCEPTION;
    decode_utf8(bytes,len,false,output,&malformed);
    JSValue result=JS_NewStringLen(ctx,output,written);
    js_free(ctx,output);
    return result;
}

void web_js_encoding_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[]={JS_CFUNC_DEF("decodeUTF8",5,native_decode_utf8)};
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof *functions);
}
