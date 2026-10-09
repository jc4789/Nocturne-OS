"""First-only keepalive lifetime boundaries, using actual native transport/host helpers."""
from pathlib import Path
import hashlib
import subprocess

def read(name): return Path(name).read_text(encoding='utf-8')
def function(source, name):
    start = source.index(name)
    return source[start:source.index('\n}', start)+2]+'\n'
webnet=read('user/libc/webnet.c')
transport=webnet[:webnet.index('static void load_cookie_psl(')]
transport+=''.join(function(webnet,name) for name in ['static void slot_close(', 'static void slot_stop(', 'uint64_t webnet_submit('])
cookie_at=webnet.index('struct cookie_scratch {')
transport+=webnet[cookie_at:webnet.index('static bool cookie_events_inner(',cookie_at)]
transport+=''.join(function(webnet,name) for name in ['static bool cookie_events_inner(', 'static bool cookie_events(', 'static void complete(', 'static void cancel('])
transport+=webnet[webnet.index('void webnet_cancel(webnet *'):]
browser=read('user/apps/browser.c')
start=browser.index('struct transfer {')
helpers=browser[start:browser.index('static struct transfer *transfers;',start)]
helpers+='static struct transfer *transfers;\nstatic webnet *network;\nstatic web_doc *doc;\nstatic uint64_t generation;\nstatic bool quit,debug_js;\nstatic unsigned delivered;\n'
helpers+='''static void debug_image_result(const struct transfer*t,const struct webnet_response*r){(void)t;(void)r;}
static enum webnet_kind network_kind(int kind){(void)kind;return WEBNET_FETCH;}
static void response_copy(struct web_response*out,const struct webnet_response*in,enum webnet_kind k){(void)in;(void)k;memset(out,0,sizeof *out);}
void web_resource_loaded(web_doc*d,uint64_t id,const struct web_response*r){(void)d;(void)id;(void)r;delivered++;}
void web_response_free(struct web_response*r){(void)r;}
'''
helpers+=''.join(function(browser,name) for name in ['static void remove_transfer(', 'static void resource_completed(', 'static void host_cancel(', 'static void host_release_request(', 'static void cancel_document_requests('])
out=Path('build/goal-20261009/keepalive-new-native.c')
# Host CRT uses a different ssize_t spelling. The actual transport remains
# unchanged; only the unused native syscall prototype name is isolated here.
out.write_text('#include <stdio.h>\n#undef SYS_OPEN\n#define ssize_t nocturne_ssize_t\n#include "nocturne.h"\n#undef ssize_t\n#include "web.h"\n'+transport+'\n'+helpers+'\n'+read('tests/fetch_keepalive_lifetime_new.c'),encoding='utf-8',newline='\n')
for name in ['user/libc/webnet.c','user/include/webnet.h','user/include/web.h','user/libc/web/js.c','user/libc/web/js_fetch.js','user/libc/web/frame_native.h','user/apps/browser.c',str(out)]:
    print(name,'SHA256',hashlib.sha256(Path(name).read_bytes()).hexdigest(),flush=True)
exe='build/goal-20261009/keepalive-new-native.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-idirafter','user/include','-Icommon',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True,timeout=30)
