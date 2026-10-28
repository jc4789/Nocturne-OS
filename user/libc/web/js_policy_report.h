/* Native CSP report transport. Never calls page JSON/toJSON or exposed Fetch. */
static void policy_report_post(struct web_js_state *s,JSValueConst init,const char *url,const char *origin);
/* A report goes to an http(s) URL without credentials (a report request carries none), as the
   URL parser reads url. */
static void policy_report_url(struct web_js_state *s,JSValueConst init,const char *url,const char *origin){
    struct web_url target;int read=url?web_url_read(url,strlen(url),&target):0;
    if(read<0){log_text(s,2,"CSP report URL could not be read: out of memory");return;}
    if(read!=1)return;
    if(web_url_http_scheme(&target)&&!web_url_includes_credentials(&target))policy_report_post(s,init,target.href,origin);
    web_url_free(&target);
}
static void policy_report_post(struct web_js_state *s,JSValueConst init,const char *url,const char *origin){
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
        const char *origin=web_url_has_scheme(document_uri,"http")||web_url_has_scheme(document_uri,"https")?document_uri:web_effective_url(s->doc);
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
/* CSP3 §5.5 "Report a violation" for a check of this realm's document (the
   checks are html_policy.h's). The record is made now, while the script that
   caused the violation is the running one; firing the event and sending the
   policy's reports is a queued task (the policy_violations queue). */
static void csp_report(void *opaque,const struct html_csp_violation *violation){
    struct csp_site *site=opaque;struct web_js_state *s=site?site->s:NULL;
    if(!s || s->disabled || !s->ctx || !violation)return;
    JSContext *ctx=s->ctx;
    JSValue init=csp_violation_init(ctx,s->doc,violation,s->doc->html_policy?s->doc->html_policy->status:0);
    struct js_policy_violation *event=JS_IsException(init)?NULL:js_mallocz(ctx,sizeof *event);
    if(!event){
        /* A check never leaves an exception for its caller to find. */
        JS_FreeValue(ctx,init);if(JS_HasException(ctx))JS_FreeValue(ctx,JS_GetException(ctx));
        log_text(s,2,"Content Security Policy: a violation could not be recorded (memory)");return;
    }
    event->init=init;event->target=JS_UNDEFINED;
    /* An element of another document can never be the target (§5.5 step 1). */
    if(site->element && site->element->owner==s->doc){
        event->target=wrap(s,site->element);
        if(JS_IsException(event->target)){event->target=JS_UNDEFINED;JS_FreeValue(ctx,JS_GetException(ctx));}
    }
    /* Parser rewind can release meta rules before the task; they have no
       report endpoints, and init owns their text. */
    event->rule=violation->policy && !violation->policy->from_meta?violation->policy:NULL;
    if(s->last_policy_violation)s->last_policy_violation->next=event;else s->policy_violations=event;
    s->last_policy_violation=event;
    char resource[320],message[900];
    diagnostic_url(violation->resource?violation->resource:"",resource,sizeof resource);
    bool report_only=violation->policy && violation->policy->report_only;
    snprintf(message,sizeof message,"Content Security Policy: %s %s (%s; policy \"%.400s\")",
             report_only?"report-only violation by":"blocked",resource,violation->directive?violation->directive:"",
             violation->policy && violation->policy->original_policy?violation->policy->original_policy:"");
    log_text(s,report_only?1:2,message);
}
/* The element a request is made for, which a violation targets. */
static node_t *pending_element(const struct js_pending *p){
    if(p->kind==P_SCRIPT && p->script)return p->script->node;
    if(p->kind==P_FRAME && p->frame)return p->frame->element;
    if(p->kind==P_CSS)return p->element;
    return NULL;
}
/* Fetch main fetch's "Should request be blocked by Content Security Policy?"
   (CSP3 §4.1.2, with §4.1.1's report-only pass) for a request this realm
   sends. url is its current URL: the request's own before it is sent, the
   response's after redirects (§4.1.3's post-request check gives the same
   answer for a response no service worker made). */
static bool csp_fetch_allowed(struct web_js_state *s,const struct html_csp_request *request,node_t *element){
    if(!s->doc->html_policy || !s->doc->html_policy->first)return true;
    struct csp_site site={s,element};
    return html_policy_request_allowed(s->doc,request,csp_report,&site);
}
static bool csp_request_allowed(struct web_js_state *s,struct js_pending *p,const char *url,unsigned redirects){
    if(!s->doc->html_policy || !s->doc->html_policy->first || p->kind==P_POLICY_REPORT)return true;
    /* A Worker's own imports and fetches answer to the Worker's CSP list. */
    if(p->kind==P_WORKER && p->worker_kind!=0)return true;
    struct html_csp_request request={.url=url,.original_url=p->url,
        .destination=p->kind==P_WORKER?WEBNET_DEST_WORKER:pending_destination(p),.nonce=p->nonce,
        /* Integrity metadata may stand for a hash only where it is enforced:
           module scripts check it (module_integrity), classic ones do not. */
        .integrity=p->kind==P_MODULE?p->integrity:NULL,.parser_inserted=p->parser_inserted,.redirect_count=redirects};
    return csp_fetch_allowed(s,&request,pending_element(p));
}
/* HTML prepare the script element: "Should element's inline behavior be
   blocked by Content Security Policy?" for an inline script's source text. */
static bool csp_script_allowed(struct web_js_state *s,node_t *n,bool parser_inserted,const char *text,size_t length){
    if(!s->doc->html_policy || !s->doc->html_policy->first)return true;
    struct html_csp_element element={.nonce=node_attr(n,"nonce"),.nonceable=html_policy_nonceable(n),.parser_inserted=parser_inserted};
    struct csp_site site={s,n};
    return html_policy_inline_allowed(s->doc,HTML_CSP_SCRIPT,&element,text,length,csp_report,&site);
}
/* HTML "update a style block": the inline check for a style element's child
   text. A document without a realm (scripting off) is checked, unreported. */
bool web_js_csp_style_allowed(web_doc *d,node_t *n,const char *text,size_t length){
    if(!d || !d->html_policy || !d->html_policy->first)return true;
    struct html_csp_element element={.nonce=node_attr(n,"nonce"),.nonceable=html_policy_nonceable(n)};
    struct csp_site site={d->js,n};
    return html_policy_inline_allowed(d,HTML_CSP_STYLE,&element,text?text:"",length,csp_report,&site);
}
/* HTML "set the frozen base URL": CSP3 §6.3.1.1 for the base element's URL. */
bool web_js_csp_base_allowed(web_doc *d,node_t *base,const char *url){
    if(!d || !d->html_policy || !d->html_policy->first)return true;
    struct csp_site site={d->js,base};
    return html_policy_base_allowed(d,url,csp_report,&site);
}
