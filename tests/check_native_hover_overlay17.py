"""Only new native hover/occlusion boundaries, one run; not real-site acceptance."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
def read(name):return (root/name).read_text(encoding='utf-8')
def fn(text,sig):
    p=text.index(sig);return text[p:text.index('\n}',p)+2]+'\n'
doc,css,paint,dom,frame,html=(read('user/libc/web/'+n+'.c') for n in ('doc','css','paint','dom','frame','html'))
pieces=fn(html,'const char *node_attr(')
pieces+=''.join(fn(dom,n) for n in ('static node_t *tree_next(', 'node_t *doc_shadow_parent(', 'node_t *doc_node_root(', 'static bool is_slot('))
pieces+='static bool slottable(const node_t *n){return n && (n->type==N_ELEM || n->type==N_TEXT);}\n'
pieces+=''.join(fn(dom,n) for n in ('static const char *slot_name(', 'static node_t *first_named_slot(', 'node_t *doc_assigned_slot(', 'node_t *doc_flat_parent('))
pieces+=fn(frame,'struct web_frame *web_frame_find(')
pieces+=fn(doc,'static void hover_target_set(')+fn(doc,'void doc_hover_update(')+fn(css,'static bool css_hovered(')
at=css.index('enum { PC_FIRST_CHILD');pieces+=css[at:css.index('};',at)+2]+'\n'
at=css.index('static const struct {\n    const char *name;');pieces+=css[at:css.index('};',at)+2]+'\n'
at=paint.index('struct pctx {');structure=paint[at:paint.index('\n};',at)+3]+'\n'
pieces+=''.join(fn(paint,n) for n in ('static bool hit_style(', 'static void hit_target(', 'static void set_hit(', 'static void set_disclosure_hit(', 'static void finish_disclosure_hit(', 'bool web_node_action(', 'bool web_hit_test('))
prefix=r'''
#include <stdio.h>
#include "webi.h"
#include "frame.h"
enum { M_PAINT, M_HIT };
static node_t *inert_node;
bool web_dialog_inert(web_doc *d,node_t *n){(void)d;return n==inert_node;}
const char *doc_link_href(web_doc *d,node_t *n){(void)d;return node_attr(n,"href");}
static int control_hit(box_t *b){return b && b->node && b->node->tag==T_input?WEB_HIT_TEXT_INPUT:WEB_HIT_NONE;}
node_t *doc_details_summary(node_t *n){(void)n;return NULL;}
node_t *doc_details_activation(node_t *n){for(;n;n=n->parent)if(n->tag==T_summary)return n->parent;return NULL;}
static void walk(struct pctx *P);
'''
c=out/'native-hover-overlay17-generated.c'
# pctx must precede its private walk declaration.
prefix=prefix.replace('static void walk(struct pctx *P);','')+structure+'static void walk(struct pctx *P);\n'
c.write_text(prefix+pieces+read('tests/native_hover_overlay17_cases.c'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'native-hover-overlay17.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
