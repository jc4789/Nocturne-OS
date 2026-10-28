"""Fetch の Accept・Accept-Language と Fetch Metadata（Sec-Fetch-Dest・Mode・Site・User）の
要求の頭を、user/apps/webfetch.c の製品の関数を文字列のまま切り出して組み、割り当ての失敗も
注入して確かめるホストの試験。範囲を限った裏付けで、実際の HTTP や QEMU での受け入れではない。

Accept-Language は、窓の側（user/libc/webnet.c の request_language と wire_request）が fetch
の始まりに webprefs から一度決めて要求の wire の別の欄に入れ、webfetch はその値だけを使う。
両方の製品の本文を切り出して組み、wire の並びと、どの跳びでも同じ値か、作者の値が勝つか、
不正な値を拒むか、WebSocket の握手と CORS-preflight を確かめる。wire を読む run_one は切り
出せないので、試験の take_language が文書どおりの並びで読み、製品と同じ検査
（webnet_wire_language_valid）を通す。

stub は前置きの webprefs_accept_language（値は試験が決める）、webcookie_export（cookie なし）、
webcookie_same_site（固定の登録可能ドメインの表）、webcookie_get（cookie なし）、WebSocket の
握手の乱数・SHA-1・base64・接続（送った bytes を写し、応答は無い）、preflight の転送（送った頭を
写して失敗を返す）と cors_allowed・request_cancelled だけ。fail・頭の緩衝・URL の正規化（http.c
と webfetch.c の http_url）・残り時間（remaining）は製品の本文を使う。
"""
from pathlib import Path
import hashlib
import os
import subprocess

root=Path(__file__).resolve().parent.parent
out=root/'build/fetch-metadata-host';out.mkdir(parents=True,exist_ok=True)
fetch=(root/'user/apps/webfetch.c').read_text(encoding='utf-8')
net=(root/'user/libc/webnet.c').read_text(encoding='utf-8')
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
#include "webprefs.h"
#include "bearssl.h"
#include "browser_identity.h"
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
    char accept_language[WEBNET_WIRE_ACCEPT_LANGUAGE_MAX + 1];
    size_t body_len;
    char *partition;
};
/* 製品の struct webnet のうち、wire_request が触れる欄だけ。 */
struct webnet { webcookie_jar *cookies; };
/* stub：ブラウザーの好みの Accept-Language（webprefs.h）。試験が prefs_value と prefs_result
   を決める。prefs_result が 0 なら値の長さを返し、cap に入らなければ -N_ERR_TOO_LARGE。 */
