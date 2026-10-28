"""CSS の色の持ち主（color.c）を、性質の表（cssprop.c）を通して host で確かめる。

WPT の css/css-color/parsing、css-color-adjust/parsing（color-scheme）、
css-color の欠けた成分と補間の頁から、試験の呼出し（test_valid_value・
test_invalid_value・test_computed_value と色の fuzzy の版、test_interpolation）を、
html/canvas の fill-and-stroke-styles から fillStyle・strokeStyle の文字の期待を
node で取り出し、color.c・cssprop.c を host の clang で組んだ道具に渡して、WPT の
比べ方（数の誤差と形、補間は小数 2 桁）で照らす。CSSOM・cascade・var() の全部を通す
guest の WPT の代わりではない：var() は頁の :root と html の custom property だけを
置き換え、要素は「親の color が rgb(255, 0, 0)」の一つだけ、補間は端が値のものだけ。

  python -X utf8 tests/check_css_color.py [--verbose] [--file 名前の一部]
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path("D:/Programes/cloned repos/wpt/css")
pages = (sorted((wpt / "css-color/parsing").glob("*.html")) + sorted((wpt / "css-color-adjust/parsing").glob("color-scheme-*.html")) +
         [wpt / "css-color/color-mix-missing-components.html", wpt / "css-color/animation/color-interpolation.html"])
out = root / "build/check-css-color"
out.mkdir(parents=True, exist_ok=True)
# the repository's tools (a worktree has them in the main checkout above it)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
for header in ("setjmp.h",):
    (abi / header).write_text('#include "' + (cc.parent.parent / "include" / header).as_posix() + '"\n', encoding="utf-8")

exe = out / "css-color.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc",
           "-Iuser/libc/web", "-Ithird_party/quickjs", "tests/css_color_native_cases.c", "user/libc/web/color.c",
           "user/libc/web/cssprop.c", "user/libc/web/shape.c", "user/libc/web/util.c", "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)

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
globalThis.test_valid_value = function (p, v, s, o) {
  calls.push({kind: 'valid', args: [p, v, arguments.length < 3 ? v : s, o && o.comparisonFunction ? o.comparisonFunction.epsilon : null]});
};
globalThis.test_invalid_value = (p, v) => calls.push({kind: 'invalid', args: [p, v]});
// the element's own color, which a page sets before a test of another property
let own_color = '';
globalThis.target = {style: new Proxy({}, {set: (o, k, v) => { if (k === 'color') own_color = v; return true; },
                                           get: () => ''})};
globalThis.test_computed_value = function (p, v, c, t, o) {
  calls.push({kind: 'computed', args: [p, v, c || v, o && o.comparisonFunction ? o.comparisonFunction.epsilon : null,
                                       own_color]});
};
globalThis.set_up_fuzzy_color_test = e => { const f = () => {}; f.epsilon = e || 0.01; return f; };
globalThis.fuzzy_test_valid_color = (v, p, e) => p ? test_valid_value('color', v, p, {comparisonFunction: set_up_fuzzy_color_test(e)}) : test_valid_value('color', v);
globalThis.fuzzy_test_valid_color_property = (q, v, p, e) => p ? test_valid_value(q, v, p, {comparisonFunction: set_up_fuzzy_color_test(e)}) : test_valid_value(q, v);
globalThis.fuzzy_test_computed_color = (v, c, e) => c ? test_computed_value('color', v, c, undefined, {comparisonFunction: set_up_fuzzy_color_test(e)}) : test_computed_value('color', v);
globalThis.fuzzy_test_computed_color_property = (q, v, c, e) => c ? test_computed_value(q, v, c, undefined, {comparisonFunction: set_up_fuzzy_color_test(e)}) : test_computed_value(q, v);
// endpoints that are values; neutral keyframes and CSS-wide keywords need the cascade
globalThis.neutralKeyframe = {};
globalThis.test_interpolation = function (spec, expectations) {
  for (const e of expectations)
    calls.push({kind: 'interpolation', args: [spec.property, spec.from, spec.to, e.at, e.expect]});
};
globalThis.test = () => calls.push({kind: 'other', args: []});
globalThis.promise_test = globalThis.async_test = globalThis.test;
globalThis.setup = globalThis.add_completion_callback = () => {};
// any element's style: a page's own target element sets its color through it
const element = new Proxy(function () {}, {get: () => element, apply: () => element,
                                           set: (o, k, v) => { if (k === 'color') own_color = v; return true; }});
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

def custom_properties(page):
    props = {}
    for style in re.findall(r"<style[^>]*>(.*?)</style>", page, re.S):
        for name, value in re.findall(r"(--[\w-]+)\s*:\s*([^;]+);", style):
            props[name] = value.strip()
    return props

def substitute(value, props, depth=0):
    if "var(" not in value or depth > 8:
        return value
    def one(m):
        name, fallback = m.group(1), m.group(2)
        if name in props:
            return props[name]
        return fallback.strip() if fallback is not None else "\0"
    value = re.sub(r"var\(\s*(--[\w-]+)\s*(?:,\s*([^()]*))?\)", one, value)
    return substitute(value, props, depth + 1)

number = re.compile(r"[0-9.]")
divider = re.compile(r"( |\(|,)")
def numbers(text):
    result = []
    for part in divider.split(text):
        m = re.match(r"\s*[+-]?(\d+\.?\d*(e[+-]?\d+)?|\.\d+(e[+-]?\d+)?)", part)
        if m:
            result.append(float(m.group(0)))
        elif re.match(r"\s*-?infinity|\s*NaN", part, re.I):
            pass
    return result

def fuzzy(actual, expected, epsilon):
    a, e = numbers(actual), numbers(expected)
    if len(a) != len(e) or any(abs(x - y) > epsilon for x, y in zip(a, e)):
        return False
    return number.sub("", actual) == number.sub("", expected)

def matches(actual, expected, epsilon):
    if isinstance(expected, list):
        return any(matches(actual, x, epsilon) for x in expected)
    if epsilon is not None:
        return fuzzy(actual, expected, epsilon)
    return actual == expected

class Tool:
    """the requests of a whole pass go to the tool at once"""
    def run(self, requests):
        lines = ("\t".join(r).replace("\n", " ") + "\n" for r in requests)
        (out / "requests.txt").write_text("".join(lines), encoding="utf-8")
        subprocess.run([str(exe), str(out / "requests.txt"), str(out / "answers.txt")], cwd=root, env=env,
                       check=True, timeout=300)
        return (out / "answers.txt").read_text(encoding="utf-8").split("\n")[:len(requests)]

tool = Tool()
only = sys.argv[sys.argv.index("--file") + 1] if "--file" in sys.argv else None
verbose = "--verbose" in sys.argv
cases = []  # (file, kind, property, value, request, want, epsilon, scheme)
skipped = {}
for path in pages:
    if only and only not in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    scripts = [s for s in re.findall(r"<script(?![^>]*src=)[^>]*>(.*?)</script>", page, re.S)]
    calls = json.loads(subprocess.run([node, str(out / "collect.js")], input=json.dumps(scripts), text=True,
                                      capture_output=True, encoding="utf-8", check=True).stdout)
    props = custom_properties(page)
    scheme = "dark" if re.search(r"color-scheme\s*:\s*dark", page) else ""
    for call in calls:
        kind, args = call["kind"], call["args"]
        if kind not in ("valid", "invalid", "computed", "interpolation"):
            continue
        prop, value = args[0], args[1]
        if prop not in ("color", "background-color", "border-top-color", "outline-color", "fill", "stroke", "stop-color",
                        "color-scheme"):
            continue
        if kind == "interpolation":
            ends = args[1:3]
            if any(not isinstance(e, str) or e in ("initial", "inherit", "unset", "revert") for e in ends):
                skipped["補間の端"] = skipped.get("補間の端", 0) + 1
                continue
            cases.append((path.name, kind, prop, f"{ends[0]} → {ends[1]} @{args[3]}", ("I", ends[0], ends[1], str(args[3])),
                          args[4], None, ""))
            continue
        if "cqw" in value:
            skipped["cqw"] = skipped.get("cqw", 0) + 1
            continue  # the page resizes its container between these: the guest's to test
        if kind == "computed":
            text = substitute(value, props)
            request = ("C", prop, scheme, args[4] or "", text.replace("\0", "var(--missing)"))
        elif "var(" in value:
            skipped["var()"] = skipped.get("var()", 0) + 1
            continue  # the CSSOM keeps a value with var() as written: not the colour owner's
        elif value.strip().lower() in ("inherit", "initial", "unset", "revert", "revert-layer"):
            skipped["CSS 全体の語"] = skipped.get("CSS 全体の語", 0) + 1
            continue  # the CSSOM keeps them as written: not the colour owner's
        else:
            request = ("S", prop, value)
        cases.append((path.name, kind, prop, value, request, None if kind == "invalid" else args[2],
                      None if kind == "invalid" else args[3], scheme))
# HTML's canvas: what fillStyle and strokeStyle return after a string is set
canvas_pages = sorted((wpt.parent / "html/canvas/element/fill-and-stroke-styles").glob("2d.*Style*.html"))
setting = re.compile(r"""ctx\.(?:fill|stroke)Style = (["'])(.*?)\1;\s*_assertSame\(ctx\.(?:fill|stroke)Style, (["'])(.*?)\3""")
for path in canvas_pages if not only or only.startswith("2d.") else []:
    if only and only not in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    element = re.search(r"canvas\.setAttribute\('style', 'color: ([^']*)'\)", page)
    for m in setting.finditer(page):
        cases.append((path.name, "canvas", "fillStyle", m.group(2), ("H", element.group(1) if element else "", m.group(2)),
                      m.group(4), None, ""))
# an interpolation's expectation is the computed value of what it names
expect_index = {}
expected_requests = []
for i, c in enumerate(cases):
    if c[1] == "interpolation":
        expect_index[i] = len(expected_requests)
        expected_requests.append(("C", "color", "", "", c[5]))
answers = tool.run([c[4] for c in cases] + expected_requests)
expected_answers = answers[len(cases):]
answers = answers[:len(cases)]

def round_numbers(text):
    """interpolation-testcommon.js: numbers with a fraction to two places"""
    def one(m):
        v = f"{float(m.group(0)):.2f}"
        v = re.sub(r"\.\d+", lambda d: d.group(0).rstrip("0"), v)
        v = re.sub(r"\.$", "", v)
        return "0" if v == "-0" else v
    return re.sub(r"-?\d*\.\d+(e-?\d+)?", one, text)
# a value that passed must round-trip: set the serialization again
again_index = {}
again = []
for i, (name, kind, prop, value, request, want, epsilon, scheme) in enumerate(cases):
    got = answers[i]
    if kind in ("interpolation", "canvas"):
        continue
    if kind != "invalid" and got != "<invalid>" and matches(got, want, epsilon) and (kind == "valid" or got != value):
        again_index[i] = len(again)
        again.append(("S", prop, got) if kind == "valid" else ("C", prop, scheme, request[3], got))
round_trip = tool.run(again) if again else []
report = {}
failures = {}
round_trips = []
for i, (name, kind, prop, value, request, want, epsilon, scheme) in enumerate(cases):
    got = answers[i]
    if kind == "invalid":
        ok = got == "<invalid>"
    elif kind == "interpolation":
        want = expected_answers[expect_index[i]]
        ok = got != "<invalid>" and want != "<invalid>" and round_numbers(got) == round_numbers(want)
    elif kind == "canvas":
        ok = got == want
    else:
        ok = got != "<invalid>" and matches(got, want, epsilon)
        # beyond WPT: the serialization parses back to itself (parsing clamps what mixing may not)
        if ok and i in again_index and round_trip[again_index[i]] != got:
            round_trips.append((name, value, got, round_trip[again_index[i]]))
    good, count = report.get(name, (0, 0))
    report[name] = (good + ok, count + 1)
    if not ok:
        failures.setdefault(name, []).append((kind, value, want, got))
for name, (good, count) in report.items():
    print(f"{name}: {good}/{count}")
    if verbose:
        for kind, value, want, got in failures.get(name, [])[:400]:
            print(f"    {kind}: {value!r}")
            print(f"      want {want!r}")
            print(f"      got  {got!r}")
for name, value, got, back in round_trips:
    print(f"往復で変わる（WPT の外）：{name}: {value!r} → {got!r} → {back!r}")
passed = sum(g for g, c in report.values())
total = sum(c for g, c in report.values())
print(f"合計 {passed}/{total}（数えない：{skipped}）")
(out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
