/* The CSSOM rule tree's owner (cssom.c) on the host: insert and remove a CSS
   rule, serialize a CSS rule, the rules' attributes, MediaList, keyframes,
   declaration blocks, and the identity of CSSRule objects across changes.
   tests/check_cssom_rules_native.py builds this with css.c and the DOM. Each
   case names the WPT page or specification section it reads. Built with
   OLD_SOURCE against the source before the rule tree (only top-level rules,
   no ids per rule kind), a case the old interface cannot express fails. */
#include "cssom.c"
#include <stdio.h>
static int passed, failed;
/* @import's URL against the sheet's base: the cases only read the rule's
   href, so the host stands in for web_url.c with the base and the URL joined */
int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    size_t a = strlen(base), b = strlen(rel);
    *out = malloc(a + b + 1);
    if (!*out) return -1;
    memcpy(*out, base, a); memcpy(*out + a, rel, b + 1);
    return 1;
}
static web_doc document_;
static node_t root_;
static void check(const char *label, const char *got, const char *want) {
    bool ok = got && want ? !strcmp(got, want) : got == want;
    if (ok) passed++;
    else { failed++; printf("FAIL %s\n  got:  %s\n  want: %s\n", label, got ? got : "(null)", want ? want : "(null)"); }
}
static void check_int(const char *label, long long got, long long want) {
    char a[32], b[32];
    snprintf(a, sizeof a, "%lld", got); snprintf(b, sizeof b, "%lld", want);
    check(label, a, b);
}
static char *squeeze(char *s) { /* whitespace out, as some WPT pages compare */
    char *w = s;
    for (char *r = s; s && *r; r++) if (!is_space((unsigned char)*r)) *w++ = *r;
    if (s) *w = 0;
    return s;
}

#ifndef OLD_SOURCE
/* ---- the new interface: rules by path ("" the sheet, "0/2" a child's child) */
static struct cssom_sheet *sheet_new(const char *text, bool constructed) {
    int error;
    struct cssom_sheet *s = cssom_construct(&document_, &error);
    if (!s) return NULL;
    if (!constructed) {
        /* a sheet a style element owns: @import stays */
        s->constructed = false;
        struct cssom_parse old = s->p;
        if (sheet_parse(s, text, strlen(text))) return NULL;
        (void)old;
    } else if (cssom_replace_sync(s, text, strlen(text))) return NULL;
    return s;
}
static uint32_t id_at(struct cssom_sheet *s, const char *path) {
    uint32_t id = 0;
    while (*path) {
        uint32_t i = (uint32_t)strtoul(path, (char **)&path, 10), count;
        struct css_rule_info *r = cssom_child(s, id, i, &count);
        if (!r) return UINT32_MAX;
        id = r->id;
        if (*path == '/') path++;
    }
    return id;
}
static struct css_rule_info *rule_of(struct cssom_sheet *s, const char *path) {
    uint32_t id = id_at(s, path);
    return id && id != UINT32_MAX ? cssom_rule(s, id, NULL) : NULL;
}
static char *text_of(struct cssom_sheet *s, const char *path) {
    struct css_rule_info *r = rule_of(s, path);
    if (!r) return NULL;
    sbuf b = {0};
    cssom_serialize(s, r, &b);
    return sb_cstr(&b);
}
static char *text_id(struct cssom_sheet *s, uint32_t id) {
    struct css_rule_info *r = cssom_rule(s, id, NULL);
    if (!r) return NULL;
    sbuf b = {0};
    cssom_serialize(s, r, &b);
    return sb_cstr(&b);
}
static int insert(struct cssom_sheet *s, const char *path, const char *text, uint32_t index) {
    uint32_t id = id_at(s, path);
    return id == UINT32_MAX ? -1 : cssom_insert(s, id, text, strlen(text), index);
}
static int delete_at(struct cssom_sheet *s, const char *path, uint32_t index) {
    uint32_t id = id_at(s, path);
    return id == UINT32_MAX ? -1 : cssom_delete(s, id, index);
}
static long long count_of(struct cssom_sheet *s, const char *path) {
    uint32_t id = id_at(s, path), count;
    if (id == UINT32_MAX) return -1;
    cssom_child(s, id, UINT32_MAX, &count);
    return count;
}
static char *attr_of(struct cssom_sheet *s, const char *path, const char *name) {
    struct css_rule_info *r = rule_of(s, path);
    sbuf b = {0};
    if (!r || !cssom_attribute(s, r, name, &b)) { sb_free(&b); return NULL; }
    for (size_t i = 0; i < b.n; i++) if (!b.p[i]) b.p[i] = '|'; /* nameList */
    return sb_cstr(&b);
}
static int set_attr(struct cssom_sheet *s, const char *path, const char *name, const char *value) {
    return cssom_set_attribute(s, id_at(s, path), name, value, strlen(value));
}
static char *style_of(struct cssom_sheet *s, const char *path) {
    struct css_rule_info *r = rule_of(s, path);
    if (!r) return NULL;
    const char *t; size_t n;
    cssom_declarations(r, &t, &n);
    sbuf b = {0};
    css_decl_serialize(t, n, cssom_declaration_check(r), &b, NULL);
    return sb_cstr(&b);
}
static int set_style(struct cssom_sheet *s, const char *path, const char *text) {
    return cssom_set_declarations(s, id_at(s, path), text, strlen(text));
}
static char *media_op(struct cssom_sheet *s, const char *path, int op, const char *text, int *error) {
    sbuf b = {0};
    *error = cssom_media(s, *path ? id_at(s, path) : 0, op, text, text ? strlen(text) : 0, &b);
    if (op) { sb_free(&b); sbuf c = {0}; cssom_media(s, *path ? id_at(s, path) : 0, 0, NULL, 0, &c); return sb_cstr(&c); }
    return sb_cstr(&b);
}
static char *decl(const char *text) {
    sbuf b = {0};
    css_decl_serialize(text, strlen(text), NULL, &b, NULL);
    return sb_cstr(&b);
}
/* setProperty on the block a style attribute's text parses to: the attribute's
   new text ("update style attribute"); the text itself when nothing changes */
