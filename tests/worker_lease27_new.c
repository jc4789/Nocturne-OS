/* 新27固有: 実public Window port＋実native capture/close publication。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <quickjs.h>
#include "../user/libc/web/js_worker.h"
#include "../user/include/js_worker_wire.h"
#include "../user/libc/web/js_worker_transfer.h"
#include "../build/goal-20261009/worker-lease27-new-once/native-extract.h"
static web_workers window_workers;static struct child child_record;
static unsigned assertions,failures;static JSContext *context;
static void check(const char *label,bool ok){assertions++;printf("%s %s\n",ok?"PASS":"FAIL",label);if(!ok)failures++;}
static void clear_exception(JSContext *ctx){if(JS_HasException(ctx)){JSValue e=JS_GetException(ctx);JS_FreeValue(ctx,e);}}
static JSValue host_class(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;return JS_NewUint32(c,n?JS_GetClassID(a[0]):0);}
static JSValue host_frame(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;const char*s=n?JS_ToCString(c,a[0]):NULL;JSValue r=JS_UNDEFINED;if(s&&!strcmp(s,"transferGeneration"))r=JS_NewUint32(c,271);else if(s&&!strcmp(s,"transferCommit"))r=worker_transfer_commit(c,a[1],!child_record.stopped);else if(s&&!strcmp(s,"messageBrand"))r=JS_TRUE;JS_FreeCString(c,s);return r;}
static JSValue host_worker(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;uint32_t op=0,id=0,port=0,creator=0,lease=0;if(n<6||JS_ToUint32(c,&op,a[0])<0||JS_ToUint32(c,&id,a[1])<0||JS_ToUint32(c,&port,a[4])<0||JS_ToUint32(c,&creator,a[5])<0)return JS_EXCEPTION;
 if(op==8)return web_worker_capture(&window_workers,id,port,creator);if(op==9){web_worker_release_capture(&window_workers,port);return JS_UNDEFINED;}
 if(n>6&&JS_ToUint32(c,&lease,a[6])<0)return JS_EXCEPTION;
 web_worker_publication*p=web_worker_prepare_captured(&window_workers,id,op==6?NJW_PORT_MESSAGE:NJW_PORT_CLOSE,port,creator,a[2],lease);if(!p)return JS_EXCEPTION;JSValue r=worker_transfer_commit(c,a[3],!child_record.stopped);if(JS_IsException(r))web_worker_abort(p);else web_worker_publish(p);return r;
}
static JSValue host_check(JSContext*c,JSValueConst t,int n,JSValueConst*a){(void)t;const char*s=n?JS_ToCString(c,a[0]):NULL;check(s?s:"unnamed",n>1&&JS_ToBool(c,a[1])>0);JS_FreeCString(c,s);return JS_UNDEFINED;}
static void packets_clear(void){while(child_record.tx){struct packet*p=child_record.tx;child_record.tx=p->next;window_workers.queued-=sizeof p->h+p->h.bytes;broker_packet_free(p);}child_record.tail=NULL;}
static unsigned ops(unsigned *out){unsigned n=0;for(struct packet*p=child_record.tx;p&&n<8;p=p->next)out[n++]=p->h.op;return n;}
static bool eval(const char*code){JSValue r=JS_Eval(context,code,strlen(code),"lease27",JS_EVAL_TYPE_GLOBAL);bool ok=!JS_IsException(r);if(!ok){JSValue e=JS_GetException(context);const char*s=JS_ToCString(context,e);printf("EXCEPTION %s\n",s?s:"unknown");JS_FreeCString(context,s);JS_FreeValue(context,e);failures++;}JS_FreeValue(context,r);return ok;}
static void add_pair(uint32_t id){struct broker_pair*p=calloc(1,sizeof*p);p->id=id;p->creator=1;p->next=child_record.ports;child_record.ports=p;}
int main(void){setvbuf(stdout,NULL,_IONBF,0);JSRuntime *rt=JS_NewRuntime();context=JS_NewContext(rt);JS_SetMaxStackSize(rt,1024*1024);window_workers.ctx=context;window_workers.generation=271;window_workers.children=&child_record;child_record.id=1;
 JSValue global=JS_GetGlobalObject(context),host=JS_NewObject(context);const JSCFunctionListEntry funcs[]={JS_CFUNC_DEF("classID",1,host_class),JS_CFUNC_DEF("frame",2,host_frame),JS_CFUNC_DEF("worker",7,host_worker)};JS_SetPropertyFunctionList(context,host,funcs,sizeof funcs/sizeof*funcs);JS_SetPropertyStr(context,global,"__host",host);JS_SetPropertyStr(context,global,"__check",JS_NewCFunction(context,host_check,"check",2));JS_FreeValue(context,global);
 FILE*f=fopen("build/goal-20261009/worker-lease27-new-once/public.js","rb");if(!f)return 2;fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);char*source=malloc(length+1);fread(source,1,length,f);fclose(f);source[length]=0;if(!eval(source)){free(source);goto done;}free(source);
 unsigned order[8],n;
 add_pair(1);if(!eval("globalThis.p=makeImported(1);globalThis.b=new ArrayBuffer(5);p.postMessage({get value(){p.close();return b;}},[b]);__check('public-getter-close-delivers-and-detaches',b.byteLength===0);"))goto done;
 n=ops(order);check("native-captured-message-published-before-deferred-close",n==2&&order[0]==NJW_PORT_MESSAGE&&order[1]==NJW_PORT_CLOSE);check("source-getter-close-leases-all-released",window_workers.leases==NULL&&child_record.ports->pins==0);packets_clear();
 add_pair(2);if(!eval("globalThis.p=makeImported(2);globalThis.b=new ArrayBuffer(6);p.postMessage(b,{get transfer(){p.close();return [b];}});__check('public-options-getter-close-keeps-captured-target',b.byteLength===0);"))goto done;n=ops(order);check("options-close-control-order-is-message-then-close",n==2&&order[0]==NJW_PORT_MESSAGE&&order[1]==NJW_PORT_CLOSE);packets_clear();
 add_pair(3);if(!eval("globalThis.p=makeImported(3);globalThis.b=new ArrayBuffer(7);let caught=false;try{p.postMessage({get value(){p.close();throw new Error('author');}},[b]);}catch(e){caught=e.message==='author';}__check('serializer-throw-keeps-real-buffer-attached',caught&&b.byteLength===7);"))goto done;n=ops(order);check("serializer-failure-releases-lease-and-only-close",n==1&&order[0]==NJW_PORT_CLOSE&&!window_workers.leases);packets_clear();
 add_pair(4);if(!eval("globalThis.p=makeImported(4);p.postMessage({get value(){p.postMessage('inner');p.close();return 'outer';}});"))goto done;n=ops(order);check("nested-capture-publishes-two-messages-before-close",n==3&&order[0]==NJW_PORT_MESSAGE&&order[1]==NJW_PORT_MESSAGE&&order[2]==NJW_PORT_CLOSE);packets_clear();
 add_pair(5);if(!eval("globalThis.p=makeImported(5);globalThis.received=0;p.onmessage=e=>received=e.data;enqueueImported(5,17);p.close();pumpTask();__check('own-close-preserves-already-queued-message-task',received===17);"))goto done;packets_clear();
 add_pair(6);JSValue token=web_worker_capture(&window_workers,1,6,1);uint32_t captured=0;JS_ToUint32(context,&captured,token);JS_FreeValue(context,token);web_worker_publication*close=web_worker_prepare(&window_workers,1,NJW_PORT_CLOSE,6,1,JS_NULL);if(close)web_worker_publish(close);struct broker_pair*pair=broker_find(&child_record,6,1);size_t held=window_workers.queued;check("native-held-close-is-charged-before-release",pair&&pair->held_close&&held>0&&!child_record.tx);web_worker_cancel_captures(&window_workers);check("outer-task-cancel-publishes-owned-close-without-accounting-double-charge",!window_workers.leases&&child_record.tx&&window_workers.queued==held);packets_clear();
 add_pair(7);token=web_worker_capture(&window_workers,1,7,1);JS_ToUint32(context,&captured,token);JS_FreeValue(context,token);pair=broker_find(&child_record,7,1);pair->closed=pair->sealed=true;web_worker_publication*ack=web_worker_prepare(&window_workers,1,NJW_PORT_DRAINED,7,1,JS_NULL);if(ack)web_worker_publish(ack);broker_retire(&child_record);check("native-remote-seal-keeps-pinned-tombstone-and-ack",broker_find(&child_record,7,1)&&pair->held_ack&&!child_record.tx);web_worker_release_capture(&window_workers,captured);check("last-pin-publishes-seal-ack-and-reclaims-tombstone",!broker_find(&child_record,7,1)&&child_record.tx&&child_record.tx->h.op==NJW_PORT_DRAINED);packets_clear();
 token=web_worker_capture(&window_workers,1,999,1);check("unknown-pair-cannot-acquire-native-target-lease",JS_IsException(token));clear_exception(context);JS_FreeValue(context,token);
 add_pair(8);web_worker_publication*bad=web_worker_prepare_captured(&window_workers,1,NJW_PORT_MESSAGE,8,1,JS_NULL,999);check("forged-or-retired-capture-rejected-before-publication",!bad&&!child_record.tx);clear_exception(context);
done:packets_clear();broker_free(child_record.ports);child_record.ports=NULL;web_worker_cancel_captures(&window_workers);JS_FreeContext(context);JS_FreeRuntime(rt);printf("RESULT %u assertions %u failures\n",assertions,failures);return failures?1:0;}
