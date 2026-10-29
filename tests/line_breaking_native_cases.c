/* CSS Text 3/4 line breaking in the inline layout (layout.c) on the host: the real build_items,
   text_items, wrap_between, finish_line, split_word, layout_inline and inline_intrinsic over text
   boxes processed by the real ws_process (box.c), their styles parsed by the real property table
   (cssprop.c). The font is a monospace model: a Latin or other narrow character is half the
   font-size, a wide (East Asian Width W or F) one the font-size, a default ignorable one 0; the
   ascent is 0.8 em and the descent 0.2 em. The cases are WPT's css/css-text pages (named at each
   group) whose expectations are where the lines break, and lines the specification's text decides.

   Not the guest WPT: the cascade (css.c), box generation, floats, atomic inlines, paint and real
   fonts are not reached. */
#include <stdio.h>

static int checks, failed;
static const char *group;
static void check(bool ok, const char *what) {
    checks++;
    if (!ok) { failed++; printf("FAIL line-breaking %s: %s\n", group, what); }
}
static void near(double got, double want, const char *what) {
    char text[256];
    snprintf(text, sizeof text, "%s = %g, want %g", what, got, want);
    check(fabs(got - want) < 0.01, text);
}

/* reached only from properties these cases do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
/* web_url.c's, reached only from a registered <url> custom property (cssprop.c's syn_url_text) */
int web_resolve_url_owned(const char *base, const char *rel, char **out) { (void)base; (void)rel; (void)out; abort(); }
void web_avmedia_checkpoint(void) {}
bool web_avmedia_snapshot_is_probe(void) { return false; }
void *thread_local_get(unsigned slot) { (void)slot; return NULL; }
unsigned parallel_worker_index(void) { return 0; }
bool css_valid_declaration(const char *p, size_t pn, const char *v, size_t vn) { (void)p; (void)pn; (void)v; (void)vn; return false; }
bool css_container_names_valid(const char *s, size_t n) { (void)s; (void)n; return false; }
void css_transform_list_matrix_3d(const struct css_transform *t, double m[16]) { (void)t; (void)m; abort(); }
void css_transform_list_matrix(const struct css_transform *t, float w, float h, double m[16]) { (void)t; (void)w; (void)h; (void)m; abort(); }
/* no links (the boxes have no nodes), no atomic inlines, no floats; abspos boxes are only listed */
node_t *doc_flat_parent(node_t *n) { (void)n; return NULL; }
static void size_atomic(box_t *c, float cbw, float cbh) { (void)c; (void)cbw; (void)cbh; abort(); }
static void outer_intrinsic(box_t *c, float *mn, float *mx) { (void)c; *mn = *mx = 0; abort(); }
static void place_float(box_t *c, box_t *parent, struct bfc *f, float ox, float oy, float y_rel, float cbh) {
    (void)c; (void)parent; (void)f; (void)ox; (void)oy; (void)y_rel; (void)cbh; abort();
}
static void position_float(box_t *c, float cw, struct bfc *f, float ox, float oy, float y_rel) {
    (void)c; (void)cw; (void)f; (void)ox; (void)oy; (void)y_rel; abort();
}
static bool float_fits_line(struct iline *L, box_t *c, float used) { (void)L; (void)c; (void)used; abort(); }
static int listed_abs;
static void list_abs(box_t *b) { b->gen = GEN; listed_abs++; }

/* the monospace font model */
struct font { int unused; };
static struct font mono;
font_t *font_ui(int style) { (void)style; return &mono; }
font_t *font_family(int family, int style) { (void)family; (void)style; return &mono; }
int gfx_utf8_decode(const char *s, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xe0) == 0xc0) { *cp = (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f); return 2; }
    if ((u[0] & 0xf0) == 0xe0) { *cp = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f); return 3; }
    *cp = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
    return 4;
}
static bool ignorable(uint32_t c) {
    return c == 0x00ad || (c >= 0x0300 && c <= 0x036f) || (c >= 0x200b && c <= 0x200f) || c == 0x2060 || c == 0xfeff;
}
static bool wide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3041 && c <= 0x33ff) ||
           (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xa000 && c <= 0xa4cf) ||
           (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
           (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6);
}
float font_width(font_t *f, float px, const char *s, size_t n) {
    (void)f;
    float w = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += (size_t)gfx_utf8_decode(s + i, &cp);
        w += ignorable(cp) ? 0 : wide(cp) ? px : px / 2;
    }
    return w;
}
void font_metrics(font_t *f, float px, float *ascent, float *descent, float *line_gap) {
    (void)f;
    if (ascent) *ascent = px * 0.8f;
    if (descent) *descent = px * 0.2f;
    if (line_gap) *line_gap = 0;
}
/* the test face has no OS/2 table: CSS's fallbacks (font.h), the cap height its ascent */
float font_aspect(font_t *f, int metric) {
    (void)f;
    return metric == FONT_ASPECT_CAP_HEIGHT ? 0.8f : metric == FONT_ASPECT_EX_HEIGHT || metric == FONT_ASPECT_CH_WIDTH ? 0.5f : 1;
}
bool font_has_primary(font_t *f, uint32_t cp) { (void)f; (void)cp; return true; }
/* no measurement cache: text_width measures every time */
uint64_t font_metrics_generation(void) { return 0; }
float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *s, size_t n, uint32_t color) {
    (void)c; (void)f; (void)px; (void)x; (void)y; (void)s; (void)n; (void)color; abort();
}
int gfx_char(canvas_t *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int font) {
    (void)c; (void)x; (void)y; (void)cp; (void)fg; (void)bg; (void)font; abort();
}
int gfx_font_h(int font) { (void)font; return 16; }
int gfx_codepoint_width(uint32_t cp, int font) { (void)cp; (void)font; return 8; }

