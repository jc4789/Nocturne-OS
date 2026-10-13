/* Actual product scaler, not codec/GUI/site acceptance. */
#include "../user/include/media.h"
__attribute__((noinline)) void nmedia_draw(canvas_t *,const uint32_t *,int,int,int,int,int,int);
#include "../user/libc/media_draw.c"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){if(failed<8)printf("失敗 行 %d\n",__LINE__);failed++;}}while(0)
static uint32_t reference(const uint32_t *src,int sw,int sh,int xx,int yy,int dw,int dh){
    double x=((xx+0.5)*sw/dw)-0.5,y=((yy+0.5)*sh/dh)-0.5;
    if(x<0)x=0;if(y<0)y=0;if(x>sw-1)x=sw-1;if(y>sh-1)y=sh-1;
    int sx=(int)x,sy=(int)y,nx=sx+1<sw?sx+1:sx,ny=sy+1<sh?sy+1:sy;
    double fx=x-sx,fy=y-sy;uint32_t pixel=0;
    for(unsigned shift=0;shift<32;shift+=8){
        double a=((src[sy*sw+sx]>>shift)&255)*(1-fx)+((src[sy*sw+nx]>>shift)&255)*fx;
        double b=((src[ny*sw+sx]>>shift)&255)*(1-fx)+((src[ny*sw+nx]>>shift)&255)*fx;
        pixel|=(uint32_t)(a*(1-fy)+b*fy)<<shift;
    }return pixel;
}
/* Original product nearest path for a same-host, same-geometry cost baseline. */
__attribute__((noinline)) static void nearest(canvas_t *c,const uint32_t *src,int sw,int sh){
    for(int yy=0;yy<c->h;yy++)for(int xx=0;xx<c->w;xx++)
        c->px[(size_t)yy*c->pitch+xx]=src[(size_t)((int64_t)yy*sh/c->h)*sw+(size_t)((int64_t)xx*sw/c->w)];
}
int main(void){
    uint32_t source[64*48],storage[32*23+2];
    for(unsigned i=0;i<64*48;i++)source[i]=0xff000000u|((i*37u)&255u)<<16|((i*53u)&255u)<<8|((i*71u)&255u);
    const int widths[]={1,2,7,16,64},heights[]={1,2,5,13,48};
    for(unsigned k=0;k<5;k++)for(unsigned l=0;l<5;l++)for(int offset=-11;offset<12;offset+=11){
        for(unsigned i=0;i<32*23+2;i++)storage[i]=0x159753bdu;
        canvas_t c={.px=storage+1,.w=29,.h=23,.pitch=32,.cx0=3,.cy0=2,.cx1=26,.cy1=21};
        int sw=widths[k],sh=heights[l],w=31,h=25,x=offset,y=offset;
        nmedia_draw(&c,source,sw,sh,x,y,w,h);
        int dw=w,dh=sh*w/sw;if(dh>h){dh=h;dw=sw*h/sh;}
        int dx=x+(w-dw)/2,dy=y+(h-dh)/2;
        CHECK(storage[0]==0x159753bdu&&storage[32*23+1]==0x159753bdu);
        for(int yy=0;yy<23;yy++)for(int xx=0;xx<32;xx++){
            uint32_t got=c.px[yy*32+xx];
            if(!dw||!dh||xx<dx||xx>=dx+dw||yy<dy||yy>=dy+dh||xx<3||xx>=26||yy<2||yy>=21){CHECK(got==0x159753bdu);continue;}
            uint32_t expected=reference(source,sw,sh,xx-dx,yy-dy,dw,dh);
            for(unsigned shift=0;shift<32;shift+=8){int delta=(int)((got>>shift)&255)-(int)((expected>>shift)&255);CHECK(delta>=-2&&delta<=2);}
        }
    }
    canvas_t c={.px=storage+1,.w=16,.h=16,.pitch=32,.cx0=0,.cy0=0,.cx1=16,.cy1=16};
    nmedia_draw(&c,source,16,16,0,0,16,16);
    for(int yy=0;yy<16;yy++)for(int xx=0;xx<16;xx++)CHECK(c.px[yy*32+xx]==source[yy*16+xx]);
    uint32_t *frame=malloc(480u*270u*4),*screen=malloc(1920u*1080u*4);
    if(frame&&screen){
        for(unsigned i=0;i<480u*270u;i++)frame[i]=source[i%(64*48)];
        canvas_t full={.px=screen,.w=1920,.h=1080,.pitch=1920,.cx0=0,.cy0=0,.cx1=1920,.cy1=1080};
        volatile uint32_t consume=0;
        clock_t a=clock();for(unsigned i=0;i<12;i++){nearest(&full,frame,480,270);consume^=screen[i*173];}clock_t b=clock();
        for(unsigned i=0;i<12;i++){nmedia_draw(&full,frame,480,270,0,0,1920,1080);consume^=screen[i*173];}clock_t d=clock();
        printf("同一ホスト 12 フレーム 最近傍 %.3f ms / 双線形 %.3f ms（実OS性能ではない）\n",(double)(b-a)*1000/CLOCKS_PER_SEC,(double)(d-b)*1000/CLOCKS_PER_SEC);
    }
    free(frame);free(screen);printf("製品scaler %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
