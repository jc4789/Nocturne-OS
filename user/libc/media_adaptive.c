/* Native clear adaptive VOD input -> custom AVIO -> persistent packet decoder.
 * All blocking HTTP/demux work stays in browsermediaworker. No upstream URL
 * protocol, network thread, CLI, foreign process ABI, or external player. */
#include "media_adaptive_private.h"
#include "media_http_private.h"
#include "media_feed_private.h"
#include "media_alloc_private.h"
#include "media_mse_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include "libavutil/dict.h"
#include "libavutil/mem.h"
struct adaptive_input {
    const uint8_t *init,*body;
    size_t init_bytes,body_bytes,at;
};
struct adaptive_track {
    struct adaptive_input input;
    AVFormatContext *format;
    AVIOContext *io;
    AVPacket *pending;
    uint8_t *init,*body;
    size_t init_bytes;
    unsigned next,index,packets;
    int map[8];
    int64_t shift_us;
    unsigned media_kinds;
    bool anchor,ended,need_key;
};
struct adaptive {
    struct nmedia_adaptive_manifest manifest;
    struct adaptive_track tracks[2];
    AVFormatContext *metadata;
    char document[NMEDIA_ADAPTIVE_URL],error[160];
};
static bool bad(struct adaptive *a,const char *message){
    snprintf(a->error,sizeof a->error,"%s",message);return false;
}
static const char *input_error(void *owner){return ((struct adaptive *)owner)->error;}
static int input_read(void *owner,uint8_t *out,int count){
    struct adaptive_input *p=owner;size_t total=p->init_bytes+p->body_bytes;
    if(count<=0||p->at>total)return AVERROR(EINVAL);
    size_t take=(size_t)count;if(take>total-p->at)take=total-p->at;if(!take)return AVERROR_EOF;
    size_t first=0;if(p->at<p->init_bytes){first=p->init_bytes-p->at;if(first>take)first=take;memcpy(out,p->init+p->at,first);p->at+=first;}
    if(first<take){memcpy(out+first,p->body+p->at-p->init_bytes,take-first);p->at+=take-first;}return (int)take;
}
static int64_t input_seek(void *owner,int64_t offset,int whence){
    struct adaptive_input *p=owner;int64_t size=(int64_t)(p->init_bytes+p->body_bytes);
    if(whence&AVSEEK_SIZE)return size;whence&=~AVSEEK_FORCE;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)p->at:whence==SEEK_END?size:-1;
    if(base<0||offset< -base||offset>size-base)return AVERROR(EINVAL);
    p->at=(size_t)(base+offset);return base+offset;
}
static int deny_external(AVFormatContext *f,AVIOContext **io,const char *url,int flags,AVDictionary **opts){
    (void)f;(void)io;(void)url;(void)flags;(void)opts;return AVERROR(EPERM);
}
static bool fetch(struct adaptive *a,const char *base,const char *uri,int64_t start,int64_t count,
                   size_t maximum,uint8_t **bytes,size_t *length){
    char url[NMEDIA_ADAPTIVE_URL];
    if(!nmedia_adaptive_resolve(base,uri,url,sizeof url))return bad(a,"invalid or mixed-content adaptive segment URL");
    if(!count)return nmedia_http_get_bounded_cors(url,a->document,maximum,bytes,length,a->error,sizeof a->error);
    if(start<0||count<1||(uint64_t)count>maximum||start>INT64_MAX-count)return bad(a,"adaptive byte range limit");
    nmedia_http *r=nmedia_http_open_cors(url,a->document,a->error,sizeof a->error);if(!r)return false;
    bool ok=false;uint8_t *b=NULL;
    if(start>nmedia_http_size(r)||count>nmedia_http_size(r)-start||nmedia_http_seek(r,start,SEEK_SET)<0){bad(a,"adaptive byte range exceeds resource");goto done;}
    b=nmedia_ff_malloc((size_t)count);if(!b){bad(a,"adaptive segment allocation quota");goto done;}
    size_t got=0;while(got<(size_t)count){
        int n=nmedia_http_read(r,b+got,(int)((size_t)count-got));
        if(n<=0){bad(a,nmedia_http_error(r)[0]?nmedia_http_error(r):"truncated adaptive segment range");goto done;}got+=(size_t)n;
    }
    *bytes=b;*length=(size_t)count;b=NULL;ok=true;
done:nmedia_ff_free(b);nmedia_http_close(r);return ok;
}
static void close_segment(struct adaptive_track *t){
    if(t->pending)av_packet_unref(t->pending);avformat_close_input(&t->format);
    if(t->io){av_freep(&t->io->buffer);avio_context_free(&t->io);}
    nmedia_ff_free(t->body);t->body=NULL;memset(&t->input,0,sizeof t->input);
}
static void release(void *owner){
    struct adaptive *a=owner;if(!a)return;
    for(unsigned i=0;i<2;i++){close_segment(&a->tracks[i]);av_packet_free(&a->tracks[i].pending);nmedia_ff_free(a->tracks[i].init);}
    avformat_free_context(a->metadata);nmedia_ff_free(a);
}
static bool clear_metadata(AVFormatContext *format){
    for(unsigned i=0;i<format->nb_streams;i++){
        AVStream *s=format->streams[i];if(av_dict_get(s->metadata,"enc_key_id",NULL,0))return false;
        AVCodecParameters *p=s->codecpar;
        for(int j=0;j<p->nb_coded_side_data;j++)if(p->coded_side_data[j].type==AV_PKT_DATA_ENCRYPTION_INIT_INFO||
           p->coded_side_data[j].type==AV_PKT_DATA_ENCRYPTION_INFO)return false;
    }return true;
}
static bool validate_mp4(struct adaptive *a,const uint8_t *bytes,size_t length,bool initialized){
    size_t at=0;for(unsigned i=0;i<1024&&at<length;i++){
        size_t init=0,span=0;int n=nmedia_mse_boundary(bytes+at,length-at,false,initialized,true,&init,&span,a->error,sizeof a->error);
        if(n<1||(!init&&!span))return false;
        if(init){if(initialized||span)return bad(a,"unexpected adaptive MP4 initialization");initialized=true;at+=init;}
        else at+=span;
    }return at==length||bad(a,"adaptive MP4 fragment count limit");
}
static bool compatible(const AVStream *s,const AVStream *d){
    const AVCodecParameters *p=s->codecpar,*q=d->codecpar;
    return p->codec_id==q->codec_id&&p->codec_type==q->codec_type&&
        p->extradata_size==q->extradata_size&&p->sample_rate==q->sample_rate&&
        p->width==q->width&&p->height==q->height&&!av_channel_layout_compare(&p->ch_layout,&q->ch_layout)&&
        (!p->extradata_size||!memcmp(p->extradata,q->extradata,(size_t)p->extradata_size));
}
static bool map_streams(struct adaptive *a,struct adaptive_track *t,bool first){
    AVFormatContext *f=t->format;
    if(!clear_metadata(f))return bad(a,"encrypted adaptive media metadata is unsupported");
    unsigned found=0;bool kinds[2]={false,false};
    for(unsigned i=0;i<8;i++)t->map[i]=-1;
    for(unsigned i=0;i<f->nb_streams;i++){
        AVStream *s=f->streams[i];AVCodecParameters *p=s->codecpar;
        bool audio=p->codec_type==AVMEDIA_TYPE_AUDIO,video=p->codec_type==AVMEDIA_TYPE_VIDEO;
        if(!audio&&!video)continue;
        if(kinds[video]||p->width<0||p->height<0||(uint64_t)p->width*p->height>NMEDIA_MAX_PIXELS||
           p->sample_rate>384000||p->ch_layout.nb_channels>8||s->time_base.num<=0||s->time_base.den<=0)
            return bad(a,"adaptive codec/track resource limit");
        if((audio&&p->codec_id!=AV_CODEC_ID_AAC)||(video&&p->codec_id!=AV_CODEC_ID_H264))
            return bad(a,"unsupported adaptive media codec");
        kinds[video]=true;found++;
        if(first){
            for(unsigned j=0;j<a->metadata->nb_streams;j++)if(a->metadata->streams[j]->codecpar->codec_type==p->codec_type)
                return bad(a,"duplicate adaptive audio/video track");
            AVStream *d=avformat_new_stream(a->metadata,NULL);
            if(!d||avcodec_parameters_copy(d->codecpar,p)<0)return bad(a,"adaptive codec metadata quota");
            d->time_base=(AVRational){1,1000000};d->avg_frame_rate=s->avg_frame_rate;t->map[i]=(int)d->index;
        }else{
            for(unsigned j=0;j<a->metadata->nb_streams;j++)if(a->metadata->streams[j]->codecpar->codec_type==p->codec_type){
                if(!compatible(s,a->metadata->streams[j]))return bad(a,"adaptive codec configuration changed");
                t->map[i]=(int)j;break;
            }
            if(t->map[i]<0)return bad(a,"adaptive media track changed");
        }
    }
    unsigned mask=(kinds[0]?1u:0u)|(kinds[1]?2u:0u);
    if(first){t->media_kinds=mask;t->need_key=kinds[1];}
    else if(t->media_kinds!=mask)return bad(a,"adaptive segment audio/video tracks changed");
    return found!=0||bad(a,"adaptive segment has no playable packets");
}
static bool open_segment(struct adaptive *a,unsigned i,bool first){
    struct adaptive_track *t=&a->tracks[i];struct nmedia_adaptive_track *plan=&a->manifest.tracks[i];
    if(t->next>=plan->count){t->ended=true;return true;}
    close_segment(t);t->index=t->next++;t->packets=0;
    struct nmedia_adaptive_segment *segment=&plan->segments[t->index];size_t bytes=0;
    if(!fetch(a,plan->base,segment->uri,segment->range_start,segment->range_bytes,
                NMEDIA_ADAPTIVE_SEGMENT_BYTES,&t->body,&bytes))return false;
    if(plan->mp4){
        if(!validate_mp4(a,t->body,bytes,true))return false;
    }else{
        if(!bytes||bytes%188)return bad(a,"unsupported adaptive transport packet framing");
        for(size_t at=0;at<bytes;at+=188)if(t->body[at]!=0x47||(t->body[at+3]&0xc0))
            return bad(a,"encrypted or malformed adaptive transport stream");
    }
    t->input=(struct adaptive_input){t->init,t->body,t->init_bytes,bytes,0};
    uint8_t *buffer=av_malloc(32768);t->io=buffer?avio_alloc_context(buffer,32768,0,&t->input,input_read,NULL,input_seek):NULL;
    if(!t->io){av_free(buffer);return bad(a,"adaptive AVIO quota");}
    t->format=avformat_alloc_context();if(!t->format)return bad(a,"adaptive demux quota");
    t->format->pb=t->io;t->format->flags|=AVFMT_FLAG_CUSTOM_IO;t->format->io_open=deny_external;
    t->format->max_streams=8;t->format->probesize=1024*1024;t->format->max_analyze_duration=2000000;
    const AVInputFormat *demuxer=av_find_input_format(plan->mp4?"mov":"mpegts");
    if(!demuxer)return bad(a,"adaptive segment demuxer is not built");
    int r=avformat_open_input(&t->format,NULL,demuxer,NULL);
    if(r<0||!t->format||!t->format->nb_streams||t->format->nb_streams>8)return bad(a,"invalid adaptive media segment");
    AVDictionary *options[8]={0};unsigned count=t->format->nb_streams;
    for(unsigned j=0;j<count;j++)if(av_dict_set(&options[j],"max_pixels","2359296",0)<0||
        av_dict_set(&options[j],"threads","1",0)<0){r=AVERROR(ENOMEM);goto probed;}
    r=avformat_find_stream_info(t->format,options);
probed:for(unsigned j=0;j<count;j++)av_dict_free(&options[j]);
    if(r<0)return bad(a,"adaptive segment stream information failed");
    if(!map_streams(a,t,first))return false;
    if(!t->anchor){
        if(a->manifest.dash)t->shift_us=-plan->presentation_offset_us;
        else{if(t->format->start_time==AV_NOPTS_VALUE)return bad(a,"HLS segment has no presentation origin");t->shift_us=-t->format->start_time;}
        t->anchor=true;
    }
    return true;
}
static bool refill_packet(struct adaptive *a,unsigned i){
    struct adaptive_track *t=&a->tracks[i];
    if(t->pending->size)return true;
    for(unsigned budget=0;budget<256;budget++){
        if(t->ended)return !t->need_key||bad(a,"adaptive video has no decodable keyframe");
        if(!t->format&&!open_segment(a,i,false))return false;
        if(t->ended)return !t->need_key||bad(a,"adaptive video has no decodable keyframe");
        int n=av_read_frame(t->format,t->pending);
        if(n==AVERROR_EOF){close_segment(t);continue;}
        if(n<0)return bad(a,"adaptive segment packet demux failed");
        if(++t->packets>65536||t->pending->size<=0||t->pending->size>16*1024*1024||
           t->pending->stream_index<0||(unsigned)t->pending->stream_index>=t->format->nb_streams)
            return bad(a,"adaptive packet resource limit");
        if(av_packet_get_side_data(t->pending,AV_PKT_DATA_ENCRYPTION_INFO,NULL)||
           av_packet_get_side_data(t->pending,AV_PKT_DATA_ENCRYPTION_INIT_INFO,NULL))
            return bad(a,"encrypted adaptive packet is unsupported");
        int source=t->pending->stream_index;if(t->map[source]<0){av_packet_unref(t->pending);continue;}
        AVStream *s=t->format->streams[source];
        if(s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO&&t->need_key){
            if(!(t->pending->flags&AV_PKT_FLAG_KEY)){av_packet_unref(t->pending);continue;}
            t->need_key=false;
        }
        if(t->pending->pts==AV_NOPTS_VALUE&&t->pending->dts==AV_NOPTS_VALUE)return bad(a,"adaptive packet has no timestamp");
        av_packet_rescale_ts(t->pending,s->time_base,(AVRational){1,1000000});
        int64_t *stamps[2]={&t->pending->pts,&t->pending->dts};
        for(unsigned k=0;k<2;k++)if(*stamps[k]!=AV_NOPTS_VALUE){
            if(*stamps[k]< -1000000000000000LL||*stamps[k]>1000000000000000LL||
               (t->shift_us>0&&*stamps[k]>INT64_MAX-t->shift_us)||(t->shift_us<0&&*stamps[k]<INT64_MIN-t->shift_us))
                return bad(a,"adaptive packet timestamp limit");
            *stamps[k]+=t->shift_us;
        }
        const struct nmedia_adaptive_segment *segment=&a->manifest.tracks[i].segments[t->index];
        int64_t pts=t->pending->pts==AV_NOPTS_VALUE?t->pending->dts:t->pending->pts;
        if(pts<segment->start_us-2000000||pts>segment->start_us+segment->duration_us+2000000)
            return bad(a,"adaptive packet disagrees with manifest timeline");
        t->pending->stream_index=t->map[source];return true;
    }return bad(a,"adaptive empty segment packet budget");
}
static int packet_read(void *owner,AVPacket *packet){
    struct adaptive *a=owner;if(a->error[0])return AVERROR(EIO);
    int selected=-1;int64_t earliest=INT64_MAX;
    for(unsigned i=0;i<a->manifest.count;i++){
        struct adaptive_track *t=&a->tracks[i];if(!refill_packet(a,i))return AVERROR(EIO);
        if(t->pending->size){int64_t at=t->pending->dts==AV_NOPTS_VALUE?t->pending->pts:t->pending->dts;
            if(selected<0||at<earliest){selected=(int)i;earliest=at;}}
    }
    if(selected<0)return AVERROR_EOF;av_packet_move_ref(packet,a->tracks[selected].pending);return 0;
}
static bool packet_seek(void *owner,int64_t ms){
    struct adaptive *a=owner;if(ms<0||ms>INT64_MAX/1000)return false;
    for(unsigned i=0;i<a->manifest.count;i++){
        struct adaptive_track *t=&a->tracks[i];struct nmedia_adaptive_track *p=&a->manifest.tracks[i];
        unsigned next=0;while(next+1<p->count&&p->segments[next+1].start_us<=ms*1000)next++;
        /* Start one segment earlier for bounded GOP preroll. The existing
         * decoder's seek trim suppresses audio/video before the requested time. */
        if(next)next--;
        close_segment(t);t->next=next;t->ended=false;t->need_key=(t->media_kinds&2u)!=0;
    }
    a->error[0]=0;return true;
}
bool nmedia_adaptive_url(const char *url){
    if(!url)return false;const char *end=url+strcspn(url,"?#");size_t n=(size_t)(end-url);
    return (n>=5&&!strncasecmp(end-5,".m3u8",5))||(n>=4&&!strncasecmp(end-4,".mpd",4));
}
nmedia *nmedia_adaptive_open_cors(const char *url,const char *document,char *error,size_t size){
    struct adaptive *a=nmedia_ff_mallocz(sizeof *a);nmedia *decoder=NULL;uint8_t *text=NULL;size_t bytes=0;
    if(!a){if(error&&size)snprintf(error,size,"adaptive manifest quota");return NULL;}
    if(url&&strchr(url,'#')){bad(a,"adaptive media URL fragments are unsupported");goto done;}
    if(!url||!document||strlen(document)>=sizeof a->document||!nmedia_http_browser_url(url,document)){
        bad(a,"invalid adaptive native document or mixed content");goto done;}
    memcpy(a->document,document,strlen(document)+1);
    char manifest_url[2048];if(strlen(url)>=sizeof manifest_url){bad(a,"adaptive URL size limit");goto done;}memcpy(manifest_url,url,strlen(url)+1);
    for(unsigned depth=0;depth<4;depth++){
        if(!nmedia_http_get_bounded_cors(manifest_url,document,NMEDIA_ADAPTIVE_MANIFEST_BYTES,&text,&bytes,a->error,sizeof a->error))goto done;
        /* Parser requires its own trailing NUL; embedded NUL remains invalid. */
        uint8_t *p=nmedia_ff_realloc(text,bytes+1);if(!p){bad(a,"adaptive manifest text quota");goto done;}text=p;text[bytes]=0;
        int result;
        if(bytes>=7&&!memcmp(text,"#EXTM3U",7))result=nmedia_adaptive_parse_hls((char *)text,bytes,manifest_url,&a->manifest,a->error,sizeof a->error);
        else result=nmedia_adaptive_parse_dash((char *)text,bytes,manifest_url,&a->manifest,a->error,sizeof a->error)?1:-1;
        nmedia_ff_free(text);text=NULL;
        if(result<0)goto done;if(result==1)break;
        char next[2048];if(depth==3||!nmedia_adaptive_resolve(manifest_url,a->manifest.variant,next,sizeof next)){bad(a,"HLS master recursion or URL limit");goto done;}
        memcpy(manifest_url,next,strlen(next)+1);
    }
    a->metadata=avformat_alloc_context();if(!a->metadata){bad(a,"adaptive metadata quota");goto done;}
    a->metadata->duration=a->manifest.duration_us;
    for(unsigned i=0;i<a->manifest.count;i++){
        struct adaptive_track *t=&a->tracks[i];struct nmedia_adaptive_track *plan=&a->manifest.tracks[i];
        t->pending=av_packet_alloc();if(!t->pending){bad(a,"adaptive packet quota");goto done;}
        if(plan->mp4){
            if(!fetch(a,plan->base,plan->init,plan->init_start,plan->init_bytes,NMEDIA_ADAPTIVE_INIT_BYTES,&t->init,&t->init_bytes)||
               !validate_mp4(a,t->init,t->init_bytes,false))goto done;
        }
        if(!open_segment(a,i,true))goto done;
    }
    decoder=nmedia_open_packets(a->metadata,packet_read,packet_seek,a,a->error,sizeof a->error);
    if(!decoder)goto done;nmedia_packets_set_lifecycle(decoder,release,input_error);
    if(error&&size)*error=0;return decoder;
done:if(error&&size)snprintf(error,size,"%s",a->error[0]?a->error:"invalid adaptive media");
    nmedia_ff_free(text);release(a);return NULL;
}

