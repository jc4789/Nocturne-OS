/* The filter functions' pixels (Filter Effects 1 §13: the primitives each function is), for filter
   and backdrop-filter (Filter Effects 2 §2). paint.c owns where they run: the group's private
   surface of an element's content, or the canvas under a backdrop-filter element.

   A surface is premultiplied ARGB, 8 bits a channel. Filter functions work in sRGB whatever
   color-interpolation-filters says (§10). The colour functions are linear maps of a pixel's colour
   (feColorMatrix, and feComponentTransfer's linear and table functions), so they apply to the
   premultiplied colour directly, each clamped to [0, alpha] as its unpremultiplied value is to
   [0, 1]. blur() is feGaussianBlur (§9.4: three box blurs, or the exact kernel below a deviation of
   2), drop-shadow() the §13.1.10 chain. What lies past a surface's edge is transparent black (edgeMode
   none), or for backdrop-filter the surface mirrored at its edge. */
#include <math.h>
#include <parallel.h>
#include "webi.h"

#define BAND 32 /* rows (or columns) one task of a stage owns */

uint32_t web_filter_shadow_color(const style_t *st, const struct css_filter_step *s) {
    if (s->color != COLOR_CURRENT) return s->color;
    return s->color_value ? css_color_value_argb(s->color_value, st->color) : st->color;
}

/* ---------------------------------------------------------------- extents */
/* feGaussianBlur's box size d for a deviation of at least 2 (§9.4) */
static int blur_box(double sigma) { return (int)floor(sigma * 3 * sqrt(2 * M_PI) / 4 + 0.5); }
/* deviations past this reach farther than any surface is kept */
#define SIGMA_MAX 100000.0
static double blur_sigma(double v) { return v > 0 ? fmin(v, SIGMA_MAX) : 0; }
/* how far a blur of deviation sigma reads from a pixel: the three boxes' reaches, or the kernel's */
static int blur_extent(double sigma) {
    if (!(sigma > 0)) return 0;
    if (sigma < 2) return (int)ceil(sigma * 3);
    int d = blur_box(sigma);
    return d & 1 ? 3 * (d - 1) / 2 : 3 * d / 2 - 1;
}
void web_filter_extent(const struct css_filter *f, float outset[4], float reach[4]) {
    double o[4] = {0, 0, 0, 0};
    for (size_t i = 0; f && i < f->count; i++) {
        const struct css_filter_step *s = &f->steps[i];
        double e = s->kind == CSS_FILTER_BLUR || s->kind == CSS_FILTER_DROP_SHADOW ? blur_extent(blur_sigma(s->value)) : 0;
        if (s->kind == CSS_FILTER_BLUR) for (int k = 0; k < 4; k++) o[k] += e;
        else if (s->kind == CSS_FILTER_DROP_SHADOW) {
            /* the shadow is the input moved by the offset and spread by its blur, under the input */
            double x = fabs(s->x) < 1e7 ? round(s->x) : copysign(1e7, s->x), y = fabs(s->y) < 1e7 ? round(s->y) : copysign(1e7, s->y);
            o[0] += fmax(0, e - x); o[1] += fmax(0, e - y); o[2] += fmax(0, e + x); o[3] += fmax(0, e + y);
        }
    }
    /* a pixel of the output reads the input as far as the output reaches past it, mirrored */
    for (int k = 0; k < 4; k++) {
        outset[k] = (float)fmin(o[k], 1e7);
        reach[k] = (float)fmin(o[(k + 2) & 3], 1e7);
    }
}

/* ---------------------------------------------------------------- the colour functions */
/* a step's colour map: c' = m·c + k·alpha for the three colour channels (premultiplied), the
   alpha times a */
