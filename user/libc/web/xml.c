/* HTML "XML parser" over Expat (third_party/expat, README.nocturne.md).
   https://html.spec.whatwg.org/multipage/xhtml.html#parsing-xhtml-documents
   Expat runs without its namespace mode: the DOM keeps each element's
   attributes in source order with its xmlns declarations, and the Namespaces
   in XML errors are this file's. No external entity is fetched; the HTML
   Standard's XHTML and MathML public identifiers read the named character
   reference DTD that scripts/xml_xhtml_entities.py generates.
   Entities expand as XML specifies, without an amplification cap: every byte
   Expat and the pending text hold is charged to the document, an allocator
   failure fails the parse, and the native checkpoint cancels a long
   expansion. Element depth lives in heap stacks, never in C recursion.
   One parser serves both callers: DOMParser and XHR parse a whole input
   at once with XML scripting support disabled; a browsing context's
   document (doc.c) is parsed a slice at a time with it enabled, and Expat
   stops (resumably) after each script's end tag so the document's parser
   loop prepares the script, with the HTML parser's html_resume results. */
#include "xml.h"
#include "html_encoding.h"
#include "html_policy.h"
#include "elements.h"
#include "expat_config.h" /* the features the library was built with */
#include <expat.h>
#include <nocturne.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include "xml_xhtml_entities.h"

#define XML_PARSERERROR_NS "http://www.mozilla.org/newlayout/xml/parsererror.xml"
#define XML_FEED_BYTES ((size_t)1 << 20)
/* A document's parse is cooperative like the HTML parser's (html_lexbor.c):
   Expat reads slices, and a resume returns after these many bytes or this
   long. Work quanta, not limits on the document. */
#define XML_SLICE_BYTES ((size_t)16384)
#define XML_RESUME_BYTES ((size_t)131072)
#define XML_RESUME_MS 16u

/* ---------------------------------------------------------------- accounting */
/* Expat's allocator has no context argument. A parse runs on its document's
   thread; the parse being created or fed owns a new block, and each block
   records its owner for realloc and free. A parse started from a checkpoint
   inside another restores the outer one when it returns. */
struct xml_memory { web_doc *d; size_t live; };
static struct xml_memory *xml_memory_current;
typedef union { struct { struct xml_memory *owner; size_t size; } h; max_align_t align; } xml_block;

static bool charge(struct xml_memory *m, size_t size) {
    if (size > SIZE_MAX - m->live || size > SIZE_MAX - m->d->control_bytes) return false;
    m->live += size; m->d->control_bytes += size;
    return true;
}
static void release(struct xml_memory *m, size_t size) {
    m->live -= size; m->d->control_bytes -= size;
}
static void *xml_malloc(size_t size) {
    struct xml_memory *m = xml_memory_current;
    if (!m || size > SIZE_MAX - sizeof(xml_block) || !charge(m, size)) return NULL;
    xml_block *b = malloc(sizeof *b + size);
    if (!b) { release(m, size); return NULL; }
    b->h.owner = m; b->h.size = size;
    return b + 1;
}
static void xml_free(void *p) {
    if (!p) return;
    xml_block *b = (xml_block *)p - 1;
    release(b->h.owner, b->h.size);
    free(b);
}
static void *xml_realloc(void *p, size_t size) {
    if (!p) return xml_malloc(size);
    xml_block *b = (xml_block *)p - 1;
    struct xml_memory *m = b->h.owner;
    size_t old = b->h.size;
    if (size > SIZE_MAX - sizeof *b || (size > old && !charge(m, size - old))) return NULL;
    xml_block *grown = realloc(b, sizeof *b + size);
    if (!grown) { if (size > old) release(m, size - old); return NULL; }
    if (size < old) release(m, old - size);
    grown->h.size = size;
    return grown + 1;
}
static const XML_Memory_Handling_Suite xml_suite = { xml_malloc, xml_realloc, xml_free };
/* Grow an array of element size to hold at least need elements. */
static bool grow(void **array, size_t *capacity, size_t need, size_t size) {
    if (need <= *capacity) return true;
    size_t next = *capacity ? *capacity : 16;
    while (next < need) { if (next > SIZE_MAX / 2) return false; next *= 2; }
    if (next > SIZE_MAX / size) return false;
    void *grown = xml_realloc(*array, next * size);
    if (!grown) return false;
    *array = grown; *capacity = next;
    return true;
}

/* -------------------------------------------------------------- parse state */
/* A namespace declaration in scope. prefix "" is the default namespace; uri
   NULL is no namespace (xmlns=""). Bindings shadow by a hash chain whose head
   is the innermost binding of that bucket, so lookups do not scan the scope. */
struct xml_binding { const char *prefix, *uri; size_t prefix_length; uint32_t hash; int next; };
/* An open element (HTML: "as if the element was in a stack of open
   elements"): element is the node its start tag made; children go to
   container, a template's contents for an HTML template. The bottom entry
   is the document, with no element. */
struct xml_open { node_t *element, *container; size_t bindings; };
struct xml_parser {
    web_doc *d, *checkpoint;
    XML_Parser expat, current;
    struct xml_memory memory;
    struct xml_open *open; size_t depth, open_capacity;
    struct xml_binding *bindings; size_t binding_count, binding_capacity;
    int *buckets; size_t bucket_count;
    char *text; size_t text_length, text_capacity;
    char *scratch; size_t scratch_capacity;
    size_t *order; size_t order_capacity;
    bool cdata, in_doctype;
    enum xml_parse_result result;
    struct xml_parse_error *error, own_error;
    /* What Expat reads, in its protocol encoding: the caller's bytes, or the
       parser's own copy or decoding of them (owned). */
    const char *feed; char *owned; size_t feed_length, fed;
    /* HTML "XML scripting support enabled": d is a browsing context's
       document, the parse runs a slice at a time, and a script's end tag
       stops Expat (resumable) with script naming the element. */
    bool scripting, suspended, finished;
    bool unpublished; /* nodes appended since the document was last told */
    bool changed;     /* the latest resume told the document of new nodes */
    bool details;     /* an HTML details element was appended since then */
    node_t *script;
};

