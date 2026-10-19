#!/bin/sh
# Usage: tests/fuzz/browser/build_font.sh <outdir>   (after build_font_vendor.sh <outdir>)
set -e
OUT=${1:-fuzz-out}
CL="/c/Program Files/LLVM/bin/clang.exe"
"$CL" -w -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -D_SSIZE_T_DEFINED=1 -D_CRT_SECURE_NO_WARNINGS \
  -idirafter user/include -Icommon -Ithird_party/img -Ithird_party/freetype/include -Iports/freetype -Ithird_party/brotli/c/include \
  -DFT2_BUILD_LIBRARY -DFT_CONFIG_OPTIONS_H='"nocturne-ftoption.h"' -DFT_CONFIG_MODULES_H='"nocturne-ftmodule.h"' -DBROTLI_BUILD_PORTABLE \
  -include tests/fuzz/browser/host_compat.h \
  tests/fuzz/browser/fuzz_font.c user/libc/font.c user/libc/third_party_ttf.c "$OUT"/ftobj/*.o -o "$OUT/fuzz_font.exe"
