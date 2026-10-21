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
int *__errno_location(void){static int value;return &value;}
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
#if defined(WEBNET_DIAGNOSTIC_HOST_TEST)
/* Link-only native boundaries for this diagnostic-only mode. No requests,
   sockets, scheduler budget or origin lookup are exercised by its checks. */
#include <stdarg.h>
FILE *stderr;
int fprintf(FILE *stream,const char *format,...){(void)stream;va_list args;
    va_start(args,format);int n=vprintf(format,args);va_end(args);return n;}
n_result cpu_info(struct n_cpuinfo *cpu){memset(cpu,0,sizeof *cpu);cpu->online_cpus=4;return 0;}
int sysinfo(struct n_sysinfo *memory){memset(memory,0,sizeof *memory);memory->free_mem=2ull<<30;return 0;}
bool url_parse_owned(const char *raw,struct url_owned *out){(void)raw;memset(out,0,sizeof *out);return false;}
void url_owned_free(struct url_owned *out){(void)out;}
enum http_url_result http_origin_owned(const char *raw,char **out){(void)raw;*out=NULL;return HTTP_URL_OPAQUE;}
int main(void) {
    struct request r={.debug_request=true};
    char headers[512]="HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHTTP/Nocturne-Transport phase=6 attempts=2 resolve=3 connect=4 tls=5 first_header=7 headers=8 body=9\r\n";
    CHECK(consume_transport_diagnostic(&r,headers));
    CHECK(!strcmp(headers,"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"));
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=3 attempts=1 resolve=0 connect=2 tls=20000 first_header=0 headers=0 body=0\r\n");
    CHECK(consume_transport_diagnostic(&r,headers)&&!headers[0]);
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=99 attempts=1 resolve=0 connect=0 tls=0 first_header=0 headers=0 body=0\r\n");
    CHECK(!consume_transport_diagnostic(&r,headers));
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=6 attempts=0 resolve=0 connect=0 tls=0 first_header=0 headers=0 body=0\r\n");
    CHECK(!consume_transport_diagnostic(&r,headers));
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=6 attempts=1 resolve=18446744073709551616 connect=0 tls=0 first_header=0 headers=0 body=0\r\n");
    CHECK(!consume_transport_diagnostic(&r,headers));
    const char *head="\r\nHTTP/Nocturne-Transport phase=6 attempts=1 resolve=3 connect=4 tls=5 first_header=7 headers=8 body=9\r\n";
    r.head=(struct webnet_wire_response){.headers_len=strlen(head),.body_len=3};
    r.rx=malloc(strlen(head)+3);memcpy(r.rx,head,strlen(head));memcpy(r.rx+strlen(head),"abc",3);
    CHECK(unpack(&r));CHECK(r.rx[1]==0);
    CHECK(!memcmp(r.rx+1+r.head.headers_len+1,"abc",3));free(r.rx);
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=3 attempts=1 resolve=0 connect=3 tls=11 first_header=0 headers=0 body=0 tls_io=7 tls_other=4\r\n");
    CHECK(consume_transport_diagnostic(&r,headers)&&!headers[0]);
    strcpy(headers,"\r\nHTTP/Nocturne-Transport phase=3 attempts=1 resolve=0 connect=3 tls=11 first_header=0 headers=0 body=0 tls_io=7 tls_other=5\r\n");
    CHECK(!consume_transport_diagnostic(&r,headers));
    printf("webnet-private-timing: %d checks, %d failed\n",checks,failed);return failed!=0;
}
#else
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
#endif
