"""Filter Effects 1・2 の値（filter・backdrop-filter・color-interpolation-filters）の持ち主（cssprop.c）を、
WPT の css/filter-effects の頁のまま host で確かめる。

parsing の頁（test_valid_value・test_invalid_value・test_computed_value）、animation の頁
（test_interpolation・test_no_interpolation・test_composition）、inheritance.html（assert_inherited・
assert_not_inherited）、drop-shadow-currentcolor-inheritance.html の二つの assert_equals を node で取り出し、
cssprop.c・cssmath.c・color.c・css.c を host の clang で組んだ道具に渡して、WPT の比べ方で照らす：
指定値は CSSOM の直列化と往復、計算値は直接 css_apply に渡す道と element.style（css.c の css_inline_set）を
通す道の両方と往復、補間は interpolation-testcommon.js の normalizeValue（小数 2 桁、字句の間に空白）で、
期待はその文字列を期待の要素に置いた計算値。要素は頁の .parent・.target（または #container・#target、
#filter・#target）の宣言を持ち、container の幅は頁の #container の 100px（cqw）。

guest の WPT の代わりではない。補間は CSS Transitions・Animations・Web Animations の四つの道を一つに
まとめる（時間の模型・transition の始まりの判定は animation.c のもので、ここでは見ない）：期待一つを
一件に数え、test_no_interpolation は CSS Animations と Web Animations の 0.5 で切り替わる期待で数える。
--pixels は filter.c の画素の操作を仕様の行列と式で照らし、paint.c の面の手順（黒と白の面からの
premultiplied の色、filter の後の不透明度と clip の mask を通した合成、backdrop の角の丸い border box を
通した書き戻し）を paint.c の源から取り出して式で照らす（描画の歩き・面の大きさ・損傷は guest のもの）。

  python -X utf8 tests/check_filter_effects.py [--verbose] [--file 名前の一部] [--pixels]
                                               [--before --source 前の版の源の場所]
--before は Filter Effects 1 の前の版（getComputedStyle の filter を js.c が直列化した版）の道で、
--source でその版の user/・common/・third_party/quickjs/ を渡す（git archive で取り出したもの）。
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt")) / "css/filter-effects"
pages = ([p for p in sorted((wpt / "parsing").glob("*.html"))
          if p.name.startswith(("filter-", "backdrop-filter-", "color-interpolation-filters-"))] +
         sorted((wpt / "animation").glob("*.html")) +
         [wpt / "inheritance.html", wpt / "drop-shadow-currentcolor-inheritance.html"])
properties = ("filter", "backdrop-filter", "color-interpolation-filters")
out = root / "build/check-filter-effects"
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
for header in ("setjmp.h",):
    (abi / header).write_text('#include "' + (cc.parent.parent / "include" / header).as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
verbose = "--verbose" in sys.argv
only = sys.argv[sys.argv.index("--file") + 1] if "--file" in sys.argv else None
# --source DIR: the product's sources from DIR (another version's user/, common/ and third_party/quickjs/),
# the requests' tool from this tree
source = Path(sys.argv[sys.argv.index("--source") + 1]) if "--source" in sys.argv else root


def build(sources, exe, extra=()):
    """the tool from sources; every external the requests never reach stops with its name"""
    command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
               "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc",
               "-Iuser/libc/web", "-Ithird_party/quickjs", *extra, *sources, str(exe.with_suffix(".unreached.c")),
               "-o", str(exe)]
    undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
    unreached = set()
    for attempt in range(8):
        exe.with_suffix(".unreached.c").write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
            f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(unreached)), encoding="utf-8")
        r = subprocess.run(command, cwd=source, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode == 0:
            return
        found = set(undefined.findall(r.stdout + r.stderr)) - unreached
        if not found:
            sys.exit(r.stdout + r.stderr)
        unreached |= found
    sys.exit("道具を組めない")


# ---------------------------------------------------------------- pixels (filter.c)
if "--pixels" in sys.argv:
    # paint.c's steps around filter.c, taken from its source as they are
    paint = (source / "user/libc/web/paint.c").read_text(encoding="utf-8")

    def between(first, stop):
        start = paint.index(first)
        return paint[start:paint.index(stop, start)]

    def function(signature):
        start = paint.index(signature)
        return paint[start:paint.index("\n}", start) + 2] + "\n"
    pieces = ('#include <math.h>\n#include <stdlib.h>\n#include <nocturne.h>\n#include <parallel.h>\n#include "webi.h"\n' +
              between("struct border_radii {", "\n") + "\n" + function("static float sdf_rrect(") +
              function("static float coverage(") + function("static float corners_coverage(") +
              between("struct transformed_job {", "/* Bilinear samples") +
              between("struct filtered_job {", "/* A surface past this many pixels") +
              between("struct backdrop_job {", "/* Filter Effects 2 §2.1 and §3:"))
    (out / "paint_pieces.h").write_text("#pragma once\n" + pieces, encoding="utf-8")
    exe = out / "filter-pixels.exe"
    build([str(root / "tests/filter_effects_pixels.c"), "user/libc/web/filter.c", "user/libc/web/color.c", "user/libc/web/cssmath.c",
           "user/libc/web/util.c", "common/gfx.c"], exe, ["-I" + str(out)])
    r = subprocess.run([str(exe)], cwd=root, env=env, capture_output=True, text=True, encoding="utf-8", timeout=300)
    print(r.stdout, end="")
    sys.exit(r.returncode)

exe = out / "filter-effects.exe"
# --before: the product before Filter Effects 1, whose getComputedStyle (js.c) serialized filter outside the table
build([str(root / "tests/filter_effects_native_cases.c"), "user/libc/web/color.c", "user/libc/web/cssmath.c", "user/libc/web/cssprop.c",
       "user/libc/web/shape.c", "user/libc/web/util.c", "user/libc/web/html.c", "user/libc/web/css.c"], exe,
      ["-DFILTER_BEFORE"] if "--before" in sys.argv else [])

node = None
for candidate in ("node", "C:/nvm4w/nodejs/node.exe", "C:/Program Files/nodejs/node.exe"):
    try:
        subprocess.run([candidate, "--version"], capture_output=True, check=True)
        node = candidate
        break
    except (OSError, subprocess.CalledProcessError):
        pass
if not node:
    sys.exit("node が見つからない")

# The page's scripts run in node with the harness functions recording their calls.
collector = r"""
const fs = require('fs');
const calls = [];
const neutral = {};
globalThis.neutralKeyframe = neutral;
const end = v => v === neutral ? null : v;
globalThis.test_valid_value = function (p, v, s) { calls.push({kind: 'valid', args: [p, v, arguments.length < 3 ? v : s]}); };
globalThis.test_invalid_value = (p, v) => calls.push({kind: 'invalid', args: [p, v]});
globalThis.test_computed_value = function (p, v, c) { calls.push({kind: 'computed', args: [p, v, c || v]}); };
globalThis.test_interpolation = function (o, ex) {
  for (const e of ex) calls.push({kind: 'interpolation', args: [o.property, end(o.from), end(o.to), e.at, end(e.expect)]});
};
// interpolation-testcommon.js expectFlip, as CSS Animations and Web Animations expect it
globalThis.test_no_interpolation = function (o) {
  for (const at of [-0.3, 0, 0.3, 0.5, 0.6, 1, 1.5])
    calls.push({kind: 'interpolation', args: [o.property, end(o.from), end(o.to), at, at < 0.5 ? end(o.from) : end(o.to)]});
};
globalThis.test_composition = function (o, ex) {
  const from = o.accumulateFrom || o.addFrom || o.replaceFrom, to = o.accumulateTo || o.addTo || o.replaceTo;
  const fc = 'accumulateFrom' in o ? 'c' : 'addFrom' in o ? 'a' : 'r', tc = 'accumulateTo' in o ? 'c' : 'addTo' in o ? 'a' : 'r';
  for (const e of ex) calls.push({kind: 'composition', args: [o.property, o.underlying, from, fc, to, tc, e.at, e.expect]});
};
globalThis.assert_inherited = (p, i, o) => calls.push({kind: 'inherited', args: [p, i, o]});
globalThis.assert_not_inherited = (p, i, o) => calls.push({kind: 'not_inherited', args: [p, i, o]});
globalThis.test = () => calls.push({kind: 'other', args: []});
globalThis.promise_test = globalThis.async_test = globalThis.test;
globalThis.setup = globalThis.add_completion_callback = () => {};
const element = new Proxy(function () {}, {get: () => element, apply: () => element, set: () => true});
globalThis.document = new Proxy({}, {get: () => () => element});
globalThis.window = globalThis;
globalThis.getComputedStyle = () => element;
globalThis.CSS = {supports: () => true};
for (const script of JSON.parse(fs.readFileSync(0, 'utf8'))) {
  try { (0, eval)(script); } catch (e) { calls.push({kind: 'error', args: [String(e)]}); }
}
process.stdout.write(JSON.stringify(calls));
"""
(out / "collect.js").write_text(collector, encoding="utf-8")


def rules(page):
    """the page's style rules: selector -> declarations (name, value)"""
    found = {}
    for style in re.findall(r"<style[^>]*>(.*?)</style>", page, re.S):
        style = re.sub(r"/\*.*?\*/", "", style, flags=re.S)
        for selector, body in re.findall(r"([^{}]+)\{([^{}]*)\}", style):
            decls = []
            for decl in body.split(";"):
                if ":" in decl:
                    name, value = decl.split(":", 1)
                    decls.append((name.strip().lower(), value.strip()))
            for one in selector.split(","):
                found.setdefault(one.strip(), []).extend(decls)
    return found