static char *set_decl(const char *text, const char *property, const char *value, const char *priority) {
    char *block = decl(text);
    sbuf b = {0};
    int status = css_decl_set(block, strlen(block), property, value, priority, NULL, &b);
    if (status <= 0) { sb_free(&b); return status < 0 ? NULL : block; }
    free(block);
    return sb_cstr(&b);
}
#define NEW_INTERFACE 1
#else
/* ---- the old interface: top-level rules only */
enum { CSSOM_NOT_FOUND = 9, CSSOM_INVALID_STATE = 10 };
static struct cssom_sheet *sheet_new(const char *text, bool constructed) {
    int error;
    struct cssom_sheet *s = cssom_construct(&document_, &error);
    if (!s || cssom_replace_sync(s, text, strlen(text))) return NULL;
    s->constructed = constructed;
    return s;
}
static struct css_rule_info *rule_of(struct cssom_sheet *s, const char *path) {
    if (!*path || strchr(path, '/')) return NULL;
    uint32_t i = (uint32_t)strtoul(path, NULL, 10);
    struct css_rule_info *r = css_sheet_rules(s->ast, NULL);
    while (r && i--) r = r->next;
    return r;
}
static char *text_of(struct cssom_sheet *s, const char *path) {
    struct css_rule_info *r = rule_of(s, path);
    if (!r) return NULL;
    sbuf b = {0};
    css_rule_text_serialize(r, &b);
    return sb_cstr(&b);
}
static int insert(struct cssom_sheet *s, const char *path, const char *text, uint32_t index) {
    return *path ? -1 : cssom_insert(s, text, strlen(text), index);
}
static int delete_at(struct cssom_sheet *s, const char *path, uint32_t index) { return *path ? -1 : cssom_delete(s, index); }
static long long count_of(struct cssom_sheet *s, const char *path) {
    uint32_t count;
    if (*path) return -1;
    css_sheet_rules(s->ast, &count);
    return count;
}
static uint32_t id_at(struct cssom_sheet *s, const char *path) { struct css_rule_info *r = rule_of(s, path); return r ? r->id : UINT32_MAX; }
static char *text_id(struct cssom_sheet *s, uint32_t id) {
    for (struct css_rule_info *r = css_sheet_rules(s->ast, NULL); r; r = r->next)
        if (r->id == id) { sbuf b = {0}; css_rule_text_serialize(r, &b); return sb_cstr(&b); }
    return NULL;
}
static char *attr_of(struct cssom_sheet *s, const char *path, const char *name) {
    struct css_rule_info *r = rule_of(s, path);
    if (!r || strcmp(name, "selectorText") || r->type != 1) return NULL;
    sbuf b = {0};
    css_rule_selector_serialize(r, &b);
    return sb_cstr(&b);
}
static int set_attr(struct cssom_sheet *s, const char *path, const char *name, const char *value) {
    if (strcmp(name, "selectorText")) return -1;
    return cssom_rule_selector(s, id_at(s, path), value, strlen(value));
}
static char *style_of(struct cssom_sheet *s, const char *path) { (void)s; (void)path; return NULL; }
static int set_style(struct cssom_sheet *s, const char *path, const char *text) { (void)s; (void)path; (void)text; return -1; }
static char *media_op(struct cssom_sheet *s, const char *path, int op, const char *text, int *error) {
    (void)s; (void)path; (void)op; (void)text; *error = -1; return NULL;
}
static char *decl(const char *text) { (void)text; return NULL; }
static char *set_decl(const char *text, const char *property, const char *value, const char *priority) {
    (void)text; (void)property; (void)value; (void)priority; return NULL;
}
#endif

