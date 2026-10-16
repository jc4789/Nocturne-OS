/* Nonblocking GUI-side transport for a dynamic native MSE child registry.
 * The child gets a control channel (one command and one response per
 * message) and a VIDEO channel (headers; new shared pages travel attached). */
#include "media_mse_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
struct request {struct request *next;struct nmsw_command c;uint8_t *bytes;uint64_t epoch;};
struct slot_state {
    uint64_t revision,epoch;uint32_t parsing;int64_t group_end;
    struct nmedia_info info;bool metadata,used,key_waiting;
    size_t count,capacity;struct nmedia_time_range *ranges;
};
struct nmedia_mse_worker {
    struct nmedia_mse_worker *next;
    n_handle app,control,video;uint32_t generation,sequence,completed_sequence;
    bool reserved,stopping,sent,output_ready,quota_error,starving,video_taken;
    uint64_t decoded_frames,dropped_frames;
    uint64_t service_at,service_progress;
    unsigned service_idle_ms;
    struct request *head,*tail;
    size_t queued_bytes,queued_count,rx_capacity,capacity;
    uint8_t *rx; /* the last response message; meta points into it */
    uint8_t *payload;int64_t current;
    struct nmsw_response response;
    struct nmedia_output output;unsigned output_slot;uint64_t output_revision;
    struct nmsw_video video_header;
    uint8_t *video_pixels;size_t video_capacity;
    struct nmsw_shared_video *shared_video;size_t video_shared_bytes;uint64_t video_shared_id;
    bool video_ready,video_loan;uint32_t video_sequence,video_expected_sequence;
    unsigned taken_slot;uint64_t taken_revision;bool taken_video_lane;
    const uint8_t *meta;struct slot_state *slots;size_t slot_count,slot_capacity;uint64_t epoch;
    char error[160];
};
static nmedia_mse_worker *live,*retired;
static void destroy(nmedia_mse_worker *w){for(size_t i=0;i<w->slot_count;i++)nmedia_ff_free(w->slots[i].ranges);nmedia_ff_free(w->slots);nmedia_ff_free(w->rx);nmedia_ff_free(w->payload);if(w->shared_video)n_memory_release(w->shared_video,w->video_shared_bytes);nmedia_ff_free(w);}
static bool reserve_slots(nmedia_mse_worker *w,size_t count){
    if(count>w->slot_capacity){size_t capacity=w->slot_capacity?w->slot_capacity:4;
        while(capacity<count){if(capacity>SIZE_MAX/sizeof *w->slots/2){capacity=count;break;}capacity*=2;}
        if(capacity>SIZE_MAX/sizeof *w->slots)return false;
        struct slot_state *slots=nmedia_ff_realloc(w->slots,capacity*sizeof *slots);if(!slots)return false;
        memset(slots+w->slot_capacity,0,(capacity-w->slot_capacity)*sizeof *slots);w->slots=slots;w->slot_capacity=capacity;
    }if(count>w->slot_count)w->slot_count=count;return true;
}
static void request_free(struct request *r){if(r->bytes&&r->c.op==NMSW_KEYS){volatile uint8_t *wipe=r->bytes;for(size_t i=0;i<sizeof r->c+r->c.bytes;i++)wipe[i]=0;}nmedia_ff_free(r->bytes);nmedia_ff_free(r);}
static void clear_queue(nmedia_mse_worker *w){while(w->head){struct request *r=w->head;w->head=r->next;request_free(r);}w->tail=NULL;w->queued_bytes=w->queued_count=0;}
static void stop(nmedia_mse_worker *w,const char *error){
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;w->stopping=true;w->output_ready=w->video_taken=w->video_ready=w->video_loan=false;
    if(w->control>=0)n_handle_close(w->control);if(w->video>=0)n_handle_close(w->video);w->control=w->video=-1;if(w->app>=0)n_app_stop(w->app,0);clear_queue(w);
}
static bool reap(nmedia_mse_worker *w){
    if(w->app>=0){if(n_app_result(w->app,0,NULL)==-N_ERR_NOT_READY)return false;n_handle_close(w->app);w->app=-1;}
    if(w->reserved){nmedia_alloc_release_worker();w->reserved=false;}return true;
}
void nmedia_mse_worker_background(uint64_t now){
    (void)now;nmedia_mse_worker **p=&retired;while(*p){nmedia_mse_worker *w=*p;if(reap(w)){*p=w->next;destroy(w);}else p=&w->next;}
    for(nmedia_mse_worker *w=live;w;w=w->next)if(w->stopping)reap(w);
}
static bool spawn_child(nmedia_mse_worker *w){
    if(!nmedia_alloc_reserve_worker()){stop(w,"MSE worker count overflow");return false;}w->reserved=true;
    n_handle control[2]={-1,-1},video[2]={-1,-1};
    if(n_channel_pair(control)<0||n_channel_pair(video)<0){
        for(int i=0;i<2;i++){if(control[i]>=0)n_handle_close(control[i]);if(video[i]>=0)n_handle_close(video[i]);}
        stop(w,"cannot create native MSE worker channels");reap(w);return false;
    }
    /* No input, output or log: stray diagnostics cannot reach either channel. */
    const char *args[]={"browsermseworker"};const int32_t handles[]={-1,-1,-1,control[1],video[1]};
    struct n_launch l={.arguments=args,.argument_count=1,.handles=handles,.handle_count=5};
    w->app=n_app_launch("/bin/browsermseworker",&l);
    n_handle_close(control[1]);n_handle_close(video[1]);
    w->control=control[0];w->video=video[0];
    if(w->app<0){w->app=-1;stop(w,"cannot launch native MSE worker");reap(w);return false;}
    return true;
}
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t generation,char *error,size_t size){
    nmedia_mse_worker_background(uptime_ms());
    nmedia_mse_worker *w=nmedia_ff_mallocz(sizeof *w);if(!w){if(error&&size)strlcpy(error,"MSE worker allocation failed",size);return NULL;}
    w->app=w->control=w->video=-1;w->generation=generation;w->next=live;live=w;if(error&&size)*error=0;return w;
}
bool nmedia_mse_worker_command(nmedia_mse_worker *w,unsigned op,unsigned slot,const void *bytes,size_t size,int64_t offset,int64_t start,int64_t end,bool flag){
    if(!w||w->stopping||slot>INT32_MAX||op<NMSW_ADD||op>NMSW_KEYS||size>UINT32_MAX||(!bytes&&size)||w->queued_count==SIZE_MAX||size>SIZE_MAX-w->queued_bytes)return false;
    if(op==NMSW_KEYS&&size%32)return false;
    bool local=op==NMSW_ADD||op==NMSW_APPEND||op==NMSW_REMOVE||op==NMSW_ABORT||op==NMSW_DROP||op==NMSW_CHANGE;
    if(op==NMSW_SELECT&&slot>1)return false; /* audio/video kind, not a buffer count */
    if(local&&op!=NMSW_ADD&&(slot>=w->slot_count||!w->slots[slot].used))return false;
    if((op==NMSW_ADD||op==NMSW_DROP)&&w->epoch==UINT64_MAX)return false;
    if(op==NMSW_ADD&&!reserve_slots(w,(size_t)slot+1))return false;
    if(op==NMSW_ADD&&w->slots[slot].used)return false;
    struct request *r=nmedia_ff_mallocz(sizeof *r);if(!r)return false;
    /* Room for the command before the bytes: the message is sent from here. */
    if(size){r->bytes=nmedia_ff_malloc(sizeof r->c+size);if(!r->bytes){nmedia_ff_free(r);return false;}memcpy(r->bytes+sizeof r->c,bytes,size);}
    if(op==NMSW_ADD||op==NMSW_DROP){struct slot_state *s=&w->slots[slot];s->epoch=++w->epoch;s->used=op==NMSW_ADD;s->metadata=false;s->count=0;s->parsing=0;s->group_end=0;s->revision=0;}
    w->video_taken=false;r->epoch=w->epoch;
    r->c=(struct nmsw_command){.magic=NMSW_MAGIC,.op=op,.generation=w->generation,.slot=slot,.flags=flag,.bytes=(uint32_t)size,.offset=offset,.start=start,.end=end,.current=w->current,.epoch=r->epoch};
    if(w->tail)w->tail->next=r;else w->head=r;w->tail=r;w->queued_bytes+=size;w->queued_count++;
    if(op!=NMSW_STEP){w->service_at=0;w->service_idle_ms=0;}
    if(op!=NMSW_STEP){if(op==NMSW_ABORT){w->error[0]=0;w->quota_error=false;}}
    if(w->head==r){r->c.sequence=++w->sequence;w->sent=false;memset(&w->response,0,sizeof w->response);}return true;
}
static bool valid(nmedia_mse_worker *w){
    const struct nmsw_response *r=&w->response;const struct nmsw_command *c=&w->head->c;
    if(r->magic!=NMSW_MAGIC||r->op!=c->op||r->generation!=w->generation||r->sequence!=c->sequence||r->slot<0||r->quota_error>1||!memchr(r->error,0,sizeof r->error)||
       r->metadata_count>w->slot_count||r->metadata_count>r->metadata_bytes/sizeof(struct nmsw_metadata))return false;
    if((r->kind==NMEDIA_AUDIO||r->kind==NMEDIA_VIDEO||r->kind==NMSW_CLOCK)&&(size_t)r->slot>=w->slot_count)return false;
    if(r->kind==NMEDIA_AUDIO)return r->op==NMSW_STEP&&r->frames>0&&r->frames<=4096&&r->payload_bytes==r->frames*4;
    return (r->kind==NMEDIA_AGAIN||r->kind==NMEDIA_END||r->kind==NMEDIA_ERROR||r->kind==NMSW_BUSY||r->kind==NMSW_CLOCK||
        (r->kind==NMSW_VIDEO_PENDING&&r->op==NMSW_STEP))&&!r->payload_bytes;
}
static bool metadata_valid(nmedia_mse_worker *w){
    size_t at=0;uint32_t previous=0;
    for(uint32_t i=0;i<w->response.metadata_count;i++){
        if(sizeof(struct nmsw_metadata)>w->response.metadata_bytes-at)return false;
        const struct nmsw_metadata *m=(const void *)(w->meta+at);at+=sizeof *m;const struct nmsw_info *n=&m->info;
        if(m->slot>=w->slot_count||(i&&m->slot<=previous)||m->key_waiting>1||m->parsing>1||m->group_end<0||m->group_end>1000001000000LL||
           n->duration< -1||n->duration>1000000000000LL||n->audio>1||n->video>1||n->channels>8||n->sample_rate>384000||!nmedia_video_dimensions(n->width,n->height)||
           !memchr(n->audio_codec,0,32)||!memchr(n->video_codec,0,32)||!memchr(n->container,0,32)||m->count>(w->response.metadata_bytes-at)/sizeof(struct nmedia_time_range))return false;
        const struct nmedia_time_range *ranges=(const void *)(w->meta+at);
        for(uint32_t j=0;j<m->count;j++)if(ranges[j].start_ms<0||ranges[j].end_ms<=ranges[j].start_ms||ranges[j].end_ms>1000001000000LL||(j&&ranges[j-1].end_ms>ranges[j].start_ms))return false;
        at+=(size_t)m->count*sizeof *ranges;previous=m->slot;
    }return at==w->response.metadata_bytes;
}
static void completed(nmedia_mse_worker *w){
    /* Empty STEP acknowledgements are flow-control state, not useful supply
       progress. They must not keep DOM safepoints polling an idle child. */
    if(w->response.op!=NMSW_STEP||
       (w->response.kind!=NMSW_BUSY&&w->response.kind!=NMEDIA_AGAIN)||
       w->response.decoded_frames||w->response.dropped_frames)w->service_progress++;
    struct nmsw_response *r=&w->response;
    w->completed_sequence=r->sequence;
    w->decoded_frames+=r->decoded_frames;w->dropped_frames+=r->dropped_frames;
    if(r->kind==NMSW_VIDEO_PENDING)w->video_expected_sequence=r->sequence;
    size_t at=0;
    for(uint32_t i=0;i<r->metadata_count;i++){const struct nmsw_metadata *m=(const void *)(w->meta+at);at+=sizeof *m;const struct nmedia_time_range *ranges=(const void *)(w->meta+at);at+=(size_t)m->count*sizeof *ranges;
        struct slot_state *s=&w->slots[m->slot];if(w->head->epoch<s->epoch)continue;const struct nmsw_info *a=&m->info;
        if(m->count>s->capacity){struct nmedia_time_range *v=nmedia_ff_realloc(s->ranges,(size_t)m->count*sizeof *v);if(!v){stop(w,"MSE range allocation failed");return;}s->ranges=v;s->capacity=m->count;}
        s->metadata=a->audio||a->video;s->info=(struct nmedia_info){.audio=a->audio,.video=a->video,.channels=(int)a->channels,.sample_rate=(int)a->sample_rate,.width=a->width,.height=a->height,.duration_ms=a->duration};
        strlcpy(s->info.audio_codec,a->audio_codec,32);strlcpy(s->info.video_codec,a->video_codec,32);strlcpy(s->info.container,a->container,32);
        s->revision=m->revision;s->parsing=m->parsing;s->key_waiting=!!m->key_waiting;s->group_end=m->group_end;s->count=m->count;if(s->count)memcpy(s->ranges,ranges,s->count*sizeof *ranges);}
    if(r->kind==NMEDIA_ERROR){strlcpy(w->error,r->error[0]?r->error:"native MSE worker failed",sizeof w->error);w->quota_error=r->quota_error;}
    else if(r->op==NMSW_ABORT||w->quota_error){w->error[0]=0;w->quota_error=false;if(w->output.kind==NMEDIA_ERROR)w->output_ready=false;}
    if(w->output_ready&&nmedia_mse_worker_revision(w,w->output_slot)!=w->output_revision)w->output_ready=false;
    bool data=r->kind==NMEDIA_AUDIO||r->kind==NMEDIA_VIDEO||r->kind==NMSW_CLOCK;
    bool current_epoch=data?w->head->epoch>=w->slots[r->slot].epoch:w->head->epoch==w->epoch;
    if(r->op==NMSW_STEP&&current_epoch){w->output_ready=true;w->output_slot=(unsigned)r->slot;w->output_revision=nmedia_mse_worker_revision(w,w->output_slot);
        w->starving=r->kind==NMEDIA_AGAIN;
        w->output=(struct nmedia_output){.kind=r->kind==NMSW_VIDEO_PENDING?NMSW_BUSY:r->kind,.pts_ms=r->pts,.frames=r->frames,.width=r->width,.height=r->height,.samples=(const int16_t *)w->payload,.audio_observation=r->audio_observation};}
    else if(r->kind!=NMEDIA_ERROR)w->starving=false;
    struct request *old=w->head;w->head=old->next;w->queued_bytes-=old->c.bytes;w->queued_count--;
    request_free(old);if(!w->head)w->tail=NULL;
    w->sent=false;
    if(w->head){w->head->c.sequence=++w->sequence;memset(&w->response,0,sizeof w->response);}
}
static void control_io(nmedia_mse_worker *w,bool dispatch){
    if(!w||w->stopping||!w->head||w->app<0)return;
    if(!w->sent){
        if(!dispatch)return;
        /* Queued APPEND/REMOVE can wait behind transfer or author JS work. Their
           decoder resume point must use the real sink clock at dispatch, not the
           old enqueue clock. The child takes one command at a time, so its queue
           is empty and the send never waits. */
        struct request *r=w->head;r->c.current=w->current;n_result sent;
        if(r->c.bytes){memcpy(r->bytes,&r->c,sizeof r->c);sent=n_channel_send(w->control,r->bytes,sizeof r->c+(size_t)r->c.bytes,-1);}
        else sent=n_channel_send(w->control,&r->c,sizeof r->c,-1);
        if(sent<0){stop(w,"MSE worker command channel failed");return;}
        w->sent=true;
    }
    n_result size=n_channel_receive(w->control,NULL,0,N_CHANNEL_PEEK,NULL);
    if(size==-N_ERR_NOT_READY)return;
    if(size<0){stop(w,"MSE worker response ended early");return;}
    if((uint64_t)size<sizeof w->response||(uint64_t)size>sizeof w->response+2ull*UINT32_MAX){stop(w,"invalid native MSE worker response");return;}
    if((size_t)size>w->rx_capacity){
        /* A response is a replacement, not an append: never copy stale bytes. */
        nmedia_ff_free(w->rx);w->rx=NULL;w->rx_capacity=0;
        uint8_t *a=nmedia_ff_malloc((size_t)size);if(!a){stop(w,"MSE metadata allocation failed");return;}w->rx=a;w->rx_capacity=(size_t)size;
    }
    if(n_channel_receive(w->control,w->rx,w->rx_capacity,0,NULL)!=size){stop(w,"MSE worker response ended early");return;}
    memcpy(&w->response,w->rx,sizeof w->response);w->meta=w->rx+sizeof w->response;
    if(!valid(w)||(uint64_t)size!=sizeof w->response+(uint64_t)w->response.metadata_bytes+w->response.payload_bytes){stop(w,"invalid native MSE worker response");return;}
    if(!metadata_valid(w)){stop(w,"invalid MSE metadata wire");return;}
    if(w->response.payload_bytes){
        /* Dispatch released the prior borrowed PCM span. */
        if(w->response.payload_bytes>w->capacity){
            nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
            uint8_t *a=nmedia_ff_malloc(w->response.payload_bytes);if(!a){stop(w,"MSE worker frame allocation failed");return;}w->payload=a;w->capacity=w->response.payload_bytes;
        }
        memcpy(w->payload,w->meta+w->response.metadata_bytes,w->response.payload_bytes);
    }
    completed(w);
}
static void video_release(nmedia_mse_worker *w){
    if((w->video_ready||w->video_loan)&&w->shared_video)
        __atomic_store_n(&w->shared_video->occupied[w->video_header.buffer_slot],0,__ATOMIC_RELEASE);
    w->video_ready=w->video_loan=false;
    w->video_pixels=NULL;
}
static bool video_current(const nmedia_mse_worker *w){
    const struct nmsw_video *v=&w->video_header;
    return v->slot<w->slot_count&&w->slots[v->slot].used&&
        v->epoch>=w->slots[v->slot].epoch&&v->revision==w->slots[v->slot].revision;
}
static void video_io(nmedia_mse_worker *w){
    if(w->video<0||w->video_ready||w->video_loan)return;
    n_handle attached=-1;
    n_result n=n_channel_receive(w->video,&w->video_header,sizeof w->video_header,0,&attached);
    if(n==-N_ERR_NOT_READY)return;
    if(n!=(n_result)sizeof w->video_header){
        if(attached>=0)n_handle_close(attached);
        stop(w,n<0&&n!=-N_ERR_TOO_LARGE?"MSE VIDEO channel ended early":"invalid MSE VIDEO wire");return;
    }
    const struct nmsw_video *v=&w->video_header;uint32_t bytes;size_t shared_bytes;
    bool fresh=v->shared_id!=w->video_shared_id;
    if(v->magic!=NMSW_MAGIC||v->generation!=w->generation||!v->sequence||v->sequence<=w->video_sequence||v->sequence>w->sequence||
       v->slot>=w->slot_count||v->reserved||v->shared_reserved||v->attached!=fresh||(attached>=0)!=fresh||!v->shared_id||v->buffer_slot>=NMSW_VIDEO_SLOTS||
       !nmedia_video_wire_bytes(v->width,v->height,&bytes)||bytes!=v->bytes||bytes>v->shared_capacity||
       !nmsw_shared_size(v->shared_capacity,&shared_bytes)){if(attached>=0)n_handle_close(attached);stop(w,"invalid MSE VIDEO wire");return;}
    if(fresh){
        /* The mapping keeps the pages; the handle is not needed after it. */
        struct nmsw_shared_video *shared=n_shared_map(attached,shared_bytes,N_MEMORY_READ_WRITE);
        n_handle_close(attached);
        if(!shared){stop(w,"MSE shared VIDEO import failed");return;}
        if(w->shared_video)n_memory_release(w->shared_video,w->video_shared_bytes);
        w->shared_video=shared;w->video_shared_bytes=shared_bytes;w->video_shared_id=v->shared_id;
    }else if(shared_bytes!=w->video_shared_bytes){stop(w,"MSE shared VIDEO geometry changed without a new owner");return;}
    if(!nmsw_shared_matches(w->shared_video,v)){stop(w,"MSE shared VIDEO identity/slot is not owned");return;}
    w->video_pixels=(uint8_t *)(w->shared_video+1)+(size_t)v->buffer_slot*v->shared_capacity;
    w->video_capacity=v->shared_capacity;
    w->service_progress++;
    w->video_sequence=v->sequence;w->video_ready=true;
}
static void pump_io(nmedia_mse_worker *w,uint64_t now,bool dispatch){
    if(!w||w->stopping)return;
    nmedia_mse_worker_background(now);
    if(w->app<0){if(!dispatch||!w->head||!spawn_child(w))return;}
    /* PCM/control first, but delivered VIDEO is taken in the same pass:
       sustained APPEND/control progress must not suppress it. */
    control_io(w,dispatch);if(w->stopping)return;
    video_io(w);
}
void nmedia_mse_worker_pump_budget(nmedia_mse_worker *w,uint64_t now,uint64_t until){(void)until;pump_io(w,now,true);}
void nmedia_mse_worker_collect(nmedia_mse_worker *w){uint64_t now=uptime_ms();pump_io(w,now,false);}
bool nmedia_mse_worker_service_due(const nmedia_mse_worker *w,uint64_t now){
    return w&&w->app>=0&&!w->stopping&&
        (w->video_ready||(w->output_ready&&w->output.kind!=NMSW_BUSY)||now>=w->service_at);
}
void nmedia_mse_worker_service(nmedia_mse_worker *w,uint64_t now,uint64_t until){(void)until;
    if(!nmedia_mse_worker_service_due(w,now))return;
    uint64_t progress=w->service_progress;pump_io(w,now,true);
    if(w->service_progress!=progress){w->service_idle_ms=0;w->service_at=0;}
    else{
        /* No sleep/yield: reserve the next readiness probe instead of paying
           several syscalls at every layout safepoint for the same empty lane.
           16 ms is below one 48 kHz 1024-sample packet and the native PCM ring
           headroom. New APPEND/key/control work immediately clears this wait. */
        w->service_idle_ms=w->service_idle_ms?MIN(w->service_idle_ms*2,16u):1;
        w->service_at=uptime_ms()+w->service_idle_ms;
    }
}
bool nmedia_mse_worker_needs_service(const nmedia_mse_worker *w){return w&&w->app>=0&&!w->stopping&&
    (w->head||w->output_ready||w->video_ready||w->video_expected_sequence>w->video_sequence||!w->starving);}
