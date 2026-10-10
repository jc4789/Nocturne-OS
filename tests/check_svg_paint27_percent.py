"""後で発見したSVG百分率線幅の新境界だけ一回。旧37条件は含めない。"""
from pathlib import Path
import subprocess,os,json
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/svg-paint27-percent-once';out.mkdir(exist_ok=True)
assert not (out/'runtime-started.json').exists(), '同一実行禁止'
def read(p):return (root/p).read_text(encoding='utf-8')
def fn(s,sig):
    a=s.index(sig);return s[a:s.index('\n}',a)+2]+'\n'
old=read('build/goal-20261009/svg-paint27-new-once/native.c')
prefix=old[:old.index('node_t *doc_shadow_parent(')]
box=read('user/libc/web/box.c');dom=read('user/libc/web/dom.c')
parts=fn(dom,'node_t *doc_shadow_parent(')+fn(dom,'node_t *doc_node_root(')+fn(box,'static node_t *find_id(')+box[box.index('static void xml_escape('):box.index('/* ---------------------------------------------------------------- generation */')]
css=read('user/libc/web/css.c').replace('#include <nocturne.h>\n','').replace('#include "avmedia.h"\n','void web_avmedia_checkpoint(void);\n')
(out/'native.c').write_text(prefix+parts+css+'\n#define NANOSVG_IMPLEMENTATION\n#include "nanosvg.h"\n'+read('tests/svg_paint27_percent_cases.c'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''));exe=out/'native.exe'
r=subprocess.run([str(cc),'-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Ibuild/goal-20261009/svg-paint27-new-once/host-abi','-Iuser/include','-Icommon','-Iuser/libc/web','-Ithird_party/img',str(out/'native.c'),'user/libc/web/cssprop.c','user/libc/web/util.c','user/libc/web/html.c','build/goal-20261009/svg-paint27-new-once/url.c','-o',str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'compile-original.log').write_text(r.stdout+r.stderr,encoding='utf-8');print('compile',r.returncode,flush=True)
if r.returncode:print((r.stdout+r.stderr)[-3000:]);raise SystemExit(r.returncode)
(out/'runtime-started.json').write_text('{}',encoding='utf-8')
r=subprocess.run([str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'runtime-original.log').write_text(r.stdout+r.stderr,encoding='utf-8')
(out/'result.json').write_text(json.dumps({'compile':0,'runtime':r.returncode,'output':r.stdout+r.stderr},ensure_ascii=False,indent=2),encoding='utf-8');print(r.stdout+r.stderr);raise SystemExit(r.returncode)
