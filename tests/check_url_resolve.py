"""Host check of the native callers' URL parser: web_url_resolve and web_resolve_url_owned in
user/libc/web/web_url.c, which the engine's own resolution (img, link, script, iframe, form
action, base, CSS url() and @import, redirects) goes through.

Builds tests/url_resolve_hosttest.c with web_url.c, web_encoding.c, util.c and Lexbor's core,
encoding, punycode, unicode and URL modules, then compares it with WPT's own expectations:
- url/resources/urltestdata.json: every case with an input and a base (or none), through
  web_resolve_url_owned (the URL parser, UTF-8). A case whose base is not a URL is left out
  (the native entry takes such a base as none, where new URL() throws), as is an input or base
  with U+0000 (a native caller's strings end there);
- url/resources/percent-encoding.json: the query in each encoding, the fragment in UTF-8, as
  url/percent-encoding.window.js reads them from a document of that encoding (encoding-parse);
- html/infrastructure/urls/resolving-urls/query-encoding/resources/resolve-url.js: its input
  relative to a base in each of its document encodings.
Inputs are given as the DOM holds strings: UTF-8, a lone surrogate as its three bytes.
Then the harness runs every request again on 8 threads at once and reports any result that
differs (the parsers it lends are never shared between threads).
Not the guest: which encoding a document passes is doc.c's (doc_encoding_parse_url)."""
from pathlib import Path
import json
import struct
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Programes\cloned repos\wpt")
out = root / "build" / "url-resolve-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "url_resolve_hosttest.exe"
clang = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
             if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())  # a worktree uses its repository's

modules = ("core", "encoding", "punycode", "unicode", "url")
lexbor = [str(root / line.split("+=")[1].strip())
          for line in (root / "third_party/lexbor/sources.mk").read_text("utf-8").splitlines()
          if "+=" in line and line.startswith("LXB_VENDOR_C") and
          any(f"/lexbor/{m}/" in line for m in modules)]
lexbor_flags = ["-O2", "-std=gnu11", "-DLEXBOR_STATIC", "-I" + str(root / "third_party/lexbor/source"),
                "-I" + str(root / "ports/lexbor/include")]
# The harness with the host's headers; the engine's files with Nocturne's (user/include).
subprocess.run([str(clang), *lexbor_flags, "-Wall", "-c", str(root / "tests/url_resolve_hosttest.c"),
                "-o", str(out / "harness.o")], check=True)
subprocess.run([str(clang), *lexbor_flags, "-Wall", "-Wextra", "-I" + str(root / "user/include"),
                "-I" + str(root / "common"), "-c", str(root / "user/libc/web/web_url.c"),
                "-o", str(out / "web_url.o")], check=True)
subprocess.run([str(clang), *lexbor_flags, "-w", "-I" + str(root / "user/include"), "-I" + str(root / "common"),
                str(out / "harness.o"), str(out / "web_url.o"), str(root / "user/libc/web/web_encoding.c"),
                str(root / "user/libc/web/util.c"), str(root / "ports/lexbor/memory.c"), *lexbor,
                "-o", str(exe)], check=True)


def dom(text):
    return text.encode("utf-8", "surrogatepass")


cases = []  # (label, base or None, input, expected href or None, what)
skipped = 0
for case in json.loads((wpt / "url/resources/urltestdata.json").read_text("utf-8")):
    if isinstance(case, str):
        continue
    base = case.get("base")
    if "\0" in case["input"] or (base and "\0" in base):
        skipped += 1
        continue
    cases.append(("", base, case["input"], None if case.get("failure") else case["href"],
                  "urltestdata"))
base_checks = {}  # base -> whether it is a URL, from the harness itself (first pass below)

for case in json.loads((wpt / "url/resources/percent-encoding.json").read_text("utf-8")):
    if isinstance(case, str):
        continue
    for encoding, query in case["output"].items():
        value = case["input"]
        cases.append((encoding, None, "https://doesnotmatter.invalid/?" + value + "#" + value,
                      "https://doesnotmatter.invalid/?" + query + "#" + case["output"]["utf-8"],
                      "percent-encoding"))

query_encoding = {"utf-8": "%C3%A5", "utf-16be": "%C3%A5", "utf-16le": "%C3%A5",
                  "windows-1252": "%E5", "windows-1251": "%26%23229%3B"}
base = "http://web-platform.test:8000/html/infrastructure/urls/resolving-urls/query-encoding/windows-1252.html"
for encoding, query in query_encoding.items():
    cases.append((encoding, base, "resources/resource.py?q=\u00E5&encoding=" + encoding + "&type=html",
                  "http://web-platform.test:8000/html/infrastructure/urls/resolving-urls/query-encoding/"
                  "resources/resource.py?q=" + query + "&encoding=" + encoding + "&type=html",
                  "query-encoding"))
# The fragment and a URL that is not special keep UTF-8 whatever the document's encoding
# (resolve-url.js's "Scheme ... (getting <a>.href)" cases).
for url, utf8 in (("http://example.invalid/?x=\u00E5", False), ("ws://example.invalid/?x=\u00E5", True),
                  ("mailto:example@invalid?x=\u00E5", True), ("web+http:foo?x=\u00E5", True)):
    cases.append(("windows-1252", base, url, url.replace("\u00E5", "%C3%A5" if utf8 else "%E5"),
                  "query-encoding scheme"))


def request(label, base, text):
    def put(b):
        return struct.pack("<I", len(b)) + b
    return put(label.encode()) + (struct.pack("<I", 0xffffffff) if base is None else put(dom(base))) + \
        put(dom(text))


# Which bases are URLs: the harness parses each alone first.
bases = sorted({c[1] for c in cases if c[1] is not None})
probe = out / "bases.bin"
probe.write_bytes(b"".join(request("", None, b) for b in bases))
subprocess.run([str(exe), str(probe), str(out / "bases.out")], check=True, capture_output=True)
raw = (out / "bases.out").read_bytes()
at = 0
for b in bases:
    status, length = struct.unpack_from("<bI", raw, at)
    at += 5 + length
    base_checks[b] = status == 1
kept = []
for c in cases:
    if c[1] is not None and not base_checks[c[1]]:
        skipped += 1
        continue
    kept.append(c)
cases = kept

(out / "requests.bin").write_bytes(b"".join(request(c[0], c[1], c[2]) for c in cases))
run = subprocess.run([str(exe), str(out / "requests.bin"), str(out / "results.bin")],
                     capture_output=True, text=True)
print(run.stdout.strip())
raw = (out / "results.bin").read_bytes()
at, failed = 0, []
for c in cases:
    status, length = struct.unpack_from("<bI", raw, at)
    at += 5
    href = raw[at:at + length].decode("utf-8")
    at += length
    got = href if status == 1 else None
    if status < 0 or got != c[3]:
        failed.append((c, status, got))
for (label, base, text, want, what), status, got in failed[:40]:
    print(f"FAIL {what} [{label or 'utf-8'}] {text!r} against {base!r}: want {want!r}, got {got!r} ({status})")
print(f"check_url_resolve: {len(cases)} cases, {len(failed)} failed, {skipped} left out")
sys.exit(1 if failed or run.returncode else 0)
