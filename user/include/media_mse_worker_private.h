#pragma once
#include "media_mse_private.h"
/* A native child owns a dynamic SourceBuffer registry. Wire has no pointers,
 * size_t, file paths or page-chosen protocols. */
#define NMSW_MAGIC 0x4e4d5338u /* v8; VIDEO shared-page double buffer */
#define NMSW_BUSY 4
#define NMSW_CLOCK 5 /* Decoded disabled tracks: time only, never fake AV data. */
#define NMSW_VIDEO_PENDING 6 /* STEP ACK: one VIDEO packet on its own lane. */
enum { NMSW_ADD=1,NMSW_APPEND,NMSW_REMOVE,NMSW_ABORT,NMSW_DROP,NMSW_END,NMSW_SEEK,NMSW_CHANGE,NMSW_STEP,NMSW_SELECT,NMSW_KEYS };
struct nmsw_command {
    uint32_t magic,op,generation,sequence,slot,flags,bytes,reserved;
    int64_t offset,start,end,current;
    uint64_t epoch;
};
struct nmsw_info {
    uint32_t audio,video,channels,sample_rate;
    int32_t width,height;
    int64_t duration;
    char audio_codec[32],video_codec[32],container[32];
};
struct nmsw_response {
    uint32_t magic,op,generation,sequence,quota_error,payload_bytes,frames,decoded_frames;
    int32_t kind,width,height,slot;
    int64_t pts;
    uint32_t metadata_bytes,metadata_count;
    uint32_t dropped_frames;
    char error[160];
    struct nmedia_audio_observation audio_observation;
};
/* Each record is followed by count ranges. Records are ordered by slot. */
struct nmsw_metadata {
    uint32_t slot,count,parsing,key_waiting;
    uint64_t revision;int64_t group_end;
    struct nmsw_info info;
};
/* Dedicated VIDEO fd carries headers only. Raster bytes reside in a native
 * FD-backed double buffer, imported only by the direct parent. Release/acquire
 * occupied flags preserve decoder/receiver ownership across seek and resize.
 * Epoch/revision reject old images without reusing their borrowed storage. */
#define NMSW_VIDEO_SLOTS 2u
struct nmsw_shared_video {uint32_t occupied[NMSW_VIDEO_SLOTS];uint64_t id;};
static inline bool nmsw_shared_size(uint32_t capacity,size_t *bytes){
    if(!capacity || capacity%64)return false;
    size_t raster=(size_t)capacity*NMSW_VIDEO_SLOTS;
    if(raster/NMSW_VIDEO_SLOTS!=capacity || raster>SIZE_MAX-sizeof(struct nmsw_shared_video))return false;
    *bytes=sizeof(struct nmsw_shared_video)+raster;return true;
}
struct nmsw_video {
    uint32_t magic,generation,sequence,slot,bytes,reserved;
    uint64_t epoch,revision;
    int64_t pts;
    int32_t width,height;
    uint64_t video_measured_frames,video_work_ms;
    uint64_t video_measurement_id;
    struct nmedia_video_configuration video_configuration;
    int32_t shared_fd;
    uint32_t buffer_slot,shared_capacity,shared_reserved;
    uint64_t shared_id;
};
static inline bool nmsw_shared_matches(const struct nmsw_shared_video *s,const struct nmsw_video *v){
    return s&&v&&v->buffer_slot<NMSW_VIDEO_SLOTS&&v->shared_id&&
        __atomic_load_n(&s->occupied[v->buffer_slot],__ATOMIC_ACQUIRE)==1&&s->id==v->shared_id;
}
_Static_assert(sizeof(struct nmsw_command)==72,"MSE command wire v3");
_Static_assert(sizeof(struct nmsw_video)==136,"MSE VIDEO wire v8");
_Static_assert(sizeof(struct nmsw_info)==128,"MSE info wire");
_Static_assert(sizeof(struct nmsw_response)==272,"MSE response wire v7");
_Static_assert(sizeof(struct nmsw_metadata)==160,"MSE metadata wire v2");
typedef struct nmedia_mse_worker nmedia_mse_worker;
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t,char *,size_t);
bool nmedia_mse_worker_command(nmedia_mse_worker *,unsigned,unsigned,const void *,size_t,int64_t,int64_t,int64_t,bool);
void nmedia_mse_worker_pump(nmedia_mse_worker *,uint64_t);
/* Native-only browser burst: all nested handoffs share the caller's end time. */
void nmedia_mse_worker_pump_budget(nmedia_mse_worker *,uint64_t,uint64_t);
/* One nonblocking receive pass only: never spawn, send, or yield. */
void nmedia_mse_worker_collect(nmedia_mse_worker *);
/* Native hotloop service: never spawn or hand off to a blocked peer. */
void nmedia_mse_worker_service(nmedia_mse_worker *,uint64_t,uint64_t);
bool nmedia_mse_worker_needs_service(const nmedia_mse_worker *);
bool nmedia_mse_worker_service_due(const nmedia_mse_worker *,uint64_t);
void nmedia_mse_worker_video_stats(const nmedia_mse_worker *,uint64_t *decoded,uint64_t *dropped);
void nmedia_mse_worker_background(uint64_t);
int64_t nmedia_mse_worker_deadline(uint64_t);
bool nmedia_mse_worker_pending(const nmedia_mse_worker *);
bool nmedia_mse_worker_running(const nmedia_mse_worker *);
bool nmedia_mse_worker_quota_error(const nmedia_mse_worker *);
const char *nmedia_mse_worker_error(const nmedia_mse_worker *);
const struct nmedia_info *nmedia_mse_worker_info(const nmedia_mse_worker *,unsigned);
size_t nmedia_mse_worker_quota(const nmedia_mse_worker *,unsigned);
size_t nmedia_mse_worker_ranges(const nmedia_mse_worker *,unsigned,struct nmedia_time_range *,size_t);
const struct nmedia_time_range *nmedia_mse_worker_ranges_view(const nmedia_mse_worker *,unsigned,size_t *);
int nmedia_mse_worker_step(nmedia_mse_worker *,struct nmedia_output *);
/* Taken, current-generation/revision VIDEO only. Both independent and muxed
 * decoder VIDEO use the separate lane; its owner permits a queued successor.
 * The retained control-payload compatibility path requires no successor.
 * Copy its shared slot to the caller's exclusively owned display/lookahead
 * spare, then release the slot. Capacity is pixels, not bytes. Never wait/JS. */
bool nmedia_mse_worker_move_video(nmedia_mse_worker *,const struct nmedia_output *,
    uint32_t generation,unsigned slot,uint64_t revision,uint32_t **pixels,size_t *capacity);
/* Consume/copy the previously returned span before this call: its storage may
 * be reused. At most one STEP, sharing the current native burst end time. */
void nmedia_mse_worker_prefetch(nmedia_mse_worker *,uint64_t);
void nmedia_mse_worker_prefetch_service(nmedia_mse_worker *,uint64_t);
void nmedia_mse_worker_close(nmedia_mse_worker *);

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *);
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *,unsigned);
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *);
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *,unsigned);
bool nmedia_mse_worker_key_waiting(const nmedia_mse_worker *);
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *,unsigned);
void nmedia_mse_worker_time(nmedia_mse_worker *,int64_t);
