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
    uint8_t pseudo; /* PE_BEFORE/PE_AFTER: the target's generated box, else PE_NONE */
    box_t *box;
    const struct radius_cache *radius;
    float radius_x,radius_y;
    /* A text run's primitive: the run (run_block's runs[run]) and its start's x, for the
       text position under the point (caretPositionFromPoint). run_block NULL otherwise. */
    const box_t *run_block;
    int run;
    float run_x;
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
    int view_x, view_y, view_w, view_h;
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
    uint8_t target_pseudo;
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
    struct hit_stack *stack; /* doc_elements_at: every target the exact walk reaches, bottom first */
    /* Paint Timing, of a paint walk: it drew something other than the default
       background (first paint), and something contentful (first contentful
       paint): text, an image, a canvas bitmap, a video frame, an input's value. */
    bool painted, contentful;
    /* background-clip: text's walk (bg_text): the text alone, white, into a coverage mask */
    bool text_mask;
    /* The latest target's hit point in its own untransformed space and, for a text run, the
       run and the point's advance from its start: the topmost hit's text position. */
    float hit_local_x, hit_local_y, hit_run_dx;
    const box_t *hit_run_block;
    int hit_run;
};
struct hit_entry { node_t *node; uint8_t pseudo; };
struct hit_stack { struct hit_entry *items; size_t count, capacity; bool failed; };
#include "paint_media_debug.h"
/* debug_js only: where top-level paint time goes between profile reports. */
static struct { uint64_t paints, order_ms, frames_ms, walk_ms, probe_ms, total_ms, groups, group_pixels,
    order_builds, layer_builds, radius_builds, radius_pixels, radius_moves; } paint_profile;
/* debug_js only: how restyles' paint damage was decided between profile reports. */
static struct { uint64_t commits, boxes, lost_layout, lost_unbounded, lost_room, lost_ink,
    taken_partial, taken_full, taken_none, taken_stale; } damage_profile;

/* What a paint walk drew, for Paint Timing; only a real paint counts. */
static void paint_drew(struct pctx *P, bool contentful) {
    if (P->mode != M_PAINT || P->text_mask) return;
    P->painted = true;
    if (contentful) P->contentful = true;
}
/* Paint Timing's non-empty text: a character that is not document white space. */
static bool contentful_text(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\f' && s[i] != '\r') return true;
    return false;
}
static bool positioned(const box_t *b) {
    if (!b->st || b->kind == B_INLINE || b->kind == B_TEXT) return false;
    if (b->st->position != POS_STATIC) return b->st->position != POS_STICKY;
    /* z-index also applies to in-flow flex/grid items. Their layout remains
       static, but painting and hit testing must place them in the z order. */
    return !b->st->z_auto && b->parent && !b->abspos &&
           (b->parent->kind == B_FLEX || b->parent->kind == B_GRID);
}

/* css-masking-1 §8.1: a mask-border-source other than none */
static bool mask_border_set(const style_t *st) {
    const struct css_border_image *m = css_style_mask_border(st);
    return m->source.url || m->source.gradient;
}
static bool stacking_context(const box_t *b) {
    if (!b->st || b->kind == B_TEXT || b->kind == B_BR) return false;
    /* a mask (§7.1 a mask-image layer other than none, §8.1) makes one */
    if (b->st->filter || b->st->opacity < 1 || mask_border_set(b->st) || css_mask_layers_set(css_style_mask(b->st)) ||
        (b->kind!=B_INLINE&&css_translation_active(b->st))) return true;
    if (b->kind == B_INLINE) return false;
    /* Opacity groups are atomic even in normal flow. Fixed/sticky boxes also
       establish a context with auto z-index; their descendants cannot escape
       into the outer context's global sort after author layout hydration. */
    return b->st->opacity < 1.0f || b->st->position == POS_FIXED || b->st->position == POS_STICKY ||
           b->st->clip_path || (positioned(b) && !b->st->z_auto);
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
/* Distances from the left, top, right and bottom edges past which
   corners_coverage() is exactly 1 inside the box (both the circular SDF and
   the elliptical path clamp there), and a row's coverage depends on x alone.
   A corner reaches across by its x radius and down by its y radius. */
static void radii_reach(const struct border_radii *r,float reach[4]) {
    reach[0]=fmaxf(r->x[0],r->x[3])+2;reach[1]=fmaxf(r->y[0],r->y[1])+2;
    reach[2]=fmaxf(r->x[1],r->x[2])+2;reach[3]=fmaxf(r->y[2],r->y[3])+2;
}
/* The pixels [*first, *last) of a row whose centres lie between `from` and
   `to` (row-relative): past both side reaches, where a row's coverage
   depends on y alone. Empty (first = last = -1) when there are none. */
static void radii_middle(float from,float to,int width,int *first,int *last) {
    float a=ceilf(from-.5f),b=floorf(to-.5f)+1;
    *first=(int)fmaxf(0,fminf((float)width,a));*last=(int)fmaxf(0,fminf((float)width,b));
    if(*first>=*last)*first=*last=-1;
}
/* Rounded-box coverage masks. A mask depends on the shape alone: its size, its radii and its
   offset from the pixel grid. Shapes are shared, so like boxes (a grid of thumbnails) build one
   mask, and a box that moves by whole pixels, scrolls or is laid out again keeps its own. A mask
   keeps only the rows that differ: those across the corners, and one for the middle, where
   coverage depends on x alone (gfx_mask_clip's repeated rows). That bounds it by the radii, not
   by the box, so it covers the whole box and no view cuts it. It is built when a paint first
   draws through it. */
struct radius_key { float fx,fy,w,h;struct border_radii radii; };
struct radius_shape {
    struct radius_shape *next;
    struct radius_key key;
    uint32_t hash,refs;          /* refs: the caches holding it, under radius_shapes' lock */
    int width,height;            /* pixels covered, from floor(x), floor(y) */
    int repeat_at,repeat_extra;  /* as gfx_mask_clip's */
    size_t rows;                 /* stored; 0 past RADIUS_MASK_MAX: painted without the mask */
    struct gfx_mask_span *mask;  /* NULL until built: each stored row's all-255 run, then its
                                    width alpha bytes (radius_shape_alpha) */
};
struct radius_cache {
    float x,y,w,h;struct border_radii radii;
    int left,top;                /* the mask's first pixel: floor(x), floor(y) */
    struct radius_shape *shape;
};
#define RADIUS_MASK_MAX (64u<<20)
/* Every document's shapes. Caches take them while paint order is prepared and release them there
   and in layout's workers (fragments trimmed), so the table has a lock. */
static struct { uint32_t lock,cap,count; struct radius_shape **v; } radius_shapes;
static void radius_shapes_lock(void){while(__atomic_exchange_n(&radius_shapes.lock,1,__ATOMIC_ACQUIRE))__asm__ volatile("pause");}
static void radius_shapes_unlock(void){__atomic_store_n(&radius_shapes.lock,0,__ATOMIC_RELEASE);}
static uint32_t radius_key_hash(const struct radius_key *key) {
    const unsigned char *p=(const unsigned char *)key;uint32_t hash=2166136261u;
    for(size_t i=0;i<sizeof *key;i++)hash=(hash^p[i])*16777619u;
    return hash;
}
/* A table twice the size; without the memory the chains just grow longer. Under the lock. */
static void radius_shapes_grow(void) {
    uint32_t cap=radius_shapes.cap?radius_shapes.cap*2:256;
    struct radius_shape **v=calloc(cap,sizeof *v);if(!v)return;
    for(uint32_t i=0;i<radius_shapes.cap;i++)
        for(struct radius_shape *s=radius_shapes.v[i],*next;s;s=next){next=s->next;s->next=v[s->hash&(cap-1)];v[s->hash&(cap-1)]=s;}
    free(radius_shapes.v);radius_shapes.v=v;radius_shapes.cap=cap;
}
static struct radius_shape *radius_shape_take(const struct radius_key *key) {
    uint32_t hash=radius_key_hash(key);
    radius_shapes_lock();
    struct radius_shape *s=NULL;
    if(radius_shapes.cap)
        for(s=radius_shapes.v[hash&(radius_shapes.cap-1)];s;s=s->next)if(s->hash==hash&&!memcmp(&s->key,key,sizeof *key))break;
    if(!s&&(s=calloc(1,sizeof *s))) {
        if(radius_shapes.count>=radius_shapes.cap)radius_shapes_grow();
        if(!radius_shapes.cap){radius_shapes_unlock();free(s);return NULL;}
        s->key=*key;s->hash=hash;
        double width=ceil((double)key->fx+key->w),height=ceil((double)key->fy+key->h);
        s->width=width<INT_MAX?(int)width:INT_MAX;s->height=height<INT_MAX?(int)height:INT_MAX;
        /* Rows whose centre lies past both the top and the bottom reach: x alone decides their coverage. */
        float reach[4];radii_reach(&key->radii,reach);
        double plain0=fmax(0,ceil((double)reach[1]+key->fy-.5)),plain1=fmin(s->height,floor((double)key->fy+key->h-.5-reach[3])+1);
        if(plain1-plain0>=2){s->repeat_at=(int)plain0;s->repeat_extra=(int)(plain1-plain0)-1;}
        s->rows=(size_t)s->height-(size_t)s->repeat_extra;
        if(!s->width||!s->rows||(size_t)s->width>RADIUS_MASK_MAX/s->rows)s->rows=0;
        s->next=radius_shapes.v[hash&(radius_shapes.cap-1)];radius_shapes.v[hash&(radius_shapes.cap-1)]=s;radius_shapes.count++;
    }
    if(s)s->refs++;
    radius_shapes_unlock();
    return s;
}
static void radius_shape_release(struct radius_shape *s) {
    if(!s)return;
    radius_shapes_lock();
    if(--s->refs){radius_shapes_unlock();return;}
    struct radius_shape **at=&radius_shapes.v[s->hash&(radius_shapes.cap-1)];
    while(*at!=s)at=&(*at)->next;
    *at=s->next;radius_shapes.count--;
    radius_shapes_unlock();
    free(s->mask);free(s);
}
static const uint8_t *radius_shape_alpha(const struct radius_shape *s){return (const uint8_t *)(s->mask+s->rows);}
/* The shape's mask, built on first use: NULL without the memory or past RADIUS_MASK_MAX. A mask
   is published whole, so two threads painting at once at worst build it twice. */
static const struct radius_shape *radius_shape_mask(struct radius_shape *s,bool profile) {
    if(!s||!s->rows)return NULL;
    if(__atomic_load_n(&s->mask,__ATOMIC_ACQUIRE))return s;
    const struct radius_key *k=&s->key;
    struct gfx_mask_span *mask=malloc(s->rows*(sizeof *mask+(size_t)s->width));
    if(!mask)return NULL;
    uint8_t *alpha=(uint8_t *)(mask+s->rows);
    float reach[4];radii_reach(&k->radii,reach);
    int mid0,mid1;radii_middle(k->fx+reach[0],k->fx+k->w-reach[2],s->width,&mid0,&mid1);
    for(size_t i=0;i<s->rows;i++) {
        int py=(int)i<=s->repeat_at?(int)i:(int)i+s->repeat_extra;
        int first=-1,last=0;
        uint8_t *row=alpha+i*(size_t)s->width;
        for(int px=0;px<s->width;px++) {
            uint8_t a=(uint8_t)(255*corners_coverage(px+.5f,py+.5f,k->fx,k->fy,k->w,k->h,&k->radii)+.5f);
            if(px==mid0){memset(row+mid0,a,(size_t)(mid1-mid0));if(a==255){if(first<0)first=mid0;last=mid1;}px=mid1-1;continue;}
            row[px]=a;
            if(a==255){if(first<0)first=px;last=px+1;}
        }
        mask[i]=(struct gfx_mask_span){first<0?0:first,last};
    }
    struct gfx_mask_span *none=NULL;
    if(!__atomic_compare_exchange_n(&s->mask,&none,mask,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))free(mask);
    else if(profile){paint_profile.radius_builds++;paint_profile.radius_pixels+=(uint64_t)s->width*s->rows;}
    return s;
}
void web_radius_cache_free(struct radius_cache **slot){struct radius_cache *cache=*slot;*slot=NULL;if(cache){radius_shape_release(cache->shape);free(cache);}}
static bool radius_cache_prepare(web_doc *doc,struct radius_cache **slot,float x,float y,float w,float h,struct border_radii radii) {
    if(!radii_any(&radii)||w<=0||h<=0){web_radius_cache_free(slot);return true;}
    if(!web_native_checkpoint(doc))return false;
    double left=floor((double)x),top=floor((double)y);
    if(!isfinite(left)||!isfinite(top)||left<INT_MIN||top<INT_MIN||left>INT_MAX||top>INT_MAX)return false;
    struct radius_key key={(float)((double)x-left),(float)((double)y-top),w,h,radii};
    struct radius_cache *cache=*slot;
    if(!cache||!cache->shape||memcmp(&cache->shape->key,&key,sizeof key)) {
        struct radius_shape *shape=radius_shape_take(&key);if(!shape)return false;
        if(!cache&&!(cache=calloc(1,sizeof *cache))){radius_shape_release(shape);return false;}
        radius_shape_release(cache->shape);cache->shape=shape;
    } else if((cache->left!=(int)left||cache->top!=(int)top)&&doc->profile_enabled)paint_profile.radius_moves++;
    cache->x=x;cache->y=y;cache->w=w;cache->h=h;cache->radii=radii;cache->left=(int)left;cache->top=(int)top;
    *slot=cache;return true;
}
static bool radius_clip_push(struct pctx *P,const struct radius_cache *cache,struct gfx_mask_clip *clip,float x,float y) {
    if(!cache)return false;
    const struct radius_shape *s=radius_shape_mask(cache->shape,P->d->profile_enabled);
    if(!s){P->failed=cache->shape->rows!=0;return false;} /* without memory; an oversized shape goes unclipped */
    double left=cache->left+((double)x-cache->x),top=cache->top+((double)y-cache->y);
    if(!isfinite(left)||!isfinite(top)||left<INT_MIN||top<INT_MIN||left>INT_MAX||top>INT_MAX){P->failed=true;return false;}
    *clip=(struct gfx_mask_clip){.next=P->c->mask_clip,.alpha=radius_shape_alpha(s),.x=(int)lround(left),.y=(int)lround(top),.w=s->width,.h=s->height,
                                 .pitch=s->width,.opaque=s->mask,.repeat_at=s->repeat_at,.repeat_extra=s->repeat_extra};
    P->c->mask_clip=clip;
    web_avmedia_snapshot_blend_region(P->c,clip->x,clip->y,clip->w,clip->h);
    return true;
}

/* ---------------------------------------------------------------- clip-path and clip
   css-masking-1 §5.1 and §6.1. A box's clip cuts what it and every descendant paint, its outline
   and shadows too, and where they are hit; fixed descendants do not escape it. An inline box's
   fragments have no one reference box yet, so its clip-path does not apply. */
/* A box's coverage, prepared with the paint order: a rounded rectangle's radius mask, or the
   coverage of a polygon, a path or an SVG <clipPath>, drawn from markup whose user space starts at
   the document pixel (origin_x, origin_y). A box moved by whole pixels makes the same markup and
   keeps its coverage. */
struct clip_cache {
    struct radius_cache *round;
    char *markup; size_t markup_n;
    bool none;                   /* the url() names no <clipPath>: no clip-path at all */
    int origin_x, origin_y;
    uint8_t *alpha; int left, top, w, h; /* from origin + (left, top); NULL with w: too large, bounds only */
};
#define CLIP_COVERAGE_MAX (16u<<20) /* pixels */
void web_clip_cache_free(struct clip_cache **slot) {
    struct clip_cache *cache=*slot;*slot=NULL;
    if(cache){web_radius_cache_free(&cache->round);free(cache->markup);free(cache->alpha);free(cache);}
}
enum { CLIP_CUT_NONE, CLIP_CUT_ROUND, CLIP_CUT_COVERAGE };
/* b's clip in document space: nothing outside rect (x0 y0 x1 y1) paints, and within it a rounded
   rectangle (shape: x y w h, with radii) or b's coverage cuts further. */
struct clip_used { float rect[4]; uint8_t cut; float shape[4]; struct border_radii radii; };
static bool box_clips(const box_t *b) {
    if(!b->st||b->kind==B_INLINE||b->kind==B_TEXT||b->kind==B_BR)return false;
    return b->st->clip_path||(b->st->clip.rect&&(b->st->position==POS_ABSOLUTE||b->st->position==POS_FIXED));
}
static bool clips_above(const box_t *b){for(b=b->parent;b;b=b->parent)if(box_clips(b))return true;return false;}
/* A CSS_BOX_*'s rectangle (x y w h, from b's border box's top left) and the radii its corners take
   from the border radii (css-backgrounds-3 §5.3: inner boxes shrink them, the margin box grows
   curved ones); clip-path, shape-outside and offset-path's reference boxes. */
void box_reference_box(const box_t *b,uint8_t which,float r[4],float rx[4],float ry[4]) {
    r[0]=r[1]=0;
    r[2]=b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3];r[3]=b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];
    struct border_radii outer=b->st?used_radii(b->st,r[2],r[3]):(struct border_radii){0},radii=outer;
    float edges[4];
    switch(which) {
    case CSS_BOX_PADDING: memcpy(edges,b->b,sizeof edges);break;
    case CSS_BOX_CONTENT: case CSS_BOX_FILL: for(int i=0;i<4;i++)edges[i]=b->b[i]+b->p[i];break;
    case CSS_BOX_MARGIN: for(int i=0;i<4;i++)edges[i]=-b->m[i];break;
    default: memset(edges,0,sizeof edges);break; /* border-box; stroke-box and view-box are it for CSS boxes */
    }
    r[0]+=edges[3];r[1]+=edges[0];r[2]=fmaxf(0,r[2]-edges[1]-edges[3]);r[3]=fmaxf(0,r[3]-edges[0]-edges[2]);
    if(which==CSS_BOX_MARGIN) {
        static const int xs[4]={3,1,1,3},ys[4]={0,0,2,2};
        for(int i=0;i<4;i++)if(outer.x[i]>0&&outer.y[i]>0){
            radii.x[i]=fmaxf(0,outer.x[i]+b->m[xs[i]]);radii.y[i]=fmaxf(0,outer.y[i]+b->m[ys[i]]);
        }
        radii_normalize(&radii,r[2],r[3]);
    } else if(which==CSS_BOX_PADDING||which==CSS_BOX_CONTENT||which==CSS_BOX_FILL) radii=inset_radii(outer,edges,r[2],r[3]);
    memcpy(rx,radii.x,sizeof radii.x);memcpy(ry,radii.y,sizeof radii.y);
}
static void clip_reference(const box_t *b,uint8_t which,float r[4],struct border_radii *radii) {
    struct border_radii scratch;
    if(!radii)radii=&scratch;
    box_reference_box(b,which,r,radii->x,radii->y);
    r[0]+=box_visual_x(b)-b->p[3]-b->b[3];r[1]+=box_visual_y(b)-b->p[0]-b->b[0];
}
static void clip_rect_set(struct clip_used *u,double x0,double y0,double x1,double y1) {
    u->rect[0]=(float)x0;u->rect[1]=(float)y0;u->rect[2]=(float)fmax(x0,x1);u->rect[3]=(float)fmax(y0,y1);
}
static void clip_rect_meet(struct clip_used *u,double x0,double y0,double x1,double y1) {
    u->rect[0]=fmaxf(u->rect[0],(float)x0);u->rect[1]=fmaxf(u->rect[1],(float)y0);
    u->rect[2]=fmaxf(u->rect[0],fminf(u->rect[2],(float)x1));u->rect[3]=fmaxf(u->rect[1],fminf(u->rect[3],(float)y1));
}
/* False when b has no clip. Before b's coverage is drawn, a <clipPath>'s bounds are not known,
   and the rectangle does not limit it. The shape's geometry is shape.c's. */
