/* Opaque/identity suffix classification from the black and white probes.
 * An intermediate pixel is accepted only when the paint declared per-pixel
 * linear compositing over the video (group opacity, a rounded clip edge):
 * then out = black + video * (white - black) / 255 per channel, within the
 * renderer's integer rounding. The per-channel span replaces that pixel's
 * unused committed value. Any other intermediate pixel rejects the surface. */
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
                                          uint32_t *committed,uint8_t *opaque,size_t count,bool blend) {
    for(size_t i=0;i<count;i++) {
        uint32_t a=black[i]&0xffffffu,b=white[i]&0xffffffu;
        if(a==b)opaque[i]=1;
        else if(a==0 && b==0xffffffu)opaque[i]=0;
        else if(blend && (b&0xff0000u)>=(a&0xff0000u) && (b&0xff00u)>=(a&0xff00u) && (b&0xffu)>=(a&0xffu)){
            committed[i]=b-a;opaque[i]=2; /* no channel borrows: each byte is its own span */
        }
        else return false;
    }
    return true;
}
static inline void web_video_snapshot_overlay(uint32_t *output,const uint32_t *committed,const uint32_t *black,
                                             const uint8_t *opaque,size_t count) {
    for(size_t i=0;i<count;i++) {
        if(opaque[i]==1)output[i]=committed[i];
        else if(opaque[i]==2) {
            uint32_t v=output[i],k=committed[i],a=black[i],out=0xff000000u;
            for(int shift=0;shift<24;shift+=8)
                out|=(((a>>shift)&255u)+(((v>>shift)&255u)*((k>>shift)&255u)+127u)/255u)<<shift;
            output[i]=out;
        }
    }
}
