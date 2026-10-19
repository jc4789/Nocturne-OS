#!/bin/sh
# Usage: tests/fuzz/browser/build_image.sh <outdir>   (repo root, Git Bash)
set -e
OUT=${1:-fuzz-out}
CL="/c/Program Files/LLVM/bin/clang.exe"
"$CL" -w -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -D_SSIZE_T_DEFINED=1 -D_CRT_SECURE_NO_WARNINGS \
  -idirafter user/include -Icommon -Ithird_party/img -include tests/fuzz/browser/host_compat.h \
  tests/fuzz/browser/fuzz_image.c user/libc/image.c user/libc/third_party_img.c -o "$OUT/fuzz_image.exe"
