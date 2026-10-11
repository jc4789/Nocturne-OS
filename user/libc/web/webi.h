/* Internals of the web engine (see web.h). Files:
     util.c    arenas, string buffers, URLs
     html.c    HTML tokenizer and tree builder, character references
     css.c     CSS parser, selectors, the cascade and computed values
     cssprop.c CSS properties, values, shorthands and the UA stylesheet
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

enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_OTHER, PE_BACKDROP }; /* pseudo-elements */

enum { N_DOC, N_ELEM, N_TEXT, N_COMMENT, N_FRAGMENT, N_DOCTYPE, N_PI, N_ATTR };
enum { NS_HTML, NS_SVG, NS_MATHML, NS_NONE };

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
    bool shadow_manual, shadow_declarative;
    /* Native assignment snapshots/lists: these are not child/parent links. */
    struct node *assigned_slot, *assigned_next;
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
    uint16_t tag;
    bool foreign;     /* namespace_id != NS_HTML, retained for renderer/parser */
    const char *name; /* lowercase tag name; case-preserving PI target */
    const char *raw_name;
    const char *public_id, *system_id;
    size_t public_id_len, system_id_len;
    bool doctype_identifiers_sized;
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
    arena_t style_mem; /* element-owned computed values; descendants borrow inherited values */
    arena_t style_previous_mem;
    struct style *style_previous;
    struct node *style_retired_next;
    bool style_dirty, style_children_dirty;
    uint8_t style_form_state; /* supervisor-published form pseudo snapshot; cascade reads only */
    uint32_t style_subtree_work; /* flat-tree element count, refreshed before a full cascade */
    /* Published only after layout joins; CSS workers never read mutable boxes
       or borrow this name list from a retired computed-style arena. */
    char *container_names;
    float container_width, container_height, container_em;
    uint8_t container_type;
    bool container_valid;
    struct node *query_inline_container, *query_size_container;
    struct css_scope *style_scope; /* root-only, borrowed from current immutable rule index */
    struct style *animation_base_style; /* cascade without this node's effects */
    struct css_animation *animations; /* bounded malloc storage, not DOM attributes */
    struct css_motion_state *css_motion; /* document-owned native CSS playback, not a style-arena borrow */
    struct box *box;          /* the element's first box */
    struct box *anchor_block; /* inline elements: the block holding its first line */
    float anchor_dy;
    /* Persistent CSSOM element scroll position; box/layout arenas are rebuilt.
       Only the element's principal box consumes these offsets. */
    double scroll_x, scroll_y;
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
enum { OF_FILL, OF_CONTAIN, OF_COVER, OF_NONE, OF_SCALE_DOWN };
enum { CV_VISIBLE, CV_AUTO, CV_HIDDEN };
enum { CT_NORMAL, CT_INLINE_SIZE, CT_SIZE };
enum { BO_HORIZONTAL, BO_VERTICAL };

