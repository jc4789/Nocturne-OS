/* Private native CSSOM receiver bridge. Author conversions precede every
   current-owner/generation check; no supplied origin controls sheet access.
   Sheets are (owner node, sheet id); rules are (sheet, rule id), the ids
   cssom.c keeps across every change, 0 for the sheet itself as a container.
   js_stylesheets.js wraps these: every value it shows is read here. */
#include "cssom.h"
static JSValue cssom_string(JSContext *ctx, const sbuf *b) { return JS_NewStringLen(ctx, b->p ? b->p : "", b->n); }
static JSValue cssom_error(JSContext *ctx, int error) { return error == CSSOM_OOM ? oom(ctx) : JS_NewInt32(ctx, -error); }
/* a rule's declaration block (CSSOM CSSStyleDeclaration over the rule's
   declarations, its validity in the rule's block) */
static JSValue cssom_declaration_op(struct web_js_state *s, struct cssom_sheet *sheet, struct css_rule_info *r,
                                    uint32_t id, const char *operation, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx;
    if (!strcmp(operation, "declText")) {
        size_t length;
        const char *value = argc > 5 ? JS_ToCStringLen(ctx, &length, argv[5]) : NULL;
        if (!value) return argc > 5 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "cssText required");
        int error = cssom_set_declarations(sheet, id, value, length);
        JS_FreeCString(ctx, value);
        return cssom_error(ctx, error);
    }
    /* the rule's declarations as its block serializes them */
    const char *text; size_t n;
    cssom_declarations(r, &text, &n);
    css_decl_check *check = cssom_declaration_check(r);
    sbuf block = {0};
    if (!style_serialize(text, n, check, &block, NULL)) { sb_free(&block); return oom(ctx); }
    const char *serialized = block.p ? block.p : "";
    if (!strcmp(operation, "decls")) { JSValue list = style_text_block(ctx, serialized, block.n); sb_free(&block); return list; }
    const char *property = argc > 5 ? JS_ToCString(ctx, argv[5]) : NULL;
    if (!property) { sb_free(&block); return argc > 5 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Property name required"); }
    JSValue result;
    bool write;
    if (!strcmp(operation, "declGet")) {
        bool priority = argc > 6 && JS_ToBool(ctx, argv[6]) > 0;
        result = style_text_access(ctx, serialized, block.n, property, false, JS_UNDEFINED, JS_UNDEFINED, priority, check,
                                   NULL, &write);
    } else {
        sbuf b = {0};
        result = style_text_access(ctx, serialized, block.n, property, true, argc > 6 ? argv[6] : JS_UNDEFINED,
                                   argc > 7 ? argv[7] : JS_UNDEFINED, false, check, &b, &write);
        if (write && !JS_IsException(result)) {
            int error = cssom_set_declarations(sheet, id, b.p ? b.p : "", b.n);
            if (error == CSSOM_OOM) result = oom(ctx);
        }
        sb_free(&b);
    }
    JS_FreeCString(ctx, property);
    sb_free(&block);
    return result;
}
static JSValue cssom_rule_op(struct web_js_state *s, struct cssom_sheet *sheet, const char *operation, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx;
    uint32_t id = 0;
    if (argc > 4 && JS_ToUint32(ctx, &id, argv[4]) < 0) return JS_EXCEPTION;
    if (!strcmp(operation, "length") || !strcmp(operation, "child")) {
        uint32_t index = 0, count;
        if (argc > 5 && JS_ToUint32(ctx, &index, argv[5]) < 0) return JS_EXCEPTION;
        if (id && !cssom_rule(sheet, id, NULL)) return operation[0] == 'l' ? JS_NewInt32(ctx, 0) : JS_NULL;
        struct css_rule_info *child = cssom_child(sheet, id, operation[0] == 'l' ? UINT32_MAX : index, &count);
        if (operation[0] == 'l') return JS_NewInt64(ctx, count);
        if (!child) return JS_NULL;
        JSValue pair = JS_NewArray(ctx);
        if (JS_IsException(pair) || JS_SetPropertyUint32(ctx, pair, 0, JS_NewInt64(ctx, child->id)) < 0 ||
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewInt64(ctx, child->type)) < 0) { JS_FreeValue(ctx, pair); return JS_EXCEPTION; }
        return pair;
    }
    if (!strcmp(operation, "insert") || !strcmp(operation, "delete")) {
        bool insertion = operation[0] == 'i';
        uint32_t index = 0;
        size_t length = 0;
        const char *text = NULL;
        if (insertion) {
            if (argc < 7) return JS_ThrowTypeError(ctx, "insertRule requires rule and index");
            text = JS_ToCStringLen(ctx, &length, argv[5]);
            if (!text) return JS_EXCEPTION;
            if (JS_ToUint32(ctx, &index, argv[6]) < 0) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
        } else if (argc < 6 || JS_ToUint32(ctx, &index, argv[5]) < 0) return argc < 6 ? JS_ThrowTypeError(ctx, "deleteRule requires index") : JS_EXCEPTION;
        int error = insertion ? cssom_insert(sheet, id, text, length, index) : cssom_delete(sheet, id, index);
        JS_FreeCString(ctx, text);
        return error == CSSOM_OOM ? oom(ctx) : JS_NewInt64(ctx, error ? -(int64_t)error : index);
    }
    if (!strcmp(operation, "media")) {
        uint32_t op = 0;
        size_t length = 0;
        const char *text = NULL;
        if (argc > 5 && JS_ToUint32(ctx, &op, argv[5]) < 0) return JS_EXCEPTION;
        if (op) {
            text = argc > 6 ? JS_ToCStringLen(ctx, &length, argv[6]) : NULL;
            if (!text) return argc > 6 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Media text required");
        }
        sbuf b = {0};
        int error = cssom_media(sheet, id, (int)op, text, length, &b);
        JS_FreeCString(ctx, text);
        JSValue result = error ? cssom_error(ctx, error) : op ? JS_NewInt32(ctx, 0) : cssom_string(ctx, &b);
        sb_free(&b);
        return result;
    }
    bool attached;
    struct css_rule_info *r = cssom_rule(sheet, id, &attached);
    if (!strcmp(operation, "rule")) {
        if (!r) return JS_NULL;
        struct css_rule_info *parent = cssom_parent_rule(sheet, r);
        JSValue info = JS_NewArray(ctx);
        if (JS_IsException(info) || JS_SetPropertyUint32(ctx, info, 0, JS_NewInt64(ctx, r->type)) < 0 ||
            JS_SetPropertyUint32(ctx, info, 1, JS_NewBool(ctx, attached)) < 0 ||
            JS_SetPropertyUint32(ctx, info, 2, JS_NewInt64(ctx, parent ? parent->id : 0)) < 0 ||
            JS_SetPropertyUint32(ctx, info, 3, JS_NewInt64(ctx, parent ? parent->type : 0)) < 0) { JS_FreeValue(ctx, info); return JS_EXCEPTION; }
        return info;
    }
    if (!r) {
        /* a rule that stands nowhere now: it reads as empty and changes nothing */
        if (!strcmp(operation, "decls")) return style_text_block(ctx, "", 0);
        return !strcmp(operation, "cssText") || !strcmp(operation, "declGet") ? JS_NewString(ctx, "") :
            !strcmp(operation, "keyframe") || !strcmp(operation, "attr") ? JS_NULL :
            !strcmp(operation, "features") ? JS_NewArray(ctx) : JS_NewInt32(ctx, 0);
    }
    if (!strcmp(operation, "cssText")) {
        sbuf b = {0};
        cssom_serialize(sheet, r, &b);
        JSValue result = cssom_string(ctx, &b);
        sb_free(&b);
        return result;
    }
    if (!strncmp(operation, "decl", 4)) return cssom_declaration_op(s, sheet, r, id, operation, argc, argv);
    const char *name = argc > 5 ? JS_ToCString(ctx, argv[5]) : NULL;
    if (!name) return argc > 5 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Native CSSOM operand required");
    JSValue result;
    if (!strcmp(operation, "attr")) {
        sbuf b = {0};
        if (!cssom_attribute(sheet, r, name, &b)) result = JS_NULL;
        else if (!strcmp(name, "nameList")) {
            result = JS_NewArray(ctx);
            uint32_t i = 0;
            for (size_t at = 0; !JS_IsException(result) && at < b.n; at += strlen(b.p + at) + 1)
                if (JS_SetPropertyUint32(ctx, result, i++, JS_NewString(ctx, b.p + at)) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; }
        } else result = cssom_string(ctx, &b);
        sb_free(&b);
    } else if (!strcmp(operation, "setAttr")) {
        size_t length;
        const char *value = argc > 6 ? JS_ToCStringLen(ctx, &length, argv[6]) : NULL;
        if (!value) result = argc > 6 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Attribute value required");
        else { result = cssom_error(ctx, cssom_set_attribute(sheet, id, name, value, length)); JS_FreeCString(ctx, value); }
    } else if (!strcmp(operation, "keyframe")) {
        int64_t found = cssom_keyframe_find(sheet, id, name, strlen(name));
        result = found < 0 ? JS_NULL : JS_NewInt64(ctx, found);
    } else if (!strcmp(operation, "features")) {
        sbuf b = {0};
        result = JS_NewArray(ctx);
        if (!JS_IsException(result) && cssom_feature_values(sheet, r, name, &b)) {
            uint32_t i = 0;
            for (size_t at = 0; at < b.n;) {
                const char *key = b.p + at, *values = key + strlen(key) + 1;
                JSValue list = JS_NewArray(ctx);
                uint32_t j = 0;
                for (const char *v = values; *v;) {
                    char *stop;
                    unsigned long number = strtoul(v, &stop, 10);
                    if (stop == v) break;
                    JS_SetPropertyUint32(ctx, list, j++, JS_NewInt64(ctx, (int64_t)number));
                    v = *stop ? stop + 1 : stop;
                }
                if (JS_SetPropertyUint32(ctx, result, i++, JS_NewString(ctx, key)) < 0 ||
                    JS_SetPropertyUint32(ctx, result, i++, list) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; break; }
                at = (size_t)(values - b.p) + strlen(values) + 1;
            }
        }
        sb_free(&b);
    } else if (!strcmp(operation, "setFeature")) {
        /* argv[6]: the name (null: clear), argv[7]: its values (null: delete) */
        const char *key = argc > 6 && !JS_IsNull(argv[6]) ? JS_ToCString(ctx, argv[6]) : NULL;
        uint32_t count = 0, *values = NULL;
        bool listed = argc > 7 && !JS_IsNull(argv[7]) && !JS_IsUndefined(argv[7]);
        int converted = (argc > 6 && !JS_IsNull(argv[6]) && !key) ? -1 : 0;
        if (!converted && listed) {
            JSValue length = JS_GetPropertyStr(ctx, argv[7], "length");
            converted = JS_ToUint32(ctx, &count, length);
            JS_FreeValue(ctx, length);
            values = !converted && count ? calloc(count, sizeof *values) : NULL;
            if (!converted && count && !values) converted = -2;
            for (uint32_t i = 0; !converted && i < count; i++) {
                JSValue v = JS_GetPropertyUint32(ctx, argv[7], i);
                converted = JS_ToUint32(ctx, &values[i], v);
                JS_FreeValue(ctx, v);
            }
        }
        if (converted == -2) result = oom(ctx);
        else if (converted) result = JS_EXCEPTION;
        else {
            static const uint32_t none[1];
            result = cssom_error(ctx, cssom_set_feature_values(sheet, id, name, key, listed ? (values ? values : none) : NULL, count));
        }
        free(values);
        JS_FreeCString(ctx, key);
    } else result = JS_ThrowTypeError(ctx, "Unknown native CSSOM operation");
    JS_FreeCString(ctx, name);
    return result;
}
static JSValue cssom_dom(struct web_js_state *s,node_t *n,int argc,JSValueConst *argv) {
    JSContext *ctx=s->ctx;
    if (!n || argc<3)return JS_ThrowTypeError(ctx,"Native style sheet receiver required");
    web_doc *original=n->owner; node_t *root=original ? original->root : NULL;
    const char *operation=JS_ToCString(ctx,argv[2]),*text=NULL;
    uint32_t id=0,index=0;size_t length=0;JSValue result=JS_EXCEPTION;
    if (!operation) return JS_EXCEPTION;
    bool adoption = !strcmp(operation,"adopt");
    if (!adoption && argc>3 && JS_ToUint32(ctx,&id,argv[3])<0) goto out;
    bool list=!strcmp(operation,"listLength")||!strcmp(operation,"listItem");
    bool sheet_op=!strcmp(operation,"disable")||list;
    if (sheet_op && argc>4 && JS_ToUint32(ctx,&index,argv[4])<0) goto out;
    if (!strcmp(operation,"replace")) {
        if (argc<6) { result=JS_ThrowTypeError(ctx,"replace requires text");goto out; }
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
    if(list) {
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
    if (!strcmp(operation,"replace")) {
        result=cssom_error(ctx,cssom_replace_sync(sheet,text,length));goto out;
    }
    /* The sheet's own MediaList stays readable across origins; its rules do
       not (CSSOM: cssRules, insertRule and deleteRule throw SecurityError),
       nor anything a CSSRule of it reads or changes. */
    uint32_t rule=0;
    if (!strcmp(operation,"media") && argc>4 && JS_ToUint32(ctx,&rule,argv[4])<0) goto out;
    if ((strcmp(operation,"media") || rule) && !sheet->origin_clean) { result=JS_NewInt32(ctx,-CSSOM_SECURITY);goto out; }
    result=cssom_rule_op(s,sheet,operation,argc,argv);
out:
    JS_FreeCString(ctx,text);JS_FreeCString(ctx,operation);return result;
}
