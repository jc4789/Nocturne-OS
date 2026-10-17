/* Painting, hit testing and find in page.

   Paint order (a simplification of CSS 2 appendix E): within each supported stacking context,
   its background; negative z-index layers; normal flow; then nonnegative layers. Descendants
   of an explicit z-index context must not escape into a global z-index sort.
   Hit testing walks the same order and keeps the last thing under the point. */
#include <stdio.h>
#include <math.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include <limits.h>
#include <float.h>
#include "frame.h"
#include "web_dialog.h"
#include "js_canvas.h"
#include "avmedia.h"
#include "elements.h"
#include "form_value.h"
#include "form_validation.h"

enum { M_PAINT, M_HIT, M_BOUNDS };

/* Exact hit primitives are collected through the same walk, clips and stable
 * stacking order as painting. A BVH only selects the last primitive; default
 * actions and dynamic link resolution still use the ordinary native path. */
struct hit_primitive {
    float rect[4];
    node_t *target, *node;
    size_t order;
    int kind;
    box_t *box;
    const struct radius_cache *radius;
    float radius_x,radius_y;
};
struct hit_branch {
    float rect[4];
    size_t first, count, left, right, last_order;
};
struct web_hit_index {
    struct hit_primitive *items;
    struct hit_branch *branches;
    size_t count, capacity, branch_count, branch_capacity;
    /* Transformed subtrees are not indexed. A point inside one of their
       document-space bounds takes the exact inverse-mapped walk instead. */
    float (*regions)[4];
    size_t region_count, region_capacity;
    uint64_t layout, dom, visual, fonts;
    int view_x, view_y;
    bool valid;
};

struct pctx {
    web_doc *d;
    canvas_t *c;
    int vx0, vy0, vx1, vy1; /* viewport clip, before any element overflow */
    float ox, oy; /* canvas position of document (0, 0) */
    int mode;
    float hx, hy; /* hit testing: the point, in document coordinates */
    struct web_hit *hit;
    bool hit_any;
    node_t *target;
    box_t *top_root;
    uint32_t canvas_bg_from; /* 1: html's background was used for the canvas, 2: body's */
    uint32_t canvas_bg;
    bool canvas_group;
    uint64_t bounds_generation;
    unsigned bounds_work;
    float bounds[4];
    bool bounds_valid, failed;
    struct web_hit_index *index_build;
    float hit_rect[4];
    const struct radius_cache *hit_radius;
    float hit_shape_x,hit_shape_y;
    const struct gfx_mask_clip *viewport_mask;
    const box_t *untransformed; /* this box's own used transform is being applied by the caller */
};
#include "paint_media_debug.h"
/* debug_js only: where top-level paint time goes between profile reports. */
static struct { uint64_t paints, order_ms, frames_ms, walk_ms, probe_ms, total_ms, groups, group_pixels,
    order_builds, layer_builds, radius_builds, radius_pixels, radius_moves; } paint_profile;

static bool positioned(const box_t *b) {
    if (!b->st || b->kind == B_INLINE || b->kind == B_TEXT) return false;
    if (b->st->position != POS_STATIC) return b->st->position != POS_STICKY;
    /* z-index also applies to in-flow flex/grid items. Their layout remains
       static, but painting and hit testing must place them in the z order. */
    return !b->st->z_auto && b->parent && !b->abspos &&
           (b->parent->kind == B_FLEX || b->parent->kind == B_GRID);
}

static bool stacking_context(const box_t *b) {
    if (!b->st || b->kind == B_TEXT || b->kind == B_BR) return false;
    if (b->st->filter || b->st->opacity < 1 ||
        (b->kind!=B_INLINE&&css_translation_active(b->st))) return true;
    if (b->kind == B_INLINE) return false;
    /* Opacity groups are atomic even in normal flow. Fixed/sticky boxes also
       establish a context with auto z-index; their descendants cannot escape
       into the outer context's global sort after author layout hydration. */
    return b->st->opacity < 1.0f || b->st->position == POS_FIXED || b->st->position == POS_STICKY ||
           (positioned(b) && !b->st->z_auto);
}
static bool paint_box_layer(const box_t *b) { return positioned(b) || stacking_context(b); }

static float cy(const box_t *b) { return box_visual_y(b) + b->content_dy; }

static image_t *scaled_image(struct web_image *im, int w, int h,
                             const canvas_t *canvas, int x, int y, int box_w, int box_h);

/* ---------------------------------------------------------------- rounded rectangles */
static float sdf_rrect(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cyy = y + h / 2;
    float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cyy) - (h / 2 - r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return sqrtf(mx * mx + my * my) + in - r;
}

static float coverage(float d) {
    float c = 0.5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

static uint32_t with_alpha(uint32_t col, float a) {
    int al = (int)((float)(col >> 24) * a + 0.5f);
    if (al <= 0) return 0;
    return ((uint32_t)al << 24) | (col & 0xFFFFFF);
}
struct border_radii { float x[4],y[4]; };
static void radii_normalize(struct border_radii *r,float w,float h) {
    double factor=1;
    for(int side=0;side<4;side++) {
        double sum=side==0?r->x[0]+(double)r->x[1]:side==1?r->y[1]+(double)r->y[2]:
                   side==2?r->x[2]+(double)r->x[3]:r->y[3]+(double)r->y[0];
        double extent=(side&1)?h:w;if(sum>extent&&sum>0)factor=fmin(factor,extent/sum);
    }
    for(int i=0;i<4;i++){r->x[i]*=(float)factor;r->y[i]*=(float)factor;if(r->x[i]<=0||r->y[i]<=0)r->x[i]=r->y[i]=0;}
}
static struct border_radii used_radii(const style_t *st,float w,float h) {
    struct border_radii r={0};
    bool specified=false;for(int i=0;i<4;i++)for(int axis=0;axis<2;axis++)specified|=st->corner_radius[i].axis[axis].kind!=LK_AUTO;
    for(int i=0;i<4;i++) {
        if(specified){r.x[i]=fmaxf(0,len_resolve(&st->corner_radius[i].axis[0],w));r.y[i]=fmaxf(0,len_resolve(&st->corner_radius[i].axis[1],h));}
        else if(st->border_radius>=0)r.x[i]=r.y[i]=st->border_radius;
        else{r.x[i]=-st->border_radius*w*.01f;r.y[i]=-st->border_radius*h*.01f;}
        if(!isfinite(r.x[i]))r.x[i]=FLT_MAX/8;if(!isfinite(r.y[i]))r.y[i]=FLT_MAX/8;
    }
    radii_normalize(&r,w,h);return r;
}
static struct border_radii inset_radii(struct border_radii r,const float *edges,float w,float h) {
    static const int xs[4]={3,1,1,3},ys[4]={0,0,2,2};
    for(int i=0;i<4;i++){r.x[i]=fmaxf(0,r.x[i]-edges[xs[i]]);r.y[i]=fmaxf(0,r.y[i]-edges[ys[i]]);}
    radii_normalize(&r,w,h);return r;
}
static bool radii_any(const struct border_radii *r){for(int i=0;i<4;i++)if(r->x[i]>0&&r->y[i]>0)return true;return false;}
/* Signed-distance linearization is used only at the anti-aliased elliptical
   boundary. The rectangular middle takes no divisions or square roots. */
static float corners_coverage(float px,float py,float x,float y,float w,float h,const struct border_radii *r) {
    bool circular=r->x[0]==r->y[0];
    for(int i=1;i<4&&circular;i++)circular=r->x[i]==r->x[0]&&r->y[i]==r->y[0];
    if(circular)return coverage(sdf_rrect(px,py,x,y,w,h,r->x[0])); /* preserve established circle AA exactly */
    float cover=fminf(fminf(px-x+.5f,x+w-px+.5f),fminf(py-y+.5f,y+h-py+.5f));
    if(cover<=0)return 0;cover=fminf(1,cover);
    for(int i=0;i<4;i++) {
        float rx=r->x[i],ry=r->y[i];if(rx<=0||ry<=0)continue;
        bool right=i==1||i==2,bottom=i>=2;
        float cx=right?x+w-rx:x+rx,cy=bottom?y+h-ry:y+ry;
        if((right?px<cx:px>cx)||(bottom?py<cy:py>cy))continue;
        float dx=fabsf(px-cx)/rx,dy=fabsf(py-cy)/ry;
        float norm=sqrtf(dx*dx+dy*dy);if(norm==0)continue;
        float gradient=sqrtf((dx/rx)*(dx/rx)+(dy/ry)*(dy/ry));
        if(gradient>0)cover=fminf(cover,coverage((norm-1)/gradient));
    }
    return cover;
}
struct radius_cache {
    float x,y,w,h;struct border_radii radii;
    int left,top,width,height;bool clipped;
    int view_x,view_y,view_w,view_h;
    uint8_t *alpha;
    struct gfx_mask_span *opaque;
};
void web_radius_cache_free(struct radius_cache **slot){struct radius_cache *cache=*slot;*slot=NULL;if(cache){free(cache->alpha);free(cache->opaque);free(cache);}}
static bool radius_cache_prepare(web_doc *doc,struct radius_cache **slot,float x,float y,float w,float h,struct border_radii radii) {
    if(!radii_any(&radii)||w<=0||h<=0){web_radius_cache_free(slot);return true;}
    if(!web_native_checkpoint(doc))return false; /* empty/offscreen slices still cancel before publication */
    struct radius_cache *old=*slot;
    if(old&&old->x==x&&old->y==y&&old->w==w&&old->h==h&&!memcmp(&old->radii,&radii,sizeof radii)&&
        (!old->clipped||(old->view_x==doc->view_x&&old->view_y==doc->view_y&&old->view_w==doc->width&&old->view_h==doc->height))){doc->paint_shadow_crop|=old->clipped;return true;}
    double vx1=(double)doc->view_x+doc->width,vy1=(double)doc->view_y+doc->height;
    double left=fmin(vx1,fmax(floor((double)x),doc->view_x)),top=fmin(vy1,fmax(floor((double)y),doc->view_y));
    double right=fmax(doc->view_x,fmin(ceil((double)x+w),vx1)),bottom=fmax(doc->view_y,fmin(ceil((double)y+h),vy1));
    if(!isfinite(left)||!isfinite(top)||left<INT_MIN||top<INT_MIN||left>INT_MAX||top>INT_MAX)return false;
    double width=fmax(0,right-left),height=fmax(0,bottom-top);
    if(width>INT_MAX||height>INT_MAX||(height>0&&(size_t)width>SIZE_MAX/(size_t)height))return false;
    bool clipped=left!=floor((double)x)||top!=floor((double)y)||right!=ceil((double)x+w)||bottom!=ceil((double)y+h);
    /* Coverage depends only on the pixel offset from the box, so an unclipped
       mask moved by whole pixels (content shifted by layout) is the same mask. */
    if(old&&!old->clipped&&!clipped&&old->w==w&&old->h==h&&!memcmp(&old->radii,&radii,sizeof radii)&&
       x-floorf(x)==old->x-floorf(old->x)&&y-floorf(y)==old->y-floorf(old->y)&&
       old->width==(int)width&&old->height==(int)height){
        old->x=x;old->y=y;old->left=(int)left;old->top=(int)top;
        old->view_x=doc->view_x;old->view_y=doc->view_y;old->view_w=doc->width;old->view_h=doc->height;
        if(doc->profile_enabled)paint_profile.radius_moves++;
        return true;
    }
    struct radius_cache *next=calloc(1,sizeof *next);if(!next)return false;
    next->x=x;next->y=y;next->w=w;next->h=h;next->radii=radii;next->left=(int)left;next->top=(int)top;next->width=(int)width;next->height=(int)height;
    next->clipped=clipped;
    next->view_x=doc->view_x;next->view_y=doc->view_y;next->view_w=doc->width;next->view_h=doc->height;
    size_t pixels=(size_t)next->width*next->height;
    if(pixels&&((size_t)next->height>SIZE_MAX/sizeof *next->opaque||
        !(next->alpha=malloc(pixels))||!(next->opaque=calloc((size_t)next->height,sizeof *next->opaque)))){web_radius_cache_free(&next);return false;}
    if(doc->profile_enabled){paint_profile.radius_builds++;paint_profile.radius_pixels+=pixels;}
    /* Rows clear of every corner and of the top/bottom anti-aliased edge have
       coverage that depends on x alone (both the circular SDF and the
       elliptical path): compute one such row and copy it. */
    float top_reach=fmaxf(fmaxf(radii.x[0],radii.y[0]),fmaxf(radii.x[1],radii.y[1]))+2;
    float bottom_reach=fmaxf(fmaxf(radii.x[2],radii.y[2]),fmaxf(radii.x[3],radii.y[3]))+2;
    int middle=-1;
    for(int py=0;py<next->height;py++) {
        if(!(py&31)&&!web_native_checkpoint(doc)){web_radius_cache_free(&next);return false;}
        float center=next->top+py+.5f;
        bool plain=center-y>=top_reach&&y+h-center>=bottom_reach;
        if(plain&&middle>=0&&next->alpha){
            memcpy(next->alpha+(size_t)py*next->width,next->alpha+(size_t)middle*next->width,(size_t)next->width);
            next->opaque[py]=next->opaque[middle];continue;
        }
        if(plain)middle=py;
        int first=-1,last=0;
        for(int px=0;px<next->width;px++) {
            uint8_t alpha=(uint8_t)(255*corners_coverage(next->left+px+.5f,next->top+py+.5f,x,y,w,h,&radii)+.5f);
            next->alpha[(size_t)py*next->width+px]=alpha;
            if(alpha==255){if(first<0)first=px;last=px+1;}
        }
        if(next->opaque)next->opaque[py]=(struct gfx_mask_span){first<0?0:first,last};
    }
    doc->paint_shadow_crop|=next->clipped;web_radius_cache_free(slot);*slot=next;return true;
}
static bool radius_clip_push(struct pctx *P,const struct radius_cache *cache,struct gfx_mask_clip *clip,float x,float y) {
    if(!cache)return false;
    double left=cache->left+((double)x-cache->x),top=cache->top+((double)y-cache->y);
    if(!isfinite(left)||!isfinite(top)||left<INT_MIN||top<INT_MIN||left>INT_MAX||top>INT_MAX){P->failed=true;return false;}
    *clip=(struct gfx_mask_clip){.next=P->c->mask_clip,.alpha=cache->alpha,.x=(int)lround(left),.y=(int)lround(top),.w=cache->width,.h=cache->height,.pitch=cache->width,.opaque=cache->opaque};
    P->c->mask_clip=clip;
    web_avmedia_snapshot_reject_region(P->c,clip->x,clip->y,clip->w,clip->h);
    return true;
}

static uint32_t lerp_col(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        r |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
    }
    return r;
}

/* fill a rectangle with rounded corners (radius r) with a vertical gradient top..bot */
static void fill_rrect_serial(canvas_t *c, float x, float y, float w, float h, float r, uint32_t top, uint32_t bot) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int X0 = (int)floorf(x), Y0 = (int)floorf(y), X1 = (int)ceilf(x + w), Y1 = (int)ceilf(y + h);
    if (Y0 < c->cy0) Y0 = c->cy0;
    if (Y1 > c->cy1) Y1 = c->cy1;
    if (X0 < c->cx0) X0 = c->cx0;
    if (X1 > c->cx1) X1 = c->cx1;
    if (X0 >= X1 || Y0 >= Y1) return;
    bool grad = top != bot;
    if (r < 0.5f && !grad) {
        gfx_fill_blend(c, (int)roundf(x), (int)roundf(y), (int)roundf(x + w) - (int)roundf(x),
                       (int)roundf(y + h) - (int)roundf(y), top);
        return;
    }
    for (int py = Y0; py < Y1; py++) {
        float fy = (float)py + 0.5f;
        uint32_t col = grad ? lerp_col(top, bot, (fy - y) / h) : top;
        bool corner = fy < y + r || fy > y + h - r;
        if (!corner) {
            int a = (int)roundf(x), b = (int)roundf(x + w);
            gfx_fill_blend(c, a, py, b - a, 1, col);
            continue;
        }
        for (int px = X0; px < X1; px++) {
            float fx = (float)px + 0.5f;
            if (fx > x + r && fx < x + w - r) {
                gfx_blend_pixel(c, px, py, col);
                continue;
            }
            float cov = coverage(sdf_rrect(fx, fy, x, y, w, h, r));
            if (cov > 0) gfx_blend_pixel(c, px, py, with_alpha(col, cov));
        }
    }
}

/* a border ring with rounded corners: everything between the outer and the inner rounded rect */
static void ring_rrect_serial(canvas_t *c, float x, float y, float w, float h, float r, const float *bw, const uint32_t *col) {
    float ix = x + bw[3], iy = y + bw[0], iw = w - bw[1] - bw[3], ih = h - bw[0] - bw[2];
    float maxb = fmaxf(fmaxf(bw[0], bw[1]), fmaxf(bw[2], bw[3]));
    float ir = r - maxb;
    if (ir < 0) ir = 0;
    int X0 = (int)floorf(x), Y0 = (int)floorf(y), X1 = (int)ceilf(x + w), Y1 = (int)ceilf(y + h);
    if (Y0 < c->cy0) Y0 = c->cy0;
    if (Y1 > c->cy1) Y1 = c->cy1;
    if (X0 < c->cx0) X0 = c->cx0;
    if (X1 > c->cx1) X1 = c->cx1;
    float k = fmaxf(r, maxb);
    for (int py = Y0; py < Y1; py++) {
        float fy = (float)py + 0.5f;
        bool band = fy < y + k || fy > y + h - k;
        uint32_t rowcol = fy < y + h / 2 ? col[0] : col[2];
        for (int px = X0; px < X1; px++) {
            float fx = (float)px + 0.5f;
            if (!band && fx > x + k && fx < x + w - k) {
                int skip = (int)(x + w - k) - 1;
                if (skip > px) px = skip;
                continue;
            }
            float co = coverage(sdf_rrect(fx, fy, x, y, w, h, r));
            if (co <= 0) continue;
            float ci = iw > 0 && ih > 0 ? coverage(sdf_rrect(fx, fy, ix, iy, iw, ih, ir)) : 0;
            float a = co * (1 - ci);
            if (a <= 0) continue;
            uint32_t cc = band ? rowcol : fx < x + w / 2 ? col[3] : col[1];
            gfx_blend_pixel(c, px, py, with_alpha(cc, a));
        }
    }
}

/* Shadow geometry is cached with its box, independently of computed-style
   arena addresses/colors. Gaussian construction happens before canvas writes;
   ordinary repaint only composites the retained alpha mask. */
