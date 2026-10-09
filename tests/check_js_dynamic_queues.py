"""Only the new dynamic queue/argument boundary, one runtime execution."""
from pathlib import Path
import hashlib
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / 'build/goal-20261009'
source=(root / 'user/libc/web/js.c').read_text(encoding='utf-8')
helpers=source[source.index('static void release_timer('):source.index('static JSValue native_css_supports(')]
helpers+=source[source.index('static JSValue native_clear('):source.index('static JSValue microtask_job(')]
helpers+=source[source.index('static struct js_pending *pending_new('):source.index('static void pending_error(')]
rejections=source[source.index('static void promise_rejection('):source.index('static node_t *unwrap(')]
helpers+=rejections.replace('js_malloc_rt(s->rt,sizeof *p)','test_rejection_allocate(s->rt,sizeof *p)')
for name,end in [('run_timer','run_posted_task'),('run_posted_task','observers_dirty')]:
    start=source.index('static bool '+name+'(')
    section=source[start:]
    helpers+=section[:section.index('    uint64_t elapsed =')]+ 'return true;\n}\n'
at=source.index('        struct js_rejection *rejected=s->rejections;') if '        struct js_rejection *rejected=s->rejections;' in source else source.index('        struct js_rejection *rejected=s->rejections,*batch_end=')
batch=source[at:source.index('        if (s->timed_out)',at)]
helpers+='static void report_rejections(struct web_js_state *s){\n'+batch+'}\n'
prefix=(root / 'tests/js_dynamic_queues_prefix.c').read_text(encoding='utf-8')
c=out / 'js-dynamic-queues-generated.c';c.write_text(prefix.replace('/* ACTUAL_QUEUE_FUNCTIONS */',helpers),encoding='utf-8')
# Preserve an exact copy of the sole included product header used for the run.
header=(root / 'user/libc/web/js_broadcast.h').read_bytes()
(out / 'js-broadcast-tested-dynamic.h').write_bytes(header)
c.write_text(c.read_text(encoding='utf-8').replace('#include "js_broadcast.h"','#include "js-broadcast-tested-dynamic.h"'),encoding='utf-8')
cc=root / 'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[];log=out / 'js-dynamic-queues-once.log'
with log.open('w',encoding='utf-8') as f:
    f.write('generated SHA256 '+hashlib.sha256(c.read_bytes()).hexdigest()+'\n')
    f.write('broadcast SHA256 '+hashlib.sha256(header).hexdigest()+'\n')
    for name in ['quickjs','dtoa','libregexp','libunicode','cutils']:
        data=(root / 'third_party/quickjs' / (name+'.c')).read_bytes();copy=out / ('dynamic-engine-'+name+'.c');copy.write_bytes(data);engine.append(copy)
        f.write(name+' SHA256 '+hashlib.sha256(data).hexdigest()+'\n')
    f.flush();exe=out / 'js-dynamic-queues.exe'
    result=subprocess.run([str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(c),*map(str,engine),'-lm','-o',str(exe)],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
    if result.returncode==0:result=subprocess.run([str(exe)],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=30)
    f.write('exit '+str(result.returncode)+'\n')
print(log.read_text(encoding='utf-8'))
