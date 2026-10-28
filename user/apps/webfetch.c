/* Persistent isolated browser transport. DNS, TCP and verified TLS never run on the GUI task. */
#include "nocturne.h"
#include "http.h"
#include "web.h"
#include "webnet_wire.h"
#include "webprefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include "browser_identity.h"
#include "png.h"
#include "bearssl.h"

struct header_buffer { char *p; size_t length, capacity; };
struct cache_entry;
static volatile uint32_t service_stopping;
struct job {
    struct webnet_wire_request wire;
    char *url, *document, *method, *headers, *request_body;
    char *origin, *target_origin;
    char *top_level_url,*top_origin;
    bool top_opaque,top_prepared;
    char *referrer,*referrer_origin;
    int referrer_policy;
    char *final_url;
    char error[160];
    struct header_buffer outgoing;
    char *non_simple;
    char *response_headers;
    size_t response_headers_len;
    /* WEBNET_WIRE_SENT_HEADERS: the request fields the latest hop sent, as
       http.c writes them (Host, User-Agent, Content-Length, then outgoing). */
    struct header_buffer sent;
    char *body;
    size_t body_len, body_cap;
    int status;
    webcookie_jar *cookies;
    unsigned char *cookie_events;
    size_t cookie_len, cookie_cap;
    bool hop_cookies, redirect_cross_site;
    bool cors_tainted, origin_tainted, redirected, opaque_tainted;
    /* Fetch request metadata (WEBNET_WIRE_FETCH_METADATA): the destination,
       WEBNET_FETCH_* flags, and the Sec-Fetch-Site relation of every URL in
       the request's URL list so far (0 same-origin, 1 same-site, 2 cross-site). */
    struct webnet_wire_fetch fetch;
    unsigned fetch_site;
    /* The Accept-Language the browser chose when the fetch began
       (WEBNET_WIRE_ACCEPT_LANGUAGE), checked; "" when the request has none. */
    char accept_language[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX + 1];
    /* An element's no-cors subresource (an image, a stylesheet) that left the
       request origin: its response tainting is opaque. The browser still uses
       the bytes, unlike fetch()'s opaque response, but they are not
       CORS-same-origin (a canvas or WebGL may not read them). */
    bool resource_opaque;
    /* Redirect mode "manual" (WEBNET_WIRE_REDIRECT_MANUAL) met a redirect status: that response is
       the fetch's, an opaque-redirect filtered one. Its fields and body never leave this process. */
    bool opaque_redirect;
    bool stream_headers_sent, stream_final, upload_finished;
    size_t cookie_start;
    const char *hop_method;
    bool debug_request_measured, debug_cookie, debug_content_length;
    uint64_t debug_header_bytes, debug_ctl, debug_non_ascii;
    struct http_transport_timing debug_timing;
    unsigned debug_attempts;
    struct http_transport_result debug_transport; /* the last attempt's connection */
    /* A debug request in flight: its transport phase, and since when it has
       not changed, so a stalled phase is reported while it still waits. */
    const struct http_transport_timing *live_timing;
    const struct http_transport_result *live_transport;
    unsigned stall_phase;
    uint64_t stall_since, stall_reported;
    n_handle input, output;
    n_handle events;         /* input's readiness, for timed waits */
    struct webnet_wire_in in; /* the input message being read */
    char *partition;
    struct cache_entry *cached;
    struct header_buffer cache_body;
    bool cache_capture, cache_replay;
    size_t output_credit;
    size_t input_consumed;
    unsigned char *backlog;
    size_t backlog_pos, backlog_len, backlog_cap;
    /* Fetch's timing record of this fetch (http.h), returned to the browser
       (WEBNET_WIRE_TIMING), which adds the start time it asked at. Each hop
       adds what its transport, the cache and the TAO check recorded:
       timing_allow_failed is the request's timing allow failed flag,
       redirect_tao a navigation's redirect hops' Timing-Allow-Origin values
       (its navigation timing allow values list; NULL when a hop had none),
       hop_decoded the current hop's decoded body bytes. */
    struct http_fetch_timing timing;
    bool timing_allow_failed;
    char **redirect_tao;
    size_t redirect_tao_count;
    uint64_t hop_decoded;
};
/* Response bytes the browser has not credited yet wait in the job, so the
   socket keeps being read while the browser's main thread is busy (a long
   script or wasm task). Same bandwidth-delay bound as the browser's own
   per-stream read-ahead; past it the job waits for credit again. */
#define SERVICE_READ_AHEAD (8u * 1024u * 1024u)
/* NAVIGATION is also the transport kind for iframe HTML. Only native chrome's
   explicit top-navigation flag changes the enclosing partition to a new hop. */
static bool top_level_navigation(const struct job *j) {
    return j->wire.kind==WEBNET_NAVIGATION&&(j->wire.user_navigation&WEBNET_WIRE_USER_NAVIGATION)!=0;
}
static bool cache_context_allowed(const struct job *j) {
    return top_level_navigation(j)||(j->origin&&!j->top_opaque);
}
/* Wait up to ms for the browser: 1 input (or its end) waits, 0 nothing yet,
   -1 the browser has gone or the service is stopping. */
static int input_wait(struct job *j,int ms) {
    if(j->in.at<j->in.length)return 1;
    if(__atomic_load_n(&service_stopping,__ATOMIC_ACQUIRE))return -1;
    struct n_event e;
    n_result got=n_events_wait(j->events,&e,1,ms);
    if(got<0||(got&&(e.events&N_EVENT_CLOSED)))return -1;
    return got>0;
}
static bool read_all(struct job *j,void *p,size_t n) {
    while(n){
        n_result k=webnet_wire_read(j->input,&j->in,p,n,0);
        if(k<0)return false;
        if(!k){if(input_wait(j,100)<0)return false;continue;}
        p=(char *)p+k;n-=(size_t)k;
    }
    return true;
}
/* One message; the browser's side never lets output exceed its window, so
   this waits only while the browser is far behind. */
static bool write_all(struct job *j,const void *p,size_t n) {
    return !n||webnet_wire_write(j->output,p,n)==(n_result)n;
}
/* Every transport wait polls the cancellation hook, so it also reports a
   debug request whose phase stays unchanged: the system log then shows where
   a request that never completes is waiting. */
#define STALL_REPORT_MS 5000u
static void stall_report(struct job *j,uint64_t now) {
    unsigned phase=j->live_timing->phase;
    if(phase!=j->stall_phase||!j->stall_since){j->stall_phase=phase;j->stall_since=j->stall_reported=now;return;}
    /* A long body transfer progresses; only setup and header waits stall. */
    if(phase>=HTTP_PHASE_BODY||now-j->stall_reported<STALL_REPORT_MS)return;
    j->stall_reported=now;
    static n_handle log=-2;
    n_handle h=__atomic_load_n(&log,__ATOMIC_ACQUIRE);
    if(h==-2){n_handle opened=n_system_log_open(),expected=-2;
        if(!__atomic_compare_exchange_n(&log,&expected,opened,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)&&opened>=0)n_handle_close(opened);
        h=__atomic_load_n(&log,__ATOMIC_ACQUIRE);}
    if(h<0)return;
    const struct http_transport_result *t=j->live_transport;
    /* The host names the stalled peer; path, query and userinfo stay private. */
    const char *host=j->url?strstr(j->url,"://"):NULL;host=host?host+3:"";
    size_t host_len=strcspn(host,"/?#");
    for(size_t i=host_len;i-->0;)if(host[i]=='@'){host+=i+1;host_len-=i+1;break;}
    if(host_len>96)host_len=96;
    char line[288];
    int n=snprintf(line,sizeof line,"[webfetch] waiting id=%lu phase=%u for %lu ms host=%.*s connection=%lu h2=%u stream=%lu\n",
        (unsigned long)j->wire.id,phase,(unsigned long)(now-j->stall_since),(int)host_len,host,
        (unsigned long)(t?t->connection_id:0),(unsigned)(t&&t->http2),(unsigned long)(t?t->stream_id:0));
    if(n>0)n_channel_send(h,line,(size_t)MIN(n,(int)sizeof line-1),-1);
}
static int request_cancelled(void *opaque) {
    struct job *j=opaque;
    uint64_t now=n_clock_uptime_ms();
    if(j->live_timing)stall_report(j,now);
    if(now>=j->wire.deadline)return 1;
    if(!(j->wire.user_navigation&WEBNET_WIRE_SERVICE))return 0;
    if(__atomic_load_n(&service_stopping,__ATOMIC_ACQUIRE))return 1;
    struct n_event e;
    n_result got=n_events_wait(j->events,&e,1,0);
    return got<0||(got&&(e.events&N_EVENT_CLOSED));
}
/* -1 failure, 0 no credit within wait_ms, 1 credit added. A grant returns
   bytes the browser consumed, so it never exceeds what is outstanding. */
static int credit_receive(struct job *j,int wait_ms) {
    int ready=input_wait(j,wait_ms);
    if(ready<=0)return ready;
    struct webnet_wire_credit credit;
    if(!read_all(j,&credit,sizeof credit)||credit.magic!=WEBNET_CREDIT_MAGIC||
       credit.id!=j->wire.id||!credit.bytes||credit.bytes>WEBNET_SERVICE_WINDOW-j->output_credit)return -1;
    j->output_credit+=credit.bytes;
    return 1;
}
/* Send what the credit allows; wait for more only while over keep bytes. */
static bool backlog_drain(struct job *j,size_t keep) {
    for(;;){
        size_t pending=j->backlog_len-j->backlog_pos;
        if(!pending){j->backlog_pos=j->backlog_len=0;return true;}
        if(!j->output_credit){
            bool wait=pending>keep;
            if(wait&&request_cancelled(j))return false;
            int got=credit_receive(j,wait?(int)MIN(j->wire.deadline-n_clock_uptime_ms(),100u):0);
            if(got<0)return false;
            if(!got){if(!wait)return true;continue;}
        }
        size_t count=MIN(pending,j->output_credit);
        if(!write_all(j,j->backlog+j->backlog_pos,count))return false;
        j->backlog_pos+=count;j->output_credit-=count;
    }
}
static bool job_write(struct job *j,const void *data,size_t length) {
    if(!(j->wire.user_navigation&WEBNET_WIRE_SERVICE))return write_all(j,data,length);
    if(request_cancelled(j))return false;
    if(length){
        if(j->backlog_pos&&j->backlog_pos>=j->backlog_len-j->backlog_pos){
            memmove(j->backlog,j->backlog+j->backlog_pos,j->backlog_len-j->backlog_pos);
            j->backlog_len-=j->backlog_pos;j->backlog_pos=0;
        }
        if(length>SIZE_MAX-j->backlog_len)return false;
        if(j->backlog_len+length>j->backlog_cap){
            size_t cap=MAX(j->backlog_len+length,MAX(j->backlog_cap*2,(size_t)WEBNET_STREAM_CHUNK_MAX));
            unsigned char *grown=realloc(j->backlog,cap);
            if(!grown)return false;
            j->backlog=grown;j->backlog_cap=cap;
        }
        memcpy(j->backlog+j->backlog_len,data,length);j->backlog_len+=length;
    }
    return backlog_drain(j,SERVICE_READ_AHEAD);
}
static bool job_read_all(struct job *j,void *data,size_t length) {
    char *p=data;
    while(length){
        if(request_cancelled(j))return false;
        size_t count=MIN(length,WEBNET_SERVICE_CREDIT_STEP);
        n_result got=webnet_wire_read(j->input,&j->in,p,count,0);
        if(got<0)return false;
        if(!got){if(input_wait(j,(int)MIN(j->wire.deadline-n_clock_uptime_ms(),100u))<0)return false;continue;}
        p+=got;length-=(size_t)got;
        if(j->wire.user_navigation&WEBNET_WIRE_SERVICE){
            j->input_consumed+=(size_t)got;
            if(j->input_consumed>=WEBNET_SERVICE_CREDIT_STEP){
                struct webnet_wire_response frame={0};frame.magic=WEBNET_STREAM_REQUEST_CREDIT;
                frame.id=j->wire.id;frame.generation=j->wire.generation;frame.status=(uint32_t)j->input_consumed;
                if(!write_all(j,&frame,sizeof frame))return false;
                j->input_consumed=0;
            }
        }
    }
    return true;
}
static char *read_string(struct job *j, uint32_t len, bool binary) {
    if ((uint64_t)len + 1 > SIZE_MAX) return NULL;
    char *p = malloc((size_t)len + 1);
    if (!p) return NULL;
    if (!job_read_all(j, p, len) || (!binary && memchr(p, 0, len))) { free(p); return NULL; }
    p[len] = 0; return p;
}
static bool fail(struct job *j, const char *message) {
    snprintf(j->error, sizeof j->error, "%s", message); return false;
}
static bool header_reserve(struct job *j, struct header_buffer *b, size_t extra) {
    if (b->length == SIZE_MAX || extra > SIZE_MAX - b->length - 1)
        return fail(j, "Request header allocation size overflow");
    size_t need = b->length + extra + 1;
    if (need <= b->capacity) return true;
    size_t capacity = b->capacity ? b->capacity : 256;
    while (capacity < need) capacity = capacity > SIZE_MAX / 2 ? need : capacity * 2;
    char *p = realloc(b->p, capacity);
    if (!p) return fail(j, "Out of memory preparing request headers");
    b->p = p; b->capacity = capacity; b->p[b->length] = 0;
    return true;
}
static bool header_append(struct job *j, struct header_buffer *b, const char *p, size_t n) {
    if (!header_reserve(j, b, n)) return false;
    if (n) memcpy(b->p + b->length, p, n);
    b->length += n; b->p[b->length] = 0; return true;
}
static bool header_text(struct job *j, struct header_buffer *b, const char *p) {
    return header_append(j, b, p, strlen(p));
}
static int remaining(struct job *j) {
    uint64_t now = n_clock_uptime_ms();
    if (now >= j->wire.deadline) { fail(j, "Request deadline exceeded"); return 0; }
    uint64_t left = j->wire.deadline - now;
    /* TCP/read timeout parameters are signed int. Never turn a long positive
       request allowance into a negative/infinite wait by narrowing it. */
    return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}
static bool token_char(unsigned char c) {
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
/* Only publish a replacement after complete allocation and buffer checks.
   Private slot may already own an origin; OOM never becomes opaque/null. */
static enum http_url_result http_url(const char *raw, char **canonical, char **origin) {
    /* Preserve this transport caller's stricter wire URL policy. Origin-only
       users may have non-ASCII paths or underscores: do not impose it there. */
    char *next = NULL, *next_origin = NULL;
    enum http_url_result result = http_canonical_owned_n(raw, raw ? strlen(raw) : 0, canonical ? &next : NULL, &next_origin);
    if (result != HTTP_URL_TUPLE) return result;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++)
        if (*p >= 127) { free(next); free(next_origin); return HTTP_URL_INVALID; }
    if (strchr(next_origin, '_')) { free(next); free(next_origin); return HTTP_URL_INVALID; }
    if(canonical){free(*canonical);*canonical=next;}
    free(*origin); *origin = next_origin;
    return HTTP_URL_TUPLE;
}
static bool url_failure(struct job *j, enum http_url_result result, const char *invalid) {
    return fail(j, result == HTTP_URL_OOM ? "Out of memory preparing canonical URL or origin" : invalid);
}
/* Strip credentials/fragment through the existing canonical URL boundary.
   Non-HTTP sources have no network referrer; malformed metadata fails closed. */
static bool prepare_referrer(struct job *j) {
    if(!j->referrer||!*j->referrer)return true;
    const char *raw=j->referrer,*scheme=strstr(raw,"://");
    if(!scheme){free(j->referrer);j->referrer=NULL;return true;}
    const char *authority=scheme+3,*end=authority+strcspn(authority,"/?#"),*at=NULL;
    for(const char *p=authority;p<end;p++)if(*p=='@')at=p;
    char *clean=NULL;
    if(at){size_t prefix=(size_t)(authority-raw),tail=strlen(at+1);if(prefix>SIZE_MAX-tail-1)return fail(j,"Referrer allocation overflow");
        clean=malloc(prefix+tail+1);if(!clean)return fail(j,"Out of memory preparing referrer");memcpy(clean,raw,prefix);memcpy(clean+prefix,at+1,tail+1);raw=clean;}
    char *next=NULL,*origin=NULL;
    enum http_url_result result=http_canonical_owned_n(raw,strlen(raw),&next,&origin);free(clean);
    if(result==HTTP_URL_OPAQUE){free(j->referrer);j->referrer=NULL;return true;}
    if(result!=HTTP_URL_TUPLE)return url_failure(j,result,"Invalid private referrer URL");
    size_t length=strlen(origin);char *origin_url=malloc(length+2);
    if(!origin_url){free(next);free(origin);return fail(j,"Out of memory preparing referrer origin");}
    memcpy(origin_url,origin,length);origin_url[length]='/';origin_url[length+1]=0;free(origin);
    if(strlen(next)>4096){free(next);next=strdup(origin_url);if(!next){free(origin_url);return fail(j,"Out of memory reducing referrer");}}
    free(j->referrer);j->referrer=next;j->referrer_origin=origin_url;return true;
}
static bool potentially_trustworthy(const char *url) {
    if(!strncmp(url,"https://",8))return true;
    if(strncmp(url,"http://",7))return false;
    const char *host=url+7;
    if(*host=='['){const char *end=strchr(host,']');return end&&end-host==4&&!memcmp(host,"[::1]",5);}
    size_t length=strcspn(host,":/");
    /* The native resolver guarantees exact localhost, not *.localhost.
       127/8 means an IPv4 literal, never a DNS name like 127.evil.test. */
    if(length==9&&!memcmp(host,"localhost",9))return true;
    size_t at=0;
    for(unsigned part=0;part<4;part++){
        unsigned value=0,digits=0;
        while(at<length&&host[at]>='0'&&host[at]<='9'){if(++digits>3)return false;value=value*10+(unsigned)(host[at++]-'0');}
        if(!digits||value>255||(part==0&&value!=127))return false;
        if(part==3)return at==length;
        if(at==length||host[at++]!='.')return false;
    }
    return false;
}
static bool append_referrer(struct job *j,const char *current) {
    if(!j->referrer||!*j->referrer)return true;
    bool same=j->referrer_origin&&strlen(j->referrer_origin)==strlen(j->target_origin)+1&&
        !memcmp(j->referrer_origin,j->target_origin,strlen(j->target_origin));
    bool downgrade=potentially_trustworthy(j->referrer)&&!potentially_trustworthy(current);
    int policy=j->referrer_policy?j->referrer_policy:WEBNET_REFERRER_STRICT_ORIGIN_CROSS;
    const char *value=j->referrer;
    if(j->wire.user_navigation&WEBNET_WIRE_NO_REFERRER)policy=WEBNET_REFERRER_NONE;
    switch(policy){
    case WEBNET_REFERRER_NONE:value="";break;
    case WEBNET_REFERRER_DOWNGRADE:if(downgrade)value="";break;
    case WEBNET_REFERRER_SAME_ORIGIN:if(!same)value="";break;
    case WEBNET_REFERRER_ORIGIN:value=j->referrer_origin;break;
    case WEBNET_REFERRER_STRICT_ORIGIN:value=downgrade?"":j->referrer_origin;break;
    case WEBNET_REFERRER_ORIGIN_CROSS:if(!same)value=j->referrer_origin;break;
    case WEBNET_REFERRER_STRICT_ORIGIN_CROSS:if(!same)value=downgrade?"":j->referrer_origin;break;
    case WEBNET_REFERRER_UNSAFE:break;
    default:return fail(j,"Invalid private referrer policy");
    }
    if(*value&&(!header_text(j,&j->outgoing,"Referer: ")||!header_text(j,&j->outgoing,value)||!header_text(j,&j->outgoing,"\r\n")))return false;
    /* A redirect must not resurrect a path/header reduced by an earlier hop. */
    if(value!=j->referrer){char *next=strdup(value);if(!next)return fail(j,"Out of memory updating referrer");free(j->referrer);j->referrer=next;}
    return true;
}
/* WHATWG host parsing uses the final nonempty label to detect possible IPv4:
   decimal, shortened/integer, hexadecimal and octal variants must not be
   treated as DNS domains just because this native stack lacks that parser.
   Conservatively leave them to the existing mixed-content rejection. */
