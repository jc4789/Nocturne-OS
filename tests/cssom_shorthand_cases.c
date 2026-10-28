/* CSSOM's shorthands in a declaration block on the host, through an element's
   inline style as js.c drives it: the block it holds for the element, cssText
   (its serialization, CSSOM "serialize a CSS declaration block"), getPropertyValue()
   and getPropertyPriority() (a shorthand from its longhands), setProperty() and
   removeProperty() (a shorthand sets and removes its longhands, css.c's
   css_inline_set), the declarations' names (the CSSStyleDeclaration's items)
   and the cssText setter (css_inline_replace). tests/check_cssom_rules_native.py
   builds this with css.c, cssprop.c and the DOM. Each case names the WPT page it
   reads (css/cssom unless named). Built with OLD_SOURCE against the source before
   the block held longhands (a block was its serialization, a shorthand one
   declaration), the same cases run on what that source has. */
#include "webi.h"
#include <stdio.h>
static int passed, failed;
static void check(const char *label, const char *got, const char *want) {
    bool ok = got && want ? !strcmp(got, want) : got == want;
    if (ok) passed++;
    else { failed++; printf("FAIL %s\n  got:  %s\n  want: %s\n", label, got ? got : "(null)", want ? want : "(null)"); }
}

/* ---- an element with a style attribute */
struct styled { node_t e; struct attr a; char *value; };
static void attribute(struct styled *s, const char *text, size_t n) {
    free(s->value);
    s->value = strndup(text ? text : "", text ? n : 0);
    s->a = (struct attr){.name = "style", .raw = "style", .local = "style", .value = s->value};
    s->e.type = N_ELEM; s->e.attrs = &s->a; s->e.nattrs = 1;
}
static struct styled *element(const char *style) {
    struct styled *s = calloc(1, sizeof *s);
    attribute(s, style, strlen(style));
    return s;
}
/* the block js.c holds for the element's inline style (style_memo_get) */
static void block_of(struct styled *s, sbuf *block) {
#ifndef OLD_SOURCE
    const char *own = css_inline_block(&s->e);
    if (own) { sb_puts(block, own); return; }
#endif
    const char *css = node_attr(&s->e, "style");
    if (!css) css = "";
    size_t n;
    char *clean = css_preprocess(css, strlen(css), &n);
#ifndef OLD_SOURCE
    css_decl_block(clean, n, NULL, block);
#else
    css_decl_serialize(clean, n, NULL, block, NULL);
#endif
    free(clean);
}
/* cssText, and the names as "a,b,c" */
static char *css_text(struct styled *s, bool names) {
    sbuf block = {0}, text = {0}, list = {0};
    block_of(s, &block);
#ifndef OLD_SOURCE
    css_decl_block_serialize(block.p ? block.p : "", block.n, NULL, &text, &list);
#else
    sb_put(&text, block.p ? block.p : "", block.n);
    css_decl_names(block.p ? block.p : "", block.n, &list);
#endif
    for (size_t i = 0; i < list.n; i++) if (!list.p[i]) list.p[i] = i + 1 < list.n ? ',' : 0;
    sb_free(&block);
    if (names) { sb_free(&text); return list.p ? list.p : strdup(""); }
    sb_free(&list);
    return sb_cstr(&text);
}
static char *names_of(struct styled *s) { return css_text(s, true); }
/* getPropertyValue(), or with priority getPropertyPriority() */
static char *get(struct styled *s, const char *property, bool priority) {
    sbuf block = {0}, value = {0};
    bool important = false;
    block_of(s, &block);
#ifndef OLD_SOURCE
    css_decl_get(block.p ? block.p : "", block.n, property, NULL, &value, &important);
#else
    const char *v; size_t vn;
    if (css_decl_find(block.p ? block.p : "", block.n, property, &v, &vn, &important)) sb_put(&value, v, vn);
#endif
    sb_free(&block);
    if (priority) { sb_free(&value); return strdup(important ? "important" : ""); }
    return sb_cstr(&value);
}
static void set(struct styled *s, const char *property, const char *value, const char *priority) {
    sbuf block = {0}, written = {0};
    block_of(s, &block);
    if (css_inline_set(NULL, &s->e, block.p ? block.p : "", block.n, property, value, priority, &written) > 0)
        attribute(s, written.p, written.n);
    sb_free(&block); sb_free(&written);
}
static void set_text(struct styled *s, const char *text) {
    sbuf written = {0};
    if (css_inline_replace(NULL, &s->e, text, strlen(text), &written)) attribute(s, written.p, written.n);
    sb_free(&written);
}
static char *round_trip(const char *text) { struct styled *s = element(""); set_text(s, text); return css_text(s, false); }

