"""New HTTP/header negotiation conditions, run once. Never old suite replay."""
from pathlib import Path
import hashlib
import subprocess
source=Path('user/apps/webfetch.c').read_text(encoding='utf-8')
def part(start,end):
    at=source.index(start)
    return source[at:source.index(end,at)]
out=Path('build/goal-20261009/http-response-filter-tested.inc')
out.write_text(part('static bool list_has_slice','static bool field_exists')+
               part('static bool exposed_header','static bool run_http'),encoding='utf-8',newline='\n')
for name in ['user/libc/http.c','user/include/http.h','user/include/webnet_wire.h',str(out),'tests/http_dynamic_headers_hosttest.c']:
    print(name,'SHA256',hashlib.sha256(Path(name).read_bytes()).hexdigest(),flush=True)
exe='build/goal-20261009/http-dynamic-headers-host.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-O2','-idirafter','user/include',
                'tests/http_dynamic_headers_hosttest.c','-o',exe],check=True)
subprocess.run([exe],check=True)
