/* HTTP/CORS, probing, decode and seeking belong to this isolated native child.
 * No audio device or GUI here. Parent applies timing and owns presentation.
 * It is launched with no input, output or log: handle 3 is the control
 * channel (one command in, one response out), handle 4 the VIDEO channel
 * (frames out, one credit message in per frame the parent can take). */
#include "media_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#define CONTROL 3
#define VIDEO 4
enum { WAKE_CONTROL = 1, WAKE_VIDEO };
static n_handle events = -1;
/* A frame message is the pixels followed by its header: the decoder's own
 * buffer is sent as is, once it has room for the trailer. Converted pixels
 * are detached from the decoder before any subsequent command. */
struct video_tx {
    struct nmedia_worker_video header;
    uint32_t *pixels;size_t capacity;
    unsigned credits;
    bool pending;
};
static bool video_send(struct video_tx *v){
    if(!v->pending||!v->credits)return true;
    /* A credit means the parent has taken every earlier frame: the queue is
       empty, so the send is accepted at once whatever the size. */
    memcpy((uint8_t *)v->pixels+v->header.bytes,&v->header,sizeof v->header);
    if(n_channel_send(VIDEO,v->pixels,(size_t)v->header.bytes+sizeof v->header,-1)<0)return false;
    v->credits--;v->pending=false;return true;
}
static bool credits_take(struct video_tx *v){
    for(;;){
        uint8_t credit;n_result r=n_channel_receive(VIDEO,&credit,sizeof credit,0,NULL);
        if(r==-N_ERR_NOT_READY)return true;
        if(r!=1)return false;
        v->credits++;
    }
}
/* The next command, sending a held frame as soon as a credit allows. */
static bool command_next(struct nmedia_worker_command *c,struct video_tx *v){
    for(;;){
        if(!credits_take(v)||!video_send(v))return false;
        n_result r=n_channel_receive(CONTROL,c,sizeof *c,0,NULL);
        if(r==(n_result)sizeof *c)return true;
        if(r!=-N_ERR_NOT_READY)return false;
        struct n_event e[2];
        if(n_events_wait(events,e,2,-1)<0)return false;
    }
}
static bool respond(const struct nmedia_worker_response *r,const void *payload){
    static uint8_t out[sizeof(struct nmedia_worker_response)+4096*4];
    if(r->payload_bytes>sizeof out-sizeof *r)return false;
    memcpy(out,r,sizeof *r);if(r->payload_bytes)memcpy(out+sizeof *r,payload,r->payload_bytes);
    return n_channel_send(CONTROL,out,sizeof *r+r->payload_bytes,-1)>=0;
}

