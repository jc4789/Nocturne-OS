/* Internals of the web engine (see web.h). Files:
     util.c    arenas, string buffers, URLs
     html.c    HTML tag IDs (html_lexbor.c builds the tree)
     css.c     CSS parser, selectors, the cascade and computed values
     cssprop.c CSS properties, values, shorthands and the UA stylesheet
     cssmath.c CSS units and math functions: parsing, types, simplification, evaluation, serialization
     color.c   CSS colours: parsing, computed values, conversion, interpolation, serialization
     box.c     box tree generation
     layout.c  block, inline, float, table and positioned layout
     paint.c   painting, hit testing, find
     doc.c     the public API: documents, resources, forms */
#pragma once
#include <http.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "gfx.h"
#include "font.h"
#include "image.h"
#include "web.h"

/* ---------------------------------------------------------------- memory */
typedef struct arena {
    struct achunk *head;
    size_t allocated, limit;
    jmp_buf *trap;
    size_t chunk_size; /* preferred total chunk bytes; 0 retains the 64 KiB default */
} arena_t;

void *ar_alloc(arena_t *a, size_t n); /* zeroed, 8-byte aligned */
char *ar_strndup(arena_t *a, const char *s, size_t n);
char *ar_strdup(arena_t *a, const char *s);
void ar_free(arena_t *a);
void ar_adopt(arena_t *into, arena_t *from);

typedef struct sbuf {
    char *p;
    size_t n, cap;
} sbuf;
void sb_put(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_putc(sbuf *b, char c);
void sb_utf8(sbuf *b, uint32_t cp);
char *sb_cstr(sbuf *b); /* NUL-terminates; the buffer stays owned by b */
void sb_free(sbuf *b);

/* growable array of pointers */
typedef struct pvec {
    void **v;
    int n, cap;
} pvec;
void pv_push(pvec *p, void *x);
void pv_free(pvec *p);

static inline bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
bool str_ieq(const char *a, const char *b);
bool strn_ieq(const char *a, const char *b, size_t n); /* a has length n, b is NUL-terminated */
int utf8_put(char *out, uint32_t cp);                 /* returns bytes written (1..4) */
int web_hex_digit(unsigned char c);                   /* an ASCII hex digit's value, else -1 */

/* ---------------------------------------------------------------- DOM */
#define WEB_TAGS(X)                                                                                     \
    X(a) X(abbr) X(address) X(applet) X(area) X(article) X(aside) X(audio) X(b) X(base) X(bdi) X(bdo)   \
    X(big) X(blockquote) X(body) X(br) X(button) X(canvas) X(caption) X(center) X(cite) X(code) X(col)  \
    X(colgroup) X(data) X(datalist) X(dd) X(del) X(details) X(dfn) X(dialog) X(dir) X(div) X(dl) X(dt)  \
    X(em) X(embed) X(fieldset) X(figcaption) X(figure) X(font) X(footer) X(form) X(frame) X(frameset)   \
    X(h1) X(h2) X(h3) X(h4) X(h5) X(h6) X(head) X(header) X(hgroup) X(hr) X(html) X(i) X(iframe)        \
    X(image) X(img) X(input) X(ins) X(kbd) X(keygen) X(label) X(legend) X(li) X(link) X(listing)        \
    X(main) X(map) X(mark) X(marquee) X(math) X(menu) X(meta) X(meter) X(nav) X(nobr) X(noembed)        \
    X(noframes) X(noscript) X(object) X(ol) X(optgroup) X(option) X(output) X(p) X(param) X(picture)    \
    X(plaintext) X(pre) X(progress) X(q) X(rp) X(rt) X(ruby) X(s) X(samp) X(script) X(search)           \
    X(section) X(select) X(slot) X(small) X(source) X(span) X(strike) X(strong) X(style) X(sub)         \
    X(summary) X(sup) X(svg) X(table) X(tbody) X(td) X(template) X(textarea) X(tfoot) X(th) X(thead)    \
    X(time) X(title) X(tr) X(track) X(tt) X(u) X(ul) X(var) X(video) X(wbr) X(xmp)

enum {
    T_UNKNOWN,
#define X(n) T_##n,
    WEB_TAGS(X)
#undef X
    T_COUNT
};
int tag_lookup(const char *name, size_t n);
extern const char *const tag_names[T_COUNT];

/* PE_SLOTTED selects assigned elements, never a generated pseudo box. Keep
   that existing selector marker in the same value namespace as real pseudos. */
enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_OTHER, PE_BACKDROP, PE_SLOTTED, PE_PLACEHOLDER, PE_MARKER };

enum { N_DOC, N_ELEM, N_TEXT, N_COMMENT, N_FRAGMENT, N_DOCTYPE, N_PI, N_ATTR };
/* An element's namespace: the three the renderer knows, the null namespace,
   or NS_OTHER, whose URI is the element's own namespace_uri. */
enum { NS_HTML, NS_SVG, NS_MATHML, NS_NONE, NS_OTHER };
#define WEB_NS_HTML "http://www.w3.org/1999/xhtml"
#define WEB_NS_SVG "http://www.w3.org/2000/svg"
#define WEB_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define WEB_NS_XML "http://www.w3.org/XML/1998/namespace"
#define WEB_NS_XMLNS "http://www.w3.org/2000/xmlns/"
#define WEB_NS_XLINK "http://www.w3.org/1999/xlink"

struct attr {
    const char *name;  /* lowercase */
    const char *raw;   /* as written (SVG is case sensitive) */
    const char *value;
    const char *namespace_uri, *prefix, *local; /* NULL namespace/prefix are significant */
    struct node *node; /* Lazily materialized, stable Attr identity. */
};

struct style;
struct box;

typedef struct node {
    web_doc *owner;          /* logical ownerDocument; changes on adoption */
    web_doc *allocation_doc; /* arena/value lifetime owner; never changes */
    struct node *template_content; /* separate inert tree, not element children */
    struct node *template_host; /* host-inclusive cycle validation only */
    struct node *parser_form_owner; /* parser's non-ancestor association; reset by DOM mutation */
    struct node *parser_form_next; /* only nodes with parser associations, not every DOM node */
    int32_t custom_registry_id; /* -2 unset, -1 explicit null, 0 global, positive scoped */
    void *custom_registry_realm; /* native identity, never an author-visible property */
    const char *custom_is; /* internal customized built-in name, independent of is attribute */
    /* Shadow trees share the native node arena but never the DOM parent links.
       Only an N_FRAGMENT root has shadow_host; only an element has shadow_root. */
    struct node *shadow_root, *shadow_host;
    struct node *shadow_dirty_next, *shadow_slots, *shadow_slot_next;
    bool shadow_dirty;
    bool shadow_closed, shadow_delegates_focus, shadow_clonable, shadow_serializable;
    bool shadow_manual, shadow_declarative, shadow_keep_registry_null;
    /* Native assignment snapshots/lists: these are not child/parent links. */
    struct node *assigned_slot, *assigned_next;
    uint64_t wheel_blocking; /* non-passive native listener mirror, not DOM state */
    struct node *pending_assigned_slot, *pending_assigned_next;
    struct node *slot_assigned_first, *slot_assigned_last;
    struct node *slot_wanted_first, *slot_wanted_last;
    struct node *manual_slot, *manual_next, *slot_manual_first, *slot_manual_last;
    bool slot_change_pending;
    struct node *slot_change_next; /* family-owned FIFO, deduplicated while pending */
    bool details_toggle_pending, details_toggle_old_open, details_toggle_new_open;
    struct node *details_toggle_next;
    uint8_t type;
    uint8_t namespace_id; /* stable across detach/clone/adopt; not ancestor-derived */
    uint16_t tag; /* HTML tag ID of the local name; an HTML-namespace local name with ASCII uppercase has none */
    bool foreign;     /* namespace_id != NS_HTML, retained for renderer/parser */
    bool svg_snapshot_dirty; /* outermost <svg>: a descendant restyle may change its serialized paint */
    bool cdata;       /* N_TEXT: a CDATASection node */
    const char *name; /* element: ASCII-lowercase local name; case-preserving PI target */
    const char *raw_name; /* element: qualified name, case preserved */
    /* Element: DOM prefix (NULL for none), local name (a tail of raw_name) and,
       for NS_OTHER only, the namespace URI. Read the URI with doc_element_namespace. */
    const char *prefix, *local_name, *namespace_uri;
    const char *public_id, *system_id;
    size_t public_id_len, system_id_len;
    bool doctype_identifiers_sized;
    bool referrer_meta_processed; /* Removal does not restore an earlier policy. */
    struct attr *attrs;
    int nattrs;
    struct attr *attribute; /* N_ATTR: the canonical current record, attached or detached. */
    struct node *attr_owner; /* Not a DOM parent. */
    char *text; /* Text, Comment and ProcessingInstruction data */
    size_t textlen;
    struct html_pi_attribute *pi_attributes;
    size_t pi_attribute_count;
    bool pi_attributes_ready;
    bool html_policy_processed;
    struct node *parent, *first, *last, *next, *prev;
    const char *id;
    const char **classes;
    int nclasses;
    int elem_index; /* 1-based position among element siblings */
    /* cascade and layout */
    struct style *style;
    arena_t style_mem; /* lazy variable tail; fixed styles live in document slabs */
    arena_t style_previous_mem;
    /* An adopted style's inherited values, which the children's styles
       borrow, when they are not in style_mem or an ancestor's memory. */
    arena_t style_inherit_mem;
    struct css_style_slot *style_slots, *style_previous_slots;
    struct css_declarations *style_declarations; /* immutable inline declaration snapshot */
    /* css.c's css_inline_text: "the style attribute\0its exact declarations", written by the
       last change through CSSOM */
    char *style_cssom;
    struct style *style_previous;
    struct node *style_retired_next;
    bool style_dirty, style_children_dirty;
    bool style_self_dirty; /* only this element's own match changed */
    /* a child's style read sibling-index() or sibling-count() (css-values-5 §10): a change of the
       child list restyles the children; css workers set it, so it is written atomically */
    bool style_counts_children;
    uint8_t style_mark_source; /* --debug-js: what set style_dirty */
    uint8_t style_form_state; /* supervisor-published form pseudo snapshot; cascade reads only */
    uint32_t style_subtree_work; /* elements the next cascade restyles here, counted before it splits */
    /* Published only after layout joins; CSS workers never read mutable boxes
       or borrow this name list from a retired computed-style arena. */
    char *container_names;
    float container_width, container_height, container_em;
    uint8_t container_type;
    bool container_valid;
    struct node *query_inline_container, *query_size_container;
    struct css_scope *style_scope; /* root-only, borrowed from current immutable rule index */
    struct style *animation_base_style; /* cascade without this node's effects */
    struct style *animation_after_style; /* CSS Transitions: the after-change style its children inherit from */
    struct web_effect *effects;          /* the animation effects targeting it (animation.c) */
    struct web_animation *owned;         /* the CSS animations and transitions it owns (animation.c) */
    struct box *box;          /* the element's first box */
    struct box *anchor_block; /* inline elements: the block holding its first line */
    bool box_regen; /* its own box must be generated again (restyle or box-only HTML semantics) */
    float anchor_dy;
    /* Persistent CSSOM element scroll position; box/layout arenas are rebuilt.
       Only the element's principal box consumes these offsets. */
    double scroll_x, scroll_y;
    /* Native input notifications only. A detached dispatch batch has its own
       link so callback-time input/adoption cannot overwrite that batch. */
    struct web_doc *scroll_input_owner;
    struct node *scroll_input_next, *scroll_dispatch_next;
    /* Marquee motion belongs to the native element, not CSSOM scroll state.
       Retained across detach/adopt and box reconstruction. */
    float marquee_x, marquee_y, marquee_progress;
    uint64_t marquee_last, marquee_loops;
    uint8_t marquee_direction, marquee_behavior;
    bool marquee_initialized, marquee_off, marquee_reverse;
    /* Link completion identity survives stylesheet snapshot rebuilds. */
    const char *stylesheet_url;
    struct web_doc *stylesheet_owner;
    uint64_t stylesheet_generation;
    bool stylesheet_notified;
    /* Its document's parser created it as a style element, or as a link that
       contributed to styling with its style sheet enabled (HTML "contributes a
       script-blocking style sheet"; doc_parser_element_created). */
    bool parser_sheet;
    /* form controls */
    char *value; /* current value of input/textarea (malloc'd) */
    size_t value_capacity;
    /* Native editing may temporarily contain an incomplete number/date while
       the script-visible value remains sanitized. Both buffers are budgeted. */
    char *input_edit;
    size_t input_edit_capacity;
    bool face_associated; /* Only the private CE definition bridge sets this. */
    struct web_face_state *internals;
    bool input_bad_input, control_user_edited;
    const char *custom_validity; /* allocation-document arena; initially empty */
    size_t custom_validity_length;
    bool checked, selected_set;
    bool click_in_progress;
    bool constructing_form_entries; /* entry-list construction and formdata event reentry guard */
    bool firing_submission_events; /* a form's HTML "firing submission events" (web_form_submit) */
    int image_submit_x,image_submit_y; /* selected image CSS point, initially 0,0; non-pointer activation preserves it */
    bool indeterminate; /* native checkbox display state, independent of checkedness */
    struct web_form_files *files;
    uint64_t file_revision;
    bool value_dirty, checked_dirty;
    /* DOM endpoints are UTF-16 code units, not renderer UTF-8 byte offsets.
       Keep even half-surrogate endpoints exact until native editing/rendering. */
    uint32_t selection_start, selection_end;
    uint8_t selection_direction; /* 0 none, 1 forward, 2 backward */
    bool selection_set; /* explicit API/user range, or a previously focused control */
    int selected; /* select: index of the selected option */
    int image;    /* <img>: index into the document's images, or -1 */
    int image_request; /* latest selected resource; image may retain an available old request */
    uint64_t image_generation;
    struct web_canvas *canvas; /* independent bounded bitmap; allocation_doc owns it */
    bool image_initialized, image_invalidated, image_has_source;
    struct node *owned_next; /* document-owned allocation list, including detached nodes */
    struct node *owned_image_next; /* the same allocation's <img>/<input> elements (doc_image_node_next) */
    bool control_ready, script_started;
    /* An inserted frame element waiting for its HTML post-connection steps: until they run (after
       every node of the insertion is in place, in tree order with scripts) it has no child navigable. */
    bool frame_connection_pending;
    bool script_parse_eligible; /* fragment script closed before EOF; explicit runScripts only */
    const char *script_text_snapshot; /* admitted source, allocation_doc arena; never a DOM property */
    size_t script_text_snapshot_len;
    bool script_text_snapshot_valid;
    /* Native resource state must survive collection/recreation of a JS wrapper. */
    bool js_force_async_set, js_force_async, js_image_notified;
    int js_image;
    uint64_t js_image_generation;
    bool style_disabled; /* stylesheet state, not the HTML disabled attribute */
    bool style_disabled_set; /* link IDL/CSSOM override of its initial disabled attribute */
    /* CSP: what this element's last check judged (a style element's child
       text, a base element's URL), as a hash and length, and the answer:
       0 none yet, 1 allowed, 2 blocked. A check runs (and reports) once for
       each new text, not on every resource scan. */
    uint64_t csp_checked_hash; size_t csp_checked_length; uint8_t csp_checked;
    /* CSP for the style attribute (HTML: its value applies only when the
       inline check allows it): 0 not checked (applies), 1 allowed, 2 blocked
       (neither the cascade nor CSSOM reads it), 3 CSSOM is writing it (its
       updating flag: the attribute change steps neither parse nor check it). */
    uint8_t csp_style_attribute;
    /* HTML "nonce attributes": [[CryptographicNonce]] of an element that includes
       HTMLOrSVGOrMathMLElement, in allocation_doc's arena (NULL is the empty string). The
       nonce content attribute's change steps set it; the nonce IDL attribute reads and
       writes only it. Read it with doc_element_nonce, never from the attribute. */
    const char *crypto_nonce; size_t crypto_nonce_length;
    struct cssom_sheet *cssom_sheets, *cssom_current; /* native owner/source/AST, allocation-owned */
    uint32_t cssom_serial;
    struct cssom_sheet **adopted_sheets;
    uint32_t adopted_count;
    uint64_t resource_revision;
    void *html_bindings; /* private active parser subscriptions, identity-owned */
    const char *option_label;
    uint64_t option_label_revision;
    struct web_paint_diagnostic *paint_diagnostic; /* debug-only, allocation owner frees */
} node_t;

const char *node_attr(const node_t *n, const char *name); /* NULL if absent */
bool node_has_class(const node_t *n, const char *cls);
node_t *node_ancestor(node_t *n, int tag); /* the nearest ancestor (or n) with this tag */
void node_text_content(const node_t *n, sbuf *out);

/* ---------------------------------------------------------------- CSS values */
/* a length or percentage, possibly a math function: resolved at layout time against a base.
   LK_SIZE: a sizing value of the sizing properties (css-sizing-3 §3.2, css-sizing-4 §3.2, §7) that
   layout resolves: a keyword, fit-content(<length-percentage>) or calc-size() */
enum { LK_AUTO, LK_LEN, LK_NONE, LK_EXPR, LK_NORMAL, LK_NUMBER, LK_SIZE };
/* the sizing keywords (<size-keyword>, and flex-basis's content), calc-size()'s other bases, and the
   function forms: a len_t's size, a calc-size() tree's basis (cssmath.c) */
enum { CSS_SIZING_AUTO = 1, CSS_SIZING_MIN_CONTENT, CSS_SIZING_MAX_CONTENT, CSS_SIZING_FIT_CONTENT, CSS_SIZING_STRETCH,
       CSS_SIZING_CONTENT,
       CSS_SIZING_FIT_CONTENT_FN, /* fit-content(<length-percentage>): its argument as a length */
       CSS_SIZING_ANY,            /* calc-size(any, ...) */
       CSS_SIZING_LENGTH,         /* calc-size(<calc-sum>, ...) */
       CSS_SIZING_CALC_SIZE };    /* calc-size(): a len_t's expr is the tree; a basis: a nested calc-size() */
struct css_calc;
typedef struct len {
    uint8_t kind;
    uint8_t size; /* LK_SIZE: CSS_SIZING_* (a keyword, CSS_SIZING_FIT_CONTENT_FN or CSS_SIZING_CALC_SIZE) */
    float px, pct; /* LK_LEN: px + pct% of the base; LK_NUMBER: px is the number; fit-content()'s argument */
    /* LK_EXPR: a computed calculation (cssmath.c) that needs the base; LK_SIZE: fit-content()'s argument
       when it is one, calc-size()'s tree */
    struct css_calc *expr;
} len_t;
float len_resolve(const len_t *l, float base); /* LK_AUTO/LK_NONE resolve to 0 */
bool css_length_equal(const len_t *,const len_t *); /* semantic computed expression, not arena address */
bool css_length_serialize(const len_t *l, sbuf *out, web_doc *d); /* fallible computed value; append/NUL or restore */
/* a computed calculation of type <length-percentage> as a len_t: px + pct% when it is one (negative:
   the property allows negative values, else a negative one clamps to 0), else LK_EXPR */
bool css_length_from_calc(const struct css_calc *c, bool negative, len_t *out);
/* What a value's grammar accepts (cssprop.c): auto, none, normal, negative values, percentages, and
   LF_LENGTH: unitless numbers other than 0 are not lengths; LF_ANCHOR, LF_ANCHOR_SIZE: anchor() (the
   inset properties) and anchor-size() (the properties @position-try accepts), css-anchor-position-1 */
enum { LF_AUTO = 1, LF_NONE = 2, LF_NEG = 4, LF_NORMAL = 8, LF_NOPCT = 16, LF_LENGTH = 32, LF_ANCHOR = 64,
       LF_ANCHOR_SIZE = 128 };
struct cx;
bool css_parse_length(const char *s, size_t n, struct cx *cx, len_t *out, int flags);
bool css_number_token(const char **s, const char *end, float *out); /* a CSS <number-token> at *s */
const char *css_parse_url(const char *s, size_t n, arena_t *a); /* url(...) (or a string) unescaped; NULL: not one */
void css_length_complement(arena_t *a, const len_t *x, const len_t *y, len_t *out); /* 100% - x - y (y may be NULL) */
void css_length_mix(arena_t *a, const len_t *x, const len_t *y, double ka, double kb, len_t *out); /* ka·x + kb·y */
/* the sizing values (cssmath.c): a keyword by name (CSS_SIZING_*, 0: none) and its name */
int css_sizing_keyword(const char *s, size_t n);
const char *css_sizing_name(int size);
/* fit-content()'s argument of l (LK_SIZE, CSS_SIZING_FIT_CONTENT_FN) as a <length-percentage> */
len_t css_size_argument(const len_t *l);
/* ka·x + kb·y of two values of a sizing property (css-sizing-4 §7.2) as computed-value text: either is
   calc-size(), or keywords combine with <length-percentage>s (interpolate-size: allow-keywords); false:
   they do not combine */
bool css_size_mix(arena_t *a, const len_t *x, const len_t *y, double ka, double kb, bool allow_keywords, sbuf *out);
bool css_query_length(const char *, size_t, float em, float rem, float vw, float vh, double *value);

/* ---------------------------------------------------------------- units and math functions (cssmath.c) */
/* CSS units: a dimension's unit by name ("" a number, "%" a percentage), -1 for none */
int css_unit_find(const char *u, size_t n);
unsigned css_unit_kind(int unit);   /* its CSS_CALC_* type */
bool css_unit_relative(int unit);   /* a length of the element's font, the viewport or a container */
const char *css_unit_name(int unit);
/* v in the type's canonical unit: px, deg, s, Hz, dppx, fr; numbers and percentages as they are.
   cx resolves relative lengths (NULL: no element, the initial font and an 800x600 viewport) */
double css_unit_canonical(int unit, double v, const struct cx *cx);
/* a <number-token>, <percentage-token> or <dimension-token> at *s: its value and unit's name */
bool css_numeric_token(const char **s, const char *end, double *v, const char **unit, size_t *unit_n);
void css_number_serialize(sbuf *b, double v); /* a <number> as CSSOM writes one: six significant figures */
/* What a context accepts where a math function is (CSS Values 4 §10.7). PERCENT with one dimension:
   <dimension-percentage>, whose percentages resolve against that dimension; PERCENT otherwise:
   <percentage> is a type of its own (opacity, scale, a colour's channels). */
enum { CSS_CALC_NUMBER = 1, CSS_CALC_PERCENT = 2, CSS_CALC_LENGTH = 4, CSS_CALC_ANGLE = 8, CSS_CALC_TIME = 16,
       CSS_CALC_FREQUENCY = 32, CSS_CALC_RESOLUTION = 64, CSS_CALC_FLEX = 128, CSS_CALC_ANY = 255 };
struct css_calc_parse {
    arena_t *a;              /* the tree's memory */
    const struct cx *cx;     /* a computed value: relative lengths resolve against it; NULL keeps them */
    const char *const *keys; /* identifiers that stand for numbers (a relative colour's channels) */
    unsigned nkeys;
    unsigned nodes, depth;   /* the parse's budget, from 0 */
    /* LF_ANCHOR, LF_ANCHOR_SIZE: anchor() and anchor-size() are <length>s here (css-anchor-position-1
       §3.2, §5.1; a computed value only, cx set). cssprop.c reads their arguments; each is a leaf */
    uint8_t anchors;
    /* CSS Typed OM §5.6 "reify a math expression": the tree as written, not simplified (units kept,
       subtraction a negate, division an invert, a calc() without an operator a sum of one); a math
       function Typed OM has no object for fails, unrepresentable set. Typed OM reads values this
       owner serialized, so what it reads is already simplified (§10.10.1). */
    bool raw, unrepresentable;
};
/* a numeric type (CSS Values 4 §10.7, CSS Typed OM §4.3.2): the exponent of each base type in the
   order length, angle, time, frequency, resolution, flex, percent, and the percent hint (a base, -1) */
enum { CSS_TYPE_BASES = 7 };
struct css_calc_type { int8_t e[CSS_TYPE_BASES]; int8_t hint; };
bool css_calc_type_add(const struct css_calc_type *a, const struct css_calc_type *b, struct css_calc_type *out);
bool css_calc_type_multiply(const struct css_calc_type *a, const struct css_calc_type *b, struct css_calc_type *out);
/* the CSS_CALC_* type t is in a context that accepts accept (CSS Typed OM "match"), or 0 */
unsigned css_calc_type_match(const struct css_calc_type *t, unsigned accept);
/* the units of the table, 0 .. css_unit_count() - 1: name ("" a number, "%" a percentage), base type
   (-1 a number, else as in struct css_calc_type), relative (needs an element), canonical factor */
int css_unit_count(void);
void css_unit_info(int unit, const char **name, int *base, bool *relative, double *factor);
/* a tree's shape for a reader outside this owner: CSS_MATH_* and the leaf's value and unit, or the
   children (clamp()'s none is NULL) */
enum { CSS_MATH_VALUE, CSS_MATH_SUM, CSS_MATH_PRODUCT, CSS_MATH_NEGATE, CSS_MATH_INVERT, CSS_MATH_MIN,
       CSS_MATH_MAX, CSS_MATH_CLAMP, CSS_MATH_OTHER };
int css_calc_shape(const struct css_calc *c, double *v, int *unit, unsigned *n, struct css_calc *const **children);
bool css_calc_is_math(const char *s, size_t n); /* s starts with a math function */
/* s..s+n, one math function: its simplified tree, typed against accept (*kind: the CSS_CALC_* type it
   is); NULL if it is not valid there */
struct css_calc *css_calc_read(struct css_calc_parse *P, unsigned accept, const char *s, size_t n, unsigned *kind);
struct css_calc *css_calc_key(arena_t *a, unsigned index); /* a key written outside a math function */
int css_calc_key_of(const struct css_calc *c);             /* the key c is, or -1 */
bool css_calc_function_root(const struct css_calc *c);     /* min(), round() ...: not serialized in calc() */
bool css_calc_constant(const struct css_calc *c, double *v); /* one value that needs nothing, canonical */
bool css_calc_linear(const struct css_calc *c, double *px, double *pct); /* px + pct% of a length's basis */
bool css_calc_context_free(const struct css_calc *c);      /* no relative length, no key */
bool css_calc_percent_free(const struct css_calc *c);      /* no percentage that needs a basis */
bool css_calc_equal(const struct css_calc *a, const struct css_calc *b);
/* c's value in its canonical unit: basis is what 100% is (NaN: percentages stay numbers), keys the
   keys' values, cx the element's relative lengths. NaN and infinities as IEEE-754 gives them. */
