/* Each Browser Range element owns an independent native child/transport.
 * All GUI-side IO is nonblocking.
 * Retired state deliberately contains no document/node/JS pointers. */
#include "media_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct nmedia_worker {
    struct nmedia_worker *next;
    int pid,in,out,video_fd;
    uint32_t generation,sequence,completed_sequence,video_sequence,video_expected_sequence;
    uint64_t epoch;
    struct nmedia_worker_video video_header;
    uint8_t *video_pixels;size_t video_head_pos,video_rx_pos,video_capacity;
    bool video_ready,video_loan;
    bool reserved,stopping,waiting,metadata,output_ready,seeking,want_seek,video_taken;
    int64_t seek_ms;
    uint64_t request_at;
    struct nmedia_worker_stats stats;
    struct nmedia_info info;
    struct nmedia_worker_command command;
    struct nmedia_worker_response response;
    unsigned char *tx; /* exact OPEN storage; STEP/SEEK borrow command itself */
    size_t tx_len,tx_pos,head_pos,rx_pos,capacity;
    unsigned char *payload;
    char error[160];
};
static nmedia_worker *live,*retired;
static void destroy(nmedia_worker *w){nmedia_ff_free(w->payload);nmedia_ff_free(w->video_pixels);nmedia_ff_free(w->tx);nmedia_ff_free(w);}
static void pipes_close(nmedia_worker *w) {
    if(w->in>=0)close(w->in);if(w->out>=0)close(w->out);if(w->video_fd>=0)close(w->video_fd);w->in=w->out=w->video_fd=-1;
}
static void stop(nmedia_worker *w,const char *error) {
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;
    w->stopping=true;w->waiting=w->seeking=w->want_seek=w->output_ready=w->video_taken=w->video_ready=w->video_loan=false;
    pipes_close(w);if(w->pid>0)kill(w->pid);
    nmedia_ff_free(w->tx);w->tx=NULL;w->tx_len=w->tx_pos=0;
}
static bool reap(nmedia_worker *w) {
    if(w->pid>0) {
        int got=waitpid(w->pid,NULL,WNOHANG);
        if(got!=w->pid&&!(got<0&&errno==ECHILD))return false;
        w->pid=0;
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
    nmedia_ff_free(w->tx);w->tx=NULL;w->tx_len=sizeof w->command;w->tx_pos=0;
    w->head_pos=w->rx_pos=0;memset(&w->response,0,sizeof w->response);
    w->waiting=true;w->output_ready=w->video_taken=false;w->request_at=uptime_ms();w->stats.requests++;
}
static bool spawn_child(nmedia_worker *w) {
    if(!nmedia_alloc_reserve_worker()){stop(w,"media worker aggregate reservation denied");return false;}
    w->reserved=true;
    int ip[2]={-1,-1},op[2]={-1,-1},vp[2]={-1,-1};
    if(pipe(ip)<0||pipe(op)<0||pipe(vp)<0)goto fail;
    int map[3]={ip[0],op[1],vp[1]};char *argv[]={"browsermediaworker",NULL};
    w->pid=spawn("/bin/browsermediaworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(vp[1]);vp[1]=-1;
    if(w->pid<0){w->pid=0;goto fail;}
    w->in=ip[1];ip[1]=-1;w->out=op[0];op[0]=-1;w->video_fd=vp[0];vp[0]=-1;
    if(fcntl(w->in,F_SETFL,O_NONBLOCK)<0||fcntl(w->out,F_SETFL,O_NONBLOCK)<0||fcntl(w->video_fd,F_SETFL,O_NONBLOCK)<0){stop(w,"media worker nonblocking pipe setup failed");return false;}
    return true;
fail:
    for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);if(vp[i]>=0)close(vp[i]);}
    stop(w,"cannot spawn native media worker");reap(w);return false;
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
    w->in=w->out=w->video_fd=-1;w->epoch=1;w->generation=generation;w->info.duration_ms=-1;queue(w,NMW_OPEN,0);
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
static void control_io(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool dispatch,bool allow_spawn) {
    nmedia_worker_background(now);
    if(!w||!b||w->stopping||!w->waiting)return;
    /* OPEN can legitimately span several HTTP ranges/probe steps, or wait
       for cooperative scheduling. Do not turn their total wall time into a
       browser-invented fatal error. Child/HTTP errors, pipe EOF and explicit
       cancellation still terminate this owner; each burst remains bounded. */
    if(dispatch&&uptime_ms()>=b->until)return;
    if(!w->pid) {if(!dispatch||!allow_spawn)return;if(!spawn_child(w))return;}
    if(!dispatch&&w->tx_pos<w->tx_len)return;
    struct n_pollfd p[2]={{w->in,dispatch?N_POLLOUT:0,0},{w->out,N_POLLIN,0}};
    if(poll(p,2,0)<0){stop(w,"native media worker poll failed");return;}
    while(dispatch&&w->tx_pos<w->tx_len&&b->tx_bytes&&(p[0].revents&N_POLLOUT)) {
        size_t take=MIN(w->tx_len-w->tx_pos,b->tx_bytes);
        const unsigned char *command=w->tx?w->tx:(const unsigned char *)&w->command;
        ssize_t n=write(w->in,command+w->tx_pos,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){stop(w,"native media worker command pipe failed");return;}
        w->tx_pos+=(size_t)n;b->tx_bytes-=(size_t)n;w->stats.tx_bytes+=(size_t)n;
    }
    /* Initial revents predate STEP dispatch. Collect a fast peer's response
     * now instead of inserting an entire author-JS/browser pass. */
    if(dispatch&&w->tx_pos==w->tx_len&&!(p[1].revents&(N_POLLIN|N_POLLHUP))){
        if(poll(&p[1],1,0)<0){stop(w,"native media worker response poll failed");return;}
    }
    /* A video frame used to drain at most one 16 KiB pipe window per 10 ms
     * GUI tick. Wake the blocked writer cooperatively instead of multiplying
     * that timer delay by every window. Never wait for network/decode: only
     * yield after receiving any response bytes, bounded by time and
     * handoffs. The child writes header and body separately: header-only
     * progress also means a producer may need a wake before its first pixel. */
    bool received=false;
    while(w->waiting&&(p[1].revents&(N_POLLIN|N_POLLHUP))) {
        /* Real available bytes, not a fixed fraction of a video frame. An
           expired collect gets one nonblocking wire component, no new time
           allowance; a continuously writing peer cannot occupy GUI forever. */
        if(received&&uptime_ms()>=b->until)break;
        received=true;
        bool head=w->head_pos<sizeof w->response;
        size_t take=head?sizeof w->response-w->head_pos:w->response.payload_bytes-w->rx_pos;
        if(!take){completed(w);break;}
        void *dest=head?(unsigned char *)&w->response+w->head_pos:w->payload+w->rx_pos;
        ssize_t n=read(w->out,dest,take);
        if(n<0&&errno==EAGAIN&&dispatch&&(w->head_pos||w->rx_pos)&&b->handoffs<16&&uptime_ms()<b->until) {
            if(!w->rx_pos)w->stats.header_waits++;
            b->handoffs++;w->stats.handoffs++;yield();
            if(uptime_ms()>=b->until)break;
            continue;
        }
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){stop(w,"native media worker response ended early");return;}
        w->stats.rx_bytes+=(size_t)n;
        if(head) {
            w->head_pos+=(size_t)n;
            if(w->head_pos==sizeof w->response) {
                if(!valid_response(w)){stop(w,"invalid native media worker response");return;}
                if(w->response.payload_bytes>w->capacity) {
                    /* A new response invalidates the old borrowed output.
                     * No bytes survive: avoid realloc's old+new copy peak. */
                    nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
                    unsigned char *b=nmedia_ff_malloc(w->response.payload_bytes);
                    if(!b){stop(w,"native media worker frame aggregate limit");return;}
                    w->payload=b;w->capacity=w->response.payload_bytes;
                }
            }
        } else w->rx_pos+=(size_t)n;
        if(w->head_pos==sizeof w->response&&w->rx_pos==w->response.payload_bytes){completed(w);break;}
    }
}
static void video_release(nmedia_worker *w){w->video_ready=w->video_loan=false;w->video_head_pos=w->video_rx_pos=0;}
static void video_io(nmedia_worker *w,struct nmedia_worker_budget *b,bool dispatch){
    if(w->video_fd<0||w->video_ready||w->video_loan)return;
    struct n_pollfd p={w->video_fd,N_POLLIN,0};if(poll(&p,1,0)<0){stop(w,"Range VIDEO poll failed");return;}
    bool received=false;
    while(p.revents&(N_POLLIN|N_POLLHUP)){
        if(received&&uptime_ms()>=b->until)break;received=true;
        bool header=w->video_head_pos<sizeof w->video_header;
        size_t take=header?sizeof w->video_header-w->video_head_pos:w->video_header.bytes-w->video_rx_pos;
        void *dest=header?(uint8_t *)&w->video_header+w->video_head_pos:w->video_pixels+w->video_rx_pos;
        ssize_t n=read(w->video_fd,dest,take);
        if(n<0&&errno==EAGAIN&&dispatch&&(w->video_head_pos||w->video_rx_pos)&&uptime_ms()<b->until){b->handoffs++;w->stats.handoffs++;yield();continue;}
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){stop(w,"Range VIDEO pipe ended early");return;}
        w->stats.rx_bytes+=(size_t)n;
        if(header){w->video_head_pos+=(size_t)n;if(w->video_head_pos==sizeof w->video_header){
            const struct nmedia_worker_video *v=&w->video_header;uint32_t bytes;
            if(v->magic!=NMEDIA_WORKER_MAGIC||v->generation!=w->generation||!v->sequence||v->sequence<=w->video_sequence||v->sequence>w->sequence||v->epoch>w->epoch||
               !nmedia_video_wire_bytes(v->width,v->height,&bytes)||bytes!=v->bytes){stop(w,"invalid Range VIDEO wire");return;}
            if(bytes>w->video_capacity){nmedia_ff_free(w->video_pixels);w->video_pixels=NULL;w->video_capacity=0;
                w->video_pixels=nmedia_ff_malloc(bytes);if(!w->video_pixels){stop(w,"Range VIDEO allocation failed");return;}w->video_capacity=bytes;}
        }}else w->video_rx_pos+=(size_t)n;
        if(w->video_head_pos==sizeof w->video_header&&w->video_rx_pos==w->video_header.bytes){w->video_sequence=w->video_header.sequence;w->video_ready=true;break;}
    }
}
static void pump_io(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool dispatch,bool allow_spawn){
    if(!w||!b||w->stopping)return;
    control_io(w,now,b,dispatch,allow_spawn);if(w->stopping)return;
    /* Do not starve delivered VIDEO behind repeated PCM/control progress.
       Only its initial nonblocking read can follow the elapsed boundary;
       subsequent reads/yields still use the same caller-owned budget. */
    video_io(w,b,dispatch);
}
void nmedia_worker_snapshot(const nmedia_worker *w,struct nmedia_worker_stats *out){
    if(!out)return;memset(out,0,sizeof *out);if(!w)return;*out=w->stats;
    out->header_bytes=w->head_pos;out->payload_bytes=w->rx_pos;out->payload_total=w->response.payload_bytes;out->waiting=w->waiting;
}
void nmedia_worker_pump_budget(nmedia_worker *w,uint64_t now,struct nmedia_worker_budget *b,bool allow_spawn){pump_io(w,now,b,true,allow_spawn);}
void nmedia_worker_collect(nmedia_worker *w,struct nmedia_worker_budget *b){pump_io(w,uptime_ms(),b,false,false);}
void nmedia_worker_pump(nmedia_worker *w,uint64_t now){
    struct nmedia_worker_budget b={.until=uptime_ms()+2,.tx_bytes=65536};
    nmedia_worker_pump_budget(w,now,&b,true);
}
bool nmedia_worker_running(const nmedia_worker *w){return w&&w->pid>0&&!w->stopping;}
bool nmedia_worker_pending(const nmedia_worker *w){return w&&!w->stopping&&(w->waiting||w->video_head_pos||w->video_expected_sequence>w->video_sequence);}
void nmedia_worker_prefetch(nmedia_worker *w,struct nmedia_worker_budget *b){
    if(w&&w->video_loan)video_release(w);
    if(!w||!b||w->stopping||w->waiting||w->output_ready||w->seeking||w->want_seek)return;
    if(w->response.kind!=NMEDIA_AUDIO&&w->response.kind!=NMW_VIDEO_PENDING)return;
    /* Caller has released the borrowed span. Queue at most one successor;
     * transport uses the same byte/time/handoff budget and cannot spawn. */
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
        *o=(struct nmedia_output){.kind=NMEDIA_VIDEO,.pts_ms=v->pts,.width=v->width,.height=v->height,.pixels=(const uint32_t *)w->video_pixels};return NMEDIA_VIDEO;
    }
    if(!w->output_ready)return NMEDIA_AGAIN;
    struct nmedia_worker_response *r=&w->response;
    if(r->kind==NMEDIA_END&&(w->video_head_pos||w->video_ready||w->video_expected_sequence>w->video_sequence))return NMEDIA_AGAIN;
    w->output_ready=false;
    if(r->kind==NMW_VIDEO_PENDING){memset(o,0,sizeof *o);return NMEDIA_AGAIN;}
    *o=(struct nmedia_output){.kind=r->kind,.pts_ms=r->pts_ms,.frames=r->frames,.samples=(const int16_t *)w->payload};return r->kind;
}
bool nmedia_worker_move_video(nmedia_worker *w,const struct nmedia_output *o,uint32_t **pixels,size_t *capacity){
    uint32_t bytes;
    if(!w||!o||!pixels||!capacity||w->stopping||w->seeking||w->want_seek||!w->video_loan||w->video_header.epoch!=w->epoch||
       o->kind!=NMEDIA_VIDEO||o->pixels!=(const uint32_t *)w->video_pixels||o->width!=w->video_header.width||o->height!=w->video_header.height||o->pts_ms!=w->video_header.pts||
       !nmedia_video_wire_bytes(o->width,o->height,&bytes)||bytes!=w->video_header.bytes||w->video_rx_pos!=bytes||w->video_capacity<bytes||
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
    for(nmedia_worker *w=live;w;w=w->next){if(nmedia_worker_pending(w))return (int64_t)now+1;if(w->stopping&&w->pid>0)stopping=true;}
    return stopping?(int64_t)now+10:-1;
}
