/* filter.c's pixels against Filter Effects 1 on the host: each colour function against its §13.1
   primitive's matrix or transfer function applied to the unpremultiplied colour in double
   precision (§9.6, §9.7: the result clamped to [0, 1], then premultiplied), blur() against the §9.4
   box blurs (d odd: three boxes of d centred; d even: boxes of d centred on the pixel's left and
   right edges and one of d + 1) written here as direct sums, and against the Gaussian whose
   approximation they are, drop-shadow() against §13.1.10's chain (the input's alpha blurred,
   offset, flooded, merged under the input), and the edge modes. Then paint.c's own steps around
   them, as tests/check_filter_effects.py takes them from its source (paint_pieces.h): a surface
   painted over black and over white gives the premultiplied colour, the filtered surface is
   composited with the element's opacity through the clip's mask (§6: filter, clip, opacity), and
   a backdrop is drawn back source-over through the rounded border box (Filter Effects 2 §2.1).
   One line per check; the exit status is the number of failures. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "webi.h"
#include "parallel.h"
#include "paint_pieces.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

/* the parallel stages run in order on the host */
bool parallel_for_stage(enum parallel_stage stage, size_t count, parallel_fn fn, void *context,
                        parallel_service_fn service, void *service_context) {
    (void)stage; (void)service; (void)service_context;
    for (size_t i = 0; i < count; i++) fn(i, context);
    return true;
}
void parallel_record_work(enum parallel_stage stage, uint64_t units) { (void)stage; (void)units; }

