#!/usr/bin/env python3
"""Run web-platform-tests in Nocturne's browser and keep the results.

The host serves a WPT checkout with WPT's own server (tools/serve on 127.0.0.1, wptrunner's ports:
http 8000/8001, https 8443/8444, h2 9000, ws 8888, wss 8889 ...) and answers the guest's DNS for
the WPT host names (web-platform.test, its www/www1/... subdomains, not-web-platform.test) with
10.0.2.2, QEMU's address for the host; every other name is NXDOMAIN. A copy of build/nocturne.img
boots headless in QEMU with the boot setting dns-server=10.0.2.2:<port> and tests/wptrunner.c in
C:\\Tests, which init runs. The guest runner keeps one browser per slot as this script says over
one TCP connection. Each browser goes from test to test by itself: tests/wpt/testharnessreport.js
(served in place of WPT's) posts a top-level test document's results to /_nocturne/harness.py,
which hands them to this script and returns the slot's next test. A slot whose test does not
report in time, or whose browser ends, gets a new browser, and its test is recorded as
EXTERNAL-TIMEOUT or CRASH.

testharness.js and test262 tests only so far (--type): no reftests. The testdriver backend is
tests/wpt/testdriver-vendor.js, served in place of WPT's empty one: the guest runner starts each browser with
--wpt-automation, a test tool that lets test_driver.click, send_keys and action_sequence put their input into the
browser's own input queue; every other test_driver call fails at once as unimplemented. For the https and h2 tests
the test disk's system trusts WPT's certificate authority (tools/certs/cacert.pem, put in
C:\\Nocturne\\Certificates); --no-https leaves them out.

Results go to build/wpt/<label>/: results.jsonl (one test per line; --resume skips the tests
already there), summary.txt and summary.json (per top-level directory: tests whose harness and
subtests all passed, and subtests passed of those reported), serial.log, wptserve.log.

usage: python -X utf8 scripts/wpt.py [--label L] [--slots N] [--resume] [--limit N] [path ...]
  path   run only the tests under these WPT paths (dom/, fetch/api/basic/, ...); default all
"""
import argparse
import collections
import contextlib
import importlib.util
import json
import logging
import multiprocessing
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
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(ROOT, "tests", "wpt")
DEFAULT_WPT = r"D:\Programes\cloned repos\wpt"
GUEST_HOST = "10.0.2.2"  # the host as QEMU's user networking shows it to the guest
BROWSER_HOST = "web-platform.test"
PORTS = {"http": [8000, 8001], "http-local": [8002], "http-public": [8003], "https": [8443, 8444],
         "https-local": [8445], "https-public": [8446], "ws": [8888], "wss": [8889], "h2": [9000],
         "webtransport-h3": [11000], "dns": [8053]}  # the last two are named for substitutions, not started
HARNESS_STATUS = {0: "OK", 1: "ERROR", 2: "TIMEOUT", 3: "PRECONDITION_FAILED"}
SUBTEST_STATUS = {0: "PASS", 1: "FAIL", 2: "TIMEOUT", 3: "NOTRUN", 4: "PRECONDITION_FAILED"}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Test:
    __slots__ = ("url", "protocol", "subdomain", "long")

    def __init__(self, url, protocol, subdomain, long):
        self.url, self.protocol, self.subdomain, self.long = url, protocol, subdomain, long

    def full_url(self):
        scheme = "https" if self.protocol == "h2" else self.protocol
        host = ("www." if self.subdomain else "") + BROWSER_HOST
        return "%s://%s:%d%s" % (scheme, host, PORTS[self.protocol][0], self.url)


def load_tests(wpt, manifest_path, paths, https, update, kinds):
    from tools.manifest import manifest
    if update or not os.path.exists(manifest_path):
        print("wpt: updating %s" % manifest_path, flush=True)
        m = manifest.load_and_update(wpt, manifest_path, "/", parallel=True)
    else:
        m = manifest.load(wpt, manifest_path)
    tests = []
    for _kind, _path, items in m.itertypes(*kinds):
        for item in items:
            if getattr(item, "jsshell", False):
                continue
            if paths and not any(item.url.startswith(p) for p in paths):
                continue
            protocol = "h2" if item.h2 else "https" if item.https else "http"
            if protocol != "http" and not https:
                continue
            tests.append(Test(item.url, protocol, item.subdomain, item.timeout == "long"))
    tests.sort(key=lambda t: t.url)
    return tests


