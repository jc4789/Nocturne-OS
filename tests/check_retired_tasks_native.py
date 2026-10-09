"""Supporting actual QuickJS refcount check of the current native task cleanup."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
out.mkdir(parents=True, exist_ok=True)
text = (root / "user/libc/web/js.c").read_text(encoding="utf-8")
def section(start, end):
    return text[text.index(start):text.index(end, text.index(start))]
pieces = [section("struct js_timer {", "struct js_script {"),
          section("static void release_timer(", "/* Posted messages are tasks"),
          section("static JSValue native_post_task(", "static JSValue native_cancel_post("),
          section("static struct js_idle_task *take_idle(", "static JSValue native_css_supports("),
          section("static JSValue native_clear(", "static JSValue microtask_job(")]
prefix = (root / "tests/retired_tasks_native_prefix.c").read_text(encoding="utf-8")
cases = (root / "tests/retired_tasks_native_cases.c").read_text(encoding="utf-8")
# Struct declarations precede the minimal harness state; native functions are
# copied verbatim, not reimplemented or replaced with a test success API.
source = out / "retired-tasks-native-generated.c"
source.write_text(prefix.replace("/* TASK_STRUCTS */", pieces[0]) + "\n" + "\n".join(pieces[1:]) + cases, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH", ""))
exe = out / "retired-tasks-native.exe"
engine = [root / ("third_party/quickjs/" + name + ".c") for name in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
subprocess.run([str(cc), "-std=gnu11", "-O1", "-fwrapv", "-funsigned-char", "-DCONFIG_NOCTURNE",
                '-DCONFIG_VERSION="Nocturne-test"', "-Ithird_party/quickjs", "-Ibuild/nocturne-audit/url-host-include",
                str(source), *map(str, engine), "-lm", "-o", str(exe)], cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
