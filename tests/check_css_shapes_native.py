"""図形の値の持ち主（shape.c）を、性質の表（cssprop.c）と動きの値の層（css_anim_*）を通して host で確かめる。

WPT の css-shapes（shape-functions・parsing・animation）、css-masking の clip-path（parsing・
animations）、motion（parsing・animation）の頁から、試験の呼出し（test_valid_value・
test_invalid_value・test_computed_value・test_interpolation・test_no_interpolation・
test_composition）を node で取り出し、shape.c・cssprop.c を host の clang で組んだ道具
（tests/css_shapes_native_cases.c）に渡して、WPT の比べ方で照らす。補間と合成は
interpolation-testcommon.js の normalizeValue（数は小数 2 桁、字句の間に空白）で、期待は
その文字の計算値と比べる。

guest の WPT の代わりではない。CSSOM の宣言の塊・cascade・var()・時間の模型（animation.c）・
配置と描画は通らない。数えないもの：test_shorthand_value（CSSOM が shorthand の longhand の
文字を持つかどうかは宣言の塊の仕事）、補間の端が neutral・CSS 全体の語（cascade の仕事）。
補間は CSS animation の流儀（離散は 0.5 で入れ替わる）だけを試す。

  python -X utf8 tests/check_css_shapes_native.py [--verbose] [--file 名前の一部] [--out 出力の場所]
"""
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt")) / "css"
pages = (sorted(p for p in (wpt / "css-shapes/shape-functions").glob("*.html") if re.search(r"-(valid|invalid|computed|parsing)\.html$", p.name)) +
         sorted((wpt / "css-shapes/parsing").glob("*.html")) +
         sorted((wpt / "css-masking/parsing").glob("clip-path-*.html")) +
         sorted((wpt / "motion/parsing").glob("*.html")) +
         sorted((wpt / "css-shapes/animation").glob("*.html")) +
         sorted((wpt / "css-masking/animations").glob("clip-path-*.html")) +
         [wpt / "css-masking/clip-path/animations/clip-path-interpolation-discrete.html"] +
         sorted((wpt / "motion/animation").glob("*.html")))
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / "build/check-css-shapes"
out.mkdir(parents=True, exist_ok=True)
# the repository's tools (a worktree has them in the main checkout above it)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
for header in ("setjmp.h",):
    (abi / header).write_text('#include "' + (cc.parent.parent / "include" / header).as_posix() + '"\n', encoding="utf-8")

exe = out / "css-shapes.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc",
           "-Iuser/libc/web", "-Ithird_party/quickjs", "tests/css_shapes_native_cases.c", "user/libc/web/shape.c",
           "user/libc/web/cssprop.c", "user/libc/web/color.c", "user/libc/web/util.c", "user/libc/web/html.c", "-o", str(exe)]
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
globalThis.test_valid_value = function (p, v, s) {
  calls.push({kind: 'valid', args: [p, v, arguments.length < 3 ? v : s]});
};
globalThis.test_invalid_value = (p, v) => calls.push({kind: 'invalid', args: [p, v]});
globalThis.test_computed_value = function (p, v, c) { calls.push({kind: 'computed', args: [p, v, c || v]}); };
const neutral = {};
globalThis.neutralKeyframe = neutral;
const keyframe = v => v === neutral || typeof v !== 'string' ? null : v;
globalThis.test_interpolation = function (o, expectations) {
  // CSS transitions do not start for a pair that does not interpolate: that is the time model's
  if (o.method && o.method !== 'CSS Animations') { calls.push({kind: 'method', args: []}); return; }
  for (const e of expectations)
    calls.push({kind: 'interpolation', args: [o.property, keyframe(o.from), keyframe(o.to), e.at, e.expect]});
};
// interpolation-testcommon.js: CSS animations flip a pair that does not interpolate at 0.5
globalThis.test_no_interpolation = function (o) {
  const flip = [[-0.3, o.from], [0, o.from], [0.3, o.from], [0.5, o.to], [0.6, o.to], [1, o.to], [1.5, o.to]];
  test_interpolation(o, flip.map(([at, expect]) => ({at, expect})));
};
globalThis.test_composition = function (o, expectations) {
  const op = (k) => 'accumulate' + k in o ? 'accumulate' : 'add' + k in o ? 'add' : 'replace';
  const from = o.accumulateFrom || o.addFrom || o.replaceFrom, to = o.accumulateTo || o.addTo || o.replaceTo;
  for (const e of expectations)
    calls.push({kind: 'composition', args: [o.property, o.underlying, op('From'), keyframe(from), op('To'), keyframe(to), e.at, e.expect]});
};
globalThis.test = () => calls.push({kind: 'other', args: []});
globalThis.test_shorthand_value = () => calls.push({kind: 'shorthand', args: []});
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

def to_fixed_2(x):
    """JavaScript's Number.prototype.toFixed(2): the nearer of two, the larger magnitude on a tie of the
    exact binary value"""
    return str(Decimal(x).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))