static float used_radius(const style_t *, float, float);
struct shadow_mask {
    struct css_box_shadow value;
    uint8_t *alpha;
    int left, top, width, height;
};
struct shadow_cache {
    float width, height, fx, fy, borders[4];
    struct border_radii radii;
    float viewport[4];
    bool clipped;
    size_t count;
    struct shadow_mask masks[];
};
void web_shadow_cache_free(struct shadow_cache **slot) {
    struct shadow_cache *cache = *slot; *slot = NULL;
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++) free(cache->masks[i].alpha);
    free(cache);
}
static float shadow_spread_radius(float radius, float spread) {
    if (radius <= 0) return 0;
    if (spread > radius) {
        float ratio = 1 - radius / spread;
        return radius + spread * (1 - ratio * ratio * ratio);
    }
    return fmaxf(0, radius + spread);
}
struct shadow_blur_job {
    const uint8_t *source;
    uint8_t *target;
    const float *kernel;
    int width, height, radius;
    bool vertical;
};
static void shadow_blur_rows(size_t tile, void *opaque) {
    const struct shadow_blur_job *job = opaque;
    int y0 = (int)tile * 32, y1 = MIN(y0 + 32, job->height);
    for (int y = y0; y < y1; y++) for (int x = 0; x < job->width; x++) {
        float sum = 0; int position = job->vertical ? y : x, limit = job->vertical ? job->height : job->width;
        int first = MAX(-job->radius, -position), last = MIN(job->radius, limit - position - 1);
        for (int k = first; k <= last; k++) {
            size_t at = job->vertical ? (size_t)(y+k)*job->width+x : (size_t)y*job->width+x+k;
            sum += job->source[at] * job->kernel[k + job->radius];
        }
        job->target[(size_t)y*job->width+x] = (uint8_t)fminf(255, floorf(sum + .5f));
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)job->width * (y1-y0));
}
static void shadow_service(void *opaque) { web_native_checkpoint(opaque); }
static bool shadow_mask_build(web_doc *doc, struct shadow_cache *cache, struct shadow_mask *mask) {
    const struct css_box_shadow *shadow = &mask->value;
    if (!(shadow->color >> 24)) return true;
    float spread = shadow->inset ? -shadow->spread : shadow->spread;
    float x = cache->fx + shadow->x, y = cache->fy + shadow->y, w = cache->width, h = cache->height;
    struct border_radii radii=cache->radii;
    if (shadow->inset) {
        x += cache->borders[3]; y += cache->borders[0];
        w -= cache->borders[1] + cache->borders[3]; h -= cache->borders[0] + cache->borders[2];
        radii=inset_radii(radii,cache->borders,w,h);
    }
    x -= spread; y -= spread; w += spread*2; h += spread*2;
    for(int i=0;i<4;i++){radii.x[i]=shadow_spread_radius(radii.x[i],spread);radii.y[i]=shadow_spread_radius(radii.y[i],spread);}
    radii_normalize(&radii,w,h);
    if (w <= 0 || h <= 0) return true; /* empty outset / solid inset */
    double halo = ceil((double)shadow->blur * 1.5), left = floor(x)-halo-1, top = floor(y)-halo-1;
    double right=ceil(x+w)+halo+1,bottom=ceil(y+h)+halo+1;
    /* Only a convolution halo around the requested viewport is necessary.
       A million-pixel-tall author box does not allocate a million-pixel mask.
       When scrolling exposes a new slice the cache is rebuilt, not skipped. */
    double clipped_left=fmax(left,floor(cache->viewport[0])-halo-1),clipped_top=fmax(top,floor(cache->viewport[1])-halo-1);
    double clipped_right=fmin(right,ceil(cache->viewport[2])+halo+1),clipped_bottom=fmin(bottom,ceil(cache->viewport[3])+halo+1);
    cache->clipped|=clipped_left!=left||clipped_top!=top||clipped_right!=right||clipped_bottom!=bottom;
    left=clipped_left;top=clipped_top;right=clipped_right;bottom=clipped_bottom;
    double width=right-left,height=bottom-top;
    if(width<=0||height<=0)return true;
    if (!isfinite(width) || !isfinite(height) || left < INT_MIN || left > INT_MAX || top < INT_MIN || top > INT_MAX ||
        width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX || halo > (INT_MAX-1)/2) return false;
    mask->left = (int)left; mask->top = (int)top; mask->width = (int)width; mask->height = (int)height;
    if((size_t)mask->width>SIZE_MAX/(size_t)mask->height)return false;
    size_t pixels = (size_t)mask->width * mask->height;
    mask->alpha = calloc(pixels, 1); if (!mask->alpha) return false;
    float sx = x - mask->left, sy = y - mask->top;
    for (int py = 0; py < mask->height; py++) {
        if (!(py & 31) && !web_native_checkpoint(doc)) return false;
        float fy = py + .5f;
        if (fy < sy-.5f || fy > sy+h+.5f) continue;
        int first = (int)fmaxf(0,fminf(mask->width,floorf(sx)));
        int last = (int)fmaxf(0,fminf(mask->width,ceilf(sx+w)));
        for (int px = first; px < last; px++)
            mask->alpha[(size_t)py*mask->width+px] = (uint8_t)(corners_coverage(px+.5f,fy,sx,sy,w,h,&radii)*255+.5f);
    }
    if (shadow->blur <= 0) return true;
    int extent = (int)halo; size_t kernel_count = (size_t)extent*2+1;
    if (kernel_count > SIZE_MAX / sizeof(float)) return false;
    float *kernel = malloc(kernel_count*sizeof *kernel); uint8_t *temporary = malloc(pixels);
    if (!kernel || !temporary) { free(kernel); free(temporary); return false; }
    double sigma = shadow->blur / 2.0, inverse = -.5 / (sigma*sigma), sum = 0;
    for (int i = -extent; i <= extent; i++) { kernel[i+extent] = (float)exp((double)i*i*inverse); sum += kernel[i+extent]; }
    float normalize = (float)(1/sum); for (size_t i = 0; i < kernel_count; i++) kernel[i] *= normalize;
    struct shadow_blur_job job = {mask->alpha,temporary,kernel,mask->width,mask->height,extent,false};
    size_t rows = ((size_t)mask->height+31)/32;
    parallel_for_stage(PARALLEL_RASTER, rows, shadow_blur_rows, &job, shadow_service, doc);
    bool success = !__atomic_load_n(&doc->native_cancelled,__ATOMIC_ACQUIRE);
    if (success) {
        job.source = temporary; job.target = mask->alpha; job.vertical = true;
        parallel_for_stage(PARALLEL_RASTER, rows, shadow_blur_rows, &job, shadow_service, doc);
        success = !__atomic_load_n(&doc->native_cancelled,__ATOMIC_ACQUIRE);
    }
    free(kernel); free(temporary); return success;
}
static bool shadow_cache_prepare_radii(web_doc *doc, struct shadow_cache **slot, const style_t *style,
                                 float x, float y, float w, float h, const float *borders,struct border_radii radii) {
    const struct css_box_shadow *shadow = style->box_shadow;
    if (!shadow) { web_shadow_cache_free(slot); return true; }
    float fx = x-floorf(x), fy = y-floorf(y); size_t count = 0;
    for (const struct css_box_shadow *s = shadow; s; s=s->next) {
        if(!(count&63)&&!web_native_checkpoint(doc))return false;
        if (count == (SIZE_MAX-sizeof(struct shadow_cache))/sizeof(struct shadow_mask)) return false; count++;
    }
    struct shadow_cache *old = *slot;
    float viewport[4]={doc->view_x-floorf(x),doc->view_y-floorf(y),doc->view_x+doc->width-floorf(x),doc->view_y+doc->height-floorf(y)};
    bool same = old && old->count==count && old->width==w && old->height==h && !memcmp(&old->radii,&radii,sizeof radii) && old->fx==fx && old->fy==fy &&
        !memcmp(old->borders,borders,sizeof old->borders) && (!old->clipped||!memcmp(old->viewport,viewport,sizeof viewport));
    for (size_t i=0; same && i<count; i++,shadow=shadow->next) {
        const struct css_box_shadow *a=&old->masks[i].value;
        same = a->x==shadow->x && a->y==shadow->y && a->blur==shadow->blur && a->spread==shadow->spread && a->inset==shadow->inset &&
            (!!(a->color>>24)==!!(shadow->color>>24));
    }
    if (same) {
        shadow=style->box_shadow; for(size_t i=0;i<count;i++,shadow=shadow->next)old->masks[i].value.color=shadow->color;
        doc->paint_shadow_crop|=old->clipped;
        return true;
    }
    struct shadow_cache *next=calloc(1,sizeof *next+count*sizeof *next->masks); if(!next)return false;
    next->width=w;next->height=h;next->radii=radii;next->fx=fx;next->fy=fy;next->count=count;memcpy(next->borders,borders,sizeof next->borders);
    memcpy(next->viewport,viewport,sizeof viewport);
    shadow=style->box_shadow;
    for(size_t i=0;i<count;i++,shadow=shadow->next){
        if(!(i&63)&&!web_native_checkpoint(doc)){web_shadow_cache_free(&next);return false;}
        next->masks[i].value=*shadow;next->masks[i].value.next=NULL;
        if(!shadow_mask_build(doc,next,&next->masks[i])){web_shadow_cache_free(&next);return false;}
    }
    doc->paint_shadow_crop|=next->clipped;web_shadow_cache_free(slot);*slot=next;return true;
}
static bool shadow_cache_prepare(web_doc *doc,struct shadow_cache **slot,const style_t *style,
                                 float x,float y,float w,float h,const float *borders) {
    return shadow_cache_prepare_radii(doc,slot,style,x,y,w,h,borders,used_radii(style,w,h));
}
static void paint_shadows(struct pctx *P,const struct shadow_cache *cache,float x,float y,bool inset) {
    if(!cache)return;canvas_t *canvas=P->c;
    for(size_t i=cache->count;i>0;i--){
        const struct shadow_mask *mask=&cache->masks[i-1];const struct css_box_shadow *shadow=&mask->value;
        if(shadow->inset!=inset || !(shadow->color>>24))continue;
        float bx=inset?x+cache->borders[3]:x,by=inset?y+cache->borders[0]:y;
        float bw=inset?cache->width-cache->borders[1]-cache->borders[3]:cache->width;
        float bh=inset?cache->height-cache->borders[0]-cache->borders[2]:cache->height;
        struct border_radii radii=inset?inset_radii(cache->radii,cache->borders,bw,bh):cache->radii;
        if(!inset&&!mask->alpha)continue;
        double mx=floor((double)x)+mask->left,my=floor((double)y)+mask->top;
        double left=inset?floor((double)bx):mx,top=inset?floor((double)by):my;
        double right=inset?ceil((double)bx+bw):mx+mask->width,bottom=inset?ceil((double)by+bh):my+mask->height;
        int x0=(int)fmax(canvas->cx0,fmin(canvas->cx1,left)),y0=(int)fmax(canvas->cy0,fmin(canvas->cy1,top));
        int x1=(int)fmax(canvas->cx0,fmin(canvas->cx1,right)),y1=(int)fmax(canvas->cy0,fmin(canvas->cy1,bottom));
        /* Masks, allocation, Gaussian work and cancellation are completed in
           preflight. Do not introduce a new halfway-cancelled canvas write:
           ordinary (non-alpha-group) paint has no viewport rollback buffer. */
        for(int py=y0;py<y1;py++) {
            for(int px=x0;px<x1;px++) {
                float cover=corners_coverage(px+.5f,py+.5f,bx,by,bw,bh,&radii);
                if(!inset)cover=1-cover;if(cover<=0)continue;
                unsigned alpha=0;
                if(mask->alpha&&px>=mx&&py>=my&&px-mx<mask->width&&py-my<mask->height)
                    alpha=mask->alpha[(size_t)(py-my)*mask->width+(size_t)(px-mx)];
                if(inset)alpha=255-alpha;
                uint32_t color=with_alpha(shadow->color,cover*(float)alpha*(1.0f/255));
                if(color>>24)gfx_blend_pixel(canvas,px,py,color);
            }
        }
    }
}

/* Each primitive is joined before the next one, preserving stacking/alpha
 * order. Helpers see only immutable geometry and private canvas clip state;
 * DOM, font, image-cache, video and debug side effects stay on the caller. */
struct raster_job {
    canvas_t canvas;
    void (*draw)(canvas_t *, const void *);
    const void *geometry;
    int y0, y1, visible_width;
};
static void raster_tile(size_t tile, void *context) {
    const struct raster_job *j = context;
    canvas_t c = j->canvas;
    c.cy0 = j->y0 + (int)tile * 32;
    c.cy1 = MIN(c.cy0 + 32, j->y1);
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->visible_width * (c.cy1 - c.cy0));
    j->draw(&c, j->geometry);
}
static void raster_draw(canvas_t *c, float x, float y, float w, float h,
                        void (*draw)(canvas_t *, const void *), const void *geometry) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) return;
    /* Intersect in floating point before converting untrusted CSS dimensions. */
    int y0 = (int)fmaxf((float)c->cy0, fminf((float)c->cy1, floorf(y)));
    int y1 = (int)fmaxf((float)c->cy0, fminf((float)c->cy1, ceilf(y + h)));
    float visible_w = fmaxf(0, fminf((float)c->cx1, x + w) - fmaxf((float)c->cx0, x));
    if (y1 - y0 < 64 || visible_w * (y1 - y0) < 65536) { draw(c, geometry); return; }
    struct raster_job j = {*c, draw, geometry, y0, y1, (int)ceilf(visible_w)};
    parallel_for_stage(PARALLEL_RASTER,(size_t)(y1 - y0 + 31) / 32, raster_tile, &j,NULL,NULL);
}
struct rrect_geometry { float x, y, w, h, r; uint32_t top, bottom; const float *bw; const uint32_t *colors; };
static void raster_fill(canvas_t *c, const void *geometry) {
    const struct rrect_geometry *g = geometry;
    fill_rrect_serial(c, g->x, g->y, g->w, g->h, g->r, g->top, g->bottom);
}
static void raster_ring(canvas_t *c, const void *geometry) {
    const struct rrect_geometry *g = geometry;
    ring_rrect_serial(c, g->x, g->y, g->w, g->h, g->r, g->bw, g->colors);
}
static void fill_rrect(canvas_t *c, float x, float y, float w, float h, float r, uint32_t top, uint32_t bot) {
    struct rrect_geometry g = {x,y,w,h,r,top,bot,NULL,NULL};
    raster_draw(c,x,y,w,h,raster_fill,&g);
}
static void ring_rrect(canvas_t *c, float x, float y, float w, float h, float r, const float *bw, const uint32_t *col) {
    struct rrect_geometry g = {x,y,w,h,r,0,0,bw,col};
    raster_draw(c,x,y,w,h,raster_ring,&g);
}

static uint32_t shade(uint32_t c, float f) {
    uint32_t r = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        float v = (float)((c >> s) & 255) * f;
        if (v > 255) v = 255;
        r |= (uint32_t)v << s;
    }
    return r;
}

static void edge(canvas_t *c, int style, int side, float x, float y, float w, float h, uint32_t col) {
    int X = (int)roundf(x), Y = (int)roundf(y);
    int W = (int)roundf(x + w) - X, H = (int)roundf(y + h) - Y;
    if (W <= 0 || H <= 0) return;
    bool horiz = side == 0 || side == 2;
    int t = horiz ? H : W;
    switch (style) {
    case BS_DASHED: case BS_DOTTED: {
        int len = horiz ? W : H;
        int dash = style == BS_DOTTED ? t : t * 3, gap = style == BS_DOTTED ? t : t * 2;
        if (dash < 1) dash = 1;
        if (gap < 1) gap = 1;
        for (int p = 0; p < len; p += dash + gap) {
            int n = p + dash > len ? len - p : dash;
            if (style == BS_DOTTED && t >= 3) {
                int rr = t / 2;
                if (horiz) gfx_fill_circle(c, X + p + rr, Y + rr, rr, col);
                else gfx_fill_circle(c, X + rr, Y + p + rr, rr, col);
            } else if (horiz) gfx_fill_blend(c, X + p, Y, n, H, col);
            else gfx_fill_blend(c, X, Y + p, W, n, col);
        }
        return;
    }
    case BS_DOUBLE:
        if (t >= 3) {
            int a = (t + 1) / 3;
            if (horiz) {
                gfx_fill_blend(c, X, Y, W, a, col);
                gfx_fill_blend(c, X, Y + H - a, W, a, col);
            } else {
                gfx_fill_blend(c, X, Y, a, H, col);
                gfx_fill_blend(c, X + W - a, Y, a, H, col);
            }
            return;
        }
        break;
    case BS_GROOVE: case BS_RIDGE: {
        bool first_dark = (style == BS_GROOVE) == (side == 0 || side == 3);
        uint32_t a = first_dark ? shade(col, 0.6f) : shade(col, 1.3f), b = first_dark ? shade(col, 1.3f) : shade(col, 0.6f);
        bool outer_first = side == 0 || side == 3;
        if (t < 2) {
            gfx_fill_blend(c, X, Y, W, H, a);
            return;
        }
        uint32_t c1 = outer_first ? a : b, c2 = outer_first ? b : a;
        if (horiz) {
            gfx_fill_blend(c, X, Y, W, H / 2, c1);
            gfx_fill_blend(c, X, Y + H / 2, W, H - H / 2, c2);
        } else {
            gfx_fill_blend(c, X, Y, W / 2, H, c1);
            gfx_fill_blend(c, X + W / 2, Y, W - W / 2, H, c2);
        }
        return;
    }
    case BS_INSET: col = side == 0 || side == 3 ? shade(col, 0.6f) : shade(col, 1.25f); break;
    case BS_OUTSET: col = side == 0 || side == 3 ? shade(col, 1.25f) : shade(col, 0.6f); break;
    }
    gfx_fill_blend(c, X, Y, W, H, col);
}

static float used_radius(const style_t *st, float w, float h) {
    float r = st->border_radius;
    if (r < 0) r = -r / 100 * fminf(w, h); /* percentages are stored negated */
    return r;
}

/* draw img at (x, y) as a mask: col with the image's alpha */
static void mask_draw(canvas_t *c, const image_t *img, int x, int y, uint32_t col) {
    int x0 = x > c->cx0 ? x : c->cx0, y0 = y > c->cy0 ? y : c->cy0;
    int x1 = x + img->w < c->cx1 ? x + img->w : c->cx1, y1 = y + img->h < c->cy1 ? y + img->h : c->cy1;
    uint32_t ca = col >> 24, rgb = col & 0xFFFFFFu;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            uint32_t a = (img->px[(size_t)(py - y) * img->w + (px - x)] >> 24) * ca / 255;
            if (a) gfx_blend_pixel(c, px, py, a << 24 | rgb);
        }
}

/* an image layer (background-image, or mask-image tinted with col) over the padding box, clipped
   to the border box */
static void paint_layer(struct pctx *P, int img_idx, uint8_t size_kind, const len_t *size, const len_t *pos,
                        uint8_t repeat, uint32_t col, float x, float y, float w, float h, const float *bw) {
    web_doc *d = P->d;
    int idx = img_idx - 1;
    if (idx < 0 || idx >= d->images.n) return;
    struct web_image *im = d->images.v[idx];
    if (!im->img || im->img->w < 1 || im->img->h < 1) return;
    float iw = (float)im->img->w, ih = (float)im->img->h;
    float ax = x + bw[3], ay = y + bw[0], aw = w - bw[1] - bw[3], ah = h - bw[0] - bw[2];
    if (aw <= 0 || ah <= 0) return;
    float tw = iw, th = ih;
    switch (size_kind) {
    case BSZ_COVER: case BSZ_CONTAIN: {
        float sx = aw / iw, sy = ah / ih;
        float s = size_kind == BSZ_COVER ? fmaxf(sx, sy) : fminf(sx, sy);
        tw = iw * s;
        th = ih * s;
        break;
    }
    case BSZ_LEN: {
        bool wa = len_auto(&size[0]), ha = len_auto(&size[1]);
        if (!wa) tw = len_resolve(&size[0], aw);
        if (!ha) th = len_resolve(&size[1], ah);
        if (wa && !ha) tw = th * iw / ih;
        else if (ha && !wa) th = tw * ih / iw;
        break;
    }
    }
    int TW = (int)roundf(tw), TH = (int)roundf(th);
    if (TW < 1 || TH < 1) return;
    float px = ax + len_resolve(&pos[0], aw - (float)TW), py = ay + len_resolve(&pos[1], ah - (float)TH);
    canvas_t *c = P->c;
    int X0 = (int)roundf(x), Y0 = (int)roundf(y);
    int X1 = (int)roundf(x + w), Y1 = (int)roundf(y + h);
    if (X0 >= c->cx1 || Y0 >= c->cy1 || X1 <= c->cx0 || Y1 <= c->cy0) return;
    image_t *img = scaled_image(im, TW, TH, c, X0, Y0, X1-X0, Y1-Y0);
    if (!img) return;
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    /* rounded edges, so a box at a fractional position shows no sliver of a neighbouring tile */
    gfx_clip(c, X0, Y0, (int)roundf(x + w) - X0, (int)roundf(y + h) - Y0);
    bool rx = repeat == BR_REPEAT || repeat == BR_REPEAT_X;
    bool ry = repeat == BR_REPEAT || repeat == BR_REPEAT_Y;
    int x0 = (int)roundf(px), y0 = (int)roundf(py);
    int x1 = x0 + TW, y1 = y0 + TH;
    if (rx) {
        while (x0 > c->cx0) x0 -= TW;
        x1 = c->cx1;
    }
    if (ry) {
        while (y0 > c->cy0) y0 -= TH;
        y1 = c->cy1;
    }
    /* skip tiles above the clip */
    if (ry && y0 + TH <= c->cy0) y0 += (c->cy0 - y0) / TH * TH;
    if (rx && x0 + TW <= c->cx0) x0 += (c->cx0 - x0) / TW * TW;
    for (int ty = y0; ty < y1 && ty < c->cy1; ty += TH)
        for (int tx = x0; tx < x1 && tx < c->cx1; tx += TW) {
            if (col) mask_draw(c, img, tx, ty, col);
            else image_draw(c, img, tx, ty);
        }
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
}

/* background and borders of a border box */
/* ---------------------------------------------------------------- gradients */
#define NOPOS (-1e30f)
struct gstops {
    int n;
    bool rep;
    uint32_t col[GRAD_MAX];
    float pos[GRAD_MAX]; /* fractions of the gradient's length */
};

/* interpolate with premultiplied alpha, as CSS does: a fade to transparent keeps its colour */
static uint32_t premul_lerp(uint32_t a, uint32_t b, float t) {
    float aa = (float)(a >> 24) / 255, ba = (float)(b >> 24) / 255, oa = aa + (ba - aa) * t;
    if (oa <= 0) return 0;
    uint32_t r = (uint32_t)(oa * 255 + 0.5f) << 24;
    for (int s = 0; s < 24; s += 8) {
        float x = (float)((a >> s) & 255) * aa, y = (float)((b >> s) & 255) * ba, v = (x + (y - x) * t) / oa;
        r |= (uint32_t)(v > 255 ? 255 : v + 0.5f) << s;
    }
    return r;
}

static void resolve_stops(const struct gradient *g, uint32_t cur, float len, struct gstops *o) {
    o->n = g->n;
    o->rep = g->repeating;
    for (int i = 0; i < g->n; i++) {
        o->col[i] = g->col[i] == COLOR_CURRENT ? cur : g->col[i];
        o->pos[i] = g->pos[i].kind == LK_AUTO ? NOPOS : len > 0 ? len_resolve(&g->pos[i], len) / len : 0;
    }
    if (o->pos[0] == NOPOS) o->pos[0] = 0;
    if (o->pos[o->n - 1] == NOPOS) o->pos[o->n - 1] = 1;
    float mx = o->pos[0];
    for (int i = 1; i < o->n; i++) /* stops never go backwards */
        if (o->pos[i] != NOPOS) {
            if (o->pos[i] < mx) o->pos[i] = mx;
            mx = o->pos[i];
        }
    for (int i = 1; i < o->n; i++) /* stops without a position share the gap between their neighbours */
        if (o->pos[i] == NOPOS) {
            int j = i;
            while (o->pos[j] == NOPOS) j++;
            for (int k = i; k < j; k++) o->pos[k] = o->pos[i - 1] + (o->pos[j] - o->pos[i - 1]) * (float)(k - i + 1) / (float)(j - i + 1);
        }
}

