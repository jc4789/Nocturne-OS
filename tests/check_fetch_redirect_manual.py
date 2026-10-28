"""Fetch の redirect mode "manual" の native の境界をホストで試す（fetch_redirect_manual_hosttest.c）。
webfetch.c の run_http より前の production のコードをそのまま切り出して組み、URL の記録の持ち主
（web_url.c と Lexbor、tests/url_owner.py）をプログラムと同じく結ぶ。ゲストの配送、run_http の
転送の繰り返し、webnet.c の列、実際のサーバーは試さない。
走らせ方：python -X utf8 tests/check_fetch_redirect_manual.py"""
from pathlib import Path
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests'))
import url_owner

out = root / 'build/fetch-redirect-manual'
out.mkdir(parents=True, exist_ok=True)
worker = (root / 'user/apps/webfetch.c').read_text(encoding='utf-8')
out.joinpath('webfetch-tested.inc').write_text(worker[:worker.index('static bool run_http(')], encoding='utf-8', newline='\n')
cc = url_owner.clang(root)
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
owner = url_owner.objects(root, cc)
exe = out / 'fetch-redirect-manual-hosttest.exe'
for command in ([cc, '-O1', '-w', '-Iuser/include', '-Icommon', '-Ithird_party/bearssl/inc',
                 'tests/fetch_redirect_manual_hosttest.c', *owner, '-o', exe], [exe]):
    completed = subprocess.run([str(x) for x in command], cwd=root, env=env, capture_output=True)
    print((completed.stdout + completed.stderr).decode('utf-8', errors='replace'), end='', flush=True)
    if completed.returncode:
        raise SystemExit(completed.returncode)
