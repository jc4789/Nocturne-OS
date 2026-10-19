#!/bin/sh
# Compiles and links one Rust program for Nocturne:
#   ports/rust/rustc-nocturne.sh <main.rs> <output> [more rustc arguments]
# Needs the sysroot from build-std.sh and the C library objects of the last
# `make` (build/u/user/libc/crt0.asm.o and build/libc-objects.list). The result
# is a static x86-64 ELF at 0x400000, linked like the C apps in the Makefile
# (user/user.ld, the libc objects pulled in with --start-lib).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
winpath() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }

SRC=$1
OUT=$2
shift 2

SYSROOT=${RUST_SYSROOT:-$ROOT/tools/rust/sysroot}
BUILD=${BUILD:-build}
LD=${RUST_LD:-ld.lld}
# rustc starts the linker itself; on Windows it needs the real program name.
if command -v "$LD.exe" >/dev/null 2>&1; then LD=$(command -v "$LD.exe"); fi

cd "$ROOT"
mkdir -p "$(dirname "$OUT")"
export RUSTC_BOOTSTRAP=1
exec rustc --edition 2021 -Zunstable-options \
    --target "$(winpath "$HERE")/x86_64-unknown-nocturne.json" \
    --sysroot "$(winpath "$SYSROOT")" \
    -C opt-level=2 -C panic=abort -C "linker=$LD" \
    -C "link-arg=$BUILD/u/user/libc/crt0.asm.o" \
    -C link-arg=-T -C link-arg=user/user.ld -C link-arg=--gc-sections \
    -C link-arg=--start-lib -C "link-arg=@$BUILD/libc-objects.list" -C link-arg=--end-lib \
    --crate-type bin --crate-name "$(basename "$(dirname "$SRC")")" \
    "$SRC" -o "$OUT" "$@"
