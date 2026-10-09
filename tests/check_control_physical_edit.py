"""One new physical native editing boundary. No prior suites are rerun."""
from pathlib import Path
import hashlib
import os
import subprocess

root=Path(__file__).resolve().parent.parent
out=root / 'build/goal-20261009'
doc=(root / 'user/libc/web/doc.c').read_text(encoding='utf-8')
utf=doc[doc.index('static uint32_t control_decode('):doc.index('static bool focus_under(')]
keys=doc[doc.index('static bool readonly('):doc.index('static node_t *form_id(')]
prefix=(root / 'tests/control_physical_edit_prefix.c').read_text(encoding='utf-8')
c=out / 'control-physical-edit-generated.c'
c.write_text(prefix.replace('/* ACTUAL_CONTROL_FUNCTIONS */',utf+'\n'+keys),encoding='utf-8')
cc=root / 'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out / 'control-physical-edit.exe'
log=out / 'control-physical-edit-once.log'
with log.open('w',encoding='utf-8') as f:
    for file in [c,root / 'user/libc/web/control_edit.h']:
        f.write(file.name+' SHA256 '+hashlib.sha256(file.read_bytes()).hexdigest()+'\n')
    f.flush()
    result=subprocess.run([str(cc),'-std=gnu11','-O1','-I.','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
    if result.returncode==0:
        result=subprocess.run([str(exe)],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=20)
    f.write('exit '+str(result.returncode)+'\n')
print(log.read_text(encoding='utf-8'))