static bool clip_used(const box_t *b,struct clip_used *u) {
    if(!box_clips(b))return false;
    const style_t *st=b->st;const struct css_shape *c=st->clip_path;
    u->rect[0]=u->rect[1]=-INFINITY;u->rect[2]=u->rect[3]=INFINITY;u->cut=CLIP_CUT_NONE;
    if(c&&!(b->clip_cache&&b->clip_cache->none)) {
        float r[4];struct border_radii radii;clip_reference(b,c->box,r,&radii);
        double x=r[0],y=r[1],w=r[2],h=r[3];
        switch(c->kind) {
        case CSS_SHAPE_BOX:
            clip_rect_set(u,x,y,x+w,y+h);
            if(radii_any(&radii)){u->cut=CLIP_CUT_ROUND;memcpy(u->shape,r,sizeof r);u->radii=radii;}
            break;
        case CSS_SHAPE_INSET: {
            double q[4];struct border_radii round;
            css_shape_inset(c,r,q,round.x,round.y);
            clip_rect_set(u,q[0],q[1],q[0]+q[2],q[1]+q[3]);
            radii_normalize(&round,(float)q[2],(float)q[3]);
            if(radii_any(&round)&&q[2]>0&&q[3]>0){
                u->cut=CLIP_CUT_ROUND;for(int i=0;i<4;i++)u->shape[i]=(float)q[i];u->radii=round;
            }
            break;
        }
        case CSS_SHAPE_CIRCLE: case CSS_SHAPE_ELLIPSE: {
            double centre[2],rr[2];css_shape_ellipse(c,r,centre,rr);
            double cx=centre[0],cy=centre[1],rx=rr[0],ry=rr[1];
            if(!(rx>0&&ry>0)){clip_rect_set(u,cx,cy,cx,cy);break;}
            clip_rect_set(u,cx-rx,cy-ry,cx+rx,cy+ry);
            u->cut=CLIP_CUT_ROUND;u->shape[0]=(float)(cx-rx);u->shape[1]=(float)(cy-ry);u->shape[2]=(float)(2*rx);u->shape[3]=(float)(2*ry);
            for(int i=0;i<4;i++){u->radii.x[i]=(float)rx;u->radii.y[i]=(float)ry;}
            break;
        }
        default: { /* polygon(), path(), shape(), url(): coverage */
            const struct clip_cache *cache=b->clip_cache;
            double q[4];
            u->cut=CLIP_CUT_COVERAGE;
            if(cache&&cache->markup)clip_rect_set(u,(double)cache->origin_x+cache->left,(double)cache->origin_y+cache->top,
                (double)cache->origin_x+cache->left+cache->w,(double)cache->origin_y+cache->top+cache->h);
            else if(c->kind!=CSS_SHAPE_URL) {
                if(css_shape_bounds(c,r,q))clip_rect_set(u,q[0],q[1],q[2],q[3]);else clip_rect_set(u,x,y,x,y);
            }
        }
        }
    }
    if(st->clip.rect&&(st->position==POS_ABSOLUTE||st->position==POS_FIXED)) {
        float r[4];clip_reference(b,CSS_BOX_BORDER,r,NULL);
        const struct css_clip_rect *k=&st->clip;
        clip_rect_meet(u,r[0]+(k->auto_edges&8?0:k->edge[3]),r[1]+(k->auto_edges&1?0:k->edge[0]),
                         r[0]+(k->auto_edges&2?r[2]:k->edge[1]),r[1]+(k->auto_edges&4?r[3]:k->edge[2]));
    }
    return true;
}
/* Whether the document point (x, y) is inside b's clip. */
static bool clip_contains(const box_t *b,const struct clip_used *u,float x,float y) {
    if(!(x>=u->rect[0]&&x<u->rect[2]&&y>=u->rect[1]&&y<u->rect[3]))return false;
    if(u->cut==CLIP_CUT_ROUND)return corners_coverage(x,y,u->shape[0],u->shape[1],u->shape[2],u->shape[3],&u->radii)>=.5f;
    const struct clip_cache *cache=b->clip_cache;
    if(u->cut!=CLIP_CUT_COVERAGE||!cache||!cache->alpha)return true;
    double px=floor(x)-cache->origin_x-cache->left,py=floor(y)-cache->origin_y-cache->top;
    return px>=0&&py>=0&&px<cache->w&&py<cache->h&&cache->alpha[(size_t)py*(size_t)cache->w+(size_t)px]>=128;
}
/* b's bounds cut by its clip; *valid turns false when nothing is left. */
static void clip_bounds(const box_t *b,float bounds[4],bool *valid) {
    struct clip_used u;if(!clip_used(b,&u))return;
    bounds[0]=fmaxf(bounds[0],u.rect[0]);bounds[1]=fmaxf(bounds[1],u.rect[1]);
    bounds[2]=fminf(bounds[2],u.rect[2]);bounds[3]=fminf(bounds[3],u.rect[3]);
    if(!(bounds[2]>bounds[0]&&bounds[3]>bounds[1]))*valid=false;
}
/* Pushes b's clip onto P's canvas: the rectangle into its clip (an antialiased cut keeps the pixels
   its edge crosses, a plain rectangle the pixels whose centres it holds) and, when painting, the cut
   as a mask. The caller restores both. False when b paints nothing. */
static bool clip_push(struct pctx *P,const box_t *b,const struct clip_used *u,struct gfx_mask_clip *mask) {
    double v[4];
    for(int i=0;i<4;i++) {
        v[i]=(double)u->rect[i]+(i&1?P->oy:P->ox);
        v[i]=u->cut?(i<2?floor(v[i]):ceil(v[i])):floor(v[i]+.5);
        v[i]=fmax(-1e9,fmin(1e9,v[i]));
    }
    if(!(v[2]>v[0]&&v[3]>v[1]))return false;
    canvas_t *c=P->c;gfx_clip(c,(int)v[0],(int)v[1],(int)(v[2]-v[0]),(int)(v[3]-v[1]));
    const struct clip_cache *cache=b->clip_cache;
    if(P->mode!=M_PAINT||!cache)return true;
    if(u->cut==CLIP_CUT_ROUND)radius_clip_push(P,cache->round,mask,P->ox+u->shape[0],P->oy+u->shape[1]);
    else if(u->cut==CLIP_CUT_COVERAGE&&cache->alpha) {
        double left=(double)P->ox+cache->origin_x+cache->left,top=(double)P->oy+cache->origin_y+cache->top;
        if(!(fabs(left)<1e9&&fabs(top)<1e9))return true;
        *mask=(struct gfx_mask_clip){.next=c->mask_clip,.alpha=cache->alpha,.x=(int)lround(left),.y=(int)lround(top),
                                     .w=cache->w,.h=cache->h,.pitch=cache->w};
        c->mask_clip=mask;
        web_avmedia_snapshot_blend_region(c,mask->x,mask->y,mask->w,mask->h);
    }
    return true;
}
static void clip_pop(canvas_t *c,const int saved[4],const struct gfx_mask_clip *prior) {
    c->cx0=saved[0];c->cy0=saved[1];c->cx1=saved[2];c->cy1=saved[3];c->mask_clip=prior;
}
/* nanosvg markup of b's polygon(), path(), shape() or <clipPath>, in document pixels from (ox, oy).
   False when a url() names no <clipPath>. */
static bool clip_markup(const box_t *b,const struct css_shape *c,const float r[4],double ox,double oy,sbuf *m) {
    if(c->kind==CSS_SHAPE_URL) {
        const float box[4]={(float)(r[0]-ox),(float)(r[1]-oy),r[2],r[3]};
        return b->node&&doc_svg_clip_markup(b->node,c->url,box,m);
    }
    sb_puts(m,"<svg xmlns=\"http://www.w3.org/2000/svg\"><path d=\"");
    css_shape_path_data(c,r,ox,oy,m); /* only commands and numbers */
    sb_puts(m,c->evenodd?"\" fill=\"#000\" fill-rule=\"evenodd\"/></svg>":"\" fill=\"#000\"/></svg>");
    return true;
}
static bool clip_cache_prepare(web_doc *d,box_t *b) {
    struct clip_used u;
    const struct css_shape *c=box_clips(b)?b->st->clip_path:NULL;
    if(!c){web_clip_cache_free(&b->clip_cache);return true;}
    struct clip_cache *cache=b->clip_cache;
    if(!cache&&!(cache=b->clip_cache=calloc(1,sizeof *cache)))return false;
    cache->none=false;clip_used(b,&u);
    if(u.cut==CLIP_CUT_ROUND)return radius_cache_prepare(d,&cache->round,u.shape[0],u.shape[1],u.shape[2],u.shape[3],u.radii);
    web_radius_cache_free(&cache->round);
    if(u.cut!=CLIP_CUT_COVERAGE){free(cache->markup);cache->markup=NULL;free(cache->alpha);cache->alpha=NULL;return true;}
    float r[4];clip_reference(b,c->box,r,NULL);
    double ox=floor(r[0]),oy=floor(r[1]);
    if(!(fabs(ox)<1e9&&fabs(oy)<1e9))return false;
    sbuf m={0};
    if(!clip_markup(b,c,r,ox,oy,&m)){sb_free(&m);free(cache->markup);cache->markup=NULL;free(cache->alpha);cache->alpha=NULL;cache->none=true;return true;}
    cache->origin_x=(int)ox;cache->origin_y=(int)oy;
    if(cache->markup&&m.p&&cache->markup_n==m.n&&!memcmp(cache->markup,m.p,m.n)){sb_free(&m);return true;}
    if(!web_native_checkpoint(d)){sb_free(&m);return false;}
    free(cache->markup);free(cache->alpha);cache->alpha=NULL;cache->markup=NULL;
    if(!m.p||!image_svg_coverage(m.p,m.n,CLIP_COVERAGE_MAX,&cache->alpha,&cache->left,&cache->top,&cache->w,&cache->h)){sb_free(&m);return false;}
    cache->markup=m.p;cache->markup_n=m.n;
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
    const uint8_t *column_same; /* vertical pass: source column x equals column x-1 */
    int width, height, radius;
};
/* One line of the separable Gaussian, `step` bytes between its samples.
   A rounded-rect mask is mostly runs of one value: where both windows are
   whole and every sample of [i-1-radius, i+radius] is equal, the sum at i is
   the same sum of the same products as at i-1, so it is copied. */
