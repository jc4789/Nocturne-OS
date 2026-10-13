/* New second-review boundaries only; no repetition of the geometry suite. */
#include "../user/libc/media_draw.c"
#include "media_mse_worker_private.h"
#include <stdio.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){if(failed<8)printf("媒体境界失敗 行 %d\n",__LINE__);failed++;}}while(0)
static uint32_t reference(const uint32_t *p,int sw,int sh,int64_t x,int64_t y,int64_t dw,int64_t dh){
    double sx=((x+0.5)*sw/dw)-0.5,sy=((y+0.5)*sh/dh)-0.5;
    if(sx<0)sx=0;if(sy<0)sy=0;if(sx>sw-1)sx=sw-1;if(sy>sh-1)sy=sh-1;
    int ix=(int)sx,iy=(int)sy,nx=ix+1<sw?ix+1:ix,ny=iy+1<sh?iy+1:iy;uint32_t out=0;
    for(unsigned shift=0;shift<32;shift+=8){
        double a=((p[iy*sw+ix]>>shift)&255)*(1-(sx-ix))+((p[iy*sw+nx]>>shift)&255)*(sx-ix);
        double b=((p[ny*sw+ix]>>shift)&255)*(1-(sx-ix))+((p[ny*sw+nx]>>shift)&255)*(sx-ix);
        out|=(uint32_t)(a*(1-(sy-iy))+b*(sy-iy))<<shift;
    }return out;
}
static void close_pixel(uint32_t got,uint32_t expected){
    for(unsigned s=0;s<32;s+=8){int delta=(int)((got>>s)&255)-(int)((expected>>s)&255);CHECK(delta>=-2&&delta<=2);}
}
int main(int argc,char **argv){
    struct nmsw_shared_video shared={.occupied={1,0},.id=72};
    struct nmsw_video header={.shared_id=72,.buffer_slot=0};
    CHECK(nmsw_shared_matches(&shared,&header));
    shared.id=73;CHECK(!nmsw_shared_matches(&shared,&header)); /* same-size FD reuse */
    header.shared_id=73;CHECK(nmsw_shared_matches(&shared,&header));
    header.buffer_slot=1;CHECK(!nmsw_shared_matches(&shared,&header)); /* unpublished/free */
    shared.occupied[1]=2;CHECK(!nmsw_shared_matches(&shared,&header));
    header.buffer_slot=NMSW_VIDEO_SLOTS;CHECK(!nmsw_shared_matches(&shared,&header));
    header.buffer_slot=0;header.shared_id=0;shared.id=0;CHECK(!nmsw_shared_matches(&shared,&header));
    size_t bytes=0;CHECK(nmsw_shared_size(64,&bytes)&&bytes==144);
    CHECK(!nmsw_shared_size(63,&bytes)&&!nmsw_shared_size(0,&bytes));
    if(argc>1&&!strcmp(argv[1],"--identity")){
        printf("共有識別子 %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
    }
    uint32_t source[49],out[19*17+2];
    for(unsigned i=0;i<49;i++)source[i]=(i*39u&255u)<<24|(i*23u&255u)<<16|(i*77u&255u)<<8|(i*127u&255u);
    canvas_t c={.px=out+1,.w=17,.h=17,.pitch=19,.cx0=0,.cy0=0,.cx1=17,.cy1=17};
    for(unsigned i=0;i<19*17+2;i++)out[i]=0x154873de;
    nmedia_draw(&c,source,7,7,0,0,17,17);
    for(int y=0;y<17;y++)for(int x=0;x<17;x++)close_pixel(c.px[y*19+x],reference(source,7,7,x,y,17,17));
    CHECK(out[0]==0x154873de&&out[19*17+1]==0x154873de);
    /* A tiny viewport clips the centre of an INT-sized destination. Previous
       16.16 stepping became zero; approximate origins could drift whole pixels. */
    const int extents[]={1700000000,INT_MAX};
    for(unsigned k=0;k<2;k++){
        int extent=extents[k],origin=-extent/2;
        nmedia_draw(&c,source,7,7,origin,origin,extent,extent);
        for(int y=0;y<17;y++)for(int x=0;x<17;x++)close_pixel(c.px[y*19+x],reference(source,7,7,(int64_t)x-origin,(int64_t)y-origin,extent,extent));
    }
    uint32_t saved=out[1];canvas_t invalid=c;invalid.w=INT_MAX;invalid.h=INT_MAX;invalid.pitch=INT_MAX;
    nmedia_draw(&invalid,source,1,1,0,0,1,1);CHECK(out[1]==saved);
    nmedia_draw(&c,source,7,7,INT_MIN,INT_MIN,INT_MAX,INT_MAX);CHECK(out[1]==saved);
    printf("媒体第二境界 %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
