#pragma once
#include "media_mse_private.h"
/* A native child owns a dynamic SourceBuffer registry. Wire has no pointers,
 * size_t, file paths or page-chosen protocols. */
#define NMSW_MAGIC 0x4e4d5333u /* v3; split VIDEO lane, reject older children */
#define NMSW_BUSY 4
#define NMSW_CLOCK 5 /* Decoded disabled tracks: time only, never fake AV data. */
#define NMSW_VIDEO_PENDING 6 /* STEP ACK: one VIDEO packet on its own lane. */
enum { NMSW_ADD=1,NMSW_APPEND,NMSW_REMOVE,NMSW_ABORT,NMSW_DROP,NMSW_END,NMSW_SEEK,NMSW_CHANGE,NMSW_STEP,NMSW_SELECT };
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
    uint32_t magic,op,generation,sequence,quota_error,payload_bytes,frames,reserved;
    int32_t kind,width,height,slot;
    int64_t pts;
    uint32_t metadata_bytes,metadata_count;
    char error[160];
};
/* Each record is followed by count ranges. Records are ordered by slot. */
struct nmsw_metadata {
    uint32_t slot,count,parsing,reserved;
    uint64_t revision;int64_t group_end;
    struct nmsw_info info;
};
/* Dedicated VIDEO fd: complete header then exactly bytes ARGB. No text,
 * control metadata or PCM ever appears on this lane. Epoch/revision reject
 * old slot/seek images without abandoning a partially received packet. */
struct nmsw_video {
    uint32_t magic,generation,sequence,slot,bytes,reserved;
    uint64_t epoch,revision;
    int64_t pts;
    int32_t width,height;
};
_Static_assert(sizeof(struct nmsw_command)==72,"MSE command wire v3");
_Static_assert(sizeof(struct nmsw_video)==56,"MSE VIDEO wire v3");
_Static_assert(sizeof(struct nmsw_info)==128,"MSE info wire");
_Static_assert(sizeof(struct nmsw_response)==224,"MSE response wire v2");
_Static_assert(sizeof(struct nmsw_metadata)==160,"MSE metadata wire v2");
typedef struct nmedia_mse_worker nmedia_mse_worker;
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t,char *,size_t);
bool nmedia_mse_worker_command(nmedia_mse_worker *,unsigned,unsigned,const void *,size_t,int64_t,int64_t,int64_t,bool);
void nmedia_mse_worker_pump(nmedia_mse_worker *,uint64_t);
/* Native-only browser burst: all nested handoffs share the caller's end time. */
void nmedia_mse_worker_pump_budget(nmedia_mse_worker *,uint64_t,uint64_t);
/* One nonblocking receive pass only: never spawn, send, or yield. */
void nmedia_mse_worker_collect(nmedia_mse_worker *);
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
/* Taken, current-generation/revision VIDEO only. The legacy muxed control
 * payload requires no queued successor; the split lane is a separate owner.
 * Swap its completed owner for the caller's exclusively owned spare; capacity
 * is pixels, not bytes. No copy, wait, allocation or JS. */
bool nmedia_mse_worker_move_video(nmedia_mse_worker *,const struct nmedia_output *,
    uint32_t generation,unsigned slot,uint64_t revision,uint32_t **pixels,size_t *capacity);
/* Consume/copy the previously returned span before this call: its storage may
 * be reused. At most one STEP, sharing the current native burst end time. */
void nmedia_mse_worker_prefetch(nmedia_mse_worker *,uint64_t);
void nmedia_mse_worker_close(nmedia_mse_worker *);

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *);
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *,unsigned);
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *);
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *,unsigned);
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *,unsigned);
void nmedia_mse_worker_time(nmedia_mse_worker *,int64_t);