class DnsHandlerMixin:
    def answer(self, packet):
        name, question = self.server.parse(packet)
        flags, rcode, answers = 0x8180, 0, b""
        known = name in self.server.names
        qtype = int.from_bytes(question[-4:-2], "big")
        if not known:
            rcode = 3  # NXDOMAIN
        elif qtype in (1, 255):  # A, ANY
            answers = b"\xc0\x0c" + (1).to_bytes(2, "big") + (1).to_bytes(2, "big") + (3600).to_bytes(4, "big") + \
                (4).to_bytes(2, "big") + socket.inet_aton(GUEST_HOST)
        count = 1 if answers else 0
        header = packet[:2] + (flags | rcode).to_bytes(2, "big") + (1).to_bytes(2, "big") + \
            count.to_bytes(2, "big") + b"\0\0\0\0"
        return header + question + answers


class DnsUdpHandler(DnsHandlerMixin, socketserver.BaseRequestHandler):
    def handle(self):
        packet, sock = self.request
        try:
            sock.sendto(self.answer(packet), self.client_address)
        except ValueError:
            pass


class DnsTcpHandler(DnsHandlerMixin, socketserver.BaseRequestHandler):
    def handle(self):
        data = b""
        while True:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            data += chunk
            while len(data) >= 2 and len(data) >= 2 + int.from_bytes(data[:2], "big"):
                size = int.from_bytes(data[:2], "big")
                packet, data = data[2:2 + size], data[2 + size:]
                try:
                    reply = self.answer(packet)
                except ValueError:
                    return
                self.request.sendall(len(reply).to_bytes(2, "big") + reply)


def start_dns(names):
    """UDP and TCP on one port of 127.0.0.1: the WPT names are 10.0.2.2, the rest NXDOMAIN"""
    parse = load_module("nocturne_dns_fixture", os.path.join(ROOT, "tests", "fixtures", "dns_server.py")).question
    tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), DnsTcpHandler)
    udp = socketserver.ThreadingUDPServer(tcp.server_address, DnsUdpHandler)
    for server in (tcp, udp):
        server.daemon_threads = True
        server.names = {n.lower().rstrip(".") for n in names}
        server.parse = parse
        threading.Thread(target=server.serve_forever, daemon=True).start()
    return tcp.server_address[1], (tcp, udp)


class Slot:
    def __init__(self, index):
        self.index = index
        self.test = None        # the Test in flight
        self.started = 0.0
        self.deadline = 0.0
        self.count = 0          # tests since this browser started
        self.opened = 0.0       # when its browser was (re)started; 0 once it asked for a test
        self.failures = 0       # browsers in a row that never asked for a test