def decl_text(decls):
    return ";".join(f"{n}:{v}" for n, v in decls)


class Tool:
    """the requests of a whole pass go to the tool at once"""
    def run(self, requests):
        lines = ("\t".join(r).replace("\n", " ") + "\n" for r in requests)
        (out / "requests.txt").write_text("".join(lines), encoding="utf-8")
        subprocess.run([str(exe), str(out / "requests.txt"), str(out / "answers.txt")], cwd=root, env=env,
                       check=True, timeout=600)
        return (out / "answers.txt").read_text(encoding="utf-8").split("\n")[:len(requests)]


def round_numbers(text):
    """interpolation-testcommon.js roundNumbers: numbers with a fraction to two places"""
    def one(m):
        v = f"{float(m.group(0)):.2f}"
        v = re.sub(r"\.\d+", lambda d: d.group(0).rstrip("0"), v)
        v = re.sub(r"\.$", "", v)
        return "0" if v == "-0" else v
    return re.sub(r"-?\d*\.\d+(e-?\d+)?", one, text)


def normalize(text):
    """interpolation-testcommon.js normalizeValue (its URLs are the page's own: none here are relative)"""
    return re.sub(r"\s+", " ", re.sub(r"([\w\d.]+|[^\s])", r"\1 ", round_numbers(text)))