static uint32_t stops_at(const struct gstops *s, float t) {
    if (s->rep) {
        float a = s->pos[0], span = s->pos[s->n - 1] - a;
        if (span > 1e-4f) {
            t = a + fmodf(t - a, span);
            if (t < a) t += span;
        }
    }
    if (t <= s->pos[0]) return s->col[0];
    for (int i = 1; i < s->n; i++)
        if (t <= s->pos[i]) {
            float d = s->pos[i] - s->pos[i - 1];
            return d <= 1e-6f ? s->col[i] : premul_lerp(s->col[i - 1], s->col[i], (t - s->pos[i - 1]) / d);
        }
    return s->col[s->n - 1];
}

static void fill_gradient_serial(canvas_t *c, float x, float y, float w, float h, float r, const struct gradient *g, uint32_t cur) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    bool round = r >= 0.5f;
    int X0 = round ? (int)floorf(x) : (int)roundf(x), Y0 = round ? (int)floorf(y) : (int)roundf(y);
    int X1 = round ? (int)ceilf(x + w) : (int)roundf(x + w), Y1 = round ? (int)ceilf(y + h) : (int)roundf(y + h);
    if (Y0 < c->cy0) Y0 = c->cy0;
    if (Y1 > c->cy1) Y1 = c->cy1;
    if (X0 < c->cx0) X0 = c->cx0;
    if (X1 > c->cx1) X1 = c->cx1;
    if (X0 >= X1 || Y0 >= Y1) return;
    float cx = x + w / 2, cy = y + h / 2, dx = 0, dy = 0, len, rx = 1, ry = 1;
    if (!g->radial) {
        if (g->to_x || g->to_y) { /* to a corner: the 50% line joins the other two corners */
            dx = (float)g->to_x * h;
            dy = (float)g->to_y * w;
        } else {
            float a = g->angle * (float)M_PI / 180;
            dx = sinf(a);
            dy = -cosf(a);
        }
        float m = sqrtf(dx * dx + dy * dy);
        dx /= m;
        dy /= m;
        len = fabsf(w * dx) + fabsf(h * dy);
        if (len < 1e-3f) len = 1e-3f;
    } else {
        cx = x + len_resolve(&g->at[0], w);
        cy = y + len_resolve(&g->at[1], h);
        float l = fabsf(cx - x), rt = fabsf(x + w - cx), t = fabsf(cy - y), b = fabsf(y + h - cy);
        float sxn = fminf(l, rt), sxf = fmaxf(l, rt), syn = fminf(t, b), syf = fmaxf(t, b);
        bool far = g->rsize == RG_FARTHEST_SIDE || g->rsize == RG_FARTHEST_CORNER;
        float sx = far ? sxf : sxn, sy = far ? syf : syn;
        if (g->rsize == RG_CLOSEST_SIDE || g->rsize == RG_FARTHEST_SIDE) {
            rx = sx;
            ry = sy;
            if (g->circle) rx = ry = far ? fmaxf(sx, sy) : fminf(sx, sy);
        } else if (g->circle) rx = ry = sqrtf(sx * sx + sy * sy);
        else { /* the ellipse through the corner with the sides' aspect ratio */
            rx = sx * 1.41421356f;
            ry = sy * 1.41421356f;
        }
        if (rx < 1e-3f) rx = 1e-3f;
        if (ry < 1e-3f) ry = 1e-3f;
        len = rx;
    }
    struct gstops S;
    resolve_stops(g, cur, len, &S);
    for (int py = Y0; py < Y1; py++) {
        float fy = (float)py + 0.5f;
        bool corner_row = round && (fy < y + r || fy > y + h - r);
        if (!g->radial && fabsf(dx) < 1e-6f && !corner_row) { /* vertical: one colour per row */
            uint32_t col = stops_at(&S, ((fy - cy) * dy) / len + 0.5f);
            gfx_fill_blend(c, X0, py, X1 - X0, 1, col);
            continue;
        }
        for (int px = X0; px < X1; px++) {
            float fx = (float)px + 0.5f, t;
            if (!g->radial) t = ((fx - cx) * dx + (fy - cy) * dy) / len + 0.5f;
            else {
                float ex = (fx - cx) / rx, ey = (fy - cy) / ry;
                t = sqrtf(ex * ex + ey * ey);
            }
            uint32_t col = stops_at(&S, t);
            if (corner_row && (fx < x + r || fx > x + w - r)) {
                float cov = coverage(sdf_rrect(fx, fy, x, y, w, h, r));
                if (cov <= 0) continue;
                col = with_alpha(col, cov);
            }
            gfx_blend_pixel(c, px, py, col);
        }
    }
}

struct gradient_geometry { float x,y,w,h,r; const struct gradient *gradient; uint32_t color; };
static void raster_gradient(canvas_t *c, const void *geometry) {
    const struct gradient_geometry *g = geometry;
    fill_gradient_serial(c,g->x,g->y,g->w,g->h,g->r,g->gradient,g->color);
}
static void fill_gradient(canvas_t *c, float x, float y, float w, float h, float r, const struct gradient *g, uint32_t cur) {
    struct gradient_geometry geometry = {x,y,w,h,r,g,cur};
    raster_draw(c,x,y,w,h,raster_gradient,&geometry);
}

static unsigned radius_alpha(const struct radius_cache *cache,int x,int y,float shape_x,float shape_y) {
    int64_t px=(int64_t)x-(int64_t)lround((double)cache->left+((double)shape_x-cache->x)),py=(int64_t)y-(int64_t)lround((double)cache->top+((double)shape_y-cache->y));
    return px>=0&&py>=0&&px<cache->width&&py<cache->height&&cache->alpha?cache->alpha[(size_t)py*cache->width+(size_t)px]:0;
}
static void paint_corner_border(struct pctx *P,float x,float y,float w,float h,const float *widths,const uint32_t *colors,
                                const struct radius_cache *outer,const struct radius_cache *inner) {
    canvas_t *c=P->c;
    int x0=(int)fmax(c->cx0,fmin(c->cx1,floor((double)x))),y0=(int)fmax(c->cy0,fmin(c->cy1,floor((double)y)));
    int x1=(int)fmax(c->cx0,fmin(c->cx1,ceil((double)x+w))),y1=(int)fmax(c->cy0,fmin(c->cy1,ceil((double)y+h)));
    for(int py=y0;py<y1;py++)for(int px=x0;px<x1;px++) {
        unsigned outside=radius_alpha(outer,px,py,x,y);if(!outside)continue;
        unsigned inside=inner?radius_alpha(inner,px,py,x+widths[3],y+widths[0]):
            (px+.5f>=x+widths[3]&&px+.5f<x+w-widths[1]&&py+.5f>=y+widths[0]&&py+.5f<y+h-widths[2]?255:0);
        float cover=outside*(255-inside)*(1.f/(255*255));if(cover<=0)continue;
        float distance[4]={py+.5f-y,x+w-px-.5f,y+h-py-.5f,px+.5f-x};int side=-1;
        for(int i=0;i<4;i++)if(widths[i]>0&&(side<0||distance[i]*widths[side]<distance[side]*widths[i]))side=i;
        if(side>=0)gfx_blend_pixel(c,px,py,with_alpha(colors[side],cover));
    }
}
static void paint_bg_border(struct pctx *P, const style_t *st, float x, float y, float w, float h, const float *bw,
                            bool skip_bg, bool open_l, bool open_r, const struct shadow_cache *shadows,struct radius_cache *const *rounded) {
    canvas_t *c = P->c;
    /* Culling the primitive, not the subtree: visible-overflow and fixed
     * descendants must still be walked, and offscreen resources stay wanted. */
    paint_shadows(P,shadows,x,y,false);
    if(P->failed)return;
    if (x >= c->cx1 || y >= c->cy1 || x + w <= c->cx0 || y + h <= c->cy0) return;
    float r = used_radius(st, w, h);
    const struct gfx_mask_clip *previous=c->mask_clip;struct gfx_mask_clip boundary;
    if(rounded)radius_clip_push(P,rounded[0],&boundary,x,y);
    if (st->mask_img) { /* an icon: the background colour through the mask, nothing else */
        uint32_t col = st->has_grad ? st->grad[0] : st->bg_color;
        if (!skip_bg && (col >> 24))
            paint_layer(P, st->mask_img, st->mask_size_kind, st->mask_size, st->mask_pos, st->mask_repeat, col, x, y, w,
                        h, bw);
        c->mask_clip=previous;return;
    }
    if (!skip_bg) {
        if (st->bg_color >> 24) {
            if(rounded&&rounded[0])gfx_fill_blend(c,(int)floorf(x),(int)floorf(y),(int)ceilf(x+w)-(int)floorf(x),(int)ceilf(y+h)-(int)floorf(y),st->bg_color);
            else fill_rrect(c, x, y, w, h, r, st->bg_color, st->bg_color);
        }
        if (st->has_grad && st->gradient) fill_gradient(c, x, y, w, h, rounded&&rounded[0]?0:r, st->gradient, st->color);
    }
    if (st->bg_img)
        paint_layer(P, st->bg_img, st->bg_size_kind, st->bg_size, st->bg_pos, st->bg_repeat, 0, x, y, w, h, bw);
    c->mask_clip=previous;
    paint_shadows(P,shadows,x,y,true);
    if(P->failed)return;
    const uint32_t *bc = st->border_color;
    bool any = false;
    for (int i = 0; i < 4; i++)
        if (bw[i] > 0 && (bc[i] >> 24) && st->border_style[i] > BS_HIDDEN) any = true;
    if (!any) return;
    if(rounded&&rounded[0]) {
        float widths[4];uint32_t colors[4];for(int i=0;i<4;i++){widths[i]=bw[i];colors[i]=st->border_style[i]>BS_HIDDEN?bc[i]:0;}
        paint_corner_border(P,x,y,w,h,widths,colors,rounded[0],rounded[1]);return;
    }
    if (r >= 1) {
        float bws[4];
        uint32_t cols[4];
        for (int i = 0; i < 4; i++) {
            bws[i] = st->border_style[i] > BS_HIDDEN ? bw[i] : 0;
            cols[i] = bc[i];
        }
        ring_rrect(c, x, y, w, h, r, bws, cols);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (bw[i] <= 0 || !(bc[i] >> 24) || st->border_style[i] <= BS_HIDDEN) continue;
        if ((i == 3 && open_l) || (i == 1 && open_r)) continue;
        switch (i) {
        case 0: edge(c, st->border_style[0], 0, x, y, w, bw[0], bc[0]); break;
        case 2: edge(c, st->border_style[2], 2, x, y + h - bw[2], w, bw[2], bc[2]); break;
        case 3: edge(c, st->border_style[3], 3, x, y + bw[0], bw[3], h - bw[0] - bw[2], bc[3]); break;
        case 1: edge(c, st->border_style[1], 1, x + w - bw[1], y + bw[0], bw[1], h - bw[0] - bw[2], bc[1]); break;
        }
    }
}

/* ---------------------------------------------------------------- text */
static void draw_text(struct pctx *P, const style_t *st, float x, float baseline, const char *s, int n, uint32_t color) {
    wfont f = style_font(st);
    canvas_t *c = P->c;
    int bl = (int)roundf(baseline);
    if (st->letter_spacing == 0 && st->word_spacing == 0) {
        wf_draw(c, &f, x, bl, s, (size_t)n, color);
        return;
    }
    for (int i = 0; i < n;) {
        int cl = 1;
        while (i + cl < n && ((unsigned char)s[i + cl] & 0xC0) == 0x80) cl++;
        x = wf_draw(c, &f, x, bl, s + i, (size_t)cl, color) + st->letter_spacing;
        if (s[i] == ' ') x += st->word_spacing;
        i += cl;
    }
}

static void paint_run_text(struct pctx *P, const struct run *r, float bx, float by) {
    const style_t *st = r->st;
    if (st->visibility) return;
    float x = P->ox + bx + r->x, base = P->oy + by + r->y;
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    if (base - asc > P->c->cy1 || base + desc < P->c->cy0) return;
    if (r->highlight) gfx_fill_blend(P->c, (int)x, (int)(base - asc), (int)ceilf(r->w), (int)ceilf(asc + desc), RGB(255, 213, 0));
    uint32_t col = st->color;
    draw_text(P, st, x, base, r->s, r->n, col);
    if (st->text_decoration) {
        float th = st->font_size / 14;
        if (th < 1) th = 1;
        int t = (int)roundf(th);
        int w = (int)ceilf(r->w);
        /* trailing spaces are not decorated at the end of a line, but between words they are */
        if (st->text_decoration & TD_UNDERLINE) gfx_fill_blend(P->c, (int)x, (int)roundf(base + desc * 0.45f), w, t, col);
        if (st->text_decoration & TD_OVERLINE) gfx_fill_blend(P->c, (int)x, (int)roundf(base - asc), w, t, col);
        if (st->text_decoration & TD_LINE_THROUGH)
            gfx_fill_blend(P->c, (int)x, (int)roundf(base - asc * 0.32f), w, t, col);
    }
}

/* ---------------------------------------------------------------- replaced elements and controls */
static image_t *scaled_image(struct web_image *im, int w, int h,
                             const canvas_t *canvas, int x, int y, int box_w, int box_h) {
    if (w <= 0 || h <= 0 || !im->img) return NULL;
    if (im->img->w == w && im->img->h == h) return im->img;
    if (im->scaled && im->scaled->w == w && im->scaled->h == h) return im->scaled;
    if(web_avmedia_snapshot_is_probe()){
        /* An uncached image outside the committed video span must not disable
           that surface. Probes neither allocate nor replace shared caches;
           only an intersecting paint occurrence makes their result unsafe. */
        web_avmedia_snapshot_reject_region(canvas,x,y,box_w,box_h);
        return NULL;
    }
    if ((long)w * h > IMAGE_MAX_PIXELS) return NULL;
    if (im->scaled) image_free(im->scaled);
    im->scaled = im->svg ? image_decode_svg(im->svg, im->svg_n, w, h) : image_scale(im->img, w, h);
    return im->scaled;
}

static void text_in(struct pctx *P, const style_t *st, float x, float y, float h, const char *s, uint32_t col) {
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    draw_text(P, st, x, y + (h - (asc + desc)) / 2 + asc, s, (int)strlen(s), col);
}
static size_t password_offset(const char *text, size_t byte) {
    size_t bullets = 0;
    for (size_t i = 0; i < byte && text[i]; i++) if (((unsigned char)text[i] & 0xc0) != 0x80) bullets++;
    return bullets * 3;
}
static uint32_t control_text_color(const style_t *style, bool placeholder) {
    if (!placeholder) return style->color;
    return style->placeholder ? with_alpha(style->placeholder->color, style->placeholder->opacity) : RGB(117, 117, 117);
}

static void control_line_text(struct pctx *P, const style_t *st, float x, float baseline,
                              const char *text, size_t length, uint32_t color) {
    if (!(color >> 24)) return;
    size_t first = 0;
    float offset = 0, stop = control_text_width(st,"\t",1);
    for (size_t at = 0; at < length; at++) if (text[at] == '\t') {
        draw_text(P, st, x + offset, baseline, text + first, (int)(at - first), color);
        offset += style_text_width(st,text + first,at - first);
        if (stop > 0) offset += stop - fmodf(fmaxf(0,offset),stop);
        first = at + 1;
    }
    draw_text(P, st, x + offset, baseline, text + first, (int)(length - first), color);
}
struct control_line_paint {
    struct pctx *P;
    const style_t *style, *base;
    const char *text;
    size_t length, selection_start, selection_end;
    float x, y, ascent, descent;
    int caret;
    uint32_t color;
    bool selected, focused;
};
static void paint_control_line(const struct control_text_line *line, void *opaque) {
    struct control_line_paint *p = opaque;
    canvas_t *c = p->P->c;
    float ly = p->y + line->top, lh = line->height;
    if (!(lh > 0) || ly + lh < c->cy0 || ly > c->cy1) return;
    const char *text = p->text + line->first;
    size_t len = line->length, start = p->selection_start > line->first ? p->selection_start - line->first : 0,
        end = p->selection_end > line->first ? p->selection_end - line->first : 0;
    if (start > len) start = len;
    if (end > len) end = len;
    float left = p->x + control_text_width(p->style, text, start), right = p->x + control_text_width(p->style, text, end);
    bool selected = p->selected && p->selection_start <= line->first + len && p->selection_end > line->first;
    if (selected && line->hard_break && p->selection_end > line->first + len) right += control_text_width(p->style, " ", 1);
    float baseline = ly + (lh - p->ascent - p->descent) / 2 + p->ascent;
    if (selected) gfx_fill(c, (int)floorf(left), (int)ly, (int)ceilf(right-left), (int)ceilf(lh), RGB(0,117,255));
    control_line_text(p->P, p->style, p->x, baseline, text, len, p->color);
    if (selected) {
        int cx0=c->cx0,cy0=c->cy0,cx1=c->cx1,cy1=c->cy1;
        gfx_clip(c,(int)floorf(left),(int)ly,(int)ceilf(right-left),(int)ceilf(lh));
        control_line_text(p->P,p->base,p->x,baseline,text,len,RGB(255,255,255));
        c->cx0=cx0;c->cy0=cy0;c->cx1=cx1;c->cy1=cy1;
    }
    if (p->focused && !p->selected && p->caret >= 0 && (size_t)p->caret >= line->first &&
        (size_t)p->caret <= line->first + len &&
        ((size_t)p->caret < line->first + len || line->hard_break || line->next == p->length)) {
        float cx = p->x + control_text_width(p->style,text,(size_t)p->caret-line->first);
        gfx_fill(c,(int)cx,(int)ly+2,1,(int)fmaxf(1,lh-4),p->base->color);
    }
}

