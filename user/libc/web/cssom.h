/* Native style/link CSSOM. Rule metadata is owned by the real CSS parser AST. */
#pragma once
#include "webi.h"
enum { CSSOM_OK, CSSOM_OOM, CSSOM_INDEX, CSSOM_SYNTAX, CSSOM_UNSUPPORTED, CSSOM_INACTIVE, CSSOM_HIERARCHY, CSSOM_SECURITY, CSSOM_NOT_ALLOWED };
struct cssom_link_source {
    const char *text, *base;
    size_t length;
    char *url, *owned_text;
};
int doc_cssom_link_source(web_doc *d, node_t *node, struct cssom_link_source *source);
bool cssom_owner_disabled(node_t *node);
node_t *cssom_list_owner(web_doc *d, node_t *root, uint32_t index, bool select, uint32_t *count, int *error);
struct cssom_sheet {
    struct cssom_sheet *next;
    node_t *owner;
    web_doc *document;
    arena_t arena;
    sheet_t *ast;
    struct css_rule_info *removed;
    char *source, *base, *media;
    char *dom_source, *dom_href, *href;
    size_t length;
    size_t dom_length;
    uint64_t observed_dom_revision;
    uint32_t id, next_rule_id;
    bool associated, disabled, origin_clean, constructed;
};
struct cssom_sheet *cssom_style_sheet(web_doc *d, node_t *owner, int *error);
struct cssom_sheet *cssom_style_find(node_t *owner, uint32_t id);
int cssom_insert(struct cssom_sheet *sheet, const char *text, size_t length, uint32_t index);
int cssom_delete(struct cssom_sheet *sheet, uint32_t index);
struct cssom_sheet *cssom_construct(web_doc *d, int *error);
int cssom_replace_sync(struct cssom_sheet *sheet, const char *text, size_t length);
int cssom_adopt(node_t *root, struct cssom_sheet **sheets, uint32_t count);
void cssom_changed(struct cssom_sheet *sheet);
void cssom_style_text_changed(node_t *node);
void cssom_style_lifecycle(node_t *subtree);
void cssom_style_free(node_t *node);
