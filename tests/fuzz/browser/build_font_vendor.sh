#!/bin/sh
# Compiles FreeType (memory-only subset, as in the Makefile's FT_C) and brotli once, for the font harness.
# Usage: tests/fuzz/browser/build_font_vendor.sh <outdir>   (repo root, Git Bash)
set -e
OUT=${1:-fuzz-out}/ftobj
CL="/c/Program Files/LLVM/bin/clang.exe"
mkdir -p "$OUT"
FT="base/ftbase.c base/ftinit.c base/ftdebug.c base/ftmm.c base/ftbitmap.c truetype/truetype.c cff/cff.c psaux/psaux.c psnames/psnames.c pshinter/pshinter.c sfnt/sfnt.c smooth/smooth.c gzip/ftgzip.c"
FLAGS="-c -O1 -g -w -fsanitize=address,fuzzer-no-link -D_SSIZE_T_DEFINED=1 -DFT2_BUILD_LIBRARY -Iports/freetype -Ithird_party/freetype/include -Ithird_party/brotli/c/include -DBROTLI_BUILD_PORTABLE"
for f in $FT; do
  "$CL" $FLAGS '-DFT_CONFIG_OPTIONS_H="nocturne-ftoption.h"' '-DFT_CONFIG_MODULES_H="nocturne-ftmodule.h"' third_party/freetype/src/$f -o "$OUT/ft_$(echo $f | tr '/' '_').o"
done
for f in third_party/brotli/c/common/*.c third_party/brotli/c/dec/*.c; do
  "$CL" $FLAGS $f -o "$OUT/br_$(basename $f).o"
done
