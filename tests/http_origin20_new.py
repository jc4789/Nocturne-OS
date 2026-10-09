"""First-only new owned-origin, resolver-authority and webfetch flow cases."""
from pathlib import Path
import hashlib,subprocess,datetime
root=Path(__file__).resolve().parent.parent
source=(root/'user/apps/webfetch.c').read_text(encoding='utf-8')
def part(start,end):
    at=source.index(start)
    return source[at:source.index(end,at)]
inc=root/'build/goal-20261009/http-origin20-webfetch-tested.inc'
inc.write_text(part('struct header_buffer','static bool read_all')+
    part('static bool fail(','static int hex(')+
    part('static bool cors_kind(','int main(void)'),encoding='utf-8',newline='\n')
log=root/'build/goal-20261009/http-origin20-new-once.log'
with log.open('x',encoding='utf-8') as out:
    out.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for name in ['user/include/http.h','user/libc/http.c','user/apps/webfetch.c','user/libc/web/util.c','tests/http_origin20_new.c',str(inc.relative_to(root))]:
        out.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    out.flush()
    exe=root/'build/goal-20261009/http-origin20-new-native.exe'
    cmd=[str(root/'tools/msys64/ucrt64/bin/clang.exe'),'-std=gnu11','-O1','-Iuser/include','-Icommon','tests/http_origin20_new.c','-o',str(exe)]
    out.write('COMPILE '+repr(cmd)+'\n');out.flush()
    compiled=subprocess.run(cmd,cwd=root,stdout=out,stderr=subprocess.STDOUT)
    out.write('COMPILE_EXIT '+str(compiled.returncode)+'\n');out.flush()
    if compiled.returncode:raise SystemExit(compiled.returncode)
    run=subprocess.run([str(exe)],cwd=root,stdout=out,stderr=subprocess.STDOUT,timeout=20)
    out.write('RUNTIME_EXIT '+str(run.returncode)+'\n')
    raise SystemExit(run.returncode)
