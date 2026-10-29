"""The product's TLS client (user/libc/tls.c) on the host against real TLS servers: local OpenSSL
servers (each version, cipher suite, group, signature, HelloRetryRequest, resumption, a
certificate request, bad certificates), a KeyUpdate server of its own (tests/tls_hostpeer.c, on
the OpenSSL library), a few hand-made bad ServerHellos, and, unless --no-net, Internet hosts. Everything is built and written under %TEMP%\\nocturne-tls-host (never
build/); QEMU is not involved.

usage: python -X utf8 tests/tls_hosttest.py [--tls user/libc/tls.c] [--no-net] [--only NAME]"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.gettempdir()) / "nocturne-tls-host"
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
openssl = root / "tools/msys64/ucrt64/bin/openssl.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))


def run(command, **kw):
    kw.setdefault("env", env)
    return subprocess.run([str(c) for c in command], capture_output=True, text=True, encoding="utf-8", errors="replace", **kw)


def build(tls_source):
    objects = work / "bearssl"
    objects.mkdir(parents=True, exist_ok=True)
    jobs, result = [], []
    for source in sorted((root / "third_party/bearssl/src").rglob("*.c")):
        obj = objects / (source.relative_to(root / "third_party/bearssl/src").as_posix().replace("/", "_") + ".o")
        result.append(obj)
        if not obj.exists() or obj.stat().st_mtime < source.stat().st_mtime:
            jobs.append([cc, "-O2", "-w", "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src", "-c", source, "-o", obj])
    roots = work / "tls_roots.c"
    if not roots.exists():
        done = run([sys.executable, "scripts/mkroots.py", "third_party/ca/ca-bundle.crt", roots], cwd=root)
        if done.returncode:
            raise SystemExit(done.stdout + done.stderr)
    jobs.append([cc, "-O2", "-w", "-Ithird_party/bearssl/inc", "-c", roots, "-o", work / "tls_roots.o"])
    result.append(work / "tls_roots.o")
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for command, done in zip(jobs, pool.map(lambda c: run(c, cwd=root), jobs)):
            if done.returncode:
                raise SystemExit(" ".join(map(str, command)) + "\n" + done.stdout + done.stderr)
    # the harness includes ../user/libc/tls.c: a tree of its own under work
    tree = work / "tree"
    (tree / "tests").mkdir(parents=True, exist_ok=True)
    (tree / "user/libc").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / "tests/tls_hosttest.c", tree / "tests/tls_hosttest.c")
    shutil.copyfile(tls_source, tree / "user/libc/tls.c")
    exe = work / "tls_hosttest.exe"
    done = run([cc, "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
                "-Ithird_party/bearssl/inc", "-iquote", "user/include", "-idirafter", "user/include", "-Icommon",
                tree / "tests/tls_hosttest.c",
                *result, "-lws2_32", "-lbcrypt", "-ladvapi32", "-o", exe], cwd=root)
    if done.returncode or done.stderr.strip():
        print(done.stdout + done.stderr)
        if done.returncode:
            raise SystemExit("harness build failed")
    peer = work / "tls_hostpeer.exe"
    done = run([cc, "-O1", "-g", "-std=gnu11", "-Wall", "-Wextra", root / "tests/tls_hostpeer.c", "-lssl", "-lcrypto", "-lws2_32", "-o", peer], cwd=root)
    if done.returncode or done.stderr.strip():
        print(done.stdout + done.stderr)
        if done.returncode:
            raise SystemExit("peer build failed")
    return exe, peer


def certificates():
    """A test CA, leaves for localhost (RSA 2048, P-256, P-384), and three that must fail."""
    d = work / "certs"
    if (d / "done").exists():
        return d
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    conf = d / "openssl.cnf"
    conf.write_text("[req]\ndistinguished_name=dn\n[dn]\n[ca]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
                    "[rsa]\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n"
                    "[ec]\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n"
                    "[other]\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:other.test\n")

    def ossl(*a):
        done = run([openssl, *a], cwd=d)
        if done.returncode:
            raise SystemExit("openssl " + " ".join(map(str, a)) + "\n" + done.stdout + done.stderr)

    ossl("req", "-config", conf, "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem",
         "-subj", "/CN=Nocturne TLS test CA", "-days", "30", "-extensions", "ca")
    (d / "trusted").mkdir()
    shutil.copyfile(d / "ca.pem", d / "trusted/ca.pem")
    for name, key, ext, days in (("rsa", "rsa:2048", "rsa", "30"), ("p256", "ec", "ec", "30"), ("p384", "ec", "ec", "30"),
                                 ("other", "rsa:2048", "other", "30"), ("expired", "rsa:2048", "rsa", "-1")):
        if key == "ec":
            ossl("ecparam", "-name", "prime256v1" if name == "p256" else "secp384r1", "-genkey", "-noout", "-out", name + ".key")
            ossl("req", "-config", conf, "-new", "-key", name + ".key", "-subj", "/CN=localhost", "-out", name + ".csr")
        else:
            ossl("req", "-config", conf, "-new", "-newkey", key, "-nodes", "-keyout", name + ".key", "-subj", "/CN=localhost", "-out", name + ".csr")
        if days == "-1":  # valid yesterday only
            ossl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", name + ".pem",
                 "-extfile", conf, "-extensions", ext, "-not_before", "20200101000000Z", "-not_after", "20200201000000Z")
        else:
            ossl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", name + ".pem",
                 "-extfile", conf, "-extensions", ext, "-days", days)
    # a certificate no anchor knows
    ossl("req", "-config", conf, "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "self.key", "-out", "self.pem",
         "-subj", "/CN=localhost", "-days", "30", "-extensions", "rsa")
    (d / "done").write_text("ok")
    return d


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, certs, cert, args, www=True, chain=True):
        self.port = free_port()
        command = [openssl, "s_server", "-accept", f"127.0.0.1:{self.port}", "-cert", certs / (cert + ".pem"), "-key", certs / (cert + ".key"),
                   *(["-cert_chain", certs / "ca.pem"] if chain else []), *args]
        if www:
            command.append("-www")
        self.out = []
        self.p = subprocess.Popen([str(c) for c in command], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding="utf-8", errors="replace", bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.1).close()
                break
            except OSError:
                time.sleep(0.05)
        time.sleep(0.2)
        self.out.clear()

    def _read(self):
        for line in self.p.stdout:
            self.out.append(line)

    def send(self, text):
        self.p.stdin.write(text)
        self.p.stdin.flush()

    def stop(self):
        self.p.kill()
        self.p.wait()


def harness(exe, certs, *args, timeout=60):
    return run([exe, *args], env=dict(env, TLS_TEST_CERTS=str(certs / "trusted")), timeout=timeout)


def results(text):
    return [dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv) | {"ok": line.split()[1] == "ok"}
            for line in text.splitlines() if line.startswith("RESULT ")]


class Checks:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, condition, detail=""):
        if condition:
            self.passed += 1
            print(f"PASS {name}")
        else:
            self.failed += 1
            print(f"FAIL {name}\n{detail}")


def local_cases(exe, peer_exe, certs, checks, only):
    def case(name, cert, server_args, expect, times=1, http="0", send="get", www=True):
        if only and only not in name:
            return
        server = Server(certs, cert, server_args, www, chain=cert != "self")
        try:
            done = harness(exe, certs, "connect", "localhost", str(server.port), http, str(times), send)
        finally:
            server.stop()
        r = results(done.stdout)
        detail = done.stdout + done.stderr + "".join(server.out[-20:])
        checks.check(name, len(r) == times and expect(r, done.stdout), detail)

    v13 = lambda suite=None: (lambda r, o: all(x["ok"] and x["version"] == "1.3" and (suite is None or x["suite"] == suite) for x in r) and "response: HTTP/1.0 200 ok" in o)
    v12 = lambda suite: (lambda r, o: all(x["ok"] and x["version"] == "1.2" and x["suite"] == suite for x in r) and "response: HTTP/1.0 200 ok" in o)
    case("tls13 rsa-pss default suite", "rsa", ["-tls1_3"], v13("1301"))
    case("tls13 aes256-gcm", "rsa", ["-tls1_3", "-ciphersuites", "TLS_AES_256_GCM_SHA384"], v13("1302"))
    case("tls13 aes128-gcm", "rsa", ["-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256"], v13("1301"))
    case("tls13 chacha20-poly1305", "rsa", ["-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256"], v13("1303"))
    case("tls13 ecdsa p-256", "p256", ["-tls1_3"], v13())
    case("tls13 ecdsa p-384", "p384", ["-tls1_3"], v13())
    case("tls13 rsa-pss sha384 and sha512", "rsa", ["-tls1_3", "-sigalgs", "rsa_pss_rsae_sha512:rsa_pss_rsae_sha384"], v13())
    case("tls13 group p-256", "rsa", ["-tls1_3", "-groups", "P-256"], v13())
    case("tls13 hello retry p-384", "rsa", ["-tls1_3", "-groups", "P-384"], v13())
    case("tls13 hello retry p-521", "rsa", ["-tls1_3", "-groups", "P-521"], v13())
    case("tls13 hello retry with cookie (stateless)", "rsa", ["-tls1_3", "-stateless"], v13())
    case("tls13 certificate request", "rsa", ["-tls1_3", "-verify", "1"], v13())
    case("tls13 resumption", "rsa", ["-tls1_3"], lambda r, o: v13()(r, o) and [x["resumed"] for x in r] == ["0", "1", "1"], times=3)
    case("tls13 resumption after hello retry", "rsa", ["-tls1_3", "-groups", "P-384"], lambda r, o: v13()(r, o) and [x["resumed"] for x in r] == ["0", "1"], times=2)
    case("tls13 alpn h2", "rsa", ["-tls1_3", "-alpn", "h2"], lambda r, o: r[0]["ok"] and r[0]["alpn"] == "h2", http="1", send="none")
    case("tls13 alpn http/1.1", "rsa", ["-tls1_3", "-alpn", "http/1.1"], lambda r, o: r[0]["ok"] and r[0]["alpn"] == "http/1.1", http="1")
    for suite, name, cert in (("c02f", "ECDHE-RSA-AES128-GCM-SHA256", "rsa"), ("c030", "ECDHE-RSA-AES256-GCM-SHA384", "rsa"),
                              ("cca8", "ECDHE-RSA-CHACHA20-POLY1305", "rsa"), ("c013", "ECDHE-RSA-AES128-SHA", "rsa"),
                              ("c014", "ECDHE-RSA-AES256-SHA", "rsa"), ("009c", "AES128-GCM-SHA256", "rsa"), ("009d", "AES256-GCM-SHA384", "rsa"),
                              ("002f", "AES128-SHA", "rsa"), ("0035", "AES256-SHA", "rsa"), ("c02b", "ECDHE-ECDSA-AES128-GCM-SHA256", "p256"),
                              ("c02c", "ECDHE-ECDSA-AES256-GCM-SHA384", "p256"), ("cca9", "ECDHE-ECDSA-CHACHA20-POLY1305", "p256"),
                              ("c009", "ECDHE-ECDSA-AES128-SHA", "p256"), ("c00a", "ECDHE-ECDSA-AES256-SHA", "p384")):
        case(f"tls12 {name}", cert, ["-tls1_2", "-cipher", name + ":@SECLEVEL=0"], v12(suite))
    case("tls12 rsa pkcs1 sha256 signature", "rsa", ["-tls1_2", "-sigalgs", "RSA+SHA256"], v12("c02f"))
    case("tls12 rsa pkcs1 sha512 signature", "rsa", ["-tls1_2", "-sigalgs", "RSA+SHA512"], v12("c02f"))
    for group in ("X25519", "P-256", "P-384", "P-521"):
        case(f"tls12 group {group}", "rsa", ["-tls1_2", "-groups", group], v12("c02f"))
    case("tls12 without extended master secret", "rsa", ["-tls1_2", "-no_ems"], v12("c02f"))
    case("tls12 session id resumption", "rsa", ["-tls1_2", "-no_ticket"], lambda r, o: v12("c02f")(r, o) and [x["resumed"] for x in r] == ["0", "1", "1"], times=3)
    case("tls12 certificate request", "rsa", ["-tls1_2", "-verify", "1"], v12("c02f"))
    case("tls12 alpn h2", "rsa", ["-tls1_2", "-alpn", "h2"], lambda r, o: r[0]["ok"] and r[0]["alpn"] == "h2", http="1", send="none")
    case("tls12 h2 refuses a CBC suite", "rsa", ["-tls1_2", "-alpn", "h2", "-cipher", "ECDHE-RSA-AES128-SHA"],
         lambda r, o: not r[0]["ok"] and "AEAD" in o, http="1", send="none")
    bad = lambda text: (lambda r, o: not r[0]["ok"] and text in o)
    case("refuses an expired certificate", "expired", [], bad("certificate expired"))
    case("refuses another host's certificate", "other", [], bad("does not match the host name"))
    case("refuses an untrusted certificate", "self", [], bad("not trusted"))
    case("refuses an expired certificate on tls12", "expired", ["-tls1_2"], bad("certificate expired"))
    case("refuses an untrusted certificate on tls12", "self", ["-tls1_2"], bad("not trusted"))
    case("refuses tls 1.1", "rsa", ["-tls1_1", "-cipher", "ALL:@SECLEVEL=0"], lambda r, o: not r[0]["ok"])
    if not only or "key update" in only:
        chain = certs / "rsa-chain.pem"
        chain.write_bytes((certs / "rsa.pem").read_bytes() + (certs / "ca.pem").read_bytes())
        port = free_port()
        peer = subprocess.Popen([str(peer_exe), "keyupdate", str(port), str(chain), str(certs / "rsa.key")], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        try:
            first = peer.stdout.readline()
            done = harness(exe, certs, "keyupdate", "localhost", str(port), timeout=40) if first.startswith("LISTENING") else None
            peer_out = first + peer.communicate(timeout=30)[0]
        finally:
            if peer.poll() is None:
                peer.kill()
                peer.wait()
        out = (done.stdout + done.stderr) if done else ""
        checks.check("tls13 key update requested by the server", done is not None and done.returncode == 0 and peer.returncode == 0
                     and "read key updated=1 write key updated=1" in out and "after pong: close_notify" in out, out + peer_out)


def fake_server(reply):
    """A server that reads one ClientHello and answers with reply(client_hello) bytes."""
    port = free_port()
    listener = socket.socket()
    listener.bind(("127.0.0.1", port))
    listener.listen(1)

    def serve():
        c, _ = listener.accept()
        data = b""
        while len(data) < 5 or len(data) < 5 + struct.unpack(">H", data[3:5])[0]:
            chunk = c.recv(65536)
            if not chunk:
                break
            data += chunk
        c.sendall(reply(data))
        try:
            fake_server.alert = c.recv(64)
        except OSError:
            fake_server.alert = b""
        c.close()
        listener.close()

    threading.Thread(target=serve, daemon=True).start()
    return port


def server_hello(ch, version=0x0303, suite=0xc02f, random=None, extensions=b"", echo=False):
    sid = ch[5 + 4 + 2 + 32 + 1:5 + 4 + 2 + 32 + 1 + ch[5 + 4 + 2 + 32]] if echo else b""
    body = struct.pack(">H", version) + (random or os.urandom(32)) + bytes([len(sid)]) + sid + struct.pack(">HB", suite, 0)
    body += struct.pack(">H", len(extensions)) + extensions
    msg = b"\x02" + struct.pack(">I", len(body))[1:] + body
    return b"\x16\x03\x03" + struct.pack(">H", len(msg)) + msg


def fake_cases(exe, certs, checks, only):
    def case(name, reply, expect_text, expect_alert):
        if only and only not in name:
            return
        fake_server.alert = None
        port = fake_server(reply)
        done = harness(exe, certs, "connect", "localhost", str(port), "0", "1", "none", timeout=30)
        time.sleep(0.2)
        alert = fake_server.alert or b""
        ok = "RESULT fail" in done.stdout and expect_text in done.stdout and alert[:1] == b"\x15" and alert[5:7] == bytes([2, expect_alert])
        checks.check(name, ok, done.stdout + done.stderr + f"alert sent: {alert.hex()}")

    case("downgrade sentinel DOWNGRD 01 refused", lambda ch: server_hello(ch, random=os.urandom(24) + b"DOWNGRD\x01"), "downgrade", 47)
    case("downgrade sentinel DOWNGRD 00 refused", lambda ch: server_hello(ch, random=os.urandom(24) + b"DOWNGRD\x00"), "downgrade", 47)
    case("cipher suite not offered refused", lambda ch: server_hello(ch, suite=0x000a), "cipher suite", 47)
    case("tls 1.1 server hello refused", lambda ch: server_hello(ch, version=0x0302), "TLS 1.2 or 1.3", 70)
    case("unsolicited extension refused", lambda ch: server_hello(ch, extensions=struct.pack(">HH", 0x1234, 0)), "extension", 110)
    case("tls13 session id not echoed refused",
         lambda ch: server_hello(ch, suite=0x1301, extensions=struct.pack(">HHH", 43, 2, 0x0304) + struct.pack(">HHHH", 51, 36, 29, 32) + os.urandom(32)),
         "legacy_session_id", 47)
    case("random session id echoed as resumption refused", lambda ch: server_hello(ch, echo=True), "not offered", 47)


def net_cases(exe, certs, checks, only):
    def case(name, host, port, expect, times=1, http="1", send="get"):
        if only and only not in name:
            return
        done = harness(exe, certs, "connect", host, str(port), http, str(times), send, timeout=90)
        r = results(done.stdout)
        checks.check(name, len(r) == times and expect(r, done.stdout), done.stdout + done.stderr)

    ok13 = lambda r, o: all(x["ok"] and x["version"] == "1.3" for x in r) and "read failed" not in o and "write failed" not in o
    for host in ("www.cloudflare.com", "nowsecure.nl", "www.scrapingcourse.com", "challenges.cloudflare.com", "www.google.com", "www.youtube.com",
                 "github.com", "www.wikipedia.org", "www.bilibili.com", "www.mozilla.org", "cdn.jsdelivr.net"):
        case(f"net {host} h2", host, 443, ok13)
        case(f"net {host} http/1.1", host, 443, lambda r, o: ok13(r, o) and "response: HTTP/1.1" in o, http="0")
    # the NewSessionTicket comes after the handshake: reading a whole HTTP/1.1 response takes it.
    # The edge may decline a ticket (a full handshake; OpenSSL's s_client resumed 3 of 6 on
    # 2026-10-29), so one resumption of four connections is the expectation.
    case("net www.cloudflare.com resumption", "www.cloudflare.com", 443,
         lambda r, o: ok13(r, o) and any(x["resumed"] == "1" for x in r[1:]), times=4, http="0")
    case("net tls-v1-2.badssl.com:1012 (TLS 1.2 only)", "tls-v1-2.badssl.com", 1012,
         lambda r, o: r[0]["ok"] and r[0]["version"] == "1.2" and "response: HTTP/1.1" in o, http="0")
    for host in ("ecc256.badssl.com", "ecc384.badssl.com", "rsa2048.badssl.com", "rsa4096.badssl.com", "sha256.badssl.com"):
        case(f"net {host}", host, 443, lambda r, o: r[0]["ok"] and "response: HTTP/1.1" in o, http="0")
    for host, text in (("expired.badssl.com", "certificate expired"), ("wrong.host.badssl.com", "does not match the host name"),
                       ("self-signed.badssl.com", "not trusted"), ("untrusted-root.badssl.com", "not trusted"),
                       ("1000-sans.badssl.com", "certificate expired")):  # its certificate ended on 2021-10-13
        case(f"net {host} refused", host, 443, lambda r, o, text=text: not r[0]["ok"] and text in o, http="0")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tls", default=str(root / "user/libc/tls.c"))
    parser.add_argument("--no-net", action="store_true")
    parser.add_argument("--only")
    a = parser.parse_args()
    exe, peer_exe = build(Path(a.tls))
    certs = certificates()
    checks = Checks()
    local_cases(exe, peer_exe, certs, checks, a.only)
    fake_cases(exe, certs, checks, a.only)
    if not a.no_net:
        net_cases(exe, certs, checks, a.only)
    print(f"{checks.passed} passed, {checks.failed} failed")
    sys.exit(1 if checks.failed else 0)


if __name__ == "__main__":
    main()
