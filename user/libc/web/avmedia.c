/* Private native HTML media bridge. No page-selected filesystem paths.
 * Buffered fetch or anonymous per-response CORS-checked bounded Range input. */
#include "avmedia.h"
#include "video_snapshot.h"
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
    /* Idle elements retain metadata/poster, not a 64 MiB child reservation.
     * URLs remain subject to the same native anonymous Range policy. */
    char range_url[NMEDIA_WORKER_URL];
    struct nmedia_info range_info;
    bool range_waiting,range_resume,range_seek_sent,range_keep;
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
    /* One owned future picture permits PCM decode past a video timestamp.
     * Decoder/worker output storage is borrowed and reused by the next STEP. */
    struct nmedia_output ahead;
    uint32_t *ahead_pixels;
    size_t ahead_capacity;
    int ahead_slot;
    uint64_t ahead_revision;
    uint32_t *pixels;
    size_t pixel_capacity;
    int width, height, fd;
    bool playing, ended, muted, url_input,audio_enabled,video_enabled;
    int ready;
    double volume;
    int64_t base_ms;
    uint64_t started;
    uint64_t video_frames,dropped_video_frames;
    /* Aggregate timing diagnostics only: no source URL, packet, or page data. */
    uint64_t presented_frames,audio_frames,profile_at;
    int64_t presented_ms,audio_end_ms;
    bool audio_written,profile_ended;
    char error[160];
    /* Private, bounded plain-text caption surface; owned with this media node. */
    char caption[2049];
    /* Resolved from the last ordinary paint: no box/style/node is read when
     * composing a new frame. Storage is charged to the existing media budget. */
    struct {
        canvas_t target;
        uint8_t *storage,*opaque;
        uint32_t *committed,*black,*output;
        size_t count;
        int x,y,w,h,video_x,video_y,video_w,video_h,source_w,source_h;
        int scroll_x,scroll_y;
        uint32_t generation;
        uint64_t delivered;
        bool valid;
    } snapshot;
    uint64_t window_frames;
    int16_t scaled[4096 * 2];
};
static struct web_avmedia *streams;
static struct web_avmedia *service_cursor;
static unsigned native_depth;
static bool service_running,service_clock_set;
static uint64_t service_at;
static struct web_avmedia *snapshot_candidate;
static web_doc *snapshot_document;
static unsigned snapshot_phase;
static bool snapshot_rejected;
bool web_js_video_present(web_doc *doc,const struct web_video_patch *patch);
static void snapshot_clear(struct web_avmedia *s) {
    s->snapshot.valid=false;
    nmedia_ff_free(s->snapshot.storage);memset(&s->snapshot,0,sizeof s->snapshot);
    if(snapshot_candidate==s)snapshot_candidate=NULL;
}
void web_avmedia_enter(void){native_depth++;}
void web_avmedia_leave(void){if(native_depth)native_depth--;}
static const struct nmedia_info *information(struct web_avmedia *s) {
    if(s->mse_active){
        memset(&s->mse_info,0,sizeof s->mse_info);s->mse_info.duration_ms=isfinite(s->mse_duration)?(int64_t)(s->mse_duration*1000):-1;
        for(int i=0;i<2;i++){const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,i);if(!info)continue;
            if(info->audio){s->mse_info.audio=true;s->mse_info.channels=info->channels;s->mse_info.sample_rate=info->sample_rate;strlcpy(s->mse_info.audio_codec,info->audio_codec,32);}
            if(info->video){s->mse_info.video=true;s->mse_info.width=info->width;s->mse_info.height=info->height;strlcpy(s->mse_info.video_codec,info->video_codec,32);}}
        return &s->mse_info;
    }
    if(s->worker){const struct nmedia_info *info=nmedia_worker_info(s->worker);if(info)return info;}
    return s->url_input&&s->ready?&s->range_info:nmedia_get_info(s->decoder);
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
static void close_audio(struct web_avmedia *s) { if (s->fd >= 0) { audio_flush(s->fd); close(s->fd); } s->fd = -1;s->audio_written=false; }
static void clear_ahead(struct web_avmedia *s) {
    nmedia_ff_free(s->ahead_pixels);s->ahead_pixels=NULL;s->ahead_capacity=0;s->ahead.kind=0;
}
static void unload(struct web_avmedia *s) {
    snapshot_clear(s);
    clear_ahead(s);
    close_audio(s); nmedia_close(s->decoder); s->decoder = NULL;
    nmedia_worker_close(s->worker);s->worker=NULL;
    nmedia_mse_worker_close(s->mse_worker);s->mse_worker=NULL;memset(s->mse,0,sizeof s->mse);memset(s->mse_revision_seen,0,sizeof s->mse_revision_seen);memset(s->mse_audio_owner,0,sizeof s->mse_audio_owner);s->mse_seeking=s->mse_quota_error=false;
    s->mse_active=s->starved=s->mse_eos=false;s->mse_duration=NAN;
    nmedia_ff_free(s->pixels); s->pixels = NULL; s->pixel_capacity = 0;
    s->width = s->height = 0; s->playing = s->ended = false;
    s->url_input = false;
    s->range_url[0]=0;memset(&s->range_info,0,sizeof s->range_info);
    s->range_info.duration_ms=-1;s->range_waiting=s->range_resume=s->range_seek_sent=s->range_keep=false;
    s->audio_enabled=s->video_enabled=true;
    s->base_ms = 0; s->input_size = 0; s->audio_at = 0; s->pending.kind = 0; s->ready = 0; s->error[0] = 0; s->caption[0] = 0;
    s->video_frames=s->dropped_video_frames=0;
    s->presented_frames=s->audio_frames=s->profile_at=0;s->presented_ms=s->audio_end_ms=-1;s->audio_written=s->profile_ended=false;
}
static int64_t current(struct web_avmedia *s, uint64_t now) {
    int64_t ms = s->base_ms + (s->playing && !s->starved && !nmedia_worker_seeking(s->worker) && now >= s->started ? (int64_t)(now - s->started) : 0);
    /* Wall time is not audible progress while JS/paint delays PCM supply.
     * Use this stream's consumed PCM timeline once a write establishes it.
     * Empty software queue freezes at the last supplied sample, not at a
     * fictitious future wall position. Silent/disabled tracks keep wall time. */
    if(s->playing&&s->audio_enabled&&s->fd>=0&&s->audio_written){
        uint32_t queued=0;
        if(read(s->fd,&queued,sizeof queued)==sizeof queued)ms=MAX(s->base_ms,s->audio_end_ms-(int64_t)queued*1000/SOUND_RATE);
    }
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
static void range_suspend(struct web_avmedia *s,uint64_t now) {
    s->base_ms=current(s,now);s->playing=false;close_audio(s);
    clear_ahead(s);
    const struct nmedia_info *info=nmedia_worker_info(s->worker);
    if(info)s->range_info=*info;
    nmedia_worker_close(s->worker);s->worker=NULL;
    s->input_size=0;
    s->pending.kind=0;s->audio_at=0;s->range_resume=s->range_seek_sent=s->range_keep=false;
    /* A cancelled initial preload remains queued; a decoded idle element
     * can be reopened at base_ms only on an explicit play/seek request. */
    s->range_waiting=s->ready<2;
    s->doc->dirty=true;
}
static void range_failed(struct web_avmedia *s,const char *error){
    if(error!=s->error)strlcpy(s->error,error,sizeof s->error);
    s->playing=false;s->ready=0;close_audio(s);
    clear_ahead(s);
    nmedia_worker_close(s->worker);s->worker=NULL;s->input_size=0;
    s->pending.kind=0;s->range_waiting=s->range_resume=s->range_keep=false;
    s->doc->dirty=true;
}
static bool seek(struct web_avmedia *s, int64_t ms, uint64_t now) {
    snapshot_clear(s);
    if(s->mse_active){
        if(!nmedia_mse_worker_command(s->mse_worker,NMSW_SEEK,0,NULL,0,0,ms,0,false))return false;
        clear_ahead(s);
        close_audio(s);s->base_ms=ms;s->started=now;s->pending.kind=0;s->audio_at=0;s->ended=false;s->starved=true;s->mse_seeking=true;s->ready=1;return true;
    }
    if(s->url_input&&!s->worker&&s->range_url[0]) {
        clear_ahead(s);
        close_audio(s);s->base_ms=ms;s->started=now;s->pending.kind=0;s->audio_at=0;
        s->ended=false;s->range_waiting=s->range_resume=true;s->range_seek_sent=false;
        return true;
    }
    if(s->worker) {
        if(!nmedia_worker_seek(s->worker,ms))return false;
        clear_ahead(s);
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
    bool size_changed=s->width!=o->width||s->height!=o->height;
    memcpy(s->pixels,o->pixels,count*4);s->width=o->width;s->height=o->height;
    if(size_changed){s->doc->layout_valid=false;s->doc->dirty=true;}else s->doc->paint_dirty=true;
    if(size_changed)s->snapshot.valid=false;
    s->presented_frames++;s->presented_ms=o->pts_ms;return true;
}
static bool hold_video(struct web_avmedia *s) {
    const struct nmedia_output *o=&s->pending;
    if(s->ahead.kind||!o->pixels||o->width<=0||o->height<=0||(uint64_t)o->width*o->height>NMEDIA_MAX_PIXELS){strlcpy(s->error,"invalid native lookahead span",sizeof s->error);return false;}
    size_t count=(size_t)o->width*o->height;
    if(count>s->ahead_capacity){uint32_t *p=nmedia_ff_realloc(s->ahead_pixels,count*4);if(!p){strlcpy(s->error,"video lookahead allocation",sizeof s->error);return false;}s->ahead_pixels=p;s->ahead_capacity=count;}
    memcpy(s->ahead_pixels,o->pixels,count*4);s->ahead=*o;s->ahead.pixels=s->ahead_pixels;
    s->ahead_slot=s->mse_pending_slot;s->ahead_revision=s->mse_pending_revision;return true;
}
static bool present_ahead(struct web_avmedia *s,uint64_t now) {
    if(!s->ahead.kind||s->ahead.pts_ms>current(s,now)+5)return true;
    if(s->pixels&&s->ahead.pts_ms<current(s,now)-100){s->dropped_video_frames++;s->ahead.kind=0;return true;}
    struct nmedia_output pending=s->pending;s->pending=s->ahead;
    bool ok=present(s);s->pending=pending;s->ahead.kind=0;return ok;
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
    JS_SetPropertyStr(ctx,o,"totalVideoFrames",JS_NewFloat64(ctx,s?(double)s->video_frames:0));
    JS_SetPropertyStr(ctx,o,"droppedVideoFrames",JS_NewFloat64(ctx,s?(double)s->dropped_video_frames:0));
    JS_SetPropertyStr(ctx,o,"error",s&&s->error[0]?JS_NewString(ctx,s->error):JS_NULL);
    JS_SetPropertyStr(ctx,o,"rangeInput",JS_NewBool(ctx,s&&s->url_input));
    JS_SetPropertyStr(ctx,o,"queued",JS_NewBool(ctx,s&&s->range_waiting&&!s->worker));
    JS_SetPropertyStr(ctx,o,"loading",JS_NewBool(ctx,s&&(s->range_waiting||nmedia_worker_loading(s->worker))));
    JS_SetPropertyStr(ctx,o,"seeking",JS_NewBool(ctx,s&&(s->range_resume||s->mse_seeking||nmedia_worker_seeking(s->worker))));
    JS_SetPropertyStr(ctx,o,"msePending",JS_NewBool(ctx,s&&nmedia_mse_worker_pending(s->mse_worker)));
    JS_SetPropertyStr(ctx,o,"mseQuotaError",JS_NewBool(ctx,s&&(s->mse_quota_error||nmedia_mse_worker_quota_error(s->mse_worker))));
    JS_SetPropertyStr(ctx,o,"waiting",JS_NewBool(ctx,s&&s->starved));
    return o;
}
static int mse_next(struct web_avmedia *s,struct nmedia_output *out,uint64_t until){
    int r=nmedia_mse_worker_step(s->mse_worker,out);
    /* STEP is local buffered decode, not author JS or network. Sending and
     * collecting several packets within one finite native burst avoids a
     * complete JS/CSS/paint pass between every 20 ms PCM packet. */
    for(unsigned handoff=0;r==NMSW_BUSY&&handoff<8&&uptime_ms()<until;handoff++){
        nmedia_mse_worker_pump_budget(s->mse_worker,uptime_ms(),until);
        r=nmedia_mse_worker_step(s->mse_worker,out);
        if(r!=NMSW_BUSY||uptime_ms()>=until)break;
        yield(); /* Time is rechecked before the next pump, including slow yields. */
    }
    /* A scheduler handoff can consume the remaining 2 ms while the peer
     * produces PCM. Collect an already-readable response once, with the
     * expired/shared deadline: no additional yield or blocking wait. */
    if(r==NMSW_BUSY){nmedia_mse_worker_collect(s->mse_worker);r=nmedia_mse_worker_step(s->mse_worker,out);}
    if(r==NMEDIA_ERROR)strlcpy(s->error,nmedia_mse_worker_error(s->mse_worker),sizeof s->error);
    if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){s->mse_pending_slot=(int)nmedia_mse_worker_output_slot(s->mse_worker);s->mse_pending_revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)s->mse_pending_slot);}
    return r;
}
static int range_next(struct web_avmedia *s,struct nmedia_output *out,struct nmedia_worker_budget *b){
    int r=nmedia_worker_take(s->worker,out);
    if(r!=NMEDIA_AGAIN)return r;
    nmedia_worker_step(s->worker);
    /* Decode/network runs in the existing child. Only finite nonblocking
     * dispatch/reap and scheduler handoffs run here; no new burst allowance. */
    while(nmedia_worker_pending(s->worker)&&b->rx_bytes&&uptime_ms()<b->until){
        nmedia_worker_pump_budget(s->worker,uptime_ms(),b,false);
        r=nmedia_worker_take(s->worker,out);
        if(r!=NMEDIA_AGAIN||!nmedia_worker_pending(s->worker)||b->handoffs>=16||uptime_ms()>=b->until)break;
        b->handoffs++;yield();
    }
    if(r==NMEDIA_AGAIN){nmedia_worker_collect(s->worker,b);r=nmedia_worker_take(s->worker,out);}
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
        if(audio){s->base_ms=current(s,now);s->started=now;}
        *enabled=!!selected;if(audio){close_audio(s);if(selected&&s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}
        else {if(!selected){clear_ahead(s);nmedia_ff_free(s->pixels);s->pixels=NULL;s->pixel_capacity=0;s->width=s->height=0;s->doc->dirty=true;}else if(s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}return JS_TRUE;
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
static JSValue avmedia_call(JSContext *ctx, web_doc *d, node_t *n, const char *op, int argc, JSValueConst *argv) {
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
    if(!strcmp(op,"captionText")){
        if(!argc)return JS_ThrowTypeError(ctx,"caption text required");
        size_t length;const char *text=JS_ToCStringLen(ctx,&length,argv[0]);if(!text)return JS_EXCEPTION;
        if(length>sizeof(s->caption)-1||memchr(text,0,length)){JS_FreeCString(ctx,text);return JS_ThrowRangeError(ctx,"caption text exceeds bounded plain-text surface");}
        if(strlen(s->caption)!=length||memcmp(s->caption,text,length)){memcpy(s->caption,text,length);s->caption[length]=0;d->dirty=true;}
        JS_FreeCString(ctx,text);return JS_UNDEFINED;
    }
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
        if(url_input){
            strlcpy(s->range_url,url,sizeof s->range_url);s->range_waiting=true;
            if(argc>2){int requested=JS_ToBool(ctx,argv[2]);if(requested<0){JS_FreeCString(ctx,url);return JS_EXCEPTION;}s->range_keep=s->range_resume=requested!=0;}
        }
        else s->decoder=nmedia_open_memory(bytes,size,s->error,sizeof s->error);
        JS_FreeCString(ctx,url);s->url_input=url_input;
        /* Queued Range elements own no cache/child yet; charge only after OPEN. */
        if(s->decoder){
            s->input_size=size;s->ready=1;
            int result=NMEDIA_AGAIN;
            for(int i=0;i<4&&result==NMEDIA_AGAIN;i++)result=nmedia_step(s->decoder,&s->pending);
            if(result==NMEDIA_ERROR){strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            else if(result==NMEDIA_AUDIO||result==NMEDIA_VIDEO){
                if(result==NMEDIA_VIDEO)s->video_frames++;
                s->ready=url_input?2:4; /* A single window does not promise canplaythrough. */
                if(result==NMEDIA_VIDEO&&!present(s)){nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            }
        }
        d->dirty=true;return JS_NewBool(ctx,s->decoder!=NULL||s->range_waiting);
    }
    if(!strcmp(op,"preparePlay")){
        if(s->url_input&&s->range_url[0]&&(!s->worker||s->range_waiting)){
            s->range_waiting=s->range_resume=true;s->range_seek_sent=false;
        }
        if(s->url_input)s->range_keep=true;
        return JS_TRUE;
    }
    if(!strcmp(op,"play")){
        if((!s->decoder&&!s->worker&&!s->mse_active)||!s->ready||nmedia_worker_seeking(s->worker))return JS_FALSE;
        if(s->playing)return JS_TRUE;
        if(s->ended&&!seek(s,0,now))return JS_FALSE;
        /* An ended worker restarts through a real asynchronous seek. Do not
         * open (and then overwrite) an audio FD before that seek's ACK. */
        if(!nmedia_worker_seeking(s->worker)&&!audio_open(s))return JS_FALSE;
        s->playing=true;s->started=now;s->ended=false;s->range_keep=false;return JS_TRUE;
    }
    if(!strcmp(op,"pause")){
        if(s->url_input){range_suspend(s,now);}
        else if(s->playing){int64_t ms=current(s,now);s->playing=false;if(!seek(s,ms,now))s->base_ms=ms;close_audio(s);}
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
JSValue web_avmedia_call(JSContext *ctx,web_doc *d,node_t *n,const char *op,int argc,JSValueConst *argv){
    web_avmedia_enter();JSValue result=avmedia_call(ctx,d,n,op,argc,argv);web_avmedia_leave();return result;
}
static void pump(struct web_avmedia *s,uint64_t now,uint64_t mse_until,bool cooperative){
    if(uptime_ms()>=mse_until)return;
    struct nmedia_worker_budget range_budget={.until=mse_until,.tx_bytes=65536,.rx_bytes=262144};
    if(s->url_input&&s->error[0]){if(s->worker)range_failed(s,s->error);return;}
    if(!cooperative&&s->url_input&&s->range_waiting&&!s->worker&&nmedia_worker_available()){
        /* Revalidate after an idle element is resumed. An adopted/removed
         * document cannot reuse a stale origin grant or asynchronous handle. */
        if(!range_node(s->doc,s->node,s->range_url)){
            strlcpy(s->error,"native media Range resume policy denied",sizeof s->error);
            s->range_waiting=s->range_resume=false;s->ready=0;return;
        }
        s->worker=nmedia_worker_open(s->range_url,s->doc->url,s->generation,s->error,sizeof s->error);
        if(!s->worker){s->range_waiting=s->range_resume=false;s->ready=0;return;}
        s->input_size=NMEDIA_HTTP_CACHE_BYTES;
    }
    if(s->mse_worker){
        nmedia_mse_worker_pump_budget(s->mse_worker,now,mse_until);
        const char *error=nmedia_mse_worker_error(s->mse_worker);if(*error){strlcpy(s->error,error,sizeof s->error);s->mse_quota_error=nmedia_mse_worker_quota_error(s->mse_worker);if(!s->mse_quota_error){s->base_ms=current(s,now);s->playing=false;close_audio(s);}s->doc->dirty=true;return;}
        if(s->mse_quota_error){s->mse_quota_error=false;s->error[0]=0;}
        if(s->mse_eos&&!s->mse_eos_duration&&!nmedia_mse_worker_pending(s->mse_worker)){
            int64_t highest=0;for(int i=0;i<2;i++){struct nmedia_time_range ranges[64];size_t n=nmedia_mse_worker_ranges(s->mse_worker,i,ranges,64);if(n)highest=MAX(highest,ranges[n-1].end_ms);}
            s->mse_duration=highest/1000.0;s->mse_eos_duration=true;
        }
        for(int i=0;i<2;i++){
            const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,(unsigned)i);uint64_t revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)i);
            if(revision!=s->mse_revision_seen[i]){
                if(s->ahead.kind&&s->ahead_slot==i&&s->ahead_revision!=revision)clear_ahead(s);
                if(s->pending.kind&&s->mse_pending_slot==i&&s->mse_pending_revision!=revision){s->pending.kind=0;s->audio_at=0;}
                if(s->mse_audio_owner[i]||(info&&info->audio)){s->base_ms=current(s,now);s->started=now;close_audio(s);if(s->playing&&!audio_open(s))s->playing=false;}
                s->mse_revision_seen[i]=revision;
            }s->mse_audio_owner[i]=info&&info->audio;
        }
        if(!s->ready&&(nmedia_mse_worker_info(s->mse_worker,0)||nmedia_mse_worker_info(s->mse_worker,1)))s->ready=1;
        if(s->mse_seeking){if(nmedia_mse_worker_seeking(s->mse_worker))return;s->mse_seeking=false;s->started=now;if(s->playing&&!audio_open(s)){s->playing=false;return;}}
    }
    if(s->mse_active&&!s->error[0]&&!s->pending.kind){
        int r=mse_next(s,&s->pending,mse_until);s->audio_at=0;
        if(r==NMEDIA_VIDEO)s->video_frames++;
        if(r==NMEDIA_ERROR){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->ready=0;s->doc->dirty=true;return;}
        if(r==NMSW_BUSY)return;
        if(r==NMEDIA_AGAIN){
            uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);
            if(!queued&&!s->starved){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}return;
        }
        if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){
            s->ready=2;if(s->starved){s->starved=false;s->started=now;if(s->playing&&s->fd<0&&!audio_open(s)){s->playing=false;return;}}
            if(r==NMEDIA_VIDEO&&!s->playing){if(!present(s))return;}
        }
    }
    if(s->worker) {
        bool was_seeking=nmedia_worker_seeking(s->worker);
        nmedia_worker_pump_budget(s->worker,now,&range_budget,!cooperative);
        const char *error=nmedia_worker_error(s->worker);
        if(*error){range_failed(s,error);return;}
        if(nmedia_worker_loading(s->worker))return;
        const struct nmedia_info *info=nmedia_worker_info(s->worker);if(info)s->range_info=*info;
        if(s->range_resume&&!s->range_seek_sent){
            s->range_seek_sent=true;
            if(s->base_ms>0&&!nmedia_worker_seek(s->worker,s->base_ms)){
                range_failed(s,"native media Range resume seek failed");return;
            }
        }
        if(nmedia_worker_seeking(s->worker))return;
        if(was_seeking){s->started=now;if(s->playing&&!audio_open(s)){s->playing=false;return;}}
        if(!s->ready)s->ready=1;
        if(!s->pending.kind) {
            int r=s->playing||s->ready<2||s->range_resume?range_next(s,&s->pending,&range_budget):nmedia_worker_take(s->worker,&s->pending);s->audio_at=0;
            if(r==NMEDIA_VIDEO)s->video_frames++;
            if(r==NMEDIA_AGAIN)return;
            if(r==NMEDIA_ERROR){range_failed(s,nmedia_worker_error(s->worker));return;}
            if(r==NMEDIA_END&&s->ready<2){range_failed(s,"native worker input has no decoded data");return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO){
                s->ready=2;s->range_waiting=false;
                if(r==NMEDIA_VIDEO&&!s->playing){if(!present(s)){range_failed(s,s->error);return;}s->pending.kind=0;}
                if(!s->playing&&!s->range_keep){range_suspend(s,now);return;}
                s->range_resume=false;
            }
        }
    }
    if((!s->decoder&&!s->worker&&!s->mse_active)||!s->playing)return;
    for(int budget=0;budget<8;budget++){
        if(!present_ahead(s,now)){s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;}
        /* Release borrowed storage only after presentation, snapshot, drop or
         * complete PCM write. Dispatch before the cap return so the child can
         * decode while the browser performs its next JS/paint phase. */
        if(budget&&s->mse_active&&!s->pending.kind)nmedia_mse_worker_prefetch(s->mse_worker,mse_until);
        if(budget&&s->worker&&!s->pending.kind)nmedia_worker_prefetch(s->worker,&range_budget);
        if((s->mse_active||s->worker)&&budget&&uptime_ms()>=mse_until)return;
        if(!s->pending.kind){int r=s->mse_active?mse_next(s,&s->pending,mse_until):s->worker?range_next(s,&s->pending,&range_budget):nmedia_step(s->decoder,&s->pending);s->audio_at=0;
            if(r==NMEDIA_VIDEO)s->video_frames++;
            if(r==NMSW_BUSY)return;
            if(r==NMEDIA_AGAIN){if(s->mse_active){uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);if(!queued){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}}return;}
            if(r==NMEDIA_ERROR){if(s->worker){range_failed(s,nmedia_worker_error(s->worker));return;}if(!s->mse_active)strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO)s->ready=s->url_input||s->mse_active?2:4;
        }
        if(s->pending.kind==NMSW_CLOCK){if(s->pending.pts_ms>current(s,now)+5)return;s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_VIDEO){
            if(s->pending.pts_ms>current(s,now)+5){
                /* A video-first packet must not block the following PCM:
                 * consumed audio can stop a few ms before its timestamp.
                 * Exactly one owned picture, no clock invention or eager
                 * display, and the existing 2 ms / 8 packet burst still bounds
                 * progress. A second future picture applies backpressure. */
                if((s->mse_active||s->worker)&&s->fd>=0&&s->audio_written&&!s->ahead.kind){
                    if(!hold_video(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;}
                    s->pending.kind=0;continue;
                }return;
            }
            /* Keep decoding reference frames, but do not spend full document
             * paint/layout work displaying a backlog of obsolete frames. */
            if(s->pixels&&s->pending.pts_ms<current(s,now)-100){s->dropped_video_frames++;s->pending.kind=0;continue;}
            if(!present(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            s->pending.kind=0;
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
                s->audio_at+=(size_t)n/4;s->audio_frames+=(size_t)n/4;s->audio_written=true;
                s->audio_end_ms=s->pending.pts_ms+(int64_t)s->audio_at*1000/SOUND_RATE;
                if(s->audio_at<s->pending.frames)return;
            }s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_END){
            uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);if(queued)return;
            /* EOF is not the end of the last displayed frame. Honor a known
             * duration only for a bounded <=1s tail; corrupt/unknown duration
             * must not hold an already-drained decoder forever. */
            const struct nmedia_info *info=information(s);
            int64_t at=current(s,now);
            if(info&&info->video&&info->duration_ms>at&&info->duration_ms-at<=1000){
                /* Drained audio may end slightly before the final picture.
                 * Transfer only this bounded EOF tail back to the wall clock. */
                if(s->fd>=0){s->base_ms=at;s->started=now;close_audio(s);}return;
            }
            s->base_ms=current(s,now);s->playing=false;s->ended=true;close_audio(s);
            if(s->url_input)range_suspend(s,now);
            s->doc->dirty=true;return;
        }
    }
    if(s->mse_active&&!s->pending.kind)nmedia_mse_worker_prefetch(s->mse_worker,mse_until);
    if(s->worker&&!s->pending.kind)nmedia_worker_prefetch(s->worker,&range_budget);
}
static void timing_profile(struct web_avmedia *s,uint64_t now){
    if((!s->playing&&(!s->ended||s->profile_ended))||(s->playing&&now-s->profile_at<2000))return;
    s->profile_at=now;s->profile_ended=s->ended;
    uint32_t queued=0;bool audio=s->fd>=0&&read(s->fd,&queued,sizeof queued)==sizeof queued;
    int64_t played=s->audio_written&&audio?s->audio_end_ms-(int64_t)queued*1000/SOUND_RATE:-1;
    char message[384];
    snprintf(message,sizeof message,"Native media timing: generation %u, input %s, clock %lld ms, presented %llu at %lld ms, decoded %llu/drop %llu; audio %s, queued %u frames, written %llu frames, end %lld/played-estimate %lld ms, starved %u/ended %u",
        s->generation,s->mse_active?"MSE":s->url_input?"Range":"buffer",(long long)current(s,now),
        (unsigned long long)s->presented_frames,(long long)s->presented_ms,(unsigned long long)s->video_frames,(unsigned long long)s->dropped_video_frames,
        audio?"open":"closed",queued,(unsigned long long)s->audio_frames,(long long)s->audio_end_ms,(long long)played,s->starved,s->ended);
    /* queued covers this stream's software ring, not the hardware mix queue. */
    web_js_console(s->doc,0,message);
}
void web_avmedia_tick(web_doc *d,uint64_t now){
    web_avmedia_enter();
    nmedia_worker_background(now);nmedia_mse_worker_background(now);
    struct web_avmedia *prepared=NULL;
    /* Explicit play/seek gets the child ahead of passive metadata preloads. */
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d&&s->range_resume&&s->range_waiting&&!s->worker&&
        (s->node->owner==d||(s->node->owner&&s->node->owner->dom_family==d))){pump(s,now,uptime_ms()+2,false);prepared=s;break;}
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d){
        if(s==prepared)continue;
        if(s->node->owner!=d&&(!s->node->owner||s->node->owner->dom_family!=d))unload(s);
        else {pump(s,now,uptime_ms()+2,false);timing_profile(s,now);}
    }
    web_avmedia_leave();
}
void web_avmedia_service(uint64_t now){
    if(!streams||native_depth||service_running)return;
    uint64_t start=uptime_ms();
    if(service_clock_set&&start>=service_at&&start-service_at<10)return;
    service_at=start;service_clock_set=true;service_running=true;
    /* Shared across every context: one existing live MSE/Range child, not one new
     * 2ms allowance per iframe. Candidate scanning itself is bounded and fair.
     * Only private media pixels/state + deferred dirty flags may be updated. */
    uint64_t until=start+2;
    struct web_avmedia *first=service_cursor&&service_cursor->next?service_cursor->next:streams,*s=first;
    for(unsigned scanned=0;s&&scanned<16&&uptime_ms()<until;scanned++){
        service_cursor=s;
        if(s->playing&&s->doc&&s->doc->live&&s->node&&
           (s->node->owner==s->doc||(s->node->owner&&s->node->owner->dom_family==s->doc))&&
           ((s->mse_active&&nmedia_mse_worker_running(s->mse_worker))||
            (s->url_input&&nmedia_worker_running(s->worker)))){
            pump(s,now,until,true);break;
        }
        s=s->next?s->next:streams;if(s==first)break;
    }
    service_running=false;
}
void web_avmedia_background(uint64_t now){nmedia_worker_background(now);nmedia_mse_worker_background(now);}
int64_t web_avmedia_background_deadline(uint64_t now){int64_t a=nmedia_worker_deadline(now),b=nmedia_mse_worker_deadline(now);return a<0?b:b<0?a:MIN(a,b);}
int64_t web_avmedia_deadline(web_doc *d,uint64_t now){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d&&(s->playing||s->range_waiting))return (int64_t)now+10;
    return -1;
}
void web_avmedia_free(web_doc *d){
    web_avmedia_enter();
    struct web_avmedia **link=&streams;
    while(*link){struct web_avmedia *s=*link;if(s->doc!=d){link=&s->next;continue;}*link=s->next;if(service_cursor==s)service_cursor=NULL;unload(s);nmedia_ff_free(s);}
    web_avmedia_leave();
}
/* Finite plain-text WebVTT surface, not the WebVTT region/style layout engine.
 * Wrap on complete UTF-8 scalars and clip to the real media rectangle. */
