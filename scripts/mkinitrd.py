#!/usr/bin/env python3
"""Pack one or more directory trees into a USTAR initrd (later trees win)."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import sys
import tarfile
import tempfile


NVIDIA_ARCHIVE_ROOT = "usr/share/firmware/nvidia"


def digest(path):
    with open(path, "rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def firmware_state(output, source, prefix):
    """Record the complete selected firmware inputs, including the disabled state."""
    archive = PurePosixPath(prefix)
    if archive.is_absolute() or ".." in archive.parts or not prefix.startswith(NVIDIA_ARCHIVE_ROOT + "/"):
        raise ValueError("NVIDIA firmware prefix must be inside " + NVIDIA_ARCHIVE_ROOT)
    selected = Path(source) if source else None
    state = {"source": str(selected.resolve()) if selected else "", "files": {}}
    # A checkout without optional local firmware remains a supported build. A partial
    # installation is not: do not package a GSP image without its ucodes and licence.
    if selected and (selected.exists() or selected.is_symlink()):
        for name in ("gsp_ga10x.bin", "ucodes_ga10x.bin", "LICENSE"):
            path = selected / (name if name == "LICENSE" else "firmware/" + name)
            state["files"][str(archive / name)] = {"source": str(path.resolve()), "sha256": digest(path)}
    data = (json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")
    out = Path(output)
    if out.exists() and out.read_bytes() == data:
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = out.with_name(out.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-state")
    parser.add_argument("--firmware-source", default="")
    parser.add_argument("--firmware-prefix", default=NVIDIA_ARCHIVE_ROOT + "/615.78.08")
    parser.add_argument("--firmware-manifest")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    if args.firmware_state:
        if args.paths or args.firmware_manifest:
            parser.error("firmware state generation and initrd packing are separate operations")
        firmware_state(args.firmware_state, args.firmware_source, args.firmware_prefix)
        return
    if not args.paths:
        parser.error("an output archive is required")
    out = args.paths[0]
    selected = None
    if args.firmware_manifest:
        with open(args.firmware_manifest, encoding="utf-8") as manifest:
            selected = json.load(manifest)["files"]
    files = {}
    dirs = set()
    for root in args.paths[1:]:
        if not os.path.isdir(root):
            continue
        for dp, dn, fn in os.walk(root):
            rel = os.path.relpath(dp, root).replace("\\", "/")
            if selected is not None:
                # Stale build/root copies must not override an explicitly empty selection.
                if rel == NVIDIA_ARCHIVE_ROOT or rel.startswith(NVIDIA_ARCHIVE_ROOT + "/"):
                    dn[:] = []
                    continue
            if rel != ".":
                dirs.add(rel)
            for f in fn:
                p = os.path.join(dp, f)
                r = (f if rel == "." else rel + "/" + f)
                files[r] = p
    for name, item in (selected or {}).items():
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts or not name.startswith(NVIDIA_ARCHIVE_ROOT + "/"):
            raise ValueError("invalid firmware archive path: " + name)
        if digest(item["source"]) != item["sha256"]:
            raise ValueError("firmware changed after selection: " + item["source"])
        files[name] = item["source"]
        dirs.update(str(parent) for parent in path.parents if str(parent) != ".")
    # A failed pack must not leave a newer, partial target that make would reuse.
    temporary = tempfile.NamedTemporaryFile(prefix=Path(out).name + ".", suffix=".tmp",
                                            dir=Path(out).parent, delete=False)
    try:
        with tarfile.open(fileobj=temporary, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for d in sorted(dirs):
                ti = tarfile.TarInfo(d)
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                tar.addfile(ti)
            for r in sorted(files):
                with open(files[r], "rb") as fh:
                    data = fh.read()
                if selected and r in selected and hashlib.sha256(data).hexdigest() != selected[r]["sha256"]:
                    raise ValueError("firmware changed while packing: " + files[r])
                ti = tarfile.TarInfo(r)
                ti.size = len(data)
                ti.mode = 0o755 if r.startswith("bin/") else 0o644
                tar.addfile(ti, io.BytesIO(data))
        temporary.close()
        os.replace(temporary.name, out)
    finally:
        temporary.close()
        if os.path.exists(temporary.name):
            os.unlink(temporary.name)


if __name__ == "__main__":
    main()
