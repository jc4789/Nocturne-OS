/* Internals of the web engine (see web.h). Files:
     util.c    arenas, string buffers, URLs
     html.c    HTML tag IDs (html_lexbor.c builds the tree)
     css.c     CSS parser, selectors, the cascade and computed values
     cssprop.c CSS properties, values, shorthands and the UA stylesheet
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
enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_OTHER, PE_BACKDROP, PE_SLOTTED, PE_PLACEHOLDER };

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
    struct style *style_previous;
    struct node *style_retired_next;
    bool style_dirty, style_children_dirty;
    bool style_self_dirty; /* only this element's own match changed */
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
/* a length or percentage, possibly calc(): resolved at layout time against a base */
enum { LK_AUTO, LK_LEN, LK_NONE, LK_EXPR, LK_NORMAL, LK_NUMBER };
struct cexpr;
typedef struct len {
    uint8_t kind;
    float px, pct; /* LK_LEN: px + pct% of the base; LK_NUMBER: px is the number */
    struct cexpr *expr;
} len_t;
float len_resolve(const len_t *l, float base); /* LK_AUTO/LK_NONE resolve to 0 */
bool css_length_equal(const len_t *,const len_t *); /* semantic computed expression, not arena address */
bool css_length_serialize(const len_t *l, sbuf *out, web_doc *d); /* fallible/cancellable computed value; append/NUL or restore */
/* What a value's grammar accepts (cssprop.c): auto, none, normal, negative values, percentages, and
   LF_LENGTH: unitless numbers other than 0 are not lengths */
enum { LF_AUTO = 1, LF_NONE = 2, LF_NEG = 4, LF_NORMAL = 8, LF_NOPCT = 16, LF_LENGTH = 32 };
struct cx;
bool css_parse_length(const char *s, size_t n, struct cx *cx, len_t *out, int flags);
bool css_number_token(const char **s, const char *end, float *out); /* a CSS <number-token> at *s */
const char *css_parse_url(const char *s, size_t n, arena_t *a); /* url(...) (or a string) unescaped; NULL: not one */
void css_length_complement(arena_t *a, const len_t *x, const len_t *y, len_t *out); /* 100% - x - y (y may be NULL) */
void css_length_mix(arena_t *a, const len_t *x, const len_t *y, double ka, double kb, len_t *out); /* ka·x + kb·y */
bool css_query_length(const char *, size_t, float em, float rem, float vw, float vh, double *value);
struct css_filter_step { float value; bool percentage; };
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
struct css_filter {
    size_t count;
    float opacity; /* product of used, range-clamped steps; not the computed list */
    struct css_filter_step steps[];
};
bool css_filter_serialize(const struct css_filter *filter, sbuf *out, web_doc *d);
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
/* the groups' initial values (a style's NULL group), and whether two groups hold the same computed
   values (NULL: the initial ones) */
extern const struct css_float_shape css_float_shape_initial;
extern const struct css_offset css_offset_initial;
bool css_float_shape_equal(const struct css_float_shape *a, const struct css_float_shape *b);
bool css_offset_equal(const struct css_offset *a, const struct css_offset *b);
enum css_offset_property { CSS_OFFSET_POSITION, CSS_OFFSET_ANCHOR, CSS_OFFSET_ROTATE };
bool css_offset_parse(enum css_offset_property which, const char *v, size_t n, struct cx *cx, void *out, sbuf *specified);
bool css_offset_serialize(enum css_offset_property which, const void *value, sbuf *out, web_doc *d);
bool css_offset_combine(enum css_offset_property which, const void *a, const void *b, double t, bool add,
                        sbuf *out, web_doc *d);
/* the offset shorthand (motion-1 §2.6): the longhands it sets, false when v is not one */
bool css_offset_shorthand(const char *v, size_t n, struct cx *cx, struct css_offset *out, sbuf *specified);
bool css_offset_shorthand_serialize(const struct css_offset *o, sbuf *out, web_doc *d); /* its computed value */
/* CSSOM specified value of a declared shape, Motion Path, shape-margin or shape-image-threshold
   value (name: the property); false leaves the authored text */
bool css_shape_specified(const char *name, const char *v, size_t n, sbuf *out);
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
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_DECIMAL_LZ, LS_LOWER_ALPHA, LS_UPPER_ALPHA,
       LS_LOWER_ROMAN, LS_UPPER_ROMAN, LS_LOWER_GREEK, LS_NONE, LS_STRING };
