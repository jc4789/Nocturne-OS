"""実C書記素helperを同版原GraphemeBreakTestと照合する。実サイト受入ではない。"""
from pathlib import Path
from hashlib import sha256
import os
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/browser-grapheme-host"
OUT.mkdir(parents=True, exist_ok=True)
layout = (ROOT / "user/libc/web/layout.c").read_text(encoding="utf-8")
implementation = layout[layout.index("/* ---------------------------------------------------------------- grapheme boundaries */"):
                        layout.index("float wf_width(")]
webi = (ROOT / "user/libc/web/webi.h").read_text(encoding="utf-8")
state = webi[webi.index("typedef struct wf_grapheme_state {"):webi.index("} wf_grapheme_state;") + len("} wf_grapheme_state;")]
source = OUT / "native.c"
source.write_text("#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <windows.h>\n" + state + "\n" + implementation + "\n" +
                  (ROOT / "tests/grapheme_native_cases.c").read_text(encoding="utf-8"), encoding="utf-8")
cc = ROOT / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = OUT / "native.exe"
subprocess.run([str(cc), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Iuser/libc/web", str(source), "-o", str(exe)], cwd=ROOT, env=env, check=True)
test = ROOT / "third_party/unicode/18.0.0/GraphemeBreakTest.txt"
cases = []
for line_number, line in enumerate(test.read_text(encoding="utf-8").splitlines(), 1):
    body = line.split("#", 1)[0].strip()
    if not body:
        continue
    codepoints, boundaries, offset = [], [], 0
    for token in body.split():
        if token == "÷":
            boundaries.append(offset)
        elif token != "×":
            cp = int(token, 16)
            codepoints.append(cp)
            offset += len(chr(cp).encode("utf-8"))
    cases.append((line_number, codepoints, boundaries))
inputs = "".join(str(len(cps)) + " " + " ".join(f"{cp:X}" for cp in cps) + "\n" for _, cps, _ in cases)
result = subprocess.run([str(exe)], cwd=ROOT, env=env, input=inputs, text=True,
                        encoding="utf-8", capture_output=True, timeout=30, check=True)
lines = result.stdout.splitlines()
if lines[0] != "BOUNDED 0":
    raise SystemExit(lines[0])
if len(lines) != len(cases) * 2 + 1:
    raise SystemExit("原入力と実C出力の件数が一致しません")
failed = 0
for index, (line_number, _, expected) in enumerate(cases):
    for mode in range(2):
        fields = lines[index * 2 + 1 + mode].split()
        actual = list(map(int, fields[1:]))
        if actual != expected:
            failed += 1
            print(f"FAIL 原UCD行{line_number} {fields[0]} expected={expected} actual={actual}")
print(f"Unicode18.0.0実C: 原{len(cases)}行 × next/run跨ぎ2経路、境界外読出し保護、{failed}失敗")
print(f"原試験 SHA256 {sha256(test.read_bytes()).hexdigest()}")
raise SystemExit(bool(failed))