static const char sample[] = ".a {\n  color: red;\n  & .b { color: green; }\n  & .c { color: blue; }\n}";

static void nesting(void) {
    /* WPT css/css-nesting/cssom.html */
    struct cssom_sheet *s = sheet_new("", false);
    check_int("nesting: insert top", insert(s, "", ".a { color: red; }", 0), 0);
    check("nesting: top cssText", text_of(s, "0"), ".a { color: red; }");
    insert(s, "0", "& .b { color: green; }", 0);
    insert(s, "0", "& .c { color: blue; }", 1);
    insert(s, "0", "& .d { color: hotpink; }", 1);
    check("nesting: inserting should work", text_of(s, "0"),
          ".a {\n  color: red;\n  & .b { color: green; }\n  & .d { color: hotpink; }\n  & .c { color: blue; }\n}");
    uint32_t b = id_at(s, "0/0"), d = id_at(s, "0/1"), c = id_at(s, "0/2");
    check_int("nesting: delete", delete_at(s, "0", 1), 0);
    check("nesting: deleting should work", text_of(s, "0"), sample);
    /* CSSOM: other CSSRule objects stay the same rules; the removed one has no parents */
    check_int("identity: first child keeps its id", id_at(s, "0/0"), b);
    check_int("identity: later child keeps its id", id_at(s, "0/1"), c);
    check("identity: removed rule still serializes", text_id(s, d), "& .d { color: hotpink; }");
#ifdef NEW_INTERFACE
    bool attached = true;
    struct css_rule_info *gone = cssom_rule(s, d, &attached);
    check_int("identity: removed rule has no parentStyleSheet", attached, 0);
    check("identity: removed rule has no parentRule", cssom_parent_rule(s, gone) ? "rule" : NULL, NULL);
    struct css_rule_info *kept = cssom_rule(s, c, NULL);
    check_int("identity: kept rule's parentRule", cssom_parent_rule(s, kept) ? (long long)cssom_parent_rule(s, kept)->id : 0,
              id_at(s, "0"));
#endif

    s = sheet_new(sample, false);
    check_int("nesting: index past the end", insert(s, "0", "& .broken {}", 3), CSSOM_INDEX);
    check("nesting: unchanged after no-insert", text_of(s, "0"), sample);
    check_int("nesting: delete past the end", delete_at(s, "0", 5), CSSOM_INDEX);
    check_int("nesting: invalid rule", insert(s, "0", "% {}", 0), CSSOM_SYNTAX);
    check("nesting: unchanged after invalid rule", text_of(s, "0"), sample);
    check("nesting: a child alone", text_of(s, "0/1"), "& .c { color: blue; }");

    s = sheet_new(sample, false);
    insert(s, "0", "@supports selector(&) { & div { font-size: 10px; }}", 1);
    check("nesting: @supports is added", text_of(s, "0"),
          ".a {\n  color: red;\n  & .b { color: green; }\n  @supports selector(&) {\n  & div { font-size: 10px; }\n}\n  & .c { color: blue; }\n}");
    delete_at(s, "0", 1);
    check("nesting: @supports removed", text_of(s, "0"), sample);

    s = sheet_new(sample, false);
    set_style(s, "0", "color: olivedrab; &.d { color: peru; }");
    check("nesting: style ignores a rule", text_of(s, "0"),
          ".a {\n  color: olivedrab;\n  & .b { color: green; }\n  & .c { color: blue; }\n}");

    s = sheet_new(sample, false);
    set_attr(s, "0/0", "selectorText", "div.b .c &");
    set_attr(s, "0/1", "selectorText", ".c div.b &, div &");
    insert(s, "0", "div & {}", 0);
    check("nesting: selectorText and insertRule", text_of(s, "0"),
          ".a {\n  color: red;\n  div & { }\n  div.b .c & { color: green; }\n  .c div.b &, div & { color: blue; }\n}");
    check("nesting: relative selector absolutized", (s = sheet_new(".a { > .b { } .c, &.d { } }", false), text_of(s, "0")),
          ".a {\n  & > .b { }\n  & .c, &.d { }\n}");
}

