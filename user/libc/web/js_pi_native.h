/* Included by js.c after native_dom_magic. ProcessingInstruction attributes
 * are a native ordered map, not Element attributes or author-visible fields.
 * Attribute-origin updates preserve the map even when its serialized names
 * are not XML Names; ordinary CharacterData writes invalidate it in dom.c. */
#include "html_pi.h"

enum pi_operation { PI_BRAND, PI_NAMES, PI_GET, PI_HAS, PI_ALL,
                    PI_SET, PI_REMOVE, PI_TOGGLE };

static bool pi_local_name_valid(const char *name,size_t length) {
    if(!length)return false;
    for(size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)name[i];
        if(!c||c=='\t'||c=='\n'||c=='\f'||c=='\r'||c==' '||c=='/'||c=='='||c=='>')return false;
    }
    return true;
}
static bool pi_buffer_put(sbuf *b,const char *text,size_t length) {
    if(b->n==SIZE_MAX||length>SIZE_MAX-b->n-1)return false;
    size_t needed=b->n+length+1;
    if(needed>b->cap) {
        size_t capacity=b->cap?b->cap:64;
        while(capacity<needed)capacity=capacity>SIZE_MAX/2?needed:capacity*2;
        char *bytes=realloc(b->p,capacity);if(!bytes)return false;
        b->p=bytes;b->cap=capacity;
    }
    if(length)memcpy(b->p+b->n,text,length);
    b->n+=length;b->p[b->n]=0;return true;
}
static bool pi_serialize_map(sbuf *out,const struct html_pi_attribute *attrs,size_t count) {
    for(size_t i=0;i<count;i++) {
        if((i&&!pi_buffer_put(out," ",1))||
           !pi_buffer_put(out,attrs[i].name,strlen(attrs[i].name))||
           !pi_buffer_put(out,"=\"",2))return false;
        for(size_t j=0;j<attrs[i].value_length;j++) {
            const char *escape=NULL;size_t length=0;
            switch((unsigned char)attrs[i].value[j]) {
            case '&':escape="&amp;";length=5;break;
            case '<':escape="&lt;";length=4;break;
            case '>':escape="&gt;";length=4;break;
            case '"':escape="&quot;";length=6;break;
            default:break;
            }
            if(!pi_buffer_put(out,escape?escape:attrs[i].value+j,escape?length:1))return false;
        }
        if(!pi_buffer_put(out,"\"",1))return false;
    }
    return true;
}
/* Cache strings live in the node's allocation document, which cannot change
 * on adoption. Do not attach heap pointers to a node or to wrapper properties. */
