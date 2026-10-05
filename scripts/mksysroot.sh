#!/bin/bash
# Stage the C development files that go into the initrd, for TinyCC running inside Nocturne:
#   /usr/include            libc, Nocturne and BearSSL headers (+ stdint.h, limits.h)
#   /usr/lib/tcc/include    TinyCC's own headers (stdarg.h, stddef.h, float.h, ...)
#   /usr/lib/tcc            libtcc1.a (compiler runtime), runmain.o (for tcc -run)
#   /usr/lib                crt1.o crti.o crtn.o libc.a (libc + gfx + BearSSL)
#   /usr/src/apps           sources of the programs in /bin (API examples)
# Objects are copied without debug info or unwind tables, which TinyCC has no use for.
# usage: mksysroot.sh OUTDIR LIBC_OBJS... -- TCCRT_OBJS...
set -e
S=$1
shift
T=third_party/tinycc
W=$S.tmp
rm -rf "$S" "$W"
mkdir -p "$S/usr/include/sys" "$S/usr/lib/tcc/include" "$W/libc" "$W/rt"

cp user/include/*.h "$S/usr/include/"
cp user/include/sys/*.h "$S/usr/include/sys/"
cp common/abi.h common/gfx.h "$S/usr/include/"
cp ports/tcc/include/*.h "$S/usr/include/"
cp third_party/bearssl/inc/*.h "$S/usr/include/"
cp $T/include/*.h "$S/usr/lib/tcc/include/"
# the sources of the programs in /bin, as examples of the APIs
mkdir -p "$S/usr/src/apps"
cp user/apps/*.c "$S/usr/src/apps/"

strip_obj() { objcopy --strip-debug -R .eh_frame -R .llvm_addrsig -R .comment "$1" "$2"; }

dest=libc
crt0=
for o in "$@"; do
    if [ "$o" = "--" ]; then dest=rt; continue; fi
    case "$o" in
        *crt0.asm.o) crt0=$o; continue ;;
    esac
    n=$(basename "$o" .o)
    n=${n%.c}
    n=${n%.S}
    strip_obj "$o" "$W/$dest/$n.o"
done
strip_obj "$crt0" "$S/usr/lib/crt1.o"
# TinyCC links crti.o/crtn.o around every program; Nocturne needs nothing there
printf '' > "$W/empty.asm"
nasm -f elf64 "$W/empty.asm" -o "$S/usr/lib/crti.o"
cp "$S/usr/lib/crti.o" "$S/usr/lib/crtn.o"

(cd "$W/libc" && ar rcs ../libc.a *.o)
mv "$W/libc.a" "$S/usr/lib/libc.a"
mv "$W/rt/runmain.o" "$S/usr/lib/tcc/runmain.o"
(cd "$W/rt" && ar rcs ../libtcc1.a *.o)
mv "$W/libtcc1.a" "$S/usr/lib/tcc/libtcc1.a"
rm -rf "$W"
