"""One new native primary-press/CSS boundary run, not real-site acceptance."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
def read(name):return (root/name).read_text(encoding='utf-8')
def fn(text,sig):
    p=text.index(sig);return text[p:text.index('\n}',p)+2]+'\n'
doc,css,dom,frame,html=(read('user/libc/web/'+n+'.c') for n in ('doc','css','dom','frame','html'))
browser=read('user/apps/browser.c')
pieces=fn(html,'const char *node_attr(')
pieces+=''.join(fn(dom,n) for n in ('static node_t *tree_next(', 'node_t *doc_shadow_parent(', 'node_t *doc_node_root(', 'static bool is_slot('))
pieces+='static bool slottable(const node_t *n){return n && (n->type==N_ELEM || n->type==N_TEXT);}\n'
pieces+=''.join(fn(dom,n) for n in ('static const char *slot_name(', 'static node_t *first_named_slot(', 'node_t *doc_assigned_slot(', 'node_t *doc_flat_parent('))
pieces+=''.join(fn(frame,n) for n in ('bool web_frame_element(', 'node_t *web_frame_dom_next(', 'struct web_frame *web_frame_find('))
pieces+=''.join(fn(doc,n) for n in ('static void active_target_set(', 'static bool active_target_valid(', 'void web_active_release(', 'void web_active_press(', 'void doc_active_cancel(', 'node_t *doc_active_target('))
pieces+=fn(css,'static bool css_active(')
pieces+=''.join(fn(frame,n) for n in ('void web_frame_retire(', 'void web_frames_detach_tree('))
pieces+=fn(browser,'static void native_active_event(')
at=css.index('enum { PC_FIRST_CHILD');pieces+=css[at:css.index('};',at)+2]+'\n'
at=css.index('static const struct {\n    const char *name;');pieces+=css[at:css.index('};',at)+2]+'\n'
# Validate the real browser integration statically; do not emulate author JS.
down=fn(browser,'static void mouse_down(')
assert down.index('web_active_press(doc,pressed_node)')<down.index('dispatch_native("mousedown"')
assert 'if(result>0)native_active_event(event);' in browser
assert 'if(!(e->buttons&1))web_active_release(doc);' in browser
assert 'doc_active_cancel(d);' in fn(doc,'void web_free(')
assert 'case PC_ACTIVE: return css_active(e);' in css
prefix=r'''
#include <stdio.h>
#include "webi.h"
#include "frame.h"
static web_doc *doc;
static node_t *inert_node;
bool web_dialog_inert(web_doc *d,node_t *n){(void)d;return n==inert_node;}
void web_js_retire(web_doc *d){(void)d;} /* Isolate native retirement before its realm work. */
'''
c=out/'native-active18-generated.c'
c.write_text(prefix+pieces+read('tests/native_active18_cases.c'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'native-active18.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