static int failures, checks;
static void check(const char *name, bool ok, const char *detail) {
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, detail && *detail ? ": " : "", detail ? detail : "");
}
static struct css_filter *list1(struct css_filter_step s) {
    struct css_filter *f = calloc(1, sizeof *f + sizeof s);
    f->count = 1;
    f->steps[0] = s;
    f->effects = true;
    f->opacity = 1;
    return f;
}
static uint32_t premul(double r, double g, double b, double a) {
    uint32_t A = (uint32_t)lround(a * 255);
    return A << 24 | (uint32_t)lround(r * a * 255) << 16 | (uint32_t)lround(g * a * 255) << 8 | (uint32_t)lround(b * a * 255);
}
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* ---------------------------------------------------------------- the colour functions */
/* §13.1's primitive for kind at amount v, on an unpremultiplied colour */
static void reference_colour(uint8_t kind, double v, const double in[4], double out[4]) {
    double r = in[0], g = in[1], b = in[2], m[3][3];
    out[3] = in[3];
    switch (kind) {
    case CSS_FILTER_GRAYSCALE: {
        double k = 1 - v;
        double x[3][3] = {{0.2126 + 0.7874 * k, 0.7152 - 0.7152 * k, 0.0722 - 0.0722 * k},
                          {0.2126 - 0.2126 * k, 0.7152 + 0.2848 * k, 0.0722 - 0.0722 * k},
                          {0.2126 - 0.2126 * k, 0.7152 - 0.7152 * k, 0.0722 + 0.9278 * k}};
        memcpy(m, x, sizeof m);
        break;
    }
    case CSS_FILTER_SEPIA: {
        double k = 1 - v;
        double x[3][3] = {{0.393 + 0.607 * k, 0.769 - 0.769 * k, 0.189 - 0.189 * k},
                          {0.349 - 0.349 * k, 0.686 + 0.314 * k, 0.168 - 0.168 * k},
                          {0.272 - 0.272 * k, 0.534 - 0.534 * k, 0.131 + 0.869 * k}};
        memcpy(m, x, sizeof m);
        break;
    }
    case CSS_FILTER_SATURATE: {
        double x[3][3] = {{0.213 + 0.787 * v, 0.715 - 0.715 * v, 0.072 - 0.072 * v},
                          {0.213 - 0.213 * v, 0.715 + 0.285 * v, 0.072 - 0.072 * v},
                          {0.213 - 0.213 * v, 0.715 - 0.715 * v, 0.072 + 0.928 * v}};
        memcpy(m, x, sizeof m);
        break;
    }
    case CSS_FILTER_HUE_ROTATE: {
        double c = cos(v * M_PI / 180), s = sin(v * M_PI / 180);
        double x[3][3] = {{0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715, 0.072 - c * 0.072 + s * 0.928},
                          {0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283},
                          {0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715, 0.072 + c * 0.928 + s * 0.072}};
        memcpy(m, x, sizeof m);
        break;
    }
    case CSS_FILTER_INVERT: /* table [v, 1 - v]: v + C (1 - 2v) */
        out[0] = clamp01(v + r * (1 - 2 * v)); out[1] = clamp01(v + g * (1 - 2 * v)); out[2] = clamp01(v + b * (1 - 2 * v));
        return;
    case CSS_FILTER_BRIGHTNESS:
        out[0] = clamp01(r * v); out[1] = clamp01(g * v); out[2] = clamp01(b * v);
        return;
    case CSS_FILTER_CONTRAST: /* slope v, intercept -(0.5 v) + 0.5 */
        out[0] = clamp01(r * v - 0.5 * v + 0.5); out[1] = clamp01(g * v - 0.5 * v + 0.5); out[2] = clamp01(b * v - 0.5 * v + 0.5);
        return;
    case CSS_FILTER_OPACITY: /* the alpha's table [0, v] */
        out[0] = r; out[1] = g; out[2] = b; out[3] = in[3] * v;
        return;
    }
    for (int i = 0; i < 3; i++) out[i] = clamp01(m[i][0] * r + m[i][1] * g + m[i][2] * b);
}
static void colour_functions(void) {
    static const uint8_t kinds[] = {CSS_FILTER_GRAYSCALE, CSS_FILTER_SEPIA, CSS_FILTER_SATURATE, CSS_FILTER_HUE_ROTATE,
                                    CSS_FILTER_INVERT, CSS_FILTER_BRIGHTNESS, CSS_FILTER_CONTRAST, CSS_FILTER_OPACITY};
    static const char *names[] = {"grayscale", "sepia", "saturate", "hue-rotate", "invert", "brightness", "contrast", "opacity"};
    static const double amounts[8][4] = {{0, 0.3, 0.75, 1}, {0, 0.4, 0.9, 1}, {0, 0.5, 2, 3.5}, {0, 45, 180, 300},
                                         {0, 0.25, 0.5, 1}, {0, 0.5, 1.7, 3}, {0, 0.6, 1.5, 4}, {0, 0.2, 0.5, 1}};
    /* colours across the cube and alphas from opaque to almost transparent */
    double colours[64][4];
    int n = 0;
    for (int r = 0; r < 4; r++) for (int g = 0; g < 2; g++) for (int b = 0; b < 2; b++) for (int a = 0; a < 4; a++) {
        colours[n][0] = r / 3.0; colours[n][1] = g ? 0.85 : 0.1; colours[n][2] = b ? 0.6 : 0.05;
        colours[n][3] = (double[]){1, 0.75, 0.5, 0.2}[a];
        n++;
    }
    uint32_t px[64], expected[64];
    for (size_t k = 0; k < sizeof kinds; k++)
        for (int t = 0; t < 4; t++) {
            double v = amounts[k][t];
            int worst = 0;
            for (int i = 0; i < n; i++) {
                /* the input as it is stored, and its unpremultiplied value */
                px[i] = premul(colours[i][0], colours[i][1], colours[i][2], colours[i][3]);
                double A = (px[i] >> 24) / 255.0, in[4] = {(px[i] >> 16 & 255) / 255.0 / A, (px[i] >> 8 & 255) / 255.0 / A,
                                                            (px[i] & 255) / 255.0 / A, A}, out[4];
                reference_colour(kinds[k], v, in, out);
                expected[i] = premul(out[0], out[1], out[2], out[3]);
            }
            struct css_filter *f = list1((struct css_filter_step){.kind = kinds[k], .value = (float)v});
            web_filter_apply(NULL, NULL, f, px, 8, 8, false);
            for (int i = 0; i < n; i++)
                for (int lane = 0; lane < 32; lane += 8) {
                    int diff = abs((int)(px[i] >> lane & 255) - (int)(expected[i] >> lane & 255));
                    if (diff > worst) worst = diff;
                }
            char name[64], detail[64];
            snprintf(name, sizeof name, "%s(%g) against its primitive", names[k], v);
            snprintf(detail, sizeof detail, "largest difference %d/255", worst);
            check(name, worst <= 1, detail);
            free(f);
        }
    /* a chain clamps between its steps: brightness(3) then brightness(0.5) is not brightness(1.5) */
    struct css_filter *f = calloc(1, sizeof *f + 2 * sizeof(struct css_filter_step));
    f->count = 2;
    f->steps[0] = (struct css_filter_step){.kind = CSS_FILTER_BRIGHTNESS, .value = 3};
    f->steps[1] = (struct css_filter_step){.kind = CSS_FILTER_BRIGHTNESS, .value = 0.5f};
    /* rgb(153 51 25): 459 clamps to 255, 153, 75; then halves to 128 (127.5 up), 77 (76.5 up), 38 (37.5 up),
       not the 230 77 38 of the unclamped product brightness(1.5) */
    uint32_t p = 0xFF993319u;
    web_filter_apply(NULL, NULL, f, &p, 1, 1, false);
    char detail[64];
    snprintf(detail, sizeof detail, "%08x", p);
    check("brightness(3) brightness(0.5) clamps after the first", p == 0xFF804D26u, detail);
    free(f);
}

