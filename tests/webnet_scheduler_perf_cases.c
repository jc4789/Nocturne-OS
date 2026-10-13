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
int main(void){
    test_budget_values();test_shrink_live_slots();test_parked_total_and_reservation();test_mixed_single_worker_fifo();test_initial_origin_and_fifo_bypass();
    printf("webnet-scheduler-perf: %u checks, %u failures\n",checks,failures);return failures!=0;
}
