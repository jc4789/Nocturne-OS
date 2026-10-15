static unsigned failures,callbacks,chunks,ends;static bool resume_immediately,slow_callback;
static void check(bool ok,const char *text){printf("%s %s\n",ok?"PASS":"FAIL",text);failures+=!ok;}
static void received(webnet *n,uint64_t id,uint64_t generation,const struct webnet_response *r,void *opaque){
    (void)generation;(void)opaque;callbacks++;
    if(r->event==WEBNET_CHUNK){chunks++;for(size_t i=0;i<r->body_len;i++)if(((unsigned char *)r->body)[i]!=0xa5)abort();}
    if(r->event==WEBNET_END)ends++;
    /* The recursive pump must neither call back nor consume the next frame. */
    unsigned before=callbacks;webnet_pump(n,clock_ms);if(callbacks!=before)abort();
    if(resume_immediately)webnet_resume(n,id);
    if(slow_callback)clock_ms+=PUMP_TIME_MS;
}
static void reset(webnet *n){
    memset(n,0,sizeof *n);n->worker_limit=WORKERS;n->service_listener=-1;
    for(unsigned i=0;i<WORKERS;i++)n->slots[i].in=n->slots[i].out=-1;
    memset(channels,0,sizeof channels);clock_ms=100;callbacks=chunks=ends=0;resume_immediately=true;slow_callback=false;
    closed_count=killed_count=wait_count=0;
}
static struct request *request_new(webnet *n,unsigned slot,int fd,uint64_t id){
    struct request *r=calloc(1,sizeof *r);r->id=id;r->generation=9;r->deadline=100000;r->response_limit=WEBNET_BODY_LIMIT;
    r->streaming=r->headers_seen=true;r->callback=received;r->tx_credit=WEBNET_SERVICE_WINDOW;
    n->slots[slot]=(struct slot){.rq=r,.pid=77,.in=fd,.out=fd,.shared=true};n->count++;return r;
}
static void append_frame(int fd,uint64_t id,uint32_t magic,size_t bytes){
    struct fixture *f=&channels[fd];struct webnet_wire_response h={0};h.magic=magic;h.id=id;h.generation=9;h.body_len=(uint32_t)bytes;
    memcpy(f->bytes+f->length,&h,sizeof h);f->length+=sizeof h;
    memset(f->bytes+f->length,0xa5,bytes);f->length+=bytes;
}
static void cleanup(webnet *n){
    for(unsigned i=0;i<WORKERS;i++)if(n->slots[i].rq)request_free(n->slots[i].rq);
    while(n->parked){struct slot *s=n->parked;n->parked=s->next;request_free(s->rq);free(s);}
}
int main(void){
    webnet n;reset(&n);request_new(&n,0,1,1);
    for(unsigned i=0;i<8;i++)append_frame(1,1,WEBNET_STREAM_CHUNK,4096);
    webnet_pump(&n,clock_ms);
    check(chunks>1,"an immediately resumed stream drains multiple 4KiB frames in one fair pump");
    check(channels[1].position<=PUMP_BYTES,"one request cannot consume another request's byte quantum");cleanup(&n);

    reset(&n);struct request *r=request_new(&n,0,1,1);resume_immediately=false;
    append_frame(1,1,WEBNET_STREAM_CHUNK,4096);append_frame(1,1,WEBNET_STREAM_CHUNK,4096);
    webnet_pump(&n,clock_ms);size_t paused_at=channels[1].position;
    webnet_pump(&n,clock_ms);webnet_service(&n,clock_ms);
    check(chunks==1&&paused_at==channels[1].position&&r->paused,"pause stops reads and author callbacks until explicit resume");
    r->credit_due=WEBNET_SERVICE_CREDIT_STEP;unsigned credits=channels[1].credits;
    webnet_pump(&n,clock_ms);check(channels[1].credits==credits,"paused streams do not replenish sender memory credit");
    webnet_resume(&n,1);webnet_pump(&n,clock_ms);
    check(chunks==2&&channels[1].credits==credits+1,"resumed streams replenish consumed byte credit and preserve frame order");cleanup(&n);

    reset(&n);r=request_new(&n,0,1,1);append_frame(1,1,WEBNET_STREAM_CHUNK,4096);
    webnet_service(&n,clock_ms);
    check(callbacks==0&&r->frame_ready,"native transport checkpoints defer callbacks while author JS is running");
    size_t deferred=channels[1].position;webnet_service(&n,clock_ms);
    check(callbacks==0&&channels[1].position==deferred,"a deferred frame bounds receive memory without a second callback or read");
    webnet_pump(&n,clock_ms);check(chunks==1&&!r->frame_ready,"the next safe author pump dispatches the deferred frame exactly once");cleanup(&n);

    reset(&n);request_new(&n,0,1,1);request_new(&n,1,2,2);slow_callback=true;
    append_frame(1,1,WEBNET_STREAM_CHUNK,4096);append_frame(2,2,WEBNET_STREAM_END,0);
    webnet_pump(&n,clock_ms);webnet_pump(&n,clock_ms);
    check(chunks==1&&ends==1,"a costly callback cannot starve another request's terminal frame");
    check(killed_count==0&&wait_count==0,"request completion never kills or reaps the shared service");cleanup(&n);

    reset(&n);r=request_new(&n,0,1,1);append_frame(1,1,WEBNET_STREAM_CHUNK,4096);
    int ready=webnet_wait(&n,50,10000);
    check(ready>0&&watched_gui==1&&watched_net==1&&last_timeout==10000,"GUI and response channel readiness share one wait instead of a 10ms polling tick");
    r->paused=true;ready=webnet_wait(&n,50,10000);
    check(watched_gui==1&&watched_net==1&&net_events==0&&!ready,"paused body readiness cannot cause a busy GUI wait loop");
    channels[1].hup=true;ready=webnet_wait(&n,50,10000);
    check(ready>0,"retirement of a paused response wakes queued request admission without reading body data");cleanup(&n);

    reset(&n);r=request_new(&n,0,1,1);channels[1].hup=true;
    for(unsigned i=0;i<24;i++)append_frame(1,1,WEBNET_STREAM_CHUNK,4096);
    append_frame(1,1,WEBNET_STREAM_END,0);
    for(unsigned i=0;i<20&&webnet_busy(&n);i++)webnet_pump(&n,clock_ms);
    check(!webnet_busy(&n)&&chunks==24&&ends==1,"a retired writer's credit EPIPE preserves its complete buffered response until real END");cleanup(&n);

    reset(&n);r=request_new(&n,0,1,1);r->streaming=false;r->headers_seen=false;r->tx=malloc(WEBNET_SERVICE_WINDOW*2);
    r->tx_len=WEBNET_SERVICE_WINDOW*2;n.queued_bytes=r->tx_len;
    for(unsigned i=0;i<20;i++)webnet_pump(&n,clock_ms);
    check(r->tx_pos==WEBNET_SERVICE_WINDOW&&!r->tx_credit,"upload cannot queue more than its initial memory window without worker consumption");
    struct webnet_wire_response ack={.magic=WEBNET_STREAM_REQUEST_CREDIT,.status=WEBNET_SERVICE_CREDIT_STEP,.id=1,.generation=9};
    memcpy(channels[1].bytes,&ack,sizeof ack);channels[1].length=sizeof ack;
    webnet_pump(&n,clock_ms);webnet_pump(&n,clock_ms);
    check(r->tx_pos>WEBNET_SERVICE_WINDOW&&r->tx_pos<=WEBNET_SERVICE_WINDOW+WEBNET_SERVICE_CREDIT_STEP&&!callbacks,
          "typed upload consumption credit permits bounded progress without an author response callback");cleanup(&n);
    printf("webnet pacing: %u failed\n",failures);return failures!=0;
}
