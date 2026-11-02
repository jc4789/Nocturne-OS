/* The shapes of the pointer the window manager draws. */
#include "kernel.h"
#include "gui/wm.h"
#include "abi.h"

/* ---- the pointer ----
   Every shape of the pointer (N_POINTER_*, abi.h) is drawn here, once, into a square of
   N_SCREEN_POINTER_SIZE pixels of 0xAARRGGBB, not premultiplied: what the compositor blends, the
   display driver's own pointer shows and a remote viewer is sent. A shape is one or two sprites
   (an arrow and the badge beside it); a sprite is a white body with dark marks on it and a dark
   rim around it, built of rectangles, polygons, discs, rings and strokes given in quarter
   pixels, and sampled 4 x 4 a pixel. Only the WM calls this, under
   its lock. */
#define PS ((int)N_SCREEN_POINTER_SIZE)
#define SN (PS * 4) /* samples a side */
#define Q(p) ((p) * 4) /* pixels to quarter pixels */
enum { BODY, INK };
static uint64_t sprite[2][SN][SN / 64];
static uint32_t art[N_POINTER_SHAPES][PS * PS];
static bool art_drawn[N_POINTER_SHAPES];

/* The samples near a box (quarter pixels; a sample's centre is at s + 1/2), each with its centre
   (px, py) in eighths (2 s + 1), so a sample never lies on a vertex given in quarters. */
#define SAMPLES(lo0, hi0, lo1, hi1)                                                                 \
    for (int sy = MAX(0, (lo1) - 1); sy < MIN(SN, (hi1) + 1); sy++)                                   \
        for (int sx = MAX(0, (lo0) - 1), py = 2 * sy + 1; sx < MIN(SN, (hi0) + 1); sx++)               \
            for (int px = 2 * sx + 1, once = 1; once; once = 0)
static void mark(int layer, int sx, int sy) { sprite[layer][sy][sx >> 6] |= 1ull << (sx & 63); }

static void p_rect(int layer, int x0, int y0, int x1, int y1) {
    SAMPLES(x0, x1, y0, y1)
    if (px >= 2 * x0 && px < 2 * x1 && py >= 2 * y0 && py < 2 * y1) mark(layer, sx, sy);
}

static void p_disk(int layer, int cx, int cy, int r) {
    SAMPLES(cx - r, cx + r, cy - r, cy + r) {
        int dx = px - 2 * cx, dy = py - 2 * cy;
        if (dx * dx + dy * dy <= 4 * r * r) mark(layer, sx, sy);
    }
}

/* Octants count anticlockwise from the right (screen y grows down): bit 0 is 0..45 degrees up
   from the right, bit 2 is 90..135, and so on. */
static unsigned octant(int dx, int dy) {
    int ux = dx, uy = -dy;
    if (uy >= 0) return ux > 0 ? (ux >= uy ? 0 : 1) : (-ux < uy ? 2 : 3);
    return ux < 0 ? (-ux >= -uy ? 4 : 5) : (ux < -uy ? 6 : 7);
}

static void p_ring(int layer, int cx, int cy, int r0, int r1, unsigned octants) {
    SAMPLES(cx - r1, cx + r1, cy - r1, cy + r1) {
        int dx = px - 2 * cx, dy = py - 2 * cy, d = dx * dx + dy * dy;
        if (d >= 4 * r0 * r0 && d <= 4 * r1 * r1 && (octants >> octant(dx, dy) & 1)) mark(layer, sx, sy);
    }
}

/* A stroke of radius r with round ends from (x0, y0) to (x1, y1). */
static void p_stroke(int layer, int x0, int y0, int x1, int y1, int r) {
    SAMPLES(MIN(x0, x1) - r, MAX(x0, x1) + r, MIN(y0, y1) - r, MAX(y0, y1) + r) {
        int64_t vx = 2 * (x1 - x0), vy = 2 * (y1 - y0), wx = px - 2 * x0, wy = py - 2 * y0;
        int64_t t = wx * vx + wy * vy, len = vx * vx + vy * vy, rr = 4 * (int64_t)r * r;
        bool in;
        if (t <= 0 || !len) in = wx * wx + wy * wy <= rr;
        else if (t >= len) in = (wx - vx) * (wx - vx) + (wy - vy) * (wy - vy) <= rr;
        else in = (wx * vy - wy * vx) * (wx * vy - wy * vx) <= rr * len;
        if (in) mark(layer, sx, sy);
    }
}

