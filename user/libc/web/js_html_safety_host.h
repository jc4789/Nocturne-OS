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
                if(n->tag==T_script && n->script_parse_eligible && !n->script_started && connected(s,n)){n->script_parse_eligible=false;
                    if(!web_js_script_parser_capture(n->owner,n)){result=JS_ThrowOutOfMemory(ctx);break;}
                    struct js_script *script=queue_script(s,n,true);
                    if(script && script->ready && !script->asynchronous)run_script(s,script);
                }
                if(n->shadow_root)pv_push(&stack,n->shadow_root);
                for(node_t *c=n->last;c;c=c->prev)pv_push(&stack,c);
            }
            pv_free(&stack);
        }
    }else if(!strcmp(op,"violation")){
        if(argc<6)result=JS_ThrowTypeError(ctx,"Policy violation fields required");
        else{
            struct js_policy_violation *event=js_mallocz(ctx,sizeof *event);
            if(!event)result=JS_EXCEPTION;
            else{
                event->init=JS_NewObjectProto(ctx,JS_NULL);
                const char *fields[]={"effectiveDirective","sample","disposition"};
                const unsigned values[]={2,4,5};
                int32_t index=-1;if(argc>6 && JS_ToInt32(ctx,&index,argv[6])<0){JS_FreeValue(ctx,event->init);js_free(ctx,event);JS_FreeCString(ctx,op);return JS_EXCEPTION;}
                int32_t at=0;for(struct web_html_policy_rule *r=owner->html_policy?owner->html_policy->first:NULL;r;r=r->next,at++)if(at==index){event->rule=r;break;}
                bool ok=!JS_IsException(event->init);
                for(unsigned i=0;i<3 && ok;i++)ok=JS_DefinePropertyValueStr(ctx,event->init,fields[i],JS_DupValue(ctx,argv[values[i]]),JS_PROP_C_W_E)>=0;
                if(ok)ok=JS_DefinePropertyValueStr(ctx,event->init,"violatedDirective",JS_DupValue(ctx,argv[2]),JS_PROP_C_W_E)>=0;
                if(ok)ok=JS_DefinePropertyValueStr(ctx,event->init,"sourceFile",JS_NewString(ctx,owner->url),JS_PROP_C_W_E)>=0;
                if(ok)ok=JS_DefinePropertyValueStr(ctx,event->init,"documentURI",JS_NewString(ctx,owner->url),JS_PROP_C_W_E)>=0;
                const char *directive=JS_ToCString(ctx,argv[2]);
                if(!directive)ok=false;
                if(ok)ok=JS_DefinePropertyValueStr(ctx,event->init,"blockedURI",JS_NewString(ctx,!strcmp(directive,"trusted-types")?"trusted-types-policy":"trusted-types-sink"),JS_PROP_C_W_E)>=0;
                JS_FreeCString(ctx,directive);
                if(ok)ok=JS_DefinePropertyValueStr(ctx,event->init,"originalPolicy",JS_NewString(ctx,event->rule?event->rule->original_policy:""),JS_PROP_C_W_E)>=0;
                if(!ok){JS_FreeValue(ctx,event->init);js_free(ctx,event);result=JS_EXCEPTION;}
                else{if(s->last_policy_violation)s->last_policy_violation->next=event;else s->policy_violations=event;s->last_policy_violation=event;}
            }
        }
    }else result=JS_ThrowTypeError(ctx,"Unknown HTML safety operation");
    JS_FreeCString(ctx,op);return result;
}
static JSValue trusted_code_check(JSContext *ctx,JSValueConst value,int kind,int argc,JSValueConst *argv,void *opaque){
    struct web_js_state *s=opaque;
    if(!s || !JS_IsObject(s->hooks))return JS_DupValue(ctx,value);
    if(!html_policy_requires_script(s->doc) && (kind==JS_DYNAMIC_FUNCTION || !JS_IsObject(value)))return JS_DupValue(ctx,value);
    JSValue args_array=JS_NewArray(ctx);
    if(JS_IsException(args_array))return args_array;
    for(int i=0;i<argc;i++)if(JS_SetPropertyUint32(ctx,args_array,(uint32_t)i,JS_DupValue(ctx,argv[i]))<0){JS_FreeValue(ctx,args_array);return JS_EXCEPTION;}
    JSValue args[]={JS_DupValue(ctx,value),JS_NewInt32(ctx,kind),args_array};
    JSValue result=custom_element_hook(s,"safetyDynamicCode",3,args);
    for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,args[i]);return result;
}
