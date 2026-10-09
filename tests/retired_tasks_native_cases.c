static int checks,failures,finalized,reentries;
static JSClassID token_class;
static struct web_js_state *current;
static int retired_interrupt(JSRuntime *rt,void *opaque){(void)rt;return ((struct web_js_state *)opaque)->disabled;}
static void verify(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool empty(const struct web_js_state *s){return !s->timers&&!s->timer_capacity&&!s->idle_pending&&!s->last_idle_pending&&!s->idle_runnable&&!s->last_idle_runnable&&!s->idle_count&&!s->posted&&!s->last_posted&&!s->posted_count;}
static void token_finalizer(JSRuntime *rt,JSValue value){
    (void)rt;(void)value;finalized++;
    verify(empty(current),"all-roots-unpublished-before-finalizer");
    reentries++;release_document_tasks(current);
}
static JSValue make_token(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    (void)receiver;(void)argc;(void)argv;return JS_NewObjectClass(ctx,token_class);
}
static JSValue retire(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    (void)receiver;(void)argc;(void)argv;struct web_js_state *s=state(ctx);s->disabled=true;release_document_tasks(s);return JS_UNDEFINED;
}
static JSValue evaluate(JSContext *ctx,const char *code){
    JSValue result=JS_Eval(ctx,code,strlen(code),"retired-native-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);printf("EXCEPTION %s\n",message?message:"");JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);failures++;}
    return result;
}
int main(void){
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);web_doc doc={0};
    struct web_js_state s={.ctx=ctx,.doc=&doc};doc.js=&s;doc.live=true;current=&s;JS_SetContextOpaque(ctx,&s);
    JS_NewClassID(&token_class);JSClassDef definition={.class_name="CleanupToken",.finalizer=token_finalizer};JS_NewClass(rt,token_class,&definition);
    JSValue global=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,global,"makeToken",JS_NewCFunction(ctx,make_token,"makeToken",0));
    JS_SetPropertyStr(ctx,global,"retire",JS_NewCFunction(ctx,retire,"retire",0));
    JS_SetPropertyStr(ctx,global,"timer",JS_NewCFunction(ctx,native_timer,"timer",4));
    JS_SetPropertyStr(ctx,global,"idle",JS_NewCFunction(ctx,native_idle,"idle",2));
    JS_SetPropertyStr(ctx,global,"posted",JS_NewCFunction(ctx,native_post_task,"posted",1));
    struct web_js_state partial={0};release_document_tasks(NULL);release_document_tasks(&partial);verify(empty(&partial),"failed-context-retirement");
    verify(task_context_active(&s),"active-owner");
    JSValue result=evaluate(ctx,"(()=>{const token=makeToken();const callback=()=>token;timer(0,callback,1000,[]);timer(1,callback,1000,[]);timer(2,callback,1000,[]);idle(callback,1000);idle(callback,1000);posted(callback);posted(callback);})()");JS_FreeValue(ctx,result);
    verify(s.timer_capacity==32&&s.idle_count==2&&s.posted_count==2,"pending-real-native-roots");
    // Move one idle entry to the runnable list using the actual queue helper.
    struct js_idle_task *p=take_idle(&s.idle_pending,&s.last_idle_pending,s.idle_pending->id);
    s.idle_runnable=s.last_idle_runnable=p;
    JS_RunGC(rt);verify(finalized==0,"queued-closure-root-keeps-token");
    s.disabled=true;release_document_tasks(&s);JS_RunGC(rt);
    verify(empty(&s),"retire-empty");verify(finalized==1&&reentries==1,"retire-releases-closure-and-reentry-safe");
    release_document_tasks(&s);verify(finalized==1,"repeat-retire-idempotent");
    result=evaluate(ctx,"(()=>{let n=0;for(const enqueue of [()=>timer(0,()=>{},0,[]),()=>idle(()=>{},0),()=>posted(()=>{})]){try{enqueue()}catch(e){if(e instanceof TypeError)n++;else throw e}}return n})()");
    int n=0;JS_ToInt32(ctx,&n,result);JS_FreeValue(ctx,result);verify(n==3&&empty(&s),"inactive-registration-no-new-roots");
    s.disabled=false;doc.live=false;verify(!task_context_active(&s),"nonlive-owner-denied");doc.live=true;doc.js=NULL;verify(!task_context_active(&s),"owner-generation-denied");doc.js=&s;
    // A timer delay conversion can retire the owner before publishing a slot.
    result=evaluate(ctx,"(()=>{try{timer(0,()=>{}, {valueOf(){retire();return 1}},[])}catch(e){return e instanceof TypeError}return false})()");
    verify(JS_ToBool(ctx,result)==1&&empty(&s),"timer-conversion-retirement");JS_FreeValue(ctx,result);
    s.disabled=false;
    result=evaluate(ctx,"(()=>{try{idle(()=>{}, {valueOf(){retire();return 1}})}catch(e){return e instanceof TypeError}return false})()");
    verify(JS_ToBool(ctx,result)==1&&empty(&s),"idle-conversion-retirement");JS_FreeValue(ctx,result);
    s.disabled=false;
    result=evaluate(ctx,"(()=>{const token=makeToken();const callback=()=>{retire();for(;;){void token}};timer(1,callback,1,[]);idle(()=>{},1000);posted(()=>{});})()");JS_FreeValue(ctx,result);
    JSValue dispatched=JS_DupValue(ctx,s.timers[0].fn),arguments=JS_DupValue(ctx,s.timers[0].args);
    JS_SetInterruptHandler(rt,retired_interrupt,&s);
    result=JS_Call(ctx,dispatched,global,0,NULL);
    JSValue error=JS_GetException(ctx);const char *message=JS_ToCString(ctx,error);
    verify(JS_IsException(result)&&message&&strstr(message,"interrupted")&&empty(&s),"executing-callback-retirement-abort-safe");
    JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);
    verify(finalized==1,"executing-local-reference-retained");JS_FreeValue(ctx,result);JS_FreeValue(ctx,dispatched);JS_FreeValue(ctx,arguments);JS_RunGC(rt);
    verify(finalized==2&&reentries==2,"executing-local-reference-eventually-released");
    release_document_tasks(&s);JS_FreeValue(ctx,global);JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("retired task roots native: %d checks, %d failed\n",checks,failures);return failures!=0;
}
