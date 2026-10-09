#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct nmedia;
struct nmedia_output;
/* Just-returned native VIDEO loan only; exchange exclusively owned ARGB
 * allocations before another step/seek/reclaim. No copy or allocation. */
bool nmedia_move_video(struct nmedia *,const struct nmedia_output *,uint32_t **,size_t *);

/* ARGB storage follows actual dimensions, not a configured pixel quota.
 * Pointer offsets and allocation bytes must remain representable. Metadata
 * may have zero (not-yet-known) dimensions; an output span may not. */
static inline bool nmedia_video_dimensions(int width,int height){
    return width>=0&&height>=0&&(!width||!height||
        (size_t)width<=(size_t)PTRDIFF_MAX/sizeof(uint32_t)/(size_t)height);
}
static inline bool nmedia_video_size(int width,int height,size_t *pixels,size_t *bytes){
    if(width<=0||height<=0||!nmedia_video_dimensions(width,height))return false;
    size_t count=(size_t)width*(size_t)height;
    if(pixels)*pixels=count;
    if(bytes)*bytes=count*sizeof(uint32_t);
    return true;
}
/* Both current native worker protocols encode payload length in uint32_t.
 * This is a wire representation limit, not a resolution/heap quota. */
static inline bool nmedia_video_wire_bytes(int width,int height,uint32_t *bytes){
    size_t count;
    if(!nmedia_video_size(width,height,NULL,&count)||count>UINT32_MAX)return false;
    if(bytes)*bytes=(uint32_t)count;
    return true;
}