struct colour_map { float m[3][3], k[3], a; };
static void colour_map(const struct css_filter_step *s, struct colour_map *out) {
    double v = s->value;
    if (s->kind != CSS_FILTER_HUE_ROTATE && v < 0) v = 0;
    if ((s->kind == CSS_FILTER_GRAYSCALE || s->kind == CSS_FILTER_INVERT || s->kind == CSS_FILTER_OPACITY ||
         s->kind == CSS_FILTER_SEPIA) && v > 1) v = 1;
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, k = 0, a = 1;
    switch (s->kind) {
    case CSS_FILTER_GRAYSCALE: { /* §13.1.1 */
        double r = 1 - v;
        double g[3][3] = {{0.2126 + 0.7874 * r, 0.7152 - 0.7152 * r, 0.0722 - 0.0722 * r},
                          {0.2126 - 0.2126 * r, 0.7152 + 0.2848 * r, 0.0722 - 0.0722 * r},
                          {0.2126 - 0.2126 * r, 0.7152 - 0.7152 * r, 0.0722 + 0.9278 * r}};
        memcpy(m, g, sizeof m);
        break;
    }
    case CSS_FILTER_SEPIA: { /* §13.1.2 */
        double r = 1 - v;
        double g[3][3] = {{0.393 + 0.607 * r, 0.769 - 0.769 * r, 0.189 - 0.189 * r},
                          {0.349 - 0.349 * r, 0.686 + 0.314 * r, 0.168 - 0.168 * r},
                          {0.272 - 0.272 * r, 0.534 - 0.534 * r, 0.131 + 0.869 * r}};
        memcpy(m, g, sizeof m);
        break;
    }
    case CSS_FILTER_SATURATE: { /* feColorMatrix type="saturate" (§9.6) */
        double g[3][3] = {{0.213 + 0.787 * v, 0.715 - 0.715 * v, 0.072 - 0.072 * v},
                          {0.213 - 0.213 * v, 0.715 + 0.285 * v, 0.072 - 0.072 * v},
                          {0.213 - 0.213 * v, 0.715 - 0.715 * v, 0.072 + 0.928 * v}};
        memcpy(m, g, sizeof m);
        break;
    }
    case CSS_FILTER_HUE_ROTATE: { /* feColorMatrix type="hueRotate" (§9.6) */
        double c = cos(v * M_PI / 180), n = sin(v * M_PI / 180);
        static const double base[3][3] = {{0.213, 0.715, 0.072}, {0.213, 0.715, 0.072}, {0.213, 0.715, 0.072}};
        static const double cs[3][3] = {{0.787, -0.715, -0.072}, {-0.213, 0.285, -0.072}, {-0.213, -0.715, 0.928}};
        static const double sn[3][3] = {{-0.213, -0.715, 0.928}, {0.143, 0.140, -0.283}, {-0.787, 0.715, 0.072}};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[i][j] = base[i][j] + c * cs[i][j] + n * sn[i][j];
        break;
    }
    /* feComponentTransfer (§13.1.5 to §13.1.8): the same function on each colour channel */
    case CSS_FILTER_INVERT: m[0][0] = m[1][1] = m[2][2] = 1 - 2 * v; k = v; break; /* table [v, 1 - v] */
    case CSS_FILTER_BRIGHTNESS: m[0][0] = m[1][1] = m[2][2] = v; break;            /* linear, slope v */
    case CSS_FILTER_CONTRAST: m[0][0] = m[1][1] = m[2][2] = v; k = 0.5 - 0.5 * v; break;
    case CSS_FILTER_OPACITY: a = v; break; /* the alpha's table [0, v] */
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out->m[i][j] = (float)m[i][j];
        out->k[i] = (float)k;
    }
    out->a = (float)a;
}
static bool colour_step(uint8_t kind) { return kind != CSS_FILTER_BLUR && kind != CSS_FILTER_DROP_SHADOW && kind != CSS_FILTER_URL; }
struct colour_job { uint32_t *px; int w, h; const struct colour_map *maps; size_t count; };
/* one band of rows through a run of colour steps, each clamped as its primitive's result is */
static void colour_band(size_t band, void *context) {
    const struct colour_job *j = context;
    int last = (int)MIN((band + 1) * BAND, (size_t)j->h);
    for (int y = (int)band * BAND; y < last; y++) {
        uint32_t *row = j->px + (size_t)y * (size_t)j->w;
        for (int x = 0; x < j->w; x++) {
            uint32_t p = row[x];
            float a = (float)(p >> 24);
            if (!a) continue;
            float c[3] = {(float)(p >> 16 & 255), (float)(p >> 8 & 255), (float)(p & 255)};
            for (size_t s = 0; s < j->count; s++) {
                const struct colour_map *m = &j->maps[s];
                float o[3];
                for (int i = 0; i < 3; i++) {
                    float v = m->m[i][0] * c[0] + m->m[i][1] * c[1] + m->m[i][2] * c[2] + m->k[i] * a;
                    o[i] = v < 0 ? 0 : v > a ? a : v;
                }
                if (m->a != 1) { a *= m->a; for (int i = 0; i < 3; i++) o[i] *= m->a; }
                memcpy(c, o, sizeof c);
                if (a <= 0) break;
            }
            uint32_t ai = (uint32_t)(a + .5f);
            if (!ai) { row[x] = 0; continue; }
            uint32_t ci[3];
            for (int i = 0; i < 3; i++) { ci[i] = (uint32_t)(c[i] + .5f); if (ci[i] > ai) ci[i] = ai; }
            row[x] = ai << 24 | ci[0] << 16 | ci[1] << 8 | ci[2];
        }
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->w * (uint64_t)MAX(0, last - (int)band * BAND));
}