int main(void) {
    if(!nmedia_alloc_restrict_worker())return 1;
    events=n_events_create();
    if(events<0||n_events_watch(events,CONTROL,N_EVENT_RECEIVABLE,WAKE_CONTROL)<0||
       n_events_watch(events,VIDEO,N_EVENT_RECEIVABLE,WAKE_VIDEO)<0)return 1;
    /* OPEN: the command, then exactly url_bytes and document_bytes, no NULs. */
    n_result size=n_channel_receive(CONTROL,NULL,0,N_CHANNEL_WAIT|N_CHANNEL_PEEK,NULL);
    if(size<(n_result)sizeof(struct nmedia_worker_command)||(uint64_t)size>(uint64_t)UINT32_MAX*2)return 1;
    uint8_t *open=nmedia_ff_malloc((size_t)size+2);
    if(!open||n_channel_receive(CONTROL,open,(size_t)size,0,NULL)!=size)return 1;
    struct nmedia_worker_command c;memcpy(&c,open,sizeof c);
    if(c.magic!=NMEDIA_WORKER_MAGIC||c.operation!=NMW_OPEN||!c.url_bytes||!c.document_bytes||!c.epoch||
       (uint64_t)size!=sizeof c+(uint64_t)c.url_bytes+c.document_bytes)return 1;
    uint32_t generation=c.generation,sequence=c.sequence;uint64_t epoch=c.epoch;
    /* The NUL is private storage, not an extra wire byte. */
    char *url=(char *)open+sizeof c,*document=url+c.url_bytes+1;
    memmove(document,url+c.url_bytes,c.document_bytes);url[c.url_bytes]=0;document[c.document_bytes]=0;
    if(strlen(url)!=c.url_bytes||strlen(document)!=c.document_bytes)return 1;
    struct video_tx video={0};struct nmedia_output pending={0};
    char error[160]={0};
    nmedia *m=nmedia_open_url_cors(url,document,error,sizeof error);
    nmedia_ff_free(open);
    for(;;) {
        struct nmedia_worker_response r={.magic=NMEDIA_WORKER_MAGIC,.operation=c.operation,
            .generation=generation,.sequence=sequence,.kind=NMEDIA_AGAIN};
        struct nmedia_output o={0};const void *payload=NULL;
        if(!m){r.kind=NMEDIA_ERROR;strlcpy(r.error,error,sizeof r.error);}
        else {
            const struct nmedia_info *info=nmedia_get_info(m);
            r.audio=info->audio;r.video=info->video;r.channels=info->channels;r.sample_rate=info->sample_rate;
            r.width=info->width;r.height=info->height;r.duration_ms=info->duration_ms;
            strlcpy(r.audio_codec,info->audio_codec,sizeof r.audio_codec);
            strlcpy(r.video_codec,info->video_codec,sizeof r.video_codec);strlcpy(r.container,info->container,sizeof r.container);
            if(c.operation==NMW_SEEK) {
                pending.kind=0;
                if(!nmedia_seek(m,c.seek_ms)){r.kind=NMEDIA_ERROR;strlcpy(r.error,nmedia_error(m),sizeof r.error);if(!r.error[0])strlcpy(r.error,"native worker seek failed",sizeof r.error);}
            } else if(c.operation==NMW_STEP) {
                if(!pending.kind)pending.kind=nmedia_step(m,&pending);
                o=pending;r.kind=o.kind;r.pts_ms=o.pts_ms;r.audio_observation=o.audio_observation;
                /* The first image is detached. Continue muxed PCM while it
                   waits for a credit; hold a second decoder image without overwriting it. */
                if((r.kind==NMEDIA_VIDEO||r.kind==NMEDIA_END)&&video.pending)r.kind=NMEDIA_AGAIN;
                else if(r.kind==NMEDIA_AUDIO){r.channels=2;r.sample_rate=SOUND_RATE;r.frames=(uint32_t)o.frames;r.payload_bytes=r.frames*4;payload=o.samples;pending.kind=0;}
                else if(r.kind==NMEDIA_VIDEO){r.width=o.width;r.height=o.height;uint32_t bytes;
                    size_t trailer=(sizeof video.header+sizeof(uint32_t)-1)/sizeof(uint32_t);
                    bool moved=nmedia_video_wire_bytes(o.width,o.height,&bytes)&&nmedia_move_video(m,&o,&video.pixels,&video.capacity);
                    /* Room for the header after the pixels; buffers keep it as they circulate. */
                    if(moved&&video.capacity<bytes/sizeof(uint32_t)+trailer){
                        uint32_t *grown=nmedia_ff_realloc(video.pixels,((size_t)bytes/sizeof(uint32_t)+trailer)*sizeof(uint32_t));
                        if(grown){video.pixels=grown;video.capacity=bytes/sizeof(uint32_t)+trailer;}else moved=false;
                    }
                    if(!moved){
                        r.kind=NMEDIA_ERROR;strlcpy(r.error,"Range VIDEO owner/size transfer failed",sizeof r.error);
                    }else{video.header=(struct nmedia_worker_video){.magic=NMEDIA_WORKER_MAGIC,.generation=generation,.sequence=sequence,
                        .bytes=bytes,.epoch=epoch,.pts=o.pts_ms,.width=o.width,.height=o.height,
                        .video_measured_frames=o.video_measured_frames,.video_work_ms=o.video_work_ms,
                        .video_measurement_id=o.video_measurement_id,.video_configuration=o.video_configuration};
                        video.pending=true;pending.kind=0;r.kind=NMW_VIDEO_PENDING;}
                }else if(r.kind==NMEDIA_ERROR)strlcpy(r.error,nmedia_error(m),sizeof r.error);
                else if(r.kind==NMEDIA_AGAIN)pending.kind=0;
            }
        }
        if(!respond(&r,payload))break;
        if(r.kind==NMEDIA_ERROR)break;
        if(!command_next(&c,&video))break;
        if(c.magic!=NMEDIA_WORKER_MAGIC||c.generation!=generation||
           c.sequence!=sequence+1||c.url_bytes||c.document_bytes||
           (c.operation!=NMW_STEP&&c.operation!=NMW_SEEK)||c.seek_ms<0||c.seek_ms>1000000000000LL||
           (c.operation==NMW_SEEK?c.epoch<=epoch:c.epoch!=epoch))break;
        sequence=c.sequence;epoch=c.epoch;
    }
    nmedia_close(m);nmedia_ff_free(video.pixels);return 0;
}
