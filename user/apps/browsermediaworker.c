/* HTTP/CORS, probing, decode and seeking belong to this isolated native child.
 * No audio device or GUI here. Parent applies timing and owns presentation. */
#include "media_worker_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdio.h>
static bool read_all(void *out,size_t n) {
    while(n){ssize_t k=read(0,out,n);if(k<=0)return false;out=(char *)out+k;n-=(size_t)k;}return true;
}
static bool write_all(const void *in,size_t n) {
    while(n){ssize_t k=write(1,in,n);if(k<=0)return false;in=(const char *)in+k;n-=(size_t)k;}return true;
}
int main(void) {
    if(!nmedia_alloc_restrict_worker())return 1;
    struct nmedia_worker_command c;
    if(!read_all(&c,sizeof c)||c.magic!=NMEDIA_WORKER_MAGIC||c.operation!=NMW_OPEN||
       !c.url_bytes||c.url_bytes>=NMEDIA_WORKER_URL||!c.document_bytes||c.document_bytes>=NMEDIA_WORKER_URL)return 1;
    uint32_t generation=c.generation,sequence=c.sequence;
    char url[NMEDIA_WORKER_URL],document[NMEDIA_WORKER_URL],error[160]={0};
    if(!read_all(url,c.url_bytes)||!read_all(document,c.document_bytes)||
       memchr(url,0,c.url_bytes)||memchr(document,0,c.document_bytes))return 1;
    url[c.url_bytes]=0;document[c.document_bytes]=0;
    nmedia *m=nmedia_open_url_cors(url,document,error,sizeof error);
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
                if(!nmedia_seek(m,c.seek_ms)){r.kind=NMEDIA_ERROR;strlcpy(r.error,nmedia_error(m),sizeof r.error);if(!r.error[0])strlcpy(r.error,"native worker seek failed",sizeof r.error);}
            } else if(c.operation==NMW_STEP) {
                r.kind=nmedia_step(m,&o);r.pts_ms=o.pts_ms;
                if(r.kind==NMEDIA_AUDIO){r.channels=2;r.sample_rate=SOUND_RATE;r.frames=(uint32_t)o.frames;r.payload_bytes=r.frames*4;payload=o.samples;}
                else if(r.kind==NMEDIA_VIDEO){r.width=o.width;r.height=o.height;r.payload_bytes=(uint32_t)o.width*(uint32_t)o.height*4;payload=o.pixels;}
                else if(r.kind==NMEDIA_ERROR)strlcpy(r.error,nmedia_error(m),sizeof r.error);
            }
        }
        if(!write_all(&r,sizeof r)|| (r.payload_bytes&&!write_all(payload,r.payload_bytes)))break;
        if(r.kind==NMEDIA_ERROR)break;
        if(!read_all(&c,sizeof c)||c.magic!=NMEDIA_WORKER_MAGIC||c.generation!=generation||
           c.sequence!=sequence+1||c.url_bytes||c.document_bytes||
           (c.operation!=NMW_STEP&&c.operation!=NMW_SEEK)||c.seek_ms<0||c.seek_ms>1000000000000LL)break;
        sequence=c.sequence;
    }
    nmedia_close(m);return 0;
}
