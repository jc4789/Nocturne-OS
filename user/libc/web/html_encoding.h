#pragma once
#include "webi.h"

/* A transport byte stream is decoded once. DOMString callers must not pass
 * through this API: BOM, labels and decoder error replacement do not apply. */
char *html_decode_bytes(web_doc *document, const char *source, size_t length,
                        const char *transport_label, size_t *decoded_length);
/* The same for a text document's bytes (HTML "load a text document"): no prescan, and a
 * JSON MIME type's bytes (json) are UTF-8 unless a BOM or the label says otherwise. */
char *html_decode_text(web_doc *document, const char *source, size_t length,
                       const char *transport_label, bool json, size_t *decoded_length);
void html_transport_label(const char *content_type,char *label,size_t capacity);
bool html_meta_label(const char *label,size_t length,char *canonical,size_t capacity);
bool html_encoding_label(const char *label,size_t length,char *canonical,size_t capacity);
bool html_prescan_label(const char *bytes,size_t length,char *canonical,size_t capacity);
