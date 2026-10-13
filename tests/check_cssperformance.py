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
production += css[css.index("struct cascade {"):css.index("static void apply_decl(")]
for name in ("static void css_motion_reset(", "int css_animation_set(", "void css_animation_free(",
             "void css_node_style_free(", "void css_node_style_unpublish(", "void css_styles_release(", "static void retire_style("):
    production += function(name)
cases = (root / "tests/cssperformancetest.c").read_text(encoding="utf-8")
cases = cases[cases.index("static int checks, failed;"):]
native = out / "native.c"
native.write_text(production + "\nbool web_native_checkpoint(web_doc *d){(void)d;return true;}\n" + cases,
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
           "-Iuser/libc/web", "-Ithird_party/quickjs", str(native), "user/libc/web/cssprop.c",
           "user/libc/web/util.c", "user/libc/web/html.c", str(urls), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)] + (["second"] if "--second" in sys.argv else []), cwd=root, env=env, check=True, timeout=30)