# one case: (page, title, [requests], judge(answers) -> (ok, detail))
cases = []
for path in pages:
    if only and only not in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    scripts = re.findall(r"<script(?![^>]*src=)[^>]*>(.*?)</script>", page, re.S)
    found = rules(page)
    if path.name == "drop-shadow-currentcolor-inheritance.html":
        parent, target = found.get("#filter", []), found.get("#target", [])
        expects = re.findall(r'assert_equals\(getComputedStyle\(target\)\.filter, "([^"]*)"\)', page)
        changes = [None, "blue"]
        for want, color in zip(expects, changes):
            t = [d for d in target if d[0] != "color"] + [("color", color)] if color else target
            req = ("C", "filter", decl_text(parent), decl_text(t), "")
            cases.append((path.name, f"currentcolor {color or ''}", [req],
                          lambda a, want=want: (a[0] == want, f"want {want!r} got {a[0]!r}")))
        continue
    calls = json.loads(subprocess.run([node, str(out / "collect.js")], input=json.dumps(scripts), text=True,
                                      capture_output=True, encoding="utf-8", check=True).stdout)
    # the elements: an interpolation page's .parent and .target, a parsing page's #container and #target;
    # the parent inherits the body's color (the elements are the body's descendants)
    inherited = [d for d in found.get("body", []) if d[0] == "color"]
    parent = found.get(".parent", []) + found.get("#container", [])
    target = found.get(".target", []) + found.get("#target", [])
    parent_text = decl_text([d for d in parent if d[0] in properties or d[0] == "color"] +
                            [d for d in inherited if not any(p[0] == "color" for p in parent)])
    for call in calls:
        kind, args = call["kind"], call["args"]
        if kind in ("other", "error") or args[0] not in properties:
            continue
        prop = args[0]
        own = [d for d in target if d[0] != prop and (d[0] in properties or d[0] == "color")]
        own_value = next((v for n, v in reversed(target) if n == prop), None)
        target_text = decl_text(own)
        if kind == "valid":
            value, want = args[1], args[2]
            def judge(a, want=want, value=value):
                ok = a[0] != "<invalid>" and (a[0] in want if isinstance(want, list) else a[0] == want)
                return ok, f"want {want!r} got {a[0]!r}"
            cases.append((path.name, f"valid {value}", [("S", prop, value)], judge))
        elif kind == "invalid":
            cases.append((path.name, f"invalid {args[1]}", [("S", prop, args[1])],
                          lambda a: (a[0] == "<invalid>", f"got {a[0]!r}")))
        elif kind == "computed":
            value, want = args[1], args[2]
            reqs = [("C", prop, parent_text, target_text, value), ("O", prop, parent_text, target_text, value)]
            def judge(a, want=want, value=value):
                ok = a[0] != "<invalid>" and (a[0] in want if isinstance(want, list) else a[0] == want)
                through = a[1] == a[0]
                return ok and through, f"want {want!r} got {a[0]!r} element.style {a[1]!r}"
            cases.append((path.name, f"computed {value}", reqs, judge))
        elif kind in ("interpolation", "composition"):
            if kind == "interpolation":
                prop, frm, to, at, expect = args
                underlying, fc, tc = "", "r", "r"
            else:
                prop, underlying, frm, fc, to, tc, at, expect = args
            # a neutral keyframe is the underlying value: the target's own (or the underlying set inline)
            neutral = underlying or own_value or "unset"
            frm = neutral if frm is None else frm
            to = neutral if to is None else to
            # the underlying value: set inline by a composition test, else the target's own
            underlying_value = underlying or own_value or ""
            req = ("I", prop, parent_text, target_text, underlying_value, frm, fc, to, tc, repr(float(at)))
            # the expected element: the expectation set inline (a neutral one leaves the target's own)
            exp = ("C", prop, parent_text, target_text, (own_value or "") if expect is None else expect)
            def judge(a, expect=expect):
                ok = a[0] != "<invalid>" and a[1] != "<invalid>" and normalize(a[0]) == normalize(a[1])
                return ok, f"want {a[1]!r} ({expect!r}) got {a[0]!r}"
            cases.append((path.name, f"{kind} {frm} → {to} @{at}", [req, exp], judge))
        elif kind in ("inherited", "not_inherited"):
            initial, other = args[1], args[2]
            c = lambda v: ("C", prop, "", "", v)
            t = lambda pv, tv: ("C", prop, f"{prop}:{pv}", "", tv)
            cases.append((path.name, f"{prop} initial", [c("initial")],
                          lambda a, initial=initial: (a[0] == initial, f"want {initial!r} got {a[0]!r}")))
            if kind == "inherited":
                reqs = [c("initial"), t("initial", "unset"), c(other), t(other, "unset"), t(other, "initial"),
                        t(other, "inherit")]
                judge = lambda a, o=other: (a[0] != o and a[1] != o and a[2] == o and a[3] == o and a[4] != o and a[5] == o,
                                            repr(a))
            else:
                reqs = [c("initial"), t("initial", "unset"), c(other), t(other, "unset"), t(other, "inherit")]
                judge = lambda a, o=other: (a[0] != o and a[1] != o and a[2] == o and a[3] != o and a[4] == o, repr(a))
            cases.append((path.name, f"{prop} {kind}", reqs, judge))

