#include "js_encoding.h"
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

JSValue web_js_decode_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t)) {
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
        if(prepare)prepare(ctx,len*2u+64u);
        return JS_NewStringLen(ctx,(const char *)bytes,len);
    }
    if(prepare)prepare(ctx,written*3u+64u);
    char *output=js_malloc(ctx,written?written:1);
    if(!output)return JS_EXCEPTION;
    decode_utf8(bytes,len,false,output,&malformed);
    JSValue result=JS_NewStringLen(ctx,output,written);
    js_free(ctx,output);
    return result;
}
/* QuickJS writes a lone surrogate in its three-byte form ED A0..BF xx, the
   length of U+FFFD's: the Encoding Standard's scalar values come from
   replacing those in place. ED never occurs as a continuation byte. */
static void replace_lone_surrogates(uint8_t *bytes, size_t length) {
    for (uint8_t *p=memchr(bytes,0xed,length);p;p=memchr(p,0xed,length-(size_t)(p-bytes))) {
        if(p[1]>=0xa0){p[0]=0xef;p[1]=0xbf;p[2]=0xbd;}
        p+=3;
        if((size_t)(p-bytes)>=length)break;
    }
}

JSValue web_js_encode_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t)) {
    if(argc<1||!JS_IsString(argv[0]))return JS_ThrowTypeError(ctx,"UTF-8 encode requires a string");
    size_t length=0;
    const char *text=JS_ToCStringLen(ctx,&length,argv[0]);
    if(!text)return JS_EXCEPTION;
    if(prepare)prepare(ctx,length);
    JSValue buffer=JS_NewArrayBufferCopy(ctx,(const uint8_t *)text,length);
    JS_FreeCString(ctx,text);
    if(JS_IsException(buffer))return buffer;
    size_t size=0;
    uint8_t *bytes=JS_GetArrayBuffer(ctx,&size,buffer);
    if(bytes&&size)replace_lone_surrogates(bytes,size);
    return buffer;
}

JSValue web_js_encode_utf8_into(JSContext *ctx, int argc, JSValueConst *argv) {
    uint64_t offset,capacity;
    if(argc<4||!JS_IsString(argv[0]))return JS_ThrowTypeError(ctx,"UTF-8 encodeInto requires a string and a buffer view");
    if(JS_ToIndex(ctx,&offset,argv[2])<0||JS_ToIndex(ctx,&capacity,argv[3])<0)return JS_EXCEPTION;
    size_t length=0;
    const char *text=JS_ToCStringLen(ctx,&length,argv[0]);
    if(!text)return JS_EXCEPTION;
    size_t size=0;
    uint8_t *data=JS_GetArrayBuffer(ctx,&size,argv[1]);
    if(!data&&JS_HasException(ctx)){JS_FreeCString(ctx,text);return JS_EXCEPTION;}
    if(offset>size||capacity>size-(size_t)offset){
        JS_FreeCString(ctx,text);
        return JS_ThrowRangeError(ctx,"UTF-8 view exceeds its buffer");
    }
    /* Whole scalar values only, as many as fit; a four-byte one was two
       UTF-16 code units of the source. */
    size_t read=0,written=0;
    while(written<length){
        unsigned lead=(unsigned char)text[written];
        size_t n=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:4;
        if(n>capacity-written)break;
        read+=n==4?2:1;
        written+=n;
    }
    if(written){
        memcpy(data+(size_t)offset,text,written);
        replace_lone_surrogates(data+(size_t)offset,written);
    }
    JS_FreeCString(ctx,text);
    JSValue result=JS_NewObject(ctx);
    if(JS_IsException(result))return result;
    if(JS_SetPropertyStr(ctx,result,"read",JS_NewInt64(ctx,(int64_t)read))<0||
       JS_SetPropertyStr(ctx,result,"written",JS_NewInt64(ctx,(int64_t)written))<0){
        JS_FreeValue(ctx,result);
        return JS_EXCEPTION;
    }
    return result;
}

