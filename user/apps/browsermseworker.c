/* Nocturne native MSE worker: page-fetch bytes only, no FFmpeg URL protocol.
 * It is launched with no input, output or log: handle 3 is the control
 * channel (one command in, one response out), handle 4 the VIDEO channel
 * (headers out; new shared pages travel attached). */
#include "media_mse_worker_private.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#define CONTROL 3
#define VIDEO 4

struct child_slot {nmedia_mse *buffer;struct nmedia_output pending;uint64_t published_revision,reported_decoded,reported_dropped;struct nmedia_info published_info;bool published,published_key_waiting;};
struct video_storage {
    struct video_storage *next;
    struct nmsw_shared_video *shared;
    size_t bytes;uint32_t capacity;n_handle handle;uint64_t id;
};
/* Shared slots own snapshots, not decoder loans. Busy old geometry remains
 * alive until the receiver returns it, even across APPEND/seek/drop. */
struct video_tx {
    struct nmsw_video header;
    struct video_storage *storage,*retired;uint64_t storage_id,sent_id;
    bool pending,horizon_valid;
    int64_t horizon_pts;
};
static unsigned video_storage_used(const struct video_storage *s){
    unsigned used=0;
    if(s)for(unsigned i=0;i<NMSW_VIDEO_SLOTS;i++)used+=__atomic_load_n(&s->shared->occupied[i],__ATOMIC_ACQUIRE)!=0;
    return used;
}
static bool video_storage_busy(const struct video_storage *s){
    return video_storage_used(s)!=0;
}
static bool video_full(const struct video_tx *v){
    if(v->pending)return true;
    unsigned used=video_storage_used(v->storage);
    if(used>=NMSW_VIDEO_SLOTS)return true;
    /* Resize must not turn a two-frame transfer window into an unbounded
       succession of busy old rings. This is backpressure, not frame loss. */
    for(const struct video_storage *s=v->retired;s;s=s->next){
        used+=video_storage_used(s);if(used>=NMSW_VIDEO_SLOTS)return true;
    }return false;
}
static void video_storage_free(struct video_storage *s){
    if(!s)return;if(s->shared)n_memory_release(s->shared,s->bytes);if(s->handle>=0)n_handle_close(s->handle);nmedia_ff_free(s);
}
static void video_collect(struct video_tx *v){
    struct video_storage **link=&v->retired;
    while(*link){struct video_storage *s=*link;if(video_storage_busy(s)){link=&s->next;continue;}
        *link=s->next;video_storage_free(s);
    }
}
static bool video_publish(struct video_tx *v,const struct nmedia_output *o,uint32_t bytes){
    if(v->pending||!o->pixels)return false;
    if(!v->storage || v->storage->capacity<bytes){
        if(bytes>UINT32_MAX-63 || v->storage_id==UINT64_MAX)return false;
        uint32_t capacity=(bytes+63u)&~63u;size_t total;
        if(!nmsw_shared_size(capacity,&total))return false;
        struct video_storage *s=nmedia_ff_mallocz(sizeof *s);if(!s)return false;
        s->handle=n_shared_create(total);s->bytes=total;s->capacity=capacity;s->id=++v->storage_id;
        if(s->handle<0 || !(s->shared=n_shared_map(s->handle,total,N_MEMORY_READ_WRITE))){video_storage_free(s);return false;}
        s->shared->id=s->id; /* immutable backing identity, not only wire state */
        if(v->storage){v->storage->next=v->retired;v->retired=v->storage;}
        v->storage=s;
    }
    struct video_storage *s=v->storage;
    for(unsigned slot=0;slot<NMSW_VIDEO_SLOTS;slot++){
        if(__atomic_load_n(&s->shared->occupied[slot],__ATOMIC_ACQUIRE))continue;
        uint8_t *pixels=(uint8_t *)(s->shared+1)+(size_t)slot*s->capacity;
        memcpy(pixels,o->pixels,bytes);
        __atomic_store_n(&s->shared->occupied[slot],1,__ATOMIC_RELEASE);
        v->header.buffer_slot=slot;
        v->header.shared_capacity=s->capacity;v->header.shared_id=s->id;
        v->pending=true;return true;
    }return false;
}
/* At most NMSW_VIDEO_SLOTS headers wait in the parent's queue, so the send
   never waits. The storage's handle travels with its first header. */
