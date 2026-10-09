"""First-only new fixed/sticky/normal-flow opacity stacking boundaries."""
from pathlib import Path
import hashlib,subprocess
source=Path('user/libc/web/paint.c').read_text(encoding='utf-8')
def function(name):
    at=source.index(name);return source[at:source.index('\n}',at)+2]+'\n'
helpers=source[source.index('static bool positioned('):source.index('static float cy(')]
helpers+=function('static int layer_z(')+function('static void collect_layers(')
out=Path('build/goal-20261009/paint-stacking-new-native.c')
out.write_text('#include <stdio.h>\n#include <setjmp.h>\n#undef SYS_OPEN\n#undef RGB\n#undef TRANSPARENT\n#undef TA_LEFT\n#undef TA_RIGHT\n#undef TA_CENTER\n#undef BS_SOLID\n#include "webi.h"\nstatic bool web_dialog_layer_box(web_doc*d,const box_t*b){(void)d;(void)b;return false;}\nvoid pv_push(pvec*p,void*v){if(p->n==p->cap){p->cap=p->cap?p->cap*2:8;p->v=realloc(p->v,(size_t)p->cap*sizeof*p->v);if(!p->v)abort();}p->v[p->n++]=v;}\n'+helpers+Path('tests/paint_stacking_new.c').read_text(encoding='utf-8'),encoding='utf-8',newline='\n')
for name in ['user/libc/web/paint.c','user/libc/web/paint_media_debug.h','user/libc/web/webi.h',str(out)]:
    print(name,'SHA256',hashlib.sha256(Path(name).read_bytes()).hexdigest(),flush=True)
exe='build/goal-20261009/paint-stacking-new-native.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True,timeout=20)
