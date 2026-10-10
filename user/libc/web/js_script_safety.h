#pragma once
#include "webi.h"
#include "quickjs.h"
#include <string.h>
#include <setjmp.h>

/* The HTMLScriptElement script-text slot. This is not the mutable DOM child
 * text, nor a writable wrapper marker. Adoption retains its allocation owner. */
static bool js_script_text_capture(node_t *node,const char *source,size_t length){
    if(!node||node->type!=N_ELEM||node->namespace_id!=NS_HTML||node->tag!=T_script)return false;
    if(!source&&length)return false;
    web_doc *allocation=node->allocation_doc?node->allocation_doc:node->owner;
    if(!allocation)return false;
    jmp_buf trap; jmp_buf *saved=allocation->mem.trap;allocation->mem.trap=&trap;
    if(setjmp(trap)){allocation->mem.trap=saved;return false;}
    const char *copy=ar_strndup(&allocation->mem,source?source:"",length);
    allocation->mem.trap=saved;
    if(!copy)return false;
    node->script_text_snapshot=copy;node->script_text_snapshot_len=length;node->script_text_snapshot_valid=true;return true;
}

/* Private host.safety('scriptText',node,string|null). Return the old slot so
 * compliant setter wrappers can restore it if the original setter throws.
 * Set the slot BEFORE the native setter: connected scripts can prepare there. */
static JSValue js_script_text_marker(JSContext *ctx,node_t *node,JSValueConst value){
    if(!node||node->type!=N_ELEM||node->namespace_id!=NS_HTML||node->tag!=T_script)
        return JS_ThrowTypeError(ctx,"HTMLScriptElement receiver required");
    if(!JS_IsNull(value)&&!JS_IsString(value))return JS_ThrowTypeError(ctx,"Admitted script text must be a string or null");
    JSValue old=node->script_text_snapshot_valid?JS_NewStringLen(ctx,node->script_text_snapshot,node->script_text_snapshot_len):JS_NULL;
    if(JS_IsException(old))return old;
    if(JS_IsNull(value)){node->script_text_snapshot=NULL;node->script_text_snapshot_len=0;node->script_text_snapshot_valid=false;return old;}
    size_t length;const char *source=JS_ToCStringLen(ctx,&length,value);
    if(!source){JS_FreeValue(ctx,old);return JS_EXCEPTION;}
    bool ok=js_script_text_capture(node,source,length);JS_FreeCString(ctx,source);
    if(!ok){JS_FreeValue(ctx,old);return JS_ThrowOutOfMemory(ctx);}return old;
}

typedef JSValue js_script_compliant_source(void *,node_t *,JSValueConst);

/* Called once during script preparation, after obtaining child text and
 * before freezing js_script->source. A transformed default-policy result is
 * the execution source; changing DOM text/observers/serialization is forbidden.
 * A queued script must keep this prepared source even if later DOM text changes. */
static JSValue js_script_source_check(JSContext *ctx,node_t *node,const char *source,size_t length,
                                     js_script_compliant_source *check,void *opaque){
    if(!node||node->type!=N_ELEM||node->namespace_id!=NS_HTML||node->tag!=T_script||(!source&&length))
        return JS_ThrowTypeError(ctx,"Invalid script source receiver or bytes");
    source=source?source:"";
    if(node->script_text_snapshot_valid&&node->script_text_snapshot_len==length&&
       (!length||!memcmp(node->script_text_snapshot,source,length)))return JS_NewStringLen(ctx,source,length);
    if(!check)return JS_ThrowTypeError(ctx,"Script source safety check unavailable");
    JSValue input=JS_NewStringLen(ctx,source,length);if(JS_IsException(input))return input;
    JSValue approved=check(opaque,node,input);JS_FreeValue(ctx,input);if(JS_IsException(approved))return approved;
    if(!JS_IsString(approved)){JS_FreeValue(ctx,approved);return JS_ThrowTypeError(ctx,"Script source safety check did not return a string");}
    size_t approved_length;const char *text=JS_ToCStringLen(ctx,&approved_length,approved);
    if(!text){JS_FreeValue(ctx,approved);return JS_EXCEPTION;}
    bool ok=js_script_text_capture(node,text,approved_length);JS_FreeCString(ctx,text);
    if(!ok){JS_FreeValue(ctx,approved);return JS_ThrowOutOfMemory(ctx);}return approved;
}
