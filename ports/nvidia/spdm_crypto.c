/* NVIDIA RM on Nocturne: the cryptography under RM's SPDM library (the port of
 * kernel-open/nvidia/libspdm_*.c).
 *
 * RM links DMTF's libspdm (third_party/nvidia-open/src/nvidia/src/libraries/libspdm) for its
 * SPDM sessions with the GPU (attestation and the confidential-computing key exchange), and
 * libspdm leaves the primitives to the host: SHA-2, HMAC, HKDF, AES-GCM, ECDSA verification,
 * ECDH, RSA-PSS signing, the few X.509 operations RM uses, base64 and random bytes. NVIDIA's
 * Linux module builds them on the kernel's crypto API; here they are built on BearSSL
 * (third_party/bearssl, MIT), which nvrm links like every Nocturne program, and the system's
 * entropy (n_random).
 *
 * The behaviour follows NVIDIA's version, including what it leaves out: ECDSA signing, RSA-PSS
 * verification, RSA keys from certificates and most certificate field getters answer false
 * there and here, and the date comparison answers -3. Where NVIDIA's version and the DMTF
 * contract of the library (the headers in include/hal/library/cryptlib) part, the contract RM's callers
 * rely on is kept and the place says so.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <base.h>
#include "library/cryptlib.h"
#include "nvspdm_cryptlib_extensions.h"

#include <bearssl.h>

#include "nocturne.h"

/* ---------------------------------------------------------------- helpers */
static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static bool input_ok(const void *data, size_t size) {
    return (data || !size) && size <= INT_MAX;
}

/* ---------------------------------------------------------------- random bytes */
bool libspdm_random_bytes(uint8_t *output, size_t size) {
    if (!output && size) return false;
    return !size || n_random(output, size) >= 0;
}

bool libspdm_check_crypto_backend(void) { return true; }

/* A deterministic generator seeded from the system's entropy, for BearSSL's key generation. */
static bool drbg_seed(br_hmac_drbg_context *drbg) {
    unsigned char seed[48];
    if (n_random(seed, sizeof seed) < 0) return false;
    br_hmac_drbg_init(drbg, &br_sha384_vtable, seed, sizeof seed);
    wipe(seed, sizeof seed);
    return true;
}

/* ---------------------------------------------------------------- SHA-256, SHA-384 */
static void *hash_new(const br_hash_class *hash) {
    br_hash_compat_context *c = malloc(sizeof *c);
    if (c) hash->init(&c->vtable);
    return c;
}

static void hash_free(void *ctx) {
    if (!ctx) return;
    wipe(ctx, sizeof(br_hash_compat_context));
    free(ctx);
}

static bool hash_init(void *ctx) {
    br_hash_compat_context *c = ctx;
    if (!c) return false;
    c->vtable->init(&c->vtable);
    return true;
}

static bool hash_duplicate(const void *from, void *to) {
    if (!from || !to) return false;
    memcpy(to, from, sizeof(br_hash_compat_context));
    return true;
}

static bool hash_update(void *ctx, const void *data, size_t size) {
    br_hash_compat_context *c = ctx;
    if (!c || !input_ok(data, size)) return false;
    c->vtable->update(&c->vtable, data, size);
    return true;
}

static bool hash_final(void *ctx, uint8_t *out) {
    br_hash_compat_context *c = ctx;
    if (!c || !out) return false;
    c->vtable->out(&c->vtable, out);
    return true;
}

static bool hash_all(const br_hash_class *hash, const void *data, size_t size, uint8_t *out) {
    if (!out || !input_ok(data, size)) return false;
    br_hash_compat_context c;
    hash->init(&c.vtable);
    hash->update(&c.vtable, data, size);
    hash->out(&c.vtable, out);
    wipe(&c, sizeof c);
    return true;
}

void *libspdm_sha256_new(void) { return hash_new(&br_sha256_vtable); }
void libspdm_sha256_free(void *ctx) { hash_free(ctx); }
bool libspdm_sha256_init(void *ctx) { return hash_init(ctx); }
bool libspdm_sha256_duplicate(const void *ctx, void *new_ctx) { return hash_duplicate(ctx, new_ctx); }
bool libspdm_sha256_update(void *ctx, const void *data, size_t size) { return hash_update(ctx, data, size); }
bool libspdm_sha256_final(void *ctx, uint8_t *hash_value) { return hash_final(ctx, hash_value); }
bool libspdm_sha256_hash_all(const void *data, size_t size, uint8_t *hash_value) {
    return hash_all(&br_sha256_vtable, data, size, hash_value);
}

void *libspdm_sha384_new(void) { return hash_new(&br_sha384_vtable); }
void libspdm_sha384_free(void *ctx) { hash_free(ctx); }
bool libspdm_sha384_init(void *ctx) { return hash_init(ctx); }
bool libspdm_sha384_duplicate(const void *ctx, void *new_ctx) { return hash_duplicate(ctx, new_ctx); }
bool libspdm_sha384_update(void *ctx, const void *data, size_t size) { return hash_update(ctx, data, size); }
bool libspdm_sha384_final(void *ctx, uint8_t *hash_value) { return hash_final(ctx, hash_value); }
bool libspdm_sha384_hash_all(const void *data, size_t size, uint8_t *hash_value) {
    return hash_all(&br_sha384_vtable, data, size, hash_value);
}

/* ---------------------------------------------------------------- HMAC */
struct hmac {
    const br_hash_class *hash;
    br_hmac_key_context key;
    br_hmac_context mac;
    bool keyed;
};

static void *hmac_new(const br_hash_class *hash) {
    struct hmac *m = calloc(1, sizeof *m);
    if (m) m->hash = hash;
    return m;
}

