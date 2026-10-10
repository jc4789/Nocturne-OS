/* Official .dat expected trees compared with the real native product DOM.
 * This does not validate parse-error counts or execute author scripts, and
 * does not substitute for visible real-site/scrolling acceptance. */
#include <stdio.h>
#include "webi.h"
#include "html5lib_cases.h"

struct utf16_cursor { const unsigned char *p; unsigned pending; };
static unsigned utf16_unit(struct utf16_cursor *cursor) {
    if(cursor->pending){unsigned result=cursor->pending;cursor->pending=0;return result;}
    unsigned c=*cursor->p++;if(!c)return 0;
    if(c>=0xc2&&c<=0xdf){c=(c&31)<<6;c|=*cursor->p++&63;}
    else if(c>=0xe0&&c<=0xef){c=(c&15)<<12;c|=(*cursor->p++&63)<<6;c|=*cursor->p++&63;}
    else if(c>=0xf0&&c<=0xf4){c=(c&7)<<18;c|=(*cursor->p++&63)<<12;c|=(*cursor->p++&63)<<6;c|=*cursor->p++&63;}
    if(c>0xffff){c-=0x10000;cursor->pending=0xdc00+(c&1023);return 0xd800+(c>>10);}
    return c;
}
static int utf16_compare(const char *a,const char *b) {
    struct utf16_cursor x={(const unsigned char *)a,0},y={(const unsigned char *)b,0};
    for(;;){unsigned first=utf16_unit(&x),second=utf16_unit(&y);if(first!=second)return first<second?-1:1;if(!first)return 0;}
}
static void indentation(sbuf *out,unsigned depth) {
    sb_puts(out,"| ");while(depth--)sb_puts(out,"  ");
}
static const char *element_namespace(const node_t *node) {
    return node->namespace_id==NS_HTML?"":node->namespace_id==NS_SVG?"svg ":node->namespace_id==NS_MATHML?"math ":NULL;
}
static const char *attribute_namespace(const struct attr *attr) {
    const char *uri=attr->namespace_uri;
    if(!uri||!*uri)return "";
    if(!strcmp(uri,"http://www.w3.org/1999/xlink"))return "xlink ";
    if(!strcmp(uri,"http://www.w3.org/XML/1998/namespace"))return "xml ";
    if(!strcmp(uri,"http://www.w3.org/2000/xmlns/"))return "xmlns ";
    return NULL;
}
struct dump_attribute { const struct attr *attr; char *name; };
static bool dump_attributes(sbuf *out,const node_t *node,unsigned depth) {
    if(!node->nattrs)return true;
    if(node->nattrs<0||(size_t)node->nattrs>SIZE_MAX/sizeof(struct dump_attribute))return false;
    struct dump_attribute *attrs=calloc((size_t)node->nattrs,sizeof *attrs);if(!attrs)return false;
    bool ok=true;int prepared=0;
    for(int i=0;i<node->nattrs;i++) {
        const struct attr *attr=&node->attrs[i];const char *prefix=attribute_namespace(attr);
        if(!prefix){ok=false;break;}
        sbuf name={0};sb_puts(&name,prefix);sb_puts(&name,attr->local?attr->local:attr->raw);
        attrs[i]=(struct dump_attribute){attr,strdup(sb_cstr(&name))};sb_free(&name);
        if(!attrs[i].name){ok=false;break;}prepared++;
    }
    if(ok)for(int i=1;i<node->nattrs;i++) {
        struct dump_attribute value=attrs[i];int j=i;
        while(j&&utf16_compare(attrs[j-1].name,value.name)>0){attrs[j]=attrs[j-1];j--;}
        attrs[j]=value;
    }
    if(ok)for(int i=0;i<node->nattrs;i++) {
        indentation(out,depth);sb_puts(out,attrs[i].name);sb_puts(out,"=\"");
        sb_puts(out,attrs[i].attr->value);sb_puts(out,"\"\n");
    }
    for(int i=0;i<prepared;i++)free(attrs[i].name);free(attrs);return ok;
}
static bool dump_children(sbuf *,const node_t *,unsigned,size_t *);
static bool dump_node(sbuf *out,const node_t *node,unsigned depth,size_t *remaining) {
    if(depth>512||!*remaining)return false;(*remaining)--;
    indentation(out,depth);
    switch(node->type) {
    case N_ELEM: {
        const char *prefix=element_namespace(node);if(!prefix)return false;
        sb_putc(out,'<');sb_puts(out,prefix);sb_puts(out,node->foreign?node->raw_name:node->name);sb_puts(out,">\n");
        if(!dump_attributes(out,node,depth+1)||!dump_children(out,node,depth+1,remaining))return false;
        if(node->template_content) {
            indentation(out,depth+1);sb_puts(out,"content\n");
            if(!dump_children(out,node->template_content,depth+2,remaining))return false;
        }
        return true;
    }
    case N_TEXT:sb_putc(out,'"');sb_put(out,node->text?node->text:"",node->textlen);sb_puts(out,"\"\n");return true;
    case N_COMMENT:sb_puts(out,"<!-- ");sb_put(out,node->text?node->text:"",node->textlen);sb_puts(out," -->\n");return true;
    case N_PI:sb_puts(out,"<?");sb_puts(out,node->name);sb_putc(out,' ');sb_put(out,node->text?node->text:"",node->textlen);sb_puts(out,"?>\n");return true;
    case N_DOCTYPE: {
        size_t public_length=node->doctype_identifiers_sized?node->public_id_len:node->public_id?strlen(node->public_id):0;
        size_t system_length=node->doctype_identifiers_sized?node->system_id_len:node->system_id?strlen(node->system_id):0;
        sb_puts(out,"<!DOCTYPE ");sb_puts(out,node->name?node->name:"");
        if(public_length||system_length) {
            sb_puts(out," \"");sb_put(out,node->public_id?node->public_id:"",public_length);
            sb_puts(out,"\" \"");sb_put(out,node->system_id?node->system_id:"",system_length);sb_putc(out,'"');
        }
        sb_puts(out,">\n");return true;
    }
    default:return false;
    }
}
static bool dump_children(sbuf *out,const node_t *parent,unsigned depth,size_t *remaining) {
    for(const node_t *node=parent?parent->first:NULL;node;node=node->next)
        if(!dump_node(out,node,depth,remaining))return false;
    return true;
}
static web_doc *new_family(void) {
    web_doc *d=calloc(1,sizeof *d);if(!d)return NULL;
    d->url=strdup("http://html5lib.test/corpus");if(!d->url){free(d);return NULL;}
    snprintf(d->base,sizeof d->base,"%s",d->url);
    const char *initial="<!doctype html><html><head></head><body></body>";
    d->root=html_parse(d,initial,strlen(initial),"utf-8");
    if(!d->root){web_free(d);return NULL;}return d;
}
static node_t *parse_case(web_doc *family,const struct html5lib_case *test) {
    if(test->context[0]) {
        family->live=test->scripting;
        node_t *context=doc_node_create(family,N_ELEM,test->context,NULL,0);if(!context)return NULL;
        context->namespace_id=test->namespace_id==1?NS_SVG:test->namespace_id==2?NS_MATHML:NS_HTML;
        context->foreign=context->namespace_id!=NS_HTML;
        if(context->foreign){context->name=ar_strdup(&family->mem,test->context);context->raw_name=context->name;}
        return html_fragment(family,context,test->data,test->data_length);
    }
    if(!test->scripting) {
        web_doc *document=doc_inert(family,test->data,test->data_length,family->url);
        return document?document->root:NULL;
    }
    /* An inert native document lets the tree-construction scripting flag be
       true without turning a corpus into author script execution tests. */
    family->inert=true;
    struct html_parser *parser=html_begin_string(family,test->data,test->data_length,true,false);
    if(!parser)return NULL;
    node_t *ignored=NULL;int result=html_resume(parser,&ignored);html_finish(parser);
    return result==0?family->root:NULL;
}
int main(int argc,char **argv) {
    size_t passed=0,failed=0,skipped=0,selected=0;
    for(size_t i=0;i<sizeof html5lib_cases/sizeof *html5lib_cases;i++) {
        const struct html5lib_case *test=&html5lib_cases[i];
        if(argc>1&&!strstr(test->id,argv[1]))continue;selected++;
        if(test->skip[0]){printf("SKIP html5lib %s: %s\n",test->id,test->skip);skipped++;continue;}
        web_doc *family=new_family();node_t *root=family?parse_case(family,test):NULL;
        sbuf actual={0};size_t remaining=100000;
        bool ok=root&&dump_children(&actual,root,0,&remaining)&&actual.n==test->expected_length&&
                (!actual.n||!memcmp(actual.p,test->expected,actual.n));
        if(ok)passed++;
        else {
            failed++;size_t difference=0;
            while(difference<actual.n&&difference<test->expected_length&&actual.p[difference]==test->expected[difference])difference++;
            printf("FAIL html5lib %s at byte %zu root=%d: expected %zu, actual %zu bytes\nexpected: %.1200s\nactual: %.1200s\n",
                test->id,difference,root!=NULL,test->expected_length,actual.n,test->expected,actual.p?actual.p:"");
        }
        sb_free(&actual);web_free(family);
    }
    if(!selected){printf("FAIL html5lib: no cases selected\n");failed++;}
    printf("html5lib: %zu selected, %zu passed, %zu failed, %zu explicit skipped; parse errors unchecked\n",selected,passed,failed,skipped);
    return failed!=0;
}