double css_calc_value(const struct css_calc *c, double basis, const double *keys, const struct cx *cx);
struct css_calc *css_calc_bind(arena_t *a, const struct css_calc *c, const struct cx *cx); /* relative lengths resolved */
/* anchor() and anchor-size() leaves (css-anchor-position-1): whether c holds one; a copy of c in a with
   each replaced by what map gives (NULL: the copy fails), simplified again; the leaves map may give */
struct css_anchor_function;
typedef struct css_calc *(*css_calc_anchor_map)(void *context, arena_t *a, const struct css_anchor_function *f);
bool css_calc_anchored(const struct css_calc *c);
struct css_calc *css_calc_map_anchors(arena_t *a, const struct css_calc *c, css_calc_anchor_map map, void *context);
struct css_calc *css_calc_anchor(arena_t *a, const struct css_anchor_function *f); /* a <length> */
struct css_calc *css_calc_length(arena_t *a, const len_t *l); /* a computed <length-percentage> as a tree */
/* calc-size() (css-sizing-4 §7) from s..s+n: its tree, whose basis is any, a <calc-sum>, a nested calc-size(),
   or one of keywords (bits 1 << CSS_SIZING_*; fit-content() with CSS_SIZING_FIT_CONTENT_FN); NULL: invalid.
   A computed value (P->cx) resolves its relative lengths; neither is resolved further before layout (§7.1) */
struct css_calc *css_calc_size_read(struct css_calc_parse *P, unsigned keywords, const char *s, size_t n);
bool css_calc_is_size(const char *s, size_t n); /* s starts with calc-size( */
/* its basis through its nesting: CSS_SIZING_* (a keyword, FIT_CONTENT_FN, ANY or LENGTH) and that basis's
   tree (LENGTH: the <calc-sum>; FIT_CONTENT_FN: the argument; NULL otherwise) */
int css_calc_size_basis(const struct css_calc *c, const struct css_calc **tree);
/* its used value (§7.1): size is the innermost basis's size (a keyword's, fit-content()'s; LENGTH and ANY
   need none), basis what its percentages are of (0 when that is indefinite) */
double css_calc_size_value(const struct css_calc *c, double size, double basis);
/* cssprop.c: an anchor function's arguments from *s, after its '(' up to and past its ')'; NULL: invalid.
   Its text and equality */
const struct css_anchor_function *css_anchor_function_read(struct css_calc_parse *P, const char **s, const char *e,
                                                           bool size);
bool css_anchor_function_text(sbuf *out, const struct css_anchor_function *f);
bool css_anchor_function_equal(const struct css_anchor_function *a, const struct css_anchor_function *b);
bool css_calc_serialize(sbuf *b, const struct css_calc *c, const char *const *keys); /* §10.13; false: out of memory */
/* a literal or a math function of a type accept allows, in its canonical unit (a percentage that
   resolves against a dimension: of basis, NaN if none); a top-level NaN is 0 (§10.9), infinities are the
   caller's to clamp. a: the parse's memory (NULL: scratch); *kind: the type it is */
bool css_numeric_read(const char *s, size_t n, unsigned accept, const struct cx *cx, arena_t *a, double basis, double *out,
                      unsigned *kind);
/* text whose math functions are each serialized as a specified value (§10.13) and whose numbers,
   percentages and dimensions outside them as CSSOM's component values (0.5em, not .5em), the rest as
   written; false: neither in it, or a value with var() */
bool css_calc_specified(const char *s, size_t n, unsigned accept, sbuf *out);
struct css_corner_radius { len_t axis[2]; }; /* x resolves against width, y against height */
struct style;
bool css_radius_serialize(const struct style *,int,sbuf *,web_doc *);
bool css_radius_specified(const char *,const char *,size_t,sbuf *);
struct css_box_shadow {
    struct css_box_shadow *next;
    float x, y, blur, spread;
    uint32_t color;
    const struct css_color_value *color_value; /* the exact computed colour when 8 bits cannot hold it */
    bool inset;
};
bool css_shadow_serialize(const struct css_box_shadow *, sbuf *, web_doc *);
bool css_shadow_specified(const char *, size_t, sbuf *);
struct shadow_cache;
void web_shadow_cache_free(struct shadow_cache **);
struct radius_cache;
void web_radius_cache_free(struct radius_cache **);
struct clip_cache;
void web_clip_cache_free(struct clip_cache **);
/* A <filter-value-list> (Filter Effects 1 §5, filter and backdrop-filter): its computed value, the
   functions in order with their omitted arguments filled in (§6.2). value is the amount as a number
   (a percentage over 100; grayscale, invert, opacity and sepia clamped to 1), blur's and
   drop-shadow's standard deviation in px, hue-rotate's angle in degrees; x and y are drop-shadow's
   offset in px. Its colour is held as a shadow's (COLOR_CURRENT: currentcolor, resolved against the
   element's colour where it is used). A url() keeps the URL as written. */
enum { CSS_FILTER_URL, CSS_FILTER_BLUR, CSS_FILTER_BRIGHTNESS, CSS_FILTER_CONTRAST, CSS_FILTER_DROP_SHADOW,
       CSS_FILTER_GRAYSCALE, CSS_FILTER_HUE_ROTATE, CSS_FILTER_INVERT, CSS_FILTER_OPACITY, CSS_FILTER_SATURATE,
       CSS_FILTER_SEPIA };
struct css_filter_step {
    uint8_t kind;
    float value, x, y;
    uint32_t color;
    const struct css_color_value *color_value; /* the exact computed colour when 8 bits cannot hold it */
    const char *url;
};
struct css_filter {
    size_t count;
    /* the used amount the opacity() functions multiply to when no other step changes a pixel or
       reads a reference (paint.c composites such a list as group opacity), else 1 */
    float opacity;
    bool effects; /* a step other than opacity() that changes pixels, or a url() */
    struct css_filter_step steps[];
};
/* its computed value (§6.3); st: the element, whose colour currentcolor is (NULL: currentcolor kept) */
bool css_filter_serialize(const struct style *st, const struct css_filter *filter, sbuf *out, web_doc *d);
/* CSSOM: the specified value's serialization (§6.3); false: not none or a <filter-value-list> */
bool css_filter_specified(const char *s, size_t n, sbuf *out);
/* filter.c: the functions' pixels (Filter Effects 1 §13) on premultiplied ARGB. A drop-shadow()'s
   used colour, its currentcolor st's colour */
uint32_t web_filter_shadow_color(const struct style *st, const struct css_filter_step *s);
/* how far f's output reaches past its input on each side (left, top, right, bottom) in px, and how
   far an output pixel reads the input past itself (reach); url() steps add nothing */
void web_filter_extent(const struct css_filter *f, float outset[4], float reach[4]);
/* f's functions in order over the w×h surface px (pitch w); url() steps are the caller's. mirror:
   backdrop-filter's edge mode (Filter Effects 2 §2.1), else transparent past the edges. d (may be
   NULL): checkpoints and cancellation. False: cancelled or out of memory */
bool web_filter_apply(web_doc *d, const struct style *st, const struct css_filter *f, uint32_t *px, int w, int h,
                      bool mirror);
/* color-interpolation-filters (§10): the initial linearRGB is 0 */
enum { CIF_LINEARRGB, CIF_AUTO, CIF_SRGB };
/* The shape values (shape.c): <basic-shape> (css-shapes-1 §3: inset(), circle(), ellipse(),
   polygon(), path(), shape(), with rect() and xywh() computing to inset()), a reference box
   alone, ray() (motion-1 §2.1.1) and <url>. clip-path, shape-outside and offset-path hold the
   same computed value; the property says which of them a value belongs to (its grammar and
   its default reference box). A zero percentage that must stay a percentage (0%, a
   keyword's, a percentage sum that cancels) is a LK_LEN whose pct is -0. */
enum css_shape_property { CSS_SHAPE_CLIP_PATH, CSS_SHAPE_OUTSIDE, CSS_SHAPE_OFFSET_PATH };
enum { CSS_SHAPE_BOX, CSS_SHAPE_INSET, CSS_SHAPE_CIRCLE, CSS_SHAPE_ELLIPSE, CSS_SHAPE_POLYGON,
       CSS_SHAPE_PATH, CSS_SHAPE_SHAPE, CSS_SHAPE_RAY, CSS_SHAPE_URL };
enum { CSS_BOX_BORDER, CSS_BOX_PADDING, CSS_BOX_CONTENT, CSS_BOX_MARGIN, CSS_BOX_FILL,
       CSS_BOX_STROKE, CSS_BOX_VIEW };
/* circle()/ellipse() <radial-extent> and ray()'s <ray-size>; LENGTH: radius[] holds it */
enum { CSS_EXTENT_LENGTH, CSS_EXTENT_CLOSEST_SIDE, CSS_EXTENT_FARTHEST_SIDE, CSS_EXTENT_CLOSEST_CORNER,
       CSS_EXTENT_FARTHEST_CORNER, CSS_EXTENT_SIDES };
/* shape() commands; a control point's anchor (CSS_ANCHOR_POSITION: a <position>, from the origin) */
enum { CSS_CMD_MOVE, CSS_CMD_LINE, CSS_CMD_HLINE, CSS_CMD_VLINE, CSS_CMD_CURVE, CSS_CMD_SMOOTH,
       CSS_CMD_ARC, CSS_CMD_CLOSE };
enum { CSS_ANCHOR_POSITION, CSS_ANCHOR_START, CSS_ANCHOR_END, CSS_ANCHOR_ORIGIN };
struct css_shape_command {
    uint8_t op, controls, radii; /* CSS_CMD_*; curve 1-2, smooth 0-1 control points; arc 1-2 radii written */
    uint8_t anchor[2];           /* CSS_ANCHOR_* of each control point */
    bool by, cw, large;          /* by: relative end point; arc sweep and size */
    len_t end[2];                /* the end point (HLINE: [0], VLINE: [1]) */
    len_t control[2][2];
    len_t radius[2];             /* arc: rx, ry (ry = rx when one was written) */
    float rotate;                /* arc: degrees */
};
struct svg_path_seg;
struct css_shape {
    uint8_t kind, box;           /* CSS_SHAPE_*, CSS_BOX_* */
    bool evenodd;                /* POLYGON, PATH, SHAPE: the fill rule */
    bool at_given;               /* CIRCLE, ELLIPSE, RAY: a position was written */
    bool contain;                /* RAY */
    uint8_t extent[2];           /* CIRCLE: [0]; ELLIPSE: x, y; RAY: [0] (CSS_EXTENT_*) */
    len_t radius[2];             /* CSS_EXTENT_LENGTH radii */
    len_t at[2];                 /* CIRCLE, ELLIPSE, RAY: the centre; SHAPE: the from point */
    float angle;                 /* RAY: degrees */
    len_t inset[4];              /* INSET: top right bottom left */
    struct css_corner_radius corner[4]; /* INSET round: TL TR BR BL; zero without round */
    float round;                 /* POLYGON: the corner rounding, px */
    const char *url;             /* URL: the reference, unescaped */
    size_t count;                /* POLYGON points, PATH segments, SHAPE commands */
    union {
        const len_t (*point)[2];
        const struct svg_path_seg *seg;
        const struct css_shape_command *command;
    };
};
struct cx;
bool css_shape_parse(enum css_shape_property which, const char *v, size_t n, struct cx *cx,
                     const struct css_shape **out, sbuf *specified);
/* computed value (none for NULL); false: out of memory or cancelled, out unchanged */
bool css_shape_serialize(enum css_shape_property which, const struct css_shape *s, sbuf *out, web_doc *d);
bool css_shape_equal(const struct css_shape *a, const struct css_shape *b);
/* a·(1-t) + b·t, or (add) b added onto a, as computed-value text; false: they do not combine
   (the caller falls back to discrete, or b replaces) */
bool css_shape_combine(enum css_shape_property which, const struct css_shape *a, const struct css_shape *b,
                       double t, bool add, sbuf *out, web_doc *d);
/* CSS Shapes 1 (css-shapes-1#shape-outside-property, #shape-margin-property,
   #shape-image-threshold-property): the properties that shape a float's area. A style holds them
   as one group (style_t's float_shape). */
struct css_float_shape {
    const struct css_shape *outside; /* shape-outside; NULL: none */
    len_t margin;                    /* shape-margin */
    float image_threshold;           /* shape-image-threshold */
};
/* Motion Path 1 (motion-1 §2): offset-path (path), offset-distance, offset-position (LK_NORMAL,
   LK_AUTO or a position), offset-anchor (LK_AUTO or a position), offset-rotate. A style holds them
   as one group (style_t's offset). */
struct css_offset_rotate { float angle; bool auto_rotate; }; /* angle in degrees; reverse is auto 180deg */
struct css_offset {
    const struct css_shape *path; /* NULL: none */
    len_t distance;
    len_t position[2], anchor[2];
    struct css_offset_rotate rotate;
};
/* CSS Anchor Positioning 1 (css-anchor-position-1): anchor-name (§2.1), anchor-scope (§2.2),
   position-anchor (§2.4), position-area (§3.1), position-try-fallbacks (§6.1), position-try-order
   (§6.2) and position-visibility (§6.6). A style holds them as one group (style_t's anchor). Names
   are <dashed-ident>s with their escapes resolved.
   The names are tree-scoped (CSS Shadow 1 §4.2, the shadow-names section): each value keeps the tree
   of the style sheet whose declaration gave it (struct cx's tree; NULL the document's, else a shadow
   root), also when it is inherited, and its serialization drops it. anchor-name's names are loosely
   matched (a reference from their tree or a tree inside it finds them), anchor-scope's strictly (only
   anchor names of their own tree, all too); position-anchor, anchor() and anchor-size() and
   position-try-fallbacks' rule names are references, which look from their tree outward. */
struct css_anchor_names {
    const char *const *name;
    uint32_t count;      /* 0: none */
    bool all;            /* anchor-scope: all */
    const node_t *tree;  /* with count or all: the tree the names are from */
};
enum { CSS_ANCHOR_NORMAL, CSS_ANCHOR_NONE, CSS_ANCHOR_AUTO, CSS_ANCHOR_NAMED, CSS_ANCHOR_MATCH_PARENT };
/* kind CSS_ANCHOR_*; name and its tree: CSS_ANCHOR_NAMED */
struct css_position_anchor { const char *name; uint8_t kind; const node_t *tree; };
/* <position-area> (§3.1.2): a keyword of each axis, the block axis's (or the horizontal axis's)
   first; the computed value holds the long logical keywords. css_area_keyword describes one. */
enum { CSS_AREA_X = 1, CSS_AREA_Y, CSS_AREA_BLOCK, CSS_AREA_INLINE, CSS_AREA_SELF_BLOCK, CSS_AREA_SELF_INLINE,
       CSS_AREA_LOGICAL, CSS_AREA_SELF_LOGICAL }; /* families; 0: center and span-all, either axis */
enum { CSS_AREA_START, CSS_AREA_END, CSS_AREA_CENTER, CSS_AREA_SPAN_START, CSS_AREA_SPAN_END, CSS_AREA_SPAN_ALL };
struct css_area_keyword {
    const char *name;
    uint8_t family, track; /* CSS_AREA_X..., CSS_AREA_START... */
    uint8_t writing;       /* x and y keywords: 0 physical, 1 the containing block's writing mode, 2 the box's */
};
const struct css_area_keyword *css_area_keyword(uint8_t keyword); /* NULL: 0 or not one */
/* position-try-fallbacks' try tactics (§6.1), in the order they apply */
enum { CSS_TRY_FLIP_BLOCK = 1, CSS_TRY_FLIP_INLINE, CSS_TRY_FLIP_START, CSS_TRY_FLIP_X, CSS_TRY_FLIP_Y };
struct css_position_try {
    const char *rule;  /* a @position-try rule's name; NULL: the base styles */
    uint8_t tactic[5], tactics;
    uint8_t area[2];   /* a <position-area> option instead; area[0] 0: none */
    /* not a computed value: the cascade with the option's declarations in the position fallback
       origin (§6.4, css.c), for an absolutely positioned box's rule and area options. NULL: the
       base styles, or (rule set) no @position-try rule has the name, and the option is skipped. */
    const struct style *style;
};
/* count 0: none; tree: the tree its rule names are looked up from (with count) */
struct css_position_tries { const struct css_position_try *option; uint32_t count; const node_t *tree; };
enum { CSS_TRY_ORDER_NORMAL, CSS_TRY_ORDER_MOST_WIDTH, CSS_TRY_ORDER_MOST_HEIGHT, CSS_TRY_ORDER_MOST_BLOCK,
       CSS_TRY_ORDER_MOST_INLINE };
enum { CSS_VISIBLE_ANCHOR_VALID = 1, CSS_VISIBLE_ANCHOR_VISIBLE = 2, CSS_VISIBLE_NO_OVERFLOW = 4 }; /* 0: always */
struct css_anchor {
    struct css_anchor_names name, scope;
    struct css_position_anchor position_anchor;
    uint8_t area[2];   /* area[0] 0: none */
    struct css_position_tries tries;
    uint8_t try_order, visibility;
};
/* anchor() and anchor-size() (§3.2, §5.1) in a length: a leaf of its calc() tree. side is a
   CSS_SIDE_* (anchor()) or CSS_SIZE_* (anchor-size()); a percentage side is CSS_SIDE_PERCENT. */
enum { CSS_SIDE_INSIDE, CSS_SIDE_OUTSIDE, CSS_SIDE_TOP, CSS_SIDE_LEFT, CSS_SIDE_RIGHT, CSS_SIDE_BOTTOM,
       CSS_SIDE_START, CSS_SIDE_END, CSS_SIDE_SELF_START, CSS_SIDE_SELF_END, CSS_SIDE_CENTER, CSS_SIDE_PERCENT };
enum { CSS_SIZE_IMPLIED, CSS_SIZE_WIDTH, CSS_SIZE_HEIGHT, CSS_SIZE_BLOCK, CSS_SIZE_INLINE, CSS_SIZE_SELF_BLOCK,
       CSS_SIZE_SELF_INLINE };
struct css_anchor_function {
    bool size;          /* anchor-size() */
    uint8_t side;
    bool percent_function; /* a math function gave the percentage: it serializes as calc() */
    float percent;
    const char *name;   /* NULL: the default anchor */
    const node_t *tree; /* with name: the tree the reference is from */
    bool fallback_given;
    len_t fallback;
};
/* A positioned box's anchor functions resolved (layout.c): for each function, the length it is, or
   false when it is not resolvable (its fallback, if any, is used instead). axis: the property's (0
   horizontal, 1 vertical); inset: which inset property (0-3, top right bottom left), -1 otherwise. */
typedef bool (*css_anchor_resolver)(void *context, const struct css_anchor_function *f, unsigned axis, int inset,
                                    float *out);
/* the length l with every anchor function it holds replaced by what resolve gives, or its fallback;
   false: one has neither, and the declaration is invalid at computed-value time. resolve NULL: no
   function is resolvable (a box that is not absolutely positioned). */
bool css_length_resolve_anchors(arena_t *a, const len_t *l, css_anchor_resolver resolve, void *context,
                                unsigned axis, int inset, len_t *out);
bool css_length_anchored(const len_t *l); /* l holds an anchor function */
void css_length_own(arena_t *a, len_t *l); /* l's calculation, if any, copied into a */
/* the anchor functions of a style that is not absolutely positioned: each computes to its fallback,
   or the property to its initial value (cssprop.c) */
void css_style_anchor_fallbacks(struct style *s, arena_t *a);
/* The accepted @position-try properties (§6.4): the inset, margin, sizing and self-alignment
   properties, position-anchor and position-area. css_style_position_option copies them from a
   position option's style into used (anchor: storage for used's anchor group); css_style_try_tactic
   executes a try tactic on them (§6.5.2), in horizontal-tb ltr; css_style_position_hash digests them
   with position and the position-try longhands, to tell a fallback-sensitive change (§6.5.1). */
struct propdef;
bool css_prop_position_try_accepted(const struct propdef *p);
void css_style_position_option(struct style *used, const struct style *option, struct css_anchor *anchor);
void css_style_try_tactic(struct style *s, uint8_t tactic, struct css_anchor *anchor, arena_t *a);
uint64_t css_style_position_hash(const struct style *s);
bool css_position_area_text(const uint8_t area[2], sbuf *out); /* a computed <position-area> */
/* the groups' initial values (a style's NULL group), and whether two groups hold the same computed
   values (NULL: the initial ones) */
extern const struct css_float_shape css_float_shape_initial;
extern const struct css_offset css_offset_initial;
extern const struct css_anchor css_anchor_initial;
bool css_float_shape_equal(const struct css_float_shape *a, const struct css_float_shape *b);
bool css_offset_equal(const struct css_offset *a, const struct css_offset *b);
bool css_anchor_equal(const struct css_anchor *a, const struct css_anchor *b);
/* CSS Text 3/4 and CSS Text Decoration 4's inherited text properties that few elements set: a style's
   text group (style_t's text; NULL: every one initial). Unlike the other groups it is inherited, and
   as a whole: an element that sets none of them shares its parent's record (css_style_init copies the
   parent), so inheriting it costs a pointer; one that sets one writes a copy. */
enum { CSS_WORD_BREAK_NORMAL, CSS_WORD_BREAK_BREAK_ALL, CSS_WORD_BREAK_KEEP_ALL, CSS_WORD_BREAK_MANUAL,
       CSS_WORD_BREAK_AUTO_PHRASE, CSS_WORD_BREAK_BREAK_WORD };
enum { CSS_LINE_BREAK_AUTO, CSS_LINE_BREAK_LOOSE, CSS_LINE_BREAK_NORMAL, CSS_LINE_BREAK_STRICT,
       CSS_LINE_BREAK_ANYWHERE };
enum { CSS_OVERFLOW_WRAP_NORMAL, CSS_OVERFLOW_WRAP_BREAK_WORD, CSS_OVERFLOW_WRAP_ANYWHERE };
enum { CSS_HYPHENS_MANUAL, CSS_HYPHENS_NONE, CSS_HYPHENS_AUTO };
/* text-underline-position: auto is none of them; at most one of FROM_FONT and UNDER, of LEFT and RIGHT */
enum { CSS_UNDERLINE_FROM_FONT = 1, CSS_UNDERLINE_UNDER = 2, CSS_UNDERLINE_LEFT = 4, CSS_UNDERLINE_RIGHT = 8 };
#define CSS_TEXT_AUTO INT32_MIN /* hyphenate-limit-chars' auto, hyphenate-limit-lines' no-limit */
struct css_text {
    /* text-shadow, the first on top: no spread and never inset (css-text-decor-3 §4); a colour that is
       currentcolor stays COLOR_CURRENT, which each style resolves against its own color */
    const struct css_box_shadow *shadow;
    const char *hyphenate_character; /* UTF-8; NULL: auto */
    int32_t hyphenate_limit_chars[3]; /* the word, before and after the hyphenation point; CSS_TEXT_AUTO: auto */
    int32_t hyphenate_limit_lines;    /* CSS_TEXT_AUTO: no-limit */
    struct css_tab_size { float value; bool length; } tab_size; /* spaces' advances, or (length) px */
    len_t underline_offset;           /* text-underline-offset; LK_AUTO: auto */
    uint8_t word_break, line_break, overflow_wrap, hyphens; /* CSS_WORD_BREAK_* ... CSS_HYPHENS_* */
    uint8_t underline_position;       /* CSS_UNDERLINE_* */
};
extern const struct css_text css_text_initial;
bool css_text_equal(const struct css_text *a, const struct css_text *b);
/* the values of a and b that line breaking and white-space processing read */
bool css_text_layout_equal(const struct css_text *a, const struct css_text *b);
/* text-decoration-color, -style and -thickness: a style's decoration group (style_t's decoration;
   NULL: currentcolor, solid, auto). Not inherited; text-decoration-line is style_t's text_decoration. */
enum { CSS_DECORATION_SOLID, CSS_DECORATION_DOUBLE, CSS_DECORATION_DOTTED, CSS_DECORATION_DASHED,
       CSS_DECORATION_WAVY };
struct css_decoration {
    /* argb COLOR_CURRENT: currentcolor; exact: the computed colour when 8 bits cannot hold it */
    struct css_decoration_color { uint32_t argb; const struct css_color_value *exact; } color;
    len_t thickness;                          /* LK_AUTO: auto; LK_NONE: from-font; a percentage of 1em */
    uint8_t style;                            /* CSS_DECORATION_* */
};
extern const struct css_decoration css_decoration_initial;
bool css_decoration_equal(const struct css_decoration *a, const struct css_decoration *b);
/* A decorating box's lines (css-text-decor-4 §2: an element's own text-decoration-line, drawn over its
   in-flow inline content and propagated to its in-flow descendants, but not into out-of-flow ones nor
   the contents of atomic inlines). A style holds the list of every decorating box whose lines draw
   over its text, its own first; each entry holds the decorating box's used values, so it outlives
   that box's style (descendants borrow the list as they borrow inherited values). */
struct css_decorating {
    const struct css_decorating *next;
    font_t *font;          /* the decorating box's first available font, NULL the bitmap font */
    float font_size;
    float thickness;       /* px; NAN: auto (from-font: the font gives no metric here, so auto too) */
    float underline_offset; /* px; NAN: auto */
    float shift;           /* the decorating box's baseline shift (vertical-align) within its line */
    uint32_t color;        /* the used text-decoration-color */
    uint8_t lines, style, underline_position;
};
bool css_decorating_equal(const struct css_decorating *a, const struct css_decorating *b);
/* the used text-shadow colour of a shadow of st's text group */
uint32_t css_text_shadow_color(const struct style *st, const struct css_box_shadow *shadow);
/* tab-size's tab stop interval in px for st's preserved tabs, given space: the advance of a space
   (letter-spacing and word-spacing included) of their nearest block container (css-text-4 §4.4) */
float css_tab_interval(const struct style *st, float space);
enum css_offset_property { CSS_OFFSET_POSITION, CSS_OFFSET_ANCHOR, CSS_OFFSET_ROTATE, CSS_PERSPECTIVE_ORIGIN };
bool css_offset_parse(enum css_offset_property which, const char *v, size_t n, struct cx *cx, void *out, sbuf *specified);
bool css_offset_serialize(enum css_offset_property which, const void *value, sbuf *out, web_doc *d);
bool css_offset_combine(enum css_offset_property which, const void *a, const void *b, double t, bool add,
                        sbuf *out, web_doc *d);
/* the offset shorthand (motion-1 §2.6): the longhands it sets, false when v is not one */
bool css_offset_shorthand(const char *v, size_t n, struct cx *cx, struct css_offset *out, sbuf *specified);
bool css_offset_shorthand_serialize(const struct css_offset *o, sbuf *out, web_doc *d); /* its computed value */
/* CSSOM specified value of a declared shape, Motion Path, perspective-origin, shape-margin or shape-image-threshold
   value (name: the property); false leaves the authored text */