static void hmac_free(void *ctx) {
    if (!ctx) return;
    wipe(ctx, sizeof(struct hmac));
    free(ctx);
}

static bool hmac_set_key(void *ctx, const uint8_t *key, size_t key_size) {
    struct hmac *m = ctx;
    if (!m || !input_ok(key, key_size)) return false;
    br_hmac_key_init(&m->key, m->hash, key, key_size);
    br_hmac_init(&m->mac, &m->key, 0);
    m->keyed = true;
    return true;
}

static bool hmac_duplicate(const void *from, void *to) {
    const struct hmac *a = from;
    struct hmac *b = to;
    if (!a || !b || a->hash != b->hash) return false;
    *b = *a;
    return true;
}

static bool hmac_update(void *ctx, const void *data, size_t size) {
    struct hmac *m = ctx;
    if (!m || !m->keyed || !input_ok(data, size)) return false;
    br_hmac_update(&m->mac, data, size);
    return true;
}

static bool hmac_final(void *ctx, uint8_t *out) {
    struct hmac *m = ctx;
    if (!m || !m->keyed || !out) return false;
    br_hmac_out(&m->mac, out);
    return true;
}

static bool hmac_all(const br_hash_class *hash, const void *data, size_t size, const uint8_t *key,
                     size_t key_size, uint8_t *out) {
    if (!out || !input_ok(data, size) || !input_ok(key, key_size)) return false;
    struct hmac m = {.hash = hash};
    hmac_set_key(&m, key, key_size);
    br_hmac_update(&m.mac, data, size);
    br_hmac_out(&m.mac, out);
    wipe(&m, sizeof m);
    return true;
}

void *libspdm_hmac_sha256_new(void) { return hmac_new(&br_sha256_vtable); }
void libspdm_hmac_sha256_free(void *ctx) { hmac_free(ctx); }
bool libspdm_hmac_sha256_set_key(void *ctx, const uint8_t *key, size_t key_size) {
    return hmac_set_key(ctx, key, key_size);
}
bool libspdm_hmac_sha256_duplicate(const void *ctx, void *new_ctx) { return hmac_duplicate(ctx, new_ctx); }
bool libspdm_hmac_sha256_update(void *ctx, const void *data, size_t size) { return hmac_update(ctx, data, size); }
bool libspdm_hmac_sha256_final(void *ctx, uint8_t *hmac_value) { return hmac_final(ctx, hmac_value); }
bool libspdm_hmac_sha256_all(const void *data, size_t size, const uint8_t *key, size_t key_size,
                             uint8_t *hmac_value) {
    return hmac_all(&br_sha256_vtable, data, size, key, key_size, hmac_value);
}

void *libspdm_hmac_sha384_new(void) { return hmac_new(&br_sha384_vtable); }
void libspdm_hmac_sha384_free(void *ctx) { hmac_free(ctx); }
bool libspdm_hmac_sha384_set_key(void *ctx, const uint8_t *key, size_t key_size) {
    return hmac_set_key(ctx, key, key_size);
}
bool libspdm_hmac_sha384_duplicate(const void *ctx, void *new_ctx) { return hmac_duplicate(ctx, new_ctx); }
bool libspdm_hmac_sha384_update(void *ctx, const void *data, size_t size) { return hmac_update(ctx, data, size); }
bool libspdm_hmac_sha384_final(void *ctx, uint8_t *hmac_value) { return hmac_final(ctx, hmac_value); }
bool libspdm_hmac_sha384_all(const void *data, size_t size, const uint8_t *key, size_t key_size,
                             uint8_t *hmac_value) {
    return hmac_all(&br_sha384_vtable, data, size, key, key_size, hmac_value);
}

/* ---------------------------------------------------------------- HKDF (RFC 5869) */
/* Extract is HMAC(salt, IKM) into a PRK of exactly the hash's size, as NVIDIA's version. */
bool libspdm_hkdf_sha256_extract(const uint8_t *key, size_t key_size, const uint8_t *salt, size_t salt_size,
                                 uint8_t *prk_out, size_t prk_out_size) {
    if (prk_out_size != LIBSPDM_SHA256_DIGEST_SIZE) return false;
    return libspdm_hmac_sha256_all(key, key_size, salt, salt_size, prk_out);
}
bool libspdm_hkdf_sha384_extract(const uint8_t *key, size_t key_size, const uint8_t *salt, size_t salt_size,
                                 uint8_t *prk_out, size_t prk_out_size) {
    if (prk_out_size != LIBSPDM_SHA384_DIGEST_SIZE) return false;
    return libspdm_hmac_sha384_all(key, key_size, salt, salt_size, prk_out);
}

