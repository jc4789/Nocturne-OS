/* Native CSSOM: style sheets and their rule trees. css.c's CSSOM parse builds
   each tree (the rules' kinds, children, parents and source ranges) in the same
   pass as the sheet's cascade AST; this file owns the trees and every change to
   them, so the cascade and CSSOM always read one parse. js_cssom.h binds it to
   js_stylesheets.js, a thin wrapper over these rules' ids. */
#pragma once
#include "webi.h"
enum { CSSOM_OK, CSSOM_OOM, CSSOM_INDEX, CSSOM_SYNTAX, CSSOM_UNSUPPORTED, CSSOM_INACTIVE, CSSOM_HIERARCHY,
       CSSOM_SECURITY, CSSOM_NOT_ALLOWED, CSSOM_NOT_FOUND, CSSOM_INVALID_STATE };
struct cssom_link_source {
    const char *text, *base;
    size_t length;
    char *url, *owned_text;
};
int doc_cssom_link_source(web_doc *d, node_t *node, struct cssom_link_source *source);
/* the same for an @import rule's absolute URL: a data: URL's text, or a
   completed fetch's from the document's cache (CSSOM_INACTIVE: none yet) */
int doc_cssom_import_source(web_doc *d, const char *url, struct cssom_link_source *source);
bool cssom_owner_disabled(node_t *node);
node_t *cssom_list_owner(web_doc *d, node_t *root, uint32_t index, bool select, uint32_t *count, int *error);
/* One CSSOM parse of a text, in its own arena: the cascade AST, the rule tree,
   the comment-free text every rule points into, and an index of each rule's
   children (top: the top-level rules, malloc'd, room for cap). Once a change
   has been spliced in rule by rule, each top-level rule's text is its own
   (spliced): the parse's text is theirs in order, no longer source. */
struct cssom_parse {
    arena_t arena;
    sheet_t *ast;
    const char *source;
    size_t length;
    struct css_rule_info **top;
    uint32_t count, cap;
    bool spliced;
};
/* Rules removed from a sheet while a CSSRule object stood for one of them: a
   parse of the removed rule inside its old ancestors' preludes (root_id the
   rule's id, the ancestors with none), or a whole old parse that replace()
   took the sheet from (root_id 0). They keep their ids and stay mutable, but
   belong to no sheet and reach no cascade. */
struct cssom_tree {
    struct cssom_tree *next;
    struct cssom_parse p;
    uint32_t root_id;
};
struct cssom_slot;
struct cssom_sheet {
    struct cssom_sheet *next;
    node_t *owner;
    web_doc *document;
    struct cssom_parse p;
    struct cssom_tree *detached;
    struct cssom_slot *slots; /* id -> rule, over p and detached */
    uint32_t slot_mask, slot_count;
    /* the text bytes p's live top-level rules hold, and those of the rules
       its arena still holds after their change (a whole parse frees them) */
    size_t live, garbage;
    char *base;
    char *media;     /* the sheet's MediaList, serialized; NULL: empty */
    char *dom_media; /* the media attribute it was last set from */
    char *dom_source, *dom_href, *href;
    size_t dom_length;
    uint64_t observed_dom_revision;
    /* An @import rule's sheet (CSSOM: the rule's associated CSS style sheet),
       in the list of the owner its parent sheet is in: parent's rule owner_rule
       made it. It reaches the cascade while that rule stands in parent. */
    struct cssom_sheet *parent;
    uint32_t owner_rule;
    uint32_t id, next_rule_id;
    bool associated, disabled, origin_clean, constructed;
};
struct cssom_sheet *cssom_style_sheet(web_doc *d, node_t *owner, int *error);
struct cssom_sheet *cssom_style_find(node_t *owner, uint32_t id);
struct cssom_sheet *cssom_construct(web_doc *d, int *error);
/* new CSSStyleSheet(options): base_url (NULL: none) parsed against the
   document's base URL is the sheet's base; CSSOM_NOT_ALLOWED when it is not a URL */
