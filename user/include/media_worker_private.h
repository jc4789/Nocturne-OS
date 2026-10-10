/* Browser-only Nocturne child transport. No pointers/native size_t on wire. */
#pragma once
#include "media.h"
#include <stdint.h>
#define NMEDIA_WORKER_MAGIC 0x4e4d5732u /* v2: independent VIDEO lane */
#define NMW_VIDEO_PENDING 4 /* STEP ACK; actual pixels follow on VIDEO fd */
enum { NMW_OPEN=1, NMW_STEP=2, NMW_SEEK=3 };
struct nmedia_worker_command {
    uint32_t magic, operation, generation, sequence;
    int64_t seek_ms;
    uint32_t url_bytes, document_bytes; /* exact OPEN spans, no NUL on wire */
    uint64_t epoch; /* author seek invalidates old VIDEO without abandoning wire */
};
struct nmedia_worker_response {
    uint32_t magic, operation, generation, sequence;
    int32_t kind, width, height;
    uint32_t payload_bytes, frames, audio, video, channels, sample_rate, reserved;
    int64_t pts_ms, duration_ms;
    char audio_codec[32], video_codec[32], container[32], error[160];
};
struct nmedia_worker_video {
    uint32_t magic,generation,sequence,bytes;
    uint64_t epoch;int64_t pts;int32_t width,height;
};
_Static_assert(sizeof(struct nmedia_worker_video)==40,"Range VIDEO wire v2");
_Static_assert(sizeof(struct nmedia_worker_command)==40,"native media command v2 layout");
_Static_assert(sizeof(struct nmedia_worker_response)==328,"native media response layout");
typedef struct nmedia_worker nmedia_worker;
nmedia_worker *nmedia_worker_open(const char *url,const char *native_document,
                                  uint32_t generation,char *error,size_t size);
/* Every element owns its worker. Retired cancellation/reap never blocks a
 * different element's OPEN; allocation/spawn failure is reported honestly. */
void nmedia_worker_background(uint64_t now_ms);
int64_t nmedia_worker_deadline(uint64_t now_ms);
void nmedia_worker_pump(nmedia_worker *,uint64_t now_ms);
/* Anonymous cumulative transport counters and the current receive span. */
struct nmedia_worker_stats {
    uint64_t requests,responses,request_ms,max_request_ms,tx_bytes,rx_bytes,handoffs,header_waits;
    size_t header_bytes,payload_bytes,payload_total;
    bool waiting;
};
void nmedia_worker_snapshot(const nmedia_worker *,struct nmedia_worker_stats *);
/* One caller-owned burst budget, shared by every STEP/collect in that burst.
 * Cooperative callers use allow_spawn=false: no process/URL open is started. */
struct nmedia_worker_budget {
    uint64_t until;
    size_t tx_bytes;
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
/* Only a complete independent-lane VIDEO just returned by take.
 * PCM/control successors do not invalidate this pixel loan; seek does.
 * Move its allocation to the caller and return the caller's distinct owned
 * spare to the transport. capacity is uint32_t pixels, not bytes. No copying
 * or allocation. A false return leaves both owners unchanged. Old wire
 * response/counters describe the consumed response, never the spare's data.
 * The caller must not retain readers of its spare (paint/snapshot included). */
bool nmedia_worker_move_video(nmedia_worker *,const struct nmedia_output *,
                             uint32_t **pixels,size_t *capacity);
void nmedia_worker_step(nmedia_worker *);
void nmedia_worker_close(nmedia_worker *);