static void group_rules(void) {
    /* WPT css/css-nesting/serialize-group-rules-with-decls.html */
    static const char *const cases[][2] = {
        {"@media screen {\n  div { color: red; background-color: green; }\n}", NULL},
        {"div { @media screen { color: red; background-color: green; } }",
         "div {\n  @media screen {\n  color: red; background-color: green;\n}\n}"},
        {"div {\n  @supports selector(&) { color: red; background-color: green; } &:hover { color: navy; } }",
         "div {\n  @supports selector(&) {\n  color: red; background-color: green;\n}\n  &:hover { color: navy; }\n}"},
        {"div {\n  @media screen {\n  & { color: red; }\n}\n}", NULL},
        {"div { @media screen { & { color: red; &:hover { } } }",
         "div {\n  @media screen {\n  & {\n  color: red;\n  &:hover { }\n}\n}\n}"},
        {"div { @media screen { &.cls { color: red; } & { color: red; }",
         "div {\n  @media screen {\n  &.cls { color: red; }\n  & { color: red; }\n}\n}"},
        {"div { @media screen { color: red; & { color: red; }", "div {\n  @media screen {\n  color: red;\n  & { color: red; }\n}\n}"},
        {"div { @media screen { &, p > & { color: blue; }", "div {\n  @media screen {\n  &, p > & { color: blue; }\n}\n}"},
        {"div { & { color: red; } }", "div {\n  & { color: red; }\n}"},
        {"@media screen {\n}", NULL},
        {"div { }", NULL},
        {"div {\n  @media screen {\n}\n}", NULL},
        {"@media screen {\n  div { }\n}", NULL}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        struct cssom_sheet *s = sheet_new("", false);
        char label[96];
        snprintf(label, sizeof label, "group rules: case %zu", i + 1);
        insert(s, "", cases[i][0], 0);
        check(label, text_of(s, "0"), cases[i][1] ? cases[i][1] : cases[i][0]);
    }
}

