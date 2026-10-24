"""nvrm の画面（ports/nvidia/display.c）を、偽の KAPI・RM・ハンドルで検査する。GPU・QEMU・実機は実行しない。"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ)
env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
# ヘッダーの一覧は正規ビルド（nvrm.mk）から読む。試験側に複製しない。
makefile = (root / "ports/nvidia/nvrm.mk").read_text(encoding="utf-8").replace("\\\n", " ")
include_dirs = re.search(r"^NVRM_INC := (.+)$", makefile, re.MULTILINE).group(1).split()
defines = re.search(r"^NVRM_DEFS := (.+)$", makefile, re.MULTILINE).group(1).split()
kapi_dirs = re.search(r"^\$\(NVRM_OUT\)/os/display\.o: NVRM_OS_CFLAGS \+= \$\(addprefix -I\$\(NVRM_VENDOR\)/src/,(.+?)\)$",
                      makefile, re.MULTILINE).group(1).split()
vendor = root / "third_party/nvidia-open/src"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", default="build")
options = parser.parse_args()
output = root / options.output_dir
output.mkdir(parents=True, exist_ok=True)
command = [
    str(cc), "-std=gnu11", "-O2", "-Wall", "-Wno-unused-function", "-ffreestanding", *defines, "-DNVRM_NOCTURNE",
    "-include", str(vendor / "common/sdk/nvidia/inc/cpuopsys.h"),
    *("-I" + str(vendor / "nvidia" / d) for d in include_dirs),
    *("-I" + str(vendor / d) for d in kapi_dirs),
    "-Iports/nvidia", "-Iports/nvidia/include", "-Iuser/include", "-Icommon",
]
# 製品の翻訳単位そのものも対象の設定で確かめる。
checked = subprocess.run([*command, "--target=x86_64-unknown-none-elf", "-fsyntax-only", "ports/nvidia/display.c"],
                         cwd=root, env=env)
if checked.returncode:
    sys.exit(checked.returncode)
exe = output / "nvrm_display_hosttest.exe"
compiled = subprocess.run([*command, "tests/nvrm_display_hosttest.c", "-o", str(exe)], cwd=root, env=env)
if compiled.returncode:
    sys.exit(compiled.returncode)
sys.exit(subprocess.run([str(exe)], cwd=root, env=env).returncode)
