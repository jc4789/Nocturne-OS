/* net_stream: TCP connections, optionally wrapped in TLS 1.2 by BearSSL.
   Certificates are checked against the Mozilla root store compiled in by scripts/mkroots.py and
   the certificate authorities added to the system in N_FOLDER_CERTIFICATES (trust_anchors), and
   the current time comes from the CMOS clock. Entropy for the handshake comes from the system entropy pool (n_random). */
#include <stdio.h>
#include <stdlib.h>
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
            free(object.data);
            return;
        }
    }
    free(object.data);
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
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* Session secrets are process-local, origin/partition/ALPN separated, and
   bounded independently of socket lifetime. No certificate check is bypassed
   on a full handshake; BearSSL only resumes the authenticated master secret. */
struct tls_session {
    char *key;
    uint64_t saved;
    br_ssl_session_parameters parameters;
};
static struct tls_session sessions[16];
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
static bool session_load(net_stream *s) {
    if(!s->session_key)return false;
    bool found=false;uint64_t now=uptime_ms();session_enter();
    for(unsigned i=0;i<16;i++)if(sessions[i].key&&!strcmp(sessions[i].key,s->session_key)&&
        now-sessions[i].saved<1800000u) {
        br_ssl_engine_set_session_parameters(&s->cc.eng,&sessions[i].parameters);
        found=true;break;
    }
    session_leave();return found;
}
static void session_save(net_stream *s) {
    if(!s->session_key)return;
    br_ssl_session_parameters params;
    br_ssl_engine_get_session_parameters(&s->cc.eng,&params);
    if(!params.session_id_len)return;
    char *key=strdup(s->session_key);if(!key)return;
    session_enter();unsigned slot=0;
    for(unsigned i=0;i<16;i++) {
        if(sessions[i].key&&!strcmp(sessions[i].key,key)){slot=i;break;}
        if(!sessions[i].key){slot=i;break;}
        if(sessions[i].saved<sessions[slot].saved)slot=i;
    }
    free(sessions[slot].key);memset(&sessions[slot],0,sizeof sessions[slot]);
    sessions[slot].key=key;sessions[slot].saved=uptime_ms();sessions[slot].parameters=params;
    session_leave();memset(&params,0,sizeof params);
}
void ns_sessions_clear(void) {
    session_enter();for(unsigned i=0;i<16;i++) {
        free(sessions[i].key);memset(&sessions[i],0,sizeof sessions[i]);
    }session_leave();
}
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

static const char *br_errstr(int e) {
    switch (e) {
    case BR_ERR_X509_EXPIRED: return "certificate expired or not yet valid (is the clock right?)";
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate does not match the host name";
    case BR_ERR_BAD_VERSION: return "server does not support TLS 1.2";
    case BR_ERR_BAD_CIPHER_SUITE: return "no common cipher suite";
    case BR_ERR_NO_RANDOM: return "no entropy source";
    case BR_ERR_IO: return "connection error";
    }
    return NULL;
}

