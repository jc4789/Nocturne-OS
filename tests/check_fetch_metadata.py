"""Fetch の Accept・Accept-Language と Fetch Metadata（Sec-Fetch-Dest・Mode・Site・User）の
要求の頭を、user/apps/webfetch.c の製品の関数を文字列のまま切り出して組み、割り当ての失敗も
注入して確かめるホストの試験。範囲を限った裏付けで、実際の HTTP や QEMU での受け入れではない。

stub は前置きの n_user_languages（言語の一覧は試験が決める）、webcookie_same_site（固定の
登録可能ドメインの表）、webcookie_get（cookie なし）だけ。fail・頭の緩衝・URL の正規化
（http.c と webfetch.c の http_url）は製品の本文を使う。
"""
from pathlib import Path
import hashlib
import os
import subprocess

root=Path(__file__).resolve().parent.parent
out=root/'build/fetch-metadata-host';out.mkdir(parents=True,exist_ok=True)
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
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include "http.h"
#include "webnet_wire.h"
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
static int allocations,fail_allocation=-1,live_allocations;
static void *fault_malloc(size_t n){if(allocations++==fail_allocation)return NULL;void *p=malloc(n);if(p)live_allocations++;return p;}
static void *fault_realloc(void *p,size_t n){if(allocations++==fail_allocation)return NULL;void *q=realloc(p,n);if(q&&!p)live_allocations++;return q;}
static void fault_free(void *p){if(p)live_allocations--;free(p);}
static char *fault_strdup(const char *s){size_t n=strlen(s);char *p=fault_malloc(n+1);if(p)memcpy(p,s,n+1);return p;}
#define malloc fault_malloc
#define realloc fault_realloc
#define free fault_free
#define strdup fault_strdup
struct header_buffer {char *p;size_t length,capacity;};
/* 製品の struct job のうち、切り出した関数が触れる欄だけ。 */
struct job {
    struct webnet_wire_request wire;
    char *document,*headers,*origin,*target_origin,*top_level_url,*referrer,*referrer_origin,*final_url;
    int referrer_policy;
    char error[160];
    struct header_buffer outgoing;
    webcookie_jar *cookies;
    bool hop_cookies,redirect_cross_site,cors_tainted,origin_tainted;
    struct webnet_wire_fetch fetch;
    unsigned fetch_site;
};
/* stub：利用者の言語の一覧。試験が languages_value を決める。形は "ja,en-US" のような
   コンマ区切りで、戻り値は長さ、cap に入らなければ負（N_ERR_TOO_LARGE）。 */
static const char *languages_value="";
static unsigned languages_calls;
n_result n_user_languages(char *out,size_t cap);
n_result n_user_languages(char *out,size_t cap){
    languages_calls++;
    size_t n=strlen(languages_value);
    if(n+1>cap)return -N_ERR_TOO_LARGE;
    memcpy(out,languages_value,n+1);return (n_result)n;
}
/* stub：same site の判断。公開接尾辞の一覧の代わりに、試験に出る host ごとの登録可能
   ドメインを固定の表で書く。ここでは "test" と "github.test" を公開接尾辞とみなすので、
   www.example.test と example.test は同じ登録可能ドメイン（example.test）、
   alice.github.test と bob.github.test は別の登録可能ドメインになる。IP と localhost は
   host そのもの。末尾の点の付いた綴りの登録可能ドメインは点を残すので、点の無い綴りとは
   別になる。HTML の same site：両方が tuple origin で scheme が同じ、かつ host の
   登録可能ドメインが同じ（port は問わない）。表に無い host は unknown_site_hosts に数える。 */
static const struct {const char *host,*registrable;} site_table[]={
    {"example.test","example.test"},
    {"www.example.test","example.test"},
    {"example.test.","example.test."},
    {"www.example.test.","example.test."},
    {"api.example.test","example.test"},
    {"other.test","other.test"},
    {"www.other.test","other.test"},
    {"alice.github.test","alice.github.test"},
    {"bob.github.test","bob.github.test"},
    {"localhost","localhost"},
    {"127.0.0.1","127.0.0.1"},
};
static unsigned same_site_calls,unknown_site_hosts;
static const char *registrable_domain(const char *url,bool *secure){
    const char *host;
    if(!strncmp(url,"https://",8)){*secure=true;host=url+8;}
    else if(!strncmp(url,"http://",7)){*secure=false;host=url+7;}
    else return NULL;
    size_t n=strcspn(host,":/?#");
    for(size_t i=0;i<sizeof site_table/sizeof *site_table;i++)
        if(strlen(site_table[i].host)==n&&!memcmp(site_table[i].host,host,n))return site_table[i].registrable;
    unknown_site_hosts++;return NULL;
}
bool webcookie_same_site(const webcookie_jar *jar,const char *a,const char *b){
    (void)jar;same_site_calls++;
    bool secure_a=false,secure_b=false;
    const char *site_a=registrable_domain(a,&secure_a),*site_b=registrable_domain(b,&secure_b);
    return site_a&&site_b&&secure_a==secure_b&&!strcmp(site_a,site_b);
}
/* stub：cookie の瓶は空。prepare_outgoing の Cookie の頭は試験の対象外。 */
static unsigned cookie_reads;
long webcookie_get(webcookie_jar *jar,const struct webcookie_context *c,char *out,size_t cap,int64_t now){
    (void)jar;(void)c;(void)now;cookie_reads++;if(out&&cap)out[0]=0;return 0;
}
'''
# URL の正規化は製品の実装（http.c と webfetch.c の http_url）で、試験用の解析器ではない。
product=between(http,'void url_owned_free(','struct rd {')
product+=between(fetch,'static bool top_level_navigation(','static bool cache_context_allowed(')
product+=between(fetch,'static bool fail(','static bool header_reserve(')
product+=between(fetch,'static bool header_reserve(','static int remaining(')
product+=between(fetch,'static enum http_url_result http_url(','static bool url_failure(')
product+=between(fetch,'static bool potentially_trustworthy(','/* WHATWG host parsing')
product+=between(fetch,'static bool no_cors(','static bool forbidden_header(')
product+=between(fetch,'static bool field_name_is(','/* Iterate every field occurrence')
product+=between(fetch,'static const char *cookie_site_url(','static bool cache_partition(')
product+=between(fetch,'/* Fetch: a navigation request','/* Browser-generated fields remain')
product+=between(fetch,'static bool cache_headers(','/* Headers have already been validated.')
generated=out/'fetch_metadata.c'
generated.write_text(prefix+product+(root/'tests/fetch_metadata_host_cases.c').read_text(encoding='utf-8'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
log=out/'result.log';exe=out/'fetch_metadata.exe'
with log.open('w',encoding='utf-8') as stream:
    for name in ['user/apps/webfetch.c','user/libc/http.c','user/include/webnet.h','user/include/webnet_wire.h','tests/fetch_metadata_host_cases.c']:
        stream.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    stream.flush()
    subprocess.run([str(cc),'-std=gnu11','-O1','-Wall','-Wextra','-idirafter','user/include','-idirafter','common',str(generated),'-o',str(exe)],cwd=root,env=env,check=True,stdout=stream,stderr=subprocess.STDOUT)
    result=subprocess.run([str(exe)],cwd=root,env=env,timeout=20,stdout=stream,stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'));raise SystemExit(result.returncode)
