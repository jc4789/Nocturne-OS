"""New-only source-path checks. No network, Worker OS execution or site acceptance."""
from pathlib import Path
import hashlib, subprocess

root=Path(__file__).resolve().parent.parent
output=root/'build/goal-20261009'
def function(path,signature):
    text=(root/path).read_text(encoding='utf-8')
    start=text.index(signature)
    line=text[start:text.index('\n',start)]
    if line.rstrip().endswith('}'):return line+'\n'
    return text[start:text.index('\n}',start)+2]+'\n'
def structure(path,name):
    text=(root/path).read_text(encoding='utf-8')
    start=text.index('struct '+name+' {')
    return text[start:text.index('};',start)+2]+'\n'

helpers=''.join(function(p,s) for p,s in [
    ('user/libc/http.c','bool url_parse('),
    ('user/libc/web/frame.c','const char *web_effective_url('),
    ('user/libc/web/js.c','static bool make_origin('),
    ('user/libc/web/js.c','static bool permitted_url('),
    ('user/libc/web/js.c','static bool worker_load('),
    ('user/apps/browser.c','static enum webnet_kind network_kind('),
    ('user/apps/browser.c','static bool host_request('),
    ('user/apps/webfetch.c','static int remaining('),
])
worker=''.join(function('user/libc/web/js_worker.c',s) for s in [
    'static bool queue_room(', 'static bool queue('
])
worker_types=''.join(structure('user/libc/web/js_worker.c',n) for n in ['packet','child','web_workers'])
source=(root/'tests/worker_origin17_new.c').read_text(encoding='utf-8')
source=source.replace('/* PRODUCT_TRANSFER */',structure('user/apps/browser.c','transfer'))
source=source.replace('/* PRODUCT_FUNCTIONS */',helpers).replace('/* PRODUCT_QUEUE_TYPES */',worker_types).replace('/* PRODUCT_QUEUE */',worker)
generated=output/'worker-origin17-new-native.c'
generated.write_text(source,encoding='utf-8',newline='\n')
for name in ['user/libc/web/js.c','user/libc/web/js_worker.c','user/include/js_worker_wire.h','user/include/webnet.h','user/apps/webfetch.c','user/apps/browserjsworker.c',str(generated)]:
    print(name,'SHA256',hashlib.sha256((root/name).read_bytes()).hexdigest(),flush=True)
compiler=root/'tools/msys64/ucrt64/bin/clang.exe'
binary=output/'worker-origin17-new-native.exe'
subprocess.run([str(compiler),'-std=gnu11','-O1','-Iuser/include','-Icommon',str(generated),'-o',str(binary)],cwd=root,check=True)
subprocess.run([str(binary)],cwd=root,check=True,timeout=20)
