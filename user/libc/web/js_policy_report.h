/* Native CSP report transport. Never calls page JSON/toJSON or exposed Fetch. */
static void policy_report_url(struct web_js_state *s,JSValueConst init,const char *url,const char *origin){
    if(!url || (strncasecmp(url,"http://",7)&&strncasecmp(url,"https://",8)))return;
    const char *authority=strstr(url,"://")+3,*end=authority+strcspn(authority,"/?#");
    if(memchr(authority,'@',end-authority))return;
    JSValue record=JS_NewObjectProto(s->ctx,JS_NULL),envelope=JS_NewObjectProto(s->ctx,JS_NULL);
    const char *fields[]={"documentURI","referrer","blockedURI","violatedDirective","effectiveDirective","originalPolicy","sourceFile","sample","disposition","statusCode","lineNumber","columnNumber"};
    const char *wire[]={"document-uri","referrer","blocked-uri","violated-directive","effective-directive","original-policy","source-file","script-sample","disposition","status-code","line-number","column-number"};
    bool ok=!JS_IsException(record)&&!JS_IsException(envelope);
    for(unsigned i=0;i<sizeof fields/sizeof *fields && ok;i++){
        JSValue value=JS_GetPropertyStr(s->ctx,init,fields[i]);
        if(JS_IsUndefined(value)){JS_FreeValue(s->ctx,value);value=i>=9?JS_NewInt32(s->ctx,0):JS_NewString(s->ctx,"");}
        if(JS_IsException(value))ok=false;
        else ok=JS_DefinePropertyValueStr(s->ctx,record,wire[i],value,JS_PROP_C_W_E)>=0;
    }
    JSValue json=JS_UNDEFINED;
    if(ok){ok=JS_DefinePropertyValueStr(s->ctx,envelope,"csp-report",record,JS_PROP_C_W_E)>=0;record=JS_UNDEFINED;}
    if(ok)json=JS_JSONStringify(s->ctx,envelope,JS_UNDEFINED,JS_UNDEFINED);
    JS_FreeValue(s->ctx,record);JS_FreeValue(s->ctx,envelope);
    if(!ok || JS_IsException(json)){JS_FreeValue(s->ctx,json);exception(s);return;}
    size_t length;const char *body=JS_ToCStringLen(s->ctx,&length,json);
    if(body){struct js_pending *pending=pending_new(s,P_POLICY_REPORT,url);
        if(pending){pending->credentials=1;pending->redirect_error=true;
            if(origin){pending->origin=js_strdup(s->ctx,origin);if(!pending->origin){pending_error(pending,"Report initiator allocation failed");exception(s);JS_FreeCString(s->ctx,body);JS_FreeValue(s->ctx,json);return;}}
            send_request(s,pending,WEB_RESOURCE_REPORT,"POST","Content-Type: application/csp-report\r\n",body,length);
        }else exception(s);
        JS_FreeCString(s->ctx,body);
    }else exception(s);
    JS_FreeValue(s->ctx,json);
}
static void policy_report_send(struct web_js_state *s,JSValueConst init,struct web_html_policy_rule *rule){
    if(!rule || rule->from_meta)return;
    if(rule->report_to && *rule->report_to){log_text(s,1,"Capability: CSP report-to requires the unavailable Reporting API; report-uri fallback is suppressed");return;}
    for(size_t i=0;i<rule->report_uri_count;i++)policy_report_url(s,init,rule->report_uris[i],NULL);
}
static void js_policy_worker_report(void *opaque,JSValueConst message){
    struct web_js_state *s=opaque;if(!s || s->disabled)return;
    JSValue init=JS_GetPropertyStr(s->ctx,message,"init"),endpoints=JS_GetPropertyStr(s->ctx,message,"endpoints"),report_to=JS_GetPropertyStr(s->ctx,message,"reportTo");
    const char *group=JS_IsString(report_to)?JS_ToCString(s->ctx,report_to):NULL;
    if(group && *group)log_text(s,1,"Capability: Worker CSP report-to requires the unavailable Reporting API; report-uri fallback is suppressed");
    else if(JS_IsObject(init)&&JS_IsArray(s->ctx,endpoints)){
        // This is a private native Worker record, not an author event. HTTP
        // Worker credentials use its actual response URL; local blob/data
        // Workers use the creator's effective URL, never the report endpoint.
        JSValue document=JS_GetPropertyStr(s->ctx,init,"documentURI");
        const char *document_uri=JS_IsString(document)?JS_ToCString(s->ctx,document):NULL;
        const char *origin=document_uri&&(!strncasecmp(document_uri,"http://",7)||!strncasecmp(document_uri,"https://",8))?document_uri:web_effective_url(s->doc);
        JSValue count=JS_GetPropertyStr(s->ctx,endpoints,"length");uint32_t length=0;
        if(JS_ToUint32(s->ctx,&length,count)<0)exception(s);
        else for(uint32_t i=0;i<length;i++){
            JSValue value=JS_GetPropertyUint32(s->ctx,endpoints,i);const char *url=JS_ToCString(s->ctx,value);
            if(url)policy_report_url(s,init,url,origin);else exception(s);
            JS_FreeCString(s->ctx,url);JS_FreeValue(s->ctx,value);
        }
        JS_FreeValue(s->ctx,count);
        JS_FreeCString(s->ctx,document_uri);JS_FreeValue(s->ctx,document);
    }
    JS_FreeCString(s->ctx,group);JS_FreeValue(s->ctx,init);JS_FreeValue(s->ctx,endpoints);JS_FreeValue(s->ctx,report_to);
}
