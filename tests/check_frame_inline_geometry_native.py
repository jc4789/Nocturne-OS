"""One new exact-C geometry/frameset boundary run; no former suite or site run."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/goal-20261009'
def read(name):
    return (root / name).read_text(encoding='utf-8')
def function(text, name):
    start = text.index(name)
    return text[start:text.index('\n}', start)+2]+'\n'
layout, doc, paint, dom, util, html = (read('user/libc/web/'+name+'.c') for name in ('layout','doc','paint','dom','util','html'))
pieces = function(html, 'const char *node_attr(')
pieces += ''.join(function(dom, sig) for sig in ('node_t *doc_shadow_parent(', 'node_t *doc_node_root('))
pieces += ''.join(function(layout, sig) for sig in ('float box_abs_x(', 'float box_abs_y(', 'bool box_element_scrollable(', 'float box_visual_x(', 'float box_visual_y('))
pieces += function(layout, 'static void add_deco(')
pieces += function(doc, 'static bool scroll_target_rect(') + function(doc, 'bool web_node_rect(')
pieces += function(paint, 'node_t *doc_element_at(')
pieces += layout[layout.index('struct frameset_track {'):layout.index('static void layout_inner(', layout.index('struct frameset_track {'))]
arena = util[util.index('struct achunk {'):util.index('char *ar_strndup(')] + function(util, 'void ar_free(')
prefix = r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
#include "frame.h"
static web_doc *D;
static bool fail_malloc;
static void *test_malloc(size_t n){return fail_malloc?NULL:malloc(n);}
#define malloc test_malloc
'''
bridge = r'''
#undef malloc
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){return b->node==d->html;}
static void web_dialog_scroll_offset(web_doc *d,node_t *n,int *x,int *y){(void)d;(void)n;(void)x;(void)y;}
static node_t *query_hit;
web_node *web_node_at(web_doc *d,int x,int y){(void)d;(void)x;(void)y;return query_hit;}
node_t *doc_flat_parent(node_t *n){return n?n->parent:NULL;}
struct web_frame *web_frame_find(web_doc *d,node_t *n){for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->element==n)return f;return NULL;}
/* Isolate font rasterization: the product line-fragment calculation uses these
   deterministic ascent/descent inputs, not a mock rectangle result. */
wfont style_font(const style_t *st){return (wfont){NULL,st->font_size,false};}
void wf_metrics(const wfont *f,float *a,float *d){(void)f;*a=10;*d=4;}
void sb_put(sbuf *b,const char *s,size_t n){void *p=realloc(b->p,b->n+n);if(!p)abort();b->p=p;memcpy(b->p+b->n,s,n);b->n+=n;}
struct iline {sbuf decos;};
'''
c = out / 'frame-inline-geometry-native-generated.c'
c.write_text(prefix+arena+bridge+pieces+read('tests/frame_inline_geometry_native_cases.c'), encoding='utf-8')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
abi = out / 'frame-inline-geometry-host-abi'
abi.mkdir(exist_ok=True)
(abi / 'setjmp.h').write_text('#include "'+(cc.parent.parent/'include/setjmp.h').as_posix()+'"\n',encoding='utf-8')
exe = out / 'frame-inline-geometry-native.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-I'+str(abi),'-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)], cwd=root,env=env,check=True)
if os.environ.get('NOCTURNE_COMPILE_ONLY') != '1':
    subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