static bool image_host_potential_ip(const char *host, size_t length) {
    if (length && host[length - 1] == '.') length--;
    size_t start = length;
    while (start && host[start - 1] != '.') start--;
    if (start == length) return true;
    bool decimal = true;
    for (size_t i = start; i < length; i++)
        if (host[i] < '0' || host[i] > '9') { decimal = false; break; }
    if (decimal) return true;
    if (length - start >= 2 && host[start] == '0' && (host[start + 1] == 'x' || host[start + 1] == 'X')) {
        for (size_t i = start + 2; i < length; i++)
            if (!((host[i] >= '0' && host[i] <= '9') ||
                  (host[i] >= 'a' && host[i] <= 'f') || (host[i] >= 'A' && host[i] <= 'F'))) return false;
        return true;
    }
    return false;
}
/* Prepare before publishing either canonical URL or origin. No retry path
   ever falls back to HTTP after a TLS error. Each redirect calls this too. */
static enum http_url_result image_http_url(struct job *j, const char *raw,
                                          char **canonical, char **origin) {
    char *next_origin = NULL,*prepared=NULL;
    enum http_url_result result = http_url(raw, &prepared, &next_origin);
    if (result != HTTP_URL_TUPLE) return result;
    char *upgraded = NULL;
    bool eligible = j->wire.kind == WEBNET_RESOURCE &&
        (j->wire.user_navigation & WEBNET_WIRE_IMAGE_UPGRADE) &&
        j->origin && !strncmp(j->origin, "https://", 8) && !strncmp(prepared, "http://", 7);
    if (eligible) {
        const char *host = prepared + 7;
        size_t hl = strcspn(host, ":/?#");
        if (!image_host_potential_ip(host, hl)) {
            /* http_url already removes default port80. Explicit nondefault
               ports remain in this spelling and survive the scheme change. */
            size_t length = strlen(prepared);
            if (length > SIZE_MAX - 2 || length >= UINT32_MAX) {
                free(prepared);free(next_origin); return HTTP_URL_INVALID;
            }
            upgraded = malloc(length + 2);
            if (!upgraded) { free(prepared);free(next_origin); return HTTP_URL_OOM; }
            memcpy(upgraded, "https://", 8); memcpy(upgraded + 8, prepared + 7, length - 7 + 1);
            result = http_url(upgraded, &prepared, &next_origin);
            free(upgraded);
            if (result != HTTP_URL_TUPLE) { free(prepared);free(next_origin); return result; }
        }
    }
    size_t length = strlen(prepared);
    if (!canonical || length > webnet_wire_url_limit(j->wire.user_navigation) || length == SIZE_MAX) { free(prepared);free(next_origin); return HTTP_URL_INVALID; }
    free(*canonical);*canonical=prepared;
    free(*origin); *origin = next_origin; return HTTP_URL_TUPLE;
}
/* The path a file: URL names (n_path_from_url), in *out: 1; 0 invalid, -1 out of memory. */
static int file_path(const char *url,char **out) {
    *out=NULL;size_t length=url?strlen(url):0;if(!url||length>UINT32_MAX)return 0;
    char *path=malloc(length+4);if(!path)return -1;
    n_result named=n_path_from_url(url,path,length+4);
    if(named<0){free(path);return named==-N_ERR_NO_MEMORY?-1:0;}
    *out=path;return 1;
}
static bool load_file_open(struct job *j,const char *path) {
    if(strcmp(j->method,"GET")||j->wire.body_len)return fail(j,"Local resources support GET only");
    n_handle doc=n_document_open(path,N_DOCUMENT_READ);if(doc<0)return fail(j,"Cannot open local resource");
    n_result size=n_document_size(doc);
    if(size<0||(uint64_t)size>webnet_wire_response_limit(j->wire.kind,j->wire.user_navigation)||(uint64_t)size+1>SIZE_MAX){n_handle_close(doc);return fail(j,"Invalid or oversized local resource");}
    j->body=malloc((size_t)size+1);if(!j->body){n_handle_close(doc);return fail(j,"Out of memory");}
    while(j->body_len<(size_t)size){
        if(!remaining(j)){n_handle_close(doc);return false;}
        n_result k=n_document_read_at(doc,j->body_len,j->body+j->body_len,MIN((size_t)size-j->body_len,65536));
        if(k<=0){n_handle_close(doc);return fail(j,"Local resource ended early");}
        j->body_len+=(size_t)k;
    }
    n_handle_close(doc);j->body[j->body_len]=0;
    char *url=strdup(j->url);if(!url)return fail(j,"Out of memory copying local URL");
    free(j->final_url);j->final_url=url;j->status=200;return true;
}
static bool load_file(struct job *j) {
    if(j->wire.kind==WEBNET_FETCH||j->wire.kind==WEBNET_REPORT)return fail(j,"fetch/report accepts only HTTP(S)");
    char *path=NULL,*document_path=NULL;int parsed=file_path(j->url,&path);bool ok=false;
    if(parsed!=1)return fail(j,parsed<0?"Out of memory preparing local URL":"Invalid local URL");
    if(j->wire.kind==WEBNET_NAVIGATION){
        if(!top_level_navigation(j)){fail(j,"Only a user navigation may open a local file");goto out;}
    }else{
        parsed=file_path(j->document,&document_path);
        if(parsed!=1){fail(j,parsed<0?"Out of memory preparing local document URL":"Remote pages cannot open local files");goto out;}
        size_t dl=(size_t)(n_path_name(document_path)-document_path); /* the document's folder, with its '\' */
        if(strncasecmp(path,document_path,dl)){fail(j,"Local resource escapes document directory");goto out;}
    }
    ok=load_file_open(j,path);
out:
    free(path);free(document_path);return ok;
}
static bool no_cors(const struct job *j) { return (j->wire.user_navigation & WEBNET_WIRE_NO_CORS)!=0; }
static bool cors_kind(const struct job *j) { return (j->wire.kind == WEBNET_FETCH && !no_cors(j)) || j->wire.kind == WEBNET_MODULE; }
/* Redirect-taint changes the serialized Origin, not the document origin used
   for same-origin mode and mixed-content checks. It can never be cleared. */
static const char *request_origin(const struct job *j) { return j->origin_tainted || !j->origin ? "null" : j->origin; }
static bool forbidden_header(const char *name) {
    static const char *const forbidden[] = {"host", "origin", "referer", "cookie", "cookie2",
        "proxy-authorization", "connection", "content-length", "transfer-encoding", "accept-encoding", "te", "trailer",
        "upgrade", "expect", "user-agent", "via", "date", "dnt", "permissions-policy", "access-control-request-method",
        "access-control-request-headers", "accept-charset", "keep-alive", NULL};
    if (!strncasecmp(name, "sec-", 4) || !strncasecmp(name, "proxy-", 6)) return true;
    for (int i = 0; forbidden[i]; i++) if (!strcasecmp(name, forbidden[i])) return true;
    return false;
}
static bool unsafe_value(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 32 && c != 9) || c == 127 || strchr("\"():<>?@[\\]{}", c)) return true;
    }
    return false;
}
/* Fetch "parse a single range header value" without whitespace; a CORS-
   safelisted range must name its first byte ("bytes=N-" or "bytes=N-M"). */
static bool safelisted_range(const char *value) {
    if (strncasecmp(value, "bytes=", 6)) return false;
    const char *s = value + 6;
    if (!isdigit((unsigned char)*s)) return false;
    unsigned long long start = 0, end = 0;
    for (; isdigit((unsigned char)*s); s++) { if (start > (~0ULL - 9) / 10) return false; start = start * 10 + (unsigned)(*s - '0'); }
    if (*s++ != '-') return false;
    if (!*s) return true;
    for (; isdigit((unsigned char)*s); s++) { if (end > (~0ULL - 9) / 10) return false; end = end * 10 + (unsigned)(*s - '0'); }
    return !*s && start <= end;
}
static bool safelisted(const char *name, const char *value) {
    if (strlen(value) > 128) return false;
    if (!strcasecmp(name, "range")) return safelisted_range(value);
    if (!strcasecmp(name, "accept")) return !unsafe_value(value);
    if (!strcasecmp(name, "accept-language") || !strcasecmp(name, "content-language")) {
        for (const char *s = value; *s; s++) if (!isalnum((unsigned char)*s) && !strchr(" *,-.;=", *s)) return false;
        return true;
    }
    if (!strcasecmp(name, "content-type")) {
        if (unsafe_value(value)) return false;
        size_t l = strcspn(value, ";");
        while (l && value[l - 1] == ' ') l--;
        return (l == 33 && !strncasecmp(value, "application/x-www-form-urlencoded", l)) ||
               (l == 19 && !strncasecmp(value, "multipart/form-data", l)) ||
               (l == 10 && !strncasecmp(value, "text/plain", l));
    }
    return false;
}
static bool list_has_slice(const char *list, const char *limit, const char *token, size_t tl, bool insensitive) {
    for (const char *p = list; p < limit;) {
        while (p < limit && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        const char *e = memchr(p, ',', (size_t)(limit - p)); if (!e) e = limit;
        const char *trim = e; while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        if ((size_t)(trim - p) == tl && (insensitive ? !strncasecmp(p, token, tl) : !strncmp(p, token, tl))) return true;
        p = e < limit ? e + 1 : e;
    }
    return false;
}
static bool list_has(const char *list, const char *token, bool insensitive) {
    return list_has_slice(list, list + strlen(list), token, strlen(token), insensitive);
}
static bool field_name_is(const char *name, size_t len, const char *expected) {
    return len == strlen(expected) && !strncasecmp(name, expected, len);
}
static bool field_exists(const char *headers, const char *name) {
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && field_name_is(p, (size_t)(colon - p), name)) return true;
        p = *end ? end + 2 : end;
    }
    return false;
}
/* Iterate every field occurrence in wire order. Values retain their complete
   bytes after the colon (including OWS); a duplicate is never an absent field.
   Singleton security lookups below deliberately reject multiple occurrences,
   while cache identity uses the complete ordered sequence. */
static bool field_value_next(const char **cursor,const char *name,size_t name_length,
                             const char **value,size_t *length) {
    const char *p=*cursor;
    while(*p){
        const char *end=strstr(p,"\r\n");if(!end)end=p+strlen(p);
        const char *next=*end?end+2:end;
        if((size_t)(end-p)>name_length&&p[name_length]==':'&&!strncasecmp(p,name,name_length)){
            *cursor=next;*value=p+name_length+1;*length=(size_t)(end-*value);return true;
        }
        p=next;
    }
    *cursor=p;*value=NULL;*length=0;return false;
}
static bool field_values_equal(const char *left,const char *right,const char *name,size_t name_length) {
    for(;;){
        const char *a,*b;size_t al,bl;
        bool ha=field_value_next(&left,name,name_length,&a,&al);
        bool hb=field_value_next(&right,name,name_length,&b,&bl);
        if(ha!=hb)return false;
        if(!ha)return true;
        if(al!=bl||memcmp(a,b,al))return false;
    }
}
/* Strict response field lookup: security-critical singleton fields cannot be duplicated. */
static bool field_slice(const char *headers, const char *name, const char **out, size_t *length) {
    size_t nl = strlen(name); bool found = false;
    *out = NULL; *length = 0;
    const char *v;size_t n;
    while(field_value_next(&headers,name,nl,&v,&n)){
        if(found){*out=NULL;*length=0;return false;}
        const char *end=v+n;
        while(v<end&&(*v==' '||*v=='\t'))v++;
        while(end>v&&(end[-1]==' '||end[-1]=='\t'))end--;
        *out=v;*length=(size_t)(end-v);found=true;
    }
    return found;
}
static bool field(const char *headers, const char *name, char *out, size_t cap) {
    const char *value; size_t length;
    if (!cap) return false;
    out[0] = 0;
    if (!field_slice(headers, name, &value, &length) || length >= cap) return false;
    memcpy(out, value, length); out[length] = 0; return true;
}
/* Private HTTP cache shared by the transport's request threads. Entries become
   visible only after a complete, successful representation. The 16 MiB quota
   limits retained cache memory, never the response accepted by Fetch. Requests
   own snapshots: eviction/revalidation cannot invalidate a paused stream. */
#define FETCH_CACHE_BYTES (16u * 1024u * 1024u)
#define FETCH_CACHE_OBJECT (2u * 1024u * 1024u)
struct cache_entry {
    struct cache_entry *next;
    char *url, *partition, *request_headers, *headers, *reason, *body;
    size_t body_len, charge;
    int status;
    uint64_t stored_ms, age_seconds, lifetime_seconds;
    bool no_cache, must_revalidate;
    /* Fetch stores a response's body info with it: its encoded size and
       content encoding as they were transmitted. */
    uint64_t encoded_size;
    char content_encoding[HTTP_CONTENT_ENCODING_MAX];
};
static struct cache_entry *cache_entries;
static size_t cache_bytes;
static volatile uint32_t cache_lock_word;
static void cache_lock(void) {
    while (__atomic_exchange_n(&cache_lock_word, 1, __ATOMIC_ACQUIRE))
        wait_on_address(&cache_lock_word, 1, UINT_MAX);
}
static void cache_unlock(void) {
    __atomic_store_n(&cache_lock_word, 0, __ATOMIC_RELEASE);
    wake_address(&cache_lock_word, 1);
}
static void cache_free(struct cache_entry *e) {
    if (!e) return;
    free(e->url);free(e->partition);free(e->request_headers);free(e->headers);
    free(e->reason);free(e->body);free(e);
}
static struct cache_entry *cache_clone(const struct cache_entry *s) {
    struct cache_entry *e=calloc(1,sizeof *e);if(!e)return NULL;
    *e=*s;e->next=NULL;
    e->url=strdup(s->url);e->partition=strdup(s->partition);e->request_headers=strdup(s->request_headers);
    e->headers=strdup(s->headers);e->reason=strdup(s->reason);e->body=malloc(s->body_len+1);
    if(!e->url||!e->partition||!e->request_headers||!e->headers||!e->reason||!e->body){cache_free(e);return NULL;}
    memcpy(e->body,s->body,s->body_len);e->body[s->body_len]=0;return e;
}
/* Repeated Cache-Control fields are equivalent to a comma-combined list.
   Quoted values are supported; malformed/duplicate numeric directives fail
   conservatively rather than giving an attacker an extended freshness. */
