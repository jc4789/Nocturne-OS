"""Host check of the product QuickJS (third_party/quickjs) against WPT's vendored test262.

Builds tests/test262_hosttest.c with the engine's sources and runs each script test under the
directories given (relative to test262's test/), with its harness files, in the modes its flags
ask (strict and sloppy unless onlyStrict, noStrict or raw), as test262's INTERPRETING.md says.
Module tests and tests needing $262.agent are skipped and counted.

What it does not show: the guest, the browser's realms (Window, Worker) and their job queue,
which the WPT harness (scripts/wpt.py --type test262) runs.
    python -X utf8 tests/check_test262.py built-ins/Array/fromAsync [more directories] [-j N]
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
test262 = Path(r"D:\Programes\cloned repos\wpt\third_party\test262")
out = root / "build" / "test262-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "test262_hosttest.exe"

args = sys.argv[1:]
jobs = 8
if "-j" in args:
    i = args.index("-j")
    jobs = int(args[i + 1])
    del args[i:i + 2]
if not args:
    raise SystemExit(__doc__)

quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
subprocess.run([str(root / "tools/msys64/ucrt64/bin/clang.exe"), "-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char",
                "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"', "-I" + str(root / "third_party/quickjs"),
                str(root / "tests/test262_hosttest.c"), *quickjs, "-lm", "-o", str(exe)], check=True)


def metadata(source):
    m = re.search(r"/\*---(.*?)---\*/", source, re.S)
    text = m.group(1) if m else ""

    def listed(key):
        m = re.search(r"^%s:\s*\[(.*?)\]" % key, text, re.M)
        if m:
            return [x.strip() for x in m.group(1).split(",") if x.strip()]
        m = re.search(r"^%s:\s*\n((?:\s+-\s*.*\n?)+)" % key, text, re.M)
        return [x.strip()[1:].strip() for x in m.group(1).splitlines() if x.strip()] if m else []

    negative = re.search(r"^negative:\s*\n\s+phase:\s*(\w+)\s*\n\s+type:\s*(\w+)", text, re.M)
    return listed("includes"), listed("flags"), negative.groups() if negative else None


harness = {}


def harness_file(name):
    if name not in harness:
        harness[name] = (test262 / "harness" / name).read_text(encoding="utf-8")
    return harness[name]


def run(path, strict):
    source = path.read_text(encoding="utf-8")
    includes, flags, negative = metadata(source)
    parts = [] if "raw" in flags else ["assert.js", "sta.js"] + (["doneprintHandle.js"] if "async" in flags else []) + includes
    script = ('"use strict";\n' if strict else "") + "".join(harness_file(p) + "\n" for p in parts) + source
    name = path.relative_to(test262 / "test").as_posix() + (" [strict]" if strict else "")
    file = out / "scripts" / (re.sub(r"[^A-Za-z0-9_.-]", "_", name) + ".js")
    file.parent.mkdir(exist_ok=True)
    file.write_text(script, encoding="utf-8", newline="\n")
    try:
        result = subprocess.run([str(exe), str(file)], capture_output=True, timeout=20)
        output = result.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return name, "TIMEOUT"
    if result.returncode != 0:
        return name, "CRASH %d" % result.returncode
    exception = re.search(r"^exception: (\w+|\(not an Error\)): ?(.*)$", output, re.M)
    if negative:
        if exception and exception.group(1) == negative[1]:
            return name, None
        return name, "expected %s %s, got %s" % (negative[0], negative[1], exception.group(0) if exception else "none")
    if exception:
        return name, exception.group(0)
    if "async" in flags:
        if "Test262:AsyncTestComplete" in output:
            return name, None
        failure = re.search(r"^Test262:AsyncTestFailure:(.*)$", output, re.M)
        return name, failure.group(1) if failure else "the async test never finished"
    return name, None


tests, skipped = [], 0
for directory in args:
    for path in sorted((test262 / "test" / directory).rglob("*.js")):
        if path.name.endswith("_FIXTURE.js"):
            continue
        source = path.read_text(encoding="utf-8")
        includes, flags, negative = metadata(source)
        if "module" in flags or "$262.agent" in source:
            skipped += 1
            continue
        modes = [True] if "onlyStrict" in flags else [False] if ("noStrict" in flags or "raw" in flags) else [False, True]
        tests += [(path, strict) for strict in modes]

with ThreadPoolExecutor(jobs) as pool:
    results = list(pool.map(lambda t: run(*t), tests))
failed = [(n, why) for n, why in results if why]
for name, why in failed:
    print("FAIL %s: %s" % (name, why))
print("test262: %d passed, %d failed, %d skipped (module or agent)" % (len(results) - len(failed), len(failed), skipped))
sys.exit(1 if failed else 0)
