"""新21 owned 16KiB超の初回だけ、旧19/20は実行しない。"""
from pathlib import Path
import datetime, hashlib, os, subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/http-owned-span21-new-once'
out.mkdir(parents=True,exist_ok=True)
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
binary=out/'http-owned-span21-new.exe'
command=[str(cc),'-std=gnu11','-O1','-Iuser/include','tests/http_owned_span21_new.c','-o',str(binary)]
with (out/'result.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for name in ['user/include/http.h','user/libc/http.c','tests/http_owned_span21_new.c']:
        log.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    log.write('COMPILE '+repr(command)+'\n');log.flush()
    result=subprocess.run(command,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
    log.write('COMPILE_EXIT '+str(result.returncode)+'\n');log.flush()
    if result.returncode:raise SystemExit(result.returncode)
    result=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=20)
    log.write('RUNTIME_EXIT '+str(result.returncode)+'\n')
    raise SystemExit(result.returncode)
