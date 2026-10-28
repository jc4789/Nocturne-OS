/* Included after native control selection, before native_dom_impl. */
struct document_command_guard {web_doc *document;struct document_command_guard *previous;};
static struct document_command_guard *document_commands_running;
static bool document_command_live(struct web_js_state *s,web_doc *d,node_t *document){
    return d&&document&&document->type==N_DOC&&d->root==document&&d->live&&!d->inert&&
        d->js&&d->js->ctx&&!d->js->disabled&&JS_GetRuntime(d->js->ctx)==JS_GetRuntime(s->ctx);
}
static node_t *document_command_target(web_doc *d,bool insert){
    node_t *target=d->focus;
    if(!target||target->owner!=d||!doc_node_connected(target)||!doc_control_selection_supported(target)||
       web_control_disabled(target)||web_dialog_inert(d,target)||(insert&&node_attr(target,"readonly")))return NULL;
    return target;
}
static bool document_command_maxlength(node_t *target,uint32_t *limit){
    const unsigned char *p=(const unsigned char *)node_attr(target,"maxlength");if(!p)return false;
    while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f')p++;
    if(*p=='+')p++;if(*p<'0'||*p>'9')return false;
    uint64_t n=0;while(*p>='0'&&*p<='9'){n=n*10+*p++-'0';if(n>UINT32_MAX)n=UINT32_MAX;}
    *limit=(uint32_t)n;return true;
}
static JSValue document_command_event(struct web_js_state *owner,node_t *target,const char *type,JSValueConst text,JSValueConst input_type){
    JSContext *ctx=owner->ctx;JSValue args[]={wrap(owner,target),JS_NewString(ctx,type),JS_DupValue(ctx,text),JS_DupValue(ctx,input_type)};
    JSValue result=JS_EXCEPTION;
    if(!JS_IsException(args[0])&&!JS_IsException(args[1]))result=custom_element_hook(owner,"documentCommandEvent",4,args);
    for(int i=0;i<4;i++)JS_FreeValue(ctx,args[i]);return result;
}
/* The plain-text commands a focused text control takes (js_document_commands.js
   computes the value each one makes); all but selectAll edit the value. */
