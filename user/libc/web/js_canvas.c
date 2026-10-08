/* Actual Canvas2D RAM bitmap. No dummy WebGL or falsely successful methods.
 * Per-document bitmap memory and per-path work are bounded. Attribute changes
 * release/reset the bitmap, detached nodes remain allocation-document owned. */
#include "js_canvas.h"
#include "gpu.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CANVAS_MAX_PIXELS (1u<<20)
#define CANVAS_BUDGET (16u<<20)
#define MAX_POINTS 256
struct web_canvas {
    unsigned w,h,generation;
    uint32_t *pixels;
    size_t bytes;
    bool tainted;
};
unsigned web_canvas_dimension(const node_t *n,const char *name) {
    unsigned fallback=!strcmp(name,"width")?300:150;
    const char *s=node_attr(n,name);
    if (!s) return fallback;
    while (*s==' ' || *s=='\t' || *s=='\r' || *s=='\n' || *s=='\f') s++;
    if (*s=='+') s++;
    if (*s<'0' || *s>'9') return fallback;
    uint64_t value=0;
    while (*s>='0' && *s<='9') {
        value=value*10+(unsigned)(*s++-'0');
        if (value>0x7fffffff) return fallback;
    }
    return (unsigned)value;
}
static void release(node_t *n) {
    struct web_canvas *c=n->canvas;
    if (!c) return;
    free(c->pixels); c->pixels=NULL;
    web_doc *d=n->allocation_doc?n->allocation_doc:n->owner;
    if (d && d->canvas_bytes>=c->bytes) d->canvas_bytes-=c->bytes;
    c->bytes=0;
}
void web_canvas_free(node_t *n) { if (n) { release(n); free(n->canvas); n->canvas=NULL; } }
void web_canvas_attr_changed(node_t *n,const char *name) {
    if (n && n->canvas && (!strcmp(name,"width") || !strcmp(name,"height"))) {
        release(n); n->canvas->generation++;
        n->canvas->tainted=false;
        n->canvas->w=web_canvas_dimension(n,"width"); n->canvas->h=web_canvas_dimension(n,"height");
    }
}
static struct web_canvas *ensure(node_t *n) {
    if (!n->canvas) { n->canvas=calloc(1,sizeof *n->canvas); if (!n->canvas) return NULL; }
    struct web_canvas *c=n->canvas;
    unsigned w=web_canvas_dimension(n,"width"),h=web_canvas_dimension(n,"height");
    if (c->w!=w || c->h!=h) { release(n); c->w=w; c->h=h; c->generation++; c->tainted=false; }
    if (!w || !h) return c;
    if (c->pixels) return c;
    if ((uint64_t)w*h>CANVAS_MAX_PIXELS) return NULL;
    web_doc *d=n->allocation_doc?n->allocation_doc:n->owner;
    size_t bytes=(size_t)w*h*4;
    if (!d || d->canvas_bytes>CANVAS_BUDGET || bytes>CANVAS_BUDGET-d->canvas_bytes) return NULL;
    c->pixels=calloc((size_t)w*h,4);
    if (!c->pixels) return NULL;
    c->bytes=bytes; d->canvas_bytes+=bytes;
    return c;
}
static uint32_t over(uint32_t d,uint32_t s) {
    unsigned sa=s>>24,da=d>>24;
    if (sa==255) return s;
    if (!sa) return d;
    unsigned old=(da*(255-sa)+127)/255,alpha=sa+old;
    unsigned r=(((s>>16)&255)*sa+((d>>16)&255)*old+alpha/2)/alpha;
    unsigned g=(((s>>8)&255)*sa+((d>>8)&255)*old+alpha/2)/alpha;
    unsigned b=((s&255)*sa+(d&255)*old+alpha/2)/alpha;
    return (alpha<<24)|(r<<16)|(g<<8)|b;
}
/* Keep conversion exceptions, rather than returning a value with an exception
 * still pending in QuickJS. A nonfinite number is an ordinary ignored draw. */
static int numeric(JSContext *ctx,int argc,JSValueConst *argv,int start,double *v,int count) {
    if (argc<start+count) return 0;
    for (int i=0;i<count;i++) {
        if (JS_ToFloat64(ctx,&v[i],argv[start+i])<0) return -1;
        if (!isfinite(v[i])) return 0;
    }
    return 1;
}
static void changed(node_t *n) { if (n->owner) n->owner->dirty=true; }
/* Bilinear in premultiplied space, then return straight ARGB for over().
 * Sampling clamps to the image edge, not to an internal sprite crop edge. */