static bool cache_directive(const char *headers,const char *name,uint64_t *number) {
    bool found=false;uint64_t result=UINT64_MAX;
    for(const char *p=headers;*p;){
        const char *end=strstr(p,"\r\n");if(!end)end=p+strlen(p);
        const char *colon=memchr(p,':',(size_t)(end-p));
        if(colon&&field_name_is(p,(size_t)(colon-p),"Cache-Control")){
            const char *v=colon+1;
            while(v<end){
                while(v<end&&(*v==' '||*v=='\t'||*v==','))v++;
                const char *key=v;while(v<end&&token_char((unsigned char)*v))v++;
                size_t length=(size_t)(v-key);while(v<end&&(*v==' '||*v=='\t'))v++;
                bool match=field_name_is(key,length,name);uint64_t value=0;bool valid=true;
                if(v<end&&*v=='='){
                    v++;while(v<end&&(*v==' '||*v=='\t'))v++;
                    bool quoted=v<end&&*v=='"';if(quoted)v++;
                    const char *digits=v;
                    while(v<end&&*v>='0'&&*v<='9'){
                        unsigned d=(unsigned)(*v++-'0');
                        if(value>(UINT64_MAX-d)/10)valid=false;else value=value*10+d;
                    }
                    if(v==digits)valid=false;
                    if(quoted){if(v==end||*v!='"')valid=false;else v++;}
                    while(v<end&&(*v==' '||*v=='\t'))v++;
                    if(v<end&&*v!=',')valid=false;
                }else if(number){if(!strcasecmp(name,"max-stale"))value=UINT64_MAX;else valid=false;}
                if(match){if(found&&number)value=0;found=true;result=valid?value:0;}
                bool quote=false;
                while(v<end){if(*v=='"')quote=!quote;if(*v++==','&&!quote)break;}
            }
        }
        p=*end?end+2:end;
    }
    if(found&&number)*number=result;return found;
}
static bool cache_decimal(const char *p,size_t n,uint64_t *out) {
    if(!n)return false;uint64_t value=0;
    for(size_t i=0;i<n;i++){unsigned d=(unsigned char)p[i]-'0';if(d>9||value>(UINT64_MAX-d)/10)return false;value=value*10+d;}
    *out=value;return true;
}
static bool cache_date(const char *headers,const char *name,int64_t *out) {
    const char *p;size_t n;if(!field_slice(headers,name,&p,&n)||n!=29)return false;
    /* IMF-fixdate is the generated HTTP date format. Reject impossible dates;
       obsolete server date formats conservatively yield stale entries. */
    if(p[3]!=','||p[4]!=' '||p[7]!=' '||p[11]!=' '||p[16]!=' '||p[19]!=':'||p[22]!=':'||memcmp(p+25," GMT",4))return false;
    uint64_t day,year,hour,min,sec;
    if(!cache_decimal(p+5,2,&day)||!cache_decimal(p+12,4,&year)||!cache_decimal(p+17,2,&hour)||
       !cache_decimal(p+20,2,&min)||!cache_decimal(p+23,2,&sec)||year<1601||hour>23||min>59||sec>59)return false;
    const char *months="JanFebMarAprMayJunJulAugSepOctNovDec";unsigned month=0;
    while(month<12&&memcmp(months+month*3,p+8,3))month++;
    if(month==12||!day)return false;
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=year%4==0&&(year%100!=0||year%400==0);
    if(day>days[month]+(month==1&&leap))return false;
    int64_t y=(int64_t)year-(month<2);unsigned m=month+1;
    int64_t era=y/400;unsigned yoe=(unsigned)(y-era*400);
    unsigned doy=(153*(m+(m>2?-3:9))+2)/5+(unsigned)day-1;
    int64_t total=era*146097+(int64_t)(yoe*365+yoe/4-yoe/100+doy)-719468;
    *out=total*86400+(int64_t)(hour*3600+min*60+sec);return true;
}
static void cache_freshness(struct cache_entry *e) {
    uint64_t maximum=0,age=0;int64_t date=0,expires=0,modified=0,now=time(NULL);
    bool dated=cache_date(e->headers,"Date",&date);
    const char *a;size_t n;
    if(field_exists(e->headers,"Age")&&(!field_slice(e->headers,"Age",&a,&n)||!cache_decimal(a,n,&age)))age=UINT64_MAX;
    if(dated&&now>date&&(uint64_t)(now-date)>age)age=(uint64_t)(now-date);
    e->age_seconds=age;e->stored_ms=n_clock_uptime_ms();e->lifetime_seconds=0;
    if(cache_directive(e->headers,"max-age",&maximum))e->lifetime_seconds=maximum;
    else if(cache_date(e->headers,"Expires",&expires)){
        int64_t base=dated?date:now;if(expires>base)e->lifetime_seconds=(uint64_t)(expires-base);
    }else if(cache_date(e->headers,"Last-Modified",&modified)){
        int64_t base=dated?date:now;
        if(base>modified)e->lifetime_seconds=MIN((uint64_t)(base-modified)/10,86400u);
    }
    e->no_cache=cache_directive(e->headers,"no-cache",NULL);
    e->must_revalidate=cache_directive(e->headers,"must-revalidate",NULL);
}
static uint64_t cache_age(const struct cache_entry *e) {
    uint64_t elapsed=(n_clock_uptime_ms()-e->stored_ms)/1000;
    return elapsed>UINT64_MAX-e->age_seconds?UINT64_MAX:e->age_seconds+elapsed;
}
static bool cache_vary(const struct cache_entry *e,const char *request) {
    for(const char *p=e->headers;*p;){
        const char *end=strstr(p,"\r\n");if(!end)end=p+strlen(p);
        const char *colon=memchr(p,':',(size_t)(end-p));
        if(colon&&field_name_is(p,(size_t)(colon-p),"Vary")){
            const char *v=colon+1;
            while(v<end){
                while(v<end&&(*v==' '||*v=='\t'||*v==','))v++;
                const char *name=v;while(v<end&&token_char((unsigned char)*v))v++;
                size_t length=(size_t)(v-name);if(!length||(length==1&&*name=='*'))return false;
                if(!field_values_equal(e->request_headers,request,name,length))return false;
                while(v<end&&(*v==' '||*v=='\t'))v++;
                if(v<end&&*v!=',')return false;
            }
        }
        p=*end?end+2:end;
    }
    return true;
}
static struct cache_entry *cache_find(struct job *j) {
    struct cache_entry *snapshot=NULL;cache_lock();
    struct cache_entry **link=&cache_entries;
    while(*link){struct cache_entry *e=*link;
        if(!strcmp(e->url,j->final_url)&&!strcmp(e->partition,j->partition)&&cache_vary(e,j->outgoing.p)){
            snapshot=cache_clone(e);*link=e->next;e->next=cache_entries;cache_entries=e;break;
        }link=&e->next;
    }
    cache_unlock();return snapshot;
}
static void cache_insert(struct cache_entry *e) {
    e->charge=sizeof *e+e->body_len+1+strlen(e->url)+1+strlen(e->partition)+1+
        strlen(e->request_headers)+1+strlen(e->headers)+1+strlen(e->reason)+1;
    if(e->charge>FETCH_CACHE_BYTES){cache_free(e);return;}
    cache_lock();struct cache_entry **link=&cache_entries;
    while(*link){struct cache_entry *old=*link;
        if(!strcmp(old->url,e->url)&&!strcmp(old->partition,e->partition)&&cache_vary(old,e->request_headers)){
            *link=old->next;cache_bytes-=old->charge;cache_free(old);
        }else link=&old->next;
    }
    while(cache_entries&&cache_bytes>FETCH_CACHE_BYTES-e->charge){
        link=&cache_entries;while((*link)->next)link=&(*link)->next;
        struct cache_entry *old=*link;*link=NULL;cache_bytes-=old->charge;cache_free(old);
    }
    e->next=cache_entries;cache_entries=e;cache_bytes+=e->charge;cache_unlock();
}
static unsigned cache_mode(const struct job *j) {
    unsigned mode=WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation);
    if(mode==WEBNET_CACHE_DEFAULT&&(field_exists(j->headers,"If-Modified-Since")||field_exists(j->headers,"If-None-Match")||
       field_exists(j->headers,"If-Unmodified-Since")||field_exists(j->headers,"If-Match")||field_exists(j->headers,"If-Range")))return WEBNET_CACHE_NO_STORE;
    return mode;
}
static bool prepare_top_level(struct job *j) {
    if(j->top_prepared)return true;
    const char *raw=j->top_level_url?j->top_level_url:j->document?j->document:"";
    char *origin=NULL;
    enum http_url_result result=*raw?http_origin_owned(raw,&origin):HTTP_URL_OPAQUE;
    if(result!=HTTP_URL_TUPLE&&result!=HTTP_URL_OPAQUE){
        free(origin);return url_failure(j,result,"Invalid trusted top document URL");
    }
    free(j->top_origin);j->top_origin=origin;j->top_opaque=result!=HTTP_URL_TUPLE;j->top_prepared=true;return true;
}
static const char *cookie_site_url(const struct job *j) {
    if(j->top_level_url)return j->top_level_url;
    return j->document&&*j->document?j->document:top_level_navigation(j)?NULL:"";
}
static bool cache_partition(struct job *j) {
    /* Separate enclosing top and initiating frame origins, then retain exact
       credentials. Every HTTP pool/TLS entry receives this same private key. */
    if(!prepare_top_level(j))return false;
    struct header_buffer b={0};const char *origin=j->origin?j->origin:j->target_origin;
    const char *top=top_level_navigation(j)?j->target_origin:j->top_origin?j->top_origin:"";
    char prefix[96];
    /* Origins are unbounded; only the numeric prefix uses a fixed buffer. */
    snprintf(prefix,sizeof prefix,"T%zu:",strlen(top));
    bool ok=header_text(j,&b,prefix)&&header_text(j,&b,top);
    snprintf(prefix,sizeof prefix,"%zu:",strlen(origin));
    ok=ok&&header_text(j,&b,prefix)&&header_text(j,&b,origin);
    snprintf(prefix,sizeof prefix,"/%u/",j->wire.credentials);ok=ok&&header_text(j,&b,prefix);
    static const char *names[]={"Cookie","Authorization"};
    for(unsigned i=0;ok&&i<2;i++){
        const char *cursor=j->outgoing.p,*value;size_t n;
        while(ok&&field_value_next(&cursor,names[i],strlen(names[i]),&value,&n)){
            snprintf(prefix,sizeof prefix,"%zu:",n);
            ok=header_text(j,&b,prefix)&&header_append(j,&b,value,n);
        }
        /* Length-delimited values followed by an explicit field terminator
           distinguish missing, empty, repeated, and differently ordered fields. */
        ok=ok&&header_text(j,&b,";");
    }
    if((!j->origin||j->top_opaque)&&!top_level_navigation(j)){
        snprintf(prefix,sizeof prefix,"/opaque/%llu/%llu",(unsigned long long)j->wire.id,(unsigned long long)j->wire.generation);
        ok=ok&&header_text(j,&b,prefix);
    }
    if(!ok){free(b.p);return false;}free(j->partition);j->partition=b.p;return true;
}
static bool cache_usable(struct job *j,const struct cache_entry *e) {
    unsigned mode=cache_mode(j);uint64_t age=cache_age(e),max=0,min=0;
    if(e->no_cache||mode==WEBNET_CACHE_NO_CACHE||cache_directive(j->headers,"no-cache",NULL))return false;
    const char *pragma;size_t pragma_length;
    if(!field_exists(j->headers,"Cache-Control")&&field_slice(j->headers,"Pragma",&pragma,&pragma_length)&&
       list_has_slice(pragma,pragma+pragma_length,"no-cache",8,true))return false;
    if(mode==WEBNET_CACHE_FORCE_CACHE||mode==WEBNET_CACHE_ONLY_IF_CACHED)
        return !e->must_revalidate||age<e->lifetime_seconds;
    uint64_t lifetime=e->lifetime_seconds;
    if(cache_directive(j->headers,"max-age",&max)&&max<lifetime)lifetime=max;
    cache_directive(j->headers,"min-fresh",&min);
    if(age<lifetime&&min<lifetime-age)return true;
    if(!e->must_revalidate&&cache_directive(j->headers,"max-stale",NULL)){
        uint64_t stale=UINT64_MAX;
        cache_directive(j->headers,"max-stale",&stale);
        return age>=lifetime&&age-lifetime<=stale;
    }
    return false;
}
static void cache_invalidate(const char *url) {
    cache_lock();struct cache_entry **link=&cache_entries;
    while(*link){struct cache_entry *e=*link;if(!strcmp(e->url,url)){*link=e->next;cache_bytes-=e->charge;cache_free(e);}else link=&e->next;}
    cache_unlock();
}
static bool cache_validator_matches(const struct cache_entry *e,const struct http_resp *r) {
    const char *old,*next;size_t old_length,next_length;
    if(!field_exists(http_response_headers(r),"ETag"))return true;
    if(!field_slice(http_response_headers(r),"ETag",&next,&next_length))return false;
    if(!field_slice(e->headers,"ETag",&old,&old_length))return true;
    if(old_length>2&&!memcmp(old,"W/",2)){old+=2;old_length-=2;}
    if(next_length>2&&!memcmp(next,"W/",2)){next+=2;next_length-=2;}
    return old_length==next_length&&!memcmp(old,next,old_length);
}
static char *cache_clean_headers(const char *headers,const char *updates,bool replay,uint64_t age) {
    struct job scratch={0};struct header_buffer b={0};
    for(const char *p=headers;*p;){
        const char *end=strstr(p,"\r\n");if(!end)end=p+strlen(p);
        const char *colon=memchr(p,':',(size_t)(end-p));bool keep=colon!=NULL;
        if(colon){size_t n=(size_t)(colon-p);
            if(field_name_is(p,n,"Set-Cookie")||field_name_is(p,n,"Set-Cookie2")||
               ((replay||updates)&&field_name_is(p,n,"Age")))keep=false;
            if(updates){char *name=malloc(n+1);if(!name){free(b.p);return NULL;}
                memcpy(name,p,n);name[n]=0;if(field_exists(updates,name)&&!field_name_is(p,n,"Content-Length")&&
                    !field_name_is(p,n,"Content-Encoding")&&!field_name_is(p,n,"Content-Range"))keep=false;
                free(name);}
        }
        if(keep&&(!header_append(&scratch,&b,p,(size_t)(end-p))||!header_text(&scratch,&b,"\r\n"))){free(b.p);return NULL;}
        p=*end?end+2:end;
    }
    if(updates)for(const char *p=updates;*p;){
        const char *end=strstr(p,"\r\n");if(!end)end=p+strlen(p);const char *colon=memchr(p,':',(size_t)(end-p));
        size_t n=colon?(size_t)(colon-p):0;
        if(colon&&!field_name_is(p,n,"Set-Cookie")&&!field_name_is(p,n,"Set-Cookie2")&&!field_name_is(p,n,"Content-Length")&&
           !field_name_is(p,n,"Content-Encoding")&&!field_name_is(p,n,"Content-Range")&&!field_name_is(p,n,"Connection")&&
           !field_name_is(p,n,"Transfer-Encoding")&&(!replay||!field_name_is(p,n,"Age"))&&
           (!header_append(&scratch,&b,p,(size_t)(end-p))||!header_text(&scratch,&b,"\r\n"))){free(b.p);return NULL;}
        p=*end?end+2:end;
    }
    if(replay){char value[64];snprintf(value,sizeof value,"Age: %llu\r\n",(unsigned long long)age);
        if(!header_text(&scratch,&b,value)){free(b.p);return NULL;}}
    if(!b.p)b.p=calloc(1,1);return b.p;
}
static void cache_store(struct job *j,const struct http_resp *r) {
    if(!j->cache_capture||j->error[0]||!remaining(j))return;
    size_t metadata=sizeof(struct cache_entry)+j->cache_body.length+1;
    const char *pieces[]={j->final_url,j->partition,j->outgoing.p,http_response_headers(r),http_response_status_text(r)};
    for(unsigned i=0;i<sizeof pieces/sizeof pieces[0];i++){
        size_t length=strlen(pieces[i]);
        if(metadata>=FETCH_CACHE_BYTES||length>=FETCH_CACHE_BYTES-metadata)return;
        metadata+=length+1;
    }
    struct cache_entry source={.url=j->final_url,.partition=j->partition,.request_headers=j->outgoing.p,
        .headers=(char *)http_response_headers(r),.reason=(char *)http_response_status_text(r),
        .body=j->cache_body.p?j->cache_body.p:"",.body_len=j->cache_body.length,.status=r->status,
        .encoded_size=j->timing.encoded_body_size};
    memcpy(source.content_encoding,j->timing.content_encoding,sizeof source.content_encoding);
    struct cache_entry *e=cache_clone(&source);if(!e)return;
    char *clean=cache_clean_headers(e->headers,NULL,false,0);if(!clean){cache_free(e);return;}
    free(e->headers);e->headers=clean;cache_freshness(e);
    if(!cache_vary(e,e->request_headers)){cache_free(e);return;}cache_insert(e);
}
static bool validate_headers(struct job *j) {
    struct header_buffer seen = {0}, names = {0};
    char *name = NULL; bool ok = false;
    if (!header_reserve(j, &seen, 0) || !header_reserve(j, &names, 0)) goto out;
    const char *p = j->headers;
    while (*p) {
        const char *end = strstr(p, "\r\n");
        if (!end) { fail(j, "Request headers must end in CRLF"); goto out; }
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || colon == p) { fail(j, "Invalid request header name"); goto out; }
        size_t nl = colon - p;
        name = malloc(nl + 1);
        if (!name) { fail(j, "Out of memory validating request header name"); goto out; }
        for (size_t i = 0; i < nl; i++) {
            if (!token_char((unsigned char)p[i])) { fail(j, "Invalid request header name"); goto out; }
            name[i] = (char)tolower((unsigned char)p[i]);
        }
        name[nl] = 0;
        if (forbidden_header(name)) { fail(j, "Prohibited request header"); goto out; }
        if (list_has(seen.p, name, true)) { fail(j, "Duplicate request header"); goto out; }
        if ((seen.length && !header_text(j, &seen, ",")) || !header_append(j, &seen, name, nl)) goto out;
        const char *v = colon + 1; while (v < end && (*v == ' ' || *v == '\t')) v++;
        size_t vl = end - v;
        for (size_t i = 0; i < vl; i++) if (((unsigned char)v[i] < 32 && v[i] != '\t') || (unsigned char)v[i] == 127) { fail(j, "Invalid request header value"); goto out; }
        /* Fetch's 128-byte safelist condition is a protocol rule, not a
           request capacity: longer values remain valid and need preflight. */
        bool safe = false;
        if (vl <= 128) { char value[129]; memcpy(value, v, vl); value[vl] = 0; safe = safelisted(name, value); }
        /* Range is CORS-safelisted but not a no-CORS-safelisted name. */
        if (safe && no_cors(j) && !strcmp(name, "range")) { fail(j, "No-cors request requires safelisted headers"); goto out; }
        if (!safe) {
            if(no_cors(j)){fail(j,"No-cors request requires safelisted headers");goto out;}
            if ((names.length && !header_text(j, &names, ", ")) || !header_append(j, &names, name, nl)) goto out;
        }
        free(name); name = NULL;
        p = end + 2;
    }
    free(j->non_simple); j->non_simple = names.p; names.p = NULL; ok = true;
out:
    free(name); free(seen.p); free(names.p); return ok;
}
/* Fetch: a navigation request's destination is document, embed, frame, iframe or object. */
static bool navigation_request(const struct job *j) {
    switch (j->fetch.destination) {
    case WEBNET_DEST_DOCUMENT: case WEBNET_DEST_EMBED: case WEBNET_DEST_FRAME:
    case WEBNET_DEST_IFRAME: case WEBNET_DEST_OBJECT: return true;
    default: return false;
    }
}
/* Fetch's fetch step: the Accept value for a request without one, by destination. */
static const char *accept_value(const struct job *j) {
    switch (j->fetch.destination) {
    case WEBNET_DEST_DOCUMENT: case WEBNET_DEST_FRAME: case WEBNET_DEST_IFRAME:
        return "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
    case WEBNET_DEST_IMAGE: return "image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5";
    case WEBNET_DEST_JSON: return "application/json,*/*;q=0.5";
    case WEBNET_DEST_STYLE: return "text/css,*/*;q=0.1";
    case WEBNET_DEST_TEXT: return "text/plain,*/*;q=0.5";
    default: return "*/*";
    }
}
/* Fetch "fetch", step 14: the Accept-Language the user agent appends. The
   browser chose the value when the fetch began (the user's languages, the
   list navigator.languages reports) and it came with the request, so every
   hop sends the same one; the service knows nothing of the user's choice. */
static bool append_accept_language(struct job *j) {
    if (!j->accept_language[0]) return true;
    return header_text(j, &j->outgoing, "Accept-Language: ") && header_text(j, &j->outgoing, j->accept_language) &&
        header_text(j, &j->outgoing, "\r\n");
}
/* Sec-Fetch-Site: how the request's origin relates to every URL of the
   request's URL list so far (Fetch Metadata 2.3). Called once per URL, the
   first and each redirect target, before that hop's request is sent. */
static void note_fetch_site(struct job *j, const char *url, const char *url_origin) {
    if (j->fetch.flags & WEBNET_FETCH_BROWSER_UI) return;
    unsigned relation = j->origin && url_origin && !strcmp(j->origin, url_origin) ? 0u :
        j->origin && webcookie_same_site(j->cookies, j->origin, url) ? 1u : 2u;
    if (relation > j->fetch_site) j->fetch_site = relation;
}
/* Fetch Metadata's headers, for a request to a potentially trustworthy URL. */
static bool append_fetch_metadata(struct job *j, const char *current) {
    if (!potentially_trustworthy(current)) return true;
    const char *destination = webnet_destination_name((enum webnet_destination)j->fetch.destination);
    const char *mode = navigation_request(j) || j->wire.kind == WEBNET_NAVIGATION ? "navigate" :
        j->wire.kind == WEBNET_WEBSOCKET ? "websocket" :
        (j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN) ? "same-origin" : cors_kind(j) ? "cors" : "no-cors";
    static const char *const sites[] = {"same-origin", "same-site", "cross-site"};
    const char *site = (j->fetch.flags & WEBNET_FETCH_BROWSER_UI) ? "none" : sites[j->fetch_site];
    return header_text(j, &j->outgoing, "Sec-Fetch-Dest: ") && header_text(j, &j->outgoing, *destination ? destination : "empty") &&
        header_text(j, &j->outgoing, "\r\nSec-Fetch-Mode: ") && header_text(j, &j->outgoing, mode) &&
        header_text(j, &j->outgoing, "\r\nSec-Fetch-Site: ") && header_text(j, &j->outgoing, site) &&
        header_text(j, &j->outgoing, "\r\n") &&
        (!navigation_request(j) || !(j->fetch.flags & WEBNET_FETCH_USER_ACTIVATION) ||
         header_text(j, &j->outgoing, "Sec-Fetch-User: ?1\r\n"));
}
/* Browser-generated fields remain separate from author classification. Cache
   lookup and conditional validation below own their network/storage effects;
   these fields never contaminate the CORS preflight author-header list. */