static struct html_pi_attribute *pi_arena_map(node_t *node,
        const struct html_pi_attribute *attrs,size_t count) {
    if(!count)return NULL;
    if(count>SIZE_MAX/sizeof *attrs)return NULL;
    web_doc *d=node->allocation_doc?node->allocation_doc:node->owner;
    if(!d)return NULL;
    doc_dom_budget(d);
    jmp_buf trap; jmp_buf *previous=d->mem.trap;
    d->mem.trap=&trap;
    if(setjmp(trap)){d->mem.trap=previous;return NULL;}
    struct html_pi_attribute *map=ar_alloc(&d->mem,count*sizeof *map);
    for(size_t i=0;i<count;i++) {
        map[i].name=ar_strdup(&d->mem,attrs[i].name);
        map[i].value=ar_strndup(&d->mem,attrs[i].value,attrs[i].value_length);
        map[i].value_length=attrs[i].value_length;
    }
    d->mem.trap=previous;return map;
}
static bool pi_attributes_ensure(node_t *node) {
    if(node->pi_attributes_ready)return true;
    struct html_pi_attribute *parsed=NULL;size_t count=0;
    /* The grammar invalidates the complete map, including a valid prefix. */
    enum html_pi_parse_status status=html_pi_parse_ex(node->text,node->textlen,&parsed,&count);
    if(status==HTML_PI_OOM)return false;
    struct html_pi_attribute *map=pi_arena_map(node,parsed,count);
    html_pi_free(parsed,count);
    if(count&&!map)return false;
    node->pi_attributes=map;node->pi_attribute_count=count;node->pi_attributes_ready=true;
    return true;
}
static JSValue native_pi(JSContext *ctx,JSValueConst this_value,int argc,JSValueConst *argv) {
    (void)this_value;
    if(argc<2)return JS_ThrowTypeError(ctx,"ProcessingInstruction receiver required");
    int32_t op;if(JS_ToInt32(ctx,&op,argv[0])<0)return JS_EXCEPTION;
    node_t *node=unwrap(ctx,argv[1]);
    if(!node){if(JS_IsNull(argv[1])||JS_IsUndefined(argv[1]))return JS_ThrowTypeError(ctx,"ProcessingInstruction receiver required");return JS_EXCEPTION;}
    if(node->type!=N_PI)return JS_ThrowTypeError(ctx,"ProcessingInstruction receiver required");
    struct web_js_state *s=state(ctx);web_doc *d=node->owner?node->owner:s->doc;
    if(!s->starting&&((sandbox_authority(s)&SB_SCRIPTS)||
        (sandbox_actor(s)->doc->sandbox_flags&&sandbox_actor(s)->doc!=d)))
        return history_security_error(ctx,"Sandbox denied borrowed document authority");
    if(op==PI_BRAND)return JS_UNDEFINED;
    if(op<PI_NAMES||op>PI_TOGGLE)return JS_ThrowTypeError(ctx,"Unknown processing instruction operation");
    if(!pi_attributes_ensure(node))return oom(ctx);
    size_t count=node->pi_attribute_count;
    struct html_pi_attribute *attrs=node->pi_attributes;
    if(op==PI_NAMES) {
        JSValue names=JS_NewArray(ctx);if(JS_IsException(names))return names;
        if(count>UINT32_MAX){JS_FreeValue(ctx,names);return oom(ctx);}
        for(size_t i=0;i<count;i++)if(JS_DefinePropertyValueUint32(ctx,names,(uint32_t)i,JS_NewString(ctx,attrs[i].name),JS_PROP_C_W_E)<0){JS_FreeValue(ctx,names);return JS_EXCEPTION;}
        return names;
    }
    if(op==PI_ALL)return JS_NewBool(ctx,count!=0);
    if(argc<3)return JS_ThrowTypeError(ctx,"ProcessingInstruction attribute name required");
    size_t name_length=0;const char *name=JS_ToCStringLen(ctx,&name_length,argv[2]);
    if(!name)return JS_EXCEPTION;
    size_t index=count;
    for(size_t i=0;i<count;i++)if(strlen(attrs[i].name)==name_length&&!memcmp(attrs[i].name,name,name_length)){index=i;break;}
    if(op==PI_GET||op==PI_HAS) {
        JSValue result=op==PI_HAS?JS_NewBool(ctx,index<count):index<count?
            JS_NewStringLen(ctx,attrs[index].value,attrs[index].value_length):JS_NULL;
        JS_FreeCString(ctx,name);return result;
    }
    if((op==PI_SET||op==PI_TOGGLE)&&!pi_local_name_valid(name,name_length)) {
        JS_FreeCString(ctx,name);return JS_ThrowTypeError(ctx,"Invalid processing instruction attribute name");
    }
    bool add=op==PI_SET,remove=op==PI_REMOVE,toggle_value=false;
    if(op==PI_TOGGLE) {
        bool given=argc>3&&!JS_IsUndefined(argv[3]);int force=given?JS_ToBool(ctx,argv[3]):0;
        if(force<0){JS_FreeCString(ctx,name);return JS_EXCEPTION;}
        if(index==count){add=!given||force;toggle_value=add;}
        else {remove=!given||!force;toggle_value=!remove;}
        if(!add&&!remove){JS_FreeCString(ctx,name);return JS_NewBool(ctx,toggle_value);}
    }
    const char *value="";size_t value_length=0;
    if(op==PI_SET) {
        if(argc<4){JS_FreeCString(ctx,name);return JS_ThrowTypeError(ctx,"ProcessingInstruction attribute value required");}
        value=JS_ToCStringLen(ctx,&value_length,argv[3]);
        if(!value){JS_FreeCString(ctx,name);return JS_EXCEPTION;}
    }
    size_t next_count=count+(add&&index==count?1:0)-(remove&&index<count?1:0);
    if(next_count>SIZE_MAX/sizeof *attrs) {
        JS_FreeCString(ctx,name);if(op==PI_SET)JS_FreeCString(ctx,value);return oom(ctx);
    }
    struct html_pi_attribute *next=next_count?malloc(next_count*sizeof *next):NULL;
    if(next_count&&!next){JS_FreeCString(ctx,name);if(op==PI_SET)JS_FreeCString(ctx,value);return oom(ctx);}
    size_t at=0;
    for(size_t i=0;i<count;i++) {
        if(remove&&i==index)continue;
        next[at++]=add&&i==index?(struct html_pi_attribute){(char *)name,(char *)value,value_length}:attrs[i];
    }
    if(add&&index==count)next[at++]=(struct html_pi_attribute){(char *)name,(char *)value,value_length};
    struct html_pi_attribute *stored=pi_arena_map(node,next,next_count);
    sbuf data={0};bool prepared=(!next_count||stored)&&pi_serialize_map(&data,next,next_count);
    free(next);JS_FreeCString(ctx,name);if(op==PI_SET)JS_FreeCString(ctx,value);
    if(!prepared){sb_free(&data);return oom(ctx);}
    JSValue text=JS_NewStringLen(ctx,data.p?data.p:"",data.n),key=JS_NewString(ctx,"nodeValue");
    if(JS_IsException(text)||JS_IsException(key)){JS_FreeValue(ctx,text);JS_FreeValue(ctx,key);sb_free(&data);return JS_EXCEPTION;}
    /* This goes through native ownership, sandbox, DOM revision, native
       MutationObserver, and JS live-Range hooks exactly as a data setter. */
    JSValue args[]={argv[1],key,text};
    JSValue result=native_dom_magic(ctx,JS_UNDEFINED,3,args,DOM_set);
    JS_FreeValue(ctx,key);JS_FreeValue(ctx,text);sb_free(&data);
    if(!JS_IsException(result)||!node->pi_attributes_ready) {
        node->pi_attributes=stored;node->pi_attribute_count=next_count;node->pi_attributes_ready=true;
    }
    if(op==PI_TOGGLE&&!JS_IsException(result)){JS_FreeValue(ctx,result);return JS_NewBool(ctx,toggle_value);}
    return result;
}