class Run:
    def __init__(self, a, tests, out):
        self.a = a
        self.lock = threading.Lock()
        self.pending = collections.deque(tests)
        self.total = len(tests)
        self.done = 0
        self.slots = [Slot(i) for i in range(a.slots)]
        self.results = open(os.path.join(out, "results.jsonl"), "a", encoding="utf-8")
        self.stray = open(os.path.join(out, "stray.jsonl"), "a", encoding="utf-8")
        self.guest = None
        self.guest_lock = threading.Lock()
        self.hello = threading.Event()
        self.finished = threading.Event()
        self.stuck = False      # stopped because browsers never asked for tests: the guest is examined
        self.t0 = time.time()

    # -- the guest runner's connection
    def send(self, line):
        with self.guest_lock:
            if self.guest:
                try:
                    self.guest.sendall((line + "\n").encode("utf-8"))
                except OSError:
                    pass

    def start_url(self, slot):
        return "http://%s:%d/_nocturne/harness.py?slot=%d" % (BROWSER_HOST, PORTS["http"][0], slot.index)

    def restart(self, slot):  # with the lock held
        slot.test = None
        slot.count = 0
        slot.opened = time.time()
        if not self.pending:
            self.send("close %d" % slot.index)
            slot.opened = 0.0
            self.check_finished()
            return
        self.send("open %d %s" % (slot.index, " ".join(self.a.browser_option + [self.start_url(slot)])))

    def check_finished(self):  # with the lock held
        if not self.pending and all(s.test is None for s in self.slots):
            self.finished.set()

    def guest_line(self, line):
        words = line.split()
        if not words:
            return
        if words[0] == "hello":
            self.hello.set()
        elif words[0] == "ended" and len(words) >= 2:
            with self.lock:
                slot = self.slots[int(words[1])]
                if slot.test:
                    self.record(slot, {"status": "CRASH", "message": "the browser ended (status %s)" % words[2:]})
                self.restart(slot)
        elif words[0] == "failed":
            print("wpt: guest: %s" % line, flush=True)
        elif words[0] == "error":
            print("wpt: guest: %s" % line, flush=True)

    # -- the pages' requests, through tests/wpt/nocturne/harness.py
    def next_test(self, index):
        with self.lock:
            slot = self.slots[index]
            if slot.test:
                self.record(slot, {"status": "CRASH", "message": "the slot's browser asked for a new test"})
            slot.opened = 0.0
            slot.failures = 0
            return self.assign(slot)

    def assign(self, slot):  # with the lock held -> the next URL, or "" to wait
        if slot.count >= self.a.restart_every or not self.pending:
            return ""
        test = self.pending.popleft()
        slot.test = test
        slot.started = time.time()
        timeout = (60 if test.long else 10) * self.a.timeout_multiplier
        slot.deadline = slot.started + timeout + self.a.load_allowance
        slot.count += 1
        return test.full_url()

    def result(self, body):
        data = json.loads(body)
        url = data.get("url") or ""
        with self.lock:
            slot = self.match(url)
            if slot is None:
                self.stray.write(json.dumps({"url": url, "at": time.time()}, ensure_ascii=False) + "\n")
                self.stray.flush()
                return ""
            self.record(slot, data)
            following = self.assign(slot)
            if not following:
                self.restart(slot)
            return following

    def match(self, url):  # with the lock held
        for slot in self.slots:
            if slot.test and slot.test.full_url() == url:
                return slot
        parts = urllib.parse.urlsplit(url)
        local = parts.path + ("?" + parts.query if parts.query else "")
        for slot in self.slots:
            if slot.test and urllib.parse.unquote(slot.test.url) == urllib.parse.unquote(local):
                return slot
        return None

    def record(self, slot, data):  # with the lock held
        test = slot.test
        slot.test = None
        status = data.get("status")
        status = HARNESS_STATUS.get(status, status)
        subtests = [{"name": t.get("name"), "status": SUBTEST_STATUS.get(t.get("status"), t.get("status")),
                     "message": (t.get("message") or "")[:400] or None} for t in data.get("tests") or []]
        line = {"test": test.url, "status": status, "message": (data.get("message") or "")[:400] or None,
                "subtests": subtests, "time": round(time.time() - slot.started, 2), "slot": slot.index}
        self.results.write(json.dumps(line, ensure_ascii=False) + "\n")
        self.results.flush()
        self.done += 1
        self.check_finished()

    def watchdog(self):
        while not self.finished.is_set():
            time.sleep(1)
            now = time.time()
            with self.lock:
                for slot in self.slots:
                    if slot.test and now > slot.deadline:
                        self.record(slot, {"status": "EXTERNAL-TIMEOUT",
                                           "message": "no report in %d s" % (now - slot.started)})
                        self.restart(slot)
                    elif slot.opened and now - slot.opened > self.a.open_timeout:
                        slot.failures += 1
                        if slot.failures > 3:
                            print("wpt: slot %d: %d browsers in a row never asked for a test; stopping" %
                                  (slot.index, slot.failures), flush=True)
                            self.stuck = True
                            self.finished.set()
                            return
                        self.restart(slot)


