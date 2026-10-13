"""CSS固定スタイル共有と宣言キャッシュの実C限定検査。VM受入ではない。"""
from pathlib import Path
import os
import subprocess
import sys

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
production += css[css.index("struct css_animation {"):css.index("static void css_motion_reset(")]
production += css[css.index("struct cascade {"):css.index("struct css_rollback {")]
for name in ("static void css_motion_reset(", "int css_animation_set(", "void css_animation_free(",
             "void css_node_style_free(", "void css_node_style_unpublish(", "void css_styles_release(", "static void retire_style("):
    production += function(name)
cases = (root / "tests/cssperformancetest.c").read_text(encoding="utf-8")
cases = cases[cases.index("static int checks, failed;"):]
paint = (root / "user/libc/web/paint.c").read_text(encoding="utf-8")
for signature in ("static uint32_t with_alpha(", "static uint32_t control_text_color("):
    start = paint.index(signature)
    production += paint[start:paint.index("\n}", start) + 2] + "\n"
box = (root / "user/libc/web/box.c").read_text(encoding="utf-8")
for signature in ("static int atomic_kind(", "bool box_pseudo_generated("):
    start = box.index(signature)
    production += box[start:box.index("\n}", start) + 2] + "\n"
start = box.index("struct local_box_subject {")
production += box[start:box.index("\n", start)+1]
for signature in ("static bool local_plain_length(", "static bool local_plain_style(",
                  "static bool local_plain_subtree(", "static struct local_box_subject *local_subject(",
                  "static bool local_plain_parent("):
    start = box.index(signature)
    production += box[start:box.index("\n}", start) + 2] + "\n"
dom = (root / "user/libc/web/dom.c").read_text(encoding="utf-8")
for signature in ("node_t *doc_shadow_parent(", "node_t *doc_node_root("):
    start = dom.index(signature)
    production += dom[start:dom.index("\n}", start) + 2] + "\n"
document = (root / "user/libc/web/doc.c").read_text(encoding="utf-8")
start = document.index("static bool committed_geometry_current(")
production += document[start:document.index("\n}", start) + 2] + "\n"
native = out / "native.c"
native.write_text(production + "\nbool web_native_checkpoint(web_doc *d){(void)d;return true;}\nvoid web_avmedia_checkpoint(void){}\n" + cases,
                  encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n',
                               encoding="utf-8")
http = (root / "user/libc/http.c").read_text(encoding="utf-8")
urls = out / "url.c"
urls.write_text(http[:http.index("enum http_url_result http_origin_owned_n(")], encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "native.exe"
command = [str(cc), "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon",
           "-Iuser/libc/web", "-Itests", "-Ithird_party/quickjs", str(native), "user/libc/web/cssprop.c",
           "user/libc/web/util.c", "user/libc/web/html.c", str(urls), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)] + (["local-boxes"] if "--local-boxes" in sys.argv else ["placeholder"] if "--placeholder" in sys.argv else ["text-overflow"] if "--text-overflow" in sys.argv else ["font-shorthand"] if "--font-shorthand" in sys.argv else ["flex"] if "--flex" in sys.argv else ["media-calc"] if "--media-calc" in sys.argv else ["fonts"] if "--fonts" in sys.argv else ["second"] if "--second" in sys.argv else []), cwd=root, env=env, check=True, timeout=30)
