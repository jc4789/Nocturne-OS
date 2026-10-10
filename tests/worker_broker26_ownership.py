"""親許可の非重複native ownership境界、初回一度。原fixtureを再走しない。"""
from pathlib import Path
import subprocess,datetime,hashlib,os
root=Path(__file__).resolve().parent.parent;out=root/'build/goal-20261009/worker-broker26-ownership-once';out.mkdir(parents=True,exist_ok=True)
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[str(root/f'third_party/quickjs/{name}.c') for name in ['quickjs','dtoa','libregexp','libunicode','cutils']];binary=out/'ownership.exe'
cmd=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(root/'tests/worker_broker26_ownership.c'),*engine,'-lm','-o',str(binary)]
with (out/'result.log').open('x',encoding='utf-8') as log:
 log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n');log.write('ENGINE_SHA256 '+hashlib.sha256((root/'third_party/quickjs/quickjs.c').read_bytes()).hexdigest()+'\n');log.write('COMPILE '+repr(cmd)+'\n');log.flush();r=subprocess.run(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT);log.write('COMPILE_EXIT '+str(r.returncode)+'\n');log.flush()
 if r.returncode:raise SystemExit(r.returncode)
 r=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=60);log.write('RUNTIME_EXIT '+str(r.returncode)+'\n');raise SystemExit(r.returncode)
