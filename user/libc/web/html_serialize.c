/* https://html.spec.whatwg.org/multipage/parsing.html#serialising-html-fragments
 * native DOMだけを直列化する。innerHTMLはshadow treeを公開しない。 */
#include "html_serialize.h"

static bool serializes_as_void(const node_t *n) {
    if (!n || n->type != N_ELEM || n->namespace_id != NS_HTML) return false;
    switch (n->tag) {
    case T_area: case T_base: case T_br: case T_col: case T_embed: case T_hr:
    case T_img: case T_input: case T_link: case T_meta: case T_source:
    case T_track: case T_wbr: case T_frame: case T_keygen: case T_param:
        return true;
    default: {
        const char *local = n->local_name ? n->local_name : n->name;
        return local && (!strcmp(local, "basefont") || !strcmp(local, "bgsound"));
    }
    }
}
/* HTML "serializing HTML fragments": an HTML, SVG or MathML element's tag is
   its local name, any other element's its qualified name. */
static const char *html_tag_name(const node_t *n) {
    if (n->namespace_id == NS_HTML || n->namespace_id == NS_SVG || n->namespace_id == NS_MATHML)
        return n->local_name ? n->local_name : n->name;
    return n->raw_name ? n->raw_name : n->name;
}

static bool literal_text(const node_t *n) {
    const node_t *p = n->parent;
    if (!p || p->type != N_ELEM || p->namespace_id != NS_HTML) return false;
    switch (p->tag) {
    case T_style: case T_script: case T_xmp: case T_iframe: case T_noembed:
    case T_noframes: case T_plaintext: return true;
    case T_noscript: return n->owner && n->owner->live && !n->owner->inert;
    default: return false;
    }
}

static void escaped(sbuf *out, const char *text, size_t length, bool attribute) {
    if (!text) return;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '&') sb_puts(out, "&amp;");
        else if (c == '<') sb_puts(out, "&lt;");
        else if (c == '>') sb_puts(out, "&gt;");
        else if (attribute && c == '"') sb_puts(out, "&quot;");
        else if (c == 0xc2 && i + 1 < length && (unsigned char)text[i + 1] == 0xa0) {
            sb_puts(out, "&nbsp;"); i++;
        } else sb_putc(out, (char)c);
    }
}

static void attribute_name(const struct attr *a, sbuf *out) {
    const char *local = a->local ? a->local : a->raw ? a->raw : a->name;
    if (!a->namespace_uri) sb_puts(out, local);
    else if (!strcmp(a->namespace_uri, "http://www.w3.org/XML/1998/namespace")) {
        sb_puts(out, "xml:"); sb_puts(out, local);
    } else if (!strcmp(a->namespace_uri, "http://www.w3.org/2000/xmlns/")) {
        if (strcmp(local, "xmlns")) sb_puts(out, "xmlns:");
        sb_puts(out, local);
    } else if (!strcmp(a->namespace_uri, "http://www.w3.org/1999/xlink")) {
        sb_puts(out, "xlink:"); sb_puts(out, local);
    } else sb_puts(out, a->raw ? a->raw : local);
}

