"""CSS の選択子の持ち主（css.c）を、DOM の節（dom.c）と一緒に host で確かめる。

WPT の css/selectors から、選択子の解析の頁（parsing/*.html の test_valid_selector・
test_invalid_selector・test_valid_forgiving_selector）、属性の大文字小文字の頁
（attribute-selectors/attribute-case の semantics・syntax・cssom）、:heading の頁、
An+B の上限の頁と、決まった値を照らす小さな頁を node で読み、試験の呼出しと期待を
取り出して、css.c を取り込んだ道具（tests/css_selectors_native_cases.c）に渡す。
css/css-shadow/part（::part()）の頁は、頁の木（custom element の template を shadow
tree に）を道具へ渡し、cascade 自身の集め方（cascade_collect・style 属性・cmp_dent）で
勝つ色を照らす。読む前に DOM を変える subtest と form の部品のある頁は数えて落とす。

照らすのは native の層だけ：insertRule は css_parse_sheet と css_sheet_single_rule、
selectorText・cssText は規則の直列化、querySelector・matches は css_select_n・
css_matches_n、getComputedStyle は「規則の選択子のどれかが要素に当たるか」で置き換える
（cascade の索引と Bloom の篩は通さない。::part() の頁だけは索引を通る）。JS の束縛（例外の型・CSSOM の物）、iframe の
読込み、:focus-visible の打鍵は guest の WPT の仕事。

  python -X utf8 tests/check_css_selectors.py [--old] [--verbose] [--file 名前の一部]

--old は c18b3fac の源（build/check-css-selectors/old に git archive で取り出す）で同じ
問いに答え、変更前の数を出す。
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
wpt = Path("D:/Programes/cloned repos/wpt/css/selectors")
old = "--old" in sys.argv
out = root / "build/check-css-selectors" / ("old" if old else "new")
out.mkdir(parents=True, exist_ok=True)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))

# ---- the tool
source = root
if old:
    source = out / "src"
    if not (source / "user/libc/web/css.c").exists():
        archive = out / "old.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive), "c18b3fac", "user/libc/web",
                        "user/include", "common"], cwd=root, check=True)
        with tarfile.open(archive) as t:
            t.extractall(source, filter="data")
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
web = source / "user/libc/web"
generated = out / "css-selectors-generated.c"
generated.write_text('#include "css.c"\n' + ("#define OLD_SOURCE 1\n" if old else "") +
                     (root / "tests/css_selectors_native_cases.c").read_text(encoding="utf-8"), encoding="utf-8")
stubs = out / "unreached.c"
exe = out / "css-selectors.exe"
units = [str(generated), str(web / "dom.c"), str(web / "html.c"), str(web / "util.c"), str(web / "cssprop.c"),
         str(web / "color.c")]
flags =["-std=gnu11", "-O1", "-w", "-I" + str(abi), "-I" + str(source / "user/include"), "-I" + str(source / "common"),
         "-I" + str(source / "user/libc"), "-I" + str(web), "-I" + str(root / "third_party/quickjs")]
undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")
names = set()
for attempt in range(6):
    # every external the linked units reach but this check never calls: a call aborts with its name
    stubs.write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
        f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(names)), encoding="utf-8")
    r = subprocess.run([str(cc), *flags, *units, str(stubs), "-o", str(exe)], cwd=root, env=env,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode == 0:
        break
    found = set(undefined.findall(r.stdout + r.stderr)) - names
    if not found:
        sys.exit(r.stdout + r.stderr)
    names |= found
else:
    sys.exit("道具を組めない")
print(f"道具：{len(names)} の外部の関数は呼ばれれば止まる")

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

# A page's inline scripts run in a node context with the harness functions recording
# their calls; the page's own top-level variables come back by name.
collector = r"""
const fs = require('fs'), vm = require('vm');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const calls = [];
const context = {
  test_valid_selector(selector, serialized, options) {
    calls.push({kind: 'valid', selector, serialized: arguments.length < 2 ? selector : serialized,
                forgiving: !!(options && options.onlyWhenForgiving)});
  },
  test_invalid_selector(selector) { calls.push({kind: 'invalid', selector}); },
  test() {}, setup() {}, done() {}, promise_test() {}, async_test() {},
  document: new Proxy({}, {get: () => () => null}),
};
context.test_valid_forgiving_selector = (selector, serialized = null) =>
  context.test_valid_selector(selector, serialized ? serialized : selector, {onlyWhenForgiving: true});
