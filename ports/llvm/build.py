"""Nocturne の LLVM（clang・lld）を tools/llvm/src から作る。

  host      Windows で動き Nocturne 向けに作る clang・lld などを tools/llvm/host に置く。
            既定のターゲットは x86_64-unknown-nocturne。
  runtimes  host の clang で Nocturne 向けの実行時ライブラリーを作る。compiler-rt の builtins は
            host の clang のリソースディレクトリーへ、libunwind と libc++（libc++abi を含む）は
            tools/llvm/runtimes の Include と Lib へ入れ、build/sdk を写した tools/llvm/sysroot にも
            重ねる。先に make と host が要る。
  nocturne  host の clang で、Nocturne で動く clang・lld など（一つのプログラム llvm）を作る。
            tools/llvm/nocturne の System に llvm を、SDK に SDK へ加わる物（clang のヘッダーと
            builtins、C++ の実行時ライブラリー）を置く（ports/llvm/llvm.mk）。プログラムに結合する
            LLVM のライブラリー（llvmpipe の JIT の MCJIT）も build-nocturne/lib に作る。先に runtimes が要る。
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent.parent
LLVM = ROOT / 'tools' / 'llvm'
SOURCE = LLVM / 'src'
TRIPLE = 'x86_64-unknown-nocturne'
VCVARS = ('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/'
          'VC/Auxiliary/Build/vcvars64.bat')
# What the OS build and the Nocturne cross build use from the host toolchain.
HOST_COMPONENTS = ('clang', 'clang-resource-headers', 'lld', 'llvm-ar',
                   'llvm-ranlib', 'llvm-objcopy', 'llvm-strip', 'llvm-nm',
                   'llvm-readobj', 'llvm-readelf', 'llvm-objdump')
HOST_BIN = LLVM / 'host' / 'bin'
SDK = ROOT / 'build' / 'sdk'
SYSROOT = LLVM / 'sysroot'
# What the runtimes stage installs, in the SDK's layout: libunwind, libc++abi and libc++.
RUNTIMES = LLVM / 'runtimes'
TOOLCHAIN = ROOT / 'ports' / 'llvm' / 'cmake' / 'nocturne.cmake'
# Room for the sysroot copy, both build trees and what they install.
RUNTIMES_SPACE = 4 << 30
# What llvm, the one Nocturne program of the toolchain, is made of: each tool
# runs by the name it is run with (llvm-driver.cpp), and ports/llvm/llvm.mk
# gives each name its launcher.
NOCTURNE_TOOLS = ('clang', 'lld', 'llvm-ar', 'llvm-nm', 'llvm-objcopy', 'llvm-objdump',
                  'llvm-readobj', 'llvm-size', 'llvm-symbolizer', 'llvm-cxxfilt')
# The libraries of LLVM that are not in a tool but that programs are linked against: MCJIT, which
# llvmpipe compiles WebGL's shaders with (ports/mesa/llvmpipe.mk links them from build-nocturne/lib).
NOCTURNE_LIBRARIES = ('LLVMMCJIT',)
# What goes into C:\Nocturne: the program in System, its part of the SDK in SDK.
DIST = LLVM / 'nocturne'
# Room for the build tree of clang and lld for Nocturne and what it makes.
NOCTURNE_SPACE = 12 << 30
# The release that setup-src.sh takes from llvm-project.
SETUP_SRC = ROOT / 'ports' / 'llvm' / 'setup-src.sh'


def version_args():
    # clang and lld say they are Nocturne's build of the release, and give the
    # release's commit in llvm-project: tools/llvm/src is a repository of this
    # machine, and its commit is not one that anyone else has.
    commit = next(line.split('=', 1)[1].strip() for line in SETUP_SRC.read_text().splitlines()
                  if line.startswith('COMMIT='))
    return ['-DCLANG_VENDOR=Nocturne', '-DLLD_VENDOR=Nocturne',
            '-DLLVM_FORCE_VC_REPOSITORY=https://github.com/llvm/llvm-project',
            f'-DLLVM_FORCE_VC_REVISION={commit}']


def jobs():
    return os.environ.get('NOCTURNE_LLVM_JOBS', str(max(1, (os.cpu_count() or 2) - 2)))


def msvc_environment():
    """The environment vcvars64.bat sets up for the MSVC x64 compiler."""
    vcvars = os.environ.get('NOCTURNE_VCVARS', VCVARS)
    if not Path(vcvars).is_file():
        raise RuntimeError(f'{vcvars} が見つかりません。NOCTURNE_VCVARS を指定してください。')
    # cmd /u prints set's listing as UTF-16 whatever the console code page, so a
    # variable holding non-ASCII text (a profile folder's name) reads back intact.
    listing = subprocess.run(f'cmd /d /s /u /c ""{vcvars}" >nul && set"', check=True,
                             capture_output=True).stdout.decode('utf-16-le')
    env = {}
    for line in listing.splitlines():
        name, sep, value = line.partition('=')
        if sep and name:
            env[name] = value
    if 'VCToolsInstallDir' not in env:
        raise RuntimeError('vcvars64.bat が MSVC の環境を用意しませんでした。')
    return env


def program(name, env):
    found = shutil.which(name, path=env.get('PATH') or env.get('Path'))
    if not found:
        raise RuntimeError(f'{name} が見つかりません。')
    return found


def build_host():
    # LLVM supports MSVC as its Windows host compiler; the static CRT keeps the
    # tools free of Visual C++ runtime DLLs.
    env = msvc_environment()
    build = LLVM / 'build-host'
    cmake = program('cmake', env)
    ninja = program('ninja', env)
    subprocess.run([
        cmake, '-G', 'Ninja', '-S', str(SOURCE / 'llvm'), '-B', str(build),
        f'-DCMAKE_MAKE_PROGRAM={ninja}',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
        # The sources are UTF-8; cl otherwise reads them in the system code page
        # (932 here), where a lead byte can swallow the end of a comment line.
        '-DCMAKE_C_FLAGS=/utf-8', '-DCMAKE_CXX_FLAGS=/utf-8',
        f'-DCMAKE_INSTALL_PREFIX={LLVM / "host"}',
        '-DLLVM_ENABLE_PROJECTS=clang;lld',
        '-DLLVM_TARGETS_TO_BUILD=X86',
        f'-DLLVM_DEFAULT_TARGET_TRIPLE={TRIPLE}',
        '-DLLVM_ENABLE_ASSERTIONS=OFF',
        '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
        '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
        '-DLLVM_ENABLE_BINDINGS=OFF',
        '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF',
        '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
        # Reading PDBs through DIA needs ATL, which Build Tools leave out.
        '-DLLVM_ENABLE_DIA_SDK=OFF',
        '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_DOCS=OFF',
        '-DCLANG_ENABLE_STATIC_ANALYZER=OFF',
        # Linking clang takes several GiB each.
        '-DLLVM_PARALLEL_LINK_JOBS=2',
    ] + version_args(), check=True, env=env)
    subprocess.run([ninja, '-C', str(build), '-j', jobs()] +
                   [f'install-{c}' for c in HOST_COMPONENTS], check=True, env=env)


def build_runtimes():
    for needed in (SDK / 'Lib' / 'libc.a', HOST_BIN / 'clang++.exe'):
        if not needed.is_file():
            raise RuntimeError(f'{needed} がありません。先に make と build.py host を実行してください。')
    free = shutil.disk_usage(LLVM).free
    if free < RUNTIMES_SPACE:
        raise RuntimeError(f'{LLVM} のドライブの空きが {free >> 20} MiB しかありません'
                           f'（{RUNTIMES_SPACE >> 20} MiB 必要）。')
    env = msvc_environment()
    cmake = program('cmake', env)
    ninja = program('ninja', env)
    resource = subprocess.run([str(HOST_BIN / 'clang.exe'), '-print-resource-dir'], check=True,
                              capture_output=True, text=True).stdout.strip()

    # The sysroot is the SDK the OS build staged, with the C++ runtimes added.
    # copytree keeps the files' times, so rebuilds only recompile what changed.
    if SYSROOT.exists():
        shutil.rmtree(SYSROOT)
    shutil.copytree(SDK, SYSROOT)

    common = ['-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}',
              f'-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}', '-DCMAKE_BUILD_TYPE=Release']

    # The builtins come first: every link for Nocturne needs them, the runtimes'
    # configure checks included. clang finds them in its resource directory.
    builtins = LLVM / 'build-builtins'
    subprocess.run([
        cmake, *common, '-S', str(SOURCE / 'compiler-rt' / 'lib' / 'builtins'), '-B', str(builtins),
        f'-DCMAKE_INSTALL_PREFIX={resource}',
        '-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=ON',
        '-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON',
        # Nocturne programs are not position independent.
        '-DCOMPILER_RT_BUILTINS_ENABLE_PIC=OFF',
        # Nocturne has no libatomic, so the builtins carry the atomic library calls.
        '-DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF',
    ], check=True, env=env)
    subprocess.run([ninja, '-C', str(builtins), '-j', jobs(), 'install'], check=True, env=env)

    runtimes = LLVM / 'build-runtimes'
    subprocess.run([
        cmake, *common, '-S', str(SOURCE / 'runtimes'), '-B', str(runtimes),
        f'-DCMAKE_INSTALL_PREFIX={RUNTIMES}',
        # CMake's own test links a C++ program, which needs the libc++ this
        # builds; the runtimes check the compiler with -nostdlib++ instead.
        '-DCMAKE_CXX_COMPILER_WORKS=ON',
        '-DLLVM_ENABLE_RUNTIMES=libunwind;libcxxabi;libcxx',
        f'-DLLVM_DEFAULT_TARGET_TRIPLE={TRIPLE}',
        '-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF',
        '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_DOCS=OFF', '-DLIBCXX_INCLUDE_BENCHMARKS=OFF',
        # Nocturne links every program statically: libc++.a holds libc++abi too,
        # and libunwind is libunwind.a.
        '-DLIBCXX_ENABLE_SHARED=OFF', '-DLIBCXXABI_ENABLE_SHARED=OFF',
        '-DLIBUNWIND_ENABLE_SHARED=OFF', '-DLIBUNWIND_ENABLE_STATIC=ON',
        '-DLIBCXX_CXX_ABI=libcxxabi', '-DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON',
        '-DLIBCXXABI_INSTALL_LIBRARY=OFF',
        '-DLIBCXX_USE_COMPILER_RT=ON', '-DLIBCXXABI_USE_COMPILER_RT=ON',
        '-DLIBUNWIND_USE_COMPILER_RT=ON',
        # C++ exceptions unwind with libunwind. A Nocturne program is one static
        # image and no loader can name its unwind tables, so user.ld marks them
        # with __eh_frame_start/__eh_frame_end and __eh_frame_hdr_start/
        # __eh_frame_hdr_end: libunwind's bare-metal configuration.
        '-DLIBCXX_ENABLE_EXCEPTIONS=ON', '-DLIBCXXABI_ENABLE_EXCEPTIONS=ON',
        '-DLIBCXXABI_USE_LLVM_UNWINDER=ON', '-DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON',
        '-DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=OFF',
        '-DLIBUNWIND_IS_BAREMETAL=ON', '-DLIBUNWIND_ENABLE_ASSERTIONS=OFF',
        '-DLIBCXX_ENABLE_RTTI=ON',
        # Threads are Nocturne's C11 threads.h, clocks ISO C timespec_get.
        '-DLIBCXX_ENABLE_THREADS=ON', '-DLIBCXXABI_ENABLE_THREADS=ON',
        '-DLIBUNWIND_ENABLE_THREADS=ON',
        '-DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON',
        '-DLIBCXX_ENABLE_FILESYSTEM=ON', '-DLIBCXX_ENABLE_LOCALIZATION=ON',
        # The C library's wide characters are UTF-32 (<wchar.h>, <wctype.h>);
        # Nocturne has no time zone database.
        '-DLIBCXX_ENABLE_WIDE_CHARACTERS=ON', '-DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF',
        '-DLIBCXX_HARDENING_MODE=fast',
        # The SDK's layout: headers in Include, libraries in Lib, sources in Source.
        '-DLIBCXX_INSTALL_INCLUDE_DIR=Include/c++/v1',
        '-DLIBCXX_INSTALL_INCLUDE_TARGET_DIR=Include/c++/v1',
        '-DLIBCXXABI_INSTALL_INCLUDE_DIR=Include/c++/v1',
        '-DLIBUNWIND_INSTALL_HEADERS=ON', '-DLIBUNWIND_INSTALL_INCLUDE_DIR=Include',
        '-DLIBCXX_INSTALL_LIBRARY_DIR=Lib', '-DLIBUNWIND_INSTALL_LIBRARY_DIR=Lib',
        '-DLIBCXX_INSTALL_MODULES_DIR=Source/c++/v1',
    ], check=True, env=env)
    # RUNTIMES holds only what this build installs; the sysroot gets it on top of the SDK.
    if RUNTIMES.exists():
        shutil.rmtree(RUNTIMES)
    subprocess.run([ninja, '-C', str(runtimes), '-j', jobs(), 'install'], check=True, env=env)
    shutil.copytree(RUNTIMES, SYSROOT, dirs_exist_ok=True)


def build_nocturne():
    tablegen = LLVM / 'build-host' / 'bin'
    for needed in (SYSROOT / 'Lib' / 'libc++.a', tablegen / 'llvm-tblgen.exe',
                   tablegen / 'clang-tblgen.exe'):
        if not needed.is_file():
            raise RuntimeError(f'{needed} がありません。先に build.py host と runtimes を実行してください。')
    free = shutil.disk_usage(LLVM).free
    if free < NOCTURNE_SPACE:
        raise RuntimeError(f'{LLVM} のドライブの空きが {free >> 20} MiB しかありません'
                           f'（{NOCTURNE_SPACE >> 20} MiB 必要）。')
    env = msvc_environment()
    cmake = program('cmake', env)
    ninja = program('ninja', env)
    build = LLVM / 'build-nocturne'
    subprocess.run([
        cmake, '-G', 'Ninja', '-S', str(SOURCE / 'llvm'), '-B', str(build),
        f'-DCMAKE_MAKE_PROGRAM={ninja}', f'-DCMAKE_TOOLCHAIN_FILE={TOOLCHAIN}',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DLLVM_ENABLE_PROJECTS=clang;lld',
        '-DLLVM_TARGETS_TO_BUILD=X86',
        f'-DLLVM_HOST_TRIPLE={TRIPLE}', f'-DLLVM_DEFAULT_TARGET_TRIPLE={TRIPLE}',
        # The host build's tablegens generate the sources.
        f'-DLLVM_NATIVE_TOOL_DIR={tablegen}',
        # Nocturne programs are static and not position independent.
        '-DLLVM_ENABLE_PIC=OFF', '-DBUILD_SHARED_LIBS=OFF',
        '-DLLVM_ENABLE_THREADS=ON',
        # LLVM throws no exceptions and Nocturne gives it no backtraces, so it
        # needs no unwind tables of its own.
        '-DLLVM_ENABLE_UNWIND_TABLES=OFF',
        # One program for every tool: a document has one name, and each copy of
        # clang and lld would be most of it again.
        '-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON',
        f'-DLLVM_DISTRIBUTION_COMPONENTS={";".join(NOCTURNE_TOOLS)}',
        # C:\Nocturne\System\llvm keeps its headers and runtime in the SDK,
        # C:\Nocturne\SDK\Lib\clang.
        '-DCLANG_RESOURCE_DIR=../SDK/Lib/clang',
        '-DLLVM_ENABLE_ASSERTIONS=OFF',
        '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
        '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
        '-DLLVM_ENABLE_BINDINGS=OFF', '-DLLVM_ENABLE_PLUGINS=OFF',
        '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF',
        '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
        '-DLLVM_ENABLE_LIBPFM=OFF',
        '-DLLVM_ENABLE_CURL=OFF', '-DLLVM_ENABLE_HTTPLIB=OFF',
        '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_DOCS=OFF',
        '-DCLANG_ENABLE_STATIC_ANALYZER=OFF', '-DCLANG_ENABLE_ARCMT=OFF',
        '-DLLVM_PARALLEL_LINK_JOBS=2',
    ] + version_args(), check=True, env=env)
    subprocess.run([ninja, '-C', str(build), '-j', jobs(), 'llvm-driver',
                    'clang-resource-headers', *NOCTURNE_LIBRARIES], check=True, env=env)

    # scripts/mksdk.sh --llvm adds SDK to C:\Nocturne\SDK: the C++ runtimes as
    # the runtimes stage installed them, and clang's headers and builtins in
    # the resource directory llvm looks for beside itself.
    if DIST.exists():
        shutil.rmtree(DIST)
    (DIST / 'System').mkdir(parents=True)
    subprocess.run([str(HOST_BIN / 'llvm-strip.exe'), '-o', str(DIST / 'System' / 'llvm'),
                    str(build / 'bin' / 'llvm')], check=True)
    shutil.copytree(RUNTIMES, DIST / 'SDK')
    resource = DIST / 'SDK' / 'Lib' / 'clang'
    shutil.copytree(build / 'SDK' / 'Lib' / 'clang' / 'include', resource / 'include')
    builtins = Path(subprocess.run([str(HOST_BIN / 'clang.exe'), '-print-resource-dir'], check=True,
                                   capture_output=True, text=True).stdout.strip()) / 'lib' / TRIPLE
    shutil.copytree(builtins, resource / 'lib' / TRIPLE)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('stage', choices=('host', 'runtimes', 'nocturne'))
    args = parser.parse_args()
    if not (SOURCE / 'llvm' / 'CMakeLists.txt').is_file():
        raise SystemExit('tools/llvm/src がありません。先に ports/llvm/setup-src.sh を実行してください。')
    if args.stage == 'host':
        build_host()
    elif args.stage == 'runtimes':
        build_runtimes()
    elif args.stage == 'nocturne':
        build_nocturne()


if __name__ == '__main__':
    main()
