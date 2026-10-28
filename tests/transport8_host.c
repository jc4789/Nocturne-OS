/* Windows loopback adapter for the production HTTP parsers. No TLS assertion
   is made by this adapter; QEMU uses the actual net_stream implementation. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "../user/include/http.h"
struct net_stream{SOCKET socket;uint64_t sent;char error[128];int (*cancelled)(void*);void *ctx;bool h2_model;};
static void error(net_stream *s){int e=WSAGetLastError();errno=e==WSAETIMEDOUT?ETIMEDOUT:e==WSAECONNRESET?ECONNRESET:EIO;snprintf(s->error,sizeof s->error,"winsock %d",e);}
uint64_t uptime_ms(void){return GetTickCount64();}
void yield(void){SwitchToThread();}
void n_sleep_ms(uint64_t n){Sleep((DWORD)n);}
struct native_thread{void (*fn)(void*);void *arg;};
static DWORD WINAPI run_thread(void *arg){struct native_thread t=*(struct native_thread*)arg;free(arg);t.fn(t.arg);return 0;}
int thread_create(void (*fn)(void*),void *arg){struct native_thread *t=malloc(sizeof *t);if(!t)return -1;t->fn=fn;t->arg=arg;HANDLE h=CreateThread(NULL,0,run_thread,t,0,NULL);return h?(int)(intptr_t)h:-1;}
int thread_join(int tid){HANDLE h=(HANDLE)(intptr_t)tid;WaitForSingleObject(h,INFINITE);CloseHandle(h);return 0;}
void ns_set_timeout(net_stream *s,int timeout){DWORD n=timeout<0?0:(DWORD)timeout;setsockopt(s->socket,SOL_SOCKET,SO_RCVTIMEO,(const char*)&n,sizeof n);setsockopt(s->socket,SOL_SOCKET,SO_SNDTIMEO,(const char*)&n,sizeof n);}
net_stream *ns_open_http_timed(const char *host,uint16_t port,bool tls,int timeout,char *out,size_t cap,struct http_transport_timing *timing,const char *partition) {
    (void)partition;if(timing)memset(timing,0,sizeof *timing);
    bool model=tls&&getenv("NOCTURNE_H2_TLS_MODEL")!=NULL;
    if(tls&&!model){snprintf(out,cap,"host adapter has no TLS");return NULL;}
    static LONG initialized;if(InterlockedCompareExchange(&initialized,1,0)==0){WSADATA d;WSAStartup(MAKEWORD(2,2),&d);InterlockedExchange(&initialized,2);}else while(initialized!=2)Sleep(0);
    net_stream *s=calloc(1,sizeof *s);if(!s)return NULL;s->h2_model=model;s->socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(port);inet_pton(AF_INET,host,&address.sin_addr);
    if(connect(s->socket,(struct sockaddr*)&address,sizeof address)){error(s);snprintf(out,cap,"%s",s->error);closesocket(s->socket);free(s);return NULL;}
    ns_set_timeout(s,timeout);return s;
}
const char *ns_protocol(net_stream *s){return s->h2_model?"h2":"http/1.1";}
net_stream *ns_open_http_cancelled_timed(const char *h,uint16_t p,bool tls,int t,char *e,size_t n,
    struct http_transport_timing *m,const char *partition,int (*cancelled)(void *),void *ctx){
    if(cancelled&&cancelled(ctx)){errno=ECANCELED;snprintf(e,n,"request cancelled");return NULL;}
    return ns_open_http_timed(h,p,tls,t,e,n,m,partition);}
bool ns_session_resumed(net_stream *s){(void)s;return false;}
void ns_set_cancel(net_stream *s,int (*fn)(void*),void *ctx){s->cancelled=fn;s->ctx=ctx;}
long ns_write(net_stream *s,const void *p,size_t n){size_t at=0;while(at<n){int got=send(s->socket,(const char*)p+at,(int)(n-at),0);if(got<=0){error(s);return -1;}at+=got;s->sent+=got;}return (long)at;}
long ns_read(net_stream *s,void *p,size_t n){int got=recv(s->socket,p,(int)n,0);if(got<0)error(s);return got;}
long ns_try_read(net_stream *s,void *p,size_t n){fd_set f;FD_ZERO(&f);FD_SET(s->socket,&f);struct timeval t={0};int ready=select(0,&f,NULL,NULL,&t);if(!ready)return -2;return ns_read(s,p,n);}
long ns_try_write(net_stream *s,const void *p,size_t n){if(!n)return 0;fd_set f;FD_ZERO(&f);FD_SET(s->socket,&f);struct timeval t={0};int ready=select(0,NULL,&f,NULL,&t);if(!ready)return -2;int got=send(s->socket,p,(int)n,0);if(got<0)error(s);else s->sent+=got;return got;}
const char *ns_error(net_stream *s){return s->error;}
uint64_t ns_bytes_sent(net_stream *s){return s->sent;}
void ns_abort(net_stream *s){if(s){closesocket(s->socket);free(s);}}
void ns_close(net_stream *s){ns_abort(s);}
void ns_sessions_clear(void){}
void *http_br_create(void){return NULL;}int http_br_decode(void *s,const unsigned char **in,size_t *n,unsigned char **out,size_t *room){(void)s;(void)in;(void)n;(void)out;(void)room;return -1;}void http_br_free(void *s){(void)s;}
