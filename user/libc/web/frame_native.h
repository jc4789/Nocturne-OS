/* Included once by js.c after its bounded resource transport. */
static void frame_cancel(struct web_js_state *s,struct web_frame *f) {
    if(!f->request)return;
    for(struct js_pending *p=s->pending;p;p=p->next)if(p->id==f->request){
        if(s->host.cancel)s->host.cancel(s->host.opaque,p->id);
        p->frame=NULL;p->aborted=true;pending_error(p,"Frame navigation replaced");break;
    }
    f->request=0;
}
static void frame_retire_document(web_doc *d) {
    struct web_js_state *s=d?d->js:NULL;if(!s)return;
    s->disabled=true;
    release_document_tasks(s);
    history_forget(s);
    broadcast_free(s);
    web_avmedia_free(d);
    web_worker_free(s->workers);s->workers=NULL;
    for(struct js_pending *p=s->pending;p;p=p->next) {
        if(!p->done)release_pending_request(s,p);
        p->aborted=true;pending_error(p,"Child browsing context retired");
    }
}
void web_js_retire(web_doc *d) {
    frame_retire_document(d);
    for(struct web_frame *f=d?d->frames:NULL;f;) {
        struct web_frame *next=web_frame_walk_next(d,f,true);
        web_doc *old=f->document;
        f->detached=true;f->window_token=NULL;
        if(old) {
            frame_retire_document(old);old->live=false;
            old->frame_retired_next=f->retired;f->retired=old;f->document=NULL;f->notified=false;
        }
        f=next;
    }
}
bool web_js_complete(web_doc *d){return d && d->js && (d->js->load_sent || d->js->disabled);}
static bool frame_string_replace(char **slot,const char *text) {
    char *copy=text?strdup(text):NULL;
    if(text && !copy)return false;
    free(*slot);*slot=copy;return true;
}
static void frame_sync(struct web_js_state *s,node_t *n) {
    if(!web_frame_element(n) || !connected(s,n) || n->owner!=s->doc)return;
    struct web_frame *f=web_frame_ensure(s->doc,n);
    if(!f){if(!s->doc->frame_allocation_reported){s->doc->frame_allocation_reported=true;log_text(s,2,"Frame metadata allocation failed");}return;}
    const char *src=node_attr(n,"src"),*srcdoc=n->tag==T_iframe?node_attr(n,"srcdoc"):NULL,*sandbox=node_attr(n,"sandbox");
    if(!src)src="";
    if(web_frame_initial_blocked(f,src,srcdoc))return;
    if((f->document || f->failed) && !f->detached && f->source && !strcmp(f->source,src) &&
       ((!f->srcdoc && !srcdoc)||(f->srcdoc && srcdoc && !strcmp(f->srcdoc,srcdoc))) &&
       !f->navigation)return; /* changing the attribute does not relax an active Document */
    frame_cancel(s,f);
    f->initial_failed=false; /* new source/srcdoc or new child navigable */
    if(!frame_string_replace(&f->source,src) || !frame_string_replace(&f->srcdoc,srcdoc) || !frame_string_replace(&f->sandbox,sandbox)){
        log_text(s,2,"Frame navigation could not retain its source");return;
    }
    char known[161];uint32_t own=sandbox_parse(sandbox,known,sizeof known);
    f->pending_sandbox_flags=own|s->doc->sandbox_flags;
    if(s->host.debug_js && f->pending_sandbox_flags) {
        char *absolute=NULL,*origin=NULL;const char *desired=f->navigation?f->navigation:src;
        if(web_resolve_url_owned(s->doc->base,desired,&absolute)==1)http_origin_owned(absolute,&origin);
        char message[480];snprintf(message,sizeof message,
            "Frame sandbox policy: allow=[%s] origin=[%.160s] inherited=0x%x pending=0x%x supported=%d",
            known,origin?origin:"null",s->doc->sandbox_flags,f->pending_sandbox_flags,
            sandbox_supported(f->pending_sandbox_flags));
        free(absolute);free(origin);log_text(s,0,message);
    }
    if(!sandbox_supported(f->pending_sandbox_flags) ||
       (f->pending_sandbox_flags && srcdoc && !f->navigation)) {
        web_frame_retire(f);f->failed=true;f->detached=false;
        log_text(s,1,"Sandboxed frame blocked: unsupported opaque/script-disabled/srcdoc policy");return;
    }
    if(!f->document || f->detached) {
        if(!web_frame_initial_create(f,&s->host)){
            log_text(s,2,"Frame initial about:blank context could not be created");return;
        }
        node_t *ignored=NULL;html_resume(f->document->parser,&ignored);
    }
    if(srcdoc && !f->navigation){
        if(!web_frame_commit(f,srcdoc,strlen(srcdoc),"about:srcdoc","utf-8",&s->host,true))
            log_text(s,2,"Frame srcdoc document allocation failed");
        return;
    }
    const char *desired=f->navigation?f->navigation:src;
    if(f->pending_sandbox_flags && (!*desired || !strcasecmp(desired,"about:blank"))){
        web_frame_retire(f);f->failed=true;f->detached=false;
        free(f->navigation);free(f->navigation_origin);f->navigation=f->navigation_origin=NULL;f->navigation_owner=NULL;
        log_text(s,1,"Sandboxed authored about:blank policy is outside the supported subset");return;
    }
    if(!*desired || !strcasecmp(desired,"about:blank")) {
        if((f->navigation || strcmp(f->document->url,"about:blank")) && !web_frame_commit(f,"",0,"about:blank",NULL,&s->host,true))
            log_text(s,2,"Frame about:blank navigation failed");
        free(f->navigation);free(f->navigation_origin);f->navigation=f->navigation_origin=NULL;f->navigation_owner=NULL;return;
    }
    if(!strncasecmp(desired,"javascript:",11)) {
        if(web_frame_same_origin(s->doc,f->document) && f->document->js && !f->document->js->disabled){
            struct web_js_state *child=f->document->js;begin_task(child);
            JSValue result=JS_Eval(child->ctx,desired+11,strlen(desired+11),s->doc->url,JS_EVAL_TYPE_GLOBAL);
            if(JS_IsException(result))exception(child);else JS_FreeValue(child->ctx,result);
            end_task(child);
        }else log_text(s,1,"Cross-origin javascript frame navigation blocked");
        free(f->navigation);free(f->navigation_origin);f->navigation=f->navigation_origin=NULL;f->navigation_owner=NULL;return;
    }
    char *url=NULL;
    int resolved=web_resolve_url_owned(s->doc->base,desired,&url);
    bool valid=resolved==1 && permitted_url(s,url,false);
    free(f->navigation);f->navigation=NULL;
    if(!valid){free(url);free(f->navigation_origin);f->navigation_origin=NULL;f->navigation_owner=NULL;f->failed=true;log_text(s,resolved<0?2:1,resolved<0?"Frame source URL allocation failed":"Frame source URL is not permitted");return;}
    int ancestor=f->navigation_owner?0:web_frame_ancestor_url(s->doc,url);
    if(ancestor){
        free(url);free(f->navigation_origin);f->navigation_origin=NULL;f->failed=true;
        log_text(s,ancestor<0?2:1,ancestor<0?"Frame ancestor URL comparison allocation failed":
                 "Frame src repeats an ancestor document URL (fragment excluded)");return;
    }
    struct js_pending *p=pending_new(s,P_FRAME,url);free(url);
    if(p && f->navigation_origin){p->origin=js_strdup(s->ctx,f->navigation_origin);if(!p->origin)pending_error(p,"Frame initiator could not be retained");}
    free(f->navigation_origin);f->navigation_origin=NULL;f->navigation_owner=NULL;
    if(!p){log_text(s,2,"Frame request queue or memory limit reached");return;}
    p->frame=f;f->request=p->id;f->notified=false;
    if(!p->done)send_request(s,p,WEB_RESOURCE_FRAME,"GET",NULL,NULL,0);
}
static void frames_walk(struct web_js_state *s,node_t *root,unsigned *order) {
    for(node_t *n=root;n;) {
        node_t *next=web_frame_dom_next(root,n);
        if(web_frame_element(n)){frame_sync(s,n);struct web_frame *f=web_frame_find(s->doc,n);if(f)f->dom_order=(*order)++;}
        n=next;
    }
}
void web_js_frames_sync(web_doc *d) {
    struct web_js_state *s=d?d->js:NULL;if(!s || !s->ctx || s->disabled)return;
    if(!s->frames_scanned || s->frames_revision!=d->dom_revision){
        uint64_t revision=d->dom_revision;
        unsigned order=0;frames_walk(s,d->root,&order);s->frames_scanned=true;s->frames_revision=revision;
    }
    for(struct web_frame *f=d->frames;f;f=f->next)if(f->navigation)frame_sync(s,f->element);
    for(struct web_frame *f=d->frames;f;f=f->next)if(!connected(s,f->element) || f->element->owner!=d) {
        frame_cancel(s,f);f->detached=true;f->window_token=NULL;web_frame_retire(f);
    }
}
static bool frame_response_allowed(struct web_js_state *s,const struct web_response *r,const char *url) {
    const char *headers=web_response_headers(r);
    for(const char *p=headers;*p;) {
        const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);
        const char *colon=memchr(p,':',(size_t)(end-p));
        if(colon && (size_t)(colon-p)==15 && !strncasecmp(p,"X-Frame-Options",15)){
            const char *v=colon+1;while(v<end && is_space(*v))v++;
            if((size_t)(end-v)>=4 && !strncasecmp(v,"DENY",4))return false;
            if((size_t)(end-v)>=10 && !strncasecmp(v,"SAMEORIGIN",10)){
                web_doc *probe=calloc(1,sizeof *probe);if(!probe)return false;probe->url=(char *)url;
                bool same=true;
                for(web_doc *ancestor=s->doc;ancestor;ancestor=ancestor->frame_parent)
                    if(!web_frame_same_origin(ancestor,probe)){same=false;break;}
                free(probe);if(!same)return false;
            }else return false;
        }
        if(colon && (size_t)(colon-p)==23 && !strncasecmp(p,"Content-Security-Policy",23)){
            for(const char *q=colon+1;q<end;) {
                while(q<end && (is_space(*q) || *q==';' || *q==','))q++;
                const char *word=q;while(q<end && !is_space(*q) && *q!=';' && *q!=',')q++;
                if(q-word==7 && !strncasecmp(word,"sandbox",7))return false; /* CSP union is not implemented; fail closed */
                while(q<end && *q!=';' && *q!=',')q++;
            }
            for(const char *q=colon+1;q+15<=end;q++)if(!strncasecmp(q,"frame-ancestors",15))return false;
        }
        p=*end?end+1:end;
    }
    return true;
}
static void frame_response(struct web_js_state *s,struct js_pending *p,bool ok) {
    struct web_frame *f=p->frame;
    if(!f || f->request!=p->id || !connected(s,f->element) || f->element->owner!=s->doc)return;
    f->request=0;
    const char *url=web_response_url(&p->response)[0]?web_response_url(&p->response):p->url;
    if(ok && permitted_url(s,url,false) && frame_response_allowed(s,&p->response,url) &&
       web_sandbox_document_allowed(f,url,false)) {
        if(web_frame_commit(f,p->response.body,p->response.body_len,url,NULL,&s->host,false))return;
        log_text(s,2,"Frame document allocation failed");
    }else log_text(s,1,"Frame document navigation failed or was blocked by its embedding policy");
    f->failed=true;f->notified=true;script_event(s,f->element,true);
}
static node_t *frame_window_token(web_doc *d){
    if(!d)return NULL;
    if(!d->frame_element)return d->root;
    return d->window_token;
}
static web_doc *frame_window_document(node_t *token) {
    if(!token)return NULL;
    if(token->type==N_DOC)return token->owner;
    if(token->type==N_FRAGMENT) {
        for(struct web_frame *f=token->owner->frames;f;f=f->next)
            if(f->window_token==token && !f->detached && doc_node_connected(f->element))return f->document;
        return NULL;
    }
    if(!doc_node_connected(token))return NULL;
    struct web_frame *f=web_frame_find(token->owner,token);
    return f && !f->detached?f->document:NULL;
}
static JSValue frame_security_error(JSContext *ctx) {
    return JS_ThrowTypeError(ctx,"SecurityError: cross-origin Window access is forbidden");
}
/* A Window message receiver is a genuine realm global or a canonical proxy in
 * this runtime's native registry, never an author-created lookalike/Proxy. */