context.window = context;
vm.createContext(context);
for (const script of input.scripts) {
  try { vm.runInContext(script, context); } catch (e) { calls.push({kind: 'error', message: String(e)}); }
}
const names = {};
for (const name of input.names) names[name] = context[name];
process.stdout.write(JSON.stringify({calls, names}));
"""
(out / "collect.js").write_text(collector, encoding="utf-8")

def page_data(path, names=()):
    page = path.read_text(encoding="utf-8")
    scripts = re.findall(r"<script(?![^>]*src=)[^>]*>(.*?)</script>", page, re.S)
    r = subprocess.run([node, str(out / "collect.js")], input=json.dumps({"scripts": scripts, "names": list(names)}),
                       text=True, capture_output=True, encoding="utf-8", check=True)
    return page, json.loads(r.stdout)

def encode(field):
    b = field.encode("utf-8", "surrogatepass") if isinstance(field, str) else field
    return (b.replace(b"\\", b"\\\\").replace(b"\t", b"\\t").replace(b"\n", b"\\n").replace(b"\r", b"\\r")
            .replace(b"\0", b"\\0"))

def decode(field):
    result, i = bytearray(), 0
    while i < len(field):
        c = field[i]
        if c == 0x5C and i + 1 < len(field):
            n = field[i + 1]
            result += {0x74: b"\t", 0x6E: b"\n", 0x72: b"\r", 0x30: b"\0"}.get(n, bytes([n]))
            i += 2
            continue
        result.append(c)
        i += 1
    return result.decode("utf-8", "surrogatepass")

def ask(requests):
    """the requests of a pass go to the tool at once; each answer is its fields"""
    (out / "requests.txt").write_bytes(b"".join(b"\t".join(encode(f) for f in r) + b"\n" for r in requests))
    subprocess.run([str(exe), str(out / "requests.txt"), str(out / "answers.txt")], cwd=root, env=env, check=True,
                   timeout=600)
    lines = (out / "answers.txt").read_bytes().split(b"\n")[:len(requests)]
    if len(lines) < len(requests):
        sys.exit("道具の答えが足りない（途中で止まった）")
    return [[decode(f) for f in line.split(b"\t")] for line in lines]

def attrs(*records):
    return "\x1e".join(f"{ns}\x1f{name}\x1f{value}" for ns, name, value in records)

class Rules:
    """an 'R' answer: whether the text was one rule, how many, and each rule's type, selectorText, cssText"""
    def __init__(self, fields):
        self.single, self.count = fields[0] == "1", int(fields[1])
        self.rules = [(int(fields[i]), fields[i + 1], fields[i + 2]) for i in range(2, len(fields) - 2, 3)]
    def inserted(self):
        """insertRule of the text into an empty sheet succeeds with one style rule"""
        return self.single and self.count == 1 and self.rules[0][0] == 1

# ---- the subtests: (page, name, [requests], judge(answers) -> bool)
only = sys.argv[sys.argv.index("--file") + 1] if "--file" in sys.argv else None
verbose = "--verbose" in sys.argv
subtests = []
def subtest(page, name, requests, judge, note=None):
    if only and only not in page:
        return
    subtests.append((page, name, requests, judge, note))

# parsing/*.html and invalid-pseudos.html: test_valid_selector and test_invalid_selector
# (support/parsing-testcommon.js), one subtest each, with the round trip as a second pass

