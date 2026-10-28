"""CSS Typed OM の持ち主（js_typed_om.js・js_typed_om.c の上の cssmath.c・cssprop.c・color.c・css.c）を、
WPT の css/css-typed-om の頁のまま、WPT の testharness.js で host の QuickJS の一つの realm に当てる。

頁の台本（META: script=、<script src>、<script> の中身）を順に走らせる。頁より先に読むのは製品の
js_dom_exception.js・js_geometry.js・js_typed_om.js と、代役の tests/typed_om_host.js（時計、console、
要素の代役）。要素のインラインの style は、js.c のインラインの style と同じ css.c の宣言の塊の関数
（css_decl_find・css_decl_set・css_decl_serialize）の上の代役で、IDL の形は js_webidl.c が与える。

guest の WPT の代わりではない。数えないもの：計算値（computedStyleMap、cascade が無い）、規則の
styleMap（cssom.c が無い）、配置と描画、Image。それらに触れた件は「host では見ない」に数え、合否に入れない。
--before は js_typed_om.js を読まずに同じ頁を走らせ、Typed OM の無い製品の数を出す（変更前の main には
Typed OM の台本も C の口も無い）。

  python -X utf8 tests/check_typed_om.py [--before] [--verbose] [頁の名の一部]...
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt"))
suite = wpt / "css/css-typed-om"
before = "--before" in sys.argv
verbose = "--verbose" in sys.argv
filters = [a for a in sys.argv[1:] if not a.startswith("--")]
out = root / "build/check-typed-om"
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "typed_om_hosttest.exe"

# ---------------------------------------------------------------- the tool
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
web = root / "user/libc/web"
lexbor = [str(root / line.split("+=")[1].strip())
          for line in (root / "third_party/lexbor/sources.mk").read_text("utf-8").splitlines()
          if line.startswith("LXB_VENDOR_C") and "+=" in line and
          any(f"/lexbor/{m}/" in line for m in ("core", "css"))] + [str(root / "ports/lexbor/memory.c")]
quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
objects = out / "obj"
objects.mkdir(exist_ok=True)

def compile_once(source, flags, name):
    """an object for source, compiled again only when the source is newer"""
    obj = objects / (name + ".o")
    if not obj.exists() or obj.stat().st_mtime < Path(source).stat().st_mtime:
        subprocess.run([str(cc), *flags, "-c", str(source), "-o", str(obj)], cwd=root, env=env, check=True)
    return str(obj)

host_flags = ["-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"',
              "-I" + str(root / "third_party/quickjs")]
lexbor_flags = ["-O2", "-std=gnu11", "-w", "-DLEXBOR_STATIC", "-I" + str(root / "third_party/lexbor/source"),
                "-I" + str(root / "ports/lexbor/include")]
engine_flags = ["-std=gnu11", "-O1", "-w", "-DLEXBOR_STATIC", "-I" + str(abi), "-I" + str(root / "user/include"),
                "-I" + str(root / "common"), "-I" + str(root / "user/libc"), "-I" + str(web),
                "-I" + str(root / "third_party/quickjs"), "-I" + str(root / "third_party/lexbor/source"),
                "-I" + str(root / "ports/lexbor/include")]
units = [compile_once(s, host_flags, "qjs_" + Path(s).stem) for s in quickjs]
units += [compile_once(s, lexbor_flags, "lxb_" + "_".join(Path(s).parts[-2:]).replace(".c", "")) for s in lexbor]
engine = ["js_typed_om.c", "js_webidl.c", "cssmath.c", "cssprop.c", "color.c", "css.c", "shape.c", "util.c", "html.c", "dom.c"]
units += [compile_once(web / s, engine_flags, "web_" + Path(s).stem) for s in engine]
units.append(compile_once(root / "tests/typed_om_hosttest.c", host_flags, "harness"))  # the host's stdio
stubs = out / "unreached.c"
undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
names = set()
for attempt in range(8):
    # every external the linked units reach but these pages never call: a call stops with its name
    stubs.write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
        f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(names)), encoding="utf-8")
    r = subprocess.run([str(cc), "-w", *units, str(stubs), "-lm", "-o", str(exe)], cwd=root, env=env,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode == 0:
        break
    found = set(undefined.findall(r.stdout + r.stderr)) - names
    if not found:
        print(r.stdout + r.stderr)
        sys.exit("道具を組めない")
    names |= found
else:
    sys.exit("道具を組めない")

# ---------------------------------------------------------------- the pages
REPORT = ("add_completion_callback((tests,status)=>{const names=['PASS','FAIL','TIMEOUT','NOTRUN','PRECONDITION_FAILED'];"
          "for(const t of tests)print('RESULT '+names[t.status]+' | '+t.name.replace(/\\n/g,' ')+(t.status?' | '+String(t.message).replace(/\\n/g,' '):''));"
          "print('HARNESS '+status.status+' '+(status.message||''));});")
product = [str(web / "js_dom_exception.js"), str(web / "js_geometry.js")] + ([] if before else [str(web / "js_typed_om.js")])
# the realm: with Typed OM, the product's Window setup over the stand-in document; without, the same
# document with what the elements had before (a style, no maps)
SETUP = ("if(typeof typedOMInterfaces==='function')__hostRealm(%s,%s);else{globalThis.DOMException=domExceptionBridge.DOMException;"
         "geometryInterfaces({});const style={cssText:''};globalThis.document={createElement:()=>({style,remove(){},"
         "computedStyleMap(){throw new Error(HOST_UNAVAILABLE);},get sheet(){throw new Error(HOST_UNAVAILABLE);}}),"
         "body:{appendChild:e=>e},head:{appendChild:e=>e},location:{href:%s},getElementsByTagName:()=>[]};globalThis.META_TITLE=%s;}")
# idlharness's members of the document's interfaces: the host has no Element, CSSStyleRule or HTMLElement,
# and the CSS namespace's members other than the factories are js_bootstrap.js's
DOCUMENT_INTERFACES = ("Element ", "Element:", "HTMLElement", "SVGElement", "MathMLElement", "CSSStyleRule", "Document", "Window",
                       "CSS namespace: operation escape", "CSS namespace: operation supports")
# what a page reaches of the document that the stand-in does not have: an element by its id (the
# window's named properties), the document's style sheets, CSS.escape
DOCUMENT_REACH = re.compile(r"host: not available|'styleMap' is not defined|'(element|target)' is not defined")
pages = sorted(p for p in suite.rglob("*") if p.is_file() and (p.name.endswith(".any.js") or p.suffix == ".html")
               and "resources" not in p.parts)
pages = [p for p in pages if p.name.endswith(".any.js") or "testharness.js" in p.read_text(encoding="utf-8")]
if filters:
    pages = [p for p in pages if any(f in p.relative_to(suite).as_posix() for f in filters)]
inline_dir = out / "inline"
inline_dir.mkdir(exist_ok=True)

def page_scripts(page):
    """the page's scripts in document order: a file path, or None for the harness's own"""
    if page.name.endswith(".any.js"):
        scripts = []
        for line in page.read_text(encoding="utf-8").splitlines():
            m = re.match(r"//\s*META:\s*script=(\S+)", line)
            if m:
                scripts.append(m[1])
        return [resolve(page, s) for s in scripts] + [str(page)]
    html = page.read_text(encoding="utf-8")
    result = []
    for i, m in enumerate(re.finditer(r"<script\b([^>]*)>(.*?)</script>", html, re.S | re.I)):
        src = re.search(r"\bsrc\s*=\s*[\"']?([^\"' >]+)", m[1])
        if src:
            result.append(resolve(page, src[1]))
        elif m[2].strip():
            path = inline_dir / (page.relative_to(suite).as_posix().replace("/", "__") + f".{i}.js")
            path.write_text(m[2], encoding="utf-8")
            result.append(str(path))
    return result

def resolve(page, src):
    if src.endswith(("/testharness.js", "/testharnessreport.js")):
        return None
    src = src.replace("/resources/WebIDLParser.js", "/resources/webidl2/lib/webidl2.js")  # wptserve's rewrite
    if src.startswith("/"):
        return str(wpt / src.lstrip("/"))
    return str(page.parent / src)

totals = {"PASS": 0, "FAIL": 0, "HOST": 0}
silent = []
record = {}
run_env = dict(env, WPT_ROOT=str(wpt))
for page in pages:
    name = page.relative_to(suite).as_posix()
    title = re.search(r"<title>(.*?)</title>", page.read_text(encoding="utf-8"), re.S | re.I) if page.suffix == ".html" else None
    title = json.dumps(title[1].strip() if title else "")
    url = json.dumps("http://web-platform.test:8000/css/css-typed-om/" + name)
    scripts = [str(root / "tests/typed_om_host.js"), *product, str(wpt / "resources/testharness.js"), "-e", REPORT,
               "-e", SETUP % (title, url, url, title)] + [s for s in page_scripts(page) if s]
    try:
        p = subprocess.run([str(exe), *scripts], capture_output=True, timeout=300, cwd=root, env=run_env)
        text = p.stdout.decode("utf-8", "replace")
        if p.returncode not in (0, 2):
            text += f"\nEXIT {p.returncode}"
    except subprocess.TimeoutExpired:
        text = "TIMEOUT"
    (out / (name.replace("/", "__") + ".log")).write_text(text, encoding="utf-8")
    results = [line.split(" | ", 2) for line in text.splitlines() if line.startswith("RESULT ")]
    host = [r for r in results if r[0] != "RESULT PASS" and len(r) > 1 and
            ((len(r) > 2 and DOCUMENT_REACH.search(r[2])) or
             r[1].startswith(DOCUMENT_INTERFACES))]
    for r in results:
        record[name + " | " + r[1]] = "HOST" if r in host else r[0][7:]
    judged = [r for r in results if r not in host]
    passed = sum(1 for r in judged if r[0] == "RESULT PASS")
    totals["PASS"] += passed
    totals["FAIL"] += len(judged) - passed
    totals["HOST"] += len(host)
    harness = next((line for line in text.splitlines() if line.startswith("HARNESS")), "HARNESS ? (no report)")
    note = "" if harness.startswith("HARNESS 0") else " " + harness
    if host:
        note += f"（host では見ない {len(host)}）"
    print(f"{name}: {passed}/{len(judged)}{note}")
    for r in judged:
        if r[0] != "RESULT PASS" and verbose:
            print("   ", " | ".join(r)[7:][:300])
    if not results:
        silent.append(name)
        if verbose:
            print("   ", text[-600:].replace("\n", "\n    "))
print(f"css-typed-om（{'Typed OM なし' if before else 'Typed OM あり'}）：{len(pages)} 頁、合格 {totals['PASS']}、"
      f"不合格 {totals['FAIL']}、host では見ない {totals['HOST']}、結果の無い頁 {len(silent)}")
if silent:
    print("  結果の無い頁：" + "、".join(silent))
# the two runs over the same subtests: a subtest the other run has not is "not run" there
mine, other = out / ("before.json" if before else "after.json"), out / ("after.json" if before else "before.json")
if not filters:
    mine.write_text(json.dumps(record, ensure_ascii=False, indent=0), encoding="utf-8")
    if other.exists():
        b, a = (record, json.loads(other.read_text(encoding="utf-8"))) if before else (json.loads(other.read_text(encoding="utf-8")), record)
        judged = [k for k in a if a[k] != "HOST"]
        print(f"同じ {len(judged)} 件（Typed OM ありの回が host で見た件）で：Typed OM なし 合格 "
              f"{sum(1 for k in judged if b.get(k) == 'PASS')}、Typed OM あり 合格 {sum(1 for k in judged if a[k] == 'PASS')}")
sys.exit(1 if totals["FAIL"] else 0)
