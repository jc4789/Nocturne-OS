"""New winner/substitution boundary only; no earlier CSS or layout suites."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")

def function(signature):
    start = css.index(signature)
    end = css.index("\n}", start) + 2
    return css[start:end] + "\n"

start = css.index("struct decl {")
parser = css[start:css.index("\n};", start) + 3] + "\n"
for signature in ("static const char *scan_to(", "static const char *skip_ws(", "static void trim_r("):
    parser += function(signature)
parser += next(line for line in css.splitlines() if line.startswith("static bool ident_char(")) + "\n"
parser += function("static bool supports_custom_name(")
variables = css[css.index("static const struct custom_prop *find_var("):css.index("struct var_walk {")]
old = "for (; v; v = v->next)\n        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v;"
new = "for (; v; v = v->next) { var_probes++;\n        if (strlen(v->name) == n && !memcmp(v->name, name, n)) return v; }"
assert variables.count(old) == 1
variables = variables.replace(old, new)
signature = "static bool subst(const char *s, size_t n, const struct custom_prop *vars, sbuf *out) {"
assert variables.count(signature) == 1
variables = variables.replace(signature, signature + "\n    subst_calls++;")
parser += variables + function("static bool value_references(")
parser += css[css.index("struct cascade {"):css.index("static style_t *compute(")]
compute = function("static style_t *compute(")
assert "memset(setbits, 0, (size_t)css_prop_count());" in compute
assert "cx.font_pass = false;" in compute
prefix = '''#include <stdio.h>
#include <ctype.h>
#include "webi.h"
static unsigned walk_allocs, subst_calls, var_probes;
static int fail_calloc;
static void *tracked_calloc(size_t n,size_t s) {
    walk_allocs++;
    if (fail_calloc && !--fail_calloc) return NULL;
    return calloc(n,s);
}
#define calloc tracked_calloc
'''
cases = (root / "tests/cascade_winner23_cases.c").read_text(encoding="utf-8")
c = out / "cascade-winner23-generated.c"
c.write_text(prefix + parser + "\n#undef calloc\n" + cases, encoding="utf-8")
# cssprop retains its URL-valued property path. Link the current real owned
# canonicalizer (no HTTP transport is used by these winner-only cases).
http = (root / "user/libc/http.c").read_text(encoding="utf-8")
urls = out / "cascade-winner23-url-generated.c"
urls.write_text(http[:http.index("enum http_url_result http_origin_owned_n(")], encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "cascade-winner23-host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "cascade-winner23-native.exe"
command = [str(cc), "-std=gnu11", "-O1", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc/web", str(c),
           "user/libc/web/cssprop.c", "user/libc/web/util.c", "user/libc/web/html.c", str(urls), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=60)
