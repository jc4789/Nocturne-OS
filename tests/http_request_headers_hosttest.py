"""Run new outgoing-header boundaries once; no old HTTP/browser suite replay."""
from pathlib import Path
import hashlib
import subprocess

source = Path('user/apps/webfetch.c').read_text(encoding='utf-8')
def part(start, end):
    at = source.index(start)
    return source[at:source.index(end, at)]
out = Path('build/goal-20261009/http-request-headers-tested.inc')
out.write_text(part('struct header_buffer', 'static bool read_all') +
    part('static bool fail(', 'static int remaining(') +
    part('static bool token_char(', '/* Validate before') +
    part('static bool cors_kind(', '/* Headers have already') +
    part('static bool cors_allowed(', 'static int cookie_header(') +
    part('static bool preflight(', 'static bool js_mime('),
    encoding='utf-8', newline='\n')
for name in ['user/apps/webfetch.c', 'user/libc/webnet.c', 'user/libc/web/js.c',
             'user/include/webnet_wire.h', str(out), 'tests/http_request_headers_hosttest.c']:
    print(name, 'SHA256', hashlib.sha256(Path(name).read_bytes()).hexdigest(), flush=True)
exe = 'build/goal-20261009/http-request-headers-host.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe', '-O2', '-idirafter', 'user/include',
                'tests/http_request_headers_hosttest.c', '-o', exe], check=True)
subprocess.run([exe], check=True)
