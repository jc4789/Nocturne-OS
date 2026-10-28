/* Browser-only unforgeable Trusted Type data. Kept in QuickJS native class
 * slots, never in writable JS properties or a second DOM representation. */
static JSClassID trusted_value_class;
struct trusted_value_data { int kind; JSValue value; };
static void trusted_value_finalizer(JSRuntime *rt, JSValue value) {
    struct trusted_value_data *data=JS_GetOpaque(value,trusted_value_class);
    if(data){JS_FreeValueRT(rt,data->value);js_free_rt(rt,data);}
}
static void trusted_value_mark(JSRuntime *rt,JSValueConst value,JS_MarkFunc *mark) {
    struct trusted_value_data *data=JS_GetOpaque(value,trusted_value_class);
    if(data)JS_MarkValue(rt,data->value,mark);
}
static int trusted_native_init(JSContext *ctx) {
    if(!trusted_value_class)JS_NewClassID(&trusted_value_class);
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(JS_IsRegisteredClass(rt,trusted_value_class))return 0;
    JSClassDef def={.class_name="TrustedType",.finalizer=trusted_value_finalizer,.gc_mark=trusted_value_mark};
    return JS_NewClass(rt,trusted_value_class,&def);
}
/* CSP3 §2.4.1 and §5.5's event fields for a violation the policy model found
 * (html_policy.h), shared by Window and Worker: one record that becomes both
 * the SecurityPolicyViolationEvent's init and the report-uri body. Nothing
 * here comes from writable author globals or an author-provided dictionary.
 * document is the global's (its CSP list, URL and referrer); status is its
 * response's status. The source location is the running author script's
 * (§2.4.1: only "if the user agent is currently executing script"). */
static JSValue csp_report_text(JSContext *ctx,web_doc *document,const char *url){
    if(!url || !*url)return JS_NewString(ctx,"");
    char *stripped=html_policy_report_url(document,url);
    if(!stripped)return JS_ThrowOutOfMemory(ctx);
    JSValue value=JS_NewString(ctx,stripped);free(stripped);return value;
}
static JSValue csp_violation_init(JSContext *ctx,web_doc *document,const struct html_csp_violation *violation,int status){
    JSValue init=JS_NewObjectProto(ctx,JS_NULL);
    if(JS_IsException(init))return init;
    const struct web_html_policy_rule *rule=violation->policy;
    /* §5.2: a URL is stripped; "inline", "eval", "wasm-eval" and the
       Trusted Types keywords are reported as they are. */
    const char *resource=violation->resource?violation->resource:"";
    bool url=strchr(resource,':')!=NULL;
    char file[512];int column=0,line=JS_GetScriptLocation(ctx,file,sizeof file,&column);
    JSValue values[]={
        csp_report_text(ctx,document,document->url),
        csp_report_text(ctx,document,document->referrer),
        url?csp_report_text(ctx,document,resource):JS_NewString(ctx,resource),
        JS_NewString(ctx,violation->directive),JS_NewString(ctx,violation->directive),
        JS_NewString(ctx,rule&&rule->original_policy?rule->original_policy:""),
        JS_NewString(ctx,rule&&rule->report_only?"report":"enforce"),
        line?csp_report_text(ctx,document,file):JS_NewString(ctx,""),
        JS_NewStringLen(ctx,violation->sample?violation->sample:"",violation->sample?violation->sample_length:0),
        JS_NewInt32(ctx,status),JS_NewInt32(ctx,line),JS_NewInt32(ctx,line?column:0)};
    const char *keys[]={"documentURI","referrer","blockedURI","effectiveDirective","violatedDirective","originalPolicy",
        "disposition","sourceFile","sample","statusCode","lineNumber","columnNumber"};
    bool ok=true;
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++){
        if(!ok || JS_IsException(values[i])){JS_FreeValue(ctx,values[i]);ok=false;continue;}
        ok=JS_DefinePropertyValueStr(ctx,init,keys[i],values[i],JS_PROP_C_W_E)>=0;
    }
    if(!ok){JS_FreeValue(ctx,init);return JS_EXCEPTION;}
    return init;
}
/* §4.4.1 EnsureCSPDoesNotBlockStringCompilation after its Trusted Types step,
   from QuickJS's dynamic code check: code is eval's string or the Function
   constructor's source, which QuickJS assembles inside parentheses (the
   ECMAScript sourceString is what is between them). Takes code; returns it,
   or throws the EvalError. */
