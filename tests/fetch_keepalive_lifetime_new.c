/* A native completion/worker boundary fixture, not real-site acceptance. */
static unsigned checks,failures,kills,reaps,callbacks;
#define CHECK(name,expr) do {checks++;if(!(expr)){failures++;printf("FAIL %s\n",name);}}while(0)
uint64_t uptime_ms(void){return 100;}
int close(int fd){(void)fd;return 0;}
int kill(int pid){(void)pid;kills++;return 0;}
int waitpid(int pid,int*status,int flags){(void)status;(void)flags;reaps++;return pid;}
long webcookie_export(webcookie_jar*j,void*out,size_t n,int64_t now){(void)j;(void)out;(void)n;(void)now;return 0;}
int webcookie_set(webcookie_jar*j,const struct webcookie_context*c,const char*v,size_t n,int64_t now){(void)j;(void)c;(void)v;(void)n;(void)now;return 1;}
bool url_parse(const char*text,struct url*u){(void)text;(void)u;return false;}
static void done(webnet*n,uint64_t id,uint64_t gen,const struct webnet_response*r,void*arg){(void)n;(void)id;(void)gen;(void)r;(void)arg;callbacks++;}
static struct request *take(webnet*n,uint64_t id){struct request**p=&n->queue;while(*p&&(*p)->id!=id)p=&(*p)->next;struct request*r=*p;if(r){*p=r->next;r->next=NULL;}return r;}
static void finish(webnet*n,uint64_t id){struct request*r=take(n,id);if(r){struct slot s={r,0,-1,-1};complete(n,&s);}}
static struct transfer *transfer(webnet*n,uint64_t gen,uint64_t group,bool alive,uint64_t resource){
    struct transfer*t=calloc(1,sizeof*t);t->generation=gen;t->resource_id=resource;t->kind=WEB_RESOURCE_FETCH;t->keepalive=alive;
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=gen,.url="https://target.test/collect",.origin="https://source.test/page",.credentials=WEBNET_CREDENTIALS_INCLUDE,.keepalive=alive,.fetch_group=group};
    t->network_id=webnet_submit(n,&q,resource_completed,t);if(!t->network_id){free(t);return NULL;}t->next=transfers;transfers=t;return t;
}
int main(void){
    struct webnet net={0};network=&net;for(int i=0;i<WORKERS;i++)net.slots[i].in=net.slots[i].out=-1;
    static unsigned char bytes[65537];memset(bytes,0xa5,sizeof bytes);
    struct webnet_request q={.kind=WEBNET_FETCH,.generation=1,.url="https://target.test/collect",.origin="https://source.test/page",.method="POST",.body=bytes,.body_len=32768,.credentials=WEBNET_CREDENTIALS_INCLUDE,.keepalive=true,.fetch_group=10};
    uint64_t a=webnet_submit(&net,&q,done,NULL),b=webnet_submit(&net,&q,done,NULL);
    CHECK("two inflight bodies exactly 64KiB accepted",a&&b&&net.count==2);
    q.body_len=1;CHECK("same group 64KiB plus one rejected",!webnet_submit(&net,&q,done,NULL));
    q.fetch_group=11;uint64_t c=webnet_submit(&net,&q,done,NULL);CHECK("different environment group independent",c!=0);
    q.fetch_group=12;q.body_len=65537;CHECK("single keepalive body plus one rejected",!webnet_submit(&net,&q,done,NULL));
    q.keepalive=false;uint64_t ordinary=webnet_submit(&net,&q,done,NULL);CHECK("nonkeepalive body outside keepalive spec bound accepted",ordinary!=0);
    q.keepalive=true;q.body_len=0;q.fetch_group=10;uint64_t empty=webnet_submit(&net,&q,done,NULL);CHECK("empty keepalive accepted with full group body budget",empty!=0);
    struct request*r=take(&net,a);net.slots[1]=(struct slot){r,7,-1,-1};
    q.body_len=1;CHECK("active plus queued share group budget",!webnet_submit(&net,&q,done,NULL));
    webnet_cancel_generation(&net,1);CHECK("retirement retains active and queued keepalive",net.slots[1].rq==r&&net.count==4&&kills==0);
    CHECK("ordinary generation request removed",take(&net,ordinary)==NULL);
    struct webnet_wire_request h;memcpy(&h,r->tx,sizeof h);
    CHECK("original origin snapshot survives retirement",h.origin_len==strlen(q.origin)&&!memcmp(r->tx+sizeof h+h.url_len,q.origin,h.origin_len));
    CHECK("original credentials survive retirement",h.credentials==WEBNET_CREDENTIALS_INCLUDE);
    CHECK("original buffered body survives retirement",h.body_len==32768&&r->tx[sizeof h+h.url_len+h.origin_len+h.method_len+h.headers_len]==0xa5);
    webnet_cancel(&net,a);CHECK("explicit abort kills active keepalive",r->cancelled&&kills>=1);
    q.body_len=1;uint64_t d=webnet_submit(&net,&q,done,NULL);CHECK("explicit abort releases inflight body budget",d!=0);
    complete(&net,&net.slots[1]);CHECK("aborted keepalive no callback",callbacks==0);
    finish(&net,b);q.body_len=65535;uint64_t e=webnet_submit(&net,&q,done,NULL);CHECK("completion releases group budget",e!=0);
    finish(&net,c);finish(&net,empty);finish(&net,d);finish(&net,e);CHECK("native requests fully drained",net.count==0&&net.queued_bytes==0);
    doc=(web_doc*)(uintptr_t)1;generation=20;
    struct transfer*t=transfer(&net,20,20,true,100);uint64_t id=t?t->network_id:0;
    host_release_request(NULL,100);CHECK("child retirement detaches transport delivery",t&&t->detached&&net.count==1);
    finish(&net,id);CHECK("same top generation detached child never delivered",delivered==0&&transfers==NULL);
    t=transfer(&net,20,21,true,101);id=t?t->network_id:0;
    struct transfer*u=transfer(&net,20,0,false,102);CHECK("host keepalive and ordinary prepared",t&&u);
    cancel_document_requests();CHECK("navigation keeps only detached keepalive",transfers==t&&t->detached&&net.count==1);
    generation=21;cancel_document_requests();CHECK("next navigation preserves older detached transport",transfers==t&&net.count==1);
    finish(&net,id);CHECK("old generation completion discarded and host freed",delivered==0&&transfers==NULL&&net.count==0);
    t=transfer(&net,21,22,true,103);id=t?t->network_id:0;host_cancel(NULL,103);CHECK("live explicit AbortSignal cancels native keepalive",transfers==NULL&&net.count==0);
    t=transfer(&net,21,23,true,104);cancel_document_requests();generation=22;quit=true;cancel_document_requests();CHECK("UA shutdown cancels all detached keepalive",transfers==NULL&&net.count==0);
    printf("keepalive new native boundaries: %u checks / %u failed\n",checks,failures);return failures?1:0;
}
