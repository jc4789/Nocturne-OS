#include "js_crypto.h"
#include <bearssl_hash.h>
#include <bearssl_block.h>
#include <bearssl_aead.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void crypto_wipe(void *p,size_t length){volatile uint8_t *v=p;while(length--)*v++=0;}
static void crypto_counter_add(uint8_t counter[16],int bits,uint64_t blocks){
    for(int i=15;i>=0&&bits>0&&blocks;--i){
        unsigned width=bits>=8?8:(unsigned)bits,mask=(1u<<width)-1;
        unsigned sum=(counter[i]&mask)+(unsigned)(blocks&mask);
        counter[i]=(uint8_t)((counter[i]&~mask)|(sum&mask));
        blocks=(blocks>>width)+(sum>>width);bits-=(int)width;
    }
}

static void crypto_buffer_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)opaque;
    js_free_rt(rt, ptr);
}

static JSValue crypto_random(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint64_t length;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Random request requires a length");
    if (JS_ToIndex(ctx, &length, argv[0]) < 0) return JS_EXCEPTION;
    if (length > 65536) return JS_ThrowRangeError(ctx, "Random request exceeds 65536 bytes");
    if (!length) return JS_NewArrayBufferCopy(ctx, NULL, 0);
    uint8_t *bytes = js_malloc(ctx, (size_t)length);
    if (!bytes) return JS_EXCEPTION;
    int fd = open("/dev/random", O_RDONLY);
    if (fd < 0) {
        js_free(ctx, bytes);
        return JS_ThrowInternalError(ctx, "System random source is unavailable");
    }
    size_t done = 0;
    while (done < length) {
        ssize_t n = read(fd, bytes + done, (size_t)length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > (size_t)length - done) {
            close(fd);
            memset(bytes, 0, (size_t)length);
            js_free(ctx, bytes);
            return JS_ThrowInternalError(ctx, "System random source could not fill the request");
        }
        done += (size_t)n;
    }
    close(fd);
    JSValue result = JS_NewArrayBuffer(ctx, bytes, (size_t)length, crypto_buffer_free, NULL, 0);
    /* JS_NewArrayBuffer does not take ownership on allocation failure. */
    if (JS_IsException(result)) {
        memset(bytes, 0, (size_t)length);
        js_free(ctx, bytes);
    }
    return result;
}

