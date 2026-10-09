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
build_root=$(realpath -e -- build)
for target in "$S" "$W"; do
    absolute=$(realpath -m -- "$target")
    case "$absolute" in
        "$build_root"/*) ;;
        *) printf 'sysrootの削除対象がworkspaceのbuild外です: %s\n' "$absolute" >&2; exit 1 ;;
    esac
done
rm -rf "$S" "$W"
mkdir -p "$S/usr/include/sys" "$S/usr/lib/tcc/include" "$W/libc" "$W/rt"

cp user/include/*.h "$S/usr/include/"
cp user/include/sys/*.h "$S/usr/include/sys/"
cp common/abi.h common/gfx.h common/gpu_abi.h "$S/usr/include/"
cp ports/tcc/include/*.h "$S/usr/include/"
cp third_party/bearssl/inc/*.h "$S/usr/include/"
cp ports/lexbor/include/*.h "$S/usr/include/"
# Preserve Lexbor's include tree, but do not stage its C sources in /usr/include.
while IFS= read -r h; do
    relative=${h#third_party/lexbor/source/}
    mkdir -p "$S/usr/include/$(dirname "$relative")"
    cp "$h" "$S/usr/include/$relative"
done < third_party/lexbor/headers.list
mkdir -p "$S/usr/share/licenses/lexbor"
cp third_party/lexbor/LICENSE third_party/lexbor/NOTICE third_party/lexbor/UPSTREAM.json "$S/usr/share/licenses/lexbor/"
mkdir -p "$S/usr/share/licenses/ffmpeg"
cp third_party/ffmpeg/LICENSE.md third_party/ffmpeg/COPYING.LGPLv2.1 third_party/ffmpeg/manifest.json third_party/ffmpeg/README.nocturne.md "$S/usr/share/licenses/ffmpeg/"
mkdir -p "$S/usr/share/licenses/unifont"
cp third_party/unifont/OFL-1.1.txt third_party/unifont/NOTICE.txt third_party/unifont/manifest.json "$S/usr/share/licenses/unifont/"
cp $T/include/*.h "$S/usr/lib/tcc/include/"
# the sources of the programs in /bin, as examples of the APIs
mkdir -p "$S/usr/src/apps"
cp user/apps/*.c "$S/usr/src/apps/"

strip_obj() { objcopy --strip-debug -R .eh_frame -R .llvm_addrsig -R .comment "$1" "$2"; }

dest=libc
crt0=
i=0
objects=()
for argument in "$@"; do
    if [[ "$argument" == @* ]]; then
        read -r -a batch < "${argument#@}"
        objects+=("${batch[@]}")
    else
        objects+=("$argument")
    fi
done
for o in "${objects[@]}"; do
    if [ "$o" = "--" ]; then dest=rt; continue; fi
    case "$o" in
        *crt0.asm.o) crt0=$o; continue ;;
    esac
    n=$(basename "$o" .o)
    n=${n%.c}
    n=${n%.S}
    # Lexbor modules (and libc) have repeated source basenames. Archive members
    # must be unique or one module silently replaces another before archiving.
    if [ "$dest" = libc ]; then i=$((i + 1)); n=libc_$i; fi
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