static bool video_send(struct video_tx *v){
    if(!v->pending)return true;
    bool fresh=v->header.shared_id!=v->sent_id;
    v->header.attached=fresh;
    if(n_channel_send(VIDEO,&v->header,sizeof v->header,fresh?v->storage->handle:-1)<0)return false;
    v->sent_id=v->header.shared_id;v->pending=false;return true;
}
static bool slots_reserve(struct child_slot **slots,size_t *count,size_t *capacity,size_t wanted){
    if(wanted>*capacity){size_t n=*capacity?*capacity:4;while(n<wanted){if(n>SIZE_MAX/sizeof **slots/2){n=wanted;break;}n*=2;}
        if(n>SIZE_MAX/sizeof **slots)return false;struct child_slot *v=nmedia_ff_realloc(*slots,n*sizeof *v);if(!v)return false;
        memset(v+*capacity,0,(n-*capacity)*sizeof *v);*slots=v;*capacity=n;}
    if(wanted>*count)*count=wanted;return true;
}
static bool metadata_changed(struct child_slot *slot,bool force){
    const struct nmedia_info *info=nmedia_mse_info(slot->buffer);
    return force||!slot->published||slot->published_revision!=nmedia_mse_revision(slot->buffer)||
        slot->published_key_waiting!=nmedia_mse_key_waiting(slot->buffer)||
        (info&&memcmp(info,&slot->published_info,sizeof *info));
}
static bool metadata(struct child_slot *slots,size_t count,struct nmsw_response *r,uint8_t **storage,size_t *capacity){
    bool force=r->op!=NMSW_STEP;
    size_t needed=0;
    for(size_t i=0;i<count;i++)if(slots[i].buffer&&metadata_changed(&slots[i],force)){size_t ranges=nmedia_mse_range_capacity(slots[i].buffer);
        if(ranges>UINT32_MAX||ranges>(UINT32_MAX-sizeof(struct nmsw_metadata))/sizeof(struct nmedia_time_range))return false;
        size_t bytes=sizeof(struct nmsw_metadata)+ranges*sizeof(struct nmedia_time_range);if(bytes>UINT32_MAX-needed)return false;needed+=bytes;}
    if(needed>*capacity){uint8_t *v=nmedia_ff_realloc(*storage,needed);if(!v)return false;*storage=v;*capacity=needed;}
    size_t at=0;unsigned audio=0,video=0;
    for(size_t i=0;i<count;i++)if(slots[i].buffer){nmedia_mse *m=slots[i].buffer;
        const struct nmedia_info *info=nmedia_mse_info(m);if(info){audio+=info->audio;video+=info->video;}
        if(nmedia_mse_error(m)[0]){r->kind=NMEDIA_ERROR;r->payload_bytes=0;r->quota_error=nmedia_mse_quota_error(m);strlcpy(r->error,nmedia_mse_error(m),sizeof r->error);}
        if(!metadata_changed(&slots[i],force))continue;
        struct nmsw_metadata *record=(void *)(*storage+at);memset(record,0,sizeof *record);
        record->slot=(uint32_t)i;record->revision=nmedia_mse_revision(m);record->parsing=nmedia_mse_parsing(m);record->group_end=nmedia_mse_group_end(m);record->key_waiting=nmedia_mse_key_waiting(m);
        struct nmedia_time_range *ranges=(void *)(*storage+at+sizeof *record);record->count=(uint32_t)nmedia_mse_ranges(m,ranges,nmedia_mse_range_capacity(m));
        if(nmedia_mse_error(m)[0]){r->kind=NMEDIA_ERROR;r->payload_bytes=0;r->quota_error=nmedia_mse_quota_error(m);strlcpy(r->error,nmedia_mse_error(m),sizeof r->error);}
        at+=sizeof *record+(size_t)record->count*sizeof *ranges;r->metadata_count++;
        slots[i].published=true;slots[i].published_revision=record->revision;slots[i].published_key_waiting=!!record->key_waiting;
        if(!info){memset(&slots[i].published_info,0,sizeof slots[i].published_info);continue;}
        slots[i].published_info=*info;
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
    struct video_tx video={0};
    struct child_slot *slots=NULL;size_t count=0,capacity=0,meta_capacity=0,out_capacity=0;uint8_t *meta=NULL,*out=NULL;
    uint32_t generation=0,sequence=0;bool established=false,enabled[2]={true,true};
    int64_t video_delivered_pts=0;bool video_delivered=false;
    uint8_t *keys=NULL;size_t key_bytes=0;
    int16_t pcm_batch[4096*2];
    for(;;){
        video_collect(&video);
        /* Commit the image horizon only after a complete shared-slot header
           is available to the receiver. Its immutable raster was published
           first; a partially written header cannot release seconds of PCM. */
        if(!video.pending&&video.horizon_valid){video_delivered=true;video_delivered_pts=video.horizon_pts;video.horizon_valid=false;}
        /* One message: the command, then its bytes. The bytes become this
           child's own block; without room for them only the command is read. */
        struct nmsw_command c;
        n_result size=n_channel_receive(CONTROL,NULL,0,N_CHANNEL_WAIT|N_CHANNEL_PEEK,NULL);
        if(size<(n_result)sizeof c||(uint64_t)size-sizeof c>UINT32_MAX)break;
        uint8_t *bytes=NULL;bool starved=false;
        if(size==(n_result)sizeof c){if(n_channel_receive(CONTROL,&c,sizeof c,0,NULL)!=size)break;}
        else if((bytes=nmedia_ff_malloc((size_t)size+1))){
            if(n_channel_receive(CONTROL,bytes,(size_t)size,0,NULL)!=size){nmedia_ff_free(bytes);break;}
            memcpy(&c,bytes,sizeof c);
        }else{if(n_channel_receive(CONTROL,&c,sizeof c,N_CHANNEL_DISCARD,NULL)!=size)break;starved=true;}
        if(c.magic!=NMSW_MAGIC||c.reserved||c.bytes!=(uint64_t)size-sizeof c||c.slot>INT32_MAX||c.flags>1||c.op<NMSW_ADD||c.op>NMSW_KEYS||c.current<0||c.current>1000000000000LL||
           (c.bytes&&c.op!=NMSW_ADD&&c.op!=NMSW_CHANGE&&c.op!=NMSW_APPEND&&c.op!=NMSW_KEYS)||
           (established&&(c.generation!=generation||c.sequence!=sequence+1))){nmedia_ff_free(bytes);break;}
        if(bytes){
            /* The parser takes the block as its staging input from offset 0;
               the moved-from tail held a copy of the bytes (keys, too). */
            memmove(bytes,bytes+sizeof c,c.bytes);memset(bytes+c.bytes,0,sizeof c+1);
        }
        generation=c.generation;sequence=c.sequence;established=true;
        struct nmsw_response r={.magic=NMSW_MAGIC,.op=c.op,.generation=generation,.sequence=sequence};
        bool ok=true;unsigned slot=c.slot;nmedia_mse *m=slot<count?slots[slot].buffer:NULL;struct nmedia_output output={0};
        uint64_t before_revision=nmedia_mse_revision(m);const struct nmedia_info *before_info=nmedia_mse_info(m);
        bool video_mutation=before_info&&before_info->video;
        if(starved){
            struct nmedia_alloc_stats stats;nmedia_alloc_snapshot(&stats);
            r.kind=NMEDIA_ERROR;r.quota_error=1;
            snprintf(r.error,sizeof r.error,"MSE child append %s: request %zu charged %zu limit %zu",
                stats.last_failure==NMEDIA_ALLOC_BACKEND_OOM?"backend OOM":"allocation quota",
                (size_t)size+1,stats.current,stats.limit);goto response;
        }
        if(c.op==NMSW_ADD||c.op==NMSW_CHANGE){
            uint64_t revision=nmedia_mse_revision(m);
            if(!bytes||!c.bytes||c.bytes>=256||memchr(bytes,0,c.bytes)){ok=false;strlcpy(r.error,"invalid MSE MIME wire",sizeof r.error);}
            else if(c.op==NMSW_ADD){if(m)ok=false;else if(!slots_reserve(&slots,&count,&capacity,(size_t)slot+1)){ok=false;r.quota_error=1;strlcpy(r.error,"MSE slot allocation failed",sizeof r.error);}else{slots[slot].reported_decoded=slots[slot].reported_dropped=0;slots[slot].published=false;slots[slot].buffer=m=nmedia_mse_create((char *)bytes,r.error,sizeof r.error);ok=m!=NULL;if(c.bytes>=6&&!memcmp(bytes,"video/",6)){video_delivered=false;video.horizon_valid=false;}}}
            else ok=m&&nmedia_mse_change_type(m,(char *)bytes);
            if(ok&&c.op==NMSW_ADD&&!nmedia_mse_keys(m,keys,key_bytes))ok=false;
            if(revision!=nmedia_mse_revision(m))if(slot<count)slots[slot].pending.kind=0;
        }else if(c.op==NMSW_APPEND){uint64_t revision=nmedia_mse_revision(m);ok=nmedia_mse_append_take(m,bytes,c.bytes,c.offset,c.start,c.end,c.flags);bytes=NULL;if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);if(slot<count)slots[slot].pending.kind=0;}}
        else if(c.op==NMSW_REMOVE){uint64_t revision=nmedia_mse_revision(m);ok=m&&nmedia_mse_remove(m,c.start,c.end);if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);if(slot<count)slots[slot].pending.kind=0;}}
        else if(c.op==NMSW_ABORT){if(m){nmedia_mse_abort(m);if(nmedia_mse_info(m))nmedia_mse_seek(m,c.current);}if(slot<count)slots[slot].pending.kind=0;}
        else if(c.op==NMSW_DROP){const struct nmedia_info *info=nmedia_mse_info(m);if(info&&info->video){video_delivered=false;video.horizon_valid=false;}nmedia_mse_close(m);if(slot<count)slots[slot].buffer=NULL;m=NULL;if(slot<count)slots[slot].pending.kind=0;}
        else if(c.op==NMSW_END){for(size_t i=0;i<count;i++)if(slots[i].buffer){uint64_t revision=nmedia_mse_revision(slots[i].buffer);if(!nmedia_mse_end(slots[i].buffer,c.flags)){m=slots[i].buffer;ok=false;break;}if(revision!=nmedia_mse_revision(slots[i].buffer)||(!c.flags&&slots[i].pending.kind==NMEDIA_END))slots[i].pending.kind=0;}}
        else if(c.op==NMSW_SEEK){video_delivered=false;video.horizon_valid=false;for(size_t i=0;i<count;i++){slots[i].pending.kind=0;if(slots[i].buffer&&nmedia_mse_info(slots[i].buffer)&&!nmedia_mse_seek(slots[i].buffer,c.start)){m=slots[i].buffer;ok=false;break;}}}
        else if(c.op==NMSW_SELECT){if(c.slot>1)ok=false;else{enabled[c.slot]=c.flags;if(c.slot==1){video_delivered=false;video.horizon_valid=false;}}}
        else if(c.op==NMSW_KEYS){
            if(c.bytes%32){ok=false;strlcpy(r.error,"invalid CENC key snapshot",sizeof r.error);}
            else{
                for(size_t i=0;i<count;i++)if(slots[i].buffer&&!nmedia_mse_keys(slots[i].buffer,bytes,c.bytes)){m=slots[i].buffer;ok=false;break;}
                if(ok){volatile uint8_t *wipe=keys;for(size_t i=0;i<key_bytes;i++)wipe[i]=0;
                    nmedia_ff_free(keys);keys=bytes;key_bytes=c.bytes;bytes=NULL;}
            }
        }
        else if(c.op==NMSW_STEP){
            bool busy=false;int selected=-1;size_t active=0,ended=0;
            bool independent_video=false,video_ended=false;
            /* The shared VIDEO ring can be backpressured while PCM's control
               messages are writable. An independent audio decoder must not
               run seconds past the last completely transferred image.
               This is output flow control, not clock forgery: retain its PCM
               loan and keep advancing/servicing the VIDEO owner instead. */
            for(size_t i=0;i<count;i++)if(slots[i].buffer&&enabled[1]){
                const struct nmedia_info *info=nmedia_mse_info(slots[i].buffer);
                if(info&&info->video&&!info->audio){independent_video=true;video_ended=slots[i].pending.kind==NMEDIA_END;}
            }
            for(size_t i=0;i<count;i++)if(slots[i].buffer){
                const struct nmedia_info *info=nmedia_mse_info(slots[i].buffer);if(!info)continue;
                bool needed=(!enabled[0]&&!enabled[1])||(info->audio&&enabled[0])||(info->video&&enabled[1]);if(needed)active++;
                /* Shared VIDEO does not borrow the decoder's storage. A muxed
                   decoder can yield PCM while the ring holds earlier images.
                   Retain the next decoder loan only when every slot is busy. */
                if(video_full(&video)&&info->video&&!info->audio&&enabled[1])continue;
                for(int budget=0;budget<16;budget++){
                    if(!slots[i].pending.kind){int result=nmedia_mse_step_at(slots[i].buffer,&slots[i].pending,c.current);if(result==NMEDIA_ERROR){m=slots[i].buffer;ok=false;break;}if(result==NMEDIA_AGAIN){if(needed&&!nmedia_mse_waiting_for_input(slots[i].buffer))busy=true;break;}}
                    if((slots[i].pending.kind==NMEDIA_AUDIO&&!enabled[0])||(slots[i].pending.kind==NMEDIA_VIDEO&&!enabled[1])){
                        if(!enabled[0]&&!enabled[1]){int64_t end=slots[i].pending.pts_ms+(slots[i].pending.kind==NMEDIA_AUDIO?(int64_t)slots[i].pending.frames*1000/SOUND_RATE:40);memset(&slots[i].pending,0,sizeof slots[i].pending);slots[i].pending.kind=NMSW_CLOCK;slots[i].pending.pts_ms=end;break;}
                        slots[i].pending.kind=0;if(budget==15&&needed)busy=true;continue;}break;
                }
                if(!ok)break;if(!needed)continue;
                if(slots[i].pending.kind==NMEDIA_END){ended++;continue;}if(!slots[i].pending.kind)continue;
                if(video_full(&video)&&slots[i].pending.kind==NMEDIA_VIDEO)continue;
            }
            /* Scan candidates after every decoder has advanced: actual video
               EOF permits a legitimately longer audio tail, input/key waiting
               does not. Only a completely published shared image establishes
               the PCM horizon; an unpublished header still belongs here. */
            if(independent_video)for(size_t i=0;i<count;i++)if(slots[i].buffer){const struct nmedia_info *info=nmedia_mse_info(slots[i].buffer);if(info&&info->video&&!info->audio)video_ended=slots[i].pending.kind==NMEDIA_END;}
            for(size_t i=0;i<count;i++)if(slots[i].buffer&&slots[i].pending.kind&&slots[i].pending.kind!=NMEDIA_END){
                const struct nmedia_info *info=nmedia_mse_info(slots[i].buffer);if(!info)continue;
                bool needed=(!enabled[0]&&!enabled[1])||(info->audio&&enabled[0])||(info->video&&enabled[1]);if(!needed)continue;
                if(video_full(&video)&&slots[i].pending.kind==NMEDIA_VIDEO)continue;
                if(independent_video&&!video_ended&&slots[i].pending.kind==NMEDIA_AUDIO&&
                   (!video_delivered||slots[i].pending.pts_ms>video_delivered_pts))continue;
                if(selected<0||slots[i].pending.pts_ms<slots[selected].pending.pts_ms)selected=(int)i;
            }
            /* A ready owned VIDEO must not be suppressed by independent
               audio preroll/BUSY. PCM is retained above when beyond horizon;
               the VIDEO lane is exactly the progress that releases it. */
            if(ok&&selected>=0){r.slot=selected;output=slots[selected].pending;slots[selected].pending.kind=0;r.kind=output.kind;r.pts=output.pts_ms;r.frames=(uint32_t)output.frames;r.width=output.width;r.height=output.height;
                if(output.kind==NMEDIA_AUDIO){
                    const struct nmedia_info *info=nmedia_mse_info(slots[selected].buffer);
                    if(info&&info->audio&&!info->video){
                        size_t frames=output.frames;memcpy(pcm_batch,output.samples,frames*4);
                        /* One PCM response can carry a native 4096-frame sink
                           span instead of one round trip per AAC/Opus packet.
                           Preserve gaps, video horizon, pending decoder loans,
                           and bounded decode work; never concatenate timestamps. */
                        for(unsigned part=0;part<3&&frames<4096;part++){
                            struct nmedia_output *next=&slots[selected].pending;
                            if(!next->kind){int result=nmedia_mse_step_at(slots[selected].buffer,next,c.current);if(result==NMEDIA_ERROR){m=slots[selected].buffer;ok=false;break;}if(result==NMEDIA_AGAIN){memset(next,0,sizeof *next);break;}}
                            if(next->kind!=NMEDIA_AUDIO||next->frames>4096-frames||next->audio_observation.id!=output.audio_observation.id)break;
                            int64_t expected=output.pts_ms+(int64_t)frames*1000/SOUND_RATE;
                            if(next->pts_ms<expected-1||next->pts_ms>expected+1||
                               (independent_video&&!video_ended&&(!video_delivered||next->pts_ms>video_delivered_pts)))break;
                            memcpy(pcm_batch+frames*2,next->samples,next->frames*4);frames+=next->frames;output.audio_observation=next->audio_observation;next->kind=0;
                        }
                        output.samples=pcm_batch;output.frames=frames;output.duration_ms=(int64_t)frames*1000/SOUND_RATE;r.frames=(uint32_t)frames;
                    }
                    r.audio_observation=output.audio_observation;r.payload_bytes=(uint32_t)output.frames*4;
                }
                else if(output.kind==NMEDIA_VIDEO){
                    if(!nmedia_video_wire_bytes(output.width,output.height,&r.payload_bytes)){r.kind=NMEDIA_ERROR;strlcpy(r.error,"MSE video payload length is not representable",sizeof r.error);}
                    else {
                            if(video_full(&video)){
                                r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"MSE VIDEO owner transfer failed",sizeof r.error);
                            }else{
                                video.header=(struct nmsw_video){.magic=NMSW_MAGIC,.generation=generation,.sequence=sequence,.slot=(uint32_t)selected,
                                    .bytes=r.payload_bytes,.epoch=c.epoch,.revision=nmedia_mse_revision(slots[selected].buffer),.pts=output.pts_ms,.width=output.width,.height=output.height,
                                    .video_measured_frames=output.video_measured_frames,.video_work_ms=output.video_work_ms,
                                    .video_measurement_id=output.video_measurement_id,.video_configuration=output.video_configuration};
                                if(!video_publish(&video,&output,r.payload_bytes)){
                                    r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"MSE shared VIDEO allocation/publication failed",sizeof r.error);
                                }else{r.kind=NMSW_VIDEO_PENDING;r.payload_bytes=0;
                                /* A single coded image may intentionally span
                                   seconds. Use its real duration, not a fixed
                                   assumed frame rate or synthetic clock. */
                                int64_t duration=MAX((int64_t)1,output.duration_ms);
                                video.horizon_valid=true;
                                video.horizon_pts=output.pts_ms<=INT64_MAX-duration?output.pts_ms+duration:INT64_MAX;
                                }
                            }
                    }
                }}
            else if(ok&&active&&ended==active&&!video.pending){r.kind=NMEDIA_END;}
            else if(ok&&(busy||video.pending))r.kind=NMSW_BUSY;
        }
        if(c.op==NMSW_CHANGE||c.op==NMSW_ABORT||c.op==NMSW_REMOVE||c.op==NMSW_APPEND){
            const struct nmedia_info *info=nmedia_mse_info(m);video_mutation|=info&&info->video;
            if(video_mutation&&(c.op==NMSW_CHANGE||c.op==NMSW_ABORT||before_revision!=nmedia_mse_revision(m))){video_delivered=false;video.horizon_valid=false;}
        }
        nmedia_ff_free(bytes);
        if(!ok){r.kind=NMEDIA_ERROR;r.payload_bytes=0;r.quota_error|=nmedia_mse_quota_error(m);if(!r.error[0])strlcpy(r.error,m?nmedia_mse_error(m):"MSE command has no SourceBuffer",sizeof r.error);}
