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
LIBRARY_SOURCES = ('algorithm.cpp', 'hash.cpp', 'memory.cpp', 'string.cpp', 'vector.cpp', 'verbose_abort.cpp')
WABT_COMMIT = '1e9fffd80efa901b3e4708d4ba1f0df972e63518'
WABT_SOURCES = ('binary-reader.cc', 'binary-reader-logging.cc', 'binary.cc',
                'common.cc', 'config.cc', 'feature.cc', 'ir.cc', 'binding-hash.cc', 'leb128.cc',
                'opcode.cc', 'opcode-code-table.c', 'shared-validator.cc',
                'stream.cc', 'type-checker.cc', 'utf8.cc',
                'interp/binary-reader-interp.cc', 'interp/interp.cc',
                'interp/istream.cc')
# Nocturne's own interpreter parts, under ports/wabt.
WABT_PORT_SOURCES = ('interp-x64.cc',)
WASM2C_SOURCES = ('apply-names.cc', 'binary-reader-ir.cc', 'c-writer.cc',
                 'color.cc', 'error-formatter.cc', 'expr-visitor.cc',
                 'filenames.cc', 'generate-names.cc', 'ir-util.cc',
                 'lexer-source-line-finder.cc', 'lexer-source.cc', 'literal.cc',
                 'option-parser.cc', 'sha256.cc', 'validator.cc',
                 'prebuilt/wasm2c_header_top.cc', 'prebuilt/wasm2c_header_bottom.cc',
                 'prebuilt/wasm2c_source_includes.cc',
                 'prebuilt/wasm2c_source_declarations.cc',
                 'prebuilt/wasm2c_simd_source_declarations.cc',
                 'prebuilt/wasm2c_atomicops_source_declarations.cc')


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


def compile_source(source, output, upstream, extra=()):
    output.parent.mkdir(parents=True, exist_ok=True)
    compiler = 'clang++'
    options = flags(upstream)
    if source.suffix == '.c':
        compiler = 'clang'
        options = [option for option in options if option not in ('-std=c++20', '-nostdinc++')]
        options += ['-std=c11']
    subprocess.run([tool(compiler), *options, *extra, '-MMD', '-MP', '-c',
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


def prepare_wabt():
    """固定revisionの必要sourceのみ取り込む。外部checkoutは変更しない。"""
    destination = ROOT / 'third_party' / 'wabt'
    sources = (*WABT_SOURCES, *WASM2C_SOURCES, 'tools/wasm2c.cc')
    if all((destination / 'src' / name).is_file() for name in sources) and (destination / 'wasm2c' / 'wasm-rt.h').is_file():
        return destination
    source = Path(os.environ.get('NOCTURNE_WABT_SOURCE',
                                 'D:/Programes/cloned repos/wabt'))
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse',
                                     WABT_COMMIT + '^{commit}'], text=True).strip()
    if commit != WABT_COMMIT:
        raise RuntimeError('WABTのcommitが期待値と一致しません。')
    archive = subprocess.check_output(['git', '-C', str(source), 'archive',
                                      WABT_COMMIT, 'LICENSE', 'include/wabt',
                                      *('src/' + name for name in sources),
                                      'wasm2c', 'src/template',
                                      'scripts/gen-wasm2c-templates.cmake'])
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
        for member in tar.getmembers():
            relative = Path(member.name)
            if relative.is_absolute() or '..' in relative.parts or member.issym() or member.islnk():
                raise RuntimeError('WABT archiveに不正なパスがあります。')
        # Preserve the reviewed Nocturne embedding changes on later imports.
        tar.extractall(destination, members=[member for member in tar.getmembers()
                                            if not (destination / member.name).exists()])
    return destination


