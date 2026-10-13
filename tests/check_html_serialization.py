"""Compile the exact HTML/XML serializer with allocator/checkpoint fault probes.

Only the native tree fixture, cancellation and node_attr consumers are shims.
This supplements native JS and real-site acceptance; it is not rendering evidence.
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/xml-serialization-host"
out.mkdir(parents=True, exist_ok=True)
source = (root / "user/libc/web/html_serialize.c").read_text(encoding="utf-8")
header = (root / "user/libc/web/webi.h").read_text(encoding="utf-8")
dom = (root / "user/libc/web/dom.c").read_text(encoding="utf-8")
util = (root / "user/libc/web/util.c").read_text(encoding="utf-8")
contract = (root / "user/libc/web/html_serialize.h").read_text(encoding="utf-8")

def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct {char *p;size_t n,cap;} sbuf;
typedef struct web_doc {bool live,inert;struct web_doc *dom_family;} web_doc;
typedef struct node node_t;
struct attr {const char *name,*raw,*value,*namespace_uri,*prefix,*local;node_t *node;};
struct node {web_doc *owner;int type,namespace_id,tag,nattrs;size_t textlen,public_id_len,system_id_len;
    const char *name,*raw_name,*custom_is,*public_id,*system_id;char *text;struct attr *attrs;
    node_t *first,*next,*parent,*template_content,*shadow_root;
    bool doctype_identifiers_sized,shadow_serializable,shadow_closed,shadow_delegates_focus,shadow_clonable,shadow_manual;
    int custom_registry_id;
};
static int allocations,fail_allocation=-1,live_allocations,checkpoints,cancel_at=-1;
static void *fault_malloc(size_t n){if(allocations++==fail_allocation)return NULL;void *p=malloc(n);if(p)live_allocations++;return p;}
static void *fault_realloc(void *p,size_t n){if(allocations++==fail_allocation)return NULL;void *q=realloc(p,n);if(q&&!p)live_allocations++;return q;}
static void fault_free(void *p){if(p)live_allocations--;free(p);}
#define malloc fault_malloc
#define realloc fault_realloc
#define free fault_free
static web_doc *last_checkpoint_document;
static bool web_native_checkpoint(web_doc *d){last_checkpoint_document=d;return checkpoints++!=cancel_at;}
static const char *node_attr(const node_t *n,const char *name){for(int i=0;i<n->nattrs;i++)if(!strcmp(n->attrs[i].name,name))return n->attrs[i].value;return NULL;}
static int lower(int c){return c>='A'&&c<='Z'?c+32:c;}
'''
tags = between(header, "#define WEB_TAGS(X)", "int tag_lookup(")
types = between(header, "enum { N_DOC", "struct attr {")
statuses = between(contract, "enum xml_serialization_result", "/* XMLSerializer")
buffer = between(util, "static void sb_grow(", "int utf8_put(")
buffer += between(util, "char *sb_cstr(", "void pv_push(")
compare = between(util, "bool str_ieq(", "bool strn_ieq(")
name_validation = between(dom, "static bool xml_name_start(", "void doc_create_diagnostic(")
cases = (root / "tests/html_serialization_host_cases.c").read_text(encoding="utf-8")
generated = out / "serializer.c"
generated.write_text(prefix+tags+types+statuses+buffer+compare+name_validation+
                     source.replace('#include "html_serialize.h"', '')+cases, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH", ""))
exe = out / "serializer.exe"
log = out / "result.log"
with log.open("w", encoding="utf-8") as stream:
    for path in ("user/libc/web/html_serialize.c", "user/libc/web/html_serialize.h", "tests/html_serialization_host_cases.c"):
        stream.write(path+" SHA256 "+hashlib.sha256((root/path).read_bytes()).hexdigest()+"\n")
    stream.flush()
    subprocess.run([str(cc), "-std=c11", "-Wall", "-Wextra", "-O1", str(generated), "-o", str(exe)],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe)], cwd=root, env=env, timeout=20,
                            stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding="utf-8"))
raise SystemExit(result.returncode)
