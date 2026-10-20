/* The real CPU clip/raster implementation, with compiler trap instrumentation.
   This does not claim to exercise kernel or GPU device IO. */
#include "../user/libc/gpu.c"
#include <stdio.h>

long __syscall(long n,long a,long b,long c,long d,long e) {return -N_ERR_UNSUPPORTED;}
void gfx_init(canvas_t *c,uint32_t *p,int w,int h,int pitch) {
    *c=(canvas_t){.px=p,.w=w,.h=h,.pitch=pitch,.cx1=w,.cy1=h};
}
void gfx_fill(canvas_t *c,int x,int y,int w,int h,uint32_t color) {
    for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)c->px[(size_t)yy*c->pitch+xx]=color;
}
void gfx_blit(canvas_t *d,int dx,int dy,const canvas_t *s,int sx,int sy,int w,int h) {
    for(int y=0;y<h;y++)for(int x=0;x<w;x++)d->px[(size_t)(dy+y)*d->pitch+dx+x]=s->px[(size_t)(sy+y)*s->pitch+sx+x];
}
static uint32_t state=0x9e3779b9;
static uint32_t random_word(void) { /* xorshift32 deterministic test corpus */
    state^=state<<13;state^=state>>17;state^=state<<5;return state;
}
int main(void) {
    enum {SIDE=16,CASES=100000};
    uint32_t pixels[SIDE*SIDE+2]={0};
    pixels[0]=0x12345678;pixels[SIDE*SIDE+1]=0x87654321;
    canvas_t canvas;gfx_init(&canvas,pixels+1,SIDE,SIDE,SIDE);
    const struct n_gpu_vertex boundary[3]={{INT32_MIN,INT32_MAX,0,1,0xffffffff},
        {INT32_MAX,INT32_MIN,0,INT32_MAX,0xffffffff},{0,0,INT32_MIN,-1,0xffffffff}};
    draw_triangle(&canvas,boundary,SIDE,SIDE);
    for(unsigned i=0;i<CASES;i++){
        struct n_gpu_vertex v[3];
        for(unsigned j=0;j<3;j++){
            v[j]=(struct n_gpu_vertex){(int32_t)random_word(),(int32_t)random_word(),
                (int32_t)random_word(),(int32_t)random_word(),random_word()};
            if(!v[j].w)v[j].w=1;
        }
        draw_triangle(&canvas,v,SIDE,SIDE);
        if(pixels[0]!=0x12345678 || pixels[SIDE*SIDE+1]!=0x87654321)return 1;
    }
    puts("gpu_cpu_hosttest: clip/raster boundary and 100000 deterministic cases passed");
    return 0;
}
