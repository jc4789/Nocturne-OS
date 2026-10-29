#include "js_crypto.h"
#include <bearssl_hash.h>
#include <bearssl_hmac.h>
#include <bearssl_kdf.h>
#include <bearssl_block.h>
#include <bearssl_aead.h>
#include <bearssl_rand.h>
#include <bearssl_rsa.h>
#include <bearssl_ec.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <nocturne.h>
#include "js_atomics.h"

/* The native primitives of the Web Cryptography API (js_crypto.js owns every
 * specification step: algorithm normalization, key formats, usages and the
 * error types). Inputs are private bootstrap snapshots (ArrayBuffers that no
 * author code can reach); every secret intermediate is wiped. The primitives
 * are BearSSL's constant-time implementations, except Ed25519, which BearSSL
 * does not have (the section below, RFC 8032). Errors thrown here are
 * internal: the JavaScript half maps them to the operation's DOMException. */

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

/* An ArrayBuffer argument; a zero-length buffer may have no data pointer. */
static bool crypto_bytes(JSContext *ctx, JSValueConst value, const uint8_t **bytes, size_t *size) {
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, value);
    if (!p && JS_HasException(ctx)) return false;
    static const uint8_t empty[1];
    *bytes = p ? p : empty; *size = p ? n : 0;
    return true;
}
/* A copy of a secret result; the scratch copy is wiped by the caller. */
static JSValue crypto_result(JSContext *ctx, const void *bytes, size_t size) {
    return JS_NewArrayBufferCopy(ctx, bytes, size);
}
/* An array of fresh values; each item's reference passes to the array (or is freed). */
static JSValue crypto_array(JSContext *ctx, JSValue *items, unsigned count) {
    JSValue array = JS_NewArray(ctx);
    for (unsigned i = 0; i < count; i++) {
        if (JS_IsException(array) || JS_IsException(items[i]) || JS_SetPropertyUint32(ctx, array, i, items[i]) < 0) {
            for (unsigned k = i + (JS_IsException(array) ? 0 : 1); k < count; k++) JS_FreeValue(ctx, items[k]);
            JS_FreeValue(ctx, array); return JS_EXCEPTION;
        }
    }
    return array;
}
static const br_hash_class *crypto_hash(JSContext *ctx, JSValueConst name) {
    const char *s = JS_ToCString(ctx, name);
    if (!s) return NULL;
    const br_hash_class *hash = !strcmp(s, "SHA-1") ? &br_sha1_vtable :
        !strcmp(s, "SHA-256") ? &br_sha256_vtable : !strcmp(s, "SHA-384") ? &br_sha384_vtable :
        !strcmp(s, "SHA-512") ? &br_sha512_vtable : NULL;
    JS_FreeCString(ctx, s);
    if (!hash) JS_ThrowTypeError(ctx, "Unsupported hash algorithm");
    return hash;
}
static size_t crypto_hash_size(const br_hash_class *hash) {
    return (hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
}
static void crypto_hash_bytes(const br_hash_class *hash, const void *data, size_t size, uint8_t out[64]) {
    br_hash_compat_context context;
    hash->init(&context.vtable);
    hash->update(&context.vtable, data, size);
    hash->out(&context.vtable, out);
    crypto_wipe(&context, sizeof context);
}
/* A deterministic generator seeded from the system entropy pool, for the
 * BearSSL primitives that take one (key generation, PSS salts, OAEP seeds). */
static bool crypto_prng(JSContext *ctx, br_hmac_drbg_context *rng) {
    uint8_t seed[48];
    if (n_random(seed, sizeof seed) != (n_result)sizeof seed) {
        crypto_wipe(seed, sizeof seed);
        JS_ThrowInternalError(ctx, "System random source could not fill the request");
        return false;
    }
    br_hmac_drbg_init(rng, &br_sha256_vtable, seed, sizeof seed);
    crypto_wipe(seed, sizeof seed);
    return true;
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
    if (n_random(bytes, (size_t)length) != (n_result)length) {
        memset(bytes, 0, (size_t)length);
        js_free(ctx, bytes);
        return JS_ThrowInternalError(ctx, "System random source could not fill the request");
    }
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
    const br_hash_class *hash = crypto_hash(ctx, argv[0]);
    if (!hash) return JS_EXCEPTION;
    const uint8_t *data; size_t length;
    if (!crypto_bytes(ctx, argv[1], &data, &length)) return JS_EXCEPTION;
    uint8_t output[64];
    crypto_hash_bytes(hash, data, length, output);
    return JS_NewArrayBufferCopy(ctx, output, crypto_hash_size(hash));
}

/* AES-KW (RFC 3394 §2.2.1 and §2.2.2, the default initial value). Each step
 * is one block: CBC over a single block with a zero IV is the cipher itself. */
static const uint8_t kw_iv[8] = {0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
static JSValue crypto_aes_kw(JSContext *ctx, const uint8_t *key, size_t key_size,
                             const uint8_t *input, size_t size, bool unwrap) {
    if (size % 8 || size < (unwrap ? 24u : 16u)) return JS_ThrowTypeError(ctx, "Invalid AES-KW data length");
    size_t n = size / 8 - (unwrap ? 1 : 0);
    size_t out_size = unwrap ? size - 8 : size + 8;
    uint8_t *out = js_malloc(ctx, out_size + 8);
    if (!out) return JS_EXCEPTION;
    uint8_t a[8], block[16], iv[16];
    /* out holds A || R[1..n] while running. */
    if (!unwrap) { memcpy(out, kw_iv, 8); memcpy(out + 8, input, size); }
    else memcpy(out, input, size);
    memcpy(a, out, 8);
    if (!unwrap) {
        br_aes_ct64_cbcenc_keys aes; br_aes_ct64_cbcenc_init(&aes, key, key_size);
        for (unsigned j = 0; j < 6; j++)
            for (size_t i = 1; i <= n; i++) {
                memcpy(block, a, 8); memcpy(block + 8, out + 8 * i, 8);
                memset(iv, 0, sizeof iv); br_aes_ct64_cbcenc_run(&aes, iv, block, 16);
                uint64_t t = (uint64_t)n * j + i;
                for (int k = 7; k >= 0; k--) { block[k] ^= (uint8_t)t; t >>= 8; }
                memcpy(a, block, 8); memcpy(out + 8 * i, block + 8, 8);
            }
        crypto_wipe(&aes, sizeof aes);
        memcpy(out, a, 8);
        JSValue result = crypto_result(ctx, out, out_size);
        crypto_wipe(out, out_size + 8); crypto_wipe(block, sizeof block); crypto_wipe(a, sizeof a);
        js_free(ctx, out); return result;
    }
    br_aes_ct64_cbcdec_keys aes; br_aes_ct64_cbcdec_init(&aes, key, key_size);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--) {
            uint64_t t = (uint64_t)n * (unsigned)j + i;
            memcpy(block, a, 8);
            for (int k = 7; k >= 0; k--) { block[k] ^= (uint8_t)t; t >>= 8; }
            memcpy(block + 8, out + 8 * i, 8);
            memset(iv, 0, sizeof iv); br_aes_ct64_cbcdec_run(&aes, iv, block, 16);
            memcpy(a, block, 8); memcpy(out + 8 * i, block + 8, 8);
        }
    crypto_wipe(&aes, sizeof aes);
    unsigned diff = 0;
    for (int k = 0; k < 8; k++) diff |= a[k] ^ kw_iv[k];
    JSValue result = diff ? JS_ThrowTypeError(ctx, "AES-KW integrity check failed") :
        crypto_result(ctx, out + 8, out_size);
    crypto_wipe(out, out_size + 8); crypto_wipe(block, sizeof block); crypto_wipe(a, sizeof a);
    js_free(ctx, out); return result;
}

/* Keys and input are private bootstrap snapshots. The constant-time BearSSL
 * implementation does not index secret-dependent tables. CTR increments only
 * the requested low bits, including counter lengths not divisible by eight.
 * Modes: 1 CBC, 2 CTR, 3 GCM, 4 KW (decrypt is unwrap). */
static JSValue crypto_aes(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t mode, decrypt, bits;
    size_t key_size=0, iv_size=0, size=0;
    if (argc < 6) return JS_ThrowTypeError(ctx,"AES operation requires parameters");
    if (JS_ToInt32(ctx, &mode, argv[0]) < 0 ||
        JS_ToInt32(ctx, &decrypt, argv[1]) < 0 || JS_ToInt32(ctx, &bits, argv[5]) < 0)
        return JS_EXCEPTION;
    const uint8_t *key, *iv, *input;
    if (!crypto_bytes(ctx, argv[2], &key, &key_size) || !crypto_bytes(ctx, argv[3], &iv, &iv_size) ||
        !crypto_bytes(ctx, argv[4], &input, &size)) return JS_EXCEPTION;
    if ((key_size != 16 && key_size != 24 && key_size != 32) ||
        (mode!=3&&mode!=4&&iv_size!=16) || (mode < 1 || mode > 4) ||
        (mode == 2 && (bits < 1 || bits > 128)))
        return JS_ThrowTypeError(ctx, "Invalid AES parameters");
    if(mode==4)return crypto_aes_kw(ctx,key,key_size,input,size,decrypt!=0);
    if(mode==3){
        if(argc<7)return JS_ThrowTypeError(ctx,"GCM additional data is required");
        size_t aad_size=0;const uint8_t *aad;
        if(!crypto_bytes(ctx,argv[6],&aad,&aad_size))return JS_EXCEPTION;
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

/* HMAC (RFC 2104) over the named hash: cryptoHMAC(hash, key, data). */
static JSValue crypto_hmac(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 3) return JS_ThrowTypeError(ctx, "HMAC requires hash, key and data");
    const br_hash_class *hash = crypto_hash(ctx, argv[0]);
    const uint8_t *key, *data; size_t key_size, size;
    if (!hash || !crypto_bytes(ctx, argv[1], &key, &key_size) || !crypto_bytes(ctx, argv[2], &data, &size))
        return JS_EXCEPTION;
    br_hmac_key_context kc; br_hmac_context hc; uint8_t mac[64];
    br_hmac_key_init(&kc, hash, key, key_size);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, data, size);
    size_t length = br_hmac_out(&hc, mac);
    JSValue result = crypto_result(ctx, mac, length);
    crypto_wipe(&kc, sizeof kc); crypto_wipe(&hc, sizeof hc); crypto_wipe(mac, sizeof mac);
    return result;
}
/* count iterations of PBKDF2's F (RFC 8018 §5.2): U = PRF(P, U), T ^= U.
 * cryptoPBKDF2Step(hash, password, u, t, count) returns [u, t]. The script
 * calls it in bounded rounds, so a long derivation passes through the
 * realm's interrupt checks between them. */
static JSValue crypto_pbkdf2_step(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint32_t count;
    if (argc < 5) return JS_ThrowTypeError(ctx, "PBKDF2 requires its state");
    const br_hash_class *hash = crypto_hash(ctx, argv[0]);
    const uint8_t *password, *u_in, *t_in; size_t password_size, u_size, t_size;
    if (!hash || !crypto_bytes(ctx, argv[1], &password, &password_size) ||
        !crypto_bytes(ctx, argv[2], &u_in, &u_size) || !crypto_bytes(ctx, argv[3], &t_in, &t_size) ||
        JS_ToUint32(ctx, &count, argv[4]) < 0) return JS_EXCEPTION;
    size_t length = crypto_hash_size(hash);
    if (u_size != length || t_size != length) return JS_ThrowTypeError(ctx, "Invalid PBKDF2 state");
    uint8_t u[64], t[64];
    memcpy(u, u_in, length); memcpy(t, t_in, length);
    br_hmac_key_context kc; br_hmac_context hc;
    br_hmac_key_init(&kc, hash, password, password_size);
    for (uint32_t i = 0; i < count; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, length);
        br_hmac_out(&hc, u);
        for (size_t k = 0; k < length; k++) t[k] ^= u[k];
    }
    JSValue items[2] = {crypto_result(ctx, u, length), crypto_result(ctx, t, length)};
    crypto_wipe(&kc, sizeof kc); crypto_wipe(&hc, sizeof hc); crypto_wipe(u, sizeof u); crypto_wipe(t, sizeof t);
    return crypto_array(ctx, items, 2);
}
/* HKDF (RFC 5869): cryptoHKDF(hash, ikm, salt, info, bytes). */
static JSValue crypto_hkdf(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint32_t length;
    if (argc < 5) return JS_ThrowTypeError(ctx, "HKDF requires its parameters");
    const br_hash_class *hash = crypto_hash(ctx, argv[0]);
    const uint8_t *ikm, *salt, *info; size_t ikm_size, salt_size, info_size;
    if (!hash || !crypto_bytes(ctx, argv[1], &ikm, &ikm_size) || !crypto_bytes(ctx, argv[2], &salt, &salt_size) ||
        !crypto_bytes(ctx, argv[3], &info, &info_size) || JS_ToUint32(ctx, &length, argv[4]) < 0)
        return JS_EXCEPTION;
    if (length > 255 * crypto_hash_size(hash)) return JS_ThrowRangeError(ctx, "HKDF output is too long");
    uint8_t *out = js_malloc(ctx, length ? length : 1);
    if (!out) return JS_EXCEPTION;
    br_hkdf_context hc;
    br_hkdf_init(&hc, hash, salt, salt_size);
    br_hkdf_inject(&hc, ikm, ikm_size);
    br_hkdf_flip(&hc);
    br_hkdf_produce(&hc, info, info_size, out, length);
    JSValue result = crypto_result(ctx, out, length);
    crypto_wipe(&hc, sizeof hc); crypto_wipe(out, length); js_free(ctx, out);
    return result;
}

