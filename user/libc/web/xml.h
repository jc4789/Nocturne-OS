#ifndef NOCTURNE_XML_H
#define NOCTURNE_XML_H

#include "webi.h"

/* HTML "XML parser" (with XML scripting support disabled) into a native
   document. Expat owns the XML 1.0 grammar, well-formedness, the internal
   DTD subset and entity expansion; xml.c owns Namespaces in XML, the DOM it
   builds, the parsererror element, memory accounting and cancellation. */
enum xml_parse_result { XML_PARSED, XML_PARSE_MALFORMED, XML_PARSE_OOM, XML_PARSE_CANCELLED };
struct xml_parse_error {
    unsigned long line, column; /* 1-based */
    char reason[128];
};
/* Parse into d, an XML document whose root has no children. string: input is
   a DOMString's UTF-8 (the encoding is UTF-8, a declaration does not apply);
   otherwise transport bytes decoded by BOM, transport_label (NULL or empty:
   none), the XML declaration, then UTF-8. checkpoint is the document whose
   native checkpoint may cancel the parse. Anything but XML_PARSED leaves d's
   root without children. */
enum xml_parse_result xml_parse_document(web_doc *d, web_doc *checkpoint, const char *input, size_t length,
                                         bool string, const char *transport_label, struct xml_parse_error *error);
/* DOMParser: append the parsererror element describing error to d's root. */
bool xml_error_document(web_doc *d, const struct xml_parse_error *error);
/* DOM "XML document" without a browsing context (createDocument, new
   Document()): an empty document of content_type (NULL: application/xml);
   xml_interface: it implements XMLDocument. */
web_doc *xml_document_new(web_doc *family, const char *url, const char *content_type, bool xml_interface);
/* DOMParser and XHR: a new XML document parsed from input. On a
   well-formedness error, with error_document the document holds the
   parsererror element; otherwise NULL and *result says why. */
web_doc *xml_document_parse(web_doc *family, web_doc *checkpoint, const char *input, size_t length, bool string,
                            const char *transport_label, const char *url, const char *content_type,
                            bool error_document, enum xml_parse_result *result);

#endif
