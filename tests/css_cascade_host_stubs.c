/* What the cascade and dom.c reach outside the units tests/check_registered_properties.py links,
   answered as a document without scripts, fonts, forms, dialogs, frames, animations or boxes answers
   it (tests/css_cascade_host.c). The style work runs on the calling thread: parallel_*'s stages run
   their items in order, as parallel.c does without helpers. */
#include "webi.h"
#include "parallel.h"
#include "form_value.h"

/* doc.c */
void doc_dom_budget(web_doc *d) { if (d) d->mem.limit = 0; }
size_t doc_dom_remaining(web_doc *d) { (void)d; return SIZE_MAX / 2; }
struct web_font_resource *doc_font_resource(web_doc *d, const char *url) { (void)d; (void)url; return NULL; }
void doc_image_node_owned(node_t *n) { (void)n; }
void doc_referrer_meta(web_doc *d, node_t *node) { (void)d; (void)node; }
bool doc_style_csp_allowed(web_doc *d, node_t *style, const char *text, size_t length) {
    (void)d; (void)style; (void)text; (void)length; return true;
}
node_t *doc_active_target(web_doc *document) { (void)document; return NULL; }
void doc_details_attribute_changed(web_doc *d, node_t *details, const char *name, bool old_present) {
    (void)d; (void)details; (void)name; (void)old_present;
}
void doc_details_inserted(node_t *subtree) { (void)subtree; }
bool doc_option_selected(node_t *option) { (void)option; return false; }
void doc_control_init(web_doc *d, node_t *node) { (void)d; (void)node; }
void doc_control_checked(web_doc *d, node_t *node, bool checked) { (void)d; (void)node; (void)checked; }
const char *web_effective_url(web_doc *d) { return d->url; }
void html_bridge_native_changed(node_t *node, unsigned flags) { (void)node; (void)flags; }
/* web_url.c's: an absolute reference as it is, else joined to the base's directory (the pages resolve
   only plain relative paths) */
