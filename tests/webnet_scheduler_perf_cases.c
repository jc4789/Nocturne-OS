static unsigned checks, failures;
static void check(bool okay,const char *name){checks++;if(!okay){failures++;printf("FAIL %s\n",name);}}
static struct request request(unsigned priority,uint64_t id,const char *origin){
    struct request r={.priority=(int)priority,.id=id,.transport_origin=(char *)origin};return r;
}
static void queue(webnet *n,struct request *r){
    struct request **at=&n->queue;while(*at&&(*at)->priority<=r->priority)at=&(*at)->next;
    r->next=*at;*at=r;
}
static void test_budget_values(void){
    test_cpu.online_cpus=4;test_memory.free_mem=UINT64_C(4096)*1024*1024;
    check(worker_budget()==16,"four CPUs select sixteen native workers");
    test_cpu.online_cpus=64;check(worker_budget()==32,"physical budget never exceeds thirty-two workers");
    test_memory.free_mem=128u*1024u*1024u;check(worker_budget()==2,"current free RAM reserves half for two helpers");
    test_memory.free_mem=0;check(worker_budget()==1,"physical pressure keeps one progress lane not zero workers");
    test_cpu.online_cpus=1;test_memory.free_mem=UINT64_C(4096)*1024*1024;
    check(worker_budget()==8,"current design explicitly retains minimum eight for one CPU");
    fail_cpu=true;fail_memory=true;check(worker_budget()==8,"native information failure has conservative CPU fallback");
    fail_cpu=fail_memory=false;
}
static void test_shrink_live_slots(void){
    webnet n={.worker_limit=16};struct request active[15];
    for(unsigned i=0;i<15;i++){active[i]=request(2,i+10,"old");n.slots[i+1]=(struct slot){.rq=&active[i],.pid=(int)i+100};}
    struct request pending=request(0,1,"new");queue(&n,&pending);
    n.worker_limit=1;check(live_workers(&n)==15,"worker shrink retains every high-numbered live process");
    check(!take_request(&n,0)&&n.queue==&pending,"empty slot zero cannot spawn while live workers exceed shrunk budget");
    active[0].cancelled=true;active[1].finishing=true;
    check(live_workers(&n)==15&&!take_request(&n,0),"cancelled or finishing process consumes reserve until reaped");
    for(unsigned i=0;i<15;i++)n.slots[i+1].pid=0;
    check(live_workers(&n)==0&&take_request(&n,0)==&pending,"reaped processes release admission while request shells remain owned");
    check(!pending.next&&!n.queue,"admission removes exactly selected queue item");
}
static void test_parked_total_and_reservation(void){
    webnet n={.worker_limit=32};struct request parked_requests[30],active[2];struct slot parked[30];
    for(unsigned i=0;i<30;i++){parked_requests[i]=request(3,i+10,"parked");parked_requests[i].paused=true;
        parked[i]=(struct slot){.rq=&parked_requests[i],.pid=(int)i+100,.next=i+1<30?&parked[i+1]:NULL};}
    n.parked=parked;
    for(unsigned i=0;i<2;i++){active[i]=request(2,i+50,"active");n.slots[i+1]=(struct slot){.rq=&active[i],.pid=(int)i+200};}
    struct request nav=request(0,1,"unrelated"),fetch=request(3,2,"unrelated");queue(&n,&fetch);queue(&n,&nav);
    check(live_workers(&n)==32&&!take_request(&n,0),"thirty parked and two active transports share one global cap");
    parked[0].pid=0;check(live_workers(&n)==31,"reaped parked process no longer consumes admission");
    check(!available_class(&n,3),"empty reserved lane refuses final Fetch while parked processes occupy all other budget");
    check(take_request(&n,0)==&nav,"navigation can use global final reserve despite parked transports");
    n.slots[0]=(struct slot){.rq=&nav,.pid=999};check(!take_request(&n,1),"occupied reserve at full physical budget cannot over-admit Fetch");
    parked[1].pid=0;check(take_request(&n,2)==&fetch,"Fetch can use spare capacity once native reserved lane is occupied");
}
static void test_mixed_single_worker_fifo(void){
    webnet n={.worker_limit=1};struct request requests[6];
    for(unsigned p=1;p<=3;p++)for(unsigned i=0;i<2;i++){
        unsigned at=(p-1)*2+i;requests[at]=request(p,at+1,"same");queue(&n,&requests[at]);}
    bool fair=true,fifo=true;unsigned seen[3]={0};
    for(unsigned i=0;i<18;i++){
        struct request *r=take_request(&n,0);unsigned p=1+i%3;
        if(!r||r->priority!=(int)p){fair=false;break;}
        unsigned first=(p-1)*2;struct request *expected=&requests[first+(seen[p-1]++%2)];
        if(r!=expected)fifo=false;
        queue(&n,r); /* continuous mixed supply, not a Fetch-only empty queue */
    }
    check(fair,"single physical lane rotates script resource and Fetch under continuous supply");
    check(fifo,"round robin preserves eligible FIFO within each resource class");
    check(seen[0]==6&&seen[1]==6&&seen[2]==6,"single lane makes bounded progress for every competing class");
    struct request nav=request(0,90,"same");queue(&n,&nav);
    check(take_request(&n,0)==&nav,"user navigation retains priority over mixed single-lane classes");
}
static void test_initial_origin_and_fifo_bypass(void){
    webnet n={.worker_limit=16};struct slot held[5];struct request workers[5];
    for(unsigned i=0;i<5;i++){workers[i]=request(3,i+10,"origin-a");held[i]=(struct slot){.rq=&workers[i],.pid=(int)i+20,.next=i+1<5?&held[i+1]:NULL};}
    n.parked=held;struct request blocked=request(3,1,"origin-a"),eligible=request(3,2,"origin-b"),dependency=request(1,3,"origin-a");
    queue(&n,&blocked);queue(&n,&eligible);
    check(!origin_available(&n,&blocked)&&origin_available(&n,&dependency),"five parked Fetches reserve sixth initial-origin connection for document dependency");
    n.next_priority=3;check(take_request(&n,1)==&eligible&&n.queue==&blocked,"blocked initial origin does not head-of-line stall another eligible origin");
    n.slots[0]=(struct slot){.rq=&dependency,.pid=30};
    check(!origin_available(&n,&dependency),"six total initial-origin transports also prevent another dependency");
    workers[0].cancelled=true;
    check(live_workers(&n)==6&&origin_available(&n,&dependency),"cancelled socket scheduling and unreaped physical resource counts are distinct");
    struct request opaque=request(2,4,NULL);
    check(origin_available(&n,&opaque),"opaque or local request bookkeeping is not URL admission policy");
}
static void test_reap_predicate(void){
    static const int returned[]={0,-1,-1,999,71,-1};
    static const int errors[]={0,EINTR,EIO,0,0,ECHILD};
    for(unsigned i=0;i<sizeof returned/sizeof *returned;i++){
        struct request r={.paused=true,.streaming=true};struct slot s={.pid=71,.in=4,.out=5,.rq=&r};
        wait_result=returned[i];wait_error=errors[i];wait_calls=0;
        bool reaped=slot_reap(&s),expected=i>=4;
        check(reaped==expected&&s.pid==(expected?0:71),"only owned PID or ECHILD proves transport retirement");
        check(wait_calls==1&&waited_pid==71&&wait_flags==WNOHANG&&s.rq==&r&&s.in==4&&s.out==5,
              "reap is nonblocking and retains owned unread response descriptors");
    }
    struct slot retired={0};wait_calls=0;
    check(slot_reap(&retired)&&!wait_calls,"already retired transport is not waited or reused");
}
static void test_paused_reap_and_admission(void){
    webnet n={.worker_limit=16};struct slot held[5];struct request responses[5];
    unsigned char unread[]={1,2,3,4};
    for(unsigned i=0;i<5;i++){
        responses[i]=request(3,i+10,"paused-origin");responses[i].paused=responses[i].streaming=true;
        responses[i].rx=unread;responses[i].rx_len=sizeof unread;responses[i].rx_pos=2;responses[i].head_pos=3;
        held[i]=(struct slot){.rq=&responses[i],.pid=(int)i+80,.in=-1,.out=(int)i+20,.next=i+1<5?&held[i+1]:NULL};
    }
    n.parked=held;struct request next=request(3,1,"paused-origin");queue(&n,&next);
    check(!origin_available(&n,&next)&&!take_request(&n,1),"five genuinely live paused transports retain origin backpressure");
    poll_result=0;poll_events=0;poll_calls=wait_calls=closed_fds=killed_pid=0;
    reap_paused_stream(&held[0]);
    check(poll_calls==1&&!poll_requested&&!wait_calls&&held[0].pid==80,"open paused pipe is only polled for hangup, not drained or waited");
    poll_result=1;poll_events=N_POLLHUP|N_POLLIN;wait_result=80;wait_error=0;
    struct request before=responses[0];reap_paused_stream(&held[0]);
    check(!held[0].pid&&held[0].out==20&&held[0].in==-1&&held[0].rq==&responses[0]&&!closed_fds&&!killed_pid,
          "closed writer releases live transport but preserves its unread dynamic pipe");
    check(!memcmp(&before,&responses[0],sizeof before)&&!memcmp(unread,(unsigned char[]){1,2,3,4},4),
          "reaping does not consume partial frame, change pause, or publish callback/END");
    check(live_workers(&n)==4&&origin_available(&n,&next)&&take_request(&n,1)==&next,
          "retired paused response no longer permanently starves same-origin Fetch admission");
    wait_calls=0;reap_paused_stream(&held[0]);
    check(!wait_calls,"retained response never waits a recycled transport PID");
    slot_stop(&held[0],NULL);
    check(responses[0].finishing&&held[0].out==-1&&!killed_pid,"later cancellation closes retained pipe without killing retired PID");
    struct request inactive={.streaming=true,.paused=false};struct slot active={.pid=88,.out=8,.rq=&inactive};
    poll_calls=0;reap_paused_stream(&active);inactive.paused=true;inactive.finishing=true;reap_paused_stream(&active);
    inactive.finishing=false;inactive.streaming=false;reap_paused_stream(&active);
    check(!poll_calls&&active.pid==88,"paused reap cannot replace active receive or finishing validation");
    inactive.streaming=true;poll_result=-1;poll_events=0;killed_pid=0;
    reap_paused_stream(&active);
    check(inactive.finishing&&strstr(inactive.error,"Worker poll failed")&&killed_pid==88,
          "invalid pipe follows existing failed-worker cancellation, never fabricated success");
}
int main(void){
    test_budget_values();test_shrink_live_slots();test_parked_total_and_reservation();test_mixed_single_worker_fifo();test_initial_origin_and_fifo_bypass();
    test_reap_predicate();test_paused_reap_and_admission();
    printf("webnet-scheduler-perf: %u checks, %u failures\n",checks,failures);return failures!=0;
}
