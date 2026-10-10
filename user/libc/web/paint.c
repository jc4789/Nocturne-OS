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
#include "frame.h"
#include "web_dialog.h"
#include "js_canvas.h"
#include "avmedia.h"
#include "elements.h"
#include "form_value.h"
#include "form_validation.h"

enum { M_PAINT, M_HIT };

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
};
#include "paint_media_debug.h"

static bool positioned(const box_t *b) {
    if (!b->st || b->kind == B_INLINE || b->kind == B_TEXT) return false;
    if (b->st->position != POS_STATIC) return b->st->position != POS_STICKY;
    /* z-index also applies to in-flow flex/grid items. Their layout remains
       static, but painting and hit testing must place them in the z order. */
    return !b->st->z_auto && b->parent && !b->abspos &&
           (b->parent->kind == B_FLEX || b->parent->kind == B_GRID);
}

static bool stacking_context(const box_t *b) {
    if (!b->st || b->kind == B_INLINE || b->kind == B_TEXT) return false;
    /* Opacity groups are atomic even in normal flow. Fixed/sticky boxes also
       establish a context with auto z-index; their descendants cannot escape
       into the outer context's global sort after author layout hydration. */
    return b->st->opacity < 1.0f || b->st->position == POS_FIXED || b->st->position == POS_STICKY ||
           (positioned(b) && !b->st->z_auto);
}
static bool paint_box_layer(const box_t *b) { return positioned(b) || stacking_context(b); }

static float cy(const box_t *b) { return box_visual_y(b) + b->content_dy; }

static image_t *scaled_image(struct web_image *im, int w, int h);

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
    image_t *img = scaled_image(im, TW, TH);
    if (!img) return;
    canvas_t *c = P->c;
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    /* rounded edges, so a box at a fractional position shows no sliver of a neighbouring tile */
    int X0 = (int)roundf(x), Y0 = (int)roundf(y);
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

