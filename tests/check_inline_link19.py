"""Only the new one-walk native inline-link boundary, executed once."""
from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
def read(n):return (root/n).read_text(encoding='utf-8')
def fn(text,sig):
    at=text.index(sig);return text[at:text.index('\n}',at)+2]+'\n'
layout=read('user/libc/web/layout.c');dom=read('user/libc/web/dom.c');html=read('user/libc/web/html.c')
pieces=fn(html,'const char *node_attr(')
pieces+=''.join(fn(dom,n) for n in ('static node_t *tree_next(', 'node_t *doc_shadow_parent(', 'node_t *doc_node_root(', 'static bool is_slot('))
pieces+='static bool slottable(const node_t *n){return n && (n->type==N_ELEM || n->type==N_TEXT);}\n'
pieces+=''.join(fn(dom,n) for n in ('static const char *slot_name(', 'static node_t *first_named_slot(', 'node_t *doc_assigned_slot(', 'node_t *doc_flat_parent('))
pieces+='static unsigned parent_walks;\nstatic node_t *measured_flat_parent(node_t *n){parent_walks++;return doc_flat_parent(n);}\n'
pieces+=fn(layout,'static node_t *inline_link_ancestor(').replace('doc_flat_parent(n)','measured_flat_parent(n)')
assert 'bs.link=inline_link_ancestor(b);' in layout
c=out/'inline-link19-generated.c'
c.write_text('#include <stdio.h>\n#include "webi.h"\n'+pieces+read('tests/inline_link19_cases.c'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'inline-link19.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
