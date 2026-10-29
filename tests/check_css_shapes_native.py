"""図形の値の持ち主（shape.c）と gap の飾りの値を、性質の表（cssprop.c）と動きの値の層（css_anim_*）を通して
host で確かめる。

WPT の css-shapes（shape-functions・parsing・animation）、css-masking の clip-path（parsing・
animations）、motion（parsing・animation）の頁から、試験の呼出し（test_valid_value・
test_invalid_value・test_computed_value・test_interpolation・test_no_interpolation・
test_composition）を node で取り出し、shape.c・cssprop.c を host の clang で組んだ道具
（tests/css_shapes_native_cases.c）に渡して、WPT の比べ方で照らす。補間と合成は
interpolation-testcommon.js の normalizeValue（数は小数 2 桁、字句の間に空白）で、期待は
その文字の計算値と比べる。--group gaps では WPT の css-gaps（parsing・animation）の頁を同じ道具で照らす
（頁の #target の font-size と color を要素に与える）。--group backgrounds では css-backgrounds の背景の層と
border-image の頁（parsing・animations）、compositing の background-blend-mode、css-masking の mask-border の
動きの頁を照らす。--group ui では css-ui の cursor・caret-color・accent-color の頁（parsing・animation）を
照らす（性質の表の G_UI の群）。
--group fonts では css-fonts の parsing・animations の頁と、根にある数式・離散の動きの頁を照らす。

guest の WPT の代わりではない。CSSOM の宣言の塊・cascade・var()・時間の模型（animation.c）・
配置と描画は通らない。数えないもの：test_shorthand_value（CSSOM が shorthand の longhand の
文字を持つかどうかは宣言の塊の仕事）、補間の端が neutral・CSS 全体の語（cascade の仕事）。
補間は CSS animation の流儀（離散は 0.5 で入れ替わる）だけを試す。

--group gaps は css.c も組み（CSS_BLOCK）、要素の CSSOM の宣言の塊（css_decl_set・css_decl_get。
shorthand は longhand を与えられた値で持つ）を js.c と同じ順で通す：test_valid_value は設定して
読み戻し、test_computed_value は塊の宣言を cascade のとおり一つずつ当てて計算値を読み、
test_shorthand_value も数える。頁の .parent と .target の規則を親と要素に当て（T の要求）、
補間の端の neutral（要素の下の値）と CSS 全体の語も数える。test() の中で DOM を読む頁
（important・from-longhands・bidirectional-shorthands・col-rule-width・roundtrip、neutral の
keyframe と conversion の頁）は、頁に書かれた入力と期待をここに写して同じ要求で照らす。
css-multicol の column-rule の頁（parsing・animation）も同じ性質なので一緒に照らす。
--rev <版> は、その版の user/libc/web・user/include・common（git archive）を今の道具で組む
（前の版と同じ道具で比べる）。

  python -X utf8 tests/check_css_shapes_native.py [--group shapes|gaps|anchor|lists|backgrounds|ui|text|decor|fonts] [--verbose] [--file 名前の一部] [--out 出力の場所] [--rev 版]
"""
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt")) / "css"
# --group anchor: CSS Anchor Positioning 1 の値（anchor-position の構文と計算値の頁）を同じ道具で
# --group text・decor: CSS Text 3/4 と CSS Text Decoration 4 の値（解析・計算値・補間の頁）を同じ道具で
group = sys.argv[sys.argv.index("--group") + 1] if "--group" in sys.argv else "shapes"
# the gaps group reads values through an element's CSSOM declaration block (css.c)
block = group == "gaps"
if group == "gaps":
    pages = (sorted((wpt / "css-gaps/parsing").glob("*.html")) + sorted((wpt / "css-gaps/animation").glob("*.html")) +
             sorted((wpt / "css-multicol/parsing").glob("column-rule*.html")) +
             sorted((wpt / "css-multicol/animation").glob("column-rule*.html")))
elif group == "anchor":
    pages = (sorted((wpt / "css-anchor-position/parsing").glob("*.html")) +
             [wpt / "css-anchor-position" / name for name in (
                 "anchor-parse-valid.html", "anchor-parse-invalid.html", "anchor-size-parse-valid.html",
                 "anchor-size-parse-invalid.html", "position-area-parsing.html", "position-area-computed.html",
                 "anchor-name-basics.html", "position-anchor-basics.html", "property-interpolations.html")])