static bool cache_headers(struct job *j) {
    unsigned mode = WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation);
    if (mode == WEBNET_CACHE_DEFAULT &&
        (field_exists(j->headers, "If-Modified-Since") || field_exists(j->headers, "If-None-Match") ||
         field_exists(j->headers, "If-Unmodified-Since") || field_exists(j->headers, "If-Match") ||
         field_exists(j->headers, "If-Range"))) mode = WEBNET_CACHE_NO_STORE;
    const char *pragma = "", *control = "";
    if (mode == WEBNET_CACHE_NO_STORE || mode == WEBNET_CACHE_RELOAD) {
        if (!field_exists(j->headers, "Pragma")) pragma = "Pragma: no-cache\r\n";
        if (!field_exists(j->headers, "Cache-Control")) control = "Cache-Control: no-cache\r\n";
    } else if (mode == WEBNET_CACHE_NO_CACHE && !field_exists(j->headers, "Cache-Control"))
        control = "Cache-Control: max-age=0\r\n";
    return header_text(j, &j->outgoing, pragma) && header_text(j, &j->outgoing, control);
}
static bool prepare_outgoing(struct job *j, const char *current, const char *method, bool cross) {
    j->outgoing.length = 0;
    if (!header_text(j, &j->outgoing, j->headers)) return false;
    /* Fetch's fetch step adds Accept and Accept-Language the author did not
       set, and Upgrade Insecure Requests marks every navigation request (no
       host here is a preloadable HSTS host). WebSocket handshakes have their
       own fields. */
    if (j->wire.kind != WEBNET_WEBSOCKET) {
        if (!field_exists(j->headers, "Accept") && !(header_text(j, &j->outgoing, "Accept: ") &&
            header_text(j, &j->outgoing, accept_value(j)) && header_text(j, &j->outgoing, "\r\n"))) return false;
        if (!field_exists(j->headers, "Accept-Language") && !append_accept_language(j)) return false;
        if (navigation_request(j) && !header_text(j, &j->outgoing, "Upgrade-Insecure-Requests: 1\r\n")) return false;
    }
    /* Fetch "append a request Origin header": always for a CORS request, and
       for any other request (no-cors, a navigation, a form POST) whose
       method is neither GET nor HEAD, nulled by its referrer policy. */
    const char *serialized_origin=request_origin(j);
    if(!cors_kind(j)&&j->wire.kind!=WEBNET_REPORT){
        int policy=j->referrer_policy?j->referrer_policy:WEBNET_REFERRER_STRICT_ORIGIN_CROSS;
        bool downgrade=j->origin&&potentially_trustworthy(j->origin)&&!potentially_trustworthy(current);
        if((j->wire.user_navigation&WEBNET_WIRE_NO_REFERRER)||policy==WEBNET_REFERRER_NONE ||
           (policy==WEBNET_REFERRER_SAME_ORIGIN&&cross) ||
           ((policy==WEBNET_REFERRER_DOWNGRADE||policy==WEBNET_REFERRER_STRICT_ORIGIN||policy==WEBNET_REFERRER_STRICT_ORIGIN_CROSS)&&downgrade))serialized_origin="null";
    }
    if ((cors_kind(j) || j->wire.kind==WEBNET_REPORT || (strcmp(method,"GET") && strcmp(method,"HEAD"))) && !(header_text(j, &j->outgoing, "Origin: ") &&
        header_text(j, &j->outgoing,serialized_origin) && header_text(j, &j->outgoing, "\r\n"))) return false;
    if (!cache_headers(j)) return false;
    if(!append_referrer(j,current))return false;
    if (!append_fetch_metadata(j, current)) return false;
    /* Fetch's network layer owns content-coding negotiation, after author
       headers have been classified for CORS. Advertise only the codings
       http_coding.h decodes; byte ranges must keep their representation
       offsets (Fetch step 19). */
    if (!field_exists(j->headers,"Accept-Encoding") &&
        !header_text(j,&j->outgoing,field_exists(j->headers,"Range") ?
            "Accept-Encoding: identity\r\n" : "Accept-Encoding: gzip, deflate, br\r\n")) return false;
    j->hop_cookies = j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE ||
        (j->wire.credentials == WEBNET_CREDENTIALS_SAME_ORIGIN && !cross && !j->cors_tainted);
    if (j->hop_cookies) {
        struct webcookie_context c = {current, cookie_site_url(j), method,
            top_level_navigation(j), true, j->redirect_cross_site};
        time_t now = time(NULL);
        long size = webcookie_get(j->cookies, &c, NULL, 0, now);
        if (size < 0) return fail(j, "Cannot measure request cookies");
        if (size) {
            if (!header_text(j, &j->outgoing, "Cookie: ") || !header_reserve(j, &j->outgoing, (size_t)size)) return false;
            long wrote = webcookie_get(j->cookies, &c, j->outgoing.p + j->outgoing.length, (size_t)size + 1, now);
            if (wrote != size) return fail(j, "Cannot serialize complete request cookies");
            j->outgoing.length += (size_t)wrote;
            if (!header_text(j, &j->outgoing, "\r\n")) return false;
        }
    }
    return true;
}
/* Headers have already been validated. Rewrites can only shorten the owned buffer. */
static void remove_request_header(char *headers, const char *name) {
    size_t nl = strlen(name);
    char *src = headers, *dst = headers;
    while (*src) {
        char *end = strstr(src, "\r\n");
        if (!end) break; /* Unreachable for validated request headers. */
        size_t len = (size_t)(end - src) + 2;
        bool remove = (size_t)(end - src) > nl && src[nl] == ':' && !strncasecmp(src, name, nl);
        if (!remove) { memmove(dst, src, len); dst += len; }
        src = end + 2;
    }
    *dst = 0;
}
static void remove_body_headers(char *headers) {
    remove_request_header(headers, "Content-Encoding");
    remove_request_header(headers, "Content-Language");
    remove_request_header(headers, "Content-Location");
    remove_request_header(headers, "Content-Type");
}
/* The fields this hop sends, for the browser's developer tools: the ones
   http.c's request_head adds ahead of the prepared fields, then those. The
   host is the URL's, without any userinfo, and its port only when not the
   scheme's default. Only when the request asked (WEBNET_WIRE_SENT_HEADERS). */
static bool record_sent(struct job *j, const struct http_req *q) {
    if (!(j->wire.user_navigation & WEBNET_WIRE_SENT_HEADERS)) return true;
    j->sent.length = 0;
    if (j->sent.p) j->sent.p[0] = 0;
    const char *authority = strstr(q->url, "://");
    if (!authority) return fail(j, "Invalid request URL for developer tools");
    bool tls = !strncasecmp(q->url, "https:", 6);
    authority += 3;
    const char *end = authority + strcspn(authority, "/?#");
    const char *at = memchr(authority, '@', (size_t)(end - authority));
    if (at) authority = at + 1;
    size_t host_len = (size_t)(end - authority);
    const char *port = NULL;
    for (const char *p = end; p > authority; p--) if (p[-1] == ':') { port = p - 1; break; } else if (p[-1] == ']') break;
    if (port && ((tls && !strncmp(port, ":443", (size_t)(end - port)) && end - port == 4) ||
                 (!tls && !strncmp(port, ":80", (size_t)(end - port)) && end - port == 3))) host_len = (size_t)(port - authority);
    char length[64] = "";
    if (q->body || !strcmp(q->method, "POST") || !strcmp(q->method, "PUT"))
        snprintf(length, sizeof length, "Content-Length: %zu\r\n", q->body_len);
    if (!header_append(j, &j->sent, "Host: ", 6) || !header_append(j, &j->sent, authority, host_len) ||
        !header_append(j, &j->sent, "\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\n", sizeof("\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\n") - 1) ||
        !header_append(j, &j->sent, length, strlen(length)) ||
        !header_append(j, &j->sent, q->headers ? q->headers : "", q->headers ? strlen(q->headers) : 0)) return false;
    return true;
}
/* One first-hop observation, only after trusted debug negotiation. Never
   retain/log names, values, bodies, URLs, or hashes of credential material. */
static void observe_request(struct job *j, const struct http_req *q) {
    if (!(j->wire.user_navigation & WEBNET_WIRE_DEBUG_REQUEST) || j->debug_request_measured) return;
    j->debug_request_measured = true;
    j->debug_cookie = field_exists(j->outgoing.p, "Cookie");
    j->debug_content_length = q->body || !strcmp(q->method, "POST") || !strcmp(q->method, "PUT");
    const char *authority = strstr(q->url, "://") + 3;
    const char *path = strchr(authority, '/');
    size_t authority_len = path ? (size_t)(path - authority) : strlen(authority);
    char length[64] = "";
    if (j->debug_content_length) snprintf(length, sizeof length, "Content-Length: %zu\r\n", q->body_len);
    j->debug_header_bytes = (uint64_t)j->outgoing.length + authority_len +
        sizeof("Host: \r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\nConnection: close\r\n\r\n") - 1 + strlen(length);
    for (size_t i = 0; i < j->outgoing.length; i++) {
        unsigned char c = (unsigned char)j->outgoing.p[i];
        if (c >= 128) j->debug_non_ascii++;
        if ((c < 32 && c != '\t' &&
             !(c == '\r' && i + 1 < j->outgoing.length && j->outgoing.p[i + 1] == '\n') &&
             !(c == '\n' && i && j->outgoing.p[i - 1] == '\r')) || c == 127) j->debug_ctl++;
    }
}
static bool cors_allowed(struct job *j, const struct http_resp *r) {
    const char *headers = http_response_headers(r);
    const char *allow; size_t length;
    const char *origin = request_origin(j);
    if (!field_slice(headers, "Access-Control-Allow-Origin", &allow, &length))
        return fail(j, "Cross-origin response is not allowed");
    bool wildcard = length == 1 && allow[0] == '*';
    if (!wildcard && (length != strlen(origin) || memcmp(allow, origin, length)))
        return fail(j, "Cross-origin response is not allowed");
    if (j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE) {
        char credentials[16];
        if (wildcard || !field(headers,"Access-Control-Allow-Credentials",credentials,sizeof credentials) ||
            strcmp(credentials,"true")) return fail(j,"Credentialed CORS requires exact origin and credentials permission");
    }
    return true;
}
/* ---- Fetch's timing record (http.h) ----
   Fetch "get, decode, and split": whether a list value is exactly token. A
   quoted string stays one value with its quotes, commas inside it included. */
static bool split_list_has(const char *value, const char *token) {
    size_t length = strlen(token);
    for (const char *p = value;;) {
        const char *start = p;
        while (*p && *p != ',') {
            if (*p != '"') { p++; continue; }
            for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
            if (*p) p++;
        }
        const char *a = start, *b = p;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if ((size_t)(b - a) == length && !memcmp(a, token, length)) return true;
        if (!*p) return false;
        p++;
    }
}
/* Fetch "get" over every name field: the values combined with ", " into
   *out (NULL when there is none). False only when memory runs out. */
static bool combined_field(struct job *j, const char *headers, const char *name, char **out) {
    *out = NULL;
    size_t name_length = strlen(name), total = 0;
    const char *cursor = headers, *value; size_t n;
    bool any = false;
    while (field_value_next(&cursor, name, name_length, &value, &n)) {
        if (n > SIZE_MAX - total - 3) return fail(j, "Out of memory combining a response field");
        total += n + 2; any = true;
    }
    if (!any) return true;
    char *combined = malloc(total + 1), *at = combined;
    if (!combined) return fail(j, "Out of memory combining a response field");
    cursor = headers;
    while (field_value_next(&cursor, name, name_length, &value, &n)) {
        const char *end = value + n;
        while (value < end && (*value == ' ' || *value == '\t')) value++;
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (at != combined) { memcpy(at, ", ", 2); at += 2; }
        memcpy(at, value, (size_t)(end - value)); at += end - value;
    }
    *at = 0; *out = combined;
    return true;
}
/* Fetch's TAO check of one hop's response (HTTP fetch). A failure stays for
   the rest of the request: the request's timing allow failed flag. */
static bool timing_allow_check(struct job *j, const char *headers) {
    if (j->timing_allow_failed) return true;
    char *values;
    if (!combined_field(j, headers, "Timing-Allow-Origin", &values)) return false;
    bool allowed = values && (split_list_has(values, "*") || split_list_has(values, request_origin(j)));
    free(values);
    if (allowed) return true;
    bool cross = !j->origin || !j->target_origin || strcmp(j->origin, j->target_origin);
    bool navigate = j->wire.kind == WEBNET_NAVIGATION;
    /* A navigation's tainting is basic; otherwise basic means it never left
       the request origin (cors and opaque tainting are kept once set). */
    bool basic = navigate ? !cross : !j->cors_tainted && !j->opaque_tainted && !j->resource_opaque;
    if (!basic) j->timing_allow_failed = true;
    return true;
}
/* What one hop adds to the record: the transport's times and connection for
   a network exchange (a 304 revalidation included), the cache state, and
   the body info (a stored response's own when it came from the cache). */
static void timing_hop(struct job *j, bool network, bool cached, const struct http_transport_result *transport) {
    struct http_fetch_timing *f = &j->timing;
    if (network) {
        f->connection_valid = true; f->connection = transport->connection;
        f->final_network_request_start = transport->request_start;
        f->final_network_response_start = transport->final_response_start;
        if (!f->first_interim_network_response_start)
            f->first_interim_network_response_start = transport->first_interim_response_start;
    }
    f->cache_state = !cached ? HTTP_CACHE_STATE_NONE : network ? HTTP_CACHE_STATE_VALIDATED : HTTP_CACHE_STATE_LOCAL;
    if (cached && j->cached) {
        f->encoded_body_size = j->cached->encoded_size;
        memcpy(f->content_encoding, j->cached->content_encoding, sizeof f->content_encoding);
    } else {
        f->encoded_body_size = transport->encoded_body_size;
        memcpy(f->content_encoding, transport->content_encoding, sizeof f->content_encoding);
    }
    f->decoded_body_size = j->hop_decoded;
}
/* The fetch ended (its body's end, or a network error): the end time, the
   flags only the whole request decides, and HTML's redirect count exposure
   of a navigation (create and initialize a Document). */
static void timing_finish(struct job *j) {
    struct http_fetch_timing *f = &j->timing;
    f->end_time = uptime_ms();
    f->timing_allow_passed = !j->timing_allow_failed && !j->error[0] && j->status;
    f->redirect_same_origin = !j->origin_tainted;
    if (j->wire.kind != WEBNET_NAVIGATION) return;
    /* Fetch's navigation TAO check: every redirect hop's values allow the
       destination origin. */
    bool tao = j->target_origin != NULL;
    for (size_t i = 0; tao && i < j->redirect_tao_count; i++)
        tao = j->redirect_tao[i] && (split_list_has(j->redirect_tao[i], "*") || split_list_has(j->redirect_tao[i], j->target_origin));
    bool no_client = (j->fetch.flags & WEBNET_FETCH_BROWSER_UI) || !j->document || !*j->document;
    f->navigation_redirects_exposed = f->redirect_same_origin || ((no_client || (j->referrer && *j->referrer)) && tao);
}
/* The private lines that carry the record (WEBNET_WIRE_TIMING): one with its
   numbers, then the Server-Timing values when the response had any. The
   browser's webnet.c takes them out of the header block. */