for path in sorted((wpt / "parsing").glob("*.html")):
    page, data = page_data(path)
    for call in data["calls"]:
        if call["kind"] == "error":
            print(f"{path.name}: 頁の script が node で止まった：{call['message']}")
            continue
        s = call["selector"]
        if call["kind"] == "invalid":
            subtest(path.name, f"{s!r} should be an invalid selector", [("Q", s), ("R", s + "{}")],
                    lambda a: a[0][0] == "0" and not Rules(a[1]).single)
            continue
        expected = call["serialized"]
        def judge(a, expected=expected, forgiving=call["forgiving"]):
            rules = Rules(a[1])
            if a[0][0] != "1" or not rules.inserted():
                return False
            read = rules.rules[0][1]
            if read not in (expected if isinstance(expected, list) else [expected]):
                return False
            if a[2][0] != ("0" if forgiving else "1"):
                return False
            # deleteRule, then insertRule of what was read: it reads back the same
            return [("R", read + "{}")], lambda b: Rules(b[0]).inserted() and Rules(b[0]).rules[0][1] == read
        subtest(path.name, f"{s!r} should be a valid selector",
                [("Q", s), ("R", s + "{}"), ("S", f"selector({s})")], judge)

# attribute-selectors/attribute-case: semantics.html, syntax.html, cssom.html
case = wpt / "attribute-selectors/attribute-case"
_, data = page_data(case / "semantics.html", ("match", "matchHTMLOnly", "nomatch"))
lists = data["names"]
for mode, label in (("html", "standards mode"), ("quirks", "quirks mode"), ("xml", "XML")):
    for kind, entries in (("match", lists["match"] + (lists["matchHTMLOnly"] if mode != "xml" else [])),
                          ("nomatch", lists["nomatch"])):
        for entry in entries:
            s, records = entry[0], entry[1:]
            ns_decl = s.startswith("@namespace")
            described = " ".join(("{" + ns + "}" if ns else "") + f'{name}="{value}"' for ns, name, value in records)
            want_rules = 2 if ns_decl else 1
            subtest("semantics.html", f"{s} <div {described}> in {label}",
                    [("D", mode, "div", attrs(*records), s + " { visibility:hidden }", "")],
                    lambda a, k=kind, w=want_rules: int(a[0][0]) == w and a[0][1] == ("1" if k == "match" else "0"))
            if not ns_decl:
                subtest("semantics.html", f"{s} <div {described}> with querySelector in {label}",
                        [("D", mode, "div", attrs(*records), "", s)],
                        lambda a, k=kind: a[0][2] == ("E" if k == "match" else "N"))
_, data = page_data(case / "syntax.html", ("valid", "invalid"))
element = attrs(("", "id", "test"), ("", "foo", "BAR"), ("", "baz", "quux"))
for mode, label in (("html", "standards mode"), ("quirks", "quirks mode"), ("xml", "XML")):
    for s in data["names"]["valid"]:
        subtest("syntax.html", f"{s} in {label}", [("D", mode, "div", element, s + " { visibility:hidden }", "")],
                lambda a: a[0][0] == "1" and a[0][1] == "1")
        subtest("syntax.html", f"{s} with querySelector in {label}", [("D", mode, "div", element, "", s)],
                lambda a: a[0][2] == "E")
    for s in data["names"]["invalid"]:
        subtest("syntax.html", f"{s} in {label}", [("D", mode, "div", element, s + " { visibility:hidden }", "")],
                lambda a: a[0][0] == "0" and a[0][1] == "0")
        subtest("syntax.html", f"{s} with querySelector in {label}", [("D", mode, "div", element, "", s)],
                lambda a: a[0][2] == "!")
