"""CSS の counter（CSS Lists 3 §4・CSS Generated Content 3・CSS Counter Styles 3）の数え方を、
実物のコードで host で確かめる。

数えの持ち主 counter.c をそのまま、box.c の数える箱の判定（counting_style・marker_generated）を
取り出し、性質の表（cssprop.c）で解析した装いの箱を手で組んだ道具（tests/counters_native_cases.c）
で走らせる。試すのは WPT の css/css-lists の reftest（li-value-reversed-*、li-list-item-counter-*、
counter-list-item、counter-reset-reversed-*、counters-scope-*、counters-00*、counter-set-001、
counter-reset-increment-overflow-underflow、implicit-and-explicit-list-item-counters、
marker-quotes、display: contents の二つ）が参照の頁で示す番号と文字。counters-001〜004 は WPT の頁と参照から読む。

guest の WPT の代わりではない。cascade（css.c の selector、UA の style sheet、ol・li の
presentational hint。hint は css.c が写す宣言で書く：li value=V → counter-set: list-item V、
ol reversed → counter-reset: reversed(list-item)、start=S → list-item S-1、reversed と start →
reversed(list-item) S+1）、箱の生成（display: none の要素は箱を作らない。contents は箱を作らず子の箱を
上の箱へ置く形を手で組む）、空白の処理、配置・描画は通らない。

  python -X utf8 tests/check_counters_native.py [--out 出力の場所]
"""
from pathlib import Path
import html
import os
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else root / "build/check-counters"
out.mkdir(parents=True, exist_ok=True)
wpt = Path(os.environ.get("NOCTURNE_WPT", "D:/Programes/cloned repos/wpt")) / "css/css-lists"
box = (root / "user/libc/web/box.c").read_text(encoding="utf-8")


def box_function(signature):
    start = box.index(signature)
    return box[start:box.index("\n}", start) + 2] + "\n"


# ---------------------------------------------------------------- the cases
class E:
    """an element: its declarations, children, the marker text it wants (None: not looked at),
    its own ::marker declarations, and its ::before and ::after as (declarations, wanted text)"""
    def __init__(self, decls="", *kids, marker=None, marker_decls=None, before=None, after=None):
        self.decls, self.kids, self.marker, self.marker_decls = decls, kids, marker, marker_decls
        self.before, self.after = before, after


UA_OL = "counter-reset: list-item; list-style-type: decimal"
UA_UL = "counter-reset: list-item; list-style-type: disc"
LI = "display: list-item"


def ol(*kids, reversed=False, start=None, style="", before=None):
    """an ol: the UA style sheet's rule, then its presentational hint (css.c), then the author's"""
    decls = UA_OL
    if reversed:
        decls += "; counter-reset: reversed(list-item)" + (" %d" % (start + 1) if start is not None else "")
    elif start is not None:
        decls += "; counter-reset: list-item %d" % (start - 1)
    return E(decls + ("; " + style if style else ""), *kids, before=before)


def li(marker=None, style="", *kids, value=None, **more):
    decls = LI + ("; counter-set: list-item %d" % value if value is not None else "") + ("; " + style if style else "")
    return E(decls, *kids, marker=marker, **more)


def items(*numbers, style=""):
    return [li("%d. " % n, style) for n in numbers]


cases = []


def case(name, *kids):
    cases.append((name, kids))


case("li-value-reversed-001", ol(li("7. "), li("6. ", value=6), li("5. "), reversed=True))
case("li-value-reversed-002", ol(li("9. "), li("8. "), li("7. ", value=7), li("6. "), li("10. ", value=10), li("9. "),
                                  reversed=True))
case("li-value-reversed-003", ol(li("9. "), li("8. "), li("7. ", "counter-set: list-item 7"),
                                  li("6. ", "counter-increment: list-item -1"),
                                  li("10. ", "counter-set: list-item 10; counter-increment: list-item 1"), li("9. "),
                                  reversed=True))
case("li-value-reversed-004", ol(li("3. "), E("counter-increment: list-item -1"), li("1. "), reversed=True))
case("li-value-reversed-005", ol(li("0. "), E("counter-increment: list-item 1; counter-set: list-item 1"), li("0. "),
                                  reversed=True))