static char *timing_lines(const struct job *j, size_t *length) {
    const struct http_fetch_timing *f = &j->timing;
    const struct http_connection_timing *c = &f->connection;
    char line[768];
    int n = snprintf(line, sizeof line,
        "\r\nHTTP/Nocturne-Timing redirects=%lu redirect_end=%llu post_redirect=%llu lookup_start=%llu lookup_end=%llu "
        "connect_start=%llu connect_end=%llu secure_start=%llu request=%llu interim=%llu response=%llu end=%llu "
        "cache=%u tao=%u same_origin=%u navigation_redirects=%u connection=%u encoded=%llu decoded=%llu protocol=%s encoding=%s\r\n",
        (unsigned long)f->redirect_count, (unsigned long long)f->redirect_end, (unsigned long long)f->post_redirect_start,
        (unsigned long long)c->domain_lookup_start, (unsigned long long)c->domain_lookup_end,
        (unsigned long long)c->connect_start, (unsigned long long)c->connect_end, (unsigned long long)c->secure_connection_start,
        (unsigned long long)f->final_network_request_start, (unsigned long long)f->first_interim_network_response_start,
        (unsigned long long)f->final_network_response_start, (unsigned long long)f->end_time,
        (unsigned)f->cache_state, (unsigned)f->timing_allow_passed, (unsigned)f->redirect_same_origin,
        (unsigned)f->navigation_redirects_exposed, (unsigned)f->connection_valid,
        (unsigned long long)f->encoded_body_size, (unsigned long long)f->decoded_body_size,
        f->connection_valid ? c->protocol : "", f->content_encoding);
    if (n < 0 || (size_t)n >= sizeof line) return NULL;
    static const char mark[] = "HTTP/Nocturne-Server-Timing ";
    size_t server = f->server_timing ? strlen(f->server_timing) : 0;
    if (server > SIZE_MAX - (size_t)n - sizeof mark - 2) return NULL;
    size_t total = (size_t)n + (server ? sizeof mark - 1 + server + 2 : 0);
    char *out = malloc(total + 1);
    if (!out) return NULL;
    memcpy(out, line, (size_t)n);
    if (server) {
        char *at = out + n;
        memcpy(at, mark, sizeof mark - 1); at += sizeof mark - 1;
        memcpy(at, f->server_timing, server); at += server;
        memcpy(at, "\r\n", 2);
    }
    out[total] = 0; *length = total;
    return out;
}
static int cookie_header(void *opaque, const char *line, size_t length) {
    struct job *j = opaque;
    if (!j->hop_cookies || length < 11 || strncasecmp(line,"Set-Cookie:",11)) return 0;
    const char *value = line+11, *end=line+length;
    while (value<end && (*value==' ' || *value=='\t')) value++;
    size_t vl=(size_t)(end-value), ul=strlen(j->final_url), limit=webnet_wire_cookie_limit(j->wire.user_navigation);
    uint64_t size=sizeof(struct webnet_wire_cookie)+(uint64_t)ul+vl;
    if (vl>WEBCOOKIE_FIELD_MAX || ul>UINT32_MAX || j->cookie_len>limit || size>limit-j->cookie_len || size>SIZE_MAX) {
        fail(j,"Cookie response fields exceed bounded storage"); return -1;
    }
    size_t need=(size_t)size;
    if (j->cookie_len+need>j->cookie_cap) {
        size_t cap=j->cookie_cap?j->cookie_cap:8192;
        while(cap<j->cookie_len+need){if(cap>limit/2){cap=j->cookie_len+need;break;}cap*=2;}
        unsigned char *p=realloc(j->cookie_events,cap);
        if(!p){fail(j,"Out of memory receiving cookies");return -1;}
        j->cookie_events=p;j->cookie_cap=cap;
    }
    struct webnet_wire_cookie e={(uint32_t)ul,(uint32_t)vl,j->redirect_cross_site,0,time(NULL)};
    unsigned char *p=j->cookie_events+j->cookie_len;memcpy(p,&e,sizeof e);p+=sizeof e;
    memcpy(p,j->final_url,ul);memcpy(p+ul,value,vl);j->cookie_len+=need;return 0;
}
static bool apply_cookie_events(struct job *j,size_t start,const char *method) {
    if(start==j->cookie_len)return true;
    const unsigned char *p=j->cookie_events+start,*end=j->cookie_events+j->cookie_len;
    while(p<end){
        struct webnet_wire_cookie e;memcpy(&e,p,sizeof e);p+=sizeof e;
        char *url=malloc((size_t)e.url_len+1);if(!url)return fail(j,"Out of memory applying cookie URL");
        memcpy(url,p,e.url_len);url[e.url_len]=0;p+=e.url_len;
        struct webcookie_context c={url,cookie_site_url(j),method,
            top_level_navigation(j),true,e.redirect_cross_site!=0};
        int result=webcookie_set(j->cookies,&c,(const char*)p,e.value_len,e.received);free(url);
        if(result<0)return fail(j,"Out of memory applying cookies");
        p+=e.value_len;
    }
    return true;
}
static int body_cb(void *opaque, const char *data, size_t n) {
    struct job *j = opaque;
    if (!remaining(j)||request_cancelled(j)) return -1;
    j->hop_decoded += n; /* Fetch's decoded size: the bytes after content codings */
    if(j->cache_capture){
        struct header_buffer *b=&j->cache_body;
        if(n>FETCH_CACHE_OBJECT-b->length){free(b->p);*b=(struct header_buffer){0};j->cache_capture=false;}
        else{
            size_t need=b->length+n+1;
            if(need>b->capacity){size_t cap=MAX(need,MIN(b->capacity?b->capacity*2:8192,FETCH_CACHE_OBJECT+1));
                char *next=realloc(b->p,cap);
                if(!next){free(b->p);*b=(struct header_buffer){0};j->cache_capture=false;}
                else{b->p=next;b->capacity=cap;}}
            if(j->cache_capture){memcpy(b->p+b->length,data,n);b->length+=n;b->p[b->length]=0;}
        }
    }
    if(j->opaque_tainted||j->opaque_redirect||j->wire.kind==WEBNET_REPORT)return 0; /* Retain no opaque/report response bytes. */
    if (j->wire.user_navigation & WEBNET_WIRE_STREAM) {
        while (n) {
            size_t count = MIN(n, WEBNET_STREAM_CHUNK_MAX);
            struct webnet_wire_response frame = {0};
            frame.magic = WEBNET_STREAM_CHUNK; frame.id = j->wire.id; frame.generation = j->wire.generation;
            frame.body_len = (uint32_t)count;
            if (!job_write(j, &frame, sizeof frame) || !job_write(j, data, count)) return -1;
            data += count; n -= count;
        }
        return 0;
    }
    size_t limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
    if (j->body_len > limit || n > limit - j->body_len) {
        fail(j, "Response body exceeds the negotiated wire representation"); return -1;
    }
    size_t need = j->body_len + n + 1;
    if (need > j->body_cap) {
        size_t cap = j->body_cap ? j->body_cap : 8192;
        while (cap < need) cap = cap > SIZE_MAX / 2 ? need : MIN(cap * 2, limit + 1);
        char *p = realloc(j->body, cap);
        if (!p) { fail(j, "Out of memory receiving response"); return -1; }
        j->body = p; j->body_cap = cap;
    }
    memcpy(j->body + j->body_len, data, n); j->body_len += n; j->body[j->body_len] = 0;
    return 0;
}
static int discard_cb(void *opaque, const char *data, size_t n) {
    (void)data;
    struct job *j = opaque;
    if (!remaining(j)||request_cancelled(j)) return -1;
    if (j->body_len > HTTP_RESPONSE_BODY_MAX || n > HTTP_RESPONSE_BODY_MAX - j->body_len) { fail(j, "Preflight response length is not representable"); return -1; }
    j->body_len += n; return 0;
}
static bool preflight(struct job *j, const char *url, const char *method, const char *names) {
    struct header_buffer head = {0};
    bool prepared = header_text(j, &head, "Origin: ") && header_text(j, &head, request_origin(j)) &&
        header_text(j, &head, "\r\nAccess-Control-Request-Method: ") && header_text(j, &head, method) && header_text(j, &head, "\r\n");
    if (prepared && *names) prepared = header_text(j, &head, "Access-Control-Request-Headers: ") &&
        header_text(j, &head, names) && header_text(j, &head, "\r\n");
    int left = prepared ? remaining(j) : 0;
    if (!left) { free(head.p); return false; }
    struct http_req q = {"OPTIONS", url, head.p, NULL, 0, left, discard_cb, j, NULL};
    struct http_resp r;
    const struct http_stream_hooks hooks={.cancelled=request_cancelled};
    j->body_len = 0;
    int result=http_request_stream_ex(&q,&r,HTTP_RESPONSE_BODY_MAX,&hooks,NULL,j->partition,NULL);
    free(head.p);
    j->body_len = 0;
    if (result < 0) { if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
    if (r.status < 200 || r.status >= 300) { http_resp_free(&r); return fail(j, "CORS preflight failed"); }
    if (!cors_allowed(j, &r)) { http_resp_free(&r); return false; }
    const char *methods = NULL, *allowed = NULL; size_t methods_len = 0, allowed_len = 0;
    /* Safelisted methods do not need an Allow-Methods entry. XHR upload
       listeners can force OPTIONS even for a simple GET/POST request. */
    bool ok = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST") ||
              (field_slice(http_response_headers(&r), "Access-Control-Allow-Methods", &methods, &methods_len) &&
               (list_has_slice(methods, methods + methods_len, method, strlen(method), false) ||
                (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && list_has_slice(methods, methods + methods_len, "*", 1, false))));
    if (*names) {
        ok = ok && field_slice(http_response_headers(&r), "Access-Control-Allow-Headers", &allowed, &allowed_len);
        for (const char *p = names; ok && *p;) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = strchr(p, ','); if (!e) e = p + strlen(p);
            size_t l = e - p; while (l && p[l - 1] == ' ') l--;
            /* Authorization is never covered by the CORS header wildcard. */
            ok = list_has_slice(allowed, allowed + allowed_len, p, l, true) ||
                 (j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE && !field_name_is(p, l, "authorization") &&
                  list_has_slice(allowed, allowed + allowed_len, "*", 1, false));
            p = *e ? e + 1 : e;
        }
    }
    http_resp_free(&r);
    return ok || fail(j, "CORS preflight did not allow method or headers");
}
static bool js_mime(const char *headers) {
    char mime[256];
    if (!field(headers, "Content-Type", mime, sizeof mime)) return false;
    char *semi = strchr(mime, ';'); if (semi) *semi = 0;
    size_t n = strlen(mime); while (n && mime[n - 1] == ' ') mime[--n] = 0;
    return !strcasecmp(mime, "text/javascript") || !strcasecmp(mime, "application/javascript") ||
           !strcasecmp(mime, "text/ecmascript") || !strcasecmp(mime, "application/ecmascript") ||
           !strcasecmp(mime, "application/x-javascript");
}
/* A rejected response deliberately has no body/status/header exposure on the
   wire. Preserve only bounded, non-secret rejection metadata in its error;
   otherwise a real HTTP denial becomes an indistinguishable "HTTP 0". Never
   include a response body, URL suffix, cookies, or arbitrary header values. */
static bool reject_script_response(struct job *j, const struct http_resp *r,
                                   const char *message, const char *reason) {
    const char *headers = http_response_headers(r), *value;
    size_t length;
    char mime[49] = "(missing)", nosniff[64];
    if (field_slice(headers, "Content-Type", &value, &length)) {
        const char *semi = memchr(value, ';', length);
        if (semi) length = (size_t)(semi - value);
        while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) length--;
        size_t n = MIN(length, sizeof mime - 1);
        bool valid = n != 0;
        for (size_t i = 0; i < length && valid; i++)
            if (value[i] != '/' && !token_char((unsigned char)value[i])) valid = false;
        if (valid) { memcpy(mime, value, n); mime[n] = 0; }
        else strcpy(mime, "(invalid)");
    } else if (field_exists(headers, "Content-Type")) strcpy(mime, "(invalid)");
    const char *sniff = !field_exists(headers, "X-Content-Type-Options") ? "absent" :
        !field(headers, "X-Content-Type-Options", nosniff, sizeof nosniff) ? "invalid" :
        !strcasecmp(nosniff, "nosniff") ? "nosniff" : "other";
    snprintf(j->error, sizeof j->error, "%s (HTTP %d; MIME %.48s; nosniff=%s; reason=%s)",
             message, r->status, mime, sniff, reason);
    return false;
}
/* List-valued exposure fields may repeat. Scan every complete value without
   copying it into a smaller temporary array or dropping long field names. */