/* RSA. Keys travel as unsigned big-endian integers: n, e, d, p, q, dp, dq, qi. */
static uint32_t crypto_bit_length(const uint8_t *p, size_t n) {
    while (n && !*p) { p++; n--; }
    if (!n) return 0;
    uint32_t bits = (uint32_t)(n - 1) * 8;
    for (unsigned v = *p; v; v >>= 1) bits++;
    return bits;
}
static const uint8_t *crypto_trim(const uint8_t *p, size_t *n) {
    while (*n && !*p) { p++; (*n)--; }
    return p;
}
static const unsigned char *crypto_hash_oid(const br_hash_class *hash) {
    return hash == &br_sha1_vtable ? (const unsigned char *)BR_HASH_OID_SHA1 :
        hash == &br_sha256_vtable ? (const unsigned char *)BR_HASH_OID_SHA256 :
        hash == &br_sha384_vtable ? (const unsigned char *)BR_HASH_OID_SHA384 : (const unsigned char *)BR_HASH_OID_SHA512;
}
/* argv: n, p, q, dp, dq, qi */
static bool crypto_rsa_private_key(JSContext *ctx, JSValueConst *argv, br_rsa_private_key *sk, size_t *modulus) {
    const uint8_t *n, *p, *q, *dp, *dq, *qi; size_t nl, pl, ql, dpl, dql, qil;
    if (!crypto_bytes(ctx, argv[0], &n, &nl) || !crypto_bytes(ctx, argv[1], &p, &pl) ||
        !crypto_bytes(ctx, argv[2], &q, &ql) || !crypto_bytes(ctx, argv[3], &dp, &dpl) ||
        !crypto_bytes(ctx, argv[4], &dq, &dql) || !crypto_bytes(ctx, argv[5], &qi, &qil)) return false;
    n = crypto_trim(n, &nl);
    sk->n_bitlen = crypto_bit_length(n, nl);
    sk->p = (unsigned char *)crypto_trim(p, &pl); sk->plen = pl;
    sk->q = (unsigned char *)crypto_trim(q, &ql); sk->qlen = ql;
    sk->dp = (unsigned char *)crypto_trim(dp, &dpl); sk->dplen = dpl;
    sk->dq = (unsigned char *)crypto_trim(dq, &dql); sk->dqlen = dql;
    sk->iq = (unsigned char *)crypto_trim(qi, &qil); sk->iqlen = qil;
    *modulus = (sk->n_bitlen + 7) / 8;
    if (!sk->n_bitlen || sk->n_bitlen > 4096 || !pl || !ql) { JS_ThrowTypeError(ctx, "Unsupported RSA private key"); return false; }
    return true;
}
static bool crypto_rsa_public_key(JSContext *ctx, JSValueConst n_value, JSValueConst e_value, br_rsa_public_key *pk) {
    const uint8_t *n, *e; size_t nl, el;
    if (!crypto_bytes(ctx, n_value, &n, &nl) || !crypto_bytes(ctx, e_value, &e, &el)) return false;
    n = crypto_trim(n, &nl); e = crypto_trim(e, &el);
    pk->n = (unsigned char *)n; pk->nlen = nl; pk->e = (unsigned char *)e; pk->elen = el;
    if (!nl || !el || crypto_bit_length(n, nl) > 4096) { JS_ThrowTypeError(ctx, "Unsupported RSA public key"); return false; }
    return true;
}
/* ---- RSA key generation, in parallel ----
 * A 4096-bit key takes seconds to make, so the generate key operation runs on a thread of its
 * own: the specification's "in parallel" (generateKey step 8), off the realm's thread.
 *   cryptoRSAGenerateStart(bits, publicExponent) checks the parameters and starts the job.
 *   cryptoJobWait(job, callback) has the realm call callback in a task once the thread is
 *     through (web_atomics_watch_word: the native watch, event-loop wake-up and task of
 *     Atomics.waitAsync, the same in the Window and Worker loops); false when it already is.
 *   cryptoRSAGenerateResult(job) then returns [n, e, d, p, q, dp, dq, qi], once.
 * When no thread can start, Start makes the key at once. owner: 0 both, 1 the thread is
 * through, 2 the realm let go (the thread then frees the job). */