static void paint_bg_border(struct pctx *P, const style_t *st, float x, float y, float w, float h, const float *bw,
                            bool skip_bg, bool open_l, bool open_r) {
    canvas_t *c = P->c;
    float r = used_radius(st, w, h);
    if (st->mask_img) { /* an icon: the background colour through the mask, nothing else */
        uint32_t col = st->has_grad ? st->grad[0] : st->bg_color;
        if (!skip_bg && (col >> 24))
            paint_layer(P, st->mask_img, st->mask_size_kind, st->mask_size, st->mask_pos, st->mask_repeat, col, x, y, w,
                        h, bw);
        return;
    }
    if (!skip_bg) {
        if (st->bg_color >> 24) fill_rrect(c, x, y, w, h, r, st->bg_color, st->bg_color);
        if (st->has_grad && st->gradient) fill_gradient(c, x, y, w, h, r, st->gradient, st->color);
    }
    if (st->bg_img)
        paint_layer(P, st->bg_img, st->bg_size_kind, st->bg_size, st->bg_pos, st->bg_repeat, 0, x, y, w, h, bw);
    const uint32_t *bc = st->border_color;
    bool any = false;
    for (int i = 0; i < 4; i++)
        if (bw[i] > 0 && (bc[i] >> 24) && st->border_style[i] > BS_HIDDEN) any = true;
    if (!any) return;
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
static image_t *scaled_image(struct web_image *im, int w, int h) {
    if (w <= 0 || h <= 0 || !im->img) return NULL;
    if (im->img->w == w && im->img->h == h) return im->img;
    if (im->scaled && im->scaled->w == w && im->scaled->h == h) return im->scaled;
    if(web_avmedia_snapshot_is_probe()){web_avmedia_snapshot_reject();return NULL;}
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
        int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
        /* like browsers, clip to the padding box: descenders may hang below a short content box */
        gfx_clip(c, (int)(x - b->p[3]), (int)(y - b->p[0]), (int)ceilf(w + b->p[1] + b->p[3]) + 1, (int)ceilf(h + b->p[0] + b->p[2]) + 1);
        const char *t = node_attr(n, "type");
        bool pw = t && str_ieq(t, "password") && !placeholder;
        uint32_t col = placeholder ? RGB(117, 117, 117) : st->color;
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
            text_in(P, st, x - scroll, y, h, txt, col);
            if (selected) {
                int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
                gfx_clip(c, (int)floorf(left), (int)y, (int)ceilf(right - left), (int)ceilf(h));
                text_in(P, st, x - scroll, y, h, txt, RGB(255, 255, 255));
                c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
            }
            caret_x = x - scroll + caret_w;
            sb_free(&s);
        } else {
            wfont ff = f;
            float asc, desc;
            wf_metrics(&ff, &asc, &desc);
            float lh = st->font_size * 1.2f;
            float ly = y;
            const char *p = v;
            int caret = focused ? P->d->caret : -1;
            float cx_ = x, cy_ = y;
            for (;;) {
                const char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                size_t offset = (size_t)(p - v), start = selection_start > offset ? selection_start - offset : 0;
                size_t end = selection_end > offset ? selection_end - offset : 0;
                if (start > len) start = len;
                if (end > len) end = len;
                float left = x + wf_width(&ff, p, start), right = x + wf_width(&ff, p, end);
                bool line_selected = selected && selection_start <= offset + len && selection_end > offset;
                if (line_selected && e && selection_end > offset + len) right += wf_width(&ff, " ", 1);
                if (line_selected) gfx_fill(c, (int)floorf(left), (int)ly, (int)ceilf(right - left), (int)ceilf(lh), RGB(0, 117, 255));
                draw_text(P, st, x, ly + (lh - asc - desc) / 2 + asc, p, (int)len, col);
                if (line_selected) {
                    int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
                    gfx_clip(c, (int)floorf(left), (int)ly, (int)ceilf(right - left), (int)ceilf(lh));
                    draw_text(P, st, x, ly + (lh - asc - desc) / 2 + asc, p, (int)len, RGB(255, 255, 255));
                    c->cx0 = cx0; c->cy0 = cy0; c->cx1 = cx1; c->cy1 = cy1;
                }
                if (caret >= p - v && caret <= (int)(p - v + (long)len)) {
                    cx_ = x + wf_width(&ff, p, (size_t)(caret - (p - v)));
                    cy_ = ly;
                }
                ly += lh;
                if (!e) break;
                p = e + 1;
            }
            if (focused && !selected) gfx_fill(c, (int)cx_, (int)cy_ + 2, 1, (int)lh - 4, st->color);
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
        if(web_frame_element(n))web_avmedia_snapshot_reject();
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
    if (Y > c->cy1 || Y + H < c->cy0) return;
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
        image_t *s = im ? scaled_image(im, fitted_w, fitted_h) : NULL;
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
            if(web_avmedia_snapshot_is_probe()){web_avmedia_snapshot_reject();return;}
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
    return P->hx >= x && P->hx < x + w && P->hy >= y && P->hy < y + h &&
           P->hx + P->ox >= P->c->cx0 && P->hx + P->ox < P->c->cx1 &&
           P->hy + P->oy >= P->c->cy0 && P->hy + P->oy < P->c->cy1;
}

static int control_hit(box_t *b) {
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

static void hit_target(struct pctx *P,node_t *target) {
    P->target=target;
    P->hit_any=false;
    memset(P->hit,0,sizeof *P->hit);
}
static void set_hit(struct pctx *P, int kind, node_t *n, node_t *link) {
    if (n) hit_target(P,n);
    P->hit_any = true;
    P->hit->kind = kind;
    P->hit->node = n;
    P->hit->href = NULL;
    if (link) {
        P->hit->href = doc_link_href(P->d, link);
        if (kind == WEB_HIT_NONE) {
            P->hit->kind = WEB_HIT_LINK;
            P->hit->node = link;
        }
    }
}

static void set_disclosure_hit(struct pctx *P, node_t *details) {
    /* The action belongs to details, but the event target stays the summary
       (or its actual child), not a renderer-invented summary DOM node. */
    node_t *target = P->target;
    set_hit(P, WEB_HIT_DETAILS, details, NULL);
    P->target = target;
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

static void collect_layers(web_doc *d, pvec *layers, box_t *b) {
    for (box_t *c = b->first; c; c = c->next) {
        if (c->clamp_hidden || (c->st && c->st->display == D_NONE) || web_dialog_layer_box(d,c)) continue;
        if (paint_box_layer(c)) pv_push(layers, c);
        if (!stacking_context(c)) collect_layers(d, layers, c);
    }
}

/* the atomic inlines and floats inside an inline formatting context, in tree order */
static void inline_children(struct pctx *P, box_t *b, bool floats) {
    for (box_t *c = b->first; c; c = c->next) {
        if (c->clamp_hidden) continue;
        if (paint_box_layer(c)) continue;
        if (c->kind == B_INLINE) {
            inline_children(P, c, floats);
            continue;
        }
        if (c->kind == B_TEXT || c->kind == B_BR) continue;
        if (c->floated == floats) paint_box(P, c, false);
    }
}

static void paint_runs(struct pctx *P, box_t *b) {
    float bx = box_visual_x(b), by = cy(b);
    if (box_element_scrollable(b)) {
        bx -= (float)b->node->scroll_x;
        by -= (float)b->node->scroll_y;
    }
    /* inline box backgrounds */
    for (int i = 0; i < b->ndecos; i++) {
        struct deco *d = &b->decos[i];
        const style_t *st = d->st;
        if (st->visibility) continue;
        if (P->mode == M_HIT) {
            if (hit_style(st) && d->node && inside(P, bx + d->x, by + d->y, d->w, d->h)) hit_target(P,d->node);
            continue;
        }
        float l = d->first ? 0 : 0;
        (void)l;
        float bw[4] = {st->border_width[0], st->border_width[1], st->border_width[2], st->border_width[3]};
        if (!d->first) bw[3] = 0;
        if (!d->last) bw[1] = 0;
        float x = P->ox + bx + d->x, y = P->oy + by + d->y;
        if (y > P->c->cy1 || y + d->h < P->c->cy0) continue;
        paint_bg_border(P, st, x, y, d->w, d->h, bw, false, !d->first, !d->last);
    }
    for (int i = 0; i < b->nruns; i++) {
        struct run *r = &b->runs[i];
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
        paint_run_text(P, r, bx, by);
    }
}

static void paint_box(struct pctx *P, box_t *b, bool layer_root) {
    if (b->clamp_hidden || !b->st || b->st->display == D_NONE) return;
    if (web_dialog_layer_box(P->d,b) && b != P->top_root) return;
    if (P->mode == M_HIT && b->node && web_dialog_inert(P->d,b->node)) return;
    if (paint_box_layer(b) && !layer_root) return; /* painted with the layers */
    const style_t *st = b->st;
    /* Transparent auto boxes still participate in hit testing. */
    if (P->mode == M_PAINT && st->opacity <= 0.001f) return;
    pvec layers = {0};
    if (b == P->d->root_box || b == P->top_root || stacking_context(b)) {
        collect_layers(P->d, &layers, b);
        /* Stable ordering preserves tree order for equal z-index values. */
        for (int i = 1; i < layers.n; i++)
            for (int j = i; j > 0 && layer_z(layers.v[j - 1]) > layer_z(layers.v[j]); j--) {
                void *t = layers.v[j]; layers.v[j] = layers.v[j - 1]; layers.v[j - 1] = t;
            }
    }
    canvas_t *c = P->c;
    float x = box_visual_x(b), y = box_visual_y(b);
    float bx = x - b->p[3] - b->b[3], by = y - b->p[0] - b->b[0];
    float bw = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
    float bh = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
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
    bool clip = (b->clamp_truncated || st->overflow != OV_VISIBLE) && b->kind != B_INLINE && b->parent && !doc_viewport_overflow_box(P->d,b);
    /* Off-screen contexts can still contain viewport-fixed descendants. */
    if (clip && !layers.n && (P->oy + by > c->cy1 || P->oy + by + bh < c->cy0)) { pv_free(&layers); return; }
    /* group opacity: paint, then blend the result with what was there */
    uint32_t *saved = NULL;
    int gx = 0, gy = 0, gw = 0, gh = 0;
    if (P->mode == M_PAINT && st->opacity < 0.999f && b->kind != B_TEXT) {
        web_avmedia_snapshot_reject();
        gx = (int)floorf(P->ox + bx);
        gy = (int)floorf(P->oy + by);
        gw = (int)ceilf(bw) + 1;
        gh = (int)ceilf(bh) + 1;
        if (gx < c->cx0) gw -= c->cx0 - gx, gx = c->cx0;
        if (gy < c->cy0) gh -= c->cy0 - gy, gy = c->cy0;
        if (gx + gw > c->cx1) gw = c->cx1 - gx;
        if (gy + gh > c->cy1) gh = c->cy1 - gy;
        if (gw > 0 && gh > 0 && (saved = malloc(sizeof(uint32_t) * (size_t)gw * (size_t)gh)))
            for (int j = 0; j < gh; j++) memcpy(saved + (size_t)j * gw, c->px + (size_t)(gy + j) * c->pitch + gx, sizeof(uint32_t) * (size_t)gw);
    }
    bool visible = !st->visibility;
    if (b->kind != B_INLINE && b->kind != B_TEXT && b->kind != B_BR && b->kind != B_ROW && b->kind != B_ROW_GROUP) {
        if (P->mode == M_PAINT && visible) {
            bool skip_bg = (P->canvas_bg_from == 1 && b->node == P->d->html && b->node) ||
                           (P->canvas_bg_from == 2 && b->node == P->d->body && b->node);
            paint_bg_border(P, st, P->ox + bx, P->oy + by, bw, bh, b->b, skip_bg, false, false);
        } else if (P->mode == M_HIT && hit_style(st) && st->content_visibility!=CV_HIDDEN && b->kind == B_ATOMIC && inside(P, bx, by, bw, bh)) {
            int k = control_hit(b);
            if (k != WEB_HIT_NONE) set_hit(P, k, b->node, NULL);
        }
    } else if ((b->kind == B_ROW || b->kind == B_ROW_GROUP) && P->mode == M_PAINT && visible && (st->bg_color >> 24)) {
        float z[4] = {0, 0, 0, 0};
        paint_bg_border(P, st, P->ox + x, P->oy + y, b->w, b->h, z, false, false, false);
    }
    if (b->kind == B_ATOMIC && b->atomic != AT_INLINE_BLOCK) {
        if (P->mode == M_PAINT && visible && st->content_visibility!=CV_HIDDEN) paint_replaced(P, b);
        goto done;
    }
    if(st->content_visibility==CV_HIDDEN)goto done;
    if (b->marker || b->marker_shape) {
        if (P->mode == M_PAINT) paint_marker(P, b);
    }
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    bool contents = true;
    if (clip) {
        float px = P->ox + bx + b->b[3], py = P->oy + by + b->b[0];
        gfx_clip(c, (int)floorf(px), (int)floorf(py), (int)ceilf(bw - b->b[1] - b->b[3]), (int)ceilf(bh - b->b[0] - b->b[2]));
        if (P->mode == M_HIT && !inside(P, bx + b->b[3], by + b->b[0], bw - b->b[1] - b->b[3], bh - b->b[0] - b->b[2])) {
            contents = false;
        }
    }
    int layer = 0;
    for (; layer < layers.n && layer_z(layers.v[layer]) < 0; layer++)
        paint_stacked_layer(P, layers.v[layer]);
    if (contents && b->inline_ctx) {
        inline_children(P, b, true);
        paint_runs(P, b);
        inline_children(P, b, false);
    } else if (contents) {
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated == false) paint_box(P, ch, false);
        for (box_t *ch = b->first; ch; ch = ch->next)
            if (ch->floated) paint_box(P, ch, false);
    }
    for (; layer < layers.n; layer++) paint_stacked_layer(P, layers.v[layer]);
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
done:
    pv_free(&layers);
    if (saved) {
        int a = (int)(st->opacity * 255 + 0.5f);
        for (int j = 0; j < gh; j++) {
            uint32_t *row = c->px + (size_t)(gy + j) * c->pitch + gx, *old = saved + (size_t)j * gw;
            for (int i = 0; i < gw; i++) row[i] = gfx_mix(old[i], row[i], a);
        }
        free(saved);
    }
}

static void paint_stacked_layer(struct pctx *P, box_t *l) {
    /* Stacking order and containing-block clipping are independent. Rebuild
       this layer's clip from the viewport, not its paint context's clip:
       fixed children escape overflow, and absolute children skip overflow
       below their containing block. This also works across nested contexts. */
    canvas_t *c = P->c;
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    c->cx0 = P->vx0, c->cy0 = P->vy0, c->cx1 = P->vx1, c->cy1 = P->vy1;
    bool skip = false, reached = l->st->position != POS_ABSOLUTE;
    for (box_t *a = l->parent; a && a->parent; a = a->parent) {
        if (a->clamp_hidden || (a->st && a->st->display == D_NONE)) skip = true;
        if (a->st && a->kind != B_INLINE && a->st->position != POS_STATIC) reached = true;
        if (a->st && (a->clamp_truncated || a->st->overflow != OV_VISIBLE) && !doc_viewport_overflow_box(P->d,a) && a->kind != B_INLINE && reached &&
            l->st->position != POS_FIXED) {
            float ax = box_visual_x(a) - a->p[3], ay = box_visual_y(a) - a->p[0];
            gfx_clip(c, (int)(P->ox + ax), (int)(P->oy + ay), (int)(a->w + a->p[1] + a->p[3]), (int)(a->h + a->p[0] + a->p[2]));
        }
        /* Internal overflow still clips, but a viewport-fixed ancestor and
           its descendants escape ancestors outside that containing block. */
        if (a->st && a->st->position == POS_FIXED) break;
    }
    if (!skip) paint_box(P, l, true);
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
}

static void walk(struct pctx *P) {
    P->vx0 = P->c->cx0, P->vy0 = P->c->cy0;
    P->vx1 = P->c->cx1, P->vy1 = P->c->cy1;
    if (P->d->root_box) paint_box(P, P->d->root_box, true);
    float ox=P->ox,oy=P->oy,hx=P->hx,hy=P->hy;
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

void web_paint(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y) {
    paint_debug_begin(d);
    d->view_x=doc_x; d->view_y=doc_y; /* frame crops use this paint's real scroll */
    bool snapshot_root=!d->frame_parent;
    if(snapshot_root){web_frames_prepare_paint(d);web_avmedia_snapshot_begin(d);}
    int sx0 = c->cx0, sy0 = c->cy0, sx1 = c->cx1, sy1 = c->cy1;
    gfx_clip(c, x, y, w, h);
    struct pctx P = {0};
    P.d = d;
    P.c = c;
    P.ox = (float)(x - doc_x);
    P.oy = (float)(y - doc_y);
    P.mode = M_PAINT;
    int from;
    uint32_t bg = doc_canvas_bg(d, &from);
    /* a translucent canvas background is drawn over white */
    gfx_fill(c, x, y, w, h, RGB(255, 255, 255));
    gfx_fill_blend(c, x, y, w, h, bg);
    P.canvas_bg_from = (uint32_t)from;
    walk(&P);
    paint_debug_finish(d);
    if(snapshot_root) {
        canvas_t probe;int px,py;
        if(web_avmedia_snapshot_prepare(d,c,&probe,&px,&py,doc_x,doc_y)) {
            /* Only normal paint examines the committed tree. The small probe
             * canvas changes neither document view offsets nor the real UI. */
            struct pctx Q=P;Q.c=&probe;Q.ox-=px;Q.oy-=py;
            font_probe_begin();
            for(unsigned phase=1;phase<=2;phase++) {
                web_avmedia_snapshot_probe(phase);
                gfx_noclip(&probe);
                gfx_fill(&probe,0,0,probe.w,probe.h,RGB(255,255,255));
                gfx_fill_blend(&probe,x-px,y-py,w,h,bg);
                walk(&Q);
            }
            if(!font_probe_end())web_avmedia_snapshot_reject();
        }
        web_avmedia_snapshot_finish();
        web_frames_finish_paint(d);
    }
    c->cx0 = sx0, c->cy0 = sy0, c->cx1 = sx1, c->cy1 = sy1;
}

bool web_hit_test(web_doc *d, int x, int y, struct web_hit *hit) {
    memset(hit, 0, sizeof *hit);
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
    walk(&P);
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
    node_t *fallback=NULL;
    while(d && d->root_box) {
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
    walk(&P);
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