def wabt_library(upstream, translator=False):
    source_root = prepare_wabt()
    if translator:
        # Use WABT's real generator after any template edits; never hand-edit
        # the prebuilt C++ string literals used by the C writer.
        cmake = shutil.which('cmake')
        if not cmake:
            configured_cmake = Path(os.environ.get('NOCTURNE_CMAKE', 'C:/Program Files/CMake/bin/cmake.exe'))
            if configured_cmake.is_file():
                cmake = str(configured_cmake)
        if not cmake:
            raise RuntimeError('wasm2cテンプレート生成にはCMakeが必要です。')
        templates = (
            ('wasm2c_header_top', 'wasm2c.top.h', 's_header_top'),
            ('wasm2c_header_bottom', 'wasm2c.bottom.h', 's_header_bottom'),
            ('wasm2c_source_includes', 'wasm2c.includes.c', 's_source_includes'),
            ('wasm2c_source_declarations', 'wasm2c.declarations.c', 's_source_declarations'),
            ('wasm2c_simd_source_declarations', 'wasm2c_simd.declarations.c', 's_simd_source_declarations'),
            ('wasm2c_atomicops_source_declarations', 'wasm2c_atomicops.declarations.c', 's_atomicops_source_declarations'))
        for name, template, symbol in templates:
            source = source_root / 'src' / 'template' / template
            output = ROOT / 'build' / 'wabt' / 'generated' / (name + '.cc')
            generator = source_root / 'scripts' / 'gen-wasm2c-templates.cmake'
            if not output.is_file() or output.stat().st_mtime_ns < max(source.stat().st_mtime_ns, generator.stat().st_mtime_ns):
                output.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run([cmake, '-Dout=' + str(output), '-Din=' + str(source),
                                '-Dsymbol=' + symbol, '-P', str(generator)], check=True)
    # Release build of the library: its asserts check interpreter invariants
    # on every decoded instruction and memory access.
    extra = ['-Iports/wabt/include', '-Ithird_party/wabt/include',
             '-Ithird_party/bearssl/inc', '-Wno-unused-private-field', '-DNDEBUG']
    headers = [*source_root.rglob('*.h'), *source_root.rglob('*.def'),
               *source_root.rglob('*.inc'), *(ROOT / 'ports' / 'wabt').rglob('*.h'),
               Path(__file__)]
    newest_header = max(max(path.stat().st_mtime_ns for path in headers),
                        (BUILD / 'libc++.a').stat().st_mtime_ns)
    objects = []
    names = WASM2C_SOURCES if translator else (*WABT_SOURCES, *WABT_PORT_SOURCES)
    for name in names:
        source = source_root / 'src' / name
        if name in WABT_PORT_SOURCES:
            source = ROOT / 'ports' / 'wabt' / name
        if name.startswith('prebuilt/'):
            source = ROOT / 'build' / 'wabt' / 'generated' / Path(name).name
        output = ROOT / 'build' / 'wabt' / (name + '.o')
        if not output.is_file() or output.stat().st_mtime_ns < max(newest_header, source.stat().st_mtime_ns):
            print('  CXX  wabt/' + name, flush=True)
            compile_source(source, output, upstream, extra)
        objects.append(output)
    archive = ROOT / 'build' / 'wabt' / ('libwasm2c.a' if translator else 'libwabt.a')
    if not archive.is_file() or any(obj.stat().st_mtime_ns > archive.stat().st_mtime_ns for obj in objects):
        subprocess.run([tool('llvm-ar'), 'rcs', str(archive), *map(str, objects)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    actions.add_parser('library')
    actions.add_parser('wabt')
    actions.add_parser('vendor-wabt')
    actions.add_parser('wabt-sources')
    actions.add_parser('wabt-port-sources')
    actions.add_parser('wasm2c')
    actions.add_parser('wasm2c-sources')
    compile_parser = actions.add_parser('compile')
    compile_parser.add_argument('source', type=Path)
    compile_parser.add_argument('-o', '--output', required=True, type=Path)
    compile_parser.add_argument('-I', '--include', action='append', default=[])
    archive_parser = actions.add_parser('archive')
    archive_parser.add_argument('output', type=Path)
    archive_parser.add_argument('objects', type=Path, nargs='+')
    args = parser.parse_args()
    if args.action == 'archive':
        args.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([tool('llvm-ar'), 'rcs', str(args.output), *map(str, args.objects)], check=True)
        return
    if args.action in ('wabt-sources', 'wasm2c-sources'):
        print(' '.join(WABT_SOURCES if args.action == 'wabt-sources' else WASM2C_SOURCES))
        return
    if args.action == 'wabt-port-sources':
        print(' '.join(WABT_PORT_SOURCES))
        return
    upstream = prepare()
    if args.action == 'library':
        library(upstream)
    elif args.action == 'wabt':
        library(upstream)
        wabt_library(upstream)
    elif args.action == 'vendor-wabt':
        prepare_wabt()
    elif args.action == 'wasm2c':
        library(upstream)
        wabt_library(upstream, translator=True)
    else:
        compile_source(args.source, args.output, upstream,
                       ['-I' + path for path in args.include])


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
