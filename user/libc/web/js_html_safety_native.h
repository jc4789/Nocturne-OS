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
/* Shared native CSP record for Window and Worker. These fields never come
 * from writable author globals or an author-provided policy dictionary. */
static JSValue wasm_policy_violation_init(JSContext *ctx,const struct web_html_policy_rule *rule,
                                           const char *url,int status) {
    JSValue init=JS_NewObjectProto(ctx,JS_NULL);
    if(JS_IsException(init))return init;
    const char *keys[]={"effectiveDirective","violatedDirective","blockedURI","sample",
        "disposition","originalPolicy","documentURI","sourceFile","referrer"};
    const char *values[]={"script-src","script-src","wasm-eval","",
        rule->report_only?"report":"enforce",rule->original_policy?rule->original_policy:"",
        url?url:"",url?url:"",""};
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++){
        JSValue value=JS_NewString(ctx,values[i]);
        if(JS_IsException(value)||JS_DefinePropertyValueStr(ctx,init,keys[i],value,JS_PROP_C_W_E)<0)goto fail;
    }
    if(JS_DefinePropertyValueStr(ctx,init,"statusCode",JS_NewInt32(ctx,status),JS_PROP_C_W_E)<0 ||
       JS_DefinePropertyValueStr(ctx,init,"lineNumber",JS_NewInt32(ctx,0),JS_PROP_C_W_E)<0 ||
       JS_DefinePropertyValueStr(ctx,init,"columnNumber",JS_NewInt32(ctx,0),JS_PROP_C_W_E)<0)goto fail;
    return init;
fail:JS_FreeValue(ctx,init);return JS_EXCEPTION;
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
