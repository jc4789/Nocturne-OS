"""CSS の数式の関数と単位の持ち主（cssmath.c）を、性質の表（cssprop.c）を通して host で確かめる。

WPT の css-values の頁（css/css-values/*.html）から、試験の呼出し（numeric-testcommon.js の
test_math_used・test_math_computed・test_math_specified、parsing-testcommon.js の
test_valid_value・test_invalid_value、computed-testcommon.js の test_computed_value・
testComputedValueGreaterOrLowerThan、serialize-testcommon.js の test_*_serialization）を node で
取り出し、cssmath.c・cssprop.c を host の clang で組んだ道具（tests/css_values_native_cases.c）に渡して、
WPT の比べ方で照らす。頁の中の getComputedStyle は「その値の計算値」の印として取り、道具で計算値にする。

guest の WPT の代わりではない。CSSOM の宣言の塊・cascade の全体・var()・配置・描画は通らない。
字の大きさは頁の style の #target・#container（#parent）・:root の font-size と、頁の台本が後で
#container に置くものを読む。数えないもの：性質が表に無いもの、道具が読まない性質の計算値
（transform の行列は layout.c の仕事）、使用値の % （基が頁の配置）、試験の関数が上のどれでもないもの。

  python -X utf8 tests/check_css_values_native.py [--verbose] [--file 名前の一部] [--out 出力の場所]
  （--verbose は一件ごとに PASS・FAIL の行を出す。二つの版の行を比べると、同じ件の行き来が分かる）
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt")) / "css"
pages = sorted((wpt / "css-values").glob("*.html"))
if "--file" in sys.argv:
    part = sys.argv[sys.argv.index("--file") + 1]
    pages = [p for p in pages if part in p.name]
verbose = "--verbose" in sys.argv
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / "build/check-css-values"
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
exe = out / "css-values.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc",
           "-Iuser/libc/web", "-Ithird_party/quickjs", "tests/css_values_native_cases.c", "user/libc/web/cssmath.c",
           "user/libc/web/cssprop.c", "user/libc/web/shape.c", "user/libc/web/color.c", "user/libc/web/util.c",
           "user/libc/web/html.c", "-o", str(exe)]
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

# The page's scripts run in node with the harness functions recording their calls. An element's
# style records what is set; its computed style answers a mark that names that value's computed value.
collector = r"""
const fs = require('fs');
const calls = [];
const rec = (kind, args) => calls.push({kind, args});
const MARK = '\u0001computed\u0001';
const math = stage => (t, e, o = {}) => rec('math', [stage, t, e, o.type || 'length', o.prop || '', o.approx || 0,
                                                     (o.extraStyle || {})['font-size'] || (o.extraStyle || {}).fontSize || '']);
