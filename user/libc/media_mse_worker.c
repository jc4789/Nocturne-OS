/* Nonblocking GUI-side transport for a dynamic native MSE child registry. */
#include "media_mse_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct request {struct request *next;struct nmsw_command c;uint8_t *bytes;uint64_t epoch;};
struct slot_state {
    uint64_t revision,epoch;uint32_t parsing;int64_t group_end;
    struct nmedia_info info;bool metadata,used;
    size_t count,capacity;struct nmedia_time_range *ranges;
};
struct nmedia_mse_worker {
    struct nmedia_mse_worker *next;
    int pid,in,out,video_fd;uint32_t generation,sequence,completed_sequence;
    bool reserved,stopping,output_ready,quota_error,starving,video_taken;
    struct request *head,*tail;
    size_t queued_bytes,queued_count,tx_pos,head_pos,meta_pos,meta_capacity,rx_pos,capacity;
    uint8_t *payload;int64_t current;
    struct nmsw_response response;
    struct nmedia_output output;unsigned output_slot;uint64_t output_revision;
    struct nmsw_video video_header;
    uint8_t *video_pixels;size_t video_head_pos,video_rx_pos,video_capacity;
    bool video_ready,video_loan;uint32_t video_sequence,video_expected_sequence;
    unsigned taken_slot;uint64_t taken_revision;bool taken_video_lane;
    uint8_t *meta;struct slot_state *slots;size_t slot_count,slot_capacity;uint64_t epoch;
    char error[160];
};
static nmedia_mse_worker *live,*retired;
static void destroy(nmedia_mse_worker *w){for(size_t i=0;i<w->slot_count;i++)nmedia_ff_free(w->slots[i].ranges);nmedia_ff_free(w->slots);nmedia_ff_free(w->meta);nmedia_ff_free(w->payload);nmedia_ff_free(w->video_pixels);nmedia_ff_free(w);}
static bool reserve_slots(nmedia_mse_worker *w,size_t count){
    if(count>w->slot_capacity){size_t capacity=w->slot_capacity?w->slot_capacity:4;
        while(capacity<count){if(capacity>SIZE_MAX/sizeof *w->slots/2){capacity=count;break;}capacity*=2;}
        if(capacity>SIZE_MAX/sizeof *w->slots)return false;
        struct slot_state *slots=nmedia_ff_realloc(w->slots,capacity*sizeof *slots);if(!slots)return false;
        memset(slots+w->slot_capacity,0,(capacity-w->slot_capacity)*sizeof *slots);w->slots=slots;w->slot_capacity=capacity;
    }if(count>w->slot_count)w->slot_count=count;return true;
}
static void clear_queue(nmedia_mse_worker *w){while(w->head){struct request *r=w->head;w->head=r->next;nmedia_ff_free(r->bytes);nmedia_ff_free(r);}w->tail=NULL;w->queued_bytes=w->queued_count=0;}
static void stop(nmedia_mse_worker *w,const char *error){
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;w->stopping=true;w->output_ready=w->video_taken=w->video_ready=w->video_loan=false;
    if(w->in>=0)close(w->in);if(w->out>=0)close(w->out);if(w->video_fd>=0)close(w->video_fd);w->in=w->out=w->video_fd=-1;if(w->pid>0)kill(w->pid);clear_queue(w);
}
static bool reap(nmedia_mse_worker *w){
    if(w->pid>0){int got=waitpid(w->pid,NULL,WNOHANG);if(got!=w->pid&&!(got<0&&errno==ECHILD))return false;w->pid=0;}
    if(w->reserved){nmedia_alloc_release_worker();w->reserved=false;}return true;
}
void nmedia_mse_worker_background(uint64_t now){
    (void)now;nmedia_mse_worker **p=&retired;while(*p){nmedia_mse_worker *w=*p;if(reap(w)){*p=w->next;destroy(w);}else p=&w->next;}
    for(nmedia_mse_worker *w=live;w;w=w->next)if(w->stopping)reap(w);
}
static bool spawn_child(nmedia_mse_worker *w){
    if(!nmedia_alloc_reserve_worker()){stop(w,"MSE worker count overflow");return false;}w->reserved=true;
    int ip[2]={-1,-1},op[2]={-1,-1},vp[2]={-1,-1};
    if(pipe(ip)<0||pipe(op)<0||pipe(vp)<0)goto fail;
    int map[3]={ip[0],op[1],vp[1]};char *argv[]={"browsermseworker",NULL};w->pid=spawn("/bin/browsermseworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(vp[1]);vp[1]=-1;if(w->pid<0){w->pid=0;goto fail;}
    w->in=ip[1];ip[1]=-1;w->out=op[0];op[0]=-1;w->video_fd=vp[0];vp[0]=-1;
    if(fcntl(w->in,F_SETFL,O_NONBLOCK)<0||fcntl(w->out,F_SETFL,O_NONBLOCK)<0||fcntl(w->video_fd,F_SETFL,O_NONBLOCK)<0){stop(w,"MSE worker nonblocking pipe setup failed");return false;}return true;
fail:
    for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);if(vp[i]>=0)close(vp[i]);}stop(w,"cannot spawn native MSE worker");reap(w);return false;
}
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t generation,char *error,size_t size){
    nmedia_mse_worker_background(uptime_ms());
    nmedia_mse_worker *w=nmedia_ff_mallocz(sizeof *w);if(!w){if(error&&size)strlcpy(error,"MSE worker allocation failed",size);return NULL;}
    w->in=w->out=w->video_fd=-1;w->generation=generation;w->next=live;live=w;if(error&&size)*error=0;return w;
}
bool nmedia_mse_worker_command(nmedia_mse_worker *w,unsigned op,unsigned slot,const void *bytes,size_t size,int64_t offset,int64_t start,int64_t end,bool flag){
    if(!w||w->stopping||slot>INT32_MAX||op<NMSW_ADD||op>NMSW_SELECT||size>UINT32_MAX||(!bytes&&size)||w->queued_count==SIZE_MAX||size>SIZE_MAX-w->queued_bytes)return false;
    bool local=op==NMSW_ADD||op==NMSW_APPEND||op==NMSW_REMOVE||op==NMSW_ABORT||op==NMSW_DROP||op==NMSW_CHANGE;
    if(op==NMSW_SELECT&&slot>1)return false; /* audio/video kind, not a buffer count */
    if(local&&op!=NMSW_ADD&&(slot>=w->slot_count||!w->slots[slot].used))return false;
    if((op==NMSW_ADD||op==NMSW_DROP)&&w->epoch==UINT64_MAX)return false;
    if(op==NMSW_ADD&&!reserve_slots(w,(size_t)slot+1))return false;
    if(op==NMSW_ADD&&w->slots[slot].used)return false;
    struct request *r=nmedia_ff_mallocz(sizeof *r);if(!r)return false;
    if(size){r->bytes=nmedia_ff_malloc(size);if(!r->bytes){nmedia_ff_free(r);return false;}memcpy(r->bytes,bytes,size);}
    if(op==NMSW_ADD||op==NMSW_DROP){struct slot_state *s=&w->slots[slot];s->epoch=++w->epoch;s->used=op==NMSW_ADD;s->metadata=false;s->count=0;s->parsing=0;s->group_end=0;s->revision=0;}
    w->video_taken=false;r->epoch=w->epoch;
    r->c=(struct nmsw_command){.magic=NMSW_MAGIC,.op=op,.generation=w->generation,.slot=slot,.flags=flag,.bytes=(uint32_t)size,.offset=offset,.start=start,.end=end,.current=w->current,.epoch=r->epoch};
    if(w->tail)w->tail->next=r;else w->head=r;w->tail=r;w->queued_bytes+=size;w->queued_count++;
    if(op!=NMSW_STEP){if(op==NMSW_ABORT){w->error[0]=0;w->quota_error=false;}}
    if(w->head==r){r->c.sequence=++w->sequence;w->tx_pos=w->head_pos=w->meta_pos=w->rx_pos=0;memset(&w->response,0,sizeof w->response);}return true;
}
static bool valid(nmedia_mse_worker *w){
    const struct nmsw_response *r=&w->response;const struct nmsw_command *c=&w->head->c;
    if(r->magic!=NMSW_MAGIC||r->op!=c->op||r->generation!=w->generation||r->sequence!=c->sequence||r->reserved||r->slot<0||r->quota_error>1||!memchr(r->error,0,sizeof r->error)||
       r->metadata_count>w->slot_count||r->metadata_count>r->metadata_bytes/sizeof(struct nmsw_metadata))return false;
    if((r->kind==NMEDIA_AUDIO||r->kind==NMEDIA_VIDEO||r->kind==NMSW_CLOCK)&&(size_t)r->slot>=w->slot_count)return false;
    if(r->kind==NMEDIA_AUDIO)return r->op==NMSW_STEP&&r->frames>0&&r->frames<=4096&&r->payload_bytes==r->frames*4;
    if(r->kind==NMEDIA_VIDEO){uint32_t bytes;return r->op==NMSW_STEP&&nmedia_video_wire_bytes(r->width,r->height,&bytes)&&r->payload_bytes==bytes;}
    return (r->kind==NMEDIA_AGAIN||r->kind==NMEDIA_END||r->kind==NMEDIA_ERROR||r->kind==NMSW_BUSY||r->kind==NMSW_CLOCK||
        (r->kind==NMSW_VIDEO_PENDING&&r->op==NMSW_STEP))&&!r->payload_bytes;
}
static bool metadata_valid(nmedia_mse_worker *w){
    size_t at=0;uint32_t previous=0;
    for(uint32_t i=0;i<w->response.metadata_count;i++){
        if(sizeof(struct nmsw_metadata)>w->response.metadata_bytes-at)return false;
        const struct nmsw_metadata *m=(const void *)(w->meta+at);at+=sizeof *m;const struct nmsw_info *n=&m->info;
        if(m->slot>=w->slot_count||(i&&m->slot<=previous)||m->reserved||m->parsing>1||m->group_end<0||m->group_end>1000001000000LL||
           n->duration< -1||n->duration>1000000000000LL||n->audio>1||n->video>1||n->channels>8||n->sample_rate>384000||!nmedia_video_dimensions(n->width,n->height)||
           !memchr(n->audio_codec,0,32)||!memchr(n->video_codec,0,32)||!memchr(n->container,0,32)||m->count>(w->response.metadata_bytes-at)/sizeof(struct nmedia_time_range))return false;
        const struct nmedia_time_range *ranges=(const void *)(w->meta+at);
        for(uint32_t j=0;j<m->count;j++)if(ranges[j].start_ms<0||ranges[j].end_ms<=ranges[j].start_ms||ranges[j].end_ms>1000001000000LL||(j&&ranges[j-1].end_ms>ranges[j].start_ms))return false;
        at+=(size_t)m->count*sizeof *ranges;previous=m->slot;
    }return at==w->response.metadata_bytes;
}
static void completed(nmedia_mse_worker *w){
    struct nmsw_response *r=&w->response;
    w->completed_sequence=r->sequence;
    if(r->kind==NMSW_VIDEO_PENDING)w->video_expected_sequence=r->sequence;
    size_t at=0;
    for(uint32_t i=0;i<r->metadata_count;i++){const struct nmsw_metadata *m=(const void *)(w->meta+at);at+=sizeof *m;const struct nmedia_time_range *ranges=(const void *)(w->meta+at);at+=(size_t)m->count*sizeof *ranges;
        struct slot_state *s=&w->slots[m->slot];if(w->head->epoch<s->epoch)continue;const struct nmsw_info *a=&m->info;
        if(m->count>s->capacity){struct nmedia_time_range *v=nmedia_ff_realloc(s->ranges,(size_t)m->count*sizeof *v);if(!v){stop(w,"MSE range allocation failed");return;}s->ranges=v;s->capacity=m->count;}
        s->metadata=a->audio||a->video;s->info=(struct nmedia_info){.audio=a->audio,.video=a->video,.channels=(int)a->channels,.sample_rate=(int)a->sample_rate,.width=a->width,.height=a->height,.duration_ms=a->duration};
        strlcpy(s->info.audio_codec,a->audio_codec,32);strlcpy(s->info.video_codec,a->video_codec,32);strlcpy(s->info.container,a->container,32);
        s->revision=m->revision;s->parsing=m->parsing;s->group_end=m->group_end;s->count=m->count;if(s->count)memcpy(s->ranges,ranges,s->count*sizeof *ranges);}
    if(r->kind==NMEDIA_ERROR){strlcpy(w->error,r->error[0]?r->error:"native MSE worker failed",sizeof w->error);w->quota_error=r->quota_error;}
    else if(r->op==NMSW_ABORT||w->quota_error){w->error[0]=0;w->quota_error=false;if(w->output.kind==NMEDIA_ERROR)w->output_ready=false;}
    if(w->output_ready&&nmedia_mse_worker_revision(w,w->output_slot)!=w->output_revision)w->output_ready=false;
    bool data=r->kind==NMEDIA_AUDIO||r->kind==NMEDIA_VIDEO||r->kind==NMSW_CLOCK;
    bool current_epoch=data?w->head->epoch>=w->slots[r->slot].epoch:w->head->epoch==w->epoch;
    if(r->op==NMSW_STEP&&current_epoch){w->output_ready=true;w->output_slot=(unsigned)r->slot;w->output_revision=nmedia_mse_worker_revision(w,w->output_slot);
        w->starving=r->kind==NMEDIA_AGAIN;
        w->output=(struct nmedia_output){.kind=r->kind==NMSW_VIDEO_PENDING?NMSW_BUSY:r->kind,.pts_ms=r->pts,.frames=r->frames,.width=r->width,.height=r->height,.samples=(const int16_t *)w->payload,.pixels=(const uint32_t *)w->payload};}
    else if(r->kind!=NMEDIA_ERROR)w->starving=false;
    struct request *old=w->head;w->head=old->next;w->queued_bytes-=old->c.bytes;w->queued_count--;nmedia_ff_free(old->bytes);nmedia_ff_free(old);if(!w->head)w->tail=NULL;
    w->tx_pos=w->head_pos=w->meta_pos=w->rx_pos=0;
    if(w->head){w->head->c.sequence=++w->sequence;memset(&w->response,0,sizeof w->response);}
}
static void control_io(nmedia_mse_worker *w,uint64_t now,uint64_t until,bool dispatch){
    nmedia_mse_worker_background(now);if(!w||w->stopping||!w->head)return;
    if(!w->pid){if(!dispatch)return;if(!spawn_child(w))return;}
    /* Queued APPEND/REMOVE can wait behind transfer or author JS work. Their
       decoder resume point must use the real sink clock at dispatch, not the
       old enqueue clock. Never mutate a partially transmitted wire header. */
    if(dispatch&&!w->tx_pos)w->head->c.current=w->current;
    if(!dispatch&&w->tx_pos<sizeof w->head->c+w->head->c.bytes)return;
    struct n_pollfd p[2]={{w->in,dispatch?N_POLLOUT:0,0},{w->out,N_POLLIN,0}};if(poll(p,2,0)<0){stop(w,"MSE worker poll failed");return;}
    size_t budget=65536,total=sizeof w->head->c+w->head->c.bytes;
    /* Append bytes and decoded pixels both cross a 16 KiB pipe. Cooperatively
     * wake the blocked peer after transfer progress; never wait for decode or
     * a network request. Both directions share one bounded handoff interval. */
    uint64_t transfer_end=MIN(uptime_ms()+2,until);unsigned handoffs=0;
    while(dispatch&&w->tx_pos<total&&budget&&(p[0].revents&N_POLLOUT)){
        bool header=w->tx_pos<sizeof w->head->c;const uint8_t *src=header?(const uint8_t *)&w->head->c+w->tx_pos:w->head->bytes+w->tx_pos-sizeof w->head->c;
        size_t take=MIN(budget,header?sizeof w->head->c-w->tx_pos:total-w->tx_pos);ssize_t n=write(w->in,src,take);
        if(n<0&&errno==EAGAIN&&w->tx_pos>sizeof w->head->c&&budget&&handoffs<16&&uptime_ms()<transfer_end){
            handoffs++;yield();if(uptime_ms()>=transfer_end)break;continue;
        }
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){stop(w,"MSE worker command pipe failed");return;}w->tx_pos+=(size_t)n;budget-=(size_t)n;
    }
    /* The first poll predates command dispatch. A fast peer may already have
     * replied by now; do not schedule a whole browser pass on stale revents. */
    if(dispatch&&w->tx_pos==total&&!(p[1].revents&(N_POLLIN|N_POLLHUP))){
        if(poll(&p[1],1,0)<0){stop(w,"MSE worker response poll failed");return;}
    }
    bool received=false;
    while(w->head&&(p[1].revents&(N_POLLIN|N_POLLHUP))){
        /* Drain actual available bytes under the caller's elapsed deadline,
           not a 256KiB-per-pump frame quota. Expired collection permits one
           nonblocking component only; it never renews the native burst. */
        if(received&&uptime_ms()>=transfer_end)break;
        received=true;
        bool header=w->head_pos<sizeof w->response,metadata=!header&&w->meta_pos<w->response.metadata_bytes;
        size_t take=header?sizeof w->response-w->head_pos:metadata?w->response.metadata_bytes-w->meta_pos:w->response.payload_bytes-w->rx_pos;
        if(!take){if(!metadata_valid(w)){stop(w,"invalid MSE metadata wire");return;}completed(w);break;}void *dest=header?(uint8_t *)&w->response+w->head_pos:metadata?w->meta+w->meta_pos:w->payload+w->rx_pos;ssize_t n=read(w->out,dest,take);
        if(n<0&&errno==EAGAIN&&(w->head_pos||w->meta_pos||w->rx_pos)&&handoffs<16&&uptime_ms()<transfer_end){
            handoffs++;yield();if(uptime_ms()>=transfer_end)break;continue;
        }
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){stop(w,"MSE worker response ended early");return;}
        if(header){w->head_pos+=(size_t)n;if(w->head_pos==sizeof w->response){if(!valid(w)){stop(w,"invalid native MSE worker response");return;}
            if(w->response.metadata_bytes>w->meta_capacity){uint8_t *a=nmedia_ff_realloc(w->meta,w->response.metadata_bytes);if(!a){stop(w,"MSE metadata allocation failed");return;}w->meta=a;w->meta_capacity=w->response.metadata_bytes;}
            if(w->response.payload_bytes>w->capacity){
                /* Dispatch released the prior borrowed span. A response is a
                   replacement, not an append: never copy stale frame bytes. */
                nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
                uint8_t *a=nmedia_ff_malloc(w->response.payload_bytes);if(!a){stop(w,"MSE worker frame allocation failed");return;}w->payload=a;w->capacity=w->response.payload_bytes;}}}
        else if(metadata)w->meta_pos+=(size_t)n;else w->rx_pos+=(size_t)n;
        if(w->head_pos==sizeof w->response&&w->meta_pos==w->response.metadata_bytes&&w->rx_pos==w->response.payload_bytes){if(!metadata_valid(w)){stop(w,"invalid MSE metadata wire");return;}completed(w);break;}
    }
}
static void video_release(nmedia_mse_worker *w){
    w->video_ready=w->video_loan=false;w->video_head_pos=w->video_rx_pos=0;
}
static bool video_current(const nmedia_mse_worker *w){
    const struct nmsw_video *v=&w->video_header;
    return v->slot<w->slot_count&&w->slots[v->slot].used&&
        v->epoch>=w->slots[v->slot].epoch&&v->revision==w->slots[v->slot].revision;
}
static void video_io(nmedia_mse_worker *w,uint64_t until){
    if(w->video_fd<0||w->video_ready||w->video_loan)return;
    struct n_pollfd p={w->video_fd,N_POLLIN,0};
    if(poll(&p,1,0)<0){stop(w,"MSE VIDEO poll failed");return;}
    bool received=false;unsigned handoffs=0;
    while(p.revents&(N_POLLIN|N_POLLHUP)){
        if(received&&uptime_ms()>=until)break;received=true;
        bool header=w->video_head_pos<sizeof w->video_header;
        size_t take=header?sizeof w->video_header-w->video_head_pos:w->video_header.bytes-w->video_rx_pos;
        void *dest=header?(uint8_t *)&w->video_header+w->video_head_pos:w->video_pixels+w->video_rx_pos;
        ssize_t n=read(w->video_fd,dest,take);
        if(n<0&&errno==EAGAIN&&(w->video_head_pos||w->video_rx_pos)&&handoffs<16&&uptime_ms()<until){handoffs++;yield();continue;}
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){stop(w,"MSE VIDEO pipe ended early");return;}
        if(header){w->video_head_pos+=(size_t)n;
            if(w->video_head_pos==sizeof w->video_header){
                const struct nmsw_video *v=&w->video_header;uint32_t bytes;
                if(v->magic!=NMSW_MAGIC||v->generation!=w->generation||!v->sequence||v->sequence<=w->video_sequence||v->sequence>w->sequence||
                   v->slot>=w->slot_count||v->reserved||!nmedia_video_wire_bytes(v->width,v->height,&bytes)||bytes!=v->bytes){stop(w,"invalid MSE VIDEO wire");return;}
                if(bytes>w->video_capacity){nmedia_ff_free(w->video_pixels);w->video_pixels=NULL;w->video_capacity=0;
                    w->video_pixels=nmedia_ff_malloc(bytes);if(!w->video_pixels){stop(w,"MSE VIDEO receive allocation failed");return;}w->video_capacity=bytes;}
            }
        }else w->video_rx_pos+=(size_t)n;
        if(w->video_head_pos==sizeof w->video_header&&w->video_rx_pos==w->video_header.bytes){
            w->video_sequence=w->video_header.sequence;w->video_ready=true;break;
        }
    }
}
static void pump_io(nmedia_mse_worker *w,uint64_t now,uint64_t until,bool dispatch){
    if(!w||w->stopping)return;
    until=MIN(until,uptime_ms()+2);
    /* PCM/control first, but each ready lane gets its initial nonblocking
       read. Sustained APPEND/control progress must not suppress already
       delivered VIDEO. Further reads/yields obey the same original end time. */
    control_io(w,now,until,dispatch);if(w->stopping)return;
    video_io(w,until);
}
void nmedia_mse_worker_pump_budget(nmedia_mse_worker *w,uint64_t now,uint64_t until){pump_io(w,now,until,true);}
void nmedia_mse_worker_collect(nmedia_mse_worker *w){uint64_t now=uptime_ms();pump_io(w,now,now,false);}
void nmedia_mse_worker_pump(nmedia_mse_worker *w,uint64_t now){nmedia_mse_worker_pump_budget(w,now,uptime_ms()+2);}
bool nmedia_mse_worker_pending(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op!=NMSW_STEP&&r->c.op!=NMSW_SEEK)return true;return false;}
bool nmedia_mse_worker_running(const nmedia_mse_worker *w){return w&&w->pid>0&&!w->stopping;}
bool nmedia_mse_worker_quota_error(const nmedia_mse_worker *w){return w&&w->quota_error;}
const char *nmedia_mse_worker_error(const nmedia_mse_worker *w){return w?w->error:"no MSE worker";}
const struct nmedia_info *nmedia_mse_worker_info(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count&&w->slots[slot].metadata?&w->slots[slot].info:NULL;}
size_t nmedia_mse_worker_quota(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count&&w->slots[slot].used?SIZE_MAX:0;}
const struct nmedia_time_range *nmedia_mse_worker_ranges_view(const nmedia_mse_worker *w,unsigned slot,size_t *count){*count=w&&slot<w->slot_count?w->slots[slot].count:0;return *count?w->slots[slot].ranges:NULL;}
size_t nmedia_mse_worker_ranges(const nmedia_mse_worker *w,unsigned slot,struct nmedia_time_range *out,size_t maximum){size_t count;const struct nmedia_time_range *v=nmedia_mse_worker_ranges_view(w,slot,&count);size_t n=MIN(count,maximum);if(n&&out)memcpy(out,v,n*sizeof *out);return n;}
int nmedia_mse_worker_step(nmedia_mse_worker *w,struct nmedia_output *out){
    if(!w||w->error[0])return NMEDIA_ERROR;
    /* Calling STEP releases the previously returned VIDEO loan. Receiving
       PCM/control never releases it: a paused/future image may still be used. */
    if(w->video_loan)video_release(w);
    if(w->video_ready&&w->completed_sequence>=w->video_header.sequence&&!video_current(w))video_release(w);
    bool video=w->video_ready&&w->completed_sequence>=w->video_header.sequence;
    bool data=w->output_ready&&(w->output.kind==NMEDIA_AUDIO||w->output.kind==NMEDIA_VIDEO||w->output.kind==NMSW_CLOCK);
    if(video&&(!data||w->video_header.pts<=w->output.pts_ms)){
        const struct nmsw_video *v=&w->video_header;w->video_ready=false;w->video_loan=true;w->taken_video_lane=true;
        w->taken_slot=v->slot;w->taken_revision=nmedia_mse_worker_revision(w,v->slot);
        *out=(struct nmedia_output){.kind=NMEDIA_VIDEO,.pts_ms=v->pts,.width=v->width,.height=v->height,.pixels=(const uint32_t *)w->video_pixels};
        return NMEDIA_VIDEO;
    }
    /* END on the control lane cannot overtake an in-flight final image. */
    if(w->output_ready&&w->output.kind==NMEDIA_END&&(w->video_head_pos||w->video_ready||w->video_expected_sequence>w->video_sequence))return NMSW_BUSY;
    if(w->output_ready){w->output_ready=false;*out=w->output;int kind=out->kind;w->video_taken=kind==NMEDIA_VIDEO;w->taken_video_lane=false;
        w->taken_slot=w->output_slot;w->taken_revision=w->output_revision;if(kind==NMSW_BUSY){memset(out,0,sizeof *out);
        /* BUSY owns no borrowed span: enqueue its continuation now, rather
         * than spending another handoff just to discover an empty head. */
        if(!w->head&&!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue allocation/size failure");return NMEDIA_ERROR;}
    }return kind;}
    if(w->starving&&!w->head)return w->video_head_pos||w->video_expected_sequence>w->video_sequence?NMSW_BUSY:NMEDIA_AGAIN;
    if(!w->head&&!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue allocation/size failure");return NMEDIA_ERROR;}return NMSW_BUSY;
}
bool nmedia_mse_worker_move_video(nmedia_mse_worker *w,const struct nmedia_output *o,
    uint32_t generation,unsigned slot,uint64_t revision,uint32_t **pixels,size_t *capacity){
    uint32_t bytes;
    if(w&&w->taken_video_lane){
        const struct nmsw_video *v=&w->video_header;
        if(!o||!pixels||!capacity||w->stopping||w->error[0]||!w->video_loan||generation!=w->generation||
           slot!=w->taken_slot||revision!=w->taken_revision||revision!=nmedia_mse_worker_revision(w,slot)||!video_current(w)||
           o->kind!=NMEDIA_VIDEO||o->pixels!=(const uint32_t *)w->video_pixels||o->width!=v->width||o->height!=v->height||o->pts_ms!=v->pts||
           !nmedia_video_wire_bytes(o->width,o->height,&bytes)||w->video_capacity<bytes||
           *capacity>SIZE_MAX/sizeof(uint32_t)||(!*pixels&&*capacity)||*pixels==(uint32_t *)w->video_pixels)return false;
        uint32_t *completed=(uint32_t *)w->video_pixels;size_t available=w->video_capacity/sizeof(uint32_t);
        w->video_pixels=(uint8_t *)*pixels;w->video_capacity=*capacity*sizeof(uint32_t);video_release(w);w->taken_video_lane=false;
        *pixels=completed;*capacity=available;return true;
    }
    if(!w||!o||!pixels||!capacity||w->stopping||w->error[0]||w->head||w->output_ready||!w->video_taken||
       generation!=w->generation||slot!=w->output_slot||slot>=w->slot_count||!w->slots[slot].used||
       revision!=w->output_revision||revision!=nmedia_mse_worker_revision(w,slot)||
       o->kind!=NMEDIA_VIDEO||w->output.kind!=NMEDIA_VIDEO||o->pixels!=(const uint32_t *)w->payload||
       o->pixels!=w->output.pixels||o->width!=w->output.width||o->height!=w->output.height||o->pts_ms!=w->output.pts_ms||
       !nmedia_video_wire_bytes(o->width,o->height,&bytes)||w->capacity<bytes||
       *capacity>SIZE_MAX/sizeof(uint32_t)||(!*pixels&&*capacity)||*pixels==(uint32_t *)w->payload)return false;
    uint32_t *completed=(uint32_t *)w->payload;size_t available=w->capacity/sizeof(uint32_t);
    w->payload=(uint8_t *)*pixels;w->capacity=*capacity*sizeof(uint32_t);w->video_taken=false;
    /* Preserve kind for prefetch eligibility, but no longer advertise a
       borrowed pointer after the allocation has moved to display/lookahead. */
    w->output.pixels=NULL;w->output.samples=NULL;*pixels=completed;*capacity=available;return true;
}
void nmedia_mse_worker_prefetch(nmedia_mse_worker *w,uint64_t until){
    if(w&&w->video_loan)video_release(w);
    if(!w||w->stopping||w->error[0]||w->head||w->output_ready||w->starving)return;
    int kind=w->output.kind;
    if(kind!=NMEDIA_AUDIO&&kind!=NMEDIA_VIDEO&&kind!=NMSW_CLOCK)return;
    /* The caller has consumed/copied the last borrowed span. Reuse its charged
     * payload, dispatch exactly one STEP, and overlap decode with GUI/JS work.
     * No queue growth, extra frame buffer, wait, or new burst deadline. */
    if(!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue allocation/size failure");return;}
    nmedia_mse_worker_pump_budget(w,uptime_ms(),until);
}
void nmedia_mse_worker_close(nmedia_mse_worker *w){if(!w)return;nmedia_mse_worker **p=&live;while(*p&&*p!=w)p=&(*p)->next;if(!*p)return;*p=w->next;stop(w,NULL);
    if(reap(w))destroy(w);else{w->next=retired;retired=w;}}
int64_t nmedia_mse_worker_deadline(uint64_t now){
    bool stopping=retired!=NULL;for(nmedia_mse_worker *w=live;w;w=w->next){if((w->head||w->video_head_pos)&&!w->stopping)return (int64_t)now+1;if(w->stopping&&w->pid>0)stopping=true;}
    return stopping?(int64_t)now+10:-1;
}

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op==NMSW_SEEK)return true;return false;}
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count?(w->slots[slot].epoch<<32)^w->slots[slot].revision:0;}
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *w){return w?w->taken_slot:0;}
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count&&w->slots[slot].parsing;}
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count?w->slots[slot].group_end:0;}
void nmedia_mse_worker_time(nmedia_mse_worker *w,int64_t ms){if(w)w->current=ms;}
