/* Native MutationObserver bookkeeping. No JS DOM reads on every insert. The
 * observer/options are mirrored once at observe(); records keep actual native
 * node identities and mutation-time observer membership until a microtask. */
enum { MO_CHILD=1, MO_ATTR=2, MO_DATA=4, MO_SUBTREE=8, MO_ATTR_OLD=16, MO_DATA_OLD=32, MO_FILTER=64 };
struct js_mutation_registration {
    struct js_mutation_registration *next;
    node_t *node,*source_node;
    JSValue observer;
    unsigned flags;
    char **filter;
    unsigned filter_count;
    bool transient;
};
struct js_mutation_recipient { JSValue observer; bool old; };
struct js_mutation_record {
    struct js_mutation_record *next;
    int type;
    node_t *target,*previous,*following;
    pvec added,removed;
    char *name,*namespace_uri;
    const char *old_value; /* immutable DOM-arena text, alive until runtime free */
    size_t old_length;
    struct js_mutation_recipient *recipients;
    unsigned recipient_count;
};
struct js_mutation_snapshot {
    node_t *node,*target;
    int mode; /* 1 insert, 2 replace, 3 remove, 4 children, 5 character data */
    pvec nodes;
    struct js_mutation_record *first,*last;
    bool style_only;
};
static bool mutation_same(JSValueConst a,JSValueConst b) {return JS_VALUE_GET_PTR(a)==JS_VALUE_GET_PTR(b);}
static bool mutation_under(node_t *n,node_t *ancestor) {for(;n;n=n->parent)if(n==ancestor)return true;return false;}
static void mutation_registration_free(struct web_js_state *s,struct js_mutation_registration *r) {
    JS_FreeValue(s->ctx,r->observer);
    for(unsigned i=0;i<r->filter_count;i++)free(r->filter[i]);
    free(r->filter);free(r);
}
static void mutation_record_free(struct web_js_state *s,struct js_mutation_record *r) {
    for(unsigned i=0;i<r->recipient_count;i++)JS_FreeValue(s->ctx,r->recipients[i].observer);
    free(r->recipients);free(r->name);free(r->namespace_uri);
    pv_free(&r->added);pv_free(&r->removed);free(r);
}
static bool mutation_filter(struct js_mutation_registration *r,const char *name,const char *ns) {
    if(!(r->flags&MO_FILTER))return true;
    if(ns)return false;
    for(unsigned i=0;i<r->filter_count;i++)if(!strcmp(r->filter[i],name))return true;
    return false;
}
static struct js_mutation_record *mutation_record(struct web_js_state *s,int type,node_t *target,const char *name,const char *ns,const char *old) {
    struct js_mutation_record *e=calloc(1,sizeof *e);if(!e)return NULL;
    e->type=type;e->target=target;
    if(name)e->name=strdup(name);if(ns)e->namespace_uri=strdup(ns);e->old_value=old;
    e->old_length=old?SIZE_MAX:0; /* Attr values forbid NUL; measure only if requested. */
    if((name&&!e->name)||(ns&&!e->namespace_uri))goto fail;
    for(struct js_mutation_registration *r=s->mutation_registrations;r;r=r->next) {
        if(!(r->flags&type) || (r->node!=target && (!(r->flags&MO_SUBTREE)||!mutation_under(target,r->node))) ||
                (type==MO_ATTR&&!mutation_filter(r,name,ns)))continue;
        bool keep_old=!!(r->flags&(type==MO_ATTR?MO_ATTR_OLD:type==MO_DATA?MO_DATA_OLD:0));
        unsigned i=0;for(;i<e->recipient_count;i++)if(mutation_same(e->recipients[i].observer,r->observer))break;
        if(i<e->recipient_count){e->recipients[i].old|=keep_old;continue;}
        void *next=realloc(e->recipients,(size_t)(i+1)*sizeof *e->recipients);if(!next)goto fail;
        e->recipients=next;e->recipients[i]=(struct js_mutation_recipient){JS_DupValue(s->ctx,r->observer),keep_old};e->recipient_count++;
    }
    if(e->recipient_count)return e;
fail:
    mutation_record_free(s,e);return NULL;
}
static void mutation_snapshot_add(struct js_mutation_snapshot *c,struct js_mutation_record *r) {
    if(!r)return;if(c->last)c->last->next=r;else c->first=r;c->last=r;
}
static void mutation_schedule(struct web_js_state *s);
static void mutation_transient(struct web_js_state *s,node_t *removed,node_t *parent) {
    /* Detached subtrees retain their old subtree registrations until delivery.
     * Snapshot the original list head so newly appended copies cannot recurse. */
    struct js_mutation_registration *stop=s->mutation_registrations;
    for(struct js_mutation_registration *r=stop;r;r=r->next) {
        if(!(r->flags&MO_SUBTREE)||!mutation_under(parent,r->node))continue;
        bool exists=false;for(struct js_mutation_registration *t=s->mutation_registrations;t;t=t->next)
            if(t->transient&&t->node==removed&&mutation_same(t->observer,r->observer)&&t->source_node==(r->transient?r->source_node:r->node))exists=true;
        if(exists)continue;
        struct js_mutation_registration *t=calloc(1,sizeof *t);if(!t)continue;
        t->node=removed;t->observer=JS_DupValue(s->ctx,r->observer);t->flags=r->flags;t->transient=true;
        t->source_node=r->transient?r->source_node:r->node;
        if(r->filter_count){t->filter=calloc(r->filter_count,sizeof *t->filter);if(!t->filter){mutation_registration_free(s,t);continue;}
            for(unsigned i=0;i<r->filter_count;i++){t->filter[i]=strdup(r->filter[i]);if(!t->filter[i])break;t->filter_count++;}
            if(t->filter_count!=r->filter_count){mutation_registration_free(s,t);continue;}}
        t->next=s->mutation_registrations;s->mutation_registrations=t;
        mutation_schedule(s); /* transient expiry also needs a job without childList interest */
    }
}
static void mutation_clear_transients(struct web_js_state *s,JSValueConst observer,bool all) {
    struct js_mutation_registration **link=&s->mutation_registrations;
    while(*link){struct js_mutation_registration *r=*link;
        if(r->transient&&(all||mutation_same(r->observer,observer))){*link=r->next;mutation_registration_free(s,r);}else link=&r->next;}
}
static JSValue mutation_notify_job(JSContext *ctx,int argc,JSValueConst *argv) {
    (void)argc;(void)argv;struct web_js_state *s=state(ctx);s->mutation_job_pending=false;
    /* Removal's transient registrations expire before observer callbacks, but
     * takeRecords() alone must not expire them. */
    mutation_clear_transients(s,JS_UNDEFINED,true);
    return custom_element_hook(s,"mutationFlush",0,NULL);
}
static void mutation_schedule(struct web_js_state *s) {
    if(!s->mutation_job_pending){s->mutation_job_pending=true;
        if(JS_EnqueueJob(s->ctx,mutation_notify_job,0,NULL)<0){s->mutation_job_pending=false;exception(s);}}
}
static void mutation_publish(struct web_js_state *s,struct js_mutation_record *r) {
    if(!r)return;r->next=NULL;
    if(s->mutation_last)s->mutation_last->next=r;else s->mutation_first=r;s->mutation_last=r;
    mutation_schedule(s);
}
static void mutation_removed(struct web_js_state *s,struct js_mutation_snapshot *c,node_t *n,bool suppressed) {
    if(!n||!n->parent)return;
    node_t *p=n->parent;mutation_transient(s,n,p);
    if(suppressed)return;
    struct js_mutation_record *e=mutation_record(s,MO_CHILD,p,NULL,NULL,NULL);
    if(e){e->previous=n->prev;e->following=n->next;pv_push(&e->removed,n);mutation_snapshot_add(c,e);}
}
static struct js_mutation_snapshot mutation_before(struct web_js_state *s,int opcode,node_t *n,int argc,JSValueConst *argv) {
    struct js_mutation_snapshot c={0};if(!s->mutation_registrations||!n)return c;
    node_t *incoming=argc>2?node_opaque(argv[2]):NULL,*old=argc>3?node_opaque(argv[3]):NULL;
    c.node=n;
    node_t *element=n,*attribute=NULL;const char *name=NULL,*ns=NULL,*owned_name=NULL,*owned_ns=NULL;
    if(opcode==DOM_attr&&argc>3&&JS_IsString(argv[2]))owned_name=name=JS_ToCString(s->ctx,argv[2]);
    else if(opcode==DOM_attrNS&&argc>4&&JS_IsString(argv[3])){
        owned_name=name=JS_ToCString(s->ctx,argv[3]);if(JS_IsString(argv[2]))owned_ns=ns=JS_ToCString(s->ctx,argv[2]);
    }else if((opcode==DOM_attrSetNode||opcode==DOM_attrRemoveNode)&&incoming&&incoming->type==N_ATTR){
        if(opcode==DOM_attrSetNode&&incoming->attr_owner==n)return c;
        name=incoming->attribute->local?incoming->attribute->local:incoming->attribute->raw;ns=incoming->attribute->namespace_uri;
    }else if(opcode==DOM_adopt&&incoming&&incoming->type==N_ATTR){attribute=incoming;element=incoming->attr_owner;}
    else if(opcode==DOM_style&&argc>3){name="style";c.style_only=true;}
    else if(opcode==DOM_set&&argc>3&&JS_IsString(argv[2])){
        const char *key=JS_ToCString(s->ctx,argv[2]);
        if(key){
            if(n->type==N_ATTR&&(!strcmp(key,"attrValue")||!strcmp(key,"nodeValue")||!strcmp(key,"textContent"))){attribute=n;element=n->attr_owner;}
            else if(!strcmp(key,"imageWidth"))name="width";else if(!strcmp(key,"imageHeight"))name="height";
            else if(!strcmp(key,"async")&&n->tag==T_script)name="async";
            else if(!strcmp(key,"value")&&n->tag==T_option)name="value";
        }JS_FreeCString(s->ctx,key);
    }
    if(attribute){name=attribute->attribute->local?attribute->attribute->local:attribute->attribute->raw;ns=attribute->attribute->namespace_uri;}
    if(name&&element&&element->type==N_ELEM){
        int i=doc_attr_index(element,ns,name,opcode!=DOM_attr);const char *oldvalue=i>=0?element->attrs[i].value:NULL;
        if(i>=0){name=element->attrs[i].local?element->attrs[i].local:element->attrs[i].raw;ns=element->attrs[i].namespace_uri;}
        char *normalized=NULL;
        if(i<0&&opcode==DOM_attr&&!element->foreign){normalized=strdup(name);if(normalized){for(char *p=normalized;*p;p++)*p=(char)lower((unsigned char)*p);name=normalized;}}
        c.mode=6;c.target=element;mutation_snapshot_add(&c,mutation_record(s,MO_ATTR,element,name,ns,oldvalue));free(normalized);
    }
    JS_FreeCString(s->ctx,owned_name);JS_FreeCString(s->ctx,owned_ns);
    if(c.mode==6)return c;
    if(opcode==DOM_insert||opcode==DOM_replace){
        if(!incoming||(opcode==DOM_insert&&incoming==old))return c;
        c.mode=opcode==DOM_insert?1:2;c.target=n;
        bool fragment=incoming->type==N_FRAGMENT;
        if(fragment){for(node_t *child=incoming->first;child;child=child->next)pv_push(&c.nodes,child);
            for(int i=0;i<c.nodes.n;i++)mutation_removed(s,&c,c.nodes.v[i],true);
            struct js_mutation_record *e=mutation_record(s,MO_CHILD,incoming,NULL,NULL,NULL);
            if(e&&c.nodes.n){for(int i=0;i<c.nodes.n;i++)pv_push(&e->removed,c.nodes.v[i]);mutation_snapshot_add(&c,e);}else if(e)mutation_record_free(s,e);
        }else{pv_push(&c.nodes,incoming);mutation_removed(s,&c,incoming,false);}
        if(c.mode==2&&old&&old!=incoming)mutation_removed(s,&c,old,true);
        struct js_mutation_record *e=mutation_record(s,MO_CHILD,n,NULL,NULL,NULL);
        if(e){if(c.mode==2&&old){e->previous=old->prev;e->following=old->next==incoming?incoming->next:old->next;
                if(old!=incoming)pv_push(&e->removed,old);}
            mutation_snapshot_add(&c,e);}
    }else if(opcode==DOM_remove||(opcode==DOM_adopt&&incoming&&incoming->type!=N_ATTR)){
        c.mode=3;c.node=opcode==DOM_adopt?incoming:n;mutation_removed(s,&c,c.node,false);
    }else if(opcode==DOM_set&&argc>3&&JS_IsString(argv[2])){
        const char *key=JS_ToCString(s->ctx,argv[2]);
        if(key&&(!strcmp(key,"innerHTML")||!strcmp(key,"textContent")||!strcmp(key,"nodeValue"))){
            if(strcmp(key,"innerHTML")&&(n->type==N_TEXT||n->type==N_COMMENT||n->type==N_PI)){
                c.mode=5;struct js_mutation_record *e=mutation_record(s,MO_DATA,n,NULL,NULL,n->text?n->text:"");
                if(e){e->old_length=n->textlen;mutation_snapshot_add(&c,e);}
            }
            else if(strcmp(key,"nodeValue")&&(n->type==N_ELEM||n->type==N_FRAGMENT)){
                if(n->tag==T_template&&n->template_content)n=n->template_content;c.mode=4;c.target=n;
                struct js_mutation_record *e=mutation_record(s,MO_CHILD,n,NULL,NULL,NULL);
                for(node_t *child=n->first;child;child=child->next){mutation_removed(s,&c,child,true);if(e)pv_push(&e->removed,child);}
                mutation_snapshot_add(&c,e);
            }
        }
        JS_FreeCString(s->ctx,key);
    }
    return c;
}
static void mutation_after(struct web_js_state *s,struct js_mutation_snapshot *c,bool success) {
    struct js_mutation_record *e=c->first;
    while(e){struct js_mutation_record *next=e->next;
        if(success){
            if(e->target==c->target){
                if(c->mode==1||c->mode==2){for(int i=0;i<c->nodes.n;i++)pv_push(&e->added,c->nodes.v[i]);
                    if(c->mode==1&&c->nodes.n){node_t *first=c->nodes.v[0],*last=c->nodes.v[c->nodes.n-1];e->previous=first->prev;e->following=last->next;}}
                else if(c->mode==4)for(node_t *child=c->target->first;child;child=child->next)pv_push(&e->added,child);
            }
            bool changed=true;
            if(e->type==MO_ATTR){int i=doc_attr_index(e->target,e->namespace_uri,e->name,true);const char *now=i>=0?e->target->attrs[i].value:NULL;
                changed=now||e->old_value;if(c->style_only)changed=changed&&(!now||!e->old_value||strcmp(now,e->old_value));}
            if(changed&&(e->type!=MO_CHILD||e->added.n||e->removed.n))mutation_publish(s,e);else mutation_record_free(s,e);
        }else mutation_record_free(s,e);
        e=next;
    }
    pv_free(&c->nodes);
}
static JSValue native_mutation_control(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    (void)this_val;struct web_js_state *s=state(ctx);int32_t flags=0;
    if(argc<2||!JS_IsObject(argv[0]))return JS_ThrowTypeError(ctx,"MutationObserver registration required");
    JSValueConst observer=argv[0];node_t *target=JS_IsNull(argv[1])?NULL:unwrap(ctx,argv[1]);
    if(target&&argc>2&&JS_ToInt32(ctx,&flags,argv[2])<0)return JS_EXCEPTION;
    struct js_mutation_registration *made=NULL;
    if(target){made=calloc(1,sizeof *made);if(!made)return oom(ctx);
        made->node=target;made->observer=JS_DupValue(ctx,observer);made->flags=(unsigned)flags;
        if((flags&MO_FILTER)&&argc>3){JSValue length=JS_GetPropertyStr(ctx,argv[3],"length");uint32_t count=0;
            int status=JS_ToUint32(ctx,&count,length);JS_FreeValue(ctx,length);if(status<0)goto failed;
            made->filter=count?calloc(count,sizeof *made->filter):NULL;if(count&&!made->filter)goto failed;
            for(uint32_t i=0;i<count;i++){JSValue value=JS_GetPropertyUint32(ctx,argv[3],i);const char *text=JS_ToCString(ctx,value);JS_FreeValue(ctx,value);
                if(!text)goto failed;made->filter[i]=strdup(text);JS_FreeCString(ctx,text);if(!made->filter[i])goto failed;made->filter_count++;}
        }
    }
    struct js_mutation_registration **link=&s->mutation_registrations;
    while(*link){struct js_mutation_registration *r=*link;
        if(mutation_same(r->observer,observer)&&(!target||(!r->transient&&r->node==target)||(r->transient&&r->source_node==target))){*link=r->next;mutation_registration_free(s,r);}else link=&r->next;}
    if(made){made->next=s->mutation_registrations;s->mutation_registrations=made;}
    return JS_UNDEFINED;
failed:
    mutation_registration_free(s,made);return oom(ctx);
}
static JSValue mutation_nodes(struct web_js_state *s,pvec *nodes) {
    JSValue array=JS_NewArray(s->ctx);
    for(int i=0;!JS_IsException(array)&&i<nodes->n;i++)if(JS_SetPropertyUint32(s->ctx,array,(uint32_t)i,wrap(s,nodes->v[i]))<0){JS_FreeValue(s->ctx,array);return JS_EXCEPTION;}
    return array;
}
static JSValue native_mutation_take(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    (void)this_val;(void)argc;(void)argv;struct web_js_state *s=state(ctx);
    struct js_mutation_record *r=s->mutation_first;s->mutation_first=s->mutation_last=NULL;
    JSValue array=JS_NewArray(ctx);uint32_t index=0;
    while(r){struct js_mutation_record *next=r->next;
        for(unsigned i=0;!JS_IsException(array)&&i<r->recipient_count;i++){
            JSValue row=JS_NewObject(ctx),fields=JS_NewObject(ctx);
            JS_SetPropertyStr(ctx,row,"observer",JS_DupValue(ctx,r->recipients[i].observer));
            JS_SetPropertyStr(ctx,fields,"type",JS_NewString(ctx,r->type==MO_CHILD?"childList":r->type==MO_ATTR?"attributes":"characterData"));
            JS_SetPropertyStr(ctx,fields,"target",wrap(s,r->target));
            JS_SetPropertyStr(ctx,fields,"previousSibling",wrap(s,r->previous));JS_SetPropertyStr(ctx,fields,"nextSibling",wrap(s,r->following));
            JS_SetPropertyStr(ctx,fields,"attributeName",str_or_null(ctx,r->name));JS_SetPropertyStr(ctx,fields,"attributeNamespace",str_or_null(ctx,r->namespace_uri));
            JS_SetPropertyStr(ctx,fields,"oldValue",r->recipients[i].old&&r->old_value?JS_NewStringLen(ctx,r->old_value,r->old_length==SIZE_MAX?strlen(r->old_value):r->old_length):JS_NULL);
            JS_SetPropertyStr(ctx,fields,"addedNodes",mutation_nodes(s,&r->added));JS_SetPropertyStr(ctx,fields,"removedNodes",mutation_nodes(s,&r->removed));
            JS_SetPropertyStr(ctx,row,"fields",fields);
            if(JS_SetPropertyUint32(ctx,array,index++,row)<0){JS_FreeValue(ctx,array);array=JS_EXCEPTION;}
        }
        mutation_record_free(s,r);r=next;
    }
    return array;
}
static void mutation_free(struct web_js_state *s) {
    while(s->mutation_registrations){struct js_mutation_registration *r=s->mutation_registrations;s->mutation_registrations=r->next;mutation_registration_free(s,r);}
    while(s->mutation_first){struct js_mutation_record *r=s->mutation_first;s->mutation_first=r->next;mutation_record_free(s,r);}
    s->mutation_last=NULL;
}
