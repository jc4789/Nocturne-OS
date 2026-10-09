static int checks,failures;
static void verify(bool value,const char *name){checks++;if(!value){failures++;printf("FAIL %s\n",name);}}
static JSValue evaluate(struct web_js_state *s,const char *code){
    s->runtime_owner->runtime_active=s;JSValue result=JS_Eval(s->ctx,code,strlen(code),"message-boundary",JS_EVAL_TYPE_GLOBAL);s->runtime_owner->runtime_active=NULL;
    if(JS_IsException(result)){JSValue e=JS_GetException(s->ctx);const char *text=JS_ToCString(s->ctx,e);printf("EXCEPTION %s\n",text?text:"");JS_FreeCString(s->ctx,text);JS_FreeValue(s->ctx,e);failures++;}return result;
}
static void js_check(struct web_js_state *s,const char *code,const char *name){JSValue result=evaluate(s,code);verify(JS_ToBool(s->ctx,result)==1,name);JS_FreeValue(s->ctx,result);}
static void pump(struct web_js_state *s){
    while(s->posted){struct js_posted_task *p=s->posted;s->posted=p->next;if(!s->posted)s->last_posted=NULL;s->posted_count--;JSValue fn=p->fn;js_free(s->ctx,p);
        s->runtime_owner->runtime_active=s;JSValue result=JS_Call(s->ctx,fn,JS_UNDEFINED,0,NULL);s->runtime_owner->runtime_active=NULL;
        if(JS_IsException(result)){JSValue e=JS_GetException(s->ctx);const char *text=JS_ToCString(s->ctx,e);printf("DELIVERY EXCEPTION %s\n",text?text:"");JS_FreeCString(s->ctx,text);JS_FreeValue(s->ctx,e);failures++;}JS_FreeValue(s->ctx,result);JS_FreeValue(s->ctx,fn);}
}
static void load(struct web_js_state *s,const char *path){
    FILE *f=fopen(path,"rb");if(!f){failures++;return;}fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *bytes=malloc(n+1);fread(bytes,1,n,f);fclose(f);bytes[n]=0;
    s->hooks=evaluate(s,bytes);free(bytes);
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();JS_SetMemoryLimit(rt,128u<<20);JS_SetMaxStackSize(rt,1u<<20);
    JS_NewClassID(&node_class);JSClassDef definition={.class_name="FixtureNativeNode"};JS_NewClass(rt,node_class,&definition);
    struct web_js_state top={.ctx=JS_NewContext(rt)},child={.ctx=JS_NewContext(rt)};
    node_t tr={0},cr={0},ct={0};web_doc td={.js=&top,.live=true,.root=&tr,.window_token=&tr,.origin="https://parent.test",.url="https://parent.test/page"};
    web_doc cd={.js=&child,.live=true,.root=&cr,.window_token=&ct,.parent=&td,.origin="https://child.test",.url="https://child.test/page"};
    tr.owner=tr.document=&td;cr.owner=cr.document=&cd;ct.owner=&td;ct.document=&cd;
    top.doc=&td;child.doc=&cd;top.runtime_owner=child.runtime_owner=&top;install(&top);install(&child);
    tr.wrapper=JS_NewObjectClass(top.ctx,node_class);cr.wrapper=JS_NewObjectClass(child.ctx,node_class);ct.wrapper=JS_NewObjectClass(top.ctx,node_class);
    JS_SetOpaque(tr.wrapper,&tr);JS_SetOpaque(cr.wrapper,&cr);JS_SetOpaque(ct.wrapper,&ct);
    load(&top,"build/goal-20261009/frame-message-bindings.js");load(&child,"build/goal-20261009/frame-message-bindings.js");
    JSValue token=wrap(&top,&ct),proxy=custom_element_hook(&top,"frameWindowProxy",1,&token);JS_FreeValue(top.ctx,token);
    JSValue global=JS_GetGlobalObject(top.ctx);JS_SetPropertyStr(top.ctx,global,"childWindow",proxy);JS_FreeValue(top.ctx,global);
    js_check(&child,"parent.postMessage(['esms',true,new Map([['x',[7]]])],'*');true","reddit-parent-send");
    verify(top.posted_count==1&&child.posted_count==0,"target-native-queue");
    pump(&top);
    js_check(&top,"received.length===1&&received[0].data instanceof Array&&received[0].data[2] instanceof Map&&received[0].data[2].get('x') instanceof Array&&received[0].origin==='https://child.test'&&received[0].source===childWindow","target-clone-realm-source-origin");
    js_check(&top,"received[0].source.postMessage({reply:[1,2]},'https://child.test/path');true","source-reply-send");pump(&child);
    js_check(&child,"received.length===1&&received[0].data.reply instanceof Array&&received[0].source===parent&&received[0].origin==='https://parent.test'","source-reply-delivery");
    js_check(&child,"parent.postMessage('wrong','https://else.test');parent.postMessage('default');true","origin-filter-enqueue");pump(&top);
    js_check(&top,"received.length===1","wrong-and-default-origin-drop");
    js_check(&child,"(()=>{try{postMessage.call({},'x','*')}catch(e){return e.name==='TypeError'}return false})()","fake-receiver-denied");
    js_check(&child,"(()=>{try{postMessage.call(new Proxy(parent,{}),'x','*')}catch(e){return e.name==='TypeError'}return false})()","proxy-lookalike-denied");
    js_check(&child,"(()=>{try{void parent.document}catch(e){return e.name==='SecurityError'}return false})()","cross-document-still-denied");
    js_check(&child,"(()=>{try{new MessageEvent('message',{source:{}})}catch(e){return e.name==='TypeError'}return false})()","message-source-brand-denied");
    js_check(&child,"new MessageEvent('message',{source:parent}).source===parent","registered-source-brand");
    js_check(&child,"(()=>{const b=new ArrayBuffer(8);try{parent.postMessage(b,'*',[b])}catch(e){return e.name==='NotSupportedError'&&b.byteLength===8}return false})()","external-transfer-explicit-no-detach");
    js_check(&child,"(()=>{const c=new MessageChannel();try{parent.postMessage('port','*',[c.port1])}catch(e){return e.name==='NotSupportedError'}return false})()","external-port-transfer-explicit");
    js_check(&child,"(()=>{try{parent.postMessage(()=>{},'*')}catch(e){return e.name==='DataCloneError'}return false})()","uncloneable-no-publication");verify(top.posted_count==0,"failure-queue-empty");
    js_check(&child,"(()=>{try{parent.postMessage('x','not a url')}catch(e){return e.name==='SyntaxError'}return false})()","invalid-target-origin");
    js_check(&child,"parent.postMessage('origin changed','https://parent.test');true","dispatch-origin-recheck-enqueue");td.origin="https://moved.test";pump(&top);td.origin="https://parent.test";
    js_check(&top,"received.length===1","dispatch-origin-rechecked");
    js_check(&child,"parent.postMessage('retired','*');true","retirement-enqueue");td.live=false;top.disabled=true;pump(&top);td.live=true;top.disabled=false;
    js_check(&top,"received.length===1","retired-target-does-not-dispatch");
    cd.origin="https://parent.test";
    js_check(&child,"parent.postMessage({same:[9]});true","same-origin-default-enqueue");pump(&top);
    js_check(&top,"received.length===2&&received[1].data.same instanceof Array&&received[1].source===childWindow&&received[1].origin==='https://parent.test'","same-origin-parent-correct-source");
    js_check(&top,"frames.postMessage('self','*');true","self-proxy-enqueue");pump(&top);
    js_check(&top,"received.length===3&&received[2].source===globalThis","self-proxy-local-route");
    js_check(&child,"parent.postMessage('snapshot');true","snapshot-default-enqueue");cd.origin="https://sender-moved.test";pump(&top);
    js_check(&top,"received.length===4&&received[3].origin==='https://parent.test'&&received[3].source===childWindow","sender-origin-snapshot-not-current");
    cd.origin=td.origin="null";
    js_check(&child,"parent.postMessage('opaque default');parent.postMessage('opaque wildcard','*');true","opaque-enqueue");pump(&top);
    js_check(&top,"received.length===5&&received[4].data==='opaque wildcard'&&received[4].origin==='null'&&received[4].source===childWindow","distinct-opaque-default-denied-star-allowed");
    cd.origin="https://child.test";td.origin="https://parent.test";
    js_check(&child,"parent.postMessage('sender retired','*');true","sender-retirement-enqueue");cd.live=false;child.disabled=true;pump(&top);
    js_check(&top,"received.length===6&&received[5].source===childWindow&&received[5].source.closed&&received[5].origin==='https://child.test'","retired-sender-source-not-null");cd.live=true;child.disabled=false;
    top.posted_count=4096;js_check(&child,"(()=>{try{parent.postMessage('quota','*')}catch(e){return e.name==='RangeError'}return false})()","queue-quota-before-clone");top.posted_count=0;
    JS_FreeValue(top.ctx,tr.wrapper);JS_FreeValue(child.ctx,cr.wrapper);JS_FreeValue(top.ctx,ct.wrapper);JS_FreeValue(top.ctx,top.hooks);JS_FreeValue(child.ctx,child.hooks);
    while(top.frame_proxies){struct js_frame_proxy *p=top.frame_proxies;top.frame_proxies=p->next;JS_FreeValue(top.ctx,p->proxy);js_free(top.ctx,p);}
    JS_FreeContext(child.ctx);JS_FreeContext(top.ctx);JS_FreeRuntime(rt);
    printf("frame messages native: %d checks, %d failed\n",checks,failures);return failures!=0;
}
