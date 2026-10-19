#!/bin/sh
# Build one harness:  tests/fuzz/kernel/build.sh <name> [extra product .c files...]
# Output goes to $FUZZ_OUT (default: ./out). Uses the LLVM install on the host
# (Windows: C:/Program Files/LLVM; elsewhere: clang from PATH). Never touches build/.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=${FUZZ_OUT:-$here/out}
CLANG=${CLANG:-"/c/Program Files/LLVM/bin/clang.exe"}
[ -x "$CLANG" ] || CLANG=clang
name=$1; shift
mkdir -p "$out"
SAN=${SAN:-fuzzer,address,undefined}
"$CLANG" -fsanitize=$SAN -g -O1 -w -std=gnu11 -fno-sanitize-recover=undefined -fno-sanitize=alignment \
    -include "$here/fz.h" -I"$root/kernel/src" -I"$root/common" -I"$root/kernel/src/net" -I"$here" \
    "$here/$name.c" "$here/stubs_core.c" ${EXTRA:+$EXTRA} "$@" -o "$out/$name.exe"
echo "built $out/$name.exe"
