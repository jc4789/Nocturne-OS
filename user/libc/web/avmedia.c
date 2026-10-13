/* Private native HTML media bridge. No page-selected filesystem paths.
 * Buffered fetch or anonymous per-response CORS-checked bounded Range input. */
#include "avmedia.h"
#include "video_snapshot.h"
#include "media.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "media_worker_private.h"
#include "media_mse_private.h"
#include "media_mse_worker_private.h"
#include "media_rate_private.h"
#include "web_presentation.h"
#include "../media_http_private.h"
#include "nocturne.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>
struct web_mse_slot {uint32_t id;bool used,audio_owner;uint64_t revision_seen;};
struct caption_span {char *text,*ruby;uint32_t flags,color;};
struct caption_cue {
    struct caption_span *spans;size_t count;
    double line,position,size,width,region_x,region_y,viewport_x,viewport_y;
    uint32_t region_lines;bool snap,region,scroll;int vertical,align,line_align,position_align;
    uint64_t region_id;size_t region_group;
};
static void caption_cues_free(struct caption_cue *cues,size_t count){for(size_t i=0;i<count;i++){for(size_t j=0;j<cues[i].count;j++){nmedia_ff_free(cues[i].spans[j].text);nmedia_ff_free(cues[i].spans[j].ruby);}nmedia_ff_free(cues[i].spans);}nmedia_ff_free(cues);}
struct web_video_ahead {
    struct web_video_ahead *next;
    struct nmedia_output output;
    size_t capacity;
    int slot;uint32_t generation;uint64_t revision;
};
struct web_avmedia {
    struct web_avmedia *next;
    web_doc *doc;
    node_t *node;
    uint32_t generation;
    uint32_t performance_generation;
    int performance_width,performance_height;
    uint64_t performance_frames,performance_work_ms,performance_at;
    struct nmedia_video_configuration performance_configuration;
    uint64_t performance_measurement_id,performance_revision;
    int performance_slot;
    struct nmedia_audio_observation performance_audio;
    uint32_t performance_audio_generation;
    int performance_audio_slot;
    uint64_t performance_audio_revision,performance_audio_at;
    nmedia *decoder;
    nmedia_worker *worker;
    /* Idle elements retain metadata/poster, not a 64 MiB child reservation.
     * URLs remain subject to the same native anonymous Range policy. */
    char *range_url; /* exact queued URL; retained across legal pause/resume */
    struct nmedia_info range_info;
    bool range_waiting,range_resume,range_seek_sent,range_keep;
    struct web_mse_slot *mse;size_t mse_count,mse_capacity;
    nmedia_mse_worker *mse_worker;
    uint64_t mse_pending_revision;

    bool mse_seeking,mse_quota_error,mse_resume_needed,mse_preparing;
    struct nmedia_info mse_info;
    bool mse_active,starved,mse_eos,mse_eos_duration,mse_audio_wait;
    double mse_duration;
    int mse_pending_slot;
    size_t input_size, audio_at;
    struct nmedia_output pending;
    /* Owned pictures preserve legal video-first demux order while allowing
     * later PCM to reach the sink. Each allocation is charged until consumed;
     * caller work remains bounded by its existing time/packet burst. */
    struct nmedia_output ahead;
    uint32_t *ahead_pixels;
    size_t ahead_capacity;
    int ahead_slot;
    uint64_t ahead_revision;
    uint32_t ahead_generation;
    struct web_video_ahead *ahead_next,*ahead_tail;
    size_t ahead_count;
    uint32_t *pixels;
    size_t pixel_capacity;
    int width, height, fd;
    bool playing, ended, muted, url_input,audio_enabled,video_enabled;
    int ready;
    double volume,rate;
    bool preserve_pitch,rate_owned;
    nmedia_rate *rate_decoder;
    uint8_t *eme_keys;size_t eme_key_bytes;
    int64_t base_ms;
    uint64_t started;
    uint64_t video_frames,dropped_video_frames;
    /* Aggregate timing diagnostics only: no source URL, packet, or page data. */
    uint64_t presented_frames,audio_frames,audio_silence_frames,profile_at;
    int64_t presented_ms,audio_end_ms;
    int64_t audio_end_frame;
    bool audio_written,profile_ended;
    bool audio_clock_failed,audio_played_valid;
    int64_t audio_played_ms;
    bool clock_snapshot_enabled,clock_snapshot_valid;
    struct n_audio_status clock_snapshot;
    ssize_t clock_snapshot_bytes;
    uint64_t profile_pumps,profile_services,profile_pump_ms,profile_pump_at,profile_pump_gap;
    char error[160];
    /* Private, bounded plain-text caption surface; owned with this media node. */
    char caption[2049];
    struct caption_cue *caption_cues;size_t caption_count;
    int *caption_flow;
    uint64_t caption_revision;
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
    /* Debug transition records contain no URL, error text, or packet data. */
    const char *transition_reason,*transition_pending;
    uint32_t transition_generation,transition_target;
    unsigned transition_flags;
    int16_t scaled[4096 * 2];
};
static struct web_avmedia *streams;
static struct web_avmedia *service_cursor;
static unsigned native_depth;
static bool service_running,service_clock_set;
static uint64_t service_at;
static bool debug_transitions;
/* Leave a device period of headroom in the 8192-frame native ring. The
 * actual consumed-frame clock, not this lookahead, drives video and cues. */
#define WEB_AUDIO_TARGET 7200u
void web_avmedia_debug(bool enabled){debug_transitions=enabled;}
static int64_t current(struct web_avmedia *,uint64_t);
static void video_counts(struct web_avmedia *s,uint64_t *decoded,uint64_t *dropped){
    *decoded=s?s->video_frames:0;*dropped=s?s->dropped_video_frames:0;
    if(s&&s->mse_active){uint64_t native_decoded,native_dropped;nmedia_mse_worker_video_stats(s->mse_worker,&native_decoded,&native_dropped);*decoded=native_decoded;*dropped+=native_dropped;}
}
static void transition(struct web_avmedia *s,const char *reason,uint32_t target){
    if(!debug_transitions)return;
    if(s->transition_pending){const char *pending=s->transition_pending;s->transition_pending=NULL;transition(s,pending,s->generation);}
    unsigned flags=(s->playing?1u:0u)|(s->ended?2u:0u)|(s->starved?4u:0u)|(s->error[0]?8u:0u)|((unsigned)s->ready<<4)|(s->mse_active?256u:0u)|(s->url_input?512u:0u);
    if(s->transition_reason==reason&&s->transition_generation==s->generation&&s->transition_target==target&&s->transition_flags==flags)return;
    s->transition_reason=reason;s->transition_generation=s->generation;s->transition_target=target;s->transition_flags=flags;
    char message[320];uint64_t now=uptime_ms();
    snprintf(message,sizeof message,"Native media transition: reason %s, generation %u->%u, input %s, playing %u/ready %d/starved %u/ended %u/error %u, clock %lld ms, presented %llu/decoded %llu/drop %llu, pending %d/ahead %d; wall %llu ms",
        reason,s->generation,target,s->mse_active?"MSE":s->url_input?"Range":"buffer",s->playing,s->ready,s->starved,s->ended,s->error[0]!=0,(long long)current(s,now),
        (unsigned long long)s->presented_frames,(unsigned long long)s->video_frames,(unsigned long long)s->dropped_video_frames,s->pending.kind,s->ahead.kind,(unsigned long long)now);
    web_js_console(s->doc,0,message);
    if(!strcmp(reason,"native-error-stop")&&s->error[0]){
        /* This field contains only native codec/transport/sink diagnostics:
           authors' URL/MIME/error strings are never copied into it. Keep a
           fail-closed text guard as well; no callback occurs in service(). */
        bool safe=true;
        for(const unsigned char *p=(const unsigned char *)s->error;*p;p++)
            if(*p<32||*p>126||*p=='\\'||*p=='?'||*p=='#'||*p=='&'||*p=='=')safe=false;
        if(strstr(s->error,"://"))safe=false;
        const char *kind=strstr(s->error,"alloc")||strstr(s->error,"quota")||strstr(s->error,"ENOMEM")||strstr(s->error,"Cannot allocate memory")||strstr(s->error,"aggregate")?"allocation":
            strstr(s->error,"pipe")||strstr(s->error,"worker")||strstr(s->error,"wire")||strstr(s->error,"response")||strstr(s->error,"deadline")?"transport":
            strstr(s->error,"audio output")||strstr(s->error,"audio write")||strstr(s->error,"audio gap write")?"sink":"decoder/input";
        snprintf(message,sizeof message,"Native media error cause: generation %u, category %s, cause %s",s->generation,kind,safe?s->error:"non-diagnostic spelling redacted");
        web_js_console(s->doc,2,message);
    }
}
static struct web_avmedia *snapshot_candidate;
static web_doc *snapshot_document;
static unsigned snapshot_phase;
static bool snapshot_rejected;
struct snapshot_region {int64_t x0,y0,x1,y1;};
static struct snapshot_region *snapshot_regions;
static size_t snapshot_region_count,snapshot_region_capacity;
static bool snapshot_region_intersects(const struct snapshot_region *r,const struct web_avmedia *s){
    return r->x0<(int64_t)s->snapshot.x+s->snapshot.w&&r->x1>s->snapshot.x&&
        r->y0<(int64_t)s->snapshot.y+s->snapshot.h&&r->y1>s->snapshot.y;
}
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
        for(size_t i=0;i<s->mse_count;i++){const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,i);if(!info)continue;
            if(info->audio){s->mse_info.audio=true;s->mse_info.channels=info->channels;s->mse_info.sample_rate=info->sample_rate;strlcpy(s->mse_info.audio_codec,info->audio_codec,32);}
            if(info->video){s->mse_info.video=true;s->mse_info.width=info->width;s->mse_info.height=info->height;strlcpy(s->mse_info.video_codec,info->video_codec,32);}}
        return &s->mse_info;
    }
    if(s->worker){const struct nmedia_info *info=nmedia_worker_info(s->worker);if(info)return info;}
    return s->url_input&&s->ready?&s->range_info:nmedia_get_info(s->decoder);
}
static bool media_node(node_t *n) { return n && n->type == N_ELEM && !n->foreign && (n->tag == T_audio || n->tag == T_video); }
static struct web_avmedia *find(web_doc *d, node_t *n, bool create) {
    for (struct web_avmedia *s = streams; s; s = s->next) {
        if (s->doc == d && s->node == n) return s;
    }
    if (!create) return NULL;
    struct web_avmedia *s = nmedia_ff_mallocz(sizeof *s);
    if (!s) return NULL;
    s->doc = d; s->node = n; s->fd = -1;s->mse_pending_slot=-1;s->audio_enabled=s->video_enabled=true; s->volume = s->rate = 1;s->preserve_pitch=true; s->next = streams; streams = s;
    return s;
}
static void close_audio(struct web_avmedia *s) { s->clock_snapshot_valid=false;s->audio_clock_failed=s->audio_played_valid=false;if (s->fd >= 0) { audio_flush(s->fd); close(s->fd); } s->fd = -1;s->audio_written=false;nmedia_rate_close(s->rate_decoder);s->rate_decoder=NULL;s->rate_owned=false; }
static void clear_ahead(struct web_avmedia *s) {
    s->mse_audio_wait=false;
    while(s->ahead_next){struct web_video_ahead *v=s->ahead_next;s->ahead_next=v->next;nmedia_ff_free((void *)v->output.pixels);nmedia_ff_free(v);}
    s->ahead_tail=NULL;s->ahead_count=0;
    nmedia_ff_free(s->ahead_pixels);s->ahead_pixels=NULL;s->ahead_capacity=0;s->ahead.kind=0;
}
static void unload(struct web_avmedia *s) {
    if(s->doc&&s->doc->pip_element==s->node)web_presentation_exit(s->doc,2);
    snapshot_clear(s);
    caption_cues_free(s->caption_cues,s->caption_count);s->caption_cues=NULL;s->caption_count=0;
    nmedia_ff_free(s->caption_flow);s->caption_flow=NULL;
    clear_ahead(s);
    close_audio(s); nmedia_close(s->decoder); s->decoder = NULL;
    nmedia_worker_close(s->worker);s->worker=NULL;
    nmedia_mse_worker_close(s->mse_worker);s->mse_worker=NULL;nmedia_ff_free(s->mse);s->mse=NULL;s->mse_count=s->mse_capacity=0;s->mse_seeking=s->mse_quota_error=false;
    s->mse_resume_needed=s->mse_preparing=false;
    s->mse_active=s->starved=s->mse_eos=false;s->mse_duration=NAN;
    nmedia_ff_free(s->pixels); s->pixels = NULL; s->pixel_capacity = 0;
    s->width = s->height = 0; s->playing = s->ended = false;
    s->url_input = false;
    nmedia_ff_free(s->range_url);s->range_url=NULL;memset(&s->range_info,0,sizeof s->range_info);
    s->range_info.duration_ms=-1;s->range_waiting=s->range_resume=s->range_seek_sent=s->range_keep=false;
    s->audio_enabled=s->video_enabled=true;
    s->base_ms = 0; s->input_size = 0; s->audio_at = 0; s->pending.kind = 0; s->ready = 0; s->error[0] = 0; s->caption[0] = 0;
    s->video_frames=s->dropped_video_frames=0;
    s->performance_frames=s->performance_work_ms=s->performance_measurement_id=0;
    memset(&s->performance_audio,0,sizeof s->performance_audio);
    s->presented_frames=s->audio_frames=s->audio_silence_frames=s->profile_at=0;s->presented_ms=s->audio_end_ms=-1;s->audio_end_frame=0;s->audio_written=s->profile_ended=false;
    s->window_frames=0;
    s->profile_pumps=s->profile_services=s->profile_pump_ms=s->profile_pump_at=s->profile_pump_gap=0;
}
static int64_t current(struct web_avmedia *s, uint64_t now) {
    const struct nmedia_info *info = information(s);
    int64_t ms = s->base_ms + (s->playing && !s->starved && !nmedia_worker_seeking(s->worker) && now >= s->started ? (int64_t)((now - s->started)*s->rate) : 0);
    /* An enabled audio track has not started until real PCM is submitted.
       Preroll/backend readiness must not advance a silent wall clock and
       cause video to be discarded before the first audible sample. */
    if(s->playing&&s->audio_enabled&&s->fd>=0&&!s->audio_written&&info&&info->audio)ms=s->base_ms;
    /* Wall time is not audible progress while JS/paint delays PCM supply.
     * Use this stream's consumed PCM timeline once a write establishes it.
     * Empty software queue freezes at the last supplied sample, not at a
     * fictitious future wall position. Silent/disabled tracks keep wall time. */
    if(s->playing&&s->audio_enabled&&s->fd>=0&&s->audio_written){
        /* One bounded native pump uses one device-consumption snapshot, not
           a read syscall for every video candidate comparison. PCM writes
           and sink replacement invalidate it; author getters outside the
           pump always read current device state. */
        struct n_audio_status status={0};ssize_t bytes;
        if(s->audio_clock_failed)bytes=-1;
        else if(s->clock_snapshot_enabled&&s->clock_snapshot_valid){status=s->clock_snapshot;bytes=s->clock_snapshot_bytes;}
        else {bytes=read(s->fd,&status,sizeof status);if(s->clock_snapshot_enabled){s->clock_snapshot=status;s->clock_snapshot_bytes=bytes;s->clock_snapshot_valid=true;}}
        if(bytes==sizeof status||bytes==sizeof(uint32_t)){uint64_t queued=(uint64_t)status.queued_frames+status.device_pending_frames;
            int64_t frames=s->audio_end_frame-(int64_t)lround(queued*s->rate);ms=MAX(s->base_ms,frames/SOUND_RATE*1000+(frames%SOUND_RATE)*1000/SOUND_RATE);
            s->audio_played_ms=ms;s->audio_played_valid=true;
        }else {
            /* A failed native sink is not silent-video playback. Retain the
               last confirmed consumption; cleanup belongs to the pump, not
               to a geometry/time getter holding a picture/PCM loan. */
            s->audio_clock_failed=true;ms=s->audio_played_valid?s->audio_played_ms:s->base_ms;
        }
    }
    if (info && info->duration_ms >= 0 && ms > info->duration_ms) ms = info->duration_ms;
    return ms;
}
static void audio_failed(struct web_avmedia *s,uint64_t now){
    s->audio_clock_failed=true;s->base_ms=current(s,now);s->playing=false;
    strlcpy(s->error,"native audio consumption status failed",sizeof s->error);
    s->pending.kind=0;s->audio_at=0;close_audio(s);clear_ahead(s);s->doc->dirty=true;
}
static bool audio_queue(struct web_avmedia *s,uint32_t *queued,uint64_t now){
    *queued=0;if(s->fd<0)return true;
    if(s->audio_clock_failed||read(s->fd,queued,sizeof *queued)!=sizeof *queued){audio_failed(s,now);return false;}
    return true;
}
static bool audio_target_frame(struct web_avmedia *s,int64_t *target){
    const int factor=SOUND_RATE/1000; /* the native sink is exactly 48 kHz */
    int64_t pts=s->pending.pts_ms;
    int64_t consumed=(int64_t)lround(s->audio_at*s->rate);
    if(pts>INT64_MAX/factor||pts<INT64_MIN/factor||pts*factor>INT64_MAX-consumed){strlcpy(s->error,"native audio timebase overflow",sizeof s->error);return false;}
    *target=pts*factor+consumed;return true;
}
/* 0: PCM starts here/overlaps, 1: silence committed, 2: await sink room,
 * -1: real write/size failure. Fill only the positive authored PTS gap, never
 * wall elapsed time. The existing frame/byte/packet burst still bounds work. */