# ol::before, li::before { content: counters(list-item,"."); } (the first two of its lists, and
# the other two signs)
counters_li = ("content: counters(list-item, '.')",)
for increments, start in (((-2, -2, -2), 8), ((-3, -1, -8), 20), ((2, 2, 2), -8), ((3, 1, 8), -20)):
    value, kids = start, []
    for k in increments:
        value += k
        kids.append(li("%d. " % value, "counter-increment: list-item %d" % k, before=(counters_li[0], str(value))))
    case("li-value-reversed-006a", ol(*kids, reversed=True, before=(counters_li[0], str(start))))
case("li-value-reversed-010", ol(li("4. "), li("2. ", "counter-increment: list-item -2"), li("1. "), reversed=True))
case("li-value-reversed-011", ol(*items(3, 2, 1), reversed=True, start=3))
case("li-value-reversed-012", ol(li("3. ", "counter-increment: list-item -2"), li("2. "), li("1. "), reversed=True))
case("li-value-reversed-013", ol(li("5. "), li("3. ", "counter-increment: list-item -2", value=3), li("2. "),
                                  reversed=True))
# a nested ol, the outer's child: its counter is nested (the outer ol's set has list-item), so the
# li after it counts the outer one (CSS Lists 3 §4.4.1, Issue 5477)
case("li-value-reversed-014", ol(li("5. "), li("2. ", "counter-increment: list-item -3"),
                                  ol(li("4. "), li("2. ", "counter-increment: list-item -2"), li("1. "), reversed=True),
                                  li("1. "), reversed=True))
case("li-value-reversed-015", ol(li("-1. "), li("2. ", "counter-increment: list-item 3"), li("1. "), reversed=True))
case("li-value-reversed-016", ol(li("5. ", value=5), E("counter-set: list-item 2"), E("counter-increment: list-item -1"),
                                  li("0. "), reversed=True))
case("li-value-reversed-017", ol(E("counter-increment: list-item -2"), li("2. "), reversed=True, start=4))
case("li-value-reversed-018", ol(li("1. "), E("counter-increment: list-item 1"), li("1. "), reversed=True))
case("li-value-reversed-019", ol(li("3. "), E("counter-increment: list-item -1"),
                                  ol(li("4. "), E("counter-set: list-item 3"), li("2. "), reversed=True),
                                  li("1. "), reversed=True))
case("li-value-reversed-020", ol(li("5. "), li("4. "), li("2. ", "counter-increment: list-item -2"), reversed=True))
case("li-value-reversed-021", ol(li("3. ", "counter-increment: list-item -3"), li("2. "), li("1. "), reversed=True, start=5))
case("li-value-reversed-022", ol(li("6. "), li("5. ", "counter-set: list-item 5"), li("4. "),
                                  li("2. ", "counter-set: list-item 2"), li("1. "), reversed=True))
case("li-value-reversed-023", ol(*items(3, 2, 1), style="counter-reset: reversed(list-item)"))
case("li-value-reversed-024", ol(*items(1, 2, 3), reversed=True, style="counter-reset: list-item"))
case("li-value-reversed-025", ol(*items(4, 5, 6), reversed=True, style="counter-reset: list-item 3"))
case("li-value-reversed-026", ol(*items(3, 2, 1), start=5, style="counter-reset: reversed(list-item)"))
case("li-value-reversed-027", ol(*items(3, 2, 1), start=9, style="counter-reset: reversed(list-item) 4"))
case("li-value-reversed-028", ol(E("counter-reset: reversed(list-item)", *items(3, 2, 1))))
case("li-value-reversed-029", ol(E("counter-reset: list-item", *items(1, 2, 3))))
case("li-value-reversed-030", ol(E("counter-reset: list-item 3", *items(4, 5, 6))))

# li { counter-increment: list-item 0; }
zero = "counter-increment: list-item 0"
for cls, want in (("", (0, 4, 4)), ("counter-increment: list-item -1", (-1, 4, 3)),
                  ("counter-increment: list-item 1 list-item -1", (0, 4, 4)),
                  ("counter-increment: list-item 3 list-item -1", (2, 4, 6))):
    rule = zero + ("; " + cls if cls else "")
    case("li-list-item-counter-001", ol(li("%d. " % want[0], rule), li("%d. " % want[1], rule, value=4),
                                        li("%d. " % want[2], rule)))
case("li-list-item-counter-001", ol(li("0. ", zero), li("4. ", zero + "; counter-increment: list-item 5", value=4),
                                    li("4. ", zero)))
case("li-list-item-counter-002", ol(li("2. ", value=2), E("counter-set: list-item 4"), E("counter-increment: list-item -1"),
                                    li("4. ")))
