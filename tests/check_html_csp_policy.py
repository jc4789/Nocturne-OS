"""CSP3 の方針の模型（user/libc/web/html_policy.c）をホストで組んで走らせる。

製品の html_policy.c・web_url.c（Lexbor の WHATWG URL）・util.c・http.c と
BearSSL の SHA-2 をそのまま繋ぐ。期待値は CSP3 の手順と WPT の期待から取る
（tests/html_csp_policy_hosttest.c）。ゲストへの配送も WPT の合格も主張しない。
Lexbor と BearSSL の目的ファイルは build/csp-policy-host/ に残し、次からは組み直さない。
"""
from pathlib import Path
import os, subprocess, sys

root = Path(__file__).resolve().parents[1]
out = root / 'build' / 'csp-policy-host'
objects = out / 'vendor'
objects.mkdir(parents=True, exist_ok=True)

def find_clang():
    # A worktree has no tools/; the main checkout keeps the shared toolchain.
    for base in [root, *root.parents]:
        cc = base / 'tools' / 'msys64' / 'ucrt64' / 'bin' / 'clang.exe'
        if cc.exists():
            return cc
    sys.exit('clang.exe (tools/msys64/ucrt64/bin) was not found')

cc = find_clang()
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
lexbor = root / 'third_party' / 'lexbor' / 'source' / 'lexbor'
vendor = sorted((lexbor / 'core').glob('*.c')) + [lexbor / 'url' / 'url.c', lexbor / 'punycode' / 'punycode.c'] + \
    sorted((lexbor / 'unicode').glob('*.c')) + sorted((lexbor / 'encoding').glob('*.c')) + [root / 'ports' / 'lexbor' / 'memory.c']
bearssl = root / 'third_party' / 'bearssl' / 'src'
crypto = [bearssl / 'hash' / 'sha2small.c', bearssl / 'hash' / 'sha2big.c'] + \
    [bearssl / 'codec' / f'{name}.c' for name in ('dec32be', 'enc32be', 'dec64be', 'enc64be')]

def run(command):
    completed = subprocess.run([str(x) for x in command], cwd=root, env=env, capture_output=True)
    text = (completed.stdout + completed.stderr).decode('utf-8', errors='replace')
    if text:
        print(text, end='', flush=True)
    if completed.returncode:
        raise SystemExit(completed.returncode)

linked = []
for source in vendor + crypto:
    target = objects / (source.parent.name + '_' + source.stem + '.o')
    if not target.exists() or target.stat().st_mtime < source.stat().st_mtime:
        flags = ['-I' + str(root / 'third_party' / 'lexbor' / 'source'), '-DLEXBOR_STATIC'] if source not in crypto else \
            ['-I' + str(bearssl), '-I' + str(root / 'third_party' / 'bearssl' / 'inc')]
        run([cc, '-O1', '-w', *flags, '-c', source, '-o', target])
    linked.append(target)

exe = out / 'html-csp-policy-hosttest.exe'
includes = ['-Iuser/include', '-Icommon', '-Iuser/libc/web', '-Ithird_party/lexbor/source', '-Iports/lexbor/include',
            '-Ithird_party/bearssl/inc']
run([cc, '-std=gnu11', '-O1', '-Wall', '-Wno-unused-parameter', '-Wno-sign-compare', '-DLEXBOR_STATIC',
     '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *includes,
     'tests/html_csp_policy_hosttest.c', 'user/libc/web/html_policy.c', 'user/libc/web/web_url.c',
     'user/libc/web/util.c', 'user/libc/http.c', *linked, '-o', exe])
run([exe])

# The earlier frame-ancestors / Wasm / snapshot checks against the same
# html_policy.c. http.c's errno and locking are never entered there.
stubs = out / 'frame-embedding-stubs.c'
stubs.write_text('#include <threads.h>\nint *__errno_location(void){static int e;return &e;}\n'
                 'int mtx_lock(mtx_t *m){(void)m;return 0;}\nint mtx_unlock(mtx_t *m){(void)m;return 0;}\n'
                 'struct web_doc;\n_Bool web_frame_same_origin(struct web_doc *a,struct web_doc *b){(void)a;(void)b;return 1;}\n',
                 encoding='utf-8', newline='\n')
embedding = out / 'frame-embedding-policy-hosttest.exe'
run([cc, '-std=gnu11', '-O1', '-w', '-DLEXBOR_STATIC', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', *includes,
     'tests/frame_embedding_policy_hosttest.c', stubs, 'user/libc/web/html_policy.c', 'user/libc/web/web_url.c',
     'user/libc/web/util.c', 'user/libc/http.c', *linked, '-o', embedding])
run([embedding])
