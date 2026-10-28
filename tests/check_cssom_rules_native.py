"""CSSOM の規則の木の持ち主（cssom.c）を、css.c の解析と DOM と一緒に host で確かめる。

tests/cssom_rules_native_cases.c の各件は、WPT の頁（css/css-nesting/cssom.html、
serialize-group-rules-with-decls.html、css/cssom/CSSKeyframesRule*.html、cssom-pagerule.html
など）か仕様の節を名に書いた、insertRule・deleteRule・「serialize a CSS rule」・規則の属性・
MediaList・宣言の塊・CSSRule の同一性の問いと期待。JS の束縛（例外の型、IDL の形、
[SameObject]）は guest の WPT の仕事で、ここでは照らさない。

tests/cssom_splice_cases.c は、規則ごとの変更（変わった最上位の規則だけを解析し、AST の
列と規則の木と cascade の索引に差し込む）のあとの AST と木と索引が、同じ頁の全文を一度に
解析したものと同じであること、CSSRule の id が規則を指し続けること、置き換えた規則の
記憶が溜まり続けないこと、insertRule を一つずつ 5000 回する時間が 1000 回の十倍に届か
ないことを確かめる。

tests/cssom_bindings_cases.c は、製品の js_stylesheets.js を製品の QuickJS と橋（js_cssom.h）と
規則の木の上で走らせ、tests/cssom_bindings_cases.js の件（CSSImportRule.styleSheet、
CSSMediaRule・CSSSupportsRule の matches、keyframe の !important、入れ子の selectorText、
CSSStyleSheetInit、nameList の同一性、規則の setProperty が cascade の読む文に与えたままの値を
置くこと）を照らす。js.c の宣言の塊の関数は js.c から切り出す。

tests/cssom_import_cascade_cases.c は、doc.c の資源の走査（scan）が、@import の規則に CSSOM が
与えたシートの解析（CSSOM が読むものと同じ一つ）を cascade に入れ、その変更と disabled が
cascade に届くことを確かめる。

tests/cssom_shorthand_cases.c は、要素のインラインの style を js.c と同じ順（要素の宣言の塊、
css_inline_set・css_inline_replace）で動かし、WPT の css/cssom の shorthand の頁
（shorthand-values.html、shorthand-serialization.html、cssstyledeclaration-setter-declarations.html、
-setter-logical.html、cssom-setProperty-shorthand.html、style-border-shorthand-var.html、
font-shorthand-serialization.html）の cssText・getPropertyValue・setProperty・removeProperty・
名の並びを照らす。

tests/cssom_style_mutation_cases.c は、js.c の DOM の呼出しの「変更か」の式、cssText の setter
（style_block_access）、属性の書込み（style_attribute_set）を js.c から切り出し、js_mutations.h と
一緒に組んで、css/cssom/MutationObserver-style.html の二つの問い（同じ値の setProperty は記録を
出さず、同じ値の cssText は毎回出す）を MutationObserver の記録の数で照らす。

  python -X utf8 tests/check_cssom_rules_native.py [--old] [rules|splice|bindings|cascade|shorthand|mutation ...]

--old は前の源で同じ件を組み、変更前の数を出す。規則の木の件は規則の木の前（930c2d13）で、
前の口で書けない件（子の規則、規則の種類ごとの属性、MediaList、宣言）は落ちる。差し込みの
件は規則ごとの変更の前（8cbe1194、変更のたびに頁の全文を解析し直した）で、時間の件が落ちる。
束縛と cascade の件は 4983e50e（CSSImportRule.styleSheet の前）で、束縛の件は落ち、cascade の
件は前の口で書けない（@import の規則にシートが無い）ので落ちる。shorthand の件は 9e120cd7（宣言の
塊が直列化の文で、shorthand が一つの宣言だった）で、同じ件を前の口（css_decl_serialize・
css_decl_find・css_decl_names）で走らせる。記録の件も 9e120cd7（cssText の setter が記録を出さ
なかった）で、cssText の二件が落ちる。
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parent.parent
old = "--old" in sys.argv
cc = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
          if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
undefined = re.compile(r"undefined (?:symbol: |reference to `)([A-Za-z_][A-Za-z0-9_]*)")


def source_of(out, revision):
    """the tree to build from: this one, or an old revision's web engine"""
    if not old:
        return root
    source = out / "src"
    if not (source / "user/libc/web/cssom.c").exists():
        archive = out / "old.tar"
        subprocess.run(["git", "archive", "--format=tar", "-o", str(archive), revision, "user/libc/web",
                        "user/include", "common"], cwd=root, check=True)
        with tarfile.open(archive) as t:
            t.extractall(source, filter="data")
    return source


def quickjs(out):
    """the product's QuickJS, built once for the host"""
    objects = []
    for unit in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils"):
        obj = out / f"{unit}.o"
        source = root / "third_party/quickjs" / f"{unit}.c"
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            subprocess.run([str(cc), "-std=gnu11", "-O1", "-w", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE",
                            '-DCONFIG_VERSION="Nocturne-test"', "-I" + str(root / "third_party/quickjs"), "-c",
                            str(source), "-o", str(obj)], cwd=root, env=env, check=True)
        objects.append(str(obj))
    return objects