/* ---------------------------------------------------------------- blur */
/* §9.4 with direct sums: one box along x, reading [x - before, x + after], 8 bits after each box */
static void reference_box(const double *in, double *out, int n, int before, int after, bool mirror) {
    for (int x = 0; x < n; x++) {
        double sum = 0;
        for (int k = x - before; k <= x + after; k++) {
            int i = k;
            if (i < 0 || i >= n) {
                if (!mirror) continue;
                int period = 2 * n;
                i %= period; if (i < 0) i += period; if (i >= n) i = period - 1 - i;
            }
            sum += in[i];
        }
        out[x] = floor(sum / (before + after + 1) + 0.5);
    }
}
static void reference_blur_1d(double *line, int n, double sigma, bool mirror) {
    int d = (int)floor(sigma * 3 * sqrt(2 * M_PI) / 4 + 0.5);
    double *t = malloc(sizeof *t * (size_t)n);
    int reach[3][2];
    if (d & 1) for (int p = 0; p < 3; p++) reach[p][0] = reach[p][1] = (d - 1) / 2;
    else { reach[0][0] = d / 2; reach[0][1] = d / 2 - 1; reach[1][0] = d / 2 - 1; reach[1][1] = d / 2; reach[2][0] = reach[2][1] = d / 2; }
    for (int p = 0; p < 3; p++) { reference_box(line, t, n, reach[p][0], reach[p][1], mirror); memcpy(line, t, sizeof *t * (size_t)n); }
    free(t);
}
static void blur_boxes(double sigma, bool mirror) {
    /* one channel's pattern in each of the four, in each direction */
    enum { W = 61, H = 37 };
    static uint32_t px[W * H];
    static double plane[4][H][W];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int a = (x * 7 + y * 13) % 5 == 0 || (x > 20 && x < 30 && y > 10 && y < 20) ? 255 : (x * 3 + y) % 7 * 30;
        int r = a * ((x + y) % 3) / 2, g = a * (x % 2), b = a / 3;
        px[y * W + x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        for (int lane = 0; lane < 4; lane++) plane[lane][y][x] = px[y * W + x] >> (lane * 8) & 255;
    }
    for (int lane = 0; lane < 4; lane++) {
        for (int y = 0; y < H; y++) reference_blur_1d(plane[lane][y], W, sigma, mirror);
        double column[H];
        for (int x = 0; x < W; x++) {
            for (int y = 0; y < H; y++) column[y] = plane[lane][y][x];
            reference_blur_1d(column, H, sigma, mirror);
            for (int y = 0; y < H; y++) plane[lane][y][x] = column[y];
        }
    }
    struct css_filter *f = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = (float)sigma});
    web_filter_apply(NULL, NULL, f, px, W, H, mirror);
    int worst = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int lane = 0; lane < 4; lane++) {
        int diff = abs((int)(px[y * W + x] >> (lane * 8) & 255) - (int)plane[lane][y][x]);
        if (diff > worst) worst = diff;
    }
    char name[96], detail[64];
    int d = (int)floor(sigma * 3 * sqrt(2 * M_PI) / 4 + 0.5);
    snprintf(name, sizeof name, "blur(%gpx) is §9.4's three boxes of d = %d (%s), %s edges", sigma, d, d & 1 ? "odd" : "even",
             mirror ? "mirrored" : "transparent");
    snprintf(detail, sizeof detail, "largest difference %d/255", worst);
    check(name, worst <= 1, detail);
    free(f);
}
/* the error function (the product's libm has none): its Maclaurin series near 0, the
   continued fraction of erfc further out; both well inside 1e-9 of the exact value */