static bool exposed_header(const char *headers, const char *name, size_t len, bool wildcard) {
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && field_name_is(p, (size_t)(colon - p), "Access-Control-Expose-Headers") &&
            (list_has_slice(colon + 1, end, name, len, true) ||
             (wildcard && list_has_slice(colon + 1, end, "*", 1, false)))) return true;
        p = *end ? end + 2 : end;
    }
    return false;
}
static bool include_response_field(const struct job *j, const char *headers, const char *p, const char *end, bool cross) {
    if(j->opaque_tainted||j->opaque_redirect)return false;
    const char *colon = memchr(p, ':', (size_t)(end - p));
    if (!colon || colon == p) return false;
    size_t len = (size_t)(colon - p);
    if (field_name_is(p, len, "set-cookie") || field_name_is(p, len, "set-cookie2")) return false;
    if (j->wire.kind != WEBNET_FETCH || !cross) return true;
    const char *safe = "cache-control,content-language,content-length,content-type,expires,last-modified,pragma";
    return list_has_slice(safe, safe + strlen(safe), p, len, true) ||
        exposed_header(headers, p, len, j->wire.credentials != WEBNET_CREDENTIALS_INCLUDE);
}
static bool response_headers(struct job *j, const struct http_resp *r, bool cross) {
    if(j->wire.kind==WEBNET_REPORT){free(j->response_headers);j->response_headers=NULL;j->response_headers_len=0;return true;}
    const char *headers = http_response_headers(r);
    char *prefix = NULL;
    size_t prefix_len = 0;
    /* Negotiated private metadata; old clients retain the header-only format.
       The metadata line has no colon and cannot pass the server-field filter
       below. The genuine status phrase cannot inject lines (http.c validates
       CR/LF before storing it). Keep this prefix ahead of all server fields. */
    if (j->wire.user_navigation & WEBNET_WIRE_STATUS_LINE) {
        const char *reason = http_response_status_text(r);
        size_t reason_len = strlen(reason);
        /* A navigation's document learns the referrer its last hop actually
           sent (HTML: the document's referrer), already a canonical URL. */
        const char *referrer = j->wire.kind == WEBNET_NAVIGATION && j->referrer ? j->referrer : "";
        size_t referrer_len = strlen(referrer);
        char start[32], end[320], resource[16] = "";
        /* What Fetch's cross-origin resource policy check needs of the response an opaque one
           hides, for a later check against another embedder policy (Cache Storage's match):
           1 a valid Cross-Origin-Resource-Policy, 2 a URL of another origin than the request's,
           4 a request that included credentials. Nothing else of it leaves this process. */
        if (j->opaque_tainted || j->resource_opaque) {
            const char *policy = NULL; size_t length = 0;
            bool other = !j->origin || !j->target_origin || strcmp(j->origin, j->target_origin);
            bool corp = field_slice(http_response_headers(r), "Cross-Origin-Resource-Policy", &policy, &length) &&
                ((length == 11 && !memcmp(policy, "same-origin", 11)) || (length == 9 && !memcmp(policy, "same-site", 9)) ||
                 (length == 12 && !memcmp(policy, "cross-origin", 12)));
            bool credentials = j->wire.credentials == WEBNET_CREDENTIALS_INCLUDE ||
                (j->wire.credentials == WEBNET_CREDENTIALS_SAME_ORIGIN && !other);
            snprintf(resource, sizeof resource, " resource=%u", (corp ? 1u : 0u) | (other ? 2u : 0u) | (credentials ? 4u : 0u));
        }
        int a = snprintf(start, sizeof start, "HTTP/1.1 %d ", r->status);
        int m = snprintf(end, sizeof end, "\r\nHTTP/Nocturne-Meta cors=%d redirected=%d%s%s%s%s", j->cors_tainted, j->redirected,
                         j->opaque_tainted||j->resource_opaque?" opaque=1":"", resource, j->opaque_redirect?" opaqueredirect=1":"",
                         referrer_len?" referrer=":"");
        int b = m >= 0 && (size_t)m < sizeof end ? snprintf(end + m, sizeof end - (size_t)m, "\r\n") : -1;
        if (b >= 0) b += m;
        if (b >= 0 && (size_t)b < sizeof end && j->debug_request_measured) {
            int c = snprintf(end + b, sizeof end - (size_t)b,
                "HTTP/Nocturne-Request bytes=%lu ctl=%lu nonascii=%lu cookie=%u content_length=%u\r\n",
                (unsigned long)j->debug_header_bytes, (unsigned long)j->debug_ctl,
                (unsigned long)j->debug_non_ascii, (unsigned)j->debug_cookie, (unsigned)j->debug_content_length);
            if (c < 0 || (size_t)c >= sizeof end - (size_t)b) return fail(j, "Request diagnostic metadata length overflow");
            b += c;
        }
        if (a < 0 || b < 0 || (size_t)a >= sizeof start || (size_t)b >= sizeof end || referrer_len > 4096 || strpbrk(referrer, "\r\n ") ||
            reason_len > SIZE_MAX - (size_t)a - (size_t)b - referrer_len - 1) return fail(j, "Response metadata length overflow");
        /* The sent fields: "HTTP/Nocturne-Sent " ahead of each line. Field
           values never hold CR or LF (validated before the request went). */
        size_t sent_lines = 0;
        const char *sent = (j->wire.user_navigation & WEBNET_WIRE_SENT_HEADERS) && j->sent.p ? j->sent.p : "";
        for (const char *p = sent; *p; p = strstr(p, "\r\n") + 2) sent_lines++;
        size_t sent_len = strlen(sent);
        if (sent_lines > (SIZE_MAX - sent_len) / 32) return fail(j, "Response metadata length overflow");
        sent_len += sent_lines * (sizeof "HTTP/Nocturne-Sent " - 1);
        if (sent_len > SIZE_MAX - (size_t)a - (size_t)b - referrer_len - reason_len - 1) return fail(j, "Response metadata length overflow");
        prefix_len = (size_t)a + reason_len + (size_t)b + referrer_len + sent_len;
        if (prefix_len > webnet_wire_header_limit(j->wire.user_navigation)) return fail(j, "Response metadata exceeds negotiated representation");
        prefix = malloc(prefix_len + 1);
        if (!prefix) return fail(j, "Out of memory storing response metadata");
        char *at = prefix;
        memcpy(at, start, (size_t)a); at += a;
        memcpy(at, reason, reason_len); at += reason_len;
        memcpy(at, end, (size_t)m); at += m;
        memcpy(at, referrer, referrer_len); at += referrer_len;
        memcpy(at, end + m, (size_t)(b - m)); at += b - m;
        for (const char *p = sent; *p;) {
            const char *line_end = strstr(p, "\r\n") + 2;
            memcpy(at, "HTTP/Nocturne-Sent ", sizeof "HTTP/Nocturne-Sent " - 1); at += sizeof "HTTP/Nocturne-Sent " - 1;
            memcpy(at, p, (size_t)(line_end - p)); at += line_end - p;
            p = line_end;
        }
        *at = 0;
    }
    size_t limit = webnet_wire_header_limit(j->wire.user_navigation);
    size_t required = prefix_len;
    /* Measure the complete filtered output first; capacity is never a reason
       to turn an included field into an omitted field. */
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if (include_response_field(j, headers, p, end, cross)) {
            size_t l = (size_t)(end - p);
            if (l > SIZE_MAX - 2 || required > limit || l + 2 > limit - required) {
                free(prefix); return fail(j, "Response headers exceed negotiated representation");
            }
            l += 2;
            required += l;
        }
        p = *end ? end + 2 : end;
    }
    if (required > limit || required == SIZE_MAX) { free(prefix); return fail(j, "Response header length overflow"); }
    char *out = malloc(required + 1);
    if (!out) { free(prefix); return fail(j, "Out of memory filtering response headers"); }
    if (prefix_len) memcpy(out, prefix, prefix_len);
    free(prefix);
    size_t used = prefix_len;
    for (const char *p = headers; *p;) {
        const char *end = strstr(p, "\r\n"); if (!end) end = p + strlen(p);
        if (include_response_field(j, headers, p, end, cross)) {
            size_t l = (size_t)(end - p);
            memcpy(out + used, p, l); used += l;
            memcpy(out + used, "\r\n", 2); used += 2;
        }
        p = *end ? end + 2 : end;
    }
    out[used] = 0;
    free(j->response_headers); j->response_headers = out; j->response_headers_len = used;
    return true;
}
static int stream_upload(void *opaque, size_t sent, size_t total) {
    struct job *j = opaque;
    if (!(j->wire.user_navigation & WEBNET_WIRE_STREAM) || j->upload_finished) return 0;
    struct webnet_wire_response frame = {0};
    frame.magic = WEBNET_STREAM_UPLOAD; frame.id = j->wire.id; frame.generation = j->wire.generation;
    frame.status = (uint32_t)sent;
    if (sent == total) j->upload_finished = true;
    return job_write(j, &frame, sizeof frame) ? 0 : -1;
}
static int response_start(void *opaque, const struct http_resp *response) {
    struct job *j = opaque;
    if (!j->cache_replay&&!apply_cookie_events(j, j->cookie_start, j->hop_method)) return -1;
    /* A 304 is metadata for an owned stored response. Its omitted CORS fields
       and status must not be published as a new Fetch response. */
    if(response->status==304&&j->cached)return 1;
    if (j->cors_tainted && !cors_allowed(j, response)) return -1;
    int status = response->status;
    bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (redirect) {
        if (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_ERROR) { fail(j, "Redirect forbidden by redirect mode"); return -1; }
        /* "manual": this response is the fetch's, after its CORS check above, as an
           opaque-redirect filtered response, with or without a Location. */
        if (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_MANUAL) j->opaque_redirect = true;
        /* A missing Location is a received response, not a network failure. */
        else if (field_exists(http_response_headers(response), "Location")) return 1;
    }
    bool cross = !j->origin || strcmp(j->origin, j->target_origin);
    if (j->opaque_tainted && cross) {
        const char *policy = NULL; size_t length = 0;
        if (field_exists(http_response_headers(response), "Cross-Origin-Resource-Policy") &&
            !field_slice(http_response_headers(response), "Cross-Origin-Resource-Policy", &policy, &length)) {
            fail(j, "Invalid Cross-Origin-Resource-Policy"); return -1;
        }
        /* COEP require-corp: a missing policy is same-origin. */
        if (!policy && (j->wire.user_navigation & WEBNET_WIRE_REQUIRE_CORP)) {
            fail(j, "No-cors response blocked by Cross-Origin-Embedder-Policy: missing Cross-Origin-Resource-Policy"); return -1;
        }
        if (policy && ((length == 11 && !memcmp(policy, "same-origin", 11)) ||
            (length == 9 && !memcmp(policy, "same-site", 9) && !webcookie_same_site(j->cookies, j->document, j->final_url)))) {
            fail(j, "No-cors response blocked by Cross-Origin-Resource-Policy"); return -1;
        }
    }
    j->stream_final = true;
    const char *raw=http_response_headers(response);
    bool default_cacheable=status==200||status==203||status==204||status==300||status==404||status==405||status==410||status==414||status==501;
    j->cache_capture=!j->cache_replay&&!j->opaque_redirect&&!strcmp(j->hop_method,"GET")&&j->partition&&
        cache_context_allowed(j)&&j->wire.kind!=WEBNET_REPORT&&
        cache_mode(j)!=WEBNET_CACHE_NO_STORE&&!cache_directive(j->headers,"no-store",NULL)&&
        !field_exists(j->headers,"Range")&&!cache_directive(raw,"no-store",NULL)&&
        (default_cacheable||(status>=200&&status<600&&(field_exists(raw,"Expires")||cache_directive(raw,"max-age",NULL))))&&
        strlen(raw)<FETCH_CACHE_OBJECT;
    if (!(j->wire.user_navigation & WEBNET_WIRE_STREAM)) return 0;
    if (!response_headers(j, response, j->cors_tainted)) return -1;
    struct webnet_wire_response frame = {0};
    frame.magic = WEBNET_STREAM_HEADERS; frame.status = status;
    frame.id = j->wire.id; frame.generation = j->wire.generation;
    frame.url_len = strlen(j->final_url); frame.headers_len = j->response_headers_len;
    frame.cookie_len = j->cookie_len;
    if (!job_write(j, &frame, sizeof frame) || !job_write(j, j->final_url, frame.url_len) ||
        !job_write(j, j->response_headers, frame.headers_len) || !job_write(j, j->cookie_events, frame.cookie_len)) return -1;
    j->stream_headers_sent = true;
    /* Native cookies were published with the headers, not deferred until EOF. */
    j->cookie_len = 0;
    return 0;
}
static int cache_response(struct job *j,struct cache_entry *e,struct http_resp *r) {
    memset(r,0,sizeof *r);r->status=e->status;
    r->headers_full=cache_clean_headers(e->headers,NULL,true,cache_age(e));
    r->status_text_full=strdup(e->reason);
    if(!r->headers_full||!r->status_text_full){fail(j,"Out of memory reading cached response");return -1;}
    r->headers_len=strlen(r->headers_full);j->cache_replay=true;j->cache_capture=false;
    int result=response_start(j,r);
    if(result==0&&strcmp(j->hop_method,"HEAD"))result=body_cb(j,e->body,e->body_len);
    j->cache_replay=false;return result<0?-1:0;
}
static bool transport_retry(const struct job *j,const struct http_req *q,const struct http_resp *r,
                            const struct http_transport_result *transport,size_t cookie_start) {
    if(r->status||transport->response_bytes_received||j->body_len||j->stream_headers_sent||
       j->cookie_len!=cookie_start||j->error[0]||request_cancelled((void *)j))return false;
    switch(transport->error){
    case HTTP_ERR_DNS_TEMP:case HTTP_ERR_CONNECT:case HTTP_ERR_RESET:case HTTP_ERR_IO:break;
    default:return false;
    }
    /* Before the first byte is sent even a POST can safely try another
       connection. A partially submitted unsafe operation is never replayed. */
    if(!transport->request_bytes_sent)return transport->phase<=HTTP_PHASE_HEADER;
    return !q->body&&(!strcmp(q->method,"GET")||!strcmp(q->method,"HEAD"));
}
static bool run_http(struct job *j) {
    if (top_level_navigation(j)) {
        /* Native chrome navigations have no client document. The previous
           page (an error page for an address the user mistyped) is only
           context; an unparsable one makes the request browser-initiated. */
        char *context[2] = {j->document, j->top_level_url};
        for (int i = 0; i < 2; i++) {
            if (!context[i] || !*context[i]) continue;
            char *origin = NULL;
            enum http_url_result result = http_url(context[i], NULL, &origin);
            free(origin);
            if (result == HTTP_URL_OOM) return url_failure(j, result, "");
            if (result == HTTP_URL_TUPLE || result == HTTP_URL_OPAQUE) continue;
            if (i) { free(j->top_level_url); j->top_level_url = NULL; }
            else j->document[0] = 0;
        }
    }
    enum http_url_result parsed = *j->document ? http_url(j->document, NULL, &j->origin) : HTTP_URL_OPAQUE;
    if (parsed != HTTP_URL_TUPLE && parsed != HTTP_URL_OPAQUE)
        return url_failure(j, parsed, "Invalid initiating document URL");
    bool document_http = parsed == HTTP_URL_TUPLE;
    parsed = image_http_url(j, j->url, &j->final_url, &j->target_origin);
    if (parsed != HTTP_URL_TUPLE) return url_failure(j, parsed, "Invalid HTTP(S) URL");
    if (!validate_headers(j)) return false;
    if(!prepare_referrer(j))return false;
    if(no_cors(j) && strcmp(j->method,"GET") && strcmp(j->method,"HEAD") && strcmp(j->method,"POST"))return fail(j,"No-cors request requires a safelisted method");
    if (WEBNET_WIRE_CACHE_MODE(j->wire.user_navigation) == WEBNET_CACHE_ONLY_IF_CACHED) {
        if (!(j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "only-if-cached requires same-origin mode");
    }
    bool drop_body = false;
    for (unsigned hop = 0; hop <= 20; hop++) {
        /* Initial and every redirect hop were fully prepared before publish.
           The current final URL is the single canonical owner, not a copy. */
        const char *current=j->final_url;
        if (!top_level_navigation(j) && document_http && !strncmp(j->origin, "https://", 8) && !strncmp(current, "http://", 7)) return fail(j, "HTTPS page cannot load insecure content");
        bool cross = !j->origin || strcmp(j->origin, j->target_origin) != 0;
        if (cross && (j->wire.user_navigation & WEBNET_WIRE_SAME_ORIGIN))
            return fail(j, "Cross-origin fetch is forbidden by same-origin mode");
        /* Main fetch never changes cors response taint back to basic, even
           when a redirect returns to the original document's origin. */
        if (cors_kind(j) && cross) j->cors_tainted = true;
        if(no_cors(j) && cross)j->opaque_tainted=true;
        /* Fetch's main fetch: a no-cors request to another origin is a network error unless its
           redirect mode is "follow" (an opaque-redirect would tell where that origin redirects). */
        if(no_cors(j) && cross && (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_MANUAL))
            return fail(j,"No-cors request to another origin requires redirect mode follow");
        if(j->wire.kind==WEBNET_RESOURCE && cross)j->resource_opaque=true;
        note_fetch_site(j, current, j->target_origin);
        const char *method = drop_body ? "GET" : j->method;
        /* Redirects can remove authorization/body fields; rederive the preflight names. */
        if (!validate_headers(j)) return false;
        const char *names = j->non_simple;
        bool simple_method = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "POST");
        if (!prepare_outgoing(j, current, method, cross)) return false;
        if(!cache_partition(j))return false;
        int left = remaining(j); if (!left) return false;
        free(j->body); j->body = NULL; j->body_len = j->body_cap = 0; j->hop_decoded = 0;
        struct http_req q = {method, current, j->outgoing.p,
                             !drop_body && (j->wire.body_len || (j->wire.user_navigation & WEBNET_WIRE_BODY_PRESENT)) ?
                                 j->request_body : NULL,
                             drop_body ? 0 : j->wire.body_len,
                             left, body_cb, j, cookie_header};
        observe_request(j, &q);
        if (!record_sent(j, &q)) return false;
        struct http_resp r={0};
        size_t cookie_start=j->cookie_len; j->cookie_start = cookie_start; j->stream_final = false; j->hop_method = method;
        size_t body_limit = webnet_wire_response_limit(j->wire.kind, j->wire.user_navigation);
        const struct http_stream_hooks hooks = {response_start, stream_upload, request_cancelled};
        struct http_transport_timing timing={0};
        bool timed=(j->wire.user_navigation&WEBNET_WIRE_DEBUG_REQUEST)!=0;
        cache_free(j->cached);j->cached=NULL;free(j->cache_body.p);j->cache_body=(struct header_buffer){0};j->cache_capture=false;
        unsigned mode=cache_mode(j);
        bool cache_read=(!strcmp(method,"GET")||!strcmp(method,"HEAD"))&&mode!=WEBNET_CACHE_NO_STORE&&mode!=WEBNET_CACHE_RELOAD&&
            !field_exists(j->headers,"Range")&&!cache_directive(j->headers,"no-store",NULL)&&
            cache_context_allowed(j);
        if(cache_read)j->cached=cache_find(j);
        bool hit=j->cached&&cache_usable(j,j->cached);
        if(!hit&&mode==WEBNET_CACHE_ONLY_IF_CACHED)return fail(j,"No usable cached response is available");
        /* CORS permission belongs to this request's method/author fields, not
           to the cached representation's Vary key. A hit must not bypass a
           required preflight, including a redirect hop returning to a cache. */
        if(request_cancelled(j))return fail(j,"Request was cancelled");
        if(j->cors_tainted&&(!simple_method||*names||(j->wire.user_navigation&WEBNET_WIRE_FORCE_PREFLIGHT))&&
           !preflight(j,current,method,names))return false;
        if(!remaining(j))return false;
        if(request_cancelled(j))return fail(j,"Request was cancelled");
        int result;
        struct http_transport_result transport={0};
        if(hit)result=cache_response(j,j->cached,&r);
        else{
            if(j->cached){
                const char *value;size_t n;
                if(field_slice(j->cached->headers,"ETag",&value,&n)){
                    if(!header_text(j,&j->outgoing,"If-None-Match: ")||!header_append(j,&j->outgoing,value,n)||!header_text(j,&j->outgoing,"\r\n"))return false;
                }else if(field_slice(j->cached->headers,"Last-Modified",&value,&n)){
                    if(!header_text(j,&j->outgoing,"If-Modified-Since: ")||!header_append(j,&j->outgoing,value,n)||!header_text(j,&j->outgoing,"\r\n"))return false;
                }
                q.headers=j->outgoing.p;
            }
            q.timeout_ms=remaining(j);if(!q.timeout_ms)return false;
            if(timed){j->live_timing=&timing;j->live_transport=&transport;j->stall_since=0;}
            result=http_request_stream_ex(&q,&r,body_limit,&hooks,timed?&timing:NULL,j->partition,&transport);
            j->live_timing=NULL;j->live_transport=NULL;
        }
        if(timed&&!hit){
            j->debug_attempts++;j->debug_timing.phase=timing.phase;j->debug_transport=transport;
            j->debug_timing.resolve_ms+=timing.resolve_ms;j->debug_timing.connect_ms+=timing.connect_ms;
            j->debug_timing.tls_ms+=timing.tls_ms;j->debug_timing.first_header_ms+=timing.first_header_ms;
            j->debug_timing.header_ms+=timing.header_ms;j->debug_timing.body_ms+=timing.body_ms;
            j->debug_timing.tls_io_ms+=timing.tls_io_ms;j->debug_timing.tls_other_ms+=timing.tls_other_ms;
        }
        if(result<0&&!hit&&transport_retry(j,&q,&r,&transport,cookie_start)) {
            int retry_left=remaining(j);
            if(retry_left) {
                http_resp_free(&r);q.timeout_ms=retry_left;
                if(timed){j->live_timing=&timing;j->live_transport=&transport;j->stall_since=0;}
                result=http_request_stream_ex(&q,&r,body_limit,&hooks,timed?&timing:NULL,j->partition,&transport);
                j->live_timing=NULL;j->live_transport=NULL;
                if(timed){
                    j->debug_attempts++;j->debug_timing.phase=timing.phase;j->debug_transport=transport;
                    j->debug_timing.resolve_ms+=timing.resolve_ms;j->debug_timing.connect_ms+=timing.connect_ms;
                    j->debug_timing.tls_ms+=timing.tls_ms;j->debug_timing.first_header_ms+=timing.first_header_ms;
                    j->debug_timing.header_ms+=timing.header_ms;j->debug_timing.body_ms+=timing.body_ms;
                    j->debug_timing.tls_io_ms+=timing.tls_io_ms;j->debug_timing.tls_other_ms+=timing.tls_other_ms;
                }
            }
        }
        if (result < 0) { j->cookie_len=j->stream_headers_sent?0:cookie_start; if (!j->error[0]) fail(j, r.error); http_resp_free(&r); return false; }
        bool network=!hit;
        if(!hit&&r.status==304&&j->cached){
            if(!cache_validator_matches(j->cached,&r)){
                http_resp_free(&r);return fail(j,"Revalidation returned a mismatched entity validator");
            }
            char *merged=cache_clean_headers(j->cached->headers,http_response_headers(&r),false,0);
            if(!merged){http_resp_free(&r);return fail(j,"Out of memory revalidating cached response");}
            free(j->cached->headers);j->cached->headers=merged;cache_freshness(j->cached);
            if(cache_directive(merged,"no-store",NULL)||!cache_vary(j->cached,j->outgoing.p))cache_invalidate(current);
            else{struct cache_entry *updated=cache_clone(j->cached);if(updated)cache_insert(updated);}
            http_resp_free(&r);result=cache_response(j,j->cached,&r);
            if(result<0){http_resp_free(&r);return false;}hit=true;
        }
        timing_hop(j,network,hit,&transport);
        if(!timing_allow_check(j,http_response_headers(&r))){http_resp_free(&r);return false;}
        bool redirect = !j->stream_final && (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308);
        if (redirect) {
            j->referrer_policy=webnet_referrer_policy_header(http_response_headers(&r),j->referrer_policy);
            /* Fetch: a navigation keeps every redirect hop's Timing-Allow-Origin values. */
            if (j->wire.kind == WEBNET_NAVIGATION) {
                char **grown = realloc(j->redirect_tao, (j->redirect_tao_count + 1) * sizeof *grown);
                if (!grown) { http_resp_free(&r); return fail(j, "Out of memory keeping redirect timing"); }
                j->redirect_tao = grown;
                if (!combined_field(j, http_response_headers(&r), "Timing-Allow-Origin", &grown[j->redirect_tao_count])) { http_resp_free(&r); return false; }
                j->redirect_tao_count++;
            }
            if (hop == 20) { http_resp_free(&r); return fail(j, "Fetch redirect count exceeded"); }
            if (j->wire.user_navigation & WEBNET_WIRE_REDIRECT_ERROR) {
                http_resp_free(&r); return fail(j, "Redirect forbidden by redirect mode");
            }
            const char *location_value;size_t location_len;
            if (!field_slice(http_response_headers(&r), "Location", &location_value, &location_len) || location_len==SIZE_MAX) {
                http_resp_free(&r); return fail(j, "Invalid or prohibited redirect target");
            }
            char *location=malloc(location_len+1),*resolved=NULL;
            if(!location){http_resp_free(&r);return fail(j,"Out of memory preparing redirect target");}
            memcpy(location,location_value,location_len);location[location_len]=0;
            int resolution=web_resolve_url_owned(current,location,&resolved);free(location);
            if(resolution!=1){http_resp_free(&r);return fail(j,resolution<0?"Out of memory resolving redirect target":"Invalid or prohibited redirect target");}
            char *next_origin = NULL,*next_url=NULL;
            parsed = image_http_url(j, resolved, &next_url, &next_origin);free(resolved);
            if (parsed != HTTP_URL_TUPLE) {
                http_resp_free(&r); return url_failure(j, parsed, "Invalid or prohibited redirect target");
            }
            if (!webcookie_same_site(j->cookies,j->final_url,next_url)) j->redirect_cross_site=true;
            if (strcmp(j->target_origin, next_origin)) {
                remove_request_header(j->headers, "Authorization");
                /* Fetch redirect-taint: an origin-changing redirect from a
                   URL already outside the request origin serializes to null. */
                if (!j->origin || strcmp(j->target_origin, j->origin)) j->origin_tainted = true;
            }
            free(j->target_origin); j->target_origin = next_origin;
            free(j->final_url);j->final_url=next_url;
            j->redirected = true;
            /* Fetch (HTTP-redirect fetch): the redirect ends and the next hop starts. */
            j->timing.redirect_count++;
            j->timing.redirect_end = j->timing.post_redirect_start = uptime_ms();
            if ((!strcmp(method, "POST") && (r.status == 301 || r.status == 302)) ||
                (r.status == 303 && strcmp(method, "GET") && strcmp(method, "HEAD"))) {
                drop_body = true;
                remove_body_headers(j->headers);
            }
            http_resp_free(&r); continue;
        }
        /* Fetch: the final response's Server-Timing values, when the request's
           client is a secure context. A browser-UI navigation has no client;
           the document it makes reads them, so its URL decides. */
        const char *client = top_level_navigation(j) && ((j->fetch.flags & WEBNET_FETCH_BROWSER_UI) || !*j->document) ? current : j->document;
        free(j->timing.server_timing); j->timing.server_timing = NULL;
        if (j->wire.kind != WEBNET_REPORT && potentially_trustworthy(client) &&
            !combined_field(j, http_response_headers(&r), "Server-Timing", &j->timing.server_timing)) { http_resp_free(&r); return false; }
        if (j->wire.kind == WEBNET_MODULE && (r.status < 200 || r.status >= 300 || !js_mime(http_response_headers(&r)))) {
            reject_script_response(j, &r, "Module response rejected",
                                   r.status < 200 || r.status >= 300 ? "status" : "MIME");
            http_resp_free(&r); return false;
        }
        if (j->wire.kind == WEBNET_CLASSIC) {
            char nosniff[64];
            const char *headers = http_response_headers(&r);
            bool have_nosniff = field_exists(headers, "X-Content-Type-Options");
            if (r.status < 200 || r.status >= 300 ||
                (have_nosniff && (!field(headers, "X-Content-Type-Options", nosniff, sizeof nosniff) ||
                    (!strcasecmp(nosniff, "nosniff") && !js_mime(headers))))) {
                reject_script_response(j, &r, "Script response is not executable",
                    r.status < 200 || r.status >= 300 ? "status" :
                    !field(headers, "X-Content-Type-Options", nosniff, sizeof nosniff) ? "nosniff-field" : "MIME");
                http_resp_free(&r); return false;
            }
        }
        j->status = r.status;
        if(!hit){
            if(cache_directive(http_response_headers(&r),"no-store",NULL))cache_invalidate(current);
            cache_store(j,&r);
        }
        if(strcmp(method,"GET")&&strcmp(method,"HEAD")&&strcmp(method,"OPTIONS")&&strcmp(method,"TRACE")&&r.status>=200&&r.status<400)
            cache_invalidate(current);
        if (j->wire.user_navigation & WEBNET_WIRE_STREAM) { http_resp_free(&r); return true; }
        bool headers_ok = response_headers(j, &r, j->cors_tainted); http_resp_free(&r);
        if (!headers_ok) return false;
        if (!j->body) { j->body = calloc(1, 1); if (!j->body) return fail(j, "Out of memory"); }
        return true;
    }
    return false;
}
/* ---- WebSocket (RFC 6455) ----
   The opening handshake, framing, masking, fragmentation and control frames
   live here beside TCP/TLS; the browser exchanges whole messages as
   webnet_wire_socket records over the request channel. A reader thread sleeps
   in the transport while this thread waits on the browser channel; both share
   the TLS engine through its lock, which the reader drops while it sleeps. */
