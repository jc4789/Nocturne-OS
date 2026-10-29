"""mask の層（css-masking-1 §7）の描画の部品を、描画の本物のコード（user/libc/web/paint.c の mask_value・mask_box・
layer_repeat_axis・mask_composite）で host で確かめる。

paint.c から四つの関数を名前で切り出し、webi.h の列挙と一緒に host の clang で組んだ小さな道具に渡す。期待は
§7.2（alpha と luminance：輝度は Compositing の係数 .2125・.7154・.0721 と alpha の積）、§7.5（fill-box は内容の箱、
stroke-box・view-box は border の箱）、§7.8 と Compositing 1 §9.1.1（add は source-over、subtract は source-out、
intersect は source-in、exclude は xor）、css-backgrounds-3 §3.4（space：入るだけの数を両端に付けて等間隔、
二つ入らなければ一つを位置どおり）から手で計算したもの。

画素の流れ（層の画像の大きさと位置、切り抜き、group との混ぜ）・画像の取得と CORS は通らない。guest の WPT の
reftest の代わりではない。

  python -X utf8 tests/check_mask_layers_native.py
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build/check-mask-layers"
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
paint = (root / "user/libc/web/paint.c").read_text(encoding="utf-8")
webi = (root / "user/libc/web/webi.h").read_text(encoding="utf-8")


def function(name):
    """a static function of paint.c, from its signature to the brace that closes it at column 0"""
    m = re.search(r"^static [^\n]*\b" + name + r"\(.*?^\}\n", paint, re.S | re.M)
    if not m:
        sys.exit(f"paint.c に {name} が無い")
    return m.group(0)


def enum(first):
    """the enum of webi.h whose first name is first"""
    m = re.search(r"^enum \{ " + first + r"\b[^}]*\};[^\n]*\n", webi, re.M)
    if not m:
        sys.exit(f"webi.h に {first} の列挙が無い")
    return m.group(0)


driver = r"""
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
""" + enum("CSS_BOX_BORDER") + enum("CSS_MASK_MATCH_SOURCE") + enum("CSS_COMPOSITE_ADD") + enum("CSS_REPEAT") + \
    function("mask_value") + function("mask_box") + function("layer_repeat_axis") + function("mask_composite") + r"""
/* each line: a kind and its numbers; the answer on one line */
int main(void) {
    char kind[16];
    while (scanf("%15s", kind) == 1) {
        if (!strcmp(kind, "value")) {
            unsigned col, luminance;
            if (scanf("%x %u", &col, &luminance) != 2) return 1;
            printf("%u\n", mask_value(col, luminance != 0));
        } else if (!strcmp(kind, "box")) {
            unsigned box;
            if (scanf("%u", &box) != 1) return 1;
            printf("%u\n", mask_box((uint8_t)box));
        } else if (!strcmp(kind, "axis")) {
            unsigned repeat;
            float start, size, t, first;
            if (scanf("%u %f %f %f %f", &repeat, &start, &size, &t, &first) != 5) return 1;
            float period;
            layer_repeat_axis((uint8_t)repeat, start, size, t, &first, &period);
            printf("%.4f %.4f\n", first, period);
        } else if (!strcmp(kind, "composite")) {
            unsigned op, s, d;
            if (scanf("%u %u %u", &op, &s, &d) != 3) return 1;
            printf("%u\n", mask_composite((uint8_t)op, s, d));
        } else return 1;
    }
    return 0;
}
"""
(out / "mask_layers.c").write_text(driver, encoding="utf-8")
exe = out / "mask-layers.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
subprocess.run([str(cc), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                str(out / "mask_layers.c"), "-o", str(exe)], cwd=root, env=env, check=True)

BORDER, PADDING, CONTENT, MARGIN, FILL, STROKE, VIEW = range(7)
REPEAT, SPACE, ROUND, NO = range(4)
ADD, SUBTRACT, INTERSECT, EXCLUDE = range(4)
cases = []  # (説明, 入力の行, 期待を判じる関数)


def near(want, slack):
    return lambda got: all(abs(float(g) - w) <= slack for g, w in zip(got.split(), want))


def value(what, col, luminance):
    a = col >> 24
    if luminance:
        r, g, b = (col >> 16) & 255, (col >> 8) & 255, col & 255
        a = a * (.2125 * r + .7154 * g + .0721 * b) / 255
    cases.append((what, f"value {col:x} {int(luminance)}", near([a], 1)))


value("alpha は画素の alpha", 0x80FFFFFF, False)
value("alpha は色を見ない", 0xFF000000, False)
value("luminance：白は不透明", 0xFFFFFFFF, True)
value("luminance：黒は透明", 0xFF000000, True)
value("luminance：緑は .7154", 0xFF00FF00, True)
value("luminance：半透明の白は alpha との積", 0x80FFFFFF, True)
value("luminance：赤は .2125", 0xFFFF0000, True)

for what, box, want in (("border-box", BORDER, BORDER), ("padding-box", PADDING, PADDING), ("content-box", CONTENT, CONTENT),
                        ("fill-box は内容の箱", FILL, CONTENT), ("stroke-box は border の箱", STROKE, BORDER),
                        ("view-box は border の箱", VIEW, BORDER)):
    cases.append((what, f"box {box}", near([want], 0)))


def axis(what, repeat, start, size, t, first, want_first, want_period):
    cases.append((what, f"axis {repeat} {start} {size} {t} {first}", near([want_first, want_period], 1e-3)))


axis("repeat：位置のまま、周期は tile", REPEAT, 10, 100, 30, 17, 17, 30)
axis("round：丸めた tile で繰り返す", ROUND, 10, 100, 25, 10, 10, 25)
axis("no-repeat：一つ", NO, 10, 100, 30, 40, 40, 0)
axis("space：三つ入る、間は (100 - 90) / 2", SPACE, 10, 100, 30, 50, 10, 35)
axis("space：ちょうど二つ、間は 0", SPACE, 0, 60, 30, 15, 0, 30)
axis("space：一つしか入らない：位置のまま一つ", SPACE, 0, 50, 30, 10, 10, 0)
axis("space：一つも入らない：位置のまま一つ", SPACE, 0, 20, 30, -5, -5, 0)


def composite(what, op, s, d):
    S, D = s / 255, d / 255
    v = {ADD: S + D * (1 - S), SUBTRACT: S * (1 - D), INTERSECT: S * D, EXCLUDE: S * (1 - D) + D * (1 - S)}[op]
    cases.append((what, f"composite {op} {s} {d}", near([v * 255], 1)))


for op, name in ((ADD, "add"), (SUBTRACT, "subtract"), (INTERSECT, "intersect"), (EXCLUDE, "exclude")):
    for s, d in ((0, 0), (255, 0), (0, 255), (255, 255), (128, 128), (200, 60)):
        composite(f"{name} {s} の上の層を {d} の下の層へ", op, s, d)

result = subprocess.run([str(exe)], input="\n".join(c[1] for c in cases) + "\n", capture_output=True, text=True,
                        cwd=root, env=env, timeout=60)
lines = result.stdout.splitlines()
if result.returncode or len(lines) != len(cases):
    sys.exit(f"道具が答えない（終了 {result.returncode}、{len(lines)}/{len(cases)} 行）")
failed = 0
for (what, line, ok), got in zip(cases, lines):
    if not ok(got):
        failed += 1
        print(f"FAIL {what}：{line} → {got}")
print(f"mask-layers: {len(cases)} checks, {failed} failures")
sys.exit(1 if failed else 0)
