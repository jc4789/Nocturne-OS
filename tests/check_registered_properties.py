"""登録された custom property（CSS Properties and Values API 1：CSS.registerProperty()・@property）、
CSS Values 5 の attr() と木を数える関数（sibling-index()・sibling-count()）の持ち主（css.c の登録・置き換え・
cascade、cssprop.c の構文と計算値、cssmath.c の木を数える葉、cssom.c の CSSPropertyRule）を、WPT の頁のまま、
WPT の testharness.js で host の QuickJS の一つの realm に当てる。

頁の markup は dom.c の節にし（<template shadowrootmode> は影の木）、<style> の文字は css.c の表になり、
getComputedStyle() は css.c の cascade の計算値を読む（tests/css_cascade_host.c）。頁の台本（<script src>、
<script> の中身）は markup を組んだ後に順に走る。頁より先に読むのは、代役の tests/css_cascade_host.js
（文書と要素の形、時計）と製品の js_dom_exception.js・js_css_supports.js（CSS.supports()・
CSS.registerProperty()）。

guest の WPT の代わりではない。getComputedStyle は計算値で、js.c の解決値（使用値の幅、行列）ではない。
配置・描画・動き（element.animate、遷移、Typed OM の computedStyleMap）・innerHTML に触れた件は
「host では見ない」に数え、合否に入れない。

--old は変更前の源（既定 9e120cd7）を build の下に取り出し、同じ頁を同じ道具で走らせる（その源に無い
css.c の口は道具の側で前の口に読み替え、CSS.registerProperty はその源の js_css_supports.js に無い）。

  python -X utf8 tests/check_registered_properties.py [--old [版]] [--verbose] [頁の名の一部]...
"""
from html.parser import HTMLParser
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt"))
argv = sys.argv[1:]
old = None
if "--old" in argv:
    i = argv.index("--old")
    old = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith("--") and re.fullmatch(r"[0-9a-f]{7,40}", argv[i + 1]) else "9e120cd7"
    del argv[i:i + (2 if i + 1 < len(argv) and argv[i + 1] == old else 1)]
verbose = "--verbose" in argv
filters = [a for a in argv if not a.startswith("--")]
out = root / "build/check-registered-properties" / ("old-" + old if old else "new")
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
source = root
if old:
    source = out / "src"
    if not (source / "user/libc/web/css.c").exists():
        archive = out / "old.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive), old, "user/libc/web", "user/libc/parallel.c",
                        "user/include", "common"], cwd=root, check=True)
        with tarfile.open(archive) as t:
            t.extractall(source, filter="data")
web = source / "user/libc/web"
exe = out / "css_cascade_hosttest.exe"

# ---------------------------------------------------------------- the tool
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
lexbor = [str(root / line.split("+=")[1].strip())
          for line in (root / "third_party/lexbor/sources.mk").read_text("utf-8").splitlines()
          if line.startswith("LXB_VENDOR_C") and "+=" in line and
          any(f"/lexbor/{m}/" in line for m in ("core", "css"))] + [str(root / "ports/lexbor/memory.c")]
quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
objects = out / "obj"
objects.mkdir(exist_ok=True)
shared = root / "build/check-registered-properties/obj-shared"
shared.mkdir(exist_ok=True)

# the engine's headers: a unit that includes them is compiled again when one is newer (a style_t or
# web_doc of another shape in a stale object reads the wrong fields)
engine_headers = max(h.stat().st_mtime for d in (web, source / "user/include", source / "common") for h in d.glob("*.h"))

def compile_once(src, flags, name, where=objects):
    """an object for src, compiled again only when the source (or, for the engine, a header) is newer"""
    obj = where / (name + ".o")
    newest = max(Path(src).stat().st_mtime, engine_headers if flags is engine_flags else 0)
    if not obj.exists() or obj.stat().st_mtime < newest:
        subprocess.run([str(cc), *flags, "-c", str(src), "-o", str(obj)], cwd=root, env=env, check=True)
    return str(obj)

host_flags = ["-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"',
              "-I" + str(root / "third_party/quickjs")]
