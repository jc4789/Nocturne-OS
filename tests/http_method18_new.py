"""Exactly one runtime of new method-length/native-write/failure boundaries."""
from pathlib import Path
import hashlib,subprocess
root=Path(__file__).resolve().parent.parent
for path in ['user/libc/http.c','user/include/http.h','user/include/webnet.h','user/libc/webnet.c','user/apps/webfetch.c','user/libc/web/js.c','tests/http_method18_new.c']:
    print(path,'SHA256',hashlib.sha256((root/path).read_bytes()).hexdigest(),flush=True)
exe=root/'build/goal-20261009/http-method18-new-native.exe'
subprocess.run([str(root/'tools/msys64/ucrt64/bin/clang.exe'),'-std=gnu11','-O1','-Iuser/include','-Icommon','tests/http_method18_new.c','-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],cwd=root,check=True,timeout=20)