globalThis.test_math_used = math('used');
globalThis.test_math_computed = math('computed');
globalThis.test_math_specified = math('specified');
globalThis.test_plus_infinity = t => test_math_used(t, 'calc(infinity)', {type: 'number'});
globalThis.test_minus_infinity = t => test_math_used(t, 'calc(-infinity)', {type: 'number'});
globalThis.test_plus_zero = t => test_math_used(`calc(1 / ${t})`, 'calc(infinity)', {type: 'number'});
globalThis.test_minus_zero = t => test_math_used(`calc(1 / ${t})`, 'calc(-infinity)', {type: 'number'});
globalThis.test_nan = t => { test_math_used(t, 'calc(NaN)', {type: 'number'}); test_math_used(`calc(-1 * ${t})`, 'calc(NaN)', {type: 'number'}); };
globalThis.test_valid_value = function (p, v, s) { rec('valid', [p, v, arguments.length < 3 ? v : s]); };
globalThis.test_invalid_value = (p, v) => rec('invalid', [p, v]);
globalThis.test_computed_value = (p, v, c) => rec('computed', [p, v, c || v]);
globalThis.testComputedValueGreaterOrLowerThan = (p, v, limit) => rec('bound', [p, v, limit]);
globalThis.test_specified_serialization = (p, t, e) => rec('serialization', ['specified', p, t, e]);
globalThis.test_computed_serialization = (p, t, e) => rec('serialization', ['computed', p, t, e]);
globalThis.test_used_serialization = (p, t, e) => rec('serialization', ['used', p, t, e]);
if (!globalThis.test_serialization) globalThis.test_serialization = function (t, s, c, u, {prop} = {}) {
  test_specified_serialization(prop || 'text-indent', t, s);
  test_computed_serialization(prop || 'text-indent', t, c);
  if (u) test_used_serialization(prop || 'margin-left', t, u);
};
const styles = new Map();
const element = id => {
  if (!styles.has(id)) styles.set(id, {});
  const values = styles.get(id);
  const put = (o, k, v) => { o[k] = v; if (k === 'fontSize' || k === 'font-size') rec('font', [id, String(v)]); };
  const style = new Proxy(values, {set: (o, k, v) => { put(o, k, v); return true; }, get: (o, k) => k === 'getPropertyValue' ? (p => o[p] || '') : k === 'setProperty' ? ((p, v) => put(o, p, v)) : (o[k] || '')});
  return new Proxy(function () {}, {get: (o, k) => k === 'style' ? style : k === '__values' ? values : element(id + '.' + String(k)),
                                    set: (o, k, v) => { if (k === 'style') { for (const key of Object.keys(values)) delete values[key]; } return true; },
                                    apply: () => element(id + '()')});
};
globalThis.document = new Proxy({}, {get: (o, k) => (k === 'getElementById' || k === 'querySelector') ? (id => element(String(id))) : () => element(String(k))});
globalThis.getComputedStyle = el => new Proxy({}, {get: (o, p) => MARK + String(p) + '\u0001' + (el.__values[p] || ''), has: () => true});
globalThis.window = globalThis;
globalThis.CSS = {supports: () => true};
globalThis.test = () => rec('other', []);
globalThis.promise_test = globalThis.async_test = globalThis.test;
globalThis.setup = globalThis.add_completion_callback = globalThis.assert_equals = () => {};
for (const script of JSON.parse(fs.readFileSync(0, 'utf8'))) {
  try { (0, eval)(script); } catch (e) { rec('error', [String(e)]); }
}
process.stdout.write(JSON.stringify(calls));
"""
(out / "collect.js").write_text(collector, encoding="utf-8")

def scripts_of(page):
    html = page.read_text(encoding="utf-8")
    return [m.group(1) for m in re.finditer(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", html, re.S)]

# numeric-testcommon.js: the property a type's math is tried in
props = {
    "used": {"number": "scale", "integer": "z-index", "length": "margin-left", "angle": "rotate", "time": "transition-delay",
             "resolution": "image-resolution", "flex": "grid-template-rows"},
    "computed": {"number": "scale", "integer": "z-index", "length": "flex-basis", "angle": "rotate", "time": "transition-delay",
                 "resolution": "image-resolution", "flex": "grid-template-rows"},
}
props["specified"] = props["computed"]
MARK = "\u0001computed\u0001"

class Tool:
    def __init__(self):
        self.cache = {}
    def ask(self, requests):
        todo = [r for r in dict.fromkeys(requests) if r not in self.cache]
        if todo:
            (out / "requests.txt").write_text("".join("\t".join(r).replace("\n", " ") + "\n" for r in todo), encoding="utf-8")
            subprocess.run([str(exe), str(out / "requests.txt"), str(out / "answers.txt")], cwd=root, env=env, check=True, timeout=300)
            answers = (out / "answers.txt").read_text(encoding="utf-8").split("\n")
            for r, a in zip(todo, answers):
                self.cache[r] = a
        return [self.cache[r] for r in requests]
tool = Tool()

# CSSOM: the resolved value of these is the used value
resolved_used = re.compile(r"^(width|height|(margin|padding)(-(top|right|bottom|left))?|top|right|bottom|left)$")
def font_sizes(page):
    """the font sizes in px the page's style sheets and style attributes give: selector → size"""
    html = page.read_text(encoding="utf-8")
    sizes = {}
    for sheet in re.findall(r"<style[^>]*>(.*?)</style>", html, re.S):
        for selectors, body in re.findall(r"([^{}]+)\{([^{}]*)\}", sheet):
            m = re.search(r"(?<![-\w])font-size:\s*([\d.]+)px", body)
            if m:
                for s in selectors.split(","):
                    sizes[s.strip()] = m.group(1)
    for name, style in re.findall(r"<\w+ id=\"([\w-]+)\" style=\"([^\"]*)\"", html):
        m = re.search(r"(?<![-\w])font-size:\s*([\d.]+)px", style)
        if m:
            sizes["#" + name] = m.group(1)
    return sizes

def value_of(stage, prop, v, font="16,16,16"):
    """the request that reads v at stage; a computed-style mark reads that value's computed value"""
    if isinstance(v, str) and v.startswith(MARK):
        p, _, inner = v[len(MARK):].partition("\u0001")
        prop, v, stage = p or prop, inner, "computed"
    if stage == "specified":
        return ("S", prop, v)
    op = "U" if stage == "used" or resolved_used.match(prop) else "C"
    return (op, prop, font, v)