static void set_tls_error(net_stream *s) {
    int e = br_ssl_engine_last_error(&s->cc.eng);
    if (e == BR_ERR_OK) return;
    errno=s->transport_errno?s->transport_errno:
        (e>=BR_ERR_X509_OK&&e<=BR_ERR_X509_NOT_TRUSTED?EACCES:EPROTO);
    const char *m = br_errstr(e);
    if (e == BR_ERR_IO && s->err[0]) {
        char detail[sizeof s->err];
        snprintf(detail,sizeof detail,"%s [TLS=%d transport=%d eof=%u]",
                 s->err,e,s->transport_errno,s->transport_eof);
        strcpy(s->err,detail);
        return;
    }
    if (m) snprintf(s->err, sizeof s->err, "TLS: %s", m);
    else if (e >= BR_ERR_SEND_FATAL_ALERT) snprintf(s->err, sizeof s->err, "TLS: alert %d", e - BR_ERR_SEND_FATAL_ALERT);
    else if (e >= BR_ERR_RECV_FATAL_ALERT) snprintf(s->err, sizeof s->err, "TLS: server sent alert %d", e - BR_ERR_RECV_FATAL_ALERT);
    else snprintf(s->err, sizeof s->err, "TLS error %d", e);
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
    net_stream *s = calloc(1, tls ? sizeof *s : offsetof(net_stream, cc));
    if (!s) {
        tcp_close((n_handle)sock);
        snprintf(err, errlen, "out of memory");
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

    size_t anchors_known;
    const br_x509_trust_anchor *known = trust_anchors(&anchors_known);
    br_ssl_client_init_full(&s->cc, &s->xc, known, anchors_known);
    br_ssl_engine_set_buffer(&s->cc.eng, s->iobuf, sizeof s->iobuf, 1);
    static const char *alpn[] = {"h2","http/1.1"};
    br_ssl_engine_set_protocol_names(&s->cc.eng, http?alpn:alpn+1, http?2:1);
    s->session_key=session_key(host,port,http,partition);
    bool resume=session_load(s);
    br_ssl_session_parameters offered={0};
    if(resume)br_ssl_engine_get_session_parameters(&s->cc.eng,&offered);
    if (!br_ssl_client_reset(&s->cc, host, resume)) {
        set_tls_error(s);
        goto fail;
    }
    br_sslio_init(&s->io, &s->cc.eng, sock_read, s, sock_write, s);
    /* force the handshake now so errors are reported by ns_open */
    if (br_sslio_flush(&s->io) < 0 || (br_ssl_engine_current_state(&s->cc.eng) & BR_SSL_CLOSED)) {
        set_tls_error(s);
        if (!s->err[0]) snprintf(s->err, sizeof s->err, "TLS handshake failed");
        goto fail;
    }
    const char *selected=br_ssl_engine_get_selected_protocol(&s->cc.eng);
    s->h2=selected&&!strcmp(selected,"h2");
    /* Only the IDs offered above can be selected, all shorter than the field. */
    if(selected&&strlen(selected)<sizeof s->timing_info.protocol)strcpy(s->timing_info.protocol,selected);
    br_ssl_session_parameters negotiated;
    br_ssl_engine_get_session_parameters(&s->cc.eng,&negotiated);
    s->resumed=resume&&offered.session_id_len&&offered.session_id_len==negotiated.session_id_len&&
        offered.version==negotiated.version&&offered.cipher_suite==negotiated.cipher_suite&&
        !memcmp(offered.session_id,negotiated.session_id,offered.session_id_len)&&
        !memcmp(offered.master_secret,negotiated.master_secret,sizeof offered.master_secret);
    memset(&offered,0,sizeof offered);memset(&negotiated,0,sizeof negotiated);
    if(s->h2) {
        /* Offering legacy suites preserves HTTP/1.1 fallback. An h2 server
           must choose an AEAD ephemeral-key suite (RFC 9113 9.2.2). */
        uint16_t suite=s->cc.eng.session.cipher_suite;
        if(suite!=BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256&&
           suite!=BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256&&
           suite!=BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384&&
           suite!=BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384&&
           suite!=BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256&&
           suite!=BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256) {
            errno=EPROTO;snprintf(s->err,sizeof s->err,"HTTP/2 requires an AEAD ephemeral-key TLS suite");goto fail;
        }
    }
    session_save(s);
    s->timing_info.connect_end=uptime_ms();
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    s->handshaking=false;s->io_limited=false;s->timing=NULL;
    s->cancelled=NULL;s->cancel_ctx=NULL;
    return s;
fail:
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    snprintf(err, errlen, "%s", s->err);
    tcp_close(s->sock);
    free(s->session_key);
    free(s);
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
    io_begin(s,s->timeout_ms);
    if (br_sslio_write_all(&s->io, buf, n) < 0 || br_sslio_flush(&s->io) < 0) {
        set_tls_error(s);
        s->io_limited=false;
        return -1;
    }
    s->io_limited=false;
    return (long)n;
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
    int r = br_sslio_read(&s->io, buf, n);
    s->io_limited=false;
    if (r >= 0) return r;
    if (br_ssl_engine_last_error(&s->cc.eng) == BR_ERR_OK) return 0; /* clean close_notify */
    set_tls_error(s);
    return -1;
}

long ns_try_read(net_stream *s,void *buf,size_t n) {
    if(io_cancelled(s))return -1;
    if(!s->tls) {
        n_result got=tcp_recv(s->sock,buf,n,0);
        if(got==-N_ERR_NOT_READY)return -2;
        if(got<0){errno=(int)-got;return -1;}
        return got;
    }
    for(unsigned steps=0;steps<32;steps++) {
        unsigned state=br_ssl_engine_current_state(&s->cc.eng);size_t size;
        unsigned char *p;
        if(state&BR_SSL_RECVAPP) {
            p=br_ssl_engine_recvapp_buf(&s->cc.eng,&size);if(size>n)size=n;
            memcpy(buf,p,size);br_ssl_engine_recvapp_ack(&s->cc.eng,size);return (long)size;
        }
        if(state&BR_SSL_CLOSED) {
            if(br_ssl_engine_last_error(&s->cc.eng)==BR_ERR_OK)return 0;
            set_tls_error(s);return -1;
        }
        if(state&BR_SSL_SENDREC) {
            p=br_ssl_engine_sendrec_buf(&s->cc.eng,&size);
            n_result got=tcp_send_some(s->sock,p,size);
            if(got==-N_ERR_NOT_READY)return -2;
            if(got<=0){transport_failed(s,got);set_tls_error(s);return -1;}
            s->bytes_sent+=(uint64_t)got;
            br_ssl_engine_sendrec_ack(&s->cc.eng,(size_t)got);continue;
        }
        if(state&BR_SSL_RECVREC) {
            p=br_ssl_engine_recvrec_buf(&s->cc.eng,&size);
            n_result got=tcp_recv(s->sock,p,size,0);
            if(got==-N_ERR_NOT_READY)return -2;
            if(got<=0) {
                snprintf(s->err,sizeof s->err,"TLS transport: %s",n_result_text(transport_failed(s,got)));return -1;
            }
            br_ssl_engine_recvrec_ack(&s->cc.eng,(size_t)got);continue;
        }
        return -2;
    }
    return -2;
}
/* ns_try_read that sleeps up to timeout_ms for the first transport bytes. A
   timeout is pending (-2), not an error: the TLS engine stays usable. The
   optional engine lock is held by the caller and released only while the
   transport sleeps: received bytes land in the engine's receive half, which
   the send operations of another thread never touch. */
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
    for(unsigned steps=0;steps<32;steps++) {
        long got=ns_try_read(s,buf,n);
        if(got!=-2||!timeout_ms)return got;
        unsigned state=br_ssl_engine_current_state(&s->cc.eng);
        if(!(state&BR_SSL_RECVREC)||(state&BR_SSL_SENDREC))return -2;
        size_t size;unsigned char *p=br_ssl_engine_recvrec_buf(&s->cc.eng,&size);
        if(lock)mtx_unlock(lock);
        n_result received=tcp_recv(s->sock,p,size,timeout_ms);
        if(lock)mtx_lock(lock);
        if(received==-N_ERR_NOT_READY||received==-N_ERR_TIMEOUT)return -2;
        if(received<=0) {
            snprintf(s->err,sizeof s->err,"TLS transport: %s",n_result_text(transport_failed(s,received)));return -1;
        }
        got=received;
        br_ssl_engine_recvrec_ack(&s->cc.eng,(size_t)got);
        timeout_ms=0;
    }
    return -2;
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
    for(unsigned steps=0;steps<32;steps++) {
        unsigned state=br_ssl_engine_current_state(&s->cc.eng);size_t size;unsigned char *p;
        if(state&BR_SSL_CLOSED){set_tls_error(s);if(!errno)errno=ECONNRESET;return -1;}
        if(state&BR_SSL_SENDREC) {
            p=br_ssl_engine_sendrec_buf(&s->cc.eng,&size);n_result got=tcp_send_some(s->sock,p,size);
            if(got==-N_ERR_NOT_READY)return -2;
            if(got<=0){transport_failed(s,got);set_tls_error(s);return -1;}
            s->bytes_sent+=(uint64_t)got;br_ssl_engine_sendrec_ack(&s->cc.eng,(size_t)got);continue;
        }
        if(state&BR_SSL_SENDAPP) {
            if(!n)return 0;p=br_ssl_engine_sendapp_buf(&s->cc.eng,&size);if(size>n)size=n;
            memcpy(p,buf,size);br_ssl_engine_sendapp_ack(&s->cc.eng,size);
            br_ssl_engine_flush(&s->cc.eng,0);return (long)size;
        }
        /* The receive pump handles peer alerts/handshake records; a pending
           send never destroys the engine or waits while holding its lock. */
        return -2;
    }
    return -2;
}

void ns_set_timeout(net_stream *s, int timeout_ms) { s->timeout_ms = timeout_ms; }
const char *ns_error(net_stream *s) { return s->err; }

void ns_abort(net_stream *s) {
    if(!s)return;
    tcp_close(s->sock);free(s->session_key);
    size_t size=s->tls?sizeof *s:offsetof(net_stream,cc);
    memset(s,0,size);free(s);
}

void ns_reset(net_stream *s) { if(s)tcp_cancel(s->sock); }

void ns_close(net_stream *s) {
    if (!s) return;
    if (s->tls && !(br_ssl_engine_current_state(&s->cc.eng) & BR_SSL_CLOSED)) {
        s->timeout_ms = 1000; /* don't hang waiting for the server's close_notify */
        s->closing=true;io_begin(s,1000);
        br_sslio_close(&s->io);
    }
    ns_abort(s);
}
