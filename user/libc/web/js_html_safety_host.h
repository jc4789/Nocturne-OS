/* Included by js.c after the DOM bridge. All policy authority is native. */
#include "js_html_safety_native.h"
static JSValue html_policy_state(JSContext *ctx,web_doc *document) {
    JSValue out=JS_NewObject(ctx),rules=JS_NewArray(ctx),requirements=JS_NewArray(ctx);uint32_t at=0,require_at=0,index=0;
    if(JS_IsException(out)||JS_IsException(rules)||JS_IsException(requirements)){JS_FreeValue(ctx,out);JS_FreeValue(ctx,rules);JS_FreeValue(ctx,requirements);return JS_EXCEPTION;}
    bool required=false,enforce=false;
    for(struct web_html_policy_rule *r=document->html_policy?document->html_policy->first:NULL;r;r=r->next,index++){
        if(r->require_script){required=true;if(!r->report_only)enforce=true;
            JSValue requirement=JS_NewObject(ctx);
            if(JS_IsException(requirement))goto fail;
            if(JS_DefinePropertyValueStr(ctx,requirement,"policyIndex",JS_NewUint32(ctx,index),JS_PROP_C_W_E)<0 ||
               JS_DefinePropertyValueStr(ctx,requirement,"reportOnly",JS_NewBool(ctx,r->report_only),JS_PROP_C_W_E)<0 ||
               JS_DefinePropertyValueStr(ctx,requirement,"originalPolicy",JS_NewString(ctx,r->original_policy?r->original_policy:""),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,requirement);goto fail;}
            if(JS_DefinePropertyValueUint32(ctx,requirements,require_at++,requirement,JS_PROP_C_W_E)<0)goto fail;
        }
        if(!r->has_names)continue;
        JSValue record=JS_NewObject(ctx),names=JS_NewArray(ctx);
        if(JS_IsException(record)||JS_IsException(names)){JS_FreeValue(ctx,record);JS_FreeValue(ctx,names);goto fail;}
        for(size_t i=0;i<r->count;i++)if(i>UINT32_MAX || JS_DefinePropertyValueUint32(ctx,names,(uint32_t)i,JS_NewString(ctx,r->names[i]),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,record);JS_FreeValue(ctx,names);goto fail;}
        if(JS_DefinePropertyValueStr(ctx,record,"names",names,JS_PROP_C_W_E)<0){JS_FreeValue(ctx,record);goto fail;}
        if(JS_DefinePropertyValueStr(ctx,record,"policyIndex",JS_NewUint32(ctx,index),JS_PROP_C_W_E)<0 ||
           JS_DefinePropertyValueStr(ctx,record,"allowDuplicates",JS_NewBool(ctx,r->allow_duplicates),JS_PROP_C_W_E)<0 ||
           JS_DefinePropertyValueStr(ctx,record,"reportOnly",JS_NewBool(ctx,r->report_only),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,record);goto fail;}
        if(JS_DefinePropertyValueUint32(ctx,rules,at++,record,JS_PROP_C_W_E)<0)goto fail;
    }
    if(JS_DefinePropertyValueStr(ctx,out,"required",JS_NewBool(ctx,required),JS_PROP_C_W_E)<0 ||
       JS_DefinePropertyValueStr(ctx,out,"enforce",JS_NewBool(ctx,enforce),JS_PROP_C_W_E)<0)goto fail;
    if(JS_DefinePropertyValueStr(ctx,out,"policies",rules,JS_PROP_C_W_E)<0){JS_FreeValue(ctx,out);JS_FreeValue(ctx,requirements);return JS_EXCEPTION;}
    if(JS_DefinePropertyValueStr(ctx,out,"requirements",requirements,JS_PROP_C_W_E)<0){JS_FreeValue(ctx,out);return JS_EXCEPTION;}
    return out;
fail:JS_FreeValue(ctx,out);JS_FreeValue(ctx,rules);JS_FreeValue(ctx,requirements);return JS_EXCEPTION;
}
static JSValue native_safety_required(JSContext *ctx,JSValueConst this_value,int argc,JSValueConst *argv){
    (void)this_value;struct web_js_state *s=state(ctx);
    node_t *node=argc && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])?unwrap(ctx,argv[0]):NULL;
    web_doc *owner=node && node->owner && node->owner->js?node->owner:s->doc;
    return JS_NewBool(ctx,html_policy_requires_script(owner));
}
static JSValue native_safety(JSContext *ctx,JSValueConst this_value,int argc,JSValueConst *argv){
    (void)this_value;struct web_js_state *s=state(ctx);
    if(!argc)return JS_ThrowTypeError(ctx,"HTML safety operation required");
    const char *op=JS_ToCString(ctx,argv[0]);if(!op)return JS_EXCEPTION;
    node_t *node=argc>1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])?unwrap(ctx,argv[1]):NULL;
    web_doc *owner=node && node->owner && node->owner->js?node->owner:s->doc;
    JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"state"))result=html_policy_state(ctx,owner);
    else if(!strcmp(op,"policyHeaders")){
        char *headers=html_policy_snapshot(owner);result=headers?JS_NewString(ctx,headers):JS_ThrowOutOfMemory(ctx);free(headers);
    }
    else if(!strcmp(op,"scriptText"))result=argc>2?js_script_text_marker(ctx,node,argv[2]):JS_ThrowTypeError(ctx,"Script text required");
    else if(!strcmp(op,"owner")){
        if(!node || owner==s->doc || !node->owner->js)result=JS_NULL;
        else if(argc<3)result=JS_ThrowTypeError(ctx,"Owner safety hook required");
        else{
            struct web_js_state *target=owner->js;
            const char *name=JS_ToCString(ctx,argv[2]);
            if(!name)result=JS_EXCEPTION;
            else if(target->disabled || target->rt!=s->rt || !JS_IsObject(target->hooks))result=JS_ThrowTypeError(ctx,"Owner safety realm is inactive");
            else if(strcmp(name,"check") && strcmp(name,"options") && strcmp(name,"input"))result=JS_ThrowTypeError(ctx,"Unsupported owner safety hook");
            else{
                const char *hook=!strcmp(name,"check")?"safetyCheck":!strcmp(name,"options")?"safetyOptions":"safetyInput";
                int insertion=!strcmp(name,"options")?2:3,provided=argc-3,count=provided+1;
                JSValue *args=js_malloc(ctx,(size_t)count*sizeof *args);
                if(!args)result=JS_EXCEPTION;
                else{
                    bool ok=provided>=insertion;
                    for(int i=0;i<count;i++){
                        if(i==insertion)args[i]=wrap(target,node);
                        else{int source=i<insertion?i:i-1;args[i]=source<provided?JS_DupValue(target->ctx,argv[source+3]):JS_UNDEFINED;}
                        if(JS_IsException(args[i]))ok=false;
                    }
                    begin_task(target);
                    JSValue value=ok?custom_element_hook(target,hook,count,args):JS_ThrowTypeError(ctx,"Invalid owner safety arguments");
                    end_task(target);
                    for(int i=0;i<count;i++)JS_FreeValue(target->ctx,args[i]);js_free(ctx,args);
                    if(JS_IsException(value))result=value;
                    else{result=JS_NewObject(ctx);if(JS_IsException(result))JS_FreeValue(ctx,value);else if(JS_SetPropertyStr(ctx,result,"value",value)<0){JS_FreeValue(ctx,result);result=JS_EXCEPTION;}}
                }
            }
            JS_FreeCString(ctx,name);
        }
    }
    else if(!strcmp(op,"serialize")){
        if(!node || (node->type!=N_ELEM && !node->shadow_host))result=JS_ThrowTypeError(ctx,"Element or ShadowRoot receiver required");
        else{
            uint32_t count=0;JSValue length=argc>3?JS_GetPropertyStr(ctx,argv[3],"length"):JS_UNDEFINED;
            if(argc>3 && (JS_IsException(length)||JS_ToUint32(ctx,&count,length)<0))result=JS_EXCEPTION;
            JS_FreeValue(ctx,length);
            node_t **roots=count?js_malloc(ctx,(size_t)count*sizeof *roots):NULL;
            if(count && !roots)result=JS_EXCEPTION;
            for(uint32_t i=0;i<count && !JS_IsException(result);i++){
                JSValue value=JS_GetPropertyUint32(ctx,argv[3],i);roots[i]=node_opaque(value);JS_FreeValue(ctx,value);
                if(!roots[i]||!roots[i]->shadow_host)result=JS_ThrowTypeError(ctx,"ShadowRoot list required");
            }
            if(!JS_IsException(result)){
                sbuf text={0};html_serialize_get(node,&text,argc>2 && JS_ToBool(ctx,argv[2])>0,roots,count);
                result=JS_NewStringLen(ctx,text.p?text.p:"",text.n);sb_free(&text);
            }
            js_free(ctx,roots);
        }
    }else if(!strcmp(op,"scripts")){
        if(!node)result=JS_ThrowTypeError(ctx,"Script insertion root required");
        else{
            pvec stack={0};pv_push(&stack,node);
            while(stack.n){node_t *n=stack.v[--stack.n];
                if(n->type==N_ELEM && !n->foreign && n->tag==T_template)continue;
                if(n->tag==T_script && n->script_parse_eligible && !n->script_started && connected(s,n)){
                    struct js_script *script=queue_script(s,n,true);
                    if(script && script->ready && !script->asynchronous)run_script(s,script);
                }
                if(n->shadow_root)pv_push(&stack,n->shadow_root);
                for(node_t *c=n->last;c;c=c->prev)pv_push(&stack,c);
            }
            pv_free(&stack);
        }
    }else if(!strcmp(op,"violation")){
        /* A Trusted Types violation (its sample already formatted). */
        if(argc<6)result=JS_ThrowTypeError(ctx,"Policy violation fields required");
        else{
            int32_t index=-1;if(argc>6 && JS_ToInt32(ctx,&index,argv[6])<0){JS_FreeCString(ctx,op);return JS_EXCEPTION;}
            struct web_html_policy_rule *rule=NULL;int32_t at=0;
            for(struct web_html_policy_rule *r=owner->html_policy?owner->html_policy->first:NULL;r;r=r->next,at++)if(at==index){rule=r;break;}
            size_t sample_length=0;
            const char *directive=JS_ToCString(ctx,argv[2]),*sample=JS_ToCStringLen(ctx,&sample_length,argv[4]);
            struct js_policy_violation *event=directive&&sample?js_mallocz(ctx,sizeof *event):NULL;
            if(!event)result=JS_EXCEPTION;
            else{
                struct html_csp_violation violation={.policy=rule,.directive=directive,
                    .resource=!strcmp(directive,"trusted-types")?"trusted-types-policy":"trusted-types-sink",
                    .sample=sample,.sample_length=sample_length};
                event->init=csp_violation_init(ctx,owner,&violation,owner->html_policy?owner->html_policy->status:0);
                /* Without a policy record the caller's disposition stands. */
                if(!JS_IsException(event->init) && !rule &&
                   JS_DefinePropertyValueStr(ctx,event->init,"disposition",JS_DupValue(ctx,argv[5]),JS_PROP_C_W_E)<0){
                    JS_FreeValue(ctx,event->init);event->init=JS_EXCEPTION;
                }
                if(JS_IsException(event->init)){js_free(ctx,event);result=JS_EXCEPTION;}
                else{
                    /* Parser rewind can release meta rules before this task.
                     * Their report endpoints are ignored; init owns the text. */
                    event->rule=rule && !rule->from_meta?rule:NULL;
                    if(s->last_policy_violation)s->last_policy_violation->next=event;else s->policy_violations=event;s->last_policy_violation=event;
                }
            }
            JS_FreeCString(ctx,directive);JS_FreeCString(ctx,sample);
        }
    }else if(!strcmp(op,"cspInline")){
        /* HTML: an event handler content attribute's (or a style attribute's)
           "Should element's inline behavior be blocked by CSP?" */
        size_t length=0;
        const char *type=argc>2?JS_ToCString(ctx,argv[2]):NULL,*text=argc>3?JS_ToCStringLen(ctx,&length,argv[3]):NULL;
        struct web_js_state *realm=owner==s->doc?s:owner->js;
        if(!type||!text)result=JS_EXCEPTION;
        else if(!node || node->type!=N_ELEM || !realm || realm->disabled)result=JS_ThrowTypeError(ctx,"Inline check requires an element of an active document");
        else{
            struct html_csp_element element={.nonce=doc_element_csp_nonce(node),.nonceable=html_policy_nonceable(node)};
            struct csp_site site={realm,node};
            result=JS_NewBool(ctx,html_policy_inline_allowed(realm->doc,!strcmp(type,"style attribute")?HTML_CSP_STYLE_ATTRIBUTE:HTML_CSP_SCRIPT_ATTRIBUTE,
                                                            &element,text,length,csp_report,&site));
        }
        JS_FreeCString(ctx,type);JS_FreeCString(ctx,text);
    }else if(!strcmp(op,"cspTimer")){
        /* HTML timer initialization steps: a string handler, after its Trusted
           Types step, is EnsureCSPDoesNotBlockStringCompilation's "timer"
           codeString; blocked, setTimeout returns 0 (null here). */
        struct csp_site site={s,NULL};
        result=argc>2?csp_timer_code(ctx,s->doc,argv[2],s->doc->url,csp_report,&site):JS_ThrowTypeError(ctx,"Timer handler required");
    }else result=JS_ThrowTypeError(ctx,"Unknown HTML safety operation");
    JS_FreeCString(ctx,op);return result;
}
static JSValue trusted_code_check(JSContext *ctx,JSValueConst value,int kind,int argc,JSValueConst *argv,void *opaque){
    struct web_js_state *s=opaque;
    if(!s || !JS_IsObject(s->hooks))return JS_DupValue(ctx,value);
    JSValue result;
    if(!html_policy_requires_script(s->doc) && (kind==JS_DYNAMIC_FUNCTION || !JS_IsObject(value)))result=JS_DupValue(ctx,value);
    else{
        JSValue args_array=JS_NewArray(ctx);
        if(JS_IsException(args_array))return args_array;
        for(int i=0;i<argc;i++)if(JS_SetPropertyUint32(ctx,args_array,(uint32_t)i,JS_DupValue(ctx,argv[i]))<0){JS_FreeValue(ctx,args_array);return JS_EXCEPTION;}
        JSValue args[]={JS_DupValue(ctx,value),JS_NewInt32(ctx,kind),args_array};
        result=custom_element_hook(s,"safetyDynamicCode",3,args);
        for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,args[i]);
    }
    /* A Function's parameters and body are checked once, assembled. */
    if(kind==JS_DYNAMIC_FUNCTION_ARGUMENT)return result;
    struct csp_site site={s,NULL};
    return csp_compile_check(ctx,s->doc,result,kind==JS_DYNAMIC_FUNCTION,csp_report,&site);
}
static int wasm_can_compile(JSContext *ctx){
    struct web_js_state *s=state(ctx);
    if(!s || s->disabled)return 0;
    bool allowed=true;
    for(struct web_html_policy_rule *rule=s->doc->html_policy?s->doc->html_policy->first:NULL;rule;rule=rule->next){
        if(html_policy_wasm_allowed(rule))continue;
        if(!rule->report_only)allowed=false;
        struct js_policy_violation *event=js_mallocz(ctx,sizeof *event);
        if(!event)return -1;
        event->init=wasm_policy_violation_init(ctx,s->doc,rule,s->doc->html_policy?s->doc->html_policy->status:0);
        if(JS_IsException(event->init)){js_free(ctx,event);return -1;}
        event->rule=rule->from_meta?NULL:rule;
        if(s->last_policy_violation)s->last_policy_violation->next=event;else s->policy_violations=event;
        s->last_policy_violation=event;
        log_text(s,1,rule->report_only?"CSP report-only: WebAssembly compilation violates script-src":"CSP: WebAssembly compilation blocked by script-src");
    }
    return allowed;
}