static void fail(struct xml_parser *x, enum xml_parse_result result, const char *reason) {
    if (x->result != XML_PARSED) return;
    x->result = result;
    if (x->error && reason) {
        x->error->line = XML_GetCurrentLineNumber(x->current);
        x->error->column = XML_GetCurrentColumnNumber(x->current) + 1;
        snprintf(x->error->reason, sizeof x->error->reason, "%s", reason);
    }
    /* Outside Expat (an encoding refused before it starts) there is nothing
       to stop. */
    if (x->current) XML_StopParser(x->current, XML_FALSE);
}
static bool live(struct xml_parser *x) { return x->result == XML_PARSED; }
static bool checkpoint(struct xml_parser *x) {
    if (!live(x)) return false;
    if (!x->checkpoint || web_native_checkpoint(x->checkpoint)) return true;
    fail(x, XML_PARSE_CANCELLED, NULL);
    return false;
}

/* ---------------------------------------------------------------- namespaces */
static uint32_t prefix_hash(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 16777619u;
    return h;
}
static bool buckets_fit(struct xml_parser *x, size_t count) {
    if (count <= x->bucket_count) return true;
    size_t next = x->bucket_count ? x->bucket_count : 64;
    while (next < count) { if (next > SIZE_MAX / 2 / sizeof(int)) return false; next *= 2; }
    int *buckets = xml_realloc(x->buckets, next * sizeof *buckets);
    if (!buckets) return false;
    x->buckets = buckets; x->bucket_count = next;
    for (size_t i = 0; i < next; i++) buckets[i] = -1;
    /* Oldest first, so each chain's head is its innermost binding again. */
    for (size_t i = 0; i < x->binding_count; i++) {
        size_t at = x->bindings[i].hash & (next - 1);
        x->bindings[i].next = buckets[at]; buckets[at] = (int)i;
    }
    return true;
}
static const struct xml_binding *lookup(const struct xml_parser *x, const char *prefix, size_t length) {
    if (!x->bucket_count) return NULL;
    for (int i = x->buckets[prefix_hash(prefix, length) & (x->bucket_count - 1)]; i >= 0; i = x->bindings[i].next) {
        const struct xml_binding *b = &x->bindings[i];
        if (b->prefix_length == length && !memcmp(b->prefix, prefix, length)) return b;
    }
    return NULL;
}
static bool bind(struct xml_parser *x, const char *prefix, size_t length, const char *uri) {
    size_t uri_length = uri ? strlen(uri) : 0;
    if (x->binding_count >= INT_MAX || length > SIZE_MAX - uri_length - 2 ||
        !grow((void **)&x->bindings, &x->binding_capacity, x->binding_count + 1, sizeof *x->bindings) ||
        !buckets_fit(x, x->binding_count + 1)) return false;
    char *copy = xml_malloc(length + uri_length + 2);
    if (!copy) return false;
    memcpy(copy, prefix, length); copy[length] = 0;
    if (uri) { memcpy(copy + length + 1, uri, uri_length); copy[length + 1 + uri_length] = 0; }
    struct xml_binding *b = &x->bindings[x->binding_count];
    *b = (struct xml_binding){ .prefix = copy, .uri = uri ? copy + length + 1 : NULL,
                               .prefix_length = length, .hash = prefix_hash(prefix, length) };
    size_t at = b->hash & (x->bucket_count - 1);
    b->next = x->buckets[at]; x->buckets[at] = (int)x->binding_count++;
    return true;
}
/* LIFO: the newest binding is the head of its chain. */
static void unbind_to(struct xml_parser *x, size_t count) {
    while (x->binding_count > count) {
        struct xml_binding *b = &x->bindings[--x->binding_count];
        x->buckets[b->hash & (x->bucket_count - 1)] = b->next;
        xml_free((void *)b->prefix);
    }
}
/* Namespaces in XML 1.0, "Namespace constraints: Reserved Prefixes and
   Namespace Names" and "No Prefix Undeclaring". */
static bool declare(struct xml_parser *x, const char *prefix, size_t length, const char *uri) {
    bool xml_uri = !strcmp(uri, WEB_NS_XML), xmlns_uri = !strcmp(uri, WEB_NS_XMLNS);
    const char *reason = NULL;
    if (length) {
        bool xml_prefix = length == 3 && !memcmp(prefix, "xml", 3);
        if (length == 5 && !memcmp(prefix, "xmlns", 5)) reason = "接頭辞 xmlns は宣言できない";
        else if (xml_prefix != xml_uri) reason = "接頭辞 xml と XML の名前空間は互いにだけ結び付く";
        else if (xmlns_uri) reason = "xmlns の名前空間は宣言できない";
        else if (!*uri) reason = "接頭辞の宣言は空にできない";
    } else if (xml_uri || xmlns_uri) reason = "既定の名前空間を XML や xmlns の名前空間にできない";
    if (reason) { fail(x, XML_PARSE_MALFORMED, reason); return false; }
    if (!bind(x, prefix, length, *uri ? uri : NULL)) { fail(x, XML_PARSE_OOM, NULL); return false; }
    return true;
}
/* Namespaces in XML "QName": Expat has checked the XML Name, so a QName has at
   most one colon, with a non-empty prefix and an NCName after it. */
