/* Actual product bootstrap/native publication during one uninterrupted JS
 * producer task, with generation-bound transport credits and deferred input.
 * Endpoint I/O is modeled: this is not QEMU or real-site acceptance. */
#define WORKER_STREAM_MODEL
#define SHARED_SERVER_SUPPORT_ONLY
#include "sharedworker_server_hosttest.c"
struct input_record {struct input_record*next;uint8_t*wire;size_t bytes,at;};
static struct input_record*input,*input_tail;
static struct worker*stream_worker;
static size_t awaiting_credit,largest_flight;
static unsigned delivered,credit_reads,read_calls;
static size_t packet_remaining,packet_charge;
static bool peer_dead;
static void input_value(unsigned op,unsigned request,JSValueConst value){
    struct input_record*r=calloc(1,sizeof*r);size_t n=0;uint8_t*p=JS_WriteObject(stream_worker->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);r->bytes=sizeof(struct njw_header)+n;r->wire=malloc(r->bytes);struct njw_header h={NJW_MAGIC,op,stream_worker->generation,request,0,(uint32_t)n};memcpy(r->wire,&h,sizeof h);memcpy(r->wire+sizeof h,p,n);js_free(stream_worker->ctx,p);if(input_tail)input_tail->next=r;else input=r;input_tail=r;
}
ssize_t read(int fd,void*bytes,size_t n){(void)fd;read_calls++;if(peer_dead)return 0;
    if(!input&&awaiting_credit){JSValue amount=JS_NewFloat64(stream_worker->ctx,(double)awaiting_credit);awaiting_credit=0;input_value(NJW_CREDIT,0,amount);JS_FreeValue(stream_worker->ctx,amount);credit_reads++;}
    if(!input){errno=EAGAIN;return -1;}struct input_record*r=input;size_t take=r->bytes-r->at;if(take>n)take=n;memcpy(bytes,r->wire+r->at,take);r->at+=take;if(r->at==r->bytes){input=r->next;if(!input)input_tail=NULL;free(r->wire);free(r);}return take;
}
ssize_t write(int fd,const void*bytes,size_t n){(void)fd;if(peer_dead){errno=EPIPE;return -1;}
    if(!packet_remaining&&n==sizeof(struct njw_header)){struct njw_header h;memcpy(&h,bytes,sizeof h);if(h.magic==NJW_MAGIC){delivered++;packet_remaining=h.bytes;packet_charge=sizeof h+h.bytes;}}
    else if(packet_remaining){packet_remaining-=n;if(!packet_remaining){awaiting_credit+=packet_charge;packet_charge=0;}}
    if(stream_worker->in_flight>largest_flight)largest_flight=stream_worker->in_flight;return n;
}
int poll(struct n_pollfd*p,int n,int timeout){(void)timeout;int ready=0;for(int i=0;i<n;i++){p[i].revents=0;if(p[i].fd==0&&(input||awaiting_credit||peer_dead))p[i].revents|=peer_dead?N_POLLHUP:N_POLLIN;if(p[i].fd==1&&(p[i].events&N_POLLOUT))p[i].revents|=N_POLLOUT;if(p[i].revents)ready++;}return ready;}
static bool script(JSContext*c,const char*s){JSValue v=eval(c,s);bool ok=!JS_IsException(v);if(!ok){JSValue e=JS_GetException(c);const char*text=JS_ToCString(c,e);printf("EXCEPTION %s\n",text?text:"unknown");JS_FreeCString(c,text);JS_FreeValue(c,e);}JS_FreeValue(c,v);return ok;}
int main(void){JSRuntime*rt=JS_NewRuntime();JSContext*ctx=JS_NewContext(rt);JS_SetMaxStackSize(rt,1024*1024);struct worker w={0};w.ctx=ctx;w.rt=rt;w.generation=71;w.established=true;w.until=UINT64_MAX;w.hooks=JS_UNDEFINED;stream_worker=&w;JS_SetContextOpaque(ctx,&w);trusted_native_init(ctx);
 JSValue g=JS_GetGlobalObject(ctx),host=JS_NewObject(ctx);const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),JS_CFUNC_DEF("safetyRequired",1,native_safety_required),JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("portSend",6,native_port_send),JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("close",0,native_close)};JS_SetPropertyFunctionList(ctx,host,funcs,sizeof funcs/sizeof*funcs);JS_SetPropertyStr(ctx,g,"__workerHost",host);w.hooks=JS_Eval(ctx,js_worker_runtime,sizeof js_worker_runtime-1,"product-worker-bootstrap",JS_EVAL_TYPE_GLOBAL);check("actual-worker-bootstrap-ready",!JS_IsException(w.hooks));if(JS_IsException(w.hooks))goto done;
 JSValue author=eval(ctx,"[[0,'deferred-author'],[]]"),loaded=eval(ctx,"[200,'https://fixture.test/import.js','',new ArrayBuffer(0),'']");input_value(NJW_MESSAGE,0,author);input_value(NJW_LOADED,999,loaded);JS_FreeValue(ctx,author);JS_FreeValue(ctx,loaded);
 check("producer-completes-without-peer-callback-reentry",script(ctx,"globalThis.incoming=0;onmessage=()=>incoming++;for(let i=0;i<2500;i++)postMessage(i);globalThis.noReentry=incoming===0;"));JSValue marker=JS_GetPropertyStr(ctx,g,"noReentry");check("author-input-remains-deferred-during-producer-task",JS_ToBool(ctx,marker)>0&&w.head&&w.head->h.op==NJW_MESSAGE&&w.head->next&&w.head->next->h.op==NJW_LOADED&&w.head->next->h.request==999);JS_FreeValue(ctx,marker);check("records-reach-peer-before-author-task-returns",delivered==2500&&!w.output&&w.output_bytes==0);check("byte-credit-applies-backpressure-without-dropping-records",credit_reads>0&&read_calls>0&&largest_flight<=NJW_TRANSPORT_WINDOW&&!w.closed);
 while(w.head){struct deferred*d=w.head;w.head=d->next;w.queued-=d->h.bytes;if(d->h.op==NJW_MESSAGE){JSValue packet=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE),a[]={packet,JS_NewInt32(ctx,NJW_MESSAGE),JS_NewInt32(ctx,0),JS_NewInt32(ctx,0)};JSValue v=hook(&w,"receive",4,a);JS_FreeValue(ctx,v);for(unsigned i=0;i<4;i++)JS_FreeValue(ctx,a[i]);}deferred_free(d);}w.tail=NULL;marker=JS_GetPropertyStr(ctx,g,"incoming");int32_t count=0;JS_ToInt32(ctx,&count,marker);JS_FreeValue(ctx,marker);check("deferred-author-message-runs-once-at-checkpoint",count==1);
 struct deferred ack={0};JSValue amount=JS_NewFloat64(ctx,(double)w.in_flight);ack.data=JS_WriteObject(ctx,(size_t*)&awaiting_credit,amount,JS_WRITE_OBJ_REFERENCE);ack.h.bytes=(uint32_t)awaiting_credit;JS_FreeValue(ctx,amount);check("private-credit-does-not-repeat-transfer-commit",transport_credit(&w,&ack)&&w.in_flight==0);js_free(ctx,ack.data);awaiting_credit=0;
 w.in_flight=NJW_TRANSPORT_WINDOW;awaiting_credit=NJW_TRANSPORT_WINDOW;check("credit-wait-preserves-single-transfer-commit",script(ctx,"const moved=new ArrayBuffer(8);postMessage(moved,[moved]);if(moved.byteLength!==0)throw new Error('transfer not committed');")&&!w.closed);awaiting_credit=0;
 w.in_flight=0;unsigned old=delivered;check("larger-than-copy-quantum-record-completes-during-task",script(ctx,"const large=new ArrayBuffer(100000);postMessage(large,[large]);if(large.byteLength!==0)throw new Error('large transfer not committed');")&&delivered==old+1&&!w.output&&!packet_remaining&&!w.in_flight);
 w.in_flight=NJW_TRANSPORT_WINDOW;peer_dead=true;check("consumer-death-breaks-native-credit-wait",!output_wait(&w,NULL,1)&&w.closed);
done:while(input){struct input_record*r=input;input=r->next;free(r->wire);free(r);}while(w.output){struct outgoing*p=w.output;w.output=p->next;output_packet_free(&w,p);}while(w.head){struct deferred*d=w.head;w.head=d->next;deferred_free(d);}output_ports_free(&w,w.ports);JS_FreeValue(ctx,w.hooks);JS_FreeValue(ctx,g);JS_FreeContext(ctx);JS_FreeRuntime(rt);printf("RESULT %u checks %u failures\n",checks,failures);return failures?1:0;}