_, data = page_data(case / "cssom.html", ("tests",))
for given, expected in data["names"]["tests"]:
    for media in ("", " in @media"):
        cases = [
            (" insertRule", [("R", given + " {}")], lambda a: Rules(a[0]).single and Rules(a[0]).count == 1),
            (" getting CSSRule#cssText", [("R", given + " {}")],
             lambda a, e=expected: Rules(a[0]).inserted() and Rules(a[0]).rules[0][2][:len(e)] == e),
            (" getting CSSStyleRule#selectorText", [("R", given + " {}")],
             lambda a, e=expected: Rules(a[0]).inserted() and Rules(a[0]).rules[0][1] == e),
            (" setting CSSStyleRule#selectorText", [("W", "foobar {}", given)], lambda a, e=expected: a[0][0] == e),
        ]
        for suffix, requests, judge in cases:
            if media:
                # CSSMediaRule.insertRule and cssRules: the engine has no CSSGroupingRule
                subtest("cssom.html", given + suffix + media, [], lambda a: False, "CSSGroupingRule が無い")
            else:
                subtest("cssom.html", given + suffix, requests, judge)

# heading.html: each element's matches(':heading') and matches(':heading(…)')
page = (wpt / "heading.html").read_text(encoding="utf-8")
body = page[page.index("<h1"):page.index("<script>", page.index("<h1"))]
levels = [(["1"], [1]), (["2"], [2]), (["3"], [3]), (["4"], [4]), (["5"], [5]), (["6"], [6]), (["7"], []),
          (["8"], []), (["9"], []), (["0"], []), (["0", "1", "2"], [1, 2]), (["6", "7"], [6])]
assert "{args: ['0', '1', '2'], match: [1, 2]}" in page  # the page's table, as above
for m in re.finditer(r"(<section>)?<(\w+)([^>]*)></\2>", body):
    section, name, rest = m.group(1), m.group(2), m.group(3)
    found = dict(re.findall(r'(\S+)="([^"]*)"', rest))
    if "data-expect-match" not in found:
        continue
    expect = found["data-expect-match"]
    chain = ("section/" if section else "") + name + "@" + ";".join(f"{k}={v}" for k, v in found.items())
    shown = f"<{name}{rest}></{name}>" + (" in section" if section else "")
    subtest("heading.html", shown + " :heading", [("D", "html", chain, "", "", ":heading")],
            lambda a, e=expect: a[0][3] == ("1" if e else "0"))
    for args, match in levels:
        selector = f":heading({', '.join(args)})"
        subtest("heading.html", f"{shown} {selector}", [("D", "html", chain, "", "", selector)],
                lambda a, e=expect, match=match: a[0][3] == ("1" if e and int(e) in match else "0"))
for level in range(1, 7):
    chain = f"h|h{level}"
    subtest("heading-prefixed.html", f":heading({level}) matches <h:h{level}> in the HTML namespace",
            [("D", "html", chain, "", "", f":heading({level})"), ("D", "html", chain, "", "", ":heading")],
            lambda a: a[0][3] == "1" and a[1][3] == "1")

# nth-child-large-anplusb-clamp.html
for given, want, name in ((":nth-child(99999999999999999999)", ":nth-child(2147483647)", "b term (NumberToken path) is clamped to INT_MAX"),
                          (":nth-child(99999999999999999999n)", ":nth-child(2147483647n)", "a term (DimensionToken path) is clamped to INT_MAX"),
                          (":nth-child(2n+99999999999999999999)", ":nth-child(2n+2147483647)", "b term following an a coefficient is clamped to INT_MAX")):
    subtest("nth-child-large-anplusb-clamp.html", name, [("R", given + " { color: green; }")],
            lambda a, w=want: Rules(a[0]).inserted() and Rules(a[0]).rules[0][1] == w)