def round_numbers(value):
    """interpolation-testcommon.js roundNumbers: numbers with a fraction to two places"""
    def one(m):
        v = to_fixed_2(float(m.group(0)))
        v = re.sub(r"\.\d+", lambda d: d.group(0).rstrip("0"), v)
        v = re.sub(r"\.$", "", v)
        return "0" if v == "-0" else v
    return re.sub(r"-?\d*\.\d+(e-?\d+)?", one, value)

def normalize(value):
    """interpolation-testcommon.js normalizeValue"""
    value = re.sub(r"([\w\d.]+|[^\s])", r"\1 ", round_numbers(value))
    return re.sub(r"\s+", " ", value)

def matches(actual, expected):
    if isinstance(expected, list):
        return any(matches(actual, x) for x in expected)
    return actual == expected

class Tool:
    """the requests of a whole pass go to the tool at once"""
    def run(self, requests):
        lines = ("\t".join(r).replace("\n", " ") + "\n" for r in requests)
        (out / "requests.txt").write_text("".join(lines), encoding="utf-8")
        subprocess.run([str(exe), str(out / "requests.txt"), str(out / "answers.txt")], cwd=root, env=env,
                       check=True, timeout=300)
        return (out / "answers.txt").read_text(encoding="utf-8").split("\n")[:len(requests)]

def page_font(page):
    """the font-size the page gives its target (computed values of em)"""
    m = re.search(r"font-size:\s*(\d+(?:\.\d+)?)px", page)
    return m.group(1) if m else "16"

tool = Tool()
only = sys.argv[sys.argv.index("--file") + 1] if "--file" in sys.argv else None
verbose = "--verbose" in sys.argv
cases = []  # (file, kind, description, request, want)
skipped = {}
def skip(why):
    skipped[why] = skipped.get(why, 0) + 1
wide = ("initial", "inherit", "unset", "revert", "revert-layer")
for path in pages:
    if only and only not in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    scripts = re.findall(r"<script(?![^>]*src=)[^>]*>(.*?)</script>", page, re.S)
    calls = json.loads(subprocess.run([node, str(out / "collect.js")], input=json.dumps(scripts), text=True,
                                      capture_output=True, encoding="utf-8", check=True).stdout)
    font = page_font(page)
    for call in calls:
        kind, args = call["kind"], call["args"]
        if kind == "shorthand":
            skip("test_shorthand_value")
            continue
        if kind == "method":
            skip("CSS animation 以外の流儀だけの補間")
            continue
        if kind == "error":
            skip("頁の script の誤り")
            continue
        if kind not in ("valid", "invalid", "computed", "interpolation", "composition"):
            continue
        prop = args[0]
        if kind in ("valid", "invalid"):
            if "var(" in args[1] or args[1].strip().lower() in wide:
                skip("var()・CSS 全体の語")
                continue
            cases.append((path.name, kind, args[1], ("S", prop, args[1]), None if kind == "invalid" else args[2]))
        elif kind == "computed":
            cases.append((path.name, kind, args[1], ("C", prop, font, args[1]), args[2]))
        elif kind == "interpolation":
            _, a, b, at, want = args
            if a is None or b is None or a.strip() in wide or b.strip() in wide or not isinstance(want, str):
                skip("補間の端が neutral・CSS 全体の語")
                continue
            cases.append((path.name, kind, f"{a} → {b} @{at}", ("I", prop, font, a, b, repr(float(at))), want))
        else:
            _, under, from_op, a, to_op, b, at, want = args
            if a is None or b is None or not isinstance(want, str) or not isinstance(under, str):
                skip("合成の端が neutral")
                continue
            cases.append((path.name, kind, f"[{under}] {from_op} {a} → {to_op} {b} @{at}",
                          ("A", prop, font, under, from_op, a, to_op, b, repr(float(at))), want))
# an animated value's expectation is the computed value of what it names
expect_index = {}
expected_requests = []
for i, c in enumerate(cases):
    if c[1] in ("interpolation", "composition"):
        expect_index[i] = len(expected_requests)
        expected_requests.append(("C", c[3][1], c[3][2], c[4]))
answers = tool.run([c[3] for c in cases] + expected_requests)
expected_answers = answers[len(cases):]
answers = answers[:len(cases)]
# a value that passed must round-trip: its serialization parses to itself
again_index = {}
again = []
for i, (name, kind, value, request, want) in enumerate(cases):
    got = answers[i]
    if kind in ("valid", "computed") and got != "<invalid>" and matches(got, want):
        again_index[i] = len(again)
        again.append(("S", request[1], got) if kind == "valid" else ("C", request[1], request[2], got))
round_trip = tool.run(again) if again else []
report = {}
failures = {}
round_trips = []
for i, (name, kind, value, request, want) in enumerate(cases):
    got = answers[i]
    if kind == "invalid":
        ok = got == "<invalid>"
    elif kind in ("interpolation", "composition"):
        want = expected_answers[expect_index[i]]
        ok = got != "<invalid>" and want != "<invalid>" and normalize(got) == normalize(want)
    else:
        ok = got != "<invalid>" and matches(got, want)
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
(out / "report.json").write_text(json.dumps({"report": report, "failures": failures}, ensure_ascii=False, indent=1),
                                 encoding="utf-8")
sys.exit(0 if passed == total else 1)
