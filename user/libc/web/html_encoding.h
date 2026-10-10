#pragma once
#include "webi.h"

/* A transport byte stream is decoded once. DOMString callers must not pass
 * through this API: BOM, labels and decoder error replacement do not apply. */
char *html_decode_bytes(web_doc *document, const char *source, size_t length,
                        const char *transport_label, size_t *decoded_length);
void html_transport_label(const char *content_type,char *label,size_t capacity);
bool html_meta_label(const char *label,size_t length,char *canonical,size_t capacity);
