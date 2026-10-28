/* CSS properties: the table of supported properties, value parsing (lengths, calc(), colours,
   keywords), shorthand expansion, and the user agent stylesheet. */
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <stddef.h>
#include <ctype.h>
#include "webi.h"
#include "inspect_private.h"

/* ---------------------------------------------------------------- small parsing helpers */
static void trim(const char **s, size_t *n) {
    while (*n && is_space((unsigned char)**s)) (*s)++, (*n)--;
    while (*n && is_space((unsigned char)(*s)[*n - 1])) (*n)--;
}

static bool ident_is(const char *s, size_t n, const char *kw) { return strn_ieq(s, kw, n); }

/* split at top-level separators (whitespace when sep is ' '), keeping () and quotes together */
static int split(const char *s, size_t n, const char **tok, size_t *tl, int max, char sep) {
    int cnt = 0;
    size_t i = 0;
    while (i < n && cnt < max) {
        if (sep == ' ') {
            while (i < n && is_space((unsigned char)s[i])) i++;
            if (i >= n) break;
        }
        size_t st = i;
        int depth = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) {
                if (c == '\\') i++;
                else if (c == q) q = 0;
            } else if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (depth <= 0 && (sep == ' ' ? is_space((unsigned char)c) : c == sep)) break;
            i++;
        }
        const char *ts = s + st;
        size_t tn = (i < n ? i : n) - st;
        trim(&ts, &tn);
        tok[cnt] = ts;
        tl[cnt++] = tn;
        if (i < n && sep != ' ') i++;
    }
    return cnt;
}

/* Rendering retains its existing recovery behavior; feature queries must not
   report success after a parser silently drops extra tokens. */
static int split_checked(const char *s, size_t n, const char **tok, size_t *tl, int max, char sep, struct cx *cx) {
    int k = split(s, n, tok, tl, max, sep);
    if (cx->supports_probe && k) {
        const char *end = tok[k - 1] + tl[k - 1];
        while (end < s + n && is_space((unsigned char)*end)) end++;
        if (end != s + n) cx->invalid = true;
        for (int i = 0; i < k; i++) if (!tl[i]) cx->invalid = true;
    }
    return k;
}

/* Resolve name-character escapes without turning escaped punctuation into CSS
   syntax. Used by both the cascade and queries. Quoted strings remain encoded
   for the existing string parser. Canonical var() spelling also makes the
   cascade's substitution dispatch case-insensitive. */
const char *css_value_canonical(arena_t *a, const char *s, size_t n, size_t *out_n) {
    *out_n = n;
    bool needed = memchr(s, '\\', n) != NULL;
    for (size_t i = 0; !needed && i + 4 <= n; i++)
        if (strn_ieq(s + i, "var(", 4) && (s[i] != 'v' || s[i + 1] != 'a' || s[i + 2] != 'r')) needed = true;
    if (!needed) return s;
    char *out = ar_alloc(a, n * 3 + 1);
    size_t k = 0;
    char quote = 0;
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (quote) {
            out[k++] = s[i++];
            if (c == '\\' && i < n) out[k++] = s[i++];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out[k++] = s[i++]; continue; }
        if (c == '\\' && i + 1 < n) {
            size_t start = i++;
            uint32_t cp = 0;
            int digits = 0;
            while (i < n && digits < 6 && isxdigit((unsigned char)s[i])) {
                int d = s[i] | 32;
                cp = cp * 16 + (uint32_t)(d <= '9' ? d - '0' : d - 'a' + 10);
                i++; digits++;
            }
            if (!digits) cp = (unsigned char)s[i++];
            else if (i < n && is_space((unsigned char)s[i])) {
                if (s[i++] == '\r' && i < n && s[i] == '\n') i++;
            }
            if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
            if (cp >= 128 || isalnum((unsigned char)cp) || cp == '-' || cp == '_') {
                char u[4]; int len = utf8_put(u, cp);
                memcpy(out + k, u, (size_t)len); k += (size_t)len;
            } else { memcpy(out + k, s + start, i - start); k += i - start; }
            continue;
        }
        out[k++] = s[i++];
    }
    out[k] = 0;
    /* At a function-token boundary only; names such as myVAR() are unchanged. */
    for (size_t i = 0; i + 4 <= k;) {
        char c = out[i];
        if (c == '"' || c == '\'') {
            i++; while (i < k && out[i] != c) { if (out[i] == '\\') i++; i++; } i++;
        } else if (strn_ieq(out + i, "var(", 4) && (!i || !(isalnum((unsigned char)out[i - 1]) ||
                     out[i - 1] == '-' || out[i - 1] == '_' || (unsigned char)out[i - 1] >= 128))) {
            memcpy(out + i, "var", 3); i += 4;
        } else i++;
    }
    *out_n = k;
    return out;
}

void css_unescape(sbuf *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\') {
            sb_putc(out, s[i]);
            continue;
        }
        if (++i >= n) break;
        if (s[i] == '\n') continue;
        uint32_t cp = 0;
        int k = 0;
        while (k < 6 && i < n && isxdigit((unsigned char)s[i])) {
            int c = s[i] | 32;
            cp = cp * 16 + (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
            i++, k++;
        }
        if (k) {
            if (i < n && is_space((unsigned char)s[i])) i++;
            i--;
            if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            sb_utf8(out, cp);
        } else sb_putc(out, s[i]);
    }
}

static bool parse_number(const char **ps, const char *e, float *out) {
    const char *s = *ps;
    double sign = 1, v = 0;
    if (s < e && (*s == '+' || *s == '-')) sign = *s++ == '-' ? -1 : 1;
    bool digits = false;
    while (s < e && *s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), digits = true;
    if (s < e && *s == '.' && s + 1 < e && s[1] >= '0' && s[1] <= '9') {
        s++;
        double f = 0.1;
        while (s < e && *s >= '0' && *s <= '9') v += (*s++ - '0') * f, f /= 10, digits = true;
    }
    if (!digits) return false;
    if (s + 1 < e && (*s | 32) == 'e' &&
        ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s + 2 < e && s[2] >= '0' && s[2] <= '9'))) {
        s++;
        int es = 1, ev = 0;
        if (*s == '-' || *s == '+') es = *s++ == '-' ? -1 : 1;
        while (s < e && *s >= '0' && *s <= '9') {
            /* A huge exponent is still a number token, not signed-int UB. */
            if (ev < 10000) ev = ev * 10 + (*s - '0');
            s++;
        }
        v *= pow(10, es * ev);
    }
    *out = (float)(sign * v);
    *ps = s;
    return true;
}

/* The implemented filter backend is a real opacity function list. Preserve
   computed numbers/percentages (including values above one), while clipping
   only their used amounts. Unknown effects invalidate the whole declaration. */
static bool parse_filter(const char *v, size_t n, struct cx *cx, const struct css_filter **out) {
    if (ident_is(v, n, "none")) { *out = NULL; return true; }
    size_t count = 0;
    const char *end = v + n;
    struct css_filter *result = NULL;
    for (unsigned pass = 0; pass < 2; pass++) {
        const char *q = v;
        size_t i = 0;
        float used = 1;
        while (q < end) {
            while (q < end && is_space((unsigned char)*q)) q++;
            if (q == end) break;
            if ((size_t)(end - q) < 8 || !strn_ieq(q, "opacity", 7) || q[7] != '(') return false;
            q += 8;
            while (q < end && is_space((unsigned char)*q)) q++;
            float value = 1;
            bool percentage = false;
            if (q == end) return false;
            if (*q != ')') {
                if (!parse_number(&q, end, &value) || !isfinite(value) || value < 0) return false;
                if (q < end && *q == '%') { percentage = true; q++; }
                while (q < end && is_space((unsigned char)*q)) q++;
            }
            if (q == end || *q++ != ')') return false;
            if (i == SIZE_MAX) return false;
            if (pass) result->steps[i] = (struct css_filter_step){value, percentage};
            i++;
            float amount = percentage ? value / 100 : value;
            used *= amount > 1 ? 1 : amount;
            if (!(i & 63) && !cx->supports_probe && cx->node && !web_native_checkpoint(cx->node->owner)) return false;
        }
        if (!i) return false;
        if (!pass) {
            count = i;
            if (count > (SIZE_MAX - sizeof *result) / sizeof *result->steps) return false;
            result = ar_alloc(cx->a, sizeof *result + count * sizeof *result->steps);
        } else { result->count = count; result->opacity = used; }
    }
    *out = result;
    return true;
}

/* ---------------------------------------------------------------- lengths and numbers (cssmath.c owns the math) */
/* Unlike the general sb_put API, the author-readable paths report OOM. */
static bool length_append(sbuf *out, const char *text, size_t length) {
    if (out->n == SIZE_MAX || length > SIZE_MAX - out->n - 1) return false;
    size_t need = out->n + length + 1;
    if (need > out->cap) {
        size_t capacity = out->cap ? out->cap : 64;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) { capacity = need; break; }
            capacity *= 2;
        }
        char *p = realloc(out->p, capacity);
        if (!p) return false;
        out->p = p; out->cap = capacity;
    }
    memcpy(out->p + out->n, text, length);
    out->n += length; out->p[out->n] = 0;
    return true;
}
static bool length_number(sbuf *out, float value, const char *unit) {
    if (!isfinite(value)) return false;
    if (value == 0) value = 0; /* CSS serialization has no negative zero. */
    char number[64];
    int n = snprintf(number, sizeof number, "%.9g%s", (double)value, unit);
    return n > 0 && (size_t)n < sizeof number && length_append(out, number, (size_t)n);
}
bool css_shadow_serialize(const struct css_box_shadow *shadow, sbuf *out, web_doc *d) {
    size_t original = out->n; bool success = true;
    if (!shadow) success = length_append(out, "none", 4);
    size_t count = 0;
    for (; shadow && success; shadow = shadow->next, count++) {
        if (!(count & 63) && d && !web_native_checkpoint(d)) { success = false; break; }
        success = (!count || length_append(out, ", ", 2)) &&
            css_color_value_text(shadow->color_value, shadow->color, out) && length_append(out, " ", 1) &&
            length_number(out, shadow->x, "px") && length_append(out, " ", 1) && length_number(out, shadow->y, "px") &&
            length_append(out, " ", 1) && length_number(out, shadow->blur, "px") && length_append(out, " ", 1) &&
            length_number(out, shadow->spread, "px") && (!shadow->inset || length_append(out, " inset", 6));
    }
    if (!success) { out->n = original; if (out->p && original < out->cap) out->p[original] = 0; }
    return success;
}
bool css_filter_serialize(const struct css_filter *filter, sbuf *out, web_doc *d) {
    size_t original = out->n;
    bool success = false;
    if (!filter) success = length_append(out, "none", 4);
    else if (filter->count) {
        success = true;
        for (size_t i = 0; i < filter->count; i++) {
            if (!(i & 63) && d && !web_native_checkpoint(d)) { success = false; break; }
            const struct css_filter_step *step = &filter->steps[i];
            if (step->value < 0 || (i && !length_append(out, " ", 1)) ||
                !length_append(out, "opacity(", 8) || !length_number(out, step->value, step->percentage ? "%" : "") ||
                !length_append(out, ")", 1)) { success = false; break; }
        }
    }
    if (!success) {
        out->n = original;
        if (out->p && original < out->cap) out->p[original] = 0;
    }
    return success;
}
/* a math function's infinity is the largest value a float field keeps (CSS Values 4 §10.12) */
static float calc_float(double v) { return v != v ? 0 : v > FLT_MAX ? FLT_MAX : v < -FLT_MAX ? -FLT_MAX : (float)v; }
static bool parse_len(const char *s, size_t n, struct cx *cx, len_t *out, int flags) {
    trim(&s, &n);
    if (!n) return false;
    memset(out, 0, sizeof *out);
    if (isalpha((unsigned char)s[0]) || (s[0] == '-' && n > 1 && isalpha((unsigned char)s[1]))) {
        size_t k = 0;
        while (k < n && s[k] != '(') k++;
        if (k == n) {
            if (ident_is(s, n, "auto")) {
                if (!(flags & LF_AUTO)) return false;
                out->kind = LK_AUTO;
                return true;
            }
            if (ident_is(s, n, "none")) {
                if (!(flags & LF_NONE)) return false;
                out->kind = LK_NONE;
                return true;
            }
            if (ident_is(s, n, "normal")) {
                if (!(flags & LF_NORMAL)) return false;
                out->kind = LK_NORMAL;
                return true;
            }
            if (ident_is(s, n, "min-content") || ident_is(s, n, "max-content") || ident_is(s, n, "fit-content") ||
                ident_is(s, n, "-webkit-fill-available") || ident_is(s, n, "-moz-available") ||
                ident_is(s, n, "stretch") || ident_is(s, n, "-webkit-fit-content") || ident_is(s, n, "-moz-fit-content")) {
                if (cx->supports_probe) return false; /* intrinsic sizing is currently an auto/none fallback */
                if (flags & LF_AUTO) out->kind = LK_AUTO;
                else if (flags & LF_NONE) out->kind = LK_NONE;
                else return false;
                return true;
            }
            return false;
        }
        if (ident_is(s, k, "fit-content")) {
            if (cx->supports_probe) return false;
            if (!(flags & LF_AUTO)) return false;
            out->kind = LK_AUTO;
            return true;
        }
        /* a math function of type <length> (or <length-percentage>): computed, range-clamped. anchor()
           and anchor-size() are <length>s where the property takes them (css-anchor-position-1 §3.2, §5.1) */
        struct css_calc_parse P = {.a = cx->a, .cx = cx, .anchors = (uint8_t)(flags & (LF_ANCHOR | LF_ANCHOR_SIZE))};
        struct css_calc *c = css_calc_read(&P, CSS_CALC_LENGTH | (flags & LF_NOPCT ? 0 : CSS_CALC_PERCENT), s, n, NULL);
        return c && css_length_from_calc(c, flags & LF_NEG, out);
    }
    const char *p = s;
    float v;
    if (!parse_number(&p, s + n, &v)) return false;
    int unit = css_unit_find(p, (size_t)(s + n - p));
    switch (css_unit_kind(unit)) {
    case CSS_CALC_NUMBER:
        if ((flags & LF_LENGTH) && v != 0) return false;
        out->px = v;
        break;
    case CSS_CALC_PERCENT:
        if ((flags & LF_NOPCT) || cx->absolute_lengths) return false;
        out->pct = v;
        break;
    case CSS_CALC_LENGTH:
        if (cx->absolute_lengths && css_unit_relative(unit)) return false;
        out->px = (float)css_unit_canonical(unit, v, cx);
        break;
    default: return false;
    }
    if (!(flags & LF_NEG) && v < 0) return false;
    out->kind = LK_LEN;
    return true;
}
/* A <number>: a number token, or a math function whose type is <number>.
   calc(20 / 16) is the multiplier 1.25 wherever a number is accepted, never
   1.25px. */
static bool parse_number_value(const char *v, size_t n, struct cx *cx, float *out) {
    trim(&v, &n);
    const char *q = v;
    if (parse_number(&q, v + n, out)) return q == v + n;
    double x;
    if (!css_calc_is_math(v, n) || !css_numeric_read(v, n, CSS_CALC_NUMBER, cx, cx->a, NAN, &x, NULL)) return false;
    *out = calc_float(x);
    return true;
}
/* The value grammar for the value owners outside this file (shape.c). */
bool css_parse_length(const char *s, size_t n, struct cx *cx, len_t *out, int flags) { return parse_len(s, n, cx, out, flags); }
bool css_number_token(const char **s, const char *end, float *out) { return parse_number(s, end, out); }

/* ---------------------------------------------------------------- colours (color.c owns them) */
/* a <color> for a property that keeps only the 8-bit used value (COLOR_CURRENT for currentcolor) */
static bool css_color_checked(const char *s, size_t n, uint32_t *out, struct cx *cx) {
    return css_color_value_compute(s, n, cx, NULL, out, NULL);
}

/* ---------------------------------------------------------------- the property table */
struct kw {
    const char *name;
    uint8_t v;
};

static bool shadow_specified_length(sbuf *out, const char *token, size_t size) {
    /* Preserve authored calculation/relative-unit expressions: this is a
       specified value, not a computed pixel value. Unitless zero is a length. */
    const char *p = token; float number;
    if (parse_number(&p, token + size, &number) && p == token + size && number == 0)
        return length_append(out, "0px", 3);
    return length_append(out, token, size);
}
static bool parse_box_shadows(const char *value, size_t length, struct cx *cx, const struct css_box_shadow **out,
                              sbuf *specified) {
    if (ident_is(value, length, "none")) { *out = NULL; return !specified || length_append(specified,"none",4); }
    const char *cursor = value, *end = value + length;
    struct css_box_shadow *first = NULL, **tail = &first;
    while (cursor < end) {
        const char *stop = cursor; size_t depth = 0;
        for (; stop < end; stop++) {
            if (*stop == '(') depth++;
            else if (*stop == ')') { if (!depth) return false; depth--; }
            else if (*stop == ',' && !depth) break;
        }
        if (depth) return false;
        const char *tokens[6]; size_t sizes[6];
        int count = split_checked(cursor, (size_t)(stop - cursor), tokens, sizes, 6, ' ', cx);
        if (count < 2) return false;
        const char *consumed = tokens[count-1] + sizes[count-1];
        while (consumed < stop && is_space((unsigned char)*consumed)) consumed++;
        if (consumed != stop) return false; /* strict in cascade and CSSOM too */
        struct css_box_shadow shadow = {.color = COLOR_CURRENT};
        bool color = false, inset = false, lengths_closed = false; unsigned lengths = 0;
        float dimensions[4] = {0};
        int color_token = -1, length_tokens[4];
        for (int i = 0; i < count; i++) {
            uint32_t candidate;
            const struct css_color_value *exact;
            if (ident_is(tokens[i], sizes[i], "inset")) {
                if (inset) return false; inset = shadow.inset = true; lengths_closed |= lengths != 0;
            } else if (css_color_value_compute(tokens[i], sizes[i], cx, NULL, &candidate, &exact)) {
                if (color) return false; color = true; shadow.color = candidate; shadow.color_value = exact;
                color_token=i; lengths_closed |= lengths != 0;
            } else {
                if (lengths_closed || lengths == 4 || memchr(tokens[i], '%', sizes[i])) return false;
                len_t dimension;
                unsigned flags = LF_NOPCT | LF_LENGTH | (lengths == 2 ? 0 : LF_NEG);
                if (!parse_len(tokens[i], sizes[i], cx, &dimension, flags) || dimension.kind != LK_LEN || dimension.pct ||
                    !isfinite(dimension.px)) return false;
                length_tokens[lengths] = i; dimensions[lengths++] = dimension.px;
            }
        }
        if (lengths < 2) return false;
        if (specified) {
            if (first && !length_append(specified,", ",2)) return false;
            if (color_token >= 0 && (!css_color_specified(tokens[color_token],sizes[color_token],specified) ||
                !length_append(specified," ",1))) return false;
            for (unsigned i=0;i<lengths;i++) {
                int token=length_tokens[i];
                if ((i && !length_append(specified," ",1)) ||
                    !shadow_specified_length(specified,tokens[token],sizes[token])) return false;
            }
            if (inset && !length_append(specified," inset",6)) return false;
        }
        shadow.x = dimensions[0]; shadow.y = dimensions[1]; shadow.blur = dimensions[2]; shadow.spread = dimensions[3];
        struct css_box_shadow *node = ar_alloc(cx->a, sizeof *node); *node = shadow; *tail = node; tail = &node->next;
        if (stop == end) break;
        cursor = stop + 1;
        const char *remaining = cursor; size_t n = (size_t)(end - cursor); trim(&remaining, &n); if (!n) return false;
    }
    if (!first) return false; *out = first; return true;
}
bool css_shadow_specified(const char *value, size_t size, sbuf *out) {
    size_t original=out->n;
    arena_t *arena=calloc(1,sizeof *arena);if(!arena)return false;
    jmp_buf trap;arena->trap=&trap;
    bool success=false;
    if (!setjmp(trap)) {
        struct cx cx={.em=16,.rem=16,.vw=800,.vh=600,.a=arena};
        const struct css_box_shadow *ignored;
        trim(&value,&size);
        success=parse_box_shadows(value,size,&cx,&ignored,out);
    }
    ar_free(arena);free(arena);
    if(!success){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}
    return success;
}
/* A <border-radius> value of four corners (TL TR BR BL) per axis, in its shortest form. */
static bool radius_lengths_serialize(const len_t lengths[2][4],sbuf *out,web_doc *doc) {
    bool ok=true,equal=true;for(int i=0;i<4;i++)equal&=css_length_equal(&lengths[0][i],&lengths[1][i]);
    for(int axis=0;axis<(equal?1:2)&&ok;axis++) {
        int count=4;if(css_length_equal(&lengths[axis][1],&lengths[axis][3])){count=3;if(css_length_equal(&lengths[axis][0],&lengths[axis][2])){count=2;if(css_length_equal(&lengths[axis][0],&lengths[axis][1]))count=1;}}
        if(axis)ok=length_append(out," / ",3);
        for(int i=0;i<count&&ok;i++)ok=(!i||length_append(out," ",1))&&css_length_serialize(&lengths[axis][i],out,doc);
    }
    return ok;
}
bool css_radius_serialize(const style_t *style,int corner,sbuf *out,web_doc *doc) {
    size_t original=out->n;bool ok=true;len_t lengths[2][4];
    for(int i=0;i<4;i++)for(int axis=0;axis<2;axis++) {
        if(style->corner_radius[i].axis[axis].kind!=LK_AUTO)lengths[axis][i]=style->corner_radius[i].axis[axis];
        else lengths[axis][i]=(len_t){.kind=LK_LEN,.px=style->border_radius>=0?style->border_radius:0,.pct=style->border_radius<0?-style->border_radius:0};
    }
    if(corner>=0&&corner<4) {
        ok=css_length_serialize(&lengths[0][corner],out,doc);
        if(ok&&!css_length_equal(&lengths[0][corner],&lengths[1][corner]))ok=length_append(out," ",1)&&css_length_serialize(&lengths[1][corner],out,doc);
    } else ok=radius_lengths_serialize(lengths,out,doc);
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}return ok;
}
bool css_clip_rect_serialize(const struct css_clip_rect *clip,sbuf *out) {
    size_t original=out->n;bool ok=true;
    if(!clip->rect)ok=length_append(out,"auto",4);
    else {
        ok=length_append(out,"rect(",5);
        for(int i=0;i<4&&ok;i++)ok=(!i||length_append(out,", ",2))&&(clip->auto_edges>>i&1?length_append(out,"auto",4):length_number(out,clip->edge[i],"px"));
        ok=ok&&length_append(out,")",1);
    }
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}
    return ok;
}
bool css_radius_specified(const char *property,const char *value,size_t size,sbuf *out) {
    if(!css_valid_declaration(property,strlen(property),value,size))return false;
    if(memchr(value,'\\',size))return false;
    for(size_t i=0;i+4<=size;i++)if(strn_ieq(value+i,"var(",4))return false;
    bool shorthand=!strcasecmp(property,"border-radius");size_t original=out->n;
    struct cx cx={0};const char *axes[3];size_t axis_n[3];
    int count=shorthand?split_checked(value,size,axes,axis_n,3,'/',&cx):1;
    if(!shorthand){axes[0]=value;axis_n[0]=size;}
    if(count<1||count>2)return false;
    const char *tokens[2][5];size_t token_n[2][5];int lengths[2];
    for(int axis=0;axis<count;axis++) {
        lengths[axis]=split_checked(axes[axis],axis_n[axis],tokens[axis],token_n[axis],5,' ',&cx);
        if(lengths[axis]<1||lengths[axis]>(shorthand?4:2))return false;
    }
    if(shorthand&&count==2) {
        static const int pick[5][4]={{0},{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};bool equal=true;
        for(int i=0;i<4;i++){int x=pick[lengths[0]][i],y=pick[lengths[1]][i];equal&=token_n[0][x]==token_n[1][y]&&!memcmp(tokens[0][x],tokens[1][y],token_n[0][x]);}
        if(equal)count=1;
    }
    bool ok=true;
    for(int axis=0;axis<count&&ok;axis++) {
        int n=lengths[axis];
        if(shorthand) {
            while(n>1) {
                int other=n==4?1:n==3?0:0;
                if(token_n[axis][n-1]!=token_n[axis][other]||memcmp(tokens[axis][n-1],tokens[axis][other],token_n[axis][other]))break;
                n--;
            }
        } else if(n==2&&token_n[axis][0]==token_n[axis][1]&&!memcmp(tokens[axis][0],tokens[axis][1],token_n[axis][0]))n=1;
        if(axis)ok=length_append(out," / ",3);
        for(int i=0;i<n&&ok;i++)ok=(!i||length_append(out," ",1))&&shadow_specified_length(out,tokens[axis][i],token_n[axis][i]);
    }
    if(!ok){out->n=original;if(out->p&&original<out->cap)out->p[original]=0;}return ok;
}

enum { PT_SHORT, PT_KW, PT_DISPLAY, PT_LEN, PT_PX, PT_COLOR, PT_NUM, PT_INT, PT_FONT_SIZE, PT_FONT_WEIGHT,
       PT_FONT_FAMILY, PT_LINE_HEIGHT, PT_VALIGN, PT_CONTENT, PT_LIST_TYPE, PT_BG_IMAGE, PT_TEXT_DECO,
       PT_OVERFLOW, PT_OPACITY, PT_FILTER, PT_BOX_SHADOW, PT_RADIUS, PT_ZINDEX, PT_BG_POS, PT_BG_SIZE, PT_GTEMPLATE, PT_GAREAS, PT_GLINE, PT_MASK_IMAGE, PT_MASK_SIZE, PT_SVG_PAINT, PT_SVG_WIDTH, PT_CONTAINER_NAMES, PT_LINE_CLAMP,
       PT_MOTION_LIST, PT_TRANSFORM, PT_TRANSLATE,
       PT_ROTATE, PT_SCALE, PT_TRANSFORM_ORIGIN, PT_SHAPE, PT_OFFSET, PT_CLIP, PT_COLOR_SCHEME, PT_RULE_LIST,
       PT_RULE_INSET, PT_ANCHOR, PT_COUNTER, PT_QUOTES, PT_LIST_IMAGE,
       PT_BG_LIST, PT_BG_OFFSET, PT_BI_SOURCE, PT_BI_SLICE, PT_BI_SIDES, PT_BI_REPEAT, PT_MASK_POSITION, PT_MASK_REPEAT };

enum { SH_MARGIN = 1, SH_PADDING, SH_INSET, SH_BORDER_RADIUS, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM,
       SH_BORDER_LEFT, SH_BORDER_WIDTH, SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BORDER_INLINE, SH_BORDER_BLOCK,
       SH_MARGIN_INLINE, SH_MARGIN_BLOCK, SH_PADDING_INLINE, SH_PADDING_BLOCK, SH_INSET_INLINE, SH_INSET_BLOCK,
       SH_LIST_STYLE, SH_FONT, SH_BACKGROUND, SH_FLEX, SH_FLEX_FLOW, SH_GAP, SH_OVERFLOW, SH_TEXT_DECORATION, SH_ALL,
       SH_PLACE_ITEMS, SH_PLACE_SELF, SH_PLACE_CONTENT, SH_GRID_AREA, SH_GRID_ROW, SH_GRID_COLUMN,
       SH_GRID_TEMPLATE, SH_GRID, SH_CONTAINER, SH_ANIMATION, SH_TRANSITION, SH_OUTLINE, SH_OFFSET, SH_GAP_RULE, SH_POSITION_TRY,
       SH_BACKGROUND_POSITION, SH_BORDER_IMAGE, SH_MASK_BORDER, SH_MASK };

struct propdef {
    const char *name;
    uint8_t type, flags; /* flags: LF_* for lengths; which shape or offset value (shape.c) */
    bool inherited;
    uint8_t group; /* G_*: the record that holds the field, off being within it */
    uint16_t off, size;
    const struct kw *kws;
    uint8_t sh;
    uint8_t anim; /* AN_*: the animation type when it is not the value type's (css_prop_animation) */
};
/* Web Animations 1 §5.3 animation types that differ from a value type's default */
enum { AN_TYPE, AN_NONE, AN_DISCRETE, AN_VISIBILITY, AN_DISPLAY };
/* style_t itself, or a group of rarely set properties it holds by pointer (style_t's float_shape,
   offset, gap_rules, anchor, counters, list, background, border_image, mask_border, mask). A group
   holds only properties that are not inherited, but for G_LIST, which holds only inherited ones: a
   style takes its parent's by the pointer. */
enum { G_STYLE, G_FLOAT_SHAPE, G_OFFSET, G_GAP_RULE, G_ANCHOR, G_COUNTER, G_LIST, G_BACKGROUND, G_BORDER_IMAGE,
       G_MASK_BORDER, G_MASK };
/* which sides list a PT_BI_SIDES property holds (its flags) */
enum { BI_WIDTH, BI_OUTSET };
/* the gap decoration shorthands (css-gaps-1): propdef flags hold what one sets (GS_*) and on which
   axes (GS_COLUMN, GS_ROW) */
enum { GS_RULE, GS_COLOR, GS_STYLE, GS_WIDTH, GS_BREAK, GS_ITEMS, GS_INSET_START, GS_INSET_END, GS_INSET_CAP,
       GS_INSET_JUNCTION, GS_INSET };
#define GS_COLUMN 0x10
#define GS_ROW 0x20
#define GS_BOTH (GS_COLUMN | GS_ROW)
/* which Anchor Positioning value a PT_ANCHOR property holds (its flags) */
enum { CSS_ANCHOR_NAME, CSS_ANCHOR_SCOPE, CSS_POSITION_ANCHOR, CSS_POSITION_AREA, CSS_POSITION_TRY_FALLBACKS,
       CSS_POSITION_VISIBILITY };

static const struct kw kw_position[] = {{"static", POS_STATIC}, {"relative", POS_RELATIVE}, {"absolute", POS_ABSOLUTE},
                                        {"fixed", POS_FIXED}, {"sticky", POS_STICKY}, {"-webkit-sticky", POS_STICKY}, {0}};
static const struct kw kw_box_orient[] = {{"horizontal", BO_HORIZONTAL}, {"inline-axis", BO_HORIZONTAL},
                                        {"vertical", BO_VERTICAL}, {"block-axis", BO_VERTICAL}, {0}};
static const struct kw kw_motion_direction[] = {{"normal",CM_NORMAL},{"reverse",CM_REVERSE},
    {"alternate",CM_ALTERNATE},{"alternate-reverse",CM_ALTERNATE_REVERSE},{0}};
static const struct kw kw_motion_fill[] = {{"none",CM_FILL_NONE},{"forwards",CM_FILL_FORWARDS},
    {"backwards",CM_FILL_BACKWARDS},{"both",CM_FILL_BOTH},{0}};
static const struct kw kw_motion_play[] = {{"running",0},{"paused",1},{0}};
static const struct kw kw_motion_composition[] = {{"replace",CM_COMPOSITE_REPLACE},{"add",CM_COMPOSITE_ADD},
    {"accumulate",CM_COMPOSITE_ACCUMULATE},{0}};
static const struct kw kw_transition_behavior[] = {{"normal",0},{"allow-discrete",1},{0}};
static const struct kw kw_float[] = {{"none", FL_NONE}, {"left", FL_LEFT}, {"right", FL_RIGHT},
                                     {"inline-start", FL_LEFT}, {"inline-end", FL_RIGHT}, {0}};
static const struct kw kw_clear[] = {{"none", CL_NONE}, {"left", CL_LEFT}, {"right", CL_RIGHT}, {"both", CL_BOTH},
                                     {"inline-start", CL_LEFT}, {"inline-end", CL_RIGHT}, {0}};
static const struct kw kw_ws[] = {{"normal", WS_NORMAL}, {"pre", WS_PRE}, {"nowrap", WS_NOWRAP},
                                  {"pre-wrap", WS_PRE_WRAP}, {"pre-line", WS_PRE_LINE},
                                  {"break-spaces", WS_PRE_WRAP}, {"-moz-pre-wrap", WS_PRE_WRAP}, {0}};
static const struct kw kw_textwrap[] = {{"wrap", WS_NORMAL}, {"nowrap", WS_NOWRAP}, {"balance", WS_NORMAL},
                                        {"pretty", WS_NORMAL}, {"stable", WS_NORMAL}, {0}};
static const struct kw kw_text_overflow[] = {{"clip",TO_CLIP},{"ellipsis",TO_ELLIPSIS},{0}};
static const struct kw kw_talign[] = {{"left", TA_LEFT}, {"right", TA_RIGHT}, {"center", TA_CENTER},
                                      {"justify", TA_JUSTIFY}, {"start", TA_LEFT}, {"end", TA_RIGHT},
                                      {"-webkit-center", TA_WCENTER}, {"-moz-center", TA_WCENTER},
                                      {"-webkit-left", TA_LEFT}, {"-webkit-right", TA_RIGHT}, {"-moz-left", TA_LEFT},
                                      {"-moz-right", TA_RIGHT}, {"match-parent", TA_LEFT}, {"justify-all", TA_JUSTIFY}, {0}};
static const struct kw kw_lspos[] = {{"outside", 0}, {"inside", 1}, {0}};
static const struct kw kw_fstyle[] = {{"normal", 0}, {"italic", 1}, {"oblique", 1}, {0}};
static const struct kw kw_ttrans[] = {{"none", TT_NONE}, {"uppercase", TT_UPPER}, {"lowercase", TT_LOWER},
                                      {"capitalize", TT_CAPITALIZE}, {"full-width", TT_NONE}, {0}};
static const struct kw kw_overflow[] = {{"visible", OV_VISIBLE}, {"hidden", OV_HIDDEN}, {"scroll", OV_SCROLL},
                                        {"auto", OV_AUTO}, {"clip", OV_CLIP}, {"overlay", OV_AUTO},
                                        {"-moz-hidden-unscrollable", OV_HIDDEN}, {0}};
static const struct kw kw_boxsizing[] = {{"content-box", 0}, {"border-box", 1}, {0}};
static const struct kw kw_object_fit[] = {{"fill", OF_FILL}, {"contain", OF_CONTAIN}, {"cover", OF_COVER},
                                         {"none", OF_NONE}, {"scale-down", OF_SCALE_DOWN}, {0}};
static const struct kw kw_bg_attachment[] = {{"scroll", CSS_BG_SCROLL}, {"fixed", CSS_BG_FIXED}, {"local", CSS_BG_LOCAL}, {0}};
static const struct kw kw_bg_box[] = {{"border-box", CSS_BOX_BORDER}, {"padding-box", CSS_BOX_PADDING},
                                      {"content-box", CSS_BOX_CONTENT}, {0}};
/* <bg-clip> (css-backgrounds-4 §2.1): a <visual-box> or border-area || text, the pair written in this order */
static const struct kw kw_bg_clip[] = {{"border-box", CSS_BOX_BORDER}, {"padding-box", CSS_BOX_PADDING},
                                       {"content-box", CSS_BOX_CONTENT}, {"border-area", CSS_BG_CLIP_AREA},
                                       {"text", CSS_BG_CLIP_TEXT}, {"border-area text", CSS_BG_CLIP_AREA | CSS_BG_CLIP_TEXT}, {0}};
static const struct kw kw_blend[] = {{"normal", CSS_BLEND_NORMAL}, {"multiply", CSS_BLEND_MULTIPLY},
    {"screen", CSS_BLEND_SCREEN}, {"overlay", CSS_BLEND_OVERLAY}, {"darken", CSS_BLEND_DARKEN},
    {"lighten", CSS_BLEND_LIGHTEN}, {"color-dodge", CSS_BLEND_COLOR_DODGE}, {"color-burn", CSS_BLEND_COLOR_BURN},
    {"hard-light", CSS_BLEND_HARD_LIGHT}, {"soft-light", CSS_BLEND_SOFT_LIGHT}, {"difference", CSS_BLEND_DIFFERENCE},
    {"exclusion", CSS_BLEND_EXCLUSION}, {"hue", CSS_BLEND_HUE}, {"saturation", CSS_BLEND_SATURATION},
    {"color", CSS_BLEND_COLOR}, {"luminosity", CSS_BLEND_LUMINOSITY}, {0}};
static const struct kw kw_bi_repeat[] = {{"stretch", CSS_BI_STRETCH}, {"repeat", CSS_BI_REPEAT}, {"round", CSS_BI_ROUND},
                                         {"space", CSS_BI_SPACE}, {0}};
static const struct kw kw_mask_border_mode[] = {{"alpha", CSS_MASK_BORDER_ALPHA}, {"luminance", CSS_MASK_BORDER_LUMINANCE}, {0}};
/* the mask layers' keywords (css-masking-1 §7.2, §7.5, §7.6, §7.8) and mask-type (§9.2) */
static const struct kw kw_mask_mode[] = {{"match-source", CSS_MASK_MATCH_SOURCE}, {"alpha", CSS_MASK_ALPHA},
                                         {"luminance", CSS_MASK_LUMINANCE}, {0}};
/* <coord-box> (mask-origin); mask-clip also no-clip */
static const struct kw kw_mask_box[] = {{"border-box", CSS_BOX_BORDER}, {"padding-box", CSS_BOX_PADDING},
                                        {"content-box", CSS_BOX_CONTENT}, {"fill-box", CSS_BOX_FILL},
                                        {"stroke-box", CSS_BOX_STROKE}, {"view-box", CSS_BOX_VIEW}, {0}};
static const struct kw kw_mask_clip[] = {{"border-box", CSS_BOX_BORDER}, {"padding-box", CSS_BOX_PADDING},
                                         {"content-box", CSS_BOX_CONTENT}, {"fill-box", CSS_BOX_FILL},
                                         {"stroke-box", CSS_BOX_STROKE}, {"view-box", CSS_BOX_VIEW},
                                         {"no-clip", CSS_MASK_NO_CLIP}, {0}};
static const struct kw kw_mask_composite[] = {{"add", CSS_COMPOSITE_ADD}, {"subtract", CSS_COMPOSITE_SUBTRACT},
                                              {"intersect", CSS_COMPOSITE_INTERSECT}, {"exclude", CSS_COMPOSITE_EXCLUDE}, {0}};
static const struct kw kw_mask_type[] = {{"luminance", CSS_MASK_TYPE_LUMINANCE}, {"alpha", CSS_MASK_TYPE_ALPHA}, {0}};
/* one axis of a <repeat-style> (css-backgrounds-3 §3.4) */
static const struct kw kw_repeat_axis[] = {{"repeat", CSS_REPEAT}, {"space", CSS_REPEAT_SPACE}, {"round", CSS_REPEAT_ROUND},
                                           {"no-repeat", CSS_REPEAT_NO}, {0}};
static const struct kw kw_bg_repeat[] = {{"repeat", BR_REPEAT}, {"repeat-x", BR_REPEAT_X}, {"repeat-y", BR_REPEAT_Y},
                                         {"no-repeat", BR_NO_REPEAT}, {"space", BR_REPEAT}, {"round", BR_REPEAT},
                                         {0}};
/* HTML hit testing supports auto/none; SVG painted/geometry keywords remain unsupported. */
static const struct kw kw_pointer_events[] = {{"auto", 0}, {"none", 1}, {0}};
static const struct kw kw_transform_box[] = {{"view-box", TB_VIEW_BOX}, {"fill-box", TB_FILL_BOX}, {"stroke-box", TB_STROKE_BOX},
                                           {"content-box", TB_CONTENT_BOX}, {"border-box", TB_BORDER_BOX}, {0}};
static const struct kw kw_visibility[] = {{"visible", 0}, {"hidden", 1}, {"collapse", 1}, {0}};
static const struct kw kw_content_visibility[] = {{"visible", CV_VISIBLE}, {"auto", CV_AUTO}, {"hidden", CV_HIDDEN}, {0}};
static const struct kw kw_container_type[] = {{"normal", CT_NORMAL}, {"inline-size", CT_INLINE_SIZE}, {"size", CT_SIZE}, {0}};
static const struct kw kw_bcollapse[] = {{"separate", 0}, {"collapse", 1}, {0}};
static const struct kw kw_fdir[] = {{"row", FD_ROW}, {"row-reverse", FD_ROW_REVERSE}, {"column", FD_COLUMN},
                                    {"column-reverse", FD_COLUMN_REVERSE}, {0}};
static const struct kw kw_fwrap[] = {{"nowrap", FW_NOWRAP}, {"wrap", FW_WRAP}, {"wrap-reverse", FW_WRAP_REVERSE}, {0}};
static const struct kw kw_acontent[] = {{"normal", AC_NORMAL}, {"stretch", AC_STRETCH},
    {"flex-start", AC_FLEX_START}, {"flex-end", AC_FLEX_END}, {"center", AC_CENTER},
    {"space-between", AC_BETWEEN}, {"space-around", AC_AROUND}, {"space-evenly", AC_EVENLY},
    {"start", AC_START}, {"end", AC_END}, {0}};
static const struct kw kw_justify[] = {{"normal", JC_START}, {"flex-start", JC_START}, {"start", JC_START},
                                       {"left", JC_START}, {"stretch", JC_START}, {"flex-end", JC_END},
                                       {"end", JC_END}, {"right", JC_END}, {"center", JC_CENTER},
                                       {"space-between", JC_BETWEEN}, {"space-around", JC_AROUND},
                                       {"space-evenly", JC_EVENLY}, {"safe center", JC_CENTER}, {0}};
/* align-items and justify-items; the -self properties also take anchor-center (css-anchor-position-1 §4.2) */
#define KW_ALIGN {"normal", AI_NORMAL}, {"stretch", AI_STRETCH}, {"flex-start", AI_START},                  {"start", AI_START}, {"self-start", AI_START}, {"flex-end", AI_END},                  {"end", AI_END}, {"self-end", AI_END}, {"center", AI_CENTER},                  {"safe center", AI_CENTER}, {"baseline", AI_BASELINE},                  {"first baseline", AI_BASELINE}, {"last baseline", AI_BASELINE}, {"auto", 255}
#define KW_JALIGN {"normal", AI_NORMAL}, {"stretch", AI_STRETCH}, {"legacy", AI_NORMAL},                   {"start", AI_START}, {"self-start", AI_START}, {"flex-start", AI_START},                   {"left", AI_START}, {"end", AI_END}, {"self-end", AI_END},                   {"flex-end", AI_END}, {"right", AI_END}, {"center", AI_CENTER},                   {"safe center", AI_CENTER}, {"legacy center", AI_CENTER},                   {"baseline", AI_START}, {"auto", 255}
static const struct kw kw_align[] = {KW_ALIGN, {0}};
static const struct kw kw_align_self[] = {KW_ALIGN, {"anchor-center", AI_ANCHOR_CENTER}, {0}};
static const struct kw kw_jalign[] = {KW_JALIGN, {0}};
static const struct kw kw_jalign_self[] = {KW_JALIGN, {"anchor-center", AI_ANCHOR_CENTER}, {0}};
static const struct kw kw_gflow[] = {{"row", 0}, {"column", 1}, {"dense", 0}, {"row dense", 0},
                                     {"column dense", 1}, {"dense row", 0}, {"dense column", 1}, {0}};
static const struct kw kw_capside[] = {{"top", 0}, {"bottom", 1}, {"block-start", 0}, {"block-end", 1}, {0}};
static const struct kw kw_tlayout[] = {{"auto", 0}, {"fixed", 1}, {0}};
static const struct kw kw_svg_fill_rule[] = {{"nonzero", 0}, {"evenodd", 1}, {0}};
static const struct kw kw_svg_cap[] = {{"butt", 0}, {"round", 1}, {"square", 2}, {0}};
static const struct kw kw_svg_join[] = {{"miter", 0}, {"round", 1}, {"bevel", 2}, {0}};
static const struct kw kw_bstyle[] = {{"none", BS_NONE}, {"hidden", BS_HIDDEN}, {"solid", BS_SOLID},
                                      {"dashed", BS_DASHED}, {"dotted", BS_DOTTED}, {"double", BS_DOUBLE},
                                      {"groove", BS_GROOVE}, {"ridge", BS_RIDGE}, {"inset", BS_INSET},
                                      {"outset", BS_OUTSET}, {0}};
static const struct kw kw_bwidth[] = {{"thin", 1}, {"medium", 3}, {"thick", 5}, {0}};
static const struct kw kw_ostyle[] = {{"none", BS_NONE}, {"solid", BS_SOLID}, {"auto", BS_SOLID}, {"dashed", BS_DASHED},
                                      {"dotted", BS_DOTTED}, {"double", BS_DOUBLE}, {"groove", BS_GROOVE},
                                      {"ridge", BS_RIDGE}, {"inset", BS_INSET}, {"outset", BS_OUTSET}, {0}};
static const struct kw kw_normal0[] = {{"normal", 0}, {0}};
static const struct kw kw_rule_break[] = {{"none", CSS_RULE_BREAK_NONE}, {"normal", CSS_RULE_BREAK_NORMAL},
                                          {"intersection", CSS_RULE_BREAK_INTERSECTION}, {0}};
static const struct kw kw_rule_items[] = {{"all", CSS_RULE_ITEMS_ALL}, {"around", CSS_RULE_ITEMS_AROUND},
                                          {"between", CSS_RULE_ITEMS_BETWEEN}, {"normal", CSS_RULE_ITEMS_NORMAL}, {0}};
static const struct kw kw_rule_overlap[] = {{"row-over-column", CSS_RULE_ROW_OVER_COLUMN},
                                            {"column-over-row", CSS_RULE_COLUMN_OVER_ROW}, {0}};
static const struct kw kw_try_order[] = {{"normal", CSS_TRY_ORDER_NORMAL}, {"most-width", CSS_TRY_ORDER_MOST_WIDTH},
    {"most-height", CSS_TRY_ORDER_MOST_HEIGHT}, {"most-block-size", CSS_TRY_ORDER_MOST_BLOCK},
    {"most-inline-size", CSS_TRY_ORDER_MOST_INLINE}, {0}};
static const struct kw kw_valign[] = {{"baseline", VA_BASELINE}, {"sub", VA_SUB}, {"super", VA_SUPER},
                                      {"top", VA_TOP}, {"middle", VA_MIDDLE}, {"bottom", VA_BOTTOM},
                                      {"text-top", VA_TEXT_TOP}, {"text-bottom", VA_TEXT_BOTTOM}, {0}};

#define O(f) G_STYLE, (uint16_t)offsetof(style_t, f), (uint16_t)sizeof(((style_t *)0)->f)
#define OG(g, T, f) g, (uint16_t)offsetof(T, f), (uint16_t)sizeof(((T *)0)->f) /* a field of group g, a T */
#define SH(name, id) {name, PT_SHORT, 0, false, G_STYLE, 0, 0, NULL, id, AN_NONE}
#define SH_GAP(name, flags) {name, PT_SHORT, flags, false, G_STYLE, 0, 0, NULL, SH_GAP_RULE, AN_NONE}
#define OGAP(f) OG(G_GAP_RULE, struct css_gap_rules, f)
#define OCOUNTER(i) G_COUNTER, (uint16_t)(offsetof(struct css_counters, ops) + (i) * sizeof(void *)), (uint16_t)sizeof(void *)
#define OBG(f) OG(G_BACKGROUND, struct css_background, f)
#define OBI(f) OG(G_BORDER_IMAGE, struct css_border_image, f)
#define OMB(f) OG(G_MASK_BORDER, struct css_border_image, f)
#define OMK(f) OG(G_MASK, struct css_mask, f)

/* sorted by name at first use */
static struct propdef props[] = {
    {"-webkit-box-orient", PT_KW, 0, false, O(box_orient), kw_box_orient, 0, AN_TYPE},
    {"-webkit-line-clamp", PT_LINE_CLAMP, 0, false, O(line_clamp), NULL, 0, AN_TYPE},
    SH("animation", SH_ANIMATION),
    /* flags: which ML_* list a motion longhand is */
    {"animation-name",PT_MOTION_LIST,ML_NAME,false,O(motion.list[ML_NAME]),NULL,0,AN_NONE},
    {"animation-duration",PT_MOTION_LIST,ML_DURATION,false,O(motion.list[ML_DURATION]),NULL,0,AN_NONE},
    {"animation-delay",PT_MOTION_LIST,ML_DELAY,false,O(motion.list[ML_DELAY]),NULL,0,AN_NONE},
    {"animation-timing-function",PT_MOTION_LIST,ML_EASING,false,O(motion.list[ML_EASING]),NULL,0,AN_NONE},
    {"animation-iteration-count",PT_MOTION_LIST,ML_ITERATIONS,false,O(motion.list[ML_ITERATIONS]),NULL,0,AN_NONE},
    {"animation-direction",PT_MOTION_LIST,ML_DIRECTION,false,O(motion.list[ML_DIRECTION]),kw_motion_direction,0,AN_NONE},
    {"animation-fill-mode",PT_MOTION_LIST,ML_FILL,false,O(motion.list[ML_FILL]),kw_motion_fill,0,AN_NONE},
    {"animation-play-state",PT_MOTION_LIST,ML_PLAY,false,O(motion.list[ML_PLAY]),kw_motion_play,0,AN_NONE},
    {"animation-composition",PT_MOTION_LIST,ML_COMPOSITION,false,O(motion.list[ML_COMPOSITION]),kw_motion_composition,0,AN_NONE},
    SH("transition", SH_TRANSITION),
    {"transition-property",PT_MOTION_LIST,ML_TPROPERTY,false,O(motion.list[ML_TPROPERTY]),NULL,0,AN_NONE},
    {"transition-duration",PT_MOTION_LIST,ML_TDURATION,false,O(motion.list[ML_TDURATION]),NULL,0,AN_NONE},
    {"transition-delay",PT_MOTION_LIST,ML_TDELAY,false,O(motion.list[ML_TDELAY]),NULL,0,AN_NONE},
    {"transition-timing-function",PT_MOTION_LIST,ML_TEASING,false,O(motion.list[ML_TEASING]),NULL,0,AN_NONE},
    {"transition-behavior",PT_MOTION_LIST,ML_TBEHAVIOR,false,O(motion.list[ML_TBEHAVIOR]),kw_transition_behavior,0,AN_NONE},
    {"align-content", PT_KW, 0, false, O(align_content), kw_acontent, 0, AN_TYPE},
    {"align-items", PT_KW, 0, false, O(align_items), kw_align, 0, AN_TYPE},
    {"align-self", PT_KW, 0, false, O(align_self), kw_align_self, 0, AN_TYPE},
    SH("background", SH_BACKGROUND),
    {"background-color", PT_COLOR, 0, false, O(bg_color), NULL, 0, AN_TYPE},
    {"background-image", PT_BG_IMAGE, 0, false, O(bg_image), NULL, 0, AN_TYPE},
    {"box-shadow", PT_BOX_SHADOW, 0, false, O(box_shadow), NULL, 0, AN_TYPE},
    {"background-repeat", PT_KW, 0, false, O(bg_repeat), kw_bg_repeat, 0, AN_TYPE},
    {"background-size", PT_BG_SIZE, 0, false, O(bg_size), NULL, 0, AN_TYPE},
    /* the layered lists (flags: enum css_bg_list; the position's axis) and background-position, their shorthand */
    {"background-attachment", PT_BG_LIST, CSS_BG_ATTACHMENT, false, OBG(list[CSS_BG_ATTACHMENT]), kw_bg_attachment, 0, AN_DISCRETE},
    {"background-clip", PT_BG_LIST, CSS_BG_CLIP, false, OBG(list[CSS_BG_CLIP]), kw_bg_clip, 0, AN_DISCRETE},
    {"background-origin", PT_BG_LIST, CSS_BG_ORIGIN, false, OBG(list[CSS_BG_ORIGIN]), kw_bg_box, 0, AN_DISCRETE},
    {"background-blend-mode", PT_BG_LIST, CSS_BG_BLEND, false, OBG(list[CSS_BG_BLEND]), kw_blend, 0, AN_DISCRETE},
    SH("background-position", SH_BACKGROUND_POSITION),
    {"background-position-x", PT_BG_OFFSET, 0, false, OBG(position[0]), NULL, 0, AN_TYPE},
    {"background-position-y", PT_BG_OFFSET, 1, false, OBG(position[1]), NULL, 0, AN_TYPE},
    /* border-image and mask-border: one record type, two groups */
    SH("border-image", SH_BORDER_IMAGE),
    {"border-image-source", PT_BI_SOURCE, 0, false, OBI(source), NULL, 0, AN_DISCRETE},
    {"border-image-slice", PT_BI_SLICE, 0, false, OBI(slice), NULL, 0, AN_TYPE},
    {"border-image-width", PT_BI_SIDES, BI_WIDTH, false, OBI(width), NULL, 0, AN_TYPE},
    {"border-image-outset", PT_BI_SIDES, BI_OUTSET, false, OBI(outset), NULL, 0, AN_TYPE},
    {"border-image-repeat", PT_BI_REPEAT, 0, false, OBI(repeat), kw_bi_repeat, 0, AN_DISCRETE},
    SH("mask-border", SH_MASK_BORDER),
    {"mask-border-source", PT_BI_SOURCE, 0, false, OMB(source), NULL, 0, AN_DISCRETE},
    {"mask-border-slice", PT_BI_SLICE, 0, false, OMB(slice), NULL, 0, AN_TYPE},
    {"mask-border-width", PT_BI_SIDES, BI_WIDTH, false, OMB(width), NULL, 0, AN_TYPE},
    {"mask-border-outset", PT_BI_SIDES, BI_OUTSET, false, OMB(outset), NULL, 0, AN_TYPE},
    {"mask-border-repeat", PT_BI_REPEAT, 0, false, OMB(repeat), kw_bi_repeat, 0, AN_DISCRETE},
    {"mask-border-mode", PT_KW, 0, false, OMB(mode), kw_mask_border_mode, 0, AN_DISCRETE},
    SH("border", SH_BORDER),
    SH("border-block", SH_BORDER_BLOCK),
    SH("border-bottom", SH_BORDER_BOTTOM),
    {"border-bottom-left-radius", PT_RADIUS, 0, false, O(corner_radius[3]), NULL, 0, AN_TYPE},
    {"border-bottom-right-radius", PT_RADIUS, 0, false, O(corner_radius[2]), NULL, 0, AN_TYPE},
    {"border-bottom-color", PT_COLOR, 0, false, O(border_color[2]), NULL, 0, AN_TYPE},
    {"border-bottom-style", PT_KW, 0, false, O(border_style[2]), kw_bstyle, 0, AN_TYPE},
    {"border-bottom-width", PT_PX, LF_NOPCT, false, O(border_width[2]), kw_bwidth, 0, AN_TYPE},
    {"border-collapse", PT_KW, 0, true, O(border_collapse), kw_bcollapse, 0, AN_TYPE},
    SH("border-color", SH_BORDER_COLOR),
    SH("border-inline", SH_BORDER_INLINE),
    SH("border-left", SH_BORDER_LEFT),
    {"border-left-color", PT_COLOR, 0, false, O(border_color[3]), NULL, 0, AN_TYPE},
    {"border-left-style", PT_KW, 0, false, O(border_style[3]), kw_bstyle, 0, AN_TYPE},
    {"border-left-width", PT_PX, LF_NOPCT, false, O(border_width[3]), kw_bwidth, 0, AN_TYPE},
    SH("border-radius", SH_BORDER_RADIUS),
    SH("border-right", SH_BORDER_RIGHT),
    {"border-right-color", PT_COLOR, 0, false, O(border_color[1]), NULL, 0, AN_TYPE},
    {"border-right-style", PT_KW, 0, false, O(border_style[1]), kw_bstyle, 0, AN_TYPE},
    {"border-right-width", PT_PX, LF_NOPCT, false, O(border_width[1]), kw_bwidth, 0, AN_TYPE},
    {"border-spacing", PT_PX, LF_NOPCT, true, O(border_spacing), NULL, 0, AN_TYPE},
    SH("border-style", SH_BORDER_STYLE),
    SH("border-top", SH_BORDER_TOP),
    {"border-top-left-radius", PT_RADIUS, 0, false, O(corner_radius[0]), NULL, 0, AN_TYPE},
    {"border-top-right-radius", PT_RADIUS, 0, false, O(corner_radius[1]), NULL, 0, AN_TYPE},
    {"border-top-color", PT_COLOR, 0, false, O(border_color[0]), NULL, 0, AN_TYPE},
    {"border-top-style", PT_KW, 0, false, O(border_style[0]), kw_bstyle, 0, AN_TYPE},
    {"border-top-width", PT_PX, LF_NOPCT, false, O(border_width[0]), kw_bwidth, 0, AN_TYPE},
    SH("border-width", SH_BORDER_WIDTH),
    {"bottom", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR | LF_ANCHOR_SIZE, false, O(inset[2]), NULL, 0, AN_TYPE},
    {"box-sizing", PT_KW, 0, false, O(box_sizing), kw_boxsizing, 0, AN_TYPE},
    {"caption-side", PT_KW, 0, true, O(caption_bottom), kw_capside, 0, AN_TYPE},
    {"clear", PT_KW, 0, false, O(clear), kw_clear, 0, AN_TYPE},
    {"color", PT_COLOR, 0, true, O(color), NULL, 0, AN_TYPE},
    {"color-scheme", PT_COLOR_SCHEME, 0, true, O(color_scheme), NULL, 0, AN_DISCRETE},
    {"column-gap", PT_PX, LF_NOPCT, false, O(gap_col), kw_normal0, 0, AN_TYPE},
    /* CSS Gap Decorations 1. flags: a list's enum css_gap_rule_kind, a shorthand's GS_* */
    SH_GAP("column-rule", GS_RULE | GS_COLUMN),
    SH_GAP("row-rule", GS_RULE | GS_ROW),
    SH_GAP("rule", GS_RULE | GS_BOTH),
    {"column-rule-color", PT_RULE_LIST, CSS_GAP_COLOR, false, OGAP(axis[0].list[CSS_GAP_COLOR]), NULL, 0, AN_TYPE},
    {"column-rule-style", PT_RULE_LIST, CSS_GAP_STYLE, false, OGAP(axis[0].list[CSS_GAP_STYLE]), NULL, 0, AN_DISCRETE},
    {"column-rule-width", PT_RULE_LIST, CSS_GAP_WIDTH, false, OGAP(axis[0].list[CSS_GAP_WIDTH]), NULL, 0, AN_TYPE},
    {"row-rule-color", PT_RULE_LIST, CSS_GAP_COLOR, false, OGAP(axis[1].list[CSS_GAP_COLOR]), NULL, 0, AN_TYPE},
    {"row-rule-style", PT_RULE_LIST, CSS_GAP_STYLE, false, OGAP(axis[1].list[CSS_GAP_STYLE]), NULL, 0, AN_DISCRETE},
    {"row-rule-width", PT_RULE_LIST, CSS_GAP_WIDTH, false, OGAP(axis[1].list[CSS_GAP_WIDTH]), NULL, 0, AN_TYPE},
    SH_GAP("rule-color", GS_COLOR | GS_BOTH),
    SH_GAP("rule-style", GS_STYLE | GS_BOTH),
    SH_GAP("rule-width", GS_WIDTH | GS_BOTH),
    {"column-rule-break", PT_KW, 0, false, OGAP(axis[0].rule_break), kw_rule_break, 0, AN_TYPE},
    {"row-rule-break", PT_KW, 0, false, OGAP(axis[1].rule_break), kw_rule_break, 0, AN_TYPE},
    SH_GAP("rule-break", GS_BREAK | GS_BOTH),
    {"column-rule-visibility-items", PT_KW, 0, false, OGAP(axis[0].visibility), kw_rule_items, 0, AN_TYPE},
    {"row-rule-visibility-items", PT_KW, 0, false, OGAP(axis[1].visibility), kw_rule_items, 0, AN_TYPE},
    SH_GAP("rule-visibility-items", GS_ITEMS | GS_BOTH),
    {"rule-overlap", PT_KW, 0, false, OGAP(overlap), kw_rule_overlap, 0, AN_TYPE},
    {"column-rule-inset-cap-start", PT_RULE_INSET, 0, false, OGAP(axis[0].inset[CSS_RULE_CAP_START]), NULL, 0, AN_TYPE},
    {"column-rule-inset-cap-end", PT_RULE_INSET, 0, false, OGAP(axis[0].inset[CSS_RULE_CAP_END]), NULL, 0, AN_TYPE},
    {"column-rule-inset-junction-start", PT_RULE_INSET, 0, false, OGAP(axis[0].inset[CSS_RULE_JUNCTION_START]), NULL, 0,
     AN_TYPE},
    {"column-rule-inset-junction-end", PT_RULE_INSET, 0, false, OGAP(axis[0].inset[CSS_RULE_JUNCTION_END]), NULL, 0, AN_TYPE},
    {"row-rule-inset-cap-start", PT_RULE_INSET, 0, false, OGAP(axis[1].inset[CSS_RULE_CAP_START]), NULL, 0, AN_TYPE},
    {"row-rule-inset-cap-end", PT_RULE_INSET, 0, false, OGAP(axis[1].inset[CSS_RULE_CAP_END]), NULL, 0, AN_TYPE},
    {"row-rule-inset-junction-start", PT_RULE_INSET, 0, false, OGAP(axis[1].inset[CSS_RULE_JUNCTION_START]), NULL, 0, AN_TYPE},
    {"row-rule-inset-junction-end", PT_RULE_INSET, 0, false, OGAP(axis[1].inset[CSS_RULE_JUNCTION_END]), NULL, 0, AN_TYPE},
    SH_GAP("column-rule-inset-start", GS_INSET_START | GS_COLUMN),
    SH_GAP("column-rule-inset-end", GS_INSET_END | GS_COLUMN),
    SH_GAP("row-rule-inset-start", GS_INSET_START | GS_ROW),
    SH_GAP("row-rule-inset-end", GS_INSET_END | GS_ROW),
    SH_GAP("rule-inset-start", GS_INSET_START | GS_BOTH),
    SH_GAP("rule-inset-end", GS_INSET_END | GS_BOTH),
    SH_GAP("column-rule-inset-cap", GS_INSET_CAP | GS_COLUMN),
    SH_GAP("column-rule-inset-junction", GS_INSET_JUNCTION | GS_COLUMN),
    SH_GAP("row-rule-inset-cap", GS_INSET_CAP | GS_ROW),
    SH_GAP("row-rule-inset-junction", GS_INSET_JUNCTION | GS_ROW),
    SH_GAP("rule-inset-cap", GS_INSET_CAP | GS_BOTH),
    SH_GAP("rule-inset-junction", GS_INSET_JUNCTION | GS_BOTH),
    SH_GAP("column-rule-inset", GS_INSET | GS_COLUMN),
    SH_GAP("row-rule-inset", GS_INSET | GS_ROW),
    SH_GAP("rule-inset", GS_INSET | GS_BOTH),
    {"content", PT_CONTENT, 0, false, O(content), NULL, 0, AN_TYPE},
    {"counter-increment", PT_COUNTER, CSS_COUNTER_INCREMENT, false, OCOUNTER(CSS_COUNTER_INCREMENT), NULL, 0, AN_DISCRETE},
    {"counter-reset", PT_COUNTER, CSS_COUNTER_RESET, false, OCOUNTER(CSS_COUNTER_RESET), NULL, 0, AN_DISCRETE},
    {"counter-set", PT_COUNTER, CSS_COUNTER_SET, false, OCOUNTER(CSS_COUNTER_SET), NULL, 0, AN_DISCRETE},
    {"content-visibility", PT_KW, 0, false, O(content_visibility), kw_content_visibility, 0, AN_DISPLAY},
    SH("container", SH_CONTAINER),
    {"container-name", PT_CONTAINER_NAMES, 0, false, O(container_names), NULL, 0, AN_TYPE},
    {"container-type", PT_KW, 0, false, O(container_type), kw_container_type, 0, AN_NONE},
    {"display", PT_DISPLAY, 0, false, O(display), NULL, 0, AN_TYPE},
    SH("flex", SH_FLEX),
    {"flex-basis", PT_LEN, LF_AUTO | LF_LENGTH, false, O(flex_basis), NULL, 0, AN_TYPE},
    {"flex-direction", PT_KW, 0, false, O(flex_direction), kw_fdir, 0, AN_TYPE},
    SH("flex-flow", SH_FLEX_FLOW),
    {"flex-grow", PT_NUM, 0, false, O(flex_grow), NULL, 0, AN_TYPE},
    {"flex-shrink", PT_NUM, 0, false, O(flex_shrink), NULL, 0, AN_TYPE},
    {"flex-wrap", PT_KW, 0, false, O(flex_wrap), kw_fwrap, 0, AN_TYPE},
    {"float", PT_KW, 0, false, O(float_), kw_float, 0, AN_TYPE},
    {"fill", PT_SVG_PAINT, 0, true, O(svg_fill), NULL, 0, AN_TYPE},
    {"fill-opacity", PT_OPACITY, 0, true, O(svg_fill_opacity), NULL, 0, AN_TYPE},
    {"filter", PT_FILTER, 0, false, O(filter), NULL, 0, AN_TYPE},
    {"clip", PT_CLIP, 0, false, O(clip), NULL, 0, AN_TYPE},
    {"clip-path", PT_SHAPE, CSS_SHAPE_CLIP_PATH, false, O(clip_path), NULL, 0, AN_TYPE},
    {"clip-rule", PT_KW, 0, true, O(svg_clip_rule), kw_svg_fill_rule, 0, AN_DISCRETE},
    {"shape-outside", PT_SHAPE, CSS_SHAPE_OUTSIDE, false, OG(G_FLOAT_SHAPE, struct css_float_shape, outside), NULL, 0, AN_TYPE},
    {"shape-margin", PT_LEN, LF_LENGTH, false, OG(G_FLOAT_SHAPE, struct css_float_shape, margin), NULL, 0, AN_TYPE},
    {"shape-image-threshold", PT_OPACITY, 0, false, OG(G_FLOAT_SHAPE, struct css_float_shape, image_threshold), NULL, 0,
     AN_TYPE},
    SH("offset", SH_OFFSET),
    {"offset-path", PT_SHAPE, CSS_SHAPE_OFFSET_PATH, false, OG(G_OFFSET, struct css_offset, path), NULL, 0, AN_TYPE},
    {"offset-distance", PT_LEN, LF_NEG | LF_LENGTH, false, OG(G_OFFSET, struct css_offset, distance), NULL, 0, AN_TYPE},
    {"offset-position", PT_OFFSET, CSS_OFFSET_POSITION, false, OG(G_OFFSET, struct css_offset, position), NULL, 0, AN_TYPE},
    {"offset-anchor", PT_OFFSET, CSS_OFFSET_ANCHOR, false, OG(G_OFFSET, struct css_offset, anchor), NULL, 0, AN_TYPE},
    {"offset-rotate", PT_OFFSET, CSS_OFFSET_ROTATE, false, OG(G_OFFSET, struct css_offset, rotate), NULL, 0, AN_TYPE},
    {"anchor-name", PT_ANCHOR, CSS_ANCHOR_NAME, false, OG(G_ANCHOR, struct css_anchor, name), NULL, 0, AN_DISCRETE},
    {"anchor-scope", PT_ANCHOR, CSS_ANCHOR_SCOPE, false, OG(G_ANCHOR, struct css_anchor, scope), NULL, 0, AN_DISCRETE},
    {"position-anchor", PT_ANCHOR, CSS_POSITION_ANCHOR, false, OG(G_ANCHOR, struct css_anchor, position_anchor), NULL, 0,
     AN_DISCRETE},
    {"position-area", PT_ANCHOR, CSS_POSITION_AREA, false, OG(G_ANCHOR, struct css_anchor, area), NULL, 0, AN_DISCRETE},
    SH("position-try", SH_POSITION_TRY),
    {"position-try-fallbacks", PT_ANCHOR, CSS_POSITION_TRY_FALLBACKS, false, OG(G_ANCHOR, struct css_anchor, tries), NULL,
     0, AN_DISCRETE},
    {"position-try-order", PT_KW, 0, false, OG(G_ANCHOR, struct css_anchor, try_order), kw_try_order, 0, AN_DISCRETE},
    {"position-visibility", PT_ANCHOR, CSS_POSITION_VISIBILITY, false, OG(G_ANCHOR, struct css_anchor, visibility), NULL,
     0, AN_DISCRETE},
    {"fill-rule", PT_KW, 0, true, O(svg_fill_rule), kw_svg_fill_rule, 0, AN_TYPE},
    SH("font", SH_FONT),
    SH("all", SH_ALL),
    {"font-family", PT_FONT_FAMILY, 0, true, O(font_family), NULL, 0, AN_TYPE},
    {"font-size", PT_FONT_SIZE, 0, true, O(font_size), NULL, 0, AN_TYPE},
    {"font-style", PT_KW, 0, true, O(font_style), kw_fstyle, 0, AN_TYPE},
    {"font-weight", PT_FONT_WEIGHT, 0, true, O(font_weight), NULL, 0, AN_TYPE},
    SH("gap", SH_GAP),
    SH("grid", SH_GRID),
    SH("grid-area", SH_GRID_AREA),
    {"grid-auto-columns", PT_GTEMPLATE, 0, false, O(grid_auto_cols), NULL, 0, AN_TYPE},
    {"grid-auto-flow", PT_KW, 0, false, O(grid_flow_col), kw_gflow, 0, AN_TYPE},
    {"grid-auto-rows", PT_GTEMPLATE, 0, false, O(grid_auto_rows), NULL, 0, AN_TYPE},
    SH("grid-column", SH_GRID_COLUMN),
    {"grid-column-end", PT_GLINE, 0, false, O(grid_place[3]), NULL, 0, AN_TYPE},
    {"grid-column-start", PT_GLINE, 0, false, O(grid_place[1]), NULL, 0, AN_TYPE},
    SH("grid-row", SH_GRID_ROW),
    {"grid-row-end", PT_GLINE, 0, false, O(grid_place[2]), NULL, 0, AN_TYPE},
    {"grid-row-start", PT_GLINE, 0, false, O(grid_place[0]), NULL, 0, AN_TYPE},
    SH("grid-template", SH_GRID_TEMPLATE),
    {"grid-template-areas", PT_GAREAS, 0, false, O(grid_areas), NULL, 0, AN_TYPE},
    {"grid-template-columns", PT_GTEMPLATE, 0, false, O(grid_cols), NULL, 0, AN_TYPE},
    {"grid-template-rows", PT_GTEMPLATE, 0, false, O(grid_rows), NULL, 0, AN_TYPE},
    {"height", PT_LEN, LF_AUTO | LF_ANCHOR_SIZE, false, O(height), NULL, 0, AN_TYPE},
    SH("inset", SH_INSET),
    SH("inset-block", SH_INSET_BLOCK),
    SH("inset-inline", SH_INSET_INLINE),
    {"justify-content", PT_KW, 0, false, O(justify_content), kw_justify, 0, AN_TYPE},
    {"justify-items", PT_KW, 0, false, O(justify_items), kw_jalign, 0, AN_TYPE},
    {"justify-self", PT_KW, 0, false, O(justify_self), kw_jalign_self, 0, AN_TYPE},
    {"left", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR | LF_ANCHOR_SIZE, false, O(inset[3]), NULL, 0, AN_TYPE},
    {"letter-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(letter_spacing), kw_normal0, 0, AN_TYPE},
    {"line-height", PT_LINE_HEIGHT, 0, true, O(line_height), NULL, 0, AN_TYPE},
    SH("list-style", SH_LIST_STYLE),
    {"list-style-image", PT_LIST_IMAGE, 0, true, OG(G_LIST, struct css_list, image), NULL, 0, AN_DISCRETE},
    {"list-style-position", PT_KW, 0, true, O(list_style_inside), kw_lspos, 0, AN_TYPE},
    {"list-style-type", PT_LIST_TYPE, 0, true, O(list_style), NULL, 0, AN_TYPE},
    {"quotes", PT_QUOTES, 0, true, OG(G_LIST, struct css_list, quotes), NULL, 0, AN_DISCRETE},
    SH("margin", SH_MARGIN),
    SH("margin-block", SH_MARGIN_BLOCK),
    {"margin-bottom", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR_SIZE, false, O(margin[2]), NULL, 0, AN_TYPE},
    SH("margin-inline", SH_MARGIN_INLINE),
    {"margin-left", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR_SIZE, false, O(margin[3]), NULL, 0, AN_TYPE},
    {"margin-right", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR_SIZE, false, O(margin[1]), NULL, 0, AN_TYPE},
    {"margin-top", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR_SIZE, false, O(margin[0]), NULL, 0, AN_TYPE},
    /* the mask layers (css-masking-1 §7; flags: enum css_mask_list) and their shorthand */
    SH("mask", SH_MASK),
    {"mask-image", PT_MASK_IMAGE, 0, false, OMK(image), NULL, 0, AN_DISCRETE},
    {"mask-mode", PT_BG_LIST, CSS_MASK_MODE, false, OMK(list[CSS_MASK_MODE]), kw_mask_mode, 0, AN_DISCRETE},
    {"mask-repeat", PT_MASK_REPEAT, CSS_MASK_REPEAT, false, OMK(list[CSS_MASK_REPEAT]), kw_repeat_axis, 0, AN_DISCRETE},
    {"mask-position", PT_MASK_POSITION, 0, false, OMK(position), NULL, 0, AN_TYPE},
    {"mask-clip", PT_BG_LIST, CSS_MASK_CLIP, false, OMK(list[CSS_MASK_CLIP]), kw_mask_clip, 0, AN_DISCRETE},
    {"mask-origin", PT_BG_LIST, CSS_MASK_ORIGIN, false, OMK(list[CSS_MASK_ORIGIN]), kw_mask_box, 0, AN_DISCRETE},
    {"mask-size", PT_MASK_SIZE, 0, false, OMK(size), NULL, 0, AN_TYPE},
    {"mask-composite", PT_BG_LIST, CSS_MASK_COMPOSITE, false, OMK(list[CSS_MASK_COMPOSITE]), kw_mask_composite, 0, AN_DISCRETE},
    {"mask-type", PT_KW, 0, false, OMK(type), kw_mask_type, 0, AN_DISCRETE},
    {"object-fit", PT_KW, 0, false, O(object_fit), kw_object_fit, 0, AN_TYPE},
    {"object-position", PT_BG_POS, 0, true, O(object_pos), NULL, 0, AN_TYPE},
    {"max-height", PT_LEN, LF_NONE | LF_ANCHOR_SIZE, false, O(max_height), NULL, 0, AN_TYPE},
    {"max-width", PT_LEN, LF_NONE | LF_ANCHOR_SIZE, false, O(max_width), NULL, 0, AN_TYPE},
    {"min-height", PT_LEN, LF_AUTO | LF_ANCHOR_SIZE, false, O(min_height), NULL, 0, AN_TYPE},
    {"min-width", PT_LEN, LF_AUTO | LF_ANCHOR_SIZE, false, O(min_width), NULL, 0, AN_TYPE},
    {"opacity", PT_OPACITY, 0, false, O(opacity), NULL, 0, AN_TYPE},
    {"order", PT_INT, 0, false, O(order), NULL, 0, AN_TYPE},
    SH("outline", SH_OUTLINE),
    {"outline-color", PT_COLOR, 0, false, O(outline_color), NULL, 0, AN_TYPE},
    {"outline-offset", PT_PX, LF_NEG | LF_NOPCT, false, O(outline_offset), NULL, 0, AN_TYPE},
    {"outline-style", PT_KW, 0, false, O(outline_style), kw_ostyle, 0, AN_TYPE},
    {"outline-width", PT_PX, LF_NOPCT, false, O(outline_width), kw_bwidth, 0, AN_TYPE},
    SH("overflow", SH_OVERFLOW),
    {"overflow-x", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0, AN_TYPE},
    {"overflow-y", PT_OVERFLOW, 0, false, O(overflow), kw_overflow, 0, AN_TYPE},
    SH("padding", SH_PADDING),
    SH("padding-block", SH_PADDING_BLOCK),
    {"padding-bottom", PT_LEN, 0, false, O(padding[2]), NULL, 0, AN_TYPE},
    SH("padding-inline", SH_PADDING_INLINE),
    {"padding-left", PT_LEN, 0, false, O(padding[3]), NULL, 0, AN_TYPE},
    {"padding-right", PT_LEN, 0, false, O(padding[1]), NULL, 0, AN_TYPE},
    {"padding-top", PT_LEN, 0, false, O(padding[0]), NULL, 0, AN_TYPE},
    SH("place-content", SH_PLACE_CONTENT),
    SH("place-items", SH_PLACE_ITEMS),
    SH("place-self", SH_PLACE_SELF),
    {"pointer-events", PT_KW, 0, true, O(pointer_events), kw_pointer_events, 0, AN_TYPE},
    {"position", PT_KW, 0, false, O(position), kw_position, 0, AN_TYPE},
    {"right", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR | LF_ANCHOR_SIZE, false, O(inset[1]), NULL, 0, AN_TYPE},
    {"row-gap", PT_PX, LF_NOPCT, false, O(gap_row), kw_normal0, 0, AN_TYPE},
    {"stroke", PT_SVG_PAINT, 0, true, O(svg_stroke), NULL, 0, AN_TYPE},
    {"stroke-opacity", PT_OPACITY, 0, true, O(svg_stroke_opacity), NULL, 0, AN_TYPE},
    {"stroke-width", PT_SVG_WIDTH, 0, true, O(svg_stroke_width), NULL, 0, AN_TYPE},
    {"stroke-linecap", PT_KW, 0, true, O(svg_stroke_cap), kw_svg_cap, 0, AN_TYPE},
    {"stroke-linejoin", PT_KW, 0, true, O(svg_stroke_join), kw_svg_join, 0, AN_TYPE},
    {"stop-color", PT_COLOR, 0, false, O(svg_stop_color), NULL, 0, AN_TYPE},
    {"stop-opacity", PT_OPACITY, 0, false, O(svg_stop_opacity), NULL, 0, AN_TYPE},
    {"table-layout", PT_KW, 0, false, O(table_layout), kw_tlayout, 0, AN_TYPE},
    {"text-align", PT_KW, 0, true, O(text_align), kw_talign, 0, AN_TYPE},
    {"text-overflow",PT_KW,0,false,O(text_overflow),kw_text_overflow,0, AN_TYPE},
    SH("text-decoration", SH_TEXT_DECORATION),
    {"text-decoration-line", PT_TEXT_DECO, 0, true, O(text_decoration), NULL, 0, AN_TYPE},
    {"text-indent", PT_LEN, LF_NEG, true, O(text_indent), NULL, 0, AN_TYPE},
    {"text-transform", PT_KW, 0, true, O(text_transform), kw_ttrans, 0, AN_TYPE},
    {"text-wrap-mode", PT_KW, 0, true, O(white_space), kw_textwrap, 0, AN_TYPE},
    {"top", PT_LEN, LF_AUTO | LF_NEG | LF_ANCHOR | LF_ANCHOR_SIZE, false, O(inset[0]), NULL, 0, AN_TYPE},
    {"transform", PT_TRANSFORM, 0, false, O(transform), NULL, 0, AN_TYPE},
    {"transform-origin", PT_TRANSFORM_ORIGIN, 0, false, O(transform_origin), NULL, 0, AN_TYPE},
    {"transform-box", PT_KW, 0, false, O(transform_box), kw_transform_box, 0, AN_TYPE},
    {"rotate", PT_ROTATE, 0, false, O(rotate), NULL, 0, AN_TYPE},
    {"scale", PT_SCALE, 0, false, O(scale), NULL, 0, AN_TYPE},
    {"translate", PT_TRANSLATE, 0, false, O(translate), NULL, 0, AN_TYPE},
    {"vertical-align", PT_VALIGN, 0, false, O(vertical_align), kw_valign, 0, AN_TYPE},
    {"visibility", PT_KW, 0, true, O(visibility), kw_visibility, 0, AN_VISIBILITY},
    {"white-space", PT_KW, 0, true, O(white_space), kw_ws, 0, AN_TYPE},
    {"width", PT_LEN, LF_AUTO | LF_ANCHOR_SIZE, false, O(width), NULL, 0, AN_TYPE},
    {"word-spacing", PT_PX, LF_NEG | LF_NOPCT, true, O(word_spacing), kw_normal0, 0, AN_TYPE},
    {"z-index", PT_ZINDEX, 0, false, O(z_index), NULL, 0, AN_TYPE},
};
#define NPROPS (int)(sizeof props / sizeof *props)

/* other names for the same properties (logical properties map to left-to-right, horizontal text) */
static const struct {
    const char *alias, *name;
} aliases[] = {
    {"-webkit-transform", "transform"},
    {"block-size", "height"}, {"border-block-end", "border-bottom"}, {"border-block-start", "border-top"},
    {"border-block-end-color", "border-bottom-color"}, {"border-block-start-color", "border-top-color"},
    {"border-block-end-width", "border-bottom-width"}, {"border-block-start-width", "border-top-width"},
    {"border-block-end-style", "border-bottom-style"}, {"border-block-start-style", "border-top-style"},
    {"border-inline-end", "border-right"}, {"border-inline-start", "border-left"},
    {"border-inline-end-color", "border-right-color"}, {"border-inline-start-color", "border-left-color"},
    {"border-inline-end-width", "border-right-width"}, {"border-inline-start-width", "border-left-width"},
    {"border-inline-end-style", "border-right-style"}, {"border-inline-start-style", "border-left-style"},
    {"grid-column-gap", "column-gap"}, {"grid-gap", "gap"}, {"grid-row-gap", "row-gap"},
    {"inline-size", "width"}, {"inset-block-end", "bottom"}, {"inset-block-start", "top"},
    {"inset-inline-end", "right"}, {"inset-inline-start", "left"}, {"margin-block-end", "margin-bottom"},
    {"margin-block-start", "margin-top"}, {"margin-inline-end", "margin-right"},
    {"margin-inline-start", "margin-left"}, {"max-block-size", "max-height"}, {"max-inline-size", "max-width"},
    {"min-block-size", "min-height"}, {"min-inline-size", "min-width"}, {"padding-block-end", "padding-bottom"},
    {"padding-block-start", "padding-top"}, {"padding-inline-end", "padding-right"},
    {"padding-inline-start", "padding-left"}, {"word-wrap", ""},
};

static bool props_sorted;
static style_t initial;

static int cmp_prop(const void *a, const void *b) {
    return strcmp(((const struct propdef *)a)->name, ((const struct propdef *)b)->name);
}

static void init_initial(void) {
    style_t *s = &initial;
    s->display = D_INLINE;
    s->color = 0xFF000000u;
    s->svg_fill.kind = SVG_PAINT_COLOR;
    s->svg_fill.color = s->svg_stop_color = 0xFF000000u;
    s->svg_stroke.kind = SVG_PAINT_NONE;
    s->svg_fill_opacity = s->svg_stroke_opacity = s->svg_stop_opacity = 1;
    s->svg_stroke_width.kind = LK_LEN; s->svg_stroke_width.px = 1;
    s->outline_color = COLOR_CURRENT;
    s->outline_width = 3;
    for (int i = 0; i < 4; i++) {
        s->border_color[i] = COLOR_CURRENT;
        s->border_width[i] = 3;
        s->inset[i].kind = LK_AUTO;
        s->margin[i].kind = LK_LEN;
        s->padding[i].kind = LK_LEN;
    }
    s->font_size = 16;
    s->font_weight = 400;
    s->font_family = FONT_FAMILY_SERIF;
    s->line_height.kind = LK_NORMAL;
    s->width.kind = s->height.kind = LK_AUTO;
    s->min_width.kind = s->min_height.kind = LK_AUTO;
    s->max_width.kind = s->max_height.kind = LK_NONE;
    s->text_indent.kind = LK_LEN;
    s->flex_basis.kind = LK_AUTO;
    s->flex_shrink = 1;
    s->align_content = AC_NORMAL;
    s->opacity = 1;
    s->object_pos[0].kind=s->object_pos[1].kind=LK_LEN;
    s->object_pos[0].pct=s->object_pos[1].pct=50;
    s->transform_origin[0].kind=s->transform_origin[1].kind=LK_LEN;
    s->transform_origin[0].pct=s->transform_origin[1].pct=50;
    s->align_items = s->justify_items = AI_NORMAL;
    s->align_self = 255;
    s->justify_self = 255;
    s->z_auto = true;
    s->bg_layers = 1;
    /* the groups (float_shape, offset, gap_rules, anchor, background, border_image, mask_border,
       mask) stay NULL: their initial records */
}

static void props_init(void) {
    if (props_sorted) return;
    qsort(props, NPROPS, sizeof *props, cmp_prop);
    init_initial();
    props_sorted = true;
}

static const struct propdef *find_prop(const char *name, size_t n) {
    int lo = 0, hi = NPROPS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(name, props[mid].name, n);
        if (!c) c = props[mid].name[n] ? -1 : 0;
        if (!c) return &props[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

const struct propdef *css_prop_lookup(const char *name, size_t n) {
    props_init();
    char low[64];
    if (n >= sizeof low) return NULL;
    for (size_t i = 0; i < n; i++) low[i] = (char)lower((unsigned char)name[i]);
    low[n] = 0;
    const struct propdef *p = find_prop(low, n);
    if (p) return p;
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcmp(aliases[i].alias, low)) return aliases[i].name[0] ? find_prop(aliases[i].name, strlen(aliases[i].name)) : NULL;
    /* prefixed versions of standard properties */
    static const char *const pre[] = {"-webkit-", "-moz-", "-ms-"};
    for (int k = 0; k < 3; k++) {
        size_t pl = strlen(pre[k]);
        if (n > pl && !strncmp(low, pre[k], pl)) return css_prop_lookup(low + pl, n - pl);
    }
    return NULL;
}

int css_prop_count(void) { return NPROPS; }
const char *css_prop_name(int index,bool *longhand) {
    if(index<0||index>=NPROPS)return NULL;
    props_init();
    if(longhand)*longhand=props[index].type!=PT_SHORT;
    return props[index].name;
}
int css_prop_index(const struct propdef *p) { return (int)(p - props); }
const struct propdef *css_prop_at(int index) { return index >= 0 && index < NPROPS ? &props[index] : NULL; }
/* font-size gives em, color-scheme the system colours and light-dark() */
static bool early(const struct propdef *p) { return p->type == PT_FONT_SIZE || p->type == PT_COLOR_SCHEME; }
bool css_prop_is_early(const struct propdef *p) {
    return early(p) || (p->type == PT_SHORT && (p->sh == SH_FONT || p->sh == SH_ALL));
}
bool css_prop_is_motion(const struct propdef *p) {
    return p && (p->type == PT_MOTION_LIST || (p->type == PT_SHORT && (p->sh == SH_ANIMATION || p->sh == SH_TRANSITION)));
}
bool css_prop_inherited(const struct propdef *p) { return p && p->inherited; }
/* the colour slot (color.c) of a PT_COLOR or PT_SVG_PAINT property */
static enum css_color_slot color_slot(const struct propdef *p) {
    size_t off = p->off;
    if (off == offsetof(style_t, color)) return CSS_SLOT_COLOR;
    if (off == offsetof(style_t, svg_fill)) return CSS_SLOT_FILL;
    if (off == offsetof(style_t, svg_stroke)) return CSS_SLOT_STROKE;
    if (off == offsetof(style_t, bg_color)) return CSS_SLOT_BACKGROUND;
    if (off == offsetof(style_t, outline_color)) return CSS_SLOT_OUTLINE;
    if (off == offsetof(style_t, svg_stop_color)) return CSS_SLOT_STOP;
    return (enum css_color_slot)(CSS_SLOT_BORDER_TOP + (off - offsetof(style_t, border_color)) / sizeof(uint32_t));
}
/* ---------------------------------------------------------------- the style's groups */
/* the record that holds group g's fields in s (the initial one when s holds none) */
static const void *group_of(const style_t *s, uint8_t g) {
    switch (g) {
    case G_FLOAT_SHAPE: return css_style_float_shape(s);
    case G_OFFSET: return css_style_offset(s);
    case G_GAP_RULE: return css_style_gap_rules(s);
    case G_ANCHOR: return css_style_anchor(s);
    case G_COUNTER: return css_style_counters(s);
    case G_LIST: return css_style_list(s);
    case G_BACKGROUND: return css_style_background(s);
    case G_BORDER_IMAGE: return css_style_border_image(s);
    case G_MASK_BORDER: return css_style_mask_border(s);
    case G_MASK: return css_style_mask(s);
    }
    return s;
}
static size_t group_size(uint8_t g) {
    switch (g) {
    case G_FLOAT_SHAPE: return sizeof(struct css_float_shape);
    case G_OFFSET: return sizeof(struct css_offset);
    case G_GAP_RULE: return sizeof(struct css_gap_rules);
    case G_ANCHOR: return sizeof(struct css_anchor);
    case G_COUNTER: return sizeof(struct css_counters);
    case G_BACKGROUND: return sizeof(struct css_background);
    case G_BORDER_IMAGE: case G_MASK_BORDER: return sizeof(struct css_border_image);
    case G_MASK: return sizeof(struct css_mask);
    }
    return sizeof(struct css_list);
}
static bool group_same(const style_t *a, const style_t *b, uint8_t g) {
    switch (g) {
    case G_FLOAT_SHAPE: return a->float_shape == b->float_shape;
    case G_OFFSET: return a->offset == b->offset;
    case G_GAP_RULE: return a->gap_rules == b->gap_rules;
    case G_ANCHOR: return a->anchor == b->anchor;
    case G_COUNTER: return a->counters == b->counters;
    case G_BACKGROUND: return a->background == b->background;
    case G_BORDER_IMAGE: return a->border_image == b->border_image;
    case G_MASK_BORDER: return a->mask_border == b->mask_border;
    case G_MASK: return a->mask == b->mask;
    }
    return a->list == b->list;
}
/* s's group g becomes a copy of value in a, for the caller to finish writing: the group s held may
   be another style's too (box.c's copies of a style, the animation layer's copy of its base) */
static void *group_write(style_t *s, uint8_t g, const void *value, arena_t *a) {
    size_t size = group_size(g);
    void *copy = ar_alloc(a, size);
    memcpy(copy, value, size);
    switch (g) {
    case G_FLOAT_SHAPE: s->float_shape = copy; break;
    case G_OFFSET: s->offset = copy; break;
    case G_GAP_RULE: s->gap_rules = copy; break;
    case G_BACKGROUND: s->background = copy; break;
    case G_BORDER_IMAGE: s->border_image = copy; s->border_image_img = 0; break; /* found again after the cascade */
    case G_MASK_BORDER: s->mask_border = copy; s->mask_border_img = 0; break;
    case G_MASK: s->mask = copy; break; /* its images keep their documents' images (css_image's img) */
    case G_ANCHOR: s->anchor = copy; break;
    case G_COUNTER: s->counters = copy; break;
    default: s->list = copy;
    }
    return copy;
}
/* every group of src whose properties are not inherited, as copying each of them would */
static void groups_copy(style_t *dst, const style_t *src) {
    dst->float_shape = src->float_shape;
    dst->offset = src->offset;
    dst->gap_rules = src->gap_rules;
    dst->anchor = src->anchor;
    dst->counters = src->counters;
    dst->background = src->background;
    dst->border_image = src->border_image; dst->border_image_img = src->border_image_img;
    dst->mask_border = src->mask_border; dst->mask_border_img = src->mask_border_img;
    dst->mask = src->mask;
}
/* the property's field in s */
static const void *field_of(const style_t *s, const struct propdef *p) {
    return (const char *)group_of(s, p->group) + p->off;
}
/* the gap decorations' values (below): the computed value getComputedStyle reads, longhand or
   shorthand (1; -1 out of memory), and a declared value's specified serialization */
static int gap_computed(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d);
static bool gap_specified(const struct propdef *p, const char *v, size_t n, sbuf *out);
static bool position_try_text(const struct css_anchor *a, sbuf *out);
static bool anchor_specified(const struct propdef *p, const char *v, size_t n, sbuf *out);
static void list_style_text(const style_t *st, sbuf *out);
static bool list_value_text(const style_t *st, const struct propdef *p, sbuf *out);
static bool list_specified(const struct propdef *p, const char *v, size_t n, sbuf *out);
/* the background, border-image and mask-border groups' values (below): a declared value's specified
   serialization, and their shorthands' computed values */
static bool background_specified(const struct propdef *p, const char *v, size_t n, sbuf *out);
static bool group_shorthand_text(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d);
/* the mask shorthand's (below): its specified value, and its computed one */
static bool mask_specified(const struct propdef *p, const char *v, size_t n, sbuf *out);
static bool mask_computed(const style_t *st, sbuf *out, web_doc *d);
int css_computed_longhand(const style_t *st, const char *name, sbuf *out, web_doc *d) {
    const struct propdef *p = css_prop_lookup(name, strlen(name));
    if (!p) return 0;
    const void *field = field_of(st, p);
    char text[32];
    switch (p->type) {
    case PT_KW: case PT_OVERFLOW: case PT_VALIGN: {
        if (p->type == PT_VALIGN && st->vertical_align == VA_LEN) return length_number(out, st->vertical_align_px, "px") ? 1 : -1;
        /* Each table names a stored value's canonical keyword first. */
        uint8_t v = *(const uint8_t *)field;
        for (const struct kw *k = p->kws; k && k->name; k++)
            if (k->v == v) { sb_puts(out, k->name); return 1; }
        return 0;
    }
    case PT_LEN:
        return css_length_serialize((const len_t *)field, out, d) ? 1 : -1;
    case PT_PX: {
        if (p->size != sizeof(float)) return 0;
        float v = *(const float *)field;
        if (p->kws == kw_normal0 && v == 0) { sb_puts(out, "normal"); return 1; }
        return length_number(out, v, "px") ? 1 : -1;
    }
    case PT_NUM: case PT_OPACITY:
        snprintf(text, sizeof text, "%g", (double)*(const float *)field); sb_puts(out, text); return 1;
    case PT_INT:
        if (p->size != sizeof(int32_t)) return 0;
        snprintf(text, sizeof text, "%d", (int)*(const int32_t *)field); sb_puts(out, text); return 1;
    case PT_COLOR: return css_style_color_text(st, color_slot(p), out) ? 1 : -1;
    case PT_COLOR_SCHEME: sb_puts(out, st->color_scheme_text ? st->color_scheme_text : "normal"); return 1;
    case PT_MOTION_LIST: case PT_SHAPE: case PT_OFFSET: case PT_ROTATE: case PT_SCALE: case PT_ANCHOR:
    case PT_CONTENT: case PT_LIST_TYPE: case PT_LIST_IMAGE: case PT_QUOTES: case PT_COUNTER:
    case PT_BG_LIST: case PT_BG_OFFSET: case PT_BI_SLICE: case PT_BI_SIDES: case PT_BI_REPEAT:
    case PT_MASK_IMAGE: case PT_MASK_REPEAT: case PT_MASK_POSITION: case PT_MASK_SIZE: case PT_CLIP:
        return css_computed_value(st, p, out, d) ? 1 : -1;
    case PT_BI_SOURCE: return css_computed_value(st, p, out, d) ? 1 : 0; /* a gradient: not yet serialized */
    case PT_RULE_LIST: case PT_RULE_INSET: return gap_computed(st, p, out, d);
    case PT_SHORT: /* the shorthands whose longhands' owner serializes them together */
        if (p->sh == SH_LIST_STYLE) { list_style_text(st, out); return 1; }
        if (p->sh == SH_OFFSET) return css_offset_shorthand_serialize(css_style_offset(st), out, d) ? 1 : -1;
        if (p->sh == SH_GAP_RULE) return gap_computed(st, p, out, d);
        if (p->sh == SH_POSITION_TRY) return position_try_text(css_style_anchor(st), out) ? 1 : -1;
        if (p->sh == SH_BACKGROUND_POSITION || p->sh == SH_BORDER_IMAGE || p->sh == SH_MASK_BORDER || p->sh == SH_MASK) {
            size_t begin = out->n;
            if (group_shorthand_text(st, p, out, d)) return 1;
            out->n = begin;
            sb_cstr(out);
        }
        return 0;
    }
    return 0;
}
/* CSSOM: a declared value's specified serialization for the properties whose
   values the table serializes itself; false leaves the authored text */
static bool apply_long(const struct propdef *p, const char *v, size_t n, struct cx *cx);
bool css_specified_longhand(const char *name, const char *v, size_t n, sbuf *out) {
    const struct propdef *p = css_prop_lookup(name, strlen(name));
    if (!p) return false;
    trim(&v, &n);
    /* a CSS-wide keyword serializes as itself */
    if (ident_is(v, n, "inherit") || ident_is(v, n, "initial") || ident_is(v, n, "unset") || ident_is(v, n, "revert") ||
        ident_is(v, n, "revert-layer")) return false;
    if (p->type == PT_COLOR || (p->type == PT_SVG_PAINT && !ident_is(v, n, "none"))) return css_color_specified(v, n, out);
    if (p->group == G_GAP_RULE || (p->type == PT_SHORT && p->sh == SH_GAP_RULE)) return gap_specified(p, v, n, out);
    if (p->group == G_ANCHOR || (p->type == PT_SHORT && p->sh == SH_POSITION_TRY)) return anchor_specified(p, v, n, out);
    if (p->type == PT_CONTENT || p->type == PT_LIST_TYPE || p->type == PT_LIST_IMAGE || p->type == PT_QUOTES ||
        p->type == PT_COUNTER || (p->type == PT_SHORT && p->sh == SH_LIST_STYLE))
        return list_specified(p, v, n, out);
    if (p->type == PT_SHORT && p->sh == SH_MASK) return mask_specified(p, v, n, out);
    if (p->group == G_BACKGROUND || p->group == G_BORDER_IMAGE || p->group == G_MASK_BORDER || p->group == G_MASK ||
        (p->type == PT_SHORT && (p->sh == SH_BACKGROUND_POSITION || p->sh == SH_BORDER_IMAGE || p->sh == SH_MASK_BORDER)))
        return background_specified(p, v, n, out);
    /* a length (or a shorthand of lengths) with an anchor function in it; the rest as other lengths */
    if (((p->type == PT_LEN && (p->flags & (LF_ANCHOR | LF_ANCHOR_SIZE))) ||
         (p->type == PT_SHORT && (p->sh == SH_INSET || p->sh == SH_INSET_BLOCK || p->sh == SH_INSET_INLINE ||
                                  p->sh == SH_MARGIN || p->sh == SH_MARGIN_BLOCK || p->sh == SH_MARGIN_INLINE))) &&
        anchor_specified(p, v, n, out))
        return true;
    if (p->type == PT_SHAPE || p->group != G_STYLE || (p->type == PT_SHORT && p->sh == SH_OFFSET))
        return css_shape_specified(p->name, v, n, out);
    if (p->type != PT_COLOR_SCHEME) {
        /* math functions serialize simplified (CSS Values 4 §10.13); in a <length-percentage> context
           percentages are lengths' (calc(10% / 1px) is a number there) */
        unsigned accept = CSS_CALC_ANY;
        if (p->type == PT_LEN || p->type == PT_PX || p->type == PT_FONT_SIZE || p->type == PT_VALIGN)
            accept = CSS_CALC_LENGTH | (p->type == PT_PX || (p->type == PT_LEN && (p->flags & LF_NOPCT)) ? 0 : CSS_CALC_PERCENT);
        else if (p->type == PT_LINE_HEIGHT) accept = CSS_CALC_NUMBER | CSS_CALC_LENGTH | CSS_CALC_PERCENT;
        return css_calc_specified(v, n, accept, out);
    }
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t scratch = {0};
        uint8_t set[1] = {0};
        struct cx cx = {.s = &scratch, .a = &a, .set = set, .supports_probe = true};
        if (apply_long(p, v, n, &cx)) {
            sb_puts(out, scratch.color_scheme_text ? scratch.color_scheme_text : "normal");
            ok = true;
        }
    }
    ar_free(&a);
    return ok;
}
/* CSS Typed OM §5 (css-typed-om-1 "CSSStyleValue reification"): what the property's grammar makes of a
   value, read from its row: shorthands and font-family are a CSSStyleValue (§5.1), a colour property's
   keyword is one only for currentcolor, transform and the image layers have their own objects, and a
   numeric value is a CSSNumericValue where the grammar takes that type. The layered properties
   (animations, transitions, background and mask layers) are list-valued. The logical shorthands of
   two sides (margin-block, inset-inline, border-block, ...) and the logical names of the border
   sides (border-block-start is border-top here) take one value for both, which reifies as one side's
   longhand does: a length or percentage, or (the borders) a keyword. */
bool css_prop_reify(const char *name, size_t n, struct css_reify *out) {
    const struct propdef *p = css_prop_lookup(name, n);
    if (!p) return false;
    *out = (struct css_reify){0};
    if (p->type == PT_SHORT) {
        bool alias = strlen(p->name) != n || strncasecmp(p->name, name, n);
        switch (p->sh) {
        case SH_MARGIN_INLINE: case SH_MARGIN_BLOCK: case SH_PADDING_INLINE: case SH_PADDING_BLOCK:
        case SH_INSET_INLINE: case SH_INSET_BLOCK:
            out->kind = CSS_REIFY_VALUE; out->typed = true; out->accept = CSS_CALC_LENGTH | CSS_CALC_PERCENT;
            return true;
        case SH_BORDER_INLINE: case SH_BORDER_BLOCK:
            out->kind = CSS_REIFY_VALUE; out->typed = true;
            return true;
        case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT:
            if (alias) { out->kind = CSS_REIFY_VALUE; out->typed = true; return true; }
            break;
        }
        out->kind = CSS_REIFY_STYLE; out->shorthand = true;
        return true;
    }
    out->list = p->type == PT_MOTION_LIST || p->type == PT_BG_IMAGE || p->type == PT_MASK_IMAGE || p->type == PT_BG_POS ||
                p->type == PT_BG_SIZE || p->type == PT_MASK_SIZE || (p->type == PT_KW && p->kws == kw_bg_repeat) ||
                p->type == PT_BG_LIST || p->type == PT_BG_OFFSET || p->type == PT_MASK_REPEAT || p->type == PT_MASK_POSITION;
    out->kind = p->type == PT_FONT_FAMILY ? CSS_REIFY_STYLE :
                p->type == PT_COLOR || p->type == PT_SVG_PAINT ? CSS_REIFY_COLOR :
                p->type == PT_TRANSFORM ? CSS_REIFY_TRANSFORM :
                p->type == PT_BG_IMAGE || p->type == PT_MASK_IMAGE ||
                (p->type == PT_SHAPE && p->flags == CSS_SHAPE_OUTSIDE) ? CSS_REIFY_IMAGE : CSS_REIFY_VALUE;
    unsigned accept = 0;
    bool typed = true;
    switch (p->type) {
    case PT_LEN: accept = CSS_CALC_LENGTH | (p->flags & LF_NOPCT ? 0 : CSS_CALC_PERCENT); break;
    case PT_PX: accept = CSS_CALC_LENGTH; break;
    case PT_FONT_SIZE: case PT_VALIGN: case PT_RADIUS: case PT_TRANSLATE: case PT_BG_SIZE: case PT_MASK_SIZE:
    case PT_BG_POS: case PT_TRANSFORM_ORIGIN: case PT_MASK_POSITION:
        accept = CSS_CALC_LENGTH | CSS_CALC_PERCENT; break;
    case PT_GTEMPLATE: accept = CSS_CALC_LENGTH | CSS_CALC_PERCENT | CSS_CALC_FLEX; break;
    case PT_LINE_HEIGHT: case PT_SVG_WIDTH: accept = CSS_CALC_NUMBER | CSS_CALC_LENGTH | CSS_CALC_PERCENT; break;
    case PT_INT: case PT_ZINDEX: case PT_LINE_CLAMP: out->integer = true; /* fall through */
    case PT_NUM: case PT_FONT_WEIGHT: accept = CSS_CALC_NUMBER; break;
    case PT_OPACITY: case PT_SCALE: accept = CSS_CALC_NUMBER | CSS_CALC_PERCENT; break;
    case PT_ROTATE: accept = CSS_CALC_ANGLE; break;
    /* offset-rotate: [auto | reverse] || <angle>; shape-outside: a box, an <image> (a basic shape is
       not reified in Level 1) */
    case PT_OFFSET: if (p->flags == CSS_OFFSET_ROTATE) accept = CSS_CALC_ANGLE; else typed = false; break;
    case PT_SHAPE: if (p->flags != CSS_SHAPE_OUTSIDE) typed = false; break;
    case PT_MOTION_LIST:
        accept = p->flags == ML_DURATION || p->flags == ML_DELAY || p->flags == ML_TDURATION || p->flags == ML_TDELAY ?
                 CSS_CALC_TIME : p->flags == ML_ITERATIONS ? CSS_CALC_NUMBER : 0;
        break;
    /* keywords, colours, images, names, shadows and filters: no lone numeric value; a grid line's
       integer goes with a name or span, a CSSStyleValue (WPT grid-start-end.html) */
    case PT_KW: case PT_DISPLAY: case PT_COLOR: case PT_FONT_FAMILY: case PT_LIST_TYPE: case PT_BG_IMAGE: case PT_OVERFLOW:
    case PT_MASK_IMAGE: case PT_SVG_PAINT: case PT_CONTAINER_NAMES: case PT_TRANSFORM: case PT_GAREAS: case PT_FILTER:
    case PT_BOX_SHADOW: case PT_CLIP: case PT_COLOR_SCHEME: case PT_GLINE: case PT_BG_LIST: case PT_MASK_REPEAT:
        break;
    default: typed = false;
    }
    out->accept = accept;
    out->typed = typed;
    return true;
}

/* ---------------------------------------------------------------- longhand values */
static bool kw_find(const struct kw *k, const char *s, size_t n, uint8_t *out) {
    for (; k->name; k++)
        if (ident_is(s, n, k->name)) {
            *out = k->v;
            return true;
        }
    return false;
}
static const char *kw_name(const struct kw *k, uint8_t v) {
    for (; k->name; k++) if (k->v == v) return k->name;
    return NULL;
}

/* ---------------------------------------------------------------- gap decorations (css-gaps-1) */
static void anim_unit(sbuf *b, double v, const char *unit);
static struct css_color anim_color_used(const style_t *st, const struct css_color_computed *v);
static bool anim_color_text(sbuf *b, const struct css_color_computed *v);

/* §4.1-4.3 and §3: currentcolor, none, medium; insets 0; normal; row-over-column */
static const struct css_rule_item rule_initial_item = {0, 0, 1};
static const union css_rule_value rule_initial_values[3] = {
    {.color = {COLOR_CURRENT, NULL}}, {.style = BS_NONE}, {.width = 3}};
#define RULE_INITIAL_AXIS {                                                                                          \
    .list = {{&rule_initial_item, &rule_initial_values[CSS_GAP_COLOR], 1, 1},                                     \
             {&rule_initial_item, &rule_initial_values[CSS_GAP_STYLE], 1, 1},                                     \
             {&rule_initial_item, &rule_initial_values[CSS_GAP_WIDTH], 1, 1}},                                    \
    .inset = {{{.kind = LK_LEN}, false}, {{.kind = LK_LEN}, false}, {{.kind = LK_LEN}, false}, {{.kind = LK_LEN}, false}}, \
    .rule_break = CSS_RULE_BREAK_NORMAL, .visibility = CSS_RULE_ITEMS_NORMAL}
const struct css_gap_rules css_gap_rules_initial = {.axis = {RULE_INITIAL_AXIS, RULE_INITIAL_AXIS},
                                                    .overlap = CSS_RULE_ROW_OVER_COLUMN};

static bool rule_value_equal(int kind, const union css_rule_value *a, const union css_rule_value *b) {
    if (kind == CSS_GAP_COLOR) return a->color.argb == b->color.argb && css_color_value_equal(a->color.exact, b->color.exact);
    return kind == CSS_GAP_STYLE ? a->style == b->style : a->width == b->width;
}
static bool rule_items_equal(const struct css_rule_list *a, const struct css_rule_list *b) {
    if (a->nitems != b->nitems || a->nvalues != b->nvalues) return false;
    for (uint32_t i = 0; i < a->nitems; i++)
        if (a->items[i].repeat != b->items[i].repeat || a->items[i].count != b->items[i].count) return false;
    return true;
}
static bool rule_list_equal(int kind, const struct css_rule_list *a, const struct css_rule_list *b) {
    if (!rule_items_equal(a, b)) return false;
    for (uint32_t i = 0; i < a->nvalues; i++) if (!rule_value_equal(kind, &a->values[i], &b->values[i])) return false;
    return true;
}
static bool rule_inset_equal(const struct css_rule_inset *a, const struct css_rule_inset *b) {
    return a->join == b->join && (a->join || css_length_equal(&a->length, &b->length));
}
/* what the segments' geometry takes from one axis (layout.c): everything but colour and style */
static bool rule_axis_layout_equal(const struct css_gap_axis *a, const struct css_gap_axis *b) {
    if (a->rule_break != b->rule_break || a->visibility != b->visibility ||
        !rule_list_equal(CSS_GAP_WIDTH, &a->list[CSS_GAP_WIDTH], &b->list[CSS_GAP_WIDTH])) return false;
    for (int i = 0; i < 4; i++) if (!rule_inset_equal(&a->inset[i], &b->inset[i])) return false;
    return true;
}
bool css_gap_rules_equal(const struct css_gap_rules *a, const struct css_gap_rules *b) {
    if (a == b) return true;
    if (!a) a = &css_gap_rules_initial;
    if (!b) b = &css_gap_rules_initial;
    if (a->overlap != b->overlap) return false;
    for (int x = 0; x < 2; x++)
        if (!rule_axis_layout_equal(&a->axis[x], &b->axis[x]) ||
            !rule_list_equal(CSS_GAP_COLOR, &a->axis[x].list[CSS_GAP_COLOR], &b->axis[x].list[CSS_GAP_COLOR]) ||
            !rule_list_equal(CSS_GAP_STYLE, &a->axis[x].list[CSS_GAP_STYLE], &b->axis[x].list[CSS_GAP_STYLE])) return false;
    return true;
}
/* a container records its segments only while its style holds the group (layout.c) */
bool css_gap_rules_layout_equal(const struct css_gap_rules *a, const struct css_gap_rules *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return rule_axis_layout_equal(&a->axis[0], &b->axis[0]) && rule_axis_layout_equal(&a->axis[1], &b->axis[1]);
}

/* how many values item it stands for once written out (an auto repeater's own values once) */
static uint64_t rule_item_span(const struct css_rule_item *it) {
    return it->repeat == 0 ? 1 : it->repeat == CSS_RULE_AUTO ? it->count : (uint64_t)it->repeat * it->count;
}
/* value k of items [from, to) with their integer repeaters written out */
static union css_rule_value rule_expanded_at(const struct css_rule_list *l, uint32_t from, uint32_t to, uint64_t k) {
    for (uint32_t i = from; i < to; i++) {
        const struct css_rule_item *it = &l->items[i];
        uint64_t span = rule_item_span(it);
        if (k < span) return l->values[it->first + (uint32_t)(k % it->count)];
        k -= span;
    }
    return l->values[l->nvalues - 1];
}
/* §4.6 "assign gap decoration values to a list of gaps" for gap index of count */
union css_rule_value css_rule_value_at(const struct css_rule_list *l, uint32_t index, uint32_t count) {
    uint32_t a = l->nitems;
    for (uint32_t i = 0; i < l->nitems; i++) if (l->items[i].repeat == CSS_RULE_AUTO) { a = i; break; }
    uint64_t leading = 0, trailing = 0;
    for (uint32_t i = 0; i < a; i++) leading += rule_item_span(&l->items[i]);
    if (a == l->nitems) return rule_expanded_at(l, 0, a, leading ? index % leading : 0);
    for (uint32_t i = a + 1; i < l->nitems; i++) trailing += rule_item_span(&l->items[i]);
    if (index < leading) return rule_expanded_at(l, 0, a, index);
    /* the trailing gaps are the last trailing count, without the leading ones */
    uint64_t first_trailing = count > trailing ? count - trailing : 0;
    if (first_trailing < leading) first_trailing = leading;
    if (index >= first_trailing) return rule_expanded_at(l, a + 1, l->nitems, index - first_trailing);
    const struct css_rule_item *it = &l->items[a];
    return l->values[it->first + (uint32_t)((index - leading) % it->count)];
}

/* the parts of a comma-separated list, each trimmed; -1 for an empty part or an unbalanced bracket */
static int rule_parts(const char *v, size_t n, struct cx *cx, const char ***tok, size_t **len) {
    size_t count = 1;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (quote) { if (c == '\\') i++; else if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '(') depth++;
        else if (c == ')') { if (!depth) return -1; depth--; }
        else if (c == ',' && !depth) count++;
    }
    if (depth || quote || count > INT32_MAX) return -1;
    const char **t = ar_alloc(cx->a, count * sizeof *t);
    size_t *l = ar_alloc(cx->a, count * sizeof *l), k = 0, start = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? v[i] : ',';
        if (i < n && quote) { if (c == '\\') i++; else if (c == quote) quote = 0; continue; }
        if (i < n && (c == '"' || c == '\'')) { quote = c; continue; }
        if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && !depth) {
            const char *s = v + start;
            size_t m = (i < n ? i : n) - start;
            trim(&s, &m);
            if (!m) return -1;
            t[k] = s; l[k++] = m;
            start = i + 1;
        }
    }
    *tok = t; *len = l;
    return (int)k;
}
/* repeat( <count> , <values> ): the two arguments' text, when s is that whole function */
static bool rule_repeat(const char *s, size_t n, const char **count, size_t *count_n, const char **values, size_t *values_n) {
    if (n < 8 || !strn_ieq(s, "repeat(", 7)) return false;
    const char *p = s + 7, *end = s + n, *comma = NULL;
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (!depth) break; depth--; }
        else if (*p == ',' && !depth && !comma) comma = p;
    }
    if (p != end - 1) return false; /* its closing bracket ends the part */
    if (!comma) comma = p;
    *count = s + 7; *count_n = (size_t)(comma - (s + 7));
    *values = comma < p ? comma + 1 : p; *values_n = (size_t)(p - *values);
    return true;
}
/* <integer [1,∞]> (a math function rounds to one) or auto */
static bool rule_repeat_count(const char *s, size_t n, struct cx *cx, uint32_t *out) {
    trim(&s, &n);
    if (!n) return false;
    if (ident_is(s, n, "auto")) { *out = CSS_RULE_AUTO; return true; }
    size_t i = s[0] == '+' ? 1 : 0;
    if (i < n && isdigit((unsigned char)s[i])) {
        uint64_t v = 0;
        for (; i < n; i++) {
            if (!isdigit((unsigned char)s[i])) return false;
            if (v < INT32_MAX) v = v * 10 + (uint64_t)(s[i] - '0');
        }
        if (v < 1) return false;
        *out = v > INT32_MAX ? INT32_MAX : (uint32_t)v; /* integers beyond the range clamp */
        return true;
    }
    float x;
    if (!isalpha((unsigned char)s[0]) || !parse_number_value(s, n, cx, &x)) return false;
    x = roundf(x);
    if (!(x >= 1)) return false;
    *out = x > (float)INT32_MAX ? INT32_MAX : (uint32_t)x;
    return true;
}
/* §4.3: a <line-width> as a border width snaps (css-values-4 "snap as a border width") */
static float rule_snap(float w) { return w > 0 && w < 1 ? 1 : floorf(w); }
static bool rule_width(const char *s, size_t n, struct cx *cx, float *out) {
    uint8_t k;
    if (kw_find(kw_bwidth, s, n, &k)) { *out = k; return true; }
    len_t l;
    if (!parse_len(s, n, cx, &l, LF_NOPCT | LF_LENGTH) || (l.kind != LK_LEN && l.kind != LK_EXPR)) return false;
    float px = len_resolve(&l, 0);
    if (!isfinite(px) || px != len_resolve(&l, 100)) return false; /* a <length>: no percentage inside */
    *out = rule_snap(px < 0 ? 0 : px); /* a math function clamps to the range */
    return true;
}
/* one value of a list of kind */
static bool rule_value(int kind, const char *s, size_t n, struct cx *cx, union css_rule_value *out) {
    trim(&s, &n);
    if (!n) return false;
    if (kind == CSS_GAP_STYLE) return kw_find(kw_bstyle, s, n, &out->style);
    if (kind == CSS_GAP_WIDTH) return rule_width(s, n, cx, &out->width);
    return css_color_value_compute(s, n, cx, NULL, &out->color.argb, &out->color.exact);
}
static void rule_spec_word(sbuf *spec, const char *s, size_t n) {
    if (!isalpha((unsigned char)s[0])) { shadow_specified_length(spec, s, n); return; }
    for (size_t i = 0; i < n; i++) sb_putc(spec, (char)lower((unsigned char)s[i]));
}
/* §4.4 <gap-rule> = <line-width> || <line-style> || <color>, by list kind; the specified value
   leaves out what is initial and orders the rest as the grammar does */
static bool gap_rule(const char *s, size_t n, struct cx *cx, union css_rule_value v[3], sbuf *spec) {
    const char *t[4];
    size_t tl[4];
    int k = split(s, n, t, tl, 4, ' ');
    if (k < 1 || k > 3 || t[k - 1] + tl[k - 1] != s + n) return false;
    int at[3] = {-1, -1, -1};
    v[CSS_GAP_COLOR] = rule_initial_values[CSS_GAP_COLOR];
    v[CSS_GAP_STYLE] = rule_initial_values[CSS_GAP_STYLE];
    v[CSS_GAP_WIDTH] = rule_initial_values[CSS_GAP_WIDTH];
    static const int order[3] = {CSS_GAP_STYLE, CSS_GAP_COLOR, CSS_GAP_WIDTH};
    for (int i = 0; i < k; i++) {
        int found = -1;
        for (int j = 0; j < 3 && found < 0; j++)
            if (at[order[j]] < 0 && rule_value(order[j], t[i], tl[i], cx, &v[order[j]])) found = order[j];
        if (found < 0) return false;
        at[found] = i;
    }
    if (!spec) return true;
    size_t begin = spec->n;
    int w = at[CSS_GAP_WIDTH], st = at[CSS_GAP_STYLE], c = at[CSS_GAP_COLOR];
    if (w >= 0 && !ident_is(t[w], tl[w], "medium")) rule_spec_word(spec, t[w], tl[w]);
    if (st >= 0 && !ident_is(t[st], tl[st], "none")) {
        if (spec->n > begin) sb_putc(spec, ' ');
        rule_spec_word(spec, t[st], tl[st]);
    }
    if (c >= 0) {
        sbuf color = {0};
        if (css_color_specified(t[c], tl[c], &color) && !ident_is(color.p, color.n, "currentcolor")) {
            if (spec->n > begin) sb_putc(spec, ' ');
            sb_put(spec, color.p, color.n);
        }
        sb_free(&color);
    }
    if (spec->n == begin) sb_puts(spec, "medium");
    return true;
}
/* §4.1-4.5: a list of kind's values (kind RULE_GAP: the shorthand's <gap-rule-list>, into the three
   lists at once, with its specified value in spec) with at most one auto repeater */
enum { RULE_GAP = 3 };
static bool rule_lists(int kind, const char *v, size_t n, struct cx *cx, struct css_rule_list out[3], sbuf *spec) {
    const char **part;
    size_t *part_n;
    int parts = rule_parts(v, n, cx, &part, &part_n);
    if (parts < 1) return false;
    struct css_rule_item *items = ar_alloc(cx->a, (size_t)parts * sizeof *items);
    const char ***text = ar_alloc(cx->a, (size_t)parts * sizeof *text);
    size_t **text_n = ar_alloc(cx->a, (size_t)parts * sizeof *text_n);
    uint64_t total = 0;
    bool repeat_auto = false;
    for (int i = 0; i < parts; i++) {
        const char *count, *values;
        size_t count_n, values_n;
        if (rule_repeat(part[i], part_n[i], &count, &count_n, &values, &values_n)) {
            uint32_t times;
            if (!rule_repeat_count(count, count_n, cx, &times)) return false;
            if (times == CSS_RULE_AUTO) { if (repeat_auto) return false; repeat_auto = true; }
            int inner = rule_parts(values, values_n, cx, &text[i], &text_n[i]);
            if (inner < 1) return false;
            items[i] = (struct css_rule_item){times, (uint32_t)total, (uint32_t)inner};
            total += (uint64_t)inner;
        } else {
            text[i] = &part[i]; text_n[i] = &part_n[i];
            items[i] = (struct css_rule_item){0, (uint32_t)total, 1};
            total++;
        }
        if (total > UINT32_MAX) return false;
    }
    int first = kind == RULE_GAP ? 0 : kind, last = kind == RULE_GAP ? 2 : kind;
    union css_rule_value *values[3] = {NULL, NULL, NULL};
    for (int l = first; l <= last; l++) values[l] = ar_alloc(cx->a, (size_t)total * sizeof *values[l]);
    for (int i = 0; i < parts; i++) {
        const struct css_rule_item *it = &items[i];
        if (spec) {
            if (i) sb_puts(spec, ", ");
            if (it->repeat == CSS_RULE_AUTO) sb_puts(spec, "repeat(auto, ");
            else if (it->repeat) { char number[16]; snprintf(number, sizeof number, "repeat(%u, ", it->repeat); sb_puts(spec, number); }
        }
        for (uint32_t j = 0; j < it->count; j++) {
            if (spec && j) sb_puts(spec, ", ");
            if (kind == RULE_GAP) {
                union css_rule_value triple[3];
                if (!gap_rule(text[i][j], text_n[i][j], cx, triple, spec)) return false;
                for (int l = 0; l < 3; l++) values[l][it->first + j] = triple[l];
            } else if (!rule_value(kind, text[i][j], text_n[i][j], cx, &values[kind][it->first + j])) return false;
        }
        if (spec && it->repeat) sb_putc(spec, ')');
    }
    for (int l = first; l <= last; l++) out[l] = (struct css_rule_list){items, values[l], (uint32_t)parts, (uint32_t)total};
    return true;
}
/* <inset-value> (§3.3) */
static bool rule_inset(const char *s, size_t n, struct cx *cx, struct css_rule_inset *out, sbuf *spec) {
    trim(&s, &n);
    if (ident_is(s, n, "overlap-join")) {
        *out = (struct css_rule_inset){{.kind = LK_LEN}, true};
        if (spec) sb_puts(spec, "overlap-join");
        return true;
    }
    len_t l;
    if (!parse_len(s, n, cx, &l, LF_NEG | LF_LENGTH) || (l.kind != LK_LEN && l.kind != LK_EXPR)) return false;
    *out = (struct css_rule_inset){l, false};
    if (spec) shadow_specified_length(spec, s, n);
    return true;
}
/* the longhand names each GS_* sets on one axis, after its column- or row- */
static const char *const gap_longhand_names[GS_INSET + 1][4] = {
    [GS_RULE] = {"-rule-width", "-rule-style", "-rule-color"},
    [GS_COLOR] = {"-rule-color"}, [GS_STYLE] = {"-rule-style"}, [GS_WIDTH] = {"-rule-width"},
    [GS_BREAK] = {"-rule-break"}, [GS_ITEMS] = {"-rule-visibility-items"},
    [GS_INSET_START] = {"-rule-inset-cap-start", "-rule-inset-junction-start"},
    [GS_INSET_END] = {"-rule-inset-cap-end", "-rule-inset-junction-end"},
    [GS_INSET_CAP] = {"-rule-inset-cap-start", "-rule-inset-cap-end"},
    [GS_INSET_JUNCTION] = {"-rule-inset-junction-start", "-rule-inset-junction-end"},
    [GS_INSET] = {"-rule-inset-cap-start", "-rule-inset-cap-end", "-rule-inset-junction-start", "-rule-inset-junction-end"},
};
static const struct propdef *find_prop(const char *name, size_t n);
static const struct propdef *gap_longhand(int what, int axis, int i) {
    const char *suffix = gap_longhand_names[what][i];
    if (!suffix) return NULL;
    char name[64];
    snprintf(name, sizeof name, "%s%s", axis ? "row" : "column", suffix);
    return find_prop(name, strlen(name));
}
/* the inset shorthands' values (§3.3.1-3.3.3) into inset in the longhands' order, each value's
   specified text in spec (when given) */
static bool rule_insets(int what, const char *v, size_t n, struct cx *cx, struct css_rule_inset inset[4], sbuf spec[4]) {
    const char *half[3], *t[3];
    size_t half_n[3], tn[3];
    trim(&v, &n);
    int halves = what == GS_INSET ? split(v, n, half, half_n, 3, '/') : 1;
    if (what != GS_INSET) { half[0] = v; half_n[0] = n; }
    if (halves < 1 || halves > 2 || half[halves - 1] + half_n[halves - 1] != v + n) return false;
    int slot[2][2] = {{CSS_RULE_CAP_START, CSS_RULE_CAP_END}, {CSS_RULE_JUNCTION_START, CSS_RULE_JUNCTION_END}};
    if (what == GS_INSET_JUNCTION) slot[0][0] = CSS_RULE_JUNCTION_START, slot[0][1] = CSS_RULE_JUNCTION_END;
    int most = what == GS_INSET_START || what == GS_INSET_END ? 1 : 2;
    for (int h = 0; h < halves; h++) {
        if (!half_n[h]) return false;
        int k = split(half[h], half_n[h], t, tn, 3, ' ');
        if (k < 1 || k > most || t[k - 1] + tn[k - 1] != half[h] + half_n[h]) return false;
        if (what == GS_INSET_START || what == GS_INSET_END) {
            int a = what == GS_INSET_START ? CSS_RULE_CAP_START : CSS_RULE_CAP_END;
            int b = what == GS_INSET_START ? CSS_RULE_JUNCTION_START : CSS_RULE_JUNCTION_END;
            if (!rule_inset(t[0], tn[0], cx, &inset[a], spec ? &spec[a] : NULL)) return false;
            inset[b] = inset[a];
            if (spec) sb_put(&spec[b], spec[a].p, spec[a].n);
            return true;
        }
        int a = slot[h][0], b = slot[h][1];
        if (!rule_inset(t[0], tn[0], cx, &inset[a], spec ? &spec[a] : NULL) ||
            !rule_inset(t[k - 1], tn[k - 1], cx, &inset[b], spec ? &spec[b] : NULL)) return false;
    }
    if (halves == 1 && what == GS_INSET) { /* the junction values are the cap ones */
        inset[CSS_RULE_JUNCTION_START] = inset[CSS_RULE_CAP_START];
        inset[CSS_RULE_JUNCTION_END] = inset[CSS_RULE_CAP_END];
        if (spec) {
            sb_put(&spec[CSS_RULE_JUNCTION_START], spec[CSS_RULE_CAP_START].p, spec[CSS_RULE_CAP_START].n);
            sb_put(&spec[CSS_RULE_JUNCTION_END], spec[CSS_RULE_CAP_END].p, spec[CSS_RULE_CAP_END].n);
        }
    }
    return true;
}
/* a gap decoration shorthand's value (what: GS_*) into one axis' values */
static bool gap_shorthand_parse(int what, const char *v, size_t n, struct cx *cx, struct css_gap_axis *out, sbuf *spec,
                                sbuf inset_spec[4]) {
    switch (what) {
    case GS_RULE: return rule_lists(RULE_GAP, v, n, cx, out->list, spec);
    case GS_COLOR: return rule_lists(CSS_GAP_COLOR, v, n, cx, out->list, NULL);
    case GS_STYLE: return rule_lists(CSS_GAP_STYLE, v, n, cx, out->list, NULL);
    case GS_WIDTH: return rule_lists(CSS_GAP_WIDTH, v, n, cx, out->list, NULL);
    case GS_BREAK: return kw_find(kw_rule_break, v, n, &out->rule_break);
    case GS_ITEMS: return kw_find(kw_rule_items, v, n, &out->visibility);
    }
    return rule_insets(what, v, n, cx, out->inset, inset_spec);
}
/* a shorthand: the longhands no more important declaration set, written into one copy of the group */
static bool set_long(const char *name, const char *v, size_t n, struct cx *cx);
static bool gap_shorthand_apply(const struct propdef *p, const char *v, size_t n, bool global, struct cx *cx) {
    int what = p->flags & 15;
    if (global) {
        for (int axis = 0; axis < 2; axis++)
            if (p->flags & (axis ? GS_ROW : GS_COLUMN))
                for (int i = 0; i < 4; i++) {
                    const struct propdef *q = gap_longhand(what, axis, i);
                    if (q) set_long(q->name, v, n, cx);
                }
        return true;
    }
    struct css_gap_axis parsed = css_gap_rules_initial.axis[0];
    if (!gap_shorthand_parse(what, v, n, cx, &parsed, NULL, NULL)) return false;
    if (cx->font_pass) return true;
    struct css_gap_rules own = *css_style_gap_rules(cx->s);
    bool written = false;
    for (int axis = 0; axis < 2; axis++) {
        if (!(p->flags & (axis ? GS_ROW : GS_COLUMN))) continue;
        size_t base = offsetof(struct css_gap_rules, axis) + (size_t)axis * sizeof(struct css_gap_axis);
        for (int i = 0; i < 4; i++) {
            const struct propdef *q = gap_longhand(what, axis, i);
            if (!q) continue;
            int at = css_prop_index(q);
            if (cx->set[at]) continue;
            memcpy((char *)&own + q->off, (const char *)&parsed + (q->off - base), q->size);
            cx->set[at] = 1;
            written = true;
        }
    }
    if (written) group_write(cx->s, G_GAP_RULE, &own, cx->a);
    return true;
}

/* ---- serialization */
static void rule_value_text(const style_t *st, int kind, const union css_rule_value *v, bool resolved, sbuf *out) {
    if (kind == CSS_GAP_STYLE) { sb_puts(out, kw_name(kw_bstyle, v->style)); return; }
    if (kind == CSS_GAP_WIDTH) { anim_unit(out, v->width, "px"); return; }
    struct css_color_computed c;
    css_color_held(v->color.argb, v->color.exact, st->color, &c);
    if (!resolved) { anim_color_text(out, &c); return; }
    struct css_color used = anim_color_used(st, &c);
    css_color_serialize(&used, false, out);
}
static void rule_repeat_open(const struct css_rule_item *it, sbuf *out) {
    if (it->repeat == CSS_RULE_AUTO) sb_puts(out, "repeat(auto, ");
    else { char number[24]; snprintf(number, sizeof number, "repeat(%u, ", it->repeat); sb_puts(out, number); }
}
/* a list's computed value; resolved: currentcolor as the used colour (getComputedStyle), else as
   the animation layer reads it back */
static void rule_list_text(const style_t *st, int kind, const struct css_rule_list *l, bool resolved, sbuf *out) {
    for (uint32_t i = 0; i < l->nitems; i++) {
        const struct css_rule_item *it = &l->items[i];
        if (i) sb_puts(out, ", ");
        if (it->repeat) rule_repeat_open(it, out);
        for (uint32_t j = 0; j < it->count; j++) {
            if (j) sb_puts(out, ", ");
            rule_value_text(st, kind, &l->values[it->first + j], resolved, out);
        }
        if (it->repeat) sb_putc(out, ')');
    }
}
static bool rule_inset_text(const struct css_rule_inset *v, sbuf *out, web_doc *d) {
    if (v->join) { sb_puts(out, "overlap-join"); return true; }
    return css_length_serialize(&v->length, out, d);
}
/* §4.8: column-rule and row-rule from their longhands when they are repeater aligned (false: they
   serialize as the empty string) */
static bool rule_triple_text(const style_t *st, const struct css_gap_axis *x, sbuf *out) {
    const struct css_rule_list *w = &x->list[CSS_GAP_WIDTH], *s = &x->list[CSS_GAP_STYLE], *c = &x->list[CSS_GAP_COLOR];
    if (w->nitems != s->nitems || w->nitems != c->nitems) return false;
    for (uint32_t i = 0; i < w->nitems; i++)
        if (w->items[i].repeat != s->items[i].repeat || w->items[i].repeat != c->items[i].repeat ||
            w->items[i].count != s->items[i].count || w->items[i].count != c->items[i].count) return false;
    for (uint32_t i = 0; i < w->nitems; i++) {
        const struct css_rule_item *it = &w->items[i];
        if (i) sb_puts(out, ", ");
        if (it->repeat) rule_repeat_open(it, out);
        for (uint32_t j = 0; j < it->count; j++) {
            if (j) sb_puts(out, ", ");
            rule_value_text(st, CSS_GAP_WIDTH, &w->values[w->items[i].first + j], true, out);
            sb_putc(out, ' ');
            rule_value_text(st, CSS_GAP_STYLE, &s->values[s->items[i].first + j], true, out);
            sb_putc(out, ' ');
            rule_value_text(st, CSS_GAP_COLOR, &c->values[c->items[i].first + j], true, out);
        }
        if (it->repeat) sb_putc(out, ')');
    }
    return true;
}
/* a shorthand's value on one axis: false when its longhands have no common value */
static bool gap_axis_text(const style_t *st, int what, const struct css_gap_axis *x, sbuf *out, web_doc *d) {
    const struct css_rule_inset *in = x->inset;
    switch (what) {
    case GS_RULE: return rule_triple_text(st, x, out);
    case GS_COLOR: rule_list_text(st, CSS_GAP_COLOR, &x->list[CSS_GAP_COLOR], true, out); return true;
    case GS_STYLE: rule_list_text(st, CSS_GAP_STYLE, &x->list[CSS_GAP_STYLE], true, out); return true;
    case GS_WIDTH: rule_list_text(st, CSS_GAP_WIDTH, &x->list[CSS_GAP_WIDTH], true, out); return true;
    case GS_BREAK: sb_puts(out, kw_name(kw_rule_break, x->rule_break)); return true;
    case GS_ITEMS: sb_puts(out, kw_name(kw_rule_items, x->visibility)); return true;
    case GS_INSET_START: case GS_INSET_END: {
        int a = what == GS_INSET_START ? CSS_RULE_CAP_START : CSS_RULE_CAP_END;
        int b = what == GS_INSET_START ? CSS_RULE_JUNCTION_START : CSS_RULE_JUNCTION_END;
        return rule_inset_equal(&in[a], &in[b]) && rule_inset_text(&in[a], out, d);
    }
    case GS_INSET_CAP: case GS_INSET_JUNCTION: {
        int a = what == GS_INSET_CAP ? CSS_RULE_CAP_START : CSS_RULE_JUNCTION_START;
        if (!rule_inset_text(&in[a], out, d)) return false;
        if (rule_inset_equal(&in[a], &in[a + 1])) return true;
        sb_putc(out, ' ');
        return rule_inset_text(&in[a + 1], out, d);
    }
    }
    /* <'column-rule-inset-cap'> [ / <'column-rule-inset-junction'> ]?: one value when the four agree */
    bool same = true;
    for (int i = 1; i < 4; i++) same &= rule_inset_equal(&in[0], &in[i]);
    if (same) return rule_inset_text(&in[0], out, d);
    return rule_inset_text(&in[0], out, d) && (sb_putc(out, ' '), rule_inset_text(&in[1], out, d)) &&
           (sb_puts(out, " / "), rule_inset_text(&in[2], out, d)) && (sb_putc(out, ' '), rule_inset_text(&in[3], out, d));
}
static int gap_computed(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d) {
    const struct css_gap_rules *g = css_style_gap_rules(st);
    size_t begin = out->n;
    if (p->type == PT_RULE_LIST) { rule_list_text(st, p->flags, (const struct css_rule_list *)field_of(st, p), true, out); return 1; }
    if (p->type == PT_RULE_INSET) return rule_inset_text((const struct css_rule_inset *)field_of(st, p), out, d) ? 1 : -1;
    int what = p->flags & 15;
    bool column = p->flags & GS_COLUMN, row = p->flags & GS_ROW, ok = true;
    if (column) ok = gap_axis_text(st, what, &g->axis[0], out, d);
    if (ok && row) {
        if (!column) ok = gap_axis_text(st, what, &g->axis[1], out, d);
        else { /* the bidirectional shorthands: both axes' values, when they agree */
            sbuf other = {0};
            ok = gap_axis_text(st, what, &g->axis[1], &other, d) && other.n == out->n - begin &&
                 !memcmp(other.p, out->p + begin, other.n);
            sb_free(&other);
        }
    }
    if (!ok) { out->n = begin; sb_cstr(out); }
    return 1;
}
/* CSSOM: a declared value's specified serialization; false leaves the authored text */
static bool gap_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    int what = p->type == PT_SHORT ? p->flags & 15 : -1;
    if (p->type != PT_RULE_INSET && what != GS_RULE && what < GS_INSET_START) return false;
    if (memchr(v, '\\', n)) return false;
    for (size_t i = 0; i + 4 <= n; i++) if (strn_ieq(v + i, "var(", 4)) return false;
    arena_t arena = {.chunk_size = 4096};
    jmp_buf trap;
    arena.trap = &trap;
    size_t begin = out->n;
    volatile bool ok = false;
    sbuf spec[4] = {{0}};
    if (!setjmp(trap)) {
        struct cx cx = {.em = 16, .rem = 16, .vw = 800, .vh = 600, .a = &arena};
        struct css_gap_axis parsed = css_gap_rules_initial.axis[0];
        if (p->type == PT_RULE_INSET) ok = rule_inset(v, n, &cx, &parsed.inset[0], out);
        else if (what == GS_RULE) ok = rule_lists(RULE_GAP, v, n, &cx, parsed.list, out);
        else if ((ok = gap_shorthand_parse(what, v, n, &cx, &parsed, NULL, spec))) {
            /* the shortest form of the specified values, as the computed ones' */
            int a = what == GS_INSET_JUNCTION ? CSS_RULE_JUNCTION_START : what == GS_INSET_END ? CSS_RULE_CAP_END : 0;
            int count = what == GS_INSET ? 4 : what == GS_INSET_START || what == GS_INSET_END ? 1 : 2;
            bool same = true;
            for (int i = 1; i < count; i++)
                same &= spec[a + i].n == spec[a].n && !memcmp(spec[a + i].p, spec[a].p, spec[a].n);
            for (int i = 0; i < (same ? 1 : count); i++) {
                if (i) sb_puts(out, i == 2 ? " / " : " ");
                sb_put(out, spec[a + i].p, spec[a + i].n);
            }
        }
    }
    for (int i = 0; i < 4; i++) sb_free(&spec[i]);
    ar_free(&arena);
    if (!ok) out->n = begin;
    sb_cstr(out);
    return ok;
}

/* ---------------------------------------------------------------- anchor positioning */
/* css-anchor-position-1: the values of the anchor group's properties, and anchor() and
   anchor-size() in lengths. Layout (layout.c) finds the anchors and resolves the functions. */
const struct css_anchor css_anchor_initial = {.visibility = CSS_VISIBLE_ANCHOR_VISIBLE};

static bool dashed_ident(const char *s, size_t n) {
    if (n < 3 || s[0] != '-' || s[1] != '-') return false;
    for (size_t i = 2; i < n; i++)
        if ((unsigned char)s[i] < 128 && !isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return false;
    return true;
}
static bool anchor_text(sbuf *out, const char *s) { return length_append(out, s, strlen(s)); }
static bool same_name(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

/* <position-area> keywords (§3.1.2); a keyword is its index here */
static const struct css_area_keyword area_keywords[] = {
    {NULL, 0, 0, 0},
    {"center", 0, CSS_AREA_CENTER, 0}, {"span-all", 0, CSS_AREA_SPAN_ALL, 0},
#define AREA_AXIS(family, writing, start, end, span_start, span_end) \
    {start, family, CSS_AREA_START, writing}, {end, family, CSS_AREA_END, writing}, \
    {span_start, family, CSS_AREA_SPAN_START, writing}, {span_end, family, CSS_AREA_SPAN_END, writing}
    AREA_AXIS(CSS_AREA_X, 0, "left", "right", "span-left", "span-right"),
    AREA_AXIS(CSS_AREA_X, 1, "x-start", "x-end", "span-x-start", "span-x-end"),
    AREA_AXIS(CSS_AREA_X, 2, "self-x-start", "self-x-end", "span-self-x-start", "span-self-x-end"),
    AREA_AXIS(CSS_AREA_Y, 0, "top", "bottom", "span-top", "span-bottom"),
    AREA_AXIS(CSS_AREA_Y, 1, "y-start", "y-end", "span-y-start", "span-y-end"),
    AREA_AXIS(CSS_AREA_Y, 2, "self-y-start", "self-y-end", "span-self-y-start", "span-self-y-end"),
    AREA_AXIS(CSS_AREA_BLOCK, 1, "block-start", "block-end", "span-block-start", "span-block-end"),
    AREA_AXIS(CSS_AREA_INLINE, 1, "inline-start", "inline-end", "span-inline-start", "span-inline-end"),
    AREA_AXIS(CSS_AREA_SELF_BLOCK, 2, "self-block-start", "self-block-end", "span-self-block-start",
              "span-self-block-end"),
    AREA_AXIS(CSS_AREA_SELF_INLINE, 2, "self-inline-start", "self-inline-end", "span-self-inline-start",
              "span-self-inline-end"),
    AREA_AXIS(CSS_AREA_LOGICAL, 1, "start", "end", "span-start", "span-end"),
    AREA_AXIS(CSS_AREA_SELF_LOGICAL, 2, "self-start", "self-end", "span-self-start", "span-self-end"),
#undef AREA_AXIS
};
#define AREA_KEYWORDS (sizeof area_keywords / sizeof *area_keywords)
const struct css_area_keyword *css_area_keyword(uint8_t keyword) {
    return keyword && keyword < AREA_KEYWORDS ? &area_keywords[keyword] : NULL;
}
static uint8_t area_lookup(const char *s, size_t n) {
    for (uint8_t i = 1; i < AREA_KEYWORDS; i++) if (ident_is(s, n, area_keywords[i].name)) return i;
    return 0;
}
static uint8_t area_find(uint8_t family, uint8_t track) {
    if (track == CSS_AREA_CENTER) return 1;
    if (track == CSS_AREA_SPAN_ALL) return 2;
    for (uint8_t i = 3; i < AREA_KEYWORDS; i++)
        if (area_keywords[i].family == family && area_keywords[i].track == track) return i;
    return 0;
}
static uint8_t area_family(uint8_t k) { return k < AREA_KEYWORDS ? area_keywords[k].family : 0; }
static bool area_ambiguous(uint8_t family) {
    return !family || family == CSS_AREA_LOGICAL || family == CSS_AREA_SELF_LOGICAL;
}
/* a <position-area>: written, its keywords in their axes' order; computed, with the ambiguous
   logical keywords made the long ones of their axes (§3.1.3). false: not one. */
static bool parse_position_area(const char *v, size_t n, uint8_t written[2], uint8_t computed[2]) {
    const char *t[3]; size_t tl[3];
    int k = split(v, n, t, tl, 3, ' ');
    if (k < 1 || k > 2) return false;
    uint8_t a = area_lookup(t[0], tl[0]), b = k == 2 ? area_lookup(t[1], tl[1]) : 0;
    if (!a || (k == 2 && !b)) return false;
    uint8_t fa = area_family(a), fb = area_family(b);
    if (k == 1) {
        /* §3.1.2: an unambiguous keyword alone spans the other axis; an ambiguous one is repeated */
        if (area_ambiguous(fa)) b = a;
        else if (fa == CSS_AREA_X || fa == CSS_AREA_BLOCK || fa == CSS_AREA_SELF_BLOCK) b = 2;
        else { b = a; a = 2; }
    } else if (fa && fb) {
        bool pair = (fa == CSS_AREA_X && fb == CSS_AREA_Y) || (fa == CSS_AREA_BLOCK && fb == CSS_AREA_INLINE) ||
            (fa == CSS_AREA_SELF_BLOCK && fb == CSS_AREA_SELF_INLINE);
        bool flipped = (fb == CSS_AREA_X && fa == CSS_AREA_Y) || (fb == CSS_AREA_BLOCK && fa == CSS_AREA_INLINE) ||
            (fb == CSS_AREA_SELF_BLOCK && fa == CSS_AREA_SELF_INLINE);
        if (flipped) { uint8_t x = a; a = b; b = x; }
        else if (!pair && !(fa == fb && (fa == CSS_AREA_LOGICAL || fa == CSS_AREA_SELF_LOGICAL))) return false;
    } else if (fa || fb) {
        /* center or span-all refers to the other keyword's opposite axis */
        uint8_t f = fa ? fa : fb, keyword = fa ? a : b, other = fa ? b : a;
        if (f == CSS_AREA_X || f == CSS_AREA_BLOCK || f == CSS_AREA_SELF_BLOCK) { a = keyword; b = other; }
        else if (f == CSS_AREA_Y || f == CSS_AREA_INLINE || f == CSS_AREA_SELF_INLINE) { a = other; b = keyword; }
    }
    written[0] = a; written[1] = b;
    for (int i = 0; i < 2; i++) {
        uint8_t f = area_family(written[i]), keyword = written[i];
        if (f == CSS_AREA_LOGICAL) keyword = area_find(i ? CSS_AREA_INLINE : CSS_AREA_BLOCK, area_keywords[keyword].track);
        else if (f == CSS_AREA_SELF_LOGICAL)
            keyword = area_find(i ? CSS_AREA_SELF_INLINE : CSS_AREA_SELF_BLOCK, area_keywords[keyword].track);
        computed[i] = keyword;
    }
    return true;
}
/* a written <position-area> (CSSOM's specified value): span-all beside an unambiguous keyword and a
   repeated keyword are left out */
static bool area_written_text(sbuf *out, const uint8_t a[2]) {
    uint8_t fa = area_family(a[0]), fb = area_family(a[1]);
    if (a[0] == 2 && !area_ambiguous(fb)) return anchor_text(out, area_keywords[a[1]].name);
    if (a[1] == 2 && !area_ambiguous(fa)) return anchor_text(out, area_keywords[a[0]].name);
    if (a[0] == a[1]) return anchor_text(out, area_keywords[a[0]].name);
    return anchor_text(out, area_keywords[a[0]].name) && anchor_text(out, " ") && anchor_text(out, area_keywords[a[1]].name);
}
/* a computed one (§3.1.3): logical keywords in their short forms, unless span-all leaves one alone */
static bool area_computed_text(sbuf *out, const uint8_t a[2]) {
    if (!a[0]) return anchor_text(out, "none");
    uint8_t fa = area_family(a[0]), fb = area_family(a[1]);
    if (!fa && !fb) {
        if (a[0] == a[1]) return anchor_text(out, area_keywords[a[0]].name);
    } else if (a[0] == 2) return anchor_text(out, area_keywords[a[1]].name);
    else if (a[1] == 2) return anchor_text(out, area_keywords[a[0]].name);
    uint8_t shown[2] = {a[0], a[1]};
    for (int i = 0; i < 2; i++) {
        uint8_t f = area_family(a[i]);
        if (f == CSS_AREA_BLOCK || f == CSS_AREA_INLINE) shown[i] = area_find(CSS_AREA_LOGICAL, area_keywords[a[i]].track);
        else if (f == CSS_AREA_SELF_BLOCK || f == CSS_AREA_SELF_INLINE)
            shown[i] = area_find(CSS_AREA_SELF_LOGICAL, area_keywords[a[i]].track);
    }
    if (shown[0] == shown[1]) return anchor_text(out, area_keywords[shown[0]].name);
    return anchor_text(out, area_keywords[shown[0]].name) && anchor_text(out, " ") &&
        anchor_text(out, area_keywords[shown[1]].name);
}

/* none | <dashed-ident># (anchor-name; anchor-scope also all) */
static bool parse_anchor_names(const char *v, size_t n, struct cx *cx, bool scope, struct css_anchor_names *out) {
    *out = (struct css_anchor_names){0};
    if (ident_is(v, n, "none")) return true;
    if (scope && ident_is(v, n, "all")) { out->all = true; return true; }
    uint32_t count = 1;
    for (size_t i = 0; i < n; i++) if (v[i] == ',') count++;
    const char **names = ar_alloc(cx->a, count * sizeof *names);
    uint32_t got = 0;
    for (const char *p = v, *e = v + n; got < count; got++) {
        const char *comma = memchr(p, ',', (size_t)(e - p)), *end = comma ? comma : e;
        const char *name = p; size_t length = (size_t)(end - p);
        trim(&name, &length);
        if (!dashed_ident(name, length)) return false;
        names[got] = ar_strndup(cx->a, name, length);
        if (!comma) { got++; break; }
        p = comma + 1;
    }
    if (got != count) return false;
    out->name = names; out->count = count;
    return true;
}
static bool anchor_names_text(sbuf *out, const struct css_anchor_names *names, bool scope) {
    if (scope && names->all) return anchor_text(out, "all");
    if (!names->count) return anchor_text(out, "none");
    for (uint32_t i = 0; i < names->count; i++)
        if ((i && !anchor_text(out, ", ")) || !anchor_text(out, names->name[i])) return false;
    return true;
}
static bool anchor_names_equal(const struct css_anchor_names *a, const struct css_anchor_names *b) {
    if (a->count != b->count || a->all != b->all) return false;
    for (uint32_t i = 0; i < a->count; i++) if (strcmp(a->name[i], b->name[i])) return false;
    return true;
}

/* position-try-fallbacks (§6.1): none | [ [<dashed-ident> || <try-tactic>] | <position-area> ]# */
static const struct kw kw_try_tactic[] = {{"flip-block", CSS_TRY_FLIP_BLOCK}, {"flip-inline", CSS_TRY_FLIP_INLINE},
    {"flip-start", CSS_TRY_FLIP_START}, {"flip-x", CSS_TRY_FLIP_X}, {"flip-y", CSS_TRY_FLIP_Y}, {0}};
static bool parse_try_option(const char *v, size_t n, struct cx *cx, struct css_position_try *out) {
    *out = (struct css_position_try){0};
    uint8_t written[2];
    if (parse_position_area(v, n, written, out->area)) return true;
    const char *t[7]; size_t tl[7];
    int k = split(v, n, t, tl, 7, ' ');
    if (k < 1 || k > 6) return false;
    for (int i = 0; i < k; i++) {
        uint8_t tactic;
        if (dashed_ident(t[i], tl[i])) {
            /* the name before or after the tactics, once */
            if (out->rule || (i && i != k - 1)) return false;
            out->rule = ar_strndup(cx->a, t[i], tl[i]);
            continue;
        }
        if (!kw_find(kw_try_tactic, t[i], tl[i], &tactic)) return false;
        for (int j = 0; j < out->tactics; j++) if (out->tactic[j] == tactic) return false;
        if (out->tactics == 5) return false;
        out->tactic[out->tactics++] = tactic;
    }
    return true;
}
static bool parse_try_fallbacks(const char *v, size_t n, struct cx *cx, struct css_position_tries *out) {
    *out = (struct css_position_tries){0};
    if (ident_is(v, n, "none")) return true;
    uint32_t count = 1;
    for (size_t i = 0; i < n; i++) if (v[i] == ',') count++;
    struct css_position_try *options = ar_alloc(cx->a, count * sizeof *options);
    const char *p = v, *e = v + n;
    for (uint32_t i = 0; i < count; i++) {
        const char *comma = memchr(p, ',', (size_t)(e - p)), *end = comma ? comma : e;
        const char *item = p; size_t length = (size_t)(end - p);
        trim(&item, &length);
        if (!length || !parse_try_option(item, length, cx, &options[i])) return false;
        p = comma ? comma + 1 : e;
    }
    out->option = options; out->count = count;
    return true;
}
static bool try_option_text(sbuf *out, const struct css_position_try *o) {
    if (o->area[0]) return area_computed_text(out, o->area);
    bool space = false;
    if (o->rule) { if (!anchor_text(out, o->rule)) return false; space = true; }
    for (int i = 0; i < o->tactics; i++) {
        if ((space && !anchor_text(out, " ")) || !anchor_text(out, kw_name(kw_try_tactic, o->tactic[i]))) return false;
        space = true;
    }
    return true;
}
static bool try_fallbacks_text(sbuf *out, const struct css_position_tries *t) {
    if (!t->count) return anchor_text(out, "none");
    for (uint32_t i = 0; i < t->count; i++)
        if ((i && !anchor_text(out, ", ")) || !try_option_text(out, &t->option[i])) return false;
    return true;
}
static bool try_option_equal(const struct css_position_try *a, const struct css_position_try *b) {
    return same_name(a->rule, b->rule) && a->tactics == b->tactics &&
        !memcmp(a->tactic, b->tactic, a->tactics) && a->area[0] == b->area[0] && a->area[1] == b->area[1];
}

/* position-visibility (§6.6): always | [ anchor-valid || anchor-visible || no-overflow ] */
static const struct kw kw_visibility_bits[] = {{"anchor-valid", CSS_VISIBLE_ANCHOR_VALID},
    {"anchor-visible", CSS_VISIBLE_ANCHOR_VISIBLE}, {"no-overflow", CSS_VISIBLE_NO_OVERFLOW}, {0}};
static bool parse_position_visibility(const char *v, size_t n, uint8_t *out) {
    if (ident_is(v, n, "always")) { *out = 0; return true; }
    const char *t[4]; size_t tl[4];
    int k = split(v, n, t, tl, 4, ' ');
    if (k < 1 || k > 3) return false;
    uint8_t bits = 0;
    for (int i = 0; i < k; i++) {
        uint8_t bit;
        if (!kw_find(kw_visibility_bits, t[i], tl[i], &bit) || (bits & bit)) return false;
        bits |= bit;
    }
    *out = bits;
    return true;
}
static bool position_visibility_text(sbuf *out, uint8_t bits) {
    if (!bits) return anchor_text(out, "always");
    bool first = true;
    for (const struct kw *k = kw_visibility_bits; k->name; k++)
        if (bits & k->v) {
            if ((!first && !anchor_text(out, " ")) || !anchor_text(out, k->name)) return false;
            first = false;
        }
    return true;
}

/* a PT_ANCHOR value into field (the group's copy); specified: also its CSSOM serialization */
static bool anchor_value_parse(uint8_t which, const char *v, size_t n, struct cx *cx, void *field, sbuf *specified) {
    trim(&v, &n);
    if (!n) return false;
    switch (which) {
    case CSS_ANCHOR_NAME: case CSS_ANCHOR_SCOPE: {
        struct css_anchor_names names;
        if (!parse_anchor_names(v, n, cx, which == CSS_ANCHOR_SCOPE, &names)) return false;
        *(struct css_anchor_names *)field = names;
        return !specified || anchor_names_text(specified, &names, which == CSS_ANCHOR_SCOPE);
    }
    case CSS_POSITION_ANCHOR: {
        static const struct kw kinds[] = {{"normal", CSS_ANCHOR_NORMAL}, {"none", CSS_ANCHOR_NONE},
            {"auto", CSS_ANCHOR_AUTO}, {"match-parent", CSS_ANCHOR_MATCH_PARENT}, {0}};
        struct css_position_anchor anchor = {0};
        if (!kw_find(kinds, v, n, &anchor.kind)) {
            if (!dashed_ident(v, n)) return false;
            anchor.kind = CSS_ANCHOR_NAMED;
            anchor.name = ar_strndup(cx->a, v, n);
        }
        *(struct css_position_anchor *)field = anchor;
        return !specified || anchor_text(specified, anchor.name ? anchor.name : kw_name(kinds, anchor.kind));
    }
    case CSS_POSITION_AREA: {
        uint8_t written[2] = {0}, computed[2] = {0};
        if (!ident_is(v, n, "none") && !parse_position_area(v, n, written, computed)) return false;
        memcpy(field, computed, 2);
        return !specified || (written[0] ? area_written_text(specified, written) : anchor_text(specified, "none"));
    }
    case CSS_POSITION_TRY_FALLBACKS: {
        struct css_position_tries tries;
        if (!parse_try_fallbacks(v, n, cx, &tries)) return false;
        *(struct css_position_tries *)field = tries;
        if (!specified) return true;
        if (!tries.count) return anchor_text(specified, "none");
        /* a <position-area> option serializes as written */
        const char *p = v, *e = v + n;
        for (uint32_t i = 0; i < tries.count; i++) {
            const char *comma = memchr(p, ',', (size_t)(e - p)), *end = comma ? comma : e;
            const char *item = p; size_t length = (size_t)(end - p);
            trim(&item, &length);
            uint8_t written[2], computed[2];
            if (i && !anchor_text(specified, ", ")) return false;
            if (tries.option[i].area[0]) {
                if (!parse_position_area(item, length, written, computed) || !area_written_text(specified, written)) return false;
            } else if (!try_option_text(specified, &tries.option[i])) return false;
            p = comma ? comma + 1 : e;
        }
        return true;
    }
    case CSS_POSITION_VISIBILITY: {
        uint8_t bits;
        if (!parse_position_visibility(v, n, &bits)) return false;
        *(uint8_t *)field = bits;
        return !specified || position_visibility_text(specified, bits);
    }
    }
    return false;
}
static bool anchor_value_text(uint8_t which, const void *field, sbuf *out) {
    switch (which) {
    case CSS_ANCHOR_NAME: case CSS_ANCHOR_SCOPE:
        return anchor_names_text(out, field, which == CSS_ANCHOR_SCOPE);
    case CSS_POSITION_ANCHOR: {
        const struct css_position_anchor *a = field;
        static const char *const kinds[] = {"normal", "none", "auto", NULL, "match-parent"};
        return anchor_text(out, a->kind == CSS_ANCHOR_NAMED ? a->name : kinds[a->kind < 5 ? a->kind : 0]);
    }
    case CSS_POSITION_AREA: return area_computed_text(out, field);
    case CSS_POSITION_TRY_FALLBACKS: return try_fallbacks_text(out, field);
    case CSS_POSITION_VISIBILITY: return position_visibility_text(out, *(const uint8_t *)field);
    }
    return false;
}
static bool anchor_value_equal(uint8_t which, const void *a, const void *b) {
    switch (which) {
    case CSS_ANCHOR_NAME: case CSS_ANCHOR_SCOPE: return anchor_names_equal(a, b);
    case CSS_POSITION_ANCHOR: {
        const struct css_position_anchor *x = a, *y = b;
        return x->kind == y->kind && same_name(x->name, y->name);
    }
    case CSS_POSITION_AREA: return !memcmp(a, b, 2);
    case CSS_POSITION_TRY_FALLBACKS: {
        const struct css_position_tries *x = a, *y = b;
        if (x->count != y->count) return false;
        for (uint32_t i = 0; i < x->count; i++) if (!try_option_equal(&x->option[i], &y->option[i])) return false;
        return true;
    }
    case CSS_POSITION_VISIBILITY: return *(const uint8_t *)a == *(const uint8_t *)b;
    }
    return false;
}
bool css_anchor_equal(const struct css_anchor *a, const struct css_anchor *b) {
    if (!a) a = &css_anchor_initial;
    if (!b) b = &css_anchor_initial;
    return a == b || (anchor_names_equal(&a->name, &b->name) && anchor_names_equal(&a->scope, &b->scope) &&
        anchor_value_equal(CSS_POSITION_ANCHOR, &a->position_anchor, &b->position_anchor) &&
        !memcmp(a->area, b->area, 2) && anchor_value_equal(CSS_POSITION_TRY_FALLBACKS, &a->tries, &b->tries) &&
        a->try_order == b->try_order && a->visibility == b->visibility);
}

/* the position-try shorthand (§6.3): <'position-try-order'>? <'position-try-fallbacks'> */
static bool parse_position_try(const char *v, size_t n, struct cx *cx, uint8_t *order, struct css_position_tries *tries,
                               sbuf *specified) {
    trim(&v, &n);
    *order = CSS_TRY_ORDER_NORMAL;
    size_t first = 0;
    while (first < n && !is_space((unsigned char)v[first]) && v[first] != ',') first++;
    const char *rest = v; size_t rest_n = n;
    if (first < n && is_space((unsigned char)v[first]) && kw_find(kw_try_order, v, first, order)) {
        rest = v + first; rest_n = n - first; trim(&rest, &rest_n);
    }
    if (!rest_n) return false;
    if (!specified) return parse_try_fallbacks(rest, rest_n, cx, tries);
    if (*order != CSS_TRY_ORDER_NORMAL &&
        (!anchor_text(specified, kw_name(kw_try_order, *order)) || !anchor_text(specified, " "))) return false;
    return anchor_value_parse(CSS_POSITION_TRY_FALLBACKS, rest, rest_n, cx, tries, specified);
}

/* anchor() and anchor-size() (§3.2, §5.1) */
static const struct kw kw_anchor_side[] = {{"inside", CSS_SIDE_INSIDE}, {"outside", CSS_SIDE_OUTSIDE},
    {"top", CSS_SIDE_TOP}, {"left", CSS_SIDE_LEFT}, {"right", CSS_SIDE_RIGHT}, {"bottom", CSS_SIDE_BOTTOM},
    {"start", CSS_SIDE_START}, {"end", CSS_SIDE_END}, {"self-start", CSS_SIDE_SELF_START},
    {"self-end", CSS_SIDE_SELF_END}, {"center", CSS_SIDE_CENTER}, {0}};
static const struct kw kw_anchor_size[] = {{"width", CSS_SIZE_WIDTH}, {"height", CSS_SIZE_HEIGHT},
    {"block", CSS_SIZE_BLOCK}, {"inline", CSS_SIZE_INLINE}, {"self-block", CSS_SIZE_SELF_BLOCK},
    {"self-inline", CSS_SIZE_SELF_INLINE}, {0}};
/* an <anchor-side>'s <percentage>: a percentage token, or a math function of type <percentage> */
static bool anchor_percentage(const char *v, size_t n, const struct cx *cx, arena_t *a, float *out, bool *function) {
    const char *q = v;
    float x;
    if (parse_number(&q, v + n, &x) && q + 1 == v + n && *q == '%') { *out = x; *function = false; return true; }
    double value;
    /* against a basis of 100: its value is the percentage's number */
    if (!css_calc_is_math(v, n) || !css_numeric_read(v, n, CSS_CALC_PERCENT, cx, a, 100, &value, NULL) || !isfinite(value))
        return false;
    *out = (float)value;
    *function = true;
    return true;
}
const struct css_anchor_function *css_anchor_function_read(struct css_calc_parse *P, const char **s, const char *e,
                                                           bool size) {
    if (!P || !P->cx || P->cx->anchor_depth > 32) return NULL;
    /* the fallback is a length of the same element, which may hold the same anchor functions */
    struct cx cx = *P->cx;
    cx.a = P->a;
    cx.anchor_functions = P->anchors;
    cx.anchor_depth++;
    const char *begin = *s, *p = begin, *comma = NULL;
    int depth = 0;
    char quote = 0;
    for (; p < e; p++) {
        if (quote) { if (*p == '\\' && p + 1 < e) p++; else if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') quote = *p;
        else if (*p == '(') depth++;
        else if (*p == ')') { if (!depth) break; depth--; }
        else if (*p == ',' && !depth && !comma) comma = p;
    }
    if (p >= e) return NULL;
    *s = p + 1;
    struct css_anchor_function *f = ar_alloc(cx.a, sizeof *f);
    f->size = size;
    const char *head = begin, *fallback = comma ? comma + 1 : NULL;
    size_t head_n = (size_t)((comma ? comma : p) - begin), fallback_n = comma ? (size_t)(p - comma - 1) : 0;
    trim(&head, &head_n);
    const char *t[3]; size_t tl[3];
    int k = head_n ? split(head, head_n, t, tl, 3, ' ') : 0;
    if (k > 2 || (comma && !k)) return NULL;
    uint8_t kw;
    /* anchor-size(<length-percentage>): only the fallback */
    if (size && !comma && k == 1 && !dashed_ident(t[0], tl[0]) && !kw_find(kw_anchor_size, t[0], tl[0], &kw)) {
        fallback = head; fallback_n = head_n; k = 0;
    }
    bool side = false;
    for (int i = 0; i < k; i++) {
        if (dashed_ident(t[i], tl[i]) && !f->name) { f->name = ar_strndup(cx.a, t[i], tl[i]); continue; }
        if (side) return NULL;
        if (size) {
            if (!kw_find(kw_anchor_size, t[i], tl[i], &kw)) return NULL;
            f->side = kw;
        } else if (kw_find(kw_anchor_side, t[i], tl[i], &kw)) f->side = kw;
        else if (anchor_percentage(t[i], tl[i], &cx, cx.a, &f->percent, &f->percent_function)) f->side = CSS_SIDE_PERCENT;
        else return NULL;
        side = true;
    }
    if (!size && !side) return NULL;
    if (fallback) {
        trim(&fallback, &fallback_n);
        if (!fallback_n || !parse_len(fallback, fallback_n, &cx, &f->fallback, LF_NEG | LF_LENGTH | P->anchors))
            return NULL;
        f->fallback_given = true;
    }
    return f;
}
bool css_anchor_function_text(sbuf *out, const struct css_anchor_function *f) {
    if (!f || !anchor_text(out, f->size ? "anchor-size(" : "anchor(")) return false;
    bool any = false;
    if (f->name) { if (!anchor_text(out, f->name)) return false; any = true; }
    if (!f->size || f->side != CSS_SIZE_IMPLIED) {
        if (any && !anchor_text(out, " ")) return false;
        if (f->size) { if (!anchor_text(out, kw_name(kw_anchor_size, f->side))) return false; }
        else if (f->side != CSS_SIDE_PERCENT) { if (!anchor_text(out, kw_name(kw_anchor_side, f->side))) return false; }
        else if ((f->percent_function && !anchor_text(out, "calc(")) || !length_number(out, f->percent, "%") ||
                 (f->percent_function && !anchor_text(out, ")"))) return false;
        any = true;
    }
    if (f->fallback_given && ((any && !anchor_text(out, ", ")) || !css_length_serialize(&f->fallback, out, NULL))) return false;
    return anchor_text(out, ")");
}
bool css_anchor_function_equal(const struct css_anchor_function *a, const struct css_anchor_function *b) {
    if (a == b) return true;
    if (!a || !b || a->size != b->size || a->side != b->side || a->percent != b->percent ||
        a->percent_function != b->percent_function || !same_name(a->name, b->name) ||
        a->fallback_given != b->fallback_given) return false;
    return !a->fallback_given || css_length_equal(&a->fallback, &b->fallback);
}
bool css_length_anchored(const len_t *l) { return l && l->kind == LK_EXPR && css_calc_anchored(l->expr); }
/* css_length_resolve_anchors: each anchor function as resolve gives it, else its fallback (whose own
   anchor functions resolve the same way) */
struct anchor_resolution { css_anchor_resolver resolve; void *context; unsigned axis; int inset; unsigned depth; };
static struct css_calc *anchor_resolved(void *context, arena_t *a, const struct css_anchor_function *f) {
    struct anchor_resolution *r = context;
    float px;
    if (r->resolve && r->resolve(r->context, f, r->axis, r->inset, &px)) {
        len_t value = {.kind = LK_LEN, .px = px};
        return css_calc_length(a, &value);
    }
    if (!f->fallback_given || r->depth > 32) return NULL;
    if (f->fallback.kind != LK_EXPR) return css_calc_length(a, &f->fallback);
    r->depth++;
    struct css_calc *c = css_calc_map_anchors(a, f->fallback.expr, anchor_resolved, r);
    r->depth--;
    return c;
}
bool css_length_resolve_anchors(arena_t *a, const len_t *l, css_anchor_resolver resolve, void *context,
                                unsigned axis, int inset, len_t *out) {
    if (!css_length_anchored(l)) { *out = *l; return true; }
    struct anchor_resolution r = {resolve, context, axis, inset, 0};
    struct css_calc *c = css_calc_map_anchors(a, l->expr, anchor_resolved, &r);
    return c && css_length_from_calc(c, true, out);
}
void css_style_anchor_fallbacks(style_t *s, arena_t *a) {
    if (s->position == POS_ABSOLUTE || s->position == POS_FIXED) return;
    props_init();
    for (int i = 0; i < NPROPS; i++) {
        const struct propdef *p = &props[i];
        if (p->type != PT_LEN || p->group != G_STYLE || !(p->flags & (LF_ANCHOR | LF_ANCHOR_SIZE))) continue;
        len_t *field = (len_t *)((char *)s + p->off);
        if (field->kind != LK_EXPR || !css_length_anchored(field)) continue;
        len_t used;
        /* invalid at computed-value time: the property's initial value (it is not inherited) */
        if (!css_length_resolve_anchors(a, field, NULL, NULL, 0, -1, &used)) used = *(const len_t *)((const char *)&initial + p->off);
        *field = used;
    }
}

/* css_length_own: an anchor function copied with its name and fallback */
static struct css_calc *anchor_copied(void *context, arena_t *a, const struct css_anchor_function *f) {
    (void)context;
    struct css_anchor_function *g = ar_alloc(a, sizeof *g);
    *g = *f;
    if (g->name) g->name = ar_strdup(a, g->name);
    if (g->fallback_given) css_length_own(a, &g->fallback);
    return css_calc_anchor(a, g);
}
void css_length_own(arena_t *a, len_t *l) {
    if (l->kind != LK_EXPR || !l->expr) return;
    struct css_calc *c = css_calc_map_anchors(a, l->expr, anchor_copied, NULL);
    if (c) l->expr = c;
}

/* the accepted @position-try properties (§6.4) */
bool css_prop_position_try_accepted(const struct propdef *p) {
    static const char *const names[] = {"top", "right", "bottom", "left", "inset", "inset-block", "inset-inline",
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left", "margin-block", "margin-inline",
        "width", "height", "min-width", "min-height", "max-width", "max-height", "align-self", "justify-self",
        "place-self", "position-anchor", "position-area"};
    if (!p) return false;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(p->name, names[i])) return true;
    return false;
}
bool css_position_area_text(const uint8_t area[2], sbuf *out) { return area_computed_text(out, area); }
void css_style_position_option(style_t *used, const style_t *option, struct css_anchor *anchor) {
    props_init();
    *anchor = *css_style_anchor(used);
    const struct css_anchor *from = css_style_anchor(option);
    for (int i = 0; i < NPROPS; i++) {
        const struct propdef *p = &props[i];
        if (p->type == PT_SHORT || !css_prop_position_try_accepted(p)) continue;
        if (p->group == G_STYLE) memcpy((char *)used + p->off, (const char *)option + p->off, p->size);
        else if (p->group == G_ANCHOR) memcpy((char *)anchor + p->off, (const char *)from + p->off, p->size);
    }
    used->anchor = anchor;
}
/* §6.5.2 a try tactic on anchor functions: across one axis (flipped: the property is in that axis) the
   sides and percentages mirror; flip-start swaps the axes, so physical sides and sizes trade places */
static const struct css_anchor_function *function_tactic(arena_t *a, const struct css_anchor_function *f,
                                                         uint8_t tactic, bool flipped);
struct anchor_tactic { uint8_t tactic; bool flipped; };
static struct css_calc *anchor_flipped(void *context, arena_t *a, const struct css_anchor_function *f) {
    const struct anchor_tactic *t = context;
    return css_calc_anchor(a, function_tactic(a, f, t->tactic, t->flipped));
}
static void length_tactic(arena_t *a, len_t *l, uint8_t tactic, bool flipped) {
    if (!css_length_anchored(l)) return;
    struct anchor_tactic t = {tactic, flipped};
    struct css_calc *c = css_calc_map_anchors(a, l->expr, anchor_flipped, &t);
    if (c) l->expr = c;
}
static const struct css_anchor_function *function_tactic(arena_t *a, const struct css_anchor_function *f,
                                                         uint8_t tactic, bool flipped) {
    static const uint8_t start_sides[][2] = {{CSS_SIDE_TOP, CSS_SIDE_LEFT}, {CSS_SIDE_BOTTOM, CSS_SIDE_RIGHT}};
    static const uint8_t axis_sides[][2] = {{CSS_SIDE_TOP, CSS_SIDE_BOTTOM}, {CSS_SIDE_LEFT, CSS_SIDE_RIGHT},
        {CSS_SIDE_START, CSS_SIDE_END}, {CSS_SIDE_SELF_START, CSS_SIDE_SELF_END}};
    static const uint8_t sizes[][2] = {{CSS_SIZE_WIDTH, CSS_SIZE_HEIGHT}, {CSS_SIZE_BLOCK, CSS_SIZE_INLINE},
        {CSS_SIZE_SELF_BLOCK, CSS_SIZE_SELF_INLINE}};
    struct css_anchor_function g = *f;
    const uint8_t (*pairs)[2] = NULL;
    size_t count = 0;
    if (f->size) { if (tactic == CSS_TRY_FLIP_START) { pairs = sizes; count = 3; } }
    else if (tactic == CSS_TRY_FLIP_START) { pairs = start_sides; count = 2; }
    else if (flipped) {
        pairs = axis_sides; count = 4;
        if (f->side == CSS_SIDE_PERCENT) g.percent = 100 - f->percent;
    }
    for (size_t i = 0; i < count; i++) {
        if (f->side == pairs[i][0]) g.side = pairs[i][1];
        else if (f->side == pairs[i][1]) g.side = pairs[i][0];
    }
    if (f->fallback_given) length_tactic(a, &g.fallback, tactic, flipped);
    if (g.side == f->side && g.percent == f->percent &&
        (!f->fallback_given || g.fallback.expr == f->fallback.expr)) return f;
    struct css_anchor_function *r = ar_alloc(a, sizeof *r);
    *r = g;
    return r;
}
/* a computed <position-area>'s keyword's physical axis (1: vertical): the first of an ambiguous pair is
   the block axis's */
static unsigned area_axis(const uint8_t area[2], int i) {
    uint8_t f = area_family(area[i]), other = area_family(area[!i]);
    if (f == CSS_AREA_X || f == CSS_AREA_INLINE || f == CSS_AREA_SELF_INLINE) return 0;
    if (f == CSS_AREA_Y || f == CSS_AREA_BLOCK || f == CSS_AREA_SELF_BLOCK) return 1;
    if (other) return !area_axis(area, !i);
    return i == 0;
}
static void area_tactic(uint8_t area[2], uint8_t tactic) {
    if (!area[0]) return;
    if (tactic == CSS_TRY_FLIP_START) {
        static const uint8_t swap[][2] = {{CSS_AREA_X, CSS_AREA_Y}, {CSS_AREA_BLOCK, CSS_AREA_INLINE},
                                          {CSS_AREA_SELF_BLOCK, CSS_AREA_SELF_INLINE}};
        uint8_t next[2];
        for (int i = 0; i < 2; i++) {
            uint8_t k = area[i], f = area_family(k), g = f;
            for (int j = 0; j < 3; j++) { if (f == swap[j][0]) g = swap[j][1]; else if (f == swap[j][1]) g = swap[j][0]; }
            next[!i] = g == f ? k : area_find(g, area_keywords[k].track);
        }
        area[0] = next[0]; area[1] = next[1];
        return;
    }
    unsigned axis = tactic == CSS_TRY_FLIP_X || tactic == CSS_TRY_FLIP_INLINE ? 0 : 1;
    uint8_t original[2] = {area[0], area[1]};
    for (int i = 0; i < 2; i++) {
        if (area_axis(original, i) != axis || !area_family(original[i])) continue;
        uint8_t track = area_keywords[original[i]].track;
        uint8_t mirrored = track == CSS_AREA_START ? CSS_AREA_END : track == CSS_AREA_END ? CSS_AREA_START :
            track == CSS_AREA_SPAN_START ? CSS_AREA_SPAN_END : track == CSS_AREA_SPAN_END ? CSS_AREA_SPAN_START : track;
        area[i] = area_find(area_family(original[i]), mirrored);
    }
}
static void swap_len(len_t *x, len_t *y) { len_t t = *x; *x = *y; *y = t; }
/* §6.5.2 execute a try tactic on s's accepted @position-try properties, in horizontal-tb ltr (the
   logical tactics are the physical ones): flip-x mirrors across a vertical line, flip-y across a
   horizontal one, flip-start across the start-start to end-end diagonal */
void css_style_try_tactic(style_t *s, uint8_t tactic, struct css_anchor *anchor, arena_t *a) {
    if (anchor != s->anchor) { *anchor = *css_style_anchor(s); s->anchor = anchor; }
    if (tactic == CSS_TRY_FLIP_START) {
        swap_len(&s->inset[0], &s->inset[3]); swap_len(&s->inset[2], &s->inset[1]);
        swap_len(&s->margin[0], &s->margin[3]); swap_len(&s->margin[2], &s->margin[1]);
        swap_len(&s->width, &s->height); swap_len(&s->min_width, &s->min_height); swap_len(&s->max_width, &s->max_height);
        uint8_t align = s->align_self; s->align_self = s->justify_self; s->justify_self = align;
        len_t *all[] = {&s->inset[0], &s->inset[1], &s->inset[2], &s->inset[3], &s->margin[0], &s->margin[1],
                        &s->margin[2], &s->margin[3], &s->width, &s->height, &s->min_width, &s->min_height,
                        &s->max_width, &s->max_height};
        for (size_t i = 0; i < sizeof all / sizeof *all; i++) length_tactic(a, all[i], tactic, false);
    } else {
        bool x = tactic == CSS_TRY_FLIP_X || tactic == CSS_TRY_FLIP_INLINE;
        int from = x ? 3 : 0, to = x ? 1 : 2; /* left and right, or top and bottom */
        swap_len(&s->inset[from], &s->inset[to]); swap_len(&s->margin[from], &s->margin[to]);
        for (int i = 0; i < 4; i++) length_tactic(a, &s->inset[i], tactic, i == from || i == to);
        uint8_t *self = x ? &s->justify_self : &s->align_self;
        if (*self == AI_START) *self = AI_END;
        else if (*self == AI_END) *self = AI_START;
    }
    area_tactic(anchor->area, tactic);
}
/* FNV-1a over the computed values a fallback-sensitive change (§6.5.1) can change */
static uint64_t position_hash_style(uint64_t h, const style_t *s, bool options, sbuf *t) {
    h = (h ^ s->position) * 1099511628211ull;
    for (int i = 0; i < NPROPS; i++) {
        const struct propdef *p = &props[i];
        bool tries = p->group == G_ANCHOR && (p->off == offsetof(struct css_anchor, tries) ||
                                              p->off == offsetof(struct css_anchor, try_order));
        if (p->type == PT_SHORT || (!css_prop_position_try_accepted(p) && !(options && tries))) continue;
        t->n = 0;
        if (!css_computed_value(s, p, t, NULL)) continue;
        for (size_t j = 0; j < t->n; j++) h = (h ^ (unsigned char)t->p[j]) * 1099511628211ull;
        h = (h ^ 0xff) * 1099511628211ull;
    }
    if (options) {
        const struct css_position_tries *tries = &css_style_anchor(s)->tries;
        for (uint32_t i = 0; i < tries->count; i++)
            h = tries->option[i].style ? position_hash_style(h, tries->option[i].style, false, t) :
                (h ^ (tries->option[i].rule ? 1 : 2)) * 1099511628211ull;
    }
    return h;
}
uint64_t css_style_position_hash(const style_t *s) {
    props_init();
    sbuf t = {0};
    uint64_t h = position_hash_style(1469598103934665603ull, s, true, &t);
    sb_free(&t);
    return h;
}
/* whether the text has a dimension in a unit of the element's font, the viewport or a container
   (outside names and strings) */
static bool relative_unit_mentioned(const char *v, size_t n) {
    const char *p = v, *e = v + n;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\'') {
            for (p++; p < e && *p != (char)c; p++) if (*p == '\\' && p + 1 < e) p++;
            if (p < e) p++;
            continue;
        }
        unsigned char before = p > v ? (unsigned char)p[-1] : ' ';
        if (before < 128 && !isalnum(before) && before != '-' && before != '_') {
            const char *q = p, *u;
            double x;
            size_t un;
            if (css_numeric_token(&q, e, &x, &u, &un) && q > p) {
                if (un && css_unit_relative(css_unit_find(u, un))) return true;
                p = q;
                continue;
            }
        }
        p++;
    }
    return false;
}
/* CSSOM's specified value of one length of an anchored value. Parsing computes relative lengths, so
   an anchored length with one, like a length without an anchor function, has its math functions
   serialized as other lengths' (css_calc_specified) and the rest as written */
static bool anchor_length_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t scratch = {0};
        struct cx cx = {.s = &scratch, .a = &a, .supports_probe = true, .em = 16, .rem = 16};
        len_t l;
        size_t begin = out->n;
        trim(&v, &n);
        unsigned accept = CSS_CALC_LENGTH | (p->flags & LF_NOPCT ? 0 : CSS_CALC_PERCENT);
        ok = parse_len(v, n, &cx, &l, p->flags) && !cx.invalid &&
            (css_length_anchored(&l) && !relative_unit_mentioned(v, n) ? css_length_serialize(&l, out, NULL)
                                                                       : css_calc_specified(v, n, accept, out) ||
                                                                             length_append(out, v, n));
        if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    }
    ar_free(&a);
    return ok;
}
static bool anchor_mentioned(const char *v, size_t n) {
    for (size_t i = 0; i + 7 <= n; i++) if (strn_ieq(v + i, "anchor(", 7) || strn_ieq(v + i, "anchor-", 7)) return true;
    return false;
}
static bool position_try_text(const struct css_anchor *a, sbuf *out) {
    if (a->try_order != CSS_TRY_ORDER_NORMAL &&
        (!anchor_text(out, kw_name(kw_try_order, a->try_order)) || !anchor_text(out, " "))) return false;
    return try_fallbacks_text(out, &a->tries);
}
/* CSSOM's specified value of the anchor group's properties, the position-try shorthand, and the
   lengths and shorthands of lengths that hold an anchor function; false keeps the authored text */
static bool anchor_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    if (p->type == PT_KW) return false;
    if ((p->type == PT_LEN || p->type == PT_SHORT) && p->sh != SH_POSITION_TRY && !anchor_mentioned(v, n)) return false;
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    size_t begin = out->n;
    if (!setjmp(trap)) {
        style_t scratch = {0};
        struct cx cx = {.s = &scratch, .a = &a, .supports_probe = true};
        if (p->type == PT_ANCHOR) {
            union { struct css_anchor_names names; struct css_position_anchor anchor; struct css_position_tries tries;
                    uint8_t bytes[2]; } field;
            ok = anchor_value_parse(p->flags, v, n, &cx, &field, out) && !cx.invalid;
        } else if (p->type == PT_SHORT && p->sh == SH_POSITION_TRY) {
            uint8_t order;
            struct css_position_tries tries;
            ok = parse_position_try(v, n, &cx, &order, &tries, out) && !cx.invalid;
        } else if (p->type == PT_LEN) ok = anchor_length_specified(p, v, n, out);
        else {
            /* a shorthand of lengths: each of its values */
            bool margin = p->sh == SH_MARGIN || p->sh == SH_MARGIN_BLOCK || p->sh == SH_MARGIN_INLINE;
            const struct propdef *longhand = find_prop(margin ? "margin-top" : "top", margin ? 10 : 3);
            const char *t[5]; size_t tl[5];
            int max = p->sh == SH_MARGIN || p->sh == SH_INSET ? 4 : 2;
            int k = split(v, n, t, tl, max + 1, ' ');
            ok = longhand && k >= 1 && k <= max;
            for (int i = 0; ok && i < k; i++) ok = (!i || anchor_text(out, " ")) && anchor_length_specified(longhand, t[i], tl[i], out);
        }
    }
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    ar_free(&a);
    return ok;
}

static bool parse_display(const char *s, size_t n, uint8_t *out, struct cx *cx) {
    const char *t[3];
    size_t tl[3];
    int k = split_checked(s, n, t, tl, 3, ' ', cx);
    if (k == 1) {
        if (cx->supports_probe && (ident_is(s,n,"run-in") || ident_is(s,n,"ruby") ||
            ident_is(s,n,"ruby-base") || ident_is(s,n,"ruby-text") ||
            ident_is(s,n,"-moz-box"))) return false;
        static const struct kw kd[] = {
            {"none", D_NONE}, {"inline", D_INLINE}, {"block", D_BLOCK}, {"list-item", D_LIST_ITEM},
            {"inline-block", D_INLINE_BLOCK}, {"table", D_TABLE}, {"inline-table", D_INLINE_TABLE},
            {"table-row-group", D_TABLE_ROW_GROUP}, {"table-header-group", D_TABLE_HEADER_GROUP},
            {"table-footer-group", D_TABLE_FOOTER_GROUP}, {"table-row", D_TABLE_ROW}, {"table-cell", D_TABLE_CELL},
            {"table-column", D_TABLE_COLUMN}, {"table-column-group", D_TABLE_COLUMN_GROUP},
            {"table-caption", D_TABLE_CAPTION}, {"flex", D_FLEX}, {"inline-flex", D_INLINE_FLEX},
            {"grid", D_GRID}, {"inline-grid", D_INLINE_GRID}, {"contents", D_CONTENTS}, {"flow-root", D_FLOW_ROOT},
            {"run-in", D_BLOCK}, {"-webkit-box", D_BLOCK}, {"-moz-box", D_BLOCK}, {"-webkit-inline-box", D_INLINE_BLOCK},
            {"-webkit-flex", D_FLEX}, {"-ms-flexbox", D_FLEX}, {"-webkit-inline-flex", D_INLINE_FLEX},
            {"-ms-grid", D_GRID}, {"ruby", D_INLINE}, {"ruby-text", D_INLINE}, {"ruby-base", D_INLINE}, {0}};
        return kw_find(kd, t[0], tl[0], out);
    }
    /* two-value syntax: outer and inner display, and list-item */
    bool inl = false, block = false, li = false;
    int inner = -1;
    for (int i = 0; i < k; i++) {
        if (cx->supports_probe && inner >= 0 && (ident_is(t[i],tl[i],"flow") || ident_is(t[i],tl[i],"flow-root") ||
            ident_is(t[i],tl[i],"flex") || ident_is(t[i],tl[i],"grid") || ident_is(t[i],tl[i],"table"))) return false;
        if (ident_is(t[i], tl[i], "inline")) { if (cx->supports_probe && (inl || block)) return false; inl = true; }
        else if (ident_is(t[i], tl[i], "block")) { if (cx->supports_probe && (inl || block)) return false; block = true; }
        else if (ident_is(t[i], tl[i], "list-item")) { if (cx->supports_probe && li) return false; li = true; }
        else if (ident_is(t[i], tl[i], "flow")) inner = 0;
        else if (ident_is(t[i], tl[i], "flow-root")) inner = 1;
        else if (ident_is(t[i], tl[i], "flex")) inner = 2;
        else if (ident_is(t[i], tl[i], "grid")) inner = 3;
        else if (ident_is(t[i], tl[i], "table")) inner = 4;
        else return false;
    }
    if (inl && block) return false;
    if (cx->supports_probe && li && inner > 1) return false;
    if (li) *out = D_LIST_ITEM;
    else if (inner == 2) *out = inl ? D_INLINE_FLEX : D_FLEX;
    else if (inner == 3) *out = inl ? D_INLINE_GRID : D_GRID;
    else if (inner == 4) *out = inl ? D_INLINE_TABLE : D_TABLE;
    else if (inner == 1) *out = inl ? D_INLINE_BLOCK : D_FLOW_ROOT;
    else *out = inl ? D_INLINE : D_BLOCK;
    return true;
}

static const float font_kw_px[] = {9, 10, 13, 16, 18, 24, 32, 48};
float css_font_keyword_px(unsigned keyword) {
    return font_kw_px[keyword < sizeof font_kw_px / sizeof *font_kw_px ? keyword : 3];
}

static bool parse_font_size(const char *s, size_t n, struct cx *cx, float *out) {
    static const char *const kws[] = {"xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
    for (int i = 0; i < 8; i++)
        if (ident_is(s, n, kws[i])) {
            *out = font_kw_px[i];
            return true;
        }
    float pfs = cx->parent ? cx->parent->font_size : 16;
    if (ident_is(s, n, "smaller")) {
        *out = pfs / 1.2f;
        return true;
    }
    if (ident_is(s, n, "larger")) {
        *out = pfs * 1.2f;
        return true;
    }
    if (ident_is(s, n, "math")) {
        *out = pfs;
        return true;
    }
    len_t l;
    if (!parse_len(s, n, cx, &l, 0)) return false;
    float v = len_resolve(&l, pfs);
    if (v < 0) return false;
    *out = v;
    return true;
}

static bool parse_font_weight(const char *s, size_t n, struct cx *cx, uint16_t *out) {
    int pw = cx->parent ? cx->parent->font_weight : 400;
    if (ident_is(s, n, "normal")) *out = 400;
    else if (ident_is(s, n, "bold")) *out = 700;
    else if (ident_is(s, n, "bolder")) *out = pw < 350 ? 400 : pw < 550 ? 700 : 900;
    else if (ident_is(s, n, "lighter")) *out = pw < 550 ? 100 : pw < 750 ? 400 : 700;
    else if (css_calc_is_math(s, n)) {
        /* a calculation clamps to [1, 1000] (§10.12) */
        double v;
        if (!css_numeric_read(s, n, CSS_CALC_NUMBER, cx, cx->a, NAN, &v, NULL)) return false;
        *out = (uint16_t)(v < 1 ? 1 : v > 1000 ? 1000 : v);
    } else {
        const char *p = s;
        float v;
        if (!parse_number(&p, s + n, &v) || p != s + n || v < 1 || v > 1000) return false;
        *out = (uint16_t)v;
    }
    return true;
}

const char *css_font_family_next(arena_t *a,const char **cursor,const char *end) {
    const char *s=*cursor;while(s<end && is_space((unsigned char)*s))s++;
    const char *begin=s,*finish=s;bool quoted=s<end && (*s=='"' || *s=='\'');
    if(quoted) {
        char quote=*s++;begin=s;
        while(s<end && *s!=quote) {if(*s=='\\' && s+1<end)s++;s++;}
        if(s==end)goto bad;finish=s++;
        while(s<end && is_space((unsigned char)*s))s++;
        if(s<end && *s!=',')goto bad;
    } else {
        while(s<end && *s!=',') {
            if(*s=='\\') {s++;if(s==end || *s=='\n' || *s=='\r' || *s=='\f')goto bad;
                if(isxdigit((unsigned char)*s)){unsigned n=0;while(s<end && n<6 && isxdigit((unsigned char)*s)){s++;n++;}if(s<end && is_space((unsigned char)*s))s++;}
                else s++;
                continue;
            }
            if(!isalnum((unsigned char)*s) && *s!='-' && *s!='_' && (unsigned char)*s<128 && !is_space((unsigned char)*s))goto bad;
            if((s==begin || is_space((unsigned char)s[-1])) && isdigit((unsigned char)*s))goto bad;
            s++;
        }
        finish=s;while(finish>begin && is_space((unsigned char)finish[-1]))finish--;
    }
    /* A <string> family name may be empty; a <custom-ident>+ one may not. */
    if(!quoted && finish==begin)goto bad;
    sbuf b={0};css_unescape(&b,begin,(size_t)(finish-begin));
    if(!quoted) {
        size_t write=0;bool space=false;
        for(size_t i=0;i<b.n;i++)if(is_space((unsigned char)b.p[i]))space=true;
        else {if(space && write)b.p[write++]=' ';b.p[write++]=b.p[i];space=false;}
        b.n=write;
    }
    const char *name=ar_strndup(a,b.p?b.p:"",b.n);sb_free(&b);
    /* <custom-ident> excludes the CSS-wide keywords and default (CSS Values 4). */
    if(!quoted && (!strcasecmp(name,"inherit") || !strcasecmp(name,"initial") || !strcasecmp(name,"unset") ||
       !strcasecmp(name,"revert") || !strcasecmp(name,"revert-layer") || !strcasecmp(name,"default")))goto bad;
    if(s<end){s++;const char *tail=s;while(tail<end && is_space((unsigned char)*tail))tail++;if(tail==end)goto bad;}
    *cursor=s;return name;
bad:*cursor=NULL;return NULL;
}

static bool choose_family(const char *s, size_t n, uint8_t *out, struct cx *cx) {
    const char *cursor=s,*end=s+n;bool any=false,chosen=false;
    *out=FONT_FAMILY_SERIF;
    static const char *const mono[] = {"monospace", "ui-monospace", "courier", "courier new", "consolas",
                                       "menlo", "monaco", "sfmono-regular", "sf mono", "fira code", "fira mono",
                                       "source code pro", "dejavu sans mono", "liberation mono", "roboto mono",
                                       "jetbrains mono", "lucida console", "andale mono", "ubuntu mono",
                                       "cascadia code", "cascadia mono", "ibm plex mono", "noto sans mono",
                                       "inconsolata", "hack", "go mono", "lucida sans typewriter",
                                       "maple mono", "maple mono nf", "maple mono nf cn", NULL};
    /* Named families in these lists use the bundled class substitute; this is
       not a claim that Arial/Times or downloaded webfonts are installed. */
    static const char *const serif[] = {"serif", "ui-serif", "georgia", "times", "times new roman", "noto serif", NULL};
    static const char *const sans[] = {"sans-serif", "system-ui", "-apple-system", "blinkmacsystemfont",
                                       "ui-sans-serif", "arial", "helvetica", "helvetica neue",
                                       "inter", "roboto", "segoe ui", "verdana",
                                       "tahoma", "cursive", "fantasy", "noto sans", "open sans",
                                       "ubuntu", "cantarell", "lato", NULL};
    while(cursor<end) {
        const char *f=css_font_family_next(cx->a,&cursor,end);
        if(!f || !cursor)return false;any=true;size_t fl=strlen(f);
        if(chosen)continue;
        for(int j=0;mono[j];j++)if(ident_is(f,fl,mono[j])){*out=FONT_FAMILY_MONO;chosen=true;break;}
        if(chosen)continue;
        for(int j=0;serif[j];j++)if(ident_is(f,fl,serif[j])){*out=FONT_FAMILY_SERIF;chosen=true;break;}
        if(chosen)continue;
        for(int j=0;sans[j];j++)if(ident_is(f,fl,sans[j])){*out=FONT_FAMILY_SANS;chosen=true;break;}
    }
    return any;

}

/* url(...) contents, unescaped */
const char *css_parse_url(const char *s, size_t n, arena_t *a) {
    if (n < 5 || !strn_ieq(s, "url(", 4) || s[n - 1] != ')') {
        if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) { /* @import "x" */
            sbuf b = {0};
            css_unescape(&b, s + 1, n - 2);
            char *r = ar_strndup(a, b.p ? b.p : "", b.n);
            sb_free(&b);
            return r;
        }
        return NULL;
    }
    s += 4, n -= 5;
    trim(&s, &n);
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) s++, n -= 2;
    sbuf b = {0};
    css_unescape(&b, s, n);
    char *r = ar_strndup(a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

/* background-position: one or two values (keywords, lengths, percentages); the first layer only */
static bool parse_bg_pos(const char *s, size_t n, struct cx *cx, len_t *out) {
    const char *layer[8];
    size_t ll[8];
    int layers = split_checked(s, n, layer, ll, 8, ',', cx);
    if (layers < 1 || (cx->supports_probe && layers != 1)) return false;
    const char *t[4];
    size_t tl[4];
    int k = split_checked(layer[0], ll[0], t, tl, 4, ' ', cx);
    if (k < 1 || k > 4) return false;
    len_t v[2];
    memset(v, 0, sizeof v);
    v[0].kind = v[1].kind = LK_LEN;
    v[0].pct = v[1].pct = 50;
    bool have[2] = {false, false};
    int pending = -1; /* a length after a keyword offsets from that side */
    /* "center" takes the axis the other keywords leave free: center right, top center */
    bool hx = false, hy = false;
    for (int i = 0; i < k; i++) {
        hx |= ident_is(t[i], tl[i], "left") || ident_is(t[i], tl[i], "right");
        hy |= ident_is(t[i], tl[i], "top") || ident_is(t[i], tl[i], "bottom");
    }
    for (int i = 0; i < k; i++) {
        int axis = -1;
        float pct = -1;
        if (ident_is(t[i], tl[i], "left")) axis = 0, pct = 0;
        else if (ident_is(t[i], tl[i], "right")) axis = 0, pct = 100;
        else if (ident_is(t[i], tl[i], "top")) axis = 1, pct = 0;
        else if (ident_is(t[i], tl[i], "bottom")) axis = 1, pct = 100;
        else if (ident_is(t[i], tl[i], "center")) axis = -2, pct = 50;
        if (pct >= 0) {
            if (axis == -2) axis = hx && !hy ? 1 : hy && !hx ? 0 : !have[0] ? 0 : 1;
            if (have[axis]) axis = !axis;
            v[axis].px = 0;
            v[axis].pct = pct;
            have[axis] = true;
            pending = axis;
            continue;
        }
        len_t l;
        if (!parse_len(t[i], tl[i], cx, &l, LF_NEG)) return false;
        if (l.kind != LK_LEN) l.px = len_resolve(&l, 0), l.pct = 0, l.kind = LK_LEN;
        int a = pending >= 0 && k > 2 ? pending : (!have[0] ? 0 : 1);
        if (pending >= 0 && k > 2) {
            if (v[a].pct == 100) l.px = -l.px, l.pct = 100 - l.pct;
            else l.pct += v[a].pct;
        }
        v[a] = l;
        have[a] = true;
        pending = -1;
    }
    out[0] = v[0];
    out[1] = v[1];
    return true;
}

/* a <bg-size> (css-backgrounds-3 §3.9): cover, contain, or a width and a height (one: the height
   auto), each a non-negative <length-percentage> or auto */
static bool layer_size_parse(const char *s, size_t n, struct cx *cx, struct css_layer_size *out) {
    memset(out, 0, sizeof *out);
    if (ident_is(s, n, "cover")) { out->kind = BSZ_COVER; return true; }
    if (ident_is(s, n, "contain")) { out->kind = BSZ_CONTAIN; return true; }
    const char *t[3];
    size_t tl[3];
    int k = split(s, n, t, tl, 3, ' ');
    if (k < 1 || k > 2) return false;
    for (int i = 0; i < k; i++)
        if (!parse_len(t[i], tl[i], cx, &out->v[i], LF_AUTO)) return false;
    out->kind = out->v[0].kind == LK_AUTO && out->v[1].kind == LK_AUTO ? BSZ_AUTO : BSZ_LEN;
    return true;
}
/* background-size: its first layer's */
static bool parse_bg_size(const char *s, size_t n, struct cx *cx, len_t *size, uint8_t *kind) {
    const char *layer[8];
    size_t ll[8];
    int layers = split_checked(s, n, layer, ll, 8, ',', cx);
    if (layers < 1 || (cx->supports_probe && layers != 1)) return false;
    struct css_layer_size z;
    trim(&layer[0], &ll[0]);
    if (!layer_size_parse(layer[0], ll[0], cx, &z)) return false;
    memcpy(size, z.v, sizeof z.v);
    *kind = z.kind;
    return true;
}

/* ---------------------------------------------------------------- border-radius lists and clip (shape.c owns clip-path) */
/* A <border-radius> value: one to four horizontal radii and, after a /, one to four vertical ones,
   expanded to each axis's four corners (TL TR BR BL). */
static bool parse_radius_list(const char *v, size_t n, struct cx *cx, len_t lengths[2][4]) {
    const char *axes[3];size_t sizes[3];int count=split_checked(v,n,axes,sizes,3,'/',cx);
    if(count<1||count>2||!sizes[0]||(count==2&&!sizes[1]))return false;
    const char *consumed=axes[count-1]+sizes[count-1];while(consumed<v+n&&is_space((unsigned char)*consumed))consumed++;
    if(consumed!=v+n)return false;
    static const int pick[5][4]={{0},{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
    for(int axis=0;axis<count;axis++) {
        const char *tokens[5];size_t lengths_n[5];int k=split_checked(axes[axis],sizes[axis],tokens,lengths_n,5,' ',cx);
        if(k<1||k>4)return false;
        len_t values[4];for(int i=0;i<k;i++)if(!parse_len(tokens[i],lengths_n[i],cx,&values[i],LF_LENGTH))return false;
        for(int i=0;i<4;i++)lengths[axis][i]=values[pick[k][i]];
    }
    if(count==1)memcpy(lengths[1],lengths[0],sizeof lengths[0]);
    return true;
}
/* name(arguments): the arguments, NULL for any other token */
static const char *function_args(const char *v,size_t n,const char *name,size_t *size) {
    size_t k=strlen(name);
    if(n<k+2||!strn_ieq(v,name,k)||v[k]!='('||v[n-1]!=')')return NULL;
    *size=n-k-2;return v+k+1;
}
/* clip: auto | rect(<top>, <right>, <bottom>, <left>), each a length or auto; CSS 2.1's spaces still parse */
static bool parse_clip_rect(const char *v,size_t n,struct cx *cx,struct css_clip_rect *out) {
    trim(&v,&n);
    struct css_clip_rect r;memset(&r,0,sizeof r);
    if(ident_is(v,n,"auto")){*out=r;return true;}
    size_t size;const char *args=function_args(v,n,"rect",&size);
    if(!args)return false;
    const char *t[5];size_t tl[5];int k=split_checked(args,size,t,tl,5,',',cx);
    if(k==1)k=split_checked(args,size,t,tl,5,' ',cx);
    if(k!=4)return false;
    r.rect=true;
    for(int i=0;i<4;i++) {
        if(ident_is(t[i],tl[i],"auto")){r.auto_edges|=(uint8_t)(1u<<i);continue;}
        len_t edge;
        if(!parse_len(t[i],tl[i],cx,&edge,LF_NEG|LF_LENGTH|LF_NOPCT)||edge.kind!=LK_LEN)return false;
        r.edge[i]=edge.px;
    }
    *out=r;return true;
}

static bool transform_angle(const char *s, size_t n, struct cx *cx, float *out);
/* linear-gradient(), radial-gradient() and their repeating- forms; NULL if it is not one */
static const struct gradient *parse_gradient(const char *s, size_t n, struct cx *cx) {
    size_t fl = 0;
    while (fl < n && s[fl] != '(') fl++;
    if (fl >= n || s[n - 1] != ')') return NULL;
    const char *name = s;
    size_t nl = fl;
    if (nl > 8 && strn_ieq(name, "-webkit-", 8)) name += 8, nl -= 8;
    struct gradient g;
    memset(&g, 0, sizeof g);
    g.angle = 180;
    if (nl > 10 && strn_ieq(name, "repeating-", 10)) {
        g.repeating = true;
        name += 10;
        nl -= 10;
    }
    if (ident_is(name, nl, "radial-gradient")) g.radial = true;
    else if (!ident_is(name, nl, "linear-gradient")) return NULL;
    g.at[0].kind = g.at[1].kind = LK_LEN;
    g.at[0].pct = g.at[1].pct = 50;
    const char *a[GRAD_MAX + 2];
    size_t al[GRAD_MAX + 2];
    int m = split_checked(s + fl + 1, n - fl - 2, a, al, GRAD_MAX + 2, ',', cx);
    for (int j = 0; j < m; j++) {
        const char *c[8];
        size_t cl[8];
        int q = split_checked(a[j], al[j], c, cl, 8, ' ', cx);
        uint32_t col;
        const struct css_color_value *exact;
        if (q >= 1 && css_color_value_compute(c[0], cl[0], cx, NULL, &col, &exact)) { /* a colour stop: colour [pos [pos]] */
            if (cx->supports_probe && q > 3) return NULL;
            for (int z = 0; z < 2 && g.n < GRAD_MAX; z++) {
                if (z == 1 && q < 3) break;
                g.col[g.n] = col;
                g.col_value[g.n] = col == COLOR_CURRENT ? exact : NULL;
                g.pos[g.n].kind = LK_AUTO;
                if (q > 1 + z && !parse_len(c[1 + z], cl[1 + z], cx, &g.pos[g.n], 0)) {
                    if (cx->supports_probe) return NULL;
                    g.pos[g.n].kind = LK_AUTO;
                }
                g.n++;
            }
            continue;
        }
        if (q == 1 && g.n && j < m - 1) { /* a colour hint: ignored */
            len_t hint;
            if (parse_len(c[0], cl[0], cx, &hint, 0)) { if (cx->supports_probe) return NULL; continue; }
        }
        if (j) return NULL;
        /* the first argument: direction or shape */
        if (!g.radial) {
            float deg;
            if (q >= 2 && ident_is(c[0], cl[0], "to")) {
                for (int z = 1; z < q; z++) {
                    if (ident_is(c[z], cl[z], "left")) g.to_x = -1;
                    else if (ident_is(c[z], cl[z], "right")) g.to_x = 1;
                    else if (ident_is(c[z], cl[z], "top")) g.to_y = -1;
                    else if (ident_is(c[z], cl[z], "bottom")) g.to_y = 1;
                    else return NULL;
                }
                if (!g.to_x || !g.to_y) { /* a side is just an angle */
                    g.angle = g.to_y < 0 ? 0 : g.to_x > 0 ? 90 : g.to_y > 0 ? 180 : 270;
                    g.to_x = g.to_y = 0;
                }
            } else if (q == 1 && transform_angle(c[0], cl[0], cx, &deg)) g.angle = deg * 180 / (float)M_PI;
            else return NULL;
            continue;
        }
        for (int z = 0; z < q; z++) {
            if (ident_is(c[z], cl[z], "circle")) g.circle = true;
            else if (ident_is(c[z], cl[z], "ellipse")) g.circle = false;
            else if (ident_is(c[z], cl[z], "closest-side")) g.rsize = RG_CLOSEST_SIDE;
            else if (ident_is(c[z], cl[z], "farthest-side")) g.rsize = RG_FARTHEST_SIDE;
            else if (ident_is(c[z], cl[z], "closest-corner")) g.rsize = RG_CLOSEST_CORNER;
            else if (ident_is(c[z], cl[z], "farthest-corner")) g.rsize = RG_FARTHEST_CORNER;
            else if (ident_is(c[z], cl[z], "at") && z + 1 < q) {
                const char *p0 = c[z + 1];
                if (!parse_bg_pos(p0, (size_t)(a[j] + al[j] - p0), cx, g.at)) return NULL;
                break;
            } else {
                len_t l; /* an explicit size: approximated by the default */
                if (cx->supports_probe) return NULL;
                if (!parse_len(c[z], cl[z], cx, &l, 0)) return NULL;
            }
        }
    }
    if (g.n < 1 || (cx->supports_probe && g.n < 2)) return NULL;
    if (g.n == 1) { /* one stop: a solid colour */
        g.col[1] = g.col[0];
        g.pos[1].kind = LK_AUTO;
        g.n = 2;
    }
    struct gradient *r = ar_alloc(cx->a, sizeof *r);
    *r = g;
    return r;
}

/* ---------------------------------------------------------------- layered backgrounds, border-image and mask-border */
/* the groups' initial values (css-backgrounds-3 §2.2-§3.7, §6.1-§6.5; css-backgrounds-4 §3.2;
   css-masking-1 §8.1-§8.6; compositing-1 §9.1) */
static const uint8_t bg_keyword_initial[CSS_BG_LISTS] = {CSS_BG_SCROLL, CSS_BOX_BORDER, CSS_BOX_PADDING, CSS_BLEND_NORMAL};
static const struct css_bg_offset bg_offset_initial = {{.kind = LK_LEN}, CSS_BG_EDGE_START, true};
const struct css_background css_background_initial = {
    {{&bg_keyword_initial[0], 1}, {&bg_keyword_initial[1], 1}, {&bg_keyword_initial[2], 1}, {&bg_keyword_initial[3], 1}},
    {{&bg_offset_initial, 1}, {&bg_offset_initial, 1}}};
#define BI_NUMBER(v) {{.kind = LK_LEN}, v, CSS_BI_NUMBER}
#define BI_PERCENT(v) {{.kind = LK_LEN, .pct = v}, 0, CSS_BI_LENGTH}
#define BI_AUTO {{.kind = LK_AUTO}, 0, CSS_BI_AUTO}
#define BI_FOUR(x) {x, x, x, x}
const struct css_border_image css_border_image_initial = {
    {NULL, NULL}, {BI_FOUR(BI_PERCENT(100)), false}, BI_FOUR(BI_NUMBER(1)), BI_FOUR(BI_NUMBER(0)),
    {CSS_BI_STRETCH, CSS_BI_STRETCH}, CSS_MASK_BORDER_ALPHA};
const struct css_border_image css_mask_border_initial = {
    {NULL, NULL}, {BI_FOUR(BI_NUMBER(0)), false}, BI_FOUR(BI_AUTO), BI_FOUR(BI_NUMBER(0)),
    {CSS_BI_STRETCH, CSS_BI_STRETCH}, CSS_MASK_BORDER_ALPHA};
static bool bg_offset_equal(const struct css_bg_offset *a, const struct css_bg_offset *b) {
    return a->edge == b->edge && a->percent == b->percent && css_length_equal(&a->offset, &b->offset);
}
static bool bg_offsets_equal(const struct css_bg_offsets *a, const struct css_bg_offsets *b) {
    if (a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) if (!bg_offset_equal(&a->v[i], &b->v[i])) return false;
    return true;
}
static bool bg_keywords_equal(const struct css_bg_keywords *a, const struct css_bg_keywords *b) {
    return a->n == b->n && !memcmp(a->v, b->v, a->n);
}
bool css_background_equal(const struct css_background *a, const struct css_background *b) {
    if (!a) a = &css_background_initial;
    if (!b) b = &css_background_initial;
    if (a == b) return true;
    for (int l = 0; l < CSS_BG_LISTS; l++) if (!bg_keywords_equal(&a->list[l], &b->list[l])) return false;
    return bg_offsets_equal(&a->position[0], &b->position[0]) && bg_offsets_equal(&a->position[1], &b->position[1]);
}
static bool bi_side_equal(const struct css_bi_side *a, const struct css_bi_side *b) {
    return a->kind == b->kind &&
           (a->kind == CSS_BI_AUTO || (a->kind == CSS_BI_NUMBER ? a->number == b->number : css_length_equal(&a->length, &b->length)));
}
static bool bi_sides_equal(const struct css_bi_side *a, const struct css_bi_side *b) {
    for (int i = 0; i < 4; i++) if (!bi_side_equal(&a[i], &b[i])) return false;
    return true;
}
static bool bi_source_equal(const struct css_bi_source *a, const struct css_bi_source *b) {
    return same_name(a->url, b->url) && a->gradient == b->gradient;
}
bool css_border_image_equal(const struct css_border_image *a, const struct css_border_image *b, bool mask) {
    const struct css_border_image *initial_value = mask ? &css_mask_border_initial : &css_border_image_initial;
    if (!a) a = initial_value;
    if (!b) b = initial_value;
    if (a == b) return true;
    return bi_source_equal(&a->source, &b->source) && a->slice.fill == b->slice.fill &&
           bi_sides_equal(a->slice.side, b->slice.side) && bi_sides_equal(a->width, b->width) &&
           bi_sides_equal(a->outset, b->outset) && a->repeat[0] == b->repeat[0] && a->repeat[1] == b->repeat[1] &&
           a->mode == b->mode;
}
/* the mask layers' initial values (css-masking-1 §7.1-§7.8, §9.2) */
static const uint8_t mask_keyword_initial[CSS_MASK_LISTS] = {CSS_MASK_MATCH_SOURCE, CSS_REPEAT << 2 | CSS_REPEAT,
                                                             CSS_BOX_BORDER, CSS_BOX_BORDER, CSS_COMPOSITE_ADD};
static const struct css_image *const mask_image_none = NULL;
static const struct css_layer_size layer_size_auto = {{{.kind = LK_AUTO}, {.kind = LK_AUTO}}, BSZ_AUTO};
const struct css_mask css_mask_initial = {
    {&mask_image_none, 1},
    {{&mask_keyword_initial[0], 1}, {&mask_keyword_initial[1], 1}, {&mask_keyword_initial[2], 1},
     {&mask_keyword_initial[3], 1}, {&mask_keyword_initial[4], 1}},
    {{&bg_offset_initial, 1}, {&bg_offset_initial, 1}}, {&layer_size_auto, 1}, CSS_MASK_TYPE_LUMINANCE};
static bool image_equal(const struct css_image *a, const struct css_image *b);
static bool layer_images_equal(const struct css_layer_images *a, const struct css_layer_images *b) {
    if (a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) if (!image_equal(a->v[i], b->v[i])) return false;
    return true;
}
static bool layer_size_equal(const struct css_layer_size *a, const struct css_layer_size *b) {
    return a->kind == b->kind &&
           (a->kind != BSZ_LEN || (css_length_equal(&a->v[0], &b->v[0]) && css_length_equal(&a->v[1], &b->v[1])));
}
static bool layer_sizes_equal(const struct css_layer_sizes *a, const struct css_layer_sizes *b) {
    if (a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) if (!layer_size_equal(&a->v[i], &b->v[i])) return false;
    return true;
}
bool css_mask_equal(const struct css_mask *a, const struct css_mask *b) {
    if (!a) a = &css_mask_initial;
    if (!b) b = &css_mask_initial;
    if (a == b) return true;
    for (int l = 0; l < CSS_MASK_LISTS; l++) if (!bg_keywords_equal(&a->list[l], &b->list[l])) return false;
    return a->type == b->type && layer_images_equal(&a->image, &b->image) && layer_sizes_equal(&a->size, &b->size) &&
           bg_offsets_equal(&a->position[0], &b->position[0]) && bg_offsets_equal(&a->position[1], &b->position[1]);
}
bool css_mask_layers_set(const struct css_mask *m) {
    for (uint32_t i = 0; i < m->image.n; i++) if (m->image.v[i]) return true;
    return false;
}
/* an offset from the start side: a logical end's complement */
static len_t bg_offset_from_start(const struct css_bg_offset *o, arena_t *a) {
    if (o->edge != CSS_BG_EDGE_LOGICAL_END) return o->offset;
    if (o->offset.kind == LK_LEN) return (len_t){.kind = LK_LEN, .px = -o->offset.px, .pct = 100 - o->offset.pct};
    len_t r = o->offset;
    if (a) css_length_complement(a, &o->offset, NULL, &r);
    return r;
}
float css_bg_offset_px(const struct css_bg_offsets *l, uint32_t layer, float range) {
    const struct css_bg_offset *o = &l->v[layer % l->n];
    float v = len_resolve(&o->offset, range);
    return o->edge == CSS_BG_EDGE_LOGICAL_END ? range - v : v;
}

/* the tokens of a value split at whitespace, a top-level '/' a token of its own (the background,
   border-image and mask-border shorthands); -1 when there are more than max */
static int slash_tokens(const char *s, size_t n, const char **t, size_t *tl, int max) {
    int k = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && is_space((unsigned char)s[i])) i++;
        if (i >= n) break;
        if (k == max) return -1;
        size_t start = i;
        if (s[i] == '/') { t[k] = s + i; tl[k++] = 1; i++; continue; }
        int depth = 0;
        char q = 0;
        for (; i < n; i++) {
            char c = s[i];
            if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (depth <= 0 && (is_space((unsigned char)c) || c == '/')) break;
        }
        if (i > n) i = n;
        t[k] = s + start;
        tl[k++] = i - start;
    }
    return k;
}
static bool slash_at(const char *const *t, const size_t *tl, int i) { return tl[i] == 1 && t[i][0] == '/'; }
/* the items of a comma-separated list in arena storage, and their count: 0 when one is empty */
static int list_split(const char *s, size_t n, struct cx *cx, const char ***t, size_t **tl) {
    int count = 1, depth = 0;
    char q = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && depth <= 0) { if (count == 1 << 30) return 0; count++; }
    }
    *t = ar_alloc(cx->a, (size_t)count * sizeof **t);
    *tl = ar_alloc(cx->a, (size_t)count * sizeof **tl);
    if (split(s, n, *t, *tl, count, ',') != count) return 0;
    for (int i = 0; i < count; i++) if (!(*tl)[i]) return 0;
    return count;
}

/* <bg-position> (css-backgrounds-3 §3.6) and background-position-x/-y (css-backgrounds-4 §3.2) */
enum { BP_NONE, BP_LEFT, BP_RIGHT, BP_TOP, BP_BOTTOM, BP_CENTER, BP_X_START, BP_X_END, BP_Y_START, BP_Y_END };
static const struct kw kw_bg_edge[] = {{"left", BP_LEFT}, {"right", BP_RIGHT}, {"top", BP_TOP}, {"bottom", BP_BOTTOM},
    {"center", BP_CENTER}, {"x-start", BP_X_START}, {"x-end", BP_X_END}, {"y-start", BP_Y_START}, {"y-end", BP_Y_END}, {0}};
/* one axis of a position: a keyword (BP_NONE: none) and the offset written after it, or alone */
struct bg_part { uint8_t kw; const char *off; size_t off_n; };
static int bp_axis(uint8_t kw) { /* 0 horizontal, 1 vertical, -1 either (center) */
    return kw == BP_CENTER ? -1 : kw == BP_LEFT || kw == BP_RIGHT || kw == BP_X_START || kw == BP_X_END ? 0 : 1;
}
static bool bp_length(const char *t, size_t n, struct cx *cx) {
    len_t l;
    return parse_len(t, n, cx, &l, LF_NEG | LF_LENGTH);
}
/* tokens t[0..k) of a <bg-position>, or (three false) of a <position> (CSS Values 4 §8.3: no
   three-value form): its horizontal and vertical parts */
static bool bg_position_parts(const char *const *t, const size_t *tl, int k, bool three, struct cx *cx,
                              struct bg_part part[2]) {
    memset(part, 0, 2 * sizeof *part);
    if (k < 1 || k > 4 || (k == 3 && !three)) return false;
    uint8_t kw[4];
    bool len[4];
    for (int i = 0; i < k; i++) {
        kw[i] = BP_NONE;
        len[i] = false;
        if (kw_find(kw_bg_edge, t[i], tl[i], &kw[i])) { if (kw[i] >= BP_X_START) return false; }
        else if (bp_length(t[i], tl[i], cx)) len[i] = true;
        else return false;
    }
    if (k == 1) { /* the other axis is center */
        if (len[0]) { part[0] = (struct bg_part){BP_NONE, t[0], tl[0]}; part[1].kw = BP_CENTER; return true; }
        int a = bp_axis(kw[0]);
        part[a < 0 ? 0 : a].kw = kw[0];
        part[a <= 0 ? 1 : 0].kw = BP_CENTER;
        return true;
    }
    if (k == 2) {
        if (!len[0] && !len[1]) { /* two keywords, in either order */
            int a0 = bp_axis(kw[0]), a1 = bp_axis(kw[1]);
            if (a0 >= 0 && a0 == a1) return false;
            int x = a0 == 1 || a1 == 0 ? 1 : 0;
            part[0].kw = kw[x];
            part[1].kw = kw[!x];
            return true;
        }
        /* a horizontal value then a vertical one, one of them a length */
        if ((!len[0] && bp_axis(kw[0]) == 1) || (!len[1] && bp_axis(kw[1]) == 0)) return false;
        for (int i = 0; i < 2; i++)
            part[i] = len[i] ? (struct bg_part){BP_NONE, t[i], tl[i]} : (struct bg_part){kw[i], NULL, 0};
        return true;
    }
    /* three or four values: two keywords, each but center perhaps with an offset, in either order */
    struct bg_part groups[2];
    int g = 0;
    for (int i = 0; i < k;) {
        if (len[i] || g == 2) return false;
        groups[g] = (struct bg_part){kw[i], NULL, 0};
        if (i + 1 < k && len[i + 1]) {
            if (kw[i] == BP_CENTER) return false;
            groups[g].off = t[i + 1];
            groups[g].off_n = tl[i + 1];
            i += 2;
        } else i++;
        g++;
    }
    if (g != 2) return false;
    int a0 = bp_axis(groups[0].kw), a1 = bp_axis(groups[1].kw);
    if (a0 >= 0 && a0 == a1) return false;
    int x = a0 == 1 || a1 == 0 ? 1 : 0;
    part[0] = groups[x];
    part[1] = groups[!x];
    return true;
}
/* a part's computed offset: from the start side, or the logical side it names */
static bool bg_part_offset(const struct bg_part *part, struct cx *cx, struct css_bg_offset *out) {
    len_t l = {.kind = LK_LEN};
    bool percent = true;
    if (part->off) {
        if (!parse_len(part->off, part->off_n, cx, &l, LF_NEG | LF_LENGTH)) return false;
        percent = part->off_n && part->off[part->off_n - 1] == '%';
    }
    out->edge = CSS_BG_EDGE_START;
    switch (part->kw) {
    case BP_CENTER: l = (len_t){.kind = LK_LEN, .pct = 50}; percent = true; break;
    case BP_RIGHT: case BP_BOTTOM: {
        len_t from = l;
        css_length_complement(cx->a, &from, NULL, &l);
        percent = true;
        break;
    }
    case BP_X_START: case BP_Y_START: out->edge = CSS_BG_EDGE_LOGICAL_START; break;
    case BP_X_END: case BP_Y_END: out->edge = CSS_BG_EDGE_LOGICAL_END; break;
    }
    out->offset = l;
    out->percent = percent;
    return true;
}
/* an item of background-position-x (axis 0) or -y: [ center | [ edge ]? <length-percentage>? ]! */
static bool bg_offset_item(int axis, const char *v, size_t n, struct cx *cx, struct bg_part *part) {
    const char *t[3];
    size_t tl[3];
    int k = split(v, n, t, tl, 3, ' ');
    *part = (struct bg_part){0};
    if (k < 1 || k > 2) return false;
    if (kw_find(kw_bg_edge, t[0], tl[0], &part->kw)) {
        int a = bp_axis(part->kw);
        if (a >= 0 && a != axis) return false;
        if (k == 2) {
            if (part->kw == BP_CENTER || !bp_length(t[1], tl[1], cx)) return false;
            part->off = t[1];
            part->off_n = tl[1];
        }
        return true;
    }
    if (k != 1 || !bp_length(t[0], tl[0], cx)) return false;
    part->off = t[0];
    part->off_n = tl[0];
    return true;
}
static bool parse_bg_offsets(int axis, const char *v, size_t n, struct cx *cx, struct css_bg_offsets *out) {
    const char **t;
    size_t *tl;
    int k = list_split(v, n, cx, &t, &tl);
    if (!k) return false;
    struct css_bg_offset *o = ar_alloc(cx->a, (size_t)k * sizeof *o);
    for (int i = 0; i < k; i++) {
        struct bg_part part;
        if (!bg_offset_item(axis, t[i], tl[i], cx, &part) || !bg_part_offset(&part, cx, &o[i])) return false;
    }
    *out = (struct css_bg_offsets){o, (uint32_t)k};
    return true;
}
/* background-position (<bg-position>#) or mask-position (three false: <position>#): the horizontal
   and vertical lists of its layers */
static bool parse_bg_positions(const char *v, size_t n, bool three, struct cx *cx, struct css_bg_offsets out[2]) {
    const char **item;
    size_t *il;
    int k = list_split(v, n, cx, &item, &il);
    if (!k) return false;
    struct css_bg_offset *o[2] = {ar_alloc(cx->a, (size_t)k * sizeof *o[0]), ar_alloc(cx->a, (size_t)k * sizeof *o[1])};
    for (int i = 0; i < k; i++) {
        const char *t[5];
        size_t tl[5];
        struct bg_part part[2];
        int m = split(item[i], il[i], t, tl, 5, ' ');
        if (!bg_position_parts(t, tl, m, three, cx, part) || !bg_part_offset(&part[0], cx, &o[0][i]) ||
            !bg_part_offset(&part[1], cx, &o[1][i])) return false;
    }
    for (int a = 0; a < 2; a++) out[a] = (struct css_bg_offsets){o[a], (uint32_t)k};
    return true;
}
/* background-attachment, -clip, -origin, -blend-mode: a keyword per layer (clip: or border-area || text) */
static bool parse_bg_keywords(const struct propdef *p, const char *v, size_t n, struct cx *cx, struct css_bg_keywords *out) {
    const char **t;
    size_t *tl;
    int k = list_split(v, n, cx, &t, &tl);
    if (!k) return false;
    uint8_t *values = ar_alloc(cx->a, (size_t)k);
    for (int i = 0; i < k; i++) {
        const char *w[3];
        size_t wl[3];
        int m = split(t[i], tl[i], w, wl, 3, ' ');
        if (m == 1 && kw_find(p->kws, w[0], wl[0], &values[i])) continue;
        uint8_t a, b;
        if (p->kws == kw_bg_clip && m == 2 && kw_find(p->kws, w[0], wl[0], &a) && kw_find(p->kws, w[1], wl[1], &b) &&
            a != b && (a | b) == (CSS_BG_CLIP_AREA | CSS_BG_CLIP_TEXT)) {
            values[i] = a | b;
            continue;
        }
        return false;
    }
    *out = (struct css_bg_keywords){values, (uint32_t)k};
    return true;
}
/* the background group's longhands (bit i of which: css_bg_list i, then position -x and -y) that no
   more important declaration set, written from parsed into one copy of the group */
static const char *const bg_group_names[CSS_BG_LISTS + 2] = {"background-attachment", "background-clip", "background-origin",
                                                              "background-blend-mode", "background-position-x",
                                                              "background-position-y"};
static void bg_group_write(struct cx *cx, const struct css_background *parsed, unsigned which) {
    if (cx->font_pass) return;
    struct css_background own = *css_style_background(cx->s);
    bool written = false;
    for (int i = 0; i < CSS_BG_LISTS + 2; i++) {
        if (!(which >> i & 1)) continue;
        int at = css_prop_index(find_prop(bg_group_names[i], strlen(bg_group_names[i])));
        if (cx->set[at]) continue;
        if (i < CSS_BG_LISTS) own.list[i] = parsed->list[i];
        else own.position[i - CSS_BG_LISTS] = parsed->position[i - CSS_BG_LISTS];
        cx->set[at] = 1;
        written = true;
    }
    if (written) group_write(cx->s, G_BACKGROUND, &own, cx->a);
}

/* border-image-* and mask-border-* (css-backgrounds-3 §6, css-masking-1 §8) */
/* the mask layers' longhands (css-masking-1 §7.1-§7.7): mask-mode, -clip, -origin and -composite are
   keyword lists (parse_bg_keywords), mask-position <position>s (parse_bg_positions) */
/* a <repeat-style> (css-backgrounds-3 §3.4): repeat-x, repeat-y, or one or two of repeat, space, round
   and no-repeat, the horizontal one first (one: both) */
static bool repeat_style_parse(const char *s, size_t n, uint8_t *out) {
    const char *t[3];
    size_t tl[3];
    int k = split(s, n, t, tl, 3, ' ');
    if (k == 1 && ident_is(t[0], tl[0], "repeat-x")) { *out = CSS_REPEAT << 2 | CSS_REPEAT_NO; return true; }
    if (k == 1 && ident_is(t[0], tl[0], "repeat-y")) { *out = CSS_REPEAT_NO << 2 | CSS_REPEAT; return true; }
    uint8_t x, y;
    if (k < 1 || k > 2 || !kw_find(kw_repeat_axis, t[0], tl[0], &x)) return false;
    y = x;
    if (k == 2 && !kw_find(kw_repeat_axis, t[1], tl[1], &y)) return false;
    *out = (uint8_t)(x << 2 | y);
    return true;
}
/* its serialization (CSSOM: repeat-x and repeat-y where they apply, a pair once when the same) */
static void repeat_style_text(uint8_t r, sbuf *out) {
    uint8_t x = r >> 2 & 3, y = r & 3;
    if (x == CSS_REPEAT && y == CSS_REPEAT_NO) { sb_puts(out, "repeat-x"); return; }
    if (x == CSS_REPEAT_NO && y == CSS_REPEAT) { sb_puts(out, "repeat-y"); return; }
    sb_puts(out, kw_name(kw_repeat_axis, x));
    if (y != x) { sb_putc(out, ' '); sb_puts(out, kw_name(kw_repeat_axis, y)); }
}
static bool parse_repeat_styles(const char *v, size_t n, struct cx *cx, struct css_bg_keywords *out) {
    const char **t;
    size_t *tl;
    int k = list_split(v, n, cx, &t, &tl);
    if (!k) return false;
    uint8_t *values = ar_alloc(cx->a, (size_t)k);
    for (int i = 0; i < k; i++) if (!repeat_style_parse(t[i], tl[i], &values[i])) return false;
    *out = (struct css_bg_keywords){values, (uint32_t)k};
    return true;
}
static bool parse_layer_sizes(const char *v, size_t n, struct cx *cx, struct css_layer_sizes *out) {
    const char **t;
    size_t *tl;
    int k = list_split(v, n, cx, &t, &tl);
    if (!k) return false;
    struct css_layer_size *z = ar_alloc(cx->a, (size_t)k * sizeof *z);
    for (int i = 0; i < k; i++) {
        trim(&t[i], &tl[i]);
        if (!layer_size_parse(t[i], tl[i], cx, &z[i])) return false;
    }
    *out = (struct css_layer_sizes){z, (uint32_t)k};
    return true;
}
static bool layer_size_text(const struct css_layer_size *z, sbuf *out, web_doc *d) {
    if (z->kind == BSZ_COVER) { sb_puts(out, "cover"); return true; }
    if (z->kind == BSZ_CONTAIN) { sb_puts(out, "contain"); return true; }
    if (z->kind == BSZ_AUTO) { sb_puts(out, "auto"); return true; }
    for (int i = 0; i < 2; i++) {
        if (i) sb_putc(out, ' ');
        if (z->v[i].kind == LK_AUTO) sb_puts(out, "auto");
        else if (!css_length_serialize(&z->v[i], out, d)) return false;
    }
    return true;
}
/* a <mask-reference> (css-masking-1 §7.1): 1 an <image> or a <mask-source> (a url(), which names a
   mask element or an image), 0 none, -1 another image function (accepted outside @supports, not
   drawn), -2 not one */
static bool parse_image_value(const char *s, size_t n, struct cx *cx, const struct css_image **out);
static int mask_reference_parse(const char *t, size_t n, struct cx *cx, const struct css_image **out) {
    *out = NULL;
    if (ident_is(t, n, "none")) return 0;
    if (parse_image_value(t, n, cx, out)) return 1;
    size_t open = 0;
    while (open < n && t[open] != '(') open++;
    return open && open < n && t[n - 1] == ')' && !strn_ieq(t, "url(", 4) ? -1 : -2;
}
static bool parse_mask_images(const char *v, size_t n, struct cx *cx, struct css_layer_images *out) {
    const char **t;
    size_t *tl;
    int k = list_split(v, n, cx, &t, &tl);
    if (!k) return false;
    const struct css_image **items = ar_alloc(cx->a, (size_t)k * sizeof *items);
    for (int i = 0; i < k; i++) {
        const char *w[2];
        size_t wl[2];
        if (split(t[i], tl[i], w, wl, 2, ' ') != 1) return false;
        int r = mask_reference_parse(w[0], wl[0], cx, &items[i]);
        if (r == -2 || (r == -1 && cx->supports_probe)) return false;
    }
    *out = (struct css_layer_images){items, (uint32_t)k};
    return true;
}
static const int side_pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
static void bi_four(struct css_bi_side *side, int k) {
    struct css_bi_side given[4];
    memcpy(given, side, sizeof given);
    for (int i = 0; i < 4; i++) side[i] = given[side_pick[k][i]];
}
/* a non-negative <number> (or <percentage>) of the accepted types; a calculation's negative value is 0 */
static bool bi_numeric(const char *t, size_t n, unsigned accept, struct cx *cx, double *x, unsigned *kind) {
    unsigned k = 0;
    if (!css_numeric_read(t, n, accept, cx, cx->a, NAN, x, &k) || !isfinite(*x)) return false;
    if (*x < 0) { if (!css_calc_is_math(t, n)) return false; *x = 0; }
    if (kind) *kind = k;
    return true;
}
static bool bi_slice_token(const char *t, size_t n, struct cx *cx) {
    double x;
    return bi_numeric(t, n, CSS_CALC_NUMBER | CSS_CALC_PERCENT, cx, &x, NULL);
}
static bool bi_side_parse(int which, const char *t, size_t n, struct cx *cx, struct css_bi_side *out) {
    double x;
    memset(out, 0, sizeof *out);
    if (which == BI_WIDTH && ident_is(t, n, "auto")) { out->kind = CSS_BI_AUTO; out->length.kind = LK_AUTO; return true; }
    if (bi_numeric(t, n, CSS_CALC_NUMBER, cx, &x, NULL)) {
        out->kind = CSS_BI_NUMBER;
        out->length.kind = LK_LEN;
        out->number = calc_float(x);
        return true;
    }
    out->kind = CSS_BI_LENGTH;
    return parse_len(t, n, cx, &out->length, LF_LENGTH | (which == BI_WIDTH ? 0 : LF_NOPCT));
}
/* [<number [0,∞]> | <percentage [0,∞]>]{1,4} && fill? */
static bool parse_bi_slice(const char *v, size_t n, struct cx *cx, struct css_bi_slice *out) {
    const char *t[6];
    size_t tl[6];
    int k = split(v, n, t, tl, 6, ' ');
    if (k < 1 || k > 5) return false;
    struct css_bi_slice s = {0};
    int first = 0, last = k;
    if (ident_is(t[0], tl[0], "fill")) { s.fill = true; first = 1; }
    else if (ident_is(t[k - 1], tl[k - 1], "fill")) { s.fill = true; last = k - 1; }
    int m = last - first;
    if (m < 1 || m > 4) return false;
    for (int i = 0; i < m; i++) {
        double x;
        unsigned kind;
        if (!bi_numeric(t[first + i], tl[first + i], CSS_CALC_NUMBER | CSS_CALC_PERCENT, cx, &x, &kind)) return false;
        s.side[i] = kind == CSS_CALC_PERCENT ? (struct css_bi_side){{.kind = LK_LEN, .pct = calc_float(x)}, 0, CSS_BI_LENGTH}
                                             : (struct css_bi_side){{.kind = LK_LEN}, calc_float(x), CSS_BI_NUMBER};
    }
    bi_four(s.side, m);
    *out = s;
    return true;
}
/* border-image-width: [<length-percentage [0,∞]> | <number [0,∞]> | auto]{1,4};
   border-image-outset: [<length [0,∞]> | <number [0,∞]>]{1,4} */
static bool parse_bi_sides(int which, const char *v, size_t n, struct cx *cx, struct css_bi_side out[4]) {
    const char *t[5];
    size_t tl[5];
    int k = split(v, n, t, tl, 5, ' ');
    if (k < 1 || k > 4) return false;
    struct css_bi_side s[4];
    for (int i = 0; i < k; i++) if (!bi_side_parse(which, t[i], tl[i], cx, &s[i])) return false;
    bi_four(s, k);
    memcpy(out, s, sizeof s);
    return true;
}
static bool parse_bi_repeat(const char *v, size_t n, uint8_t out[2]) {
    const char *t[3];
    size_t tl[3];
    uint8_t r[2];
    int k = split(v, n, t, tl, 3, ' ');
    if (k < 1 || k > 2 || !kw_find(kw_bi_repeat, t[0], tl[0], &r[0])) return false;
    if (k == 1) r[1] = r[0];
    else if (!kw_find(kw_bi_repeat, t[1], tl[1], &r[1])) return false;
    out[0] = r[0];
    out[1] = r[1];
    return true;
}
/* none | <image>: url() or a gradient */
static bool parse_bi_source(const char *v, size_t n, struct cx *cx, struct css_bi_source *out) {
    trim(&v, &n);
    if (ident_is(v, n, "none")) { *out = (struct css_bi_source){0}; return true; }
    const char *t[2];
    size_t tl[2];
    if (split(v, n, t, tl, 2, ' ') != 1) return false;
    if (n > 4 && strn_ieq(v, "url(", 4)) {
        const char *u = css_parse_url(v, n, cx->a);
        if (!u) return false;
        *out = (struct css_bi_source){u, NULL};
        return true;
    }
    const struct gradient *g = parse_gradient(v, n, cx);
    if (!g) return false;
    *out = (struct css_bi_source){NULL, g};
    return true;
}
/* the border-image and mask-border shorthands' parts as written: source, slice, width, outset,
   repeat and (mask-border) mode, each NULL when omitted */
enum { BIS_SOURCE, BIS_SLICE, BIS_WIDTH, BIS_OUTSET, BIS_REPEAT, BIS_MODE, BIS_PARTS };
struct bi_span { const char *s; size_t n; };
static void bi_span(struct bi_span *span, const char *const *t, const size_t *tl, int from, int to) {
    span->s = t[from];
    span->n = (size_t)(t[to - 1] + tl[to - 1] - t[from]);
}
static bool bi_shorthand(bool mask, const char *v, size_t n, struct cx *cx, struct bi_span span[BIS_PARTS]) {
    const char *t[40];
    size_t tl[40];
    int k = slash_tokens(v, n, t, tl, 40);
    memset(span, 0, BIS_PARTS * sizeof *span);
    if (k < 1) return false;
    struct css_bi_side side;
    for (int i = 0; i < k;) {
        uint8_t kw;
        struct css_bi_source source;
        if (slash_at(t, tl, i)) return false;
        if (!span[BIS_REPEAT].s && kw_find(kw_bi_repeat, t[i], tl[i], &kw)) {
            int j = i + 1;
            if (j < k && kw_find(kw_bi_repeat, t[j], tl[j], &kw)) j++;
            bi_span(&span[BIS_REPEAT], t, tl, i, j);
            i = j;
        } else if (mask && !span[BIS_MODE].s && kw_find(kw_mask_border_mode, t[i], tl[i], &kw)) {
            bi_span(&span[BIS_MODE], t, tl, i, i + 1);
            i++;
        } else if (!span[BIS_SLICE].s && (ident_is(t[i], tl[i], "fill") || bi_slice_token(t[i], tl[i], cx))) {
            int j = i;
            while (j < k && (ident_is(t[j], tl[j], "fill") || bi_slice_token(t[j], tl[j], cx))) j++;
            bi_span(&span[BIS_SLICE], t, tl, i, j);
            i = j;
            if (i < k && slash_at(t, tl, i)) { /* / width, / width / outset or / / outset */
                int w = ++i;
                while (i < k && !slash_at(t, tl, i) && bi_side_parse(BI_WIDTH, t[i], tl[i], cx, &side)) i++;
                if (i > w) bi_span(&span[BIS_WIDTH], t, tl, w, i);
                if (i < k && slash_at(t, tl, i)) {
                    int o = ++i;
                    while (i < k && !slash_at(t, tl, i) && bi_side_parse(BI_OUTSET, t[i], tl[i], cx, &side)) i++;
                    if (i == o) return false;
                    bi_span(&span[BIS_OUTSET], t, tl, o, i);
                } else if (i == w) return false;
            }
        } else if (!span[BIS_SOURCE].s && parse_bi_source(t[i], tl[i], cx, &source)) {
            bi_span(&span[BIS_SOURCE], t, tl, i, i + 1);
            i++;
        } else return false;
    }
    /* each part by its longhand's grammar */
    struct css_bi_slice slice;
    struct css_bi_side sides[4];
    uint8_t repeat[2];
    return (!span[BIS_SLICE].s || parse_bi_slice(span[BIS_SLICE].s, span[BIS_SLICE].n, cx, &slice)) &&
           (!span[BIS_WIDTH].s || parse_bi_sides(BI_WIDTH, span[BIS_WIDTH].s, span[BIS_WIDTH].n, cx, sides)) &&
           (!span[BIS_OUTSET].s || parse_bi_sides(BI_OUTSET, span[BIS_OUTSET].s, span[BIS_OUTSET].n, cx, sides)) &&
           (!span[BIS_REPEAT].s || parse_bi_repeat(span[BIS_REPEAT].s, span[BIS_REPEAT].n, repeat));
}
static const char *const bi_names[2][BIS_PARTS] = {
    {"border-image-source", "border-image-slice", "border-image-width", "border-image-outset", "border-image-repeat", NULL},
    {"mask-border-source", "mask-border-slice", "mask-border-width", "mask-border-outset", "mask-border-repeat", "mask-border-mode"}};
/* the shorthand sets every longhand: a part as written, an omitted one to its initial value */
static bool bi_shorthand_apply(bool mask, const char *v, size_t n, bool global, struct cx *cx) {
    struct bi_span span[BIS_PARTS];
    if (!global && !bi_shorthand(mask, v, n, cx, span)) return false;
    for (int i = 0; i < BIS_PARTS; i++) {
        const char *name = bi_names[mask][i];
        if (!name) continue;
        if (global) set_long(name, v, n, cx);
        else if (span[i].s) set_long(name, span[i].s, span[i].n, cx);
        else set_long(name, "initial", 7, cx);
    }
    return true;
}

/* -------- their values' text: specified (as written, math simplified) and computed */
/* a token as specified: math functions simplified, a keyword in lower case */
static void spec_token(sbuf *out, const char *t, size_t n, unsigned accept) {
    if (css_calc_specified(t, n, accept, out)) return;
    bool ident = n && (isalpha((unsigned char)t[0]) || (t[0] == '-' && n > 1 && isalpha((unsigned char)t[1])));
    for (size_t i = 0; ident && i < n; i++) ident = isalnum((unsigned char)t[i]) || t[i] == '-' || t[i] == '_';
    for (size_t i = 0; i < n; i++) sb_putc(out, ident ? (char)tolower((unsigned char)t[i]) : t[i]);
}
/* two tokens of a value (neither need end the string) the same but for ASCII case */
static bool token_ieq(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return false;
    return true;
}
/* one to four sides as written, those that repeat the others left out (CSSOM's sides rule) */
static void spec_sides(sbuf *out, const char *const *t, const size_t *tl, int k, unsigned accept) {
    if (k < 1 || k > 4) return;
    int at[4];
    for (int i = 0; i < 4; i++) at[i] = side_pick[k][i];
#define SAME(i, j) token_ieq(t[at[i]], tl[at[i]], t[at[j]], tl[at[j]])
    int m = 4;
    if (SAME(3, 1)) { m = 3; if (SAME(2, 0)) { m = 2; if (SAME(1, 0)) m = 1; } }
#undef SAME
    for (int i = 0; i < m; i++) { if (i) sb_putc(out, ' '); spec_token(out, t[at[i]], tl[at[i]], accept); }
}
static void url_text(sbuf *out, const char *url) {
    sb_puts(out, "url(\"");
    for (const char *c = url; *c; c++) { if (*c == '"' || *c == '\\') sb_putc(out, '\\'); sb_putc(out, *c); }
    sb_puts(out, "\")");
}
/* a longhand of the background, border-image and mask-border groups as specified; false leaves the
   text as written */
static bool bi_part_specified(int part, const char *v, size_t n, struct cx *cx, sbuf *out) {
    trim(&v, &n);
    const char *t[6];
    size_t tl[6];
    switch (part) {
    case BIS_SOURCE: {
        struct css_bi_source source;
        if (!parse_bi_source(v, n, cx, &source)) return false;
        if (source.url) url_text(out, source.url);
        else if (!source.gradient) sb_puts(out, "none");
        else spec_token(out, v, n, CSS_CALC_ANY);
        return true;
    }
    case BIS_SLICE: {
        int k = split(v, n, t, tl, 6, ' ');
        if (k < 1 || k > 5) return false;
        bool fill_first = ident_is(t[0], tl[0], "fill"), fill_last = !fill_first && ident_is(t[k - 1], tl[k - 1], "fill");
        spec_sides(out, t + fill_first, tl + fill_first, k - fill_first - fill_last, CSS_CALC_NUMBER | CSS_CALC_PERCENT);
        if (fill_first || fill_last) sb_puts(out, " fill");
        return true;
    }
    case BIS_WIDTH: case BIS_OUTSET: {
        int k = split(v, n, t, tl, 5, ' ');
        if (k < 1 || k > 4) return false;
        spec_sides(out, t, tl, k, CSS_CALC_NUMBER | CSS_CALC_LENGTH | (part == BIS_WIDTH ? CSS_CALC_PERCENT : 0));
        return true;
    }
    case BIS_REPEAT: case BIS_MODE: {
        int k = split(v, n, t, tl, 3, ' ');
        if (k < 1 || k > 2) return false;
        if (k == 2 && token_ieq(t[0], tl[0], t[1], tl[1])) k = 1;
        for (int i = 0; i < k; i++) { if (i) sb_putc(out, ' '); spec_token(out, t[i], tl[i], CSS_CALC_ANY); }
        return true;
    }
    }
    return false;
}
static bool background_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    size_t begin = out->n;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t scratch = {0};
        struct cx cx = {.s = &scratch, .em = 16, .rem = 16, .vw = 800, .vh = 600, .a = &a};
        const char **item;
        size_t *il;
        int k;
        static const int part_of[] = {[PT_BI_SOURCE] = BIS_SOURCE, [PT_BI_SLICE] = BIS_SLICE, [PT_BI_REPEAT] = BIS_REPEAT};
        if (p->type == PT_BI_SOURCE || p->type == PT_BI_SLICE || p->type == PT_BI_REPEAT)
            ok = bi_part_specified(part_of[p->type], v, n, &cx, out);
        else if (p->type == PT_BI_SIDES) ok = bi_part_specified(p->flags == BI_WIDTH ? BIS_WIDTH : BIS_OUTSET, v, n, &cx, out);
        else if (p->type == PT_KW) ok = bi_part_specified(BIS_MODE, v, n, &cx, out);
        else if (p->type == PT_SHORT && (p->sh == SH_BORDER_IMAGE || p->sh == SH_MASK_BORDER)) {
            struct bi_span span[BIS_PARTS];
            ok = bi_shorthand(p->sh == SH_MASK_BORDER, v, n, &cx, span);
            static const int order[] = {BIS_SOURCE, BIS_SLICE, BIS_WIDTH, BIS_OUTSET, BIS_REPEAT, BIS_MODE};
            bool any = false;
            for (unsigned i = 0; ok && i < sizeof order / sizeof *order; i++) {
                int part = order[i];
                bool slash = part == BIS_WIDTH ? span[BIS_WIDTH].s || span[BIS_OUTSET].s : part == BIS_OUTSET && span[part].s;
                if (slash) sb_puts(out, " /");
                if (!span[part].s) continue;
                if (any) sb_putc(out, ' ');
                ok = bi_part_specified(part, span[part].s, span[part].n, &cx, out);
                any = true;
            }
        } else if ((k = list_split(v, n, &cx, &item, &il)) > 0) {
            ok = true;
            for (int i = 0; ok && i < k; i++) {
                if (i) sb_puts(out, ", ");
                const char *t[5];
                size_t tl[5];
                int m = split(item[i], il[i], t, tl, 5, ' ');
                if (p->type == PT_SHORT || p->type == PT_MASK_POSITION) {
                    /* background-position (a <bg-position>) and mask-position (a <position>): the
                       horizontal part, then the vertical one */
                    struct bg_part part[2];
                    ok = (p->type == PT_MASK_POSITION || p->sh == SH_BACKGROUND_POSITION) &&
                         bg_position_parts(t, tl, m, p->type != PT_MASK_POSITION, &cx, part);
                    for (int axis = 0; ok && axis < 2; axis++) {
                        if (axis) sb_putc(out, ' ');
                        if (part[axis].kw) sb_puts(out, kw_name(kw_bg_edge, part[axis].kw));
                        if (part[axis].kw && part[axis].off) sb_putc(out, ' ');
                        if (part[axis].off) spec_token(out, part[axis].off, part[axis].off_n, CSS_CALC_LENGTH | CSS_CALC_PERCENT);
                    }
                } else if (p->type == PT_BG_LIST && p->kws == kw_bg_clip && m == 2) {
                    sb_puts(out, "border-area text");
                } else if (p->type == PT_MASK_REPEAT) {
                    uint8_t repeat;
                    ok = repeat_style_parse(item[i], il[i], &repeat);
                    if (ok) repeat_style_text(repeat, out);
                } else if (p->type == PT_MASK_SIZE && m == 1 && !ident_is(t[0], tl[0], "auto") &&
                           !ident_is(t[0], tl[0], "cover") && !ident_is(t[0], tl[0], "contain")) {
                    /* one length: the height auto (css-backgrounds-3 §3.9) */
                    spec_token(out, t[0], tl[0], CSS_CALC_LENGTH | CSS_CALC_PERCENT);
                    sb_puts(out, " auto");
                } else if (p->type == PT_MASK_SIZE && m == 2 && ident_is(t[0], tl[0], "auto") && ident_is(t[1], tl[1], "auto")) {
                    sb_puts(out, "auto");
                } else if (p->type == PT_MASK_IMAGE) {
                    const struct css_image *image;
                    int r = m == 1 ? mask_reference_parse(t[0], tl[0], &cx, &image) : -2;
                    if (r == 0) sb_puts(out, "none");
                    else if (r == 1 && image->kind == CSS_IMAGE_URL) url_text(out, image->url);
                    else if (r != -2) spec_token(out, t[0], tl[0], CSS_CALC_ANY);
                    else ok = false;
                } else {
                    for (int j = 0; j < m; j++) {
                        if (j) sb_putc(out, ' ');
                        spec_token(out, t[j], tl[j], CSS_CALC_LENGTH | CSS_CALC_PERCENT);
                    }
                }
            }
        }
    }
    ar_free(&a);
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    return ok;
}

/* computed values */
/* slice: its length kind is a percentage of the image */
static bool bi_side_text(const struct css_bi_side *s, bool slice, sbuf *out, web_doc *d) {
    if (s->kind == CSS_BI_AUTO) { sb_puts(out, "auto"); return true; }
    if (s->kind == CSS_BI_NUMBER) { css_number_serialize(out, s->number); return true; }
    if (slice) { css_number_serialize(out, s->length.pct); sb_putc(out, '%'); return true; }
    return css_length_serialize(&s->length, out, d);
}
static bool bi_sides_text(const struct css_bi_side *s, bool slice, sbuf *out, web_doc *d) {
    int m = 4;
    if (bi_side_equal(&s[3], &s[1])) { m = 3; if (bi_side_equal(&s[2], &s[0])) { m = 2; if (bi_side_equal(&s[1], &s[0])) m = 1; } }
    for (int i = 0; i < m; i++) {
        if (i) sb_putc(out, ' ');
        if (!bi_side_text(&s[i], slice, out, d)) return false;
    }
    return true;
}
static bool bi_slice_text(const struct css_bi_slice *s, sbuf *out, web_doc *d) {
    if (!bi_sides_text(s->side, true, out, d)) return false;
    if (s->fill) sb_puts(out, " fill");
    return true;
}
static bool bi_source_text(const struct css_bi_source *s, sbuf *out) {
    if (s->gradient) return false; /* a gradient's computed value has no serializer yet */
    if (s->url) url_text(out, s->url);
    else sb_puts(out, "none");
    return true;
}
static void bi_repeat_text(const uint8_t *r, sbuf *out) {
    sb_puts(out, kw_name(kw_bi_repeat, r[0]));
    if (r[1] != r[0]) { sb_putc(out, ' '); sb_puts(out, kw_name(kw_bi_repeat, r[1])); }
}
static bool bg_offset_text(int axis, const struct css_bg_offset *o, sbuf *out, web_doc *d) {
    bool zero = o->offset.kind == LK_LEN && o->offset.px == 0 && o->offset.pct == 0;
    if (o->edge != CSS_BG_EDGE_START) {
        sb_puts(out, axis ? o->edge == CSS_BG_EDGE_LOGICAL_START ? "y-start" : "y-end"
                          : o->edge == CSS_BG_EDGE_LOGICAL_START ? "x-start" : "x-end");
        if (zero) return true;
        sb_putc(out, ' ');
    }
    if (zero) { sb_puts(out, o->percent ? "0%" : "0px"); return true; }
    return css_length_serialize(&o->offset, out, d);
}
static bool bg_offsets_text(int axis, const struct css_bg_offsets *l, sbuf *out, web_doc *d) {
    for (uint32_t i = 0; i < l->n; i++) {
        if (i) sb_puts(out, ", ");
        if (!bg_offset_text(axis, &l->v[i], out, d)) return false;
    }
    return true;
}
static bool border_image_text(const struct css_border_image *b, bool mask, sbuf *out, web_doc *d) {
    if (!bi_source_text(&b->source, out)) return false;
    sb_putc(out, ' ');
    if (!bi_slice_text(&b->slice, out, d)) return false;
    sb_puts(out, " / ");
    if (!bi_sides_text(b->width, false, out, d)) return false;
    sb_puts(out, " / ");
    if (!bi_sides_text(b->outset, false, out, d)) return false;
    sb_putc(out, ' ');
    bi_repeat_text(b->repeat, out);
    if (mask) { sb_putc(out, ' '); sb_puts(out, kw_name(kw_mask_border_mode, b->mode)); }
    return true;
}
/* the shorthands whose longhands are one group's: their computed value */
static bool group_shorthand_text(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d) {
    if (p->sh == SH_BORDER_IMAGE) return border_image_text(css_style_border_image(st), false, out, d);
    if (p->sh == SH_MASK_BORDER) return border_image_text(css_style_mask_border(st), true, out, d);
    if (p->sh == SH_MASK) return mask_computed(st, out, d);
    if (p->sh != SH_BACKGROUND_POSITION) return false;
    const struct css_background *bg = css_style_background(st);
    uint32_t nx = bg->position[0].n, ny = bg->position[1].n, count = nx > ny ? nx : ny;
    for (uint32_t i = 0; i < count; i++) {
        if (i) sb_puts(out, ", ");
        if (!bg_offset_text(0, &bg->position[0].v[i % nx], out, d)) return false;
        sb_putc(out, ' ');
        if (!bg_offset_text(1, &bg->position[1].v[i % ny], out, d)) return false;
    }
    return true;
}

/* -------- background-image's layers, and the shorthands that set the groups' longhands */
/* a <bg-image>: 1 an image painting draws (*url or *gradient), 0 none, -1 another image function
   (accepted outside @supports, not drawn), -2 not an image */
static int bg_image_item(const char *t, size_t n, struct cx *cx, const char **url, const struct gradient **gradient) {
    *url = NULL;
    *gradient = NULL;
    if (ident_is(t, n, "none")) return 0;
    if (n > 4 && strn_ieq(t, "url(", 4)) return (*url = css_parse_url(t, n, cx->a)) ? 1 : -2;
    if ((*gradient = parse_gradient(t, n, cx))) return 1;
    size_t open = 0;
    while (open < n && t[open] != '(') open++;
    return open && open < n && t[n - 1] == ')' ? -1 : -2;
}
/* background-image: the layers' count, and the first one painting draws (the topmost) */
static bool parse_bg_image(const char *s, size_t n, struct cx *cx) {
    style_t *st = cx->s;
    const char **t;
    size_t *tl;
    int k = list_split(s, n, cx, &t, &tl);
    if (!k || k > UINT16_MAX) return false;
    int used = -1;
    const char *url = NULL;
    const struct gradient *gradient = NULL;
    for (int i = 0; i < k; i++) {
        const char *u;
        const struct gradient *g;
        int r = bg_image_item(t[i], tl[i], cx, &u, &g);
        if (r == -2 || (r == -1 && cx->supports_probe)) return false;
        if (r == 1 && used < 0) { used = i; url = u; gradient = g; }
    }
    st->bg_layers = (uint16_t)k;
    st->bg_layer = used < 0 ? 0 : (uint16_t)used;
    st->bg_image = url;
    st->bg_img = 0;
    st->gradient = gradient;
    st->has_grad = gradient != NULL;
    if (gradient) {
        st->grad[0] = gradient->col[0];
        st->grad[1] = gradient->col[gradient->n - 1];
    }
    return true;
}

/* background-position: the -x and -y longhands */
static bool bg_position_apply(const char *v, size_t n, bool global, struct cx *cx) {
    if (global) {
        set_long("background-position-x", v, n, cx);
        set_long("background-position-y", v, n, cx);
        return true;
    }
    struct css_background parsed;
    if (!parse_bg_positions(v, n, true, cx, parsed.position)) return false;
    bg_group_write(cx, &parsed, 3u << CSS_BG_LISTS);
    return true;
}

/* background (css-backgrounds-3 §3.10, css-backgrounds-4 §2.12) and mask (css-masking-1 §7.9): a
   layer's parts as written */
struct bg_layer_value {
    struct bi_span image, position, size, repeat, attachment, color, composite, mode;
    struct bg_part part[2];
    uint8_t box[2], nbox; /* the <visual-box>es (mask: <coord-box>es), in order */
    uint8_t clip_only;    /* border-area || text; mask: no-clip */
};
static bool bg_size_token(const char *t, size_t n, struct cx *cx) {
    len_t l;
    return ident_is(t, n, "cover") || ident_is(t, n, "contain") || parse_len(t, n, cx, &l, LF_AUTO);
}
static bool bg_position_token(const char *t, size_t n, struct cx *cx) {
    uint8_t kw;
    return kw_find(kw_bg_edge, t, n, &kw) || bp_length(t, n, cx);
}
/* <bg-layer>, or with final <final-bg-layer> (a colour allowed); mask: a <mask-layer> */
static bool bg_layer_parse(const char *v, size_t n, bool final, bool mask, struct cx *cx, struct bg_layer_value *L) {
    const char *t[48];
    size_t tl[48];
    int k = slash_tokens(v, n, t, tl, 48);
    memset(L, 0, sizeof *L);
    if (k < 1) return false;
    for (int i = 0; i < k;) {
        uint8_t kw, other;
        uint32_t colour;
        const char *url;
        const struct gradient *gradient;
        int image;
        const struct css_image *reference;
        if (slash_at(t, tl, i)) return false;
        if (!L->position.s && bg_position_token(t[i], tl[i], cx)) {
            /* the longest run that is a <bg-position> (mask: a <position>), then / and its <bg-size> */
            int j = i;
            while (j < k && j - i < 4 && !slash_at(t, tl, j) && bg_position_token(t[j], tl[j], cx)) j++;
            while (j > i && !bg_position_parts(t + i, tl + i, j - i, !mask, cx, L->part)) j--;
            if (j == i) return false;
            bi_span(&L->position, t, tl, i, j);
            i = j;
            if (i < k && slash_at(t, tl, i)) {
                int s = ++i;
                while (i < k && i - s < 2 && !slash_at(t, tl, i) && bg_size_token(t[i], tl[i], cx)) i++;
                if (i == s) return false;
                bi_span(&L->size, t, tl, s, i);
                len_t size[2];
                uint8_t kind;
                if (!parse_bg_size(L->size.s, L->size.n, cx, size, &kind)) return false;
            }
        } else if (final && !mask && !L->color.s && css_color_checked(t[i], tl[i], &colour, cx)) {
            bi_span(&L->color, t, tl, i, i + 1);
            i++;
        } else if (!L->repeat.s && kw_find(kw_bg_repeat, t[i], tl[i], &kw)) {
            /* repeat-x | repeat-y | [repeat | space | round | no-repeat]{1,2} */
            bool single = ident_is(t[i], tl[i], "repeat-x") || ident_is(t[i], tl[i], "repeat-y");
            int j = i + 1;
            if (!single && j < k && kw_find(kw_bg_repeat, t[j], tl[j], &other) && !ident_is(t[j], tl[j], "repeat-x") &&
                !ident_is(t[j], tl[j], "repeat-y"))
                j++;
            bi_span(&L->repeat, t, tl, i, j);
            i = j;
        } else if (!mask && !L->attachment.s && kw_find(kw_bg_attachment, t[i], tl[i], &kw)) {
            bi_span(&L->attachment, t, tl, i, i + 1);
            i++;
        } else if (mask && !L->composite.s && kw_find(kw_mask_composite, t[i], tl[i], &kw)) {
            bi_span(&L->composite, t, tl, i, i + 1);
            i++;
        } else if (mask && !L->mode.s && kw_find(kw_mask_mode, t[i], tl[i], &kw)) {
            bi_span(&L->mode, t, tl, i, i + 1);
            i++;
        } else if (kw_find(mask ? kw_mask_box : kw_bg_box, t[i], tl[i], &kw)) {
            if (L->nbox == 2 || (L->nbox == 1 && L->clip_only)) return false;
            L->box[L->nbox++] = kw;
            i++;
        } else if (mask && !L->clip_only && ident_is(t[i], tl[i], "no-clip")) {
            /* <coord-box> || [ <coord-box> | no-clip ]: no-clip is the second */
            if (L->nbox == 2) return false;
            L->clip_only = CSS_MASK_NO_CLIP;
            i++;
        } else if (!mask && !L->clip_only && kw_find(kw_bg_clip, t[i], tl[i], &kw) && kw >= CSS_BG_CLIP_AREA) {
            if (L->nbox == 2) return false;
            if (i + 1 < k && kw_find(kw_bg_clip, t[i + 1], tl[i + 1], &other) && other >= CSS_BG_CLIP_AREA && other != kw &&
                (kw | other) == (CSS_BG_CLIP_AREA | CSS_BG_CLIP_TEXT)) {
                kw |= other;
                i++;
            }
            L->clip_only = kw;
            i++;
        } else if (!L->image.s &&
                   (image = mask ? mask_reference_parse(t[i], tl[i], cx, &reference) : bg_image_item(t[i], tl[i], cx, &url, &gradient)) != -2 &&
                   !(image == -1 && cx->supports_probe)) {
            bi_span(&L->image, t, tl, i, i + 1);
            i++;
        } else return false;
    }
    return true;
}
/* a <repeat-style> as background-repeat's one keyword holds it: a pair's nearest; NULL: one keyword */
static const char *bg_repeat_keyword(const struct bi_span *s) {
    const char *t[2];
    size_t tl[2];
    int k = split(s->s, s->n, t, tl, 2, ' ');
    if (k == 1) return NULL;
    bool x = !ident_is(t[0], tl[0], "no-repeat"), y = !ident_is(t[1], tl[1], "no-repeat");
    return x && y ? "repeat" : x ? "repeat-x" : y ? "repeat-y" : "no-repeat";
}
static bool set_background(const char *v, size_t n, struct cx *cx) {
    const char **item;
    size_t *il;
    int k = list_split(v, n, cx, &item, &il);
    if (!k || k > UINT16_MAX) return false;
    struct bg_layer_value *L = ar_alloc(cx->a, (size_t)k * sizeof *L);
    for (int i = 0; i < k; i++) if (!bg_layer_parse(item[i], il[i], i == k - 1, false, cx, &L[i])) return false;
    /* every layer's position, validated before any longhand is written */
    struct css_background parsed = css_background_initial;
    uint8_t *lists = ar_alloc(cx->a, (size_t)k * 3);
    struct css_bg_offset *offsets = ar_alloc(cx->a, (size_t)k * 2 * sizeof *offsets);
    for (int i = 0; i < k; i++) {
        uint8_t attachment = CSS_BG_SCROLL, origin = CSS_BOX_PADDING, clip = CSS_BOX_BORDER;
        if (L[i].attachment.s) kw_find(kw_bg_attachment, L[i].attachment.s, L[i].attachment.n, &attachment);
        /* one box sets both; a value only background-clip takes sets it and origin border-box; two:
           origin, then clip */
        if (L[i].clip_only) { clip = L[i].clip_only; origin = L[i].nbox ? L[i].box[0] : CSS_BOX_BORDER; }
        else if (L[i].nbox == 1) origin = clip = L[i].box[0];
        else if (L[i].nbox == 2) { origin = L[i].box[0]; clip = L[i].box[1]; }
        lists[i] = attachment;
        lists[k + i] = clip;
        lists[2 * k + i] = origin;
        for (int axis = 0; axis < 2; axis++) {
            struct css_bg_offset *o = &offsets[axis * k + i];
            if (!L[i].position.s) *o = bg_offset_initial;
            else if (!bg_part_offset(&L[i].part[axis], cx, o)) return false;
        }
    }
    parsed.list[CSS_BG_ATTACHMENT] = (struct css_bg_keywords){lists, (uint32_t)k};
    parsed.list[CSS_BG_CLIP] = (struct css_bg_keywords){lists + k, (uint32_t)k};
    parsed.list[CSS_BG_ORIGIN] = (struct css_bg_keywords){lists + 2 * k, (uint32_t)k};
    parsed.position[0] = (struct css_bg_offsets){offsets, (uint32_t)k};
    parsed.position[1] = (struct css_bg_offsets){offsets + k, (uint32_t)k};
    /* background-color, then each layer's image */
    const struct bi_span *colour = &L[k - 1].color;
    if (colour->s) set_long("background-color", colour->s, colour->n, cx);
    else set_long("background-color", "transparent", 11, cx);
    sbuf images = {0};
    for (int i = 0; i < k; i++) {
        if (i) sb_puts(&images, ", ");
        if (L[i].image.s) sb_put(&images, L[i].image.s, L[i].image.n);
        else sb_puts(&images, "none");
    }
    size_t image_n = images.n;
    char *image_text = ar_strndup(cx->a, images.p ? images.p : "", image_n);
    sb_free(&images);
    set_long("background-image", image_text, image_n, cx);
    /* background-repeat and -size hold one layer's value: the painted one's */
    uint32_t painted = cx->s->bg_layer < (uint32_t)k ? cx->s->bg_layer : 0;
    const struct bg_layer_value *p = &L[painted];
    const char *repeat = p->repeat.s ? bg_repeat_keyword(&p->repeat) : "repeat";
    if (repeat) set_long("background-repeat", repeat, strlen(repeat), cx);
    else set_long("background-repeat", p->repeat.s, p->repeat.n, cx);
    if (p->size.s) set_long("background-size", p->size.s, p->size.n, cx);
    else set_long("background-size", "auto", 4, cx);
    bg_group_write(cx, &parsed, 1u << CSS_BG_ATTACHMENT | 1u << CSS_BG_CLIP | 1u << CSS_BG_ORIGIN | 3u << CSS_BG_LISTS);
    return true;
}
/* mask (css-masking-1 §7.9): the layers' longhands, in its canonical order, then mask-border's, which
   it resets (§8.7) */
enum { MK_IMAGE, MK_POSITION, MK_SIZE, MK_REPEAT, MK_ORIGIN, MK_CLIP, MK_COMPOSITE, MK_MODE, MK_LAYER_PARTS,
       MK_PARTS = MK_LAYER_PARTS + 6 };
static const char *const mask_names[MK_PARTS] = {
    "mask-image", "mask-position", "mask-size", "mask-repeat", "mask-origin", "mask-clip", "mask-composite", "mask-mode",
    "mask-border-source", "mask-border-slice", "mask-border-width", "mask-border-outset", "mask-border-repeat",
    "mask-border-mode"};
/* each one's initial value as serialized */
static const char *const mask_initial_text[MK_PARTS] = {
    "none", "0% 0%", "auto", "repeat", "border-box", "border-box", "add", "match-source",
    "none", "0", "auto", "0", "stretch", "alpha"};
static bool mask_shorthand_apply(const char *v, size_t n, bool global, struct cx *cx) {
    if (global) {
        for (int i = 0; i < MK_PARTS; i++) set_long(mask_names[i], v, n, cx);
        return true;
    }
    const char **item;
    size_t *il;
    int k = list_split(v, n, cx, &item, &il);
    if (!k || k > UINT16_MAX) return false;
    struct bg_layer_value *L = ar_alloc(cx->a, (size_t)k * sizeof *L);
    for (int i = 0; i < k; i++) if (!bg_layer_parse(item[i], il[i], false, true, cx, &L[i])) return false;
    /* each longhand's list: a layer's part as written, an omitted one its initial value; one box sets
       the origin and the clip, two the origin then the clip, no-clip the clip */
    sbuf text[MK_LAYER_PARTS] = {{0}};
    for (int i = 0; i < k; i++) {
        const struct bg_layer_value *x = &L[i];
        const struct bi_span *given[MK_LAYER_PARTS] = {&x->image, &x->position, &x->size, &x->repeat, NULL, NULL,
                                                      &x->composite, &x->mode};
        uint8_t origin = x->nbox ? x->box[0] : CSS_BOX_BORDER;
        uint8_t clip = x->clip_only ? x->clip_only : x->nbox == 2 ? x->box[1] : origin;
        for (int part = 0; part < MK_LAYER_PARTS; part++) {
            if (i) sb_puts(&text[part], ", ");
            if (part == MK_ORIGIN) sb_puts(&text[part], kw_name(kw_mask_box, origin));
            else if (part == MK_CLIP) sb_puts(&text[part], kw_name(kw_mask_clip, clip));
            else if (given[part]->s) sb_put(&text[part], given[part]->s, given[part]->n);
            else sb_puts(&text[part], mask_initial_text[part]);
        }
    }
    for (int part = 0; part < MK_LAYER_PARTS; part++) {
        size_t length = text[part].n;
        char *value = ar_strndup(cx->a, text[part].p ? text[part].p : "", length);
        sb_free(&text[part]);
        set_long(mask_names[part], value, length, cx);
    }
    for (int part = MK_LAYER_PARTS; part < MK_PARTS; part++)
        set_long(mask_names[part], mask_initial_text[part], strlen(mask_initial_text[part]), cx);
    return true;
}
/* item i of a comma-separated list (commas outside parentheses and strings), trimmed; false past its
   end */
static bool list_item_at(const char *s, size_t n, uint32_t i, const char **item, size_t *length) {
    size_t start = 0;
    int depth = 0;
    char q = 0;
    for (size_t at = 0; at <= n; at++) {
        char c = at < n ? s[at] : ',';
        if (at < n && q) { if (c == '\\') at++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && (depth <= 0 || at == n)) {
            if (!i--) {
                *item = s + start;
                *length = at - start;
                trim(item, length);
                return true;
            }
            start = at + 1;
        }
    }
    return false;
}
static uint32_t list_count(const char *s, size_t n) {
    uint32_t count = 0;
    const char *item;
    size_t length;
    while (list_item_at(s, n, count, &item, &length)) count++;
    return count;
}
static bool text_same(const char *a, size_t an, const char *b, size_t bn);
static bool text_word(const char *a, size_t an, const char *word);
/* mask's value from its longhands' (v, in mask_names' order): each layer's parts in the canonical
   order, those at their initial values left out (none when all are); false when mask-border is not
   at its initial value, or (a declared value, !repeat) the lists' lengths differ. repeat: computed
   lists, layer i taking item i mod n of each (css-backgrounds-3 §2.2) */
static bool mask_shorthand_text(const char *const *v, const size_t *l, bool repeat, sbuf *out) {
    for (int part = MK_LAYER_PARTS; part < MK_PARTS; part++)
        if (!text_word(v[part], l[part], mask_initial_text[part])) return false;
    uint32_t count[MK_LAYER_PARTS];
    for (int part = 0; part < MK_LAYER_PARTS; part++) {
        count[part] = list_count(v[part], l[part]);
        if (!count[part] || (!repeat && count[part] != count[MK_IMAGE])) return false;
    }
    for (uint32_t i = 0; i < count[MK_IMAGE]; i++) {
        const char *t[MK_LAYER_PARTS];
        size_t tl[MK_LAYER_PARTS];
        for (int part = 0; part < MK_LAYER_PARTS; part++)
            list_item_at(v[part], l[part], i % count[part], &t[part], &tl[part]);
        if (i) sb_puts(out, ", ");
        size_t at = out->n;
#define MASK_PUT(x, xl) do { if (out->n > at) sb_putc(out, ' '); sb_put(out, x, xl); } while (0)
#define MASK_INITIAL(part) text_word(t[part], tl[part], mask_initial_text[part])
        if (!MASK_INITIAL(MK_IMAGE)) MASK_PUT(t[MK_IMAGE], tl[MK_IMAGE]);
        bool sized = !MASK_INITIAL(MK_SIZE);
        if (sized || !MASK_INITIAL(MK_POSITION)) {
            MASK_PUT(t[MK_POSITION], tl[MK_POSITION]);
            if (sized) { sb_puts(out, " / "); sb_put(out, t[MK_SIZE], tl[MK_SIZE]); }
        }
        if (!MASK_INITIAL(MK_REPEAT)) MASK_PUT(t[MK_REPEAT], tl[MK_REPEAT]);
        /* one box for both; no-clip alone over a border-box origin; else the origin, then the clip */
        if (token_ieq(t[MK_ORIGIN], tl[MK_ORIGIN], t[MK_CLIP], tl[MK_CLIP])) {
            if (!MASK_INITIAL(MK_ORIGIN)) MASK_PUT(t[MK_ORIGIN], tl[MK_ORIGIN]);
        } else if (MASK_INITIAL(MK_ORIGIN) && text_word(t[MK_CLIP], tl[MK_CLIP], "no-clip")) {
            MASK_PUT(t[MK_CLIP], tl[MK_CLIP]);
        } else {
            MASK_PUT(t[MK_ORIGIN], tl[MK_ORIGIN]);
            MASK_PUT(t[MK_CLIP], tl[MK_CLIP]);
        }
        if (!MASK_INITIAL(MK_COMPOSITE)) MASK_PUT(t[MK_COMPOSITE], tl[MK_COMPOSITE]);
        if (!MASK_INITIAL(MK_MODE)) MASK_PUT(t[MK_MODE], tl[MK_MODE]);
        if (out->n == at) sb_puts(out, "none");
#undef MASK_INITIAL
#undef MASK_PUT
    }
    return true;
}
/* its specified value: its longhands' as specified, put back together */
struct mask_specified_parts { sbuf text[MK_PARTS]; };
static void mask_specified_part(void *opaque, const char *name, const char *value, size_t n) {
    struct mask_specified_parts *parts = opaque;
    for (int i = 0; i < MK_PARTS; i++) {
        if (strcmp(name, mask_names[i])) continue;
        parts->text[i].n = 0;
        if (!css_specified_longhand(name, value, n, &parts->text[i])) {
            parts->text[i].n = 0;
            sb_put(&parts->text[i], value, n);
        }
        sb_cstr(&parts->text[i]);
    }
}
static bool mask_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    struct mask_specified_parts parts;
    memset(&parts, 0, sizeof parts);
    bool ok = css_shorthand_expand(p, v, n, mask_specified_part, &parts) == 1;
    const char *text[MK_PARTS];
    size_t length[MK_PARTS];
    for (int i = 0; i < MK_PARTS; i++) {
        text[i] = parts.text[i].p ? parts.text[i].p : "";
        length[i] = parts.text[i].n;
    }
    size_t begin = out->n;
    ok = ok && mask_shorthand_text(text, length, false, out);
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    for (int i = 0; i < MK_PARTS; i++) sb_free(&parts.text[i]);
    return ok;
}
/* its computed value: its longhands' computed values put back together */
static bool mask_computed(const style_t *st, sbuf *out, web_doc *d) {
    sbuf text[MK_PARTS];
    memset(text, 0, sizeof text);
    bool ok = true;
    for (int i = 0; ok && i < MK_PARTS; i++) {
        ok = css_computed_value(st, css_prop_lookup(mask_names[i], strlen(mask_names[i])), &text[i], d);
        sb_cstr(&text[i]);
    }
    const char *v[MK_PARTS];
    size_t l[MK_PARTS];
    for (int i = 0; i < MK_PARTS; i++) { v[i] = text[i].p ? text[i].p : ""; l[i] = text[i].n; }
    size_t begin = out->n;
    ok = ok && mask_shorthand_text(v, l, true, out);
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    for (int i = 0; i < MK_PARTS; i++) sb_free(&text[i]);
    return ok;
}

/* ---------------------------------------------------------------- lists, counters and generated content */
/* The values of list-style-* (CSS Lists 3 §3), counter-* (§4), quotes and content (CSS Generated
   Content 3 §1-2), and the counter styles they name (CSS Counter Styles 3 §3.1, §6.1). counter.c
   counts the counters and writes the text these values generate. */
const struct css_counters css_counters_initial = {{NULL}};
const struct css_list css_list_initial = {0};
const struct css_content css_content_none = {.none = true};
static const struct css_quotes quotes_none = {0};
static const char *const counter_style_names[LS_PREDEFINED] = {
#define CSS_COUNTER_STYLE_NAME(id, name) name,
    CSS_COUNTER_STYLES(CSS_COUNTER_STYLE_NAME)
#undef CSS_COUNTER_STYLE_NAME
};

static bool prefix_is(const char *s, size_t n, const char *prefix) {
    size_t k = strlen(prefix);
    return n >= k && strn_ieq(s, prefix, k);
}
/* a <string-token> that is the whole of s */
static bool string_token(const char *s, size_t n) {
    if (n < 2 || (s[0] != '"' && s[0] != '\'') || s[n - 1] != s[0]) return false;
    for (size_t i = 1; i + 1 < n; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == s[0]) return false;
    }
    return true;
}
static const char *string_value(const char *s, size_t n, arena_t *a) {
    sbuf b = {0};
    css_unescape(&b, s + 1, n - 2);
    const char *r = ar_strndup(a, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}
/* the next space-separated component of [*p, e), its strings, functions and escapes kept whole */
static bool value_token(const char **p, const char *e, const char **token, size_t *length) {
    const char *s = *p;
    while (s < e && is_space((unsigned char)*s)) s++;
    if (s >= e) { *p = s; return false; }
    const char *start = s;
    int depth = 0;
    char q = 0;
    for (; s < e; s++) {
        char c = *s;
        if (c == '\\' && s + 1 < e) { s++; continue; }
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (!depth && is_space((unsigned char)c)) break;
    }
    *token = start;
    *length = (size_t)(s - start);
    *p = s;
    return true;
}
static bool name_start(const char *s, size_t n, size_t i) {
    if (i >= n) return false;
    unsigned char c = (unsigned char)s[i];
    return isalpha(c) || c == '_' || c >= 0x80 || (c == '\\' && i + 1 < n && s[i + 1] != '\n');
}
/* the length of the <ident-token> s starts with, its escapes included; 0: none */
static size_t ident_token(const char *s, size_t n) {
    size_t i = 0;
    if (n && s[0] == '-') i = n > 1 && s[1] == '-' ? 2 : name_start(s, n, 1) ? 1 : 0;
    if (!i && !name_start(s, n, 0)) return 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') {
            if (i + 1 >= n || s[i + 1] == '\n') break;
            i++;
            if (isxdigit((unsigned char)s[i])) {
                for (int k = 0; k < 6 && i < n && isxdigit((unsigned char)s[i]); k++) i++;
                if (i < n && is_space((unsigned char)s[i])) i++;
            } else i++;
        } else if (isalnum(c) || c == '-' || c == '_' || c >= 0x80) i++;
        else break;
    }
    return i;
}
/* a <custom-ident> (CSS Values 4 §6.6.1) that is the whole of s, decoded, which is not a CSS-wide
   keyword, default or excluded (ASCII case-insensitively); NULL when it is not one */
static const char *custom_ident(const char *s, size_t n, arena_t *a, const char *excluded) {
    if (!n || ident_token(s, n) != n) return NULL;
    sbuf b = {0};
    css_unescape(&b, s, n);
    const char *name = b.p ? b.p : "";
    static const char *const reserved[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default"};
    bool bad = excluded && ident_is(name, b.n, excluded);
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) bad = bad || ident_is(name, b.n, reserved[i]);
    const char *r = bad ? NULL : ar_strndup(a, name, b.n);
    sb_free(&b);
    return r;
}

/* symbols() (CSS Counter Styles 3 §6.1): symbols( <symbols-type>? [ <string> | <image> ]+ ). An
   <image> symbol is not supported, so the function is invalid with one. */
static bool parse_symbols(const char *s, size_t n, struct cx *cx, const struct css_symbols **out) {
    static const char *const types[] = {"cyclic", "numeric", "alphabetic", "symbolic", "fixed"};
    if (!prefix_is(s, n, "symbols(") || s[n - 1] != ')') return false;
    const char *p = s + 8, *e = s + n - 1, *t;
    size_t tn;
    uint8_t type = CSS_SYMBOLS_SYMBOLIC;
    uint32_t count = 0;
    bool first = true;
    for (const char *q = p; value_token(&q, e, &t, &tn); first = false) {
        if (string_token(t, tn)) { count++; continue; }
        bool typed = false;
        for (uint8_t i = 0; first && i < 5; i++) if (ident_is(t, tn, types[i])) type = i, typed = true;
        if (!typed) return false;
    }
    if (!count || ((type == CSS_SYMBOLS_NUMERIC || type == CSS_SYMBOLS_ALPHABETIC) && count < 2)) return false;
    struct css_symbols *y = ar_alloc(cx->a, sizeof *y + count * sizeof *y->symbol);
    y->type = type;
    for (const char *q = p; value_token(&q, e, &t, &tn);)
        if (string_token(t, tn)) y->symbol[y->n++] = string_value(t, tn, cx->a);
    *out = y;
    return true;
}
/* a <counter-style> (§3.1): a <counter-style-name>, which none is not, or symbols(). The predefined
   names match ASCII case-insensitively; another name keeps its case. */
static bool parse_counter_style(const char *s, size_t n, struct cx *cx, struct css_counter_style *out) {
    memset(out, 0, sizeof *out);
    if (prefix_is(s, n, "symbols(")) { out->id = LS_SYMBOLS; return parse_symbols(s, n, cx, &out->symbols); }
    const char *name = custom_ident(s, n, cx->a, "none");
    if (!name) return false;
    size_t length = strlen(name);
    for (uint8_t i = 0; i < LS_PREDEFINED; i++)
        if (ident_is(name, length, counter_style_names[i])) { out->id = i; return true; }
    out->id = LS_NAMED;
    out->name = name;
    return true;
}
static void symbols_text(const struct css_symbols *y, sbuf *out) {
    static const char *const types[] = {"cyclic", "numeric", "alphabetic", "symbolic", "fixed"};
    sb_puts(out, "symbols(");
    if (y->type != CSS_SYMBOLS_SYMBOLIC) { sb_puts(out, types[y->type]); sb_putc(out, ' '); }
    for (uint32_t i = 0; i < y->n; i++) {
        if (i) sb_putc(out, ' ');
        css_serialize_string(out, y->symbol[i]);
    }
    sb_putc(out, ')');
}
static void counter_style_text(const struct css_counter_style *c, sbuf *out) {
    if (c->id < LS_PREDEFINED) sb_puts(out, counter_style_names[c->id]);
    else if (c->id == LS_STRING) css_serialize_string(out, c->name ? c->name : "");
    else if (c->id == LS_NAMED) css_serialize_ident(out, c->name ? c->name : "");
    else if (c->id == LS_SYMBOLS && c->symbols) symbols_text(c->symbols, out);
    else sb_puts(out, "none");
}
static bool symbols_equal(const struct css_symbols *a, const struct css_symbols *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type || a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) if (strcmp(a->symbol[i], b->symbol[i])) return false;
    return true;
}
static bool counter_style_equal(const struct css_counter_style *a, const struct css_counter_style *b) {
    return a->id == b->id && same_name(a->name, b->name) && symbols_equal(a->symbols, b->symbols);
}

/* list-style-type (CSS Lists 3 §3.4): <counter-style> | <string> | none. Its id is the style's
   list_style; a string, another name or symbols() is the list group's. */
static bool parse_list_type(const char *s, size_t n, struct cx *cx) {
    struct css_counter_style c = {0};
    if (ident_is(s, n, "none")) c.id = LS_NONE;
    else if (string_token(s, n)) { c.id = LS_STRING; c.name = string_value(s, n, cx->a); }
    else if (!parse_counter_style(s, n, cx, &c)) return false;
    cx->s->list_style = c.id;
    const struct css_list *l = css_style_list(cx->s);
    if (l->type_name != c.name || l->type_symbols != c.symbols) {
        struct css_list *own = group_write(cx->s, G_LIST, l, cx->a);
        own->type_name = c.name;
        own->type_symbols = c.symbols;
    }
    return true;
}
/* an <image> (CSS Images 3 §2): url() or a gradient function. image-set(), cross-fade() and
   element() are not supported; nor do they parse. */
static bool parse_image_value(const char *s, size_t n, struct cx *cx, const struct css_image **out) {
    struct css_image im = {0};
    if (prefix_is(s, n, "url(")) {
        if (s[n - 1] != ')' || !(im.url = css_parse_url(s, n, cx->a))) return false;
        im.kind = CSS_IMAGE_URL;
    } else {
        static const char *const gradients[] = {"linear-gradient(", "radial-gradient(", "conic-gradient(",
            "repeating-linear-gradient(", "repeating-radial-gradient(", "repeating-conic-gradient("};
        int which = -1;
        for (int i = 0; i < 6; i++) if (prefix_is(s, n, gradients[i])) which = i;
        if (which < 0 || s[n - 1] != ')') return false;
        im.gradient = parse_gradient(s, n, cx);
        if (!im.gradient && which % 3 != 2) return false; /* the background layer's parser reads all but conic */
        im.kind = CSS_IMAGE_GRADIENT;
        im.text = ar_strndup(cx->a, s, n);
    }
    struct css_image *r = ar_alloc(cx->a, sizeof *r);
    *r = im;
    *out = r;
    return true;
}
static void image_text(const struct css_image *im, sbuf *out) {
    if (im->kind != CSS_IMAGE_URL) { sb_puts(out, im->text ? im->text : ""); return; }
    sb_puts(out, "url(");
    css_serialize_string(out, im->url ? im->url : "");
    sb_putc(out, ')');
}
static bool image_equal(const struct css_image *a, const struct css_image *b) {
    if (a == b) return true;
    return a && b && a->kind == b->kind && same_name(a->url, b->url) && same_name(a->text, b->text);
}
/* exactly one component */
static bool single_token(const char *s, size_t n, const char **t, size_t *tn) {
    const char *p = s, *e = s + n, *rest;
    size_t rn;
    return value_token(&p, e, t, tn) && !value_token(&p, e, &rest, &rn);
}
/* list-style-image (§3.3): <image> | none */
static bool parse_list_image(const char *s, size_t n, struct cx *cx, const struct css_image **out) {
    const char *t;
    size_t tn;
    if (ident_is(s, n, "none")) { *out = NULL; return true; }
    return single_token(s, n, &t, &tn) && parse_image_value(t, tn, cx, out);
}
/* quotes (CSS Generated Content 3 §2.1): auto | none | [<string> <string>]+ */
static bool parse_quotes(const char *s, size_t n, struct cx *cx, const struct css_quotes **out) {
    if (ident_is(s, n, "auto")) { *out = NULL; return true; }
    if (ident_is(s, n, "none")) { *out = &quotes_none; return true; }
    const char *p = s, *e = s + n, *t;
    size_t tn;
    uint32_t count = 0;
    while (value_token(&p, e, &t, &tn)) {
        if (!string_token(t, tn)) return false;
        count++;
    }
    if (!count || count % 2) return false;
    struct css_quotes *q = ar_alloc(cx->a, sizeof *q + count * sizeof *q->quote);
    q->pairs = count / 2;
    count = 0;
    for (p = s; value_token(&p, e, &t, &tn);) q->quote[count++] = string_value(t, tn, cx->a);
    *out = q;
    return true;
}
static void quotes_text(const struct css_quotes *q, sbuf *out) {
    if (!q) { sb_puts(out, "auto"); return; }
    if (!q->pairs) { sb_puts(out, "none"); return; }
    for (uint32_t i = 0; i < 2 * q->pairs; i++) {
        if (i) sb_putc(out, ' ');
        css_serialize_string(out, q->quote[i]);
    }
}
static bool quotes_equal(const struct css_quotes *a, const struct css_quotes *b) {
    if (a == b) return true;
    if (!a || !b || a->pairs != b->pairs) return false;
    for (uint32_t i = 0; i < 2 * a->pairs; i++) if (strcmp(a->quote[i], b->quote[i])) return false;
    return true;
}
bool css_list_equal(const struct css_list *a, const struct css_list *b) {
    if (!a) a = &css_list_initial;
    if (!b) b = &css_list_initial;
    return a == b || (same_name(a->type_name, b->type_name) && symbols_equal(a->type_symbols, b->type_symbols) &&
                      image_equal(a->image, b->image) && quotes_equal(a->quotes, b->quotes));
}

/* an <integer> (CSS Values 4 §5.2), or a math function that resolves to a number, which an integer
   context rounds to the nearest integer, a half toward +infinity (§10.9); in range */
static bool integer_value(const char *s, size_t n, struct cx *cx, int32_t *out) {
    double d;
    if (css_calc_is_math(s, n)) {
        if (!css_numeric_read(s, n, CSS_CALC_NUMBER, cx, cx->a, NAN, &d, NULL)) return false;
        d = isnan(d) ? 0 : floor(d + 0.5);
    } else {
        size_t i = n && (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (i == n) return false;
        d = 0;
        for (size_t k = i; k < n; k++) {
            if (s[k] < '0' || s[k] > '9') return false;
            if (d < 1e12) d = d * 10 + (s[k] - '0');
        }
        if (s[0] == '-') d = -d;
    }
    *out = d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : (int32_t)d;
    return true;
}
/* counter-reset, counter-increment and counter-set (CSS Lists 3 §4.1-4.3): none, or
   [<counter-name> <integer>?]+, reversed(<counter-name>) also for counter-reset. A missing value is
   0 (1 for counter-increment), and a reversed one without a value is left for counting to find.
   specified: the value's CSSOM serialization as declared (its integers' math functions kept). */
static bool parse_counter_ops(const char *s, size_t n, struct cx *cx, int which, const struct css_counter_ops **out,
                              sbuf *specified) {
    if (ident_is(s, n, "none")) {
        *out = NULL;
        if (specified) sb_puts(specified, "none");
        return true;
    }
    const char *e = s + n, *t, *v = NULL;
    size_t tn, vn = 0;
    uint32_t count = 0;
    for (const char *p = s; value_token(&p, e, &t, &tn);) count++;
    if (!count) return false;
    struct css_counter_ops *ops = ar_alloc(cx->a, sizeof *ops + count * sizeof *ops->op);
    for (const char *p = s; value_token(&p, e, &t, &tn);) {
        struct css_counter_op op = {0};
        if (which == CSS_COUNTER_RESET && prefix_is(t, tn, "reversed(") && t[tn - 1] == ')') {
            const char *in = t + 9;
            size_t inn = tn - 10;
            trim(&in, &inn);
            op.reversed = true;
            op.name = custom_ident(in, inn, cx->a, "none");
        } else op.name = custom_ident(t, tn, cx->a, "none");
        if (!op.name) return false;
        const char *q = p;
        if (value_token(&q, e, &v, &vn) && integer_value(v, vn, cx, &op.value)) { op.valued = true; p = q; }
        else op.value = which == CSS_COUNTER_INCREMENT ? 1 : 0;
        if (specified) {
            if (ops->n) sb_putc(specified, ' ');
            if (op.reversed) sb_puts(specified, "reversed(");
            css_serialize_ident(specified, op.name);
            if (op.reversed) sb_putc(specified, ')');
            if (op.valued || !op.reversed) {
                char number[16];
                snprintf(number, sizeof number, " %d", (int)op.value);
                if (op.valued && css_calc_is_math(v, vn)) {
                    sb_putc(specified, ' ');
                    sbuf math = {0};
                    if (css_calc_specified(v, vn, CSS_CALC_NUMBER, &math)) sb_put(specified, math.p ? math.p : "", math.n);
                    else sb_put(specified, v, vn);
                    sb_free(&math);
                } else sb_puts(specified, number);
            }
        }
        ops->op[ops->n++] = op;
    }
    *out = ops;
    return true;
}
static void counter_ops_text(const struct css_counter_ops *ops, sbuf *out) {
    if (!ops || !ops->n) { sb_puts(out, "none"); return; }
    for (uint32_t i = 0; i < ops->n; i++) {
        const struct css_counter_op *op = &ops->op[i];
        if (i) sb_putc(out, ' ');
        if (op->reversed) sb_puts(out, "reversed(");
        css_serialize_ident(out, op->name);
        if (op->reversed) sb_putc(out, ')');
        if (op->valued || !op->reversed) {
            char number[16];
            snprintf(number, sizeof number, " %d", (int)op->value);
            sb_puts(out, number);
        }
    }
}
static bool counter_ops_equal(const struct css_counter_ops *a, const struct css_counter_ops *b) {
    if (a == b) return true;
    if (!a || !b || a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) {
        const struct css_counter_op *x = &a->op[i], *y = &b->op[i];
        if (strcmp(x->name, y->name) || x->value != y->value || x->reversed != y->reversed || x->valued != y->valued)
            return false;
    }
    return true;
}
bool css_counters_equal(const struct css_counters *a, const struct css_counters *b) {
    if (!a) a = &css_counters_initial;
    if (!b) b = &css_counters_initial;
    for (int i = 0; i < 3; i++) if (!counter_ops_equal(a->ops[i], b->ops[i])) return false;
    return true;
}

/* one item of content's list (alt: of its alternative text, which takes strings, counters and
   attr() only): <string>, counter(), counters(), a <quote>, attr() or an <image> */
static bool content_item(const char *t, size_t tn, struct cx *cx, bool alt, struct css_content_item *it) {
    static const char *const quotes[] = {"open-quote", "close-quote", "no-open-quote", "no-close-quote"};
    memset(it, 0, sizeof *it);
    it->style.id = LS_DECIMAL;
    if (string_token(t, tn)) { it->kind = CSS_CONTENT_STRING; it->text = string_value(t, tn, cx->a); return true; }
    for (int i = 0; i < 4; i++)
        if (ident_is(t, tn, quotes[i])) { it->kind = (uint8_t)(CSS_CONTENT_OPEN_QUOTE + i); return !alt; }
    bool plural = prefix_is(t, tn, "counters(");
    if ((plural || prefix_is(t, tn, "counter(")) && t[tn - 1] == ')') {
        size_t head = plural ? 9 : 8;
        const char *a[4];
        size_t al[4];
        int k = split(t + head, tn - head - 1, a, al, 4, ',');
        if (k < (plural ? 2 : 1) || k > (plural ? 3 : 2)) return false;
        it->kind = plural ? CSS_CONTENT_COUNTERS : CSS_CONTENT_COUNTER;
        if (!(it->text = custom_ident(a[0], al[0], cx->a, "none"))) return false;
        if (plural) {
            if (!string_token(a[1], al[1])) return false;
            it->separator = string_value(a[1], al[1], cx->a);
        }
        return k == (plural ? 2 : 1) || parse_counter_style(a[k - 1], al[k - 1], cx, &it->style);
    }
    if (prefix_is(t, tn, "attr(") && t[tn - 1] == ')') {
        const char *in = t + 5;
        size_t inn = tn - 6;
        trim(&in, &inn);
        size_t name = ident_token(in, inn);
        if (!name) return false;
        it->kind = CSS_CONTENT_STRING;
        node_t *e = cx->node;
        if (!e) { it->kind = CSS_CONTENT_ATTR; it->text = ar_strndup(cx->a, t, tn); return true; }
        /* CSS Values 5 §7.7: substituted at computed-value time; the attribute's value, or the fallback */
        const char *fallback = NULL;
        size_t fl = 0;
        const char *comma = memchr(in, ',', inn);
        if (comma) { fallback = comma + 1; fl = inn - (size_t)(comma + 1 - in); trim(&fallback, &fl); }
        for (int j = 0; j < e->nattrs; j++)
            if (!e->attrs[j].namespace_uri && strlen(e->attrs[j].raw) == name && !memcmp(in, e->attrs[j].raw, name)) {
                it->text = ar_strdup(cx->a, e->attrs[j].value);
                return true;
            }
        it->text = fallback && string_token(fallback, fl) ? string_value(fallback, fl, cx->a) : "";
        return true;
    }
    if (alt) return false;
    it->kind = CSS_CONTENT_IMAGE;
    return parse_image_value(t, tn, cx, &it->image);
}
/* content (CSS Generated Content 3 §1.1): normal | none | [<content-replacement> | <content-list>]
   [/ [<string> | <counter> | <attr()>]+]? */
static bool parse_content(const char *s, size_t n, struct cx *cx) {
    if (ident_is(s, n, "normal")) { cx->s->content = NULL; return true; }
    if (ident_is(s, n, "none")) { cx->s->content = &css_content_none; return true; }
    size_t slash = n;
    int depth = 0;
    char q = 0;
    for (size_t i = 0; i < n && slash == n; i++) {
        char c = s[i];
        if (c == '\\') { i++; continue; }
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (!depth && c == '/') slash = i;
    }
    const char *e = s + slash, *alt_end = s + n, *t;
    size_t tn;
    uint32_t main = 0, alt = 0;
    for (const char *p = s; value_token(&p, e, &t, &tn);) main++;
    if (slash < n) for (const char *p = e + 1; value_token(&p, alt_end, &t, &tn);) alt++;
    if (!main || (slash < n && !alt)) return false;
    struct css_content *c = ar_alloc(cx->a, sizeof *c + (main + alt) * sizeof *c->item);
    c->n = main;
    c->alt = alt;
    uint32_t i = 0;
    for (const char *p = s; value_token(&p, e, &t, &tn);) if (!content_item(t, tn, cx, false, &c->item[i++])) return false;
    if (slash < n)
        for (const char *p = e + 1; value_token(&p, alt_end, &t, &tn);)
            if (!content_item(t, tn, cx, true, &c->item[i++])) return false;
    cx->s->content = c;
    return true;
}
static void content_item_text(const struct css_content_item *it, sbuf *out) {
    static const char *const quotes[] = {"open-quote", "close-quote", "no-open-quote", "no-close-quote"};
    switch (it->kind) {
    case CSS_CONTENT_STRING: css_serialize_string(out, it->text ? it->text : ""); break;
    case CSS_CONTENT_COUNTER: case CSS_CONTENT_COUNTERS:
        sb_puts(out, it->kind == CSS_CONTENT_COUNTERS ? "counters(" : "counter(");
        css_serialize_ident(out, it->text);
        if (it->kind == CSS_CONTENT_COUNTERS) { sb_puts(out, ", "); css_serialize_string(out, it->separator); }
        if (it->style.id != LS_DECIMAL) { sb_puts(out, ", "); counter_style_text(&it->style, out); }
        sb_putc(out, ')');
        break;
    case CSS_CONTENT_OPEN_QUOTE: case CSS_CONTENT_CLOSE_QUOTE: case CSS_CONTENT_NO_OPEN_QUOTE:
    case CSS_CONTENT_NO_CLOSE_QUOTE: sb_puts(out, quotes[it->kind - CSS_CONTENT_OPEN_QUOTE]); break;
    case CSS_CONTENT_IMAGE: image_text(it->image, out); break;
    case CSS_CONTENT_ATTR: sb_puts(out, it->text); break;
    }
}
static void content_text(const struct css_content *c, sbuf *out) {
    if (!c) { sb_puts(out, "normal"); return; }
    if (c->none) { sb_puts(out, "none"); return; }
    for (uint32_t i = 0; i < c->n + c->alt; i++) {
        if (i == c->n) sb_puts(out, " / ");
        else if (i) sb_putc(out, ' ');
        content_item_text(&c->item[i], out);
    }
}
bool css_content_equal(const struct css_content *a, const struct css_content *b) {
    if (a == b) return true;
    if (!a || !b || a->none != b->none || a->n != b->n || a->alt != b->alt) return false;
    for (uint32_t i = 0; i < a->n + a->alt; i++) {
        const struct css_content_item *x = &a->item[i], *y = &b->item[i];
        if (x->kind != y->kind || !same_name(x->text, y->text) || !same_name(x->separator, y->separator) ||
            !counter_style_equal(&x->style, &y->style) || !image_equal(x->image, y->image)) return false;
    }
    return true;
}
bool css_content_counted(const struct css_content *c) {
    if (!c) return false;
    for (uint32_t i = 0; i < c->n; i++)
        if (c->item[i].kind != CSS_CONTENT_STRING && c->item[i].kind != CSS_CONTENT_IMAGE &&
            c->item[i].kind != CSS_CONTENT_ATTR) return true;
    return false;
}
void css_content_strings(const struct css_content *c, sbuf *out) {
    for (uint32_t i = 0; c && i < c->n; i++)
        if (c->item[i].kind == CSS_CONTENT_STRING) sb_puts(out, c->item[i].text);
}

/* CSS Lists 3 §3.1: the UA style sheet's ::marker rule, which the cascade applies under every author
   declaration of a list item's ::marker (css.c) and which a ::marker without any takes alone. Of
   its declarations, unicode-bidi and font-variant-numeric have no field here. */
void css_style_marker_defaults(style_t *s) {
    s->white_space = WS_PRE;
    s->text_transform = TT_NONE;
}
void css_style_marker(style_t *s, const style_t *item) {
    css_style_init(s, item);
    css_style_marker_defaults(s);
    css_style_finish(s, item, false);
}

static bool apply_short(const struct propdef *p, const char *v, size_t n, struct cx *cx);
/* the list-style shorthand (CSS Lists 3 §3.6): <'list-style-position'> || <'list-style-image'> ||
   <'list-style-type'>, where none goes to whichever of the image and the type the others leave */
static bool list_style_parts(const char *v, size_t n, struct cx *cx, const char *part[3], size_t length[3]) {
    const char *p = v, *e = v + n, *t;
    size_t tn;
    int nones = 0;
    part[0] = part[1] = part[2] = NULL; /* position, image, type */
    style_t scratch = initial;
    struct cx probe = *cx;
    probe.s = &scratch;
    probe.supports_probe = true;
    const struct css_image *image;
    uint8_t position;
    while (value_token(&p, e, &t, &tn)) {
        /* each component once: after the position, a second "outside" is a <counter-style-name> */
        int which;
        if (!part[0] && kw_find(kw_lspos, t, tn, &position)) which = 0;
        else if (ident_is(t, tn, "none")) { nones++; continue; }
        else if (!part[1] && parse_image_value(t, tn, &probe, &image)) which = 1;
        else if (!part[2] && parse_list_type(t, tn, &probe)) which = 2;
        else return false;
        part[which] = t;
        length[which] = tn;
    }
    if (nones > !part[1] + !part[2]) return false;
    if (nones) {
        if (!part[1]) { part[1] = "none"; length[1] = 4; }
        if (!part[2]) { part[2] = "none"; length[2] = 4; }
    }
    return true;
}
static void list_style_text(const style_t *st, sbuf *out) {
    const struct css_list *l = css_style_list(st);
    struct css_counter_style c = {st->list_style, l->type_name, l->type_symbols};
    /* a type named inside or outside alone would read back as the position: write the position first */
    bool positional = c.id == LS_NAMED && c.name && (!strcasecmp(c.name, "inside") || !strcasecmp(c.name, "outside"));
    bool any = false;
    if (st->list_style_inside || positional) { sb_puts(out, st->list_style_inside ? "inside" : "outside"); any = true; }
    if (l->image) { if (any) sb_putc(out, ' '); image_text(l->image, out); any = true; }
    if (st->list_style != LS_DISC) {
        if (any) sb_putc(out, ' ');
        counter_style_text(&c, out);
        any = true;
    }
    if (!any) sb_puts(out, "outside");
}
/* the computed value of a list, counter, quotes or content property */
static bool list_value_text(const style_t *st, const struct propdef *p, sbuf *out) {
    const struct css_list *l = css_style_list(st);
    switch (p->type) {
    case PT_LIST_TYPE: {
        struct css_counter_style c = {st->list_style, l->type_name, l->type_symbols};
        counter_style_text(&c, out);
        return true;
    }
    case PT_LIST_IMAGE:
        if (l->image) image_text(l->image, out);
        else sb_puts(out, "none");
        return true;
    case PT_QUOTES: quotes_text(l->quotes, out); return true;
    case PT_COUNTER: counter_ops_text(css_style_counters(st)->ops[p->flags], out); return true;
    case PT_CONTENT: content_text(st->content, out); return true;
    }
    return false;
}
/* CSSOM: the specified serialization of a declared list, counter, quotes or content value, or of the
   list-style shorthand; false leaves the authored text */
static bool list_specified(const struct propdef *p, const char *v, size_t n, sbuf *out) {
    props_init();
    arena_t a = {.chunk_size = 1024};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    size_t begin = out->n;
    if (!setjmp(trap)) {
        style_t scratch = initial;
        uint8_t set[NPROPS];
        memset(set, 0, sizeof set);
        struct cx cx = {.s = &scratch, .a = &a, .set = set, .supports_probe = true};
        if (p->type == PT_COUNTER) {
            const struct css_counter_ops *ops;
            ok = parse_counter_ops(v, n, &cx, p->flags, &ops, out);
        } else if (p->type == PT_SHORT) {
            ok = apply_short(p, v, n, &cx) && !cx.invalid;
            if (ok) list_style_text(&scratch, out);
        } else {
            ok = apply_long(p, v, n, &cx) && !cx.invalid;
            if (ok) list_value_text(&scratch, p, out);
        }
    }
    ar_free(&a);
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    return ok;
}

/* copy a longhand's value (all the fields it sets) */
/* ---------------------------------------------------------------- grid values */
/* one breadth: a length, auto, min-content, max-content, or (for the max) <n>fr */
static bool parse_breadth(const char *s, size_t n, struct cx *cx, uint8_t *kind, len_t *l, bool allow_fr) {
    memset(l, 0, sizeof *l);
    if (ident_is(s, n, "auto")) return *kind = GT_AUTO, true;
    if (ident_is(s, n, "min-content")) return *kind = GT_MIN, true;
    if (ident_is(s, n, "max-content")) return *kind = GT_MAX, true;
    if (n > 2 && strn_ieq(s + n - 2, "fr", 2)) {
        const char *q = s;
        float x;
        if (!parse_number(&q, s + n - 2, &x) || q != s + n - 2 || x < 0) return false;
        if (!allow_fr) return false;
        *kind = GT_FR;
        l->kind = LK_LEN;
        l->px = x;
        return true;
    }
    if (!parse_len(s, n, cx, l, 0)) return false;
    *kind = GT_LEN;
    return true;
}

static bool parse_track(const char *s, size_t n, struct cx *cx, struct gtrack *t) {
    memset(t, 0, sizeof *t);
    size_t k = 0;
    while (k < n && s[k] != '(') k++;
    if (k < n && s[n - 1] == ')' && (ident_is(s, k, "minmax") || ident_is(s, k, "fit-content"))) {
        const char *a[2];
        size_t al[2];
        int m = split_checked(s + k + 1, n - k - 2, a, al, 2, ',', cx);
        if (ident_is(s, k, "fit-content")) {
            if (m != 1 || !parse_len(a[0], al[0], cx, &t->max, 0)) return false;
            t->min_kind = GT_AUTO;
            t->max_kind = GT_FIT;
            return true;
        }
        if (m != 2) return false;
        if (!parse_breadth(a[0], al[0], cx, &t->min_kind, &t->min, false)) return false;
        return parse_breadth(a[1], al[1], cx, &t->max_kind, &t->max, true);
    }
    if (!parse_breadth(s, n, cx, &t->max_kind, &t->max, true)) return false;
    if (t->max_kind == GT_FR) t->min_kind = GT_AUTO;
    else {
        t->min_kind = t->max_kind;
        t->min = t->max;
    }
    return true;
}

/* a track list into v (at most max tracks); [line names] are skipped. rep_at, rep_n mark an
   auto-fill/auto-fit repeat (only one is allowed). */
static int parse_track_list(const char *s, size_t n, struct cx *cx, struct gtrack *v, int max, int *rep_at,
                            int *rep_n, bool *fit) {
    const char *t[64];
    size_t tl[64];
    int k = split_checked(s, n, t, tl, 64, ' ', cx), cnt = 0;
    bool in_names = false;
    for (int i = 0; i < k; i++) {
        if (in_names || t[i][0] == '[') {
            in_names = t[i][tl[i] - 1] != ']';
            continue;
        }
        size_t f = 0;
        while (f < tl[i] && t[i][f] != '(') f++;
        if (f < tl[i] && ident_is(t[i], f, "repeat") && t[i][tl[i] - 1] == ')') {
            const char *a[2];
            size_t al[2];
            if (split_checked(t[i] + f + 1, tl[i] - f - 2, a, al, 2, ',', cx) != 2) return -1;
            struct gtrack inner[32];
            int m = rep_at ? parse_track_list(a[1], al[1], cx, inner, 32, NULL, NULL, NULL) : -1;
            if (m <= 0) return -1;
            int times;
            if (ident_is(a[0], al[0], "auto-fill") || ident_is(a[0], al[0], "auto-fit")) {
                if (*rep_n) return -1;
                *rep_at = cnt;
                *rep_n = m;
                *fit = ident_is(a[0], al[0], "auto-fit");
                times = 1;
            } else {
                times = atoi(a[0]);
                if (times < 1) return -1;
            }
            for (int r = 0; r < times; r++)
                for (int j = 0; j < m; j++) {
                    if (cnt >= max) return -1;
                    v[cnt++] = inner[j];
                }
            continue;
        }
        if (cnt >= max || !parse_track(t[i], tl[i], cx, &v[cnt])) return -1;
        cnt++;
    }
    return cnt;
}

static bool parse_gtemplate(const char *s, size_t n, struct cx *cx, const struct gtemplate **out) {
    if (ident_is(s, n, "none")) {
        *out = NULL;
        return true;
    }
    struct gtrack v[256];
    int ra = 0, rn = 0;
    bool fit = false;
    int k = parse_track_list(s, n, cx, v, 256, &ra, &rn, &fit);
    if (k <= 0) return false;
    struct gtemplate *g = ar_alloc(cx->a, sizeof *g);
    g->t = ar_alloc(cx->a, sizeof *g->t * (size_t)k);
    memcpy(g->t, v, sizeof *g->t * (size_t)k);
    g->n = k;
    g->rep_at = ra;
    g->rep_n = rn;
    g->rep_fit = fit;
    *out = g;
    return true;
}

static bool parse_gareas(const char *s, size_t n, struct cx *cx, const struct gareas **out) {
    if (ident_is(s, n, "none")) {
        *out = NULL;
        return true;
    }
    const char *t[64];
    size_t tl[64];
    int rows = split_checked(s, n, t, tl, 64, ' ', cx);
    if (rows < 1) return false;
    const char *names[64][32];
    int cols = 0;
    for (int r = 0; r < rows; r++) {
        if (tl[r] < 2 || (t[r][0] != '"' && t[r][0] != '\'') || t[r][tl[r] - 1] != t[r][0]) return false;
        const char *p = t[r] + 1, *e = t[r] + tl[r] - 1;
        int c = 0;
        while (p < e) {
            if (is_space((unsigned char)*p)) {
                p++;
                continue;
            }
            const char *st = p;
            if (*p == '.') {
                while (p < e && *p == '.') p++;
                if (c < 32) names[r][c] = NULL;
            } else {
                while (p < e && !is_space((unsigned char)*p) && *p != '.') p++;
                if (c < 32) names[r][c] = ar_strndup(cx->a, st, (size_t)(p - st));
            }
            c++;
        }
        if (c > 32 || c < 1 || (r && c != cols)) return false;
        cols = c;
    }
    struct gareas *g = ar_alloc(cx->a, sizeof *g);
    g->rows = rows;
    g->cols = cols;
    g->cell = ar_alloc(cx->a, sizeof *g->cell * (size_t)(rows * cols));
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) g->cell[r * cols + c] = names[r][c];
    *out = g;
    return true;
}

static bool is_custom_ident(const char *s, size_t n) {
    if (!n || isdigit((unsigned char)s[0]) || s[0] == '+' || (s[0] == '-' && n > 1 && isdigit((unsigned char)s[1])))
        return false;
    return !ident_is(s, n, "auto") && !ident_is(s, n, "span");
}

/* animation-name and transition-property identifiers: <custom-ident> */
static bool motion_name(const char *s, size_t n) {
    if (!n || ident_is(s,n,"initial") || ident_is(s,n,"inherit") || ident_is(s,n,"unset") ||
        ident_is(s,n,"revert") || ident_is(s,n,"revert-layer") || ident_is(s,n,"default") ||
        isdigit((unsigned char)s[0]) || s[0]=='+') return false;
    if (s[0]=='-' && (n==1 || isdigit((unsigned char)s[1]))) return false;
    for (size_t i=0;i<n;i++) if ((unsigned char)s[i]<128 && !isalnum((unsigned char)s[i]) &&
        s[i]!='_' && s[i]!='-') return false;
    return true;
}
/* a <time> in milliseconds, or a math function of type <time> (CSS Values 4 §10); duration: it
   may not be negative, which a calculation clamps to 0 (§10.12) and a literal is invalid */
static bool motion_time(const char *s,size_t n,struct cx *cx,bool duration,float *out) {
    double v;
    bool math=css_calc_is_math(s,n);
    if (!css_numeric_read(s,n,CSS_CALC_TIME,cx,cx->a,NAN,&v,NULL)) return false;
    v*=1000;
    if (math) { if (duration && v<0) v=0; } /* the range clamps a calculation (§10.12) */
    else if (!isfinite(v) || (duration && v<0)) return false;
    *out=calc_float(v);return true;
}
/* an <easing-function>'s <number> or <integer>: a literal or a math function (no element) */
static bool easing_number(const char *s,size_t n,bool integer,float *out) {
    double v;
    bool math=css_calc_is_math(s,n);
    if (!css_numeric_read(s,n,CSS_CALC_NUMBER,NULL,NULL,NAN,&v,NULL) || (!math && !isfinite(v))) return false;
    if (integer) { if (math) v=floor(v+0.5); else if (v!=floor(v)) return false; }
    *out=calc_float(v);return true;
}
/* <easing-function> (css-easing-1 §2): linear, the cubic-bezier() keywords and function,
   step-start/step-end and steps(<integer>[, <step-position>]). */
bool css_easing_parse(const char *s,size_t n,struct css_easing *out) {
    trim(&s,&n);
    struct css_easing v={.kind=CM_BEZIER,.y2=1};
    static const struct {const char *name;float x1,y1,x2;uint8_t keyword;} named[]={
        {"ease",.25f,.1f,.25f,CM_EASE},{"ease-in",.42f,0,1,CM_EASE_IN},{"ease-out",0,0,.58f,CM_EASE_OUT},
        {"ease-in-out",.42f,0,.58f,CM_EASE_IN_OUT}};
    for(size_t i=0;i<sizeof named/sizeof *named;i++)if(ident_is(s,n,named[i].name)){
        v.x1=named[i].x1;v.y1=named[i].y1;v.x2=named[i].x2;v.keyword=named[i].keyword;*out=v;return true;
    }
    if(ident_is(s,n,"linear")){*out=(struct css_easing){.kind=CM_LINEAR};return true;}
    if(ident_is(s,n,"step-start")){*out=(struct css_easing){.kind=CM_STEPS,.steps=1,.position=CM_STEP_START};return true;}
    if(ident_is(s,n,"step-end")){*out=(struct css_easing){.kind=CM_STEPS,.steps=1,.position=CM_STEP_END};return true;}
    if(n>7 && strn_ieq(s,"steps(",6) && s[n-1]==')') {
        const char *t[3];size_t tl[3];int k=split(s+6,n-7,t,tl,3,',');
        if(k<1 || k>2)return false;
        float count;
        if(!easing_number(t[0],tl[0],true,&count) || count<1)return false;
        if(count>1e9f)count=1e9f; /* steps beyond a float's integers are not told apart */
        uint8_t position=CM_STEP_END;
        if(k==2) {
            static const struct kw positions[]={{"end",CM_STEP_END},{"jump-end",CM_STEP_END},{"start",CM_STEP_START},
                {"jump-start",CM_STEP_JUMP_START},{"jump-both",CM_STEP_JUMP_BOTH},{"jump-none",CM_STEP_JUMP_NONE},{0}};
            if(!kw_find(positions,t[1],tl[1],&position))return false;
        }
        if(position==CM_STEP_JUMP_NONE && count<2)return false;
        *out=(struct css_easing){.kind=CM_STEPS,.steps=(uint32_t)count,.position=position};return true;
    }
    if(n<15 || !strn_ieq(s,"cubic-bezier(",13) || s[n-1]!=')')return false;
    const char *t[5];size_t tl[5];float a[4];
    if(split(s+13,n-14,t,tl,5,',')!=4)return false;
    for(int i=0;i<4;i++) if(!easing_number(t[i],tl[i],false,&a[i]))return false;
    if(a[0]<0 || a[0]>1 || a[2]<0 || a[2]>1)return false;
    v.x1=a[0];v.y1=a[1];v.x2=a[2];v.y2=a[3];
    *out=v;return true;
}
static double bezier_at(double t,double p1,double p2){double u=1-t;return 3*u*u*t*p1+3*u*t*t*p2+t*t*t;}
double css_easing_apply(const struct css_easing *e,double x,bool before) {
    if(!e || e->kind==CM_LINEAR || !isfinite(x))return x;
    if(e->kind==CM_STEPS) {
        /* css-easing-1 §2.3 step easing function */
        double steps=e->steps?e->steps:1,current=floor(x*steps);
        if(e->position==CM_STEP_START || e->position==CM_STEP_JUMP_START || e->position==CM_STEP_JUMP_BOTH)current+=1;
        if(before && fmod(x*steps,1)==0)current-=1;
        if(x>=0 && current<0)current=0;
        double jumps=e->position==CM_STEP_JUMP_BOTH?steps+1:e->position==CM_STEP_JUMP_NONE?steps-1:steps;
        if(x<=1 && current>jumps)current=jumps;
        return current/jumps;
    }
    double x1=e->x1,y1=e->y1,x2=e->x2,y2=e->y2;
    /* §2.2: outside [0, 1] the curve continues along its end tangents */
    if(x<0) {
        if(x1>0)return y1/x1*x;
        if(y1==0 && x2>0)return y2/x2*x;
        return 0;
    }
    if(x>1) {
        if(x2<1)return 1+(y2-1)/(x2-1)*(x-1);
        if(y2==1 && x1<1)return 1+(y1-1)/(x1-1)*(x-1);
        return 1;
    }
    if(x==0 || x==1)return x;
    double t=x;
    for(int i=0;i<8;i++) {
        double err=bezier_at(t,x1,x2)-x,u=1-t,slope=3*u*u*x1+6*u*t*(x2-x1)+3*t*t*(1-x2);
        if(fabs(err)<1e-9)return bezier_at(t,y1,y2);
        if(fabs(slope)<1e-6)break;
        t-=err/slope;
    }
    double lo=0,hi=1;t=x;
    for(int i=0;i<60;i++){double z=bezier_at(t,x1,x2);if(fabs(z-x)<1e-9)break;if(z<x)lo=t;else hi=t;t=(lo+hi)/2;}
    return bezier_at(t,y1,y2);
}
bool css_easing_serialize(const struct css_easing *e,sbuf *out) {
    static const char *const keywords[]={NULL,"ease","ease-in","ease-out","ease-in-out"};
    char text[160];
    if(e->kind==CM_LINEAR)snprintf(text,sizeof text,"linear");
    else if(e->kind==CM_STEPS) {
        static const char *const positions[]={"",", start",", jump-start",", jump-both",", jump-none"};
        snprintf(text,sizeof text,"steps(%u%s)",(unsigned)e->steps,positions[e->position<5?e->position:0]);
    } else if(e->keyword && e->keyword<5)snprintf(text,sizeof text,"%s",keywords[e->keyword]);
    else snprintf(text,sizeof text,"cubic-bezier(%.7g, %.7g, %.7g, %.7g)",(double)e->x1,(double)e->y1,(double)e->x2,(double)e->y2);
    sb_puts(out,text);return true;
}
bool css_easing_same(const struct css_easing *a, const struct css_easing *b) {
    return a->kind == b->kind && a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2 &&
        a->steps == b->steps && a->position == b->position && a->keyword == b->keyword;
}
static union css_motion_item motion_initial(int list) {
    union css_motion_item v;
    memset(&v, 0, sizeof v);
    if (list == ML_EASING || list == ML_TEASING) v.e = (struct css_easing){.x1=.25f,.y1=.1f,.x2=.25f,.y2=1,.kind=CM_BEZIER,.keyword=CM_EASE};
    else if (list == ML_ITERATIONS) v.f = 1;
    else if (list == ML_TPROPERTY) v.s = "all";
    return v;
}
union css_motion_item css_motion_item(const style_t *st, int list, uint32_t i) {
    const struct css_motion_list *l = st->motion.list[list];
    return l && l->count ? l->v[i % l->count] : motion_initial(list);
}
uint32_t css_motion_count(const style_t *st, int list) {
    const struct css_motion_list *l = st->motion.list[list];
    return l && l->count ? l->count : 1;
}
static bool motion_item_equal(int list, union css_motion_item a, union css_motion_item b) {
    switch (list) {
    case ML_NAME: case ML_TPROPERTY: return a.s == b.s || (a.s && b.s && !strcmp(a.s, b.s));
    case ML_EASING: case ML_TEASING: return css_easing_same(&a.e, &b.e);
    case ML_DURATION: case ML_DELAY: case ML_ITERATIONS: case ML_TDURATION: case ML_TDELAY:
        return a.f == b.f || (isnan(a.f) && isnan(b.f));
    }
    return a.k == b.k;
}
bool css_motion_equal(const struct css_motion *a, const struct css_motion *b) {
    style_t x, y;
    x.motion = *a; y.motion = *b;
    for (int list = 0; list < ML_COUNT; list++) {
        if (a->list[list] == b->list[list]) continue;
        uint32_t count = css_motion_count(&x, list);
        if (count != css_motion_count(&y, list)) return false;
        for (uint32_t i = 0; i < count; i++)
            if (!motion_item_equal(list, css_motion_item(&x, list, i), css_motion_item(&y, list, i))) return false;
    }
    return true;
}
/* one item of an animation-* or transition-* list */
static bool motion_item_parse(int list, const struct kw *kws, const char *s, size_t n, struct cx *cx, union css_motion_item *out) {
    trim(&s, &n);
    if (!n) return false;
    *out = motion_initial(list);
    uint8_t k;
    const char *q = s;
    switch (list) {
    case ML_NAME:
        if (ident_is(s, n, "none")) { out->s = NULL; return true; }
        if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
            /* <keyframes-name>: a <string>, not the empty one */
            sbuf b = {0}; css_unescape(&b, s + 1, n - 2);
            bool named = b.n != 0;
            if (named) out->s = ar_strdup(cx->a, sb_cstr(&b));
            sb_free(&b);
            return named;
        }
        if (!motion_name(s, n)) return false;
        out->s = ar_strndup(cx->a, s, n);
        return true;
    case ML_DURATION:
        if (ident_is(s, n, "auto")) { out->f = NAN; return true; }
        return motion_time(s, n, cx, true, &out->f);
    case ML_TDURATION: return motion_time(s, n, cx, true, &out->f);
    case ML_DELAY: case ML_TDELAY: return motion_time(s, n, cx, false, &out->f);
    case ML_EASING: case ML_TEASING: return css_easing_parse(s, n, &out->e);
    case ML_ITERATIONS:
        if (ident_is(s, n, "infinite")) { out->f = INFINITY; return true; }
        if (css_calc_is_math(s, n)) {
            /* a calculation clamps to [0, infinity], which the count keeps (§10.12) */
            double v;
            if (!css_numeric_read(s, n, CSS_CALC_NUMBER, cx, cx->a, NAN, &v, NULL)) return false;
            out->f = v < 0 ? 0 : v > FLT_MAX ? INFINITY : (float)v;
            return true;
        }
        return parse_number(&q, s + n, &out->f) && q == s + n && isfinite(out->f) && out->f >= 0;
    case ML_TPROPERTY: {
        if (ident_is(s, n, "all")) { out->s = "all"; return true; }
        if (ident_is(s, n, "none") || !motion_name(s, n)) return false;
        /* a supported property keeps its lowercase name; any other identifier is kept as written */
        const struct propdef *p = n > 2 && s[0] == '-' && s[1] == '-' ? NULL : css_prop_lookup(s, n);
        char *copy = ar_strndup(cx->a, s, n);
        if (p) for (size_t i = 0; i < n; i++) copy[i] = (char)lower((unsigned char)copy[i]);
        out->s = copy;
        return true;
    }
    }
    if (!kw_find(kws, s, n, &k)) return false;
    out->k = k;
    return true;
}
/* the top-level comma-separated items of v: their count, and the first max of them */
static size_t motion_items(const char *v, size_t n, const char **items, size_t *lengths, size_t max) {
    size_t count = 0, start = 0;
    int depth = 0; char quote = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = i < n ? v[i] : ',';
        if (quote && i < n) { if (c == '\\') i++; else if (c == quote) quote = 0; continue; }
        if (i < n && (c == '"' || c == '\'')) quote = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (c == ',' && (depth <= 0 || i == n)) { /* the end closes what is open (CSS Syntax) */
            if (count < max) { items[count] = v + start; lengths[count] = i - start; }
            count++; start = i + 1;
        }
    }
    return count;
}
static struct css_motion_list *motion_list_new(struct cx *cx, size_t count) {
    struct css_motion_list *l = ar_alloc(cx->a, sizeof *l + count * sizeof *l->v);
    l->count = (uint32_t)count;
    return l;
}
static bool parse_motion_list(const struct propdef *p, const char *v, size_t n, struct cx *cx, const struct css_motion_list **out) {
    size_t count = motion_items(v, n, NULL, NULL, 0);
    if (!count || count > 1u << 20) return false;
    const char **items = ar_alloc(cx->a, count * sizeof *items);
    size_t *lengths = ar_alloc(cx->a, count * sizeof *lengths);
    motion_items(v, n, items, lengths, count);
    struct css_motion_list *l = motion_list_new(cx, count);
    for (size_t i = 0; i < count; i++) {
        const char *s = items[i]; size_t len = lengths[i];
        trim(&s, &len);
        /* transition-property: none stands alone */
        if (p->flags == ML_TPROPERTY && ident_is(s, len, "none")) {
            if (count != 1) return false;
            l->v[0].s = NULL;
            continue;
        }
        if (!motion_item_parse(p->flags, p->kws, s, len, cx, &l->v[i])) return false;
    }
    *out = l;
    return true;
}
/* <single-animation> (CSS Animations 1 §3.11): the first <time> is the duration, the second the
   delay; a keyword another longhand takes is that longhand's before it can be a name */
static bool animation_single(const char *s, size_t n, struct cx *cx, union css_motion_item *v) {
    const char *t[10]; size_t tl[10];
    int k = split(s, n, t, tl, 10, ' ');
    if (!k || k > 8) return false;
    for (int list = ML_NAME; list <= ML_PLAY; list++) v[list] = motion_initial(list);
    unsigned seen = 0, times = 0;
    for (int i = 0; i < k; i++) {
        union css_motion_item item;
        if (motion_item_parse(ML_TDELAY, NULL, t[i], tl[i], cx, &item)) {
            if (times == 0) { if (!motion_time(t[i], tl[i], cx, true, &item.f)) return false; v[ML_DURATION] = item; }
            else if (times == 1) v[ML_DELAY] = item;
            else return false;
            times++; continue;
        }
        if (!(seen & 1) && motion_item_parse(ML_EASING, NULL, t[i], tl[i], cx, &item)) { v[ML_EASING] = item; seen |= 1; continue; }
        if (!(seen & 2) && motion_item_parse(ML_ITERATIONS, NULL, t[i], tl[i], cx, &item)) { v[ML_ITERATIONS] = item; seen |= 2; continue; }
        if (!(seen & 4) && motion_item_parse(ML_DIRECTION, kw_motion_direction, t[i], tl[i], cx, &item)) { v[ML_DIRECTION] = item; seen |= 4; continue; }
        if (!(seen & 8) && motion_item_parse(ML_FILL, kw_motion_fill, t[i], tl[i], cx, &item)) { v[ML_FILL] = item; seen |= 8; continue; }
        if (!(seen & 16) && motion_item_parse(ML_PLAY, kw_motion_play, t[i], tl[i], cx, &item)) { v[ML_PLAY] = item; seen |= 16; continue; }
        if (!(seen & 32) && motion_item_parse(ML_NAME, NULL, t[i], tl[i], cx, &item)) { v[ML_NAME] = item; seen |= 32; continue; }
        return false;
    }
    return true;
}
/* <single-transition> (CSS Transitions 1 §2.5, 2 §2): [none | <single-transition-property>] || <time> ||
   <easing-function> || <time> || <transition-behavior-value> */
static bool transition_single(const char *s, size_t n, struct cx *cx, union css_motion_item *v, bool *none) {
    const char *t[8]; size_t tl[8];
    int k = split(s, n, t, tl, 8, ' ');
    if (!k || k > 5) return false;
    for (int list = ML_TPROPERTY; list <= ML_TBEHAVIOR; list++) v[list] = motion_initial(list);
    unsigned seen = 0, times = 0;
    *none = false;
    for (int i = 0; i < k; i++) {
        union css_motion_item item;
        if (motion_item_parse(ML_TDELAY, NULL, t[i], tl[i], cx, &item)) {
            if (times == 0) { if (!motion_time(t[i], tl[i], cx, true, &item.f)) return false; v[ML_TDURATION] = item; }
            else if (times == 1) v[ML_TDELAY] = item;
            else return false;
            times++; continue;
        }
        if (!(seen & 1) && motion_item_parse(ML_TEASING, NULL, t[i], tl[i], cx, &item)) { v[ML_TEASING] = item; seen |= 1; continue; }
        if (!(seen & 2) && motion_item_parse(ML_TBEHAVIOR, kw_transition_behavior, t[i], tl[i], cx, &item)) { v[ML_TBEHAVIOR] = item; seen |= 2; continue; }
        if (!(seen & 4) && ident_is(t[i], tl[i], "none")) { v[ML_TPROPERTY].s = NULL; *none = true; seen |= 4; continue; }
        if (!(seen & 4) && motion_item_parse(ML_TPROPERTY, NULL, t[i], tl[i], cx, &item)) { v[ML_TPROPERTY] = item; seen |= 4; continue; }
        return false;
    }
    return true;
}
/* the animation or transition shorthand: every longhand gets a list of the same length */
static bool motion_shorthand(bool animation, const char *v, size_t n, struct cx *cx, struct css_motion *out) {
    size_t count = motion_items(v, n, NULL, NULL, 0);
    if (!count || count > 1u << 20) return false;
    const char **items = ar_alloc(cx->a, count * sizeof *items);
    size_t *lengths = ar_alloc(cx->a, count * sizeof *lengths);
    motion_items(v, n, items, lengths, count);
    int first = animation ? ML_NAME : ML_TPROPERTY, last = animation ? ML_PLAY : ML_TBEHAVIOR;
    struct css_motion_list *lists[ML_COUNT] = {0};
    for (int list = first; list <= last; list++) lists[list] = motion_list_new(cx, count);
    for (size_t i = 0; i < count; i++) {
        union css_motion_item item[ML_COUNT];
        const char *s = items[i]; size_t len = lengths[i];
        trim(&s, &len);
        bool none = false;
        if (!len || !(animation ? animation_single(s, len, cx, item) : transition_single(s, len, cx, item, &none))) return false;
        if (none && count != 1) return false;
        for (int list = first; list <= last; list++) lists[list]->v[i] = item[list];
    }
    memset(out, 0, sizeof *out);
    for (int list = first; list <= last; list++) out->list[list] = lists[list];
    return true;
}
static bool parse_gline(const char *s, size_t n, struct cx *cx, struct gline *g) {
    memset(g, 0, sizeof *g);
    if (ident_is(s, n, "auto")) return true;
    const char *t[3];
    size_t tl[3];
    int k = split_checked(s, n, t, tl, 3, ' ', cx);
    bool span = false;
    int num = 0;
    const char *name = NULL;
    size_t name_n = 0;
    for (int i = 0; i < k; i++) {
        if (ident_is(t[i], tl[i], "span")) span = true;
        else if (isdigit((unsigned char)t[i][0]) || t[i][0] == '-' || t[i][0] == '+') {
            const char *q = t[i];
            float x;
            if (!parse_number(&q, t[i] + tl[i], &x) || q != t[i] + tl[i] || x == 0) return false;
            num = (int)x;
        } else if (is_custom_ident(t[i], tl[i])) {
            name = t[i];
            name_n = tl[i];
        } else return false;
    }
    if (span) {
        g->kind = GL_SPAN;
        g->n = (int16_t)(num > 0 ? (num > 1000 ? 1000 : num) : 1);
        return num >= 0;
    }
    if (name) {
        g->kind = GL_NAME;
        g->name = ar_strndup(cx->a, name, name_n);
        g->n = (int16_t)(num ? num : 1);
        return true;
    }
    if (!num) return false;
    g->kind = GL_LINE;
    g->n = (int16_t)(num > 1000 ? 1000 : num < -1000 ? -1000 : num);
    return true;
}

/* src's value of p into dst; a: where dst's copy of a group goes */
static void copy_prop(const struct propdef *p, style_t *dst, const style_t *src, arena_t *a) {
    if (p->group != G_STYLE) {
        if (group_same(dst, src, p->group)) return;
        char *own = group_write(dst, p->group, group_of(dst, p->group), a);
        memcpy(own + p->off, field_of(src, p), p->size);
        return;
    }
    memcpy((char *)dst + p->off, (const char *)src + p->off, p->size);
    switch (p->type) {
    case PT_DISPLAY: dst->legacy_box = src->legacy_box; break;
    case PT_FONT_FAMILY: dst->font_names=src->font_names;dst->named_font=src->named_font;dst->font_choices=src->font_choices;break;
    case PT_VALIGN: dst->vertical_align_px = src->vertical_align_px; break;
    case PT_LIST_TYPE: {
        const struct css_list *x = css_style_list(dst), *y = css_style_list(src);
        if (x->type_name != y->type_name || x->type_symbols != y->type_symbols) {
            struct css_list *own = group_write(dst, G_LIST, x, a);
            own->type_name = y->type_name;
            own->type_symbols = y->type_symbols;
        }
        break;
    }
    case PT_BG_IMAGE:
        dst->bg_img = src->bg_img;
        dst->bg_layers = src->bg_layers;
        dst->bg_layer = src->bg_layer;
        dst->has_grad = src->has_grad;
        dst->gradient = src->gradient;
        dst->grad[0] = src->grad[0];
        dst->grad[1] = src->grad[1];
        break;
    case PT_ZINDEX: dst->z_auto = src->z_auto; break;
    case PT_BG_SIZE: dst->bg_size_kind = src->bg_size_kind; break;
    case PT_COLOR_SCHEME: dst->color_scheme_text = src->color_scheme_text; break;
    }
}

static bool parse_translate_value(const char *v,size_t n,struct cx *cx,struct css_translate *out){
    struct css_translate t={.xy={{.kind=LK_LEN},{.kind=LK_LEN}}};
    if(ident_is(v,n,"none")){*out=t;return true;}
    const char *parts[3];size_t lengths[3];int count=split_checked(v,n,parts,lengths,3,' ',cx);
    if(count<1||count>2)return false; /* no invented 3D support */
    for(int i=0;i<count;i++)if(!parse_len(parts[i],lengths[i],cx,&t.xy[i],LF_NEG|LF_LENGTH))return false;
    t.active=true;*out=t;return true;
}
/* a <number> (percentage: or a <percentage>, as a fraction) of a transform: a literal or a math function */
static bool transform_number(const char *s,size_t n,struct cx *cx,bool percentage,float *out){
    double v;unsigned kind;
    bool math=css_calc_is_math(s,n);
    if(!css_numeric_read(s,n,CSS_CALC_NUMBER|(percentage?CSS_CALC_PERCENT:0),cx,cx->a,NAN,&v,&kind))return false;
    if(kind==CSS_CALC_PERCENT)v/=100;
    if(!math&&(!isfinite(v)||fabs(v)>FLT_MAX))return false;
    *out=calc_float(v);return true;
}
/* an <angle> of a transform in radians: a literal (unitless zero too) or a math function of type <angle> */
static bool transform_angle(const char *s,size_t n,struct cx *cx,float *out){
    double v;unsigned kind;
    bool math=css_calc_is_math(s,n);
    if(!css_numeric_read(s,n,CSS_CALC_ANGLE|(math?0:CSS_CALC_NUMBER),cx,cx->a,NAN,&v,&kind))return false;
    if(kind==CSS_CALC_NUMBER&&v!=0)return false; /* unitless zero only */
    if(!isfinite(v)){
        if(!math)return false;
        v=0; /* an infinite or NaN calculation is no turn: it has no angle in range to clamp to */
    }
    double r=v*M_PI/180;
    *out=(float)r;return isfinite(*out);
}
static struct css_transform transform_identity(uint8_t kind){
    struct css_transform t={.xy={{.kind=LK_LEN},{.kind=LK_LEN}},.kind=kind};
    if(kind==CT_SCALE)t.v[0]=t.v[1]=t.v[2]=1;
    if(kind==CT_ROTATE)t.v[2]=1;
    for(int i=0;i<4;i++)t.m[i*5]=1;
    return t;
}
/* One transform function from its name and comma-separated arguments. */
static bool transform_function(const char *name,size_t nn,const char **a,const size_t *al,int count,struct cx *cx,struct css_transform *t){
#define IS(s) ident_is(name,nn,s)
    if(IS("matrix")||IS("matrix3d")){
        bool three=IS("matrix3d");float v[16];
        if(count!=(three?16:6))return false;
        for(int i=0;i<count;i++)if(!transform_number(a[i],al[i],cx,false,&v[i]))return false;
        *t=transform_identity(CT_MATRIX);
        if(three){for(int r=0;r<4;r++)for(int c=0;c<4;c++)t->m[r*4+c]=v[c*4+r];} /* column-major input */
        else{t->m[0]=v[0];t->m[4]=v[1];t->m[1]=v[2];t->m[5]=v[3];t->m[3]=v[4];t->m[7]=v[5];}
        return true;
    }
    if(IS("translate")||IS("translateX")||IS("translateY")||IS("translateZ")||IS("translate3d")){
        *t=transform_identity(CT_TRANSLATE);
        bool both=IS("translate"),three=IS("translate3d"),y=IS("translateY"),z=IS("translateZ");
        if(three?count!=3:both?(count<1||count>2):count!=1)return false;
        int planar=z?0:three?2:count;
        for(int i=0;i<planar;i++)if(!parse_len(a[i],al[i],cx,&t->xy[y?1:i],LF_NEG|LF_LENGTH))return false;
        if(z||three){
            len_t depth;int at=z?0:2;
            if(memchr(a[at],'%',al[at])||!parse_len(a[at],al[at],cx,&depth,LF_NEG|LF_NOPCT|LF_LENGTH))return false;
            t->v[0]=len_resolve(&depth,0);
        }
        return true;
    }
    if(IS("scale")||IS("scaleX")||IS("scaleY")||IS("scaleZ")||IS("scale3d")){
        *t=transform_identity(CT_SCALE);
        bool both=IS("scale"),three=IS("scale3d");float v[3];
        if(three?count!=3:both?(count<1||count>2):count!=1)return false;
        for(int i=0;i<count;i++)if(!transform_number(a[i],al[i],cx,true,&v[i]))return false;
        if(three){t->v[0]=v[0];t->v[1]=v[1];t->v[2]=v[2];}
        else if(both){t->v[0]=v[0];t->v[1]=count>1?v[1]:v[0];}
        else t->v[IS("scaleX")?0:IS("scaleY")?1:2]=v[0];
        return true;
    }
    if(IS("rotate")||IS("rotateZ")||IS("rotateX")||IS("rotateY")||IS("rotate3d")){
        *t=transform_identity(CT_ROTATE);
        if(IS("rotate3d")){
            if(count!=4)return false;
            for(int i=0;i<3;i++)if(!transform_number(a[i],al[i],cx,false,&t->v[i]))return false;
            return transform_angle(a[3],al[3],cx,&t->v[3]);
        }
        if(count!=1||!transform_angle(a[0],al[0],cx,&t->v[3]))return false;
        if(IS("rotateX")){t->v[0]=1;t->v[2]=0;}
        else if(IS("rotateY")){t->v[1]=1;t->v[2]=0;}
        return true;
    }
    if(IS("skew")||IS("skewX")||IS("skewY")){
        *t=transform_identity(CT_SKEW);
        if(IS("skew")?(count<1||count>2):count!=1)return false;
        for(int i=0;i<count;i++)if(!transform_angle(a[i],al[i],cx,&t->v[IS("skewY")?1:i]))return false;
        return true;
    }
    if(IS("perspective")){
        *t=transform_identity(CT_PERSPECTIVE);
        if(count!=1)return false;
        if(ident_is(a[0],al[0],"none")){t->v[1]=1;return true;}
        len_t depth;if(!parse_len(a[0],al[0],cx,&depth,LF_NOPCT|LF_LENGTH))return false;
        t->v[0]=len_resolve(&depth,0);return true;
    }
#undef IS
    return false;
}
static bool parse_transform_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *p=v,*end=v+n;struct css_transform *first=NULL,**tail=&first;
    while(p<end){
        while(p<end&&isspace((unsigned char)*p))p++;
        if(p==end)break;
        const char *name=p;while(p<end&&isalnum((unsigned char)*p))p++;
        size_t name_length=(size_t)(p-name);
        if(!name_length||p==end||*p!='(')return false;
        const char *args=++p;unsigned depth=1;
        while(p<end&&depth){if(*p=='(')depth++;else if(*p==')')depth--;if(depth)p++;}
        if(depth)return false;
        const char *parts[17];size_t lengths[17];int count=split_checked(args,(size_t)(p-args),parts,lengths,17,',',cx);
        struct css_transform value;
        if(!transform_function(name,name_length,parts,lengths,count,cx,&value))return false;
        struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*tail=node;tail=&node->next;p++;
    }
    if(!first)return false;*out=first;return true;
}
bool css_transform_string_matrix(const char *s,size_t n,double m[16],bool *two_d){
    static const char identity2d[]="matrix(1, 0, 0, 1, 0, 0)";
    if(!n){s=identity2d;n=sizeof identity2d-1;}
    if(n>65536)return false;
    /* A comment separates tokens as white space does: "none" followed by a comment is none, a
       comment between a function's name and its parenthesis leaves no function. */
    char *text=malloc(n+1);if(!text)return false;
    size_t len=0;
    for(size_t i=0;i<n;){
        if(s[i]=='/'&&i+1<n&&s[i+1]=='*'){
            const char *close=NULL;for(size_t j=i+2;j+1<n;j++)if(s[j]=='*'&&s[j+1]=='/'){close=s+j;break;}
            text[len++]=' ';i=close?(size_t)(close-s)+2:n;continue;
        }
        text[len++]=s[i++];
    }
    const char *v=text;size_t vn=len;trim(&v,&vn);
    bool ok=false,flat=true;
    arena_t *a=calloc(1,sizeof *a);
    jmp_buf trap;
    if(a){
        a->limit=1u<<20;a->trap=&trap;
        if(!setjmp(trap)){
            style_t style,parent;css_style_init(&parent,NULL);css_style_init(&style,&parent);
            struct cx cx={.s=&style,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=a,
                .set=ar_alloc(a,(size_t)css_prop_count()),.supports_probe=true,.absolute_lengths=true};
            const struct css_transform *t=NULL;
            if(parse_transform_value(v,vn,&cx,&t)&&!cx.invalid){
                double r[16];css_transform_list_matrix_3d(t,r);
                for(int c=0;c<4;c++)for(int row=0;row<4;row++)m[c*4+row]=r[row*4+c];
                ok=true;
                /* 2D-ness is by function name, not by value: translate3d(0, 0, 0) is 3D. */
                static const char *const three[]={"matrix3d","translate3d","translateZ","scale3d","scaleZ","rotate3d","rotateX","rotateY","perspective"};
                for(size_t i=0;i<vn;){
                    if(!isalpha((unsigned char)v[i])){i++;continue;}
                    size_t start=i;while(i<vn&&(isalnum((unsigned char)v[i])||v[i]=='-'))i++;
                    if(i<vn&&v[i]=='(')for(size_t k=0;k<sizeof three/sizeof *three;k++)if(ident_is(v+start,i-start,three[k]))flat=false;
                }
            }
        }
        ar_free(a);free(a);
    }
    free(text);
    if(ok)*two_d=flat;
    return ok;
}
/* rotate: none | <angle> | [x | y | z | <number>{3}] && <angle> */
static bool parse_rotate_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *t[5];size_t tl[5];int k=split_checked(v,n,t,tl,5,' ',cx);
    struct css_transform value=transform_identity(CT_ROTATE);
    if(k==1){if(!transform_angle(t[0],tl[0],cx,&value.v[3]))return false;}
    else if(k==2||k==4){
        int angle=transform_angle(t[0],tl[0],cx,&value.v[3])?0:k-1;
        if(angle&&!transform_angle(t[angle],tl[angle],cx,&value.v[3]))return false;
        int axis=angle?0:1;
        if(k==2){
            value.v[2]=0;
            if(ident_is(t[axis],tl[axis],"x"))value.v[0]=1;
            else if(ident_is(t[axis],tl[axis],"y"))value.v[1]=1;
            else if(ident_is(t[axis],tl[axis],"z"))value.v[2]=1;
            else return false;
        }else for(int i=0;i<3;i++)if(!transform_number(t[axis+i],tl[axis+i],cx,false,&value.v[i]))return false;
    }else return false;
    struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*out=node;return true;
}
/* scale: none | [<number> | <percentage>]{1,3} */
static bool parse_scale_value(const char *v,size_t n,struct cx *cx,const struct css_transform **out){
    if(ident_is(v,n,"none")){*out=NULL;return true;}
    const char *t[4];size_t tl[4];int k=split_checked(v,n,t,tl,4,' ',cx);
    if(k<1||k>3)return false;
    struct css_transform value=transform_identity(CT_SCALE);
    for(int i=0;i<k;i++)if(!transform_number(t[i],tl[i],cx,true,&value.v[i]))return false;
    if(k==1)value.v[1]=value.v[0];
    struct css_transform *node=ar_alloc(cx->a,sizeof *node);*node=value;*out=node;return true;
}
/* transform-origin: one or two position components plus an ignored z length. */
static bool parse_transform_origin(const char *v,size_t n,struct cx *cx,len_t out[2]){
    const char *t[4];size_t tl[4];int k=split_checked(v,n,t,tl,4,' ',cx);
    if(k<1||k>3)return false;
    len_t r[2]={{.kind=LK_LEN,.pct=50},{.kind=LK_LEN,.pct=50}};
    int planar=k>2?2:k;
    bool vertical_first=ident_is(t[0],tl[0],"top")||ident_is(t[0],tl[0],"bottom")||
        (planar==2&&(ident_is(t[1],tl[1],"left")||ident_is(t[1],tl[1],"right")));
    for(int i=0;i<planar;i++){
        int axis=vertical_first?!i:i;len_t *l=&r[axis];
        if(ident_is(t[i],tl[i],"center")){*l=(len_t){.kind=LK_LEN,.pct=50};continue;}
        if(ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"top")||ident_is(t[i],tl[i],"right")||ident_is(t[i],tl[i],"bottom")){
            bool x=ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"right");
            if(x!=(axis==0))return false;
            *l=(len_t){.kind=LK_LEN,.pct=ident_is(t[i],tl[i],"left")||ident_is(t[i],tl[i],"top")?0:100};continue;
        }
        if(!parse_len(t[i],tl[i],cx,l,LF_NEG|LF_LENGTH))return false;
    }
    if(k==3){len_t depth;if(!parse_len(t[2],tl[2],cx,&depth,LF_NEG|LF_NOPCT|LF_LENGTH))return false;}
    out[0]=r[0];out[1]=r[1];return true;
}

static bool apply_value(const struct propdef *p, const char *v, size_t n, struct cx *cx, char *field);
static bool apply_long(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    style_t *s = cx->s;
    const style_t *par = cx->parent ? cx->parent : &initial;
    bool colour = p->type == PT_COLOR || p->type == PT_SVG_PAINT;
    const style_t *from = ident_is(v, n, "inherit") ? par : ident_is(v, n, "initial") ? &initial :
        ident_is(v, n, "unset") || ident_is(v, n, "revert") || ident_is(v, n, "revert-layer") ?
        p->inherited ? par : &initial : NULL;
    /* the initial colour is CanvasText, which follows color-scheme */
    if (from == &initial && colour && color_slot(p) == CSS_SLOT_COLOR) return css_color_apply("canvastext", 10, cx, CSS_SLOT_COLOR);
    if (from) {
        copy_prop(p, s, from, cx->a);
        if (colour && !cx->supports_probe) css_color_slot_copy(s, from, color_slot(p), cx->a);
        if (from == par && !p->inherited && ident_is(v, n, "inherit")) s->explicit_inherit = true;
        return true;
    }
    if (p->group == G_STYLE) return apply_value(p, v, n, cx, (char *)s + p->off);
    /* a group's field: parsed into a copy of the group, which s takes when the value is valid */
    union { struct css_float_shape float_shape; struct css_offset offset; struct css_gap_rules gap_rules;
            struct css_anchor anchor; struct css_counters counters; struct css_list list;
            struct css_background background; struct css_border_image border_image; struct css_mask mask; } own;
    memcpy(&own, group_of(s, p->group), group_size(p->group));
    if (!apply_value(p, v, n, cx, (char *)&own + p->off)) return false;
    group_write(s, p->group, &own, cx->a);
    return true;
}
/* a value (not a CSS-wide keyword) of p into field, s's own or its group's copy */
static bool apply_value(const struct propdef *p, const char *v, size_t n, struct cx *cx, char *field) {
    style_t *s = cx->s;
    switch (p->type) {
    case PT_TRANSFORM:return parse_transform_value(v,n,cx,&s->transform);
    case PT_TRANSLATE:return parse_translate_value(v,n,cx,&s->translate);
    case PT_ROTATE:return parse_rotate_value(v,n,cx,&s->rotate);
    case PT_SCALE:return parse_scale_value(v,n,cx,&s->scale);
    case PT_TRANSFORM_ORIGIN:return parse_transform_origin(v,n,cx,s->transform_origin);
    case PT_MOTION_LIST: return parse_motion_list(p, v, n, cx, (const struct css_motion_list **)field);
    case PT_CONTAINER_NAMES:
        if (ident_is(v, n, "none")) { s->container_names = NULL; return true; }
        if (!css_container_names_valid(v,n)) return false;
        s->container_names = ar_strndup(cx->a,v,n);
        return true;
    case PT_KW: {
        uint8_t k;
        if (cx->supports_probe && ((p->kws == kw_textwrap && !ident_is(v,n,"wrap") && !ident_is(v,n,"nowrap")) ||
            (p->kws == kw_ttrans && ident_is(v,n,"full-width")))) return false;
        if (!kw_find(p->kws, v, n, &k)) return false;
        *(uint8_t *)field = k;
        return true;
    }
    case PT_OVERFLOW: {
        uint8_t k;
        if (!kw_find(p->kws, v, n, &k)) return false;
        *(uint8_t *)field = k;
        return true;
    }
    case PT_DISPLAY:
        if (!parse_display(v, n, (uint8_t *)field, cx)) return false;
        s->legacy_box = ident_is(v, n, "-webkit-box") || ident_is(v, n, "-webkit-inline-box");
        return true;
    case PT_LINE_CLAMP: {
        if (ident_is(v, n, "none")) { s->line_clamp = 0; return true; }
        size_t i = n && v[0] == '+' ? 1 : 0;
        uint32_t value = 0;
        if (i == n) return false;
        for (; i < n; i++) {
            if (v[i] < '0' || v[i] > '9') return false;
            unsigned digit = (unsigned)(v[i] - '0');
            if (value > (UINT32_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (!value) return false;
        s->line_clamp = value;
        return true;
    }
    case PT_LEN: {
        len_t l;
        if (!parse_len(v, n, cx, &l, p->flags)) return false;
        *(len_t *)field = l;
        return true;
    }
    case PT_PX: {
        uint8_t k;
        if (p->kws && kw_find(p->kws, v, n, &k)) {
            *(float *)field = k;
            return true;
        }
        const char *t[2];
        size_t tl[2];
        int count = split_checked(v, n, t, tl, 2, ' ', cx);
        if (count < 1 || (cx->supports_probe && count != 1)) return false; /* only one stored axis */
        len_t l;
        if (!parse_len(t[0], tl[0], cx, &l, p->flags)) return false;
        *(float *)field = len_resolve(&l, 0);
        return true;
    }
    case PT_RADIUS: {
        const char *t[3];size_t tl[3];int count=split_checked(v,n,t,tl,3,' ',cx);
        if(count<1||count>2||t[count-1]+tl[count-1]!=v+n)return false;
        struct css_corner_radius parsed;
        if(!parse_len(t[0],tl[0],cx,&parsed.axis[0],LF_LENGTH))return false;
        parsed.axis[1]=parsed.axis[0];
        if(count==2&&!parse_len(t[1],tl[1],cx,&parsed.axis[1],LF_LENGTH))return false;
        *(struct css_corner_radius *)field=parsed;
        return true;
    }
    case PT_COLOR: return css_color_apply(v, n, cx, color_slot(p));
    case PT_COLOR_SCHEME: {
        /* CSS Color Adjust 1, 2.1: normal | [ light | dark | <custom-ident> ]+ && only? */
        if (ident_is(v, n, "normal")) {
            s->color_scheme = 0;
            s->color_scheme_text = NULL;
        } else {
            uint8_t bits = 0;
            size_t names = 0;
            char *text = ar_alloc(cx->a, n + 6), *t = text;
            for (const char *q = v, *e = v + n; q < e;) {
                while (q < e && is_space((unsigned char)*q)) q++;
                const char *w = q;
                while (q < e && !is_space((unsigned char)*q)) q++;
                size_t wn = (size_t)(q - w);
                if (!wn) break;
                /* only once, first or last */
                if (ident_is(w, wn, "only")) {
                    if ((bits & CSS_SCHEME_ONLY) || (names && q != e)) return false;
                    bits |= CSS_SCHEME_ONLY;
                    continue;
                }
                if (ident_is(w, wn, "normal") || !motion_name(w, wn)) return false; /* a <custom-ident> */
                bool light = ident_is(w, wn, "light"), dark = ident_is(w, wn, "dark");
                bits |= light ? CSS_SCHEME_LIGHT : dark ? CSS_SCHEME_DARK : 0;
                if (names++) *t++ = ' ';
                if (light || dark) { memcpy(t, light ? "light" : "dark", light ? 5 : 4); t += light ? 5 : 4; }
                else { memcpy(t, w, wn); t += wn; }
            }
            if (!names) return false;
            if (bits & CSS_SCHEME_ONLY) { memcpy(t, " only", 5); t += 5; }
            *t = 0;
            s->color_scheme = bits;
            s->color_scheme_text = text;
        }
        /* the root's initial colour follows its scheme until color is declared */
        if (!cx->parent && !cx->supports_probe && !cx->set[css_prop_index(find_prop("color", 5))])
            css_color_apply("canvastext", 10, cx, CSS_SLOT_COLOR);
        return true;
    }
    case PT_SVG_WIDTH: {
        len_t width;
        if (!parse_len(v, n, cx, &width, 0) || width.kind != LK_LEN || (width.pct && width.px)) return false;
        /* Pure percentages must reach the SVG viewport, not the HTML parent.
           The codec cannot yet evaluate mixed length/percentage expressions. */
        *(len_t *)field = width;
        return true;
    }
    case PT_SVG_PAINT: {
        struct svg_paint paint = {0};
        const struct css_color_value *exact = NULL;
        if (ident_is(v, n, "none")) paint.kind = SVG_PAINT_NONE;
        else if (css_color_value_compute(v, n, cx, NULL, &paint.color, &exact)) paint.kind = SVG_PAINT_COLOR;
        else {
            /* The rasterizer can resolve local SVG gradients, not external
               paint servers or context paints. Do not claim those in supports(). */
            const char *ref = n >= 5 && strn_ieq(v, "url(", 4) && v[n-1] == ')' ? css_parse_url(v, n, cx->a) : NULL;
            if (!ref) return false;
            if (ref[0] != '#') {
                const char *fragment = strchr(ref, '#');
                web_doc *doc = cx->node ? cx->node->owner : NULL;
                const char *document_url = doc ? doc->url : NULL;
                size_t prefix = fragment ? (size_t)(fragment - ref) : 0;
                size_t document_prefix = document_url ? strcspn(document_url, "#") : 0;
                if (!fragment || !document_url || prefix != document_prefix || memcmp(ref, document_url, prefix)) return false;
                ref = fragment;
            }
            if (!ref[1]) return false;
            for (const char *q = ref + 1; *q; q++)
                if (is_space((unsigned char)*q) || strchr("()\"';\\", *q)) return false;
            paint.kind = SVG_PAINT_REF; paint.ref = ref;
        }
        *(struct svg_paint *)field = paint;
        if (!cx->supports_probe) css_color_slot_set(s, color_slot(p), exact, cx->a);
        return true;
    }
    case PT_NUM: {
        float x;
        if (!parse_number_value(v, n, cx, &x)) return false;
        if (x < 0) { if (!css_calc_is_math(v, n)) return false; x = 0; } /* a calculation clamps to the range */
        *(float *)field = x;
        return true;
    }
    case PT_FILTER: return parse_filter(v, n, cx, &s->filter);
    case PT_SHAPE: return css_shape_parse(p->flags, v, n, cx, (const struct css_shape **)field, NULL);
    case PT_OFFSET: return css_offset_parse(p->flags, v, n, cx, field, NULL);
    case PT_RULE_LIST: {
        struct css_rule_list lists[3];
        if (!rule_lists(p->flags, v, n, cx, lists, NULL)) return false;
        *(struct css_rule_list *)field = lists[p->flags];
        return true;
    }
    case PT_RULE_INSET: return rule_inset(v, n, cx, (struct css_rule_inset *)field, NULL);
    case PT_ANCHOR: return anchor_value_parse(p->flags, v, n, cx, field, NULL);
    case PT_CLIP: return parse_clip_rect(v, n, cx, &s->clip);
    case PT_BOX_SHADOW: return parse_box_shadows(v, n, cx, &s->box_shadow, NULL);
    case PT_OPACITY: {
        /* <opacity-value>: <number> | <percentage>, either of them a calculation */
        double x;
        unsigned kind;
        if (!css_numeric_read(v, n, CSS_CALC_NUMBER | CSS_CALC_PERCENT, cx, cx->a, NAN, &x, &kind)) return false;
        if (kind == CSS_CALC_PERCENT) x /= 100;
        *(float *)field = x < 0 ? 0 : x > 1 ? 1 : (float)x;
        return true;
    }
    case PT_INT: case PT_ZINDEX: {
        if (p->type == PT_ZINDEX && ident_is(v, n, "auto")) {
            s->z_index = 0;
            s->z_auto = true;
            return true;
        }
        const char *q = v;
        float x;
        bool math = !parse_number(&q, v + n, &x) || q != v + n;
        if (math) {
            /* a calculation resolves to the nearest integer, a half toward +infinity, in range (§10.12) */
            double d;
            if (!css_calc_is_math(v, n) || !css_numeric_read(v, n, CSS_CALC_NUMBER, cx, cx->a, NAN, &d, NULL)) return false;
            d = floor(d + 0.5);
            x = d > 2147483520.0 ? 2147483520.0f : d < -2147483648.0 ? -2147483648.0f : (float)d;
        }
        if (cx->supports_probe && (x != floorf(x) || x > 2147483520.0f || x < -2147483648.0f)) return false;
        *(int *)field = (int)x;
        if (p->type == PT_ZINDEX) s->z_auto = false;
        return true;
    }
    case PT_FONT_SIZE: return parse_font_size(v, n, cx, &s->font_size);
    case PT_FONT_WEIGHT: return parse_font_weight(v, n, cx, &s->font_weight);
    case PT_FONT_FAMILY:
        if(!choose_family(v,n,&s->font_family,cx))return false;
        s->font_names=ar_strndup(cx->a,v,n);s->named_font=NULL;s->font_choices=NULL;return true;
    case PT_LINE_HEIGHT: {
        if (ident_is(v, n, "normal")) {
            s->line_height.kind = LK_NORMAL;
            return true;
        }
        /* <number> | <length-percentage>: percentages are of the font size, also in a calculation
           (calc(10% / 1px) is a number) */
        double x;
        unsigned kind;
        if (!css_numeric_read(v, n, CSS_CALC_NUMBER | CSS_CALC_LENGTH | CSS_CALC_PERCENT, cx, cx->a, s->font_size, &x, &kind))
            return false;
        if (x < 0) { if (!css_calc_is_math(v, n)) return false; x = 0; }
        s->line_height.kind = kind == CSS_CALC_NUMBER ? LK_NUMBER : LK_LEN;
        s->line_height.px = kind == CSS_CALC_NUMBER ? calc_float(x) : x > 1e7 ? 1e7f : (float)x;
        s->line_height.pct = 0;
        return true;
    }
    case PT_VALIGN: {
        uint8_t k;
        if (kw_find(p->kws, v, n, &k)) {
            s->vertical_align = k;
            return true;
        }
        len_t l;
        if (!parse_len(v, n, cx, &l, LF_NEG)) return false;
        s->vertical_align = VA_LEN;
        s->vertical_align_px = len_resolve(&l, s->font_size * 1.2f);
        return true;
    }
    case PT_CONTENT: return parse_content(v, n, cx);
    case PT_LIST_TYPE: return parse_list_type(v, n, cx);
    case PT_LIST_IMAGE: return parse_list_image(v, n, cx, (const struct css_image **)field);
    case PT_QUOTES: return parse_quotes(v, n, cx, (const struct css_quotes **)field);
    case PT_COUNTER: return parse_counter_ops(v, n, cx, p->flags, (const struct css_counter_ops **)field, NULL);
    case PT_BG_IMAGE: return parse_bg_image(v, n, cx);
    case PT_BG_POS: return parse_bg_pos(v, n, cx, (len_t *)field);
    case PT_BG_LIST: return parse_bg_keywords(p, v, n, cx, (struct css_bg_keywords *)field);
    case PT_BG_OFFSET: return parse_bg_offsets(p->flags, v, n, cx, (struct css_bg_offsets *)field);
    case PT_BI_SOURCE: return parse_bi_source(v, n, cx, (struct css_bi_source *)field);
    case PT_BI_SLICE: return parse_bi_slice(v, n, cx, (struct css_bi_slice *)field);
    case PT_BI_SIDES: return parse_bi_sides(p->flags, v, n, cx, (struct css_bi_side *)field);
    case PT_BI_REPEAT: return parse_bi_repeat(v, n, (uint8_t *)field);
    case PT_BG_SIZE: return parse_bg_size(v, n, cx, s->bg_size, &s->bg_size_kind);
    case PT_MASK_IMAGE: return parse_mask_images(v, n, cx, (struct css_layer_images *)field);
    case PT_MASK_REPEAT: return parse_repeat_styles(v, n, cx, (struct css_bg_keywords *)field);
    case PT_MASK_POSITION: return parse_bg_positions(v, n, false, cx, (struct css_bg_offsets *)field);
    case PT_MASK_SIZE: return parse_layer_sizes(v, n, cx, (struct css_layer_sizes *)field);
    case PT_GTEMPLATE: return parse_gtemplate(v, n, cx, (const struct gtemplate **)field);
    case PT_GAREAS: return parse_gareas(v, n, cx, &s->grid_areas);
    case PT_GLINE: return parse_gline(v, n, cx, (struct gline *)field);
    case PT_TEXT_DECO: {
        const char *t[4];
        size_t tl[4];
        int k = split_checked(v, n, t, tl, 4, ' ', cx);
        uint8_t d = 0;
        for (int i = 0; i < k; i++) {
            if (ident_is(t[i], tl[i], "underline")) d |= TD_UNDERLINE;
            else if (ident_is(t[i], tl[i], "overline")) d |= TD_OVERLINE;
            else if (ident_is(t[i], tl[i], "line-through")) d |= TD_LINE_THROUGH;
            else if (ident_is(t[i], tl[i], "none") || ident_is(t[i], tl[i], "blink")) {
            } else return false;
        }
        s->text_decoration = d;
        return true;
    }
    }
    return false;
}

/* ---------------------------------------------------------------- shorthands */
/* A shorthand gives each of its longhands a value through set_long, by the
   longhand's own name: a logical one (margin-inline-start) is the table's
   physical property. cx->record, when set, gets each longhand's name and the
   text it was given (CSSOM's longhands of a shorthand, css_shorthand_expand). */
static bool set_long(const char *name, const char *v, size_t n, struct cx *cx) {
    const struct propdef *p = find_prop(name, strlen(name));
    if (!p) p = css_prop_lookup(name, strlen(name));
    if (!p) { if (cx->supports_probe) cx->invalid = true; return false; }
    int i = css_prop_index(p);
    if (cx->set[i]) return true;
    if (cx->font_pass && !early(p)) return true;
    if (!apply_long(p, v, n, cx)) { if (cx->supports_probe) cx->invalid = true; return false; }
    cx->set[i] = 1;
    if (cx->record) cx->record(cx->record_opaque, name, v, n);
    return true;
}

static const char *const side_names[4] = {"top", "right", "bottom", "left"};
static const char *const inline_sides[2] = {"inline-start", "inline-end"}, *const block_sides[2] = {"block-start", "block-end"};

/* margin: 1-4 values for the four sides; margin-inline: 1-2 for its start and end */
static bool set_box(const char *fmt, const char *v, size_t n, struct cx *cx, const char *const *sides, int nsides) {
    const char *t[4];
    size_t tl[4];
    int k = split_checked(v, n, t, tl, 4, ' ', cx);
    if (k < 1 || (cx->supports_probe && nsides == 2 && k > 2)) return false;
    char name[64];
    if (nsides == 4) {
        static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof name, fmt, side_names[i]);
            ok &= set_long(name, t[pick[k][i]], tl[pick[k][i]], cx);
        }
        return ok;
    }
    /* logical pairs: start end */
    bool ok = true;
    for (int i = 0; i < nsides; i++) {
        int ti = k > 1 ? i : 0;
        snprintf(name, sizeof name, fmt, sides[i]);
        ok &= set_long(name, t[ti], tl[ti], cx);
    }
    return ok;
}

static bool is_bwidth(const char *s, size_t n, struct cx *cx) {
    uint8_t k;
    len_t l;
    return kw_find(kw_bwidth, s, n, &k) || parse_len(s, n, cx, &l, LF_NOPCT);
}

/* border: width style color, for the given sides (the widths, then the styles,
   then the colours: the shorthand's longhands in order) */
static bool set_border(const char *v, size_t n, struct cx *cx, const char *const *sides, int nsides) {
    const char *t[3];
    size_t tl[3];
    int k = split_checked(v, n, t, tl, 3, ' ', cx);
    const char *w = "medium", *st = "none", *c = "currentcolor";
    size_t wl = 6, sl = 4, cl = 12;
    uint8_t dummy;
    uint32_t col;
    unsigned seen = 0;
    for (int i = 0; i < k; i++) {
        unsigned bit;
        if (kw_find(kw_bstyle, t[i], tl[i], &dummy)) { bit = 1; st = t[i]; sl = tl[i]; }
        else if (is_bwidth(t[i], tl[i], cx)) { bit = 2; w = t[i]; wl = tl[i]; }
        else if (css_color_checked(t[i], tl[i], &col, cx)) { bit = 4; c = t[i]; cl = tl[i]; }
        else return false;
        if (cx->supports_probe && (seen & bit)) return false;
        seen |= bit;
    }
    char name[64];
    for (int i = 0; i < nsides; i++) { snprintf(name, sizeof name, "border-%s-width", sides[i]); set_long(name, w, wl, cx); }
    for (int i = 0; i < nsides; i++) { snprintf(name, sizeof name, "border-%s-style", sides[i]); set_long(name, st, sl, cx); }
    for (int i = 0; i < nsides; i++) { snprintf(name, sizeof name, "border-%s-color", sides[i]); set_long(name, c, cl, cx); }
    return true;
}

static bool set_font(const char *v, size_t n, struct cx *cx) {
    const char *t[16];
    size_t tl[16];
    int k = split_checked(v, n, t, tl, 16, ' ', cx);
    static const char *const sys[] = {"caption", "icon", "menu", "message-box", "small-caption", "status-bar",
                                      "-webkit-small-control", "-apple-system-body", NULL};
    if (k == 1)
        for (int i = 0; sys[i]; i++)
            if (ident_is(t[0], tl[0], sys[i])) {
                set_long("font-style", "normal", 6, cx);
                set_long("font-weight", "normal", 6, cx);
                set_long("font-size", "13px", 4, cx);
                set_long("line-height", "normal", 6, cx);
                set_long("font-family", "sans-serif", 10, cx);
                return true;
            }
    const char *style = "normal", *weight = "normal";
    size_t stl = 6, wl = 6;
    int i = 0;
    for (; i < k; i++) {
        uint8_t d;
        uint16_t w;
        if (kw_find(kw_fstyle, t[i], tl[i], &d) && !ident_is(t[i], tl[i], "normal")) style = t[i], stl = tl[i];
        else if (ident_is(t[i], tl[i], "normal") || ident_is(t[i], tl[i], "small-caps")) {
        } else if (!isdigit((unsigned char)t[i][0]) && parse_font_weight(t[i], tl[i], cx, &w)) weight = t[i], wl = tl[i];
        else if (isdigit((unsigned char)t[i][0]) && parse_font_weight(t[i], tl[i], cx, &w) && i + 1 < k)
            weight = t[i], wl = tl[i];
        else if (ident_is(t[i], tl[i], "condensed") || ident_is(t[i], tl[i], "expanded") ||
                 ident_is(t[i], tl[i], "semi-condensed") || ident_is(t[i], tl[i], "semi-expanded")) {
        } else break;
    }
    if (i >= k) return false;
    /* size[/line-height] */
    const char *sz = t[i];
    size_t szl = tl[i];
    const char *lh = "normal";
    size_t lhl = 6;
    const char *slash = memchr(sz, '/', szl);
    int fam = i + 1;
    if (slash) {
        lh = slash + 1;
        lhl = (size_t)(sz + szl - lh);
        szl = (size_t)(slash - sz);
        if (!lhl && fam < k) lh = t[fam], lhl = tl[fam], fam++;
    } else if (fam < k && t[fam][0] == '/') {
        if (tl[fam] > 1) lh = t[fam] + 1, lhl = tl[fam] - 1, fam++;
        else if (fam + 1 < k) lh = t[fam + 1], lhl = tl[fam + 1], fam += 2;
    }
    float fs;
    if (!parse_font_size(sz, szl, cx, &fs)) return false;
    if (fam >= k) return false;
    /* The shorthand takes <'font-family'>, the property's own grammar: a
       CSS-wide keyword there is not a family list (CSS Fonts 4 font). */
    const char *family = t[fam];
    size_t family_n = (size_t)(v + n - t[fam]);
    if (ident_is(family, family_n, "inherit") || ident_is(family, family_n, "initial") || ident_is(family, family_n, "unset") ||
        ident_is(family, family_n, "revert") || ident_is(family, family_n, "revert-layer") || ident_is(family, family_n, "default")) {
        if (cx->supports_probe) cx->invalid = true;
        return false;
    }
    /* the longhands in their order: style, weight, size, line height, family */
    if (!cx->font_pass) {
        set_long("font-style", style, stl, cx);
        set_long("font-weight", weight, wl, cx);
    }
    set_long("font-size", sz, szl, cx);
    if (cx->font_pass) return true;
    set_long("line-height", lh, lhl, cx);
    set_long("font-family", family, family_n, cx);
    return true;
}

static bool set_flex(const char *v, size_t n, struct cx *cx) {
    const char *t[4];
    size_t tl[4];
    int k = split_checked(v, n, t, tl, 4, ' ', cx);
    if (k < 1 || k > 3) return false;
    if (k == 1 && ident_is(t[0], tl[0], "none"))
        return set_long("flex-grow", "0", 1, cx), set_long("flex-shrink", "0", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    if (k == 1 && ident_is(t[0], tl[0], "auto"))
        return set_long("flex-grow", "1", 1, cx), set_long("flex-shrink", "1", 1, cx),
               set_long("flex-basis", "auto", 4, cx);
    const char *g = "1", *sh = "1", *b = "0%";
    size_t gl = 1, shl = 1, bl = 2;
    int nums = 0, first_number = -1;
    bool basis = false;
    for (int i = 0; i < k; i++) {
        const char *q = t[i];
        float x;
        if (parse_number(&q, t[i] + tl[i], &x) && q == t[i] + tl[i]) {
            if (!isfinite(x) || x < 0) return false;
            /* A unitless zero is a factor until both factors are supplied.
               The third zero is then the definite zero-length basis, not an
               illegal third factor (Flexbox 7.1). */
            if (nums == 2) {
                if (basis || x != 0) return false;
                basis = true; b = t[i]; bl = tl[i];
            } else {
                if (nums == 0) { g = t[i]; gl = tl[i]; first_number = i; }
                else {
                    if (i != first_number + 1) return false;
                    sh = t[i]; shl = tl[i];
                }
                nums++;
            }
        } else {
            if (basis) return false;
            basis = true; b = t[i]; bl = tl[i];
        }
    }
    if (!nums) g = "1", gl = 1;
    /* Validate before publishing any longhand: an invalid basis must not
       overwrite the grow/shrink of an earlier valid declaration. */
    len_t parsed;
    if (!parse_len(b, bl, cx, &parsed, LF_AUTO | LF_LENGTH)) return false;
    return set_long("flex-grow", g, gl, cx) && set_long("flex-shrink", sh, shl, cx) &&
           set_long("flex-basis", b, bl, cx);
}

static bool apply_short(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    const char *const *all = side_names, *const *lr = inline_sides, *const *tb = block_sides;
    bool global = ident_is(v, n, "inherit") || ident_is(v, n, "initial") || ident_is(v, n, "unset") ||
                  ident_is(v, n, "revert") || ident_is(v, n, "revert-layer");
    switch (p->sh) {
    case SH_ALL: {
        if(!global)return false;
        for(int i=0;i<NPROPS;i++)if(props[i].type!=PT_SHORT && strcmp(props[i].name,"direction") &&
            strcmp(props[i].name,"unicode-bidi"))set_long(props[i].name,v,n,cx);
        return true;
    }
    case SH_ANIMATION: case SH_TRANSITION: {
        /* the longhands each shorthand sets; animation-composition is reset only */
        static const char *const animation[] = {"animation-name", "animation-duration", "animation-delay",
            "animation-timing-function", "animation-iteration-count", "animation-direction",
            "animation-fill-mode", "animation-play-state", "animation-composition"};
        static const char *const transition[] = {"transition-property", "transition-duration", "transition-delay",
            "transition-timing-function", "transition-behavior"};
        bool is_animation = p->sh == SH_ANIMATION;
        const char *const *names = is_animation ? animation : transition;
        unsigned count = is_animation ? 9 : 5;
        if (global) { for (unsigned i = 0; i < count; i++) set_long(names[i], v, n, cx); return true; }
        struct css_motion motion;
        if (!motion_shorthand(is_animation, v, n, cx, &motion)) return false;
        /* The shorthand resets all longhands, but cannot override a higher
           priority longhand already selected by this cascade. */
        style_t temp = *cx->s;
        temp.motion = motion;
        for (unsigned i = 0; i < count; i++) {
            const struct propdef *q = find_prop(names[i], strlen(names[i]));
            int at = css_prop_index(q);
            if (!cx->set[at]) { copy_prop(q, cx->s, &temp, cx->a); cx->set[at] = 1; }
        }
        return true;
    }
    case SH_CONTAINER: {
        if (global) {
            set_long("container-name",v,n,cx);
            set_long("container-type",v,n,cx);
            return true;
        }
        const char *slash=NULL,*names=v;
        for(size_t i=0;i<n;i++) {
            if(v[i]=='\\' && i+1<n) {
                i++;unsigned count=0;
                while(i<n && isxdigit((unsigned char)v[i]) && count<6){i++;count++;}
                if(count && i<n && !is_space((unsigned char)v[i]))i--;
                continue;
            }
            if(v[i]=='/'){slash=v+i;break;}
        }
        size_t nn=slash?(size_t)(slash-v):n; trim(&names,&nn);
        const char *type=slash?slash+1:"normal";
        size_t tn=slash?(size_t)(v+n-type):6; trim(&type,&tn);
        type=css_value_canonical(cx->a,type,tn,&tn);
        uint8_t dummy;
        if ((!ident_is(names,nn,"none") && !css_container_names_valid(names,nn)) ||
            !kw_find(kw_container_type,type,tn,&dummy)) return false;
        set_long("container-name",names,nn,cx);
        set_long("container-type",type,tn,cx);
        return true;
    }
    case SH_BORDER_RADIUS: {
        static const char *const names[]={"border-top-left-radius","border-top-right-radius","border-bottom-right-radius","border-bottom-left-radius"};
        if(global){bool ok=true;for(int i=0;i<4;i++)ok&=set_long(names[i],v,n,cx);return ok;}
        len_t lengths[2][4];
        if(!parse_radius_list(v,n,cx,lengths))return false;
        for(int i=0;i<4;i++) {
            const struct propdef *corner=css_prop_lookup(names[i],strlen(names[i]));int index=css_prop_index(corner);
            if(cx->set[index])continue;
            cx->s->corner_radius[i].axis[0]=lengths[0][i];cx->s->corner_radius[i].axis[1]=lengths[1][i];cx->set[index]=1;
        }
        return true;
    }
    case SH_MARGIN: return set_box("margin-%s", v, n, cx, all, 4);
    case SH_PADDING: return set_box("padding-%s", v, n, cx, all, 4);
    case SH_INSET: {
        static const char *const nm[4] = {"top", "right", "bottom", "left"};
        const char *t[4];
        size_t tl[4];
        int k = split_checked(v, n, t, tl, 4, ' ', cx);
        if (k < 1) return false;
        static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        for (int i = 0; i < 4; i++) set_long(nm[i], t[pick[k][i]], tl[pick[k][i]], cx);
        return true;
    }
    case SH_MARGIN_INLINE: return set_box("margin-%s", v, n, cx, lr, 2);
    case SH_MARGIN_BLOCK: return set_box("margin-%s", v, n, cx, tb, 2);
    case SH_PADDING_INLINE: return set_box("padding-%s", v, n, cx, lr, 2);
    case SH_PADDING_BLOCK: return set_box("padding-%s", v, n, cx, tb, 2);
    case SH_INSET_INLINE: case SH_INSET_BLOCK: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        bool in = p->sh == SH_INSET_INLINE;
        const char *a = in ? "inset-inline-start" : "inset-block-start", *b = in ? "inset-inline-end" : "inset-block-end";
        set_long(a, t[0], tl[0], cx);
        set_long(b, t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_BORDER: case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT:
    case SH_BORDER_INLINE: case SH_BORDER_BLOCK: {
        int side = p->sh - SH_BORDER_TOP;
        const char *const *sides = p->sh == SH_BORDER ? all : p->sh == SH_BORDER_INLINE ? lr : p->sh == SH_BORDER_BLOCK ? tb : &all[side];
        int ns = p->sh == SH_BORDER ? 4 : (p->sh == SH_BORDER_INLINE || p->sh == SH_BORDER_BLOCK) ? 2 : 1;
        if (global) {
            char name[64];
            static const char *const f[3] = {"width", "style", "color"};
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < ns; i++) {
                    snprintf(name, sizeof name, "border-%s-%s", sides[i], f[j]);
                    set_long(name, v, n, cx);
                }
            if (p->sh == SH_BORDER) bi_shorthand_apply(false, v, n, true, cx);
            return true;
        }
        /* border also resets border-image (css-backgrounds-3 §4.4) */
        if (!set_border(v, n, cx, sides, ns)) return false;
        if (p->sh == SH_BORDER) bi_shorthand_apply(false, "initial", 7, true, cx);
        return true;
    }
    case SH_GAP_RULE: return gap_shorthand_apply(p, v, n, global, cx);
    case SH_POSITION_TRY: {
        if (global) { set_long("position-try-order", v, n, cx); set_long("position-try-fallbacks", v, n, cx); return true; }
        uint8_t order;
        struct css_position_tries tries;
        if (!parse_position_try(v, n, cx, &order, &tries, NULL)) return false;
        if (cx->font_pass) return true;
        /* the longhands no more important declaration set, written into one copy of the group */
        struct css_anchor own = *css_style_anchor(cx->s);
        int at_order = css_prop_index(find_prop("position-try-order", 18));
        int at_tries = css_prop_index(find_prop("position-try-fallbacks", 22));
        bool written = false;
        if (!cx->set[at_order]) { own.try_order = order; cx->set[at_order] = 1; written = true; }
        if (!cx->set[at_tries]) { own.tries = tries; cx->set[at_tries] = 1; written = true; }
        if (written) group_write(cx->s, G_ANCHOR, &own, cx->a);
        return true;
    }
    case SH_OFFSET: {
        static const char *const names[] = {"offset-position", "offset-path", "offset-distance", "offset-rotate", "offset-anchor"};
        if (global) { for (unsigned i = 0; i < 5; i++) set_long(names[i], v, n, cx); return true; }
        struct css_offset parsed;
        if (!css_offset_shorthand(v, n, cx, &parsed, NULL)) return false;
        if (cx->font_pass) return true;
        /* the longhands no more important declaration set, written into one copy of the group */
        struct css_offset own = *css_style_offset(cx->s);
        bool written = false;
        for (unsigned i = 0; i < 5; i++) {
            const struct propdef *q = find_prop(names[i], strlen(names[i]));
            int at = css_prop_index(q);
            if (cx->set[at]) continue;
            memcpy((char *)&own + q->off, (const char *)&parsed + q->off, q->size);
            cx->set[at] = 1;
            written = true;
        }
        if (written) group_write(cx->s, G_OFFSET, &own, cx->a);
        return true;
    }
    case SH_OUTLINE: {
        if (global) {
            set_long("outline-width", v, n, cx);
            set_long("outline-style", v, n, cx);
            set_long("outline-color", v, n, cx);
            return true;
        }
        const char *t[3];
        size_t tl[3];
        int k = split_checked(v, n, t, tl, 3, ' ', cx);
        const char *w = "medium", *st = "none", *c = "currentcolor";
        size_t wl = 6, sl = 4, cl = 12;
        uint8_t style;
        uint32_t col;
        unsigned seen = 0;
        for (int i = 0; i < k; i++) {
            unsigned bit;
            if (kw_find(kw_ostyle, t[i], tl[i], &style)) { bit = 1; st = t[i]; sl = tl[i]; }
            else if (is_bwidth(t[i], tl[i], cx)) { bit = 2; w = t[i]; wl = tl[i]; }
            else if (css_color_checked(t[i], tl[i], &col, cx)) { bit = 4; c = t[i]; cl = tl[i]; }
            else return false;
            if (seen & bit) return false;
            seen |= bit;
        }
        if (k < 1) return false;
        set_long("outline-width", w, wl, cx);
        set_long("outline-style", st, sl, cx);
        set_long("outline-color", c, cl, cx);
        return true;
    }
    case SH_BORDER_WIDTH: return set_box("border-%s-width", v, n, cx, all, 4);
    case SH_BORDER_STYLE: return set_box("border-%s-style", v, n, cx, all, 4);
    case SH_BORDER_COLOR: return set_box("border-%s-color", v, n, cx, all, 4);
    case SH_LIST_STYLE: {
        static const char *const names[3] = {"list-style-position", "list-style-image", "list-style-type"};
        static const char *const initials[3] = {"outside", "none", "disc"};
        if (global) {
            for (int i = 0; i < 3; i++) set_long(names[i], v, n, cx);
            return true;
        }
        const char *part[3];
        size_t length[3];
        if (!list_style_parts(v, n, cx, part, length)) return false;
        for (int i = 0; i < 3; i++)
            set_long(names[i], part[i] ? part[i] : initials[i], part[i] ? length[i] : strlen(initials[i]), cx);
        return true;
    }
    case SH_FONT:
        if (global) {
            if (!cx->font_pass) {
                set_long("font-style", v, n, cx);
                set_long("font-weight", v, n, cx);
            }
            set_long("font-size", v, n, cx);
            if (!cx->font_pass) {
                set_long("line-height", v, n, cx);
                set_long("font-family", v, n, cx);
            }
            return true;
        }
        return set_font(v, n, cx);
    case SH_BACKGROUND:
        if (global) {
            static const char *const longhands[] = {"background-color", "background-image", "background-repeat",
                "background-position-x", "background-position-y", "background-size", "background-attachment",
                "background-origin", "background-clip"};
            for (unsigned i = 0; i < sizeof longhands / sizeof *longhands; i++) set_long(longhands[i], v, n, cx);
            return true;
        }
        return set_background(v, n, cx);
    case SH_BACKGROUND_POSITION: return bg_position_apply(v, n, global, cx);
    case SH_BORDER_IMAGE: return bi_shorthand_apply(false, v, n, global, cx);
    case SH_MASK_BORDER: return bi_shorthand_apply(true, v, n, global, cx);
    case SH_MASK: return mask_shorthand_apply(v, n, global, cx);
    case SH_FLEX:
        if (global) {
            set_long("flex-grow", v, n, cx);
            set_long("flex-shrink", v, n, cx);
            set_long("flex-basis", v, n, cx);
            return true;
        }
        return set_flex(v, n, cx);
    case SH_FLEX_FLOW: {
        const char *t[2];
        size_t tl[2];
        if (global) return set_long("flex-direction", v, n, cx), set_long("flex-wrap", v, n, cx), true;
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        /* an omitted longhand is its initial value */
        const char *direction = "row", *wrap = "nowrap";
        size_t dn = 3, wn = 6;
        bool seen_direction = false, seen_wrap = false;
        for (int i = 0; i < k; i++) {
            uint8_t d;
            if (!seen_direction && kw_find(kw_fdir, t[i], tl[i], &d)) direction = t[i], dn = tl[i], seen_direction = true;
            else if (!seen_wrap && kw_find(kw_fwrap, t[i], tl[i], &d)) wrap = t[i], wn = tl[i], seen_wrap = true;
            else return false;
        }
        if (k < 1) return false;
        set_long("flex-direction", direction, dn, cx);
        set_long("flex-wrap", wrap, wn, cx);
        return true;
    }
    case SH_GAP: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        set_long("row-gap", t[0], tl[0], cx);
        set_long("column-gap", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_OVERFLOW: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        set_long("overflow-x", t[0], tl[0], cx);
        set_long("overflow-y", t[k - 1], tl[k - 1], cx);
        return true;
    }
    case SH_TEXT_DECORATION: {
        if (global) return set_long("text-decoration-line", v, n, cx);
        const char *t[6];
        size_t tl[6];
        int k = split_checked(v, n, t, tl, 6, ' ', cx);
        char line[64] = "none";
        size_t ln = 4;
        for (int i = 0; i < k; i++)
            if (ident_is(t[i], tl[i], "underline") || ident_is(t[i], tl[i], "overline") ||
                ident_is(t[i], tl[i], "line-through")) {
                if (ln == 4 && !strcmp(line, "none")) ln = 0;
                if (ln + tl[i] + 1 < sizeof line) {
                    if (ln) line[ln++] = ' ';
                    memcpy(line + ln, t[i], tl[i]);
                    ln += tl[i];
                    line[ln] = 0;
                }
            } else if (cx->supports_probe && !ident_is(t[i],tl[i],"none")) return false; /* other decoration fields aren't painted */
        return set_long("text-decoration-line", line, ln, cx);
    }
    case SH_PLACE_SELF: case SH_PLACE_CONTENT: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        if (p->sh == SH_PLACE_CONTENT) {
            /* <'align-content'> <'justify-content'>? */
            set_long("align-content", t[0], tl[0], cx);
            return set_long("justify-content", t[k - 1], tl[k - 1], cx);
        }
        set_long("align-self", t[0], tl[0], cx);
        return set_long("justify-self", t[k - 1], tl[k - 1], cx);
    }
    case SH_GRID_AREA: case SH_GRID_ROW: case SH_GRID_COLUMN: {
        /* start / end lines: a name given alone stands for both edges of that area */
        static const char *const area[4] = {"grid-row-start", "grid-column-start", "grid-row-end", "grid-column-end"};
        static const char *const row[2] = {"grid-row-start", "grid-row-end"};
        static const char *const col[2] = {"grid-column-start", "grid-column-end"};
        const char *const *nm = p->sh == SH_GRID_AREA ? area : p->sh == SH_GRID_ROW ? row : col;
        int want = p->sh == SH_GRID_AREA ? 4 : 2;
        const char *t[4];
        size_t tl[4];
        int k = split_checked(v, n, t, tl, 4, '/', cx);
        if (k < 1 || k > want) return false;
        const char *s[4];
        size_t sl[4];
        for (int i = 0; i < want; i++) {
            s[i] = "auto", sl[i] = 4;
            if (i < k) s[i] = t[i], sl[i] = tl[i];
            else if (global) s[i] = v, sl[i] = n;
            else {
                int from = want == 4 && i == 3 ? 1 : 0;
                if (is_custom_ident(s[from], sl[from])) s[i] = s[from], sl[i] = sl[from];
            }
        }
        for (int i = 0; i < want; i++)
            if (!set_long(nm[i], s[i], sl[i], cx)) return false;
        return true;
    }
    case SH_GRID_TEMPLATE: case SH_GRID: {
        /* grid also resets the implicit grid's longhands (CSS Grid 2 §7.8) */
        if (global || ident_is(v, n, "none")) {
            set_long("grid-template-rows", v, n, cx);
            set_long("grid-template-columns", v, n, cx);
            set_long("grid-template-areas", v, n, cx);
            if (p->sh == SH_GRID) {
                set_long("grid-auto-rows", global ? v : "auto", global ? n : 4, cx);
                set_long("grid-auto-columns", global ? v : "auto", global ? n : 4, cx);
                set_long("grid-auto-flow", global ? v : "row", global ? n : 3, cx);
            }
            return true;
        }
        const char *t[2];
        size_t tl[2];
        if (split_checked(v, n, t, tl, 2, '/', cx) != 2) return false;
        /* grid: auto-flow forms */
        if (p->sh == SH_GRID) {
            for (int side = 0; side < 2; side++) {
                const char *w[4];
                size_t wl[4];
                int k = split_checked(t[side], tl[side], w, wl, 4, ' ', cx);
                int a = -1;
                for (int i = 0; i < k; i++)
                    if (ident_is(w[i], wl[i], "auto-flow")) a = i;
                if (a < 0) continue;
                const char *rest = w[k - 1] + wl[k - 1];
                for (int i = 0; i < k; i++)
                    if (!ident_is(w[i], wl[i], "auto-flow") && !ident_is(w[i], wl[i], "dense")) {
                        rest = w[i];
                        break;
                    }
                size_t rn = (size_t)(t[side] + tl[side] - rest);
                bool dense = false;
                for (int i = 0; i < k; i++) if (ident_is(w[i], wl[i], "dense")) dense = true;
                const char *flow = side ? (dense ? "column dense" : "column") : (dense ? "row dense" : "row");
                bool other = set_long(side ? "grid-template-rows" : "grid-template-columns", t[1 - side], tl[1 - side], cx);
                set_long(side ? "grid-template-columns" : "grid-template-rows", "none", 4, cx);
                set_long("grid-template-areas", "none", 4, cx);
                set_long(side ? "grid-auto-columns" : "grid-auto-rows", rn ? rest : "auto", rn ? rn : 4, cx);
                set_long(side ? "grid-auto-rows" : "grid-auto-columns", "auto", 4, cx);
                set_long("grid-auto-flow", flow, strlen(flow), cx);
                return other;
            }
        }
        bool ok;
        if (!memchr(t[0], '"', tl[0]) && !memchr(t[0], '\'', tl[0])) {
            set_long("grid-template-areas", "none", 4, cx);
            ok = set_long("grid-template-rows", t[0], tl[0], cx) &&
                 set_long("grid-template-columns", t[1], tl[1], cx);
        } else {
        /* "a b" size "c d" size ... / columns */
        char areas[1024], rows[1024];
        size_t an = 0, rn = 0;
        const char *w[64];
        size_t wl[64];
        int k = split_checked(t[0], tl[0], w, wl, 64, ' ', cx);
        bool pending = false; /* a string without its size yet */
        for (int i = 0; i < k; i++) {
            if (w[i][0] == '[') continue;
            if (w[i][0] == '"' || w[i][0] == '\'') {
                if (pending) {
                    if (rn + 6 > sizeof rows) return false;
                    memcpy(rows + rn, "auto ", 5), rn += 5;
                }
                if (an + wl[i] + 2 > sizeof areas) return false;
                memcpy(areas + an, w[i], wl[i]), an += wl[i];
                areas[an++] = ' ';
                pending = true;
            } else if (w[i][wl[i] - 1] != ']') {
                if (!pending || rn + wl[i] + 2 > sizeof rows) return false;
                memcpy(rows + rn, w[i], wl[i]), rn += wl[i];
                rows[rn++] = ' ';
                pending = false;
            }
        }
        if (pending) {
            if (rn + 6 > sizeof rows) return false;
            memcpy(rows + rn, "auto ", 5), rn += 5;
        }
        ok = set_long("grid-template-areas", areas, an, cx) && set_long("grid-template-rows", rows, rn, cx) &&
             set_long("grid-template-columns", t[1], tl[1], cx);
        }
        if (ok && p->sh == SH_GRID) {
            set_long("grid-auto-rows", "auto", 4, cx);
            set_long("grid-auto-columns", "auto", 4, cx);
            set_long("grid-auto-flow", "row", 3, cx);
        }
        return ok;
    }
    case SH_PLACE_ITEMS: {
        const char *t[2];
        size_t tl[2];
        int k = split_checked(v, n, t, tl, 2, ' ', cx);
        if (k < 1) return false;
        bool aligned = set_long("align-items", t[0], tl[0], cx);
        set_long("justify-items", t[k - 1], tl[k - 1], cx);
        return aligned;
    }
    }
    return false;
}

bool css_apply(const struct propdef *p, const char *v, size_t n, struct cx *cx) {
    trim(&v, &n);
    if (!n) return false;
    if (p->type == PT_SHORT) return apply_short(p, v, n, cx);
    int i = css_prop_index(p);
    if (cx->set[i]) return true;
    if (cx->font_pass && !early(p)) return true;
    if (!apply_long(p, v, n, cx)) return false;
    cx->set[i] = 1;
    return true;
}

static bool value_parse(const struct propdef *p, const char *v, size_t n, bool probe) {
    if (!p || !v || n == SIZE_MAX) return false;
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return false;
    if(probe)a->limit = 1u << 20;
    jmp_buf trap;
    a->trap = &trap;
    if (setjmp(trap)) { ar_free(a); free(a); return false; }
    style_t style, parent;
    css_style_init(&parent, NULL);
    css_style_init(&style, &parent);
    uint8_t *set = ar_alloc(a, (size_t)css_prop_count());
    struct cx cx = {.s=&style, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600,
                    .a=a, .set=set, .supports_probe=probe};
    bool result = css_apply(p, v, n, &cx) && !cx.invalid;
    ar_free(a);
    free(a);
    return result;
}
bool css_value_supported(const struct propdef *p,const char *v,size_t n) {
    return n <= 65536 && value_parse(p,v,n,true);
}
bool css_value_valid(const struct propdef *p,const char *v,size_t n) {
    return value_parse(p,v,n,false);
}

void css_style_init(style_t *s, const style_t *parent) {
    props_init();
    if (!parent) {
        *s = initial;
        return;
    }
    *s = *parent;
    s->border_radius=0; /* legacy native-only radius is not inherited */
    for (int i = 0; i < NPROPS; i++)
        if (props[i].type != PT_SHORT && !props[i].inherited && props[i].group == G_STYLE)
            copy_prop(&props[i], s, &initial, NULL);
    groups_copy(s, &initial);
    s->color_own = NULL;
    s->before = s->after = s->backdrop = s->placeholder = s->marker = NULL;
    s->before_generated = s->after_generated = s->explicit_inherit = false;
}

/* The inverse split of css_style_init: what it copies comes from from, what
   it resets stays s's own. */
void css_style_adopt_inherited(style_t *s, const style_t *from) {
    style_t own = *s;
    css_style_init(s, from);
    for (int i = 0; i < NPROPS; i++)
        if (props[i].type != PT_SHORT && !props[i].inherited && props[i].group == G_STYLE)
            copy_prop(&props[i], s, &own, NULL);
    groups_copy(s, &own);
    s->border_radius = own.border_radius;
    s->color_own = own.color_own;
    s->before = own.before; s->after = own.after; s->backdrop = own.backdrop; s->placeholder = own.placeholder;
    s->marker = own.marker;
    s->before_generated = own.before_generated; s->after_generated = own.after_generated;
    s->explicit_inherit = own.explicit_inherit;
}

void css_style_finish(style_t *s, const style_t *parent, bool root) {
    if (s->color == COLOR_CURRENT) s->color = parent ? parent->color : 0xFF000000u;
    css_color_finish(s);
    if (s->bg_color == COLOR_CURRENT) { s->bg_color = s->color; s->color_current |= 1u << CSS_SLOT_BACKGROUND; }
    for (struct css_box_shadow *shadow = (struct css_box_shadow *)s->box_shadow; shadow; shadow = shadow->next)
        if (shadow->color == COLOR_CURRENT)
            shadow->color = shadow->color_value ? css_color_value_argb(shadow->color_value, s->color) : s->color;
    for (int i = 0; i < 4; i++) {
        if (s->border_color[i] == COLOR_CURRENT) {
            s->border_color[i] = s->color;
            s->color_current |= (uint16_t)(1u << (CSS_SLOT_BORDER_TOP + i));
        }
        if (s->border_style[i] == BS_NONE || s->border_style[i] == BS_HIDDEN) s->border_width[i] = 0;
    }
    if (s->outline_color == COLOR_CURRENT) { s->outline_color = s->color; s->color_current |= 1u << CSS_SLOT_OUTLINE; }
    if (s->outline_style == BS_NONE) s->outline_width = 0;
    if (s->has_grad) {
        const struct gradient *g = s->gradient;
        for (int i = 0; i < 2; i++) {
            const struct css_color_value *exact = g && g->n ? g->col_value[i ? g->n - 1 : 0] : NULL;
            if (s->grad[i] == COLOR_CURRENT) s->grad[i] = exact ? css_color_value_argb(exact, s->color) : s->color;
        }
    }
    if (s->position == POS_ABSOLUTE || s->position == POS_FIXED) s->float_ = FL_NONE;
    /* floated, absolutely positioned, root and flex items are blockified */
    bool blockify = root || s->float_ != FL_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED;
    if (parent && (parent->display == D_FLEX || parent->display == D_INLINE_FLEX || parent->display == D_GRID ||
                   parent->display == D_INLINE_GRID))
        blockify = true;
    if (blockify) {
        switch (s->display) {
        case D_INLINE: case D_INLINE_BLOCK: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP:
        case D_TABLE_FOOTER_GROUP: case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_CAPTION:
        case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP:
            s->display = D_BLOCK;
            break;
        case D_INLINE_TABLE: s->display = D_TABLE; break;
        case D_INLINE_FLEX: s->display = D_FLEX; break;
        case D_INLINE_GRID: s->display = D_GRID; break;
        }
    }
    if (root && s->display == D_CONTENTS) s->display = D_BLOCK;
    /* Legacy vertical clamping is a block formatting context, not a flex
       row. Retain the authored display flag for CSS-wide copies/activation. */
    if (s->legacy_box && s->box_orient == BO_VERTICAL && s->line_clamp && s->display == D_BLOCK)
        s->display = D_FLOW_ROOT;
}

/* ---------------------------------------------------------------- the user agent stylesheet */
const char *css_ua_sheet(void) {
    /* Slots remain real DOM nodes but have no wrapper box by default. */
    return "slot { display: contents; }\n"
           "dialog { position: absolute; left: 0; right: 0; width: auto; height: auto; margin: auto; "
           "border: 3px solid black; padding: 1em; background-color: white; color: black; }\n"
           "dialog:modal { position: fixed; inset: 0; max-width: calc(100% - 2em - 6px); "
           "max-height: calc(100% - 2em - 6px); overflow: auto; }\n"
           "dialog::backdrop { display: block; position: fixed; inset: 0; background-color: rgba(0,0,0,0.1); }\n"
           /* The rest of the fullscreen UA rule is forced (web_presentation_style); this part is not. */
           ":not(:root):fullscreen { object-fit: contain; }\n"
           "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, "
           "legend, listing, main, p, plaintext, pre, search, xmp, article, aside, h1, h2, h3, h4, h5, h6, hgroup, "
           "nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup { display: block; }\n"
           "head, script, style, title, meta, link, base, template, datalist, param, noembed, noframes, area, map, "
           "track, source, rp, [hidden]:not([hidden=until-found i]), input[type=hidden], dialog:not([open]) "
           "{ display: none; }\n"
           "[hidden=until-found i] { content-visibility: hidden; }\n"
           "li { display: list-item; }\n"
           "table { display: table; border-spacing: 2px; border-collapse: separate; box-sizing: border-box; "
           "text-indent: 0; }\n"
           "caption { display: table-caption; text-align: center; }\n"
           "colgroup { display: table-column-group; } col { display: table-column; }\n"
           "thead { display: table-header-group; vertical-align: middle; }\n"
           "tbody { display: table-row-group; vertical-align: middle; }\n"
           "tfoot { display: table-footer-group; vertical-align: middle; }\n"
           "tr { display: table-row; vertical-align: inherit; }\n"
           "td, th { display: table-cell; vertical-align: inherit; padding: 1px; }\n"
           "th { font-weight: bold; text-align: center; }\n"
           "body { margin: 8px; line-height: normal; }\n"
           "p, dl, multicol { margin: 1em 0; }\n"
           "dd { margin-left: 40px; }\n"
           "blockquote, figure { margin: 1em 40px; }\n"
           "address { font-style: italic; }\n"
           "center { text-align: -webkit-center; }\n"
           "h1 { font-size: 2em; margin: 0.67em 0; font-weight: bold; }\n"
           "h2 { font-size: 1.5em; margin: 0.83em 0; font-weight: bold; }\n"
           "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold; }\n"
           "h4 { margin: 1.33em 0; font-weight: bold; }\n"
           "h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold; }\n"
           "h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold; }\n"
           "article h1, aside h1, nav h1, section h1 { font-size: 1.5em; margin: 0.83em 0; }\n"
           "pre, listing, xmp, plaintext { white-space: pre; margin: 1em 0; font-family: monospace; }\n"
           "code, kbd, samp, tt, var { font-family: monospace; }\n"
           "textarea { white-space: pre-wrap; font-family: monospace; }\n"
           "ul, menu, dir { list-style-type: disc; margin: 1em 0; padding-left: 40px; }\n"
           "ol { list-style-type: decimal; margin: 1em 0; padding-left: 40px; }\n"
           "ul ul, ol ul, ul ol, ol ol, menu ul, ul menu { margin-top: 0; margin-bottom: 0; }\n"
           "ul ul, ol ul, menu ul { list-style-type: circle; }\n"
           "ul ul ul, ul ol ul, ol ul ul, ol ol ul { list-style-type: square; }\n"
           /* HTML §15.3.8: the lists scope the list-item counter */
           "ol, ul, menu { counter-reset: list-item; }\n"
           "hr { color: gray; border-style: inset; border-width: 1px; margin: 0.5em auto; overflow: hidden; }\n"
           "b, strong { font-weight: bolder; }\n"
           "i, cite, em, var, dfn { font-style: italic; }\n"
           "big { font-size: larger; } small { font-size: smaller; }\n"
           "sub { vertical-align: sub; font-size: smaller; } sup { vertical-align: super; font-size: smaller; }\n"
           "u, ins { text-decoration: underline; } s, strike, del { text-decoration: line-through; }\n"
           "mark { background-color: yellow; color: black; }\n"
           "a:link { color: #0645ad; text-decoration: underline; }\n"
           "abbr[title] { text-decoration: underline; }\n"
           "q::before { content: open-quote; } q::after { content: close-quote; }\n"
           "img, video, canvas, iframe, embed, svg, input, select, textarea, button, meter, progress "
           "{ display: inline-block; }\n"
           "iframe { border: 2px inset; }\n"
           "frameset, frame { display:block; margin:0; padding:0; border:0; }\n"
           /* HTML canvas starts with transparent black pixels. Giving replaced
              elements an invented gray CSS background made an empty overlay
              canvas hide the real video underneath it. */
           "fieldset { margin: 0 2px; padding: 0.35em 0.75em 0.625em; border: 2px groove #c0c0c0; }\n"
           "legend { padding: 0 2px; }\n"
           "input, select, textarea, button { font-size: 13.33px; font-family: sans-serif; color: black; "
           "letter-spacing: normal; text-align: start; text-indent: 0; text-transform: none; line-height: normal; "
           "box-sizing: border-box; }\n"
           "input, textarea, select { background-color: white; border: 1px solid #767676; padding: 2px 3px; "
           "border-radius: 2px; }\n"
           "button, input[type=submit], input[type=button], input[type=reset] { background-color: #efefef; "
           "border: 1px solid #767676; padding: 2px 8px; border-radius: 3px; text-align: center; }\n"
           "input[type=checkbox], input[type=radio] { border: none; padding: 0; margin: 3px 4px; "
           "background-color: transparent; }\n"
           "input[type=image] { border: none; padding: 0; }\n"
           "select { padding-right: 18px; }\n"
           "option, optgroup { display: none; }\n"
           "summary { display: block; }\n"
           "details > summary:first-of-type { display: list-item; counter-increment: list-item 0; list-style: disclosure-closed inside; }\n"
           "details[open] > summary:first-of-type { list-style-type: disclosure-open; }\n"
           "ruby { display: ruby; } rt { font-size: 50%; }\n"
           "nobr { white-space: nowrap; }\n"
           "wbr { display: inline; }\n"
           "marquee { display: inline-block; width: 100%; overflow: hidden; white-space: nowrap; }\n"
           "math { display: inline; }\n"
           "svg:not(:root) { overflow: hidden; }\n"
           "@media (scripting:enabled) { noscript { display: none !important; } }\n";
}

/* ---------------------------------------------------------------- animation values
   The one place computed values combine (CSS Values 4 §3 "Combining values", Web Animations 1
   §5.3 "Animation types"). CSS Transitions, CSS Animations and script animations all reach it
   through animation.c. The property table gives each longhand its animation type
   (propdef.anim, else its value type's default); below, each value type has its interpolation,
   addition and accumulation. Values come in as computed values in scratch copies of the
   element's base style (css_anim_parse) and leave as computed-value text, which the cascade
   applies at the animation or transition origin so that em, currentcolor and inheritance
   follow the animated values. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
int css_prop_animation(const struct propdef *p) {
    if (!p || p->type == PT_SHORT) return CSS_ANIM_NONE;
    switch (p->anim) {
    case AN_NONE: return CSS_ANIM_NONE;
    case AN_DISCRETE: return CSS_ANIM_DISCRETE;
    case AN_VISIBILITY: return CSS_ANIM_VISIBILITY;
    case AN_DISPLAY: return CSS_ANIM_DISPLAY;
    }
    switch (p->type) {
    case PT_MOTION_LIST: case PT_CONTAINER_NAMES: return CSS_ANIM_NONE;
    case PT_DISPLAY: return CSS_ANIM_DISPLAY;
    case PT_KW: case PT_OVERFLOW: case PT_FONT_FAMILY: case PT_CONTENT: case PT_LIST_TYPE: case PT_BG_IMAGE:
    case PT_TEXT_DECO: case PT_GTEMPLATE: case PT_GAREAS: case PT_GLINE: case PT_MASK_IMAGE: case PT_LINE_CLAMP:
    case PT_COUNTER: case PT_QUOTES: case PT_LIST_IMAGE:
        return CSS_ANIM_DISCRETE;
    }
    return CSS_ANIM_VALUE;
}
/* A shorthand's longhands are what its own parser sets for "initial" (the same expansion
   revert uses), computed once per shorthand. */
static uint8_t longhand_sets[NPROPS][NPROPS];
static uint8_t longhand_ready[NPROPS];
void css_prop_longhands(const struct propdef *p, uint8_t *set) {
    memset(set, 0, NPROPS);
    if (!p) return;
    int i = css_prop_index(p);
    if (p->type != PT_SHORT) { set[i] = 1; return; }
    if (!__atomic_load_n(&longhand_ready[i], __ATOMIC_ACQUIRE)) {
        arena_t a = {0}; jmp_buf trap; a.trap = &trap;
        uint8_t bits[NPROPS]; memset(bits, 0, sizeof bits);
        if (!setjmp(trap)) {
            style_t st, parent; css_style_init(&parent, NULL); css_style_init(&st, &parent);
            struct cx cx = {.s=&st, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600, .a=&a, .set=bits};
            css_apply(p, "initial", 7, &cx);
        }
        ar_free(&a);
        memcpy(longhand_sets[i], bits, NPROPS);
        __atomic_store_n(&longhand_ready[i], 1, __ATOMIC_RELEASE);
    }
    memcpy(set, longhand_sets[i], NPROPS);
}

/* ---------------------------------------------------------------- CSSOM shorthands
   A CSSOM declaration block holds longhands (CSSOM 6.6 "CSS declaration blocks"): setting a
   shorthand sets each longhand it maps to, and serialization puts them back together. Both
   directions follow the shorthand's own parser (apply_short): the longhands it maps to are the
   names it gives a value for "initial" (the expansion css_prop_longhands reads), in that,
   canonical, order; a value's longhand values are the texts it gives each (set_long, through
   cx->record), in the strict grammar CSS.supports() reads. */
struct longhand_names { int count; const char *names[]; };
static struct longhand_names *longhand_lists[NPROPS];
static void record_name(void *opaque, const char *name, const char *value, size_t n) {
    (void)value; (void)n;
    sbuf *b = opaque;
    sb_puts(b, name); sb_putc(b, 0);
}
int css_shorthand_longhands(const struct propdef *p, const char *const **names) {
    *names = NULL;
    if (!p || p->type != PT_SHORT) return 0;
    int i = css_prop_index(p);
    struct longhand_names *list = __atomic_load_n(&longhand_lists[i], __ATOMIC_ACQUIRE);
    if (!list) {
        sbuf b = {0};
        arena_t a = {0}; jmp_buf trap; a.trap = &trap;
        uint8_t bits[NPROPS]; memset(bits, 0, sizeof bits);
        if (!setjmp(trap)) {
            style_t st, parent; css_style_init(&parent, NULL); css_style_init(&st, &parent);
            struct cx cx = {.s=&st, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600, .a=&a, .set=bits,
                            .record=record_name, .record_opaque=&b};
            css_apply(p, "initial", 7, &cx);
        }
        ar_free(&a);
        int count = 0;
        for (size_t k = 0; k < b.n; k++) if (!b.p[k]) count++;
        list = malloc(sizeof *list + (size_t)count * sizeof *list->names + b.n);
        if (!list) { sb_free(&b); return 0; }
        char *text = (char *)(list->names + count);
        if (b.n) memcpy(text, b.p, b.n);
        list->count = count;
        for (int k = 0; k < count; k++) { list->names[k] = text; text += strlen(text) + 1; }
        sb_free(&b);
        struct longhand_names *none = NULL;
        if (!__atomic_compare_exchange_n(&longhand_lists[i], &none, list, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            free(list); list = none;
        }
    }
    *names = list->names;
    return list->count;
}

static bool css_wide_keyword(const char *v, size_t n) {
    return ident_is(v, n, "inherit") || ident_is(v, n, "initial") || ident_is(v, n, "unset") ||
           ident_is(v, n, "revert") || ident_is(v, n, "revert-layer");
}
/* the four corners' values of border-radius: h, or "h v" when the vertical radius differs */
static int radius_expand(const struct propdef *p, const char *v, size_t n, const char *const *names,
                         css_longhand_record *each, void *opaque) {
    if (!css_value_supported(p, v, n)) return css_value_valid(p, v, n) ? -1 : 0;
    const char *half[2], *t[2][4];
    size_t hl[2], tl[2][4];
    int halves = split(v, n, half, hl, 2, '/'), k[2] = {0, 0};
    for (int h = 0; h < halves; h++) k[h] = split(half[h], hl[h], t[h], tl[h], 4, ' ');
    if (halves < 1 || k[0] < 1 || (halves == 2 && k[1] < 1)) return 0;
    static const int pick[5][4] = {{0}, {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    sbuf corner = {0};
    for (int i = 0; i < 4; i++) {
        int a = pick[k[0]][i];
        corner.n = 0;
        sb_put(&corner, t[0][a], tl[0][a]);
        if (halves == 2) {
            int b = pick[k[1]][i];
            if (tl[1][b] != tl[0][a] || memcmp(t[1][b], t[0][a], tl[0][a])) { sb_putc(&corner, ' '); sb_put(&corner, t[1][b], tl[1][b]); }
        }
        each(opaque, names[i], corner.p, corner.n);
    }
    sb_free(&corner);
    return 1;
}
static void record_value(void *opaque, const char *name, const char *value, size_t n) {
    sbuf *b = opaque;
    sb_puts(b, name); sb_putc(b, 0); sb_put(b, value, n); sb_putc(b, 0);
}
int css_shorthand_expand(const struct propdef *p, const char *v, size_t n, css_longhand_record *each, void *opaque) {
    if (!p || p->type != PT_SHORT) return 0;
    trim(&v, &n);
    const char *const *names;
    int count = css_shorthand_longhands(p, &names);
    if (!n) return 0;
    if (!count) return -2;
    if (css_wide_keyword(v, n)) { for (int i = 0; i < count; i++) each(opaque, names[i], v, n); return 1; }
    if (p->sh == SH_ALL) return 0;
    /* values whose parser writes no longhand text (motion lists, offset), or
       loses some of it (grid-template's areas with line names) */
    if (p->sh == SH_ANIMATION || p->sh == SH_TRANSITION || p->sh == SH_OFFSET ||
        ((p->sh == SH_GRID || p->sh == SH_GRID_TEMPLATE) && (memchr(v, '"', n) || memchr(v, '\'', n))))
        return css_value_valid(p, v, n) ? -1 : 0;
    if (p->sh == SH_BORDER_RADIUS) return radius_expand(p, v, n, names, each, opaque);
    sbuf given = {0};
    arena_t *a = calloc(1, sizeof *a);
    if (!a) return -2;
    a->limit = 1u << 20;
    jmp_buf trap;
    a->trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t st, parent; css_style_init(&parent, NULL); css_style_init(&st, &parent);
        uint8_t *set = ar_alloc(a, NPROPS);
        struct cx cx = {.s=&st, .parent=&parent, .em=16, .rem=16, .vw=800, .vh=600, .a=a, .set=set,
                        .supports_probe=true, .record=record_value, .record_opaque=&given};
        ok = css_apply(p, v, n, &cx) && !cx.invalid;
    }
    ar_free(a); free(a);
    /* every longhand, or the block keeps the value as the shorthand's own */
    int found = 0;
    for (size_t at = 0; ok && at < given.n; found++) { at += strlen(given.p + at) + 1; at += strlen(given.p + at) + 1; }
    int result = ok && found == count ? 1 : css_value_valid(p, v, n) ? -1 : 0;
    for (size_t at = 0; result == 1 && at < given.n;) {
        const char *name = given.p + at; at += strlen(name) + 1;
        const char *value = given.p + at; at += strlen(value) + 1;
        each(opaque, name, value, strlen(value));
    }
    sb_free(&given);
    return result;
}

static bool text_same(const char *a, size_t an, const char *b, size_t bn) { return an == bn && !memcmp(a, b, an); }
static bool text_word(const char *a, size_t an, const char *word) { return an == strlen(word) && strn_ieq(a, word, an); }
/* 1-4 values for top, right, bottom, left: the shortest that gives each */
static void put_box(sbuf *out, const char *const *v, const size_t *l) {
    int k = 4;
    if (text_same(v[3], l[3], v[1], l[1])) {
        k = 3;
        if (text_same(v[2], l[2], v[0], l[0])) { k = 2; if (text_same(v[1], l[1], v[0], l[0])) k = 1; }
    }
    for (int i = 0; i < k; i++) { if (i) sb_putc(out, ' '); sb_put(out, v[i], l[i]); }
}
static void put_pair(sbuf *out, const char *a, size_t an, const char *b, size_t bn) {
    sb_put(out, a, an);
    if (!text_same(a, an, b, bn)) { sb_putc(out, ' '); sb_put(out, b, bn); }
}
/* the values (in order: indexes into v) other than their initial ones; all
   initial, the one at fallback */
static void put_given(sbuf *out, const char *const *v, const size_t *l, const int *order, const char *const *initial,
                      int count, int fallback) {
    size_t at = out->n;
    for (int i = 0; i < count; i++) {
        int j = order[i];
        if (text_word(v[j], l[j], initial[i])) continue;
        if (out->n > at) sb_putc(out, ' ');
        sb_put(out, v[j], l[j]);
    }
    if (out->n == at) sb_put(out, v[fallback], l[fallback]);
}
static bool equal_groups(const char *const *v, const size_t *l, int groups, int size) {
    for (int g = 0; g < groups; g++)
        for (int i = 1; i < size; i++) if (!text_same(v[g * size], l[g * size], v[g * size + i], l[g * size + i])) return false;
    return true;
}
/* a grid line the shorthand implies when it omits one: a custom ident stands for itself, else auto */
static bool implied_line(const char *line, size_t ln, const char *from, size_t fn) {
    return is_custom_ident(from, fn) ? text_same(line, ln, from, fn) : text_word(line, ln, "auto");
}
bool css_shorthand_serialize(const struct propdef *p, const char *const *v, const size_t *l, sbuf *out) {
    const char *const *names;
    int count = css_shorthand_longhands(p, &names);
    if (!count) return false;
    /* a CSS-wide keyword stands for the shorthand only when every longhand has it */
    int wide = 0;
    for (int i = 0; i < count; i++) wide += css_wide_keyword(v[i], l[i]);
    if (wide == count) {
        for (int i = 1; i < count; i++) if (!text_same(v[i], l[i], v[0], l[0])) return false;
        sb_put(out, v[0], l[0]);
        return true;
    }
    /* border also resets border-image (css-backgrounds-3 §4.4), whose longhands follow the
       sides': the shorthand stands for the declarations only while they keep their initial
       values, which it does not write */
    if (p->sh == SH_BORDER) {
        static const char *const image_initial[][2] = {{"border-image-source", "none"},
            {"border-image-slice", "100%"}, {"border-image-width", "1"}, {"border-image-outset", "0"},
            {"border-image-repeat", "stretch"}};
        int own = count;
        for (int i = 0; i < count; i++) {
            if (strncmp(names[i], "border-image-", 13)) {
                if (own != count) return false;
                continue;
            }
            if (own == count) own = i;
            bool initial = text_word(v[i], l[i], "initial");
            wide -= initial;
            for (size_t k = 0; !initial && k < sizeof image_initial / sizeof *image_initial; k++)
                initial = !strcmp(names[i], image_initial[k][0]) &&
                          text_same(v[i], l[i], image_initial[k][1], strlen(image_initial[k][1]));
            if (!initial) return false;
        }
        count = own;
    }
    if (wide) return false;
    static const int wsc[3] = {0, 1, 2}, csw[3] = {2, 1, 0};
    static const char *const border_initial[3] = {"medium", "none", "currentcolor"};
    switch (p->sh) {
    case SH_MARGIN: case SH_PADDING: case SH_INSET: case SH_BORDER_WIDTH: case SH_BORDER_STYLE: case SH_BORDER_COLOR:
        if (count != 4) return false;
        put_box(out, v, l);
        return true;
    case SH_MARGIN_INLINE: case SH_MARGIN_BLOCK: case SH_PADDING_INLINE: case SH_PADDING_BLOCK: case SH_INSET_INLINE:
    case SH_INSET_BLOCK: case SH_GAP: case SH_OVERFLOW: case SH_PLACE_ITEMS: case SH_PLACE_SELF: case SH_PLACE_CONTENT:
        if (count != 2) return false;
        put_pair(out, v[0], l[0], v[1], l[1]);
        return true;
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT:
        if (count != 3) return false;
        put_given(out, v, l, wsc, border_initial, 3, 1);
        return true;
    case SH_BORDER: case SH_BORDER_INLINE: case SH_BORDER_BLOCK: {
        /* the widths, styles and colours of every side: one value for all of them */
        int sides = count / 3;
        if (sides * 3 != count || !equal_groups(v, l, 3, sides)) return false;
        const char *side[3] = {v[0], v[sides], v[2 * sides]};
        size_t side_l[3] = {l[0], l[sides], l[2 * sides]};
        put_given(out, side, side_l, wsc, border_initial, 3, 1);
        return true;
    }
    case SH_OUTLINE: {
        static const char *const initial[3] = {"currentcolor", "none", "medium"};
        if (count != 3) return false;
        put_given(out, v, l, csw, initial, 3, 1);
        return true;
    }
    case SH_LIST_STYLE: {
        /* position, image, type; a type named inside or outside alone would read back as the
           position, so the position is written before it (list_style_text does the same) */
        static const int order[3] = {0, 1, 2};
        static const char *const initial[3] = {"outside", "none", "disc"};
        if (count != 3) return false;
        if (text_word(v[2], l[2], "inside") || text_word(v[2], l[2], "outside")) {
            sb_put(out, v[0], l[0]);
            if (!text_word(v[1], l[1], "none")) { sb_putc(out, ' '); sb_put(out, v[1], l[1]); }
            sb_putc(out, ' ');
            sb_put(out, v[2], l[2]);
            return true;
        }
        put_given(out, v, l, order, initial, 3, 0);
        return true;
    }
    case SH_FLEX_FLOW: {
        static const int order[2] = {0, 1};
        static const char *const initial[2] = {"row", "nowrap"};
        if (count != 2) return false;
        put_given(out, v, l, order, initial, 2, 0);
        return true;
    }
    case SH_FLEX:
        if (count != 3) return false;
        for (int i = 0; i < 3; i++) { if (i) sb_putc(out, ' '); sb_put(out, v[i], l[i]); }
        return true;
    case SH_TEXT_DECORATION:
        if (count != 1) return false;
        sb_put(out, v[0], l[0]);
        return true;
    case SH_ALL: case SH_ANIMATION: case SH_TRANSITION: case SH_OFFSET:
        return false; /* only a CSS-wide keyword: their longhands' values are not the shorthand's text */
    case SH_FONT: {
        /* style, weight, size, line height, family: [style] [weight] size[ / line-height] family */
        if (count != 5) return false;
        for (int i = 0; i < 2; i++) if (!text_word(v[i], l[i], "normal")) { sb_put(out, v[i], l[i]); sb_putc(out, ' '); }
        sb_put(out, v[2], l[2]);
        if (!text_word(v[3], l[3], "normal")) { sb_puts(out, " / "); sb_put(out, v[3], l[3]); }
        sb_putc(out, ' ');
        sb_put(out, v[4], l[4]);
        return true;
    }
    case SH_BACKGROUND: {
        /* image, position, size, repeat, colour: [colour] [image] [position[ / size]] [repeat], all initial none */
        if (count != 5) return false;
        size_t at = out->n;
        const char *sep = "";
        if (!text_word(v[4], l[4], "transparent")) { sb_put(out, v[4], l[4]); sep = " "; }
        if (!text_word(v[0], l[0], "none")) { sb_puts(out, sep); sb_put(out, v[0], l[0]); sep = " "; }
        bool sized = !text_word(v[2], l[2], "auto");
        if (sized || !text_word(v[1], l[1], "0% 0%")) {
            sb_puts(out, sep); sb_put(out, v[1], l[1]); sep = " ";
            if (sized) { sb_puts(out, " / "); sb_put(out, v[2], l[2]); }
        }
        if (!text_word(v[3], l[3], "repeat")) { sb_puts(out, sep); sb_put(out, v[3], l[3]); }
        if (out->n == at) sb_puts(out, "none");
        return true;
    }
    case SH_MASK: {
        if (count != MK_PARTS) return false;
        for (int i = 0; i < MK_PARTS; i++) if (strcmp(names[i], mask_names[i])) return false;
        size_t at = out->n;
        if (mask_shorthand_text(v, l, false, out)) return true;
        out->n = at;
        if (out->p && at < out->cap) out->p[at] = 0;
        return false;
    }
    case SH_GRID_ROW: case SH_GRID_COLUMN:
        if (count != 2) return false;
        sb_put(out, v[0], l[0]);
        if (!implied_line(v[1], l[1], v[0], l[0])) { sb_puts(out, " / "); sb_put(out, v[1], l[1]); }
        return true;
    case SH_GRID_AREA: {
        /* row-start, column-start, row-end, column-end: the end ones the shorthand implies are left out */
        if (count != 4) return false;
        int k = 4;
        if (implied_line(v[3], l[3], v[1], l[1])) {
            k = 3;
            if (implied_line(v[2], l[2], v[0], l[0])) { k = 2; if (implied_line(v[1], l[1], v[0], l[0])) k = 1; }
        }
        for (int i = 0; i < k; i++) { if (i) sb_puts(out, " / "); sb_put(out, v[i], l[i]); }
        return true;
    }
    case SH_GRID_TEMPLATE: case SH_GRID: {
        /* rows, columns, areas (grid: then the implicit grid's rows, columns and flow) */
        if (count < 3 || !text_word(v[2], l[2], "none")) return false;
        bool implicit = count == 6 && (!text_word(v[3], l[3], "auto") || !text_word(v[4], l[4], "auto") ||
                                       !text_word(v[5], l[5], "row"));
        if (!implicit) {
            if (text_word(v[0], l[0], "none") && text_word(v[1], l[1], "none")) sb_puts(out, "none");
            else { sb_put(out, v[0], l[0]); sb_puts(out, " / "); sb_put(out, v[1], l[1]); }
            return true;
        }
        bool dense = l[5] > 5 && (strn_ieq(v[5], "dense ", 6) || (l[5] > 6 && strn_ieq(v[5] + l[5] - 6, " dense", 6)));
        bool column = l[5] >= 6 && (strn_ieq(v[5], "column", 6) || strn_ieq(v[5] + l[5] - 6, "column", 6));
        if (!column && text_word(v[0], l[0], "none") && text_word(v[4], l[4], "auto")) {
            sb_puts(out, dense ? "auto-flow dense" : "auto-flow");
            if (!text_word(v[3], l[3], "auto")) { sb_putc(out, ' '); sb_put(out, v[3], l[3]); }
            sb_puts(out, " / "); sb_put(out, v[1], l[1]);
            return true;
        }
        if (column && text_word(v[1], l[1], "none") && text_word(v[3], l[3], "auto")) {
            sb_put(out, v[0], l[0]); sb_puts(out, dense ? " / auto-flow dense" : " / auto-flow");
            if (!text_word(v[4], l[4], "auto")) { sb_putc(out, ' '); sb_put(out, v[4], l[4]); }
            return true;
        }
        return false;
    }
    case SH_CONTAINER:
        if (count != 2) return false;
        sb_put(out, v[0], l[0]);
        if (!text_word(v[1], l[1], "normal")) { sb_puts(out, " / "); sb_put(out, v[1], l[1]); }
        return true;
    case SH_BORDER_RADIUS: {
        /* each corner "h" or "h v": the horizontal radii, then " / " and the vertical ones when they differ */
        if (count != 4) return false;
        const char *h[4], *vv[4], *t[2];
        size_t hn[4], vn[4], tl[2];
        bool differ = false;
        for (int i = 0; i < 4; i++) {
            int k = split(v[i], l[i], t, tl, 2, ' ');
            if (k < 1) return false;
            h[i] = t[0]; hn[i] = tl[0];
            vv[i] = t[k - 1]; vn[i] = tl[k - 1];
            differ |= !text_same(h[i], hn[i], vv[i], vn[i]);
        }
        put_box(out, h, hn);
        if (differ) { sb_puts(out, " / "); put_box(out, vv, vn); }
        return true;
    }
    }
    return false;
}

/* each longhand name with the shorthands it is in, in CSSOM's preferred order (CSSOM
   "preferred order": the most longhands first, then by name) */
struct shorthands_entry { const char *name; int count; const struct propdef *shorthands[8]; };
struct shorthands_index { int count; struct shorthands_entry entries[]; };
static struct shorthands_index *shorthands_index;
static int preferred_cmp(const void *a, const void *b) {
    const struct propdef *x = *(const struct propdef *const *)a, *y = *(const struct propdef *const *)b;
    const char *const *names;
    int xn = css_shorthand_longhands(x, &names), yn = css_shorthand_longhands(y, &names);
    return xn != yn ? (xn > yn ? -1 : 1) : strcmp(x->name, y->name);
}
struct shorthand_pair { const char *name; const struct propdef *shorthand; int order; };
static int pair_cmp(const void *a, const void *b) {
    const struct shorthand_pair *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : x->order - y->order;
}
static int entry_cmp(const void *key, const void *entry) {
    return strcmp(key, ((const struct shorthands_entry *)entry)->name);
}
int css_shorthands_of(const char *longhand, const struct propdef *const **shorthands) {
    *shorthands = NULL;
    props_init();
    struct shorthands_index *index = __atomic_load_n(&shorthands_index, __ATOMIC_ACQUIRE);
    if (!index) {
        const struct propdef *list[NPROPS];
        int n = 0, pairs = 0;
        for (int i = 0; i < NPROPS; i++) if (props[i].type == PT_SHORT) list[n++] = &props[i];
        qsort(list, (size_t)n, sizeof *list, preferred_cmp);
        for (int i = 0; i < n; i++) { const char *const *names; pairs += css_shorthand_longhands(list[i], &names); }
        struct shorthand_pair *pair = malloc((size_t)(pairs ? pairs : 1) * sizeof *pair);
        if (!pair) return 0;
        int k = 0;
        for (int i = 0; i < n; i++) {
            const char *const *names;
            int count = css_shorthand_longhands(list[i], &names);
            for (int j = 0; j < count; j++, k++) pair[k] = (struct shorthand_pair){names[j], list[i], k};
        }
        qsort(pair, (size_t)k, sizeof *pair, pair_cmp);
        int entries = 0;
        for (int i = 0; i < k; i++) entries += !i || strcmp(pair[i].name, pair[i - 1].name);
        index = calloc(1, sizeof *index + (size_t)entries * sizeof *index->entries);
        if (!index) { free(pair); return 0; }
        for (int i = 0; i < k; i++) {
            if (!i || strcmp(pair[i].name, pair[i - 1].name)) index->entries[index->count++].name = pair[i].name;
            struct shorthands_entry *e = &index->entries[index->count - 1];
            if (e->count < 8) e->shorthands[e->count++] = pair[i].shorthand;
        }
        free(pair);
        struct shorthands_index *none = NULL;
        if (!__atomic_compare_exchange_n(&shorthands_index, &none, index, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            free(index); index = none;
        }
    }
    const struct shorthands_entry *e = bsearch(longhand, index->entries, (size_t)index->count, sizeof *index->entries, entry_cmp);
    if (!e) return 0;
    *shorthands = e->shorthands;
    return e->count;
}

/* CSS Logical 1's logical property groups (the ones the table has): a longhand's group, and
   whether its mapping is flow-relative (a logical name, the table's alias of the physical
   property it maps to here). false: in no group. */
bool css_prop_logical_group(const char *name, char *group, size_t size, bool *logical) {
    *logical = false;
    const char *physical = name;
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (!strcmp(aliases[i].alias, name) && aliases[i].name[0] && (strstr(name, "inline") || strstr(name, "block"))) {
            physical = aliases[i].name; *logical = true; break;
        }
    static const struct { const char *name, *group; } fixed[] = {
        {"top", "inset"}, {"right", "inset"}, {"bottom", "inset"}, {"left", "inset"}, {"width", "size"}, {"height", "size"},
        {"min-width", "min-size"}, {"min-height", "min-size"}, {"max-width", "max-size"}, {"max-height", "max-size"}};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (!strcmp(physical, fixed[i].name)) { snprintf(group, size, "%s", fixed[i].group); return true; }
    static const char *const families[] = {"margin-", "padding-", "border-"};
    for (int f = 0; f < 3; f++) {
        size_t fl = strlen(families[f]);
        if (strncmp(physical, families[f], fl)) continue;
        for (int s = 0; s < 4; s++) {
            size_t sl = strlen(side_names[s]);
            const char *rest = physical + fl + sl;
            if (strncmp(physical + fl, side_names[s], sl)) continue;
            if (f < 2 && !*rest) { snprintf(group, size, "%.*s", (int)(fl - 1), families[f]); return true; }
            if (f == 2 && (!strcmp(rest, "-width") || !strcmp(rest, "-style") || !strcmp(rest, "-color"))) {
                snprintf(group, size, "border%s", rest); return true;
            }
        }
    }
    *logical = false;
    return false;
}

/* Fixed notation within the CSS number grammar, to the precision of the stored floats
   (seven significant digits) so a value read back serializes as it was written. */
static void anim_number(sbuf *b, double v) {
    if (!isfinite(v) || fabs(v) < 5e-10) v = 0;
    if (v > 1e15) v = 1e15; else if (v < -1e15) v = -1e15;
    char text[64]; snprintf(text, sizeof text, "%.7g", v);
    if (strchr(text, 'e')) {
        int len = snprintf(text, sizeof text, "%.6f", v);
        while (len > 1 && text[len - 1] == '0') text[--len] = 0;
        if (len > 1 && text[len - 1] == '.') text[--len] = 0;
    }
    if (!strcmp(text, "-0")) strcpy(text, "0");
    sb_puts(b, text);
}
static void anim_unit(sbuf *b, double v, const char *unit) { anim_number(b, v); sb_puts(b, unit); }
static double anim_clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the combinations: interpolation, addition, accumulation */
enum { MIX, ADD, ACCUMULATE };
/* <color>: computed values of color.c's type (exact, or an expression of currentcolor) */
static void anim_shadow_color(const style_t *st, const struct css_box_shadow *s, struct css_color_computed *out) {
    css_color_held(s->color, s->color_value, st->color, out);
}
/* the used value a computed colour names: currentcolor is the style's color (CSS Color 4 §13) */
static struct css_color anim_color_used(const style_t *st, const struct css_color_computed *v) {
    if (!v->current) return v->color;
    struct css_color_computed current;
    css_style_color(st, CSS_SLOT_COLOR, &current);
    struct css_color out;
    css_color_resolve(v, &current.color, &out);
    return out;
}
/* currentcolor and expressions of it as written, for the cascade to resolve against the element */
static bool anim_color_text(sbuf *b, const struct css_color_computed *v) {
    return v->current ? css_color_value_specified(v->current, b) : css_color_animation_text(&v->color, b);
}
/* x interpolated to y at p (MIX: CSS Color 4 §13, in the space §13.2 gives the two), or y added to
   x (ADD and ACCUMULATE alike: CSS Values 4 §3.3); x's and y's currentcolor are their styles' */
static bool anim_color(sbuf *b, const style_t *sx, const struct css_color_computed *x, const style_t *sy,
                       const struct css_color_computed *y, double p, int mode) {
    /* between one expression of currentcolor the value stays it, resolved after the cascade */
    if (mode == MIX && x->current && css_color_computed_equal(x, y)) return anim_color_text(b, x);
    struct css_color u = anim_color_used(sx, x), v = anim_color_used(sy, y);
    struct css_color_interpolation m = css_color_default_interpolation(&u, &v);
    struct css_color r = mode == MIX ? css_color_interpolate(&u, &v, p, &m) : css_color_add(&u, &v, &m);
    return css_color_animation_text(&r, b);
}
static bool anim_is_length(const len_t *l) { return l->kind == LK_LEN || l->kind == LK_EXPR; }
/* ka·x + kb·y of two <length-percentage>s; a keyword (auto, none, normal) combines only with
   itself. A property without negative values clamps (calc() ranges clamp when parsed). */
static bool anim_len(sbuf *b, const len_t *x, const len_t *y, double ka, double kb, int flags, web_doc *d) {
    if (!anim_is_length(x) || !anim_is_length(y)) {
        if (x->kind != y->kind || anim_is_length(x)) return false;
        if (x->kind == LK_NUMBER) { anim_number(b, x->px * ka + y->px * kb); return true; }
        return css_length_serialize(x, b, d);
    }
    if (x->kind == LK_LEN && y->kind == LK_LEN) {
        double px = x->px * ka + y->px * kb, pct = x->pct * ka + y->pct * kb;
        if (!(flags & LF_NEG)) { if (fabs(pct) < 1e-9 && px < 0) px = 0; if (fabs(px) < 1e-9 && pct < 0) pct = 0; }
        if (fabs(pct) < 1e-9) { anim_unit(b, px, "px"); return true; }
        if (fabs(px) < 1e-9) { anim_unit(b, pct, "%"); return true; }
        sb_puts(b, "calc("); anim_unit(b, pct, "%"); sb_puts(b, " + "); anim_unit(b, px, "px"); sb_putc(b, ')');
        return true;
    }
    /* calc() keeps percentages unresolved: combine the expressions */
    sbuf ta = {0}, tb = {0};
    bool ok = css_length_serialize(x, &ta, d) && css_length_serialize(y, &tb, d);
    if (ok) {
        sb_puts(b, "calc("); sb_puts(b, sb_cstr(&ta)); sb_puts(b, " * "); anim_number(b, ka);
        sb_puts(b, " + "); sb_puts(b, sb_cstr(&tb)); sb_puts(b, " * "); anim_number(b, kb); sb_putc(b, ')');
    }
    sb_free(&ta); sb_free(&tb);
    return ok;
}

/* -------- transform lists (CSS Transforms 1 §9, CSS Transforms 2 §16) */
static bool tf_axis(const struct css_transform *t, double axis[3]) {
    double x = t->v[0], y = t->v[1], z = t->v[2], length = sqrt(x * x + y * y + z * z);
    if (!(length > 0) || !isfinite(length)) return false;
    axis[0] = x / length; axis[1] = y / length; axis[2] = z / length;
    return true;
}
static bool tf_same_axis(const struct css_transform *a, const struct css_transform *b) {
    if (a->v[3] == 0 || b->v[3] == 0) return true;
    double p[3], q[3];
    if (!tf_axis(a, p) || !tf_axis(b, q)) return false;
    return fabs(p[0] - q[0]) <= 1e-6 && fabs(p[1] - q[1]) <= 1e-6 && fabs(p[2] - q[2]) <= 1e-6;
}
/* the neutral function of t's kind, which pads the shorter list */
static struct css_transform tf_identity(const struct css_transform *t) {
    struct css_transform r = {.xy = {{.kind = LK_LEN}, {.kind = LK_LEN}}, .kind = t->kind};
    if (t->kind == CT_SCALE) r.v[0] = r.v[1] = r.v[2] = 1;
    if (t->kind == CT_ROTATE) { r.v[0] = t->v[0]; r.v[1] = t->v[1]; r.v[2] = t->v[2]; }
    for (int i = 0; i < 4; i++) r.m[i * 5] = 1;
    return r;
}
/* Lists combine function by function when every pair shares a primitive (rotations a
   common axis); matrices and perspective only meet themselves. */
static bool tf_lists_match(const struct css_transform *a, const struct css_transform *b) {
    for (; a || b; a = a ? a->next : NULL, b = b ? b->next : NULL) {
        const struct css_transform *t = a ? a : b;
        if (t->kind == CT_MATRIX || t->kind == CT_PERSPECTIVE) return false;
        if (!a || !b) continue;
        if (a->kind != b->kind) return false;
        if (a->kind == CT_ROTATE && !tf_same_axis(a, b)) return false;
    }
    return true;
}
/* ka·x + kb·y function by function (accumulate: scales add their excess over 1). With x == y,
   ka = 1, kb = 0 it serializes any list exactly, matrices and perspective included. */
static bool tf_pairs(sbuf *b, const struct css_transform *x, const struct css_transform *y, double ka, double kb,
                     int mode, web_doc *d) {
    bool first = true;
    for (; x || y; x = x ? x->next : NULL, y = y ? y->next : NULL) {
        struct css_transform ix, iy;
        const struct css_transform *s = x, *e = y;
        if (!s) { ix = tf_identity(e); s = &ix; }
        if (!e) { iy = tf_identity(s); e = &iy; }
        if (!first) sb_putc(b, ' ');
        first = false;
        double v[4];
        for (int i = 0; i < 4; i++) v[i] = s->v[i] * ka + e->v[i] * kb;
        if (mode == ACCUMULATE && s->kind == CT_SCALE) for (int i = 0; i < 3; i++) v[i] = s->v[i] + e->v[i] - 1;
        switch (s->kind) {
        case CT_TRANSLATE:
            sb_puts(b, v[0] ? "translate3d(" : "translate(");
            if (!anim_len(b, &s->xy[0], &e->xy[0], ka, kb, LF_NEG, d)) return false;
            sb_puts(b, ", ");
            if (!anim_len(b, &s->xy[1], &e->xy[1], ka, kb, LF_NEG, d)) return false;
            if (v[0]) { sb_puts(b, ", "); anim_unit(b, v[0], "px"); }
            sb_putc(b, ')');
            break;
        case CT_SCALE:
            sb_puts(b, v[2] != 1 ? "scale3d(" : "scale(");
            anim_number(b, v[0]); sb_puts(b, ", "); anim_number(b, v[1]);
            if (v[2] != 1) { sb_puts(b, ", "); anim_number(b, v[2]); }
            sb_putc(b, ')');
            break;
        case CT_ROTATE: {
            double axis[3] = {0, 0, 1};
            if (!tf_axis(s->v[3] != 0 || e->v[3] == 0 ? s : e, axis)) { axis[0] = axis[1] = 0; axis[2] = 1; }
            double angle = s->v[3] * ka + e->v[3] * kb;
            if (axis[0] == 0 && axis[1] == 0 && axis[2] > 0) { sb_puts(b, "rotate("); anim_unit(b, angle, "rad)"); }
            else {
                sb_puts(b, "rotate3d(");
                for (int i = 0; i < 3; i++) { anim_number(b, axis[i]); sb_puts(b, ", "); }
                anim_unit(b, angle, "rad)");
            }
            break;
        }
        case CT_SKEW:
            sb_puts(b, "skew("); anim_unit(b, v[0], "rad, "); anim_unit(b, v[1], "rad)");
            break;
        case CT_PERSPECTIVE:
            if (!(v[0] > 0)) { sb_puts(b, "perspective(none)"); break; }
            sb_puts(b, "perspective("); anim_unit(b, v[0], "px)");
            break;
        case CT_MATRIX:
            sb_puts(b, "matrix3d(");
            for (int col = 0; col < 4; col++)
                for (int row = 0; row < 4; row++) {
                    anim_number(b, s->m[row * 4 + col] * ka + e->m[row * 4 + col] * kb);
                    if (col < 3 || row < 3) sb_puts(b, ", ");
                }
            sb_putc(b, ')');
            break;
        default: return false;
        }
    }
    if (first) sb_puts(b, "none");
    return true;
}
struct tf_2d { double tx, ty, sx, sy, angle, m11, m12, m21, m22; };
/* CSS Transforms 1 §10 "Interpolation of matrices", 2D decomposition. Percentages need the
   used box; without one the list does not decompose. */
static bool tf_decompose(const struct css_transform *t, const struct box *box, struct tf_2d *out) {
    float w = 0, h = 0;
    if (box) {
        w = box->w + box->p[1] + box->p[3] + box->b[1] + box->b[3];
        h = box->h + box->p[0] + box->p[2] + box->b[0] + box->b[2];
    } else for (const struct css_transform *x = t; x; x = x->next)
        if (x->kind == CT_TRANSLATE && (len_has_pct(&x->xy[0]) || len_has_pct(&x->xy[1]))) return false;
    double m[16]; css_transform_list_matrix(t, w, h, m);
    double r0x = m[0], r0y = m[4], r1x = m[1], r1y = m[5];
    out->tx = m[3]; out->ty = m[7];
    out->sx = sqrt(r0x * r0x + r0y * r0y); out->sy = sqrt(r1x * r1x + r1y * r1y);
    if (r0x * r1y - r0y * r1x < 0) { if (r0x < r1y) out->sx = -out->sx; else out->sy = -out->sy; }
    if (out->sx) { r0x /= out->sx; r0y /= out->sx; }
    if (out->sy) { r1x /= out->sy; r1y /= out->sy; }
    out->angle = atan2(r0y, r0x);
    if (out->angle) {
        double sn = -r0y, cs = r0x, a = r0x, b = r0y, cc = r1x, dd = r1y;
        r0x = cs * a + sn * cc; r0y = cs * b + sn * dd; r1x = -sn * a + cs * cc; r1y = -sn * b + cs * dd;
    }
    out->m11 = r0x; out->m12 = r0y; out->m21 = r1x; out->m22 = r1y;
    const double *f = &out->tx;
    for (int i = 0; i < 9; i++) if (!isfinite(f[i])) return false;
    return true;
}
static bool tf_matrix_mix(sbuf *b, const struct css_transform *x, const struct css_transform *y, const struct box *box, double t) {
    struct tf_2d A, B;
    if (!tf_decompose(x, box, &A) || !tf_decompose(y, box, &B)) return false;
    if ((A.sx < 0 && B.sy < 0) || (A.sy < 0 && B.sx < 0)) { A.sx = -A.sx; A.sy = -A.sy; A.angle += A.angle < 0 ? M_PI : -M_PI; }
    if (!A.angle) A.angle = 2 * M_PI;
    if (!B.angle) B.angle = 2 * M_PI;
    if (fabs(A.angle - B.angle) > M_PI) { if (A.angle > B.angle) A.angle -= 2 * M_PI; else B.angle -= 2 * M_PI; }
    struct tf_2d R; double *r = &R.tx; const double *a = &A.tx, *q = &B.tx;
    for (int i = 0; i < 9; i++) r[i] = a[i] + (q[i] - a[i]) * t;
    /* rows = diag(sx, sy) · rotate · remainder */
    double cs = cos(R.angle), sn = sin(R.angle);
    double n11 = cs * R.m11 + sn * R.m21, n12 = cs * R.m12 + sn * R.m22, n21 = -sn * R.m11 + cs * R.m21, n22 = -sn * R.m12 + cs * R.m22;
    double values[6] = {n11 * R.sx, n12 * R.sx, n21 * R.sy, n22 * R.sy, R.tx, R.ty};
    sb_puts(b, "matrix(");
    for (int i = 0; i < 6; i++) { anim_number(b, values[i]); if (i < 5) sb_puts(b, ", "); }
    sb_putc(b, ')');
    return true;
}
static bool tf_list(sbuf *b, const struct css_transform *x, const struct css_transform *y, double ka, double kb,
                    int mode, const struct box *box, web_doc *d) {
    if (mode == ADD || (mode == ACCUMULATE && !tf_lists_match(x, y))) {
        /* addition appends the value's functions to the underlying list */
        if (!x) return tf_pairs(b, y, y, 1, 0, MIX, d);
        if (!tf_pairs(b, x, x, 1, 0, MIX, d)) return false;
        if (y) { sb_putc(b, ' '); return tf_pairs(b, y, y, 1, 0, MIX, d); }
        return true;
    }
    if (tf_lists_match(x, y)) return tf_pairs(b, x, y, ka, kb, mode, d);
    return tf_matrix_mix(b, x, y, box, kb);
}
static void tf_rotate_text(sbuf *b, const struct css_transform *t) {
    if (!t) { sb_puts(b, "none"); return; }
    double deg = t->v[3] * 180 / M_PI;
    if (t->v[0] == 0 && t->v[1] == 0 && t->v[2] > 0) { anim_unit(b, deg, "deg"); return; }
    for (int i = 0; i < 3; i++) { anim_number(b, t->v[i]); sb_putc(b, ' '); }
    anim_unit(b, deg, "deg");
}
static void tf_scale_text(sbuf *b, const struct css_transform *t) {
    if (!t) { sb_puts(b, "none"); return; }
    anim_number(b, t->v[0]);
    /* the shortest form that reads back (CSS Transforms 2: scale's y is its x when omitted) */
    if (t->v[1] != t->v[0] || t->v[2] != 1) { sb_putc(b, ' '); anim_number(b, t->v[1]); }
    if (t->v[2] != 1) { sb_putc(b, ' '); anim_number(b, t->v[2]); }
}
static bool tf_equal(const struct css_transform *a, const struct css_transform *b) {
    for (; a && b; a = a->next, b = b->next)
        if (a->kind != b->kind || memcmp(a->v, b->v, sizeof a->v) || memcmp(a->m, b->m, sizeof a->m) ||
            !css_length_equal(&a->xy[0], &b->xy[0]) || !css_length_equal(&a->xy[1], &b->xy[1])) return false;
    return !a && !b;
}

/* -------- shadow and filter lists */
/* one shadow's computed value: its colour first, then the four lengths (and inset) */
static void anim_shadow_lengths(sbuf *b, double x, double y, double blur, double spread, bool inset) {
    anim_unit(b, x, "px "); anim_unit(b, y, "px ");
    anim_unit(b, anim_clamp(blur, 0, INFINITY), "px ");
    anim_unit(b, spread, "px");
    if (inset) sb_puts(b, " inset");
}
static void anim_shadow_text(sbuf *b, const style_t *st, const struct css_box_shadow *s) {
    struct css_color_computed c;
    anim_shadow_color(st, s, &c);
    anim_color_text(b, &c); sb_putc(b, ' ');
    anim_shadow_lengths(b, s->x, s->y, s->blur, s->spread, s->inset);
}
static void anim_shadow_list_text(sbuf *b, const style_t *st) {
    if (!st->box_shadow) { sb_puts(b, "none"); return; }
    for (const struct css_box_shadow *s = st->box_shadow; s; s = s->next) {
        if (s != st->box_shadow) sb_puts(b, ", ");
        anim_shadow_text(b, st, s);
    }
}
/* a pair of shadows component by component (Web Animations 1, A: interpolation, and accumulation
   with each component accumulated); the shorter list pads with transparent shadows of no length */
static bool anim_shadow_item(sbuf *b, const style_t *sa, const struct css_box_shadow *s, const style_t *sv,
                             const struct css_box_shadow *e, double ka, double kb, int mode) {
    struct css_box_shadow zs = {0}, ze = {0};
    if (!s) { zs.inset = e->inset; s = &zs; }
    if (!e) { ze.inset = s->inset; e = &ze; }
    if (s->inset != e->inset) return false;
    struct css_color_computed cs, ce;
    anim_shadow_color(sa, s, &cs);
    anim_shadow_color(sv, e, &ce);
    if (!anim_color(b, sa, &cs, sv, &ce, kb, mode)) return false;
    sb_putc(b, ' ');
    anim_shadow_lengths(b, s->x * ka + e->x * kb, s->y * ka + e->y * kb, s->blur * ka + e->blur * kb,
                        s->spread * ka + e->spread * kb, s->inset);
    return true;
}
static bool anim_shadows(sbuf *b, const style_t *sa, const style_t *sv, double ka, double kb, int mode) {
    const struct css_box_shadow *x = sa->box_shadow, *y = sv->box_shadow;
    if (!x && !y) { sb_puts(b, "none"); return true; }
    bool first = true;
    if (mode == ADD) {
        /* addition is the concatenation of the lists */
        for (const struct css_box_shadow *s = x; s; s = s->next, first = false) {
            if (!first) sb_puts(b, ", ");
            anim_shadow_text(b, sa, s);
        }
        for (const struct css_box_shadow *s = y; s; s = s->next, first = false) {
            if (!first) sb_puts(b, ", ");
            anim_shadow_text(b, sv, s);
        }
        return true;
    }
    for (; x || y; x = x ? x->next : NULL, y = y ? y->next : NULL, first = false) {
        if (!first) sb_puts(b, ", ");
        if (!anim_shadow_item(b, sa, x, sv, y, ka, kb, mode)) return false;
    }
    return true;
}
static double filter_amount(const struct css_filter_step *s) { return s->percentage ? s->value / 100 : s->value; }
static bool anim_filters(sbuf *b, const struct css_filter *x, const struct css_filter *y, double ka, double kb, int mode) {
    size_t nx = x ? x->count : 0, ny = y ? y->count : 0;
    if (!nx && !ny) { sb_puts(b, "none"); return true; }
    if (mode == ADD) {
        for (size_t i = 0; i < nx + ny; i++) {
            const struct css_filter_step *s = i < nx ? &x->steps[i] : &y->steps[i - nx];
            if (i) sb_putc(b, ' ');
            sb_puts(b, "opacity("); anim_number(b, filter_amount(s)); sb_putc(b, ')');
        }
        return true;
    }
    /* the shorter list pads with each function's initial value, opacity(1) */
    size_t n = nx > ny ? nx : ny;
    for (size_t i = 0; i < n; i++) {
        double a = i < nx ? filter_amount(&x->steps[i]) : 1, c = i < ny ? filter_amount(&y->steps[i]) : 1;
        double v = mode == ACCUMULATE ? a + c - 1 : a * ka + c * kb;
        if (i) sb_putc(b, ' ');
        sb_puts(b, "opacity("); anim_number(b, anim_clamp(v, 0, INFINITY)); sb_putc(b, ')');
    }
    return true;
}

/* -------- serialization of computed values */
static const char *display_keyword(const style_t *st) {
    static const char *const names[] = {"none", "inline", "block", "list-item", "inline-block", "table",
        "inline-table", "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell",
        "table-column", "table-column-group", "table-caption", "flex", "inline-flex", "grid", "inline-grid",
        "contents", "flow-root"};
    if (st->legacy_box) return st->display == D_INLINE_BLOCK || st->display == D_INLINE_FLEX ? "-webkit-inline-box" : "-webkit-box";
    return st->display < sizeof names / sizeof *names ? names[st->display] : NULL;
}
static bool anim_pair_text(sbuf *b, const len_t *l, web_doc *d) {
    return css_length_serialize(&l[0], b, d) && length_append(b, " ", 1) && css_length_serialize(&l[1], b, d);
}
static bool same_text(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
bool css_computed_value(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d) {
    if (!st || !p) return false;
    if (p->type == PT_SHORT) { /* the shorthands of one group's longhands */
        size_t begin = out->n;
        if (group_shorthand_text(st, p, out, d)) return true;
        out->n = begin;
        if (out->p && begin < out->cap) out->p[begin] = 0;
        return false;
    }
    const char *field = field_of(st, p);
    size_t begin = out->n;
    char text[64];
    bool ok = true;
    switch (p->type) {
    case PT_KW: case PT_OVERFLOW: case PT_VALIGN: {
        if (p->type == PT_VALIGN && st->vertical_align == VA_LEN) { anim_unit(out, st->vertical_align_px, "px"); break; }
        uint8_t v = *(const uint8_t *)field;
        const struct kw *k = p->kws;
        while (k && k->name && k->v != v) k++;
        if (!k || !k->name) return false;
        sb_puts(out, k->name);
        break;
    }
    case PT_DISPLAY: {
        const char *name = display_keyword(st);
        if (!name) return false;
        sb_puts(out, name);
        break;
    }
    case PT_LEN: case PT_SVG_WIDTH: ok = css_length_serialize((const len_t *)field, out, d); break;
    case PT_PX: anim_unit(out, *(const float *)field, "px"); break;
    case PT_FONT_SIZE: anim_unit(out, st->font_size, "px"); break;
    case PT_NUM: case PT_OPACITY: anim_number(out, *(const float *)field); break;
    case PT_INT: snprintf(text, sizeof text, "%d", *(const int *)field); sb_puts(out, text); break;
    case PT_ZINDEX:
        if (st->z_auto) sb_puts(out, "auto");
        else { snprintf(text, sizeof text, "%d", st->z_index); sb_puts(out, text); }
        break;
    case PT_FONT_WEIGHT: snprintf(text, sizeof text, "%u", (unsigned)st->font_weight); sb_puts(out, text); break;
    case PT_LINE_HEIGHT:
        if (st->line_height.kind == LK_NORMAL) sb_puts(out, "normal");
        else if (st->line_height.kind == LK_NUMBER) anim_number(out, st->line_height.px);
        else anim_unit(out, st->line_height.px, "px");
        break;
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT && ((const struct svg_paint *)field)->kind != SVG_PAINT_COLOR) {
            if (((const struct svg_paint *)field)->kind != SVG_PAINT_NONE) return false;
            sb_puts(out, "none");
            break;
        }
        struct css_color_computed c;
        css_style_color(st, color_slot(p), &c);
        ok = anim_color_text(out, &c);
        break;
    }
    case PT_FONT_FAMILY: if (!st->font_names) return false; sb_puts(out, st->font_names); break;
    case PT_CONTAINER_NAMES: sb_puts(out, st->container_names ? st->container_names : "none"); break;
    case PT_LINE_CLAMP:
        if (!st->line_clamp) sb_puts(out, "none");
        else { snprintf(text, sizeof text, "%u", (unsigned)st->line_clamp); sb_puts(out, text); }
        break;
    case PT_TEXT_DECO:
        if (!st->text_decoration) sb_puts(out, "none");
        else {
            bool first = true;
            if (st->text_decoration & TD_UNDERLINE) { sb_puts(out, "underline"); first = false; }
            if (st->text_decoration & TD_OVERLINE) { if (!first) sb_putc(out, ' '); sb_puts(out, "overline"); first = false; }
            if (st->text_decoration & TD_LINE_THROUGH) { if (!first) sb_putc(out, ' '); sb_puts(out, "line-through"); }
        }
        break;
    case PT_BG_IMAGE: {
        if (st->has_grad) return false;
        if (!st->bg_image) { sb_puts(out, "none"); break; }
        url_text(out, st->bg_image);
        break;
    }
    /* the mask layers' lists (css-masking-1 §7): a gradient as written (its computed form has no
       serializer yet) */
    case PT_MASK_IMAGE: {
        const struct css_layer_images *l = (const struct css_layer_images *)field;
        for (uint32_t i = 0; i < l->n; i++) {
            if (i) sb_puts(out, ", ");
            if (!l->v[i]) sb_puts(out, "none");
            else if (l->v[i]->kind == CSS_IMAGE_URL) url_text(out, l->v[i]->url ? l->v[i]->url : "");
            else image_text(l->v[i], out);
        }
        break;
    }
    case PT_MASK_REPEAT: {
        const struct css_bg_keywords *l = (const struct css_bg_keywords *)field;
        for (uint32_t i = 0; i < l->n; i++) { if (i) sb_puts(out, ", "); repeat_style_text(l->v[i], out); }
        break;
    }
    case PT_MASK_POSITION: {
        const struct css_bg_offsets *l = (const struct css_bg_offsets *)field;
        uint32_t count = l[0].n > l[1].n ? l[0].n : l[1].n;
        for (uint32_t i = 0; ok && i < count; i++) {
            if (i) sb_puts(out, ", ");
            ok = bg_offset_text(0, &l[0].v[i % l[0].n], out, d);
            sb_putc(out, ' ');
            ok = ok && bg_offset_text(1, &l[1].v[i % l[1].n], out, d);
        }
        break;
    }
    case PT_MASK_SIZE: {
        const struct css_layer_sizes *l = (const struct css_layer_sizes *)field;
        for (uint32_t i = 0; ok && i < l->n; i++) { if (i) sb_puts(out, ", "); ok = layer_size_text(&l->v[i], out, d); }
        break;
    }
    case PT_GLINE: {
        const struct gline *g = (const struct gline *)field;
        if (g->kind == GL_AUTO) sb_puts(out, "auto");
        else if (g->kind == GL_SPAN) { snprintf(text, sizeof text, "span %d", g->n); sb_puts(out, text); }
        else if (g->kind == GL_NAME) {
            sb_puts(out, g->name);
            if (g->n != 1) { snprintf(text, sizeof text, " %d", g->n); sb_puts(out, text); }
        } else { snprintf(text, sizeof text, "%d", g->n); sb_puts(out, text); }
        break;
    }
    case PT_FILTER: ok = css_filter_serialize(st->filter, out, d); break;
    case PT_BOX_SHADOW: anim_shadow_list_text(out, st); break;
    case PT_RADIUS: ok = anim_pair_text(out, ((const struct css_corner_radius *)field)->axis, d); break;
    case PT_BG_POS: case PT_TRANSFORM_ORIGIN: ok = anim_pair_text(out, (const len_t *)field, d); break;
    case PT_BG_SIZE: {
        uint8_t kind = st->bg_size_kind;
        if (kind == BSZ_COVER) sb_puts(out, "cover");
        else if (kind == BSZ_CONTAIN) sb_puts(out, "contain");
        else if (kind == BSZ_AUTO) sb_puts(out, "auto");
        else ok = anim_pair_text(out, st->bg_size, d);
        break;
    }
    case PT_TRANSFORM: ok = tf_pairs(out, st->transform, st->transform, 1, 0, MIX, d); break;
    case PT_TRANSLATE:
        if (!st->translate.active) sb_puts(out, "none");
        else ok = anim_pair_text(out, st->translate.xy, d);
        break;
    case PT_ROTATE: tf_rotate_text(out, st->rotate); break;
    case PT_SCALE: tf_scale_text(out, st->scale); break;
    case PT_SHAPE: ok = css_shape_serialize(p->flags, *(const struct css_shape *const *)field, out, d); break;
    case PT_OFFSET: ok = css_offset_serialize(p->flags, field, out, d); break;
    case PT_ANCHOR: ok = anchor_value_text(p->flags, field, out); break;
    case PT_CLIP: ok = css_clip_rect_serialize(&st->clip, out); break;
    case PT_MOTION_LIST: {
        int list = p->flags;
        uint32_t count = css_motion_count(st, list);
        for (uint32_t i = 0; i < count; i++) {
            union css_motion_item item = css_motion_item(st, list, i);
            if (i) sb_puts(out, ", ");
            switch (list) {
            case ML_NAME: case ML_TPROPERTY:
                if (!item.s) sb_puts(out, "none");
                else if (list == ML_TPROPERTY || (motion_name(item.s, strlen(item.s)) && !ident_is(item.s, strlen(item.s), "none")))
                    sb_puts(out, item.s);
                else {
                    sb_putc(out, '"');
                    for (const char *c = item.s; *c; c++) { if (*c == '"' || *c == '\\') sb_putc(out, '\\'); sb_putc(out, *c); }
                    sb_putc(out, '"');
                }
                break;
            case ML_DURATION: case ML_DELAY: case ML_TDURATION: case ML_TDELAY:
                if (isnan(item.f)) sb_puts(out, "auto");
                else anim_unit(out, item.f / 1000, "s");
                break;
            case ML_EASING: case ML_TEASING: css_easing_serialize(&item.e, out); break;
            case ML_ITERATIONS:
                if (isinf(item.f)) sb_puts(out, "infinite");
                else anim_number(out, item.f);
                break;
            default: {
                const struct kw *k = p->kws;
                while (k && k->name && k->v != item.k) k++;
                if (!k || !k->name) return false;
                sb_puts(out, k->name);
            }
            }
        }
        break;
    }
    /* gap decorations: currentcolor stays itself, for the cascade to resolve against the element */
    case PT_RULE_LIST: rule_list_text(st, p->flags, (const struct css_rule_list *)field, false, out); break;
    case PT_RULE_INSET: ok = rule_inset_text((const struct css_rule_inset *)field, out, d); break;
    case PT_CONTENT: case PT_LIST_TYPE: case PT_LIST_IMAGE: case PT_QUOTES: case PT_COUNTER:
        ok = list_value_text(st, p, out);
        break;
    /* the background and border-image groups */
    case PT_BG_LIST: {
        const struct css_bg_keywords *l = (const struct css_bg_keywords *)field;
        for (uint32_t i = 0; ok && i < l->n; i++) {
            const char *name = kw_name(p->kws, l->v[i]);
            if (i) sb_puts(out, ", ");
            if (name) sb_puts(out, name);
            else ok = false;
        }
        break;
    }
    case PT_BG_OFFSET: ok = bg_offsets_text(p->flags, (const struct css_bg_offsets *)field, out, d); break;
    case PT_BI_SOURCE: ok = bi_source_text((const struct css_bi_source *)field, out); break;
    case PT_BI_SLICE: ok = bi_slice_text((const struct css_bi_slice *)field, out, d); break;
    case PT_BI_SIDES: ok = bi_sides_text((const struct css_bi_side *)field, false, out, d); break;
    case PT_BI_REPEAT: bi_repeat_text((const uint8_t *)field, out); break;

    default: return false;
    }
    if (!ok) { out->n = begin; if (out->p && begin < out->cap) out->p[begin] = 0; }
    return ok;
}

bool css_anim_equal(const struct propdef *p, const style_t *a, const style_t *b) {
    if (!p || p->type == PT_SHORT) return true;
    const char *fa = field_of(a, p), *fb = field_of(b, p);
    switch (p->type) {
    case PT_LEN: case PT_SVG_WIDTH: return css_length_equal((const len_t *)fa, (const len_t *)fb);
    case PT_RADIUS: case PT_BG_POS: case PT_TRANSFORM_ORIGIN: {
        const len_t *x = p->type == PT_RADIUS ? ((const struct css_corner_radius *)fa)->axis : (const len_t *)fa;
        const len_t *y = p->type == PT_RADIUS ? ((const struct css_corner_radius *)fb)->axis : (const len_t *)fb;
        return css_length_equal(&x[0], &y[0]) && css_length_equal(&x[1], &y[1]);
    }
    case PT_BG_SIZE: return a->bg_size_kind == b->bg_size_kind && css_length_equal(&a->bg_size[0], &b->bg_size[0]) &&
        css_length_equal(&a->bg_size[1], &b->bg_size[1]);
    case PT_MASK_SIZE: return layer_sizes_equal((const struct css_layer_sizes *)fa, (const struct css_layer_sizes *)fb);
    case PT_MASK_IMAGE: return layer_images_equal((const struct css_layer_images *)fa, (const struct css_layer_images *)fb);
    case PT_MASK_REPEAT: return bg_keywords_equal((const struct css_bg_keywords *)fa, (const struct css_bg_keywords *)fb);
    case PT_MASK_POSITION:
        return bg_offsets_equal(&((const struct css_bg_offsets *)fa)[0], &((const struct css_bg_offsets *)fb)[0]) &&
               bg_offsets_equal(&((const struct css_bg_offsets *)fa)[1], &((const struct css_bg_offsets *)fb)[1]);
    case PT_TRANSFORM: case PT_ROTATE: case PT_SCALE:
        return tf_equal(*(const struct css_transform *const *)fa, *(const struct css_transform *const *)fb);
    case PT_TRANSLATE: return a->translate.active == b->translate.active && css_length_equal(&a->translate.xy[0], &b->translate.xy[0]) &&
        css_length_equal(&a->translate.xy[1], &b->translate.xy[1]);
    case PT_BOX_SHADOW: {
        const struct css_box_shadow *x = a->box_shadow, *y = b->box_shadow;
        for (; x && y; x = x->next, y = y->next) {
            if (x->x != y->x || x->y != y->y || x->blur != y->blur || x->spread != y->spread || x->inset != y->inset)
                return false;
            struct css_color_computed u, v;
            anim_shadow_color(a, x, &u);
            anim_shadow_color(b, y, &v);
            if (!css_color_computed_equal(&u, &v)) return false;
        }
        return !x && !y;
    }
    case PT_FILTER: {
        const struct css_filter *x = a->filter, *y = b->filter;
        if (!x || !y) return x == y;
        return x->count == y->count && !memcmp(x->steps, y->steps, x->count * sizeof *x->steps);
    }
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT) {
            const struct svg_paint *x = (const struct svg_paint *)fa, *y = (const struct svg_paint *)fb;
            if (x->kind != y->kind || !same_text(x->ref, y->ref)) return false;
            if (x->kind != SVG_PAINT_COLOR) return true;
        }
        struct css_color_computed x, y;
        css_style_color(a, color_slot(p), &x);
        css_style_color(b, color_slot(p), &y);
        return css_color_computed_equal(&x, &y);
    }
    case PT_VALIGN: return a->vertical_align == b->vertical_align && (a->vertical_align != VA_LEN || a->vertical_align_px == b->vertical_align_px);
    case PT_ZINDEX: return a->z_auto == b->z_auto && (a->z_auto || a->z_index == b->z_index);
    case PT_DISPLAY: return a->display == b->display && a->legacy_box == b->legacy_box;
    case PT_FONT_FAMILY: return a->font_family == b->font_family && same_text(a->font_names, b->font_names);
    case PT_CONTAINER_NAMES: return same_text(a->container_names, b->container_names);
    case PT_CONTENT: return css_content_equal(a->content, b->content);
    case PT_LIST_TYPE: {
        const struct css_list *x = css_style_list(a), *y = css_style_list(b);
        return a->list_style == b->list_style && same_text(x->type_name, y->type_name) &&
            symbols_equal(x->type_symbols, y->type_symbols);
    }
    case PT_LIST_IMAGE: return image_equal(css_style_list(a)->image, css_style_list(b)->image);
    case PT_QUOTES: return quotes_equal(css_style_list(a)->quotes, css_style_list(b)->quotes);
    case PT_COUNTER: return counter_ops_equal(css_style_counters(a)->ops[p->flags], css_style_counters(b)->ops[p->flags]);
    case PT_BG_IMAGE: return same_text(a->bg_image, b->bg_image) && a->has_grad == b->has_grad && a->gradient == b->gradient;
    case PT_GLINE: {
        const struct gline *x = (const struct gline *)fa, *y = (const struct gline *)fb;
        return x->kind == y->kind && x->n == y->n && same_text(x->name, y->name);
    }
    case PT_SHAPE: return css_shape_equal(*(const struct css_shape *const *)fa, *(const struct css_shape *const *)fb);
    case PT_RULE_LIST: return rule_list_equal(p->flags, (const struct css_rule_list *)fa, (const struct css_rule_list *)fb);
    case PT_RULE_INSET: return rule_inset_equal((const struct css_rule_inset *)fa, (const struct css_rule_inset *)fb);
    case PT_OFFSET:
        if (p->flags == CSS_OFFSET_ROTATE) return !memcmp(fa, fb, sizeof(struct css_offset_rotate));
        return css_length_equal(&((const len_t *)fa)[0], &((const len_t *)fb)[0]) &&
            css_length_equal(&((const len_t *)fa)[1], &((const len_t *)fb)[1]);
    case PT_GTEMPLATE: case PT_GAREAS:
        return *(const void *const *)fa == *(const void *const *)fb;
    case PT_MOTION_LIST: return css_motion_equal(&a->motion, &b->motion);
    case PT_ANCHOR: return anchor_value_equal(p->flags, fa, fb);
    case PT_BG_LIST: return bg_keywords_equal((const struct css_bg_keywords *)fa, (const struct css_bg_keywords *)fb);
    case PT_BG_OFFSET: return bg_offsets_equal((const struct css_bg_offsets *)fa, (const struct css_bg_offsets *)fb);
    case PT_BI_SOURCE: return bi_source_equal((const struct css_bi_source *)fa, (const struct css_bi_source *)fb);
    case PT_BI_SLICE: {
        const struct css_bi_slice *x = (const struct css_bi_slice *)fa, *y = (const struct css_bi_slice *)fb;
        return x->fill == y->fill && bi_sides_equal(x->side, y->side);
    }
    case PT_BI_SIDES: return bi_sides_equal((const struct css_bi_side *)fa, (const struct css_bi_side *)fb);
    }
    return !memcmp(fa, fb, p->size);
}

bool css_anim_parse(const struct css_anim_ctx *c, const struct propdef *p, const char *v, size_t n, bool computed,
                    style_t *out, uint8_t *set) {
    if (!p || !v) return false;
    *out = *c->base;
    uint8_t local[NPROPS];
    if (!set) set = local;
    memset(set, 0, NPROPS);
    const style_t *parent = c->parent;
    struct cx cx = {.s = out, .parent = parent, .node = c->node, .em = parent ? parent->font_size : 16, .rem = c->rem,
                    .vw = c->vw, .vh = c->vh, .a = c->a, .set = set, .font_pass = true, .computed_text = computed};
    /* font-size first, against the parent's, as the cascade does; then the rest against it */
    if (css_prop_is_early(p) && !css_apply(p, v, n, &cx)) return false;
    cx.font_pass = false;
    cx.em = out->font_size;
    return css_apply(p, v, n, &cx) && !cx.invalid;
}

bool css_anim_discrete_second(const struct propdef *p, const style_t *a, const style_t *b, double t) {
    int type = css_prop_animation(p);
    if (type == CSS_ANIM_DISPLAY || type == CSS_ANIM_VISIBILITY) {
        if (t <= 0) return false;
        if (t >= 1) return true;
        bool hides_a, hides_b;
        if (type == CSS_ANIM_VISIBILITY) hides_a = a->visibility != 0, hides_b = b->visibility != 0;
        else if (p->type == PT_DISPLAY) hides_a = a->display == D_NONE, hides_b = b->display == D_NONE;
        else hides_a = a->content_visibility == CV_HIDDEN, hides_b = b->content_visibility == CV_HIDDEN;
        /* inside the interval the showing value wins */
        if (hides_a != hides_b) return hides_a;
    }
    return t >= .5;
}

/* -------- gap decoration lists (css-gaps-1 §4.7) */
/* a walk through items [from, to) of a list with their integer repeaters written out, round and
   round (an auto repeater's values once a round) */
struct rule_walk {
    const struct css_rule_list *l;
    uint32_t from, to, item, j;
    uint64_t round;
};
static void rule_walk_start(struct rule_walk *w, const struct css_rule_list *l, uint32_t from, uint32_t to) {
    *w = (struct rule_walk){l, from, to, from, 0, 0};
}
static const union css_rule_value *rule_walk_next(struct rule_walk *w) {
    const struct css_rule_item *it = &w->l->items[w->item];
    const union css_rule_value *v = &w->l->values[it->first + w->j];
    if (++w->j < it->count) return v;
    w->j = 0;
    uint64_t rounds = it->repeat && it->repeat != CSS_RULE_AUTO ? it->repeat : 1;
    if (++w->round < rounds) return v;
    w->round = 0;
    if (++w->item == w->to) w->item = w->from;
    return v;
}
static uint64_t rule_span(const struct css_rule_list *l, uint32_t from, uint32_t to) {
    uint64_t n = 0;
    for (uint32_t i = from; i < to; i++) {
        uint64_t span = rule_item_span(&l->items[i]);
        if (n > UINT64_MAX - span) return UINT64_MAX;
        n += span;
    }
    return n;
}
static uint32_t rule_auto_item(const struct css_rule_list *l) {
    for (uint32_t i = 0; i < l->nitems; i++) if (l->items[i].repeat == CSS_RULE_AUTO) return i;
    return l->nitems;
}
/* the repeatable list's length (Web Animations 1 §5.3): the lowest common multiple; 0 past 64 bits */
static uint64_t rule_lcm(uint64_t a, uint64_t b) {
    if (!a || !b) return 0;
    uint64_t x = a, y = b;
    while (y) { uint64_t r = x % y; x = y; y = r; }
    a /= x;
    return a > UINT64_MAX / b ? 0 : a * b;
}
static bool rule_value_combine(int kind, const style_t *a, const union css_rule_value *x, const style_t *b,
                               const union css_rule_value *y, double ka, double kb, int mode, sbuf *out) {
    if (kind == CSS_GAP_WIDTH) { anim_unit(out, anim_clamp(x->width * ka + y->width * kb, 0, INFINITY), "px"); return true; }
    struct css_color_computed u, v;
    css_color_held(x->color.argb, x->color.exact, a->color, &u);
    css_color_held(y->color.argb, y->color.exact, b->color, &v);
    return anim_color(out, a, &u, b, &v, kb, mode);
}
/* n values of the two walks against each other, each after a comma unless *first; the text grows
   only while memory allows, and stops when the document is cancelled */
static bool rule_walks_combine(const struct css_anim_ctx *c, int kind, const style_t *a, struct rule_walk *wx,
                               const style_t *b, struct rule_walk *wy, uint64_t n, double ka, double kb, int mode,
                               bool *first, sbuf *out) {
    sbuf one = {0};
    bool ok = true;
    for (uint64_t k = 0; k < n && ok; k++) {
        if (!(k & 1023) && c->d && __atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE)) { ok = false; break; }
        one.n = 0;
        ok = rule_value_combine(kind, a, rule_walk_next(wx), b, rule_walk_next(wy), ka, kb, mode, &one) &&
             (*first || length_append(out, ", ", 2)) && length_append(out, one.p, one.n);
        *first = false;
    }
    sb_free(&one);
    return ok;
}
/* §4.7: integer repeaters written out, then the repeatable list's items against each other; with
   an auto repeater in both, the leading and trailing values pairwise (their lengths must match)
   and the auto repeaters' values as a repeatable list. Otherwise false: discrete. */
static bool rule_lists_combine(const struct css_anim_ctx *c, int kind, const style_t *a, const struct css_rule_list *x,
                               const style_t *b, const struct css_rule_list *y, double ka, double kb, int mode, sbuf *out) {
    uint32_t ax = rule_auto_item(x), ay = rule_auto_item(y);
    bool repeat_auto = ax < x->nitems;
    if (repeat_auto != (ay < y->nitems)) return false;
    struct rule_walk wx, wy;
    bool first = true;
    if (!repeat_auto) {
        uint64_t n = rule_lcm(rule_span(x, 0, x->nitems), rule_span(y, 0, y->nitems));
        if (!n) return false;
        rule_walk_start(&wx, x, 0, x->nitems);
        rule_walk_start(&wy, y, 0, y->nitems);
        return rule_walks_combine(c, kind, a, &wx, b, &wy, n, ka, kb, mode, &first, out);
    }
    uint64_t leading = rule_span(x, 0, ax), trailing = rule_span(x, ax + 1, x->nitems);
    if (leading != rule_span(y, 0, ay) || trailing != rule_span(y, ay + 1, y->nitems)) return false;
    if (leading) {
        rule_walk_start(&wx, x, 0, ax);
        rule_walk_start(&wy, y, 0, ay);
        if (!rule_walks_combine(c, kind, a, &wx, b, &wy, leading, ka, kb, mode, &first, out)) return false;
    }
    uint64_t n = rule_lcm(x->items[ax].count, y->items[ay].count);
    bool inner = true;
    if (!n || !length_append(out, first ? "repeat(auto, " : ", repeat(auto, ", first ? 13 : 15)) return false;
    rule_walk_start(&wx, x, ax, ax + 1);
    rule_walk_start(&wy, y, ay, ay + 1);
    if (!rule_walks_combine(c, kind, a, &wx, b, &wy, n, ka, kb, mode, &inner, out) || !length_append(out, ")", 1)) return false;
    first = false;
    if (!trailing) return true;
    rule_walk_start(&wx, x, ax + 1, x->nitems);
    rule_walk_start(&wy, y, ay + 1, y->nitems);
    return rule_walks_combine(c, kind, a, &wx, b, &wy, trailing, ka, kb, mode, &first, out);
}

/* -------- the background and border-image groups (css-backgrounds-3 §3.6, §6.2-§6.4; css-masking-1
   §8.2-§8.4; Web Animations 1 §5.3) */
/* one offset of a position's axis: a <length-percentage> from the start side */
static bool bg_offset_mix(const struct css_anim_ctx *c, const struct css_bg_offset *u, const struct css_bg_offset *v,
                          double ka, double kb, sbuf *out) {
    len_t lu = bg_offset_from_start(u, c->a), lv = bg_offset_from_start(v, c->a);
    if (lu.kind == LK_LEN && lv.kind == LK_LEN) { /* a percentage stays one at zero */
        double px = lu.px * ka + lv.px * kb, pct = lu.pct * ka + lv.pct * kb;
        if (fabs(px) < 1e-9 && (fabs(pct) >= 1e-9 || (u->percent && ka != 0) || (v->percent && kb != 0))) {
            anim_unit(out, pct, "%");
            return true;
        }
    }
    return anim_len(out, &lu, &lv, ka, kb, LF_NEG, c->d);
}
static bool anim_cancelled(const struct css_anim_ctx *c, uint64_t i) {
    return !(i & 1023) && c->d && __atomic_load_n(&c->d->native_cancelled, __ATOMIC_ACQUIRE);
}
/* background-position-x, -y: a repeatable list of <length-percentage>s from the start side */
static bool bg_offsets_combine(const struct css_anim_ctx *c, const struct css_bg_offsets *x, const struct css_bg_offsets *y,
                               double ka, double kb, sbuf *out) {
    uint64_t n = rule_lcm(x->n, y->n);
    if (!n) return false;
    for (uint64_t i = 0; i < n; i++) {
        if (anim_cancelled(c, i) || (i && !length_append(out, ", ", 2))) return false;
        if (!bg_offset_mix(c, &x->v[i % x->n], &y->v[i % y->n], ka, kb, out)) return false;
    }
    return true;
}
/* mask-position (css-masking-1 §7.4): a repeatable list of positions, each axis as above */
static bool mask_positions_combine(const struct css_anim_ctx *c, const struct css_bg_offsets *x, const struct css_bg_offsets *y,
                                   double ka, double kb, sbuf *out) {
    uint64_t n = rule_lcm(x[0].n, y[0].n);
    if (!n || x[1].n != x[0].n || y[1].n != y[0].n) return false;
    for (uint64_t i = 0; i < n; i++) {
        if (anim_cancelled(c, i) || (i && !length_append(out, ", ", 2))) return false;
        for (int axis = 0; axis < 2; axis++)
            if ((axis && !length_append(out, " ", 1)) ||
                !bg_offset_mix(c, &x[axis].v[i % x[axis].n], &y[axis].v[i % y[axis].n], ka, kb, out))
                return false;
    }
    return true;
}
/* mask-size (§7.7): a repeatable list of <bg-size>s; a width or height auto only with auto, cover and
   contain only with themselves; otherwise the value is discrete */
static bool mask_sizes_combine(const struct css_anim_ctx *c, const struct css_layer_sizes *x, const struct css_layer_sizes *y,
                               double ka, double kb, sbuf *out) {
    uint64_t n = rule_lcm(x->n, y->n);
    if (!n) return false;
    for (uint64_t i = 0; i < n; i++) {
        if (anim_cancelled(c, i) || (i && !length_append(out, ", ", 2))) return false;
        const struct css_layer_size *u = &x->v[i % x->n], *v = &y->v[i % y->n];
        bool lu = u->kind == BSZ_LEN || u->kind == BSZ_AUTO, lv = v->kind == BSZ_LEN || v->kind == BSZ_AUTO;
        if (!lu || !lv) {
            if (u->kind != v->kind) return false;
            if (!layer_size_text(u, out, c->d)) return false;
            continue;
        }
        for (int axis = 0; axis < 2; axis++) {
            if (axis && !length_append(out, " ", 1)) return false;
            bool au = u->v[axis].kind == LK_AUTO, av = v->v[axis].kind == LK_AUTO;
            if (au != av) return false;
            if (au) sb_puts(out, "auto");
            else if (!anim_len(out, &u->v[axis], &v->v[axis], ka, kb, 0, c->d)) return false;
        }
    }
    return true;
}
/* border-image-slice, -width, -outset (and mask-border's): each side by computed value when the two
   are the same kind (number, length-percentage, auto) on every side, else the value is discrete;
   none is negative. slice: its length kind is a percentage */
static bool bi_sides_combine(const struct css_bi_side *x, const struct css_bi_side *y, double ka, double kb, bool slice,
                             sbuf *out, web_doc *d) {
    for (int i = 0; i < 4; i++) if (x[i].kind != y[i].kind) return false;
    for (int i = 0; i < 4; i++) {
        if (i && !length_append(out, " ", 1)) return false;
        if (x[i].kind == CSS_BI_AUTO) sb_puts(out, "auto");
        else if (x[i].kind == CSS_BI_NUMBER) anim_number(out, anim_clamp(x[i].number * ka + y[i].number * kb, 0, INFINITY));
        else if (slice) anim_unit(out, anim_clamp(x[i].length.pct * ka + y[i].length.pct * kb, 0, INFINITY), "%");
        else if (!anim_len(out, &x[i].length, &y[i].length, ka, kb, 0, d)) return false;
    }
    return true;
}
/* fill: equal for interpolation; addition takes the added value's */
static bool bi_slice_combine(const struct css_bi_slice *x, const struct css_bi_slice *y, double ka, double kb, int mode,
                             sbuf *out, web_doc *d) {
    if (mode == MIX && x->fill != y->fill) return false;
    if (!bi_sides_combine(x->side, y->side, ka, kb, true, out, d)) return false;
    if (y->fill) sb_puts(out, " fill");
    return true;
}

/* ka·a + kb·b for each value type (mode MIX), or b added / accumulated onto a */
static bool anim_combine(const struct css_anim_ctx *c, const struct propdef *p, const style_t *a, const style_t *b,
                         double ka, double kb, int mode, sbuf *out) {
    const char *fa = field_of(a, p), *fb = field_of(b, p);
    web_doc *d = c->d;
    switch (p->type) {
    case PT_LEN: case PT_SVG_WIDTH:
        return anim_len(out, (const len_t *)fa, (const len_t *)fb, ka, kb, p->type == PT_SVG_WIDTH ? 0 : p->flags, d);
    case PT_PX: {
        double v = *(const float *)fa * ka + *(const float *)fb * kb;
        anim_unit(out, p->flags & LF_NEG ? v : anim_clamp(v, 0, INFINITY), "px");
        return true;
    }
    case PT_FONT_SIZE: anim_unit(out, anim_clamp(a->font_size * ka + b->font_size * kb, 0, INFINITY), "px"); return true;
    case PT_NUM: anim_number(out, anim_clamp(*(const float *)fa * ka + *(const float *)fb * kb, 0, INFINITY)); return true;
    case PT_OPACITY: anim_number(out, anim_clamp(*(const float *)fa * ka + *(const float *)fb * kb, 0, 1)); return true;
    case PT_FONT_WEIGHT: anim_number(out, anim_clamp(a->font_weight * ka + b->font_weight * kb, 1, 1000)); return true;
    case PT_INT: case PT_ZINDEX: {
        if (p->type == PT_ZINDEX && (a->z_auto || b->z_auto)) return false;
        /* <integer>: as a real number, then to the nearest integer, halves towards +infinity */
        char text[32]; snprintf(text, sizeof text, "%.0f", floor(*(const int *)fa * ka + *(const int *)fb * kb + .5));
        sb_puts(out, text);
        return true;
    }
    case PT_LINE_HEIGHT:
        if (a->line_height.kind != b->line_height.kind || a->line_height.kind == LK_NORMAL) return false;
        if (a->line_height.kind == LK_NUMBER) anim_number(out, anim_clamp(a->line_height.px * ka + b->line_height.px * kb, 0, INFINITY));
        else anim_unit(out, anim_clamp(a->line_height.px * ka + b->line_height.px * kb, 0, INFINITY), "px");
        return true;
    case PT_VALIGN:
        if (a->vertical_align != VA_LEN || b->vertical_align != VA_LEN) return false;
        anim_unit(out, a->vertical_align_px * ka + b->vertical_align_px * kb, "px");
        return true;
    case PT_COLOR: case PT_SVG_PAINT: {
        if (p->type == PT_SVG_PAINT && (((const struct svg_paint *)fa)->kind != SVG_PAINT_COLOR ||
                                        ((const struct svg_paint *)fb)->kind != SVG_PAINT_COLOR)) return false;
        struct css_color_computed x, y;
        css_style_color(a, color_slot(p), &x);
        css_style_color(b, color_slot(p), &y);
        return anim_color(out, a, &x, b, &y, kb, mode);
    }
    case PT_RADIUS: {
        const len_t *x = ((const struct css_corner_radius *)fa)->axis, *y = ((const struct css_corner_radius *)fb)->axis;
        return anim_len(out, &x[0], &y[0], ka, kb, 0, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, 0, d);
    }
    case PT_BG_POS: case PT_TRANSFORM_ORIGIN: {
        const len_t *x = (const len_t *)fa, *y = (const len_t *)fb;
        return anim_len(out, &x[0], &y[0], ka, kb, LF_NEG, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, LF_NEG, d);
    }
    case PT_BG_SIZE: {
        uint8_t kx = a->bg_size_kind, ky = b->bg_size_kind;
        if (kx != BSZ_LEN || ky != BSZ_LEN) {
            if (kx != ky) return false;
            return css_computed_value(a, p, out, d);
        }
        const len_t *x = a->bg_size, *y = b->bg_size;
        return anim_len(out, &x[0], &y[0], ka, kb, 0, d) && length_append(out, " ", 1) && anim_len(out, &x[1], &y[1], ka, kb, 0, d);
    }
    case PT_MASK_SIZE:
        return mask_sizes_combine(c, (const struct css_layer_sizes *)fa, (const struct css_layer_sizes *)fb, ka, kb, out);
    case PT_MASK_POSITION:
        return mask_positions_combine(c, (const struct css_bg_offsets *)fa, (const struct css_bg_offsets *)fb, ka, kb, out);
    case PT_TRANSFORM: return tf_list(out, a->transform, b->transform, ka, kb, mode, c->box, d);
    case PT_TRANSLATE:
        if (!a->translate.active && !b->translate.active) { sb_puts(out, "none"); return true; }
        /* none is translate(0) */
        return anim_len(out, &a->translate.xy[0], &b->translate.xy[0], ka, kb, LF_NEG, d) && length_append(out, " ", 1) &&
            anim_len(out, &a->translate.xy[1], &b->translate.xy[1], ka, kb, LF_NEG, d);
    case PT_ROTATE: {
        const struct css_transform *x = a->rotate, *y = b->rotate;
        if (!x && !y) { sb_puts(out, "none"); return true; }
        struct css_transform ix, iy;
        if (!x) { ix = tf_identity(y); x = &ix; }
        if (!y) { iy = tf_identity(x); y = &iy; }
        if (!tf_same_axis(x, y)) return false;
        struct css_transform r = x->v[3] != 0 || y->v[3] == 0 ? *x : *y;
        r.v[3] = x->v[3] * ka + y->v[3] * kb;
        tf_rotate_text(out, &r);
        return true;
    }
    case PT_SCALE: {
        const struct css_transform *x = a->scale, *y = b->scale;
        if (!x && !y) { sb_puts(out, "none"); return true; }
        struct css_transform ix, iy;
        if (!x) { ix = tf_identity(y); x = &ix; }
        if (!y) { iy = tf_identity(x); y = &iy; }
        struct css_transform r = *x;
        /* CSS Transforms 2 §16: scales multiply when added, add their excess over 1 when accumulated */
        for (int i = 0; i < 3; i++)
            r.v[i] = mode == ADD ? x->v[i] * y->v[i] : mode == ACCUMULATE ? x->v[i] + y->v[i] - 1 : x->v[i] * ka + y->v[i] * kb;
        tf_scale_text(out, &r);
        return true;
    }
    case PT_BOX_SHADOW: return anim_shadows(out, a, b, ka, kb, mode);
    case PT_FILTER: return anim_filters(out, a->filter, b->filter, ka, kb, mode);
    case PT_SHAPE: /* css-shapes-1 §3.4, motion-1 §2.1; accumulation is addition */
        return css_shape_combine(p->flags, *(const struct css_shape *const *)fa, *(const struct css_shape *const *)fb,
                                 kb, mode != MIX, out, d);
    case PT_OFFSET: return css_offset_combine(p->flags, fa, fb, kb, mode != MIX, out, d);
    case PT_RULE_LIST:
        return rule_lists_combine(c, p->flags, a, (const struct css_rule_list *)fa, b, (const struct css_rule_list *)fb, ka,
                                  kb, mode, out);
    case PT_RULE_INSET: { /* <length-percentage> | overlap-join: the keyword only with itself */
        const struct css_rule_inset *x = (const struct css_rule_inset *)fa, *y = (const struct css_rule_inset *)fb;
        if (x->join || y->join) {
            if (mode != MIX || !x->join || !y->join) return false;
            sb_puts(out, "overlap-join");
            return true;
        }
        return anim_len(out, &x->length, &y->length, ka, kb, LF_NEG, d);
    }
    case PT_CLIP:
        if (!a->clip.rect || !b->clip.rect || a->clip.auto_edges != b->clip.auto_edges) return false;
        sb_puts(out, "rect(");
        for (int i = 0; i < 4; i++) {
            if (i) sb_puts(out, ", ");
            if (a->clip.auto_edges >> i & 1) sb_puts(out, "auto");
            else anim_unit(out, a->clip.edge[i] * ka + b->clip.edge[i] * kb, "px");
        }
        sb_putc(out, ')');
        return true;
    case PT_BG_OFFSET:
        return bg_offsets_combine(c, (const struct css_bg_offsets *)fa, (const struct css_bg_offsets *)fb, ka, kb, out);
    case PT_BI_SLICE:
        return bi_slice_combine((const struct css_bi_slice *)fa, (const struct css_bi_slice *)fb, ka, kb, mode, out, d);
    case PT_BI_SIDES:
        return bi_sides_combine((const struct css_bi_side *)fa, (const struct css_bi_side *)fb, ka, kb, false, out, d);
    }
    return false;
}

bool css_anim_interpolate(const struct css_anim_ctx *c, const struct propdef *p, const style_t *a, const style_t *b,
                          double t, sbuf *out) {
    int type = css_prop_animation(p);
    if (type == CSS_ANIM_VISIBILITY)
        return css_computed_value(a->visibility == b->visibility || !css_anim_discrete_second(p, a, b, t) ? a : b, p, out, c->d);
    if (type != CSS_ANIM_VALUE) return false;
    size_t begin = out->n;
    if (anim_combine(c, p, a, b, 1 - t, t, MIX, out)) return true;
    out->n = begin;
    if (out->p && begin < out->cap) out->p[begin] = 0;
    return false;
}

bool css_anim_compose(const struct css_anim_ctx *c, const struct propdef *p, const style_t *underlying,
                      const style_t *value, bool accumulate, sbuf *out) {
    if (css_prop_animation(p) != CSS_ANIM_VALUE) return false;
    size_t begin = out->n;
    if (anim_combine(c, p, underlying, value, 1, 1, accumulate ? ACCUMULATE : ADD, out)) return true;
    out->n = begin;
    if (out->p && begin < out->cap) out->p[begin] = 0;
    return false;
}