static void paint_control(struct pctx *P, box_t *b, float x, float y) {
    /* x, y: content box on the canvas */
    canvas_t *c = P->c;
    node_t *n = b->node;
    const style_t *st = b->st;
    float w = b->w, h = b->h;
    wfont f = style_font(st);
    bool focused = P->d->focus == n;
    switch (b->atomic) {
    case AT_INPUT: case AT_TEXTAREA: {
        const char *v = web_input_edit_text(n);
        bool placeholder = !*v;
        if (placeholder) v = node_attr(n, "placeholder");
        if (!v) v = "";
        const style_t *text_style = placeholder && st->placeholder ? st->placeholder : st;
        if (placeholder && st->placeholder) f = style_font(text_style);
        int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
        /* like browsers, clip to the padding box: descenders may hang below a short content box */
        gfx_clip(c, (int)(x - b->p[3]), (int)(y - b->p[0]), (int)ceilf(w + b->p[1] + b->p[3]) + 1, (int)ceilf(h + b->p[0] + b->p[2]) + 1);
        const char *t = node_attr(n, "type");
        bool pw = t && str_ieq(t, "password") && !placeholder;
        uint32_t col = control_text_color(st, placeholder);
        if (placeholder && !(col >> 24) && b->atomic == AT_INPUT) v = "";
        size_t selection_start = !placeholder ? doc_utf16_to_byte(v, n->selection_start, false) : 0;
        size_t selection_end = !placeholder ? doc_utf16_to_byte(v, n->selection_end, true) : 0;
        bool selected = focused && !placeholder && n->selection_start != n->selection_end;
        float caret_x = x;
        if (b->atomic == AT_INPUT) {
            sbuf s = {0};
            if (pw)
                for (const char *p = v; *p; p++) {
                    if (((unsigned char)*p & 0xC0) != 0x80) sb_puts(&s, "\xe2\x80\xa2");
                }
            else sb_puts(&s, v);
            const char *txt = s.p ? sb_cstr(&s) : "";
            /* keep the caret in view */
            float caret_w = 0;
            if (focused && !placeholder) {
                int cb = P->d->caret;
                if (pw) cb = (int)password_offset(v, (size_t)cb);
                caret_w = wf_width(&f, txt, (size_t)cb);
            }
            float scroll = caret_w > w - 2 ? caret_w - w + 2 : 0;
            float left = 0, right = 0;
            if (selected) {
                size_t start = pw ? password_offset(v, selection_start) : selection_start;
                size_t end = pw ? password_offset(v, selection_end) : selection_end;
                left = x - scroll + wf_width(&f, txt, start);
                right = x - scroll + wf_width(&f, txt, end);
                gfx_fill(c, (int)floorf(left), (int)y, (int)ceilf(right - left), (int)ceilf(h), RGB(0, 117, 255));
            }
            text_in(P, text_style, x - scroll, y, h, txt, col);
            if (selected) {
                int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
                gfx_clip(c, (int)floorf(left), (int)y, (int)ceilf(right - left), (int)ceilf(h));
                text_in(P, st, x - scroll, y, h, txt, RGB(255, 255, 255));
                c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
            }
            caret_x = x - scroll + caret_w;
            sb_free(&s);
        } else {
            struct control_line_paint lines = {.P=P,.style=text_style,.base=st,.text=v,.length=strlen(v),
                .selection_start=selection_start,.selection_end=selection_end,.x=x,.y=y,
                .caret=focused?P->d->caret:-1,.color=col,.selected=selected,.focused=focused};
            wf_metrics(&f,&lines.ascent,&lines.descent);
            if (box_element_scrollable(b)) { lines.x-=(float)n->scroll_x; lines.y-=(float)n->scroll_y; }
            float text_width, text_height;
            if (!control_text_lines(b,text_style,v,paint_control_line,&lines,&text_width,&text_height)) P->failed=true;
        }
        if (focused && !selected && b->atomic == AT_INPUT) gfx_fill(c, (int)caret_x, (int)y + 1, 1, (int)h - 2, st->color);
        c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
        break;
    }
    case AT_BUTTON_INPUT: {
        const char *l = web_button_label(n);
        float tw = wf_width(&f, l, strlen(l));
        text_in(P, st, x + (w - tw) / 2, y, h, l, st->color);
        break;
    }
    case AT_CHECKBOX: {
        int X = (int)roundf(x), Y = (int)roundf(y), S = (int)roundf(w);
        if (n->indeterminate) {
            gfx_fill_round(c, X, Y, S, S, 2, RGB(0, 117, 255));
            gfx_fill(c, X + S / 4, Y + S / 2 - 1, S / 2, 2, RGB(255, 255, 255));
        } else if (n->checked) {
            gfx_fill_round(c, X, Y, S, S, 2, RGB(0, 117, 255));
            int s = S;
            /* a check mark */
            for (int k = -1; k <= 0; k++) {
                gfx_line(c, X + s * 22 / 100, Y + s / 2 + k, X + s * 42 / 100, Y + s * 70 / 100 + k, RGB(255, 255, 255));
                gfx_line(c, X + s * 42 / 100, Y + s * 70 / 100 + k, X + s * 78 / 100, Y + s * 30 / 100 + k, RGB(255, 255, 255));
            }
        } else {
            gfx_fill_round(c, X, Y, S, S, 2, RGB(118, 118, 118));
            gfx_fill_round(c, X + 1, Y + 1, S - 2, S - 2, 1, RGB(255, 255, 255));
        }
        break;
    }
    case AT_RADIO: {
        int r = (int)(w / 2), cx_ = (int)roundf(x + w / 2), cy_ = (int)roundf(y + h / 2);
        if (n->checked) {
            gfx_fill_circle(c, cx_, cy_, r, RGB(0, 117, 255));
            gfx_fill_circle(c, cx_, cy_, r - 1, RGB(255, 255, 255));
            gfx_fill_circle(c, cx_, cy_, r - 3, RGB(0, 117, 255));
        } else {
            gfx_fill_circle(c, cx_, cy_, r, RGB(118, 118, 118));
            gfx_fill_circle(c, cx_, cy_, r - 1, RGB(255, 255, 255));
        }
        break;
    }
    case AT_SELECT: {
        const char *labels[1];
        int sel = 0;
        sbuf s = {0};
        /* the selected option's label */
        int idx = 0;
        node_t *found = NULL;
        for (node_t *o = n->first; o && !found; o = o->next) {
            node_t *list = o->type == N_ELEM && o->tag == T_optgroup ? o->first : o;
            for (node_t *q = list; q; q = q->next) {
                if (q->type == N_ELEM && q->tag == T_option) {
                    if (idx == n->selected) found = q;
                    idx++;
                }
                if (list == o) break;
            }
        }
        if (found) {
            const char *lab = node_attr(found, "label");
            if (lab) sb_puts(&s, lab);
            else node_text_content(found, &s);
        }
        (void)labels;
        (void)sel;
        const char *txt = s.p ? sb_cstr(&s) : "";
        while (*txt == ' ' || *txt == '\n') txt++;
        int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
        gfx_clip(c, (int)x, (int)y, (int)w - 12, (int)ceilf(h) + 1);
        text_in(P, st, x, y, h, txt, st->color);
        c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
        int ax = (int)(x + w - 9), ay = (int)(y + h / 2 - 2);
        gfx_triangle(c, ax, ay, ax + 8, ay, ax + 4, ay + 5, st->color);
        sb_free(&s);
        break;
    }
    case AT_PLACEHOLDER: {
        int X = (int)roundf(x), Y = (int)roundf(y), W = (int)roundf(w), H = (int)roundf(h);
        if(web_frame_element(n))web_avmedia_snapshot_reject_region(c,X,Y,W,H);
        if(web_frame_element(n)&&web_avmedia_snapshot_is_probe())break;
        if(web_frame_paint(n,c,X,Y,W,H))break;
        if (n->tag == T_canvas) {
            web_canvas_paint(n,c,X,Y,W,H);
            break;
        }
        if (n->tag == T_audio || n->tag == T_video) {
            paint_debug_reached(n,c,X,Y,W,H);
            if(web_avmedia_paint(P->d,n,c,X,Y,W,H))break;
        }
        if (n->tag == T_meter || n->tag == T_progress) {
            double value = web_gauge_value(n, "value"), max = web_gauge_value(n, "max"), min = web_gauge_value(n, "min");
            double fraction = max > min ? (value - min) / (max - min) : 0;
            gfx_fill_round(c, X, Y + H / 4, W, H / 2, H / 4, RGB(220, 220, 220));
            if (n->tag == T_progress && web_gauge_value(n, "position") < 0) {
                gfx_fill_round(c, X + W / 3, Y + H / 4, W / 3, H / 2, H / 4, RGB(0, 117, 255));
            } else {
                int quality = n->tag == T_meter ? web_meter_quality(n) : 0;
                uint32_t color = n->tag == T_progress ? RGB(0, 117, 255) : quality == 0 ? RGB(16, 160, 64) : quality == 1 ? RGB(220, 160, 0) : RGB(210, 50, 45);
                gfx_fill_round(c, X, Y + H / 4, (int)(W * fraction), H / 2, H / 4, color);
            }
            break;
        }
        if (str_ieq(n->name, "input")) { /* range */
            gfx_fill_round(c, X, Y + H / 2 - 2, W, 4, 2, RGB(180, 180, 180));
            gfx_fill_circle(c, X + W / 2, Y + H / 2, 7, RGB(0, 117, 255));
            break;
        }
        gfx_fill_blend(c, X, Y, W, H, RGB(232, 232, 236));
        gfx_rect(c, X, Y, W, H, RGB(190, 190, 196));
        const char *label = n->tag == T_video ? "video" : n->tag == T_audio ? "audio" : n->tag == T_iframe ? "frame" :
                            n->tag == T_canvas ? "canvas" : "embedded content";
        char buf[160];
        const char *src = node_attr(n, "src");
        if (n->tag == T_iframe && src) snprintf(buf, sizeof buf, "[frame: %.120s]", src);
        else snprintf(buf, sizeof buf, "[%s]", label);
        wfont lf = f;
        float tw = wf_width(&lf, buf, strlen(buf));
        int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
        gfx_clip(c, X, Y, W, H);
        text_in(P, st, x + fmaxf(4, (w - tw) / 2), y, h, buf, RGB(90, 90, 100));
        c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
        break;
    }
    }
}

static void paint_replaced(struct pctx *P, box_t *b) {
    float x = P->ox + box_visual_x(b), y = P->oy + cy(b);
    int X = (int)roundf(x), Y = (int)roundf(y), W = (int)roundf(b->w), H = (int)roundf(b->h);
    canvas_t *c = P->c;
    if (X >= c->cx1 || Y >= c->cy1 ||
        (int64_t)X + W <= c->cx0 || (int64_t)Y + H <= c->cy0) return;
    if (b->atomic == AT_IMG) {
        node_t *n = b->node;
        struct web_image *im = n->image >= 0 && n->image < P->d->images.n ? P->d->images.v[n->image] : NULL;
        int fitted_w=W,fitted_h=H;
        if(im && im->img && W>0 && H>0 && im->img->w>0 && im->img->h>0 && b->st->object_fit!=OF_FILL){
            float iw=(float)im->img->w,ih=(float)im->img->h;
            float factor=b->st->object_fit==OF_COVER?fmaxf((float)W/iw,(float)H/ih):fminf((float)W/iw,(float)H/ih);
            if(b->st->object_fit==OF_NONE)factor=1;
            else if(b->st->object_fit==OF_SCALE_DOWN)factor=fminf(factor,1);
            /* The native decoder's finite pixel budget also caps fitted
               copies, including extreme aspect ratios in cover mode. */
            float fw=iw*factor,fh=ih*factor;
            if(fw>IMAGE_MAX_PIXELS || fh>IMAGE_MAX_PIXELS || fw*fh>IMAGE_MAX_PIXELS)return;
            fitted_w=(int)roundf(fw);fitted_h=(int)roundf(fh);
        }
        image_t *s = im ? scaled_image(im, fitted_w, fitted_h, c, X, Y, W, H) : NULL;
        if (s) {
            float dx=len_resolve(&b->st->object_pos[0],(float)(W-fitted_w));
            float dy=len_resolve(&b->st->object_pos[1],(float)(H-fitted_h));
            if(!isfinite(dx) || !isfinite(dy) || fabsf(dx)>10000000 || fabsf(dy)>10000000)return;
            int sx0=c->cx0,sy0=c->cy0,sx1=c->cx1,sy1=c->cy1;
            gfx_clip(c,X,Y,W,H);
            image_draw(c, s, X+(int)roundf(dx), Y+(int)roundf(dy));
            c->cx0=sx0;c->cy0=sy0;c->cx1=sx1;c->cy1=sy1;
            return;
        }
        if (im && !im->failed && !im->done) return; /* still loading */
        const char *alt = node_attr(n, "alt");
        if (alt && *alt) {
            int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
            gfx_clip(c, X, Y, W, H);
            if (W > 16 && H > 16) gfx_rect(c, X, Y, W, H, RGB(192, 192, 192));
            text_in(P, b->st, x + 2, y, fminf(b->h, b->st->font_size * 1.3f), alt, b->st->color);
            c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
        } else if (W > 2 && H > 2) gfx_rect(c, X, Y, W, H, RGB(200, 200, 200));
        return;
    }
    if (b->atomic == AT_SVG && b->svg) {
        struct svg_cache *sc = b->svg;
        if (!sc->img || sc->w != W || sc->h != H) {
            if(web_avmedia_snapshot_is_probe()){
                web_avmedia_snapshot_reject_region(c,X,Y,W,H);
                return;
            }
            if (sc->img) image_free(sc->img);
            sc->img = W > 0 && H > 0 && (long)W * H <= IMAGE_MAX_PIXELS ? image_decode_svg(sc->src, sc->n, W, H) : NULL;
            sc->w = W;
            sc->h = H;
        }
        if (sc->img) image_draw(c, sc->img, X, Y);
        return;
    }
    paint_control(P, b, x, y);
}

/* ---------------------------------------------------------------- list markers */
static void paint_marker(struct pctx *P, box_t *b) {
    const style_t *st = b->st;
    if (st->visibility || (!b->marker && !b->marker_shape)) return;
    float x = P->ox + box_visual_x(b), y = P->oy + cy(b);
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    float bl = b->baseline >= 0 ? b->baseline : (style_line_height(st) - asc - desc) / 2 + asc;
    float base = y + bl;
    float fs = st->font_size;
    if (b->marker_shape) {
        float r = fs * 0.18f;
        if (r < 2) r = 2;
        float mx = x - fs * 0.85f, my = base - fs * 0.32f;
        if (b->marker_shape == 1) {
            fill_rrect(P->c, mx - r, my - r, 2 * r, 2 * r, r, st->color, st->color);
        } else if (b->marker_shape == 2) {
            float bw[4] = {1.2f, 1.2f, 1.2f, 1.2f};
            uint32_t cols[4] = {st->color, st->color, st->color, st->color};
            ring_rrect(P->c, mx - r, my - r, 2 * r, 2 * r, r, bw, cols);
        } else gfx_fill_blend(P->c, (int)roundf(mx - r), (int)roundf(my - r), (int)roundf(2 * r), (int)roundf(2 * r), st->color);
        return;
    }
    const char *m = b->marker;
    float w = wf_width(&f, m, strlen(m));
    draw_text(P, st, x - w, base, m, (int)strlen(m), st->color);
}

/* ---------------------------------------------------------------- hit testing helpers */
static bool inside(struct pctx *P, float x, float y, float w, float h) {
    P->hit_shape_x=x;P->hit_shape_y=y;
    if (P->index_build) {
        float left = fmaxf(x, P->c->cx0 - P->ox), top = fmaxf(y, P->c->cy0 - P->oy);
        float right = fminf(x + w, P->c->cx1 - P->ox), bottom = fminf(y + h, P->c->cy1 - P->oy);
        if (!(right > left && bottom > top) || !isfinite(left) || !isfinite(top) ||
            !isfinite(right) || !isfinite(bottom)) return false;
        P->hit_rect[0] = left; P->hit_rect[1] = top;
        P->hit_rect[2] = right; P->hit_rect[3] = bottom;
        return true;
    }
    return P->hx >= x && P->hx < x + w && P->hy >= y && P->hy < y + h &&
           P->hx + P->ox >= P->c->cx0 && P->hx + P->ox < P->c->cx1 &&
           P->hy + P->oy >= P->c->cy0 && P->hy + P->oy < P->c->cy1;
}

static int control_hit(box_t *b) {
    if(b->node && b->node->tag==T_button)
        return web_control_submit_button(b->node)?WEB_HIT_SUBMIT:WEB_HIT_BUTTON;
    switch (b->atomic) {
    case AT_INPUT: return web_input_type(b->node) == WEB_INPUT_FILE ? WEB_HIT_FILE : WEB_HIT_TEXT_INPUT;
    case AT_TEXTAREA: return WEB_HIT_TEXTAREA;
    case AT_CHECKBOX: return WEB_HIT_CHECKBOX;
    case AT_RADIO: return WEB_HIT_RADIO;
    case AT_SELECT: return WEB_HIT_SELECT;
    case AT_BUTTON_INPUT: {
        return web_control_submit_button(b->node) ? WEB_HIT_SUBMIT : WEB_HIT_BUTTON;
    }
    case AT_IMG:
        if (b->node && b->node->tag == T_input) return WEB_HIT_SUBMIT; /* <input type=image> */
        return WEB_HIT_NONE;
    case AT_INLINE_BLOCK:
        if (b->node && b->node->tag == T_button) {
            return web_control_submit_button(b->node) ? WEB_HIT_SUBMIT : WEB_HIT_BUTTON;
        }
    }
    return WEB_HIT_NONE;
}

/* Test each painted box/run, never prune a none subtree: descendants may
   explicitly restore auto, including inside flattened slots and pseudos. */
static bool hit_style(const style_t *st) {
    return st && !st->visibility && !st->pointer_events;
}

static bool hit_target(struct pctx *P,node_t *target) {
    box_t *box=target?target->box:NULL;
    const struct radius_cache *radius=P->hit_radius?P->hit_radius:box?box->radius_clip[0]:NULL;
    if(!P->index_build&&radius&&corners_coverage(P->hx,P->hy,P->hit_shape_x,P->hit_shape_y,radius->w,radius->h,&radius->radii)<.5f)return false;
    P->target=target;
    P->hit_any=false;
    memset(P->hit,0,sizeof *P->hit);
    if (P->index_build && !P->failed) {
        struct web_hit_index *index = P->index_build;
        if (index->count == index->capacity) {
            size_t capacity = index->capacity ? index->capacity * 2 : 128;
            if (capacity <= index->capacity || capacity > SIZE_MAX / sizeof *index->items) { P->failed = true; return false; }
            void *items = realloc(index->items, capacity * sizeof *index->items);
            if (!items) { P->failed = true; return false; }
            index->items = items; index->capacity = capacity;
        }
        struct hit_primitive *item = &index->items[index->count];
        *item = (struct hit_primitive){.target=target,.order=index->count,.kind=WEB_HIT_NONE};
        item->box=box;item->radius=radius;item->radius_x=P->hit_shape_x;item->radius_y=P->hit_shape_y;
        memcpy(item->rect,P->hit_rect,sizeof item->rect);
        index->count++;
        if (!(index->count & 63) && !web_native_checkpoint(P->d)) P->failed=true;
    }
    return !P->failed;
}
static void set_hit(struct pctx *P, int kind, node_t *n, node_t *link) {
    if (n) {if(!hit_target(P,n))return;}
    else if (link && P->index_build) hit_target(P,P->target);
    P->hit_any = true;
    P->hit->kind = kind;
    P->hit->node = n;
    P->hit->href = NULL;
    if (link) {
        if (!P->index_build) P->hit->href = doc_link_href(P->d, link);
        if (kind == WEB_HIT_NONE) {
            P->hit->kind = WEB_HIT_LINK;
            P->hit->node = link;
        }
    }
    if (P->index_build && P->index_build->count) {
        struct hit_primitive *item = &P->index_build->items[P->index_build->count-1];
        item->kind = P->hit->kind; item->node = P->hit->node;
    }
}

static void set_disclosure_hit(struct pctx *P, node_t *details) {
    /* The action belongs to details, but the event target stays the summary
       (or its actual child), not a renderer-invented summary DOM node. */
    node_t *target = P->target;
    set_hit(P, WEB_HIT_DETAILS, details, NULL);
    P->target = target;
    if (P->index_build && P->index_build->count)
        P->index_build->items[P->index_build->count-1].target = target;
}

static void finish_disclosure_hit(struct pctx *P) {
    if (P->hit->kind != WEB_HIT_NONE && P->hit->kind != WEB_HIT_DETAILS) return;
    node_t *details = doc_details_activation(P->target);
    if (details) set_disclosure_hit(P, details);
    else if (P->hit->kind == WEB_HIT_DETAILS && P->target != P->hit->node) {
        /* A disabled control, label or other interactive descendant does not
           accidentally inherit its summary's activation. */
        P->hit->kind = WEB_HIT_NONE;
        P->hit->node = NULL;
        P->hit_any = false;
    }
}

/* ---------------------------------------------------------------- the tree walk */
static void paint_box(struct pctx *P, box_t *b, bool layer_root);
static void paint_stacked_layer(struct pctx *P, box_t *l);
static int layer_z(const box_t *b) {
    /* A static opacity context's author z-index is still inapplicable unless
       it is a flex/grid item. Sticky z-index remains applicable. */
    return b->st->z_auto || (!positioned(b) && b->st->position != POS_STICKY) ? 0 : b->st->z_index;
}

static bool layer_push(pvec *layers, box_t *box) {
    if (layers->n == layers->cap) {
        int capacity = layers->cap ? layers->cap > INT_MAX / 2 ? INT_MAX : layers->cap * 2 : 16;
        if (capacity <= layers->cap || (size_t)capacity > SIZE_MAX / sizeof *layers->v) return false;
        void **v = realloc(layers->v, (size_t)capacity * sizeof *v);
        if (!v) return false;
        layers->v = v; layers->cap = capacity;
    }
    layers->v[layers->n++] = box;
    return true;
}
static bool collect_layers(web_doc *d, pvec *layers, box_t *b) {
    for (box_t *c = b->first; c; c = c->next) {
        if(!web_native_checkpoint(d))return false;
        if (c->clamp_hidden || (c->st && c->st->display == D_NONE) || web_dialog_layer_box(d,c)) continue;
        if (paint_box_layer(c) && !layer_push(layers, c)) return false;
        if (!stacking_context(c) && !collect_layers(d, layers, c)) return false;
    }
    return true;
}
static bool sort_layers(web_doc *d,pvec *layers) {
    if (layers->n < 2) return true;
    size_t count=(size_t)layers->n;
    void **scratch=malloc(count*sizeof *scratch);
    if(!scratch)return false;
    unsigned work=0;
    for(size_t width=1;width<count;){
        for(size_t first=0;first<count;first+=MIN(width*2,count-first)){
            size_t middle=first+MIN(width,count-first),end=middle+MIN(width,count-middle);
            size_t left=first,right=middle,out=first;
            while(left<middle||right<end){
                if(!(++work&255)&&!web_native_checkpoint(d)){free(scratch);return false;}
                scratch[out++]=right==end||(left<middle&&layer_z(layers->v[left])<=layer_z(layers->v[right]))?
                    layers->v[left++]:layers->v[right++];
            }
        }
        memcpy(layers->v,scratch,count*sizeof *scratch);
        if(width>count/2)break;
        width*=2;
    }
    free(scratch);return true;
}
static bool prepare_layer_cache(web_doc *d,box_t *b){
    if(b->paint_layers_valid&&b->paint_layers_layout==d->layout_revision&&b->paint_layers_style==d->style_revision)return true;
    b->paint_layers_valid=false;b->paint_layers.n=0;
    if(d->profile_enabled)paint_profile.layer_builds++;
    if(!collect_layers(d,&b->paint_layers,b)||!sort_layers(d,&b->paint_layers)){b->paint_layers.n=0;return false;}
    b->paint_layers_layout=d->layout_revision;b->paint_layers_style=d->style_revision;b->paint_layers_valid=true;return true;
}

static const char overflow_ellipsis[] = "\xe2\x80\xa6";

static size_t ellipsis_next(web_doc *d,const char *s,size_t n,size_t at,bool *ok) {
    wf_grapheme_state state={0};unsigned work=0;
    for(size_t i=at;i<n;) {
        uint32_t cp;size_t used=wf_grapheme_decode(s+i,n-i,&cp);
        if(wf_grapheme_break(&state,cp)&&i>at)return i;
        i+=used;
        if(!(++work&63)&&!web_native_checkpoint(d)){*ok=false;return n;}
        if(i==n)return n;
    }
    return n;
}

