"""全BDF字形の独立照合と、実カーネル描画/コンソールのホスト試験。"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    vendor = ROOT / 'third_party/unifont'
    manifest = json.loads((vendor / 'manifest.json').read_text(encoding='utf-8'))
    compressed = (vendor / manifest['compressed_filename']).read_bytes()
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == manifest['source_sha256']
    assert hashlib.sha256(compressed).hexdigest() == manifest['compressed_sha256']
    assert hashlib.sha256((vendor / 'OFL-1.1.txt').read_bytes()).hexdigest() == manifest['license_sha256']
    data = (ROOT / 'common/unifont.bin').read_bytes()
    assert len(data) == 65536 + 65536 * 32
    glyphs = {}
    cp = width = None
    rows = None
    for line in raw.decode('utf-8').splitlines():
        if line.startswith('ENCODING '):
            cp = int(line.split()[1])
        elif line.startswith('DWIDTH '):
            width = int(line.split()[1])
        elif line.startswith('BITMAP'):
            rows = []
        elif line == 'ENDCHAR':
            assert cp not in glyphs and 0 <= cp < 65536
            assert width in (8, 16) and len(rows) == 16
            expected = b''.join((r << (16 - width)).to_bytes(2, 'big') for r in rows)
            assert data[cp] == width, hex(cp)
            assert data[65536 + cp * 32:65536 + (cp + 1) * 32] == expected, hex(cp)
            glyphs[cp] = width
            rows = None
        elif rows is not None:
            rows.append(int(line.strip(), 16))
    assert len(glyphs) == 57086 == manifest['glyph_count']
    for cp in range(65536):
        if cp not in glyphs:
            assert data[cp] == 0 and data[65536 + cp * 32:65536 + (cp + 1) * 32] == bytes(32)
    sys.path.insert(0, str(ROOT / 'scripts'))
    from unifont2bin import convert
    regenerated, count = convert(vendor / manifest['compressed_filename'])
    assert count == len(glyphs) and regenerated == data
    print('全57,086字形の幅・全16行一致、未収録8,450位置は空、原本SHA-256・再生成一致。')


def check_renderer(compiler):
    out = ROOT / 'build/kernel-unifont/host'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'arch').mkdir(exist_ok=True)
    (out / 'mm').mkdir(exist_ok=True)
    (out / 'kernel.h').write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "gfx.h"
#define OS_NAME "Nocturne"
#define OS_VERSION "host-test"
#define PAGE_SIZE 4096
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ALIGN_UP(x,a) (((x)+(a)-1)&~((a)-1))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ksnprintf snprintf
#define kprintf printf
struct regs { uint64_t rip,rsp,rflags,cs,ss,rax,rbx,rcx,rdx,rsi,rdi,rbp,error,r8,r9,r10,r11,r12,r13,r14,r15,vector; };
size_t klog_read(char *, size_t);
''', encoding='utf-8')
    (out / 'arch/cpu.h').write_text('''#pragma once
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t x) { (void)x; }
static inline void *phys_to_virt(uint64_t x) { return (void *)(uintptr_t)x; }
static inline void cli(void) {}
static inline void hlt(void) {}
static inline uint64_t read_cr2(void) { return 0; }
static inline uint64_t read_cr3(void) { return 0; }
''', encoding='utf-8')
    (out / 'mm/pmm.h').write_text('#pragma once\nuint64_t pmm_alloc_contig(size_t);\n', encoding='utf-8')
    assembly = out / 'data.s'
    section = '.section .rdata,"dr"' if sys.platform == 'win32' else '.section .rodata'
    binary = (ROOT / 'common/unifont.bin').as_posix()
    assembly.write_text(section + '\n.globl unifont_widths\n.globl unifont_rows\n'
                        'unifont_widths:\n.incbin "' + binary + '",0,65536\n'
                        'unifont_rows:\n.incbin "' + binary + '",65536\n', encoding='utf-8')
    executable = out / ('test.exe' if sys.platform == 'win32' else 'test')
    # Kernel uses LP64 formats; Windows host stubs use LLP64 and panic is not run.
    command = [compiler, '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-format', '-I' + str(out),
               '-I' + str(ROOT / 'common'), '-I' + str(ROOT / 'kernel/src'),
               str(ROOT / 'tests/unifont_host_test.c'), str(ROOT / 'common/gfx.c'), str(assembly), '-o', str(executable)]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=str(ROOT / 'tools/msys64/ucrt64/bin/clang.exe') if sys.platform == 'win32' else 'cc')
    args = parser.parse_args()
    check_source()
    check_renderer(args.cc)