case("li-list-item-counter-003", ol(E("counter-increment: list-item 2"), li("4. "), start=2))
case("li-list-item-counter-004", ol(li("1. "), E("counter-increment: list-item 1"), li("3. ")))
case("li-list-item-counter-005", ol(li("1. "), E("counter-increment: list-item 1"),
                                    ol(li("1. "), E("counter-set: list-item -3"), li("-2. ")), li("3. ")))

# li:before { content: counter(list-item) " "; } li:after { content: " " counter(list-item); }
def listed(n, value=None):
    return li("%d. " % n, value=value, before=("content: counter(list-item) ' '", "%d " % n),
              after=("content: ' ' counter(list-item)", " %d" % n))


case("counter-list-item", ol(listed(1), listed(2), listed(3)), ol(listed(30), listed(31), listed(32), start=30),
     ol(listed(1), listed(2), listed(30, 30), listed(31), listed(35, 35), listed(36)))

# ol { counter-reset: reversed(foo); } li { display: block } li::before { counter-increment: foo -1;
# content: counter(foo) ". " } .set { counter-set: foo 10; }
foo = "counter-increment: foo -1; content: counter(foo) '. '"
case("counter-reset-reversed-nested",
     E("counter-reset: reversed(foo)", E("display: block", before=(foo, "3. ")),
       E("display: block", E("counter-reset: reversed(foo)", E("display: block", before=(foo, "11. ")),
                             E("display: block; counter-set: foo 10", before=(foo, "9. ")),
                             E("display: block", before=(foo, "8. "))), before=(foo, "2. ")),
       E("display: block", before=(foo, "1. "))))
item = dict(before=("counter-increment: foo -1; content: 'B' counter(foo)", None),
            after=("counter-increment: foo -2; content: 'A' counter(foo)", None))
case("counter-reset-reversed-pseudo-001",
     E("counter-reset: reversed(foo)", E(before=(item["before"][0], "B7"), after=(item["after"][0], "A5")),
       E(before=(item["before"][0], "B4"), after=(item["after"][0], "A2"))))
case("counter-reset-reversed-siblings-002",
     E("counter-reset: reversed(c)", E("counter-increment: c -1", before=("content: counter(c)", "1"))),
     E("counter-reset: reversed(c)", E("counter-increment: c -3", before=("content: counter(c)", "3"))))

# .scope { counter-reset: c 1; } .scope::before, .scope::after { content: counter(c); }
# .c::before { content: counter(c); } .one::before { counter-reset: c 2; } .two { counter-reset: c 3; }
cc = "content: counter(c)"
def scope(first, want):
    return E("counter-reset: c 1", E(first[0], E(before=(cc, want[2])), before=(first[1], want[1])),
             E(before=(cc, want[3])), before=(cc, want[0]), after=(cc, want[4]))


case("counters-scope-001",
     E("", scope(("", "counter-reset: c 2; " + cc), "12111"), E(before=(cc, "1")),
       scope(("counter-reset: c 3", cc), "13311"), E(before=(cc, "1"))))
# #scope, span#reset { counter-reset: c 0; } span::before { counter-increment: c 1; content: "B"
# counters(c,".") "-" } span::after { counter-increment: c 1; content: "A" counters(c,".") "-" }
B = "counter-increment: c 1; content: 'B' counters(c, '.') '-'"
A = "counter-increment: c 1; content: 'A' counters(c, '.') '-'"
def span(decls, b, a, *kids):
    return E(decls, *kids, before=(B, b), after=(A, a))


case("counters-scope-002",
     E("counter-reset: c 0",
       span("", "B1-", "A8-", span("", "B2-", "A7-",
                                   span("counter-reset: c 0", "B2.1-", "A2.6-", span("", "B2.2-", "A2.3-"),
                                        span("", "B2.4-", "A2.5-")),
                                   span("", "B3-", "A6-", span("", "B4-", "A5-"))))))
# body, span#reset:before { counter-reset: c 0; } (and the span::before and span::after above)
case("counters-scope-003",
     E("counter-reset: c 0", E(), E("", span("", "B1-", "A5-",
                                             E("", span("", "B2-", "A3-"), before=("counter-reset: c 0; " + B, "B1.1-"),
                                               after=(A, "A4-"))))))
