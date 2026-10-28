"""ブラウザーのサイトのデータ（user/libc/websitedata.c：設定の窓の一覧と消去、閉じるときに消す）の
ホストの試験。製品の websitedata.c をそのまま組み、各保存先（Cookie・Web Storage・IndexedDB・
OPFS）の関数、利用者の選択、プロセスの一覧、一時フォルダーの文書 "windows" を
tests/websitedata_host.c の小さな写しに替える。範囲を限った裏付けで、保存先そのものやゲストの
ブラウザーでの受け入れではない。
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/websitedata-host'
out.mkdir(parents=True, exist_ok=True)
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
log = out / 'result.log'
exe = out / 'websitedata.exe'
sources = ['user/libc/websitedata.c', 'tests/websitedata_host.c']
with log.open('w', encoding='utf-8') as stream:
    for name in sources + ['user/include/websitedata.h', 'tests/check_websitedata.py']:
        stream.write(name + ' SHA256 ' + hashlib.sha256((root / name).read_bytes()).hexdigest() + '\n')
    stream.flush()
    subprocess.run([str(cc), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-fsanitize=undefined', '-fsanitize-trap=undefined',
                    '-Ithird_party/quickjs', '-Iuser/libc', '-idirafter', 'user/include', '-idirafter', 'common']
                   + sources + ['-o', str(exe)],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe)], cwd=root, env=env, timeout=20, stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
