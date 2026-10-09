/* Browser-only Nocturne child transport. No pointers/native size_t on wire. */
#pragma once
#include "media.h"
#include <stdint.h>
#define NMEDIA_WORKER_MAGIC 0x4e4d5731u
#define NMEDIA_WORKER_URL 2048u
#define NMEDIA_WORKER_DEADLINE 30000u
#define NMEDIA_WORKER_FRAME_BYTES (NMEDIA_MAX_PIXELS * 4u)
enum { NMW_OPEN=1, NMW_STEP=2, NMW_SEEK=3 };
struct nmedia_worker_command {
    uint32_t magic, operation, generation, sequence;
    int64_t seek_ms;
    uint32_t url_bytes, document_bytes;
};
struct nmedia_worker_response {
    uint32_t magic, operation, generation, sequence;
    int32_t kind, width, height;
    uint32_t payload_bytes, frames, audio, video, channels, sample_rate, reserved;
    int64_t pts_ms, duration_ms;
    char audio_codec[32], video_codec[32], container[32], error[160];
};
_Static_assert(sizeof(struct nmedia_worker_command)==32,"native media command layout");
_Static_assert(sizeof(struct nmedia_worker_response)==328,"native media response layout");
typedef struct nmedia_worker nmedia_worker;
nmedia_worker *nmedia_worker_open(const char *url,const char *native_document,
                                  uint32_t generation,char *error,size_t size);
/* A queued HTML element must not allocate another child/reservation. */
bool nmedia_worker_available(void);
void nmedia_worker_background(uint64_t now_ms);
int64_t nmedia_worker_deadline(uint64_t now_ms);
void nmedia_worker_pump(nmedia_worker *,uint64_t now_ms);
/* One caller-owned burst budget, shared by every STEP/collect in that burst.
 * Cooperative callers use allow_spawn=false: no process/URL open is started. */
struct nmedia_worker_budget {
    uint64_t until;
    size_t tx_bytes,rx_bytes;
    unsigned handoffs;
};
void nmedia_worker_pump_budget(nmedia_worker *,uint64_t now_ms,
                              struct nmedia_worker_budget *,bool allow_spawn);
/* One nonblocking reap of an already-dispatched response; no send/yield/spawn. */
void nmedia_worker_collect(nmedia_worker *,struct nmedia_worker_budget *);
bool nmedia_worker_running(const nmedia_worker *);
bool nmedia_worker_pending(const nmedia_worker *);
/* Only after consuming/copying the last borrowed AUDIO/VIDEO span. */
void nmedia_worker_prefetch(nmedia_worker *,struct nmedia_worker_budget *);
const struct nmedia_info *nmedia_worker_info(nmedia_worker *);
const char *nmedia_worker_error(nmedia_worker *);
bool nmedia_worker_loading(nmedia_worker *);
bool nmedia_worker_seeking(nmedia_worker *);
bool nmedia_worker_seek(nmedia_worker *,int64_t ms);
/* Output is borrowed until the next requested step/seek or close. */
int nmedia_worker_take(nmedia_worker *,struct nmedia_output *);
void nmedia_worker_step(nmedia_worker *);
void nmedia_worker_close(nmedia_worker *);
