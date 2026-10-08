/* Nonblocking GUI-side transport for the native two-buffer MSE child. */
#include "media_mse_worker_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct request {struct request *next;struct nmsw_command c;uint8_t *bytes;uint64_t epoch[2];};
struct nmedia_mse_worker {
    int pid,in,out;uint32_t generation,sequence;
    bool reserved,stopping,output_ready,quota_error,starving;
    struct request *head,*tail;
    size_t queued_bytes,queued_count,tx_pos,head_pos,rx_pos,capacity;
    uint8_t *payload;uint64_t deadline;int64_t current;
    struct nmsw_response response;
    struct nmedia_output output;unsigned output_slot;uint64_t output_revision;
    uint64_t revision[2];uint32_t parsing[2];int64_t group_end[2];
    uint64_t epoch[2];
    struct nmedia_info info[2];bool metadata[2];
    uint32_t quota[2],counts[2];struct nmedia_time_range ranges[2][64];
    char error[160];
};
static nmedia_mse_worker *owner,*retired;
static void clear_queue(nmedia_mse_worker *w){while(w->head){struct request *r=w->head;w->head=r->next;nmedia_ff_free(r->bytes);nmedia_ff_free(r);}w->tail=NULL;w->queued_bytes=w->queued_count=0;}
static void stop(nmedia_mse_worker *w,const char *error){
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;w->stopping=true;w->output_ready=false;
    if(w->in>=0)close(w->in);if(w->out>=0)close(w->out);w->in=w->out=-1;if(w->pid>0)kill(w->pid);clear_queue(w);
}
static bool reap(nmedia_mse_worker *w){
    if(w->pid>0){int got=waitpid(w->pid,NULL,WNOHANG);if(got!=w->pid&&!(got<0&&errno==ECHILD))return false;w->pid=0;}
    if(w->reserved){nmedia_alloc_release_worker();w->reserved=false;}return true;
}
void nmedia_mse_worker_background(uint64_t now){
    (void)now;if(retired&&reap(retired)){nmedia_ff_free(retired->payload);nmedia_ff_free(retired);retired=NULL;}
    if(owner&&owner->stopping)reap(owner);
}
static bool spawn_child(nmedia_mse_worker *w){
    if(!nmedia_alloc_reserve_worker()){stop(w,"MSE worker aggregate reservation denied");return false;}w->reserved=true;
    int ip[2]={-1,-1},op[2]={-1,-1},nullfd=-1;
    if(pipe(ip)<0||pipe(op)<0||(nullfd=open("/dev/null",O_WRONLY))<0)goto fail;
    int map[3]={ip[0],op[1],nullfd};char *argv[]={"browsermseworker",NULL};w->pid=spawn("/bin/browsermseworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(nullfd);nullfd=-1;if(w->pid<0){w->pid=0;goto fail;}
    w->in=ip[1];ip[1]=-1;w->out=op[0];op[0]=-1;
    if(fcntl(w->in,F_SETFL,O_NONBLOCK)<0||fcntl(w->out,F_SETFL,O_NONBLOCK)<0){stop(w,"MSE worker nonblocking pipe setup failed");return false;}return true;
fail:
    for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);}if(nullfd>=0)close(nullfd);stop(w,"cannot spawn native MSE worker");reap(w);return false;
}
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t generation,char *error,size_t size){
    nmedia_mse_worker_background(uptime_ms());if(owner){if(error&&size)strlcpy(error,"one native MSE worker per browser process",size);return NULL;}
    nmedia_mse_worker *w=nmedia_ff_mallocz(sizeof *w);if(!w){if(error&&size)strlcpy(error,"MSE worker control quota",size);return NULL;}
    w->in=w->out=-1;w->generation=generation;w->quota[0]=w->quota[1]=NMEDIA_MAX_BYTES;owner=w;if(error&&size)*error=0;return w;
}
bool nmedia_mse_worker_command(nmedia_mse_worker *w,unsigned op,unsigned slot,const void *bytes,size_t size,int64_t offset,int64_t start,int64_t end,bool flag){
    if(!w||w->stopping||slot>1||op<NMSW_ADD||op>NMSW_SELECT||size>NMEDIA_MAX_BYTES||(!bytes&&size)||w->queued_count>=16||size>NMEDIA_MAX_BYTES-w->queued_bytes)return false;
    struct request *r=nmedia_ff_mallocz(sizeof *r);if(!r)return false;
    if(size){r->bytes=nmedia_ff_malloc(size);if(!r->bytes){nmedia_ff_free(r);return false;}memcpy(r->bytes,bytes,size);}
    if(op==NMSW_ADD||op==NMSW_DROP){w->epoch[slot]++;w->metadata[slot]=false;w->counts[slot]=0;w->parsing[slot]=0;w->group_end[slot]=0;w->revision[slot]=0;w->quota[slot]=op==NMSW_ADD?NMEDIA_MAX_BYTES:0;}
    memcpy(r->epoch,w->epoch,sizeof r->epoch);
    r->c=(struct nmsw_command){.magic=NMSW_MAGIC,.op=op,.generation=w->generation,.slot=slot,.flags=flag,.bytes=(uint32_t)size,.offset=offset,.start=start,.end=end,.current=w->current};
    if(w->tail)w->tail->next=r;else w->head=r;w->tail=r;w->queued_bytes+=size;w->queued_count++;
    if(op!=NMSW_STEP){if(op==NMSW_ABORT){w->error[0]=0;w->quota_error=false;}}
    if(w->head==r){r->c.sequence=++w->sequence;w->tx_pos=w->head_pos=w->rx_pos=0;w->deadline=uptime_ms()+30000;memset(&w->response,0,sizeof w->response);}return true;
}
static bool valid(nmedia_mse_worker *w){
    const struct nmsw_response *r=&w->response;const struct nmsw_command *c=&w->head->c;
    if(r->magic!=NMSW_MAGIC||r->op!=c->op||r->generation!=w->generation||r->sequence!=c->sequence||r->reserved||r->slot<0||r->slot>1||r->quota_error>1||!memchr(r->error,0,sizeof r->error))return false;
    for(int i=0;i<2;i++){const struct nmsw_info *n=&r->info[i];
        if(n->duration< -1||n->duration>1000000000000LL||r->parsing[i]>1||r->group_end[i]<0||r->group_end[i]>1000001000000LL||r->quota[i]>NMEDIA_MAX_BYTES||r->counts[i]>64||n->audio>1||n->video>1||n->channels>8||n->sample_rate>384000||n->width<0||n->height<0||(uint64_t)n->width*n->height>NMEDIA_MAX_PIXELS||
           !memchr(n->audio_codec,0,32)||!memchr(n->video_codec,0,32)||!memchr(n->container,0,32))return false;
        for(unsigned j=0;j<r->counts[i];j++)if(r->ranges[i][j].start_ms<0||r->ranges[i][j].end_ms<=r->ranges[i][j].start_ms||r->ranges[i][j].end_ms>1000001000000LL||
           (j&&r->ranges[i][j-1].end_ms>r->ranges[i][j].start_ms))return false;
    }
    if(r->kind==NMEDIA_AUDIO)return r->op==NMSW_STEP&&r->frames>0&&r->frames<=4096&&r->payload_bytes==r->frames*4;
    if(r->kind==NMEDIA_VIDEO)return r->op==NMSW_STEP&&r->width>0&&r->height>0&&(uint64_t)r->width*r->height<=NMEDIA_MAX_PIXELS&&r->payload_bytes==(uint64_t)r->width*r->height*4;
    return (r->kind==NMEDIA_AGAIN||r->kind==NMEDIA_END||r->kind==NMEDIA_ERROR||r->kind==NMSW_BUSY||r->kind==NMSW_CLOCK)&&!r->payload_bytes;
}
static void completed(nmedia_mse_worker *w){
    struct nmsw_response *r=&w->response;
    for(int i=0;i<2;i++){if(w->head->epoch[i]!=w->epoch[i])continue;const struct nmsw_info *a=&r->info[i];w->metadata[i]=a->audio||a->video;
        w->info[i]=(struct nmedia_info){.audio=a->audio,.video=a->video,.channels=(int)a->channels,.sample_rate=(int)a->sample_rate,.width=a->width,.height=a->height,.duration_ms=a->duration};
        strlcpy(w->info[i].audio_codec,a->audio_codec,32);strlcpy(w->info[i].video_codec,a->video_codec,32);strlcpy(w->info[i].container,a->container,32);
        w->revision[i]=r->revision[i];w->parsing[i]=r->parsing[i];w->group_end[i]=r->group_end[i];w->quota[i]=r->quota[i];w->counts[i]=r->counts[i];memcpy(w->ranges[i],r->ranges[i],sizeof w->ranges[i]);}
    if(r->kind==NMEDIA_ERROR){strlcpy(w->error,r->error[0]?r->error:"native MSE worker failed",sizeof w->error);w->quota_error=r->quota_error;}
    else if(r->op==NMSW_ABORT||w->quota_error){w->error[0]=0;w->quota_error=false;if(w->output.kind==NMEDIA_ERROR)w->output_ready=false;}
    if(w->output_ready&&nmedia_mse_worker_revision(w,w->output_slot)!=w->output_revision)w->output_ready=false;
    bool data=r->kind==NMEDIA_AUDIO||r->kind==NMEDIA_VIDEO||r->kind==NMSW_CLOCK;
    bool current_epoch=data?w->head->epoch[r->slot]==w->epoch[r->slot]:w->head->epoch[0]==w->epoch[0]&&w->head->epoch[1]==w->epoch[1];
    if(r->op==NMSW_STEP&&current_epoch){w->output_ready=true;w->output_slot=(unsigned)r->slot;w->output_revision=nmedia_mse_worker_revision(w,w->output_slot);
        w->starving=r->kind==NMEDIA_AGAIN;
        w->output=(struct nmedia_output){.kind=r->kind,.pts_ms=r->pts,.frames=r->frames,.width=r->width,.height=r->height,.samples=(const int16_t *)w->payload,.pixels=(const uint32_t *)w->payload};}
    else if(r->kind!=NMEDIA_ERROR)w->starving=false;
    struct request *old=w->head;w->head=old->next;w->queued_bytes-=old->c.bytes;w->queued_count--;nmedia_ff_free(old->bytes);nmedia_ff_free(old);if(!w->head)w->tail=NULL;
    w->tx_pos=w->head_pos=w->rx_pos=0;
    if(w->head){w->head->c.sequence=++w->sequence;w->deadline=uptime_ms()+30000;memset(&w->response,0,sizeof w->response);}
}
void nmedia_mse_worker_pump(nmedia_mse_worker *w,uint64_t now){
    nmedia_mse_worker_background(now);if(!w||w->stopping||!w->head)return;
    if(now>=w->deadline){stop(w,"MSE worker request deadline exceeded");return;}
    if(!w->pid){if(retired)return;if(!spawn_child(w))return;}
    struct n_pollfd p[2]={{w->in,N_POLLOUT,0},{w->out,N_POLLIN,0}};if(poll(p,2,0)<0){stop(w,"MSE worker poll failed");return;}
    size_t budget=65536,total=sizeof w->head->c+w->head->c.bytes;
    while(w->tx_pos<total&&budget&&(p[0].revents&N_POLLOUT)){
        bool header=w->tx_pos<sizeof w->head->c;const uint8_t *src=header?(const uint8_t *)&w->head->c+w->tx_pos:w->head->bytes+w->tx_pos-sizeof w->head->c;
        size_t take=MIN(budget,header?sizeof w->head->c-w->tx_pos:total-w->tx_pos);ssize_t n=write(w->in,src,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){stop(w,"MSE worker command pipe failed");return;}w->tx_pos+=(size_t)n;budget-=(size_t)n;
    }
    budget=262144;
    while(w->head&&budget&&(p[1].revents&(N_POLLIN|N_POLLHUP))){
        bool header=w->head_pos<sizeof w->response;size_t take=header?sizeof w->response-w->head_pos:w->response.payload_bytes-w->rx_pos;
        if(!take){completed(w);break;}take=MIN(take,budget);void *dest=header?(uint8_t *)&w->response+w->head_pos:w->payload+w->rx_pos;ssize_t n=read(w->out,dest,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;if(n<=0){stop(w,"MSE worker response ended early");return;}budget-=(size_t)n;
        if(header){w->head_pos+=(size_t)n;if(w->head_pos==sizeof w->response){if(!valid(w)){stop(w,"invalid native MSE worker response");return;}
            if(w->response.payload_bytes>w->capacity){uint8_t *a=nmedia_ff_realloc(w->payload,w->response.payload_bytes);if(!a){stop(w,"MSE worker frame aggregate quota");return;}w->payload=a;w->capacity=w->response.payload_bytes;}}}
        else w->rx_pos+=(size_t)n;
        if(w->head_pos==sizeof w->response&&w->rx_pos==w->response.payload_bytes){completed(w);break;}
    }
}
bool nmedia_mse_worker_pending(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op!=NMSW_STEP&&r->c.op!=NMSW_SEEK)return true;return false;}
bool nmedia_mse_worker_quota_error(const nmedia_mse_worker *w){return w&&w->quota_error;}
const char *nmedia_mse_worker_error(const nmedia_mse_worker *w){return w?w->error:"no MSE worker";}
const struct nmedia_info *nmedia_mse_worker_info(const nmedia_mse_worker *w,unsigned slot){return w&&slot<2&&w->metadata[slot]?&w->info[slot]:NULL;}
size_t nmedia_mse_worker_quota(const nmedia_mse_worker *w,unsigned slot){if(!w||slot>1)return 0;size_t available=w->quota[slot];for(struct request *r=w->head;r;r=r->next)if(r->c.slot==slot&&r->c.op==NMSW_APPEND&&r->epoch[slot]==w->epoch[slot])available-=MIN(available,(size_t)r->c.bytes);return available;}
size_t nmedia_mse_worker_ranges(const nmedia_mse_worker *w,unsigned slot,struct nmedia_time_range *out,size_t maximum){if(!w||slot>1)return 0;size_t n=MIN((size_t)w->counts[slot],maximum);memcpy(out,w->ranges[slot],n*sizeof *out);return n;}
int nmedia_mse_worker_step(nmedia_mse_worker *w,struct nmedia_output *out){
    if(!w||w->error[0])return NMEDIA_ERROR;
    if(w->output_ready){w->output_ready=false;*out=w->output;int kind=out->kind;if(kind==NMSW_BUSY)memset(out,0,sizeof *out);return kind;}
    if(w->starving&&!w->head)return NMEDIA_AGAIN;
    if(!w->head&&!nmedia_mse_worker_command(w,NMSW_STEP,0,NULL,0,0,0,0,false)){stop(w,"MSE STEP queue quota");return NMEDIA_ERROR;}return NMSW_BUSY;
}
void nmedia_mse_worker_close(nmedia_mse_worker *w){if(!w)return;if(owner==w)owner=NULL;stop(w,NULL);nmedia_ff_free(w->payload);w->payload=NULL;
    if(reap(w))nmedia_ff_free(w);else retired=w;}
int64_t nmedia_mse_worker_deadline(uint64_t now){return retired||(owner&&(owner->head||(owner->stopping&&owner->pid>0)))?(int64_t)now+10:-1;}

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *w){if(!w)return false;for(struct request *r=w->head;r;r=r->next)if(r->c.op==NMSW_SEEK)return true;return false;}
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *w,unsigned slot){return w&&slot<2?(w->epoch[slot]<<32)^w->revision[slot]:0;}
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *w){return w?w->output_slot:0;}
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *w,unsigned slot){return w&&slot<2&&w->parsing[slot];}
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *w,unsigned slot){return w&&slot<2?w->group_end[slot]:0;}
void nmedia_mse_worker_time(nmedia_mse_worker *w,int64_t ms){if(w)w->current=ms;}