bool css_shape_specified(const char *name, const char *v, size_t n, sbuf *out);
/* CSS Gap Decorations 1 (css-gaps-1 §3, §4): the lines a container paints in its gaps. A style
   holds them as one group (style_t's gap_rules); axis 0 is the column gaps' (column-rule-*), 1
   the row gaps' (row-rule-*). */
enum css_gap_rule_kind { CSS_GAP_COLOR, CSS_GAP_STYLE, CSS_GAP_WIDTH };
/* one value of a <line-color-list>, <line-style-list> or <line-width-list> (§4.1-4.3) */
union css_rule_value {
    struct { uint32_t argb; const struct css_color_value *exact; } color; /* as a shadow's colour */
    uint8_t style; /* BS_* */
    float width;   /* px, snapped as a border width */
};
/* an item of a list (§4.5): the values [first, first + count) once (repeat 0), repeat times (an
   integer repeater), or as many times as the gaps between the other items need (CSS_RULE_AUTO) */
#define CSS_RULE_AUTO UINT32_MAX
struct css_rule_item { uint32_t repeat, first, count; };
struct css_rule_list {
    const struct css_rule_item *items;
    const union css_rule_value *values;
    uint32_t nitems, nvalues;
};
/* <inset-value> (§3.3): a <length-percentage> of the crossing gap width, or overlap-join */
struct css_rule_inset { len_t length; bool join; };
enum { CSS_RULE_CAP_START, CSS_RULE_CAP_END, CSS_RULE_JUNCTION_START, CSS_RULE_JUNCTION_END };
enum { CSS_RULE_BREAK_NONE, CSS_RULE_BREAK_NORMAL, CSS_RULE_BREAK_INTERSECTION };
enum { CSS_RULE_ITEMS_ALL, CSS_RULE_ITEMS_AROUND, CSS_RULE_ITEMS_BETWEEN, CSS_RULE_ITEMS_NORMAL };
enum { CSS_RULE_ROW_OVER_COLUMN, CSS_RULE_COLUMN_OVER_ROW };
struct css_gap_axis {
    struct css_rule_list list[3];     /* by enum css_gap_rule_kind */
    struct css_rule_inset inset[4];   /* cap-start, cap-end, junction-start, junction-end */
    uint8_t rule_break, visibility;   /* CSS_RULE_BREAK_*, CSS_RULE_ITEMS_* */
};
struct css_gap_rules {
    struct css_gap_axis axis[2];
    uint8_t overlap; /* rule-overlap */
};
extern const struct css_gap_rules css_gap_rules_initial;
/* the same computed values (NULL: the initial ones); layout: those the segments' geometry takes
   (widths, insets, breaks, visibility), whether the style holds the group at all included */
bool css_gap_rules_equal(const struct css_gap_rules *a, const struct css_gap_rules *b);
bool css_gap_rules_layout_equal(const struct css_gap_rules *a, const struct css_gap_rules *b);
/* §4.6: the value list l gives gap index of count gaps */
union css_rule_value css_rule_value_at(const struct css_rule_list *l, uint32_t index, uint32_t count);
/* CSS Backgrounds 3 §2-3, CSS Backgrounds 4 §2-3 and Compositing 1 §9.1: the layered background
   properties a style holds as one group (style_t's background): background-attachment, -clip,
   -origin and -blend-mode, and background-position-x and -y. Each is a list of one value per layer
   as computed, not repeated to the number of layers (css-backgrounds-3 §2.2): layer i uses item
   i mod n (css_bg_keyword, css_bg_offset_used). */
enum css_bg_list { CSS_BG_ATTACHMENT, CSS_BG_CLIP, CSS_BG_ORIGIN, CSS_BG_BLEND, CSS_BG_LISTS };
enum { CSS_BG_SCROLL, CSS_BG_FIXED, CSS_BG_LOCAL };
/* origin and clip: a <visual-box> (CSS_BOX_BORDER, _PADDING, _CONTENT); clip also border-area ||
   text (css-backgrounds-4 §2.1), these bits */
#define CSS_BG_CLIP_AREA 0x10
#define CSS_BG_CLIP_TEXT 0x20
/* <blend-mode> (Compositing 1 §10) */
enum { CSS_BLEND_NORMAL, CSS_BLEND_MULTIPLY, CSS_BLEND_SCREEN, CSS_BLEND_OVERLAY, CSS_BLEND_DARKEN, CSS_BLEND_LIGHTEN,
       CSS_BLEND_COLOR_DODGE, CSS_BLEND_COLOR_BURN, CSS_BLEND_HARD_LIGHT, CSS_BLEND_SOFT_LIGHT, CSS_BLEND_DIFFERENCE,
       CSS_BLEND_EXCLUSION, CSS_BLEND_HUE, CSS_BLEND_SATURATION, CSS_BLEND_COLOR, CSS_BLEND_LUMINOSITY };
struct css_bg_keywords { const uint8_t *v; uint32_t n; };
/* an item of background-position-x or -y (css-backgrounds-4 §3.2): the offset from the start side
   (left, top: a right or bottom offset is stored as its complement), or from the side a logical
   keyword names (x-start, x-end, y-start, y-end), which keeps its keyword */
enum { CSS_BG_EDGE_START, CSS_BG_EDGE_LOGICAL_START, CSS_BG_EDGE_LOGICAL_END };
struct css_bg_offset {
    len_t offset;
    uint8_t edge;
    bool percent; /* a zero offset is 0% (a keyword or a percentage gave it), not 0px */
};
struct css_bg_offsets { const struct css_bg_offset *v; uint32_t n; };
struct css_background {
    struct css_bg_keywords list[CSS_BG_LISTS];
    struct css_bg_offsets position[2]; /* background-position-x, background-position-y */
};
extern const struct css_background css_background_initial;
bool css_background_equal(const struct css_background *a, const struct css_background *b);
static inline uint8_t css_bg_keyword(const struct css_background *bg, enum css_bg_list list, uint32_t layer) {
    return bg->list[list].v[layer % bg->list[list].n];
}
/* layer's position along the axis whose offsets l are (background-position-x or -y, one axis of
   mask-position): its offset in px from the left or top, range being the positioning area less the
   image (what a percentage is of; horizontal-tb, ltr: x-start is left, y-start top) */
float css_bg_offset_px(const struct css_bg_offsets *l, uint32_t layer, float range);
/* CSS Masking 1 §7 (mask-image, -mode, -repeat, -position, -clip, -origin, -size, -composite) and
   §9.2 (mask-type): the mask layers' values a style holds as one group (style_t's mask), none of
   them inherited. Each layered one is a list of one value per layer as computed, as the background
   group's are: the layers are mask-image's, and layer i uses item i mod n of each other list. */
enum css_mask_list { CSS_MASK_MODE, CSS_MASK_REPEAT, CSS_MASK_CLIP, CSS_MASK_ORIGIN, CSS_MASK_COMPOSITE, CSS_MASK_LISTS };
enum { CSS_MASK_MATCH_SOURCE, CSS_MASK_ALPHA, CSS_MASK_LUMINANCE };             /* <masking-mode> */
enum { CSS_COMPOSITE_ADD, CSS_COMPOSITE_SUBTRACT, CSS_COMPOSITE_INTERSECT, CSS_COMPOSITE_EXCLUDE };
/* origin and clip: a <coord-box> (CSS_BOX_*); clip also no-clip */
#define CSS_MASK_NO_CLIP 0x10
/* a <repeat-style> (css-backgrounds-3 §3.4), the item of mask-repeat's list: x << 2 | y */
enum { CSS_REPEAT, CSS_REPEAT_SPACE, CSS_REPEAT_ROUND, CSS_REPEAT_NO };
enum { CSS_MASK_TYPE_LUMINANCE, CSS_MASK_TYPE_ALPHA };                          /* mask-type */
/* a <bg-size> (css-backgrounds-3 §3.9): kind BSZ_*; BSZ_LEN its width and height, LK_AUTO from the
   other and the image's ratio */
struct css_layer_size { len_t v[2]; uint8_t kind; };
struct css_layer_sizes { const struct css_layer_size *v; uint32_t n; };
/* mask-image's <mask-reference>s: none (NULL), or an <image> (css_image: a url() or a gradient) */
struct css_layer_images { const struct css_image *const *v; uint32_t n; };
struct css_mask {
    struct css_layer_images image;
    struct css_bg_keywords list[CSS_MASK_LISTS];
    struct css_bg_offsets position[2]; /* mask-position's horizontal and vertical offsets */
    struct css_layer_sizes size;
    uint8_t type; /* mask-type: CSS_MASK_TYPE_* */
};
extern const struct css_mask css_mask_initial;
bool css_mask_equal(const struct css_mask *a, const struct css_mask *b);
/* some layer's mask-image is not none: the element is masked by its layers and is a stacking
   context (§7.1, §7.10) */
bool css_mask_layers_set(const struct css_mask *m);
static inline uint8_t css_mask_keyword(const struct css_mask *m, enum css_mask_list list, uint32_t layer) {
    return m->list[list].v[layer % m->list[list].n];
}
/* CSS Backgrounds 3 §6 (border-image-*) and CSS Masking 1 §8 (mask-border-*): one record type for
   both groups (style_t's border_image and mask_border), whose initial values differ. */
enum { CSS_BI_STRETCH, CSS_BI_REPEAT, CSS_BI_ROUND, CSS_BI_SPACE };
/* a side of a slice (number: image pixels; length: a percentage of the image), of a width (number:
   a multiple of the border width; length: a length-percentage of the border image area; auto: the
   slice's natural size) or of an outset (number: border widths; length) */
enum { CSS_BI_NUMBER, CSS_BI_LENGTH, CSS_BI_AUTO };
struct css_bi_side { len_t length; float number; uint8_t kind; };
struct css_bi_source { const char *url; const struct gradient *gradient; }; /* both NULL: none */
struct css_bi_slice { struct css_bi_side side[4]; bool fill; };
enum { CSS_MASK_BORDER_ALPHA, CSS_MASK_BORDER_LUMINANCE };
struct css_border_image {
    struct css_bi_source source;
    struct css_bi_slice slice;               /* top right bottom left */
    struct css_bi_side width[4], outset[4];  /* top right bottom left */
    uint8_t repeat[2];                       /* CSS_BI_*: horizontal, vertical */
    uint8_t mode;                            /* mask-border-mode: CSS_MASK_BORDER_* */
};
extern const struct css_border_image css_border_image_initial, css_mask_border_initial;
/* mask: the records are mask-border's, whose NULL is its initial values */
bool css_border_image_equal(const struct css_border_image *a, const struct css_border_image *b, bool mask);
/* the gap decoration segments a container's layout found (layout.c), in its content box: each
   runs along its axis from s to e, centred across it on centre; gap is its place in §4.6's order */
struct gap_segment { float s, e, centre; uint32_t gap; uint8_t axis; };
struct gap_decorations {
    const struct gap_segment *seg;
    uint32_t n;
    uint32_t gaps[2]; /* the gaps of each axis, the count the value lists assign to */
};
/* CSS Basic User Interface 4 (css-ui-4 §5, §6.2, §6.3): the pointer over the element and the
   colours of its caret and of its controls' accents. All inherited; a style holds them as one group
   (style_t's ui), NULL being the initial ones. light-dark() is resolved to one side by the element's
   used color-scheme when the value is computed. */
/* a <cursor-image>'s image (§5.1): url() (one choice, resolution 0), or a <url-set> (image-set() of
   url()s or strings: each choice's resolution in dppx, 0 when it gives none, and its type() or NULL) */
struct css_cursor_choice { const char *url, *type; float dppx; };
struct css_cursor_image {
    const struct css_cursor_choice *choices;
    uint32_t count;
    bool set;    /* written as image-set() */
    bool hot;    /* <x> <y> given: the hot spot, in the image's pixels from its top left */
    float x, y;
};
#define CSS_CURSOR_AUTO 0xFFu /* the shape auto: the user agent's choice by what is under the pointer */
/* cursor: the images to try in order, then the shape (a native N_POINTER_* of abi.h, or auto) */
struct css_cursor {
    const struct css_cursor_image *images;
    uint32_t count;
    uint8_t shape;
};
/* auto | <color>: a computed colour held as a shadow's is (argb COLOR_CURRENT for currentcolor, exact
   when 8 bits cannot hold it) */
struct css_ui_color { uint32_t argb; const struct css_color_value *exact; bool automatic; };
struct css_ui {
    struct css_cursor cursor;
    struct css_ui_color caret[2]; /* caret-color: the caret's, and the text's under a block caret */
    struct css_ui_color accent;   /* accent-color */
};
extern const struct css_ui css_ui_initial;
bool css_ui_equal(const struct css_ui *a, const struct css_ui *b); /* NULL: the initial values */
/* appearance (css-ui-4 §7.2): none, auto, base, base-select, and the <compat-auto> and
   <compat-special> keywords, each its own computed value; every one but none behaves as auto here
   (the controls' native appearance; there is no base appearance) */
enum { CSS_APPEARANCE_NONE, CSS_APPEARANCE_AUTO, CSS_APPEARANCE_BASE, CSS_APPEARANCE_BASE_SELECT,
       CSS_APPEARANCE_SEARCHFIELD, CSS_APPEARANCE_TEXTAREA, CSS_APPEARANCE_CHECKBOX, CSS_APPEARANCE_RADIO,
       CSS_APPEARANCE_MENULIST, CSS_APPEARANCE_LISTBOX, CSS_APPEARANCE_METER, CSS_APPEARANCE_PROGRESS_BAR,
       CSS_APPEARANCE_BUTTON, CSS_APPEARANCE_TEXTFIELD, CSS_APPEARANCE_MENULIST_BUTTON };
/* user-select (css-ui-4 §6.1): its computed value; the used one is paint.c's user_select_used (the pointer's) */
enum { CSS_USER_SELECT_AUTO, CSS_USER_SELECT_TEXT, CSS_USER_SELECT_NONE, CSS_USER_SELECT_CONTAIN,
       CSS_USER_SELECT_ALL };
/* touch-action (Pointer Events 3 §9.1): 0 auto; none; or the pan bits, manipulation */
#define CSS_TOUCH_NONE 0x01
#define CSS_TOUCH_PAN_X 0x02
#define CSS_TOUCH_PAN_LEFT 0x04
#define CSS_TOUCH_PAN_RIGHT 0x08
#define CSS_TOUCH_PAN_Y 0x10
#define CSS_TOUCH_PAN_UP 0x20
#define CSS_TOUCH_PAN_DOWN 0x40
#define CSS_TOUCH_MANIPULATION 0x80
/* Compositing 1 §3 (mix-blend-mode, isolation), CSS Transforms 2 §8-§10 (perspective,
   perspective-origin, transform-style, backface-visibility), CSS Will Change 1 §2.1 (will-change)
   and CSS Containment 2 §2 (contain): not inherited; a style holds them as one group (style_t's
   effects), NULL being the initial values. */
enum { CSS_CONTAIN_SIZE = 1, CSS_CONTAIN_INLINE_SIZE = 2, CSS_CONTAIN_LAYOUT = 4, CSS_CONTAIN_STYLE = 8,
       CSS_CONTAIN_PAINT = 16 };
/* what the properties will-change names would make of the element with a value that is not their
   initial one (§2.1): a stacking context (CSS_WILL_STACK; _TRANSFORM: one that a transform's, not for
   an inline box; _Z: z-index's, for a positioned box or a flex or grid item), a containing block for
   fixed descendants (and absolute ones: CSS_WILL_FIXED_CB, _TRANSFORM as above), for absolute ones
   (position) */
enum { CSS_WILL_STACK = 1, CSS_WILL_STACK_TRANSFORM = 2, CSS_WILL_STACK_Z = 4, CSS_WILL_FIXED_CB = 8,
       CSS_WILL_FIXED_CB_TRANSFORM = 16, CSS_WILL_ABS_CB = 32 };
enum { CSS_TRANSFORM_FLAT, CSS_TRANSFORM_PRESERVE_3D };
enum { CSS_ISOLATION_AUTO, CSS_ISOLATION_ISOLATE };
enum { CSS_BACKFACE_VISIBLE, CSS_BACKFACE_HIDDEN };
struct css_effects {
    len_t perspective;            /* LK_NONE: none; a length, never negative */
    len_t perspective_origin[2];  /* x, y: a length-percentage of the reference box */
    /* will-change: its computed value as it serializes (NULL: auto), and the CSS_WILL_* of the
       properties it names */
    struct css_will_change { const char *text; uint8_t effects; } will_change;
    uint8_t isolation, blend;     /* CSS_ISOLATION_*, mix-blend-mode's CSS_BLEND_* */
    uint8_t transform_style, backface; /* CSS_TRANSFORM_*, CSS_BACKFACE_* */
    uint8_t contain;              /* CSS_CONTAIN_* */
};
extern const struct css_effects css_effects_initial;
bool css_effects_equal(const struct css_effects *a, const struct css_effects *b); /* NULL: the initial values */
/* the values of a and b that layout reads: containment, and what makes a containing block */
bool css_effects_layout_equal(const struct css_effects *a, const struct css_effects *b);
/* A computed shape's geometry against its reference box r (x y w h, any one space). */
void css_shape_ellipse(const struct css_shape *s, const float r[4], double centre[2], double radii[2]); /* circle(), ellipse() */
/* inset(): the inner rectangle (x y w h) and its corner radii TL TR BR BL, scaled to fit (css-backgrounds-3 §5.5) */
void css_shape_inset(const struct css_shape *s, const float r[4], double rect[4], float rx[4], float ry[4]);
/* the outline as absolute SVG path data less (ox, oy): polygon() with its rounded corners, path(), shape() */
bool css_shape_path_data(const struct css_shape *s, const float r[4], double ox, double oy, sbuf *out);
bool css_shape_bounds(const struct css_shape *s, const float r[4], double out[4]); /* x0 y0 x1 y1; false: no outline */
/* css-shapes-1 §4 (the float area): the horizontal extent of the shape grown by margin within the band
   y0..y1; false when the band misses it */
bool css_shape_band(const struct css_shape *s, const float r[4], float margin, float y0, float y1, float *x0, float *x1);
/* motion-1 §2.2: the offset position of an element whose border box is element (x y w h) on its
   offset-path, cb the containing block's reference box for it, and the path's direction there
   (degrees clockwise from the x axis); false when the path has no geometry */
bool css_offset_place(const struct css_offset *o, const float cb[4], const float element[4], double *x, double *y,
                      double *direction);
struct box;
/* paint.c: a box's CSS_BOX_* reference box (x y w h from its border box's top left) and the corner
   radii it takes from the border radii */
void box_reference_box(const struct box *b, uint8_t which, float r[4], float rx[4], float ry[4]);
/* clip (css-masking-1 §6.1): rect() offsets from the border box's top left; auto edges are its edges */
struct css_clip_rect { bool rect; uint8_t auto_edges; float edge[4]; };
bool css_clip_rect_serialize(const struct css_clip_rect *clip, sbuf *out);
static inline bool len_auto(const len_t *l) { return l->kind == LK_AUTO; }
static inline bool len_none(const len_t *l) { return l->kind == LK_NONE; }
static inline bool len_has_pct(const len_t *l) { return l->kind == LK_EXPR || (l->kind == LK_LEN && l->pct != 0); }

enum { D_NONE, D_INLINE, D_BLOCK, D_LIST_ITEM, D_INLINE_BLOCK, D_TABLE, D_INLINE_TABLE, D_TABLE_ROW_GROUP,
       D_TABLE_HEADER_GROUP, D_TABLE_FOOTER_GROUP, D_TABLE_ROW, D_TABLE_CELL, D_TABLE_COLUMN,
       D_TABLE_COLUMN_GROUP, D_TABLE_CAPTION, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID, D_CONTENTS, D_FLOW_ROOT };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { CL_NONE = 0, CL_LEFT = 1, CL_RIGHT = 2, CL_BOTH = 3 };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE, WS_BREAK_SPACES };
enum { TO_CLIP, TO_ELLIPSIS };
enum { TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY, TA_WCENTER /* -webkit-center: also centers blocks */ };
enum { VA_BASELINE, VA_SUB, VA_SUPER, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_TEXT_TOP, VA_TEXT_BOTTOM, VA_LEN };
/* CSS Counter Styles 3 §6: the predefined counter styles a <counter-style-name> matches ASCII
   case-insensitively (§3.1), each an LS_* value; counter.c holds their definitions. A
   list-style-type or counter() style is one of them, none, a <string> (list-style-type only),
   another name (LS_NAMED, case-sensitive; no @counter-style rule defines it, so it is decimal) or
   symbols() (LS_SYMBOLS). */
#define CSS_COUNTER_STYLES(X)                                                                                      \
    X(DISC, "disc") X(CIRCLE, "circle") X(SQUARE, "square") X(DISCLOSURE_OPEN, "disclosure-open")                 \
    X(DISCLOSURE_CLOSED, "disclosure-closed") X(DECIMAL, "decimal") X(DECIMAL_LEADING_ZERO, "decimal-leading-zero") \
    X(ARABIC_INDIC, "arabic-indic") X(ARMENIAN, "armenian") X(UPPER_ARMENIAN, "upper-armenian")                   \
    X(LOWER_ARMENIAN, "lower-armenian") X(BENGALI, "bengali") X(CAMBODIAN, "cambodian") X(KHMER, "khmer")         \
    X(CJK_DECIMAL, "cjk-decimal") X(DEVANAGARI, "devanagari") X(GEORGIAN, "georgian") X(GUJARATI, "gujarati")     \
    X(GURMUKHI, "gurmukhi") X(HEBREW, "hebrew") X(KANNADA, "kannada") X(LAO, "lao") X(MALAYALAM, "malayalam")     \
    X(MONGOLIAN, "mongolian") X(MYANMAR, "myanmar") X(ORIYA, "oriya") X(PERSIAN, "persian")                       \
    X(LOWER_ROMAN, "lower-roman") X(UPPER_ROMAN, "upper-roman") X(TAMIL, "tamil") X(TELUGU, "telugu")             \
    X(THAI, "thai") X(TIBETAN, "tibetan") X(LOWER_ALPHA, "lower-alpha") X(LOWER_LATIN, "lower-latin")             \
    X(UPPER_ALPHA, "upper-alpha") X(UPPER_LATIN, "upper-latin") X(LOWER_GREEK, "lower-greek")                     \
    X(HIRAGANA, "hiragana") X(HIRAGANA_IROHA, "hiragana-iroha") X(KATAKANA, "katakana")                           \
    X(KATAKANA_IROHA, "katakana-iroha") X(CJK_EARTHLY_BRANCH, "cjk-earthly-branch")                                \
    X(CJK_HEAVENLY_STEM, "cjk-heavenly-stem")
enum {
#define CSS_COUNTER_STYLE_ID(id, name) LS_##id,
    CSS_COUNTER_STYLES(CSS_COUNTER_STYLE_ID)
#undef CSS_COUNTER_STYLE_ID
    LS_PREDEFINED, /* the count of the predefined styles */
    LS_NONE = LS_PREDEFINED, LS_STRING, LS_NAMED, LS_SYMBOLS
};
/* symbols() (§6.1): its type and symbols, each a decoded string */
enum { CSS_SYMBOLS_CYCLIC, CSS_SYMBOLS_NUMERIC, CSS_SYMBOLS_ALPHABETIC, CSS_SYMBOLS_SYMBOLIC, CSS_SYMBOLS_FIXED };
struct css_symbols { uint8_t type; uint32_t n; const char *symbol[]; };
/* a counter style as a value names it: id an LS_*; name the LS_STRING string or the LS_NAMED
   name; symbols the LS_SYMBOLS function */
struct css_counter_style { uint8_t id; const char *name; const struct css_symbols *symbols; };
/* CSS Lists 3 §4.1-4.3: counter-reset, counter-increment and counter-set, each a list of
   (name, value) in their order; NULL is none. A reset's value may be reversed() and not given. */
enum { CSS_COUNTER_RESET, CSS_COUNTER_INCREMENT, CSS_COUNTER_SET };
struct css_counter_op { const char *name; int32_t value; bool reversed, valued; };
struct css_counter_ops { uint32_t n; struct css_counter_op op[]; };
struct css_counters { const struct css_counter_ops *ops[3]; }; /* by CSS_COUNTER_* */
extern const struct css_counters css_counters_initial;
/* an <image> of list-style-image or content: a url() (url as the style sheet's base resolves it)
   or a gradient (text: its serialization; gradient: what the background layer's parser reads
   of it, NULL when it does not) */
enum { CSS_IMAGE_URL, CSS_IMAGE_GRADIENT };
/* img: its index in the document's images + 1, found once the image is first used (a marker's:
   counter.c), 0 until then, -1 when it cannot be fetched */
struct css_image { uint8_t kind; const char *url; const char *text; const struct gradient *gradient; int img; };
/* quotes (CSS Generated Content 3 §2.1): pairs of strings, open then close; NULL is auto, no
   pair none */
struct css_quotes { uint32_t pairs; const char *quote[]; };
/* the inherited list and quotation values (CSS Lists 3 §3, CSS Generated Content 3 §2.1) held by
   pointer (style_t's list): list-style-type's name or symbols (its id is style_t's list_style),
   list-style-image (NULL: none) and quotes */
struct css_list {
    const char *type_name;                  /* LS_STRING: the string; LS_NAMED: the name */
    const struct css_symbols *type_symbols; /* LS_SYMBOLS */
    const struct css_image *image;
    const struct css_quotes *quotes;
};
extern const struct css_list css_list_initial;
/* content (CSS Generated Content 3 §1.1): the items of its content list, then those of its
   alternative text. A style's content NULL is normal; css_content_none is none. attr() is
   substituted before the value is parsed (css.c's subst), like var(). */
enum { CSS_CONTENT_STRING, CSS_CONTENT_COUNTER, CSS_CONTENT_COUNTERS, CSS_CONTENT_OPEN_QUOTE,
       CSS_CONTENT_CLOSE_QUOTE, CSS_CONTENT_NO_OPEN_QUOTE, CSS_CONTENT_NO_CLOSE_QUOTE, CSS_CONTENT_IMAGE };