response:
        for(size_t i=0;i<count;i++)if(slots[i].buffer){uint64_t decoded,dropped;nmedia_mse_video_stats(slots[i].buffer,&decoded,&dropped);
            uint64_t dd=decoded-slots[i].reported_decoded,dr=dropped-slots[i].reported_dropped;
            if(dd>UINT32_MAX-r.decoded_frames||dr>UINT32_MAX-r.dropped_frames){r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"MSE video statistics representation overflow",sizeof r.error);}
            else{r.decoded_frames+=(uint32_t)dd;r.dropped_frames+=(uint32_t)dr;}
            slots[i].reported_decoded=decoded;slots[i].reported_dropped=dropped;}
        if(!metadata(slots,count,&r,&meta,&meta_capacity)){r.kind=NMEDIA_ERROR;r.payload_bytes=0;r.metadata_count=r.metadata_bytes=0;r.quota_error=1;strlcpy(r.error,"MSE metadata size/allocation failure",sizeof r.error);}
        /* One response message: the response, metadata, then PCM. VIDEO
           never rides here; its header follows on its own lane. */
        if(r.payload_bytes&&output.kind!=NMEDIA_AUDIO){r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"MSE payload without PCM",sizeof r.error);}
        size_t total=sizeof r+(size_t)r.metadata_bytes+r.payload_bytes;
        if(total>out_capacity){uint8_t *grown=nmedia_ff_realloc(out,total);if(grown){out=grown;out_capacity=total;}
            else{r.kind=NMEDIA_ERROR;r.payload_bytes=0;r.metadata_count=r.metadata_bytes=0;r.quota_error=1;strlcpy(r.error,"MSE response allocation failure",sizeof r.error);total=sizeof r;}}
        n_result sent;
        if(total==sizeof r)sent=n_channel_send(CONTROL,&r,sizeof r,-1);
        else{memcpy(out,&r,sizeof r);memcpy(out+sizeof r,meta,r.metadata_bytes);
            if(r.payload_bytes)memcpy(out+sizeof r+r.metadata_bytes,output.samples,r.payload_bytes);
            sent=n_channel_send(CONTROL,out,total,-1);}
        if(sent<0||!video_send(&video))break;
    }
    {volatile uint8_t *wipe=keys;for(size_t i=0;i<key_bytes;i++)wipe[i]=0;}nmedia_ff_free(keys);
    for(size_t i=0;i<count;i++)nmedia_mse_close(slots[i].buffer);nmedia_ff_free(slots);nmedia_ff_free(meta);nmedia_ff_free(out);
    video_storage_free(video.storage);while(video.retired){struct video_storage *s=video.retired;video.retired=s->next;video_storage_free(s);}
    return 0;
}
