/* XPath 1.0 (https://www.w3.org/TR/1999/REC-xpath-19991116/) over the native
   DOM, for DOM's XPathEvaluatorBase (https://dom.spec.whatwg.org/#xpath).

   The data model maps the DOM as DOM Level 3 XPath section 1.2 does: an Attr's
   parent is its element, namespace declaration attributes are not attributes,
   the first non-empty Text of adjacent Text siblings stands for one XPath text
   node, and namespace nodes come from the element's own namespace and the
   xmlns attributes in scope. An HTML document uses the HTML namespace as the
   default element namespace and compares HTML elements' and their attributes'
   names in ASCII lowercase (HTML "Interactions with XPath and XSLT", DOM's
   HTML-document names).

   A compiled expression owns no DOM pointer; it may be evaluated against any
   document. The JS bindings are js_xpath.js and js_xpath_native.h. */
#pragma once
#include "webi.h"

enum xpath_status {
    XPATH_OK = 0,
    XPATH_SYNTAX = 1,         /* not an expression the DOM can evaluate (INVALID_EXPRESSION_ERR) */
    XPATH_NAMESPACE = 2,      /* a QName prefix the resolver does not bind (NAMESPACE_ERR) */
    XPATH_TYPE = 3,           /* the value has no conversion to the requested result type */
    XPATH_CONTEXT = 4,        /* not a permitted context node (NOT_SUPPORTED_ERR) */
    XPATH_RESULT_TYPE = 5,    /* not one of XPathResult's result types (NOT_SUPPORTED_ERR) */
    XPATH_NAMESPACE_NODE = 6, /* the node-set result holds a namespace node, which no DOM Node is */
    XPATH_DEPTH = 7,          /* nested deeper than the native stack allows */
    XPATH_EXCEPTION = 8,      /* the resolver failed; its exception is pending in the caller */
    XPATH_OOM = 9,
    XPATH_CANCELLED = 10,     /* web_native_checkpoint stopped the evaluation */
};

/* XPathResult's result types. */
enum {
    XPATH_ANY_TYPE, XPATH_NUMBER_TYPE, XPATH_STRING_TYPE, XPATH_BOOLEAN_TYPE,
    XPATH_UNORDERED_NODE_ITERATOR_TYPE, XPATH_ORDERED_NODE_ITERATOR_TYPE,
    XPATH_UNORDERED_NODE_SNAPSHOT_TYPE, XPATH_ORDERED_NODE_SNAPSHOT_TYPE,
    XPATH_ANY_UNORDERED_NODE_TYPE, XPATH_FIRST_ORDERED_NODE_TYPE
};

struct xpath_expr;

/* The namespace resolver, asked once for each prefixed QName in a node test,
   in source order. It returns 1 with a malloc'd URI (*uri_length bytes,
   NUL-terminated; the empty string is no namespace), 0 when the prefix has no
   binding, or -1 when it failed with an exception pending. */
typedef int (*xpath_resolve_fn)(void *opaque, const char *prefix, size_t length, char **uri, size_t *uri_length);

/* resolve NULL: no resolver, so every prefix is unbound. detail receives a
   short NUL-terminated explanation (the prefix for XPATH_NAMESPACE). */
enum xpath_status xpath_compile(const char *source, size_t length, xpath_resolve_fn resolve, void *opaque,
                                struct xpath_expr **out, char *detail, size_t detail_size);
void xpath_free(struct xpath_expr *expr);

struct xpath_result {
    unsigned type;                      /* the requested type, ANY_TYPE resolved */
    double number;
    bool boolean;
    char *string; size_t string_length; /* STRING_TYPE: malloc'd UTF-8 */
    node_t **nodes; size_t count;       /* node types: malloc'd, document order, attributes as Attr nodes */
};
enum xpath_status xpath_evaluate(const struct xpath_expr *expr, node_t *context, unsigned type, struct xpath_result *out);
void xpath_result_free(struct xpath_result *result);