enum { BS_NONE, BS_HIDDEN, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_GROOVE, BS_RIDGE, BS_INSET, BS_OUTSET };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { TD_UNDERLINE = 1, TD_OVERLINE = 2, TD_LINE_THROUGH = 4 };
enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum { FD_ROW, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE };
enum { FW_NOWRAP, FW_WRAP, FW_WRAP_REVERSE };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE };
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
};

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
typedef struct style {
    uint8_t display, position, float_, clear, white_space, text_align, vertical_align, list_style,
        list_style_inside, font_style, text_transform, text_decoration, overflow, box_sizing, visibility, pointer_events,
        border_collapse, flex_direction, flex_wrap, justify_content, align_items, align_self, align_content, table_layout,
        content_visibility,text_overflow;
    uint8_t border_style[4];
    uint8_t container_type;
    uint8_t box_orient;
    uint8_t transform_box; /* TB_*: the transform-origin/percentage reference box */
    bool legacy_box; /* authored -webkit-box/-webkit-inline-box display */
    uint32_t line_clamp; /* legacy positive line budget; zero = none */
    const char *container_names; /* authored identifier list, NULL = none */
    uint16_t font_weight;
    uint8_t font_family; /* FONT_FAMILY_*; inherited as one byte */
    const char *font_names; /* full authored family list, computed-style arena */
    font_t *named_font;     /* document-owned downloaded face, never AST-owned */
    const struct web_font_choice *font_choices; /* computed-arena composite family */
    uint8_t object_fit;
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
    const struct css_shape *clip_path; /* NULL: none; a computed-arena value */
    /* Groups of rarely set properties that are not inherited, held by pointer so that every
       element's style does not carry them (css_style_float_shape, css_style_offset read them).
       NULL: all their values initial. A group is a computed-arena record that no one writes once a
       style holds it: cssprop.c writes a longhand into a copy and points the style at the copy. */
    const struct css_float_shape *float_shape; /* shape-outside, shape-margin, shape-image-threshold */
    const struct css_offset *offset;           /* Motion Path */
    struct css_clip_rect clip;
    const struct css_transform *transform;
    struct css_translate translate;
    const struct css_transform *rotate, *scale; /* individual properties; NULL = none */
    len_t transform_origin[2];
    struct css_motion motion; /* animation-* and transition-*; not inherited */
    bool animated; /* the cascade applied animation or transition values to this style */
    int z_index, order;
    const char *content; /* ::before / ::after text, NULL = none */
    const char *list_style_string;
    const char *bg_image; /* url() of background-image (absolute, or relative to the document) */
    int bg_img;           /* its index in the document's images + 1, or 0 (set after the cascade) */
    uint8_t bg_repeat;    /* BR_* */
    uint8_t bg_size_kind; /* BSZ_* */
    len_t bg_pos[2];      /* x, y: px + pct% of (area - image) */
    len_t bg_size[2];     /* BSZ_LEN: width, height (LK_AUTO: from the other and the ratio) */
    const char *mask_image; /* mask-image: the background colour shows through this image's alpha */
    int mask_img;
    uint8_t mask_repeat, mask_size_kind;
    len_t mask_pos[2], mask_size[2];
    uint32_t grad[2];     /* a gradient's first and last colour stops (for things that want one colour) */
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
    struct style *before, *after, *backdrop, *placeholder;
} style_t;
/* a style's float shape and Motion Path values, the initial ones when it holds no group */
static inline const struct css_float_shape *css_style_float_shape(const style_t *s) {
    return s->float_shape ? s->float_shape : &css_float_shape_initial;
}
static inline const struct css_offset *css_style_offset(const style_t *s) {
    return s->offset ? s->offset : &css_offset_initial;
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
/* An @import rule's resolved URL (NULL: not fetched) and media query text. */
struct css_rule_import { const char *url, *media; };
/* Parser-owned CSSOM metadata points at the SAME native cascade AST. One
   per top-level rule, so a sheet of fifty thousand rules keeps it small. */
struct css_rule_info {
    struct css_rule_info *next;
    /* text: the rule's text_len bytes in its sheet's source, not terminated */
    const char *text, *selector;
    const struct css_rule_import *import; /* @import only */
    void *native_rule;
    uint32_t text_len, type, id;
    uint8_t layer_kind; /* 1 statement, 2 block; not a CSSRule.type surrogate */
};

/* the document's styling state */
struct styling {
    pvec sheets;   /* sheet_t*, in cascade order */
    css_ctx *ctx;  /* the sheets' rule indexes bound for the current viewport */
    int index_w, index_h;
    bool index_dirty, index_scripting, index_quirks;
};

/* imports: receives struct css_import* for each @import, to be fetched and added with their order */
struct css_import {
    char *url;
    double order;
    const struct css_rule_info *owner;
};
sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
sheet_t *css_parse_sheet_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
void css_sheet_scope(sheet_t *sheet, node_t *shadow_root);
/* The arena the sheet was parsed into now lives at another address: the AST's
   rule index is built later in its owning arena, which it names by address. */
void css_sheet_arena_moved(sheet_t *sheet, arena_t *arena);
sheet_t *css_sheet_instance(arena_t *a, const sheet_t *source, double order, node_t *scope);
void css_sheet_import_context(sheet_t *sheet,sheet_t *parent,const struct css_rule_info *owner);
void css_sheet_media(sheet_t *sheet, const char *media);
struct css_rule_info *css_sheet_rules(sheet_t *sheet, uint32_t *length);
uint32_t css_sheet_import_urls(const sheet_t *sheet); /* how many of its rules are @import with a URL */
bool css_sheet_single_rule(sheet_t *sheet, const char *source, size_t length);
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
void css_box_change_report(web_doc *d); /* debug_js only */
/* An animation sample changes the element's own values only: restyle it, not what selectors match. */
void css_mark_style_own(web_doc *d, node_t *n);
/* v with var() and env() substituted against st's custom properties; false: invalid at computed-value time */
bool css_substitute(const style_t *st, const char *v, sbuf *out);
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
/* Animation values (cssprop.c). The property table owns each longhand's animation type
   (Web Animations 1 §5.3): not animatable, discrete, by computed value, visibility (visible
   wins inside the interval), or display (the value that is not none/hidden wins inside it;
   discrete otherwise). */
enum { CSS_ANIM_NONE, CSS_ANIM_DISCRETE, CSS_ANIM_VALUE, CSS_ANIM_VISIBILITY, CSS_ANIM_DISPLAY };
int css_prop_animation(const struct propdef *p);
/* the longhands a property sets (itself, or a shorthand's), as css_prop_index bits in set */
void css_prop_longhands(const struct propdef *p, uint8_t *set);
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
    void *arg;
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
    unsigned supports_nodes; /* bound arithmetic AST traversal, not just parentheses */
    float cq_width, cq_height;
    bool cq_width_set, cq_height_set;
    bool absolute_lengths; /* relative lengths and percentages fail (Geometry's matrix strings) */
    bool time;             /* dimensions are <time>s, in milliseconds; lengths and percentages fail */
    bool computed_text;    /* the value is computed-value text the animation layer wrote (css_anim_parse):
                              what a style sheet's parse clamps (rgb()'s components) keeps its value */
    bool absolute_units;   /* relative lengths fail, percentages stay (a specified value's calc()) */
    bool angles;           /* dimensions are <angle>s, in degrees; lengths and percentages fail */
};
/* apply a declaration (var() already substituted) unless its properties are all set; false if invalid */
bool css_apply(const struct propdef *p, const char *v, size_t n, struct cx *cx);
bool css_value_supported(const struct propdef *p, const char *value, size_t n);
bool css_value_valid(const struct propdef *p, const char *value, size_t n);
const char *css_value_canonical(arena_t *a, const char *value, size_t n, size_t *out_n);
bool css_supports_declaration(const char *property, size_t pn, const char *value, size_t vn);
bool css_valid_declaration(const char *property, size_t pn, const char *value, size_t vn);
bool css_supports_condition(const char *condition, size_t n, bool implied_parens);
/* start a style: inherited properties from the parent, the others initial */
void css_style_init(style_t *s, const style_t *parent);
/* s takes from's inherited values (what css_style_init copies from a parent)
   and keeps its own others */