/* ---------------------------------------------------------------- blur */
/* the index i of a line of n mirrored into it (its edge pixels repeat at the fold) */
static inline int mirrored(int i, int n) {
    int period = 2 * n;
    i %= period;
    if (i < 0) i += period;
    return i >= n ? period - 1 - i : i;
}
/* a channel's samples along one line, past its ends transparent or mirrored */
static inline uint32_t sample(const uint8_t *line, int n, int stride, int i, bool mirror) {
    if (i >= 0 && i < n) return line[(size_t)i * (size_t)stride];
    if (!mirror) return 0;
    return line[(size_t)mirrored(i, n) * (size_t)stride];
}
struct blur_job {
    uint8_t *a, *b;   /* the image and a scratch of its size; the result ends in a */
    int w, h, channels;
    int boxes, reach[3][2]; /* three box passes: each one's reach before and after the pixel */
    const float *kernel;    /* or one pass of this kernel, of reach radius each way */
    int radius;
    bool mirror;
};
/* one line (n samples, stride apart) from src to dst: a box of [x - before, x + after], or the kernel */
static void blur_line(const struct blur_job *j, const uint8_t *src, uint8_t *dst, int n, int stride, int pass) {
    if (j->kernel) {
        for (int x = 0; x < n; x++) {
            float sum = 0;
            for (int k = -j->radius; k <= j->radius; k++) sum += j->kernel[k + j->radius] * (float)sample(src, n, stride, x + k, j->mirror);
            uint32_t v = (uint32_t)(sum + .5f);
            dst[(size_t)x * (size_t)stride] = (uint8_t)(v > 255 ? 255 : v);
        }
        return;
    }
    int before = j->reach[pass][0], after = j->reach[pass][1];
    /* the mean, rounded half up: (sum + size / 2) / size by a multiply, exact while the sum fits 32 bits */
    uint32_t size = (uint32_t)(before + after + 1), half = size / 2;
    uint64_t magic = ((UINT64_C(1) << 32) + size - 1) / size;
    uint32_t sum = 0;
    for (int k = -before; k <= after; k++) sum += sample(src, n, stride, k, j->mirror);
    for (int x = 0; x < n; x++) {
        uint32_t v = (uint32_t)(((uint64_t)(sum + half) * magic) >> 32);
        dst[(size_t)x * (size_t)stride] = (uint8_t)(v > 255 ? 255 : v);
        sum += sample(src, n, stride, x + after + 1, j->mirror);
        sum -= sample(src, n, stride, x - before, j->mirror);
    }
}
/* the horizontal passes of a band of rows, through a row of scratch each */
static void blur_rows(size_t band, void *context) {
    const struct blur_job *j = context;
    int last = (int)MIN((band + 1) * BAND, (size_t)j->h), passes = j->kernel ? 1 : j->boxes;
    size_t pitch = (size_t)j->w * (size_t)j->channels;
    for (int y = (int)band * BAND; y < last; y++) {
        uint8_t *row = j->a + (size_t)y * pitch, *scratch = j->b + (size_t)y * pitch;
        for (int c = 0; c < j->channels; c++)
            for (int pass = 0; pass < passes; pass++) {
                const uint8_t *from = pass & 1 ? scratch : row;
                uint8_t *to = pass & 1 ? row : scratch;
                blur_line(j, from + c, to + c, j->w, j->channels, pass);
            }
        if (passes & 1) memcpy(row, scratch, pitch);
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->w * (uint64_t)MAX(0, last - (int)band * BAND) * (uint64_t)passes);
}
/* the vertical passes of a band of columns */
static void blur_columns(size_t band, void *context) {
    const struct blur_job *j = context;
    int last = (int)MIN((band + 1) * BAND, (size_t)j->w), passes = j->kernel ? 1 : j->boxes;
    int stride = j->w * j->channels;
    for (int x = (int)band * BAND; x < last; x++)
        for (int c = 0; c < j->channels; c++) {
            uint8_t *column = j->a + (size_t)x * (size_t)j->channels + c, *scratch = j->b + (size_t)x * (size_t)j->channels + c;
            for (int pass = 0; pass < passes; pass++) {
                const uint8_t *from = pass & 1 ? scratch : column;
                uint8_t *to = pass & 1 ? column : scratch;
                blur_line(j, from, to, j->h, stride, pass);
            }
            if (passes & 1) for (int y = 0; y < j->h; y++) column[(size_t)y * (size_t)stride] = scratch[(size_t)y * (size_t)stride];
        }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->h * (uint64_t)MAX(0, last - (int)band * BAND) * (uint64_t)passes);
}
static void filter_service(void *d) { web_native_checkpoint(d); }
static bool cancelled(web_doc *d) { return d && __atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE); }
/* a Gaussian blur of deviation sigma over w×h pixels of `channels` interleaved bytes */
static bool blur(web_doc *d, uint8_t *image, int w, int h, int channels, double sigma, bool mirror) {
    sigma = blur_sigma(sigma);
    if (!(sigma > 0) || w <= 0 || h <= 0) return true;
    size_t bytes = (size_t)w * (size_t)h * (size_t)channels;
    struct blur_job j = {.a = image, .w = w, .h = h, .channels = channels, .mirror = mirror};
    float *kernel = NULL;
    if (sigma < 2) {
        /* the exact kernel, normalized */
        j.radius = (int)ceil(sigma * 3);
        kernel = malloc(sizeof *kernel * (size_t)(2 * j.radius + 1));
        if (!kernel) return false;
        double sum = 0;
        for (int k = -j.radius; k <= j.radius; k++) sum += kernel[k + j.radius] = (float)exp(-(double)k * k / (2 * sigma * sigma));
        for (int k = 0; k <= 2 * j.radius; k++) kernel[k] = (float)(kernel[k] / sum);
        j.kernel = kernel;
    } else {
        /* §9.4: d odd, three boxes of d centred on the pixel; d even, two boxes of d centred on the pixel's
           left and right edges and one of d + 1 centred on it */
        int b = blur_box(sigma);
        j.boxes = 3;
        if (b & 1) for (int p = 0; p < 3; p++) j.reach[p][0] = j.reach[p][1] = (b - 1) / 2;
        else {
            j.reach[0][0] = b / 2; j.reach[0][1] = b / 2 - 1;
            j.reach[1][0] = b / 2 - 1; j.reach[1][1] = b / 2;
            j.reach[2][0] = j.reach[2][1] = b / 2;
        }
    }
    j.b = malloc(bytes);
    if (!j.b) { free(kernel); return false; }
    parallel_for_stage(PARALLEL_RASTER, ((size_t)h + BAND - 1) / BAND, blur_rows, &j, d ? filter_service : NULL, d);
    if (!cancelled(d)) parallel_for_stage(PARALLEL_RASTER, ((size_t)w + BAND - 1) / BAND, blur_columns, &j, d ? filter_service : NULL, d);
    free(j.b);
    free(kernel);
    return !cancelled(d);
}