static void serialization(void) {
    /* shorthand-values.html: cssText = specified, cssText */
    static const char *const values[][2] = {
        {"border: 1px; border-top: 1px;", "border: 1px;"},
        {"border: 1px solid red;", "border: 1px solid red;"},
        {"border: 1px red;", "border: 1px red;"},
        {"border: red;", "border: red;"},
        {"border-top: 1px; border-right: 2px; border-bottom: 3px; border-left: 4px;",
         "border-width: 1px 2px 3px 4px; border-style: none; border-color: currentcolor;"},
        {"border: solid; border-style: dotted", "border: dotted;"},
        {"border-width: 1px;", "border-width: 1px;"},
        {"overflow-x: scroll; overflow-y: hidden;", "overflow: scroll hidden;"},
        {"overflow-x: scroll; overflow-y: scroll;", "overflow: scroll;"},
        {"outline-width: 2px; outline-style: dotted; outline-color: blue;", "outline: blue dotted 2px;"},
        {"margin-top: 1px; margin-right: 2px; margin-bottom: 3px; margin-left: 4px;", "margin: 1px 2px 3px 4px;"},
        {"list-style-type: lower-alpha;", "list-style-type: lower-alpha;"},
        {"padding-top: 1px; padding-right: 2px; padding-bottom: 3px; padding-left: 4px;", "padding: 1px 2px 3px 4px;"},
        /* shorthand-serialization.html */
        {"margin-right: 10px; margin-left: 10px; margin-top: 10px; margin-bottom: 10px;", "margin: 10px;"},
        {"margin-right: 10px; margin-left: 10px; margin-top: 10px; margin-bottom: 10px!important;",
         "margin-right: 10px; margin-left: 10px; margin-top: 10px; margin-bottom: 10px !important;"},
        {"margin-right: 10px !important; margin-left: 10px !important; margin-top: 10px !important; margin-bottom: 10px!important;",
         "margin: 10px !important;"},
        {"margin: initial;", "margin: initial;"},
        /* font-shorthand-serialization.html */
        {"font: 10px/1 Ahem;", "font: 10px / 1 Ahem;"},
        /* style-border-shorthand-var.html, shorthand-serialization.html: a var() value stays whole */
        {"border: 0.1rem solid var(--my-color-value)", "border: 0.1rem solid var(--my-color-value);"},
        {"background:var(--a);", "background: var(--a);"},
        /* css-backgrounds/parsing/border-valid.html (the canonical order: width, style, colour) */
        {"border: green double thin", "border: thin double green;"},
        {"border-bottom: green", "border-bottom: green;"},
        /* css-ui/parsing/outline-valid.html (colour, style, width) */
        {"outline: 3px ridge rgba(10, 20, 30, 0.4)", "outline: rgba(10, 20, 30, 0.4) ridge 3px;"},
        /* css-typed-om properties/logical.html: a border side all of whose values are initial */
        {"border-top: none", "border-top: none;"},
    };
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        char label[160];
        snprintf(label, sizeof label, "cssText: %s", values[i][0]);
        check(label, round_trip(values[i][0]), values[i][1]);
    }
    /* shorthand-serialization.html: a shorthand between longhands of other importances */
    struct styled *s = element("background-color: blue; background: red !important; background-color: green;");
    char *text = css_text(s, false);
    check("cssText: an important shorthand over longhands ends important",
          text && strlen(text) > 11 && !strcmp(text + strlen(text) - 11, "!important;") ? "yes" : text, "yes");
    s = element("background-color: blue; background: red; background-color: green !important;");
    char *background = get(s, "background", false);
    check("getPropertyValue: a shorthand of mixed importance is empty", background, "");
}

