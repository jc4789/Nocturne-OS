"""user/libc/webprefs.c（ブラウザーの好み：言語の一覧と検索の履歴の有効）のホストの試験。

製品の webprefs.c をそのまま、名前を変えて何度も組む（一つ一つが別のプロセスの写しで、静的な
状態を共有しない）。C: の文書・claim・見張り・待ち行列は tests/webprefs_host_cases.c が記憶の上で
真似る。錠（threads.h）は Win32 の SRW の上の写しで、同じスレッドが二度取ると止まる。malloc は
失敗を注入できる写しに替える。範囲を限った裏付けで、カーネルの見張り（K1）・ゲストのブラウザー・
設定の窓での受け入れではない。

目録の tag は、同梱の formatjs の CLDR の表（言語は likelySubtags、地域は英語の地域名、別名は
languageAlias・territoryAlias）でも照らす：言語と地域の subtag が CLDR の知る物で、別名（廃止された
tag）でないこと。IANA の登録簿そのものではなく、その代わりの裏付け。
"""
from pathlib import Path
import hashlib
import os
import re
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/webprefs-host'
stub = out / 'stub'
stub.mkdir(parents=True, exist_ok=True)
PROCESSES = 48

header = (root / 'user/include/webprefs.h').read_text(encoding='utf-8')
names = re.findall(r'\b(webprefs_\w+)\s*\(', header)
expected = ['webprefs_catalog', 'webprefs_watch', 'webprefs_events', 'webprefs_sync',
            'webprefs_languages_generation', 'webprefs_languages', 'webprefs_languages_default', 'webprefs_accept_language',
            'webprefs_languages_fit', 'webprefs_search_history', 'webprefs_set_languages',
            'webprefs_reset_languages', 'webprefs_set_search_history']
problems = []
if sorted(set(names)) != sorted(expected):
    problems.append('webprefs.h の関数が試験の知る一覧と違う：' + ' '.join(sorted(set(names) ^ set(expected))))


def write(path, text):
    path.write_bytes(text.encode('utf-8'))


# 錠の写し：零の mtx_t が錠の掛かっていない錠（製品の threads.h と同じ約束）。
write(stub / 'threads.h', r'''#pragma once
typedef struct { void *srw; volatile unsigned long owner; } mtx_t;
enum { thrd_success = 0 };
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(void *lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(void *lock);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
void webprefs_test_lock_failure(const char *what);
void webprefs_test_locked(int delta);
static inline int mtx_lock(mtx_t *m) {
    if (m->owner == GetCurrentThreadId()) webprefs_test_lock_failure("同じスレッドが同じ錠を二度取る");
    AcquireSRWLockExclusive(&m->srw);
    m->owner = GetCurrentThreadId();
    webprefs_test_locked(1);
    return thrd_success;
}
static inline int mtx_unlock(mtx_t *m) {
    if (m->owner != GetCurrentThreadId()) webprefs_test_lock_failure("取っていない錠を放す");
    m->owner = 0;
    webprefs_test_locked(-1);
    ReleaseSRWLockExclusive(&m->srw);
    return thrd_success;
}
''')
write(out / 'webprefs_fault.h', r'''#pragma once
#include <stdlib.h>
void *webprefs_test_malloc(size_t n);
void *webprefs_test_calloc(size_t n, size_t size);
void *webprefs_test_realloc(void *p, size_t n);
void webprefs_test_free(void *p);
#define malloc webprefs_test_malloc
#define calloc webprefs_test_calloc
#define realloc webprefs_test_realloc
#define free webprefs_test_free
''')
sources = []
for i in range(PROCESSES):
    p = f'p{i}'
    renames = ''.join(f'#define {n} {p}_{n}\n' for n in expected)
    source = out / f'process_{i:02}.c'
    write(source, f'#include "webprefs_fault.h"\n{renames}#include "user/libc/webprefs.c"\n')
    sources.append(source)
write(out / 'webprefs_processes.h',
      '#define PROCESSES(X) ' + ' '.join(f'X(p{i})' for i in range(PROCESSES)) + '\n')

# 目録の tag を CLDR の表で照らす。
product = (root / 'user/libc/webprefs.c').read_text(encoding='utf-8')
block = product[product.index('catalog[] = {'):product.index('};', product.index('catalog[] = {'))]
tags = re.findall(r'^\s*\{"([^"]+)", "[^"]*", "[^"]*"\},$', block, re.M)
cldr = (root / 'third_party/formatjs/source/@formatjs/intl-getcanonicallocales/index.js').read_text(encoding='utf-8')


def table(name):
    start = cldr.index(f'const {name} = {{')
    return set(re.findall(r'^\t"([^"]+)":', cldr[start:cldr.index('\n};', start)], re.M))


likely, language_alias, territory_alias = table('likelySubtags'), table('languageAlias'), table('territoryAlias')
# 地域：likelySubtags は推せる地域を省くので、CLDR の英語の地域名の表（intl-displaynames）の鍵で照らす。
display = (root / 'third_party/formatjs/source/@formatjs/intl-displaynames/locale-data/en.js').read_text(encoding='utf-8')
start = display.index('"region": {')
regions = set(re.findall(r'^\s+"([0-9A-Z]{2,3})": "', display[start:display.index('"script": {', start)], re.M))
cldr_checked = 0
for tag in tags:
    parts = tag.split('-')
    language, region = parts[0], parts[1] if len(parts) > 1 else None
    if language not in likely or language in language_alias:
        problems.append(f'目録の {tag}：言語の subtag {language} が CLDR の likelySubtags に無いか、別名')
    if region and (region not in regions or region in territory_alias):
        problems.append(f'目録の {tag}：地域の subtag {region} が CLDR の地域名の表に無いか、別名')
    cldr_checked += 1

cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
log = out / 'result.log'
exe = out / 'webprefs_host.exe'
with log.open('w', encoding='utf-8') as stream:
    for name in ['user/libc/webprefs.c', 'user/include/webprefs.h', 'user/include/nocturne.h', 'common/abi.h',
                 'tests/webprefs_host_cases.c', 'tests/check_webprefs.py']:
        stream.write(name + ' SHA256 ' + hashlib.sha256((root / name).read_bytes()).hexdigest() + '\n')
    stream.write(f'目録の tag {len(tags)} 件を CLDR の表で照らした（{cldr_checked} 件）\n')
    for problem in problems:
        stream.write('失敗 ' + problem + '\n')
    stream.flush()
    built = subprocess.run([str(cc), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare',
                            '-I', str(stub), '-iquote', str(root), '-iquote', str(out), '-idirafter', 'user/include',
                            '-idirafter', 'common', *map(str, sources), 'tests/webprefs_host_cases.c', '-o', str(exe)],
                           cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT)
    stream.flush()
    code = built.returncode
    if not code:
        code = subprocess.run([str(exe)], cwd=root, env=env, timeout=120, stdout=stream, stderr=subprocess.STDOUT).returncode
print(log.read_text(encoding='utf-8'))
raise SystemExit(code or (1 if problems else 0))
