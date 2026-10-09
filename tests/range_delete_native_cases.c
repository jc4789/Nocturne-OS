#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "quickjs.h"
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner;struct node *parent,*first,*last,*prev,*next,*all_next,*shadow_host,*slot_assigned_first;int type,tag;JSValue data,wrapper;bool connected;} node_t;
struct web_doc {node_t *focus;int caret;};
enum{T_select};
static JSClassID node_class;static JSContext *context;static node_t *all;static web_doc native_doc;
static int total,failed,removals,sets;
static bool fail_mutation;
static node_t *node_ancestor(node_t *n,int tag){return NULL;}
static bool assignment_structure(node_t *p,node_t *n){return false;}
static bool shadow_under(node_t *focus,node_t *n){return false;}
static bool is_slot(node_t *n){return false;}
static node_t *doc_node_root(node_t *n,bool shadow){return n;}
static void doc_slot_signal(node_t *n){}
static void web_frames_detach_tree(web_doc *d,node_t *n){}
static void stylesheet_detach(node_t *n){}
static void indices(node_t *n){}
static void textarea_changed(node_t *n){}
static void structure_changed(web_doc *d,node_t *p,node_t *n){removals++;}
static void doc_shadow_reassign(web_doc *d){}
static void web_select_inserted(web_doc *d,node_t *n,node_t *p){}
static void web_select_sync(web_doc *d,node_t *n,bool b){}
/* PRODUCT_REMOVE */
static JSValue wrap(node_t *n){return n?JS_DupValue(context,n->wrapper):JS_NULL;}
static node_t *unwrap(JSValueConst value){return JS_GetOpaque(value,node_class);}
static void link_node(node_t *p,node_t *n){if(n->parent)detach(n);n->parent=p;n->prev=p->last;if(p->last)p->last->next=n;else p->first=n;p->last=n;}
static JSValue make_node(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    int type=0;if(!argc||JS_ToInt32(ctx,&type,argv[0]))return JS_EXCEPTION;
    node_t *n=calloc(1,sizeof *n);if(!n)return JS_ThrowOutOfMemory(ctx);n->owner=&native_doc;n->type=type;
    n->data=argc>2?JS_DupValue(ctx,argv[2]):JS_NewString(ctx,"");n->wrapper=JS_NewObjectClass(ctx,node_class);
    if(JS_IsException(n->wrapper)){JS_FreeValue(ctx,n->data);free(n);return JS_EXCEPTION;}
    JS_SetOpaque(n->wrapper,n);n->all_next=all;all=n;return wrap(n);
}
static JSValue raw_dom(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){
    const char *op=argc?JS_ToCString(ctx,argv[0]):NULL;if(!op)return JS_EXCEPTION;
    node_t *n=argc>1?unwrap(argv[1]):NULL;JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"isNode")){result=JS_NewBool(ctx,argc>2&&unwrap(argv[2]));goto done;}
    if(!n){result=JS_ThrowTypeError(ctx,"Native Node required");goto done;}
    if(!strcmp(op,"get")){
        const char *key=argc>2?JS_ToCString(ctx,argv[2]):NULL;if(!key){result=JS_EXCEPTION;goto done;}
        if(!strcmp(key,"nodeType"))result=JS_NewInt32(ctx,n->type);
        else if(!strcmp(key,"parentNode"))result=wrap(n->parent);
        else if(!strcmp(key,"nodeValue"))result=JS_DupValue(ctx,n->data);
        else if(!strcmp(key,"childNodes")){result=JS_NewArray(ctx);uint32_t i=0;for(node_t *c=n->first;c;c=c->next)JS_SetPropertyUint32(ctx,result,i++,wrap(c));}
        else if(!strcmp(key,"ownerDocument")){node_t *d=all;while(d&&d->type!=9)d=d->all_next;result=wrap(d);}
        else if(!strcmp(key,"shadowHost"))result=JS_NULL;
        else result=JS_ThrowTypeError(ctx,"Unsupported fixture getter: %s",key);
        JS_FreeCString(ctx,key);
    }else if(!strcmp(op,"insert")){
        node_t *child=argc>2?unwrap(argv[2]):NULL;if(!child)result=JS_ThrowTypeError(ctx,"Native child required");else link_node(n,child);
    }else if(!strcmp(op,"remove")){
        if(fail_mutation){fail_mutation=false;result=JS_ThrowInternalError(ctx,"fixture actual mutation rejection");}else doc_node_remove(n->owner,n);
    }else if(!strcmp(op,"set")){
        if(fail_mutation){fail_mutation=false;result=JS_ThrowInternalError(ctx,"fixture actual mutation rejection");}
        else{JSValue value=JS_DupValue(ctx,argv[3]);JS_FreeValue(ctx,n->data);n->data=value;sets++;}
    }else result=JS_ThrowTypeError(ctx,"Unsupported fixture mutation: %s",op);