elif group == "lists":
    # CSS Lists 3 and CSS Generated Content 3: counter-*, list-style-*, content (their syntax,
    # computed values and the discrete animation of each)
    pages = (sorted((wpt / "css-lists/parsing").glob("*.html")) + sorted((wpt / "css-content/parsing").glob("*.html")) +
             sorted((wpt / "css-lists/animations").glob("*.html")) +
             [wpt / "css-lists/css-lists-no-interpolation.html", wpt / "css-content/content-no-interpolation.html"])
elif group == "text":
    # CSS Text 3/4: line breaking, hyphenation and tab-size; text-shadow (css-text-decor and its
    # interpolation pages in css-transitions)
    names = ("hyphenate-character", "hyphenate-limit-chars", "hyphenate-limit-lines", "hyphens", "line-break",
             "overflow-wrap", "tab-size", "word-break", "word-wrap")
    pages = (sorted(p for p in (wpt / "css-text/parsing").glob("*.html") if p.name.rsplit("-", 1)[0] in names) +
             [wpt / "css-text/animations" / name for name in (
                 "hyphen-no-interpolation.html", "hyphenate-limit-chars-interpolation.html",
                 "hyphenate-limit-lines-interpolation.html", "line-break-no-interpolation.html",
                 "tab-size-interpolation.html")] +
             sorted((wpt / "css-text-decor/text-shadow/parsing").glob("*.html")) +
             [wpt / "css-transitions/animations" / name for name in (
                 "text-shadow-interpolation.html", "text-shadow-composition.html")])
elif group == "decor":
    # CSS Text Decoration 4: the text-decoration longhands and shorthand, text-underline-position and -offset
    pages = (sorted(p for p in (wpt / "css-text-decor/parsing").glob("*.html")
                    if p.name.startswith(("text-decoration-color", "text-decoration-line", "text-decoration-style",
                                          "text-decoration-computed", "text-decoration-valid", "text-decoration-invalid",
                                          "text-decoration-shorthand", "text-underline-position"))) +
             [wpt / "css-text-decor" / name for name in (
                 "text-decoration-thickness-computed.html", "text-decoration-thickness-valid.html",
                 "text-decoration-thickness-invalid.html", "text-underline-offset-computed.html",
                 "text-underline-offset-valid.html", "text-underline-offset-invalid.html")] +
             sorted((wpt / "css-text-decor/animations").glob("*.html")))
elif group == "shapes":
    pages = (sorted(p for p in (wpt / "css-shapes/shape-functions").glob("*.html") if re.search(r"-(valid|invalid|computed|parsing)\.html$", p.name)) +
             sorted((wpt / "css-shapes/parsing").glob("*.html")) +
             sorted((wpt / "css-masking/parsing").glob("clip-path-*.html")) +
             sorted((wpt / "motion/parsing").glob("*.html")) +
             sorted((wpt / "css-shapes/animation").glob("*.html")) +
             sorted((wpt / "css-masking/animations").glob("clip-path-*.html")) +
             [wpt / "css-masking/clip-path/animations/clip-path-interpolation-discrete.html"] +
             sorted((wpt / "motion/animation").glob("*.html")))
elif group == "backgrounds":
    # 背景の層の性質（attachment・clip・origin・position-x・-y・blend-mode）、border-image と mask-border
    background = wpt / "css-backgrounds"
    pages = (sorted(p for p in (background / "parsing").glob("*.html") if re.match(
                 r"(background-(attachment|clip|origin|position|computed|valid|invalid)|border-image|border-(shorthand|valid|invalid))", p.name)) +
             sorted((wpt / "compositing/parsing").glob("background-blend-mode-*.html")) +
             sorted(p for p in (background / "animations").glob("*.html") if re.match(r"(background-position|border-image)", p.name)) +
             sorted((wpt / "css-masking/animations").glob("mask-border-*.html")))
elif group == "ui":
    pages = ([wpt / "css-ui/parsing" / f"{name}-{kind}.html" for name in ("cursor", "caret-color")
              for kind in ("valid", "invalid", "computed")] +
             [wpt / "css-ui" / name for name in ("accent-color-parsing.html", "accent-color-computed.html")] +
             [wpt / "css-ui/animation" / name for name in (
                 "caret-color-interpolation.html", "caret-color-composition.html", "accent-color-interpolation.html",
                 "cursor-no-interpolation.html")])