void css_style_adopt_inherited(style_t *s, const style_t *from);
/* computed-value fixups after all declarations: currentColor, blockification, border widths */
void css_style_finish(style_t *s, const style_t *parent, bool root);
/* a dimension's pixels against cx: false for an unknown unit; *num for no unit, *pct for % */
bool css_unit_px(const char *u, size_t un, float v, const struct cx *cx, float *px, float *pct, bool *num);

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
    struct box *cb;         /* the block box these coordinates are relative to */
    struct box *inline_paint_owner, *inline_paint_source;
    int inline_run_first, inline_run_last, inline_deco_first, inline_deco_last;
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
    bool style_sync_dirty; /* retirement join: this subtree has changed style borrows */
    bool style_sync_subtree; /* and every box below may borrow one: a display:contents element generated them */
    /* inline formatting results (block containers with inline content) */
    struct run *runs;
    int nruns;
    struct deco *decos;
    int ndecos;
    struct line_fragment *lines;
    int nlines;
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
    /* list items: marker text, or a shape (1 disc, 2 circle, 3 square) */
    const char *marker;
    uint8_t marker_shape;
    /* inline <svg>: its entry in the document's cache */
    struct svg_cache *svg;
} box_t;

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
bool box_block_level(const box_t *b);

struct run {
    float x, y, w; /* y is the baseline */
    const char *s;
    int n;
    style_t *st;
    node_t *link;  /* the <a href> it belongs to */
    box_t *atomic; /* or an atomic inline box at (x, y = its top) */
    bool highlight;
    node_t *node; /* original text/atomic node, for generic DOM event targeting */
    box_t *paint_owner; /* filtered inline group; private layout-arena dependency */
    int paint_next;
    float descent; /* measured with the line; overflow does not query fonts again */
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
void doc_hover_update(web_doc *document,node_t *target); /* native-only CSS state; no JS dispatch */
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
void boxes_discard(web_doc *d); /* unpublish node/top-layer borrowers, then bmem */
/* Widths are float sums of advances, and the same content summed in another order (a line,
   an intrinsic width) may differ in the last bits: content within this much of a width fits it. */
#define LAYOUT_FIT_SLOP 0.01f
void layout_doc(web_doc *d, int width, int height);
void layout_cache_free(web_doc *d);
void layout_clamp_trace_report(web_doc *d);
void layout_invalidate(box_t *b);
void layout_invalidate_fonts(box_t *b);
float box_abs_x(const box_t *b);
float box_abs_y(const box_t *b);
/* Paint/hit/DOMRect positions; box_abs remains unscrolled offset geometry. */
float box_visual_x(const box_t *b);
float box_visual_y(const box_t *b);
/* Within one paint scroll/translation inputs are fixed: memoize visual
   coordinates until box_visual_memo_end restores the previous generation. */
uint32_t box_visual_memo_begin(void);
void box_visual_memo_end(uint32_t previous);
bool box_filter_containing_block(const box_t *b); /* filters/transforms establish descendant containing blocks */
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
/* The used affine matrix of s's transform properties for a reference box of
   w x h whose top-left corner is at (x,y); false if none or not finite. */
bool css_style_transform_matrix(const style_t *s,double x,double y,float w,float h,double m[6]);
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]);
/* The same list as a full 4x4 with perspective and no box (Geometry Interfaces); row-major. */
void css_transform_list_matrix_3d(const struct css_transform *t,double m[16]);
/* Geometry's "parse a string into an abstract matrix": column-major m, *two_d false when a
   3D transform function is in the list. False when the string is no valid list. */
