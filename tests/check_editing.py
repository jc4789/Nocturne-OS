"""Host check of the editing layer: the product's QuickJS compiles the four programs of the
generated js_bootstrap.inc (bindings, locale, Web Audio, editing), then runs the product's
js_input_event.js, js_document_commands.js and the js_editing program (with the execCommand
draft's implementation from third_party/editing in it) over the small DOM of
tests/editing_host_dom.js, through the cases of tests/editing_host_cases.js.

What it does not show: the guest's native DOM, Range, Selection, MutationObserver and computed
style (tests/js_editing_cases.js runs in the guest, through tests/jstest.c), the native text-control
path (tests/check_document_commands_native.py), the editing keys and the clipboard keys of the
browser, and WPT (editing/, input-events/, html/editing/), which runs in the guest.
    python -X utf8 tests/check_editing.py
Regenerate js_bootstrap.inc first (python -X utf8 user/libc/web/js_embed.py).
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = root / "build" / "editing-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "editing_hosttest.exe"
quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
# The bundled tools are not in git: a work tree under .claude/worktrees finds them in the checkout above it.
clang = next((p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
              if (p / "tools/msys64/ucrt64/bin/clang.exe").exists()), None)
if clang is None:
    sys.exit("tools/msys64/ucrt64/bin/clang.exe not found")
subprocess.run([str(clang), "-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char",
                "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"', "-I" + str(root / "third_party/quickjs"),
                str(root / "tests/editing_hosttest.c"), *quickjs, "-lm", "-o", str(exe)], check=True)
scripts = [root / "tests/editing_host_dom.js", root / "tests/editing_host_prelude.js",
           root / "user/libc/web/js_input_event.js", root / "user/libc/web/js_document_commands.js",
           root / "tests/editing_host_cases.js"]
result = subprocess.run([str(exe), *map(str, scripts)], capture_output=True, text=True, encoding="utf-8", timeout=120)
print(result.stdout, end="")
if result.stderr:
    print(result.stderr, end="")
ok = result.returncode == 0 and "EXCEPTION" not in result.stdout and "COMPILE FAIL" not in result.stdout \
    and ", failed: 0" in result.stdout
print("editing host check:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
