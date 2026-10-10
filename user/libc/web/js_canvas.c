/* Actual Canvas2D RAM bitmap. No dummy WebGL or falsely successful methods.
 * Bitmap/temporary storage follows real allocation and size representation. Attribute changes
 * release/reset the bitmap, detached nodes remain allocation-document owned. */
#include "js_canvas.h"
#include "js_memory.h"
#include "gpu.h"
#include "png.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* Immutable clip masks share ownership across save()/restore(). Every live
 * mask (including saved states) is charged to the allocation document. */
struct canvas_clip { unsigned refs; size_t bytes; uint8_t pixels[]; };
struct web_canvas {
    unsigned w,h,generation;
    uint32_t *pixels;
    size_t bytes;
    bool tainted;
    struct canvas_clip *clip,**saved_clip;
    size_t saved_count,saved_capacity,saved_bytes;
};
struct image_bitmap { unsigned w,h;bool tainted;uint32_t *pixels; };
static JSClassID bitmap_class;
static void bitmap_finalizer(JSRuntime *rt,JSValue object) {
    struct image_bitmap *b=JS_GetOpaque(object,bitmap_class);
    if(b){js_free_rt(rt,b->pixels);js_free_rt(rt,b);}
}
bool web_image_bitmap_is(JSValueConst value) {
    return bitmap_class && JS_GetOpaque(value,bitmap_class)!=NULL;
}
bool web_image_bitmap_dimensions(JSValueConst value,unsigned *width,unsigned *height) {
    struct image_bitmap *b=bitmap_class?JS_GetOpaque(value,bitmap_class):NULL;
    if(!b)return false;*width=b->w;*height=b->h;return true;
}
static web_doc *allocation_doc(node_t *n) { return n->allocation_doc?n->allocation_doc:n->owner; }
/* Native bitmap/gfx dimensions and pitch are int. No area or document quota:
 * reject only unrepresentable sizes, accounting overflow, or actual OOM. */
static bool canvas_extent(size_t w,size_t h,size_t pixel_bytes,size_t *bytes) {
    if (!w || !h || w>INT32_MAX || h>INT32_MAX || !pixel_bytes || h>SIZE_MAX/w || w*h>SIZE_MAX/pixel_bytes) return false;
    *bytes=w*h*pixel_bytes; return true;
}
static bool canvas_room(const web_doc *d,size_t bytes) {
    return d && bytes<=SIZE_MAX-d->canvas_bytes;
}
static bool canvas_save(node_t *n,struct web_canvas *c) {
    if (c->clip && c->clip->refs==UINT32_MAX) return false;
    if (c->saved_count==c->saved_capacity) {
        if (c->saved_count==SIZE_MAX/sizeof *c->saved_clip) return false;
        size_t capacity=c->saved_capacity?c->saved_capacity:8,required=c->saved_count+1;
        if (capacity<required) capacity=capacity>SIZE_MAX/sizeof *c->saved_clip/2?required:capacity*2;
        if (capacity>SIZE_MAX/sizeof *c->saved_clip) return false;
        size_t bytes=capacity*sizeof *c->saved_clip,extra=bytes-c->saved_bytes;
        web_doc *d=allocation_doc(n);
        if (!canvas_room(d,extra)) return false;
        struct canvas_clip **saved=realloc(c->saved_clip,bytes);
        if (!saved) return false;
        c->saved_clip=saved;c->saved_capacity=capacity;c->saved_bytes=bytes;d->canvas_bytes+=extra;
    }
    c->saved_clip[c->saved_count++]=c->clip;
    if (c->clip) c->clip->refs++;
    return true;
}
static void clip_release(node_t *n,struct canvas_clip *clip) {
    if (!clip || --clip->refs) return;
    web_doc *d=allocation_doc(n);
    if (d && d->canvas_bytes>=clip->bytes) d->canvas_bytes-=clip->bytes;
    free(clip);
}
static bool visible(const struct web_canvas *c,unsigned x,unsigned y) {
    return !c->clip || c->clip->pixels[(size_t)y*c->w+x];
}
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
    clip_release(n,c->clip); c->clip=NULL;
    while (c->saved_count) { size_t i=--c->saved_count; clip_release(n,c->saved_clip[i]); }
    web_doc *d=n->allocation_doc?n->allocation_doc:n->owner;
    if (d && d->canvas_bytes>=c->bytes) d->canvas_bytes-=c->bytes;
    if (d && d->canvas_bytes>=c->saved_bytes) d->canvas_bytes-=c->saved_bytes;
    free(c->saved_clip);c->saved_clip=NULL;c->saved_capacity=c->saved_bytes=0;
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
    web_doc *d=n->allocation_doc?n->allocation_doc:n->owner;
    size_t bytes;
    if (!canvas_extent(w,h,4,&bytes) || !canvas_room(d,bytes)) return NULL;
    c->pixels=calloc(1,bytes);
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
/* The same usable bitmap and conservative origin provenance serve drawImage
   and ImageBitmap snapshots. Status 2 means an ordinary image is still loading;
   drawImage does nothing, while createImageBitmap rejects its unusable source. */
