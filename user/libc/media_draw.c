#include "media.h"
#include "media_video_private.h"
#include <limits.h>
#include <string.h>
#include "display_abi.h"
#include <parallel.h>
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
/* A large picture is drawn in bands of rows on the parallel pool, a video
 * frame every few milliseconds. Each band is the same draw clipped to its
 * rows, and the sampling keeps the whole picture's transform, so the result
 * does not depend on the split. */
enum { DRAW_BAND_ROWS = 32 };
struct draw_bands {
    const canvas_t *c; uint8_t *cd; const uint32_t *pixels; const uint8_t *deep;
    int sw, sh, x, y, w, h, top;
};
static void draw_band(size_t i, void *context) {
    const struct draw_bands *j = context;
    canvas_t band = *j->c;
    int top = j->top + (int)i * DRAW_BAND_ROWS;
    if (band.cy0 < top) band.cy0 = top;
    if (band.cy1 > top + DRAW_BAND_ROWS) band.cy1 = top + DRAW_BAND_ROWS;
    if (j->deep) nmedia_draw_deep(&band, j->cd, j->pixels, j->deep, j->sw, j->sh, j->x, j->y, j->w, j->h);
    else nmedia_draw(&band, j->pixels, j->sw, j->sh, j->x, j->y, j->w, j->h);
}
static bool draw_in_bands(const canvas_t *c, uint8_t *cd, const uint32_t *pixels, const uint8_t *deep,
                          int sw, int sh, int x, int y, int w, int h, int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    if ((x1 - x0) * (y1 - y0) < 65536 || parallel_active()) return false;
    struct draw_bands j = {c, cd, pixels, deep, sw, sh, x, y, w, h, (int)y0};
    parallel_for_stage(PARALLEL_IMAGE_SCALE, (size_t)((y1 - y0 + DRAW_BAND_ROWS - 1) / DRAW_BAND_ROWS), draw_band, &j, NULL, NULL);
    return true;
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
    if(draw_in_bands(c,NULL,pixels,NULL,sw,sh,x,y,w,h,x0,y0,x1,y1))return;
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

/* 10-bit channel of a deep pixel (display_abi.h). */
static unsigned deep_channel(uint32_t pixel,uint8_t deep,unsigned shift,unsigned deep_shift){
    unsigned v=(pixel>>shift)&255u;
    return deep&DEEP_VALID?v<<2|((deep>>deep_shift)&3u):deep_widen(v);
}
/* 10 -> 8 bits with the 4x4 ordered dither: +1 with probability frac/4. */
static unsigned dither10(unsigned v10,unsigned threshold){
    unsigned v=(v10*4u+threshold)>>4;
    return v>255u?255u:v;
}
/* One 10-bit pixel: into both planes of a deep canvas, otherwise dithered to
 * 8 bits where it lands, so scaling never re-quantises the dither into bands. */
static void put_deep(canvas_t *c,uint8_t *cd,int64_t xx,int64_t yy,unsigned r,unsigned g,unsigned b){
    size_t to=(size_t)yy*c->pitch+(size_t)xx;
    if(cd){c->px[to]=deep_pixel(r,g,b);cd[to]=deep_pack(r,g,b);return;}
    unsigned t=dither_threshold((unsigned)xx,(unsigned)yy);
    c->px[to]=0xff000000u|dither10(r,t)<<16|dither10(g,t)<<8|dither10(b,t);
}
#if defined(__SSE2__)
/* A deep pixel as four 10-bit lanes B, G, R, A. */
static __m128i deep_lanes(uint32_t pixel,uint8_t deep){
    __m128i v=_mm_slli_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)pixel),_mm_setzero_si128()),2);
    if(!(deep&DEEP_VALID))return _mm_or_si128(v,_mm_srli_epi16(v,8));
    uint64_t low=(uint64_t)(deep&3u)|(uint64_t)(deep>>2&3u)<<16|(uint64_t)(deep>>4&3u)<<32;
    return _mm_or_si128(v,_mm_cvtsi64_si128((long long)low));
}
#endif
/* The same aspect fit, clip and pixel-centre bilinear sampling as nmedia_draw,
 * interpolating 10-bit channels. A deep canvas (cd) receives 10-bit pixels; an
 * 8-bit one receives them ordered-dithered after scaling. Without a frame deep
 * plane, or with a mask clip, it is exactly nmedia_draw. */