/* A polygon of n points (x, y pairs), even-odd. */
static void p_poly(int layer, const int *xy, int n) {
    int x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
    for (int i = 1; i < n; i++) {
        x0 = MIN(x0, xy[2 * i]), x1 = MAX(x1, xy[2 * i]);
        y0 = MIN(y0, xy[2 * i + 1]), y1 = MAX(y1, xy[2 * i + 1]);
    }
    SAMPLES(x0, x1, y0, y1) {
        bool in = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int xi = 2 * xy[2 * i], yi = 2 * xy[2 * i + 1], xj = 2 * xy[2 * j], yj = 2 * xy[2 * j + 1];
            if ((yi > py) == (yj > py)) continue;
            int64_t l = (int64_t)(px - xi) * (yj - yi), r = (int64_t)(py - yi) * (xj - xi);
            if (yj > yi ? l < r : l > r) in = !in;
        }
        if (in) mark(layer, sx, sy);
    }
}
#define POLY(layer, ...)                                                                            \
    do {                                                                                            \
        static const int pts_[] = {__VA_ARGS__};                                                    \
        p_poly(layer, pts_, (int)ARRAY_SIZE(pts_) / 2);                                             \
    } while (0)

/* A polygon in a frame turned to the direction (ux, uy) (256 is 1) about (cx, cy): its points
   are (t, s) pairs, t along the direction and s across it. */
static int turn(int v) { return (v >= 0 ? v + 128 : v - 128) / 256; }
static void p_turned(int layer, int cx, int cy, int ux, int uy, const int *ts, int n) {
    int xy[32];
    n = MIN(n, 16);
    for (int i = 0; i < n; i++) {
        xy[2 * i] = cx + turn(ts[2 * i] * ux - ts[2 * i + 1] * uy);
        xy[2 * i + 1] = cy + turn(ts[2 * i] * uy + ts[2 * i + 1] * ux);
    }
    p_poly(layer, xy, n);
}

/* The coverage (0..16) of pixel (x, y) by a mask of sample rows. */
static int coverage(uint64_t (*m)[SN / 64], int x, int y) {
    static const uint8_t bits[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    int n = 0, b = x * 4;
    for (int r = y * 4; r < y * 4 + 4; r++) n += bits[(m[r][b >> 6] >> (b & 63)) & 15];
    return n;
}

/* The sprite drawn so far onto out (over what is there), its masks cleared for the next. The rim
   is the body grown by a disc of about a pixel. */
static void sprite_put(uint32_t *out) {
    static uint64_t grown[SN][SN / 64], rim[SN][SN / 64], white[SN][SN / 64], dark[SN][SN / 64];
    memset(grown, 0, sizeof grown);
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            if (dx * dx + dy * dy > 18) continue;
            for (int y = MAX(0, -dy); y < SN && y + dy < SN; y++) {
                const uint64_t *s = sprite[BODY][y + dy];
                uint64_t lo = s[0], hi = s[1];
                if (dx > 0) { lo = lo >> dx | hi << (64 - dx); hi >>= dx; }       /* takes x + dx */
                else if (dx < 0) { hi = hi << -dx | lo >> (64 + dx); lo <<= -dx; } /* takes x - |dx| */
                grown[y][0] |= lo;
                grown[y][1] |= hi;
            }
        }
    for (int y = 0; y < SN; y++)
        for (int k = 0; k < SN / 64; k++) {
            uint64_t body = sprite[BODY][y][k], ink = sprite[INK][y][k] & body;
            rim[y][k] = grown[y][k] & ~body;
            white[y][k] = body & ~ink;
            dark[y][k] = ink;
        }
    const int shade = 14; /* the rim's and the marks' grey */
    for (int y = 0; y < PS; y++)
        for (int x = 0; x < PS; x++) {
            int w = coverage(white, x, y), d = coverage(rim, x, y) + coverage(dark, x, y), n = w + d;
            if (!n) continue;
            uint32_t a = (uint32_t)(n * 255 / 16), g = (uint32_t)((w * 255 + d * shade) / n);
            uint32_t *p = &out[y * PS + x], under = *p, ua = under >> 24;
            uint32_t oa = a + ua * (255 - a) / 255, rgb = 0;
            for (int bit = 0; bit < 24; bit += 8) {
                uint32_t v = (g * a + ((under >> bit) & 255) * ua * (255 - a) / 255) / oa;
                rgb |= (v & 255) << bit;
            }
            *p = oa << 24 | rgb;
        }
    memset(sprite, 0, sizeof sprite);
}

