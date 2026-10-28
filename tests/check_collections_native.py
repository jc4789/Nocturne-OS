"""The DOM collections' actual native half and JS half on the product QuickJS.

tests/collections_native_cases.c gives them a small native tree in place of
dom.c and js.c's accessors; tests/collections_native_cases.js carries WPT's
expectations for legacy platform objects and HTMLCollection and what each
collection reads. A host contract only: not the OS, the browser's own DOM or a
WPT run.
"""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/collections-native'
out.mkdir(parents=True, exist_ok=True)
shim = out / 'include'
shim.mkdir(exist_ok=True)
# The engine's <nocturne.h> is libc's own; the cases define the calls it makes.
(shim / 'nocturne.h').write_text('#pragma once\n#undef SYS_OPEN\n#include "user/include/nocturne.h"\n',
                                 encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
engine = [root / ('third_party/quickjs/' + name + '.c') for name in ('quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils')]
exe = out / 'collections-native.exe'
log = out / 'collections-native.log'
with log.open('w', encoding='utf-8') as stream:
    command = [str(cc), '-std=gnu11', '-O1', '-fwrapv', '-funsigned-char', '-DCONFIG_NOCTURNE',
               '-DCONFIG_VERSION="Nocturne-test"', '-I' + str(shim), '-I.', '-Ithird_party/quickjs',
               '-Icommon', '-idirafter', 'user/include',
               str(root / 'tests/collections_native_cases.c'), *map(str, engine), '-lm', '-o', str(exe)]
    subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(exe), 'user/libc/web/js_collections.js', 'tests/collections_native_cases.js'],
                            cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=60)
print(log.read_text(encoding='utf-8'), end='')
raise SystemExit(result.returncode)