/* ---------------------------------------------------------------- drop-shadow() */
struct shadow_job { uint32_t *px; const uint8_t *alpha; int w, h, dx, dy; uint32_t colour[4]; bool mirror; };
/* §13.1.10: the blurred alpha moved by the offset, flooded with the colour, under the input */
static void shadow_band(size_t band, void *context) {
    const struct shadow_job *j = context;
    int last = (int)MIN((band + 1) * BAND, (size_t)j->h);
    for (int y = (int)band * BAND; y < last; y++) {
        uint32_t *row = j->px + (size_t)y * (size_t)j->w;
        int sy = y - j->dy;
        bool inside = sy >= 0 && sy < j->h;
        if (!inside && !j->mirror) continue;
        int my = inside ? sy : mirrored(sy, j->h);
        for (int x = 0; x < j->w; x++) {
            int sx = x - j->dx;
            if (!j->mirror && (sx < 0 || sx >= j->w)) continue;
            int mx = sx >= 0 && sx < j->w ? sx : mirrored(sx, j->w);
            uint32_t s = j->alpha[(size_t)my * (size_t)j->w + (size_t)mx];
            if (!s) continue;
            uint32_t p = row[x], under = 255 - (p >> 24), out = 0;
            for (int lane = 0; lane < 4; lane++) {
                uint32_t shadow = (j->colour[lane] * s + 127) / 255; /* the flood "in" the shadow's alpha */
                uint32_t v = (p >> (lane * 8) & 255) + (shadow * under + 127) / 255;
                out |= (v > 255 ? 255 : v) << (lane * 8);
            }
            row[x] = out;
        }
    }
    parallel_record_work(PARALLEL_RASTER, (uint64_t)j->w * (uint64_t)MAX(0, last - (int)band * BAND));
}
static bool drop_shadow(web_doc *d, const style_t *st, const struct css_filter_step *s, uint32_t *px, int w, int h, bool mirror) {
    uint32_t argb = web_filter_shadow_color(st, s);
    if (!(argb >> 24)) return true;
    size_t count = (size_t)w * (size_t)h;
    uint8_t *alpha = malloc(count);
    if (!alpha) return false;
    for (size_t i = 0; i < count; i++) alpha[i] = (uint8_t)(px[i] >> 24);
    if (!blur(d, alpha, w, h, 1, s->value, mirror)) { free(alpha); return false; }
    struct shadow_job j = {px, alpha, w, h, 0, 0, {0}, mirror};
    j.dx = (int)fmax(-1e7, fmin(1e7, round(s->x)));
    j.dy = (int)fmax(-1e7, fmin(1e7, round(s->y)));
    uint32_t a = argb >> 24;
    for (int lane = 0; lane < 3; lane++) j.colour[lane] = ((argb >> (lane * 8) & 255) * a + 127) / 255;
    j.colour[3] = a;
    parallel_for_stage(PARALLEL_RASTER, ((size_t)h + BAND - 1) / BAND, shadow_band, &j, d ? filter_service : NULL, d);
    free(alpha);
    return !cancelled(d);
}