static const char *prefs_value="";
static n_result prefs_result;
static unsigned prefs_calls;
n_result webprefs_accept_language(char *out,size_t cap){
    prefs_calls++;
    if(prefs_result)return prefs_result;
    size_t n=strlen(prefs_value);
    if(n+1>cap)return -N_ERR_TOO_LARGE;
    memcpy(out,prefs_value,n+1);return (n_result)n;
}
/* stub：cookie の瓶の写しは空。 */
long webcookie_export(webcookie_jar *jar,void *out,size_t cap,int64_t now){(void)jar;(void)out;(void)cap;(void)now;return 0;}
/* stub：時計。remaining は製品の本文で、試験の要求の締切は十分に先。 */
uint64_t n_clock_uptime_ms(void){return 1000;}
/* stub：preflight の転送。送った頭を写し、応答は無く失敗にする。 */
static char preflight_sent[1024];
static unsigned preflight_requests;
int http_request_stream_ex(const struct http_req *q,struct http_resp *r,size_t limit,const struct http_stream_hooks *hooks,
                           struct http_transport_timing *timing,const char *partition,struct http_transport_result *result){
    (void)limit;(void)hooks;(void)timing;(void)partition;(void)result;
    preflight_requests++;memset(r,0,sizeof *r);
    snprintf(preflight_sent,sizeof preflight_sent,"%s %s\n%s",q->method,q->url,q->headers?q->headers:"");
    snprintf(r->error,sizeof r->error,"試験の stub は応答しない");return -1;
}
void http_resp_free(struct http_resp *r){(void)r;}
const char *http_response_headers(const struct http_resp *r){return r->headers;}
static bool cors_allowed(struct job *j,const struct http_resp *r){(void)j;(void)r;return false;}
static int request_cancelled(void *opaque){(void)opaque;return 0;}
/* stub：WebSocket の握手の乱数・SHA-1・base64 と接続。送った bytes を写し、応答は無い。 */
void br_hmac_drbg_generate(br_hmac_drbg_context *ctx,void *out,size_t len){(void)ctx;memset(out,0x5a,len);}
void br_sha1_init(br_sha1_context *ctx){(void)ctx;}
void br_sha1_update(br_sha1_context *ctx,const void *data,size_t len){(void)ctx;(void)data;(void)len;}
void br_sha1_out(const br_sha1_context *ctx,void *out){(void)ctx;memset(out,0,20);}
char *base64_encode(const void *data,size_t n,size_t *outlen){
    (void)data;size_t length=(n+2)/3*4;char *p=malloc(length+1);
    if(!p)return NULL;memset(p,'A',length);p[length]=0;*outlen=length;return p;
}
static char socket_sent[2048];
long ns_write(net_stream *s,const void *buf,size_t n){(void)s;size_t k=n<sizeof socket_sent-1?n:sizeof socket_sent-1;memcpy(socket_sent,buf,k);socket_sent[k]=0;return (long)n;}
long ns_read(net_stream *s,void *buf,size_t n){(void)s;(void)buf;(void)n;return 0;}
const char *ns_error(net_stream *s){(void)s;return "試験の stub は応答しない";}
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
# 窓の側：fetch の始まりに Accept-Language を決めて要求の wire に入れる。
product+=between(net,'static void request_language(','uint64_t webnet_submit(')
# CORS-preflight と WebSocket の握手が頭を作る製品の本文。
product+=between(fetch,'static int remaining(','static bool token_char(')
product+=between(fetch,'static bool list_has_slice(','static bool list_has(')
product+=between(fetch,'static bool field_value_next(','static bool field_values_equal(')
product+=between(fetch,'static bool field_slice(','static bool field(')
product+=between(fetch,'static int discard_cb(','static bool js_mime(')
product+=between(fetch,'struct websocket {','static bool ws_record(')
# stub：握手の後の OPEN の記録は窓への channel に書くので、ここでは数えるだけ。
product+='static unsigned socket_records;\nstatic bool ws_record(struct websocket *ws,uint32_t magic,uint32_t code,const void *data,size_t length){(void)ws;(void)magic;(void)code;(void)data;(void)length;socket_records++;return true;}\n'
product+=between(fetch,'static bool ws_handshake(','static bool run_websocket(')
generated=out/'fetch_metadata.c'
generated.write_text(prefix+product+(root/'tests/fetch_metadata_host_cases.c').read_text(encoding='utf-8'),encoding='utf-8')
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
log=out/'result.log';exe=out/'fetch_metadata.exe'
with log.open('w',encoding='utf-8') as stream:
    for name in ['user/apps/webfetch.c','user/libc/webnet.c','user/libc/http.c','user/include/webnet.h','user/include/webnet_wire.h','user/include/webprefs.h','tests/fetch_metadata_host_cases.c']:
        stream.write(name+' SHA256 '+hashlib.sha256((root/name).read_bytes()).hexdigest()+'\n')
    stream.flush()
    subprocess.run([str(cc),'-std=gnu11','-O1','-Wall','-Wextra','-idirafter','user/include','-idirafter','common','-idirafter','third_party/bearssl/inc',str(generated),'-o',str(exe)],cwd=root,env=env,check=True,stdout=stream,stderr=subprocess.STDOUT)
    result=subprocess.run([str(exe)],cwd=root,env=env,timeout=20,stdout=stream,stderr=subprocess.STDOUT)
print(log.read_text(encoding='utf-8'));raise SystemExit(result.returncode)