/* Find a complete-grapheme prefix using the same font/spacing measurement as
   layout. This is cached per committed line/scroll position, not per paint.
   Checkpoints keep a very long unbroken run cancelable without a clock syscall
   for each character. No logical run is shortened. */
static int ellipsis_prefix(web_doc *d,const struct run *r,float available,float *width,bool *ok) {
    size_t at=0,count=0;
    while(at<(size_t)r->n) {
        at=ellipsis_next(d,r->s,(size_t)r->n,at,ok);if(!*ok)return 0;count++;
        if(!(count&63)&&!web_native_checkpoint(d)){*ok=false;return 0;}
    }
    size_t low=0,high=count;
    while(low<high) {
        size_t middle=low+(high-low+1)/2,bytes=0;
        for(size_t i=0;i<middle;i++) {
            bytes=ellipsis_next(d,r->s,(size_t)r->n,bytes,ok);if(!*ok)return 0;
            if(!(i&63)&&!web_native_checkpoint(d)){*ok=false;return 0;}
        }
        float w=style_text_width(r->st,r->s,bytes);
        if(w<=available)low=middle;else high=middle-1;
    }
    at=0;for(size_t i=0;i<low;i++){at=ellipsis_next(d,r->s,(size_t)r->n,at,ok);if(!*ok)return 0;}
    *width=style_text_width(r->st,r->s,at);return (int)at;
}
struct overflow_boundary { int run,bytes; };
static void overflow_boundary_record(struct overflow_boundary point,struct overflow_boundary target,
                                      struct overflow_boundary *first,struct overflow_boundary *floor) {
    if(first->run<0)*first=point;
    if(point.run<target.run || (point.run==target.run&&point.bytes<=target.bytes))*floor=point;
}
/* Styled DOM text runs are not Unicode segmentation boundaries. Reconcile
   the measured prefix with the whole line so an accent, flag or ZWJ emoji
   cannot be separated just because a span changes its font/color. */
static bool ellipsis_line_boundary(web_doc *d,box_t *b,const struct line_fragment *line,
                                   int *last,int *bytes,float *width,float *x) {
    struct overflow_boundary target={*last,*bytes},first={-1,0},floor={-1,0};
    wf_grapheme_state state={0};bool seen=false;unsigned work=0;
    for(int i=line->first_run;i<line->end_run;i++) {
        struct run *r=&b->runs[i];
        if(r->atomic) {
            if(seen&&i>line->first_run)overflow_boundary_record(
                (struct overflow_boundary){i-1,b->runs[i-1].n},target,&first,&floor);
            overflow_boundary_record((struct overflow_boundary){i,0},target,&first,&floor);
            state=(wf_grapheme_state){0};seen=true;continue;
        }
        for(size_t at=0;at<(size_t)r->n;) {
            uint32_t cp;size_t used=wf_grapheme_decode(r->s+at,(size_t)r->n-at,&cp);
            if(wf_grapheme_break(&state,cp)&&seen) {
                struct overflow_boundary point=at?(struct overflow_boundary){i,(int)at}:
                    (struct overflow_boundary){i-1,b->runs[i-1].n};
                overflow_boundary_record(point,target,&first,&floor);
            }
            at+=used;seen=true;
            if(!(++work&63)&&!web_native_checkpoint(d))return false;
        }
    }
    if(seen)overflow_boundary_record((struct overflow_boundary){line->end_run-1,b->runs[line->end_run-1].n},target,&first,&floor);
    struct overflow_boundary point=floor.run>=0?floor:first;
    if(point.run<0)return true;
    *last=point.run;*bytes=point.bytes;
    struct run *r=&b->runs[*last];
    *width=r->atomic?r->w:style_text_width(r->st,r->s,(size_t)*bytes);
    *x=r->x+*width;return true;
}
static bool prepare_text_overflow(web_doc *d,box_t *b) {
    float scroll=box_element_scrollable(b)?(float)b->node->scroll_x:0;
    if(b->ellipsis_layout_revision==d->layout_revision && b->ellipsis_scroll_x==scroll)return true;
    b->ellipsis_layout_revision=0;
    for(int i=0;i<b->nruns;i++)if(b->runs[i].atomic)b->runs[i].atomic->ellipsis_hidden=false;
    bool enabled=b->st && b->st->text_overflow==TO_ELLIPSIS && b->st->overflow!=OV_VISIBLE &&
        !(b->anon&&b->parent&&(b->parent->kind==B_FLEX||b->parent->kind==B_GRID));
    float marker=enabled?style_text_width(b->st,overflow_ellipsis,sizeof overflow_ellipsis-1):0;
    for(int k=0;k<b->nlines;k++) {
        struct line_fragment *line=&b->lines[k];line->ellipsis_active=false;
        if(!enabled||line->end_run<=line->first_run)continue;
        float edge=line->right+scroll,extent=line->left;
        for(int i=line->first_run;i<line->end_run;i++)extent=fmaxf(extent,b->runs[i].x+b->runs[i].w);
        if(extent<=edge)continue;
        float cut=edge-marker,x=line->left;
        int last=line->first_run-1,bytes=0;float width=0;
        for(int i=line->first_run;i<line->end_run;i++) {
            if(!(i&63)&&!web_native_checkpoint(d))return false;
            struct run *r=&b->runs[i];
            if(r->x+r->w<=cut){last=i;bytes=r->n;width=r->w;x=r->x+r->w;continue;}
            if(r->atomic) {
                /* The first atomic on the line is clipped, never removed. */
                if(i==line->first_run){last=i;bytes=0;width=r->w;x=r->x+r->w;}
                break;
            }
            bool ok=true;
            float prefix_width=0;
            int prefix=ellipsis_prefix(d,r,cut-r->x,&prefix_width,&ok);if(!ok)return false;
            if(i==line->first_run && !prefix && r->n>0) {
                prefix=(int)ellipsis_next(d,r->s,(size_t)r->n,0,&ok);if(!ok)return false;
                prefix_width=style_text_width(r->st,r->s,(size_t)prefix);
            }
            if(prefix){last=i;bytes=prefix;width=prefix_width;x=r->x+width;}
            break;
        }
        if(!ellipsis_line_boundary(d,b,line,&last,&bytes,&width,&x))return false;
        line->ellipsis_active=true;line->ellipsis_run=last;line->ellipsis_bytes=bytes;
        line->ellipsis_run_width=width;line->ellipsis_x=x;line->ellipsis_width=marker;
        for(int i=last+1;i<line->end_run;i++)if(b->runs[i].atomic){
            b->runs[i].atomic->ellipsis_hidden=true;b->runs[i].atomic->ellipsis_hidden_revision=d->layout_revision;
        }
    }
    b->ellipsis_scroll_x=scroll;b->ellipsis_layout_revision=d->layout_revision;return true;
}
static bool prepare_paint_tree(web_doc *d,box_t *b){
    if(!b)return true;
    if(!web_native_checkpoint(d))return false;
    if(b->inline_ctx&&!prepare_text_overflow(d,b))return false;
    if((b==d->root_box||stacking_context(b)||web_dialog_layer_box(d,b)||
        (b->node&&b->node==d->fullscreen_element))&&!prepare_layer_cache(d,b))return false;
    if(b->st && b->kind!=B_INLINE && b->kind!=B_TEXT && b->kind!=B_BR) {
        float x=box_visual_x(b),y=box_visual_y(b);
        bool row=b->kind==B_ROW||b->kind==B_ROW_GROUP;float zero[4]={0};
        float left=row?x:x-b->p[3]-b->b[3],top=row?y:y-b->p[0]-b->b[0];
        float w=row?b->w:b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3],h=row?b->h:b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];
        if(!shadow_cache_prepare(d,&b->shadow_cache,b->st,left,top,w,h,row?zero:b->b))return false;
        struct border_radii radii=used_radii(b->st,w,h);const float *border=row?zero:b->b;
        if(!radius_cache_prepare(d,&b->radius_clip[0],left,top,w,h,radii))return false;
        float pw=w-border[1]-border[3],ph=h-border[0]-border[2];
        struct border_radii padding=inset_radii(radii,border,pw,ph);
        if(!radius_cache_prepare(d,&b->radius_clip[1],left+border[3],top+border[0],pw,ph,padding))return false;
        float edges[4];for(int i=0;i<4;i++)edges[i]=border[i]+b->p[i];
        struct border_radii content=inset_radii(radii,edges,b->w,b->h);
        if(b->kind==B_ATOMIC&&b->atomic!=AT_INLINE_BLOCK) {
            if(!radius_cache_prepare(d,&b->radius_clip[2],x,cy(b),b->w,b->h,content))return false;
        } else web_radius_cache_free(&b->radius_clip[2]);
    }
    float ix=box_visual_x(b),iy=cy(b);
    if(web_marquee_box(b)){ix+=b->node->marquee_x;iy+=b->node->marquee_y;}
    if(box_element_scrollable(b)){ix-=b->node->scroll_x;iy-=b->node->scroll_y;}
    for(int i=0;i<b->ndecos;i++) {
        struct deco *deco=&b->decos[i];float borders[4];memcpy(borders,deco->st->border_width,sizeof borders);
        if(!deco->first)borders[3]=0;if(!deco->last)borders[1]=0;
        struct border_radii radii=used_radii(deco->st,deco->w,deco->h);
        if(!deco->first)radii.x[0]=radii.y[0]=radii.x[3]=radii.y[3]=0;
        if(!deco->last)radii.x[1]=radii.y[1]=radii.x[2]=radii.y[2]=0;
        if(!shadow_cache_prepare_radii(d,&deco->shadow_cache,deco->st,ix+deco->x,iy+deco->y,deco->w,deco->h,borders,radii))return false;
        if(!radius_cache_prepare(d,&deco->radius_clip[0],ix+deco->x,iy+deco->y,deco->w,deco->h,radii))return false;
        float pw=deco->w-borders[1]-borders[3],ph=deco->h-borders[0]-borders[2];
        if(!radius_cache_prepare(d,&deco->radius_clip[1],ix+deco->x+borders[3],iy+deco->y+borders[0],pw,ph,inset_radii(radii,borders,pw,ph)))return false;
    }
    for(box_t *c=b->first;c;c=c->next)if(!prepare_paint_tree(d,c))return false;
    return true;
}
static bool prepare_paint_order(web_doc *d){
    if(d->paint_order_valid&&d->paint_order_layout==d->layout_revision&&
       d->paint_order_visual==d->visual_revision&&(!d->paint_shadow_crop||
       (d->paint_shadow_view_x==d->view_x&&d->paint_shadow_view_y==d->view_y)))return true;
    d->paint_order_valid=false;
    d->paint_shadow_crop=false;
    if(d->profile_enabled)paint_profile.order_builds++;
    if(!prepare_paint_tree(d,d->root_box))return false;
    d->paint_order_layout=d->layout_revision;d->paint_order_visual=d->visual_revision;
    d->paint_shadow_view_x=d->view_x;d->paint_shadow_view_y=d->view_y;d->paint_order_valid=true;return true;
}
static void release_layers(pvec *layers,bool borrowed){if(!borrowed)pv_free(layers);}

/* the atomic inlines and floats inside an inline formatting context, in tree order */
static void bounds_rect(struct pctx *P, float x, float y, float w, float h) {
    if (!(w > 0 && h > 0) || !isfinite(x) || !isfinite(y) || !isfinite(x + w) || !isfinite(y + h)) return;
    if (!P->bounds_valid) {
        P->bounds[0] = x; P->bounds[1] = y; P->bounds[2] = x + w; P->bounds[3] = y + h;
        P->bounds_valid = true;
    } else {
        if (x < P->bounds[0]) P->bounds[0] = x;
        if (y < P->bounds[1]) P->bounds[1] = y;
        if (x + w > P->bounds[2]) P->bounds[2] = x + w;
        if (y + h > P->bounds[3]) P->bounds[3] = y + h;
    }
}
static void shadow_ink_bounds(const style_t *style,float x,float y,float w,float h,float *rect) {
    for(const struct css_box_shadow *shadow=style->box_shadow;shadow;shadow=shadow->next) {
        if(shadow->inset || !(shadow->color>>24) || w+2*shadow->spread<=0 || h+2*shadow->spread<=0)continue;
        float extent=shadow->spread+ceilf(shadow->blur*1.5f)+1;
        rect[0]=fminf(rect[0],x+shadow->x-extent);rect[1]=fminf(rect[1],y+shadow->y-extent);
        rect[2]=fmaxf(rect[2],x+w+shadow->x+extent);rect[3]=fmaxf(rect[3],y+h+shadow->y+extent);
    }
    if(style->outline_width>0&&(style->outline_color>>24)) {
        float extent=fmaxf(style->outline_offset+style->outline_width,0)+1;
        rect[0]=fminf(rect[0],x-extent);rect[1]=fminf(rect[1],y-extent);
        rect[2]=fmaxf(rect[2],x+w+extent);rect[3]=fmaxf(rect[3],y+h+extent);
    }
}
static void bounds_decoration(struct pctx *P,const style_t *style,float x,float y,float w,float h) {
    float rect[4]={x,y,x+w,y+h};shadow_ink_bounds(style,x,y,w,h,rect);
    bounds_rect(P,rect[0],rect[1],rect[2]-rect[0],rect[3]-rect[1]);
}
/* The outline is drawn over the content, outside the border box by outline-offset. */
static void paint_outline(canvas_t *c,const style_t *st,float x,float y,float w,float h) {
    float ow=st->outline_width;
    if(ow<=0||!(st->outline_color>>24))return;
    float grow=st->outline_offset+ow;
    x-=grow;y-=grow;w+=2*grow;h+=2*grow;
    if(w<=0||h<=0)return;
    if(ow*2>h)ow=h/2;
    float side=fminf(ow,w/2);
    edge(c,st->outline_style,0,x,y,w,ow,st->outline_color);
    edge(c,st->outline_style,2,x,y+h-ow,w,ow,st->outline_color);
    edge(c,st->outline_style,3,x,y+ow,side,h-2*ow,st->outline_color);
    edge(c,st->outline_style,1,x+w-side,y+ow,side,h-2*ow,st->outline_color);
}
static void inline_children(struct pctx *P, box_t *b, bool floats) {
    for (box_t *c = b->first; c; c = c->next) {
        if (c->clamp_hidden) continue;
        if (P->mode != M_BOUNDS && paint_box_layer(c)) continue;
        if (c->kind == B_INLINE) {
            if (P->mode == M_BOUNDS && floats) paint_box(P, c, true);
            else if (P->mode != M_BOUNDS) inline_children(P, c, floats);
            continue;
        }
        if (c->kind == B_TEXT || c->kind == B_BR) continue;
        if (c->floated == floats) paint_box(P, c, false);
    }
}

static struct line_fragment *overflow_line(box_t *b,int run) {
    int low=0,high=b->nlines;
    while(low<high){int mid=low+(high-low)/2;if(b->lines[mid].end_run<=run)low=mid+1;else high=mid;}
    return low<b->nlines && b->lines[low].first_run<=run && b->lines[low].ellipsis_active?&b->lines[low]:NULL;
}
static void paint_runs(struct pctx *P, box_t *b, box_t *scope) {
    float bx = box_visual_x(b), by = cy(b);
    if(web_marquee_box(b)){bx+=b->node->marquee_x;by+=b->node->marquee_y;}
    if (box_element_scrollable(b)) {
        bx -= (float)b->node->scroll_x;
        by -= (float)b->node->scroll_y;
    }
    /* inline box backgrounds */
    for (int i = scope ? scope->inline_deco_first : 0; i >= 0 && i < b->ndecos;
         i = scope ? b->decos[i].paint_next : i + 1) {
        struct deco *d = &b->decos[i];
        if (scope && d->paint_owner != scope) continue;
        if (!scope && P->mode != M_BOUNDS && d->paint_owner) continue;
        const style_t *st = d->st;
        if (st->visibility) continue;
        if (P->mode == M_BOUNDS) { bounds_decoration(P,st,bx + d->x, by + d->y, d->w, d->h); continue; }
        if (P->mode == M_HIT) {
            P->hit_radius=d->radius_clip[0];
            if (hit_style(st) && d->node && inside(P, bx + d->x, by + d->y, d->w, d->h)) hit_target(P,d->node);
            P->hit_radius=NULL;
            continue;
        }
        float l = d->first ? 0 : 0;
        (void)l;
        float bw[4] = {st->border_width[0], st->border_width[1], st->border_width[2], st->border_width[3]};
        if (!d->first) bw[3] = 0;
        if (!d->last) bw[1] = 0;
        float x = P->ox + bx + d->x, y = P->oy + by + d->y;
        paint_bg_border(P, st, x, y, d->w, d->h, bw, false, !d->first, !d->last,d->shadow_cache,d->radius_clip);
    }
    for (int i = scope ? scope->inline_run_first : 0; i >= 0 && i < b->nruns;
         i = scope ? b->runs[i].paint_next : i + 1) {
        struct run *r = &b->runs[i];
        if (scope && r->paint_owner != scope) continue;
        if (!scope && P->mode != M_BOUNDS && r->paint_owner) continue;
        if (r->atomic) {
            if (P->mode == M_HIT) {
                box_t *a = r->atomic;
                if (!hit_style(a->st)) continue;
                float ax = box_visual_x(a) - a->p[3] - a->b[3], ay = cy(a) - a->p[0] - a->b[0];
                if (inside(P, ax, ay, a->w + a->p[1] + a->p[3] + a->b[1] + a->b[3], a->h + a->p[0] + a->p[2] + a->b[0] + a->b[2]))
                    set_hit(P, control_hit(a), a->node, r->link);
            }
            continue;
        }
        if (P->mode == M_HIT) {
            if (!hit_style(r->st)) continue;
            wfont f = style_font(r->st);
            float asc, desc;
            wf_metrics(&f, &asc, &desc);
            if (inside(P, bx + r->x, by + r->y - asc, r->w, asc + desc)) {
                if (r->node) hit_target(P,r->node->type == N_ELEM ? r->node : doc_flat_parent(r->node));
                if (r->link) set_hit(P, WEB_HIT_NONE, NULL, r->link);
            }
            continue;
        }
        if (P->mode == M_BOUNDS) {
            if (r->st->visibility) continue;
            wfont f = style_font(r->st); float asc, desc;
            wf_metrics(&f, &asc, &desc);
            /* Include glyph overhang/decoration coverage, not only advance. */
            bounds_rect(P, bx + r->x - r->st->font_size, by + r->y - asc - 1,
                r->w + 2 * r->st->font_size, asc + desc + 2);
        } else {
            struct line_fragment *line=overflow_line(b,i);
            if(line && i>line->ellipsis_run)continue;
            if(line && i==line->ellipsis_run && line->ellipsis_bytes<r->n) {
                struct run visible=*r;visible.n=line->ellipsis_bytes;visible.w=line->ellipsis_run_width;
                canvas_t saved=*P->c;
                int right=(int)floorf(P->ox+bx+line->ellipsis_x);
                if(right<P->c->cx1)P->c->cx1=MAX(right,P->c->cx0);
                if(visible.n)paint_run_text(P,&visible,bx,by);
                *P->c=saved;
            } else paint_run_text(P,r,bx,by);
        }
    }
    if(P->mode==M_PAINT&&!scope)for(int k=0;k<b->nlines;k++) {
        struct line_fragment *line=&b->lines[k];if(!line->ellipsis_active)continue;
        struct run marker={.x=line->ellipsis_x,.y=line->baseline,.w=line->ellipsis_width,
            .s=overflow_ellipsis,.n=sizeof overflow_ellipsis-1,.st=b->st};
        paint_run_text(P,&marker,bx,by);
    }
    /* inline box outlines, over the text like block outlines */
    if (P->mode == M_PAINT)
        for (int i = scope ? scope->inline_deco_first : 0; i >= 0 && i < b->ndecos;
             i = scope ? b->decos[i].paint_next : i + 1) {
            struct deco *d = &b->decos[i];
            if (scope && d->paint_owner != scope) continue;
            if (!scope && d->paint_owner) continue;
            if (d->st->visibility || d->st->outline_width <= 0) continue;
            paint_outline(P->c, d->st, P->ox + bx + d->x, P->oy + by + d->y, d->w, d->h);
        }
}

/* ---------------------------------------------------------------- 2D transforms */
static void paint_box(struct pctx *P, box_t *b, bool layer_root);
static void affine_rect(const double m[6], double x0, double y0, double x1, double y1, double out[4]) {
    double xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
    out[0] = out[1] = INFINITY; out[2] = out[3] = -INFINITY;
    for (int i = 0; i < 4; i++) {
        double x = m[0] * xs[i] + m[2] * ys[i] + m[4], y = m[1] * xs[i] + m[3] * ys[i] + m[5];
        out[0] = fmin(out[0], x); out[1] = fmin(out[1], y); out[2] = fmax(out[2], x); out[3] = fmax(out[3], y);
    }
}
static bool affine_invert(const double m[6], double inv[6]) {
    double det = m[0] * m[3] - m[1] * m[2];
    if (!(fabs(det) > 1e-12) || !isfinite(det)) return false;
    inv[0] = m[3] / det; inv[1] = -m[1] / det; inv[2] = -m[2] / det; inv[3] = m[0] / det;
    inv[4] = -(inv[0] * m[4] + inv[2] * m[5]); inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
    return true;
}
/* Untransformed document-space ink of b's subtree, with a private bounds
   generation so cached descendant bounds from another space never leak in. */