void nmedia_mse_worker_video_stats(const nmedia_mse_worker *w,uint64_t *decoded,uint64_t *dropped){if(decoded)*decoded=w?w->decoded_frames:0;if(dropped)*dropped=w?w->dropped_frames:0;}
void nmedia_mse_worker_pump(nmedia_mse_worker *w,uint64_t now){nmedia_mse_worker_pump_budget(w,now,uptime_ms()+2);}
bool nmedia_mse_worker_pending(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op!=NMSW_STEP&&r->c.op!=NMSW_SEEK)return true;return false;}
bool nmedia_mse_worker_running(const nmedia_mse_worker *w){return w&&w->app>=0&&!w->stopping;}
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
        *out=(struct nmedia_output){.kind=NMEDIA_VIDEO,.pts_ms=v->pts,.width=v->width,.height=v->height,.pixels=(const uint32_t *)w->video_pixels,
            .video_measured_frames=v->video_measured_frames,.video_work_ms=v->video_work_ms,
            .video_measurement_id=v->video_measurement_id,.video_configuration=v->video_configuration};
        return NMEDIA_VIDEO;
    }
    /* END on the control lane cannot overtake an in-flight final image. */
    if(w->output_ready&&w->output.kind==NMEDIA_END&&(w->video_ready||w->video_expected_sequence>w->video_sequence))return NMSW_BUSY;
    if(w->output_ready){w->output_ready=false;*out=w->output;int kind=out->kind;w->video_taken=kind==NMEDIA_VIDEO;w->taken_video_lane=false;
        w->taken_slot=w->output_slot;w->taken_revision=w->output_revision;if(kind==NMSW_BUSY){memset(out,0,sizeof *out);
        /* BUSY owns no borrowed span: enqueue its continuation now, rather
         * than spending another handoff just to discover an empty head. */
        if(!w->head&&!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue allocation/size failure");return NMEDIA_ERROR;}
    }return kind;}
    if(w->starving&&!w->head)return w->video_expected_sequence>w->video_sequence?NMSW_BUSY:NMEDIA_AGAIN;
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
        /* The display/lookahead owns ordinary allocations. Copy directly from
           the shared slot once, then return it; never free a mapping as heap. */
        size_t count=bytes/sizeof(uint32_t);
        if(*capacity<count){uint32_t *p=nmedia_ff_realloc(*pixels,bytes);if(!p)return false;*pixels=p;*capacity=count;}
        memcpy(*pixels,w->video_pixels,bytes);video_release(w);w->taken_video_lane=false;return true;
    }
    return false; /* VIDEO arrives only on its own lane */
}
static void prefetch(nmedia_mse_worker *w,uint64_t until,bool cooperative){
    if(w&&w->video_loan)video_release(w);
    if(!w||w->stopping||w->error[0]||w->head||w->output_ready||w->starving)return;
    int kind=w->output.kind;
    if(kind!=NMEDIA_AUDIO&&kind!=NMEDIA_VIDEO&&kind!=NMSW_CLOCK)return;
    /* The caller has consumed/copied the last borrowed span. Reuse its charged
     * payload, dispatch exactly one STEP, and overlap decode with GUI/JS work.
     * No queue growth, extra frame buffer, wait, or new burst deadline. */
    if(!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue allocation/size failure");return;}
    if(cooperative)nmedia_mse_worker_service(w,uptime_ms(),until);else nmedia_mse_worker_pump_budget(w,uptime_ms(),until);
}
void nmedia_mse_worker_prefetch(nmedia_mse_worker *w,uint64_t until){prefetch(w,until,false);}
void nmedia_mse_worker_prefetch_service(nmedia_mse_worker *w,uint64_t until){prefetch(w,until,true);}
void nmedia_mse_worker_close(nmedia_mse_worker *w){if(!w)return;nmedia_mse_worker **p=&live;while(*p&&*p!=w)p=&(*p)->next;if(!*p)return;*p=w->next;stop(w,NULL);
    if(reap(w))destroy(w);else{w->next=retired;retired=w;}}
int64_t nmedia_mse_worker_deadline(uint64_t now){
    bool stopping=retired!=NULL;int64_t next=-1;
    for(nmedia_mse_worker *w=live;w;w=w->next){
        if((w->head||w->video_expected_sequence>w->video_sequence)&&!w->stopping){
            int64_t due=w->service_at>now?(int64_t)w->service_at:(int64_t)now+1;
            if(next<0||due<next)next=due;
        }if(w->stopping&&w->app>=0)stopping=true;
    }
    if(stopping&&(next<0||next>(int64_t)now+10))next=(int64_t)now+10;
    return next;
}

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op==NMSW_SEEK)return true;return false;}
bool nmedia_mse_worker_key_waiting(const nmedia_mse_worker *w){if(w)for(size_t i=0;i<w->slot_count;i++)if(w->slots[i].used&&w->slots[i].key_waiting)return true;return false;}
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count?(w->slots[slot].epoch<<32)^w->slots[slot].revision:0;}
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *w){return w?w->taken_slot:0;}
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count&&w->slots[slot].parsing;}
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *w,unsigned slot){return w&&slot<w->slot_count?w->slots[slot].group_end:0;}
void nmedia_mse_worker_time(nmedia_mse_worker *w,int64_t ms){if(w)w->current=ms;}