static bool qname(const char *name, size_t *prefix_length) {
    const char *colon = strchr(name, ':');
    *prefix_length = 0;
    if (!colon) return true;
    if (colon == name || strchr(colon + 1, ':') || !doc_pi_target_valid(colon + 1, strlen(colon + 1))) return false;
    *prefix_length = (size_t)(colon - name);
    return true;
}
/* The namespace of a prefixed name; an unprefixed attribute has none. */
static bool resolve(struct xml_parser *x, const char *name, size_t length, const char **ns) {
    if (length == 3 && !memcmp(name, "xml", 3)) { *ns = WEB_NS_XML; return true; }
    const struct xml_binding *b = length == 5 && !memcmp(name, "xmlns", 5) ? NULL : lookup(x, name, length);
    if (!b) { fail(x, XML_PARSE_MALFORMED, "宣言されていない名前空間の接頭辞"); return false; }
    *ns = b->uri;
    return true;
}

/* ---------------------------------------------------------------- the tree */
static struct xml_open *top(struct xml_parser *x) { return &x->open[x->depth - 1]; }
/* Appended last. A document's scripts can change an open element's children
   between resumes, so the index follows the element siblings before it, not
   a count kept by the parser; the walk back stops at the nearest element. */
static void append(struct xml_parser *x, node_t *child) {
    node_t *parent = top(x)->container;
    child->parent = parent; child->prev = parent->last; child->next = NULL;
    if (parent->last) parent->last->next = child; else parent->first = child;
    parent->last = child;
    if (child->type == N_ELEM) {
        child->elem_index = 1;
        for (node_t *c = child->prev; c; c = c->prev)
            if (c->type == N_ELEM) { child->elem_index = c->elem_index + 1; break; }
    }
    x->unpublished = true;
}
static bool text_append(struct xml_parser *x, const char *s, size_t n) {
    if (n > SIZE_MAX - 1 - x->text_length) return false;
    if (!grow((void **)&x->text, &x->text_capacity, x->text_length + n + 1, 1)) return false;
    memcpy(x->text + x->text_length, s, n);
    x->text_length += n;
    return true;
}
/* DOM "create a text node" (or a CDATA section node) of the pending text. */
static void text_flush(struct xml_parser *x, bool cdata) {
    if (!live(x) || (!x->text_length && !cdata)) return;
    node_t *container = top(x)->container;
    node_t *t = doc_node_create(container->owner, N_TEXT, NULL, x->text ? x->text : "", x->text_length);
    x->text_length = 0;
    if (!t) { fail(x, XML_PARSE_OOM, NULL); return; }
    t->cdata = cdata;
    append(x, t);
    /* HTML textarea: its default value is its child text, which can follow
       a publication of the element itself. */
    if (container->type == N_ELEM && container->namespace_id == NS_HTML && container->tag == T_textarea &&
        !container->value_dirty) container->control_ready = false;
}
/* NUL-terminated copy of a name's prefix in the parse's scratch buffer. */
static const char *scratch_prefix(struct xml_parser *x, const char *name, size_t length) {
    if (!length) return NULL;
    if (length == SIZE_MAX || !grow((void **)&x->scratch, &x->scratch_capacity, length + 1, 1)) return NULL;
    memcpy(x->scratch, name, length); x->scratch[length] = 0;
    return x->scratch;
}
static int expanded_order(const void *a, const void *b, void *context) {
    const struct attr *attrs = context;
    const struct attr *l = &attrs[*(const size_t *)a], *r = &attrs[*(const size_t *)b];
    int c = strcmp(l->local, r->local);
    return c ? c : strcmp(l->namespace_uri, r->namespace_uri);
}
/* Namespaces in XML "Attributes Unique": no two attributes with a namespace
   share an expanded name. Expat already rejected equal qualified names. */
static bool attributes_unique(struct xml_parser *x, const struct attr *attrs, int count) {
    size_t n = 0;
    for (int i = 0; i < count; i++) if (attrs[i].namespace_uri && attrs[i].prefix) n++;
    if (n < 2) return true;
    if (!grow((void **)&x->order, &x->order_capacity, n, sizeof *x->order)) { fail(x, XML_PARSE_OOM, NULL); return false; }
    n = 0;
    for (int i = 0; i < count; i++) if (attrs[i].namespace_uri && attrs[i].prefix) x->order[n++] = (size_t)i;
    qsort_s(x->order, n, sizeof *x->order, expanded_order, (void *)attrs);
    for (size_t i = 1; i < n; i++)
        if (!expanded_order(&x->order[i - 1], &x->order[i], (void *)attrs)) {
            fail(x, XML_PARSE_MALFORMED, "同じ名前空間と局所名の属性が二つある");
            return false;
        }
    return true;
}

/* HTML elements whose insertion into a browsing context's document acts at
   once, as when the HTML parser inserts them: a meta's Content Security
   Policy pragma (a child of head) and referrer metadata apply before any
   later script runs; details name groups are settled at the publication. */
static void inserted_html(struct xml_parser *x, node_t *element) {
    if (element->tag == T_details) { x->details = true; return; }
    if (element->tag != T_meta) return;
    element->html_policy_processed = true;
    node_t *parent = element->parent;
    const char *equiv = node_attr(element, "http-equiv"), *content = node_attr(element, "content");
    if (parent && parent->type == N_ELEM && parent->namespace_id == NS_HTML && parent->tag == T_head &&
        equiv && str_ieq(equiv, "content-security-policy") && content && *content &&
        !html_policy_meta(x->d, content, strlen(content))) { fail(x, XML_PARSE_OOM, NULL); return; }
    doc_referrer_meta(x->d, element);
}
/* HTML "create an element for the token" with the token's prefix, then its
   attributes in source order (Expat appends the DTD's defaulted ones). */
