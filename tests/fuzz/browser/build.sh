#!/bin/sh
# Usage: tests/fuzz/browser/build.sh <target> [outdir]   (run from the repo root, Git Bash)
# Builds tests/fuzz/browser/fuzz_<target>.c[pp] together with the product sources listed below.
set -e
T=$1
OUT=${2:-fuzz-out}
CL="/c/Program Files/LLVM/bin/clang.exe"
CXX="/c/Program Files/LLVM/bin/clang++.exe"
FLAGS="-w -D_CRT_SECURE_NO_WARNINGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 -idirafter user/include -Icommon -Itests/fuzz/browser -Itests/fuzz/browser/shim -Ithird_party/img -Ithird_party/brotli/c/include -DBROTLI_BUILD_PORTABLE -include host_compat.h"
# http.c decodes br through the same adapter and brotli decoder the product links.
HTTP="user/libc/http.c user/libc/http2.c user/libc/third_party_brotli.c third_party/brotli/c/common/*.c third_party/brotli/c/dec/*.c"
case $T in
  json)   SRC="user/libc/json.c" ;;
  cookie) SRC="user/libc/webcookie.c" ;;
  http)   SRC="$HTTP tests/fuzz/browser/net_stub.c" ;;
  mse)    SRC="user/libc/media_mse_parser.c" ;;
  adaptive) SRC="user/libc/media_adaptive_parser.c user/libc/media_http.c $HTTP tests/fuzz/browser/net_stub.c" ;;
  url)    SRC="$HTTP tests/fuzz/browser/net_stub.c" ;;
  *) echo "unknown target $T" >&2; exit 1 ;;
esac
mkdir -p "$OUT"
"$CL" $FLAGS tests/fuzz/browser/fuzz_$T.c $SRC -o "$OUT/fuzz_$T.exe"
