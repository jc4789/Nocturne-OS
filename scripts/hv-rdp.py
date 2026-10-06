"""A minimal RDP client for testing the Enhanced Session server in a Nocturne Hyper-V guest.

It connects over a Hyper-V socket (as VMConnect does), runs the connection sequence the way
VMConnect does it (standard RDP security, no encryption), then plays a list of input actions and
rebuilds the desktop from the bitmap updates it receives. The picture is saved as a PNG, so
output and input are both checked without touching the host's own keyboard or mouse.

Needs Windows, Python 3.12+ (socket.AF_HYPERV) and an elevated shell.

    python scripts/hv-rdp.py --vm Nocturne-G2 --size 1280x720 --out build/hv/rdp.png \
        "type:neofetch\\n" wait:2 dclick:53,130 wait:2

Actions: type:TEXT (letters, digits, space and \\n), key:SCANCODE (hex), move:X,Y, click:X,Y,
dclick:X,Y, wheel:X,Y,N (N notches, positive is up), wait:SECONDS.
"""
import argparse
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

SERVICE = "{:08x}-facb-11e6-bd58-64006a7986d3"

# set 1 scancodes for the characters `type:` knows
SCANCODES = {c: s for s, row in ((0x10, "qwertyuiop"), (0x1E, "asdfghjkl"), (0x2C, "zxcvbnm"))
             for c, s in zip(row, range(s, s + len(row)))}
SCANCODES.update({str(d): 0x02 + (d - 1) % 10 for d in range(10)})
SCANCODES.update({" ": 0x39, "\n": 0x1C, "-": 0x0C, ".": 0x34, "/": 0x35})


def vm_id(name):
    out = subprocess.run(["powershell", "-NoProfile", "-Command", f"(Get-VM -Name '{name}').Id.Guid"],
                         capture_output=True, text=True, check=True).stdout.strip()
    if not out:
        sys.exit(f"no VM called {name}")
    return out


def tpkt(payload):
    return struct.pack(">BBH", 3, 0, 4 + len(payload)) + payload


def x224_data(payload):
    return tpkt(b"\x02\xf0\x80" + payload)


def ber_len(n):
    return bytes([n]) if n < 0x80 else b"\x82" + struct.pack(">H", n)


def per_len(n):
    return struct.pack(">H", 0x8000 | n)


