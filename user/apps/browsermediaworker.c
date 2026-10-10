/* HTTP/CORS, probing, decode and seeking belong to this isolated native child.
 * No audio device or GUI here. Parent applies timing and owns presentation. */
#include "media_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
static bool read_all(void *out,size_t n) {
    while(n){ssize_t k=read(0,out,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;out=(char *)out+k;n-=(size_t)k;}return true;
}
static bool write_all(const void *in,size_t n) {
    while(n){ssize_t k=write(1,in,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;in=(const char *)in+k;n-=(size_t)k;}return true;
}
/* Converted pixels are detached from the decoder before any subsequent
 * command. APPEND/seek/drop can therefore run while this owner is in flight. */
struct video_tx {
    struct nmedia_worker_video header;
    uint32_t *pixels;size_t capacity,position;
    bool pending;
};
static int video_fd=-1;
static bool video_send(struct video_tx *v){
    while(v->pending){
        bool header=v->position<sizeof v->header;
        size_t total=sizeof v->header+(size_t)v->header.bytes;
        const uint8_t *src=header?(const uint8_t *)&v->header+v->position:
            (const uint8_t *)v->pixels+v->position-sizeof v->header;
        size_t nbytes=header?sizeof v->header-v->position:total-v->position;
        ssize_t n=write(video_fd,src,nbytes);
        if(n<0&&errno==EINTR)continue;
        if(n<0&&errno==EAGAIN)return true;
        if(n<=0)return false;
        v->position+=(size_t)n;
        if(v->position==total){v->pending=false;v->position=0;}
    }return true;
}
static bool command_wait(struct video_tx *v){
    while(v->pending){
        struct n_pollfd p[2]={{0,N_POLLIN,0},{video_fd,N_POLLOUT,0}};
        if(poll(p,2,-1)<0){if(errno==EINTR)continue;return false;}
        /* Control/input demand wins over a large VIDEO write. */
        if(p[0].revents&N_POLLIN)return true;
        if(p[0].revents&N_POLLHUP)return false;
        if(p[1].revents&N_POLLHUP)return false;
        if((p[1].revents&N_POLLOUT)&&!video_send(v))return false;
    }return true;
}
static bool video_setup(void){
    /* spawn only maps descriptors 0..2. Duplicate the VIDEO endpoint, then
       restore stderr to null: FFmpeg/native diagnostics cannot become pixels. */
    video_fd=dup(2);if(video_fd<0)return false;
    int nullfd=open("/dev/null",O_WRONLY);
    if(nullfd<0){close(video_fd);video_fd=-1;return false;}
    int result=dup2(nullfd,2);close(nullfd);
    if(result<0||fcntl(video_fd,F_SETFL,O_NONBLOCK)<0){close(video_fd);video_fd=-1;return false;}
    return true;
}

int main(void) {
    if(!nmedia_alloc_restrict_worker())return 1;
    struct nmedia_worker_command c;
    if(!read_all(&c,sizeof c)||c.magic!=NMEDIA_WORKER_MAGIC||c.operation!=NMW_OPEN||
       !c.url_bytes||!c.document_bytes||!c.epoch)return 1;
    uint32_t generation=c.generation,sequence=c.sequence;uint64_t epoch=c.epoch;
    bool video_ok=video_setup();struct video_tx video={0};struct nmedia_output pending={0};
    char error[160]={0};char *url=NULL,*document=NULL;
    /* Wire lengths stay uint32. The NUL is private storage, not a quota or
     * an extra wire byte. Check size_t arithmetic before actual allocation. */
    if((size_t)c.url_bytes==SIZE_MAX||(size_t)c.document_bytes==SIZE_MAX)return 1;
    url=nmedia_ff_malloc((size_t)c.url_bytes+1);
    document=nmedia_ff_malloc((size_t)c.document_bytes+1);
    if(!url||!document){
        nmedia_ff_free(url);nmedia_ff_free(document);
        struct nmedia_worker_response r={.magic=NMEDIA_WORKER_MAGIC,.operation=NMW_OPEN,
            .generation=generation,.sequence=sequence,.kind=NMEDIA_ERROR,.duration_ms=-1};
        strlcpy(r.error,"native media OPEN URL allocation failed",sizeof r.error);write_all(&r,sizeof r);return 1;
    }
    if(!read_all(url,c.url_bytes)||!read_all(document,c.document_bytes)||
       memchr(url,0,c.url_bytes)||memchr(document,0,c.document_bytes)){
        nmedia_ff_free(url);nmedia_ff_free(document);return 1;
    }
    url[c.url_bytes]=0;document[c.document_bytes]=0;
    nmedia *m=video_ok?nmedia_open_url_cors(url,document,error,sizeof error):NULL;
    if(!video_ok)strlcpy(error,"Range VIDEO descriptor setup failed",sizeof error);
    nmedia_ff_free(url);nmedia_ff_free(document);
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
                o=pending;r.kind=o.kind;r.pts_ms=o.pts_ms;
                /* The first image is detached. Continue muxed PCM while it
                   drains; hold a second decoder image without overwriting it. */
                if((r.kind==NMEDIA_VIDEO||r.kind==NMEDIA_END)&&video.pending)r.kind=NMEDIA_AGAIN;
                else if(r.kind==NMEDIA_AUDIO){r.channels=2;r.sample_rate=SOUND_RATE;r.frames=(uint32_t)o.frames;r.payload_bytes=r.frames*4;payload=o.samples;pending.kind=0;}
                else if(r.kind==NMEDIA_VIDEO){r.width=o.width;r.height=o.height;uint32_t bytes;
                    if(!nmedia_video_wire_bytes(o.width,o.height,&bytes)||!nmedia_move_video(m,&o,&video.pixels,&video.capacity)){
                        r.kind=NMEDIA_ERROR;strlcpy(r.error,"Range VIDEO owner/size transfer failed",sizeof r.error);
                    }else{video.header=(struct nmedia_worker_video){.magic=NMEDIA_WORKER_MAGIC,.generation=generation,.sequence=sequence,
                        .bytes=bytes,.epoch=epoch,.pts=o.pts_ms,.width=o.width,.height=o.height};
                        video.position=0;video.pending=true;pending.kind=0;r.kind=NMW_VIDEO_PENDING;}
                }else if(r.kind==NMEDIA_ERROR)strlcpy(r.error,nmedia_error(m),sizeof r.error);
                else if(r.kind==NMEDIA_AGAIN)pending.kind=0;
            }
        }
        if(!write_all(&r,sizeof r)|| (r.payload_bytes&&!write_all(payload,r.payload_bytes)))break;
        if(r.kind==NMEDIA_ERROR)break;
        if(!video_send(&video)||!command_wait(&video))break;
        if(!read_all(&c,sizeof c)||c.magic!=NMEDIA_WORKER_MAGIC||c.generation!=generation||
           c.sequence!=sequence+1||c.url_bytes||c.document_bytes||
           (c.operation!=NMW_STEP&&c.operation!=NMW_SEEK)||c.seek_ms<0||c.seek_ms>1000000000000LL||
           (c.operation==NMW_SEEK?c.epoch<=epoch:c.epoch!=epoch))break;
        sequence=c.sequence;epoch=c.epoch;
    }
    nmedia_close(m);nmedia_ff_free(video.pixels);if(video_fd>=0)close(video_fd);return 0;
}