static JSValue csp_compile_check(JSContext *ctx,web_doc *document,JSValue code,bool function,
                                 html_csp_reporter report,void *opaque){
    if(JS_IsException(code) || !JS_IsString(code) || !document->html_policy || !document->html_policy->first)return code;
    size_t length;const char *source=JS_ToCStringLen(ctx,&length,code);
    if(!source){JS_FreeValue(ctx,code);return JS_EXCEPTION;}
    const char *text=source;
    if(function && length>=2 && source[0]=='(' && source[length-1]==')'){text++;length-=2;}
    bool allowed=html_policy_eval_allowed(document,text,length,report,opaque);
    JS_FreeCString(ctx,source);
    if(allowed)return code;
    JS_FreeValue(ctx,code);
    return JS_ThrowEvalError(ctx,"Code generation from strings disallowed by Content Security Policy");
}
/* HTML timer initialization steps for a string handler, after its Trusted
   Types step: EnsureCSPDoesNotBlockStringCompilation's "timer" codeString.
   Blocked, null (setTimeout then returns 0); otherwise the handler compiled
   as a function, in the global scope, named for the realm's script URL. */
static JSValue csp_timer_code(JSContext *ctx,web_doc *document,JSValueConst handler,const char *url,
                              html_csp_reporter report,void *opaque){
    size_t length=0;const char *text=JS_ToCStringLen(ctx,&length,handler);
    if(!text)return JS_EXCEPTION;
    JSValue result=JS_NULL;
    if(html_policy_eval_allowed(document,text,length,report,opaque)){
        /* JS_Eval reads up to the terminating NUL (quickjs.h), which sb_cstr keeps. */
        sbuf b={0};sb_puts(&b,"(function(){\n");sb_put(&b,text,length);sb_puts(&b,"\n})");
        result=JS_Eval(ctx,sb_cstr(&b),b.n,url?url:"",JS_EVAL_TYPE_GLOBAL);sb_free(&b);
    }
    JS_FreeCString(ctx,text);return result;
}
/* §4.5.1's violation: script-src, resource "wasm-eval". */
static JSValue wasm_policy_violation_init(JSContext *ctx,web_doc *document,struct web_html_policy_rule *rule,int status) {
    struct html_csp_violation violation={.policy=rule,.directive="script-src",.resource="wasm-eval"};
    return csp_violation_init(ctx,document,&violation,status);
}
static JSValue native_trusted(JSContext *ctx,JSValueConst this_value,int argc,JSValueConst *argv) {
    (void)this_value;
    if(argc<1)return JS_ThrowTypeError(ctx,"Trusted Type operation required");
    const char *op=JS_ToCString(ctx,argv[0]);
    if(!op)return JS_EXCEPTION;
    bool create=!strcmp(op,"create"),kind=!strcmp(op,"kind"),payload=!strcmp(op,"payload");
    JS_FreeCString(ctx,op);
    if(create){
        int32_t type;
        if(argc<4)return JS_ThrowTypeError(ctx,"Trusted Type creation arguments required");
        if(JS_ToInt32(ctx,&type,argv[1])<0)return JS_EXCEPTION;
        /* 1..4 are Trusted Types; 5 is the host-private cross-realm Sanitizer
           configuration slot. No author code can call this factory. */
        if(type<1||type>5)return JS_ThrowTypeError(ctx,"Invalid Trusted Type kind");
        if(trusted_native_init(ctx)<0)return JS_EXCEPTION;
        struct trusted_value_data *data=js_mallocz(ctx,sizeof *data);
        if(!data)return JS_EXCEPTION;
        data->kind=type;data->value=JS_DupValue(ctx,argv[2]);
        JSValue value=JS_NewObjectProtoClass(ctx,argv[3],trusted_value_class);
        if(JS_IsException(value)){JS_FreeValue(ctx,data->value);js_free(ctx,data);return value;}
        JS_SetOpaque(value,data);return value;
    }
    struct trusted_value_data *data=argc>1?JS_GetOpaque(argv[1],trusted_value_class):NULL;
    if(kind)return JS_NewInt32(ctx,data?data->kind:0);
    if(payload){
        int32_t type=0;
        if(argc<3||JS_ToInt32(ctx,&type,argv[2])<0)return JS_EXCEPTION;
        if(!data||data->kind!=type)return JS_ThrowTypeError(ctx,"Illegal Trusted Type receiver");
        return JS_DupValue(ctx,data->value);
    }
    return JS_ThrowTypeError(ctx,"Unknown Trusted Type operation");
}
