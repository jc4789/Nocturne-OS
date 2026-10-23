#!/usr/bin/env python3
"""Store an AI provider key for the `agent` program in a boot disk: the user's
C:\\Users\\<name>\\AppData\\Agent\\agent.conf, where the agent reads it.

usage: setkey.py KEYFILE [--endpoint URL] [--model NAME] [--img build/run.img]

KEYFILE holds just the key, or has a line like "api key: <key>". The disk is a raw one: `make run`'s
build/run.img by default (its C:\\Users stays from one run to the next). The key is never printed,
and the temporary copy written for the store tool is deleted right away. build/ is git-ignored, so
the key stays out of the repository. (For Hyper-V use: hyperv.ps1 -ApiKeyFile KEYFILE.)
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(ROOT, "build", "host", "store.exe")


def store(*args):
    r = subprocess.run([STORE, *args], capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if r.returncode:
        sys.exit("setkey: store %s: %s" % (args[0], r.stderr.strip()))
    return r.stdout


def user_folder(img):
    """The user's folder, as the kernel picks it: the first folder in C:\\Users the user can reach."""
    for line in store("list", img, "C:\\Users").splitlines():
        kind, _size, attributes, name = line.split(None, 3)
        if kind == "folder" and "p" not in attributes:
            return "C:\\Users\\" + name
    sys.exit("setkey: %s has no user folder in C:\\Users" % img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("keyfile")
    ap.add_argument("--endpoint", default="https://hyper.charm.land/v1")
    ap.add_argument("--model", default="glm-5.3-flash")
    ap.add_argument("--img", default=os.path.join(ROOT, "build", "run.img"))
    a = ap.parse_args()

    text = open(a.keyfile, encoding="utf-8", errors="replace").read()
    m = re.search(r"(?im)^\s*api[ _-]?key\s*[:=]\s*(\S+)", text)
    key = m.group(1) if m else (text.strip() if text.strip() and not re.search(r"\s", text.strip()) else None)
    if not key:
        sys.exit("setkey: no key found in the file (expected a line like 'api key: ...')")
    if not os.path.exists(a.img):
        sys.exit("setkey: %s does not exist; build first (make run makes build/run.img)" % a.img)
    if not os.path.exists(STORE):
        sys.exit("setkey: %s does not exist; build first" % STORE)

    agent = user_folder(a.img) + "\\AppData\\Agent"
    conf = os.path.join(ROOT, "build", ".agent.conf.tmp")
    fill = os.path.join(ROOT, "build", ".agent.fill.tmp")
    try:
        with open(conf, "w", newline="\n") as f:
            f.write("# written by scripts/setkey.py\nendpoint=%s\nmodel=%s\napi_key=%s\n" % (a.endpoint, a.model, key))
        # one fill: the folders (kept when they are there) and the document, in one commit
        with open(fill, "w", encoding="utf-8", newline="\n") as f:
            f.write("%s\t-\t\n%s\t-\t\n%s\\agent.conf\t%s\t\n" % (agent.rsplit("\\", 1)[0], agent, agent, conf))
        store("fill", a.img, fill)
    finally:
        for tmp in (conf, fill):
            if os.path.exists(tmp):
                os.remove(tmp)
    print("setkey: stored the key in %s\\agent.conf (endpoint %s, model %s)" % (agent, a.endpoint, a.model))


if __name__ == "__main__":
    main()
