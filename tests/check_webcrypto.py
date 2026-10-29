"""Host check of the Web Cryptography API: WPT's WebCryptoAPI/ files, as they are, under WPT's
testharness.js in one host QuickJS realm with the product's js_dom_exception.js, js_encoding.js,
js_crypto.js and js_crypto.c over BearSSL.

What it does not show: the guest, a Window or Worker realm, js_clone.js's transport between
realms (serialization/ runs CryptoKey's serialize and deserialize steps through a stand-in
structuredClone), iframes (algorithm-discards-context), and idlharness (the interface shapes
come from js_webidl.c in the browser). tests/webcrypto_host.js stands in for timers, the
realm's posted tasks (the crypto task source), the console, structuredClone and the URL parser,
and webcrypto_hosttest.c for the thread and the word watch (web_atomics_watch_word) of RSA key
generation; .https. files run with an https realm URL and the others with an http one. The executable
and the logs go to the system temporary folder, never to build/.
    python -X utf8 tests/check_webcrypto.py [file filter]...
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
wpt = Path(r"D:\Programes\cloned repos\wpt")
out = Path(tempfile.gettempdir()) / "nocturne-webcrypto-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "webcrypto_hosttest.exe"
args = [a for a in sys.argv[1:] if not a.startswith("--")]

cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = os.environ.copy()
env["PATH"] = str(cc.parent) + os.pathsep + env["PATH"]
bearssl = [str(p) for p in sorted((root / "third_party/bearssl/src").rglob("*.c"))]
quickjs = [str(root / "third_party/quickjs" / (s + ".c")) for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
subprocess.run([str(cc), "-O2", "-std=gnu11", "-w", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE",
                '-DCONFIG_VERSION="Nocturne-test"', "-ffunction-sections", "-fdata-sections",
                "-I" + str(root / "third_party/quickjs"), "-I" + str(root / "third_party/bearssl/inc"),
                "-I" + str(root / "third_party/bearssl/src"), "-idirafter", str(root / "user/include"),
                "-I" + str(root / "common"), str(root / "tests/webcrypto_hosttest.c"), *bearssl, *quickjs,
                "-Wl,--gc-sections", "-Wl,--stack,8388608", "-lbcrypt", "-ladvapi32", "-lm", "-o", str(exe)],
               check=True, env=env)

REPORT = ("add_completion_callback((tests,status)=>{const names=['PASS','FAIL','TIMEOUT','NOTRUN','PRECONDITION_FAILED'];"
          "for(const t of tests)print('RESULT '+names[t.status]+' | '+t.name+(t.status?' | '+t.message:''));"
          "print('HARNESS '+status.status+' '+(status.message||''));});")
SKIP = ("modern-algos", "secure-curves", "idlharness", "crashtests", "secure_context")
api = wpt / "WebCryptoAPI"
files = sorted(p for p in api.rglob("*.any.js") if not any(s in p.as_posix() for s in SKIP))
if args:
    files = [f for f in files if any(a in f.as_posix() for a in args)]
totals = {"PASS": 0, "FAIL": 0}
for test in files:
    origin = "https://web-platform.test:8443" if ".https." in test.name else "http://web-platform.test:8000"
    scripts = ["-e", f"globalThis.__realmURL='{origin}/WebCryptoAPI/';",
               str(root / "tests/webcrypto_host.js"), str(root / "user/libc/web/js_dom_exception.js"),
               "-e", "globalThis.DOMException=domExceptionBridge.DOMException;globalThis.QuotaExceededError=domExceptionBridge.QuotaExceededError;",
               str(root / "user/libc/web/js_encoding.js"), str(root / "user/libc/web/js_crypto.js"),
               str(wpt / "resources/testharness.js"), "-e", REPORT]
    for line in test.read_text(encoding="utf-8").splitlines():
        m = re.match(r"//\s*META:\s*script=(\S+)", line)
        if m:
            src = m[1]
            scripts.append(str(wpt / src.lstrip("/")) if src.startswith("/") else str(test.parent / src))
    scripts.append(str(test))
    try:
        p = subprocess.run([str(exe), *scripts], capture_output=True, timeout=1800, env=env)
        text = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        text = "TIMEOUT"
    name = test.relative_to(api).as_posix() if test.is_relative_to(api) else test.name
    (out / (name.replace("/", "_") + ".log")).write_text(text, encoding="utf-8")
    results = [line.split(" | ") for line in text.splitlines() if line.startswith("RESULT ")]
    passed = sum(1 for r in results if r[0] == "RESULT PASS")
    harness = next((line for line in text.splitlines() if line.startswith("HARNESS")), "HARNESS ? (no report)")
    totals["PASS"] += passed
    totals["FAIL"] += len(results) - passed
    print(f"{name}: {passed}/{len(results)} {harness if not harness.startswith('HARNESS 0') else ''}".rstrip(), flush=True)
    shown = 0
    for r in results:
        if r[0] != "RESULT PASS" and shown < 25:
            print("   ", " | ".join(r)[7:][:300])
            shown += 1
    if not results:
        print("   ", text[-600:])
print(f"WebCryptoAPI: {totals['PASS']} passed, {totals['FAIL']} not passed (logs in {out})")
sys.exit(1 if totals["FAIL"] else 0)
