/* The shape values' owner (shape.c) and the gap decorations' values through the property table
   (cssprop.c) on the host: one request per line of the file argv[1], one answer per line written
   to argv[2].
     S<TAB>property<TAB>value          whether the declaration is valid, and its CSSOM specified
                                       value (the text as written when the owner keeps it)
     C<TAB>property<TAB>font<TAB>value the computed value on an element whose font-size is font px
                                       (font: px, or px;colour for the element's color as well)
     I<TAB>property<TAB>font<TAB>from<TAB>to<TAB>progress
                                       the animated value at progress, by the property's animation
                                       type (a pair that does not interpolate is discrete)
     A<TAB>property<TAB>font<TAB>underlying<TAB>from-op<TAB>from<TAB>to-op<TAB>to<TAB>progress
                                       the same with each keyframe composited onto underlying first
                                       (op: replace, add or accumulate)
   An invalid value answers <invalid>.

   Built with CSS_BLOCK (and css.c), as --group gaps builds it, the requests go through an
   element's CSSOM declaration block as js.c drives it (css_decl_set and css_decl_get, the block
   holding longhands with the values a shorthand gives them): S sets the property and reads it
   back, C sets it and computes the block's declarations as the cascade applies them (each, in
   order), and these requests are added:
     H<TAB>shorthand<TAB>value<TAB>longhands      shorthand-testcommon.js test_shorthand_value:
                                       for each longhand (comma-separated) its value after
                                       setProperty(shorthand, value) and that value again after
                                       the longhand is cleared and set to it; then whether
                                       CSS.supports(shorthand, value), and how many declarations
                                       stay once each longhand is cleared (fields split by \x1f)
     G<TAB>block<TAB>property          getPropertyValue(property) of the declarations block
                                       (a style attribute's text) parses to
   and, in either build:
     B<TAB>property<TAB>font           the computed value of the element itself
     T<TAB>parent<TAB>target<TAB>request  request (C, I, A or B) on an element whose parent the
                                       declarations parent apply to and itself target, applied
                                       as written (a page's .parent and .target rules); a
                                       keyframe value \x01 is the neutral keyframe, the
                                       element's underlying value */
#include <stdio.h>
#include "webi.h"

/* reached only from properties these requests do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
/* web_url.c's, reached only from a registered <url> custom property (cssprop.c's syn_url_text) */
int web_resolve_url_owned(const char *base, const char *rel, char **out) { (void)base; (void)rel; (void)out; abort(); }
#ifndef CSS_BLOCK /* css.c's */
bool css_valid_declaration(const char *p, size_t pn, const char *v, size_t vn) { (void)p; (void)pn; (void)v; (void)vn; return false; }
bool css_container_names_valid(const char *s, size_t n) { (void)s; (void)n; return false; }
#endif
/* layout.c's, reached only from transform values */
void css_transform_list_matrix_3d(const struct css_transform *t, double m[16]) { (void)t; (void)m; abort(); }
void css_transform_list_matrix(const struct css_transform *t, float w, float h, double m[16]) { (void)t; (void)w; (void)h; (void)m; abort(); }

static char *field(char **p) {
    char *s = *p, *tab = strchr(s, '\t');
    if (tab) { *tab = 0; *p = tab + 1; } else *p = s + strlen(s);
    return s;
}
#ifdef CSS_BLOCK
/* setProperty(property, value) on an element's declarations (the empty value removes it) */
static bool block_set(sbuf *block, const char *property, const char *value) {
    sbuf next = {0};
    int r = css_decl_set(block->p ? block->p : "", block->n, property, value, "", NULL, &next);
    if (r == 1) { sb_free(block); *block = next; return true; }
    sb_free(&next);
    return false;
}
static void block_get(const sbuf *block, const char *property, sbuf *out) {
    bool important;
    css_decl_get(block->p ? block->p : "", block->n, property, NULL, out, &important);
}
#endif
static bool specified(char *rest, sbuf *out) {
    const char *property = field(&rest), *value = rest;
    const struct propdef *p = css_prop_lookup(property, strlen(property));
#ifdef CSS_BLOCK
    /* div.style[property] = value, read back: an empty value is not set */
    sbuf block = {0};
    bool set = p && block_set(&block, property, value);
    if (set) block_get(&block, property, out);
    sb_free(&block);
    return set && out->n;
#else
    if (!p || !css_value_valid(p, value, strlen(value))) return false;
    if (css_specified_longhand(property, value, strlen(value), out)) return true;
    size_t n = strlen(value);
    while (n && is_space((unsigned char)*value)) value++, n--;
    while (n && is_space((unsigned char)value[n - 1])) n--;
    sb_put(out, value, n);
    return true;
#endif
}
/* an element with font-size font (and the colour after a ';'), its parent's the same; the
   declarations page_parent and page_target (a T request's) apply to the parent and the element */
