/* Exact opaque/identity suffix classification. Intermediate alpha is rejected:
 * no affine approximation of the renderer's integer blend rounding. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
