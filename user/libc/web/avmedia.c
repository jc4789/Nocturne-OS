/* Private native HTML media bridge. No page-selected filesystem paths.
 * Buffered fetch or anonymous per-response CORS-checked bounded Range input. */
#include "avmedia.h"
#include "media.h"
#include "media_alloc_private.h"
#include "media_worker_private.h"
#include "media_mse_private.h"
#include "media_mse_worker_private.h"
#include "../media_http_private.h"
#include "nocturne.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#define DOCUMENT_MEDIA_LIMIT 64
#define DOCUMENT_DECODER_LIMIT 4
#define DOCUMENT_MEDIA_BYTES (64u * 1024u * 1024u)
struct web_avmedia {
    struct web_avmedia *next;
    web_doc *doc;
    node_t *node;
    uint32_t generation;
    nmedia *decoder;
    nmedia_worker *worker;
    struct {uint32_t id;bool used;} mse[2];
    nmedia_mse_worker *mse_worker;
    uint64_t mse_pending_revision;
    uint64_t mse_revision_seen[2];bool mse_audio_owner[2];
    bool mse_seeking,mse_quota_error;
    struct nmedia_info mse_info;
    bool mse_active,starved,mse_eos,mse_eos_duration;
    double mse_duration;
    int mse_pending_slot;
    size_t input_size, audio_at;
    struct nmedia_output pending;
    uint32_t *pixels;
    size_t pixel_capacity;
    int width, height, fd;
    bool playing, ended, muted, url_input,audio_enabled,video_enabled;
    int ready;
    double volume;
    int64_t base_ms;
    uint64_t started;
    char error[160];
    int16_t scaled[4096 * 2];
};
static struct web_avmedia *streams;
static const struct nmedia_info *information(struct web_avmedia *s) {
    if(s->mse_active){
        memset(&s->mse_info,0,sizeof s->mse_info);s->mse_info.duration_ms=isfinite(s->mse_duration)?(int64_t)(s->mse_duration*1000):-1;
        for(int i=0;i<2;i++){const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,i);if(!info)continue;
            if(info->audio){s->mse_info.audio=true;s->mse_info.channels=info->channels;s->mse_info.sample_rate=info->sample_rate;strlcpy(s->mse_info.audio_codec,info->audio_codec,32);}
            if(info->video){s->mse_info.video=true;s->mse_info.width=info->width;s->mse_info.height=info->height;strlcpy(s->mse_info.video_codec,info->video_codec,32);}}
        return &s->mse_info;
    }
    return s->worker?nmedia_worker_info(s->worker):nmedia_get_info(s->decoder);
}
static bool media_node(node_t *n) { return n && n->type == N_ELEM && !n->foreign && (n->tag == T_audio || n->tag == T_video); }
static struct web_avmedia *find(web_doc *d, node_t *n, bool create) {
    unsigned count = 0;
    for (struct web_avmedia *s = streams; s; s = s->next) {
        if (s->doc == d) { count++; if (s->node == n) return s; }
    }
    if (!create || count >= DOCUMENT_MEDIA_LIMIT) return NULL;
    struct web_avmedia *s = nmedia_ff_mallocz(sizeof *s);
    if (!s) return NULL;
    s->doc = d; s->node = n; s->fd = -1;s->mse_pending_slot=-1;s->audio_enabled=s->video_enabled=true; s->volume = 1; s->next = streams; streams = s;
    return s;
}
static void close_audio(struct web_avmedia *s) { if (s->fd >= 0) { audio_flush(s->fd); close(s->fd); } s->fd = -1; }
static void unload(struct web_avmedia *s) {
    close_audio(s); nmedia_close(s->decoder); s->decoder = NULL;
    nmedia_worker_close(s->worker);s->worker=NULL;
    nmedia_mse_worker_close(s->mse_worker);s->mse_worker=NULL;memset(s->mse,0,sizeof s->mse);memset(s->mse_revision_seen,0,sizeof s->mse_revision_seen);memset(s->mse_audio_owner,0,sizeof s->mse_audio_owner);s->mse_seeking=s->mse_quota_error=false;
    s->mse_active=s->starved=s->mse_eos=false;s->mse_duration=NAN;
    nmedia_ff_free(s->pixels); s->pixels = NULL; s->pixel_capacity = 0;
    s->width = s->height = 0; s->playing = s->ended = false;
    s->url_input = false;
    s->audio_enabled=s->video_enabled=true;
    s->base_ms = 0; s->input_size = 0; s->audio_at = 0; s->pending.kind = 0; s->ready = 0; s->error[0] = 0;
}
static int64_t current(struct web_avmedia *s, uint64_t now) {
    int64_t ms = s->base_ms + (s->playing && !s->starved && !nmedia_worker_seeking(s->worker) && now >= s->started ? (int64_t)(now - s->started) : 0);
    const struct nmedia_info *info = information(s);
    if (info && info->duration_ms >= 0 && ms > info->duration_ms) ms = info->duration_ms;
    return ms;
}
static bool audio_open(struct web_avmedia *s) {
    if(s->fd>=0)return true;
    const struct nmedia_info *info = information(s);
    if (!info || !info->audio || !s->audio_enabled) return true;
    s->fd = open("/dev/audio", O_RDWR | O_NONBLOCK);
    uint32_t queued=0;
    if (s->fd < 0 || read(s->fd,&queued,4)!=4) { close_audio(s);strlcpy(s->error,"native audio output unavailable",sizeof s->error);return false; }
    s->error[0]=0;return true;
}
static bool seek(struct web_avmedia *s, int64_t ms, uint64_t now) {
    if(s->mse_active){
        if(!nmedia_mse_worker_command(s->mse_worker,NMSW_SEEK,0,NULL,0,0,ms,0,false))return false;
        close_audio(s);s->base_ms=ms;s->started=now;s->pending.kind=0;s->audio_at=0;s->ended=false;s->starved=true;s->mse_seeking=true;s->ready=1;return true;
    }
    if(s->worker) {
        if(!nmedia_worker_seek(s->worker,ms))return false;
        close_audio(s);s->base_ms=ms;s->started=now;s->pending.kind=0;s->audio_at=0;s->ended=false;s->ready=1;
        return true;
    }
    if (!s->decoder || !nmedia_seek(s->decoder,ms)) return false;
    close_audio(s); s->base_ms=ms; s->started=now; s->pending.kind=0; s->audio_at=0;s->ended=false;
    if(s->playing&&!audio_open(s)){s->playing=false;return false;}
    return true;
}
/* Decoder output is borrowed only until its next step. The display owns its
 * last presented frame, separate from a future-PTS pending decoder frame. */
