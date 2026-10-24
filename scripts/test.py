#!/usr/bin/env python3
"""Run Nocturne's test suite: boot a copy of the built disk headless in QEMU with the tests on its C:.

The built disk (build/nocturne.img, `make qemu-image`) is one GPT disk: the EFI system partition
(FAT: Limine and the kernel) and drive C:. The test copies it to build/test.img, checks that the
copy's kernel and system programs are the current build's, and puts tests/ and an autorun.sh in
C:\\Tests with build/host/store.exe. The copy is the VM's only disk, on AHCI and without a
snapshot. init runs C:\\Tests\\autorun.sh at boot: it compiles tests/runtests.c with the in-OS tcc
and runs it, then powers off. Results come back over the serial port. Afterwards the host checks
the volume the OS just wrote to with `store check`, and deletes the copy (it is kept only when the
check finds problems, for looking into them). For the TCP tests the host also runs a small server
the guest reaches at 10.0.2.2 (QEMU's user networking maps it to the host's 127.0.0.1); its port
is in C:\\Tests\\tcpport. The VM has an AC'97 sound card, recorded to build/test-audio.wav by
QEMU's wav backend; when the audio tests ran, the host looks for the tones tests/audiotest.c plays
in the recording.

usage: python scripts/test.py [--quick] [--full] [--no-net] [--timeout S] [group...]
  --boot-image IMG  copy and boot this disk instead of build/nocturne.img (its kernel and programs
                    must still be the current build's)
  --output-dir DIR  save into a new directory strictly under build/
  --quick   skip the slow tests (compile every app, tcc self-hosting)
  --full    also copy the TinyCC sources so tcc can rebuild itself inside the OS
  --no-net  skip the network tests (they need internet access from the host)
  group     run only these groups: sh tools mem fs tcc cxx gui audio media web tcp net agent
Exit status 0 when every test passed. Build first (make qemu-image). The built disk and your
build/run.img are never booted: the tests run on their own copy.
"""
import argparse
import glob
import hashlib
import importlib.util
import json
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from functools import partial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tools", "msys64", "ucrt64", "bin")
USR_BIN = os.path.join(ROOT, "tools", "msys64", "usr", "bin")
QEMU = os.path.join("C:/Program Files/qemu", "qemu-system-x86_64.exe")
if not os.path.isfile(QEMU):
    QEMU = os.path.join(BIN, "qemu-system-x86_64.exe")