struct css_content_item {
    uint8_t kind;
    const char *text;      /* STRING: the string; COUNTER(S): the counter's name */
    const char *separator; /* COUNTERS */
    struct css_counter_style style; /* COUNTER(S) */
    const struct css_image *image;  /* IMAGE */
};
struct css_content { uint32_t n, alt; bool none; struct css_content_item item[]; }; /* item[n .. n+alt): alt text */
extern const struct css_content css_content_none;
/* the same computed values (NULL: the initial ones, or normal) */
bool css_list_equal(const struct css_list *a, const struct css_list *b);
bool css_counters_equal(const struct css_counters *a, const struct css_counters *b);
bool css_content_equal(const struct css_content *a, const struct css_content *b);
/* whether content holds an item that counters or quotes decide (counter.c writes its text) */
bool css_content_counted(const struct css_content *c);
/* the text of content's strings alone (its counters and quotes left out) */
void css_content_strings(const struct css_content *c, sbuf *out);
/* CSS Counter Styles 3 §2.2: value represented in style (a marker's representation with the style's
   prefix and suffix), appended to out (counter.c) */
void css_counter_text(const struct css_counter_style *style, int32_t value, bool marker, sbuf *out);
enum { BS_NONE, BS_HIDDEN, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_GROOVE, BS_RIDGE, BS_INSET, BS_OUTSET };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
/* text-decoration-line (css-text-decor-4 §2.1): the lines, or one of the two error lines alone */
enum { TD_UNDERLINE = 1, TD_OVERLINE = 2, TD_LINE_THROUGH = 4, TD_BLINK = 8, TD_SPELLING_ERROR = 16,
       TD_GRAMMAR_ERROR = 32 };
enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum { FD_ROW, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE };
enum { FW_NOWRAP, FW_WRAP, FW_WRAP_REVERSE };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
/* AI_NORMAL: normal (and justify-items' legacy), which flex, grid and block layout treat as stretch
   and an anchor-positioned box's position-area replaces (css-anchor-position-1 §4.1); AI_ANCHOR_CENTER:
   justify-self and align-self anchor-center (§4.2), which is center for a box that is not anchor
   positioned. ai_used: the value those other layouts use. */
enum { AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE, AI_ANCHOR_CENTER, AI_NORMAL };
static inline int ai_used(int v) { return v == AI_ANCHOR_CENTER ? AI_CENTER : v == AI_NORMAL ? AI_STRETCH : v; }
enum { AC_NORMAL, AC_STRETCH, AC_FLEX_START, AC_FLEX_END, AC_CENTER,
       AC_BETWEEN, AC_AROUND, AC_EVENLY, AC_START, AC_END };
enum { BR_REPEAT, BR_REPEAT_X, BR_REPEAT_Y, BR_NO_REPEAT };
enum { BSZ_AUTO, BSZ_COVER, BSZ_CONTAIN, BSZ_LEN };

/* grid track sizes: GT_LEN (len), GT_FR (max only; max.px is the factor), GT_AUTO, GT_MIN (min-content),
   GT_MAX (max-content), GT_FIT (max only: fit-content(max)) */
enum { GT_LEN, GT_FR, GT_AUTO, GT_MIN, GT_MAX, GT_FIT };
struct gtrack {
    uint8_t min_kind, max_kind;
    len_t min, max;
};
struct gtemplate {
    int n;
    struct gtrack *t;
    int rep_at, rep_n; /* repeat(auto-fill|auto-fit, ...): tracks [rep_at, rep_at + rep_n), rep_n 0 if none */
    bool rep_fit;      /* auto-fit: repeated tracks without items collapse */
};
struct gareas {
    int rows, cols;
    const char **cell; /* rows * cols area names, NULL for '.' */
};
/* a grid line: GL_LINE n (negative counts from the end), GL_SPAN n, GL_NAME (an area's edge) */
enum { GL_AUTO, GL_LINE, GL_SPAN, GL_NAME };
struct gline {
    uint8_t kind;
    int16_t n;
    const char *name;
};

#define COLOR_CURRENT 0x00FFFFFEu /* placeholder for currentColor while computing */
/* CSS colours (color.c). An absolute colour in the spec's model: a colour
   space, its three components (rgb and xyz 0..1; hsl, hwb: hue in degrees
   then 0..100; lab, lch: L 0..100; oklab, oklch: L 0..1), alpha 0..1, and
   which components are missing (none). legacy: written in a legacy sRGB form
   (hex, named, rgb(), hsl(), hwb(), system colours), serialized as rgb(). */
enum css_color_space { CSS_SPACE_SRGB, CSS_SPACE_SRGB_LINEAR, CSS_SPACE_DISPLAY_P3, CSS_SPACE_DISPLAY_P3_LINEAR,
    CSS_SPACE_A98_RGB, CSS_SPACE_PROPHOTO_RGB, CSS_SPACE_REC2020, CSS_SPACE_XYZ_D50, CSS_SPACE_XYZ_D65,
    CSS_SPACE_LAB, CSS_SPACE_LCH, CSS_SPACE_OKLAB, CSS_SPACE_OKLCH, CSS_SPACE_HSL, CSS_SPACE_HWB, CSS_SPACES };
#define CSS_COLOR_NONE_ALPHA 8u /* none: bit i for component i, this for alpha */
struct css_color { double c[3], alpha; uint8_t space, none; bool legacy; };
enum css_hue_method { CSS_HUE_SHORTER, CSS_HUE_LONGER, CSS_HUE_INCREASING, CSS_HUE_DECREASING };
/* <color-interpolation-method>; legacy: both ends legacy, the result stays legacy sRGB */
struct css_color_interpolation { uint8_t space, hue; bool legacy; };
/* a parsed or computed <color> value: absolute, or an expression of currentcolor */
struct css_color_value;
/* the style's colour properties; the first CSS_SLOT_INHERITED are inherited */
enum css_color_slot { CSS_SLOT_COLOR, CSS_SLOT_FILL, CSS_SLOT_STROKE, CSS_SLOT_INHERITED,
    CSS_SLOT_BACKGROUND = CSS_SLOT_INHERITED, CSS_SLOT_BORDER_TOP, CSS_SLOT_BORDER_RIGHT, CSS_SLOT_BORDER_BOTTOM,
    CSS_SLOT_BORDER_LEFT, CSS_SLOT_OUTLINE, CSS_SLOT_STOP, CSS_SLOTS };
/* a computed colour: current is non-NULL when it depends on currentcolor */
struct css_color_computed { struct css_color color; const struct css_color_value *current; };
/* color-scheme bits */
enum { CSS_SCHEME_LIGHT = 1, CSS_SCHEME_DARK = 2, CSS_SCHEME_ONLY = 4 };
enum { OF_FILL, OF_CONTAIN, OF_COVER, OF_NONE, OF_SCALE_DOWN };
enum { CV_VISIBLE, CV_AUTO, CV_HIDDEN };
enum { CT_NORMAL, CT_INLINE_SIZE, CT_SIZE };
enum { BO_HORIZONTAL, BO_VERTICAL };

struct var_segment;
struct custom_prop {
    const char *name, *value;
    struct custom_prop *next;
    /* Only on the first of one element's own declarations: a name index of
       that element's declarations and the inherited chain after them. */
    const struct var_segment *segment;
    /* the cascade's, while it computes the element (css.c): the declared value and the URL of the
       sheet that declared it (a registered <url> resolves against it), and CUSTOM_* */
    const char *raw, *base;
    uint8_t state;
};
/* custom_prop.state: a registered value waits for the element's font size or colour scheme (the
   early properties) or its colour; it took part in a cycle (through them, or a substitution that
   guarded it again: css-values-5 §8.6); attr() substituted into it (§8.7.3 attr()-tainted); its
   computed value is not known yet this round (a substitution that reaches it computes it then) */
enum { CUSTOM_PENDING = 1, CUSTOM_COLOR = 2, CUSTOM_CYCLE = 4, CUSTOM_TAINTED = 8, CUSTOM_LAZY = 16 };

/* linear-gradient() and radial-gradient(): colour stops along a line, or out from a centre */
#define GRAD_MAX 8
enum { RG_FARTHEST_CORNER, RG_FARTHEST_SIDE, RG_CLOSEST_CORNER, RG_CLOSEST_SIDE };
struct gradient {
    bool radial, repeating, circle;
    int8_t to_x, to_y;   /* linear "to <side or corner>": -1, 0 or 1 each; both 0: use angle */
    uint8_t rsize;       /* RG_* */
    float angle;         /* linear: degrees clockwise from "to top" */
    len_t at[2];         /* radial: the centre, like background-position */
    int n;
    uint32_t col[GRAD_MAX]; /* may be COLOR_CURRENT */
    const struct css_color_value *col_value[GRAD_MAX]; /* COLOR_CURRENT: an expression of it, or NULL */
    len_t pos[GRAD_MAX];    /* LK_AUTO: spread between its neighbours */
};

enum { SVG_PAINT_NONE, SVG_PAINT_COLOR, SVG_PAINT_REF };
struct svg_paint {
    uint32_t color; /* COLOR_CURRENT is resolved against each element, not the SVG root. */
    const char *ref; /* local paint-server fragment; no external resource bypass */
    uint8_t kind;
};

enum { CM_NORMAL, CM_REVERSE, CM_ALTERNATE, CM_ALTERNATE_REVERSE };
enum { CM_FILL_NONE, CM_FILL_FORWARDS, CM_FILL_BACKWARDS, CM_FILL_BOTH };
/* An <easing-function> (css-easing-1): cubic-bezier() (keyword: which named curve, for
   serialization), linear, or steps(steps, position). step-start/step-end are steps(1, …). */
enum { CM_BEZIER, CM_LINEAR, CM_STEPS };
enum { CM_STEP_END, CM_STEP_START, CM_STEP_JUMP_START, CM_STEP_JUMP_BOTH, CM_STEP_JUMP_NONE };
enum { CM_CURVE, CM_EASE, CM_EASE_IN, CM_EASE_OUT, CM_EASE_IN_OUT };
struct css_easing { float x1, y1, x2, y2; uint32_t steps; uint8_t kind, position, keyword; };
enum { CM_COMPOSITE_REPLACE, CM_COMPOSITE_ADD, CM_COMPOSITE_ACCUMULATE };
/* The animation-* and transition-* lists (CSS Animations 1 §3, CSS Animations 2 §4,
   CSS Transitions 1 §2, CSS Transitions 2 §2): one comma-separated list per longhand,
   NULL being the property's initial single value. Times are milliseconds; a NAN duration
   is auto; a NULL name is none; a transition property keeps its identifier as written. */
union css_motion_item { float f; uint8_t k; const char *s; struct css_easing e; };
struct css_motion_list { uint32_t count; union css_motion_item v[]; };
enum { ML_NAME, ML_DURATION, ML_DELAY, ML_EASING, ML_ITERATIONS, ML_DIRECTION, ML_FILL, ML_PLAY, ML_COMPOSITION,
       ML_TPROPERTY, ML_TDURATION, ML_TDELAY, ML_TEASING, ML_TBEHAVIOR, ML_COUNT };
struct css_motion { const struct css_motion_list *list[ML_COUNT]; };
/* One computed transform function. Translation percentages retain the
 * element's border-box reference; every other function is size independent.
 * 3D functions keep their parameters and are flattened orthographically
 * (the z result is discarded) when the used 2D matrix is composed. */
enum { CT_TRANSLATE, CT_SCALE, CT_ROTATE, CT_SKEW, CT_MATRIX, CT_PERSPECTIVE };
/* transform-box. With a CSS layout box, fill-box acts as content-box and
   stroke-box/view-box as border-box (CSS Transforms 1, 6). */
enum { TB_VIEW_BOX, TB_FILL_BOX, TB_STROKE_BOX, TB_CONTENT_BOX, TB_BORDER_BOX };
struct css_transform {
    struct css_transform *next;
    len_t xy[2];  /* CT_TRANSLATE x/y; its z length is v[0] */
    float v[4];   /* SCALE x,y,z; ROTATE axis x,y,z + radians; SKEW x,y radians; PERSPECTIVE length (v[1] 1: none) */
    float m[16];  /* CT_MATRIX, row-major 4x4 */
    uint8_t kind;
};
struct css_translate {len_t xy[2];bool active;};
/* CSS Fonts 4 (with font-size-adjust's metrics of Fonts 5): font-style's keyword (style_t's font_style;
   any but normal selects the italic or oblique faces) and the inherited font values held by pointer
   (style_t's font): a style takes its parent's by the pointer, NULL is all initial. */
enum { CSS_FONT_STYLE_NORMAL, CSS_FONT_STYLE_ITALIC, CSS_FONT_STYLE_OBLIQUE, CSS_FONT_STYLE_LEFT, CSS_FONT_STYLE_RIGHT };
enum { CSS_FONT_KERNING_AUTO, CSS_FONT_KERNING_NORMAL, CSS_FONT_KERNING_NONE };
enum { CSS_FONT_OPTICAL_AUTO, CSS_FONT_OPTICAL_NONE };
enum { CSS_FONT_CAPS_NORMAL, CSS_FONT_CAPS_SMALL, CSS_FONT_CAPS_ALL_SMALL, CSS_FONT_CAPS_PETITE, CSS_FONT_CAPS_ALL_PETITE,
       CSS_FONT_CAPS_UNICASE, CSS_FONT_CAPS_TITLING };
enum { CSS_FONT_POSITION_NORMAL, CSS_FONT_POSITION_SUB, CSS_FONT_POSITION_SUPER };
enum { CSS_FONT_EMOJI_NORMAL, CSS_FONT_EMOJI_TEXT, CSS_FONT_EMOJI_EMOJI, CSS_FONT_EMOJI_UNICODE };
/* font-synthesis-weight, -style, -small-caps, -position: auto, none, and (style only) oblique-only */
enum { CSS_SYNTHESIS_WEIGHT, CSS_SYNTHESIS_STYLE, CSS_SYNTHESIS_SMALL_CAPS, CSS_SYNTHESIS_POSITION, CSS_SYNTHESIS_KINDS };
enum { CSS_SYNTHESIS_AUTO, CSS_SYNTHESIS_NONE, CSS_SYNTHESIS_OBLIQUE_ONLY };
/* font-variant-ligatures' none, beside the bits of its keywords (cssprop.c's tables give each
   keyword of a variant its bit, in the order they serialize; 0 is normal) */
#define CSS_FONT_LIGATURES_NONE 0x8000u
/* font-size-adjust: none, a number, or from-font (a number once the element's first available font
   gives it: css.c), of a metric (font.h's FONT_ASPECT_*) */
enum { CSS_SIZE_ADJUST_NONE, CSS_SIZE_ADJUST_NUMBER, CSS_SIZE_ADJUST_FROM_FONT };
struct css_font_size_adjust { float value; uint8_t kind, metric; };
/* font-feature-settings' and font-variation-settings' computed lists: sorted by tag, each tag once */
struct css_font_tag { char tag[4]; double value; }; /* a <number>'s whole range (an axis value may be large) */
struct css_font_tags { uint32_t n; struct css_font_tag item[]; };
/* font-variant-alternates: each function's argument list as it serializes (NULL: not given) */
enum { CSS_ALT_STYLISTIC, CSS_ALT_STYLESET, CSS_ALT_CHARACTER_VARIANT, CSS_ALT_SWASH, CSS_ALT_ORNAMENTS, CSS_ALT_ANNOTATION,
       CSS_ALT_FUNCTIONS };
struct css_font_alternates { const char *arg[CSS_ALT_FUNCTIONS]; bool historical; };
/* font-palette: normal, light, dark, a <dashed-ident> (name), or palette-mix() of palettes, each with
   its percentage (NAN: not given) in a colour interpolation method (color.c's CSS_SPACE_*, CSS_HUE_*) */
enum { CSS_PALETTE_NORMAL, CSS_PALETTE_LIGHT, CSS_PALETTE_DARK, CSS_PALETTE_NAMED, CSS_PALETTE_MIX };
struct css_palette;
struct css_palette_item {
    const struct css_palette *palette;
    float pct;
    const char *math; /* a math function's percentage as its specified value serializes; NULL: a literal */
};
struct css_palette { uint8_t kind, space, hue; const char *name; uint32_t count; struct css_palette_item item[]; };
struct css_font {
    float width;         /* font-width: a percentage */
    float oblique;       /* font-style oblique's angle, degrees */
    bool oblique_angle;  /* written with its angle (the computed value keeps whether it was) */
    uint8_t caps, position, emoji, kerning, optical_sizing;
    uint8_t synthesis[CSS_SYNTHESIS_KINDS];
    uint16_t ligatures, numeric, east_asian;
    struct css_font_size_adjust size_adjust;
    const struct css_font_alternates *alternates; /* NULL: normal */
    const struct css_font_tags *features, *variations; /* NULL: normal */
    const struct css_palette *palette;            /* NULL: normal */
    char language[5]; /* font-language-override's string without its trailing spaces; "": normal */
};
extern const struct css_font css_font_initial;
bool css_font_equal(const struct css_font *a, const struct css_font *b);
typedef struct style {
    uint8_t display, position, float_, clear, white_space, text_align, vertical_align, list_style,
        list_style_inside, font_style, text_transform, text_decoration, overflow, box_sizing, visibility, pointer_events,
        border_collapse, flex_direction, flex_wrap, justify_content, align_items, align_self, align_content, table_layout,
        content_visibility,text_overflow,
        scroll_behavior; /* CSS Overflow 3: 0 auto, 1 smooth (CSSOM View "perform a scroll") */
    uint8_t border_style[4];
    uint8_t container_type;
    uint8_t box_orient;
    uint8_t transform_box; /* TB_*: the transform-origin/percentage reference box */
    bool legacy_box; /* authored -webkit-box/-webkit-inline-box display */
    uint8_t interpolate_size; /* css-sizing-4 §7.3, inherited: 0 numeric-only, 1 allow-keywords */
    uint32_t line_clamp; /* legacy positive line budget; zero = none */
    const char *container_names; /* authored identifier list, NULL = none */
    uint16_t font_weight;
    uint8_t font_family; /* FONT_FAMILY_*; inherited as one byte */
    const char *font_names; /* full authored family list, computed-style arena */
    font_t *named_font;     /* document-owned downloaded face, never AST-owned */
    const struct web_font_choice *font_choices; /* computed-arena composite family */
    uint8_t object_fit;
    uint8_t appearance, user_select, touch_action; /* CSS_APPEARANCE_*, CSS_USER_SELECT_*, CSS_TOUCH_*: not inherited */
    len_t object_pos[2]; /* position within (content box - fitted image) */
    float font_size;
    len_t line_height;
    float vertical_align_px;
    uint32_t color, bg_color; /* the colours' used values in 8-bit sRGB, for painting */
    /* Computed colours the 8-bit fields cannot hold (color.c): per enum
       css_color_slot, the inherited ones and the element's own. NULL when no
       slot needs one; shared and never written, copied on change. */
    const struct css_color_value *const *color_inherited, *const *color_own;
    /* per enum css_color_slot: the computed value is currentcolor and the 8-bit field holds what
       css_style_finish resolved it to (CSS Color 4 §4.4: currentcolor computes to itself) */
    uint16_t color_current;
    uint8_t color_scheme; /* CSS_SCHEME_* of color-scheme; 0: normal */
    const char *color_scheme_text; /* its computed value; NULL: normal */
    struct svg_paint svg_fill, svg_stroke;
    float svg_fill_opacity, svg_stroke_opacity;
    uint32_t svg_stop_color;
    float svg_stop_opacity;
    len_t svg_stroke_width;
    uint8_t svg_fill_rule, svg_stroke_cap, svg_stroke_join;
    uint8_t svg_clip_rule; /* clip-rule (css-masking-1 §5.2): 0 nonzero, 1 evenodd; inherited */
    uint32_t border_color[4];
    float border_width[4];
    uint32_t outline_color;
    float outline_width, outline_offset; /* width is zero unless outline_style paints */
    uint8_t outline_style; /* BS_* */
    float border_radius;
    struct css_corner_radius corner_radius[4]; /* TL, TR, BR, BL POD; only calc AST needs arena */
    const struct css_box_shadow *box_shadow; /* computed-arena list; initial none */
    len_t margin[4], padding[4], inset[4]; /* top right bottom left */
    len_t width, height, min_width, max_width, min_height, max_height;
    len_t text_indent, flex_basis;
    float flex_grow, flex_shrink;
    float gap_row, gap_col;
    float letter_spacing, word_spacing;
    float border_spacing;
    float opacity;
    const struct css_filter *filter; /* NULL: none; identity effects still establish a context */
    const struct css_filter *backdrop_filter; /* Filter Effects 2 §2; NULL: none */
    uint8_t color_interpolation_filters; /* CIF_*; inherited, initial linearRGB */
    const struct css_shape *clip_path; /* NULL: none; a computed-arena value */
    /* Groups of rarely set properties, all but text not inherited, held by pointer so that every
       element's style does not carry them (css_style_float_shape, css_style_offset read them).
       NULL: all their values initial. A group is a computed-arena record that no one writes once a
       style holds it: cssprop.c writes a longhand into a copy and points the style at the copy. */
    const struct css_float_shape *float_shape; /* shape-outside, shape-margin, shape-image-threshold */
    const struct css_offset *offset;           /* Motion Path */
    const struct css_gap_rules *gap_rules;     /* gap decorations */
    const struct css_anchor *anchor;           /* Anchor Positioning */
    const struct css_background *background;   /* the layered background lists */
    const struct css_border_image *border_image, *mask_border;
    const struct css_ui *ui; /* cursor, caret-color, accent-color: inherited, the group a parent's style shares */
    const struct css_decoration *decoration;   /* text-decoration-color, -style and -thickness */
    const struct css_effects *effects;         /* compositing, 3D, will-change, containment */
    /* an inherited group: the text properties of struct css_text */
    const struct css_text *text;
    /* not computed values: the decorating boxes whose lines draw over this style's text, and the
       baseline shift of its box within its line (css_style_decorations) */
    const struct css_decorating *decorations;
    float baseline_shift;
    struct css_clip_rect clip;
    const struct css_transform *transform;
    struct css_translate translate;
    const struct css_transform *rotate, *scale; /* individual properties; NULL = none */
    len_t transform_origin[2];
    struct css_motion motion; /* animation-* and transition-*; not inherited */
    bool animated; /* the cascade applied animation or transition values to this style */
    int z_index, order;
    const struct css_content *content; /* NULL: normal (none, for ::before and ::after) */
    const struct css_list *list;       /* list-style-type's name, list-style-image, quotes; NULL: initial */
    const struct css_font *font;       /* the inherited CSS Fonts 4 values; NULL: initial */
    const struct css_counters *counters; /* counter-*: a group that is not inherited; NULL: none */
    const char *bg_image; /* url() of background-image (absolute, or relative to the document) */
    int bg_img;           /* its index in the document's images + 1, or 0 (set after the cascade) */
    /* border-image-source's and mask-border-source's url() the same way (set after the cascade) */
    int border_image_img, mask_border_img;
    uint16_t bg_layers;   /* the layers background-image gives (css-backgrounds-3 §2.2), at least 1 */
    uint16_t bg_layer;    /* which of them bg_image or the gradient is (the one painted) */
    uint8_t bg_repeat;    /* BR_* */
    uint8_t bg_size_kind; /* BSZ_* */
    len_t bg_size[2];     /* BSZ_LEN: width, height (LK_AUTO: from the other and the ratio) */
    const struct css_mask *mask; /* the mask layers and mask-type: a group not inherited; NULL: initial */
    uint32_t grad[2];    /* a gradient's first and last colour stops (for things that want one colour) */
    bool has_grad;
    const struct gradient *gradient; /* the gradient itself, when has_grad */
    const struct gtemplate *grid_cols, *grid_rows, *grid_auto_cols, *grid_auto_rows; /* NULL: none / auto */
    const struct gareas *grid_areas;
    struct gline grid_place[4]; /* row-start, column-start, row-end, column-end */
    uint8_t grid_flow_col, justify_items, justify_self;
    uint8_t caption_bottom;
    bool z_auto;
    bool before_generated, after_generated; /* computed pseudo bodies need not generate boxes */
    bool explicit_inherit; /* some property that is not inherited took its parent's value */
    struct custom_prop *vars;
    /* where vars reaches the inherited chain: the entries before it are the element's own (a registered
       property that does not inherit takes only those, else its initial value) */
    const struct custom_prop *vars_inherited;
    struct style *before, *after, *backdrop, *placeholder;
    struct style *marker; /* a list item's ::marker with declarations of its own; NULL: css_style_marker() */
} style_t;
/* a style's float shape and Motion Path values, the initial ones when it holds no group */
static inline const struct css_float_shape *css_style_float_shape(const style_t *s) {
    return s->float_shape ? s->float_shape : &css_float_shape_initial;
}
static inline const struct css_offset *css_style_offset(const style_t *s) {
    return s->offset ? s->offset : &css_offset_initial;
}
static inline const struct css_gap_rules *css_style_gap_rules(const style_t *s) {
    return s->gap_rules ? s->gap_rules : &css_gap_rules_initial;
}
static inline const struct css_anchor *css_style_anchor(const style_t *s) {
    return s->anchor ? s->anchor : &css_anchor_initial;
}
static inline const struct css_list *css_style_list(const style_t *s) {
    return s->list ? s->list : &css_list_initial;
}
static inline const struct css_counters *css_style_counters(const style_t *s) {
    return s->counters ? s->counters : &css_counters_initial;
}
static inline const struct css_font *css_style_font(const style_t *s) {
    return s->font ? s->font : &css_font_initial;
}
/* A list item's ::marker style when the cascade gave it none of its own (CSS Lists 3 §3.1): inherited
   from the item, with the UA style sheet's ::marker rule (white-space: pre, text-transform: none). */
void css_style_marker(style_t *s, const style_t *item);
/* the UA style sheet's ::marker declarations, which the cascade applies under the author's */
void css_style_marker_defaults(style_t *s);
static inline const struct css_background *css_style_background(const style_t *s) {
    return s->background ? s->background : &css_background_initial;
}
static inline const struct css_border_image *css_style_border_image(const style_t *s) {
    return s->border_image ? s->border_image : &css_border_image_initial;
}
static inline const struct css_border_image *css_style_mask_border(const style_t *s) {
    return s->mask_border ? s->mask_border : &css_mask_border_initial;
}
static inline const struct css_mask *css_style_mask(const style_t *s) {
    return s->mask ? s->mask : &css_mask_initial;
}
static inline const struct css_ui *css_style_ui(const style_t *s) {
    return s->ui ? s->ui : &css_ui_initial;
}
/* the used colour of caret-color's caret (auto: currentcolor), and of accent-color (false: auto) */
uint32_t css_ui_caret_color(const style_t *st);
bool css_ui_accent_color(const style_t *st, uint32_t *argb);
static inline const struct css_decoration *css_style_decoration(const style_t *s) {
    return s->decoration ? s->decoration : &css_decoration_initial;
}
static inline const struct css_effects *css_style_effects(const style_t *s) {
    return s->effects ? s->effects : &css_effects_initial;
}
static inline const struct css_text *css_style_text(const style_t *s) {
    return s->text ? s->text : &css_text_initial;
}
/* how far vertical-align raises an inline box's baseline above its parent's (sub, super, a length;
   the line-relative values align the box in layout.c instead) */