static void XMLCALL start_element(void *data, const XML_Char *name, const XML_Char **atts) {
    struct xml_parser *x = data;
    if (!checkpoint(x)) return;
    text_flush(x, false);
    if (!live(x)) return;
    size_t mark = x->binding_count;
    int count = 0;
    while (atts[2 * (size_t)count]) {
        const char *attribute = atts[2 * (size_t)count], *value = atts[2 * (size_t)count + 1];
        if (!strcmp(attribute, "xmlns")) { if (!declare(x, "", 0, value)) return; }
        else if (!strncmp(attribute, "xmlns:", 6)) {
            const char *declared = attribute + 6;
            if (!*declared || strchr(declared, ':') || !doc_pi_target_valid(declared, strlen(declared))) {
                fail(x, XML_PARSE_MALFORMED, "名前空間の宣言の接頭辞が NCName ではない"); return;
            }
            if (!declare(x, declared, strlen(declared), value)) return;
        }
        if (count == INT_MAX) { fail(x, XML_PARSE_OOM, NULL); return; }
        count++;
    }
    size_t prefix_length;
    if (!qname(name, &prefix_length)) { fail(x, XML_PARSE_MALFORMED, "要素の名前が QName ではない"); return; }
    const char *ns = NULL;
    if (prefix_length == 5 && !memcmp(name, "xmlns", 5)) { fail(x, XML_PARSE_MALFORMED, "要素の名前に接頭辞 xmlns は使えない"); return; }
    if (prefix_length) { if (!resolve(x, name, prefix_length, &ns)) return; }
    else { const struct xml_binding *b = lookup(x, "", 0); ns = b ? b->uri : NULL; }
    node_t *container = top(x)->container;
    web_doc *owner = container->owner;
    const char *prefix = scratch_prefix(x, name, prefix_length);
    if (prefix_length && !prefix) { fail(x, XML_PARSE_OOM, NULL); return; }
    node_t *element = doc_element_create(owner, ns, prefix, prefix_length ? name + prefix_length + 1 : name);
    if (!element) { fail(x, XML_PARSE_OOM, NULL); return; }
    jmp_buf trap; jmp_buf *old = owner->mem.trap;
    owner->mem.trap = &trap;
    if (setjmp(trap)) { owner->mem.trap = old; fail(x, XML_PARSE_OOM, NULL); return; }
    struct attr *attrs = count ? ar_alloc(&owner->mem, (size_t)count * sizeof *attrs) : NULL;
    for (int i = 0; i < count; i++) {
        const char *attribute = atts[2 * (size_t)i], *value = atts[2 * (size_t)i + 1], *attribute_ns = NULL;
        size_t length = 0;
        if (!strcmp(attribute, "xmlns")) attribute_ns = WEB_NS_XMLNS;
        else if (!strncmp(attribute, "xmlns:", 6)) { attribute_ns = WEB_NS_XMLNS; length = 5; }
        else if (!qname(attribute, &length)) { owner->mem.trap = old; fail(x, XML_PARSE_MALFORMED, "属性の名前が QName ではない"); return; }
        else if (length && !resolve(x, attribute, length, &attribute_ns)) { owner->mem.trap = old; return; }
        struct attr *a = &attrs[i];
        char *raw = ar_strdup(&owner->mem, attribute), *low = ar_strdup(&owner->mem, attribute);
        for (char *p = low; *p; p++) *p = (char)lower((unsigned char)*p);
        a->raw = raw; a->name = low; a->value = ar_strdup(&owner->mem, value);
        a->namespace_uri = attribute_ns ? ar_strdup(&owner->mem, attribute_ns) : NULL;
        a->prefix = length ? ar_strndup(&owner->mem, attribute, length) : NULL;
        a->local = length ? raw + length + 1 : raw;
    }
    if (!attributes_unique(x, attrs, count)) { owner->mem.trap = old; return; }
    doc_attrs_initial(owner, element, attrs, count);
    if (element->namespace_id == NS_HTML)
        for (int i = 0; i < count; i++) if (!attrs[i].namespace_uri && !strcmp(attrs[i].raw, "is")) {
            element->custom_is = ar_strdup(&owner->mem, attrs[i].value); break;
        }
    owner->mem.trap = old;
    if (owner == x->d) doc_parser_element_created(element);
    element->custom_registry_id = owner->custom_registry_id;
    element->custom_registry_realm = owner->custom_registry_realm;
    /* HTML: a script the parser creates with XML scripting support enabled
       waits for its end tag to be prepared. With it disabled, and in template
       contents (an inert document), it is already started. */
    if (element->tag == T_script) element->script_started = !x->scripting || owner != x->d;
    node_t *children = element;
    if (element->namespace_id == NS_HTML && element->tag == T_template) {
        children = doc_template_content(owner, element);
        if (!children) { fail(x, XML_PARSE_OOM, NULL); return; }
    }
    append(x, element);
    if (!grow((void **)&x->open, &x->open_capacity, x->depth + 1, sizeof *x->open)) { fail(x, XML_PARSE_OOM, NULL); return; }
    x->open[x->depth++] = (struct xml_open){ .element = element, .container = children, .bindings = mark };
    if (x->scripting && owner == x->d && element->namespace_id == NS_HTML) inserted_html(x, element);
}
/* A script's end tag with XML scripting support enabled: HTML's microtask
   checkpoint, "prepare the script element" and the wait for a
   parsing-blocking script belong to the document's parser loop (js.c), as
   for the HTML parser's script results, so Expat stops here, resumably, and
   the resume that reached this tag returns the script. A script that has
   left the document meanwhile is never prepared by this parser. */