/* ---------------------------------------------------------------- the list */
bool web_filter_apply(web_doc *d, const style_t *st, const struct css_filter *f, uint32_t *px, int w, int h, bool mirror) {
    if (!f || w <= 0 || h <= 0) return true;
    for (size_t i = 0; i < f->count;) {
        if (d && !web_native_checkpoint(d)) return false;
        const struct css_filter_step *s = &f->steps[i];
        if (s->kind == CSS_FILTER_URL) { i++; continue; } /* paint.c resolved the references */
        if (s->kind == CSS_FILTER_BLUR) {
            if (!blur(d, (uint8_t *)px, w, h, 4, s->value, mirror)) return false;
            i++;
            continue;
        }
        if (s->kind == CSS_FILTER_DROP_SHADOW) {
            if (!drop_shadow(d, st, s, px, w, h, mirror)) return false;
            i++;
            continue;
        }
        /* a run of colour steps: one pass over the pixels */
        struct colour_map maps[16];
        size_t n = 0;
        for (; i < f->count && n < 16 && colour_step(f->steps[i].kind); i++) colour_map(&f->steps[i], &maps[n++]);
        struct colour_job j = {px, w, h, maps, n};
        parallel_for_stage(PARALLEL_RASTER, ((size_t)h + BAND - 1) / BAND, colour_band, &j, d ? filter_service : NULL, d);
        if (cancelled(d)) return false;
    }
    return true;
}
