/* 初回26原runtimeとは非重複：native broker buffer/ledger allocator所有のみ。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <quickjs.h>
#include "../user/libc/web/js_worker.h"
#include "../user/include/js_worker_wire.h"
#include "../user/libc/web/js_worker_transfer.h"
#include "../build/goal-20261009/worker-broker26-new-once/native-extract.h"
union allocation{size_t n;long double alignment;};struct backend{size_t count;};
static void *am(JSMallocState*s,size_t n){if(!n||n>SIZE_MAX-sizeof(union allocation))return NULL;union allocation*p=malloc(n+sizeof*p);if(!p)return NULL;p->n=n;((struct backend*)s->opaque)->count++;s->malloc_count++;s->malloc_size+=n+sizeof*p;return p+1;}
static void af(JSMallocState*s,void*v){if(!v)return;union allocation*p=(union allocation*)v-1;((struct backend*)s->opaque)->count--;s->malloc_count--;s->malloc_size-=p->n+sizeof*p;free(p);}
static void *ar(JSMallocState*s,void*v,size_t n){if(!v)return am(s,n);if(!n){af(s,v);return NULL;}if(n>SIZE_MAX-sizeof(union allocation))return NULL;union allocation*p=(union allocation*)v-1;size_t old=p->n;union allocation*q=realloc(p,n+sizeof*q);if(!q)return NULL;q->n=n;s->malloc_size=s->malloc_size-old+n;return q+1;}
static size_t au(const void*v){return v?((const union allocation*)v-1)->n:0;}static const JSMallocFunctions mf={am,af,ar,au};
static unsigned count,failures;static void check(const char*n,bool ok){count++;printf("%s %s\n",ok?"PASS":"FAIL",n);if(!ok)failures++;}
static JSValue eval(JSContext*c,const char*s){return JS_Eval(c,s,strlen(s),"<broker-ownership26>",JS_EVAL_TYPE_GLOBAL);}
static void clear(JSContext*c){if(JS_HasException(c))JS_FreeValue(c,JS_GetException(c));}
int main(void){setvbuf(stdout,NULL,_IONBF,0);struct backend a={0},b={0};JSRuntime*ra=JS_NewRuntime2(&mf,&a),*rb=JS_NewRuntime2(&mf,&b);JSContext*ca=JS_NewContext(ra),*cb=JS_NewContext(rb);JS_SetMaxStackSize(ra,512*1024);JS_SetMaxStackSize(rb,512*1024);
 struct child child={.id=1,.parent_port=1,.in=-1,.out=-1};web_workers w={.ctx=ca,.generation=77,.children=&child};struct worker cw={.ctx=cb,.rt=rb,.generation=77};JS_SetContextOpaque(cb,&cw);
 JSValue g=JS_GetGlobalObject(ca),envelope=eval(ca,"globalThis.buffer=new ArrayBuffer(8);globalThis.owner={owner:'source'};globalThis.plan={buffers:[buffer],writes:[{object:owner,key:'owner',value:'remote'}],cancels:[]};[new Uint8Array([17,29]).buffer,[[1,0,[]]]]");
 web_worker_publication*t=web_worker_prepare(&w,1,NJW_MESSAGE,0,NJW_WITH_PORTS,envelope);check("native-ledger-reservation-not-published-before-source-commit",t&&child.ports==NULL&&child.tx==NULL&&w.queued==0);web_worker_abort(t);
 JSValue buffer=JS_GetPropertyStr(ca,g,"buffer");size_t bytes=0;uint8_t*data=JS_GetArrayBuffer(ca,&bytes,buffer);check("native-aborted-reservation-retains-runtime-owned-source",data&&bytes==8&&child.ports==NULL&&w.queued==0);
 t=web_worker_prepare(&w,1,NJW_MESSAGE,0,NJW_WITH_PORTS,envelope);JSValue plan=JS_GetPropertyStr(ca,g,"plan"),r=worker_transfer_commit(ca,plan,true);bool ok=t&&!JS_IsException(r);JS_FreeValue(ca,r);if(ok)web_worker_publish(t);else web_worker_abort(t);check("native-source-commit-precedes-ledger-and-wire-link",ok&&child.ports&&child.ports->id==1&&child.tx&&child.tx->h.generation==77&&w.queued==sizeof(struct njw_header)+child.tx->h.bytes);
 JS_GetArrayBuffer(ca,&bytes,buffer);check("native-source-detach-is-not-a-wire-ownership-alias",JS_HasException(ca));clear(ca);
 JSValue received=child.tx?JS_ReadObject(cb,child.tx->data,child.tx->h.bytes,JS_READ_OBJ_REFERENCE):JS_EXCEPTION,copy=JS_GetPropertyUint32(cb,received,0);data=JS_GetArrayBuffer(cb,&bytes,copy);check("other-runtime-reconstructs-independent-byte-storage",data&&bytes==2&&data[0]==17&&data[1]==29);
 struct packet*p=child.tx;if(p){child.tx=child.tail=NULL;w.queued-=sizeof p->h+p->h.bytes;free(p->data);free(p);}JS_FreeValue(cb,copy);JS_FreeValue(cb,received);
 JSValue cg=JS_GetGlobalObject(cb),source=eval(cb,"globalThis.childBuffer=new ArrayBuffer(5);globalThis.childOwner={owner:'child'};globalThis.childPlan={buffers:[childBuffer],writes:[{object:childOwner,key:'owner',value:'parent'}],cancels:[]};new Uint8Array([41]).buffer"),cp=JS_GetPropertyStr(cb,cg,"childPlan"),args[]={JS_NewUint32(cb,NJW_PORT_MESSAGE),JS_NewUint32(cb,1),JS_NewUint32(cb,0),source,cp};r=native_port_send(cb,JS_UNDEFINED,5,args);check("child-native-commit-and-outgoing-publication-share-one-step",!JS_IsException(r)&&cw.output&&cw.output->h.op==NJW_PORT_MESSAGE&&cw.output->h.request==1&&cw.output->h.kind==0&&cw.output_bytes==sizeof(struct njw_header)+cw.output->h.bytes);JS_FreeValue(cb,r);
 JSValue childbuf=JS_GetPropertyStr(cb,cg,"childBuffer");JS_GetArrayBuffer(cb,&bytes,childbuf);check("child-detach-completes-before-parent-deserialization",JS_HasException(cb));clear(cb);
 received=cw.output?JS_ReadObject(ca,cw.output->data,cw.output->h.bytes,JS_READ_OBJ_REFERENCE):JS_EXCEPTION;data=JS_GetArrayBuffer(ca,&bytes,received);check("child-serialized-buffer-is-freed-by-its-own-runtime-only",data&&bytes==1&&data[0]==41);JS_FreeValue(ca,received);
 while(cw.output){struct outgoing*q=cw.output;cw.output=q->next;js_free(cb,q->data);free(q);}cw.last_output=NULL;cw.output_bytes=0;
 JSValue empty=eval(ca,"({buffers:[],writes:[],cancels:[]})");t=web_worker_prepare(&w,1,NJW_PORT_CLOSE,1,0,JS_NULL);check("close-ledger-remains-live-until-publication",t&&child.ports&&!child.ports->closed);if(t)web_worker_publish(t);check("close-publication-marks-native-route-without-freeing-queued-wire",child.ports&&child.ports->closed&&child.tx&&child.tx->h.op==NJW_PORT_CLOSE);while(child.tx){p=child.tx;child.tx=p->next;free(p->data);free(p);}child.tail=NULL;w.queued=0;
 child.stopped=true;t=web_worker_prepare(&w,1,NJW_PORT_MESSAGE,1,0,JS_NULL);check("retired-native-child-rejects-before-wire-allocation",!t&&JS_HasException(ca));clear(ca);
 broker_free(child.ports);JS_FreeValue(ca,empty);JS_FreeValue(ca,plan);JS_FreeValue(ca,buffer);JS_FreeValue(ca,envelope);JS_FreeValue(ca,g);JS_FreeValue(cb,childbuf);JS_FreeValue(cb,source);JS_FreeValue(cb,cp);JS_FreeValue(cb,cg);for(int i=0;i<3;i++)JS_FreeValue(cb,args[i]);JS_FreeContext(ca);JS_FreeContext(cb);JS_FreeRuntime(ra);JS_FreeRuntime(rb);check("distinct-runtime-allocator-teardown-has-no-live-blocks",a.count==0&&b.count==0);printf("RESULT %u assertions %u failures\n",count,failures);return failures?1:0;}
