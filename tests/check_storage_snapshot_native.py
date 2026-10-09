"""Actual storage backend plus real isolated host files and bounded I/O faults."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
include = out / "storage-native-include"
include.mkdir(parents=True, exist_ok=True)
(include / "webstorage.h").write_text((root / "user/include/webstorage.h").read_text(encoding="utf-8"), encoding="utf-8")
(include / "nocturne.h").write_text("#pragma once\n", encoding="utf-8")
(include / "fcntl.h").write_text("#pragma once\n#define O_RDONLY 0\n#define O_WRONLY 1\n#define O_CREAT 0x40\n#define O_TRUNC 0x200\n", encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH", ""))
exe = out / "storage-snapshot-native.exe"
subprocess.run([str(cc), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-function",
                "-I"+str(include), "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src",
                "tests/storage_snapshot_native_cases.c", "third_party/bearssl/src/hash/sha2small.c",
                "third_party/bearssl/src/codec/enc32be.c", "third_party/bearssl/src/codec/dec32be.c",
                "-o", str(exe)], cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=30)