static void shadow_blur_line(const uint8_t *s,uint8_t *t,size_t step,int limit,const float *kernel,int radius) {
    int run=0; /* consecutive j ending at i+radius with s[j]==s[j-1] */
    for(int j=1;j<radius&&j<limit;j++)run=s[j*step]==s[(j-1)*step]?run+1:0;
    for(int i=0;i<limit;i++) {
        int head=i+radius;
        run=head<limit&&head>0&&s[head*step]==s[(head-1)*step]?run+1:0;
        if(i>radius&&head<limit&&run>2*radius){t[i*step]=t[(i-1)*step];continue;}
        float sum=0;int first=MAX(-radius,-i),last=MIN(radius,limit-i-1);
        for(int k=first;k<=last;k++)sum+=s[(size_t)(i+k)*step]*kernel[k+radius];
        t[i*step]=(uint8_t)fminf(255,floorf(sum+.5f));
    }
}
static void shadow_blur_rows(size_t tile, void *opaque) {
    const struct shadow_blur_job *job = opaque;
    int y0 = (int)tile * 32, y1 = MIN(y0 + 32, job->height);
    size_t width = (size_t)job->width;
    for (int y = y0; y < y1; y++) {
        const uint8_t *s = job->source + y*width; uint8_t *t = job->target + y*width;
        if (y > y0 && !memcmp(s, s - width, width)) memcpy(t, t - width, width);
        else shadow_blur_line(s, t, 1, job->width, job->kernel, job->radius);
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)job->width * (y1-y0));
}
static void shadow_blur_columns(size_t tile, void *opaque) {
    const struct shadow_blur_job *job = opaque;
    int x0 = (int)tile * 32, x1 = MIN(x0 + 32, job->width);
    size_t width = (size_t)job->width;
    for (int x = x0; x < x1; x++) {
        if (x > x0 && job->column_same[x])
            for (uint8_t *t = job->target + x, *end = t + (size_t)job->height*width; t < end; t += width) t[0] = t[-1];
        else shadow_blur_line(job->source + x, job->target + x, width, job->height, job->kernel, job->radius);
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)job->height * (x1-x0));
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
    float reach[4]; radii_reach(&radii, reach);
    int middle = -1; size_t pitch = (size_t)mask->width;
    for (int py = 0; py < mask->height; py++) {
        if (!(py & 31) && !web_native_checkpoint(doc)) return false;
        float fy = py + .5f;
        if (fy < sy-.5f || fy > sy+h+.5f) continue;
        uint8_t *row = mask->alpha + (size_t)py*pitch;
        bool plain = fy-sy >= reach[1] && sy+h-fy >= reach[3]; /* coverage depends on x alone */
        if (plain && middle >= 0) { memcpy(row, mask->alpha + (size_t)middle*pitch, pitch); continue; }
        if (plain) middle = py;
        int first = (int)fmaxf(0,fminf(mask->width,floorf(sx)));
        int last = (int)fmaxf(0,fminf(mask->width,ceilf(sx+w)));
        int mid0, mid1; radii_middle(sx+reach[0], sx+w-reach[2], mask->width, &mid0, &mid1);
        for (int px = first; px < last; px++) {
            uint8_t alpha = (uint8_t)(corners_coverage(px+.5f,fy,sx,sy,w,h,&radii)*255+.5f);
            if (px == mid0) { memset(row+mid0, alpha, (size_t)(mid1-mid0)); px = mid1-1; continue; }
            row[px] = alpha;
        }
    }
    if (shadow->blur <= 0) return true;
    int extent = (int)halo; size_t kernel_count = (size_t)extent*2+1;
    if (kernel_count > SIZE_MAX / sizeof(float)) return false;
    float *kernel = malloc(kernel_count*sizeof *kernel); uint8_t *temporary = malloc(pixels);
    uint8_t *column_same = malloc(pitch);
    if (!kernel || !temporary || !column_same) { free(kernel); free(temporary); free(column_same); return false; }
    double sigma = shadow->blur / 2.0, inverse = -.5 / (sigma*sigma), sum = 0;
    for (int i = -extent; i <= extent; i++) { kernel[i+extent] = (float)exp((double)i*i*inverse); sum += kernel[i+extent]; }
    float normalize = (float)(1/sum); for (size_t i = 0; i < kernel_count; i++) kernel[i] *= normalize;
    struct shadow_blur_job job = {mask->alpha,temporary,kernel,column_same,mask->width,mask->height,extent};
    parallel_for_stage(PARALLEL_RASTER, ((size_t)mask->height+31)/32, shadow_blur_rows, &job, shadow_service, doc);
    bool success = !__atomic_load_n(&doc->native_cancelled,__ATOMIC_ACQUIRE);
    if (success) {
        /* Columns clear of the corners are equal after the horizontal pass;
           the vertical pass blurs one of each run and copies it. */
        memset(column_same, 1, pitch); column_same[0] = 0;
        for (int py = 0; py < mask->height; py++) {
            const uint8_t *row = temporary + (size_t)py*pitch;
            for (size_t px = 1; px < pitch; px++) if (row[px] != row[px-1]) column_same[px] = 0;
        }
        job.source = temporary; job.target = mask->alpha;
        parallel_for_stage(PARALLEL_RASTER, (pitch+31)/32, shadow_blur_columns, &job, shadow_service, doc);
        success = !__atomic_load_n(&doc->native_cancelled,__ATOMIC_ACQUIRE);
    }
    free(kernel); free(temporary); free(column_same); return success;
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
        /* The box's coverage is exactly 0 half a pixel outside it and exactly
           1 past its corners' reach inside it; only the band between needs
           corners_coverage(). */
        float reach[4];radii_reach(&radii,reach);
        float out_x0=bx-.5f,out_x1=bx+bw+.5f,in_x0=bx+reach[0],in_x1=bx+bw-reach[2];
        /* Masks, allocation, Gaussian work and cancellation are completed in
           preflight. Do not introduce a new halfway-cancelled canvas write:
           ordinary (non-alpha-group) paint has no viewport rollback buffer. */
        for(int py=y0;py<y1;py++) {
            float fy=py+.5f;
            bool row_out=fy<=by-.5f||fy>=by+bh+.5f,row_in=fy>=by+reach[1]&&fy<=by+bh-reach[3];
            const uint8_t *row=mask->alpha&&py>=my&&py-my<mask->height?mask->alpha+(size_t)(py-my)*mask->width:NULL;
            /* An outer shadow is hidden wherever the box covers fully. */
            int hidden0=-1,hidden1=-1;
            if(!inset&&row_in)radii_middle(in_x0,in_x1,x1,&hidden0,&hidden1);
            for(int px=x0;px<x1;px++) {
                if(hidden0>=0&&px>=hidden0&&px<hidden1){px=hidden1-1;continue;}
                unsigned alpha=row&&px>=mx&&px-mx<mask->width?row[(size_t)(px-mx)]:0;
                if(inset)alpha=255-alpha;
                if(!alpha)continue;
                float fx=px+.5f,cover;
                if(row_out||fx<=out_x0||fx>=out_x1)cover=0;
                else if(row_in&&fx>=in_x0&&fx<=in_x1)cover=1;
                else cover=corners_coverage(fx,fy,bx,by,bw,bh,&radii);
                if(!inset)cover=1-cover;if(cover<=0)continue;
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
        /* a stop that names currentcolor, alone or in an expression (color.c) */
        o->col[i] = g->col[i] != COLOR_CURRENT ? g->col[i] : g->col_value[i] ? css_color_value_argb(g->col_value[i], cur) : cur;
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

/* A gradient laid over the rectangle x, y, w, h: where each point falls along it. */
struct gradient_sampler {
    const struct gradient *g;
    float cx, cy, dx, dy, len, rx, ry;
    struct gstops stops;
};
static void gradient_setup(struct gradient_sampler *s, const struct gradient *g, uint32_t cur, float x, float y, float w, float h) {
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
    s->g = g;
    s->cx = cx; s->cy = cy; s->dx = dx; s->dy = dy; s->len = len; s->rx = rx; s->ry = ry;
    resolve_stops(g, cur, len, &s->stops);
}
static float gradient_t(const struct gradient_sampler *s, float fx, float fy) {
    if (!s->g->radial) return ((fx - s->cx) * s->dx + (fy - s->cy) * s->dy) / s->len + 0.5f;
    float ex = (fx - s->cx) / s->rx, ey = (fy - s->cy) / s->ry;
    return sqrtf(ex * ex + ey * ey);
}
static uint32_t gradient_at(const struct gradient_sampler *s, float fx, float fy) { return stops_at(&s->stops, gradient_t(s, fx, fy)); }

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
    struct gradient_sampler G;
    gradient_setup(&G, g, cur, x, y, w, h);
    for (int py = Y0; py < Y1; py++) {
        float fy = (float)py + 0.5f;
        bool corner_row = round && (fy < y + r || fy > y + h - r);
        if (!g->radial && fabsf(G.dx) < 1e-6f && !corner_row) { /* vertical: one colour per row */
            uint32_t col = stops_at(&G.stops, ((fy - G.cy) * G.dy) / G.len + 0.5f);
            gfx_fill_blend(c, X0, py, X1 - X0, 1, col);
            continue;
        }
        for (int px = X0; px < X1; px++) {
            float fx = (float)px + 0.5f;
            uint32_t col = gradient_at(&G, fx, fy);
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

/* Where a paint at x,y puts the first pixel of the cache's mask. */
static void radius_origin(const struct radius_cache *cache,float x,float y,int64_t *left,int64_t *top) {
    *left=lround((double)cache->left+((double)x-cache->x));*top=lround((double)cache->top+((double)y-cache->y));
}
static unsigned radius_alpha(const struct radius_shape *s,int64_t x,int64_t y) {
    if(x<0||y<0||x>=s->width||y>=s->height)return 0;
    return radius_shape_alpha(s)[(size_t)gfx_mask_stored_row(s->repeat_at,s->repeat_extra,(int)y)*(size_t)s->width+(size_t)x];
}
static void paint_corner_border(struct pctx *P,float x,float y,float w,float h,const float *widths,const uint32_t *colors,
                                const struct radius_cache *outer_cache,const struct radius_cache *inner_cache) {
    canvas_t *c=P->c;
    /* A shape past RADIUS_MASK_MAX has no mask: its border is painted square. */
    const struct radius_shape *outer=radius_shape_mask(outer_cache->shape,P->d->profile_enabled);
    const struct radius_shape *inner=inner_cache?radius_shape_mask(inner_cache->shape,P->d->profile_enabled):NULL;
    if((!outer&&outer_cache->shape->rows)||(inner_cache&&!inner&&inner_cache->shape->rows)){P->failed=true;return;}
    int x0=(int)fmax(c->cx0,fmin(c->cx1,floor((double)x))),y0=(int)fmax(c->cy0,fmin(c->cy1,floor((double)y)));
    int x1=(int)fmax(c->cx0,fmin(c->cx1,ceil((double)x+w))),y1=(int)fmax(c->cy0,fmin(c->cy1,ceil((double)y+h)));
    int64_t outer_x=0,outer_y=0,inner_x=0,inner_y=0;
    if(outer)radius_origin(outer_cache,x,y,&outer_x,&outer_y);
    if(inner)radius_origin(inner_cache,x+widths[3],y+widths[0],&inner_x,&inner_y);
    /* Where the inner shape covers fully the border has no coverage; it is
       convex, so that is one run per row, which is skipped. */
    for(int py=y0;py<y1;py++) {
      int64_t skip0=0,skip1=0;
      if(inner){
          int64_t row=py-inner_y;
          if(row>=0&&row<inner->height){const struct gfx_mask_span *run=&inner->mask[gfx_mask_stored_row(inner->repeat_at,inner->repeat_extra,(int)row)];skip0=inner_x+run->x0;skip1=inner_x+run->x1;}
      } else if(py+.5f>=y+widths[0]&&py+.5f<y+h-widths[2]){skip0=(int64_t)ceilf(x+widths[3]-.5f);skip1=(int64_t)ceilf(x+w-widths[1]-.5f);}
      for(int px=x0;px<x1;px++) {
        if(px>=skip0&&px<skip1){px=(int)skip1-1;continue;}
        unsigned outside=outer?radius_alpha(outer,px-outer_x,py-outer_y):255;if(!outside)continue;
        unsigned inside=inner?radius_alpha(inner,px-inner_x,py-inner_y):
            (px+.5f>=x+widths[3]&&px+.5f<x+w-widths[1]&&py+.5f>=y+widths[0]&&py+.5f<y+h-widths[2]?255:0);
        float cover=outside*(255-inside)*(1.f/(255*255));if(cover<=0)continue;
        float distance[4]={py+.5f-y,x+w-px-.5f,y+h-py-.5f,px+.5f-x};int side=-1;
        for(int i=0;i<4;i++)if(widths[i]>0&&(side<0||distance[i]*widths[side]<distance[side]*widths[i]))side=i;
        if(side>=0)gfx_blend_pixel(c,px,py,with_alpha(colors[side],cover));
      }
    }
}
/* ---------------------------------------------------------------- backgrounds
   css-backgrounds-3 §3, css-backgrounds-4 §2-§3 and Compositing 1 §9-§10: a box's background colour
   and its painted image layer, each in its background-clip, the layer over its background-origin
   (or the viewport, or the scrolled contents: background-attachment), blended with the colour by
   background-blend-mode. */
static void paint_box(struct pctx *P, box_t *b, bool layer_root);
static void inline_children(struct pctx *P, box_t *b, bool floats);
static void paint_runs(struct pctx *P, box_t *b, box_t *scope);
static void paint_run_text(struct pctx *P, const struct run *r, float bx, float by);
static bool ink_outside(struct pctx *P, box_t *b, bool context);

/* What a background is the background of: a box (its own contents are what a text clip reads), or
   an inline box's fragment (the deco) among its block's runs; with its used padding. */
struct bg_target {
    box_t *box;
    box_t *block;
    const struct deco *deco;
    float pad[4];
    bool content_mask; /* the radius masks hold a content-box one (radius_clip[2]) */
};
/* any layer's background-clip is content-box: the box keeps a content-box radius mask */
static bool bg_content_clip(const style_t *st) {
    const struct css_bg_keywords *l = &css_style_background(st)->list[CSS_BG_CLIP];
    for (uint32_t i = 0; i < l->n; i++)
        if (l->v[i] == CSS_BOX_CONTENT) return true;
    return false;
}
/* B(Cb, Cs) of one colour channel, 0..1 (Compositing 1 §10.2) */
static float blend_hard_light(float b, float s) { return s <= .5f ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1); }
static float blend_channel(int mode, float b, float s) {
    switch (mode) {
    case CSS_BLEND_MULTIPLY: return b * s;
    case CSS_BLEND_SCREEN: return b + s - b * s;
    case CSS_BLEND_OVERLAY: return blend_hard_light(s, b);
    case CSS_BLEND_DARKEN: return fminf(b, s);
    case CSS_BLEND_LIGHTEN: return fmaxf(b, s);
    case CSS_BLEND_COLOR_DODGE: return b <= 0 ? 0 : s >= 1 ? 1 : fminf(1, b / (1 - s));
    case CSS_BLEND_COLOR_BURN: return b >= 1 ? 1 : s <= 0 ? 0 : 1 - fminf(1, (1 - b) / s);
    case CSS_BLEND_HARD_LIGHT: return blend_hard_light(b, s);
    case CSS_BLEND_SOFT_LIGHT: {
        if (s <= .5f) return b - (1 - 2 * s) * b * (1 - b);
        float d = b <= .25f ? ((16 * b - 12) * b + 4) * b : sqrtf(b);
        return b + (2 * s - 1) * (d - b);
    }
    case CSS_BLEND_DIFFERENCE: return fabsf(b - s);
    case CSS_BLEND_EXCLUSION: return b + s - 2 * b * s;
    }
    return s;
}
/* the non-separable modes' helpers (§10.3) */
static float blend_lum(const float c[3]) { return .3f * c[0] + .59f * c[1] + .11f * c[2]; }
static void blend_clip_color(float c[3]) {
    float l = blend_lum(c), n = fminf(c[0], fminf(c[1], c[2])), x = fmaxf(c[0], fmaxf(c[1], c[2]));
    if (n < 0 && l - n > 1e-9f) for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * l / (l - n);
    if (x > 1 && x - l > 1e-9f) for (int i = 0; i < 3; i++) c[i] = l + (c[i] - l) * (1 - l) / (x - l);
}
static void blend_set_lum(float c[3], float l) {
    float d = l - blend_lum(c);
    for (int i = 0; i < 3; i++) c[i] += d;
    blend_clip_color(c);
}
static float blend_sat(const float c[3]) { return fmaxf(c[0], fmaxf(c[1], c[2])) - fminf(c[0], fminf(c[1], c[2])); }
static void blend_set_sat(float c[3], float s) {
    int lo = 0, hi = 0;
    for (int i = 1; i < 3; i++) {
        if (c[i] < c[lo]) lo = i;
        if (c[i] >= c[hi]) hi = i;
    }
    if (lo == hi) hi = (lo + 1) % 3;
    int mid = 3 - lo - hi;
    if (c[hi] > c[lo]) {
        c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
        c[hi] = s;
    } else c[mid] = c[hi] = 0;
    c[lo] = 0;
}
static void blend_rgb(int mode, const float b[3], const float s[3], float out[3]) {
    float t[3];
    switch (mode) {
    case CSS_BLEND_HUE: memcpy(t, s, sizeof t); blend_set_sat(t, blend_sat(b)); blend_set_lum(t, blend_lum(b)); break;
    case CSS_BLEND_SATURATION: memcpy(t, b, sizeof t); blend_set_sat(t, blend_sat(s)); blend_set_lum(t, blend_lum(b)); break;
    case CSS_BLEND_COLOR: memcpy(t, s, sizeof t); blend_set_lum(t, blend_lum(b)); break;
    case CSS_BLEND_LUMINOSITY: memcpy(t, b, sizeof t); blend_set_lum(t, blend_lum(s)); break;
    default: for (int i = 0; i < 3; i++) t[i] = blend_channel(mode, b[i], s[i]);
    }
    memcpy(out, t, sizeof t);
}
/* A source pixel s (straight alpha) over the element's isolated background below it, the colour cb
   with coverage ab (Compositing 1 §5.2, §9.1): Cs' = (1 - ab)·Cs + ab·B(Cb, Cs), which drawn
   source-over with s's alpha composes the group onto the canvas. */
static uint32_t blend_pixel(int mode, uint32_t s, uint32_t cb, float ab) {
    if (ab <= 0 || !(s >> 24)) return s;
    float S[3], B[3], O[3];
    for (int i = 0; i < 3; i++) {
        S[i] = (float)((s >> (16 - 8 * i)) & 255) / 255;
        B[i] = (float)((cb >> (16 - 8 * i)) & 255) / 255;
    }
    blend_rgb(mode, B, S, O);
    uint32_t r = s & 0xFF000000u;
    for (int i = 0; i < 3; i++) {
        float v = (1 - ab) * S[i] + ab * O[i];
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        r |= (uint32_t)(v * 255 + .5f) << (16 - 8 * i);
    }
    return r;
}

/* What a background tile or a border image part samples: a decoded image's pixels, or a gradient
   laid over the image's rectangle (0, 0, w, h) (css-images-3 §2.4: a gradient has no natural size). */
struct paint_source { const image_t *img; struct gradient_sampler gradient; float w, h; };
/* The source's colour at (x, y) in its own pixels, from inside the rectangle clamp (x0, y0, x1, y1)
   only (a border image part does not bleed into its neighbours): an image's bilinearly, with
   straight alpha. */
static uint32_t source_at(const struct paint_source *s, float x, float y, const float clamp[4]) {
    if (!s->img) return gradient_at(&s->gradient, x, y);
    const image_t *im = s->img;
    int lx = (int)fmaxf(0, floorf(clamp[0])), ly = (int)fmaxf(0, floorf(clamp[1]));
    int hx = (int)fminf((float)im->w, ceilf(clamp[2])) - 1, hy = (int)fminf((float)im->h, ceilf(clamp[3])) - 1;
    if (hx < lx || hy < ly) return 0;
    float fx = x - .5f, fy = y - .5f, x0 = floorf(fx), y0 = floorf(fy), ax = fx - x0, ay = fy - y0;
    int xs[2] = {(int)x0, (int)x0 + 1}, ys[2] = {(int)y0, (int)y0 + 1};
    for (int i = 0; i < 2; i++) {
        xs[i] = xs[i] < lx ? lx : xs[i] > hx ? hx : xs[i];
        ys[i] = ys[i] < ly ? ly : ys[i] > hy ? hy : ys[i];
    }
    float weight[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay}, a = 0, c[3] = {0, 0, 0};
    for (int k = 0; k < 4; k++) {
        uint32_t p = im->px[(size_t)ys[k >> 1] * im->w + xs[k & 1]];
        float pa = weight[k] * (float)(p >> 24);
        a += pa;
        for (int i = 0; i < 3; i++) c[i] += pa * (float)((p >> (16 - 8 * i)) & 255);
    }
    if (a < .5f) return 0;
    uint32_t r = (uint32_t)(a + .5f) << 24;
    for (int i = 0; i < 3; i++) {
        float v = c[i] / a;
        r |= (uint32_t)(v > 255 ? 255 : v + .5f) << (16 - 8 * i);
    }
    return r;
}

/* The state of one background's painting. */
struct bg_paint {
    struct pctx *P;
    const style_t *st;
    const struct bg_target *T;
    struct radius_cache *const *rounded; /* border, padding (and content) masks, or NULL */
    float rect[3][4];                    /* the border, padding and content boxes on the canvas */
    float r;                             /* the uniform radius, without a mask */
    bool skip_color;                     /* the root's colour (and gradient) went to the canvas */
    uint8_t color_clip;                  /* the final layer's background-clip */
    uint32_t *text;                      /* text clip: the text's coverage over text_rect */
    int text_rect[4];
    bool text_tried;
};
/* One clip of a background's colour or layer: the rectangle and radius masks of its box, and for
   border-area and text the pixels before it, mixed back by the coverage after. */
struct bg_clip {
    int saved_rect[4];
    const struct gfx_mask_clip *prior;
    struct gfx_mask_clip mask;
    uint32_t *saved;
    int x, y, w, h;
    uint8_t clip;
};
static uint8_t bg_clip_box(uint8_t clip) {
    if (clip & (CSS_BG_CLIP_AREA | CSS_BG_CLIP_TEXT)) return CSS_BOX_BORDER;
    return clip <= CSS_BOX_CONTENT ? clip : CSS_BOX_BORDER;
}
/* The text that b and its in-flow (and floated) descendants paint, with the walk's own pctx (Q):
   paint_box's contents without the background, borders, outlines, markers or replaced content. */
static void text_mask_contents(struct pctx *Q, box_t *b) {
    if (!b->st || (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) || b->st->content_visibility == CV_HIDDEN) return;
    if (b->inline_ctx) {
        inline_children(Q, b, true);
        paint_runs(Q, b, NULL);
        inline_children(Q, b, false);
        return;
    }
    for (int floated = 0; floated < 2; floated++)
        for (box_t *ch = b->first; ch && !Q->failed; ch = ch->next)
            if (ch->floated == (bool)floated && !paint_box_layer(ch) && !ink_outside(Q, ch, false)) paint_box(Q, ch, false);
}
/* an inline box's fragment: the runs of its block that are its text, within it */
static void text_mask_runs(struct pctx *Q, box_t *b, const struct deco *deco) {
    float bx, by;
    layout_runs_origin(b, true, &bx, &by);
    for (int i = 0; i < b->nruns; i++) {
        const struct run *r = &b->runs[i];
        if (r->atomic || !r->node || r->paint_owner != deco->paint_owner) continue;
        float top, bottom;
        layout_run_extent(r, &top, &bottom);
        if (r->x + r->w <= deco->x || r->x >= deco->x + deco->w || bottom <= deco->y || top >= deco->y + deco->h) continue;
        node_t *n = r->node;
        while (n && n != deco->node) n = doc_flat_parent(n);
        if (n) paint_run_text(Q, r, bx, by);
    }
}
/* css-backgrounds-4 §2.1 text: the coverage of the text the target paints, over the canvas clip as
   it is (the border box's), drawn white on black by a walk of its contents. */
static bool bg_text(struct bg_paint *G) {
    if (G->text || G->text_tried) return G->text != NULL;
    G->text_tried = true;
    struct pctx *P = G->P;
    canvas_t *c = P->c;
    int x = c->cx0, y = c->cy0, w = c->cx1 - c->cx0, h = c->cy1 - c->cy0;
    if (w <= 0 || h <= 0) return false;
    if ((size_t)w > SIZE_MAX / sizeof *G->text / (size_t)h || !(G->text = malloc(sizeof *G->text * (size_t)w * (size_t)h))) {
        P->failed = true;
        return false;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) G->text[i] = 0xFF000000u;
    G->text_rect[0] = x; G->text_rect[1] = y; G->text_rect[2] = w; G->text_rect[3] = h;
    canvas_t mask;
    gfx_init(&mask, G->text, w, h, w);
    struct pctx Q = *P;
    Q.c = &mask;
    Q.ox -= (float)x; Q.oy -= (float)y;
    Q.vx0 -= x; Q.vx1 -= x; Q.vy0 -= y; Q.vy1 -= y;
    Q.viewport_mask = NULL;
    Q.text_mask = true;
    if (G->T->box) text_mask_contents(&Q, G->T->box);
    else if (G->T->deco && G->T->block) text_mask_runs(&Q, G->T->block, G->T->deco);
    if (Q.failed) {
        P->failed = true;
        free(G->text);
        G->text = NULL;
        return false;
    }
    return true;
}
static unsigned bg_text_at(const struct bg_paint *G, int x, int y) {
    if (!G->text) return 0;
    int64_t dx = (int64_t)x - G->text_rect[0], dy = (int64_t)y - G->text_rect[1];
    if (dx < 0 || dy < 0 || dx >= G->text_rect[2] || dy >= G->text_rect[3]) return 0;
    uint32_t p = G->text[(size_t)dy * (size_t)G->text_rect[2] + (size_t)dx];
    return (((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3;
}
/* the padding box's coverage of pixel (x, y): its radius mask, or the rectangle */
static unsigned bg_padding_at(const struct bg_paint *G, int x, int y) {
    const float *p = G->rect[CSS_BOX_PADDING];
    const struct radius_cache *cache = G->rounded ? G->rounded[1] : NULL;
    const struct radius_shape *s = cache ? radius_shape_mask(cache->shape, G->P->d->profile_enabled) : NULL;
    if (s) {
        int64_t left, top;
        radius_origin(cache, p[0], p[1], &left, &top);
        return radius_alpha(s, x - left, y - top);
    }
    float fx = (float)x + .5f, fy = (float)y + .5f;
    return fx >= p[0] && fx < p[0] + p[2] && fy >= p[1] && fy < p[1] + p[3] ? 255 : 0;
}
/* how much of pixel (x, y) the special clips (border-area, text) keep, 0..255 */
static unsigned bg_clip_coverage(const struct bg_paint *G, uint8_t clip, int x, int y) {
    unsigned a = 255;
    if (clip & CSS_BG_CLIP_TEXT) a = bg_text_at(G, x, y);
    if (a && (clip & CSS_BG_CLIP_AREA)) a = a * (255 - bg_padding_at(G, x, y)) / 255;
    return a;
}
/* The clip of a colour or a layer: true when it shows anything. rect: the border box's rectangle
   too (a colour in the border box is shaped by fill_rrect or the radius mask alone). bg_clip_end
   always follows. */
static bool bg_clip_begin(struct bg_paint *G, uint8_t clip, bool rect, struct bg_clip *s) {
    struct pctx *P = G->P;
    canvas_t *c = P->c;
    s->saved_rect[0] = c->cx0; s->saved_rect[1] = c->cy0; s->saved_rect[2] = c->cx1; s->saved_rect[3] = c->cy1;
    s->prior = c->mask_clip;
    s->saved = NULL;
    s->clip = clip;
    uint8_t box = bg_clip_box(clip);
    bool special = clip & (CSS_BG_CLIP_AREA | CSS_BG_CLIP_TEXT);
    const float *b = G->rect[box];
    if (b[2] <= 0 || b[3] <= 0) return false;
    if (rect || special || box != CSS_BOX_BORDER) {
        /* rounded edges, so a box at a fractional position shows no sliver of a neighbouring tile */
        int X0 = (int)roundf(b[0]), Y0 = (int)roundf(b[1]);
        gfx_clip(c, X0, Y0, (int)roundf(b[0] + b[2]) - X0, (int)roundf(b[1] + b[3]) - Y0);
    }
    if (G->rounded && box == CSS_BOX_PADDING) radius_clip_push(P, G->rounded[1], &s->mask, b[0], b[1]);
    else if (G->rounded && box == CSS_BOX_CONTENT && G->T->content_mask && G->rounded[2])
        radius_clip_push(P, G->rounded[2], &s->mask, P->ox + G->rounded[2]->x, P->oy + G->rounded[2]->y);
    if (P->failed || c->cx0 >= c->cx1 || c->cy0 >= c->cy1) return false;
    if (!special) return true;
    if ((clip & CSS_BG_CLIP_TEXT) && !bg_text(G)) return false;
    s->x = c->cx0; s->y = c->cy0; s->w = c->cx1 - c->cx0; s->h = c->cy1 - c->cy0;
    if ((size_t)s->w > SIZE_MAX / sizeof *s->saved / (size_t)s->h ||
        !(s->saved = malloc(sizeof *s->saved * (size_t)s->w * (size_t)s->h))) {
        P->failed = true;
        return false;
    }
    for (int j = 0; j < s->h; j++)
        memcpy(s->saved + (size_t)j * s->w, c->px + (size_t)(s->y + j) * c->pitch + s->x, sizeof *s->saved * (size_t)s->w);
    /* each pixel is mixed with what was under it by a coverage of its own */
    web_avmedia_snapshot_reject_region(c, s->x, s->y, s->w, s->h);
    return true;
}
static void bg_clip_end(struct bg_paint *G, struct bg_clip *s) {
    canvas_t *c = G->P->c;
    if (s->saved) {
        for (int j = 0; j < s->h; j++) {
            uint32_t *row = c->px + (size_t)(s->y + j) * c->pitch + s->x, *old = s->saved + (size_t)j * s->w;
            if (G->P->failed) { memcpy(row, old, sizeof *row * (size_t)s->w); continue; }
            for (int i = 0; i < s->w; i++) {
                unsigned a = bg_clip_coverage(G, s->clip, s->x + i, s->y + j);
                row[i] = a >= 255 ? row[i] : a ? gfx_mix(old[i], row[i], (int)a) : old[i];
            }
        }
        free(s->saved);
        s->saved = NULL;
    }
    c->cx0 = s->saved_rect[0]; c->cy0 = s->saved_rect[1]; c->cx1 = s->saved_rect[2]; c->cy1 = s->saved_rect[3];
    c->mask_clip = s->prior;
}
/* the background colour under a layer's pixel (x, y), for its blend: the colour's coverage there */
static float bg_backdrop(const struct bg_paint *G, int x, int y) {
    uint32_t col = G->st->bg_color;
    if (G->skip_color || !(col >> 24)) return 0;
    const float *b = G->rect[bg_clip_box(G->color_clip)];
    float fx = (float)x + .5f, fy = (float)y + .5f;
    if (!(fx >= b[0] && fx < b[0] + b[2] && fy >= b[1] && fy < b[1] + b[3])) return 0;
    return (float)(col >> 24) / 255 * (float)bg_clip_coverage(G, G->color_clip, x, y) / 255;
}
/* the colour, in the final layer's clip (css-backgrounds-3 §3.2) */
static void paint_bg_color(struct bg_paint *G) {
    uint32_t col = G->st->bg_color;
    if (G->skip_color || !(col >> 24)) return;
    struct pctx *P = G->P;
    canvas_t *c = P->c;
    struct bg_clip s;
    if (bg_clip_begin(G, G->color_clip, false, &s)) {
        const float *b = G->rect[0];
        if (bg_clip_box(G->color_clip) == CSS_BOX_BORDER) {
            if (G->rounded && G->rounded[0])
                gfx_fill_blend(c, (int)floorf(b[0]), (int)floorf(b[1]), (int)ceilf(b[0] + b[2]) - (int)floorf(b[0]),
                               (int)ceilf(b[1] + b[3]) - (int)floorf(b[1]), col);
            else fill_rrect(c, b[0], b[1], b[2], b[3], G->r, col, col);
        } else gfx_fill_blend(c, c->cx0, c->cy0, c->cx1 - c->cx0, c->cy1 - c->cy0, col);
    }
    bg_clip_end(G, &s);
}
/* background-size (css-backgrounds-3 §3.9): the tile's size in the positioning area aw by ah, of
   an image of natural size iw by ih, or (iw 0) of a gradient, which has none */
static void bg_tile_size(uint8_t kind, const len_t *size, float iw, float ih, float aw, float ah, float *tw, float *th) {
    bool natural = iw > 0 && ih > 0;
    *tw = natural ? iw : aw;
    *th = natural ? ih : ah;
    switch (kind) {
    case BSZ_COVER: case BSZ_CONTAIN:
        if (natural) {
            float sx = aw / iw, sy = ah / ih, s = kind == BSZ_COVER ? fmaxf(sx, sy) : fminf(sx, sy);
            *tw = iw * s;
            *th = ih * s;
        }
        break;
    case BSZ_LEN: {
        bool wa = len_auto(&size[0]), ha = len_auto(&size[1]);
        if (!wa) *tw = len_resolve(&size[0], aw);
        if (!ha) *th = len_resolve(&size[1], ah);
        if (natural && wa && !ha) *tw = *th * iw / ih;
        else if (natural && ha && !wa) *th = *tw * ih / iw;
        break;
    }
    }
}
/* image's tiles over the clip, the first at px, py */
static void draw_tiles(canvas_t *c, const image_t *img, float px, float py, int TW, int TH, bool rx, bool ry) {
    int x0 = (int)roundf(px), y0 = (int)roundf(py);
    int x1 = x0 + TW, y1 = y0 + TH;
    if (rx) {
        if (x0 > c->cx0) x0 -= (int)(((int64_t)x0 - c->cx0 + TW - 1) / TW) * TW;
        x1 = c->cx1;
    }
    if (ry) {
        if (y0 > c->cy0) y0 -= (int)(((int64_t)y0 - c->cy0 + TH - 1) / TH) * TH;
        y1 = c->cy1;
    }
    /* skip tiles above the clip */
    if (ry && y0 + TH <= c->cy0) y0 += (c->cy0 - y0) / TH * TH;
    if (rx && x0 + TW <= c->cx0) x0 += (c->cx0 - x0) / TW * TW;
    for (int ty = y0; ty < y1 && ty < c->cy1; ty += TH)
        for (int tx = x0; tx < x1 && tx < c->cx1; tx += TW) image_draw(c, img, tx, ty);
}
/* g drawn over an image of its own, w by h, straight alpha as decoded images are */
static image_t *gradient_image(const struct gradient *g, uint32_t cur, int w, int h) {
    if (w < 1 || h < 1 || (int64_t)w * h > IMAGE_MAX_PIXELS) return NULL;
    image_t *img = malloc(sizeof *img);
    if (!img) return NULL;
    if (!(img->px = malloc(sizeof *img->px * (size_t)w * (size_t)h))) { free(img); return NULL; }
    img->w = w;
    img->h = h;
    struct gradient_sampler s;
    gradient_setup(&s, g, cur, 0, 0, (float)w, (float)h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) img->px[(size_t)y * w + x] = gradient_at(&s, (float)x + .5f, (float)y + .5f);
    return img;
}
/* a gradient layer's tiles over the clip, the first at px, py: drawn in place, or once into a tile
   image when there are many small ones */
static void gradient_tiles(struct pctx *P, const struct gradient *g, uint32_t cur, float px, float py, float tw, float th,
                           bool rx, bool ry, float r) {
    canvas_t *c = P->c;
    float x0 = px, y0 = py;
    double nx = 1, ny = 1;
    if (rx) { x0 = px + floorf(((float)c->cx0 - px) / tw) * tw; nx = ceil(((double)c->cx1 - x0) / tw); }
    if (ry) { y0 = py + floorf(((float)c->cy0 - py) / th) * th; ny = ceil(((double)c->cy1 - y0) / th); }
    if (!(nx >= 1) || !(ny >= 1)) return;
    if (nx * ny > 16) {
        int TW = (int)fmaxf(1, roundf(tw)), TH = (int)fmaxf(1, roundf(th));
        if ((int64_t)TW * TH <= (1 << 20)) {
            image_t *img = gradient_image(g, cur, TW, TH);
            if (!img) { P->failed = true; return; }
            draw_tiles(c, img, px, py, TW, TH, rx, ry);
            image_free(img);
            return;
        }
    }
    for (double j = 0; j < ny; j++) {
        if (!web_native_checkpoint(P->d)) { P->failed = true; return; }
        for (double i = 0; i < nx; i++) fill_gradient(c, x0 + (float)i * tw, y0 + (float)j * th, tw, th, r, g, cur);
    }
}
/* a blended layer (background-blend-mode not normal): each pixel of the clip from its tile */
static void blend_tiles(struct bg_paint *G, const image_t *img, const struct gradient_sampler *gs, float px, float py,
                        float tw, float th, bool rx, bool ry, int mode) {
    struct pctx *P = G->P;
    canvas_t *c = P->c;
    uint32_t cb = G->st->bg_color;
    for (int y = c->cy0; y < c->cy1; y++) {
        if (!((y - c->cy0) & 63) && !web_native_checkpoint(P->d)) { P->failed = true; return; }
        float v = (float)y + .5f - py;
        if (ry) v -= floorf(v / th) * th;
        else if (v < 0 || v >= th) continue;
        for (int x = c->cx0; x < c->cx1; x++) {
            float u = (float)x + .5f - px;
            if (rx) u -= floorf(u / tw) * tw;
            else if (u < 0 || u >= tw) continue;
            uint32_t s;
            if (img) {
                int ix = (int)u, iy = (int)v;
                s = img->px[(size_t)(iy < img->h ? iy : img->h - 1) * img->w + (ix < img->w ? ix : img->w - 1)];
            } else s = gradient_at(gs, u, v);
            if (s >> 24) gfx_blend_pixel(c, x, y, blend_pixel(mode, s, cb, bg_backdrop(G, x, y)));
        }
    }
}
/* the painted image layer, st->bg_layer of the background-image list */
static void paint_bg_image(struct bg_paint *G) {
    struct pctx *P = G->P;
    web_doc *d = P->d;
    const style_t *st = G->st;
    canvas_t *c = P->c;
    const struct css_background *bg = css_style_background(st);
    uint32_t layer = st->bg_layer;
    struct web_image *im = NULL;
    const struct gradient *g = NULL;
    if (st->bg_img) {
        int idx = st->bg_img - 1;
        if (idx < 0 || idx >= d->images.n) return;
        im = d->images.v[idx];
        if (!im->img || im->img->w < 1 || im->img->h < 1) return;
    } else if (st->has_grad && st->gradient && !G->skip_color) g = st->gradient;
    else return;
    /* §3.7-§3.8: the positioning area */
    uint8_t attachment = css_bg_keyword(bg, CSS_BG_ATTACHMENT, layer), origin = css_bg_keyword(bg, CSS_BG_ORIGIN, layer);
    if (origin > CSS_BOX_CONTENT) origin = CSS_BOX_PADDING;
    float A[4];
    if (attachment == CSS_BG_FIXED) {
        A[0] = P->ox + (float)d->view_x; A[1] = P->oy + (float)d->view_y; A[2] = (float)d->width; A[3] = (float)d->height;
    } else {
        memcpy(A, G->rect[origin], sizeof A);
        box_t *b = G->T->box;
        if (attachment == CSS_BG_LOCAL && b && box_element_scrollable(b)) {
            /* the scrolling area in place of the padding box, scrolled with the contents */
            A[0] -= (float)b->node->scroll_x;
            A[1] -= (float)b->node->scroll_y;
            A[2] += fmaxf(0, b->scroll_w - G->rect[CSS_BOX_PADDING][2]);
            A[3] += fmaxf(0, b->scroll_h - G->rect[CSS_BOX_PADDING][3]);
        }
    }
    if (!(A[2] > 0) || !(A[3] > 0)) return;
    float tw, th;
    bg_tile_size(st->bg_size_kind, st->bg_size, im ? (float)im->img->w : 0, im ? (float)im->img->h : 0, A[2], A[3], &tw, &th);
    int TW = 0, TH = 0;
    if (im) {
        TW = (int)roundf(tw);
        TH = (int)roundf(th);
        if (TW < 1 || TH < 1) return;
        tw = (float)TW;
        th = (float)TH;
    } else if (!(tw > 0) || !(th > 0) || !isfinite(tw) || !isfinite(th)) return;
    float px = A[0] + css_bg_offset_px(&bg->position[0], layer, A[2] - tw), py = A[1] + css_bg_offset_px(&bg->position[1], layer, A[3] - th);
    bool rx = st->bg_repeat == BR_REPEAT || st->bg_repeat == BR_REPEAT_X;
    bool ry = st->bg_repeat == BR_REPEAT || st->bg_repeat == BR_REPEAT_Y;
    int mode = css_bg_keyword(bg, CSS_BG_BLEND, layer);
    uint8_t clip = css_bg_keyword(bg, CSS_BG_CLIP, layer);
    struct bg_clip s;
    if (bg_clip_begin(G, clip, true, &s)) {
        if (im) {
            image_t *img = scaled_image(im, TW, TH, c, c->cx0, c->cy0, c->cx1 - c->cx0, c->cy1 - c->cy0);
            if (img) {
                paint_drew(P, true);
                if (mode == CSS_BLEND_NORMAL) draw_tiles(c, img, px, py, TW, TH, rx, ry);
                else blend_tiles(G, img, NULL, roundf(px), roundf(py), tw, th, rx, ry, mode);
            }
        } else if (mode == CSS_BLEND_NORMAL) {
            /* one tile the size of the border box keeps the uniform radius's antialiased edge */
            const float *b = G->rect[0];
            bool whole = bg_clip_box(clip) == CSS_BOX_BORDER && px == b[0] && py == b[1] && tw == b[2] && th == b[3];
            gradient_tiles(P, g, st->color, px, py, tw, th, rx, ry, whole && !(G->rounded && G->rounded[0]) ? G->r : 0);
        } else {
            struct gradient_sampler gs;
            gradient_setup(&gs, g, st->color, 0, 0, tw, th);
            blend_tiles(G, NULL, &gs, px, py, tw, th, rx, ry, mode);
        }
    }
    bg_clip_end(G, &s);
}
/* a box's (or fragment's) background: x, y, w, h its border box on the canvas, bw its border widths */
static void paint_background(struct pctx *P, const style_t *st, const struct bg_target *T, float x, float y, float w,
                             float h, const float *bw, bool skip_bg, struct radius_cache *const *rounded, float r) {
    bool gradient = st->has_grad && st->gradient && !skip_bg;
    if (!st->bg_img && !gradient && (skip_bg || !(st->bg_color >> 24))) return;
    if (!skip_bg && ((st->bg_color >> 24) || gradient)) paint_drew(P, false);
    const struct css_background *bg = css_style_background(st);
    struct bg_paint G = {.P = P, .st = st, .T = T, .rounded = rounded, .r = r, .skip_color = skip_bg,
                         .color_clip = css_bg_keyword(bg, CSS_BG_CLIP, st->bg_layers ? st->bg_layers - 1u : 0u)};
    float *b = G.rect[0], *p = G.rect[1], *k = G.rect[2];
    b[0] = x; b[1] = y; b[2] = w; b[3] = h;
    p[0] = x + bw[3]; p[1] = y + bw[0]; p[2] = fmaxf(0, w - bw[1] - bw[3]); p[3] = fmaxf(0, h - bw[0] - bw[2]);
    k[0] = p[0] + T->pad[3]; k[1] = p[1] + T->pad[0];
    k[2] = fmaxf(0, p[2] - T->pad[1] - T->pad[3]); k[3] = fmaxf(0, p[3] - T->pad[0] - T->pad[2]);
    paint_bg_color(&G);
    if (!P->failed) paint_bg_image(&G);
    free(G.text);
}

/* ---------------------------------------------------------------- nine-slice images
   css-backgrounds-3 §6 (border-image) and css-masking-1 §8 (mask-border): an image sliced in nine,
   each part scaled and tiled into its region of the border image area. One plan and one drawer
   serve both: border-image draws onto the canvas, mask-border into a mask. */
/* A part: the source rectangle (image pixels), its region of the area, the tile's size, the first
   tile's position and the gap after each tile (space). */
struct nine_part { float sx, sy, sw, sh, x, y, w, h, tw, th, ox, oy, gx, gy; };
/* §6.6 steps 2-3 along one axis of a part's region (from start, size), its tile t: the tile's
   size, the first tile and the gap; false when no tile fits (space) */
static bool nine_slice_axis(uint8_t repeat, float start, float size, float *t, float *first, float *gap) {
    *first = start;
    *gap = 0;
    switch (repeat) {
    case CSS_BI_STRETCH: *t = size; return true;
    case CSS_BI_ROUND: {
        float n = roundf(size / *t);
        *t = size / (n < 1 ? 1 : n);
        return true;
    }
    case CSS_BI_REPEAT: *first = start + (size - *t) / 2; return true;
    case CSS_BI_SPACE: {
        float n = floorf(size / *t);
        if (n < 1) return false;
        *gap = (size - n * *t) / (n + 1);
        *first = start + *gap;
        return true;
    }
    }
    return true;
}
/* §6.6: the parts of an image iw by ih sliced at slice (top right bottom left, image pixels) into
   the area (x, y, w, h) with the border image widths width, repeat rh across and rv down; out[4]
   is the middle region even when it is not drawn. The bits of the parts that draw (the middle
   only with fill). */
static unsigned nine_slice_plan(float iw, float ih, const float slice[4], const float area[4], const float width[4],
                                uint8_t rh, uint8_t rv, bool fill, struct nine_part out[9]) {
    float s[4], W[4];
    for (int i = 0; i < 4; i++) {
        float dim = (i & 1) ? iw : ih;
        s[i] = slice[i] < 0 || !(slice[i] == slice[i]) ? 0 : slice[i] > dim ? dim : slice[i];
        W[i] = width[i] > 0 && isfinite(width[i]) ? width[i] : 0;
    }
    float aw = area[2] > 0 ? area[2] : 0, ah = area[3] > 0 ? area[3] : 0;
    /* §6.3: widths that overlap are reduced in proportion until they do not */
    float f = 1;
    if (W[1] + W[3] > 0) f = fminf(f, aw / (W[1] + W[3]));
    if (W[0] + W[2] > 0) f = fminf(f, ah / (W[0] + W[2]));
    if (f < 1) for (int i = 0; i < 4; i++) W[i] *= f;
    float rx[3] = {area[0], area[0] + W[3], area[0] + aw - W[1]}, rw[3] = {W[3], aw - W[3] - W[1], W[1]};
    float ry[3] = {area[1], area[1] + W[0], area[1] + ah - W[2]}, rhh[3] = {W[0], ah - W[0] - W[2], W[2]};
    float sx[3] = {0, s[3], iw - s[1]}, sw[3] = {s[3], iw - s[3] - s[1], s[1]};
    float sy[3] = {0, s[0], ih - s[2]}, sh[3] = {s[0], ih - s[0] - s[2], s[2]};
    /* §6.6 step 1: the edges scale in proportion to their border image widths; the middle as the
       top (else the bottom) across and the left (else the right) down */
    float scale[4];
    for (int i = 0; i < 4; i++) scale[i] = s[i] > 0 ? W[i] / s[i] : 0;
    float across = scale[0] > 0 && isfinite(scale[0]) ? scale[0] : scale[2] > 0 && isfinite(scale[2]) ? scale[2] : 1;
    float down = scale[3] > 0 && isfinite(scale[3]) ? scale[3] : scale[1] > 0 && isfinite(scale[1]) ? scale[1] : 1;
    unsigned parts = 0;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++) {
            int i = row * 3 + col;
            struct nine_part *p = &out[i];
            p->x = rx[col]; p->w = rw[col]; p->y = ry[row]; p->h = rhh[row];
            p->sx = sx[col]; p->sw = sw[col]; p->sy = sy[row]; p->sh = sh[row];
            p->tw = col != 1 ? p->w : p->sw * (row == 0 ? scale[0] : row == 2 ? scale[2] : across);
            p->th = row != 1 ? p->h : p->sh * (col == 0 ? scale[3] : col == 2 ? scale[1] : down);
            p->ox = p->x; p->oy = p->y; p->gx = p->gy = 0;
            bool shown = p->w > 0 && p->h > 0 && p->sw > 0 && p->sh > 0 && p->tw > 0 && p->th > 0 &&
                         isfinite(p->tw) && isfinite(p->th) && (i != 4 || fill);
            if (shown && col == 1) shown = nine_slice_axis(rh, p->x, p->w, &p->tw, &p->ox, &p->gx);
            if (shown && row == 1) shown = nine_slice_axis(rv, p->y, p->h, &p->th, &p->oy, &p->gy);
            if (shown && p->tw > 0 && p->th > 0) parts |= 1u << i;
        }
    return parts;
}
/* A nine-slice image ready to draw: its source, plan and parts. */
struct nine_slice { struct paint_source src; struct nine_part part[9]; unsigned parts; bool fill; uint8_t mode; };
/* the outsets of a border image whose border widths are bw (§6.5) */
static void border_image_outsets(const struct css_border_image *bi, const float *bw, float out[4]) {
    for (int i = 0; i < 4; i++) {
        const struct css_bi_side *o = &bi->outset[i];
        float v = o->kind == CSS_BI_NUMBER ? o->number * bw[i] : o->kind == CSS_BI_LENGTH ? len_resolve(&o->length, 0) : 0;
        out[i] = v > 0 && isfinite(v) ? v : 0;
    }
}
/* bi's source, slices, widths and plan over the border box x, y, w, h with border widths bw;
   image: the document's image of its source url (style_t's border_image_img or mask_border_img).
   false when its source shows nothing: none, or an image not (yet) decoded. */
static bool nine_slice_prepare(web_doc *d, const struct css_border_image *bi, int image, uint32_t cur, float x, float y,
                               float w, float h, const float *bw, struct nine_slice *N) {
    float o[4];
    border_image_outsets(bi, bw, o);
    float area[4] = {x - o[3], y - o[0], w + o[1] + o[3], h + o[0] + o[2]};
    if (!(area[2] > 0) || !(area[3] > 0)) return false;
    memset(N, 0, sizeof *N);
    float iw, ih;
    bool natural = false;
    if (bi->source.url) {
        int idx = image - 1;
        if (idx < 0 || idx >= d->images.n) return false;
        const struct web_image *im = d->images.v[idx];
        if (!im->img || im->img->w < 1 || im->img->h < 1) return false;
        N->src.img = im->img;
        iw = (float)im->img->w;
        ih = (float)im->img->h;
        natural = true;
    } else if (bi->source.gradient) {
        /* sized by the default sizing algorithm: the border image area */
        iw = area[2];
        ih = area[3];
        gradient_setup(&N->src.gradient, bi->source.gradient, cur, 0, 0, iw, ih);
    } else return false;
    N->src.w = iw;
    N->src.h = ih;
    float slice[4], width[4];
    for (int i = 0; i < 4; i++) {
        const struct css_bi_side *s = &bi->slice.side[i];
        float dim = (i & 1) ? iw : ih;
        slice[i] = s->kind == CSS_BI_NUMBER ? s->number : s->length.pct / 100 * dim;
        if (!(slice[i] > 0)) slice[i] = 0;
        if (slice[i] > dim) slice[i] = dim;
    }
    for (int i = 0; i < 4; i++) {
        const struct css_bi_side *s = &bi->width[i];
        if (s->kind == CSS_BI_NUMBER) width[i] = s->number * bw[i];
        else if (s->kind == CSS_BI_LENGTH) width[i] = len_resolve(&s->length, (i & 1) ? area[2] : area[3]);
        else width[i] = natural ? slice[i] : bw[i]; /* auto: the slice's natural size */
    }
    N->fill = bi->slice.fill;
    N->mode = bi->mode;
    N->parts = nine_slice_plan(iw, ih, slice, area, width, bi->repeat[0], bi->repeat[1], N->fill, N->part);
    return true;
}
/* Draw the parts into the pixels (x0, y0)-(x1, y1), each through put. false when cancelled. */
static bool nine_slice_draw(web_doc *d, const struct nine_slice *N, int x0, int y0, int x1, int y1,
                            void (*put)(void *, int, int, uint32_t), void *context) {
    for (int i = 0; i < 9; i++) {
        if (!(N->parts & (1u << i))) continue;
        const struct nine_part *p = &N->part[i];
        float clamp[4] = {p->sx, p->sy, p->sx + p->sw, p->sy + p->sh};
        int px0 = (int)fmaxf((float)x0, ceilf(p->x - .5f)), px1 = (int)fminf((float)x1, ceilf(p->x + p->w - .5f));
        int py0 = (int)fmaxf((float)y0, ceilf(p->y - .5f)), py1 = (int)fminf((float)y1, ceilf(p->y + p->h - .5f));
        float period_x = p->tw + p->gx, period_y = p->th + p->gy;
        for (int py = py0; py < py1; py++) {
            if (!((py - py0) & 63) && !web_native_checkpoint(d)) return false;
            float v = (float)py + .5f - p->oy;
            v -= floorf(v / period_y) * period_y;
            if (v >= p->th) continue;
            float sy = p->sy + v * p->sh / p->th;
            for (int px = px0; px < px1; px++) {
                float u = (float)px + .5f - p->ox;
                u -= floorf(u / period_x) * period_x;
                if (u >= p->tw) continue;
                uint32_t col = source_at(&N->src, p->sx + u * p->sw / p->tw, sy, clamp);
                if (col >> 24) put(context, px, py, col);
            }
        }
    }
    return true;
}
static void nine_put_canvas(void *context, int x, int y, uint32_t col) { gfx_blend_pixel(context, x, y, col); }
/* border-image (§6): drawn in place of the border styles when its source shows */
static bool paint_border_image(struct pctx *P, const style_t *st, float x, float y, float w, float h, const float *bw) {
    const struct css_border_image *bi = css_style_border_image(st);
    struct nine_slice N;
    if ((!bi->source.url && !bi->source.gradient) ||
        !nine_slice_prepare(P->d, bi, st->border_image_img, st->color, x, y, w, h, bw, &N)) return false;
    canvas_t *c = P->c;
    if (!N.parts) return true;
    paint_drew(P, false);
    if (!nine_slice_draw(P->d, &N, c->cx0, c->cy0, c->cx1, c->cy1, nine_put_canvas, c)) P->failed = true;
    return true;
}
/* mask-border (css-masking-1 §8): a mask over the pixels (x, y, w, h); the middle part is opaque
   white unless fill draws it, the rest of the area and beyond it masks out */
/* a mask pixel's value (§7.2, §8.2): its alpha, or with luminance its luminance (by Compositing's
   coefficients) times its alpha */
static unsigned mask_value(uint32_t col, bool luminance) {
    unsigned a = col >> 24;
    if (luminance)
        a = a * (unsigned)(.2125f * (float)((col >> 16) & 255) + .7154f * (float)((col >> 8) & 255) + .0721f * (float)(col & 255) + .5f) / 255;
    return a;
}
struct nine_mask { uint8_t *alpha; int x, y, w; uint8_t mode; };
static void nine_put_mask(void *context, int x, int y, uint32_t col) {
    struct nine_mask *m = context;
    m->alpha[(size_t)(y - m->y) * (size_t)m->w + (size_t)(x - m->x)] = (uint8_t)mask_value(col, m->mode == CSS_MASK_BORDER_LUMINANCE);
}
static uint8_t *mask_border_alpha(struct pctx *P, const struct nine_slice *N, int x, int y, int w, int h) {
    uint8_t *alpha = calloc((size_t)w, (size_t)h);
    if (!alpha) { P->failed = true; return NULL; }
    if (!N->fill) {
        const struct nine_part *m = &N->part[4];
        int mx0 = (int)fmaxf((float)x, ceilf(m->x - .5f)), mx1 = (int)fminf((float)(x + w), ceilf(m->x + m->w - .5f));
        int my0 = (int)fmaxf((float)y, ceilf(m->y - .5f)), my1 = (int)fminf((float)(y + h), ceilf(m->y + m->h - .5f));
        for (int j = my0; j < my1; j++) memset(alpha + (size_t)(j - y) * w + (mx0 - x), 255, mx1 > mx0 ? (size_t)(mx1 - mx0) : 0);
    }
    struct nine_mask m = {alpha, x, y, w, N->mode};
    if (!nine_slice_draw(P->d, N, x, y, x + w, y + h, nine_put_mask, &m)) { P->failed = true; free(alpha); return NULL; }
    return alpha;
}

/* ---------------------------------------------------------------- mask layers
   css-masking-1 §7: each layer's image sized, positioned and repeated over its mask-origin box as a
   background layer is (css-backgrounds-3 §3.4-§3.9), cut to its mask-clip box, its alpha or
   luminance the layer's mask; the layers composited from the bottom one up by mask-composite. */
/* what the layers do to a box: nothing (every layer none), mask it; or wait for an image still
   loading (the box is not painted until it comes, as browsers do); or a layer that is not drawn
   yet (a mask element, a conic gradient), which leaves the box unmasked */
enum { MASK_NONE, MASK_READY, MASK_PENDING, MASK_UNDRAWN };
/* layer i's image: drawn (*im an image, else *g a gradient), or false a transparent black layer
   (none, an image that failed, a reference to no mask element: §7.1) */
static bool mask_layer_source(web_doc *d, const box_t *b, const struct css_image *image, struct web_image **im,
                              const struct gradient **g, int *state) {
    *im = NULL;
    *g = NULL;
    if (!image) return false;
    if (image->kind != CSS_IMAGE_URL) {
        if (image->gradient) { *g = image->gradient; return true; }
        *state = MASK_UNDRAWN; /* a conic gradient, which no painter draws yet */
        return false;
    }
    if (doc_url_local(d, image->url)) {
        node_t *n = b->node ? doc_svg_reference(b->node, image->url) : NULL;
        if (n && n->foreign && !strcmp(n->name, "mask")) *state = MASK_UNDRAWN;
        return false;
    }
    if (!image->img) { *state = MASK_UNDRAWN; return false; } /* a style no image was found for */
    int idx = image->img - 1;
    if (idx < 0 || idx >= d->images.n) return false;
    struct web_image *w = d->images.v[idx];
    if (w->img && w->img->w > 0 && w->img->h > 0) { *im = w; return true; }
    if (!w->done && !w->failed && *state != MASK_UNDRAWN) *state = MASK_PENDING;
    return false;
}
static int mask_layers_state(web_doc *d, const box_t *b) {
    const struct css_mask *m = css_style_mask(b->st);
    if (!css_mask_layers_set(m)) return MASK_NONE;
    int state = MASK_READY;
    for (uint32_t i = 0; i < m->image.n && state != MASK_UNDRAWN; i++) {
        struct web_image *im;
        const struct gradient *g;
        mask_layer_source(d, b, m->image.v[i], &im, &g, &state);
    }
    return state;
}
/* a <coord-box> of an element with a CSS box: fill-box is its content box, stroke-box and view-box
   its border box (§7.5) */
static uint8_t mask_box(uint8_t box) {
    return box == CSS_BOX_FILL ? CSS_BOX_CONTENT : box > CSS_BOX_CONTENT ? CSS_BOX_BORDER : box;
}
/* one axis of an image layer's <repeat-style> (css-backgrounds-3 §3.4), the tile t already rounded
   for round (§3.9): the first tile's start (*first: the position's, unless space moves it) and the
   period of the tiles (0: one tile) over the positioning area (start, size). The mask layers use it;
   background-repeat's space and round are not drawn yet (paint_bg_image repeats them). */
static void layer_repeat_axis(uint8_t repeat, float start, float size, float t, float *first, float *period) {
    *period = repeat == CSS_REPEAT || repeat == CSS_REPEAT_ROUND ? t : 0;
    if (repeat != CSS_REPEAT_SPACE) return;
    /* space: as many tiles as fit, the first and last touching the edges; one fits: placed by the
       position */
    float n = floorf(size / t + 1e-4f);
    if (n < 2) return;
    *first = start;
    *period = t + (size - n * t) / (n - 1);
}
/* layer i's mask over the pixels (x, y, w, h) into out (zeroed), the boxes rect (border, padding,
   content) on the canvas; false when cancelled */
static bool mask_layer_draw(struct pctx *P, const box_t *b, const struct css_mask *m, uint32_t i, const float rect[3][4],
                            int x, int y, int w, int h, uint8_t *out) {
    struct web_image *im;
    const struct gradient *g;
    int state = MASK_READY;
    if (!mask_layer_source(P->d, b, m->image.v[i], &im, &g, &state)) return true;
    const float *A = rect[mask_box(css_mask_keyword(m, CSS_MASK_ORIGIN, i))];
    if (!(A[2] > 0) || !(A[3] > 0)) return true;
    const struct css_layer_size *z = &m->size.v[i % m->size.n];
    float iw = im ? (float)im->img->w : 0, ih = im ? (float)im->img->h : 0, tw, th;
    bg_tile_size(z->kind, z->v, iw, ih, A[2], A[3], &tw, &th);
    uint8_t repeat = css_mask_keyword(m, CSS_MASK_REPEAT, i), rx = repeat >> 2 & 3, ry = repeat & 3;
    /* round (§3.9): a whole number of tiles; the other axis keeps the ratio when its size is auto */
    bool auto_w = z->kind == BSZ_AUTO || (z->kind == BSZ_LEN && len_auto(&z->v[0]));
    bool auto_h = z->kind == BSZ_AUTO || (z->kind == BSZ_LEN && len_auto(&z->v[1]));
    float tw0 = tw, th0 = th;
    if (rx == CSS_REPEAT_ROUND && tw > 0) tw = A[2] / fmaxf(1, roundf(A[2] / tw));
    if (ry == CSS_REPEAT_ROUND && th > 0) th = A[3] / fmaxf(1, roundf(A[3] / th));
    if (rx == CSS_REPEAT_ROUND && ry != CSS_REPEAT_ROUND && auto_h && tw0 > 0) th = th0 * tw / tw0;
    if (ry == CSS_REPEAT_ROUND && rx != CSS_REPEAT_ROUND && auto_w && th0 > 0) tw = tw0 * th / th0;
    if (!(tw > 0) || !(th > 0) || !isfinite(tw) || !isfinite(th)) return true;
    float px = A[0] + css_bg_offset_px(&m->position[0], i, A[2] - tw), py = A[1] + css_bg_offset_px(&m->position[1], i, A[3] - th);
    float period_x, period_y;
    layer_repeat_axis(rx, A[0], A[2], tw, &px, &period_x);
    layer_repeat_axis(ry, A[1], A[3], th, &py, &period_y);
    /* the clip: its box's pixels (rounded edges, as a background layer's), or with no-clip all */
    int X0 = x, Y0 = y, X1 = x + w, Y1 = y + h;
    uint8_t clip = css_mask_keyword(m, CSS_MASK_CLIP, i);
    if (clip != CSS_MASK_NO_CLIP) {
        const float *C = rect[mask_box(clip)];
        X0 = (int)fmaxf((float)X0, roundf(C[0])); Y0 = (int)fmaxf((float)Y0, roundf(C[1]));
        X1 = (int)fminf((float)X1, roundf(C[0] + C[2])); Y1 = (int)fminf((float)Y1, roundf(C[1] + C[3]));
    }
    /* one tile on an axis that does not repeat */
    if (!period_x) { X0 = (int)fmaxf((float)X0, ceilf(px - .5f)); X1 = (int)fminf((float)X1, ceilf(px + tw - .5f)); }
    if (!period_y) { Y0 = (int)fmaxf((float)Y0, ceilf(py - .5f)); Y1 = (int)fminf((float)Y1, ceilf(py + th - .5f)); }
    if (X0 >= X1 || Y0 >= Y1) return true;
    const image_t *src = NULL;
    struct gradient_sampler gs;
    if (im) {
        /* the document image's cached size (scaled_image), else its decoded pixels sampled */
        int TW = (int)fmaxf(1, roundf(tw)), TH = (int)fmaxf(1, roundf(th));
        src = (int64_t)TW * TH <= IMAGE_MAX_PIXELS ? scaled_image(im, TW, TH, P->c, X0, Y0, X1 - X0, Y1 - Y0) : NULL;
        if (!src) src = im->img;
    } else gradient_setup(&gs, g, b->st->color, 0, 0, tw, th);
    bool luminance = css_mask_keyword(m, CSS_MASK_MODE, i) == CSS_MASK_LUMINANCE; /* match-source: alpha */
    float sx = src ? (float)src->w / tw : 0, sy = src ? (float)src->h / th : 0;
    for (int j = Y0; j < Y1; j++) {
        if (!((j - Y0) & 63) && !web_native_checkpoint(P->d)) return false;
        float v = (float)j + .5f - py;
        if (period_y) v -= floorf(v / period_y) * period_y;
        if (v < 0 || v >= th) continue;
        uint8_t *row = out + (size_t)(j - y) * (size_t)w;
        int iy = src ? (int)(v * sy) : 0;
        if (src && iy >= src->h) iy = src->h - 1;
        for (int k = X0; k < X1; k++) {
            float u = (float)k + .5f - px;
            if (period_x) u -= floorf(u / period_x) * period_x;
            if (u < 0 || u >= tw) continue;
            uint32_t col;
            if (src) {
                int ix = (int)(u * sx);
                col = src->px[(size_t)iy * (size_t)src->w + (size_t)(ix < src->w ? ix : src->w - 1)];
            } else col = gradient_at(&gs, u, v);
            row[k - x] = (uint8_t)mask_value(col, luminance);
        }
    }
    return true;
}
/* mask-composite (§7.8, Compositing 1 §9.1.1): a layer's mask s over the mask d of the layers below
   it, 0..255 */
static uint8_t mask_composite(uint8_t op, unsigned s, unsigned d) {
    unsigned v = op == CSS_COMPOSITE_ADD ? s * 255 + d * (255 - s) :      /* source over */
                 op == CSS_COMPOSITE_SUBTRACT ? s * (255 - d) :          /* source out */
                 op == CSS_COMPOSITE_INTERSECT ? s * d :                 /* source in */
                 s * (255 - d) + d * (255 - s);                          /* xor */
    return (uint8_t)((v + 127) / 255);
}
/* the layers' mask over the pixels (x, y, w, h), 0..255 each, of b whose border box is (bx, by, bw,
   bh) on the canvas; NULL (P->failed) when there is no memory or the paint is cancelled */
static uint8_t *mask_layers_alpha(struct pctx *P, const box_t *b, float bx, float by, float bw, float bh, int x, int y,
                                  int w, int h) {
    const struct css_mask *m = css_style_mask(b->st);
    float rect[3][4] = {{bx, by, bw, bh}};
    rect[1][0] = bx + b->b[3]; rect[1][1] = by + b->b[0];
    rect[1][2] = fmaxf(0, bw - b->b[1] - b->b[3]); rect[1][3] = fmaxf(0, bh - b->b[0] - b->b[2]);
    rect[2][0] = rect[1][0] + b->p[3]; rect[2][1] = rect[1][1] + b->p[0];
    rect[2][2] = fmaxf(0, rect[1][2] - b->p[1] - b->p[3]); rect[2][3] = fmaxf(0, rect[1][3] - b->p[0] - b->p[2]);
    size_t size = (size_t)w * (size_t)h;
    uint8_t *result = calloc(size ? size : 1, 1), *layer = m->image.n > 1 ? malloc(size ? size : 1) : NULL;
    if (!result || (m->image.n > 1 && !layer)) { free(result); free(layer); P->failed = true; return NULL; }
    /* the bottom layer (the last) is composited onto nothing: its operator is not used (§7.8) */
    bool ok = mask_layer_draw(P, b, m, m->image.n - 1, rect, x, y, w, h, result);
    for (uint32_t i = m->image.n - 1; ok && i-- > 0;) {
        memset(layer, 0, size);
        if (!(ok = mask_layer_draw(P, b, m, i, rect, x, y, w, h, layer))) break;
        uint8_t op = css_mask_keyword(m, CSS_MASK_COMPOSITE, i);
        for (size_t k = 0; k < size; k++) result[k] = mask_composite(op, layer[k], result[k]);
    }
    free(layer);
    if (!ok) { free(result); P->failed = true; return NULL; }
    return result;
}

/* background and borders of a border box (x, y, w, h on the canvas), of what T says */
static void paint_bg_border(struct pctx *P, const style_t *st, float x, float y, float w, float h, const float *bw,
                            bool skip_bg, bool open_l, bool open_r, const struct shadow_cache *shadows,struct radius_cache *const *rounded,
                            const struct bg_target *T) {
    if (P->text_mask) return; /* a text clip's walk (bg_text) draws the text alone */
    canvas_t *c = P->c;
    /* Culling the primitive, not the subtree: visible-overflow and fixed
     * descendants must still be walked, and offscreen resources stay wanted. */
    paint_shadows(P,shadows,x,y,false);
    if(P->failed)return;
    /* a border image reaches past the border box by its outsets */
    float o[4] = {0, 0, 0, 0};
    const struct css_border_image *bi = css_style_border_image(st);
    if (bi->source.url || bi->source.gradient) border_image_outsets(bi, bw, o);
    if (x - o[3] >= c->cx1 || y - o[0] >= c->cy1 || x + w + o[1] <= c->cx0 || y + h + o[2] <= c->cy0) return;
    bool shown = !(x >= c->cx1 || y >= c->cy1 || x + w <= c->cx0 || y + h <= c->cy0);
    float r = used_radius(st, w, h);
    const struct gfx_mask_clip *previous=c->mask_clip;struct gfx_mask_clip boundary;
    if(rounded&&shown)radius_clip_push(P,rounded[0],&boundary,x,y);
    if (shown) paint_background(P, st, T, x, y, w, h, bw, skip_bg, rounded, r);
    c->mask_clip=previous;
    if(P->failed)return;
    paint_shadows(P,shadows,x,y,true);
    if(P->failed)return;
    if (paint_border_image(P, st, x, y, w, h, bw) || !shown) return;
    const uint32_t *bc = st->border_color;
    bool any = false;
    for (int i = 0; i < 4; i++)
        if (bw[i] > 0 && (bc[i] >> 24) && st->border_style[i] > BS_HIDDEN) any = true;
    if (!any) return;
    paint_drew(P, false);
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
    if (r->highlight && r->highlight_first < r->highlight_end && !P->text_mask) {
        /* Only the matched bytes of the run, measured as the line placed them. */
        float x0 = layout_run_x(r, (size_t)r->highlight_first), x1 = layout_run_x(r, (size_t)r->highlight_end);
        gfx_fill_blend(P->c, (int)(x + x0), (int)(base - asc), (int)ceilf(x1 - x0), (int)ceilf(asc + desc), RGB(255, 213, 0));
    }
    /* Text is contentful even in a transparent colour (Paint Timing). */
    if (r->n > 0) paint_drew(P, contentful_text(r->s, (size_t)r->n));
    uint32_t col = P->text_mask ? 0xFFFFFFFFu : st->color; /* a text clip's coverage, whatever its colour */
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
    const size_t kept = sizeof im->scaled / sizeof *im->scaled;
    for (size_t i = 0; i < kept; i++) {
        image_t *s = im->scaled[i];
        if (!s || s->w != w || s->h != h) continue;
        memmove(im->scaled + 1, im->scaled, i * sizeof *im->scaled);
        im->scaled[0] = s;
        return s;
    }
    if(web_avmedia_snapshot_is_probe()){
        /* An uncached image outside the committed video span must not disable
           that surface. Probes neither allocate nor replace shared caches;
           only an intersecting paint occurrence makes their result unsafe. */
        web_avmedia_snapshot_reject_region(canvas,x,y,box_w,box_h);
        return NULL;
    }
    if ((long)w * h > IMAGE_MAX_PIXELS) return NULL;
    image_t *made = im->svg ? image_decode_svg(im->svg, im->svg_n, w, h) : image_scale(im->img, w, h);
    if (!made) return NULL;
    image_free(im->scaled[kept - 1]);
    memmove(im->scaled + 1, im->scaled, (kept - 1) * sizeof *im->scaled);
    im->scaled[0] = made;
    return made;
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
/* ---- where a text control shows its value. paint_control draws from these; the caret
   positions and caret rects of caretPositionFromPoint read them, so both agree. */
/* An <input>'s one line: the text it draws for text (a bullet for each character of a
   password) and the scroll that keeps the caret at byte caret (or none, -1) in view. */
struct input_line { sbuf shown; const char *text, *drawn; bool password; float scroll; };
static float input_line_x(const struct input_line *L, const style_t *st, size_t byte) {
    wfont f = style_font(st);
    size_t at = L->password ? password_offset(L->text, byte) : byte, n = strlen(L->drawn);
    return wf_width(&f, L->drawn, at < n ? at : n);
}
static void input_line_begin(struct input_line *L, const style_t *st, const char *text, bool password,
                             float width, int caret) {
    *L = (struct input_line){.text = text, .password = password};
    if (password) for (const char *p = text; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) sb_puts(&L->shown, "\xe2\x80\xa2");
    L->drawn = !password ? text : L->shown.p ? sb_cstr(&L->shown) : "";
    if (caret >= 0) {
        float caret_w = input_line_x(L, st, (size_t)caret);
        L->scroll = caret_w > width - 2 ? caret_w - width + 2 : 0;
    }
}
static void input_line_end(struct input_line *L) { sb_free(&L->shown); }
static float input_measure(const void *opaque, const char *s, size_t at, size_t next, float x) {
    (void)x;
    return wf_width(opaque, s + at, next - at);
}
/* The byte of the input's text nearest dx from the start of its drawn text. */
static size_t input_line_byte(const struct input_line *L, const style_t *st, float dx) {
    wfont f = style_font(st);
    size_t at = layout_text_boundary(L->drawn, strlen(L->drawn), -1, dx, input_measure, &f);
    if (!L->password) return at;
    /* Each character of the value drew one 3-byte bullet. */
    size_t bullets = at / 3, byte = 0;
    while (L->text[byte] && bullets) {
        byte++;
        while (((unsigned char)L->text[byte] & 0xC0) == 0x80) byte++;
        bullets--;
    }
    return byte;
}
static bool input_password(node_t *n) {
    const char *type = node_attr(n, "type");
    return type && str_ieq(type, "password");
}
/* A <textarea>'s line holding a point (the first line whose bottom is below it, else the
   last) or a caret (the line paint_control draws it on). */
struct textarea_find { size_t byte, length, first, line_length; float y, top, height; bool point, found; };
static void textarea_find_line(const struct control_text_line *line, void *opaque) {
    struct textarea_find *f = opaque;
    if (f->found) return;
    bool take;
    if (f->point) { take = true; f->found = f->y < line->top + line->height; }
    else take = f->found = f->byte >= line->first && f->byte <= line->first + line->length &&
        (f->byte < line->first + line->length || line->hard_break || line->next == f->length);
    if (take) { f->first = line->first; f->line_length = line->length; f->top = line->top; f->height = line->height; }
}
static bool text_control_box(const box_t *b) {
    return b && b->kind == B_ATOMIC && b->node &&
        (b->atomic == AT_TEXTAREA || (b->atomic == AT_INPUT && web_input_type(b->node) != WEB_INPUT_FILE));
}
/* The UTF-16 offset in control b's value nearest point (x, y) of b's own space. */
static bool control_offset_at(web_doc *d, const box_t *b, float x, float y, uint32_t *offset) {
    node_t *n = b->node;
    const char *v = web_input_edit_text(n);
    *offset = 0;
    if (!v || !*v) return true;
    float left = box_visual_x(b), top = cy(b);
    size_t byte;
    if (b->atomic == AT_INPUT) {
        struct input_line L;
        input_line_begin(&L, b->st, v, input_password(n), b->w, d->focus == n ? d->caret : -1);
        byte = input_line_byte(&L, b->st, x - (left - L.scroll));
        input_line_end(&L);
    } else {
        if (box_element_scrollable(b)) { left -= (float)n->scroll_x; top -= (float)n->scroll_y; }
        struct textarea_find f = {.length = strlen(v), .y = y - top, .point = true};
        float width, height;
        if (!control_text_lines(b, b->st, v, textarea_find_line, &f, &width, &height)) return false;
        byte = f.first + control_text_boundary(b->st, v + f.first, f.line_length, x - left);
    }
    *offset = doc_text_utf16(v, strlen(v), byte);
    return true;
}
bool doc_control_caret_rect(node_t *control, uint32_t offset, double rect[4]) {
    box_t *b = control ? control->box : NULL;
    if (!text_control_box(b) || b->node != control || !control->owner) return false;
    const char *v = web_input_edit_text(control);
    if (!v) v = "";
    size_t length = strlen(v), byte = doc_text_byte(v, length, offset, false);
    double left = box_visual_x(b), top = cy(b), height = b->h;
    if (b->atomic == AT_INPUT) {
        web_doc *d = control->owner;
        struct input_line L;
        input_line_begin(&L, b->st, v, input_password(control), b->w, d->focus == control && *v ? d->caret : -1);
        left += input_line_x(&L, b->st, byte) - L.scroll;
        input_line_end(&L);
    } else {
        if (box_element_scrollable(b)) { left -= control->scroll_x; top -= control->scroll_y; }
        struct textarea_find f = {.byte = byte, .length = length, .height = style_line_height(b->st)};
        float width, lines_height;
        if (!control_text_lines(b, b->st, v, textarea_find_line, &f, &width, &lines_height)) return false;
        if (f.found) {
            left += control_text_width(b->st, v + f.first, byte - f.first);
            top += f.top;
        }
        height = f.height;
    }
    rect[0] = left; rect[1] = top; rect[2] = 0; rect[3] = height;
    box_transform_rect(b, rect);
    return isfinite(rect[0]) && isfinite(rect[1]) && isfinite(rect[2]) && isfinite(rect[3]);
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
        bool pw = input_password(n) && !placeholder;
        uint32_t col = control_text_color(st, placeholder);
        if (placeholder && !(col >> 24) && b->atomic == AT_INPUT) v = "";
        if (!placeholder) paint_drew(P, contentful_text(v, strlen(v))); /* an input's value */
        size_t selection_start = !placeholder ? doc_utf16_to_byte(v, n->selection_start, false) : 0;
        size_t selection_end = !placeholder ? doc_utf16_to_byte(v, n->selection_end, true) : 0;
        bool selected = focused && !placeholder && n->selection_start != n->selection_end;
        float caret_x = x;
        if (b->atomic == AT_INPUT) {
            /* The caret stays in view; caret positions and rects scroll the same way. */
            struct input_line L;
            int caret = focused && !placeholder ? P->d->caret : -1;
            input_line_begin(&L, st, v, pw, w, caret);
            const char *txt = L.drawn;
            float scroll = L.scroll;
            float left = 0, right = 0;
            if (selected) {
                left = x - scroll + input_line_x(&L, st, selection_start);
                right = x - scroll + input_line_x(&L, st, selection_end);
                gfx_fill(c, (int)floorf(left), (int)y, (int)ceilf(right - left), (int)ceilf(h), RGB(0, 117, 255));
            }
            text_in(P, text_style, x - scroll, y, h, txt, col);
            if (selected) {
                int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
                gfx_clip(c, (int)floorf(left), (int)y, (int)ceilf(right - left), (int)ceilf(h));
                text_in(P, st, x - scroll, y, h, txt, RGB(255, 255, 255));
                c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
            }
            if (caret >= 0) caret_x = x - scroll + input_line_x(&L, st, (size_t)caret);
            input_line_end(&L);
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
        paint_drew(P, contentful_text(l, strlen(l)));
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
        paint_drew(P, contentful_text(txt, strlen(txt)));
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
            /* A canvas with a context is contentful (Paint Timing). */
            if (web_canvas_paint(n,st,c,X,Y,W,H)) paint_drew(P, true);
            break;
        }
        if (n->tag == T_audio || n->tag == T_video) {
            paint_debug_reached(n,c,X,Y,W,H);
            if(web_avmedia_paint(P->d,n,c,X,Y,W,H)){paint_drew(P,web_avmedia_shows_frame(P->d,n));break;}
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
        paint_drew(P, s != NULL);
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
        if (sc->img) { paint_drew(P, true); image_draw(c, sc->img, X, Y); }
        return;
    }
    paint_drew(P, false);
    paint_control(P, b, x, y);
}

/* ---------------------------------------------------------------- list markers */
/* An outside marker (CSS Lists 3 §3.1-3.2): its style, the list item's ::marker or the one the
   item gives it (inherited, which paints the same); and its marker image, a url() that loaded or a
   gradient, sized by the default sizing algorithm with a default object of 1em square, which it
   shows followed by a space in the place of its text (w, h: 0 when none) */
struct marker_look { const style_t *st; struct web_image *im; const struct gradient *gradient; float w, h; };
static void marker_look(const web_doc *d, const box_t *b, struct marker_look *m) {
    *m = (struct marker_look){.st = b->st->marker ? b->st->marker : b->st};
    const struct css_image *image = css_style_list(b->st)->image;
    float em = m->st->font_size;
    if (!image) return;
    if (image->kind == CSS_IMAGE_GRADIENT) {
        if (image->gradient) { m->gradient = image->gradient; m->w = m->h = em; }
        return;
    }
    int i = image->img - 1;
    struct web_image *im = d && i >= 0 && i < d->images.n ? d->images.v[i] : NULL;
    if (!im || !im->img || im->failed) return;
    m->im = im;
    m->w = im->img->w > 0 ? (float)im->img->w : em;
    m->h = im->img->h > 0 ? (float)im->img->h : em;
}
static void paint_marker(struct pctx *P, box_t *b) {
    struct marker_look look;
    marker_look(P->d, b, &look);
    const style_t *st = look.st;
    if (st->visibility || (!b->marker && !b->marker_shape)) return;
    float x = P->ox + box_visual_x(b), y = P->oy + cy(b);
    wfont f = style_font(st);
    float asc, desc;
    wf_metrics(&f, &asc, &desc);
    float bl = b->baseline >= 0 ? b->baseline : (style_line_height(b->st) - asc - desc) / 2 + asc;
    float base = y + bl;
    float fs = st->font_size;
    if (look.w > 0 && look.h > 0) { /* an inline replaced element on the baseline, then a space */
        float right = x - wf_width(&f, " ", 1), left = right - look.w, top = base - look.h;
        canvas_t *c = P->c;
        int X = (int)roundf(left), Y = (int)roundf(top), W = (int)roundf(look.w), H = (int)roundf(look.h);
        if (look.gradient) {
            paint_drew(P, true);
            fill_gradient(c, left, top, look.w, look.h, 0, look.gradient, st->color);
            return;
        }
        image_t *img = W > 0 && H > 0 ? scaled_image(look.im, W, H, c, X, Y, W, H) : NULL;
        paint_drew(P, img != NULL);
        if (img) image_draw(c, img, X, Y);
        return;
    }
    if (b->marker_shape) {
        paint_drew(P, false);
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
    paint_drew(P, contentful_text(m, strlen(m))); /* a ::marker's text */
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

/* A generated box carries its element as node and the pseudo-element's style. */
static uint8_t hit_pseudo(const node_t *target,const style_t *st){
    const style_t *own=target&&st?target->style:NULL;
    if(!own)return PE_NONE;
    return st==own->before?PE_BEFORE:st==own->after?PE_AFTER:PE_NONE;
}
static bool hit_stack_push(struct hit_stack *stack,node_t *target,uint8_t pseudo){
    if(!target)return true;
    if(stack->count&&stack->items[stack->count-1].node==target&&stack->items[stack->count-1].pseudo==pseudo)return true;
    if(stack->count==stack->capacity){
        size_t capacity=stack->capacity?stack->capacity*2:32;
        void *items=capacity>SIZE_MAX/sizeof *stack->items?NULL:realloc(stack->items,capacity*sizeof *stack->items);
        if(!items){stack->failed=true;return false;}
        stack->items=items;stack->capacity=capacity;
    }
    stack->items[stack->count++]=(struct hit_entry){target,pseudo};
    return true;
}
static bool hit_target(struct pctx *P,node_t *target,uint8_t pseudo) {
    box_t *box=target?target->box:NULL;
    const struct radius_cache *radius=P->hit_radius?P->hit_radius:box?box->radius_clip[0]:NULL;
    if(!P->index_build&&radius&&corners_coverage(P->hx,P->hy,P->hit_shape_x,P->hit_shape_y,radius->w,radius->h,&radius->radii)<.5f)return false;
    P->target=target;P->target_pseudo=pseudo;
    P->hit_local_x=P->hx;P->hit_local_y=P->hy;P->hit_run_block=NULL;
    if(P->stack&&!hit_stack_push(P->stack,target,pseudo)){P->failed=true;return false;}
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
        *item = (struct hit_primitive){.target=target,.order=index->count,.kind=WEB_HIT_NONE,.pseudo=pseudo};
        item->box=box;item->radius=radius;item->radius_x=P->hit_shape_x;item->radius_y=P->hit_shape_y;
        memcpy(item->rect,P->hit_rect,sizeof item->rect);
        index->count++;
        if (!(index->count & 63) && !web_native_checkpoint(P->d)) P->failed=true;
    }
    return !P->failed;
}
static void set_hit(struct pctx *P, int kind, node_t *n, const style_t *st, node_t *link) {
    if (n) {if(!hit_target(P,n,hit_pseudo(n,st)))return;}
    else if (link && P->index_build) hit_target(P,P->target,P->target_pseudo);
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
    uint8_t pseudo = P->target_pseudo;
    set_hit(P, WEB_HIT_DETAILS, details, NULL, NULL);
    P->target = target; P->target_pseudo = pseudo;
    if (P->stack && P->stack->count) P->stack->items[P->stack->count-1] = (struct hit_entry){target, pseudo};
    if (P->index_build && P->index_build->count) {
        P->index_build->items[P->index_build->count-1].target = target;
        P->index_build->items[P->index_build->count-1].pseudo = pseudo;
    }
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
        if(extent<=edge+LAYOUT_FIT_SLOP)continue;
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
        } else if(bg_content_clip(b->st)) { /* background-clip: content-box */
            if(!radius_cache_prepare(d,&b->radius_clip[2],x,y,b->w,b->h,content))return false;
        } else web_radius_cache_free(&b->radius_clip[2]);
        if(!clip_cache_prepare(d,b))return false;
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
       (d->paint_shadow_view_x==d->view_x&&d->paint_shadow_view_y==d->view_y&&
        d->paint_shadow_view_w==d->width&&d->paint_shadow_view_h==d->height)))return true;
    /* Viewport-cropped shadow masks are cut to the whole view rect: a
       viewport resize that leaves layout unchanged must still recut them. */
    d->paint_order_valid=false;
    d->paint_shadow_crop=false;
    if(d->profile_enabled)paint_profile.order_builds++;
    if(!prepare_paint_tree(d,d->root_box))return false;
    d->paint_order_layout=d->layout_revision;d->paint_order_visual=d->visual_revision;
    d->paint_shadow_view_x=d->view_x;d->paint_shadow_view_y=d->view_y;
    d->paint_shadow_view_w=d->width;d->paint_shadow_view_h=d->height;d->paint_order_valid=true;return true;
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
    const struct css_border_image *image=css_style_border_image(style);
    if(image->source.url||image->source.gradient) { /* css-backgrounds-3 §6.5 */
        float o[4];border_image_outsets(image,style->border_width,o);
        rect[0]=fminf(rect[0],x-o[3]-1);rect[1]=fminf(rect[1],y-o[0]-1);
        rect[2]=fmaxf(rect[2],x+w+o[1]+1);rect[3]=fmaxf(rect[3],y+h+o[2]+1);
    }
}
static void bounds_decoration(struct pctx *P,const style_t *style,float x,float y,float w,float h) {
    float rect[4]={x,y,x+w,y+h};shadow_ink_bounds(style,x,y,w,h,rect);
    bounds_rect(P,rect[0],rect[1],rect[2]-rect[0],rect[3]-rect[1]);
}
/* A text run's ink around its baseline, with bx/by its block's content origin: glyph
   overhang and decorations pass the advance by up to a font size. Up and down, its face's
   extents and a pixel; tall (a fallback face taller than the style's own) adds half a font
   size, for a test that costs nothing per pixel when it is loose. */
struct run_metrics { const style_t *st; float asc, desc; };
static void run_ink(const struct run *r,float bx,float by,struct run_metrics *m,bool tall,float rect[4]) {
    if(m->st!=r->st){wfont f=style_font(r->st);wf_metrics(&f,&m->asc,&m->desc);m->st=r->st;}
    float fs=r->st->font_size,slack=tall?fmaxf(1,fs*.5f):1;
    rect[0]=bx+r->x-fs;rect[1]=by+r->y-m->asc-slack;
    rect[2]=bx+r->x+r->w+fs;rect[3]=by+r->y+m->desc+slack;
}
/* An outside list marker's ink (paint_marker): left of the content box on its baseline. */
static void marker_ink(const web_doc *d,const box_t *b,float rect[4]) {
    struct marker_look look;marker_look(d,b,&look);
    const style_t *st=look.st;float fs=st->font_size,x=box_visual_x(b),y=cy(b);
    wfont f=style_font(st);float asc,desc;wf_metrics(&f,&asc,&desc);
    float base=y+(b->baseline>=0?b->baseline:(style_line_height(b->st)-asc-desc)/2+asc);
    float w=look.w>0?look.w+wf_width(&f," ",1):b->marker?wf_width(&f,b->marker,strlen(b->marker)):fs*.85f+fmaxf(2,fs*.18f);
    asc=fmaxf(asc,look.h);
    float slack=fmaxf(2,fs*.5f),bx=x-b->p[3]-b->b[3],by=y-b->content_dy-b->p[0]-b->b[0];
    rect[0]=fminf(x-w,bx-fs*2)-slack;rect[1]=fminf(base-asc,by)-slack;
    rect[2]=x+slack;rect[3]=fmaxf(base+desc,by+b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2])+slack;
}
/* b clips what its own paint recursion draws to its padding box: an overflow, marquee or
   line-clamp box. The viewport takes the root's and body's propagated overflow instead. */
static bool overflow_clips(const web_doc *d,const box_t *b) {
    return (web_marquee_box(b)||b->clamp_truncated||b->st->overflow!=OV_VISIBLE)&&
           b->kind!=B_INLINE&&b->parent&&!doc_viewport_overflow_box(d,b);
}

/* ---------------------------------------------------------------- normal-flow ink */
/* What paint_box draws for b and for the descendants its own recursion reaches (layers
   paint with their stacking context), relative to b's visual position. Nothing inside
   moves on its own: a translated, sticky or fixed box is a layer, and a scroll container
   or marquee holds what scrolls inside it to its padding box. So scrolling, sticky shifts
   and translations above b move it whole, and it stays true until the next layout, or a
   restyle on b's path (box.c restyle_boxes). Empty: no area; unbounded: infinite.
   flow_ink_layers: a layer below was left to the stacking contexts, or may be. */
struct ink_walk { web_doc *d; unsigned work; bool failed; struct run_metrics metrics; };
static const float *flow_ink(struct ink_walk *W,box_t *b);
static void ink_union(float r[4],const float a[4]) {
    if(!(a[2]>a[0]&&a[3]>a[1]))return;
    r[0]=fminf(r[0],a[0]);r[1]=fminf(r[1],a[1]);r[2]=fmaxf(r[2],a[2]);r[3]=fmaxf(r[3],a[3]);
}
static void ink_unbounded(float r[4]){r[0]=r[1]=-INFINITY;r[2]=r[3]=INFINITY;}
static void flow_ink_child(struct ink_walk *W,box_t *c,float r[4],bool *layers) {
    if(c->clamp_hidden){*layers|=c->first!=NULL;return;}
    if(paint_box_layer(c)){*layers=true;return;}
    if(c->kind==B_TEXT||c->kind==B_BR)return;
    if(c->kind==B_INLINE){ink_unbounded(r);*layers=true;return;} /* paints its scoped runs: never apart from its block */
    const float *k=flow_ink(W,c);
    if(!k)return;
    *layers|=c->flow_ink_layers;
    float x=box_visual_x(c),y=box_visual_y(c);
    if(!isfinite(x)||!isfinite(y)){ink_unbounded(r);return;}
    float a[4]={x+k[0],y+k[1],x+k[2],y+k[3]};ink_union(r,a);
}
/* inline_children: the atomic inlines, floats and blocks inside an inline formatting context */
static void flow_ink_inline(struct ink_walk *W,box_t *b,float r[4],bool *layers) {
    for(box_t *c=b->first;c&&!W->failed;c=c->next) {
        if(c->kind==B_INLINE&&!c->clamp_hidden&&!paint_box_layer(c))flow_ink_inline(W,c,r,layers);
        else flow_ink_child(W,c,r,layers);
    }
}
/* ---------------------------------------------------------------- gap decorations (css-gaps-1) */
/* Segment s of b's gap decorations (layout.c) as the rectangle it paints, relative to b's content
   box before scrolling, with the style and width §4.6 gives its gap; false when it paints nothing. */
static bool gap_segment_rect(const box_t *b, const struct gap_segment *s, float r[4], uint8_t *style) {
    const struct css_gap_axis *a = &css_style_gap_rules(b->st)->axis[s->axis];
    uint32_t count = b->gap_decorations->gaps[s->axis];
    *style = css_rule_value_at(&a->list[CSS_GAP_STYLE], s->gap, count).style;
    float w = css_rule_value_at(&a->list[CSS_GAP_WIDTH], s->gap, count).width;
    if (*style == BS_NONE || *style == BS_HIDDEN || !(w > 0) || !(s->e > s->s)) return false;
    /* a column gap's line runs down its centre, a row gap's across */
    if (s->axis == 0) { r[0] = s->centre - w / 2; r[1] = s->s; r[2] = s->centre + w / 2; r[3] = s->e; }
    else { r[0] = s->s; r[1] = s->centre - w / 2; r[2] = s->e; r[3] = s->centre + w / 2; }
    return true;
}
/* where b's content scrolls to: its decorations scroll with its items (§3) */
static void gap_scroll(const box_t *b, float *x, float *y) {
    if (b->node && box_element_scrollable(b)) { *x -= (float)b->node->scroll_x; *y -= (float)b->node->scroll_y; }
}
/* §3: painted just above the container's border, each gap's line in its gap's colour and style as
   a border's side; rule-overlap (§3.5) puts one axis' lines over the other's */
static void paint_gap_decorations(struct pctx *P, const box_t *b, float x, float y) {
    const struct gap_decorations *g = b->gap_decorations;
    if (!g || !g->n) return;
    const style_t *st = b->st;
    const struct css_gap_rules *rules = css_style_gap_rules(st);
    gap_scroll(b, &x, &y);
    int under = rules->overlap == CSS_RULE_ROW_OVER_COLUMN ? 0 : 1;
    for (int pass = 0; pass < 2; pass++) {
        int axis = pass ? !under : under;
        const struct css_rule_list *colors = &rules->axis[axis].list[CSS_GAP_COLOR];
        for (uint32_t i = 0; i < g->n; i++) {
            const struct gap_segment *s = &g->seg[i];
            float r[4];
            uint8_t style;
            if (s->axis != axis || !gap_segment_rect(b, s, r, &style)) continue;
            union css_rule_value c = css_rule_value_at(colors, s->gap, g->gaps[axis]);
            uint32_t col = c.color.exact ? css_color_value_argb(c.color.exact, st->color) :
                           c.color.argb == COLOR_CURRENT ? st->color : c.color.argb;
            if (!(col >> 24)) continue;
            edge(P->c, style, axis ? 0 : 3, x + r[0], y + r[1], r[2] - r[0], r[3] - r[1], col);
        }
    }
}
/* the rectangles they paint, relative to (x, y), b's content box */
static void gap_decorations_ink(const box_t *b, float x, float y, float rect[4]) {
    const struct gap_decorations *g = b->gap_decorations;
    if (!g) return;
    for (uint32_t i = 0; i < g->n; i++) {
        float r[4];
        uint8_t style;
        if (!gap_segment_rect(b, &g->seg[i], r, &style)) continue;
        float a[4] = {x + r[0], y + r[1], x + r[2], y + r[3]};
        ink_union(rect, a);
    }
}
static const float *flow_ink(struct ink_walk *W,box_t *b) {
    web_doc *d=W->d;
    if(b->flow_ink_valid&&b->flow_ink_layout==d->layout_revision)return b->flow_ink;
    if(!(++W->work&63)&&!web_native_checkpoint(d))W->failed=true;
    if(W->failed)return NULL;
    float r[4]={INFINITY,INFINITY,-INFINITY,-INFINITY};
    float x=box_visual_x(b),y=box_visual_y(b);
    const style_t *st=b->st;
    bool layers=b->first!=NULL; /* what it does not paint may hold any */
    if(!b->clamp_hidden&&st&&st->display!=D_NONE) {
        if(b->kind!=B_INLINE&&b->kind!=B_TEXT&&b->kind!=B_BR) {
            float bx=x-b->p[3]-b->b[3],by=y-b->p[0]-b->b[0];
            float bw=b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3],bh=b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];
            float a[4]={bx,by,bx+bw,by+bh};shadow_ink_bounds(st,bx,by,bw,bh,a);ink_union(r,a);
            if(b->marker||b->marker_shape){marker_ink(d,b,a);ink_union(r,a);}
        }
        if(!(b->kind==B_ATOMIC&&b->atomic!=AT_INLINE_BLOCK)&&st->content_visibility!=CV_HIDDEN&&!overflow_clips(d,b)) {
            layers=false;
            if(b->kind==B_INLINE){ink_unbounded(r);layers=true;}
            else if(b->inline_ctx) {
                /* paint_runs: a marquee or scroll container would move these, but it clips them */
                float rx=x,ry=cy(b),a[4];
                for(int i=0;i<b->ndecos;i++) {
                    const struct deco *e=&b->decos[i];
                    a[0]=rx+e->x;a[1]=ry+e->y;a[2]=a[0]+e->w;a[3]=a[1]+e->h;
                    shadow_ink_bounds(e->st,a[0],a[1],e->w,e->h,a);ink_union(r,a);
                }
                for(int i=0;i<b->nruns;i++)
                    if(!b->runs[i].atomic){run_ink(&b->runs[i],rx,ry,&W->metrics,true,a);ink_union(r,a);}
                flow_ink_inline(W,b,r,&layers);
            } else for(box_t *c=b->first;c&&!W->failed;c=c->next)flow_ink_child(W,c,r,&layers);
            gap_decorations_ink(b,x,y,r);
        }
    }
    if(W->failed)return NULL;
    if(!isfinite(x)||!isfinite(y))ink_unbounded(r);
    else if(r[2]>r[0]&&r[3]>r[1]){r[0]-=x;r[1]-=y;r[2]-=x;r[3]-=y;}
    memcpy(b->flow_ink,r,sizeof r);b->flow_ink_layers=layers;
    b->flow_ink_layout=d->layout_revision;b->flow_ink_valid=true;
    return b->flow_ink;
}
/* Painting: nothing b's own paint_box call draws reaches the canvas clip. A box that
   shows is painted without measuring; the rest measure once per layout, and a partial
   repaint, a video probe or a scrolled page then skips them whole. A stacking context
   (context) also paints its own layers, so only one without any is skipped. */
static bool ink_outside(struct pctx *P,box_t *b,bool context) {
    if(P->mode!=M_PAINT||!b->st||b->kind==B_INLINE||b->kind==B_TEXT||b->kind==B_BR||
       (b->node&&b->node==P->d->html))return false; /* the html box may paint the whole canvas */
    const canvas_t *c=P->c;
    float x=box_visual_x(b),y=box_visual_y(b);
    float left=P->ox+x-b->p[3]-b->b[3],top=P->oy+y-b->p[0]-b->b[0];
    float right=left+b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3],bottom=top+b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];
    if(!(right<=c->cx0||left>=c->cx1||bottom<=c->cy0||top>=c->cy1))return false;
    struct ink_walk W={.d=P->d};
    const float *k=flow_ink(&W,b);
    if(W.failed){P->failed=true;return true;}
    if(context&&b->flow_ink_layers)return false;
    if(!(k[2]>k[0]&&k[3]>k[1]))return true; /* paints nothing */
    const float slack=2; /* antialiased edges, rounded clips */
    return P->ox+x+k[2]+slack<=c->cx0||P->ox+x+k[0]-slack>=c->cx1||
           P->oy+y+k[3]+slack<=c->cy0||P->oy+y+k[1]-slack>=c->cy1;
}
/* A layer: a general transform or a filter moves or spreads its ink. */
static bool layer_outside(struct pctx *P,box_t *l) {
    return !box_transformed(l)&&!l->st->filter&&ink_outside(P,l,stacking_context(l));
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
        if (c->floated == floats && !ink_outside(P, c, false)) paint_box(P, c, false);
    }
}

