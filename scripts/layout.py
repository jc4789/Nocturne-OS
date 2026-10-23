#!/usr/bin/env python3
"""What goes where on drive C: of the boot disk.

  layout.py --firmware-state STATE --firmware-source DIR --firmware-version VERSION
      records the NVIDIA firmware chosen for the disk (none when DIR is empty or missing)
  layout.py [--firmware-manifest STATE] [--system] LIST
      writes the list `store fill` puts on C: (host/store/store.c) and prints the bytes it holds;
      --system lists only the system's part (C:\\Nocturne and the desktop apps' folders in
      C:\\Programs), not C:\\Users

C:\\Nocturne holds the system (rootfs/Nocturne, the licences, the SDK, the firmware, the trusted
certificates and every system program in System); C:\\Programs a folder for each desktop app, next
to the apps the user puts there; C:\\Users\\User the user's folder (rootfs/Users, the music made by
mksounds.py, diskfiles/). The system's part is protected (--system) and replaced whole by a fill:
what an earlier one put there goes first, so a disk keeps its user's files and apps when its system
is brought up to date.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path

BUILD = Path("build")
PROGRAMS = BUILD / "programs"
FIRMWARE_FILES = ("firmware/gsp_ga10x.bin", "firmware/ucodes_ga10x.bin", "LICENSE")
USER = "C:\\Users\\User"

# The desktop apps, each in its folder of C:\Programs (kernel/src/gui/wm.c starts them there).
APPS = {
    "browser": "Browser", "notepad": "Notepad", "paint": "Paint", "calc": "Calculator",
    "clock": "Clock", "piano": "Piano", "player": "Player", "mediaplayer": "Media Player",
    "snake": "Snake", "tetris": "Tetris", "mines": "Minesweeper", "mandel": "Mandelbrot",
    "cube": "Cube", "life": "Life", "agent": "Agent",
}


def digest(path):
    with open(path, "rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write_if_changed(path, data):
    """Leave an unchanged file alone, so that make sees no change."""
    out = Path(path)
    if out.exists() and out.read_bytes() == data:
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    temporary = out.with_name(out.name + ".tmp")
    temporary.write_bytes(data)
    os.replace(temporary, out)


def firmware_state(output, source, version):
    """Record the complete selected firmware, including that there is none."""
    selected = Path(source) if source else None
    state = {"source": str(selected.resolve()) if selected else "", "version": version, "files": {}}
    # A checkout without the optional local firmware is a supported build. A partial one is not:
    # a GSP image never goes without its ucodes and licence.
    if selected and (selected.exists() or selected.is_symlink()):
        for name in FIRMWARE_FILES:
            path = selected / name
            state["files"][Path(name).name] = {"source": str(path.resolve()), "sha256": digest(path)}
    write_if_changed(output, (json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8"))


def size_of(host):
    path = Path(host)
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def layout(firmware_manifest, system_only):
    """(path on C:, host file or folder, "-" for an empty folder or "!" to remove what is there,
    attributes) in order: a folder's attributes come before what goes into it."""
    system = "--system"
    apps = sorted({APPS[program.name] for program in PROGRAMS.iterdir() if program.name in APPS})
    entries = [("C:\\Nocturne", "!", "")] + [(f"C:\\Programs\\{app}", "!", "") for app in apps]
    entries += [
        ("C:\\Nocturne", "rootfs/Nocturne", system),
        ("C:\\Nocturne\\Licenses", str(BUILD / "licenses"), system),
        ("C:\\Nocturne\\SDK", str(BUILD / "sdk"), system),
        ("C:\\Nocturne\\Certificates\\ca-bundle.crt", "third_party/ca/ca-bundle.crt", system),
        ("C:\\Nocturne\\System", "-", system),
        ("C:\\Programs", "-", ""),
    ]
    entries += [(f"C:\\Programs\\{app}", "-", system) for app in apps]
    for program in sorted(PROGRAMS.iterdir()):
        app = APPS.get(program.name)
        place = f"C:\\Programs\\{app}" if app else "C:\\Nocturne\\System"
        entries.append((f"{place}\\{program.name}", str(program), system))
    if firmware_manifest:
        state = json.loads(Path(firmware_manifest).read_text(encoding="utf-8"))
        for name, item in sorted(state["files"].items()):
            if digest(item["source"]) != item["sha256"]:
                raise SystemExit("firmware changed after it was chosen: " + item["source"])
            entries.append((f"C:\\Nocturne\\Firmware\\NVIDIA\\{state['version']}\\{name}", item["source"], system))
    if system_only:
        return entries
    entries += [("C:\\Users", "rootfs/Users", ""), (USER + "\\Music", str(BUILD / "sounds"), "")]
    entries += [(f"{USER}\\{name}", "-", "") for name in ("Desktop", "Downloads", "Pictures", "Videos")]
    if Path("diskfiles").is_dir():
        entries.append((USER, "diskfiles", ""))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware-state")
    parser.add_argument("--firmware-source", default="")
    parser.add_argument("--firmware-version", default="")
    parser.add_argument("--firmware-manifest")
    parser.add_argument("--system", action="store_true")
    parser.add_argument("list", nargs="?")
    args = parser.parse_args()
    if args.firmware_state:
        if args.list or not args.firmware_version:
            parser.error("--firmware-state takes --firmware-source and --firmware-version, and no list")
        firmware_state(args.firmware_state, args.firmware_source, args.firmware_version)
        return
    if not args.list:
        parser.error("the list to write is missing")
    entries = layout(args.firmware_manifest, args.system)
    lines = ["# made by scripts/layout.py: a path on C:, the host's file or folder, its attributes"]
    lines += [f"{path}\t{host}\t{attributes}" for path, host, attributes in entries]
    write_if_changed(args.list, ("\n".join(lines) + "\n").encode("utf-8"))
    print(sum(size_of(host) for _, host, _ in entries if host not in ("-", "!")))


if __name__ == "__main__":
    main()
