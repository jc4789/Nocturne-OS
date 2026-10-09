"""次25だけ初回実行。原23 suite／fault条件は含めない。"""
from pathlib import Path
import re,subprocess,hashlib,datetime,os
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/worker-clone25-new-once';out.mkdir(parents=True,exist_ok=True)
base=root/'user/libc/web';source=(base/'js_worker_runtime.js').read_text(encoding='utf-8')
source=re.sub(r'/\* @include ([a-z_]+\.js) \*/',lambda m:(base/m[1]).read_text(encoding='utf-8'),source)
(out/'bootstrap.js').write_text(source,encoding='utf-8',newline='\n')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[str(root/f'third_party/quickjs/{name}.c') for name in ['quickjs','dtoa','libregexp','libunicode','cutils']]
binary=out/'worker-clone25-new.exe'
command=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(root/'tests/worker_clone25_new.c'),*engine,'-lm','-o',str(binary)]
with (out/'result.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for f in ['user/libc/web/js_worker_runtime.js','user/libc/web/js_clone.js','user/libc/web/js_worker_messaging.js','tests/worker_clone25_new.js','tests/worker_clone25_new.c','tests/worker_messaging23_new.c','third_party/quickjs/quickjs.c']:
        log.write(f+' SHA256 '+hashlib.sha256((root/f).read_bytes()).hexdigest()+'\n')
    log.write('COMPILE '+repr(command)+'\n');log.flush()
    r=subprocess.run(command,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('COMPILE_EXIT '+str(r.returncode)+'\n');log.flush()
    if r.returncode:raise SystemExit(r.returncode)
    r=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60);log.write('RUNTIME_EXIT '+str(r.returncode)+'\n');raise SystemExit(r.returncode)