static void paint_caption(struct web_avmedia *s,canvas_t *c,int x,int y,int w,int h){
    if(!s||!s->caption[0]||w<=16||h<=0)return;
    const char *at=s->caption,*starts[4];size_t lengths[4];int widths[4],count=0;
    int line_height=gfx_font_h(FONT_SMALL)+4,limit=MIN(4,h/line_height);
    while(*at&&count<limit){
        starts[count]=at;int width=0;
        while(*at&&*at!='\n'){
            uint32_t cp;int bytes=gfx_utf8_decode(at,&cp);char scalar[5];
            memcpy(scalar,at,(size_t)bytes);scalar[bytes]=0;int advance=gfx_text_width(scalar,FONT_SMALL);
            if(width&&width+advance>w-16)break;
            width+=advance;at+=bytes;
        }
        lengths[count]=(size_t)(at-starts[count]);widths[count++]=width;
        if(*at=='\n')at++;
    }
    if(!count)return;
    int64_t top=(int64_t)y+h-count*line_height;
    int64_t left=MAX((int64_t)x,c->cx0),right=MIN((int64_t)x+w,c->cx1),bottom=MIN((int64_t)y+h,c->cy1);
    int64_t visible_top=MAX(top,c->cy0);
    if(left>=right||visible_top>=bottom)return;
    gfx_fill(c,(int)left,(int)visible_top,(int)(right-left),(int)(bottom-visible_top),RGB(20,20,24));
    char line[2049];
    for(int i=0;i<count;i++){
        memcpy(line,starts[i],lengths[i]);line[lengths[i]]=0;
        int64_t tx=(int64_t)x+MAX(8,(w-widths[i])/2),ty=top+i*line_height+2;
        if(tx>=INT_MIN&&tx<=INT_MAX&&ty>=INT_MIN&&ty<=INT_MAX)gfx_text(c,(int)tx,(int)ty,line,RGB(255,255,255),TRANSPARENT,FONT_SMALL);
    }
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
    if(n->tag==T_video)paint_caption(s,c,x,y,w,h-strip);
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
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->node==n&&(s->decoder||s->worker||s->mse_active||(s->url_input&&s->ready))){
        const struct nmedia_info *info=information(s);
        if(info&&info->video&&info->width>0&&info->height>0){*w=info->width;*h=info->height;return true;}
    }
    return false;
}
