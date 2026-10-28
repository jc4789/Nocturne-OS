"""Host check of the one URL record every native layer reads (user/include/weburl.h, made by
user/libc/web/web_url.c on Lexbor's URL parser).

Every http(s) URL WPT's url/resources/urltestdata.json expects the parser to give (its "href",
each distinct one once) goes through the request layer (http.c's http_target_of, webfetch.c's
http_url, sliced from the product as written), the cookie jar (user/libc/webcookie.c whole, with
rootfs's Public Suffix List) and the origin entry (web_url_origin, web_origin_http,
web_url_same_origin). tests/url_record_hosttest.c checks that each layer takes it and that the
host, the port, the request target and the origin are the ones WPT gives: hostname, host,
port (or the scheme's default), the serialization up to the fragment after the authority, and
origin (scheme "://" host where WPT leaves it out). Two more URLs are serializations a layer was
seen to refuse (the ledger's K80 and K94).
Bounded evidence: the layers' entries, not a request on the network or the guest's browser.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import url_owner

root = Path(__file__).resolve().parent.parent
wpt = Path(sys.argv[1] if len(sys.argv) > 1 else r"D:\Programes\cloned repos\wpt")
out = root / "build/url-record-host"
out.mkdir(parents=True, exist_ok=True)


def c_string(text):
    return '"' + "".join("\\" + ch if ch in '\\"' else ch if 32 <= ord(ch) < 127 else f"\\{ord(ch):03o}"
                         for ch in text) + '"'


seen = {}
for case in json.loads((wpt / "url/resources/urltestdata.json").read_text("utf-8")):
    if not isinstance(case, dict) or case.get("failure") or case.get("protocol") not in ("http:", "https:"):
        continue
    href = case["href"]
    origin = case["protocol"] + "//" + case["host"]
    if "origin" in case and case["origin"] != origin:
        raise SystemExit(f"{href}: WPT's origin {case['origin']} is not {origin}")
    without_fragment = href.split("#", 1)[0]
    target = without_fragment[without_fragment.index("/", len(case["protocol"]) + 2):]
    port = int(case["port"]) if case["port"] else 443 if case["protocol"] == "https:" else 80
    entry = (href, origin, case["hostname"], case["host"], target, without_fragment, port, case["protocol"] == "https:")
    if seen.setdefault(href, entry) != entry:
        raise SystemExit(f"{href}: two different expectations")
wpt_count = len(seen)
# Serializations a layer was seen to refuse before it read the record (docs/browser/LEDGER.md
# K80, K94): a domain with '_' (no forbidden domain code point) and '\' in a special URL's query
# and fragment (neither percent-encode set has it), as the URL Standard serializes them.
for entry in [("https://a_b.example.test:8443/log/web?a\\b|c#d\\e", "https://a_b.example.test:8443",
               "a_b.example.test", "a_b.example.test:8443", "/log/web?a\\b|c",
               "https://a_b.example.test:8443/log/web?a\\b|c", 8443, True),
              ("http://data_x.example.test/log?x=\\", "http://data_x.example.test", "data_x.example.test",
               "data_x.example.test", "/log?x=\\", "http://data_x.example.test/log?x=\\", 80, False)]:
    if entry[0] in seen:
        raise SystemExit(f"{entry[0]}: already one of WPT's")
    seen[entry[0]] = entry
lines =["static const struct url_case cases[] = {"]
for href, origin, hostname, host, target, url, port, tls in seen.values():
    lines.append("    {" + ", ".join(map(c_string, (href, origin, hostname, host, target, url))) +
                 f", {port}, {'true' if tls else 'false'}}},")
lines.append("};")
(out / "url_record_cases.inc").write_text("\n".join(lines) + "\n", encoding="utf-8")


def between(source, start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]


http = (root / "user/libc/http.c").read_text(encoding="utf-8")
fetch = (root / "user/apps/webfetch.c").read_text(encoding="utf-8")
prefix = "\n".join(["#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", "#include <stdbool.h>",
                    "#include <stdint.h>", "#include <time.h>", '#include "http.h"', '#include "webcookie.h"', ""])
generated = out / "url_record.c"
generated.write_text(prefix + between(http, "enum web_url_result http_target_of(", "struct rd {") +
                     between(fetch, "static enum web_url_result http_url(", "static bool url_failure(") +
                     (root / "tests/url_record_hosttest.c").read_text(encoding="utf-8"), encoding="utf-8")

cc = url_owner.clang(root)
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
owner = url_owner.objects(root, cc)
exe = out / "url_record.exe"
log = out / "result.log"
with log.open("w", encoding="utf-8") as stream:
    for name in ["user/include/weburl.h", "user/libc/web/web_url.c", "user/libc/http.c", "user/include/http.h",
                 "user/apps/webfetch.c", "user/libc/webcookie.c", "tests/url_record_hosttest.c",
                 "tests/check_url_record.py"]:
        stream.write(name + " SHA256 " + hashlib.sha256((root / name).read_bytes()).hexdigest() + "\n")
    stream.flush()
    subprocess.run([str(cc), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
                    "-fsanitize=undefined", "-fsanitize-trap=undefined", "-iquote", str(out), "-iquote", "user/include",
                    "-idirafter", "user/include", "-idirafter", "common", str(generated), "user/libc/webcookie.c",
                    *owner, "-o", str(exe)], cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe), "rootfs/Nocturne/Web/public_suffix_list.dat"], cwd=root, env=env, timeout=120,
                            stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding="utf-8"), end="")
print(f"url_record: {wpt_count} of the URLs are WPT's, {len(seen) - wpt_count} the ledger's")
raise SystemExit(result.returncode)