static void XMLCALL end_element(void *data, const XML_Char *name) {
    (void)name;
    struct xml_parser *x = data;
    text_flush(x, false);
    if (!live(x) || x->depth < 2) return;
    struct xml_open *at = top(x);
    node_t *element = at->element;
    unbind_to(x, at->bindings);
    x->depth--;
    if (!x->scripting || element->tag != T_script || element->script_started ||
        (element->namespace_id != NS_HTML && element->namespace_id != NS_SVG)) return;
    if (element->owner != x->d) { element->script_started = true; return; }
    x->script = element;
    if (XML_StopParser(x->expat, XML_TRUE) != XML_STATUS_OK) {
        /* Not stoppable here (Expat refuses only inside a parameter entity,
           which holds no content): the script cannot be prepared in order. */
        x->script = NULL; element->script_started = true;
    }
}
static void XMLCALL characters(void *data, const XML_Char *s, int length) {
    struct xml_parser *x = data;
    if (!checkpoint(x)) return;
    if (length > 0 && !text_append(x, s, (size_t)length)) fail(x, XML_PARSE_OOM, NULL);
}
static void XMLCALL cdata_start(void *data) {
    struct xml_parser *x = data;
    text_flush(x, false);
    x->cdata = true;
}
static void XMLCALL cdata_end(void *data) {
    struct xml_parser *x = data;
    text_flush(x, true);
    x->cdata = false;
}
static void XMLCALL comment(void *data, const XML_Char *text) {
    struct xml_parser *x = data;
    if (!checkpoint(x) || x->in_doctype) return; /* the internal subset is not the tree */
    text_flush(x, false);
    if (!live(x)) return;
    node_t *c = doc_node_create(top(x)->container->owner, N_COMMENT, NULL, text, strlen(text));
    if (!c) { fail(x, XML_PARSE_OOM, NULL); return; }
    append(x, c);
}
static void XMLCALL instruction(void *data, const XML_Char *target, const XML_Char *text) {
    struct xml_parser *x = data;
    if (!checkpoint(x)) return;
    if (strchr(target, ':')) { fail(x, XML_PARSE_MALFORMED, "処理命令の対象の名前にコロンがある"); return; }
    if (x->in_doctype) return;
    text_flush(x, false);
    if (!live(x)) return;
    node_t *pi = doc_node_create(top(x)->container->owner, N_PI, target, text, strlen(text));
    if (!pi) { fail(x, XML_PARSE_OOM, NULL); return; }
    append(x, pi);
}
static void XMLCALL doctype_start(void *data, const XML_Char *name, const XML_Char *system_id,
                                  const XML_Char *public_id, int internal_subset) {
    (void)internal_subset;
    struct xml_parser *x = data;
    x->in_doctype = true;
    size_t prefix_length;
    if (!live(x)) return;
    if (!qname(name, &prefix_length)) { fail(x, XML_PARSE_MALFORMED, "文書型の名前が QName ではない"); return; }
    const char *pub = public_id ? public_id : "", *sys = system_id ? system_id : "";
    node_t *doctype = doc_doctype_create(x->d, name, strlen(name), pub, strlen(pub), sys, strlen(sys));
    if (!doctype) { fail(x, XML_PARSE_OOM, NULL); return; }
    append(x, doctype);
}
static void XMLCALL doctype_end(void *data) { ((struct xml_parser *)data)->in_doctype = false; }
/* Namespaces in XML: no entity name or notation name contains a colon. */
static void XMLCALL entity_declaration(void *data, const XML_Char *name, int parameter, const XML_Char *value,
                                       int value_length, const XML_Char *base, const XML_Char *system_id,
                                       const XML_Char *public_id, const XML_Char *notation) {
    (void)parameter; (void)value; (void)value_length; (void)base; (void)system_id; (void)public_id; (void)notation;
    struct xml_parser *x = data;
    if (live(x) && strchr(name, ':')) fail(x, XML_PARSE_MALFORMED, "実体の名前にコロンがある");
}
static void XMLCALL notation_declaration(void *data, const XML_Char *name, const XML_Char *base,
                                         const XML_Char *system_id, const XML_Char *public_id) {
    (void)base; (void)system_id; (void)public_id;
    struct xml_parser *x = data;
    if (live(x) && strchr(name, ':')) fail(x, XML_PARSE_MALFORMED, "記法の名前にコロンがある");
}
/* A reference to an entity in a DTD that was not read: XML 1.0 "Entity
   Declared" is then not a well-formedness constraint, and nothing is inserted. */
static void XMLCALL skipped_entity(void *data, const XML_Char *name, int parameter) {
    (void)data; (void)name; (void)parameter;
}
/* HTML: retrieve the named character reference DTD for the listed public
   identifiers and no other external entity's content. */
static const char *const xhtml_public_ids[] = {
    "-//W3C//DTD XHTML 1.0 Transitional//EN", "-//W3C//DTD XHTML 1.1//EN",
    "-//W3C//DTD XHTML 1.0 Strict//EN", "-//W3C//DTD XHTML 1.0 Frameset//EN",
    "-//W3C//DTD XHTML Basic 1.0//EN", "-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN",
    "-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN", "-//W3C//DTD MathML 2.0//EN",
    "-//WAPFORUM//DTD XHTML Mobile 1.0//EN", "-//WAPFORUM//DTD XHTML Mobile 1.1//EN",
    "-//WAPFORUM//DTD XHTML Mobile 1.2//EN",
};
static int XMLCALL external_entity(XML_Parser parser, const XML_Char *context, const XML_Char *base,
                                   const XML_Char *system_id, const XML_Char *public_id) {
    (void)base; (void)system_id;
    struct xml_parser *x = XML_GetUserData(parser);
    bool listed = false;
    for (size_t i = 0; public_id && i < sizeof xhtml_public_ids / sizeof *xhtml_public_ids; i++)
        if (!strcmp(public_id, xhtml_public_ids[i])) { listed = true; break; }
    if (!listed || !live(x)) return XML_STATUS_OK;
    XML_Parser dtd = XML_ExternalEntityParserCreate(parser, context, "UTF-8");
    if (!dtd) { fail(x, XML_PARSE_OOM, NULL); return XML_STATUS_ERROR; }
    XML_Parser outer = x->current;
    x->current = dtd;
    enum XML_Status status = XML_Parse(dtd, xml_xhtml_entities_dtd, (int)(sizeof xml_xhtml_entities_dtd - 1), XML_TRUE);
    if (status != XML_STATUS_OK && live(x))
        fail(x, XML_GetErrorCode(dtd) == XML_ERROR_NO_MEMORY ? XML_PARSE_OOM : XML_PARSE_MALFORMED, "XHTML の実体の DTD を読めない");
    x->current = outer;
    XML_ParserFree(dtd);
    return status == XML_STATUS_OK ? XML_STATUS_OK : XML_STATUS_ERROR;
}

