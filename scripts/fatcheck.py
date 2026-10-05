#!/usr/bin/env python3
"""Check a Nocturne data disk image (FAT32 at 1 MiB): chain lengths match file sizes, no cluster
is used twice, both FAT copies agree. usage: fatcheck.py [build/data.img]"""
import struct
import sys

img = sys.argv[1] if len(sys.argv) > 1 else "build/data.img"
f = open(img, "rb")
BASE = 1048576
f.seek(BASE)
bs = f.read(512)
bps, spc, res, nfats = struct.unpack_from("<HBHB", bs, 11)
fatsz, root = struct.unpack_from("<I", bs, 36)[0], struct.unpack_from("<I", bs, 44)[0]
f.seek(BASE + res * bps)
fat = f.read(fatsz * bps)
fat2 = f.read(fatsz * bps)
data = BASE + (res + nfats * fatsz) * bps
cb = bps * spc
problems = []
if nfats > 1 and fat != fat2:
    problems.append("the two FAT copies differ")
nent = len(fat) // 4
get = lambda c: struct.unpack_from("<I", fat, c * 4)[0] & 0x0FFFFFFF
owner = {}


def chain(c, who):
    out = []
    while 2 <= c < 0x0FFFFFF8:
        if c >= nent:
            problems.append("%s: cluster %d out of range" % (who, c))
            break
        if c in owner:
            problems.append("%s: cluster %d also used by %s" % (who, c, owner[c]))
            break
        owner[c] = who
        out.append(c)
        c = get(c)
    return out


def walk(c, path):
    buf = b""
    for x in chain(c, path or "/"):
        f.seek(data + (x - 2) * cb)
        buf += f.read(cb)
    nfiles = 0
    for i in range(0, len(buf), 32):
        e = buf[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
            continue
        name = e[0:8].decode("latin1").rstrip() + ("." + e[8:11].decode("latin1").rstrip() if e[8:11].strip() else "")
        if name in (".", ".."):
            continue
        first = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        p = path + "/" + name
        if e[11] & 0x10:
            nfiles += walk(first, p)
        else:
            n = len(chain(first, p))
            if n != (size + cb - 1) // cb:
                problems.append("%s: %d bytes but %d clusters" % (p, size, n))
            nfiles += 1
    return nfiles


n = walk(root, "")
lost = sum(1 for c in range(2, nent) if get(c) and c not in owner)
if lost:
    problems.append("%d allocated clusters belong to no file" % lost)
print("%s: %d files, %s" % (img, n, "OK" if not problems else "%d problems" % len(problems)))
for p in problems[:40]:
    print("  " + p)
sys.exit(1 if problems else 0)