struct cssom_sheet *cssom_construct_based(web_doc *d, const char *base_url, int *error);
/* CSSImportRule.styleSheet: the sheet of sheet's @import rule, made from the
   rule's loaded resource the first time; NULL while it has none (no URL, its
   supports() false, not loaded, failed) */
struct cssom_sheet *cssom_import_sheet(struct cssom_sheet *sheet, const struct css_rule_info *rule, int *error);
/* the sheet sheet's @import rule rule_id already has, or NULL (the cascade's) */
struct cssom_sheet *cssom_import_child(const struct cssom_sheet *sheet, uint32_t rule_id);
int cssom_replace_sync(struct cssom_sheet *sheet, const char *text, size_t length);
int cssom_adopt(node_t *root, struct cssom_sheet **sheets, uint32_t count);
void cssom_changed(struct cssom_sheet *sheet);
void cssom_style_text_changed(node_t *node);
void cssom_style_lifecycle(node_t *subtree);
void cssom_style_free(node_t *node);

/* ---- rules, by id (0: the sheet itself where a container is meant) */
struct css_rule_info *cssom_rule(struct cssom_sheet *sheet, uint32_t id, bool *attached);
/* the index-th child of a rule (or of the sheet), now standing for a CSSRule object */
struct css_rule_info *cssom_child(struct cssom_sheet *sheet, uint32_t parent, uint32_t index, uint32_t *count);
/* parentRule: NULL at the top of a sheet and at a removed rule's root */
struct css_rule_info *cssom_parent_rule(struct cssom_sheet *sheet, const struct css_rule_info *rule);
/* insert a CSS rule / remove a CSS rule (CSSOM 6.4.2), into the sheet or a rule */
int cssom_insert(struct cssom_sheet *sheet, uint32_t parent, const char *text, size_t length, uint32_t index);
int cssom_delete(struct cssom_sheet *sheet, uint32_t parent, uint32_t index);
/* serialize a CSS rule (CSSOM 6.4.3 and each rule's specification) */
void cssom_serialize(struct cssom_sheet *sheet, const struct css_rule_info *rule, sbuf *out);
/* A rule's attribute, by its IDL name; false: null (or not this rule's). */
bool cssom_attribute(struct cssom_sheet *sheet, const struct css_rule_info *rule, const char *name, sbuf *out);
/* An attribute's setter: CSSOM_SYNTAX when the value does not parse (setters
   the specifications make throw, the binding throws for; others do nothing). */
int cssom_set_attribute(struct cssom_sheet *sheet, uint32_t id, const char *name, const char *value, size_t length);
/* a rule's declarations as written, their validity in its block, and setting them */
void cssom_declarations(const struct css_rule_info *rule, const char **text, size_t *length);
/* the rule's declaration block serialized: the declarations its block keeps
   (a keyframe's without !important ones, CSS Animations 1); false: no memory */
bool cssom_block(const struct css_rule_info *rule, sbuf *out);
css_decl_check *cssom_declaration_check(const struct css_rule_info *rule);
int cssom_set_declarations(struct cssom_sheet *sheet, uint32_t id, const char *text, size_t length);
/* MediaList of the sheet (id 0), an @media rule or an @import rule:
   op 0 mediaText, 1 set mediaText, 2 appendMedium, 3 deleteMedium */
int cssom_media(struct cssom_sheet *sheet, uint32_t id, int op, const char *text, size_t length, sbuf *out);
/* CSSKeyframesRule.findRule: the index of the last keyframe whose keyText is key's, or -1 */
int64_t cssom_keyframe_find(struct cssom_sheet *sheet, uint32_t id, const char *key, size_t length);
/* CSSFontFeatureValuesMap: block's entries as "name\0values\0" pairs, or set
   (values NULL: delete name; name NULL: clear) */
bool cssom_feature_values(struct cssom_sheet *sheet, const struct css_rule_info *rule, const char *block, sbuf *out);
int cssom_set_feature_values(struct cssom_sheet *sheet, uint32_t id, const char *block, const char *name,
                             const uint32_t *values, uint32_t count);
