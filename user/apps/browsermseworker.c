/* Nocturne native pipe worker: page-fetch bytes only, no FFmpeg URL protocol. */
#include "media_mse_worker_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
static bool exact(int fd,void *bytes,size_t size,bool writing){
    size_t at=0;while(at<size){ssize_t n=writing?write(fd,(uint8_t *)bytes+at,size-at):read(fd,(uint8_t *)bytes+at,size-at);
        if(n<0&&errno==EINTR)continue;if(n<=0)return false;at+=(size_t)n;}return true;
}
int main(void){
    if(!nmedia_alloc_restrict_worker())return 1;
    nmedia_mse *buffers[2]={0};struct nmedia_output pending[2]={{0}};
    uint32_t generation=0,sequence=0;bool established=false,enabled[2]={true,true};
    for(;;){
        struct nmsw_command c;if(!exact(0,&c,sizeof c,false))break;
        if(c.magic!=NMSW_MAGIC||c.reserved||c.slot>1||c.flags>1||c.bytes>NMEDIA_MAX_BYTES||c.op<NMSW_ADD||c.op>NMSW_SELECT||c.current<0||c.current>1000000000000LL||
           (c.bytes&&c.op!=NMSW_ADD&&c.op!=NMSW_CHANGE&&c.op!=NMSW_APPEND)||
           (established&&(c.generation!=generation||c.sequence!=sequence+1)))break;
        generation=c.generation;sequence=c.sequence;established=true;
        struct nmsw_response r={.magic=NMSW_MAGIC,.op=c.op,.generation=generation,.sequence=sequence};
        bool ok=true;unsigned slot=c.slot;nmedia_mse *m=buffers[slot];struct nmedia_output output={0};
        uint8_t *bytes=NULL;if(c.bytes){bytes=nmedia_ff_malloc((size_t)c.bytes+1);if(!bytes){
                uint8_t drain[4096];size_t left=c.bytes;while(left){size_t n=MIN(left,sizeof drain);if(!exact(0,drain,n,false))goto shutdown;left-=n;}
                r.kind=NMEDIA_ERROR;r.quota_error=1;strlcpy(r.error,"MSE child append allocation quota",sizeof r.error);goto response;
            }
            if(!exact(0,bytes,c.bytes,false)){nmedia_ff_free(bytes);break;}bytes[c.bytes]=0;}
        if(c.op==NMSW_ADD||c.op==NMSW_CHANGE){
            uint64_t revision=nmedia_mse_revision(m);
            if(!bytes||!c.bytes||c.bytes>=256||memchr(bytes,0,c.bytes)){ok=false;strlcpy(r.error,"invalid MSE MIME wire",sizeof r.error);}
            else if(c.op==NMSW_ADD){if(m)ok=false;else buffers[slot]=m=nmedia_mse_create((char *)bytes,r.error,sizeof r.error);ok=m!=NULL;}
            else ok=m&&nmedia_mse_change_type(m,(char *)bytes);
            if(revision!=nmedia_mse_revision(m))pending[slot].kind=0;
        }else if(c.op==NMSW_APPEND){uint64_t revision=nmedia_mse_revision(m);ok=m&&nmedia_mse_append(m,bytes,c.bytes,c.offset,c.start,c.end,c.flags);if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);pending[slot].kind=0;}}
        else if(c.op==NMSW_REMOVE){uint64_t revision=nmedia_mse_revision(m);ok=m&&nmedia_mse_remove(m,c.start,c.end);if(revision!=nmedia_mse_revision(m)){nmedia_mse_seek(m,c.current);pending[slot].kind=0;}}
        else if(c.op==NMSW_ABORT){if(m){nmedia_mse_abort(m);if(nmedia_mse_info(m))nmedia_mse_seek(m,c.current);}pending[slot].kind=0;}
        else if(c.op==NMSW_DROP){nmedia_mse_close(m);buffers[slot]=m=NULL;pending[slot].kind=0;}
        else if(c.op==NMSW_END){for(int i=0;i<2;i++)if(buffers[i]){uint64_t revision=nmedia_mse_revision(buffers[i]);if(!nmedia_mse_end(buffers[i],c.flags)){m=buffers[i];ok=false;break;}if(revision!=nmedia_mse_revision(buffers[i])||(!c.flags&&pending[i].kind==NMEDIA_END))pending[i].kind=0;}}
        else if(c.op==NMSW_SEEK){for(int i=0;i<2;i++){pending[i].kind=0;if(buffers[i]&&nmedia_mse_info(buffers[i])&&!nmedia_mse_seek(buffers[i],c.start)){m=buffers[i];ok=false;break;}}}
        else if(c.op==NMSW_SELECT)enabled[c.slot]=c.flags;
        else if(c.op==NMSW_STEP){
            bool waiting=false,busy=false;int selected=-1,active=0,ended=0;
            for(int i=0;i<2;i++)if(buffers[i]){
                const struct nmedia_info *info=nmedia_mse_info(buffers[i]);bool needed=(!enabled[0]&&!enabled[1])||!info||(info->audio&&enabled[0])||(info->video&&enabled[1]);if(needed)active++;
                for(int budget=0;budget<16;budget++){
                    if(!pending[i].kind){int result=nmedia_mse_step(buffers[i],&pending[i]);if(result==NMEDIA_ERROR){m=buffers[i];ok=false;break;}if(result==NMEDIA_AGAIN){if(needed)waiting=true;break;}}
                    if((pending[i].kind==NMEDIA_AUDIO&&!enabled[0])||(pending[i].kind==NMEDIA_VIDEO&&!enabled[1])){
                        if(!enabled[0]&&!enabled[1]){int64_t end=pending[i].pts_ms+(pending[i].kind==NMEDIA_AUDIO?(int64_t)pending[i].frames*1000/SOUND_RATE:40);memset(&pending[i],0,sizeof pending[i]);pending[i].kind=NMSW_CLOCK;pending[i].pts_ms=end;break;}
                        pending[i].kind=0;if(budget==15)busy=true;continue;}break;
                }
                if(!ok)break;if(!needed)continue;
                if(pending[i].kind==NMEDIA_END){ended++;continue;}if(!pending[i].kind)continue;if(selected<0||pending[i].pts_ms<pending[selected].pts_ms)selected=i;
            }
            if(ok&&!waiting&&selected>=0){r.slot=selected;output=pending[selected];pending[selected].kind=0;r.kind=output.kind;r.pts=output.pts_ms;r.frames=(uint32_t)output.frames;r.width=output.width;r.height=output.height;
                r.payload_bytes=output.kind==NMEDIA_AUDIO?(uint32_t)output.frames*4:output.kind==NMEDIA_VIDEO?(uint32_t)output.width*output.height*4:0;}
            else if(ok&&active&&ended==active){r.kind=NMEDIA_END;}
            else if(ok&&busy&&!waiting)r.kind=NMSW_BUSY;
        }
        nmedia_ff_free(bytes);
        if(!ok){r.kind=NMEDIA_ERROR;r.quota_error=nmedia_mse_quota_error(m);if(!r.error[0])strlcpy(r.error,m?nmedia_mse_error(m):"MSE command has no SourceBuffer",sizeof r.error);}
