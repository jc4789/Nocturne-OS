#!/usr/bin/env python3
"""Headless QEMU test driver.

usage: qtest.py [--iso] [--wait N] [script...]
script steps:  sleep:SECS  shot:NAME  key:KEYSPEC  type:TEXT  mouse:DX,DY  click[:BTN]
Serial output goes to build/serial.log; screenshots to build/shots/NAME.png
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "tools", "msys64", "ucrt64", "bin", "qemu-system-x86_64.exe")
SHOTS = os.path.join(ROOT, "build", "shots")


def ppm_to_png(ppm, png):
    with open(ppm, "rb") as f:
        data = f.read()
    # parse header
    parts = []
    i = 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = data[i:i + w * h * 3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


class Monitor:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port))
                break
            except OSError:
                time.sleep(0.1)
        self.s.settimeout(5)
        self.read()

    def read(self):
        buf = b""
        try:
            while not buf.endswith(b"(qemu) "):
                d = self.s.recv(65536)
                if not d:
                    break
                buf += d
        except (socket.timeout, ConnectionError):
            pass
        return buf

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\n")
        return self.read()


KEYMAP = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", "/": "slash", ".": "dot", ",": "comma",
          ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
          "`": "grave_accent", "\t": "tab", "*": "shift-8", "|": "shift-backslash", ">": "shift-dot",
          "<": "shift-comma", "_": "shift-minus", "+": "shift-equal", ":": "shift-semicolon", '"': "shift-apostrophe",
          "!": "shift-1", "@": "shift-2", "#": "shift-3", "$": "shift-4", "%": "shift-5", "^": "shift-6",
          "&": "shift-7", "(": "shift-9", ")": "shift-0", "?": "shift-slash", "~": "shift-grave_accent",
          "{": "shift-bracket_left", "}": "shift-bracket_right"}


def keyname(ch):
    if ch in KEYMAP:
        return KEYMAP[ch]
    if ch.isupper():
        return "shift-" + ch.lower()
    return ch


def main():
    args = sys.argv[1:]
    media = ["-drive", "file=" + os.path.join(ROOT, "build", "nocturne.img") + ",format=raw,if=ide"]
    extra = []
    if args and args[0] == "--iso":
        media = ["-cdrom", os.path.join(ROOT, "build", "nocturne.iso")]
        args = args[1:]
    while args and args[0].startswith("--q="):
        extra += args[0][4:].split()
        args = args[1:]
    os.makedirs(SHOTS, exist_ok=True)
    port = 45454
    serial = os.path.join(ROOT, "build", "serial.log")
    cmd = [QEMU, "-M", "pc", "-m", "512M", "-display", "none", "-vga", "std",
           "-serial", "file:" + serial, "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
           "-no-reboot"] + media + extra
    p = subprocess.Popen(cmd, cwd=ROOT)
    try:
        mon = Monitor(port)
        for step in args:
            kind, _, val = step.partition(":")
            if kind == "sleep":
                time.sleep(float(val))
            elif kind == "shot":
                ppm = os.path.join(SHOTS, val + ".ppm")
                mon.cmd("screendump build/shots/" + val + ".ppm")
                time.sleep(0.5)
                ppm_to_png(ppm, os.path.join(SHOTS, val + ".png"))
                os.remove(ppm)
                print("shot", os.path.join(SHOTS, val + ".png"))
            elif kind == "key":
                mon.cmd("sendkey " + val)
                time.sleep(0.05)
            elif kind == "type":
                val = val.replace("\\n", "\n")
                for ch in val:
                    mon.cmd("sendkey " + keyname(ch))
                    time.sleep(0.04)
            elif kind == "mouse":
                dx, dy = val.split(",")
                mon.cmd("mouse_move %s %s" % (dx, dy))
                time.sleep(0.05)
            elif kind == "goto":
                # absolute move: slam into the top-left corner, then walk in unaccelerated steps
                tx, ty = (int(v) for v in val.split(","))
                for _ in range(12):
                    mon.cmd("mouse_move -200 -200")
                    time.sleep(0.02)
                while tx > 0 or ty > 0:
                    sx, sy = min(tx, 5), min(ty, 5)
                    mon.cmd("mouse_move %d %d" % (sx, sy))
                    time.sleep(0.02)
                    tx -= sx
                    ty -= sy
                time.sleep(0.1)
            elif kind == "dclick":
                for _ in range(2):
                    mon.cmd("mouse_button 1")
                    time.sleep(0.05)
                    mon.cmd("mouse_button 0")
                    time.sleep(0.08)
            elif kind == "click":
                btn = int(val) if val else 1
                mon.cmd("mouse_button %d" % btn)
                time.sleep(0.1)
                mon.cmd("mouse_button 0")
                time.sleep(0.1)
            elif kind == "down":
                mon.cmd("mouse_button %d" % (int(val) if val else 1))
            elif kind == "up":
                mon.cmd("mouse_button 0")
            elif kind == "mon":
                print(mon.cmd(val).decode(errors="replace"))
        mon.cmd("quit")
    finally:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    with open(serial, "rb") as f:
        log = f.read().decode(errors="replace")
    print("---- serial ----")
    print(log[-6000:])


main()