def collector_server(run):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, text):
            body = text.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain;charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            self.reply(run.next_test(int(query.get("slot", ["0"])[0])))

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            try:
                self.reply(run.result(body))
            except (ValueError, KeyError) as e:
                self.reply("")
                print("wpt: a report that could not be read: %s" % e, flush=True)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def control_server(run):
    listener = socket.create_server(("127.0.0.1", 0))

    def accept():
        while True:
            conn, _ = listener.accept()
            with run.guest_lock:
                run.guest = conn
            data = b""
            while True:
                try:
                    chunk = conn.recv(4096)
                except OSError:
                    break
                if not chunk:
                    break
                data += chunk
                while b"\n" in data:
                    line, data = data.split(b"\n", 1)
                    run.guest_line(line.decode("utf-8", "replace").strip())
            with run.guest_lock:
                run.guest = None
    threading.Thread(target=accept, daemon=True).start()
    return listener


@contextlib.contextmanager
def wptserve(wpt, out, multiplier, collector):
    from tools.serve import serve
    from wptserve import stash
    os.environ["NOCTURNE_WPT_COLLECTOR"] = "127.0.0.1:%d" % collector
    logger = serve.get_logger("WARNING", None)
    builder = serve.ConfigBuilder(logger)
    builder.ports = PORTS
    builder.check_subdomains = False
    builder.server_host = "127.0.0.1"
    builder.bind_address = True
    builder.doc_root = wpt
    builder.inject_script = None
    builder.logging = {"level": "WARNING", "suppress_handler_traceback": True}
    with builder as config:
        routes = serve.get_route_builder(logger, [], config)
        routes.add_static([os.path.join(HARNESS, "testharnessreport.js")], {"timeout_multiplier": multiplier},
                          "text/javascript;charset=utf8", "/resources/testharnessreport.js",
                          headers={"Cache-Control": "max-age=3600"})
        # Empty format arguments: the file is sent as it is, with no %-formatting.
        routes.add_static([os.path.join(HARNESS, "testdriver-vendor.js")], {},
                          "text/javascript;charset=utf8", "/resources/testdriver-vendor.js",
                          headers={"Cache-Control": "max-age=3600"})
        routes.add_mount_point("/_nocturne/", os.path.join(HARNESS, "nocturne"))
        with stash.StashServer(("127.0.0.1", 0), authkey=str(uuid.uuid4())):
            servers = serve.start(logger, config, routes.get_routes(), multiprocessing.get_context("spawn"), None)
            try:
                yield config
            finally:
                for server in serve.iter_servers(servers):
                    server.request_shutdown()
                for server in serve.iter_servers(servers):
                    server.wait(timeout=2)


def prepare_disk(a, test, disk, dns_port, control_port):
    shutil.copyfile(a.boot_image, disk)
    with tempfile.TemporaryDirectory(prefix="wpt-", dir=BUILD) as scratch:
        if not a.skip_build_check:
            test.system_payload(disk, scratch)
        test.edit_boot_entry(disk, scratch, "dns-server=%s:%d" % (GUEST_HOST, dns_port))
        files = []

        def note(name, text):
            path = os.path.join(scratch, name)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            files.append(("C:\\Tests\\" + name, path))
        files.append(("C:\\Tests\\wptrunner.c", a.runner_source))
        if a.https:  # WPT's certificate authority, trusted by this test disk's system (user/libc/tls.c)
            files.append(("C:\\Nocturne\\Certificates", "-"))
            files.append(("C:\\Nocturne\\Certificates\\web-platform-tests.pem",
                          os.path.join(a.wpt, "tools", "certs", "cacert.pem")))
        note("wptport", "%d\n" % control_port)
        note("autorun.sh", "tcc -o C:\\Tests\\wptrunner C:\\Tests\\wptrunner.c && C:\\Tests\\wptrunner\n")
        print("wpt: C:\\Tests: " + test.put_tests(disk, files, scratch), flush=True)
    test.write_to_larger_disk(disk, os.path.getsize(disk) + test.TEST_ROOM)