static double ref_erf(double x) {
    double ax = fabs(x);
    if (ax < 2.5) {
        double term = ax, sum = ax;
        for (int n = 1; n < 80; n++) {
            term *= -ax * ax / n;
            sum += term / (2 * n + 1);
        }
        return (x < 0 ? -1 : 1) * 2 / sqrt(M_PI) * sum;
    }
    double fraction = 0;
    for (int n = 60; n >= 1; n--) fraction = n / 2.0 / (ax + fraction);
    double erfc = exp(-ax * ax) / sqrt(M_PI) / (ax + fraction);
    return (x < 0 ? -1 : 1) * (1 - erfc);
}
/* an impulse spreads as the Gaussian of the deviation does (§9.4: the boxes are within about 3%) */
static void blur_gaussian(double sigma) {
    enum { W = 121 };
    static uint32_t px[W * W];
    memset(px, 0, sizeof px);
    /* a block bright enough to keep 8 bits of precision once spread */
    for (int y = 55; y < 66; y++) for (int x = 55; x < 66; x++) px[y * W + x] = 0xFFFFFFFFu;
    struct css_filter *f = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = (float)sigma});
    float outset[4], reach[4];
    web_filter_extent(f, outset, reach);
    web_filter_apply(NULL, NULL, f, px, W, W, false);
    double total = 0, worst = 0, peak = 0;
    for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) {
        double a = px[y * W + x] >> 24;
        total += a;
        /* the exact result: the block convolved with the Gaussian */
        double gx = 0.5 * (ref_erf((66 - (x + 0.5)) / (sigma * M_SQRT2)) - ref_erf((55 - (x + 0.5)) / (sigma * M_SQRT2)));
        double gy = 0.5 * (ref_erf((66 - (y + 0.5)) / (sigma * M_SQRT2)) - ref_erf((55 - (y + 0.5)) / (sigma * M_SQRT2)));
        double want = 255 * gx * gy;
        if (fabs(a - want) > worst) worst = fabs(a - want);
        if (want > peak) peak = want;
        if (a && (x < 55 - outset[0] || x > 65 + outset[2] || y < 55 - outset[1] || y > 65 + outset[3])) worst = 1e9;
    }
    char name[96], detail[96];
    snprintf(name, sizeof name, "blur(%gpx) of a block against the Gaussian, inside its extent", sigma);
    snprintf(detail, sizeof detail, "largest difference %.1f of a peak %.1f (%.1f%%), coverage %.0f of %d", worst, peak,
             100 * worst / peak, total, 121 * 255);
    check(name, worst <= 0.05 * peak + 1 && fabs(total - 121 * 255) <= 121 * 255 * 0.01, detail);
    free(f);
}
static void blur_edges(void) {
    /* backdrop-filter's mirrored edges keep an even backdrop even; transparent edges darken it */
    enum { W = 40, H = 30 };
    static uint32_t px[W * H];
    for (int i = 0; i < W * H; i++) px[i] = 0xFF336699u;
    struct css_filter *f = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = 6});
    web_filter_apply(NULL, NULL, f, px, W, H, true);
    bool even = true;
    for (int i = 0; i < W * H; i++) even &= px[i] == 0xFF336699u;
    check("blur(6px) of an even surface with mirrored edges stays even", even, NULL);
    free(f);
    /* blur(3px) reads 8 px each way: the middle keeps its colour, a corner loses half and more */
    for (int i = 0; i < W * H; i++) px[i] = 0xFF336699u;
    f = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = 3});
    web_filter_apply(NULL, NULL, f, px, W, H, false);
    char detail[64];
    snprintf(detail, sizeof detail, "corner %08x, middle %08x", px[0], px[15 * W + 20]);
    check("blur(3px) with transparent edges fades at the edge and keeps the middle",
          (px[0] >> 24) < 128 && px[15 * W + 20] == 0xFF336699u, detail);
    free(f);
    /* extents: blur(2px) is d = 4 (reach 3·4/2 - 1 = 5), blur(3px) d = 6 (8), blur(1px) the kernel's 3 */
    float outset[4], reach[4];
    const float sigmas[] = {2, 3, 1, 10};
    const float want[] = {5, 8, 3, 27};
    for (int i = 0; i < 4; i++) {
        struct css_filter *g = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = sigmas[i]});
        web_filter_extent(g, outset, reach);
        char name[64];
        snprintf(name, sizeof name, "blur(%gpx) reaches %g px", sigmas[i], want[i]);
        snprintf(detail, sizeof detail, "%g %g %g %g", outset[0], outset[1], outset[2], outset[3]);
        check(name, outset[0] == want[i] && outset[3] == want[i] && reach[1] == want[i], detail);
        free(g);
    }
}

