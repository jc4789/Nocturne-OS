"""New int/float extent boundary only. Does not rerun absolute-height25."""
from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
s=(root/'user/libc/web/layout.c').read_text(encoding='utf-8')
def fn(sig):
    at=s.index(sig);return s[at:s.index('\n}',at)+2]+'\n'
assert 'd->doc_h = document_extent(h);' in s
c=out/'layout-extent18-generated.c'
c.write_text('#include <stdio.h>\n#include <math.h>\n#include <limits.h>\n'+fn('static float scroll_extent(')+fn('static int document_extent(')+r'''
static unsigned checks,failures;
static void check(int ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
int main(void){
    check(scroll_extent(1e9f)==1e9f,"internal scroll width no longer capped at 100 million");
    check(scroll_extent(3e30f)==3e30f,"finite native float extent preserved");
    check(scroll_extent(-1)==0&&scroll_extent(NAN)==0&&scroll_extent(INFINITY)==0,"invalid internal extent stays nonnegative finite");
    check(document_extent(1e9f)==1000000000,"document API accepts representable extent beyond old quota");
    check(document_extent(1.1f)==2&&document_extent(0)==0,"ordinary document height uses ceiling");
    check(document_extent(2147483520.0f)==2147483520,"largest float below signed int maximum casts exactly");
    check(document_extent((float)INT_MAX)==INT_MAX,"float-rounded signed max safely saturates before cast");
    check(document_extent(3e30f)==INT_MAX&&document_extent(INFINITY)==INT_MAX,"positive overflow saturates actual signed API representation");
    check(document_extent(NAN)==0&&document_extent(-INFINITY)==0&&document_extent(-0.1f)==0,"invalid or negative document height cannot trigger undefined cast");
    printf("layout-extent18: %u checks, %u failures\n",checks,failures);return failures!=0;
}
''',encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
exe=out/'layout-extent18.exe'
subprocess.run([str(cc),'-std=gnu11','-O1',str(c),'-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
