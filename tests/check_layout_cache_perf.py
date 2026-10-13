"""製品のdirty枝/相対offset/overflow集計を限定ホスト条件で実行。"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build/browser-performance-layout"
out.mkdir(parents=True, exist_ok=True)
source = (root / "user/libc/web/layout.c").read_text(encoding="utf-8")

def function(signature):
    at = source.index(signature)
    return source[at:source.index("\n}", at) + 2] + "\n"

prefix = r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
static web_doc document, *D = &document;
static float VW=800, VH=600;
static unsigned checks, failures, checkpoints, resolutions, pct_queries;
static unsigned cancel_at;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.01f;}
static bool layout_checkpoint(void){checkpoints++;if(cancel_at&&checkpoints>=cancel_at)D->native_cancelled=true;return !D->native_cancelled;}
float len_resolve(const len_t *l,float base){resolutions++;return l->kind==LK_LEN?l->px+l->pct*base:0;}
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){return b==d->root_box;}
bool box_filter_containing_block(const box_t *b){return b->st&&b->st->filter;}
bool web_dialog_layer_box(web_doc *d,box_t *b){(void)d;(void)b;return false;}
bool box_viewport_fixed(const box_t *b){return b->st&&b->st->position==POS_FIXED&&b->cb==D->root_box;}
float box_translation_axis(const box_t *b,unsigned axis,unsigned sources){(void)sources;return b->st->translate.active?b->st->translate.xy[axis].px:0;}
static bool counted_pct(const len_t *l){pct_queries++;return len_has_pct(l);}
#define len_has_pct counted_pct
'''
functions = [
    "static float fmaxf_(", "static bool is_flex_item(", "static bool size_container(",
    "static bool is_bfc_root(", "bool box_element_scrollable(", "static double scroll_limit(",
    "void box_scroll_clamp(", "void layout_invalidate(", "void layout_invalidate_fonts(",
    "static bool layout_cache_prepare(", "static void relative_offsets(",
    "static float scroll_extent(", "static void scroll_area_publish(",
    "static void scroll_areas(", "static float max_bottom(",
]
cases = (root / "tests/layout_cache_perf_cases.c").read_text(encoding="utf-8")
generated = out / "layout-cache-perf-generated.c"
generated.write_text(prefix + "".join(function(s) for s in functions) + cases, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
exe = out / "layout-cache-perf.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
subprocess.run([str(cc), "-std=gnu11", "-O1", "-Iuser/include", "-Icommon", "-Iuser/libc/web", str(generated), "-o", str(exe)], cwd=root, env=env, check=True)
subprocess.run([str(exe)] + (["--round2"] if "--round2" in sys.argv else []), cwd=root, env=env, check=True, timeout=30)
