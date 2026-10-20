"""NVIDIA OS層の実コードを検査する。GPU・QEMU・実機は実行しない。"""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ)
env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
# ヘッダー一覧を試験側に複製せず、正規ビルドの一覧を使う。
makefile = (root / "ports/nvidia/nvrm.mk").read_text(encoding="utf-8").replace("\\\n", " ")
include_dirs = re.search(r"^NVRM_INC := (.+)$", makefile, re.MULTILINE).group(1).split()
defines = re.search(r"^NVRM_DEFS := (.+)$", makefile, re.MULTILINE).group(1).split()
vendor = root / "third_party/nvidia-open/src/nvidia"
exe = root / "build/nvrm-dma-hosttest.exe"
command = [
    str(cc), "-std=gnu11", "-O2", "-ffreestanding", "-ffunction-sections", "-fdata-sections",
    *defines, "-DNVRM_NOCTURNE", "-include",
    str(vendor / "../common/sdk/nvidia/inc/cpuopsys.h"),
    *("-I" + str(vendor / d) for d in include_dirs),
    "-Iports/nvidia", "-Iports/nvidia/include", "-Iuser/include", "-Icommon",
    "-Wl,--gc-sections", "tests/nvrm_dma_hosttest.c", "-o", str(exe),
]
compiled = subprocess.run(command, cwd=root, env=env)
if compiled.returncode:
    sys.exit(compiled.returncode)
sys.exit(subprocess.run([str(exe)], cwd=root, env=env).returncode)
