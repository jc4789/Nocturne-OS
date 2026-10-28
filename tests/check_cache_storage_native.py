"""Cache Storage の保存（user/libc/webcache.c：名前の表、要求と応答の一覧、本文、Batch Cache
Operations、Query Cache、Vary、消した cache の扱い、設定の窓の量と消去）のホストの試験。製品の
webcache.c をそのまま組み、ネイティブの文書・フォルダー・乱数・借りの呼出しだけを
tests/cache_storage_native_cases.c が build/cache-native-<番号> のホストのファイルへ写す。二つの
engine を二つの窓の broker に見立てる。範囲を限った裏付けで、broker の管・realm・ゲストの
ブラウザーでの受け入れではない。
"""
from pathlib import Path
import hashlib
import os
import subprocess
import url_owner

root = Path(__file__).resolve().parent.parent
out = root / 'build/cache-storage-native'
out.mkdir(parents=True, exist_ok=True)
cc = url_owner.clang(root)
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
log = out / 'result.log'
exe = out / 'cache-storage-native.exe'
sources = ['user/libc/webcache.c', 'tests/cache_storage_native_cases.c']
bearssl = ['third_party/bearssl/src/hash/sha2small.c', 'third_party/bearssl/src/codec/enc32be.c',
           'third_party/bearssl/src/codec/dec32be.c']
# The origin key is checked against its URL record: the URL parser's own objects (tests/url_owner.py).
owner = url_owner.objects(root, cc)
with log.open('w', encoding='utf-8') as stream:
    for name in sources + ['user/include/webcache.h', 'user/include/weburl.h', 'tests/check_cache_storage_native.py']:
        stream.write(name + ' SHA256 ' + hashlib.sha256((root / name).read_bytes()).hexdigest() + '\n')
    stream.flush()
    subprocess.run([str(cc), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-fsanitize=undefined', '-fsanitize-trap=undefined',
                    '-idirafter', 'user/include', '-idirafter', 'common',
                    '-Ithird_party/bearssl/inc', '-Ithird_party/bearssl/src']
                   + sources + bearssl + owner + ['-o', str(exe)],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe)], cwd=root, env=env, timeout=120, stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
