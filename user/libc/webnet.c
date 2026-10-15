#include "webnet.h"
#include "webnet_wire.h"
#include "http.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

/* Request concurrency is separate from pooled HTTP connection concurrency. */
#define WORKERS 32
#define ORIGIN_WORKERS 6
#define PUMP_BYTES (32u * 1024u)
#define PUMP_TOTAL_BYTES (1024u * 1024u)
#define PUMP_TIME_MS 3u
struct request {
    struct request *next;
    uint64_t id, generation, deadline;
    int priority;
    webnet_callback callback;
    void *opaque;
    unsigned char *tx, *rx;
    size_t tx_len, tx_pos, rx_len, rx_pos;
    size_t response_limit;
    size_t credit_due, credit_pos, tx_credit;
    struct webnet_wire_credit credit;
    struct webnet_wire_response head;
    size_t head_pos;
    bool cancelled, finishing, in_callback;
    bool streaming, paused, headers_seen, debug_request, debug_reported, frame_ready, credit_closed;
    char *cookie_site, *cookie_origin;
    char *transport_origin;
    uint64_t queued_at,started_at;
    /* Debug stream accounting: delivered frames/bytes and time spent paused
       waiting for the consumer, separating consumer from transport stalls. */
    uint64_t stream_frames,stream_bytes,paused_at,paused_ms;
    /* Consecutive body chunks already received from the worker, delivered to
       the consumer as one callback instead of one per worker read. */
    unsigned char *merge;
    size_t merge_len,merge_cap;
    enum webnet_credentials credentials;
    bool top_level;
    bool keepalive;
    bool image_upgrade;
    uint64_t fetch_group;
    size_t keepalive_bytes;
    char error[160];
};
struct slot { struct request *rq; int pid, in, out; struct slot *next; bool shared; };
struct webnet {
    struct request *queue;
    struct slot slots[WORKERS];
    struct slot *parked;
    uint64_t serial;
    unsigned count;
    unsigned next_priority;
    unsigned worker_limit;
    unsigned pump_cursor;
    uint64_t budget_at;
    int service_pid, service_listener;
    char service_key[96];
    bool debug, pumping, parked_first;
    size_t queued_bytes;
    webcookie_jar *cookies;
};