struct websocket {
    struct job *j;
    net_stream *stream;
    mtx_t engine; /* zeroed: unlocked */
    volatile uint32_t channel, done, stopping;
    bool close_sent, failed;
    br_hmac_drbg_context mask_random;
    unsigned char *in, *message;
    size_t in_len, in_cap, message_len, message_cap;
    unsigned message_type; /* 0 none, else the fragmented message's record code */
};
static bool ws_grow(unsigned char **p, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t next = *cap ? *cap : 16384;
    while (next < need) next = next > SIZE_MAX / 2 ? need : next * 2;
    unsigned char *grown = realloc(*p, next);
    if (!grown) return false;
    *p = grown; *cap = next; return true;
}
static bool ws_record(struct websocket *ws, uint32_t magic, uint32_t code, const void *data, size_t length) {
    struct webnet_wire_socket record = {magic, code, length};
    while (__atomic_exchange_n(&ws->channel, 1, __ATOMIC_ACQUIRE)) wait_on_address(&ws->channel, 1, UINT_MAX);
    bool ok = write_all(ws->j, &record, sizeof record) && write_all(ws->j, data, length);
    __atomic_store_n(&ws->channel, 0, __ATOMIC_RELEASE); wake_address(&ws->channel, 1);
    return ok;
}
/* Caller holds the engine lock. A full transport releases it while waiting
   so the reader can keep acknowledging received records. */
static bool ws_write_locked(struct websocket *ws, const unsigned char *p, size_t n) {
    while (n) {
        long sent = ns_try_write(ws->stream, p, n);
        if (sent == -2) {
            if (__atomic_load_n(&ws->stopping, __ATOMIC_ACQUIRE)) return false;
            mtx_unlock(&ws->engine); n_sleep_ms(1); mtx_lock(&ws->engine);
            continue;
        }
        if (sent <= 0) return false;
        p += sent; n -= (size_t)sent;
    }
    while (ns_try_write(ws->stream, NULL, 0) == -2) { /* drain buffered records */
        if (__atomic_load_n(&ws->stopping, __ATOMIC_ACQUIRE)) return false;
        mtx_unlock(&ws->engine); n_sleep_ms(1); mtx_lock(&ws->engine);
    }
    return true;
}
/* One unfragmented client frame, masked with a fresh key (RFC 6455 5.3). */
static bool ws_frame_locked(struct websocket *ws, unsigned opcode, const unsigned char *data, size_t n) {
    unsigned char head[14], key[4], chunk[16384];
    size_t h = 0;
    head[h++] = (unsigned char)(0x80 | opcode);
    if (n < 126) head[h++] = (unsigned char)(0x80 | n);
    else if (n <= 0xffff) { head[h++] = 0x80 | 126; head[h++] = (unsigned char)(n >> 8); head[h++] = (unsigned char)n; }
    else { head[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) head[h++] = (unsigned char)((uint64_t)n >> (8 * i)); }
    br_hmac_drbg_generate(&ws->mask_random, key, sizeof key);
    memcpy(head + h, key, 4); h += 4;
    if (!ws_write_locked(ws, head, h)) return false;
    for (size_t at = 0; at < n;) {
        size_t take = MIN(n - at, sizeof chunk);
        for (size_t i = 0; i < take; i++) chunk[i] = data[at + i] ^ key[(at + i) & 3];
        if (!ws_write_locked(ws, chunk, take)) return false;
        at += take;
    }
    return true;
}
static bool ws_fail(struct websocket *ws, const char *why) {
    if (!ws->failed) { ws->failed = true; ws_record(ws, WEBNET_SOCKET_ERROR, 0, why, strlen(why)); }
    return false;
}
/* Deliver records without the engine lock: the browser may be slow to read. */
static bool ws_deliver(struct websocket *ws, uint32_t magic, uint32_t code, const void *data, size_t n) {
    mtx_unlock(&ws->engine);
    bool ok = ws_record(ws, magic, code, data, n);
    mtx_lock(&ws->engine);
    return ok;
}
/* Parse complete server frames from ws->in. 1 continue, 0 closed, -1 failed. */
static int ws_frames(struct websocket *ws) {
    size_t used = 0;
    int result = 1;
    while (result == 1) {
        const unsigned char *p = ws->in + used;
        size_t avail = ws->in_len - used;
        if (avail < 2) break;
        unsigned opcode = p[0] & 15;
        bool fin = (p[0] & 0x80) != 0;
        if (p[0] & 0x70) { result = -1; ws_fail(ws, "WebSocket frame uses an unnegotiated extension"); break; }
        if (p[1] & 0x80) { result = -1; ws_fail(ws, "WebSocket server sent a masked frame"); break; }
        uint64_t length = p[1] & 127;
        size_t pos = 2;
        if (length == 126) { if (avail < 4) break; length = (uint64_t)p[2] << 8 | p[3]; pos = 4; }
        else if (length == 127) {
            if (avail < 10) break;
            length = 0; for (int i = 0; i < 8; i++) length = length << 8 | p[2 + i];
            pos = 10;
            if (length >> 63) { result = -1; ws_fail(ws, "Invalid WebSocket frame length"); break; }
        }
        if (length > SIZE_MAX - pos) { result = -1; ws_fail(ws, "WebSocket frame exceeds addressable memory"); break; }
        if (avail < pos + length) {
            if (!ws_grow(&ws->in, &ws->in_cap, used + pos + (size_t)length)) { result = -1; ws_fail(ws, "Out of memory receiving WebSocket frame"); }
            break;
        }
        const unsigned char *payload = p + pos;
        size_t n = (size_t)length;
        if (opcode >= 8) {
            if (!fin || n > 125) { result = -1; ws_fail(ws, "Invalid WebSocket control frame"); break; }
            if (opcode == 8) {
                if (n == 1) { result = -1; ws_fail(ws, "Invalid WebSocket close frame"); break; }
                unsigned code = n >= 2 ? (unsigned)payload[0] << 8 | payload[1] : 1005;
                if (!ws->close_sent) {
                    ws->close_sent = true;
                    ws_frame_locked(ws, 8, payload, n >= 2 ? 2 : 0);
                }
                ws_deliver(ws, WEBNET_SOCKET_CLOSE, code, n > 2 ? payload + 2 : NULL, n > 2 ? n - 2 : 0);
                result = 0;
            } else if (opcode == 9) {
                if (!ws->close_sent && !ws_frame_locked(ws, 10, payload, n)) { result = -1; ws_fail(ws, "WebSocket pong failed"); }
            } else if (opcode != 10) { result = -1; ws_fail(ws, "Unknown WebSocket control opcode"); }
        } else {
            if (opcode == 1 || opcode == 2) {
                if (ws->message_type) { result = -1; ws_fail(ws, "WebSocket message interleaved with a fragment"); break; }
                ws->message_type = opcode == 1 ? WEBNET_SOCKET_TEXT : WEBNET_SOCKET_BINARY;
                ws->message_len = 0;
            } else if (opcode != 0 || !ws->message_type) { result = -1; ws_fail(ws, "Invalid WebSocket data frame"); break; }
            if (fin && !ws->message_len) {
                /* The whole message is this frame: deliver it in place. */
                unsigned type = ws->message_type; ws->message_type = 0;
                if (!ws_deliver(ws, WEBNET_SOCKET_MESSAGE, type, payload, n)) result = -1;
            } else {
                if (n > SIZE_MAX - ws->message_len || !ws_grow(&ws->message, &ws->message_cap, ws->message_len + n)) {
                    result = -1; ws_fail(ws, "Out of memory receiving WebSocket message"); break;
                }
                memcpy(ws->message + ws->message_len, payload, n); ws->message_len += n;
                if (fin) {
                    unsigned type = ws->message_type; ws->message_type = 0;
                    if (!ws_deliver(ws, WEBNET_SOCKET_MESSAGE, type, ws->message, ws->message_len)) result = -1;
                    ws->message_len = 0;
                    /* A large reassembled message does not stay resident. */
                    if (ws->message_cap > 1u << 20) { free(ws->message); ws->message = NULL; ws->message_cap = 0; }
                }
            }
        }
        used += pos + n;
    }
    if (used) { memmove(ws->in, ws->in + used, ws->in_len - used); ws->in_len -= used; }
    return result;
}
static void ws_reader(void *opaque) {
    struct websocket *ws = opaque;
    mtx_lock(&ws->engine);
    int state = ws->in_len ? ws_frames(ws) : 1; /* bytes that followed the handshake */
    while (state == 1 && !__atomic_load_n(&ws->stopping, __ATOMIC_ACQUIRE)) {
        if (ws->in_cap - ws->in_len < 16384 && !ws_grow(&ws->in, &ws->in_cap, ws->in_len + 16384)) {
            ws_fail(ws, "Out of memory receiving WebSocket data"); break;
        }
        long got = ns_read_wait(ws->stream, ws->in + ws->in_len, ws->in_cap - ws->in_len, 1000, &ws->engine);
        if (got == -2) continue;
        if (got <= 0) {
            if (!__atomic_load_n(&ws->stopping, __ATOMIC_ACQUIRE))
                ws_fail(ws, got == 0 ? "WebSocket connection closed without a closing handshake" : "WebSocket transport failed");
            break;
        }
        ws->in_len += (size_t)got;
        state = ws_frames(ws);
    }
    mtx_unlock(&ws->engine);
    __atomic_store_n(&ws->done, 1, __ATOMIC_RELEASE);
}
/* Opening handshake (RFC 6455 4.1). The browser supplied only a validated
   Sec-WebSocket-Protocol field; Origin and cookies come from trusted state. */