struct rsa_job {
    volatile uint32_t done;   /* 1 once the key is made: the word the realm watches */
    uint32_t owner;
    bool threaded, taken;
    unsigned bits;
    uint32_t e, ok;
    size_t priv_size, pub_size, d_size, d_length;
    uint8_t *buffer;          /* the private key, the public key and d, priv_size + pub_size + d_size */
    br_rsa_private_key sk;
    br_rsa_public_key pk;
    br_hmac_drbg_context rng;
};
static JSClassID rsa_job_class;

static void rsa_job_free(struct rsa_job *j) {
    if (j->buffer) {
        crypto_wipe(j->buffer, j->priv_size + j->pub_size + j->d_size);
        free(j->buffer);
    }
    crypto_wipe(j, sizeof *j);
    free(j);
}
static void rsa_job_work(struct rsa_job *j) {
    j->ok = br_rsa_keygen_get_default()(&j->rng.vtable, &j->sk, j->buffer, &j->pk, j->buffer + j->priv_size, j->bits, j->e);
    j->d_length = j->ok ? br_rsa_compute_privexp_get_default()(j->buffer + j->priv_size + j->pub_size, &j->sk, j->e) : 0;
    crypto_wipe(&j->rng, sizeof j->rng);
}
static int rsa_job_thread(void *arg) {
    struct rsa_job *j = arg;
    rsa_job_work(j);
    /* wake first: the realm takes the key, and may free the job, only once owner says so */
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    wake_address(&j->done, UINT32_MAX);
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&j->owner, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        rsa_job_free(j); /* the realm went; nobody waits for it */
    return 0;
}
/* The realm lets go of the job (its watch was withdrawn first: the waiter holds the callback
   that holds the job); a thread still at work frees it when it is through. */
