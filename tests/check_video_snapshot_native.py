"""One new compositor boundary run; no former media/YouTube suites."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
out.mkdir(parents=True,exist_ok=True)
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
# The snapshot's own state: candidate, probe phase, rejection and the blend regions.
at=source.index("static struct web_avmedia *snapshot_candidate;")
funcs=source[at:source.index("static void snapshot_clear(",at)]
for sig in ("void web_avmedia_enter(","void web_avmedia_leave("):
    funcs+=function(source,sig)
at=source.index("enum { SNAPSHOT_BAND_ROWS")
funcs+=source[at:source.index("static void snapshot_band(",at)]
for sig in ("static void snapshot_band(","static void snapshot_draw(","static void snapshot_clear(","static bool snapshot_publishable(","static void snapshot_publish(","void web_avmedia_snapshot_begin(","void web_avmedia_snapshot_reject(","bool web_avmedia_snapshot_prepare(","static size_t snapshot_band_row(","void web_avmedia_snapshot_probe(","void web_avmedia_snapshot_finish("):
    funcs+=function(source,sig)
drawfunc="".join(function(draw,s) for s in ("static int64_t centre_position(","static uint32_t blend(","void nmedia_draw("))
gfxfunc="".join(function(gfx,s) for s in ("uint8_t gfx_clip_alpha(","int gfx_clip_row(","void gfx_init(","void gfx_noclip(","void gfx_pixel(","void gfx_fill("))
cases=(root / "tests/video_snapshot_native_cases.c").read_text(encoding="utf-8")
c=out / "video-snapshot-native-generated.c"
c.write_text(cases.replace("/* PRODUCT_STRUCT */",structure).replace("/* PRODUCT_FUNCTIONS */",gfxfunc+drawfunc+funcs),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "video-snapshot-native.exe"
# The host C library's setjmp.h and time.h, not the product's.
abi=out / "host-abi"
abi.mkdir(exist_ok=True)
for name in ("setjmp.h","time.h"):
    (abi / name).write_text('#include "' + (cc.parent.parent / "include" / name).as_posix() + '"\n', encoding="utf-8")
subprocess.run([str(cc),"-std=gnu11","-O1","-Wno-incompatible-library-redeclaration","-I"+str(abi),"-Icommon","-Iuser/include","-Iuser/libc","-Iuser/libc/web","-Ithird_party/quickjs",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=15)
