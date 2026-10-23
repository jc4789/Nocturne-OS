#!/bin/sh
# Builds Rust's standard library for x86_64-unknown-nocturne from the patched
# source in tools/rust/src (see README.md) and assembles a sysroot in
# tools/rust/sysroot. Run from anywhere; needs the stable 1.98.1 toolchain.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# Windows tools want Windows paths; Git Bash's `pwd -W` gives them.
winpath() { (cd "$1" && (pwd -W 2>/dev/null || pwd)); }

SRC=${RUST_SRC:-$ROOT/tools/rust/src}
SYSROOT=${RUST_SYSROOT:-$ROOT/tools/rust/sysroot}
TARGET_DIR=${RUST_TARGET_DIR:-$ROOT/tools/rust/target}
TARGET=x86_64-unknown-nocturne

LIB=$(winpath "$SRC/library")
JSON="$(winpath "$HERE")/$TARGET.json"

# std is a normal Cargo workspace, built by -Zbuild-std (the stable compiler is
# allowed to with RUSTC_BOOTSTRAP). The source tree is the rust-src of 1.98.1
# plus ports/rust/nocturne.patch; its vendor directory holds the dependencies.
export RUSTC_BOOTSTRAP=1
export __CARGO_TESTS_ONLY_SRC_ROOT="$LIB"

# Cargo does not look for edits in the standard library's source (to it the
# library is not a local package), so when the patch has changed since the
# last build the library is built again from the start.
mkdir -p "$TARGET_DIR"
BUILT_PATCH=$TARGET_DIR/nocturne.patch
if ! cmp -s "$HERE/nocturne.patch" "$BUILT_PATCH"; then
    rm -rf "$TARGET_DIR/$TARGET/release/.fingerprint"
fi

cargo build --offline --release \
    --manifest-path "$HERE/stdbuild/Cargo.toml" \
    --target-dir "$(winpath "$TARGET_DIR")" \
    --config 'source.crates-io.replace-with="vendored-sources"' \
    --config "source.vendored-sources.directory=\"$LIB/vendor\"" \
    -Zbuild-std=core,alloc,std,panic_abort -Zbuild-std-features= \
    -Zjson-target-spec --target "$JSON"
cp "$HERE/nocturne.patch" "$BUILT_PATCH"

# rustc finds the standard library in <sysroot>/lib/rustlib/<target>/lib.
DEST=$SYSROOT/lib/rustlib/$TARGET/lib
rm -rf "$DEST"
mkdir -p "$DEST"
for f in "$TARGET_DIR/$TARGET/release/deps/"*.rlib; do
    case "$f" in */libstdbuild-*) continue ;; esac
    cp "$f" "$DEST/"
done
echo "sysroot: $SYSROOT ($(ls "$DEST" | wc -l) libraries)"