# webkit-pseudo-element.html: the page's sheet and calls
page = (wpt / "webkit-pseudo-element.html").read_text(encoding="utf-8")
sheet = re.search(r'<style id="style">(.*?)</style>', page, re.S).group(1)
second = sheet[sheet.index("span::-webkit"):sheet.index("}", sheet.index("span::-webkit")) + 1]
wanted = "span::-webkit-something-invalid, #test, ::-webkit-something-nonexist123, ::-webkit-\\ escaped"
assert '"::-webkit-\\\\ escaped"' in page  # the page joins the expected text from parts like this
subtest("webkit-pseudo-element.html", "rules include webkit-prefixed pseudo-element should be cascaded",
        [("R", sheet), ("D", "html", "div@id=test", "", second, "")],
        lambda a: Rules(a[0]).count == 2 and a[1][1] == "1")
subtest("webkit-pseudo-element.html", "webkit-prefixed pseudo-element selectors should be accessible from CSSOM",
        [("R", sheet)], lambda a: len(Rules(a[0]).rules) > 1 and Rules(a[0]).rules[1][1] == wanted)
subtest("webkit-pseudo-element.html", "qS and qSA shouldn't throw exception",
        [("Q", "span::-webkit-something-invalid")], lambda a: a[0][0] == "1")
subtest("webkit-pseudo-element.html", "User-action pseudo-classes are valid",
        [("Q", "span::-webkit-something-invalid:active"), ("Q", "span::-webkit-something-invalid:hover")],
        lambda a: a[0][0] == "1" and a[1][0] == "1")
subtest("webkit-pseudo-element.html", "Nested pseudos are invalid",
        [("Q", "span::-webkit-something-invalid::before")], lambda a: a[0][0] == "0")
subtest("webkit-pseudo-element.html", "webkit-prefix without dash is invalid",
        [("R", sheet), ("Q", "span::-webkitfoo")], lambda a: Rules(a[0]).count == 2 and a[1][0] == "0")

# x-pseudo-element.html: the second rule is dropped, so p keeps green
page = (wpt / "x-pseudo-element.html").read_text(encoding="utf-8")
sheet = re.search(r"<style>(.*?)</style>", page, re.S).group(1)
subtest("x-pseudo-element.html", "x-prefixed pseudo-elements should make the whole rule invalid",
        [("R", sheet)], lambda a: Rules(a[0]).count == 1)

# is-where-error-recovery.html: selectorText set to forgiving lists keeps them as written
checks = []
for pseudo in ("is", "where"):
    nonsense = f":{pseudo}(:total-nonsense)"
    checks += [("W", "random-selector { color: blue; }", nonsense), ("D", "html", "div@id=test-div", "", "", nonsense)]
    for mixed in (f":{pseudo}(:total-nonsense, #test-div)", f":{pseudo}(:total-nonsense and-more-stuff, #test-div)",
                  f":{pseudo}(weird-token || and-more-stuff, #test-div)"):
        checks += [("W", "random-selector { color: blue; }", mixed),
                   ("D", "html", "div@id=test-div", "", mixed + " { color: blue; }", mixed)]
def recovery(a):
    for i in range(0, len(checks), 2):
        written = checks[i][2]
        if a[i][0] != written:
            return False
        document = a[i + 1]
        if checks[i + 1][4] == "":
            if document[2] != "N":
                return False
        elif document[2] != "E" or document[1] != "1":
            return False
    return True
subtest("is-where-error-recovery.html", ":is() and :where() error recovery", checks, recovery)

# missing-right-token.html: a selector missing its closing tokens parses
subtest("missing-right-token.html", "attribute selectors with missing right tokens succeed",
        [("D", "html", "meta@id=expected;charset=utf-8", "", "", 'meta[charset="utf-8"'),
         ("D", "html", "meta@id=expected;charset=utf-8", "", "", 'meta[charset="utf-8'),
         ("D", "html", "div@id=container/span", "", "", "span:not([class]"),
         ("D", "html", "div@id=container/span", "", "", "span:not([class")],
        lambda a: all(x[2] == "E" for x in a))

