#!/usr/bin/env python3
"""Pack one or more directory trees into a USTAR initrd (later trees win)."""
import io
import os
import sys
import tarfile


def main():
    out = sys.argv[1]
    files = {}
    dirs = set()
    for root in sys.argv[2:]:
        if not os.path.isdir(root):
            continue
        for dp, dn, fn in os.walk(root):
            rel = os.path.relpath(dp, root).replace("\\", "/")
            if rel != ".":
                dirs.add(rel)
            for f in fn:
                p = os.path.join(dp, f)
                r = (f if rel == "." else rel + "/" + f)
                files[r] = p
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        for d in sorted(dirs):
            ti = tarfile.TarInfo(d)
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            tar.addfile(ti)
        for r in sorted(files):
            with open(files[r], "rb") as fh:
                data = fh.read()
            ti = tarfile.TarInfo(r)
            ti.size = len(data)
            ti.mode = 0o755 if r.startswith("bin/") else 0o644
            tar.addfile(ti, io.BytesIO(data))


main()
