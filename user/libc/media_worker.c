/* Each Browser Range element owns an independent native child/transport.
 * All GUI-side IO is nonblocking. The child gets two message channels: the
 * control channel carries one command and one response (header, then PCM)
 * per message; the VIDEO channel carries one frame per message (pixels, then
 * header) and, back to the child, one credit per frame it may send.
 * Retired state deliberately contains no document/node/JS pointers. */
#include "media_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
struct nmedia_worker {
    struct nmedia_worker *next;
    n_handle app,control,video;
    uint32_t generation,sequence,completed_sequence,video_sequence,video_expected_sequence;
    uint64_t epoch;
    struct nmedia_worker_video video_header;
    uint8_t *video_pixels;size_t video_capacity; /* bytes, room for the header trailer */
    bool video_ready,video_loan;
    bool reserved,stopping,waiting,sent,metadata,output_ready,seeking,want_seek,video_taken;
    int64_t seek_ms;
    uint64_t request_at;
    struct nmedia_worker_stats stats;
    struct nmedia_info info;
    struct nmedia_worker_command command;
    struct nmedia_worker_response response;
    unsigned char *tx; /* exact OPEN storage; STEP/SEEK send command itself */
    size_t tx_len,capacity;
    unsigned char *payload; /* the last response message: header, then PCM */
    char error[160];
};
static nmedia_worker *live,*retired;
static void destroy(nmedia_worker *w){nmedia_ff_free(w->payload);nmedia_ff_free(w->video_pixels);nmedia_ff_free(w->tx);nmedia_ff_free(w);}
static void channels_close(nmedia_worker *w) {
    if(w->control>=0)n_handle_close(w->control);if(w->video>=0)n_handle_close(w->video);w->control=w->video=-1;
}
static void stop(nmedia_worker *w,const char *error) {
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;
    w->stopping=true;w->waiting=w->seeking=w->want_seek=w->output_ready=w->video_taken=w->video_ready=w->video_loan=false;
    channels_close(w);if(w->app>=0)n_app_stop(w->app,0);
    nmedia_ff_free(w->tx);w->tx=NULL;w->tx_len=0;
}
static bool reap(nmedia_worker *w) {
    if(w->app>=0) {
        if(n_app_result(w->app,0,NULL)==-N_ERR_NOT_READY)return false;
        n_handle_close(w->app);w->app=-1;
    }
    if(w->reserved){nmedia_alloc_release_worker();w->reserved=false;}
    return true;
}
void nmedia_worker_background(uint64_t now) {
    (void)now;
    nmedia_worker **p=&retired;
    while(*p){nmedia_worker *w=*p;if(reap(w)){*p=w->next;destroy(w);}else p=&w->next;}
    for(nmedia_worker *w=live;w;w=w->next)if(w->stopping)reap(w);
}
static void queue(nmedia_worker *w,unsigned op,int64_t seek) {
    w->command=(struct nmedia_worker_command){.magic=NMEDIA_WORKER_MAGIC,.operation=op,
        .generation=w->generation,.sequence=++w->sequence,.seek_ms=seek,.epoch=w->epoch};
    nmedia_ff_free(w->tx);w->tx=NULL;w->tx_len=sizeof w->command;w->sent=false;
    memset(&w->response,0,sizeof w->response);
    w->waiting=true;w->output_ready=w->video_taken=false;w->request_at=uptime_ms();w->stats.requests++;
}
static bool credit(nmedia_worker *w){uint8_t one=1;return n_channel_send(w->video,&one,sizeof one,-1)==1;}
static bool spawn_child(nmedia_worker *w) {
    if(!nmedia_alloc_reserve_worker()){stop(w,"media worker aggregate reservation denied");return false;}
    w->reserved=true;
    n_handle control[2]={-1,-1},video[2]={-1,-1};
    if(n_channel_pair(control)<0||n_channel_pair(video)<0){
        for(int i=0;i<2;i++){if(control[i]>=0)n_handle_close(control[i]);if(video[i]>=0)n_handle_close(video[i]);}
        stop(w,"cannot create native media worker channels");reap(w);return false;
    }
    /* No input, output or log: stray diagnostics cannot reach either channel. */
    const char *args[]={"browsermediaworker"};const int32_t handles[]={-1,-1,-1,control[1],video[1]};
    struct n_launch l={.arguments=args,.argument_count=1,.handles=handles,.handle_count=5};
    w->app=n_app_launch(N_FOLDER_SYSTEM "\\browsermediaworker",&l);
    n_handle_close(control[1]);n_handle_close(video[1]);
    w->control=control[0];w->video=video[0];
    if(w->app<0){w->app=-1;stop(w,"cannot launch native media worker");reap(w);return false;}
    if(!credit(w)){stop(w,"Range VIDEO channel setup failed");return false;}
    return true;
}
static bool open_size(size_t url,size_t document,size_t *size) {
    if(!url||!document||url>UINT32_MAX||document>UINT32_MAX||
       document>SIZE_MAX-sizeof(struct nmedia_worker_command)||
       url>SIZE_MAX-sizeof(struct nmedia_worker_command)-document)return false;
    *size=sizeof(struct nmedia_worker_command)+url+document;return true;
}
nmedia_worker *nmedia_worker_open(const char *url,const char *document,uint32_t generation,char *error,size_t size) {
    nmedia_worker_background(uptime_ms());
    size_t bytes;
    if(!url||!document||!open_size(strlen(url),strlen(document),&bytes)){
        if(error&&size)snprintf(error,size,"native media OPEN URL length is not representable");return NULL;
    }
    nmedia_worker *w=nmedia_ff_mallocz(sizeof *w);
    if(!w){if(error&&size)snprintf(error,size,"native media worker control allocation");return NULL;}
    w->app=w->control=w->video=-1;w->epoch=1;w->generation=generation;w->info.duration_ms=-1;queue(w,NMW_OPEN,0);
    w->tx=nmedia_ff_malloc(bytes);
    if(!w->tx){if(error&&size)snprintf(error,size,"native media OPEN storage allocation failed");nmedia_ff_free(w);return NULL;}
    w->command.url_bytes=(uint32_t)strlen(url);w->command.document_bytes=(uint32_t)strlen(document);
    memcpy(w->tx,&w->command,sizeof w->command);
    memcpy(w->tx+sizeof w->command,url,w->command.url_bytes);
    memcpy(w->tx+sizeof w->command+w->command.url_bytes,document,w->command.document_bytes);
    w->tx_len=bytes;
    w->next=live;live=w;
    if(error&&size)*error=0;return w;
}
static bool valid_response(nmedia_worker *w) {
    struct nmedia_worker_response *r=&w->response;
    if(r->magic!=NMEDIA_WORKER_MAGIC||r->operation!=w->command.operation||r->generation!=w->generation||r->sequence!=w->sequence||
       r->reserved||r->audio>1||r->video>1||r->channels>8||r->sample_rate>384000||r->duration_ms< -1||
       !memchr(r->error,0,sizeof r->error)||!memchr(r->audio_codec,0,32)||!memchr(r->video_codec,0,32)||!memchr(r->container,0,32))return false;
    if(r->operation==NMW_OPEN&&!nmedia_video_dimensions(r->width,r->height))return false;
    if(r->operation!=NMW_STEP&&r->kind!=NMEDIA_AGAIN&&r->kind!=NMEDIA_ERROR)return false;
    if(r->kind==NMEDIA_AUDIO)return r->operation==NMW_STEP&&r->audio&&r->channels==2&&r->sample_rate==SOUND_RATE&&
        r->frames>0&&r->frames<=4096&&r->payload_bytes==r->frames*4;
    if(r->kind==NMW_VIDEO_PENDING)return r->operation==NMW_STEP&&r->video&&nmedia_video_dimensions(r->width,r->height)&&!r->payload_bytes;
    return (r->kind==NMEDIA_AGAIN||r->kind==NMEDIA_END||r->kind==NMEDIA_ERROR)&&!r->payload_bytes;
}
static void completed(nmedia_worker *w) {
    struct nmedia_worker_response *r=&w->response;
    w->waiting=false;w->completed_sequence=r->sequence;
    if(r->kind==NMW_VIDEO_PENDING)w->video_expected_sequence=r->sequence;
    uint64_t now=uptime_ms(),elapsed=now>=w->request_at?now-w->request_at:0;
    w->stats.responses++;w->stats.request_ms+=elapsed;w->stats.max_request_ms=MAX(w->stats.max_request_ms,elapsed);
    if(r->kind==NMEDIA_ERROR){stop(w,r->error[0]?r->error:"native media worker failed");return;}
    if(r->operation==NMW_OPEN) {
        nmedia_ff_free(w->tx);w->tx=NULL;
        w->info=(struct nmedia_info){.audio=r->audio,.video=r->video,.channels=(int)r->channels,.sample_rate=(int)r->sample_rate,
            .width=r->width,.height=r->height,.duration_ms=r->duration_ms};
        strlcpy(w->info.audio_codec,r->audio_codec,sizeof w->info.audio_codec);
        strlcpy(w->info.video_codec,r->video_codec,sizeof w->info.video_codec);strlcpy(w->info.container,r->container,sizeof w->info.container);
        w->metadata=true;
    } else if(r->operation==NMW_SEEK&&!w->want_seek)w->seeking=false;
    else if(r->operation==NMW_STEP&&!w->want_seek){w->output_ready=true;if(r->kind==NMEDIA_VIDEO){w->info.width=r->width;w->info.height=r->height;}}
    if(w->want_seek){w->want_seek=false;queue(w,NMW_SEEK,w->seek_ms);}
}
/* The whole response arrives as one message: take it when it is there. */
static void response_io(nmedia_worker *w) {
    n_result size=n_channel_receive(w->control,NULL,0,N_CHANNEL_PEEK,NULL);
    if(size==-N_ERR_NOT_READY)return;
    if(size<0){stop(w,"native media worker response ended early");return;}
    if((uint64_t)size<sizeof w->response||(uint64_t)size>sizeof w->response+(uint64_t)UINT32_MAX){stop(w,"invalid native media worker response");return;}
    if((size_t)size>w->capacity) {
        /* A new response invalidates the old borrowed output.
         * No bytes survive: avoid realloc's old+new copy peak. */
        nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
        unsigned char *b=nmedia_ff_malloc((size_t)size);
        if(!b){stop(w,"native media worker frame aggregate limit");return;}
        w->payload=b;w->capacity=(size_t)size;
    }
    if(n_channel_receive(w->control,w->payload,w->capacity,0,NULL)!=size){stop(w,"native media worker response ended early");return;}
    w->stats.rx_bytes+=(uint64_t)size;
    memcpy(&w->response,w->payload,sizeof w->response);
    if(!valid_response(w)||w->response.payload_bytes!=(uint64_t)size-sizeof w->response){stop(w,"invalid native media worker response");return;}
    completed(w);
}
static void control_io(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool dispatch,bool allow_spawn) {
    nmedia_worker_background(now);
    if(!w||!b||w->stopping||!w->waiting)return;
    /* OPEN can legitimately span several HTTP ranges/probe steps, or wait
       for cooperative scheduling. Do not turn their total wall time into a
       browser-invented fatal error. Child/HTTP errors, a closed channel and
       explicit cancellation still terminate this owner. */
    if(dispatch&&uptime_ms()>=b->until)return;
    if(w->app<0) {if(!dispatch||!allow_spawn)return;if(!spawn_child(w))return;}
    if(!w->sent) {
        if(!dispatch)return;
        /* The child takes one command at a time, so its queue is empty and
           the send never waits. */
        const unsigned char *command=w->tx?w->tx:(const unsigned char *)&w->command;
        if(n_channel_send(w->control,command,w->tx_len,-1)<0){stop(w,"native media worker command channel failed");return;}
        w->sent=true;w->stats.tx_bytes+=w->tx_len;b->tx_bytes-=MIN(b->tx_bytes,w->tx_len);
    }
    response_io(w);
}
static void video_release(nmedia_worker *w){w->video_ready=w->video_loan=false;}
static void video_io(nmedia_worker *w){
    if(w->video<0||w->video_ready||w->video_loan)return;
    n_result size=n_channel_receive(w->video,NULL,0,N_CHANNEL_PEEK,NULL);
    if(size==-N_ERR_NOT_READY)return;
    if(size<0){stop(w,"Range VIDEO channel ended early");return;}
    if((uint64_t)size<=sizeof w->video_header||(uint64_t)size>sizeof w->video_header+(uint64_t)UINT32_MAX){stop(w,"invalid Range VIDEO wire");return;}
    if((size_t)size>w->video_capacity){nmedia_ff_free(w->video_pixels);w->video_pixels=NULL;w->video_capacity=0;
        w->video_pixels=nmedia_ff_malloc((size_t)size);if(!w->video_pixels){stop(w,"Range VIDEO allocation failed");return;}w->video_capacity=(size_t)size;}
    if(n_channel_receive(w->video,w->video_pixels,w->video_capacity,0,NULL)!=size){stop(w,"Range VIDEO channel ended early");return;}
    w->stats.rx_bytes+=(uint64_t)size;
    /* Taken off the queue: the child may send its next frame. */
    if(!credit(w)){stop(w,"Range VIDEO channel ended early");return;}
    memcpy(&w->video_header,w->video_pixels+(size_t)size-sizeof w->video_header,sizeof w->video_header);
    const struct nmedia_worker_video *v=&w->video_header;uint32_t bytes;
    if(v->magic!=NMEDIA_WORKER_MAGIC||v->generation!=w->generation||!v->sequence||v->sequence<=w->video_sequence||v->sequence>w->sequence||v->epoch>w->epoch||
       !nmedia_video_wire_bytes(v->width,v->height,&bytes)||bytes!=v->bytes||(uint64_t)bytes!=(uint64_t)size-sizeof w->video_header){stop(w,"invalid Range VIDEO wire");return;}
    w->video_sequence=v->sequence;w->video_ready=true;
}
static void pump_io(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool dispatch,bool allow_spawn){
    if(!w||!b||w->stopping)return;
    control_io(w,now,b,dispatch,allow_spawn);if(w->stopping)return;
    /* Do not starve delivered VIDEO behind repeated PCM/control progress. */
    video_io(w);
}
void nmedia_worker_snapshot(const nmedia_worker *w,struct nmedia_worker_stats *out){
    if(!out)return;memset(out,0,sizeof *out);if(!w)return;*out=w->stats;
    out->payload_total=w->response.payload_bytes;out->waiting=w->waiting;
}
void nmedia_worker_pump_budget(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool allow_spawn){pump_io(w,now,b,true,allow_spawn);}
void nmedia_worker_collect(nmedia_worker *w,struct nmedia_worker_budget *b){pump_io(w,uptime_ms(),b,false,false);}
void nmedia_worker_pump(nmedia_worker *w,uint64_t now){
    struct nmedia_worker_budget b={.until=uptime_ms()+2,.tx_bytes=65536};
    nmedia_worker_pump_budget(w,now,&b,true);
}
bool nmedia_worker_running(const nmedia_worker *w){return w&&w->app>=0&&!w->stopping;}
bool nmedia_worker_pending(const nmedia_worker *w){return w&&!w->stopping&&(w->waiting||w->video_expected_sequence>w->video_sequence);}
void nmedia_worker_prefetch(nmedia_worker *w,struct nmedia_worker_budget *b){
    if(w&&w->video_loan)video_release(w);
    if(!w||!b||w->stopping||w->waiting||w->output_ready||w->seeking||w->want_seek)return;
    if(w->response.kind!=NMEDIA_AUDIO&&w->response.kind!=NMW_VIDEO_PENDING)return;
    /* Caller has released the borrowed span. Queue at most one successor;
     * transport uses the same time budget and cannot spawn. */
    nmedia_worker_step(w);nmedia_worker_pump_budget(w,uptime_ms(),b,false);
}
const struct nmedia_info *nmedia_worker_info(nmedia_worker *w){return w&&w->metadata?&w->info:NULL;}
const char *nmedia_worker_error(nmedia_worker *w){return w?w->error:"no native worker";}
bool nmedia_worker_loading(nmedia_worker *w){return w&&!w->metadata&&!w->error[0];}
bool nmedia_worker_seeking(nmedia_worker *w){return w&&w->seeking&&!w->error[0];}
bool nmedia_worker_seek(nmedia_worker *w,int64_t ms) {
    if(!w||!w->metadata||w->error[0]||ms<0||ms>1000000000000LL||w->epoch==UINT64_MAX)return false;
    w->epoch++;if(w->video_loan)video_release(w);
    w->seeking=true;w->seek_ms=ms;w->output_ready=w->video_taken=false;
    if(w->waiting)w->want_seek=true;else queue(w,NMW_SEEK,ms);
    return true;
}
void nmedia_worker_step(nmedia_worker *w) {
    if(w&&w->metadata&&!w->stopping&&!w->waiting&&!w->seeking&&!w->output_ready)queue(w,NMW_STEP,0);
}
int nmedia_worker_take(nmedia_worker *w,struct nmedia_output *o) {
    if(!w||w->error[0])return NMEDIA_ERROR;
    if(w->video_loan)video_release(w);
    if(w->video_ready&&w->completed_sequence>=w->video_header.sequence&&w->video_header.epoch!=w->epoch)video_release(w);
    bool video=w->video_ready&&w->completed_sequence>=w->video_header.sequence&&!w->seeking&&!w->want_seek;
    bool audio=w->output_ready&&w->response.kind==NMEDIA_AUDIO;
    if(video&&(!audio||w->video_header.pts<=w->response.pts_ms)){
        const struct nmedia_worker_video *v=&w->video_header;w->video_ready=false;w->video_loan=true;
        w->info.width=v->width;w->info.height=v->height;
        *o=(struct nmedia_output){.kind=NMEDIA_VIDEO,.pts_ms=v->pts,.width=v->width,.height=v->height,.pixels=(const uint32_t *)w->video_pixels,
            .video_measured_frames=v->video_measured_frames,.video_work_ms=v->video_work_ms,
            .video_measurement_id=v->video_measurement_id,.video_configuration=v->video_configuration};return NMEDIA_VIDEO;
    }
    if(!w->output_ready)return NMEDIA_AGAIN;
    struct nmedia_worker_response *r=&w->response;
    if(r->kind==NMEDIA_END&&(w->video_ready||w->video_expected_sequence>w->video_sequence))return NMEDIA_AGAIN;
    w->output_ready=false;
    if(r->kind==NMW_VIDEO_PENDING){memset(o,0,sizeof *o);return NMEDIA_AGAIN;}
    *o=(struct nmedia_output){.kind=r->kind,.pts_ms=r->pts_ms,.frames=r->frames,
        .samples=r->payload_bytes?(const int16_t *)(w->payload+sizeof *r):NULL,.audio_observation=r->audio_observation};return r->kind;
}
bool nmedia_worker_move_video(nmedia_worker *w,const struct nmedia_output *o,uint32_t **pixels,size_t *capacity){
    uint32_t bytes;
    if(!w||!o||!pixels||!capacity||w->stopping||w->seeking||w->want_seek||!w->video_loan||w->video_header.epoch!=w->epoch||
       o->kind!=NMEDIA_VIDEO||o->pixels!=(const uint32_t *)w->video_pixels||o->width!=w->video_header.width||o->height!=w->video_header.height||o->pts_ms!=w->video_header.pts||
       !nmedia_video_wire_bytes(o->width,o->height,&bytes)||bytes!=w->video_header.bytes||w->video_capacity<bytes||
       *capacity>SIZE_MAX/sizeof(uint32_t)||(!*pixels&&*capacity)||*pixels==(uint32_t *)w->video_pixels)return false;
    uint32_t *completed=(uint32_t *)w->video_pixels;size_t available=w->video_capacity/sizeof(uint32_t);
    w->video_pixels=(uint8_t *)*pixels;w->video_capacity=*capacity*sizeof(uint32_t);video_release(w);*pixels=completed;*capacity=available;return true;
}
void nmedia_worker_close(nmedia_worker *w) {
    if(!w)return;nmedia_worker **p=&live;while(*p&&*p!=w)p=&(*p)->next;
    if(!*p)return;*p=w->next;
    stop(w,NULL);nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
    if(reap(w))destroy(w);
    else {w->next=retired;retired=w;}
}
int64_t nmedia_worker_deadline(uint64_t now) {
    bool stopping=retired!=NULL;
    for(nmedia_worker *w=live;w;w=w->next){if(nmedia_worker_pending(w))return (int64_t)now+1;if(w->stopping&&w->app>=0)stopping=true;}
    return stopping?(int64_t)now+10:-1;
}
