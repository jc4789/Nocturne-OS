/* New same-runtime transfer boundary only. No previous message cases run. */
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
    load(&top,"build/goal-20261009/frame-transfer-bindings.js");load(&child,"build/goal-20261009/frame-transfer-bindings.js");
    JSValue token=wrap(&top,&ct),proxy=custom_element_hook(&top,"frameWindowProxy",1,&token);JS_FreeValue(top.ctx,token);
    JSValue global=JS_GetGlobalObject(top.ctx);JS_SetPropertyStr(top.ctx,global,"childWindow",proxy);JS_FreeValue(top.ctx,global);
    const char *poison="globalThis.leaks=0;for(const k of ['queue','scheduledRealm','committed','realm'])Object.defineProperty(Object.prototype,k,{configurable:true,set(v){leaks++},get(){leaks++;return undefined}});for(const k of ['0','1','2'])Object.defineProperty(Array.prototype,k,{configurable:true,set(v){leaks++},get(){leaks++;return undefined}});true";
    js_check(&top,poison,"target-prototype-setters-installed");js_check(&child,poison,"sender-prototype-setters-installed");
    js_check(&child,"globalThis.c=new MessageChannel();globalThis.b=new Uint8Array([29]).buffer;c.port1.postMessage({queued:[31]});parent.postMessage({p:c.port2,b},'*',[c.port2,b]);b.byteLength===0&&leaks===0","private-records-never-prototype-exposed");
    pump(&top);
    js_check(&top,"globalThis.p=received[0].data.p;globalThis.last=null;p.onmessage=e=>{last=e.data};received[0].ports[0]===p&&received[0].data.b instanceof ArrayBuffer&&leaks===0","target-factory-brands-with-poison");pump(&top);
    js_check(&top,"last.queued instanceof Array&&last.queued[0]===31&&leaks===0","queued-clone-has-no-index-setter-leak");
    js_check(&child,"globalThis.reply=null;c.port1.onmessage=e=>{reply=e.data};true","peer-enable-with-poison");
    js_check(&top,"p.postMessage({reply:[37]});leaks===0","target-port-send-with-poison");pump(&child);
    js_check(&child,"reply.reply instanceof Array&&reply.reply[0]===37&&leaks===0","source-private-slots-safe-on-reply");
    js_check(&top,"childWindow.postMessage(p,'*',[p]);leaks===0","poisoned-retransfer-commit");pump(&child);
    js_check(&child,"received[0].data instanceof MessagePort&&leaks===0","poisoned-retransfer-target-brand");
    const char *unpoison="for(const k of ['queue','scheduledRealm','committed','realm'])delete Object.prototype[k];for(const k of ['0','1','2'])delete Array.prototype[k];leaks===0";
    js_check(&top,unpoison,"target-zero-private-capability-leaks");js_check(&child,unpoison,"source-zero-private-capability-leaks");
    JS_FreeValue(top.ctx,tr.wrapper);JS_FreeValue(child.ctx,cr.wrapper);JS_FreeValue(top.ctx,ct.wrapper);JS_FreeValue(top.ctx,top.hooks);JS_FreeValue(child.ctx,child.hooks);
    while(top.frame_proxies){struct js_frame_proxy *p=top.frame_proxies;top.frame_proxies=p->next;JS_FreeValue(top.ctx,p->proxy);js_free(top.ctx,p);}
    JS_FreeContext(child.ctx);JS_FreeContext(top.ctx);JS_FreeRuntime(rt);
    printf("frame transfers poison: %d new checks, %d failed\n",checks,failures);return failures!=0;
}
