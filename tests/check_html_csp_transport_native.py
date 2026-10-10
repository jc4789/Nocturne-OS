"""現行 production 境界を抽出して実行する補助試験。guest 配送を主張しない。"""
from pathlib import Path
import hashlib, json, subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/html-parser-20261010'
out.mkdir(parents=True, exist_ok=True)
paths = ['user/libc/webnet.c', 'user/apps/webfetch.c']
net, worker = [root.joinpath(p).read_text(encoding='utf-8') for p in paths]

def between(text, begin, end):
    start = text.index(begin)
    return text[start:text.index(end, start)]

parts = [net[:net.index('static void load_cookie_psl')],
         between(net, 'uint64_t webnet_submit', 'static bool start('),
         worker[:worker.index('static bool read_all(')],
         between(worker, 'static bool fail(', '/* Only publish a replacement'),
         between(worker, 'static bool no_cors(', '/* Headers have already been validated'),
         between(worker, 'static int body_cb(', 'static int discard_cb('),
         between(worker, 'static bool exposed_header(', 'static bool run_http(')]
out.joinpath('html-csp-transport-tested.inc').write_text('\n'.join(parts), encoding='utf-8')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
exe = out / 'html-csp-transport-hosttest.exe'
commands = [[cc, '-O2', '-Iuser/include', '-Icommon', 'tests/html_csp_transport_hosttest.c', '-o', exe], [exe]]
results = []
for command in commands:
    completed = subprocess.run([str(x) for x in command], cwd=root, capture_output=True)
    text = (completed.stdout + completed.stderr).decode('utf-8', errors='replace')
    print(text, end='', flush=True)
    results.append({'argv': [str(x) for x in command], 'exit': completed.returncode, 'output': text})
    out.joinpath('html-csp-transport-results.json').write_text(json.dumps({'sources': {p: hashlib.sha256(root.joinpath(p).read_bytes()).hexdigest() for p in paths}, 'results': results}, ensure_ascii=False, indent=2), encoding='utf-8')
    if completed.returncode:
        raise SystemExit(completed.returncode)
