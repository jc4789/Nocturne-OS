"""ブラウザーの Cookie の保存庫（user/libc/webnet.c の cookie_sync と周り）のホストの試験。

製品の webnet.c から保存庫の部分（定数、cookie_path_ready〜cookie_pump、webnet_cookie_sites〜
webnet_site）をそのまま切り出し、製品の user/libc/webcookie.c と tests/webnet_cookie_store_host.c
で組む。いくつかの入れ物が、profile の文書 "cookies" を分け合う別々の窓のプロセスを表す。文書の口は
記憶の中の一つの文書と claim で、見張りは無い扱い。範囲を限った裏付けで、カーネルの文書と見張り、
ゲストのブラウザーでの受け入れではない。
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/webnet-cookie-store-host'
out.mkdir(parents=True, exist_ok=True)
product = (root / 'user/libc/webnet.c').read_text(encoding='utf-8')


def part(start, end):
    a = product.index(start)
    return product[a:product.index(end, a)]


cut = (part('#define WEBNET_COOKIE_TOKEN', '/* A WebSocket') +
       part('/* The cookie store: the persistent cookies', '/* Closing the end also ends its watches. */') +
       part('long webnet_cookie_sites(', 'long webnet_cookie_get(webnet *n,'))
(out / 'store.inc').write_bytes(cut.encode('utf-8'))

cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
log = out / 'result.log'
exe = out / 'webnet_cookie_store.exe'
sources = ['tests/webnet_cookie_store_host.c', 'user/libc/webcookie.c']
with log.open('w', encoding='utf-8') as stream:
    for name in sources + ['user/libc/webnet.c', 'user/include/webcookie.h', 'tests/check_webnet_cookie_store.py']:
        stream.write(name + ' SHA256 ' + hashlib.sha256((root / name).read_bytes()).hexdigest() + '\n')
    stream.flush()
    subprocess.run([str(cc), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-function',
                    '-fsanitize=undefined', '-fsanitize-trap=undefined', '-iquote', str(out),
                    '-idirafter', 'user/include', '-idirafter', 'common'] + sources + ['-o', str(exe)],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe), 'rootfs/Nocturne/Web/public_suffix_list.dat'], cwd=root, env=env, timeout=60,
                            stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