/* ------------------------------------------------------------------ encoding */
/* The encoding of transport bytes: a BOM, the transport label, the XML
   declaration (XML 1.0 Appendix F), else UTF-8. False: an unsupported label. */
static bool xml_encoding(const char *s, size_t n, const char *transport, char *name, size_t capacity, size_t *bom) {
    const unsigned char *u = (const unsigned char *)s;
    *bom = 0;
    if (n >= 3 && u[0] == 0xef && u[1] == 0xbb && u[2] == 0xbf) { *bom = 3; snprintf(name, capacity, "UTF-8"); return true; }
    if (n >= 2 && u[0] == 0xfe && u[1] == 0xff) { *bom = 2; snprintf(name, capacity, "UTF-16BE"); return true; }
    if (n >= 2 && u[0] == 0xff && u[1] == 0xfe) { *bom = 2; snprintf(name, capacity, "UTF-16LE"); return true; }
    if (transport && *transport && html_encoding_label(transport, strlen(transport), name, capacity)) return true;
    if (n >= 4 && u[0] == '<' && !u[1] && u[2] == '?' && !u[3]) { snprintf(name, capacity, "UTF-16LE"); return true; }
    if (n >= 4 && !u[0] && u[1] == '<' && !u[2] && u[3] == '?') { snprintf(name, capacity, "UTF-16BE"); return true; }
    snprintf(name, capacity, "UTF-8");
    if (n < 6 || memcmp(s, "<?xml", 5) || !strchr(" \t\r\n", s[5]) || !s[5]) return true;
    size_t end = n < 1024 ? n : 1024, close = 5;
    while (close + 1 < end && !(s[close] == '?' && s[close + 1] == '>')) close++;
    for (size_t i = 5; i + 8 <= close; i++) {
        if (memcmp(s + i, "encoding", 8)) continue;
        size_t at = i + 8;
        while (at < close && strchr(" \t\r\n", s[at]) && s[at]) at++;
        if (at >= close || s[at] != '=') return true;
        at++;
        while (at < close && strchr(" \t\r\n", s[at]) && s[at]) at++;
        if (at >= close || (s[at] != '"' && s[at] != '\'')) return true;
        char quote = s[at++];
        size_t start = at;
        while (at < close && s[at] != quote) at++;
        if (at >= close) return true;
        if (!html_encoding_label(s + start, at - start, name, capacity)) return false;
        /* A declaration read as ASCII cannot be UTF-16: the bytes are not. */
        if (!strcmp(name, "UTF-16LE") || !strcmp(name, "UTF-16BE")) snprintf(name, capacity, "UTF-8");
        return true;
    }
    return true;
}

/* -------------------------------------------------------------------- parse */
static struct xml_memory *enter(struct xml_parser *x) {
    struct xml_memory *outer = xml_memory_current;
    xml_memory_current = &x->memory;
    return outer;
}
static void close_parser(struct xml_parser *x) {
    if (x->bindings) unbind_to(x, 0);
    xml_free(x->bindings); xml_free(x->buckets); xml_free(x->open);
    xml_free(x->text); xml_free(x->scratch); xml_free(x->order);
    if (x->expat) XML_ParserFree(x->expat);
    free(x->owned);
    x->bindings = NULL; x->buckets = NULL; x->open = NULL; x->order = NULL;
    x->text = x->scratch = x->owned = NULL;
    x->expat = x->current = NULL;
}
/* The input's encoding, then Expat over it with the document at the bottom
   of the open elements. copy: the input does not outlive this call (a
   browsing context's response), so the parser reads its own copy. */