static inline float css_style_baseline_shift(const style_t *st) {
    switch (st->vertical_align) {
    case VA_SUB: return -st->font_size * 0.25f;
    case VA_SUPER: return st->font_size * 0.4f;
    case VA_LEN: return st->vertical_align_px;
    }
    return 0;
}

/* ---------------------------------------------------------------- stylesheets */
typedef struct sheet sheet_t;
struct cached_css {
    char *url, *base, *body;
    size_t n;
    uint64_t loading_id; /* request identity survives resource-scan snapshots */
    bool done;
    /* A completed fetch is immutable. Keep its parser ownership independent
       of resource-scan snapshots; only scope/order/media instances are rebuilt. */
    arena_t ast_mem;
    sheet_t *ast;
};
typedef struct css_ctx css_ctx;
/* An @import rule's resolved URL (NULL: not fetched) and media query text;
   href is the URL as written, layer NULL without layer ("" anonymous),
   supports the text inside supports() or NULL. */
struct css_rule_import { const char *url, *media, *href, *layer, *supports; };
/* The CSSOM rule interfaces. Values below 16 are the legacy CSSRule.type
   numbers; the others have none (CSSRule.type reads 0). */
enum css_rule_kind {
    CSS_RULE_STYLE = 1, CSS_RULE_IMPORT = 3, CSS_RULE_MEDIA = 4, CSS_RULE_FONT_FACE = 5,
    CSS_RULE_PAGE = 6, CSS_RULE_KEYFRAMES = 7, CSS_RULE_KEYFRAME = 8, CSS_RULE_MARGIN = 9,
    CSS_RULE_NAMESPACE = 10, CSS_RULE_COUNTER_STYLE = 11, CSS_RULE_SUPPORTS = 12,
    CSS_RULE_FONT_FEATURE_VALUES = 14,
    CSS_RULE_LAYER_BLOCK = 16, CSS_RULE_LAYER_STATEMENT, CSS_RULE_CONTAINER, CSS_RULE_SCOPE,
    CSS_RULE_STARTING_STYLE, CSS_RULE_PROPERTY, CSS_RULE_NESTED_DECLARATIONS,
    CSS_RULE_FONT_PALETTE_VALUES
};
/* Parser-owned CSSOM metadata points at the SAME native cascade AST. An
   ordinary parse records only top-level @import rules (the resource scan
   needs them), so a sheet of fifty thousand rules keeps it small. A CSSOM
   parse records the whole rule tree: every rule the CSS grammar keeps, with
   its parent and children, whether or not the cascade implements it. */
struct css_rule_info {
    struct css_rule_info *next;                  /* next sibling */
    struct css_rule_info *parent, *first, *last; /* CSSOM parses only */
    struct css_rule_info **children;             /* cssom.c's index of first..last */
    /* text: the rule's text_len bytes in the comment-free source of the parse
       that made it (its sheet's, or its top-level rule's own when cssom.c
       spliced that in), not terminated. The other ranges are offsets into text: the trimmed
       prelude (selector, key text, at-rule prelude), the block's contents
       between its braces, and the declarations the rule owns itself. */
    const char *text, *selector;
    const struct css_rule_import *import; /* @import only */
    void *native_rule;
    /* A top-level rule of a CSSOM parse: what it put into the cascade AST's
       lists, which css_cssom_splice takes out or puts in with the rule. */
    struct css_rule_span *span;
    uint32_t text_len, type, id, child_count;
    uint32_t prelude_at, prelude_len, block_at, block_len, decls_at, decls_len;
    uint32_t index;     /* cssom.c: its place among its parent's (or the sheet's) rules */
    bool block, closed; /* has a block; ended by its own '}' or ';', not by end of input */
    bool marker;        /* a nested declarations rule written as CSS_NESTED_DECLARATIONS_MARKER */
    bool exposed;       /* cssom.c: a CSSRule object stands for it */
};
/* cssom.c writes a CSSNestedDeclarations rule that its text cannot otherwise
   keep apart (beside another, or with no declarations) as this at-rule with the
   declarations as its block. The parser takes it as that rule only at the
   offsets cssom.c passes; anywhere else, like any author text, it is an unknown
   at-rule. */
#define CSS_NESTED_DECLARATIONS_MARKER "@-nocturne-nested-declarations"

/* the document's styling state */
struct css_script_registration;
struct styling {
    pvec sheets;   /* sheet_t*, in cascade order */
    css_ctx *ctx;  /* the sheets' rule indexes bound for the current viewport */
    int index_w, index_h;
    bool index_dirty, index_scripting, index_quirks;
    /* css-properties-values-api-1 [[registeredPropertySet]]: CSS.registerProperty()'s, newest first */
    struct css_script_registration *registered;
};

/* imports: receives struct css_import* for each @import, to be fetched and added with their order */
struct css_import {
    char *url;
    double order;
    const struct css_rule_info *owner;
};
sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
sheet_t *css_parse_sheet_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
/* The same parse, also recording the whole CSSOM rule tree (cssom.c), with the
   nested-declarations markers at the sorted offsets markers holds. */
sheet_t *css_parse_cssom_sheet(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url,
                               const uint32_t *markers, uint32_t nmarkers);
/* A change to a CSSOM sheet's rules parses only the text of the top-level rule
   it changes. css_parse_cssom_rules parses comment-free text as top-level rules
   of sheet, in its context (its @namespace prefixes, its layer ids, never an
   @import or @namespace), into sheet's arena: a sheet of its own the rules wait
   in. css_cssom_splice then replaces top[at] (old; NULL: none) of the sheet's
   top-level rules, the count of them before the change, by fresh's one rule
   (NULL: none), its span in every list of the AST with them, and patches the
   rule index of the AST when the rules come after all others (else drops it, to
   be built again at the next cascade). false: the parse cannot be changed
   rule by rule (rules in it stand in no top-level CSSOM rule's span). */
sheet_t *css_parse_cssom_rules(web_doc *d, sheet_t *sheet, const char *css, size_t n, const char *base_url,
                               const uint32_t *markers, uint32_t nmarkers);
bool css_cssom_spliceable(const sheet_t *sheet);
/* the index was dropped (its memory is the arena's garbage) */
bool css_cssom_splice(web_doc *d, sheet_t *sheet, struct css_rule_info *const *top, uint32_t count, uint32_t at,
                      struct css_rule_info *old, sheet_t *fresh);
/* css as the parser reads it: comments out, NUL as U+FFFD (malloc; NULL: no memory).
   A parse of text that went through this has its rules at the same offsets. */
char *css_preprocess(const char *css, size_t n, size_t *out_n);
/* The comment-free source the parse's css_rule_info texts point into. */
const char *css_sheet_source(const sheet_t *sheet, size_t *length);
void css_sheet_scope(sheet_t *sheet, node_t *shadow_root);
/* The arena the sheet was parsed into now lives at another address: the AST's
   rule index is built later in its owning arena, which it names by address. */
void css_sheet_arena_moved(sheet_t *sheet, arena_t *arena);
sheet_t *css_sheet_instance(arena_t *a, const sheet_t *source, double order, node_t *scope);
void css_sheet_import_context(sheet_t *sheet,sheet_t *parent,const struct css_rule_info *owner);
void css_sheet_media(sheet_t *sheet, const char *media);
struct css_rule_info *css_sheet_rules(sheet_t *sheet, uint32_t *length);
uint32_t css_sheet_import_urls(const sheet_t *sheet); /* how many of its rules are @import with a URL */
const char *css_ua_sheet(void);
/* compute every element's style for the viewport */
void css_cascade(web_doc *d, int vw, int vh);
/* Retry failed font sources against the published style snapshot. */
void css_refresh_font_selection(web_doc *d);
/* CSS Font Loading (https://drafts.csswg.org/css-font-loading-3/): the
   @font-face descriptors its FontFace attributes reflect, FontFace records,
   the document's font source and its font matching. The page-facing objects
   are js_font_loading.js; js_fonts.h binds these to it. */
enum css_font_descriptor {
    CSS_FONT_FAMILY, CSS_FONT_STYLE, CSS_FONT_WEIGHT, CSS_FONT_WIDTH, CSS_FONT_UNICODE_RANGE,
    CSS_FONT_FEATURE_SETTINGS, CSS_FONT_VARIATION_SETTINGS, CSS_FONT_DISPLAY, CSS_FONT_ASCENT_OVERRIDE,
    CSS_FONT_DESCENT_OVERRIDE, CSS_FONT_LINE_GAP_OVERRIDE, CSS_FONT_SIZE_ADJUST, CSS_FONT_DESCRIPTORS
};
enum css_font_status { CSS_FONT_UNLOADED, CSS_FONT_LOADING, CSS_FONT_LOADED, CSS_FONT_ERROR };
struct font_face;
struct css_font_face;
/* A CSS-connected FontFace: one @font-face rule of a document style sheet, in
   document order. occurrence counts earlier instances of the same parsed sheet. */
struct css_font_rule { struct font_face *face; arena_t *arena; uint64_t id; uint32_t occurrence; };
/* One face that "find the matching font faces" selected: a rule or a record. */
struct css_font_match { uint64_t id; uint32_t occurrence; struct css_font_face *author; };
/* Validate a descriptor (the family never fails: an invalid name is a string).
   1: valid, *serialized its CSSOM text (malloc); 0: syntax error; -1: allocator. */
int css_font_descriptor(enum css_font_descriptor which, const char *value, size_t n, char **serialized);
/* The document's @font-face rules (malloc array). 1; 0 unavailable now
   (cancelled native work), nothing returned; -1 allocator. */
int css_font_rules(web_doc *d, struct css_font_rule **rules, size_t *count);
const char *css_font_rule_descriptor(const struct font_face *face, enum css_font_descriptor which);
int css_font_rule_set(web_doc *d, const struct css_font_rule *rule, enum css_font_descriptor which, const char *value, size_t n);
/* Status of a rule's or record's sources; started (load()) demands the next
   untried source, as does an earlier source having failed. */
enum css_font_status css_font_rule_status(web_doc *d, struct font_face *face, bool started);
struct css_font_face *css_font_face_new(void);
void css_font_face_release(struct css_font_face *f);
uint64_t css_font_face_id(const struct css_font_face *f);
const char *css_font_face_descriptor(const struct css_font_face *f, enum css_font_descriptor which);
int css_font_face_set(web_doc *d, struct css_font_face *f, enum css_font_descriptor which, const char *value, size_t n);
/* <font-src-list> resolved against base; resources belong to d. 1, 0 syntax error, -1 allocator. */
int css_font_face_source(web_doc *d, struct css_font_face *f, const char *base, const char *value, size_t n);
/* Decode a BufferSource source. 1 decoded, 0 not a font, -1 allocator. */
int css_font_face_data(web_doc *d, struct css_font_face *f, const void *bytes, size_t n);
enum css_font_status css_font_face_status(web_doc *d, struct css_font_face *f, bool load);
/* The document's font source: FontFaces added to document.fonts. */
bool css_font_set_add(web_doc *d, struct css_font_face *f);
void css_font_set_delete(web_doc *d, struct css_font_face *f);
void css_font_set_clear(web_doc *d);
void css_font_set_free(web_doc *d);
/* Find the matching font faces (CSS Font Loading 3.1) for a font shorthand and
   text, without system fonts (they are always loaded). 1, 0 syntax error, -1 allocator. */
int css_font_match(web_doc *d, const char *font, size_t n, const char *text, size_t tn,
                   struct css_font_match **matches, size_t *count);
bool css_containers_update(web_doc *d); /* supervisor, after layout; changed query inputs */
bool css_container_names_valid(const char *s, size_t n);
void css_mark_dirty(web_doc *d, node_t *n);
void css_mark_child_list_dirty(web_doc *d,node_t *parent,node_t *subtree,bool inserted);
/* The latest user input a document received: no input yet, a key or a pointer press. */
enum web_modality { WEB_MODALITY_NONE, WEB_MODALITY_KEYBOARD, WEB_MODALITY_POINTER };
/* The dynamic states css_mark_state_moved moves; :focus and :focus-within move together. */
enum css_state { CSS_STATE_HOVER, CSS_STATE_ACTIVE, CSS_STATE_FOCUS, CSS_STATES };
/* the state moved from one element of d to another (either may be NULL) */
void css_mark_state_moved(web_doc *d, enum css_state state, node_t *from, node_t *to);
/* One attribute of an element changed: its lowercase name and its old and new
   values (NULL: absent). */
struct css_attribute_change { const char *name, *old_value, *new_value; };
/* restyle what selectors naming the attribute can reach; NULL change: unknown */
void css_mark_attribute_dirty(web_doc *d, node_t *n, const struct css_attribute_change *change);
/* a control's value, checkedness or selection changed */
void css_mark_form_state(web_doc *d, node_t *control);
void css_node_style_free(node_t *n);
void css_style_doc_free(web_doc *d);
void css_node_style_unpublish(node_t *n);
void css_styles_release(web_doc *d);
bool css_restyled(const node_t *n);
bool css_style_layout_equal(const style_t *a, const style_t *b);
const struct custom_prop *css_custom_property(const style_t *s, const char *name); /* computed --name, or NULL */
/* ---- registered custom properties (css-properties-values-api-1). cssprop.c reads a registration's
   syntax string (§5) and computes a value against it (§2.4); css.c owns the document's registrations:
   CSS.registerProperty()'s, else the last valid @property of the active sheets (§2.1) */
enum { CSS_SYNTAX_IDENT, CSS_SYNTAX_ANGLE, CSS_SYNTAX_COLOR, CSS_SYNTAX_CUSTOM_IDENT, CSS_SYNTAX_IMAGE,
       CSS_SYNTAX_INTEGER, CSS_SYNTAX_LENGTH, CSS_SYNTAX_LENGTH_PERCENTAGE, CSS_SYNTAX_NUMBER,
       CSS_SYNTAX_PERCENTAGE, CSS_SYNTAX_RESOLUTION, CSS_SYNTAX_STRING, CSS_SYNTAX_TIME,
       CSS_SYNTAX_TRANSFORM_FUNCTION, CSS_SYNTAX_TRANSFORM_LIST, CSS_SYNTAX_URL, CSS_SYNTAX_TYPES };
struct css_syntax_component { uint8_t type; char multiplier; const char *ident; /* IDENT: unescaped */ };
struct css_syntax { unsigned count; struct css_syntax_component component[]; }; /* count 0: universal */
/* §5.4.2 consume a syntax definition: one block from a (NULL: malloc, free()), NULL on failure */
struct css_syntax *css_syntax_parse(const char *s, size_t n, arena_t *a);
bool css_syntax_text(const struct css_syntax *syn, sbuf *out);
/* a component's data type name as the syntax writes it ("<length>"), or an ident component's ident */
const char *css_syntax_type_name(const struct css_syntax_component *c);
/* what a value's computed value depends on, read from its units, keywords and functions */
enum { CSS_SYNTAX_DEP_FONT = 1, CSS_SYNTAX_DEP_ROOT_FONT = 2, CSS_SYNTAX_DEP_LINE_HEIGHT = 4, CSS_SYNTAX_DEP_COLOR = 8,
       CSS_SYNTAX_DEP_SCHEME = 16, CSS_SYNTAX_DEP_CONTAINER = 32, CSS_SYNTAX_DEP_TREE = 64, CSS_SYNTAX_DEP_VIEWPORT = 128,
       CSS_SYNTAX_DEP_REFERENCE = 256 };
unsigned css_syntax_dependencies(const char *s, size_t n);
/* v (var() already substituted) against syn: the index of the component it matched (§5.3: the first
   that does), -1 none. out (NULL: only match) gets its computed value's text; cx is the element (em
   its font size); current is what currentcolor is (NULL keeps it, *deps gets CSS_SYNTAX_DEP_COLOR);
   base resolves a <url> */
int css_syntax_compute(const struct css_syntax *syn, const char *v, size_t n, struct cx *cx, const char *base,
                       const struct css_color *current, sbuf *out, unsigned *deps);
struct css_registration {
    const char *name;
    const struct css_syntax *syntax;
    bool inherits;
    const char *initial; /* the computed initial value; NULL: the guaranteed-invalid value */
};
/* the registration of a custom property name (NULL: none); its records live until the next cascade */
const struct css_registration *css_registration(web_doc *d, const char *name, size_t n);
/* §4.1 registerProperty(): CSS_REGISTER_SYNTAX is a SyntaxError, DUPLICATE an InvalidModificationError */
enum { CSS_REGISTER_OK, CSS_REGISTER_SYNTAX, CSS_REGISTER_DUPLICATE, CSS_REGISTER_MEMORY };
int css_register_property(web_doc *d, const char *name, const char *syntax, bool inherits, const char *initial);
/* st's computed value of a custom property, its registration's rules applied (NULL: guaranteed-invalid) */
const char *css_custom_value(web_doc *d, const style_t *st, const char *name);
/* each custom property st has a computed value for, once (each returns false to stop: false) */
bool css_custom_values(web_doc *d, const style_t *st, bool (*each)(void *arg, const char *name, const char *value),
                       void *arg);
/* the syntax component a registered property's computed value matches (CSS Typed OM reifies it by that
   component's type, css-properties-values-api-1 §6.2); NULL: unregistered or universal, or no match */
const struct css_syntax_component *css_custom_component(web_doc *d, const char *name, const char *value);
void css_box_change_report(web_doc *d); /* debug_js only */
/* An animation sample changes the element's own values only: restyle it, not what selectors match. */
void css_mark_style_own(web_doc *d, node_t *n);
/* v with var() and env() substituted against st's custom properties; false: invalid at computed-value time */
bool css_substitute(web_doc *d, const style_t *st, node_t *node, const char *v, sbuf *out); /* node: what attr() reads */
bool css_easing_parse(const char *s, size_t n, struct css_easing *out);
/* css-easing-1 §2: the output progress for input x; before: the before flag of step easing */
double css_easing_apply(const struct css_easing *e, double x, bool before);
bool css_easing_serialize(const struct css_easing *e, sbuf *out);
bool css_easing_same(const struct css_easing *a, const struct css_easing *b); /* the same function, keyword and all */
/* list's item i, the list repeating (CSS Animations 1 §3: excess values are not used, missing ones
   repeat the list); the initial value when the list is NULL */
union css_motion_item css_motion_item(const style_t *st, int list, uint32_t i);
uint32_t css_motion_count(const style_t *st, int list); /* 1 for an initial (NULL) list */
bool css_motion_equal(const struct css_motion *a, const struct css_motion *b);
/* Shared @media / matchMedia evaluator; optional serialization cap >= 9*n+16. */
bool css_media_evaluate(const char *query, int vw, int vh, bool scripting, char *out, size_t cap);
void css_styling_free(struct styling *st);
void css_registrations_free(struct styling *st); /* at the document's end only */
/* Around a rescan's rebuild of st->sheets: true when it rebuilt the list the
   rule bindings came from, which then stay, with the styles computed from them. */
void css_sheets_rescan(struct styling *st);
bool css_sheets_rescanned(web_doc *d);
/* parse a <color> outside an element into 8-bit sRGB; currentcolor gives
   COLOR_CURRENT; false if it is not one */
bool css_color(const char *s, size_t n, uint32_t *out);
/* The pixel size of the absolute-size keyword xx-small (0) .. xxx-large (7). */
float css_font_keyword_px(unsigned keyword);

/* properties (cssprop.c), used by the cascade in css.c */
struct propdef;
const struct propdef *css_prop_lookup(const char *name, size_t n); /* NULL: not supported */
int css_prop_count(void);
const char *css_prop_name(int index,bool *longhand); /* actual native table, lexicographical order */
/* CSS Cascade 5 legacy name aliases (font-stretch for font-width): the property name n names, NULL for
   any other name; and the aliases by index (NULL past the last), with their properties in *name */
const char *css_prop_legacy_alias(const char *name, size_t n);
const char *css_prop_legacy_alias_at(int index, const char **name);
int css_prop_index(const struct propdef *p);
/* computed before the other declarations, which depend on them: font-size, color-scheme, or a
   shorthand that sets one */
bool css_prop_is_early(const struct propdef *p);
bool css_prop_is_motion(const struct propdef *p);
/* CSSOM computed value of a longhand the table stores whole (keyword, length, pixels, number,
   integer, colour, color-scheme, shape, Motion Path, and the offset shorthand): 1 with the text in
   out; -1 out of memory; 0 for any other property, whose value has its own serializer or none. */
int css_computed_longhand(const style_t *st, const char *name, sbuf *out, web_doc *d);
/* CSSOM specified value of a declared colour or color-scheme; false: keep the authored text */
bool css_specified_longhand(const char *name, const char *v, size_t n, sbuf *out);
/* CSS Typed OM §5: what a property's value reifies as by its grammar (cssprop.c's table): always a
   CSSStyleValue, a colour (only currentcolor is a keyword), a transform list, an image layer, or by the
   value (an identifier a keyword, a numeric value of a type accept takes a CSSNumericValue); whether it
   is a shorthand or list-valued. false: no such property. */
enum { CSS_REIFY_VALUE, CSS_REIFY_STYLE, CSS_REIFY_COLOR, CSS_REIFY_TRANSFORM, CSS_REIFY_IMAGE };
/* accept: the CSS_CALC_* types a lone numeric value of the property takes; typed: accept is all of
   them (0: the grammar takes no lone numeric value), else the grammar is not known that far;
   integer: a lone number must be an integer */
struct css_reify { uint8_t kind; bool shorthand, list, typed, integer; unsigned accept; };
bool css_prop_reify(const char *name, size_t n, struct css_reify *out);
/* Animation values (cssprop.c). The property table owns each longhand's animation type
   (Web Animations 1 §5.3): not animatable, discrete, by computed value, visibility (visible
   wins inside the interval), or display (the value that is not none/hidden wins inside it;
   discrete otherwise). */
enum { CSS_ANIM_NONE, CSS_ANIM_DISCRETE, CSS_ANIM_VALUE, CSS_ANIM_VISIBILITY, CSS_ANIM_DISPLAY };
int css_prop_animation(const struct propdef *p);
/* the longhands a property sets (itself, or a shorthand's), as css_prop_index bits in set */
void css_prop_longhands(const struct propdef *p, uint8_t *set);
/* CSSOM's shorthands (a declaration block holds longhands). css_shorthand_longhands: the names
   of the longhands shorthand p maps to, in canonical order (logical ones by their own names),
   0 for a longhand. css_shorthand_expand: each longhand's name and the text value v gives it;
   1, 0 invalid, -1 a value with no longhand text (the block keeps the shorthand as one),
   -2 no memory. css_shorthand_serialize: the shorthand's value from its longhands' values
   (serialized, in css_shorthand_longhands order); false when it cannot represent them.
   css_shorthands_of: the shorthands a longhand name is in, in CSSOM's preferred order. */
typedef void css_longhand_record(void *opaque, const char *name, const char *value, size_t n);
int css_shorthand_longhands(const struct propdef *p, const char *const **names);
int css_shorthand_expand(const struct propdef *p, const char *v, size_t n, css_longhand_record *each, void *opaque);
bool css_shorthand_serialize(const struct propdef *p, const char *const *values, const size_t *lengths, sbuf *out);
int css_shorthands_of(const char *longhand, const struct propdef *const **shorthands);
/* the logical property group (CSS Logical 1) of a longhand name into group, and whether its
   mapping is flow-relative; false: in none */
bool css_prop_logical_group(const char *name, char *group, size_t size, bool *logical);
/* p's computed value in st as text css_apply reads back to the same value; false: no serializer */
bool css_computed_value(const style_t *st, const struct propdef *p, sbuf *out, web_doc *d);
/* do a and b hold the same computed value of longhand p */
bool css_anim_equal(const struct propdef *p, const style_t *a, const style_t *b);
/* An element's animation context: its style without effects (the underlying values and
   what em/currentcolor resolve against) and its last box (matrix decomposition). */
struct box;
struct css_anim_ctx {
    const style_t *base, *parent;
    node_t *node;
    const struct box *box;
    float rem, vw, vh;
    arena_t *a;
    web_doc *d;
};
/* value text (var() already substituted) parsed into a copy of the base; computed: the text is
   this layer's own (css_anim_interpolate, css_anim_compose), whose values may lie outside what a
   style sheet's parse clamps until the final value; set: the longhands it set (css_prop_count
   bytes, may be NULL) */
bool css_anim_parse(const struct css_anim_ctx *c, const struct propdef *p, const char *v, size_t n, bool computed,
                    style_t *out, uint8_t *set);
/* CSS Values 4 §3: longhand p's value a·(1-t) + b·t as computed-value text (t may lie outside
   [0, 1]); false: the pair does not interpolate and the caller falls back to discrete */
bool css_anim_interpolate(const struct css_anim_ctx *c, const struct propdef *p, const style_t *a,
                          const style_t *b, double t, sbuf *out);
/* discrete interpolation (Web Animations 1 §5.3, with the visibility and display rules):
   true when b (rather than a) is the value at t */
bool css_anim_discrete_second(const struct propdef *p, const style_t *a, const style_t *b, double t);
/* value added (or accumulated) onto underlying; false: the type is not additive (value replaces) */
bool css_anim_compose(const struct css_anim_ctx *c, const struct propdef *p, const style_t *underlying,
                      const style_t *value, bool accumulate, sbuf *out);
/* css-properties-values-api-1 §2.5, a custom property: by computed value, as the type its values parsed
   as against syn (cssprop.c); false: discrete (unregistered, the universal syntax, a type that does not
   interpolate, or lists of different lengths) */
bool css_custom_interpolate(const struct css_anim_ctx *c, const struct css_syntax *syn, const char *a, const char *b, double t,
                            sbuf *out);
bool css_custom_compose(const struct css_anim_ctx *c, const struct css_syntax *syn, const char *underlying, const char *value,
                        bool accumulate, sbuf *out);
/* the cascade's (css.c): a registered custom property's typed syntax, NULL when its values animate
   discretely; a keyframe value of custom property name at a site, its var() and attr() substituted against
   the base style, a CSS-wide keyword defaulted and a registered one computed: 1 the computed value into
   out, 0 the guaranteed-invalid value, -1 no value (invalid: no keyframe) */
