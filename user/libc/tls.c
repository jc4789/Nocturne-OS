/* net_stream: TCP connections, optionally wrapped in TLS 1.2 by BearSSL.
   Certificates are checked against the Mozilla root store compiled in by scripts/mkroots.py, and
   the current time comes from the CMOS clock. Entropy for the handshake comes from /dev/urandom. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "bearssl.h"
#include "http.h"
#include "nocturne.h"

extern const br_x509_trust_anchor tls_trust_anchors[];
extern const size_t tls_trust_anchors_num;

struct net_stream {
    int sock;
    int timeout_ms;
    bool tls;
    bool io_limited, handshaking, closing, transport_eof;
    int transport_errno;
    uint64_t io_deadline;
    uint64_t bytes_sent;
    struct ns_connection_info connection;
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
static unsigned session_lock;
static void session_enter(void) {
    while (__atomic_exchange_n(&session_lock,1,__ATOMIC_ACQUIRE)) yield();
}
static void session_leave(void) { __atomic_store_n(&session_lock,0,__ATOMIC_RELEASE); }
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
    long r;
    do {
        int slice=s->cancelled&&(timeout<0||timeout>100)?100:timeout;
        r=tcp_recv(s->sock,buf,len,slice);
        if(r>=0||(errno!=EAGAIN&&errno!=ETIMEDOUT)||slice==timeout)break;
        if(io_timeout(s,&timeout)<0)return -1;
    }while(1);
    int cause=r<0?errno:0;
    if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
    if (r > 0)return (int)r;
    s->transport_errno=cause;s->transport_eof=r==0;
    if(r==0)snprintf(s->err,sizeof s->err,"peer closed connection%s",
                     s->handshaking?" during TLS handshake":"");
    else snprintf(s->err,sizeof s->err,"%s%s",
                  s->handshaking?"TLS handshake: ":"",
                  s->transport_errno==EAGAIN||s->transport_errno==ETIMEDOUT?"timed out":strerror(s->transport_errno));
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
    long r;
    do {
        uint64_t deadline=s->io_limited?s->io_deadline:0;
        if(s->cancelled){uint64_t slice=uptime_ms()+100;if(!deadline||slice<deadline)deadline=slice;}
        r=tcp_send_until(s->sock,buf,len,deadline);
        if(r>=0||errno!=ETIMEDOUT||!s->cancelled)break;
        if(io_timeout(s,&timeout)<0)return -1;
    }while(1);
    int cause=r<0?errno:0;
    if(s->handshaking&&s->timing)s->timing->tls_io_ms+=uptime_ms()-callback_started;
    if (r > 0){s->bytes_sent+=(uint64_t)r;return (int)r;}
    s->transport_errno=cause;s->transport_eof=r==0;
    snprintf(s->err,sizeof s->err,"%s",r==0?"connection closed while sending":strerror(s->transport_errno));
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
    if (!net_wait_up(5000)) {
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        snprintf(err, errlen, "network is down");
        return NULL;
    }
    uint32_t first[16],*ips=first;
    struct n_dns_result dns;
    if (net_resolve_all(host, first,16,&dns) < 0) {
        int cause=errno;
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        snprintf(err, errlen, "%s: %s", host, cause == ENOENT ? "host not found" : strerror(cause));
        return NULL;
    }
    size_t count=dns.count;
    if(count>16) {
        if(count>SIZE_MAX/sizeof *ips || !(ips=malloc(count*sizeof *ips))) {
            errno=ENOMEM;snprintf(err,errlen,"out of memory resolving host");return NULL;
        }
        if(net_resolve_all(host,ips,count,&dns)<0){free(ips);return NULL;}
        if(dns.count<count)count=dns.count;
    }
    /* Preserve the established TCP-only connect policy. TLS has always used
       the caller's read allowance; spending TCP time must not silently reduce
       that allowance or reject a previously supported slower handshake. */
    int connect_ms=timeout_ms>0&&timeout_ms<20000?timeout_ms:20000;
    uint64_t connect_started=uptime_ms();
    if(timing){timing->resolve_ms=connect_started-resolve_started;timing->phase=HTTP_PHASE_CONNECT;}
    uint64_t connect_deadline=connect_started+(uint64_t)connect_ms;
    int sock=-1;uint32_t attempts=0,peer_ip=0;
    for(size_t i=0;i<count;i++) {
        uint64_t now=uptime_ms();if(now>=connect_deadline){errno=ETIMEDOUT;break;}
        /* An unresponsive first address must leave later DNS candidates a
           fair share of the one connect deadline. */
        uint64_t left=connect_deadline-now,remaining=count-i;
        int allowance=(int)(left/remaining);if(allowance<1)allowance=1;
        attempts++;sock=tcp_connect(ips[i],port,allowance);if(sock>=0){peer_ip=ips[i];break;}
        if(errno!=ETIMEDOUT&&errno!=ECONNREFUSED&&errno!=EHOSTUNREACH&&errno!=ENETUNREACH)break;
    }
    if(ips!=first)free(ips);
    int connect_error=sock<0?errno:0;
    if(timing){tls_started=uptime_ms();timing->connect_ms=tls_started-connect_started;}
    if (sock < 0) {
        snprintf(err, errlen, "connect to %s:%d: %s", host, port, strerror(connect_error));
        return NULL;
    }
    net_stream *s = calloc(1, tls ? sizeof *s : offsetof(net_stream, cc));
    if (!s) {
        tcp_close(sock);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->sock = sock;
    s->connection=(struct ns_connection_info){1,(uint32_t)count,attempts,peer_ip};
    s->tls = tls;
    s->http_alpn=http;
    s->cancelled=cancelled;s->cancel_ctx=cancel_ctx;
    s->timeout_ms = timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 60000;
    if (!tls) {s->cancelled=NULL;s->cancel_ctx=NULL;return s;}
    if(timing)timing->phase=HTTP_PHASE_TLS;
    s->handshaking=true;s->timing=timing;io_begin(s,s->timeout_ms);

    br_ssl_client_init_full(&s->cc, &s->xc, tls_trust_anchors, tls_trust_anchors_num);
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
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    s->handshaking=false;s->io_limited=false;s->timing=NULL;
    s->cancelled=NULL;s->cancel_ctx=NULL;
    return s;
fail:
    if(timing){timing->tls_ms=uptime_ms()-tls_started;
        timing->tls_other_ms=timing->tls_ms>=timing->tls_io_ms?timing->tls_ms-timing->tls_io_ms:0;}
    snprintf(err, errlen, "%s", s->err);
    tcp_close(sock);
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
        int timeout;long r=-1;
        while(io_timeout(s,&timeout)==0) {
            int slice=s->cancelled&&(timeout<0||timeout>100)?100:timeout;
            r=tcp_recv(s->sock,buf,n,slice);
            if(r>=0||(errno!=EAGAIN&&errno!=ETIMEDOUT)||slice==timeout)break;
        }
        s->io_limited=false;
        if (r < 0) snprintf(s->err, sizeof s->err, "%s", errno == EAGAIN ? "timed out" : strerror(errno));
        return r;
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
        long got=tcp_recv(s->sock,buf,n,0);
        if(got<0&&errno==EAGAIN)return -2;
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
            long got=tcp_send_some(s->sock,p,size);
            if(got<0&&errno==EAGAIN)return -2;
            if(got<=0){s->transport_errno=errno?errno:ECONNRESET;set_tls_error(s);return -1;}
            s->bytes_sent+=(uint64_t)got;
            br_ssl_engine_sendrec_ack(&s->cc.eng,(size_t)got);continue;
        }
        if(state&BR_SSL_RECVREC) {
            p=br_ssl_engine_recvrec_buf(&s->cc.eng,&size);
            long got=tcp_recv(s->sock,p,size,0);
            if(got<0&&errno==EAGAIN)return -2;
            if(got<=0) {
                s->transport_errno=errno=got==0?ECONNRESET:errno;
                snprintf(s->err,sizeof s->err,"TLS transport: %s",strerror(errno));return -1;
            }
            br_ssl_engine_recvrec_ack(&s->cc.eng,(size_t)got);continue;
        }
        return -2;
    }
    return -2;
}
long ns_try_write(net_stream *s,const void *buf,size_t n) {
    if(io_cancelled(s))return -1;
    if(!s->tls) {
        if(!n)return 0;long got=tcp_send_some(s->sock,buf,n);
        if(got<0&&errno==EAGAIN)return -2;
        if(got>0)s->bytes_sent+=(uint64_t)got;
        else if(got<0)snprintf(s->err,sizeof s->err,"%s",strerror(errno));
        return got;
    }
    for(unsigned steps=0;steps<32;steps++) {
        unsigned state=br_ssl_engine_current_state(&s->cc.eng);size_t size;unsigned char *p;
        if(state&BR_SSL_CLOSED){set_tls_error(s);if(!errno)errno=ECONNRESET;return -1;}
        if(state&BR_SSL_SENDREC) {
            p=br_ssl_engine_sendrec_buf(&s->cc.eng,&size);long got=tcp_send_some(s->sock,p,size);
            if(got<0&&errno==EAGAIN)return -2;
            if(got<=0){s->transport_errno=errno?errno:ECONNRESET;set_tls_error(s);return -1;}
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

void ns_close(net_stream *s) {
    if (!s) return;
    if (s->tls && !(br_ssl_engine_current_state(&s->cc.eng) & BR_SSL_CLOSED)) {
        s->timeout_ms = 1000; /* don't hang waiting for the server's close_notify */
        s->closing=true;io_begin(s,1000);
        br_sslio_close(&s->io);
    }
    ns_abort(s);
}
