/* The product multi-client server's private routing, lifetime and clone
 * publication. Host endpoints are a model, never OS or website acceptance. */
#include <windows.h>
#undef TA_LEFT
#undef TA_RIGHT
#undef TA_CENTER
#undef BS_SOLID
#undef RGB
#undef TRANSPARENT
#define main unused_product_worker_main
#include "../user/apps/browserjsworker.c"
#undef main
#include <stdio.h>
int waitpid(int p,int*s,int f){(void)p;(void)s;(void)f;errno=ECHILD;return -1;}
int kill(int p){(void)p;return 0;}
int pipe(int*p){(void)p;return -1;}
int spawn(const char*p,char*const*a,const int*f,int n){(void)p;(void)a;(void)f;(void)n;return -1;}
int open(const char*p,int f,...){(void)p;(void)f;return 7;}
int close(int f){(void)f;return 0;}
static struct njw_header last_wire;static int last_wire_fd;
#ifndef WORKER_STREAM_MODEL
ssize_t read(int f,void*p,size_t n){(void)f;(void)p;(void)n;errno=EAGAIN;return -1;}
ssize_t write(int f,const void*p,size_t n){if(n==sizeof last_wire){struct njw_header h;memcpy(&h,p,sizeof h);if(h.magic==NJW_MAGIC){last_wire=h;last_wire_fd=f;}}return n;}
int poll(struct n_pollfd*p,int n,int t){(void)p;(void)n;(void)t;return 0;}
#endif
int fcntl(int f,int c,...){(void)f;(void)c;return 0;}
int n_channel_listen(const char*k){(void)k;return 8;}
int n_channel_connect(const char*k){(void)k;return 9;}
#ifndef WORKER_CANCEL_MODEL
int n_channel_accept(int f){(void)f;errno=EAGAIN;return -1;}
#endif
uint64_t uptime_ms(void){return 100;}
int cpu_info(struct n_cpuinfo*p){(void)p;return -1;}
void web_js_encoding_init_isolated(JSContext*c,JSValue h){(void)c;(void)h;}
bool html_policy_requires_script(const web_doc*d){(void)d;return false;}
bool html_policy_init(web_doc*d,const char*h,const web_doc*i){(void)d;(void)h;(void)i;return true;}
bool html_policy_snapshot_init(web_doc*d,const char*h){(void)d;(void)h;return true;}
void html_policy_free(web_doc*d){(void)d;}
static unsigned checks,failures;
static void check(const char*n,bool ok){checks++;printf("%s %s\n",ok?"PASS":"FAIL",n);if(!ok)failures++;}
static JSValue eval(JSContext*c,const char*s){return JS_Eval(c,s,strlen(s),"shared-server-fixture",JS_EVAL_TYPE_GLOBAL);}
static JSValue envelope(JSContext*c){return eval(c,"[[[1,1],[['ExternalTransferred',0],['Object',0,[['data',[0,null]],['ports',[1,2]]]],['Array',1,[['0',[1,0]]]]]],[[1,0,[]]],'https://fixture.test/worker.js']");}
#ifndef SHARED_SERVER_SUPPORT_ONLY
int main(void){JSRuntime*rt=JS_NewRuntime();JSContext*ctx=JS_NewContext(rt);struct worker w={0};w.ctx=ctx;w.rt=rt;w.hooks=JS_UNDEFINED;w.shared=true;w.script_url="https://fixture.test/worker.js";w.policy_initialized=true;w.had_client=true;JS_SetContextOpaque(ctx,&w);
 struct client*a=calloc(1,sizeof*a),*b=calloc(1,sizeof*b);*a=(struct client){.fd=8,.generation=11,.active=true,.established=true,.next=b};*b=(struct client){.fd=9,.generation=22,.active=true,.established=true};w.clients=a;w.network_client=a;
 struct njw_header h={NJW_MAGIC,NJW_CONNECT,11,0,NJW_WITH_PORTS,1};JSValue va=envelope(ctx),vb=envelope(ctx);w.current_client=a;bool aa=native_port_receive(&w,&h,va);w.current_client=b;bool bb=native_port_receive(&w,&h,vb);check("equal-client-local-ids-get-distinct-global-ports",aa&&bb&&shared_pair(&w,a,1,0)&&shared_pair(&w,b,1,0)&&shared_pair(&w,a,1,0)->id!=shared_pair(&w,b,1,0)->id);uint32_t globalb=shared_pair(&w,b,1,0)->id;
 JSValue args[]={JS_NewInt32(ctx,NJW_PORT_MESSAGE),JS_NewUint32(ctx,globalb),JS_NewInt32(ctx,0),eval(ctx,"[[1,0],[['Object',0,[['data',[0,42]],['ports',[1,1]]]],['Array',0,[]]]]"),eval(ctx,"({buffers:[],writes:[],cancels:[]})")};JSValue result=native_port_send(ctx,JS_UNDEFINED,5,args);check("port-reply-targets-client-and-local-id-generation",!JS_IsException(result)&&!w.output&&last_wire_fd==b->fd&&last_wire.request==1&&last_wire.generation==22);JS_FreeValue(ctx,result);for(unsigned i=0;i<5;i++)JS_FreeValue(ctx,args[i]);
 struct deferred message={.h={NJW_MAGIC,NJW_PORT_MESSAGE,22,1,0,1},.client=b};check("client-incoming-header-remaps-once",shared_incoming(&w,&message)&&message.h.request==globalb&&shared_incoming(&w,&message)&&message.h.request==globalb);
 struct pending_load*load=calloc(1,sizeof*load);*load=(struct pending_load){NULL,17,b};b->references++;w.loads=load;w.network_client=b;w.current_client=NULL;shared_disconnect(&w,b);check("client-disconnect-preserves-other-native-connection",!b->active&&a->active&&!w.closed&&w.network_client==a);check("network-owner-disconnect-rejects-pending-resource",!w.loads&&w.head&&w.head->h.op==NJW_LOADED&&w.head->h.request==17);check("disconnect-queues-own-port-close-not-other-port",w.head&&w.head->next&&w.head->next->h.op==NJW_PORT_CLOSE&&w.head->next->h.request==globalb&&!shared_pair(&w,a,1,0)->closed);
 output_flush(&w);check("disconnected-client-output-is-reclaimed",!w.output&&w.output_bytes==0);
 w.policy_initialized=false;w.need_start=true;shared_restart_source(&w);check("startup-owner-loss-requests-source-from-surviving-client",w.output&&w.output->h.op==NJW_LOAD&&!w.output->h.request&&w.output->client==a&&!w.need_start);
 JSValue wrong=eval(ctx,"[null,[],'https://fixture.test/other.js']");size_t size;uint8_t*bytes=JS_WriteObject(ctx,&size,wrong,JS_WRITE_OBJ_REFERENCE);struct deferred mismatch={.h={.bytes=(uint32_t)size},.data=bytes};check("named-worker-script-url-mismatch-is-rejected",!shared_connect_url(&w,a,&mismatch));js_free(ctx,bytes);JS_FreeValue(ctx,wrong);
 while(w.output){struct outgoing*p=w.output;w.output=p->next;output_packet_free(&w,p);}while(w.head){struct deferred*p=w.head;w.head=p->next;free(p->data);free(p);}output_ports_free(&w,w.ports);JS_FreeValue(ctx,va);JS_FreeValue(ctx,vb);JS_FreeValue(ctx,w.hooks);free(a);free(b);JS_FreeContext(ctx);JS_FreeRuntime(rt);printf("RESULT %u checks %u failures\n",checks,failures);return failures?1:0;}
#endif
