"""ブラウザーの好みのうち、Cookie とサイトのデータの例外（許可・閉じるときに消す・ブロック）と
「閉じるときに Cookie とサイトのデータを消す」を、製品の user/libc/webprefs.c をそのまま組んで
確かめるホストの試験。文書の口（n_document_*）と profile の場所だけを
tests/webprefs_sites_host.c の記憶の中の文書に差し替える。見張りは無い扱いで、別のプロセスの
書き込みは試験が文書を直に書き換えて表す。範囲を限った裏付けで、ゲストでの受け入れではない。
"""
from pathlib import Path
import hashlib
import os
import subprocess
import url_owner

root = Path(__file__).resolve().parent.parent
out = root / 'build/webprefs-sites-host'
out.mkdir(parents=True, exist_ok=True)
cc = url_owner.clang(root)
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
log = out / 'result.log'
exe = out / 'webprefs_sites.exe'
sources = ['user/libc/webprefs.c', 'tests/webprefs_sites_host.c']
# A URL's host comes from its record: the URL parser's own objects (tests/url_owner.py).
owner = url_owner.objects(root, cc)
with log.open('w', encoding='utf-8') as stream:
    for name in sources + ['user/include/webprefs.h', 'user/include/webcookie.h', 'user/include/weburl.h']:
        stream.write(name + ' SHA256 ' + hashlib.sha256((root / name).read_bytes()).hexdigest() + '\n')
    stream.flush()
    subprocess.run([str(cc), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-idirafter', 'user/include', '-idirafter', 'common'] + sources + owner + ['-o', str(exe)],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe)], cwd=root, env=env, timeout=20, stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'))
raise SystemExit(result.returncode)
