"""Only new image-upgrade conditions; old origin20 main is never included."""
from pathlib import Path
import hashlib,subprocess,datetime,sys
root=Path(__file__).resolve().parent.parent
def piece(source,start,end):
    at=source.index(start)
    return source[at:source.index(end,at)]
fetch=(root/'user/apps/webfetch.c').read_text(encoding='utf-8')
native=(root/'user/libc/webnet.c').read_text(encoding='utf-8')
fetch_inc=root/'build/goal-20261009/image-upgrade20-webfetch-tested.inc'
fetch_inc.write_text(piece(fetch,'struct header_buffer','static bool read_all')+piece(fetch,'static bool fail(','static int hex(')+piece(fetch,'static bool cors_kind(','int main(void)'),encoding='utf-8')
native_inc=root/'build/goal-20261009/image-upgrade20-webnet-tested.inc'
native_inc.write_text(piece(native,'#define WORKERS','static void load_cookie_psl(')+piece(native,'uint64_t webnet_submit(','static bool start(')+piece(native,'static bool unpack(','void webnet_pump('),encoding='utf-8')
original=(root/'tests/http_origin20_new.c').read_text(encoding='utf-8')
prefix=original[:original.index('int main(void)')]
prefix=prefix[:prefix.index('static int fixture_request(')]+prefix[prefix.index('int http_request(const'):]
prefix=prefix.replace('static uint64_t uptime_ms', 'static char *count_strdup(const char*p){size_t n=strlen(p)+1;char*q=count_malloc(n);if(q)memcpy(q,p,n);return q;}\nstatic int fixture_request(const struct http_req*,struct http_resp*);\nstatic uint64_t uptime_ms')
prefix=prefix.replace('#define malloc count_malloc','#define strdup count_strdup\n#define malloc count_malloc')
prefix=prefix.replace('http-origin20-webfetch-tested.inc','image-upgrade20-webfetch-tested.inc')
prefix=prefix.replace('#undef malloc','#include "image-upgrade20-webnet-tested.inc"\n#undef strdup\n#undef malloc')
prefix=prefix.replace('#include "../user/libc/http.c"','#include "../../user/libc/http.c"').replace('#include "../user/libc/web/util.c"','#include "../../user/libc/web/util.c"')
prefix=prefix.replace('#include "../build/goal-20261009/image-upgrade20-webfetch-tested.inc"','#include "image-upgrade20-webfetch-tested.inc"')
test=root/'build/goal-20261009/image-upgrade20-new.c'
test.write_text(prefix+(root/'tests/image_upgrade20_new_main.inc').read_text(encoding='utf-8'),encoding='utf-8')
exe=root/'build/goal-20261009/image-upgrade20-new-native.exe'
cmd=[str(root/'tools/msys64/ucrt64/bin/clang.exe'),'-std=gnu11','-O1','-Iuser/include','-Icommon',str(test),'-o',str(exe)]
if '--compile-only' in sys.argv:
    raise SystemExit(subprocess.run(cmd,cwd=root).returncode)
with (root/'build/goal-20261009/image-upgrade20-new-once.log').open('x',encoding='utf-8') as out:
    out.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    out.write('Old origin20 fixture declarations reused; no old main/assertions included.\n')
    for path in ['user/include/webnet.h','user/include/webnet_wire.h','user/libc/webnet.c','user/apps/webfetch.c','user/libc/http.c','user/libc/web/util.c',str(fetch_inc.relative_to(root)),str(native_inc.relative_to(root)),str(test.relative_to(root)),'tests/image_upgrade20_new_main.inc','tests/image_upgrade20_new.py']:
        out.write(path+' SHA256 '+hashlib.sha256((root/path).read_bytes()).hexdigest()+'\n')
    out.write('COMPILE '+repr(cmd)+'\n');out.flush()
    compiled=subprocess.run(cmd,cwd=root,stdout=out,stderr=subprocess.STDOUT);out.write('COMPILE_EXIT '+str(compiled.returncode)+'\n');out.flush()
    if compiled.returncode:raise SystemExit(compiled.returncode)
    result=subprocess.run([str(exe)],cwd=root,stdout=out,stderr=subprocess.STDOUT,timeout=20)
    out.write('RUNTIME_EXIT '+str(result.returncode)+'\n');raise SystemExit(result.returncode)