/* The arrow every badge goes with, its tip at (1, 1). */
static void arrow_sprite(void) {
    POLY(BODY, Q(2), Q(2), Q(2), Q(18) + 2, Q(6), Q(14) + 3, Q(8) + 2, Q(20) + 2, Q(11) + 1, Q(19) + 2,
         Q(8) + 2, Q(13) + 3, Q(13) + 2, Q(13) + 2);
}

/* An hourglass between x0 and x1 (pixels), y0 and y1, sand in its top and bottom. */
static void hourglass(int x0, int y0, int x1, int y1) {
    int bar = Q(y1 - y0) / 10 + 2, l = Q(x0), r = Q(x1), t = Q(y0), b = Q(y1), mid = (t + b) / 2,
        cx = (l + r) / 2, neck = (r - l) / 14 + 1, side = (r - l) / 12 + 2;
    p_rect(BODY, l, t, r, t + bar);
    p_rect(BODY, l, b - bar, r, b);
    int glass[] = {l + side, t + bar, r - side, t + bar, r - side, t + bar + 2, cx + neck, mid,
                   r - side, b - bar - 2, r - side, b - bar, l + side, b - bar, l + side, b - bar - 2,
                   cx - neck, mid, l + side, t + bar + 2};
    p_poly(BODY, glass, (int)ARRAY_SIZE(glass) / 2);
    p_rect(INK, l, t, r, t + bar);
    p_rect(INK, l, b - bar, r, b);
    int in = (r - l) / 4, top = t + bar + (mid - t - bar) / 4, low = b - bar - 2;
    int sand_top[] = {l + in, top, r - in, top, cx, mid - 2};
    int sand_low[] = {l + in - 2, low, r - in + 2, low, cx, low - (low - mid) / 2};
    p_poly(INK, sand_top, 3);
    p_poly(INK, sand_low, 3);
    p_stroke(INK, cx, mid - 2, cx, low - 2, 2);
}

/* A hand: fingers is 1 for the pointing hand, 2 open, 3 closed. */
static void hand(int fingers) {
    static const int tips[3][4] = {{Q(4) + 2, Q(11), Q(12), Q(13) + 2},
                                   {Q(7), Q(5) + 2, Q(6) + 2, Q(9)},
                                   {Q(12), Q(11), Q(11) + 2, Q(12) + 2}};
    static const int fx[4] = {Q(11) + 2, Q(15), Q(18) + 2, Q(22)};
    const int *tip = tips[fingers - 1];
    for (int i = 0; i < 4; i++) p_stroke(BODY, fx[i], tip[i], fx[i], Q(15) + (i == 3 ? 2 : 0), i == 3 ? 6 : 7);
    POLY(BODY, Q(9) + 3, Q(14), Q(23) + 2, Q(14) + 2, Q(23) + 2, Q(20), Q(21), Q(25) + 2, Q(13), Q(25) + 2,
         Q(9) + 3, Q(20) + 2);
    if (fingers == 3) p_stroke(BODY, Q(8) + 2, Q(16), Q(12) + 2, Q(18) + 2, 7);
    else p_stroke(BODY, Q(6), Q(15) + 2 - (fingers == 2 ? 8 : 0), Q(10), Q(20) + 2, 7);
    for (int i = 0; i < 3; i++) {
        int x = (fx[i] + fx[i + 1]) / 2;
        p_stroke(INK, x, MAX(tip[i], tip[i + 1]) + 6, x, Q(15) + 2 + (i == 2 ? 2 : 0), 2);
    }
}