static JSValue crypto_digest(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Digest requires algorithm and data");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const br_hash_class *hash = NULL;
    if (!strcmp(name, "SHA-1")) hash = &br_sha1_vtable;
    else if (!strcmp(name, "SHA-256")) hash = &br_sha256_vtable;
    else if (!strcmp(name, "SHA-384")) hash = &br_sha384_vtable;
    else if (!strcmp(name, "SHA-512")) hash = &br_sha512_vtable;
    JS_FreeCString(ctx, name);
    if (!hash) return JS_ThrowTypeError(ctx, "Unsupported digest algorithm");
    size_t length;
    uint8_t *data = JS_GetArrayBuffer(ctx, &length, argv[1]);
    if (!data && JS_HasException(ctx)) return JS_EXCEPTION;
    br_hash_compat_context context;
    uint8_t output[64];
    hash->init(&context.vtable);
    hash->update(&context.vtable, data, length);
    hash->out(&context.vtable, output);
    size_t output_length = (hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    return JS_NewArrayBufferCopy(ctx, output, output_length);
}

/* Keys and input are private bootstrap snapshots. The constant-time BearSSL
 * implementation does not index secret-dependent tables. CTR increments only
 * the requested low bits, including counter lengths not divisible by eight. */
static JSValue crypto_aes(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t mode, decrypt, bits;
    size_t key_size=0, iv_size=0, size=0;
    if (argc < 6) return JS_ThrowTypeError(ctx,"AES operation requires parameters");
    if (JS_ToInt32(ctx, &mode, argv[0]) < 0 ||
        JS_ToInt32(ctx, &decrypt, argv[1]) < 0 || JS_ToInt32(ctx, &bits, argv[5]) < 0)
        return JS_EXCEPTION;
    uint8_t *key = JS_GetArrayBuffer(ctx, &key_size, argv[2]);
    uint8_t *iv = JS_GetArrayBuffer(ctx, &iv_size, argv[3]);
    uint8_t *input = JS_GetArrayBuffer(ctx, &size, argv[4]);
    if ((!key || !iv || (!input && size)) && JS_HasException(ctx)) return JS_EXCEPTION;
    if ((key_size != 16 && key_size != 24 && key_size != 32) ||
        (mode!=3&&iv_size!=16) || (mode != 1 && mode != 2 && mode != 3) ||
        (mode == 2 && (bits < 1 || bits > 128)))
        return JS_ThrowTypeError(ctx, "Invalid AES parameters");
    if(mode==3){
        size_t aad_size=0;uint8_t *aad=argc>6?JS_GetArrayBuffer(ctx,&aad_size,argv[6]):NULL;
        if(argc<7)return JS_ThrowTypeError(ctx,"GCM additional data is required");
        if(!aad&&JS_HasException(ctx))return JS_EXCEPTION;
        if(!iv_size||(bits!=32&&bits!=64&&bits!=96&&bits!=104&&bits!=112&&bits!=120&&bits!=128)||
           (uint64_t)iv_size>UINT64_MAX/8||(uint64_t)aad_size>UINT64_MAX/8)
            return JS_ThrowTypeError(ctx,"Invalid GCM parameters");
        size_t tag_size=(size_t)bits/8;
        if(decrypt&&size<tag_size)return JS_ThrowTypeError(ctx,"GCM ciphertext is shorter than its tag");
        size_t plain_size=decrypt?size-tag_size:size;
        if((uint64_t)plain_size>UINT64_C(68719476704)||(!decrypt&&size>SIZE_MAX-tag_size))
            return JS_ThrowTypeError(ctx,"GCM input exceeds the block counter limit");
        size_t output_size=decrypt?plain_size:size+tag_size;
        uint8_t *output=js_malloc(ctx,output_size?output_size:1);if(!output)return JS_EXCEPTION;
        if(plain_size)memcpy(output,input,plain_size);
        br_aes_ct64_ctr_keys aes;br_aes_ct64_ctr_init(&aes,key,key_size);
        br_gcm_context gcm;br_gcm_init(&gcm,&aes.vtable,br_ghash_ctmul64);
        br_gcm_reset(&gcm,iv,iv_size);br_gcm_aad_inject(&gcm,aad,aad_size);br_gcm_flip(&gcm);
        br_gcm_run(&gcm,!decrypt,output,plain_size);
        bool authenticated=!decrypt||br_gcm_check_tag_trunc(&gcm,input+plain_size,tag_size);
        if(!decrypt)br_gcm_get_tag_trunc(&gcm,output+plain_size,tag_size);
        crypto_wipe(&gcm,sizeof gcm);crypto_wipe(&aes,sizeof aes);
        JSValue result=authenticated?JS_NewArrayBufferCopy(ctx,output,output_size):
            JS_ThrowTypeError(ctx,"GCM authentication failed");
        crypto_wipe(output,output_size);js_free(ctx,output);return result;
    }
    size_t out_size = size;
    if (mode == 1) {
        if (decrypt && (!size || size % 16)) return JS_ThrowTypeError(ctx, "Invalid CBC ciphertext length");
        if (!decrypt) {
            size_t pad = 16 - size % 16;
            if (size > SIZE_MAX - pad) return JS_ThrowOutOfMemory(ctx);
            out_size += pad;
        }
    } else {
        size_t blocks = size / 16 + !!(size % 16);
        if (bits < (int)(sizeof(size_t) * 8) && blocks > ((size_t)1 << bits))
            return JS_ThrowTypeError(ctx, "AES counter would repeat");
    }
    uint8_t *out = js_malloc(ctx, out_size ? out_size : 1);
    if (!out) return JS_EXCEPTION;
    if (size) memcpy(out, input, size);
    uint8_t counter[16]; memcpy(counter, iv, sizeof counter);
    if (mode == 1 && decrypt) {
        br_aes_ct64_cbcdec_keys aes; br_aes_ct64_cbcdec_init(&aes, key, key_size);
        br_aes_ct64_cbcdec_run(&aes, counter, out, out_size);
        crypto_wipe(&aes, sizeof aes);
        unsigned pad = out[out_size - 1], invalid = !pad || pad > 16;
        for (unsigned i = 0; i < 16; ++i)
            invalid |= (i < pad) & (out[out_size - 1 - i] != pad);
        if (invalid) { crypto_wipe(out, out_size); js_free(ctx, out); return JS_ThrowTypeError(ctx, "Invalid CBC padding"); }
        out_size -= pad;
    } else if (mode == 1) {
        memset(out + size, (int)(out_size - size), out_size - size);
        br_aes_ct64_cbcenc_keys aes; br_aes_ct64_cbcenc_init(&aes, key, key_size);
        br_aes_ct64_cbcenc_run(&aes, counter, out, out_size);
        crypto_wipe(&aes, sizeof aes);
    } else {
        br_aes_ct64_ctr_keys aes; br_aes_ct64_ctr_init(&aes, key, key_size);
        for (size_t offset = 0; offset < size;) {
            uint32_t low = ((uint32_t)counter[12] << 24) | ((uint32_t)counter[13] << 16) |
                           ((uint32_t)counter[14] << 8) | counter[15];
            /* The BearSSL primitive batches four blocks. Stop only at the
             * requested counter field's boundary (or its low-32 carry), not
             * on every 16-byte block. Nonce bits are never incremented. */
            uint64_t available=bits>=32?(uint64_t)UINT32_MAX-low+1:
                ((uint64_t)1<<bits)-(low&(((uint32_t)1<<bits)-1));
            size_t count=size-offset;if((uint64_t)count>available*16)count=(size_t)(available*16);
            br_aes_ct64_ctr_run(&aes, counter, low, out + offset, count);
            crypto_counter_add(counter,bits,(uint64_t)(count/16+!!(count%16)));
            offset+=count;
        }
        crypto_wipe(&aes, sizeof aes);
    }
    JSValue result = JS_NewArrayBufferCopy(ctx, out, out_size);
    crypto_wipe(out, mode == 1 && !decrypt ? size + 16 - size % 16 : size);
    js_free(ctx, out);
    return result;
}

void web_js_crypto_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("cryptoRandom", 1, crypto_random),
        JS_CFUNC_DEF("cryptoDigest", 2, crypto_digest),
        JS_CFUNC_DEF("cryptoAES", 7, crypto_aes),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}