static void nested_declarations(void) {
    /* CSSOM insert a CSS rule step 4; CSS Nesting CSSNestedDeclarations */
    struct cssom_sheet *s = sheet_new(".a { color: red; b { } top: 0px; }", false);
    check_int("nested declarations: children", count_of(s, "0"), 2);
    check("nested declarations: serialized", text_of(s, "0/1"), "top: 0px;");
    uint32_t nd = id_at(s, "0/1");
    /* removing b would merge the run into .a's own declarations */
    delete_at(s, "0", 0);
    check_int("nested declarations: kept apart after delete", count_of(s, "0"), 1);
    check_int("nested declarations: same object", id_at(s, "0/0"), nd);
    check("nested declarations: parent", text_of(s, "0"), ".a {\n  color: red;\n  top: 0px;\n}");
    check_int("nested declarations: insert", insert(s, "0", "left: 1px", 1), 0);
    check("nested declarations: inserted", text_of(s, "0"), ".a {\n  color: red;\n  top: 0px;\n  left: 1px;\n}");
    check_int("nested declarations: empty is a syntax error", insert(s, "0", "nonsense", 0), CSSOM_SYNTAX);
    set_style(s, "0/0", "bottom: 2px");
    check("nested declarations: style set", text_of(s, "0"), ".a {\n  color: red;\n  bottom: 2px;\n  left: 1px;\n}");
    /* the marker is cssom.c's, never the author's */
    s = sheet_new(".a { @-nocturne-nested-declarations { color: red; } }", false);
    check_int("nested declarations: author marker is an unknown at-rule", count_of(s, "0"), 0);
}

static void keyframes(void) {
    /* WPT css/cssom/CSSKeyframesRule.html, CSSKeyframesRule-appendRule-invalid.html */
    struct cssom_sheet *s = sheet_new("@keyframes foo { 0% { top: 0px; } 100% { top: 200px; } } @keyframes empty {}", false);
    check("keyframes: name", attr_of(s, "0", "name"), "foo");
    check_int("keyframes: length", count_of(s, "0"), 2);
    check("keyframes: first", text_of(s, "0/0"), "0% { top: 0px; }");
    insert(s, "0", "50% { top: 100px; }", 2);
    insert(s, "0", "0% { top: 50px; }", 3);
    check("keyframes: appended", text_of(s, "0/3"), "0% { top: 50px; }");
#ifdef NEW_INTERFACE
    check_int("keyframes: findRule 50%", cssom_keyframe_find(s, id_at(s, "0"), "50%", 3), 2);
    check_int("keyframes: findRule 0% is the last", cssom_keyframe_find(s, id_at(s, "0"), "0%", 2), 3);
    check_int("keyframes: findRule from", cssom_keyframe_find(s, id_at(s, "0"), "from", 4), 3);
    check_int("keyframes: findRule 70%", cssom_keyframe_find(s, id_at(s, "0"), "70%", 3), -1);
#endif
    set_attr(s, "1", "name", "bar");
    check("keyframes: name setter", squeeze(text_of(s, "1")), "@keyframesbar{}");
    set_attr(s, "1", "name", "initial");
    check("keyframes: CSS-wide keyword name", squeeze(text_of(s, "1")), "@keyframes\"initial\"{}");
    check("keyframes: name getter", attr_of(s, "1", "name"), "initial");
    set_attr(s, "1", "name", "none");
    check("keyframes: none name", squeeze(text_of(s, "1")), "@keyframes\"none\"{}");
    s = sheet_new("@keyframes test {}", false);
    insert(s, "0", "from { opacity: 0 } garbage", 0);
    insert(s, "0", "from { opacity: 0 } @media all {}", 0);
    insert(s, "0", "from { opacity: 0 } to { opacity: 1 }", 0);
    check_int("keyframes: invalid appends ignored", count_of(s, "0"), 0);
    insert(s, "0", "from { opacity: 0 }", 0);
    check("keyframes: keyText from", attr_of(s, "0/0", "keyText"), "0%");
    check_int("keyframes: keyText invalid", set_attr(s, "0/0", "keyText", "nope"), CSSOM_SYNTAX);
    set_attr(s, "0/0", "keyText", "to, 50%");
    check("keyframes: keyText set", attr_of(s, "0/0", "keyText"), "100%, 50%");
}

