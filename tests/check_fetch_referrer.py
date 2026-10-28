"""Actual native URL/referrer/header policy functions, with allocation faults.
This is bounded supporting evidence, not real HTTP/QEMU acceptance.
The URL record's owner (user/libc/web/web_url.c on Lexbor, tests/url_owner.py) is linked as
the programs link it; the faults are injected into the product's own allocations, and only those
are counted (what the URL parser hands over is freed by whichever side then holds it).
"""
from pathlib import Path
import hashlib
import os
import subprocess
import url_owner

root=Path(__file__).resolve().parent.parent
out=root/'build/fetch-referrer-host';out.mkdir(parents=True,exist_ok=True)
fetch=(root/'user/apps/webfetch.c').read_text(encoding='utf-8')
http=(root/'user/libc/http.c').read_text(encoding='utf-8')
def between(source,start,end):
    at=source.index(start);return source[at:source.index(end,at)]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "http.h"
#include "webnet_wire.h"
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#ifdef SB_TOP
#undef SB_TOP
#endif
static int allocations,fail_allocation=-1,live_allocations;
static void *fault_live[4096];
static void fault_track(void *p){for(int i=0;i<4096;i++)if(!fault_live[i]){fault_live[i]=p;live_allocations++;return;}abort();}
static bool fault_untrack(void *p){for(int i=0;i<4096;i++)if(fault_live[i]==p){fault_live[i]=NULL;live_allocations--;return true;}return false;}
static void *fault_malloc(size_t n){if(allocations++==fail_allocation)return NULL;void *p=malloc(n);if(p)fault_track(p);return p;}
static void *fault_realloc(void *p,size_t n){if(allocations++==fail_allocation)return NULL;bool own=p&&fault_untrack(p);void *q=realloc(p,n);if(q)fault_track(q);else if(own)fault_track(p);return q;}
static void fault_free(void *p){if(p)fault_untrack(p);free(p);}
static char *fault_strdup(const char *s){size_t n=strlen(s);char *p=fault_malloc(n+1);if(p)memcpy(p,s,n+1);return p;}
#define malloc fault_malloc
#define realloc fault_realloc
#define free fault_free
#define strdup fault_strdup
struct header_buffer {char *p;size_t length,capacity;};
struct job {struct webnet_wire_request wire;char *referrer,*referrer_origin,*target_origin;int referrer_policy;struct header_buffer outgoing;char error[160];};
static bool fail(struct job*j,const char*s){snprintf(j->error,sizeof j->error,"%s",s);return false;}
'''
# The request layer's target is the production implementation (http.c's http_target_of, from the
# URL record), not a test URL parser.
product=between(http,'enum web_url_result http_target_of(', 'struct rd {')
product+=between(fetch,'static bool header_reserve(', 'static int remaining(')
product+=between(fetch,'static enum web_url_result http_url(', 'static bool url_failure(')
product+=between(fetch,'/* Referrer Policy\'s "strip url for use as a referrer"', 'static enum web_url_result image_http_url(')
product+=between(fetch,'static bool forbidden_header(', 'static bool unsafe_value(')
frame=(root/'user/libc/web/frame.c').read_text(encoding='utf-8')
sandbox=(root/'user/libc/web/sandbox.h').read_text(encoding='utf-8')
dispatcher=(root/'user/libc/web/js.c').read_text(encoding='utf-8')
product+=between(sandbox,'enum web_sandbox_flag {','#define SB_ALL')
product+='typedef struct web_doc {const char *url,*inherited_url;unsigned sandbox_flags;struct web_doc *frame_parent;} web_doc;\n'
product+=between(frame,'const char *web_effective_url(', 'bool web_frame_element(')
product+=between(dispatcher,'static const char *fetch_client_referrer_source(', 'static bool link_type(')
generated=out/'referrer.c'
generated.write_text(prefix+product+(root/'tests/referrer_policy_host_cases.c').read_text(encoding='utf-8'),encoding='utf-8')
cc=url_owner.clang(root)
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
log=out/'result.log';exe=out/'referrer.exe'
owner=url_owner.objects(root,cc)
with log.open('w',encoding='utf-8') as stream:
    for name in ['user/apps/webfetch.c','user/libc/http.c','user/include/http.h','user/include/weburl.h','user/libc/web/web_url.c','user/include/webnet.h','user/include/webnet_wire.h','user/libc/web/js.c','user/libc/web/frame.c','user/libc/web/sandbox.h','tests/referrer_policy_host_cases.c']:
        stream.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    stream.flush()
    subprocess.run([str(cc),'-std=gnu11','-O1','-Wall','-Wextra','-idirafter','user/include','-idirafter','common',str(generated),*owner,'-o',str(exe)],cwd=root,env=env,check=True,stdout=stream,stderr=subprocess.STDOUT)
    result=subprocess.run([str(exe)],cwd=root,env=env,timeout=20,stdout=stream,stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'));raise SystemExit(result.returncode)
