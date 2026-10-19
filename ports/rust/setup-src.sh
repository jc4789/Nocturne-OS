#!/bin/sh
# Prepares the Rust standard library source for Nocturne in tools/rust/src:
# unpacks rust-src 1.98.1 (downloaded if it is not in tools/rust/dl) and
# applies ports/rust/nocturne.patch. The licence files of the tarball are
# kept in tools/rust/licenses for the image.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
VERSION=1.98.1
TOOLS=$ROOT/tools/rust
TARBALL=${RUST_SRC_TARBALL:-$TOOLS/dl/rust-src-$VERSION.tar.xz}

if [ -e "$TOOLS/src/library" ]; then
    echo "tools/rust/src/library exists already; remove it to start again" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    curl -fL -o "$TARBALL" "https://static.rust-lang.org/dist/rust-src-$VERSION.tar.xz"
fi

WORK=$TOOLS/ex
rm -rf "$WORK"
mkdir -p "$WORK" "$TOOLS/src" "$TOOLS/licenses"
tar -xf "$TARBALL" -C "$WORK"
PKG=$WORK/rust-src-$VERSION
mv "$PKG/rust-src/lib/rustlib/src/rust/library" "$TOOLS/src/library"
cp "$PKG/COPYRIGHT" "$PKG/LICENSE-APACHE" "$PKG/LICENSE-MIT" "$TOOLS/licenses/"
rm -rf "$WORK"

cd "$ROOT"
git apply --directory=tools/rust/src "$HERE/nocturne.patch"
echo "tools/rust/src is ready; next: ports/rust/build-std.sh"
