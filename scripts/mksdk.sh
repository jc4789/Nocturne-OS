#!/bin/bash
# Stage the SDK, C:\Nocturne\SDK: the C development files for TinyCC and clang running inside
# Nocturne.
#   Include              libc, Nocturne, BearSSL, QuickJS and libnvrm headers (+ stdint.h, limits.h)
#   Lib\tcc\include      TinyCC's own headers (stdarg.h, stddef.h, float.h, ...)
#   Lib\tcc              libtcc1.a (compiler runtime), runmain.o (for tcc -run)
#   Lib                  crt1.o crti.o crtn.o libc.a (libc + gfx + BearSSL), libwasm-rt.a,
#                        user.ld (the program layout clang's driver links with)
#   Source\Apps          sources of the system's programs (API examples)
#   Source\libc          the private libc headers those sources include by their place
#                        in the tree ("../libc/web/js_memory.h")
#   Source\wasm2c        wasm2c's runtime, for programs it makes
# and, with --llvm, what the Nocturne build of LLVM puts there (ports/llvm/build.py):
#   Lib\clang            clang's headers (include) and runtime (lib\x86_64-unknown-nocturne)
#   Include\c++\v1       libc++'s headers
#   Include              libunwind's headers (unwind.h, libunwind.h)
#   Lib                  libc++.a (libc++ with libc++abi), libc++experimental.a, libunwind.a
#   Source\c++\v1        libc++'s std and std.compat modules, for import std
# Objects are copied without debug info or unwind tables, which TinyCC has no use for.
# usage: mksdk.sh OUTDIR [--llvm LLVM_SDK] LIBC_OBJS... -- TCCRT_OBJS... --wasmrt WASMRT_OBJS...
set -e
S=$1
shift
L=
if [ "${1:-}" = --llvm ]; then
    L=$2
    shift 2
fi
T=third_party/tinycc
W=$S.tmp
build_root=$(realpath -e -- build)
for target in "$S" "$W"; do
    absolute=$(realpath -m -- "$target")
    case "$absolute" in
        "$build_root"/*) ;;
        *) printf 'SDKの削除対象がworkspaceのbuild外です: %s\n' "$absolute" >&2; exit 1 ;;
    esac
done
rm -rf "$S" "$W"
mkdir -p "$S/Include" "$S/Lib/tcc/include" "$S/Source/Apps" "$S/Source/wasm2c" "$W/libc" "$W/rt" "$W/wasmrt"

cp user/include/*.h "$S/Include/"
cp common/abi.h common/gfx.h common/gpu_abi.h common/display_abi.h "$S/Include/"
cp ports/tcc/include/*.h "$S/Include/"
cp third_party/bearssl/inc/*.h "$S/Include/"
cp ports/lexbor/include/*.h "$S/Include/"
# QuickJS and libnvrm are libc.a members like BearSSL and Lexbor
cp third_party/quickjs/quickjs.h "$S/Include/"
cp ports/nvidia/include/*.h "$S/Include/"
# wasm2c's regular-memory runtime uses Nocturne-native trap/TLS/stack handling.
# Its compiler-host symbol binder is not a member of the standalone runtime.
cp third_party/wabt/wasm2c/wasm-rt.h third_party/wabt/wasm2c/wasm-rt-impl.h "$S/Include/"
cp ports/wabt/include/wasm-rt-nocturne.h ports/wabt/include/wasm-rt-host.h "$S/Include/"
cp third_party/wabt/wasm2c/wasm-rt-impl.c third_party/wabt/wasm2c/wasm-rt-mem-impl.c \
   third_party/wabt/wasm2c/wasm-rt-mem-impl-helper.inc third_party/wabt/wasm2c/wasm-rt-impl-tableops.inc \
   ports/wabt/wasm_rt_nocturne.c "$S/Source/wasm2c/"
# Preserve Lexbor's include tree, but do not stage its C sources in Include.
while IFS= read -r h; do
    relative=${h#third_party/lexbor/source/}
    mkdir -p "$S/Include/$(dirname "$relative")"
    cp "$h" "$S/Include/$relative"
done < third_party/lexbor/headers.list
cp $T/include/*.h "$S/Lib/tcc/include/"
# the sources of the system's programs, as examples of the APIs
cp user/apps/*.c user/apps/*.h user/apps/*.js user/apps/*.ts "$S/Source/Apps/"
# and the private libc files they #include or #embed by a relative path, with the ones those
# include, at the same place beside Source\Apps (user/libc/web/x.h -> Source\libc\web\x.h)
declare -A staged
pending=(user/apps/*.c user/apps/*.h)
while ((${#pending[@]})); do
    f=${pending[-1]}
    unset 'pending[-1]'
    while read -r directive included; do
        # a program's own files stay in Source\Apps
        [[ "$f" == user/libc/* || "$included" == ../* ]] || continue
        h=$(realpath -m --relative-to=. -- "${f%/*}/$included")
        [[ "$h" == user/libc/* && -f "$h" && -z "${staged[$h]}" ]] || continue
        staged[$h]=1
        # an #embed'ded file is data
        [[ "$directive" == embed ]] || pending+=("$h")
        mkdir -p "$S/Source/$(dirname "${h#user/}")"
        cp "$h" "$S/Source/${h#user/}"
    done < <(sed -n 's/^[[:space:]]*#[[:space:]]*\(include\|embed\)[[:space:]]*"\([^"]*\)".*/\1 \2/p' "$f")
done

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
    if [ "$o" = "--wasmrt" ]; then dest=wasmrt; continue; fi
    case "$o" in
        *crt0.asm.o) crt0=$o; continue ;;
        *.a)
            # a static library made by another compiler (NVK's Rust archive): its members join
            # libc.a one by one, so TinyCC pulls only the ones a program needs
            archive=$(realpath -e -- "$o")
            rm -rf "$W/ar" && mkdir -p "$W/ar"
            (cd "$W/ar" && ar x "$archive")
            for m in "$W/ar"/*; do
                i=$((i + 1))
                strip_obj "$m" "$W/$dest/libc_$i.o"
            done
            rm -rf "$W/ar"
            continue ;;
    esac
    n=$(basename "$o" .o)
    n=${n%.c}
    n=${n%.S}
    # Lexbor modules (and libc) have repeated source basenames. Archive members
    # must be unique or one module silently replaces another before archiving.
    if [ "$dest" = libc ]; then i=$((i + 1)); n=libc_$i; fi
    strip_obj "$o" "$W/$dest/$n.o"
done
strip_obj "$crt0" "$S/Lib/crt1.o"
# TinyCC links crti.o/crtn.o around every program; Nocturne needs nothing there
printf '' > "$W/empty.asm"
nasm -f elf64 "$W/empty.asm" -o "$S/Lib/crti.o"
cp "$S/Lib/crti.o" "$S/Lib/crtn.o"
cp user/user.ld "$S/Lib/user.ld"

(cd "$W/libc" && ar rcs ../libc.a *.o)
mv "$W/libc.a" "$S/Lib/libc.a"
(cd "$W/wasmrt" && ar rcs ../libwasm-rt.a *.o)
mv "$W/libwasm-rt.a" "$S/Lib/libwasm-rt.a"
mv "$W/rt/runmain.o" "$S/Lib/tcc/runmain.o"
(cd "$W/rt" && ar rcs ../libtcc1.a *.o)
mv "$W/libtcc1.a" "$S/Lib/tcc/libtcc1.a"
rm -rf "$W"
if [ -n "$L" ]; then
    cp -R "$L"/. "$S/"
fi