struct site { style_t parent, base; arena_t arena; jmp_buf trap; struct css_anim_ctx ctx; };
static const char *page_parent, *page_target;
static void site_init(struct site *s) {
    memset(s, 0, sizeof *s);
    s->arena.trap = &s->trap;
}
/* declarations (a block's text, or a rule's) applied in order to st, each as css_anim_parse applies
   one (font-size first, then against it); false when one does not apply */
static bool rules_apply(struct site *s, style_t *st, const style_t *parent, const char *rules) {
    bool ok = true;
    for (const char *p = rules; *p;) {
        const char *e = p;
        int depth = 0;
        char quote = 0;
        for (; *e && (quote || depth || *e != ';'); e++) {
            if (quote) { if (*e == '\\' && e[1]) e++; else if (*e == quote) quote = 0; }
            else if (*e == '"' || *e == '\'') quote = *e;
            else if (*e == '(') depth++;
            else if (*e == ')' && depth) depth--;
        }
        const char *colon = memchr(p, ':', (size_t)(e - p));
        if (colon) {
            const char *name = p, *v = colon + 1;
            size_t nn = (size_t)(colon - p), vn = (size_t)(e - v);
            while (nn && is_space((unsigned char)*name)) name++, nn--;
            while (nn && is_space((unsigned char)name[nn - 1])) nn--;
            while (vn && is_space((unsigned char)*v)) v++, vn--;
            while (vn && is_space((unsigned char)v[vn - 1])) vn--;
            if (vn > 10 && !strncasecmp(v + vn - 10, "!important", 10)) {
                vn -= 10;
                while (vn && is_space((unsigned char)v[vn - 1])) vn--;
            }
            const struct propdef *q = css_prop_lookup(name, nn);
            char *value = ar_alloc(&s->arena, vn + 1);
            memcpy(value, v, vn);
            value[vn] = 0;
            uint8_t set[css_prop_count()];
            memset(set, 0, sizeof set);
            struct cx cx = {.s = st, .parent = parent, .em = parent ? parent->font_size : 16, .rem = 16, .vw = 800, .vh = 600,
                            .a = &s->arena, .set = set, .font_pass = true};
            if (!q || (css_prop_is_early(q) && !css_apply(q, value, vn, &cx))) ok = false;
            else {
                cx.font_pass = false;
                cx.em = st->font_size;
                ok &= css_apply(q, value, vn, &cx) && !cx.invalid;
            }
        }
        p = *e ? e + 1 : e;
    }
    return ok;
}
static void site_style(struct site *s, const char *font) {
    char *end;
    float size = strtof(font, &end);
    css_style_init(&s->parent, NULL);
    s->parent.font_size = size;
    if (*end == ';') { /* inherited by the element */
        uint8_t set[css_prop_count()];
        memset(set, 0, sizeof set);
        struct cx cx = {.s = &s->parent, .em = size, .rem = 16, .vw = 800, .vh = 600, .a = &s->arena, .set = set};
        css_apply(css_prop_lookup("color", 5), end + 1, strlen(end + 1), &cx);
    }
    if (page_parent) rules_apply(s, &s->parent, NULL, page_parent);
    css_style_finish(&s->parent, NULL, true);
    css_style_init(&s->base, &s->parent);
    s->base.font_size = size;
    if (page_target) rules_apply(s, &s->base, &s->parent, page_target);
    css_style_finish(&s->base, &s->parent, false);
    s->ctx = (struct css_anim_ctx){.base = &s->base, .parent = &s->parent, .rem = 16, .vw = 800, .vh = 600, .a = &s->arena};
}
/* v's value of p on the element; \x01: the neutral keyframe, its underlying value */
static bool parse(struct site *s, const struct propdef *p, const char *v, style_t *out) {
    if (v[0] == '\x01') { *out = s->base; return true; }
    return css_anim_parse(&s->ctx, p, v, strlen(v), false, out, NULL);
}
static bool computed(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = field(&rest), *value = rest;
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    struct site s;
    site_init(&s);
    bool ok = false;
#ifdef CSS_BLOCK
    /* the element's declarations hold what setProperty gave them; the cascade applies them */
    sbuf block = {0};
    if (!block_set(&block, property, value)) return false;
    if (!setjmp(s.trap)) {
        site_style(&s, font);
        style_t x = s.base;
        ok = rules_apply(&s, &x, &s.parent, sb_cstr(&block)) && css_computed_longhand(&x, property, out, NULL) == 1;
    }
    sb_free(&block);
#else
    if (!setjmp(s.trap)) {
        site_style(&s, font);
        style_t x;
        ok = parse(&s, p, value, &x) && css_computed_longhand(&x, property, out, NULL) == 1;
    }
#endif
    ar_free(&s.arena);
    return ok;
}
/* the computed value of the element itself (its page's rules alone) */
static bool base_value(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = rest;
    struct site s;
    site_init(&s);
    bool ok = false;
    if (!setjmp(s.trap)) {
        site_style(&s, font);
        ok = css_computed_longhand(&s.base, property, out, NULL) == 1;
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
static bool animate(const char *property, const char *font, const char *under, const char *from_op, const char *from,
                    const char *to_op, const char *to, double t, sbuf *out) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    struct site s;
    site_init(&s);
    bool ok = false;
    if (!setjmp(s.trap)) {
        site_style(&s, font);
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
    return animate(property, font, NULL, NULL, from, NULL, to, strtod(rest, NULL), out);
}
static bool composite(char *rest, sbuf *out) {
    const char *property = field(&rest), *font = field(&rest), *under = field(&rest), *from_op = field(&rest),
               *from = field(&rest), *to_op = field(&rest), *to = field(&rest);
    return animate(property, font, under, from_op, from, to_op, to, strtod(rest, NULL), out);
}
#ifdef CSS_BLOCK
/* test_shorthand_value's steps on an element's declarations */
static bool shorthand(char *rest, sbuf *out) {
    const char *property = field(&rest), *value = field(&rest);
    char *names[64];
    int count = 0;
    for (char *l = rest; *l && count < 64;) {
        char *comma = strchr(l, ',');
        if (comma) *comma = 0;
        names[count++] = l;
        l = comma ? comma + 1 : l + strlen(l);
    }
    for (int i = 0; i < count; i++) {
        sbuf block = {0}, read = {0}, back = {0};
        block_set(&block, property, value);
        block_get(&block, names[i], &read);
        block_set(&block, names[i], "");
        block_set(&block, names[i], sb_cstr(&read));
        block_get(&block, names[i], &back);
        sb_put(out, read.p, read.n); sb_putc(out, '\x1f');
        sb_put(out, back.p, back.n); sb_putc(out, '\x1f');
        sb_free(&block); sb_free(&read); sb_free(&back);
    }
    sbuf block = {0}, text = {0}, list = {0};
    block_set(&block, property, value);
    bool supports = css_supports_declaration(property, strlen(property), value, strlen(value));
    for (int i = 0; i < count; i++) block_set(&block, names[i], "");
    css_decl_block_serialize(block.p ? block.p : "", block.n, NULL, &text, &list);
    int left = 0;
    for (size_t k = 0; k < list.n; k++) left += !list.p[k];
    char tail[32];
    snprintf(tail, sizeof tail, "%d\x1f%d", supports, left);
    sb_puts(out, tail);
    sb_free(&block); sb_free(&text); sb_free(&list);
    return true;
}
/* getPropertyValue of the declarations a style attribute's text parses to */
static bool attribute_value(char *rest, sbuf *out) {
    const char *text = field(&rest), *property = rest;
    sbuf block = {0};
    bool ok = css_decl_block(text, strlen(text), NULL, &block);
    if (ok) block_get(&block, property, out);
    sb_free(&block);
    return ok;
}
#endif
static bool request(char *line, sbuf *out);
/* a request on an element of the page's rules */
static bool page_request(char *rest, sbuf *out) {
    page_parent = field(&rest);
    page_target = field(&rest);
    bool ok = request(rest, out);
    page_parent = page_target = NULL;
    return ok;
}
static bool request(char *line, sbuf *out) {
    char *rest = line[0] && line[1] ? line + 2 : line + strlen(line);
    return line[0] == 'S' ? specified(rest, out) :
           line[0] == 'C' ? computed(rest, out) :
           line[0] == 'I' ? interpolate(rest, out) :
           line[0] == 'A' ? composite(rest, out) :
           line[0] == 'B' ? base_value(rest, out) :
           line[0] == 'T' ? page_request(rest, out) :
#ifdef CSS_BLOCK
           line[0] == 'H' ? shorthand(rest, out) :
           line[0] == 'G' ? attribute_value(rest, out) :
#endif
           false;
}
int main(int argc, char **argv) {
    static char line[1 << 16];
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *answers = fopen(argv[2], "wb");
    if (!in || !answers) return 2;
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        sbuf out = {0};
        bool ok = request(line, &out);
        fputs(ok ? sb_cstr(&out) : "<invalid>", answers);
        fputs("\n", answers);
        fflush(answers); /* a request that hangs shows as the first one unanswered */
        sb_free(&out);
    }
    fclose(in);
    fclose(answers);
    return 0;
}
