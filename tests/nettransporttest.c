/* Real native-network regression: DNS UDP/TCP, candidates, send deadlines,
   cancellation and AP data-plane copies. Host fixtures are scripts/test.py's
   TCP service and tests/fixtures/dns_server.py, never an Internet dependency. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
#include "http.h"

#define HOST 0x0a000202u
static int failed, tcp_port, dns_port;
static void check(bool ok,const char *name) {printf("%s %s\n",ok?"ok  ":"FAIL",name);if(!ok)failed++;}
static int read_port(const char *path) {
    FILE *f=fopen(path,"r");char text[32]="";
    if(f){fgets(text,sizeof text,f);fclose(f);}return atoi(text);
}
static int open_stall(void) {
    int s=tcp_connect(HOST,(uint16_t)tcp_port,2000);
    if(s<0)return -1;
    const char request[]="STALL 0 0\n";
    if(tcp_send(s,request,sizeof request-1)!=(long)sizeof request-1){tcp_close(s);return -1;}
    char ready[6];size_t done=0;
    while(done<sizeof ready){long n=tcp_recv(s,ready+done,sizeof ready-done,2000);if(n<=0){tcp_close(s);return -1;}done+=(size_t)n;}
    if(memcmp(ready,"READY\n",6)){tcp_close(s);return -1;}return s;
}
static unsigned char *upload;
static volatile uint32_t sender_entered;
/* Same aligned x86 acquire/release contract as threadtest; guest TCC does
   not implement GCC's __atomic builtins. */