# css/css-shadow/part (CSS Shadow Parts): the pages that build their shadow trees with
# support/shadow-helper.js's installCustomElement and read a computed color through
# getElementByShadowIds. The page's tree, with each custom element's template as its
# shadow tree, goes to the tool ('C'), which answers with the color the cascade's own
# collection (cascade_collect, the style attribute, cmp_dent) lets win, inherited along
# the flat tree. A subtest that changes the DOM before it reads is the guest's: it is
# counted, and fails here as "not transcribed".
from html.parser import HTMLParser
parts = Path("D:/Programes/cloned repos/wpt/css/css-shadow/part")
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
class Element:
    def __init__(self, tag, attrs):
        self.tag, self.attrs, self.children, self.text = tag, [(k, v or "") for k, v in attrs], [], ""
class Builder(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Element("#document", [])
        self.stack = [self.root]
    def handle_starttag(self, tag, attrs):
        e = Element(tag, attrs)
        self.stack[-1].children.append(e)
        if tag not in VOID:
            self.stack.append(e)
    def handle_startendtag(self, tag, attrs):
        self.stack[-1].children.append(Element(tag, attrs))
    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                break
    def handle_data(self, data):
        self.stack[-1].text += data
def walk(e):
    yield e
    for c in e.children:
        yield from walk(c)
NAMED = {"green": "rgb(0, 128, 0)", "red": "rgb(255, 0, 0)", "blue": "rgb(0, 0, 255)", "black": "rgb(0, 0, 0)"}
def computed(value):
    """the declared value as getComputedStyle serializes it; '-': the initial CanvasText (light)"""
    v = value.strip().lower()
    if v == "-":
        return "rgb(0, 0, 0)"
    if v in NAMED:
        return NAMED[v]
    m = re.fullmatch(r"rgb\(\s*(\d+)\s*,?\s*(\d+)\s*,?\s*(\d+)\s*\)", v)
    return f"rgb({m.group(1)}, {m.group(2)}, {m.group(3)})" if m else v
lookup = re.compile(r"(?:const|var|let)\s+(\w+)\s*=\s*getElementByShadowIds\(\s*document\s*,\s*\[([^\]]*)\]\s*\)")
check = re.compile(r"assert_(not_)?equals\(\s*window\.getComputedStyle\(\s*(\w+)\s*(?:,\s*['\"]::?([\w-]+)['\"])?\s*\)"
                   r"\.color\s*,\s*(\w+)\s*\)")
for path in sorted(parts.glob("*.html")):
    if "-ref" in path.name or ".ref" in path.name:
        continue
    page = path.read_text(encoding="utf-8")
    if "getElementByShadowIds" not in page or "installCustomElement" not in page:
        continue
    builder = Builder()
    builder.feed(page)
    document = builder.root
    scripts = [e.text for e in walk(document) if e.tag == "script" and not dict(e.attrs).get("src")]
    registry = dict(re.findall(r"installCustomElement\(\s*[\"']([\w-]+)[\"']\s*,\s*[\"']([\w-]+)[\"']\s*\)", "\n".join(scripts)))
    templates = {dict(e.attrs).get("id"): e for e in walk(document) if e.tag == "template"}
    colors = dict(re.findall(r"const\s+(\w+)\s*=\s*\"(rgb\([^\"]*\))\"", "\n".join(scripts)))
    items, sheets, ids, shadow_of = [], [], [{}], {}
    def emit(children, depth, tree):
        for e in children:
            index = len(items)
            items.append([str(depth), e.tag] + [x for kv in e.attrs for x in kv])
            if dict(e.attrs).get("id"):
                ids[tree].setdefault(dict(e.attrs)["id"], index)
            if e.tag == "style":
                sheets.append((tree, e.text))
            if e.tag in registry and registry[e.tag] in templates:
                items.append([str(depth + 1), "#shadow"])
                ids.append({})
                shadow_of[index] = len(ids) - 1
                emit(templates[registry[e.tag]].children, depth + 2, len(ids) - 1)
            if e.tag != "template":
                emit(e.children, depth + 1, tree)
    html = next((e for e in document.children if e.tag == "html"), document)
    for e in html.children:
        if e.tag == "head":
            sheets += [(0, s.text) for s in walk(e) if s.tag == "style"]
    body = next((e for e in walk(html) if e.tag == "body"), None)
    emit(body.children if body else [], 1, 0)
    tree = "\x1d".join("\x1f".join(item) for item in items)
    sheet_text = "\x1e".join(f"{level}\x1f{text}" for level, text in sheets)
    # the scripts' statements other than the helpers' calls, the color constants and the tests
    rest = "\n".join(scripts)
    for m in re.finditer(r"test\(\s*(?:function\s*\(\s*\)|\(\s*\)\s*=>)\s*\{(.*?)\}\s*,\s*([\"'])(.*?)\2\s*\)\s*;?", rest, re.S):
        body_text, name = m.group(1), m.group(3)
        statements = [s.strip() for s in body_text.split(";") if s.strip()]
        requests, expectations, ok = [], [], bool(statements)
        elements = {}
        for s in statements:
            if (found := lookup.fullmatch(s)):
                path_ids = re.findall(r"[\"']([^\"']+)[\"']", found.group(2))
                tree_at, target = 0, None
                for i, name_id in enumerate(path_ids):
                    target = ids[tree_at].get(name_id) if tree_at is not None else None
                    if target is None:
                        break
                    tree_at = shadow_of.get(target) if i + 1 < len(path_ids) else tree_at
                if target is None:
                    ok = False
                elements[found.group(1)] = target
            elif (found := check.fullmatch(s)) and found.group(2) in elements and found.group(4) in colors:
                requests.append(("C", tree, str(elements[found.group(2)]), found.group(3) or "", sheet_text))
                expectations.append((bool(found.group(1)), colors[found.group(4)]))
            elif s in ('"use strict"', "'use strict'"):
                continue
            else:
                ok = False
        if not ok or not requests:
            subtest("css-shadow/part/" + path.name, name, [], lambda a: False, "読む前に DOM を変える：guest の WPT")
            continue
        if any(item[1] in ("input", "select", "textarea") for item in items):
            # the tool builds no form control (setting their attributes asks the form layer)
            subtest("css-shadow/part/" + path.name, name, [], lambda a: False, "form の部品：guest の WPT")
            continue
        def judge(a, expectations=expectations):
            return all((computed(x[0]) != want) if negated else (computed(x[0]) == want)
                       for x, (negated, want) in zip(a, expectations))
        subtest("css-shadow/part/" + path.name, name, requests, judge)

# ---- run
requests = [r for t in subtests for r in t[2]]
answers = ask(requests) if requests else []
verdicts, at = [], 0
for page, name, reqs, judge, note in subtests:
    got = answers[at:at + len(reqs)]
    at += len(reqs)
    verdicts.append((judge(got), got))
# a judge that answers with more requests and a second judge: the round trip
second = [(i, v[0]) for i, v in enumerate(verdicts) if isinstance(v[0], tuple)]
more = ask([r for _, (reqs, _) in second for r in reqs]) if second else []
at = 0
for i, (reqs, judge) in second:
    got = more[at:at + len(reqs)]
    at += len(reqs)
    verdicts[i] = (judge(got), verdicts[i][1] + got)
report, failures = {}, {}
for (page, name, reqs, judge, note), (verdict, got) in zip(subtests, verdicts):
    ok = bool(verdict)
    good, count = report.get(page, (0, 0))
    report[page] = (good + ok, count + 1)
    if not ok:
        failures.setdefault(page, []).append((name, note, got))
for page, (good, count) in report.items():
    print(f"{page}: {good}/{count}")
    if verbose:
        for name, note, got in failures.get(page, [])[:300]:
            print(f"    {name}" + (f"（{note}）" if note else ""))
            if got:
                print(f"      {got!r}")
passed = sum(g for g, c in report.values())
total = sum(c for g, c in report.values())
print(f"合計 {passed}/{total}" + ("（c18b3fac の源）" if old else ""))
(out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
