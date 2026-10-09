"""First-only new owned-hostname/DNS boundaries; never rerun old suites."""
from pathlib import Path
import hashlib,subprocess,datetime
root=Path(__file__).resolve().parent.parent
log=root/'build/goal-20261009/http-hostname19-new-once.log'
with log.open('x',encoding='utf-8') as out:
    out.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for name in ['user/include/http.h','user/libc/http.c','kernel/src/net/net.c','kernel/src/net/dns_name.h','tests/http_hostname19_new.c']:
        out.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    out.flush()
    exe=root/'build/goal-20261009/http-hostname19-new-native.exe'
    cmd=[str(root/'tools/msys64/ucrt64/bin/clang.exe'),'-std=gnu11','-O1','-Iuser/include','-Icommon','tests/http_hostname19_new.c','-o',str(exe)]
    out.write('COMPILE '+repr(cmd)+'\n');out.flush()
    compile_run=subprocess.run(cmd,cwd=root,stdout=out,stderr=subprocess.STDOUT)
    out.write('COMPILE_EXIT '+str(compile_run.returncode)+'\n');out.flush()
    if compile_run.returncode:raise SystemExit(compile_run.returncode)
    run=subprocess.run([str(exe)],cwd=root,stdout=out,stderr=subprocess.STDOUT,timeout=20)
    out.write('RUNTIME_EXIT '+str(run.returncode)+'\n')
    raise SystemExit(run.returncode)