static bool present(struct web_avmedia *s) {
    if(!s->video_enabled)return true;
    struct nmedia_output *o=&s->pending;
    if(!o->pixels||o->width<=0||o->height<=0||(uint64_t)o->width*o->height>NMEDIA_MAX_PIXELS){strlcpy(s->error,"invalid native video span",sizeof s->error);return false;}
    size_t count=(size_t)o->width*o->height;
    if(count>s->pixel_capacity){uint32_t *p=nmedia_ff_realloc(s->pixels,count*4);if(!p){strlcpy(s->error,"video presentation allocation",sizeof s->error);return false;}s->pixels=p;s->pixel_capacity=count;}
    memcpy(s->pixels,o->pixels,count*4);s->width=o->width;s->height=o->height;return true;
}
static JSValue state(JSContext *ctx, struct web_avmedia *s) {
    JSValue o=JS_NewObject(ctx);
    const struct nmedia_info *info=s?information(s):NULL;
    JS_SetPropertyStr(ctx,o,"readyState",JS_NewInt32(ctx,s?s->ready:0));
    JS_SetPropertyStr(ctx,o,"duration",JS_NewFloat64(ctx,s&&s->mse_active?s->mse_duration:info&&info->duration_ms>=0?info->duration_ms/1000.0:NAN));
    JS_SetPropertyStr(ctx,o,"currentTime",JS_NewFloat64(ctx,s?current(s,uptime_ms())/1000.0:0));
    JS_SetPropertyStr(ctx,o,"paused",JS_NewBool(ctx,!s||!s->playing));
    JS_SetPropertyStr(ctx,o,"ended",JS_NewBool(ctx,s&&s->ended));
    JS_SetPropertyStr(ctx,o,"videoWidth",JS_NewInt32(ctx,info?info->width:0));
    JS_SetPropertyStr(ctx,o,"videoHeight",JS_NewInt32(ctx,info?info->height:0));
    JS_SetPropertyStr(ctx,o,"error",s&&s->error[0]?JS_NewString(ctx,s->error):JS_NULL);
    JS_SetPropertyStr(ctx,o,"loading",JS_NewBool(ctx,s&&nmedia_worker_loading(s->worker)));
    JS_SetPropertyStr(ctx,o,"seeking",JS_NewBool(ctx,s&&(s->mse_seeking||nmedia_worker_seeking(s->worker))));
    JS_SetPropertyStr(ctx,o,"msePending",JS_NewBool(ctx,s&&nmedia_mse_worker_pending(s->mse_worker)));
    JS_SetPropertyStr(ctx,o,"mseQuotaError",JS_NewBool(ctx,s&&(s->mse_quota_error||nmedia_mse_worker_quota_error(s->mse_worker))));
    JS_SetPropertyStr(ctx,o,"waiting",JS_NewBool(ctx,s&&s->starved));
    return o;
}
static int mse_next(struct web_avmedia *s,struct nmedia_output *out){
    int r=nmedia_mse_worker_step(s->mse_worker,out);
    if(r==NMEDIA_ERROR)strlcpy(s->error,nmedia_mse_worker_error(s->mse_worker),sizeof s->error);
    if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){s->mse_pending_slot=(int)nmedia_mse_worker_output_slot(s->mse_worker);s->mse_pending_revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)s->mse_pending_slot);}
    return r;
}
static JSValue mse_info(JSContext *ctx,struct web_avmedia *s,unsigned slot){
    const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,slot);
    JSValue o=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,o,"audio",JS_NewBool(ctx,info&&info->audio));JS_SetPropertyStr(ctx,o,"video",JS_NewBool(ctx,info&&info->video));
    JS_SetPropertyStr(ctx,o,"audioEnabled",JS_NewBool(ctx,s->audio_enabled));JS_SetPropertyStr(ctx,o,"videoSelected",JS_NewBool(ctx,s->video_enabled));
    JS_SetPropertyStr(ctx,o,"audioCodec",JS_NewString(ctx,info?info->audio_codec:""));JS_SetPropertyStr(ctx,o,"videoCodec",JS_NewString(ctx,info?info->video_codec:""));
    JS_SetPropertyStr(ctx,o,"width",JS_NewInt32(ctx,info?info->width:0));JS_SetPropertyStr(ctx,o,"height",JS_NewInt32(ctx,info?info->height:0));
    JS_SetPropertyStr(ctx,o,"parsing",JS_NewBool(ctx,nmedia_mse_worker_parsing(s->mse_worker,slot)));
    JS_SetPropertyStr(ctx,o,"groupEnd",JS_NewFloat64(ctx,nmedia_mse_worker_group_end(s->mse_worker,slot)/1000.0));
    JS_SetPropertyStr(ctx,o,"initDuration",JS_NewFloat64(ctx,info&&info->duration_ms>=0?info->duration_ms/1000.0:INFINITY));
    JS_SetPropertyStr(ctx,o,"sequenceOffset",JS_NewFloat64(ctx,nmedia_mse_worker_group_end(s->mse_worker,slot)/1000.0));return o;
}
static size_t mse_document_free(struct web_avmedia *s){
    size_t used=0;for(struct web_avmedia *p=streams;p;p=p->next)if(p->doc==s->doc){used+=p->input_size;for(int i=0;i<2;i++)if(p->mse[i].used)used+=NMEDIA_MAX_BYTES-nmedia_mse_worker_quota(p->mse_worker,i);}
    return used<DOCUMENT_MEDIA_BYTES?DOCUMENT_MEDIA_BYTES-used:0;
}
static JSValue mse_queue(JSContext *ctx,struct web_avmedia *s,unsigned op,unsigned slot,const void *bytes,size_t size,int64_t offset,int64_t a,int64_t b,bool flag){
    if(!nmedia_mse_worker_command(s->mse_worker,op,slot,bytes,size,offset,a,b,flag))return JS_ThrowRangeError(ctx,"MSE command/input aggregate quota");
    return JS_TRUE;
}
static JSValue mse_call(JSContext *ctx,struct web_avmedia *s,const char *op,int argc,JSValueConst *argv,uint64_t now){
    nmedia_mse_worker_time(s->mse_worker,current(s,now));
    if(!strcmp(op,"mseSelect")){
        if(argc<2)return JS_FALSE;const char *kind=JS_ToCString(ctx,argv[0]);if(!kind)return JS_EXCEPTION;
        int selected=JS_ToBool(ctx,argv[1]);if(selected<0){JS_FreeCString(ctx,kind);return JS_EXCEPTION;}bool audio=!strcmp(kind,"audio"),video=!strcmp(kind,"video");JS_FreeCString(ctx,kind);
        if(!audio&&!video)return JS_ThrowTypeError(ctx,"invalid media track kind");bool *enabled=audio?&s->audio_enabled:&s->video_enabled;if(*enabled==!!selected)return JS_TRUE;
        if(s->mse_active&&!nmedia_mse_worker_command(s->mse_worker,NMSW_SELECT,audio?0:1,NULL,0,0,0,0,selected))return JS_ThrowRangeError(ctx,"MSE selection queue quota");
        *enabled=!!selected;if(audio){close_audio(s);if(selected&&s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}
        else {if(!selected){nmedia_ff_free(s->pixels);s->pixels=NULL;s->pixel_capacity=0;s->width=s->height=0;s->doc->dirty=true;}else if(s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}return JS_TRUE;
    }
    if(!strcmp(op,"mseCreate")){
        uint32_t generation;if(!argc||JS_ToUint32(ctx,&generation,argv[0])<0)return JS_EXCEPTION;
        if(s->generation!=generation)return JS_FALSE;unload(s);s->generation=generation;s->mse_worker=nmedia_mse_worker_open(generation,s->error,sizeof s->error);
        if(!s->mse_worker)return JS_FALSE;s->mse_active=true;s->starved=true;s->mse_duration=NAN;return JS_TRUE;
    }
    if(!s->mse_active)return JS_FALSE;
    if(!strcmp(op,"mseDuration")){
        double seconds;if(!argc||JS_ToFloat64(ctx,&seconds,argv[0])<0)return JS_EXCEPTION;
        if(isnan(seconds)||seconds<0||(isfinite(seconds)&&seconds>1e9))return JS_ThrowRangeError(ctx,"invalid MSE duration");
        if(isfinite(seconds))for(int i=0;i<2;i++)if(s->mse[i].used){struct nmedia_time_range ranges[64];size_t count=nmedia_mse_worker_ranges(s->mse_worker,i,ranges,64);
            if(count&&ranges[count-1].end_ms>seconds*1000){JSValue result=mse_queue(ctx,s,NMSW_REMOVE,i,NULL,0,0,(int64_t)(seconds*1000),INT64_MAX,false);if(JS_IsException(result))return result;}}
        s->mse_duration=seconds;return JS_TRUE;
    }
    if(!strcmp(op,"mseEnd")||!strcmp(op,"mseReopen")){
        bool eos=!strcmp(op,"mseEnd");const char *error=eos&&argc?JS_ToCString(ctx,argv[0]):NULL;if(eos&&argc&&!error)return JS_EXCEPTION;
        if(error&&*error){if(strcmp(error,"network")&&strcmp(error,"decode")){JS_FreeCString(ctx,error);return JS_ThrowTypeError(ctx,"invalid MSE end error");}
            strlcpy(s->error,!strcmp(error,"network")?"MSE network error":"MSE decode error",sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);JS_FreeCString(ctx,error);return JS_TRUE;}
        JS_FreeCString(ctx,error);s->mse_eos=eos;s->mse_eos_duration=false;s->ended=false;return mse_queue(ctx,s,NMSW_END,0,NULL,0,0,0,0,eos);
    }
    uint32_t id;if(!argc||JS_ToUint32(ctx,&id,argv[0])<0)return JS_EXCEPTION;
    int slot=-1;for(int i=0;i<2;i++)if(s->mse[i].used&&s->mse[i].id==id)slot=i;
    if(!strcmp(op,"mseAdd")){
        if(slot>=0||!id||argc<2)return JS_FALSE;
        for(int i=0;i<2;i++)if(!s->mse[i].used){slot=i;break;}if(slot<0)return JS_ThrowRangeError(ctx,"MSE SourceBuffer limit");
        unsigned decoders=0;for(struct web_avmedia *p=streams;p;p=p->next)if(p->doc==s->doc){if(p->decoder||p->worker)decoders++;for(int i=0;i<2;i++)if(p->mse[i].used)decoders++;}
        if(decoders>=DOCUMENT_DECODER_LIMIT)return JS_ThrowRangeError(ctx,"document decoder limit");
        const char *mime=JS_ToCString(ctx,argv[1]);if(!mime)return JS_EXCEPTION;
        if(!nmedia_mse_type(mime)){JS_FreeCString(ctx,mime);return JS_FALSE;}
        JSValue result=mse_queue(ctx,s,NMSW_ADD,(unsigned)slot,mime,strlen(mime),0,0,0,false);JS_FreeCString(ctx,mime);
        if(!JS_IsException(result)){s->mse[slot].used=true;s->mse[slot].id=id;}return result;
    }
    if(slot<0)return JS_FALSE;
    if(!strcmp(op,"mseInfo"))return mse_info(ctx,s,(unsigned)slot);
    if(!strcmp(op,"mseQuota"))return JS_NewFloat64(ctx,(double)MIN(nmedia_mse_worker_quota(s->mse_worker,(unsigned)slot),mse_document_free(s)));
    if(!strcmp(op,"mseRanges")){
        struct nmedia_time_range ranges[64];size_t count=nmedia_mse_worker_ranges(s->mse_worker,(unsigned)slot,ranges,64);JSValue array=JS_NewArray(ctx);
        for(size_t i=0;i<count;i++){JSValue pair=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,pair,0,JS_NewFloat64(ctx,ranges[i].start_ms/1000.0));JS_SetPropertyUint32(ctx,pair,1,JS_NewFloat64(ctx,ranges[i].end_ms/1000.0));JS_SetPropertyUint32(ctx,array,(uint32_t)i,pair);}return array;
    }
    if(!strcmp(op,"mseAppend")){
        double offset,a,b;size_t size;if(argc<6||JS_ToFloat64(ctx,&offset,argv[2])<0||JS_ToFloat64(ctx,&a,argv[3])<0||JS_ToFloat64(ctx,&b,argv[4])<0)return JS_EXCEPTION;
        if(!isfinite(offset)||fabs(offset)>1e9||!isfinite(a)||a<0||a>1e9||isnan(b)||b<=a||(isfinite(b)&&b>1e9))return JS_ThrowRangeError(ctx,"invalid MSE append properties");
        int sequence=JS_ToBool(ctx,argv[5]);if(sequence<0)return JS_EXCEPTION;uint8_t *bytes=JS_GetArrayBuffer(ctx,&size,argv[1]);if(!bytes)return JS_EXCEPTION;
        if(size>mse_document_free(s)||size>nmedia_mse_worker_quota(s->mse_worker,(unsigned)slot))return JS_ThrowRangeError(ctx,"document media input quota");
        s->mse_eos=false;s->ended=false;if(s->pending.kind==NMEDIA_END)s->pending.kind=0;return mse_queue(ctx,s,NMSW_APPEND,(unsigned)slot,bytes,size,(int64_t)(offset*1000),(int64_t)(a*1000),isfinite(b)?(int64_t)(b*1000):INT64_MAX,sequence);
    }
    if(!strcmp(op,"mseChangeType")){
        if(argc<2)return JS_FALSE;const char *mime=JS_ToCString(ctx,argv[1]);if(!mime)return JS_EXCEPTION;
        JSValue result=mse_queue(ctx,s,NMSW_CHANGE,(unsigned)slot,mime,strlen(mime),0,0,0,false);JS_FreeCString(ctx,mime);return result;
    }
    if(!strcmp(op,"mseRemove")){
        double a,b;if(argc<3||JS_ToFloat64(ctx,&a,argv[1])<0||JS_ToFloat64(ctx,&b,argv[2])<0)return JS_EXCEPTION;
        if(!isfinite(a)||a<0||a>1e9||isnan(b)||b<=a||(isfinite(b)&&b>1e9))return JS_ThrowRangeError(ctx,"invalid MSE removal range");
        return mse_queue(ctx,s,NMSW_REMOVE,(unsigned)slot,NULL,0,0,(int64_t)(a*1000),isfinite(b)?(int64_t)(b*1000):INT64_MAX,false);
    }
    if(!strcmp(op,"mseAbort")){s->error[0]=0;return mse_queue(ctx,s,NMSW_ABORT,(unsigned)slot,NULL,0,0,0,0,false);}
    if(!strcmp(op,"mseDrop")){JSValue result=mse_queue(ctx,s,NMSW_DROP,(unsigned)slot,NULL,0,0,0,0,false);if(!JS_IsException(result)){s->mse[slot].used=false;s->mse[slot].id=0;}return result;}
    return JS_ThrowTypeError(ctx,"unknown native MSE operation");
}
static bool range_node(web_doc *d,node_t *n,const char *url) {
    const char *credentials=node_attr(n,"crossorigin");
    /* Inert/adopted family nodes cannot borrow the caller's document origin.
     * Browser host permission is independently checked by native_avmedia. */
    return n->owner==d&&!d->inert&&(!credentials||strcasecmp(credentials,"use-credentials"))&&
        nmedia_http_browser_url(url,d->url);
}
JSValue web_avmedia_call(JSContext *ctx, web_doc *d, node_t *n, const char *op, int argc, JSValueConst *argv) {
    if(!strcmp(op,"mseType")){
        const char *type=argc?JS_ToCString(ctx,argv[0]):NULL;if(argc&&!type)return JS_EXCEPTION;bool supported=nmedia_mse_type(type);JS_FreeCString(ctx,type);return JS_NewBool(ctx,supported);
    }
    if (!strcmp(op,"type")) {
        const char *type=argc?JS_ToCString(ctx,argv[0]):NULL;
        if(!type&&argc)return JS_EXCEPTION;
        JSValue result=JS_NewString(ctx,nmedia_can_play_type(type));JS_FreeCString(ctx,type);return result;
    }
    if(!media_node(n)||!d)return JS_ThrowTypeError(ctx,"HTMLMediaElement receiver required");
    if(n->owner!=d && (!n->owner || n->owner->dom_family!=d))return JS_ThrowTypeError(ctx,"foreign document media receiver");
    if(!strcmp(op,"range")) {
        if(!argc)return JS_FALSE;
        size_t length;const char *url=JS_ToCStringLen(ctx,&length,argv[0]);if(!url)return JS_EXCEPTION;
        bool allowed=length==strlen(url)&&range_node(d,n,url);JS_FreeCString(ctx,url);
        return JS_NewBool(ctx,allowed);
    }
    struct web_avmedia *s=find(d,n,strcmp(op,"state")!=0);
    if(!s)return !strcmp(op,"state")?state(ctx,NULL):JS_ThrowRangeError(ctx,"document media limit");
    uint64_t now=uptime_ms();
    if(!strcmp(op,"state"))return state(ctx,s);
    if(!strncmp(op,"mse",3))return mse_call(ctx,s,op,argc,argv,now);
    if(!strcmp(op,"reset")){
        uint32_t generation;if(!argc||JS_ToUint32(ctx,&generation,argv[0])<0)return JS_EXCEPTION;
        unload(s);s->generation=generation;d->dirty=true;return JS_UNDEFINED;
    }
    if(!strcmp(op,"load")||!strcmp(op,"loadURL")){
        uint32_t generation;size_t size=0;
        if(argc<2||JS_ToUint32(ctx,&generation,argv[1])<0)return JS_EXCEPTION;
        if(s->generation!=generation)return JS_FALSE;
        bool url_input=!strcmp(op,"loadURL");
        const char *url=NULL;uint8_t *bytes=NULL;
        if(url_input) {
            size_t length;url=JS_ToCStringLen(ctx,&length,argv[0]);if(!url)return JS_EXCEPTION;
            if(length!=strlen(url)||!range_node(d,n,url)){JS_FreeCString(ctx,url);return JS_ThrowTypeError(ctx,"native media Range policy denied");}
            size=NMEDIA_HTTP_CACHE_BYTES;
        } else {bytes=JS_GetArrayBuffer(ctx,&size,argv[0]);if(!bytes)return JS_EXCEPTION;}
        size_t total=0;unsigned decoders=0;for(struct web_avmedia *p=streams;p;p=p->next)if(p->doc==d&&p!=s){total+=p->input_size;if(p->decoder||p->worker)decoders++;}
        if(size>NMEDIA_MAX_BYTES||total>DOCUMENT_MEDIA_BYTES-size||decoders>=DOCUMENT_DECODER_LIMIT){JS_FreeCString(ctx,url);return JS_ThrowRangeError(ctx,"document media resource limit");}
        unload(s);s->generation=generation;
        if(url_input)s->worker=nmedia_worker_open(url,d->url,generation,s->error,sizeof s->error);
        else s->decoder=nmedia_open_memory(bytes,size,s->error,sizeof s->error);
        JS_FreeCString(ctx,url);s->url_input=url_input;
        if(s->worker)s->input_size=size; /* Metadata arrives only after worker OPEN. */
        if(s->decoder){
            s->input_size=size;s->ready=1;
            int result=NMEDIA_AGAIN;
            for(int i=0;i<4&&result==NMEDIA_AGAIN;i++)result=nmedia_step(s->decoder,&s->pending);
            if(result==NMEDIA_ERROR){strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            else if(result==NMEDIA_AUDIO||result==NMEDIA_VIDEO){
                s->ready=url_input?2:4; /* A single window does not promise canplaythrough. */
                if(result==NMEDIA_VIDEO&&!present(s)){nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            }
        }
        d->dirty=true;return JS_NewBool(ctx,s->decoder!=NULL||s->worker!=NULL);
    }
    if(!strcmp(op,"play")){
        if((!s->decoder&&!s->worker&&!s->mse_active)||!s->ready||nmedia_worker_seeking(s->worker))return JS_FALSE;
        if(s->playing)return JS_TRUE;
        if(s->ended&&!seek(s,0,now))return JS_FALSE;
        /* An ended worker restarts through a real asynchronous seek. Do not
         * open (and then overwrite) an audio FD before that seek's ACK. */
        if(!nmedia_worker_seeking(s->worker)&&!audio_open(s))return JS_FALSE;
        s->playing=true;s->started=now;s->ended=false;return JS_TRUE;
    }
    if(!strcmp(op,"pause")){
        if(s->playing){int64_t ms=current(s,now);s->playing=false;if(!seek(s,ms,now))s->base_ms=ms;close_audio(s);}
        return JS_UNDEFINED;
    }
    if(!strcmp(op,"seek")){
        double seconds;if(!argc||JS_ToFloat64(ctx,&seconds,argv[0])<0)return JS_EXCEPTION;
        if(!isfinite(seconds)||seconds<0||seconds>1e9)return JS_ThrowRangeError(ctx,"invalid media time");
        return JS_NewBool(ctx,seek(s,(int64_t)(seconds*1000),now));
    }
    if(!strcmp(op,"volume")){
        double v;if(argc<2||JS_ToFloat64(ctx,&v,argv[0])<0)return JS_EXCEPTION;
        int muted=JS_ToBool(ctx,argv[1]);if(muted<0)return JS_EXCEPTION;
        if(!isfinite(v)||v<0||v>1)return JS_ThrowRangeError(ctx,"invalid media volume");
        s->volume=v;s->muted=muted;return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx,"unknown native media operation");
}
static void pump(struct web_avmedia *s,uint64_t now){
    if(s->mse_worker){
        nmedia_mse_worker_pump(s->mse_worker,now);
        const char *error=nmedia_mse_worker_error(s->mse_worker);if(*error){strlcpy(s->error,error,sizeof s->error);s->mse_quota_error=nmedia_mse_worker_quota_error(s->mse_worker);if(!s->mse_quota_error){s->base_ms=current(s,now);s->playing=false;close_audio(s);}s->doc->dirty=true;return;}
        if(s->mse_quota_error){s->mse_quota_error=false;s->error[0]=0;}
        if(s->mse_eos&&!s->mse_eos_duration&&!nmedia_mse_worker_pending(s->mse_worker)){
            int64_t highest=0;for(int i=0;i<2;i++){struct nmedia_time_range ranges[64];size_t n=nmedia_mse_worker_ranges(s->mse_worker,i,ranges,64);if(n)highest=MAX(highest,ranges[n-1].end_ms);}
            s->mse_duration=highest/1000.0;s->mse_eos_duration=true;
        }
        for(int i=0;i<2;i++){
            const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,(unsigned)i);uint64_t revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)i);
            if(revision!=s->mse_revision_seen[i]){
                if(s->pending.kind&&s->mse_pending_slot==i&&s->mse_pending_revision!=revision){s->pending.kind=0;s->audio_at=0;}
                if(s->mse_audio_owner[i]||(info&&info->audio)){close_audio(s);if(s->playing&&!audio_open(s))s->playing=false;}
                s->mse_revision_seen[i]=revision;
            }s->mse_audio_owner[i]=info&&info->audio;
        }
        if(!s->ready&&(nmedia_mse_worker_info(s->mse_worker,0)||nmedia_mse_worker_info(s->mse_worker,1)))s->ready=1;
        if(s->mse_seeking){if(nmedia_mse_worker_seeking(s->mse_worker))return;s->mse_seeking=false;s->started=now;if(s->playing&&!audio_open(s)){s->playing=false;return;}}
    }
    if(s->mse_active&&!s->error[0]&&!s->pending.kind){
        int r=mse_next(s,&s->pending);s->audio_at=0;
        if(r==NMEDIA_ERROR){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->ready=0;s->doc->dirty=true;return;}
        if(r==NMSW_BUSY)return;
        if(r==NMEDIA_AGAIN){
            uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);
            if(!queued&&!s->starved){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}return;
        }
        if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){
            s->ready=2;if(s->starved){s->starved=false;s->started=now;if(s->playing&&s->fd<0&&!audio_open(s)){s->playing=false;return;}}
            if(r==NMEDIA_VIDEO&&!s->playing){if(!present(s))return;s->doc->dirty=true;}
        }
    }
    if(s->worker) {
        bool was_seeking=nmedia_worker_seeking(s->worker);
        nmedia_worker_pump(s->worker,now);
        const char *error=nmedia_worker_error(s->worker);
        if(*error){strlcpy(s->error,error,sizeof s->error);s->playing=false;s->ready=0;close_audio(s);s->doc->dirty=true;return;}
        if(nmedia_worker_loading(s->worker)||nmedia_worker_seeking(s->worker))return;
        if(was_seeking){s->started=now;if(s->playing&&!audio_open(s)){s->playing=false;return;}}
        if(!s->ready)s->ready=1;
        if(!s->pending.kind) {
            int r=nmedia_worker_take(s->worker,&s->pending);s->audio_at=0;
            if(r==NMEDIA_AGAIN){if(s->playing||s->ready<2)nmedia_worker_step(s->worker);return;}
            if(r==NMEDIA_END&&s->ready<2){strlcpy(s->error,"native worker input has no decoded data",sizeof s->error);s->ready=0;nmedia_worker_close(s->worker);s->worker=NULL;s->input_size=0;s->doc->dirty=true;return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO){s->ready=2;if(r==NMEDIA_VIDEO&&!s->playing){if(!present(s))return;s->pending.kind=0;s->doc->dirty=true;}}
        }
    }
    if((!s->decoder&&!s->worker&&!s->mse_active)||!s->playing)return;
    for(int budget=0;budget<8;budget++){
        if(!s->pending.kind){int r=s->mse_active?mse_next(s,&s->pending):s->worker?nmedia_worker_take(s->worker,&s->pending):nmedia_step(s->decoder,&s->pending);s->audio_at=0;
            if(s->worker&&r==NMEDIA_AGAIN){nmedia_worker_step(s->worker);return;}
            if(r==NMSW_BUSY)return;
            if(r==NMEDIA_AGAIN){if(s->mse_active){uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);if(!queued){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}}return;}
            if(r==NMEDIA_ERROR){if(!s->mse_active)strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO)s->ready=s->url_input||s->mse_active?2:4;
        }
        if(s->pending.kind==NMSW_CLOCK){if(s->pending.pts_ms>current(s,now)+5)return;s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_VIDEO){
            if(s->pending.pts_ms>current(s,now)+5)return;
            if(!present(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            s->doc->dirty=true;s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_AUDIO){
            if(s->pending.pts_ms>current(s,now)+100)return;
            if(s->fd>=0){uint32_t queued=0;if(read(s->fd,&queued,4)!=4||queued>=4800)return;
                if(s->pending.frames>4096||s->audio_at>s->pending.frames){strlcpy(s->error,"invalid native audio span",sizeof s->error);s->playing=false;close_audio(s);return;}
                size_t frames=MIN(MIN(s->pending.frames-s->audio_at,(size_t)(4800-queued)),(size_t)4096);
                double gain=s->muted?0:s->volume;
                for(size_t i=0;i<frames*2;i++)s->scaled[i]=(int16_t)(s->pending.samples[s->audio_at*2+i]*gain);
                ssize_t n=write(s->fd,s->scaled,frames*4);
                if(n<0&&errno==EAGAIN)return;
                if(n<=0||(n&3)){strlcpy(s->error,"native audio write failed",sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);return;}
                s->audio_at+=(size_t)n/4;if(s->audio_at<s->pending.frames)return;
            }s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_END){
            uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);if(queued)return;
            /* EOF is not the end of the last displayed frame. Honor a known
             * duration only for a bounded <=1s tail; corrupt/unknown duration
             * must not hold an already-drained decoder forever. */
            const struct nmedia_info *info=information(s);
            int64_t at=current(s,now);
            if(info&&info->video&&info->duration_ms>at&&info->duration_ms-at<=1000)return;
            s->base_ms=current(s,now);s->playing=false;s->ended=true;close_audio(s);s->doc->dirty=true;return;
        }
    }
}
void web_avmedia_tick(web_doc *d,uint64_t now){
    nmedia_worker_background(now);nmedia_mse_worker_background(now);
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d){
        if(s->node->owner!=d&&(!s->node->owner||s->node->owner->dom_family!=d))unload(s);
        else pump(s,now);
    }
}
void web_avmedia_background(uint64_t now){nmedia_worker_background(now);nmedia_mse_worker_background(now);}
int64_t web_avmedia_background_deadline(uint64_t now){int64_t a=nmedia_worker_deadline(now),b=nmedia_mse_worker_deadline(now);return a<0?b:b<0?a:MIN(a,b);}
int64_t web_avmedia_deadline(web_doc *d,uint64_t now){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d&&s->playing)return (int64_t)now+10;
    return -1;
}
void web_avmedia_free(web_doc *d){
    struct web_avmedia **link=&streams;
    while(*link){struct web_avmedia *s=*link;if(s->doc!=d){link=&s->next;continue;}*link=s->next;unload(s);nmedia_ff_free(s);}
}
bool web_avmedia_paint(web_doc *d,node_t *n,canvas_t *c,int x,int y,int w,int h){
    struct web_avmedia *s=find(d,n,false);
    if(!media_node(n)||w<=0||h<=0)return false;
    int64_t x0=MAX(MAX((int64_t)x,c->cx0),0),y0=MAX(MAX((int64_t)y,c->cy0),0);
    int64_t x1=MIN(MIN((int64_t)x+w,c->cx1),c->w),y1=MIN(MIN((int64_t)y+h,c->cy1),c->h);
    if(x0>=x1||y0>=y1)return true;
    int ox0=c->cx0,oy0=c->cy0,ox1=c->cx1,oy1=c->cy1;
    c->cx0=(int)x0;c->cy0=(int)y0;c->cx1=(int)x1;c->cy1=(int)y1;
    gfx_fill(c,(int)x0,(int)y0,(int)(x1-x0),(int)(y1-y0),RGB(0,0,0));
    bool controls=node_attr(n,"controls")!=NULL;
    int strip=controls?MIN(h,32):0;
    if(s&&s->pixels&&n->tag==T_video)nmedia_draw(c,s->pixels,s->width,s->height,x,y,w,h-strip);
    if(controls){
        int64_t top=MAX((int64_t)y+h-strip,y0);
        if(top<y1){
            gfx_fill(c,(int)x0,(int)top,(int)(x1-x0),(int)(y1-top),RGB(38,38,44));
            const char *text=s&&s->error[0]?"Media error":s&&s->playing?"Pause":"Play";
            gfx_text(c,(int)x0+8,(int)top+7,text,RGB(255,255,255),TRANSPARENT,FONT_SMALL);
        }
    }
    c->cx0=ox0;c->cy0=oy0;c->cx1=ox1;c->cy1=oy1;return true;
}
bool web_avmedia_size(node_t *n,int *w,int *h){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->node==n&&(s->decoder||s->worker||s->mse_active)){
        const struct nmedia_info *info=information(s);
        if(info&&info->video&&info->width>0&&info->height>0){*w=info->width;*h=info->height;return true;}
    }
    return false;
}