class Client:
    def __init__(self, sock, width, height):
        self.s = sock
        self.w, self.h = width, height
        self.user = 0
        self.fb = bytearray(width * height * 3)
        self.updates = 0
        self.lock = threading.Lock()
        self.up = threading.Event()
        self.closed = False

    # ---- reading ----
    def recv_exact(self, n):
        b = b""
        while len(b) < n:
            chunk = self.s.recv(n - len(b))
            if not chunk:
                raise EOFError("server closed the connection")
            b += chunk
        return b

    def read_pdu(self):
        """A TPKT PDU (returned whole) or a fast-path one (returned as b"FP" + its updates)."""
        h = self.recv_exact(2)
        if h[0] == 3:
            lb = self.recv_exact(2)
            return h + lb + self.recv_exact(struct.unpack(">H", lb)[0] - 4)
        n = h[1]
        hdr = 2
        if n & 0x80:
            n = (n & 0x7F) << 8 | self.recv_exact(1)[0]
            hdr = 3
        return b"FP" + self.recv_exact(n - hdr)

    def reader(self):
        try:
            while True:
                p = self.read_pdu()
                if p[:2] == b"FP":
                    self.fastpath(p[2:])
        except (EOFError, OSError):
            self.closed = True

    def fastpath(self, d):
        i = 0
        while i + 3 <= len(d):
            code = d[i] & 0x0F
            comp = d[i] >> 6
            i += 1
            if comp == 2:
                i += 1
            size = struct.unpack_from("<H", d, i)[0]
            i += 2
            body = d[i:i + size]
            i += size
            if code == 1:
                self.bitmap(body)

    def bitmap(self, b):
        n = struct.unpack_from("<H", b, 2)[0]
        o = 4
        for _ in range(n):
            left, top, right, bottom, w, h, bpp, flags, size = struct.unpack_from("<9H", b, o)
            o += 18
            data = b[o:o + size]
            o += size
            if flags or bpp not in (32, 24, 16):
                continue
            bypp = bpp // 8
            cols = min(right, self.w - 1) - left + 1
            with self.lock:
                for j in range(h):  # rows arrive bottom-up
                    y = bottom - j
                    if y < 0 or y >= self.h or cols <= 0:
                        continue
                    seg = data[j * w * bypp:j * w * bypp + cols * bypp]
                    rgb = bytearray(cols * 3)
                    if bypp == 2:
                        for i in range(cols):
                            v = seg[2 * i] | seg[2 * i + 1] << 8
                            rgb[3 * i:3 * i + 3] = bytes(((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8))
                    else:
                        rgb[0::3], rgb[1::3], rgb[2::3] = seg[2::bypp], seg[1::bypp], seg[0::bypp]
                    o2 = (y * self.w + left) * 3
                    self.fb[o2:o2 + cols * 3] = rgb
                self.updates += 1

    # ---- connection sequence ----
    def connect(self):
        neg = struct.pack("<BBHI", 1, 0, 8, 0)  # RDP_NEG_REQ: standard RDP security
        cookie = b"Cookie: mstshash=hv-rdp\r\n"
        cr = bytes([6 + len(cookie) + len(neg), 0xE0, 0, 0, 0, 0, 0]) + cookie + neg
        self.s.sendall(tpkt(cr))
        cc = self.read_pdu()
        if cc[5] != 0xD0:
            raise RuntimeError("no X.224 connection confirm")

        name = "hv-rdp".encode("utf-16-le").ljust(32, b"\0")
        core = struct.pack("<IHHHHII", 0x00080004, self.w, self.h, 0xCA01, 0xAA03, 0x409, 2600) + name
        core += struct.pack("<III", 4, 0, 12) + b"\0" * 64
        core += struct.pack("<HHIHHH", 0xCA01, 1, 0, 32, 0x000F, 0x0002)
        blocks = struct.pack("<HH", 0xC001, 4 + len(core)) + core
        blocks += struct.pack("<HHII", 0xC002, 12, 0, 0)
        blocks += struct.pack("<HHI", 0xC003, 8, 0)
        gcc = b"\x00\x05\x00\x14\x7c\x00\x01"
        ccrq = b"\x00\x08\x00\x10\x00\x01\xc0\x00Duca" + per_len(len(blocks)) + blocks
        gcc += per_len(len(ccrq)) + ccrq
        p = bytes.fromhex("020122020102020100020101020100020101020300fff8020102")  # domain parameters
        params = b"\x30" + ber_len(len(p)) + p
        body = b"\x04\x01\x01" + b"\x04\x01\x01" + b"\x01\x01\xff" + params * 3
        body += b"\x04" + ber_len(len(gcc)) + gcc
        self.s.sendall(x224_data(b"\x7f\x65" + ber_len(len(body)) + body))
        self.read_pdu()  # connect response

        self.s.sendall(x224_data(b"\x04\x01\x00\x01\x00"))  # erect domain
        self.s.sendall(x224_data(b"\x28"))  # attach user
        ac = self.read_pdu()
        self.user = 1001 + struct.unpack_from(">H", ac, 9)[0]
        for ch in (self.user, 1003):
            self.s.sendall(x224_data(b"\x38" + struct.pack(">HH", self.user - 1001, ch)))
            self.read_pdu()

        info = struct.pack("<HH", 0x0040, 0) + struct.pack("<II5H", 0, 0x0033, 0, 0, 0, 0, 0) + b"\0" * 10
        self.send_io(info)
        self.s.settimeout(None)  # from here on, a quiet desktop sends nothing for as long as it likes
        threading.Thread(target=self.reader, daemon=True).start()
        time.sleep(0.5)  # licence and demand active arrive; nothing in them matters here

        confirm = struct.pack("<IHHH", 0x000103EA, 0x03EA, 4, 4) + b"RDP\0" + struct.pack("<HH", 0, 0)
        self.send_share(0x13, confirm)
        self.send_data(0x1F, struct.pack("<HH", 1, 1002))
        self.send_data(0x14, struct.pack("<HHI", 4, 0, 0))
        self.send_data(0x14, struct.pack("<HHI", 1, 0, 0))
        self.send_data(0x27, struct.pack("<HHHH", 0, 0, 3, 50))

    def send_io(self, data):
        sdr = b"\x64" + struct.pack(">HH", self.user - 1001, 1003) + b"\x70" + per_len(len(data)) + data
        self.s.sendall(x224_data(sdr))

    def send_share(self, pdutype, body):
        self.send_io(struct.pack("<HHH", 6 + len(body), pdutype, self.user) + body)

    def send_data(self, type2, payload):
        hdr = struct.pack("<IBBHBBH", 0x000103EA, 0, 1, len(payload) + 4, type2, 0, 0)
        self.send_share(0x17, hdr + payload)

    # ---- input (fast-path) ----
    def events(self, evs):
        for k in range(0, len(evs), 15):
            chunk = evs[k:k + 15]
            body = b"".join(chunk)
            n = 2 + len(body)
            if n < 0x80:
                pdu = bytes([len(chunk) << 2, n]) + body
            else:
                pdu = bytes([len(chunk) << 2]) + struct.pack(">H", 0x8000 | (n + 1)) + body
            self.s.sendall(pdu)

    @staticmethod
    def key_ev(code, release):
        return bytes([0x01 if release else 0x00, code])

    @staticmethod
    def mouse_ev(flags, x, y):
        return bytes([1 << 5]) + struct.pack("<HHH", flags, x, y)

    def type(self, text):
        evs = []
        for ch in text:
            code = SCANCODES[ch.lower()]
            evs += [self.key_ev(code, False), self.key_ev(code, True)]
        for e in evs:  # one at a time, as a keyboard would
            self.events([e])
            time.sleep(0.02)

    def click(self, x, y, times=1):
        self.events([self.mouse_ev(0x0800, x, y)])
        time.sleep(0.05)
        for _ in range(times):
            self.events([self.mouse_ev(0x9000, x, y)])
            time.sleep(0.03)
            self.events([self.mouse_ev(0x1000, x, y)])
            time.sleep(0.08)

    def save_png(self, path):
        with self.lock:
            raw = b"".join(b"\0" + bytes(self.fb[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vm", default="Nocturne-G2")
    ap.add_argument("--port", type=int, default=3389)
    ap.add_argument("--size", default="1024x768")
    ap.add_argument("--out", default="build/hv/rdp.png")
    ap.add_argument("actions", nargs="*")
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split("x"))

    s = socket.socket(socket.AF_HYPERV, socket.SOCK_STREAM, socket.HV_PROTOCOL_RAW)
    s.settimeout(15)
    s.connect((vm_id(a.vm), SERVICE.format(a.port)))
    c = Client(s, w, h)
    c.connect()
    time.sleep(2)
    print(f"connected as {w}x{h}: {c.updates} bitmap updates so far")
    for act in a.actions:
        verb, _, arg = act.partition(":")
        if verb == "type":
            c.type(arg.encode().decode("unicode_escape"))
        elif verb == "key":
            code = int(arg, 16)
            c.events([c.key_ev(code, False)])
            c.events([c.key_ev(code, True)])
        elif verb in ("move", "click", "dclick"):
            x, y = (int(v) for v in arg.split(","))
            if verb == "move":
                c.events([c.mouse_ev(0x0800, x, y)])
            else:
                c.click(x, y, 2 if verb == "dclick" else 1)
        elif verb == "wheel":  # one event per notch: 120 up, -120 (9-bit two's complement) down
            x, y, n = (int(v) for v in arg.split(","))
            flags = 0x0200 | (120 if n > 0 else 0x100 | (-120 & 0xFF))
            for _ in range(abs(n)):
                c.events([c.mouse_ev(flags, x, y)])
                time.sleep(0.05)
        elif verb == "wait":
            time.sleep(float(arg))
        else:
            sys.exit(f"unknown action {act}")
    time.sleep(1)
    if c.closed:
        sys.exit("the server closed the connection")
    c.save_png(a.out)
    print(f"{c.updates} bitmap updates; saved {a.out}")
    s.close()


if __name__ == "__main__":
    main()