static web_doc doc;
static arena_t values;
static jmp_buf values_trap;
static style_t root_style;
static uint32_t generation;
static box_t *boxes[512];
static int nboxes;

/* an element's computed style under parent with the declarations "property: value; ..." (longhands) */
static style_t *style_of(const char *decls, const style_t *parent) {
    style_t *s = ar_alloc(&values, sizeof *s);
    css_style_init(s, parent);
    css_style_finish(s, parent, false);
    char text[1024];
    snprintf(text, sizeof text, "%s", decls);
    for (char *d = strtok(text, ";"); d; d = strtok(NULL, ";")) {
        char *colon = strchr(d, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = d, *value = colon + 1;
        while (*name == ' ') name++;
        for (char *e = name + strlen(name); e > name && e[-1] == ' '; ) *--e = 0;
        const struct propdef *p = css_prop_lookup(name, strlen(name));
        struct css_anim_ctx c = {.base = s, .parent = parent, .rem = 16, .vw = 800, .vh = 600, .a = &values};
        style_t next;
        if (!p || !css_anim_parse(&c, p, value, strlen(value), false, &next, NULL)) {
            char what[256];
            snprintf(what, sizeof what, "the declaration %s:%s parses", name, value);
            check(false, what);
            continue;
        }
        *s = next;
    }
    return s;
}
static box_t *new_box(int kind, box_t *parent, style_t *st) {
    box_t *b = calloc(1, sizeof *b);
    b->kind = (uint8_t)kind;
    b->st = st;
    b->parent = parent;
    b->baseline = b->last_baseline = -1;
    b->layout_mem.trap = b->text_mem.trap = &values_trap;
    if (parent) {
        if (parent->last) parent->last->next = b; else parent->first = b;
        parent->last = b;
    }
    boxes[nboxes++] = b;
    return b;
}
static void scene(void) {
    for (int i = 0; i < nboxes; i++) { ar_free(&boxes[i]->layout_mem); free(boxes[i]); }
    nboxes = 0;
    listed_abs = 0;
    ar_free(&doc.lmem);
    memset(&doc, 0, sizeof doc);
    doc.width = 800; doc.height = 600;
    doc.lmem.trap = &values_trap;
}
/* a block container of the given content width holding inline content */
static box_t *block(const char *decls, float width) {
    box_t *b = new_box(B_BLOCK, NULL, style_of(decls, &root_style));
    b->w = width;
    b->inline_ctx = true;
    return b;
}
static box_t *span(box_t *parent, const char *decls) { return new_box(B_INLINE, parent, style_of(decls, parent->st)); }
/* an absolutely positioned box among the inline content */
static box_t *abspos(box_t *parent) {
    box_t *b = new_box(B_BLOCK, parent, style_of("position: absolute", parent->st));
    b->abspos = true;
    return b;
}
static box_t *text(box_t *parent, const char *s) {
    box_t *t = new_box(B_TEXT, parent, parent->st);
    size_t length;
    t->text = ws_process(&values, s, strlen(s), t->st, &length, NULL);
    t->len = length;
    return t;
}
static void lay(box_t *b) {
    default_layout = (struct layout_context){.document = &doc, .viewport_w = 800, .viewport_h = 600,
        .generation = ++generation};
    ar_free(&doc.lmem);
    layout_inline(b, NULL, 0, 0, -1);
}
/* the lines' text, "|" between lines: the runs on a baseline, the soft hyphens' bytes left out and the
   hyphen of a line broken at one shown */
static const char *lines_of(const box_t *b) {
    static char out[4096];
    size_t k = 0;
    bool any = false;
    float y = 0;
    for (int i = 0; i < b->nruns; i++) {
        const struct run *r = &b->runs[i];
        if (!r->s) continue;
        if (any && r->y != y && k < sizeof out - 1) out[k++] = '|';
        any = true;
        y = r->y;
        for (int j = 0; j < r->n && k < sizeof out - 8; j++) {
            if ((unsigned char)r->s[j] == 0xc2 && j + 1 < r->n && (unsigned char)r->s[j + 1] == 0xad) { j++; continue; }
            out[k++] = r->s[j];
        }
        if (r->hyphen) {
            const char *h = layout_hyphen_string(r->st);
            for (; *h && k < sizeof out - 1; h++) out[k++] = *h;
        }
    }
    out[k] = 0;
    return out;
}
static int line_count(const box_t *b) {
    int n = 0;
    float y = 0;
    for (int i = 0; i < b->nruns; i++) {
        if (!b->runs[i].s) continue;
        if (!n || b->runs[i].y != y) n++;
        y = b->runs[i].y;
    }
    return n;
}
static void lines(box_t *b, const char *want, const char *name) {
    lay(b);
    const char *got = lines_of(b);
    char what[1024];
    snprintf(what, sizeof what, "%s: lines \"%s\", want \"%s\"", name, got, want);
    check(!strcmp(got, want), what);
}
/* the x of the first run whose text starts with s */
static double run_x(const box_t *b, const char *s) {
    for (int i = 0; i < b->nruns; i++)
        if (b->runs[i].s && b->runs[i].n >= (int)strlen(s) && !strncmp(b->runs[i].s, s, strlen(s))) return b->runs[i].x;
    return NAN;
}
static void min_content(box_t *b, double want, const char *name) {
    float mn, mx;
    inline_intrinsic(b, &mn, &mx);
    char what[256];
    snprintf(what, sizeof what, "%s: min-content", name);
    near(mn, want, what);
}
/* one block of text in a style and width */
static void text_lines(const char *decls, float width, const char *s, const char *want) {
    scene();
    box_t *b = block(decls, width);
    text(b, s);
    char name[512];
    snprintf(name, sizeof name, "{%s} %gpx \"%s\"", decls, width, s);
    lines(b, want, name);
}
static void text_min(const char *decls, const char *s, double want) {
    scene();
    box_t *b = block(decls, 800);
    text(b, s);
    char name[512];
    snprintf(name, sizeof name, "{%s} \"%s\"", decls, s);
    min_content(b, want, name);
}
/* "a\tb"-like text in pre: where the run starting with after begins */
static void tab_at(const char *decls, const char *s, const char *after, double want) {
    scene();
    box_t *b = block(decls, 800);
    text(b, s);
    lay(b);
    char name[512];
    snprintf(name, sizeof name, "{%s} \"%s\": x of \"%s\"", decls, s, after);
    near(run_x(b, after), want, name);
}

#define SHY "\xc2\xad"
#define HH "\xe2\x80\x90"   /* U+2010 HYPHEN, the hyphen shown at a soft hyphen in this font */

int main(void) {
    values.trap = &values_trap;
    if (setjmp(values_trap)) { printf("FAIL line-breaking: allocation\n"); return 1; }
    css_style_init(&root_style, NULL);
    root_style.font_size = 16;
    css_style_finish(&root_style, NULL, true);

    /* hyphens-manual-inline-011.html (32px monospace, 10ch), hyphens-none-shy-on-2nd-line-001.html,
       hyphenate-character-003.html (5.5ch, "\2022"); a soft hyphen is invisible and zero-width */
    group = "hyphens";
    {
        scene();
        box_t *b = block("font-size: 32px", 160);
        text(b, "DNA ");
        box_t *s = span(b, "hyphens: manual");
        text(s, "means Deoxy" SHY "ribo" SHY "nucleic acid");
        text(b, ".");
        lines(b, "DNA means|Deoxyribo" HH "|nucleic|acid.", "hyphens-manual-inline-011");
        scene();
        b = block("hyphens: none", 80);
        text(b, "\n  12345678\n  Deoxy" SHY "ribo" SHY "nucleic\n  Deoxy" SHY "ribo" SHY "nucleic\n  12345678\n");
        lines(b, "12345678|Deoxyribonucleic|Deoxyribonucleic|12345678", "hyphens-none-shy-on-2nd-line-001");
        text_lines("hyphens: manual", 80, "Deoxy" SHY "ribo" SHY "nucleic", "Deoxyribo" HH "|nucleic");
        text_lines("hyphens: manual", 72, "Deoxy" SHY "ribo" SHY "nucleic", "Deoxy" HH "|ribo" HH "|nucleic");
        /* hyphens: auto with no hyphenation resource hyphenates where soft hyphens are (§6.3.1) */
        text_lines("hyphens: auto", 72, "Deoxy" SHY "ribo" SHY "nucleic", "Deoxy" HH "|ribo" HH "|nucleic");
        text_lines("hyphens: manual; hyphenate-character: \"\\2022\"", 44,
                   "im" SHY "ple" SHY "men" SHY "ta" SHY "tion", "im\xe2\x80\xa2|ple\xe2\x80\xa2|men\xe2\x80\xa2|ta\xe2\x80\xa2|tion");
        text_lines("hyphens: manual; hyphenate-character: \"\\2022\"", 44,
                   "re" SHY "al" SHY "iza" SHY "tion", "real\xe2\x80\xa2|iza\xe2\x80\xa2|tion");
        text_lines("hyphens: manual; hyphenate-character: \"-\"", 48, "high" SHY "way", "high-|way");
        /* an empty hyphenate-character: the break shows nothing */
        text_lines("hyphens: manual; hyphenate-character: \"\"", 48, "high" SHY "way", "high|way");
        /* unbroken, the word is as wide as without its soft hyphen */
        scene();
        b = block("hyphens: manual", 800);
        text(b, "high" SHY "way");
        lay(b);
        check(!strcmp(lines_of(b), "highway"), "an unbroken soft hyphen shows nothing");
        float w = 0;
        for (int i = 0; i < b->nruns; i++) w += b->runs[i].w;
        near(w, 56, "the width of high&shy;way on one line");
        /* "high‐" is 40px, "highway" 56px: the word stays whole where it fits, and breaks at its
           only opportunity where nothing fits */
        text_lines("hyphens: manual", 56, "high" SHY "way", "highway");
        text_lines("hyphens: manual", 55, "high" SHY "way", "high" HH "|way");
        text_lines("hyphens: manual", 40, "high" SHY "way", "high" HH "|way");
        text_lines("hyphens: manual", 39, "high" SHY "way", "high" HH "|way");
    }

    /* hyphens-span-001.html, hyphens-out-of-flow-001.html: a word continues across inline element
       boundaries and out-of-flow boxes, and so does its soft hyphen (6ch) */
    group = "hyphens across boxes";
    {
        static const char *const pieces[][3] = {
            {"high" SHY "way", NULL, NULL}, {"", "high", SHY "way"}, {"", "high" SHY, "way"},
            {"high", SHY, "way"}, {"high", SHY "way", ""}, {"high" SHY, "way", ""}, {"high" SHY "wa", NULL, "y"},
            {"", "hi", "gh" SHY "way"},
        };
        for (size_t i = 0; i < sizeof pieces / sizeof *pieces; i++) {
            scene();
            box_t *b = block("hyphens: manual", 48);
            if (pieces[i][1]) {
                if (*pieces[i][0]) text(b, pieces[i][0]);
                text(span(b, ""), pieces[i][1]);
                if (*pieces[i][2]) text(b, pieces[i][2]);
            } else if (pieces[i][2]) {
                text(span(b, ""), pieces[i][0]);
                text(b, pieces[i][2]);
            } else text(b, pieces[i][0]);
            char name[64];
            snprintf(name, sizeof name, "hyphens-span-001 #%zu", i + 1);
            lines(b, "high" HH "|way", name);
        }
        /* <span>high</span>&shy;<span>way</span> */
        scene();
        box_t *b = block("hyphens: manual", 48);
        text(span(b, ""), "high");
        text(b, SHY);
        text(span(b, ""), "way");
        lines(b, "high" HH "|way", "hyphens-span-001 #10");
        static const char *const around[][2] = {
            {"", "high" SHY "way"}, {"h", "igh" SHY "way"}, {"high", SHY "way"}, {"high" SHY, "way"},
            {"high" SHY "wa", "y"}, {"high" SHY "way", ""},
        };
        for (size_t i = 0; i < sizeof around / sizeof *around; i++) {
            scene();
            b = block("hyphens: manual", 48);
            if (*around[i][0]) text(b, around[i][0]);
            abspos(b);
            if (*around[i][1]) text(b, around[i][1]);
            char name[64];
            snprintf(name, sizeof name, "hyphens-out-of-flow-001 #%zu", i + 2);
            lines(b, "high" HH "|way", name);
            check(listed_abs == 1, "the abspos box is listed once");
        }
        /* hyphens of the soft hyphen's own style: none in a span turns that one off */
        scene();
        b = block("hyphens: manual", 48);
        text(b, "high");
        text(span(b, "hyphens: none"), SHY);
        text(b, "way");
        lines(b, "highway", "a soft hyphen with hyphens: none");
    }

    /* hyphenate-limit-chars (§6.3.4) over soft hyphens: the word's letters, those before and after */
    group = "hyphenate-limit-chars";
    {
        text_lines("hyphens: manual; hyphenate-limit-chars: 6 3 3", 48, "high" SHY "way", "high" HH "|way");
        text_lines("hyphens: manual; hyphenate-limit-chars: 7", 48, "high" SHY "way", "high" HH "|way");
        text_lines("hyphens: manual; hyphenate-limit-chars: 8", 48, "high" SHY "way", "highway");
        text_lines("hyphens: manual; hyphenate-limit-chars: auto 5", 48, "high" SHY "way", "highway");
        text_lines("hyphens: manual; hyphenate-limit-chars: auto 4 3", 48, "high" SHY "way", "high" HH "|way");
        text_lines("hyphens: manual; hyphenate-limit-chars: auto auto 4", 48, "high" SHY "way", "highway");
        /* punctuation does not count */
        text_lines("hyphens: manual; hyphenate-limit-chars: auto auto 3", 48, "high" SHY "wa.", "highwa.");
        /* the word spans inline boxes */
        scene();
        box_t *b = block("hyphens: manual; hyphenate-limit-chars: auto 5", 48);
        text(b, "high" SHY);
        text(span(b, ""), "way");
        lines(b, "highway", "the limit over a word across boxes");
        scene();
        b = block("hyphens: manual; hyphenate-limit-chars: auto 4 3", 48);
        text(span(b, ""), "hi");
        text(b, "gh" SHY "way");
        lines(b, "high" HH "|way", "letters before the soft hyphen in another box");
    }

    /* hyphenate-limit-lines (§6.3.5): successive lines ending at a soft hyphen */
    group = "hyphenate-limit-lines";
    {
        const char *s = "ab" SHY "cd" SHY "ef" SHY "gh";
        text_lines("hyphens: manual", 24, s, "ab" HH "|cd" HH "|ef" HH "|gh");
        text_lines("hyphens: manual; hyphenate-limit-lines: no-limit", 24, s, "ab" HH "|cd" HH "|ef" HH "|gh");
        text_lines("hyphens: manual; hyphenate-limit-lines: 2", 24, s, "ab" HH "|cd" HH "|efgh");
        text_lines("hyphens: manual; hyphenate-limit-lines: 1", 24, s, "ab" HH "|cdefgh");
        /* a line that ends otherwise starts the count again */
        text_lines("hyphens: manual; hyphenate-limit-lines: 1", 24, "ab" SHY "cd ef" SHY "gh", "ab" HH "|cd|ef" HH "|gh");
    }

    /* line-break-{normal,strict,loose}-hyphens-002.html (2ch, break-all), line-break-anywhere-001.html,
       and the CJK rules of §6.2 */
    group = "line-break";
    {
        text_lines("line-break: normal; word-break: break-all", 16, "aa" HH "a", "a|a" HH "|a");
        text_lines("line-break: normal; word-break: break-all", 16, "aa\xe2\x80\x93" "a", "a|a\xe2\x80\x93|a");
        text_lines("line-break: strict; word-break: break-all", 16, "aa" HH "a", "a|a" HH "|a");
        text_lines("line-break: loose; word-break: break-all", 16, "aa" HH "a", "aa|" HH "a");
        text_lines("line-break: loose; word-break: break-all", 16, "aa\xe2\x80\x93" "a", "aa|\xe2\x80\x93" "a");
        /* small kana: normal does not break before them, loose does */
        text_lines("line-break: normal", 32, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x81\xe3\x81\x86",
                   "\xe3\x81\x82|\xe3\x81\x84\xe3\x81\x81|\xe3\x81\x86");
        text_lines("line-break: strict", 32, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x81\xe3\x81\x86",
                   "\xe3\x81\x82|\xe3\x81\x84\xe3\x81\x81|\xe3\x81\x86");
        text_lines("line-break: loose", 32, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x81\xe3\x81\x86",
                   "\xe3\x81\x82\xe3\x81\x84|\xe3\x81\x81\xe3\x81\x86");
        text_lines("line-break: auto", 32, "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x81\xe3\x81\x86",
                   "\xe3\x81\x82|\xe3\x81\x84\xe3\x81\x81|\xe3\x81\x86");
        /* the iteration mark 々 likewise */
        text_lines("line-break: normal", 32, "\xe6\x97\xa5\xe6\x97\xa5\xe3\x80\x85\xe6\x97\xa5",
                   "\xe6\x97\xa5|\xe6\x97\xa5\xe3\x80\x85|\xe6\x97\xa5");
        text_lines("line-break: loose", 32, "\xe6\x97\xa5\xe6\x97\xa5\xe3\x80\x85\xe6\x97\xa5",
                   "\xe6\x97\xa5\xe6\x97\xa5|\xe3\x80\x85\xe6\x97\xa5");
        /* 〜 U+301C: normal and loose break before it, strict does not */
        text_lines("line-break: normal", 16, "\xe3\x81\x82\xe3\x80\x9c", "\xe3\x81\x82|\xe3\x80\x9c");
        text_lines("line-break: loose", 16, "\xe3\x81\x82\xe3\x80\x9c", "\xe3\x81\x82|\xe3\x80\x9c");
        text_lines("line-break: strict", 16, "\xe3\x81\x82\xe3\x80\x9c", "\xe3\x81\x82\xe3\x80\x9c");
        /* closing punctuation never starts a line, but in anywhere */
        text_lines("line-break: loose", 32, "\xe6\x97\xa5\xe6\x97\xa5\xe3\x80\x82", "\xe6\x97\xa5|\xe6\x97\xa5\xe3\x80\x82");
        text_lines("line-break: anywhere", 32, "\xe6\x97\xa5\xe6\x97\xa5\xe3\x80\x82", "\xe6\x97\xa5\xe6\x97\xa5|\xe3\x80\x82");
        /* anywhere: every typographic character unit, punctuation and GL, WJ, ZWJ too */
        scene();
        box_t *b = block("line-break: anywhere", 8);
        text(b, "aa-a.a)a,a\xef\xbc\x89" "a");
        text(span(b, ""), "\xc2\xa0");
        text(b, "a\xef\xbb\xbf" "a\xe2\x81\xa0" "a\xe2\x80\x8d" "a\xef\xbd\xa5" "a");
        lay(b);
        char what[128];
        snprintf(what, sizeof what, "line-break-anywhere-001: %d lines, want 19", line_count(b));
        check(line_count(b) == 19, what);
        /* the zero-width U+FEFF and U+2060 fit after the letter before them; U+200D extends it */
        const char *want = "a|a|-|a|.|a|)|a|,|a|\xef\xbc\x89|a|\xc2\xa0|a\xef\xbb\xbf|a\xe2\x81\xa0|a\xe2\x80\x8d|a|\xef\xbd\xa5|a";
        snprintf(what, sizeof what, "line-break-anywhere-001 lines \"%s\"", lines_of(b));
        check(!strcmp(lines_of(b), want), what);
        /* in nowrap, anywhere breaks nothing */
        text_lines("line-break: anywhere; white-space: nowrap", 8, "aaa", "aaa");
    }

    /* word-break: §6.1; word-break-break-all-001, -keep-all-001, the Southeast Asian fallback
       (line-breaking-021 Thai), break-word */
    group = "word-break";
    {
        text_lines("word-break: normal", 179, "Latin latin latin latin", "Latin latin latin|latin");
        text_lines("word-break: break-all", 179, "Latin latin latin latin", "Latin latin latin lati|n");
        text_lines("word-break: keep-all", 179, "Latin latin latin latin", "Latin latin latin|latin");
        const char *cjk = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
        text_lines("word-break: normal", 40, cjk, "\xe6\x97\xa5\xe6\x9c\xac|\xe8\xaa\x9e\xe6\x97\xa5|\xe6\x9c\xac\xe8\xaa\x9e");
        text_lines("word-break: break-all", 40, cjk, "\xe6\x97\xa5\xe6\x9c\xac|\xe8\xaa\x9e\xe6\x97\xa5|\xe6\x9c\xac\xe8\xaa\x9e");
        text_lines("word-break: keep-all", 40, cjk, cjk);
        text_lines("word-break: auto-phrase", 40, cjk, "\xe6\x97\xa5\xe6\x9c\xac|\xe8\xaa\x9e\xe6\x97\xa5|\xe6\x9c\xac\xe8\xaa\x9e");
        /* keep-all: punctuation between letters still breaks; a space does */
        text_lines("word-break: keep-all", 40, "\xe6\x97\xa5\xe6\x9c\xac\xe3\x80\x81\xe8\xaa\x9e\xe6\x97\xa5",
                   "\xe6\x97\xa5\xe6\x9c\xac\xe3\x80\x81|\xe8\xaa\x9e\xe6\x97\xa5");
        /* word-break-keep-all-u002d.html: keep-all as normal around U+002D (3ch at 20px) */
        text_lines("font-size: 20px; word-break: keep-all", 30, "AB-CD-EF", "AB-|CD-|EF");
        text_lines("font-size: 20px", 30, "AB-CD-EF", "AB-|CD-|EF");
        text_lines("font-size: 20px; word-break: keep-all", 30, "12-34-56", "12-34-56");
        text_lines("font-size: 20px", 30, "12-34-56", "12-34-56");
        /* Southeast Asian scripts with no dictionary break between their letters, but in manual */
        const char *thai = "\xe0\xb8\x81\xe0\xb8\x82\xe0\xb8\x84\xe0\xb8\x87\xe0\xb8\x88\xe0\xb8\x89\xe0\xb8\x8a";
        text_lines("word-break: normal", 24, thai, "\xe0\xb8\x81\xe0\xb8\x82\xe0\xb8\x84|\xe0\xb8\x87\xe0\xb8\x88\xe0\xb8\x89|\xe0\xb8\x8a");
        text_lines("word-break: manual", 24, thai, thai);
        text_lines("word-break: keep-all", 24, thai, "\xe0\xb8\x81\xe0\xb8\x82\xe0\xb8\x84|\xe0\xb8\x87\xe0\xb8\x88\xe0\xb8\x89|\xe0\xb8\x8a");
        /* break-word: as normal, with overflow-wrap: anywhere's breaks */
        text_lines("word-break: break-word", 24, "aaaaaaaa", "aaa|aaa|aa");
        text_lines("word-break: break-word", 32, "aa bbbbbbb", "aa|bbbb|bbb");
        /* break-all does not hyphenate */
        text_lines("hyphens: manual; word-break: break-all", 40, "high" SHY "way", "highw|ay");
    }

    /* overflow-wrap (and its alias word-wrap): §6.4 */
    group = "overflow-wrap";
    {
        text_lines("overflow-wrap: normal", 32, "Supercali", "Supercali");
        text_lines("overflow-wrap: break-word", 32, "Supercali", "Supe|rcal|i");
        text_lines("overflow-wrap: anywhere", 32, "Supercali", "Supe|rcal|i");
        text_lines("word-wrap: break-word", 32, "Supercali", "Supe|rcal|i");
        text_lines("overflow-wrap: break-word", 32, "aa bbbbbbb", "aa|bbbb|bbb");
        text_lines("overflow-wrap: break-word; white-space: nowrap", 32, "aa bbbbbbb", "aa bbbbbbb");
        text_lines("overflow-wrap: break-word; white-space: pre", 32, "aa bbbbbbb", "aa bbbbbbb");
        text_lines("overflow-wrap: break-word; white-space: pre-wrap", 32, "aa bbbbbbb", "aa|bbbb|bbb");
        /* the emergency break keeps grapheme clusters: e + U+0301 */
        text_lines("overflow-wrap: anywhere", 16, "ae\xcc\x81" "e", "ae\xcc\x81|e");
        /* the emergency break shows no hyphen */
        text_lines("overflow-wrap: anywhere; hyphens: manual", 32, "Supercali", "Supe|rcal|i");
    }

    /* min-content contributions: overflow-wrap-min-content-size-001..003, word-break-min-content-001 */
    group = "min-content";
    {
        text_min("", "aaa bbbbb", 40);
        text_min("overflow-wrap: break-word", "aaa bbbbb", 40);
        text_min("overflow-wrap: anywhere", "aaa bbbbb", 8);
        text_min("word-break: break-word", "aaa bbbbb", 8);
        text_min("line-break: anywhere", "aaa bbbbb", 8);
        text_min("word-break: break-all", "aaa bbbbb", 8);
        text_min("word-break: keep-all", "\xe6\x97\xa5\xe6\x9c\xac \xe8\xaa\x9e", 32);
        text_min("", "\xe6\x97\xa5\xe6\x9c\xac \xe8\xaa\x9e", 16);
        text_min("hyphens: manual", "high" SHY "way", 40);
        text_min("hyphens: auto", "high" SHY "way", 40);
        text_min("hyphens: none", "high" SHY "way", 56);
        text_min("overflow-wrap: anywhere; white-space: nowrap", "aaa bbbbb", 72);
    }

    /* tab-size (§4.1.3, tab-size): tab stops from the block's start content edge, multiples of tab-size
       spaces of the block (with letter-spacing and word-spacing) or a length, at least 0.5ch on */
    group = "tab-size";
    {
        tab_at("white-space: pre", "a\tb", "b", 64);
        tab_at("white-space: pre; tab-size: 4", "a\tb", "b", 32);
        tab_at("white-space: pre; tab-size: 0", "a\tb", "b", 8);
        tab_at("white-space: pre; tab-size: 20px", "a\tb", "b", 20);
        tab_at("white-space: pre; tab-size: 20px", "aa\tb", "b", 20);
        tab_at("white-space: pre; tab-size: 20px", "aaa\tb", "b", 40);
        /* tab-min-rendered-width-1: less than 0.5ch to the stop takes the next one */
        tab_at("white-space: pre; tab-size: 4", "aaa\tb", "b", 32);
        tab_at("white-space: pre; tab-size: 4", "aaaa\tb", "b", 64);
        tab_at("white-space: pre; tab-size: 18px", "aa\tb", "b", 36);
        tab_at("white-space: pre; tab-size: 4", "a\t\tb", "b", 64);
        tab_at("white-space: pre-wrap; tab-size: 4", "a\tb", "b", 32);
        tab_at("white-space: break-spaces; tab-size: 4", "a\tb", "b", 32);
        /* tabs collapse to spaces where white-space collapses */
        tab_at("tab-size: 4", "a\tb", "b", 16);
        /* text-indent moves the content, not the stops */
        tab_at("white-space: pre; tab-size: 4; text-indent: 16px", "a\tb", "b", 32);
        /* tab-size-spacing-001: the space with its letter-spacing and word-spacing */
        tab_at("white-space: pre; letter-spacing: 2px; word-spacing: 10px", "\tb", "b", 160);
        tab_at("white-space: pre; tab-size: 2; word-spacing: 8px", "a\tb", "b", 32);
        /* tab-size-block-ancestor: the space of the nearest block container */
        static const char *const ancestor[] = {"font-size: 1px", "font-size: 10px"};
        for (int i = 0; i < 2; i++) {
            scene();
            box_t *b = block("white-space: pre; tab-size: 10", 800);
            text(span(b, ancestor[i]), "\tA");
            lay(b);
            char name[64];
            snprintf(name, sizeof name, "tab-size-block-ancestor {%s}", ancestor[i]);
            near(run_x(b, "A"), 80, name);
        }
        /* tab-size-inline-001: tab-size of the tab's own style */
        scene();
        box_t *b = block("white-space: pre; tab-size: 2", 800);
        text(span(b, "tab-size: 5"), "\t\tA");
        lay(b);
        near(run_x(b, "A"), 80, "tab-size-inline-001 t5");
        scene();
        b = block("white-space: pre; tab-size: 2", 800);
        text(span(b, "tab-size: 10"), "\tA");
        lay(b);
        near(run_x(b, "A"), 80, "tab-size-inline-001 t10");
        /* a tab is a soft wrap opportunity after it in pre-wrap; on a new line it reaches that line's stops */
        text_lines("white-space: pre-wrap", 48, "aaaa\tbbbb", "aaaa\t|bbbb");
        scene();
        b = block("white-space: pre-wrap; tab-size: 2", 48);
        text(b, "aaaa bb\tb");
        lay(b);
        check(!strcmp(lines_of(b), "aaaa|bb\tb"), "a tab moved to the next line");
        near(b->runs[b->nruns - 1].x, 32, "its stop on the new line");
        /* min-content: the tab's advance in its line */
        scene();
        b = block("white-space: pre-wrap; tab-size: 4", 800);
        text(b, "a\tb");
        min_content(b, 32, "the tab in min-content");
    }

    /* UAX #14 classes the rules tell apart */
    group = "line breaking classes";
    {
        /* LB21 × HY, LB25 HY × NU, LB20a */
        text_lines("", 48, "ABCD-EFGH", "ABCD-|EFGH");
        text_lines("", 48, "ABCD-1234", "ABCD-1234");
        text_lines("", 48, "1234-5678", "1234-5678");
        text_lines("", 24, "x -ab", "x|-ab");
        text_lines("", 24, "x -13", "x|-13");
        /* B2 — B2 */
        text_lines("", 32, "aaa\xe2\x80\x94" "bbb", "aaa\xe2\x80\x94|bbb");
        text_lines("", 40, "aaa\xe2\x80\x94\xe2\x80\x94" "bbb", "aaa\xe2\x80\x94\xe2\x80\x94|bbb");
        text_lines("", 32, "aaa\xe2\x80\x94\xe2\x80\x94" "bbb", "aaa|\xe2\x80\x94\xe2\x80\x94|bbb");
        /* ZW */
        text_lines("", 32, "aaa\xe2\x80\x8b" "bbb", "aaa\xe2\x80\x8b|bbb");
        /* GL and WJ do not break */
        text_lines("", 32, "aaa\xc2\xa0" "bbb", "aaa\xc2\xa0" "bbb");
        text_lines("", 32, "aaa\xe2\x81\xa0" "bbb", "aaa\xe2\x81\xa0" "bbb");
        /* opening and closing punctuation of CJK text */
        text_lines("", 32, "\xe6\x97\xa5\xe3\x80\x8c\xe6\x97\xa5\xe3\x80\x8d",
                   "\xe6\x97\xa5|\xe3\x80\x8c\xe6\x97\xa5\xe3\x80\x8d");
        /* the break between words in different inline boxes: <span>日</span>日 */
        scene();
        box_t *b = block("", 16);
        text(span(b, ""), "\xe6\x97\xa5");
        text(b, "\xe6\x97\xa5");
        lines(b, "\xe6\x97\xa5|\xe6\x97\xa5", "a CJK break at an element boundary");
        scene();
        b = block("", 8);
        text(span(b, ""), "a");
        text(b, "b");
        lines(b, "ab", "no break within a Latin word at an element boundary");
        /* the white-space of the two characters' nearest common ancestor decides (css-text-3 §5) */
        scene();
        b = block("", 16);
        text(span(b, "white-space: nowrap"), "\xe6\x97\xa5\xe6\x97\xa5");
        text(b, "\xe6\x97\xa5");
        lines(b, "\xe6\x97\xa5\xe6\x97\xa5|\xe6\x97\xa5", "nowrap inside, normal around an element boundary");
        scene();
        b = block("white-space: nowrap", 16);
        text(span(b, "white-space: normal"), "\xe6\x97\xa5");
        text(b, "\xe6\x97\xa5");
        lines(b, "\xe6\x97\xa5\xe6\x97\xa5", "normal inside, nowrap around an element boundary");
        /* <wbr> */
        scene();
        b = block("", 32);
        text(b, "aaa");
        box_t *wbr = new_box(B_BR, b, b->st);
        node_t wbr_node = {.type = N_ELEM, .tag = T_wbr};
        wbr->node = &wbr_node;
        text(b, "bbb");
        lines(b, "aaa|bbb", "<wbr>");
        scene();
        b = block("white-space: nowrap", 32);
        text(b, "aaa");
        wbr = new_box(B_BR, b, b->st);
        wbr->node = &wbr_node;
        text(b, "bbb");
        lines(b, "aaabbb", "<wbr> in nowrap");
    }

    printf("line-breaking: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
