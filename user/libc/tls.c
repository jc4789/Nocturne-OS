/* net_stream: TCP connections, optionally wrapped in TLS 1.3 (RFC 8446) or TLS 1.2 (RFC 5246).
   The protocol (handshakes, key schedules, record layer, resumption) is this file's; BearSSL gives
   the primitives: hashes, HMAC, the TLS 1.2 PRF and record ciphers, AES-GCM, ChaCha20-Poly1305,
   the elliptic curves, the RSA and ECDSA verifiers, and the X.509 chain validation
   (br_x509_minimal). Certificates are checked against the Mozilla root store compiled in by
   scripts/mkroots.py and the certificate authorities added to the system in
   N_FOLDER_CERTIFICATES (trust_anchors), and the current time comes from the CMOS clock. Entropy
   comes from the system entropy pool (n_random). */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <threads.h>
#include "bearssl.h"
#include "http.h"
#include "nocturne.h"

extern const br_x509_trust_anchor tls_trust_anchors[];
extern const size_t tls_trust_anchors_num;

/* The trust anchors: the compiled-in roots, then each certificate in the documents of
   N_FOLDER_CERTIFICATES (PEM with any number of CERTIFICATE blocks, or one DER certificate). A
   CA certificate anchors chains; any other is trusted as itself (BearSSL's direct trust, for a
   server's own self-signed certificate). Read once per app, at its first TLS connection; a
   certificate that does not decode is left out and the rest still count. */
struct der_bytes { unsigned char *data; size_t len, cap; bool failed; };
static void der_append(void *ctx, const void *buf, size_t len) {
    struct der_bytes *b = ctx;
    if (b->failed || !len) return;
    if (len > SIZE_MAX - b->len) { b->failed = true; return; }
    if (b->len + len > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + len) cap = cap > SIZE_MAX / 2 ? b->len + len : cap * 2;
        unsigned char *grown = realloc(b->data, cap);
        if (!grown) { b->failed = true; return; }
        b->data = grown; b->cap = cap;
    }
    memcpy(b->data + b->len, buf, len);
    b->len += len;
}
static unsigned char *blob_copy(const unsigned char *data, size_t len) {
    unsigned char *copy = malloc(len ? len : 1);
    if (copy) memcpy(copy, data, len);
    return copy;
}
static bool anchor_from_certificate(const unsigned char *der, size_t len, br_x509_trust_anchor *ta) {
    br_x509_decoder_context dc;
    struct der_bytes dn = {0};
    br_x509_decoder_init(&dc, der_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || dn.failed || !dn.len) { free(dn.data); return false; }
    *ta = (br_x509_trust_anchor){.dn = {dn.data, dn.len}, .flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0};
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = blob_copy(pk->key.rsa.n, pk->key.rsa.nlen);
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = blob_copy(pk->key.rsa.e, pk->key.rsa.elen);
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        if (ta->pkey.key.rsa.n && ta->pkey.key.rsa.e) return true;
        free(ta->pkey.key.rsa.n); free(ta->pkey.key.rsa.e);
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = blob_copy(pk->key.ec.q, pk->key.ec.qlen);
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        if (ta->pkey.key.ec.q) return true;
    }
    free(dn.data);
    return false;
}
struct anchor_list { br_x509_trust_anchor *items; size_t count, cap; };
static void anchor_add(struct anchor_list *list, const unsigned char *der, size_t len) {
    br_x509_trust_anchor ta;
    if (!anchor_from_certificate(der, len, &ta)) return;
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 8;
        br_x509_trust_anchor *grown = cap > SIZE_MAX / sizeof *grown ? NULL : realloc(list->items, cap * sizeof *grown);
        if (!grown) {
            free(ta.dn.data);
            if (ta.pkey.key_type == BR_KEYTYPE_RSA) { free(ta.pkey.key.rsa.n); free(ta.pkey.key.rsa.e); }
            else free(ta.pkey.key.ec.q);
            return;
        }
        list->items = grown; list->cap = cap;
    }
    list->items[list->count++] = ta;
}
static void anchors_from_document(struct anchor_list *list, const unsigned char *data, size_t len) {
    static const char begin[] = "-----BEGIN ";
    bool pem = false;
    for (size_t i = 0; i + sizeof begin - 1 <= len && !pem; i++) pem = !memcmp(data + i, begin, sizeof begin - 1);
    if (!pem) { anchor_add(list, data, len); return; }
    /* RFC 7468 §2: a line may end in CRLF, CR or LF. BearSSL's decoder takes LF only (a CR in
       the BEGIN line ends up in the label), so the copy it reads has LF alone. */
    unsigned char *lf = malloc(len);
    if (lf) {
        size_t n = 0;
        for (size_t i = 0; i < len; i++) {
            if (data[i] != '\r') lf[n++] = data[i];
            else if (i + 1 == len || data[i + 1] != '\n') lf[n++] = '\n';
        }
        data = lf; len = n;
    }
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    struct der_bytes object = {0};
    bool certificate = false;
    while (len) {
        size_t used = br_pem_decoder_push(&pc, data, len);
        data += used; len -= used;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ: {
            const char *name = br_pem_decoder_name(&pc);
            certificate = !strcmp(name, "CERTIFICATE") || !strcmp(name, "X509 CERTIFICATE");
            object.len = 0; object.failed = false;
            br_pem_decoder_setdest(&pc, certificate ? der_append : NULL, &object);
            break;
        }
        case BR_PEM_END_OBJ:
            if (certificate && !object.failed) anchor_add(list, object.data, object.len);
            certificate = false;
            break;
        case BR_PEM_ERROR:
            len = 0;
            break;
        }
    }
    free(object.data);
    free(lf);
}
static const br_x509_trust_anchor *anchors = tls_trust_anchors;
static size_t anchor_count;
static once_flag anchors_once = ONCE_FLAG_INIT;
static void anchors_read(void) {
    anchor_count = tls_trust_anchors_num;
    struct n_entry *entries = NULL;
    n_result count = n_folder_entries(N_FOLDER_CERTIFICATES, &entries);
    struct anchor_list added = {0};
    for (n_result i = 0; i < count; i++) {
        char path[4096];
        if (entries[i].kind == N_KIND_FOLDER || n_path_join(N_FOLDER_CERTIFICATES, entries[i].name, path, sizeof path) < 0)
            continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        struct der_bytes document = {0};
        unsigned char chunk[4096];
        size_t got;
        while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) der_append(&document, chunk, got);
        fclose(f);
        if (!document.failed) anchors_from_document(&added, document.data, document.len);
        free(document.data);
    }
    free(entries);
    if (!added.count) return;
    br_x509_trust_anchor *all = malloc((tls_trust_anchors_num + added.count) * sizeof *all);
    if (!all) return; /* the compiled-in roots still hold */
    memcpy(all, tls_trust_anchors, tls_trust_anchors_num * sizeof *all);
    memcpy(all + tls_trust_anchors_num, added.items, added.count * sizeof *all);
    free(added.items);
    anchors = all;
    anchor_count = tls_trust_anchors_num + added.count;
}
static const br_x509_trust_anchor *trust_anchors(size_t *count) {
    call_once(&anchors_once, anchors_read);
    *count = anchor_count;
    return anchors;
}

/* The resumption table's lock and key: session secrets are process-local, separated by host,
   port, ALPN use and partition, and bounded independently of socket lifetime. */
static mtx_t session_lock;
static void session_enter(void) { mtx_lock(&session_lock); }
static void session_leave(void) { mtx_unlock(&session_lock); }
static char *session_key(const char *host,uint16_t port,bool http,const char *partition) {
    size_t h=strlen(host),p=partition?strlen(partition):0;
    if(h>65536 || p>65536-h) return NULL; /* a cache budget, never a request limit */
    char *key=malloc(h+p+48);
    if(key)snprintf(key,h+p+48,"%zu:%s:%u:%u:%zu:%s",h,host,port,http,p,partition?partition:"");
    return key;
}

/* ---- the TLS client ----
   One ClientHello offers TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246); the ServerHello picks one.
   TLS 1.2 here: ECDHE (RFC 8422) or RSA key exchange, AES-GCM (RFC 5288), ChaCha20-Poly1305
   (RFC 7905) and AES-CBC with HMAC-SHA1 suites, renegotiation indication (RFC 5746, never
   renegotiating), extended master secret (RFC 7627), RSA-PSS signatures (RFC 8446 §4.2.3), and
   session-ID resumption. TLS 1.3 here: X25519 and the NIST curves (key shares for X25519 and
   P-256, HelloRetryRequest for the others), certificate authentication with RSA-PSS and ECDSA
   CertificateVerify, PSK resumption from NewSessionTicket (psk_dhe_ke), KeyUpdate, the
   middlebox compatibility mode (Appendix D.4) and the downgrade sentinels (§4.1.3).
   Not offered: 0-RTT, client certificates (a CertificateRequest gets an empty Certificate),
   post-handshake authentication, TLS 1.0/1.1, Ed25519/Ed448 signatures (br_x509_minimal decodes
   no such key and no WebPKI certificate carries one), the hybrid post-quantum groups (no
   ML-KEM in BearSSL). */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APP = 23 };
enum { HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4,
       HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11, HS_SERVER_KEY_EXCHANGE = 12,
       HS_CERTIFICATE_REQUEST = 13, HS_SERVER_HELLO_DONE = 14, HS_CERTIFICATE_VERIFY = 15,
       HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254,
       HS_CCS = 0x100 /* a TLS 1.2 ChangeCipherSpec, as hs_next reports it */ };
enum { AL_NONE = -1, AL_CLOSE_NOTIFY = 0, AL_UNEXPECTED_MESSAGE = 10, AL_BAD_RECORD_MAC = 20,
       AL_RECORD_OVERFLOW = 22, AL_HANDSHAKE_FAILURE = 40, AL_BAD_CERTIFICATE = 42,
       AL_UNSUPPORTED_CERTIFICATE = 43, AL_CERTIFICATE_EXPIRED = 45, AL_ILLEGAL_PARAMETER = 47,
       AL_UNKNOWN_CA = 48, AL_DECODE_ERROR = 50, AL_DECRYPT_ERROR = 51, AL_PROTOCOL_VERSION = 70,
       AL_INTERNAL_ERROR = 80, AL_USER_CANCELED = 90, AL_MISSING_EXTENSION = 109,
       AL_UNSUPPORTED_EXTENSION = 110 };
enum { EXT_SERVER_NAME = 0, EXT_SUPPORTED_GROUPS = 10, EXT_EC_POINT_FORMATS = 11,
       EXT_SIGNATURE_ALGORITHMS = 13, EXT_ALPN = 16, EXT_PADDING = 21, EXT_EXTENDED_MASTER_SECRET = 23,
       EXT_PRE_SHARED_KEY = 41, EXT_SUPPORTED_VERSIONS = 43, EXT_COOKIE = 44,
       EXT_PSK_KEY_EXCHANGE_MODES = 45, EXT_KEY_SHARE = 51, EXT_RENEGOTIATION_INFO = 0xff01 };
enum { GROUP_P256 = 23, GROUP_P384 = 24, GROUP_P521 = 25, GROUP_X25519 = 29 }; /* = BR_EC_* */
#define TLS12 0x0303u
#define TLS13 0x0304u
#define PLAIN_MAX 16384u
#define RECORD_IN_MAX (5u + PLAIN_MAX + 2048u)
#define WBUF_SIZE (5u + PLAIN_MAX + 2048u + 512u)
/* A guard against a peer's absurd handshake message, not a limit sites meet: a certificate
   chain is a few kilobytes and OpenSSL and BoringSSL refuse much less than this. */
#define HANDSHAKE_MESSAGE_MAX (1u << 20)

enum { KX_TLS13, KX_ECDHE_RSA, KX_ECDHE_ECDSA, KX_RSA };
enum { C_AES128_GCM, C_AES256_GCM, C_CHACHA20, C_AES128_CBC, C_AES256_CBC };
struct suite { uint16_t id, version; uint8_t kx, cipher, key_len, hash_len; };
/* The offer, in order of preference: AEAD and forward secrecy first, then the legacy suites a
   TLS 1.2-only server may still need (RFC 9113 §9.2.2 keeps those from HTTP/2). */
static const struct suite suites[] = {
    {0x1301, TLS13, KX_TLS13, C_AES128_GCM, 16, 32},
    {0x1302, TLS13, KX_TLS13, C_AES256_GCM, 32, 48},
    {0x1303, TLS13, KX_TLS13, C_CHACHA20, 32, 32},
    {0xc02b, TLS12, KX_ECDHE_ECDSA, C_AES128_GCM, 16, 32},
    {0xc02f, TLS12, KX_ECDHE_RSA, C_AES128_GCM, 16, 32},
    {0xc02c, TLS12, KX_ECDHE_ECDSA, C_AES256_GCM, 32, 48},
    {0xc030, TLS12, KX_ECDHE_RSA, C_AES256_GCM, 32, 48},
    {0xcca9, TLS12, KX_ECDHE_ECDSA, C_CHACHA20, 32, 32},
    {0xcca8, TLS12, KX_ECDHE_RSA, C_CHACHA20, 32, 32},
    {0xc009, TLS12, KX_ECDHE_ECDSA, C_AES128_CBC, 16, 32},
    {0xc013, TLS12, KX_ECDHE_RSA, C_AES128_CBC, 16, 32},
    {0xc00a, TLS12, KX_ECDHE_ECDSA, C_AES256_CBC, 32, 32},
    {0xc014, TLS12, KX_ECDHE_RSA, C_AES256_CBC, 32, 32},
    {0x009c, TLS12, KX_RSA, C_AES128_GCM, 16, 32},
    {0x009d, TLS12, KX_RSA, C_AES256_GCM, 32, 48},
    {0x002f, TLS12, KX_RSA, C_AES128_CBC, 16, 32},
    {0x0035, TLS12, KX_RSA, C_AES256_CBC, 32, 32},
};
#define SUITE_COUNT (sizeof suites / sizeof *suites)
static const struct suite *suite_of(uint16_t id) {
    for (size_t i = 0; i < SUITE_COUNT; i++) if (suites[i].id == id) return &suites[i];
    return NULL;
}
/* signature_algorithms (RFC 8446 §4.2.3) for the server's handshake signature, without SHA-1 or
   MD5 (RFC 9155 §2). The PKCS#1 v1.5 schemes count in a TLS 1.2 ServerKeyExchange only, never in a
   TLS 1.3 CertificateVerify; the certificate chain's own signatures are br_x509_minimal's. */
static const uint16_t sigalgs[] = {0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601, 0x0603};
static const uint16_t groups[] = {GROUP_X25519, GROUP_P256, GROUP_P384, GROUP_P521};
static bool offered_group(unsigned g) { for (size_t i = 0; i < 4; i++) if (groups[i] == g) return true; return false; }
static bool offered_sigalg(unsigned a) {
    for (size_t i = 0; i < sizeof sigalgs / sizeof *sigalgs; i++) if (sigalgs[i] == a) return true;
    return false;
}
static size_t point_len(unsigned group) {
    return group == GROUP_X25519 ? 32 : group == GROUP_P256 ? 65 : group == GROUP_P384 ? 97 : group == GROUP_P521 ? 133 : 0;
}
static const br_hash_class *hash_of(size_t len) { return len == 48 ? &br_sha384_vtable : &br_sha256_vtable; }

