"""Only the new same-forest notification boundary; no prior GC/cache suite."""
from pathlib import Path
import os
import re
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
dom=(root / "user/libc/web/dom.c").read_text(encoding="utf-8")
cache=(root / "user/libc/web/js_nodes.h").read_text(encoding="utf-8")
def function(source,signature):
    at=source.index(signature)
    return source[at:source.index("\n}",at)+2]+"\n"
funcs=""
for sig in ("static node_t *node_tree_root(","bool web_js_nodes_same_forest("):
    funcs+=function(cache,sig)
for sig in ("static void indices(","static bool under(","node_t *doc_shadow_parent(","node_t *doc_node_root(","static bool shadow_under(","static void changed(","void doc_mutated(","static bool resource_subtree(","static bool resource_ancestor(","static void structure_changed_lifetime(","static void structure_changed(","void doc_attrs_publish(","static void detach(","static int tree_depth(","static bool may_insert(","bool doc_node_move(","void doc_node_remove("):
    funcs+=function(dom,sig)
cases=(root / "tests/same_forest_native_cases.c").read_text(encoding="utf-8")
tags=sorted(set(re.findall(r"\bT_[A-Za-z0-9_]+",funcs+cases)))
c=out / "same-forest-native-generated.c"
c.write_text(cases.replace("/* PRODUCT_TAGS */","enum {"+",".join(tags)+"};").replace("/* PRODUCT_FUNCTIONS */",funcs.replace("void doc_node_remove(","static void doc_node_remove(")),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "same-forest-native.exe"
subprocess.run([str(cc),"-std=gnu11","-O1",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=10)
