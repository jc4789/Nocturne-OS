#pragma once
#include "media_mse_private.h"
/* One native child owns both SourceBuffer track buffers. Wire has no pointers,
 * size_t, file paths or page-chosen protocols. */
#define NMSW_MAGIC 0x4e4d5331u
#define NMSW_BUSY 4
#define NMSW_CLOCK 5 /* Decoded disabled tracks: time only, never fake AV data. */
enum { NMSW_ADD=1,NMSW_APPEND,NMSW_REMOVE,NMSW_ABORT,NMSW_DROP,NMSW_END,NMSW_SEEK,NMSW_CHANGE,NMSW_STEP,NMSW_SELECT };
struct nmsw_command {
    uint32_t magic,op,generation,sequence,slot,flags,bytes,reserved;
    int64_t offset,start,end,current;
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
    uint32_t quota[2],counts[2];
    uint64_t revision[2];uint32_t parsing[2];int64_t group_end[2];
    struct nmsw_info info[2];
    struct nmedia_time_range ranges[2][64];
    char error[160];
};
_Static_assert(sizeof(struct nmsw_command)==64,"MSE command wire");
_Static_assert(sizeof(struct nmsw_info)==128,"MSE info wire");
_Static_assert(sizeof(struct nmsw_response)==2576,"MSE response wire");
typedef struct nmedia_mse_worker nmedia_mse_worker;
nmedia_mse_worker *nmedia_mse_worker_open(uint32_t,char *,size_t);
bool nmedia_mse_worker_command(nmedia_mse_worker *,unsigned,unsigned,const void *,size_t,int64_t,int64_t,int64_t,bool);
void nmedia_mse_worker_pump(nmedia_mse_worker *,uint64_t);
void nmedia_mse_worker_background(uint64_t);
int64_t nmedia_mse_worker_deadline(uint64_t);
bool nmedia_mse_worker_pending(const nmedia_mse_worker *);
bool nmedia_mse_worker_quota_error(const nmedia_mse_worker *);
const char *nmedia_mse_worker_error(const nmedia_mse_worker *);
const struct nmedia_info *nmedia_mse_worker_info(const nmedia_mse_worker *,unsigned);
size_t nmedia_mse_worker_quota(const nmedia_mse_worker *,unsigned);
size_t nmedia_mse_worker_ranges(const nmedia_mse_worker *,unsigned,struct nmedia_time_range *,size_t);
int nmedia_mse_worker_step(nmedia_mse_worker *,struct nmedia_output *);
void nmedia_mse_worker_close(nmedia_mse_worker *);

bool nmedia_mse_worker_seeking(const nmedia_mse_worker *);
uint64_t nmedia_mse_worker_revision(const nmedia_mse_worker *,unsigned);
unsigned nmedia_mse_worker_output_slot(const nmedia_mse_worker *);
bool nmedia_mse_worker_parsing(const nmedia_mse_worker *,unsigned);
int64_t nmedia_mse_worker_group_end(const nmedia_mse_worker *,unsigned);
void nmedia_mse_worker_time(nmedia_mse_worker *,int64_t);
