#ifndef NOCTURNE_HTML_SERIALIZE_H
#define NOCTURNE_HTML_SERIALIZE_H

#include "webi.h"

void html_serialize_node(const node_t *node, sbuf *out);
void html_serialize_children(const node_t *node, sbuf *out);

#endif