static void rsa_job_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    struct rsa_job *j = JS_GetOpaque(val, rsa_job_class);
    if (!j) return;
    uint32_t expected = 0;
    if (!j->threaded || !__atomic_compare_exchange_n(&j->owner, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        rsa_job_free(j);
}
static JSValue crypto_rsa_generate_start(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint32_t bits;
    const uint8_t *exponent; size_t exponent_size;
    if (argc < 2 || JS_ToUint32(ctx, &bits, argv[0]) < 0 || !crypto_bytes(ctx, argv[1], &exponent, &exponent_size))
        return JS_EXCEPTION;
    exponent = crypto_trim(exponent, &exponent_size);
    uint32_t e = 0;
    if (exponent_size > 4) return JS_ThrowRangeError(ctx, "RSA public exponent is too large");
    for (size_t i = 0; i < exponent_size; i++) e = (e << 8) | exponent[i];
    if (e < 3 || !(e & 1) || bits < 512 || bits > 4096) return JS_ThrowRangeError(ctx, "Unsupported RSA key generation parameters");
    struct rsa_job *j = calloc(1, sizeof *j);
    if (!j) return JS_ThrowOutOfMemory(ctx);
    j->bits = bits; j->e = e;
    j->priv_size = BR_RSA_KBUF_PRIV_SIZE(bits); j->pub_size = BR_RSA_KBUF_PUB_SIZE(bits); j->d_size = (bits + 7) / 8;
    j->buffer = malloc(j->priv_size + j->pub_size + j->d_size);
    if (!j->buffer) { rsa_job_free(j); return JS_ThrowOutOfMemory(ctx); }
    if (!crypto_prng(ctx, &j->rng)) { rsa_job_free(j); return JS_EXCEPTION; }
    JSValue job = JS_NewObjectClass(ctx, (int)rsa_job_class);
    if (JS_IsException(job)) { rsa_job_free(j); return job; }
    JS_SetOpaque(job, j);
    thrd_t thread;
    j->threaded = thrd_create(&thread, rsa_job_thread, j) == thrd_success;
    if (j->threaded) thrd_detach(thread);
    else { rsa_job_work(j); j->done = 1; }
    return job;
}
static JSValue crypto_job_wait(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    struct rsa_job *j = argc >= 2 ? JS_GetOpaque2(ctx, argv[0], rsa_job_class) : NULL;
    if (!j) return argc >= 2 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "cryptoJobWait requires a job and a callback");
    if (!JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "cryptoJobWait requires a callback");
    if (__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) return JS_FALSE;
    int watched = web_atomics_watch_word(ctx, &j->done, 0, argv[1]);
    return watched < 0 ? JS_EXCEPTION : JS_NewBool(ctx, watched == 0);
}
static JSValue crypto_rsa_generate_result(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    struct rsa_job *j = argc >= 1 ? JS_GetOpaque2(ctx, argv[0], rsa_job_class) : NULL;
    if (!j) return argc >= 1 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "cryptoRSAGenerateResult requires a job");
    if (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE) || j->taken) return JS_ThrowTypeError(ctx, "No RSA key to take");
    j->taken = true;
    JSValue result = JS_EXCEPTION;
    if (!j->ok || !j->d_length) JS_ThrowRangeError(ctx, "RSA key generation failed");
    else {
        JSValue items[8] = {
            crypto_result(ctx, j->pk.n, j->pk.nlen), crypto_result(ctx, j->pk.e, j->pk.elen),
            crypto_result(ctx, j->buffer + j->priv_size + j->pub_size, j->d_length),
            crypto_result(ctx, j->sk.p, j->sk.plen), crypto_result(ctx, j->sk.q, j->sk.qlen),
            crypto_result(ctx, j->sk.dp, j->sk.dplen), crypto_result(ctx, j->sk.dq, j->sk.dqlen),
            crypto_result(ctx, j->sk.iq, j->sk.iqlen)};
        result = crypto_array(ctx, items, 8);
    }
    /* the key now lives in the realm's buffers only */
    crypto_wipe(j->buffer, j->priv_size + j->pub_size + j->d_size);
    return result;
}
/* cryptoRSAPrivate(op, hash, param, data, n, p, q, dp, dq, qi):
 * 0 RSASSA-PKCS1-v1_5 sign, 1 RSASSA-PSS sign (param: salt length),
 * 2 RSAES-OAEP decrypt (param: label). */