struct concat_part { JSValue value; const char *text; const uint8_t *bytes; size_t length; };

JSValue web_js_concat_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t)) {
    int array=argc<1?0:JS_IsArray(ctx,argv[0]);
    if(array<0)return JS_EXCEPTION;
    if(!array)return JS_ThrowTypeError(ctx,"UTF-8 concatenation requires an array");
    int64_t count=0;
    JSValue length_value=JS_GetPropertyStr(ctx,argv[0],"length");
    int converted=JS_ToInt64(ctx,&count,length_value);
    JS_FreeValue(ctx,length_value);
    if(converted<0)return JS_EXCEPTION;
    if(count<0||(uint64_t)count>SIZE_MAX/sizeof(struct concat_part))return JS_ThrowRangeError(ctx,"Too many parts");
    struct concat_part *parts=js_mallocz(ctx,(size_t)(count?count:1)*sizeof *parts);
    if(!parts)return JS_EXCEPTION;
    JSValue result=JS_EXCEPTION;
    size_t total=0,held=0;
    /* Borrow every part first: a string's UTF-8 and a view's bytes stay valid
       while the part is held, and no JavaScript runs in between. */
    while(held<(size_t)count){
        struct concat_part *part=&parts[held];
        part->value=JS_GetPropertyUint32(ctx,argv[0],(uint32_t)held++);
        if(JS_IsException(part->value))goto done;
        if(JS_IsString(part->value)){
            part->text=JS_ToCStringLen(ctx,&part->length,part->value);
            if(!part->text)goto done;
        }else{
            size_t offset=0,size=0;
            JSValue buffer=JS_GetTypedArrayBuffer(ctx,part->value,&offset,&part->length,NULL);
            if(JS_IsException(buffer))goto done;
            uint8_t *data=JS_GetArrayBuffer(ctx,&size,buffer);
            JS_FreeValue(ctx,buffer);
            if(!data&&JS_HasException(ctx))goto done;
            if(offset>size||part->length>size-offset){JS_ThrowRangeError(ctx,"View exceeds its buffer");goto done;}
            part->bytes=data?data+offset:NULL;
        }
        if(part->length>(size_t)INT32_MAX-total){JS_ThrowRangeError(ctx,"Byte length is not representable by ArrayBuffer");goto done;}
        total+=part->length;
    }
    if(prepare)prepare(ctx,total);
    result=JS_NewArrayBufferCopy(ctx,NULL,total);
    if(JS_IsException(result))goto done;
    size_t size=0;
    uint8_t *out=JS_GetArrayBuffer(ctx,&size,result);
    for(size_t i=0,at=0;i<held&&out;i++){
        const struct concat_part *part=&parts[i];
        if(!part->length)continue;
        memcpy(out+at,part->text?(const uint8_t *)part->text:part->bytes,part->length);
        if(part->text)replace_lone_surrogates(out+at,part->length);
        at+=part->length;
    }
done:
    for(size_t i=0;i<held;i++){
        if(parts[i].text)JS_FreeCString(ctx,parts[i].text);
        JS_FreeValue(ctx,parts[i].value);
    }
    js_free(ctx,parts);
    return result;
}

static JSValue isolated_decode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;return web_js_decode_utf8(ctx,argc,argv,NULL);
}
static JSValue isolated_encode_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;return web_js_encode_utf8(ctx,argc,argv,NULL);
}
static JSValue isolated_encode_utf8_into(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;return web_js_encode_utf8_into(ctx,argc,argv);
}
static JSValue isolated_concat_utf8(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;return web_js_concat_utf8(ctx,argc,argv,NULL);
}

void web_js_encoding_init_isolated(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[]={
        JS_CFUNC_DEF("decodeUTF8",5,isolated_decode_utf8),
        JS_CFUNC_DEF("encodeUTF8",1,isolated_encode_utf8),
        JS_CFUNC_DEF("encodeUTF8Into",4,isolated_encode_utf8_into),
        JS_CFUNC_DEF("concatUTF8",1,isolated_concat_utf8)
    };
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof *functions);
}
