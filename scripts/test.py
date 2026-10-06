#!/usr/bin/env python3
"""Run Nocturne's test suite: boot the built image headless in QEMU with a fresh test data disk.

The disk carries tests/ and an autorun.sh. init runs that script at boot. It compiles
tests/runtests.c with the in-OS tcc and runs it, then powers off. Results come back over the
serial port. Afterwards the host checks the data disk's FAT with fatcheck.py, since the OS just
wrote to it.

usage: python scripts/test.py [--quick] [--full] [--no-net] [--timeout S] [group...]
  --quick   skip the slow tests (compile every app, tcc self-hosting)
  --full    also copy the TinyCC sources so tcc can rebuild itself inside the OS
  --no-net  skip the network tests (they need internet access from the host)
  group     run only these groups: sh tools mem fs tcc gui net agent
Exit status 0 when every test passed. Build first (build.ps1). Your own build/data.img is not
touched: the tests use build/test-data.img.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tools", "msys64", "ucrt64", "bin")
BASH = os.path.join(ROOT, "tools", "msys64", "usr", "bin", "bash.exe")
QEMU = os.path.join(BIN, "qemu-system-x86_64.exe")
BUILD = os.path.join(ROOT, "build")
IMG = os.path.join(BUILD, "test-data.img")
SERIAL = os.path.join(BUILD, "test-serial.log")
PART = IMG + "@@1048576"


def mtools(*args):
    env = dict(os.environ, PATH=BIN + os.pathsep + os.environ.get("PATH", ""), MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(list(args), env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode:
        sys.exit("test: %s failed: %s" % (args[0], r.stderr.strip()))


def make_disk(a):
    if os.path.exists(IMG):
        os.remove(IMG)
    env = dict(os.environ, PATH=BIN + os.pathsep + os.environ.get("PATH", ""))
    subprocess.run([BASH, "scripts/mkdata.sh", "build/test-data.img", "256"], cwd=ROOT, env=env, check=True,
                   stdout=subprocess.DEVNULL)
    mtools("mmd", "-i", PART, "::/tests")
    srcs = sorted(glob.glob(os.path.join(ROOT, "tests", "*.c")))
    mtools("mcopy", "-i", PART, *srcs, "::/tests/")
    if a.full:
        tcc = os.path.join(ROOT, "third_party", "tinycc")
        files = sorted(glob.glob(os.path.join(tcc, "*.c")) + glob.glob(os.path.join(tcc, "*.h")) +
                       glob.glob(os.path.join(tcc, "*.def")))
        files += [os.path.join(ROOT, "ports", "tcc", "config.h"), os.path.join(BUILD, "tcc", "tccdefs_.h")]
        mtools("mmd", "-i", PART, "::/tests/tcc")
        mtools("mcopy", "-i", PART, *files, "::/tests/tcc/")
    args = (["-quick"] if a.quick else []) + (["-nonet"] if a.no_net else []) + a.groups
    script = os.path.join(BUILD, "autorun.sh")
    with open(script, "w", newline="\n") as f:
        f.write("tcc -o /home/runtests /data/tests/runtests.c && /home/runtests %s\n" % " ".join(args))
    mtools("mcopy", "-i", PART, script, "::/tests/autorun.sh")
    os.remove(script)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--no-net", action="store_true")
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("groups", nargs="*")
    a = ap.parse_args()
    if not os.path.exists(os.path.join(BUILD, "nocturne.img")):
        sys.exit("test: build first (powershell -ExecutionPolicy Bypass -File build.ps1)")

    make_disk(a)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    cmd = [QEMU, "-M", "pc", "-m", "512M", "-display", "none", "-vga", "std", "-no-reboot",
           "-serial", "file:" + SERIAL,
           "-drive", "file=%s,format=raw,if=ide,index=0,snapshot=on" % os.path.join(BUILD, "nocturne.img"),
           "-drive", "file=%s,format=raw,if=ide,index=1" % IMG]
    t0 = time.time()
    q = subprocess.Popen(cmd, cwd=ROOT)
    seen = 0
    timed_out = False
    while True:
        done = q.poll() is not None
        if os.path.exists(SERIAL):
            with open(SERIAL, "rb") as f:
                lines = f.read().decode("utf-8", "replace").splitlines()
            # print results as they arrive (the last line may still be incomplete)
            upto = len(lines) if done else max(seen, len(lines) - 1)
            for line in lines[seen:upto]:
                if re.match(r"(PASS|FAIL|SKIP|TESTS) ", line):
                    print(line, flush=True)
            seen = upto
        if done:
            break
        if time.time() - t0 > a.timeout:
            timed_out = True
            q.kill()
            q.wait()
            break
        time.sleep(0.5)

    log = open(SERIAL, "rb").read().decode("utf-8", "replace")
    problems = []
    if timed_out:
        problems.append("timed out after %d s (the VM did not power off)" % a.timeout)
    if "TESTS DONE" not in log:
        problems.append("the in-OS runner did not finish (did tcc fail to build it?)")
    for m in re.finditer(r"(?im)^.*(panic|kernel fault|double fault).*$", log):
        problems.append("kernel: " + m.group(0).strip())
    # wxtest crashes its probe children on purpose; any other user crash is a bug
    for m in re.finditer(r"^\[(segfault|crash)\] pid \d+ \(([^)]*)\).*$", log, re.M):
        if m.group(2) != "wxtest":
            problems.append(m.group(0))
    fc = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "fatcheck.py"), IMG],
                        capture_output=True, text=True)
    fc_out = (fc.stdout + fc.stderr).strip().splitlines()
    print("%s fatcheck: %s" % ("PASS" if fc.returncode == 0 else "FAIL", fc_out[0] if fc_out else "?"))
    if fc.returncode:
        problems += ["fatcheck: " + l for l in fc_out]

    fails = re.findall(r"^FAIL ", log, re.M)
    for p in problems:
        print("PROBLEM", p)
    ok = not fails and not problems
    print("test: %s in %d s (serial log: build/test-serial.log)" % ("all passed" if ok else "FAILED", time.time() - t0))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
