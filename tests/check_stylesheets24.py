"""First new CSSOM list/link/realm boundaries; no previous suite or site replay."""
from pathlib import Path
import os
import subprocess

root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/stylesheets24'
out.mkdir(exist_ok=True)
def read(path):return (root/path).read_text(encoding='utf-8')
def function(text,signature):
    start=text.index(signature);return text[start:text.index('\n}',start)+2]+'\n'
css=read('user/libc/web/css.c')
parser=css[:css.index('/* ---------------------------------------------------------------- matching */')]
parser+=css[css.index('/* ---------------------------------------------------------------- feature queries'):css.index('/* ---------------------------------------------------------------- the rule index */')]
for include in ('#include <nocturne.h>\n','#include "web_dialog.h"\n','#include "form_validation.h"\n','#include "form_value.h"\n','#include "avmedia.h"\n'):
    parser=parser.replace(include,'')
doc=read('user/libc/web/doc.c')
doc_parts=doc[doc.index('struct cached_css {'):doc.index('/* Repeated shadow')]
for signature in ('static struct cached_css *cached_css(','static int b64(','static int hexv(',
                  'static char *data_url(','static char *doc_absolute_url(','static bool media_wanted(',
                  'static bool link_stylesheet(','static bool link_sheet_applies(','int doc_cssom_link_source(',
                  'const char *doc_link_href('):
    doc_parts+=function(doc,signature)
dom=read('user/libc/web/dom.c')
doc_parts+=function(dom,'node_t *doc_shadow_parent(')+function(dom,'node_t *doc_node_root(')
http=read('user/libc/http.c')
urls=out/'url.c'
at=http.index('enum http_url_result http_origin_owned(')
urls.write_text(http[:http.index('\n}',at)+2],encoding='utf-8')
prefix=read('tests/stylesheets24_native_prefix.c')
c=out/'native.c'
c.write_text(prefix.replace('/* REAL_PARSER */',parser).replace('/* REAL_DOCUMENT_HELPERS */',doc_parts)
             .replace('/* REAL_CSSOM */',read('user/libc/web/cssom.c'))
             .replace('/* REAL_CSSOM_BRIDGE */',read('user/libc/web/js_cssom.h'))
             +read('tests/stylesheets24_native_cases.c'),encoding='utf-8')
boot='''globalThis.DOMException=class DOMException extends Error{constructor(m,n){super(m);this.name=n}};
class Document{};class ShadowRoot{};class HTMLStyleElement{};class HTMLLinkElement{};
Object.assign(globalThis,{Document,ShadowRoot,HTMLStyleElement,HTMLLinkElement});
globalThis.document=fixtureDom('document');
(function(){'use strict';const rawDom=fixtureRaw;
function htmlElementBrand(node,tag){if(fixtureDom('tag',node)!==tag)throw new TypeError('Wrong HTML element receiver');}
function reflectedAttr(node,name){return fixtureDom('attr',node,name);}
'''+read('user/libc/web/js_stylesheets.js')+'})();\n'
(out/'bindings.js').write_text(boot,encoding='utf-8')
(out/'cases.js').write_text(read('tests/stylesheets24_cases.js'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
abi=out/'host-abi';abi.mkdir(exist_ok=True)
(abi/'setjmp.h').write_text('#include "'+(cc.parent.parent/'include/setjmp.h').as_posix()+'"\n',encoding='utf-8')
objects=[]
native_flags=['-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-I'+str(abi),'-Iuser/include','-Icommon','-Iuser/libc/web','-Ithird_party/quickjs']
for i,path in enumerate((c,root/'user/libc/web/cssprop.c',root/'user/libc/web/util.c',root/'user/libc/web/html.c',urls)):
    obj=out/f'native{i}.o';objects.append(obj)
    subprocess.run([str(cc),*native_flags,'-c',str(path),'-o',str(obj)],cwd=root,env=env,check=True)
for name in ('quickjs','dtoa','libregexp','libunicode','cutils'):
    obj=out/f'{name}.o';objects.append(obj)
    subprocess.run([str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"',
                    '-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include','-c',str(root/f'third_party/quickjs/{name}.c'),'-o',str(obj)],cwd=root,env=env,check=True)
exe=out/'native.exe'
subprocess.run([str(cc),'-Wl,--gc-sections',*map(str,objects),'-lm','-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=30)