const struct css_syntax *css_custom_syntax(web_doc *d, const char *name);
int css_custom_animation_value(const struct css_anim_ctx *c, const char *name, const char *text, sbuf *out);
const struct propdef *css_prop_at(int index);
/* animation.c, the one time model (animation.h), as the cascade sees it. A site is one element or
   one of its pseudo-elements in one cascade. */
struct web_effect; struct web_animation; struct web_animations; struct web_keyframe;
struct web_motion_site {
    struct css_anim_ctx ctx;  /* .base: the site's style without effects */
    uint8_t pseudo;
    /* the keyframes of the @keyframes rule called name for this site, in ctx.a (easing: the
       keyframes' default); NULL: there is no such rule */
    const struct web_keyframe *(*keyframes)(void *arg, const char *name, const struct css_easing *easing, uint32_t *count);
    void (*emit)(void *arg, const struct propdef *p, const char *value); /* one longhand's sampled value */
    /* one custom property's sampled computed value; NULL: the guaranteed-invalid value */
    void (*emit_custom)(void *arg, const char *name, const char *value);
    void *arg;
    /* the custom properties the animation origin set (web_motion_sample fills them, in ctx.a): the
       transition origin leaves them alone, as it does the animated longhands */
    const char **animated_customs;
    uint32_t nanimated_customs;
};
/* 1: animations may apply or update (animation-name, owned CSS animations, script effects);
   2: transitions may (transition durations, owned transitions). base NULL: the records only. */
int web_motion_wanted(const node_t *n, uint8_t pseudo, const style_t *base);
void web_motion_animations(struct web_motion_site *site);      /* CSS Animations 2 §3 */
/* the effect stack into the animation origin (transitions false) or the transition origin;
   animated: css_prop_count() flags, set by the first, skipped by the second */
size_t web_motion_sample(struct web_motion_site *site, bool transitions, uint8_t *animated);
void web_motion_transitions(struct web_motion_site *site, const style_t *before, const style_t *after, const uint8_t *animated);
void web_motion_cancel(node_t *n, int pseudo);                  /* style canceled them; pseudo -1: all */
void web_animations_cascading(web_doc *d, bool on);
void web_animations_tick(web_doc *d, uint64_t now);
int64_t web_animations_deadline(web_doc *d);                   /* absolute uptime_ms(), as web_deadline */
void web_animations_doc_free(web_doc *d);
void web_animations_node_free(node_t *n);
void web_animations_set_origin(web_doc *d, uint64_t origin);   /* the time origin of performance.now() */
bool web_animations_script_work(web_doc *d);                   /* events or promises for the script facade */
/* the values a declaration is resolved against */
struct cx {
    style_t *s;
    const style_t *parent;
    node_t *node;
    float em, rem, vw, vh; /* em: the element's font size (its parent's while computing font-size) */
    arena_t *a;            /* for computed values that need memory */
    uint8_t *set;          /* per property: already given a value by a more important declaration */
    bool font_pass;        /* only the early properties (font-size, color-scheme) are being computed */
    bool supports_probe, invalid; /* scratch validation: never touch a document */
    float cq_width, cq_height;
    bool cq_width_set, cq_height_set;
    bool absolute_lengths; /* relative lengths and percentages fail (Geometry's matrix strings) */
    bool computed_text;    /* the value is computed-value text the animation layer wrote (css_anim_parse):
                              what a style sheet's parse clamps (rgb()'s components) keeps its value */
    uint8_t anchor_functions; /* LF_ANCHOR, LF_ANCHOR_SIZE: the anchor functions the length being parsed may hold */
    uint8_t anchor_depth;     /* the anchor functions' fallbacks the length being parsed is in */
    css_longhand_record *record; /* when set: each longhand a shorthand gives a value, by name, with its text */
    void *record_opaque;
    bool tree_mismatch;       /* the declaration is from an outer tree than node's (css-values-5 §10: a
                                 tree-counting function's reference fails to match) */
    const node_t *tree;       /* the tree of the declaration's style sheet, which its tree-scoped names
                                 and references keep (CSS Shadow 1 §4.2): NULL the document's, else a
                                 shadow root (css_tree_scope) */
};
/* the tree scope of a node's tree, as struct cx's tree names it: NULL the document's (or no
   shadow root's), else the shadow root */
static inline const node_t *css_tree_scope(const node_t *n) {
    while (n && n->parent) n = n->parent;
    return n && n->shadow_host ? n : NULL;
}
/* apply a declaration (var() already substituted) unless its properties are all set; false if invalid */
bool css_apply(const struct propdef *p, const char *v, size_t n, struct cx *cx);
bool css_value_supported(const struct propdef *p, const char *value, size_t n);
bool css_value_valid(const struct propdef *p, const char *value, size_t n);
const char *css_value_canonical(arena_t *a, const char *value, size_t n, size_t *out_n);
bool css_supports_declaration(const char *property, size_t pn, const char *value, size_t vn);
bool css_valid_declaration(const char *property, size_t pn, const char *value, size_t vn);
bool css_supports_condition(const char *condition, size_t n, bool implied_parens);
/* whether the condition matches the @supports grammar, true or false */
bool css_supports_grammar(const char *condition, size_t n, bool implied_parens);
/* start a style: inherited properties from the parent, the others initial */
void css_style_init(style_t *s, const style_t *parent);
/* s takes from's inherited values (what css_style_init copies from a parent)
   and keeps its own others */
void css_style_adopt_inherited(style_t *s, const style_t *from);
/* computed-value fixups after all declarations: currentColor, blockification, border widths */
void css_style_finish(style_t *s, const style_t *parent, bool root);

/* colours (color.c) */
struct css_color css_color_from_argb(uint32_t argb);       /* legacy sRGB */
uint32_t css_color_argb(const struct css_color *c);        /* the used value: gamut mapped, 8 bits */
/* CSS Color 4, 13: interpolate a to b at progress p (not clamped: animation may extrapolate) */
struct css_color_interpolation css_color_default_interpolation(const struct css_color *a, const struct css_color *b);
struct css_color css_color_interpolate(const struct css_color *a, const struct css_color *b, double p,
                                       const struct css_color_interpolation *m);
/* CSS Values 4, 3.3: b added to a (accumulation alike), premultiplied in m's space; alpha clamps */
struct css_color css_color_add(const struct css_color *a, const struct css_color *b, const struct css_color_interpolation *m);
/* the animation layer's computed-value text (cx.computed_text reads it back exactly) */
bool css_color_animation_text(const struct css_color *c, sbuf *out);
bool css_color_computed_equal(const struct css_color_computed *a, const struct css_color_computed *b);
/* the used colour of a computed one, currentcolor being current */
void css_color_resolve(const struct css_color_computed *v, const struct css_color *current, struct css_color *out);
/* CSS Color 4, 16: a computed or used colour; html: as HTML's canvas serializes (#rrggbb when it can) */
bool css_color_serialize(const struct css_color *c, bool html, sbuf *out);
/* a <color> outside a style sheet (canvas). Only a value that names currentcolor, a system colour
   or light-dark() asks element(context) (may be NULL) for the style whose colour and scheme it
   takes; without one, black and light. */
bool css_color_parse_used(const char *s, size_t n, const style_t *(*element)(void *), void *context,
                          struct css_color *out);
/* CSSOM: the specified value's serialization; false (out unchanged) when s is not a <color> */
bool css_color_specified(const char *s, size_t n, sbuf *out);
/* CSS Color 4 §12 <color-interpolation-method>, "in <space> [<hue-method> hue]?", the hue method only
   for a polar space (xyz is xyz-d65); and its text as color-mix() serializes it (CSS Color 5 §2.4):
   nothing for oklab, no hue method when it is shorter (false: nothing written). palette-mix() takes
   both (CSS Fonts 5). */
bool css_color_method_read(const char *s, size_t n, uint8_t *space, uint8_t *hue);
bool css_color_method_text(sbuf *out, uint8_t space, uint8_t hue);
/* the percentages color-mix() (CSS Color 5 §2.4) and palette-mix() serialize, from their items' given
   percentages (NAN: omitted) and which are math functions (written as they are): put[i] is the one item
   i writes, NAN when it writes none. An omitted one is the rest of 100% shared between the omitted,
   unless one is a math function; none is written when each is the even share. */
void css_mix_percentages(unsigned count, const double *given, const bool *math, double *put);
/* CSS Typed OM §5.7: a <color> as written (color.c). KEYWORD: name (currentcolor, a system colour);
   a function's channels as authored (a math one's serialization in math at math_at), a hex or named
   colour RGB with its bytes and its alpha as a unitless fraction; OTHER: relative colours and mixes.
   false: not a colour */
enum { CSS_COLOR_REIFY_KEYWORD, CSS_COLOR_REIFY_RGB, CSS_COLOR_REIFY_HSL, CSS_COLOR_REIFY_HWB, CSS_COLOR_REIFY_LAB,
       CSS_COLOR_REIFY_LCH, CSS_COLOR_REIFY_OKLAB, CSS_COLOR_REIFY_OKLCH, CSS_COLOR_REIFY_COLOR, CSS_COLOR_REIFY_OTHER };
enum { CSS_COLOR_CHANNEL_ABSENT, CSS_COLOR_CHANNEL_NONE, CSS_COLOR_CHANNEL_VALUE, CSS_COLOR_CHANNEL_MATH };
struct css_color_channel { uint8_t kind; char unit[7]; double v; size_t math_at, math_n; };
struct css_color_reified {
    uint8_t kind;
    const char *name, *space; /* KEYWORD's name; COLOR's colour space */
    struct css_color_channel ch[4];
};
bool css_color_reify(const char *s, size_t n, struct css_color_reified *out, sbuf *math);
/* a computed colour held as its 8-bit field argb (COLOR_CURRENT: currentcolor, current the 8-bit
   colour out->color gives it meanwhile) and, when 8 bits cannot hold it, its exact value or
   expression v (a style's slot, a shadow's colour) */
void css_color_held(uint32_t argb, const struct css_color_value *v, uint32_t current, struct css_color_computed *out);
/* a style's computed colour and its CSSOM resolved value (the used colour) */
void css_style_color(const style_t *st, enum css_color_slot k, struct css_color_computed *out);
bool css_style_color_text(const style_t *st, enum css_color_slot k, sbuf *out);
bool css_style_set_color(style_t *st, enum css_color_slot k, const struct css_color_computed *v, arena_t *a);
/* the cascade (cssprop.c): parse and compute a <color> against cx (current: what currentcolor is, NULL to
   keep it). *argb is the used value or COLOR_CURRENT; *computed the exact value when 8 bits cannot hold it */
bool css_color_value_compute(const char *s, size_t n, struct cx *cx, const struct css_color *current,
                             uint32_t *argb, const struct css_color_value **computed);
bool css_color_apply(const char *s, size_t n, struct cx *cx, enum css_color_slot k);
void css_color_slot_copy(style_t *dst, const style_t *src, enum css_color_slot k, arena_t *a);
/* a slot's exact computed value beside the 8-bit field the caller wrote (NULL: the field holds it) */
void css_color_slot_set(style_t *s, enum css_color_slot k, const struct css_color_value *v, arena_t *a);
void css_color_finish(style_t *s); /* expressions of currentcolor against the element's colour */
uint32_t css_color_value_argb(const struct css_color_value *v, uint32_t current);
bool css_color_value_text(const struct css_color_value *v, uint32_t argb, sbuf *out);
bool css_color_value_specified(const struct css_color_value *v, sbuf *out);
bool css_color_value_equal(const struct css_color_value *a, const struct css_color_value *b);
bool css_color_slots_equal(const style_t *a, const style_t *b);
/* the element's used color-scheme is dark (light-dark() takes its second value) */
bool css_style_dark(const style_t *s);
/* unescape a CSS string or identifier body (backslash escapes) */
void css_unescape(sbuf *out, const char *s, size_t n);
/* One decoded family at a time; no fixed family-count/identifier truncation.
   cursor=NULL means malformed input; cursor=end means a complete list. */
const char *css_font_family_next(arena_t *a, const char **cursor, const char *end);

/* ---------------------------------------------------------------- boxes */
enum { B_BLOCK, B_INLINE, B_TEXT, B_ATOMIC, B_TABLE, B_ROW_GROUP, B_ROW, B_CELL, B_CAPTION, B_BR, B_FLEX, B_GRID };
enum { AT_NONE, AT_IMG, AT_INLINE_BLOCK, AT_INPUT, AT_CHECKBOX, AT_RADIO, AT_BUTTON_INPUT, AT_SELECT,
       AT_TEXTAREA, AT_SVG, AT_PLACEHOLDER };

struct run;  /* a positioned piece of text */
struct deco; /* a background/border span of an inline box on one line */
struct line_fragment;
/* Numeric-only, debug_js diagnostics. Allocated in the box's private arena;
 * helpers never call the console or publish an incomplete layout. */
struct clamp_trace {
    float usedh, sh, full_h, cut_h, post_h, last_y, line_bottom;
    uint32_t limit, lines;
    uint8_t last_kind;
    bool eligible, applied, last_anon, cb_reached;
};

typedef struct box {
    uint8_t kind, atomic;
    bool anon, inline_ctx, is_bfc, abspos, floated, is_marker;
    bool wrapped_inner; /* an inline-level flex/grid/table's inner box: its parent, the node's box, is the outer one */
    node_t *node; /* NULL for anonymous boxes */
    style_t *st;
    struct box *parent, *first, *last, *next;
    arena_t *branch_mem; /* a locally generated subtree root owns the arena its subtree was built in */
    uint32_t epoch; /* the local publication that generated it (0: a whole build) */
    bool local_kept; /* transient: a splice keeps this child */
    /* text boxes: the white-space-processed text */
    const char *text;
    size_t len;
    /* geometry: content box, relative to the containing block's content box */
    float x, y, w, h;
    float m[4], p[4], b[4]; /* used margin, padding, border widths */
    float content_dy;       /* table cells: vertical-align shift of the content */
    float rel_dx, rel_dy;   /* position: relative */
    float scroll_w, scroll_h; /* padding-edge scrolling area, unscrolled layout */
    float baseline;         /* first baseline, relative to the content box top (-1: none) */
    float last_baseline;    /* last baseline (inline-blocks align by it) */
    /* absolutely positioned boxes: where they would have been (relative to static_cb) */
    float static_x, static_y;
    struct box *static_cb;
    uint32_t gen;           /* layout pass that listed it */
    uint32_t abs_placed;    /* layout pass that placed it, which an anchor-positioned box may do early */
    struct box *cb;         /* the block box these coordinates are relative to */
    struct box *inline_paint_owner, *inline_paint_source;
    int inline_run_first, inline_run_last, inline_deco_first, inline_deco_last;
    /* Its line fragments from its formatting root's last line layout: a text box's runs, an
       inline box's decos, as [frag_first, frag_end) of that root's arrays (frag_end 0: none).
       Other boxes' fragments may lie between; each lookup still checks the owner. */
    int frag_first, frag_end;
    bool inline_cb_valid;
    float inline_cb_left, inline_cb_top, inline_cb_right, inline_cb_bottom;
    uint64_t paint_bounds_generation;
    float paint_bounds[4]; /* document-space conservative subtree bounds, one paint only */
    float flow_ink[4]; /* paint.c flow_ink: what its own paint recursion draws, relative to its visual position */
    uint64_t flow_ink_layout; /* the layout_revision it was measured at */
    bool flow_ink_valid; /* cleared on the restyle path, with paint_layers_valid */
    bool flow_ink_layers; /* a layer below may paint with a stacking context instead */
    uint32_t visual_memo_x, visual_memo_y; /* box_visual_memo generation of the memoized coordinate below */
    float visual_x, visual_y;
    uint32_t sticky_memo;   /* the same for the position: sticky offset below */
    float sticky_dx, sticky_dy;
    bool paint_bounds_valid;
    bool paint_alpha; /* existing build/restyle traversal aggregates real subtree alpha effects */
    pvec paint_layers; /* stable stacking order; owned until box retirement */
    uint64_t paint_layers_layout, paint_layers_style; /* box tree layout + published styles (stacking contexts) */
    bool paint_layers_valid;
    struct shadow_cache *shadow_cache; /* geometry-only Gaussian masks; box lifetime */
    struct radius_cache *radius_clip[3]; /* border, padding, replaced-content clip masks */
    struct clip_cache *clip_cache; /* clip-path's shape coverage; box lifetime */
    /* paint.c filter_used: whether a url() of its filter list names no <filter> (Filter Effects 1
       §5: the whole list is then ignored), decided for that list at that dom_revision */
    const struct css_filter *filter_ref_list;
    uint64_t filter_ref_dom;
    bool filter_ref_ignored;
    bool style_sync_dirty; /* retirement join: this subtree has changed style borrows */
    bool style_sync_subtree; /* and every box below may borrow one: a display:contents element generated them */
    /* inline formatting results (block containers with inline content) */
    struct run *runs;
    int nruns;
    struct deco *decos;
    int ndecos;
    struct line_fragment *lines;
    int nlines;
    /* a grid or flex container's gap decoration segments (layout_mem), NULL when its style holds
       no gap_rules group */
    const struct gap_decorations *gap_decorations;
    bool clamp_hidden, clamp_truncated;
    bool ellipsis_hidden; /* paint-only elision of an atomic inline, never hit/layout */
    uint64_t ellipsis_hidden_revision;
    uint64_t ellipsis_layout_revision;
    float ellipsis_scroll_x;
    struct clamp_trace *clamp_trace;
    /* intrinsic widths cache */
    float min_cw, max_cw;
    bool intrinsic_done;
    uint64_t intrinsic_font_generation;
    arena_t layout_mem, text_mem;
    bool layout_dirty, layout_cache_valid, layout_cache_safe,layout_cache_exports_floats;
    uint64_t profile_cycles; uint32_t profile_calls; /* debug_js only: inclusive layout cost of the last layout */
    bool layout_prepare_valid, layout_post_valid, layout_post_safe;
    float layout_post_w, layout_post_h, layout_post_content_dy, layout_bottom;
    bool stretch_height_dependent;
    float cached_width, cached_cbh, cached_usedh;
    float cached_height, cached_baseline, cached_last_baseline, cached_content_dy;
    float cached_end_margin; /* what its last children's margins collapse into its bottom margin */
    uint64_t cached_font_generation;
    float cached_padding[4], cached_border[4];
    /* tables */
    int colspan, rowspan, col, row;
    /* list items: an outside marker's text (in text_mem), or a shape (1 disc, 2 circle, 3
       square); its image is the list-style-image of st, painted instead once loaded */
    const char *marker;
    uint8_t marker_shape;
    /* counters (counter.c): a box whose element or pseudo-element counts (BOX_COUNTER_*), the
       text box its content or inside marker writes, and whether it or a box below it counts */
    uint8_t counter_kind;
    bool counter_tree;
    struct box *counter_text;
    /* inline <svg>: its entry in the document's cache */
    struct svg_cache *svg;
    /* an anchor-positioned box's resolved position (layout.c, css-anchor-position-1): the styles it
       is laid out with, the chosen position option, the scroll it compensates for. Box lifetime. */
    struct box_anchor *anchored;
} box_t;
/* layout.c: an anchor-positioned box's used styles (its own st with its anchor functions resolved and
   its chosen position option applied, into style and its anchor group into anchor; false: it is not
   anchor positioned), its used inset in px (side 0-3; false: auto, or not anchor positioned),
   whether position-visibility hides it, and its default scroll shift on an axis (§3.3) */
bool box_anchor_style(const box_t *b, struct style *style, struct css_anchor *anchor);
bool box_anchor_inset(const box_t *b, unsigned side, float *out);
bool box_anchor_hidden(const box_t *b);
float box_anchor_shift(const box_t *b, unsigned axis);
void box_anchor_free(box_t *b);

static inline bool box_line_clamp(const box_t *b) {
    return b && !b->anon && b->st && b->st->legacy_box &&
        b->st->box_orient == BO_VERTICAL && b->st->line_clamp &&
        (b->kind == B_BLOCK || (b->kind == B_ATOMIC && b->atomic == AT_INLINE_BLOCK));
}

/* Actual inline line boundaries, retained for clamp/overflow text. A
 * valign-shifted run is not a new line; counting distinct run baselines loses
 * that distinction and incorrectly consumes the paragraph's line budget. */
struct line_fragment {
    int first_run, end_run, first_deco, end_deco;
    float top, bottom, baseline, left, right;
    /* Cached presentation only. Logical runs and their DOM geometry remain
       intact, including the text covered by the ellipsis for pointer events. */
    bool ellipsis_active;
    int ellipsis_run, ellipsis_bytes;
    float ellipsis_x, ellipsis_width, ellipsis_run_width;
};

struct svg_cache {
    node_t *node;
    char *src; /* serialized, with currentColor replaced */
    size_t n;
    image_t *img;
    int w, h; /* size it was drawn at */
    bool used; /* reachable from the box tree currently being rebuilt */
};
struct svg_cache *doc_svg(web_doc *d, node_t *svg, uint32_t color);
/* The shapes of the <clipPath> that ref names in n's document, as nanosvg markup filled opaque by
   their clip-rule: clipPathUnits="objectBoundingBox" maps the unit square onto box (x y w h),
   userSpaceOnUse puts the origin at its corner. False when ref names no <clipPath>. */
bool doc_svg_clip_markup(node_t *n, const char *ref, const float box[4], sbuf *out);
/* A url() that names an element of d itself (CSS Values 4 §4.5.1: a fragment-only URL, or d's own
   URL with a fragment): no resource to fetch. doc_svg_reference: the element it names in n's tree
   (its document or shadow root), NULL for another document or no such element. */
bool doc_url_local(const web_doc *d, const char *ref);
node_t *doc_svg_reference(node_t *n, const char *ref);
bool box_block_level(const box_t *b);

struct run {
    float x, y, w; /* y is the baseline */
    const char *s;
    int n;
    style_t *st;
    node_t *link;  /* the <a href> it belongs to */
    box_t *atomic; /* or an atomic inline box at (x, y = its top) */
    bool highlight;
    bool hyphen;   /* the line breaks at the soft hyphen ending s: w includes layout_hyphen_string's advance */
    node_t *node; /* original text/atomic node, for generic DOM event targeting */
    box_t *paint_owner; /* filtered inline group; private layout-arena dependency */
    int paint_next;
    float descent; /* measured with the line; overflow does not query fonts again */
    int highlight_first, highlight_end; /* find in page: the bytes of s the match covers */
};

struct deco {
    float x, y, w, h; /* border box */
    style_t *st;
    bool first, last; /* the inline box starts / ends on this line */
    node_t *node;
    box_t *paint_owner;
    int paint_next;
    struct shadow_cache *shadow_cache; /* fragmented inline decoration lifetime */
    struct radius_cache *radius_clip[2]; /* border/padding geometry of this fragment */
};

node_t *doc_element_at(web_doc *d,int x,int y); /* hit retargeted to this Document's frame boundary */
/* Every hit-test target at (x, y) in this Document, topmost first, each element once and
   each of its ::before/::after boxes once as the element (CSSOM View's elementsFromPoint
   before shadow retargeting). Child frames are not entered:
   their element is the target. *elements is NULL when nothing is hit; the caller frees it.
   false only when memory ran out. */
bool doc_elements_at(web_doc *d,int x,int y,node_t ***elements,size_t *count);
/* CSSOM View caretPositionFromPoint before shadow retargeting: the text insertion point at
   document point (x, y) through the same hit test as elementFromPoint. A text entry widget
   (<input>, <textarea>) gives itself and the UTF-16 offset in its value; a hit with no text
   under the point gives the nearest point in the hit box's subtree. false when nothing. */
bool doc_caret_at(web_doc *d,double x,double y,node_t **node,uint32_t *offset);
/* CaretPosition.getClientRect for a text entry widget: the caret at UTF-16 offset of its
   value, in document coordinates through the transforms that apply. false when not one. */
bool doc_control_caret_rect(node_t *control,uint32_t offset,double rect[4]);
void doc_hover_update(web_doc *document,node_t *target); /* native-only CSS state; no JS dispatch */
/* n is editable: d is in design mode, or n is in an element whose contenteditable state is true or
   plaintext-only below no false state (HTML "editing host", "editable") */
bool doc_node_editable(web_doc *d, node_t *n);
/* css-ui-4 §5.1: d's image (its index) for a cursor image, the image-set() option shown (css-images-4
   §2.3.1) requested as CSS images are, and that option's resolution (dppx); -1 when it has none */
int doc_cursor_image(web_doc *d, const struct css_cursor_image *image, float *dppx);
void doc_active_cancel(web_doc *document); /* retire/free only this context subtree */
node_t *doc_active_target(web_doc *document); /* validates live native press and embedding generation */

void boxes_build(web_doc *d, arena_t *a);
void boxes_restyle(web_doc *d);
void boxes_note_child_list(web_doc *d,node_t *parent,node_t *subtree);
void boxes_note_regen(web_doc *d,node_t *n); /* n's own box changes without a style change */
void boxes_note_foreign(web_doc *d,node_t *n); /* a change inside foreign (SVG/MathML) content */
void boxes_require_full(web_doc *d,const char *reason); /* records non-structural topology uncertainty, without forcing geometry */
bool boxes_rebuild_local(web_doc *d);
void boxes_changes_clear(web_doc *d);
bool box_pseudo_generated(node_t *node, const style_t *origin, const style_t *pseudo);
bool boxes_update_text(web_doc *d, node_t *n);
/* A box's counter_kind: its element (a list item, or one with counter-* values: the element's
   principal box) or its ::before or ::after counts; BOX_COUNTER_MARKER: and the list item's
   ::marker generates a box; BOX_COUNTER_MARKER_STYLE: an inside ::marker's inline box, its style
   the box arena's css_style_marker() of the item (restyle_boxes derives it again). */
enum { BOX_COUNTER_ELEMENT = 1, BOX_COUNTER_PSEUDO = 2, BOX_COUNTER_KIND = 3, BOX_COUNTER_MARKER = 4,
       BOX_COUNTER_MARKER_STYLE = 8 };
/* counter.c's writes into the box tree: the text a counting box's content or inside marker
   makes (white space processed as its style says; t is its counter_text), and an outside
   marker's text and shape. Each changes the box and invalidates its layout only when the text
   differs. */
void box_counter_text(box_t *t, const char *s, size_t n);
void box_counter_marker(web_doc *d, box_t *item, const char *s, size_t n, uint8_t shape);
/* counter.c: count the document's counters again, after a publication made or retired a
   counting box (d->counters_dirty), and write the texts that changed */
void web_counters_update(web_doc *d);
/* an <image> of a style (a marker image, a mask layer): its index in d's images, registering it
   with the CORS mode cors (WEB_IMAGE_*: masks are fetched "Anonymous", css-masking-1 §7.1) when
   first used (doc.c); -1 when it is not a url() or cannot be fetched */