static void wipe(void *p, size_t n) { volatile unsigned char *v = p; while (n--) *v++ = 0; }
static bool same_bytes(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b; unsigned d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be24(const unsigned char *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }

/* ---- HMAC, HKDF (RFC 5869) and HKDF-Expand-Label (RFC 8446 §7.1) ---- */
static void hmac(const br_hash_class *h, const void *key, size_t key_len, const void *a, size_t a_len,
                 const void *b, size_t b_len, unsigned char *out) {
    br_hmac_key_context kc; br_hmac_context hc;
    br_hmac_key_init(&kc, h, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    if (a_len) br_hmac_update(&hc, a, a_len);
    if (b_len) br_hmac_update(&hc, b, b_len);
    br_hmac_out(&hc, out);
    wipe(&kc, sizeof kc); wipe(&hc, sizeof hc);
}
static void hkdf_extract(size_t hl, const unsigned char *salt, const unsigned char *ikm, size_t ikm_len, unsigned char *out) {
    hmac(hash_of(hl), salt, hl, ikm, ikm_len, NULL, 0, out);
}
static void expand_label(size_t hl, const unsigned char *secret, const char *label, const unsigned char *context,
                         size_t context_len, unsigned char *out, size_t out_len) {
    unsigned char info[2 + 1 + 255 + 1 + 255], t[64];
    size_t ll = 6 + strlen(label), n = 0;
    info[n++] = (unsigned char)(out_len >> 8); info[n++] = (unsigned char)out_len;
    info[n++] = (unsigned char)ll; memcpy(info + n, "tls13 ", 6); memcpy(info + n + 6, label, ll - 6); n += ll;
    info[n++] = (unsigned char)context_len; if (context_len) memcpy(info + n, context, context_len); n += context_len;
    const br_hash_class *h = hash_of(hl);
    size_t done = 0, tl = 0;
    for (unsigned char i = 1; done < out_len; i++) {
        br_hmac_key_context kc; br_hmac_context hc;
        br_hmac_key_init(&kc, h, secret, hl); br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, t, tl); br_hmac_update(&hc, info, n); br_hmac_update(&hc, &i, 1);
        br_hmac_out(&hc, t); tl = hl;
        size_t k = out_len - done < hl ? out_len - done : hl;
        memcpy(out + done, t, k); done += k;
        wipe(&kc, sizeof kc); wipe(&hc, sizeof hc);
    }
    wipe(t, sizeof t);
}
static void hash_bytes(const br_hash_class *h, const void *data, size_t len, unsigned char *out) {
    br_hash_compat_context c; h->init(&c.vtable); if (len) h->update(&c.vtable, data, len); h->out(&c.vtable, out);
}
/* Derive-Secret with the transcript hash given. */
static void derive_secret(size_t hl, const unsigned char *secret, const char *label, const unsigned char *th, unsigned char *out) {
    expand_label(hl, secret, label, th, hl, out, hl);
}

/* ---- the transcript (RFC 8446 §4.4.1; RFC 5246 §7.4.9): SHA-256 and SHA-384 run side by side
   until the cipher suite says which one counts ---- */
struct transcript { br_sha256_context h256; br_sha384_context h384; };
static void th_init(struct transcript *t) { br_sha256_init(&t->h256); br_sha384_init(&t->h384); }
static void th_add(struct transcript *t, const void *p, size_t n) { br_sha256_update(&t->h256, p, n); br_sha384_update(&t->h384, p, n); }
static void th_hash(const struct transcript *t, size_t hl, unsigned char *out) {
    if (hl == 48) br_sha384_out(&t->h384, out); else br_sha256_out(&t->h256, out);
}

/* ---- record protection ---- */
static const br_block_ctr_class *aes_ctr(void) { const br_block_ctr_class *c = br_aes_x86ni_ctr_get_vtable(); return c ? c : &br_aes_ct64_ctr_vtable; }
static const br_block_cbcenc_class *aes_cbcenc(void) { const br_block_cbcenc_class *c = br_aes_x86ni_cbcenc_get_vtable(); return c ? c : &br_aes_ct64_cbcenc_vtable; }
static const br_block_cbcdec_class *aes_cbcdec(void) { const br_block_cbcdec_class *c = br_aes_x86ni_cbcdec_get_vtable(); return c ? c : &br_aes_ct64_cbcdec_vtable; }
static br_ghash ghash(void) { br_ghash g = br_ghash_pclmul_get(); return g ? g : &br_ghash_ctmul64; }
static br_chacha20_run chacha(void) { br_chacha20_run c = br_chacha20_sse2_get(); return c ? c : &br_chacha20_ct_run; }
static br_poly1305_run poly(void) { br_poly1305_run p = br_poly1305_ctmulq_get(); return p ? p : &br_poly1305_ctmul_run; }

/* TLS 1.3 (§5.2, §5.3): AEAD over the TLSInnerPlaintext, the record header as additional data,
   the nonce the IV XOR the sequence number. */
struct aead13 {
    unsigned cipher;
    br_aes_gen_ctr_keys aes;
    br_ghash gh;
    unsigned char key[32], iv[12];
    uint64_t seq;
};
static void aead13_init(struct aead13 *a, const struct suite *su, const unsigned char *secret) {
    unsigned char key[32];
    expand_label(su->hash_len, secret, "key", NULL, 0, key, su->key_len);
    expand_label(su->hash_len, secret, "iv", NULL, 0, a->iv, 12);
    a->cipher = su->cipher; a->seq = 0;
    if (su->cipher == C_CHACHA20) memcpy(a->key, key, 32);
    else { aes_ctr()->init(&a->aes.vtable, key, su->key_len); a->gh = ghash(); }
    wipe(key, sizeof key);
}
static void aead13_nonce(const struct aead13 *a, unsigned char nonce[12]) {
    memcpy(nonce, a->iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= (unsigned char)(a->seq >> (56 - 8 * i));
}
static void aead13_seal(struct aead13 *a, const unsigned char header[5], unsigned char *data, size_t len) {
    unsigned char nonce[12]; aead13_nonce(a, nonce);
    if (a->cipher == C_CHACHA20) poly()(a->key, nonce, data, len, header, 5, data + len, chacha(), 1);
    else {
        br_gcm_context g; br_gcm_init(&g, &a->aes.vtable, a->gh); br_gcm_reset(&g, nonce, 12);
        br_gcm_aad_inject(&g, header, 5); br_gcm_flip(&g); br_gcm_run(&g, 1, data, len); br_gcm_get_tag(&g, data + len);
    }
    a->seq++;
}
static bool aead13_open(struct aead13 *a, const unsigned char header[5], unsigned char *data, size_t len) {
    unsigned char nonce[12]; aead13_nonce(a, nonce);
    bool ok;
    if (a->cipher == C_CHACHA20) {
        unsigned char tag[16];
        poly()(a->key, nonce, data, len - 16, header, 5, tag, chacha(), 0);
        ok = same_bytes(tag, data + len - 16, 16);
    } else {
        br_gcm_context g; br_gcm_init(&g, &a->aes.vtable, a->gh); br_gcm_reset(&g, nonce, 12);
        br_gcm_aad_inject(&g, header, 5); br_gcm_flip(&g); br_gcm_run(&g, 0, data, len - 16);
        ok = br_gcm_check_tag(&g, data + len - 16) != 0;
    }
    a->seq++;
    return ok;
}
enum { PROTECT_NONE, PROTECT_TLS12, PROTECT_TLS13 };
struct reader {
    unsigned mode;
    union { const br_sslrec_in_class *vtable; br_sslrec_in_cbc_context cbc; br_sslrec_gcm_context gcm;
            br_sslrec_chapol_context chapol; } r12;
    struct aead13 a13;
    unsigned char secret[48];
};
struct writer {
    unsigned mode;
    union { const br_sslrec_out_class *vtable; br_sslrec_out_cbc_context cbc; br_sslrec_gcm_context gcm;
            br_sslrec_chapol_context chapol; } r12;
    struct aead13 a13;
    unsigned char secret[48];
};

/* ---- resumption state: TLS 1.3 tickets (single use, RFC 8446 §C.4) and TLS 1.2 sessions,
   process-local, under the same key as before (host, port, ALPN use, partition) ---- */
struct resumption {
    char *key;
    uint64_t saved;
    uint16_t version, suite;
    uint32_t lifetime, age_add;          /* TLS 1.3 */
    unsigned char secret[48];            /* TLS 1.3: the PSK; TLS 1.2: the master secret */
    size_t secret_len;
    unsigned char *ticket; size_t ticket_len;
    unsigned char session_id[32]; size_t session_id_len; bool ems; /* TLS 1.2 */
};
#define RESUME_SLOTS 32
#define TICKETS_PER_KEY 4
static struct resumption resumes[RESUME_SLOTS];
static void resume_free(struct resumption *r) { free(r->key); free(r->ticket); wipe(r, sizeof *r); }
static bool resume_live(const struct resumption *r, uint64_t now) {
    if (!r->key) return false;
    uint64_t age = now - r->saved;
    if (r->version == TLS13) return age < (uint64_t)r->lifetime * 1000u && age < 604800000u;
    return age < 1800000u;
}
/* Moves out the newest live TLS 1.3 ticket for key. */
static bool resume_take13(const char *key, struct resumption *out) {
    if (!key) return false;
    uint64_t now = uptime_ms(); int best = -1;
    session_enter();
    for (int i = 0; i < RESUME_SLOTS; i++) {
        struct resumption *r = &resumes[i];
        if (r->key && !strcmp(r->key, key) && r->version == TLS13) {
            if (!resume_live(r, now)) { resume_free(r); continue; }
            if (best < 0 || r->saved > resumes[best].saved) best = i;
        }
    }
    if (best >= 0) { *out = resumes[best]; memset(&resumes[best], 0, sizeof resumes[best]); }
    session_leave();
    return best >= 0;
}
/* A copy of key's TLS 1.2 session (session IDs are reused, tickets are not). */
static bool resume_get12(const char *key, struct resumption *out) {
    if (!key) return false;
    uint64_t now = uptime_ms(); bool found = false;
    session_enter();
    for (int i = 0; i < RESUME_SLOTS && !found; i++) {
        struct resumption *r = &resumes[i];
        if (r->key && !strcmp(r->key, key) && r->version == TLS12) {
            if (!resume_live(r, now)) { resume_free(r); continue; }
            *out = *r; out->key = NULL; out->ticket = NULL; found = true;
        }
    }
    session_leave();
    return found;
}
static void resume_drop12(const char *key) {
    if (!key) return;
    session_enter();
    for (int i = 0; i < RESUME_SLOTS; i++)
        if (resumes[i].key && !strcmp(resumes[i].key, key) && resumes[i].version == TLS12) resume_free(&resumes[i]);
    session_leave();
}
/* Keeps r (its key and ticket become the table's): a TLS 1.2 session replaces the key's earlier
   one, a key keeps TICKETS_PER_KEY tickets, and a full table loses its oldest entry. */
static void resume_put(struct resumption *r) {
    session_enter();
    int slot = -1, oldest = 0, same_oldest = -1, same = 0;
    for (int i = 0; i < RESUME_SLOTS; i++) {
        struct resumption *e = &resumes[i];
        if (!e->key) { if (slot < 0) slot = i; continue; }
        if (!strcmp(e->key, r->key) && e->version == r->version) {
            if (r->version == TLS12) { slot = i; break; }
            same++;
            if (same_oldest < 0 || e->saved < resumes[same_oldest].saved) same_oldest = i;
        }
        if (e->saved < resumes[oldest].saved) oldest = i;
    }
    if (r->version == TLS13 && same >= TICKETS_PER_KEY) slot = same_oldest;
    if (slot < 0) slot = oldest;
    resume_free(&resumes[slot]);
    resumes[slot] = *r;
    memset(r, 0, sizeof *r);
    session_leave();
}
void ns_sessions_clear(void) {
    session_enter();
    for (int i = 0; i < RESUME_SLOTS; i++) resume_free(&resumes[i]);
    session_leave();
}

/* ---- a reader of encoded vectors ---- */
struct cursor { const unsigned char *p; size_t n; bool bad; };
static unsigned c8(struct cursor *c) { if (c->n < 1) { c->bad = true; return 0; } c->n--; return *c->p++; }
static unsigned c16(struct cursor *c) { if (c->n < 2) { c->bad = true; c->n = 0; return 0; } unsigned v = be16(c->p); c->p += 2; c->n -= 2; return v; }
static uint32_t c24(struct cursor *c) { if (c->n < 3) { c->bad = true; c->n = 0; return 0; } uint32_t v = be24(c->p); c->p += 3; c->n -= 3; return v; }
static uint32_t c32(struct cursor *c) { if (c->n < 4) { c->bad = true; c->n = 0; return 0; } uint32_t v = be32(c->p); c->p += 4; c->n -= 4; return v; }
static const unsigned char *cbytes(struct cursor *c, size_t n) {
    if (c->n < n) { c->bad = true; c->n = 0; return NULL; }
    const unsigned char *p = c->p; c->p += n; c->n -= n; return p;
}
static struct cursor csub(struct cursor *c, size_t n) {
    const unsigned char *p = cbytes(c, n);
    return p ? (struct cursor){p, n, false} : (struct cursor){NULL, 0, true};
}
static struct cursor cvec8(struct cursor *c) { return csub(c, c8(c)); }
static struct cursor cvec16(struct cursor *c) { return csub(c, c16(c)); }
static struct cursor cvec24(struct cursor *c) { return csub(c, c24(c)); }

/* ---- a writer of encoded messages ---- */
struct out { unsigned char *p; size_t len, cap; bool failed; };
static void o_put(struct out *o, const void *d, size_t n) {
    if (o->failed) return;
    if (n > o->cap - o->len) {
        size_t cap = o->cap ? o->cap : 1024;
        while (cap - o->len < n) { if (cap > SIZE_MAX / 2) { o->failed = true; return; } cap *= 2; }
        unsigned char *grown = realloc(o->p, cap);
        if (!grown) { o->failed = true; return; }
        o->p = grown; o->cap = cap;
    }
    if (n) memcpy(o->p + o->len, d, n);
    o->len += n;
}
static void o_u8(struct out *o, unsigned v) { unsigned char b = (unsigned char)v; o_put(o, &b, 1); }
static void o_u16(struct out *o, unsigned v) { unsigned char b[2] = {(unsigned char)(v >> 8), (unsigned char)v}; o_put(o, b, 2); }
static void o_u24(struct out *o, uint32_t v) { unsigned char b[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v}; o_put(o, b, 3); }
static void o_u32(struct out *o, uint32_t v) { o_u16(o, v >> 16); o_u16(o, v & 0xffff); }
/* A length prefix to fill in later: o_open returns where it starts. */
static size_t o_open(struct out *o, size_t width) { size_t at = o->len; o_put(o, "\0\0\0", width); return at; }
static void o_close(struct out *o, size_t at, size_t width) {
    if (o->failed) return;
    size_t n = o->len - at - width;
    for (size_t i = 0; i < width; i++) o->p[at + i] = (unsigned char)(n >> (8 * (width - 1 - i)));
}
static void o_free(struct out *o) { if (o->p) wipe(o->p, o->cap); free(o->p); memset(o, 0, sizeof *o); }

/* ---- the stream ---- */
struct share { unsigned group; size_t priv_len, pub_len; unsigned char priv[BR_EC_KBUF_PRIV_MAX_SIZE], pub[BR_EC_KBUF_PUB_MAX_SIZE]; };
/* What one handshake needs, freed when it ends. */
struct handshake {
    struct transcript th;
    br_hmac_drbg_context rng;
    br_x509_minimal_context xc;
    const br_x509_pkey *pkey;
    unsigned usages;
    char name[256];                 /* the host for server_name and the certificate, no trailing dot */
    bool sni, hrr, ccs_sent, cert_request, psk_offered, sess12_offered, ems;
    const struct suite *hrr_suite;
    unsigned char client_random[32], server_random[32];
    unsigned char session_id[32]; size_t session_id_len;
    struct share shares[2]; size_t share_count;
    unsigned char *cookie; size_t cookie_len;
    unsigned char context[255]; size_t context_len; /* a TLS 1.3 CertificateRequest's */
    struct resumption psk, sess12;
    unsigned char shared[66]; size_t shared_len;
    unsigned char c_hs[48], s_hs[48], master[48];
    unsigned char keyblock[2 * (20 + 32 + 12)];
    struct out msg;
};

struct net_stream {
    n_handle sock;
    int timeout_ms;
    bool tls;
    bool io_limited, handshaking, closing, transport_eof;
    int transport_errno;
    uint64_t io_deadline;
    uint64_t bytes_sent;
    struct ns_connection_info connection;
    /* Fetch's connection timing info, recorded as the connection opened. */
    struct http_connection_timing timing_info;
    int (*cancelled)(void *);
    void *cancel_ctx;
    char *session_key;
    bool http_alpn, h2, resumed;
    struct http_transport_timing *timing;
    char err[128];
    /* TLS only: a cleartext stream is allocated without the fields from here on. */
    uint16_t version, wr_version;   /* negotiated (0 before the ServerHello); the record header's */
    const struct suite *suite;
    bool failed;                    /* fatal: no more records either way */
    bool peer_closed, close_sent, peer_finished, ku_due;
    int fail_errno;
    struct handshake *hs;           /* while handshaking */
    unsigned char *hsbuf; size_t hs_len, hs_cap, hs_used; /* handshake bytes received, hs_used of them taken */
    unsigned char *app; size_t app_len, app_record;      /* plaintext not yet read, in its record in rbuf */
    unsigned char res_master[48];   /* TLS 1.3 resumption_master_secret */
    struct reader rd;
    struct writer wr;
    size_t rlen, wstart, wlen;
    unsigned char rbuf[RECORD_IN_MAX]; /* received bytes, a record at the start */
    unsigned char wbuf[WBUF_SIZE];     /* sealed records, [wstart, wlen) not yet sent */
};
#define TLS_FIELDS offsetof(net_stream, version)
#define REKEY_RECORDS (1ull << 24) /* RFC 8446 §5.5: well inside the AES-GCM limit */

static bool io_cancelled(net_stream *s) {
    if(!s->cancelled||!s->cancelled(s->cancel_ctx))return false;
    s->transport_errno=errno=ECANCELED;
    snprintf(s->err,sizeof s->err,"request cancelled");return true;
}
/* The tcp_* calls return a negative n_result; the ns_* functions report it to
   their callers in errno. A zero-byte transfer means the peer reset. */
static n_result transport_failed(net_stream *s, n_result r) {
    if(r>=0)r=-N_ERR_CONNECTION_RESET;
    s->transport_errno=errno=(int)-r;
    return r;
}

/* The timeout is per public read operation, not per encrypted record. A
   peer dribbling partial records must not renew one blocking operation. */
static void io_begin(net_stream *s, int timeout_ms) {
    s->io_limited=timeout_ms>0; /* zero remains a nonblocking socket read */
    if(s->io_limited) {
        uint64_t now=uptime_ms(), delay=(uint64_t)timeout_ms;
        s->io_deadline=delay>UINT64_MAX-now?UINT64_MAX:now+delay;
    }
}
static int io_timeout(net_stream *s, int *timeout) {
    if(io_cancelled(s))return -1;
    if(!s->io_limited){*timeout=s->timeout_ms;return 0;}
    uint64_t now=uptime_ms();
    if(now>=s->io_deadline) {
        s->transport_errno=ETIMEDOUT;errno=ETIMEDOUT;
        snprintf(s->err,sizeof s->err,"%s timed out",
                 s->handshaking?"TLS handshake":s->closing?"TLS close":"read");
        return -1;
    }
    uint64_t left=s->io_deadline-now;
    *timeout=left>(uint64_t)INT_MAX?INT_MAX:(int)left;
    return 0;
}
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    net_stream *s = ctx;
    uint64_t callback_started=s->handshaking&&s->timing?uptime_ms():0;
    int timeout;
    if(io_timeout(s,&timeout)<0){
        if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
        return -1;
    }
    n_result r;
    do {
        int slice=s->cancelled&&(timeout<0||timeout>100)?100:timeout;
        r=tcp_recv(s->sock,buf,len,slice);
        if(r>=0||(r!=-N_ERR_NOT_READY&&r!=-N_ERR_TIMEOUT)||slice==timeout)break;
        if(io_timeout(s,&timeout)<0)return -1;
    }while(1);
    if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
    if (r > 0)return (int)r;
    s->transport_errno=r<0?(int)-r:0;s->transport_eof=r==0;
    if(r<0)errno=s->transport_errno;
    if(r==0)snprintf(s->err,sizeof s->err,"peer closed connection%s",
                     s->handshaking?" during TLS handshake":"");
    else snprintf(s->err,sizeof s->err,"%s%s",
                  s->handshaking?"TLS handshake: ":"",
                  r==-N_ERR_NOT_READY?"timed out":n_result_text(r));
    return -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    net_stream *s = ctx;
    uint64_t callback_started=s->handshaking&&s->timing?uptime_ms():0;
    int timeout;
    if(io_timeout(s,&timeout)<0){
        if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
        return -1;
    }
    n_result r;
    do {
        uint64_t deadline=s->io_limited?s->io_deadline:0;
        if(s->cancelled){uint64_t slice=uptime_ms()+100;if(!deadline||slice<deadline)deadline=slice;}
        r=tcp_send_until(s->sock,buf,len,deadline);
        if(r>=0||r!=-N_ERR_TIMEOUT||!s->cancelled)break;
        if(io_timeout(s,&timeout)<0)return -1;
    }while(1);
    if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
    if (r > 0){s->bytes_sent+=(uint64_t)r;return (int)r;}
    s->transport_errno=r<0?(int)-r:0;s->transport_eof=r==0;
    if(r<0)errno=s->transport_errno;
    snprintf(s->err,sizeof s->err,"%s",r==0?"connection closed while sending":n_result_text(r));
    return -1;
}

/* ---- failures ---- */
static long failed_result(net_stream *s) {
    errno = s->fail_errno ? s->fail_errno : EPROTO;
    return -1;
}
/* A transport failure (sock_read and sock_write wrote the message): the stream is done. */
static int transport_lost(net_stream *s) {
    s->failed = true;
    s->fail_errno = errno = s->transport_errno ? s->transport_errno : EPROTO;
    return -1;
}
static bool rec_seal(net_stream *s, unsigned type, const unsigned char *data, size_t len);
/* Sends what the queue holds without waiting, errors ignored: the last records of a failed stream. */
static void out_push(net_stream *s) {
    while (s->wstart < s->wlen) {
        n_result got = tcp_send_some(s->sock, s->wbuf + s->wstart, s->wlen - s->wstart);
        if (got <= 0) return;
        s->bytes_sent += (uint64_t)got; s->wstart += (size_t)got;
    }
}
/* A protocol failure: the message, errno, and a fatal alert (RFC 8446 §6.2, RFC 5246 §7.2.2)
   unless alert is AL_NONE or the stream already failed. Returns -1. */
__attribute__((format(printf, 4, 5)))
static int tls_fail(net_stream *s, int alert, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vsnprintf(s->err, sizeof s->err, fmt, ap); va_end(ap);
    s->fail_errno = errno = err;
    if (alert != AL_NONE && !s->failed && !s->close_sent) {
        unsigned char a[2] = {2, (unsigned char)alert};
        if (rec_seal(s, CT_ALERT, a, 2)) { s->close_sent = true; out_push(s); }
    }
    s->failed = true;
    return -1;
}
#define FAIL(s, alert, ...) tls_fail(s, alert, EPROTO, __VA_ARGS__)

/* ---- the record layer (RFC 8446 §5, RFC 5246 §6) ---- */
static void read13(net_stream *s, const unsigned char *secret) {
    memcpy(s->rd.secret, secret, s->suite->hash_len);
    aead13_init(&s->rd.a13, s->suite, secret);
    s->rd.mode = PROTECT_TLS13;
}
static void write13(net_stream *s, const unsigned char *secret) {
    memcpy(s->wr.secret, secret, s->suite->hash_len);
    aead13_init(&s->wr.a13, s->suite, secret);
    s->wr.mode = PROTECT_TLS13;
}
/* One record of at most PLAIN_MAX bytes, sealed under the write protection onto the queue;
   false when the queue lacks the room (the caller sends the queue first). */
static bool rec_seal(net_stream *s, unsigned type, const unsigned char *data, size_t len) {
    if (len > PLAIN_MAX) return false;
    if (s->wstart == s->wlen) s->wstart = s->wlen = 0;
    size_t need = 5 + len + 160; /* the header, the largest MAC, IV, padding or tag, and room */
    if (WBUF_SIZE - s->wlen < need && s->wstart) {
        memmove(s->wbuf, s->wbuf + s->wstart, s->wlen - s->wstart);
        s->wlen -= s->wstart; s->wstart = 0;
    }
    size_t room = WBUF_SIZE - s->wlen;
    if (room < need) return false;
    unsigned char *r = s->wbuf + s->wlen;
    if (s->wr.mode == PROTECT_NONE) {
        r[0] = (unsigned char)type; r[1] = (unsigned char)(s->wr_version >> 8); r[2] = (unsigned char)s->wr_version;
        r[3] = (unsigned char)(len >> 8); r[4] = (unsigned char)len;
        if (len) memcpy(r + 5, data, len);
        s->wlen += 5 + len;
    } else if (s->wr.mode == PROTECT_TLS13) {
        /* TLSInnerPlaintext: the content, its type, no padding (§5.2, §5.4) */
        size_t n = len + 1 + 16;
        r[0] = CT_APP; r[1] = 3; r[2] = 3; r[3] = (unsigned char)(n >> 8); r[4] = (unsigned char)n;
        if (len) memcpy(r + 5, data, len);
        r[5 + len] = (unsigned char)type;
        aead13_seal(&s->wr.a13, r, r + 5, len + 1);
        s->wlen += 5 + n;
    } else {
        size_t a = 5, b = room;
        s->wr.r12.vtable->max_plaintext(&s->wr.r12.vtable, &a, &b);
        if (b < a || b - a < len) return false;
        if (len) memcpy(r + a, data, len);
        size_t n = len;
        unsigned char *rec = s->wr.r12.vtable->encrypt(&s->wr.r12.vtable, (int)type, TLS12, r + a, &n);
        if (rec != r) memmove(r, rec, n);
        s->wlen += n;
    }
    return true;
}
/* Sends the queue, waiting under the operation's deadline. */
static int out_flush(net_stream *s) {
    while (s->wstart < s->wlen) {
        int r = sock_write(s, s->wbuf + s->wstart, s->wlen - s->wstart);
        if (r < 0) return transport_lost(s);
        s->wstart += (size_t)r;
    }
    s->wstart = s->wlen = 0;
    return 0;
}
/* Sends what the transport takes now: 1 the queue is empty, 0 some is still queued, -1 failed. */
static int out_flush_some(net_stream *s) {
    while (s->wstart < s->wlen) {
        n_result got = tcp_send_some(s->sock, s->wbuf + s->wstart, s->wlen - s->wstart);
        if (got == -N_ERR_NOT_READY || got == -N_ERR_TIMEOUT) return 0;
        if (got <= 0) {
            snprintf(s->err, sizeof s->err, "TLS transport: %s", n_result_text(transport_failed(s, got)));
            return transport_lost(s);
        }
        s->bytes_sent += (uint64_t)got; s->wstart += (size_t)got;
    }
    s->wstart = s->wlen = 0;
    return 1;
}
/* Seals len bytes of type in records, sending the queue whenever it fills. */
static int queue_blocking(net_stream *s, unsigned type, const unsigned char *p, size_t len) {
    while (len) {
        size_t take = len < PLAIN_MAX ? len : PLAIN_MAX;
        if (!rec_seal(s, type, p, take)) {
            if (out_flush(s) < 0) return -1;
            if (!rec_seal(s, type, p, take)) return FAIL(s, AL_INTERNAL_ERROR, "TLS: record queue");
        }
        p += take; len -= take;
    }
    return 0;
}
static void rbuf_drop(net_stream *s, size_t n) {
    memmove(s->rbuf, s->rbuf + n, s->rlen - n);
    s->rlen -= n;
}
/* At least one more received byte, waiting under the operation's deadline. */
static int in_more(net_stream *s) {
    if (s->rlen >= RECORD_IN_MAX) return FAIL(s, AL_INTERNAL_ERROR, "TLS: record buffer full");
    int r = sock_read(s, s->rbuf + s->rlen, RECORD_IN_MAX - s->rlen);
    if (r < 0) return transport_lost(s);
    s->rlen += (size_t)r;
    return 0;
}

/* KeyUpdate (RFC 8446 §4.6.3): sent when the peer asked for one or the write key has sealed
   REKEY_RECORDS records, before the next application data; one answers any number of requests.
   Deferred while the queue has no room. */
static void key_update_send(net_stream *s) {
    if (!s->ku_due || s->version != TLS13 || s->failed || s->close_sent) return;
    static const unsigned char ku[5] = {HS_KEY_UPDATE, 0, 0, 1, 0}; /* update_not_requested */
    if (!rec_seal(s, CT_HANDSHAKE, ku, sizeof ku)) return;
    unsigned char next[48];
    size_t hl = s->suite->hash_len;
    expand_label(hl, s->wr.secret, "traffic upd", NULL, 0, next, hl);
    write13(s, next);
    wipe(next, sizeof next);
    s->ku_due = false;
}

static bool post_handshake(net_stream *s);
enum { R_MORE, R_DONE, R_CCS, R_APP, R_CLOSED, R_FAIL };
/* Opens the record at the start of rbuf, if it is whole: R_MORE wants more bytes; R_DONE took a
   record with nothing for the caller; R_CCS a TLS 1.2 ChangeCipherSpec; R_APP application data
   (s->app); R_CLOSED the peer's close_notify; R_FAIL a failure. */
static int record_next(net_stream *s) {
    if (s->rlen < 5) return R_MORE;
    unsigned type = s->rbuf[0], version = be16(s->rbuf + 1);
    size_t len = be16(s->rbuf + 3);
    if (type < CT_CCS || type > CT_APP) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unexpected record type %u", type); return R_FAIL; }
    /* legacy_record_version: ignored in TLS 1.3 (§5.1); the negotiated version in TLS 1.2 */
    if ((version >> 8) != 3 || (s->version == TLS12 && version != TLS12)) {
        FAIL(s, AL_PROTOCOL_VERSION, "TLS: record version %04x", version); return R_FAIL;
    }
    size_t limit = s->rd.mode == PROTECT_NONE ? PLAIN_MAX : s->rd.mode == PROTECT_TLS13 ? PLAIN_MAX + 256 : PLAIN_MAX + 2048;
    if (len > limit) { FAIL(s, AL_RECORD_OVERFLOW, "TLS: record of %zu bytes", len); return R_FAIL; }
    if (s->rlen < 5 + len) return R_MORE;
    unsigned char *p = s->rbuf + 5;
    size_t n = len, total = 5 + len;
    if (type == CT_CCS && s->version == TLS13) {
        /* §5: a middlebox compatibility ChangeCipherSpec before the peer's Finished is dropped */
        if (len != 1 || p[0] != 1 || s->peer_finished) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unexpected ChangeCipherSpec"); return R_FAIL; }
        rbuf_drop(s, total);
        return R_DONE;
    }
    if (s->rd.mode == PROTECT_TLS13) {
        if (type != CT_APP) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unprotected record type %u", type); return R_FAIL; }
        if (len < 17 || !aead13_open(&s->rd.a13, s->rbuf, p, len)) { FAIL(s, AL_BAD_RECORD_MAC, "TLS: bad record MAC"); return R_FAIL; }
        n = len - 16;
        while (n && !p[n - 1]) n--;
        if (!n) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: record without a content type"); return R_FAIL; }
        type = p[--n];
        if (n > PLAIN_MAX) { FAIL(s, AL_RECORD_OVERFLOW, "TLS: record of %zu bytes", n); return R_FAIL; }
        if (type != CT_ALERT && type != CT_HANDSHAKE && type != CT_APP) {
            FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unexpected protected record type %u", type); return R_FAIL;
        }
    } else if (s->rd.mode == PROTECT_TLS12) {
        unsigned char *q = s->rd.r12.vtable->check_length(&s->rd.r12.vtable, len)
            ? s->rd.r12.vtable->decrypt(&s->rd.r12.vtable, (int)type, version, p, &n) : NULL;
        if (!q) { FAIL(s, AL_BAD_RECORD_MAC, "TLS: bad record MAC"); return R_FAIL; }
        p = q;
        if (n > PLAIN_MAX) { FAIL(s, AL_RECORD_OVERFLOW, "TLS: record of %zu bytes", n); return R_FAIL; }
    }
    if (!n && type != CT_APP) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: empty record of type %u", type); return R_FAIL; }
    /* a handshake message split over records has no other record between its parts */
    if (type != CT_HANDSHAKE && s->hs_len > s->hs_used) {
        FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: record of type %u inside a handshake message", type); return R_FAIL;
    }
    switch (type) {
    case CT_CCS:
        if (s->version != TLS12 || !s->hs || n != 1 || p[0] != 1) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unexpected ChangeCipherSpec"); return R_FAIL; }
        rbuf_drop(s, total);
        return R_CCS;
    case CT_ALERT: {
        if (n != 2) { FAIL(s, AL_DECODE_ERROR, "TLS: malformed alert"); return R_FAIL; }
        unsigned level = p[0], desc = p[1];
        rbuf_drop(s, total);
        if (desc == AL_CLOSE_NOTIFY) { s->peer_closed = true; return R_CLOSED; }
        /* TLS 1.3 treats every alert but the closure alerts as fatal (§6); TLS 1.2 warnings are not */
        if (s->version == TLS13 ? desc == AL_USER_CANCELED : level == 1) return R_DONE;
        tls_fail(s, AL_NONE, EPROTO, "TLS: server sent alert %u", desc);
        return R_FAIL;
    }
    case CT_HANDSHAKE:
        if (s->hs_cap - s->hs_len < n) {
            size_t need = s->hs_len + n;
            if (need > HANDSHAKE_MESSAGE_MAX + 4 + PLAIN_MAX) { FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: handshake message too large"); return R_FAIL; }
            size_t cap = s->hs_cap ? s->hs_cap : 4096;
            while (cap < need) cap *= 2;
            unsigned char *grown = realloc(s->hsbuf, cap);
            if (!grown) { tls_fail(s, AL_INTERNAL_ERROR, ENOMEM, "TLS: out of memory"); return R_FAIL; }
            s->hsbuf = grown; s->hs_cap = cap;
        }
        memcpy(s->hsbuf + s->hs_len, p, n);
        s->hs_len += n;
        rbuf_drop(s, total);
        if (!s->hs && !post_handshake(s)) return R_FAIL;
        return R_DONE;
    default: /* CT_APP */
        if (s->hs) { FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: application data during the handshake"); return R_FAIL; }
        if (!n) { rbuf_drop(s, total); return R_DONE; }
        s->app = p; s->app_len = n; s->app_record = total;
        return R_APP;
    }
}
static size_t hs_pending(const net_stream *s) { return s->hs_len - s->hs_used; }
static void hs_compact(net_stream *s) {
    if (!s->hs_used) return;
    memmove(s->hsbuf, s->hsbuf + s->hs_used, s->hs_len - s->hs_used);
    s->hs_len -= s->hs_used; s->hs_used = 0;
}

/* ---- after the handshake ---- */
/* NewSessionTicket (RFC 8446 §4.6.1): a PSK for the next connection under the same key. */
static bool ticket_save(net_stream *s, struct cursor *c) {
    uint32_t lifetime = c32(c), age_add = c32(c);
    struct cursor nonce = cvec8(c), ticket = cvec16(c), ext = cvec16(c);
    if (c->bad || c->n || !ticket.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed NewSessionTicket"), false;
    while (ext.n) { c16(&ext); cvec16(&ext); } /* early_data and unknown ones: ignored (no 0-RTT) */
    if (ext.bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed NewSessionTicket"), false;
    if (!lifetime || !s->session_key) return true; /* zero: discarded at once */
    struct resumption r = {0};
    r.key = strdup(s->session_key);
    r.ticket = malloc(ticket.n);
    if (!r.key || !r.ticket) { resume_free(&r); return true; } /* a ticket is never required */
    memcpy(r.ticket, ticket.p, ticket.n);
    r.ticket_len = ticket.n;
    r.version = TLS13; r.suite = s->suite->id;
    r.lifetime = lifetime > 604800 ? 604800 : lifetime; /* never kept longer than 7 days */
    r.age_add = age_add;
    r.saved = uptime_ms();
    r.secret_len = s->suite->hash_len;
    expand_label(r.secret_len, s->res_master, "resumption", nonce.p, nonce.n, r.secret, r.secret_len);
    resume_put(&r);
    return true;
}
static bool key_update_received(net_stream *s, struct cursor *c) {
    unsigned requested = c8(c);
    if (c->bad || c->n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed KeyUpdate"), false;
    if (requested > 1) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: malformed KeyUpdate"), false;
    if (hs_pending(s)) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake data across a key change"), false;
    unsigned char next[48];
    size_t hl = s->suite->hash_len;
    expand_label(hl, s->rd.secret, "traffic upd", NULL, 0, next, hl);
    read13(s, next);
    wipe(next, sizeof next);
    if (requested) { s->ku_due = true; key_update_send(s); }
    return true;
}
/* The whole handshake messages received after the handshake. */
static bool post_handshake(net_stream *s) {
    while (hs_pending(s) >= 4) {
        const unsigned char *m = s->hsbuf + s->hs_used;
        uint32_t n = be24(m + 1);
        if (n > HANDSHAKE_MESSAGE_MAX) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: handshake message too large"), false;
        if (hs_pending(s) < 4 + n) break;
        s->hs_used += 4 + n;
        struct cursor c = {m + 4, n, false};
        if (s->version == TLS13 && m[0] == HS_NEW_SESSION_TICKET) { if (!ticket_save(s, &c)) return false; }
        else if (s->version == TLS13 && m[0] == HS_KEY_UPDATE) { if (!key_update_received(s, &c)) return false; }
        else if (s->version == TLS12 && m[0] == HS_HELLO_REQUEST && !n) {
            /* never renegotiating: RFC 5246 §7.4.1.1 lets the client ignore it */
        } else return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: unexpected handshake message %u", (unsigned)m[0]), false;
    }
    hs_compact(s);
    return true;
}
/* Records until application data is ready: 1 ready, 0 the peer's close_notify, -1 failed,
   -2 nothing yet (block false; a bounded number of records per call). */
static int app_fill(net_stream *s, bool block) {
    for (unsigned steps = 0;; steps++) {
        if (s->app_len) return 1;
        if (s->failed) return (int)failed_result(s);
        if (s->peer_closed) return 0;
        if (!block && steps >= 64) return -2;
        int r = record_next(s);
        if (r == R_FAIL) return -1;
        if (r == R_APP) return 1;
        if (r == R_CLOSED) return 0;
        if (r == R_DONE) {
            key_update_send(s);
            if (s->wstart < s->wlen && (block ? out_flush(s) : out_flush_some(s)) < 0) return -1;
            continue;
        }
        if (block) { if (in_more(s) < 0) return -1; continue; }
        n_result got = tcp_recv(s->sock, s->rbuf + s->rlen, RECORD_IN_MAX - s->rlen, 0);
        if (got == -N_ERR_NOT_READY || got == -N_ERR_TIMEOUT) return -2;
        if (got <= 0) {
            snprintf(s->err, sizeof s->err, "TLS transport: %s", n_result_text(transport_failed(s, got)));
            return transport_lost(s);
        }
        s->rlen += (size_t)got;
    }
}
static long app_take(net_stream *s, void *buf, size_t n) {
    size_t k = s->app_len < n ? s->app_len : n;
    memcpy(buf, s->app, k);
    s->app += k; s->app_len -= k;
    if (!s->app_len) { rbuf_drop(s, s->app_record); s->app = NULL; s->app_record = 0; }
    return (long)k;
}

/* ---- the handshake ---- */
static const unsigned char hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

/* The next handshake message, its 4-byte header first, valid until the next call; HS_CCS for a
   TLS 1.2 ChangeCipherSpec. A TLS 1.2 HelloRequest is skipped (§7.4.1.1: never hashed). */
static int hs_next(net_stream *s, unsigned *type, const unsigned char **msg, size_t *len) {
    for (;;) {
        hs_compact(s);
        if (s->hs_len >= 4) {
            uint32_t n = be24(s->hsbuf + 1);
            if (n > HANDSHAKE_MESSAGE_MAX) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: handshake message too large");
            if (s->hs_len >= 4 + n) {
                s->hs_used = 4 + n;
                if (s->version == TLS12 && s->hsbuf[0] == HS_HELLO_REQUEST) {
                    if (n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed HelloRequest");
                    continue;
                }
                *type = s->hsbuf[0]; *msg = s->hsbuf; *len = 4 + n;
                return 0;
            }
        }
        int r = record_next(s);
        if (r == R_FAIL) return -1;
        if (r == R_CCS) { *type = HS_CCS; *msg = NULL; *len = 0; return 0; }
        if (r == R_CLOSED) return tls_fail(s, AL_NONE, EPROTO, "TLS: peer closed connection during TLS handshake");
        if (r == R_MORE && in_more(s) < 0) return -1;
    }
}
static int hs_expect(net_stream *s, unsigned want, const unsigned char **msg, size_t *len, struct cursor *body) {
    unsigned type;
    if (hs_next(s, &type, msg, len) < 0) return -1;
    if (type != want) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake message %u where %u belongs", type, want);
    if (body) *body = (struct cursor){*msg + 4, *len - 4, false};
    return 0;
}
static int send_hs(net_stream *s, struct handshake *h, const unsigned char *m, size_t len) {
    th_add(&h->th, m, len);
    return queue_blocking(s, CT_HANDSHAKE, m, len);
}
static int send_ccs(net_stream *s) {
    static const unsigned char one = 1;
    return queue_blocking(s, CT_CCS, &one, 1);
}

static bool share_make(struct handshake *h, struct share *sh, unsigned group) {
    const br_ec_impl *ec = br_ec_get_default();
    br_ec_private_key sk;
    size_t n = br_ec_keygen(&h->rng.vtable, ec, &sk, sh->priv, (int)group);
    if (!n) return false;
    sh->group = group; sh->priv_len = n;
    sh->pub_len = br_ec_compute_pub(ec, NULL, sh->pub, &sk);
    return sh->pub_len == point_len(group);
}
/* (EC)DHE: the X coordinate of the shared point (RFC 8446 §7.4, RFC 8422 §5.10). The peer's
   point is checked: on the curve for the NIST curves, a non-zero result for X25519 (RFC 7748 §6.1). */
static bool share_secret(const struct share *sh, const unsigned char *peer, size_t len, unsigned char *out, size_t *out_len) {
    if (len != point_len(sh->group) || (sh->group != GROUP_X25519 && peer[0] != 4)) return false;
    unsigned char g[BR_EC_KBUF_PUB_MAX_SIZE];
    memcpy(g, peer, len);
    const br_ec_impl *ec = br_ec_get_default();
    if (!ec->mul(g, len, sh->priv, sh->priv_len, (int)sh->group)) { wipe(g, sizeof g); return false; }
    size_t xl, xo = ec->xoff((int)sh->group, &xl);
    unsigned zero = 0;
    for (size_t i = 0; i < xl; i++) zero |= g[xo + i];
    memcpy(out, g + xo, xl); *out_len = xl;
    wipe(g, sizeof g);
    return zero != 0;
}

/* A server's handshake signature (TLS 1.2 ServerKeyExchange, TLS 1.3 CertificateVerify) over
   data with the end-entity key: AL_NONE, or the alert. RFC 8446 §4.2.3, §4.4.3: in TLS 1.3 no
   PKCS#1 v1.5, and an ECDSA scheme names its curve; RSA-PSS uses a salt as long as the hash. */
static int verify_signed(unsigned scheme, bool tls13, const br_x509_pkey *pk, const unsigned char *data, size_t len,
                         const unsigned char *sig, size_t sig_len) {
    enum { PKCS1, PSS, ECDSA } kind;
    const br_hash_class *hc; size_t hl; const unsigned char *oid = NULL; int curve = 0;
    switch (scheme) {
    case 0x0401: kind = PKCS1; hc = &br_sha256_vtable; hl = 32; oid = BR_HASH_OID_SHA256; break;
    case 0x0501: kind = PKCS1; hc = &br_sha384_vtable; hl = 48; oid = BR_HASH_OID_SHA384; break;
    case 0x0601: kind = PKCS1; hc = &br_sha512_vtable; hl = 64; oid = BR_HASH_OID_SHA512; break;
    case 0x0804: kind = PSS; hc = &br_sha256_vtable; hl = 32; break;
    case 0x0805: kind = PSS; hc = &br_sha384_vtable; hl = 48; break;
    case 0x0806: kind = PSS; hc = &br_sha512_vtable; hl = 64; break;
    case 0x0403: kind = ECDSA; hc = &br_sha256_vtable; hl = 32; curve = BR_EC_secp256r1; break;
    case 0x0503: kind = ECDSA; hc = &br_sha384_vtable; hl = 48; curve = BR_EC_secp384r1; break;
    case 0x0603: kind = ECDSA; hc = &br_sha512_vtable; hl = 64; curve = BR_EC_secp521r1; break;
    default: return AL_ILLEGAL_PARAMETER;
    }
    if (!offered_sigalg(scheme) || (tls13 && kind == PKCS1)) return AL_ILLEGAL_PARAMETER;
    if (kind == ECDSA ? pk->key_type != BR_KEYTYPE_EC || (tls13 && pk->key.ec.curve != curve) : pk->key_type != BR_KEYTYPE_RSA)
        return AL_ILLEGAL_PARAMETER;
    unsigned char digest[64], recovered[64];
    hash_bytes(hc, data, len, digest);
    bool ok;
    if (kind == PKCS1)
        ok = br_rsa_pkcs1_vrfy_get_default()(sig, sig_len, oid, hl, &pk->key.rsa, recovered) && same_bytes(recovered, digest, hl);
    else if (kind == PSS)
        ok = br_rsa_pss_vrfy_get_default()(sig, sig_len, hc, hc, digest, hl, &pk->key.rsa);
    else
        ok = br_ecdsa_vrfy_asn1_get_default()(br_ec_get_default(), digest, hl, &pk->key.ec, sig, sig_len);
    return ok ? AL_NONE : AL_DECRYPT_ERROR;
}

/* The server's certificate chain (RFC 8446 §4.4.2, RFC 5246 §7.4.2) through br_x509_minimal:
   the trust anchors, validity dates, the host name; then the end-entity key and its usages. */
static int cert_chain(net_stream *s, struct handshake *h, struct cursor *list, bool tls13) {
    const br_x509_class **xc = &h->xc.vtable;
    if (!list->n) return FAIL(s, AL_DECODE_ERROR, "TLS: the server sent no certificate");
    (*xc)->start_chain(xc, h->name);
    while (list->n) {
        struct cursor cert = cvec24(list);
        if (list->bad || !cert.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Certificate");
        (*xc)->start_cert(xc, (uint32_t)cert.n);
        (*xc)->append(xc, cert.p, cert.n);
        (*xc)->end_cert(xc);
        if (tls13) {
            struct cursor ext = cvec16(list);
            if (list->bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Certificate");
            /* status_request and signed_certificate_timestamp were not offered */
            if (ext.n) return FAIL(s, AL_UNSUPPORTED_EXTENSION, "TLS: certificate extension not offered");
        }
    }
    unsigned err = (*xc)->end_chain(xc);
    if (err) {
        int alert = err == BR_ERR_X509_EXPIRED ? AL_CERTIFICATE_EXPIRED : err == BR_ERR_X509_NOT_TRUSTED ? AL_UNKNOWN_CA : AL_BAD_CERTIFICATE;
        if (err == BR_ERR_X509_EXPIRED) return tls_fail(s, alert, EACCES, "TLS: certificate expired or not yet valid (is the clock right?)");
        if (err == BR_ERR_X509_NOT_TRUSTED) return tls_fail(s, alert, EACCES, "TLS: certificate not trusted");
        if (err == BR_ERR_X509_BAD_SERVER_NAME) return tls_fail(s, alert, EACCES, "TLS: certificate does not match the host name");
        if (err == BR_ERR_X509_NOT_CA) return tls_fail(s, alert, EACCES, "TLS: certificate issued by a certificate that is not a CA");
        if (err == BR_ERR_X509_BAD_SIGNATURE) return tls_fail(s, alert, EACCES, "TLS: certificate signature does not verify");
        if (err == BR_ERR_X509_WEAK_PUBLIC_KEY) return tls_fail(s, alert, EACCES, "TLS: certificate key too small");
        if (err == BR_ERR_X509_CRITICAL_EXTENSION) return tls_fail(s, alert, EACCES, "TLS: certificate with an unsupported critical extension");
        if (err == BR_ERR_X509_FORBIDDEN_KEY_USAGE) return tls_fail(s, alert, EACCES, "TLS: certificate key usage forbids this use");
        return tls_fail(s, alert, EACCES, "TLS: certificate rejected (X.509 error %u)", err);
    }
    h->pkey = (*xc)->get_pkey(xc, &h->usages);
    if (!h->pkey) return tls_fail(s, AL_BAD_CERTIFICATE, EACCES, "TLS: certificate key not decoded");
    return 0;
}

/* ALPN (RFC 7301 §3.1): one protocol the client offered. */
static int alpn_take(net_stream *s, struct cursor name) {
    bool h2 = name.n == 2 && !memcmp(name.p, "h2", 2), h1 = name.n == 8 && !memcmp(name.p, "http/1.1", 8);
    if (!h1 && !(h2 && s->http_alpn)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose an application protocol not offered");
    s->h2 = h2;
    memcpy(s->timing_info.protocol, name.p, name.n);
    s->timing_info.protocol[name.n] = 0;
    return 0;
}

/* The extensions this client sends, for telling an unsolicited one from a misplaced one. */
static bool offered_ext(unsigned type) {
    switch (type) {
    case EXT_SERVER_NAME: case EXT_SUPPORTED_GROUPS: case EXT_EC_POINT_FORMATS: case EXT_SIGNATURE_ALGORITHMS:
    case EXT_ALPN: case EXT_PADDING: case EXT_EXTENDED_MASTER_SECRET: case EXT_PRE_SHARED_KEY:
    case EXT_SUPPORTED_VERSIONS: case EXT_COOKIE: case EXT_PSK_KEY_EXCHANGE_MODES: case EXT_KEY_SHARE:
    case EXT_RENEGOTIATION_INFO: return true;
    }
    return false;
}

static void psk_binder(const struct handshake *h, size_t hl, const unsigned char *th, unsigned char *out) {
    unsigned char zero[48] = {0}, early[48], empty[48], key[48], fk[48];
    hkdf_extract(hl, zero, h->psk.secret, hl, early);
    hash_bytes(hash_of(hl), NULL, 0, empty);
    derive_secret(hl, early, "res binder", empty, key);
    expand_label(hl, key, "finished", NULL, 0, fk, hl);
    hmac(hash_of(hl), fk, hl, th, hl, NULL, 0, out);
    wipe(early, sizeof early); wipe(key, sizeof key); wipe(fk, sizeof fk);
}
/* The ClientHello (RFC 8446 §4.1.2), offering TLS 1.3 and 1.2 at once; the second one after a
   HelloRetryRequest differs only as §4.1.2 lets it. */
static bool client_hello(net_stream *s, struct handshake *h) {
    struct out *o = &h->msg;
    o->len = 0;
    o_u8(o, HS_CLIENT_HELLO);
    size_t body = o_open(o, 3);
    o_u16(o, TLS12); /* legacy_version */
    o_put(o, h->client_random, 32);
    o_u8(o, (unsigned)h->session_id_len); o_put(o, h->session_id, h->session_id_len);
    size_t at = o_open(o, 2);
    for (size_t i = 0; i < SUITE_COUNT; i++) o_u16(o, suites[i].id);
    o_close(o, at, 2);
    o_u8(o, 1); o_u8(o, 0); /* null compression */
    size_t exts = o_open(o, 2), e, l;
    if (h->sni) { /* RFC 6066 §3: no IP literal */
        size_t nl = strlen(h->name);
        o_u16(o, EXT_SERVER_NAME); e = o_open(o, 2); l = o_open(o, 2);
        o_u8(o, 0); o_u16(o, (unsigned)nl); o_put(o, h->name, nl);
        o_close(o, l, 2); o_close(o, e, 2);
    }
    o_u16(o, EXT_EXTENDED_MASTER_SECRET); o_u16(o, 0);
    o_u16(o, EXT_RENEGOTIATION_INFO); o_u16(o, 1); o_u8(o, 0);
    o_u16(o, EXT_SUPPORTED_GROUPS); o_u16(o, 2 + 2 * 4); o_u16(o, 2 * 4);
    for (size_t i = 0; i < 4; i++) o_u16(o, groups[i]);
    o_u16(o, EXT_EC_POINT_FORMATS); o_u16(o, 2); o_u8(o, 1); o_u8(o, 0);
    o_u16(o, EXT_ALPN); e = o_open(o, 2); l = o_open(o, 2);
    if (s->http_alpn) { o_u8(o, 2); o_put(o, "h2", 2); }
    o_u8(o, 8); o_put(o, "http/1.1", 8);
    o_close(o, l, 2); o_close(o, e, 2);
    size_t ns = sizeof sigalgs / sizeof *sigalgs;
    o_u16(o, EXT_SIGNATURE_ALGORITHMS); o_u16(o, (unsigned)(2 + 2 * ns)); o_u16(o, (unsigned)(2 * ns));
    for (size_t i = 0; i < ns; i++) o_u16(o, sigalgs[i]);
    o_u16(o, EXT_KEY_SHARE); e = o_open(o, 2); l = o_open(o, 2);
    for (size_t i = 0; i < h->share_count; i++) {
        o_u16(o, h->shares[i].group); o_u16(o, (unsigned)h->shares[i].pub_len); o_put(o, h->shares[i].pub, h->shares[i].pub_len);
    }
    o_close(o, l, 2); o_close(o, e, 2);
    o_u16(o, EXT_PSK_KEY_EXCHANGE_MODES); o_u16(o, 2); o_u8(o, 1); o_u8(o, 1); /* psk_dhe_ke */
    o_u16(o, EXT_SUPPORTED_VERSIONS); o_u16(o, 5); o_u8(o, 4); o_u16(o, TLS13); o_u16(o, TLS12);
    if (h->cookie) { o_u16(o, EXT_COOKIE); o_u16(o, (unsigned)(2 + h->cookie_len)); o_u16(o, (unsigned)h->cookie_len); o_put(o, h->cookie, h->cookie_len); }
    size_t hl = h->psk_offered ? suite_of(h->psk.suite)->hash_len : 0;
    size_t psk_len = h->psk_offered ? 4 + 2 + 2 + h->psk.ticket_len + 4 + 2 + 1 + hl : 0;
    /* padding (RFC 7685): a ClientHello of 256 to 511 bytes grows to 512, for servers that
       mistake those for SSL 2 */
    size_t total = o->len + psk_len;
    if (total > 0xff && total < 0x200) {
        size_t pad = 0x200 - total;
        pad = pad >= 5 ? pad - 4 : 1;
        static const unsigned char zeros[512];
        o_u16(o, EXT_PADDING); o_u16(o, (unsigned)pad); o_put(o, zeros, pad);
    }
    if (h->psk_offered) { /* last (§4.2.11) */
        uint32_t age = (uint32_t)(uptime_ms() - h->psk.saved) + h->psk.age_add;
        o_u16(o, EXT_PRE_SHARED_KEY); e = o_open(o, 2); l = o_open(o, 2);
        o_u16(o, (unsigned)h->psk.ticket_len); o_put(o, h->psk.ticket, h->psk.ticket_len); o_u32(o, age);
        o_close(o, l, 2);
        size_t binders = o->len;
        static const unsigned char blank[48];
        o_u16(o, (unsigned)(1 + hl)); o_u8(o, (unsigned)hl); o_put(o, blank, hl);
        o_close(o, e, 2); o_close(o, exts, 2); o_close(o, body, 3);
        if (o->failed) return false;
        /* the binder: over the transcript and this ClientHello up to its binders (§4.2.11.2) */
        struct transcript t = h->th;
        unsigned char th[48];
        th_add(&t, o->p, binders);
        th_hash(&t, hl, th);
        psk_binder(h, hl, th, o->p + binders + 3);
        return true;
    }
    o_close(o, exts, 2); o_close(o, body, 3);
    return !o->failed;
}

/* A ServerHello or HelloRetryRequest (RFC 8446 §4.1.3, §4.1.4; RFC 5246 §7.4.1.3), read as far as
   both versions share. */
struct server_hello {
    unsigned legacy_version, suite, compression, version;
    const unsigned char *random;
    struct cursor sid, ks, cookie, alpn;
    unsigned ks_group, psk_identity;
    bool hrr, has_ks, has_psk, has_cookie, has_alpn, ems, tls12_ext, sid_echo;
};
static int server_hello_read(net_stream *s, struct handshake *h, const unsigned char *m, size_t len, struct server_hello *sh) {
    memset(sh, 0, sizeof *sh);
    struct cursor c = {m + 4, len - 4, false}, ext = {0};
    sh->legacy_version = c16(&c);
    sh->random = cbytes(&c, 32);
    sh->sid = cvec8(&c);
    sh->suite = c16(&c);
    sh->compression = c8(&c);
    if (c.n) ext = cvec16(&c);
    if (c.bad || c.n || sh->sid.n > 32) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ServerHello");
    sh->hrr = same_bytes(sh->random, hrr_random, 32);
    uint16_t seen[64]; size_t count = 0;
    while (ext.n) {
        unsigned type = c16(&ext);
        struct cursor d = cvec16(&ext);
        if (ext.bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ServerHello");
        for (size_t i = 0; i < count; i++) if (seen[i] == type) return FAIL(s, AL_DECODE_ERROR, "TLS: extension %u twice", type);
        if (count == 64) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ServerHello");
        seen[count++] = (uint16_t)type;
        switch (type) {
        case EXT_SUPPORTED_VERSIONS: sh->version = c16(&d); break;
        case EXT_KEY_SHARE: sh->has_ks = true; sh->ks_group = c16(&d); if (!sh->hrr) sh->ks = cvec16(&d); break;
        case EXT_PRE_SHARED_KEY: sh->has_psk = true; sh->psk_identity = c16(&d); break;
        case EXT_COOKIE: sh->has_cookie = true; sh->cookie = cvec16(&d); if (!sh->cookie.n) d.bad = true; break;
        case EXT_EXTENDED_MASTER_SECRET: sh->ems = true; sh->tls12_ext = true; break;
        case EXT_RENEGOTIATION_INFO: { /* RFC 5746 §3.4: an initial handshake's is empty */
            struct cursor r = cvec8(&d);
            if (!d.bad && !d.n && r.n) return FAIL(s, AL_HANDSHAKE_FAILURE, "TLS: renegotiation_info not empty");
            sh->tls12_ext = true; break;
        }
        case EXT_ALPN: {
            struct cursor list = cvec16(&d);
            sh->alpn = cvec8(&list);
            if (list.bad || list.n || !sh->alpn.n) d.bad = true;
            sh->has_alpn = true; sh->tls12_ext = true; break;
        }
        case EXT_EC_POINT_FORMATS: { /* RFC 8422 §5.2: the list holds uncompressed */
            struct cursor f = cvec8(&d);
            bool uncompressed = false;
            while (f.n) uncompressed |= c8(&f) == 0;
            if (!d.bad && !d.n && !uncompressed) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: no uncompressed point format");
            sh->tls12_ext = true; break;
        }
        case EXT_SERVER_NAME:
            if (!h->sni) return FAIL(s, AL_UNSUPPORTED_EXTENSION, "TLS: extension %u not offered", type);
            sh->tls12_ext = true; break;
        case EXT_SUPPORTED_GROUPS: sh->tls12_ext = true; d.n = 0; break; /* echoed by some TLS 1.2 servers */
        default:
            return FAIL(s, offered_ext(type) ? AL_ILLEGAL_PARAMETER : AL_UNSUPPORTED_EXTENSION, "TLS: extension %u in ServerHello", type);
        }
        if (d.bad || d.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed extension %u", type);
    }
    if (sh->version) {
        if (sh->version != TLS13 || sh->legacy_version != TLS12) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose version %04x", sh->version);
    } else if (sh->hrr) return FAIL(s, AL_MISSING_EXTENSION, "TLS: HelloRetryRequest without supported_versions");
    else if (sh->legacy_version != TLS12) return FAIL(s, AL_PROTOCOL_VERSION, "TLS: server does not support TLS 1.2 or 1.3");
    sh->sid_echo = sh->sid.n == h->session_id_len && same_bytes(sh->sid.p, h->session_id, h->session_id_len);
    if (sh->version && !sh->sid_echo) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: legacy_session_id not echoed");
    return 0;
}

static void prf12(size_t hl, unsigned char *out, size_t out_len, const unsigned char *secret, size_t secret_len,
                  const char *label, const void *s1, size_t l1, const void *s2, size_t l2) {
    br_tls_prf_seed_chunk seed[2] = {{s1, l1}, {s2, l2}};
    (hl == 48 ? br_tls12_sha384_prf : br_tls12_sha256_prf)(out, out_len, secret, secret_len, label, l2 ? 2 : 1, seed);
}
/* TLS 1.2 keys (RFC 5246 §6.3): client MAC, server MAC, client key, server key, client IV,
   server IV; an IV only for the AEAD suites' implicit nonce (RFC 5288 §3, RFC 7905 §2). */
static void keys12(net_stream *s, struct handshake *h, const unsigned char *master) {
    const struct suite *su = s->suite;
    size_t mac = su->cipher >= C_AES128_CBC ? 20 : 0, k = su->key_len, iv = su->cipher == C_CHACHA20 ? 12 : mac ? 0 : 4;
    prf12(su->hash_len, h->keyblock, 2 * (mac + k + iv), master, 48, "key expansion", h->server_random, 32, h->client_random, 32);
}
static void install12(net_stream *s, struct handshake *h, bool client) {
    const struct suite *su = s->suite;
    size_t mac = su->cipher >= C_AES128_CBC ? 20 : 0, k = su->key_len, iv = su->cipher == C_CHACHA20 ? 12 : mac ? 0 : 4;
    const unsigned char *kb = h->keyblock;
    const unsigned char *m = kb + (client ? 0 : mac), *key = kb + 2 * mac + (client ? 0 : k), *v = kb + 2 * (mac + k) + (client ? 0 : iv);
    if (client) {
        if (mac) br_sslrec_out_cbc_vtable.init(&s->wr.r12.cbc.vtable, aes_cbcenc(), key, k, &br_sha1_vtable, m, 20, 20, NULL);
        else if (su->cipher == C_CHACHA20) br_sslrec_out_chapol_vtable.init(&s->wr.r12.chapol.vtable.out, chacha(), poly(), key, v);
        else br_sslrec_out_gcm_vtable.init(&s->wr.r12.gcm.vtable.out, aes_ctr(), key, k, ghash(), v);
        s->wr.mode = PROTECT_TLS12;
    } else {
        if (mac) br_sslrec_in_cbc_vtable.init(&s->rd.r12.cbc.vtable, aes_cbcdec(), key, k, &br_sha1_vtable, m, 20, 20, NULL);
        else if (su->cipher == C_CHACHA20) br_sslrec_in_chapol_vtable.init(&s->rd.r12.chapol.vtable.in, chacha(), poly(), key, v);
        else br_sslrec_in_gcm_vtable.init(&s->rd.r12.gcm.vtable.in, aes_ctr(), key, k, ghash(), v);
        s->rd.mode = PROTECT_TLS12;
    }
}
static void finished12(net_stream *s, struct handshake *h, const unsigned char *master, const char *label, unsigned char out[12]) {
    unsigned char th[48];
    th_hash(&h->th, s->suite->hash_len, th);
    prf12(s->suite->hash_len, out, 12, master, 48, label, th, s->suite->hash_len, NULL, 0);
}
static int client_finished12(net_stream *s, struct handshake *h, const unsigned char *master) {
    unsigned char fin[16] = {HS_FINISHED, 0, 0, 12};
    finished12(s, h, master, "client finished", fin + 4);
    if (send_ccs(s) < 0) return -1;
    install12(s, h, true);
    return send_hs(s, h, fin, sizeof fin);
}
static int server_finished12(net_stream *s, struct handshake *h, const unsigned char *master) {
    const unsigned char *m; size_t len; struct cursor c;
    if (hs_expect(s, HS_CCS, &m, &len, NULL) < 0) return -1;
    install12(s, h, false);
    unsigned char want[12];
    finished12(s, h, master, "server finished", want);
    if (hs_expect(s, HS_FINISHED, &m, &len, &c) < 0) return -1;
    if (c.n != 12) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Finished");
    if (!same_bytes(c.p, want, 12)) return FAIL(s, AL_DECRYPT_ERROR, "TLS: server Finished does not verify");
    th_add(&h->th, m, len);
    s->peer_finished = true;
    return 0;
}

/* TLS 1.2 after its ServerHello: the full handshake (RFC 5246 §7.3) or the abbreviated one. */
static int tls12(net_stream *s, struct handshake *h, const struct server_hello *sh, const unsigned char *shm, size_t shlen) {
    const struct suite *su = suite_of((uint16_t)sh->suite);
    if (!su || su->version != TLS12) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose cipher suite %04x", sh->suite);
    if (sh->compression) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose compression");
    if (sh->has_ks || sh->has_psk || sh->has_cookie) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: TLS 1.3 extension in a TLS 1.2 ServerHello");
    /* RFC 8446 §4.1.3: a TLS 1.3 server that was made to negotiate less says so */
    if (!memcmp(sh->random + 24, "DOWNGRD\x01", 8) || !memcmp(sh->random + 24, "DOWNGRD\x00", 8))
        return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: downgrade from TLS 1.3 detected");
    s->version = TLS12; s->suite = su;
    memcpy(h->server_random, sh->random, 32);
    /* the ServerHello's bytes move as hsbuf takes the next messages: its session ID is kept */
    unsigned char sid[32];
    size_t sid_len = sh->sid.n;
    memcpy(sid, sh->sid.p, sid_len);
    th_add(&h->th, shm, shlen);
    if (sh->has_alpn && alpn_take(s, sh->alpn) < 0) return -1;
    /* the session ID offered echoed: the abbreviated handshake; the random one of the middlebox
       compatibility mode is never a session */
    bool resumed = h->sess12_offered && sh->sid_echo;
    if (!h->sess12_offered && sh->sid_echo)
        return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server resumed a session not offered");
    if (resumed) {
        if (su->id != h->sess12.suite) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: resumed session with another cipher suite");
        if (sh->ems != h->sess12.ems) return FAIL(s, AL_HANDSHAKE_FAILURE, "TLS: extended master secret changed on resumption");
        keys12(s, h, h->sess12.secret);
        if (server_finished12(s, h, h->sess12.secret) < 0 || client_finished12(s, h, h->sess12.secret) < 0) return -1;
        s->resumed = true;
        return out_flush(s);
    }
    const unsigned char *m; size_t len; struct cursor c;
    if (hs_expect(s, HS_CERTIFICATE, &m, &len, &c) < 0) return -1;
    th_add(&h->th, m, len);
    struct cursor list = cvec24(&c);
    if (c.bad || c.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Certificate");
    if (cert_chain(s, h, &list, false) < 0) return -1;
    unsigned key_type = su->kx == KX_ECDHE_ECDSA ? BR_KEYTYPE_EC : BR_KEYTYPE_RSA;
    unsigned usage = su->kx == KX_RSA ? BR_KEYTYPE_KEYX : BR_KEYTYPE_SIGN;
    if (h->pkey->key_type != key_type || !(h->usages & usage))
        return FAIL(s, AL_UNSUPPORTED_CERTIFICATE, "TLS: certificate key does not fit the cipher suite");
    struct share mine = {0};
    unsigned char pms[66]; size_t pms_len = 0;
    unsigned type;
    if (su->kx != KX_RSA) { /* ServerKeyExchange, ECDHE (RFC 8422 §5.4) */
        if (hs_expect(s, HS_SERVER_KEY_EXCHANGE, &m, &len, &c) < 0) return -1;
        th_add(&h->th, m, len);
        const unsigned char *params = c.p;
        unsigned curve_type = c8(&c), group = c16(&c);
        struct cursor point = cvec8(&c);
        size_t params_len = (size_t)(c.p - params);
        unsigned scheme = c16(&c);
        struct cursor sig = cvec16(&c);
        if (c.bad || c.n || point.bad || sig.bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ServerKeyExchange");
        if (curve_type != 3 || !offered_group(group)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose group %u", group);
        if (point.n != point_len(group)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: bad ECDHE public key");
        unsigned char signed_data[64 + 4 + BR_EC_KBUF_PUB_MAX_SIZE];
        memcpy(signed_data, h->client_random, 32); memcpy(signed_data + 32, h->server_random, 32);
        memcpy(signed_data + 64, params, params_len);
        int alert = verify_signed(scheme, false, h->pkey, signed_data, 64 + params_len, sig.p, sig.n);
        if (alert != AL_NONE) return FAIL(s, alert, "TLS: ServerKeyExchange signature (%04x) does not verify", scheme);
        if (!share_make(h, &mine, group)) return FAIL(s, AL_INTERNAL_ERROR, "TLS: key generation failed");
        if (!share_secret(&mine, point.p, point.n, pms, &pms_len)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: bad ECDHE public key");
    }
    if (hs_next(s, &type, &m, &len) < 0) return -1;
    if (type == HS_CERTIFICATE_REQUEST) { /* answered with no certificate */
        c = (struct cursor){m + 4, len - 4, false};
        cvec8(&c); cvec16(&c); cvec16(&c);
        if (c.bad || c.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed CertificateRequest");
        th_add(&h->th, m, len);
        h->cert_request = true;
        if (hs_next(s, &type, &m, &len) < 0) return -1;
    }
    if (type != HS_SERVER_HELLO_DONE) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake message %u where %u belongs", type, (unsigned)HS_SERVER_HELLO_DONE);
    if (len != 4) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ServerHelloDone");
    th_add(&h->th, m, len);
    if (h->cert_request) {
        static const unsigned char none[7] = {HS_CERTIFICATE, 0, 0, 3, 0, 0, 0};
        if (send_hs(s, h, none, sizeof none) < 0) return -1;
    }
    unsigned char cke[4 + 2 + 512];
    size_t cke_len;
    if (su->kx == KX_RSA) { /* RFC 5246 §7.4.7.1: the version offered and 46 random bytes, PKCS#1 v1.5 */
        const unsigned char *n = h->pkey->key.rsa.n;
        size_t nlen = h->pkey->key.rsa.nlen;
        while (nlen && !*n) { n++; nlen--; }
        if (nlen < 59 || nlen > 512) return FAIL(s, AL_UNSUPPORTED_CERTIFICATE, "TLS: RSA key size");
        pms[0] = 3; pms[1] = 3;
        br_hmac_drbg_generate(&h->rng, pms + 2, 46);
        pms_len = 48;
        unsigned char *x = cke + 6;
        x[0] = 0; x[1] = 2;
        size_t ps = nlen - 3 - 48;
        br_hmac_drbg_generate(&h->rng, x + 2, ps);
        for (size_t i = 0; i < ps; i++) while (!x[2 + i]) br_hmac_drbg_generate(&h->rng, x + 2 + i, 1);
        x[2 + ps] = 0;
        memcpy(x + 3 + ps, pms, 48);
        br_rsa_public_key pk = h->pkey->key.rsa;
        pk.n = (unsigned char *)n; pk.nlen = nlen;
        if (!br_rsa_public_get_default()(x, nlen, &pk)) return FAIL(s, AL_INTERNAL_ERROR, "TLS: RSA encryption failed");
        cke[4] = (unsigned char)(nlen >> 8); cke[5] = (unsigned char)nlen;
        cke_len = 6 + nlen;
    } else {
        cke[4] = (unsigned char)mine.pub_len;
        memcpy(cke + 5, mine.pub, mine.pub_len);
        cke_len = 5 + mine.pub_len;
        wipe(&mine, sizeof mine);
    }
    cke[0] = HS_CLIENT_KEY_EXCHANGE; cke[1] = 0; cke[2] = (unsigned char)((cke_len - 4) >> 8); cke[3] = (unsigned char)(cke_len - 4);
    if (send_hs(s, h, cke, cke_len) < 0) { wipe(pms, sizeof pms); return -1; }
    unsigned char master[48];
    if (sh->ems) { /* RFC 7627 §4: over the session hash */
        unsigned char session_hash[48];
        th_hash(&h->th, su->hash_len, session_hash);
        prf12(su->hash_len, master, 48, pms, pms_len, "extended master secret", session_hash, su->hash_len, NULL, 0);
    } else prf12(su->hash_len, master, 48, pms, pms_len, "master secret", h->client_random, 32, h->server_random, 32);
    wipe(pms, sizeof pms);
    keys12(s, h, master);
    if (client_finished12(s, h, master) < 0 || out_flush(s) < 0 || server_finished12(s, h, master) < 0) { wipe(master, sizeof master); return -1; }
    if (s->session_key && sid_len) {
        struct resumption r = {0};
        r.key = strdup(s->session_key);
        if (r.key) {
            r.version = TLS12; r.suite = su->id; r.saved = uptime_ms();
            memcpy(r.secret, master, 48); r.secret_len = 48;
            memcpy(r.session_id, sid, sid_len); r.session_id_len = sid_len;
            r.ems = sh->ems;
            resume_put(&r);
        }
    } else if (h->sess12_offered) resume_drop12(s->session_key);
    wipe(master, sizeof master);
    return 0;
}

/* TLS 1.3 after its ServerHello (RFC 8446 §2, §4.3, §4.4, §7.1). */
static int tls13(net_stream *s, struct handshake *h, bool resumed) {
    size_t hl = s->suite->hash_len;
    unsigned char zero[48] = {0}, empty[48], early[48], derived[48], secret[48], th[48];
    hash_bytes(hash_of(hl), NULL, 0, empty);
    hkdf_extract(hl, zero, resumed ? h->psk.secret : zero, hl, early);
    derive_secret(hl, early, "derived", empty, derived);
    hkdf_extract(hl, derived, h->shared, h->shared_len, secret);
    th_hash(&h->th, hl, th);
    derive_secret(hl, secret, "c hs traffic", th, h->c_hs);
    derive_secret(hl, secret, "s hs traffic", th, h->s_hs);
    derive_secret(hl, secret, "derived", empty, derived);
    hkdf_extract(hl, derived, zero, hl, h->master);
    wipe(early, sizeof early); wipe(secret, sizeof secret); wipe(h->shared, sizeof h->shared);
    if (hs_pending(s)) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake data across a key change");
    read13(s, h->s_hs);

    const unsigned char *m; size_t len; struct cursor c;
    if (hs_expect(s, HS_ENCRYPTED_EXTENSIONS, &m, &len, &c) < 0) return -1;
    struct cursor ext = cvec16(&c);
    if (c.bad || c.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed EncryptedExtensions");
    uint16_t seen[64]; size_t count = 0;
    while (ext.n) {
        unsigned type = c16(&ext);
        struct cursor d = cvec16(&ext);
        if (ext.bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed EncryptedExtensions");
        for (size_t i = 0; i < count; i++) if (seen[i] == type) return FAIL(s, AL_DECODE_ERROR, "TLS: extension %u twice", type);
        if (count == 64) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed EncryptedExtensions");
        seen[count++] = (uint16_t)type;
        if (type == EXT_ALPN) {
            struct cursor list = cvec16(&d), name = cvec8(&list);
            if (d.bad || d.n || list.bad || list.n || !name.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed ALPN");
            if (alpn_take(s, name) < 0) return -1;
        } else if (type == EXT_SERVER_NAME) {
            if (!h->sni) return FAIL(s, AL_UNSUPPORTED_EXTENSION, "TLS: extension %u not offered", type);
            if (d.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed server_name");
        } else if (type != EXT_SUPPORTED_GROUPS) /* the server's preference, for later: ignored */
            return FAIL(s, offered_ext(type) ? AL_ILLEGAL_PARAMETER : AL_UNSUPPORTED_EXTENSION, "TLS: extension %u in EncryptedExtensions", type);
    }
    th_add(&h->th, m, len);
    if (!resumed) {
        unsigned type;
        if (hs_next(s, &type, &m, &len) < 0) return -1;
        if (type == HS_CERTIFICATE_REQUEST) { /* §4.3.2; answered with an empty Certificate */
            c = (struct cursor){m + 4, len - 4, false};
            struct cursor context = cvec8(&c), exts = cvec16(&c);
            bool sigalgs_seen = false;
            while (exts.n) { if (c16(&exts) == EXT_SIGNATURE_ALGORITHMS) sigalgs_seen = true; cvec16(&exts); }
            if (c.bad || c.n || exts.bad) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed CertificateRequest");
            if (!sigalgs_seen) return FAIL(s, AL_MISSING_EXTENSION, "TLS: CertificateRequest without signature_algorithms");
            memcpy(h->context, context.p, context.n); h->context_len = context.n;
            h->cert_request = true;
            th_add(&h->th, m, len);
            if (hs_next(s, &type, &m, &len) < 0) return -1;
        }
        if (type != HS_CERTIFICATE) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake message %u where %u belongs", type, (unsigned)HS_CERTIFICATE);
        c = (struct cursor){m + 4, len - 4, false};
        struct cursor context = cvec8(&c), list = cvec24(&c);
        if (c.bad || c.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Certificate");
        if (context.n) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server Certificate with a request context");
        if (cert_chain(s, h, &list, true) < 0) return -1;
        if (!(h->usages & BR_KEYTYPE_SIGN)) return FAIL(s, AL_UNSUPPORTED_CERTIFICATE, "TLS: certificate key is not for signing");
        th_add(&h->th, m, len);
        if (hs_expect(s, HS_CERTIFICATE_VERIFY, &m, &len, &c) < 0) return -1;
        unsigned scheme = c16(&c);
        struct cursor sig = cvec16(&c);
        if (c.bad || c.n) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed CertificateVerify");
        /* §4.4.3: 64 spaces, the context string, a zero byte, the transcript hash */
        static const char context_string[] = "TLS 1.3, server CertificateVerify";
        unsigned char content[64 + sizeof context_string + 48];
        memset(content, 0x20, 64);
        memcpy(content + 64, context_string, sizeof context_string);
        th_hash(&h->th, hl, content + 64 + sizeof context_string);
        int alert = verify_signed(scheme, true, h->pkey, content, 64 + sizeof context_string + hl, sig.p, sig.n);
        if (alert != AL_NONE) return FAIL(s, alert, "TLS: CertificateVerify (%04x) does not verify", scheme);
        th_add(&h->th, m, len);
    }
    if (hs_expect(s, HS_FINISHED, &m, &len, &c) < 0) return -1;
    unsigned char fk[48], want[48];
    expand_label(hl, h->s_hs, "finished", NULL, 0, fk, hl);
    th_hash(&h->th, hl, th);
    hmac(hash_of(hl), fk, hl, th, hl, NULL, 0, want);
    if (c.n != hl) return FAIL(s, AL_DECODE_ERROR, "TLS: malformed Finished");
    if (!same_bytes(c.p, want, hl)) return FAIL(s, AL_DECRYPT_ERROR, "TLS: server Finished does not verify");
    th_add(&h->th, m, len);
    s->peer_finished = true;
    unsigned char c_ap[48], s_ap[48];
    th_hash(&h->th, hl, th);
    derive_secret(hl, h->master, "c ap traffic", th, c_ap);
    derive_secret(hl, h->master, "s ap traffic", th, s_ap);
    if (hs_pending(s)) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake data across a key change");
    read13(s, s_ap);
    /* the client's flight: the compatibility ChangeCipherSpec once (Appendix D.4), then under
       the client handshake key an empty Certificate if one was asked for, and Finished */
    if (!h->ccs_sent) { if (send_ccs(s) < 0) return -1; h->ccs_sent = true; }
    write13(s, h->c_hs);
    if (h->cert_request) {
        struct out *o = &h->msg;
        o->len = 0;
        o_u8(o, HS_CERTIFICATE); size_t body = o_open(o, 3);
        o_u8(o, (unsigned)h->context_len); o_put(o, h->context, h->context_len); o_u24(o, 0);
        o_close(o, body, 3);
        if (o->failed) return tls_fail(s, AL_INTERNAL_ERROR, ENOMEM, "TLS: out of memory");
        if (send_hs(s, h, o->p, o->len) < 0) return -1;
    }
    unsigned char fin[4 + 48] = {HS_FINISHED, 0, 0, (unsigned char)hl};
    expand_label(hl, h->c_hs, "finished", NULL, 0, fk, hl);
    th_hash(&h->th, hl, th);
    hmac(hash_of(hl), fk, hl, th, hl, NULL, 0, fin + 4);
    if (send_hs(s, h, fin, 4 + hl) < 0) return -1;
    th_hash(&h->th, hl, th);
    derive_secret(hl, h->master, "res master", th, s->res_master);
    write13(s, c_ap);
    wipe(c_ap, sizeof c_ap); wipe(s_ap, sizeof s_ap); wipe(fk, sizeof fk);
    s->resumed = resumed;
    return out_flush(s);
}

static void handshake_free(net_stream *s) {
    struct handshake *h = s->hs;
    if (!h) return;
    free(h->cookie);
    resume_free(&h->psk);
    resume_free(&h->sess12);
    o_free(&h->msg);
    wipe(h, sizeof *h);
    free(h);
    s->hs = NULL;
}
/* RFC 6066 §3: neither an IPv4 nor an IPv6 literal is a HostName; no DNS name has a colon. */
static bool ip_literal(const char *host) {
    if (!*host) return false;
    if (strchr(host, ':')) return true;
    for (; *host; host++) if ((*host < '0' || *host > '9') && *host != '.') return false;
    return true;
}
/* The whole handshake, blocking under the open's deadline. */
static int handshake(net_stream *s, const char *host) {
    struct handshake *h = calloc(1, sizeof *h);
    if (!h) return tls_fail(s, AL_NONE, ENOMEM, "TLS: out of memory");
    s->hs = h;
    size_t name_len = strlen(host);
    if (name_len && host[name_len - 1] == '.') name_len--; /* the same name (RFC 6066 §3) */
    if (!name_len || name_len >= sizeof h->name) return tls_fail(s, AL_NONE, EINVAL, "TLS: host name of %zu bytes", name_len);
    memcpy(h->name, host, name_len);
    h->sni = !ip_literal(h->name);
    unsigned char seed[48];
    if (n_random(seed, sizeof seed) < 0 || n_random(h->client_random, 32) < 0)
        return tls_fail(s, AL_NONE, EIO, "TLS: no entropy source");
    br_hmac_drbg_init(&h->rng, &br_sha256_vtable, seed, sizeof seed);
    wipe(seed, sizeof seed);
    size_t anchors_known;
    const br_x509_trust_anchor *known = trust_anchors(&anchors_known);
    br_x509_minimal_init_full(&h->xc, known, anchors_known);
    th_init(&h->th);
    h->psk_offered = resume_take13(s->session_key, &h->psk);
    h->sess12_offered = resume_get12(s->session_key, &h->sess12);
    if (h->sess12_offered) {
        memcpy(h->session_id, h->sess12.session_id, h->sess12.session_id_len);
        h->session_id_len = h->sess12.session_id_len;
    } else { /* Appendix D.4: a fresh 32-byte ID for the middlebox compatibility mode */
        br_hmac_drbg_generate(&h->rng, h->session_id, 32);
        h->session_id_len = 32;
    }
    if (!share_make(h, &h->shares[0], GROUP_X25519) || !share_make(h, &h->shares[1], GROUP_P256))
        return FAIL(s, AL_NONE, "TLS: key generation failed");
    h->share_count = 2;
    s->wr_version = 0x0301; /* §5.1: an initial ClientHello may say TLS 1.0, for old servers */
    if (!client_hello(s, h)) return tls_fail(s, AL_NONE, ENOMEM, "TLS: out of memory");
    if (send_hs(s, h, h->msg.p, h->msg.len) < 0 || out_flush(s) < 0) return -1;
    s->wr_version = TLS12;

    const unsigned char *m; size_t len;
    struct server_hello sh;
    if (hs_expect(s, HS_SERVER_HELLO, &m, &len, NULL) < 0 || server_hello_read(s, h, m, len, &sh) < 0) return -1;
    if (sh.hrr) { /* §4.1.4 */
        const struct suite *su = suite_of((uint16_t)sh.suite);
        if (!su || su->version != TLS13 || sh.compression || sh.tls12_ext || sh.has_psk)
            return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: malformed HelloRetryRequest");
        if (!sh.has_ks && !sh.has_cookie) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: HelloRetryRequest that changes nothing");
        if (sh.has_ks) {
            bool shared_already = false;
            for (size_t i = 0; i < h->share_count; i++) shared_already |= h->shares[i].group == sh.ks_group;
            if (!offered_group(sh.ks_group) || shared_already) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: HelloRetryRequest for group %u", sh.ks_group);
        }
        s->version = TLS13; h->hrr = true; h->hrr_suite = su;
        /* §4.4.1: the first ClientHello becomes a message_hash */
        unsigned char ch1[48], mh[4] = {HS_MESSAGE_HASH, 0, 0, (unsigned char)su->hash_len};
        th_hash(&h->th, su->hash_len, ch1);
        th_init(&h->th);
        th_add(&h->th, mh, 4); th_add(&h->th, ch1, su->hash_len);
        th_add(&h->th, m, len);
        if (sh.has_ks) {
            wipe(h->shares, sizeof h->shares);
            if (!share_make(h, &h->shares[0], sh.ks_group)) return FAIL(s, AL_INTERNAL_ERROR, "TLS: key generation failed");
            h->share_count = 1;
        }
        if (sh.has_cookie) {
            if (!(h->cookie = malloc(sh.cookie.n))) return tls_fail(s, AL_INTERNAL_ERROR, ENOMEM, "TLS: out of memory");
            memcpy(h->cookie, sh.cookie.p, sh.cookie.n); h->cookie_len = sh.cookie.n;
        }
        if (h->psk_offered && suite_of(h->psk.suite)->hash_len != su->hash_len) { resume_free(&h->psk); h->psk_offered = false; }
        if (hs_pending(s)) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: handshake data after HelloRetryRequest");
        if (send_ccs(s) < 0) return -1;
        h->ccs_sent = true;
        if (!client_hello(s, h)) return tls_fail(s, AL_INTERNAL_ERROR, ENOMEM, "TLS: out of memory");
        if (send_hs(s, h, h->msg.p, h->msg.len) < 0 || out_flush(s) < 0) return -1;
        if (hs_expect(s, HS_SERVER_HELLO, &m, &len, NULL) < 0 || server_hello_read(s, h, m, len, &sh) < 0) return -1;
        if (sh.hrr) return FAIL(s, AL_UNEXPECTED_MESSAGE, "TLS: a second HelloRetryRequest");
        if (!sh.version) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: TLS 1.2 after a HelloRetryRequest");
    }
    if (!sh.version) {
        int r = tls12(s, h, &sh, m, len);
        if (r < 0 && h->sess12_offered) resume_drop12(s->session_key);
        return r;
    }
    const struct suite *su = suite_of((uint16_t)sh.suite);
    if (!su || su->version != TLS13 || (h->hrr && su != h->hrr_suite)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose cipher suite %04x", sh.suite);
    if (sh.compression || sh.tls12_ext || sh.has_cookie) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: malformed ServerHello");
    if (!sh.has_ks) return FAIL(s, AL_MISSING_EXTENSION, "TLS: ServerHello without key_share");
    const struct share *mine = NULL;
    for (size_t i = 0; i < h->share_count; i++) if (h->shares[i].group == sh.ks_group) mine = &h->shares[i];
    if (!mine) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose a group not shared");
    if (!share_secret(mine, sh.ks.p, sh.ks.n, h->shared, &h->shared_len)) return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: bad key share");
    bool resumed = false;
    if (sh.has_psk) { /* §4.2.11: the one identity offered, with its hash */
        if (!h->psk_offered || sh.psk_identity != 0 || suite_of(h->psk.suite)->hash_len != su->hash_len)
            return FAIL(s, AL_ILLEGAL_PARAMETER, "TLS: server chose a PSK not offered");
        resumed = true;
    }
    s->version = TLS13; s->suite = su;
    th_add(&h->th, m, len);
    return tls13(s, h, resumed);
}

static net_stream *open_stream(const char *host, uint16_t port, bool tls, int timeout_ms,
                               char *err, size_t errlen, struct http_transport_timing *timing,
                               bool http,const char *partition,int (*cancelled)(void *),void *cancel_ctx) {
    uint64_t resolve_started=0,tls_started=0;
    if(timing){memset(timing,0,sizeof *timing);timing->phase=HTTP_PHASE_RESOLVE;resolve_started=uptime_ms();}
    if(cancelled&&cancelled(cancel_ctx)){errno=ECANCELED;snprintf(err,errlen,"request cancelled");return NULL;}
    /* host is a URL's host serialized (http.c's http_target): an IPv6 address comes in brackets,
       and the network stack (SYS_NET_*) has IPv4 only. */
    if (host && host[0] == '[') {
        if(timing)timing->resolve_ms=0;
        errno=N_ERR_UNSUPPORTED;snprintf(err, errlen, "%s: the network has no IPv6", host);
        return NULL;
    }
    if (!net_wait_up(5000)) {
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        errno=N_ERR_NETWORK_DOWN;snprintf(err, errlen, "network is down");
        return NULL;
    }
    uint32_t first[16],*ips=first;
    struct n_dns_result dns;
    uint64_t lookup_started=uptime_ms();
    n_result resolved=net_resolve_all(host, first,16,&dns);
    if (resolved < 0) {
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        errno=(int)-resolved;
        snprintf(err, errlen, "%s: %s", host, resolved == -N_ERR_NOT_FOUND ? "host not found" : n_result_text(resolved));
        return NULL;
    }
    size_t count=dns.count;
    if(count>16) {
        if(count>SIZE_MAX/sizeof *ips || !(ips=malloc(count*sizeof *ips))) {
            errno=ENOMEM;snprintf(err,errlen,"out of memory resolving host");return NULL;
        }
        resolved=net_resolve_all(host,ips,count,&dns);
        if(resolved<0){
            free(ips);errno=(int)-resolved;
            snprintf(err,errlen,"%s: %s",host,n_result_text(resolved));return NULL;
        }
        if(dns.count<count)count=dns.count;
    }
    /* Preserve the established TCP-only connect policy. TLS has always used
       the caller's read allowance; spending TCP time must not silently reduce
       that allowance or reject a previously supported slower handshake. */
    int connect_ms=timeout_ms>0&&timeout_ms<20000?timeout_ms:20000;
    uint64_t connect_started=uptime_ms();
    if(timing){timing->resolve_ms=connect_started-resolve_started;timing->phase=HTTP_PHASE_CONNECT;}
    uint64_t connect_deadline=connect_started+(uint64_t)connect_ms;
    n_result sock=-N_ERR_NOT_FOUND;uint32_t attempts=0,peer_ip=0;
    for(size_t i=0;i<count;i++) {
        uint64_t now=uptime_ms();if(now>=connect_deadline){sock=-N_ERR_TIMEOUT;break;}
        /* An unresponsive first address must leave later DNS candidates a
           fair share of the one connect deadline. */
        uint64_t left=connect_deadline-now,remaining=count-i;
        int allowance=(int)(left/remaining);if(allowance<1)allowance=1;
        attempts++;sock=tcp_connect(ips[i],port,allowance);if(sock>=0){peer_ip=ips[i];break;}
        if(sock!=-N_ERR_TIMEOUT&&sock!=-N_ERR_CONNECTION_REFUSED&&sock!=-N_ERR_UNREACHABLE)break;
    }
    if(ips!=first)free(ips);
    uint64_t connected=uptime_ms();
    if(timing){tls_started=connected;timing->connect_ms=tls_started-connect_started;}
    if (sock < 0) {
        errno=(int)-sock;
        snprintf(err, errlen, "connect to %s:%d: %s", host, port, n_result_text(sock));
        return NULL;
    }
    net_stream *s = calloc(1, tls ? sizeof *s : TLS_FIELDS);
    if (!s) {
        tcp_close((n_handle)sock);
        errno=ENOMEM;snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->sock = (n_handle)sock;
    s->connection=(struct ns_connection_info){1,(uint32_t)count,attempts,peer_ip};
    /* Fetch: the lookup ends as the connection starts; a TLS connection is
       established when its handshake is done, its secure start just before. */
    s->timing_info=(struct http_connection_timing){.domain_lookup_start=lookup_started,
        .domain_lookup_end=connect_started,.connect_start=connect_started,.connect_end=connected,
        .secure_connection_start=tls?connected:0};
    s->tls = tls;
    s->http_alpn=http;
    s->cancelled=cancelled;s->cancel_ctx=cancel_ctx;
    s->timeout_ms = timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 60000;
    if (!tls) {s->cancelled=NULL;s->cancel_ctx=NULL;return s;}
    if(timing)timing->phase=HTTP_PHASE_TLS;
    s->handshaking=true;s->timing=timing;io_begin(s,s->timeout_ms);
    s->session_key=session_key(host,port,http,partition);
    int shaken=handshake(s,host);
    handshake_free(s);
    free(s->hsbuf);s->hsbuf=NULL;s->hs_len=s->hs_cap=s->hs_used=0;
    if(shaken<0)goto fail;
    if(s->h2&&s->version==TLS12&&(s->suite->kx==KX_RSA||s->suite->cipher>=C_AES128_CBC)) {
        /* Offering legacy suites preserves HTTP/1.1 fallback. An h2 server
           must choose an AEAD ephemeral-key suite (RFC 9113 9.2.2). */
        errno=EPROTO;snprintf(s->err,sizeof s->err,"HTTP/2 requires an AEAD ephemeral-key TLS suite");goto fail;
    }
    s->timing_info.connect_end=uptime_ms();
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    s->handshaking=false;s->io_limited=false;s->timing=NULL;
    s->cancelled=NULL;s->cancel_ctx=NULL;
    return s;
fail:
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    {
        int saved=errno;
        snprintf(err, errlen, "%s", s->err);
        tcp_close(s->sock);
        free(s->session_key);
        wipe(s,sizeof *s);
        free(s);
        errno=saved;
    }
    return NULL;
}
net_stream *ns_open(const char *host,uint16_t port,bool tls,int timeout_ms,char *err,size_t errlen) {
    return open_stream(host,port,tls,timeout_ms,err,errlen,NULL,false,NULL,NULL,NULL);
}
int ns_get_connection_info(const net_stream *s,struct ns_connection_info *out){
    if(!s||!out){errno=EINVAL;return -1;}*out=s->connection;return 0;
}
int ns_get_connection_timing(const net_stream *s,struct http_connection_timing *out){
    if(!s||!out){errno=EINVAL;return -1;}*out=s->timing_info;return 0;
}
net_stream *ns_open_timed(const char *host,uint16_t port,bool tls,int timeout_ms,char *err,size_t errlen,
                          struct http_transport_timing *timing) {
    return open_stream(host,port,tls,timeout_ms,err,errlen,timing,false,NULL,NULL,NULL);
}
net_stream *ns_open_http_timed(const char *host,uint16_t port,bool tls,int timeout,
    char *err,size_t cap,struct http_transport_timing *timing,const char *partition) {
    return open_stream(host,port,tls,timeout,err,cap,timing,true,partition,NULL,NULL);
}
net_stream *ns_open_http_cancelled_timed(const char *host,uint16_t port,bool tls,int timeout,
    char *err,size_t cap,struct http_transport_timing *timing,const char *partition,
    int (*cancelled)(void *),void *ctx) {
    return open_stream(host,port,tls,timeout,err,cap,timing,true,partition,cancelled,ctx);
}
const char *ns_protocol(net_stream *s){return s->h2?"h2":"http/1.1";}
bool ns_session_resumed(net_stream *s){return s->resumed;}
void ns_set_cancel(net_stream *s,int (*cancelled)(void *),void *ctx){s->cancelled=cancelled;s->cancel_ctx=ctx;}
uint64_t ns_bytes_sent(net_stream *s){return s->bytes_sent;}

long ns_write(net_stream *s, const void *buf, size_t n) {
    if (!s->tls) {
        io_begin(s,s->timeout_ms);size_t sent=0;
        while(sent<n){int r=sock_write(s,(const unsigned char*)buf+sent,n-sent);
            if(r<0){s->io_limited=false;return -1;}sent+=(size_t)r;}
        s->io_limited=false;return (long)sent;
    }
    if (s->failed) return failed_result(s);
    if (s->close_sent) { errno=EPIPE; snprintf(s->err,sizeof s->err,"TLS: connection closing"); return -1; }
    io_begin(s,s->timeout_ms);
    const unsigned char *p = buf;
    size_t left = n;
    while (left) {
        if (s->version == TLS13 && s->wr.a13.seq >= REKEY_RECORDS) s->ku_due = true;
        key_update_send(s);
        if (s->ku_due) { if (out_flush(s) < 0) goto fail; key_update_send(s); }
        size_t take = left < PLAIN_MAX ? left : PLAIN_MAX;
        if (!rec_seal(s, CT_APP, p, take)) {
            if (out_flush(s) < 0) goto fail;
            if (!rec_seal(s, CT_APP, p, take)) { FAIL(s, AL_INTERNAL_ERROR, "TLS: record queue"); goto fail; }
        }
        p += take; left -= take;
    }
    if (out_flush(s) < 0) goto fail;
    s->io_limited=false;
    return (long)n;
fail:
    s->io_limited=false;
    return -1;
}

long ns_read(net_stream *s, void *buf, size_t n) {
    if (!s->tls) {
        io_begin(s,s->timeout_ms);
        int timeout;
        while(io_timeout(s,&timeout)==0) {
            int slice=s->cancelled&&(timeout<0||timeout>100)?100:timeout;
            n_result r=tcp_recv(s->sock,buf,n,slice);
            if(r<0&&(r==-N_ERR_NOT_READY||r==-N_ERR_TIMEOUT)&&slice!=timeout)continue;
            s->io_limited=false;
            if(r>=0)return r;
            errno=(int)-r;
            snprintf(s->err, sizeof s->err, "%s", r == -N_ERR_NOT_READY ? "timed out" : n_result_text(r));
            return -1;
        }
        s->io_limited=false;
        return -1; /* io_timeout set errno and the message */
    }
    io_begin(s,s->timeout_ms);
    if (s->wstart < s->wlen && !s->failed && out_flush(s) < 0) { s->io_limited=false; return -1; }
    int r = app_fill(s, true);
    s->io_limited=false;
    if (r == 1) return app_take(s, buf, n);
    return r == 0 ? 0 : -1;
}

long ns_try_read(net_stream *s,void *buf,size_t n) {
    if(io_cancelled(s))return -1;
    if(!s->tls) {
        n_result got=tcp_recv(s->sock,buf,n,0);
        if(got==-N_ERR_NOT_READY)return -2;
        if(got<0){errno=(int)-got;return -1;}
        return got;
    }
    if (s->wstart < s->wlen && !s->failed && out_flush_some(s) < 0) return -1;
    int r = app_fill(s, false);
    if (r == 1) return app_take(s, buf, n);
    return r;
}
/* ns_try_read that sleeps up to timeout_ms for the first transport bytes. A
   timeout is pending (-2), not an error: the TLS stream stays usable. The
   optional lock is held by the caller and released only while the transport
   sleeps: received bytes land past the records already in rbuf, which the
   send operations of another thread never touch. Queued records that the
   transport does not take shorten the sleep, so a reader also sends them. */
long ns_read_wait(net_stream *s,void *buf,size_t n,int timeout_ms,mtx_t *lock) {
    if(io_cancelled(s))return -1;
    if(!s->tls) {
        if(lock)mtx_unlock(lock);
        n_result got=tcp_recv(s->sock,buf,n,timeout_ms);
        if(lock)mtx_lock(lock);
        if(got==-N_ERR_NOT_READY||got==-N_ERR_TIMEOUT)return -2;
        if(got<0){errno=(int)-got;snprintf(s->err,sizeof s->err,"%s",n_result_text(got));return -1;}
        return got;
    }
    long got=ns_try_read(s,buf,n);
    if(got!=-2||!timeout_ms)return got;
    int wait=timeout_ms;
    if(s->wstart<s->wlen&&(wait<0||wait>10))wait=10;
    if(s->rlen>=RECORD_IN_MAX)return -2;
    unsigned char *p=s->rbuf+s->rlen;size_t room=RECORD_IN_MAX-s->rlen;
    if(lock)mtx_unlock(lock);
    n_result received=tcp_recv(s->sock,p,room,wait);
    if(lock)mtx_lock(lock);
    if(received==-N_ERR_NOT_READY||received==-N_ERR_TIMEOUT)return -2;
    if(received<=0) {
        snprintf(s->err,sizeof s->err,"TLS transport: %s",n_result_text(transport_failed(s,received)));
        return transport_lost(s);
    }
    s->rlen+=(size_t)received;
    return ns_try_read(s,buf,n);
}
long ns_try_write(net_stream *s,const void *buf,size_t n) {
    if(io_cancelled(s))return -1;
    if(!s->tls) {
        if(!n)return 0;n_result got=tcp_send_some(s->sock,buf,n);
        if(got==-N_ERR_NOT_READY)return -2;
        if(got<0){errno=(int)-got;snprintf(s->err,sizeof s->err,"%s",n_result_text(got));return -1;}
        s->bytes_sent+=(uint64_t)got;
        return got;
    }
    if(s->failed)return failed_result(s);
    if(out_flush_some(s)<0)return -1;
    if(s->ku_due){key_update_send(s);if(out_flush_some(s)<0)return -1;}
    /* New records only behind an empty queue: what was accepted is always on its way. */
    if(s->wstart<s->wlen||s->ku_due)return -2;
    if(!n)return 0;
    if(s->close_sent){errno=EPIPE;snprintf(s->err,sizeof s->err,"TLS: connection closing");return -1;}
    if(s->version==TLS13&&s->wr.a13.seq>=REKEY_RECORDS){s->ku_due=true;key_update_send(s);}
    size_t take=n<PLAIN_MAX?n:PLAIN_MAX;
    if(!rec_seal(s,CT_APP,buf,take))return FAIL(s,AL_INTERNAL_ERROR,"TLS: record queue");
    if(out_flush_some(s)<0)return -1;
    return (long)take;
}

void ns_set_timeout(net_stream *s, int timeout_ms) { s->timeout_ms = timeout_ms; }
const char *ns_error(net_stream *s) { return s->err; }

void ns_abort(net_stream *s) {
    if(!s)return;
    tcp_close(s->sock);free(s->session_key);
    size_t size=s->tls?sizeof *s:TLS_FIELDS;
    if(s->tls){handshake_free(s);free(s->hsbuf);}
    wipe(s,size);free(s);
}

void ns_reset(net_stream *s) { if(s)tcp_cancel(s->sock); }

/* close_notify (RFC 8446 §6.1), sent with whatever is queued within a second; the peer's own
   close_notify is not waited for (§6.1 and RFC 5246 §7.2.1 allow it). */
void ns_close(net_stream *s) {
    if (!s) return;
    if (s->tls && !s->failed && !s->close_sent) {
        static const unsigned char close_notify[2] = {1, AL_CLOSE_NOTIFY};
        s->closing=true;io_begin(s,1000);
        if (rec_seal(s, CT_ALERT, close_notify, 2) || (out_flush(s) == 0 && rec_seal(s, CT_ALERT, close_notify, 2))) {
            s->close_sent = true;
            out_flush(s);
        }
    }
    ns_abort(s);
}