def number(text):
    m = re.match(r"\s*(?:calc\()?\s*(-?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?|-?infinity|NaN)", text or "", re.I)
    if not m:
        return None
    t = m.group(1).lower()
    return float("inf") if t == "infinity" else float("-inf") if t == "-infinity" else float("nan") if t == "nan" else float(t)

total = passed = 0
uncounted = {}
failures = []
for page in pages:
    scripts = scripts_of(page)
    if not scripts:
        continue
    result = subprocess.run([node, str(out / "collect.js")], input=json.dumps(scripts), capture_output=True, text=True, encoding="utf-8")
    try:
        calls = json.loads(result.stdout)
    except ValueError:
        uncounted["頁が node で走らない"] = uncounted.get("頁が node で走らない", 0) + 1
        continue
    # the target's font size, its parent's (#container or #parent) and the root's, as the page
    # sets them; its script may set the parent's later
    sizes = font_sizes(page)
    root_font = sizes.get(":root") or sizes.get("html") or "16"
    parent_font = sizes.get("#container") or sizes.get("#parent") or root_font
    def site(target=""):
        return f"{target or sizes.get('#target') or parent_font},{parent_font},{root_font}"
    cases = []  # (description, [requests], judge)
    for call in calls:
        kind, a = call["kind"], call["args"]
        if kind == "font":
            name, v = a
            if name in ("container", "parent") and v.endswith("px"):
                parent_font = v.removesuffix("px")
            continue
        page_font = site()
        if kind == "math":
            stage, t, e, typ, prop, approx, font = a
            prop = prop or props[stage].get(typ, "")
            font = site(str(font).removesuffix("px"))
            def judge(answers, approx=approx):
                x, y = answers
                if "<invalid>" in (x, y) or "<unread>" in (x, y):
                    return False
                if approx:
                    nx, ny = number(x), number(y)
                    return nx is not None and ny is not None and abs(nx - ny) <= approx
                return x == y
            cases.append((f"{stage} {prop}: {t} = {e}", [value_of(stage, prop, t, font), value_of(stage, prop, e, font)], judge))
        elif kind == "valid":
            p, v, s = a
            cases.append((f"valid {p}: {v} → {s}", [("S", p, v)], lambda r, s=s: r[0] in (s if isinstance(s, list) else [s])))
        elif kind == "invalid":
            p, v = a
            cases.append((f"invalid {p}: {v}", [("S", p, v)], lambda r: r[0] == "<invalid>"))
        elif kind == "computed":
            p, v, c = a
            if isinstance(c, str) and c.startswith(MARK):
                cases.append((f"computed {p}: {v} = computed of {c}", [value_of("computed", p, v, page_font), value_of("computed", p, c, page_font)],
                              lambda r: "<invalid>" not in r and "<unread>" not in r and r[0] == r[1]))
            else:
                cases.append((f"computed {p}: {v} → {c}", [value_of("computed", p, v, page_font)],
                              lambda r, c=c: r[0] in (c if isinstance(c, list) else [c])))
        elif kind == "bound":
            p, v, limit = a
            def judge(r, limit=limit):
                n = number(r[0])
                return n is not None and (n > limit if limit > 0 else n < limit)
            cases.append((f"bound {p}: {v} beyond {limit}", [value_of("used", p, v, page_font)], judge))
        elif kind == "serialization":
            stage, p, t, e = a
            cases.append((f"{stage} {p}: {t} → {e}", [value_of(stage, p, t, page_font), value_of(stage, p, e, page_font)],
                          lambda r, e=e: r[0] == e and r[1] == e))
        else:
            uncounted["上のどれでもない試験"] = uncounted.get("上のどれでもない試験", 0) + 1
    answers = tool.ask([q for _, rs, _ in cases for q in rs])
    page_total = page_passed = 0
    at = 0
    for description, requests, judge in cases:
        got = answers[at:at + len(requests)]
        at += len(requests)
        if "<unread>" in got or any(not r[1] for r in requests):
            uncounted["道具が読まない性質"] = uncounted.get("道具が読まない性質", 0) + 1
            continue
        if any(r[0] == "U" and "%" in r[-1] and r[1] != "line-height" for r in requests):
            # a percentage's basis is the page's layout, which this tool does not have
            uncounted["% の基が頁の配置"] = uncounted.get("% の基が頁の配置", 0) + 1
            continue
        page_total += 1
        if judge(got):
            page_passed += 1
            if verbose:
                print("PASS", f"{page.name}: {description}")
        else:
            failures.append(f"{page.name}: {description}: {got}")
    if page_total:
        print(f"{page.name}: {page_passed}/{page_total}")
    total += page_total
    passed += page_passed
if verbose:
    for f in failures:
        print("FAIL", f)
print(f"合計 {passed}/{total}（数えない：{uncounted}）")
