"""New absolute definite-height propagation boundaries, executed once only."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
def read(n):return (root/n).read_text(encoding='utf-8')
def fn(text,sig):
    p=text.index(sig);return text[p:text.index('\n}',p)+2]+'\n'
layout=read('user/libc/web/layout.c');prop=read('user/libc/web/cssprop.c');box=read('user/libc/web/box.c')
at=prop.index('struct cexpr {');pieces=prop[at:prop.index('};',at)+2]+'\n'
pieces+=fn(prop,'static float ceval(')+'#define LEN_MAX 1e7f\n'
# one-line helper: no block-ending delimiter on its own line
at=prop.index('static float len_clamp(');pieces+=prop[at:prop.index('\n',at)+1]
pieces+=fn(prop,'float len_resolve(')
for sig in ('static float hext(', 'static float vext(', 'static float fmaxf_(', 'static float fminf_('):
    at=layout.index(sig);pieces+=layout[at:layout.index('\n',at)+1]
pieces+=''.join(fn(layout,n) for n in ('static void resolve_edges(', 'static float spec_w(', 'static float spec_h(', 'static float clamp_w(', 'static float clamp_h(', 'static float resolved_content_height(', 'static float mcollapse(', 'static bool is_flex_item(', 'static bool is_bfc_root(', 'float box_abs_x(', 'float box_abs_y('))
pieces+=fn(box,'bool box_block_level(')
pieces+=''.join(fn(layout,n) for n in ('static bool collapses_top(', 'static bool collapses_bottom(', 'static box_t *first_inflow(', 'static float top_chain(', 'static void block_width(', 'static void layout_blocks(', 'static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {', 'static box_t *abs_containing_block(', 'static void layout_abs('))
assert 'float defh = resolved_content_height(b, cbh);' in fn(layout,'static void layout_flex(')
assert 'float defh = resolved_content_height(b, cbh);' in fn(layout,'static void layout_grid(')
prefix=r'''
#include <stdio.h>
#include <math.h>
#include "webi.h"
static web_doc *D;
static float VW,VH;
static unsigned unexpected;
static node_t *modal;
bool web_dialog_is_modal(web_doc *d,node_t *n){(void)d;return n==modal;}
bool web_dialog_layer_box(web_doc *d,box_t *b){(void)d;(void)b;return false;}
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){(void)d;(void)b;return false;}
void web_avmedia_checkpoint(void){}
struct bfc {int n;};
static void layout_inner(box_t *b,struct bfc *f,float x,float y,float h);
/* Guard all irrelevant paths: no float/replaced/font/table/flex/grid simulation
   can accidentally make the actual block/inset propagation cases pass. */
static void size_atomic(box_t *b,float w,float h){(void)b;(void)w;(void)h;unexpected++;}
static void table_width(box_t *b,float w){(void)b;(void)w;unexpected++;}
static float stf_width(box_t *b,float w){(void)b;(void)w;unexpected++;return 0;}
static void list_abs(box_t *b){(void)b;unexpected++;}
static void place_float(box_t *b,box_t *p,struct bfc *f,float x,float y,float h){(void)b;(void)p;(void)f;(void)x;(void)y;(void)h;unexpected++;}
static float bfc_clear(struct bfc *f,int sides){(void)f;(void)sides;unexpected++;return 0;}
static float bfc_next(struct bfc *f,float y){(void)f;(void)y;unexpected++;return -1;}
static void bfc_space(struct bfc *f,float y,float h,float x,float w,float *l,float *r){(void)f;(void)y;(void)h;*l=x;*r=w;unexpected++;}
static float bfc_bottom(struct bfc *f){(void)f;unexpected++;return 0;}
static void bfc_free(struct bfc *f){(void)f;}
static void layout_frameset(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_table(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_flex(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_grid(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_inline(box_t *b,struct bfc *f,float x,float y,float h){(void)b;(void)f;(void)x;(void)y;(void)h;unexpected++;}
'''
c=out/'absolute-height18-generated.c'
c.write_text(prefix+pieces+read('tests/absolute_height18_cases.c'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'absolute-height18.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
