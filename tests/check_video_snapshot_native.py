"""One new compositor boundary run; no former media/YouTube suites."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
source=(root / "user/libc/web/avmedia.c").read_text(encoding="utf-8")
draw=(root / "user/libc/media_draw.c").read_text(encoding="utf-8")
gfx=(root / "common/gfx.c").read_text(encoding="utf-8")
def function(text,signature):
    at=text.index(signature)
    first=text[at:text.index("\n",at)]
    if first.endswith("}"):
        return first+"\n"
    return text[at:text.index("\n}",at)+2]+"\n"
at=source.index("struct web_avmedia {")
structure=source[at:source.index("\n};",at)+3]
funcs=""
for sig in ("static void snapshot_clear(","static void snapshot_publish(","void web_avmedia_snapshot_begin(","void web_avmedia_snapshot_reject(","bool web_avmedia_snapshot_prepare(","void web_avmedia_snapshot_probe(","void web_avmedia_snapshot_finish("):
    funcs+=function(source,sig)
drawfunc=function(draw,"void nmedia_draw(")
gfxfunc="".join(function(gfx,s) for s in ("void gfx_init(","void gfx_noclip(","void gfx_fill("))
cases=(root / "tests/video_snapshot_native_cases.c").read_text(encoding="utf-8")
c=out / "video-snapshot-native-generated.c"
c.write_text(cases.replace("/* PRODUCT_STRUCT */",structure).replace("/* PRODUCT_FUNCTIONS */",gfxfunc+drawfunc+funcs),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "video-snapshot-native.exe"
subprocess.run([str(cc),"-std=gnu11","-O1","-Icommon","-Iuser/libc/web",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=15)
