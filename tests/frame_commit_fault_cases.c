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
    // Each allocation stage is a different new commit fault, not an old case.
    for(int stage=0;stage<3;stage++){
        JSValue setup=evaluate(&child,"globalThis.c=new MessageChannel();globalThis.b=new Uint8Array([41,43,47,53]).buffer;globalThis.seen=0;c.port2.onmessage=()=>{seen++};c.port1.postMessage('old queued');true");JS_FreeValue(child.ctx,setup);
        commit_allocation_attempt=0;commit_fail_index=stage;
        JSValue failed=evaluate(&child,"(()=>{try{parent.postMessage({p:c.port2,b},'*',[b,c.port2])}catch(e){return b.byteLength===4&&new Uint8Array(b)[3]===53}return false})()");
        verify(JS_ToBool(child.ctx,failed)==1,"materialization-OOM-preserves-buffer");JS_FreeValue(child.ctx,failed);
        verify(commit_allocation_attempt==stage+1&&top.posted_count==0&&child.posted_count==1,"materialization-OOM-preserves-old-task-and-no-publication");
        commit_fail_index=-1;pump(&child);
        js_check(&child,"seen===1","materialization-OOM-old-port-and-queue-still-live");
    }
    JSValue setup=evaluate(&child,"globalThis.finalChannel=new MessageChannel();globalThis.finalBuffer=new ArrayBuffer(7);finalChannel.port2.onmessage=()=>{};finalChannel.port1.postMessage('pending');true");JS_FreeValue(child.ctx,setup);
    commit_deny_allocation=true;
    JSValue final=evaluate(&child,"parent.postMessage({p:finalChannel.port2,b:finalBuffer},'*',[finalBuffer,finalChannel.port2]);true");
    commit_deny_allocation=false;JS_SetMemoryLimit(rt,128u<<20);
    verify(!JS_IsException(final)&&JS_ToBool(child.ctx,final)==1&&!JS_HasException(child.ctx),"all-allocations-denied-final-commit-has-no-late-OOM");JS_FreeValue(child.ctx,final);
    verify(top.posted_count==1&&child.posted_count==0,"allocation-denied-commit-cancelled-old-task-and-published-new");
    js_check(&child,"finalBuffer.byteLength===0","allocation-denied-commit-completed-buffer-detach");
    pump(&top);
    // Generation changes after target graph/task allocations, just at native commit.
    setup=evaluate(&child,"globalThis.retireChannel=new MessageChannel();globalThis.retireBuffer=new ArrayBuffer(11);true");JS_FreeValue(child.ctx,setup);
    commit_retire_destination=true;
    JSValue retired=evaluate(&child,"(()=>{try{parent.postMessage(retireChannel.port1,'*',[retireBuffer,retireChannel.port1])}catch(e){return retireBuffer.byteLength===11}return false})()");
    verify(JS_ToBool(child.ctx,retired)==1&&!commit_retire_destination&&top.posted_count==0,"native-commit-late-retirement-preserves-sender");JS_FreeValue(child.ctx,retired);
    td.live=true;top.disabled=false;
    JS_FreeValue(top.ctx,tr.wrapper);JS_FreeValue(child.ctx,cr.wrapper);JS_FreeValue(top.ctx,ct.wrapper);JS_FreeValue(top.ctx,top.hooks);JS_FreeValue(child.ctx,child.hooks);
    while(top.frame_proxies){struct js_frame_proxy *p=top.frame_proxies;top.frame_proxies=p->next;JS_FreeValue(top.ctx,p->proxy);js_free(top.ctx,p);}
    JS_FreeContext(child.ctx);JS_FreeContext(top.ctx);JS_FreeRuntime(rt);
    printf("frame commit fault: %d new checks, %d failed\n",checks,failures);return failures!=0;
}
