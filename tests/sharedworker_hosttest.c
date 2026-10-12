/* Real product registry/publication/lifetime in three independent QuickJS
 * contexts. Native process and network operations are mocked, not acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <quickjs.h>
#include "../user/libc/web/js_worker.c"
int waitpid(int p,int*s,int f){(void)p;(void)s;(void)f;errno=ECHILD;return -1;}
int kill(int p){(void)p;return 0;}
int pipe(int*p){(void)p;return -1;}
int spawn(const char*p,char*const*a,const int*f,int n){(void)p;(void)a;(void)f;return n==SPAWN_DETACH?500:-1;}
int open(const char*p,int f,...){(void)p;(void)f;return 7;}
int n_channel_listen(const char*k){(void)k;return 8;}
int n_channel_connect(const char*k){(void)k;return 9;}
int n_channel_accept(int f){(void)f;errno=EAGAIN;return -1;}
int close(int f){(void)f;return 0;}
ssize_t read(int f,void*p,size_t n){(void)f;(void)p;(void)n;return -1;}
ssize_t write(int f,const void*p,size_t n){(void)f;(void)p;(void)n;return -1;}
int poll(struct n_pollfd*p,int n,int t){(void)p;(void)n;(void)t;return 0;}
int fcntl(int f,int c,...){(void)f;(void)c;return 0;}
static unsigned checks,failures,loads,cancels;
static void check(const char*n,bool ok){checks++;printf("%s %s\n",ok?"PASS":"FAIL",n);if(!ok)failures++;}
static bool load(void*p,uint32_t id,uint32_t req,const char*url,int kind){(void)p;(void)id;(void)req;(void)url;(void)kind;loads++;return true;}
static void cancel(void*p,uint32_t id,uint32_t req){(void)p;(void)id;(void)req;cancels++;}
static uint32_t create(web_workers*w,const char*url,const char*name){JSContext*c=w->ctx;JSValue a[]={JS_NewInt32(c,5),JS_NewInt32(c,0),JS_NewString(c,url),JS_NewString(c,name)};JSValue v=web_worker_native(w,4,a);uint32_t id=0;if(!JS_IsException(v))JS_ToUint32(c,&id,v);else{JSValue ex=JS_GetException(c);JS_FreeValue(c,ex);}for(int i=0;i<4;i++)JS_FreeValue(c,a[i]);JS_FreeValue(c,v);return id;}
static uint32_t connect(web_workers*w,uint32_t id){JSContext*ctx=w->ctx;JSValue args[]={JS_NewInt32(ctx,4),JS_NewUint32(ctx,id)};JSValue v=web_worker_native(w,2,args);uint32_t port=0;JS_ToUint32(ctx,&port,v);JS_FreeValue(ctx,v);JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);
 JSValue wire=JS_NewArray(ctx),list=JS_NewArray(ctx),meta=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,meta,0,JS_NewUint32(ctx,port));JS_SetPropertyUint32(ctx,meta,1,JS_NewInt32(ctx,0));JS_SetPropertyUint32(ctx,meta,2,JS_NewArray(ctx));JS_SetPropertyUint32(ctx,list,0,meta);JS_SetPropertyUint32(ctx,wire,0,JS_NULL);JS_SetPropertyUint32(ctx,wire,1,list);web_worker_publication*t=web_worker_prepare(w,id,NJW_CONNECT,0,NJW_WITH_PORTS,wire);if(t)web_worker_publish(t);else port=0;JS_FreeValue(ctx,wire);return port;
}
int main(void){JSRuntime*rt=JS_NewRuntime();JSContext*a=JS_NewContext(rt),*b=JS_NewContext(rt),*d=JS_NewContext(rt);web_workers*x=web_worker_new(a,11,load,NULL),*y=web_worker_new(b,22,load,NULL),*z=web_worker_new(d,33,load,NULL);web_worker_set_origin(x,"https://fixture.test");web_worker_set_origin(y,"https://fixture.test");web_worker_set_origin(z,"https://other.test");web_worker_set_canceler(x,cancel);web_worker_set_canceler(y,cancel);
 uint32_t id=create(x,"https://fixture.test/worker.js","shared"),same=create(y,"https://fixture.test/worker.js","shared");check("same-origin-documents-reuse-one-native-realm",id&&id==same&&loads==1&&find(y,id)==find(x,id));check("cross-origin-registry-isolation",create(z,"https://fixture.test/worker.js","shared")!=id);check("named-worker-different-url-rejected",!create(y,"https://fixture.test/other.js","shared"));
 uint32_t p=connect(x,id),q=connect(y,id);struct child*c=find(x,id);check("each-connection-has-unique-native-port",p&&q&&p!=q&&broker_find(c,p,0)->client==x&&broker_find(c,q,0)->client==y);check("foreign-document-cannot-capture-port",JS_IsException(web_worker_capture(x,id,q,0)));JS_FreeValue(a,JS_GetException(a));
 web_worker_loaded(x,id,0,200,c->url,"", "onconnect=function(){};",23,"");check("start-precedes-connect-and-marks-shared",c->tx&&c->tx->h.op==NJW_START&&c->tx->next&&c->tx->next->h.op==NJW_CONNECT);uint32_t generation=c->generation;
 JSValue payload=JS_NewString(a,"routed");struct event*e=calloc(1,sizeof*e);size_t size=0;uint8_t*raw=JS_WriteObject(a,&size,payload,JS_WRITE_OBJ_REFERENCE);e->data=malloc(size);memcpy(e->data,raw,size);js_free(a,raw);JS_FreeValue(a,payload);e->id=id;e->h=(struct njw_header){NJW_MAGIC,NJW_PORT_MESSAGE,generation,q,0,(uint32_t)size};x->queued+=sizeof e->h+size;route_event(x,c,e);check("native-port-message-goes-only-to-owning-document",!x->events&&y->events==e);
 JSContext*spare=JS_NewContext(rt);web_workers*u=web_worker_new(spare,44,load,NULL);web_worker_set_origin(u,"https://fixture.test");create(u,"https://fixture.test/worker.js","shared");uint32_t disposable=connect(u,id);
 for(unsigned i=0;i<2;i++){struct event*received=calloc(1,sizeof*received);received->id=id;received->h=e->h;received->h.request=disposable;received->data=malloc(size);memcpy(received->data,e->data,size);received->from_child=i==0;x->queued+=sizeof received->h+size;route_event(x,c,received);}
 web_worker_free(u);unsigned credits=0;double returned=0;for(struct packet*t=c->tx;t;t=t->next)if(t->h.op==NJW_CREDIT){credits++;JSValue value=JS_ReadObject(a,t->data,t->h.bytes,JS_READ_OBJ_REFERENCE);JS_ToFloat64(a,&returned,value);JS_FreeValue(a,value);}
 check("retiring-non-owner-returns-one-actual-packet-credit",credits==1&&returned==(double)sizeof e->h+size&&c->owner==x&&!c->stopped);JS_FreeContext(spare);
 web_worker_free(x);check("original-document-navigation-preserves-realm",find(y,id)==c&&c->owner==y&&!c->stopped&&c->generation==generation);check("navigation-detaches-only-original-port",!broker_find(c,p,0)->client&&broker_find(c,p,0)->closed&&broker_find(c,q,0)->client==y&&!broker_find(c,q,0)->closed);check("old-network-owner-cancelled",cancels==1);size_t bytes=c->rx_charge;for(struct packet*t=c->tx;t;t=t->next)bytes+=sizeof t->h+t->h.bytes;for(struct event*t=y->events;t;t=t->next)bytes+=sizeof t->h+t->h.bytes;check("queue-charge-moves-once-with-owner",bytes==y->queued);
 web_worker_free(y);web_worker_free(z);check("last-owner-removes-shared-registry",!shared_children&&!managers);JS_FreeContext(a);JS_FreeContext(b);JS_FreeContext(d);JS_FreeRuntime(rt);printf("RESULT %u checks %u failures\n",checks,failures);return failures?1:0;
}
