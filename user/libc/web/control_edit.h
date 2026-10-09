/* Physical native text-control editing. Included after readonly() in doc.c.
 * All caller text and old native value are owned before running author code. */
struct control_edit_guard {web_doc *document;struct control_edit_guard *previous;};
static struct control_edit_guard *control_edits_running;
static bool control_edit_target(web_doc *d,node_t *n,bool write){
    if(!d||!d->live||d->inert||!n||n->owner!=d||d->focus!=n||n->type!=N_ELEM||n->foreign||
       (n->tag!=T_input&&n->tag!=T_textarea)||doc_node_root(n,true)!=d->root||
       web_control_disabled(n)||web_dialog_inert(d,n)||(write&&readonly(n)))return false;
    if(n->tag==T_input){enum web_input_kind kind=web_input_type(n);
        if(kind==WEB_INPUT_HIDDEN||kind>=WEB_INPUT_RANGE)return false;}
    return true;
}
static web_doc *control_edit_owner(web_doc *top,node_t *n){
    if(!top||!n||!n->owner||web_focused(top)!=n)return NULL;
    web_doc *owner=n->owner,*p=owner;
    for(;p&&p!=top;p=p->frame_parent)if(!p->live)return NULL;
    return p==top?owner:NULL;
}
static bool control_edit_maxlength(node_t *n,uint32_t *limit){
    if(n->tag!=T_textarea&&web_input_type(n)>WEB_INPUT_PASSWORD)return false;
    const unsigned char *p=(const unsigned char *)node_attr(n,"maxlength");if(!p)return false;
    while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f')p++;
    if(*p=='+')p++;if(*p<'0'||*p>'9')return false;
    uint64_t value=0;while(*p>='0'&&*p<='9'){value=value*10+*p++-'0';if(value>UINT32_MAX)value=UINT32_MAX;}
    *limit=(uint32_t)value;return true;
}
static bool control_edit_utf8(const char *text,size_t length){
    for(size_t i=0;i<length;){
        unsigned char c=(unsigned char)text[i++];if(!c)return false;if(c<0x80)continue;
        unsigned count=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!count||count>length-i)return false;
        uint32_t scalar=c&(count==1?31:count==2?15:7);
        for(unsigned j=0;j<count;j++){c=(unsigned char)text[i++];if((c&0xc0)!=0x80)return false;scalar=(scalar<<6)|(c&63);}
        if(scalar<(count==1?0x80u:count==2?0x800u:0x10000u)||scalar>0x10ffffu||
           (scalar>=0xd800&&scalar<=0xdfff))return false;
    }return true;
}
static int control_user_replace(web_doc *d,node_t *n,uint32_t start,uint32_t end,
                                const char *text,size_t text_len,const char *input_type,
                                bool null_data){
    if(!control_edit_target(d,n,false))return 0;
    if(!control_edit_target(d,n,true))return 1;
    for(struct control_edit_guard *p=control_edits_running;p;p=p->previous)if(p->document==d)return 1;
    doc_control_init(d,n);
    const char *value=web_input_edit_text(n);size_t bytes=strlen(value);
    if(bytes>(16u<<20)||text_len>(16u<<20)||(!text&&text_len)||
       (text_len&&!control_edit_utf8(text,text_len))||bytes+text_len>(16u<<20)+6u)return 1;
    uint32_t length=doc_utf16_length(value);if(start>end||end>length)return 1;
    char *old=malloc(bytes+1),*data=malloc(text_len+1);sbuf proposal={0};
    if(!old||!data){free(old);free(data);return 1;}
    memcpy(old,value,bytes+1);if(text_len)memcpy(data,text,text_len);data[text_len]=0;
    proposal.cap=bytes+text_len+7;proposal.p=malloc(proposal.cap);
    if(!proposal.p){free(old);free(data);return 1;}
    proposal.p[0]=0;
    uint32_t inserted=0,caret=start;size_t leading=0;
    {
        control_slice(&proposal,old,0,start);
        size_t at=proposal.n;
        for(size_t i=0;i<text_len;i++){
            char c=data[i];if(c=='\r'){
                if(n->tag==T_textarea){sb_putc(&proposal,'\n');if(i+1<text_len&&data[i+1]=='\n')i++;}
            }else if(c!='\n'||n->tag==T_textarea)sb_putc(&proposal,c);
        }
        inserted=doc_utf16_length(proposal.p+at);caret=start+inserted;
        control_slice(&proposal,old,end,length);
        if(proposal.n>(16u<<20))goto unchanged;
        if(n->tag==T_input&&(web_input_type(n)==WEB_INPUT_URL||web_input_type(n)==WEB_INPUT_EMAIL)){
            while(leading<proposal.n&&(proposal.p[leading]==' '||proposal.p[leading]=='\t'||proposal.p[leading]=='\n'||proposal.p[leading]=='\r'||proposal.p[leading]=='\f'))leading++;
            uint32_t removed=doc_byte_to_utf16(proposal.p,leading);caret=caret>removed?caret-removed:0;
        }
        uint32_t limit=0,new_units=length-(end-start)+inserted;
        if(control_edit_maxlength(n,&limit)&&new_units>limit&&new_units>length)goto unchanged;
    }
    node_t *root=d->root;uint32_t saved_start=n->selection_start,saved_end=n->selection_end;
    uint8_t direction=n->selection_direction;int kind=n->tag==T_textarea?-1:(int)web_input_type(n);
    struct control_edit_guard guard={d,control_edits_running};control_edits_running=&guard;
    bool allowed=web_js_input_event(d,n,"beforeinput",input_type,null_data?NULL:data,text_len);
    if(allowed&&control_edit_target(d,n,true)&&d->root==root&&
       kind==(n->tag==T_textarea?-1:(int)web_input_type(n))&&
       n->selection_start==saved_start&&n->selection_end==saved_end&&n->selection_direction==direction&&
       strlen(web_input_edit_text(n))==bytes&&!memcmp(old,web_input_edit_text(n),bytes)){
        uint32_t limit=0,new_units=length-(end-start)+inserted;
        bool max_ok=!control_edit_maxlength(n,&limit)||new_units<=limit||new_units<=length;
        bool changed=max_ok&&web_input_user_value(d,n,proposal.p,proposal.n);
        if(changed){
            doc_control_selection(d,n,caret,caret,0);
            /* Dispatch at the original owner only while it is still live. */
            web_js_input_event(d,n,"input",input_type,null_data?NULL:data,text_len);
            control_edits_running=guard.previous;sb_free(&proposal);free(old);free(data);return 3;
        }
    }
    control_edits_running=guard.previous;
unchanged:sb_free(&proposal);free(old);free(data);return 1;
}
int web_control_edit(web_doc *top,web_node *target,const char *input_type,const char *data,size_t length){
    web_doc *d=control_edit_owner(top,target);if(!d||!input_type||!control_edit_target(d,target,false))return 0;
    bool paste=!strcmp(input_type,"insertFromPaste"),cut=!strcmp(input_type,"deleteByCut");
    if(!paste&&!cut)return 0;
    if(!control_edit_target(d,target,true))return 1;
    doc_control_init(d,target);
    if(cut&&(data||length))return 0;
    uint32_t start=target->selection_start,end=target->selection_end;
    if((cut&&start==end)||(paste&&!length))return 1;
    return control_user_replace(d,target,start,end,paste?data:"",paste?length:0,input_type,cut);
}
char *web_control_selected_text(web_doc *top,web_node *target,size_t *length){
    if(length)*length=0;web_doc *d=control_edit_owner(top,target);
    if(!d||!control_edit_target(d,target,false)||!doc_control_selection_supported(target)||
       (target->tag==T_input&&web_input_type(target)==WEB_INPUT_PASSWORD))return NULL;
    doc_control_init(d,target);const char *value=web_input_edit_text(target);size_t bytes=strlen(value);
    if(bytes>(16u<<20))return NULL;
    sbuf text={0};text.cap=bytes+7;text.p=malloc(text.cap);if(!text.p)return NULL;text.p[0]=0;
    control_slice(&text,value,target->selection_start,target->selection_end);
    if(length)*length=text.n;return text.p;
}