static unsigned load_entered(void) {
    unsigned value;__asm__ volatile("movl %1, %0":"=r"(value):"m"(sender_entered):"memory");return value;
}
static void publish_entered(void) {
    unsigned value=1;__asm__ volatile("movl %1, %0":"=m"(sender_entered):"r"(value):"memory");
}
static long cancelled_prefix;
static int cancelled_error, cancel_socket;
static void blocked_sender(void *unused) {
    (void)unused;size_t sent=0;uint64_t deadline=uptime_ms()+3000;
    publish_entered();
    while(sent<(8u<<20)) {
        long n=tcp_send_until(cancel_socket,upload+sent,(8u<<20)-sent,deadline);
        if(n<=0){cancelled_error=errno;break;}sent+=(size_t)n;
    }
    cancelled_prefix=(long)sent;
}
static void send_deadlines(void) {
    upload=malloc(8u<<20);check(upload!=NULL,"send regression buffer allocated");if(!upload)return;
    memset(upload,0x5a,8u<<20);
    int s=open_stall();check(s>=0,"zero-window fixture READY");
    if(s>=0) {
        uint64_t start=uptime_ms(),deadline=start+500;size_t sent=0;int error=0;
        while(sent<(8u<<20)) {
            long n=tcp_send_until(s,upload+sent,(8u<<20)-sent,deadline);
            if(n<=0){error=errno;break;}sent+=(size_t)n;
        }
        uint64_t elapsed=uptime_ms()-start;
        printf("SEND-DEADLINE accepted=%lu elapsed=%lu errno=%d\n",(unsigned long)sent,(unsigned long)elapsed,error);
        check(sent>0 && sent<(8u<<20) && error==ETIMEDOUT && elapsed>=450 && elapsed<1000,
              "full transmit storage returns accepted prefix within original 500ms deadline");
        errno=0;check(tcp_send_until(s,"x",1,uptime_ms())<0 && errno==ETIMEDOUT,"expired deadline enqueues no new byte");
        start=uptime_ms();errno=0;long immediate=tcp_send_some(s,upload,8u<<20);
        check((immediate>=0 && immediate<=65536) || (immediate<0 && errno==EAGAIN),"nonblocking send returns bounded prefix or EAGAIN");
        check(uptime_ms()-start<150,"nonblocking send never waits for the peer window");
        tcp_cancel(s);tcp_close(s);
    }
    cancel_socket=open_stall();check(cancel_socket>=0,"cancel fixture READY");
    if(cancel_socket>=0) {
        sender_entered=0;cancelled_error=0;cancelled_prefix=0;
        int tid=thread_create(blocked_sender,NULL);check(tid>=0,"create blocking native sender");
        if(tid>=0) {
            while(!load_entered())yield();
            msleep(150);uint64_t start=uptime_ms();
            check(tcp_cancel(cancel_socket)==0,"cancel native connection");
            check(thread_join(tid)==0 && cancelled_error==ECANCELED && cancelled_prefix>0 &&
                  cancelled_prefix<(8u<<20) && uptime_ms()-start<750,"cancellation wakes full-buffer sender and preserves prefix result");
            char byte;errno=0;
            check(tcp_recv(cancel_socket,&byte,1,0)<0 && errno==ECANCELED,"cancelled receive does not expose abandoned data");
            errno=0;check(tcp_send(cancel_socket,"x",1)<0 && errno==ECANCELED,"cancelled connection cannot silently resend");
        }
        tcp_close(cancel_socket);
    }
    free(upload);
}
static bool has_ip(const uint32_t *ips,unsigned count,uint32_t ip) {for(unsigned i=0;i<count;i++)if(ips[i]==ip)return true;return false;}
static void dns_regression(void) {
    struct n_sockaddr resolver={.ip=HOST,.port=(uint16_t)dns_port};
    struct n_dns_result result;uint32_t ips[32];
    bool multi=net_resolve_at("multi.fixture.test",ips,32,&result,&resolver)==0 && result.status==N_DNS_OK && result.count==3 &&
               has_ip(ips,3,0x0a0002fau) && has_ip(ips,3,HOST);
    check(multi,"DNS returns all A candidates, not the first only");
    check(net_resolve_at("tc.fixture.test",ips,32,&result,&resolver)==0 && result.count==2 && has_ip(ips,2,HOST),
          "truncated UDP answer is retrieved over length-framed TCP");
    check(net_resolve_at("alias.fixture.test",ips,32,&result,&resolver)==0 && result.count==1 && ips[0]==HOST,
          "CNAME chain ignores unrelated answer addresses");
    check(net_resolve_at("missing.fixture.test",ips,32,&result,&resolver)<0 && result.status==N_DNS_NXDOMAIN && errno==ENOENT,
          "DNS NXDOMAIN has typed status");
    check(net_resolve_at("servfail.fixture.test",ips,32,&result,&resolver)<0 && result.status==N_DNS_SERVFAIL && errno==EAGAIN,
          "DNS temporary server error is distinct from NXDOMAIN");
    check(net_resolve_at("refused.fixture.test",ips,32,&result,&resolver)<0 && result.status==N_DNS_REFUSED,
          "DNS REFUSED has distinct status");
    check(net_resolve_at("empty.fixture.test",ips,32,&result,&resolver)<0 && result.status==N_DNS_NODATA,
          "successful empty DNS answer reports NODATA");
    /* The fixture answers cache-* only once. A second wire query is refused. */
    char name[220];bool cached=true;
    for(unsigned i=0;i<20;i++) {
        snprintf(name,sizeof name,"cache-%u.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.fixture.test",i);
        if(net_resolve_at(name,ips,32,&result,&resolver)<0 || result.count!=1)cached=false;
    }
    for(unsigned i=0;i<20;i++) {
        snprintf(name,sizeof name,"cache-%u.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.abcdefghijklmnopqrstuvwxyzabcdefghijklmnop.fixture.test",i);
        if(net_resolve_at(name,ips,32,&result,&resolver)<0 || result.count!=1 || result.ttl==0)cached=false;
    }
    check(cached,"more than eight long hostnames retain complete fresh cached results");
    check(net_resolve_at("multi.fixture.test",ips,1,&result,&resolver)==0 && result.count==3,"small caller capacity reports complete candidate count");
}
struct setup_case {int fd,mode,result,error;unsigned dns_status;volatile uint32_t entered,release;};
static unsigned load_flag(const volatile uint32_t *flag) {
    unsigned value;__asm__ volatile("movl %1, %0":"=r"(value):"m"(*flag):"memory");return value;
}
static void set_flag(volatile uint32_t *flag) {
    unsigned value=1;__asm__ volatile("movl %1, %0":"=m"(*flag):"r"(value):"memory");
}
static void setup_worker(void *opaque) {
    struct setup_case *test=opaque;
    if(net_set_setup_cancel_channel(test->fd)<0){test->error=errno;set_flag(&test->entered);return;}
    set_flag(&test->entered);
    if(test->mode==3) {
        while(!load_flag(&test->release))yield();
        return; /* kernel thread-exit must release the retained endpoint */
    }
    if(test->mode==1) {
        test->result=tcp_connect(0x0a0002fa,(uint16_t)tcp_port,5000);test->error=errno;
        if(test->result>=0)tcp_close(test->result);
    } else {
        struct n_sockaddr resolver={.ip=HOST,.port=(uint16_t)dns_port};struct n_dns_result result;uint32_t address;
        test->result=net_resolve_at(test->mode==2?"tcstall.fixture.test":"stall.fixture.test",&address,1,&result,&resolver);
        test->error=errno;test->dns_status=result.status;
    }
    net_set_setup_cancel_channel(-1);
}
static bool channel_ends(int *peer,int *local) {
    n_handle ends[2];if(n_channel_pair(ends)<0){*peer=*local=-1;return false;}
    *peer=ends[0];*local=ends[1];return true;
}
static bool peer_closed(int h) {
    n_handle q=n_events_create();struct n_event e={0};
    bool closed=q>=0&&n_events_watch(q,h,N_EVENT_CLOSED,1)>=0&&n_events_wait(q,&e,1,0)==1;
    if(q>=0)n_handle_close(q);return closed;
}
static void setup_cancellation(void) {
    for(unsigned mode=0;mode<3;mode++) {
        int peer,local,other_peer,other_local;channel_ends(&peer,&local);channel_ends(&other_peer,&other_local);
        if(local<0 || other_local<0){check(false,"setup cancellation channel pair");if(peer>=0)close(peer);if(local>=0)close(local);if(other_peer>=0)close(other_peer);if(other_local>=0)close(other_local);continue;}
        struct setup_case test={.fd=local,.mode=(int)mode,.result=0,.error=0,.dns_status=N_DNS_OK};
        int tid=thread_create(setup_worker,&test);check(tid>=0,"create setup cancellation worker");
        if(tid>=0) {
            while(!load_flag(&test.entered))yield();
            msleep(120);
            /* Reuse the descriptor while the context retains the original
               endpoint. Cancelling the original peer must still be seen. */
            close(local);check(dup2(other_local,local)==local,"setup cancellation pins endpoint across fd replacement");
            uint64_t before=uptime_ms();close(peer);peer=-1;
            check(thread_join(tid)==0 && test.result<0 && test.error==ECANCELED && uptime_ms()-before<600,
                  mode==0?"unanswered DNS setup cancels promptly":mode==1?"ARP and TCP setup cancels promptly":"DNS TCP fallback setup cancels promptly");
            if(mode!=1)check(test.dns_status==N_DNS_CANCELLED,"setup cancellation preserves typed resolver status");
        }
        if(peer>=0)close(peer);close(local);close(other_local);close(other_peer);
    }
    int exit_peer,exit_local;channel_ends(&exit_peer,&exit_local);
    if(exit_local>=0) {
        struct setup_case test={.fd=exit_local,.mode=3};int tid=thread_create(setup_worker,&test);
        if(tid>=0) {
            while(!load_flag(&test.entered))yield();close(exit_local);exit_local=-1;
            bool held=!peer_closed(exit_peer);
            check(held,"setup context retains endpoint after descriptor close");
            set_flag(&test.release);thread_join(tid);
            check(peer_closed(exit_peer),"native thread exit releases its setup context endpoint");
        } else check(false,"create setup scope exit worker");
    } else check(false,"setup scope exit fixture");
    if(exit_local>=0)close(exit_local);if(exit_peer>=0)close(exit_peer);
    /* Cancelling a setup scope must not poison an established wire, including
       a wire shared by independent HTTP/2 streams. */
    int established=tcp_connect(HOST,(uint16_t)tcp_port,2000);
    int peer,local;channel_ends(&peer,&local);
    if(established>=0 && local>=0) {
        check(net_set_setup_cancel_channel(local)==0,"install calling-thread setup scope");close(peer);peer=-1;
        uint32_t address;errno=0;
        check(net_resolve("localhost",&address)<0 && errno==ECANCELED,"closed peer cancels new setup before publication");
        const char request[]="GET 4 1\n";uint8_t data[4];size_t done=0;
        bool sent=tcp_send(established,request,sizeof request-1)==(long)sizeof request-1;
        while(sent && done<sizeof data){long n=tcp_recv(established,data+done,sizeof data-done,1000);if(n<=0)break;done+=(size_t)n;}
        uint32_t x=1;bool same=done==4;for(unsigned i=0;i<done;i++){x=x*1103515245u+12345u;if(data[i]!=(uint8_t)(x>>16))same=false;}
        check(sent && same,"setup scope cancellation leaves established TCP data intact");
        check(net_set_setup_cancel_channel(-1)==0 && net_resolve("localhost",&address)==0 && address==0x7f000001,
              "clearing setup context releases cancellation for later work");
    } else check(false,"established transport cancellation fixture");
    if(established>=0)tcp_close(established);if(local>=0)close(local);if(peer>=0)close(peer);
}
struct close_case {int socket,error;long result;volatile uint32_t entered;uint8_t bytes[32];};
static void waiting_receiver(void *opaque) {
    struct close_case *test=opaque;set_flag(&test->entered);
    test->result=tcp_recv(test->socket,test->bytes,sizeof test->bytes,5000);test->error=errno;
}
static bool blocked_thread(int tid) {
    struct n_procinfo processes[256];int count=proclist(processes,256);
    for(int i=0;i<count;i++)if(processes[i].pid==tid)return processes[i].state==2;
    return false;
}
static uint8_t race_payload[65536];
static void racing_sender(void *opaque) {
    struct close_case *test=opaque;set_flag(&test->entered);
    test->result=tcp_send_some(test->socket,race_payload,sizeof race_payload);test->error=errno;
}
static void close_lifetime(void) {
    int old=open_stall();check(old>=0,"native close lifetime stall fixture");
    if(old>=0) {
        struct close_case test={.socket=old};memset(test.bytes,0xcc,sizeof test.bytes);
        int tid=thread_create(waiting_receiver,&test);check(tid>=0,"create blocked old-generation receiver");
        if(tid>=0) {
            uint64_t until=uptime_ms()+1000;
            while(!blocked_thread(tid) && uptime_ms()<until)msleep(1);
            check(blocked_thread(tid),"old-generation receive entered native progress wait");
            tcp_cancel(old);tcp_close(old);old=-1;
            /* Retain lower free slots if another lingering tuple expired in
               the meantime. This guarantees that reuse tests this exact slot. */
            size_t retained_count=0,retained_capacity=(size_t)test.socket+1;
            int *retained=malloc(retained_capacity*sizeof *retained);int fresh=-1;
            if(retained) for(size_t i=0;i<retained_capacity;i++) {
                int candidate=tcp_connect(HOST,(uint16_t)tcp_port,2000);
                if(candidate<0)break;
                if(candidate==test.socket){fresh=candidate;break;}
                retained[retained_count++]=candidate;
            }
            bool fresh_ok=fresh>=0;
            if(fresh>=0) {
                const char request[]="GET 32 113\n";fresh_ok=tcp_send(fresh,request,sizeof request-1)==(long)sizeof request-1;
            }
            thread_join(tid);bool untouched=true;for(unsigned i=0;i<sizeof test.bytes;i++)if(test.bytes[i]!=0xcc)untouched=false;
            check(test.result<0 && (test.error==EBADF||test.error==ECANCELED) && untouched,
                  "closed receive cannot copy from a reused socket generation");
            if(fresh>=0) {
                uint8_t bytes[32];size_t done=0;
                while(done<sizeof bytes){long n=tcp_recv(fresh,bytes+done,sizeof bytes-done,2000);if(n<=0)break;done+=(size_t)n;}
                uint32_t x=113;fresh_ok=fresh_ok&&done==32;
                for(unsigned i=0;i<done;i++){x=x*1103515245u+12345u;if(bytes[i]!=(uint8_t)(x>>16))fresh_ok=false;}
                check(fresh==test.socket && fresh_ok,"reused native handle retains the entire new response");tcp_close(fresh);
            } else check(false,"reopen native handle after cancelled close");
            for(size_t i=0;i<retained_count;i++)tcp_close(retained[i]);free(retained);
        }
        if(old>=0)tcp_close(old);
    }
    uint32_t x=0;for(unsigned i=0;i<sizeof race_payload;i++){x=x*1103515245u+12345u;race_payload[i]=(uint8_t)(x>>16);}
    bool safe=true;unsigned copied=0;
    for(unsigned round=0;round<24;round++) {
        int socket=tcp_connect(HOST,(uint16_t)tcp_port,2000);if(socket<0){safe=false;break;}
        const char request[]="PUT 65536 0\n";
        if(tcp_send(socket,request,sizeof request-1)!=(long)sizeof request-1){tcp_close(socket);safe=false;break;}
        struct close_case test={.socket=socket};int tid=thread_create(racing_sender,&test);
        if(tid<0){tcp_close(socket);safe=false;break;}
        while(!load_flag(&test.entered))yield();
        tcp_close(socket);thread_join(tid);
        if(test.result>0)copied++;
        if(!((test.result>0&&test.result<=65536)||(test.result<0&&(test.error==EBADF||test.error==EPIPE||test.error==EAGAIN))))safe=false;
    }
    printf("TCP-CLOSE-RACE copied=%u rounds=24\n",copied);
    check(safe,"native close joins any AP ring owner before protocol teardown");
}
struct transfer {unsigned id;int result;uint64_t cpus;};
static void download_worker(void *argument) {
    struct transfer *work=argument;work->result=-1;
    int s=tcp_connect(HOST,(uint16_t)tcp_port,3000);if(s<0)return;
    char request[64];snprintf(request,sizeof request,"GET 262144 %u\n",work->id+77);
    if(tcp_send(s,request,strlen(request))!=(long)strlen(request)){tcp_close(s);return;}
    uint8_t *buffer=malloc(16384);if(!buffer){tcp_close(s);return;}
    size_t got=0;uint32_t x=work->id+77;bool same=true;
    while(got<262144) {
        work->cpus|=1ull<<cpu_index();
        long n=tcp_recv(s,buffer,16384,3000);if(n<=0)break;
        for(long i=0;i<n;i++){x=x*1103515245u+12345u;if(buffer[i]!=(uint8_t)(x>>16))same=false;}
        got+=(size_t)n;
    }
    free(buffer);tcp_close(s);work->result=same&&got==262144?0:-1;
}
static void parallel_transfers(void) {
    struct n_netstats before,after;struct n_cpuinfo info;net_stats(&before);cpu_info(&info);
    struct transfer transfers[4]={{.id=0},{.id=1},{.id=2},{.id=3}};int tids[4];
    for(unsigned i=0;i<4;i++)tids[i]=thread_create(download_worker,&transfers[i]);
    bool correct=true;for(unsigned i=0;i<4;i++)if(tids[i]<0 || thread_join(tids[i])<0 || transfers[i].result)correct=false;
    check(correct,"parallel native TCP connections preserve every byte");
    check(net_stats(&after)==0,"network CPU counters readable");
    uint64_t bytes=0,mask=0;
    for(unsigned cpu=1;cpu<16;cpu++)if(after.ap_recv_bytes[cpu]>before.ap_recv_bytes[cpu]) {
        bytes+=after.ap_recv_bytes[cpu]-before.ap_recv_bytes[cpu];mask|=1ull<<cpu;
    }
    printf("NETWORK-CPU online=%u scheduler=%u ap_rx=%lu mask=%lx\n",info.online_cpus,info.scheduler_cpus,(unsigned long)bytes,(unsigned long)mask);
    check(info.scheduler_cpus<=1 || (bytes>0 && mask!=0),"available APs perform actual TCP receive copies");
}
static bool stream_default_dns(void){
    struct n_netinfo info;bool configured=net_info(&info)==0&&info.up&&
        info.dns[0]==10&&info.dns[1]==0&&info.dns[2]==2&&info.dns[3]==2;
    check(configured,"native stream uses the isolated boot-configured default resolver");return configured;
}
static void stream_candidates(void){
    if(!stream_default_dns())return;
    struct n_dns_result answer;uint32_t ips[4]={0};
    bool order=net_resolve_all("stream-failover.fixture.test",ips,4,&answer)==0&&answer.count==2&&ips[0]==0x0a0002fau&&ips[1]==HOST;
    check(order,"default product DNS publishes an unreachable first address and the reachable second address");
    char error[160]={0};uint64_t before=uptime_ms();
    /* The test does not connect candidates. ns_open must resolve and make all
       TCP attempts itself through the normal product open_stream path. */
    net_stream *stream=ns_open("stream-failover.fixture.test",(uint16_t)tcp_port,false,1600,error,sizeof error);
    struct ns_connection_info connection={0};
    bool switched=stream&&ns_get_connection_info(stream,&connection)==0&&connection.version==1&&
        connection.candidates==2&&connection.attempts==2&&connection.peer_ip==HOST;
    check(switched,"production ns_open performs the failed first TCP attempt and connects the second DNS candidate");
    bool complete=false;
    if(stream){
        const char request[]="GET 4096 931\n";unsigned char bytes[4096];size_t received=0;
        bool same=ns_write(stream,request,sizeof request-1)==(long)(sizeof request-1);uint32_t state=931;
        while(same&&received<sizeof bytes){long got=ns_read(stream,bytes+received,sizeof bytes-received);if(got<=0){same=false;break;}received+=(size_t)got;}
        for(size_t i=0;i<received;i++){state=state*1103515245u+12345u;if(bytes[i]!=(uint8_t)(state>>16))same=false;}
        complete=same&&received==sizeof bytes;ns_close(stream);
    }
    check(complete,"the candidate selected by the product exchanges every byte of the real fixture response");
    printf("STREAM-CANDIDATES plain candidates=%u attempts=%u peer=%08x wall-ms=%lu error=%s\n",
        connection.candidates,connection.attempts,connection.peer_ip,(unsigned long)(uptime_ms()-before),error);
    struct http_transport_timing timing={0};error[0]=0;before=uptime_ms();errno=0;
    stream=ns_open_timed("stream-all-bad.fixture.test",(uint16_t)tcp_port,false,1200,error,sizeof error,&timing);
    int cause=errno;uint64_t elapsed=uptime_ms()-before;if(stream)ns_close(stream);
    check(!stream&&(cause==ETIMEDOUT||cause==EHOSTUNREACH)&&timing.phase==HTTP_PHASE_CONNECT&&elapsed<=1700,
          "all failed candidates return a typed connect error within one shared product deadline");
    printf("STREAM-CANDIDATES exhausted phase=%u errno=%d connect-ms=%lu wall-ms=%lu error=%s\n",
        (unsigned)timing.phase,cause,(unsigned long)timing.connect_ms,(unsigned long)elapsed,error);
}
static bool tls_fixture_mode(bool enabled){
    uint32_t address=0;struct n_dns_result result;
    return net_resolve_all(enabled?"arm-tls-failover.fixture.test":"disarm-tls-failover.fixture.test",&address,1,&result)==0&&result.count==1&&address==HOST;
}
static void tls_stream_candidates(void){
    if(!stream_default_dns())return;
    bool armed=tls_fixture_mode(true);check(armed,"DNS fixture enables unreachable-first candidates for the real TLS hostname");
    if(!armed)return;
    for(unsigned http=0;http<2;http++){
        char error[160]={0};struct http_transport_timing timing={0};struct ns_connection_info connection={0};
        ns_sessions_clear();uint64_t before=uptime_ms();
        net_stream *stream=http?ns_open_http_timed("nghttp2.org",443,true,10000,error,sizeof error,&timing,"dns-integration"):
            ns_open_timed("nghttp2.org",443,true,10000,error,sizeof error,&timing);
        bool authenticated=stream&&ns_get_connection_info(stream,&connection)==0&&connection.version==1&&
            connection.candidates>1&&connection.attempts>1&&connection.peer_ip!=0x0a0002fau&&connection.peer_ip!=0&&!ns_session_resumed(stream);
        check(authenticated,http?"normal HTTP TLS opener retries DNS candidates and completes certificate-verified handshake":
              "normal TLS stream opener retries DNS candidates and completes certificate-verified handshake");
        bool protocol=false;
        if(stream){
            if(http)protocol=!strcmp(ns_protocol(stream),"h2");
            else {
                const char request[]="GET /httpbin/get HTTP/1.1\r\nHost: nghttp2.org\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n";
                char response[4096]={0};size_t used=0;
                if(ns_write(stream,request,sizeof request-1)==(long)(sizeof request-1)){
                    while(used<sizeof response-1&&!strchr(response,'\n')){
                        long got=ns_read(stream,response+used,sizeof response-1-used);if(got<=0)break;
                        used+=(size_t)got;response[used]=0;
                    }
                }
                protocol=used>12&&(!strncmp(response,"HTTP/1.1 200",12)||!strncmp(response,"HTTP/1.0 200",12));
            }
            ns_close(stream);
        }
        check(protocol,http?"authenticated candidate fallback negotiates real HTTP/2 ALPN":
              "authenticated candidate fallback receives a real HTTPS 200 response");
        printf("STREAM-CANDIDATES tls=%u candidates=%u attempts=%u peer=%08x connect-ms=%lu tls-ms=%lu wall-ms=%lu error=%s\n",
            http,connection.candidates,connection.attempts,connection.peer_ip,(unsigned long)timing.connect_ms,
            (unsigned long)timing.tls_ms,(unsigned long)(uptime_ms()-before),error);
    }
    check(tls_fixture_mode(false),"TLS candidate case restores ordinary external DNS answers");ns_sessions_clear();
}
int main(int argc,char **argv) {
    tcp_port=read_port("/data/tests/tcpport");dns_port=read_port("/data/tests/dnsport");
    if(!tcp_port || !dns_port || !net_wait_up(5000)){puts("FAIL network transport fixture unavailable");return 1;}
    if(argc==2&&!strcmp(argv[1],"--stream-candidates")){stream_candidates();printf("nettransport stream: %d failed\n",failed);return failed!=0;}
    if(argc==2&&!strcmp(argv[1],"--tls-candidates")){tls_stream_candidates();printf("nettransport TLS candidates: %d failed\n",failed);return failed!=0;}
    if(argc!=1){puts("FAIL unknown nettransporttest selector");return 1;}
    dns_regression();setup_cancellation();send_deadlines();close_lifetime();parallel_transfers();
    printf("nettransporttest: %d failed\n",failed);return failed!=0;
}