static bool transformed_inner_bounds(struct pctx *P, box_t *b, bool layer_root, double out[4]) {
    struct pctx Q = *P;
    Q.mode = M_BOUNDS; Q.bounds_valid = false; Q.untransformed = b;
    if (P->mode != M_BOUNDS) Q.bounds_generation = __atomic_add_fetch(&P->d->paint_bounds_generation, 1, __ATOMIC_RELAXED);
    paint_box(&Q, b, layer_root);
    if (P->mode == M_BOUNDS) P->bounds_work = Q.bounds_work;
    if (Q.failed) { P->failed = true; return false; }
    if (!Q.bounds_valid) return false;
    for (int i = 0; i < 4; i++) out[i] = Q.bounds[i];
    return true;
}
/* Paint into two opaque scratch canvases (over black and over white). The
   shared 2D library blends onto opaque destinations only; the difference of
   the two results recovers exact coverage, and the black result is already
   the premultiplied colour. The surface is then resampled through the
   inverse matrix with bilinear filtering, which also antialiases its edges. */
static void paint_transformed_surface(struct pctx *P, box_t *b, bool layer_root, const double m[6]) {
    canvas_t *c = P->c;
    double inv[6], inner[4], ink[4];
    if (!affine_invert(m, inv)) return; /* a singular matrix has no visible area */
    if (!transformed_inner_bounds(P, b, layer_root, inner)) return;
    affine_rect(m, inner[0], inner[1], inner[2], inner[3], ink);
    double left = floor(ink[0] + P->ox), top = floor(ink[1] + P->oy);
    double right = ceil(ink[2] + P->ox), bottom = ceil(ink[3] + P->oy);
    if (left < c->cx0) left = c->cx0; if (top < c->cy0) top = c->cy0;
    if (right > c->cx1) right = c->cx1; if (bottom > c->cy1) bottom = c->cy1;
    if (!(right > left && bottom > top)) return;
    int dx = (int)left, dy = (int)top, dw = (int)(right - left), dh = (int)(bottom - top);
    /* Only the part of the subtree that reaches the destination is rastered. */
    double need[4];
    affine_rect(inv, left - P->ox, top - P->oy, right - P->ox, bottom - P->oy, need);
    double sx0 = floor(fmax(need[0], inner[0]) - 1), sy0 = floor(fmax(need[1], inner[1]) - 1);
    double sx1 = ceil(fmin(need[2], inner[2]) + 1), sy1 = ceil(fmin(need[3], inner[3]) + 1);
    if (!(sx1 > sx0 && sy1 > sy0)) return;
    double area = (sx1 - sx0) * (sy1 - sy0);
    if (!(area <= 8.0 * 1024 * 1024)) {
        /* Never allocate an unbounded surface; draw the content in place. */
        struct pctx Q = *P; Q.untransformed = b; paint_box(&Q, b, layer_root);
        if (Q.failed) P->failed = true;
        return;
    }
    int sw = (int)(sx1 - sx0), sh = (int)(sy1 - sy0);
    size_t count = (size_t)sw * (size_t)sh;
    uint32_t *black = malloc(count * sizeof *black), *white = malloc(count * sizeof *white);
    if (!black || !white) { free(black); free(white); P->failed = true; return; }
    web_avmedia_snapshot_reject_region(c, dx, dy, dw, dh);
    if (P->d->profile_enabled) { paint_profile.groups++; paint_profile.group_pixels += count; }
    for (int pass = 0; pass < 2 && !P->failed; pass++) {
        canvas_t surface;
        gfx_init(&surface, pass ? white : black, sw, sh, sw);
        gfx_fill(&surface, 0, 0, sw, sh, pass ? RGB(255, 255, 255) : RGB(0, 0, 0));
        struct pctx Q = *P;
        Q.c = &surface; Q.ox = (float)-sx0; Q.oy = (float)-sy0;
        Q.vx0 = 0; Q.vy0 = 0; Q.vx1 = sw; Q.vy1 = sh; Q.viewport_mask = NULL;
        Q.untransformed = b;
        paint_box(&Q, b, layer_root);
        if (Q.failed) P->failed = true;
    }
    if (P->failed) { free(black); free(white); return; }
    for (size_t i = 0; i < count; i++) {
        uint32_t k = black[i], w = white[i];
        int alpha = 0;
        for (int shift = 0; shift < 24; shift += 8) alpha += 255 - ((int)((w >> shift) & 255) - (int)((k >> shift) & 255));
        alpha = (alpha + 1) / 3; if (alpha < 0) alpha = 0; if (alpha > 255) alpha = 255;
        uint32_t r = (k >> 16) & 255, g = (k >> 8) & 255, bl = k & 255;
        if (r > (uint32_t)alpha) r = (uint32_t)alpha; if (g > (uint32_t)alpha) g = (uint32_t)alpha; if (bl > (uint32_t)alpha) bl = (uint32_t)alpha;
        black[i] = (uint32_t)alpha << 24 | r << 16 | g << 8 | bl;
    }
    free(white);
    for (int j = 0; j < dh && !P->failed; j++) {
        if (!(j & 63) && !web_native_checkpoint(P->d)) { P->failed = true; break; }
        int py = dy + j;
        double X = dx + 0.5 - P->ox, Y = py + 0.5 - P->oy;
        double u = inv[0] * X + inv[2] * Y + inv[4] - sx0 - 0.5, v = inv[1] * X + inv[3] * Y + inv[5] - sy0 - 0.5;
        uint32_t *row = c->px + (size_t)py * c->pitch;
        for (int i = 0; i < dw; i++, u += inv[0], v += inv[1]) {
            if (!(u > -1 && v > -1 && u < sw && v < sh)) continue;
            int x0 = (int)floor(u), y0 = (int)floor(v);
            uint32_t fx = (uint32_t)((u - x0) * 256), fy = (uint32_t)((v - y0) * 256);
            uint32_t lanes[4] = {0, 0, 0, 0};
            for (int corner = 0; corner < 4; corner++) {
                int sx = x0 + (corner & 1), sy = y0 + (corner >> 1);
                if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
                uint32_t weight = ((corner & 1) ? fx : 256 - fx) * ((corner >> 1) ? fy : 256 - fy);
                uint32_t s = black[(size_t)sy * sw + sx];
                for (int lane = 0; lane < 4; lane++) lanes[lane] += ((s >> (lane * 8)) & 255) * weight;
            }
            uint32_t a = (lanes[3] + 32768) >> 16;
            if (!a) continue;
            int px = dx + i;
            uint32_t mask = c->mask_clip ? gfx_clip_alpha(c, px, py) : 255;
            if (!mask) continue;
            uint32_t d = row[px], out = 0xFF000000u;
            for (int lane = 0; lane < 3; lane++) {
                uint32_t sv = (lanes[lane] + 32768) >> 16, dv = (d >> (lane * 8)) & 255;
                uint32_t value = (sv * 255 + dv * (255 - a) + 127) / 255; /* premultiplied source over opaque dest */
                if (mask < 255) value = (value * mask + dv * (255 - mask) + 127) / 255;
                out |= (value > 255 ? 255 : value) << (lane * 8);
            }
            row[px] = out;
        }
    }
    free(black);
}
static void paint_transformed(struct pctx *P, box_t *b, bool layer_root) {
    double m[6];
    if (!box_transform_matrix(b, m)) {
        struct pctx Q = *P; Q.untransformed = b; paint_box(&Q, b, layer_root);
        P->target = Q.target; P->hit_any = Q.hit_any; P->bounds_valid = Q.bounds_valid;
        memcpy(P->bounds, Q.bounds, sizeof P->bounds); P->bounds_work = Q.bounds_work;
        if (Q.failed) P->failed = true;
        return;
    }
    if (P->mode == M_BOUNDS) {
        double inner[4], ink[4];
        if (!transformed_inner_bounds(P, b, layer_root, inner)) return;
        affine_rect(m, inner[0], inner[1], inner[2], inner[3], ink);
        bounds_rect(P, (float)ink[0], (float)ink[1], (float)(ink[2] - ink[0]), (float)(ink[3] - ink[1]));
        return;
    }
    if (P->mode == M_PAINT) { paint_transformed_surface(P, b, layer_root, m); return; }
    canvas_t *c = P->c;
    if (P->index_build) {
        double inner[4], ink[4];
        if (!transformed_inner_bounds(P, b, layer_root, inner)) return;
        affine_rect(m, inner[0], inner[1], inner[2], inner[3], ink);
        double l = fmax(ink[0], c->cx0 - P->ox), t = fmax(ink[1], c->cy0 - P->oy);
        double r = fmin(ink[2], c->cx1 - P->ox), bt = fmin(ink[3], c->cy1 - P->oy);
        if (!(r > l && bt > t)) return;
        struct web_hit_index *index = P->index_build;
        if (index->region_count == index->region_capacity) {
            size_t capacity = index->region_capacity ? index->region_capacity * 2 : 8;
            void *regions = capacity > SIZE_MAX / sizeof *index->regions ? NULL : realloc(index->regions, capacity * sizeof *index->regions);
            if (!regions) { P->failed = true; return; }
            index->regions = regions; index->region_capacity = capacity;
        }
        float *region = index->regions[index->region_count++];
        region[0] = (float)l; region[1] = (float)t; region[2] = (float)r; region[3] = (float)bt;
        return;
    }
    /* Exact hit walk: the point must reach this box through the outer clip;
       the subtree is then tested in its own untransformed space. */
    if (P->hx + P->ox < c->cx0 || P->hx + P->ox >= c->cx1 || P->hy + P->oy < c->cy0 || P->hy + P->oy >= c->cy1) return;
    double inv[6];
    if (!affine_invert(m, inv)) return;
    float hx = P->hx, hy = P->hy;
    int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
    const box_t *outer = P->untransformed;
    P->hx = (float)(inv[0] * hx + inv[2] * hy + inv[4]);
    P->hy = (float)(inv[1] * hx + inv[3] * hy + inv[5]);
    c->cx0 = c->cy0 = -1000000000; c->cx1 = c->cy1 = 1000000000;
    P->untransformed = b;
    paint_box(P, b, layer_root);
    P->untransformed = outer; P->hx = hx; P->hy = hy;
    c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
}

static void paint_box(struct pctx *P, box_t *b, bool layer_root) {
    if (P->failed) return;
    if (P->mode==M_PAINT && b->ellipsis_hidden && b->ellipsis_hidden_revision==P->d->layout_revision) return;
    if (b->clamp_hidden || !b->st || b->st->display == D_NONE) return;
    if (web_dialog_layer_box(P->d,b) && b != P->top_root) return;
    if (P->mode == M_HIT && b->node && web_dialog_inert(P->d,b->node)) return;
    if (P->mode != M_BOUNDS && paint_box_layer(b) && !layer_root) return; /* painted with the layers */
    const style_t *st = b->st;
    float group_opacity = st->opacity * (st->filter ? st->filter->opacity : 1);
    /* Transparent auto boxes still participate in hit testing. */
    if (P->mode != M_HIT && group_opacity <= 0) return;
    if (P->untransformed != b && box_transformed(b)) { paint_transformed(P, b, layer_root); return; }
    float parent_bounds[4]; bool parent_valid = P->bounds_valid;
    if (P->mode == M_BOUNDS) {
        if (!(++P->bounds_work & 63) && !web_native_checkpoint(P->d)) { P->failed = true; return; }
        if (b->paint_bounds_generation == P->bounds_generation && P->untransformed != b) {
            if (b->paint_bounds_valid) bounds_rect(P, b->paint_bounds[0], b->paint_bounds[1],
                b->paint_bounds[2] - b->paint_bounds[0], b->paint_bounds[3] - b->paint_bounds[1]);
            return;
        }
        memcpy(parent_bounds, P->bounds, sizeof parent_bounds); P->bounds_valid = false;
    }
    pvec layers = {0}; bool layers_borrowed=false;
    if (P->mode != M_BOUNDS && (b == P->d->root_box || b == P->top_root || stacking_context(b))) {
        if(!b->paint_layers_valid||b->paint_layers_layout!=P->d->layout_revision||b->paint_layers_style!=P->d->style_revision){
            b->paint_layers_valid=false;b->paint_layers.n=0;
            if(!collect_layers(P->d,&b->paint_layers,b)||!sort_layers(P->d,&b->paint_layers)){
                b->paint_layers.n=0;P->failed=true;return;
            }
            b->paint_layers_layout=P->d->layout_revision;b->paint_layers_style=P->d->style_revision;
            b->paint_layers_valid=true;
        }
        layers=b->paint_layers;layers_borrowed=true;
    }
    canvas_t *c = P->c;
    float x = box_visual_x(b), y = box_visual_y(b);
    float bx = x - b->p[3] - b->b[3], by = y - b->p[0] - b->b[0];
    float bw = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
    float bh = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
    bool canvas_group = P->canvas_group && b->node && b->node == P->d->html;
    if (P->mode == M_BOUNDS && canvas_group)
        bounds_rect(P, P->vx0 - P->ox, P->vy0 - P->oy, P->vx1 - P->vx0, P->vy1 - P->vy0);
    if (P->mode == M_BOUNDS && !st->visibility && b->kind != B_INLINE && b->kind != B_TEXT && b->kind != B_BR) {
        bounds_decoration(P,st,bx,by,bw,bh);
        if (b->marker || b->marker_shape) bounds_rect(P, bx - st->font_size * 2, by, st->font_size * 2, bh);
    }
    if (P->mode == M_HIT && hit_style(st) && b->node &&
        b->kind != B_TEXT && inside(P, bx, by, bw, bh))
        hit_target(P,b->node->type == N_ELEM ? b->node : doc_flat_parent(b->node));
    if (P->mode == M_HIT && hit_style(st) && b->node && !b->node->foreign &&
        b->kind != B_TEXT && (inside(P, bx, by, bw, bh) ||
        ((b->marker || b->marker_shape) && inside(P, bx - st->font_size * 2, by, st->font_size * 2, bh)))) {
        node_t *details = b->node->tag == T_summary ? doc_details_activation(b->node) :
                          b->anon && b->node->tag == T_details && !doc_details_summary(b->node) ? b->node : NULL;
        if (details) {
            hit_target(P,b->node);
            set_disclosure_hit(P, details);
        }
    }
    if(P->index_build&&hit_style(st)&&b->node&&!b->node->foreign&&(b->marker||b->marker_shape)&&
       inside(P,bx-st->font_size*2,by,st->font_size*2,bh)){
        node_t *details=b->node->tag==T_summary?doc_details_activation(b->node):
            b->anon&&b->node->tag==T_details&&!doc_details_summary(b->node)?b->node:NULL;
        if(details){hit_target(P,b->node);set_disclosure_hit(P,details);}
    }
    bool clip = (web_marquee_box(b) || b->clamp_truncated || st->overflow != OV_VISIBLE) && b->kind != B_INLINE && b->parent && !doc_viewport_overflow_box(P->d,b);
    /* Off-screen contexts can still contain viewport-fixed descendants. */
    if (P->mode != M_BOUNDS && clip && !layers.n && (P->oy + by > c->cy1 || P->oy + by + bh < c->cy0)) { release_layers(&layers,layers_borrowed); return; }
    /* group opacity: paint, then blend the result with what was there */
    uint32_t *saved = NULL;
    int gx = 0, gy = 0, gw = 0, gh = 0;
    if (P->mode == M_PAINT && group_opacity < 1 && b->kind != B_TEXT) {
        struct pctx measure = *P; measure.mode = M_BOUNDS; measure.bounds_valid = false;
        paint_box(&measure, b, true);
        if (measure.failed) { P->failed = true; release_layers(&layers,layers_borrowed); return; }
        if (measure.bounds_valid) {
            double left = floor((double)P->ox + measure.bounds[0]), top = floor((double)P->oy + measure.bounds[1]);
            double right = ceil((double)P->ox + measure.bounds[2]), bottom = ceil((double)P->oy + measure.bounds[3]);
            if (left < P->vx0) left = P->vx0; if (top < P->vy0) top = P->vy0;
            if (right > P->vx1) right = P->vx1; if (bottom > P->vy1) bottom = P->vy1;
            if (right > left && bottom > top) {
                gx = (int)left; gy = (int)top; gw = (int)(right - left); gh = (int)(bottom - top);
                web_avmedia_snapshot_reject_region(c,gx,gy,gw,gh);
                if(P->d->profile_enabled){paint_profile.groups++;paint_profile.group_pixels+=(uint64_t)gw*(uint64_t)gh;}
                if ((size_t)gw > SIZE_MAX / sizeof *saved / (size_t)gh ||
                    !(saved = malloc(sizeof *saved * (size_t)gw * (size_t)gh))) {
                    P->failed = true; release_layers(&layers,layers_borrowed); return; /* never paint an unfiltered substitute */
                }
                for (int j = 0; j < gh; j++)
                    memcpy(saved + (size_t)j * gw, c->px + (size_t)(gy + j) * c->pitch + gx, sizeof *saved * (size_t)gw);
            }
        }
    }
    if (P->mode == M_PAINT && canvas_group)
        gfx_fill_blend(c, P->vx0, P->vy0, P->vx1 - P->vx0, P->vy1 - P->vy0, P->canvas_bg);
    bool visible = !st->visibility;
    if (b->kind != B_INLINE && b->kind != B_TEXT && b->kind != B_BR && b->kind != B_ROW && b->kind != B_ROW_GROUP) {
        if (P->mode == M_PAINT && visible) {
            bool skip_bg = (P->canvas_bg_from == 1 && b->node == P->d->html && b->node) ||
                           (P->canvas_bg_from == 2 && b->node == P->d->body && b->node);
            paint_bg_border(P, st, P->ox + bx, P->oy + by, bw, bh, b->b, skip_bg, false, false,b->shadow_cache,b->radius_clip);
        } else if (P->mode == M_HIT && hit_style(st) && st->content_visibility!=CV_HIDDEN &&
                   (b->kind == B_ATOMIC || (b->node&&b->node->tag==T_button)) && inside(P, bx, by, bw, bh)) {
            int k = control_hit(b);
            if (k != WEB_HIT_NONE) set_hit(P, k, b->node, NULL);
        }
    } else if ((b->kind == B_ROW || b->kind == B_ROW_GROUP) && P->mode == M_PAINT && visible && (st->bg_color >> 24)) {
        float z[4] = {0, 0, 0, 0};
        paint_bg_border(P, st, P->ox + x, P->oy + y, b->w, b->h, z, false, false, false,b->shadow_cache,b->radius_clip);
    }
    if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) {
        if (P->mode == M_PAINT && visible && st->content_visibility!=CV_HIDDEN) {
            const struct gfx_mask_clip *prior=c->mask_clip;struct gfx_mask_clip replacement;
            radius_clip_push(P,b->radius_clip[2],&replacement,P->ox+x,P->oy+cy(b));paint_replaced(P, b);c->mask_clip=prior;
        }
        goto done;
    }
    if(st->content_visibility==CV_HIDDEN)goto done;
    if (b->marker || b->marker_shape) {
        if (P->mode == M_PAINT) paint_marker(P, b);
    }
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    const struct gfx_mask_clip *previous_mask=c->mask_clip;struct gfx_mask_clip overflow_mask;
    bool contents = true;
    if (clip && P->mode != M_BOUNDS) {
        float px = P->ox + bx + b->b[3], py = P->oy + by + b->b[0];
        gfx_clip(c, (int)floorf(px), (int)floorf(py), (int)ceilf(bw - b->b[1] - b->b[3]), (int)ceilf(bh - b->b[0] - b->b[2]));
        if(P->mode==M_PAINT)radius_clip_push(P,b->radius_clip[1],&overflow_mask,px,py);
        if (P->mode == M_HIT && !inside(P, bx + b->b[3], by + b->b[0], bw - b->b[1] - b->b[3], bh - b->b[0] - b->b[2])) {
            contents = false;
        }
        if(P->mode==M_HIT&&!P->index_build) {
            struct border_radii radii=used_radii(st,bw,bh);
            radii=inset_radii(radii,b->b,bw-b->b[1]-b->b[3],bh-b->b[0]-b->b[2]);
            if(corners_coverage(P->hx,P->hy,bx+b->b[3],by+b->b[0],bw-b->b[1]-b->b[3],bh-b->b[0]-b->b[2],&radii)<.5f)contents=false;
        }
    }
    int layer = 0;
    for (; layer < layers.n && layer_z(layers.v[layer]) < 0; layer++)
        paint_stacked_layer(P, layers.v[layer]);
    if (contents && b->inline_ctx) {
        inline_children(P, b, true);
        paint_runs(P, b, NULL);
        inline_children(P, b, false);
    } else if (contents && b->kind == B_INLINE) {
        inline_children(P, b, true);
        if (b->inline_paint_source) paint_runs(P, b->inline_paint_source, b);
        inline_children(P, b, false);
    } else if (contents) {
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated == false) paint_box(P, ch, false);
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated) paint_box(P, ch, false);
    }
    for (; layer < layers.n; layer++) paint_stacked_layer(P, layers.v[layer]);
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
    c->mask_clip=previous_mask;
