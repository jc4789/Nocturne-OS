#!/usr/bin/env python3
"""Store an AI provider key in the QEMU data disk's /etc/agent.conf (for the `agent` program).

usage: setkey.py KEYFILE [--endpoint URL] [--model NAME] [--img build/data.img]

KEYFILE holds just the key, or has a line like "api key: <key>". The key is never printed, and the
temporary copy written for mtools is deleted right away. build/ is git-ignored, so the key stays out
of the repository. (For Hyper-V use: hyperv.ps1 -ApiKeyFile KEYFILE.)
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keyfile")
    ap.add_argument("--endpoint", default="https://hyper.charm.land/v1")
    ap.add_argument("--model", default="glm-5.3-flash")
    ap.add_argument("--img", default=os.path.join(ROOT, "build", "data.img"))
    a = ap.parse_args()

    text = open(a.keyfile, encoding="utf-8", errors="replace").read()
    m = re.search(r"(?im)^\s*api[ _-]?key\s*[:=]\s*(\S+)", text)
    key = m.group(1) if m else (text.strip() if text.strip() and not re.search(r"\s", text.strip()) else None)
    if not key:
        sys.exit("setkey: no key found in the file (expected a line like 'api key: ...')")
    if not os.path.exists(a.img):
        sys.exit("setkey: %s does not exist; build first" % a.img)

    tmp = os.path.join(ROOT, "build", ".agent.conf.tmp")
    try:
        with open(tmp, "w", newline="\n") as f:
            f.write("# written by scripts/setkey.py\nendpoint=%s\nmodel=%s\napi_key=%s\n" % (a.endpoint, a.model, key))
        part = a.img + "@@1048576"
        # -D s: skip on a name clash (mtools would otherwise ask interactively)
        subprocess.run(["mmd", "-D", "s", "-i", part, "::/etc"], capture_output=True, stdin=subprocess.DEVNULL)
        subprocess.run(["mcopy", "-D", "o", "-i", part, tmp, "::/etc/agent.conf"], check=True, stdin=subprocess.DEVNULL)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    print("setkey: stored the key in /data/etc/agent.conf (endpoint %s, model %s)" % (a.endpoint, a.model))


if __name__ == "__main__":
    main()