/* An arrow along (ux, uy) (256 is 1) through the centre: two heads, or one head at the end with a
   bar past it (an edge to move). In quarter pixels: L from the centre to an end, heads hl long and
   hw wide each way, the shaft sw wide each way. */
static void resize_arrow(int ux, int uy, bool both) {
    const int c = Q(16) + 2, L = Q(11) + 2, hl = Q(5) + 2, hw = Q(5), sw = 5;
    if (both) {
        const int ts[] = {L, 0, L - hl, hw, L - hl, sw, -(L - hl), sw, -(L - hl), hw,
                          -L, 0, -(L - hl), -hw, -(L - hl), -sw, L - hl, -sw, L - hl, -hw};
        p_turned(BODY, c, c, ux, uy, ts, (int)ARRAY_SIZE(ts) / 2);
        return;
    }
    const int T = L - 9;
    const int ts[] = {T, 0, T - hl, hw, T - hl, sw, -L + 4, sw, -L + 4, -sw, T - hl, -sw, T - hl, -hw};
    p_turned(BODY, c, c, ux, uy, ts, (int)ARRAY_SIZE(ts) / 2);
    const int bar[] = {L - 6, hw + 2, L, hw + 2, L, -hw - 2, L - 6, -hw - 2};
    p_turned(BODY, c, c, ux, uy, bar, 4);
}

/* Two bars with arrows out of them: rows false for the columns (the arrows go left and right),
   true for the rows (up and down). */
static void split_arrows(bool rows) {
    const int c = Q(16) + 2;
    const int bars[2][8] = {{-10, -Q(9) - 2, -4, -Q(9) - 2, -4, Q(9) + 2, -10, Q(9) + 2},
                            {4, -Q(9) - 2, 10, -Q(9) - 2, 10, Q(9) + 2, 4, Q(9) + 2}};
    const int head[] = {-Q(13), 0, -Q(8), -Q(4) - 2, -Q(8), -5, -10, -5, -10, 5, -Q(8), 5, -Q(8), Q(4) + 2};
    for (int k = 0; k < 2; k++) {
        int way = k ? -256 : 256;
        p_turned(BODY, c, c, rows ? 0 : 256, rows ? 256 : 0, bars[k], 4);
        p_turned(BODY, c, c, rows ? 0 : way, rows ? way : 0, head, (int)ARRAY_SIZE(head) / 2);
    }
}

/* Four heads about the centre, with the shafts between them (move) or a dot (all-scroll). */
static void four_ways(bool shafts) {
    static const int dir[4][2] = {{256, 0}, {0, 256}, {-256, 0}, {0, -256}};
    const int c = Q(16) + 2, far = shafts ? Q(14) : Q(13) + 2, base = shafts ? Q(8) : Q(7) + 2,
              hw = shafts ? Q(5) : Q(4) + 2;
    for (int k = 0; k < 4; k++) {
        const int ts[] = {far, 0, base, hw, base, -hw};
        p_turned(BODY, c, c, dir[k][0], dir[k][1], ts, 3);
    }
    if (shafts) {
        p_rect(BODY, c - Q(9), c - 5, c + Q(9), c + 5);
        p_rect(BODY, c - 5, c - Q(9), c + 5, c + Q(9));
    } else {
        p_disk(BODY, c, c, 9);
    }
}

/* A magnifying glass; plus is whether its mark is a plus or a minus. */
static void magnifier(bool plus) {
    const int c = Q(13) + 2;
    p_disk(BODY, c, c, Q(8) + 2);
    p_stroke(BODY, Q(19) + 2, Q(19) + 2, Q(26) + 2, Q(26) + 2, 9);
    p_ring(INK, c, c, Q(6) + 2, Q(8) + 2, 0xFF);
    p_rect(INK, c - Q(3) - 2, c - 3, c + Q(3) + 2, c + 3);
    if (plus) p_rect(INK, c - 3, c - Q(3) - 2, c + 3, c + Q(3) + 2);
}

