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
void nmedia_worker_background(uint64_t now_ms);
int64_t nmedia_worker_deadline(uint64_t now_ms);
void nmedia_worker_pump(nmedia_worker *,uint64_t now_ms);
const struct nmedia_info *nmedia_worker_info(nmedia_worker *);
const char *nmedia_worker_error(nmedia_worker *);
bool nmedia_worker_loading(nmedia_worker *);
bool nmedia_worker_seeking(nmedia_worker *);
bool nmedia_worker_seek(nmedia_worker *,int64_t ms);
/* Output is borrowed until the next requested step/seek or close. */
int nmedia_worker_take(nmedia_worker *,struct nmedia_output *);
void nmedia_worker_step(nmedia_worker *);
void nmedia_worker_close(nmedia_worker *);
