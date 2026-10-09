"""One new ephemeron boundary run against the actual product QuickJS."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "ephemeron-native.exe"
engine=[root / ("third_party/quickjs/"+n+".c") for n in ("quickjs","dtoa","libregexp","libunicode","cutils")]
subprocess.run([str(cc),"-std=gnu11","-O1","-fwrapv","-funsigned-char","-DCONFIG_NOCTURNE",'-DCONFIG_VERSION="Nocturne-test"',"-Ithird_party/quickjs","-Ibuild/nocturne-audit/url-host-include",str(root / "tests/ephemeron_native_cases.c"),*map(str,engine),"-lm","-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
