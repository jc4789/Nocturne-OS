/* Production webnet queue/pump regression. Native pipes/processes and cookies
   are substituted; no live guest network or website acceptance is claimed. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Windows uses long long for ssize_t; keep the native Nocturne declaration
   separate, without changing production headers for a host-only harness. */
#define ssize_t nocturne_test_ssize_t
#define pipe test_pipe
#define open test_open
#define close test_close
#define spawn test_spawn
#define fcntl test_fcntl
#define poll test_poll
#define read test_read
#define write test_write
#define kill test_kill
#define waitpid test_waitpid
#define uptime_ms test_uptime_ms
#include "../user/libc/webnet.c"
int errno;
static uint64_t now=100;
static int descriptor=10, process=100, spawned, checks, failed, callbacks;
struct webcookie_jar { int unused; };
#define CHECK(c) do {checks++;if(!(c)){failed++;printf("FAIL scheduler:%d %s\n",__LINE__,#c);}}while(0)
int test_pipe(int fds[2]){fds[0]=descriptor++;fds[1]=descriptor++;return 0;}
int test_open(const char *path,int flags,...){(void)path;(void)flags;return descriptor++;}
int test_close(int fd){(void)fd;return 0;}
int test_spawn(const char *path,char *const argv[],const int fdmap[3],int flags) {
    (void)argv;(void)fdmap;(void)flags;CHECK(!strcmp(path,"/bin/webfetch"));spawned++;return process++;
}
int test_fcntl(int fd,int cmd,...){(void)fd;(void)cmd;return 0;}
int test_poll(struct n_pollfd *fds,int n,int timeout) {
    (void)timeout;for(int i=0;i<n;i++)fds[i].revents=fds[i].events&N_POLLOUT;return 0;
}
ssize_t test_read(int fd,void *buf,size_t n){(void)fd;(void)buf;(void)n;errno=EAGAIN;return -1;}
ssize_t test_write(int fd,const void *buf,size_t n){(void)fd;(void)buf;return (ssize_t)n;}
int test_kill(int pid){(void)pid;return 0;}
int test_waitpid(int pid,int *status,int flags){(void)status;(void)flags;return pid;}
uint64_t test_uptime_ms(void){return now;}
webcookie_jar *webcookie_create(void){return calloc(1,sizeof(webcookie_jar));}
void webcookie_free(webcookie_jar *jar){free(jar);}
bool webcookie_psl_load(webcookie_jar *jar,const char *text,size_t len){(void)jar;(void)text;(void)len;return true;}
long webcookie_get(webcookie_jar *jar,const struct webcookie_context *ctx,char *out,size_t cap,int64_t time) {
    (void)jar;(void)ctx;(void)out;(void)cap;(void)time;return 0;
}
int webcookie_set(webcookie_jar *jar,const struct webcookie_context *ctx,const char *value,size_t len,int64_t time) {
    (void)jar;(void)ctx;(void)value;(void)len;(void)time;return 0;
}
long webcookie_export(webcookie_jar *jar,void *out,size_t cap,int64_t time){(void)jar;(void)out;(void)cap;(void)time;return 0;}
bool url_parse(const char *url,struct url *out){(void)url;(void)out;return false;}
static void completed(webnet *n,uint64_t id,uint64_t gen,const struct webnet_response *r,void *opaque) {
    (void)n;(void)id;(void)gen;(void)opaque;callbacks++;CHECK(strstr(r->error,"deadline")!=NULL);
}
static uint64_t submit(webnet *n,enum webnet_kind kind,uint64_t generation) {
    struct webnet_request q={.kind=kind,.generation=generation,.url="https://mail.google.com/channel",
        .origin="https://mail.google.com/",.method="GET",.credentials=WEBNET_CREDENTIALS_OMIT};
    uint64_t id=webnet_submit(n,&q,completed,NULL);CHECK(id!=0);return id;
}
int main(void) {
    /* New clients opt in by native script kind, never by response MIME or an
       author-controlled header; non-script/request body limits remain legacy. */
    for(unsigned kind=WEBNET_NAVIGATION;kind<=WEBNET_FETCH;kind++) {
        webnet *policy=webnet_create();CHECK(policy!=NULL);if(!policy)return 1;
        submit(policy,(enum webnet_kind)kind,99);
        struct request *r=policy->queue;struct webnet_wire_request h;memcpy(&h,r->tx,sizeof h);
        bool script=kind==WEBNET_CLASSIC||kind==WEBNET_MODULE;
        CHECK(((h.user_navigation&WEBNET_WIRE_LARGE_SCRIPT)!=0)==script);
        CHECK(r->response_limit==(script?WEBNET_SCRIPT_BODY_LIMIT:WEBNET_BODY_LIMIT));
        CHECK(WEBNET_SCRIPT_BODY_LIMIT+1>r->response_limit);
        if(!script)CHECK(WEBNET_BODY_LIMIT+1>r->response_limit);
        webnet_free(policy);
    }
    webnet *n=webnet_create();CHECK(n!=NULL);if(!n)return 1;
    for(int i=0;i<3;i++)submit(n,WEBNET_FETCH,1);
    webnet_pump(n,now);CHECK(spawned==3&&!n->slots[0].rq);
    submit(n,WEBNET_CLASSIC,2);webnet_pump(n,now);
    CHECK(spawned==4&&n->slots[0].rq&&n->slots[0].rq->priority==1);
    submit(n,WEBNET_FETCH,3);CHECK(n->queue&&n->queue->priority==3);
    webnet_cancel_generation(n,2);submit(n,WEBNET_RESOURCE,4);webnet_pump(n,now);
    CHECK(n->slots[0].rq&&n->slots[0].rq->priority==2&&n->queue&&n->queue->priority==3);
    /* Cancel/reap a shared resource slot: queued Fetch gets a turn rather than
       being forgotten when only the reserved slot was available. */
    webnet_cancel_generation(n,1);webnet_pump(n,now);
    CHECK(!n->queue&&n->slots[1].rq&&n->slots[1].rq->generation==3&&callbacks==0);
    now+=WEBNET_TIMEOUT_MS;webnet_pump(n,now);
    CHECK(callbacks==2&&!webnet_busy(n));
    webnet_free(n);
    n=webnet_create();for(int i=0;i<4;i++)submit(n,WEBNET_RESOURCE,5);
    submit(n,WEBNET_FETCH,6);webnet_pump(n,now);CHECK(n->queue&&n->queue->generation==6);
    webnet_cancel_generation(n,5);webnet_pump(n,now);
    CHECK(n->slots[1].rq&&n->slots[1].rq->generation==6&&!n->queue);
    webnet_cancel_generation(n,6);CHECK(!webnet_busy(n));webnet_free(n);
    printf("network_scheduler_hosttest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
