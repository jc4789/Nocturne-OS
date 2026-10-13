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
#ifdef WORKER_BODY_NATIVE
#include "../user/libc/web/js_worker.h"
bool worker_body_parent_dispatch(JSContext *,const struct njw_header *,const void *,web_worker_requester,web_worker_canceler,void *);
static bool body_mode,body_parent_ok=true;
static struct njw_header body_header;
static uint8_t *body_wire,*post_bytes;static size_t body_at,post_size;
static char *post_headers,*post_method,*post_url,*post_referrer;static uint32_t post_id,cancel_id;
static int post_referrer_policy;
static unsigned post_requests;static bool post_present;
void web_js_prepare_bytes(JSContext *ctx,size_t bytes){(void)bytes;JS_ThrowInternalError(ctx,"Document allocator must not run in isolated Worker encoding");}
enum http_url_result http_canonical_owned_n(const char *url,size_t length,char **out,char **origin){(void)url;(void)length;*out=NULL;if(origin)*origin=NULL;check("unrelated-native-URL-resolver-must-not-run",false);return HTTP_URL_INVALID;}
static bool body_request(void *opaque,uint32_t worker,uint32_t request,const char *url,int kind,const char *method,const char *headers,const void *bytes,size_t length,const char *referrer,int referrer_policy){
    (void)opaque;body_parent_ok=body_parent_ok&&worker==7&&(kind&~NJW_LOAD_POLICY_FLAGS)==2;
    free(post_referrer);post_referrer=referrer?strdup(referrer):NULL;post_referrer_policy=referrer_policy;
    free(post_bytes);free(post_headers);free(post_method);free(post_url);
    post_bytes=malloc(length?length:1);post_headers=strdup(headers);post_method=strdup(method);post_url=strdup(url);
    if(!post_bytes||!post_headers||!post_method||!post_url)return false;
    if(length)memcpy(post_bytes,bytes,length);post_size=length;post_present=bytes!=NULL;post_id=request;post_requests++;
    return true;
}
static void body_cancel(void *opaque,uint32_t worker,uint32_t request){(void)opaque;body_parent_ok=body_parent_ok&&worker==7;cancel_id=request;}
#endif
static void input_value(unsigned op,unsigned request,JSValueConst value){
    struct input_record*r=calloc(1,sizeof*r);size_t n=0;uint8_t*p=JS_WriteObject(stream_worker->ctx,&n,value,JS_WRITE_OBJ_REFERENCE);r->bytes=sizeof(struct njw_header)+n;r->wire=malloc(r->bytes);struct njw_header h={NJW_MAGIC,op,stream_worker->generation,request,0,(uint32_t)n};memcpy(r->wire,&h,sizeof h);memcpy(r->wire+sizeof h,p,n);js_free(stream_worker->ctx,p);if(input_tail)input_tail->next=r;else input=r;input_tail=r;
}
ssize_t read(int fd,void*bytes,size_t n){(void)fd;read_calls++;if(peer_dead)return 0;
    if(!input&&awaiting_credit){JSValue amount=JS_NewFloat64(stream_worker->ctx,(double)awaiting_credit);awaiting_credit=0;input_value(NJW_CREDIT,0,amount);JS_FreeValue(stream_worker->ctx,amount);credit_reads++;}
    if(!input){errno=EAGAIN;return -1;}struct input_record*r=input;size_t take=r->bytes-r->at;if(take>n)take=n;memcpy(bytes,r->wire+r->at,take);r->at+=take;if(r->at==r->bytes){input=r->next;if(!input)input_tail=NULL;free(r->wire);free(r);}return take;
}
ssize_t write(int fd,const void*bytes,size_t n){(void)fd;if(peer_dead){errno=EPIPE;return -1;}
#ifdef WORKER_BODY_NATIVE
    if(body_mode){
        if(!body_wire){if(n!=sizeof body_header)return -1;memcpy(&body_header,bytes,n);body_wire=malloc(body_header.bytes);body_at=0;if(!body_wire)return -1;}
        else{if(n>body_header.bytes-body_at)return -1;memcpy(body_wire+body_at,bytes,n);body_at+=n;
            if(body_at==body_header.bytes){if(body_header.op==NJW_LOAD||body_header.op==NJW_CANCEL)body_parent_ok=worker_body_parent_dispatch(stream_worker->ctx,&body_header,body_wire,body_request,body_cancel,NULL)&&body_parent_ok;free(body_wire);body_wire=NULL;}}
        return (ssize_t)n;
    }
#endif
    if(!packet_remaining&&n==sizeof(struct njw_header)){struct njw_header h;memcpy(&h,bytes,sizeof h);if(h.magic==NJW_MAGIC){delivered++;packet_remaining=h.bytes;packet_charge=sizeof h+h.bytes;}}
    else if(packet_remaining){packet_remaining-=n;if(!packet_remaining){awaiting_credit+=packet_charge;packet_charge=0;}}
    if(stream_worker->in_flight>largest_flight)largest_flight=stream_worker->in_flight;return n;
}
int poll(struct n_pollfd*p,int n,int timeout){(void)timeout;int ready=0;for(int i=0;i<n;i++){p[i].revents=0;if(p[i].fd==0&&(input||awaiting_credit||peer_dead))p[i].revents|=peer_dead?N_POLLHUP:N_POLLIN;if(p[i].fd==1&&(p[i].events&N_POLLOUT))p[i].revents|=N_POLLOUT;if(p[i].revents)ready++;}return ready;}
static bool script(JSContext*c,const char*s){JSValue v=eval(c,s);bool ok=!JS_IsException(v);if(!ok){JSValue e=JS_GetException(c);const char*text=JS_ToCString(c,e);printf("EXCEPTION %s\n",text?text:"unknown");JS_FreeCString(c,text);JS_FreeValue(c,e);}JS_FreeValue(c,v);return ok;}
#ifdef WORKER_BODY_NATIVE
static JSValue body_check(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;const char *name=argc?JS_ToCString(ctx,argv[0]):NULL;check(name?name:"missing-case",argc>1&&JS_ToBool(ctx,argv[1])>0);JS_FreeCString(ctx,name);return JS_UNDEFINED;}
static bool body_jobs(struct worker *w){unsigned count=0;JSContext *ctx;int n;while((n=JS_ExecutePendingJob(w->rt,&ctx))>0){if(++count>20000)return false;}if(n<0){exception(w);return false;}return true;}
static bool body_eval(struct worker *w,const char *source){if(!script(w->ctx,source))return false;return body_jobs(w);}
static bool body_boot(struct worker *w,const char *url,const char *origin){
    JSContext *ctx=w->ctx;JSValue g=JS_GetGlobalObject(ctx),host=JS_NewObject(ctx);
    const JSCFunctionListEntry functions[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),JS_CFUNC_DEF("safetyRequired",1,native_safety_required),JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("portSend",6,native_port_send),JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("close",0,native_close),JS_CFUNC_DEF("url",0,native_context_url),JS_CFUNC_DEF("securityURL",0,native_security_url),JS_CFUNC_DEF("referrerPolicy",0,native_referrer_policy),JS_CFUNC_DEF("eval",2,native_eval),JS_CFUNC_DEF("request",6,native_request),JS_CFUNC_DEF("cancel",1,native_cancel)};
    JS_SetPropertyFunctionList(ctx,host,functions,sizeof functions/sizeof *functions);web_js_encoding_init_isolated(ctx,host);web_js_navigator_init(ctx,host);web_js_crypto_init(ctx,host);
    JS_SetPropertyStr(ctx,g,"__workerHost",host);JS_SetPropertyStr(ctx,g,"check",JS_NewCFunction(ctx,body_check,"check",2));
    w->hooks=JS_Eval(ctx,js_worker_runtime,sizeof js_worker_runtime-1,"product-worker-bootstrap",JS_EVAL_TYPE_GLOBAL);JS_FreeValue(ctx,g);
    if(JS_IsException(w->hooks)){exception(w);return false;}
    JSValue start=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,start,0,JS_NewInt32(ctx,200));JS_SetPropertyUint32(ctx,start,1,JS_NewString(ctx,url));JS_SetPropertyUint32(ctx,start,2,JS_NewString(ctx,""));JS_SetPropertyUint32(ctx,start,3,JS_NewArrayBufferCopy(ctx,(const uint8_t *)"",0));JS_SetPropertyUint32(ctx,start,4,JS_NewString(ctx,""));JS_SetPropertyUint32(ctx,start,6,JS_TRUE);JS_SetPropertyUint32(ctx,start,NJW_START_CREATOR_POLICY,JS_NewString(ctx,""));JS_SetPropertyUint32(ctx,start,NJW_START_INHERIT_POLICY,JS_TRUE);JS_SetPropertyUint32(ctx,start,NJW_START_SHARED,JS_FALSE);JS_SetPropertyUint32(ctx,start,NJW_START_CREATOR_ORIGIN,JS_NewString(ctx,origin));JS_SetPropertyUint32(ctx,start,NJW_START_CREATOR_REFERRER_POLICY,JS_NewInt32(ctx,3));
    bool ok=worker_policy_start(w,start);if(ok){JSValue result=hook(w,"start",1,&start);ok=!JS_IsException(result);JS_FreeValue(ctx,result);}JS_FreeValue(ctx,start);if(!ok)exception(w);return ok;
}
static void body_loaded(struct worker *w,uint32_t id){JSContext *ctx=w->ctx;JSValue record=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,record,0,JS_NewInt32(ctx,200));JS_SetPropertyUint32(ctx,record,1,JS_NewString(ctx,post_url));JS_SetPropertyUint32(ctx,record,2,JS_NewString(ctx,post_headers));JS_SetPropertyUint32(ctx,record,3,JS_NewArrayBufferCopy(ctx,post_bytes,post_size));JS_SetPropertyUint32(ctx,record,4,JS_NewString(ctx,""));JSValue args[]={JS_NewUint32(ctx,id),record,JS_NewInt32(ctx,0)},result=hook(w,"loaded",3,args);if(JS_IsException(result))exception(w);JS_FreeValue(ctx,result);for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,args[i]);body_jobs(w);}
static int body_cases(void){setvbuf(stdout,NULL,_IONBF,0);body_mode=true;JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);JS_SetMaxStackSize(rt,512*1024);struct worker w={.rt=rt,.ctx=ctx,.generation=71,.established=true,.until=UINT64_MAX,.hooks=JS_UNDEFINED};stream_worker=&w;JS_SetContextOpaque(ctx,&w);trusted_native_init(ctx);
    check("product-blob-worker-starts-with-native-creator-authority",body_boot(&w,"blob:https://fixture.test/owned-id","https://fixture.test"));if(JS_HasException(ctx)||JS_IsException(w.hooks))goto finish;
    JSValue g=JS_GetGlobalObject(ctx);JS_SetPropertyStr(ctx,g,"hooks",JS_DupValue(ctx,w.hooks));JS_FreeValue(ctx,g);
    check("worker-new-body-script",body_eval(&w,"check('blob-script-location-is-preserved',location.protocol==='blob:'&&location.href==='blob:https://fixture.test/owned-id');check('blob-creator-secure-subtle',isSecureContext===true&&typeof crypto.subtle==='object');globalThis.cryptoDone=false;crypto.subtle.importKey('raw',new Uint8Array(16),'AES-GCM',false,['encrypt','decrypt']).then(async k=>{const p=new Uint8Array([0,255,1,128]);const c=await crypto.subtle.encrypt({name:'AES-GCM',iv:new Uint8Array(12)},k,p);const b=new Uint8Array(await crypto.subtle.decrypt({name:'AES-GCM',iv:new Uint8Array(12)},k,c));check('blob-worker-real-native-AES-roundtrip',b.length===4&&b[1]===255&&b[3]===128);cryptoDone=true;});globalThis.responseDone=false;globalThis.p=fetch('https://fixture.test/echo',{method:'post',body:(()=>{const f=new FormData();f.append('text','日本語');f.append('binary',new File([new Uint8Array([0,255,128,13,10])],'raw.bin',{type:'application/octet-stream'}));return f;})()}).then(async r=>{const cloned=r.clone();const f=await r.formData();check('body-formData-consumed-once',r.bodyUsed&&f.get('text')==='日本語');const b=new Uint8Array(await f.get('binary').arrayBuffer());check('binary-multipart-not-textified',f.get('binary') instanceof File&&f.get('binary').name==='raw.bin'&&b.length===5&&b[0]===0&&b[1]===255&&b[2]===128);const other=await cloned.formData();check('clone-tee-retains-distinct-consumption',other.get('text')==='日本語'&&cloned.bodyUsed);let rejected=false;try{await r.formData();}catch(e){rejected=e instanceof TypeError;}check('second-body-consumption-rejects',rejected);responseDone=true;});"));
    check("actual-private-binary-POST-reaches-parent-requester",body_parent_ok&&post_requests==1&&post_present&&!strcmp(post_method,"POST")&&strstr(post_headers,"multipart/form-data; boundary=")&&post_size>5);
    check("worker-inherits-private-referrer-policy-with-native-body-packet",post_referrer_policy==3&&post_referrer&&!strcmp(post_referrer,"blob:https://fixture.test/owned-id"));
    body_loaded(&w,post_id);check("worker-body-and-native-crypto-promises-complete",body_eval(&w,"check('completed-native-crypto-and-body',cryptoDone&&responseDone);"));
    check("worker-referrer-override-start",body_eval(&w,"globalThis.referrerDone=false;fetch('https://fixture.test/referrer',{referrer:'https://name:secret@fixture.test/from#discard',referrerPolicy:'unsafe-url'}).then(()=>{referrerDone=true;});"));
    check("worker-private-referrer-sanitized-before-parent",post_referrer&&post_referrer_policy==8&&!strcmp(post_referrer,"https://fixture.test/from"));body_loaded(&w,post_id);
    check("worker-cross-origin-referrer-start",body_eval(&w,"fetch('https://fixture.test/referrer',{referrer:'https://other.test/from',referrerPolicy:'same-origin'}).then(()=>{});"));
    check("worker-cross-origin-referrer-resolves-native-client",post_referrer&&post_referrer_policy==3&&!strcmp(post_referrer,"blob:https://fixture.test/owned-id"));body_loaded(&w,post_id);
    unsigned old=post_requests;check("worker-null-and-empty-body-start",body_eval(&w,"globalThis.emptyDone=false;fetch('https://fixture.test/empty',{method:'POST',body:new ArrayBuffer(0)}).then(r=>r.arrayBuffer()).then(b=>{check('empty-POST-response-empty-bytes',b.byteLength===0);emptyDone=true;});"));check("native-empty-POST-preserves-body-presence",body_parent_ok&&post_requests==old+1&&post_present&&post_size==0);body_loaded(&w,post_id);
    check("worker-abort-start",body_eval(&w,"globalThis.abortedDone=false;globalThis.ac=new AbortController();fetch('https://fixture.test/pending',{method:'POST',body:new Uint8Array([1,0,255]),signal:ac.signal}).then(()=>check('aborted-fetch-must-not-resolve',false),e=>{check('native-cancel-rejects-worker-promise',e.name==='AbortError');abortedDone=true;});ac.abort();"));check("actual-private-cancel-reaches-parent",body_parent_ok&&cancel_id==post_id&&post_size==3&&post_bytes[2]==255);body_loaded(&w,post_id);
    check("worker-body-failure-paths",body_eval(&w,"(async()=>{let bad=false;try{await fetch('https://fixture.test/echo',{body:new ArrayBuffer(0)});}catch(e){bad=e instanceof TypeError;}check('GET-empty-present-body-rejected',bad);const ac=new AbortController();globalThis.after=fetch('https://fixture.test/after',{signal:ac.signal}).then(async r=>{ac.abort();let rejected=false;try{await r.formData();}catch(e){rejected=e.name==='AbortError';}check('abort-after-response-rejects-consumption',rejected);});check('earlier-empty-and-abort-completed',emptyDone&&abortedDone);})();"));body_loaded(&w,post_id);