done:JS_FreeCString(ctx,op);return result;
}
static JSValue host_check(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){total++;if(!argc||JS_ToBool(ctx,argv[0])!=1){failed++;const char *name=argc>1?JS_ToCString(ctx,argv[1]):NULL;printf("FAIL %s\n",name?name:"unnamed");JS_FreeCString(ctx,name);}return JS_UNDEFINED;}
static JSValue host_counts(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){JSValue a=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,a,0,JS_NewInt32(ctx,removals));JS_SetPropertyUint32(ctx,a,1,JS_NewInt32(ctx,sets));return a;}
static JSValue host_fail(JSContext *ctx,JSValueConst receiver,int argc,JSValueConst *argv){fail_mutation=true;return JS_UNDEFINED;}
int main(void){
    setbuf(stdout,NULL);JSRuntime *rt=JS_NewRuntime();context=JS_NewContext(rt);JS_NewClassID(&node_class);JSClassDef cls={.class_name="NativeTestNode"};JS_NewClass(rt,node_class,&cls);
    JSValue g=JS_GetGlobalObject(context);JS_SetPropertyStr(context,g,"nativeDom",JS_NewCFunction(context,raw_dom,"dom",4));JS_SetPropertyStr(context,g,"makeNative",JS_NewCFunction(context,make_node,"make",3));JS_SetPropertyStr(context,g,"check",JS_NewCFunction(context,host_check,"check",2));JS_SetPropertyStr(context,g,"nativeCounts",JS_NewCFunction(context,host_counts,"counts",0));JS_SetPropertyStr(context,g,"rejectNextMutation",JS_NewCFunction(context,host_fail,"reject",0));
    JSValue args[]={JS_NewInt32(context,9),JS_NULL,JS_NewString(context,"")};JS_SetPropertyStr(context,g,"document",make_node(context,JS_UNDEFINED,3,args));JS_FreeValue(context,args[2]);JS_FreeValue(context,g);
    FILE *fp=fopen("build/goal-20261009/range-delete-native-bindings.js","rb");if(!fp)return 2;fseek(fp,0,SEEK_END);long n=ftell(fp);rewind(fp);char *source=malloc(n+1);fread(source,1,n,fp);source[n]=0;fclose(fp);
    JSValue result=JS_Eval(context,source,n,"range-delete-new-bindings",JS_EVAL_TYPE_GLOBAL);free(source);
    if(JS_IsException(result)){JSValue e=JS_GetException(context);const char *message=JS_ToCString(context,e);printf("EXCEPTION %s\n",message?message:"");JS_FreeCString(context,message);JS_FreeValue(context,e);failed++;}JS_FreeValue(context,result);
    printf("range-delete-new-boundaries: %d checks, %d failures\n",total,failed);
    for(node_t *n=all;n;n=n->all_next){JS_FreeValue(context,n->wrapper);JS_FreeValue(context,n->data);}JS_FreeContext(context);JS_FreeRuntime(rt);while(all){node_t *n=all;all=n->all_next;free(n);}return failed?1:0;
}
