#!/usr/bin/env python3
"""Make the sounds in /home/Music: three short pieces written for Nocturne, synthesised here so
the repository holds no recordings. Each is a different kind of WAV, so the Sound Player's
reader and resampler get used: 22.05 kHz stereo 16-bit, 16 kHz mono 8-bit, 48 kHz stereo 16-bit.

usage: mksounds.py OUTDIR
"""
import math
import os
import random
import struct
import sys
import wave

NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def hz(name):
    """'A4', 'C#5', 'Bb3' -> frequency"""
    n = NOTE[name[0]]
    rest = name[1:]
    if rest[0] == "#":
        n, rest = n + 1, rest[1:]
    elif rest[0] == "b":
        n, rest = n - 1, rest[1:]
    return 440.0 * 2 ** ((n + 12 * (int(rest) + 1) - 69) / 12)


class Track:
    def __init__(self, rate, seconds, stereo=True):
        self.rate = rate
        self.n = int(rate * seconds)
        self.l = [0.0] * self.n
        self.r = [0.0] * self.n if stereo else self.l

    def add(self, start, samples, pan=0.5):
        i0 = int(start * self.rate)
        gl, gr = math.cos(pan * math.pi / 2), math.sin(pan * math.pi / 2)
        if self.r is self.l:
            gl, gr = 1.0, 0.0
        for k, v in enumerate(samples):
            i = i0 + k
            if i >= self.n:
                break
            self.l[i] += v * gl
            if gr:
                self.r[i] += v * gr

    def write(self, path, bits=16, peak=0.85):
        top = max(max(abs(x) for x in self.l), max(abs(x) for x in self.r), 1e-9)
        g = peak / top
        chans = 1 if self.r is self.l else 2
        out = bytearray()
        for i in range(self.n):
            for v in ((self.l[i],) if chans == 1 else (self.l[i], self.r[i])):
                v = max(-1.0, min(1.0, v * g))
                if bits == 8:
                    out.append(int(round(v * 127)) + 128)
                else:
                    out += struct.pack("<h", int(round(v * 32767)))
        with wave.open(path, "wb") as w:
            w.setnchannels(chans)
            w.setsampwidth(bits // 8)
            w.setframerate(self.rate)
            w.writeframes(bytes(out))


def bell(rate, f, length, amp=1.0, decay=3.0):
    """a soft electric-piano bell: a few harmonics that die away, the upper ones faster"""
    n = int(rate * length)
    w = 2 * math.pi * f / rate
    out = []
    for k in range(n):
        t = k / rate
        a = min(1.0, k / (0.004 * rate))
        v = math.sin(w * k) * math.exp(-decay * t)
        v += 0.35 * math.sin(2 * w * k) * math.exp(-decay * 2.2 * t)
        v += 0.12 * math.sin(3 * w * k) * math.exp(-decay * 3.5 * t)
        out.append(amp * a * v)
    return out


def pad(rate, f, length, amp=1.0):
    """a slow swell of two slightly detuned sines"""
    n = int(rate * length)
    out = []
    for k in range(n):
        t = k / rate
        env = min(1.0, t / 0.6) * min(1.0, (length - t) / 0.8)
        v = math.sin(2 * math.pi * f * t) + math.sin(2 * math.pi * f * 1.004 * t)
        out.append(amp * env * v * 0.5)
    return out


def flute(rate, f, length, amp=1.0):
    """a sine with a breathy start and a little vibrato"""
    n = int(rate * length)
    out, ph = [], 0.0
    for k in range(n):
        t = k / rate
        vib = 1 + 0.004 * math.sin(2 * math.pi * 5.2 * t) * min(1.0, t / 0.3)
        ph += 2 * math.pi * f * vib / rate
        env = min(1.0, t / 0.08) * min(1.0, (length - t) / 0.15)
        out.append(amp * env * (math.sin(ph) + 0.15 * math.sin(2 * ph)))
    return out


def moonrise(path):
    rate, beat = 22050, 60 / 84
    chords = [("A3", "C4", "E4"), ("F3", "A3", "C4"), ("C3", "E3", "G3"), ("G3", "B3", "D4")]
    t = Track(rate, 4 * 4 * beat + 4.5)
    for c, notes in enumerate(chords):
        t0 = c * 4 * beat
        root = notes[0]
        for i, nm in enumerate(notes):
            t.add(t0, pad(rate, hz(nm) / 2, 4 * beat + 0.4, 0.10), pan=0.3 + 0.2 * i)
        # an arpeggio in quavers over two octaves
        f = [hz(n) for n in notes]
        seq = [f[0], f[1], f[2], f[0] * 2, f[1] * 2, f[2], f[1], f[2]]
        for i, fr in enumerate(seq):
            t.add(t0 + i * beat / 2, bell(rate, fr, 1.6, 0.22), pan=0.25 if i % 2 else 0.75)
    melody = [("E5", 0, 1.5), ("D5", 1.5, 0.5), ("C5", 2, 2),
              ("C5", 4, 1), ("A4", 5, 1), ("C5", 6, 2),
              ("G4", 8, 1), ("E5", 9, 2), ("D5", 11, 1),
              ("D5", 12, 1.5), ("B4", 13.5, 0.5), ("G4", 14, 2)]
    for nm, at, ln in melody:
        t.add(at * beat, flute(rate, hz(nm), ln * beat * 0.95, 0.30), pan=0.5)
    # the end: A minor once more, left to ring
    end = 16 * beat
    for i, nm in enumerate(("A3", "C4", "E4", "A4")):
        t.add(end + i * 0.12, bell(rate, hz(nm), 4.0, 0.3, decay=1.2), pan=0.3 + 0.13 * i)
        t.add(end, pad(rate, hz(nm) / 2, 4.0, 0.08), pan=0.5)
    t.add(end, flute(rate, hz("A4"), 3.2, 0.3))
    t.write(path)


def square(rate, f, length, amp, duty=0.25):
    n = int(rate * length)
    period = rate / f
    return [amp * (1 if (k % period) < duty * period else -1) * min(1.0, (n - k) / (0.01 * rate)) for k in range(n)]


def triangle(rate, f, length, amp):
    n = int(rate * length)
    out = []
    for k in range(n):
        p = (k * f / rate) % 1.0
        out.append(amp * (4 * p - 1 if p < 0.5 else 3 - 4 * p) * min(1.0, (n - k) / (0.01 * rate)))
    return out


def night_bus(path):
    rate, beat = 16000, 60 / 132
    random.seed(7)
    bars = 8
    t = Track(rate, bars * 4 * beat + 0.6, stereo=False)
    bass = ["C3", "A2", "F2", "G2"]
    for bar in range(bars):
        root = hz(bass[bar % 4])
        for q in range(8):  # quavers: root, root, fifth, root ...
            f = root * (1.5 if q in (2, 6) else 2 if q == 7 else 1)
            t.add((bar * 4 + q / 2) * beat, triangle(rate, f, beat / 2 * 0.9, 0.45))
        for q in range(4):  # a tick of noise on each off-beat
            n = int(0.025 * rate)
            t.add((bar * 4 + q + 0.5) * beat, [0.18 * random.uniform(-1, 1) * (1 - k / n) for k in range(n)])
    tune = [("E5", 0, 1), ("G5", 1, 1), ("C6", 2, 1.5), ("B5", 3.5, 0.5),
            ("A5", 4, 1), ("E5", 5, 1), ("C5", 6, 2),
            ("F5", 8, 1), ("A5", 9, 1), ("C6", 10, 1), ("A5", 11, 1),
            ("G5", 12, 1.5), ("F5", 13.5, 0.5), ("D5", 14, 1), ("B4", 15, 1)]
    for rep in range(2):
        for nm, at, ln in tune:
            t.add((rep * 16 + at) * beat, square(rate, hz(nm), ln * beat * 0.85, 0.22))
    t.add(bars * 4 * beat, square(rate, hz("C5"), 0.5, 0.22))
    t.add(bars * 4 * beat, triangle(rate, hz("C3"), 0.5, 0.45))
    t.write(path, bits=8, peak=0.9)


def chime(path):
    rate = 48000
    t = Track(rate, 2.6)
    for i, (nm, pan) in enumerate((("G5", 0.3), ("C6", 0.5), ("E6", 0.7))):
        f, n = hz(nm), int(rate * 2.2)
        s = []
        for k in range(n):
            tt = k / rate
            a = min(1.0, k / (0.002 * rate))
            v = math.sin(2 * math.pi * f * tt) * math.exp(-2.2 * tt)
            v += 0.4 * math.sin(2 * math.pi * f * 2.76 * tt) * math.exp(-5 * tt)  # a bell's inharmonic partials
            v += 0.2 * math.sin(2 * math.pi * f * 5.4 * tt) * math.exp(-9 * tt)
            s.append(a * v * 0.3)
        t.add(i * 0.18, s, pan)
    t.write(path, peak=0.7)


def main():
    out = os.path.join(sys.argv[1], "home", "Music")
    os.makedirs(out, exist_ok=True)
    moonrise(os.path.join(out, "Moonrise.wav"))
    night_bus(os.path.join(out, "Night Bus.wav"))
    chime(os.path.join(out, "Chime.wav"))


main()
