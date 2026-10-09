"""Compile the current native helpers and actual node_t, without an OS build."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
output = root / "build/goal-20261009"
output.mkdir(parents=True, exist_ok=True)
js = (root / "user/libc/web/js.c").read_text(encoding="utf-8")
dom = (root / "user/libc/web/dom.c").read_text(encoding="utf-8")
helpers = js[js.index("static bool node_precedes("):js.index("static bool custom_candidate(")]
roots = dom[dom.index("node_t *doc_shadow_parent("):dom.index("bool doc_node_connected(")]
cases = (root / "tests/node_position_native_cases.c").read_text(encoding="utf-8")
source = output / "node-position-native-generated.c"
source.write_text('#include "webi.h"\nextern int printf(const char *,...);\n' + roots + helpers + cases, encoding="utf-8")
executable = output / "node-position-native.exe"
subprocess.run([str(root / "tools/msys64/ucrt64/bin/clang.exe"), "-std=gnu11", "-Wall", "-Wextra",
                "-Iuser/include", "-Icommon", "-Iuser/libc/web", str(source), "-o", str(executable)], cwd=root, check=True)
subprocess.run([str(executable)], cwd=root, check=True)
