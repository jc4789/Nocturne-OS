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
# libspdm's cryptography backend also takes libspdm's headers and configuration (NVIDIA's
# openspdm.mk lists them, as for the build), and BearSSL's.
openspdm = (vendor / "src/libraries/libspdm/nvidia/openspdm.mk").read_text(encoding="utf-8")
spdm_version = re.search(r"^LIBSPDM_VERSION\s*:= (\S+)$", openspdm, re.MULTILINE).group(1)
spdm_includes = re.findall(r"^LIBSPDM_INCLUDES \+= \$\(LIBSPDM_SOURCE_DIR\)/(\S+)$", openspdm, re.MULTILINE)
spdm_command = [*base_command, "-Ithird_party/bearssl/inc",
                *("-I" + str(vendor / "src/libraries/libspdm" / d.replace("$(LIBSPDM_VERSION)", spdm_version))
                  for d in spdm_includes),
                "-DLIBSPDM_CONFIG=<nvspdm_rmconfig.h>"]
# Inclusion-based fixtures can accidentally supply a dependency from another
# production .c file. Check each real translation unit on the target as well.
for source in ("os.c", "os_pci.c", "os_stubs.c", "os_caps.c", "os_acpi.c", "spdm_crypto.c", "os_ioctl.c",
               "rm_api.c", "nvrm.c", "nvrm_serve.c", "libnvrm.c"):
    command = spdm_command if source == "spdm_crypto.c" else base_command
    checked = subprocess.run([*command, "--target=x86_64-unknown-none-elf", "-fsyntax-only",
                              "ports/nvidia/" + source], cwd=root, env=env)
    if checked.returncode:
        sys.exit(checked.returncode)
for case in ("nvrm_dma_hosttest", "nvrm_contract_hosttest", "nvrm_transport_hosttest", "nvrm_ioctl_hosttest"):
    exe = output / (case + ".exe")
    compiled = subprocess.run([*base_command, "-Wl,--gc-sections", "tests/" + case + ".c", "-o", str(exe)], cwd=root, env=env)
    if compiled.returncode:
        sys.exit(compiled.returncode)
    result = subprocess.run([str(exe)], cwd=root, env=env)
    if result.returncode:
        sys.exit(result.returncode)
# The cryptography against published test vectors, linked with BearSSL built for the host.
crypto_objects = []
for source in ("ports/nvidia/spdm_crypto.c", "tests/nvrm_spdm_crypto_hosttest.c"):
    obj = output / (Path(source).stem + ".host.o")
    compiled = subprocess.run([*spdm_command, "-c", source, "-o", str(obj)], cwd=root, env=env)
    if compiled.returncode:
        sys.exit(compiled.returncode)
    crypto_objects.append(str(obj))
bearssl_sources = sorted(str(p.relative_to(root)) for p in (root / "third_party/bearssl/src").rglob("*.c"))
exe = output / "nvrm_spdm_crypto_hosttest.exe"
compiled = subprocess.run([str(cc), "-O2", "-std=gnu11", "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src",
                           *bearssl_sources, *crypto_objects, "-o", str(exe)], cwd=root, env=env)
if compiled.returncode:
    sys.exit(compiled.returncode)
result = subprocess.run([str(exe)], cwd=root, env=env)
if result.returncode:
    sys.exit(result.returncode)
