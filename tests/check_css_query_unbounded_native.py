"""One new explicit-query-stack/custom-cycle run, never former CSS suites."""
from pathlib import Path
import os
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")
parser = css[:css.index("/* ---------------------------------------------------------------- matching */")]
parser += css[css.index("/* ---------------------------------------------------------------- feature queries"):css.index("/* ---------------------------------------------------------------- the rule index */")]
parser += css[css.index("static const struct custom_prop *find_var("):css.index("struct cascade {")]
prefix = '''#include <stdio.h>
#include "webi.h"
static int fail_calloc,fail_realloc,reports;
static void *query_calloc(size_t n,size_t s){if(fail_calloc && !--fail_calloc)return NULL;return calloc(n,s);}
static void *query_realloc(void *p,size_t n){if(fail_realloc && !--fail_realloc)return NULL;return realloc(p,n);}
void web_js_console(web_doc*d,int level,const char*message){(void)d;(void)level;(void)message;reports++;}
#define calloc query_calloc
#define realloc query_realloc
'''
cases = (root / "tests/css_query_unbounded_native_cases.c").read_text(encoding="utf-8")
c = out / "css-query-unbounded-generated.c"
c.write_text(prefix + parser + "\n#undef calloc\n#undef realloc\n" + cases,encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "css-query-unbounded-host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n',encoding="utf-8")
env = dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe = out / "css-query-unbounded-native.exe"
command = [str(cc),"-std=gnu11","-O1","-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
           "-I"+str(abi),"-Iuser/include","-Icommon","-Iuser/libc/web",str(c),
           "user/libc/web/cssprop.c","user/libc/web/util.c","user/libc/web/html.c","-o",str(exe)]
subprocess.run(command,cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=60)