static const uint8_t art_hot[N_POINTER_SHAPES][2] = {
    [N_POINTER_DEFAULT] = {1, 1}, [N_POINTER_CONTEXT_MENU] = {1, 1}, [N_POINTER_HELP] = {1, 1},
    [N_POINTER_POINTER] = {11, 2}, [N_POINTER_PROGRESS] = {1, 1}, [N_POINTER_WAIT] = {16, 16},
    [N_POINTER_CELL] = {16, 16}, [N_POINTER_CROSSHAIR] = {16, 16}, [N_POINTER_TEXT] = {16, 16},
    [N_POINTER_VERTICAL_TEXT] = {16, 16}, [N_POINTER_ALIAS] = {1, 1}, [N_POINTER_COPY] = {1, 1},
    [N_POINTER_MOVE] = {16, 16}, [N_POINTER_NO_DROP] = {1, 1}, [N_POINTER_NOT_ALLOWED] = {16, 16},
    [N_POINTER_GRAB] = {16, 15}, [N_POINTER_GRABBING] = {16, 16},
    [N_POINTER_E_RESIZE] = {16, 16}, [N_POINTER_N_RESIZE] = {16, 16}, [N_POINTER_NE_RESIZE] = {16, 16},
    [N_POINTER_NW_RESIZE] = {16, 16}, [N_POINTER_S_RESIZE] = {16, 16}, [N_POINTER_SE_RESIZE] = {16, 16},
    [N_POINTER_SW_RESIZE] = {16, 16}, [N_POINTER_W_RESIZE] = {16, 16}, [N_POINTER_EW_RESIZE] = {16, 16},
    [N_POINTER_NS_RESIZE] = {16, 16}, [N_POINTER_NESW_RESIZE] = {16, 16}, [N_POINTER_NWSE_RESIZE] = {16, 16},
    [N_POINTER_COL_RESIZE] = {16, 16}, [N_POINTER_ROW_RESIZE] = {16, 16}, [N_POINTER_ALL_SCROLL] = {16, 16},
    [N_POINTER_ZOOM_IN] = {13, 13}, [N_POINTER_ZOOM_OUT] = {13, 13},
};