static int canvas_source(JSContext *ctx,node_t *source,const uint32_t **pixels,unsigned *w,unsigned *h,bool *tainted) {
    if(!source || source->type!=N_ELEM || source->foreign || (source->tag!=T_canvas&&source->tag!=T_img)){
        JS_ThrowTypeError(ctx,"Expected HTMLCanvasElement or HTMLImageElement");return -1;
    }
    if(source->tag==T_canvas){
        if(!web_canvas_dimension(source,"width")||!web_canvas_dimension(source,"height"))return 1;
        struct web_canvas *c=ensure(source);if(!c){JS_ThrowOutOfMemory(ctx);return -1;}
        *pixels=c->pixels;*w=c->w;*h=c->h;*tainted=c->tainted;return 0;
    }
    web_doc *d=source->owner;if(!d)return 1;
    if(d->resources_dirty)doc_sync_tree(d);doc_image_sync(d,source);
    struct web_image *im=source->image>=0&&source->image<d->images.n?d->images.v[source->image]:NULL;
    if(!im||!im->done)return 2;
    if(im->failed||!im->img||!im->img->px||im->img->w<=0||im->img->h<=0)return 1;
    *w=(unsigned)im->img->w;*h=(unsigned)im->img->h;
    if((uint64_t)(*w)*(*h)>IMAGE_MAX_PIXELS){JS_ThrowRangeError(ctx,"Image bitmap limit");return -1;}
    *pixels=im->img->px;*tainted=!im->url||strncasecmp(im->url,"data:",5)!=0;return 0;
}
static JSValue canvas_export(JSContext *ctx,node_t *n,struct web_canvas *c,bool data_url) {
    if (c && c->tainted) return JS_FALSE; /* JS throws SecurityError synchronously. */
    if (!c || !c->w || !c->h || !c->pixels) return data_url?JS_NewString(ctx,"data:,"):JS_NULL;
    size_t png_size=png_encode_rgba_bound((int)c->w,(int)c->h);
    size_t url_size=0,reserve=png_size;
    if (data_url) {
        if (png_size>SIZE_MAX-2 || (png_size+2)/3>(SIZE_MAX-23)/4) return JS_NewString(ctx,"data:,");
        url_size=22+((png_size+2)/3)*4;
        if (url_size+1>SIZE_MAX-reserve) return JS_NewString(ctx,"data:,");
        reserve+=url_size+1;
    }
    web_doc *d=allocation_doc(n);
    if (!png_size || !canvas_room(d,reserve))
        return data_url?JS_NewString(ctx,"data:,"):JS_NULL;
    d->canvas_bytes+=reserve;
    uint8_t *png=NULL; size_t size=0; JSValue out;
    if (png_encode_rgba(c->pixels,(int)c->w,(int)c->h,(int)c->w,&png,&size)<0)
        out=data_url?JS_NewString(ctx,"data:,"):JS_NULL;
    else if (!data_url) out=JS_NewArrayBufferCopy(ctx,png,size);
    else {
        char *url=malloc(url_size+1);
        if (!url) out=JS_NewString(ctx,"data:,");
        else {
            static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            memcpy(url,"data:image/png;base64,",22); char *at=url+22;
            for (size_t i=0;i<size;i+=3) {
                uint32_t value=(uint32_t)png[i]<<16;
                if (i+1<size) value|=(uint32_t)png[i+1]<<8;
                if (i+2<size) value|=png[i+2];
                *at++=alphabet[value>>18]; *at++=alphabet[(value>>12)&63];
                *at++=i+1<size?alphabet[(value>>6)&63]:'='; *at++=i+2<size?alphabet[value&63]:'=';
            }
            *at=0; out=JS_NewStringLen(ctx,url,(size_t)(at-url)); free(url);
        }
    }
    free(png); d->canvas_bytes-=reserve; return out;
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
static void bitmap_service(void *context) { web_native_checkpoint(context); }
static uint32_t bitmap_rgba_sample(const uint8_t *rgba,unsigned w,unsigned h,double x,double y,bool smooth) {
    if(!smooth){
        size_t i=((size_t)(unsigned)floor(y)*w+(unsigned)floor(x))*4;
        return (uint32_t)rgba[i+3]<<24|(uint32_t)rgba[i]<<16|(uint32_t)rgba[i+1]<<8|rgba[i+2];
    }
    /* Match the ordinary image sampler: interpolate associated channels,
       then store straight ARGB. Transparent RGB must not bleed into edges. */
    x=fmax(0,fmin(w-1,x-0.5));y=fmax(0,fmin(h-1,y-0.5));
    unsigned ix=(unsigned)x,iy=(unsigned)y,jx=ix+1<w?ix+1:ix,jy=iy+1<h?iy+1:iy;
    size_t indexes[4]={((size_t)iy*w+ix)*4,((size_t)iy*w+jx)*4,((size_t)jy*w+ix)*4,((size_t)jy*w+jx)*4};
    double tx=x-ix,ty=y-iy,weights[4]={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty},a=0,r=0,g=0,b=0;
    for(unsigned i=0;i<4;i++){const uint8_t *p=rgba+indexes[i];double v=p[3]*weights[i];a+=v;r+=p[0]*v;g+=p[1]*v;b+=p[2]*v;}
    unsigned alpha=(unsigned)fmin(255,a+0.5);if(!alpha)return 0;
    return alpha<<24|(unsigned)fmin(255,r/a+0.5)<<16|(unsigned)fmin(255,g/a+0.5)<<8|(unsigned)fmin(255,b/a+0.5);
}
/* The current raster decoder has no EXIF orientation stage. Do not silently
   claim from-image orientation for tagged encoded data. flipY explicitly
   disregards that metadata. Only container headers, not arbitrary byte text,
   establish this unsupported metadata case. */
static bool bitmap_has_exif(const uint8_t *data,size_t size) {
    if(size>=2&&data[0]==0xff&&data[1]==0xd8){
        size_t at=2;
        while(at<size){
            if(data[at++]!=0xff)break;while(at<size&&data[at]==0xff)at++;
            if(at==size)break;unsigned marker=data[at++];
            if(marker==0xda||marker==0xd9)break;if(marker==0x01||(marker>=0xd0&&marker<=0xd7))continue;
            if(size-at<2)break;size_t length=(size_t)data[at]*256+data[at+1];
            if(length<2||length>size-at)break;
            if(marker==0xe1&&length>=8&&!memcmp(data+at+2,"Exif\0\0",6))return true;at+=length;
        }
    }else if(size>=8&&!memcmp(data,"\211PNG\r\n\032\n",8)){
        for(size_t at=8;size-at>=12;){
            uint32_t length=(uint32_t)data[at]<<24|(uint32_t)data[at+1]<<16|(uint32_t)data[at+2]<<8|data[at+3];
            if(length>size-at-12)break;if(!memcmp(data+at+4,"eXIf",4))return true;at+=(size_t)length+12;
        }
    }else if(size>=12&&!memcmp(data,"RIFF",4)&&!memcmp(data+8,"WEBP",4)){
        for(size_t at=12;size-at>=8;){
            uint32_t length=(uint32_t)data[at+4]|(uint32_t)data[at+5]<<8|(uint32_t)data[at+6]<<16|(uint32_t)data[at+7]<<24;
            size_t span=(size_t)length+(length&1u);if(span>size-at-8)break;
            if(!memcmp(data+at,"EXIF",4))return true;at+=span+8;
        }
    }
    return false;
}
JSValue web_image_bitmap_native(JSContext *ctx,web_doc *document,node_t *node,int argc,JSValueConst *argv) {
    if(!argc||!JS_IsString(argv[0]))return JS_ThrowTypeError(ctx,"ImageBitmap operation required");
    const char *op=JS_ToCString(ctx,argv[0]);if(!op)return JS_EXCEPTION;
    bool create=!strcmp(op,"create"),close=!strcmp(op,"close"),width=!strcmp(op,"width"),height=!strcmp(op,"height");
    JS_FreeCString(ctx,op);
    if(!create){
        struct image_bitmap *b=argc>1&&bitmap_class?JS_GetOpaque2(ctx,argv[1],bitmap_class):NULL;
        if(!b)return JS_ThrowTypeError(ctx,"Illegal ImageBitmap receiver");
        if(close){uint32_t *pixels=b->pixels;b->pixels=NULL;b->w=b->h=0;js_free(ctx,pixels);return JS_UNDEFINED;}
        if(width)return JS_NewUint32(ctx,b->w);if(height)return JS_NewUint32(ctx,b->h);
        return JS_ThrowTypeError(ctx,"Unsupported ImageBitmap operation");
    }
    if(argc<5)return JS_ThrowTypeError(ctx,"Invalid ImageBitmap creation packet");
    int kind;if(JS_ToInt32(ctx,&kind,argv[1])<0)return JS_EXCEPTION;
    if(kind<0||kind>3)return JS_ThrowTypeError(ctx,"Unsupported ImageBitmap source");
    size_t packet_size=0;uint8_t *packet=JS_GetArrayBuffer(ctx,&packet_size,argv[3]);double v[9];
    if(!packet||packet_size!=sizeof v)return JS_ThrowTypeError(ctx,"Invalid ImageBitmap transform packet");
    memcpy(v,packet,sizeof v);for(unsigned i=0;i<9;i++)if(!isfinite(v[i]))return JS_ThrowTypeError(ctx,"Nonfinite ImageBitmap transform");
    if((v[0]!=0&&v[0]!=1)||(v[7]!=0&&v[7]!=1)||(v[8]!=0&&v[8]!=1))return JS_ThrowTypeError(ctx,"Invalid ImageBitmap flags");
    if(v[0]&&(!v[3]||!v[4]))return JS_NULL;
    const uint32_t *pixels=NULL;const uint8_t *rgba=NULL;unsigned w=0,h=0;bool tainted=false;
    image_t *decoded=NULL;JSValue result=JS_UNDEFINED;
    if(!web_native_checkpoint(document))return JS_ThrowInternalError(ctx,"ImageBitmap decoding cancelled");
    if(kind==0){
        size_t size=0;uint8_t *data=JS_GetArrayBuffer(ctx,&size,argv[2]);
        if(!data){JSValue exception=JS_GetException(ctx);JS_FreeValue(ctx,exception);return JS_NULL;}
        /* SVG's native fallback sizes are for ordinary document rendering,
           not a claim of valid Blob natural dimensions for this subset. */
        if(image_is_svg(data,size)||(!v[7]&&bitmap_has_exif(data,size)))return JS_NewInt32(ctx,1);
        decoded=image_decode_serviced(data,size,bitmap_service,document);
        if(!web_native_checkpoint(document)){image_free(decoded);return JS_ThrowInternalError(ctx,"ImageBitmap decoding cancelled");}
        if(!decoded||!decoded->px||decoded->w<=0||decoded->h<=0){image_free(decoded);return JS_NULL;}
        pixels=decoded->px;w=(unsigned)decoded->w;h=(unsigned)decoded->h;
    }else if(kind==1){
        size_t size=0;uint8_t *data=JS_GetArrayBuffer(ctx,&size,argv[2]);
        if(!data){JSValue exception=JS_GetException(ctx);JS_FreeValue(ctx,exception);return JS_NULL;}
        double dimensions[3];size_t dimensions_size=0;uint8_t *dimensions_data=JS_GetArrayBuffer(ctx,&dimensions_size,argv[4]);
        if(!dimensions_data||dimensions_size!=sizeof dimensions)return JS_ThrowTypeError(ctx,"Invalid ImageData bitmap packet");
        memcpy(dimensions,dimensions_data,sizeof dimensions);size_t bytes=0;
        for(unsigned i=0;i<3;i++)if(!isfinite(dimensions[i])||dimensions[i]<0||floor(dimensions[i])!=dimensions[i])return JS_NULL;
        /* SIZE_MAX rounds up to 2^64 as a double. Equality at that rounded
           boundary is already unrepresentable and must precede size_t casts. */
        if(dimensions[0]>INT32_MAX||dimensions[1]>INT32_MAX||dimensions[2]>=(double)SIZE_MAX)return JS_NULL;
        if(!canvas_extent((size_t)dimensions[0],(size_t)dimensions[1],4,&bytes)||dimensions[2]>size||bytes>size-(size_t)dimensions[2])return JS_NULL;
        w=(unsigned)dimensions[0];h=(unsigned)dimensions[1];rgba=data+(size_t)dimensions[2];
    }else if(kind==2){
        int status=canvas_source(ctx,node,&pixels,&w,&h,&tainted);
        if(status<0)return JS_EXCEPTION;if(status)return JS_NULL;
    }else{
        struct image_bitmap *source=bitmap_class?JS_GetOpaque2(ctx,argv[2],bitmap_class):NULL;
        if(!source)return JS_ThrowTypeError(ctx,"Illegal ImageBitmap source");
        if(!source->pixels||!source->w||!source->h)return JS_NULL;
        pixels=source->pixels;w=source->w;h=source->h;tainted=source->tainted;
    }
    double sx=v[0]?v[1]:0,sy=v[0]?v[2]:0,sw=v[0]?v[3]:w,sh=v[0]?v[4]:h;
    if(sw<0){sx+=sw;sw=-sw;}if(sh<0){sy+=sh;sh=-sh;}
    double ow=v[5]?v[5]:(v[6]?ceil(sw*v[6]/sh):sw),oh=v[6]?v[6]:(v[5]?ceil(sh*v[5]/sw):sh);
    size_t bytes=0;
    if(!(ow>0)||!(oh>0)||ow>INT32_MAX||oh>INT32_MAX||floor(ow)!=ow||floor(oh)!=oh||!canvas_extent((size_t)ow,(size_t)oh,4,&bytes)){
        result=JS_ThrowRangeError(ctx,"ImageBitmap size representation");goto done;
    }
    if(!bitmap_class)JS_NewClassID(&bitmap_class);
    if(!JS_IsRegisteredClass(JS_GetRuntime(ctx),bitmap_class)){
        JSClassDef definition={.class_name="NocturneImageBitmap",.finalizer=bitmap_finalizer};
        if(JS_NewClass(JS_GetRuntime(ctx),bitmap_class,&definition)<0){result=JS_ThrowOutOfMemory(ctx);goto done;}
    }
    struct image_bitmap *b=js_mallocz(ctx,sizeof *b);if(!b){result=JS_EXCEPTION;goto done;}
    b->pixels=js_malloc(ctx,bytes);if(!b->pixels){js_free(ctx,b);result=JS_EXCEPTION;goto done;}
    b->w=(unsigned)ow;b->h=(unsigned)oh;b->tainted=tainted;
    for(unsigned y=0;y<b->h;y++){
        if(!web_native_checkpoint(document)){js_free(ctx,b->pixels);js_free(ctx,b);result=JS_ThrowInternalError(ctx,"ImageBitmap snapshot cancelled");goto done;}
        double yy=sy+(v[7]?b->h-y-0.5:y+0.5)*sh/b->h;
        for(unsigned x=0;x<b->w;x++){
            double xx=sx+(x+0.5)*sw/b->w;uint32_t color=0;
            if(xx>=0&&yy>=0&&xx<w&&yy<h){
                if(rgba)color=bitmap_rgba_sample(rgba,w,h,xx,yy,v[8]!=0);
                else color=image_sample(pixels,w,h,xx,yy,v[8]!=0);
            }
            b->pixels[(size_t)y*b->w+x]=color;
        }
    }
    result=JS_NewObjectProtoClass(ctx,JS_NULL,bitmap_class);
    if(JS_IsException(result)){js_free(ctx,b->pixels);js_free(ctx,b);}else JS_SetOpaque(result,b);
done:
    image_free(decoded);return result;
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
    if (c->clip || w>N_GPU_MAX_SIDE || h>N_GPU_MAX_SIDE || dw<1 || dh<1 || dw>N_GPU_MAX_SIDE || dh>N_GPU_MAX_SIDE ||
        ox<0 || oy<0 || ox+dw>c->w || oy+dh>c->h ||
        sx!=floor(sx) || sy!=floor(sy) || sw!=floor(sw) || sh!=floor(sh) ||
        ox!=floor(ox) || oy!=floor(oy) || dw!=floor(dw) || dh!=floor(dh)) return false;
    struct n_gpu_info info;
    if (gpu_info(&info)<0 || !(info.capabilities&N_GPU_CAP_BLIT)) return false;
    unsigned x0=(unsigned)sx,y0=(unsigned)sy,cw=(unsigned)sw,ch=(unsigned)sh;
    for (unsigned y=y0;y<y0+ch;y++) for (unsigned x=x0;x<x0+cw;x++)
        if (pixels[(size_t)y*w+x]>>24!=255) return false;
    size_t bytes=(size_t)(unsigned)dw*(unsigned)dh*4;
    if (!canvas_room(allocation,bytes)) return false;
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
static JSValue canvas_draw_image(JSContext *ctx,node_t *n,node_t *source,struct image_bitmap *bitmap,int argc,JSValueConst *argv) {
    if (!n || n->type!=N_ELEM || n->foreign || n->tag!=T_canvas)
        return JS_ThrowTypeError(ctx,"Illegal Canvas receiver");
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
    if (bitmap) {
        if(!bitmap->pixels||!bitmap->w||!bitmap->h)return JS_NewInt32(ctx,1);
        pixels=bitmap->pixels;w=bitmap->w;h=bitmap->h;tainted=bitmap->tainted;
    } else {
        int status=canvas_source(ctx,source,&pixels,&w,&h,&tainted);
        if(status<0)return JS_EXCEPTION;if(status==1)return JS_NewInt32(ctx,1);if(status==2)return JS_UNDEFINED;
    }
    double sx=0,sy=0,sw=w,sh=h,dx=v[0],dy=v[1],dw=w,dh=h;
    if (coord_bytes==32) { dw=v[2]; dh=v[3]; }
    else if (coord_bytes==64) { sx=v[0]; sy=v[1]; sw=v[2]; sh=v[3]; dx=v[4]; dy=v[5]; dw=v[6]; dh=v[7]; }
    if (!sw || !sh) return JS_UNDEFINED;
    if (sw<0) { sx+=sw; sw=-sw; } if (sh<0) { sy+=sh; sh=-sh; }
    if (dw<0) { dx+=dw; dw=-dw; } if (dh<0) { dy+=dh; dh=-dh; }
    bool crop_inside=sx>=0 && sy>=0 && sx+sw<=w && sy+sh<=h;
    struct web_canvas *c=ensure(n);
    if (!c) return JS_ThrowOutOfMemory(ctx);
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
        if (!canvas_room(allocation,bytes)) return JS_ThrowRangeError(ctx,"Canvas snapshot accounting overflow");
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
    /* Work is clipped by the actual destination bitmap, not
       author dimensions. Only finite, clamped source positions become indices. */
    for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++) {
        if (!visible(c,x,y)) continue;
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
JSValue web_canvas_draw_image(JSContext *ctx,node_t *n,node_t *source,int argc,JSValueConst *argv) {
    return canvas_draw_image(ctx,n,source,NULL,argc,argv);
}
JSValue web_canvas_draw_bitmap(JSContext *ctx,node_t *n,JSValueConst token,int argc,JSValueConst *argv) {
    struct image_bitmap *b=bitmap_class?JS_GetOpaque2(ctx,token,bitmap_class):NULL;
    if(!b)return JS_ThrowTypeError(ctx,"Invalid ImageBitmap token");
    return canvas_draw_image(ctx,n,NULL,b,argc,argv);
}
/* A synchronous borrowed packet rooted by the native call's arguments. The
 * JS WeakMap owns stop storage and rebuilds it only on addColorStop; no native
 * gradient allocation, author callback, or CSS parse occurs per pixel. */
struct canvas_paint {
    unsigned kind;
    double alpha,g[6],matrix[6],inverse[4],matrix_scale,geometry_scale;
    const uint8_t *stops;
    size_t count;
    bool invertible,empty;
    web_doc *document;
};
static double paint_stop(const struct canvas_paint *p,size_t i,unsigned field) {
    double value;memcpy(&value,p->stops+(i*2+field)*sizeof value,sizeof value);return value;
}
static int paint_packet(JSContext *ctx,node_t *n,int argc,JSValueConst *argv,int index,struct canvas_paint *p) {
    memset(p,0,sizeof *p);p->document=n->owner;
    if(argc<=index || JS_IsUndefined(argv[index]))return 0;
    size_t bytes=0;uint8_t *data=JS_GetArrayBuffer(ctx,&bytes,argv[index]);
    if(!data || bytes<14*sizeof(double) || (bytes-14*sizeof(double))%(2*sizeof(double)))goto invalid;
    double header[14];memcpy(header,data,sizeof header);
    for(unsigned i=0;i<14;i++)if(!isfinite(header[i]))goto invalid;
    if((header[0]!=1&&header[0]!=2) || header[1]<0 || header[1]>1 || header[4]<0 || header[7]<0)goto invalid;
    p->kind=(unsigned)header[0];p->alpha=header[1];
    memcpy(p->matrix,header+8,sizeof p->matrix);
    p->stops=data+sizeof header;p->count=(bytes-sizeof header)/(2*sizeof(double));
    double previous=-1;
    for(size_t i=0;i<p->count;i++){
        double offset=paint_stop(p,i,0),color=paint_stop(p,i,1);
        if(!isfinite(offset)||offset<previous||offset<0||offset>1||!isfinite(color)||color<0||color>UINT32_MAX||floor(color)!=color)goto invalid;
        previous=offset;
    }
    p->geometry_scale=1;
    for(unsigned i=0;i<6;i++)p->geometry_scale=fmax(p->geometry_scale,fabs(header[i+2]));
    for(unsigned i=0;i<6;i++)p->g[i]=header[i+2]/p->geometry_scale;
    p->empty=!p->count || (header[2]==header[5]&&header[3]==header[6]&&(p->kind==1||header[4]==header[7])) ||
        (p->kind==2&&header[4]==0&&header[7]==0);
    const double *m=p->matrix;
    p->matrix_scale=fmax(fmax(fabs(m[0]),fabs(m[1])),fmax(fabs(m[2]),fabs(m[3])));
    if(p->matrix_scale>0){
        double a=m[0]/p->matrix_scale,b=m[1]/p->matrix_scale,c=m[2]/p->matrix_scale,d=m[3]/p->matrix_scale,det=a*d-b*c;
        if(det){p->inverse[0]=d/det;p->inverse[1]=-b/det;p->inverse[2]=-c/det;p->inverse[3]=a/det;
            p->invertible=isfinite(p->inverse[0])&&isfinite(p->inverse[1])&&isfinite(p->inverse[2])&&isfinite(p->inverse[3]);}
    }
    return 0;
invalid:
    JS_ThrowTypeError(ctx,"Invalid Canvas gradient paint packet");return -1;
}
static uint32_t paint_stops(const struct canvas_paint *p,double t) {
    if(isnan(t)||!p->count)return 0;
    uint32_t a,b;double fraction=0;
    if(t<paint_stop(p,0,0))a=b=(uint32_t)paint_stop(p,0,1);
    else {
        /* Upper bound preserves insertion order at coincident color stops. */
        size_t lo=0,hi=p->count;
        while(lo<hi){size_t mid=lo+(hi-lo)/2;if(paint_stop(p,mid,0)<=t)lo=mid+1;else hi=mid;}
        size_t left=lo?lo-1:0;a=(uint32_t)paint_stop(p,left,1);b=a;
        if(lo<p->count){b=(uint32_t)paint_stop(p,lo,1);fraction=(t-paint_stop(p,left,0))/(paint_stop(p,lo,0)-paint_stop(p,left,0));}
    }
    uint32_t result=0;
    /* HTML Canvas sRGB gradients interpolate RGB and alpha independently,
       without premultiplication; source-over below still uses real alpha. */
    for(unsigned shift=0;shift<32;shift+=8){double value=((a>>shift)&255)*(1-fraction)+((b>>shift)&255)*fraction;
        if(shift==24)value*=p->alpha;result|=(uint32_t)floor(value+0.5)<<shift;}
    return result;
}
static uint32_t paint_color(const struct canvas_paint *p,uint32_t solid,double x,double y) {
    if(!p || !p->kind)return solid;
    if(p->empty)return 0;
    const double *g=p->g,*m=p->matrix;double qx,qy;
    if(p->invertible){
        double xx=x/p->matrix_scale-m[4]/p->matrix_scale,yy=y/p->matrix_scale-m[5]/p->matrix_scale;
        qx=(p->inverse[0]*xx+p->inverse[2]*yy)/p->geometry_scale;
        qy=(p->inverse[1]*xx+p->inverse[3]*yy)/p->geometry_scale;
    }else{
        if(p->kind==2)return 0; /* Radial gradients under singular CTMs. */
        double sx=m[0]*g[0]+m[2]*g[1]+m[4]/p->geometry_scale,sy=m[1]*g[0]+m[3]*g[1]+m[5]/p->geometry_scale;
        double ex=m[0]*g[3]+m[2]*g[4]+m[4]/p->geometry_scale,ey=m[1]*g[3]+m[3]*g[4]+m[5]/p->geometry_scale;
        double dx=ex-sx,dy=ey-sy,scale=fmax(fabs(dx),fabs(dy));
        if(!(scale>0)||!isfinite(scale))return 0;
        dx/=scale;dy/=scale;
        return paint_stops(p,((x/p->geometry_scale-sx)/scale*dx+(y/p->geometry_scale-sy)/scale*dy)/(dx*dx+dy*dy));
    }
    if(!isfinite(qx)||!isfinite(qy))return 0;
    double ux=qx-g[0],uy=qy-g[1],dx=g[3]-g[0],dy=g[4]-g[1];
    if(p->kind==1){
        double scale=fmax(fabs(dx),fabs(dy));if(!(scale>0))return 0;dx/=scale;dy/=scale;
        return paint_stops(p,(ux/scale*dx+uy/scale*dy)/(dx*dx+dy*dy));
    }
    /* The radial cone consists of C(t)=C0+t(C1-C0), r(t)=r0+t(r1-r0).
       Painting in descending t selects the greatest root with positive
       radius. Negative-radius roots and points outside the cone never paint. */
    double scale=fmax(1,fmax(fabs(ux),fabs(uy)));ux/=scale;uy/=scale;dx/=scale;dy/=scale;
    double r0=g[2]/scale,dr=(g[5]-g[2])/scale;
    double a=dx*dx+dy*dy-dr*dr,b=-2*(ux*dx+uy*dy+r0*dr),c=ux*ux+uy*uy-r0*r0;
    /* Cancellation may hide an exact tangent after coordinate normalization.
       Compare with the terms that formed each coefficient, never an absolute
       epsilon: small but well-conditioned geometry must remain quadratic. */
    double tolerance=16*DBL_EPSILON;
    if(fabs(a)<=tolerance*(dx*dx+dy*dy+dr*dr))a=0;
    if(fabs(b)<=tolerance*2*(fabs(ux*dx)+fabs(uy*dy)+fabs(r0*dr)))b=0;
    if(fabs(c)<=tolerance*(ux*ux+uy*uy+r0*r0))c=0;
    double roots[2];unsigned count=0;
    if(a==0){
        if(b!=0)roots[count++]=-c/b;
        else if(c==0)return paint_stops(p,dr<0?-r0/dr:INFINITY);
    }
    else{
        double disc=b*b-4*a*c,tolerance=16*DBL_EPSILON*(b*b+fabs(4*a*c));
        if(disc<0&&disc>=-tolerance)disc=0;
        if(disc<0)return 0;
        double q=-0.5*(b+copysign(sqrt(disc),b));
        if(q==0)roots[count++]=-b/(2*a);
        else{roots[count++]=q/a;roots[count++]=c/q;}
    }
    double chosen=-INFINITY;
    /* Include the limiting color of a collapsed circle at its center. A
       zero-radius family was rejected above; this avoids a spurious one-pixel
       transparent hole in an otherwise continuous radial field. */
    for(unsigned i=0;i<count;i++)if(isfinite(roots[i])&&g[2]+roots[i]*(g[5]-g[2])>=0&&roots[i]>chosen)chosen=roots[i];
    return chosen==-INFINITY?0:paint_stops(p,chosen);
}
static bool paint_checkpoint(const struct canvas_paint *p,unsigned row) {
    (void)row;return !p || !p->kind || web_native_checkpoint(p->document);
}
static void rectangle(struct web_canvas *c,double *v,uint32_t color,bool clear,const struct canvas_paint *paint) {
    if (!c->pixels) return;
    double ax=v[0],ay=v[1],bx=ax+v[2],by=ay+v[3];
    if (bx<ax) { double t=ax; ax=bx; bx=t; } if (by<ay) { double t=ay; ay=by; by=t; }
    ax=fmax(0,fmin(c->w,ax)); ay=fmax(0,fmin(c->h,ay));
    bx=fmax(0,fmin(c->w,bx)); by=fmax(0,fmin(c->h,by));
    int x0=(int)floor(ax),y0=(int)floor(ay),x1=(int)ceil(bx),y1=(int)ceil(by);
    int w=x1-x0,h=y1-y0;
    /* Real native virgl clear only for opaque, integer-aligned sizable fills.
       Other Canvas work remains explicitly software; no WebGL is advertised. */
    if ((!paint || !paint->kind) && !c->clip && !clear && color>>24==255 && w>=64 && h>=64 && w<=512 && h<=512 &&
        ax==x0 && ay==y0 && bx==x1 && by==y1) {
        size_t bytes=(size_t)w*h*4; uint32_t *pixels=malloc(bytes);
        struct n_gpu_render r={.width=(unsigned)w,.height=(unsigned)h,.operation=N_GPU_CLEAR,.clear_argb=color};
        if (pixels && gpu_render(&r,pixels,bytes)==0) {
            for (int y=0;y<h;y++) memcpy(c->pixels+(size_t)(y0+y)*c->w+x0,pixels+(size_t)y*w,(size_t)w*4);
            free(pixels); return;
        }
        free(pixels);
    }
    for (int y=y0;y<y1;y++) { if(!paint_checkpoint(paint,(unsigned)y))break;
      for (int x=x0;x<x1;x++) {
        if (!visible(c,x,y)) continue;
        double coverage=fmax(0,fmin(bx,x+1)-fmax(ax,x))*fmax(0,fmin(by,y+1)-fmax(ay,y));
        uint32_t *p=&c->pixels[(size_t)y*c->w+x];
        if (clear) { unsigned a=(unsigned)((*p>>24)*(1-coverage)+0.5); *p=a?(*p&0xffffff)|(a<<24):0; }
        else { uint32_t source=paint_color(paint,color,x+0.5,y+0.5);unsigned a=(unsigned)((source>>24)*coverage+0.5); *p=over(*p,(source&0xffffff)|(a<<24)); }
    }}
}
struct intersection { double x; int direction; };
/* Shared half-open ray crossing. Weighted interpolation avoids overflowing
 * finite transformed coordinates; raster fill and point queries use the same
 * winding contribution, including the implicit closing edge of open paths. */
static bool edge_intersection(double x0,double y0,double x1,double y1,double y,double *x) {
    if (!((y0<=y && y1>y) || (y1<=y && y0>y))) return false;
    double scale=fmax(fabs(y0),fabs(y1));
    double t=scale>1 ? (y/scale-y0/scale)/(y1/scale-y0/scale) : (y-y0)/(y1-y0);
    *x=x0*(1-t)+x1*t; return isfinite(*x);
}
static bool point_on_edge(double ax,double ay,double bx,double by,double x,double y) {
    if (x<fmin(ax,bx) || x>fmax(ax,bx) || y<fmin(ay,by) || y>fmax(ay,by)) return false;
    /* Scale before subtraction/cross products, rather than overflowing with
       a valid path spanning -DBL_MAX to DBL_MAX. No pixel rounding is used. */
    double scale=fmax(fmax(fabs(ax),fabs(ay)),fmax(fmax(fabs(bx),fabs(by)),fmax(fabs(x),fabs(y))));
    if (!scale) return true;
    double dx=bx/scale-ax/scale,dy=by/scale-ay/scale,px=x/scale-ax/scale,py=y/scale-ay/scale;
    double left=dx*py,right=dy*px;
    return fabs(left-right)<=8*DBL_EPSILON*(fabs(left)+fabs(right));
}
static bool path_hit(const double *points,size_t count,double x,double y,bool evenodd) {
    int winding=0; size_t first=0;
    while (first<count) {
        while (first<count && (!isfinite(points[first*2]) || !isfinite(points[first*2+1]))) first++;
        size_t end=first;
        while (end<count && isfinite(points[end*2]) && isfinite(points[end*2+1])) end++;
        /* A lone moveTo has no line/path region. Two points still have their
           boundary, but their forward/reverse crossing cancels the area. */
        if (end-first>1) for (size_t i=first;i<end;i++) {
            size_t j=i+1<end?i+1:first;
            double ax=points[i*2],ay=points[i*2+1],bx=points[j*2],by=points[j*2+1],hit;
            if (point_on_edge(ax,ay,bx,by,x,y)) return true;
            if (edge_intersection(ax,ay,bx,by,y,&hit) && hit>x)
                winding=evenodd?(winding^1):winding+(by>ay?1:-1);
        }
        first=end+1;
    }
    return winding!=0;
}
/* Butt-cap segment predicate in the pen's coordinate system. */
static bool segment_hit(double ax,double ay,double bx,double by,double half,double x,double y) {
    double scale=fmax(fmax(fabs(ax),fabs(ay)),fmax(fmax(fabs(bx),fabs(by)),fmax(fabs(x),fabs(y))));
    scale=fmax(scale,half); if (!(scale>0)) return false;
    double dx=bx/scale-ax/scale,dy=by/scale-ay/scale,px=x/scale-ax/scale,py=y/scale-ay/scale;
    double length=hypot(dx,dy); if (!(length>0)) return false;
    double along=px*(dx/length)+py*(dy/length);
    double error=8*DBL_EPSILON*(fabs(px)+fabs(py)+length);
    if (along < -error || along>length+error) return false;
    return fabs(px*(dy/length)-py*(dx/length))<=half/scale+error;
}
static bool stroke_direction(const double *a,const double *b,double *x,double *y) {
    double scale=fmax(fmax(fabs(a[0]),fabs(a[1])),fmax(fabs(b[0]),fabs(b[1])));
    if (!(scale>0)) return false;
    double dx=b[0]/scale-a[0]/scale,dy=b[1]/scale-a[1]/scale,length=hypot(dx,dy);
    if (!(length>0)) return false; *x=dx/length; *y=dy/length; return true;
}
/* Default miter join, miterLimit=10; overflow/too-sharp joins bevel as required.
 * This same outline is rasterized and queried, never a hit-test-only join. */
static unsigned join_outline(const double *a,const double *b,const double *c,double half,double *out) {
    double ax,ay,bx,by;
    if (!stroke_direction(a,b,&ax,&ay) || !stroke_direction(b,c,&bx,&by)) return 0;
    double cross=ax*by-ay*bx,dot=ax*bx+ay*by;
    if (!cross) return 0;
    double side=cross>0?1:-1,nx=side*ay,ny=-side*ax,mx=side*by,my=-side*bx;
    out[0]=b[0];out[1]=b[1];out[2]=b[0]+half*nx;out[3]=b[1]+half*ny;
    unsigned count=3;
    double denominator=1+dot,ratio=denominator>0?sqrt(2/denominator):INFINITY;
    if (ratio<=10) {
        out[4]=b[0]+half*(nx+mx)/denominator;out[5]=b[1]+half*(ny+my)/denominator; count=4;
    }
    out[(count-1)*2]=b[0]+half*mx;out[(count-1)*2+1]=b[1]+half*my;
    for (unsigned i=0;i<count*2;i++) if (!isfinite(out[i])) return 0;
    return count;
}
static bool stroke_hit(const double *points,size_t count,double x,double y,double width) {
    if (!isfinite(width) || width<=0) return false;
    size_t first=0;
    while (first<count) {
        while(first<count && (!isfinite(points[first*2]) || !isfinite(points[first*2+1])))first++;
        size_t end=first;while(end<count && isfinite(points[end*2]) && isfinite(points[end*2+1]))end++;
        size_t previous=first;
        for(size_t i=first+1;i<end;i++) {
            const double *a=points+previous*2,*b=points+i*2;
            if (a[0]==b[0] && a[1]==b[1])continue;
            if(segment_hit(a[0],a[1],b[0],b[1],width/2,x,y))return true;
            size_t next=i+1;while(next<end && points[next*2]==b[0] && points[next*2+1]==b[1])next++;
            if(next<end){double outline[8];unsigned n=join_outline(a,b,points+next*2,width/2,outline);
                if(n && path_hit(outline,n,x,y,false))return true;}
            previous=i;
        }
        if(end<count && points[end*2+1]==INFINITY && end-first>2) {
            size_t before=end-2;while(before>first && points[before*2]==points[first*2] && points[before*2+1]==points[first*2+1])before--;
            size_t after=first+1;while(after<end && points[after*2]==points[first*2] && points[after*2+1]==points[first*2+1])after++;
            if(after<end){double outline[8];unsigned n=join_outline(points+before*2,points+first*2,points+after*2,width/2,outline);
                if(n && path_hit(outline,n,x,y,false))return true;}
        }
        first=end+1;
    }
    return false;
}
static bool stroke_inverse(const double *matrix,double x,double y,double *out) {
    double scale=fmax(fmax(fabs(matrix[0]),fabs(matrix[1])),fmax(fabs(matrix[2]),fabs(matrix[3])));
    if (!(scale>0) || !isfinite(scale)) return false;
    double a=matrix[0]/scale,b=matrix[1]/scale,c=matrix[2]/scale,d=matrix[3]/scale,det=a*d-b*c;
    if (!det || !isfinite(det)) return false;
    double dx=x/scale-matrix[4]/scale,dy=y/scale-matrix[5]/scale;
    out[0]=(d*dx-c*dy)/det;out[1]=(a*dy-b*dx)/det;
    return isfinite(out[0]) && isfinite(out[1]);
}
static bool segment_outline(const double *a,const double *b,double half,double *out) {
    double dx,dy;if(!stroke_direction(a,b,&dx,&dy))return false;
    double nx=dy*half,ny=-dx*half;
    out[0]=a[0]+nx;out[1]=a[1]+ny;out[2]=b[0]+nx;out[3]=b[1]+ny;
    out[4]=b[0]-nx;out[5]=b[1]-ny;out[6]=a[0]-nx;out[7]=a[1]-ny;
    for(unsigned i=0;i<8;i++)if(!isfinite(out[i]))return false;
    return true;
}
static bool stroke_shape(struct web_canvas *c,const double *matrix,const double *outline,unsigned count,uint8_t *mask) {
    double transformed[8];
    for(unsigned i=0;i<count;i++){
        double x=outline[i*2],y=outline[i*2+1];
        transformed[i*2]=matrix[0]*x+matrix[2]*y+matrix[4];
        transformed[i*2+1]=matrix[1]*x+matrix[3]*y+matrix[5];
        if(!isfinite(transformed[i*2])||!isfinite(transformed[i*2+1]))return false;
    }
    double ax=transformed[0],ay=transformed[1],bx=ax,by=ay;
    for(unsigned i=1;i<count;i++){ax=fmin(ax,transformed[i*2]);ay=fmin(ay,transformed[i*2+1]);bx=fmax(bx,transformed[i*2]);by=fmax(by,transformed[i*2+1]);}
    unsigned x0=(unsigned)fmax(0,fmin(c->w,floor(ax))),y0=(unsigned)fmax(0,fmin(c->h,floor(ay)));
    unsigned x1=(unsigned)fmax(0,fmin(c->w,ceil(bx))),y1=(unsigned)fmax(0,fmin(c->h,ceil(by)));
    for(unsigned y=y0;y<y1;y++)for(unsigned x=x0;x<x1;x++)
        if(visible(c,x,y)&&path_hit(transformed,count,x+0.5,y+0.5,false))mask[(size_t)y*c->w+x]=1;
    return true;
}
static bool stroke_mask(struct web_canvas *c,const double *points,size_t count,double width,const double *matrix,uint8_t *mask) {
    size_t first=0;
    while(first<count){
        while(first<count&&(!isfinite(points[first*2])||!isfinite(points[first*2+1])))first++;
        size_t end=first;while(end<count&&isfinite(points[end*2])&&isfinite(points[end*2+1]))end++;
        size_t previous=first;
        for(size_t i=first+1;i<end;i++){
            const double *a=points+previous*2,*b=points+i*2;if(a[0]==b[0]&&a[1]==b[1])continue;
            double outline[8];
            if(!segment_outline(a,b,width/2,outline)||!stroke_shape(c,matrix,outline,4,mask))return false;
            size_t next=i+1;while(next<end&&points[next*2]==b[0]&&points[next*2+1]==b[1])next++;
            if(next<end){unsigned n=join_outline(a,b,points+next*2,width/2,outline);if(n&&!stroke_shape(c,matrix,outline,n,mask))return false;}
            previous=i;
        }
        if(end<count&&points[end*2+1]==INFINITY&&end-first>2){
            size_t before=end-2;while(before>first&&points[before*2]==points[first*2]&&points[before*2+1]==points[first*2+1])before--;
            size_t after=first+1;while(after<end&&points[after*2]==points[first*2]&&points[after*2+1]==points[first*2+1])after++;
            if(after<end){double outline[8];unsigned n=join_outline(points+before*2,points+first*2,points+after*2,width/2,outline);
                if(n&&!stroke_shape(c,matrix,outline,n,mask))return false;}
        }
        first=end+1;
    }
    return true;
}
/* Bounded heap sort: long chart paths must not consume the native stack or
 * acquire quadratic insertion-sort cost on adversarial intersections. */
static void intersection_sort(struct intersection *v,size_t n) {
    if(n<2)return;
    for(size_t start=n/2;start;){size_t root=--start;
        for(;;){size_t child=root*2+1;if(child>=n)break;
            if(child+1<n&&v[child].x<v[child+1].x)child++;
            if(v[root].x>=v[child].x)break;
            struct intersection t=v[root];v[root]=v[child];v[child]=t;root=child;}}
    for(size_t end=n-1;end;end--){struct intersection t=v[0];v[0]=v[end];v[end]=t;size_t root=0;
        for(;;){size_t child=root*2+1;if(child>=end)break;
            if(child+1<end&&v[child].x<v[child+1].x)child++;
            if(v[root].x>=v[child].x)break;
            t=v[root];v[root]=v[child];v[child]=t;root=child;}}
}
static void polygon(struct web_canvas *c,const double *points,size_t count,uint32_t color,bool evenodd,bool clear,uint8_t *mask,struct intersection *hits,const struct canvas_paint *paint) {
    if ((!c->pixels && !mask) || count<3) return;
    for (unsigned y=0;y<c->h;y++) {
        if(!paint_checkpoint(paint,y))break;
        size_t n=0; double yy=y+0.5;
        size_t first=0;
        while (first<count) {
            while (first<count && (!isfinite(points[first*2]) || !isfinite(points[first*2+1]))) first++;
            size_t end=first;
            while (end<count && isfinite(points[end*2]) && isfinite(points[end*2+1])) end++;
            for (size_t i=first;i<end;i++) {
                size_t j=i+1<end?i+1:first;
                double x0=points[i*2],y0=points[i*2+1],x1=points[j*2],y1=points[j*2+1];
                double hit;
                if (edge_intersection(x0,y0,x1,y1,yy,&hit))
                    hits[n++]=(struct intersection){hit,y1>y0?1:-1};
            }
            first=end+1;
        }
        intersection_sort(hits,n);
        int winding=0;
        for (size_t i=0;i+1<n;i++) {
            winding=evenodd?(winding^1):winding+hits[i].direction;
            if (!winding) continue;
            double from=fmax(0,fmin(c->w,hits[i].x)),to=fmax(0,fmin(c->w,hits[i+1].x));
            int x0=(int)ceil(from-0.5),x1=(int)ceil(to-0.5);
            for (int x=x0;x<x1;x++) {
                if (!visible(c,x,y)) continue;
                if (mask) mask[(size_t)y*c->w+x]=1;
                else { uint32_t *p=&c->pixels[(size_t)y*c->w+x]; *p=clear?0:over(*p,paint_color(paint,color,x+0.5,y+0.5)); }
            }
        }
    }
}
static void canvas_buffer_free(JSRuntime *rt,void *opaque,void *bytes) {
    (void)opaque;js_free_rt(rt,bytes);
}
/* Canvas uses straight alpha, whereas the existing UI font renderer targets
 * opaque framebuffers. Render a WHITE glyph into a BLACK coverage bitmap, then
 * use its intensity as source alpha. Never copy the UI renderer's opaque alpha. */
struct canvas_text {
    uint32_t *pixels;
    unsigned w,h,pad,baseline;
    double width,ascent,descent,left,right,top,bottom;
    size_t bytes;
    bool ink;
};
static void text_free(node_t *n,struct canvas_text *t) {
    web_doc *d=allocation_doc(n);
    free(t->pixels);
    if (d && d->canvas_bytes>=t->bytes) d->canvas_bytes-=t->bytes;
    t->pixels=NULL; t->bytes=0;
}
static int text_raster(JSContext *ctx,node_t *n,const char *text,size_t length,double px,unsigned style,unsigned family,struct canvas_text *t) {
    /* The existing font renderer stores quarter-pixel size in uint16_t. */
    if (!isfinite(px) || px<0 || px>(UINT16_MAX-0.5)/4 || style>3 || family>FONT_FAMILY_MONO) {
        JS_ThrowRangeError(ctx,"Canvas font representation"); return -1;
    }
    wfont f={.ttf=font_family(family,style),.px=(float)px,.bold=(style&FONT_BOLD)!=0};
    float ascent,descent;
    t->width=wf_width(&f,text,length); wf_metrics(&f,&ascent,&descent);
    t->ascent=ascent; t->descent=descent;
    if (!length || !px) return 0;
    /* Extra bearings cover italic and fallback glyphs without clipping to the
     * advance width. Scratch follows actual allocation and gfx int geometry. */
    t->pad=(unsigned)ceil(px)+2;
    double w=ceil(t->width)+2*t->pad,h=ceil(ascent+descent)+2*t->pad;
    size_t bytes;
    if (!isfinite(w) || !isfinite(h) || w<1 || h<1 || w>INT32_MAX || h>INT32_MAX || !canvas_extent((size_t)w,(size_t)h,4,&bytes)) {
        JS_ThrowRangeError(ctx,"Canvas text bitmap representation"); return -1;
    }
    t->w=(unsigned)w; t->h=(unsigned)h; t->baseline=t->pad+(unsigned)ceil(ascent);
    web_doc *d=allocation_doc(n);
    if (!canvas_room(d,bytes)) {
        JS_ThrowRangeError(ctx,"Canvas text accounting overflow"); return -1;
    }
    t->pixels=calloc(1,bytes);
    if (!t->pixels) { JS_ThrowOutOfMemory(ctx); return -1; }
    t->bytes=bytes; d->canvas_bytes+=bytes;
    canvas_t mask; gfx_init(&mask,t->pixels,t->w,t->h,t->w);
    gfx_fill(&mask,0,0,t->w,t->h,0xff000000);
    wf_draw(&mask,&f,t->pad,t->baseline,text,length,0xffffffff);
    unsigned x0=t->w,y0=t->h,x1=0,y1=0;
    for (unsigned y=0;y<t->h;y++) for (unsigned x=0;x<t->w;x++)
        if (t->pixels[(size_t)y*t->w+x]&0xff) { x0=MIN(x0,x); y0=MIN(y0,y); x1=MAX(x1,x+1); y1=MAX(y1,y+1); }
    if (x0<x1 && y0<y1) { t->ink=true; t->left=(double)t->pad-x0; t->right=(double)x1-t->pad;
        t->top=(double)t->baseline-y0; t->bottom=(double)y1-t->baseline; }
    return 0;
}
static double baseline_offset(const struct canvas_text *t,unsigned baseline) {
    /* The bundled face exposes alphabetic ascent/descent, not separate
     * OpenType hanging/ideographic metrics. Those two use bounded fallbacks. */
    if (baseline==1) return t->ascent; /* top */
    if (baseline==2) return t->ascent*0.8; /* hanging */
    if (baseline==3) return (t->ascent-t->descent)/2; /* middle */
    if (baseline==4 || baseline==5) return -t->descent; /* ideographic/bottom */
    return 0;
}
static void text_draw(struct web_canvas *c,const struct canvas_text *t,const double *v,uint32_t color,double align,unsigned baseline,const struct canvas_paint *paint) {
    if (!c->pixels || !t->pixels || !(v[2]>0)) return;
    double shrink=t->width>v[2]?v[2]/t->width:1;
    const double *m=v+3;
    double x=v[0]-align*t->width*shrink-t->pad*shrink;
    double y=v[1]+baseline_offset(t,baseline)-t->baseline;
    double ox=m[0]*x+m[2]*y+m[4],oy=m[1]*x+m[3]*y+m[5];
    double ux=m[0]*t->w*shrink,uy=m[1]*t->w*shrink,vx=m[2]*t->h,vy=m[3]*t->h;
    double xs[4]={ox,ox+ux,ox+vx,ox+ux+vx},ys[4]={oy,oy+uy,oy+vy,oy+uy+vy};
    double minx=xs[0],maxx=xs[0],miny=ys[0],maxy=ys[0];
    for (unsigned i=0;i<4;i++) {
        if (!isfinite(xs[i]) || !isfinite(ys[i])) return;
        minx=fmin(minx,xs[i]); maxx=fmax(maxx,xs[i]); miny=fmin(miny,ys[i]); maxy=fmax(maxy,ys[i]);
    }
    double scale=fmax(fmax(fabs(ux),fabs(uy)),fmax(fabs(vx),fabs(vy)));
    if (!(scale>0) || !isfinite(scale)) return;
    double a=ux/scale,b=uy/scale,e=vx/scale,f=vy/scale,det=a*f-b*e;
    if (!det || !isfinite(det)) return;
    int x0=(int)floor(fmax(0,fmin(c->w,minx))),x1=(int)ceil(fmax(0,fmin(c->w,maxx)));
    int y0=(int)floor(fmax(0,fmin(c->h,miny))),y1=(int)ceil(fmax(0,fmin(c->h,maxy)));
    for (int yy=y0;yy<y1;yy++) { if(!paint_checkpoint(paint,(unsigned)yy))break;
      for (int xx=x0;xx<x1;xx++) {
        if (!visible(c,xx,yy)) continue;
        double dx=(xx+0.5)/scale-ox/scale,dy=(yy+0.5)/scale-oy/scale;
        double u=(dx*f-dy*e)/det,vv=(dy*a-dx*b)/det;
        if (!isfinite(u) || !isfinite(vv) || u<0 || u>=1 || vv<0 || vv>=1) continue;
        /* All scratch pixels (including black background) are opaque, so the
         * interpolated intensity is the glyph's coverage, never its UI alpha. */
        uint32_t sample=image_sample(t->pixels,t->w,t->h,u*t->w,vv*t->h,true);
        uint32_t source=paint_color(paint,color,xx+0.5,yy+0.5);
        unsigned alpha=((sample&255)*(source>>24)+127)/255;
        uint32_t *p=&c->pixels[(size_t)yy*c->w+xx]; *p=over(*p,(source&0xffffff)|(alpha<<24));
    }}
}
static JSValue text_native(JSContext *ctx,node_t *n,struct web_canvas *c,const char *op,int argc,JSValueConst *argv) {
    if (argc<7) return JS_ThrowTypeError(ctx,"Invalid Canvas text packet");
    size_t length; const char *text=JS_ToCStringLen(ctx,&length,argv[1]);
    if (!text) return JS_EXCEPTION;
    double size,align; unsigned style,baseline,family; JSValue out=JS_UNDEFINED;
    if (JS_ToFloat64(ctx,&size,argv[2])<0 || JS_ToUint32(ctx,&style,argv[3])<0 ||
        JS_ToFloat64(ctx,&align,argv[4])<0 || JS_ToUint32(ctx,&baseline,argv[5])<0 ||
        JS_ToUint32(ctx,&family,argv[6])<0) { out=JS_EXCEPTION; goto done; }
    if (!isfinite(align) || align<0 || align>1 || baseline>5) { out=JS_ThrowTypeError(ctx,"Invalid Canvas text state"); goto done; }
    struct canvas_text t={0};
    if (text_raster(ctx,n,text,length,size,style,family,&t)<0) { out=JS_EXCEPTION; goto done; }
    if (!strcmp(op,"measureText")) {
        double shift=baseline_offset(&t,baseline);
        double metrics[7]={t.width,t.left+align*t.width,t.right-align*t.width,t.top-shift,t.bottom+shift,t.ascent-shift,t.descent+shift};
        if (!t.ink) for (unsigned i=1;i<5;i++) metrics[i]=0;
        out=JS_NewArray(ctx);
        if (!JS_IsException(out)) for (unsigned i=0;i<7;i++)
            if (JS_SetPropertyUint32(ctx,out,i,JS_NewFloat64(ctx,metrics[i]))<0) { JS_FreeValue(ctx,out); out=JS_EXCEPTION; break; }
    } else {
        size_t bytes=0; uint8_t *data=argc>7?JS_GetArrayBuffer(ctx,&bytes,argv[7]):NULL;
        uint32_t color;
        if (!data || bytes!=9*sizeof(double) || argc<9) out=JS_ThrowTypeError(ctx,"Invalid Canvas text transform");
        else if (JS_ToUint32(ctx,&color,argv[8])<0) out=JS_EXCEPTION;
        else {
            double values[9]; memcpy(values,data,sizeof values);
            bool valid=true; for (unsigned i=0;i<9;i++) if (i!=2 && !isfinite(values[i])) valid=false;
            struct canvas_paint paint;
            if(paint_packet(ctx,n,argc,argv,9,&paint)<0)out=JS_EXCEPTION;
            else if (valid && values[2]>0) { text_draw(c,&t,values,color,align,baseline,&paint); changed(n); }
        }
    }
    text_free(n,&t);
done:
    JS_FreeCString(ctx,text); return out;
}
static JSValue canvas_font(JSContext *ctx,node_t *n,JSValueConst value) {
    size_t length; const char *text=JS_ToCStringLen(ctx,&length,value);
    if (!text) return JS_EXCEPTION;
    JSValue out=JS_NULL;
    const char *trimmed=text; size_t trimmed_length=length;
    while (trimmed_length && strchr(" \t\r\n\f",*trimmed)) { trimmed++; trimmed_length--; }
    while (trimmed_length && strchr(" \t\r\n\f",trimmed[trimmed_length-1])) trimmed_length--;
    /* Canvas font accepts a font shorthand, not CSS-wide keywords. The shared
     * stylesheet parser otherwise accepts inherit/initial for every property. */
    static const char *const wide[]={"inherit","initial","unset","revert","revert-layer"};
    for (unsigned i=0;i<sizeof wide/sizeof *wide;i++)
        if (trimmed_length==strlen(wide[i]) && !strncasecmp(trimmed,wide[i],trimmed_length)) goto done;
    const struct propdef *font=css_prop_lookup("font",4);
    if (css_value_supported(font,text,length)) {
        arena_t *arena=calloc(1,sizeof *arena);
        if (!arena) out=JS_ThrowOutOfMemory(ctx);
        else {
            jmp_buf trap; arena->trap=&trap;
            if (!setjmp(trap)) {
                style_t parent,st; css_style_init(&parent,NULL);
                if (n->style) parent=*n->style;
                css_style_init(&st,&parent);
                uint8_t *set=ar_alloc(arena,css_prop_count());
                struct cx cx={.s=&st,.parent=&parent,.em=parent.font_size,.rem=16,.vw=800,.vh=600,.a=arena,.set=set};
                if (css_apply(font,text,length,&cx) && !cx.invalid && isfinite(st.font_size) && st.font_size>=0) {
                    out=JS_NewArray(ctx);
                    if (!JS_IsException(out)) {
                        unsigned style=(st.font_weight>=600?FONT_BOLD:0)|(st.font_style?FONT_ITALIC:0);
                        if (JS_SetPropertyUint32(ctx,out,0,JS_NewFloat64(ctx,st.font_size))<0 ||
                            JS_SetPropertyUint32(ctx,out,1,JS_NewUint32(ctx,style))<0 ||
                            JS_SetPropertyUint32(ctx,out,2,JS_NewUint32(ctx,st.font_family))<0) { JS_FreeValue(ctx,out); out=JS_EXCEPTION; }
                    }
                }
            }
            else out=JS_ThrowOutOfMemory(ctx);
            ar_free(arena); free(arena);
        }
    }
done:
    JS_FreeCString(ctx,text); return out;
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
    else if (!strcmp(op,"color") || !strcmp(op,"gradientColor")) {
        size_t length=0;const char *text=argc>1?JS_ToCStringLen(ctx,&length,argv[1]):NULL; uint32_t color;
        if (argc>1 && !text) { out=JS_EXCEPTION; goto done; }
        bool gradient=!strcmp(op,"gradientColor");
        bool valid=text && (!gradient||css_supports_declaration("color",5,text,length)) && css_color(text,length,&color);
        /* Canvas-neutral gradient colors resolve currentColor to the initial
           CSS color, never the canvas element that happened to create it. */
        if(valid && gradient && color==COLOR_CURRENT)color=0xff000000u;
        out=valid?JS_NewUint32(ctx,color):JS_NULL;
        JS_FreeCString(ctx,text);
    } else if (!strcmp(op,"font")) out=argc>1?canvas_font(ctx,n,argv[1]):JS_NULL;
    else if (!strcmp(op,"rtl")) {
        out=JS_FALSE;
        for (node_t *p=n;p;p=p->parent) {
            const char *direction=node_attr(p,"dir");
            if (direction && !strcasecmp(direction,"rtl")) { out=JS_TRUE; break; }
            if (direction && !strcasecmp(direction,"ltr")) break;
        }
    } else if (!strcmp(op,"reset")) { web_canvas_attr_changed(n,"width"); changed(n); }
    else if (!strcmp(op,"hitPath") || !strcmp(op,"hitStroke")) {
        /* Pure geometry: no bitmap allocation, taint read, clip sampling,
           painting or dirty/generation mutation. Queries may be off-canvas. */
        size_t bytes=0; uint8_t *data=argc>1?JS_GetArrayBuffer(ctx,&bytes,argv[1]):NULL;
        if (!data || bytes%16) { out=JS_ThrowTypeError(ctx,"Invalid Canvas path packet"); goto done; }
        double query[2]; int numbers=numeric(ctx,argc,argv,2,query,2);
        if (numbers<0) { out=JS_EXCEPTION; goto done; }
        if (!numbers) { out=JS_FALSE; goto done; }
        double *points=bytes?js_malloc(ctx,bytes):NULL;
        if (bytes && !points) { out=JS_ThrowOutOfMemory(ctx); goto done; }
        if (bytes) memcpy(points,data,bytes);
        bool hit=false;
        if (!strcmp(op,"hitPath")) hit=path_hit(points,bytes/16,query[0],query[1],argc>4&&JS_ToBool(ctx,argv[4]));
        else {
            double width=1;
            if (argc>4 && JS_ToFloat64(ctx,&width,argv[4])<0) { js_free(ctx,points); out=JS_EXCEPTION; goto done; }
            double matrix[6]={1,0,0,1,0,0},local[2];
            size_t matrix_bytes=0;uint8_t *matrix_data=argc>5?JS_GetArrayBuffer(ctx,&matrix_bytes,argv[5]):NULL;
            if(argc>5 && (!matrix_data||matrix_bytes!=sizeof matrix)){js_free(ctx,points);out=JS_ThrowTypeError(ctx,"Invalid Canvas stroke transform");goto done;}
            if(matrix_data)memcpy(matrix,matrix_data,sizeof matrix);
            if(stroke_inverse(matrix,query[0],query[1],local))hit=stroke_hit(points,bytes/16,local[0],local[1],width);
        }
        js_free(ctx,points); out=JS_NewBool(ctx,hit);
    } else {
        struct web_canvas *c=ensure(n);
        if (!strcmp(op,"png") || !strcmp(op,"dataURL")) { out=canvas_export(ctx,n,c,!strcmp(op,"dataURL")); goto done; }
        if (!c) { out=!strcmp(op,"context")?JS_FALSE:JS_ThrowOutOfMemory(ctx); goto done; }
        if (!strcmp(op,"context")) out=JS_TRUE;
        else if (!strcmp(op,"version")) out=JS_NewUint32(ctx,c->generation);
        else if (!strcmp(op,"save")) {
            if (!canvas_save(n,c)) { out=JS_ThrowOutOfMemory(ctx); goto done; }
        } else if (!strcmp(op,"restore")) {
            if (c->saved_count) { clip_release(n,c->clip); c->clip=c->saved_clip[--c->saved_count]; c->saved_clip[c->saved_count]=NULL; }
        } else if (!strcmp(op,"measureText") || !strcmp(op,"fillText")) out=text_native(ctx,n,c,op,argc,argv);
        else if (!strcmp(op,"rect") || !strcmp(op,"clear")) {
            double v[4]; uint32_t color=0;
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            if (numbers) {
                if (argc>5 && JS_ToUint32(ctx,&color,argv[5])<0) { out=JS_EXCEPTION; goto done; }
                struct canvas_paint paint;
                if(paint_packet(ctx,n,argc,argv,6,&paint)<0){out=JS_EXCEPTION;goto done;}
                rectangle(c,v,color,!strcmp(op,"clear"),&paint); changed(n);
            }
        } else if (!strcmp(op,"poly") || !strcmp(op,"stroke") || !strcmp(op,"clip")) {
            size_t bytes; uint8_t *data=argc>1?JS_GetArrayBuffer(ctx,&bytes,argv[1]):NULL;
            uint32_t color=0; double width=1,matrix[6]={1,0,0,1,0,0}; bool evenodd=false,clear=false;
            if (!data || bytes%16) { out=JS_ThrowTypeError(ctx,"Invalid Canvas path packet"); goto done; }
            if (argc>2 && JS_ToUint32(ctx,&color,argv[2])<0) { out=JS_EXCEPTION; goto done; }
            if (argc>3) {
                if (!strcmp(op,"stroke")) { if (JS_ToFloat64(ctx,&width,argv[3])<0) { out=JS_EXCEPTION; goto done; } }
                else evenodd=JS_ToBool(ctx,argv[3]);
            }
            if (argc>4) {
                if(!strcmp(op,"stroke")){
                    size_t matrix_bytes=0;uint8_t *matrix_data=JS_GetArrayBuffer(ctx,&matrix_bytes,argv[4]);
                    if(!matrix_data||matrix_bytes!=sizeof matrix){out=JS_ThrowTypeError(ctx,"Invalid Canvas stroke transform");goto done;}
                    memcpy(matrix,matrix_data,sizeof matrix);
                }else clear=JS_ToBool(ctx,argv[4]);
            }
            struct canvas_paint paint;
            if(paint_packet(ctx,n,argc,argv,5,&paint)<0){out=JS_EXCEPTION;goto done;}
            size_t count=bytes/16;
            bool stroke=!strcmp(op,"stroke");
            if (!stroke && count>SIZE_MAX/sizeof(struct intersection)) { out=JS_ThrowRangeError(ctx,"Canvas path size overflow");goto done; }
            double *points=bytes?js_malloc(ctx,bytes):NULL;
            struct intersection *hits=!stroke&&count?js_malloc(ctx,count*sizeof *hits):NULL;
            if((bytes&&!points)||(!stroke&&count&&!hits)){js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowOutOfMemory(ctx);goto done;}
            if(bytes)memcpy(points,data,bytes);
            if (!strcmp(op,"clip")) {
                size_t bytes=(size_t)c->w*c->h;
                web_doc *d=allocation_doc(n);
                if (bytes>SIZE_MAX-sizeof(struct canvas_clip) || !canvas_room(d,bytes+sizeof(struct canvas_clip))) { js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowRangeError(ctx,"Canvas clip accounting overflow"); goto done; }
                bytes+=sizeof(struct canvas_clip);
                struct canvas_clip *clip=calloc(1,bytes);
                if (!clip) { js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowOutOfMemory(ctx); goto done; }
                clip->refs=1; clip->bytes=bytes; d->canvas_bytes+=bytes;
                /* polygon samples the old clip as well: intersection, not
                 * replacement. Even an empty path produces an empty clip. */
                polygon(c,points,count,0,evenodd,false,clip->pixels,hits,NULL);
                clip_release(n,c->clip); c->clip=clip;
            } else if (!strcmp(op,"poly")) polygon(c,points,count,color,evenodd,clear,NULL,hits,&paint);
            else if (c->pixels && isfinite(width) && width>0) {
                /* Union the complete stroke before compositing: overlapping
                   segments/joins must apply globalAlpha exactly once. */
                size_t mask_bytes=(size_t)c->w*c->h;web_doc *d=allocation_doc(n);
                if(!canvas_room(d,mask_bytes)){js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowRangeError(ctx,"Canvas stroke accounting overflow");goto done;}
                uint8_t *mask=calloc(1,mask_bytes);
                if(!mask){js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowOutOfMemory(ctx);goto done;}
                d->canvas_bytes+=mask_bytes;
                bool complete=stroke_mask(c,points,count,width,matrix,mask);
                if(complete)for(unsigned y=0;y<c->h;y++){
                    if(!paint_checkpoint(&paint,y))break;
                    for(unsigned x=0;x<c->w;x++){size_t i=(size_t)y*c->w+x;
                        if(mask[i])c->pixels[i]=over(c->pixels[i],paint_color(&paint,color,x+0.5,y+0.5));}}
                free(mask);d->canvas_bytes-=mask_bytes;
                if(!complete){js_free(ctx,points);js_free(ctx,hits);out=JS_ThrowRangeError(ctx,"Canvas stroke coordinate representation");goto done;}
            }
            js_free(ctx,points);js_free(ctx,hits);
            changed(n);
        } else if (!strcmp(op,"read")) {
            double v[4];
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            size_t bytes;
            /* JS ArrayBuffer length is INT32_MAX in the current engine. */
            if (!numbers || v[2]<1 || v[3]<1 || v[2]>INT32_MAX || v[3]>INT32_MAX || v[0]<INT32_MIN || v[0]>INT32_MAX || v[1]<INT32_MIN || v[1]>INT32_MAX ||
                !canvas_extent((size_t)v[2],(size_t)v[3],4,&bytes) || bytes>INT32_MAX) {
                out=JS_ThrowRangeError(ctx,"Invalid ImageData size"); goto done;
            }
            if (c->tainted) { out=JS_NULL; goto done; }
            int x=(int)v[0],y=(int)v[1]; unsigned w=(unsigned)v[2],h=(unsigned)v[3];
            web_js_prepare_bytes(ctx,bytes);
            uint8_t *data=js_mallocz(ctx,bytes);
            if (!data) { out=JS_ThrowOutOfMemory(ctx); goto done; }
            for (unsigned j=0;j<h;j++) for (unsigned i=0;i<w;i++) {
                int64_t xx=(int64_t)x+i,yy=(int64_t)y+j;
                if (!c->pixels || xx<0 || yy<0 || xx>=c->w || yy>=c->h) continue;
                uint32_t p=c->pixels[(size_t)yy*c->w+(size_t)xx]; size_t at=((size_t)j*w+i)*4;
                data[at]=(p>>16)&255; data[at+1]=(p>>8)&255; data[at+2]=p&255; data[at+3]=p>>24;
            }
            out=JS_NewArrayBuffer(ctx,data,bytes,canvas_buffer_free,NULL,false);
            if(JS_IsException(out))js_free(ctx,data);
        } else if (!strcmp(op,"put")) {
            double v[4]; size_t bytes=0;
            int numbers=numeric(ctx,argc,argv,1,v,4);
            if (numbers<0) { out=JS_EXCEPTION; goto done; }
            uint8_t *data=argc>5?JS_GetArrayBuffer(ctx,&bytes,argv[5]):NULL;
            uint32_t offset=0;
            if(argc>6&&JS_ToUint32(ctx,&offset,argv[6])<0){out=JS_EXCEPTION;goto done;}
            size_t required;
            if (!numbers || v[2]<1 || v[3]<1 || v[2]>INT32_MAX || v[3]>INT32_MAX || v[0]<INT32_MIN || v[0]>INT32_MAX || v[1]<INT32_MIN || v[1]>INT32_MAX ||
                !canvas_extent((size_t)v[2],(size_t)v[3],4,&required) || !data || offset>bytes || bytes-offset<required) {
                out=JS_ThrowRangeError(ctx,"Invalid ImageData buffer"); goto done;
            }
            data+=offset;
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
