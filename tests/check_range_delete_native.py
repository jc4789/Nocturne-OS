"""One new deletion boundary run: actual bindings + C native tree/remove.
Construction, host notifications and CharacterData storage are fixture edges;
no rendering/site/OS/complete CE or MutationObserver acceptance is claimed.
"""
from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parent.parent
out=root / 'build/goal-20261009'
dom=(root / 'user/libc/web/dom.c').read_text(encoding='utf-8')
def fn(signature):
    a=dom.index(signature);return dom[a:dom.index('\n}',a)+2]+'\n'
c=(root / 'tests/range_delete_native_cases.c').read_text(encoding='utf-8')
c=c.replace('/* PRODUCT_REMOVE */',fn('static void detach(')+fn('void doc_node_remove('))
(out / 'range-delete-native-generated.c').write_text(c,encoding='utf-8')
prefix=(root / 'tests/range_delete_native_prefix.js').read_text(encoding='utf-8')
js=prefix+(root / 'user/libc/web/js_range.js').read_text(encoding='utf-8')+'\n'+(root / 'user/libc/web/js_document_selection.js').read_text(encoding='utf-8')+'\n'+(root / 'tests/js_range_delete_cases.js').read_text(encoding='utf-8')+'\n})();'
(out / 'range-delete-native-bindings.js').write_text(js,encoding='utf-8')
cc=root / 'tools/msys64/ucrt64/bin/clang.exe';exe=out / 'range-delete-native.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
engine=[root / ('third_party/quickjs/'+n+'.c') for n in ('quickjs','dtoa','libregexp','libunicode','cutils')]
subprocess.run([str(cc),'-std=gnu11','-O1','-fwrapv','-funsigned-char','-DCONFIG_NOCTURNE','-DCONFIG_VERSION="Nocturne-test"','-Ithird_party/quickjs','-Ibuild/nocturne-audit/url-host-include',str(out / 'range-delete-native-generated.c'),*map(str,engine),'-lm','-o',str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