static void request_free(struct request *r) { if(!r)return;free(r->tx); free(r->rx); free(r->merge);free(r->cookie_site);free(r->cookie_origin);free(r->transport_origin); free(r); }
static unsigned worker_budget(void){
    struct n_cpuinfo cpu={0};struct n_sysinfo memory={0};
    unsigned online=cpu_info(&cpu)>=0&&cpu.online_cpus?cpu.online_cpus:1;
    unsigned budget=online>WORKERS/4?WORKERS:MAX(8u,online*4u);
    /* Keep half the currently free pages for DOM/media and decoder growth.
       32 MiB headroom per active TLS/helper is a scheduling reserve, not a
       response-size quota. Existing requests drain even when budget shrinks. */
    if(sysinfo(&memory)>=0){uint64_t affordable=memory.free_mem/(64u*1024u*1024u);budget=MIN(budget,MAX(affordable,1u));}
    return MIN(budget,WORKERS);
}
static void load_cookie_psl(webcookie_jar *jar) {
    FILE *f = fopen("/usr/share/browser/public_suffix_list.dat", "r");
    if (!f) return; /* Jar falls back to host-only, not a guessed suffix table. */
    char *text = malloc(WEBCOOKIE_PSL_MAX + 1);
    if (text) { size_t n = fread(text, 1, WEBCOOKIE_PSL_MAX + 1, f); if (n <= WEBCOOKIE_PSL_MAX) webcookie_psl_load(jar, text, n); free(text); }
    fclose(f);
}
static void slot_close(struct slot *s) {
    if (s->in >= 0) close(s->in);
    if (s->out >= 0 && s->out != s->in) close(s->out);
    s->in = s->out = -1;
}
static bool slot_reap(struct slot *s) {
    if (s->shared) { s->pid=0; return true; }
    if (s->pid <= 0) return true;
    int pid = s->pid;
    int got = waitpid(pid, NULL, WNOHANG);
    /* A transient wait error is not proof that this owned child exited. */
    if (got == pid || (got < 0 && errno == ECHILD)) s->pid = 0;
    return s->pid <= 0;
}
static void slot_stop(struct slot *s, const char *error) {
    if (s->rq->finishing) return;
    if (error) snprintf(s->rq->error, sizeof s->rq->error, "%s", error);
    s->rq->finishing = true;
    slot_close(s);
    if (s->pid > 0 && !s->shared) kill(s->pid);
}
static void reap_paused_stream(struct slot *s) {
    const struct request *r = s->rq;
    if (!r || !r->streaming || !r->paused || r->finishing || s->pid <= 0) return;
    struct n_pollfd peer = {s->out, 0, 0};
    if (poll(&peer, 1, 0) < 0) slot_stop(s, "Worker poll failed");
    else if (peer.revents & N_POLLHUP) slot_reap(s);
    /* Reaping releases only the live transport. The dynamic pipe, request and
       partially received frame remain owned until pull/resume or cancellation;
       do not consume bytes, deliver a callback or manufacture END here. */
}
webnet *webnet_create(void) {
    webnet *n = calloc(1, sizeof *n);
    if (n) {
        n->service_listener=-1;
        n->cookies = webcookie_create();
        if (!n->cookies) { free(n); return NULL; }
        load_cookie_psl(n->cookies);
        n->worker_limit=worker_budget();
        for (int i = 0; i < WORKERS; i++) n->slots[i].in = n->slots[i].out = -1;
    }
    return n;
}
void webnet_debug(webnet *n, bool enabled) { if (n) n->debug = enabled; }
void webnet_free(webnet *n) {
    if (!n) return;
    for (int i = 0; i < WORKERS; i++) {
        struct slot *s = &n->slots[i];
        if (!s->rq) continue;
        slot_close(s);
        if (s->pid > 0 && !s->shared) { kill(s->pid); waitpid(s->pid, NULL, 0); }
        request_free(s->rq);
    }
    while(n->parked){struct slot *s=n->parked;n->parked=s->next;slot_close(s);if(s->pid>0&&!s->shared){kill(s->pid);waitpid(s->pid,NULL,0);}request_free(s->rq);free(s);}
    if(n->service_pid>0){kill(n->service_pid);waitpid(n->service_pid,NULL,0);}
    if(n->service_listener>=0)close(n->service_listener);
    while (n->queue) { struct request *r = n->queue; n->queue = r->next; request_free(r); }
    webcookie_free(n->cookies); free(n);
}
long webnet_cookie_get_site(webnet *n,const char *url,const char *top_level_url,char *out,size_t cap) {
    struct webcookie_context c = {url, top_level_url?top_level_url:url, "GET", false, false, false};
    return n ? webcookie_get(n->cookies, &c, out, cap, time(NULL)) : -1;
}
int webnet_cookie_set_site(webnet *n,const char *url,const char *top_level_url,const char *value) {
    struct webcookie_context c = {url, top_level_url?top_level_url:url, "GET", false, false, false};
    return n && value ? webcookie_set(n->cookies, &c, value, strlen(value), time(NULL)) : -1;
}
long webnet_cookie_get(webnet *n,const char *url,char *out,size_t cap){return webnet_cookie_get_site(n,url,NULL,out,cap);}
int webnet_cookie_set(webnet *n,const char *url,const char *value){return webnet_cookie_set_site(n,url,NULL,value);}
uint64_t webnet_submit(webnet *n, const struct webnet_request *q, webnet_callback cb, void *opaque) {
    if (!n || !q || !q->url || !cb || n->count == UINT_MAX || q->kind > WEBNET_REPORT ||
        q->kind < WEBNET_NAVIGATION || q->body_len > WEBNET_BODY_LIMIT || (q->body_len && !q->body) ||
        q->credentials < WEBNET_CREDENTIALS_OMIT || q->credentials > WEBNET_CREDENTIALS_INCLUDE ||
        q->cache_mode < WEBNET_CACHE_DEFAULT || q->cache_mode > WEBNET_CACHE_ONLY_IF_CACHED ||
        (q->kind != WEBNET_FETCH && q->cache_mode != WEBNET_CACHE_DEFAULT) ||
        q->referrer_policy<WEBNET_REFERRER_DEFAULT || q->referrer_policy>WEBNET_REFERRER_UNSAFE ||
        ((q->referrer||q->referrer_policy) && q->kind!=WEBNET_FETCH) ||
        ((q->no_cors || q->no_referrer) && q->kind != WEBNET_FETCH) ||
        (q->no_cors && (q->same_origin || q->force_preflight)) ||
        (q->keepalive && (q->kind != WEBNET_FETCH || !q->fetch_group)) ||
        (q->image_upgrade && q->kind != WEBNET_RESOURCE) ||
        (q->stream_response && q->kind != WEBNET_FETCH)) return 0;
    if(q->kind==WEBNET_REPORT && (!webnet_report_fields_valid(q->method,q->headers) || !q->origin ||
        !q->body_len || q->credentials!=WEBNET_CREDENTIALS_SAME_ORIGIN || !q->redirect_error ||
        q->user_navigation || q->force_preflight || q->same_origin || q->no_cors || q->no_referrer ||
        q->keepalive || q->image_upgrade || q->cache_mode!=WEBNET_CACHE_DEFAULT ||
        (strncasecmp(q->url,"http://",7)&&strncasecmp(q->url,"https://",8))))return 0;
    /* Fetch Standard's inflight keepalive body bound, per fetch group. Queued
       and active requests both count; explicit aborted requests are done. */
    if (q->keepalive) {
        size_t bytes = q->body_len;
        if (bytes > 65536u) return 0;
        for (const struct request *p = n->queue; p; p = p->next)
            if (p->keepalive && !p->cancelled && p->fetch_group == q->fetch_group) {
                if (p->keepalive_bytes > 65536u - bytes) return 0;
                bytes += p->keepalive_bytes;
            }
        for (int i = 0; i < WORKERS; i++) {
            const struct request *p = n->slots[i].rq;
            if (p && p->keepalive && !p->cancelled && p->fetch_group == q->fetch_group) {
                if (p->keepalive_bytes > 65536u - bytes) return 0;
                bytes += p->keepalive_bytes;
            }
        }
        for(const struct slot *s=n->parked;s;s=s->next){const struct request *p=s->rq;if(p&&p->keepalive&&!p->cancelled&&p->fetch_group==q->fetch_group){if(p->keepalive_bytes>65536u-bytes)return 0;bytes+=p->keepalive_bytes;}}
    }
    const char *origin = q->origin ? q->origin : "";
    const char *method = q->method ? q->method : "GET";
    const char *headers = q->headers ? q->headers : "";
    bool top_extension=q->top_level_url!=NULL;
    const char *top_level_url=q->top_level_url?q->top_level_url:origin;
    bool referrer_extension=q->referrer!=NULL||q->referrer_policy!=WEBNET_REFERRER_DEFAULT;
    const char *referrer=q->referrer?q->referrer:"";
    size_t ul = strlen(q->url), ol = strlen(origin), ml = strlen(method), hl = strlen(headers),rl=strlen(referrer),tl=strlen(top_level_url);
    if (!ul || ul > UINT32_MAX || ol > UINT32_MAX || ul == SIZE_MAX || ol == SIZE_MAX || ml > UINT32_MAX || !webnet_method_valid(method) || hl > UINT32_MAX ||rl>UINT32_MAX||tl>UINT32_MAX||
        ((!strcmp(method, "GET") || !strcmp(method, "HEAD")) && q->body_len)) return 0;
    int64_t cookie_now=time(NULL);
    long cookie_len = q->credentials == WEBNET_CREDENTIALS_OMIT ? 0 : webcookie_export(n->cookies, NULL, 0, cookie_now);
    if (cookie_len < 0) return 0;
    if ((uint64_t)cookie_len > UINT32_MAX) return 0;
    uint64_t total = sizeof(struct webnet_wire_request) + (uint64_t)ul + ol + ml + hl + q->body_len + (uint64_t)cookie_len+
        (referrer_extension?sizeof(struct webnet_wire_referrer)+(uint64_t)rl:0)+
        (top_extension?sizeof(struct webnet_wire_top_level)+(uint64_t)tl:0);
    if (total > SIZE_MAX || total > SIZE_MAX - n->queued_bytes) return 0;
    size_t len = (size_t)total;
    struct request *r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->tx = malloc(len);
    if (!r->tx) { free(r); return 0; }
    r->cookie_site = strdup(top_level_url);r->cookie_origin=strdup(origin);
    if (!r->cookie_site||!r->cookie_origin) { request_free(r); return 0; }
    struct url_owned destination={0};
    if(url_parse_owned(q->url,&destination)){
        size_t host_length=strlen(destination.host);
        if(host_length>SIZE_MAX-32){url_owned_free(&destination);request_free(r);return 0;}
        r->transport_origin=malloc(host_length+32);
        if(r->transport_origin)snprintf(r->transport_origin,host_length+32,"%u:%u:%s",destination.tls,destination.port,destination.host);
        if(r->transport_origin)for(char *c=r->transport_origin;*c;c++)if(*c>='A'&&*c<='Z')*c+=32;
        url_owned_free(&destination);
        if(!r->transport_origin){request_free(r);return 0;}
    }
    /* Origin bookkeeping is not admission policy. Native local navigation
       and malformed URLs still reach the worker's existing intent/error
       checks and deliver their ordinary asynchronous completion. */
    r->queued_at=uptime_ms();
    r->credentials = q->credentials; r->top_level = q->kind == WEBNET_NAVIGATION && q->user_navigation;
    r->id = ++n->serial;
    if (!r->id) r->id = ++n->serial;
    r->generation = q->generation;
    r->keepalive = q->keepalive; r->fetch_group = q->fetch_group;
    r->image_upgrade = q->image_upgrade;
    r->streaming = q->stream_response;
    r->debug_request = n->debug && (q->kind==WEBNET_RESOURCE||q->kind==WEBNET_CLASSIC||
        q->kind==WEBNET_MODULE||q->kind==WEBNET_NAVIGATION);
    r->keepalive_bytes = q->keepalive ? q->body_len : 0;
    r->response_limit = webnet_response_limit(q->kind);
    r->deadline = uptime_ms() + WEBNET_TIMEOUT_MS;
    r->callback = cb; r->opaque = opaque; r->tx_len = len;
    r->tx_credit=WEBNET_SERVICE_WINDOW;
    r->priority = q->kind == WEBNET_NAVIGATION ? 0 :
                  (q->kind == WEBNET_CLASSIC || q->kind == WEBNET_MODULE) ? 1 :
                  (q->kind == WEBNET_FETCH || q->kind == WEBNET_REPORT) ? 3 : 2;
    struct webnet_wire_request h = {0};
    h.magic = WEBNET_MAGIC; h.kind = q->kind;
    h.user_navigation = (q->user_navigation ? WEBNET_WIRE_USER_NAVIGATION : 0) |
                        (q->force_preflight ? WEBNET_WIRE_FORCE_PREFLIGHT : 0) |
                        (q->redirect_error ? WEBNET_WIRE_REDIRECT_ERROR : 0) |
                        (q->same_origin ? WEBNET_WIRE_SAME_ORIGIN : 0) | WEBNET_WIRE_SERVICE | WEBNET_WIRE_STATUS_LINE | WEBNET_WIRE_LARGE_HEADERS | WEBNET_WIRE_DYNAMIC_BODY | WEBNET_WIRE_DYNAMIC_HEADERS | WEBNET_WIRE_DYNAMIC_REQUEST_HEADERS | WEBNET_WIRE_DYNAMIC_URL |
                        ((uint32_t)q->cache_mode << WEBNET_WIRE_CACHE_SHIFT);
    if (q->kind == WEBNET_CLASSIC || q->kind == WEBNET_MODULE) h.user_navigation |= WEBNET_WIRE_LARGE_SCRIPT;
    if (q->image_upgrade) h.user_navigation |= WEBNET_WIRE_IMAGE_UPGRADE;
    if (q->no_cors) h.user_navigation |= WEBNET_WIRE_NO_CORS;
    if (q->no_referrer) h.user_navigation |= WEBNET_WIRE_NO_REFERRER;
    if (referrer_extension)h.user_navigation|=WEBNET_WIRE_REFERRER;
    if (top_extension)h.user_navigation|=WEBNET_WIRE_TOP_LEVEL;
    if (q->stream_response) h.user_navigation |= WEBNET_WIRE_STREAM;
    if (r->debug_request) h.user_navigation |= WEBNET_WIRE_DEBUG_REQUEST;
    if (q->body) h.user_navigation |= WEBNET_WIRE_BODY_PRESENT;
    h.credentials = q->credentials; h.cookie_len = (uint32_t)cookie_len;
    h.id = r->id; h.generation = r->generation; h.deadline = r->deadline;
    h.url_len = ul; h.origin_len = ol; h.method_len = ml; h.headers_len = hl; h.body_len = q->body_len;
    memcpy(r->tx, &h, sizeof h);
    unsigned char *p = r->tx + sizeof h;
    if(referrer_extension){struct webnet_wire_referrer ref={(uint32_t)rl,(uint32_t)q->referrer_policy};memcpy(p,&ref,sizeof ref);p+=sizeof ref;}
    if(top_extension){struct webnet_wire_top_level top={(uint32_t)tl,0};memcpy(p,&top,sizeof top);p+=sizeof top;}
    memcpy(p, q->url, ul); p += ul; memcpy(p, origin, ol); p += ol;
    if(referrer_extension){memcpy(p,referrer,rl);p+=rl;}
    if(top_extension){memcpy(p,top_level_url,tl);p+=tl;}
    memcpy(p, method, ml); p += ml; memcpy(p, headers, hl); p += hl;
    if (q->body_len) memcpy(p, q->body, q->body_len);
    p += q->body_len;
    if (cookie_len && webcookie_export(n->cookies, p, (size_t)cookie_len, cookie_now) != cookie_len) { request_free(r); return 0; }
    struct request **at = &n->queue;
    while (*at && (*at)->priority <= r->priority) at = &(*at)->next;
    r->next = *at; *at = r;
    n->queued_bytes += len; n->count++;
    return r->id;
}
static bool origin_available(const webnet *n,const struct request *request){
    if(!request->transport_origin)return true;
    unsigned busy=0;
    for(unsigned i=0;i<WORKERS;i++){
        const struct request *active=n->slots[i].rq;
        if(n->slots[i].pid>0&&active&&!active->cancelled&&active->transport_origin&&!strcmp(active->transport_origin,request->transport_origin))busy++;
    }
    for(const struct slot *s=n->parked;s;s=s->next){const struct request *active=s->rq;
        if(s->pid>0&&active&&!active->cancelled&&active->transport_origin&&!strcmp(active->transport_origin,request->transport_origin))busy++;
    }
    /* Reserve the final HTTP/1 connection for document dependencies too,
       not just a global slot: same-origin long polls cannot starve a script. */
    return busy<(request->priority>=3?ORIGIN_WORKERS-1:ORIGIN_WORKERS);
}
static unsigned live_workers(const webnet *n){
    unsigned live=0;
    for(unsigned i=0;i<WORKERS;i++)if(n->slots[i].rq&&n->slots[i].pid>0)live++;
    for(const struct slot *s=n->parked;s;s=s->next)if(s->rq&&s->pid>0)live++;
    return live;
}
static struct request **available_class(webnet *n,unsigned priority){
    unsigned live=live_workers(n);
    if(live>=n->worker_limit)return NULL;
    if(priority==3&&n->worker_limit>1&&!n->slots[0].rq&&live>=n->worker_limit-1)return NULL;
    struct request **link=&n->queue;
    while(*link&&(*link)->priority<(int)priority)link=&(*link)->next;
    while(*link&&(*link)->priority==(int)priority){if(origin_available(n,*link))return link;link=&(*link)->next;}
    return NULL;
}
static struct request *take_request(webnet *n,unsigned slot){
    if(!n->queue)return NULL;
    struct request **chosen=available_class(n,0);
    if(slot==0&&n->worker_limit>1){
        /* Keep the existing navigation/render-resource lane. */
        if(!chosen)chosen=available_class(n,1);
        if(!chosen)chosen=available_class(n,2);
    }else if(!chosen){
        /* A continuing stream of scripts or newly queued images must not
           starve another resource class, including media fetches. FIFO within
           each class and actual transport concurrency are preserved. */
        unsigned first=n->next_priority?n->next_priority:1;
        for(unsigned offset=0;offset<3;offset++){
            unsigned priority=1+(first-1+offset)%3;
            struct request **link=available_class(n,priority);
            if(link){
                chosen=link;n->next_priority=priority==3?1:priority+1;break;
            }
        }
    }
    if(!chosen)return NULL;
    struct request *request=*chosen;
    *chosen=request->next;request->next=NULL;
    return request;
}
static bool service_start(webnet *n) {
    if(n->service_pid>0){
        int got=waitpid(n->service_pid,NULL,WNOHANG);
        if(!got || (got<0 && errno!=ECHILD))return true;
        n->service_pid=0;
        close(n->service_listener);n->service_listener=-1;
    }
    /* The native rendezvous key is an unguessable browser-owned capability.
       A different browser cannot join this process's cache/security partition. */
    uint64_t nonce[2];int random=open("/dev/urandom",O_RDONLY);
    if(random<0)return false;
    size_t pos=0;
    while(pos<sizeof nonce){ssize_t got=read(random,(char *)nonce+pos,sizeof nonce-pos);
        if(got<0&&errno==EINTR)continue;if(got<=0){close(random);return false;}pos+=(size_t)got;}
    close(random);
    snprintf(n->service_key,sizeof n->service_key,"webfetch:%d:%016lx%016lx",getpid(),(unsigned long)nonce[0],(unsigned long)nonce[1]);
    n->service_listener=n_channel_listen(n->service_key);
    if(n->service_listener<0)return false;
    int nullfd=open("/dev/null",O_WRONLY);
    if(nullfd<0){close(n->service_listener);n->service_listener=-1;return false;}
    int map[3]={n->service_listener,nullfd,nullfd};
    char *argv[]={"webfetch","--service",NULL};
    n->service_pid=spawn("/bin/webfetch",argv,map,0);
    int saved=errno;close(nullfd);
    if(n->service_pid<=0){close(n->service_listener);n->service_listener=-1;n->service_pid=0;errno=saved;return false;}
    return true;
}
static bool start(webnet *n,struct slot *s,struct request *r) {
    s->rq=r;s->shared=true;s->pid=0;s->in=s->out=-1;
    if(service_start(n)){
        int fd=n_channel_connect(n->service_key);
        if(fd>=0){s->in=s->out=fd;s->pid=n->service_pid;return true;}
    }
    int saved=errno;r->finishing=true;
    snprintf(r->error,sizeof r->error,"Cannot connect /bin/webfetch service (errno %d)",saved);
    return false;
}
struct cookie_scratch { char *url, *site_origin; };
static bool cookie_events_inner(webnet *n, struct request *r, struct cookie_scratch *work) {
    size_t pos = (size_t)r->head.url_len + r->head.headers_len + r->head.body_len + r->head.error_len + 4;
    const unsigned char *p = r->rx + pos, *end = p + r->head.cookie_len;
    /* Validate the whole private event stream before applying any update. */
    for (int pass = 0; pass < 2; pass++) {
        p = r->rx + pos;
        while (p < end) {
            if ((size_t)(end-p) < sizeof(struct webnet_wire_cookie)) return false;
            struct webnet_wire_cookie e; memcpy(&e,p,sizeof e); p += sizeof e;
            uint64_t length=(uint64_t)e.url_len+e.value_len;
            if (!e.url_len || e.value_len > WEBCOOKIE_FIELD_MAX ||
                e.redirect_cross_site > 1 || e.reserved || e.received < 0 || e.received > time(NULL) + 60 ||
                length > (size_t)(end-p) || length >= SIZE_MAX || memchr(p,0,(size_t)length)) return false;
            char *url=malloc((size_t)e.url_len+1);if(!url)return false;
            free(work->url);work->url=url;memcpy(url,p,e.url_len);url[e.url_len]=0;
            if (r->credentials == WEBNET_CREDENTIALS_OMIT) return false;
            if (r->credentials == WEBNET_CREDENTIALS_SAME_ORIGIN) {
                char *origin=NULL;enum http_url_result parsed=http_origin_owned(url,&origin);
                bool same=parsed==HTTP_URL_TUPLE&&work->site_origin&&!strcmp(origin,work->site_origin);
                free(origin);if(!same)return false;
            }
            if (pass) {
                struct webcookie_context c = {url, *r->cookie_site ? r->cookie_site : r->top_level ? NULL : "", "GET", r->top_level, true, e.redirect_cross_site != 0};
                if (webcookie_set(n->cookies,&c,(const char *)p+e.url_len,e.value_len,e.received) < 0) return false;
            }
            p += (size_t)e.url_len + e.value_len;
        }
    }
    return true;
}
static bool cookie_events(webnet *n, struct request *r) {
    if (!r->head.cookie_len) return true;
    struct cookie_scratch *work = calloc(1,sizeof *work);
    if (!work) return false;
    bool ok=r->credentials!=WEBNET_CREDENTIALS_SAME_ORIGIN||http_origin_owned(r->cookie_origin,&work->site_origin)==HTTP_URL_TUPLE;
    if(ok)ok = cookie_events_inner(n, r, work);
    free(work->url);free(work->site_origin);
    free(work);
    return ok;
}
static void complete(webnet *n, struct slot *s) {
    struct request *r = s->rq;
    s->rq = NULL; s->pid = 0; slot_close(s); n->count--;
    if (r->tx) n->queued_bytes -= r->tx_len;
    if(n->debug){
        uint64_t done=uptime_ms();
        /* These are caller-visible completion intervals, including TLS,
           pipe pumping, a busy GUI/native task and helper reaping. They are
           not server latency, download speed, or summed worker CPU time. */
        fprintf(stderr,"[webnet] complete id=%lu class=%d queued-to-complete=%lu ms dispatch-to-complete=%lu ms decoded-frame-bytes=%lu stream=%u cancelled=%u",
            (unsigned long)r->id,r->priority,(unsigned long)(done>=r->queued_at?done-r->queued_at:0),
            (unsigned long)(r->started_at&&done>=r->started_at?done-r->started_at:0),
            (unsigned long)r->head.body_len,r->streaming,r->cancelled);
        if(r->streaming)fprintf(stderr," stream-frames=%lu stream-bytes=%lu consumer-paused=%lu ms",
            (unsigned long)r->stream_frames,(unsigned long)r->stream_bytes,(unsigned long)r->paused_ms);
        fputc('\n',stderr);
    }
    if (!r->cancelled) {
        if (!r->streaming && !r->error[0] && r->rx && !cookie_events(n,r)) snprintf(r->error,sizeof r->error,"Invalid cookie update or out of memory");
        struct webnet_response response = {0};
        response.final_url = ""; response.headers = ""; response.body = ""; response.error = r->error;
        response.event = r->streaming ? WEBNET_END : WEBNET_COMPLETE;
        if (!r->error[0] && r->rx) {
            char *p = (char *)r->rx;
            response.status = (int)r->head.status;
            response.final_url = p; p += (size_t)r->head.url_len + 1;
            response.headers = p; p += (size_t)r->head.headers_len + 1;
            response.body = p; response.body_len = r->head.body_len; p += (size_t)r->head.body_len + 1;
            response.error = p;
        }
        r->callback(n, r->id, r->generation, &response, r->opaque);
    }
    request_free(r);
}
static bool diagnostic_number(const char **cursor, const char *field, uint64_t *value) {
    size_t n = strlen(field);
    if (strncmp(*cursor, field, n)) return false;
    const char *p = *cursor + n;
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (v > (UINT64_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p; *value = v; return true;
}
/* Private worker-only non-field metadata is stripped before any native
   callback/author Fetch or XHR sees headers. Server-field filtering cannot
   produce this no-colon line, and numeric validation precedes diagnostic use. */
static bool consume_request_diagnostic(struct request *r, char *headers) {
    if (!r->debug_request) return true;
    char *line = strstr(headers, "\r\nHTTP/Nocturne-Request ");
    if (!line) return true;
    const char *p = line + sizeof("\r\nHTTP/Nocturne-Request ") - 1;
    uint64_t bytes, ctl, non_ascii, cookie, content_length;
    bool valid = diagnostic_number(&p, "bytes=", &bytes) &&
        diagnostic_number(&p, " ctl=", &ctl) && diagnostic_number(&p, " nonascii=", &non_ascii) &&
        diagnostic_number(&p, " cookie=", &cookie) && diagnostic_number(&p, " content_length=", &content_length) &&
        p[0] == '\r' && p[1] == '\n' && ctl <= bytes && non_ascii <= bytes && cookie <= 1 && content_length <= 1;
    if (!valid || r->debug_reported) return false;
    memmove(line + 2, p + 2, strlen(p + 2) + 1);
    if (strstr(headers, "\r\nHTTP/Nocturne-Request ")) return false;
    r->debug_reported = true;
    fprintf(stderr, "[web-net:request] id=%lu generation=%lu status=%u header_bytes=%lu ctl=%lu nonascii=%lu cookie=%lu content_length=%lu\n",
        (unsigned long)r->id, (unsigned long)r->generation, r->head.status, (unsigned long)bytes,
        (unsigned long)ctl, (unsigned long)non_ascii, (unsigned long)cookie, (unsigned long)content_length);
    return true;
}
static bool consume_transport_diagnostic(struct request *r,char *headers) {
    if(!r->debug_request)return true;
    char *line=strstr(headers,"\r\nHTTP/Nocturne-Transport ");if(!line)return true;
    const char *p=line+sizeof("\r\nHTTP/Nocturne-Transport ")-1;
    uint64_t phase,attempts,resolve,connect,tls,first,head,body,io=0,other=0;
    if(!diagnostic_number(&p,"phase=",&phase)||!diagnostic_number(&p," attempts=",&attempts)||
       !diagnostic_number(&p," resolve=",&resolve)||!diagnostic_number(&p," connect=",&connect)||
       !diagnostic_number(&p," tls=",&tls)||!diagnostic_number(&p," first_header=",&first)||
       !diagnostic_number(&p," headers=",&head)||!diagnostic_number(&p," body=",&body))return false;
    bool split=!strncmp(p," tls_io=",8);
    if(split&&(!diagnostic_number(&p," tls_io=",&io)||!diagnostic_number(&p," tls_other=",&other)||
       io>tls||other>tls||io>UINT64_MAX-other||io+other!=tls))return false;
    if(p[0]!='\r'||p[1]!='\n'||phase>HTTP_PHASE_DONE||!attempts||attempts>42)return false;
    memmove(line,p+2,strlen(p+2)+1);
    if(strstr(headers,"HTTP/Nocturne-Transport "))return false;
    fprintf(stderr,"[webnet] transport id=%lu phase=%lu attempts=%lu resolve=%lu ms connect=%lu ms tls=%lu ms first-header=%lu ms headers=%lu ms body=%lu ms tls-split=%u tls-io=%lu ms tls-other=%lu ms\n",
        (unsigned long)r->id,(unsigned long)phase,(unsigned long)attempts,(unsigned long)resolve,
        (unsigned long)connect,(unsigned long)tls,(unsigned long)first,(unsigned long)head,(unsigned long)body,
        split,(unsigned long)io,(unsigned long)other);
    return true;
}
/* Wire data has no terminators; insert them after the complete payload has arrived. */
static bool unpack(struct request *r) {
    const struct webnet_wire_response *h = &r->head;
    size_t wire_len;
    if (!webnet_wire_payload_size(h, &wire_len)) return false;
    /* A successful retained upgrade-class request from HTTPS cannot publish
       an insecure final response, even if a worker violates the hop policy. */
    if (r->image_upgrade && !strncasecmp(r->cookie_origin, "https://", 8) && h->status &&
        (h->url_len < 8 || strncasecmp((const char *)r->rx, "https://", 8))) return false;
    unsigned char *out = malloc(wire_len + 4);
    if (!out) return false;
    size_t sizes[4] = {h->url_len, h->headers_len, h->body_len, h->error_len};
    size_t src = 0, dst = 0;
    for (int i = 0; i < 4; i++) {
        if (i != 2 && memchr(r->rx + src, 0, sizes[i])) { free(out); return false; }
        memcpy(out + dst, r->rx + src, sizes[i]); out[dst + sizes[i]] = 0;
        src += sizes[i]; dst += sizes[i] + 1;
    }
    memcpy(out+dst,r->rx+src,h->cookie_len);
    if (!consume_request_diagnostic(r, (char *)out + (size_t)h->url_len + 1)) { free(out); return false; }
    if (!consume_transport_diagnostic(r, (char *)out + (size_t)h->url_len + 1)) { free(out); return false; }
    free(r->rx); r->rx = out;
    return true;
}
static bool stream_frame_valid(struct request *r) {
    const struct webnet_wire_response *h = &r->head;
    if (h->magic == WEBNET_STREAM_HEADERS)
        return !r->headers_seen && h->status >= 200 && h->status <= 599 && h->url_len && !h->body_len && !h->error_len;
    if (h->magic == WEBNET_STREAM_CHUNK)
        return r->headers_seen && h->body_len && h->body_len <= WEBNET_STREAM_CHUNK_MAX &&
            !h->url_len && !h->headers_len && !h->error_len && !h->cookie_len && !h->status;
    if (h->magic == WEBNET_STREAM_UPLOAD)
        return !r->headers_seen && !h->url_len && !h->headers_len && !h->body_len && !h->error_len && !h->cookie_len;
    return h->magic == WEBNET_STREAM_END && !h->url_len && !h->headers_len && !h->body_len && !h->status &&
        (r->headers_seen || h->error_len);
}
/* Every stream event pauses the transport until the consumer resumes it. */
static void deliver_stream_event(webnet *n, struct request *r, const struct webnet_response *response) {
    if (n->debug && response->event == WEBNET_CHUNK) { r->stream_frames++; r->stream_bytes += response->body_len; }
    r->paused = true; if (n->debug) r->paused_at = uptime_ms();
    r->in_callback=true;
    r->callback(n, r->id, r->generation, response, r->opaque);
    r->in_callback=false;
}
static bool dispatch_stream_frame(webnet *n, struct request *r) {
    if (!unpack(r)) return false;
    if (!cookie_events(n, r)) return false;
    if (r->head.magic == WEBNET_STREAM_END) return true; /* terminal callback after reaping */
    char *p = (char *)r->rx;
    struct webnet_response response = {0};
    response.status = (int)r->head.status;
    response.final_url = p; p += (size_t)r->head.url_len + 1;
    response.headers = p; p += (size_t)r->head.headers_len + 1;
    response.body = p; response.body_len = r->head.body_len; p += (size_t)r->head.body_len + 1;
    response.error = p;
    response.event = r->head.magic == WEBNET_STREAM_HEADERS ? WEBNET_HEADERS :
        r->head.magic == WEBNET_STREAM_CHUNK ? WEBNET_CHUNK : WEBNET_UPLOAD;
    response.uploaded = r->head.magic == WEBNET_STREAM_UPLOAD ? r->head.status : 0;
    if (response.event == WEBNET_HEADERS) r->headers_seen = true;
    deliver_stream_event(n, r, &response);
    free(r->rx); r->rx = NULL; r->rx_len = r->rx_pos = r->head_pos = 0;
    memset(&r->head, 0, sizeof r->head);
    return true;
}
/* Append a complete, validated CHUNK frame to the pending delivery. False
   leaves the frame for ordinary dispatch: merging is bounded by the same
   receive window that bounds unread worker bytes. */
static bool merge_stream_chunk(struct request *r) {
    size_t n = r->head.body_len;
    if (r->merge_len > WEBNET_SERVICE_WINDOW - n) return false;
    if (r->merge_len + n + 1 > r->merge_cap) {
        size_t cap = MAX(r->merge_len + n + 1, MIN(r->merge_cap ? r->merge_cap * 2 : (size_t)WEBNET_STREAM_CHUNK_MAX, (size_t)WEBNET_SERVICE_WINDOW + 1));
        unsigned char *next = realloc(r->merge, cap);
        if (!next) return false;
        r->merge = next; r->merge_cap = cap;
    }
    memcpy(r->merge + r->merge_len, r->rx, n); r->merge_len += n; r->merge[r->merge_len] = 0;
    free(r->rx); r->rx = NULL; r->rx_len = r->rx_pos = r->head_pos = 0;
    memset(&r->head, 0, sizeof r->head);
    return true;
}
static void dispatch_merged_chunk(webnet *n, struct request *r) {
    struct webnet_response response = {0};
    response.final_url = response.headers = response.error = "";
    response.body = r->merge; response.body_len = r->merge_len;
    response.event = WEBNET_CHUNK;
    deliver_stream_event(n, r, &response);
    r->merge_len = 0;
    /* Retained idle streams keep at most one wire chunk of merge storage. */
    if (r->merge_cap > (size_t)WEBNET_STREAM_CHUNK_MAX + 1) { free(r->merge); r->merge = NULL; r->merge_cap = 0; }
}
struct pump_budget {size_t bytes;uint64_t until;bool callbacks;};
static bool pump_room(const struct pump_budget *budget){
    return budget->bytes && uptime_ms()<budget->until;
}
static void credit_flush(struct slot *s) {
    struct request *r=s->rq;
    if(!s->shared || r->tx || r->paused || r->credit_closed || s->in<0)return;
    if(!r->credit_pos && !r->credit.bytes){
        if(r->credit_due<WEBNET_SERVICE_CREDIT_STEP)return;
        r->credit=(struct webnet_wire_credit){WEBNET_CREDIT_MAGIC,(uint32_t)r->credit_due,r->id};
        r->credit_due=0;
    }
    while(r->credit.bytes){
        ssize_t got=write(s->in,(char *)&r->credit+r->credit_pos,sizeof r->credit-r->credit_pos);
        if(got<0&&(errno==EAGAIN||errno==EINTR))return;
        /* A response that fits the ahead window may already have retired its
           writer. Its buffered bytes still belong to this request: a credit
           EPIPE is not premature response EOF and must not discard them. */
        if(got<0&&errno==EPIPE){r->credit_closed=true;r->credit_due=r->credit_pos=0;memset(&r->credit,0,sizeof r->credit);return;}
        if(got<=0){slot_stop(s,"Worker credit channel closed");return;}
        r->credit_pos+=(size_t)got;
        if(r->credit_pos==sizeof r->credit){r->credit_pos=0;memset(&r->credit,0,sizeof r->credit);}
    }
}
/* Dispatch is separate from transport service: native JS checkpoints may
   drain one bounded frame into request-owned memory, never reenter author JS. */
static bool frame_dispatch(webnet *n,struct slot *s,struct pump_budget *budget){
    struct request *r=s->rq;
    if(!budget->callbacks)return false;
    /* Merged body bytes precede any frame read after them. */
    if(r->merge_len){dispatch_merged_chunk(n,r);return true;}
    if(!r->frame_ready)return false;
    r->frame_ready=false;
    bool terminal=!r->streaming || r->head.magic==WEBNET_STREAM_END;
    if(r->streaming){if(!dispatch_stream_frame(n,r)){slot_stop(s,"Invalid stream response or out of memory");return false;}}
    else if(!unpack(r)){slot_stop(s,"Invalid response or out of memory");return false;}
    if(terminal){r->finishing=true;slot_close(s);}
    return true;
}
static void pump_slot(webnet *n,struct slot *s,uint64_t now,struct pump_budget *global){
        struct request *r = s->rq;
        if (!r||r->in_callback) return;
        if (!r->finishing && now >= r->deadline) slot_stop(s, "Request deadline exceeded");
        reap_paused_stream(s);
        if(!r->finishing&&(r->frame_ready||r->merge_len)&&!r->paused)frame_dispatch(n,s,global);
        if (!r->finishing&&!r->paused&&!r->frame_ready&&pump_room(global)) {
            struct n_pollfd pf[2] = {{s->out, N_POLLIN, 0}, {s->in, N_POLLOUT, 0}};
            if (poll(pf, r->tx&&r->tx_credit&&s->in>=0 ? 2 : 1, 0) < 0) slot_stop(s, "Worker poll failed");
            size_t budget = MIN(PUMP_BYTES,global->bytes);
            while (!r->finishing && r->tx && (!s->shared||r->tx_credit) && s->in >= 0 && budget && (pf[1].revents & N_POLLOUT)) {
                size_t take = MIN(r->tx_len - r->tx_pos, budget);
                if(s->shared)take=MIN(take,r->tx_credit);
                ssize_t k = write(s->in, r->tx + r->tx_pos, take);
                if (k < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (k <= 0) { slot_stop(s, "Worker request pipe closed"); break; }
                r->tx_pos += k; budget -= k;global->bytes-=k;
                if(s->shared)r->tx_credit-=(size_t)k;
                if (r->tx_pos == r->tx_len) {
                    if(!s->shared){close(s->in);s->in=-1;}
                    n->queued_bytes -= r->tx_len; free(r->tx); r->tx = NULL;
                }
            }
            credit_flush(s);
            /* A streamed body drains into its merge buffer only while the
               consumer is not paused, so the reader's backpressure (not a
               fixed per-slot share) bounds it within the slice's bytes and
               time; the share capped such downloads at one share per browser
               turn while the worker already held the bytes. Buffered bodies
               keep the share: draining them at once moved large page scripts
               ahead of media startup and lengthened playback stalls in
               measurements, which needs main-thread work first. */
            budget = r->streaming ? global->bytes : MIN(PUMP_BYTES,global->bytes);
            while (!r->finishing && !r->paused && !r->frame_ready && budget && pump_room(global) && (pf[0].revents & (N_POLLIN | N_POLLHUP))) {
                void *dest; size_t take;
                if (r->head_pos < sizeof r->head) {
                    dest = (unsigned char *)&r->head + r->head_pos; take = sizeof r->head - r->head_pos;
                } else { dest = r->rx + r->rx_pos; take = r->rx_len - r->rx_pos; }
                take = MIN(take, budget);
                ssize_t k = read(s->out, dest, take);
                if (k < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (k <= 0) { slot_stop(s, "Worker response ended early"); break; }
                budget -= k;global->bytes-=k;
                if (r->head_pos < sizeof r->head) {
                    r->head_pos += k;
                    if (r->head_pos == sizeof r->head) {
                        struct webnet_wire_response *h = &r->head;
                        if(s->shared && h->magic==WEBNET_STREAM_REQUEST_CREDIT){
                            if(h->id!=r->id||h->generation!=r->generation||!h->status||h->status>WEBNET_SERVICE_WINDOW-r->tx_credit||
                               h->url_len||h->headers_len||h->body_len||h->error_len||h->cookie_len){slot_stop(s,"Invalid worker upload credit");break;}
                            r->tx_credit+=h->status;r->head_pos=0;memset(h,0,sizeof *h);continue;
                        }
                        if(s->shared)r->credit_due+=sizeof r->head;
                        if ((!r->streaming && h->magic != WEBNET_MAGIC) || (r->streaming && !stream_frame_valid(r)) || h->id != r->id || h->generation != r->generation ||
                            h->headers_len > webnet_wire_header_limit(WEBNET_WIRE_DYNAMIC_HEADERS) ||
                            h->body_len > r->response_limit || h->error_len >= sizeof r->error || (!r->streaming && h->status > 999) ||
                            h->cookie_len > webnet_wire_cookie_limit(WEBNET_WIRE_DYNAMIC_URL)) {
                            slot_stop(s, "Invalid worker response"); break;
                        }
                        if (!webnet_wire_payload_size(h, &r->rx_len)) { slot_stop(s, "Worker response size overflow"); break; }
                        /* Reject a frame only on actual memory affordability;
                           byte credit bounds all unread pipe storage separately. */
                        struct n_sysinfo memory={0};
                        if(r->rx_len>WEBNET_SERVICE_WINDOW && sysinfo(&memory)>=0 && r->rx_len>memory.free_mem/2){slot_stop(s,"Response exceeds available receive memory");break;}
                        r->rx = malloc(r->rx_len + 1);
                        if (!r->rx) { slot_stop(s, "Out of memory reading response"); break; }
                    }
                } else {r->rx_pos += k;if(s->shared)r->credit_due+=(size_t)k;}
                if (r->head_pos == sizeof r->head && r->rx_pos == r->rx_len) {
                    /* Keep draining body chunks that have already arrived;
                       the consumer gets them together when the pipe is empty
                       for now, the slice ends, or another frame follows. */
                    if(!(r->streaming && r->head.magic==WEBNET_STREAM_CHUNK && merge_stream_chunk(r))){
                        r->frame_ready=true;
                        frame_dispatch(n,s,global);
                    }
                }
                if(!r->finishing)credit_flush(s);
            }
            if(!r->finishing&&!r->paused&&r->merge_len)frame_dispatch(n,s,global);
        }
        if (r->finishing && global->callbacks) {
            /* Even a child that produced its frame must not hold a slot past the deadline. */
            if (s->pid > 0 && !s->shared && now >= r->deadline) kill(s->pid);
            if (slot_reap(s)) complete(n, s);
        }
}
static void pump_parked(webnet *n,uint64_t now,struct pump_budget *budget){
    struct slot **at=&n->parked,*last=NULL;
    while(*at){
        struct slot *s=*at;
        bool room=pump_room(budget);
        pump_slot(n,s,now,budget);
        if(!s->rq){*at=s->next;free(s);}
        else {if(room)last=s;at=&s->next;}
    }
    /* Rotate the serviced prefix instead of always favouring the newest
       parked stream. Requests remain discoverable by callbacks throughout. */
    if(last&&last->next){struct slot *rest=last->next,*tail=rest;while(tail->next)tail=tail->next;
        tail->next=n->parked;last->next=NULL;n->parked=rest;}
}
static void pump(webnet *n,uint64_t now,bool callbacks){
    if(!n||n->pumping)return;
    n->pumping=true;
    struct pump_budget budget={PUMP_TOTAL_BYTES,uptime_ms()+PUMP_TIME_MS,callbacks};
    if(!n->budget_at||now>=n->budget_at){n->worker_limit=worker_budget();n->budget_at=now+1000;}
    struct request **qp=&n->queue;
    while(callbacks&&*qp){struct request *r=*qp;if(now<r->deadline){qp=&r->next;continue;}*qp=r->next;r->next=NULL;struct slot temp={r,0,-1,-1,NULL,false};snprintf(r->error,sizeof r->error,"Request deadline exceeded");complete(n,&temp);qp=&n->queue;}
    /* Alternate active and resumed retained streams. Both groups receive a
       turn even when another callback uses the rest of this bounded slice. */
    bool parked_first=n->parked_first;n->parked_first=!parked_first;
    if(parked_first)pump_parked(n,now,&budget);
    unsigned first=n->pump_cursor;
    for(unsigned step=0;step<WORKERS;step++){
        unsigned i=(first+step)%WORKERS;
        struct slot *s=&n->slots[i];
        if(!s->rq&&(unsigned)i<n->worker_limit&&pump_room(&budget)){struct request *r=take_request(n,(unsigned)i);if(r){
            r->started_at=now;
            if(n->debug)fprintf(stderr,"[webnet] dispatch id=%lu class=%d queued=%lu ms budget=%u\n",
                (unsigned long)r->id,r->priority,(unsigned long)(now>=r->queued_at?now-r->queued_at:0),n->worker_limit);
            start(n,s,r);
        }}
        if(pump_room(&budget))n->pump_cursor=(i+1)%WORKERS;
        pump_slot(n,s,now,&budget);
        if(s->rq&&s->rq->paused&&!s->rq->finishing){
            struct slot *held=malloc(sizeof *held);
            if(!held){slot_stop(s,"Out of memory retaining stream transport");continue;}
            *held=*s;held->next=n->parked;n->parked=held;memset(s,0,sizeof *s);s->in=s->out=-1;
        }
    }
    if(!parked_first)pump_parked(n,now,&budget);
    n->pumping=false;
}
void webnet_pump(webnet *n,uint64_t now){pump(n,now,true);}
void webnet_service(webnet *n,uint64_t now){pump(n,now,false);}
bool webnet_busy(const webnet *n) { return n && n->count != 0; }
unsigned webnet_inflight(const webnet *n){return n?live_workers(n):0;}
int webnet_timeout(const webnet *n, uint64_t now) {
    if (!webnet_busy(n)) return -1;
    int ms = INT_MAX;
    for (const struct request *r = n->queue; r; r = r->next) {
        if (now >= r->deadline) return 0;
        if (r->deadline - now < (uint64_t)ms) ms = (int)(r->deadline - now);
    }
    for (int i = 0; i < WORKERS; i++) if (n->slots[i].rq) {
        const struct request *r = n->slots[i].rq;
        if (r->finishing || (!r->paused&&(r->frame_ready||r->merge_len)) || now >= r->deadline) return 0;
        if (r->deadline - now < (uint64_t)ms) ms = (int)(r->deadline - now);
    }
    for(const struct slot *s=n->parked;s;s=s->next)if(s->rq){const struct request *r=s->rq;if(r->finishing||(!r->paused&&(r->frame_ready||r->merge_len))||now>=r->deadline)return 0;if(r->deadline-now<(uint64_t)ms)ms=(int)(r->deadline-now);}
    /* Unadmitted work must be reconsidered after the service has retired a
       paused peer. Other live requests wake through their own channel. */
    if(n->queue && !live_workers(n))return 0;
    return ms;
}
static void wait_slot(struct n_pollfd *fds,unsigned *count,const struct slot *s){
    const struct request *r=s->rq;
    if(!r||r->finishing||r->frame_ready||s->out<0)return;
    /* A small paused response may finish inside its byte window. Observe only
       peer retirement so its concurrency slot can admit queued dependencies;
       unread body data itself must never wake a paused consumer. */
    if(r->paused){if(s->pid>0)fds[(*count)++]=(struct n_pollfd){s->out,0,0};return;}
    short events=N_POLLIN;
    if((r->tx&&r->tx_credit)||(!r->credit_closed&&(r->credit.bytes||r->credit_due>=WEBNET_SERVICE_CREDIT_STEP)))events|=N_POLLOUT;
    fds[(*count)++]=(struct n_pollfd){s->out,events,0};
}
int webnet_wait(webnet *n,int event_fd,int timeout_ms){
    /* Only live native workers can have an unpaused transport. Closed parked
       response shells are already buffered and never enter this poll set. */
    unsigned count=event_fd>=0?1u:0u;
    struct n_pollfd fds[WORKERS*2+1];
    if(event_fd>=0)fds[0]=(struct n_pollfd){event_fd,N_POLLIN,0};
    if(n){
        int due=webnet_timeout(n,uptime_ms());
        if(due>=0&&(timeout_ms<0||due<timeout_ms))timeout_ms=due;
        for(unsigned i=0;i<WORKERS;i++)wait_slot(fds,&count,&n->slots[i]);
        for(const struct slot *s=n->parked;s;s=s->next){
            if(count==sizeof fds/sizeof *fds){timeout_ms=0;break;}
            wait_slot(fds,&count,s);
        }
    }
    return poll(fds,(int)count,timeout_ms);
}
static void cancel(webnet *n, uint64_t key, bool generation) {
    if (!n) return;
    struct request **p = &n->queue;
    while (*p) {
        struct request *r = *p;
        if ((generation ? r->generation : r->id) != key || (generation && r->keepalive)) { p = &r->next; continue; }
        *p = r->next; n->count--; n->queued_bytes -= r->tx_len; request_free(r);
    }
    for (int i = 0; i < WORKERS; i++) {
        struct slot *s = &n->slots[i];
        if (s->rq && (generation ? s->rq->generation : s->rq->id) == key && !(generation && s->rq->keepalive)) {
            s->rq->cancelled = true; slot_stop(s, NULL);
            if (s->pid > 0 && !s->shared) kill(s->pid);
        }
    }
    for(struct slot *s=n->parked;s;s=s->next)if(s->rq&&(generation?s->rq->generation:s->rq->id)==key&&!(generation&&s->rq->keepalive)){s->rq->cancelled=true;slot_stop(s,NULL);}
    if (generation) {
        /* Document teardown needs stopped children, not merely pending kill requests.
           Signal both first; Nocturne wakes blocked killed tasks so wait can reap them.
           Individual fetch aborts still use the nonblocking pump/reap path above. */
        for (int i = 0; i < WORKERS; i++) {
            struct slot *s = &n->slots[i];
            if (!s->rq || s->rq->generation != key || s->rq->keepalive) continue;
            if (s->pid > 0 && !s->shared) {waitpid(s->pid, NULL, 0);s->pid=0;}
            if(s->rq->in_callback)continue; /* borrowed response lives through callback return */
            complete(n, s); /* cancelled requests never invoke their callback */
        }
        for(struct slot *s=n->parked;s;s=s->next)if(s->rq&&s->rq->generation==key&&!s->rq->keepalive){
            if(s->pid>0&&!s->shared){waitpid(s->pid,NULL,0);s->pid=0;}
            if(!s->rq->in_callback)complete(n,s);
        }
    }
}
void webnet_cancel(webnet *n, uint64_t id) { cancel(n, id, false); }
static void resume_request(webnet *n, struct request *r) {
    if (n->debug && r->paused && r->paused_at) { r->paused_ms += uptime_ms() - r->paused_at; r->paused_at = 0; }
    r->paused = false;
}
void webnet_resume(webnet *n, uint64_t id) {
    if (!n) return;
    for (int i = 0; i < WORKERS; i++) {
        struct request *r = n->slots[i].rq;
        if (r && r->id == id && r->streaming && !r->cancelled) { resume_request(n, r); return; }
    }
    for(struct slot *s=n->parked;s;s=s->next){struct request *r=s->rq;if(r&&r->id==id&&r->streaming&&!r->cancelled){resume_request(n,r);return;}}
}
void webnet_cancel_generation(webnet *n, uint64_t generation) { cancel(n, generation, true); }
