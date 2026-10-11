"""Nocturne向けlibc++とC++20ソースをホストclangで構築する。"""
import argparse
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build' / 'cxx'
# Match the supplied Windows LLVM compiler; never modify the external checkout.
UPSTREAM_REF = 'llvmorg-18.1.8'
UPSTREAM_COMMIT = '3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff'
LIBRARY_SOURCES = ('algorithm.cpp', 'string.cpp', 'vector.cpp', 'verbose_abort.cpp')


def tool(name):
    directory = Path(os.environ.get('NOCTURNE_LLVM_BIN', 'C:/Program Files/LLVM/bin'))
    candidate = directory / (name + ('.exe' if os.name == 'nt' else ''))
    if candidate.is_file():
        return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    raise RuntimeError(f'{name} が見つかりません。NOCTURNE_LLVM_BIN を指定してください。')


def prepare():
    stamp = BUILD / 'upstream' / '.commit'
    if not stamp.is_file() or stamp.read_text(encoding='utf-8').strip() != UPSTREAM_COMMIT:
        source = Path(os.environ.get('NOCTURNE_LLVM_SOURCE',
                                     'D:/Programes/cloned repos/llvm-project'))
        commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse',
                                           UPSTREAM_REF + '^{commit}'], text=True).strip()
        if commit != UPSTREAM_COMMIT:
            raise RuntimeError(f'{UPSTREAM_REF} のcommitが期待値と一致しません。')
        archive = subprocess.check_output(['git', '-C', str(source), 'archive',
                                            UPSTREAM_COMMIT, 'libcxx/include',
                                            'libcxx/src', 'libcxx/LICENSE.TXT'])
        destination = BUILD / 'upstream'
        destination.mkdir(parents=True, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
            # git archive is pinned, but validate paths before extraction as well.
            for member in tar.getmembers():
                relative = Path(member.name)
                if relative.is_absolute() or '..' in relative.parts or member.issym() or member.islnk():
                    raise RuntimeError('LLVM archiveに不正なパスがあります。')
            tar.extractall(destination)
        stamp.write_text(UPSTREAM_COMMIT + '\n', encoding='utf-8')
    return BUILD / 'upstream' / 'libcxx'


def flags(upstream):
    # Native applications use the ordinary C-linkage main entry supplied by
    # Nocturne's crt0. Freestanding C++ would mangle main and break that ABI.
    # Disable implicit libc substitutions without changing main's semantics.
    return ['--target=x86_64-unknown-none-elf', '-std=c++20', '-fno-builtin',
            '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-pic',
            '-fno-pie', '-mno-red-zone', '-msse2', '-O2', '-g', '-nostdinc++',
            '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
            '-Iuser/include/c++', '-I' + build_path(upstream / 'include'),
            '-idirafter', 'user/include', '-Icommon', '-Wall', '-Wextra', '-Wno-unused-parameter']


def build_path(path):
    # GNU Make reads .d files in MSYS2. Relative Unix paths avoid Windows drive
    # colons and backslashes being interpreted as rule syntax.
    absolute = path.resolve()
    try:
        return absolute.relative_to(ROOT).as_posix()
    except ValueError:
        return absolute.as_posix()


def compile_source(source, output, upstream):
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([tool('clang++'), *flags(upstream), '-MMD', '-MP', '-c',
                    build_path(source), '-o', build_path(output)], check=True, cwd=ROOT)
    dependency = output.with_suffix('.d')
    # Windows clang normalizes prerequisites back to '\\' even for slash inputs.
    # Preserve Make line continuations and escaped spaces while converting only
    # path separators in this generated build artifact.
    text = dependency.read_text(encoding='utf-8')
    dependency.write_text(re.sub(r'\\(?=[^\s\\])', '/', text), encoding='utf-8')


def library(upstream):
    objects = []
    dependencies = [Path(__file__), *sorted((ROOT / 'user' / 'include').rglob('*.h')),
                    *sorted((ROOT / 'user' / 'include' / 'c++').iterdir())]
    newest_config = max(path.stat().st_mtime_ns for path in dependencies if path.is_file())
    for name in LIBRARY_SOURCES:
        source = upstream / 'src' / name
        output = BUILD / 'libcxx' / (Path(name).stem + '.o')
        if not output.is_file() or output.stat().st_mtime_ns < max(newest_config, source.stat().st_mtime_ns):
            print('  CXX  libc++/' + name, flush=True)
            compile_source(source, output, upstream)
        objects.append(output)
    archive = BUILD / 'libc++.a'
    if not archive.is_file() or any(obj.stat().st_mtime_ns > archive.stat().st_mtime_ns for obj in objects):
        subprocess.run([tool('llvm-ar'), 'rcs', str(archive), *map(str, objects)], check=True)
    # Keep the upstream license with the target runtime rather than host-only sources.
    license_file = BUILD / 'LICENSE.libc++.txt'
    shutil.copyfile(upstream / 'LICENSE.TXT', license_file)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    actions.add_parser('library')
    compile_parser = actions.add_parser('compile')
    compile_parser.add_argument('source', type=Path)
    compile_parser.add_argument('-o', '--output', required=True, type=Path)
    args = parser.parse_args()
    upstream = prepare()
    if args.action == 'library':
        library(upstream)
    else:
        compile_source(args.source, args.output, upstream)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
