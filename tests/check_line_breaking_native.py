"""CSS Text 3/4 の行分け（layout.c のインラインの行分け）を、実物のコードで host で確かめる。

layout.c から、字体の節（style_font・wf_*・字幅の記憶）・字の塊の境界・形の助け（resolve_edges
など）・インラインの項目と行分けの節（行分けの規則 wrap_between、text_items・build_items・
finish_line・split_word・layout_inline）と、固有の幅（inline_intrinsic）をそのまま取り出し、
box.c の空白の処理（ws_process）も取り出して、性質の表（cssprop.c）で解析した装いの箱を手で
組んだ道具（tests/line_breaking_native_cases.c）で走らせる。字体は道具の等幅の模型（Latin の
1 字は font-size の半分、CJK と全角は font-size、既定で無視する字は 0）。

試すのは WPT の css/css-text の頁（hyphens・line-break・word-break・overflow-wrap・tab-size）の
行の割れ方と、仕様の原文から決まる行（hyphenate-limit-chars・hyphenate-limit-lines・Southeast
Asian の代わりの割れ目）。

guest の WPT の代わりではない。cascade（css.c）・箱の生成（box.c の gen）・浮動・atomic inline・
描画・本物の字体は通らない。

  python -X utf8 tests/check_line_breaking_native.py [--out 出力の場所]
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / "build/check-line-breaking"
out.mkdir(parents=True, exist_ok=True)
layout = (root / "user/libc/web/layout.c").read_text(encoding="utf-8")
box = (root / "user/libc/web/box.c").read_text(encoding="utf-8")

def marker(name, after=0):
    m = re.compile(r"/\* -+ " + re.escape(name) + r" \*/").search(layout, after)
    if not m:
        sys.exit("layout.c に節が無い：" + name)
    return m.start()

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2] + "\n"

# the layout context and the anchor-positioned box's record (the geometry helpers read it)
production = layout[layout.index("struct layout_text_cache;"):marker("fonts")]
# fonts, grapheme boundaries and the measured text widths
production += layout[marker("fonts"):marker("geometry helpers")]
# the geometry helpers the inline layout uses
production += layout[marker("geometry helpers"):layout.index("static float mcollapse(")]
# inline items and inline layout, whole
production += layout[marker("inline items"):marker("text geometry")]
production += function(layout, "size_t layout_text_boundary(")
production += function(layout, "static void item_clusters(")
production += function(layout, "static void inline_intrinsic(")
# white-space processing
production += function(box, "static void transform(") + function(box, "static const char *ws_process(")

prefix = """#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <wctype.h>
#include "webi.h"
#include "web_dialog.h"
#include "avmedia.h"
void *thread_local_get(unsigned slot);
unsigned parallel_worker_index(void);
void web_avmedia_checkpoint(void);
/* no floats */
struct bfc { int unused; };
static void bfc_space(const struct bfc *f, float y, float h, float x0, float x1, float *l, float *r, bool lines) {
    (void)f; (void)y; (void)h; (void)lines; *l = x0; *r = x1;
}
static float bfc_next(const struct bfc *f, float y) { (void)f; (void)y; return -1; }
static void size_atomic(box_t *c, float cbw, float cbh);
static void outer_intrinsic(box_t *c, float *mn, float *mx);
static void list_abs(box_t *b);
"""
if "static void list_abs(" in production:
    prefix = prefix.replace("static void list_abs(box_t *b);\n", "")
native = out / "native.c"
native.write_text(prefix + production + "\n" + (root / "tests/line_breaking_native_cases.c").read_text(encoding="utf-8"),
                  encoding="utf-8", newline="\n")

# the repository's tools (a worktree has them in the main checkout above it)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
# the host's own setjmp.h and wctype.h (user/include has the guest libc's)
for header in ("setjmp.h", "wctype.h"):
    (abi / header).write_text('#include "' + (cc.parent.parent / "include" / header).as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "line-breaking.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
           "-Wl,--gc-sections", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon",
           "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(native), "user/libc/web/cssprop.c",
           "user/libc/web/shape.c", "user/libc/web/color.c", "user/libc/web/cssmath.c", "user/libc/web/util.c",
           "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
result = subprocess.run([str(exe)], cwd=root, env=env, timeout=60)
sys.exit(result.returncode)
