/* Private native CSSOM receiver bridge. Author conversions precede every
   current-owner/generation check; no supplied origin controls sheet access. */
#include "cssom.h"
static JSValue cssom_dom(struct web_js_state *s,node_t *n,int argc,JSValueConst *argv) {
    JSContext *ctx=s->ctx;
    if (!n || argc<3)return JS_ThrowTypeError(ctx,"Native style sheet receiver required");
    web_doc *original=n->owner; node_t *root=original ? original->root : NULL;
    const char *operation=JS_ToCString(ctx,argv[2]),*text=NULL;
    uint32_t id=0,index=0;size_t length=0;JSValue result=JS_EXCEPTION;
    if (!operation) return JS_EXCEPTION;
    bool adoption = !strcmp(operation,"adopt");
    if (!adoption && argc>3 && JS_ToUint32(ctx,&id,argv[3])<0) goto out;
    if (argc>4 && JS_ToUint32(ctx,&index,argv[4])<0) goto out;
    if (!strcmp(operation,"insert") || !strcmp(operation,"replace")) {
        if (argc<6) { result=JS_ThrowTypeError(ctx,"insertRule requires rule and index");goto out; }
        text=JS_ToCStringLen(ctx,&length,argv[5]);if(!text)goto out;
    }
    web_doc *d=n->owner;
    if (!d || d!=original || d->root!=root || !d->live || d->inert || !web_js_enabled(d) ||
        !d->js || JS_GetRuntime(d->js->ctx)!=JS_GetRuntime(ctx) ||
        !s->doc->live || s->doc->js!=s || s->disabled) { result=JS_NewInt32(ctx,-CSSOM_INACTIVE);goto out; }
    if (!strcmp(operation,"construct")) {
        if(n!=d->root){result=JS_ThrowTypeError(ctx,"Constructed sheet document required");goto out;}
        int error;struct cssom_sheet *sheet=cssom_construct(d,&error);
        result=error==CSSOM_OOM?oom(ctx):JS_NewInt64(ctx,sheet?sheet->id:0);goto out;
    }
    if (adoption || !strcmp(operation,"adopted")) {
        if(!((n->type==N_DOC&&n==d->root)||(n->type==N_FRAGMENT&&n->shadow_host))) {
            result=JS_ThrowTypeError(ctx,"Document or ShadowRoot adoptedStyleSheets receiver required");goto out;
        }
        if(!adoption) {
            result=JS_NewArray(ctx);
            for(uint32_t i=0;!JS_IsException(result)&&i<n->adopted_count;i++) {
                JSValue pair=JS_NewArray(ctx);
                struct cssom_sheet *sheet=n->adopted_sheets[i];
                if(JS_IsException(pair)||JS_SetPropertyUint32(ctx,pair,0,wrap(s,sheet->owner))<0||
                    JS_SetPropertyUint32(ctx,pair,1,JS_NewInt64(ctx,sheet->id))<0||JS_SetPropertyUint32(ctx,result,i,pair)<0) {
                    JS_FreeValue(ctx,result);result=JS_EXCEPTION;break;
                }
            }
            goto out;
        }
        uint32_t count;JSValue len=argc>3?JS_GetPropertyStr(ctx,argv[3],"length"):JS_UNDEFINED;
        int converted=JS_ToUint32(ctx,&count,len);JS_FreeValue(ctx,len);if(converted<0)goto out;
        if((size_t)count>SIZE_MAX/sizeof(struct cssom_sheet*)){result=oom(ctx);goto out;}
        struct cssom_sheet **items=count?calloc(count,sizeof *items):NULL;
        if(count&&!items){result=oom(ctx);goto out;}
        int error=CSSOM_OK;
        for(uint32_t i=0;i<count;i++) {
            JSValue pair=JS_GetPropertyUint32(ctx,argv[3],i),a=JS_GetPropertyUint32(ctx,pair,0),b=JS_GetPropertyUint32(ctx,pair,1);
            node_t *owner=unwrap(ctx,a);uint32_t serial;
            if(!owner||JS_ToUint32(ctx,&serial,b)<0)error=CSSOM_NOT_ALLOWED;
            else items[i]=cssom_style_find(owner,serial);
            JS_FreeValue(ctx,a);JS_FreeValue(ctx,b);JS_FreeValue(ctx,pair);
            if(error)break;
        }
        if(!error)error=cssom_adopt(n,items,count);free(items);
        result=error==CSSOM_OOM?oom(ctx):JS_NewInt32(ctx,-error);goto out;
    }
    if(!strcmp(operation,"listLength")||!strcmp(operation,"listItem")) {
        if(!((n->type==N_DOC&&n==d->root)||(n->type==N_FRAGMENT&&n->shadow_host))) {
            result=JS_ThrowTypeError(ctx,"Document or ShadowRoot styleSheets receiver required");goto out;
        }
        if(d->resources_dirty)doc_sync_tree(d);
        uint32_t count;int error;bool select=!strcmp(operation,"listItem");
        node_t *item=cssom_list_owner(d,n,index,select,&count,&error);
        result=error==CSSOM_OOM?oom(ctx):select?wrap(s,item):JS_NewInt64(ctx,count);goto out;
    }
    if(n!=d->root && (n->type!=N_ELEM||n->foreign||(n->tag!=T_style&&n->tag!=T_link))) {
        result=JS_ThrowTypeError(ctx,"Native style or link sheet owner required");goto out;
    }
    if (!strcmp(operation,"sheet")) {
        if(d->resources_dirty)doc_sync_tree(d);
        int error;struct cssom_sheet *sheet=cssom_style_sheet(d,n,&error);
        result=error==CSSOM_OOM ? oom(ctx) : JS_NewInt64(ctx,sheet ? sheet->id : 0);goto out;
    }
    struct cssom_sheet *sheet=cssom_style_find(n,id);
    /* A disassociated sheet can outlive adoption out of its former document.
       Do not dereference that old owner pointer; the current node/runtime was
       revalidated above and detached sheet mutation never changes a cascade. */
    if (!sheet) { result=JS_NewInt32(ctx,-CSSOM_INACTIVE);goto out; }
    if(!strcmp(operation,"href")){result=sheet->href?JS_NewString(ctx,sheet->href):JS_NULL;goto out;}
    if (!strcmp(operation,"current")) {
        if(sheet->constructed){result=JS_FALSE;goto out;}
        int error;struct cssom_sheet *current=cssom_style_sheet(d,n,&error);
        result=error==CSSOM_OOM ? oom(ctx) : JS_NewBool(ctx,current==sheet&&sheet->associated);goto out;
    }
    if ((!strcmp(operation,"length")||!strcmp(operation,"rule")||!strcmp(operation,"ruleId")||
         !strcmp(operation,"insert")||!strcmp(operation,"delete"))&&!sheet->origin_clean) {
        result=JS_NewInt32(ctx,-CSSOM_SECURITY);goto out;
    }
    if (!strcmp(operation,"length")) { uint32_t count;css_sheet_rules(sheet->ast,&count);result=JS_NewInt64(ctx,count);goto out; }
    if (!strcmp(operation,"disabled") || !strcmp(operation,"disable")) {
        bool current=sheet->associated&&n->cssom_current==sheet;
        if(current)sheet->disabled=cssom_owner_disabled(n);
        if(!strcmp(operation,"disable")) {
            sheet->disabled=index!=0;
            if(sheet->constructed)cssom_changed(sheet);
            if(current){n->style_disabled=sheet->disabled;n->style_disabled_set=true;d->resources_dirty=d->dirty=d->need_style=true;}
        }
        result=JS_NewBool(ctx,sheet->disabled);goto out;
    }
    if (!strcmp(operation,"media")) {
        if(argc>5 && !JS_IsUndefined(argv[5])) {
            const char *value=JS_ToCString(ctx,argv[5]);if(!value)goto out;
            char *copy=strdup(value);JS_FreeCString(ctx,value);
            if(!copy){result=oom(ctx);goto out;}
            free(sheet->media);sheet->media=copy;cssom_changed(sheet);
        }
        result=JS_NewString(ctx,sheet->media?sheet->media:"");goto out;
    }
    if (!strcmp(operation,"replace")) {
        int error=cssom_replace_sync(sheet,text,length);
        result=error==CSSOM_OOM?oom(ctx):JS_NewInt32(ctx,-error);goto out;
    }
    if (!strcmp(operation,"insert") || !strcmp(operation,"delete")) {
        int error=!strcmp(operation,"insert") ? cssom_insert(sheet,text,length,index) : cssom_delete(sheet,index);
        result=error==CSSOM_OOM ? oom(ctx) : JS_NewInt64(ctx,error ? -(int64_t)error : index);goto out;
    }
    if (!strcmp(operation,"rule") || !strcmp(operation,"ruleId")) {
        struct css_rule_info *r=css_sheet_rules(sheet->ast,NULL);
        if (!strcmp(operation,"rule")) { for(uint32_t i=0;r&&i<index;i++)r=r->next; }
        else while(r&&r->id!=index)r=r->next;
        bool attached=r!=NULL;
        if(!r&&!strcmp(operation,"ruleId")){r=sheet->removed;while(r&&r->id!=index)r=r->next;}
        if (!r) { result=JS_NULL;goto out; }
        result=JS_NewObjectProto(ctx,JS_NULL);if(JS_IsException(result))goto out;
        if (JS_DefinePropertyValueStr(ctx,result,"id",JS_NewInt64(ctx,r->id),JS_PROP_C_W_E)<0 ||
            JS_DefinePropertyValueStr(ctx,result,"type",JS_NewInt64(ctx,r->type),JS_PROP_C_W_E)<0 ||
            JS_DefinePropertyValueStr(ctx,result,"attached",JS_NewBool(ctx,attached),JS_PROP_C_W_E)<0 ||
            JS_DefinePropertyValueStr(ctx,result,"text",JS_NewString(ctx,r->text),JS_PROP_C_W_E)<0 ||
            JS_DefinePropertyValueStr(ctx,result,"selector",JS_NewString(ctx,r->selector ? r->selector : ""),JS_PROP_C_W_E)<0) {
            JS_FreeValue(ctx,result);result=JS_EXCEPTION;
        }
    } else result=JS_ThrowTypeError(ctx,"Unknown native CSSOM operation");
out:
    JS_FreeCString(ctx,text);JS_FreeCString(ctx,operation);return result;
}