/* The private transfer plan is fully materialized before the first detach.
 * Only ordinary existing writable data slots enter the final loop. QuickJS's
 * JS_SetPropertyInternal own-slot fast path is set_value, without allocation,
 * prototype lookup or JS calls. Retained old values defer finalization until
 * after the complete commit. */
struct frame_transfer_write {JSValue object,value,old;JSAtom key;};
struct frame_transfer_cancel {struct web_js_state *owner;uint32_t id;};
static JSValue frame_transfer_field(JSContext *ctx,JSValueConst object,const char *name,JSClassID ordinary) {
    if(JS_GetClassID(object)!=ordinary)return JS_ThrowTypeError(ctx,"Invalid private transfer record");
    JSAtom atom=JS_NewAtom(ctx,name);if(atom==JS_ATOM_NULL)return JS_EXCEPTION;
    JSPropertyDescriptor d;int found=JS_GetOwnProperty(ctx,&d,object,atom);JS_FreeAtom(ctx,atom);
    if(found<0)return JS_EXCEPTION;
    if(!found)return JS_ThrowTypeError(ctx,"Missing private transfer field");
    if(d.flags&JS_PROP_GETSET){JS_FreeValue(ctx,d.getter);JS_FreeValue(ctx,d.setter);return JS_ThrowTypeError(ctx,"Private transfer accessor forbidden");}
    return d.value;
}
static bool frame_transfer_length(JSContext *ctx,JSValueConst array,JSClassID array_class,uint32_t *length) {
    if(JS_GetClassID(array)!=array_class){JS_ThrowTypeError(ctx,"Invalid private transfer sequence");return false;}
    JSValue n=JS_GetPropertyStr(ctx,array,"length");
    bool ok=JS_IsNumber(n)&&!JS_ToUint32(ctx,length,n);JS_FreeValue(ctx,n);
    if(!ok)JS_ThrowTypeError(ctx,"Invalid private transfer length");return ok;
}
static JSValue frame_transfer_commit(JSContext *ctx,struct web_js_state *s,JSValueConst plan) {
    JSValue result=JS_EXCEPTION,sample=JS_NewObject(ctx),array=JS_NewArray(ctx),buffer=JS_NewArrayBufferCopy(ctx,NULL,0);
    if(JS_IsException(sample)||JS_IsException(array)||JS_IsException(buffer))goto samples;
    JSClassID ordinary=JS_GetClassID(sample),array_class=JS_GetClassID(array),buffer_class=JS_GetClassID(buffer);
    JSValue bs=frame_transfer_field(ctx,plan,"buffers",ordinary),ws=frame_transfer_field(ctx,plan,"writes",ordinary);
    JSValue cs=frame_transfer_field(ctx,plan,"cancels",ordinary),gs=frame_transfer_field(ctx,plan,"generations",ordinary);
    uint32_t nb=0,nw=0,nc=0,ng=0,built=0,held=0;
    struct frame_transfer_write *writes=NULL;struct frame_transfer_cancel *cancels=NULL;JSValue *buffers=NULL;
    if(JS_IsException(bs)||JS_IsException(ws)||JS_IsException(cs)||JS_IsException(gs))goto out;
    if(!frame_transfer_length(ctx,bs,array_class,&nb)||!frame_transfer_length(ctx,ws,array_class,&nw)||
       !frame_transfer_length(ctx,cs,array_class,&nc)||!frame_transfer_length(ctx,gs,array_class,&ng))goto out;
    if(nb>8192||nw>131072||nc>8192||ng>8192){JS_ThrowRangeError(ctx,"Private transfer plan quota reached");goto out;}
    if(!task_context_active(s)){JS_ThrowTypeError(ctx,"Transfer destination is inactive");goto out;}
    for(uint32_t i=0;i<ng;i++){
        JSValue value=JS_GetPropertyUint32(ctx,gs,i);node_t *generation=unwrap(ctx,value);JS_FreeValue(ctx,value);
        if(!generation||!generation->owner||generation->owner->root!=generation||
           !task_context_active(generation->owner->js)||JS_GetRuntime(generation->owner->js->ctx)!=JS_GetRuntime(ctx)){
            JS_ThrowTypeError(ctx,"Transfer generation is inactive");goto out;
        }
    }
    if(nb){buffers=js_malloc(ctx,nb*sizeof *buffers);if(!buffers)goto out;}
    if(nw){writes=js_mallocz(ctx,nw*sizeof *writes);if(!writes)goto out;}
    if(nc){cancels=js_malloc(ctx,nc*sizeof *cancels);if(!cancels)goto out;}
    for(uint32_t i=0;i<nb;i++){
        JSValue value=JS_GetPropertyUint32(ctx,bs,i);size_t size=0;
        if(JS_GetClassID(value)!=buffer_class){JS_FreeValue(ctx,value);JS_ThrowTypeError(ctx,"Invalid transfer buffer");goto out;}
        JS_GetArrayBuffer(ctx,&size,value);
        if(JS_HasException(ctx)){JS_FreeValue(ctx,value);goto out;}
        buffers[held++]=value;
    }
    for(uint32_t i=0;i<nw;i++){
        JSValue entry=JS_GetPropertyUint32(ctx,ws,i);
        JSValue object=frame_transfer_field(ctx,entry,"object",ordinary),key=frame_transfer_field(ctx,entry,"key",ordinary);
        JSValue value=frame_transfer_field(ctx,entry,"value",ordinary);JS_FreeValue(ctx,entry);
        if(JS_IsException(object)||JS_IsException(key)||JS_IsException(value)){JS_FreeValue(ctx,object);JS_FreeValue(ctx,key);JS_FreeValue(ctx,value);goto out;}
        if(JS_GetClassID(object)!=ordinary||!JS_IsString(key)){JS_FreeValue(ctx,object);JS_FreeValue(ctx,key);JS_FreeValue(ctx,value);JS_ThrowTypeError(ctx,"Invalid transfer write");goto out;}
        JSAtom atom=JS_ValueToAtom(ctx,key);JS_FreeValue(ctx,key);
        if(atom==JS_ATOM_NULL){JS_FreeValue(ctx,object);JS_FreeValue(ctx,value);goto out;}
        JSPropertyDescriptor d;int found=JS_GetOwnProperty(ctx,&d,object,atom);
        if(found!=1||(d.flags&(JS_PROP_TMASK|JS_PROP_WRITABLE|JS_PROP_LENGTH))!=JS_PROP_WRITABLE){
            if(found==1){JS_FreeValue(ctx,d.value);JS_FreeValue(ctx,d.getter);JS_FreeValue(ctx,d.setter);}
            JS_FreeAtom(ctx,atom);JS_FreeValue(ctx,object);JS_FreeValue(ctx,value);
            if(found>=0)JS_ThrowTypeError(ctx,"Transfer requires an existing writable data slot");goto out;
        }
        writes[built++]=(struct frame_transfer_write){object,value,d.value,atom};
    }
    for(uint32_t i=0;i<nc;i++){
        JSValue entry=JS_GetPropertyUint32(ctx,cs,i),gen=frame_transfer_field(ctx,entry,"generation",ordinary);
        JSValue id=frame_transfer_field(ctx,entry,"id",ordinary);JS_FreeValue(ctx,entry);
        node_t *generation=JS_IsException(gen)?NULL:unwrap(ctx,gen);uint32_t number=0;
        bool valid=generation&&generation->owner&&JS_IsNumber(id)&&!JS_ToUint32(ctx,&number,id);
        JS_FreeValue(ctx,gen);JS_FreeValue(ctx,id);
        if(!valid){if(!JS_HasException(ctx))JS_ThrowTypeError(ctx,"Invalid transfer cancellation");goto out;}
        struct web_js_state *owner=generation->owner->js;
        // Retirement has already freed that generation's native task queue.
        if(generation->owner->root!=generation||!task_context_active(owner))owner=NULL;
        cancels[i]=(struct frame_transfer_cancel){owner,number};
    }
    /* No allocation, JS_Call, property creation or string/number conversion
     * from this point through publication. All referenced slots are retained. */
    for(uint32_t i=0;i<nc;i++)if(cancels[i].owner){
        struct web_js_state *owner=cancels[i].owner;struct js_posted_task **link=&owner->posted,*previous=NULL;
        while(*link){struct js_posted_task *p=*link;if(p->id==cancels[i].id){
            *link=p->next;if(owner->last_posted==p)owner->last_posted=previous;owner->posted_count--;
            JS_FreeValue(owner->ctx,p->fn);js_free(owner->ctx,p);break;
        }previous=p;link=&p->next;}
    }
    for(uint32_t i=0;i<nb;i++)JS_DetachArrayBuffer(ctx,buffers[i]);
    for(uint32_t i=0;i<nw;i++){
        // The validated ordinary own-data fast path cannot fail/allocate.
        JS_SetProperty(ctx,writes[i].object,writes[i].key,JS_DupValue(ctx,writes[i].value));
    }
    result=JS_UNDEFINED;
out:
    for(uint32_t i=0;i<built;i++){JS_FreeValue(ctx,writes[i].object);JS_FreeValue(ctx,writes[i].value);JS_FreeValue(ctx,writes[i].old);JS_FreeAtom(ctx,writes[i].key);}
    for(uint32_t i=0;i<held;i++)JS_FreeValue(ctx,buffers[i]);
    js_free(ctx,writes);js_free(ctx,cancels);js_free(ctx,buffers);
    JS_FreeValue(ctx,bs);JS_FreeValue(ctx,ws);JS_FreeValue(ctx,cs);JS_FreeValue(ctx,gs);
samples:JS_FreeValue(ctx,sample);JS_FreeValue(ctx,array);JS_FreeValue(ctx,buffer);return result;
}
static web_doc *frame_message_receiver(struct web_js_state *s,JSValueConst receiver,bool *brand) {
    JSValue global=JS_GetGlobalObject(s->ctx);
    bool own=JS_StrictEq(s->ctx,global,receiver);JS_FreeValue(s->ctx,global);
    if(own){*brand=true;return s->doc;}
    struct web_js_state *active=s->runtime_owner->runtime_active;
    struct web_js_state *globals[]={s->runtime_owner,active};
    for(unsigned i=0;i<2;i++)if(globals[i]&&globals[i]->ctx) {
        global=JS_GetGlobalObject(globals[i]->ctx);own=JS_StrictEq(s->ctx,global,receiver);JS_FreeValue(s->ctx,global);
        if(own){*brand=true;return globals[i]->doc;}
    }
    for(struct js_frame_proxy *p=s->runtime_owner->frame_proxies;p;p=p->next)
        if(JS_StrictEq(s->ctx,p->proxy,receiver)){*brand=true;return frame_window_document(p->token);}
    *brand=false;return NULL;
}
static JSValue frame_message_deliver(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv,int magic,JSValue *data) {
    struct web_js_state *s=state(ctx);
    node_t *generation=unwrap(ctx,data[4]),*sender=unwrap(ctx,data[5]);
    if(!task_context_active(s) || !generation || generation->owner!=s->doc || generation!=s->doc->root)return JS_UNDEFINED;
    const char *expected=JS_ToCString(ctx,data[3]);if(!expected)return JS_EXCEPTION;
    char *current=NULL;
    enum http_url_result origin_status=make_origin(web_effective_url(s->doc),&current);
    if(origin_status==HTTP_URL_OOM){JS_FreeCString(ctx,expected);return JS_ThrowOutOfMemory(ctx);}
    bool origin_ok=origin_status==HTTP_URL_TUPLE;
    const char *sent_origin=JS_ToCString(ctx,data[2]);if(!sent_origin){free(current);JS_FreeCString(ctx,expected);return JS_EXCEPTION;}
    bool default_match=!strcmp(sent_origin,"null")?origin_status==HTTP_URL_OPAQUE&&sender&&web_frame_same_origin(sender->owner,s->doc):origin_ok&&!strcmp(sent_origin,current);
    bool allowed=!strcmp(expected,"*") || (!strcmp(expected,"/")?default_match:
        origin_ok&&strcmp(expected,"null")&&!strcmp(expected,current));
    free(current);JS_FreeCString(ctx,sent_origin);
    JS_FreeCString(ctx,expected);if(!allowed)return JS_UNDEFINED;
    JSValue global=JS_GetGlobalObject(ctx),args[]={global,data[0],data[1],data[2]};
    JSValue result=custom_element_hook(s,"windowMessageReceive",4,args);JS_FreeValue(ctx,global);return result;
}
/* Hold the original sender origin as an immutable engine string across author
   clone getters. Native bytes are freed before any author call; every exit
   leaves exactly one string reference for the outer caller to release. */
