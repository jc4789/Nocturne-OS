"""One new uncapped native forest run, never any former frame suite."""
from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
source=(root / "user/libc/web/frame.c").read_text(encoding="utf-8")
header=(root / "user/libc/web/frame.h").read_text(encoding="utf-8")
def function(text,sig):
    at=text.index(sig);return text[at:text.index("\n}",at)+2]+"\n"
at=header.index("struct web_frame {")
structure=header[at:header.index("\n};",at)+3]
functions="".join(function(source,s) for s in ("const char *web_effective_url(","bool web_frame_element(","node_t *web_frame_dom_next(","struct web_frame *web_frame_walk_next(","int web_frame_ancestor_url(","struct web_frame *web_frame_find(","struct web_frame *web_frame_ensure(","void web_frame_retire(","bool web_frame_commit(","void web_frames_tick(","int64_t web_frames_deadline(","void web_frames_loaded(","void web_frames_free(","static bool frame_surface_prepare(","void web_frames_prepare_paint(","void web_frames_finish_paint(","bool web_frame_paint("))
address=(root / "user/libc/web/frame_address.h").read_text(encoding="utf-8")
parse=function((root / "user/libc/http.c").read_text(encoding="utf-8"),"bool url_parse(")
cases=(root / "tests/frame_unbounded_native_cases.c").read_text(encoding="utf-8")
c=out / "frame-unbounded-native-generated.c"
c.write_text(cases.replace("/* PRODUCT_STRUCT */",structure).replace("/* PRODUCT_FUNCTIONS */",parse+functions+address),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "frame-unbounded-native.exe"
subprocess.run([str(cc),"-std=gnu11","-O1","-Icommon",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
