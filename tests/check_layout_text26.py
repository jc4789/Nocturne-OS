"""新しい同一layout文字測定cacheの境界だけを一度実行する補助。"""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
layout = (root / "user/libc/web/layout.c").read_text(encoding="utf-8")
font = (root / "user/libc/font.c").read_text(encoding="utf-8")

def fn(source, signature):
    at = source.index(signature)
    return source[at:source.index("\n}", at) + 2] + "\n"

prefix = r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
static unsigned checks, failures, measured, live_allocations;
static size_t fail_at, allocation_attempts;
static bool change_backend;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void *memo_malloc(size_t n){allocation_attempts++;if(fail_at==allocation_attempts)return NULL;void *p=malloc(n);if(p)live_allocations++;return p;}
static void *memo_calloc(size_t n,size_t m){allocation_attempts++;if(fail_at==allocation_attempts)return NULL;void *p=calloc(n,m);if(p)live_allocations++;return p;}
static void memo_free(void *p){if(p){live_allocations--;free(p);}}
static uint64_t font_metrics_epoch = 1;
'''
at = font.index("uint64_t font_metrics_generation(void)")
prefix += font[at:font.index("\n", at)] + "\n"
prefix += r'''
/* The real font decoder is not exercised here. This deterministic counting
   backend exposes duplicate dispatch and key/ownership errors. */
float wf_width(const wfont *f,const char *s,size_t n){
    measured++;float w=(f->ttf?(float)((uintptr_t)f->ttf&15):0)+(f->bold?1:0);
    for(size_t i=0;i<n;i++)w+=(unsigned char)s[i]*f->px;
    if(change_backend){font_metrics_epoch++;change_backend=false;}
    return w;
}
#define malloc memo_malloc
#define calloc memo_calloc
#define free memo_free
'''
pieces = fn(layout, "static float text_width_uncached(")
pieces += '#include "layout_text_cache.h"\n'
pieces += r'''
static web_doc *D;
static float VW,VH;
static uint32_t GEN;
static unsigned scope_mode, scope_depth;
static style_t test_style;
static wfont test_font={NULL,1,false};
static web_doc nested_doc;
static box_t nested_box;
void ar_free(arena_t *a){(void)a;}
static float fmaxf_(float a,float b){return a>b?a:b;}
static void layout_abs(box_t *b){(void)b;}
static void relative_offsets(box_t *b){(void)b;}
static void scroll_areas(box_t *b){(void)b;}
static float max_bottom(box_t *b,float y){return b->h+y;}
float box_abs_y(const box_t *b){return b->y;}
static void layout_inner(box_t *b,void *f,float x,float y,float h){
    (void)f;(void)x;(void)y;(void)h;b->h=10;
    float a=text_width(&test_style,&test_font,"scope",5);
    float z=text_width(&test_style,&test_font,"scope",5);
    check(a==z,"scope real value");
    if(scope_mode==1&&!scope_depth){
        scope_depth++;layout_doc(&nested_doc,80,90);scope_depth--;
        check(text_width(&test_style,&test_font,"scope",5)==a,"outer cache restored");
    }
    if(scope_mode==2)longjmp(*D->lmem.trap,3);
}
'''
pieces += fn(layout, "static int document_extent(")
pieces += fn(layout, "void layout_doc(")
cases = (root / "tests/layout_text26_cases.c").read_text(encoding="utf-8")
generated = out / "layout-text26-generated.c"
generated.write_text(prefix + pieces + cases, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "layout-text26-host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "layout-text26-native.exe"
subprocess.run([str(cc), "-std=gnu11", "-O1", "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc/web",
                str(generated), "-o", str(exe)], cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
