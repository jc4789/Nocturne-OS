#!/usr/bin/env python3
"""Run Nocturne's test suite: boot the built image headless in QEMU with a fresh test data disk.

The disk carries tests/ and an autorun.sh. init runs that script at boot. It compiles
tests/runtests.c with the in-OS tcc and runs it, then powers off. Results come back over the
serial port. Afterwards the host checks the data disk's FAT with fatcheck.py, since the OS just
wrote to it. For the TCP tests the host also runs a small server the guest reaches at 10.0.2.2
(QEMU's user networking maps it to the host's 127.0.0.1); its port is in /data/tests/tcpport.
The VM has an AC'97 sound card, recorded to build/test-audio.wav by QEMU's wav backend; when the
audio tests ran, the host looks for the tones tests/audiotest.c plays in the recording.

usage: python scripts/test.py [--quick] [--full] [--no-net] [--timeout S] [group...]
  --quick   skip the slow tests (compile every app, tcc self-hosting)
  --full    also copy the TinyCC sources so tcc can rebuild itself inside the OS
  --no-net  skip the network tests (they need internet access from the host)
  group     run only these groups: sh tools mem fs tcc gui audio web tcp net agent
Exit status 0 when every test passed. Build first (build.ps1). Your own build/data.img is not
touched: the tests use build/test-data.img.
"""
import argparse
import glob
import importlib.util
import os
import re
import shutil
import socketserver
import subprocess
import sys
import threading
import time
from functools import partial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tools", "msys64", "ucrt64", "bin")
USR_BIN = os.path.join(ROOT, "tools", "msys64", "usr", "bin")
BASH = os.path.join(ROOT, "tools", "msys64", "usr", "bin", "bash.exe")
QEMU = os.path.join(BIN, "qemu-system-x86_64.exe")
BUILD = os.path.join(ROOT, "build")
IMG = os.path.join(BUILD, "test-data.img")
SERIAL = os.path.join(BUILD, "test-serial.log")
AUDIO = os.path.join(BUILD, "test-audio.wav")
PART = IMG + "@@1048576"


def mtools(*args):
    env = dict(os.environ, PATH=BIN + os.pathsep + USR_BIN + os.pathsep + os.environ.get("PATH", ""), MTOOLS_SKIP_CHECK="1")
    # On Windows the child's env PATH does not determine CreateProcess executable lookup.
    # Resolve the bundled executable ourselves so an ordinary Python launch also works.
    executable = shutil.which(args[0], path=env["PATH"])
    if not executable:
        sys.exit("test: cannot find bundled tool %s" % args[0])
    r = subprocess.run([executable, *args[1:]], env=env, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        sys.exit("test: %s failed: %s" % (args[0], r.stderr.strip()))


PATTERNS = {}  # (n, seed) -> bytes, precomputed for tests/tcptest.c's transfers while the VM boots
TCPTEST_TRANSFERS = [(4 << 20, 1), (4 << 20, 2), (2 << 20, 3), (2 << 20, 4), (1 << 20, 5), (1 << 20, 6)]


def pattern(n, seed):
    """the byte stream tests/tcptest.c expects: an LCG's bits 16..23"""
    if (n, seed) in PATTERNS:
        return PATTERNS[(n, seed)]
    out = bytearray(n)
    x = seed
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 255
    return out


class TcpTestHandler(socketserver.StreamRequestHandler):
    """GET n seed -> n pattern bytes; PUT n seed + n bytes -> OK n / BAD offset"""

    def handle(self):
        try:
            op, n, seed = self.rfile.readline().decode().split()
            n, seed = int(n), int(seed)
            want = pattern(n, seed)
            if op == "GET":
                self.wfile.write(want)
            elif op == "PUT":
                got = self.rfile.read(n)
                if got == want:
                    self.wfile.write(b"OK %d\n" % n)
                else:
                    bad = next((i for i in range(min(len(got), n)) if got[i] != want[i]), len(got))
                    self.wfile.write(b"BAD %d\n" % bad)
        except (OSError, ValueError):
            pass


def start_tcp_server():
    socketserver.ThreadingTCPServer.daemon_threads = True
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), TcpTestHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    def precompute():
        for n, seed in TCPTEST_TRANSFERS:
            PATTERNS[(n, seed)] = pattern(n, seed)
    threading.Thread(target=precompute, daemon=True).start()
    return srv