static JSValue crypto_rsa_private(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op;
    if (argc < 10 || JS_ToInt32(ctx, &op, argv[0]) < 0) return JS_EXCEPTION;
    const br_hash_class *hash = crypto_hash(ctx, argv[1]);
    const uint8_t *data; size_t size;
    br_rsa_private_key sk; size_t modulus;
    if (!hash || !crypto_bytes(ctx, argv[3], &data, &size) || !crypto_rsa_private_key(ctx, argv + 4, &sk, &modulus))
        return JS_EXCEPTION;
    uint8_t *out = js_malloc(ctx, modulus);
    if (!out) return JS_EXCEPTION;
    uint8_t digest[64]; size_t digest_size = crypto_hash_size(hash);
    uint32_t ok = 0; size_t out_size = modulus;
    if (op == 0 || op == 1) {
        crypto_hash_bytes(hash, data, size, digest);
        if (op == 0) ok = br_rsa_pkcs1_sign_get_default()(crypto_hash_oid(hash), digest, digest_size, &sk, out);
        else {
            uint32_t salt;
            br_hmac_drbg_context rng;
            if (JS_ToUint32(ctx, &salt, argv[2]) < 0 || !crypto_prng(ctx, &rng)) { js_free(ctx, out); return JS_EXCEPTION; }
            ok = br_rsa_pss_sign_get_default()(&rng.vtable, hash, hash, digest, salt, &sk, out);
            crypto_wipe(&rng, sizeof rng);
        }
    } else if (op == 2) {
        const uint8_t *label; size_t label_size;
        if (!crypto_bytes(ctx, argv[2], &label, &label_size)) { js_free(ctx, out); return JS_EXCEPTION; }
        if (size == modulus) {
            memcpy(out, data, size);
            ok = br_rsa_oaep_decrypt_get_default()(hash, label, label_size, &sk, out, &out_size);
        }
    }
    JSValue result = ok ? crypto_result(ctx, out, out_size) : JS_ThrowRangeError(ctx, "RSA private key operation failed");
    crypto_wipe(out, modulus); crypto_wipe(digest, sizeof digest); js_free(ctx, out);
    return result;
}
/* cryptoRSAPublic(op, hash, param, data, signature, n, e):
 * 0 RSASSA-PKCS1-v1_5 verify, 1 RSASSA-PSS verify (param: salt length),
 * 2 RSAES-OAEP encrypt (param: label). */
static JSValue crypto_rsa_public(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op;
    if (argc < 7 || JS_ToInt32(ctx, &op, argv[0]) < 0) return JS_EXCEPTION;
    const br_hash_class *hash = crypto_hash(ctx, argv[1]);
    const uint8_t *data, *signature; size_t size, signature_size;
    br_rsa_public_key pk;
    if (!hash || !crypto_bytes(ctx, argv[3], &data, &size) || !crypto_bytes(ctx, argv[4], &signature, &signature_size) ||
        !crypto_rsa_public_key(ctx, argv[5], argv[6], &pk)) return JS_EXCEPTION;
    size_t modulus = (crypto_bit_length(pk.n, pk.nlen) + 7) / 8;
    uint8_t digest[64], recovered[64]; size_t digest_size = crypto_hash_size(hash);
    if (op == 0 || op == 1) {
        if (signature_size != modulus) return JS_FALSE;
        crypto_hash_bytes(hash, data, size, digest);
        if (op == 0) {
            if (!br_rsa_pkcs1_vrfy_get_default()(signature, signature_size, crypto_hash_oid(hash), digest_size, &pk, recovered))
                return JS_FALSE;
            unsigned diff = 0;
            for (size_t i = 0; i < digest_size; i++) diff |= digest[i] ^ recovered[i];
            return JS_NewBool(ctx, !diff);
        }
        uint32_t salt;
        if (JS_ToUint32(ctx, &salt, argv[2]) < 0) return JS_EXCEPTION;
        return JS_NewBool(ctx, br_rsa_pss_vrfy_get_default()(signature, signature_size, hash, hash, digest, salt, &pk) != 0);
    }
    const uint8_t *label; size_t label_size;
    if (op != 2 || !crypto_bytes(ctx, argv[2], &label, &label_size)) return op != 2 ? JS_ThrowTypeError(ctx, "Unknown RSA operation") : JS_EXCEPTION;
    uint8_t *out = js_malloc(ctx, modulus);
    if (!out) return JS_EXCEPTION;
    br_hmac_drbg_context rng;
    if (!crypto_prng(ctx, &rng)) { js_free(ctx, out); return JS_EXCEPTION; }
    size_t length = br_rsa_oaep_encrypt_get_default()(&rng.vtable, hash, label, label_size, &pk, out, modulus, data, size);
    crypto_wipe(&rng, sizeof rng);
    JSValue result = length ? crypto_result(ctx, out, length) : JS_ThrowRangeError(ctx, "RSA-OAEP message is too long");
    js_free(ctx, out);
    return result;
}

/* NIST curves: 23 P-256, 24 P-384, 25 P-521 (BearSSL's TLS identifiers).
 * Points are uncompressed (SEC 1 §2.3.3); private scalars are big-endian. */