bool css_transform_string_matrix(const char *s,size_t n,double m[16],bool *two_d);
/* Maps a document rectangle (x,y,w,h) through b's and its ancestors' used
   transforms and returns its bounding box. */
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
    float paint_damage[4];
    bool paint_damage_valid, paint_damage_full;
    /* Between a cascade and its publication: the boxes whose styles changed and what they
       painted before. More than these is repainted whole. */
    struct restyle_damage { box_t *box; float rect[4]; } restyle_damage[32];
    uint8_t restyle_damage_n;
    bool restyle_damage_lost;
    struct html_parser *parser;
    struct xml_parser *xml_parser; /* an XML document's, while it loads (xml.h) */
    struct web_js_state *js;
    bool live, dirty, paint_dirty, resources_dirty, images_dirty;
    bool restyle_dirty; /* dirty, but only marked elements' own computed values (CSS animation samples) */
    bool initial_render_done;
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
enum doc_scroll_alignment { DOC_SCROLL_START, DOC_SCROLL_END, DOC_SCROLL_CENTER, DOC_SCROLL_NEAREST };
/* Horizontal-LTR only. Current viewport positions enter through x/y; desired
   positions leave through them. changed must start empty and is heap-owned.
   1 rendered, 0 inactive/no box, -1 allocation, -2 bounded-chain refusal. */
int doc_element_scroll_into_view(web_doc *d, node_t *n, int block, int inline_, bool nearest_only,
                                double *x, double *y, pvec *changed);
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
bool web_js_script_parser_capture(web_doc *d,node_t *node);
bool web_js_encoding_restart(web_doc *d);
bool doc_node_html(web_doc *d, node_t *node, const char *html, size_t n);
bool doc_node_value(web_doc *d, node_t *node, const char *text, size_t n);
bool doc_control_selection_supported(const node_t *node);
uint32_t doc_utf16_length(const char *text);
uint32_t doc_byte_to_utf16(const char *text, size_t byte);
size_t doc_utf16_to_byte(const char *text, uint32_t offset, bool round_up);
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
struct cached_css *doc_css_request_next(web_doc *d, size_t *cursor);
void doc_css_loaded(web_doc *d, const char *url, const char *final_url, const char *css, size_t n);
bool doc_link_load_current(web_doc *d, node_t *n, uint64_t generation);
bool web_js_stylesheet_event(web_doc *d, node_t *n, bool failed);
bool css_select(web_doc *d, node_t *scope, const char *selector, pvec *out);
bool css_matches(node_t *node, const char *selector, bool *valid);
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
