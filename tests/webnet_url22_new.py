"""新22長URL IPCだけ、旧20/21のsuiteや実サイトは再走しない。"""
from pathlib import Path
import datetime, hashlib, os, subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/webnet-url22-new-once';out.mkdir(parents=True,exist_ok=True)
def part(s,start,end):
    a=s.index(start);return s[a:s.index(end,a)]
fetch=(root/'user/apps/webfetch.c').read_text(encoding='utf-8')
native=(root/'user/libc/webnet.c').read_text(encoding='utf-8')
(out/'webfetch-tested.inc').write_text(part(fetch,'struct header_buffer','static bool fail(')+part(fetch,'static bool fail(','static int hex(')+part(fetch,'static bool cors_kind(','int main(void)'),encoding='utf-8',newline='\n')
(out/'webnet-tested.inc').write_text(part(native,'#define WORKERS','static void load_cookie_psl(')+part(native,'static void slot_close(','webnet *webnet_create(')+part(native,'uint64_t webnet_submit(','static bool start(')+part(native,'struct cookie_scratch','void webnet_pump(')+native[native.index('static void cancel('):],encoding='utf-8',newline='\n')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
flags=['-std=gnu11','-O1','-Iuser/include','-Icommon','-include','tests/webnet_url22_alloc.h']
commands=[]
for source,label,extra in [('user/libc/http.c','http',['-Dhttp_request=unused_http_request','-Dhttp_request_limited=unused_http_request_limited']),('user/libc/web/util.c','util',[]),('user/libc/webcookie.c','cookie',[])]:
    commands.append([str(cc),*flags,*extra,'-c',source,'-o',str(out/(label+'.o'))])
binary=out/'webnet-url22-new.exe'
commands.append([str(cc),*flags,'tests/webnet_url22_new.c',str(out/'http.o'),str(out/'util.o'),str(out/'cookie.o'),'-o',str(binary)])
# The three prior logs stop before execution: ssize_t/ctype, missing MIN, then
# the host's different errno ABI. Preserve all; this is the first runtime.
with (out/'result-runtime-once.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for name in ['user/include/webnet.h','user/include/webnet_wire.h','user/include/web.h','user/libc/webnet.c','user/apps/webfetch.c','user/libc/webcookie.c','user/libc/web/util.c','tests/webnet_url22_new.c','tests/webnet_url22_new.py',str((out/'webfetch-tested.inc').relative_to(root)),str((out/'webnet-tested.inc').relative_to(root))]:log.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    for command in commands:
        log.write('COMPILE '+repr(command)+'\n');log.flush();r=subprocess.run(command,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('COMPILE_EXIT '+str(r.returncode)+'\n');log.flush()
        if r.returncode:raise SystemExit(r.returncode)
    r=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30)
    log.write('RUNTIME_EXIT '+str(r.returncode)+'\n');raise SystemExit(r.returncode)