static void get_and_set(void) {
    /* shorthand-serialization.html */
    struct styled *s = element("margin-right: 10px; margin-left: 10px; margin-top: 10px; margin-bottom: 10px;");
    check("getPropertyValue: margin from its longhands", get(s, "margin", false), "10px");
    s = element("margin-right: 10px; margin-left: 10px; margin-top: 10px; margin-bottom: 10px!important;");
    check("getPropertyValue: longhands of two importances", get(s, "margin", false), "");
    s = element("margin-right: 10px !important; margin-left: 10px !important; margin-top: 10px !important; margin-bottom: 10px!important;");
    check("getPropertyValue: important longhands", get(s, "margin", false), "10px");
    check("getPropertyPriority: every longhand important", get(s, "margin", true), "important");
    s = element("background:var(--a);");
    check("getPropertyValue: a var() shorthand", get(s, "background", false), "var(--a)");
    check("getPropertyValue: a longhand of a var() shorthand", get(s, "background-position", false), "");
    s = element("");
    set(s, "margin", "20px 20px 20px 20px", NULL);
    check("setProperty: margin, then margin", get(s, "margin", false), "20px");
    check("setProperty: margin, then cssText", css_text(s, false), "margin: 20px;");
    set_text(s, "margin: initial;");
    check("cssText: margin: initial, margin", get(s, "margin", false), "initial");
    set(s, "margin-top", "initial", "important");
    check("setProperty: one important initial longhand", get(s, "margin", false), "");
    set_text(s, "");
    set(s, "margin-top", "initial", NULL);
    set(s, "margin-right", "initial", NULL);
    set(s, "margin-bottom", "initial", NULL);
    set(s, "margin-left", "initial", "important");
    check("setProperty: initial longhands one by one, one important", get(s, "margin", false), "");

    /* cssstyledeclaration-setter-declarations.html: setProperty() of a shorthand sets its longhands */
    s = element("margin: 1px; top: 2px; padding: 3px !important;");
    check("names: a shorthand's longhands", names_of(s),
          "margin-top,margin-right,margin-bottom,margin-left,top,padding-top,padding-right,padding-bottom,padding-left");
    set(s, "margin", "4px", NULL);
    check("setProperty: an existing shorthand, a longhand", get(s, "margin-left", false), "4px");
    check("setProperty: an existing shorthand, the others stay", get(s, "padding-top", true), "important");
    set(s, "padding", "5px", NULL);
    check("setProperty: a shorthand's priority", get(s, "padding-bottom", true), "");
    check("setProperty: a shorthand, cssText", css_text(s, false), "margin: 4px; top: 2px; padding: 5px;");

    /* style-border-shorthand-var.html */
    s = element("");
    set(s, "border", "0.1rem solid var(--my-color-value)", NULL);
    check("var(): border", get(s, "border", false), "0.1rem solid var(--my-color-value)");
    check("var(): border-width", get(s, "border-width", false), "");
    set(s, "border-width", "0.2rem", NULL);
    check("var(): border after border-width", get(s, "border", false), "");
    check("var(): border-width after border-width", get(s, "border-width", false), "0.2rem");

    /* cssom-setProperty-shorthand.html: set to initial, remove, with and without !important */
    static const char *const shorthands[] = {"font", "border-top", "border-right", "border-bottom", "border-left",
        "border", "border-color", "border-style", "border-width", "list-style", "margin", "outline", "padding",
        "background", "overflow", "border-radius"};
    s = element("");
    for (size_t i = 0; i < sizeof shorthands / sizeof *shorthands; i++) {
        for (int important = 0; important < 2; important++) {
            char label[96];
            set(s, shorthands[i], "initial", important ? "important" : "");
            snprintf(label, sizeof label, "setProperty: %s initial%s", shorthands[i], important ? " !important" : "");
            check(label, get(s, shorthands[i], false), "initial");
            set(s, shorthands[i], "", NULL);
            snprintf(label, sizeof label, "removeProperty: %s%s", shorthands[i], important ? " !important" : "");
            char *after = get(s, shorthands[i], false);
            check(label, after && strcmp(after, "initial") ? "removed" : after, "removed");
        }
    }
    s = element("margin: 1px; color: red");
    set(s, "margin", "", NULL);
    check("removeProperty: a shorthand's longhands", css_text(s, false), "color: red;");

    /* cssstyledeclaration-setter-logical.html */
    s = element("padding-top: 0px; padding-right: 1px; padding-bottom: 2px; padding-left: 3px; padding-block-start: 4px; "
                "padding-block-end: 5px; padding-inline-start: 6px; padding-inline-end: 7px;");
    set(s, "padding-top", "0px", NULL);
    check("logical: a physical longhand set goes after the logical ones", names_of(s),
          "padding-right,padding-bottom,padding-left,padding-block-start,padding-block-end,padding-inline-start,"
          "padding-inline-end,padding-top");
    check("logical: its value stays", get(s, "padding-top", false), "0px");
    set(s, "padding", "0px 1px 2px 3px", NULL);
    check("logical: a physical shorthand set goes after the logical ones", names_of(s),
          "padding-block-start,padding-block-end,padding-inline-start,padding-inline-end,padding-top,padding-right,"
          "padding-bottom,padding-left");
    check("logical: the logical values stay", get(s, "padding-inline-start", false), "6px");

    /* css-logical/parsing/border-block-valid.html, css-typed-om properties/logical.html: a logical
       border side's value from its longhands */
    s = element("");
    set(s, "border-block-start", "green double thin", NULL);
    check("logical: border-block-start", get(s, "border-block-start", false), "thin double green");
    set(s, "border-block-end", "thin", NULL);
    check("logical: border-block-end", get(s, "border-block-end", false), "thin");
    set(s, "border-block-start", "none", NULL);
    check("logical: border-block-start, all initial", get(s, "border-block-start", false), "none");
}

int main(void) {
    serialization();
    get_and_set();
    printf("cssom shorthands: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