static void draw_shape(int shape, uint32_t *out) {
    /* the directions of the resize shapes from N_POINTER_E_RESIZE on (256 is 1) */
    static const int16_t dirs[12][2] = {{256, 0},    {0, -256}, {181, -181}, {-181, -181},
                                        {0, 256},    {181, 181}, {-181, 181}, {-256, 0},
                                        {256, 0},    {0, 256},   {181, -181}, {181, 181}};
    memset(out, 0, PS * PS * 4);
    const int c = Q(16) + 2;
    switch (shape) {
    case N_POINTER_NONE: return;
    case N_POINTER_CONTEXT_MENU:
        arrow_sprite();
        sprite_put(out);
        p_rect(BODY, Q(15), Q(13), Q(25), Q(24) + 2);
        for (int i = 0; i < 3; i++) p_rect(INK, Q(17), Q(15) + 2 + i * 11, Q(23), Q(16) + 3 + i * 11);
        break;
    case N_POINTER_HELP:
        arrow_sprite();
        sprite_put(out);
        p_disk(BODY, Q(21), Q(20), Q(6) + 1);
        p_ring(INK, Q(21), Q(18), Q(1) + 2, Q(3), 0x8F);
        p_stroke(INK, Q(22) + 2, Q(19) + 2, Q(21), Q(21) + 1, 3);
        p_disk(INK, Q(21), Q(23) + 2, 4);
        break;
    case N_POINTER_POINTER: hand(1); break;
    case N_POINTER_PROGRESS:
        arrow_sprite();
        sprite_put(out);
        hourglass(16, 13, 25, 25);
        break;
    case N_POINTER_WAIT: hourglass(8, 4, 25, 29); break;
    case N_POINTER_CELL:
        p_rect(BODY, Q(6) + 2, Q(13) + 2, Q(26) + 2, Q(19) + 2);
        p_rect(BODY, Q(13) + 2, Q(6) + 2, Q(19) + 2, Q(26) + 2);
        break;
    case N_POINTER_CROSSHAIR:
        p_stroke(BODY, Q(4), c, Q(13), c, 3);
        p_stroke(BODY, Q(20), c, Q(29), c, 3);
        p_stroke(BODY, c, Q(4), c, Q(13), 3);
        p_stroke(BODY, c, Q(20), c, Q(29), 3);
        p_disk(BODY, c, c, 4);
        break;
    case N_POINTER_TEXT:
    case N_POINTER_VERTICAL_TEXT: {
        bool v = shape == N_POINTER_VERTICAL_TEXT;
        const int bar_a[] = {-Q(3) - 2, -Q(10) - 2, Q(3) + 2, -Q(10) - 2, Q(3) + 2, -Q(8) - 2, -Q(3) - 2, -Q(8) - 2};
        const int bar_b[] = {-Q(3) - 2, Q(8) + 2, Q(3) + 2, Q(8) + 2, Q(3) + 2, Q(10) + 2, -Q(3) - 2, Q(10) + 2};
        const int stem[] = {-4, -Q(8) - 2, 4, -Q(8) - 2, 4, Q(8) + 2, -4, Q(8) + 2};
        p_turned(BODY, c, c, v ? 0 : 256, v ? 256 : 0, bar_a, 4);
        p_turned(BODY, c, c, v ? 0 : 256, v ? 256 : 0, bar_b, 4);
        p_turned(BODY, c, c, v ? 0 : 256, v ? 256 : 0, stem, 4);
        break;
    }
    case N_POINTER_ALIAS:
        arrow_sprite();
        sprite_put(out);
        p_rect(BODY, Q(15), Q(13), Q(25), Q(23) + 2);
        p_ring(INK, Q(22) + 2, Q(21) + 2, Q(2) + 1, Q(3) + 2, 0x0C);
        POLY(INK, Q(22), Q(16) + 1, Q(24) + 2, Q(18) + 3, Q(22), Q(21) + 1);
        p_stroke(INK, Q(19) + 2, Q(21) + 2, Q(19) + 2, Q(22) + 2, 3);
        break;
    case N_POINTER_COPY:
        arrow_sprite();
        sprite_put(out);
        p_rect(BODY, Q(15), Q(13), Q(25), Q(23));
        p_rect(INK, Q(19) + 1, Q(15), Q(20) + 3, Q(21));
        p_rect(INK, Q(17), Q(17) + 1, Q(23), Q(18) + 3);
        break;
    case N_POINTER_MOVE: four_ways(true); break;
    case N_POINTER_ALL_SCROLL: four_ways(false); break;
    case N_POINTER_NO_DROP:
        arrow_sprite();
        sprite_put(out);
        p_disk(BODY, Q(21), Q(20), Q(6) + 1);
        p_ring(INK, Q(21), Q(20), Q(3) + 2, Q(5), 0xFF);
        p_stroke(INK, Q(18) + 2, Q(17) + 2, Q(23) + 2, Q(22) + 2, 3);
        break;
    case N_POINTER_NOT_ALLOWED:
        p_ring(BODY, c, c, Q(6) + 2, Q(11) + 2, 0xFF);
        p_stroke(BODY, c - Q(5) - 1, c - Q(5) - 1, c + Q(5) + 1, c + Q(5) + 1, 8);
        break;
    case N_POINTER_GRAB: hand(2); break;
    case N_POINTER_GRABBING: hand(3); break;
    case N_POINTER_COL_RESIZE: split_arrows(false); break;
    case N_POINTER_ROW_RESIZE: split_arrows(true); break;
    case N_POINTER_ZOOM_IN: magnifier(true); break;
    case N_POINTER_ZOOM_OUT: magnifier(false); break;
    default:
        if (shape >= N_POINTER_E_RESIZE && shape <= N_POINTER_NWSE_RESIZE) {
            const int16_t *d = dirs[shape - N_POINTER_E_RESIZE];
            resize_arrow(d[0], d[1], shape >= N_POINTER_EW_RESIZE);
        } else {
            arrow_sprite();
        }
        break;
    }
    sprite_put(out);
}

const uint32_t *pointer_art(int shape, int *hot_x, int *hot_y) {
    if (shape < 0 || shape >= N_POINTER_SHAPES) shape = N_POINTER_DEFAULT;
    if (!art_drawn[shape]) {
        draw_shape(shape, art[shape]);
        art_drawn[shape] = true;
    }
    *hot_x = art_hot[shape][0];
    *hot_y = art_hot[shape][1];
    return art[shape];
}