static bool open_parser(struct xml_parser *x, const char *input, size_t length, bool string,
                        const char *transport_label, bool copy) {
    web_doc *d = x->d;
    const char *protocol = "UTF-8";
    x->feed = input ? input : ""; x->feed_length = length;
    if (string) snprintf(d->encoding, sizeof d->encoding, "UTF-8");
    else {
        char name[32]; size_t bom = 0;
        if (!xml_encoding(x->feed, length, transport_label, name, sizeof name, &bom)) {
            x->result = XML_PARSE_MALFORMED; x->error->line = x->error->column = 1;
            snprintf(x->error->reason, sizeof x->error->reason, "対応しない文字の符号化");
            return false;
        }
        if (!strcmp(name, "UTF-8") || !strcmp(name, "UTF-16LE") || !strcmp(name, "UTF-16BE")) {
            /* Expat decodes these itself, and a malformed sequence is fatal. */
            protocol = !strcmp(name, "UTF-8") ? "UTF-8" : !strcmp(name, "UTF-16LE") ? "UTF-16LE" : "UTF-16BE";
            x->feed += bom; x->feed_length -= bom;
            snprintf(d->encoding, sizeof d->encoding, "%s", name);
        } else {
            x->owned = html_decode_bytes(d, x->feed, length, name, &x->feed_length);
            if (!x->owned) { x->result = XML_PARSE_OOM; return false; }
            x->feed = x->owned;
        }
        d->encoding_certain = true;
    }
    if (copy && !x->owned) {
        if (x->feed_length == SIZE_MAX || !(x->owned = malloc(x->feed_length + 1))) { x->result = XML_PARSE_OOM; return false; }
        memcpy(x->owned, x->feed, x->feed_length); x->owned[x->feed_length] = 0;
        x->feed = x->owned;
    }
    x->expat = x->current = XML_ParserCreate_MM(protocol, &xml_suite, NULL);
    if (!x->expat || !grow((void **)&x->open, &x->open_capacity, 1, sizeof *x->open)) { x->result = XML_PARSE_OOM; return false; }
    x->open[x->depth++] = (struct xml_open){ .container = d->root };
    XML_SetUserData(x->expat, x);
    XML_SetElementHandler(x->expat, start_element, end_element);
    XML_SetCharacterDataHandler(x->expat, characters);
    XML_SetCdataSectionHandler(x->expat, cdata_start, cdata_end);
    XML_SetCommentHandler(x->expat, comment);
    XML_SetProcessingInstructionHandler(x->expat, instruction);
    XML_SetDoctypeDeclHandler(x->expat, doctype_start, doctype_end);
    XML_SetEntityDeclHandler(x->expat, entity_declaration);
    XML_SetNotationDeclHandler(x->expat, notation_declaration);
    XML_SetSkippedEntityHandler(x->expat, skipped_entity);
    XML_SetExternalEntityRefHandler(x->expat, external_entity);
    XML_SetParamEntityParsing(x->expat, XML_PARAM_ENTITY_PARSING_UNLESS_STANDALONE);
    /* No amplification cap: the accounting above and the checkpoint answer it. */
    XML_SetBillionLaughsAttackProtectionActivationThreshold(x->expat, ULLONG_MAX);
    XML_SetAllocTrackerActivationThreshold(x->expat, ULLONG_MAX);
    return true;
}
static void expat_failed(struct xml_parser *x) {
    if (!live(x)) return; /* fail() stopped Expat and said why */
    enum XML_Error code = XML_GetErrorCode(x->expat);
    x->result = code == XML_ERROR_NO_MEMORY ? XML_PARSE_OOM : XML_PARSE_MALFORMED;
    x->error->line = XML_GetCurrentLineNumber(x->expat);
    x->error->column = XML_GetCurrentColumnNumber(x->expat) + 1;
    snprintf(x->error->reason, sizeof x->error->reason, "%s", XML_ErrorString(code));
}
/* Run Expat in parts of slice bytes until it stops at a script's end tag,
   reaches the end, fails, or has read bytes more input or passed deadline
   (uptime_ms; 0: none). A stopped Expat resumes in the part it stopped in. */
static void run(struct xml_parser *x, size_t slice, size_t bytes, uint64_t deadline) {
    size_t start = x->fed;
    if (x->suspended) {
        x->suspended = false;
        enum XML_Status status = XML_ResumeParser(x->expat);
        if (status == XML_STATUS_SUSPENDED) { x->suspended = true; return; }
        if (status != XML_STATUS_OK) { expat_failed(x); return; }
        XML_ParsingStatus parsing;
        XML_GetParsingStatus(x->expat, &parsing);
        if (parsing.parsing == XML_FINISHED) { x->finished = true; return; }
        if (!checkpoint(x)) return;
    }
    while (live(x)) {
        size_t part = x->feed_length - x->fed < slice ? x->feed_length - x->fed : slice;
        bool final = x->fed + part == x->feed_length;
        const char *at = x->feed + x->fed;
        x->fed += part;
        enum XML_Status status = XML_Parse(x->expat, at, (int)part, final);
        if (status == XML_STATUS_SUSPENDED) { x->suspended = true; return; }
        if (status != XML_STATUS_OK) { expat_failed(x); return; }
        if (final) { x->finished = true; return; }
        if (!checkpoint(x)) return;
        if (x->fed - start >= bytes || (deadline && uptime_ms() >= deadline)) return;
    }
}
enum xml_parse_result xml_parse_document(web_doc *d, web_doc *checkpoint_doc, const char *input, size_t length,
                                         bool string, const char *transport_label, struct xml_parse_error *error) {
    struct xml_parse_error unused;
    if (!error) error = &unused;
    memset(error, 0, sizeof *error);
    if (!d || !d->root || d->root->first || (length && !input)) return XML_PARSE_OOM;
    struct xml_parser x = { .d = d, .checkpoint = checkpoint_doc, .memory = { .d = d }, .error = error, .result = XML_PARSED };
    struct xml_memory *outer = enter(&x);
    /* Scripting disabled: nothing stops Expat before the end. */
    if (open_parser(&x, input, length, string, transport_label, false)) run(&x, XML_FEED_BYTES, SIZE_MAX, 0);
    close_parser(&x);
    xml_memory_current = outer;
    if (x.result != XML_PARSED) {
        /* DOMParser asserts the document has no children before parsererror. */
        d->root->first = d->root->last = NULL;
        return x.result;
    }
    doc_mutated(d, NULL);
    if (d->template_doc) doc_mutated(d->template_doc, NULL);
    doc_sync_tree(d);
    return XML_PARSED;
}

/* ------------------------------------------------------- a browsing context */
/* The nodes appended since the previous resume reach the document's
   bookkeeping (mutation, the html/head/body/title snapshot, details groups)
   at one coherent boundary, before any script the resume returns runs. */
static void publish(struct xml_parser *x) {
    if (!x->unpublished) return;
    x->unpublished = false; x->changed = true;
    if (x->details) { x->details = false; doc_details_parser_finish(x->d->root); }
    doc_mutated(x->d, NULL);
    if (x->d->template_doc) doc_mutated(x->d->template_doc, NULL);
    doc_sync_tree(x->d);
}
/* A well-formedness error. HTML ("Loading XML documents") lets the error be
   reported inline by mutating the Document, and no WPT test fixes how.
   Gecko replaces the document's children with its parsererror element;
   Blink and WebKit keep the tree built so far under an error banner of
   their own. This parser does as Gecko: the error is the parsererror
   element DOMParser also gives (xml_error_document), so the document
   describes itself the same way to its scripts, to DOMParser's callers and
   to its reader, and markup the author did not finish is never shown as
   if it were the document. Scripts that already ran keep the nodes they
   hold, now outside the document. */