struct custom_prop {
    const char *name, *value;
    struct custom_prop *next;
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
enum { CM_BEZIER, CM_LINEAR, CM_STEP_START, CM_STEP_END };
struct css_easing { float x1, y1, x2, y2; uint8_t kind; };
struct css_motion {
    const char *name; /* NULL = none; one animation, never an accepted comma-list */
    float duration, delay, iterations; /* milliseconds; infinity allowed only for iterations */
    struct css_easing easing;
    uint8_t direction, fill, paused;
};
typedef struct style {
    uint8_t display, position, float_, clear, white_space, text_align, vertical_align, list_style,
        list_style_inside, font_style, text_transform, text_decoration, overflow, box_sizing, visibility, pointer_events,
        border_collapse, flex_direction, flex_wrap, justify_content, align_items, align_self, align_content, table_layout,
        content_visibility;
    uint8_t border_style[4];
    uint8_t container_type;
    uint8_t box_orient;
    bool legacy_box; /* authored -webkit-box/-webkit-inline-box display */
    uint32_t line_clamp; /* legacy positive line budget; zero = none */
    const char *container_names; /* authored identifier list, NULL = none */
    uint16_t font_weight;
    uint8_t font_family; /* FONT_FAMILY_*; inherited as one byte */
    const char *font_names; /* full authored family list, computed-style arena */
    font_t *named_font;     /* document-owned downloaded face, never AST-owned */
    uint8_t object_fit;
    len_t object_pos[2]; /* position within (content box - fitted image) */
    float font_size;
    len_t line_height;
    float vertical_align_px;
    uint32_t color, bg_color;
    struct svg_paint svg_fill, svg_stroke;
    float svg_fill_opacity, svg_stroke_opacity;
    uint32_t svg_stop_color;
    float svg_stop_opacity;
    len_t svg_stroke_width;
    uint8_t svg_fill_rule, svg_stroke_cap, svg_stroke_join;
    uint32_t border_color[4];
    float border_width[4];
    float border_radius;
    len_t margin[4], padding[4], inset[4]; /* top right bottom left */
    len_t width, height, min_width, max_width, min_height, max_height;
    len_t text_indent, flex_basis;
    float flex_grow, flex_shrink;
    float gap_row, gap_col;
    float letter_spacing, word_spacing;
    float border_spacing;
    float opacity;
    struct css_motion motion; /* native opacity animation subset; not inherited */
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
    struct custom_prop *vars;
    struct style *before, *after, *backdrop;
} style_t;

/* ---------------------------------------------------------------- stylesheets */
typedef struct sheet sheet_t;
typedef struct css_ctx css_ctx;
/* Parser-owned CSSOM metadata points at the SAME native cascade AST. */
struct css_rule_info {
    struct css_rule_info *next;
    const char *text, *selector, *import_url;
    uint32_t type, id;
    void *native_rule;
};

/* the document's styling state */
struct styling {
    pvec sheets;   /* sheet_t*, in cascade order */
    css_ctx *ctx;  /* rule index for the current viewport */
    int index_w, index_h;
    bool index_dirty, index_scripting, index_quirks;
};

/* imports: receives struct css_import* for each @import, to be fetched and added with their order */
struct css_import {
    char *url;
    double order;
};
sheet_t *css_parse_sheet(arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
sheet_t *css_parse_sheet_serviced(web_doc *d, arena_t *a, const char *css, size_t n, const char *base_url, double order, pvec *imports);
void css_sheet_scope(sheet_t *sheet, node_t *shadow_root);
sheet_t *css_sheet_instance(arena_t *a, const sheet_t *source, double order, node_t *scope);
void css_sheet_media(sheet_t *sheet, const char *media);
struct css_rule_info *css_sheet_rules(sheet_t *sheet, uint32_t *length);
bool css_sheet_single_style(sheet_t *sheet, const char *source, size_t length);
const char *css_ua_sheet(void);
/* compute every element's style for the viewport */
void css_cascade(web_doc *d, int vw, int vh);
/* Retry failed font sources against the published style snapshot. */
void css_refresh_font_selection(web_doc *d);
bool css_containers_update(web_doc *d); /* supervisor, after layout; changed query inputs */
bool css_container_names_valid(const char *s, size_t n);
void css_mark_dirty(web_doc *d, node_t *n);
void css_node_style_free(node_t *n);
void css_node_style_unpublish(node_t *n);
void css_styles_release(web_doc *d);
bool css_style_layout_equal(const style_t *a, const style_t *b);
int css_animation_set(node_t *n, uint8_t pseudo, uint32_t id, const char *text);
void css_animation_free(node_t *n);
void css_motion_tick(web_doc *d, uint64_t now);
int64_t css_motion_deadline(web_doc *d, uint64_t now);
void css_motion_doc_free(web_doc *d);
bool css_easing_parse(const char *s, size_t n, struct css_easing *out);
/* Shared @media / matchMedia evaluator; optional serialization cap >= 9*n+16. */
bool css_media_evaluate(const char *query, int vw, int vh, bool scripting, char *out, size_t cap);
void css_styling_free(struct styling *st);
/* parse a color; false if it is not one */
bool css_color(const char *s, size_t n, uint32_t *out);

/* properties (cssprop.c), used by the cascade in css.c */
struct propdef;
const struct propdef *css_prop_lookup(const char *name, size_t n); /* NULL: not supported */
int css_prop_count(void);
int css_prop_index(const struct propdef *p);
bool css_prop_is_font(const struct propdef *p); /* font-size or the font shorthand */
bool css_prop_is_motion(const struct propdef *p);
/* the values a declaration is resolved against */
struct cx {
    style_t *s;
    const style_t *parent;
    node_t *node;
    float em, rem, vw, vh; /* em: the element's font size (its parent's while computing font-size) */
    arena_t *a;            /* for computed values that need memory */
    uint8_t *set;          /* per property: already given a value by a more important declaration */
    bool font_pass;        /* only font-size is being computed */
    bool supports_probe, invalid; /* scratch validation: never touch a document */
    unsigned supports_nodes; /* bound arithmetic AST traversal, not just parentheses */
    float cq_width, cq_height;
    bool cq_width_set, cq_height_set;
};
/* apply a declaration (var() already substituted) unless its properties are all set; false if invalid */
bool css_apply(const struct propdef *p, const char *v, size_t n, struct cx *cx);
bool css_value_supported(const struct propdef *p, const char *value, size_t n);
const char *css_value_canonical(arena_t *a, const char *value, size_t n, size_t *out_n);
bool css_supports_declaration(const char *property, size_t pn, const char *value, size_t vn);
bool css_supports_condition(const char *condition, size_t n, bool implied_parens);
/* start a style: inherited properties from the parent, the others initial */
void css_style_init(style_t *s, const style_t *parent);
/* computed-value fixups after all declarations: currentColor, blockification, border widths */
void css_style_finish(style_t *s, const style_t *parent, bool root);
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
    node_t *node; /* NULL for anonymous boxes */
    style_t *st;
    struct box *parent, *first, *last, *next;
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
    /* inline formatting results (block containers with inline content) */
    struct run *runs;
    int nruns;
    struct deco *decos;
    int ndecos;
    struct line_fragment *lines;
    int nlines;
    bool clamp_hidden, clamp_truncated;
    struct clamp_trace *clamp_trace;
    /* intrinsic widths cache */
    float min_cw, max_cw;
    bool intrinsic_done;
    arena_t layout_mem, text_mem;
    bool layout_dirty, layout_cache_valid, layout_cache_safe;
    bool stretch_height_dependent;
    float cached_width, cached_cbh, cached_usedh;
    float cached_height, cached_baseline, cached_last_baseline, cached_content_dy;
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

/* Actual inline line boundaries, retained only within a clamp subtree. A
 * valign-shifted run is not a new line; counting distinct run baselines loses
 * that distinction and incorrectly consumes the paragraph's line budget. */
struct line_fragment {
    int first_run, end_run, first_deco, end_deco;
    float top, bottom, baseline, left, right;
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
};

struct deco {
    float x, y, w, h; /* border box */
    style_t *st;
    bool first, last; /* the inline box starts / ends on this line */
    node_t *node;
};

node_t *doc_element_at(web_doc *d,int x,int y); /* hit retargeted to this Document's frame boundary */
void doc_hover_update(web_doc *document,node_t *target); /* native-only CSS state; no JS dispatch */
void doc_active_cancel(web_doc *document); /* retire/free only this context subtree */
node_t *doc_active_target(web_doc *document); /* validates live native press and embedding generation */

void boxes_build(web_doc *d, arena_t *a);
void boxes_restyle(web_doc *d);
bool boxes_update_text(web_doc *d, node_t *n);
void boxes_discard(web_doc *d); /* unpublish node/top-layer borrowers, then bmem */
void layout_doc(web_doc *d, int width, int height);
void layout_clamp_trace_report(web_doc *d);
void layout_invalidate(box_t *b);
float box_abs_x(const box_t *b);
float box_abs_y(const box_t *b);
/* Paint/hit/DOMRect positions; box_abs remains unscrolled offset geometry. */
float box_visual_x(const box_t *b);
float box_visual_y(const box_t *b);
void web_paint_debug_node_free(node_t *node);
bool box_element_scrollable(const box_t *b);
void box_scroll_clamp(box_t *b);

/* Inter / Noto Serif / Maple Mono, with family-aware missing-glyph fallback. */
typedef struct wfont {
    font_t *ttf; /* NULL: monospace bitmap */
    float px;
    bool bold;
} wfont;
wfont style_font(const style_t *st);
float wf_width(const wfont *f, const char *s, size_t n);
void wf_metrics(const wfont *f, float *ascent, float *descent);
float wf_draw(canvas_t *c, const wfont *f, float x, int baseline, const char *s, size_t n, uint32_t color);
float style_line_height(const style_t *st); /* used line-height in px */
const char *web_button_label(node_t *n);   /* the text on an <input> button */
uint32_t doc_canvas_bg(web_doc *d, int *from); /* 1: from <html>, 2: from <body>, 0: white */

/* ---------------------------------------------------------------- the document */
struct web_image {
    char *url;
    image_t *img;
    image_t *scaled; /* cache of the last drawn size */
    char *svg;       /* SVG source, rasterized again at the size it is drawn at */
    size_t svg_n;
    bool done, failed;
    bool image_upgrade; /* Part of request identity: imageset must not share an upgraded image. */
};

/* Monotonic diagnostic counters, sampled at task boundaries. No timing limit
   or DOM semantics depend on these values. Times can overlap (inclusive). */
struct web_profile {
    uint64_t inserts, index_visits, index_ms, rescans, rescan_ms;
    uint64_t shadow_reassigns, shadow_visits;
    uint64_t script_scans, script_visits, script_ms, native_calls, native_ms;
    uint64_t metadata_syncs, metadata_visits, metadata_ms;
    uint64_t style_visits, style_rule_checks, style_index_entries, style_parallel_nodes;
};
struct web_doc {
    struct css_motion_state *css_motions;
    uint64_t css_motion_now; /* supervisor snapshot; immutable during AP cascade */
    web_doc *frame_parent;
    uint32_t sandbox_flags; /* immutable active document restrictions */
    uint64_t sandbox_activation_until; /* only native trusted input writes this */
    web_doc *frame_retired_next;
    node_t *frame_element;
    node_t *address_frame; /* native chrome selection; resolves the current child generation */
    struct web_frame *frames;
    const char *inherited_url; /* about:blank/srcdoc base and origin, never an author property */
    web_doc *origin_owner; /* inherited opaque origins retain native identity */
    node_t *window_token; /* stable for navigation, distinct after removal */
    struct web_frame *frame_container;
    web_doc *frame_free_next;
    struct web_profile profile;
    bool profile_enabled; /* debug_js only; diagnostic counters remain cheap */
    uint8_t clamp_trace_reported; /* at most four numeric diagnostics per document */
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
    uint32_t native_checkpoint_count;
    node_t *details_toggle_first, *details_toggle_last;
    web_doc *template_doc;
    arena_t mem;    /* DOM, stylesheets */
    arena_t smem;   /* computed styles: retained across text-only box rebuilds */
    arena_t bmem;   /* box/anonymous-style ownership, never the computed styles */
    arena_t lmem;   /* layout results */
    arena_t cssmem; /* authored sheets: replaced on a DOM stylesheet mutation */
    node_t *owned_nodes;
    node_t *parser_form_nodes;
    size_t control_bytes;
    size_t canvas_bytes;
    uint8_t dom_create_reported; /* one numeric failure diagnostic per stage */
    bool frame_allocation_reported;
    uint64_t dom_revision;
    uint64_t layout_revision;
    struct html_parser *parser;
    struct web_js_state *js;
    bool live, dirty, paint_dirty, resources_dirty, images_dirty;
    pvec css_cache;
    struct css_sheet_reuse *css_reuse; /* current CSS arena only, no DOM ownership */
    size_t css_reuse_bytes;
    unsigned css_reuse_count;
    size_t css_bytes;
    bool scan_title_seen;
    node_t *root;   /* the document node */
    node_t *html, *head, *body;
    pvec marquees; /* connected candidates collected by the existing resource scan */
    char *url;      /* the document's own URL */
    char *resolved_link; /* native hit/getter URL, borrowed until this doc's next link getter */
    char base[HTTP_URL_MAX];
    bool base_seen;
    bool quirks; /* no (or a legacy) doctype */
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
    pvec svgs;        /* struct svg_cache* */
    box_t *root_box;
    pvec abs_boxes;   /* absolutely positioned boxes, painted last */
    int width, height, doc_h, doc_w;
    int styled_w, styled_h; /* viewport the cascade ran for */
    bool need_style, need_boxes, layout_valid;
    bool style_full_dirty, style_pending_dirty;
    bool font_selection_dirty; /* coalesced failed-font completion; no metrics changed yet */
    bool style_boxes_changed;
    float container_rem; /* immutable root font size of the layout snapshot */
    node_t *style_retired_first;
    node_t *focus;
    node_t *hover_target; /* native pointer target, not an author-set property */
    web_doc *hover_leaf;  /* top document's last hovered child context; arenas remain owned */
    node_t *active_target; /* native primary-button press, independent of hover/events */
    web_doc *active_leaf; /* top document's pressed context; native arenas remain owned */
    struct web_dialog_state *dialogs; /* native modal/top-layer ownership */
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
size_t doc_dom_remaining(web_doc *d);
void doc_dom_budget(web_doc *d);
bool doc_node_adopt(web_doc *d, node_t *node);
node_t *doc_template_content(web_doc *d, node_t *node);
bool doc_templates_finish(web_doc *d, node_t *root);
const char *doc_link_href(web_doc *d, node_t *a); /* node-owner-relative absolute URL; valid until the next call */
void doc_add_stylesheet_text(web_doc *d, const char *css, size_t n, const char *base);
struct web_font_resource {
    struct web_font_resource *next;
    char *url;
    font_t *face;
    bool wanted, loading, done, failed;
};
struct web_font_resource *doc_font_resource(web_doc *d, const char *url);
void doc_font_loaded(web_doc *d, struct web_font_resource *font, const void *bytes, size_t length);

/* Incremental parser: 1 script boundary, 0 complete, -1 allocation failure.
   web_parse remains a scripting-disabled, fully parsed document. */
struct html_parser *html_begin(web_doc *d, const char *html, size_t n, const char *charset, bool scripting);
struct html_parser *html_begin_string(web_doc *d, const char *html, size_t n, bool scripting, bool allow_shadow);
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
node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t n);
node_t *doc_doctype_create(web_doc *d, const char *name, size_t name_len,
    const char *public_id, size_t public_len, const char *system_id, size_t system_len);
int doc_node_insert_validity(node_t *parent, node_t *child, node_t *before);
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
void doc_slot_signal(node_t *slot);
bool doc_slot_nodes(node_t *slot, bool flatten, pvec *out);
bool doc_slot_assign(node_t *slot, node_t **nodes, int count);
/* Appends immediate rendered children. Slots remain nodes (display:contents).
   This never mutates native parent/first/next and never crosses template trees. */
void doc_flat_children(node_t *node, pvec *out);
node_t *doc_flat_parent(node_t *node);
bool doc_pi_target_valid(const char *target, size_t len);
bool doc_node_attr(web_doc *d, node_t *node, const char *name, const char *value);
int doc_attr_index(node_t *node, const char *namespace_uri, const char *name, bool namespaced);
node_t *doc_attr_node(web_doc *d, node_t *element, int index);
node_t *doc_attr_create(web_doc *d, const char *namespace_uri, const char *prefix, const char *local, const char *value);
bool doc_attr_set_ns(web_doc *d, node_t *element, const char *namespace_uri, const char *prefix, const char *local, const char *value);
bool doc_attr_set_node(web_doc *d, node_t *element, node_t *attribute);
bool doc_attr_remove(web_doc *d, node_t *element, int index);
bool doc_attr_value(web_doc *d, node_t *attribute, const char *value);
void doc_attrs_publish(node_t *element, struct attr *attrs, int count);
bool doc_node_move(web_doc *d, node_t *parent, node_t *child, node_t *before);
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
void doc_mutated(web_doc *d, node_t *node);
void doc_rescan(web_doc *d);
void web_marquee_register(web_doc *d, node_t *node);
void web_marquee_set(node_t *node, bool running);
void web_marquee_tick(web_doc *d, uint64_t now);
int64_t web_marquee_deadline(web_doc *d, uint64_t now);
bool web_marquee_box(const box_t *box);
void doc_sync_tree(web_doc *d);
node_t *doc_image_node_next(web_doc *d, node_t *previous);
void doc_image_sync(web_doc *d, node_t *n);
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
/* Native trusted link activation only; not exposed as a script popup API. */
bool web_js_auxiliary_link(web_doc *d,const char *url);
bool web_js_video_present(web_doc *d,const struct web_video_patch *patch);
void web_js_focus_control(web_doc *d, node_t *control);
void web_js_face_reset(web_doc *d, node_t *form);
node_t *web_autofocus_candidate(web_doc *d);
void web_js_tick(web_doc *d, uint64_t now);
void web_js_free(web_doc *d);
void web_js_nodes_changed(web_doc *d);
void web_js_retire(web_doc *d);
bool web_js_complete(web_doc *d);
void web_js_frames_sync(web_doc *d);
web_doc *web_live_child(const char *html, size_t len, const char *url, const char *charset,
                        const struct web_host *host, web_doc *parent, node_t *frame, web_doc *inherited_origin);
web_doc *web_live_child_response(const char *html,size_t len,const char *url,const char *charset,
                        const char *headers,const struct web_host *host,web_doc *parent,node_t *frame,web_doc *inherited_origin);
const char *web_effective_url(web_doc *d);
void web_js_loaded(web_doc *d, uint64_t id, const struct web_response *r);
int64_t web_js_deadline(web_doc *d);
bool web_js_running(web_doc *d);
bool web_js_enabled(web_doc *d);
bool web_js_dispatch(web_doc *d, node_t *target, const struct web_event *e);
bool web_js_reveal_hidden(web_doc *d, node_t *target);
/* Native edit event: NULL data means InputEvent.data=null. */
bool web_js_input_event(web_doc *d,node_t *target,const char *type,const char *input_type,const char *data,size_t length);
/* One physical edit task encloses beforeinput, native default and input. */
void *web_js_edit_begin(web_doc *d);
void web_js_edit_end(void *scope);
void web_js_selection_changed(web_doc *d, node_t *node);

/* URL helpers (util.c) */
bool url_resolve(const char *base, const char *rel, char *out, size_t n);
void url_encode_form(sbuf *b, const char *s); /* application/x-www-form-urlencoded */