tool = Tool()
flat = [r for case in cases for r in case[2]]
answers = tool.run(flat)
# a value that passed must round-trip: set the serialization again (test_valid_value, test_computed_value)
at = 0
report, failures, again, again_of = {}, {}, [], {}
results = []
for i, (name, title, reqs, judge) in enumerate(cases):
    got = answers[at:at + len(reqs)]
    at += len(reqs)
    ok, detail = judge(got)
    results.append([ok, detail])
    if ok and reqs[0][0] == "S" and not title.startswith("invalid"):
        again_of[i] = len(again)
        again.append(("S", reqs[0][1], got[0]))
    elif ok and reqs[0][0] == "C" and title.startswith("computed") and got[0] != reqs[0][4]:
        again_of[i] = len(again)
        again.append(("C",) + reqs[0][1:4] + (got[0],))
round_trip = tool.run(again) if again else []
for i, (name, title, reqs, judge) in enumerate(cases):
    ok, detail = results[i]
    if i in again_of:
        first = answers[sum(len(c[2]) for c in cases[:i])]
        back = round_trip[again_of[i]]
        if back != first:
            ok, detail = False, detail + f"; round trip {back!r}"
    good, count = report.get(name, (0, 0))
    report[name] = (good + ok, count + 1)
    if not ok:
        failures.setdefault(name, []).append((title, detail))
for name, (good, count) in report.items():
    print(f"{name}: {good}/{count}")
    if verbose:
        for title, detail in failures.get(name, [])[:400]:
            print(f"    {title}")
            print(f"      {detail}")
passed = sum(g for g, c in report.values())
total = sum(c for g, c in report.values())
print(f"合計 {passed}/{total}")
(out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
