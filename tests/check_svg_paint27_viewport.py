"""修理後の新非正方形と明示viewBox2境界だけ。元6条件は再実行しない。"""
from pathlib import Path
import subprocess,os,json
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009/svg-paint27-viewport-once';out.mkdir(exist_ok=True)
assert not (out/'runtime-started.json').exists(), '同一実行禁止'
source=r'''
#include <stdio.h>
#include <math.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
static int checks,failures;
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
int main(void){
    char a[]="<svg width='40' height='10'><path d='M0 0L40 10' stroke='red' stroke-width='25%'/></svg>";
    NSVGimage *ia=nsvgParse(a,"px",96);
    float diagonal=sqrtf(40*40+10*10)/sqrtf(2.0f);
    check(ia&&ia->shapes&&fabsf(ia->shapes->strokeWidth-diagonal*.25f)<.001f,"new non-square viewport uses normalized actual diagonal before final inference");
    nsvgDelete(ia);
    char b[]="<svg width='40' height='10' viewBox='0 0 80 20'><path d='M0 0L80 20' stroke='blue' stroke-width='25%'/></svg>";
    NSVGimage *ib=nsvgParse(b,"px",96);
    check(ib&&ib->shapes&&fabsf(ib->shapes->strokeWidth-diagonal*.25f)<.001f,"explicit non-square viewBox retains own user coordinates and subsequent viewport scale");
    nsvgDelete(ib);printf("svg-paint27-viewport: %d checks, %d failures\n",checks,failures);return failures!=0;
}
'''
(out/'native.c').write_text(source,encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe';env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''));exe=out/'native.exe'
r=subprocess.run([str(cc),'-std=gnu11','-O1','-Ithird_party/img',str(out/'native.c'),'-o',str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'compile-original.log').write_text(r.stdout+r.stderr,encoding='utf-8');print('compile',r.returncode,flush=True)
if r.returncode:print((r.stdout+r.stderr)[-3000:]);raise SystemExit(r.returncode)
(out/'runtime-started.json').write_text('{}',encoding='utf-8')
r=subprocess.run([str(exe)],cwd=root,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'runtime-original.log').write_text(r.stdout+r.stderr,encoding='utf-8')
(out/'result.json').write_text(json.dumps({'compile':0,'runtime':r.returncode,'output':r.stdout+r.stderr},ensure_ascii=False,indent=2),encoding='utf-8');print(r.stdout+r.stderr);raise SystemExit(r.returncode)
