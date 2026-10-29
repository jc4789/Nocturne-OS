"""Host check of Resource Timing's contentType: MIME Sniffing "minimize a supported MIME type"
of a response's Content-Type, as js.c's minimized_content_type gives it to the timeline.

Cuts the product's functions out of their files as they are (js.c: the MIME helpers through
minimized_content_type; webi.h: lower; util.c: strn_ieq; image_source.c:
web_image_type_supported) into one file, builds tests/resource_content_type_hosttest.c with
it and user/libc/media_types.c (nmedia_can_play_type), and compares with WPT's expectations:
- mimesniff/mime-types/resources/mime-types-minimized.json (input -> output);
- mimesniff/mime-types/resources/mime-types.json: every navigable case that
  resource-timing/content-type-minimization.html sends (byte-compatible), its
  minimizedMIMEType.
Not the guest, and not the transport: which header block reaches it is js.c's and http.c's."""
from pathlib import Path
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
wpt = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Programes\cloned repos\wpt")
out = root / "build" / "resource-content-type-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "resource_content_type_hosttest.exe"
clang = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
             if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())  # a worktree uses its repository's


def cut(path, start, end):
    """The product text from the line that starts with start through the line before end."""
    text = (root / path).read_text("utf-8")
    a = text.index(start)
    b = text.index(end, a)
    return f'#line {text.count(chr(10), 0, a) + 1} "{(root / path).as_posix()}"\n' + text[a:b]


def function(path, signature):
    """One whole function: from its signature through its closing brace at column 0."""
    text = (root / path).read_text("utf-8")
    a = text.index(signature)
    b = text.index("\n}\n", a) + 3
    return f'#line {text.count(chr(10), 0, a) + 1} "{(root / path).as_posix()}"\n' + text[a:b]


sources = "\n".join([
    cut("user/libc/web/webi.h", "static inline int lower(int c)", "\n") + "\n",
    function("user/libc/web/util.c", "bool strn_ieq(const char *a, const char *b, size_t n) {"),
    function("user/libc/web/image_source.c", "bool web_image_type_supported(const char *type, size_t n) {"),
    cut("user/libc/web/js.c", "bool web_javascript_mime_essence(const char *value, size_t len) {",
        "static bool script_data_checkpoint(void *opaque) {"),
])
(out / "sources.inc").write_text(sources, "utf-8", newline="\n")
subprocess.run([str(clang), "-O1", "-Wall", "-Wno-unused-function", "-idirafter", str(root / "user/include"),
                "-idirafter", str(root / "common"),
                f'-DRESOURCE_CONTENT_TYPE_SOURCES="{(out / "sources.inc").as_posix()}"',
                str(root / "tests/resource_content_type_hosttest.c"), str(root / "user/libc/media_types.c"),
                "-o", str(exe)], check=True)


def byte_compatible(value):
    """content-type-minimization.html's isByteCompatible."""
    if re.search(r"^[\t \n\r]+|[\t \n\r]+$", value):
        return False
    return all(ord(c) <= 0xFF and c not in "\x00\n\r" for c in value)


cases = []  # (label, Content-Type value, wanted)
for i, case in enumerate(json.loads((wpt / "mimesniff/mime-types/resources/mime-types-minimized.json").read_text("utf-8"))):
    cases.append((f"mime-type-minimized {i}", case["input"], case["output"]))
for i, case in enumerate(json.loads((wpt / "mimesniff/mime-types/resources/mime-types.json").read_text("utf-8"))):
    if isinstance(case, str) or case.get("navigable") is None or not byte_compatible(case["input"]):
        continue
    cases.append((f"mime-type {i}", case["input"], case["minimizedMIMEType"]))
# A header value carries bytes: the test page's characters up to U+00FF are bytes.
data = "".join(value.encode("latin-1").hex() + "\n" for _, value, _ in cases).encode("ascii")
got = subprocess.run([str(exe)], input=data, capture_output=True, check=True).stdout.decode("latin-1").splitlines()
failed = 0
for (label, value, want), result in zip(cases, got):
    if result != want:
        failed += 1
        print(f"FAIL {label} {value!r}: {result!r}, wanted {want!r}")
if len(got) != len(cases):
    failed += 1
    print(f"FAIL harness answered {len(got)} of {len(cases)}")
print(f"resource content type: {len(cases)} checks, {failed} failed")
sys.exit(1 if failed else 0)