# .reset { counter-reset: c; } .rb::before { counter-reset: c; content: "R"; } .use { counter-increment: c; }
# .use::before { content: counters(c, ".") " "; }
use = "content: counters(c, '.') ' '"
case("counters-scope-004",
     E("", E("counter-reset: c"), E("counter-increment: c", before=(use, "1 ")), E("counter-reset: c"),
       E("counter-increment: c", before=(use, "1 ")),
       E("", E("counter-increment: c", before=(use, "2 ")), E("counter-reset: c"),
         E("counter-increment: c", before=(use, "3 ")), before=("counter-reset: c; content: 'R'", "R"))))
# .i { counter-increment: c 1; } .ib:before { counter-increment: c 1; content: "B" } .r { counter-reset: c 0; }
# .u:before { content: counters(c, ".") " "; }
u = "content: counters(c, '.') ' '"
case("counters-005",
     E("", E("", E(before=(u, "1 ")), E("counter-reset: c 0", E(before=(u, "0 "))),
             before=("counter-increment: c 1; content: 'B'", "B")),
       E("counter-increment: c 1", E(before=(u, "1 ")), E("counter-reset: c 0", E(before=(u, "1.0 "))))))
# .test { counter-increment: section; counter-reset: multilevel; } ol[multilevel] > li::before {
# content: counter(section) "." counters(multilevel, ".") "."; counter-increment: multilevel; }
ml = "content: counter(section) '.' counters(multilevel, '.') '.'; counter-increment: multilevel"
case("counters-006",
     E("counter-increment: section; counter-reset: multilevel"),
     ol(*[li(None, "list-style: none", before=(ml, "1.%d." % k)) for k in (1, 2, 3)]),
     E("counter-increment: section; counter-reset: multilevel"),
     ol(*[li(None, "list-style: none", before=(ml, "2.%d." % k)) for k in (1, 2, 3)]))
# span::before { content: counters(n, '.'); }
n = "content: counters(n, '.')"
case("counter-set-001",
     E("", E("counter-set: n 7", before=(n, "7")), E("counter-set: n", before=(n, "0")),
       E("counter-set: n 8 n 7", before=(n, "7")), E("counter-set: n 6; counter-increment: n 2", before=(n, "6")),
       E("counter-set: n; counter-increment: n 2", before=(n, "0")),
       E("counter-reset: n 9", E("counter-set: n 2", before=(n, "2"))),
       E("counter-increment: n", before=(n, "3")),
       E("counter-reset: n 9", E("counter-set: n", before=(n, "0")), E("counter-set: n 2", before=(n, "2")),
         E("counter-reset: n 1", E("counter-set: n 5", before=(n, "2.5")))),
       E("counter-increment: n", before=(n, "3"))))
# .overflow { counter-reset: over-counter 2000000000; } .overflow h3:before { counter-increment:
# over-counter 50000000; content: counter(over-counter); } (and the underflow ones)
over = "counter-increment: over-counter 50000000; content: counter(over-counter)"
under = "counter-increment: under-counter -50000000; content: counter(under-counter)"
case("counter-reset-increment-overflow-underflow",
     E("counter-reset: over-counter 2000000000", *[E(before=(over, v)) for v in ("2050000000", "2100000000", "2100000000")]),
     E("counter-reset: under-counter -2000000000",
       *[E(before=(under, v)) for v in ("-2050000000", "-2100000000", "-2100000000")]))
# ol.with-custom-markers { list-style: none } li { list-style: none } > li::before { content:
# counter(list-item) ". "; }
custom = "content: counter(list-item) '. '"
case("implicit-and-explicit-list-item-counters",
     E(UA_OL + "; list-style: none",
       li(None, "list-style: none", before=(custom, "1. ")),
       li(None, "list-style: none", E(UA_UL, li(None, "list-style: none"), li(None, "list-style: none")),
          before=(custom, "2. ")),
       li(None, "list-style: none", before=(custom, "3. "))))
# ol { quotes: "\2018" "\2019"; } ::marker { content: open-quote counter(list-item) close-quote " "; }
# :nth-child(2)::marker { quotes: "«" "»"; } :nth-child(3)::marker { quotes: "‹" "›"; }
mq = "content: open-quote counter(list-item) close-quote ' '"
# .inc { counter-increment: x } .reset-6 { counter-reset: x 6 } .reset-666 { counter-reset: x 666 }
# .set-666 { counter-set: x 666 } .contents { display: contents } .result::before { content: counter(x) }
# (CSS Lists 3 §4.5: an element without a box does not touch counters; its children do)
result = ("content: counter(x)", "7")
case("counter-reset-increment-set-display-contents",
     E("", E("counter-reset: x 6"), E("display: contents; counter-reset: x 666; counter-increment: x"),
       E("display: contents; counter-set: x 666"), E("counter-increment: x", before=result)))
