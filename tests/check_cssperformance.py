"""CSS固定スタイル共有と宣言キャッシュの実C限定検査。VM受入ではない。"""
from pathlib import Path
import os
import subprocess
import sys
import url_owner

root = Path(__file__).resolve().parent.parent
out = root / "build/browser-performance-css"
out.mkdir(exist_ok=True)
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")

def function(signature):
    start = css.index(signature)
    return css[start:css.index("\n}", start) + 2] + "\n"

# MinGW cannot dead-strip every exported CSS entry before unresolved-symbol
# diagnostics. Extract complete, unchanged parser/ownership functions instead
# of supplying mock DOM/form/cascade implementations to make the link succeed.
production = css[:css.index("/* ---------------------------------------------------------------- matching */")]
production += css[css.index("/* ---------------------------------------------------------------- feature queries"):
                  css.index("struct sheet_parse_job {")]
production += css[css.index("/* ---------------------------------------------------------------- media queries"):
                  css.index("static bool media_chain_ok(")]
production = production.replace("#include <nocturne.h>\n", "")
faults = """#include <stdlib.h>
static unsigned fail_style_malloc, fail_cache_calloc;
static void *style_fault_malloc(size_t n){if(fail_style_malloc && !--fail_style_malloc)return NULL;return malloc(n);}
static void *cache_fault_calloc(size_t n,size_t size){if(fail_cache_calloc && !--fail_cache_calloc)return NULL;return calloc(n,size);}
#define malloc style_fault_malloc
#define calloc cache_fault_calloc
"""
production = faults + "void *thread_local_get(unsigned slot);\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n" + production
production += css[css.index("struct cascade {"):css.index("struct css_rollback {")]
for name in ("void css_node_style_free(", "void css_node_style_unpublish(", "void css_styles_release(",
             "static void mark_style_borrowers(", "static void retire_style("):
    production += function(name)
cases = (root / "tests/cssperformancetest.c").read_text(encoding="utf-8")
cases = cases[cases.index("static int checks, failed;"):]
paint = (root / "user/libc/web/paint.c").read_text(encoding="utf-8")
for signature in ("static uint32_t with_alpha(", "static uint32_t control_text_color("):
    start = paint.index(signature)
    production += paint[start:paint.index("\n}", start) + 2] + "\n"
box = (root / "user/libc/web/box.c").read_text(encoding="utf-8")
def box_function(signature):
    start = box.index(signature)
    first = box[start:box.index("\n", start)]
    return (first if first.endswith("}") else box[start:box.index("\n}", start) + 2]) + "\n"
# The decisions of a local box publication: where a regenerated box goes in
# its parent's fixup, and which boxes count (counter.c numbers the lists again).
for signature in ("static int atomic_kind(", "bool box_pseudo_generated(", "bool box_block_level(",
                  "static bool preserves_space(", "static bool ws_only(", "static bool table_part(",
                  "static bool real_inline(", "static bool local_same_place(", "static bool inline_content(",
                  "static bool local_collapses(", "static bool counting_style(", "static bool marker_generated("):
    production += box_function(signature)
dom = (root / "user/libc/web/dom.c").read_text(encoding="utf-8")
for signature in ("node_t *doc_shadow_parent(", "node_t *doc_node_root("):
    start = dom.index(signature)
    production += dom[start:dom.index("\n}", start) + 2] + "\n"
# A retired style marks the boxes that borrow it (mark_style_borrowers): a modal dialog's
# ::backdrop box is looked up in the document's dialog stack.
dialog = (root / "user/libc/web/web_dialog.c").read_text(encoding="utf-8")
production += dialog[dialog.index("struct dialog_entry {"):dialog.index("static bool reserve_dialog(")]
for signature in ("static int index_of(", "box_t *web_dialog_backdrop("):
    start = dialog.index(signature)
    production += dialog[start:dialog.index("\n}", start) + 2] + "\n"
# Reached only from a display: contents element's style, an element that owns CSS animations
# (animation.c) and the animation of transform lists (layout.c), which these cases do not make.
production += """node_t *doc_flat_parent(node_t *n){(void)n;abort();}
void web_motion_cancel(node_t *n,int pseudo){(void)n;(void)pseudo;abort();}
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]){(void)t;(void)w;(void)h;(void)m;abort();}
void css_transform_list_matrix_3d(const struct css_transform *t,double m[16]){(void)t;(void)m;abort();}
"""
document = (root / "user/libc/web/doc.c").read_text(encoding="utf-8")
start = document.index("static bool committed_geometry_current(")
production += document[start:document.index("\n}", start) + 2] + "\n"
native = out / "native.c"
native.write_text(production + "\nbool web_native_checkpoint(web_doc *d){(void)d;return true;}\nvoid web_avmedia_checkpoint(void){}\nint display_bits_per_channel(void){return 8;}\n" + cases,
                  encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
if not cc.exists():
    cc = root.parents[2] / "tools/msys64/ucrt64/bin/clang.exe"  # a git worktree shares the main tree's tools
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n',
                               encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
# url() resolution is web_url.c's, on Lexbor's URL parser, as in the guest: the URL record's
# owner as tests/url_owner.py builds it for every host check (this check builds util.c itself)
url_objects = url_owner.objects(root, cc, util=False)
exe = out / "native.exe"
command = [str(cc), "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon",
           "-Iuser/libc", "-Iuser/libc/web", "-Itests", "-Ithird_party/quickjs", str(native), "user/libc/web/cssprop.c", "user/libc/web/shape.c","user/libc/web/color.c", "user/libc/web/cssmath.c",
           "user/libc/web/util.c", "user/libc/web/html.c", *url_objects, "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)] + (["local-boxes"] if "--local-boxes" in sys.argv else ["placeholder"] if "--placeholder" in sys.argv else ["text-overflow"] if "--text-overflow" in sys.argv else ["font-shorthand"] if "--font-shorthand" in sys.argv else ["flex"] if "--flex" in sys.argv else ["media-calc"] if "--media-calc" in sys.argv else ["fonts"] if "--fonts" in sys.argv else ["second"] if "--second" in sys.argv else []), cwd=root, env=env, check=True, timeout=30)