done:
    release_layers(&layers,layers_borrowed);
    if (P->mode == M_PAINT && visible && st->outline_width > 0 && b->kind != B_INLINE && b->kind != B_TEXT &&
        b->kind != B_BR && b->kind != B_ROW && b->kind != B_ROW_GROUP)
        paint_outline(c, st, P->ox + bx, P->oy + by, bw, bh);
    if (saved) {
        int a = (int)(group_opacity * 255 + 0.5f);
        for (int j = 0; !P->failed && j < gh; j++) {
            if (!(j & 63) && !web_native_checkpoint(P->d)) { P->failed = true; break; }
            uint32_t *row = c->px + (size_t)(gy + j) * c->pitch + gx, *old = saved + (size_t)j * gw;
            for (int i = 0; i < gw; i++) row[i] = gfx_mix(old[i], row[i], a);
        }
        if (P->failed)
            for (int j = 0; j < gh; j++) memcpy(c->px + (size_t)(gy + j) * c->pitch + gx,
                saved + (size_t)j * gw, sizeof *saved * (size_t)gw);
        free(saved);
    }
    if (P->mode == M_BOUNDS) {
        b->paint_bounds_generation = P->bounds_generation;
        b->paint_bounds_valid = P->bounds_valid;
        if (P->bounds_valid) memcpy(b->paint_bounds, P->bounds, sizeof b->paint_bounds);
        memcpy(P->bounds, parent_bounds, sizeof parent_bounds); P->bounds_valid = parent_valid;
        if (b->paint_bounds_valid) bounds_rect(P, b->paint_bounds[0], b->paint_bounds[1],
            b->paint_bounds[2] - b->paint_bounds[0], b->paint_bounds[3] - b->paint_bounds[1]);
    }
}

static void stacked_ancestor_clips(struct pctx *P,box_t *layer,box_t *ancestor,bool reached,bool fixed_escape,bool skip) {
    if(!ancestor||!ancestor->parent){if(!skip)paint_box(P,layer,true);return;}
    canvas_t *canvas=P->c;const struct gfx_mask_clip *previous=canvas->mask_clip;struct gfx_mask_clip boundary;
    if(ancestor->clamp_hidden||(ancestor->st&&ancestor->st->display==D_NONE))skip=true;
    if(ancestor==layer->cb)reached=true;
    if(ancestor->st&&(web_marquee_box(ancestor)||((ancestor->clamp_truncated||ancestor->st->overflow!=OV_VISIBLE)&&reached&&!fixed_escape))&&
        !doc_viewport_overflow_box(P->d,ancestor)&&ancestor->kind!=B_INLINE) {
        float x=box_visual_x(ancestor)-ancestor->p[3],y=box_visual_y(ancestor)-ancestor->p[0];
        gfx_clip(canvas,(int)(P->ox+x),(int)(P->oy+y),(int)(ancestor->w+ancestor->p[1]+ancestor->p[3]),(int)(ancestor->h+ancestor->p[0]+ancestor->p[2]));
        if(P->mode==M_PAINT)radius_clip_push(P,ancestor->radius_clip[1],&boundary,P->ox+x,P->oy+y);
        else if(P->mode==M_HIT&&!P->index_build) {
            float w=ancestor->w+ancestor->p[1]+ancestor->p[3],h=ancestor->h+ancestor->p[0]+ancestor->p[2];
            struct border_radii radii=used_radii(ancestor->st,w+ancestor->b[1]+ancestor->b[3],h+ancestor->b[0]+ancestor->b[2]);
            radii=inset_radii(radii,ancestor->b,w,h);
            if(corners_coverage(P->hx,P->hy,x,y,w,h,&radii)<.5f)skip=true;
        }
    }
    if(box_viewport_fixed(ancestor)) {
        if(!P->d->marquees.n){if(!skip)paint_box(P,layer,true);canvas->mask_clip=previous;return;}
        fixed_escape=true;
    }
    stacked_ancestor_clips(P,layer,ancestor->parent,reached,fixed_escape,skip);
    canvas->mask_clip=previous;
}
static void paint_stacked_layer(struct pctx *P, box_t *l) {
    /* Stacking order and containing-block clipping are independent. Rebuild
       this layer's clip from the viewport, not its paint context's clip:
       fixed children escape overflow, and absolute children skip overflow
       below their containing block. This also works across nested contexts. */
    canvas_t *c = P->c;
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    const struct gfx_mask_clip *previous=c->mask_clip;c->mask_clip=P->viewport_mask;
    c->cx0 = P->vx0, c->cy0 = P->vy0, c->cx1 = P->vx1, c->cy1 = P->vy1;
    stacked_ancestor_clips(P,l,l->parent,l->st->position!=POS_ABSOLUTE&&l->st->position!=POS_FIXED,box_viewport_fixed(l),false);
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
    c->mask_clip=previous;
}

static void walk(struct pctx *P) {
    if(P->mode!=M_BOUNDS&&!prepare_paint_order(P->d)){P->failed=true;return;}
    P->vx0 = P->c->cx0, P->vy0 = P->c->cy0;
    P->vx1 = P->c->cx1, P->vy1 = P->c->cy1;
    P->viewport_mask=P->c->mask_clip;
    if (P->d->root_box) paint_box(P, P->d->root_box, true);
    float ox=P->ox,oy=P->oy,hx=P->hx,hy=P->hy;
    node_t *fullscreen=P->d->fullscreen_element;
    if(fullscreen&&fullscreen->box){
        if(P->mode==M_HIT){memset(P->hit,0,sizeof *P->hit);P->hit_any=false;}
        else if(P->mode==M_PAINT)gfx_fill(P->c,P->vx0,P->vy0,P->vx1-P->vx0,P->vy1-P->vy0,RGB(0,0,0));
        P->top_root=fullscreen->box;paint_box(P,fullscreen->box,true);
    }
    for(int i=0;i<web_dialog_count(P->d);i++) {
        node_t *n=web_dialog_at(P->d,i); if(!n || !n->box)continue;
        if(P->mode==M_HIT) {
            if(n!=web_dialog_top(P->d))continue;
            memset(P->hit,0,sizeof *P->hit); P->hit_any=false;
            hit_target(P,hit_style(n->style)?n:NULL);
        }
        box_t *layers[2]={web_dialog_backdrop(P->d,n),n->box};
        for(int j=0;j<2;j++) {
            box_t *b=layers[j]; if(!b)continue;
            /* Fixed top-layer boxes already carry the viewport offset in
               box_visual_* just like ordinary fixed stacking contexts. */
            P->ox=ox; P->oy=oy; P->hx=hx; P->hy=hy;
            P->top_root=b; paint_box(P,b,true);
        }
    }
    P->top_root=NULL; P->ox=ox; P->oy=oy; P->hx=hx; P->hy=hy;
}

uint32_t doc_canvas_bg(web_doc *d, int *from);
uint32_t doc_canvas_bg(web_doc *d, int *from) {
    *from = 0;
    if (d->html && d->html->style && (d->html->style->bg_color >> 24 || d->html->style->has_grad)) {
        *from = 1;
        return d->html->style->has_grad ? d->html->style->grad[0] : d->html->style->bg_color;
    }
    if (d->body && d->body->style && (d->body->style->bg_color >> 24 || d->body->style->has_grad)) {
        *from = 2;
        return d->body->style->has_grad ? d->body->style->grad[0] : d->body->style->bg_color;
    }
    return RGB(255, 255, 255);
}

int web_paint_report(char *out, size_t size) {
    if(!paint_profile.paints)return 0;
    int n=snprintf(out,size,"Native paint profile: paints %lu/%lu ms, order %lu ms (%lu builds, %lu layer sorts, radius masks %lu/%lu kpx built, %lu moved), frames %lu ms, walk %lu ms, video probe %lu ms, opacity groups %lu/%lu kpx (inclusive)",
        (unsigned long)paint_profile.paints,(unsigned long)paint_profile.total_ms,(unsigned long)paint_profile.order_ms,
        (unsigned long)paint_profile.order_builds,(unsigned long)paint_profile.layer_builds,(unsigned long)paint_profile.radius_builds,
        (unsigned long)(paint_profile.radius_pixels/1000),(unsigned long)paint_profile.radius_moves,
        (unsigned long)paint_profile.frames_ms,(unsigned long)paint_profile.walk_ms,(unsigned long)paint_profile.probe_ms,
        (unsigned long)paint_profile.groups,(unsigned long)(paint_profile.group_pixels/1000));
    memset(&paint_profile,0,sizeof paint_profile);
    return n;
}
static void paint_document(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y);
void web_paint(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y) {
    uint32_t memo=box_visual_memo_begin();
    bool profile=d && d->profile_enabled && !d->frame_parent;
    uint64_t start=profile?uptime_ms():0;
    paint_document(d,c,x,y,w,h,doc_x,doc_y);
    if(profile){paint_profile.paints++;paint_profile.total_ms+=uptime_ms()-start;}
    box_visual_memo_end(memo);
}
static void paint_document(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y) {
    if(web_render_blocked(d)) {
        gfx_fill(c,x,y,w,h,RGB(255,255,255));
        return;
    }
    /* The window still owns the last coherent pixels. Never replace them
       with a white canvas while a cancelled native transaction is retired
       or rebuilt; real scrolling resumes only after layout publication. */
    if(d->native_cancelled || d->native_recovery_pending || d->native_recovery_active)return;
    /* Sorting/cancellation finishes before any committed window pixels are
       overwritten. A cancelled cache build cannot leave a white viewport. */
    d->view_x=doc_x;d->view_y=doc_y;
    bool profile=d->profile_enabled && !d->frame_parent;
    uint64_t mark=profile?uptime_ms():0;
    if(!prepare_paint_order(d)){doc_paint_full(d);return;}
    if(profile){uint64_t now=uptime_ms();paint_profile.order_ms+=now-mark;mark=now;}
    d->initial_render_done=true;
    paint_debug_begin(d);
    d->view_x=doc_x; d->view_y=doc_y; /* frame crops use this paint's real scroll */
    bool snapshot_root=!d->frame_parent;
    if(snapshot_root){web_frames_prepare_paint(d);web_avmedia_snapshot_begin(d);}
    if(profile){uint64_t now=uptime_ms();paint_profile.frames_ms+=now-mark;mark=now;}
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    gfx_clip(c, x, y, w, h);
    struct pctx P = {0};
    P.d = d;
    P.c = c;
    P.ox = (float)(x - doc_x);
    P.oy = (float)(y - doc_y);
    P.mode = M_PAINT;
    P.bounds_generation = ++d->paint_bounds_generation;
    /* Only real alpha groups need a paint transaction. Its extent is the
       already clipped viewport, never the unbounded document or CSS sizes.
       Existing build/restyle aggregation also covers native opacity samples. */
    uint32_t *frame_saved = NULL;
    int fx = c->cx0, fy = c->cy0, fw = c->cx1 - fx, fh = c->cy1 - fy;
    if (d->root_box && d->root_box->paint_alpha && fw > 0 && fh > 0) {
        if (!c->px || fx < 0 || fy < 0 || c->cx1 > c->w || c->cy1 > c->h || c->pitch < c->w ||
            (size_t)fw > SIZE_MAX / sizeof *frame_saved / (size_t)fh ||
            (size_t)c->pitch > SIZE_MAX / sizeof *frame_saved / (size_t)c->h ||
            !(frame_saved = malloc(sizeof *frame_saved * (size_t)fw * (size_t)fh))) P.failed = true;
        else for (int row = 0; row < fh; row++)
            memcpy(frame_saved + (size_t)row * fw, c->px + (size_t)(fy + row) * c->pitch + fx, sizeof *frame_saved * (size_t)fw);
    }
    int from;
    uint32_t bg = doc_canvas_bg(d, &from);
    P.canvas_bg = bg;
    P.canvas_group = d->html && d->html->style && (d->html->style->filter || d->html->style->opacity < 1);
    /* a translucent canvas background is drawn over white */
    if (!P.failed) {
        gfx_fill(c, x, y, w, h, RGB(255, 255, 255));
        if (!P.canvas_group) gfx_fill_blend(c, x, y, w, h, bg);
    }
    P.canvas_bg_from = (uint32_t)from;
    walk(&P);
    if (P.failed && frame_saved)
        for (int row = 0; row < fh; row++) memcpy(c->px + (size_t)(fy + row) * c->pitch + fx,
            frame_saved + (size_t)row * fw, sizeof *frame_saved * (size_t)fw);
    free(frame_saved);
    if (P.failed && !d->paint_effect_failure_reported) {
        d->paint_effect_failure_reported = true;
        web_js_console(d, 1, "Native filter/opacity composition stopped; no unfiltered replacement was drawn.");
    }
    if (P.failed) doc_paint_full(d);
    paint_debug_finish(d);
    if(profile){uint64_t now=uptime_ms();paint_profile.walk_ms+=now-mark;mark=now;}
    if(snapshot_root) {
        canvas_t probe;int px,py;
        if(!P.failed && web_avmedia_snapshot_prepare(d,c,&probe,&px,&py,doc_x,doc_y)) {
            /* Only normal paint examines the committed tree. The small probe
             * canvas changes neither document view offsets nor the real UI. */
            struct pctx Q=P;Q.c=&probe;Q.ox-=px;Q.oy-=py;
            font_probe_begin();
            for(unsigned phase=1;phase<=2;phase++) {
                web_avmedia_snapshot_probe(phase);
                gfx_noclip(&probe);
                gfx_fill(&probe,0,0,probe.w,probe.h,RGB(255,255,255));
                if (!Q.canvas_group) gfx_fill_blend(&probe,x-px,y-py,w,h,bg);
                walk(&Q);
            }
            if(!font_probe_end())web_avmedia_snapshot_reject();
        }
        web_avmedia_snapshot_finish();
        web_frames_finish_paint(d);
        if(profile)paint_profile.probe_ms+=uptime_ms()-mark;
    }
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
}

void web_hit_index_free(web_doc *d) {
    if(!d||!d->hit_index)return;
    free(d->hit_index->items);free(d->hit_index->branches);free(d->hit_index->regions);free(d->hit_index);d->hit_index=NULL;
}
void doc_paint_damage(web_doc *d,node_t *node){
    if(!d)return;
    /* An unclassified notification already pending cannot be downgraded to
       a small rectangle by a later known node. Layout damage is always full. */
    if(d->paint_dirty&&!d->paint_damage_valid)d->paint_damage_full=true;
    d->paint_dirty=true;
    box_t *box=node&&node->owner==d?node->box:NULL;
    if(!d->layout_valid||d->need_style||d->need_boxes||!box||box->kind!=B_ATOMIC){d->paint_damage_full=true;return;}
    float x=box_visual_x(box)-box->p[3]-box->b[3]-2,y=box_visual_y(box)-box->p[0]-box->b[0]-2;
    float right=x+box->w+box->p[1]+box->p[3]+box->b[1]+box->b[3]+4;
    float bottom=y+box->h+box->p[0]+box->p[2]+box->b[0]+box->b[2]+4;
    float ink[4]={x,y,right,bottom};
    if(box->st)shadow_ink_bounds(box->st,x+2,y+2,right-x-4,bottom-y-4,ink);
    x=ink[0];y=ink[1];right=ink[2];bottom=ink[3];
    if(!isfinite(x)||!isfinite(y)||!isfinite(right)||!isfinite(bottom)){d->paint_damage_full=true;return;}
    if(!d->paint_damage_valid){d->paint_damage[0]=x;d->paint_damage[1]=y;d->paint_damage[2]=right;d->paint_damage[3]=bottom;d->paint_damage_valid=true;}
    else{d->paint_damage[0]=fminf(d->paint_damage[0],x);d->paint_damage[1]=fminf(d->paint_damage[1],y);
        d->paint_damage[2]=fmaxf(d->paint_damage[2],right);d->paint_damage[3]=fmaxf(d->paint_damage[3],bottom);}
}
bool web_take_paint_damage(web_doc *d,int *x,int *y,int *w,int *h){
    if(!d)return false;
    bool valid=d->paint_damage_valid&&!d->paint_damage_full&&d->layout_valid&&!d->need_style&&!d->need_boxes;
    if(valid){
        double vw=MAX(d->width,0),vh=MAX(d->height,0);
        double left=fmin(vw,fmax(0,floor(d->paint_damage[0])-d->view_x)),top=fmin(vh,fmax(0,floor(d->paint_damage[1])-d->view_y));
        double right=fmax(0,fmin(vw,ceil(d->paint_damage[2])-d->view_x)),bottom=fmax(0,fmin(vh,ceil(d->paint_damage[3])-d->view_y));
        *x=(int)left;*y=(int)top;*w=right>left?(int)(right-left):0;*h=bottom>top?(int)(bottom-top):0;
    }
    d->paint_damage_valid=d->paint_damage_full=false;return valid;
}
static int hit_compare_x(const void *av,const void *bv){
    const struct hit_primitive *a=av,*b=bv;
    float x=a->rect[0]*.5f+a->rect[2]*.5f,y=b->rect[0]*.5f+b->rect[2]*.5f;
    return x<y?-1:x>y?1:a->order<b->order?-1:a->order>b->order;
}
static int hit_compare_y(const void *av,const void *bv){
    const struct hit_primitive *a=av,*b=bv;
    float x=a->rect[1]*.5f+a->rect[3]*.5f,y=b->rect[1]*.5f+b->rect[3]*.5f;
    return x<y?-1:x>y?1:a->order<b->order?-1:a->order>b->order;
}
static bool hit_sort(web_doc *d,struct web_hit_index *index,struct hit_primitive *scratch,size_t first,size_t count,
                     int (*compare)(const void *,const void *)){
    unsigned work=0;
    for(size_t width=1;width<count;){
        for(size_t begin=0;begin<count;begin+=MIN(width*2,count-begin)){
            size_t left=begin,middle=begin+MIN(width,count-begin),right=middle,end=middle+MIN(width,count-middle),out=begin;
            while(left<middle||right<end){
                if(!(++work&255)&&!web_native_checkpoint(d))return false;
                scratch[first+out++]=right==end||(left<middle&&compare(&index->items[first+left],&index->items[first+right])<=0)?
                    index->items[first+left++]:index->items[first+right++];
            }
        }
        memcpy(index->items+first,scratch+first,count*sizeof *scratch);
        if(width>count/2)break;width*=2;
    }
    return true;
}
static size_t hit_partition(web_doc *d,struct web_hit_index *index,struct hit_primitive *scratch,size_t first,size_t count){
    if(!web_native_checkpoint(d))return SIZE_MAX;
    size_t id=index->branch_count++;struct hit_branch *branch=&index->branches[id];
    *branch=(struct hit_branch){.first=first,.count=count,.left=SIZE_MAX,.right=SIZE_MAX};
    memcpy(branch->rect,index->items[first].rect,sizeof branch->rect);
    for(size_t i=first;i<first+count;i++){
        if(!((i-first)&255)&&!web_native_checkpoint(d))return SIZE_MAX;
        const struct hit_primitive *item=&index->items[i];
        branch->rect[0]=fminf(branch->rect[0],item->rect[0]);branch->rect[1]=fminf(branch->rect[1],item->rect[1]);
        branch->rect[2]=fmaxf(branch->rect[2],item->rect[2]);branch->rect[3]=fmaxf(branch->rect[3],item->rect[3]);
        branch->last_order=MAX(branch->last_order,item->order);
    }
    if(count>8){
        if(!hit_sort(d,index,scratch,first,count,
            branch->rect[2]-branch->rect[0]>=branch->rect[3]-branch->rect[1]?hit_compare_x:hit_compare_y))return SIZE_MAX;
        size_t middle=count/2;
        branch->left=hit_partition(d,index,scratch,first,middle);
        if(branch->left==SIZE_MAX)return SIZE_MAX;
        branch->right=hit_partition(d,index,scratch,first+middle,count-middle);
        if(branch->right==SIZE_MAX)return SIZE_MAX;
    }
    return id;
}
static bool hit_rect_contains(const float rect[4],float x,float y){return x>=rect[0]&&y>=rect[1]&&x<rect[2]&&y<rect[3];}
static bool hit_corner_contains(const struct hit_primitive *item,float x,float y) {
    if(item->radius&&corners_coverage(x,y,item->radius_x,item->radius_y,item->radius->w,item->radius->h,&item->radius->radii)<.5f)return false;
    box_t *box=item->box;if(!box)return true;
    web_doc *doc=item->target?item->target->owner:NULL;if(!doc)return true;
    bool reached=!box->abspos,fixed=box_viewport_fixed(box);
    for(box_t *a=box->parent;a&&a->parent;a=a->parent) {
        if(a==box->cb)reached=true;
        if(a->st&&(web_marquee_box(a)||((a->clamp_truncated||a->st->overflow!=OV_VISIBLE)&&reached&&!fixed))&&a->kind!=B_INLINE&&
            !doc_viewport_overflow_box(doc,a)&&a->radius_clip[1]) {
            const struct radius_cache *radius=a->radius_clip[1];float px=box_visual_x(a)-a->p[3],py=box_visual_y(a)-a->p[0];
            if(corners_coverage(x,y,px,py,radius->w,radius->h,&radius->radii)<.5f)return false;
        }
        if(box_viewport_fixed(a)){if(!doc->marquees.n)break;fixed=true;}
    }
    return true;
}
static const struct hit_primitive *hit_query(const struct web_hit_index *index,size_t id,float x,float y,
                                            const struct hit_primitive *best){
    const struct hit_branch *branch=&index->branches[id];
    if((best&&best->order>=branch->last_order)||!hit_rect_contains(branch->rect,x,y))return best;
    if(branch->left==SIZE_MAX){
        for(size_t i=branch->first;i<branch->first+branch->count;i++){
            const struct hit_primitive *item=&index->items[i];
            if((!best||item->order>best->order)&&hit_rect_contains(item->rect,x,y)&&hit_corner_contains(item,x,y))best=item;
        }
        return best;
    }
    size_t first=branch->left,second=branch->right;
    if(index->branches[first].last_order<index->branches[second].last_order){size_t swap=first;first=second;second=swap;}
    best=hit_query(index,first,x,y,best);return hit_query(index,second,x,y,best);
}
/* Returns false only when the optimization is unavailable. Top-layer forced
 * targets retain the exact walk until their backdrop index contract is needed. */