static struct line_fragment *overflow_line(box_t *b,int run) {
    int low=0,high=b->nlines;
    while(low<high){int mid=low+(high-low)/2;if(b->lines[mid].end_run<=run)low=mid+1;else high=mid;}
    return low<b->nlines && b->lines[low].first_run<=run && b->lines[low].ellipsis_active?&b->lines[low]:NULL;
}
static void paint_runs(struct pctx *P, box_t *b, box_t *scope) {
    float bx, by;
    layout_runs_origin(b, true, &bx, &by); /* the origin every text geometry reader uses */
    struct run_metrics metrics = {0};
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
            if (hit_style(st) && d->node && inside(P, bx + d->x, by + d->y, d->w, d->h)) hit_target(P,d->node,hit_pseudo(d->node,st));
            P->hit_radius=NULL;
            continue;
        }
        float l = d->first ? 0 : 0;
        (void)l;
        float bw[4] = {st->border_width[0], st->border_width[1], st->border_width[2], st->border_width[3]};
        if (!d->first) bw[3] = 0;
        if (!d->last) bw[1] = 0;
        float x = P->ox + bx + d->x, y = P->oy + by + d->y;
        /* an inline box's padding percentages are of its block's width */
        struct bg_target T = {.block = b, .deco = d};
        for (int k = 0; k < 4; k++) T.pad[k] = fmaxf(0, len_resolve(&st->padding[k], b->w));
        if (!d->first) T.pad[3] = 0;
        if (!d->last) T.pad[1] = 0;
        paint_bg_border(P, st, x, y, d->w, d->h, bw, false, !d->first, !d->last,d->shadow_cache,d->radius_clip,&T);
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
                    set_hit(P, control_hit(a), a->node, a->st, r->link);
            }
            continue;
        }
        if (P->mode == M_HIT) {
            if (!hit_style(r->st)) continue;
            float top, bottom;
            layout_run_extent(r, &top, &bottom);
            if (inside(P, bx + r->x, by + top, r->w, bottom - top)) {
                bool targeted = false;
                if (r->node) {
                    if (r->node->type == N_ELEM) targeted = hit_target(P,r->node,hit_pseudo(r->node,r->st));
                    else targeted = hit_target(P,doc_flat_parent(r->node),PE_NONE);
                }
                if (r->link) set_hit(P, WEB_HIT_NONE, NULL, NULL, r->link);
                if (targeted && !P->failed) {
                    /* The element is the target; the run keeps the text position under it. */
                    P->hit_run_block = b; P->hit_run = i; P->hit_run_dx = P->hx - (bx + r->x);
                    if (P->index_build && P->index_build->count) {
                        struct hit_primitive *item = &P->index_build->items[P->index_build->count-1];
                        item->run_block = b; item->run = i; item->run_x = bx + r->x;
                    }
                }
            }
            continue;
        }
        if (P->mode == M_BOUNDS) {
            if (r->st->visibility) continue;
            float ink[4]; run_ink(r, bx, by, &metrics, false, ink);
            bounds_rect(P, ink[0], ink[1], ink[2] - ink[0], ink[3] - ink[1]);
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
    if (P->mode == M_PAINT && !P->text_mask)
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
struct transformed_job {
    uint32_t *black;
    const uint32_t *white;
    canvas_t *canvas;
    int sw, sh, dx, dy, dw, dh;
    float ox, oy;
    double sx0, sy0, inv[6];
};
/* Coverage is what the white pass shows through: the black result becomes
   premultiplied ARGB, its colour clamped to the recovered alpha. */
static void transformed_coverage(size_t tile, void *context) {
    const struct transformed_job *j = context;
    size_t first = tile * 32 * (size_t)j->sw, end = MIN((tile + 1) * 32, (size_t)j->sh) * (size_t)j->sw;
    for (size_t i = first; i < end; i++) {
        uint32_t k = j->black[i], w = j->white[i];
        int alpha = 0;
        for (int shift = 0; shift < 24; shift += 8) alpha += 255 - ((int)((w >> shift) & 255) - (int)((k >> shift) & 255));
        alpha = (alpha + 1) / 3; if (alpha < 0) alpha = 0; if (alpha > 255) alpha = 255;
        uint32_t r = (k >> 16) & 255, g = (k >> 8) & 255, bl = k & 255;
        if (r > (uint32_t)alpha) r = (uint32_t)alpha; if (g > (uint32_t)alpha) g = (uint32_t)alpha; if (bl > (uint32_t)alpha) bl = (uint32_t)alpha;
        j->black[i] = (uint32_t)alpha << 24 | r << 16 | g << 8 | bl;
    }
    parallel_record_work(PARALLEL_RASTER, end - first);
}
/* Bilinear samples of the premultiplied surface, composited over the opaque
   destination through the canvas's mask clip. */
static void transformed_resample(size_t tile, void *context) {
    const struct transformed_job *j = context;
    const canvas_t *c = j->canvas;
    const double *inv = j->inv;
    int sw = j->sw, sh = j->sh, last = MIN((int)(tile + 1) * 32, j->dh);
    for (int row_index = (int)tile * 32; row_index < last; row_index++) {
        int py = j->dy + row_index, first = 0, end = j->dw, opaque0 = j->dx, opaque1 = j->dx + j->dw;
        if (c->mask_clip) {
            /* Only the span's antialiased ends need the per-pixel mask. */
            struct gfx_clip_span span;
            if (!gfx_clip_row(c, py, j->dx, j->dx + j->dw, &span)) continue;
            first = span.x0 - j->dx; end = span.x1 - j->dx; opaque0 = span.opaque0; opaque1 = span.opaque1;
        }
        double X = j->dx + first + 0.5 - j->ox, Y = py + 0.5 - j->oy;
        double u = inv[0] * X + inv[2] * Y + inv[4] - j->sx0 - 0.5, v = inv[1] * X + inv[3] * Y + inv[5] - j->sy0 - 0.5;
        uint32_t *row = c->px + (size_t)py * c->pitch;
        for (int i = first; i < end; i++, u += inv[0], v += inv[1]) {
            if (!(u > -1 && v > -1 && u < sw && v < sh)) continue;
            int x0 = (int)(u + 1) - 1, y0 = (int)(v + 1) - 1; /* floor: both exceed -1 */
            uint32_t fx = (uint32_t)((u - x0) * 256), fy = (uint32_t)((v - y0) * 256);
            uint32_t lanes[4] = {0, 0, 0, 0};
            if (x0 >= 0 && y0 >= 0 && x0 + 1 < sw && y0 + 1 < sh) {
                const uint32_t *s = j->black + (size_t)y0 * sw + x0;
                uint32_t w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
                for (int lane = 0, shift = 0; lane < 4; lane++, shift += 8)
                    lanes[lane] = ((s[0] >> shift) & 255) * w00 + ((s[1] >> shift) & 255) * w10 +
                                  ((s[sw] >> shift) & 255) * w01 + ((s[sw + 1] >> shift) & 255) * w11;
            } else for (int corner = 0; corner < 4; corner++) {
                int sx = x0 + (corner & 1), sy = y0 + (corner >> 1);
                if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
                uint32_t weight = ((corner & 1) ? fx : 256 - fx) * ((corner >> 1) ? fy : 256 - fy);
                uint32_t s = j->black[(size_t)sy * sw + sx];
                for (int lane = 0; lane < 4; lane++) lanes[lane] += ((s >> (lane * 8)) & 255) * weight;
            }
            uint32_t a = (lanes[3] + 32768) >> 16;
            if (!a) continue;
            int px = j->dx + i;
            uint32_t mask = px >= opaque0 && px < opaque1 ? 255 : gfx_clip_alpha(c, px, py);
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
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->dw * (uint64_t)MAX(0, last - (int)tile * 32));
}
/* Small surfaces (icons, a rotated glyph) cost less than a dispatch. The
   checkpoint may draw browser chrome into the window canvas, so only a step
   writing private buffers is serviced; one writing the canvas joins as a
   raster primitive does. */
static void transformed_run(web_doc *d, parallel_fn fn, int rows, size_t pixels, struct transformed_job *j) {
    size_t tiles = ((size_t)rows + 31) / 32;
    if (rows < 64 || pixels < 65536) { for (size_t t = 0; t < tiles; t++) fn(t, j); return; }
    parallel_for_stage(PARALLEL_RASTER, tiles, fn, j, d ? shadow_service : NULL, d);
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
    /* Deterministic resampling: the video probes repaint it exactly, so it
       neither hides nor rejects a video layer it overlaps. A video inside it
       paints into the private surface, which the snapshot refuses. */
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
    /* Both steps write disjoint 32-row bands: coverage per surface row, then
       the destination rows the inverse matrix samples it into. Every band of
       a step completes, so a cancellation never leaves half a resampled box. */
    struct transformed_job j = {black, white, c, sw, sh, dx, dy, dw, dh, P->ox, P->oy, sx0, sy0, {0}};
    memcpy(j.inv, inv, sizeof j.inv);
    transformed_run(P->d, transformed_coverage, sh, count, &j);
    free(white); j.white = NULL;
    if (!web_native_checkpoint(P->d)) { P->failed = true; free(black); return; }
    transformed_run(NULL, transformed_resample, dh, (size_t)dw * (size_t)dh, &j);
    free(black);
}
static void paint_transformed(struct pctx *P, box_t *b, bool layer_root) {
    double m[6];
    if (!box_transform_matrix(b, m)) {
        struct pctx Q = *P; Q.untransformed = b; paint_box(&Q, b, layer_root);
        P->target = Q.target; P->target_pseudo = Q.target_pseudo; P->hit_any = Q.hit_any; P->bounds_valid = Q.bounds_valid;
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
    /* css-anchor-position-1 §6.6: position-visibility makes it and its descendants force-hidden */
    if (b->anchored && P->mode != M_BOUNDS && box_anchor_hidden(b)) return;
    if (web_dialog_layer_box(P->d,b) && b != P->top_root) return;
    if (P->mode == M_HIT && b->node && web_dialog_inert(P->d,b->node)) return;
    if (P->mode != M_BOUNDS && paint_box_layer(b) && !layer_root) return; /* painted with the layers */
    const style_t *st = b->st;
    float group_opacity = st->opacity * (st->filter ? st->filter->opacity : 1);
    /* Transparent auto boxes still participate in hit testing. */
    if (P->mode != M_HIT && group_opacity <= 0) return;
    /* a mask layer's image still loading: the box shows once it has come (or failed) */
    if (P->mode == M_PAINT && !P->text_mask && b->kind != B_TEXT && b->kind != B_INLINE && b->kind != B_BR &&
        mask_layers_state(P->d, b) == MASK_PENDING) return;
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
    /* clip-path and clip: b and everything it paints, and where they are hit */
    const int clip_saved[4] = {c->cx0, c->cy0, c->cx1, c->cy1};
    const struct gfx_mask_clip *clip_prior = c->mask_clip; struct gfx_mask_clip clip_cut;
    bool clipped = P->mode != M_BOUNDS && box_clips(b);
    if (clipped) {
        struct clip_used u; bool shown = true;
        if (clip_used(b, &u)) {
            shown = clip_push(P, b, &u, &clip_cut);
            if (shown && P->mode == M_HIT && !P->index_build) shown = clip_contains(b, &u, P->hx, P->hy);
        }
        if (!shown) { clip_pop(c, clip_saved, clip_prior); release_layers(&layers,layers_borrowed); return; }
    }
    if (P->mode == M_BOUNDS && canvas_group)
        bounds_rect(P, P->vx0 - P->ox, P->vy0 - P->oy, P->vx1 - P->vx0, P->vy1 - P->vy0);
    if (P->mode == M_BOUNDS && !st->visibility && b->kind != B_INLINE && b->kind != B_TEXT && b->kind != B_BR) {
        bounds_decoration(P,st,bx,by,bw,bh);
        if (b->gap_decorations) {
            float gx = x, gy = y, ink[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
            gap_scroll(b, &gx, &gy);
            gap_decorations_ink(b, gx, gy, ink);
            bounds_rect(P, ink[0], ink[1], ink[2] - ink[0], ink[3] - ink[1]);
        }
        if (b->marker || b->marker_shape) { float ink[4]; marker_ink(P->d, b, ink); bounds_rect(P, ink[0], ink[1], ink[2] - ink[0], ink[3] - ink[1]); }
    }
    /* A table row or row group is painted in its cells (CSS 2.1 17.5.1) and is not a
       target itself: a point between cells is the table's. */
    if (P->mode == M_HIT && hit_style(st) && b->node &&
        b->kind != B_TEXT && b->kind != B_ROW && b->kind != B_ROW_GROUP && inside(P, bx, by, bw, bh)) {
        if (b->node->type == N_ELEM) hit_target(P,b->node,hit_pseudo(b->node,st));
        else hit_target(P,doc_flat_parent(b->node),PE_NONE);
    }
    if (P->mode == M_HIT && hit_style(st) && b->node && !b->node->foreign &&
        b->kind != B_TEXT && (inside(P, bx, by, bw, bh) ||
        ((b->marker || b->marker_shape) && inside(P, bx - st->font_size * 2, by, st->font_size * 2, bh)))) {
        node_t *details = b->node->tag == T_summary ? doc_details_activation(b->node) :
                          b->anon && b->node->tag == T_details && !doc_details_summary(b->node) ? b->node : NULL;
        if (details) {
            hit_target(P,b->node,PE_NONE);
            set_disclosure_hit(P, details);
        }
    }
    if(P->index_build&&hit_style(st)&&b->node&&!b->node->foreign&&(b->marker||b->marker_shape)&&
       inside(P,bx-st->font_size*2,by,st->font_size*2,bh)){
        node_t *details=b->node->tag==T_summary?doc_details_activation(b->node):
            b->anon&&b->node->tag==T_details&&!doc_details_summary(b->node)?b->node:NULL;
        if(details){hit_target(P,b->node,PE_NONE);set_disclosure_hit(P,details);}
    }
    bool clip = overflow_clips(P->d, b);
    /* Off-screen contexts can still contain viewport-fixed descendants. */
    if (P->mode != M_BOUNDS && clip && !layers.n && (P->oy + by > c->cy1 || P->oy + by + bh < c->cy0)) {
        if (clipped) clip_pop(c, clip_saved, clip_prior);
        release_layers(&layers,layers_borrowed); return;
    }
    /* group opacity, the mask layers (css-masking-1 §7) and mask-border (§8): paint, then mix the
       result with what was there, by the opacity and the masks (both multiply). A mask-border whose
       image is not decoded masks nothing (§8.1). */
    uint32_t *saved = NULL;
    int gx = 0, gy = 0, gw = 0, gh = 0;
    struct nine_slice mask_slice;
    bool mask_box_kind = P->mode == M_PAINT && !P->text_mask && b->kind != B_TEXT && b->kind != B_INLINE && b->kind != B_BR;
    bool masked = mask_box_kind && mask_border_set(st) &&
                  nine_slice_prepare(P->d, css_style_mask_border(st), st->mask_border_img, st->color, P->ox + bx, P->oy + by,
                                     bw, bh, b->b, &mask_slice);
    bool mask_layers = mask_box_kind && mask_layers_state(P->d, b) == MASK_READY;
    if (P->mode == M_PAINT && (group_opacity < 1 || masked || mask_layers) && b->kind != B_TEXT) {
        struct pctx measure = *P; measure.mode = M_BOUNDS; measure.bounds_valid = false;
        paint_box(&measure, b, true);
        if (measure.failed) { P->failed = true; if (clipped) clip_pop(c, clip_saved, clip_prior); release_layers(&layers,layers_borrowed); return; }
        if (measure.bounds_valid) {
            double left = floor((double)P->ox + measure.bounds[0]), top = floor((double)P->oy + measure.bounds[1]);
            double right = ceil((double)P->ox + measure.bounds[2]), bottom = ceil((double)P->oy + measure.bounds[3]);
            if (left < P->vx0) left = P->vx0; if (top < P->vy0) top = P->vy0;
            if (right > P->vx1) right = P->vx1; if (bottom > P->vy1) bottom = P->vy1;
            if (right > left && bottom > top) {
                gx = (int)left; gy = (int)top; gw = (int)(right - left); gh = (int)(bottom - top);
                /* Group opacity mixes each pixel with what was under it, and
                   nothing else; other filter functions mix neighbours. */
                if(st->filter||masked||mask_layers)web_avmedia_snapshot_reject_region(c,gx,gy,gw,gh);
                else web_avmedia_snapshot_blend_region(c,gx,gy,gw,gh);
                if(P->d->profile_enabled){paint_profile.groups++;paint_profile.group_pixels+=(uint64_t)gw*(uint64_t)gh;}
                if ((size_t)gw > SIZE_MAX / sizeof *saved / (size_t)gh ||
                    !(saved = malloc(sizeof *saved * (size_t)gw * (size_t)gh))) {
                    P->failed = true; if (clipped) clip_pop(c, clip_saved, clip_prior);
                    release_layers(&layers,layers_borrowed); return; /* never paint an unfiltered substitute */
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
            paint_bg_border(P, st, P->ox + bx, P->oy + by, bw, bh, b->b, skip_bg, false, false,b->shadow_cache,b->radius_clip,
                            &(struct bg_target){.box = b, .pad = {b->p[0], b->p[1], b->p[2], b->p[3]}, .content_mask = true});
        } else if (P->mode == M_HIT && hit_style(st) && st->content_visibility!=CV_HIDDEN &&
                   (b->kind == B_ATOMIC || (b->node&&b->node->tag==T_button)) && inside(P, bx, by, bw, bh)) {
            int k = control_hit(b);
            if (k != WEB_HIT_NONE) set_hit(P, k, b->node, st, NULL);
        }
    } else if ((b->kind == B_ROW || b->kind == B_ROW_GROUP) && P->mode == M_PAINT && visible && (st->bg_color >> 24)) {
        float z[4] = {0, 0, 0, 0};
        paint_bg_border(P, st, P->ox + x, P->oy + y, b->w, b->h, z, false, false, false,b->shadow_cache,b->radius_clip,
                        &(struct bg_target){.box = b, .content_mask = true});
    }
    if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) {
        if (P->mode == M_PAINT && visible && !P->text_mask && st->content_visibility!=CV_HIDDEN) {
            const struct gfx_mask_clip *prior=c->mask_clip;struct gfx_mask_clip replacement;
            radius_clip_push(P,b->radius_clip[2],&replacement,P->ox+x,P->oy+cy(b));paint_replaced(P, b);c->mask_clip=prior;
        }
        goto done;
    }
    if(st->content_visibility==CV_HIDDEN)goto done;
    if (b->marker || b->marker_shape) {
        if (P->mode == M_PAINT && !P->text_mask) paint_marker(P, b);
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
    if (P->mode == M_PAINT && visible && !P->text_mask && b->gap_decorations) paint_gap_decorations(P, b, P->ox + x, P->oy + y);
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
        /* a layer child returns at once: it paints with the layers */
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated == false && (paint_box_layer(ch) || !ink_outside(P, ch, false))) paint_box(P, ch, false);
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated && (paint_box_layer(ch) || !ink_outside(P, ch, false))) paint_box(P, ch, false);
    }
    for (; layer < layers.n; layer++) paint_stacked_layer(P, layers.v[layer]);
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
    c->mask_clip=previous_mask;
done:
    release_layers(&layers,layers_borrowed);
    if (P->mode == M_PAINT && visible && !P->text_mask && st->outline_width > 0 && b->kind != B_INLINE && b->kind != B_TEXT &&
        b->kind != B_BR && b->kind != B_ROW && b->kind != B_ROW_GROUP)
        paint_outline(c, st, P->ox + bx, P->oy + by, bw, bh);
    if (clipped) clip_pop(c, clip_saved, clip_prior);
    if (saved) {
        int a = (int)(group_opacity * 255 + 0.5f);
        uint8_t *mask = masked && !P->failed ? mask_border_alpha(P, &mask_slice, gx, gy, gw, gh) : NULL;
        uint8_t *layers_mask = mask_layers && !P->failed ? mask_layers_alpha(P, b, P->ox + bx, P->oy + by, bw, bh, gx, gy, gw, gh) : NULL;
        if (layers_mask && mask) { /* both masks: their product */
            for (size_t k = 0; k < (size_t)gw * (size_t)gh; k++) mask[k] = (uint8_t)((mask[k] * layers_mask[k] + 127) / 255);
            free(layers_mask);
        } else if (layers_mask) mask = layers_mask;
        for (int j = 0; !P->failed && j < gh; j++) {
            if (!(j & 63) && !web_native_checkpoint(P->d)) { P->failed = true; break; }
            uint32_t *row = c->px + (size_t)(gy + j) * c->pitch + gx, *old = saved + (size_t)j * gw;
            if (mask) {
                const uint8_t *m = mask + (size_t)j * gw;
                for (int i = 0; i < gw; i++) row[i] = gfx_mix(old[i], row[i], (a * m[i] + 127) / 255);
            } else for (int i = 0; i < gw; i++) row[i] = gfx_mix(old[i], row[i], a);
        }
        free(mask);
        if (P->failed)
            for (int j = 0; j < gh; j++) memcpy(c->px + (size_t)(gy + j) * c->pitch + gx,
                saved + (size_t)j * gw, sizeof *saved * (size_t)gw);
        free(saved);
    }
    if (P->mode == M_BOUNDS) {
        if (P->bounds_valid && box_clips(b)) clip_bounds(b, P->bounds, &P->bounds_valid);
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
    canvas_t *canvas=P->c;const struct gfx_mask_clip *previous=canvas->mask_clip;struct gfx_mask_clip boundary,cut;
    if(ancestor->clamp_hidden||(ancestor->st&&ancestor->st->display==D_NONE)||
       (ancestor->anchored&&P->mode!=M_BOUNDS&&box_anchor_hidden(ancestor)))skip=true;
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
    struct clip_used u; /* clip-path and clip reach every descendant, fixed ones too */
    if(!skip&&clip_used(ancestor,&u)) {
        if(!clip_push(P,ancestor,&u,&cut))skip=true;
        else if(P->mode==M_HIT&&!P->index_build&&!clip_contains(ancestor,&u,P->hx,P->hy))skip=true;
    }
    if(box_viewport_fixed(ancestor)) {
        if(!P->d->marquees.n&&!clips_above(ancestor)){if(!skip)paint_box(P,layer,true);canvas->mask_clip=previous;return;}
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
    if (!layer_outside(P, l))
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
        /* The top layer covers the viewport: nothing under it stays a target. */
        if(P->mode==M_HIT){memset(P->hit,0,sizeof *P->hit);P->hit_any=false;if(P->stack)P->stack->count=0;}
        else if(P->mode==M_PAINT)gfx_fill(P->c,P->vx0,P->vy0,P->vx1-P->vx0,P->vy1-P->vy0,RGB(0,0,0));
        P->top_root=fullscreen->box;paint_box(P,fullscreen->box,true);
    }
    for(int i=0;i<web_dialog_count(P->d);i++) {
        node_t *n=web_dialog_at(P->d,i); if(!n || !n->box)continue;
        if(P->mode==M_HIT) {
            if(n!=web_dialog_top(P->d))continue;
            memset(P->hit,0,sizeof *P->hit); P->hit_any=false;
            if(P->stack)P->stack->count=0;
            hit_target(P,hit_style(n->style)?n:NULL,PE_NONE);
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
    if(n>=0&&(size_t)n<size)n+=snprintf(out+n,size-(size_t)n,
        "; restyle damage: %lu commits of %lu boxes, whole after layout %lu, unbounded %lu, too many %lu, ink %lu; taken partial %lu, whole %lu, none recorded %lu, stale %lu",
        (unsigned long)damage_profile.commits,(unsigned long)damage_profile.boxes,(unsigned long)damage_profile.lost_layout,
        (unsigned long)damage_profile.lost_unbounded,(unsigned long)damage_profile.lost_room,(unsigned long)damage_profile.lost_ink,
        (unsigned long)damage_profile.taken_partial,(unsigned long)damage_profile.taken_full,
        (unsigned long)damage_profile.taken_none,(unsigned long)damage_profile.taken_stale);
    memset(&paint_profile,0,sizeof paint_profile);
    memset(&damage_profile,0,sizeof damage_profile);
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
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    gfx_clip(c, x, y, w, h);
    bool snapshot_root=!d->frame_parent;
    if(snapshot_root){
        /* A damage repaint redraws only the frames' pixels inside its clip. */
        double clip[4]={(double)doc_x+c->cx0-x,(double)doc_y+c->cy0-y,(double)doc_x+c->cx1-x,(double)doc_y+c->cy1-y};
        web_frames_prepare_paint(d,clip);web_avmedia_snapshot_begin(d,c,x,y,w,h);
    }
    if(profile){uint64_t now=uptime_ms();paint_profile.frames_ms+=now-mark;mark=now;}
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
    /* The copy lives on the document between paints: a viewport-sized buffer
       is a mapping of its own, and taking and returning one every frame cost
       a reservation, cold pages and a TLB shootdown each time. */
    uint32_t *frame_saved = NULL;
    int fx = c->cx0, fy = c->cy0, fw = c->cx1 - fx, fh = c->cy1 - fy;
    if (d->root_box && d->root_box->paint_alpha && fw > 0 && fh > 0) {
        size_t pixels = (size_t)fw * (size_t)fh;
        if (!c->px || fx < 0 || fy < 0 || c->cx1 > c->w || c->cy1 > c->h || c->pitch < c->w ||
            (size_t)fw > SIZE_MAX / sizeof *frame_saved / (size_t)fh ||
            (size_t)c->pitch > SIZE_MAX / sizeof *frame_saved / (size_t)c->h) P.failed = true;
        else if (d->paint_rollback_pixels < pixels) {
            free(d->paint_rollback);
            d->paint_rollback = malloc(sizeof *frame_saved * pixels);
            d->paint_rollback_pixels = d->paint_rollback ? pixels : 0;
        }
        if (!P.failed && !(frame_saved = d->paint_rollback)) P.failed = true;
        else if (!P.failed) for (int row = 0; row < fh; row++)
            memcpy(frame_saved + (size_t)row * fw, c->px + (size_t)(fy + row) * c->pitch + fx, sizeof *frame_saved * (size_t)fw);
    }
    int from;
    uint32_t bg = doc_canvas_bg(d, &from);
    P.canvas_bg = bg;
    P.canvas_group = d->html && d->html->style && (d->html->style->filter || d->html->style->opacity < 1);
    /* a translucent canvas background is drawn over white, an opaque one alone */
    if (!P.failed) {
        if (P.canvas_group || bg >> 24 != 255) gfx_fill(c, x, y, w, h, RGB(255, 255, 255));
        if (!P.canvas_group) gfx_fill_blend(c, x, y, w, h, bg);
    }
    P.canvas_bg_from = (uint32_t)from;
    if (from) paint_drew(&P, false); /* a non-default canvas background */
    walk(&P);
    /* Paint Timing "mark paint timing": this rendering update's end, once each. */
    if (!P.failed && (P.painted || P.contentful)) {
        uint64_t painted_at = uptime_ms();
        if (!d->first_paint_ms) d->first_paint_ms = painted_at;
        if (P.contentful && !d->first_contentful_paint_ms) d->first_contentful_paint_ms = painted_at;
    }
    if (P.failed && frame_saved)
        for (int row = 0; row < fh; row++) memcpy(c->px + (size_t)(fy + row) * c->pitch + fx,
            frame_saved + (size_t)row * fw, sizeof *frame_saved * (size_t)fw);
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

void web_paint_free(web_doc *d) {
    if(!d)return;
    free(d->paint_rollback);d->paint_rollback=NULL;d->paint_rollback_pixels=0;
}
void web_hit_index_free(web_doc *d) {
    if(!d||!d->hit_index)return;
    free(d->hit_index->items);free(d->hit_index->branches);free(d->hit_index->regions);free(d->hit_index);d->hit_index=NULL;
}
/* Unites a document rectangle (left, top, right, bottom) with the next paint's damage. */
static void damage_add(web_doc *d,const float rect[4]){
    if(!d->paint_damage_valid){memcpy(d->paint_damage,rect,sizeof d->paint_damage);d->paint_damage_valid=true;return;}
    d->paint_damage[0]=fminf(d->paint_damage[0],rect[0]);d->paint_damage[1]=fminf(d->paint_damage[1],rect[1]);
    d->paint_damage[2]=fmaxf(d->paint_damage[2],rect[2]);d->paint_damage[3]=fmaxf(d->paint_damage[3],rect[3]);
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
    /* The box's own transform and those above it move what it paints. */
    double r[4]={ink[0],ink[1],(double)ink[2]-ink[0],(double)ink[3]-ink[1]};
    box_transform_rect(box,r);
    for(int i=0;i<4;i++)if(!isfinite(r[i])){d->paint_damage_full=true;return;}
    ink[0]=(float)r[0];ink[1]=(float)r[1];ink[2]=(float)(r[0]+r[2]);ink[3]=(float)(r[1]+r[3]);
    damage_add(d,ink);
}

/* The box whose subtree paints what a restyle of n changes, NULL when nothing of n is painted;
   *known is false when no one box bounds it: the canvas takes html's and body's backgrounds,
   the top layer paints apart from the tree, and an SVG element outside a boxed outermost <svg>
   may be drawn by any <use>. Inside one, the outermost <svg>'s snapshot paints it, as any
   element without boxes of its own (display: contents, an <option>, a <col>) is painted by its
   nearest boxed ancestor, an inline by its block's runs, and whatever a filter above spreads
   or remaps, by the filtered ancestor. */
static box_t *restyle_box(web_doc *d,node_t *n,bool *known){
    *known=true;
    if(n==d->html||n==d->body){*known=false;return NULL;}
    if(!n->box&&(!n->style||n->style->display==D_NONE))return NULL;
    node_t *e=n;
    if(n->foreign&&!n->box){
        while(e->parent&&e->parent->type==N_ELEM&&e->parent->foreign)e=e->parent;
        if(e->tag!=T_svg||!e->box||e->box->atomic!=AT_SVG){*known=false;return NULL;}
    }
    while(e&&!e->box)e=doc_flat_parent(e);
    box_t *b=e&&e->owner==d?e->box:NULL;
    while(b&&(b->kind==B_INLINE||b->kind==B_TEXT||b->kind==B_BR))b=b->parent;
    for(box_t *a=b;a;a=a->parent){
        if(web_dialog_layer_box(d,a)){*known=false;return NULL;}
        if(a!=b&&a->st&&a->st->filter)b=a;
    }
    return b;
}
/* What b's subtree paints, in document space through the transforms above it; an empty
   rectangle when it paints nothing. False when that cannot be bounded. */
static bool restyle_box_ink(web_doc *d,box_t *b,float rect[4]){
    canvas_t scratch={0};
    struct pctx P={0};
    P.d=d;P.c=&scratch;P.mode=M_BOUNDS;P.top_root=b;
    P.bounds_generation=__atomic_add_fetch(&d->paint_bounds_generation,1,__ATOMIC_RELAXED);
    uint32_t memo=box_visual_memo_begin();
    paint_box(&P,b,true);
    box_visual_memo_end(memo);
    if(P.failed)return false;
    if(!P.bounds_valid){rect[0]=rect[1]=INFINITY;rect[2]=rect[3]=-INFINITY;return true;}
    /* the subtree's own transform is in its bounds; its antialiased edges reach a pixel out */
    double r[4]={P.bounds[0]-2,P.bounds[1]-2,P.bounds[2]-P.bounds[0]+4,P.bounds[3]-P.bounds[1]+4};
    box_transform_rect(b->parent,r);
    for(int i=0;i<4;i++)if(!isfinite(r[i]))return false;
    rect[0]=(float)r[0];rect[1]=(float)r[1];rect[2]=(float)(r[0]+r[2]);rect[3]=(float)(r[1]+r[3]);
    return true;
}
void doc_restyle_damage_lost(web_doc *d){
    if(d->profile_enabled&&!d->restyle_damage_lost)damage_profile.lost_layout++;
    d->restyle_damage_n=0;d->restyle_damage_lost=false;
    d->paint_damage_full=true;
}
void doc_restyle_damage_begin(web_doc *d){
    if(d->restyle_damage_lost)return;
    /* A sprite sheet's element reached by <use> re-serializes every snapshot. */
    if(d->svg_snapshots_dirty){d->restyle_damage_lost=true;if(d->profile_enabled)damage_profile.lost_unbounded++;return;}
    const size_t room=sizeof d->restyle_damage/sizeof *d->restyle_damage;
    for(node_t *n=d->style_retired_first;n;n=n->style_retired_next){
        if(!css_restyled(n))continue;
        bool known;box_t *b=restyle_box(d,n,&known);
        if(!known){d->restyle_damage_lost=true;if(d->profile_enabled)damage_profile.lost_unbounded++;return;}
        if(!b)continue;
        /* A box met again keeps what it painted before its first restyle. */
        bool seen=false;
        for(size_t i=0;i<d->restyle_damage_n&&!seen;i++)seen=d->restyle_damage[i].box==b;
        if(seen)continue;
        if(d->restyle_damage_n==room){d->restyle_damage_lost=true;if(d->profile_enabled)damage_profile.lost_room++;return;}
        struct restyle_damage *r=&d->restyle_damage[d->restyle_damage_n];
        if(!restyle_box_ink(d,b,r->rect)){d->restyle_damage_lost=true;if(d->profile_enabled)damage_profile.lost_ink++;return;}
        r->box=b;d->restyle_damage_n++;
    }
}
void doc_restyle_damage_commit(web_doc *d){
    bool lost=d->restyle_damage_lost;
    float now[sizeof d->restyle_damage/sizeof *d->restyle_damage][4];
    for(size_t i=0;i<d->restyle_damage_n&&!lost;i++)lost=!restyle_box_ink(d,d->restyle_damage[i].box,now[i]);
    if(lost){
        if(d->profile_enabled&&!d->restyle_damage_lost)damage_profile.lost_ink++;
        d->restyle_damage_lost=true;doc_restyle_damage_lost(d);return;
    }
    if(d->profile_enabled){damage_profile.commits++;damage_profile.boxes+=d->restyle_damage_n;}
    /* An unclassified notification already pending stays whole. */
    if(d->paint_dirty&&!d->paint_damage_valid)d->paint_damage_full=true;
    if(!d->paint_damage_valid){
        d->paint_damage[0]=d->paint_damage[1]=INFINITY;d->paint_damage[2]=d->paint_damage[3]=-INFINITY;
        d->paint_damage_valid=true;
    }
    for(size_t i=0;i<d->restyle_damage_n;i++){damage_add(d,d->restyle_damage[i].rect);damage_add(d,now[i]);}
    d->restyle_damage_n=0;
}
bool web_take_paint_damage(web_doc *d,int *x,int *y,int *w,int *h){
    if(!d)return false;
    bool valid=d->paint_damage_valid&&!d->paint_damage_full&&d->layout_valid&&!d->need_style&&!d->need_boxes;
    if(d->profile_enabled){
        if(valid)damage_profile.taken_partial++;
        else if(d->paint_damage_full)damage_profile.taken_full++;
        else if(!d->paint_damage_valid)damage_profile.taken_none++;
        else damage_profile.taken_stale++;
    }
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
    for(const box_t *a=box;a;a=a->parent){struct clip_used u;if(clip_used(a,&u)&&!clip_contains(a,&u,x,y))return false;}
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
       index->view_x!=d->view_x||index->view_y!=d->view_y||index->view_w!=d->width||index->view_h!=d->height||
       index->fonts!=fonts||!fonts){
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
        index->view_x=d->view_x;index->view_y=d->view_y;index->view_w=d->width;index->view_h=d->height;
        index->fonts=fonts;index->valid=true;
    }
    for(size_t i=0;i<index->region_count;i++)if(hit_rect_contains(index->regions[i],P->hx,P->hy))return false;
    const struct hit_primitive *item=index->count?hit_query(index,0,P->hx,P->hy,NULL):NULL;
    if(item){
        P->target=item->target;P->hit->kind=item->kind;P->hit->node=item->node;
        /* Indexed primitives are untransformed: the point is already in their space. */
        P->hit_local_x=P->hx;P->hit_local_y=P->hy;
        P->hit_run_block=item->run_block;P->hit_run=item->run;P->hit_run_dx=P->hx-item->run_x;
    }
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

bool doc_caret_at(web_doc *d, double x, double y, node_t **node, uint32_t *offset) {
    *node = NULL; *offset = 0;
    if (!d || d->native_cancelled || d->native_recovery_pending || d->native_recovery_active || !d->root_box ||
        !isfinite(x) || !isfinite(y)) return false;
    /* The same hit test as elementFromPoint: the topmost target, and the run under the point
       when it is text. Children of other documents' viewports stay their frame element's. */
    struct pctx P = {0};
    struct web_hit hit = {0};
    canvas_t dummy = {0};
    dummy.cx0 = dummy.cy0 = -1000000000;
    dummy.cx1 = dummy.cy1 = 1000000000;
    P.d = d; P.c = &dummy; P.mode = M_HIT; P.hx = (float)x; P.hy = (float)y; P.hit = &hit;
    if (!hit_index_lookup(&P)) walk(&P);
    if (P.failed) return false;
    node_t *target = P.target && P.target->owner == d && !web_dialog_inert(d, P.target) ? P.target : NULL;
    float local_x = target ? P.hit_local_x : (float)x, local_y = target ? P.hit_local_y : (float)y;
    if (target && P.hit_run_block && layout_run_position(P.hit_run_block, P.hit_run, P.hit_run_dx, node, offset))
        return true;
    box_t *box = target ? target->box : NULL;
    /* A target painted inside its ancestor's box (an SVG shape) has no box of its own. */
    if (target && !box) { *node = target; return true; }
    if (text_control_box(box) && box->node == target) {
        if (!control_offset_at(d, box, local_x, local_y, offset)) return false;
        *node = target;
        return true;
    }
    return layout_position_at(box ? box : d->root_box, local_x, local_y, node, offset);
}

struct hit_found { const struct hit_primitive **items; size_t count, capacity; bool failed; };
static void hit_collect(const struct web_hit_index *index,size_t id,float x,float y,struct hit_found *found){
    const struct hit_branch *branch=&index->branches[id];
    if(found->failed||!hit_rect_contains(branch->rect,x,y))return;
    if(branch->left==SIZE_MAX){
        for(size_t i=branch->first;i<branch->first+branch->count;i++){
            const struct hit_primitive *item=&index->items[i];
            if(!hit_rect_contains(item->rect,x,y)||!hit_corner_contains(item,x,y))continue;
            if(found->count==found->capacity){
                size_t capacity=found->capacity?found->capacity*2:32;
                void *items=realloc(found->items,capacity*sizeof *found->items);
                if(!items){found->failed=true;return;}
                found->items=items;found->capacity=capacity;
            }
            found->items[found->count++]=item;
        }
        return;
    }
    hit_collect(index,branch->left,x,y,found);hit_collect(index,branch->right,x,y,found);
}
static int hit_order_compare(const void *a,const void *b){
    size_t x=(*(const struct hit_primitive *const *)a)->order,y=(*(const struct hit_primitive *const *)b)->order;
    return x<y?-1:x>y;
}
static int hit_entry_compare(const void *a,const void *b){
    const struct hit_entry *x=a,*y=b;
    if(x->node!=y->node)return (uintptr_t)x->node<(uintptr_t)y->node?-1:1;
    return (x->pseudo>y->pseudo)-(x->pseudo<y->pseudo);
}
bool doc_elements_at(web_doc *d,int x,int y,node_t ***elements,size_t *count){
    *elements=NULL;*count=0;
    if(!d||d->native_cancelled||d->native_recovery_pending||d->native_recovery_active||!d->root_box)return true;
    struct hit_stack stack={0};
    struct pctx P={0};struct web_hit hit={0};canvas_t dummy={0};
    dummy.cx0=dummy.cy0=-1000000000;dummy.cx1=dummy.cy1=1000000000;
    P.d=d;P.c=&dummy;P.mode=M_HIT;P.hx=(float)x;P.hy=(float)y;P.hit=&hit;
    /* The index holds every primitive the walk would reach, in the walk's order. */
    bool indexed=hit_index_lookup(&P);
    if(indexed&&!P.failed&&d->hit_index&&d->hit_index->count){
        struct hit_found found={0};
        hit_collect(d->hit_index,0,P.hx,P.hy,&found);
        if(found.failed){free(found.items);return false;}
        if(found.count)qsort(found.items,found.count,sizeof *found.items,hit_order_compare);
        for(size_t i=0;i<found.count&&!stack.failed;i++)hit_stack_push(&stack,found.items[i]->target,found.items[i]->pseudo);
        free(found.items);
    } else if(!indexed){P.stack=&stack;walk(&P);}
    if(stack.failed){free(stack.items);return false;}
    if(P.failed){free(stack.items);return true;}
    /* Topmost first. Each element once, and each of its generated boxes once listed
       as the element: a positioned ::before can put its element both above and
       below another. Text runs count as their element; inert nodes are left out. */
    for(size_t i=0,j=stack.count;i+1<j;i++,j--){struct hit_entry swap=stack.items[i];stack.items[i]=stack.items[j-1];stack.items[j-1]=swap;}
    size_t kept=0;
    for(size_t i=0;i<stack.count;i++){
        struct hit_entry e=stack.items[i];
        if(e.node&&e.node->type!=N_ELEM){e.node=doc_flat_parent(e.node);e.pseudo=PE_NONE;}
        if(!e.node||e.node->owner!=d||web_dialog_inert(d,e.node))continue;
        stack.items[kept++]=e;
    }
    if(!kept){free(stack.items);return true;}
    struct hit_entry *sorted=malloc(kept*sizeof *sorted);
    bool *taken=calloc(kept,sizeof *taken);
    node_t **out=malloc(kept*sizeof *out);
    if(!sorted||!taken||!out){free(sorted);free(taken);free(out);free(stack.items);return false;}
    memcpy(sorted,stack.items,kept*sizeof *sorted);
    qsort(sorted,kept,sizeof *sorted,hit_entry_compare);
    size_t unique=0;
    for(size_t i=0;i<kept;i++){
        struct hit_entry *at=bsearch(&stack.items[i],sorted,kept,sizeof *sorted,hit_entry_compare);
        while(at>sorted&&!hit_entry_compare(at-1,&stack.items[i]))at--;
        if(taken[at-sorted])continue;
        taken[at-sorted]=true;out[unique++]=stack.items[i].node;
    }
    free(sorted);free(taken);free(stack.items);
    *elements=out;*count=unique;
    return true;
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

/* The searchable text of inline formatting root b: its rendered runs' bytes in line order,
   with a space between lines that do not already end in one; owner[k] is the run byte k came
   from, -1 for a joining space. Matching and highlighting both read this one text. */
static bool find_block_text(const box_t *b, sbuf *t, int **owner_out) {
    int *owner = NULL;
    size_t cap = 0;
    float prev_y = -1e30f;
    *t = (sbuf){0}; *owner_out = NULL;
    for (int i = 0; i < b->nruns; i++) {
        const struct run *r = &b->runs[i];
        if (r->atomic || !r->n || r->st->visibility) continue;
        if (t->n && r->y != prev_y && t->p[t->n - 1] != ' ') sb_putc(t, ' ');
        sb_put(t, r->s, (size_t)r->n);
        prev_y = r->y;
        if (t->n > cap) {
            size_t old = cap;
            size_t max=SIZE_MAX/sizeof *owner;
            if(t->n>max){free(owner);sb_free(t);return false;}
            cap = t->n>max/2?t->n:t->n*2;
            int *grown = realloc(owner, sizeof *owner * cap);
            if(!grown){free(owner);sb_free(t);return false;}
            owner=grown;
            for (size_t k = old; k < cap; k++) owner[k] = -1;
        }
        for (size_t k = t->n - (size_t)r->n; k < t->n; k++) owner[k] = i;
    }
    *owner_out = owner;
    return true;
}

static void find_in(struct fmatch *F, box_t *b) {
    if (F->failed || b->clamp_hidden || !b->st || b->st->display == D_NONE || b->st->content_visibility==CV_HIDDEN || (b->node && web_dialog_inert(F->d,b->node))) return;
    if (b->nruns) {
        sbuf t;
        int *owner;
        if (!find_block_text(b, &t, &owner)) { F->failed = true; return; }
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

/* Highlight the bytes of the match at byte off of b's searchable text, run by run. */
static bool highlight(box_t *b, size_t off, size_t qn) {
    sbuf t;
    int *owner;
    if (!find_block_text(b, &t, &owner)) return false;
    size_t end = off + qn < t.n ? off + qn : t.n;
    for (size_t k = off; k < end; k++) {
        int i = owner[k];
        if (i < 0 || (k > off && owner[k - 1] == i)) continue;
        /* A run's bytes are contiguous: find where they start once, at its first matched byte. */
        size_t start = k;
        while (start > 0 && owner[start - 1] == i) start--;
        struct run *r = &b->runs[i];
        size_t last = start + (size_t)r->n < end ? start + (size_t)r->n : end;
        r->highlight = true;
        r->highlight_first = (int)(k - start);
        r->highlight_end = (int)(last - start);
    }
    free(owner);
    sb_free(&t);
    return true;
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
    if (!highlight(b, off, F.qn)) return -2;
    const struct run *r = &b->runs[F.best_b ? F.best_r : F.first_r];
    float top, bottom;
    layout_run_extent(r, &top, &bottom);
    return (int)(y - r->y + top);
}
