"""New native weak identity/GC edges only; not an OS or site acceptance test."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
source=(root / "user/libc/web/js.c").read_text(encoding="utf-8")
types=source[source.index("#define JS_NODE_BUCKETS"):source.index("struct js_timer")]
brand=source[source.index("static node_t *unwrap("):source.index("static bool unknown_html_interface(")]
lookup=source[source.index("static unsigned node_bucket("):source.index('#include "js_nodes.h"')]
cases=(root / "tests/node_cache_native_cases.c").read_text(encoding="utf-8")
c=out / "node-cache-native-generated.c"
c.write_text(cases.replace("/* PRODUCT_CACHE_TYPES */",types).replace("/* PRODUCT_BRAND_HELPERS */",brand).replace("/* PRODUCT_CACHE_LOOKUP */",lookup),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "node-cache-native.exe"
engine=[root / ("third_party/quickjs/"+n+".c") for n in ("quickjs","dtoa","libregexp","libunicode","cutils")]
subprocess.run([str(cc),"-std=gnu11","-O1","-fwrapv","-funsigned-char","-DCONFIG_NOCTURNE",'-DCONFIG_VERSION="Nocturne-test"',"-Ithird_party/quickjs","-Ibuild/nocturne-audit/url-host-include",str(c),*map(str,engine),"-lm","-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
