/* The Filter Effects values (filter, backdrop-filter, color-interpolation-filters) through the
   property table (cssprop.c) on the host: one request per line of the file argv[1], one answer per
   line written to argv[2]. An element's declarations are "name:value;name:value" (empty: none); the
   element's parent has its own, and the parent's parent is the root. The container is 100px wide.
     S<TAB>property<TAB>value          the declared value's CSSOM specified serialization
     C<TAB>property<TAB>parent<TAB>target<TAB>value
                                       the property's resolved value on the target, value set
                                       inline over its declarations (empty: not set)
     O<TAB>property<TAB>parent<TAB>target<TAB>value
                                       as C, the value set through element.style: CSSOM's setProperty
                                       on the inline declarations (css.c's css_inline_set) and the
                                       value the cascade parses from them (css_inline_text)
     I<TAB>property<TAB>parent<TAB>target<TAB>underlying<TAB>from<TAB>fc<TAB>to<TAB>tc<TAB>progress
                                       a keyframe pair's sample on the target whose own value is
                                       underlying: from and to composite onto it (r replace, a add,
                                       c accumulate), then interpolate (or the discrete choice), and
                                       the sample is applied as C applies a value
   An invalid value answers <invalid>. */
#include <stdio.h>
#include "webi.h"

/* reached only from properties these requests do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
/* layout.c's, reached only from Geometry's matrix strings and the animation of transform lists */
void css_transform_list_matrix_3d(const struct css_transform *t, double m[16]) { (void)t; (void)m; abort(); }
void css_transform_list_matrix(const struct css_transform *t, float w, float h, double m[16]) {
    (void)t; (void)w; (void)h; (void)m;
    abort();
}

static char *field(char **p) {
    char *s = *p, *tab = strchr(s, '\t');
    if (tab) { *tab = 0; *p = tab + 1; } else *p = s + strlen(s);
    return s;
}
/* one declaration in one of the cascade's two passes (early: font-size and color-scheme) */
static bool declare(style_t *s, const style_t *parent, arena_t *a, uint8_t *set, bool early, const char *property,
                    size_t pn, const char *value, size_t vn) {
    const struct propdef *p = css_prop_lookup(property, pn);
    if (!p) return false;
    if (css_prop_is_early(p) != early) return true;
    struct cx cx = {.s = s, .parent = parent, .em = parent ? parent->font_size : 16, .rem = 16, .vw = 800, .vh = 600,
                    .a = a, .set = set, .font_pass = early, .cq_width = 100, .cq_width_set = true};
    if (!early) cx.em = s->font_size;
    return css_apply(p, value, vn, &cx);
}
/* "name:value;..." in order: the more important first, as the cascade applies them */
static bool declare_list(style_t *s, const style_t *parent, arena_t *a, uint8_t *set, bool early, const char *list) {
    for (const char *q = list; *q;) {
        const char *end = strchr(q, ';'), *colon;
        if (!end) end = q + strlen(q);
        colon = memchr(q, ':', (size_t)(end - q));
        if (colon && !declare(s, parent, a, set, early, q, (size_t)(colon - q), colon + 1, (size_t)(end - colon - 1)))
            return false;
        q = *end ? end + 1 : end;
    }
    return true;
}
/* the parent and the target, the target's value for property set inline (value NULL or empty: not) */
static bool elements(const char *parent_decls, const char *target_decls, const char *property, const char *value,
                     arena_t *a, style_t *parent, style_t *s) {
    size_t count = (size_t)css_prop_count();
    uint8_t *set = calloc(count, 1);
    if (!set) return false;
    bool ok = true;
    css_style_init(parent, NULL);
    for (int early = 1; early >= 0 && ok; early--) {
        memset(set, 0, count);
        ok = declare_list(parent, NULL, a, set, early, parent_decls);
    }
    css_style_finish(parent, NULL, true);
    css_style_init(s, parent);
    for (int early = 1; early >= 0 && ok; early--) {
        memset(set, 0, count);
        ok = (!value || !*value || declare(s, parent, a, set, early, property, strlen(property), value, strlen(value))) &&
             declare_list(s, parent, a, set, early, target_decls);
    }
    if (ok) css_style_finish(s, parent, false);
    free(set);
    return ok;
}
static bool computed_value(const char *property, const char *parent_decls, const char *target_decls, const char *value,
                           sbuf *out) {
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    if (!setjmp(trap)) {
        style_t parent, s;
        ok = elements(parent_decls, target_decls, property, value, &a, &parent, &s);
        int kind = ok ? css_computed_longhand(&s, property, out, NULL) : -1;
#ifdef FILTER_BEFORE
        /* the product before Filter Effects 1: js.c's computed() serialized filter itself */
        if (!kind && !strcmp(property, "filter")) kind = css_filter_serialize(s.filter, out, NULL) ? 1 : -1;
#endif
        ok = kind == 1;
    }
    ar_free(&a);
    return ok;
}
static bool computed(char *rest, sbuf *out) {
    const char *property = field(&rest), *parent = field(&rest), *target = field(&rest);
    return computed_value(property, parent, target, rest, out);
}
/* an element with no style attribute: style.setProperty(property, value), then its declarations
   as the cascade reads them */
