/* CSS math functions and units (cssmath.c) through the property table (cssprop.c) on the host:
   one request per line of the file argv[1], one answer per line written to argv[2].
     S<TAB>property<TAB>value   whether the declaration is valid, and its CSSOM specified value
                                (the text as written when no owner rewrites it)
     C<TAB>property<TAB>site<TAB>value
                                the computed value on an element in the site "font,parent,root":
                                its font-size, its parent's and the root's, in px
     U<TAB>property<TAB>site<TAB>value
                                the used value of a <length-percentage> property whose percentages are
                                of 100px; other properties: the computed value
     A<TAB>property<TAB>site<TAB>interpolate-size<TAB>underlying<TAB>op<TAB>from<TAB>op<TAB>to<TAB>t
                                one keyframe pair of an effect sampled at t (animate() below)
   An invalid value answers <invalid>; a property this tool cannot read answers <unread>. */
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
static int specified(const char *property, const char *value, sbuf *out) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return -1;
    if (!css_value_valid(p, value, strlen(value))) return 0;
    if (css_specified_longhand(property, value, strlen(value), out)) return 1;
    size_t n = strlen(value);
    while (n && is_space((unsigned char)*value)) value++, n--;
    while (n && is_space((unsigned char)value[n - 1])) n--;
    sb_put(out, value, n);
    return 1;
}
/* an element and its parent, with the font sizes "font,parent,root" gives (missing: 16px) */
struct site { style_t parent, base; arena_t arena; jmp_buf trap; struct css_anim_ctx ctx; };
static void site_init(struct site *s, const char *sizes) {
    float px[3] = {16, 16, 16};
    char *end;
    for (int i = 0; i < 3 && *sizes; i++) {
        double v = strtod(sizes, &end);
        if (end == sizes) break;
        px[i] = (float)v;
        sizes = *end == ',' ? end + 1 : end;
    }
    memset(s, 0, sizeof *s);
    css_style_init(&s->parent, NULL);
    s->parent.font_size = px[1];
    css_style_finish(&s->parent, NULL, true);
    css_style_init(&s->base, &s->parent);
    s->base.font_size = px[0];
    css_style_finish(&s->base, &s->parent, false);
    s->arena.trap = &s->trap;
    s->ctx = (struct css_anim_ctx){.base = &s->base, .parent = &s->parent, .rem = px[2], .vw = 800, .vh = 600, .a = &s->arena};
}
/* the <length-percentage> fields the used values read */
static const len_t *length_field(const style_t *st, const char *property) {
    static const struct { const char *name; size_t off; } fields[] = {
        {"margin-left", offsetof(style_t, margin[3])}, {"margin-right", offsetof(style_t, margin[1])},
        {"margin-top", offsetof(style_t, margin[0])}, {"padding-left", offsetof(style_t, padding[3])},
        {"width", offsetof(style_t, width)}, {"height", offsetof(style_t, height)},
        {"min-width", offsetof(style_t, min_width)}, {"max-width", offsetof(style_t, max_width)},
        {"flex-basis", offsetof(style_t, flex_basis)}, {"text-indent", offsetof(style_t, text_indent)},
        {"left", offsetof(style_t, inset[3])}, {"top", offsetof(style_t, inset[0])}};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
        if (!strcmp(fields[i].name, property)) return (const len_t *)((const char *)st + fields[i].off);
    return NULL;
}
/* 0: the value is invalid, -1: the property's value is not one this tool reads */
static int computed(const char *property, const char *sizes, const char *value, bool used, sbuf *out) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return -1;
    struct site s;
    site_init(&s, sizes);
    int r = 0;
    if (!setjmp(s.trap)) {
        style_t x;
        if (css_anim_parse(&s.ctx, p, value, strlen(value), false, &x, NULL)) {
            char t[64];
            const len_t *l = used ? length_field(&x, property) : NULL;
            r = 1;
            if (l && (l->kind == LK_LEN || l->kind == LK_EXPR)) {
                snprintf(t, sizeof t, "%.6gpx", (double)len_resolve(l, 100) + 0.0); /* CSSOM has no -0 */
                sb_puts(out, t);
            } else if (!strcmp(property, "z-index")) {
                if (x.z_auto) sb_puts(out, "auto");
                else { snprintf(t, sizeof t, "%d", x.z_index); sb_puts(out, t); }
            } else if (!strcmp(property, "line-height")) {
                if (x.line_height.kind == LK_NORMAL) sb_puts(out, "normal");
                else { snprintf(t, sizeof t, x.line_height.kind == LK_NUMBER ? "%.6g" : "%.6gpx", (double)x.line_height.px); sb_puts(out, t); }
            } else if (!strcmp(property, "font-weight")) {
                snprintf(t, sizeof t, "%u", x.font_weight);
                sb_puts(out, t);
            } else {
                int k = css_computed_longhand(&x, property, out, NULL);
                r = k == 1 ? 1 : k == 0 ? -1 : 0;
            }
        }
    }
    ar_free(&s.arena);
    return r;
}
/* what getComputedStyle shows once the cascade applies text: 1, 0 invalid, -1 unread */
static int show(struct site *s, const struct propdef *p, const char *property, const char *text, bool computed_text, sbuf *out) {
    style_t x;
    if (!css_anim_parse(&s->ctx, p, text, strlen(text), computed_text, &x, NULL)) return 0;
    int k = css_computed_longhand(&x, property, out, NULL);
    return k == 1 ? 1 : k == 0 ? -1 : 0;
}
/* A request: one keyframe pair of an effect (animation.c's effect value) on the target, its
   interpolate-size (cssprop.c's anim_combine reads the base style's) "0" numeric-only or "1"
   allow-keywords. Each end composites ('r' replace, 'a' add, 'c' accumulate) onto the underlying value
   (empty: the initial value), and the pair interpolates at t. The answer is the sample as
   getComputedStyle shows it; a pair that does not interpolate (or a property that animates
   discretely) answers <discrete>, its two composited ends as shown, each after a U+0001; a property
   that does not animate answers <none>. */