def start_web_servers():
    """Two ephemeral host origins for actual /bin/webfetch tests; no internet is required."""
    fixture = os.path.join(ROOT, "tests", "fixtures", "js", "server.py")
    spec = importlib.util.spec_from_file_location("nocturne_web_fixture", fixture)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    root = os.path.dirname(fixture)
    class QuietHandler(module.Handler):
        def log_message(self, format, *args):
            pass
    servers = []
    try:
        for _ in range(2):
            server = module.ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=root))
            server.daemon_threads = True
            try:
                threading.Thread(target=server.serve_forever, daemon=True).start()
            except BaseException:
                # shutdown() requires a serving thread; this one never started.
                server.server_close()
                raise
            servers.append(server)
        return servers
    except BaseException:
        for server in servers:
            server.shutdown()
            server.server_close()
        raise


def make_disk(a, tcp_port, web_ports):
    if os.path.exists(IMG):
        os.remove(IMG)
    env = dict(os.environ, PATH=BIN + os.pathsep + USR_BIN + os.pathsep + os.environ.get("PATH", ""))
    subprocess.run([BASH, "scripts/mkdata.sh", "build/test-data.img", "256"], cwd=ROOT, env=env, check=True,
                   stdout=subprocess.DEVNULL)
    mtools("mmd", "-i", PART, "::/tests")
    srcs = sorted(glob.glob(os.path.join(ROOT, "tests", "*.c")))
    srcs += sorted(glob.glob(os.path.join(ROOT, "tests", "js_*_cases.js")))
    # Internal metadata regression reads diagnostic counters from the real DOM.
    srcs.append(os.path.join(ROOT, "user", "libc", "web", "webi.h"))
    srcs.append(os.path.join(ROOT, "third_party", "quickjs", "quickjs.h"))
    srcs += [os.path.join(ROOT, "user", "libc", "web", name) for name in
             ("form_value.h", "form_validation.h", "elements.h")]
    srcs.append(os.path.join(ROOT, "tests", "web_form_validation_cases.h"))
    mtools("mcopy", "-i", PART, *srcs, "::/tests/")
    if a.full:
        tcc = os.path.join(ROOT, "third_party", "tinycc")
        files = sorted(glob.glob(os.path.join(tcc, "*.c")) + glob.glob(os.path.join(tcc, "*.h")) +
                       glob.glob(os.path.join(tcc, "*.def")))
        files += [os.path.join(ROOT, "ports", "tcc", "config.h"), os.path.join(BUILD, "tcc", "tccdefs_.h")]
        mtools("mmd", "-i", PART, "::/tests/tcc")
        mtools("mcopy", "-i", PART, *files, "::/tests/tcc/")
    port_file = os.path.join(BUILD, "tcpport")
    with open(port_file, "w", newline="\n") as f:
        f.write("%d\n" % tcp_port)
    mtools("mcopy", "-i", PART, port_file, "::/tests/tcpport")
    os.remove(port_file)
    web_file = os.path.join(BUILD, "webports")
    with open(web_file, "w", encoding="utf-8", newline="\n") as f:
        f.write("%d %d\n" % tuple(web_ports))
    mtools("mcopy", "-i", PART, web_file, "::/tests/webports")
    os.remove(web_file)
    args = (["-quick"] if a.quick else []) + (["-nonet"] if a.no_net else []) + a.groups
    script = os.path.join(BUILD, "autorun.sh")
    with open(script, "w", newline="\n") as f:
        f.write("tcc -o /home/runtests /data/tests/runtests.c && /home/runtests %s\n" % " ".join(args))
    mtools("mcopy", "-i", PART, script, "::/tests/autorun.sh")
    os.remove(script)


# what tests/audiotest.c plays, in order: (left Hz, right Hz, seconds); the last one is killed early
AUDIO_TONES = [(1000, 1500, 1.0), (600, 600, 0.5), (700, 900, 0.5), (800, 800, 0.5), (500, 1100, 0.5),
               (440, 440, 0.4), (1200, 750, 0.6), (300, 300, None)]