/* ---------------------------------------------------------------- drop-shadow() */
static void drop_shadows(void) {
    enum { W = 40, H = 30 };
    static uint32_t px[W * H];
    /* a half-transparent blue square, its shadow opaque-ish red moved by (5, 3), no blur */
    memset(px, 0, sizeof px);
    for (int y = 8; y < 16; y++) for (int x = 8; x < 16; x++) px[y * W + x] = premul(0, 0, 1, 0.5);
    uint32_t red = 0xCCFF0000u;
    struct css_filter *f = list1((struct css_filter_step){.kind = CSS_FILTER_DROP_SHADOW, .x = 5, .y = 3, .color = red});
    web_filter_apply(NULL, NULL, f, px, W, H, false);
    int worst = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        bool in = x >= 8 && x < 16 && y >= 8 && y < 16, shadow = x >= 13 && x < 21 && y >= 11 && y < 19;
        /* the merge: the input (alpha 128) over the flood "in" the moved alpha */
        double sa = shadow ? 128 / 255.0 * (0xCC / 255.0) : 0, ia = in ? 128 / 255.0 : 0;
        double a = ia + sa * (1 - ia), r = sa * (1 - ia), b = ia;
        uint32_t want = (uint32_t)lround(a * 255) << 24 | (uint32_t)lround(r * 255) << 16 | (uint32_t)lround(b * 255);
        for (int lane = 0; lane < 32; lane += 8) {
            int diff = abs((int)(px[y * W + x] >> lane & 255) - (int)(want >> lane & 255));
            if (diff > worst) worst = diff;
        }
    }
    char detail[64];
    snprintf(detail, sizeof detail, "largest difference %d/255", worst);
    check("drop-shadow(rgb(255 0 0 / 0.8) 5px 3px) of a translucent square: §13.1.10's merge", worst <= 1, detail);
    free(f);
    /* with a blur, the shadow's alpha is the input's alpha blurred: the same total */
    memset(px, 0, sizeof px);
    for (int y = 10; y < 18; y++) for (int x = 10; x < 18; x++) px[y * W + x] = 0xFF000000u;
    struct css_filter *g = list1((struct css_filter_step){.kind = CSS_FILTER_DROP_SHADOW, .x = 4, .y = 4, .value = 2, .color = 0xFF00FF00u});
    uint32_t alpha[W * H];
    for (int i = 0; i < W * H; i++) alpha[i] = px[i] & 0xFF000000u;
    struct css_filter *blur = list1((struct css_filter_step){.kind = CSS_FILTER_BLUR, .value = 2});
    web_filter_apply(NULL, NULL, blur, alpha, W, H, false);
    web_filter_apply(NULL, NULL, g, px, W, H, false);
    worst = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        bool in = x >= 10 && x < 18 && y >= 10 && y < 18;
        int sx = x - 4, sy = y - 4;
        uint32_t s = sx >= 0 && sy >= 0 ? alpha[sy * W + sx] >> 24 : 0;
        uint32_t want = in ? 0xFF000000u : s << 24 | s << 8;
        for (int lane = 0; lane < 32; lane += 8) {
            int diff = abs((int)(px[y * W + x] >> lane & 255) - (int)(want >> lane & 255));
            if (diff > worst) worst = diff;
        }
    }
    snprintf(detail, sizeof detail, "largest difference %d/255", worst);
    check("drop-shadow(lime 4px 4px 2px): the alpha blurred as blur(2px), moved, under the input", worst <= 1, detail);
    float outset[4], reach[4];
    web_filter_extent(g, outset, reach);
    snprintf(detail, sizeof detail, "outset %g %g %g %g, reach %g %g %g %g", outset[0], outset[1], outset[2], outset[3], reach[0],
             reach[1], reach[2], reach[3]);
    /* blur(2px) reaches 5: the shadow reaches 5 - 4 left and up, 5 + 4 right and down; the input is read mirrored */
    check("drop-shadow(4px 4px 2px)'s extent", outset[0] == 1 && outset[1] == 1 && outset[2] == 9 && outset[3] == 9 &&
          reach[0] == 9 && reach[2] == 1, detail);
    free(g);
    free(blur);
}

