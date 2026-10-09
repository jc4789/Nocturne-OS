/* Nocturne native pipe worker: page-fetch bytes only, no FFmpeg URL protocol. */
#include "media_mse_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
static bool exact(int fd,void *bytes,size_t size,bool writing){
    size_t at=0;while(at<size){ssize_t n=writing?write(fd,(uint8_t *)bytes+at,size-at):read(fd,(uint8_t *)bytes+at,size-at);
        if(n<0&&errno==EINTR)continue;if(n<=0)return false;at+=(size_t)n;}return true;
}

struct child_slot {nmedia_mse *buffer;struct nmedia_output pending;};
/* Converted pixels are detached from the decoder before any subsequent
 * command. APPEND/seek/drop can therefore run while this owner is in flight. */
struct video_tx {
    struct nmsw_video header;
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
static bool slots_reserve(struct child_slot **slots,size_t *count,size_t *capacity,size_t wanted){
    if(wanted>*capacity){size_t n=*capacity?*capacity:4;while(n<wanted){if(n>SIZE_MAX/sizeof **slots/2){n=wanted;break;}n*=2;}
        if(n>SIZE_MAX/sizeof **slots)return false;struct child_slot *v=nmedia_ff_realloc(*slots,n*sizeof *v);if(!v)return false;
        memset(v+*capacity,0,(n-*capacity)*sizeof *v);*slots=v;*capacity=n;}
    if(wanted>*count)*count=wanted;return true;
}
static bool metadata(struct child_slot *slots,size_t count,struct nmsw_response *r,uint8_t **storage,size_t *capacity){
    size_t needed=0;
    for(size_t i=0;i<count;i++)if(slots[i].buffer){size_t ranges=nmedia_mse_range_capacity(slots[i].buffer);
        if(ranges>UINT32_MAX||ranges>(UINT32_MAX-sizeof(struct nmsw_metadata))/sizeof(struct nmedia_time_range))return false;
        size_t bytes=sizeof(struct nmsw_metadata)+ranges*sizeof(struct nmedia_time_range);if(bytes>UINT32_MAX-needed)return false;needed+=bytes;}
    if(needed>*capacity){uint8_t *v=nmedia_ff_realloc(*storage,needed);if(!v)return false;*storage=v;*capacity=needed;}
    size_t at=0;unsigned audio=0,video=0;
    for(size_t i=0;i<count;i++)if(slots[i].buffer){nmedia_mse *m=slots[i].buffer;struct nmsw_metadata *record=(void *)(*storage+at);memset(record,0,sizeof *record);
        record->slot=(uint32_t)i;record->revision=nmedia_mse_revision(m);record->parsing=nmedia_mse_parsing(m);record->group_end=nmedia_mse_group_end(m);
        struct nmedia_time_range *ranges=(void *)(*storage+at+sizeof *record);record->count=(uint32_t)nmedia_mse_ranges(m,ranges,nmedia_mse_range_capacity(m));
        if(nmedia_mse_error(m)[0]){r->kind=NMEDIA_ERROR;r->payload_bytes=0;r->quota_error=nmedia_mse_quota_error(m);strlcpy(r->error,nmedia_mse_error(m),sizeof r->error);}
        at+=sizeof *record+(size_t)record->count*sizeof *ranges;r->metadata_count++;
        const struct nmedia_info *info=nmedia_mse_info(m);if(!info)continue;audio+=info->audio;video+=info->video;
        record->info=(struct nmsw_info){.audio=info->audio,.video=info->video,.channels=(uint32_t)info->channels,.sample_rate=(uint32_t)info->sample_rate,.width=info->width,.height=info->height,.duration=nmedia_mse_duration(m)};
        strlcpy(record->info.audio_codec,info->audio_codec,32);strlcpy(record->info.video_codec,info->video_codec,32);strlcpy(record->info.container,info->container,32);
    }
    r->metadata_bytes=(uint32_t)at;
    /* Existing sink decodes one selected audio/video owner. This is an actual
       unsupported track configuration, not a SourceBuffer allocation count. */
    if(audio>1||video>1){r->kind=NMEDIA_ERROR;r->payload_bytes=0;strlcpy(r->error,"overlapping MSE track ownership",sizeof r->error);}return true;
}
int main(void){
    if(!nmedia_alloc_restrict_worker())return 1;
    bool video_ok=video_setup();struct video_tx video={0};
    struct child_slot *slots=NULL;size_t count=0,capacity=0,meta_capacity=0;uint8_t *meta=NULL;
    uint32_t generation=0,sequence=0;bool established=false,enabled[2]={true,true};
    for(;;){
        if(!command_wait(&video))break;
        struct nmsw_command c;if(!exact(0,&c,sizeof c,false))break;
        if(c.magic!=NMSW_MAGIC||c.reserved||c.slot>INT32_MAX||c.flags>1||c.op<NMSW_ADD||c.op>NMSW_SELECT||c.current<0||c.current>1000000000000LL||
           (c.bytes&&c.op!=NMSW_ADD&&c.op!=NMSW_CHANGE&&c.op!=NMSW_APPEND)||
           (established&&(c.generation!=generation||c.sequence!=sequence+1)))break;
        generation=c.generation;sequence=c.sequence;established=true;
        struct nmsw_response r={.magic=NMSW_MAGIC,.op=c.op,.generation=generation,.sequence=sequence};
        bool ok=true;unsigned slot=c.slot;nmedia_mse *m=slot<count?slots[slot].buffer:NULL;struct nmedia_output output={0};
        if(!video_ok){r.kind=NMEDIA_ERROR;strlcpy(r.error,"MSE VIDEO descriptor setup failed",sizeof r.error);exact(1,&r,sizeof r,true);break;}
        uint8_t *bytes=NULL;if(c.bytes){bytes=nmedia_ff_malloc((size_t)c.bytes+1);if(!bytes){
                uint8_t drain[4096];size_t left=c.bytes;while(left){size_t n=MIN(left,sizeof drain);if(!exact(0,drain,n,false))goto shutdown;left-=n;}
                struct nmedia_alloc_stats stats;nmedia_alloc_snapshot(&stats);
                r.kind=NMEDIA_ERROR;r.quota_error=1;
                snprintf(r.error,sizeof r.error,"MSE child append %s: request %zu charged %zu limit %zu",
                    stats.last_failure==NMEDIA_ALLOC_BACKEND_OOM?"backend OOM":"allocation quota",
                    (size_t)c.bytes+1,stats.current,stats.limit);goto response;
            }
            if(!exact(0,bytes,c.bytes,false)){nmedia_ff_free(bytes);break;}bytes[c.bytes]=0;}
        if(c.op==NMSW_ADD||c.op==NMSW_CHANGE){
            uint64_t revision=nmedia_mse_revision(m);
            if(!bytes||!c.bytes||c.bytes>=256||memchr(bytes,0,c.bytes)){ok=false;strlcpy(r.error,"invalid MSE MIME wire",sizeof r.error);}
            else if(c.op==NMSW_ADD){if(m)ok=false;else if(!slots_reserve(&slots,&count,&capacity,(size_t)slot+1)){ok=false;r.quota_error=1;strlcpy(r.error,"MSE slot allocation failed",sizeof r.error);}else{slots[slot].buffer=m=nmedia_mse_create((char *)bytes,r.error,sizeof r.error);ok=m!=NULL;}}
            else ok=m&&nmedia_mse_change_type(m,(char *)bytes);
            if(revision!=nmedia_mse_revision(m))if(slot<count)slots[slot].pending.kind=0;
        }else if(c.op==NMSW_APPEND){uint64_t revision=nmedia_mse_revision(m);ok=nmedia_mse_append_take(m,bytes,c.bytes,c.offset,c.start,c.end,c.flags);bytes=NULL;if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);if(slot<count)slots[slot].pending.kind=0;}}
        else if(c.op==NMSW_REMOVE){uint64_t revision=nmedia_mse_revision(m);ok=m&&nmedia_mse_remove(m,c.start,c.end);if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);if(slot<count)slots[slot].pending.kind=0;}}
        else if(c.op==NMSW_ABORT){if(m){nmedia_mse_abort(m);if(nmedia_mse_info(m))nmedia_mse_seek(m,c.current);}if(slot<count)slots[slot].pending.kind=0;}
        else if(c.op==NMSW_DROP){nmedia_mse_close(m);if(slot<count)slots[slot].buffer=NULL;m=NULL;if(slot<count)slots[slot].pending.kind=0;}
        else if(c.op==NMSW_END){for(size_t i=0;i<count;i++)if(slots[i].buffer){uint64_t revision=nmedia_mse_revision(slots[i].buffer);if(!nmedia_mse_end(slots[i].buffer,c.flags)){m=slots[i].buffer;ok=false;break;}if(revision!=nmedia_mse_revision(slots[i].buffer)||(!c.flags&&slots[i].pending.kind==NMEDIA_END))slots[i].pending.kind=0;}}
        else if(c.op==NMSW_SEEK){for(size_t i=0;i<count;i++){slots[i].pending.kind=0;if(slots[i].buffer&&nmedia_mse_info(slots[i].buffer)&&!nmedia_mse_seek(slots[i].buffer,c.start)){m=slots[i].buffer;ok=false;break;}}}
        else if(c.op==NMSW_SELECT){if(c.slot>1)ok=false;else enabled[c.slot]=c.flags;}
        else if(c.op==NMSW_STEP){
            bool busy=false;int selected=-1;size_t active=0,ended=0;
            for(size_t i=0;i<count;i++)if(slots[i].buffer){
                const struct nmedia_info *info=nmedia_mse_info(slots[i].buffer);if(!info)continue;
                bool needed=(!enabled[0]&&!enabled[1])||(info->audio&&enabled[0])||(info->video&&enabled[1]);if(needed)active++;
                /* Only independent video buffers use the split lane. Their
                   already-owned image must not block decoding another AUDIO
                   buffer. Muxed/video-first demux retains its existing order. */
                if(video.pending&&info->video&&!info->audio&&enabled[1])continue;
                for(int budget=0;budget<16;budget++){
                    if(!slots[i].pending.kind){int result=nmedia_mse_step(slots[i].buffer,&slots[i].pending);if(result==NMEDIA_ERROR){m=slots[i].buffer;ok=false;break;}if(result==NMEDIA_AGAIN){if(needed&&!nmedia_mse_waiting_for_input(slots[i].buffer))busy=true;break;}}
                    if((slots[i].pending.kind==NMEDIA_AUDIO&&!enabled[0])||(slots[i].pending.kind==NMEDIA_VIDEO&&!enabled[1])){
                        if(!enabled[0]&&!enabled[1]){int64_t end=slots[i].pending.pts_ms+(slots[i].pending.kind==NMEDIA_AUDIO?(int64_t)slots[i].pending.frames*1000/SOUND_RATE:40);memset(&slots[i].pending,0,sizeof slots[i].pending);slots[i].pending.kind=NMSW_CLOCK;slots[i].pending.pts_ms=end;break;}
                        slots[i].pending.kind=0;if(budget==15&&needed)busy=true;continue;}break;
                }
                if(!ok)break;if(!needed)continue;
                if(slots[i].pending.kind==NMEDIA_END){ended++;continue;}if(!slots[i].pending.kind)continue;if(selected<0||slots[i].pending.pts_ms<slots[selected].pending.pts_ms)selected=i;
            }
            /* Independent SourceBuffers may exhaust their input at different
               PTS. An input wait must not suppress another buffer's decoded
               output. Bounded decoder work still holds it: that decoder can
               produce an earlier PTS before it next needs appended input. */
            if(ok&&!busy&&selected>=0){r.slot=selected;output=slots[selected].pending;slots[selected].pending.kind=0;r.kind=output.kind;r.pts=output.pts_ms;r.frames=(uint32_t)output.frames;r.width=output.width;r.height=output.height;
                if(output.kind==NMEDIA_AUDIO)r.payload_bytes=(uint32_t)output.frames*4;
                else if(output.kind==NMEDIA_VIDEO){
                    if(!nmedia_video_wire_bytes(output.width,output.height,&r.payload_bytes)){r.kind=NMEDIA_ERROR;strlcpy(r.error,"MSE video payload length is not representable",sizeof r.error);}
                    else {const struct nmedia_info *info=nmedia_mse_info(slots[selected].buffer);
                        if(info&&info->video&&!info->audio){
                            if(video.pending||!nmedia_mse_move_video(slots[selected].buffer,&output,&video.pixels,&video.capacity)){
                                r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"MSE VIDEO owner transfer failed",sizeof r.error);
                            }else{
                                video.header=(struct nmsw_video){.magic=NMSW_MAGIC,.generation=generation,.sequence=sequence,.slot=(uint32_t)selected,
                                    .bytes=r.payload_bytes,.epoch=c.epoch,.revision=nmedia_mse_revision(slots[selected].buffer),.pts=output.pts_ms,.width=output.width,.height=output.height};
                                video.pending=true;video.position=0;r.kind=NMSW_VIDEO_PENDING;r.payload_bytes=0;
                            }
                        }
                    }
                }}
            else if(ok&&active&&ended==active&&!video.pending){r.kind=NMEDIA_END;}
            else if(ok&&(busy||video.pending))r.kind=NMSW_BUSY;
        }
        nmedia_ff_free(bytes);
        if(!ok){r.kind=NMEDIA_ERROR;r.quota_error|=nmedia_mse_quota_error(m);if(!r.error[0])strlcpy(r.error,m?nmedia_mse_error(m):"MSE command has no SourceBuffer",sizeof r.error);}
response:
        if(!metadata(slots,count,&r,&meta,&meta_capacity)){r.kind=NMEDIA_ERROR;r.payload_bytes=0;r.metadata_count=r.metadata_bytes=0;r.quota_error=1;strlcpy(r.error,"MSE metadata size/allocation failure",sizeof r.error);}
        if(!exact(1,&r,sizeof r,true)||(r.metadata_bytes&&!exact(1,meta,r.metadata_bytes,true)))break;
        if(r.payload_bytes&&!exact(1,(void *)(output.kind==NMEDIA_AUDIO?(const void *)output.samples:(const void *)output.pixels),r.payload_bytes,true))break;
        if(!video_send(&video))break;
    }
shutdown:
    for(size_t i=0;i<count;i++)nmedia_mse_close(slots[i].buffer);nmedia_ff_free(slots);nmedia_ff_free(meta);nmedia_ff_free(video.pixels);if(video_fd>=0)close(video_fd);return 0;
}
