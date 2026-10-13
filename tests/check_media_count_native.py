"""Focused native media registry or cooperative service boundary; no old suites."""
from pathlib import Path
import os
import subprocess
import sys
root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
source = (root / "user/libc/web/avmedia.c").read_text(encoding="utf-8")


def service_cadence():
    """Compile the production service, not a replacement scheduling model."""
    at = source.index("void web_avmedia_service(")
    service = source[at:source.index("\n}", at)+2]
    prefix = r'''#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct web_doc {bool live;struct web_doc *dom_family;} web_doc;
typedef struct node_t {web_doc *owner;} node_t;
struct web_avmedia {
    struct web_avmedia *next;web_doc *doc;node_t *node;
    bool playing,mse_active,mse_audio_wait,mse_preparing,url_input;
    double rate;int fd;void *mse_worker,*worker;
    struct {int kind;} pending,ahead;
};
static struct web_avmedia *streams,*service_cursor,*last_pumped;
static bool service_running,service_clock_set,reenter;
static unsigned native_depth,pumps,publishes,clock_queries,pcm_reads;
static uint64_t service_at,fake_clock,pump_work=3,patch_work=1;
static uint32_t pcm_queued;
static uint64_t uptime_ms(void){clock_queries++;return fake_clock;}
static int mock_read(int fd,void *p,size_t n){(void)fd;pcm_reads++;*(uint32_t *)p=pcm_queued;return (int)n;}
#define read mock_read
static bool nmedia_mse_worker_needs_service(void *p){return p!=NULL;}
static bool nmedia_mse_worker_pending(void *p){return p!=NULL;}
static bool nmedia_mse_worker_service_due(void *p,uint64_t now){(void)now;return p!=NULL;}
static bool nmedia_worker_running(void *p){return p!=NULL;}
void web_avmedia_service(uint64_t now);
static void pump(struct web_avmedia *s,uint64_t now,uint64_t until,bool cooperative){
    (void)now;(void)until;(void)cooperative;pumps++;last_pumped=s;
    fake_clock+=pump_work;if(reenter)web_avmedia_service(fake_clock);
}
static void snapshot_publish(struct web_avmedia *s){(void)s;publishes++;fake_clock+=patch_work;}
'''
    cases = r'''
static int checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
int main(void){
    web_doc doc={.live=true};node_t node={.owner=&doc};
    struct web_avmedia a={.doc=&doc,.node=&node,.playing=true,.rate=1,.fd=-1,.url_input=true,.worker=&doc};
    struct web_avmedia b=a;
    fake_clock=10;web_avmedia_service(fake_clock);
    check(!pumps&&!clock_queries,"empty registry avoids service work");
    streams=&a;a.pending.kind=1;web_avmedia_service(fake_clock);
    check(pumps==1&&publishes==1&&service_at==14,"wire and patch work charge completion clock");
    unsigned clocks=clock_queries,reads=pcm_reads;
    web_avmedia_service(fake_clock);
    check(pumps==1&&clock_queries==clocks&&pcm_reads==reads,"same completion millisecond rejects before FD or clock work");
    fake_clock=15;web_avmedia_service(fake_clock);
    check(pumps==2&&service_at==19,"pending media keeps one millisecond completed-service interval");
    a.pending.kind=0;fake_clock=20;web_avmedia_service(fake_clock);
    check(pumps==2,"ordinary supply does not consume its own five millisecond interval");
    fake_clock=24;web_avmedia_service(fake_clock);
    check(pumps==3&&service_at==28,"ordinary supply resumes after completed-service interval");
    fake_clock=2;web_avmedia_service(fake_clock);
    check(pumps==4&&service_at==6,"backwards clock does not strand live media");
    a.fd=3;pcm_queued=1439;fake_clock=7;web_avmedia_service(fake_clock);
    check(pumps==5&&service_at==11,"low PCM retains urgent interval without pending pixels");
    pcm_queued=1440;fake_clock=12;web_avmedia_service(fake_clock);
    check(pumps==5,"sufficient PCM retains ordinary interval");
    a.pending.kind=1;native_depth=1;fake_clock=20;clocks=clock_queries;web_avmedia_service(fake_clock);
    check(pumps==5&&clock_queries==clocks,"native ownership suppresses cooperative reentry");
    native_depth=0;reenter=true;web_avmedia_service(fake_clock);reenter=false;
    check(pumps==6&&publishes==6&&!service_running,"pump cannot recursively service another media owner");
    a.next=&b;service_cursor=NULL;service_clock_set=false;a.fd=-1;fake_clock=30;web_avmedia_service(fake_clock);
    check(last_pumped==&a,"first live registry owner serviced");
    b.pending.kind=1;fake_clock=35;web_avmedia_service(fake_clock);
    check(last_pumped==&b,"round robin preserves next live owner");
    doc.live=false;fake_clock=45;unsigned before=pumps;web_avmedia_service(fake_clock);
    check(pumps==before,"retired document is not pumped");
    doc.live=true;node.owner=NULL;fake_clock=50;web_avmedia_service(fake_clock);
    check(pumps==before,"detached document owner is not pumped");
    node.owner=&doc;a.next=NULL;streams=&a;service_cursor=NULL;a.mse_active=true;a.mse_worker=&doc;a.pending.kind=0;
    fake_clock=55;web_avmedia_service(fake_clock);
    check(pumps==before+1,"MSE native collection remains available");
    printf("media-service-cadence: %d checks, %d failures\n",checks,failures);return failures!=0;
}
'''
    out = root / "build/browser-media-cadence-host-20261013"
    out.mkdir(parents=True, exist_ok=True)
    c = out / "media-service-cadence-generated.c"
    c.write_text(prefix+service+cases, encoding="utf-8")
    cc = root / "tools/msys64/ucrt64/bin/clang.exe"
    env = dict(os.environ, PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH", ""))
    exe = out / "media-service-cadence.exe"
    subprocess.run([str(cc), "-std=gnu11", "-O1", str(c), "-o", str(exe)], cwd=root, env=env, check=True)
    subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=20)