/* paint.c's two surfaces: what was painted over black is the premultiplied colour, what white
   adds is the coverage left */
static void surface_coverage(void) {
    enum { W = 64 };
    static uint32_t black[W * 4], white[W * 4], want[W * 4];
    for (int i = 0; i < W * 4; i++) {
        uint32_t a = (uint32_t)(i * 37 % 256), k = 0, w = 0, p = a << 24;
        for (int shift = 0; shift < 24; shift += 8) {
            uint32_t c = (uint32_t)((i * (shift + 13) * 7) % 256), v = (c * a + 127) / 255;
            k |= v << shift; w |= (v + 255 - a) << shift; p |= v << shift;
        }
        black[i] = 0xFF000000u | k; white[i] = 0xFF000000u | w; want[i] = p;
    }
    struct transformed_job j = {.black = black, .white = white, .sw = W, .sh = 4};
    transformed_coverage(0, &j);
    int worst = 0;
    for (int i = 0; i < W * 4; i++) for (int shift = 0; shift < 32; shift += 8) {
        int diff = abs((int)(black[i] >> shift & 255) - (int)(want[i] >> shift & 255));
        if (diff > worst) worst = diff;
    }
    char detail[64];
    snprintf(detail, sizeof detail, "largest difference %d/255", worst);
    check("a surface over black and over white recovers its premultiplied colour", worst == 0, detail);
}
/* the filtered surface over the page: source-over with opacity o and the mask m, out = s*o*m +
   d*(1 - a*o*m), in exact arithmetic */