static int audio_fill_gap(struct web_avmedia *s,uint32_t queued){
    int64_t target;if(!audio_target_frame(s,&target))return -1;
    if(!s->audio_written){
        if(s->base_ms>INT64_MAX/(SOUND_RATE/1000)||s->base_ms<INT64_MIN/(SOUND_RATE/1000)){strlcpy(s->error,"native audio base time overflow",sizeof s->error);return -1;}
        s->audio_end_frame=s->base_ms*(SOUND_RATE/1000);
    }
    if(target<=s->audio_end_frame)return 0;
    if(s->audio_end_frame<0&&target>INT64_MAX+s->audio_end_frame){strlcpy(s->error,"native audio gap overflow",sizeof s->error);return -1;}
    if(queued>=WEB_AUDIO_TARGET)return 2;
    size_t frames=(size_t)MIN(MIN((int64_t)ceil((target-s->audio_end_frame)/s->rate),(int64_t)(WEB_AUDIO_TARGET-queued)),4096);
    memset(s->scaled,0,frames*4);ssize_t n=write(s->fd,s->scaled,frames*4);
    s->clock_snapshot_valid=false;
    if(n<0&&errno==EAGAIN)return 2;
    if(n<=0||(n&3)||(size_t)n>frames*4){strlcpy(s->error,"native audio gap write failed",sizeof s->error);return -1;}
    s->audio_end_frame+=(int64_t)lround((n/4)*s->rate);s->audio_end_ms=s->audio_end_frame/SOUND_RATE*1000+(s->audio_end_frame%SOUND_RATE)*1000/SOUND_RATE;
    s->audio_frames+=(size_t)n/4;s->audio_silence_frames+=(size_t)n/4;s->audio_written=true;return 1;
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
    nmedia_ff_free(s->range_url);s->range_url=NULL;
    s->doc->dirty=true;
}
static bool seek(struct web_avmedia *s, int64_t ms, uint64_t now) {
    snapshot_clear(s);
    if(s->mse_active){
        if(!nmedia_mse_worker_command(s->mse_worker,NMSW_SEEK,0,NULL,0,0,ms,0,false))return false;
        s->mse_resume_needed=s->mse_preparing=false; /* An authored seek supersedes internal resume. */
        clear_ahead(s);
        close_audio(s);s->base_ms=ms;s->started=now;s->pending.kind=0;s->audio_at=0;s->ended=false;s->starved=true;s->mse_seeking=true;s->ready=1;return true;
    }
    if(s->url_input&&!s->worker&&s->range_url) {
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
/* Pause is not a seek. The sink must flush unplayed PCM to become silent,
 * so retain a private rewind requirement rather than changing public
 * seeking/readyState or resetting SourceBuffer decoders during pause. */
static void mse_pause(struct web_avmedia *s,uint64_t now){
    if(!s->playing&&!s->mse_preparing)return;
    int64_t ms=current(s,now);s->base_ms=ms;s->playing=false;
    s->mse_resume_needed=!s->mse_seeking;s->mse_preparing=false;
    s->pending.kind=0;s->audio_at=0;close_audio(s);
}
static bool mse_prepare_play(struct web_avmedia *s){
    if(s->error[0])return false;
    if(!s->mse_active||!s->mse_resume_needed)return true;
    if(!nmedia_mse_worker_command(s->mse_worker,NMSW_SEEK,0,NULL,0,0,s->base_ms,0,false)){
        strlcpy(s->error,"MSE resume preparation queue allocation/size failure",sizeof s->error);return false;
    }
    clear_ahead(s);s->pending.kind=0;s->audio_at=0;
    s->mse_resume_needed=false;s->mse_preparing=true;
    transition(s,"mse-resume-prepare",s->generation);return true;
}
/* Commit metadata only after display owns a stable picture. Paint is
 * synchronous; snapshots own composed pixels, not a retained display span. */
static void picture_committed(struct web_avmedia *s,const struct nmedia_output *o,int slot,uint64_t revision){
    if(o->video_measurement_id&&o->video_measured_frames&&o->video_work_ms>=o->video_measured_frames){
        if(s->performance_generation!=s->generation||s->performance_width!=o->width||
           s->performance_height!=o->height||o->video_measured_frames<s->performance_frames||
           o->video_work_ms<s->performance_work_ms||s->performance_measurement_id!=o->video_measurement_id||
           !nmedia_video_configuration_equal(&s->performance_configuration,&o->video_configuration)){
            s->performance_configuration=o->video_configuration;
            s->performance_measurement_id=o->video_measurement_id;
            s->performance_width=o->width;s->performance_height=o->height;
            s->performance_generation=s->generation;
        }
        s->performance_configuration.bitrate=o->video_configuration.bitrate;
        s->performance_slot=slot;s->performance_revision=revision;
        s->performance_frames=o->video_measured_frames;s->performance_work_ms=o->video_work_ms;
        s->performance_at=uptime_ms();
    }else s->performance_frames=s->performance_work_ms=s->performance_measurement_id=0;
    bool size_changed=s->width!=o->width||s->height!=o->height;
    s->width=o->width;s->height=o->height;
    if(size_changed){if(s->node&&s->node->box)layout_invalidate(s->node->box);s->doc->layout_valid=false;s->doc->dirty=true;s->snapshot.valid=false;}
    else if(!s->snapshot.valid||s->generation!=s->snapshot.generation)doc_paint_damage(s->doc,s->node);
    s->presented_frames++;s->presented_ms=o->pts_ms;
}
/* Both isolated transports can exchange a complete taken VIDEO owner before
 * successor dispatch. AUDIO and the in-process decoder still borrow spans. */
static bool move_video(struct web_avmedia *s,const struct nmedia_output *o,uint32_t **pixels,size_t *capacity){
    if(s->worker)return nmedia_worker_move_video(s->worker,o,pixels,capacity);
    return s->mse_active&&s->mse_worker&&s->mse_pending_slot>=0&&
        nmedia_mse_worker_move_video(s->mse_worker,o,s->generation,(unsigned)s->mse_pending_slot,
            s->mse_pending_revision,pixels,capacity);
}
static bool present(struct web_avmedia *s) {
    if(!s->video_enabled)return true;
    struct nmedia_output *o=&s->pending;
    size_t count,bytes;
    if(!o->pixels||!nmedia_video_size(o->width,o->height,&count,&bytes)){strlcpy(s->error,"invalid native video span",sizeof s->error);return false;}
    if(!move_video(s,o,&s->pixels,&s->pixel_capacity)){
        if(count>s->pixel_capacity){uint32_t *p=nmedia_ff_realloc(s->pixels,bytes);if(!p){strlcpy(s->error,"video presentation allocation",sizeof s->error);return false;}s->pixels=p;s->pixel_capacity=count;}
        memcpy(s->pixels,o->pixels,bytes);
    }
    picture_committed(s,o,s->mse_pending_slot,s->mse_pending_revision);return true;
}
static bool hold_video(struct web_avmedia *s) {
    const struct nmedia_output *o=&s->pending;
    size_t count,bytes;
    if(!o->pixels||!nmedia_video_size(o->width,o->height,&count,&bytes)||s->ahead_count==SIZE_MAX){strlcpy(s->error,"invalid native lookahead span/count",sizeof s->error);return false;}
    if(s->ahead.kind){
        struct web_video_ahead *v=nmedia_ff_mallocz(sizeof *v);if(!v){strlcpy(s->error,"video lookahead record allocation",sizeof s->error);return false;}
        uint32_t *pixels=NULL;size_t capacity=0;
        if(!move_video(s,o,&pixels,&capacity)){
            pixels=nmedia_ff_malloc(bytes);if(!pixels){nmedia_ff_free(v);strlcpy(s->error,"video lookahead allocation",sizeof s->error);return false;}
            memcpy(pixels,o->pixels,bytes);capacity=count;
        }
        v->output=*o;v->output.pixels=pixels;v->capacity=capacity;v->slot=s->mse_pending_slot;v->revision=s->mse_pending_revision;v->generation=s->generation;
        if(s->ahead_tail)s->ahead_tail->next=v;else s->ahead_next=v;s->ahead_tail=v;s->ahead_count++;return true;
    }
    if(!move_video(s,o,&s->ahead_pixels,&s->ahead_capacity)){
        if(count>s->ahead_capacity){uint32_t *p=nmedia_ff_realloc(s->ahead_pixels,bytes);if(!p){strlcpy(s->error,"video lookahead allocation",sizeof s->error);return false;}s->ahead_pixels=p;s->ahead_capacity=count;}
        memcpy(s->ahead_pixels,o->pixels,bytes);
    }
    s->ahead=*o;s->ahead.pixels=s->ahead_pixels;
    s->ahead_slot=s->mse_pending_slot;s->ahead_revision=s->mse_pending_revision;s->ahead_generation=s->generation;s->ahead_count=1;return true;
}
static void advance_ahead(struct web_avmedia *s){
    if(s->ahead_count)s->ahead_count--;
    struct web_video_ahead *v=s->ahead_next;
    if(!v){s->ahead.kind=0;return;}
    s->ahead_next=v->next;if(!s->ahead_next)s->ahead_tail=NULL;
    nmedia_ff_free(s->ahead_pixels);s->ahead_pixels=(uint32_t *)v->output.pixels;s->ahead_capacity=v->capacity;s->ahead=v->output;
    s->ahead_slot=v->slot;s->ahead_revision=v->revision;s->ahead_generation=v->generation;nmedia_ff_free(v);
}
static bool present_ahead(struct web_avmedia *s,uint64_t now) {
    if(!s->ahead.kind)return true;
    if(s->ahead_generation!=s->generation||(s->mse_active&&s->ahead_revision!=nmedia_mse_worker_revision(s->mse_worker,(unsigned)s->ahead_slot))){advance_ahead(s);return true;}
    if(s->ahead.pts_ms>current(s,now)+5)return true;
    int64_t due=current(s,now)+5;
    struct web_video_ahead *next=s->ahead_next;
    bool newer=(s->pending.kind==NMEDIA_VIDEO&&s->pending.pts_ms>s->ahead.pts_ms&&s->pending.pts_ms<=due)||
        (next&&next->generation==s->generation&&next->output.pts_ms>s->ahead.pts_ms&&next->output.pts_ms<=due&&
         (!s->mse_active||next->revision==nmedia_mse_worker_revision(s->mse_worker,(unsigned)next->slot)));
    /* A late but unique real image is still the only image we can present.
       Drop it only for an actually owned, due, newer decoded picture. */
    if(newer){s->dropped_video_frames++;advance_ahead(s);return true;}
    if(!s->video_enabled){advance_ahead(s);return true;}
    size_t count;
    if(s->ahead.pixels!=s->ahead_pixels||!nmedia_video_size(s->ahead.width,s->ahead.height,&count,NULL)||count>s->ahead_capacity){strlcpy(s->error,"invalid owned video span",sizeof s->error);return false;}
    struct nmedia_output picture=s->ahead;
    uint32_t *previous=s->pixels;size_t capacity=s->pixel_capacity;
    s->pixels=s->ahead_pixels;s->pixel_capacity=s->ahead_capacity;
    s->ahead_pixels=previous;s->ahead_capacity=capacity;s->ahead.pixels=previous;
    picture_committed(s,&picture,s->ahead_slot,s->ahead_revision);advance_ahead(s);return true;
}
/* Called only after taking/owning a VIDEO and before dispatching its next
 * STEP. The child's PTS/BUSY merge has already tried the independent audio
 * decoder. A future picture past every coded audio range plus a drained PCM
 * sink means audio input is exhausted, not a reason to decode all remaining
 * buffered video. Codec trim may separate nominal packet and PCM endpoints:
 * never advance the real sample clock to the nominal range end. */
static bool mse_audio_input_wait(struct web_avmedia *s,uint64_t now){
    if(!s->mse_active||s->mse_eos||s->mse_seeking||!s->playing||!s->audio_enabled||s->fd<0||!s->audio_written||
       !s->ahead.kind||s->ahead_generation!=s->generation||s->pending.kind==NMEDIA_AUDIO)return false;
    int64_t horizon=-1;bool audio=false,video=false;
    for(size_t i=0;i<s->mse_count;i++)if(s->mse[i].used){
        const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,(unsigned)i);if(!info)continue;
        if(info->audio&&info->video)return false; /* Muxed legal video-first demux must continue. */
        if(info->video)video=true;
        if(info->audio){size_t count;audio=true;const struct nmedia_time_range *ranges=nmedia_mse_worker_ranges_view(s->mse_worker,(unsigned)i,&count);
            if(count)horizon=MAX(horizon,ranges[count-1].end_ms);}
    }
    /* A future coded audio range, including an intentional timestamp gap,
       keeps demand active so its real PTS can supply PCM/gap completion. */
    if(!audio||!video||horizon<0||s->ahead.pts_ms<horizon)return false;
    uint32_t queued;if(!audio_queue(s,&queued,now))return true;if(queued)return false;
    int64_t played=current(s,now);
    if(played!=s->audio_end_ms||s->ahead.pts_ms<=played+5)return false;
    if(!s->starved){s->base_ms=played;s->starved=true;s->ready=1;s->doc->dirty=true;}
    return true;
}
static int ready_state(struct web_avmedia *s,uint64_t now) {
    if(!s || !s->mse_active || s->ready<2)return s?s->ready:0;
    /* MSE's buffered intersection, not paused or one decoder ACK, determines
       whether the current position has future data. Never advertise an empty
       or missing selected track as playable. These are the existing native
       coded-frame ranges; no synthesized samples or time advance is involved. */
    int64_t at=current(s,now),end=INT64_MAX;
    bool active=false;
    for(size_t i=0;i<s->mse_count;i++)if(s->mse[i].used){
        const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,(unsigned)i);
        if(!info)return 1;
        if(!(info->audio&&s->audio_enabled) && !(info->video&&s->video_enabled))continue;
        active=true;
        size_t count;const struct nmedia_time_range *ranges=nmedia_mse_worker_ranges_view(s->mse_worker,(unsigned)i,&count);
        size_t j=0;while(j<count && !(ranges[j].start_ms<=at && at<ranges[j].end_ms))j++;
        if(j==count)return s->ended?2:1;
        end=MIN(end,ranges[j].end_ms);
    }
    if(!active || s->ended)return 2;
    if(end<=at)return 2;
    /* End-of-stream with the remaining interval buffered cannot gain more
       data by waiting. Live/partial inputs remain HAVE_FUTURE_DATA: a fixed
       window is not evidence of uninterrupted playback to the end. */
    if(s->mse_eos && isfinite(s->mse_duration) &&
       end/1000.0>=s->mse_duration)return 4;
    return 3;
}
static JSValue state(JSContext *ctx, struct web_avmedia *s) {
    JSValue o=JS_NewObject(ctx);
    uint64_t decoded,dropped;video_counts(s,&decoded,&dropped);
    const struct nmedia_info *info=s?information(s):NULL;
    JS_SetPropertyStr(ctx,o,"readyState",JS_NewInt32(ctx,ready_state(s,uptime_ms())));
    JS_SetPropertyStr(ctx,o,"duration",JS_NewFloat64(ctx,s&&s->mse_active?s->mse_duration:info&&info->duration_ms>=0?info->duration_ms/1000.0:NAN));
    JS_SetPropertyStr(ctx,o,"currentTime",JS_NewFloat64(ctx,s?current(s,uptime_ms())/1000.0:0));
    JS_SetPropertyStr(ctx,o,"paused",JS_NewBool(ctx,!s||!s->playing));
    JS_SetPropertyStr(ctx,o,"ended",JS_NewBool(ctx,s&&s->ended));
    JS_SetPropertyStr(ctx,o,"videoWidth",JS_NewInt32(ctx,info?info->width:0));
    JS_SetPropertyStr(ctx,o,"videoHeight",JS_NewInt32(ctx,info?info->height:0));
    JS_SetPropertyStr(ctx,o,"totalVideoFrames",JS_NewFloat64(ctx,(double)decoded));
    JS_SetPropertyStr(ctx,o,"droppedVideoFrames",JS_NewFloat64(ctx,(double)dropped));
    JS_SetPropertyStr(ctx,o,"error",s&&s->error[0]?JS_NewString(ctx,s->error):JS_NULL);
    JS_SetPropertyStr(ctx,o,"rangeInput",JS_NewBool(ctx,s&&s->url_input));
    JS_SetPropertyStr(ctx,o,"queued",JS_NewBool(ctx,s&&s->range_waiting&&!s->worker));
    JS_SetPropertyStr(ctx,o,"loading",JS_NewBool(ctx,s&&(s->range_waiting||nmedia_worker_loading(s->worker))));
    JS_SetPropertyStr(ctx,o,"seeking",JS_NewBool(ctx,s&&(s->range_resume||s->mse_seeking||nmedia_worker_seeking(s->worker))));
    JS_SetPropertyStr(ctx,o,"playPreparing",JS_NewBool(ctx,s&&s->mse_preparing));
    JS_SetPropertyStr(ctx,o,"msePending",JS_NewBool(ctx,s&&nmedia_mse_worker_pending(s->mse_worker)));
    JS_SetPropertyStr(ctx,o,"mseQuotaError",JS_NewBool(ctx,s&&(s->mse_quota_error||nmedia_mse_worker_quota_error(s->mse_worker))));
    JS_SetPropertyStr(ctx,o,"waiting",JS_NewBool(ctx,s&&s->starved));
    JS_SetPropertyStr(ctx,o,"keyWaiting",JS_NewBool(ctx,s&&nmedia_mse_worker_key_waiting(s->mse_worker)));
    return o;
}
static int mse_next(struct web_avmedia *s,struct nmedia_output *out,uint64_t until,bool cooperative){
    int r=nmedia_mse_worker_step(s->mse_worker,out);
    if(cooperative){if(r==NMSW_BUSY){nmedia_mse_worker_service(s->mse_worker,uptime_ms(),until);r=nmedia_mse_worker_step(s->mse_worker,out);}goto completed;}
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
completed:
    if(r==NMEDIA_ERROR)strlcpy(s->error,nmedia_mse_worker_error(s->mse_worker),sizeof s->error);
    if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){s->mse_pending_slot=(int)nmedia_mse_worker_output_slot(s->mse_worker);s->mse_pending_revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)s->mse_pending_slot);}
    return r;
}
static bool mse_prepare_pump(struct web_avmedia *s,uint64_t until,bool cooperative){
    /* Initial preload and resume share the real decoded-output preparation
       path. A paused element may commit its first frame without opening the
       audio sink or advancing its retained playback position. */
    bool initial=s->mse_active && s->ready<2 && !s->playing &&
        !s->mse_resume_needed && !s->mse_seeking;
    if(!s->mse_preparing && !initial)return false;
    /* Resume does not present a future picture. Initial preload may present
       its first picture below. ACK alone is insufficient in either case. */
    if(nmedia_mse_worker_seeking(s->mse_worker))return true;
    if(!s->pending.kind){int r=mse_next(s,&s->pending,until,cooperative);s->audio_at=0;
        if(r==NMEDIA_VIDEO)s->video_frames++;
        if(r==NMSW_BUSY||r==NMEDIA_AGAIN)return true;
        if(r==NMEDIA_ERROR){s->mse_preparing=false;s->playing=false;close_audio(s);s->doc->dirty=true;return true;}
    }
    if(s->pending.kind==NMEDIA_AUDIO||s->pending.kind==NMEDIA_VIDEO||s->pending.kind==NMSW_CLOCK){
        s->ready=2;s->starved=false;
        /* Keep initial PCM as the existing decoder loan until playback, with
           no sink opening, time advance, or silent substitute samples. A real
           first picture can be committed while the loaded element is paused. */
        if(initial && s->pending.kind==NMEDIA_VIDEO){
            if(!present(s)){s->mse_preparing=false;s->doc->dirty=true;return true;}
            s->pending.kind=0;
        }
    }
    s->mse_preparing=false;
    if(cooperative){if(debug_transitions)s->transition_pending="mse-resume-prepared";}
    else if(!initial)transition(s,"mse-resume-prepared",s->generation);
    return true;
}
static int range_next(struct web_avmedia *s,struct nmedia_output *out,struct nmedia_worker_budget *b){
    int r=nmedia_worker_take(s->worker,out);
    if(r!=NMEDIA_AGAIN)return r;
    nmedia_worker_step(s->worker);
    /* Decode/network runs in the existing child. Only finite nonblocking
     * dispatch/reap and scheduler handoffs run here; no new burst allowance. */
    if(b->nonblocking){nmedia_worker_pump_budget(s->worker,uptime_ms(),b,false);return nmedia_worker_take(s->worker,out);}
    while(nmedia_worker_pending(s->worker)&&uptime_ms()<b->until){
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
static bool mse_reserve(struct web_avmedia *s,size_t count){
    if(count<=s->mse_capacity)return true;size_t capacity=s->mse_capacity?s->mse_capacity:4;
    while(capacity<count){if(capacity>SIZE_MAX/sizeof *s->mse/2){capacity=count;break;}capacity*=2;}
    if(capacity>SIZE_MAX/sizeof *s->mse)return false;struct web_mse_slot *v=nmedia_ff_realloc(s->mse,capacity*sizeof *v);if(!v)return false;
    memset(v+s->mse_capacity,0,(capacity-s->mse_capacity)*sizeof *v);s->mse=v;s->mse_capacity=capacity;return true;
}
static JSValue mse_queue(JSContext *ctx,struct web_avmedia *s,unsigned op,unsigned slot,const void *bytes,size_t size,int64_t offset,int64_t a,int64_t b,bool flag){
    if(!nmedia_mse_worker_command(s->mse_worker,op,slot,bytes,size,offset,a,b,flag))return JS_ThrowOutOfMemory(ctx);
    return JS_TRUE;
}
static JSValue mse_call(JSContext *ctx,struct web_avmedia *s,const char *op,int argc,JSValueConst *argv,uint64_t now){
    nmedia_mse_worker_time(s->mse_worker,current(s,now));
    if(!strcmp(op,"mseSelect")){
        if(argc<2)return JS_FALSE;const char *kind=JS_ToCString(ctx,argv[0]);if(!kind)return JS_EXCEPTION;
        int selected=JS_ToBool(ctx,argv[1]);if(selected<0){JS_FreeCString(ctx,kind);return JS_EXCEPTION;}bool audio=!strcmp(kind,"audio"),video=!strcmp(kind,"video");JS_FreeCString(ctx,kind);
        if(!audio&&!video)return JS_ThrowTypeError(ctx,"invalid media track kind");bool *enabled=audio?&s->audio_enabled:&s->video_enabled;if(*enabled==!!selected)return JS_TRUE;
        if(s->mse_active&&!nmedia_mse_worker_command(s->mse_worker,NMSW_SELECT,audio?0:1,NULL,0,0,0,0,selected))return JS_ThrowOutOfMemory(ctx);
        if(audio){s->base_ms=current(s,now);s->started=now;}
        *enabled=!!selected;if(audio){close_audio(s);if(selected&&s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}
        else {if(!selected){clear_ahead(s);nmedia_ff_free(s->pixels);s->pixels=NULL;s->pixel_capacity=0;s->width=s->height=0;s->doc->dirty=true;}else if(s->playing&&!seek(s,current(s,now),now))return JS_FALSE;}return JS_TRUE;
    }
    if(!strcmp(op,"mseCreate")){
        uint32_t generation;if(!argc||JS_ToUint32(ctx,&generation,argv[0])<0)return JS_EXCEPTION;
        if(s->generation!=generation)return JS_FALSE;transition(s,"mse-create",generation);unload(s);s->generation=generation;s->mse_worker=nmedia_mse_worker_open(generation,s->error,sizeof s->error);
        if(s->mse_worker&&s->eme_key_bytes&&!nmedia_mse_worker_command(s->mse_worker,NMSW_KEYS,0,s->eme_keys,s->eme_key_bytes,0,0,0,false))return JS_ThrowOutOfMemory(ctx);
        if(!s->mse_worker)return JS_FALSE;s->mse_active=true;s->starved=true;s->mse_duration=NAN;return JS_TRUE;
    }
    if(!s->mse_active)return JS_FALSE;
    if(!strcmp(op,"mseDuration")){
        double seconds;if(!argc||JS_ToFloat64(ctx,&seconds,argv[0])<0)return JS_EXCEPTION;
        if(isnan(seconds)||seconds<0||(isfinite(seconds)&&seconds>1e9))return JS_ThrowRangeError(ctx,"invalid MSE duration");
        if(isfinite(seconds))for(size_t i=0;i<s->mse_count;i++)if(s->mse[i].used){size_t count;const struct nmedia_time_range *ranges=nmedia_mse_worker_ranges_view(s->mse_worker,(unsigned)i,&count);
            /* Compare in the same seconds representation exposed by buffered.
               Multiplying that rounded double back into milliseconds could
               turn an unchanged EOS duration into a spurious 1ms removal. */
            if(count&&ranges[count-1].end_ms/1000.0>seconds){JSValue result=mse_queue(ctx,s,NMSW_REMOVE,i,NULL,0,0,(int64_t)ceil(seconds*1000.0),INT64_MAX,false);if(JS_IsException(result))return result;}}
        s->mse_duration=seconds;return JS_TRUE;
    }
    if(!strcmp(op,"mseEnd")||!strcmp(op,"mseReopen")){
        bool eos=!strcmp(op,"mseEnd");const char *error=eos&&argc?JS_ToCString(ctx,argv[0]):NULL;if(eos&&argc&&!error)return JS_EXCEPTION;
        if(error&&*error){if(strcmp(error,"network")&&strcmp(error,"decode")){JS_FreeCString(ctx,error);return JS_ThrowTypeError(ctx,"invalid MSE end error");}
            strlcpy(s->error,!strcmp(error,"network")?"MSE network error":"MSE decode error",sizeof s->error);s->base_ms=current(s,now);s->playing=false;s->mse_resume_needed=s->mse_preparing=false;close_audio(s);clear_ahead(s);JS_FreeCString(ctx,error);return JS_TRUE;}
        JS_FreeCString(ctx,error);s->mse_eos=eos;s->mse_eos_duration=false;s->ended=false;return mse_queue(ctx,s,NMSW_END,0,NULL,0,0,0,0,eos);
    }
    uint32_t id;if(!argc||JS_ToUint32(ctx,&id,argv[0])<0)return JS_EXCEPTION;
    int slot=-1;for(size_t i=0;i<s->mse_count;i++)if(s->mse[i].used&&s->mse[i].id==id)slot=i;
    if(!strcmp(op,"mseAdd")){
        if(slot>=0||!id||argc<2)return JS_FALSE;
        for(size_t i=0;i<s->mse_count;i++)if(!s->mse[i].used){slot=(int)i;break;}
        if(slot<0){if(s->mse_count>INT_MAX)return JS_ThrowRangeError(ctx,"MSE slot representation");if(!mse_reserve(s,s->mse_count+1))return JS_ThrowOutOfMemory(ctx);slot=(int)s->mse_count++;}
        const char *mime=JS_ToCString(ctx,argv[1]);if(!mime)return JS_EXCEPTION;
        if(!nmedia_mse_type(mime)){JS_FreeCString(ctx,mime);return JS_FALSE;}
        JSValue result=mse_queue(ctx,s,NMSW_ADD,(unsigned)slot,mime,strlen(mime),0,0,0,false);JS_FreeCString(ctx,mime);
        if(!JS_IsException(result)){s->mse[slot].used=true;s->mse[slot].id=id;}return result;
    }
    if(slot<0)return JS_FALSE;
    if(!strcmp(op,"mseInfo"))return mse_info(ctx,s,(unsigned)slot);
    if(!strcmp(op,"mseQuota"))return JS_NewFloat64(ctx,INFINITY);
    if(!strcmp(op,"mseRanges")){
        size_t count;const struct nmedia_time_range *ranges=nmedia_mse_worker_ranges_view(s->mse_worker,(unsigned)slot,&count);JSValue array=JS_NewArray(ctx);
        for(size_t i=0;i<count;i++){JSValue pair=JS_NewArray(ctx);JS_SetPropertyUint32(ctx,pair,0,JS_NewFloat64(ctx,ranges[i].start_ms/1000.0));JS_SetPropertyUint32(ctx,pair,1,JS_NewFloat64(ctx,ranges[i].end_ms/1000.0));JS_SetPropertyUint32(ctx,array,(uint32_t)i,pair);}return array;
    }
    if(!strcmp(op,"mseAppend")){
        double offset,a,b;size_t size;if(argc<6||JS_ToFloat64(ctx,&offset,argv[2])<0||JS_ToFloat64(ctx,&a,argv[3])<0||JS_ToFloat64(ctx,&b,argv[4])<0)return JS_EXCEPTION;
        if(!isfinite(offset)||fabs(offset)>1e9||!isfinite(a)||a<0||a>1e9||isnan(b)||b<=a||(isfinite(b)&&b>1e9))return JS_ThrowRangeError(ctx,"invalid MSE append properties");
        int sequence=JS_ToBool(ctx,argv[5]);if(sequence<0)return JS_EXCEPTION;uint8_t *bytes=JS_GetArrayBuffer(ctx,&size,argv[1]);if(!bytes)return JS_EXCEPTION;

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
static bool caption_number(JSContext *ctx,JSValueConst object,const char *name,double *out,bool automatic){JSValue v=JS_GetPropertyStr(ctx,object,name);if(JS_IsException(v))return false;if(automatic&&JS_IsString(v)){const char *s=JS_ToCString(ctx,v);bool ok=s&&!strcmp(s,"auto");JS_FreeCString(ctx,s);JS_FreeValue(ctx,v);if(ok){*out=NAN;return true;}JS_ThrowTypeError(ctx,"Invalid caption automatic value");return false;}int status=JS_ToFloat64(ctx,out,v);JS_FreeValue(ctx,v);if(status<0)return false;if(!isfinite(*out)){JS_ThrowTypeError(ctx,"Nonfinite caption layout");return false;}return true;}
static bool caption_enum(JSContext *ctx,JSValueConst object,const char *name,const char *const *names,int *out){JSValue v=JS_GetPropertyStr(ctx,object,name);if(JS_IsException(v))return false;const char *text=JS_ToCString(ctx,v);JS_FreeValue(ctx,v);if(!text)return false;for(int i=0;names[i];i++)if(!strcmp(text,names[i])){*out=i;JS_FreeCString(ctx,text);return true;}JS_FreeCString(ctx,text);JS_ThrowTypeError(ctx,"Invalid caption enum");return false;}
static bool caption_string(JSContext *ctx,JSValueConst object,const char *name,char **out,size_t *total){JSValue v=JS_GetPropertyStr(ctx,object,name);if(JS_IsException(v))return false;size_t size;const char *text=JS_ToCStringLen(ctx,&size,v);JS_FreeValue(ctx,v);if(!text)return false;if(size>2048-*total||memchr(text,0,size)){JS_FreeCString(ctx,text);JS_ThrowRangeError(ctx,"Caption glyph bytes exceed surface budget");return false;}char *copy=nmedia_ff_malloc(size+1);if(!copy){JS_FreeCString(ctx,text);JS_ThrowOutOfMemory(ctx);return false;}memcpy(copy,text,size+1);JS_FreeCString(ctx,text);*total+=size;*out=copy;return true;}
static bool caption_length(JSContext *ctx,JSValueConst array,uint32_t *length){if(JS_IsArray(ctx,array)!=1){JS_ThrowTypeError(ctx,"Caption sequence required");return false;}JSValue v=JS_GetPropertyStr(ctx,array,"length");int r=JS_ToUint32(ctx,length,v);JS_FreeValue(ctx,v);return r>=0;}
static bool captions_equal(const struct caption_cue *a,const struct caption_cue *b,size_t count){for(size_t i=0;i<count;i++){if(a[i].count!=b[i].count||memcmp(&a[i].line,&b[i].line,offsetof(struct caption_cue,region_group)-offsetof(struct caption_cue,line)))return false;for(size_t j=0;j<a[i].count;j++){const struct caption_span *x=&a[i].spans[j],*y=&b[i].spans[j];if(x->flags!=y->flags||x->color!=y->color||strcmp(x->text,y->text)||strcmp(x->ruby,y->ruby))return false;}}return true;}
static int caption_id_compare(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return x<y?-1:x>y?1:0;}
static JSValue caption_receive(JSContext *ctx,web_doc *d,struct web_avmedia *s,JSValueConst input){
    static const char *const vertical[]={"","rl","lr",NULL},*const align[]={"start","center","end","left","right",NULL},*const line_align[]={"start","center","end",NULL},*const position_align[]={"auto","line-left","center","line-right",NULL};
    uint32_t count;if(!caption_length(ctx,input,&count))return JS_EXCEPTION;size_t cue_bytes=(size_t)count*sizeof(struct caption_cue);if(count&&cue_bytes/count!=sizeof(struct caption_cue))return JS_ThrowRangeError(ctx,"Caption sequence allocation overflow");
    struct caption_cue *cues=count?nmedia_ff_mallocz((size_t)count*sizeof *cues):NULL;if(count&&!cues)return JS_ThrowOutOfMemory(ctx);size_t total=0;uint32_t generation=s->generation;
    for(uint32_t i=0;i<count;i++){struct caption_cue *c=&cues[i];JSValue object=JS_GetPropertyUint32(ctx,input,i),spans=JS_GetPropertyStr(ctx,object,"spans"),region=JS_GetPropertyStr(ctx,object,"region");bool ok=!JS_IsException(object)&&!JS_IsException(spans)&&!JS_IsException(region);uint32_t length=0;
        if(ok)ok=caption_length(ctx,spans,&length)&&caption_number(ctx,object,"line",&c->line,true)&&caption_number(ctx,object,"position",&c->position,true)&&caption_number(ctx,object,"size",&c->size,false)&&caption_enum(ctx,object,"vertical",vertical,&c->vertical)&&caption_enum(ctx,object,"align",align,&c->align)&&caption_enum(ctx,object,"lineAlign",line_align,&c->line_align)&&caption_enum(ctx,object,"positionAlign",position_align,&c->position_align);
        JSValue snap=JS_GetPropertyStr(ctx,object,"snapToLines");int truth=JS_ToBool(ctx,snap);JS_FreeValue(ctx,snap);if(truth<0)ok=false;c->snap=truth>0;
        if(ok&&(c->size<0||c->size>100||(isfinite(c->position)&&(c->position<0||c->position>100)))){JS_ThrowRangeError(ctx,"Invalid caption percentage");ok=false;}
        size_t span_bytes=(size_t)length*sizeof(struct caption_span);if(ok&&length&&span_bytes/length!=sizeof(struct caption_span)){JS_ThrowRangeError(ctx,"Caption spans allocation overflow");ok=false;}
        if(ok){c->spans=length?nmedia_ff_mallocz((size_t)length*sizeof *c->spans):NULL;c->count=length;if(length&&!c->spans){c->count=0;JS_ThrowOutOfMemory(ctx);ok=false;}}
        for(uint32_t j=0;ok&&j<length;j++){JSValue span=JS_GetPropertyUint32(ctx,spans,j),flags=JS_GetPropertyStr(ctx,span,"flags"),color=JS_GetPropertyStr(ctx,span,"color");ok=!JS_IsException(span)&&caption_string(ctx,span,"text",&c->spans[j].text,&total)&&caption_string(ctx,span,"ruby",&c->spans[j].ruby,&total)&&JS_ToUint32(ctx,&c->spans[j].flags,flags)>=0&&JS_ToUint32(ctx,&c->spans[j].color,color)>=0;JS_FreeValue(ctx,color);JS_FreeValue(ctx,flags);JS_FreeValue(ctx,span);}
        if(ok&&!JS_IsNull(region)){c->region=true;double identity=0;ok=caption_number(ctx,region,"serial",&identity,false)&&identity>=0&&identity<=9007199254740991.0&&caption_number(ctx,region,"width",&c->width,false)&&caption_number(ctx,region,"regionAnchorX",&c->region_x,false)&&caption_number(ctx,region,"regionAnchorY",&c->region_y,false)&&caption_number(ctx,region,"viewportAnchorX",&c->viewport_x,false)&&caption_number(ctx,region,"viewportAnchorY",&c->viewport_y,false);c->region_id=(uint64_t)identity;JSValue lines=JS_GetPropertyStr(ctx,region,"lines"),scroll=JS_GetPropertyStr(ctx,region,"scroll");if(JS_ToUint32(ctx,&c->region_lines,lines)<0)ok=false;const char *text=JS_ToCString(ctx,scroll);if(!text)ok=false;else{c->scroll=!strcmp(text,"up");if(text[0]&&!c->scroll){JS_ThrowTypeError(ctx,"Invalid region scroll");ok=false;}}JS_FreeCString(ctx,text);JS_FreeValue(ctx,lines);JS_FreeValue(ctx,scroll);if(ok&&(c->width<0||c->width>100||c->region_x<0||c->region_x>100||c->region_y<0||c->region_y>100||c->viewport_x<0||c->viewport_x>100||c->viewport_y<0||c->viewport_y>100)){JS_ThrowRangeError(ctx,"Invalid region percentage");ok=false;}}
        JS_FreeValue(ctx,region);JS_FreeValue(ctx,spans);JS_FreeValue(ctx,object);if(!ok){caption_cues_free(cues,count);return JS_EXCEPTION;}
    }
    if(!d->live||s->generation!=generation){caption_cues_free(cues,count);return JS_ThrowTypeError(ctx,"Caption document changed during conversion");}
    if(s->caption_count==count&&!s->caption[0]&&captions_equal(cues,s->caption_cues,count)){caption_cues_free(cues,count);return JS_UNDEFINED;}
    int *flow=count?nmedia_ff_mallocz((size_t)count*sizeof *flow):NULL;uint64_t *ids=count?nmedia_ff_malloc((size_t)count*sizeof *ids):NULL;if(count&&(!flow||!ids)){nmedia_ff_free(flow);nmedia_ff_free(ids);caption_cues_free(cues,count);return JS_ThrowOutOfMemory(ctx);}for(uint32_t i=0;i<count;i++)ids[i]=cues[i].region_id;if(count)qsort(ids,count,sizeof *ids,caption_id_compare);for(uint32_t i=0;i<count;i++){size_t lo=0,hi=count;while(lo<hi){size_t at=lo+(hi-lo)/2;if(ids[at]<cues[i].region_id)lo=at+1;else hi=at;}cues[i].region_group=lo;}nmedia_ff_free(ids);
    caption_cues_free(s->caption_cues,s->caption_count);nmedia_ff_free(s->caption_flow);s->caption_cues=cues;s->caption_count=count;s->caption_flow=flow;s->caption[0]=0;s->caption_revision++;s->snapshot.valid=false;doc_paint_full(d);return JS_UNDEFINED;
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
    if(!strcmp(op,"decodingPerformance")){
        if(argc<5)return JS_ThrowTypeError(ctx,"Native decoding configuration required");
        const char *type=JS_ToCString(ctx,argv[0]);if(!type)return JS_EXCEPTION;
        double width,height,rate,bitrate;
        if(JS_ToFloat64(ctx,&width,argv[1])<0||JS_ToFloat64(ctx,&height,argv[2])<0||JS_ToFloat64(ctx,&rate,argv[3])<0||JS_ToFloat64(ctx,&bitrate,argv[4])<0){JS_FreeCString(ctx,type);return JS_EXCEPTION;}
        struct nmedia_video_configuration configuration;bool known=nmedia_performance_codec(type,&configuration),video_requested=!!*type;
        if(!isfinite(bitrate)||bitrate<=0||bitrate>9007199254740991.0||floor(bitrate)!=bitrate)known=false;
        else configuration.bitrate=(uint64_t)bitrate;
        uint32_t audio_codec=0,audio_profile=0;double channels=0,samplerate=0;bool audio_requested=argc>=8,audio_known=false;
        if(audio_requested){const char *audio_type=JS_ToCString(ctx,argv[5]);if(!audio_type){JS_FreeCString(ctx,type);return JS_EXCEPTION;}
            audio_known=nmedia_performance_audio(audio_type,&audio_codec,&audio_profile);JS_FreeCString(ctx,audio_type);
            if(JS_ToFloat64(ctx,&channels,argv[6])<0||JS_ToFloat64(ctx,&samplerate,argv[7])<0){JS_FreeCString(ctx,type);return JS_EXCEPTION;}
        }
        uint64_t now=uptime_ms();bool smooth=false;
        if(d&&(audio_requested||video_requested)&&(!video_requested||(known&&isfinite(rate)&&rate>0&&isfinite(width)&&isfinite(height)&&width>0&&height>0))){
            for(struct web_avmedia *sample=streams;sample;sample=sample->next){
                if(sample->doc!=d)continue;
                double work=0;
                if(video_requested){if(sample->performance_generation!=sample->generation||
                   sample->performance_width!=width||sample->performance_height!=height||
                   !sample->performance_measurement_id||!nmedia_video_configuration_equal(&sample->performance_configuration,&configuration)||sample->performance_frames<32||
                   !sample->performance_work_ms||now<sample->performance_at||now-sample->performance_at>60000)continue;
                if(sample->mse_active&&(sample->performance_slot<0||
                   sample->performance_revision!=nmedia_mse_worker_revision(sample->mse_worker,(unsigned)sample->performance_slot)))continue;
                work=(double)sample->performance_work_ms/(double)sample->performance_frames*rate;
                uint64_t measured_bitrate=sample->performance_configuration.bitrate;
                if(measured_bitrate&&bitrate>measured_bitrate)work*=bitrate/(double)measured_bitrate;
                }
                if(audio_requested){const struct nmedia_audio_observation *a=&sample->performance_audio;
                    if(!audio_known||sample->performance_audio_generation!=sample->generation||!a->id||a->frames<SOUND_RATE||
                       a->codec!=audio_codec||a->profile!=audio_profile||a->channels!=channels||a->rate!=samplerate||
                       !a->work_ms||now<sample->performance_audio_at||now-sample->performance_audio_at>60000)continue;
                    if(sample->mse_active&&(sample->performance_audio_slot<0||sample->performance_audio_revision!=
                       nmedia_mse_worker_revision(sample->mse_worker,(unsigned)sample->performance_audio_slot)))continue;
                    work+=(double)a->work_ms*SOUND_RATE/(double)a->frames;
                }
                /* Actual decoder+conversion work, with coarse-clock upper
                   rounding and 50% headroom for audio/compositor scheduling.
                   Unknown profiles/resolutions are not inferred as fast. */
                if(work<=500.0){smooth=true;break;}
            }
        }
        JS_FreeCString(ctx,type);return JS_NewBool(ctx,smooth);
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
    if(!s)return !strcmp(op,"state")?state(ctx,NULL):JS_ThrowRangeError(ctx,"native media state allocation failed");
    uint64_t now=uptime_ms();
    if(!strcmp(op,"state"))return state(ctx,s);
    if(!strcmp(op,"emeKeys")){
        if(!argc)return JS_ThrowTypeError(ctx,"native ClearKey snapshot required");
        size_t size=0;uint8_t *bytes=JS_GetArrayBuffer(ctx,&size,argv[0]);
        if(!bytes&&JS_HasException(ctx))return JS_EXCEPTION;
        if(size%32||size>UINT32_MAX)return JS_ThrowTypeError(ctx,"invalid native ClearKey snapshot");
        uint8_t *copy=size?nmedia_ff_malloc(size):NULL;if(size&&!copy)return JS_ThrowOutOfMemory(ctx);if(size)memcpy(copy,bytes,size);
        if(s->mse_worker&&!nmedia_mse_worker_command(s->mse_worker,NMSW_KEYS,0,copy,size,0,0,0,false)){if(copy){volatile uint8_t *wipe=copy;for(size_t i=0;i<size;i++)wipe[i]=0;nmedia_ff_free(copy);}return JS_ThrowOutOfMemory(ctx);}
        volatile uint8_t *old=s->eme_keys;for(size_t i=0;i<s->eme_key_bytes;i++)old[i]=0;nmedia_ff_free(s->eme_keys);s->eme_keys=copy;s->eme_key_bytes=size;return JS_TRUE;
    }
    if(!strcmp(op,"captionText")){
        if(!argc)return JS_ThrowTypeError(ctx,"caption text required");
        size_t length;const char *text=JS_ToCStringLen(ctx,&length,argv[0]);if(!text)return JS_EXCEPTION;
        if(length>sizeof(s->caption)-1||memchr(text,0,length)){JS_FreeCString(ctx,text);return JS_ThrowRangeError(ctx,"caption text exceeds bounded plain-text surface");}
        bool rich=s->caption_count>0;caption_cues_free(s->caption_cues,s->caption_count);nmedia_ff_free(s->caption_flow);s->caption_flow=NULL;s->caption_cues=NULL;s->caption_count=0;
        if(rich||strlen(s->caption)!=length||memcmp(s->caption,text,length)){s->snapshot.valid=false;memcpy(s->caption,text,length);s->caption[length]=0;s->caption_revision++;doc_paint_full(d);}
        JS_FreeCString(ctx,text);return JS_UNDEFINED;
    }
    if(!strcmp(op,"captionCues")){if(!argc)return JS_ThrowTypeError(ctx,"Caption cue sequence required");return caption_receive(ctx,d,s,argv[0]);}
    if(!strncmp(op,"mse",3))return mse_call(ctx,s,op,argc,argv,now);
    if(!strcmp(op,"reset")){
        uint32_t generation;if(!argc||JS_ToUint32(ctx,&generation,argv[0])<0)return JS_EXCEPTION;
        transition(s,"author-reset",generation);unload(s);s->generation=generation;d->dirty=true;return JS_UNDEFINED;
    }
    if(!strcmp(op,"load")||!strcmp(op,"loadURL")){
        uint32_t generation;size_t size=0;
        if(argc<2||JS_ToUint32(ctx,&generation,argv[1])<0)return JS_EXCEPTION;
        if(s->generation!=generation)return JS_FALSE;
        bool url_input=!strcmp(op,"loadURL");
        const char *url=NULL;uint8_t *bytes=NULL;char *range_url=NULL;bool range_requested=false;
        if(url_input) {
            size_t length;url=JS_ToCStringLen(ctx,&length,argv[0]);if(!url)return JS_EXCEPTION;
            /* String conversion may run author JS and replace this source.
             * Clone from the pinned CString before unloading any old owner. */
            if(s->generation!=generation){JS_FreeCString(ctx,url);return JS_FALSE;}
            if(length!=strlen(url)||!range_node(d,n,url)){JS_FreeCString(ctx,url);return JS_ThrowTypeError(ctx,"native media Range policy denied");}
            if(argc>2){int requested=JS_ToBool(ctx,argv[2]);if(requested<0){JS_FreeCString(ctx,url);return JS_EXCEPTION;}range_requested=requested!=0;}
            if(length==SIZE_MAX||length>UINT32_MAX){JS_FreeCString(ctx,url);return JS_ThrowRangeError(ctx,"native Range OPEN URL length is not representable");}
            range_url=nmedia_ff_malloc(length+1);
            if(!range_url){JS_FreeCString(ctx,url);return JS_ThrowOutOfMemory(ctx);}
            memcpy(range_url,url,length+1);size=NMEDIA_HTTP_CACHE_BYTES;
        } else {bytes=JS_GetArrayBuffer(ctx,&size,argv[0]);if(!bytes)return JS_EXCEPTION;}
        size_t total=0;for(struct web_avmedia *p=streams;p;p=p->next)if(p->doc==d&&p!=s){
            if(p->input_size>SIZE_MAX-total){nmedia_ff_free(range_url);JS_FreeCString(ctx,url);return JS_ThrowRangeError(ctx,"native media byte accounting overflow");}
            total+=p->input_size;
        }
        if(size>SIZE_MAX-total){nmedia_ff_free(range_url);JS_FreeCString(ctx,url);return JS_ThrowRangeError(ctx,"native media byte accounting overflow");}
        transition(s,url_input?"author-load-range":"author-load-buffer",generation);unload(s);s->generation=generation;
        if(url_input){
            s->range_url=range_url;s->range_waiting=true;
            s->range_keep=s->range_resume=range_requested;
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
        if(!mse_prepare_play(s))return JS_FALSE;
        if(s->url_input&&s->range_url&&(!s->worker||s->range_waiting)){
            s->range_waiting=s->range_resume=true;s->range_seek_sent=false;
        }
        if(s->url_input)s->range_keep=true;
        return JS_TRUE;
    }
    if(!strcmp(op,"play")){
        if((!s->decoder&&!s->worker&&!s->mse_active)||s->error[0]||!s->ready||s->mse_resume_needed||s->mse_preparing||nmedia_worker_seeking(s->worker)){transition(s,"author-play-not-ready",s->generation);return JS_FALSE;}
        if(s->playing)return JS_TRUE;
        s->snapshot.valid=false;
        if(s->ended&&!seek(s,0,now))return JS_FALSE;
        /* An ended worker restarts through a real asynchronous seek. Do not
         * open (and then overwrite) an audio FD before that seek's ACK. */
        if(!nmedia_worker_seeking(s->worker)&&!audio_open(s))return JS_FALSE;
        s->playing=true;s->started=now;s->ended=false;s->range_keep=false;transition(s,"author-play",s->generation);return JS_TRUE;
    }
    if(!strcmp(op,"pause")){
        transition(s,"author-pause",s->generation);
        s->snapshot.valid=false;
        clear_ahead(s); /* Release even when the subsequent seek cannot queue. */
        if(s->url_input){range_suspend(s,now);}
        else if(s->mse_active){mse_pause(s,now);}
        else if(s->playing){int64_t ms=current(s,now);s->playing=false;if(!seek(s,ms,now))s->base_ms=ms;close_audio(s);}
        return JS_UNDEFINED;
    }
    if(!strcmp(op,"seek")){
        double seconds;if(!argc||JS_ToFloat64(ctx,&seconds,argv[0])<0)return JS_EXCEPTION;
        if(!isfinite(seconds)||seconds<0||seconds>1e9)return JS_ThrowRangeError(ctx,"invalid media time");
        return JS_NewBool(ctx,seek(s,(int64_t)(seconds*1000),now));
    }
    if(!strcmp(op,"rate")){
        double rate;if(argc<2||JS_ToFloat64(ctx,&rate,argv[0])<0)return JS_EXCEPTION;
        int pitch=JS_ToBool(ctx,argv[1]);if(pitch<0)return JS_EXCEPTION;
        if(!isfinite(rate)||(rate!=0&&(rate<0.25||rate>4)))return JS_ThrowRangeError(ctx,"unsupported playback rate");
        if(s->rate==rate&&s->preserve_pitch==!!pitch)return JS_TRUE;
        /* Flush old-speed device samples and resume at the consumed media
           position. SourceBuffer ownership and buffered packets are retained. */
        int64_t position=current(s,now);
        if(s->ready&&!seek(s,position,now))return JS_FALSE;
        close_audio(s);s->base_ms=position;s->started=now;s->rate=rate;s->preserve_pitch=!!pitch;
        if(s->playing&&rate&&!nmedia_worker_seeking(s->worker)&&!s->mse_seeking&&!audio_open(s))return JS_FALSE;
        return JS_TRUE;
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
static void pump_impl(struct web_avmedia *s,uint64_t now,uint64_t mse_until,bool cooperative){
    if(uptime_ms()>=mse_until)return;
    if(s->audio_clock_failed){audio_failed(s,now);return;}
    /* An input-starved MSE child with no command/lane/output is asleep until
       APPEND/KEYS/seek/control revives demand. Re-reading its three pipes and
       the empty audio sink cannot produce data, and makes every unrelated
       author/layout checkpoint pay for the same exhausted packet cursor. */
    if(s->mse_active&&nmedia_mse_worker_running(s->mse_worker)&&
       !nmedia_mse_worker_needs_service(s->mse_worker)&&!s->pending.kind&&!s->ahead.kind&&
       !s->mse_preparing&&!s->mse_seeking)return;
    struct nmedia_worker_budget range_budget={.until=mse_until,.tx_bytes=65536,.nonblocking=cooperative};
    if(s->url_input&&s->error[0]){if(s->worker)range_failed(s,s->error);return;}
    if(!cooperative&&s->url_input&&s->range_waiting&&!s->worker){
        /* Revalidate after an idle element is resumed. An adopted/removed
         * document cannot reuse a stale origin grant or asynchronous handle. */
        if(!range_node(s->doc,s->node,s->range_url)){
            strlcpy(s->error,"native media Range resume policy denied",sizeof s->error);
            s->range_waiting=s->range_resume=false;s->ready=0;nmedia_ff_free(s->range_url);s->range_url=NULL;return;
        }
        s->worker=nmedia_worker_open(s->range_url,s->doc->url,s->generation,s->error,sizeof s->error);
        if(!s->worker){s->range_waiting=s->range_resume=false;s->ready=0;nmedia_ff_free(s->range_url);s->range_url=NULL;return;}
        s->input_size=NMEDIA_HTTP_CACHE_BYTES;
    }
    if(s->mse_worker){
        nmedia_mse_worker_time(s->mse_worker,current(s,now));
        if(cooperative)nmedia_mse_worker_service(s->mse_worker,now,mse_until);else nmedia_mse_worker_pump_budget(s->mse_worker,now,mse_until);
        const char *error=nmedia_mse_worker_error(s->mse_worker);if(*error){strlcpy(s->error,error,sizeof s->error);s->mse_quota_error=nmedia_mse_worker_quota_error(s->mse_worker);s->mse_preparing=false;if(!s->mse_quota_error){s->mse_resume_needed=false;s->base_ms=current(s,now);s->playing=false;close_audio(s);}s->doc->dirty=true;return;}
        if(s->mse_quota_error){s->mse_quota_error=false;s->error[0]=0;}
        if(s->mse_eos&&!s->mse_eos_duration&&!nmedia_mse_worker_pending(s->mse_worker)){
            int64_t highest=0;for(size_t i=0;i<s->mse_count;i++){size_t n;const struct nmedia_time_range *ranges=nmedia_mse_worker_ranges_view(s->mse_worker,(unsigned)i,&n);if(n)highest=MAX(highest,ranges[n-1].end_ms);}
            s->mse_duration=highest/1000.0;s->mse_eos_duration=true;
        }
        for(size_t i=0;i<s->mse_count;i++){
            const struct nmedia_info *info=nmedia_mse_worker_info(s->mse_worker,(unsigned)i);uint64_t revision=nmedia_mse_worker_revision(s->mse_worker,(unsigned)i);
            if(revision!=s->mse[i].revision_seen){
                if(s->ahead.kind&&s->ahead_slot==i&&s->ahead_revision!=revision)clear_ahead(s);
                if(s->pending.kind&&s->mse_pending_slot==i&&s->mse_pending_revision!=revision){s->pending.kind=0;s->audio_at=0;}
                if(s->mse[i].audio_owner||(info&&info->audio)){s->base_ms=current(s,now);s->started=now;close_audio(s);if(s->playing&&!s->mse_seeking&&!audio_open(s))s->playing=false;}
                s->mse[i].revision_seen=revision;
            }s->mse[i].audio_owner=info&&info->audio;
        }
        if(!s->ready){
            bool initialized=false,complete=true;
            for(size_t i=0;i<s->mse_count;i++)if(s->mse[i].used){
                initialized=true;
                if(!nmedia_mse_worker_info(s->mse_worker,(unsigned)i)){complete=false;break;}
            }
            if(initialized&&complete)s->ready=1;
        }
        /* The SEEK ACK only moves packet cursors. An unbuffered seek still
           needs appended bytes and real decoded output before seeked. */
        if(s->mse_seeking&&nmedia_mse_worker_seeking(s->mse_worker))return;
        if(mse_prepare_pump(s,mse_until,cooperative))return;
        /* A paused loaded element services append/remove/abort ACKs above,
           but must not consume decoder output behind the retained position. */
        if(s->mse_resume_needed&&!s->playing)return;
        if(!s->playing && s->ready>=2 && !s->mse_seeking)return;
    }
    /* Owned pictures already due on the real media clock do not depend on
       another decoder response. BUSY/AGAIN below can persist after the last
       input packet, so returning there first stranded the final lookahead
       images (including pictures older than the drained audio clock).
       Retain authored PTS and all revision/seek guards; never advance time
       merely to make a future image due. */
    if(s->playing&&!s->mse_seeking&&!s->range_resume&&!nmedia_worker_seeking(s->worker)&&
       !present_ahead(s,now)){
        s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;
    }
    /* Append/control transfer and metadata ACK above never stop. Only new
       decode demand waits; new audio ranges or seek/EOS release the wait. */
    if(s->mse_audio_wait){if(mse_audio_input_wait(s,now))return;s->mse_audio_wait=false;}
    if(s->mse_active&&!s->error[0]&&!s->pending.kind){
        int r=mse_next(s,&s->pending,mse_until,cooperative);s->audio_at=0;
        if(r==NMEDIA_VIDEO)s->video_frames++;
        if(r==NMEDIA_ERROR){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->ready=0;s->doc->dirty=true;return;}
        if(r==NMSW_BUSY)return;
        if(r==NMEDIA_AGAIN){
            uint32_t queued=0;if(!audio_queue(s,&queued,now))return;
            if(!queued&&!s->starved){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}return;
        }
        if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO||r==NMSW_CLOCK){
            s->mse_seeking=false;
            s->ready=2;if(s->starved){s->starved=false;s->started=now;if(s->playing&&s->fd<0&&!audio_open(s)){s->playing=false;return;}}
            if(r==NMEDIA_VIDEO&&!s->playing){if(!present(s))return;}
        }else if(r==NMEDIA_END)s->mse_seeking=false;
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
    if((!s->decoder&&!s->worker&&!s->mse_active)||!s->playing||!s->rate)return;
    const struct nmedia_info *rate_info=information(s);
    if(s->rate!=1&&s->audio_enabled&&rate_info&&rate_info->audio&&!s->rate_decoder){
        s->rate_decoder=nmedia_rate_create(s->rate,s->preserve_pitch);
        if(!s->rate_decoder){strlcpy(s->error,"native PCM tempo allocation/configuration failed",sizeof s->error);s->playing=false;return;}
    }
    for(int budget=0;budget<8;budget++){
        if(!present_ahead(s,now)){s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;}
        /* Release borrowed storage only after presentation, snapshot, drop or
         * complete PCM write. Dispatch before the cap return so the child can
         * decode while the browser performs its next JS/paint phase. */
        if(budget&&s->mse_active&&!s->pending.kind){if(cooperative)nmedia_mse_worker_prefetch_service(s->mse_worker,mse_until);else nmedia_mse_worker_prefetch(s->mse_worker,mse_until);}
        if(budget&&s->worker&&!s->pending.kind)nmedia_worker_prefetch(s->worker,&range_budget);
        if((s->mse_active||s->worker)&&budget&&uptime_ms()>=mse_until)return;
        if(!s->pending.kind&&s->rate_decoder){int r=nmedia_rate_pull(s->rate_decoder,&s->pending);s->rate_owned=r==NMEDIA_AUDIO;s->audio_at=0;if(r==NMEDIA_ERROR){strlcpy(s->error,"native PCM tempo output failed",sizeof s->error);s->playing=false;close_audio(s);return;}if(r!=NMEDIA_AUDIO)s->pending.kind=0;}
        if(!s->pending.kind){int r=s->mse_active?mse_next(s,&s->pending,mse_until,cooperative):s->worker?range_next(s,&s->pending,&range_budget):nmedia_step(s->decoder,&s->pending);s->audio_at=0;s->rate_owned=false;
            if(r==NMEDIA_VIDEO)s->video_frames++;
            if(r==NMSW_BUSY)return;
            if(r==NMEDIA_AGAIN){if(s->mse_active){uint32_t queued=0;if(!audio_queue(s,&queued,now))return;if(!queued){s->base_ms=current(s,now);s->starved=true;s->ready=1;s->doc->dirty=true;}}return;}
            if(r==NMEDIA_ERROR){if(s->worker){range_failed(s,nmedia_worker_error(s->worker));return;}if(!s->mse_active)strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO)s->ready=s->url_input||s->mse_active?2:4;
        }
        if(s->pending.kind==NMSW_CLOCK){if(s->pending.pts_ms>current(s,now)+5)return;s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_VIDEO){
            if(s->ahead.kind&&s->fd>=0){
                if(!hold_video(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;}
                s->pending.kind=0;
                if(mse_audio_input_wait(s,now)){s->mse_audio_wait=true;return;}
                continue;
            }
            if(s->pending.pts_ms>current(s,now)+5){
                /* A video-first packet must not block the following PCM:
                 * consumed audio can stop a few ms before its timestamp.
                 * Privately owned pictures keep their authored PTS, never
                 * eager-display/drop or fabricate clock progress. Every
                 * record is charged; the 2 ms / 8 packet burst bounds work. */
                if(s->fd>=0){
                    if(!hold_video(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);clear_ahead(s);s->doc->dirty=true;return;}
                    s->pending.kind=0;
                    if(mse_audio_input_wait(s,now)){s->mse_audio_wait=true;return;}
                    continue;
                }return;
            }
            /* No newer decoded image is available here. Never suppress the
               only real frame merely because its transport was late. */
            if(!present(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_AUDIO){
            if(!s->rate_owned){const struct nmedia_audio_observation *a=&s->pending.audio_observation;
                if(a->id&&a->frames&&a->work_ms){
                    if(s->performance_audio.id!=a->id||s->performance_audio.frames!=a->frames)s->performance_audio_at=uptime_ms();
                    s->performance_audio=*a;s->performance_audio_generation=s->generation;
                    s->performance_audio_slot=s->mse_pending_slot;s->performance_audio_revision=s->mse_pending_revision;
                }else memset(&s->performance_audio,0,sizeof s->performance_audio);
            }
            if(s->rate_decoder&&!s->rate_owned){
                if(!nmedia_rate_push(s->rate_decoder,&s->pending)){strlcpy(s->error,"native PCM tempo input failed",sizeof s->error);s->playing=false;close_audio(s);return;}
                s->pending.kind=0;continue;
            }
            if(s->fd<0&&s->pending.pts_ms>current(s,now)+100)return;
            if(s->fd>=0){uint32_t queued=0;if(!audio_queue(s,&queued,now)||queued>=WEB_AUDIO_TARGET)return;
                if(s->pending.frames>4096||s->audio_at>s->pending.frames){strlcpy(s->error,"invalid native audio span",sizeof s->error);s->playing=false;close_audio(s);return;}
                int gap=audio_fill_gap(s,queued);
                if(gap<0){s->base_ms=current(s,now);s->playing=false;close_audio(s);return;}
                if(gap==2)return;if(gap==1)continue;
                size_t frames=MIN(MIN(s->pending.frames-s->audio_at,(size_t)(WEB_AUDIO_TARGET-queued)),(size_t)4096);
                double gain=s->muted?0:s->volume;
                for(size_t i=0;i<frames*2;i++)s->scaled[i]=(int16_t)(s->pending.samples[s->audio_at*2+i]*gain);
                ssize_t n=write(s->fd,s->scaled,frames*4);
                s->clock_snapshot_valid=false;
                if(n<0&&errno==EAGAIN)return;
                if(n<=0||(n&3)||(size_t)n>frames*4){strlcpy(s->error,"native audio write failed",sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);return;}
                s->audio_at+=(size_t)n/4;s->audio_frames+=(size_t)n/4;s->audio_written=true;
                if(!audio_target_frame(s,&s->audio_end_frame)){s->base_ms=current(s,now);s->playing=false;close_audio(s);return;}
                s->audio_end_ms=s->audio_end_frame/SOUND_RATE*1000+(s->audio_end_frame%SOUND_RATE)*1000/SOUND_RATE;
                if(s->audio_at<s->pending.frames)return;
            }s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_END){
            if(s->rate_decoder){
                if(!nmedia_rate_end(s->rate_decoder)){strlcpy(s->error,"native PCM tempo drain failed",sizeof s->error);s->playing=false;close_audio(s);return;}
                int result=nmedia_rate_pull(s->rate_decoder,&s->pending);
                if(result==NMEDIA_AUDIO){s->rate_owned=true;s->audio_at=0;continue;}
                if(result==NMEDIA_ERROR){strlcpy(s->error,"native PCM tempo tail failed",sizeof s->error);s->playing=false;close_audio(s);return;}
                s->pending.kind=NMEDIA_END;
            }
            struct n_audio_status status={0};if(s->fd>=0){ssize_t bytes=read(s->fd,&status,sizeof status);if(bytes!=sizeof status&&bytes!=sizeof(uint32_t)){audio_failed(s,now);return;}}if(status.queued_frames||status.device_pending_frames)return;
            s->starved=false; /* Confirmed EOF is not a waiting-for-input gap. */
            if(s->ahead.kind){
                /* Actual decoder EOF and a drained PCM sink, not starvation:
                 * remaining authored pictures use the silent-video clock. */
                if(s->fd>=0){s->base_ms=current(s,now);s->started=now;close_audio(s);}return;
            }
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
    if(s->mse_active&&!s->pending.kind){if(cooperative)nmedia_mse_worker_prefetch_service(s->mse_worker,mse_until);else nmedia_mse_worker_prefetch(s->mse_worker,mse_until);}
    if(s->worker&&!s->pending.kind)nmedia_worker_prefetch(s->worker,&range_budget);
}
static void pump(struct web_avmedia *s,uint64_t now,uint64_t until,bool cooperative){
    s->clock_snapshot_enabled=true;s->clock_snapshot_valid=false;
    uint64_t start=debug_transitions?uptime_ms():0;
    if(debug_transitions){s->profile_pumps++;s->profile_services+=cooperative;if(s->profile_pump_at&&start>=s->profile_pump_at)s->profile_pump_gap=MAX(s->profile_pump_gap,start-s->profile_pump_at);s->profile_pump_at=start;}
    bool playing=s->playing,preparing=s->mse_preparing;pump_impl(s,now,until,cooperative);
    if(s->audio_clock_failed)audio_failed(s,now);
    s->clock_snapshot_enabled=false;s->clock_snapshot_valid=false;
    if(debug_transitions)s->profile_pump_ms+=uptime_ms()-start;
    if(playing&&!s->playing)clear_ahead(s);
    if(debug_transitions&&((playing&&!s->playing)||(preparing&&!s->mse_preparing&&s->error[0])))s->transition_pending=s->error[0]?"native-error-stop":s->ended?"native-eos-stop":"native-stop";
    /* Interrupt service never calls logging/JS/embedder callbacks. Preserve
       its stop reason until the ordinary document tick or teardown point. */
    if(!cooperative&&s->transition_pending)transition(s,s->transition_pending,s->generation);
}
/* This path uses only native stream pixels and an immutable resolved suffix.
 * The host performs its own current-window guard before touching UI memory. */
static void snapshot_publish(struct web_avmedia *s) {
    bool unpublished=s->snapshot.delivered!=s->presented_frames;
    if(s->snapshot.valid&&(!s->doc||!s->doc->live||!s->node||!s->node->owner||
       (s->node->owner!=s->doc&&s->node->owner->dom_family!=s->doc)||!doc_node_connected(s->node))){snapshot_clear(s);return;}
    if(!s->snapshot.valid || !s->doc || !s->doc->live || !s->playing ||
       s->mse_seeking || s->range_resume || nmedia_worker_seeking(s->worker) ||
       !s->pixels || s->generation!=s->snapshot.generation ||
       s->width!=s->snapshot.source_w || s->height!=s->snapshot.source_h ||
       !unpublished){
        if(unpublished&&s->doc&&s->doc->live)doc_paint_damage(s->doc,s->node);
        return;
    }
    canvas_t patch;
    gfx_init(&patch,s->snapshot.output,s->snapshot.w,s->snapshot.h,s->snapshot.w);
    gfx_fill(&patch,0,0,patch.w,patch.h,RGB(0,0,0));
    nmedia_draw(&patch,s->pixels,s->width,s->height,
                s->snapshot.video_x-s->snapshot.x,s->snapshot.video_y-s->snapshot.y,
                s->snapshot.video_w,s->snapshot.video_h);
    web_video_snapshot_overlay(s->snapshot.output,s->snapshot.committed,s->snapshot.opaque,s->snapshot.count);
    struct web_video_patch update={.canvas=s->snapshot.target,
        .scroll_x=s->snapshot.scroll_x,.scroll_y=s->snapshot.scroll_y,
        .x=s->snapshot.x,.y=s->snapshot.y,.w=s->snapshot.w,.h=s->snapshot.h,
        .pitch=s->snapshot.w,.pixels=s->snapshot.output};
    if(web_js_video_present(s->doc,&update)) {
        s->snapshot.delivered=s->presented_frames;s->window_frames++;
    } else {snapshot_clear(s);doc_paint_damage(s->doc,s->node);}
}
static void timing_profile(struct web_avmedia *s,uint64_t now){
    if((!s->playing&&(!s->ended||s->profile_ended))||(s->playing&&now-s->profile_at<2000))return;
    s->profile_at=now;s->profile_ended=s->ended;
    struct n_audio_status status={0};ssize_t status_bytes=s->fd>=0?read(s->fd,&status,sizeof status):-1;
    bool audio=status_bytes==sizeof status||status_bytes==sizeof(uint32_t);
    uint32_t queued=status.queued_frames+status.device_pending_frames;
    int64_t played=s->audio_written&&audio?s->audio_end_ms-(int64_t)(queued*s->rate*1000/SOUND_RATE):-1;
    char message[512];
    uint64_t decoded,dropped;video_counts(s,&decoded,&dropped);
    snprintf(message,sizeof message,"Native media timing: generation %u, input %s, clock %lld ms, presented %llu at %lld ms, decoded %llu/drop %llu, window-published %llu; audio %s, queued %u frames, written %llu/silence %llu frames, end %lld/played-estimate %lld ms, pending %d at %lld ms/ahead %zu at %lld ms, starved %u/ended %u",
        s->generation,s->mse_active?"MSE":s->url_input?"Range":"buffer",(long long)current(s,now),
        (unsigned long long)s->presented_frames,(long long)s->presented_ms,(unsigned long long)decoded,(unsigned long long)dropped,(unsigned long long)s->window_frames,
        audio?"open":"closed",queued,(unsigned long long)s->audio_frames,(unsigned long long)s->audio_silence_frames,(long long)s->audio_end_ms,(long long)played,
        s->pending.kind,(long long)s->pending.pts_ms,s->ahead_count,(long long)(s->ahead.kind?s->ahead.pts_ms:-1),s->starved,s->ended);
    /* queued includes only this stream's measured DMA contribution, not other
       streams' samples. Remote flags retain server-estimate semantics. */
    web_js_console(s->doc,0,message);
    if(debug_transitions){
        snprintf(message,sizeof message,"Native audio sink: generation %u, software %u/device %u frames, consumed %llu/committed %llu, clock-flags %u, rate %.3f/pitch %u",
            status.generation,status.queued_frames,status.device_pending_frames,(unsigned long long)status.played_frames,(unsigned long long)status.committed_frames,status.flags,s->rate,s->preserve_pitch);
        web_js_console(s->doc,0,message);
        struct nmedia_worker_stats stats;nmedia_worker_snapshot(s->worker,&stats);
        snprintf(message,sizeof message,"Native media supply: generation %u, pumps %llu/cooperative %llu/time %llu ms/max-gap %llu ms; Range IPC requests %llu/responses %llu/request-time %llu/max %llu ms, tx %llu/rx %llu bytes, handoffs %llu/header-waits %llu, receiving %zu/%zu bytes/header %zu/waiting %u; video %dx%d",
            s->generation,(unsigned long long)s->profile_pumps,(unsigned long long)s->profile_services,(unsigned long long)s->profile_pump_ms,(unsigned long long)s->profile_pump_gap,
            (unsigned long long)stats.requests,(unsigned long long)stats.responses,(unsigned long long)stats.request_ms,(unsigned long long)stats.max_request_ms,(unsigned long long)stats.tx_bytes,(unsigned long long)stats.rx_bytes,
            (unsigned long long)stats.handoffs,(unsigned long long)stats.header_waits,stats.payload_bytes,stats.payload_total,stats.header_bytes,stats.waiting,s->width,s->height);
        web_js_console(s->doc,0,message);s->profile_pumps=s->profile_services=s->profile_pump_ms=s->profile_pump_gap=0;
    }
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
        if(s->node->owner!=d&&(!s->node->owner||s->node->owner->dom_family!=d)){transition(s,"owner-detach",s->generation);unload(s);}
        else {pump(s,now,uptime_ms()+2,false);snapshot_publish(s);timing_profile(s,now);}
    }
    web_avmedia_leave();
}
void web_avmedia_service(uint64_t now){
    if(!streams||native_depth||service_running)return;
    /* Most safepoints arrive within the same millisecond. Reject those before
       even querying a stream FD or scanning its pending metadata. */
    if(service_clock_set&&now>=service_at&&now-service_at<1)return;
    uint64_t start=uptime_ms();
    /* A pending native lane/low PCM supply needs frequent bounded collection.
       A fixed 10ms throttle multiplied every STEP/metadata/video component
       round trip and could starve the ring despite readily decoded input. */
    uint64_t interval=5;
    for(struct web_avmedia *p=streams;p;p=p->next)if(p->playing&&p->rate&&(!p->mse_active||p->pending.kind||p->ahead.kind||nmedia_mse_worker_needs_service(p->mse_worker))&&(!p->mse_audio_wait||nmedia_mse_worker_pending(p->mse_worker))){
        uint32_t queued=0;
        if((p->fd>=0&&read(p->fd,&queued,4)==4&&queued<1440)||p->pending.kind||nmedia_mse_worker_pending(p->mse_worker)){
            interval=1;break;
        }
    }
    if(service_clock_set&&start>=service_at&&start-service_at<interval)return;
    service_at=start;service_clock_set=true;service_running=true;
    /* Shared across every context: one existing live MSE/Range child, not one new
     * 2ms allowance per iframe. Candidate scanning visits at most one real
     * registry cycle and shares that elapsed allowance, not a fixed 16-owner
     * quota that delays media behind unrelated idle elements.
     * Supply changes private media state; an already committed, safe video
     * layer may then publish an independent patch without DOM/JS/layout. */
    uint64_t until=start+2;
    struct web_avmedia *first=service_cursor&&service_cursor->next?service_cursor->next:streams,*s=first;
    while(s&&uptime_ms()<until){
        service_cursor=s;
        if((s->playing||s->mse_preparing)&&s->doc&&s->doc->live&&s->node&&
           (s->node->owner==s->doc||(s->node->owner&&s->node->owner->dom_family==s->doc))&&
           ((s->mse_active&&(s->pending.kind||s->ahead.kind||nmedia_mse_worker_needs_service(s->mse_worker))&&(!s->mse_audio_wait||nmedia_mse_worker_pending(s->mse_worker)))||
            (s->url_input&&nmedia_worker_running(s->worker)))&&
           (!s->mse_active||s->pending.kind||nmedia_mse_worker_service_due(s->mse_worker,now))){
            pump(s,now,until,true);snapshot_publish(s);break;
        }
        s=s->next?s->next:streams;if(s==first)break;
    }
    /* Count the supply interval from completion. A nonblocking wire copy or
       committed video patch may itself consume the interval; dating it from
       start then lets every following native-layout safepoint pump again and
       starves the caller despite each individual pump having a budget. */
    service_at=uptime_ms();
    service_running=false;
}
void web_avmedia_checkpoint(void){
    if(!streams||native_depth||service_running)return;
    web_avmedia_service(uptime_ms());
}
void web_avmedia_snapshot_begin(web_doc *d) {
    web_avmedia_enter();snapshot_document=d;snapshot_candidate=NULL;
    snapshot_phase=0;snapshot_rejected=false;
    nmedia_ff_free(snapshot_regions);snapshot_regions=NULL;snapshot_region_count=snapshot_region_capacity=0;
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d)s->snapshot.valid=false;
}
void web_avmedia_layout_begin(web_doc *d){
    /* Layout mutates boxes, not the previously committed screen. The raster
       suffix owns its coordinates, mask and pixels independently; publishing
       it never consults the new boxes. Keep it until actual paint begins.
       Generation/source/connected-node guards plus the host's canvas, viewport
       and scroll guards reject retired, removed or relocated screen owners. */
    (void)d;
}
void web_avmedia_snapshot_reject(void) {if(snapshot_document)snapshot_rejected=true;}
void web_avmedia_snapshot_reject_region(const canvas_t *c,int x,int y,int w,int h){
    if(!snapshot_document||snapshot_rejected||w<=0||h<=0)return;
    if(!c){snapshot_rejected=true;return;}
    struct snapshot_region r={MAX((int64_t)x,c->cx0),MAX((int64_t)y,c->cy0),
        MIN((int64_t)x+w,c->cx1),MIN((int64_t)y+h,c->cy1)};
    if(r.x0>=r.x1||r.y0>=r.y1)return;
    if(snapshot_phase){
        if(!snapshot_candidate){snapshot_rejected=true;return;}
        r.x0+=snapshot_candidate->snapshot.x;r.x1+=snapshot_candidate->snapshot.x;
        r.y0+=snapshot_candidate->snapshot.y;r.y1+=snapshot_candidate->snapshot.y;
    }
    if(snapshot_candidate){if(snapshot_region_intersects(&r,snapshot_candidate))snapshot_rejected=true;return;}
    /* A risky layer may precede the video in paint order. Retain only its
       clipped extent until the candidate is known, not a page-wide rejection
       or a second DOM tree. Capacity follows actual paint occurrences. */
    if(snapshot_region_count==snapshot_region_capacity){
        size_t n=snapshot_region_capacity?snapshot_region_capacity*2:8;
        if(n<snapshot_region_capacity||n>SIZE_MAX/sizeof *snapshot_regions){snapshot_rejected=true;return;}
        struct snapshot_region *v=nmedia_ff_realloc(snapshot_regions,n*sizeof *v);
        if(!v){snapshot_rejected=true;return;}snapshot_regions=v;snapshot_region_capacity=n;
    }
    snapshot_regions[snapshot_region_count++]=r;
}
bool web_avmedia_snapshot_prepare(web_doc *d,canvas_t *c,canvas_t *probe,
                                 int *x,int *y,int scroll_x,int scroll_y) {
    struct web_avmedia *s=snapshot_candidate;
    if(!d || d!=snapshot_document || snapshot_rejected || !s || !d->live || d->frame_parent ||
       !c || !c->px || !probe || !x || !y)return false;
    size_t count,bytes;
    if(!web_video_snapshot_extent(c->w,c->h,c->pitch,
                                 s->snapshot.x,s->snapshot.y,s->snapshot.w,s->snapshot.h,
                                 &count,&bytes))return false;
    /* Only this actual visible span is reserved. Allocation/heap failures keep
       ordinary paint as the fallback, with no new memory/decoder allowance. */
    if(count!=s->snapshot.count) {
        uint8_t *storage=nmedia_ff_realloc(s->snapshot.storage,bytes);
        if(!storage)return false;
        s->snapshot.storage=storage;s->snapshot.count=count;
    }
    s->snapshot.committed=(uint32_t*)s->snapshot.storage;
    s->snapshot.black=s->snapshot.committed+count;
    s->snapshot.output=s->snapshot.black+count;
    s->snapshot.opaque=(uint8_t*)(s->snapshot.output+count);
    for(int row=0;row<s->snapshot.h;row++)
        memcpy(s->snapshot.committed+(size_t)row*s->snapshot.w,
               c->px+(size_t)(s->snapshot.y+row)*c->pitch+s->snapshot.x,(size_t)s->snapshot.w*4);
    s->snapshot.target=*c;s->snapshot.scroll_x=scroll_x;s->snapshot.scroll_y=scroll_y;
    s->snapshot.generation=s->generation;
    s->snapshot.source_w=s->width;s->snapshot.source_h=s->height;
    gfx_init(probe,s->snapshot.output,s->snapshot.w,s->snapshot.h,s->snapshot.w);
    *x=s->snapshot.x;*y=s->snapshot.y;return true;
}
void web_avmedia_snapshot_probe(unsigned phase) {
    if(phase==2 && snapshot_candidate)
        memcpy(snapshot_candidate->snapshot.black,snapshot_candidate->snapshot.output,snapshot_candidate->snapshot.count*4);
    snapshot_phase=phase;
}
bool web_avmedia_snapshot_is_probe(void) {return snapshot_phase!=0;}
void web_avmedia_snapshot_finish(void) {
    struct web_avmedia *s=snapshot_candidate;
    if(s && snapshot_phase==2 && !snapshot_rejected &&
       web_video_snapshot_mask(s->snapshot.black,s->snapshot.output,s->snapshot.opaque,s->snapshot.count)) {
        s->snapshot.valid=true;s->snapshot.delivered=s->presented_frames;
    }
    /* Only one usable surface is retained per painted document. Hidden or
     * rejected videos must not accumulate native compositor reservations. */
    for(struct web_avmedia *p=streams;p;p=p->next)
        if(p->doc==snapshot_document && (p!=s || !p->snapshot.valid))snapshot_clear(p);
    snapshot_candidate=NULL;snapshot_document=NULL;snapshot_phase=0;web_avmedia_leave();
    nmedia_ff_free(snapshot_regions);snapshot_regions=NULL;snapshot_region_count=snapshot_region_capacity=0;
}
void web_avmedia_background(uint64_t now){nmedia_worker_background(now);nmedia_mse_worker_background(now);}
int64_t web_avmedia_background_deadline(uint64_t now){int64_t a=nmedia_worker_deadline(now),b=nmedia_mse_worker_deadline(now);return a<0?b:b<0?a:MIN(a,b);}
int64_t web_avmedia_deadline(web_doc *d,uint64_t now){
    int64_t next=-1;
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d&&(s->playing||s->range_waiting)){
        if(s->mse_active&&nmedia_mse_worker_running(s->mse_worker)&&
           !nmedia_mse_worker_needs_service(s->mse_worker)&&!s->pending.kind&&!s->ahead.kind&&
           !s->mse_preparing&&!s->mse_seeking)continue;
        int64_t due=-1;
        /* Presentation wakes at the next authored PTS, converted using the
           actual per-stream DMA clock/rate. A future video no longer acquires
           a fixed ten-ms polling interval; multiple streams take the minimum. */
        int64_t pts=-1;
        if(s->ahead.kind==NMEDIA_VIDEO)pts=s->ahead.pts_ms;
        if((s->pending.kind==NMEDIA_VIDEO||s->pending.kind==NMSW_CLOCK)&&
           (pts<0||s->pending.pts_ms<pts))pts=s->pending.pts_ms;
        if(pts>=0&&s->playing&&s->rate>0&&!s->starved){
            int64_t delta=pts-current(s,now);
            double wait=delta>0?ceil(delta/s->rate):0;
            int64_t ms=wait>INT32_MAX?INT32_MAX:(int64_t)wait;
            due=(int64_t)now+ms;
        }
        /* Audio refill and native transport have independent deadlines. Do
           not sleep until a video PTS while a PCM queue is draining. Reading
           one tiny native status here is not a made-up presentation clock. */
        if(s->playing&&s->fd>=0&&s->audio_enabled){
            struct n_audio_status status={0};ssize_t bytes=read(s->fd,&status,sizeof status);
            uint64_t queued=(uint64_t)status.queued_frames+status.device_pending_frames;
            int64_t refill=(int64_t)now+1;
            if((bytes==sizeof status||bytes==sizeof(uint32_t))&&queued>WEB_AUDIO_TARGET/2)
                refill=(int64_t)now+(int64_t)((queued-WEB_AUDIO_TARGET/2)*1000/SOUND_RATE);
            if(due<0||refill<due)due=refill;
        }
        if(due<0)due=(int64_t)now+1; /* input/decoder/preroll progress */
        if(next<0||due<next)next=due;
    }
    return next;
}
void web_avmedia_free(web_doc *d){
    web_avmedia_enter();
    struct web_avmedia **link=&streams;
    while(*link){struct web_avmedia *s=*link;if(s->doc!=d){link=&s->next;continue;}*link=s->next;if(service_cursor==s)service_cursor=NULL;transition(s,"document-free",s->generation);unload(s);volatile uint8_t *keys=s->eme_keys;for(size_t i=0;i<s->eme_key_bytes;i++)keys[i]=0;nmedia_ff_free(s->eme_keys);nmedia_ff_free(s);}
    web_avmedia_leave();
}
/* Cue text never enters the document HTML parser. Styled glyphs share the
 * native video surface (including PiP); all pixels obey its clip rectangle. */
static int caption_advance(uint32_t cp,int vertical){char text[5];int bytes=0;if(cp<0x80)text[bytes++]=(char)cp;else if(cp<0x800){text[bytes++]=(char)(0xc0|(cp>>6));text[bytes++]=(char)(0x80|(cp&63));}else if(cp<0x10000){text[bytes++]=(char)(0xe0|(cp>>12));text[bytes++]=(char)(0x80|((cp>>6)&63));text[bytes++]=(char)(0x80|(cp&63));}else{ text[bytes++]=(char)(0xf0|(cp>>18));text[bytes++]=(char)(0x80|((cp>>12)&63));text[bytes++]=(char)(0x80|((cp>>6)&63));text[bytes++]=(char)(0x80|(cp&63));}text[bytes]=0;return vertical&&cp>=0x100?gfx_font_h(FONT_SMALL):gfx_text_width(text,FONT_SMALL);}
static void caption_glyph(canvas_t *target,int x,int y,uint32_t cp,uint32_t color,uint32_t flags,bool vertical,bool half){
    int height=gfx_font_h(FONT_SMALL),width=caption_advance(cp,0)+2,shear=(flags&2)?height/4:0;
    if(!half&&!vertical&&!(flags&2)){gfx_char(target,x,y,cp,color,TRANSPARENT,FONT_SMALL);if(flags&1)gfx_char(target,x+1,y,cp,color,TRANSPARENT,FONT_SMALL);if(flags&4)gfx_line(target,x,y+height-1,x+width-2,y+height-1,color);return;}
    width+=shear;if(width<=0||height<=0||width>64||height>64)return;uint32_t pixels[64*64];memset(pixels,0,(size_t)width*height*sizeof *pixels);canvas_t glyph;gfx_init(&glyph,pixels,width,height,width);gfx_char(&glyph,0,0,cp,color,TRANSPARENT,FONT_SMALL);if(flags&1)gfx_char(&glyph,1,0,cp,color,TRANSPARENT,FONT_SMALL);
    if(flags&4)gfx_line(&glyph,0,height-1,width-shear-2,height-1,color);
    for(int row=0;row<height;row++)for(int col=0;col<width-shear;col++){uint32_t pixel=pixels[(size_t)row*width+col];if(!(pixel>>24))continue;int shifted=col+((flags&2)?(height-1-row)/4:0),dx=shifted,dy=row;if(vertical&&cp<0x100){dx=height-1-row;dy=shifted;}if(half){dx/=2;dy/=2;}gfx_fill(target,x+dx,y+dy,1,1,pixel);}
}
static int caption_measure(const struct caption_cue *cue,int limit,bool vertical,int *widest,int *widths){int rows=1,at=0;*widest=0;for(size_t i=0;i<cue->count;i++)for(const char *text=cue->spans[i].text;*text;){uint32_t cp;int bytes=gfx_utf8_decode(text,&cp);text+=bytes;if(cp=='\n'){*widest=MAX(*widest,at);if(widths)widths[rows-1]=at;at=0;rows++;continue;}int step=caption_advance(cp,vertical);if(at&&at+step>limit){*widest=MAX(*widest,at);if(widths)widths[rows-1]=at;at=0;rows++;}at+=step;}*widest=MAX(*widest,at);if(widths)widths[rows-1]=at;return rows;}
static int caption_coordinate(double value,int minimum,int maximum){if(value<minimum)return minimum;if(value>maximum)return maximum;return (int)lround(value);}
static void paint_rich_captions(struct web_avmedia *s,canvas_t *target,int x,int y,int w,int h){
    int ruby_height=0;for(size_t i=0;i<s->caption_count;i++)for(size_t j=0;j<s->caption_cues[i].count;j++)if(s->caption_cues[i].spans[j].ruby[0])ruby_height=gfx_font_h(FONT_SMALL)/2;
    int line_height=gfx_font_h(FONT_SMALL)+ruby_height+6,default_flow=0;canvas_t media=*target;gfx_clip(&media,x,y,w,h);
    memset(s->caption_flow,0,s->caption_count*sizeof *s->caption_flow);
    for(size_t i=0;i<s->caption_count;i++){const struct caption_cue *cue=&s->caption_cues[i];if(cue->count&&cue->region&&!cue->scroll&&!cue->vertical&&!isfinite(cue->line)&&cue->size==100){int widest,limit=MAX(1,(int)lround(w*cue->width/100)-16);s->caption_flow[cue->region_group]+=caption_measure(cue,limit,false,&widest,NULL)*line_height;}}
    for(size_t reverse=s->caption_count;reverse>0;reverse--){const struct caption_cue *cue=&s->caption_cues[reverse-1];if(!cue->count)continue;bool vertical=cue->vertical!=0,region=cue->region&&!vertical&&!isfinite(cue->line)&&cue->size==100;int region_w=w,region_h=h,region_x=x,region_y=y,flow=0;
        if(region){region_w=MAX(0,(int)lround(w*cue->width/100));region_h=MIN(h,(int)MIN((uint64_t)h,(uint64_t)cue->region_lines*line_height));region_x=caption_coordinate(x+w*cue->viewport_x/100-region_w*cue->region_x/100,x-region_w,x+w);region_y=caption_coordinate(y+h*cue->viewport_y/100-region_h*cue->region_y/100,y-region_h,y+h);
            flow=s->caption_flow[cue->region_group];
        }
        int row_widths[2049],limit=MAX(1,(int)lround((vertical?h:region_w)*cue->size/100)-16),widest=0,rows=caption_measure(cue,limit,vertical,&widest,row_widths),extent=rows*line_height;
        if(region){if(cue->scroll)s->caption_flow[cue->region_group]+=extent;else {s->caption_flow[cue->region_group]-=extent;flow=s->caption_flow[cue->region_group];}}
        double position=isfinite(cue->position)?cue->position:cue->align==0||cue->align==3?0:cue->align==2||cue->align==4?100:50;int alignment=cue->position_align==0?(cue->align==0||cue->align==3?1:cue->align==2||cue->align==4?3:2):cue->position_align;double anchor=alignment==1?0:alignment==3?1:.5;
        int left=x,top=y,box_w=vertical?extent:MIN(region_w,limit+16),box_h=vertical?MIN(h,limit+16):extent;
        if(!vertical){left=caption_coordinate(region_x+region_w*position/100-box_w*anchor,region_x,region_x+MAX(0,region_w-box_w));if(region)top=cue->scroll?region_y+region_h-extent-flow:region_y+flow;else if(!isfinite(cue->line)){top=y+h-extent-default_flow;default_flow+=extent;}else if(cue->snap)top=caption_coordinate(cue->line>=0?y+cue->line*line_height:y+h+cue->line*line_height-extent+line_height,y-extent,y+h);else top=caption_coordinate(y+h*cue->line/100-(cue->line_align==1?extent/2.0:cue->line_align==2?extent:0),y-extent,y+h);}
        else {top=caption_coordinate(y+h*position/100-box_h*anchor,y,y+MAX(0,h-box_h));if(!isfinite(cue->line))left=cue->vertical==1?x+w-extent-default_flow:x+default_flow;else if(cue->snap)left=caption_coordinate(cue->vertical==1?x+w-extent-cue->line*line_height:x+cue->line*line_height,x-extent,x+w);else left=caption_coordinate(x+w*cue->line/100-(cue->line_align==1?extent/2.0:cue->line_align==2?extent:0),x-extent,x+w);if(!isfinite(cue->line))default_flow+=extent;}
        canvas_t box=media;if(region)gfx_clip(&box,region_x,region_y,region_w,region_h);gfx_clip(&box,left,top,box_w,box_h);if(box.cx0>=box.cx1||box.cy0>=box.cy1)continue;gfx_fill(&box,left,top,box_w,box_h,RGB(20,20,24));
        int inline_at=0,row=0;for(size_t i=0;i<cue->count;i++){const struct caption_span *span=&cue->spans[i];if(span->ruby&&span->ruby[0]){int ruby_width=(gfx_text_width(span->ruby,FONT_SMALL)+1)/2,base_width=gfx_text_width(span->text,FONT_SMALL);int rx=vertical?left+extent-row*line_height: left+8+inline_at+(base_width-ruby_width)/2,ry=vertical?top+8+inline_at:top+row*line_height+1;canvas_t ruby=media;if(region)gfx_clip(&ruby,region_x,region_y,region_w,region_h);for(const char *rt=span->ruby;*rt;){uint32_t cp;rt+=gfx_utf8_decode(rt,&cp);caption_glyph(&ruby,rx,ry,cp,span->color,0,false,true);rx+=(caption_advance(cp,0)+1)/2;}}
            for(const char *text=span->text;*text;){uint32_t cp;text+=gfx_utf8_decode(text,&cp);if(cp=='\n'){inline_at=0;row++;continue;}int step=caption_advance(cp,vertical);if(inline_at&&inline_at+step>limit){inline_at=0;row++;}int tx=vertical?left+(cue->vertical==1?rows-row-1:row)*line_height+2:left+8+inline_at,ty=vertical?top+8+inline_at:top+row*line_height+ruby_height+3;
                if(!vertical&&cue->align!=0&&cue->align!=3){int adjust=MAX(0,limit-row_widths[row]);tx+=cue->align==2||cue->align==4?adjust:adjust/2;}
                caption_glyph(&box,tx,ty,cp,span->color,span->flags,vertical,false);inline_at+=step;
            }
        }
    }
}
/* Legacy plain-text callers continue to use the same native surface. */
static void paint_caption(struct web_avmedia *s,canvas_t *c,int x,int y,int w,int h){
    if(s&&s->caption_count&&w>16&&h>0){paint_rich_captions(s,c,x,y,w,h);return;}
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
bool web_avmedia_pip_ready(web_doc *d,node_t *n){struct web_avmedia *s=find(d,n,false);const struct nmedia_info *info=s?information(s):NULL;return media_node(n)&&n->tag==T_video&&s&&s->ready>=1&&info&&info->video&&info->width>0&&info->height>0&&!s->error[0];}
uint64_t web_avmedia_pip_revision(web_doc *d,node_t *n){struct web_avmedia *s=find(d,n,false);return s?((uint64_t)s->generation<<32)+s->presented_frames+(s->caption_revision<<16):0;}
bool web_avmedia_pip_paint(web_doc *d,node_t *n,canvas_t *c){
    struct web_avmedia *s=find(d,n,false);if(!s||!media_node(n)||n->tag!=T_video)return false;
    gfx_fill(c,0,0,c->w,c->h,RGB(0,0,0));if(s->pixels)nmedia_draw(c,s->pixels,s->width,s->height,0,0,c->w,c->h);paint_caption(s,c,0,0,c->w,c->h);return true;
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
    if(snapshot_document && !snapshot_phase && s && s->playing && s->pixels && n->tag==T_video) {
        if(d!=snapshot_document || snapshot_candidate)snapshot_rejected=true;
        else {
            snapshot_candidate=s;
            s->snapshot.x=(int)x0;s->snapshot.y=(int)y0;s->snapshot.w=(int)(x1-x0);s->snapshot.h=(int)(y1-y0);
            s->snapshot.video_x=x;s->snapshot.video_y=y;s->snapshot.video_w=w;s->snapshot.video_h=h-strip;
            for(size_t i=0;i<snapshot_region_count;i++)if(snapshot_region_intersects(&snapshot_regions[i],s)){snapshot_rejected=true;break;}
        }
    }
    if(s && s==snapshot_candidate && snapshot_phase)
        gfx_fill(c,x,y,w,h-strip,snapshot_phase==1?RGB(0,0,0):RGB(255,255,255));
    else if(s&&s->pixels&&n->tag==T_video)nmedia_draw(c,s->pixels,s->width,s->height,x,y,w,h-strip);
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
