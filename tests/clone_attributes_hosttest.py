"""Bounded host checks of exact production clone-attribute/arena/cache helpers.
Not a substitute for native QuickJS or real Reddit acceptance.
"""
from pathlib import Path
import subprocess
import sys

source = Path(sys.argv[1] if len(sys.argv) > 1 else 'user/libc/web/dom.c').read_text(encoding='utf-8')
util = Path('user/libc/web/util.c').read_text(encoding='utf-8')
def part(text, begin, end):
    return text[text.index(begin):text.index(end, text.index(begin))]
pieces = part(util, 'struct achunk {', '/* ---------------------------------------------------------------- buffers */')
pieces += part(source, 'static void attribute_cache', '/* The element array remains')
pieces += part(source, 'void doc_attrs_publish', 'int doc_attr_index')
pieces += part(source, 'static bool clone_attributes', 'node_t *doc_node_clone')
out = Path('build/goal-20261009/clone-attributes-host.c')
prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
typedef struct arena { struct achunk *head; size_t allocated,limit; jmp_buf *trap; } arena_t;
typedef struct web_doc { arena_t mem; } web_doc;
struct node;
struct attr { const char *name,*raw,*value,*namespace_uri,*prefix,*local;struct node *node; };
typedef struct node { struct attr *attrs,*attribute; int nattrs,nclasses,tag;bool foreign;const char *id,**classes;struct node *attr_owner;web_doc *owner; } node_t;
enum { T_input=1,T_select,T_option,T_details,T_img,T_source,T_div,T_template };
static bool is_space(unsigned char c){return c==' '||c=='\n'||c=='\t'||c=='\r'||c=='\f';}
static unsigned setter_calls;
static void doc_dom_budget(web_doc *d){(void)d;}
static bool doc_attr_set_ns(web_doc *d,node_t *n,const char *ns,const char *prefix,const char *local,const char *v){(void)d;(void)n;(void)ns;(void)prefix;(void)local;(void)v;setter_calls++;return true;}
'''
suffix = r'''
static unsigned total,failed;
static void check(bool ok,const char *name){total++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",name);}}
static size_t used(arena_t *a){size_t n=0;for(struct achunk *c=a->head;c;c=c->next)n+=c->used;return n;}
static size_t chunks(arena_t *a){size_t n=0;for(struct achunk *c=a->head;c;c=c->next)n++;return n;}
static const char *safe(const char *p){return p?p:"";}
int main(void){
    web_doc owner={0},dest={0};struct attr attrs[64]={0};node_t identities[64]={0};
    owner.mem.limit=dest.mem.limit=32u<<20;
    for(unsigned i=0;i<64;i++){
        char name[32],value[32];snprintf(name,sizeof name,"data-case-%u",i);snprintf(value,sizeof value,"value-%u",i);
        attrs[i]=(struct attr){.name=ar_strdup(&owner.mem,name),.raw=ar_strdup(&owner.mem,name),.value=ar_strdup(&owner.mem,value),.local=ar_strdup(&owner.mem,name),.node=&identities[i]};
    }
    attrs[0].name=attrs[0].raw=attrs[0].local=ar_strdup(&owner.mem,"id");attrs[0].value=ar_strdup(&owner.mem,"cloned-id");
    attrs[1].name=attrs[1].raw=attrs[1].local=ar_strdup(&owner.mem,"class");attrs[1].value=ar_strdup(&owner.mem,"alpha\tBeta  alpha");
    attrs[2].raw=ar_strdup(&owner.mem,"P:MiXeD");attrs[2].name=ar_strdup(&owner.mem,"p:mixed");attrs[2].local=ar_strdup(&owner.mem,"MiXeD");attrs[2].prefix=ar_strdup(&owner.mem,"P");attrs[2].namespace_uri=ar_strdup(&owner.mem,"urn:One");
    attrs[3].raw=ar_strdup(&owner.mem,"P:MiXeD");attrs[3].name=ar_strdup(&owner.mem,"p:mixed");attrs[3].local=ar_strdup(&owner.mem,"MiXeD");attrs[3].prefix=ar_strdup(&owner.mem,"P");attrs[3].namespace_uri=ar_strdup(&owner.mem,"urn:Two");
    attrs[4].name=attrs[4].raw=attrs[4].local=ar_strdup(&owner.mem,"hidden");attrs[4].value=ar_strdup(&owner.mem,"");
    node_t source={.attrs=attrs,.nattrs=64,.tag=T_div,.owner=&owner},copy={.owner=&dest};
    jmp_buf outer;dest.mem.trap=&outer;
    check(clone_attributes(&dest,&copy,&source),"64 attributes cloned");check(setter_calls==0,"ordinary clone bypasses setter loop");check(dest.mem.trap==&outer,"outer trap restored");check(copy.owner==&dest,"owner preserved");
    check(copy.nattrs==64&&copy.attrs!=attrs,"new single canonical array");
    for(unsigned i=0;i<64;i++){
        const char *a[]={attrs[i].name,attrs[i].raw,attrs[i].value,attrs[i].namespace_uri,attrs[i].prefix,attrs[i].local};
        const char *b[]={copy.attrs[i].name,copy.attrs[i].raw,copy.attrs[i].value,copy.attrs[i].namespace_uri,copy.attrs[i].prefix,copy.attrs[i].local};
        for(unsigned j=0;j<6;j++){check(!strcmp(safe(a[j]),safe(b[j]))&&(!a[j])==(!b[j]),"metadata/order/null preserved");if(a[j])check(a[j]!=b[j],"metadata owned by destination");}
        check(copy.attrs[i].node==NULL,"source Attr identity never reused");
    }
    check(copy.id&&strcmp(copy.id,"cloned-id")==0&&copy.id==copy.attrs[0].value,"id cache owns cloned value");
    check(copy.nclasses==3&&!strcmp(copy.classes[0],"alpha")&&!strcmp(copy.classes[1],"Beta")&&!strcmp(copy.classes[2],"alpha"),"class tokens/case/duplicates preserved");
    check(!strcmp(copy.attrs[4].value,"")&&copy.attrs[4].value!=attrs[4].value,"empty boolean value copied");
    size_t bytes=used(&dest.mem);check(bytes<64*sizeof(struct attr)+8192,"linear bounded attribute allocation");check(chunks(&dest.mem)==1,"single arena chunk sufficient");
    ar_free(&owner.mem);check(!strcmp(copy.attrs[2].namespace_uri,"urn:One")&&!strcmp(copy.attrs[3].namespace_uri,"urn:Two")&&!strcmp(copy.id,"cloned-id")&&!strcmp(copy.classes[1],"Beta"),"source arena free cannot invalidate copy");
    for(int tag=T_input;tag<=T_source;tag++){node_t special=source,sp={0};special.attrs=copy.attrs;special.tag=tag;unsigned before=setter_calls;check(clone_attributes(&dest,&sp,&special)&&setter_calls==before+64,"special tag uses existing setter path");}
    node_t foreign=source,fc={0};foreign.attrs=copy.attrs;foreign.foreign=true;foreign.tag=T_input;unsigned before=setter_calls;check(clone_attributes(&dest,&fc,&foreign)&&setter_calls==before&&fc.nattrs==64,"foreign tag uses namespace preserving batch");
    node_t empty={0},ec={0};check(clone_attributes(&dest,&ec,&empty)&&!ec.attrs,"empty clone no allocation");
    node_t too_many=source;too_many.nattrs=1025;check(!clone_attributes(&dest,&ec,&too_many),"attribute cap retained");
    web_doc quota={0};quota.mem.limit=32;quota.mem.trap=&outer;node_t qc={0};source.attrs=copy.attrs;check(!clone_attributes(&quota,&qc,&source)&&quota.mem.trap==&outer&&!qc.attrs&&qc.nattrs==0,"quota failure unpublished and trap restored");
    ar_free(&quota.mem);ar_free(&dest.mem);
    printf("clone attributes host %u checks / %u failed; 64 attrs use %zu arena bytes\n",total,failed,bytes);return failed?1:0;
}
'''
out.write_text(prefix + pieces + suffix, encoding='utf-8', newline='\n')
clang = 'tools/msys64/ucrt64/bin/clang.exe'
exe = 'build/goal-20261009/clone-attributes-host.exe'
subprocess.run([clang,'-O2',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True)