int doc_css_image(web_doc *d, const struct css_image *image, int cors);
/* A Text node's text box: for each byte of its processed text (white space collapsed,
   transformed), the byte of the node's data it came from, and the data's length at [t->len].
   Allocated in a. NULL when t is not a Text node's box, or the data has changed since. */
uint32_t *box_text_source_map(arena_t *a, const box_t *t);
void boxes_discard(web_doc *d); /* unpublish node/top-layer borrowers, then bmem */
/* Widths are float sums of advances, and the same content summed in another order (a line,
   an intrinsic width) may differ in the last bits: content within this much of a width fits it. */
#define LAYOUT_FIT_SLOP 0.01f
void layout_doc(web_doc *d, int width, int height);
void layout_cache_free(web_doc *d);
void layout_clamp_trace_report(web_doc *d);
void layout_invalidate(box_t *b);
void layout_invalidate_fonts(box_t *b);
/* ---- Text geometry (layout.c): where DOM text lies in the line layout, and back. The one
   owner for Range and CaretPosition rectangles, caretPositionFromPoint, the native hit test,
   find in page and inline elements' fragments (CSSOM View §5, §6, §9). Coordinates are
   document coordinates: visual as painted and hit tested (element scrolling, sticky offsets,
   marquee), or else unscrolled layout offsets (offsetLeft/offsetTop). */
/* The origin of block b's runs and decos. */
void layout_runs_origin(const box_t *b, bool visual, float *x, float *y);
/* A text run's fragment box from its font: top (ascent above its baseline) and bottom
   (descent below), in its block's coordinates. */
void layout_run_extent(const struct run *r, float *top, float *bottom);
/* The advance from the start of r's text to its byte boundary at (r->w at its end). */
float layout_run_x(const struct run *r, size_t at);
/* What a line broken at a soft hyphen of st's text shows (css-text-4 §6.3.2): hyphenate-character's
   string, or for auto U+2010 where st's font has it and else U+002D. Layout measures and paint draws it. */
const char *layout_hyphen_string(const style_t *st);
/* The advance of s[at, next), one grapheme cluster, x after the text's start. */
typedef float (*layout_measure_fn)(const void *opaque, const char *s, size_t at, size_t next, float x);
/* The grapheme cluster boundary of s[0,n) nearest the advance dx from its start; a boundary
   at a cluster's middle is the earlier one. width >= 0: the whole text's placed width. */
size_t layout_text_boundary(const char *s, size_t n, float width, float dx, layout_measure_fn measure, const void *opaque);
/* The same for run r's text, measured as the line measured it. */
size_t layout_run_boundary(const struct run *r, float dx);
/* The formatting root whose runs and decos hold text or inline box b's fragments. */
box_t *layout_inline_root(const box_t *b);
/* A rectangle, and space: the box whose and whose ancestors' transforms apply to it. */
typedef bool (*layout_rect_fn)(void *opaque, const box_t *space, double x, double y, double w, double h);
/* Text node n's rendered text between UTF-16 offsets start and end (UINT32_MAX: its end),
   one rectangle per line fragment in content order: the part selected, widened to whole
   grapheme clusters, its height the font's ascent and descent; start == end, or a part that
   is selected but not rendered, gives a zero-width rectangle at that position. Nothing for a
   node without rendered text. false when visit returns false or a checkpoint fails. */
bool layout_text_rects(node_t *n, uint32_t start, uint32_t end, bool visual, layout_rect_fn visit, void *opaque);
/* Inline element n's own border-box fragments, not its ::before/::after boxes', in line order. */
bool layout_inline_rects(node_t *n, bool visual, layout_rect_fn visit, void *opaque);
/* The DOM position of the boundary nearest dx in run of block: a Text node and UTF-16
   offset, or for generated content its element, before (::before, ::marker) or after
   (::after) its children. false for an atomic run or a stale layout. */
bool layout_run_position(const box_t *block, int run, float dx, node_t **node, uint32_t *offset);
/* The text insertion point in b's subtree nearest visual document point (x, y): the nearest
   line fragment's nearest boundary, before or after a replaced element. */
bool layout_position_at(const box_t *b, float x, float y, node_t **node, uint32_t *offset);
float box_abs_x(const box_t *b);
float box_abs_y(const box_t *b);
/* Paint/hit/DOMRect positions; box_abs remains unscrolled offset geometry. */
float box_visual_x(const box_t *b);
float box_visual_y(const box_t *b);
/* Within one paint scroll/translation inputs are fixed: memoize visual
   coordinates until box_visual_memo_end restores the previous generation. */
uint32_t box_visual_memo_begin(void);
void box_visual_memo_end(uint32_t previous);
bool box_filter_containing_block(const box_t *b); /* transforms, 3D, filters, containment and will-change establish descendant containing blocks */
/* the containment b has in use: CSS_CONTAIN_* of contain, content-visibility and container-type, for the boxes they apply to */
unsigned box_containment(const box_t *b);
bool css_translation_active(const style_t *s);
enum { CSS_TRANSLATE_INDIVIDUAL=1,CSS_TRANSLATE_TRANSFORM=2 };
/* Pure translations stay in visual coordinates. Any other used transform is
   painted, hit tested and measured through box_transform_matrix instead and
   contributes no translation to box_translation_axis. */
float box_translation_axis(const box_t *b,unsigned axis,unsigned sources);
bool css_transform_general(const style_t *s);
bool box_transformed(const box_t *b);
/* Affine x'=m[0]x+m[2]y+m[4], y'=m[1]x+m[3]y+m[5] in document coordinates. */
bool box_transform_matrix(const box_t *b,double m[6]);
bool box_backface_hidden(const box_t *b); /* backface-visibility: hidden and its back toward the viewer */
/* The used affine matrix of s's transform properties for a reference box of
   w x h whose top-left corner is at (x,y); false if none or not finite. */
bool css_style_transform_matrix(const style_t *s,double x,double y,float w,float h,double m[6]);
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]);
/* The same list as a full 4x4 with perspective and no box (Geometry Interfaces); row-major. */
void css_transform_list_matrix_3d(const struct css_transform *t,double m[16]);
/* Geometry's "parse a string into an abstract matrix": column-major m, *two_d false when a
   3D transform function is in the list. False when the string is no valid list. */
bool css_transform_string_matrix(const char *s,size_t n,double m[16],bool *two_d);
/* The complete transform of b's space: b's and its ancestors' used transforms (b's applied
   first) composed into one affine matrix of document coordinates, layout x'=m[0]x+m[2]y+m[4]. */
void box_transform_chain(const box_t *b,double m[6]);
/* Maps a document rectangle (x,y,w,h) through b's and its ancestors' used
   transforms and returns the bounding box of the resulting quad. */
void box_transform_rect(const box_t *b,double rect[4]);
bool box_viewport_fixed(const box_t *b); /* actual layout CB, not merely position:fixed */
void box_abs_cb_rect(const box_t *cb, const web_doc *d, float rect[4]); /* x/y/w/h of padding CB in its content coordinates */
void web_paint_debug_node_free(node_t *node);
void web_hit_index_free(web_doc *d);
void web_paint_free(web_doc *d); /* paint-owned buffers kept between paints */
void doc_paint_damage(web_doc *d, node_t *node);
void doc_paint_full(web_doc *d); /* unclassified visual change, including frame composites */
/* A cascade's paint damage. _begin records what the restyled elements paint while their boxes
   still borrow the retired styles; _commit adds what they paint now once the geometry was kept;
   _lost makes the next paint whole (new boxes or layout, or no box bounds the change). */
void doc_restyle_damage_begin(web_doc *d);
void doc_restyle_damage_commit(web_doc *d);
void doc_restyle_damage_lost(web_doc *d);
bool box_element_scrollable(const box_t *b);
void box_scroll_clamp(box_t *b);

/* Noto Sans / Noto Serif / Maple Mono, with family-aware missing-glyph fallback. */
typedef struct wfont {
    font_t *ttf; /* NULL: monospace bitmap */
    float px;
    bool bold;
    const struct web_font_choice *choices;
} wfont;
wfont style_font(const style_t *st);
/* 論理text列の状態。run／style境界では保持し、text分断だけでゼロ初期化する。 */
typedef struct wf_grapheme_state {
    uint8_t previous;
    bool have_previous, ri_odd, pictograph_extend, pictograph_zwj, indic_linker;
} wf_grapheme_state;
bool wf_grapheme_break(wf_grapheme_state *state, uint32_t cp);
size_t wf_grapheme_decode(const char *s, size_t n, uint32_t *cp);
/* at は既知の書記素境界。s[0..n)以外を読まず、prefixの再走査をしない。 */
size_t wf_grapheme_next(const char *s, size_t n, size_t at);
float wf_width(const wfont *f, const char *s, size_t n);
void wf_metrics(const wfont *f, float *ascent, float *descent);
float wf_draw(canvas_t *c, const wfont *f, float x, int baseline, const char *s, size_t n, uint32_t color);
float style_line_height(const style_t *st); /* used line-height in px */
float style_text_width(const style_t *st, const char *s, size_t n);
/* Native textarea value layout, shared by scroll-area measurement and paint.
   Byte spans never split a grapheme; next skips only a hard line break. */
struct control_text_line {
    size_t first, length, next;
    float width, top, height;
    bool hard_break;
};
typedef void (*control_text_line_fn)(const struct control_text_line *, void *);
float control_text_width(const style_t *st, const char *s, size_t n);
/* The grapheme boundary of one control line's text s[0,n) nearest the advance x from its
   start, measured as control_text_width measures (tab stops included). */
size_t control_text_boundary(const style_t *st, const char *s, size_t n, float x);
bool control_text_lines(const box_t *b, const style_t *st, const char *text,
                        control_text_line_fn visit, void *opaque, float *width, float *height);
const char *web_button_label(node_t *n);   /* the text on an <input> button */
uint32_t doc_canvas_bg(web_doc *d, int *from); /* 1: from <html>, 2: from <body>, 0: white */

/* ---------------------------------------------------------------- the document */
struct web_image {
    char *url;
    image_t *img;
    /* The last sizes drawn, the most recent first: a page may draw one image at two sizes
       (a blurred backdrop behind the picture itself) on every paint. */
    image_t *scaled[2];
    char *svg;       /* SVG source, rasterized again at the size it is drawn at */
    size_t svg_n;
    bool done, failed;
    uint64_t loading_id; /* The owning transport request, not a global resource-count gate. */
    bool image_upgrade; /* Part of request identity: imageset must not share an upgraded image. */
    uint8_t cors;       /* Part of request identity: WEB_IMAGE_NO_CORS or a CORS credentials mode. */
    uint8_t referrer_policy; /* Part of request identity: the element's referrerpolicy, 0 the document's. */
    bool origin_clean;  /* The response is CORS-same-origin: a canvas or WebGL may read its pixels. */
};
/* An image request's CORS settings (the crossorigin attribute's state). */
enum { WEB_IMAGE_NO_CORS, WEB_IMAGE_ANONYMOUS, WEB_IMAGE_USE_CREDENTIALS };
/* An element's referrerpolicy attribute (enum webnet_referrer_policy); 0 when
   absent or invalid, which leaves the document's policy. */
int doc_element_referrer_policy(const node_t *n);

/* Monotonic diagnostic counters, sampled at task boundaries. No timing limit
   or DOM semantics depend on these values. Times can overlap (inclusive). */
struct web_profile {
    uint64_t inserts, index_visits, index_ms, rescans, rescan_ms;
    uint64_t shadow_reassigns, shadow_visits;
    uint64_t script_scans, script_visits, script_ms, native_calls, native_ms, native_samples;
    uint64_t metadata_syncs, metadata_visits, metadata_ms;
    uint64_t style_visits, style_rule_checks, style_parallel_nodes;
    uint64_t style_index_entries; /* selectors indexed: once per sheet AST, not per instance */
    uint64_t style_rule_filtered; /* candidates rejected by the ancestor filter */
    uint64_t style_rule_indexes; /* rule indexes looked up, one per stylesheet binding */
    uint64_t style_rule_bucket[4]; /* candidates by index bucket: id, class, tag, universal */
    uint64_t style_kept; /* restyled elements whose computed style came out unchanged */
    uint64_t style_adopted; /* restyled elements whose children kept their styles */
    uint64_t style_redo[10]; /* restyled elements by why (css.c REDO_*) */
};
struct web_doc {
    uint64_t paint_bounds_generation;
    bool paint_effect_failure_reported;
    struct web_animations *animations; /* animation.c: timelines, animations, effects, events */
    web_doc *frame_parent;
    uint32_t sandbox_flags; /* immutable active document restrictions */
    uint64_t sandbox_activation_until; /* only native trusted input writes this */
    bool window_closing; /* HTML top-level traversable "is closing": its close() was accepted */
    /* The callbacks web_live was given: a document without a realm (a sandbox without
       allow-scripts) still follows its hyperlinks through them. */
    struct web_host host;
    /* During the dispatch of a trusted pointer click at an image button (doc.c web_dispatch):
       the point the user selected, which that click's activation behavior makes the button's
       selected coordinate (js_activation_native.h). */
    node_t *image_activation;
    int image_activation_x, image_activation_y;
    /* HTML's design mode enabled. js_document_commands.js owns it and writes it
       here for the native readers: :read-write and the editing keys. */
    bool design_mode;
    web_doc *frame_retired_next;
    node_t *frame_element;
    node_t *address_frame; /* native chrome selection; resolves the current child generation */
    struct web_frame *frames;
    const char *inherited_url; /* about:blank/srcdoc base and origin, never an author property */
    int referrer_policy; /* Policy container, not Document.referrer's navigation value. */
    char *referrer; /* Document.referrer: the Referer its navigation sent; NULL for none */
    int embedder_policy; /* Policy container COEP: enum html_embedder_policy. */
    bool cross_origin_isolated; /* Agent cluster isolation; fixed at document creation. */
    web_doc *origin_owner; /* inherited opaque origins retain native identity */
    node_t *window_token; /* stable for navigation, distinct after removal */
    struct web_frame *frame_container;
    web_doc *frame_free_next;
    web_doc *frame_docs; /* web_free: the frames' documents (web_frames_detach), released with this one */
    struct web_profile profile;
    bool profile_enabled; /* debug_js only; diagnostic counters remain cheap */
    uint8_t clamp_trace_reported; /* at most four numeric diagnostics per document */
    uint8_t replaced_trace_reported; /* debug only: at most two degenerate image diagnostics */
    bool replaced_trace_pending; /* successful image update: consume at one coherent layout only */
    web_doc *dom_family, *dom_docs, *dom_next;
    bool js_nodes_dirty; /* structural/Attr ownership changed; not text/style */
    bool inert; /* independent DOM only: no window, loader, style/resource scan */
    bool template_owner;
    bool has_shadow; /* family-wide fast path: stays set after a shadow root exists */
    bool shadow_slots_pending;
    node_t *shadow_slots_first, *shadow_slots_last;
    node_t *shadow_dirty_first, *shadow_dirty_last;
    bool shadow_flushing;
    bool native_cancelled;
    bool native_recovery_pending, native_recovery_active;
    unsigned native_layout_depth; /* native work must unwind before recovery */
    unsigned native_input_depth, native_checkpoint_depth;
    uint64_t input_identity, wheel_window_blocking;
    uint32_t native_checkpoint_count;
    node_t *details_toggle_first, *details_toggle_last;
    node_t *scroll_input_first, *scroll_input_last;
    bool scroll_viewport_pending; /* native child viewport input, normal tick only */
    web_doc *template_doc;
    arena_t mem;    /* DOM, stylesheets */
    arena_t smem;   /* computed styles: retained across text-only box rebuilds */
    struct css_style_pool *style_pool; /* current/retired fixed computed-style slots */
    arena_t bmem;   /* box/anonymous-style ownership, never the computed styles */
    arena_t lmem;   /* layout results */
    struct layout_text_cache *layout_text_cache; /* owned, generation-keyed measurements */
    arena_t cssmem; /* authored sheets: replaced on a DOM stylesheet mutation */
    node_t *owned_nodes;
    node_t *owned_images; /* the <img>/<input> elements of owned_nodes, linked by owned_image_next */
    node_t *parser_form_nodes;
    size_t control_bytes;
    size_t canvas_bytes;
    uint8_t dom_create_reported; /* one numeric failure diagnostic per stage */
    bool frame_allocation_reported;
    uint64_t dom_revision;
    /* HTML "Document named properties" (dom.c doc_named_*): moves when an element that can
       give a supported property name enters or leaves the document tree or its id or name
       changes; named keeps the names read at one revision. */
    uint64_t named_revision;
    struct doc_named *named;
    uint64_t metadata_revision;
    bool metadata_valid;
    uint64_t layout_revision;
    struct web_hit_index *hit_index; /* paint-order spatial index, no DOM ownership */
    uint32_t *paint_rollback; /* an alpha-group paint's copy of the committed pixels, kept for the next paint */
    size_t paint_rollback_pixels;
    uint64_t paint_order_layout;
    uint64_t paint_order_visual; /* fractional scroll/translation invalidates shadow mask alignment */
    bool paint_shadow_crop;
    int paint_shadow_view_x, paint_shadow_view_y, paint_shadow_view_w, paint_shadow_view_h;
    bool paint_order_valid;
    uint64_t visual_revision; /* element scrolling/marquee geometry, not video pixels */
    uint64_t style_revision; /* published box styles: opacity/filter/z-index decide stacking contexts */
    node_t *box_change_node; /* debug_js only: last element whose restyle required new boxes */
    bool svg_snapshots_dirty; /* a restyled SVG element has no boxed outermost <svg>; <use> may reference it */
    bool counters_dirty; /* a publication made or retired a counting box (counter.c counts again) */
    float paint_damage[4];
    bool paint_damage_valid, paint_damage_full;
    /* Between a cascade and its publication: the boxes whose styles changed and what they
       painted before. More than these is repainted whole. */
    struct restyle_damage { box_t *box; float rect[4]; } restyle_damage[32];
    uint8_t restyle_damage_n;
    bool restyle_damage_lost;
    /* paint.c: the border boxes whose backdrop-filter read the canvas, in document space, since the
       last whole paint at this layout revision. Damage that reaches one repaints all of it (its
       backdrop is what is painted under it there); more than these repaints whole. */
    struct backdrop_region { const box_t *box; float rect[4]; } backdrop_regions[32];
    uint8_t backdrop_count;
    bool backdrop_overflow;
    uint64_t backdrop_layout;
    struct html_parser *parser;
    struct xml_parser *xml_parser; /* an XML document's, while it loads (xml.h) */
    struct web_js_state *js;
    bool live, dirty, paint_dirty, resources_dirty, images_dirty;
    bool restyle_dirty; /* dirty, but only marked elements' own computed values (CSS animation samples) */
    bool initial_render_done;
    /* Paint Timing: when this document's first paint, and its first
       contentful paint, ended (uptime_ms; 0 before). paint.c records each
       once from a real paint walk; js.c reports them to the timeline. */
    uint64_t first_paint_ms, first_contentful_paint_ms;
    uint64_t initial_render_deadline; /* paint fallback only: never cancels resources */
    /* HTML Page visibility (interaction.html 6.2). visible is this Document's visibility state
       (false is "hidden", a Document's initial value). A hidden document has no rendering
       opportunity: no animation frames, rendering observers, CSS animation samples or marquee
       steps, and its timers and idle periods are padded (js.c). On a top-level document,
       system_visible is its traversable's system visibility state as the host's window_state
       last gave it (web_visibility_event). visibility_updates counts the queued "update the
       visibility state" tasks; their states alternate, the newest being visibility_update_last. */
    bool visible, system_visible, visibility_update_last;
    unsigned visibility_updates;
    unsigned (*window_state)(void *opaque); /* the host's (web_host.window_state), or NULL */
    void *window_opaque;
    double render_head_order; /* root-sheet order after the scanned head */
    pvec css_cache;
    struct css_sheet_reuse *css_reuse; /* immutable live inline ASTs, no DOM ownership */
    struct css_sheet_reuse **css_reuse_buckets;
    size_t css_reuse_capacity;
    size_t css_reuse_bytes;
    unsigned css_reuse_count;
    size_t css_bytes;
    bool scan_title_seen;
    node_t *root;   /* the document node */
    node_t *html, *head, *body;
    pvec marquees; /* connected candidates collected by the existing resource scan */
    char *url;      /* the document's own URL */
    char *resolved_link; /* native hit/getter URL, borrowed until this doc's next link getter */
    char *base;     /* the document base URL's serialization (doc_set_base), never NULL */
    bool base_seen;
    bool quirks; /* no (or a legacy) doctype */
    /* DOM document type and content type. An XML document is not an HTML
       document: names keep their case and tagName is not uppercased.
       content_type is NULL for text/html; xml_interface: implements XMLDocument
       (DOMImplementation.createDocument). */
    bool xml, xml_interface;
    char *content_type;
    uint8_t document_mode; /* 0 no-quirks, 1 quirks, 2 limited-quirks (Lexbor values) */
    char encoding[32]; /* canonical transport encoding; DOMString documents use UTF-8 */
    bool encoding_certain; /* BOM/transport/accepted meta/author entry; strings never reparse */
    bool html_srcdoc;
    /* HTML "load a text document" made it (content_type is the text type), and its text is the
       content: the user agent's text style (css.c) applies until document.open() replaces it. */
    bool text_document;
    int32_t custom_registry_id;
    void *custom_registry_realm;
    struct web_html_policy *html_policy;
    char *title;
    char *refresh_url;
    int refresh_delay;
    struct styling sty;
    pvec pending_css; /* char* URLs still to fetch */
    double next_sheet_order;
    pvec images;      /* struct web_image* */
    struct web_font_resource *fonts; /* stable URL identities across CSS rescans */
    /* CSS Font Loading: decoded BufferSource faces (live until the document
       goes, as styles may still borrow them), FontFace records in
       document.fonts in insertion order, and a count of changes to the font
       loads and font source that js_font_loading.js has not yet seen. */
    struct web_font_resource *font_data;
    pvec author_fonts; /* struct css_font_face* */
    uint64_t font_generation;
    pvec svgs;        /* struct svg_cache* */
    box_t *root_box;
    pvec abs_boxes;   /* absolutely positioned boxes, painted last */
    struct box_anchor *anchored_boxes; /* layout.c: the anchor-positioned boxes (box_t's anchored), linked */
    uint32_t anchor_epoch; /* box.c: counts unpublished element boxes, which those boxes' records name */
    pvec box_changes; /* parent/subtree pairs, or node/NULL to regenerate a node, until box publication */
    bool box_changes_full;
    const char *box_full_reason; /* debug_js only: why the next publication rebuilds every box */
    uint32_t box_epoch; /* the current local publication: boxes it generated are already current */
    int width, height, doc_h, doc_w;
    int styled_w, styled_h; /* viewport the cascade ran for */
    bool need_style, need_boxes, layout_valid;
    bool style_full_dirty, style_pending_dirty;
    bool font_selection_dirty; /* coalesced failed-font completion; no metrics changed yet */
    bool style_boxes_changed;
    bool style_existing_boxes_changed; /* debug_js only: some restyled element marked box_regen */
    float container_rem; /* immutable root font size of the layout snapshot */
    node_t *style_retired_first;
    node_t *focus;
    /* Selectors 4 :focus-visible: whether focus shows its indicator, from the
       latest user input (enum web_modality) and a focus() call's FocusOptions
       focusVisible (0 absent, 1 false, 2 true) */
    bool focus_visible;
    uint8_t focus_modality, focus_visible_request;
    node_t *hover_target; /* native pointer target, not an author-set property */
    web_doc *hover_leaf;  /* top document's last hovered child context; arenas remain owned */
    node_t *active_target; /* native primary-button press, independent of hover/events */
    web_doc *active_leaf; /* top document's pressed context; native arenas remain owned */
    struct web_dialog_state *dialogs; /* native modal/top-layer ownership */
    node_t *fullscreen_element,*pip_element;
    web_doc *fullscreen_owner,*pip_owner; /* top document's native presentation lease */
    char *presentation_policy; /* Permissions-Policy response snapshot only */
    uint8_t presentation_inherited;bool presentation_policy_ready;
    struct web_presentation_event *presentation_events,*presentation_events_tail;
    uint64_t presentation_revision,pip_session;
    int pip_width,pip_height;
    bool (*presentation_host)(void *,web_doc *,web_node *,int);
    void *presentation_opaque;
    int view_x, view_y; /* last native viewport scroll; fixed top-layer coordinates */
    node_t *validation_target;
    bool validation_report_pending;
    const char *validation_message;
    size_t validation_message_length;
    void *form_validation_state; /* bounded native regular-expression cache */
    int caret;
    node_t *find_node;
    struct run *find_run;
    char *find_text; /* document-owned full search query */
    bool find_revealing; /* author beforematch may flush layout, not restart find */
};

node_t *html_parse(web_doc *d, const char *html, size_t n, const char *charset);
node_t *html_parse_string(web_doc *d, const char *html, size_t n, bool allow_shadow);
web_doc *doc_inert(web_doc *family, const char *html, size_t n, const char *url);
web_doc *doc_inert_ex(web_doc *family, const char *html, size_t n, const char *url, bool allow_shadow);
/* A document without a browsing context in family's DOM family, with no root
   yet; doc_inert_discard frees it and every document made after it (its
   template documents) before a wrapper escapes. */
web_doc *doc_inert_new(web_doc *family, const char *url);
web_doc *doc_inert_bytes(web_doc *family, const char *bytes, size_t n, const char *label,
                         const char *url, const char *content_type); /* XHR's HTML document */
void doc_inert_discard(web_doc *d);
size_t doc_dom_remaining(web_doc *d);
void doc_dom_budget(web_doc *d);
bool doc_node_adopt(web_doc *d, node_t *node);
node_t *doc_template_content(web_doc *d, node_t *node);
bool doc_templates_finish(web_doc *d, node_t *root);
const char *doc_link_href(web_doc *d, node_t *a); /* node-owner-relative absolute URL; valid until the next call */
/* HTML's encoding-parse a URL relative to d: input against d's base URL, with d's
   character encoding for the query (web_url_resolve's 1, 0 or -1; *out malloc'd). */
int doc_encoding_parse_url(web_doc *d, const char *input, char **out);
/* d's base URL becomes a copy of url (unchanged when it is the same); false when out of
   memory, the base as it was. */
bool doc_set_base(web_doc *d, const char *url);
void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base);
void doc_viewport_update(web_doc *d,int width,int height); /* size publication; no author callbacks */
/* wanted: a cascade or text run selected it (reset by each cascade);
   demanded: a FontFace load (CSS Font Loading) asked for it, until done. */
