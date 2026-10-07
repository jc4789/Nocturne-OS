/* Bounded native 3D rendering, with an explicit and honest RAM fallback. */
#include "gpu.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

static int result(long r) { if (r<0) { errno=(int)-r; return -1; } return (int)r; }
int gpu_info(struct n_gpu_info *out) {
    return result(__syscall(SYS_GPU_INFO,(long)out,0,0,0,0));
}
int gpu_render(const struct n_gpu_render *r, uint32_t *pixels, size_t bytes) {
    return result(__syscall(SYS_GPU_RENDER,(long)r,(long)pixels,(long)bytes,0,0));
}
struct vertex { double p[4], c[4]; };
static double plane(const struct vertex *v, unsigned p) {
    return v->p[3]+((p&1)?-v->p[p/2]:v->p[p/2]);
}
static struct vertex interpolate(const struct vertex *a,const struct vertex *b,double t) {
    struct vertex v;
    for (unsigned i=0;i<4;i++) { v.p[i]=a->p[i]+(b->p[i]-a->p[i])*t; v.c[i]=a->c[i]+(b->c[i]-a->c[i])*t; }
    return v;
}
static double edge(double ax,double ay,double bx,double by,double x,double y) {
    return (x-ax)*(by-ay)-(y-ay)*(bx-ax);
}
static unsigned channel(double v) { if (v<0) return 0; if (v>255) return 255; return (unsigned)(v+0.5); }
static void raster(canvas_t *dst,const struct vertex *a,const struct vertex *b,const struct vertex *c,
                   unsigned width,unsigned height) {
    const struct vertex *v[3]={a,b,c}; double x[3],y[3],iw[3];
    for (unsigned i=0;i<3;i++) {
        iw[i]=1/v[i]->p[3];
        x[i]=(v[i]->p[0]*iw[i]+1)*width/2;
        /* Resource Y_0_TOP matches the virgl positive viewport scale. */
        y[i]=(1-v[i]->p[1]*iw[i])*height/2;
    }
    double area=edge(x[0],y[0],x[1],y[1],x[2],y[2]);
    if (fabs(area)<1e-12) return;
    int x0=(int)floor(fmin(x[0],fmin(x[1],x[2]))), x1=(int)ceil(fmax(x[0],fmax(x[1],x[2])));
    int y0=(int)floor(fmin(y[0],fmin(y[1],y[2]))), y1=(int)ceil(fmax(y[0],fmax(y[1],y[2])));
    if (x0<dst->cx0) x0=dst->cx0; if (y0<dst->cy0) y0=dst->cy0;
    if (x1>dst->cx1) x1=dst->cx1; if (y1>dst->cy1) y1=dst->cy1;
    if (x1>(int)width) x1=(int)width; if (y1>(int)height) y1=(int)height;
    for (int yy=y0;yy<y1;yy++) for (int xx=x0;xx<x1;xx++) {
        double w[3]={edge(x[1],y[1],x[2],y[2],xx+0.5,yy+0.5)/area,
            edge(x[2],y[2],x[0],y[0],xx+0.5,yy+0.5)/area,0};
        w[2]=1-w[0]-w[1];
        if (w[0]<0 || w[1]<0 || w[2]<0) continue;
        double divisor=w[0]*iw[0]+w[1]*iw[1]+w[2]*iw[2]; unsigned col[4];
        for (unsigned j=0;j<4;j++) col[j]=channel((w[0]*iw[0]*a->c[j]+w[1]*iw[1]*b->c[j]+w[2]*iw[2]*c->c[j])/divisor);
        dst->px[(size_t)yy*dst->pitch+xx]=(col[3]<<24)|(col[0]<<16)|(col[1]<<8)|col[2];
    }
}
static bool valid(const struct n_gpu_render *r) {
    if (!r || !r->width || !r->height || r->width>N_GPU_MAX_SIDE || r->height>N_GPU_MAX_SIDE ||
        (r->operation!=N_GPU_CLEAR && r->operation!=N_GPU_TRIANGLE)) return false;
    if (r->operation==N_GPU_TRIANGLE) for (unsigned i=0;i<3;i++) {
        const struct n_gpu_vertex *v=&r->vertex[i];
        if (v->w<256 || v->w>16*65536 || v->x<-16*65536 || v->x>16*65536 ||
            v->y<-16*65536 || v->y>16*65536 || v->z<-16*65536 || v->z>16*65536) return false;
    }
    return true;
}
int gfx_render3d(canvas_t *dst,const struct n_gpu_render *r) {
    if (!dst || !dst->px || !valid(r) || dst->w<(int)r->width || dst->h<(int)r->height || dst->pitch<dst->w) {
        errno=EINVAL; return -1;
    }
    /* Other gfx calls trust the canvas. This public bounded entry also clips
       malformed/outside clip coordinates back to its advertised dimensions. */
    canvas_t safe=*dst;
    safe.cx0=MAX(0,MIN(safe.cx0,safe.w)); safe.cx1=MAX(safe.cx0,MIN(safe.cx1,safe.w));
    safe.cy0=MAX(0,MIN(safe.cy0,safe.h)); safe.cy1=MAX(safe.cy0,MIN(safe.cy1,safe.h));
    dst=&safe;
    size_t bytes=(size_t)r->width*r->height*4;
    uint32_t *pixels=malloc(bytes);
    if (pixels && gpu_render(r,pixels,bytes)==0) {
        canvas_t src; gfx_init(&src,pixels,r->width,r->height,r->width);
        gfx_blit(dst,0,0,&src,0,0,r->width,r->height); free(pixels); return N_GPU_BACKEND_VIRGL;
    }
    free(pixels);
    gfx_fill(dst,0,0,r->width,r->height,r->clear_argb);
    if (r->operation==N_GPU_CLEAR) return N_GPU_BACKEND_CPU;
    struct vertex v[16], next[16]; unsigned n=3;
    for (unsigned i=0;i<3;i++) {
        const struct n_gpu_vertex *a=&r->vertex[i];
        v[i].p[0]=a->x/65536.0; v[i].p[1]=a->y/65536.0; v[i].p[2]=a->z/65536.0; v[i].p[3]=a->w/65536.0;
        v[i].c[0]=(a->argb>>16)&255; v[i].c[1]=(a->argb>>8)&255; v[i].c[2]=a->argb&255; v[i].c[3]=a->argb>>24;
    }
    /* Homogeneous six-plane clipping before the perspective division. */
    for (unsigned p=0;p<6 && n;p++) {
        unsigned count=0;
        for (unsigned i=0;i<n;i++) {
            const struct vertex *a=&v[i],*b=&v[(i+1)%n]; double da=plane(a,p),db=plane(b,p);
            if (da>=0 && count<16) next[count++]=*a;
            if ((da<0)!=(db<0) && count<16) next[count++]=interpolate(a,b,da/(da-db));
        }
        n=count; memcpy(v,next,n*sizeof *v);
    }
    for (unsigned i=1;i+1<n;i++) raster(dst,&v[0],&v[i],&v[i+1],r->width,r->height);
    return N_GPU_BACKEND_CPU;
}
