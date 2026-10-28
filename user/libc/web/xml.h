#ifndef NOCTURNE_XML_H
#define NOCTURNE_XML_H

#include "webi.h"

/* HTML "XML parser" into a native document. Expat owns the XML 1.0 grammar,
   well-formedness, the internal DTD subset and entity expansion; xml.c owns
   Namespaces in XML, the DOM it builds, the parsererror element, memory
   accounting and cancellation. DOMParser and XHR parse with XML scripting
   support disabled (xml_parse_document); a browsing context's document
   with it enabled (xml_parser_begin), driven by doc.c's parser loop. */
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

/* HTML "load an XML document": the XML parser, with XML scripting support
   enabled, of d, a new browsing context's document that has no tree yet.
   It makes d an XML document and its tree, and reads its own copy of the
   response bytes, decoded by BOM, transport_label (NULL or empty: none),
   the XML declaration, then UTF-8. NULL: out of memory. */
struct xml_parser *xml_parser_begin(web_doc *d, const char *input, size_t length, const char *transport_label);
/* Parse on, as html_resume: HTML_PARSE_SCRIPT after a script element's end
   tag (*script: the element, to prepare after a microtask checkpoint),
   HTML_PARSE_YIELD after a slice of work, HTML_PARSE_DONE at the end of
   the input or at a well-formedness error (the document's tree is then its
   parsererror element), HTML_PARSE_FAILED at the document's memory limit
   and HTML_PARSE_CANCELLED at a native checkpoint; the last two keep the
   tree so far. */
int xml_parser_resume(struct xml_parser *x, node_t **script);
/* The latest resume changed the document's tree. */
bool xml_parser_changed(const struct xml_parser *x);
void xml_parser_finish(struct xml_parser *x);

#endif
