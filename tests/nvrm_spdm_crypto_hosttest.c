/* nvrm's libspdm cryptography (ports/nvidia/spdm_crypto.c) against published test vectors:
   FIPS 180 SHA-2 ("abc"), RFC 4231 HMAC, RFC 5869 HKDF, the GCM specification's AES-GCM cases,
   RFC 6979 ECDSA (P-256 and P-384), RFC 4648 base64; and the round trips of ECDH, RSA-PSS
   (checked by BearSSL's verifier) and X.509 signatures (certificates built here). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base.h>
#include "library/cryptlib.h"
#include "nvspdm_cryptlib_extensions.h"

#include <bearssl.h>

#include "nocturne.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

/* The entropy the code under test asks for: a fixed sequence keeps the run repeatable. */
n_result n_random(void *buf, size_t n) {
    static uint64_t state = 0x9E3779B97F4A7C15ull;
    unsigned char *p = buf;
    for (size_t i = 0; i < n; i++) {
        state ^= state << 13, state ^= state >> 7, state ^= state << 17;
        p[i] = (unsigned char)(state >> 24);
    }
    return 0;
}

static size_t unhex(const char *hex, uint8_t *out) {
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned v = 0;
        for (int i = 0; i < 2; i++) {
            char c = hex[i];
            v = v << 4 | (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        out[n++] = (uint8_t)v;
    }
    return n;
}

static bool same_hex(const uint8_t *data, size_t size, const char *hex) {
    uint8_t want[512];
    return unhex(hex, want) == size && !memcmp(data, want, size);
}

/* ---------------------------------------------------------------- SHA-2 */
static void test_sha(void) {
    uint8_t out[64];
    CHECK(libspdm_sha256_hash_all("abc", 3, out));
    CHECK(same_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(libspdm_sha384_hash_all("abc", 3, out));
    CHECK(same_hex(out, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                            "8086072ba1e7cc2358baeca134c825a7"));
    /* in two parts, through a duplicate */
    void *a = libspdm_sha256_new(), *b = libspdm_sha256_new();
    CHECK(a && b && libspdm_sha256_init(a) && libspdm_sha256_update(a, "a", 1));
    CHECK(libspdm_sha256_duplicate(a, b) && libspdm_sha256_update(b, "bc", 2) && libspdm_sha256_final(b, out));
    CHECK(same_hex(out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(!libspdm_sha256_update(a, NULL, 1));
    CHECK(libspdm_sha256_update(a, NULL, 0));
    CHECK(!libspdm_sha256_final(NULL, out));
    libspdm_sha256_free(a);
    libspdm_sha256_free(b);
}

/* ---------------------------------------------------------------- HMAC, RFC 4231 */
static void test_hmac(void) {
    uint8_t key[20], out[48];
    memset(key, 0x0b, sizeof key);
    CHECK(libspdm_hmac_sha256_all("Hi There", 8, key, 20, out));
    CHECK(same_hex(out, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK(libspdm_hmac_sha384_all("Hi There", 8, key, 20, out));
    CHECK(same_hex(out, 48, "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
                            "faea9ea9076ede7f4af152e8b2fa9cb6"));
    const char *data = "what do ya want for nothing?";
    void *m = libspdm_hmac_sha256_new(), *copy = libspdm_hmac_sha256_new();
    CHECK(m && copy && !libspdm_hmac_sha256_update(m, data, 4)); /* no key yet */
    CHECK(libspdm_hmac_sha256_set_key(m, (const uint8_t *)"Jefe", 4));
    CHECK(libspdm_hmac_sha256_update(m, data, 10) && libspdm_hmac_sha256_duplicate(m, copy));
    CHECK(libspdm_hmac_sha256_update(copy, data + 10, strlen(data) - 10) && libspdm_hmac_sha256_final(copy, out));
    CHECK(same_hex(out, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    libspdm_hmac_sha256_free(m);
    libspdm_hmac_sha256_free(copy);
    m = libspdm_hmac_sha384_new();
    CHECK(m && libspdm_hmac_sha384_set_key(m, (const uint8_t *)"Jefe", 4));
    CHECK(libspdm_hmac_sha384_update(m, data, strlen(data)) && libspdm_hmac_sha384_final(m, out));
    CHECK(same_hex(out, 48, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
                            "8e2240ca5e69e2c78b3239ecfab21649"));
    libspdm_hmac_sha384_free(m);
}

/* ---------------------------------------------------------------- HKDF, RFC 5869 */
static void test_hkdf(void) {
    uint8_t ikm[22], salt[13], info[10], prk[48], okm[82];
    memset(ikm, 0x0b, sizeof ikm);
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    CHECK(libspdm_hkdf_sha256_extract(ikm, 22, salt, 13, prk, 32));
    CHECK(same_hex(prk, 32, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
    CHECK(libspdm_hkdf_sha256_expand(prk, 32, info, 10, okm, 42));
    CHECK(same_hex(okm, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                            "34007208d5b887185865"));
    CHECK(!libspdm_hkdf_sha256_extract(ikm, 22, salt, 13, prk, 48)); /* the PRK is the hash's size */
    CHECK(!libspdm_hkdf_sha256_expand(prk, 32, info, 10, okm, 255 * 32 + 1));
    /* SHA-384: T(1) and T(2) by hand */
    CHECK(libspdm_hkdf_sha384_extract(ikm, 22, salt, 13, prk, 48));
    uint8_t t[48 + 10 + 1], want[96];
    memcpy(t, info, 10);
    t[10] = 1;
    CHECK(libspdm_hmac_sha384_all(t, 11, prk, 48, want));
    memcpy(t, want, 48);
    memcpy(t + 48, info, 10);
    t[58] = 2;
    CHECK(libspdm_hmac_sha384_all(t, 59, prk, 48, want + 48));
    CHECK(libspdm_hkdf_sha384_expand(prk, 48, info, 10, okm, 82) && !memcmp(okm, want, 82));
}

/* ---------------------------------------------------------------- AES-GCM (GCM spec, cases 4 and 16) */
static void test_gcm(void) {
    uint8_t key[32], iv[12], aad[20], plain[60], cipher[60], tag[16], back[60];
    unhex("cafebabefacedbaddecaf888", iv);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
          "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", plain);
    const struct { const char *key, *cipher, *tag; } cases[] = {
        {"feffe9928665731c6d6a8f9467308308",
         "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
         "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
         "5bc94fbc3221a5db94fae95ae7121a47"},
        {"feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
         "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
         "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
         "76fc6ece0f4e1768cddf8853bb2d551b"},
    };
    for (size_t c = 0; c < 2; c++) {
        size_t key_size = unhex(cases[c].key, key), out_size = sizeof cipher;
        CHECK(libspdm_aead_aes_gcm_encrypt(key, key_size, iv, 12, aad, 20, plain, 60, tag, 16, cipher, &out_size));
        CHECK(out_size == 60 && same_hex(cipher, 60, cases[c].cipher) && same_hex(tag, 16, cases[c].tag));
        out_size = sizeof back;
        CHECK(libspdm_aead_aes_gcm_decrypt(key, key_size, iv, 12, aad, 20, cipher, 60, tag, 16, back, &out_size));
        CHECK(out_size == 60 && !memcmp(back, plain, 60));
        /* a truncated tag (12 bytes) still authenticates */
        CHECK(libspdm_aead_aes_gcm_decrypt(key, key_size, iv, 12, aad, 20, cipher, 60, tag, 12, back, &out_size));
        /* a forged message is refused and nothing of it handed out */
        cipher[7] ^= 1;
        memset(back, 0x55, sizeof back);
        CHECK(!libspdm_aead_aes_gcm_decrypt(key, key_size, iv, 12, aad, 20, cipher, 60, tag, 16, back, &out_size));
        static const uint8_t zero[60];
        CHECK(!memcmp(back, zero, 60));
        cipher[7] ^= 1;
    }
    /* in place, through a preallocated context */
    void *ctx = NULL;
    CHECK(libspdm_aead_gcm_prealloc(&ctx) && ctx);
    size_t key_size = unhex(cases[0].key, key), out_size = 60;
    memcpy(back, plain, 60);
    CHECK(libspdm_aead_aes_gcm_encrypt_prealloc(ctx, key, key_size, iv, 12, aad, 20, back, 60, tag, 16, back, &out_size));
    CHECK(same_hex(back, 60, cases[0].cipher));
    CHECK(libspdm_aead_aes_gcm_decrypt_prealloc(ctx, key, key_size, iv, 12, aad, 20, back, 60, tag, 16, back, &out_size));
    CHECK(!memcmp(back, plain, 60));
    libspdm_aead_free(ctx);
    /* the original's argument checks */
    out_size = 60;
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 16, iv, 16, aad, 20, plain, 60, tag, 16, cipher, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 20, iv, 12, aad, 20, plain, 60, tag, 16, cipher, &out_size));
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 16, iv, 12, aad, 20, plain, 60, tag, 11, cipher, &out_size));
    out_size = 59;
    CHECK(!libspdm_aead_aes_gcm_encrypt(key, 16, iv, 12, aad, 20, plain, 60, tag, 16, cipher, &out_size));
}

/* ---------------------------------------------------------------- DER builder for the certificates */
struct der { uint8_t b[4096]; size_t n; };

static void put(struct der *d, const void *p, size_t n) { memcpy(d->b + d->n, p, n); d->n += n; }
static void put_tlv(struct der *d, uint8_t tag, const void *p, size_t n) {
    d->b[d->n++] = tag;
    if (n < 0x80) d->b[d->n++] = (uint8_t)n;
    else if (n < 0x100) { d->b[d->n++] = 0x81; d->b[d->n++] = (uint8_t)n; }
    else { d->b[d->n++] = 0x82; d->b[d->n++] = (uint8_t)(n >> 8); d->b[d->n++] = (uint8_t)n; }
    put(d, p, n);
}
static void wrap(struct der *d, uint8_t tag) {
    struct der inner = *d;
    d->n = 0;
    put_tlv(d, tag, inner.b, inner.n);
}

static const uint8_t oid_ec_public_key[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
static const uint8_t oid_p256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
static const uint8_t oid_p384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
static const uint8_t oid_ecdsa_sha384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};

static void name(struct der *out, const char *cn) {
    struct der d = {.n = 0};
    put_tlv(&d, 0x06, (const uint8_t[]){0x55, 0x04, 0x03}, 3);
    put_tlv(&d, 0x0C, cn, strlen(cn));
    wrap(&d, 0x30);
    wrap(&d, 0x31);
    wrap(&d, 0x30);
    put(out, d.b, d.n);
}

/* A v3 certificate of subject's public key, signed with ECDSA-SHA384 by signer. */
static size_t make_cert(uint8_t *out, const char *issuer, const char *subject, const br_ec_public_key *pub,
                        const br_ec_private_key *signer, bool corrupt) {
    struct der tbs = {.n = 0}, part;
    put_tlv(&tbs, 0xA0, (const uint8_t[]){0x02, 0x01, 0x02}, 3);
    put_tlv(&tbs, 0x02, (const uint8_t[]){0x01}, 1);
    part.n = 0;
    put_tlv(&part, 0x06, oid_ecdsa_sha384, sizeof oid_ecdsa_sha384);
    wrap(&part, 0x30);
    put(&tbs, part.b, part.n);
    name(&tbs, issuer);
    part.n = 0;
    put_tlv(&part, 0x17, "250101000000Z", 13);
    put_tlv(&part, 0x17, "350101000000Z", 13);
    wrap(&part, 0x30);
    put(&tbs, part.b, part.n);
    name(&tbs, subject);
    part.n = 0;
    struct der alg = {.n = 0};
    put_tlv(&alg, 0x06, oid_ec_public_key, sizeof oid_ec_public_key);
    if (pub->curve == BR_EC_secp256r1) put_tlv(&alg, 0x06, oid_p256, sizeof oid_p256);
    else put_tlv(&alg, 0x06, oid_p384, sizeof oid_p384);
    wrap(&alg, 0x30);
    put(&part, alg.b, alg.n);
    uint8_t bits[1 + 97] = {0};
    memcpy(bits + 1, pub->q, pub->qlen);
    put_tlv(&part, 0x03, bits, 1 + pub->qlen);
    wrap(&part, 0x30);
    put(&tbs, part.b, part.n);
    wrap(&tbs, 0x30);

    uint8_t digest[48], sig[1 + 160];
    br_sha384_context h;
    br_sha384_init(&h);
    br_sha384_update(&h, tbs.b, tbs.n);
    br_sha384_out(&h, digest);
    size_t sig_len = br_ecdsa_i31_sign_asn1(br_ec_get_default(), &br_sha384_vtable, digest, signer, sig + 1);
    sig[0] = 0;
    if (corrupt) sig[sig_len / 2] ^= 1;

    struct der cert = {.n = 0};
    put(&cert, tbs.b, tbs.n);
    part.n = 0;
    put_tlv(&part, 0x06, oid_ecdsa_sha384, sizeof oid_ecdsa_sha384);
    wrap(&part, 0x30);
    put(&cert, part.b, part.n);
    put_tlv(&cert, 0x03, sig, 1 + sig_len);
    wrap(&cert, 0x30);
    memcpy(out, cert.b, cert.n);
    return cert.n;
}

/* ---------------------------------------------------------------- ECDSA (RFC 6979 A.2.5, A.2.6), X.509 */
struct keypair { br_ec_private_key sk; br_ec_public_key pk; uint8_t x[48], q[97]; };

static void keypair(struct keypair *k, int curve, const char *private_hex) {
    k->sk.curve = curve;
    k->sk.x = k->x;
    k->sk.xlen = unhex(private_hex, k->x);
    br_ec_compute_pub(br_ec_get_default(), &k->pk, k->q, &k->sk);
}

static void test_ecdsa_x509(void) {
    struct keypair p256, p384;
    keypair(&p256, BR_EC_secp256r1, "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
    keypair(&p384, BR_EC_secp384r1, "6b9d3dad2e1b8c1c05b19875b6659f4de23c3b667bf297ba9aa47740787137d8"
                                    "96d5724e4c70a825f872c9ea60d2edf5");
    CHECK(same_hex(p256.q + 1, 64, "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
                                   "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"));

    uint8_t root[1024], leaf[1024], forged[1024], chain[2048];
    size_t root_len = make_cert(root, "root", "root", &p384.pk, &p384.sk, false);
    size_t leaf_len = make_cert(leaf, "root", "leaf", &p256.pk, &p384.sk, false);
    size_t forged_len = make_cert(forged, "root", "leaf", &p256.pk, &p384.sk, true);

    /* the public keys of the certificates, then the RFC 6979 signatures of "sample" */
    void *leaf_key = NULL, *root_key = NULL;
    CHECK(libspdm_ec_get_public_key_from_x509(leaf, leaf_len, &leaf_key) && leaf_key);
    CHECK(libspdm_ec_get_public_key_from_x509(root, root_len, &root_key) && root_key);
    uint8_t hash[48], sig[96];
    libspdm_sha256_hash_all("sample", 6, hash);
    unhex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
          "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8", sig);
    CHECK(libspdm_ecdsa_verify(leaf_key, LIBSPDM_CRYPTO_NID_SHA256, hash, 32, sig, 64));
    sig[10] ^= 1;
    CHECK(!libspdm_ecdsa_verify(leaf_key, LIBSPDM_CRYPTO_NID_SHA256, hash, 32, sig, 64));
    CHECK(!libspdm_ecdsa_verify(leaf_key, LIBSPDM_CRYPTO_NID_SHA384, hash, 32, sig, 64)); /* hash length */
    libspdm_sha384_hash_all("sample", 6, hash);
    unhex("94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c"
          "81a648152e44acf96e36dd1e80fabe46"
          "99ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94f"
          "a329c145786e679e7b82c71a38628ac8", sig);
    CHECK(libspdm_ecdsa_verify(root_key, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, 96));
    CHECK(!libspdm_ecdsa_verify(root_key, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, 64)); /* signature size */
    size_t sig_size = sizeof sig;
    CHECK(!libspdm_ecdsa_sign(root_key, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, &sig_size)); /* as the original */
    libspdm_ec_free(leaf_key);
    libspdm_ec_free(root_key);

    /* certificate signatures and chains */
    CHECK(libspdm_x509_verify_cert(root, root_len, root, root_len));
    CHECK(libspdm_x509_verify_cert(leaf, leaf_len, root, root_len));
    CHECK(!libspdm_x509_verify_cert(forged, forged_len, root, root_len));
    CHECK(!libspdm_x509_verify_cert(leaf, leaf_len, leaf, leaf_len)); /* not the leaf's own key */
    memcpy(chain, root, root_len);
    memcpy(chain + root_len, leaf, leaf_len);
    CHECK(libspdm_x509_verify_cert_chain(root, root_len, chain, root_len + leaf_len));
    memcpy(chain + root_len, forged, forged_len);
    CHECK(!libspdm_x509_verify_cert_chain(root, root_len, chain, root_len + forged_len));
    memcpy(chain + root_len, leaf, leaf_len);
    const uint8_t *cert;
    size_t cert_len;
    CHECK(libspdm_x509_get_cert_from_cert_chain(chain, root_len + leaf_len, 0, &cert, &cert_len) && cert == chain &&
          cert_len == root_len);
    CHECK(libspdm_x509_get_cert_from_cert_chain(chain, root_len + leaf_len, -1, &cert, &cert_len) &&
          cert == chain + root_len && cert_len == leaf_len);
    CHECK(libspdm_x509_get_cert_from_cert_chain(chain, root_len + leaf_len, 1, &cert, &cert_len) &&
          cert == chain + root_len);
    CHECK(!libspdm_x509_get_cert_from_cert_chain(chain, root_len + leaf_len, 2, &cert, &cert_len));

    /* ASN.1 (the DMTF contract): *ptr moves to the contents */
    uint8_t *p = root;
    size_t length;
    CHECK(libspdm_asn1_get_tag(&p, root + root_len, &length, LIBSPDM_CRYPTO_ASN1_SEQUENCE | LIBSPDM_CRYPTO_ASN1_CONSTRUCTED));
    CHECK(p == root + 4 && length == root_len - 4); /* 30 82 hi lo */
    uint8_t *q = p;
    CHECK(!libspdm_asn1_get_tag(&q, root + root_len, &length, LIBSPDM_CRYPTO_ASN1_INTEGER) && q == p);
}

/* ---------------------------------------------------------------- ECDH */
static void test_ecdh(void) {
    const size_t nids[] = {LIBSPDM_CRYPTO_NID_SECP256R1, LIBSPDM_CRYPTO_NID_SECP384R1};
    for (int i = 0; i < 2; i++) {
        void *a = libspdm_ec_new_by_nid(nids[i]), *b = libspdm_ec_new_by_nid(nids[i]);
        uint8_t pa[96], pb[96], ka[48], kb[48];
        size_t size = i ? 96 : 64, la = sizeof pa, lb = sizeof pb, ska = sizeof ka, skb = sizeof kb;
        CHECK(a && b && libspdm_ec_generate_key(a, pa, &la) && libspdm_ec_generate_key(b, pb, &lb));
        CHECK(la == size && lb == size && memcmp(pa, pb, size));
        CHECK(libspdm_ec_compute_key(a, pb, lb, ka, &ska) && libspdm_ec_compute_key(b, pa, la, kb, &skb));
        CHECK(ska == size / 2 && skb == size / 2 && !memcmp(ka, kb, size / 2));
        pb[5] ^= 1; /* no longer a point of the curve */
        CHECK(!libspdm_ec_compute_key(a, pb, lb, ka, &ska));
        CHECK(!libspdm_ec_compute_key(a, pb, lb - 1, ka, &ska));
        size_t small = size - 1;
        CHECK(!libspdm_ec_generate_key(a, pa, &small) && small == size);
        libspdm_ec_free(a);
        libspdm_ec_free(b);
    }
    CHECK(!libspdm_ec_new_by_nid(LIBSPDM_CRYPTO_NID_SHA256));
}

/* ---------------------------------------------------------------- RSA-PSS (3072 bits, as RM signs FINISH) */
static void test_rsa(void) {
    static uint8_t kpriv[BR_RSA_KBUF_PRIV_SIZE(3072)], kpub[BR_RSA_KBUF_PUB_SIZE(3072)], d[384];
    br_rsa_private_key sk;
    br_rsa_public_key pk;
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, "nvrm-rsa-test", 13);
    CHECK(br_rsa_i31_keygen(&rng.vtable, &sk, kpriv, &pk, kpub, 3072, 65537));
    size_t d_len = br_rsa_i31_compute_privexp(d, &sk, 65537);
    CHECK(d_len);
    void *rsa = libspdm_rsa_new();
    CHECK(rsa && libspdm_rsa_set_key(rsa, LIBSPDM_RSA_KEY_N, pk.n, pk.nlen));
    CHECK(libspdm_rsa_set_key(rsa, LIBSPDM_RSA_KEY_E, pk.e, pk.elen));
    CHECK(libspdm_rsa_set_key(rsa, LIBSPDM_RSA_KEY_D, d, d_len));
    uint8_t hash[48], sig[384];
    libspdm_sha384_hash_all("finish", 6, hash);
    size_t sig_size = sizeof sig;
    CHECK(libspdm_rsa_pss_sign(rsa, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, &sig_size) && sig_size == 384);
    CHECK(br_rsa_i31_pss_vrfy(sig, sig_size, &br_sha384_vtable, &br_sha384_vtable, hash, 48, &pk));
    hash[0] ^= 1;
    CHECK(!br_rsa_i31_pss_vrfy(sig, sig_size, &br_sha384_vtable, &br_sha384_vtable, hash, 48, &pk));
    sig_size = 100;
    CHECK(!libspdm_rsa_pss_sign(rsa, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, &sig_size) && sig_size == 384);
    CHECK(!libspdm_rsa_pss_sign(rsa, LIBSPDM_CRYPTO_NID_SHA256, hash, 32, sig, &sig_size));
    CHECK(!libspdm_rsa_pss_verify(rsa, LIBSPDM_CRYPTO_NID_SHA384, hash, 48, sig, 384)); /* as the original */
    libspdm_rsa_free(rsa);
}

/* ---------------------------------------------------------------- base64, RFC 4648 */
static void test_base64(void) {
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        uint8_t out[16];
        size_t n = sizeof out;
        CHECK(libspdm_encode_base64((const uint8_t *)plain[i], out, strlen(plain[i]), &n));
        CHECK(n == strlen(coded[i]) && !memcmp(out, coded[i], n));
        n = sizeof out;
        CHECK(libspdm_decode_base64((const uint8_t *)coded[i], out, strlen(coded[i]), &n));
        CHECK(n == strlen(plain[i]) && !memcmp(out, plain[i], n));
    }
    uint8_t out[16];
    size_t n = sizeof out;
    CHECK(libspdm_decode_base64((const uint8_t *)"Zm9v\nYmFy\n", out, 10, &n) && n == 6 && !memcmp(out, "foobar", 6));
    n = sizeof out;
    CHECK(libspdm_decode_base64((const uint8_t *)"-_8=", out, 4, &n) && n == 2 && out[0] == 0xfb && out[1] == 0xff);
    n = sizeof out;
    CHECK(!libspdm_decode_base64((const uint8_t *)"Zm9=v", out, 5, &n) && n == 0);
    n = 2;
    CHECK(!libspdm_encode_base64((const uint8_t *)"foo", out, 3, &n));
}

int main(void) {
    test_sha();
    test_hmac();
    test_hkdf();
    test_gcm();
    test_ecdsa_x509();
    test_ecdh();
    test_rsa();
    test_base64();
    uint8_t r[8];
    CHECK(libspdm_random_bytes(r, sizeof r) && libspdm_check_crypto_backend());
    CHECK(libspdm_x509_compare_date_time(NULL, NULL) == -3);
    if (failures) {
        printf("nvrm_spdm_crypto_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("nvrm_spdm_crypto_hosttest: all passed\n");
    return 0;
}
