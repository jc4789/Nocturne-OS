"""九分割の画像（border-image・mask-border）の分け方と、各部の繰り返しの矩形（stretch・repeat・round・space）を、
描画の本物のコード（user/libc/web/paint.c の nine_slice_axis と nine_slice_plan）で host で確かめる。

paint.c から struct nine_part と二つの関数を名前で切り出し、host の clang で組んだ小さな道具に渡す。期待は
css-backgrounds-3 §6.6（Drawing the Border Image）の四つの段（幅への拡大、繰り返しへの拡大、最初の tile の位置、
敷き詰め）と §6.3 の幅の重なりの縮小、§6.2 の slice の上限から手で計算したもの。

描画そのもの（画素・切り抜き・mask の合成）は通らない。guest の WPT の reftest の代わりではない。

  python -X utf8 tests/check_nine_slice_native.py [--verbose]
"""
from pathlib import Path
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build/check-nine-slice"
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


part = re.search(r"^struct nine_part \{[^}]*\};\n", paint, re.M)
repeat = re.search(r"^enum \{ CSS_BI_STRETCH[^}]*\};\n", webi, re.M)
if not part or not repeat:
    sys.exit("paint.c の struct nine_part か webi.h の CSS_BI_* が無い")
driver = r"""
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
""" + repeat.group(0) + part.group(0) + function("nine_slice_axis") + function("nine_slice_plan") + r"""
/* each line: iw ih slice[4] area[4] width[4] rh rv fill; the answer: the parts' bits, then each part */
int main(void) {
    float iw, ih, s[4], a[4], w[4];
    int rh, rv, fill;
    while (scanf("%f %f %f %f %f %f %f %f %f %f %f %f %f %f %d %d %d", &iw, &ih, &s[0], &s[1], &s[2], &s[3], &a[0], &a[1],
                 &a[2], &a[3], &w[0], &w[1], &w[2], &w[3], &rh, &rv, &fill) == 17) {
        struct nine_part p[9];
        unsigned bits = nine_slice_plan(iw, ih, s, a, w, (uint8_t)rh, (uint8_t)rv, fill != 0, p);
        printf("%u", bits);
        for (int i = 0; i < 9; i++)
            printf(" | %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", p[i].sx, p[i].sy, p[i].sw,
                   p[i].sh, p[i].x, p[i].y, p[i].w, p[i].h, p[i].tw, p[i].th, p[i].ox, p[i].oy, p[i].gx, p[i].gy);
        printf("\n");
    }
    return 0;
}
"""
(out / "nine_slice.c").write_text(driver, encoding="utf-8")
exe = out / "nine-slice.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
subprocess.run([str(cc), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", str(out / "nine_slice.c"), "-o", str(exe)],
               cwd=root, env=env, check=True)

STRETCH, REPEAT, ROUND, SPACE = 0, 1, 2, 3
names = ["左上", "上", "右上", "左", "中", "右", "左下", "下", "右下"]
FIELDS = ("sx", "sy", "sw", "sh", "x", "y", "w", "h", "tw", "th", "ox", "oy", "gx", "gy")
# (説明, 入力, 描く部分の番号, {部分: {欄: 期待}})
cases = []


def case(what, iw, ih, slice, area, width, rh, rv, fill, shown, parts):
    cases.append((what, (iw, ih, *slice, *area, *width, rh, rv, int(fill)), shown, parts))


# 90×90 の画像を 30 で切り、300×150 の領域へ幅 30 で。§6.6 段 1：角は領域の大きさ、上下の辺は高さ 30 に合わせて幅も比で
case("stretch：角と辺の矩形、中は描かない", 90, 90, (30, 30, 30, 30), (0, 0, 300, 150), (30, 30, 30, 30), STRETCH, STRETCH, False,
     {0, 1, 2, 3, 5, 6, 7, 8},
     {0: dict(sx=0, sy=0, sw=30, sh=30, x=0, y=0, w=30, h=30, tw=30, th=30, ox=0, oy=0),
      1: dict(sx=30, sy=0, sw=30, sh=30, x=30, y=0, w=240, h=30, tw=240, th=30, ox=30, oy=0),
      2: dict(sx=60, sy=0, sw=30, sh=30, x=270, y=0, w=30, h=30),
      3: dict(sx=0, sy=30, sw=30, sh=30, x=0, y=30, w=30, h=90, tw=30, th=90, ox=0, oy=30),
      4: dict(x=30, y=30, w=240, h=90),
      5: dict(sx=60, sy=30, x=270, y=30, w=30, h=90),
      6: dict(sx=0, sy=60, x=0, y=120, w=30, h=30),
      8: dict(sx=60, sy=60, x=270, y=120, w=30, h=30)})
# 段 3：repeat は領域の中央に最初の tile（30 + (240 − 30) / 2）
case("repeat：上下の辺の最初の tile は中央", 90, 90, (30, 30, 30, 30), (0, 0, 300, 150), (30, 30, 30, 30), REPEAT, STRETCH, False,
     {0, 1, 2, 3, 5, 6, 7, 8},
     {1: dict(tw=30, th=30, ox=135, oy=0, gx=0), 7: dict(tw=30, ox=135, y=120), 3: dict(th=90, oy=30)})
# 段 2：round は整数個が入る幅に（250 / round(250 / 30) = 250 / 8）
case("round：幅 250 に 8 個", 90, 90, (30, 30, 30, 30), (0, 0, 310, 150), (30, 30, 30, 30), ROUND, ROUND, False,
     {0, 1, 2, 3, 5, 6, 7, 8},
     {1: dict(w=250, tw=31.25, th=30, ox=30), 3: dict(h=90, tw=30, th=30, oy=30)})
# 段 4：space は入るだけ（floor(250 / 30) = 8）並べ、余り 10 を 9 つの隙間に
case("space：8 個と 9 つの等しい隙間", 90, 90, (30, 30, 30, 30), (0, 0, 310, 150), (30, 30, 30, 30), SPACE, STRETCH, False,
     {0, 1, 2, 3, 5, 6, 7, 8},
     {1: dict(w=250, tw=30, gx=10 / 9, ox=30 + 10 / 9), 7: dict(tw=30, gx=10 / 9, ox=30 + 10 / 9)})
case("space：一つも入らない辺は描かない", 90, 90, (30, 30, 30, 30), (0, 0, 80, 150), (30, 30, 30, 30), SPACE, STRETCH, False,
     {0, 2, 3, 5, 6, 8}, {1: dict(w=20)})
# §6.3：向かい合う幅の和が領域を越えるとき、全部を同じ比 f = min(150 / 200, 150 / 200) で縮める
case("幅の重なりを比で縮める", 90, 90, (30, 30, 30, 30), (0, 0, 150, 150), (100, 100, 100, 100), STRETCH, STRETCH, False,
     {0, 2, 6, 8},
     {0: dict(w=75, h=75, tw=75, th=75), 2: dict(x=75, w=75), 8: dict(x=75, y=75, w=75, h=75), 1: dict(w=0)})
# §6.2：左右の slice の和が画像の幅以上なら、上・中・下の部分は空。角はそれぞれの slice のまま（重なってよい）
case("左右の slice が画像の幅を越える", 90, 90, (30, 40, 30, 60), (0, 0, 300, 150), (30, 30, 30, 30), STRETCH, STRETCH, True,
     {0, 2, 3, 5, 6, 8},
     {0: dict(sx=0, sw=60), 2: dict(sx=50, sw=40)})
case("画像より大きい slice は 100%", 90, 90, (200, 200, 200, 200), (0, 0, 300, 150), (30, 30, 30, 30), STRETCH, STRETCH, True,
     {0, 2, 6, 8},
     {0: dict(sx=0, sy=0, sw=90, sh=90), 8: dict(sx=0, sy=0, sw=90, sh=90)})
# 段 1：中の部分は上の辺と同じ比で横に、左の辺と同じ比で縦に（fill のときだけ描く）
case("fill：中の tile は上と左の比", 90, 90, (30, 30, 30, 30), (0, 0, 300, 200), (60, 15, 60, 15), REPEAT, REPEAT, True,
     {0, 1, 2, 3, 4, 5, 6, 7, 8},
     {1: dict(x=15, w=270, tw=60, th=60, ox=120),
      3: dict(y=60, h=80, tw=15, th=15, oy=92.5),
      4: dict(sx=30, sy=30, sw=30, sh=30, x=15, y=60, w=270, h=80, tw=60, th=15, ox=120, oy=92.5)})
# 上の比が 0（上の slice が 0）なら中は下の比を使う
case("上の比が 0 なら中は下の比", 90, 90, (0, 30, 30, 30), (0, 0, 300, 200), (10, 30, 60, 30), REPEAT, REPEAT, True,
     {3, 4, 5, 6, 7, 8},
     {4: dict(sx=30, sy=0, sw=30, sh=60, x=30, y=10, w=240, h=130, tw=60, th=60, ox=120, oy=45),
      7: dict(tw=60, th=60, ox=120)})

lines = [" ".join(repr(float(x)) if isinstance(x, float) else str(x) for x in c[1]) for c in cases]
answers = subprocess.run([str(exe)], input="\n".join(lines) + "\n", text=True, capture_output=True, check=True,
                         cwd=root, env=env).stdout.strip().split("\n")
verbose = "--verbose" in sys.argv
checks = failures = 0
for (what, _, shown, parts), answer in zip(cases, answers):
    fields = answer.split(" | ")
    bits = int(fields[0])
    got_shown = {i for i in range(9) if bits >> i & 1}
    checks += 1
    bad = []
    if got_shown != shown:
        bad.append(f"描く部分 {sorted(got_shown)}、期待 {sorted(shown)}")
    for i, want in parts.items():
        got = dict(zip(FIELDS, map(float, fields[1 + i].split())))
        for key, value in want.items():
            checks += 1
            if abs(got[key] - value) > 1e-3:
                bad.append(f"{names[i]}の {key} = {got[key]:g}、期待 {value:g}")
    failures += len(bad)
    print(f"{what}: {'合格' if not bad else '不合格'}")
    for line in bad if verbose or bad else []:
        print("    " + line)
if len(answers) != len(cases):
    sys.exit(f"道具の答えが {len(answers)} 行、試験は {len(cases)} 件")
print(f"nine-slice: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
