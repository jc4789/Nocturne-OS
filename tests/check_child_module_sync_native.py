"""One new boundary: actual module loader + browser sync callback + CORS check.
Transport/UI/DOM surroundings are fixtures, never real-site acceptance.
"""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parent.parent
out=root / 'build/goal-20261009'
js=(root / 'user/libc/web/js.c').read_text(encoding='utf-8')
browser=(root / 'user/apps/browser.c').read_text(encoding='utf-8')
fetch=(root / 'user/apps/webfetch.c').read_text(encoding='utf-8')
web=(root / 'user/include/web.h').read_text(encoding='utf-8')
def fn(text,signature):
    a=text.index(signature)
    line=text[a:text.index('\n',a)]
    if line.rstrip().endswith('}'): return line+'\n'
    return text[a:text.index('\n}',a)+2]+'\n'
def struct(text,name):
    a=text.index('struct '+name+' {'); return text[a:text.index('};',a)+2]+'\n'
types=''.join(struct(web,n) for n in ('web_request','web_response'))
types+=struct((root / 'user/include/webnet.h').read_text(encoding='utf-8'),'webnet_request')
at=js.index('struct js_module {'); types+=js[at:js.index(';\n',at)+2]
loader=''.join(fn(js,s) for s in ('static JSValue module_bridge_call(', 'static char *module_url(', 'static char *normalize_module(', 'static bool javascript_essence(', 'static bool mime_space(', 'static bool mime_token(', 'static bool valid_mime_essence(', 'static bool javascript_mime(', 'static void set_import_meta(', 'static struct js_module *cache_module(', 'static bool module_context_current(', 'static JSModuleDef *load_module('))
host=fn(browser,'static enum webnet_kind network_kind(')+fn(browser,'static void response_copy(')
host+=struct(browser,'synchronous_load')+''.join(fn(browser,s) for s in ('static void synchronous_completed(', 'static bool host_sync_request(', 'static bool host_sync_load('))
cors='static const char *request_origin(const struct job *j) { return j->origin_tainted ? "null" : j->origin; }\n'+''.join(fn(fetch,s) for s in ('static bool field_slice(', 'static bool field(', 'static bool cors_allowed('))
source=(root / 'tests/child_module_sync_native_cases.c').read_text(encoding='utf-8')
source=source.replace('/* PRODUCT_TYPES */',types).replace('/* PRODUCT_CORS */',cors).replace('/* PRODUCT_HOST */',host).replace('/* PRODUCT_LOADER */',loader)
c=out / 'child-module-sync-native-generated.c'; c.write_text(source,encoding='utf-8')
cc=root / 'tools/msys64/ucrt64/bin/clang.exe'; exe=out / 'child-module-sync-native.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[root / ('third_party/quickjs/'+n+'.c') for n in ('quickjs','dtoa','libregexp','libunicode','cutils')]
subprocess.run([str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(c),*map(str,engine),'-lm','-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