response:
        for(int i=0;i<2;i++){
            r.revision[i]=nmedia_mse_revision(buffers[i]);r.parsing[i]=nmedia_mse_parsing(buffers[i]);r.group_end[i]=nmedia_mse_group_end(buffers[i]);
            r.quota[i]=(uint32_t)nmedia_mse_quota(buffers[i]);r.counts[i]=(uint32_t)nmedia_mse_ranges(buffers[i],r.ranges[i],64);
            const struct nmedia_info *info=nmedia_mse_info(buffers[i]);if(!info)continue;
            r.info[i]=(struct nmsw_info){.audio=info->audio,.video=info->video,.channels=(uint32_t)info->channels,.sample_rate=(uint32_t)info->sample_rate,.width=info->width,.height=info->height,.duration=nmedia_mse_duration(buffers[i])};
            strlcpy(r.info[i].audio_codec,info->audio_codec,32);strlcpy(r.info[i].video_codec,info->video_codec,32);strlcpy(r.info[i].container,info->container,32);
        }
        if((r.info[0].audio&&r.info[1].audio)||(r.info[0].video&&r.info[1].video)){r.kind=NMEDIA_ERROR;r.payload_bytes=0;strlcpy(r.error,"overlapping MSE track ownership",sizeof r.error);}
        if(!exact(1,&r,sizeof r,true))break;
        if(r.payload_bytes&&!exact(1,(void *)(output.kind==NMEDIA_AUDIO?(const void *)output.samples:(const void *)output.pixels),r.payload_bytes,true))break;
    }
shutdown:
    for(int i=0;i<2;i++)nmedia_mse_close(buffers[i]);return 0;
}
