"""次21のheap設定＋実QuickJS allocation失敗だけ、初回一度の支持試験。"""
from pathlib import Path
import datetime, hashlib, os, subprocess

root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/worker-heap21-new-once'
out.mkdir(parents=True,exist_ok=True)
source=(root/'user/libc/web/js_worker.c').read_text(encoding='utf-8')
types=''.join(source[source.index('struct '+name+' {'):source.index('\n',source.index('struct '+name+' {'))]+'\n' for name in ['packet','child','event','web_workers'])
find=source[source.index('static struct child *find('):source.index('\n',source.index('static struct child *find('))]+'\n'
helpers=source[source.index('static bool console_prefix('):source.index('\nint64_t web_worker_deadline(')]
fixtures='''static void stop(web_workers *w,struct child *c){(void)w;c->stopped=true;}
void web_worker_loaded(web_workers*w,uint32_t id,uint32_t request,int status,const char*url,const char*mime,const void*body,size_t n,const char*error){(void)w;(void)id;(void)request;(void)status;(void)url;(void)mime;(void)body;(void)n;(void)error;}
'''
(out/'console-tested.inc').write_text(types+find+fixtures+helpers,encoding='utf-8',newline='\n')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
worker=(root/'user/apps/browserjsworker.c').read_text(encoding='utf-8')
assert 'JS_SetMemoryLimit(w.rt,NJW_HEAP_BYTES)' in worker
assert 'JS_SetMaxStackSize(w.rt,512u*1024u)' in worker
files=['user/include/js_worker_wire.h','user/libc/web/js_worker.c','user/apps/browserjsworker.c','tests/worker_heap21_new.c','third_party/quickjs/quickjs.c',str((out/'console-tested.inc').relative_to(root))]
engine=[str(root/f'third_party/quickjs/{name}.c') for name in ['quickjs','dtoa','libregexp','libunicode','cutils']]
binary=out/'worker-heap21-new.exe'
# The original compile stopped before runtime (Windows host lacks alloca.h).
# Retain result.log and use the existing host shim for the first actual run.
command=[str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(root/'tests/worker_heap21_new.c'),*engine,'-lm','-o',str(binary)]
with (out/'result-first-runtime.log').open('x',encoding='utf-8') as log:
    log.write('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
    for f in files:log.write(f+' SHA256 '+hashlib.sha256((root/f).read_bytes()).hexdigest()+'\n')
    log.write('COMPILE '+repr(command)+'\n');log.flush()
    result=subprocess.run(command,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT)
    log.write('COMPILE_EXIT '+str(result.returncode)+'\n');log.flush()
    if result.returncode:raise SystemExit(result.returncode)
    result=subprocess.run([str(binary)],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=30)
    log.write('RUNTIME_EXIT '+str(result.returncode)+'\n')
    raise SystemExit(result.returncode)