static uint32_t image_sample(const uint32_t *px,unsigned w,unsigned h,double x,double y,bool smooth) {
    if (!smooth) {
        unsigned ix=(unsigned)fmax(0,fmin(w-1,floor(x))),iy=(unsigned)fmax(0,fmin(h-1,floor(y)));
        return px[(size_t)iy*w+ix];
    }
    x=fmax(0,fmin(w-1,x-0.5)); y=fmax(0,fmin(h-1,y-0.5));
    unsigned ix=(unsigned)x,iy=(unsigned)y,jx=ix+1<w?ix+1:ix,jy=iy+1<h?iy+1:iy;
    double tx=x-ix,ty=y-iy,weights[4]={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
    uint32_t p[4]={px[(size_t)iy*w+ix],px[(size_t)iy*w+jx],px[(size_t)jy*w+ix],px[(size_t)jy*w+jx]};
    double a=0,r=0,g=0,b=0;
    for (unsigned i=0;i<4;i++) {
        double v=(p[i]>>24)*weights[i]; a+=v;
        r+=((p[i]>>16)&255)*v; g+=((p[i]>>8)&255)*v; b+=(p[i]&255)*v;
    }
    unsigned alpha=(unsigned)fmin(255,a+0.5);
    if (!alpha) return 0;
    return (alpha<<24)|((unsigned)fmin(255,r/a+0.5)<<16)|((unsigned)fmin(255,g/a+0.5)<<8)|(unsigned)fmin(255,b/a+0.5);
}
static bool image_clip_axis(double *s,double *span,double *d,double *length,unsigned extent) {
    double lo=fmax(0,*s),hi=fmin(extent,*s+*span);
    if (!(hi>lo)) return false;
    double offset=(lo-*s)/ *span,fraction=(hi-lo)/ *span;
    *d+=*length*offset; *length*=fraction; *s=lo; *span=hi-lo;
    return isfinite(*d) && isfinite(*length) && *length>0;
}
static bool image_gpu_blit(struct web_canvas *c,web_doc *allocation,const uint32_t *pixels,
                           unsigned w,unsigned h,double sx,double sy,double sw,double sh,
                           double ox,double oy,double dw,double dh) {
    /* A bounded replace/nearest subset of drawImage. Source-over is identical
     * only when every sampled source texel is opaque. The existing RAM path
     * handles alpha, smoothing, rotation, reflection, fractional and clipped
     * draws; GPU failure must never corrupt that authoritative bitmap. */
    if (w>N_GPU_MAX_SIDE || h>N_GPU_MAX_SIDE || dw<1 || dh<1 || dw>N_GPU_MAX_SIDE || dh>N_GPU_MAX_SIDE ||
        ox<0 || oy<0 || ox+dw>c->w || oy+dh>c->h ||
        sx!=floor(sx) || sy!=floor(sy) || sw!=floor(sw) || sh!=floor(sh) ||
        ox!=floor(ox) || oy!=floor(oy) || dw!=floor(dw) || dh!=floor(dh)) return false;
    struct n_gpu_info info;
    if (gpu_info(&info)<0 || !(info.capabilities&N_GPU_CAP_BLIT)) return false;
    unsigned x0=(unsigned)sx,y0=(unsigned)sy,cw=(unsigned)sw,ch=(unsigned)sh;
    for (unsigned y=y0;y<y0+ch;y++) for (unsigned x=x0;x<x0+cw;x++)
        if (pixels[(size_t)y*w+x]>>24!=255) return false;
    size_t bytes=(size_t)(unsigned)dw*(unsigned)dh*4;
    if (!allocation || allocation->canvas_bytes>CANVAS_BUDGET || bytes>CANVAS_BUDGET-allocation->canvas_bytes) return false;
    uint32_t *output=malloc(bytes);
    if (!output) return false;
    allocation->canvas_bytes+=bytes;
    struct n_gpu_blit r={.source_width=w,.source_height=h,.source_x=x0,.source_y=y0,.source_w=cw,.source_h=ch,
        .width=(unsigned)dw,.height=(unsigned)dh};
    bool ok=gpu_blit(&r,pixels,(size_t)w*h*4,output,bytes)==0;
    if (ok) for (unsigned y=0;y<r.height;y++)
        memcpy(c->pixels+(size_t)((unsigned)oy+y)*c->w+(unsigned)ox,output+(size_t)y*r.width,r.width*4);
    free(output); allocation->canvas_bytes-=bytes;
    return ok;
}
JSValue web_canvas_draw_image(JSContext *ctx,node_t *n,node_t *source,int argc,JSValueConst *argv) {
    if (!n || n->type!=N_ELEM || n->foreign || n->tag!=T_canvas)
        return JS_ThrowTypeError(ctx,"Illegal Canvas receiver");
    if (!source || source->type!=N_ELEM || source->foreign || (source->tag!=T_canvas && source->tag!=T_img))
        return JS_ThrowTypeError(ctx,"Expected HTMLCanvasElement or HTMLImageElement");
    if (argc!=4) return JS_ThrowTypeError(ctx,"Invalid drawImage packet");
    size_t coord_bytes=0,matrix_bytes=0;
    uint8_t *coords=JS_GetArrayBuffer(ctx,&coord_bytes,argv[0]);
    if (!coords) return JS_EXCEPTION;
    uint8_t *matrix=JS_GetArrayBuffer(ctx,&matrix_bytes,argv[1]);
    if (!matrix) return JS_EXCEPTION;
    if ((coord_bytes!=16 && coord_bytes!=32 && coord_bytes!=64) || matrix_bytes!=48)
        return JS_ThrowTypeError(ctx,"Invalid drawImage buffer length");
    double v[8]={0},m[6],alpha;
    memcpy(v,coords,coord_bytes); memcpy(m,matrix,sizeof m);
    if (JS_ToFloat64(ctx,&alpha,argv[2])<0) return JS_EXCEPTION;
    bool smooth=JS_ToBool(ctx,argv[3])>0;
    for (unsigned i=0;i<coord_bytes/8;i++) if (!isfinite(v[i])) return JS_UNDEFINED;
    for (unsigned i=0;i<6;i++) if (!isfinite(m[i])) return JS_UNDEFINED;
    if (!isfinite(alpha) || alpha<0 || alpha>1) return JS_UNDEFINED;

    /* No author callback runs after acquiring these borrowed pixels. Images
       belong to their owner document and are freed only with that document. */
    const uint32_t *pixels; unsigned w,h; bool tainted;
    if (source->tag==T_canvas) {
        if (!web_canvas_dimension(source,"width") || !web_canvas_dimension(source,"height")) return JS_NewInt32(ctx,1);
        struct web_canvas *src=ensure(source);
        if (!src) return JS_ThrowRangeError(ctx,"Canvas bitmap memory limit");
        pixels=src->pixels; w=src->w; h=src->h; tainted=src->tainted;
    } else {
        web_doc *d=source->owner;
        if (!d) return JS_NewInt32(ctx,1);
        if (d->resources_dirty) doc_sync_tree(d);
        doc_image_sync(d,source);
        struct web_image *im=source->image>=0 && source->image<d->images.n?d->images.v[source->image]:NULL;
        if (!im || !im->done) return JS_UNDEFINED;
        if (im->failed || !im->img || !im->img->px || im->img->w<=0 || im->img->h<=0) return JS_NewInt32(ctx,1);
        w=(unsigned)im->img->w; h=(unsigned)im->img->h;
        if ((uint64_t)w*h>IMAGE_MAX_PIXELS) return JS_ThrowRangeError(ctx,"Image bitmap limit");
        pixels=im->img->px;
        /* The current cache lacks redirect/CORS provenance. Conservatively
           taint ALL network/unknown-origin images, even same-origin HTTP. */
        tainted=!im->url || strncasecmp(im->url,"data:",5)!=0;
    }
    double sx=0,sy=0,sw=w,sh=h,dx=v[0],dy=v[1],dw=w,dh=h;
    if (coord_bytes==32) { dw=v[2]; dh=v[3]; }
    else if (coord_bytes==64) { sx=v[0]; sy=v[1]; sw=v[2]; sh=v[3]; dx=v[4]; dy=v[5]; dw=v[6]; dh=v[7]; }
    if (!sw || !sh) return JS_UNDEFINED;
    if (sw<0) { sx+=sw; sw=-sw; } if (sh<0) { sy+=sh; sh=-sh; }
    if (dw<0) { dx+=dw; dw=-dw; } if (dh<0) { dy+=dh; dh=-dh; }
    bool crop_inside=sx>=0 && sy>=0 && sx+sw<=w && sy+sh<=h;
    struct web_canvas *c=ensure(n);
    if (!c) return JS_ThrowRangeError(ctx,"Canvas bitmap memory limit");
    /* Clearing/putImageData never untaints; only resetting bitmap dimensions
       does. A fully clipped or zero-alpha draw still carries the origin. */
    if (tainted) c->tainted=true;
    if (!c->pixels || !dw || !dh || !isfinite(sx) || !isfinite(sy) || !isfinite(dx) || !isfinite(dy) ||
        !image_clip_axis(&sx,&sw,&dx,&dw,w) || !image_clip_axis(&sy,&sh,&dy,&dh,h)) return JS_UNDEFINED;
    double ox=m[0]*dx+m[2]*dy+m[4],oy=m[1]*dx+m[3]*dy+m[5];
    double ux=m[0]*dw,uy=m[1]*dw,vx=m[2]*dh,vy=m[3]*dh;
    double xs[4]={ox,ox+ux,ox+vx,ox+ux+vx},ys[4]={oy,oy+uy,oy+vy,oy+uy+vy};
    double minx=xs[0],maxx=xs[0],miny=ys[0],maxy=ys[0];
    for (unsigned i=0;i<4;i++) {
        if (!isfinite(xs[i]) || !isfinite(ys[i])) return JS_UNDEFINED;
        minx=fmin(minx,xs[i]); maxx=fmax(maxx,xs[i]); miny=fmin(miny,ys[i]); maxy=fmax(maxy,ys[i]);
    }
    double scale=fmax(fmax(fabs(ux),fabs(uy)),fmax(fabs(vx),fabs(vy)));
    if (!(scale>0) || !isfinite(scale)) return JS_UNDEFINED;
    double a=ux/scale,b=uy/scale,e=vx/scale,f=vy/scale,det=a*f-b*e;
    if (!det || !isfinite(det)) return JS_UNDEFINED;
    int x0=(int)floor(fmax(0,fmin(c->w,minx))),x1=(int)ceil(fmax(0,fmin(c->w,maxx)));
    int y0=(int)floor(fmax(0,fmin(c->h,miny))),y1=(int)ceil(fmax(0,fmin(c->h,maxy)));
    if (x0>=x1 || y0>=y1) return JS_UNDEFINED;
    uint32_t *snapshot=NULL; size_t bytes=0;
    web_doc *allocation=n->allocation_doc?n->allocation_doc:n->owner;
    if (source==n) {
        bytes=(size_t)w*h*4;
        if (!allocation || allocation->canvas_bytes>CANVAS_BUDGET || bytes>CANVAS_BUDGET-allocation->canvas_bytes)
            return JS_ThrowRangeError(ctx,"Canvas snapshot memory limit");
        snapshot=malloc(bytes);
        if (!snapshot) return JS_ThrowOutOfMemory(ctx);
        allocation->canvas_bytes+=bytes; memcpy(snapshot,pixels,bytes); pixels=snapshot;
    }
    /* Origin and self-copy snapshot semantics above are common to BOTH paths.
     * The native source never escapes this synchronous call. */
    if (!smooth && alpha==1 && crop_inside && m[1]==0 && m[2]==0 && m[0]>0 && m[3]>0 &&
        image_gpu_blit(c,allocation,pixels,w,h,sx,sy,sw,sh,ox,oy,ux,vy)) {
        free(snapshot); if (bytes) allocation->canvas_bytes-=bytes;
        changed(n); return JS_UNDEFINED;
    }
    /* Work is bounded by the destination bitmap (at most 1M pixels), not
       author dimensions. Only finite, clamped source positions become indices. */
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++) {
        double px=(x+0.5)/scale-ox/scale,py=(y+0.5)/scale-oy/scale;
        double u=(px*f-py*e)/det,t=(py*a-px*b)/det;
        if (!isfinite(u) || !isfinite(t) || u<0 || u>=1 || t<0 || t>=1) continue;
        uint32_t p=image_sample(pixels,w,h,sx+u*sw,sy+t*sh,smooth);
        p=(p&0xffffff)|((unsigned)((p>>24)*alpha+0.5)<<24);
        uint32_t *dst=&c->pixels[(size_t)y*c->w+x]; *dst=over(*dst,p);
    }
    free(snapshot); if (bytes) allocation->canvas_bytes-=bytes;
    changed(n); return JS_UNDEFINED;
}
static void rectangle(struct web_canvas *c,double *v,uint32_t color,bool clear) {
    if (!c->pixels) return;
    double ax=v[0],ay=v[1],bx=ax+v[2],by=ay+v[3];
    if (bx<ax) { double t=ax; ax=bx; bx=t; } if (by<ay) { double t=ay; ay=by; by=t; }
    ax=fmax(0,fmin(c->w,ax)); ay=fmax(0,fmin(c->h,ay));
    bx=fmax(0,fmin(c->w,bx)); by=fmax(0,fmin(c->h,by));
    int x0=(int)floor(ax),y0=(int)floor(ay),x1=(int)ceil(bx),y1=(int)ceil(by);
    int w=x1-x0,h=y1-y0;
    /* Real native virgl clear only for opaque, integer-aligned sizable fills.
       Other Canvas work remains explicitly software; no WebGL is advertised. */
    if (!clear && color>>24==255 && w>=64 && h>=64 && w<=512 && h<=512 &&
        ax==x0 && ay==y0 && bx==x1 && by==y1) {
        size_t bytes=(size_t)w*h*4; uint32_t *pixels=malloc(bytes);
        struct n_gpu_render r={.width=(unsigned)w,.height=(unsigned)h,.operation=N_GPU_CLEAR,.clear_argb=color};
        if (pixels && gpu_render(&r,pixels,bytes)==0) {
            for (int y=0;y<h;y++) memcpy(c->pixels+(size_t)(y0+y)*c->w+x0,pixels+(size_t)y*w,(size_t)w*4);
            free(pixels); return;
        }
        free(pixels);
    }
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++) {
        double coverage=fmax(0,fmin(bx,x+1)-fmax(ax,x))*fmax(0,fmin(by,y+1)-fmax(ay,y));
        uint32_t *p=&c->pixels[(size_t)y*c->w+x];
        if (clear) { unsigned a=(unsigned)((*p>>24)*(1-coverage)+0.5); *p=a?(*p&0xffffff)|(a<<24):0; }
        else { unsigned a=(unsigned)((color>>24)*coverage+0.5); *p=over(*p,(color&0xffffff)|(a<<24)); }
    }
}
struct intersection { double x; int direction; };
static void polygon(struct web_canvas *c,const double *points,size_t count,uint32_t color,bool evenodd,bool clear) {
    if (!c->pixels || count<3) return;
    for (unsigned y=0;y<c->h;y++) {
        struct intersection hits[MAX_POINTS]; unsigned n=0; double yy=y+0.5;
        size_t first=0;
        while (first<count) {
            while (first<count && (!isfinite(points[first*2]) || !isfinite(points[first*2+1]))) first++;
            size_t end=first;
            while (end<count && isfinite(points[end*2]) && isfinite(points[end*2+1])) end++;
            for (size_t i=first;i<end;i++) {
                size_t j=i+1<end?i+1:first;
                double x0=points[i*2],y0=points[i*2+1],x1=points[j*2],y1=points[j*2+1];
                if ((y0<=yy && y1>yy) || (y1<=yy && y0>yy)) {
                    /* Weighted interpolation avoids overflowing x1-x0 for
                     * extreme but finite transformed web coordinates. */
                    double scale=fmax(fabs(y0),fabs(y1));
                    double t=scale>1 ? (yy/scale-y0/scale)/(y1/scale-y0/scale) : (yy-y0)/(y1-y0);
                    double hit=x0*(1-t)+x1*t;
                    if (isfinite(hit) && n<MAX_POINTS) hits[n++]=(struct intersection){hit,y1>y0?1:-1};
                }
            }
            first=end+1;
        }
        for (unsigned i=1;i<n;i++) { struct intersection h=hits[i]; unsigned j=i; while (j && hits[j-1].x>h.x) { hits[j]=hits[j-1]; j--; } hits[j]=h; }
        int winding=0;
        for (unsigned i=0;i+1<n;i++) {
            winding=evenodd?(winding^1):winding+hits[i].direction;
            if (!winding) continue;
            double from=fmax(0,fmin(c->w,hits[i].x)),to=fmax(0,fmin(c->w,hits[i+1].x));
            int x0=(int)ceil(from-0.5),x1=(int)ceil(to-0.5);
            for (int x=x0;x<x1;x++) { uint32_t *p=&c->pixels[(size_t)y*c->w+x]; *p=clear?0:over(*p,color); }
        }
    }
}
static void segment(struct web_canvas *c,double ax,double ay,double bx,double by,double width,uint32_t color) {
    if (!c->pixels) return;
    double dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;
    if (!len || !isfinite(len)) return;
    double half=width/2;
    int x0=(int)fmax(0,fmin(c->w,floor(fmin(ax,bx)-half))),x1=(int)fmax(0,fmin(c->w,ceil(fmax(ax,bx)+half)));
    int y0=(int)fmax(0,fmin(c->h,floor(fmin(ay,by)-half))),y1=(int)fmax(0,fmin(c->h,ceil(fmax(ay,by)+half)));
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++) {
        double px=x+0.5-ax,py=y+0.5-ay,t=(px*dx+py*dy)/len;
        if (t<0 || t>1) continue; /* default butt caps */
        double distance=fabs(px*dy-py*dx)/sqrt(len);
        if (distance<=half) { uint32_t *p=&c->pixels[(size_t)y*c->w+x]; *p=over(*p,color); }
    }
}
void web_canvas_paint(node_t *n,canvas_t *dst,int x,int y,int w,int h) {
    struct web_canvas *c=n?n->canvas:NULL;
    if (!c || !c->pixels || !c->w || !c->h || w<=0 || h<=0) return;
    int x0=MAX(x,dst->cx0),y0=MAX(y,dst->cy0),x1=MIN((int64_t)x+w,dst->cx1),y1=MIN((int64_t)y+h,dst->cy1);
    for (int yy=y0;yy<y1;yy++) for (int xx=x0;xx<x1;xx++) {
        unsigned sx=(unsigned)((uint64_t)(xx-x)*c->w/(unsigned)w),sy=(unsigned)((uint64_t)(yy-y)*c->h/(unsigned)h);
        uint32_t *p=&dst->px[(size_t)yy*dst->pitch+xx]; *p=over(*p,c->pixels[(size_t)sy*c->w+sx]);
    }
}
JSValue web_canvas_native(JSContext *ctx,node_t *n,int argc,JSValueConst *argv) {
    if (!n || n->type!=N_ELEM || n->foreign || n->tag!=T_canvas) return JS_ThrowTypeError(ctx,"Illegal Canvas receiver");
    if (!argc) return JS_UNDEFINED;
    const char *op=JS_ToCString(ctx,argv[0]); if (!op) return JS_EXCEPTION;
    JSValue out=JS_UNDEFINED;
    if (!strcmp(op,"width") || !strcmp(op,"height")) out=JS_NewUint32(ctx,web_canvas_dimension(n,op));
    else if (!strcmp(op,"color")) {
        const char *text=argc>1?JS_ToCString(ctx,argv[1]):NULL; uint32_t color;
        if (argc>1 && !text) { out=JS_EXCEPTION; goto done; }
        out=text && css_color(text,strlen(text),&color)?JS_NewUint32(ctx,color):JS_NULL;
        JS_FreeCString(ctx,text);
    } else if (!strcmp(op,"reset")) { web_canvas_attr_changed(n,"width"); changed(n); }
    else {
        struct web_canvas *c=ensure(n);
        if (!c) { out=!strcmp(op,"context")?JS_FALSE:JS_ThrowRangeError(ctx,"Canvas bitmap memory limit"); goto done; }
        if (!strcmp(op,"context")) out=JS_TRUE;
        else if (!strcmp(op,"version")) out=JS_NewUint32(ctx,c->generation);
        else if (!strcmp(op,"rect") || !strcmp(op,"clear")) {
            double v[4]; uint32_t color=0;
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            if (numbers) {
                if (argc>5 && JS_ToUint32(ctx,&color,argv[5])<0) { out=JS_EXCEPTION; goto done; }
                rectangle(c,v,color,!strcmp(op,"clear")); changed(n);
            }
        } else if (!strcmp(op,"poly") || !strcmp(op,"stroke")) {
            size_t bytes; uint8_t *data=argc>1?JS_GetArrayBuffer(ctx,&bytes,argv[1]):NULL;
            uint32_t color=0; double width=1; bool evenodd=false,clear=false;
            if (!data || bytes%16 || bytes/16>MAX_POINTS) { out=JS_ThrowRangeError(ctx,"Canvas path limit"); goto done; }
            if (argc>2 && JS_ToUint32(ctx,&color,argv[2])<0) { out=JS_EXCEPTION; goto done; }
            if (argc>3) {
                if (!strcmp(op,"stroke")) { if (JS_ToFloat64(ctx,&width,argv[3])<0) { out=JS_EXCEPTION; goto done; } }
                else evenodd=JS_ToBool(ctx,argv[3]);
            }
            if (argc>4) clear=JS_ToBool(ctx,argv[4]);
            double points[MAX_POINTS*2]; memcpy(points,data,bytes); size_t count=bytes/16;
            if (!strcmp(op,"poly")) polygon(c,points,count,color,evenodd,clear);
            else if (isfinite(width) && width>0) for (size_t i=1;i<count;i++) {
                const double *a=&points[(i-1)*2],*b=&points[i*2];
                if (isfinite(a[0]) && isfinite(a[1]) && isfinite(b[0]) && isfinite(b[1])) segment(c,a[0],a[1],b[0],b[1],width,color);
            }
            changed(n);
        } else if (!strcmp(op,"read")) {
            double v[4];
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            if (!numbers || v[2]<1 || v[3]<1 || v[2]>4096 || v[3]>4096 || v[2]*v[3]>CANVAS_MAX_PIXELS || fabs(v[0])>1e9 || fabs(v[1])>1e9) {
                out=JS_ThrowRangeError(ctx,"Invalid ImageData size"); goto done;
            }
            if (c->tainted) { out=JS_NULL; goto done; }
            int x=(int)v[0],y=(int)v[1]; unsigned w=(unsigned)v[2],h=(unsigned)v[3]; size_t bytes=(size_t)w*h*4;
            uint8_t *data=calloc(1,bytes);
            if (!data) { out=JS_ThrowOutOfMemory(ctx); goto done; }
            for (unsigned j=0;j<h;j++) for (unsigned i=0;i<w;i++) {
                int64_t xx=(int64_t)x+i,yy=(int64_t)y+j;
                if (!c->pixels || xx<0 || yy<0 || xx>=c->w || yy>=c->h) continue;
                uint32_t p=c->pixels[(size_t)yy*c->w+(size_t)xx]; size_t at=((size_t)j*w+i)*4;
                data[at]=(p>>16)&255; data[at+1]=(p>>8)&255; data[at+2]=p&255; data[at+3]=p>>24;
            }
            out=JS_NewArrayBufferCopy(ctx,data,bytes); free(data);
        } else if (!strcmp(op,"put")) {
            double v[4]; size_t bytes=0;
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            uint8_t *data=argc>5?JS_GetArrayBuffer(ctx,&bytes,argv[5]):NULL;
            if (!numbers || v[2]<1 || v[3]<1 || v[2]>4096 || v[3]>4096 || v[2]*v[3]>CANVAS_MAX_PIXELS ||
                fabs(v[0])>1e9 || fabs(v[1])>1e9 || !data || bytes!=(size_t)(unsigned)v[2]*(unsigned)v[3]*4) {
                out=JS_ThrowRangeError(ctx,"Invalid ImageData buffer"); goto done;
            }
            int x=(int)v[0],y=(int)v[1]; unsigned w=(unsigned)v[2],h=(unsigned)v[3];
            for (unsigned j=0;j<h;j++) for (unsigned i=0;i<w;i++) {
                int64_t xx=(int64_t)x+i,yy=(int64_t)y+j;
                if (!c->pixels || xx<0 || yy<0 || xx>=c->w || yy>=c->h) continue;
                size_t at=((size_t)j*w+i)*4; c->pixels[(size_t)yy*c->w+(size_t)xx]=((uint32_t)data[at+3]<<24)|((uint32_t)data[at]<<16)|((uint32_t)data[at+1]<<8)|data[at+2];
            }
            changed(n);
        } else out=JS_ThrowTypeError(ctx,"Unsupported Canvas operation");
    }
done:
    JS_FreeCString(ctx,op); return out;
}
