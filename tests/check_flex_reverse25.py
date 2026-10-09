"""新しい逆折返し境界だけ。以前の layout/CSS suite は実行しない。"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
layout = (root / "user/libc/web/layout.c").read_text(encoding="utf-8")
box = (root / "user/libc/web/box.c").read_text(encoding="utf-8")
js = (root / "user/libc/web/js.c").read_text(encoding="utf-8")

def fn(source, signature):
    at = source.index(signature)
    return source[at:source.index("\n}", at) + 2] + "\n"

pieces = ""
for signature in ("static float hext(", "static float vext(", "static float fmaxf_(", "static float fminf_("):
    at = layout.index(signature)
    pieces += layout[at:layout.index("\n", at) + 1]
for signature in ("static void resolve_edges(", "static float spec_w(", "static float spec_h(",
                  "static float clamp_w(", "static float clamp_h(", "static float resolved_content_height(",
                  "static float mcollapse(", "static bool is_flex_item(", "static bool is_bfc_root("):
    pieces += fn(layout, signature)
pieces += fn(box, "bool box_block_level(")
pieces += r'''
/* Inline text, table and replaced-element intrinsic widths are outside this
   boundary. Ordinary native block sizing, flex lines, both axes, stretch and
   descendant height layout below are the current product functions. */
static void intrinsic(box_t *b,float *mn,float *mx){float w=spec_w(b,&b->st->width,-1);*mn=*mx=w>=0?w:20;}
static float stf_width(box_t *b,float w){float mn,mx;intrinsic(b,&mn,&mx);return fminf_(fmaxf_(mn,w-hext(b)-b->m[1]-b->m[3]),mx);}
'''
for signature in ("static bool collapses_top(", "static bool collapses_bottom(",
                  "static box_t *first_inflow(", "static float top_chain(",
                  "static void block_width(", "static void layout_blocks("):
    pieces += fn(layout, signature)
at = layout.index("struct fitem {")
pieces += layout[at:layout.index("/* ---------------------------------------------------------------- grid */", at)]
pieces += fn(layout, "static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {")
pieces += fn(layout, "static void layout_inner_used(box_t *b, struct bfc *f, float ox, float oy, float cbh, float usedh) {")
at = js.index('    else if (!strcmp(property,"flex-wrap")) {')
serializer = js[at:js.index('    else if (!strcmp(property,"object-fit")) {', at)]
pieces += 'static void serialize_flex(const style_t *st,const char *property,char *destination) {\n    char out[128] = {0};\n    if (false) {}\n' + serializer + '    strcpy(destination,out);\n}\n'
prefix = r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
#define INF 1e30f
static web_doc *D;
static float VH;
static unsigned unexpected;
struct bfc {int n;};
static void layout_inner(box_t *b,struct bfc *f,float x,float y,float h);
static void layout_inner_used(box_t *b,struct bfc *f,float x,float y,float h,float usedh);
bool web_dialog_is_modal(web_doc *d,node_t *n){(void)d;(void)n;return false;}
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){(void)d;(void)b;return false;}
void web_avmedia_checkpoint(void){}
static void size_atomic(box_t *b,float w,float h){(void)b;(void)w;(void)h;unexpected++;}
static void table_width(box_t *b,float w){(void)b;(void)w;unexpected++;}
static void table_intrinsic(box_t *b,float *mn,float *mx){(void)b;*mn=*mx=0;unexpected++;}
static void list_abs(box_t *b){(void)b;unexpected++;}
static void place_float(box_t *b,box_t *p,struct bfc *f,float x,float y,float h){(void)b;(void)p;(void)f;(void)x;(void)y;(void)h;unexpected++;}
static float bfc_clear(struct bfc *f,int sides){(void)f;(void)sides;unexpected++;return 0;}
static float bfc_next(struct bfc *f,float y){(void)f;(void)y;unexpected++;return -1;}
static void bfc_space(struct bfc *f,float y,float h,float x,float w,float *l,float *r){(void)f;(void)y;(void)h;*l=x;*r=w;unexpected++;}
static float bfc_bottom(struct bfc *f){(void)f;unexpected++;return 0;}
static void bfc_free(struct bfc *f){(void)f;}
static void layout_frameset(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_table(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_grid(box_t *b,float h){(void)b;(void)h;unexpected++;}
static void layout_inline(box_t *b,struct bfc *f,float x,float y,float h){(void)b;(void)f;(void)x;(void)y;(void)h;unexpected++;}
'''
cases = (root / "tests/flex_reverse25_cases.c").read_text(encoding="utf-8")
label = "flex-reverse25"
if sys.argv[1:] == ["--clamp-only"]:
    # Only the previously unexecuted container clamp boundary; none of the
    # original 48 checks is included in this generated main function.
    cases = cases[:cases.index("int main(void){")] + (root / "tests/flex_reverse25_clamp_case.c").read_text(encoding="utf-8")
    label += "-clamp"
else:
    assert not sys.argv[1:]
generated = out / (label + "-generated.c")
generated.write_text(prefix + pieces + cases, encoding="utf-8")
http = (root / "user/libc/http.c").read_text(encoding="utf-8")
urls = out / "flex-reverse25-url-generated.c"
urls.write_text(http[:http.index("enum http_url_result http_origin_owned_n(")], encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "flex-reverse25-host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / (label + "-native.exe")
subprocess.run([str(cc), "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc/web", str(generated),
                "user/libc/web/cssprop.c", "user/libc/web/util.c", "user/libc/web/html.c", str(urls), "-o", str(exe)],
               cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