static bool hit_index_lookup(struct pctx *P){
    web_doc *d=P->d;
    if(!d->layout_valid||d->need_boxes||d->need_style||d->fullscreen_element||web_dialog_count(d))return false;
    struct web_hit_index *index=d->hit_index;
    if(!index){index=calloc(1,sizeof *index);if(!index)return false;d->hit_index=index;}
    uint64_t fonts=font_metrics_generation();
    if(!index->valid||index->layout!=d->layout_revision||index->dom!=d->dom_revision||index->visual!=d->visual_revision||
       index->view_x!=d->view_x||index->view_y!=d->view_y||index->fonts!=fonts||!fonts){
        index->valid=false;index->count=index->branch_count=index->region_count=0;
        struct pctx build=*P;struct web_hit ignored={0};build.hit=&ignored;build.index_build=index;
        walk(&build);
        if(build.failed||font_metrics_generation()!=fonts||!fonts){
            if(d->native_cancelled||d->native_recovery_pending||d->native_recovery_active){P->failed=true;return true;}
            return false;
        }
        if(index->count){
            if(index->count>SIZE_MAX/2/sizeof *index->branches)return false;
            size_t capacity=index->count*2;
            if(capacity>index->branch_capacity){
                void *branches=realloc(index->branches,capacity*sizeof *index->branches);
                if(!branches)return false;
                index->branches=branches;index->branch_capacity=capacity;
            }
            struct hit_primitive *scratch=malloc(index->count*sizeof *scratch);
            if(!scratch)return false;
            size_t root=hit_partition(d,index,scratch,0,index->count);free(scratch);
            if(root==SIZE_MAX){P->failed=true;return true;}
        }
        index->layout=d->layout_revision;index->dom=d->dom_revision;index->visual=d->visual_revision;
        index->view_x=d->view_x;index->view_y=d->view_y;index->fonts=fonts;index->valid=true;
    }
    for(size_t i=0;i<index->region_count;i++)if(hit_rect_contains(index->regions[i],P->hx,P->hy))return false;
    const struct hit_primitive *item=index->count?hit_query(index,0,P->hx,P->hy,NULL):NULL;
    if(item){P->target=item->target;P->hit->kind=item->kind;P->hit->node=item->node;}
    return true;
}

bool web_hit_test(web_doc *d, int x, int y, struct web_hit *hit) {
    memset(hit, 0, sizeof *hit);
    if(!d || d->native_cancelled || d->native_recovery_pending || d->native_recovery_active)return false;
    struct pctx P = {0};
    canvas_t dummy = {0};
    dummy.cx0 = dummy.cy0 = -1000000000;
    dummy.cx1 = dummy.cy1 = 1000000000;
    P.d = d;
    P.c = &dummy;
    P.mode = M_HIT;
    P.hx = (float)x;
    P.hy = (float)y;
    P.hit = hit;
    if(!hit_index_lookup(&P))walk(&P);
    if (P.failed) { memset(hit, 0, sizeof *hit); return false; }
    finish_disclosure_hit(&P);
    /* A later noninteractive overlay is still a hit-test target. Never keep
       an earlier link/control merely because the overlay has no activation. */
    bool disclosure=hit->kind==WEB_HIT_DETAILS;
    if(!web_node_action(d,P.target,hit))return false;
    if(hit->kind==WEB_HIT_DETAILS && !disclosure){
        memset(hit,0,sizeof *hit);return false; /* default details body, not its legend */
    }
    return true;
}

web_node *web_node_at(web_doc *d, int x, int y) {
    if(!d || d->native_cancelled || d->native_recovery_pending || d->native_recovery_active)return NULL;
    node_t *fallback=NULL;
    while(d && !d->native_cancelled && !d->native_recovery_pending && !d->native_recovery_active && d->root_box) {
    struct pctx P = {0};
    struct web_hit hit = {0};
    canvas_t dummy = {0};
    dummy.cx0 = dummy.cy0 = -1000000000;
    dummy.cx1 = dummy.cy1 = 1000000000;
    P.d = d;
    P.c = &dummy;
    P.mode = M_HIT;
    P.hx = (float)x;
    P.hy = (float)y;
    P.hit = &hit;
    if(!hit_index_lookup(&P))walk(&P);
    if (P.failed) return NULL;
    if(P.target && web_frame_element(P.target)){
        struct web_frame *frame=web_frame_find(d,P.target);box_t *box=P.target->box;
        if(frame && frame->document && !frame->detached && box){
            int child_x=x-(int)box_visual_x(box)+frame->scroll_x;
            int child_y=y-(int)box_visual_y(box)+frame->scroll_y;
            if(child_x>=frame->scroll_x && child_y>=frame->scroll_y &&
               child_x<frame->scroll_x+(int)box->w && child_y<frame->scroll_y+(int)box->h){
                fallback=P.target;d=frame->document;x=child_x;y=child_y;continue;
            }
        }
    }
    return P.target && !web_dialog_inert(d,P.target) ? P.target : fallback;
    }
    return fallback;
}

node_t *doc_element_at(web_doc *d,int x,int y) {
    node_t *hit=web_node_at(d,x,y);
    /* Native input deliberately enters child viewports. A Document query must
       instead expose the containing frame, including at cross-origin borders. */
    while(hit && hit->owner!=d){
        web_doc *child=hit->owner;
        if(!child || !child->live || !child->frame_parent || !child->frame_element)return NULL;
        struct web_frame *frame=web_frame_find(child->frame_parent,child->frame_element);
        if(!frame || frame->detached || frame->document!=child ||
           doc_node_root(child->frame_element,true)!=child->frame_parent->root)return NULL;
        hit=child->frame_element;
    }
    if(hit && hit->type!=N_ELEM)hit=doc_flat_parent(hit);
    return hit;
}

web_node *web_link_activation_anchor(web_doc *d, web_node *target) {
    if(target && target->owner && target->owner!=d && target->owner->frame_parent)d=target->owner;
    if (!d || d->inert || !target || target->owner != d || web_dialog_inert(d,target)) return NULL;
    for (node_t *n = target; n; n = doc_flat_parent(n)) {
        if (n->type != N_ELEM || n->foreign) continue;
        /* Activation identity does not depend on today's href: a listener may
           add/change it. Never switch to another anchor after dispatch. */
        if (n->tag == T_a) return n;
        /* Hidden controls have no hit box, but still own activation rather
           than allowing a descendant click to escape into an outer anchor. */
        if (n->tag == T_input || n->tag == T_button || n->tag == T_select ||
            n->tag == T_textarea || n->tag == T_label || n->tag == T_summary ||
            n->tag == T_iframe || n->tag == T_embed ||
            ((n->tag == T_audio || n->tag == T_video) && node_attr(n, "controls")) ||
            ((n->tag == T_img || n->tag == T_object) && node_attr(n, "usemap"))) return NULL;
    }
    return NULL;
}

bool web_link_action(web_doc *d, web_node *anchor, struct web_hit *hit) {
    if(anchor && anchor->owner && anchor->owner!=d && anchor->owner->frame_parent)d=anchor->owner;
    memset(hit, 0, sizeof *hit);
    if (!d || d->inert || !anchor || anchor->owner != d || anchor->type != N_ELEM ||
        anchor->foreign || anchor->tag != T_a || web_dialog_inert(d,anchor)) return false;
    /* HTML's cannot-navigate rule exempts <a> from connectedness. Keeping its
       owner/activity check rejects adoption into inert auxiliary documents. */
    const char *href = doc_link_href(d, anchor);
    if (!href) return false;
    hit->kind = WEB_HIT_LINK; hit->node = anchor; hit->href = href;
    return true;
}

bool web_node_action(web_doc *d, web_node *target, struct web_hit *hit) {
    if(target && target->owner && target->owner!=d && target->owner->frame_parent)d=target->owner;
    memset(hit, 0, sizeof *hit);
    if (!d || !target || web_dialog_inert(d,target)) return false;
    node_t *root = doc_node_root(target, true);
    if (root != d->root) return false;
    for (node_t *n = target; n; n = doc_flat_parent(n)) {
        if (n->type != N_ELEM) continue;
        if (n->box) {
            int kind = control_hit(n->box);
            if (kind != WEB_HIT_NONE) {
                if (node_attr(n, "disabled")) return false;
                hit->kind = kind;
                hit->node = n;
                return true;
            }
        }
        if (n->tag == T_a && node_attr(n, "href")) {
            hit->kind = WEB_HIT_LINK;
            hit->node = n;
            hit->href = doc_link_href(d, n);
            return hit->href != NULL;
        }
        if (n->tag == T_summary) {
            node_t *details = doc_details_activation(target);
            if (details) { hit->kind = WEB_HIT_DETAILS; hit->node = details; return true; }
            return false;
        }
        /* A renderer-owned default legend is focused through its details
           host. Mouse actions must still use the anonymous legend's hit box,
           not turn all of an open details body's whitespace into a toggle. */
        if (n == target && n->tag == T_details && !n->foreign && !doc_details_summary(n)) {
            hit->kind = WEB_HIT_DETAILS;
            hit->node = n;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- find in page */
struct fmatch {
    web_doc *d;
    const char *q;
    size_t qn;
    int from_y;
    float best_y, first_y;
    box_t *best_b, *first_b;
    int best_r, first_r;
    size_t best_off, first_off;
    bool failed;
};

static bool ieq_at(const char *s, size_t n, const char *q, size_t qn) {
    if (qn > n) return false;
    for (size_t i = 0; i < qn; i++)
        if (lower((unsigned char)s[i]) != lower((unsigned char)q[i])) return false;
    return true;
}

/* Flat-tree iteration keeps slots/shadow contents in native render order and
   never enters an inactive template forest. No fixed-depth traversal stack. */
static node_t *find_flat_first(node_t *n) {
    if(n->shadow_root)n=n->shadow_root;
    if(!n->foreign && n->tag==T_slot && n->slot_assigned_first && doc_node_root(n,false)->shadow_host)
        return n->slot_assigned_first;
    return n->first;
}
static node_t *find_flat_next(node_t *root,node_t *n,bool descend) {
    node_t *child=descend?find_flat_first(n):NULL;if(child)return child;
    while(n && n!=root) {
        node_t *p=doc_flat_parent(n);if(!p)return NULL;
        node_t *next=!p->foreign && p->tag==T_slot && p->slot_assigned_first?n->assigned_next:n->next;
        if(next)return next;n=p;
    }
    return NULL;
}
static bool find_until_found(node_t *n) {
    const char *value=n->type==N_ELEM && !n->foreign?node_attr(n,"hidden"):NULL;
    return value && str_ieq(value,"until-found");
}
static bool find_contained(const style_t *st) {
    return st && st->display!=D_NONE && st->display!=D_INLINE && st->display!=D_CONTENTS;
}
struct hidden_match {
    struct fmatch *visible;
    size_t *prefix, matched, position;
    node_t **owners, *first, *best;
    float first_y,best_y;
    bool space;
};
static void hidden_byte(struct hidden_match *H,unsigned char c,node_t *owner,float y) {
    const char *q=H->visible->q;size_t qn=H->visible->qn;c=(unsigned char)lower(c);
    H->owners[H->position]=owner;if(++H->position==qn)H->position=0;
    while(H->matched && c!=(unsigned char)lower((unsigned char)q[H->matched]))H->matched=H->prefix[H->matched-1];
    if(c==(unsigned char)lower((unsigned char)q[H->matched]))H->matched++;
    if(H->matched==qn) {
        node_t *start=H->owners[H->position];
        if(!H->first){H->first=start;H->first_y=y;}
        if(!H->best && y>=H->visible->from_y){H->best=start;H->best_y=y;}
        H->matched=H->prefix[qn-1];
    }
    H->space=c==' ';
}
static void hidden_text(struct hidden_match *H,node_t *n,const style_t *st,float y) {
    bool collapse=!st || st->white_space==WS_NORMAL || st->white_space==WS_NOWRAP || st->white_space==WS_PRE_LINE;
    for(size_t i=0;i<n->textlen;i++) {
        unsigned char c=(unsigned char)n->text[i];
        if(collapse && is_space(c)){if(H->space)continue;c=' ';}
        hidden_byte(H,c,n,y);
    }
}
/* Search skipped content without temporarily exposing it or changing author
   CSS/DOM. Only the selected real match enters the ancestor reveal algorithm. */
static bool find_hidden(struct hidden_match *H) {
    web_doc *d=H->visible->d;size_t qn=H->visible->qn;
    for(node_t *n=d->root;n;) {
        web_avmedia_checkpoint();
        bool descend=true;
        if(n->type==N_ELEM) {
            style_t *st=n->style;
            if(!st || st->display==D_NONE || web_dialog_inert(d,n))descend=false;
            else if(st->content_visibility==CV_HIDDEN) {
                descend=false;
                if(find_until_found(n) && find_contained(st) && n->box) {
                    if(!H->prefix) {
                        if(qn>SIZE_MAX/sizeof *H->prefix || qn>SIZE_MAX/sizeof *H->owners)return false;
                        H->prefix=malloc(qn*sizeof *H->prefix);H->owners=malloc(qn*sizeof *H->owners);
                        if(!H->prefix || !H->owners)return false;
                        H->prefix[0]=0;
                        for(size_t i=1,k=0;i<qn;i++) {
                            unsigned char c=(unsigned char)lower((unsigned char)H->visible->q[i]);
                            while(k && c!=(unsigned char)lower((unsigned char)H->visible->q[k]))k=H->prefix[k-1];
                            if(c==(unsigned char)lower((unsigned char)H->visible->q[k]))k++;
                            H->prefix[i]=k;
                        }
                    }
                    H->matched=H->position=0;H->space=false;float y=cy(n->box);node_t *text_block=NULL;
                    for(node_t *text=find_flat_first(n);text;) {
                        bool below=true;
                        node_t *parent=doc_flat_parent(text);
                        if(parent && !parent->foreign && parent->tag==T_details && !node_attr(parent,"open") &&
                            doc_details_summary(parent)!=text) {
                            text=find_flat_next(n,text,false);continue;
                        }
                        if(text->type==N_ELEM) {
                            style_t *ts=text->style;
                            if(!ts || ts->display==D_NONE || web_dialog_inert(d,text) ||
                                (ts->content_visibility==CV_HIDDEN && (!find_until_found(text) || !find_contained(ts))))below=false;
                            else if(ts->display!=D_INLINE && ts->display!=D_CONTENTS && H->matched && !H->space)
                                hidden_byte(H,' ',text,y);
                            if(!text->foreign && (text->tag==T_template || text->tag==T_input || text->tag==T_select || text->tag==T_textarea))below=false;
                        } else if(text->type==N_TEXT && text->textlen) {
                            node_t *p=doc_flat_parent(text);const style_t *ts=p?p->style:NULL;
                            if(ts && !ts->visibility) {
                                node_t *block=p;
                                while(block && block!=n && block->style &&
                                    (block->style->display==D_INLINE || block->style->display==D_CONTENTS))block=doc_flat_parent(block);
                                if(text_block && block!=text_block && H->matched && !H->space)hidden_byte(H,' ',text,y);
                                text_block=block;hidden_text(H,text,ts,y);
                            }
                        }
                        text=find_flat_next(n,text,below);
                    }
                }
            }
            if(!n->foreign && n->tag==T_template)descend=false;
        }
        n=find_flat_next(d->root,n,descend);
    }
    return true;
}

static void find_in(struct fmatch *F, box_t *b) {
    if (F->failed || b->clamp_hidden || !b->st || b->st->display == D_NONE || b->st->content_visibility==CV_HIDDEN || (b->node && web_dialog_inert(F->d,b->node))) return;
    if (b->nruns) {
        /* the block's text, with the run each byte came from */
        sbuf t = {0};
        int *owner = NULL;
        size_t cap = 0;
        float prev_y = -1e30f;
        for (int i = 0; i < b->nruns; i++) {
            struct run *r = &b->runs[i];
            if (r->atomic || !r->n || r->st->visibility) continue;
            if (t.n && r->y != prev_y && t.p[t.n - 1] != ' ') sb_putc(&t, ' ');
            sb_put(&t, r->s, (size_t)r->n);
            prev_y = r->y;
            if (t.n > cap) {
                size_t old = cap;
                size_t max=SIZE_MAX/sizeof *owner;
                if(t.n>max){F->failed=true;free(owner);sb_free(&t);return;}
                cap = t.n>max/2?t.n:t.n*2;
                int *grown = realloc(owner, sizeof *owner * cap);
                if(!grown){F->failed=true;free(owner);sb_free(&t);return;}
                owner=grown;
                for (size_t k = old; k < cap; k++) owner[k] = -1;
            }
            for (size_t k = t.n - (size_t)r->n; k < t.n; k++) owner[k] = i;
        }
        for (size_t k = 0; F->qn<=t.n && k<=t.n-F->qn; k++) {
            if (!ieq_at(t.p + k, t.n - k, F->q, F->qn)) continue;
            size_t j = k;
            while (j < t.n && owner[j] < 0) j++;
            if (j >= t.n) break;
            int ri = owner[j];
            float y = cy(b) + b->runs[ri].y;
            if (!F->first_b) F->first_b = b, F->first_r = ri, F->first_y = y, F->first_off = k;
            if (y >= (float)F->from_y && !F->best_b) F->best_b = b, F->best_r = ri, F->best_y = y, F->best_off = k;
        }
        free(owner);
        sb_free(&t);
    }
    for (box_t *c = b->first; c; c = c->next) find_in(F, c);
}

static void clear_highlight(box_t *b) {
    for (int i = 0; i < b->nruns; i++) b->runs[i].highlight = false;
    for (box_t *c = b->first; c; c = c->next) clear_highlight(c);
}

/* highlight the runs that make up the match starting at byte off of b's text */
static void highlight(box_t *b, size_t off, size_t qn) {
    size_t pos = 0;
    float prev_y = -1e30f;
    bool last_space = false;
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
        if (r->atomic || !r->n || r->st->visibility) continue;
        if (pos && r->y != prev_y && !last_space) pos++;
        size_t s = pos, e = pos + (size_t)r->n;
        if (e > (size_t)off && s < (size_t)off + qn) r->highlight = true;
        pos = e;
        prev_y = r->y;
        last_space = r->s[r->n - 1] == ' ';
    }
}

int web_find(web_doc *d, const char *text, int from_y) {
    if (!d) return -1;
    if(d->find_revealing)return -1;
    if (!text || !*text) {
        free(d->find_text); d->find_text = NULL;
        if (d->root_box) clear_highlight(d->root_box);
        return -1;
    }
    /* Allocate before committing, including a substring of our previous query.
       A relayout can reuse the exact owned query without another allocation. */
    if (text != d->find_text) {
        char *copy = strdup(text);
        if (!copy) return -2;
        free(d->find_text); d->find_text = copy;
    }
    text = d->find_text;
    if(d->need_style || d->need_boxes || !d->layout_valid) {
        d->find_revealing=true;web_layout(d,MAX(d->width,1),MAX(d->height,1));d->find_revealing=false;
    }
    if (!d->root_box) return -1;
    clear_highlight(d->root_box);
    struct fmatch F = {0};
    F.d = d;
    F.q = text;
    F.qn = strlen(text);
    F.from_y = from_y;
    find_in(&F, d->root_box);
    if(F.failed)return -2;
    struct hidden_match H={.visible=&F};bool searched=find_hidden(&H);
    node_t *hidden=NULL;
    if(H.best && (!F.best_b || H.best_y<F.best_y))hidden=H.best;
    else if(!F.best_b && !H.best && H.first && (!F.first_b || H.first_y<F.first_y))hidden=H.first;
    free(H.prefix);free(H.owners);
    if(!searched)return -2;
    if(hidden) {
        /* No box/run pointer from the old layout crosses author beforematch.
           Its callbacks may remove/adopt the target or force a fresh layout. */
        d->find_revealing=true;
        web_js_reveal_hidden(d,hidden);
        web_layout(d,MAX(d->width,1),MAX(d->height,1));
        d->find_revealing=false;
        if(!d->root_box)return -1;
        clear_highlight(d->root_box);
        F=(struct fmatch){.d=d,.q=d->find_text,.qn=strlen(d->find_text),.from_y=from_y};
        find_in(&F,d->root_box);
        if(F.failed)return -2;
    }
    box_t *b = F.best_b ? F.best_b : F.first_b;
    if (!b) return -1;
    size_t off = F.best_b ? F.best_off : F.first_off;
    float y = F.best_b ? F.best_y : F.first_y;
    highlight(b, off, F.qn);
    wfont f = style_font(b->runs[F.best_b ? F.best_r : F.first_r].st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    return (int)(y - asc);
}
