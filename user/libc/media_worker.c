/* One live Browser Range child per process. All GUI-side IO is nonblocking.
 * Retired state deliberately contains no document/node/JS pointers. */
#include "media_worker_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
struct nmedia_worker {
    int pid,in,out;
    uint32_t generation,sequence;
    bool reserved,stopping,waiting,metadata,output_ready,seeking,want_seek;
    int64_t seek_ms;
    uint64_t deadline;
    struct nmedia_info info;
    struct nmedia_worker_command command;
    struct nmedia_worker_response response;
    unsigned char tx[sizeof(struct nmedia_worker_command)+2*NMEDIA_WORKER_URL];
    size_t tx_len,tx_pos,head_pos,rx_pos,capacity;
    unsigned char *payload;
    char error[160];
};
static nmedia_worker *owner,*retired;
static void pipes_close(nmedia_worker *w) {
    if(w->in>=0)close(w->in);if(w->out>=0)close(w->out);w->in=w->out=-1;
}
static void stop(nmedia_worker *w,const char *error) {
    if(error&&!w->error[0])strlcpy(w->error,error,sizeof w->error);
    if(w->stopping)return;
    w->stopping=true;w->waiting=w->seeking=w->want_seek=w->output_ready=false;
    pipes_close(w);if(w->pid>0)kill(w->pid);
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
    if(retired&&reap(retired)){nmedia_ff_free(retired->payload);nmedia_ff_free(retired);retired=NULL;}
    if(owner&&owner->stopping)reap(owner);
}
static void queue(nmedia_worker *w,unsigned op,int64_t seek) {
    w->command=(struct nmedia_worker_command){.magic=NMEDIA_WORKER_MAGIC,.operation=op,
        .generation=w->generation,.sequence=++w->sequence,.seek_ms=seek};
    memcpy(w->tx,&w->command,sizeof w->command);w->tx_len=sizeof w->command;w->tx_pos=0;
    w->head_pos=w->rx_pos=0;memset(&w->response,0,sizeof w->response);
    w->waiting=true;w->output_ready=false;w->deadline=uptime_ms()+NMEDIA_WORKER_DEADLINE;
}
static bool spawn_child(nmedia_worker *w) {
    if(!nmedia_alloc_reserve_worker()){stop(w,"media worker aggregate reservation denied");return false;}
    w->reserved=true;
    int ip[2]={-1,-1},op[2]={-1,-1},nullfd=-1;
    if(pipe(ip)<0||pipe(op)<0||(nullfd=open("/dev/null",O_WRONLY))<0)goto fail;
    int map[3]={ip[0],op[1],nullfd};char *argv[]={"browsermediaworker",NULL};
    w->pid=spawn("/bin/browsermediaworker",argv,map,0);
    close(ip[0]);ip[0]=-1;close(op[1]);op[1]=-1;close(nullfd);nullfd=-1;
    if(w->pid<0){w->pid=0;goto fail;}
    w->in=ip[1];ip[1]=-1;w->out=op[0];op[0]=-1;
    if(fcntl(w->in,F_SETFL,O_NONBLOCK)<0||fcntl(w->out,F_SETFL,O_NONBLOCK)<0){stop(w,"media worker nonblocking pipe setup failed");return false;}
    return true;
fail:
    for(int i=0;i<2;i++){if(ip[i]>=0)close(ip[i]);if(op[i]>=0)close(op[i]);}if(nullfd>=0)close(nullfd);
    stop(w,"cannot spawn native media worker");reap(w);return false;
}
nmedia_worker *nmedia_worker_open(const char *url,const char *document,uint32_t generation,char *error,size_t size) {
    nmedia_worker_background(uptime_ms());
    if(owner||!url||!document||!url[0]||!document[0]||strlen(url)>=NMEDIA_WORKER_URL||strlen(document)>=NMEDIA_WORKER_URL) {
        if(error&&size)snprintf(error,size,"one native Range worker allowed per browser process");return NULL;
    }
    nmedia_worker *w=nmedia_ff_mallocz(sizeof *w);
    if(!w){if(error&&size)snprintf(error,size,"native media worker control allocation");return NULL;}
    w->in=w->out=-1;w->generation=generation;w->info.duration_ms=-1;queue(w,NMW_OPEN,0);
    w->command.url_bytes=(uint32_t)strlen(url);w->command.document_bytes=(uint32_t)strlen(document);
    memcpy(w->tx,&w->command,sizeof w->command);
    memcpy(w->tx+w->tx_len,url,w->command.url_bytes);w->tx_len+=w->command.url_bytes;
    memcpy(w->tx+w->tx_len,document,w->command.document_bytes);w->tx_len+=w->command.document_bytes;
    owner=w; /* A cancelled child may be awaiting reap; defer spawn, not IO. */
    if(error&&size)*error=0;return w;
}
static bool valid_response(nmedia_worker *w) {
    struct nmedia_worker_response *r=&w->response;
    if(r->magic!=NMEDIA_WORKER_MAGIC||r->operation!=w->command.operation||r->generation!=w->generation||r->sequence!=w->sequence||
       r->reserved||r->audio>1||r->video>1||r->channels>8||r->sample_rate>384000||r->duration_ms< -1||
       !memchr(r->error,0,sizeof r->error)||!memchr(r->audio_codec,0,32)||!memchr(r->video_codec,0,32)||!memchr(r->container,0,32))return false;
    if(r->operation==NMW_OPEN&&(r->width<0||r->height<0||(uint64_t)r->width*r->height>NMEDIA_MAX_PIXELS))return false;
    if(r->operation!=NMW_STEP&&r->kind!=NMEDIA_AGAIN&&r->kind!=NMEDIA_ERROR)return false;
    if(r->kind==NMEDIA_AUDIO)return r->operation==NMW_STEP&&r->audio&&r->channels==2&&r->sample_rate==SOUND_RATE&&
        r->frames>0&&r->frames<=4096&&r->payload_bytes==r->frames*4;
    if(r->kind==NMEDIA_VIDEO)return r->operation==NMW_STEP&&r->width>0&&r->height>0&&
        (uint64_t)r->width*r->height<=NMEDIA_MAX_PIXELS&&r->payload_bytes==(uint64_t)r->width*r->height*4;
    return (r->kind==NMEDIA_AGAIN||r->kind==NMEDIA_END||r->kind==NMEDIA_ERROR)&&!r->payload_bytes;
}
static void completed(nmedia_worker *w) {
    struct nmedia_worker_response *r=&w->response;
    w->waiting=false;
    if(r->kind==NMEDIA_ERROR){stop(w,r->error[0]?r->error:"native media worker failed");return;}
    if(r->operation==NMW_OPEN) {
        w->info=(struct nmedia_info){.audio=r->audio,.video=r->video,.channels=(int)r->channels,.sample_rate=(int)r->sample_rate,
            .width=r->width,.height=r->height,.duration_ms=r->duration_ms};
        strlcpy(w->info.audio_codec,r->audio_codec,sizeof w->info.audio_codec);
        strlcpy(w->info.video_codec,r->video_codec,sizeof w->info.video_codec);strlcpy(w->info.container,r->container,sizeof w->info.container);
        w->metadata=true;
    } else if(r->operation==NMW_SEEK&&!w->want_seek)w->seeking=false;
    else if(r->operation==NMW_STEP&&!w->want_seek){w->output_ready=true;if(r->kind==NMEDIA_VIDEO){w->info.width=r->width;w->info.height=r->height;}}
    if(w->want_seek){w->want_seek=false;queue(w,NMW_SEEK,w->seek_ms);}
}
void nmedia_worker_pump(nmedia_worker *w,uint64_t now) {
    nmedia_worker_background(now);
    if(!w||w->stopping||!w->waiting)return;
    if(now>=w->deadline){stop(w,"native media worker request deadline exceeded");return;}
    if(!w->pid) {if(retired)return;if(!spawn_child(w))return;}
    struct n_pollfd p[2]={{w->in,N_POLLOUT,0},{w->out,N_POLLIN,0}};
    if(poll(p,2,0)<0){stop(w,"native media worker poll failed");return;}
    size_t budget=65536;
    while(w->tx_pos<w->tx_len&&budget&&(p[0].revents&N_POLLOUT)) {
        size_t take=MIN(w->tx_len-w->tx_pos,budget);ssize_t n=write(w->in,w->tx+w->tx_pos,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){stop(w,"native media worker command pipe failed");return;}
        w->tx_pos+=(size_t)n;budget-=(size_t)n;
    }
    budget=262144;
    while(w->waiting&&budget&&(p[1].revents&(N_POLLIN|N_POLLHUP))) {
        bool head=w->head_pos<sizeof w->response;
        size_t take=head?sizeof w->response-w->head_pos:w->response.payload_bytes-w->rx_pos;
        if(!take){completed(w);break;}
        take=MIN(take,budget);
        void *dest=head?(unsigned char *)&w->response+w->head_pos:w->payload+w->rx_pos;
        ssize_t n=read(w->out,dest,take);
        if(n<0&&(errno==EAGAIN||errno==EINTR))break;
        if(n<=0){stop(w,"native media worker response ended early");return;}
        budget-=(size_t)n;
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
const struct nmedia_info *nmedia_worker_info(nmedia_worker *w){return w&&w->metadata?&w->info:NULL;}
const char *nmedia_worker_error(nmedia_worker *w){return w?w->error:"no native worker";}
bool nmedia_worker_loading(nmedia_worker *w){return w&&!w->metadata&&!w->error[0];}
bool nmedia_worker_seeking(nmedia_worker *w){return w&&w->seeking&&!w->error[0];}
bool nmedia_worker_seek(nmedia_worker *w,int64_t ms) {
    if(!w||!w->metadata||w->error[0]||ms<0||ms>1000000000000LL)return false;
    w->seeking=true;w->seek_ms=ms;w->output_ready=false;
    if(w->waiting)w->want_seek=true;else queue(w,NMW_SEEK,ms);
    return true;
}
void nmedia_worker_step(nmedia_worker *w) {
    if(w&&w->metadata&&!w->stopping&&!w->waiting&&!w->seeking&&!w->output_ready)queue(w,NMW_STEP,0);
}
int nmedia_worker_take(nmedia_worker *w,struct nmedia_output *o) {
    if(!w||w->error[0])return NMEDIA_ERROR;
    if(!w->output_ready)return NMEDIA_AGAIN;
    w->output_ready=false;struct nmedia_worker_response *r=&w->response;
    *o=(struct nmedia_output){.kind=r->kind,.pts_ms=r->pts_ms,.width=r->width,.height=r->height,
        .frames=r->frames,.samples=(const int16_t *)w->payload,.pixels=(const uint32_t *)w->payload};
    return r->kind;
}
void nmedia_worker_close(nmedia_worker *w) {
    if(!w)return;if(owner==w)owner=NULL;
    stop(w,NULL);nmedia_ff_free(w->payload);w->payload=NULL;w->capacity=0;
    if(reap(w))nmedia_ff_free(w);
    else retired=w; /* One child maximum; pending replacement has no PID. */
}
int64_t nmedia_worker_deadline(uint64_t now) {return retired||(owner&&(owner->waiting||(owner->stopping&&owner->pid>0)))?(int64_t)now+10:-1;}