static bool report_error(struct xml_parser *x) {
    web_doc *d = x->d;
    char message[256];
    snprintf(message, sizeof message, "XML の解析の誤り（%lu 行 %lu 列）：%s",
             x->error->line, x->error->column, x->error->reason);
    web_js_console(d, 2, message);
    publish(x);
    while (d->root->first) doc_node_remove(d, d->root->first);
    x->changed = true;
    return xml_error_document(d, x->error);
}
struct xml_parser *xml_parser_begin(web_doc *d, const char *input, size_t length, const char *transport_label) {
    if (!d || d->root || (length && !input)) return NULL;
    d->xml = true;
    if (!(d->root = doc_node_create(d, N_DOC, NULL, "", 0))) return NULL;
    struct xml_parser *x = malloc(sizeof *x);
    if (!x) return NULL;
    *x = (struct xml_parser){ .d = d, .checkpoint = d, .memory = { .d = d }, .result = XML_PARSED, .scripting = true };
    x->error = &x->own_error;
    struct xml_memory *outer = enter(x);
    bool opened = open_parser(x, input, length, false, transport_label, true);
    xml_memory_current = outer;
    /* An encoding Expat cannot read is the document's error, which the
       first resume reports like any other. */
    if (!opened && x->result != XML_PARSE_MALFORMED) { xml_parser_finish(x); return NULL; }
    return x;
}
int xml_parser_resume(struct xml_parser *x, node_t **script) {
    if (script) *script = NULL;
    if (!x) return HTML_PARSE_FAILED;
    x->changed = false;
    if (x->finished) return x->result == XML_PARSE_CANCELLED ? HTML_PARSE_CANCELLED :
                            x->result == XML_PARSE_OOM ? HTML_PARSE_FAILED : HTML_PARSE_DONE;
    if (live(x)) {
        struct xml_memory *outer = enter(x);
        if (checkpoint(x)) run(x, XML_SLICE_BYTES, XML_RESUME_BYTES, uptime_ms() + XML_RESUME_MS);
        xml_memory_current = outer;
    }
    switch (x->result) {
    case XML_PARSE_CANCELLED:
        /* No script, and no more of the tree: what is in it stays, known to
           the document's mutation bookkeeping only. */
        x->finished = true;
        if (x->unpublished) { x->unpublished = false; x->changed = true; doc_mutated(x->d, NULL); }
        return HTML_PARSE_CANCELLED;
    case XML_PARSE_OOM:
        /* As the HTML parser at the memory limit: the tree so far stays. */
        x->finished = true;
        publish(x);
        return HTML_PARSE_FAILED;
    case XML_PARSE_MALFORMED:
        x->finished = true;
        if (!report_error(x)) { x->result = XML_PARSE_OOM; return HTML_PARSE_FAILED; }
        return HTML_PARSE_DONE;
    case XML_PARSED:
        break;
    }
    publish(x);
    if (x->script) {
        node_t *element = x->script;
        x->script = NULL;
        /* The parser's text is the script's trusted script text. */
        if (!web_js_script_parser_capture(x->d, element)) {
            x->result = XML_PARSE_OOM; x->finished = true;
            return HTML_PARSE_FAILED;
        }
        if (script) *script = element;
        return HTML_PARSE_SCRIPT;
    }
    return x->finished ? HTML_PARSE_DONE : HTML_PARSE_YIELD;
}
bool xml_parser_changed(const struct xml_parser *x) { return x && x->changed; }
void xml_parser_finish(struct xml_parser *x) {
    if (!x) return;
    close_parser(x);
    free(x);
}

/* DOMParser "parseFromString": a parsererror element in the Mozilla
   parsererror namespace, optionally describing the error. */
bool xml_error_document(web_doc *d, const struct xml_parse_error *error) {
    if (!d || !d->root || d->root->first) return false;
    node_t *root = doc_element_create(d, XML_PARSERERROR_NS, NULL, "parsererror");
    if (!root) return false;
    char message[256];
    snprintf(message, sizeof message, "XML の解析の誤り（%lu 行 %lu 列）：%s",
             error ? error->line : 0, error ? error->column : 0, error && error->reason[0] ? error->reason : "");
    node_t *text = doc_node_create(d, N_TEXT, NULL, message, strlen(message));
    if (!text) return false;
    text->parent = root; root->first = root->last = text;
    root->parent = d->root; d->root->first = d->root->last = root; root->elem_index = 1;
    doc_mutated(d, NULL);
    doc_sync_tree(d);
    return true;
}

web_doc *xml_document_new(web_doc *family, const char *url, const char *content_type, bool xml_interface) {
    web_doc *d = doc_inert_new(family, url);
    if (!d) return NULL;
    d->xml = true; d->xml_interface = xml_interface;
    snprintf(d->encoding, sizeof d->encoding, "UTF-8");
    d->content_type = strdup(content_type && *content_type ? content_type : "application/xml");
    d->root = d->content_type ? doc_node_create(d, N_DOC, NULL, "", 0) : NULL;
    if (!d->root) { doc_inert_discard(d); return NULL; }
    d->resources_dirty = true;
    return d;
}
web_doc *xml_document_parse(web_doc *family, web_doc *checkpoint_doc, const char *input, size_t length, bool string,
                            const char *transport_label, const char *url, const char *content_type,
                            bool error_document, enum xml_parse_result *result) {
    enum xml_parse_result unused;
    if (!result) result = &unused;
    web_doc *d = xml_document_new(family, url, content_type, false);
    if (!d) { *result = XML_PARSE_OOM; return NULL; }
    struct xml_parse_error error;
    *result = xml_parse_document(d, checkpoint_doc, input, length, string, transport_label, &error);
    if (*result == XML_PARSED) return d;
    if (*result == XML_PARSE_MALFORMED && error_document) {
        if (xml_error_document(d, &error)) return d;
        *result = XML_PARSE_OOM;
    }
    doc_inert_discard(d);
    return NULL;
}
