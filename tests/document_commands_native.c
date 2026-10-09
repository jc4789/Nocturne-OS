/* Actual command helper + actual JS adapter; native form storage/DOM shims. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include "quickjs.h"
enum {N_DOC=9,N_ELEM=1,T_input=1,T_textarea=2};
enum web_input_kind {WEB_INPUT_TEXT,WEB_INPUT_SEARCH,WEB_INPUT_TEL,WEB_INPUT_URL,WEB_INPUT_EMAIL,WEB_INPUT_PASSWORD};
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner;int type,tag,kind;bool disabled,readonly;char *value;const char *maxlength;uint32_t selection_start,selection_end;JSValue wrapper;} node_t;
struct web_js_state {JSContext *ctx;bool disabled;JSValue hooks;};
struct web_doc {struct web_js_state *js;node_t *root,*focus;bool live,inert;};
static JSClassID node_class;static node_t control;static web_doc document;
static node_t *unwrap(JSContext *ctx,JSValueConst value){return JS_GetOpaque2(ctx,value,node_class);}
static JSValue wrap(struct web_js_state *s,node_t *n){return n?JS_DupValue(s->ctx,n->wrapper):JS_NULL;}
static bool doc_node_connected(node_t *n){return n&&n->owner==&document;}
static bool doc_control_selection_supported(node_t *n){return n&&(n->tag==T_textarea||n->kind<=WEB_INPUT_URL||n->kind==WEB_INPUT_PASSWORD);}
static bool web_control_disabled(node_t *n){return n->disabled;}
static bool web_dialog_inert(web_doc *d,node_t *n){return d->inert;}
static const char *node_attr(node_t *n,const char *name){return !strcmp(name,"readonly")?(n->readonly?"":NULL):!strcmp(name,"maxlength")?n->maxlength:NULL;}
static enum web_input_kind web_input_type(node_t *n){return n->kind;}
static const char *web_input_edit_text(node_t *n){return n->value?n->value:"";}
static uint32_t doc_utf16_length(const char *text){uint32_t units=0;for(const unsigned char *p=(const unsigned char *)text;*p;p++){if((*p&0xc0)!=0x80)units+=*p>=0xf0?2:1;}return units;}
static void doc_control_init(web_doc *d,node_t *n){}
static void doc_control_selection(web_doc *d,node_t *n,uint32_t a,uint32_t b,int direction){uint32_t len=doc_utf16_length(web_input_edit_text(n));n->selection_start=a<len?a:len;n->selection_end=b<len?b:len;}
static bool web_input_user_value(web_doc *d,node_t *n,const char *text,size_t len){char *p=malloc(len+1);if(!p)return false;memcpy(p,text,len);p[len]=0;free(n->value);n->value=p;return true;}
static JSValue oom(JSContext *ctx){return JS_ThrowOutOfMemory(ctx);}
static JSValue custom_element_hook(struct web_js_state *s,const char *name,int argc,JSValueConst *argv){JSValue f=JS_GetPropertyStr(s->ctx,s->hooks,name),r=JS_Call(s->ctx,f,s->hooks,argc,argv);JS_FreeValue(s->ctx,f);return r;}
#include "user/libc/web/js_document_commands.h"
static JSValue native_dom(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return document_command_dom(document.js,argc>1?unwrap(ctx,argv[1]):NULL,argc,argv);}
static JSValue brand(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){node_t *n=argc?unwrap(ctx,argv[0]):NULL;if(!n||n->type!=N_DOC)return JS_ThrowTypeError(ctx,"Document required");return JS_DupValue(ctx,argv[0]);}
static JSValue flag(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    const char *key=JS_ToCString(ctx,argv[0]);if(!key)return JS_EXCEPTION;int v=argc>1?JS_ToBool(ctx,argv[1]):0;
    if(!strcmp(key,"readonly"))control.readonly=v;else if(!strcmp(key,"disabled"))control.disabled=v;
    else if(!strcmp(key,"live"))document.live=v;else if(!strcmp(key,"inert"))document.inert=v;
    else if(!strcmp(key,"focus"))document.focus=v?&control:NULL;else if(!strcmp(key,"textarea"))control.tag=v?T_textarea:T_input;
    else if(!strcmp(key,"maxlength"))control.maxlength=v?"4":NULL;
    else if(!strcmp(key,"url"))control.kind=v?WEB_INPUT_URL:WEB_INPUT_TEXT;
    else if(!strcmp(key,"value")){const char *text=JS_ToCString(ctx,argv[1]);if(text){web_input_user_value(&document,&control,text,strlen(text));control.selection_start=control.selection_end=doc_utf16_length(text);JS_FreeCString(ctx,text);}}
    else if(!strcmp(key,"range")){uint32_t a,b;JS_ToUint32(ctx,&a,argv[1]);JS_ToUint32(ctx,&b,argv[2]);doc_control_selection(&document,&control,a,b,0);}
    JS_FreeCString(ctx,key);return JS_UNDEFINED;
}
static JSValue value_get(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_NewString(ctx,control.value);}
static JSValue position_get(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){return JS_NewUint32(ctx,control.selection_end);}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);JSRuntime *rt=JS_NewRuntime();JS_SetMemoryLimit(rt,128u<<20);JS_SetMaxStackSize(rt,1u<<20);
    struct web_js_state s={.ctx=JS_NewContext(rt)};node_t root={.owner=&document,.type=N_DOC};
    document=(web_doc){.js=&s,.root=&root,.focus=&control,.live=true};control=(node_t){.owner=&document,.type=N_ELEM,.tag=T_input,.value=strdup("ab\xf0\x9f\x98\x80" "cd"),.selection_start=2,.selection_end=4};
    JS_NewClassID(&node_class);JSClassDef def={.class_name="CommandFixtureNode"};JS_NewClass(rt,node_class,&def);
    root.wrapper=JS_NewObjectClass(s.ctx,node_class);control.wrapper=JS_NewObjectClass(s.ctx,node_class);JS_SetOpaque(root.wrapper,&root);JS_SetOpaque(control.wrapper,&control);
    JSValue g=JS_GetGlobalObject(s.ctx);
    JS_SetPropertyStr(s.ctx,g,"document",JS_DupValue(s.ctx,root.wrapper));JS_SetPropertyStr(s.ctx,g,"control",JS_DupValue(s.ctx,control.wrapper));
    JS_SetPropertyStr(s.ctx,g,"rawDom",JS_NewCFunction(s.ctx,native_dom,"rawDom",4));JS_SetPropertyStr(s.ctx,g,"nativeBrand",JS_NewCFunction(s.ctx,brand,"brand",1));
    JS_SetPropertyStr(s.ctx,g,"flag",JS_NewCFunction(s.ctx,flag,"flag",3));JS_SetPropertyStr(s.ctx,g,"value",JS_NewCFunction(s.ctx,value_get,"value",0));JS_SetPropertyStr(s.ctx,g,"caret",JS_NewCFunction(s.ctx,position_get,"caret",0));JS_FreeValue(s.ctx,g);
    FILE *f=fopen("build/goal-20261009/document-command-bindings.js","rb");fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *bytes=malloc(n+1);fread(bytes,1,n,f);fclose(f);bytes[n]=0;
    s.hooks=JS_Eval(s.ctx,bytes,n,"new-document-command-boundary",JS_EVAL_TYPE_GLOBAL);free(bytes);
    if(JS_IsException(s.hooks)){JSValue e=JS_GetException(s.ctx);const char *text=JS_ToCString(s.ctx,e);printf("COMMAND EXCEPTION %s\n",text?text:"");JS_FreeCString(s.ctx,text);JS_FreeValue(s.ctx,e);return 1;}
    /* Register the actual hook before any command invokes it. This repairs
     * the one-run fixture failure, not the recorded product result. */
    JSValue run=JS_GetPropertyStr(s.ctx,s.hooks,"run"),report=JS_Call(s.ctx,run,s.hooks,0,NULL);JS_FreeValue(s.ctx,run);
    if(JS_IsException(report)){JSValue e=JS_GetException(s.ctx);const char *text=JS_ToCString(s.ctx,e);printf("COMMAND EXCEPTION %s\n",text?text:"");JS_FreeCString(s.ctx,text);JS_FreeValue(s.ctx,e);return 1;}
    JSValue count=JS_GetPropertyStr(s.ctx,report,"checks"),bad=JS_GetPropertyStr(s.ctx,report,"failed");int32_t checks=0,failed=0;JS_ToInt32(s.ctx,&checks,count);JS_ToInt32(s.ctx,&failed,bad);JS_FreeValue(s.ctx,count);JS_FreeValue(s.ctx,bad);JS_FreeValue(s.ctx,report);
    printf("document commands: %d new checks, %d failed\n",checks,failed);
    JS_FreeValue(s.ctx,s.hooks);JS_FreeValue(s.ctx,root.wrapper);JS_FreeValue(s.ctx,control.wrapper);JS_FreeContext(s.ctx);JS_FreeRuntime(rt);free(control.value);return failed!=0;
}
