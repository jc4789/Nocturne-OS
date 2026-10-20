"""NVIDIA OS層の実コードを検査する。GPU・QEMU・実機は実行しない。"""
import os
import argparse
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", default="build")
options = parser.parse_args()
output = root / options.output_dir
output.mkdir(parents=True, exist_ok=True)
base_command = [
    str(cc), "-std=gnu11", "-O2", "-ffreestanding", "-ffunction-sections", "-fdata-sections",
    *defines, "-DNVRM_NOCTURNE", "-include",
    str(vendor / "../common/sdk/nvidia/inc/cpuopsys.h"),
    *("-I" + str(vendor / d) for d in include_dirs),
    "-Iports/nvidia", "-Iports/nvidia/include", "-Iuser/include", "-Icommon",
]
# Inclusion-based fixtures can accidentally supply a dependency from another
# production .c file. Check each real translation unit on the target as well.
for source in ("os.c", "os_pci.c", "os_stubs.c", "rm_api.c", "nvrm.c", "nvrm_serve.c", "libnvrm.c"):
    checked = subprocess.run([*base_command, "--target=x86_64-unknown-none-elf", "-fsyntax-only",
                              "ports/nvidia/" + source], cwd=root, env=env)
    if checked.returncode:
        sys.exit(checked.returncode)
for case in ("nvrm_dma_hosttest", "nvrm_contract_hosttest", "nvrm_transport_hosttest"):
    exe = output / (case + ".exe")
    compiled = subprocess.run([*base_command, "-Wl,--gc-sections", "tests/" + case + ".c", "-o", str(exe)], cwd=root, env=env)
    if compiled.returncode:
        sys.exit(compiled.returncode)
    result = subprocess.run([str(exe)], cwd=root, env=env)
    if result.returncode:
        sys.exit(result.returncode)
