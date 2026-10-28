/* The shape values' owner (shape.c) through the property table (cssprop.c) on the host:
   one request per line of the file argv[1], one answer per line written to argv[2].
     S<TAB>property<TAB>value          whether the declaration is valid, and its CSSOM specified
                                       value (the text as written when the owner keeps it)
     C<TAB>property<TAB>font<TAB>value the computed value on an element whose font-size is font px
     I<TAB>property<TAB>font<TAB>from<TAB>to<TAB>progress
                                       the animated value at progress, by the property's animation
                                       type (a pair that does not interpolate is discrete)
     A<TAB>property<TAB>font<TAB>underlying<TAB>from-op<TAB>from<TAB>to-op<TAB>to<TAB>progress
                                       the same with each keyframe composited onto underlying first
                                       (op: replace, add or accumulate)
   An invalid value answers <invalid>. */
#include <stdio.h>
#include "webi.h"

/* reached only from properties these requests do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
bool css_valid_declaration(const char *p, size_t pn, const char *v, size_t vn) { (void)p; (void)pn; (void)v; (void)vn; return false; }
bool css_container_names_valid(const char *s, size_t n) { (void)s; (void)n; return false; }
/* layout.c's, reached only from transform values */
void css_transform_list_matrix_3d(const struct css_transform *t, double m[16]) { (void)t; (void)m; abort(); }
void css_transform_list_matrix(const struct css_transform *t, float w, float h, double m[16]) { (void)t; (void)w; (void)h; (void)m; abort(); }

static char *field(char **p) {
    char *s = *p, *tab = strchr(s, '\t');
    if (tab) { *tab = 0; *p = tab + 1; } else *p = s + strlen(s);
    return s;
}
static bool specified(char *rest, sbuf *out) {
    const char *property = field(&rest), *value = rest;
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p || !css_value_valid(p, value, strlen(value))) return false;
    if (css_specified_longhand(property, value, strlen(value), out)) return true;
    size_t n = strlen(value);
    while (n && is_space((unsigned char)*value)) value++, n--;
    while (n && is_space((unsigned char)value[n - 1])) n--;
    sb_put(out, value, n);
    return true;
}
/* an element with font-size font, its parent's the same */
struct site { style_t parent, base; arena_t arena; jmp_buf trap; struct css_anim_ctx ctx; };
static void site_init(struct site *s, float font) {
    memset(s, 0, sizeof *s);
    css_style_init(&s->parent, NULL);
    s->parent.font_size = font;
    css_style_finish(&s->parent, NULL, true);
    css_style_init(&s->base, &s->parent);
    s->base.font_size = font;
    css_style_finish(&s->base, &s->parent, false);
    s->arena.trap = &s->trap;
    s->ctx = (struct css_anim_ctx){.base = &s->base, .parent = &s->parent, .rem = 16, .vw = 800, .vh = 600, .a = &s->arena};
}
static bool parse(struct site *s, const struct propdef *p, const char *v, style_t *out) {
    return css_anim_parse(&s->ctx, p, v, strlen(v), false, out, NULL);
}
static bool computed(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = field(&rest), *value = rest;
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    struct site s;
    site_init(&s, (float)strtod(font, NULL));
    bool ok = false;
    if (!setjmp(s.trap)) {
        style_t x;
        ok = parse(&s, p, value, &x) && css_computed_longhand(&x, property, out, NULL) == 1;
    }
    ar_free(&s.arena);
    return ok;
}
/* longhand q's value a·(1-t) + b·t, or the one that wins when they do not interpolate, applied to r
   as the time model applies it (animation.c: computed text, parsed into the animated style) */
static bool mix_into(struct site *s, const struct propdef *q, const style_t *a, const style_t *b, double t, style_t *r) {
    sbuf text = {0};
    bool ok = css_anim_interpolate(&s->ctx, q, a, b, t, &text);
    if (!ok) { text.n = 0; ok = css_computed_value(css_anim_discrete_second(q, a, b, t) ? b : a, q, &text, NULL); }
    if (ok) {
        uint8_t set[css_prop_count()];
        memset(set, 0, sizeof set);
        struct cx cx = {.s = r, .parent = &s->parent, .em = r->font_size, .rem = 16, .vw = 800, .vh = 600, .a = &s->arena,
                        .set = set};
        const char *v = sb_cstr(&text);
        ok = css_apply(q, v, strlen(v), &cx) && !cx.invalid;
    }
    sb_free(&text);
    return ok;
}
/* a keyframe's value of longhand q composited onto the underlying one (Web Animations 1 §5.4.4) */
static bool keyframe(struct site *s, const struct propdef *q, const style_t *underlying, const char *op, const style_t *value,
                     style_t *out) {
    if (!strcmp(op, "replace")) { *out = *value; return true; }
    sbuf text = {0};
    bool composed = css_anim_compose(&s->ctx, q, underlying, value, !strcmp(op, "accumulate"), &text);
    bool ok = composed ? parse(s, q, sb_cstr(&text), out) : (*out = *value, true); /* not additive: it replaces */
    sb_free(&text);
    return ok;
}
/* property's value at t from from to to (each first composited onto under unless op is NULL), as
   getComputedStyle reads it; a shorthand animates each of its longhands */
static bool animate(const char *property, float font, const char *under, const char *from_op, const char *from,
                    const char *to_op, const char *to, double t, sbuf *out) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    struct site s;
    site_init(&s, font);
    bool ok = false;
    if (!setjmp(s.trap)) {
        style_t u, a, b, r = s.base;
        ok = (!under || parse(&s, p, under, &u)) && parse(&s, p, from, &a) && parse(&s, p, to, &b);
        uint8_t set[css_prop_count()];
        css_prop_longhands(p, set);
        for (int i = 0; ok && i < css_prop_count(); i++) {
            if (!set[i]) continue;
            const struct propdef *q = css_prop_at(i);
            style_t x = a, y = b;
            if (under) ok = keyframe(&s, q, &u, from_op, &a, &x) && keyframe(&s, q, &u, to_op, &b, &y);
            ok = ok && mix_into(&s, q, &x, &y, t, &r);
        }
        ok = ok && css_computed_longhand(&r, property, out, NULL) == 1;
    }
    ar_free(&s.arena);
    return ok;
}
static bool interpolate(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = field(&rest), *from = field(&rest), *to = field(&rest);
    return animate(property, (float)strtod(font, NULL), NULL, NULL, from, NULL, to, strtod(rest, NULL), out);
}
static bool composite(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = field(&rest), *under = field(&rest), *from_op = field(&rest),
               *from = field(&rest), *to_op = field(&rest), *to = field(&rest);
    return animate(property, (float)strtod(font, NULL), under, from_op, from, to_op, to, strtod(rest, NULL), out);
}
int main(int argc, char **argv) {
    static char line[1 << 16];
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *answers = fopen(argv[2], "wb");
    if (!in || !answers) return 2;
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *rest = n >= 2 ? line + 2 : line + n;
        sbuf out = {0};
        bool ok = line[0] == 'S' ? specified(rest, &out) :
                  line[0] == 'C' ? computed(rest, &out) :
                  line[0] == 'I' ? interpolate(rest, &out) :
                  line[0] == 'A' ? composite(rest, &out) : false;
        fputs(ok ? sb_cstr(&out) : "<invalid>", answers);
        fputs("\n", answers);
        fflush(answers); /* a request that hangs shows as the first one unanswered */
        sb_free(&out);
    }
    fclose(in);
    fclose(answers);
    return 0;
}