void nmedia_draw_deep(canvas_t *c,uint8_t *cd,const uint32_t *pixels,const uint8_t *deep,
                      int sw,int sh,int x,int y,int w,int h){
    if(!deep||!c||c->mask_clip){nmedia_draw(c,pixels,sw,sh,x,y,w,h);return;}
    if (!c->px || !pixels || !nmedia_video_size(sw,sh,NULL,NULL) ||
        w <= 0 || h <= 0 || c->w <= 0 || c->h <= 0 || c->pitch < c->w ||
        (size_t)c->pitch>(size_t)PTRDIFF_MAX/sizeof(uint32_t)/(size_t)c->h) return;
    int64_t dw = w, dh = (int64_t)sh * w / sw;
    if (dh > h) dh = h, dw = (int64_t)sw * h / sh;
    if (!dw || !dh) return;
    int64_t dx = (int64_t)x + ((int64_t)w - dw) / 2, dy = (int64_t)y + ((int64_t)h - dh) / 2;
    int64_t x0 = maximum(maximum(dx, c->cx0), 0), y0 = maximum(maximum(dy, c->cy0), 0);
    int64_t x1 = minimum(minimum(dx + dw, c->cx1), c->w), y1 = minimum(minimum(dy + dh, c->cy1), c->h);
    if(x0>=x1||y0>=y1)return;
    if(draw_in_bands(c,cd,pixels,deep,sw,sh,x,y,w,h,x0,y0,x1,y1))return;
    static const unsigned shifts[3][2]={{16,4},{8,2},{0,0}};
    if(dw==sw&&dh==sh){
        for(int64_t yy=y0;yy<y1;yy++){
            size_t from=(size_t)(yy-dy)*sw+(size_t)(x0-dx),to=(size_t)yy*c->pitch+(size_t)x0;
            if(cd){
                memcpy(c->px+to,pixels+from,(size_t)(x1-x0)*sizeof(uint32_t));
                memcpy(cd+to,deep+from,(size_t)(x1-x0));
                continue;
            }
            for(int64_t xx=x0;xx<x1;xx++,from++){
                unsigned v[3];
                for(int k=0;k<3;k++)v[k]=deep_channel(pixels[from],deep[from],shifts[k][0],shifts[k][1]);
                put_deep(c,NULL,xx,yy,v[0],v[1],v[2]);
            }
        }
        return;
    }
    int64_t step_x=((int64_t)sw<<32)/dw,step_y=((int64_t)sh<<32)/dh;
    int64_t origin_x=centre_position(x0-dx,sw,dw);
    int64_t position_y=centre_position(y0-dy,sh,dh);
    int64_t max_x=(int64_t)(sw-1)<<32,max_y=(int64_t)(sh-1)<<32;
    for(int64_t yy=y0;yy<y1;yy++){
        int64_t py=maximum(0,minimum(position_y,max_y));if(yy+1<y1)position_y+=step_y;
        size_t sy=(size_t)(py>>32),next_y=sy+1<(size_t)sh?sy+1:sy;
        unsigned fy=(unsigned)(py>>24)&255u;
        int64_t position_x=origin_x;
        int64_t xx=x0;
#if defined(__SSE2__)
        /* Two pixels per step in 16-bit lanes: 10-bit samples times 6-bit
           weights summing to 64 stay below 65536, so no lane overflows. */
        const uint32_t *top=pixels+sy*sw,*bottom=pixels+next_y*sw;
        const uint8_t *top_deep=deep+sy*sw,*bottom_deep=deep+next_y*sw;
        __m128i scale=_mm_set1_epi16(64),vy=_mm_set1_epi16((short)(fy>>2)),iy=_mm_sub_epi16(scale,vy);
        __m128i opaque=_mm_set1_epi32((int)0xff000000u);
        for(;xx+1<x1;xx+=2){
            int64_t p0=maximum(0,minimum(position_x,max_x));position_x+=step_x;
            int64_t p1=maximum(0,minimum(position_x,max_x));if(xx+2<x1)position_x+=step_x;
            size_t a=(size_t)(p0>>32),b=(size_t)(p1>>32),an=a+1<(size_t)sw?a+1:a,bn=b+1<(size_t)sw?b+1:b;
            short f0=(short)((p0>>26)&63),f1=(short)((p1>>26)&63);
            __m128i wx=_mm_set_epi16(f1,f1,f1,f1,f0,f0,f0,f0),ix=_mm_sub_epi16(scale,wx);
            __m128i tl=_mm_unpacklo_epi64(deep_lanes(top[a],top_deep[a]),deep_lanes(top[b],top_deep[b]));
            __m128i tr=_mm_unpacklo_epi64(deep_lanes(top[an],top_deep[an]),deep_lanes(top[bn],top_deep[bn]));
            __m128i bl=_mm_unpacklo_epi64(deep_lanes(bottom[a],bottom_deep[a]),deep_lanes(bottom[b],bottom_deep[b]));
            __m128i br=_mm_unpacklo_epi64(deep_lanes(bottom[an],bottom_deep[an]),deep_lanes(bottom[bn],bottom_deep[bn]));
            __m128i round=_mm_set1_epi16(32);
            __m128i t=_mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(tl,ix),_mm_mullo_epi16(tr,wx)),round),6);
            __m128i bline=_mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(bl,ix),_mm_mullo_epi16(br,wx)),round),6);
            __m128i out=_mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(t,iy),_mm_mullo_epi16(bline,vy)),round),6);
            size_t to=(size_t)yy*c->pitch+(size_t)xx;
            if(cd){
                uint16_t v[8];_mm_storeu_si128((__m128i *)v,out);
                c->px[to]=deep_pixel(v[2],v[1],v[0]);cd[to]=deep_pack(v[2],v[1],v[0]);
                c->px[to+1]=deep_pixel(v[6],v[5],v[4]);cd[to+1]=deep_pack(v[6],v[5],v[4]);
                continue;
            }
            short t0=(short)dither_threshold((unsigned)xx,(unsigned)yy),t1=(short)dither_threshold((unsigned)xx+1,(unsigned)yy);
            __m128i d=_mm_srli_epi16(_mm_add_epi16(_mm_slli_epi16(out,2),_mm_set_epi16(t1,t1,t1,t1,t0,t0,t0,t0)),4);
            _mm_storel_epi64((__m128i *)(c->px+to),_mm_or_si128(_mm_packus_epi16(d,d),opaque));
        }
#endif
        for(;xx<x1;xx++){
            int64_t px=maximum(0,minimum(position_x,max_x));if(xx+1<x1)position_x+=step_x;
            size_t sx=(size_t)(px>>32),next_x=sx+1<(size_t)sw?sx+1:sx;
            unsigned fx=(unsigned)(px>>24)&255u;
            size_t at[4]={sy*sw+sx,sy*sw+next_x,next_y*sw+sx,next_y*sw+next_x};
            unsigned v[3];
            for(int k=0;k<3;k++){
                unsigned s=shifts[k][0],d=shifts[k][1];
                unsigned upper=deep_channel(pixels[at[0]],deep[at[0]],s,d)*(256-fx)+deep_channel(pixels[at[1]],deep[at[1]],s,d)*fx;
                unsigned lower=deep_channel(pixels[at[2]],deep[at[2]],s,d)*(256-fx)+deep_channel(pixels[at[3]],deep[at[3]],s,d)*fx;
                v[k]=(upper*(256-fy)+lower*fy+32768)>>16;
            }
            put_deep(c,cd,xx,yy,v[0],v[1],v[2]);
        }
    }
}