static JSValue frame_message_send_data(JSContext *ctx,struct web_js_state *s,
    struct web_js_state *caller,struct web_js_state *target,web_doc *document,
    JSValueConst sender_origin,JSValue *args,JSValueConst *argv) {
    (void)s;(void)args;
    node_t *sender_generation=caller->doc->root,*target_generation=document->root;
    JSValue prepared=custom_element_hook(caller,"windowMessagePrepare",2,(JSValue[]){argv[2],argv[4]});
    if(JS_IsException(prepared))return prepared;
    // Author clone getters can navigate/remove either context or post again.
    // Recheck before any publication, with no transfer committed yet.
    if(!task_context_active(caller) || caller->doc->root!=sender_generation || !task_context_active(target) ||
       target->doc->root!=target_generation || target->posted_count==UINT_MAX){JS_FreeValue(ctx,prepared);return JS_ThrowTypeError(ctx,"Window message context changed during serialization");}
    JSValue graph=JS_GetPropertyStr(caller->ctx,prepared,"data");
    if(JS_IsException(graph)){JS_FreeValue(ctx,prepared);return JS_EXCEPTION;}
    size_t length=0;uint8_t *bytes=JS_WriteObject(caller->ctx,&length,graph,JS_WRITE_OBJ_REFERENCE);JS_FreeValue(ctx,graph);
    if(!bytes){JS_FreeValue(ctx,prepared);return JS_EXCEPTION;}
    JSContext *dest=target->ctx;JSValue record=JS_ReadObject(dest,bytes,length,JS_READ_OBJ_REFERENCE);js_free(caller->ctx,bytes);
    if(JS_IsException(record)){JS_FreeValue(ctx,prepared);return record;}
    // Private endpoint capabilities remain JSValue references in this runtime;
    // they never enter JS_WriteObject or become ordinary author objects.
    JSValue imported=custom_element_hook(target,"windowMessageImport",2,(JSValue[]){record,prepared});
    JS_FreeValue(dest,record);
    if(JS_IsException(imported)){JS_FreeValue(ctx,prepared);return imported;}
    record=JS_GetPropertyStr(dest,imported,"packet");
    if(JS_IsException(record)){JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);return record;}
    JSValue token=wrap(target,frame_window_token(caller->doc));
    JSValue source=JS_IsException(token)?JS_EXCEPTION:custom_element_hook(target,"frameWindowProxy",1,&token);JS_FreeValue(dest,token);
    if(JS_IsException(source)){JS_FreeValue(dest,record);JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);return source;}
    if(!task_context_active(caller) || caller->doc->root!=sender_generation || !task_context_active(target) ||
       target->doc->root!=target_generation || target->posted_count==UINT_MAX){JS_FreeValue(dest,source);JS_FreeValue(dest,record);JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);return JS_ThrowTypeError(ctx,"Window message context changed before publication");}
    JSValue data[]={record,source,JS_DupValue(dest,sender_origin),JS_DupValue(dest,argv[3]),wrap(target,target_generation),wrap(target,sender_generation)};
    JSValue fn=JS_UNDEFINED;bool valid=true;
    for(unsigned i=0;i<6;i++)if(JS_IsException(data[i]))valid=false;
    if(valid)fn=JS_NewCFunctionData(dest,frame_message_deliver,0,0,6,data);
    for(unsigned i=0;i<6;i++)JS_FreeValue(dest,data[i]);
    if(!valid || JS_IsException(fn)){JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);return JS_EXCEPTION;}
    struct js_posted_task *p=js_malloc(dest,sizeof *p);
    if(!p){JS_FreeValue(dest,fn);JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);return JS_EXCEPTION;}
    // The destination graph, true MessagePort brands, nested queued messages,
    // callback and native queue node all exist before irreversible detach.
    JSValue committed=custom_element_hook(target,"windowMessageCommit",1,&imported);
    JS_FreeValue(dest,imported);JS_FreeValue(ctx,prepared);
    if(JS_IsException(committed)){js_free(dest,p);JS_FreeValue(dest,fn);return committed;}
    JS_FreeValue(dest,committed);
    p->fn=fn;p->next=NULL;p->id=allocate_posted_id(target);
    if(target->last_posted)target->last_posted->next=p;else target->posted=p;
    target->last_posted=p;target->posted_count++;return JS_UNDEFINED;
}
static JSValue frame_message_send(JSContext *ctx,struct web_js_state *s,struct web_js_state *caller,int argc,JSValueConst *argv) {
    bool brand=false;web_doc *document=argc>1?frame_message_receiver(s,argv[1],&brand):NULL;
    if(!brand)return JS_ThrowTypeError(ctx,"Illegal Window receiver");
    struct web_js_state *target=document?document->js:NULL;
    if(argc<5 || !task_context_active(caller) || !task_context_active(target))return JS_ThrowTypeError(ctx,"Window message browsing context is inactive");
    JSValue args[]={argv[2],argv[3],argv[4]};
    if(caller==target)return custom_element_hook(target,"windowMessageLocal",3,args);
    if(JS_GetRuntime(caller->ctx)!=JS_GetRuntime(target->ctx))return JS_ThrowTypeError(ctx,"Window transfers require the same runtime");
    if(target->posted_count==UINT_MAX)return JS_ThrowRangeError(ctx,"Document posted task count cannot be represented");
    char *origin=NULL;
    enum http_url_result status=make_origin(web_effective_url(caller->doc),&origin);
    if(status==HTTP_URL_OOM)return JS_ThrowOutOfMemory(ctx);
    if(status==HTTP_URL_INVALID)return JS_ThrowTypeError(ctx,"Window message sender origin is unavailable");
    JSValue sender_origin=JS_NewString(ctx,status==HTTP_URL_TUPLE?origin:"null");
    free(origin);
    if(JS_IsException(sender_origin))return sender_origin;
    JSValue result=frame_message_send_data(ctx,s,caller,target,document,sender_origin,args,argv);
    JS_FreeValue(ctx,sender_origin);return result;
}

