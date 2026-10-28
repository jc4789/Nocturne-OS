"""CSSOM の規則の木の持ち主（cssom.c）を、css.c の解析と DOM と一緒に host で確かめる。

tests/cssom_rules_native_cases.c の各件は、WPT の頁（css/css-nesting/cssom.html、
serialize-group-rules-with-decls.html、css/cssom/CSSKeyframesRule*.html、cssom-pagerule.html
など）か仕様の節を名に書いた、insertRule・deleteRule・「serialize a CSS rule」・規則の属性・
MediaList・宣言の塊・CSSRule の同一性の問いと期待。JS の束縛（例外の型、IDL の形、
[SameObject]）は guest の WPT の仕事で、ここでは照らさない。

  python -X utf8 tests/check_cssom_rules_native.py [--old]

--old は規則の木の前の源（930c2d13）で同じ件を組み、変更前の数を出す。前の口で
書けない件（子の規則、規則の種類ごとの属性、MediaList、宣言）は落ちる。
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
old = "--old" in sys.argv
out = root / "build/check-cssom-rules" / ("old" if old else "new")
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
source = root
if old:
    source = out / "src"
    if not (source / "user/libc/web/cssom.c").exists():
        archive = out / "old.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive), "930c2d13", "user/libc/web",
                        "user/include", "common"], cwd=root, check=True)
        with tarfile.open(archive) as t:
            t.extractall(source, filter="data")
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
web = source / "user/libc/web"
cases = out / "cssom-rules-generated.c"
cases.write_text(("#define OLD_SOURCE 1\n" if old else "") +
                 (root / "tests/cssom_rules_native_cases.c").read_text(encoding="utf-8"), encoding="utf-8")
stubs = out / "unreached.c"
exe = out / "cssom-rules.exe"
units = [str(cases), str(web / "css.c"), str(web / "dom.c"), str(web / "html.c"), str(web / "util.c"),
         str(web / "cssprop.c"), str(web / "color.c")]
flags = ["-std=gnu11", "-O1", "-w", "-I" + str(abi), "-I" + str(source / "user/include"), "-I" + str(source / "common"),
         "-I" + str(source / "user/libc"), "-I" + str(web), "-I" + str(root / "third_party/quickjs")]
undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
names = set()
for attempt in range(6):
    # every external the linked units reach but these cases never call: a call stops with its name
    stubs.write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
        f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(names)), encoding="utf-8")
    r = subprocess.run([str(cc), *flags, *units, str(stubs), "-o", str(exe)], cwd=root, env=env,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode == 0:
        break
    found = set(undefined.findall(r.stdout + r.stderr)) - names
    if not found:
        print(r.stdout + r.stderr)
        sys.exit("道具を組めない" + ("（前の口に無い関数がある）" if old else ""))
    names |= found
else:
    sys.exit("道具を組めない")
result = subprocess.run([str(exe)], cwd=root, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                        timeout=600)
print(result.stdout, end="")
if result.returncode not in (0, 1):
    print(result.stderr, end="")
    sys.exit(f"道具が止まった（{result.returncode}）")
sys.exit(result.returncode)
