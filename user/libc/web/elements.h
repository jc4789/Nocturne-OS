/* Native disclosure widgets. No parallel JavaScript DOM or layout tree. */
#pragma once
#include "webi.h"

node_t *doc_details_summary(const node_t *details);
node_t *doc_details_activation(node_t *target);
bool doc_details_toggle(web_doc *d, node_t *details);
void doc_details_attribute_changed(web_doc *d, node_t *details, const char *name, bool old_present);
void doc_details_inserted(node_t *subtree);
/* Parser snapshot publication is transactional: field import only queues a
   state change; exclusivity is enforced after every parent/child link exists. */
void doc_details_parser_attribute_changed(node_t *details, bool old_open);
void doc_details_parser_finish(node_t *root);
node_t *doc_details_take_toggle(web_doc *d, bool *old_open, bool *new_open);
