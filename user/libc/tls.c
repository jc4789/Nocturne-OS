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
    char err[128];
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

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
    int timeout;
    if(io_timeout(s,&timeout)<0)return -1;
    long r = tcp_recv(s->sock, buf, len, timeout);
    if (r > 0) return (int)r;
    s->transport_errno=r<0?errno:0;s->transport_eof=r==0;
    if(r==0)snprintf(s->err,sizeof s->err,"peer closed connection%s",
                     s->handshaking?" during TLS handshake":"");
    else snprintf(s->err,sizeof s->err,"%s%s",
                  s->handshaking?"TLS handshake: ":"",
                  s->transport_errno==EAGAIN||s->transport_errno==ETIMEDOUT?"timed out":strerror(s->transport_errno));
    return -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    net_stream *s = ctx;
    int timeout;
    if(io_timeout(s,&timeout)<0)return -1;
    long r = tcp_send(s->sock, buf, len);
    if (r > 0) return (int)r;
    s->transport_errno=r<0?errno:0;s->transport_eof=r==0;
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
                               char *err, size_t errlen, struct http_transport_timing *timing) {
    uint64_t resolve_started=0,tls_started=0;
    if(timing){memset(timing,0,sizeof *timing);timing->phase=HTTP_PHASE_RESOLVE;resolve_started=uptime_ms();}
    if (!net_wait_up(5000)) {
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        snprintf(err, errlen, "network is down");
        return NULL;
    }
    uint32_t ip;
    if (net_resolve(host, &ip) < 0) {
        int cause=errno;
        if(timing)timing->resolve_ms=uptime_ms()-resolve_started;
        snprintf(err, errlen, "%s: %s", host, cause == ENOENT ? "host not found" : strerror(cause));
        return NULL;
    }
    /* TCP plus TLS are one connection-establishment operation. The long
       application-read allowance (an LLM may think for minutes) does not
       apply to a handshake, and partial TLS records cannot restart it. */
    int connect_ms=timeout_ms>0&&timeout_ms<20000?timeout_ms:20000;
    uint64_t connect_started=uptime_ms(),connect_until=(uint64_t)connect_ms>UINT64_MAX-connect_started?
        UINT64_MAX:connect_started+(uint64_t)connect_ms;
    if(timing){timing->resolve_ms=connect_started-resolve_started;timing->phase=HTTP_PHASE_CONNECT;}
    int sock = tcp_connect(ip, port, connect_ms);
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
    s->tls = tls;
    s->timeout_ms = timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 60000;
    if (!tls) return s;
    if(timing)timing->phase=HTTP_PHASE_TLS;
    s->handshaking=true;s->io_limited=true;s->io_deadline=connect_until;

    br_ssl_client_init_full(&s->cc, &s->xc, tls_trust_anchors, tls_trust_anchors_num);
    br_ssl_engine_set_buffer(&s->cc.eng, s->iobuf, sizeof s->iobuf, 1);
    static const char *alpn[] = {"http/1.1"};
    br_ssl_engine_set_protocol_names(&s->cc.eng, alpn, 1);
    if (!br_ssl_client_reset(&s->cc, host, 0)) {
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
    if(timing)timing->tls_ms=uptime_ms()-tls_started;
    s->handshaking=false;s->io_limited=false;
    return s;
fail:
    if(timing)timing->tls_ms=uptime_ms()-tls_started;
    snprintf(err, errlen, "%s", s->err);
    tcp_close(sock);
    free(s);
    return NULL;
}
net_stream *ns_open(const char *host,uint16_t port,bool tls,int timeout_ms,char *err,size_t errlen) {
    return open_stream(host,port,tls,timeout_ms,err,errlen,NULL);
}
net_stream *ns_open_timed(const char *host,uint16_t port,bool tls,int timeout_ms,char *err,size_t errlen,
                          struct http_transport_timing *timing) {
    return open_stream(host,port,tls,timeout_ms,err,errlen,timing);
}

long ns_write(net_stream *s, const void *buf, size_t n) {
    if (!s->tls) {
        long r = tcp_send(s->sock, buf, n);
        if (r < 0) snprintf(s->err, sizeof s->err, "%s", strerror(errno));
        return r < 0 ? -1 : r;
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
        long r = tcp_recv(s->sock, buf, n, s->timeout_ms);
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

void ns_set_timeout(net_stream *s, int timeout_ms) { s->timeout_ms = timeout_ms; }
const char *ns_error(net_stream *s) { return s->err; }

void ns_close(net_stream *s) {
    if (!s) return;
    if (s->tls && !(br_ssl_engine_current_state(&s->cc.eng) & BR_SSL_CLOSED)) {
        s->timeout_ms = 1000; /* don't hang waiting for the server's close_notify */
        s->closing=true;io_begin(s,1000);
        br_sslio_close(&s->io);
    }
    tcp_close(s->sock);
    memset(s, 0, s->tls ? sizeof *s : offsetof(net_stream, cc)); /* wipe session keys */
    free(s);
}
