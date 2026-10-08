/* Native MSE: complete initialization/fragment spans -> encoded track buffer
 * -> persistent FFmpeg codec. Never concatenate/redecode the entire history.
 * All input is page-fetched bytes; there is no FFmpeg URL/POSIX protocol. */
#include "media_mse_private.h"
#include "media_mse_parser.h"
#include "media_feed_private.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include "libavutil/dict.h"

struct coded_frame {
    struct coded_frame *next;
    AVPacket *packet;
    int64_t start,end,dts;
    bool video,key;
};
struct nmedia_mse {
    nmedia *decoder;
    struct coded_frame *frames,*tail,*cursor;
    struct coded_frame *seek_key;
    uint8_t *init,*staging;
    size_t init_size,staged,capacity,packet_bytes,frame_count;
    int64_t end_ms,play_ms,offset,window0,window1,sequence_start,init_duration;
    bool webm,ended,quota_error,video_key,sequence,sequence_group;
    char mime[256],error[160];
    uint64_t revision;
};
static bool bad(nmedia_mse *m,const char *message,bool quota){
    strlcpy(m->error,message,sizeof m->error);m->quota_error=quota;return false;
}
bool nmedia_mse_type(const char *type){
    if(!type||!nmedia_can_play_type(type)[0])return false;
    char mime[256];if(strlen(type)>=sizeof mime)return false;strlcpy(mime,type,sizeof mime);
    char *semi=strchr(mime,';');if(semi)*semi=0;
    size_t n=strlen(mime);while(n&&(mime[n-1]==' '||mime[n-1]=='\t'))mime[--n]=0;
    return !strcasecmp(mime,"video/mp4")||!strcasecmp(mime,"audio/mp4")||
           !strcasecmp(mime,"video/webm")||!strcasecmp(mime,"audio/webm");
}
nmedia_mse *nmedia_mse_create(const char *type,char *error,size_t size){
    if(!nmedia_mse_type(type)){if(error&&size)strlcpy(error,"unsupported MSE byte stream or codec",size);return NULL;}
    nmedia_mse *m=nmedia_ff_mallocz(sizeof *m);
    if(!m){if(error&&size)strlcpy(error,"MSE control quota",size);return NULL;}
    strlcpy(m->mime,type,sizeof m->mime);m->webm=!strncasecmp(type,"video/webm",10)||!strncasecmp(type,"audio/webm",10);m->video_key=true;m->window1=INT64_MAX;m->revision=1;m->init_duration=-1;
    if(error&&size)*error=0;return m;
}
const char *nmedia_mse_error(const nmedia_mse *m){return m?m->error:"no MSE source buffer";}
bool nmedia_mse_quota_error(const nmedia_mse *m){return m&&m->quota_error;}
size_t nmedia_mse_quota(const nmedia_mse *m){
    if(!m)return 0;size_t used=m->init_size+m->staged+m->packet_bytes;
    return used>=NMEDIA_MAX_BYTES?0:NMEDIA_MAX_BYTES-used;
}
const struct nmedia_info *nmedia_mse_info(const nmedia_mse *m){return m?nmedia_get_info(m->decoder):NULL;}
uint64_t nmedia_mse_revision(const nmedia_mse *m){return m?m->revision:0;}
bool nmedia_mse_parsing(const nmedia_mse *m){return m&&m->staged!=0;}
int64_t nmedia_mse_group_end(const nmedia_mse *m){return m?m->end_ms:0;}
int64_t nmedia_mse_duration(const nmedia_mse *m){const struct nmedia_info *info=nmedia_mse_info(m);return !m?-1:m->init_duration>=0?m->init_duration:info?info->duration_ms:-1;}
struct fragment_input {const uint8_t *init,*body;size_t init_size,body_size,at;};
static int fragment_read(void *opaque,uint8_t *out,int count){
    struct fragment_input *p=opaque;if(count<=0)return AVERROR(EINVAL);
    size_t total=p->init_size+p->body_size,n=MIN((size_t)count,total-p->at);
    if(!n)return AVERROR_EOF;
    size_t copied=0;if(p->at<p->init_size){copied=MIN(n,p->init_size-p->at);memcpy(out,p->init+p->at,copied);p->at+=copied;}
    if(copied<n){memcpy(out+copied,p->body+(p->at-p->init_size),n-copied);p->at+=n-copied;}
    return (int)n;
}
static int64_t fragment_seek(void *opaque,int64_t offset,int whence){
    struct fragment_input *p=opaque;int64_t size=(int64_t)(p->init_size+p->body_size);
    if(whence&AVSEEK_SIZE)return size;whence&=~AVSEEK_FORCE;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)p->at:whence==SEEK_END?size:-1;
    if(base<0||offset< -base||offset>size-base)return AVERROR(EINVAL);p->at=(size_t)(base+offset);return (int64_t)p->at;
}
static int packet_read(void *opaque,AVPacket *out){
    nmedia_mse *m=opaque;
    while(m->cursor){struct coded_frame *f=m->cursor;m->cursor=f->next;
        if(m->seek_key&&f->video&&f!=m->seek_key)continue;
        if(f==m->seek_key)m->seek_key=NULL;m->play_ms=f->start;return av_packet_ref(out,f->packet);
    }return m->ended?AVERROR_EOF:AVERROR(EAGAIN);
}
static bool packet_seek(void *opaque,int64_t ms){
    nmedia_mse *m=opaque;struct coded_frame *start=NULL;
    bool video=nmedia_mse_info(m)&&nmedia_mse_info(m)->video;
    for(struct coded_frame *f=m->frames;f;f=f->next){
        if(f->start<=ms&&(!video||(f->video&&f->key))&&(!start||f->start>=start->start))start=f;
    }
    if(!start){for(start=m->frames;start&&video&&(!start->video||!start->key);start=start->next);}
    /* Include interleaved audio preceding the selected video keyframe. */
    m->seek_key=video?start:NULL;
    if(start&&video)for(struct coded_frame *f=m->frames;f&&f!=start;f=f->next)if(!f->video&&f->end>start->start){start=f;break;}
    m->cursor=start;m->play_ms=ms;return true; /* Empty buffers are seekable starvation, not EOF. */
}
static void release_frame(nmedia_mse *m,struct coded_frame *f){
    if(m->seek_key==f)m->seek_key=NULL;
    m->packet_bytes-=(size_t)f->packet->size;m->frame_count--;av_packet_free(&f->packet);nmedia_ff_free(f);
}
static void insert_frame(nmedia_mse *m,struct coded_frame *f){
    if(!m->frames){m->frames=m->tail=f;m->cursor=f;return;}
    if(f->dts>=m->tail->dts){m->tail->next=f;m->tail=f;if(!m->cursor)m->cursor=f;return;}
    struct coded_frame **p=&m->frames;while(*p&&(*p)->dts<=f->dts)p=&(*p)->next;
    f->next=*p;*p=f;if((!m->cursor||f->dts<m->cursor->dts)&&f->start>=m->play_ms)m->cursor=f;
}
static bool encrypted(const AVFormatContext *format){
    for(unsigned i=0;i<format->nb_streams;i++){
        AVStream *s=format->streams[i];
        if(av_dict_get(s->metadata,"enc_key_id",NULL,0))return true;
        AVCodecParameters *p=s->codecpar;
        for(int j=0;j<p->nb_coded_side_data;j++)if(p->coded_side_data[j].type==AV_PKT_DATA_ENCRYPTION_INIT_INFO||p->coded_side_data[j].type==AV_PKT_DATA_ENCRYPTION_INFO)return true;
    }return false;
}
static bool demux(nmedia_mse *m,const uint8_t *body,size_t bytes,int64_t offset,int64_t window0,int64_t window1,bool sequence){
    const uint8_t empty_cluster[]={0x1f,0x43,0xb6,0x75,0x80};
    struct fragment_input input={m->init,body,m->init_size,bytes,0};
    if(!bytes&&m->webm){input.body=empty_cluster;input.body_size=sizeof empty_cluster;}
    uint8_t *buffer=av_malloc(32768);AVIOContext *io=buffer?avio_alloc_context(buffer,32768,0,&input,fragment_read,NULL,fragment_seek):NULL;
    AVFormatContext *format=io?avformat_alloc_context():NULL;AVPacket *packet=NULL;
    bool ok=false,created=false,need_key=m->video_key;struct coded_frame *new_frames=NULL,*new_tail=NULL;size_t new_bytes=0,new_count=0;int64_t shift=offset,first=INT64_MIN;
    if(!format){bad(m,"MSE demux allocation quota",true);goto done;}
    format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->max_streams=8;format->probesize=1024*1024;
    int r=avformat_open_input(&format,NULL,av_find_input_format(m->webm?"matroska":"mov"),NULL);
    if(r<0||!format||!format->nb_streams||format->nb_streams>8){bad(m,"invalid MSE initialization/fragment",false);goto done;}
    if(encrypted(format)){bad(m,"encrypted MSE media is unsupported",false);goto done;}
    if(!m->decoder){m->decoder=nmedia_open_packets(format,packet_read,packet_seek,m,m->error,sizeof m->error);if(!m->decoder)goto done;created=true;}
    else if(!nmedia_packets_compatible(m->decoder,format)){bad(m,"MSE track/codec configuration changed without changeType",false);goto done;}
    const struct nmedia_info *info=nmedia_mse_info(m);
    unsigned audio_tracks=0,video_tracks=0;
    for(unsigned i=0;i<format->nb_streams;i++){audio_tracks+=format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO;video_tracks+=format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO;}
    if(audio_tracks!=(unsigned)info->audio||video_tracks!=(unsigned)info->video){bad(m,"unsupported MSE duplicate audio/video tracks",false);goto done;}
    packet=av_packet_alloc();if(!packet){bad(m,"MSE packet allocation quota",true);goto done;}
    for(unsigned budget=0;budget<65536;budget++){
        r=av_read_frame(format,packet);if(r==AVERROR_EOF){ok=true;break;}
        if(r<0){bad(m,"invalid complete MSE fragment packet",false);goto done;}
        if(packet->stream_index<0||(unsigned)packet->stream_index>=format->nb_streams||packet->size<0||packet->size>16*1024*1024){bad(m,"invalid MSE packet limits",false);goto done;}
        if(av_packet_get_side_data(packet,AV_PKT_DATA_ENCRYPTION_INFO,NULL)||av_packet_get_side_data(packet,AV_PKT_DATA_ENCRYPTION_INIT_INFO,NULL)){bad(m,"encrypted MSE packet is unsupported",false);goto done;}
        AVStream *stream=format->streams[packet->stream_index];bool video=stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO;
        if(stream->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO&&!video){av_packet_unref(packet);continue;}
        int64_t stamp=packet->pts==AV_NOPTS_VALUE?packet->dts:packet->pts;
        if(stamp==AV_NOPTS_VALUE){bad(m,"MSE packet has no timestamp",false);goto done;}
        int64_t pts=av_rescale_q(stamp,stream->time_base,(AVRational){1,1000});
        if(pts< -1000000000000LL||pts>1000000000000LL){bad(m,"MSE timestamp outside supported range",false);goto done;}
        if(first==INT64_MIN){first=pts;if(sequence)shift=(m->sequence_group?m->sequence_start:m->end_ms)-pts;}
        if((shift>0&&pts>INT64_MAX-shift)||(shift<0&&pts<INT64_MIN-shift)){bad(m,"MSE timestamp overflow",false);goto done;}
        pts+=shift;int64_t duration=av_rescale_q(packet->duration,stream->time_base,(AVRational){1,1000});
        if(duration<=0)duration=video?40:MAX(1,1024*1000/MAX(1,stream->codecpar->sample_rate));
        if(pts< -1000000000000LL||pts>1000000000000LL||duration>1000000){bad(m,"MSE coded frame time limit",false);goto done;}
        /* Matroska Opus codec delay can give the first encoded packet a
         * negative PTS, while decoder pre-skip yields a frame starting at 0. */
        int64_t filter_pts=!video&&window0==0&&pts<0&&pts+duration>0?0:pts;
        if(filter_pts<window0||filter_pts>=window1||duration>window1-filter_pts){if(video)need_key=true;av_packet_unref(packet);continue;}
        bool key=(packet->flags&AV_PKT_FLAG_KEY)!=0;
        if(video&&need_key&&!key){av_packet_unref(packet);continue;}if(video)need_key=false;
        if((size_t)packet->size>nmedia_mse_quota(m)-MIN(nmedia_mse_quota(m),new_bytes)){bad(m,"MSE encoded input quota",true);goto done;}
        if(m->frame_count+new_count>=65536){bad(m,"MSE coded frame count quota",true);goto done;}
        struct coded_frame *f=nmedia_ff_mallocz(sizeof *f);if(!f){bad(m,"MSE track buffer quota",true);goto done;}
        f->packet=av_packet_clone(packet);if(!f->packet){nmedia_ff_free(f);bad(m,"MSE packet copy quota",true);goto done;}
        int64_t delta=av_rescale_q(shift,(AVRational){1,1000},stream->time_base);
        if((delta>0&&((f->packet->pts!=AV_NOPTS_VALUE&&f->packet->pts>INT64_MAX-delta)||(f->packet->dts!=AV_NOPTS_VALUE&&f->packet->dts>INT64_MAX-delta)))||
           (delta<0&&((f->packet->pts!=AV_NOPTS_VALUE&&f->packet->pts<INT64_MIN-delta)||(f->packet->dts!=AV_NOPTS_VALUE&&f->packet->dts<INT64_MIN-delta)))){av_packet_free(&f->packet);nmedia_ff_free(f);bad(m,"MSE packet timestamp overflow",false);goto done;}
        if(f->packet->pts!=AV_NOPTS_VALUE)f->packet->pts+=delta;if(f->packet->dts!=AV_NOPTS_VALUE)f->packet->dts+=delta;
        int64_t dts=packet->dts==AV_NOPTS_VALUE?pts-shift:av_rescale_q(packet->dts,stream->time_base,(AVRational){1,1000});
        if(dts< -1000000000000LL||dts>1000000000000LL||(shift>0&&dts>INT64_MAX-shift)||(shift<0&&dts<INT64_MIN-shift)){av_packet_free(&f->packet);nmedia_ff_free(f);bad(m,"MSE packet DTS outside supported range",false);goto done;}
        f->start=filter_pts;f->end=pts+duration;f->dts=dts+shift;
        f->video=video;f->key=key;new_bytes+=(size_t)packet->size;new_count++;
        if(new_tail)new_tail->next=f;else new_frames=f;new_tail=f;av_packet_unref(packet);
    }
    if(!ok)bad(m,"MSE fragment packet budget exceeded",false);
done:
    if(ok){
        bool initial=!m->frames,replaced=false;int64_t lo[2]={INT64_MAX,INT64_MAX},hi[2]={INT64_MIN,INT64_MIN};
        for(struct coded_frame *f=new_frames;f;f=f->next){int t=f->video;lo[t]=MIN(lo[t],f->start);hi[t]=MAX(hi[t],f->end);}
        struct coded_frame **link=&m->frames;bool need_gop=false;m->tail=NULL;
        while(*link){struct coded_frame *f=*link;int t=f->video;bool drop=f->start>=lo[t]&&f->start<hi[t];
            if(f->video&&drop)need_gop=true;if(f->video&&need_gop){if(f->start>=hi[1]&&f->key)need_gop=false;else drop=true;}
            if(drop){if(m->cursor==f)m->cursor=f->next;*link=f->next;release_frame(m,f);replaced=true;}
            else {m->tail=f;link=&f->next;}}
        m->video_key=need_key;if(first!=INT64_MIN&&sequence)m->sequence_group=false;
        while(new_frames){struct coded_frame *f=new_frames;new_frames=f->next;f->next=NULL;m->packet_bytes+=(size_t)f->packet->size;m->frame_count++;m->end_ms=MAX(m->end_ms,f->end);insert_frame(m,f);}
        m->end_ms=0;for(struct coded_frame *f=m->frames;f;f=f->next)m->end_ms=MAX(m->end_ms,f->end);
        if(initial)m->cursor=m->frames;else if(replaced)nmedia_mse_seek(m,m->play_ms);
        if(nmedia_packets_resume(m->decoder))m->revision++;
    }else {while(new_frames){struct coded_frame *f=new_frames;new_frames=f->next;av_packet_free(&f->packet);nmedia_ff_free(f);}if(created){nmedia_close(m->decoder);m->decoder=NULL;}}
    av_packet_free(&packet);avformat_close_input(&format);if(io){av_freep(&io->buffer);avio_context_free(&io);}else av_free(buffer);
    return ok;
}
static bool process(nmedia_mse *m,int64_t offset,int64_t window0,int64_t window1,bool sequence,bool eos){
    for(int budget=0;budget<1024;budget++){
        size_t init=0,end=0;int r=nmedia_mse_boundary(m->staging,m->staged,m->webm,m->init!=NULL,eos,&init,&end,m->error,sizeof m->error);
        if(r<0)return false;if(!r)return true;
        if(init){if(m->init||init>m->staged||init>1024*1024)return bad(m,"MSE initialization limit",false);
            m->init=nmedia_ff_malloc(init);if(!m->init)return bad(m,"MSE initialization quota",true);
            memcpy(m->init,m->staging,init);m->init_size=init;
            if(!demux(m,NULL,0,offset,window0,window1,false)){nmedia_ff_free(m->init);m->init=NULL;m->init_size=0;return false;}
            m->init_duration=nmedia_mse_init_duration(m->init,m->init_size,m->webm);
            memmove(m->staging,m->staging+init,m->staged-init);m->staged-=init;continue;}
        if(!end||end>m->staged)return bad(m,"invalid MSE parser boundary",false);
        /* Replace staged encoded bytes with packet references transactionally.
         * They are not simultaneously counted twice against the input quota. */
        size_t staged=m->staged;m->staged-=end;
        bool ok=demux(m,m->staging,end,offset,window0,window1,sequence);
        m->staged=staged;if(!ok)return false;
        memmove(m->staging,m->staging+end,m->staged-end);m->staged-=end;
    }return bad(m,"MSE segment parser budget exceeded",false);
}
bool nmedia_mse_append(nmedia_mse *m,const void *bytes,size_t count,int64_t offset,int64_t window0,int64_t window1,bool sequence){
    if(!m||(!bytes&&count))return false;m->error[0]=0;m->quota_error=false;
    if(offset< -1000000000000LL||offset>1000000000000LL||window0<0||window1<=window0)return bad(m,"invalid MSE append time properties",false);
    if(m->staged&&(offset!=m->offset||window0!=m->window0||window1!=m->window1||sequence!=m->sequence))return bad(m,"abort partial segment before changing append properties",false);
    if(sequence&&(!m->sequence||offset!=m->offset||!m->frames)){m->sequence_group=true;m->sequence_start=offset;}
    m->offset=offset;m->window0=window0;m->window1=window1;m->sequence=sequence;
    if(count>nmedia_mse_quota(m))return bad(m,"MSE encoded input exceeds 32 MiB",true);
    if(m->staged+count>m->capacity){size_t cap=MIN(NMEDIA_MAX_BYTES,MAX(m->staged+count,(size_t)4096));
        uint8_t *p=nmedia_ff_realloc(m->staging,cap);if(!p)return bad(m,"MSE append allocation quota",true);m->staging=p;m->capacity=cap;}
    if(count)memcpy(m->staging+m->staged,bytes,count);m->staged+=count;m->ended=false;
    return process(m,offset,window0,window1,sequence,false);
}
int nmedia_mse_step(nmedia_mse *m,struct nmedia_output *out){
    if(!m||m->error[0])return NMEDIA_ERROR;if(!m->decoder)return m->ended?NMEDIA_END:NMEDIA_AGAIN;
    int r=nmedia_step(m->decoder,out);if(r==NMEDIA_ERROR)strlcpy(m->error,nmedia_error(m->decoder),sizeof m->error);return r;
}
bool nmedia_mse_seek(nmedia_mse *m,int64_t ms){if(!m||!m->decoder||!nmedia_seek(m->decoder,ms))return false;m->revision++;return true;}
bool nmedia_mse_remove(nmedia_mse *m,int64_t start,int64_t end){
    if(!m||start<0||end<=start)return false;struct coded_frame **p=&m->frames;bool need_key=false,changed=false;m->tail=NULL;
    while(*p){struct coded_frame *f=*p;bool drop=f->start>=start&&f->start<end;
        if(f->video&&drop)need_key=true;if(f->video&&need_key){if(f->start>=end&&f->key)need_key=false;else drop=true;}
        if(drop){if(m->cursor==f)m->cursor=f->next;*p=f->next;release_frame(m,f);changed=true;}
        else {m->tail=f;p=&f->next;}}
    m->end_ms=0;for(struct coded_frame *f=m->frames;f;f=f->next)m->end_ms=MAX(m->end_ms,f->end);
    if(changed&&m->decoder)nmedia_mse_seek(m,m->play_ms);if(m->quota_error){m->error[0]=0;m->quota_error=false;}return true;
}
void nmedia_mse_abort(nmedia_mse *m){if(!m)return;m->staged=0;m->error[0]=0;m->quota_error=false;m->video_key=true;if(m->decoder)nmedia_mse_seek(m,m->play_ms);}
bool nmedia_mse_end(nmedia_mse *m,bool ended){
    if(!m)return false;m->error[0]=0;
    if(ended){if(!process(m,m->offset,m->window0,m->window1,m->sequence,true)||m->staged)return bad(m,"incomplete MSE segment at endOfStream",false);}
    m->ended=ended;if(!ended&&nmedia_packets_resume(m->decoder))m->revision++;return true;
}
bool nmedia_mse_change_type(nmedia_mse *m,const char *type){
    if(!m||!nmedia_mse_type(type))return false;
    if(!strcmp(m->mime,type)){nmedia_mse_abort(m);return true;}
    if(m->frames)return bad(m,"remove retained coded frames before changing MSE codec",false);
    nmedia_close(m->decoder);m->decoder=NULL;nmedia_ff_free(m->init);m->init=NULL;m->init_size=0;m->staged=0;m->init_duration=-1;m->revision++;
    strlcpy(m->mime,type,sizeof m->mime);m->webm=!strncasecmp(type,"video/webm",10)||!strncasecmp(type,"audio/webm",10);m->error[0]=0;m->video_key=true;return true;
}
static size_t track_ranges(nmedia_mse *m,bool video,struct nmedia_time_range *out,size_t maximum){
    if(!m||!out||!maximum)return 0;size_t n=0;
    for(struct coded_frame *f=m->frames;f;f=f->next){
        if(f->video!=video)continue;
        int64_t start=MAX(0,f->start),end=f->end;if(end<=start)continue;
        size_t at=0;while(at<n&&out[at].end_ms+1<start)at++;
        if(at<n&&out[at].start_ms<=end+1){out[at].start_ms=MIN(out[at].start_ms,start);out[at].end_ms=MAX(out[at].end_ms,end);
            while(at+1<n&&out[at+1].start_ms<=out[at].end_ms+1){out[at].end_ms=MAX(out[at].end_ms,out[at+1].end_ms);memmove(out+at+1,out+at+2,(n-at-2)*sizeof *out);n--;}}
        else if(n<maximum){memmove(out+at+1,out+at,(n-at)*sizeof *out);out[at]=(struct nmedia_time_range){start,end};n++;}
    }return n;
}
size_t nmedia_mse_ranges(nmedia_mse *m,struct nmedia_time_range *out,size_t maximum){
    const struct nmedia_info *info=nmedia_mse_info(m);if(!info||!maximum)return 0;
    if(!info->audio||!info->video)return track_ranges(m,info->video,out,maximum);
    struct nmedia_time_range a[64],v[64];size_t na=track_ranges(m,false,a,64),nv=track_ranges(m,true,v,64),n=0,i=0,j=0;
    while(i<na&&j<nv&&n<maximum){int64_t lo=MAX(a[i].start_ms,v[j].start_ms),hi=MIN(a[i].end_ms,v[j].end_ms);if(hi>lo)out[n++]=(struct nmedia_time_range){lo,hi};if(a[i].end_ms<v[j].end_ms)i++;else j++;}return n;
}
void nmedia_mse_close(nmedia_mse *m){if(!m)return;nmedia_close(m->decoder);while(m->frames){struct coded_frame *f=m->frames;m->frames=f->next;release_frame(m,f);}nmedia_ff_free(m->init);nmedia_ff_free(m->staging);nmedia_ff_free(m);}
