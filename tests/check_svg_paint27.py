"""新しいCSS→SVG描画境界だけ。旧SVG suite／実サイトは再実行しない。"""
from pathlib import Path
import subprocess, os, json, hashlib

root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/svg-paint27-new-once'
out.mkdir(exist_ok=True)
assert not (out/'runtime-started.json').exists(), '同一実行禁止'
def read(p): return (root/p).read_text(encoding='utf-8')
def fn(s, sig):
    at=s.index(sig)
    return s[at:s.index('\n}',at)+2]+'\n'
box=read('user/libc/web/box.c')
parts=fn(box,'static node_t *find_id(')+box[box.index('static void xml_escape('):box.index('/* ---------------------------------------------------------------- generation */')]
dom=read('user/libc/web/dom.c')
parts=fn(dom,'node_t *doc_shadow_parent(')+fn(dom,'node_t *doc_node_root(')+parts
prefix=r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
#include "svg_geometry.h"
#include "form_value.h"
static int freed_images, reports;
void image_free(image_t *i){(void)i;freed_images++;}
void web_js_console(web_doc*d,int level,const char*m){(void)d;(void)level;(void)m;reports++;}
void web_avmedia_checkpoint(void){}
bool web_js_enabled(web_doc*d){(void)d;return true;}
bool web_control_disabled(const node_t*n){(void)n;return false;}
bool web_form_constraints_valid(web_doc*d,node_t*n){(void)d;(void)n;return true;}
bool web_dialog_is_modal(web_doc*d,const node_t*n){(void)d;(void)n;return false;}
void web_dialog_style(web_doc*d,node_t*n,style_t*s){(void)d;(void)n;(void)s;}
node_t *doc_active_target(web_doc*d){(void)d;return NULL;}
node_t *doc_assigned_slot(node_t*n,bool open){(void)n;(void)open;return NULL;}
bool doc_option_selected(node_t*n){(void)n;return false;}
bool web_control_required_applicable(const node_t*n){(void)n;return false;}
bool web_control_required(const node_t*n){(void)n;return false;}
bool web_control_read_write(const node_t*n){(void)n;return false;}
bool web_control_will_validate(const node_t*n){(void)n;return false;}
uint32_t web_control_validity(web_doc*d,node_t*n){(void)d;(void)n;return 0;}
int web_control_in_range(web_doc*d,node_t*n){(void)d;(void)n;return 0;}
enum web_input_kind web_input_type(const node_t*n){(void)n;return 0;}
const char *web_input_edit_text(const node_t*n){(void)n;return "";}
node_t *doc_flat_parent(node_t*n){return n?n->parent:NULL;}
void doc_flat_children(node_t*n,pvec*out){for(node_t*c=n->first;c;c=c->next)pv_push(out,c);}
int doc_attr_index(node_t*n,const char*ns,const char*name,bool namespaced){
    for(int i=0;i<n->nattrs;i++)if(namespaced&&n->attrs[i].namespace_uri&&ns&&!strcmp(ns,n->attrs[i].namespace_uri)&&!strcmp(name,n->attrs[i].local))return i;
    return -1;
}
'''
c=out/'native.c'
attempt=1
while (out/f'compile-{attempt}.log').exists(): attempt+=1
if c.exists(): (out/f'native-before-compile-{attempt}.c').write_bytes(c.read_bytes())
c.write_text(prefix+parts+read('tests/svg_paint27_cases.c'),encoding='utf-8')
css=read('user/libc/web/css.c').replace('#include <nocturne.h>\n','').replace('#include "avmedia.h"\n','void web_avmedia_checkpoint(void);\n')
css_source=out/'css.c'; css_source.write_text(css,encoding='utf-8')
http=read('user/libc/http.c')
urls=out/'url.c'
urls.write_text(http[:http.index('enum http_url_result http_origin_owned_n(')],encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
abi=out/'host-abi'; abi.mkdir(exist_ok=True)
(abi/'setjmp.h').write_text('#include "'+(cc.parent.parent/'include/setjmp.h').as_posix()+'"\n',encoding='utf-8')
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'native.exe'
command=[str(cc),'-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(abi),'-Iuser/include','-Icommon','-Iuser/libc/web',str(c),str(css_source),'user/libc/web/cssprop.c','user/libc/web/util.c','user/libc/web/html.c',str(urls),'-o',str(exe)]
r=subprocess.run(command,cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/f'compile-{attempt}.log').write_text(r.stdout+r.stderr,encoding='utf-8')
print('compile',r.returncode,flush=True)
if r.returncode: print((r.stdout+r.stderr)[-6500:]); raise SystemExit(r.returncode)
(out/'runtime-started.json').write_text(json.dumps({'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['user/libc/web/webi.h','user/libc/web/css.c','user/libc/web/cssprop.c','user/libc/web/box.c','user/libc/web/js.c']}}),encoding='utf-8')
r=subprocess.run([str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'runtime-original.log').write_text(r.stdout+r.stderr,encoding='utf-8')
(out/'result.json').write_text(json.dumps({'compile':0,'runtime':r.returncode,'output':r.stdout+r.stderr},ensure_ascii=False,indent=2),encoding='utf-8')
print(r.stdout+r.stderr); raise SystemExit(r.returncode)
