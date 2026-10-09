"""New native local scroll-bound contribution only, one execution."""
from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
s=(root/'user/libc/web/layout.c').read_text(encoding='utf-8')
def fn(sig):
    at=s.index(sig);return s[at:s.index('\n}',at)+2]+'\n'
publish=fn('static void scroll_area_publish(')
assert 'box_abs_' not in publish and 'for(' not in publish and 'while(' not in publish
assert 'scroll_area_publish(b);' in fn('static void scroll_areas(')
prefix=r'''
#include <stdio.h>
#include <math.h>
#include "webi.h"
static web_doc *D;
static box_t *viewport,*top_layer;
bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){(void)d;return b==viewport;}
bool web_dialog_layer_box(web_doc *d,box_t *b){(void)d;return b==top_layer;}
static float fmaxf_(float a,float b){return a>b?a:b;}
'''
c=out/'scroll-area20-generated.c'
c.write_text(prefix+fn('static float scroll_extent(')+publish+(root/'tests/scroll_area20_cases.c').read_text(encoding='utf-8'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'scroll-area20.exe'
subprocess.run([str(cc),'-std=gnu11','-O1','-Iuser/include','-Icommon','-Iuser/libc/web',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