if "--service-cadence" in sys.argv:
    service_cadence()
    raise SystemExit(0)

at = source.index("static struct web_avmedia *find(")
find = source[at:source.index("\n}", at)+2]
assert "DOCUMENT_MEDIA_LIMIT" not in source and "DOCUMENT_DECODER_LIMIT" not in source
prefix = '''#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
typedef struct {int identity;} web_doc;
typedef struct {int identity;} node_t;
struct web_avmedia {struct web_avmedia*next;web_doc*doc;node_t*node;int fd,mse_pending_slot;bool audio_enabled,video_enabled;double volume;};
static struct web_avmedia *streams;
static bool fail;
static void *nmedia_ff_mallocz(size_t n){return fail?NULL:calloc(1,n);}
'''
cases = '''
static int checks,failures;
static void check(bool ok,const char*name){checks++;if(!ok){failures++;printf("FAIL %s\\n",name);}}
int main(void){
    web_doc a={1},b={2};node_t nodes[161]={0};
    for(int i=0;i<160;i++)check(find(&a,&nodes[i],true)!=NULL,"stream creation beyond old64");
    struct web_avmedia *old=find(&a,&nodes[159],false);check(old && find(&a,&nodes[159],true)==old,"live stream identity stable");
    check(find(&b,&nodes[159],true)!=old,"same node separate native document lookup");
    struct web_avmedia*head=streams;fail=true;
    check(find(&a,&nodes[160],true)==NULL && streams==head,"actual allocation failure leaves list unchanged");
    check(find(&a,&nodes[159],true)==old,"existing stream accessible during allocation failure");
    check(old->fd==-1 && old->mse_pending_slot==-1 && old->audio_enabled && old->video_enabled && old->volume==1,"defaults preserved");
    check(find(&a,&nodes[160],false)==NULL,"noncreation does not fabricate media state");
    while(streams){struct web_avmedia*next=streams->next;free(streams);streams=next;}
    printf("media-count-native: %d checks, %d failures\\n",checks,failures);return failures!=0;
}
'''
c = out / "media-count-native-generated.c"
c.write_text(prefix+find+cases,encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe = out / "media-count-native.exe"
subprocess.run([str(cc),"-std=gnu11","-O1",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=20)