int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    *out = NULL;
    if (!rel) return -1;
    const char *p = rel;
    while (*p && *p != ':' && *p != '/' && *p != '?' && *p != '#') p++;
    size_t b = base ? strlen(base) : 0;
    if (*p == ':' || !base) { *out = strdup(rel); return *out ? 1 : -1; }
    const char *slash = strrchr(base, '/');
    size_t keep = slash ? (size_t)(slash - base) + 1 : b;
    size_t r = strlen(rel);
    *out = malloc(keep + r + 1);
    if (!*out) return -1;
    memcpy(*out, base, keep);
    memcpy(*out + keep, rel, r + 1);
    return 1;
}
/* the box tree: there is none */
bool box_pseudo_generated(node_t *node, const style_t *origin, const style_t *pseudo) {
    (void)node; (void)origin;
    return pseudo && pseudo->content != NULL;
}
void boxes_discard(web_doc *d) { (void)d; }
void boxes_note_child_list(web_doc *d, node_t *parent, node_t *subtree) { (void)d; (void)parent; (void)subtree; }
void boxes_note_foreign(web_doc *d, node_t *n) { (void)d; (void)n; }
void boxes_note_regen(web_doc *d, node_t *n) { (void)d; (void)n; }
void boxes_require_full(web_doc *d, const char *reason) { (void)d; (void)reason; }
bool boxes_update_text(web_doc *d, node_t *n) { (void)d; (void)n; return true; }
void layout_invalidate(box_t *b) { (void)b; }
void layout_invalidate_fonts(box_t *b) { (void)b; }
int display_bits_per_channel(void) { return 8; }
/* layout.c's: whether a style change needs a layout pass, which no box here has */
bool css_translation_active(const style_t *s) {
    return s && (s->transform || s->translate.active || s->rotate || s->scale || (s->offset && s->offset->path));
}
bool css_transform_general(const style_t *s) { (void)s; return true; }
/* parallel.c without helpers */
static void *locals[16];
void *thread_local_get(unsigned slot) { return slot < 16 ? locals[slot] : NULL; }
void thread_local_set(unsigned slot, void *value) { if (slot < 16) locals[slot] = value; }
bool parallel_active(void) { return false; }
unsigned parallel_worker_index(void) { return 0; }
void parallel_record_work(enum parallel_stage stage, uint64_t units) { (void)stage; (void)units; }
bool parallel_for_stage(enum parallel_stage stage, size_t count, parallel_fn fn, void *context, parallel_service_fn service,
                        void *service_context) {
    (void)stage; (void)service; (void)service_context;
    for (size_t i = 0; i < count; i++) fn(i, context);
    return true;
}
bool parallel_call_stage(enum parallel_stage stage, parallel_fn fn, void *context, parallel_service_fn service,
                         void *service_context) {
    (void)stage; (void)service; (void)service_context;
    fn(0, context);
    return true;
}
/* scripts: none run (the page's do, in the realm, not in a document's script engine) */
void web_js_console(web_doc *d, int level, const char *message) { (void)d; (void)level; (void)message; }
bool web_js_enabled(web_doc *d) { (void)d; return true; }
bool web_js_csp_style_allowed(web_doc *d, node_t *n, bool attribute, const char *text, size_t length) {
    (void)d; (void)n; (void)attribute; (void)text; (void)length; return true;
}
void web_js_link_changed(web_doc *d) { (void)d; }
void web_js_nodes_changed(web_doc *d) { (void)d; }
void web_js_nodes_reparented(web_doc *d, node_t *subtree) { (void)d; (void)subtree; }
bool web_js_nodes_same_forest(web_doc *document, node_t *node, node_t *parent) { (void)document; (void)node; (void)parent; return true; }
void web_js_window_names_changed(web_doc *d) { (void)d; }
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
void web_avmedia_checkpoint(void) {}
void web_canvas_attr_changed(node_t *node, const char *name) { (void)node; (void)name; }
void web_frames_detach_tree(web_doc *document, node_t *root) { (void)document; (void)root; }
/* forms, dialogs, selects: none of the pages' elements are controls */
bool web_control_disabled(const node_t *n) { (void)n; return false; }
int web_control_in_range(web_doc *d, node_t *n) { (void)d; (void)n; return 0; }
bool web_control_read_write(const node_t *n) { (void)n; return false; }
bool web_control_required(const node_t *n) { (void)n; return false; }
bool web_control_required_applicable(const node_t *n) { (void)n; return false; }
uint32_t web_control_validity(web_doc *d, node_t *n) { (void)d; (void)n; return 0; }
bool web_control_will_validate(const node_t *n) { (void)n; return false; }
bool web_custom_state_has(const node_t *n, const char *name) { (void)n; (void)name; return false; }
box_t *web_dialog_backdrop(web_doc *d, node_t *n) { (void)d; (void)n; return NULL; }
bool web_dialog_is_modal(web_doc *d, const node_t *n) { (void)d; (void)n; return false; }
void web_dialog_removed(web_doc *d, node_t *subtree) { (void)d; (void)subtree; }
void web_dialog_style(web_doc *d, node_t *n, style_t *st) { (void)d; (void)n; (void)st; }
void web_dialog_sync(web_doc *d) { (void)d; }
bool web_form_constraints_valid(web_doc *d, node_t *form_or_fieldset) { (void)d; (void)form_or_fieldset; return true; }
web_node *web_form_owner(web_doc *d, web_node *control) { (void)d; (void)control; return NULL; }
bool web_form_style_snapshot(web_doc *d, unsigned features) { (void)d; (void)features; return true; }
enum web_input_kind web_input_type(const node_t *n) { (void)n; return WEB_INPUT_TEXT; }
const char *web_input_edit_text(const node_t *n) { (void)n; return ""; }
bool web_presentation_matches(node_t *n) { (void)n; return false; }
void web_select_attribute_changed(web_doc *d, node_t *node, const char *name, bool present) {
    (void)d; (void)node; (void)name; (void)present;
}
void web_select_inserted(web_doc *d, node_t *subtree, node_t *old_parent) { (void)d; (void)subtree; (void)old_parent; }
void web_select_sync(web_doc *d, node_t *select, bool reset) { (void)d; (void)select; (void)reset; }
/* animation.c: no element is animated */
void web_animations_cascading(web_doc *d, bool on) { (void)d; (void)on; }
void web_motion_animations(struct web_motion_site *site) { (void)site; }
void web_motion_cancel(node_t *n, int pseudo) { (void)n; (void)pseudo; }
size_t web_motion_sample(struct web_motion_site *site, bool transitions, uint8_t *animated) {
    (void)site; (void)transitions; (void)animated; return 0;
}
void web_motion_transitions(struct web_motion_site *site, const style_t *before, const style_t *after, const uint8_t *animated) {
    (void)site; (void)before; (void)after; (void)animated;
}
int web_motion_wanted(const node_t *n, uint8_t pseudo, const style_t *base) { (void)n; (void)pseudo; (void)base; return 0; }
bool doc_control_selection_supported(const node_t *node) { (void)node; return false; }
uint32_t doc_utf16_length(const char *text) {
    uint32_t length = 0;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++)
        if ((*p & 0xc0) != 0x80) length += (*p & 0xf8) == 0xf0 ? 2 : 1; /* a 4-byte sequence is two code units */
    return length;
}