lexbor_flags = ["-O2", "-std=gnu11", "-w", "-DLEXBOR_STATIC", "-I" + str(root / "third_party/lexbor/source"),
                "-I" + str(root / "ports/lexbor/include")]
engine_flags = ["-std=gnu11", "-O1", "-w", "-DLEXBOR_STATIC", "-I" + str(abi), "-I" + str(source / "user/include"),
                "-I" + str(source / "common"), "-I" + str(source / "user/libc"), "-I" + str(web),
                "-I" + str(root / "third_party/quickjs"), "-I" + str(root / "third_party/lexbor/source"),
                "-I" + str(root / "ports/lexbor/include")] + (["-DOLD_SOURCE"] if old else [])
units = [compile_once(s, host_flags, "qjs_" + Path(s).stem, shared) for s in quickjs]
units += [compile_once(s, lexbor_flags, "lxb_" + "_".join(Path(s).parts[-2:]).replace(".c", ""), shared) for s in lexbor]
engine = ["css.c", "cssom.c", "cssmath.c", "cssprop.c", "color.c", "shape.c", "util.c", "html.c", "dom.c"]
units += [compile_once(web / s, engine_flags, "web_" + Path(s).stem) for s in engine]
units.append(compile_once(root / "tests/css_cascade_host.c", engine_flags, "cascade_host"))
units.append(compile_once(root / "tests/css_cascade_hosttest.c", host_flags, "harness", shared))  # the host's stdio
stubs = out / "unreached.c"
undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
# what the cascade reaches outside these units, answered as a document without scripts, fonts, forms,
# animations or layout answers it; everything else it links but never calls stops with its name
units.append(compile_once(root / "tests/css_cascade_host_stubs.c", engine_flags, "cascade_host_stubs"))
names = set()
for attempt in range(10):
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
HOST_REACH = re.compile(r"host: not available")
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
HEAD = {"style", "link", "meta", "title", "script", "base"}

class Markup(HTMLParser):
    """the page's elements and texts as [name, attributes, children]: the head's and the body's"""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.head, self.body = ["head", {}, []], ["body", {}, []]
        self.stack, self.in_body, self.script = [], False, 0
    def container(self):
        return self.stack[-1][2] if self.stack else (self.body if self.in_body else self.head)[2]
    def handle_starttag(self, tag, attrs):
        if tag == "html":
            return
        if tag == "head" and not self.in_body:
            return
        if tag == "body":
            self.in_body = True
            self.body[1].update({k: v or "" for k, v in attrs})
            return
        if not self.stack and not self.in_body and tag not in HEAD:
            self.in_body = True
        if tag == "script":
            self.script += 1
            return
        node = [tag, {k: v or "" for k, v in attrs}, []]
        self.container().append(node)
        if tag not in VOID:
            self.stack.append(node)
    def handle_endtag(self, tag):
        if tag == "script":
            self.script = max(0, self.script - 1)
            return
        if tag == "head":
            return
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                return
    def handle_data(self, data):
        if self.script:
            return
        if not self.stack and not data.strip():
            return
        if not self.stack and not self.in_body:
            self.in_body = True
        self.container().append(data)

# the var() pages too: the same substitution (css.c subst) replaces var() and attr()
pages = sorted(p for p in (wpt / "css/css-properties-values-api").rglob("*.html") if "resources" not in p.parts)
pages += sorted((wpt / "css/css-values").glob("attr-*.html"))
pages += sorted((wpt / "css/css-values/tree-counting").glob("*.html"))
pages += sorted((wpt / "css/css-variables").glob("*.html"))
pages = [p for p in pages if not re.search(r"-ref\.html$", p.name) and "testharness.js" in p.read_text(encoding="utf-8")]
if filters:
    pages = [p for p in pages if any(f in p.relative_to(wpt).as_posix() for f in filters)]
product = [str(web / "js_dom_exception.js"), str(web / "js_css_supports.js")]
inline_dir = out / "inline"
inline_dir.mkdir(exist_ok=True)

