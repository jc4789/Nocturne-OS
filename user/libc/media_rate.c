/* PCM tempo is a separate responsibility from packet decoding and GUI timing.
 * Pitch-preserving playback uses upstream LGPL WSOLA (atempo); pitch-changing
 * playback uses its native resampler by changing the source's sample rate. */
#include <errno.h>
#include "media_rate_private.h"
#include "media_alloc_private.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libswresample/swresample.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
struct nmedia_rate {
    AVFilterGraph *graph;
    AVFilterContext *source,*sink;
    AVFrame *frame;
    SwrContext *resampler;
    int16_t *converted;size_t converted_capacity,converted_frames;
    int16_t output[4096*2];
    size_t at;
    double rate,origin,emitted;
    int64_t fed;
    bool started,ended;
};
nmedia_rate *nmedia_rate_create(double rate,bool pitch) {
    if(!isfinite(rate)||rate<0.25||rate>4)return NULL;
    nmedia_rate *r=nmedia_ff_mallocz(sizeof *r);if(!r)return NULL;
    r->rate=rate;r->frame=av_frame_alloc();if(!r->frame)goto failed;
    if(!pitch){
        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        if(swr_alloc_set_opts2(&r->resampler,&stereo,AV_SAMPLE_FMT_S16,48000,&stereo,AV_SAMPLE_FMT_S16,(int)lround(48000*rate),0,NULL)<0||swr_init(r->resampler)<0)goto failed;
        return r;
    }
    r->graph=avfilter_graph_alloc();if(!r->graph)goto failed;
    /* No auto conversion: only native S16 stereo crosses this private lane. */
    avfilter_graph_set_auto_convert(r->graph,AVFILTER_AUTO_CONVERT_NONE);
    if(avfilter_graph_create_filter(&r->source,avfilter_get_by_name("abuffer"),"pcm","time_base=1/48000:sample_rate=48000:sample_fmt=s16:channel_layout=stereo",NULL,r->graph)<0||
       avfilter_graph_create_filter(&r->sink,avfilter_get_by_name("abuffersink"),"output",NULL,NULL,r->graph)<0)goto failed;
    AVFilterContext *previous=r->source;
    /* Cascading keeps every WSOLA stage in its quality range [0.5,2]. */
    double remaining=rate;
    for(unsigned i=0;;i++){
        double stage=remaining>2?2:remaining<0.5?0.5:remaining;
        char name[32],args[64];snprintf(name,sizeof name,"tempo%u",i);snprintf(args,sizeof args,"tempo=%.17g",stage);
        AVFilterContext *next=NULL;
        if(avfilter_graph_create_filter(&next,avfilter_get_by_name("atempo"),name,args,NULL,r->graph)<0||avfilter_link(previous,0,next,0)<0)goto failed;
        previous=next;remaining/=stage;if(fabs(remaining-1)<1e-10)break;
    }
    if(avfilter_link(previous,0,r->sink,0)<0||avfilter_graph_config(r->graph,NULL)<0)goto failed;
    return r;
failed:nmedia_rate_close(r);return NULL;
}
bool nmedia_rate_push(nmedia_rate *r,const struct nmedia_output *o){
    if(!r||r->ended||!o||o->kind!=NMEDIA_AUDIO||!o->samples||!o->frames||o->frames>4096)return false;
    if(!r->started){r->origin=o->pts_ms*48.0;r->started=true;}
    if(r->resampler){
        /* Only EOS may pass NULL input to swr_convert: flushing between
           ordinary decoded frames would repeat filter tails and click. */
        if(r->at<r->converted_frames)return false;
        int capacity=swr_get_out_samples(r->resampler,(int)o->frames);if(capacity<0)return false;
        if((size_t)capacity>r->converted_capacity){int16_t *v=nmedia_ff_realloc(r->converted,(size_t)capacity*4);if(!v)return false;r->converted=v;r->converted_capacity=(size_t)capacity;}
        const uint8_t *input=(const uint8_t *)o->samples;
        uint8_t *output=(uint8_t *)r->converted;
        int frames=swr_convert(r->resampler,&output,capacity,&input,(int)o->frames);
        if(frames<0)return false;r->converted_frames=(size_t)frames;r->at=0;return true;
    }
    AVFrame *f=av_frame_alloc();if(!f)return false;
    f->format=AV_SAMPLE_FMT_S16;f->sample_rate=48000;f->nb_samples=(int)o->frames;
    f->ch_layout=(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;f->pts=r->fed;
    int ok=av_frame_get_buffer(f,0);
    if(ok>=0){memcpy(f->data[0],o->samples,o->frames*4);ok=av_buffersrc_add_frame_flags(r->source,f,0);}
    if(ok>=0)r->fed+=(int64_t)o->frames;av_frame_free(&f);return ok>=0;
}
int nmedia_rate_pull(nmedia_rate *r,struct nmedia_output *o){
    if(!r||!o)return NMEDIA_ERROR;
    int frames=0;
    if(r->resampler){
        size_t available=r->converted_frames-r->at;
        if(available){frames=(int)(available>4096?4096:available);memcpy(r->output,r->converted+r->at*2,(size_t)frames*4);r->at+=(size_t)frames;}
        else if(r->ended){uint8_t *output=(uint8_t *)r->output;frames=swr_convert(r->resampler,&output,4096,NULL,0);if(frames<0)return NMEDIA_ERROR;}
    }
    else{
        if(r->at==(size_t)r->frame->nb_samples){av_frame_unref(r->frame);r->at=0;int code=av_buffersink_get_frame(r->sink,r->frame);
            if(code==AVERROR(EAGAIN))return NMEDIA_AGAIN;if(code==AVERROR_EOF)return NMEDIA_END;if(code<0)return NMEDIA_ERROR;}
        if(r->frame->format!=AV_SAMPLE_FMT_S16||r->frame->ch_layout.nb_channels!=2)return NMEDIA_ERROR;
        size_t available=(size_t)r->frame->nb_samples-r->at;frames=(int)(available>4096?4096:available);
        memcpy(r->output,r->frame->data[0]+r->at*4,(size_t)frames*4);r->at+=(size_t)frames;
    }
    if(!frames)return r->ended?NMEDIA_END:NMEDIA_AGAIN;
    *o=(struct nmedia_output){.kind=NMEDIA_AUDIO,.samples=r->output,.frames=(size_t)frames,.pts_ms=(int64_t)lround((r->origin+r->emitted*r->rate)/48.0)};
    r->emitted+=frames;return NMEDIA_AUDIO;
}
bool nmedia_rate_end(nmedia_rate *r){if(!r)return false;if(r->ended)return true;r->ended=true;return !r->source||av_buffersrc_add_frame_flags(r->source,NULL,0)>=0;}
void nmedia_rate_close(nmedia_rate *r){if(!r)return;swr_free(&r->resampler);av_frame_free(&r->frame);avfilter_graph_free(&r->graph);nmedia_ff_free(r->converted);nmedia_ff_free(r);}
