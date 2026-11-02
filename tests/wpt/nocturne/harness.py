# The page side of scripts/wpt.py, served by wptserve at /_nocturne/harness.py on every origin.
#   GET ?slot=N   a slot's browser starts here: redirect to the slot's next test
#   POST          a test document's results (tests/wpt/testharnessreport.js): the next test's URL,
#                 or nothing when the slot should wait (the harness restarts or closes it)
# The harness's own server (NOCTURNE_WPT_COLLECTOR, 127.0.0.1:port) decides; this only forwards.
import os
import urllib.request


def collector(path, body=None):
    address = os.environ["NOCTURNE_WPT_COLLECTOR"]
    request = urllib.request.Request("http://%s%s" % (address, path), data=body, method="POST" if body is not None else "GET")
    with urllib.request.urlopen(request, timeout=30) as reply:
        return reply.read()


def main(request, response):
    headers = [("Cache-Control", "no-store"), ("Content-Type", "text/plain;charset=utf-8")]
    if request.method == "POST":
        return 200, headers, collector("/result", request.body)
    slot = request.GET.first(b"slot", b"0").decode("ascii")
    target = collector("/next?slot=%s" % slot)
    if not target:
        return 200, headers, b"idle"
    return 302, headers + [("Location", target)], b""