BUILD = os.path.join(ROOT, "build")
CORE_BUILD = BUILD  # Generated toolchain inputs stay here when outputs are isolated.
STORE = os.path.join(CORE_BUILD, "host", "store.exe")
DISK = os.path.join(BUILD, "test.img")
SERIAL = os.path.join(BUILD, "test-serial.log")
AUDIO = os.path.join(BUILD, "test-audio.wav")


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def mtools(*args):
    env = dict(os.environ, PATH=BIN + os.pathsep + USR_BIN + os.pathsep + os.environ.get("PATH", ""), MTOOLS_SKIP_CHECK="1")
    # On Windows the child's env PATH does not determine CreateProcess executable lookup.
    # Resolve the bundled executable ourselves so an ordinary Python launch also works.
    executable = shutil.which(args[0], path=env["PATH"])
    if not executable:
        sys.exit("cannot find the bundled tool %s" % args[0])
    r = subprocess.run([executable, *args[1:]], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        sys.exit("%s failed: %s" % (args[0], r.stderr.strip()))


def store(*args, fatal=True):
    """build/host/store.exe (host/store/store.c) on a disk image; it changes only that image"""
    r = subprocess.run([STORE, *args], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode and fatal:
        sys.exit("store %s failed: %s" % (args[0], (r.stderr or r.stdout).strip()))
    return r


def mtools_name(path):
    # mtools takes the image relative to ROOT, with forward slashes ("D:" would read as its drive).
    return os.path.relpath(path, ROOT).replace("\\", "/")


def esp(disk):
    """mtools' name for the disk's EFI system partition, where `store esp` finds it in the GPT"""
    offset = store("esp", disk).stdout.split()[0]
    return mtools_name(disk) + "@@" + offset


def system_payload(disk, scratch):
    """sha256 of the disk's kernel (on its EFI system partition) and of each program scripts/layout.py
    put on its C: (build/c.list names them), each the same as the current build's; exits otherwise"""
    found = {}

    def compare(name, copy, built):
        actual = file_sha256(copy)
        os.remove(copy)
        if actual != file_sha256(built):
            sys.exit("the disk is not the current build's: %s differs (make qemu-image)" % name)
        found[name] = actual

    kernel = os.path.join(scratch, "kernel.elf")
    # -n: replace the host-side copy; -o alone still asks, and with no stdin mcopy then waits forever.
    mtools("mcopy", "-n", "-o", "-i", esp(disk), "::/boot/kernel.elf", mtools_name(kernel))
    compare("kernel.elf", kernel, os.path.join(CORE_BUILD, "kernel.elf"))
    programs = os.path.normcase(os.path.join(CORE_BUILD, "programs"))
    with open(os.path.join(CORE_BUILD, "c.list"), encoding="utf-8") as listing:
        for line in listing:
            fields = line.rstrip("\n").split("\t")
            if line.startswith("#") or len(fields) < 2:
                continue
            host = os.path.normpath(os.path.join(ROOT, fields[1]))
            if os.path.normcase(os.path.dirname(host)) != programs or not os.path.isfile(host):
                continue
            copy = os.path.join(scratch, "program")
            store("get", disk, fields[0], copy)
            compare(fields[0], copy, host)
    return found


def edit_boot_entry(disk, scratch, resolution=None, cmdline=""):
    """Change the first entry of the limine.conf on the disk's EFI system partition: its resolution,
    and words added to its kernel command line (a key=value word replaces the entry's own value for
    that key). The other entries stay as they are."""
    part = esp(disk)
    conf = os.path.join(scratch, "limine.conf")
    mtools("mcopy", "-n", "-o", "-i", part, "::/boot/limine/limine.conf", mtools_name(conf))
    with open(conf, encoding="utf-8") as f:
        config = f.read()
    first_entry = config.split("\n/", 2)[1]
    updated = first_entry
    if resolution:
        updated, count = re.subn(r"(?m)^    resolution: [^\n]+$", "    resolution: " + resolution, updated)
        if count != 1:
            sys.exit("the first boot entry must have exactly one resolution")
    if cmdline:
        def add(match):
            words = match.group(1).split()
            for word in cmdline.split():
                if "=" in word:
                    key = word.split("=", 1)[0] + "="
                    words = [w for w in words if not w.startswith(key)]
                if word not in words:
                    words.append(word)
            return "    cmdline: " + " ".join(words)
        updated, count = re.subn(r"(?m)^    cmdline: ([^\n]*)$", add, updated)
        if count == 0:
            updated, count = re.subn(r"(?m)^(    path: [^\n]*)$", lambda m: m.group(1) + "\n    cmdline: " + cmdline,
                                     updated)
        if count != 1:
            sys.exit("the first boot entry must have exactly one kernel and command line")
    with open(conf, "w", encoding="utf-8", newline="\n") as f:
        f.write(config.replace(first_entry, updated, 1))
    mtools("mcopy", "-o", "-i", part, mtools_name(conf), "::/boot/limine/limine.conf")
    os.remove(conf)


def put_tests(disk, entries, scratch):
    """Put (path in C:\\Tests, host file or folder) pairs on the disk's C: with one `store fill`.
    What C:\\Tests held before goes first, so it holds exactly these."""
    listing = os.path.join(scratch, "tests.list")
    with open(listing, "w", encoding="utf-8", newline="\n") as f:
        f.write("C:\\Tests\t!\nC:\\Tests\t-\n")
        for path, host in entries:
            if any(c in path + host for c in "\t\r\n"):
                sys.exit("a test file's name cannot be listed: %r" % host)
            f.write("%s\t%s\n" % (path, host))
    done = store("fill", disk, listing).stdout.strip()
    os.remove(listing)
    return done


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
            if op == "STALL":
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                self.wfile.write(b"READY\n")
                self.wfile.flush()
                time.sleep(5)
                return
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


class SilentTlsHandler(socketserver.BaseRequestHandler):
    """Consume ClientHello without replying; client cancellation owns shutdown."""
    def handle(self):
        self.request.settimeout(10)
        try:
            while self.request.recv(16384):
                pass
        except OSError:
            pass


def start_silent_tls_server():
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), SilentTlsHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def start_web_servers():
    """Two ephemeral host origins for the guest's actual webfetch tests; no internet is required."""
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


def start_h2_server():
    """Explicit transport regression fixture; never substitute for ALPN/TLS tests."""
    fixture = os.path.join(ROOT, "tests", "fixtures", "js", "h2_server.py")
    spec = importlib.util.spec_from_file_location("nocturne_h2_fixture", fixture)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    server = module.create_server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def start_dns_servers():
    fixture = os.path.join(ROOT, "tests", "fixtures", "dns_server.py")
    spec = importlib.util.spec_from_file_location("nocturne_dns_fixture", fixture)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    servers = module.create_servers(forward_external=True)
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    return servers


def test_files(a, tcp_port, web_ports, scratch):
    """what goes in C:\\Tests: (its path there, host file or folder)"""
    # A preserved source snapshot keeps native struct layouts and expectations
    # paired with the baseline boot image while the checkout is being repaired.
    source_root = a.source_tree or ROOT
    srcs = sorted(glob.glob(os.path.join(source_root, "tests", "*.c")))
    srcs += sorted(glob.glob(os.path.join(source_root, "tests", "*.cpp")))  # the clang group's C++
    srcs += sorted(glob.glob(os.path.join(source_root, "tests", "js_*_cases.js")))
    # Internal metadata regression reads diagnostic counters from the real DOM.
    srcs.append(os.path.join(source_root, "user", "libc", "web", "webi.h"))
    srcs.append(os.path.join(source_root, "user", "libc", "web", "cssom.h"))
    srcs.append(os.path.join(source_root, "user", "libc", "web", "js_canvas.h"))
    srcs.append(os.path.join(source_root, "third_party", "quickjs", "quickjs.h"))
    srcs.append(os.path.join(source_root, "user", "apps", "browser_events.h"))
    srcs += [os.path.join(source_root, "user", "libc", "web", name) for name in
             ("form_value.h", "form_file.h", "form_face.h", "form_validation.h", "elements.h", "frame.h", "html_lexbor.h", "html_policy.h", "html_pi.h", "sandbox.h")]
    srcs += sorted(glob.glob(os.path.join(source_root, "tests", "*.h")))
    cenc = os.path.join(CORE_BUILD, "tests", "media_cenc_test")
    if "cenc" in a.groups or "cenc/native-packet-owner" in a.groups:
        if a.source_tree or not os.path.isfile(cenc):
            sys.exit("test: build the matching native CENC test before selecting cenc")
        srcs.append(cenc)
    files = [(os.path.basename(src), src) for src in srcs]
    files.append(("media-fixtures", os.path.join(source_root, "tests", "media-fixtures")))
    if a.full:
        tcc = os.path.join(ROOT, "third_party", "tinycc")
        sources = sorted(glob.glob(os.path.join(tcc, "*.c")) + glob.glob(os.path.join(tcc, "*.h")) +
                         glob.glob(os.path.join(tcc, "*.def")))
        sources += [os.path.join(ROOT, "ports", "tcc", "config.h"), os.path.join(CORE_BUILD, "tcc", "tccdefs_.h")]
        files += [("tcc\\" + os.path.basename(src), src) for src in sources]

    def note(name, text):  # a small document made for this run
        path = os.path.join(scratch, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        files.append((name, path))

    note("tcpport", "%d\n" % tcp_port)
    note("webports", "%d %d\n" % tuple(web_ports))
    if getattr(a, "h2_port", None):
        note("h2port", "%d\n" % a.h2_port)
    if getattr(a, "tls_silent_port", None):
        note("tlssilentport", "%d\n" % a.tls_silent_port)
    if getattr(a, "dns_port", None):
        note("dnsport", "%d\n" % a.dns_port)
    args = (["-quick"] if a.quick else []) + (["-nonet"] if a.no_net else []) + a.groups
    note("autorun.sh", "tcc -o C:\\Tests\\runtests C:\\Tests\\runtests.c && C:\\Tests\\runtests %s\n" % " ".join(args))
    return [("C:\\Tests\\" + name, host) for name, host in files]


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

    # QEMU's wav backend records only while the card plays, and the sink stops the card while no
    # stream has sound (audio.h), so the silence between two tones may not be in the recording.
    # Split runs where the pitch changes as well (measured over 50 ms around each block).
    def pitch(i, r0, r1):
        a, b = max(r0, i - 2), min(r1, i + 3)
        x, y = left[a * blk:b * blk], right[a * blk:b * blk]
        return tuple(sum(1 for k in range(1, len(c)) if c[k - 1] < 0 <= c[k]) * rate / max(1, len(c)) for c in (x, y))

    def same(p, q):
        return all(abs(x - y) <= max(x, y) * 0.08 for x, y in zip(p, q))

    split = []
    for r0, r1 in runs:
        start = r0
        ref = pitch(min(r1 - 1, start + 3), r0, r1)
        for i in range(r0 + 6, r1 - 6):
            p, q = pitch(i, r0, r1), pitch(i + 4, r0, r1)  # a lasting change, not the blend at a seam
            if i - start >= 6 and not same(p, ref) and same(p, q):
                split.append([start, i])
                start, ref = i, q
        split.append([start, r1])
    runs = split

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


def prepare_disk(a, tcp_port, web_ports):
    """The run's own disk: a copy of the boot image, checked against the build, with its boot entry
    set for the run and the tests in C:\\Tests -> (sha256 of the boot image, of its payload)"""
    image_hash = a.boot_hash or file_sha256(a.boot_image)
    shutil.copyfile(a.boot_image, DISK)
    a.disk_made = True
    with tempfile.TemporaryDirectory(prefix="test-", dir=BUILD) as scratch:
        # The same bytes as a preserved baseline's boot image: the same payload.
        payload = a.baseline_payload or system_payload(DISK, scratch)
        dns_endpoint = "10.0.2.2:%d" % a.dns_port if getattr(a, "dns_port", None) else None
        if a.resolution or dns_endpoint:
            edit_boot_entry(DISK, scratch, a.resolution, "dns-server=" + dns_endpoint if dns_endpoint else "")
        print("test: C:\\Tests: " + put_tests(DISK, test_files(a, tcp_port, web_ports, scratch), scratch), flush=True)
    return image_hash, payload


def run_vm(a, tcp_port, web_ports, processes):
    image_hash, payload = prepare_disk(a, tcp_port, web_ports)
    dns_endpoint = "10.0.2.2:%d" % a.dns_port if getattr(a, "dns_port", None) else None
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    # The disk on AHCI, like a real machine's SATA; no snapshot: the volume is checked afterwards.
    cmd = [a.qemu, "-M", "pc", "-m", str(a.memory) + "M", "-smp", str(a.cpus), "-accel", a.accel,
           "-display", "egl-headless" if a.gpu == "virgl" else "none", "-vga", "std", "-no-reboot",
           "-serial", "file:" + SERIAL,
           "-device", "ahci,id=ahci", "-drive", "id=boot,file=%s,format=raw,if=none" % DISK,
           "-device", "ide-hd,drive=boot,bus=ahci.0,bootindex=0",
           "-audiodev", "wav,id=snd,path=%s,out.frequency=48000,out.buffer-length=20000,in.voices=0" % AUDIO, "-device", "AC97,audiodev=snd"]
    if a.gpu == "virgl":
        cmd += ["-device", "virtio-gpu-gl-pci"]
    if os.path.exists(AUDIO):
        os.remove(AUDIO)
    t0 = time.time()
    q = subprocess.Popen(cmd, cwd=ROOT, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    processes.append(q)
    metadata = {"pid": q.pid, "arguments": cmd, "boot_sha256": image_hash,
                "boot_source": a.boot_image, "boot_payload_sha256": payload, "user_data_attached": False,
                "disk": DISK, "test_source_tree": a.source_tree or ROOT,
                "baseline_metadata": a.baseline_metadata,
                "requested_resolution": a.resolution, "native_dns_fixture": dns_endpoint}
    metadata_path = os.path.join(BUILD, "test-metadata.json")
    with open(metadata_path, "w", encoding="utf-8") as f:
        json.dump(metadata, f, ensure_ascii=False, indent=2)

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

    log = open(SERIAL, "rb").read().decode("utf-8", "replace") if os.path.exists(SERIAL) else ""
    video = re.search(r"video: (\d+)x(\d+), (\d+) bpp", log)
    actual_resolution = "x".join(video.groups()) if video else None
    resolution_matched = not a.resolution or actual_resolution == a.resolution
    problems = []
    if not resolution_matched:
        problems.append("requested video resolution differs from the actual guest boot mode")
    if timed_out:
        problems.append("timed out after %d s (the VM did not power off)" % a.timeout)
    if q.returncode != 0:
        problems.append("QEMU exit status %s" % q.returncode)
    if "TESTS DONE" not in log:
        problems.append("the in-OS runner did not finish (did tcc fail to build it?)")
    for m in re.finditer(r"(?im)^.*(panic|kernel fault|double fault).*$", log):
        problems.append("kernel: " + m.group(0).strip())
    # Permission probes and the native-helper UD2 that each threadtest run (the
    # threads group's, and the clang group's build of it) confirms in its output
    # are deliberate. Do not suppress other threadtest faults, or more of them.
    expected_helper_faults = len(re.findall(r"^  \| threadtest: expected helper fault status=134\r?$", log, re.M))
    for m in re.finditer(r"^\[(segfault|crash)\] pid \d+ \(([^)]*)\).*$", log, re.M):
        if expected_helper_faults and re.fullmatch(
                r"\[crash\] pid \d+ \(threadtest\) cpu\d+: Invalid opcode at rip=[0-9a-fA-F]+"
                r" rsp=[0-9a-fA-F]+ stack(?: [0-9a-fA-F]+)*\r?", m.group(0)):
            expected_helper_faults -= 1
            continue
        if m.group(2) != "wxtest":
            problems.append(m.group(0))
    # The OS wrote to C: (the tests' own folders, C:\Tests\fstest, the temporary folder); a stop
    # that cut a commit short must leave it whole too.
    check = store("check", DISK, fatal=False)
    summary = check.stdout.strip().splitlines()
    print("%s store check: %s" % ("PASS" if check.returncode == 0 else "FAIL", summary[0] if summary else "?"))
    if check.returncode:
        problems += ["store check: " + l for l in (check.stdout + check.stderr).strip().splitlines()]
        a.keep_disk = True

    if re.search(r"^PASS audio/streams-beep-play", log, re.M):
        ok, msg = check_audio(AUDIO)
        print("%s audio/recording: %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            problems.append("audio recording: " + msg)

    metadata.update(exit_code=q.returncode, timed_out=timed_out, elapsed_seconds=time.time()-t0, process_stopped=True,
                    actual_resolution=actual_resolution, resolution_matched=resolution_matched,
                    store_check_passed=check.returncode == 0, disk_kept=a.keep_disk)
    with open(metadata_path, "w", encoding="utf-8") as f:
        json.dump(metadata, f, ensure_ascii=False, indent=2)
    fails = re.findall(r"^FAIL ", log, re.M)
    for p in problems:
        print("PROBLEM", p)
    ok = not fails and not problems
    print("test: %s in %d s (serial log: %s)" % ("all passed" if ok else "FAILED", time.time() - t0, SERIAL))
    if a.keep_disk:
        print("test: kept %s for the store check's problems; delete it when done with it" % DISK)
    return 0 if ok else 1


def main():
    global BUILD, DISK, SERIAL, AUDIO
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--no-net", action="store_true")
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--memory", type=int, default=2048, help="QEMU RAM in MiB (default: 2048)")
    ap.add_argument("--cpus", type=int, default=4, help="QEMU CPU count (default: 4)")
    ap.add_argument("--accel", choices=("tcg", "whpx"), default="tcg", help="No silent accelerator fallback")
    ap.add_argument("--resolution", help="Override only the copy's primary boot entry: widthxheightx32")
    ap.add_argument("--qemu", default=QEMU, help="QEMU executable (prefer the installed QEMU)")
    ap.add_argument("--gpu", choices=("none", "virgl"), default="none", help="Optional secondary virgl GPU; requires host GL")
    ap.add_argument("--boot-image", default=os.path.join(CORE_BUILD, "nocturne.img"), help="Disk to copy; its payload must match the current build")
    ap.add_argument("--output-dir", help="New, unused directory under build/; preserve earlier test evidence")
    ap.add_argument("--source-tree", help="Preserved test/header source tree for the matching baseline image")
    ap.add_argument("--baseline-metadata", help="Preserved run metadata; strictly verify its boot hash and payload for --source-tree")
    ap.add_argument("groups", nargs="*")
    a = ap.parse_args()
    if a.source_tree:
        a.source_tree = os.path.abspath(a.source_tree)
        if not os.path.isfile(os.path.join(a.source_tree, "tests", "runtests.c")):
            ap.error("--source-tree must contain tests/runtests.c")
    if a.resolution and not re.fullmatch(r"[1-9][0-9]{0,4}x[1-9][0-9]{0,4}x32", a.resolution):
        ap.error("--resolution requires positive widthxheightx32")
    if a.memory < 256:
        ap.error("--memory must be at least 256 MiB")
    if not 1 <= a.cpus <= 32:
        ap.error("--cpus must be between 1 and 32")
    if not os.path.isfile(a.qemu):
        ap.error("--qemu executable does not exist")
    a.boot_image = os.path.abspath(a.boot_image)
    if not os.path.isfile(a.boot_image) or not os.path.isfile(STORE):
        sys.exit("test: build first (make qemu-image)")
    a.boot_hash = a.baseline_payload = None
    a.disk_made = a.keep_disk = False
    if a.baseline_metadata:
        if not a.source_tree:
            ap.error("--baseline-metadata requires the preserved --source-tree")
        a.baseline_metadata = os.path.abspath(a.baseline_metadata)
        with open(a.baseline_metadata, encoding="utf-8") as f:
            baseline = json.load(f)
        payload = baseline.get("boot_payload_sha256", {})
        valid_hash = lambda value: isinstance(value, str) and bool(re.fullmatch("[0-9a-f]{64}", value))
        if (baseline.get("process_stopped") is not True or baseline.get("user_data_attached") is not False or
                not valid_hash(baseline.get("boot_sha256")) or not isinstance(payload, dict) or
                not valid_hash(payload.get("kernel.elf")) or any(not valid_hash(value) for value in payload.values())):
            ap.error("baseline metadata must identify a stopped isolated run, its boot image and its payload hashes")
        a.boot_hash = file_sha256(a.boot_image)
        if a.boot_hash != baseline["boot_sha256"]:
            ap.error("baseline boot image does not match the preserved run hash")
        a.baseline_payload = payload
    if a.output_dir:
        output = os.path.realpath(a.output_dir)
        core = os.path.realpath(CORE_BUILD)
        try:
            inside = os.path.normcase(os.path.commonpath((core, output))) == os.path.normcase(core)
        except ValueError:
            inside = False
        if not inside or os.path.normcase(output) == os.path.normcase(core) or os.path.exists(output):
            ap.error("--output-dir must be a new directory strictly under build/")
        os.makedirs(output, exist_ok=False)
        BUILD = output
        DISK, SERIAL, AUDIO = [os.path.join(BUILD, name) for name in ("test.img", "test-serial.log", "test-audio.wav")]
    overwritten = (DISK, SERIAL, AUDIO, os.path.join(BUILD, "test-metadata.json"))
    if os.path.normcase(os.path.realpath(a.boot_image)) in {os.path.normcase(os.path.realpath(p)) for p in overwritten}:
        ap.error("--boot-image must not be a test output path")
    servers, processes = [], []
    try:
        tcp = start_tcp_server()
        servers.append(tcp)
        web = start_web_servers()
        servers.extend(web)
        if not a.groups or any(group == "transport8" or group.startswith("transport8/") for group in a.groups):
            h2 = start_h2_server()
            servers.append(h2)
            a.h2_port = h2.server_address[1]
            silent_tls = start_silent_tls_server()
            servers.append(silent_tls)
            a.tls_silent_port = silent_tls.server_address[1]
        if not a.groups or any(group == "nettransport" or group.startswith("nettransport/") for group in a.groups):
            dns = start_dns_servers()
            servers.extend(dns)
            a.dns_port = dns[0].server_address[1]
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
            try:
                for server in servers:
                    try:
                        server.shutdown()
                    finally:
                        server.server_close()
            finally:
                # The run's copy of the disk does not pile up (QEMU has stopped by now).
                if a.disk_made and not a.keep_disk and os.path.exists(DISK):
                    os.remove(DISK)


if __name__ == "__main__":
    sys.exit(main())