/* T(1) = HMAC(PRK, info | 1), T(i) = HMAC(PRK, T(i-1) | info | i); OKM is their first bytes. */
static bool hkdf_expand(const br_hash_class *hash, const uint8_t *prk, size_t prk_size, const uint8_t *info,
                        size_t info_size, uint8_t *out, size_t out_size) {
    size_t hlen = (hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    if (!prk || !info || !out || prk_size > 64 || info_size > INT_MAX || out_size > 255 * hlen) return false;
    struct hmac m = {.hash = hash};
    hmac_set_key(&m, prk, prk_size);
    unsigned char t[64];
    for (size_t done = 0, i = 1; done < out_size; i++) {
        br_hmac_context step;
        br_hmac_init(&step, &m.key, 0);
        if (done) br_hmac_update(&step, t, hlen);
        br_hmac_update(&step, info, info_size);
        unsigned char counter = (unsigned char)i;
        br_hmac_update(&step, &counter, 1);
        br_hmac_out(&step, t);
        size_t n = out_size - done < hlen ? out_size - done : hlen;
        memcpy(out + done, t, n);
        done += n;
        wipe(&step, sizeof step);
    }
    wipe(t, sizeof t);
    wipe(&m, sizeof m);
    return true;
}

bool libspdm_hkdf_sha256_expand(const uint8_t *prk, size_t prk_size, const uint8_t *info, size_t info_size,
                                uint8_t *out, size_t out_size) {
    return hkdf_expand(&br_sha256_vtable, prk, prk_size, info, info_size, out, out_size);
}
bool libspdm_hkdf_sha384_expand(const uint8_t *prk, size_t prk_size, const uint8_t *info, size_t info_size,
                                uint8_t *out, size_t out_size) {
    return hkdf_expand(&br_sha384_vtable, prk, prk_size, info, info_size, out, out_size);
}

/* ---------------------------------------------------------------- AES-GCM */
/* The CPU's AES and carry-less multiply instructions when it has them, BearSSL's constant-time
   code otherwise. */
struct gcm {
    br_aes_gen_ctr_keys keys;
    br_gcm_context gcm;
};

static bool gcm_args_ok(size_t key_size, size_t iv_size, const uint8_t *a_data, size_t a_size,
                        const uint8_t *data_in, size_t in_size, const uint8_t *tag, size_t tag_size,
                        const uint8_t *data_out, const size_t *out_size) {
    if (!input_ok(data_in, in_size) || !input_ok(a_data, a_size) || iv_size != 12 || !tag) return false;
    if (key_size != 16 && key_size != 24 && key_size != 32) return false;
    if (tag_size < 12 || tag_size > 16) return false;
    if (out_size && (*out_size > INT_MAX || *out_size < in_size)) return false;
    return data_out || !in_size;
}

static bool gcm_run(struct gcm *s, const uint8_t *key, size_t key_size, const uint8_t *iv, size_t iv_size,
                    const uint8_t *a_data, size_t a_size, const uint8_t *data_in, size_t in_size, uint8_t *tag,
                    size_t tag_size, uint8_t *data_out, size_t *out_size, bool encrypt) {
    if (!key || !iv) return false;
    if (!gcm_args_ok(key_size, iv_size, a_data, a_size, data_in, in_size, tag, tag_size, data_out, out_size))
        return false;
    const br_block_ctr_class *aes = br_aes_x86ni_ctr_get_vtable();
    if (!aes) aes = &br_aes_ct64_ctr_vtable;
    br_ghash ghash = br_ghash_pclmul_get();
    if (!ghash) ghash = br_ghash_ctmul64;
    aes->init(&s->keys.vtable, key, key_size);
    br_gcm_init(&s->gcm, &s->keys.vtable, ghash);
    br_gcm_reset(&s->gcm, iv, iv_size);
    if (a_size) br_gcm_aad_inject(&s->gcm, a_data, a_size);
    br_gcm_flip(&s->gcm);
    if (in_size) {
        memmove(data_out, data_in, in_size);
        br_gcm_run(&s->gcm, encrypt, data_out, in_size);
    }
    bool ok = true;
    if (encrypt) {
        unsigned char full[16];
        br_gcm_get_tag(&s->gcm, full);
        memcpy(tag, full, tag_size);
    } else if (!br_gcm_check_tag_trunc(&s->gcm, tag, tag_size)) {
        /* nothing of a forged message is handed out */
        if (in_size) wipe(data_out, in_size);
        ok = false;
    }
    wipe(s, sizeof *s);
    /* NVIDIA's version sets the size whether or not the operation succeeded */
    if (out_size) *out_size = in_size;
    return ok;
}

bool libspdm_aead_gcm_prealloc(void **context) {
    if (!context) return false;
    *context = calloc(1, sizeof(struct gcm));
    return *context != NULL;
}

void libspdm_aead_free(void *context) {
    if (!context) return;
    wipe(context, sizeof(struct gcm));
    free(context);
}

bool libspdm_aead_aes_gcm_encrypt_prealloc(void *context, const uint8_t *key, size_t key_size, const uint8_t *iv,
                                           size_t iv_size, const uint8_t *a_data, size_t a_data_size,
                                           const uint8_t *data_in, size_t data_in_size, uint8_t *tag_out,
                                           size_t tag_size, uint8_t *data_out, size_t *data_out_size) {
    if (!context) return false;
    return gcm_run(context, key, key_size, iv, iv_size, a_data, a_data_size, data_in, data_in_size, tag_out,
                   tag_size, data_out, data_out_size, true);
}

bool libspdm_aead_aes_gcm_decrypt_prealloc(void *context, const uint8_t *key, size_t key_size, const uint8_t *iv,
                                           size_t iv_size, const uint8_t *a_data, size_t a_data_size,
                                           const uint8_t *data_in, size_t data_in_size, const uint8_t *tag,
                                           size_t tag_size, uint8_t *data_out, size_t *data_out_size) {
    if (!context) return false;
    return gcm_run(context, key, key_size, iv, iv_size, a_data, a_data_size, data_in, data_in_size,
                   (uint8_t *)tag, tag_size, data_out, data_out_size, false);
}

bool libspdm_aead_aes_gcm_encrypt(const uint8_t *key, size_t key_size, const uint8_t *iv, size_t iv_size,
                                  const uint8_t *a_data, size_t a_data_size, const uint8_t *data_in,
                                  size_t data_in_size, uint8_t *tag_out, size_t tag_size, uint8_t *data_out,
                                  size_t *data_out_size) {
    struct gcm s;
    return gcm_run(&s, key, key_size, iv, iv_size, a_data, a_data_size, data_in, data_in_size, tag_out, tag_size,
                   data_out, data_out_size, true);
}

bool libspdm_aead_aes_gcm_decrypt(const uint8_t *key, size_t key_size, const uint8_t *iv, size_t iv_size,
                                  const uint8_t *a_data, size_t a_data_size, const uint8_t *data_in,
                                  size_t data_in_size, const uint8_t *tag, size_t tag_size, uint8_t *data_out,
                                  size_t *data_out_size) {
    struct gcm s;
    return gcm_run(&s, key, key_size, iv, iv_size, a_data, a_data_size, data_in, data_in_size, (uint8_t *)tag,
                   tag_size, data_out, data_out_size, false);
}

/* ---------------------------------------------------------------- elliptic curves */
/* Public keys travel as x || y (big endian, without the 04 of the uncompressed form), as SPDM's
   KEY_EXCHANGE and NVIDIA's version have them; size is their length. */
struct ec {
    int curve;                       /* BR_EC_secp256r1 or BR_EC_secp384r1 */
    size_t size;                     /* 64 or 96 */
    unsigned char point[1 + 96];     /* 04 || x || y */
    unsigned char secret[BR_EC_KBUF_PRIV_MAX_SIZE];
    size_t secret_len;
    bool has_public, has_secret;
};

void *libspdm_ec_new_by_nid(size_t nid) {
    if (nid != LIBSPDM_CRYPTO_NID_SECP256R1 && nid != LIBSPDM_CRYPTO_NID_SECP384R1) return NULL;
    struct ec *ec = calloc(1, sizeof *ec);
    if (!ec) return NULL;
    ec->curve = nid == LIBSPDM_CRYPTO_NID_SECP256R1 ? BR_EC_secp256r1 : BR_EC_secp384r1;
    ec->size = nid == LIBSPDM_CRYPTO_NID_SECP256R1 ? 64 : 96;
    return ec;
}

void libspdm_ec_free(void *ec_context) {
    if (!ec_context) return;
    wipe(ec_context, sizeof(struct ec));
    free(ec_context);
}

/* A public key accepted only when it is a point of the curve. */
static bool ec_set_public(struct ec *ec, const uint8_t *xy, size_t size) {
    if (size != ec->size) return false;
    unsigned char check[1 + 96];
    check[0] = 0x04;
    memcpy(check + 1, xy, size);
    static const unsigned char one = 1;
    if (!br_ec_get_default()->mul(check, 1 + size, &one, 1, ec->curve)) return false;
    ec->point[0] = 0x04;
    memcpy(ec->point + 1, xy, size);
    ec->has_public = true;
    return true;
}

bool libspdm_ec_generate_key(void *ec_context, uint8_t *public_data, size_t *public_size) {
    struct ec *ec = ec_context;
    if (!ec || !public_size) return false;
    if (!public_data && *public_size) return false;
    if (*public_size < ec->size) {
        *public_size = ec->size;
        return false;
    }
    const br_ec_impl *impl = br_ec_get_default();
    br_hmac_drbg_context drbg;
    if (!drbg_seed(&drbg)) return false;
    br_ec_private_key sk;
    size_t made = br_ec_keygen(&drbg.vtable, impl, &sk, ec->secret, ec->curve);
    wipe(&drbg, sizeof drbg);
    if (!made) return false;
    br_ec_public_key pk;
    unsigned char point[BR_EC_KBUF_PUB_MAX_SIZE];
    if (br_ec_compute_pub(impl, &pk, point, &sk) != 1 + ec->size) return false;
    memcpy(ec->point, point, 1 + ec->size);
    ec->secret_len = sk.xlen;
    ec->has_secret = ec->has_public = true;
    memcpy(public_data, ec->point + 1, ec->size);
    *public_size = ec->size;
    return true;
}

bool libspdm_ec_compute_key(void *ec_context, const uint8_t *peer_public, size_t peer_public_size, uint8_t *key,
                            size_t *key_size) {
    struct ec *ec = ec_context;
    if (!ec || !peer_public || !key_size || !key || peer_public_size > INT_MAX) return false;
    if (peer_public_size != ec->size || !ec->has_secret || *key_size < ec->size / 2) return false;
    unsigned char shared[1 + 96];
    shared[0] = 0x04;
    memcpy(shared + 1, peer_public, ec->size);
    bool ok = br_ec_get_default()->mul(shared, 1 + ec->size, ec->secret, ec->secret_len, ec->curve) == 1;
    if (ok) {
        memcpy(key, shared + 1, ec->size / 2); /* the x coordinate */
        *key_size = ec->size / 2;
    }
    wipe(shared, sizeof shared);
    return ok;
}

static bool hash_size_ok(size_t hash_nid, size_t hash_size) {
    switch (hash_nid) {
    case LIBSPDM_CRYPTO_NID_SHA256: return hash_size == LIBSPDM_SHA256_DIGEST_SIZE;
    case LIBSPDM_CRYPTO_NID_SHA384: return hash_size == LIBSPDM_SHA384_DIGEST_SIZE;
    case LIBSPDM_CRYPTO_NID_SHA512: return hash_size == LIBSPDM_SHA512_DIGEST_SIZE;
    default: return false;
    }
}

/* NVIDIA's version signs nothing: RM holds no EC private key other than an ECDHE one. */
bool libspdm_ecdsa_sign(void *ec_context, size_t hash_nid, const uint8_t *message_hash, size_t hash_size,
                        uint8_t *signature, size_t *sig_size) {
    (void)ec_context; (void)hash_nid; (void)message_hash; (void)hash_size; (void)signature; (void)sig_size;
    return false;
}

/* The signature is r || s, each the size of the curve's order. */
bool libspdm_ecdsa_verify(void *ec_context, size_t hash_nid, const uint8_t *message_hash, size_t hash_size,
                          const uint8_t *signature, size_t sig_size) {
    struct ec *ec = ec_context;
    if (!ec || !message_hash || !signature || !sig_size || sig_size > INT_MAX) return false;
    if (!hash_size_ok(hash_nid, hash_size) || sig_size != ec->size || !ec->has_public) return false;
    br_ec_public_key pk = {.curve = ec->curve, .q = ec->point, .qlen = 1 + ec->size};
    return br_ecdsa_i31_vrfy_raw(br_ec_get_default(), message_hash, hash_size, &pk, signature, sig_size) == 1;
}

/* ---------------------------------------------------------------- RSA */
/* RM signs SPDM FINISH with a 3072-bit RSA-PSS key given as n, e and d (nvspdm_rsa.c), not in
   CRT form, so the private operation is m^d mod n. */
#define RSA_PARTS (LIBSPDM_RSA_KEY_Q_INV + 1)
#define RSA_MAX_BYTES (4096 / 8) /* the largest modulus BearSSL's i31 code takes */
struct rsa {
    uint8_t *part[RSA_PARTS];
    size_t length[RSA_PARTS];
};

void *libspdm_rsa_new(void) { return calloc(1, sizeof(struct rsa)); }

void libspdm_rsa_free(void *rsa_context) {
    struct rsa *rsa = rsa_context;
    if (!rsa) return;
    for (int i = 0; i < RSA_PARTS; i++) {
        if (rsa->part[i]) wipe(rsa->part[i], rsa->length[i]);
        free(rsa->part[i]);
    }
    free(rsa);
}

bool libspdm_rsa_set_key(void *rsa_context, const libspdm_rsa_key_tag_t key_tag, const uint8_t *big_number,
                         size_t bn_size) {
    struct rsa *rsa = rsa_context;
    if (!rsa || (unsigned)key_tag >= RSA_PARTS) return false;
    uint8_t *copy = NULL;
    if (big_number && bn_size) {
        copy = malloc(bn_size);
        if (!copy) return false;
        memcpy(copy, big_number, bn_size);
    } else {
        bn_size = 0; /* NULL clears the part */
    }
    if (rsa->part[key_tag]) wipe(rsa->part[key_tag], rsa->length[key_tag]);
    free(rsa->part[key_tag]);
    rsa->part[key_tag] = copy;
    rsa->length[key_tag] = bn_size;
    return true;
}

/* MGF1 with SHA-384 (RFC 8017, B.2.1), XORed into mask. */
static void mgf1_sha384_xor(uint8_t *mask, size_t length, const uint8_t *seed, size_t seed_length) {
    unsigned char block[48];
    for (uint32_t counter = 0; length; counter++) {
        br_sha384_context c;
        unsigned char be[4] = {(unsigned char)(counter >> 24), (unsigned char)(counter >> 16),
                               (unsigned char)(counter >> 8), (unsigned char)counter};
        br_sha384_init(&c);
        br_sha384_update(&c, seed, seed_length);
        br_sha384_update(&c, be, 4);
        br_sha384_out(&c, block);
        size_t n = length < sizeof block ? length : sizeof block;
        for (size_t i = 0; i < n; i++) mask[i] ^= block[i];
        mask += n;
        length -= n;
    }
    wipe(block, sizeof block);
}

/* RSASSA-PSS signing with SHA-384, MGF1-SHA-384 and a 48-byte salt (RFC 8017, 8.1.1 and 9.1.1),
   the only form NVIDIA's version makes. The signature is written at the modulus' full length. */
bool libspdm_rsa_pss_sign(void *rsa_context, size_t hash_nid, const uint8_t *message_hash, size_t hash_size,
                          uint8_t *signature, size_t *sig_size) {
    struct rsa *rsa = rsa_context;
    enum { HLEN = LIBSPDM_SHA384_DIGEST_SIZE, SLEN = LIBSPDM_SHA384_DIGEST_SIZE };
    if (!rsa || !message_hash || !sig_size) return false;
    if (hash_nid != LIBSPDM_CRYPTO_NID_SHA384 || hash_size != HLEN) return false;
    const uint8_t *n = rsa->part[LIBSPDM_RSA_KEY_N], *d = rsa->part[LIBSPDM_RSA_KEY_D];
    size_t n_len = rsa->length[LIBSPDM_RSA_KEY_N];
    if (!n || !d) return false;
    while (n_len && !*n) n++, n_len--;
    if (!n_len || n_len > RSA_MAX_BYTES) return false;
    if (*sig_size < n_len || !signature) {
        *sig_size = n_len;
        return false;
    }
    unsigned top = 8;
    while (!(n[0] & (1u << (top - 1)))) top--;
    size_t mod_bits = (n_len - 1) * 8 + top, em_bits = mod_bits - 1, em_len = (em_bits + 7) / 8;
    if (em_len < HLEN + SLEN + 2) return false;

    unsigned char salt[SLEN];
    if (n_random(salt, sizeof salt) < 0) return false;
    /* EM = maskedDB || H || bc, right-aligned in the modulus' length */
    uint8_t *em = signature + (n_len - em_len);
    memset(signature, 0, n_len);
    size_t db_len = em_len - HLEN - 1;
    uint8_t *h = em + db_len;
    static const unsigned char zeros[8];
    br_sha384_context c;
    br_sha384_init(&c);
    br_sha384_update(&c, zeros, sizeof zeros);
    br_sha384_update(&c, message_hash, HLEN);
    br_sha384_update(&c, salt, SLEN);
    br_sha384_out(&c, h);
    em[db_len - SLEN - 1] = 0x01;
    memcpy(em + db_len - SLEN, salt, SLEN);
    mgf1_sha384_xor(em, db_len, h, HLEN);
    em[0] &= (unsigned char)(0xFF >> (8 * em_len - em_bits));
    em[em_len - 1] = 0xbc;
    wipe(salt, sizeof salt);

    /* s = EM^d mod n: the same modular exponentiation as the public operation, with d */
    br_rsa_public_key private_exponent = {
        .n = (unsigned char *)n, .nlen = n_len,
        .e = (unsigned char *)d, .elen = rsa->length[LIBSPDM_RSA_KEY_D],
    };
    if (!br_rsa_i31_public(signature, n_len, &private_exponent)) {
        wipe(signature, n_len);
        return false;
    }
    *sig_size = n_len;
    return true;
}

/* As NVIDIA's version: RM only signs with RSA (the GSP verifies). */
bool libspdm_rsa_pss_verify(void *rsa_context, size_t hash_nid, const uint8_t *message_hash, size_t hash_size,
                            const uint8_t *signature, size_t sig_size) {
    (void)rsa_context; (void)hash_nid; (void)message_hash; (void)hash_size; (void)signature; (void)sig_size;
    return false;
}

/* ---------------------------------------------------------------- DER */
/* One DER element at [p, end): its tag, where its contents start and their length. */
static bool der_element(const uint8_t *p, const uint8_t *end, uint8_t *tag, const uint8_t **content,
                        size_t *length) {
    if (!p || p >= end || end - p < 2) return false;
    size_t avail = (size_t)(end - p), header = 2, n = p[1];
    if (n & 0x80) {
        size_t bytes = n & 0x7F;
        if (!bytes || bytes > 4 || avail < 2 + bytes) return false;
        n = 0;
        for (size_t i = 0; i < bytes; i++) n = n << 8 | p[2 + i];
        header += bytes;
    }
    if (n > avail - header) return false;
    *tag = p[0];
    *content = p + header;
    *length = n;
    return true;
}

/* The DMTF contract (cryptlib_cert.h), which libspdm's own certificate checks are written
   against: *ptr moves to the contents and *length is their length. (NVIDIA's version leaves
   *ptr and gives the whole element's length, which those callers do not expect.) */
bool libspdm_asn1_get_tag(uint8_t **ptr, const uint8_t *end, size_t *length, uint32_t tag) {
    if (!ptr || !length || tag > 0xFF) return false;
    uint8_t got;
    const uint8_t *content;
    size_t n;
    if (!der_element(*ptr, end, &got, &content, &n) || got != tag) return false;
    *ptr = (uint8_t *)content;
    *length = n;
    return true;
}

/* The whole element (tag and length included) at p, when it is a SEQUENCE. */
static bool der_sequence_span(const uint8_t *p, const uint8_t *end, size_t *span) {
    uint8_t tag;
    const uint8_t *content;
    size_t n;
    if (!der_element(p, end, &tag, &content, &n) ||
        tag != (LIBSPDM_CRYPTO_ASN1_CONSTRUCTED | LIBSPDM_CRYPTO_ASN1_SEQUENCE))
        return false;
    *span = (size_t)(content - p) + n;
    return true;
}

/* ---------------------------------------------------------------- base64 (RFC 4648) */
static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool libspdm_encode_base64(const uint8_t *src, uint8_t *dst, size_t srclen, size_t *p_dstlen) {
    if (!p_dstlen || (!src && srclen) || (!dst && *p_dstlen)) return false;
    size_t need = (srclen + 2) / 3 * 4;
    if (srclen > SIZE_MAX / 4 * 3 - 2 || need > *p_dstlen) {
        *p_dstlen = 0;
        return false;
    }
    uint8_t *out = dst;
    for (size_t i = 0; i < srclen; i += 3) {
        size_t left = srclen - i;
        uint32_t v = (uint32_t)src[i] << 16 | (left > 1 ? (uint32_t)src[i + 1] << 8 : 0) | (left > 2 ? src[i + 2] : 0);
        *out++ = (uint8_t)b64_alphabet[v >> 18 & 63];
        *out++ = (uint8_t)b64_alphabet[v >> 12 & 63];
        *out++ = left > 1 ? (uint8_t)b64_alphabet[v >> 6 & 63] : '=';
        *out++ = left > 2 ? (uint8_t)b64_alphabet[v & 63] : '=';
    }
    *p_dstlen = (size_t)(out - dst);
    return true;
}

static int b64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62; /* NVIDIA's version takes the URL-safe letters too */
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* PEM body: groups of four letters on lines that end in a line break; '=' pads only the last
   group. */
bool libspdm_decode_base64(const uint8_t *src, uint8_t *dst, size_t srclen, size_t *p_dstlen) {
    if (!p_dstlen || (!src && srclen) || (!dst && *p_dstlen)) return false;
    size_t written = 0;
    uint32_t group = 0;
    unsigned have = 0, pad = 0;
    for (size_t i = 0; i < srclen; i++) {
        uint8_t c = src[i];
        if (c == '\n' || c == '\r') continue;
        int v = b64_value(c);
        if (c == '=') {
            if (have < 2) goto bad;
            pad++;
            v = 0;
        } else if (v < 0 || pad) {
            goto bad;
        }
        group = group << 6 | (uint32_t)v;
        if (++have < 4) continue;
        unsigned bytes = 3 - pad;
        if (*p_dstlen - written < bytes) goto bad;
        dst[written++] = (uint8_t)(group >> 16);
        if (bytes > 1) dst[written++] = (uint8_t)(group >> 8);
        if (bytes > 2) dst[written++] = (uint8_t)group;
        group = 0;
        have = 0;
        if (pad) {
            /* only line breaks may follow the padded group */
            for (i++; i < srclen; i++)
                if (src[i] != '\n' && src[i] != '\r') goto bad;
            break;
        }
    }
    if (have) goto bad;
    *p_dstlen = written;
    return true;
bad:
    *p_dstlen = 0;
    return false;
}

/* ---------------------------------------------------------------- X.509 */
/* The subject public key of a certificate (BearSSL's decoder); false when it does not parse. */
static bool x509_public_key(const uint8_t *cert, size_t cert_size, br_x509_decoder_context *dc,
                            const br_x509_pkey **key) {
    if (!cert || !cert_size) return false;
    br_x509_decoder_init(dc, NULL, NULL);
    br_x509_decoder_push(dc, cert, cert_size);
    *key = br_x509_decoder_get_pkey(dc);
    return *key && !br_x509_decoder_last_error(dc);
}

bool libspdm_ec_get_public_key_from_x509(const uint8_t *cert, size_t cert_size, void **ec_context) {
    if (!ec_context) return false;
    br_x509_decoder_context dc;
    const br_x509_pkey *key;
    if (!x509_public_key(cert, cert_size, &dc, &key) || key->key_type != BR_KEYTYPE_EC) return false;
    size_t nid;
    if (key->key.ec.curve == BR_EC_secp256r1 && key->key.ec.qlen == 65) nid = LIBSPDM_CRYPTO_NID_SECP256R1;
    else if (key->key.ec.curve == BR_EC_secp384r1 && key->key.ec.qlen == 97) nid = LIBSPDM_CRYPTO_NID_SECP384R1;
    else return false;
    if (key->key.ec.q[0] != 0x04) return false;
    struct ec *ec = libspdm_ec_new_by_nid(nid);
    if (!ec) return false;
    if (!ec_set_public(ec, key->key.ec.q + 1, key->key.ec.qlen - 1)) {
        libspdm_ec_free(ec);
        return false;
    }
    *ec_context = ec;
    return true;
}

/* As NVIDIA's version: RM's peers present EC certificates. */
bool libspdm_rsa_get_public_key_from_x509(const uint8_t *cert, size_t cert_size, void **rsa_context) {
    (void)cert; (void)cert_size; (void)rsa_context;
    return false;
}

/* Signature algorithms a certificate may be signed with: the OID's contents, the hash. */
static const struct {
    uint8_t oid[9];
    uint8_t oid_len;
    bool ec;
    const br_hash_class *hash;
    const unsigned char *rsa_hash_oid;
} sig_algs[] = {
    {{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02}, 8, true, &br_sha256_vtable, NULL},
    {{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03}, 8, true, &br_sha384_vtable, NULL},
    {{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04}, 8, true, &br_sha512_vtable, NULL},
    {{0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B}, 9, false, &br_sha256_vtable, BR_HASH_OID_SHA256},
    {{0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C}, 9, false, &br_sha384_vtable, BR_HASH_OID_SHA384},
    {{0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D}, 9, false, &br_sha512_vtable, BR_HASH_OID_SHA512},
};

/* Whether ca_cert's key signed cert: the signature over its TBSCertificate, as NVIDIA's version
   checks it (no names, dates or extensions; libspdm checks those itself). */
static bool x509_signed_by(const uint8_t *cert, size_t cert_size, const uint8_t *ca_cert, size_t ca_cert_size) {
    if (!cert || !ca_cert) return false;
    const uint8_t *end = cert + cert_size, *body, *tbs, *alg, *oid, *sig;
    size_t body_len, tbs_len, alg_len, oid_len, sig_len;
    uint8_t tag;
    const uint8_t sequence = LIBSPDM_CRYPTO_ASN1_CONSTRUCTED | LIBSPDM_CRYPTO_ASN1_SEQUENCE;
    if (!der_element(cert, end, &tag, &body, &body_len) || tag != sequence) return false;
    end = body + body_len;
    if (!der_element(body, end, &tag, &tbs, &tbs_len) || tag != sequence) return false;
    const uint8_t *tbs_whole = body;
    size_t tbs_whole_len = (size_t)(tbs - body) + tbs_len;
    if (!der_element(tbs + tbs_len, end, &tag, &alg, &alg_len) || tag != sequence) return false;
    if (!der_element(alg, alg + alg_len, &tag, &oid, &oid_len) || tag != LIBSPDM_CRYPTO_ASN1_OID) return false;
    if (!der_element(alg + alg_len, end, &tag, &sig, &sig_len) || tag != LIBSPDM_CRYPTO_ASN1_BIT_STRING ||
        sig_len < 2 || sig[0] != 0)
        return false;
    sig++, sig_len--;

    size_t which = sizeof sig_algs / sizeof sig_algs[0];
    for (size_t i = 0; i < sizeof sig_algs / sizeof sig_algs[0]; i++)
        if (oid_len == sig_algs[i].oid_len && !memcmp(oid, sig_algs[i].oid, oid_len)) which = i;
    if (which == sizeof sig_algs / sizeof sig_algs[0]) return false;
    const br_hash_class *hash = sig_algs[which].hash;
    size_t hlen = (hash->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    unsigned char digest[64];
    br_hash_compat_context hc;
    hash->init(&hc.vtable);
    hash->update(&hc.vtable, tbs_whole, tbs_whole_len);
    hash->out(&hc.vtable, digest);

    br_x509_decoder_context dc;
    const br_x509_pkey *key;
    if (!x509_public_key(ca_cert, ca_cert_size, &dc, &key)) return false;
    if (sig_algs[which].ec) {
        if (key->key_type != BR_KEYTYPE_EC) return false;
        return br_ecdsa_i31_vrfy_asn1(br_ec_get_default(), digest, hlen, &key->key.ec, sig, sig_len) == 1;
    }
    if (key->key_type != BR_KEYTYPE_RSA) return false;
    unsigned char signed_digest[64];
    return br_rsa_i31_pkcs1_vrfy(sig, sig_len, sig_algs[which].rsa_hash_oid, hlen, &key->key.rsa, signed_digest) &&
           !memcmp(signed_digest, digest, hlen);
}

bool libspdm_x509_verify_cert(const uint8_t *cert, size_t cert_size, const uint8_t *ca_cert, size_t ca_cert_size) {
    return x509_signed_by(cert, cert_size, ca_cert, ca_cert_size);
}

/* Each certificate of the chain signed by the one before it, the first by root_cert. */
bool libspdm_x509_verify_cert_chain(const uint8_t *root_cert, size_t root_cert_length, const uint8_t *cert_chain,
                                    size_t cert_chain_length) {
    if (!root_cert || !cert_chain) return false;
    const uint8_t *previous = root_cert, *current = cert_chain, *end = cert_chain + cert_chain_length;
    size_t previous_len = root_cert_length, current_len;
    bool verified = false;
    while (der_sequence_span(current, end, &current_len)) {
        if (!x509_signed_by(current, current_len, previous, previous_len)) return false;
        verified = true;
        previous = current;
        previous_len = current_len;
        current += current_len;
    }
    return verified;
}

/* Certificate cert_index of a chain of DER certificates; -1 is the last one. */
bool libspdm_x509_get_cert_from_cert_chain(const uint8_t *cert_chain, size_t cert_chain_length,
                                           const int32_t cert_index, const uint8_t **cert, size_t *cert_length) {
    if (!cert_chain || !cert || cert_index < -1 || !cert_length) return false;
    const uint8_t *current = cert_chain, *end = cert_chain + cert_chain_length, *last = NULL;
    size_t length, last_length = 0;
    for (int32_t index = 0; der_sequence_span(current, end, &length); index++) {
        if (index == cert_index) {
            *cert = current;
            *cert_length = length;
            return true;
        }
        last = current;
        last_length = length;
        current += length;
    }
    if (cert_index != -1 || !last) return false;
    *cert = last;
    *cert_length = last_length;
    return true;
}

/* The field getters and date functions NVIDIA's version does not provide either. */
bool libspdm_x509_get_subject_name(const uint8_t *cert, size_t cert_size, uint8_t *cert_subject,
                                   size_t *subject_size) {
    (void)cert; (void)cert_size; (void)cert_subject; (void)subject_size;
    return false;
}
bool libspdm_x509_get_version(const uint8_t *cert, size_t cert_size, size_t *version) {
    (void)cert; (void)cert_size; (void)version;
    return false;
}
bool libspdm_x509_get_serial_number(const uint8_t *cert, size_t cert_size, uint8_t *serial_number,
                                    size_t *serial_number_size) {
    (void)cert; (void)cert_size; (void)serial_number; (void)serial_number_size;
    return false;
}
bool libspdm_x509_get_issuer_name(const uint8_t *cert, size_t cert_size, uint8_t *cert_issuer,
                                  size_t *issuer_size) {
    (void)cert; (void)cert_size; (void)cert_issuer; (void)issuer_size;
    return false;
}
bool libspdm_x509_get_signature_algorithm(const uint8_t *cert, size_t cert_size, uint8_t *oid, size_t *oid_size) {
    (void)cert; (void)cert_size; (void)oid; (void)oid_size;
    return false;
}
bool libspdm_x509_get_extension_data(const uint8_t *cert, size_t cert_size, const uint8_t *oid, size_t oid_size,
                                     uint8_t *extension_data, size_t *extension_data_size) {
    (void)cert; (void)cert_size; (void)oid; (void)oid_size; (void)extension_data; (void)extension_data_size;
    return false;
}
bool libspdm_x509_get_validity(const uint8_t *cert, size_t cert_size, uint8_t *from, size_t *from_size, uint8_t *to,
                               size_t *to_size) {
    (void)cert; (void)cert_size; (void)from; (void)from_size; (void)to; (void)to_size;
    return false;
}
bool libspdm_x509_get_key_usage(const uint8_t *cert, size_t cert_size, size_t *usage) {
    (void)cert; (void)cert_size; (void)usage;
    return false;
}
bool libspdm_x509_get_extended_key_usage(const uint8_t *cert, size_t cert_size, uint8_t *usage, size_t *usage_size) {
    (void)cert; (void)cert_size; (void)usage; (void)usage_size;
    return false;
}
bool libspdm_x509_get_extended_basic_constraints(const uint8_t *cert, size_t cert_size, uint8_t *basic_constraints,
                                                 size_t *basic_constraints_size) {
    (void)cert; (void)cert_size; (void)basic_constraints; (void)basic_constraints_size;
    return false;
}
bool libspdm_x509_set_date_time(const char *date_time_str, void *date_time, size_t *date_time_size) {
    (void)date_time_str; (void)date_time; (void)date_time_size;
    return false;
}
int32_t libspdm_x509_compare_date_time(const void *date_time1, const void *date_time2) {
    (void)date_time1; (void)date_time2;
    return -3;
}
