"""High Definition Audio の widget の木の読み（kernel/src/dev/hda_codec.c）を、表で作った codec で host で確かめる。

実物のコードをそのまま組み、tests/hda_codec_hosttest.c の表（複数の DAC と pin、mixer と selector、多チャンネルの
association、前面の headphone、S/PDIF、GPU の HDMI・DisplayPort の pin、閉路、形式の違い、答えない codec）で走らせる。
制御器（CORB・RIRB・stream）は通らない。

  python -X utf8 tests/check_hda_codec.py [--out 出力の場所]
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
# 作業木には tools/・build/ が無く、本体のものを使う
home = next(p for p in (root, *root.parents) if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else home / "build/check-hda-codec"
out.mkdir(parents=True, exist_ok=True)
exe = out / "hda_codec_hosttest.exe"
clang = home / "tools/msys64/ucrt64/bin/clang.exe"
built = subprocess.run([str(clang), "-std=c2x", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-fsanitize-trap=undefined", "-I", str(root / "kernel/src"),
                        str(root / "tests/hda_codec_hosttest.c"), str(root / "kernel/src/dev/hda_codec.c"),
                        "-o", str(exe)])
if built.returncode:
    sys.exit("組めない")
sys.exit(subprocess.run([str(exe)]).returncode)
