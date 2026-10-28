/* The CSS colour owner (color.c) through the property table (cssprop.c) on the
   host: one request per line of the file argv[1], one answer per line written
   to argv[2].
     S<TAB>property<TAB>value          the declared value's CSSOM specified serialization
     C<TAB>property<TAB>scheme<TAB>color<TAB>value
                                       the property's resolved value on an element whose
                                       parent has color: rgb(255, 0, 0) and whose own color
                                       is color (empty: inherited); scheme: the parent's
                                       color-scheme (empty: normal)
     I<TAB>from<TAB>to<TAB>progress    CSS Color 4 interpolation by the default method,
                                       as an animation of color: the animation layer's
                                       sample, applied as computed() applies a value
     H<TAB>color<TAB>value             a canvas's fillStyle after setting value, the canvas
                                       element's color being color (empty: no element)
     O<TAB>property<TAB>scheme<TAB>color<TAB>value
                                       as C, the value set through element.style: CSSOM's
                                       setProperty on the element's inline declarations
                                       (css.c's css_inline_set), the style attribute becoming
                                       their serialization, and the property's value in what
                                       the cascade parses (css_inline_text)
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
                    const char *value) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p) return false;
    if (css_prop_is_early(p) != early) return true;
    struct cx cx = {.s = s, .parent = parent, .em = 16, .rem = 16, .vw = 800, .vh = 600, .a = a, .set = set,
                    .font_pass = early};
    return css_apply(p, value, strlen(value), &cx);
}
static bool computed(char *rest, sbuf *out) {
    const char *property = field(&rest), *scheme = field(&rest), *color = field(&rest), *value = rest;
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    size_t count = (size_t)css_prop_count();
    uint8_t *set = calloc(count, 1);
    if (!set) return false;
    if (!setjmp(trap)) {
        style_t parent, s;
        css_style_init(&parent, NULL);
        ok = true;
        for (int early = 1; early >= 0; early--) {
            memset(set, 0, count);
            if (*scheme) ok &= declare(&parent, NULL, &a, set, early, "color-scheme", scheme);
            ok &= declare(&parent, NULL, &a, set, early, "color", "rgb(255, 0, 0)");
        }
        css_style_finish(&parent, NULL, true);
        css_style_init(&s, &parent);
        for (int early = 1; early >= 0 && ok; early--) {
            memset(set, 0, count);
            /* the more important declaration first, as the cascade applies them */
            ok = declare(&s, &parent, &a, set, early, property, value) &&
                 (!*color || declare(&s, &parent, &a, set, early, "color", color));
        }
        if (ok) {
            css_style_finish(&s, &parent, false);
            ok = css_computed_longhand(&s, property, out, NULL) == 1;
        }
    }
    free(set);
    ar_free(&a);
    return ok;
}
/* the animation layer (cssprop.c) samples color between the two keyframe values, and the cascade
   applies the sample as computed() does a declaration */
static bool interpolate(char *rest, sbuf *out) {
    const char *from = field(&rest), *to = field(&rest);
    double p = strtod(rest, NULL);
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    char *request = NULL;
    if (!setjmp(trap)) {
        style_t parent, base, x, y;
        css_style_init(&parent, NULL);
        css_style_finish(&parent, NULL, true);
        css_style_init(&base, &parent);
        css_style_finish(&base, &parent, false);
        struct css_anim_ctx c = {.base = &base, .parent = &parent, .rem = 16, .vw = 800, .vh = 600, .a = &a};
        const struct propdef *color = css_prop_lookup("color", 5);
        sbuf sample = {0};
        if (css_anim_parse(&c, color, from, strlen(from), false, &x, NULL) &&
            css_anim_parse(&c, color, to, strlen(to), false, &y, NULL) && css_anim_interpolate(&c, color, &x, &y, p, &sample)) {
            request = malloc(sample.n + 9);
            if (request) { memcpy(request, "color\t\t\t", 8); memcpy(request + 8, sb_cstr(&sample), sample.n); request[sample.n + 8] = 0; }
        }
        sb_free(&sample);
    }
    ar_free(&a);
    if (request) ok = computed(request, out);
    free(request);
    return ok;
}
static const style_t *canvas_element(void *context) { return context; }
static bool canvas(char *rest, sbuf *out) {
    const char *color = field(&rest), *value = rest;
    arena_t a = {0};
    jmp_buf trap;
    a.trap = &trap;
    bool ok = false;
    size_t count = (size_t)css_prop_count();
    uint8_t *set = calloc(count, 1);
    if (!set) return false;
    if (!setjmp(trap)) {
        style_t element;
        css_style_init(&element, NULL);
        if (!*color || declare(&element, NULL, &a, set, false, "color", color)) {
            css_style_finish(&element, NULL, true);
            struct css_color c;
            ok = css_color_parse_used(value, strlen(value), *color ? canvas_element : NULL, &element, &c) &&
                 css_color_serialize(&c, true, out);
        }
    }
    free(set);
    ar_free(&a);
    return ok;
}
/* what CSSStyleDeclaration returns (js.c's css_style_value): the table's serialization, else the
   authored text of a valid value (a CSS-wide keyword stays as written) */
static bool specified(char *rest, sbuf *out) {
    const char *property = field(&rest);
    if (css_specified_longhand(property, rest, strlen(rest), out)) return true;
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p || !css_value_valid(p, rest, strlen(rest))) return false;
    sb_puts(out, rest);
    return true;
}
/* an element with no style attribute: style.setProperty(property, value), then its declarations
   as the cascade reads them */
static bool through_cssom(char *rest, sbuf *out) {
    const char *property = field(&rest), *scheme = field(&rest), *color = field(&rest), *value = rest;
    node_t e = {.type = N_ELEM};
    sbuf attribute = {0}, request = {0};
    bool ok = css_inline_set(NULL, &e, "", 0, property, value, NULL, &attribute) > 0;
    struct attr style = {.name = "style", .raw = "style", .local = "style", .value = sb_cstr(&attribute)};
    e.attrs = &style;
    e.nattrs = 1;
    const char *text = ok ? css_inline_text(&e) : NULL, *v;
    size_t vn;
    bool important;
    if (text && css_decl_find(text, strlen(text), property, &v, &vn, &important)) {
        sb_puts(&request, property); sb_putc(&request, '\t');
        sb_puts(&request, scheme); sb_putc(&request, '\t');
        sb_puts(&request, color); sb_putc(&request, '\t');
        sb_put(&request, v, vn);
        ok = computed((char *)sb_cstr(&request), out);
    } else ok = false;
    free(e.style_cssom);
    sb_free(&attribute);
    sb_free(&request);
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
                  line[0] == 'I' ? interpolate(rest, &out) :
                  line[0] == 'H' ? canvas(rest, &out) :
                  line[0] == 'O' ? through_cssom(rest, &out) : false;
        fputs(ok ? sb_cstr(&out) : "<invalid>", answers);
        fputs("\n", answers);
        fflush(answers); /* a request that hangs shows as the first one unanswered */
        sb_free(&out);
    }
    fclose(in);
    fclose(answers);
    return 0;
}