static bool crypto_ec_curve(JSContext *ctx, JSValueConst value, int *curve) {
    int32_t id;
    if (JS_ToInt32(ctx, &id, value) < 0) return false;
    if (id != BR_EC_secp256r1 && id != BR_EC_secp384r1 && id != BR_EC_secp521r1) {
        JS_ThrowTypeError(ctx, "Unsupported curve"); return false;
    }
    *curve = id; return true;
}
/* 0 < d < n, in constant time over the scalar's bytes. */
static bool crypto_ec_scalar_valid(int curve, const uint8_t *d, size_t size) {
    const br_ec_impl *impl = br_ec_get_default();
    size_t order_size; const unsigned char *order = impl->order(curve, &order_size);
    if (size != order_size) return false;
    unsigned nonzero = 0; int borrow = 0;
    for (size_t i = size; i-- > 0;) {
        nonzero |= d[i];
        int diff = (int)d[i] - (int)order[i] - borrow;
        borrow = diff < 0;
    }
    return nonzero && borrow;  /* d - n borrows: d < n */
}
static bool crypto_ec_point_valid(int curve, const uint8_t *q, size_t size) {
    const br_ec_impl *impl = br_ec_get_default();
    size_t length; impl->generator(curve, &length);
    if (size != length || q[0] != 4) return false;
    uint8_t copy[133], one = 1;
    memcpy(copy, q, size);
    return impl->mul(copy, size, &one, 1, curve) != 0;
}
/* cryptoECGenerate(curve) returns [d, Q]. */
static JSValue crypto_ec_generate(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int curve;
    if (argc < 1 || !crypto_ec_curve(ctx, argv[0], &curve)) return JS_EXCEPTION;
    const br_ec_impl *impl = br_ec_get_default();
    br_hmac_drbg_context rng;
    if (!crypto_prng(ctx, &rng)) return JS_EXCEPTION;
    uint8_t priv[BR_EC_KBUF_PRIV_MAX_SIZE], pub[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_private_key sk; br_ec_public_key pk;
    size_t length = br_ec_keygen(&rng.vtable, impl, &sk, priv, curve);
    size_t public_length = length ? br_ec_compute_pub(impl, &pk, pub, &sk) : 0;
    crypto_wipe(&rng, sizeof rng);
    JSValue result = JS_EXCEPTION;
    if (!public_length) JS_ThrowRangeError(ctx, "EC key generation failed");
    else {
        JSValue items[2] = {crypto_result(ctx, sk.x, sk.xlen), crypto_result(ctx, pk.q, pk.qlen)};
        result = crypto_array(ctx, items, 2);
    }
    crypto_wipe(priv, sizeof priv);
    return result;
}
/* cryptoECPublic(curve, d): Q, or null when d is not a scalar of the curve. */
static JSValue crypto_ec_public(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int curve; const uint8_t *d; size_t size;
    if (argc < 2 || !crypto_ec_curve(ctx, argv[0], &curve) || !crypto_bytes(ctx, argv[1], &d, &size)) return JS_EXCEPTION;
    if (!crypto_ec_scalar_valid(curve, d, size)) return JS_NULL;
    br_ec_private_key sk = {curve, (unsigned char *)d, size};
    br_ec_public_key pk; uint8_t pub[BR_EC_KBUF_PUB_MAX_SIZE];
    size_t length = br_ec_compute_pub(br_ec_get_default(), &pk, pub, &sk);
    return length ? crypto_result(ctx, pk.q, pk.qlen) : JS_NULL;
}
/* cryptoECValid(curve, Q): whether Q is an uncompressed point of the curve. */
static JSValue crypto_ec_valid(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int curve; const uint8_t *q; size_t size;
    if (argc < 2 || !crypto_ec_curve(ctx, argv[0], &curve) || !crypto_bytes(ctx, argv[1], &q, &size)) return JS_EXCEPTION;
    return JS_NewBool(ctx, crypto_ec_point_valid(curve, q, size));
}
/* cryptoECDSA(op, curve, hash, key, data, signature): 0 sign with d, returning
 * r || s (each the order's length); 1 verify with Q. */
static JSValue crypto_ecdsa(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op; int curve;
    if (argc < 6 || JS_ToInt32(ctx, &op, argv[0]) < 0 || !crypto_ec_curve(ctx, argv[1], &curve)) return JS_EXCEPTION;
    const br_hash_class *hash = crypto_hash(ctx, argv[2]);
    const uint8_t *key, *data, *signature; size_t key_size, size, signature_size;
    if (!hash || !crypto_bytes(ctx, argv[3], &key, &key_size) || !crypto_bytes(ctx, argv[4], &data, &size) ||
        !crypto_bytes(ctx, argv[5], &signature, &signature_size)) return JS_EXCEPTION;
    const br_ec_impl *impl = br_ec_get_default();
    uint8_t digest[64], out[132];
    crypto_hash_bytes(hash, data, size, digest);
    if (op == 0) {
        if (!crypto_ec_scalar_valid(curve, key, key_size)) return JS_ThrowRangeError(ctx, "Invalid EC private key");
        br_ec_private_key sk = {curve, (unsigned char *)key, key_size};
        size_t length = br_ecdsa_sign_raw_get_default()(impl, hash, digest, &sk, out);
        JSValue result = length ? crypto_result(ctx, out, length) : JS_ThrowRangeError(ctx, "ECDSA signing failed");
        crypto_wipe(out, sizeof out); crypto_wipe(digest, sizeof digest);
        return result;
    }
    if (!crypto_ec_point_valid(curve, key, key_size)) return JS_FALSE;
    br_ec_public_key pk = {curve, (unsigned char *)key, key_size};
    return JS_NewBool(ctx, br_ecdsa_vrfy_raw_get_default()(impl, digest, crypto_hash_size(hash), &pk, signature, signature_size) != 0);
}
/* cryptoECDH(curve, d, Q): the x-coordinate of dQ, or null when it fails. */
static JSValue crypto_ecdh(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int curve; const uint8_t *d, *q; size_t d_size, q_size;
    if (argc < 3 || !crypto_ec_curve(ctx, argv[0], &curve) || !crypto_bytes(ctx, argv[1], &d, &d_size) ||
        !crypto_bytes(ctx, argv[2], &q, &q_size)) return JS_EXCEPTION;
    if (!crypto_ec_scalar_valid(curve, d, d_size) || !crypto_ec_point_valid(curve, q, q_size)) return JS_NULL;
    const br_ec_impl *impl = br_ec_get_default();
    uint8_t point[133];
    memcpy(point, q, q_size);
    uint32_t ok = impl->mul(point, q_size, d, d_size, curve);
    size_t x_length, x_offset = impl->xoff(curve, &x_length);
    JSValue result = ok ? crypto_result(ctx, point + x_offset, x_length) : JS_NULL;
    crypto_wipe(point, sizeof point);
    return result;
}
/* X25519 (RFC 7748 §5): cryptoX25519(k, u), both little-endian. BearSSL reads
 * the scalar big-endian and clamps it the same way. null for the all-zero
 * output (RFC 7748 §6.1), checked without a data-dependent branch per byte. */
static JSValue crypto_x25519(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const uint8_t *k, *u; size_t k_size, u_size;
    if (argc < 2 || !crypto_bytes(ctx, argv[0], &k, &k_size) || !crypto_bytes(ctx, argv[1], &u, &u_size)) return JS_EXCEPTION;
    if (k_size != 32 || u_size != 32) return JS_ThrowTypeError(ctx, "X25519 values are 32 bytes");
    uint8_t scalar[32], point[32];
    for (int i = 0; i < 32; i++) scalar[i] = k[31 - i];
    memcpy(point, u, 32);
    uint32_t ok = br_ec_c25519_m31.mul(point, 32, scalar, 32, BR_EC_curve25519);
    unsigned any = 0;
    for (int i = 0; i < 32; i++) any |= point[i];
    JSValue result = ok && any ? crypto_result(ctx, point, 32) : JS_NULL;
    crypto_wipe(scalar, sizeof scalar); crypto_wipe(point, sizeof point);
    return result;
}

/* Ed25519 (RFC 8032 §5.1). Field elements are sixteen signed 16-bit limbs
 * modulo 2^255-19; points use extended coordinates (X, Y, Z, T). Scalar
 * multiplication swaps in constant time; only public values (encodings,
 * verification results) are branched on. */
typedef int64_t ed_gf[16];
static const ed_gf ed_zero = {0}, ed_one = {1};
static const ed_gf ed_d = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const ed_gf ed_d2 = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const ed_gf ed_bx = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const ed_gf ed_by = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const ed_gf ed_sqrtm1 = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};
/* The group order L = 2^252 + 27742317777372353535851937790883648493, little-endian. */
static const int64_t ed_l[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                                 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
static void ed_set(ed_gf r, const ed_gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static void ed_carry(ed_gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1; else o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}
static void ed_select(ed_gf p, ed_gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void ed_pack(uint8_t o[32], const ed_gf n) {
    ed_gf m, t;
    ed_set(t, n);
    ed_carry(t); ed_carry(t); ed_carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        ed_select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static bool ed_equal(const ed_gf a, const ed_gf b) {
    uint8_t c[32], d[32]; unsigned diff = 0;
    ed_pack(c, a); ed_pack(d, b);
    for (int i = 0; i < 32; i++) diff |= c[i] ^ d[i];
    return !diff;
}
static int ed_parity(const ed_gf a) { uint8_t d[32]; ed_pack(d, a); return d[0] & 1; }
static void ed_unpack(ed_gf o, const uint8_t n[32]) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void ed_add_gf(ed_gf o, const ed_gf a, const ed_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void ed_sub_gf(ed_gf o, const ed_gf a, const ed_gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void ed_mul(ed_gf o, const ed_gf a, const ed_gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    ed_carry(o); ed_carry(o);
}
static void ed_square(ed_gf o, const ed_gf a) { ed_mul(o, a, a); }
static void ed_invert(ed_gf o, const ed_gf in) {
    ed_gf c; ed_set(c, in);
    for (int a = 253; a >= 0; a--) { ed_square(c, c); if (a != 2 && a != 4) ed_mul(c, c, in); }
    ed_set(o, c);
}
static void ed_pow2523(ed_gf o, const ed_gf in) {
    ed_gf c; ed_set(c, in);
    for (int a = 250; a >= 0; a--) { ed_square(c, c); if (a != 1) ed_mul(c, c, in); }
    ed_set(o, c);
}
static void ed_point_add(ed_gf p[4], ed_gf q[4]) {
    ed_gf a, b, c, d, t, e, f, g, h;
    ed_sub_gf(a, p[1], p[0]); ed_sub_gf(t, q[1], q[0]); ed_mul(a, a, t);
    ed_add_gf(b, p[0], p[1]); ed_add_gf(t, q[0], q[1]); ed_mul(b, b, t);
    ed_mul(c, p[3], q[3]); ed_mul(c, c, ed_d2);
    ed_mul(d, p[2], q[2]); ed_add_gf(d, d, d);
    ed_sub_gf(e, b, a); ed_sub_gf(f, d, c); ed_add_gf(g, d, c); ed_add_gf(h, b, a);
    ed_mul(p[0], e, f); ed_mul(p[1], h, g); ed_mul(p[2], g, f); ed_mul(p[3], e, h);
}
static void ed_point_swap(ed_gf p[4], ed_gf q[4], int b) { for (int i = 0; i < 4; i++) ed_select(p[i], q[i], b); }
static void ed_point_pack(uint8_t r[32], ed_gf p[4]) {
    ed_gf tx, ty, zi;
    ed_invert(zi, p[2]); ed_mul(tx, p[0], zi); ed_mul(ty, p[1], zi);
    ed_pack(r, ty); r[31] ^= (uint8_t)(ed_parity(tx) << 7);
}
static void ed_scalarmult(ed_gf p[4], ed_gf q[4], const uint8_t s[32]) {
    ed_set(p[0], ed_zero); ed_set(p[1], ed_one); ed_set(p[2], ed_one); ed_set(p[3], ed_zero);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        ed_point_swap(p, q, b); ed_point_add(q, p); ed_point_add(p, p); ed_point_swap(p, q, b);
    }
}
static void ed_scalarbase(ed_gf p[4], const uint8_t s[32]) {
    ed_gf q[4];
    ed_set(q[0], ed_bx); ed_set(q[1], ed_by); ed_set(q[2], ed_one); ed_mul(q[3], ed_bx, ed_by);
    ed_scalarmult(p, q, s);
}
/* RFC 8032 §5.1.3 decoding, negated (r = -A): fails for a y that is not
 * canonical (y >= p), a y with no x, and x = 0 with the sign bit set. */
static bool ed_unpack_negated(ed_gf r[4], const uint8_t p[32]) {
    ed_gf t, chk, num, den, den2, den4, den6;
    uint8_t canonical[32];
    ed_set(r[2], ed_one);
    ed_unpack(r[1], p);
    ed_pack(canonical, r[1]);
    if (memcmp(canonical, p, 31) || canonical[31] != (p[31] & 0x7f)) return false;
    ed_square(num, r[1]); ed_mul(den, num, ed_d); ed_sub_gf(num, num, r[2]); ed_add_gf(den, r[2], den);
    ed_square(den2, den); ed_square(den4, den2); ed_mul(den6, den4, den2); ed_mul(t, den6, num); ed_mul(t, t, den);
    ed_pow2523(t, t); ed_mul(t, t, num); ed_mul(t, t, den); ed_mul(t, t, den); ed_mul(r[0], t, den);
    ed_square(chk, r[0]); ed_mul(chk, chk, den);
    if (!ed_equal(chk, num)) ed_mul(r[0], r[0], ed_sqrtm1);
    ed_square(chk, r[0]); ed_mul(chk, chk, den);
    if (!ed_equal(chk, num)) return false;
    if (ed_equal(r[0], ed_zero) && (p[31] >> 7)) return false;
    if (ed_parity(r[0]) == (p[31] >> 7)) ed_sub_gf(r[0], ed_zero, r[0]);
    ed_mul(r[3], r[0], r[1]);
    return true;
}
/* Whether [8]P is the neutral element (0, 1): P has small order. */
static bool ed_small_order(ed_gf p[4]) {
    ed_gf q[4];
    for (int i = 0; i < 4; i++) ed_set(q[i], p[i]);
    for (int i = 0; i < 3; i++) ed_point_add(q, q);
    return ed_equal(q[0], ed_zero) && ed_equal(q[1], q[2]);
}
static void ed_mod_l(uint8_t r[32], int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; --i) {
        int j;
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ed_l[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * ed_l[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * ed_l[j];
    for (int i = 0; i < 32; ++i) { x[i + 1] += x[i] >> 8; r[i] = (uint8_t)(x[i] & 255); }
}
static void ed_reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    memset(r, 0, 64);
    ed_mod_l(r, x);
    crypto_wipe(x, sizeof x);
}
static void ed_sha512(uint8_t out[64], const uint8_t *a, size_t a_size, const uint8_t *b, size_t b_size,
                      const uint8_t *c, size_t c_size) {
    br_sha512_context context;
    br_sha512_init(&context);
    br_sha512_update(&context, a, a_size);
    if (b_size) br_sha512_update(&context, b, b_size);
    if (c_size) br_sha512_update(&context, c, c_size);
    br_sha512_out(&context, out);
    crypto_wipe(&context, sizeof context);
}
/* The secret scalar and prefix of a 32-byte private key (§5.1.5). */
static void ed_expand(uint8_t h[64], const uint8_t seed[32]) {
    ed_sha512(h, seed, 32, NULL, 0, NULL, 0);
    h[0] &= 248; h[31] &= 127; h[31] |= 64;
}
static void ed_public(uint8_t pub[32], const uint8_t seed[32]) {
    uint8_t h[64]; ed_gf p[4];
    ed_expand(h, seed);
    ed_scalarbase(p, h);
    ed_point_pack(pub, p);
    crypto_wipe(h, sizeof h); crypto_wipe(p, sizeof p);
}
static void ed_sign(uint8_t sig[64], const uint8_t seed[32], const uint8_t *m, size_t size) {
    uint8_t h[64], r[64], k[64], pub[32]; ed_gf p[4]; int64_t x[64];
    ed_expand(h, seed);
    ed_scalarbase(p, h); ed_point_pack(pub, p);
    ed_sha512(r, h + 32, 32, m, size, NULL, 0);
    ed_reduce(r);
    ed_scalarbase(p, r); ed_point_pack(sig, p);
    ed_sha512(k, sig, 32, pub, 32, m, size);
    ed_reduce(k);
    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++) x[i + j] += (int64_t)k[i] * h[j];
    ed_mod_l(sig + 32, x);
    crypto_wipe(h, sizeof h); crypto_wipe(r, sizeof r); crypto_wipe(x, sizeof x); crypto_wipe(p, sizeof p);
}
/* §5.1.7 with the cofactorless equation [S]B = R + [k]A. Invalid or
 * small-order A and R fail (the Web Cryptography API's Ed25519 verify). */
static bool ed_verify(const uint8_t pub[32], const uint8_t *m, size_t size, const uint8_t sig[64]) {
    ed_gf a[4], r[4], p[4], q[4];
    if (!ed_unpack_negated(a, pub) || ed_small_order(a)) return false;
    if (!ed_unpack_negated(r, sig) || ed_small_order(r)) return false;
    /* S < L */
    int borrow = 0;
    for (int i = 0; i < 32; i++) { int diff = (int)sig[32 + i] - (int)ed_l[i] - borrow; borrow = diff < 0; }
    if (!borrow) return false;
    uint8_t k[64], check[32];
    ed_sha512(k, sig, 32, pub, 32, m, size);
    ed_reduce(k);
    ed_scalarmult(p, a, k);
    ed_scalarbase(q, sig + 32);
    ed_point_add(p, q);
    ed_point_pack(check, p);
    return !memcmp(check, sig, 32);
}
/* cryptoEd25519(op, key, data, signature): 0 the public key of a private key,
 * 1 sign with a private key, 2 verify with a public key. */
static JSValue crypto_ed25519(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t op;
    const uint8_t *key, *data, *signature; size_t key_size, size, signature_size;
    if (argc < 4 || JS_ToInt32(ctx, &op, argv[0]) < 0 || !crypto_bytes(ctx, argv[1], &key, &key_size) ||
        !crypto_bytes(ctx, argv[2], &data, &size) || !crypto_bytes(ctx, argv[3], &signature, &signature_size))
        return JS_EXCEPTION;
    if (key_size != 32) return JS_ThrowTypeError(ctx, "Ed25519 keys are 32 bytes");
    uint8_t out[64];
    if (op == 0) { ed_public(out, key); return crypto_result(ctx, out, 32); }
    if (op == 1) {
        ed_sign(out, key, data, size);
        JSValue result = crypto_result(ctx, out, 64);
        crypto_wipe(out, sizeof out);
        return result;
    }
    return JS_NewBool(ctx, signature_size == 64 && ed_verify(key, data, size, signature));
}

void web_js_crypto_init(JSContext *ctx, JSValue host) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!rsa_job_class) JS_NewClassID(&rsa_job_class);
    if (!JS_IsRegisteredClass(rt, rsa_job_class)) {
        JSClassDef job = {"CryptoRSAJob", rsa_job_finalizer, NULL, NULL, NULL};
        JS_NewClass(rt, rsa_job_class, &job);
    }
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("cryptoRandom", 1, crypto_random),
        JS_CFUNC_DEF("cryptoDigest", 2, crypto_digest),
        JS_CFUNC_DEF("cryptoAES", 7, crypto_aes),
        JS_CFUNC_DEF("cryptoHMAC", 3, crypto_hmac),
        JS_CFUNC_DEF("cryptoPBKDF2Step", 5, crypto_pbkdf2_step),
        JS_CFUNC_DEF("cryptoHKDF", 5, crypto_hkdf),
        JS_CFUNC_DEF("cryptoRSAGenerateStart", 2, crypto_rsa_generate_start),
        JS_CFUNC_DEF("cryptoRSAGenerateResult", 1, crypto_rsa_generate_result),
        JS_CFUNC_DEF("cryptoJobWait", 2, crypto_job_wait),
        JS_CFUNC_DEF("cryptoRSAPrivate", 10, crypto_rsa_private),
        JS_CFUNC_DEF("cryptoRSAPublic", 7, crypto_rsa_public),
        JS_CFUNC_DEF("cryptoECGenerate", 1, crypto_ec_generate),
        JS_CFUNC_DEF("cryptoECPublic", 2, crypto_ec_public),
        JS_CFUNC_DEF("cryptoECValid", 2, crypto_ec_valid),
        JS_CFUNC_DEF("cryptoECDSA", 6, crypto_ecdsa),
        JS_CFUNC_DEF("cryptoECDH", 3, crypto_ecdh),
        JS_CFUNC_DEF("cryptoX25519", 2, crypto_x25519),
        JS_CFUNC_DEF("cryptoEd25519", 4, crypto_ed25519),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}
