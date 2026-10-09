/* Exact opaque/identity suffix classification. Intermediate alpha is rejected:
 * no affine approximation of the renderer's integer blend rounding. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* The surface is a real clipped canvas span, not a codec-resolution quota.
 * Three uint32_t planes plus one byte mask remain charged to the media heap.
 * No fixed pixel maximum: representability and the actual allocator decide. */
static inline bool web_video_snapshot_extent(int canvas_w,int canvas_h,int pitch,
                                             int x,int y,int w,int h,
                                             size_t *count,size_t *bytes) {
    if(!count || !bytes || canvas_w<=0 || canvas_h<=0 || pitch<canvas_w ||
       x<0 || y<0 || x>canvas_w || y>canvas_h || w<=0 || h<=0 ||
       w>canvas_w-x || h>canvas_h-y)return false;
    if((size_t)pitch>SIZE_MAX/sizeof(uint32_t)/(size_t)canvas_h)return false;
    if((size_t)w>SIZE_MAX/(size_t)h)return false;
    size_t pixels=(size_t)w*(size_t)h;
    if(pixels>SIZE_MAX/13u)return false;
    *count=pixels;*bytes=pixels*13u;return true;
}
static inline bool web_video_snapshot_mask(const uint32_t *black,const uint32_t *white,
                                          uint8_t *opaque,size_t count) {
    for(size_t i=0;i<count;i++) {
        uint32_t a=black[i]&0xffffffu,b=white[i]&0xffffffu;
        if(a==b)opaque[i]=1;
        else if(a==0 && b==0xffffffu)opaque[i]=0;
        else return false;
    }
    return true;
}
static inline void web_video_snapshot_overlay(uint32_t *output,const uint32_t *committed,
                                             const uint8_t *opaque,size_t count) {
    for(size_t i=0;i<count;i++)if(opaque[i])output[i]=committed[i];
}
