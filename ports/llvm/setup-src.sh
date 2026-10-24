#!/bin/sh
# Prepares LLVM 23.1.3 for Nocturne in tools/llvm/src: the sources of
# llvmorg-23.1.3 that clang, lld and the C++ runtimes are built from, taken
# from a checkout of llvm-project (LLVM_PROJECT) or the release's source
# tarball (downloaded if it is not in tools/llvm/dl), with
# ports/llvm/nocturne.patch applied. tools/llvm/src is a git repository of its
# own whose commit is the release, so `git diff` there gives the patch again.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
VERSION=23.1.3
COMMIT=0d261d1ca552c95a8f007e061c787ac7132fbcbc
TOOLS=$ROOT/tools/llvm
SRC=$TOOLS/src
TARBALL=${LLVM_SRC_TARBALL:-$TOOLS/dl/llvm-project-$VERSION.src.tar.xz}
# Tests, documentation and benchmarks are not built; lld's docs stay, as its
# CMake reads them.
PARTS="llvm clang lld cmake third-party libc libcxx libcxxabi libunwind compiler-rt runtimes"
LEAVE="llvm/test clang/test lld/test libc/test libcxx/test libcxxabi/test libunwind/test
compiler-rt/test llvm/unittests clang/unittests lld/unittests llvm/docs clang/docs
libunwind/docs llvm/utils/gn clang/www third-party/benchmark libc/benchmarks libc/utils/gpu"

if [ -e "$SRC" ]; then
    echo "tools/llvm/src exists already; remove it to start again" >&2
    exit 1
fi
mkdir -p "$SRC"
if [ -n "${LLVM_PROJECT:-}" ]; then
    EXCLUDE=
    for p in $LEAVE; do EXCLUDE="$EXCLUDE :(exclude)$p"; done
    # shellcheck disable=SC2086
    git -c core.autocrlf=false -C "$LLVM_PROJECT" archive --format=tar "$COMMIT" -- \
        $PARTS $EXCLUDE | tar -x -C "$SRC"
else
    if [ ! -f "$TARBALL" ]; then
        mkdir -p "$(dirname "$TARBALL")"
        curl -fL -o "$TARBALL" \
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-$VERSION/llvm-project-$VERSION.src.tar.xz"
    fi
    WORK=$TOOLS/ex
    rm -rf "$WORK"
    mkdir -p "$WORK"
    tar -xf "$TARBALL" -C "$WORK"
    PKG=$WORK/llvm-project-$VERSION.src
    for p in $LEAVE; do rm -rf "${PKG:?}/$p"; done
    for p in $PARTS; do mv "$PKG/$p" "$SRC/$p"; done
    rm -rf "$WORK"
fi

cd "$SRC"
git init -q
git add -A
git -c user.name=nocturne -c user.email=nocturne@localhost \
    commit -q -m "llvmorg-$VERSION ($COMMIT)"
git apply "$HERE/nocturne.patch"
echo "tools/llvm/src is ready; next: python ports/llvm/build.py host"