static bool document_command_text(const char *command,size_t len,bool *edit){
    static const char *const edits[]={"inserttext","delete","forwarddelete","insertparagraph","insertlinebreak"};
    *edit=false;
    if(len==9&&!memcmp(command,"selectall",9))return true;
    for(size_t i=0;i<sizeof edits/sizeof *edits;i++)if(strlen(edits[i])==len&&!memcmp(command,edits[i],len))return *edit=true;
    return false;
}
static JSValue document_command_dom(struct web_js_state *s,node_t *document,int argc,JSValueConst *argv){
    JSContext *ctx=s->ctx;web_doc *d=document?document->owner:NULL;
    if(!document||document->type!=N_DOC)return JS_ThrowTypeError(ctx,"Document receiver required");
    if(argc<4)return JS_ThrowTypeError(ctx,"Command operation and id required");
    size_t command_len=0;const char *mode=JS_ToCString(ctx,argv[2]),*command=JS_ToCStringLen(ctx,&command_len,argv[3]);
    if(!mode||!command){JS_FreeCString(ctx,mode);JS_FreeCString(ctx,command);return JS_EXCEPTION;}
    /* insert: a command that edits the value, so a readonly control does not take it. */
    bool insert=false,select=document_command_text(command,command_len,&insert)&&!insert;
    bool live=document_command_live(s,d,document),supported=live&&(insert||select);
    JSValue result=JS_FALSE;
    /* The editing layer's questions (js_document_commands.js, js_editing.js). */
    if(!strcmp(mode,"textControl"))result=JS_NewBool(ctx,live&&document_command_target(d,false));
    else if(!strcmp(mode,"designMode")){
        /* The bindings own design mode enabled; this copy is for :read-write and the editing keys. */
        int on=argc>4?JS_ToBool(ctx,argv[4]):0;
        if(on<0)result=JS_EXCEPTION;
        else if(d&&d->root==document&&d->design_mode!=(on>0)){d->design_mode=on>0;css_mark_dirty(d,d->root);d->dirty=true;}
    }else if(!strcmp(mode,"color")){
        /* The value as getComputedStyle() serializes the color (color.c, the
           exact used colour, not its 8 bits), or null: not a color, or one
           that names currentcolor, which has no value of its own. */
        uint32_t color=0;struct css_color used;
        if(!css_color(command,command_len,&color)||color==COLOR_CURRENT||
           !css_color_parse_used(command,command_len,NULL,NULL,&used))result=JS_NULL;
        else{
            sbuf text={0};css_color_serialize(&used,false,&text);
            result=text.p?JS_NewStringLen(ctx,text.p,text.n):JS_ThrowOutOfMemory(ctx);
            sb_free(&text);
        }
    }else if(!strcmp(mode,"fontKeywords")){
        result=JS_NewArray(ctx);
        for(unsigned i=0;i<8&&!JS_IsException(result);i++)
            if(JS_SetPropertyUint32(ctx,result,i,JS_NewFloat64(ctx,css_font_keyword_px(i)))<0){JS_FreeValue(ctx,result);result=JS_EXCEPTION;}
    }
    else if(!strcmp(mode,"supported"))result=JS_NewBool(ctx,supported);
    else if(!strcmp(mode,"state")||!strcmp(mode,"indeterm"))result=JS_FALSE;
    else if(!strcmp(mode,"value"))result=JS_NewString(ctx,"");
    else if(!strcmp(mode,"enabled"))result=JS_NewBool(ctx,supported&&document_command_target(d,insert));
    else if(!strcmp(mode,"prepare")){
        node_t *target=supported?document_command_target(d,insert):NULL;
        if(!target)result=JS_NULL;
        else{
            doc_control_init(d,target);result=JS_NewObjectProto(ctx,JS_NULL);
            if(!JS_IsException(result)){
#define COMMAND_SNAPSHOT(key,value) do{JSValue v=(value);if(JS_IsException(v)||JS_SetPropertyStr(ctx,result,key,v)<0){JS_FreeValue(ctx,result);result=JS_EXCEPTION;goto done;}}while(0)
                COMMAND_SNAPSHOT("target",wrap(s,target));COMMAND_SNAPSHOT("generation",wrap(s,d->root));
                COMMAND_SNAPSHOT("value",JS_NewString(ctx,web_input_edit_text(target)));
                COMMAND_SNAPSHOT("start",JS_NewUint32(ctx,target->selection_start));COMMAND_SNAPSHOT("end",JS_NewUint32(ctx,target->selection_end));
                COMMAND_SNAPSHOT("kind",JS_NewInt32(ctx,target->tag==T_textarea?-1:(int)web_input_type(target)));
                COMMAND_SNAPSHOT("multiline",JS_NewBool(ctx,target->tag==T_textarea));
                COMMAND_SNAPSHOT("trimURL",JS_NewBool(ctx,target->tag==T_input&&web_input_type(target)==WEB_INPUT_URL));
#undef COMMAND_SNAPSHOT
            }
        }
    }else if(!strcmp(mode,"execute")){
        if(!supported||argc<8)goto done;
        for(struct document_command_guard *p=document_commands_running;p;p=p->previous)if(p->document==d)goto done;
        JSValue snapshot=argv[4],tv=JS_GetPropertyStr(ctx,snapshot,"target"),gv=JS_GetPropertyStr(ctx,snapshot,"generation");
        JSValue ov=JS_GetPropertyStr(ctx,snapshot,"value"),sv=JS_GetPropertyStr(ctx,snapshot,"start"),ev=JS_GetPropertyStr(ctx,snapshot,"end"),kv=JS_GetPropertyStr(ctx,snapshot,"kind");
        node_t *target=unwrap(ctx,tv),*generation=unwrap(ctx,gv);uint32_t start=0,end=0,caret=0;int32_t kind=0;
        size_t old_len=0,new_len=0;const char *old=JS_ToCStringLen(ctx,&old_len,ov),*next=JS_ToCStringLen(ctx,&new_len,argv[5]);
        bool converted=old&&next&&!JS_ToUint32(ctx,&start,sv)&&!JS_ToUint32(ctx,&end,ev)&&!JS_ToInt32(ctx,&kind,kv)&&!JS_ToUint32(ctx,&caret,argv[6]);
        if(!converted){result=JS_EXCEPTION;goto release;}
        if(!document_command_live(s,d,document)||generation!=d->root||target!=document_command_target(d,insert)||
           kind!=(target&&target->tag==T_textarea?-1:target?(int)web_input_type(target):-2)||
           old_len!=strlen(web_input_edit_text(target))||memcmp(old,web_input_edit_text(target),old_len)||
           target->selection_start!=start||target->selection_end!=end)goto release;
        if(select){doc_control_selection(d,target,0,UINT32_MAX,0);result=JS_TRUE;goto release;}
        if(strlen(next)!=new_len)goto release;
        uint32_t limit=0,new_units=doc_utf16_length(next),old_units=doc_utf16_length(old);
        if(document_command_maxlength(target,&limit)&&new_units>limit&&new_units>old_units)goto release;
        /* No beforeinput: Input Events fires it only for the user's own input, not
           for a script's command. The input event carries the command's inputType. */
        struct document_command_guard guard={d,document_commands_running};document_commands_running=&guard;
        if(!web_input_user_value(d,target,next,new_len))result=oom(ctx);
        else{
            doc_control_selection(d,target,caret,caret,0);result=JS_TRUE;
            JSValue input_type=argc>8?JS_DupValue(ctx,argv[8]):JS_NewString(ctx,"insertText");
            JSValue event=JS_IsException(input_type)?JS_EXCEPTION:document_command_event(d->js,target,"input",argv[7],input_type);
            if(JS_IsException(event))result=JS_EXCEPTION;
            JS_FreeValue(ctx,event);JS_FreeValue(ctx,input_type);
        }
        document_commands_running=guard.previous;
release:
        JS_FreeCString(ctx,old);JS_FreeCString(ctx,next);
        JS_FreeValue(ctx,tv);JS_FreeValue(ctx,gv);JS_FreeValue(ctx,ov);JS_FreeValue(ctx,sv);JS_FreeValue(ctx,ev);JS_FreeValue(ctx,kv);
    }else result=JS_ThrowTypeError(ctx,"Unknown command operation");
done:JS_FreeCString(ctx,mode);JS_FreeCString(ctx,command);return result;
}