def summarize(out):
    seen = {}
    with open(os.path.join(out, "results.jsonl"), encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            seen[r["test"]] = r  # a later run of a test replaces the earlier one
    dirs = collections.defaultdict(lambda: collections.Counter())
    for r in seen.values():
        parts = r["test"].lstrip("/").split("/")
        top = "test262/" + parts[3] if parts[:3] == ["third_party", "test262", "test"] and len(parts) > 4 else parts[0]
        c = dirs[top]
        subtests = r["subtests"]
        passed = sum(1 for s in subtests if s["status"] == "PASS")
        c["tests"] += 1
        c["tests_passed"] += int(r["status"] == "OK" and passed == len(subtests) and len(subtests) > 0)
        c["subtests"] += max(1, len(subtests))
        c["subtests_passed"] += passed
        c[r["status"]] += 1
    total = collections.Counter()
    for c in dirs.values():
        total.update(c)
    summary = {"directories": {d: dict(c) for d, c in sorted(dirs.items())}, "total": dict(total)}
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=1)
    lines = ["%-34s %7s %7s %9s %9s %6s %6s %6s" % ("directory", "tests", "passed", "subtests", "passed", "crash",
                                                     "ext-to", "to")]
    for d, c in sorted(dirs.items()) + [("TOTAL", total)]:
        lines.append("%-34s %7d %7d %9d %9d %6d %6d %6d" % (d, c["tests"], c["tests_passed"], c["subtests"],
                                                            c["subtests_passed"], c["CRASH"], c["EXTERNAL-TIMEOUT"],
                                                            c["TIMEOUT"]))
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    write_failures(seen, os.path.join(out, "failures.txt"))
    return lines


def failure_shape(message):
    """a failure message with its quoted values and numbers taken out, so like failures count as one"""
    text = (message or "").split("\n", 1)[0]
    def keep_names(match):  # a quoted name says what is missing; other quoted values vary
        inner = match.group(0)[1:-1]
        return match.group(0) if re.fullmatch(r"[\w$.\[\]-]{1,48}", inner) else match.group(0)[0] * 2
    text = re.sub(r'"(?:[^"\\]|\\.)*"', keep_names, text)
    text = re.sub(r"'(?:[^'\\]|\\.)*'", keep_names, text)
    text = re.sub(r"\b\d+(\.\d+)?\b", "N", text)
    return text[:160]


def write_failures(seen, path):
    """The failing subtests (and harness errors) by the shape of their message, most first, each with
    how many tests show it and a few of them: where one missing piece fails many tests."""
    shapes = collections.defaultdict(lambda: [0, collections.Counter()])
    for r in seen.values():
        failed = [s for s in r["subtests"] if s["status"] != "PASS"]
        if r["status"] != "OK":
            failed.append({"status": r["status"], "message": r.get("message")})
        for s in failed:
            entry = shapes["%s: %s" % (s["status"], failure_shape(s.get("message")))]
            entry[0] += 1
            entry[1][r["test"]] += 1
    with open(path, "w", encoding="utf-8") as f:
        for shape, (count, tests) in sorted(shapes.items(), key=lambda kv: -kv[1][0])[:400]:
            f.write("%6d subtests in %5d tests  %s\n" % (count, len(tests), shape))
            for test, _n in tests.most_common(3):
                f.write("%32s%s\n" % ("", test))


