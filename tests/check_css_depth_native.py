"""New depth/cycle boundaries only; one execution, not website acceptance."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
def read(name):
    return (root / name).read_text(encoding="utf-8")
def function(text, signature):
    at = text.index(signature)
    return text[at:text.index("\n}", at) + 2] + "\n"
doc, box, layout, dom = (read("user/libc/web/" + name + ".c") for name in ("doc", "box", "layout", "dom"))
defs = doc[doc.index("struct pending {"):doc.index("/* ---------------------------------------------------------------- data: URLs */")]
defs += doc[doc.index("struct sheet_walk {"):doc.index("static bool sheet_url_equal(")]
helpers = "".join(function(doc, name) for name in (
    "static bool sheet_url_equal(", "static bool sheet_ancestor_url(", "static bool sheet_push(",
    "static void sheet_walk_free(", "static bool sheet_parse(", "static void add_sheet("))
helpers += function(dom, "node_t *doc_shadow_parent(") + function(dom, "node_t *doc_node_root(")
helpers += function(box, "bool box_block_level(")
helpers += function(doc, "bool doc_viewport_overflow_box(")
helpers += "static web_doc *D;\n"
helpers += "".join(function(layout, name) for name in (
    "static bool is_flex_item(", "static bool is_bfc_root(", "static bool collapses_top(",
    "static box_t *first_inflow(", "static float top_chain("))
svg = box[box.index("static node_t *find_id("):box.index("struct svg_cache *doc_svg(")]
prefix = '''#include <stdio.h>
#include <limits.h>
#include "webi.h"
#include "svg_geometry.h"
static int reports, fail_calloc;
void web_js_console(web_doc *d,int level,const char *message){(void)d;(void)level;(void)message;reports++;}
int doc_attr_index(node_t *n,const char *ns,const char *local,bool namespaced){
    (void)namespaced;
    for(int i=0;i<n->nattrs;i++) if(n->attrs[i].namespace_uri && !strcmp(n->attrs[i].namespace_uri,ns) &&
       n->attrs[i].local && !strcmp(n->attrs[i].local,local))return i;
    return -1;
}
static void *walk_calloc(size_t n,size_t size){if(fail_calloc && !--fail_calloc)return NULL;return calloc(n,size);}
struct sheet { struct rule *first,*last; double order; bool ua; node_t *scope; };
#define calloc walk_calloc
'''
c = out / "css-depth-native-generated.c"
cases = read("tests/css_depth_native_cases.c")
if os.environ.get("NOCTURNE_NEW_ORDER_ONLY") == "1":
    cases = cases[:cases.index("int main(void)")] + '''
int main(void){
    web_doc d={.live=true};
    const char*prior=".prior{width:2px}";
    add_sheet(&d,prior,strlen(prior),"https://order.test/prior.css",0,NULL,NULL);
    struct cached_css*a=cache(&d,"https://order.test/a.css",NULL,"@import \\"https://order.test/b.css\\";@import \\"https://order.test/c.css\\";");
    cache(&d,"https://order.test/b.css",NULL,"@import \\"https://order.test/d.css\\";");
    struct cached_css*c=cache(&d,"https://order.test/c.css",NULL,".c{width:3px}");
    struct cached_css*leaf=cache(&d,"https://order.test/d.css",NULL,".d{width:4px}");
    add_sheet(&d,a->body,a->n,a->base,1,NULL,NULL);
    check(d.sty.sheets.n==5,"new postorder batch publishes all five sheets");
    check(((sheet_t*)d.sty.sheets.v[0])->order==0,"previous root stylesheet key retained");
    bool same=true;for(int i=1;i<d.sty.sheets.n;i++)if(((sheet_t*)d.sty.sheets.v[i])->order!=1)same=false;
    check(same,"new import batch uses root key instead of shrinking fractions");
    sheet_t*leaf_sheet=NULL,*c_sheet=NULL;
    for(struct css_sheet_reuse*r=d.css_reuse;r;r=r->next){if(!strcmp(r->css,leaf->body))leaf_sheet=r->sheet;if(!strcmp(r->css,c->body))c_sheet=r->sheet;}
    check(d.sty.sheets.v[1]==leaf_sheet,"nested import precedes its parent");
    check(d.sty.sheets.v[3]==c_sheet,"second sibling import follows complete first subtree");
    check(reports==0 && !d.cssmem.trap,"new batch completes without error and restores trap");
    clear_doc(&d);printf("css-new-order-native: %d checks, %d failures\\\\n",checks,failures);return failures!=0;
}
'''
c.write_text(prefix + defs + helpers + svg + "\n#undef calloc\n" + cases, encoding="utf-8")
parser = read("user/libc/web/css.c")
parser = parser[:parser.index("/* ---------------------------------------------------------------- matching */")] + parser[parser.index("/* ---------------------------------------------------------------- feature queries"):parser.index("/* ---------------------------------------------------------------- media queries */")]
parser_file = out / "css-depth-parser-extracted.c"
parser_file.write_text(parser, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "css-depth-native.exe"
# Host execution must use the host CRT's jmp_buf ABI in every translation unit.
# The first recorded execution used Nocturne's native ABI and remains a failed,
# inconclusive run; correcting this harness does not authorize a rerun.
host_abi = out / "css-depth-host-abi"
host_abi.mkdir(exist_ok=True)
(host_abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
command = [str(cc), "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections",
           "-Wl,--gc-sections", "-I" + str(host_abi), "-Iuser/include", "-Icommon", "-Iuser/libc/web",
           str(c), str(parser_file), "user/libc/web/cssprop.c", "user/libc/web/util.c",
           "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
if os.environ.get("NOCTURNE_COMPILE_ONLY") != "1":
    subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
