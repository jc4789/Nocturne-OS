"""First-only native stylesheet rule mutation boundaries; no old suite replay."""
from pathlib import Path
import os
import subprocess
import hashlib

root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/cssom-new'
out.mkdir(exist_ok=True)
def read(name): return (root/name).read_text(encoding='utf-8')
def function(text,name):
    at=text.index(name); return text[at:text.index('\n}',at)+2]+'\n'
css=read('user/libc/web/css.c')
parser=css[:css.index('/* ---------------------------------------------------------------- matching */')]+css[css.index('/* ---------------------------------------------------------------- feature queries'):css.index('/* ---------------------------------------------------------------- media queries */')]
doc=read('user/libc/web/doc.c')
defs=doc[doc.index('struct pending {'):doc.index('/* ---------------------------------------------------------------- data: URLs */')]
defs+=doc[doc.index('struct sheet_walk {'):doc.index('static bool sheet_url_equal(')]
helpers=''.join(function(doc,s) for s in ('static bool sheet_url_equal(','static bool sheet_ancestor_url(','static bool sheet_push(','static void sheet_walk_free(','static bool sheet_parse(','static void add_sheet_ast('))
prefix='''#include <stdio.h>
#include <limits.h>
#include "cssom.h"
static unsigned reports,fail_calloc,fail_realloc;
static void *fault_calloc(size_t n,size_t k){if(fail_calloc && !--fail_calloc)return NULL;return calloc(n,k);}
static void *fault_realloc(void*p,size_t n){if(fail_realloc && !--fail_realloc)return NULL;return realloc(p,n);}
void web_js_console(web_doc*d,int level,const char*text){(void)d;(void)level;(void)text;reports++;}
bool web_js_enabled(web_doc*d){return d&&d->live;}
node_t *doc_node_root(node_t*n,bool composed){while(n&&(n->parent||(composed&&n->shadow_host)))n=n->parent?n->parent:n->shadow_host;return n;}
void css_styling_free(struct styling*st){pv_free(&st->sheets);st->ctx=NULL;}
'''
tail=read('tests/cssom_native_new_cases.c')
native=out/'native.c'
native.write_text(prefix+parser+defs+helpers+'\n#define calloc fault_calloc\n#define realloc fault_realloc\n'+read('user/libc/web/cssom.c')+'\n#undef calloc\n#undef realloc\n'+tail,encoding='utf-8',newline='\n')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
abi=out/'host-abi';abi.mkdir(exist_ok=True)
(abi/'setjmp.h').write_text('#include "'+(cc.parent.parent/'include/setjmp.h').as_posix()+'"\n',encoding='utf-8')
for path in ['user/libc/web/css.c','user/libc/web/cssom.c','user/libc/web/doc.c','user/libc/web/dom.c','user/libc/web/js_cssom.h','user/libc/web/js_stylesheets.js',str(native.relative_to(root))]:
    print(path,'SHA256',hashlib.sha256((root/path).read_bytes()).hexdigest(),flush=True)
exe=out/'native.exe'
command=[str(cc),'-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
         '-I'+str(abi),'-Iuser/include','-Icommon','-Iuser/libc/web',str(native),
         'user/libc/web/cssprop.c','user/libc/web/util.c','user/libc/web/html.c','-o',str(exe)]
subprocess.run(command,cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
