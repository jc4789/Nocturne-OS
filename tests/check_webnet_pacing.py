"""Execute the unchanged native webnet pump with deterministic channel I/O.

This checks pacing, byte windows, deferred callbacks and GUI readiness. Real
network/GUI acceptance remains the QEMU WHPX suite, not this host fixture.
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/network-pacing"
out.mkdir(parents=True, exist_ok=True)
source_path = root / "user/libc/webnet.c"
source = source_path.read_text(encoding="utf-8")

def function(signature):
    at = source.index(signature)
    # Native functions keep their closing brace in column zero.
    return source[at:source.index("\n}", at) + 2] + "\n"

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "errno.h"
#include "webnet.h"
#include "webnet_wire.h"
#include "abi.h"
#define WNOHANG 1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
/* Native diagnostic FILE objects are not Windows CRT FILE objects. */
#define fprintf(...) ((void)0)
typedef intptr_t ssize_t;
struct fixture { unsigned char bytes[1024*1024];size_t length,position;bool hup;unsigned credits;size_t credited; };
static struct fixture channels[8];
static uint64_t clock_ms;
static int test_errno,closed_count,killed_count,wait_count,last_timeout,watched_gui,watched_net,net_events;
int *__errno_location(void){return &test_errno;}
uint64_t uptime_ms(void){return clock_ms;}
int close(int fd){(void)fd;closed_count++;return 0;}
int kill(int pid){(void)pid;killed_count++;return 0;}
int waitpid(int pid,int *status,int flags){(void)pid;(void)status;(void)flags;wait_count++;return 0;}
int sysinfo(struct n_sysinfo *p){memset(p,0,sizeof *p);p->free_mem=1ULL<<30;return 0;}
int poll(struct n_pollfd *p,int count,int timeout){
    last_timeout=timeout;watched_gui=watched_net=net_events=0;int ready=0;
    for(int i=0;i<count;i++){p[i].revents=0;if(p[i].fd==50){watched_gui++;continue;}
        if(p[i].fd<0||p[i].fd>=8)abort();watched_net++;net_events|=p[i].events;
        struct fixture *f=&channels[p[i].fd];
        if((p[i].events&N_POLLIN)&&f->position<f->length)p[i].revents|=N_POLLIN;
        if(p[i].events&N_POLLOUT)p[i].revents|=N_POLLOUT;
        if(f->hup)p[i].revents|=N_POLLHUP;
        ready+=p[i].revents!=0;
    }return ready;
}
ssize_t read(int fd,void *p,size_t length){
    struct fixture *f=&channels[fd];size_t got=MIN(length,f->length-f->position);
    if(!got){if(f->hup)return 0;test_errno=EAGAIN;return -1;}
    memcpy(p,f->bytes+f->position,got);f->position+=got;return (ssize_t)got;
}
ssize_t write(int fd,const void *p,size_t length){
    if(channels[fd].hup){test_errno=EPIPE;return -1;}
    if(length==sizeof(struct webnet_wire_credit)){
        struct webnet_wire_credit credit;memcpy(&credit,p,sizeof credit);
        if(credit.magic==WEBNET_CREDIT_MAGIC){channels[fd].credits++;channels[fd].credited+=credit.bytes;}
    }return (ssize_t)length;
}
'''
types = source[source.index("#define WORKERS"):source.index("static void request_free(")]
shim = r'''
static unsigned worker_budget(void){return WORKERS;}
static bool cookie_events(webnet *n,struct request *r){(void)n;(void)r;return true;}
static bool consume_request_diagnostic(struct request *r,char *h){(void)r;(void)h;return true;}
static bool consume_transport_diagnostic(struct request *r,char *h){(void)r;(void)h;return true;}
static void request_free(struct request *r){free(r->rx);free(r->tx);free(r);}
static bool start(webnet *n,struct slot *s,struct request *r){(void)n;s->rq=r;r->finishing=true;return false;}
'''
signatures = ["static void slot_close(", "static bool slot_reap(", "static void slot_stop(",
              "static void reap_paused_stream(", "static bool origin_available(", "static unsigned live_workers(",
              "static struct request **available_class(", "static struct request *take_request(",
              "static void complete(", "static bool unpack(", "static bool stream_frame_valid(",
              "static bool dispatch_stream_frame("]
extracted = types + shim + "".join(function(s) for s in signatures)
extracted += source[source.index("struct pump_budget {"):source.index("void webnet_pump(")]
# These public one-line wrappers are deliberately obtained without rewriting.
for name in ("void webnet_pump(", "void webnet_service(", "bool webnet_busy(", "unsigned webnet_inflight("):
    at = source.index(name)
    extracted += source[at:source.index("\n", at)] + "\n"
for name in ("int webnet_timeout(", "static void wait_slot(", "int webnet_wait(", "void webnet_resume("):
    extracted += function(name)
cases = root / "tests/webnet_pacing_cases.c"
generated = out / "webnet-pacing-generated.c"
generated.write_text(prefix + extracted + cases.read_text(encoding="utf-8"), encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
exe = out / "webnet-pacing.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
command = [str(cc), "-std=gnu11", "-O1", "-Iuser/include", "-Icommon", str(generated), "-o", str(exe)]
with (out / "webnet-pacing-result.log").open("w", encoding="utf-8") as log:
    for path in (source_path, root / "user/include/webnet.h", root / "user/include/webnet_wire.h", cases, Path(__file__)):
        log.write(str(path.relative_to(root)) + " SHA256 " + hashlib.sha256(path.read_bytes()).hexdigest() + "\n")
    log.write("COMMAND " + repr(command) + "\n")
    log.flush()
    for step in (command, [str(exe)]):
        result = subprocess.run(step, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        diagnostic = result.stdout.decode("utf-8", errors="replace")
        print(diagnostic, end="")
        log.write(diagnostic + "EXIT " + str(result.returncode) + "\n")
        log.flush()
        if result.returncode:
            raise SystemExit(result.returncode)