def page_scripts(page, html):
    result = []
    for i, m in enumerate(re.finditer(r"<script\b([^>]*)>(.*?)</script>", html, re.S | re.I)):
        src = re.search(r"\bsrc\s*=\s*[\"']?([^\"' >]+)", m[1])
        if re.search(r"\btype\s*=\s*[\"']?(?!text/javascript|module)[\w/]+", m[1]):
            continue
        if src:
            s = src[1]
            if s.endswith(("/testharness.js", "/testharnessreport.js")):
                continue
            result.append(str(wpt / s.lstrip("/")) if s.startswith("/") else str(page.parent / s))
        elif m[2].strip():
            path = inline_dir / (page.relative_to(wpt).as_posix().replace("/", "__") + f".{i}.js")
            path.write_text(m[2], encoding="utf-8")
            result.append(str(path))
    return result

totals = {"PASS": 0, "FAIL": 0, "HOST": 0}
record = {}
silent = []
for page in pages:
    name = page.relative_to(wpt).as_posix()
    html = page.read_text(encoding="utf-8")
    markup = Markup()
    markup.feed(html)
    tree = out / (name.replace("/", "__") + ".json")
    tree.write_text(json.dumps([markup.head, markup.body]), encoding="utf-8")
    title = re.search(r"<title>(.*?)</title>", html, re.S | re.I)
    title = json.dumps(title[1].strip() if title else "")
    url = json.dumps("http://web-platform.test:8000/" + name)
    scripts = [str(root / "tests/css_cascade_host.js"), *product, str(wpt / "resources/testharness.js"), "-e", REPORT,
               "-e", f"__buildDocument(host.readFile({json.dumps(str(tree))}),{title},{url})"] + page_scripts(page, html)
    try:
        p = subprocess.run([str(exe), *scripts], capture_output=True, timeout=300, cwd=root, env=env)
        text = p.stdout.decode("utf-8", "replace")
        if p.returncode not in (0, 2):
            text += f"\nEXIT {p.returncode}"
    except subprocess.TimeoutExpired:
        text = "TIMEOUT"
    (out / (name.replace("/", "__") + ".log")).write_text(text, encoding="utf-8")
    results = [line.split(" | ", 2) for line in text.splitlines() if line.startswith("RESULT ")]
    host = [r for r in results if r[0] != "RESULT PASS" and len(r) > 2 and HOST_REACH.search(r[2])]
    for r in results:
        record[name + " | " + r[1]] = "HOST" if r in host else r[0][7:]
    judged = [r for r in results if r not in host]
    passed = sum(1 for r in judged if r[0] == "RESULT PASS")
    totals["PASS"] += passed
    totals["FAIL"] += len(judged) - passed
    totals["HOST"] += len(host)
    harness = next((line for line in text.splitlines() if line.startswith("HARNESS")), "HARNESS ? (no report)")
    note = "" if harness.startswith("HARNESS 0") else " " + harness[:160]
    if host:
        note += f"（host では見ない {len(host)}）"
    print(f"{name}: {passed}/{len(judged)}{note}")
    if verbose:
        for r in judged:
            if r[0] != "RESULT PASS":
                print("   ", " | ".join(r)[7:][:300])
    if not results:
        silent.append(name)
        if verbose:
            print("   ", text[-800:].replace("\n", "\n    "))
label = f"変更前 {old}" if old else "今の源"
print(f"登録された性質・attr()・木を数える関数（{label}）：{len(pages)} 頁、合格 {totals['PASS']}、不合格 {totals['FAIL']}、"
      f"host では見ない {totals['HOST']}、結果の無い頁 {len(silent)}")
if silent:
    print("  結果の無い頁：" + "、".join(silent))
if not filters:
    (out / "results.json").write_text(json.dumps(record, ensure_ascii=False, indent=0), encoding="utf-8")
    other = root / "build/check-registered-properties" / ("new" if old else "old-9e120cd7") / "results.json"
    if other.exists():
        o = json.loads(other.read_text(encoding="utf-8"))
        before, after = (record, o) if old else (o, record)
        judged = [k for k in after if after[k] != "HOST" and before.get(k) != "HOST"]
        print(f"同じ {len(judged)} 件（両方の回が host で見た件）で：変更前 合格 {sum(1 for k in judged if before.get(k) == 'PASS')}、"
              f"今の源 合格 {sum(1 for k in judged if after[k] == 'PASS')}")
sys.exit(1 if totals["FAIL"] else 0)