struct web_font_resource {
    struct web_font_resource *next;
    char *url;
    font_t *face;
    bool wanted, demanded, loading, done, failed;
};
/* Requests that have not yet been sent: the document still waits on them. */
bool doc_fonts_unrequested(web_doc *d);
/* A stylesheet the document requested has not yet arrived. */
bool doc_css_loading(web_doc *d);
struct web_font_range { uint32_t first,last; };
struct web_font_choice {
    struct web_font_choice *next;
    struct web_font_resource *resource;
    font_t *face; /* selection snapshot for loaded-font invalidation */
    const struct web_font_range *ranges;
    size_t range_count; /* zero: U+0-10FFFF */
};
struct web_font_resource *doc_font_resource(web_doc *d, const char *url);
void doc_font_loaded(web_doc *d, struct web_font_resource *font, const void *bytes, size_t length);

enum html_parse_result {
    HTML_PARSE_CANCELLED = -2, HTML_PARSE_FAILED = -1,
    HTML_PARSE_DONE = 0, HTML_PARSE_SCRIPT = 1,
    HTML_PARSE_INPUT_WAIT = 2, HTML_PARSE_YIELD = 3
};
enum html_scripting_mode {
    HTML_SCRIPTING_NORMAL, HTML_SCRIPTING_DISABLED,
    HTML_SCRIPTING_INERT, HTML_SCRIPTING_FRAGMENT
};
/* YIELD publishes a coherent native tree without consuming following input.
   Synchronous APIs pump YIELD immediately, without running general author
   tasks. CANCELLED never represents successful EOF or DOM readiness.
   web_parse remains a scripting-disabled, fully parsed document. */
struct html_parser *html_begin(web_doc *d, const char *html, size_t n, const char *charset, bool scripting);
struct html_parser *html_begin_string(web_doc *d, const char *html, size_t n, bool scripting, bool allow_shadow);
struct html_parser *html_begin_mode(web_doc *d, const char *html, size_t n, const char *charset, enum html_scripting_mode mode);
struct html_parser *html_begin_string_mode(web_doc *d, const char *html, size_t n, enum html_scripting_mode mode, bool allow_shadow);
/* HTML "load a text document": the parser of d (text_document set) after its pre start tag, in
   the PLAINTEXT state for the bytes; json: the type is a JSON MIME type (their decoding). */
struct html_parser *html_begin_text(web_doc *d, const char *bytes, size_t n, const char *charset, bool json, bool scripting);
struct html_parser *html_open(web_doc *d);
void html_close(struct html_parser *parser);
int html_resume(struct html_parser *p, node_t **script);
void *html_write_boundary(struct html_parser *p);
int html_resume_written(struct html_parser *p, node_t **script, void *boundary);
bool html_pending_input(const struct html_parser *p); /* unconsumed bytes, not stream EOF */
bool html_import_changed(const struct html_parser *p); /* actual change in latest resume/import */
void html_declarative_shadow_set(struct html_parser *parser,bool enabled);
bool html_write(struct html_parser *p, const char *text, size_t n);
void html_finish(struct html_parser *p);
node_t *html_fragment(web_doc *d, node_t *context, const char *html, size_t n);
node_t *html_contextual_fragment(web_doc *d, node_t *context, const char *html, size_t n);
node_t *html_fragment_ex(web_doc *d, node_t *context, const char *html, size_t n, bool allow_shadow, bool run_scripts);
node_t *html_fragment_mode(web_doc *d, node_t *target, node_t *context, const char *html, size_t n, bool allow_shadow, enum html_scripting_mode mode);
/* CSS overflow propagated from html/body belongs to the viewport, not a
   scrolled element-sized clipping rectangle. Computed styles stay intact. */
bool doc_viewport_overflow_box(const web_doc *d, const box_t *b);
bool doc_element_scroll_metrics(web_doc *d, node_t *n, float *width, float *height);
/* True only when the regular element's native position actually changed.
   Root/quirks-body viewport forwarding belongs to the Window host bridge. */
bool doc_element_scroll(web_doc *d, node_t *n, double x, double y);
/* CSSOM View "scroll an element": the largest scroll position of n's own scrolling box
   on axis (0 horizontal, 1 vertical); 0 when n has none. */
double doc_element_scroll_max(web_doc *d, node_t *n, int axis);
enum doc_scroll_alignment { DOC_SCROLL_START, DOC_SCROLL_END, DOC_SCROLL_CENTER, DOC_SCROLL_NEAREST };
/* One scrolling box's position in "scroll a target into view". */
struct doc_scroll_target { node_t *node; double x, y; };
/* CSSOM View "scroll a target into view", horizontal-LTR only: the position each scrolling
   box must take, innermost first, each as if the boxes inside it already had theirs. Nothing
   is left scrolled: the caller performs each scroll (instant or smooth). The current viewport
   position enters through x/y and the viewport's leaves through them. *targets (heap-owned,
   *count entries) must start NULL. 1 rendered, 0 inactive/no box, -1 allocation,
   -2 containing-block cycle, -3 too many boxes for the index. */
int doc_element_scroll_into_view(web_doc *d, node_t *n, int block, int inline_, bool nearest_only,
                                double *x, double *y, struct doc_scroll_target **targets, int *count);
enum doc_create_failure { DOC_CREATE_INVALID, DOC_CREATE_QUOTA, DOC_CREATE_ALLOCATOR,
    DOC_CREATE_OVERFLOW, DOC_CREATE_TEMPLATE, DOC_CREATE_WRAPPER };
void doc_create_diagnostic(web_doc *d, unsigned stage, int type, size_t name_bytes, size_t data_bytes);
node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t n); /* N_ELEM: HTML namespace */
/* DOM "create an element" with an already validated and extracted namespace
   (NULL: none), prefix (NULL: none) and local name. */
node_t *doc_element_create(web_doc *d, const char *ns, const char *prefix, const char *local);
bool doc_element_prefix_set(node_t *n, const char *prefix); /* NULL: no prefix */
int doc_namespace_id(const char *ns);
const char *doc_element_namespace(const node_t *n); /* NULL: no namespace */
/* DOM "locate a namespace" (prefix NULL: the default namespace) and
   "locate a namespace prefix"; strings borrow the tree's records. */
const char *doc_locate_namespace(const node_t *n, const char *prefix);
const char *doc_locate_prefix(const node_t *n, const char *ns);
/* getElementsByTagName (namespaced false) and getElementsByTagNameNS. */
void doc_elements_named(node_t *root, bool namespaced, const char *ns, const char *name, pvec *out);
/* DOM: an attribute or tag name given to an HTML-namespace element whose node
   document is an HTML document is ASCII-lowercased; any other keeps its case. */
static inline bool doc_html_names(const node_t *n) {
    return n && n->type == N_ELEM && n->namespace_id == NS_HTML && !(n->owner && n->owner->xml);
}
node_t *doc_doctype_create(web_doc *d, const char *name, size_t name_len,
    const char *public_id, size_t public_len, const char *system_id, size_t system_len);
int doc_node_insert_validity(node_t *parent, node_t *child, node_t *before);
int doc_node_move_validity(node_t *parent, node_t *child, node_t *before); /* DOM: pre-move validity */
int doc_node_replace_validity(node_t *parent, node_t *child, node_t *old);
int doc_node_replace_all_validity(node_t *parent,node_t *child);
node_t *doc_node_root(node_t *node, bool composed);
node_t *doc_shadow_parent(node_t *node); /* parent, or a shadow root's host */
bool doc_node_connected(node_t *node);
bool doc_shadow_host_valid(const node_t *host);
node_t *doc_shadow_attach(web_doc *d, node_t *host, bool closed, bool delegates_focus,
                        bool clonable, bool serializable, bool manual);
node_t *doc_assigned_slot(node_t *node, bool open_only);
void doc_shadow_reassign(web_doc *d);
void doc_shadow_dirty(node_t *root);
void doc_shadow_flush(web_doc *d);
bool web_native_checkpoint(web_doc *d);
bool web_js_native_recover(web_doc *d); /* internal outer-stack/join barrier */
void doc_slot_signal(node_t *slot);
bool doc_slot_nodes(node_t *slot, bool flatten, pvec *out);
bool doc_slot_assign(node_t *slot, node_t **nodes, int count);
/* Appends immediate rendered children. Slots remain nodes (display:contents).
   This never mutates native parent/first/next and never crosses template trees. */
void doc_flat_children(node_t *node, pvec *out);
node_t *doc_flat_parent(node_t *node);
bool doc_pi_target_valid(const char *target, size_t len);
void doc_referrer_meta(web_doc *d,node_t *node);
bool doc_node_attr(web_doc *d, node_t *node, const char *name, const char *value);
int doc_attr_index(node_t *node, const char *namespace_uri, const char *name, bool namespaced);
node_t *doc_attr_node(web_doc *d, node_t *element, int index);
node_t *doc_attr_create(web_doc *d, const char *namespace_uri, const char *prefix, const char *local, const char *value);
bool doc_attr_set_ns(web_doc *d, node_t *element, const char *namespace_uri, const char *prefix, const char *local, const char *value);
bool doc_attr_set_node(web_doc *d, node_t *element, node_t *attribute);
bool doc_attr_remove(web_doc *d, node_t *element, int index);
bool doc_attr_value(web_doc *d, node_t *attribute, const char *value);
void doc_attrs_publish(node_t *element, struct attr *attrs, int count);
void doc_attrs_initial(web_doc *d, node_t *element, struct attr *attrs, int count); /* parser, under its trap */
bool doc_node_move(web_doc *d, node_t *parent, node_t *child, node_t *before); /* remove, then insert */
/* DOM "move" (moveBefore): child keeps its state; no removing or insertion steps */
bool doc_node_move_preserving(web_doc *d, node_t *parent, node_t *child, node_t *before);
enum { HTML_BRIDGE_FIELDS=1, HTML_BRIDGE_LINKS=2, HTML_BRIDGE_CHILDREN=4 };
void html_bridge_native_changed(node_t *node, unsigned flags);
void html_bridge_native_retire(node_t *node); /* before allocation ownership ends */
bool doc_node_replace(web_doc *d, node_t *parent, node_t *child, node_t *old);
bool doc_node_replace_all(web_doc *d,node_t *parent,node_t *child);
void doc_node_remove(web_doc *d, node_t *node);
bool doc_node_text(web_doc *d, node_t *node, const char *text, size_t n);
void doc_parser_form_set(node_t *node,node_t *form);
void doc_parser_element_created(node_t *node); /* once, with the attributes its parser gave it */
bool doc_style_blocks_scripts(web_doc *d); /* HTML "has a style sheet that is blocking scripts" */
/* HTML "Document named properties" of d: whether name is a supported property name (-1 out of
   memory); the supported property names in order (NULL out of memory); the
   next named element with that name after n in tree order (n NULL: the first). */
int doc_named_supported(web_doc *d, const char *name, size_t length);
const char *const *doc_named_names(web_doc *d, uint32_t *count);
node_t *doc_named_next(web_doc *d, node_t *n, const char *name, size_t length);
void doc_named_free(web_doc *d);
bool web_js_script_parser_capture(web_doc *d,node_t *node);
bool web_js_encoding_restart(web_doc *d);
bool doc_node_html(web_doc *d, node_t *node, const char *html, size_t n);
bool doc_node_value(web_doc *d, node_t *node, const char *text, size_t n);
bool doc_control_selection_supported(const node_t *node);
uint32_t doc_utf16_length(const char *text);
uint32_t doc_byte_to_utf16(const char *text, size_t byte);
size_t doc_utf16_to_byte(const char *text, uint32_t offset, bool round_up);
/* The same for CharacterData's counted data, which may hold U+0000: the UTF-16 offset of a
   byte, and the byte of a UTF-16 offset (inside a surrogate pair: before it, or round_up after). */
uint32_t doc_text_utf16(const char *text, size_t length, size_t byte);
size_t doc_text_byte(const char *text, size_t length, uint32_t offset, bool round_up);
void doc_control_caret(web_doc *d, node_t *node);
void doc_control_selection(web_doc *d, node_t *node, uint32_t start, uint32_t end, uint8_t direction);
bool doc_control_replace(web_doc *d, node_t *node, uint32_t start, uint32_t end, const char *text);
void doc_control_init(web_doc *d, node_t *node);
void doc_control_checked(web_doc *d, node_t *node, bool checked);
bool doc_form_reset(web_doc *d, node_t *form);
node_t *doc_select_option(node_t *select, int index);
bool doc_option_selected(node_t *option);
node_t *doc_node_clone(web_doc *d, node_t *node, bool deep);
node_t *doc_node_clone_registry(web_doc *d, node_t *node, bool deep, int32_t fallback_registry, void *fallback_realm);
/* The custom element registry of a Document, element or shadow root (-1 null, 0 global with
   *realm its Window's Document node, positive scoped); -1 for any other node. */
int32_t doc_custom_registry(node_t *node, void **realm);
void doc_mutated(web_doc *d, node_t *node);
void doc_fragment_parsed(web_doc *d, node_t *forest);
void doc_rescan(web_doc *d);
void web_marquee_register(web_doc *d, node_t *node);
void web_marquee_set(node_t *node, bool running);
void web_marquee_tick(web_doc *d, uint64_t now);
int64_t web_marquee_deadline(web_doc *d); /* absolute uptime_ms(), as web_deadline */
bool web_marquee_box(const box_t *box);
void doc_sync_tree(web_doc *d);
node_t *doc_image_node_next(web_doc *d, node_t *previous);
void doc_image_node_owned(node_t *n); /* at element creation, once its tag is known */
void doc_image_sync(web_doc *d, node_t *n);
/* The next stylesheet to request, and the link element that asked for it
   (NULL for an @import), whose nonce the request carries. */
struct cached_css *doc_css_request_next(web_doc *d, size_t *cursor, node_t **element);
/* HTML "update a style block": whether CSP allows a style element's child
   text (text NULL: read it from the element). The check runs and reports
   once for each new text. */
bool doc_style_csp_allowed(web_doc *d, node_t *style, const char *text, size_t length);
/* CSP checks for elements of d, reported through d's realm (without one,
   still decided): a style element's inline text, and Is base allowed. */
bool web_js_csp_style_allowed(web_doc *d, node_t *element, bool attribute, const char *text, size_t length);
bool web_js_csp_base_allowed(web_doc *d, node_t *base, const char *url);
/* HTML's style attribute was set to value (NULL: removed) by a parser or the
   attribute change steps: CSP's inline check decides whether it applies. */
void doc_style_attribute_changed(web_doc *d, node_t *n, const char *value);
/* HTML "nonce attributes". set: [[CryptographicNonce]] := value (length 0 the empty string);
   false out of memory or for an element without the slot. nonce: the slot as script sees
   it ("" when empty). csp_nonce: the slot as CSP's nonce metadata, NULL when it is empty
   or holds U+0000 (no nonce-source matches such a value). attribute_changed: the change
   steps of the null-namespace nonce attribute, value NULL when removed. connected: the
   steps run when element (an inserted subtree's root) becomes browsing-context connected:
   with a header-delivered CSP, a non-empty nonce attribute becomes the empty string and
   the slot keeps the nonce. */
bool doc_element_nonce_set(web_doc *d, node_t *element, const char *value, size_t length);
const char *doc_element_nonce(const node_t *element, size_t *length);
const char *doc_element_csp_nonce(const node_t *element);
bool doc_element_nonce_attribute_changed(web_doc *d, node_t *element, const char *value);
void doc_element_nonce_connected(web_doc *d, node_t *element);
void doc_css_loaded(web_doc *d, const char *url, const char *final_url, const char *css, size_t n);
bool doc_link_load_current(web_doc *d, node_t *n, uint64_t generation);
bool web_js_stylesheet_event(web_doc *d, node_t *n, bool failed);
bool css_select(web_doc *d, node_t *scope, const char *selector, pvec *out);
bool css_matches(node_t *node, const char *selector, bool *valid);
/* HTML :state(): whether a custom element's states set holds the name */
bool web_custom_state_has(const node_t *n, const char *name);
/* the same over a selector that may hold NUL (U+FFFD to CSS) */
bool css_select_n(web_doc *d, node_t *scope, const char *selector, size_t length, pvec *out);
bool css_matches_n(node_t *node, node_t *scope, const char *selector, size_t length, bool *valid); /* scope NULL: node */
/* CSSOM: a style rule's selectorText; an @namespace rule's prefix (NULL: none) and URI */
void css_rule_selector_serialize(const struct css_rule_info *info, sbuf *out);
bool css_namespace_rule(arena_t *a, const char *text, size_t length, const char **prefix, const char **uri);
/* selectorText's setter: the selectors parsed in sheet's namespaces (NULL: none),
   serialized into out; false when they do not parse */
bool css_selectors_serialize(const sheet_t *sheet, const char *selector, size_t length, sbuf *out);
/* CSSOM "serialize an identifier" and "serialize a string" */
void css_serialize_ident(sbuf *out, const char *name);
void css_serialize_string(sbuf *out, const char *text);
/* CSSOM declaration blocks as text (an element's style attribute, a rule's
   declarations). A block holds longhands (a shorthand sets each of its own) with
   the values they were given; its text (block) writes each declaration, so it
   parses back to the same ones and the cascade reads the given values, and its
   serialization (serialize) writes each value serialized and longhands as their
   shorthands where CSSOM's "serialize a CSS declaration block" does.
   block: the block text css (any declaration text) parses to; serialize: the
   serialization of what css parses to, with the declarations' (longhand) names
   NUL-terminated into names when given; block_serialize: the same of a block's
   text; get: on a block's text, getPropertyValue into value (a shorthand's from
   its longhands) and getPropertyPriority; find: a name's last effective value and
   priority as written in css (no shorthands); value: a specified value as CSSOM
   serializes it; set: on a block's text, setProperty's (removeProperty's, for an
   empty value) new block text into out (1), the value or priority does not parse
   (0, out unchanged), the name is not a property name (-1), or no memory (-2);
   balanced: the text can stand inside a block. A descriptor block (check given)
   has no shorthands. false: no memory. */
/* a declaration's validity in its block (name lowercased unless custom), and
   when serialized is given, its value's serialization; NULL: a property's */
typedef bool css_decl_check(const char *name, const char *value, size_t n, sbuf *serialized);
bool css_decl_block(const char *css, size_t n, css_decl_check *check, sbuf *block);
bool css_decl_get(const char *block, size_t n, const char *property, css_decl_check *check, sbuf *value, bool *important);
bool css_decl_find(const char *css, size_t n, const char *property, const char **value, size_t *value_n, bool *important);
void css_decl_value(const char *property, const char *value, size_t n, sbuf *out);
int css_decl_set(const char *block, size_t n, const char *property, const char *value, const char *priority,
                 css_decl_check *check, sbuf *out);
bool css_decl_serialize(const char *css, size_t n, css_decl_check *check, sbuf *out, sbuf *names);
bool css_decl_block_serialize(const char *block, size_t n, css_decl_check *check, sbuf *out, sbuf *names);
/* An element's inline declarations (CSSOM §6.7). A change through CSSOM sets the style attribute
   to their serialization with the mutate flag: the attribute is not parsed again, and the
   declarations keep the values they were given (a specified serialization may round, as an
   origin colour's channels do, CSS Color 4 §15.2). block: those declarations as a block's text
   (css_decl_block) while the attribute is still their serialization, else NULL (the attribute's
   text stands for them); text: what the cascade parses, block or else the attribute's text (NULL:
   none, or CSP blocked the attribute). set: setProperty (removeProperty for an empty value) on
   block, the element's declarations (css_inline_block, or the block its attribute parses to): 1
   the declarations changed and attribute gets their serialization, else as css_decl_set (0
   nothing changed). replace: the cssText setter, attribute gets the serialization of what text
   parses to; false: no memory. */
const char *css_inline_block(const node_t *e);
const char *css_inline_text(const node_t *e);
int css_inline_set(web_doc *d, node_t *e, const char *block, size_t n, const char *property, const char *value,
                   const char *priority, sbuf *attribute);
bool css_inline_replace(web_doc *d, node_t *e, const char *text, size_t n, sbuf *attribute);
bool css_decl_balanced(const char *text, size_t n);
/* a declaration list's text (comment-free) without its !important
   declarations, the others as written: a keyframe's block, where CSS
   Animations 1 makes them invalid */
void css_decl_drop_important(const char *css, size_t n, sbuf *out);
void web_js_start(web_doc *d, const struct web_host *host);
bool web_js_parser_candidate(web_doc *d, const char *name, const char *is);
node_t *web_js_parser_element(web_doc *d, node_t *node, const char *is, bool finish);
void web_js_parser_inserted(web_doc *d);
bool web_js_shadow_allowed(web_doc *d, node_t *host);
void web_js_parser_remove(web_doc *d, node_t *node);
void web_js_console(web_doc *d, int level, const char *message);
/* styles and layout brought up to date as a CSSOM read would (canvas: a colour that names currentcolor) */
void web_js_flush_layout(web_doc *d);
/* The engine's own measurement (WEB_CONSOLE_TRACE): kept out of the page's console. */
void web_js_trace(web_doc *d, const char *message);
/* A followed hyperlink whose navigable is another top-level traversable: target is "_blank" or
   the name no frame of the page has. The host chooses (a named tab, or a new one only with
   activated). Not exposed as a script popup API. */
bool web_js_auxiliary_link(web_doc *d,node_t *anchor,const char *url,const char *target,bool activated);
bool web_js_video_present(web_doc *d,const struct web_video_patch *patch);
/* The display showed a frame of d: its rendering opportunity is now, the next about period_ms
   later (web_rendering_now). */
void web_js_rendering_now(web_doc *d, double period_ms);
/* d has animation frames or dirty rendering observers waiting for an opportunity. */
bool web_js_rendering_wanted(web_doc *d);
/* The rest of HTML "update the visibility state" once d->visible changed (doc.c): its
   VisibilityStateEntry and the visibilitychange event, in one task of d's realm. */
void web_js_visibility_changed(web_doc *d);
/* d or a document in its frame tree plays sound now: an unmuted media element with its sound
   stream open, or a running AudioContext. */
bool web_js_tab_audible(web_doc *d);
void web_js_focus_control(web_doc *d, node_t *control);
void web_js_face_reset(web_doc *d, node_t *form);
bool web_allow_focus(web_doc *d);
void web_focus_chain(web_doc *child, web_doc *until);
node_t *web_autofocus_candidate(web_doc *d);
void web_js_tick(web_doc *d, uint64_t now);
void web_js_free(web_doc *d);
void web_js_nodes_changed(web_doc *d);
void web_js_nodes_reparented(web_doc *d,node_t *subtree);
node_t *doc_forest_next(node_t *n,node_t *root);
void web_js_link_changed(web_doc *d);
void web_js_window_names_changed(web_doc *d);
uint64_t web_js_input_stream(web_doc *d);
void web_js_retire(web_doc *d);
bool web_js_complete(web_doc *d);
void web_js_frames_sync(web_doc *d);
web_doc *web_live_child(const char *html, size_t len, const char *url, const char *charset,
                        const struct web_host *host, web_doc *parent, node_t *frame, web_doc *inherited_origin);
web_doc *web_live_child_response(const char *html,size_t len,const char *url,const char *charset,
                        const char *headers,const char *type,const struct web_host *host,web_doc *parent,node_t *frame,
                        web_doc *inherited_origin);
/* The document's parser, the HTML parser (d->parser, which also serves
   document.open and document.write) or an XML document's XML parser,
   driven the same way: doc_parser_current names the active one (NULL:
   parsing is over), doc_parser_resume answers as html_resume,
   doc_parser_changed as html_import_changed, and doc_parser_stop ends it. */
const void *doc_parser_current(const web_doc *d);
int doc_parser_resume(web_doc *d, node_t **script);
bool doc_parser_changed(const web_doc *d);
void doc_parser_stop(web_doc *d);
const char *web_effective_url(web_doc *d);
void web_js_loaded(web_doc *d, uint64_t id, const struct web_response *r);
void web_js_socket_event(web_doc *d, uint64_t id, const struct web_socket_event *e);
int64_t web_js_deadline(web_doc *d);
/* The bytes the document's own JavaScript runtime holds; 0 for a frame sharing its owner's. */
size_t web_js_heap_bytes(web_doc *d);
/* --debug-js: QuickJS's census of that runtime's heap (walks every object). */
int web_js_heap_census(web_doc *d, char *out, size_t size);
bool web_js_running(web_doc *d);
bool web_js_enabled(web_doc *d);
bool web_js_dispatch(web_doc *d, node_t *target, const struct web_event *e);
bool web_js_reveal_hidden(web_doc *d, node_t *target);
/* Native edit event: NULL data means InputEvent.data=null. */
bool web_js_input_event(web_doc *d,node_t *target,const char *type,const char *input_type,const char *data,size_t length);
/* One physical edit task encloses beforeinput, native default and input. */
void *web_js_edit_begin(web_doc *d);
void web_js_edit_end(void *scope);
/* The user's edit of an editing host, by its Input Events inputType or a caret
   move (js_document_commands.js's editingAction); 1 when it was taken. */
int web_js_editing_action(web_doc *d,const char *action,bool extend,const char *data,size_t length);
/* The document selection's text, malloc owned; NULL when empty. */
char *web_js_selection_text(web_doc *d,size_t *length);
void web_js_selection_changed(web_doc *d, node_t *node);
/* MIME Sniffing's groups of a MIME type's essence (length bytes, ASCII case-insensitive), as the
   script loader (js.c) and the document loader (doc.c's web_document_kind) read them. */
bool web_javascript_mime_essence(const char *essence, size_t length);
bool web_json_mime_essence(const char *essence, size_t length);

/* Fetch's data URL processor. Input is a serialized URL; outputs are malloc
 * owned, binary body is length-delimited (and also NUL terminated). Return 1
 * on success, 0 for malformed input, -1 for allocation/size failure, -2 when
 * the optional native checkpoint cancels. No payload or MIME length quota. */
struct web_data_url { unsigned char *body; size_t length; char *mime; };
int web_data_url_process(const char *url, struct web_data_url *out,
                         bool (*checkpoint)(void *), void *opaque);
void web_data_url_free(struct web_data_url *data);
/* Infra's forgiving-base64 decode of data[0..*length), in place; *length
 * becomes the decoded length. Return 1, 0 for invalid input, -2 when the
 * optional checkpoint cancels. Shared by data URLs and atob(). */
int web_base64_decode(uint8_t *data, size_t *length, bool (*checkpoint)(void *), void *opaque);
