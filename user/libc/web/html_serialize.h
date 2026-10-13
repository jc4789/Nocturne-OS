#ifndef NOCTURNE_HTML_SERIALIZE_H
#define NOCTURNE_HTML_SERIALIZE_H

#include "webi.h"

void html_serialize_node(const node_t *node, sbuf *out);
void html_serialize_children(const node_t *node, sbuf *out);
void html_serialize_get(const node_t *node,sbuf *out,bool serializable,node_t **roots,size_t count);

enum xml_serialization_result { XML_SERIALIZED, XML_SERIALIZE_INVALID, XML_SERIALIZE_OOM, XML_SERIALIZE_CANCELLED };
/* XMLSerializer uses false. Strict serialization is also available to native
   XML consumers; it is not an HTML inner/outerHTML mode. Failure rolls back n. */
enum xml_serialization_result xml_serialize_node(const node_t *node,sbuf *out,bool require_well_formed);

#endif