static bool ws_handshake(struct websocket *ws, const char *http_url, const struct url_owned *u) {
    struct job *j = ws->j;
    unsigned char nonce[16];
    br_hmac_drbg_generate(&ws->mask_random, nonce, sizeof nonce);
    size_t key_length = 0;
    char *key = base64_encode(nonce, sizeof nonce, &key_length);
    if (!key) return fail(j, "Out of memory preparing WebSocket key");
    char accept[64] = "";
    {
        static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        br_sha1_context sha; unsigned char digest[20];
        br_sha1_init(&sha); br_sha1_update(&sha, key, key_length); br_sha1_update(&sha, guid, sizeof guid - 1);
        br_sha1_out(&sha, digest);
        size_t al = 0; char *encoded = base64_encode(digest, sizeof digest, &al);
        if (!encoded || al >= sizeof accept) { free(encoded); free(key); return fail(j, "Out of memory preparing WebSocket key"); }
        memcpy(accept, encoded, al + 1); free(encoded);
    }
    char *origin = NULL;
    if (http_origin_owned(j->document, &origin) != HTTP_URL_TUPLE) { free(origin); origin = NULL; }
    /* The handshake is a Fetch with mode websocket: Sec-Fetch-Site relates the
       creating document's origin to the socket's URL. */
    if (origin && !j->origin) { j->origin = strdup(origin); if (!j->origin) { free(origin); free(key); return fail(j, "Out of memory preparing WebSocket origin"); } }
    if (!j->target_origin && http_origin_owned(http_url, &j->target_origin) != HTTP_URL_TUPLE) { free(j->target_origin); j->target_origin = NULL; }
    note_fetch_site(j, http_url, j->target_origin);
    struct header_buffer *b = &j->outgoing; b->length = 0;
    char port[16] = "";
    if (u->port != (u->tls ? 443 : 80)) snprintf(port, sizeof port, ":%u", u->port);
    bool ok = header_text(j, b, "GET ") && header_text(j, b, u->path) && header_text(j, b, " HTTP/1.1\r\nHost: ") &&
        header_text(j, b, u->host) && header_text(j, b, port) &&
        header_text(j, b, "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ") && header_text(j, b, key) &&
        header_text(j, b, "\r\nSec-WebSocket-Version: 13\r\nOrigin: ") && header_text(j, b, origin ? origin : "null") &&
        header_text(j, b, "\r\nUser-Agent: " NOCTURNE_USER_AGENT "\r\nPragma: no-cache\r\nCache-Control: no-cache\r\n") &&
        header_text(j, b, j->headers) && header_text(j, b, "Accept: */*\r\n") && append_accept_language(j) &&
        append_fetch_metadata(j, http_url);
    free(origin); free(key);
    if (ok && j->wire.credentials != WEBNET_CREDENTIALS_OMIT) {
        struct webcookie_context c = {http_url, cookie_site_url(j), "GET", false, true, false};
        time_t now = time(NULL);
        long size = webcookie_get(j->cookies, &c, NULL, 0, now);
        if (size > 0) {
            ok = header_text(j, b, "Cookie: ") && header_reserve(j, b, (size_t)size);
            if (ok) {
                long wrote = webcookie_get(j->cookies, &c, b->p + b->length, (size_t)size + 1, now);
                if (wrote != size) ok = fail(j, "Cannot serialize WebSocket cookies");
                else { b->length += (size_t)wrote; ok = header_text(j, b, "\r\n"); }
            }
        } else if (size < 0) ok = fail(j, "Cannot measure WebSocket cookies");
    }
    if (!ok || !header_text(j, b, "\r\n")) return false;
    if (ns_write(ws->stream, b->p, b->length) < 0) return fail(j, ns_error(ws->stream));
    /* Response head; bytes after it are already frames. */
    size_t end = 0;
    while (!end) {
        if (ws->in_len >= WEBNET_RESPONSE_HEADERS_MAX) return fail(j, "WebSocket handshake response too large");
        if (!ws_grow(&ws->in, &ws->in_cap, ws->in_len + 4096)) return fail(j, "Out of memory reading WebSocket handshake");
        long got = ns_read(ws->stream, ws->in + ws->in_len, ws->in_cap - ws->in_len);
        if (got <= 0) return fail(j, got ? ns_error(ws->stream) : "WebSocket server closed during the handshake");
        size_t from = ws->in_len > 3 ? ws->in_len - 3 : 0;
        ws->in_len += (size_t)got;
        for (size_t i = from; i + 4 <= ws->in_len; i++)
            if (!memcmp(ws->in + i, "\r\n\r\n", 4)) { end = i + 4; break; }
    }
    char *head = malloc(end + 1);
    if (!head) return fail(j, "Out of memory reading WebSocket handshake");
    memcpy(head, ws->in, end); head[end] = 0;
    memmove(ws->in, ws->in + end, ws->in_len - end); ws->in_len -= end;
    int status = 0;
    char *fields = strstr(head, "\r\n");
    if (strncmp(head, "HTTP/1.1 ", 9) || sscanf(head + 9, "%3d", &status) != 1) { free(head); return fail(j, "Invalid WebSocket handshake response"); }
    fields += 2;
    const char *value; size_t length;
    if (status != 101) { snprintf(j->error, sizeof j->error, "WebSocket handshake failed: HTTP %d", status); free(head); return false; }
    if (!field_slice(fields, "Upgrade", &value, &length) || length != 9 || strncasecmp(value, "websocket", 9)) ok = fail(j, "WebSocket handshake missing Upgrade: websocket");
    else if (!field_slice(fields, "Connection", &value, &length) || !list_has_slice(value, value + length, "upgrade", 7, true)) ok = fail(j, "WebSocket handshake missing Connection: Upgrade");
    else if (!field_slice(fields, "Sec-WebSocket-Accept", &value, &length) || length != strlen(accept) || memcmp(value, accept, length)) ok = fail(j, "WebSocket handshake has a wrong Sec-WebSocket-Accept");
    else if (field_exists(fields, "Sec-WebSocket-Extensions")) ok = fail(j, "WebSocket server selected an unrequested extension");
    else if (field_exists(fields, "Sec-WebSocket-Protocol")) {
        const char *offered; size_t offered_length;
        if (!field_slice(fields, "Sec-WebSocket-Protocol", &value, &length) || !length ||
            !field_slice(j->headers, "Sec-WebSocket-Protocol", &offered, &offered_length) ||
            !list_has_slice(offered, offered + offered_length, value, length, false))
            ok = fail(j, "WebSocket server selected an unrequested subprotocol");
    }
    if (ok) {
        /* The field block (status line removed, final empty line kept off). */
        size_t fl = strlen(fields);
        if (fl >= 2) fl -= 2;
        ok = ws_record(ws, WEBNET_SOCKET_OPEN, 101, fields, fl);
        if (!ok) fail(j, "Browser channel closed");
    }
    free(head);
    return ok;
}
static bool run_websocket(struct job *j) {
    struct websocket *ws = calloc(1, sizeof *ws);
    if (!ws) return fail(j, "Out of memory opening WebSocket");
    ws->j = j;
    char *http_url = NULL;
    bool secure = !strncasecmp(j->url, "wss://", 6);
    if (secure || !strncasecmp(j->url, "ws://", 5)) {
        size_t rest = strlen(j->url) - (secure ? 6 : 5);
        http_url = malloc(rest + 9);
        if (http_url) snprintf(http_url, rest + 9, "%s://%s", secure ? "https" : "http", j->url + (secure ? 6 : 5));
    }
    struct url_owned u = {0};
    unsigned char seed[32];
    bool seeded = n_random(seed, sizeof seed) == (n_result)sizeof seed;
    if (!seeded) fail(j, "No entropy for WebSocket masking");
    else if (!http_url) fail(j, "Invalid WebSocket URL");
    else if (!url_parse_owned(http_url, &u)) fail(j, "Invalid WebSocket URL");
    else if (*j->headers && (strncasecmp(j->headers, "Sec-WebSocket-Protocol: ", 24) || strstr(j->headers, "\r\n") != j->headers + strlen(j->headers) - 2))
        fail(j, "Invalid WebSocket request fields");
    else {
        br_hmac_drbg_init(&ws->mask_random, &br_sha256_vtable, seed, sizeof seed);
        char err[160] = "";
        ws->stream = ns_open(u.host, u.port, u.tls, remaining(j), err, sizeof err);
        if (!ws->stream) fail(j, err[0] ? err : "WebSocket connection failed");
    }
    memset(seed, 0, sizeof seed);
    if (ws->stream) {
        ns_set_cancel(ws->stream, request_cancelled, j);
        ns_set_timeout(ws->stream, remaining(j));
        if (ws_handshake(ws, http_url, &u)) {
            ns_set_cancel(ws->stream, NULL, NULL);
            int reader = thread_create(ws_reader, ws);
            if (reader < 0) ws_fail(ws, "Cannot start WebSocket reader");
            /* Browser messages and close requests until the reader ends. */
            while (reader >= 0 && !__atomic_load_n(&ws->done, __ATOMIC_ACQUIRE)) {
                int ready = input_wait(j, 100);
                if (ready < 0) break;
                if (!ready) continue;
                struct webnet_wire_socket record;
                if (!read_all(j, &record, sizeof record) || record.length > SIZE_MAX - 2 ||
                    (record.magic != WEBNET_SOCKET_MESSAGE && record.magic != WEBNET_SOCKET_CLOSE)) break;
                size_t n = (size_t)record.length;
                unsigned char *data = malloc(n + 2);
                if (!data || !read_all(j, data + 2, n)) { free(data); break; }
                mtx_lock(&ws->engine);
                bool sent = true;
                if (record.magic == WEBNET_SOCKET_MESSAGE) {
                    if (!ws->close_sent) sent = ws_frame_locked(ws, record.code == WEBNET_SOCKET_TEXT ? 1 : 2, data + 2, n);
                } else if (!ws->close_sent) {
                    ws->close_sent = true;
                    data[0] = (unsigned char)(record.code >> 8); data[1] = (unsigned char)record.code;
                    sent = record.code ? ws_frame_locked(ws, 8, data, n + 2) : ws_frame_locked(ws, 8, NULL, 0);
                }
                mtx_unlock(&ws->engine);
                free(data);
                if (!sent) { ws_fail(ws, "WebSocket send failed"); break; }
            }
            __atomic_store_n(&ws->stopping, 1, __ATOMIC_RELEASE);
            if (reader >= 0) {
                if (!__atomic_load_n(&ws->done, __ATOMIC_ACQUIRE)) ns_reset(ws->stream);
                thread_join(reader);
            }
        }
        ns_abort(ws->stream);
    }
    if (j->error[0] && !ws->failed) ws_fail(ws, j->error);
    url_owned_free(&u);
    free(http_url); free(ws->in); free(ws->message); free(ws);
    return !j->error[0];
}
static void job_free_io(struct job *j) {
    if(j->events>=0)n_handle_close(j->events);
    webnet_wire_in_free(&j->in);
}
static int run_one(n_handle input,n_handle output) {
    struct job *j = calloc(1, sizeof *j);
    if (!j) return 1;
    j->input=input;j->output=output;j->output_credit=WEBNET_SERVICE_WINDOW;
    j->events=n_events_create();
    if(j->events<0||n_events_watch(j->events,input,N_EVENT_RECEIVABLE|N_EVENT_CLOSED,0)<0){job_free_io(j);free(j);return 1;}
    struct webnet_wire_request *w = &j->wire;
    size_t payload_size;
    if (!read_all(j, w, sizeof *w) || w->magic != WEBNET_MAGIC || w->kind > WEBNET_WEBSOCKET ||
        !w->url_len || w->url_len > webnet_wire_url_limit(w->user_navigation) || w->origin_len > webnet_wire_url_limit(w->user_navigation) ||
        !w->method_len || w->headers_len > webnet_wire_request_header_limit(w->user_navigation) || w->body_len > WEBNET_BODY_LIMIT ||
        w->credentials>WEBNET_CREDENTIALS_INCLUDE || (w->user_navigation & ~WEBNET_WIRE_REQUEST_FLAGS) || w->cookie_len>WEBCOOKIE_SNAPSHOT_MAX ||
        !webnet_wire_script_flag_valid(w->kind, w->user_navigation) || !webnet_wire_image_flag_valid(w->kind, w->user_navigation) || !webnet_wire_report_flag_valid(w->kind,w->user_navigation,w->credentials) || !webnet_wire_redirect_flag_valid(w->kind,w->user_navigation) || !webnet_wire_request_payload_size(w, &payload_size) ||
        (w->credentials==WEBNET_CREDENTIALS_OMIT && w->cookie_len) ||
        WEBNET_WIRE_CACHE_MODE(w->user_navigation) > WEBNET_CACHE_ONLY_IF_CACHED ||
        (w->kind != WEBNET_FETCH && (WEBNET_WIRE_CACHE_MODE(w->user_navigation) != WEBNET_CACHE_DEFAULT || (w->user_navigation & (WEBNET_WIRE_NO_CORS | WEBNET_WIRE_NO_REFERRER)))) ||
        ((w->user_navigation & WEBNET_WIRE_NO_CORS) && (w->user_navigation & (WEBNET_WIRE_SAME_ORIGIN | WEBNET_WIRE_FORCE_PREFLIGHT))) ||
        ((w->user_navigation & WEBNET_WIRE_DEBUG_REQUEST) &&
         ((w->kind != WEBNET_RESOURCE && w->kind != WEBNET_CLASSIC && w->kind != WEBNET_MODULE &&
           w->kind != WEBNET_NAVIGATION) || !(w->user_navigation & WEBNET_WIRE_STATUS_LINE))) ||
        ((w->user_navigation & WEBNET_WIRE_STREAM) && w->kind != WEBNET_FETCH) ||
        ((w->user_navigation & WEBNET_WIRE_REFERRER) && (w->kind == WEBNET_REPORT || w->kind == WEBNET_WEBSOCKET)) ||
        ((input==output)!=((w->user_navigation&WEBNET_WIRE_SERVICE)!=0))) { job_free_io(j); free(j); return 1; }
    bool socket = w->kind == WEBNET_WEBSOCKET, socket_ran = false;
    struct webnet_wire_referrer ref={0};
    struct webnet_wire_top_level top={0};
    uint64_t extended_size=payload_size;
    if(w->user_navigation&WEBNET_WIRE_REFERRER){
        if(!job_read_all(j,&ref,sizeof ref)||ref.policy>WEBNET_REFERRER_UNSAFE||ref.length>webnet_wire_url_limit(w->user_navigation)||
           (uint64_t)payload_size+ref.length+sizeof ref>SIZE_MAX-7){job_free_io(j);free(j);return 1;}
        j->referrer_policy=(int)ref.policy;
        extended_size+=(uint64_t)ref.length+sizeof ref;
    }
    if(w->user_navigation&WEBNET_WIRE_TOP_LEVEL){
        if(!job_read_all(j,&top,sizeof top)||top.reserved||top.length>webnet_wire_url_limit(w->user_navigation)){
            job_free_io(j);free(j);return 1;
        }
        extended_size+=(uint64_t)top.length+sizeof top;
    }
    if(w->user_navigation&WEBNET_WIRE_FETCH_METADATA){
        struct webnet_wire_fetch *f=&j->fetch;
        if(!job_read_all(j,f,sizeof *f)||f->destination>=WEBNET_DEST_COUNT||
           (f->flags&~(WEBNET_FETCH_USER_ACTIVATION|WEBNET_FETCH_BROWSER_UI))||
           (f->flags&&w->kind!=WEBNET_NAVIGATION)||((f->flags&WEBNET_FETCH_BROWSER_UI)&&!(w->user_navigation&WEBNET_WIRE_USER_NAVIGATION))||
           (socket&&f->destination!=WEBNET_DEST_EMPTY)){
            job_free_io(j);free(j);return 1;
        }
        extended_size+=sizeof *f;
    }
    struct webnet_wire_language lang={0};
    if(w->user_navigation&WEBNET_WIRE_ACCEPT_LANGUAGE){
        if(!job_read_all(j,&lang,sizeof lang)||lang.reserved||!lang.length||lang.length>WEBNET_WIRE_ACCEPT_LANGUAGE_MAX){
            job_free_io(j);free(j);return 1;
        }
        extended_size+=(uint64_t)lang.length+sizeof lang;
    }
    if(extended_size>SIZE_MAX-8){job_free_io(j);free(j);return 1;}
    j->url = read_string(j,w->url_len, false); j->document = read_string(j,w->origin_len, false);
    if(w->user_navigation&WEBNET_WIRE_REFERRER)j->referrer=read_string(j,ref.length,false);
    if(w->user_navigation&WEBNET_WIRE_TOP_LEVEL)j->top_level_url=read_string(j,top.length,false);
    /* The value goes into a request's fields as it is: only bytes a CORS-safelisted
       Accept-Language may hold, so it can never end the field or add another. */
    bool language_valid=true;
    if(w->user_navigation&WEBNET_WIRE_ACCEPT_LANGUAGE){
        language_valid=job_read_all(j,j->accept_language,lang.length)&&
            webnet_wire_language_valid(j->accept_language,lang.length);
        j->accept_language[language_valid?lang.length:0]=0;
    }
    j->method = read_string(j,w->method_len, false); j->headers = read_string(j,w->headers_len, false);
    j->request_body = read_string(j,w->body_len, true);
    /* Private transport diagnostics contain no cookie fields or metadata. */
    const char *cookie_snapshot_error=NULL;
    char *cookie_snapshot=malloc((size_t)w->cookie_len+1);
    if(!cookie_snapshot)cookie_snapshot_error="Cookie snapshot allocation failed";
    else if(!job_read_all(j,cookie_snapshot,w->cookie_len))cookie_snapshot_error="Cookie snapshot read failed";
    else cookie_snapshot[w->cookie_len]=0;
    j->cookies=webcookie_create();
    const char *cookie_psl_error="PSL open failed";
    /* The user's exceptions hold on every hop, a redirect's to another host too: no cookie set by
       or sent to a blocked host (RFC 6265bis §7.2). The service watches the preferences (main). */
    if(j->cookies)webcookie_set_rule(j->cookies,webprefs_cookie_rule,NULL);
    if(j->cookies){
        FILE *f=fopen("C:\\Nocturne\\Web\\public_suffix_list.dat","r");
        if(f){
            char *text=malloc(WEBCOOKIE_PSL_MAX+1);
            if(!text)cookie_psl_error="PSL buffer allocation failed";
            else{
                size_t n=fread(text,1,WEBCOOKIE_PSL_MAX+1,f);
                if(ferror(f))cookie_psl_error="PSL read failed";
                else if(n>WEBCOOKIE_PSL_MAX)cookie_psl_error="PSL size limit exceeded";
                else if(!webcookie_psl_load(j->cookies,text,n))cookie_psl_error="PSL parse or policy allocation failed";
                else cookie_psl_error=NULL;
                free(text);
            }
            fclose(f);
        }
    }
    if(!j->cookies)fail(j,"Cookie jar allocation failed");
    else if(cookie_snapshot_error)fail(j,cookie_snapshot_error);
    else if(w->cookie_len&&!webcookie_import(j->cookies,cookie_snapshot,w->cookie_len,time(NULL))){
        /* import's bool result deliberately does not claim parse rather than OOM. */
        snprintf(j->error,sizeof j->error,"Cookie snapshot rejected or allocation failed (%s)",cookie_psl_error?cookie_psl_error:"PSL ready");
    }
    free(cookie_snapshot);
    if (!j->url || !j->document || !j->method || !j->headers || !j->request_body ||
        ((w->user_navigation&WEBNET_WIRE_REFERRER)&&!j->referrer)||
        ((w->user_navigation&WEBNET_WIRE_TOP_LEVEL)&&!j->top_level_url)) fail(j, "Invalid request or out of memory");
    else if(!language_valid)fail(j,"Invalid Accept-Language in request");
    else if(w->kind==WEBNET_REPORT && (!w->body_len || !webnet_report_fields_valid(j->method,j->headers)))fail(j,"Invalid native CSP report request");
    else if (!webnet_method_valid(j->method) || ((!strcmp(j->method, "GET") || !strcmp(j->method, "HEAD")) && w->body_len)) fail(j, "Invalid HTTP method or body");
    else if (socket && (strcmp(j->method, "GET") || w->body_len || input != output)) fail(j, "Invalid WebSocket request");
    else if (socket) { if (!j->error[0] && remaining(j)) { socket_ran = true; run_websocket(j); } }
    else if (!j->error[0] && remaining(j)) {
        if (!strncmp(j->url, "file:", 5)) load_file(j);
        else run_http(j);
    }
    if (j->error[0]) {
        free(j->body); j->body = NULL; j->body_len = 0; j->status = 0;
        free(j->response_headers); j->response_headers = NULL; j->response_headers_len = 0;
    }
    if((w->user_navigation&WEBNET_WIRE_DEBUG_REQUEST)&&j->debug_attempts) {
        char line[384];
        int length=snprintf(line,sizeof line,
            "\r\nHTTP/Nocturne-Transport phase=%u attempts=%u resolve=%lu connect=%lu tls=%lu first_header=%lu headers=%lu body=%lu tls_io=%lu tls_other=%lu h2=%u stream=%lu reused=%u connection=%lu\r\n",
            (unsigned)j->debug_timing.phase,j->debug_attempts,(unsigned long)j->debug_timing.resolve_ms,
            (unsigned long)j->debug_timing.connect_ms,(unsigned long)j->debug_timing.tls_ms,
            (unsigned long)j->debug_timing.first_header_ms,(unsigned long)j->debug_timing.header_ms,
            (unsigned long)j->debug_timing.body_ms,(unsigned long)j->debug_timing.tls_io_ms,
            (unsigned long)j->debug_timing.tls_other_ms,(unsigned)j->debug_transport.http2,
            (unsigned long)j->debug_transport.stream_id,(unsigned)j->debug_transport.reused,
            (unsigned long)j->debug_transport.connection_id);
        size_t old=j->response_headers_len;
        if(length>0&&(size_t)length<sizeof line&&old<=SIZE_MAX-(size_t)length-1) {
            char *grown=realloc(j->response_headers,old+(size_t)length+1);
            if(grown){j->response_headers=grown;memcpy(grown+old,line,(size_t)length+1);
                j->response_headers_len=old+(size_t)length;}
        }
    }
    /* Fetch's timing record goes after every server field: appended to a
       buffered response's block, or as the whole block of a stream's end. */
    char *timing = NULL; size_t timing_length = 0;
    bool streaming_end = (w->user_navigation & WEBNET_WIRE_STREAM) != 0;
    if (!socket && (w->user_navigation & WEBNET_WIRE_TIMING) && w->kind != WEBNET_REPORT) {
        timing_finish(j);
        timing = timing_lines(j, &timing_length);
        if (!timing) { if (!j->error[0]) fail(j, "Out of memory reporting fetch timing"); }
        else if (!streaming_end) {
            size_t old = j->response_headers_len;
            char *grown = old <= SIZE_MAX - timing_length - 1 ? realloc(j->response_headers, old + timing_length + 1) : NULL;
            if (grown) { memcpy(grown + old, timing, timing_length + 1); j->response_headers = grown; j->response_headers_len = old + timing_length; }
            else if (!j->error[0]) fail(j, "Out of memory reporting fetch timing");
            free(timing); timing = NULL; timing_length = 0;
        }
    }
    bool ok;
    if (socket) {
        /* A socket channel carries socket records only; run_websocket
           already reported its own failure or closure. */
        ok = socket_ran && !j->error[0];
        if (!socket_ran) {
            struct webnet_wire_socket record = {WEBNET_SOCKET_ERROR, 0, strlen(j->error)};
            if (write_all(j, &record, sizeof record)) write_all(j, j->error, record.length);
        }
    } else {
    struct webnet_wire_response out = {0};
    bool streaming = (w->user_navigation & WEBNET_WIRE_STREAM) != 0;
    out.magic = streaming ? WEBNET_STREAM_END : WEBNET_MAGIC;
    out.id = w->id; out.generation = w->generation; out.status = streaming ? 0 : j->status;
    const char *final_url=j->final_url?j->final_url:"";
    out.url_len = streaming ? 0 : strlen(final_url); out.headers_len = streaming ? timing_length : j->response_headers_len;
    out.body_len = streaming ? 0 : j->body_len; out.error_len = strlen(j->error);
    out.cookie_len=(uint32_t)j->cookie_len;
    ok = job_write(j, &out, sizeof out) && job_write(j, final_url, out.url_len) &&
              job_write(j, streaming ? timing : j->response_headers, out.headers_len) && job_write(j, j->body, out.body_len) && job_write(j, j->error, out.error_len) &&
              job_write(j,j->cookie_events,out.cookie_len) && backlog_drain(j,0);
    }
    free(timing); http_fetch_timing_free(&j->timing);
    for (size_t i = 0; i < j->redirect_tao_count; i++) free(j->redirect_tao[i]);
    free(j->redirect_tao);
    free(j->backlog);
    free(j->origin); free(j->target_origin);free(j->final_url);free(j->referrer);free(j->referrer_origin);
    free(j->top_level_url);free(j->top_origin);
    free(j->url); free(j->document); free(j->method); free(j->headers); free(j->request_body); free(j->body); free(j->response_headers); free(j->cookie_events); free(j->non_simple); free(j->outgoing.p); free(j->sent.p);
    free(j->partition);free(j->cache_body.p);cache_free(j->cached);webcookie_free(j->cookies); job_free_io(j); free(j);
    return ok ? 0 : 1;
}
/* Each duplex request channel has its own lifetime and byte-credit window.
   Pausing one stream therefore cannot block another request's completion. */
/* One thread per open channel; long-lived WebSockets never take the place
   of fetches, so channels are a list, not a fixed table. */
struct service_slot { struct service_slot *next;int tid;n_handle channel;volatile uint32_t done; };
static void service_request(void *opaque) {
    struct service_slot *s=opaque;
    /* Setup waits own a thread-local reference to this request channel. Never
       keep it across slot reuse, including a failed initial registration. */
    if(net_set_setup_cancel_channel(s->channel)==0)run_one(s->channel,s->channel);
    net_set_setup_cancel_channel(-1);
    n_handle_close(s->channel);
    __atomic_store_n(&s->done,1,__ATOMIC_RELEASE);wake_address(&s->done,0);
}
int main(int argc,char **argv) {
    if(argc!=2||strcmp(argv[1],"--service"))return run_one(0,1);
    /* Handle 0 is the browser's listener. The service ends with the browser. */
    n_handle events=n_events_create();
    if(events<0||n_events_watch(events,0,N_EVENT_RECEIVABLE,0)<0)return 1;
    n_handle parent=n_app_open(n_app_parent());
    if(parent>=0&&n_events_watch(events,parent,N_EVENT_APP_ENDED,1)<0)return 1;
    struct service_slot *slots=NULL;
    /* The browser's preferences (the user's cookie exceptions), read again when they change. */
    (void)webprefs_watch(events);
    for(bool running=true;running;){
        for(struct service_slot **at=&slots;*at;){
            struct service_slot *s=*at;
            if(__atomic_load_n(&s->done,__ATOMIC_ACQUIRE)){thread_join(s->tid);*at=s->next;free(s);}
            else at=&s->next;
        }
        /* The timeout only reclaims finished request threads. */
        struct n_event reports[3];
        n_result got=n_events_wait(events,reports,3,100);
        if(got<0)break;
        for(n_result i=0;i<got;i++)if(reports[i].token==1)running=false;
        webprefs_events(reports,(size_t)got);
        while(running){
            n_handle channel=n_channel_accept(0);
            if(channel==-N_ERR_NOT_READY)break;
            if(channel<0){running=false;break;}
            struct service_slot *slot=calloc(1,sizeof *slot);
            if(!slot){n_handle_close(channel);continue;}
            slot->channel=channel;
            slot->tid=thread_create(service_request,slot);
            if(slot->tid<0){n_handle_close(channel);free(slot);continue;}
            slot->next=slots;slots=slot;
        }
    }
    __atomic_store_n(&service_stopping,1,__ATOMIC_RELEASE);
    while(slots){struct service_slot *s=slots;slots=s->next;thread_join(s->tid);free(s);}
    return 0;
}
