"""Mesaの既存build設定で実同期ソースをホスト検査する。本文の生成は行わない。"""
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ)
env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
mesa = (root / "ports/mesa/mesa.mk").read_text(encoding="utf-8").replace("\\\n", " ")
nvk = (root / "ports/mesa/nvk.mk").read_text(encoding="utf-8").replace("\\\n", " ")
def words(text, name):
    return re.search(r"^" + name + r" := (.+)$", text, re.MULTILINE).group(1).split()
dirs = words(mesa, "MESA_DIRS") + words(nvk, "NVK_DIRS")
defines = words(mesa, "MESA_DEFS")
defines = [d.replace('\\"', '"') for d in defines]
common = [str(cc), "-std=gnu11", "-O2", "-ffreestanding", "-ffunction-sections", "-fdata-sections",
           "-U_WIN32", "-U_WIN64", *defines,
           *("-Ithird_party/mesa/" + d for d in dirs),
           *("-Ibuild/mesa/gen/" + d for d in dirs if d != "include"),
           "-Iuser/include", "-Icommon", "-Wl,--gc-sections"]
for name, prefix, extras in [
    ("nvk_sync", [], ["third_party/mesa/src/util/u_dynarray.c", "third_party/mesa/src/util/ralloc.c"]),
    ("mesa_screen", [], ["-DGALLIUM_ZINK", "-Iports/mesa", "-Iports/nvidia/include",
                     "-Ithird_party/nvidia-open/src/common/sdk/nvidia/inc"]),
    ("nvk_rm", ["-Iports/nvidia/include", "-Ithird_party/nvidia-open/src/common/sdk/nvidia/inc",
                "-Ithird_party/nvidia-open/src/nvidia/arch/nvalloc/unix/include"], []),
    # 製品のソースをそれぞれ一つの翻訳単位として（zink_instance.cはgenerate.pyの生成物、全体ビルドと同じもの）
    ("zink_start", [], ["-DGALLIUM_ZINK", "-DMESA_VK_LOG=0", "-Ithird_party/mesa/src/gallium/drivers/zink",
                         "-Ibuild/mesa/gen/src/gallium/drivers/zink", "-Ithird_party/mesa/src/vulkan/util",
                         "-Ibuild/mesa/gen/src/vulkan/util", "third_party/mesa/src/util/u_dl.c",
                         "third_party/mesa/src/util/log.c", "third_party/mesa/src/vulkan/runtime/vk_log.c",
                         "build/mesa/gen/src/gallium/drivers/zink/zink_instance.c",
                         "build/mesa/gen/src/vulkan/util/vk_enum_to_str.c", "third_party/mesa/src/util/ralloc.c"]),
]:
    exe = root / ("build/" + name.replace("_", "-") + "-hosttest.exe")
    result = subprocess.run([common[0], *prefix, *common[1:], "tests/" + name + "_hosttest.c", *extras, "-o", str(exe)], cwd=root, env=env)
    if result.returncode:
        sys.exit(result.returncode)
    result = subprocess.run([str(exe)], cwd=root, env=env)
    if result.returncode:
        sys.exit(result.returncode)
