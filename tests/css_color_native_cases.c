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
                                       as an animation of color
     H<TAB>color<TAB>value             a canvas's fillStyle after setting value, the canvas
                                       element's color being color (empty: no element)
   An invalid value answers <invalid>. */
#include <stdio.h>
#include "webi.h"

/* reached only from properties these requests do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
bool css_valid_declaration(const char *p, size_t pn, const char *v, size_t vn) { (void)p; (void)pn; (void)v; (void)vn; return false; }
bool css_container_names_valid(const char *s, size_t n) { (void)s; (void)n; return false; }
enum http_url_result http_canonical_owned_n(const char *r, size_t n, char **c, char **o) {
    (void)r; (void)n; (void)c; (void)o;
    return HTTP_URL_INVALID;
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
static bool interpolate(char *rest, sbuf *out) {
    const char *from = field(&rest), *to = field(&rest);
    double p = strtod(rest, NULL);
    struct css_color a, b;
    if (!css_color_parse_used(from, strlen(from), NULL, NULL, &a) || !css_color_parse_used(to, strlen(to), NULL, NULL, &b))
        return false;
    struct css_color_interpolation m = css_color_default_interpolation(&a, &b);
    struct css_color r = css_color_interpolate(&a, &b, p, &m);
    return css_color_serialize(&r, false, out);
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
static bool specified(char *rest, sbuf *out) {
    const char *property = field(&rest);
    return css_specified_longhand(property, rest, strlen(rest), out);
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
                  line[0] == 'H' ? canvas(rest, &out) : false;
        fputs(ok ? sb_cstr(&out) : "<invalid>", answers);
        fputs("\n", answers);
        fflush(answers); /* a request that hangs shows as the first one unanswered */
        sb_free(&out);
    }
    fclose(in);
    fclose(answers);
    return 0;
}