struct serialization_options {bool serializable;node_t **roots;size_t count;};
static void serialize_node(const node_t *node,sbuf *out,const struct serialization_options *options);
static bool include_shadow(const node_t *root,const struct serialization_options *options){
    if(!options)return false;
    if(options->serializable && root->shadow_serializable)return true;
    for(size_t i=0;i<options->count;i++)if(options->roots[i]==root)return true;
    return false;
}
static void serialize_children(const node_t *node, sbuf *out,const struct serialization_options *options) {
    if (!node || serializes_as_void(node)) return;
    const node_t *parent = node->type == N_ELEM && node->namespace_id == NS_HTML && node->tag == T_template ?
        node->template_content : node;
    if(node->shadow_root && include_shadow(node->shadow_root,options)){
        node_t *r=node->shadow_root;
        sb_puts(out,"<template shadowrootmode=\"");sb_puts(out,r->shadow_closed?"closed":"open");sb_putc(out,'"');
        if(r->shadow_delegates_focus)sb_puts(out," shadowrootdelegatesfocus=\"\"");
        if(r->shadow_serializable)sb_puts(out," shadowrootserializable=\"\"");
        if(r->shadow_manual)sb_puts(out," shadowrootslotassignment=\"manual\"");
        if(r->shadow_clonable)sb_puts(out," shadowrootclonable=\"\"");
        /* shouldAppendRegistryAttribute: unless the node document's and the shadow root's
           registries are both null or both global. */
        int32_t document_registry=doc_custom_registry(r->owner?r->owner->root:NULL,NULL);
        int32_t shadow_registry=doc_custom_registry(r,NULL);
        if(!(document_registry==-1&&shadow_registry==-1) && !(document_registry==0&&shadow_registry==0))
            sb_puts(out," shadowrootcustomelementregistry=\"\"");
        sb_putc(out,'>');serialize_children(r,out,options);sb_puts(out,"</template>");
    }
    for (const node_t *child = parent ? parent->first : NULL; child; child = child->next)
        serialize_node(child, out,options);
}

static void serialize_node(const node_t *node, sbuf *out,const struct serialization_options *options) {
    if (!node) return;
    if (node->type == N_TEXT) {
        if (literal_text(node)) sb_put(out, node->text ? node->text : "", node->textlen);
        else escaped(out, node->text, node->textlen, false);
    } else if (node->type == N_COMMENT) {
        sb_puts(out, "<!--"); sb_put(out, node->text ? node->text : "", node->textlen); sb_puts(out, "-->");
    } else if (node->type == N_PI) {
        sb_puts(out, "<?"); sb_puts(out, node->name); sb_putc(out, ' ');
        sb_put(out, node->text ? node->text : "", node->textlen); sb_puts(out, "?>");
    } else if (node->type == N_DOCTYPE) {
        sb_puts(out, "<!DOCTYPE "); sb_puts(out, node->name); sb_putc(out, '>');
    } else if (node->type == N_ELEM) {
        const char *name = html_tag_name(node);
        sb_putc(out, '<'); sb_puts(out, name);
        if(node->custom_is && !node_attr(node,"is")){
            sb_puts(out," is=\"");escaped(out,node->custom_is,strlen(node->custom_is),true);sb_putc(out,'"');
        }
        for (int i = 0; i < node->nattrs; i++) {
            const struct attr *a = &node->attrs[i];
            sb_putc(out, ' '); attribute_name(a, out); sb_puts(out, "=\"");
            escaped(out, a->value, strlen(a->value), true); sb_putc(out, '"');
        }
        sb_putc(out, '>');
        if (!serializes_as_void(node)) {
            serialize_children(node, out,options);
            sb_puts(out, "</"); sb_puts(out, name); sb_putc(out, '>');
        }
    } else if (node->type == N_DOC || node->type == N_FRAGMENT) serialize_children(node, out,options);
}
void html_serialize_node(const node_t *node,sbuf *out){serialize_node(node,out,NULL);}
void html_serialize_children(const node_t *node,sbuf *out){serialize_children(node,out,NULL);}
void html_serialize_get(const node_t *node,sbuf *out,bool serializable,node_t **roots,size_t count){
    struct serialization_options options={serializable,roots,count};serialize_children(node,out,&options);
}

/* XML serialization keeps native namespace identity. Its output and namespace
   scope are fallible temporary storage, never a second DOM or a node arena.
   https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization */
static const char XML_NS[]="http://www.w3.org/XML/1998/namespace";
static const char XMLNS_NS[]="http://www.w3.org/2000/xmlns/";
struct xml_binding {
    struct xml_binding *next;
    const char *prefix,*uri;size_t length;
    char generated[24];
};
struct xml_frame {
    const node_t *node,*child;
    const char *local,*prefix,*inherited;size_t prefix_length;
    struct xml_binding *mark;
    bool entered,close;
};
struct xml_serialization {
    sbuf *out;web_doc *document;
    struct xml_binding *bindings;
    uint64_t prefix_index,steps;
    bool strict;
    enum xml_serialization_result result;
};
static bool xml_equal(const char *a,const char *b) {
    return a==b || !strcmp(a?a:"",b?b:"");
}
static bool xml_checkpoint(struct xml_serialization *x) {
    if(x->document&&!web_native_checkpoint(x->document)){x->result=XML_SERIALIZE_CANCELLED;return false;}
    return x->result==XML_SERIALIZED;
}
static bool xml_write(struct xml_serialization *x,const char *text,size_t length) {
    if(x->result!=XML_SERIALIZED)return false;
    sbuf *b=x->out;
    if(b->n==SIZE_MAX || length>SIZE_MAX-b->n-1){x->result=XML_SERIALIZE_OOM;return false;}
    size_t need=b->n+length+1;
    if(need>b->cap){
        size_t capacity=b->cap?b->cap:64;
        while(capacity<need)capacity=capacity>SIZE_MAX/2?need:capacity*2;
        char *p=realloc(b->p,capacity);
        if(!p){x->result=XML_SERIALIZE_OOM;return false;}
        b->p=p;b->cap=capacity;
    }
    for(size_t copied=0;copied<length;){
        size_t part=length-copied<65536?length-copied:65536;
        if(length>65536&&!xml_checkpoint(x))return false;
        memcpy(b->p+b->n+copied,text+copied,part);copied+=part;
    }
    b->n+=length;b->p[b->n]=0;return true;
}
static bool xml_literal(struct xml_serialization *x,const char *text) {
    return xml_write(x,text,strlen(text));
}
static bool xml_chars(const char *text,size_t length) {
    for(size_t i=0;i<length;){
        uint32_t cp=(unsigned char)text[i++],minimum=0;unsigned extra=0;
        if(cp>=0xc2&&cp<=0xdf){cp&=31;extra=1;minimum=0x80;}
        else if(cp>=0xe0&&cp<=0xef){cp&=15;extra=2;minimum=0x800;}
        else if(cp>=0xf0&&cp<=0xf4){cp&=7;extra=3;minimum=0x10000;}
        else if(cp>=0x80)return false;
        if(extra>length-i)return false;
        while(extra--){unsigned c=(unsigned char)text[i++];if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&63);}
        if(cp<minimum || !(cp==9||cp==10||cp==13||(cp>=0x20&&cp<=0xd7ff)||
            (cp>=0xe000&&cp<=0xfffd)||(cp>=0x10000&&cp<=0x10ffff)))return false;
    }
    return true;
}
static bool xml_invalid(struct xml_serialization *x) {x->result=XML_SERIALIZE_INVALID;return false;}
static bool xml_escape(struct xml_serialization *x,const char *text,size_t length,bool attribute) {
    if(!text)text="";
    if(x->strict&&!xml_chars(text,length))return xml_invalid(x);
    size_t start=0;
    for(size_t i=0;i<length;i++){
        if((i&4095)==0&&!xml_checkpoint(x))return false;
        const char *replacement=NULL;
        switch(text[i]){
        case '&':replacement="&amp;";break;case '<':replacement="&lt;";break;case '>':replacement="&gt;";break;
        case '"':if(attribute)replacement="&quot;";break;
        case '\t':if(attribute)replacement="&#9;";break;
        case '\n':if(attribute)replacement="&#xA;";break;
        case '\r':if(attribute)replacement="&#xD;";break;
        }
        if(replacement){if(!xml_write(x,text+start,i-start)||!xml_literal(x,replacement))return false;start=i+1;}
    }
    return xml_write(x,text+start,length-start);
}
static bool xml_prefix_equal(const struct xml_binding *b,const char *p,size_t length) {
    return b->length==length&&!memcmp(b->prefix,p,length);
}
static struct xml_binding *xml_prefix(struct xml_serialization *x,const char *p,size_t length) {
    for(struct xml_binding *b=x->bindings;b;b=b->next)if(xml_prefix_equal(b,p,length))return b;
    return NULL;
}
static struct xml_binding *xml_preferred(struct xml_serialization *x,const char *uri,const char *preferred,size_t length) {
    /* An empty authored prefix declaration is not an alias for an element in
       the null namespace. Such elements need a default namespace reset. */
    if(!uri||!*uri)return NULL;
    if(preferred){struct xml_binding *b=xml_prefix(x,preferred,length);if(b&&xml_equal(b->uri,uri))return b;}
    for(struct xml_binding *b=x->bindings;b;b=b->next)
        if(xml_equal(b->uri,uri)&&xml_prefix(x,b->prefix,b->length)==b)return b;
    return NULL;
}
static struct xml_binding *xml_bind(struct xml_serialization *x,const char *p,size_t length,const char *uri) {
    struct xml_binding *b=malloc(sizeof *b);
    if(!b){x->result=XML_SERIALIZE_OOM;return NULL;}
    *b=(struct xml_binding){.next=x->bindings,.prefix=p,.length=length,.uri=uri};x->bindings=b;return b;
}
static bool xml_local_prefix(struct xml_serialization *x,struct xml_binding *mark,const char *p,size_t length) {
    for(struct xml_binding *b=x->bindings;b!=mark;b=b->next)if(xml_prefix_equal(b,p,length))return true;
    return false;
}
static struct xml_binding *xml_generated(struct xml_serialization *x,const char *uri) {
    struct xml_binding *b=xml_bind(x,"",0,uri);if(!b)return NULL;
    do {
        if(x->prefix_index==UINT64_MAX){x->result=XML_SERIALIZE_OOM;return NULL;}
        char reversed[20];size_t n=0;uint64_t number=x->prefix_index++;
        do{reversed[n++]=(char)('0'+number%10);number/=10;}while(number);
        b->generated[0]='n';b->generated[1]='s';
        for(size_t i=0;i<n;i++)b->generated[2+i]=reversed[n-i-1];b->generated[2+n]=0;
        b->prefix=b->generated;b->length=n+2;
        /* Compare older live bindings, not this just-allocated candidate. */
        struct xml_binding *old=b->next;while(old&&!xml_prefix_equal(old,b->prefix,b->length))old=old->next;
        if(!old)return b;
    }while(true);
}
static bool xml_qname(struct xml_serialization *x,const char *prefix,size_t length,const char *local) {
    return (!length||(xml_write(x,prefix,length)&&xml_literal(x,":")))&&xml_literal(x,local?local:"");
}
static bool xml_declaration(struct xml_serialization *x,const char *prefix,size_t length,const char *uri) {
    return xml_literal(x,length?" xmlns:":" xmlns")&&(!length||xml_write(x,prefix,length))&&
        xml_literal(x,"=\"")&&xml_escape(x,uri?uri:"",uri?strlen(uri):0,true)&&xml_literal(x,"\"");
}
static const char *xml_attribute_local(const struct attr *a) {return a->local?a->local:a->raw?a->raw:a->name;}
static bool xml_name(struct xml_serialization *x,const char *name) {
    return !x->strict || (name&&!strchr(name,':')&&doc_pi_target_valid(name,strlen(name))) || xml_invalid(x);
}
static bool xml_element(struct xml_serialization *x,struct xml_frame *f) {
    const node_t *n=f->node;const char *uri=doc_element_namespace(n),*local_default=NULL;
    /* The element's own prefix is only the preferred one ("retrieving a
       preferred prefix string"); its local name never splits. */
    f->local=n->local_name?n->local_name:n->name;
    f->prefix=n->prefix;f->prefix_length=n->prefix?strlen(n->prefix):0;
    if(!xml_name(x,f->local))return false;
    for(int i=0;i<n->nattrs;i++){
        if((i&127)==0&&!xml_checkpoint(x))return false;
        const struct attr *a=&n->attrs[i];const char *local=xml_attribute_local(a),*value=a->value?a->value:"";
        if(xml_equal(a->namespace_uri,XMLNS_NS)&&!a->prefix&&!strcmp(local,"xmlns")){local_default=value;continue;}
        if(xml_equal(a->namespace_uri,XMLNS_NS)&&a->prefix&&!xml_equal(value,XML_NS)){
            struct xml_binding *old=xml_prefix(x,local,strlen(local));
            if(!old||!xml_equal(old->uri,value))if(!xml_bind(x,local,strlen(local),*value?value:NULL))return false;
        }
    }
    bool ignore_default=false,declare_default=false;
    struct xml_binding *candidate=NULL;
    if(xml_equal(uri,f->inherited)){
        /* The XML namespace keeps its xml: prefix even when inherited. */
        bool xml=xml_equal(uri,XML_NS);
        f->prefix=xml?"xml":NULL;f->prefix_length=xml?3:0;ignore_default=local_default!=NULL;
    }else{
        /* An explicitly matching default declaration wins over an inherited
           alias when this node did not have a prefix. */
        if(f->prefix || !xml_equal(uri,local_default))candidate=xml_preferred(x,uri,f->prefix,f->prefix_length);
        if(candidate){f->prefix=candidate->prefix;f->prefix_length=candidate->length;if(local_default&&!xml_equal(local_default,XML_NS))f->inherited=*local_default?local_default:NULL;}
        else if(f->prefix){
            if(xml_local_prefix(x,f->mark,f->prefix,f->prefix_length))candidate=xml_generated(x,uri);
            else candidate=xml_bind(x,f->prefix,f->prefix_length,uri);
            if(!candidate)return false;f->prefix=candidate->prefix;f->prefix_length=candidate->length;
            if(local_default&&!xml_equal(local_default,XML_NS))f->inherited=*local_default?local_default:NULL;
        }else{ignore_default=local_default==NULL||!xml_equal(uri,local_default);declare_default=ignore_default;f->inherited=uri;}
    }
    if(!xml_literal(x,"<")||!xml_qname(x,f->prefix,f->prefix_length,f->local))return false;
    /* A candidate added on this element needs its new declaration; borrowed
       candidates already have an in-scope authored/generated declaration. */
    if(candidate&&xml_local_prefix(x,f->mark,candidate->prefix,candidate->length)){
        bool authored=false;
        for(int i=0;i<n->nattrs;i++){const struct attr *a=&n->attrs[i];if(xml_equal(a->namespace_uri,XMLNS_NS)&&a->prefix&&
            strlen(xml_attribute_local(a))==candidate->length&&!memcmp(xml_attribute_local(a),candidate->prefix,candidate->length)&&xml_equal(a->value,uri))authored=true;}
        if(!authored&&!xml_declaration(x,candidate->prefix,candidate->length,uri))return false;
    }
    if(declare_default&&!xml_declaration(x,NULL,0,uri))return false;
    for(int i=0;i<n->nattrs;i++){
        if((i&127)==0&&!xml_checkpoint(x))return false;
        const struct attr *a=&n->attrs[i];const char *local=xml_attribute_local(a),*value=a->value?a->value:"",*ns=a->namespace_uri;
        const char *prefix=NULL;size_t prefix_length=0;
        if(x->strict){
            if(!xml_name(x,local))return false;
            for(int j=0;j<i;j++)if(xml_equal(n->attrs[j].namespace_uri,ns)&&!strcmp(xml_attribute_local(&n->attrs[j]),local))return xml_invalid(x);
        }
        if(!a->prefix&&!strcmp(local,"xmlns")){
            if(x->strict&&!ns)return xml_invalid(x);
            if(ignore_default||!ns)continue;
        }
        if(xml_equal(ns,XMLNS_NS)){
            if(xml_equal(value,XML_NS))continue;
            if(a->prefix){
                struct xml_binding *binding=xml_prefix(x,local,strlen(local));
                if(!binding||!xml_equal(binding->uri,value)||!xml_local_prefix(x,f->mark,local,strlen(local)))continue;
                if(x->strict&&(!*value||xml_equal(value,XMLNS_NS)))return xml_invalid(x);
                prefix="xmlns";prefix_length=5;
            }else if(x->strict&&xml_equal(value,XMLNS_NS))return xml_invalid(x);
        }else if(ns&&*ns){
            struct xml_binding *binding=xml_preferred(x,ns,a->prefix,a->prefix?strlen(a->prefix):0);
            if(!binding){
                if(a->prefix&&!xml_local_prefix(x,f->mark,a->prefix,strlen(a->prefix)))binding=xml_bind(x,a->prefix,strlen(a->prefix),ns);
                else binding=xml_generated(x,ns);
                if(!binding||!xml_declaration(x,binding->prefix,binding->length,ns))return false;
            }
            prefix=binding->prefix;prefix_length=binding->length;
        }
        if(!xml_literal(x," ")||!xml_qname(x,prefix,prefix_length,local)||!xml_literal(x,"=\"")||
           !xml_escape(x,value,strlen(value),true)||!xml_literal(x,"\""))return false;
    }
    const node_t *children=n->namespace_id==NS_HTML&&n->tag==T_template?n->template_content:n;
    f->child=children?children->first:NULL;
    if(!f->child&&(serializes_as_void(n)||n->namespace_id!=NS_HTML)){
        f->close=false;return xml_literal(x,n->namespace_id==NS_HTML?" />":"/>");
    }
    f->close=true;return xml_literal(x,">");
}
static bool xml_data_node(struct xml_serialization *x,const node_t *n) {
    const char *text=n->text?n->text:"";size_t length=n->textlen;
    switch(n->type){
    case N_TEXT:
        if(n->cdata){
            /* A CDATASection keeps its section syntax (the round trip of the
               XML parser); well-formed output cannot hold "]]>" inside one. */
            if(x->strict){
                if(!xml_chars(text,length))return xml_invalid(x);
                for(size_t i=2;i<length;i++)if(text[i-2]==']'&&text[i-1]==']'&&text[i]=='>')return xml_invalid(x);
            }
            return xml_literal(x,"<![CDATA[")&&xml_write(x,text,length)&&xml_literal(x,"]]>");
        }
        return xml_escape(x,text,length,false);
    case N_ATTR:return true;
    case N_COMMENT:
        if(x->strict&&(!xml_chars(text,length)||(length&&text[length-1]=='-')))return xml_invalid(x);
        if(x->strict)for(size_t i=1;i<length;i++)if(text[i-1]=='-'&&text[i]=='-')return xml_invalid(x);
        return xml_literal(x,"<!--")&&xml_write(x,text,length)&&xml_literal(x,"-->");
    case N_PI:
        if(x->strict&&(!xml_name(x,n->name)||str_ieq(n->name,"xml")||!xml_chars(text,length)))return xml_invalid(x);
        if(x->strict)for(size_t i=1;i<length;i++)if(text[i-1]=='?'&&text[i]=='>')return xml_invalid(x);
        return xml_literal(x,"<?")&&xml_literal(x,n->name?n->name:"")&&xml_literal(x," ")&&xml_write(x,text,length)&&xml_literal(x,"?>");
    case N_DOCTYPE:{
        const char *pub=n->public_id?n->public_id:"",*sys=n->system_id?n->system_id:"";
        size_t pl=n->doctype_identifiers_sized?n->public_id_len:strlen(pub),sl=n->doctype_identifiers_sized?n->system_id_len:strlen(sys);
        if(x->strict){
            if(!xml_chars(sys,sl)||(memchr(sys,'"',sl)&&memchr(sys,'\'',sl)))return xml_invalid(x);
            for(size_t i=0;i<pl;i++)if(!(pub[i]==' '||pub[i]=='\r'||pub[i]=='\n'||
                (pub[i]>='a'&&pub[i]<='z')||(pub[i]>='A'&&pub[i]<='Z')||(pub[i]>='0'&&pub[i]<='9')||
                (pub[i]&&strchr("-'()+,./:=?;!*#@$_%",pub[i]))))return xml_invalid(x);
        }
        if(!xml_literal(x,"<!DOCTYPE ")||!xml_literal(x,n->name?n->name:""))return false;
        if(pl){char q=memchr(pub,'"',pl)?'\'':'"';if(!xml_literal(x," PUBLIC ")||!xml_write(x,&q,1)||!xml_write(x,pub,pl)||!xml_write(x,&q,1))return false;}
        if(sl){char q=memchr(sys,'"',sl)?'\'':'"';if(!xml_literal(x,pl?" ":" SYSTEM ")||!xml_write(x,&q,1)||!xml_write(x,sys,sl)||!xml_write(x,&q,1))return false;}
        return xml_literal(x,">");
    }
    default:return xml_invalid(x);
    }
}
enum xml_serialization_result xml_serialize_node(const node_t *node,sbuf *out,bool require_well_formed) {
    size_t initial=out->n;
    struct xml_binding base={.prefix="xml",.length=3,.uri=XML_NS};
    /* Inert and template trees have no JS host of their own. Their native DOM
       family owns cancellation/input servicing, without changing node owners. */
    web_doc *owner=node?node->owner:NULL,*checkpoint=owner&&owner->dom_family?owner->dom_family:owner;
    struct xml_serialization x={.out=out,.document=checkpoint,.bindings=&base,.prefix_index=1,.strict=require_well_formed};
    struct xml_frame small[16],*frames=small;size_t length=0,capacity=16;
    if(!node){x.result=XML_SERIALIZE_INVALID;goto done;}
    frames[length++]=(struct xml_frame){.node=node,.mark=&base};
    while(length&&x.result==XML_SERIALIZED){
        if((x.steps++&127)==0&&!xml_checkpoint(&x))break;
        struct xml_frame *f=&frames[length-1];
        if(!f->entered){
            f->entered=true;
            if(f->node->type==N_ELEM){if(!xml_element(&x,f))break;}
            else if(f->node->type==N_DOC||f->node->type==N_FRAGMENT){
                if(x.strict&&f->node->type==N_DOC){bool element=false;for(const node_t *c=f->node->first;c;c=c->next)element|=c->type==N_ELEM;if(!element){xml_invalid(&x);break;}}
                f->child=f->node->first;
            }else if(!xml_data_node(&x,f->node))break;
        }
        if(f->child){
            const node_t *child=f->child;f->child=child->next;const char *inherited=f->inherited;
            if(length==capacity){
                if(capacity>SIZE_MAX/2/sizeof *frames){x.result=XML_SERIALIZE_OOM;break;}
                size_t grown=capacity*2;struct xml_frame *replacement=malloc(grown*sizeof *frames);
                if(!replacement){x.result=XML_SERIALIZE_OOM;break;}
                memcpy(replacement,frames,length*sizeof *frames);if(frames!=small)free(frames);frames=replacement;capacity=grown;
            }
            frames[length++]=(struct xml_frame){.node=child,.mark=x.bindings,.inherited=inherited};continue;
        }
        if(f->close&&(!xml_literal(&x,"</")||!xml_qname(&x,f->prefix,f->prefix_length,f->local)||!xml_literal(&x,">")))break;
        while(x.bindings!=f->mark){struct xml_binding *b=x.bindings;x.bindings=b->next;free(b);}length--;
    }
done:
    while(x.bindings!=&base){struct xml_binding *b=x.bindings;x.bindings=b->next;free(b);}
    if(frames!=small)free(frames);
    if(x.result!=XML_SERIALIZED){out->n=initial;if(out->p)out->p[initial]=0;}
    return x.result;
}