static bool through_cssom(char *rest, sbuf *out) {
    const char *property = field(&rest), *parent = field(&rest), *target = field(&rest), *value = rest;
    node_t e = {.type = N_ELEM};
    sbuf attribute = {0};
    bool ok = css_inline_set(NULL, &e, "", 0, property, value, NULL, &attribute) > 0;
    struct attr style = {.name = "style", .raw = "style", .local = "style", .value = sb_cstr(&attribute)};
    e.attrs = &style;
    e.nattrs = 1;
    const char *text = ok ? css_inline_text(&e) : NULL, *v;
    size_t vn;
    bool important;
    if (text && css_decl_find(text, strlen(text), property, &v, &vn, &important)) {
        char *copy = malloc(vn + 1);
        if (copy) {
            memcpy(copy, v, vn);
            copy[vn] = 0;
            ok = computed_value(property, parent, target, copy, out);
            free(copy);
        } else ok = false;
    } else ok = false;
    free(e.style_cssom);
    sb_free(&attribute);
    return ok;
}
static bool interpolate(char *rest, sbuf *out) {
    const char *property = field(&rest), *parent_decls = field(&rest), *target_decls = field(&rest);
    const char *underlying = field(&rest), *ends[2], *modes[2];
    ends[0] = field(&rest); modes[0] = field(&rest); ends[1] = field(&rest); modes[1] = field(&rest);
    double t = strtod(rest, NULL);
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    char *sample = NULL;
    if (!setjmp(trap)) {
        style_t parent, base, v[2];
        sbuf text = {0};
        bool ok = elements(parent_decls, target_decls, property, underlying, &a, &parent, &base);
        struct css_anim_ctx c = {.base = &base, .parent = &parent, .rem = 16, .vw = 800, .vh = 600, .a = &a};
        for (int i = 0; i < 2 && ok; i++) {
            ok = css_anim_parse(&c, p, ends[i], strlen(ends[i]), false, &v[i], NULL);
            if (!ok || *modes[i] == 'r') continue;
            text.n = 0;
            if (!css_anim_compose(&c, p, &base, &v[i], *modes[i] == 'c', &text)) continue; /* replace */
            char *combined = strdup(sb_cstr(&text));
            ok = combined && css_anim_parse(&c, p, combined, strlen(combined), true, &v[i], NULL);
            free(combined);
        }
        if (ok) {
            text.n = 0;
            if (!css_anim_interpolate(&c, p, &v[0], &v[1], t, &text)) {
                text.n = 0;
                ok = css_computed_value(css_anim_discrete_second(p, &v[0], &v[1], t) ? &v[1] : &v[0], p, &text, NULL);
            }
        }
        if (ok) sample = strdup(sb_cstr(&text));
        sb_free(&text);
    }
    ar_free(&a);
    bool ok = sample && computed_value(property, parent_decls, target_decls, sample, out);
    free(sample);
    return ok;
}
/* what CSSStyleDeclaration returns after style[property] = value: css.c's css_inline_set takes a valid
   value, and the style attribute holds its specified serialization (css_decl_value) */
static bool specified(char *rest, sbuf *out) {
    const char *property = field(&rest);
    node_t e = {.type = N_ELEM};
    sbuf attribute = {0};
    bool ok = css_inline_set(NULL, &e, "", 0, property, rest, NULL, &attribute) > 0;
    const char *v;
    size_t vn;
    bool important;
    ok = ok && css_decl_find(sb_cstr(&attribute), attribute.n, property, &v, &vn, &important);
    if (ok) sb_put(out, v, vn);
    free(e.style_cssom);
    sb_free(&attribute);
    return ok;
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
                  line[0] == 'O' ? through_cssom(rest, &out) :
                  line[0] == 'I' ? interpolate(rest, &out) : false;
        fputs(ok ? sb_cstr(&out) : "<invalid>", answers);
        fputs("\n", answers);
        fflush(answers); /* a request that hangs shows as the first one unanswered */
        sb_free(&out);
    }
    fclose(in);
    fclose(answers);
    return 0;
}