static int animate(const char *property, const char *sizes, char *fields, sbuf *out) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return -1;
    char *mode = field(&fields), *under = field(&fields), *ops[2], *ends[2];
    ops[0] = field(&fields), ends[0] = field(&fields), ops[1] = field(&fields), ends[1] = field(&fields);
    double t = strtod(fields, NULL);
    if (css_prop_animation(p) == CSS_ANIM_NONE) { sb_puts(out, "<none>"); return 1; }
    /* the composited ends and the sample, as text (kept across the arena's trap) */
    static sbuf text[3];
    for (int i = 0; i < 3; i++) { text[i].n = 0; if (text[i].p) text[i].p[0] = 0; }
    struct site s;
    site_init(&s, sizes);
    /* the target's interpolate-size, through the property's own row (a tree without it has none) */
    const struct propdef *ip = css_prop_lookup("interpolate-size", 16);
    if (ip) {
        uint8_t set[css_prop_count()];
        memset(set, 0, sizeof set);
        const char *v = *mode == '1' ? "allow-keywords" : "numeric-only";
        struct cx cx = {.s = &s.base, .parent = &s.parent, .em = s.base.font_size, .rem = s.ctx.rem, .vw = 800, .vh = 600,
                        .a = &s.arena, .set = set};
        css_apply(ip, v, strlen(v), &cx);
        css_style_finish(&s.base, &s.parent, false);
    }
    int r = 0;
    if (!setjmp(s.trap)) {
        style_t u, v[2];
        bool composited[2] = {false, false}, ok = true;
        if (!*under) under = "initial";
        ok = css_anim_parse(&s.ctx, p, under, strlen(under), false, &u, NULL);
        for (int i = 0; i < 2 && ok; i++) {
            ok = css_anim_parse(&s.ctx, p, ends[i], strlen(ends[i]), false, &v[i], NULL);
            /* a value that does not add replaces (Web Animations §5.4.4) */
            if (ok && *ops[i] != 'r' && css_anim_compose(&s.ctx, p, &u, &v[i], *ops[i] == 'c', &text[i])) {
                composited[i] = true;
                ok = css_anim_parse(&s.ctx, p, sb_cstr(&text[i]), text[i].n, true, &v[i], NULL);
            }
        }
        if (ok && css_anim_interpolate(&s.ctx, p, &v[0], &v[1], t, &text[2])) {
            r = show(&s, p, property, sb_cstr(&text[2]), false, out);
        } else if (ok) {
            sb_puts(out, "<discrete>");
            r = 1;
            for (int i = 0; i < 2 && r == 1; i++) {
                sb_putc(out, 1);
                r = show(&s, p, property, composited[i] ? sb_cstr(&text[i]) : ends[i], composited[i], out);
            }
        }
    }
    ar_free(&s.arena);
    return r;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *answers = fopen(argv[2], "wb");
    if (!in || !answers) return 2;
    static char line[1 << 16];
    sbuf out = {0};
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *rest = line, *op = field(&rest), *property = field(&rest), *value = rest;
        out.n = 0;
        if (out.p) out.p[0] = 0;
        int ok;
        if (!strcmp(op, "S")) ok = specified(property, value, &out);
        else if (!strcmp(op, "A")) {
            char *sizes = field(&value);
            ok = animate(property, sizes, value, &out);
        } else {
            char *sizes = field(&value);
            ok = computed(property, sizes, value, !strcmp(op, "U"), &out);
        }
        fprintf(answers, "%s\n", ok > 0 ? sb_cstr(&out) : ok < 0 ? "<unread>" : "<invalid>");
    }
    sb_free(&out);
    fclose(in);
    fclose(answers);
    return 0;
}