case("counter-increment-inside-display-contents",
     E("", E("counter-reset: x 6"), E("display: contents", E("counter-increment: x")), E(before=result)))
case("marker-quotes",
     E(UA_OL + "; quotes: '\u2018' '\u2019'", li("\u20181\u2019 ", marker_decls=mq),
       li("\u00ab2\u00bb ", marker_decls=mq + "; quotes: '\u00ab' '\u00bb'"),
       li("\u20393\u203a ", marker_decls=mq + "; quotes: '\u2039' '\u203a'")))


# counters-001 to -004: body, #test span:first-child { counter-reset: c; } p, #test span {
# counter-increment: c; } #test span::before { content: counters(c, ".", <style>); }, read from
# the page (the spans' own counter-reset) and its reference (the first line)
def counters_page(name):
    page = (wpt / (name + ".html")).read_text(encoding="utf-8-sig")
    ref = (wpt / (name + "-ref.html")).read_text(encoding="utf-8-sig")
    style = re.search(r'counters\(c, "\.", ([a-z-]+)\)', page).group(1)
    test = page[page.index('<div id="test">'):]
    test = test[:test.index("</div>")]
    resets = [m.group(1) for m in re.finditer(r'<span(?: style="counter-reset: c (\d+)")?></span>', test)]
    first = ref[ref.index("<div>") + 5:]
    first = first[:first.index("</div>")]
    texts = re.findall(r"<span>([^<]*)</span>", first) or first.split()
    texts = [html.unescape(t.strip()) for t in texts]
    if len(texts) != len(resets):
        sys.exit("%s：頁の span %d と参照の %d が合わない" % (name, len(resets), len(texts)))
    spans = []
    for k, (reset, text) in enumerate(zip(resets, texts)):
        decls = "counter-increment: c"
        if k == 0: decls = "counter-reset: c; " + decls
        if reset: decls += "; counter-reset: c " + reset
        spans.append(E(decls, before=("content: counters(c, '.', %s)" % style, text)))
    case(name, E("counter-reset: c", E("counter-increment: c"), E("", *spans)))


for name in ("counters-001", "counters-002", "counters-003", "counters-004"):
    counters_page(name)


# ---------------------------------------------------------------- C
def c_string(s):
    out = '"'
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch in '"\\': out += "\\" + ch
        elif 32 <= b < 127: out += ch
        else: out += "\\%03o" % b
    return out + '"'


def emit(e, parent, lines, names):
    v = "b%d" % len(names)
    names.append(v)
    lines.append("    box_t *%s = element(%s, %s);" % (v, parent, c_string(e.decls)))
    if e.marker_decls: lines.append("    own_marker(%s, %s);" % (v, c_string(e.marker_decls)))
    if e.marker is not None: lines.append("    want(%s, %s);" % (v, c_string(e.marker)))
    for which, kids in (("before", None), (None, e.kids), ("after", None)):
        if kids is not None:
            for k in kids: emit(k, v, lines, names)
            continue
        p = getattr(e, which)
        if not p: continue
        lines.append("    { box_t *p = pseudo(%s, %s);%s }" % (v, c_string(p[0]),
                     " want(p, %s);" % c_string(p[1]) if p[1] is not None else ""))
    lines.append("    (void)%s;" % v)


generated = ["static void cases(void) {"]
for name, kids in cases:
    lines, names = [], []
    for k in kids: emit(k, "root", lines, names)
    generated += ["    {", "    scene(%s);" % c_string(name)] + ["    " + l for l in lines] + ["    count();", "    }"]
generated += ["    scene(\"end\");", "}"]
prefix = """#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "webi.h"
"""
production = box_function("static bool counting_style(") + box_function("static bool marker_generated(")
native = out / "native.c"
native.write_text(prefix + production + "\n" + (root / "tests/counters_native_cases.c").read_text(encoding="utf-8") +
                  "\n" + "\n".join(generated) + "\n", encoding="utf-8", newline="\n")

# the repository's tools (a worktree has them in the main checkout above it)
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "counters.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
           "-Wl,--gc-sections", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include", "-Icommon",
           "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(native), "user/libc/web/counter.c",
           "user/libc/web/cssprop.c", "user/libc/web/shape.c", "user/libc/web/color.c", "user/libc/web/cssmath.c",
           "user/libc/web/util.c", "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
result = subprocess.run([str(exe)], cwd=root, env=env, timeout=60)
sys.exit(result.returncode)
