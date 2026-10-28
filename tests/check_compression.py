"""Host check of the Compression Standard's streams: WPT's compression/ files, as they are, under
WPT's testharness.js in one host QuickJS realm with the product's js_encoding.js, js_streams.js
(and its pinned Streams ponyfill) and js_compression.c over deflate.c and the brotli library.

What it does not show: the guest, a Window or Worker realm (META: global=), Fetch, Blob,
postMessage and timers, which tests/compression_host.js stands in for. The product's QuickJS has
no Array.fromAsync, which resources/decompress.js uses for brotli round trips; --fromasync adds a
stand-in to see the codec under those tests anyway.
    python -X utf8 tests/check_compression.py [--fromasync] [file filter]
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(r"D:\Programes\cloned repos\wpt")
out = root / "build" / "compression-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "compression_hosttest.exe"
args = [a for a in sys.argv[1:] if not a.startswith("--")]
fromasync = "--fromasync" in sys.argv[1:]

brotli = [str(p) for part in ("common", "dec", "enc") for p in sorted((root / "third_party/brotli/c" / part).glob("*.c"))]
quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
subprocess.run([str(root / "tools/msys64/ucrt64/bin/clang.exe"), "-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char",
                "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"', "-DBROTLI_BUILD_PORTABLE",
                "-I" + str(root / "third_party/quickjs"), "-I" + str(root / "third_party/brotli/c/include"),
                "-idirafter", str(root / "user/include"), str(root / "tests/compression_hosttest.c"),
                str(root / "user/libc/web/js_compression.c"), str(root / "user/libc/deflate.c"),
                str(root / "user/libc/third_party_brotli.c"), *brotli, *quickjs, "-lm", "-o", str(exe)], check=True)

# js_streams.js with the ponyfill in place, as js_embed.py puts it in the bootstrap.
module = (root / "third_party/web_streams/ponyfill.mjs").read_text(encoding="utf-8")
module, count = re.subn(r'export\{([A-Za-z,]+)\};\s*$', r'return {\1};', module)
streams = (root / "user/libc/web/js_streams.js").read_text(encoding="utf-8")
if count != 1 or streams.count("/* @streams-ponyfill */") != 1:
    raise SystemExit("Streams export/include contract changed")
expanded = out / "js_streams_expanded.js"
expanded.write_text(streams.replace("/* @streams-ponyfill */", module), encoding="utf-8")

REPORT = ("add_completion_callback((tests,status)=>{const names=['PASS','FAIL','TIMEOUT','NOTRUN','PRECONDITION_FAILED'];"
          "for(const t of tests)print('RESULT '+names[t.status]+' | '+t.name+(t.status?' | '+t.message:''));"
          "print('HARNESS '+status.status+' '+(status.message||''));});")
FROMASYNC = ("Array.fromAsync=async function(items){const out=[];for await(const v of items)out.push(v);return out;};")
STATUS = {}
files = sorted((wpt / "compression").glob("*.any.js")) + [root / "tests/compression_cases.js"]
if args:
    files = [f for f in files if any(a in f.name for a in args)]
totals = {"PASS": 0, "FAIL": 0}
for test in files:
    scripts = [str(root / "tests/compression_host.js"), str(root / "user/libc/web/js_encoding.js"), str(expanded),
               str(wpt / "resources/testharness.js"), "-e", REPORT]
    if fromasync:
        scripts += ["-e", FROMASYNC]
    if test.parent == root / "tests":  # this repository's own cases
        scripts += [str(wpt / "compression/third_party/pako/pako_inflate.min.js"),
                    str(wpt / "compression/resources/concatenate-stream.js")]
    for line in test.read_text(encoding="utf-8").splitlines():
        m = re.match(r"//\s*META:\s*script=(\S+)", line)
        if m:
            src = m[1].replace("/resources/WebIDLParser.js", "/resources/webidl2/lib/webidl2.js")  # wptserve's rewrite
            scripts.append(str(wpt / src.lstrip("/")) if src.startswith("/") else str(test.parent / src))
    scripts.append(str(test))
    try:
        p = subprocess.run([str(exe), str(wpt), *scripts], capture_output=True, timeout=600)
        text = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        text = "TIMEOUT"
    (out / (test.name + ".log")).write_text(text, encoding="utf-8")
    results = [line.split(" | ") for line in text.splitlines() if line.startswith("RESULT ")]
    passed = sum(1 for r in results if r[0] == "RESULT PASS")
    harness = next((line for line in text.splitlines() if line.startswith("HARNESS")), "HARNESS ? (no report)")
    totals["PASS"] += passed
    totals["FAIL"] += len(results) - passed
    print(f"{test.name}: {passed}/{len(results)} {harness if not harness.startswith('HARNESS 0') else ''}".rstrip())
    for r in results:
        if r[0] != "RESULT PASS":
            print("   ", " | ".join(r)[7:][:300])
    if not results:
        print("   ", text[-600:])
print(f"compression: {totals['PASS']} passed, {totals['FAIL']} not passed")
sys.exit(1 if totals["FAIL"] else 0)
