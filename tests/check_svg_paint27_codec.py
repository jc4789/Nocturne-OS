"""25件の旧mainを含めず、新しいcodec／hint／長URL境界だけ一回実行。"""
from pathlib import Path
import subprocess, os, json
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/svg-paint27-codec-once';out.mkdir(exist_ok=True)
assert not (out/'runtime-started.json').exists(), '同一実行禁止'
previous=(root/'build/goal-20261009/svg-paint27-new-once/native.c').read_text(encoding='utf-8')
prefix=previous[:previous.index('static int checks, failures;')]
css=(root/'user/libc/web/css.c').read_text(encoding='utf-8').replace('#include <nocturne.h>\n','').replace('#include "avmedia.h"\n','void web_avmedia_checkpoint(void);\n')
code=prefix+css+'\n#define NANOSVG_IMPLEMENTATION\n#include "nanosvg.h"\n'+(root/'tests/svg_paint27_codec_cases.c').read_text(encoding='utf-8')
(out/'native.c').write_text(code,encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
abi=root/'build/goal-20261009/svg-paint27-new-once/host-abi'
exe=out/'native.exe'
r=subprocess.run([str(cc),'-std=gnu11','-O1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(abi),'-Iuser/include','-Icommon','-Iuser/libc/web','-Ithird_party/img',str(out/'native.c'),'user/libc/web/cssprop.c','user/libc/web/util.c','user/libc/web/html.c',str(root/'build/goal-20261009/svg-paint27-new-once/url.c'),'-o',str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'compile-original.log').write_text(r.stdout+r.stderr,encoding='utf-8');print('compile',r.returncode,flush=True)
if r.returncode: print((r.stdout+r.stderr)[-4000:]);raise SystemExit(r.returncode)
(out/'runtime-started.json').write_text('{}',encoding='utf-8')
r=subprocess.run([str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'runtime-original.log').write_text(r.stdout+r.stderr,encoding='utf-8')
(out/'result.json').write_text(json.dumps({'compile':0,'runtime':r.returncode,'output':r.stdout+r.stderr},ensure_ascii=False,indent=2),encoding='utf-8')
print(r.stdout+r.stderr);raise SystemExit(r.returncode)