def check_audio(path):
    """find the test tones in the card's recording -> (ok, message)"""
    if not os.path.exists(path):
        return False, "no recording"
    d = open(path, "rb").read()
    at = d.find(b"data")
    if d[:4] != b"RIFF" or at < 0:
        return False, "the recording is not a WAV file"
    rate = int.from_bytes(d[24:28], "little")
    pcm = memoryview(d[at + 8:at + 8 + (len(d) - at - 8) // 4 * 4]).cast("h")
    left, right = pcm[0::2], pcm[1::2]
    # 10 ms blocks with sound in them, joined into runs across gaps under 60 ms
    blk = rate // 100
    loud = [max(map(abs, left[i:i + blk]), default=0) > 1000 or max(map(abs, right[i:i + blk]), default=0) > 1000
            for i in range(0, len(left), blk)]
    runs = []
    for i, on in enumerate(loud):
        if not on:
            continue
        if runs and i - runs[-1][1] <= 6:
            runs[-1][1] = i + 1
        else:
            runs.append([i, i + 1])

    def hz(ch, a, b):  # rising zero crossings per second over the middle of the run
        a, b = a + (b - a) // 10, b - (b - a) // 10
        x = ch[a:b]
        n = sum(1 for i in range(1, len(x)) if x[i - 1] < 0 <= x[i])
        return n * rate / max(1, len(x))

    found = []
    for r0, r1 in runs:
        a, b = r0 * blk, r1 * blk
        found.append((round(hz(left, a, b)), round(hz(right, a, b)), (b - a) / rate))
    desc = ", ".join("%d/%d Hz %.2f s" % f for f in found)
    if len(found) != len(AUDIO_TONES):
        return False, "%d sounds instead of %d: %s" % (len(found), len(AUDIO_TONES), desc)
    for (fl, fr, secs), (gl, gr, gs) in zip(AUDIO_TONES, found):
        if abs(gl - fl) > fl * 0.02 or abs(gr - fr) > fr * 0.02:
            return False, "expected %d/%d Hz, heard %d/%d Hz: %s" % (fl, fr, gl, gr, desc)
        if secs and abs(gs - secs) > 0.08 or not secs and gs > 0.7:
            return False, "a %d Hz tone lasted %.2f s: %s" % (fl, gs, desc)
    return True, desc


def run_vm(a, tcp_port, web_ports, processes):
    make_disk(a, tcp_port, web_ports)
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    cmd = [QEMU, "-M", "pc", "-m", str(a.memory) + "M", "-display", "none", "-vga", "std", "-no-reboot",
           "-serial", "file:" + SERIAL,
           "-drive", "file=%s,format=raw,if=ide,index=0,snapshot=on" % os.path.join(BUILD, "nocturne.img"),
           "-drive", "file=%s,format=raw,if=ide,index=1" % IMG,
           "-audiodev", "wav,id=snd,path=%s,out.frequency=48000,out.buffer-length=20000,in.voices=0" % AUDIO, "-device", "AC97,audiodev=snd"]
    if os.path.exists(AUDIO):
        os.remove(AUDIO)
    t0 = time.time()
    q = subprocess.Popen(cmd, cwd=ROOT, stderr=subprocess.PIPE, text=True, errors="replace")
    processes.append(q)

    def qemu_stderr():  # the AC'97's recording inputs have nowhere to record from: not news
        for line in q.stderr:
            if "Can not open `ac97." not in line:
                sys.stderr.write(line)
    threading.Thread(target=qemu_stderr, daemon=True).start()
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

    if re.search(r"^PASS audio/streams-beep-play", log, re.M):
        ok, msg = check_audio(AUDIO)
        print("%s audio/recording: %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            problems.append("audio recording: " + msg)

    fails = re.findall(r"^FAIL ", log, re.M)
    for p in problems:
        print("PROBLEM", p)
    ok = not fails and not problems
    print("test: %s in %d s (serial log: build/test-serial.log)" % ("all passed" if ok else "FAILED", time.time() - t0))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--no-net", action="store_true")
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--memory", type=int, default=2048, help="QEMU RAM in MiB (default: 2048)")
    ap.add_argument("groups", nargs="*")
    a = ap.parse_args()
    if a.memory < 256:
        ap.error("--memory must be at least 256 MiB")
    if not os.path.exists(os.path.join(BUILD, "nocturne.img")):
        sys.exit("test: build first with the bundled toolchain")
    servers, processes = [], []
    try:
        tcp = start_tcp_server()
        servers.append(tcp)
        web = start_web_servers()
        servers.extend(web)
        return run_vm(a, tcp.server_address[1], [s.server_address[1] for s in web], processes)
    finally:
        # Only processes and servers created by this test run are touched.
        try:
            for process in processes:
                if process.poll() is None:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    process.wait()
        finally:
            for server in servers:
                try:
                    server.shutdown()
                finally:
                    server.server_close()


if __name__ == "__main__":
    sys.exit(main())
