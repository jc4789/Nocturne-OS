"""新しい未使用 margin-chain 除去の native 境界だけを一回実行する。"""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/goal-20261009'
layout = (root / 'user/libc/web/layout.c').read_text(encoding='utf-8')
prop = (root / 'user/libc/web/cssprop.c').read_text(encoding='utf-8')
box = (root / 'user/libc/web/box.c').read_text(encoding='utf-8')

def fn(source, signature):
    at = source.index(signature)
    return source[at:source.index('\n}', at) + 2] + '\n'

at = prop.index('struct cexpr {')
pieces = prop[at:prop.index('};', at) + 2] + '\n'
pieces += fn(prop, 'static float ceval(')
at = prop.index('#define LEN_MAX ')
pieces += prop[at:prop.index('\n\n', at)] + '\n'
pieces += fn(prop, 'float len_resolve(')
for signature in ('static float hext(', 'static float vext(', 'static float fmaxf_(', 'static float fminf_('):
    at = layout.index(signature)
    pieces += layout[at:layout.index('\n', at) + 1]
for signature in ('static void resolve_edges(', 'static float spec_w(', 'static float spec_h(',
                  'static float clamp_w(', 'static float clamp_h(', 'static float resolved_content_height(',
                  'static float mcollapse(', 'static bool is_flex_item(', 'static bool is_bfc_root('):
    pieces += fn(layout, signature)
pieces += fn(box, 'bool box_block_level(')
for signature in ('static bool collapses_top(', 'static bool collapses_bottom(', 'static box_t *first_inflow('):
    pieces += fn(layout, signature)
chain = fn(layout, 'static float top_chain(')
chain = chain.replace('float positive = 0, negative = 0;', 'top_calls++;\n    float positive = 0, negative = 0;')
chain = chain.replace('float m = len_auto(', 'top_nodes++;\n        float m = len_auto(')
pieces += chain
for signature in ('static void block_width(', 'static void layout_blocks(',
                  'static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {',
                  'static void layout_inner_used(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh) {'):
    pieces += fn(layout, signature)
prefix = r'''
#include <stdio.h>
#include <math.h>
#include "webi.h"
static web_doc *D;
static float VH;
static unsigned unexpected,top_calls,top_nodes,checkpoint_calls;
struct bfc {int n;};
static void layout_inner(box_t *b,struct bfc *f,float x,float y,float h);
static void layout_inner_used(box_t *b,struct bfc *f,float x,float y,float h,float usedh);
bool web_dialog_is_modal(web_doc *d,node_t *n){(void)d;(void)n;return false;}
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){(void)d;(void)b;return false;}
void web_avmedia_checkpoint(void){checkpoint_calls++;}
/* No irrelevant stub supplies geometry or hides a tested margin failure. */
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
generated = out / 'margin-hoist22-generated.c'
generated.write_text(prefix + pieces + (root / 'tests/margin_hoist22_cases.c').read_text(encoding='utf-8'), encoding='utf-8')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
exe = out / 'margin-hoist22.exe'
subprocess.run([str(cc), '-std=gnu11', '-O1', '-Iuser/include', '-Icommon', '-Iuser/libc/web', str(generated), '-o', str(exe)], cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=20)
