"""One new stream-count/allocation boundary run; no old media suites."""
from pathlib import Path
import os
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / "build/goal-20261009"
source = (root / "user/libc/web/avmedia.c").read_text(encoding="utf-8")
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