elif group == "fonts":
    # CSS Fonts 4 の性質（font-variant の longhand と一括、feature・variation、kerning、synthesis、
    # optical-sizing、size-adjust、palette、language-override、width と別名 font-stretch、font の一括）：
    # 構文・計算値の頁と動きの頁、根にある数式と離散の頁
    fonts = wpt / "css-fonts"
    pages = (sorted((fonts / "parsing").glob("*.html")) + sorted((fonts / "animations").glob("*.html")) +
             sorted(p for p in fonts.glob("*.html") if re.search(r"(-interpolation-math-functions|-sign-function|"
                                                                 r"-calc|discrete-no-interpolation)\.html$", p.name)))
else:
    sys.exit(f"群は shapes・gaps・anchor・lists・backgrounds・ui・text・decor・fonts のどれか：{group}")
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / f"build/check-css-{group}"
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
# the engine's source: this tree's, or (--rev) a revision's beside the same cases
rev = sys.argv[sys.argv.index("--rev") + 1] if "--rev" in sys.argv else None
source = root
if rev:
    source = out / ("src-" + re.sub(r"[^\w.-]", "_", rev))
    if not (source / "user/libc/web/cssprop.c").exists():
        archive = out / "src.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive), rev, "user/libc/web", "user/include", "common"],
                       cwd=root, check=True)
        with tarfile.open(archive) as t:
            t.extractall(source, filter="data")
web = (source / "user/libc/web").as_posix()
units = ["tests/css_shapes_native_cases.c"] + [f"{web}/{u}" for u in ("shape.c", "cssprop.c", "color.c", "cssmath.c", "util.c",
                                                                      "html.c")]
command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-I" + (source / "user/include").as_posix(),
           "-I" + (source / "common").as_posix(), "-I" + (source / "user/libc").as_posix(), "-I" + web, "-Ithird_party/quickjs"]
if not block:
    subprocess.run(command + units + ["-o", str(exe)], cwd=root, env=env, check=True)