static void at_rules(void) {
    struct cssom_sheet *s = sheet_new("@page { } @page :left { } @page named:first { margin: 0px; }", false);
    /* WPT css/cssom/cssom-pagerule.html */
    check("page: empty", text_of(s, "0"), "@page { }");
    check("page: :left", text_of(s, "1"), "@page :left { }");
    check("page: named", text_of(s, "2"), "@page named:first { margin: 0px; }");
    set_attr(s, "0", "selectorText", "named:first");
    check("page: selectorText set", text_of(s, "0"), "@page named:first { }");
    /* CSS Conditional 3: @media's serialization and its MediaList */
    s = sheet_new("@media screen { div { } }", false);
    check("media: cssText", text_of(s, "0"), "@media screen {\n  div { }\n}");
    check("media: conditionText", attr_of(s, "0", "conditionText"), "screen");
    int error;
    check("media: appendMedium", media_op(s, "0", 2, "print", &error), "screen, print");
    check("media: appendMedium again", media_op(s, "0", 2, "print", &error), "screen, print");
    check("media: deleteMedium", media_op(s, "0", 3, "screen", &error), "print");
    media_op(s, "0", 3, "tv", &error);
    check_int("media: deleteMedium of none", error, CSSOM_NOT_FOUND);
    check("media: mediaText set", media_op(s, "0", 1, "", &error), "");
    check("media: empty list", text_of(s, "0"), "@media  {\n  div { }\n}");
    check("media: mediaText set again", media_op(s, "0", 1, "(min-width: 10px), print", &error), "(min-width: 10px), print");
    check("media: cssText after", text_of(s, "0"), "@media (min-width: 10px), print {\n  div { }\n}");
    check("media: sheet MediaList", media_op(s, "", 1, "screen", &error), "screen");
    /* CSSOM 6.4.2: the order CSS keeps for @import and @namespace */
    s = sheet_new("@import url(a.css); a { }", false);
    check_int("hierarchy: @import after a style rule", insert(s, "", "@import url(b.css);", 2), CSSOM_HIERARCHY);
    check_int("hierarchy: a style rule before @import", insert(s, "", "b { }", 0), CSSOM_HIERARCHY);
    check_int("hierarchy: @import first", insert(s, "", "@import url(b.css);", 0), 0);
    check_int("hierarchy: @namespace after a style rule", insert(s, "", "@namespace x url(y);", 3), CSSOM_HIERARCHY);
    check_int("hierarchy: @namespace with style rules", insert(s, "", "@namespace x url(y);", 2), CSSOM_INVALID_STATE);
    check_int("hierarchy: @import in a group", (insert(s, "", "@media all { }", 3), insert(s, "3", "@import url(c.css);", 0)),
              CSSOM_HIERARCHY);
    struct cssom_sheet *c = sheet_new("", true);
    check_int("hierarchy: @import in a constructed sheet", insert(c, "", "@import url(b.css);", 0), CSSOM_SYNTAX);
    s = sheet_new("@import url(a.css); a { }", false);
    check_int("hierarchy: @namespace after @import with rules", insert(s, "", "@namespace x url(y);", 1), CSSOM_INVALID_STATE);
    s = sheet_new("@namespace x url(y); a { }", false);
    check_int("hierarchy: delete @namespace with rules", delete_at(s, "", 0), CSSOM_INVALID_STATE);
    /* CSSImportRule (CSSOM, CSS Cascade 5) */
    s = sheet_new("@import url(\"x.css\") layer(foo) supports(display: grid) screen;", false);
    check("import: href", attr_of(s, "0", "href"), "x.css");
    check("import: layerName", attr_of(s, "0", "layerName"), "foo");
    check("import: supportsText", attr_of(s, "0", "supportsText"), "display: grid");
    check("import: media", media_op(s, "0", 0, NULL, &error), "screen");
    check("import: cssText", text_of(s, "0"), "@import url(\"x.css\") layer(foo) supports(display: grid) screen;");
    s = sheet_new("@import 'y.css';", false);
    check("import: no layer", attr_of(s, "0", "layerName"), NULL);
    check("import: string href", text_of(s, "0"), "@import url(\"y.css\");");
    /* CSSNamespaceRule */
    s = sheet_new("@namespace svg url(http://www.w3.org/2000/svg); svg|a { }", false);
    check("namespace: prefix", attr_of(s, "0", "prefix"), "svg");
    check("namespace: namespaceURI", attr_of(s, "0", "namespaceURI"), "http://www.w3.org/2000/svg");
    check("namespace: cssText", text_of(s, "0"), "@namespace svg url(\"http://www.w3.org/2000/svg\");");
    check("namespace: a prefixed selector", attr_of(s, "1", "selectorText"), "svg|a");
    check_int("namespace: a rule inserted with its prefix", insert(s, "", "svg|b { }", 2), 0);
    /* CSS Cascade 5: @layer */
    s = sheet_new("@layer a, b; @layer x { a { } } @layer { b { } }", false);
    check("layer: statement", text_of(s, "0"), "@layer a, b;");
    check("layer: nameList", attr_of(s, "0", "nameList"), "a|b|");
    check("layer: block", text_of(s, "1"), "@layer x {\n  a { }\n}");
    check("layer: anonymous", attr_of(s, "2", "name"), "");
    /* CSS Contain 3: @container */
    s = sheet_new("@container card (min-width: 400px) { a { } }", false);
    check("container: name", attr_of(s, "0", "containerName"), "card");
    check("container: query", attr_of(s, "0", "containerQuery"), "(min-width: 400px)");
    check("container: conditionText", attr_of(s, "0", "conditionText"), "card (min-width: 400px)");
    /* CSS Cascade 6: @scope */
    s = sheet_new("@scope (.a) to (.b) { c { } }", false);
    check("scope: start", attr_of(s, "0", "start"), ".a");
    check("scope: end", attr_of(s, "0", "end"), ".b");
    check("scope: cssText", text_of(s, "0"), "@scope (.a) to (.b) {\n  c { }\n}");
    /* CSS Properties and Values API */
    s = sheet_new("@property --x { initial-value: 0px; inherits: false; syntax: \"<length>\"; }", false);
    check("property: cssText", text_of(s, "0"), "@property --x { syntax: \"<length>\"; inherits: false; initial-value: 0px; }");
    check("property: syntax", attr_of(s, "0", "syntax"), "<length>");
    check("property: inherits", attr_of(s, "0", "inherits"), "false");
    check("property: initialValue", attr_of(s, "0", "initialValue"), "0px");
    /* CSS Fonts 4: @font-feature-values */
    s = sheet_new("@font-feature-values Foo { @swash { fancy: 1; } @styleset { nice: 2 4; } }", false);
    check("feature values: cssText", text_of(s, "0"), "@font-feature-values Foo { @swash { fancy: 1; } @styleset { nice: 2 4; } }");
#ifdef NEW_INTERFACE
    uint32_t values[] = {3};
    cssom_set_feature_values(s, id_at(s, "0"), "swash", "other", values, 1);
    check("feature values: set", text_of(s, "0"), "@font-feature-values Foo { @swash { fancy: 1; other: 3; } @styleset { nice: 2 4; } }");
    cssom_set_feature_values(s, id_at(s, "0"), "swash", "fancy", NULL, 0);
    check("feature values: delete", text_of(s, "0"), "@font-feature-values Foo { @swash { other: 3; } @styleset { nice: 2 4; } }");
#endif
    /* @counter-style descriptors */
    s = sheet_new("@counter-style thumbs { system: cyclic; symbols: \"x\"; }", false);
    check("counter style: name", attr_of(s, "0", "name"), "thumbs");
    check("counter style: system", attr_of(s, "0", "system"), "cyclic");
    set_attr(s, "0", "suffix", "\" \"");
    check("counter style: suffix set", text_of(s, "0"), "@counter-style thumbs { system: cyclic; symbols: \"x\"; suffix: \" \"; }");
    /* @font-face's descriptors */
    s = sheet_new("@font-face { src: url(x.woff); color: red; }", false);
    check("font face: descriptors only", text_of(s, "0"), "@font-face { src: url(\"x.woff\"); }");
}