static void filtered_over(bool masked) {
    enum { W = 40, H = 6 };
    static uint32_t page[W * H], before[W * H], surface[W * H];
    static uint8_t mask[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        page[i] = before[i] = RGB(x * 6, y * 40, 200 - x * 5);
        uint32_t a = (uint32_t)((x * 29 + y * 11) % 256), p = a << 24;
        for (int shift = 0; shift < 24; shift += 8) p |= ((uint32_t)((x + 3 * y + shift) * 41 % 256) * a / 255) << shift;
        surface[i] = p;
        mask[i] = (uint8_t)((x * 13 + y * 50) % 256);
    }
    canvas_t c;
    gfx_init(&c, page, W, H, W);
    struct gfx_mask_clip clip = {.alpha = mask, .x = 0, .y = 0, .w = W, .h = H, .pitch = W};
    if (masked) c.mask_clip = &clip;
    const double opacity = 0.6;
    struct filtered_job j = {surface, &c, W, 0, 0, 0, 0, W, H, (uint32_t)(opacity * 255 + .5)};
    filtered_composite(0, &j);
    double worst = 0;
    for (int i = 0; i < W * H; i++) {
        double m = masked ? mask[i] / 255.0 : 1, a = (surface[i] >> 24) / 255.0 * opacity * m;
        for (int shift = 0; shift < 24; shift += 8) {
            double want = (surface[i] >> shift & 255) * opacity * m + (before[i] >> shift & 255) * (1 - a);
            double diff = fabs((double)(page[i] >> shift & 255) - want);
            if (diff > worst) worst = diff;
        }
        if (page[i] >> 24 != 255) worst = 1e9;
    }
    char name[96], detail[64];
    snprintf(name, sizeof name, "the filtered surface over the page with opacity 0.6%s", masked ? " through a clip's mask" : "");
    snprintf(detail, sizeof detail, "largest difference %.2f/255", worst);
    check(name, worst <= 1, detail);
}
/* the filtered backdrop over itself through a rounded border box at fractional edges */
static void backdrop_over(void) {
    enum { W = 48, H = 40 };
    static uint32_t page[W * H], before[W * H], filtered[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        page[i] = before[i] = RGB(x * 5, 250 - y * 6, (x * y) % 256);
        /* half of it translucent, as opacity() leaves a backdrop */
        uint32_t a = x < W / 2 ? 255 : 128, p = a << 24;
        for (int shift = 0; shift < 24; shift += 8) p |= ((uint32_t)(255 - ((before[i] >> shift) & 255)) * a / 255) << shift;
        filtered[i] = p;
    }
    canvas_t c;
    gfx_init(&c, page, W, H, W);
    struct backdrop_job j = {.filtered = filtered, .canvas = &c, .x = 0, .y = 0, .w = W, .h = H,
                             .bx = 4.25f, .by = 3.5f, .bw = 38.5f, .bh = 30.f, .alpha = 255};
    for (int k = 0; k < 4; k++) { j.radii.x[k] = 8; j.radii.y[k] = 6; }
    float reach[4] = {10, 8, 10, 8};
    j.inner[0] = j.bx + reach[0]; j.inner[1] = j.by + reach[1]; j.inner[2] = j.bx + j.bw - reach[2]; j.inner[3] = j.by + j.bh - reach[3];
    backdrop_composite(0, &j); backdrop_composite(1, &j);
    double worst = 0;
    int inside = 0, outside = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        double cover = corners_coverage(x + .5f, y + .5f, j.bx, j.by, j.bw, j.bh, &j.radii);
        double fa = (filtered[i] >> 24) / 255.0;
        if (cover >= 1) inside++;
        if (cover <= 0) outside++;
        for (int shift = 0; shift < 24; shift += 8) {
            double d = before[i] >> shift & 255, over = (filtered[i] >> shift & 255) + d * (1 - fa);
            double want = over * cover + d * (1 - cover), diff = fabs((double)(page[i] >> shift & 255) - want);
            if (diff > worst) worst = diff;
        }
    }
    char detail[96];
    snprintf(detail, sizeof detail, "largest difference %.2f/255 (%d pixels inside, %d outside)", worst, inside, outside);
    check("a backdrop is drawn back source-over through its rounded border box", worst <= 1 && inside > 0 && outside > 0, detail);
}
int main(void) {
    colour_functions();
    for (int mirror = 0; mirror < 2; mirror++) {
        blur_boxes(2, mirror);   /* d = 4, even */
        blur_boxes(2.5, mirror); /* d = 5, odd */
        blur_boxes(7, mirror);   /* d = 13 */
        blur_boxes(12, mirror);  /* d = 23, wider than half the surface */
    }
    blur_gaussian(3);
    blur_gaussian(8);
    blur_gaussian(1.2); /* the exact kernel */
    blur_edges();
    drop_shadows();
    surface_coverage();
    filtered_over(false);
    filtered_over(true);
    backdrop_over();
    printf("%d/%d\n", checks - failures, checks);
    return failures;
}
