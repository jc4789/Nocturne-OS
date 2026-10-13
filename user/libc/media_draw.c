#include "media.h"
#include "media_video_private.h"
#include <limits.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

static int64_t maximum(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t minimum(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t centre_position(int64_t relative,int source,int64_t destination){
    /* Divide the integer and fractional parts separately: the numerator fits
       uint64_t even for INT_MAX rectangles, whereas a pre-shifted product may
       not. Computing the clipped origin exactly avoids accumulated step error
       when a tiny canvas intersects the middle of a gigantic destination. */
    uint64_t numerator=((uint64_t)relative*2+1)*(unsigned)source;
    uint64_t denominator=(uint64_t)destination*2;
    uint64_t whole=numerator/denominator,remainder=numerator%denominator;
    return (int64_t)((whole<<32)+((remainder<<32)/denominator))-(1LL<<31);
}
/* Two independent 8-bit channel lanes in a 32-bit product. Weights sum to
 * 256, so neither lane carries into its neighbour and no division is needed. */
static uint32_t blend(uint32_t a,uint32_t b,unsigned weight){
    unsigned inverse=256-weight;
    uint32_t rb=(((a&0x00ff00ffu)*inverse+(b&0x00ff00ffu)*weight)>>8)&0x00ff00ffu;
    uint32_t ag=((((a>>8)&0x00ff00ffu)*inverse+((b>>8)&0x00ff00ffu)*weight)>>8)&0x00ff00ffu;
    return rb|(ag<<8);
}
void nmedia_draw(canvas_t *c, const uint32_t *pixels, int sw, int sh, int x, int y, int w, int h) {
    if (!c || !c->px || !pixels || !nmedia_video_size(sw,sh,NULL,NULL) ||
        w <= 0 || h <= 0 || c->w <= 0 || c->h <= 0 || c->pitch < c->w ||
        (size_t)c->pitch>(size_t)PTRDIFF_MAX/sizeof(uint32_t)/(size_t)c->h) return;
    int64_t dw = w, dh = (int64_t)sh * w / sw;
    if (dh > h) dh = h, dw = (int64_t)sw * h / sh;
    if (!dw || !dh) return;
    int64_t dx = (int64_t)x + ((int64_t)w - dw) / 2, dy = (int64_t)y + ((int64_t)h - dh) / 2;
    int64_t x0 = maximum(maximum(dx, c->cx0), 0), y0 = maximum(maximum(dy, c->cy0), 0);
    int64_t x1 = minimum(minimum(dx + dw, c->cx1), c->w), y1 = minimum(minimum(dy + dh, c->cy1), c->h);
    if(x0>=x1||y0>=y1)return;
    if(dw==sw&&dh==sh){
        if(c->mask_clip){for(int64_t yy=y0;yy<y1;yy++) {
            struct gfx_clip_span span;if(!gfx_clip_row(c,(int)yy,(int)x0,(int)x1,&span))continue;
            const uint32_t *row=pixels+(size_t)(yy-dy)*sw;
            for(int xx=span.x0;xx<span.opaque0;xx++)gfx_pixel(c,xx,(int)yy,row[(size_t)(xx-dx)]);
            memcpy(c->px+(size_t)yy*c->pitch+span.opaque0,row+(size_t)(span.opaque0-dx),(size_t)(span.opaque1-span.opaque0)*sizeof *row);
            for(int xx=span.opaque1;xx<span.x1;xx++)gfx_pixel(c,xx,(int)yy,row[(size_t)(xx-dx)]);
        }return;}
        for(int64_t yy=y0;yy<y1;yy++)memcpy(c->px+(size_t)yy*c->pitch+(size_t)x0,
            pixels+(size_t)(yy-dy)*sw+(size_t)(x0-dx),(size_t)(x1-x0)*sizeof(uint32_t));
        return;
    }
    /* Pixel-centre bilinear sampling with constant-cost initial transforms,
     * not per-pixel quotients. Clipping retains the original transform and
     * edge samples clamp instead of wrapping. */
    int64_t step_x=((int64_t)sw<<32)/dw,step_y=((int64_t)sh<<32)/dh;
    int64_t origin_x=centre_position(x0-dx,sw,dw);
    int64_t position_y=centre_position(y0-dy,sh,dh);
    int64_t max_x=(int64_t)(sw-1)<<32,max_y=(int64_t)(sh-1)<<32;
    for (int64_t yy = y0; yy < y1; yy++) {
        int64_t py=maximum(0,minimum(position_y,max_y));if(yy+1<y1)position_y+=step_y;
        size_t sy=(size_t)(py>>32),next_y=sy+1<(size_t)sh?sy+1:sy;
        unsigned fy=(unsigned)(py>>24)&255u;
        const uint32_t *top=pixels+sy*sw,*bottom=pixels+next_y*sw;
        struct gfx_clip_span span={(int)x0,(int)x1,(int)x0,(int)x1};
        if(c->mask_clip&&!gfx_clip_row(c,(int)yy,(int)x0,(int)x1,&span))continue;
        int64_t position_x=origin_x+((int64_t)span.x0-x0)*step_x;
        int64_t xx=span.x0,end=span.x1;
#if defined(__SSE2__)
        __m128i zero=_mm_setzero_si128(),scale=_mm_set1_epi16(256);
        __m128i vy=_mm_set1_epi16((short)fy),iy=_mm_set1_epi16((short)(256-fy));
        while(xx<end){
          if(xx>=span.opaque0&&xx+1<span.opaque1){
            int64_t p0=maximum(0,minimum(position_x,max_x));position_x+=step_x;
            int64_t p1=maximum(0,minimum(position_x,max_x));if(xx+2<end)position_x+=step_x;
            size_t a=(size_t)(p0>>32),b=(size_t)(p1>>32),an=a+1<(size_t)sw?a+1:a,bn=b+1<(size_t)sw?b+1:b;
            short f0=(short)((p0>>24)&255),f1=(short)((p1>>24)&255);
            __m128i wx=_mm_set_epi16(f1,f1,f1,f1,f0,f0,f0,f0),ix=_mm_sub_epi16(scale,wx);
            __m128i tl=_mm_unpacklo_epi8(_mm_set_epi32(0,0,(int)top[b],(int)top[a]),zero);
            __m128i tr=_mm_unpacklo_epi8(_mm_set_epi32(0,0,(int)top[bn],(int)top[an]),zero);
            __m128i bl=_mm_unpacklo_epi8(_mm_set_epi32(0,0,(int)bottom[b],(int)bottom[a]),zero);
            __m128i br=_mm_unpacklo_epi8(_mm_set_epi32(0,0,(int)bottom[bn],(int)bottom[an]),zero);
            __m128i t=_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(tl,ix),_mm_mullo_epi16(tr,wx)),8);
            __m128i bline=_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(bl,ix),_mm_mullo_epi16(br,wx)),8);
            __m128i out=_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(t,iy),_mm_mullo_epi16(bline,vy)),8);
            _mm_storel_epi64((__m128i *)(c->px+(size_t)yy*c->pitch+(size_t)xx),_mm_packus_epi16(out,zero));
            xx+=2;continue;
          }
#else
        while(xx<end){
#endif
            int64_t px=maximum(0,minimum(position_x,max_x));if(xx+1<end)position_x+=step_x;
            size_t sx=(size_t)(px>>32),next_x=sx+1<(size_t)sw?sx+1:sx;
            unsigned fx=(unsigned)(px>>24)&255u;
            uint32_t color=blend(blend(top[sx],top[next_x],fx),blend(bottom[sx],bottom[next_x],fx),fy);
            if(c->mask_clip&&(xx<span.opaque0||xx>=span.opaque1))gfx_pixel(c,(int)xx,(int)yy,color);
            else c->px[(size_t)yy * c->pitch + (size_t)xx]=color;
            xx++;
        }
    }
}