static void declarations_and_replace(void) {
    /* CSSOM 6.7.2 serialize a CSS declaration block */
    check("declarations: one", decl("font-weight:bold"), "font-weight: bold;");
    check("declarations: priority", decl("color:red !important;top:1px"), "color: red !important; top: 1px;");
    check("declarations: a later one wins", decl("color: red; top: 1px; color: blue"), "top: 1px; color: blue;");
    check("declarations: an important one stays", decl("color: red !important; color: blue"), "color: red !important;");
    check("declarations: invalid ones are not in the block", decl("color: red; nonsense: 1; top: ;"), "color: red;");
    check("declarations: names lowercased, custom ones kept", decl("COLOR:red;--Case:.5"), "color: red; --Case: .5;");
    check("declarations: specified values", decl("opacity:.55 ! IMPORTANT"), "opacity: 0.55 !important;");
    /* CSSOM 6.6.1 setProperty, then update style attribute: the attribute is
       the block's serialization, the declarations already there included
       (WPT editing "bold with CSS": style="font-weight: bold;") */
    check("setProperty: into an empty attribute", set_decl("", "font-weight", "bold", NULL), "font-weight: bold;");
    check("setProperty: others re-serialized", set_decl("color:red", "font-weight", "bold", NULL),
          "color: red; font-weight: bold;");
    check("setProperty: important", set_decl("top:1px", "LEFT", "2px", "important"), "top: 1px; left: 2px !important;");
    check("setProperty: replaces the declaration", set_decl("top:1px;left:2px", "top", "3px", ""), "left: 2px; top: 3px;");
    check("setProperty: comments and space in the value", set_decl("", "opacity", " /*x*/.5 ", NULL), "opacity: 0.5;");
    check("setProperty: an invalid value changes nothing", set_decl("top:1px", "top", "red", NULL), "top: 1px;");
    check("setProperty: an invalid priority changes nothing", set_decl("top:1px", "top", "2px", "wrong"), "top: 1px;");
    check("setProperty: an empty value removes", set_decl("top:1px;left:2px", "top", "", NULL), "left: 2px;");
    check("setProperty: a space value is not a removal", set_decl("top:1px", "top", " ", NULL), "top: 1px;");
    check("setProperty: custom property", set_decl("", "--Name", " a b ", NULL), "--Name: a b;");
    struct cssom_sheet *s = sheet_new("a { color: red; }", true);
    uint32_t old = id_at(s, "0");
    cssom_replace_sync(s, "b { top: 1px; } @import url(x.css);", 35);
    check("replace: new rules", text_of(s, "0"), "b { top: 1px; }");
    check_int("replace: @import dropped", count_of(s, ""), 1);
    check("replace: an old object keeps its rule", text_id(s, old), "a { color: red; }");
    check_int("replace: new ids", id_at(s, "0") != old, 1);
    /* a removed group rule stays usable */
    s = sheet_new("@media screen { a { } }", false);
    uint32_t media = id_at(s, "0");
    id_at(s, "0/0");
    delete_at(s, "", 0);
#ifdef NEW_INTERFACE
    check_int("removed: insert into it", cssom_insert(s, media, "b { }", 5, 1), 0);
#endif
    check("removed: its text", text_id(s, media), "@media screen {\n  a { }\n  b { }\n}");
    check_int("removed: the sheet is empty", count_of(s, ""), 0);
    /* style rule declarations */
    s = sheet_new("a { color: red; }", false);
    check("style: read", style_of(s, "0"), "color: red;");
    set_style(s, "0", "top: 1px; color: blue");
    check("style: set", text_of(s, "0"), "a { top: 1px; color: blue; }");
    set_attr(s, "0", "selectorText", "b, c");
    check("style: selectorText", text_of(s, "0"), "b, c { top: 1px; color: blue; }");
    set_attr(s, "0", "selectorText", "%%");
    check("style: invalid selectorText changes nothing", attr_of(s, "0", "selectorText"), "b, c");
    /* the sheet's text closes what end of input left open */
    s = sheet_new("a { color: red", false);
    insert(s, "", "b { }", 1);
    check("closing: earlier rule", text_of(s, "0"), "a { color: red; }");
    check("closing: later rule", text_of(s, "1"), "b { }");
}

int main(void) {
    document_.root = &root_;
    document_.base = "https://example.test/";
    root_.owner = &document_;
    nesting();
    group_rules();
    nested_declarations();
    keyframes();
    at_rules();
    declarations_and_replace();
    printf("cssom rules: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
