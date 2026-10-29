"""CSS Anchor Positioning 1 の配置（layout.c の絶対配置の段）を、実物のコードで host で確かめる。

layout.c から、配置の段の記録・形の助け（resolve_edges・spec_w・clamp_h・box_abs_x など）・大きさの値の節
（spec_w・clamp_h が内在の大きさの語・fit-content()・calc-size() を解く size_w・size_h など。中身の大きさは道具が与える）・
絶対配置と anchor positioning の節（abs_place・anchor_layout・layout_abs・box_anchor_*）を
そのまま取り出し、性質の表（cssprop.c）で解析した装いの箱を手で置いた道具
（tests/anchor_position_native_cases.c）で組んで走らせる。試すのは WPT の
css/css-anchor-position の頁（position-area-basic・position-area-align-justify・
anchor-center-htb-htb・anchor-position-001・anchor-scope-basic・try-tactic-basic・
position-try-order-basic）の位置と大きさ、§2.3 の acceptable anchor element、§6.5.1.1 の
last successful position option、position-visibility、§3.3 のスクロールの補償。

guest の WPT の代わりではない。cascade（css.c の @position-try、var()）・箱の生成・inline の
配置・描画・getComputedStyle は通らない。流れの中の箱は頁の配置が置く所へ手で置き、絶対配置の
箱の中身の大きさは与える（字の配置はしない）。

  python -X utf8 tests/check_anchor_position_native.py [--out 出力の場所]
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / "build/check-anchor-position"
out.mkdir(parents=True, exist_ok=True)
layout = (root / "user/libc/web/layout.c").read_text(encoding="utf-8")

def marker(name):
    m = re.search(r"/\* -+ " + re.escape(name) + r" \*/", layout)
    if not m:
        sys.exit("layout.c に節が無い：" + name)
    return m.start()

def function(signature):
    start = layout.index(signature)
    return layout[start:layout.index("\n}", start) + 2] + "\n"

# the layout context and the anchor-positioned box's record
production = layout[layout.index("struct layout_text_cache;"):marker("fonts")]
# the geometry helpers the absolute positioning stage uses
production += layout[marker("geometry helpers"):layout.index("static float mcollapse(")]
for signature in ("float box_abs_x(", "float box_abs_y(", "bool box_element_scrollable(", "bool box_viewport_fixed(",
                  "void box_abs_cb_rect(", "static bool is_flex_item("):
    production += function(signature)
# the sizing values (keywords, fit-content(), calc-size()) spec_w and clamp_h resolve through, whole
production += layout[marker("sizing values (css-sizing-3 §3.2, css-sizing-4 §3.2, §7)"):marker("replaced elements")]
# absolute positioning and anchor positioning, whole
production += layout[marker("absolute positioning"):marker("entry")]
prefix = """#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "webi.h"
#include "web_dialog.h"
void *thread_local_get(unsigned slot);
unsigned parallel_worker_index(void);
void web_avmedia_checkpoint(void);
struct bfc;
static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh);
static float stf_width(box_t *b, float avail);
static void intrinsic(box_t *b, float *mn, float *mx);
static void size_atomic(box_t *c, float cbw, float cbh);
static void table_width(box_t *t, float cw);
static bool box_sticky(const box_t *b);
static void sticky_offset(const box_t *s, float shift[2]);
"""
native = out / "native.c"
native.write_text(prefix + production + "\n" + (root / "tests/anchor_position_native_cases.c").read_text(encoding="utf-8"),
                  encoding="utf-8", newline="\n")

# the repository's tools (a worktree has them in the main checkout above it)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "anchor-position.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
           "-Wl,--gc-sections", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon",
           "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(native), "user/libc/web/cssprop.c",
           "user/libc/web/shape.c", "user/libc/web/color.c", "user/libc/web/cssmath.c", "user/libc/web/util.c", "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
result = subprocess.run([str(exe)], cwd=root, env=env, timeout=60)
sys.exit(result.returncode)