else:
    # css.c's declaration blocks too; every external the linked units reach but these requests never
    # call is a stub that stops with its name
    stubs = out / "unreached.c"
    names = set()
    undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
    for attempt in range(8):
        stubs.write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
            f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(names)), encoding="utf-8")
        r = subprocess.run(command + ["-DCSS_BLOCK", "-w"] + units + [f"{web}/css.c", str(stubs), "-lm", "-o", str(exe)],
                           cwd=root, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode == 0:
            break
        found = set(undefined.findall(r.stdout + r.stderr)) - names
        if not found:
            sys.exit(r.stdout + r.stderr + "\n道具を組めない")
        names |= found
    else:
        sys.exit("道具を組めない")

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
// a crash test's own script (an async function using what node lacks) records nothing
process.on('unhandledRejection', () => {});
globalThis.test_valid_value = function (p, v, s) {
  calls.push({kind: 'valid', args: [p, v, arguments.length < 3 ? v : s]});
};
globalThis.test_invalid_value = (p, v) => calls.push({kind: 'invalid', args: [p, v]});
globalThis.test_computed_value = function (p, v, c) { calls.push({kind: 'computed', args: [p, v, c || v]}); };
const neutral = {};
globalThis.neutralKeyframe = neutral;
// the neutral keyframe as \u0001 (the element's underlying value)
const keyframe = v => v === neutral ? '\u0001' : typeof v !== 'string' ? null : v;
globalThis.test_interpolation = function (o, expectations) {
  // CSS transitions do not start for a pair that does not interpolate: that is the time model's
  if (o.method && o.method !== 'CSS Animations') { calls.push({kind: 'method', args: []}); return; }
  for (const e of expectations)
    calls.push({kind: 'interpolation', args: [o.property, keyframe(o.from), keyframe(o.to), e.at,
                                              e.expect === neutral ? '\u0001' : e.expect]});
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
globalThis.test_shorthand_value = (p, v, longhands) => calls.push({kind: 'shorthand', args: [p, v, longhands]});
globalThis.promise_test = globalThis.async_test = globalThis.test;
globalThis.setup = globalThis.add_completion_callback = () => {};
// what the page reads from the document (a computed value it builds an expectation from) is a marker
const element = new Proxy(function () {}, {get: (t, k) => k === Symbol.toPrimitive ? () => '\u0000page' : element,
                                           apply: () => element, set: () => true});
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
    """the font-size the page gives its target (computed values of em), and its color after a ';'
    (computed values of currentcolor) when it gives one"""
    m = re.search(r"font-size:\s*(\d+(?:\.\d+)?)px", page)
    font = m.group(1) if m else "16"
    # the text pages give the interpolation target its colour by class (.target), the others by id
    target = r"(?:#|\.)target" if group in ("text", "decor") else "#target"
    c = re.search(target + r"\s*\{[^}]*?(?<![-\w])color:\s*([^;}]+)", page)
    return font + ";" + c.group(1).strip() if c else font

def page_rules(page, selector):
    """the declarations of the page's style sheet rules for selector (.parent, .target), as written"""
    css = re.sub(r"/\*.*?\*/", "", " ".join(re.findall(r"<style[^>]*>(.*?)</style>", page, re.S)), flags=re.S)
    found = re.findall(r"(?:^|[}\s,])" + re.escape(selector) + r"\s*\{([^}]*)\}", css)
    return " ".join(f.strip().rstrip(";") + ";" for f in found if f.strip()).replace("\t", " ").replace("\n", " ")

tool = Tool()
only = sys.argv[sys.argv.index("--file") + 1] if "--file" in sys.argv else None
verbose = "--verbose" in sys.argv
cases = []  # (file, kind, description, request, want)
skipped = {}
def skip(why):
    skipped[why] = skipped.get(why, 0) + 1
wide = ("initial", "inherit", "unset", "revert", "revert-layer")
NEUTRAL = "\u0001"
for path in pages:
    if only and only not in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    scripts = re.findall(r"<script(?![^>]*src=)[^>]*>(.*?)</script>", page, re.S)
    calls = json.loads(subprocess.run([node, str(out / "collect.js")], input=json.dumps(scripts), text=True,
                                      capture_output=True, encoding="utf-8", check=True).stdout)
    font = page_font(page)
    # the gaps group: the page's .parent and .target rules on the element's parent and itself
    rules = (page_rules(page, ".parent"), page_rules(page, ".target")) if block else ("", "")
    on_page = (lambda r: ("T",) + rules + r) if any(rules) else (lambda r: r)
    for call in calls:
        kind, args = call["kind"], call["args"]
        if kind == "shorthand":
            if not block:
                skip("test_shorthand_value")
                continue
            prop, value, longhands = args
            names = sorted(longhands)
            cases.append((path.name, kind, value, ("H", prop, value, ",".join(names)), [longhands[n] for n in names]))
            continue
        if kind == "method":
            skip("CSS animation 以外の流儀だけの補間")
            continue
        if kind == "error":
            skip("頁の script の誤り")
            continue
        if kind not in ("valid", "invalid", "computed", "interpolation", "composition"):
            continue
        if any(isinstance(a, str) and "\u0000page" in a for a in args):
            skip("頁が文書から読む値")
            continue
        prop = args[0]
        if kind in ("valid", "invalid"):
            if "var(" in args[1] or args[1].strip().lower() in wide:
                skip("var()・CSS 全体の語")
                continue
            cases.append((path.name, kind, args[1], ("S", prop, args[1]), None if kind == "invalid" else args[2]))
        elif kind == "computed":
            if args[1].strip().lower() in ("inherit", "unset") and group == "ui":
                skip("親の値を取る CSS 全体の語（親の装いは頁のもの）")
                continue
            if group == "ui" and prop == "cursor" and "gradient(" in args[1]:
                # cursor-invalid.html が無効とする <image> で、期待の文字も括弧が閉じていない
                skip("頁の期待が不正（cursor-invalid.html と食い違う gradient）")
                continue
            cases.append((path.name, kind, args[1], ("C", prop, font, args[1]), args[2]))
        elif kind == "interpolation":
            _, a, b, at, want = args
            if not block:
                a, b, want = (None if x == NEUTRAL else x for x in (a, b, want))
            ends_wide = not block and (a is not None and a.strip() in wide or b is not None and b.strip() in wide)
            if a is None or b is None or ends_wide or not isinstance(want, str):
                skip("補間の端が neutral・CSS 全体の語")
                continue
            shown = lambda x: "neutral" if x == NEUTRAL else x
            cases.append((path.name, kind, f"{shown(a)} → {shown(b)} @{at}",
                          on_page(("I", prop, font, a, b, repr(float(at)))), want))
        else:
            _, under, from_op, a, to_op, b, at, want = args
            a, b, want = (None if x == NEUTRAL else x for x in (a, b, want))
            if a is None or b is None or not isinstance(want, str) or not isinstance(under, str):
                skip("合成の端が neutral")
                continue
            cases.append((path.name, kind, f"[{under}] {from_op} {a} → {to_op} {b} @{at}",
                          ("A", prop, font, under, from_op, a, to_op, b, repr(float(at))), want))
# The accumulation pages of css-gaps write their element.animate() calls in test() bodies, which the
# collector does not read: their inputs and expectations, as written there (underlying value, the two
# keyframes, composite, currentTime / duration).
if group == "gaps" and (not only or "accumulation" in only):
    animate = {
        "rule-width-accumulation-auto-repeaters.html": [
            ("column-rule-width", "repeat(auto, 10px, 20px)", "accumulate", "repeat(auto, 30px)", "repeat(auto, 50px)", "repeat(auto, 50px, 60px)"),
            ("column-rule-width", "repeat(auto, 10px)", "accumulate", "repeat(auto, 20px, 30px)", "repeat(auto, 40px, 50px)", "repeat(auto, 40px, 50px)"),
            ("column-rule-width", "repeat(auto, 10px, 20px)", "accumulate", "repeat(auto, 1px, 2px, 3px)", "repeat(auto, 1px, 2px, 3px)",
             "repeat(auto, 11px, 22px, 13px, 21px, 12px, 23px)"),
            ("column-rule-width", "repeat(auto, 10px, 20px)", "add", "repeat(auto, 30px)", "repeat(auto, 50px)", "repeat(auto, 50px, 60px)"),
            ("row-rule-width", "repeat(auto, 10px, 20px)", "accumulate", "repeat(auto, 30px)", "repeat(auto, 50px)", "repeat(auto, 50px, 60px)"),
        ],
        "rule-color-accumulation-auto-repeaters.html": [
            ("column-rule-color", "repeat(auto, rgb(100, 0, 0), rgb(0, 100, 0))", "accumulate", "repeat(auto, rgb(50, 50, 50))",
             "repeat(auto, rgb(50, 50, 50))", "repeat(auto, rgb(150, 50, 50), rgb(50, 150, 50))"),
            ("column-rule-color", "repeat(auto, rgb(50, 50, 50))", "accumulate", "repeat(auto, rgb(100, 0, 0), rgb(0, 100, 0))",
             "repeat(auto, rgb(100, 0, 0), rgb(0, 100, 0))", "repeat(auto, rgb(150, 50, 50), rgb(50, 150, 50))"),
            ("column-rule-color", "repeat(auto, rgb(100, 0, 0), rgb(0, 100, 0))", "add", "repeat(auto, rgb(50, 50, 50))",
             "repeat(auto, rgb(50, 50, 50))", "repeat(auto, rgb(150, 50, 50), rgb(50, 150, 50))"),
            ("row-rule-color", "repeat(auto, rgb(100, 0, 0), rgb(0, 100, 0))", "accumulate", "repeat(auto, rgb(50, 50, 50))",
             "repeat(auto, rgb(50, 50, 50))", "repeat(auto, rgb(150, 50, 50), rgb(50, 150, 50))"),
        ],
    }
    for name, rows in animate.items():
        page = (wpt / "css-gaps/animation" / name).read_text(encoding="utf-8")
        for prop, under, op, a, b, want in rows:
            if want not in page:
                sys.exit(f"{name} の期待が頁と違う：{want}")
            cases.append((name, "composition", f"[{under}] {op} {a} → {b} @0.5",
                          ("A", prop, "16", under, op, a, op, b, "0.5"), want))
# The gaps pages whose test() bodies read the document (the collector does not run them): their
# inputs and expectations as the page writes them. Each test is a list of requests whose answers
# must be exactly what its assert_equals calls want; a request's \x02 is the answer of the one
# before it (a value the page reads and writes back).
scripted = []  # (page, test, [(request, want)])
if block:
    def neutral(page, decls, prop, to, want):
        """element.animate([{}, {prop: to}]) on an element with inline decls, at half its duration"""
        return (page, f"{prop}: neutral → {to}", [(("T", "", decls, "I", prop, "16", NEUTRAL, to, "0.5"), want)])
    def conversion(page, decls, prop, start, later, wants):
        """@keyframes from {prop: start} (to: the underlying value) at 0 and at half, then at half once
        the inline value is later"""
        return (page, f"{prop}: {start} → neutral", [
            (("T", "", decls, "I", prop, "16", start, NEUTRAL, "0"), wants[0]),
            (("T", "", decls, "I", prop, "16", start, NEUTRAL, "0.5"), wants[1]),
            (("T", "", f"{decls} {prop}: {later};", "I", prop, "16", start, NEUTRAL, "0.5"), wants[2])])
    rows = [
        neutral("gap-decorations-color-neutral-keyframe-001.html", "row-rule-style: solid; row-rule-width: 10px; row-rule-color: black;",
                "row-rule-color", "rgb(255, 0 , 0)", "rgb(128, 0, 0)"),
        neutral("gap-decorations-color-neutral-keyframe-002.html", "column-rule-style: solid; column-rule-color: black, black, black;",
                "column-rule-color", "red, blue, rgb(255, 0, 0)", "rgb(128, 0, 0), rgb(0, 0, 128), rgb(128, 0, 0)"),
        neutral("gap-decorations-color-neutral-keyframe-003.html", "row-rule-style: solid; row-rule-color: black, black, black;",
                "row-rule-color", "red, blue, rgb(255, 0, 0)", "rgb(128, 0, 0), rgb(0, 0, 128), rgb(128, 0, 0)"),
        neutral("gap-decorations-color-neutral-keyframe-004.html",
                "row-rule-style: solid; row-rule-color: repeat(3, black, black), repeat(1, red);", "row-rule-color",
                "repeat(3, red, red), repeat(1, black)", ", ".join(["rgb(128, 0, 0)"] * 7)),
        neutral("gap-decorations-color-neutral-keyframe-005.html", "row-rule-style: solid; row-rule-color: repeat(3, black, black), red, red;",
                "row-rule-color", "repeat(3, red, red), black, black", ", ".join(["rgb(128, 0, 0)"] * 8)),
        neutral("gap-decorations-width-neutral-keyframe-001.html", "row-rule-style: solid; row-rule-width: 10px;", "row-rule-width", "20px",
                "15px"),
        neutral("gap-decorations-width-neutral-keyframe-002.html", "row-rule-style: solid; row-rule-width: 10px, 20px;", "row-rule-width",
                "20px, 30px", "15px, 25px"),
        neutral("gap-decorations-width-neutral-keyframe-003.html",
                "row-rule-style: solid; row-rule-width: repeat(2, 20px, 20px), repeat(1, 30px);", "row-rule-width",
                "repeat(2, 40px, 40px), repeat(1, 40px)", "30px, 30px, 30px, 30px, 35px"),
        neutral("gap-decorations-width-neutral-keyframe-004.html",
                "column-rule-style: solid; column-rule-width: repeat(2, 20px, 20px), 30px, 30px;", "column-rule-width",
                "repeat(2, 40px, 40px), 40px, 40px", "30px, 30px, 30px, 30px, 35px, 35px"),
        neutral("gap-decorations-width-neutral-keyframe-005.html",
                "column-rule-style: solid; column-rule-width: 10px, 15px, 20px, 25px;", "column-rule-width",
                "20px, 25px, 30px, 35px", "15px, 20px, 25px, 30px"),
        conversion("rule-color-interpolation-conversion-001.html", "column-rule-color: blue;", "column-rule-color", "red",
                   "repeat(auto, red)", ["rgb(255, 0, 0)", "rgb(128, 0, 128)", "repeat(auto, rgb(255, 0, 0))"]),
        conversion("rule-color-interpolation-conversion-002.html", "column-rule-color: blue, blue;", "column-rule-color",
                   "red, repeat(auto, red)", "blue, repeat(auto, blue)",
                   ["rgb(255, 0, 0), repeat(auto, rgb(255, 0, 0))", "rgb(0, 0, 255), rgb(0, 0, 255)",
                    "rgb(128, 0, 128), repeat(auto, rgb(128, 0, 128))"]),
        conversion("rule-width-interpolation-conversion-001.html", "column-rule-width: 10px; column-rule-style: solid;",
                   "column-rule-width", "0px", "repeat(auto, 10px)", ["0px", "5px", "repeat(auto, 10px)"]),
        conversion("rule-width-interpolation-conversion-002.html", "column-rule-width: 10px, 10px; column-rule-style: solid;",
                   "column-rule-width", "0px, repeat(auto, 0px)", "10px, repeat(auto, 10px)",
                   ["0px, repeat(auto, 0px)", "10px, 10px", "5px, repeat(auto, 5px)"]),
    ]
    for side in ("row", "column"):
        rows.append(("gap-decorations-inset-neutral-keyframe-00%d.html" % (1 if side == "row" else 2), f"{side}-rule-inset-*: neutral → 11px",
                     [(("T", "", f"{side}-rule-style: solid; " + " ".join(f"{side}-rule-inset-{x}: 1px;" for x in (
                         "cap-start", "cap-end", "junction-start", "junction-end")),
                        "I", f"{side}-rule-inset-{x}", "16", NEUTRAL, "11px", "0.5"), "6px")
                      for x in ("cap-start", "cap-end", "junction-start", "junction-end")]))
    # parsing: a style attribute's !important shorthand sets its longhands
    rows.append(("gap-decorations-important.html", "column-rule: 5px solid red !important",
                 [(("G", "display: flex; column-rule: 5px solid red !important", f"column-rule-{x}"), want)
                  for x, want in (("style", "solid"), ("width", "5px"), ("color", "red"))]))
    # the rule shorthands from longhands set through element.style: computed and the declarations'
    page = (wpt / "css-gaps/parsing/gap-decorations-rule-shorthand-from-longhands.tentative.html").read_text(encoding="utf-8")
    from_longhands = [
        ("5px", "solid", "rgb(0, 128, 0)", "5px solid rgb(0, 128, 0)"),
        ("repeat(auto, 5px)", "repeat(auto, solid)", "repeat(auto, rgb(255, 0, 0))", "repeat(auto, 5px solid rgb(255, 0, 0))"),
        ("repeat(auto, thin, medium)", "solid", "repeat(8, red, blue)", ""),
        ("repeat(6, 15px, thick)", "repeat(auto, solid)", "repeat(auto, red)", ""),
        ("15px, 25px, 35px", "solid, dotted", "green", ""),
        ("repeat(auto, 5px)", "solid, double", "repeat(7, red)", ""),
        ("repeat(auto, 5px, 8px, 10px)", "repeat(auto, solid, double)", "repeat(auto, red, green, blue)", ""),
        ("repeat(2, 1px, 3px, 5px)", "repeat(2, solid, double)", "repeat(2, red)", ""),
    ]
    for side in ("column", "row"):
        for w, s, c, want in from_longhands:
            if any(f"'{x}'" not in page for x in (w, s, c, want)):
                sys.exit(f"from-longhands の頁と違う：{w} {s} {c} {want}")
            decls = f"{side}-rule-width: {w}; {side}-rule-style: {s}; {side}-rule-color: {c};"
            rows.append(("gap-decorations-rule-shorthand-from-longhands.tentative.html", f"{side}-rule from {w}; {s}; {c}",
                         [(("T", "", decls, "B", f"{side}-rule", "16"), want), (("G", decls, f"{side}-rule"), want)]))
    # bidirectional shorthands computed from the style sheet's longhands of two elements
    page = (wpt / "css-gaps/parsing/gap-decorations-bidirectional-shorthands.html").read_text(encoding="utf-8")
    for target, wants in (("#target1", (("rule-color", "rgb(0, 255, 0)"), ("rule-width", "10px"), ("rule-style", "solid"),
                                        ("rule", "10px solid rgb(0, 255, 0)"), ("rule-break", "intersection"),
                                        ("rule-visibility-items", "around"), ("rule-inset", "10px 5px / 10px 5px"))),
                          ("#target2", tuple((p, "") for p in ("rule-color", "rule-width", "rule-style", "rule", "rule-break",
                                                                 "rule-visibility-items", "rule-inset")))):
        decls = page_rules(page, target)
        for prop, want in wants:
            if f"getPropertyValue('{prop}'), '{want}')" not in page:
                sys.exit(f"bidirectional-shorthands の頁と違う：{target} {prop} {want}")
            rows.append(("gap-decorations-bidirectional-shorthands.html", f"{target} {prop}", [(("T", "", decls, "B", prop, "16"), want)]))
    # column-rule-width as specified whatever column-rule-style is (the first test compares the
    # element's value with itself: it needs the value)
    page = (wpt / "css-gaps/parsing/gap-decorations-col-rule-width.html").read_text(encoding="utf-8")
    for target, want in (("#target1", None), ("#target2", "5px, 10px, 15px"), ("#target3", "repeat(auto, 5px)")):
        rows.append(("gap-decorations-col-rule-width.html", f"{target} column-rule-width",
                     [(("T", "", page_rules(page, target), "B", "column-rule-width", "16"), want)]))
    # the rule shorthands' computed initial value set on the element and read back
    for prop in ("column-rule", "row-rule", "rule"):
        rows.append(("gap-decorations-rule-shorthand-roundtrip.tentative.html", f"{prop} round trip",
                     [(("B", prop, "16"), None), (("G", f"{prop}: \x02", prop), "\x02")]))
    for page_name, test, checks in rows:
        if not only or only in page_name:
            text = next((p for p in (wpt / "css-gaps/animation" / page_name, wpt / "css-gaps/parsing" / page_name) if p.exists()))
            for request, want in checks:
                for x in (want,) + ((request[6], request[7]) if request[0] == "T" and request[3] == "I" else ()):
                    if isinstance(x, str) and x and x not in (NEUTRAL, "\x02") and x not in text.read_text(encoding="utf-8"):
                        sys.exit(f"{page_name} の値が頁と違う：{x}")
            scripted.append((page_name, test, checks))
# an animated value's expectation is the computed value of what it names (on an element of the
# page's rules when the request is, the underlying value for a neutral one)
expect_index = {}
expected_requests = []
for i, c in enumerate(cases):
    if c[1] in ("interpolation", "composition"):
        wrap, inner = (c[3][:3], c[3][3:]) if c[3][0] == "T" else ((), c[3])
        expect_index[i] = len(expected_requests)
        expected_requests.append(wrap + (("B", inner[1], inner[2]) if c[4] == NEUTRAL else ("C", inner[1], inner[2], c[4])))
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
# the scripted tests' requests, those that write back a value read before them once it is read
flat = [(t, k) for t, (_, _, checks) in enumerate(scripted) for k in range(len(checks))]
first = [(t, k) for t, k in flat if not any(isinstance(x, str) and "\x02" in x for x in scripted[t][2][k][0])]
script_answers = dict(zip(first, tool.run([scripted[t][2][k][0] for t, k in first]) if first else []))
later = [(t, k) for t, k in flat if (t, k) not in script_answers]
written = [tuple(x.replace("\x02", script_answers.get((t, k - 1), "")) if isinstance(x, str) else x
                 for x in scripted[t][2][k][0]) for t, k in later]
script_answers.update(zip(later, tool.run(written) if written else []))
report = {}
failures = {}
round_trips = []
def count(name, ok, kind, value, want, got):
    good, total = report.get(name, (0, 0))
    report[name] = (good + ok, total + 1)
    if not ok:
        failures.setdefault(name, []).append((kind, value, want, got))
for i, (name, kind, value, request, want) in enumerate(cases):
    got = answers[i]
    if kind == "shorthand":
        # test_shorthand_value: each longhand's value and its round trip, then no unrelated longhand
        fields = got.split("\x1f")
        names = request[3].split(",")
        if len(fields) != 2 * len(names) + 2:
            fields = [""] * (2 * len(names) + 2)
        for k, longhand in enumerate(names):
            read, back = fields[2 * k], fields[2 * k + 1]
            count(name, read == want[k] and back == read, kind, f"{request[1]}: {value} → {longhand}", want[k],
                  read if back == read else f"{read} (往復 {back})")
        count(name, fields[-2] == "1" and fields[-1] == "0", kind, f"{request[1]}: {value}（ほかの longhand）",
              "CSS.supports、残り 0", f"CSS.supports {fields[-2]}、残り {fields[-1]}")
        continue
    if kind == "invalid":
        ok = got == "<invalid>"
    elif kind in ("interpolation", "composition"):
        want = expected_answers[expect_index[i]]
        ok = got != "<invalid>" and want != "<invalid>" and normalize(got) == normalize(want)
    else:
        ok = got != "<invalid>" and matches(got, want)
        if ok and i in again_index and round_trip[again_index[i]] != got:
            round_trips.append((name, value, got, round_trip[again_index[i]]))
    count(name, ok, kind, value, want, got)
for t, (name, test, checks) in enumerate(scripted):
    got = [script_answers[(t, k)] for k in range(len(checks))]
    wants = [script_answers.get((t, k - 1), "") if want == "\x02" else want for k, (_, want) in enumerate(checks)]
    ok = all((g != "<invalid>") if w is None else g == w for g, w in zip(got, wants))
    count(name, ok, "test", test, wants, got)
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