static JSValue native_frame(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    struct web_js_state *caller=sandbox_actor(s);
    if(!s->starting && (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox disallows scripted Window access");
    const char *op=argc?JS_ToCString(ctx,argv[0]):NULL;if(!op)return JS_EXCEPTION;
    if(!strcmp(op,"transferGeneration")||!strcmp(op,"transferCommit")){
        JSValue result=!strcmp(op,"transferGeneration")?wrap(s,s->doc->root):
            argc>1?frame_transfer_commit(ctx,s,argv[1]):JS_ThrowTypeError(ctx,"Private transfer plan required");
        JS_FreeCString(ctx,op);return result;
    }
    if(!strcmp(op,"messageBrand") || !strcmp(op,"message")) {
        bool brand=false;
        if(argc>1)frame_message_receiver(s,argv[1],&brand);
        JSValue result=!strcmp(op,"messageBrand")?JS_NewBool(ctx,brand):frame_message_send(ctx,s,caller,argc,argv);
        JS_FreeCString(ctx,op);return result;
    }
    node_t *token=argc>1?unwrap(ctx,argv[1]):NULL;
    web_doc *target=token?frame_window_document(token):s->doc;
    JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"proxyGet") || !strcmp(op,"proxySet")) {
        struct js_frame_proxy *p=s->runtime_owner->frame_proxies;
        while(p && p->token!=token)p=p->next;
        if(!strcmp(op,"proxyGet"))result=p?JS_DupValue(ctx,p->proxy):JS_NULL;
        else if(p)result=JS_DupValue(ctx,p->proxy);
        else if(!token || argc<3 || !JS_IsObject(argv[2]))result=JS_ThrowTypeError(ctx,"WindowProxy registration requires a native token and proxy");
        else {
            p=js_mallocz(s->runtime_owner->ctx,sizeof *p);
            if(!p)result=oom(ctx);
            else {p->token=token;p->proxy=JS_DupValue(ctx,argv[2]);p->next=s->runtime_owner->frame_proxies;s->runtime_owner->frame_proxies=p;result=JS_DupValue(ctx,p->proxy);}
        }
    }else if(!strcmp(op,"window") || !strcmp(op,"document")) {
        if(!web_frame_element(token)){result=JS_ThrowTypeError(ctx,"Frame element receiver required");goto out;}
        frame_sync(s,token);target=frame_window_document(token);
        if(!target){result=JS_NULL;goto out;}
        if(!strcmp(op,"window"))result=wrap(s,frame_window_token(target));
        else if(!target->js || !target->js->ctx || (target->sandbox_flags&SB_SCRIPTS) || !web_frame_same_origin(caller->doc,target))result=JS_NULL;
        else {JSValue global=JS_GetGlobalObject(target->js->ctx);result=JS_GetPropertyStr(ctx,global,"document");JS_FreeValue(ctx,global);}
    }else if(!strcmp(op,"self"))result=wrap(s,frame_window_token(s->doc));
    else if(!strcmp(op,"parent") || !strcmp(op,"top")){
        web_doc *d=target;
        if(d && d->frame_parent)d=d->frame_parent;
        if(!strcmp(op,"top"))while(d && d->frame_parent)d=d->frame_parent;
        result=wrap(s,frame_window_token(d));
    }else if(!strcmp(op,"closed"))result=JS_NewBool(ctx,!target || !target->live);
    else if(!strcmp(op,"opener")) {
        /* No native Window in this implementation has an opener browsing
           context. The original getter is cross-origin readable as null;
           same-origin author replacements remain ordinary property values. */
        if(!target || !target->live || !target->js || !target->js->ctx || (target->sandbox_flags&SB_SCRIPTS) || !web_frame_same_origin(caller->doc,target))result=JS_NULL;
        else {JSValue global=JS_GetGlobalObject(target->js->ctx);result=JS_GetPropertyStr(ctx,global,"opener");JS_FreeValue(ctx,global);}
    }
    else if(!strcmp(op,"length") || !strcmp(op,"index")) {
        unsigned count=0;uint32_t index=0;
        if(!strcmp(op,"index") && (argc<3 || JS_ToUint32(ctx,&index,argv[2]))) {result=JS_EXCEPTION;goto out;}
        result=!strcmp(op,"index")?JS_UNDEFINED:JS_NewInt32(ctx,0);
        if(target){
            web_js_frames_sync(target);
            for(struct web_frame *f=target->frames;f;f=f->next)if(f->document && !f->detached){
                unsigned rank=0;
                for(struct web_frame *other=target->frames;other;other=other->next)
                    if(other->document && !other->detached && other->dom_order<f->dom_order)rank++;
                if(!strcmp(op,"index") && rank==index){result=wrap(s,f->window_token);break;}count++;
            }
        }
        if(!strcmp(op,"length"))result=JS_NewUint32(ctx,count);
    }else if(!strcmp(op,"frameElement"))result=target && target->frame_parent && web_frame_same_origin(caller->doc,target->frame_parent)?wrap(s,target->frame_element):JS_NULL;
    else if(!strcmp(op,"navigate")) {
        if(!target){result=history_security_error(ctx,"Inactive navigation target");goto out;}
        bool activated=caller->doc->sandbox_activation_until>uptime_ms();
        if(!web_sandbox_navigation_allowed(caller->doc,target,activated)){result=history_security_error(ctx,"Sandbox denied Window navigation destination");goto out;}
        if(!target->frame_element){result=JS_ThrowTypeError(ctx,"Top-level proxy navigation is not implemented");goto out;}
        if(!web_frame_same_origin(caller->doc,target) && caller->doc!=target->frame_parent){result=frame_security_error(ctx);goto out;}
        const char *value=argc>2?JS_ToCString(ctx,argv[2]):NULL;
        struct web_frame *f=web_frame_find(target->frame_parent,target->frame_element);
        if(!value)result=JS_EXCEPTION;
        else {
            char *url=js_resolve_owned(ctx,caller->doc->base,value);
            if(!url)result=JS_EXCEPTION;
            else if(!permitted_url(caller,url,false))result=JS_ThrowTypeError(ctx,"Child navigation URL is not permitted");
            else if(!task_context_active(caller) || frame_window_document(token)!=target || !web_sandbox_navigation_allowed(caller->doc,target,caller->doc->sandbox_activation_until>uptime_ms()))result=history_security_error(ctx,"Navigation target or authority retired during conversion");
            else if(!web_frame_set_navigation(f,url,caller->doc))result=oom(ctx);
            js_free(ctx,url);
        }
        JS_FreeCString(ctx,value);
    }else if(!strcmp(op,"messageMethod")) {
        if(!target || !task_context_active(target->js)){result=JS_ThrowTypeError(ctx,"Child Window is closed");goto out;}
        result=JS_GetPropertyStr(ctx,s->hooks,"windowMessageMethod");
        if(web_frame_same_origin(caller->doc,target)) {
            JSValue global=JS_GetGlobalObject(target->js->ctx),value=JS_GetPropertyStr(ctx,global,"postMessage");JS_FreeValue(ctx,global);
            JSValue original=JS_GetPropertyStr(ctx,target->js->hooks,"windowMessageMethod");
            if(JS_IsException(value)||JS_IsException(original)){
                JS_FreeValue(ctx,result);result=JS_EXCEPTION;
            }else if(!JS_SameValue(ctx,value,original)){
                JS_FreeValue(ctx,result);result=value;value=JS_UNDEFINED;
            }
            JS_FreeValue(ctx,original);JS_FreeValue(ctx,value);
        }
    }else {
        if(target && (target->sandbox_flags&SB_SCRIPTS)){result=history_security_error(ctx,"Sandbox initial Document does not export author realm objects");goto out;}
        if(!target || !target->js || !target->js->ctx){result=JS_ThrowTypeError(ctx,"Child Window is closed");goto out;}
        if(!web_frame_same_origin(caller->doc,target)){result=frame_security_error(ctx);goto out;}
        if(caller->doc->sandbox_flags && caller->doc!=target){result=history_security_error(ctx,"Sandbox borrowed realm methods are outside the supported subset");goto out;}
        bool calling=!strcmp(op,"call");
        const char *key=!calling && argc>2?JS_ToCString(ctx,argv[2]):NULL;
        if(!calling && !key){result=JS_EXCEPTION;goto out;}
        JSValue global=JS_GetGlobalObject(target->js->ctx);
        if(!strcmp(op,"get"))result=JS_GetPropertyStr(ctx,global,key);
        else if(!strcmp(op,"method")) {
            JSValue name=JS_NewString(ctx,key);
            JSValue original=JS_IsException(name)?JS_EXCEPTION:custom_element_hook(target->js,"frameMethodOriginal",1,&name);
            JS_FreeValue(ctx,name);
            JSValue value=JS_IsException(original)?JS_EXCEPTION:JS_GetPropertyStr(ctx,global,key);
            if(JS_IsException(value))result=value;
            else {
                JSValue data[]={value,wrap(s,target->root),JS_NewBool(ctx,JS_SameValue(ctx,value,original))};
                result=JS_NewArray(ctx);
                for(unsigned i=0;i<3;i++) {
                    if(JS_IsException(result))break;
                    if(JS_IsException(data[i])){JS_FreeValue(ctx,result);result=JS_EXCEPTION;break;}
                    int status=JS_SetPropertyUint32(ctx,result,i,data[i]);data[i]=JS_UNDEFINED;
                    if(status<0){JS_FreeValue(ctx,result);result=JS_EXCEPTION;break;}
                }
                for(unsigned i=0;i<3;i++)JS_FreeValue(ctx,data[i]);
            }
            JS_FreeValue(ctx,original);
        }else if(calling){
            // Bind the getter's exact function and Document generation. A
            // later author wrapper must not redirect the saved original back
            // through a fresh global[property] lookup and recurse forever.
            node_t *document=argc>4?unwrap(ctx,argv[4]):NULL;
            if(!document || document->type!=N_DOC || document->owner!=target || !target->live || target->js->disabled){
                result=JS_ThrowTypeError(ctx,"Captured Window method belongs to an inactive document");
                JS_FreeValue(ctx,global);goto out;
            }
            JSValue function=argc>2?JS_DupValue(ctx,argv[2]):JS_UNDEFINED,*args=NULL;unsigned count=0;
            JSValue length=argc>3?JS_GetPropertyStr(ctx,argv[3],"length"):JS_UNDEFINED;
            uint32_t n=0;bool valid=JS_IsFunction(ctx,function) && argc>3 && JS_ToUint32(ctx,&n,length)==0 &&
                n<=INT_MAX && (size_t)n<=SIZE_MAX/sizeof *args;
            JS_FreeValue(ctx,length);
            if(valid && n){args=js_malloc(ctx,(size_t)n*sizeof *args);if(!args)valid=false;}
            if(valid)for(;count<n;count++){args[count]=JS_GetPropertyUint32(ctx,argv[3],count);if(JS_IsException(args[count])){valid=false;count++;break;}}
            if(valid && target->live && !target->js->disabled && frame_window_document(token)==target) {
                begin_task(target->js);result=JS_Call(ctx,function,global,(int)count,args);end_task(target->js);
            } else result=JS_HasException(ctx)?JS_EXCEPTION:JS_ThrowTypeError(ctx,"Window method arguments or captured context are invalid");
            for(unsigned i=0;i<count;i++)JS_FreeValue(ctx,args[i]);js_free(ctx,args);JS_FreeValue(ctx,function);
        }
        else if(!strcmp(op,"set"))result=JS_NewBool(ctx,argc>3 && JS_SetPropertyStr(ctx,global,key,JS_DupValue(ctx,argv[3]))>=0);
        else if(!strcmp(op,"has")){JSAtom atom=JS_NewAtom(ctx,key);result=JS_NewBool(ctx,JS_HasProperty(ctx,global,atom)>0);JS_FreeAtom(ctx,atom);}
        JS_FreeValue(ctx,global);JS_FreeCString(ctx,key);
    }
out:JS_FreeCString(ctx,op);return result;
}
static JSValue native_document_stream(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);int32_t operation=0;
    if(argc && JS_ToInt32(ctx,&operation,argv[0]))return JS_EXCEPTION;
    if(s->disabled || !s->doc->live)return JS_ThrowTypeError(ctx,"Document stream belongs to an inactive browsing context");
    if((sandbox_authority(s)&SB_SCRIPTS) || sandbox_borrowed(s))return history_security_error(ctx,"Sandbox denied document stream authority");
    if(!s->doc->frame_parent)return JS_ThrowTypeError(ctx,"Explicit document.open streams are currently supported in child browsing contexts");
    if(operation==0){
        // Parser-executed scripts cannot replace their active input stream.
        if(s->parser_write)return wrap(s,s->doc->root);
        struct web_frame *f=web_frame_find(s->doc->frame_parent,s->doc->frame_element);
        if(!f || f->document!=s->doc || f->detached)return JS_ThrowTypeError(ctx,"Document is not the active frame document");
        JSValue reset=JS_GetPropertyStr(ctx,s->hooks,"documentOpenReset"),root=wrap(s,s->doc->root);
        JSValue cleared=JS_IsException(reset)||JS_IsException(root)?JS_EXCEPTION:JS_Call(ctx,reset,s->hooks,1,&root);
        JS_FreeValue(ctx,reset);JS_FreeValue(ctx,root);
        if(JS_IsException(cleared))return cleared;JS_FreeValue(ctx,cleared);
        html_finish(s->doc->parser);s->doc->parser=NULL;
        while(s->doc->root->first)doc_node_remove(s->doc,s->doc->root->first);
        for(struct js_script *script=s->scripts;script;script=script->next)script->executed=true;
        s->blocker=NULL;s->parsing_done=s->domcontent_sent=s->load_sent=false;
        s->doc->parser=html_open(s->doc);
        if(!s->doc->parser)return oom(ctx);
        s->document_open=s->document_waiting=true;
        // A new stream has its own child/embedding-element load lifecycle,
        // although the Document and WindowProxy objects themselves are reused.
        f->notified=false;f->failed=false;s->doc->frame_parent->dirty=true;
        return wrap(s,s->doc->root);
    }
    if(operation==1 && s->document_open){
        html_close(s->doc->parser);s->document_open=s->document_waiting=false;
        if(!frame_stream_pump(s))return oom(ctx);
    }
    return JS_UNDEFINED;
}
static bool frame_stream_pump(struct web_js_state *s) {
    if(s->blocker && !s->blocker->executed)return true;
    for(unsigned step=0;step<128 && s->doc->parser;step++){
        node_t *node=NULL;int result=html_resume(s->doc->parser,&node);
        if(result<0 || html_import_changed(s->doc->parser))s->doc->dirty=s->doc->resources_dirty=true;
        if(result<0)return false;
        if(result==2){s->document_waiting=true;return true;}
        if(!result){html_finish(s->doc->parser);s->doc->parser=NULL;s->parsing_done=true;return true;}
        if(node && node->tag==T_script){
            struct js_script *script=queue_script(s,node,false);
            if(script && !script->deferred && !script->asynchronous){
                if(!script->ready){s->blocker=script;return true;}
                struct js_script *previous=s->blocker;s->blocker=script;
                run_script(s,script);s->blocker=previous;
                if(s->disabled)return false;
            }
        }
    }
    return true;
}