def bindings(out, source):
    """js.c's declaration block functions the bridge calls, copied out of js.c, and QuickJS"""
    js = (source / "user/libc/web/js.c").read_text(encoding="utf-8")
    parts = []
    # the source's own (the bridge at 4983e50e also called style_serialize)
    for signature in ("static bool style_serialize(", "static JSValue style_text_access(", "static JSValue style_text_block("):
        at = js.find(signature)
        if at >= 0:
            parts.append(js[at:js.index("\n}\n", at) + 3])
    (out / "bindings-extract.h").write_text("\n".join(parts), encoding="utf-8", newline="\n")
    return quickjs(out), ["-I" + str(out)], [str(source / "user/libc/web/js_stylesheets.js")]


def mutations(out, source):
    """js.c's native DOM call's mutation test, the cssText setter and the attribute write it ends
    in, copied out of js.c (js_mutations.h is the source's own), and QuickJS"""
    js = (source / "user/libc/web/js.c").read_text(encoding="utf-8")
    at = js.index("bool mutation = opcode ==")
    parts = ["static bool dom_mutates(int opcode, int argc) { return " + js[at + len("bool mutation = "):js.index(";", at)] +
             "; }\n"]
    for signature in ("static bool style_attribute_set(", "static JSValue style_block_access("):
        at = js.index(signature)
        parts.append(js[at:js.index("\n}\n", at) + 3])
    (out / "mutation-extract.h").write_text("\n".join(parts), encoding="utf-8", newline="\n")
    return quickjs(out), ["-I" + str(out)], []


def document_headers(out, source):
    """doc.c's headers beyond the web engine's: the libraries the product builds libc with"""
    return [], ["-I" + str(root / p) for p in ("third_party/bearssl/inc", "third_party/img", "third_party/lexbor/source",
                                               "ports/lexbor/include", "third_party/freetype/include")], []


def program(name, case_file, revision, owner, extra=None):
    """cases built with the units beside them (owner: the one the cases include), run"""
    out = root / "build/check-cssom-rules" / (f"old-{name}" if old else name)
    out.mkdir(parents=True, exist_ok=True)
    source = source_of(out, revision)
    more_units, more_flags, args = extra(out, source) if extra else ([], [], [])
    abi = out / "host-abi"
    abi.mkdir(exist_ok=True)
    (abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n',
                                  encoding="utf-8")
    web = source / "user/libc/web"
    cases = out / f"{name}-generated.c"
    cases.write_text(("#define OLD_SOURCE 1\n" if old else "") + (root / case_file).read_text(encoding="utf-8"),
                     encoding="utf-8")
    stubs = out / "unreached.c"
    exe = out / f"{name}.exe"
    units = [str(cases)] + [str(web / u) for u in ("css.c", "cssom.c", "dom.c", "html.c", "util.c", "cssprop.c",
                                                   "color.c", "cssmath.c")
                            if u != owner and (web / u).exists()]
    flags = ["-std=gnu11", "-O1", "-w", "-I" + str(abi), "-I" + str(source / "user/include"),
             "-I" + str(source / "common"), "-I" + str(source / "user/libc"), "-I" + str(web),
             "-I" + str(root / "third_party/quickjs")] + more_flags
    names = set()
    for attempt in range(6):
        # every external the linked units reach but these cases never call: a call stops with its name
        stubs.write_text("int puts(const char *);\nvoid exit(int);\n" + "".join(
            f'void {n}(void) {{ puts("unreached: {n}"); exit(3); }}\n' for n in sorted(names)), encoding="utf-8")
        r = subprocess.run([str(cc), *flags, *units, *more_units, str(stubs), "-lm", "-o", str(exe)], cwd=root, env=env,
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
    result = subprocess.run([str(exe), *args], cwd=root, env=env, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=1200)
    print(result.stdout, end="")
    if result.returncode not in (0, 1):
        print(result.stderr, end="")
        sys.exit(f"道具が止まった（{result.returncode}）")
    return result.returncode


programs = {"rules": ("tests/cssom_rules_native_cases.c", "930c2d13", "cssom.c", None),
            "splice": ("tests/cssom_splice_cases.c", "8cbe1194", "css.c", None),
            "bindings": ("tests/cssom_bindings_cases.c", "4983e50e", "cssom.c", bindings),
            "cascade": ("tests/cssom_import_cascade_cases.c", "4983e50e", "doc.c", document_headers),
            "shorthand": ("tests/cssom_shorthand_cases.c", "9e120cd7", None, None),
            "mutation": ("tests/cssom_style_mutation_cases.c", "9e120cd7", None, mutations)}
chosen = [a for a in sys.argv[1:] if a in programs] or list(programs)
status = 0
for name in chosen:
    status |= program(name, *programs[name])
sys.exit(status)