finish:free(post_bytes);free(post_headers);free(post_method);free(post_url);free(post_referrer);post_bytes=NULL;post_headers=post_method=post_url=post_referrer=NULL;html_policy_free(&w.policy_doc);free(w.policy_doc.url);free(w.security_url);JS_FreeValue(ctx,w.hooks);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    rt=JS_NewRuntime();ctx=JS_NewContext(rt);JS_SetMaxStackSize(rt,512*1024);struct worker insecure={.rt=rt,.ctx=ctx,.generation=72,.established=true,.until=UINT64_MAX,.hooks=JS_UNDEFINED};stream_worker=&insecure;JS_SetContextOpaque(ctx,&insecure);trusted_native_init(ctx);check("product-insecure-blob-worker-starts",body_boot(&insecure,"blob:https://untrusted-author-spelling/owned-id","http://not-trustworthy.test"));check("blob-author-spelling-cannot-upgrade-authority",body_eval(&insecure,"check('insecure-native-creator-hides-subtle',isSecureContext===false&&crypto.subtle===undefined);"));html_policy_free(&insecure.policy_doc);free(insecure.policy_doc.url);free(insecure.security_url);JS_FreeValue(ctx,insecure.hooks);JS_FreeContext(ctx);JS_FreeRuntime(rt);printf("RESULT %u checks %u failures (new Worker body/native crypto/parent dispatcher only; host endpoints modeled)\n",checks,failures);return failures?1:0;
}
#endif
int main(int argc,char **argv){
#ifdef WORKER_BODY_NATIVE
 if(argc>1&&!strcmp(argv[1],"--fetch-body"))return body_cases();
#else
 (void)argc;(void)argv;
#endif
JSRuntime*rt=JS_NewRuntime();JSContext*ctx=JS_NewContext(rt);JS_SetMaxStackSize(rt,1024*1024);struct worker w={0};w.ctx=ctx;w.rt=rt;w.generation=71;w.established=true;w.until=UINT64_MAX;w.hooks=JS_UNDEFINED;stream_worker=&w;JS_SetContextOpaque(ctx,&w);trusted_native_init(ctx);
 JSValue g=JS_GetGlobalObject(ctx),host=JS_NewObject(ctx);const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("trusted",4,native_trusted),JS_CFUNC_DEF("safety",6,native_safety),JS_CFUNC_DEF("safetyRequired",1,native_safety_required),JS_CFUNC_DEF("classID",1,native_class),JS_CFUNC_DEF("portSend",6,native_port_send),JS_CFUNC_DEF("portLease",3,native_port_lease),JS_CFUNC_DEF("transferCommit",1,native_transfer_commit),JS_CFUNC_DEF("detach",1,native_detach),JS_CFUNC_DEF("now",0,native_now),JS_CFUNC_DEF("send",3,native_send),JS_CFUNC_DEF("close",0,native_close)};JS_SetPropertyFunctionList(ctx,host,funcs,sizeof funcs/sizeof*funcs);JS_SetPropertyStr(ctx,g,"__workerHost",host);w.hooks=JS_Eval(ctx,js_worker_runtime,sizeof js_worker_runtime-1,"product-worker-bootstrap",JS_EVAL_TYPE_GLOBAL);check("actual-worker-bootstrap-ready",!JS_IsException(w.hooks));if(JS_IsException(w.hooks))goto done;
 JSValue author=eval(ctx,"[[0,'deferred-author'],[]]"),loaded=eval(ctx,"[200,'https://fixture.test/import.js','',new ArrayBuffer(0),'']");input_value(NJW_MESSAGE,0,author);input_value(NJW_LOADED,999,loaded);JS_FreeValue(ctx,author);JS_FreeValue(ctx,loaded);
 check("producer-completes-without-peer-callback-reentry",script(ctx,"globalThis.incoming=0;onmessage=()=>incoming++;for(let i=0;i<2500;i++)postMessage(i);globalThis.noReentry=incoming===0;"));JSValue marker=JS_GetPropertyStr(ctx,g,"noReentry");check("author-input-remains-deferred-during-producer-task",JS_ToBool(ctx,marker)>0&&w.head&&w.head->h.op==NJW_MESSAGE&&w.head->next&&w.head->next->h.op==NJW_LOADED&&w.head->next->h.request==999);JS_FreeValue(ctx,marker);check("records-reach-peer-before-author-task-returns",delivered==2500&&!w.output&&w.output_bytes==0);check("byte-credit-applies-backpressure-without-dropping-records",credit_reads>0&&read_calls>0&&largest_flight<=NJW_TRANSPORT_WINDOW&&!w.closed);
 while(w.head){struct deferred*d=w.head;w.head=d->next;w.queued-=d->h.bytes;if(d->h.op==NJW_MESSAGE){JSValue packet=JS_ReadObject(ctx,d->data,d->h.bytes,JS_READ_OBJ_REFERENCE),a[]={packet,JS_NewInt32(ctx,NJW_MESSAGE),JS_NewInt32(ctx,0),JS_NewInt32(ctx,0)};JSValue v=hook(&w,"receive",4,a);JS_FreeValue(ctx,v);for(unsigned i=0;i<4;i++)JS_FreeValue(ctx,a[i]);}deferred_free(d);}w.tail=NULL;marker=JS_GetPropertyStr(ctx,g,"incoming");int32_t count=0;JS_ToInt32(ctx,&count,marker);JS_FreeValue(ctx,marker);check("deferred-author-message-runs-once-at-checkpoint",count==1);
 struct deferred ack={0};JSValue amount=JS_NewFloat64(ctx,(double)w.in_flight);ack.data=JS_WriteObject(ctx,(size_t*)&awaiting_credit,amount,JS_WRITE_OBJ_REFERENCE);ack.h.bytes=(uint32_t)awaiting_credit;JS_FreeValue(ctx,amount);check("private-credit-does-not-repeat-transfer-commit",transport_credit(&w,&ack)&&w.in_flight==0);js_free(ctx,ack.data);awaiting_credit=0;
 w.in_flight=NJW_TRANSPORT_WINDOW;awaiting_credit=NJW_TRANSPORT_WINDOW;check("credit-wait-preserves-single-transfer-commit",script(ctx,"const moved=new ArrayBuffer(8);postMessage(moved,[moved]);if(moved.byteLength!==0)throw new Error('transfer not committed');")&&!w.closed);awaiting_credit=0;
 w.in_flight=0;unsigned old=delivered;check("larger-than-copy-quantum-record-completes-during-task",script(ctx,"const large=new ArrayBuffer(100000);postMessage(large,[large]);if(large.byteLength!==0)throw new Error('large transfer not committed');")&&delivered==old+1&&!w.output&&!packet_remaining&&!w.in_flight);
 w.in_flight=NJW_TRANSPORT_WINDOW;peer_dead=true;check("consumer-death-breaks-native-credit-wait",!output_wait(&w,NULL,1)&&w.closed);
done:while(input){struct input_record*r=input;input=r->next;free(r->wire);free(r);}while(w.output){struct outgoing*p=w.output;w.output=p->next;output_packet_free(&w,p);}while(w.head){struct deferred*d=w.head;w.head=d->next;deferred_free(d);}output_ports_free(&w,w.ports);JS_FreeValue(ctx,w.hooks);JS_FreeValue(ctx,g);JS_FreeContext(ctx);JS_FreeRuntime(rt);printf("RESULT %u checks %u failures\n",checks,failures);return failures?1:0;}