def bind_children_to_this_process():
    """On Windows, put this process in a job that ends with it: the wptserve servers and QEMU it
    starts end too, also when this process is killed."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in ("ReadOperationCount", "WriteOperationCount",
                                                         "OtherOperationCount", "ReadTransferCount",
                                                         "WriteTransferCount", "OtherTransferCount")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    JobObjectExtendedLimitInformation = 9
    if not job or not kernel32.SetInformationJobObject(wintypes.HANDLE(job), JobObjectExtendedLimitInformation,
                                                       ctypes.byref(limits), ctypes.sizeof(limits)) or \
            not kernel32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(kernel32.GetCurrentProcess())):
        print("wpt: cannot tie the servers to this process (error %d)" % ctypes.get_last_error(), flush=True)
    bind_children_to_this_process.job = job  # the handle stays open for this process's life


def ports_in_use():
    busy = []
    for scheme, ports in PORTS.items():
        if scheme in ("webtransport-h3", "dns"):
            continue
        for port in ports:
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    busy.append(port)
    return busy


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def hmp(port, commands, timeout=30):
    """Each command's output from QEMU's human monitor on port."""
    outputs = []
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as m:
        def until_prompt():
            data = b""
            while not data.endswith(b"(qemu) "):
                chunk = m.recv(65536)
                if not chunk:
                    break
                data += chunk
            return data.decode("utf-8", "replace")
        until_prompt()
        for command in commands:
            m.sendall((command + "\n").encode("utf-8"))
            outputs.append(until_prompt())
    return outputs


def examine_guest(port, out):
    """A guest that stopped starting browsers: where its CPUs are, twice a few seconds apart,
    and its screen, into the label's folder (read the registers with build/kernel.elf)."""
    try:
        first = hmp(port, ["info registers -a"])[0]
        time.sleep(5)
        shot = os.path.join(out, "stuck.png").replace("\\", "/")
        second = hmp(port, ["info registers -a", 'screendump "%s" -f png' % shot])[0]
        with open(os.path.join(out, "stuck-registers.txt"), "w", encoding="utf-8") as f:
            f.write(first + "\n\n---- 5 s later ----\n\n" + second)
        print("wpt: the guest's registers are in %s" % os.path.join(out, "stuck-registers.txt"), flush=True)
    except OSError as error:
        print("wpt: could not examine the guest: %s" % error, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--wpt", default=DEFAULT_WPT)
    ap.add_argument("--label", default="run")
    ap.add_argument("--slots", type=int, default=4)
    ap.add_argument("--cpus", type=int, default=8)
    ap.add_argument("--memory", type=int, default=8192, help="MiB")
    ap.add_argument("--accel", choices=("tcg", "whpx"), default="whpx")
    ap.add_argument("--resolution", default="1920x1080")
    ap.add_argument("--boot-image", default=os.path.join(BUILD, "nocturne.img"))
    ap.add_argument("--skip-build-check", action="store_true", help="boot an image that is not the current build's")
    # An older image's browser takes only the options of its own time: it is run by the runner it
    # was built with (tests/wptrunner.c of that revision), not this tree's.
    ap.add_argument("--runner-source", default=os.path.join(ROOT, "tests", "wptrunner.c"),
                    help="the guest runner's C source for this image (default: tests/wptrunner.c)")
    ap.add_argument("--timeout-multiplier", type=int, default=2)
    ap.add_argument("--load-allowance", type=int, default=25, help="seconds beyond the test's own timeout")
    ap.add_argument("--open-timeout", type=int, default=120, help="seconds for a new browser to ask for a test")
    ap.add_argument("--restart-every", type=int, default=100, help="tests per browser before a new one")
    ap.add_argument("--browser-option", action="append", default=[], help="passed to each browser (--log-console)")
    ap.add_argument("--no-https", dest="https", action="store_false")
    ap.add_argument("--resume", action="store_true", help="skip the tests already in results.jsonl")
    ap.add_argument("--limit", type=int, default=0, help="at most this many tests")
    ap.add_argument("--type", action="append", choices=("testharness", "test262"),
                    help="the kinds of test to run (default testharness); test262's wrappers report through testharness.js too")
    ap.add_argument("--update-manifest", action="store_true")
    ap.add_argument("--summary-only", action="store_true")
    a = ap.parse_args()
    if not 1 <= a.slots <= 64:
        sys.exit("wpt: --slots is 1..64")
    if any(c in a.label for c in "\\/:.") or not a.label:
        sys.exit("wpt: a label is one plain name")
    out = os.path.join(BUILD, "wpt", a.label)
    os.makedirs(out, exist_ok=True)
    if a.summary_only:
        print("\n".join(summarize(out)))
        return 0
    sys.path.insert(0, a.wpt)
    from tools import localpaths  # noqa: F401  (WPT's own module paths)
    test = load_module("nocturne_test", os.path.join(ROOT, "scripts", "test.py"))
    paths = ["/" + p.replace("\\", "/").lstrip("/") for p in a.paths]
    tests = load_tests(a.wpt, os.path.join(BUILD, "wpt", "MANIFEST.json"), paths, a.https, a.update_manifest,
                       a.type or ["testharness"])
    results = os.path.join(out, "results.jsonl")
    if a.resume and os.path.exists(results):
        done = set()
        with open(results, encoding="utf-8") as f:
            for line in f:
                try:
                    done.add(json.loads(line)["test"])
                except (ValueError, KeyError):
                    pass
        tests = [t for t in tests if t.url not in done]
    elif os.path.exists(results):
        os.replace(results, results + ".old")
    if a.limit:
        tests = tests[:a.limit]
    print("wpt: %d tests" % len(tests), flush=True)
    if not tests:
        print("\n".join(summarize(out)))
        return 0

    busy = ports_in_use()
    if busy:
        sys.exit("wpt: something already listens on 127.0.0.1 port %s (an earlier run's servers?)" %
                 ", ".join(map(str, busy)))
    bind_children_to_this_process()
    run = Run(a, tests, out)
    collector = collector_server(run)
    control = control_server(run)
    with wptserve(a.wpt, out, a.timeout_multiplier, collector.server_address[1]) as config:
        dns_port, _dns = start_dns(set(config.domains_set))
        disk = os.path.join(BUILD, "wpt", "wpt.img")
        prepare_disk(a, test, disk, dns_port, control.getsockname()[1])
        serial = os.path.join(out, "serial.log")
        monitor = free_port()
        cmd = [test.QEMU, *test.vm_machine(test.QEMU, os.path.join(BUILD, "wpt", "wpt-vars.fd"), disk),
               "-m", "%dM" % a.memory, "-smp", str(a.cpus), "-accel", a.accel,
               "-cpu", "host" if a.accel == "whpx" else "max", *test.vm_display("VGA", a.resolution), "-no-reboot",
               "-serial", "file:" + serial, "-monitor", "tcp:127.0.0.1:%d,server,nowait" % monitor,
               "-netdev", "user,id=net", "-device", "e1000,netdev=net",
               "-audiodev", "none,id=snd", "-device", "AC97,audiodev=snd"]
        q = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(os.path.join(out, "run.json"), "w", encoding="utf-8") as f:
            json.dump({"pid": q.pid, "arguments": cmd, "tests": len(tests), "slots": a.slots,
                       "started": time.strftime("%Y-%m-%d %H:%M:%S")}, f, ensure_ascii=False, indent=1)
        try:
            if not run.hello.wait(300):
                sys.exit("wpt: the guest runner did not connect (see %s)" % serial)
            print("wpt: the guest runner is up after %d s" % (time.time() - run.t0), flush=True)
            with run.lock:
                for slot in run.slots:
                    run.restart(slot)
            threading.Thread(target=run.watchdog, daemon=True).start()
            last = 0
            while not run.finished.wait(30):
                if q.poll() is not None:
                    print("wpt: QEMU ended (status %s) before the tests did" % q.returncode, flush=True)
                    break
                rate = run.done / max(1.0, time.time() - run.t0)
                if run.done != last:
                    print("wpt: %d/%d done, %.1f tests/min, about %d min left" %
                          (run.done, run.total, rate * 60, (run.total - run.done) / max(rate, 1e-6) / 60), flush=True)
                    last = run.done
            if run.stuck and q.poll() is None:
                examine_guest(monitor, out)
            run.send("quit")
            try:
                q.wait(120)
            except subprocess.TimeoutExpired:
                q.kill()
                q.wait()
        finally:
            if q.poll() is None:
                q.kill()
                q.wait()
    print("\n".join(summarize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
