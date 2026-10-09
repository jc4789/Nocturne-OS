"""Supporting host check of the actual native collector, not browser acceptance."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
output = root / "build/goal-20261009"
output.mkdir(parents=True, exist_ok=True)
header = (root / "user/libc/web/js_inner_text.h").read_text(encoding="utf-8")
helpers = header[header.index("#define INNER_TEXT_MAX"):header.index("static void flush_layout(")]
cases = (root / "tests/inner_text_native_cases.c").read_text(encoding="utf-8")
source = output / "inner-text-native-generated.c"
prefix = '''#include "webi.h"
extern int printf(const char *,...);
extern void *malloc(size_t);
extern void *realloc(void *,size_t);
extern void free(void *);
typedef void JSContext;
static int range_errors;
static int JS_ThrowRangeError(JSContext *ctx,const char *format,...){(void)ctx;(void)format;range_errors++;return -1;}
static void *js_realloc(JSContext *ctx,void *pointer,size_t size){(void)ctx;return realloc(pointer,size);}
'''
source.write_text(prefix + helpers + cases, encoding="utf-8")
executable = output / "inner-text-native.exe"
subprocess.run([str(root / "tools/msys64/ucrt64/bin/clang.exe"), "-std=gnu11", "-Wall", "-Wextra",
                "-Iuser/include", "-Icommon", "-Iuser/libc/web", str(source), "-o", str(executable)], cwd=root, check=True)
subprocess.run([str(executable)], cwd=root, check=True)
