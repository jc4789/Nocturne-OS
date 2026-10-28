"""The URL record's owner built for host checks: user/libc/web/web_url.c on Lexbor's URL parser
(with web_encoding.c, util.c and ports/lexbor/memory.c). Every native layer that is given a URL
reads it through user/include/weburl.h (the request layer, the cookie jar, the network and fetch
processes, media, the storage layers, the engine), so a check that builds one of those layers
links these objects as the programs do (the Makefile's LIBC_LINK).

objects() keeps the objects in build/url-owner between checks: Lexbor's are built again when their
source changes, the owner's own every time (they read many of the engine's headers)."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import subprocess

MODULES = ("core", "encoding", "punycode", "unicode", "url")
FLAGS = ["-std=gnu11", "-O1", "-w", "-DLEXBOR_STATIC", "-Ithird_party/lexbor/source", "-Iports/lexbor/include",
         "-Iuser/include", "-Icommon", "-Iuser/libc", "-Iuser/libc/web"]


def clang(root):
    """The host's clang: the repository's tools, or the main tree's from a git worktree."""
    for base in (root, *root.parents):
        cc = base / "tools/msys64/ucrt64/bin/clang.exe"
        if cc.exists():
            return cc
    raise SystemExit("tools/msys64/ucrt64/bin/clang.exe is missing")


def lexbor_sources(root):
    return [line.split("+=")[1].strip()
            for line in (root / "third_party/lexbor/sources.mk").read_text("utf-8").splitlines()
            if "+=" in line and line.startswith("LXB_VENDOR_C") and any(f"/lexbor/{m}/" in line for m in MODULES)]


def objects(root, cc=None, util=True):
    """The owner's objects, to link after a check's own sources. util=False leaves out
    user/libc/web/util.c for a check that builds it itself."""
    root = Path(root)
    cc = cc or clang(root)
    env = dict(os.environ, PATH=str(Path(cc).parent) + os.pathsep + os.environ.get("PATH", ""))
    out = root / "build/url-owner"
    out.mkdir(parents=True, exist_ok=True)
    owner = ["user/libc/web/web_url.c", "user/libc/web/web_encoding.c", "ports/lexbor/memory.c"]
    if util:
        owner.append("user/libc/web/util.c")
    jobs = []
    result = []
    for source in owner + lexbor_sources(root):
        obj = out / (source.replace("/", "_") + ".o")
        result.append(str(obj))
        if source in owner or not obj.exists() or obj.stat().st_mtime < (root / source).stat().st_mtime:
            jobs.append([str(cc), *FLAGS, "-c", source, "-o", str(obj)])

    def build(command):
        return subprocess.run(command, cwd=root, env=env, capture_output=True, text=True)

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for command, done in zip(jobs, pool.map(build, jobs)):
            if done.returncode:
                raise SystemExit(f"{' '.join(command)}\n{done.stdout}{done.stderr}")
    return result
