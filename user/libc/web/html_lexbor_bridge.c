/* Lexbor's tree-builder state is private to an active parse. The observable
   DOM remains Nocturne's node_t tree. Synchronize at script boundaries rather
   than relying on mutation callbacks: foster parenting/adoption/text merging
   in Lexbor deliberately bypass some of those callbacks. */
#include "html_lexbor.h"
#include "elements.h"
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/processing_instruction.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/tree/active_formatting.h>

struct binding {
    lxb_dom_node_t *lex;
    node_t *native;
};
struct html_bridge {
    struct binding **items, **table;
    size_t length, capacity, table_size;
};

static size_t ptr_hash(const void *p) {
    uintptr_t v = (uintptr_t)p >> 3;
    v ^= v >> 23;
    v *= (uintptr_t)0x2127599bf4325c37ULL;
    return (size_t)(v ^ (v >> 47));
}

static struct binding *by_native(struct html_bridge *b, node_t *n) {
    if (!n || !b->table_size) return NULL;
    size_t i = ptr_hash(n) & (b->table_size - 1);
    while (b->table[i]) {
        if (b->table[i]->native == n) return b->table[i];
        i = (i + 1) & (b->table_size - 1);
    }
    return NULL;
}

static struct binding *by_lex(lxb_dom_node_t *n) {
    if (!n || !n->user) return NULL;
    struct binding *b = n->user;
    /* Lexbor clones copy node.user. A clone must get a NEW native identity. */
    return b->lex == n ? b : NULL;
}

static struct binding *bind_node(struct html_parser *p, lxb_dom_node_t *lex, node_t *native) {
    struct html_bridge *b = p->bridge;
    if ((b->length + 1) * 2 >= b->table_size) {
        size_t size = b->table_size ? b->table_size * 2 : 128;
        struct binding **table = calloc(size, sizeof *table);
        if (!table) return NULL;
        for (size_t j = 0; j < b->length; j++) {
            struct binding *v = b->items[j];
            size_t i = ptr_hash(v->native) & (size - 1);
            while (table[i]) i = (i + 1) & (size - 1);
            table[i] = v;
        }
        free(b->table); b->table = table; b->table_size = size;
    }
    if (b->length == b->capacity) {
        size_t size = b->capacity ? b->capacity * 2 : 64;
        struct binding **items = realloc(b->items, size * sizeof *items);
        if (!items) return NULL;
        b->items = items; b->capacity = size;
    }
    struct binding *v = malloc(sizeof *v);
    if (!v) return NULL;
    v->lex = lex; v->native = native;
    b->items[b->length++] = v;
    size_t i = ptr_hash(native) & (b->table_size - 1);
    while (b->table[i]) i = (i + 1) & (b->table_size - 1);
    b->table[i] = v;
    lex->user = v;
    return v;
}

static char *native_lower(arena_t *a, const lxb_char_t *s, size_t len) {
    char *v = ar_strndup(a, (const char *)s, len);
    for (size_t i = 0; i < len; i++) v[i] = (char)lower((unsigned char)v[i]);
    return v;
}

static node_t *new_native(struct html_parser *p, lxb_dom_node_t *lex, web_doc *owner) {
    web_doc *d = owner ? owner : p->d;
    doc_dom_budget(d);
    jmp_buf trap; jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    node_t *n = ar_alloc(&d->mem, sizeof *n);
    n->owner = n->allocation_doc = d;
    n->image = n->image_request = -1;
    n->owned_next = d->owned_nodes; d->owned_nodes = n;
    size_t len = 0;
    const lxb_char_t *name;
    switch (lex->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
        n->type = N_ELEM;
        name = lxb_dom_element_qualified_name(lxb_dom_interface_element(lex), &len);
        n->name = native_lower(&d->mem, name, len);
        n->raw_name = ar_strndup(&d->mem, (const char *)name, len);
        n->tag = (uint16_t)tag_lookup(n->name, len);
        n->namespace_id = lex->ns == LXB_NS_SVG ? NS_SVG : lex->ns == LXB_NS_MATH ? NS_MATHML : NS_HTML;
        n->foreign = n->namespace_id != NS_HTML;
        /* Contextual Fragment mode is distinct from innerHTML's Inert mode.
           A detached eligible HTML script is not already started, but parsing
           never executes it. Preserve inert template owners/foreign scripts;
           html_resume independently marks EOF-truncated scripts as started. */
        n->script_started = n->tag == T_script && (d->inert ||
            (p->fragment && (!p->contextual_fragment || !p->scripting || n->foreign)));
        break;
    case LXB_DOM_NODE_TYPE_DOCUMENT: n->type = N_DOC; break;
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT: n->type = N_FRAGMENT; break;
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: {
        n->type = N_DOCTYPE;
        lxb_dom_document_type_t *dt = lxb_dom_interface_document_type(lex);
        name = lxb_dom_document_type_name(dt, &len);
        n->name = ar_strndup(&d->mem, (const char *)name, len);
        n->public_id = ar_strndup(&d->mem, (const char *)dt->public_id.data, dt->public_id.length);
        n->system_id = ar_strndup(&d->mem, (const char *)dt->system_id.data, dt->system_id.length);
        break;
    }
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION: n->type = N_TEXT; break;
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION:
        n->type = N_PI;
        name = lxb_dom_processing_instruction_target(lxb_dom_interface_processing_instruction(lex), &len);
        n->name = ar_strndup(&d->mem, (const char *)name, len);
        break;
    default: n->type = N_COMMENT; break;
    }
    d->mem.trap = old;
    return n;
}

static struct binding *import_node(struct html_parser *p, lxb_dom_node_t *lex, web_doc *owner) {
    struct binding *b = by_lex(lex);
    if (b) return b;
    node_t *n = new_native(p, lex, owner);
    return n ? bind_node(p, lex, n) : NULL;
}

/* Lexbor's HTML namespace/attribute helpers case-fold arbitrary namespace URIs
   and local names. The bridge uses its raw hashes for DOM-created metadata. */
LXB_API lxb_dom_attr_data_t *lxb_dom_attr_qualified_name_append(lexbor_hash_t *, const lxb_char_t *, size_t);
static bool lex_attr_context_namespace(lxb_ns_id_t ns) {
    return ns == LXB_NS_HTML || ns == LXB_NS_SVG || ns == LXB_NS_MATH;
}
static const char *lex_attr_namespace(lxb_dom_attr_t *a, size_t *len) {
    *len = 0;
    /* tree_append_attributes uses the element namespace as a lookup context,
       not as the DOM attribute namespace. XML/XLINK/XMLNS adjustments are real
       namespaces. Export uses dynamic IDs for explicit HTML/SVG/MathML URIs. */
    if (a->node.ns == LXB_NS__UNDEF || lex_attr_context_namespace(a->node.ns)) return NULL;
    return (const char *)lxb_ns_by_id(a->node.owner_document->ns, a->node.ns, len);
}
static const char *lex_attr_local_name(lxb_dom_attr_t *a, size_t *len) {
    const char *name = (const char *)lxb_dom_attr_qualified_name(a, len);
    size_t ns_len = 0;
    /* Qualified names preserve adjusted SVG case. The parser's formatting
       reconstruction copies qualified/local names and ns, but drops prefix. */
    if (lex_attr_namespace(a, &ns_len)) {
        const char *colon = memchr(name, ':', *len);
        if (colon) { *len -= (size_t)(colon + 1 - name); return colon + 1; }
    }
    return name;
}
static bool attr_metadata_equal(const struct attr *native, lxb_dom_attr_t *lex) {
    size_t nl = 0, ll = 0;
    const char *ns = lex_attr_namespace(lex, &nl);
    const char *local = lex_attr_local_name(lex, &ll);
    const char *expected = native->local ? native->local : native->raw;
    return (native->namespace_uri ? ns && strlen(native->namespace_uri) == nl && !memcmp(native->namespace_uri, ns, nl) : !ns || !nl) &&
        strlen(expected) == ll && !memcmp(expected, local, ll);
}
static bool attrs_equal(node_t *n, lxb_dom_element_t *el) {
    int i = 0;
    for (lxb_dom_attr_t *a = el->first_attr; a; a = a->next, i++) {
        if (i == n->nattrs) return false;
        size_t nl = 0, vl = 0;
        const char *name = (const char *)lxb_dom_attr_qualified_name(a, &nl);
        const char *value = (const char *)lxb_dom_attr_value(a, &vl);
        if (!attr_metadata_equal(&n->attrs[i], a) || strlen(n->attrs[i].raw) != nl || memcmp(n->attrs[i].raw, name, nl) ||
            strlen(n->attrs[i].value) != vl || (vl && memcmp(n->attrs[i].value, value, vl))) return false;
    }
    return i == n->nattrs;
}

static void attr_cache(web_doc *d, node_t *n) {
    n->id = NULL; n->classes = NULL; n->nclasses = 0;
    const char *classes = NULL;
    for (int i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].namespace_uri) continue;
        const char *name = n->attrs[i].raw;
        if (!strcmp(name, "id") && *n->attrs[i].value) n->id = n->attrs[i].value;
        if (!strcmp(name, "class")) classes = n->attrs[i].value;
    }
    if (!classes) return;
    size_t count = 0;
    const char *s = classes;
    while (*s) {
        while (is_space((unsigned char)*s)) s++;
        if (!*s) break;
        count++;
        while (*s && !is_space((unsigned char)*s)) s++;
    }
    if (!count) return;
    n->classes = ar_alloc(&d->mem, count * sizeof *n->classes);
    s = classes;
    while (*s) {
        while (is_space((unsigned char)*s)) s++;
        if (!*s) break;
        const char *start = s;
        while (*s && !is_space((unsigned char)*s)) s++;
        n->classes[n->nclasses++] = ar_strndup(&d->mem, start, (size_t)(s - start));
    }
}

static bool import_fields(struct binding *b) {
    node_t *n = b->native;
    web_doc *d = n->allocation_doc;
    doc_dom_budget(d);
    jmp_buf trap; jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return false; }
    if (n->type == N_ELEM) {
        lxb_dom_element_t *el = lxb_dom_interface_element(b->lex);
        if (!attrs_equal(n, el)) {
            bool was_open=node_attr(n,"open")!=NULL;
            size_t count = 0;
            for (lxb_dom_attr_t *a = el->first_attr; a; a = a->next) count++;
            struct attr *attrs = count ? ar_alloc(&d->mem, count * sizeof *attrs) : NULL;
            size_t i = 0;
            for (lxb_dom_attr_t *a = el->first_attr; a; a = a->next, i++) {
                size_t nl = 0, vl = 0;
                const lxb_char_t *name = lxb_dom_attr_qualified_name(a, &nl);
                const lxb_char_t *value = lxb_dom_attr_value(a, &vl);
                attrs[i].name = native_lower(&d->mem, name, nl);
                attrs[i].raw = ar_strndup(&d->mem, (const char *)name, nl);
                attrs[i].value = ar_strndup(&d->mem, value ? (const char *)value : "", vl);
                size_t nsl = 0, ll = 0;
                const char *ns = lex_attr_namespace(a, &nsl);
                const char *local = lex_attr_local_name(a, &ll);
                attrs[i].namespace_uri = ns && nsl ? ar_strndup(&d->mem, ns, nsl) : NULL;
                if (ns && nsl) {
                    const char *colon = memchr(name, ':', nl);
                    if (colon) {
                        attrs[i].prefix = ar_strndup(&d->mem, (const char *)name, (size_t)(colon - (const char *)name));
                    }
                }
                attrs[i].local = ar_strndup(&d->mem, local, ll);
                /* Value/order changes must not replace a materialized Attr. */
                for (int j = 0; j < n->nattrs; j++) if (attr_metadata_equal(&n->attrs[j], a)) {
                    attrs[i].node = n->attrs[j].node; break;
                }
            }
            node_t temp = *n;
            temp.attrs = attrs; temp.nattrs = (int)count;
            attr_cache(d, &temp);
            doc_attrs_publish(n, attrs, (int)count);
            n->id = temp.id; n->classes = temp.classes; n->nclasses = temp.nclasses;
            doc_details_parser_attribute_changed(n,was_open);
        }
    } else if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) {
        lexbor_str_t *s = &lxb_dom_interface_character_data(b->lex)->data;
        if (!n->text || n->textlen != s->length || (s->length && memcmp(n->text, s->data, s->length))) {
            n->text = ar_strndup(&d->mem, s->data ? (const char *)s->data : "", s->length);
            n->textlen = s->length;
        }
    }
    d->mem.trap = old;
    return true;
}

bool html_bridge_init(struct html_parser *p) {
    p->bridge = calloc(1, sizeof *p->bridge);
    if (!p->bridge) return false;
    if(!p->native_root)p->native_root = doc_node_create(p->d, p->fragment ? N_FRAGMENT : N_DOC, NULL, NULL, 0);
    return p->native_root && bind_node(p, p->root, p->native_root);
}

node_t *html_bridge_native(struct html_parser *p, lxb_dom_node_t *node) {
    if (!node) return NULL;
    struct binding *b = import_node(p, node, p->d);
    return b ? b->native : NULL;
}

bool html_bridge_import(struct html_parser *p) {
    struct html_bridge *b = p->bridge;
    /* Discover the entire forest, not just document descendants. Removed
       open elements and active formatting nodes still participate in parsing. */
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < b->length; i++) {
            struct binding *v = b->items[i];
            /* Native style/layout walks have a bounded recursive depth. Reject
               hostile trees before publishing links, never flatten HTML into
               a different document to pretend it parsed successfully. */
            unsigned depth = 0;
            for (lxb_dom_node_t *a = v->lex; a; a = a->parent)
                if (++depth > 400) return false;
            if (!import_fields(v)) return false;
            for (lxb_dom_node_t *ch = v->lex->first_child; ch; ch = ch->next)
                if (!import_node(p, ch, v->native->owner)) return false;
            if (v->native->type == N_ELEM && v->lex->ns == LXB_NS_HTML && v->lex->local_name == LXB_TAG_TEMPLATE) {
                lxb_html_template_element_t *t = lxb_html_interface_template(v->lex);
                node_t *content = doc_template_content(v->native->owner, v->native);
                if (!content || !t->content) return false;
                lxb_dom_node_t *lc = lxb_dom_interface_node(t->content);
                if (!by_lex(lc) && !bind_node(p, lc, content)) return false;
            }
        }
        if (pass == 0 && p->lex && p->lex->tree) {
            lexbor_array_t *arrays[] = {p->lex->tree->open_elements, p->lex->tree->active_formatting};
            for (size_t k = 0; k < 2; k++) if (arrays[k]) for (size_t i = 0; i < arrays[k]->length; i++) {
                lxb_dom_node_t *n = arrays[k]->list[i];
                if (n && n != (lxb_dom_node_t *)lxb_html_tree_active_formatting_marker() &&
                    !import_node(p, n, p->d)) return false;
            }
        }
    }
    /* The tree builder can move an adopted formatting node again. Its native
       logical owner follows the destination forest, never the proxy's fixed
       Lexbor allocation document. Run adoption on the old coherent tree before
       committing new links so its resource/template bookkeeping stays intact. */
    for (size_t i = 0; i < b->length; i++) {
        struct binding *v = b->items[i];
        lxb_dom_node_t *root = v->lex;
        while (root != p->root && root->parent) root = root->parent;
        struct binding *ancestor = by_lex(root);
        if (ancestor && v->native->owner != ancestor->native->owner &&
            !doc_node_adopt(ancestor->native->owner, v->native)) return false;
    }
    /* Commit links after every referenced native node and field is available.
       No observer or script is entered halfway through this transaction. */
    web_doc *changed_docs[66];
    size_t ndocs = 0;
    for (size_t i = 0; i < b->length; i++) {
        node_t *n = b->items[i]->native;
        n->parent = n->prev = n->next = n->first = n->last = NULL;
        n->elem_index = 0;
    }
    for (size_t i = 0; i < b->length; i++) {
        struct binding *v = b->items[i];
        node_t *parent = v->native;
        int index = 0;
        for (lxb_dom_node_t *ch = v->lex->first_child; ch; ch = ch->next) {
            struct binding *c = by_lex(ch);
            if (!c) return false;
            node_t *n = c->native;
            n->parent = parent; n->prev = parent->last;
            if (parent->last) parent->last->next = n; else parent->first = n;
            parent->last = n;
            if (n->type == N_ELEM) n->elem_index = ++index;
        }
        if (parent->tag == T_textarea && !parent->foreign && !parent->value_dirty) parent->control_ready = false;
        web_doc *d = parent->owner;
        parent->resource_revision++;
        size_t j = 0;
        while (j < ndocs && changed_docs[j] != d) j++;
        if (j == ndocs) {
            if (ndocs < sizeof changed_docs / sizeof *changed_docs) changed_docs[ndocs++] = d;
        }
    }
    /* Publish mutation/slot snapshots only after every native link is coherent.
       A root's children can occur later than its host in this binding forest. */
    doc_details_parser_finish(p->native_root);
    for (size_t i = 0; i < ndocs; i++) doc_mutated(changed_docs[i], NULL);
    if (!p->fragment) {
        p->d->root = p->native_root;
        p->d->quirks = p->document->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS;
        doc_sync_tree(p->d);
    }
    return true;
}

static lxb_dom_node_t *new_lex(struct html_parser *p, node_t *n) {
    lxb_dom_document_t *d = &p->document->dom_document;
    switch (n->type) {
    case N_ELEM: {
        const char *name = n->raw_name ? n->raw_name : n->name;
        const char *ns = n->namespace_id == NS_SVG ? "http://www.w3.org/2000/svg" :
            n->namespace_id == NS_MATHML ? "http://www.w3.org/1998/Math/MathML" : "http://www.w3.org/1999/xhtml";
        return lxb_dom_interface_node(lxb_dom_element_create(d, (const lxb_char_t *)name, strlen(name),
            (const lxb_char_t *)ns, strlen(ns), NULL, 0, NULL, 0, false));
    }
    case N_TEXT:
        return lxb_dom_interface_node(lxb_dom_document_create_text_node(d, (const lxb_char_t *)n->text, n->textlen));
    case N_COMMENT:
        return lxb_dom_interface_node(lxb_dom_document_create_comment(d, (const lxb_char_t *)n->text, n->textlen));
    case N_PI: {
        /* Snapshot export is not a new DOM API call: CharacterData mutations
           can legally introduce ?> after creation. The upstream convenience
           creator validates initial data, so use its raw native interface. */
        lxb_dom_processing_instruction_t *pi = lxb_dom_processing_instruction_interface_create(d);
        if (!pi) return NULL;
        size_t len = strlen(n->name);
        if (!lexbor_str_init(&pi->target, d->text, len) ||
            !lexbor_str_append(&pi->target, d->text, (const lxb_char_t *)n->name, len) ||
            lxb_dom_character_data_replace(&pi->char_data, (const lxb_char_t *)n->text, n->textlen, 0, 0) != LXB_STATUS_OK)
            return lxb_dom_interface_node(lxb_dom_processing_instruction_interface_destroy(pi));
        return lxb_dom_interface_node(pi);
    }
    case N_DOCTYPE: {
        const char *name = n->name ? n->name : "html";
        const char *pub = n->public_id ? n->public_id : "", *sys = n->system_id ? n->system_id : "";
        return lxb_dom_interface_node(lxb_dom_document_type_create(d, (const lxb_char_t *)name, strlen(name),
            (const lxb_char_t *)pub, strlen(pub), (const lxb_char_t *)sys, strlen(sys), NULL));
    }
    default:
        /* External document roots only serve as private attachment containers;
           their logical owner and Web API type stay on the native node. */
        return lxb_dom_interface_node(lxb_dom_document_create_document_fragment(d));
    }
}

static struct binding *export_node(struct html_parser *p, node_t *n) {
    struct binding *b = by_native(p->bridge, n);
    if (b) return b;
    if (n->type == N_FRAGMENT && n->template_host) {
        struct binding *host = export_node(p, n->template_host);
        if (!host) return NULL;
        lxb_html_template_element_t *t = lxb_html_interface_template(host->lex);
        return t->content ? bind_node(p, lxb_dom_interface_node(t->content), n) : NULL;
    }
    lxb_dom_node_t *lex = new_lex(p, n);
    return lex ? bind_node(p, lex, n) : NULL;
}

static lxb_dom_attr_t *export_attr(lxb_dom_document_t *d, lxb_ns_id_t element_ns, const struct attr *a) {
    lxb_dom_attr_t *made = lxb_dom_attr_interface_create(d);
    if (!made) return NULL;
    const char *local = a->local ? a->local : a->raw;
    lxb_dom_attr_data_t *ld = lxb_dom_attr_qualified_name_append(d->attrs, (const lxb_char_t *)local, strlen(local));
    lxb_dom_attr_data_t *qd = lxb_dom_attr_qualified_name_append(d->attrs, (const lxb_char_t *)a->raw, strlen(a->raw));
    if (!ld || !qd) goto failed;
    const lxb_dom_attr_data_t *known_local = !a->namespace_uri && !a->prefix ?
        lxb_dom_attr_data_by_qualified_name(d->attrs, (const lxb_char_t *)local, strlen(local)) : NULL;
    const lxb_dom_attr_data_t *known_qualified = lxb_dom_attr_data_by_qualified_name(d->attrs, (const lxb_char_t *)a->raw, strlen(a->raw));
    made->node.local_name = known_local ? known_local->attr_id : ld->attr_id;
    made->qualified_name = known_qualified ? known_qualified->attr_id : qd->attr_id;
    if (a->namespace_uri) {
        /* raw insertion avoids lxb_ns_append's ASCII case folding. */
        const lxb_ns_data_t *known = lxb_ns_data_by_link(d->ns, (const lxb_char_t *)a->namespace_uri, strlen(a->namespace_uri));
        size_t len = 0; const lxb_char_t *uri = known ? lxb_ns_by_id(d->ns, known->ns_id, &len) : NULL;
        if (uri && !lex_attr_context_namespace(known->ns_id) &&
            len == strlen(a->namespace_uri) && !memcmp(uri, a->namespace_uri, len)) made->node.ns = known->ns_id;
        else {
            /* A DOM-authored attribute may explicitly use the same URI as its
               element. Keep its real namespace distinguishable from the HTML
               tree builder's static context IDs, including after clone/rebuild. */
            lxb_ns_data_t *ns = lexbor_hash_insert(d->ns, lexbor_hash_insert_raw, (const lxb_char_t *)a->namespace_uri, strlen(a->namespace_uri));
            if (!ns) goto failed;
            ns->ns_id = (lxb_ns_id_t)ns; made->node.ns = ns->ns_id;
        }
    } else made->node.ns = element_ns;
    if (a->prefix) {
        lxb_ns_prefix_data_t *prefix = lexbor_hash_insert(d->prefix, lexbor_hash_insert_raw, (const lxb_char_t *)a->prefix, strlen(a->prefix));
        if (!prefix) goto failed;
        prefix->prefix_id = (lxb_ns_prefix_id_t)prefix; made->node.prefix = prefix->prefix_id;
    }
    if (lxb_dom_attr_set_value(made, (const lxb_char_t *)a->value, strlen(a->value)) != LXB_STATUS_OK) goto failed;
    return made;
failed:
    lxb_dom_attr_interface_destroy(made); return NULL;
}

static bool export_fields(struct html_parser *p, struct binding *b) {
    node_t *n = b->native;
    if (n->type == N_ELEM) {
        lxb_dom_element_t *el = lxb_dom_interface_element(b->lex);
        if (attrs_equal(n, el)) return true;
        /* Qualified names may be duplicated across namespaces. by_name/set_attr
           would collapse those records, so construct an ordered full snapshot. */
        lxb_dom_attr_t **attrs = n->nattrs ? calloc((size_t)n->nattrs, sizeof *attrs) : NULL;
        if (n->nattrs && !attrs) return false;
        for (int i = 0; i < n->nattrs; i++) {
            attrs[i] = export_attr(el->node.owner_document, el->node.ns, &n->attrs[i]);
            if (!attrs[i]) {
                for (int j = 0; j < i; j++) lxb_dom_attr_interface_destroy(attrs[j]);
                free(attrs); return false;
            }
        }
        for (lxb_dom_attr_t *a = el->first_attr, *next; a; a = next) {
            next = a->next; lxb_dom_attr_remove(a); lxb_dom_attr_interface_destroy(a);
        }
        for (int i = 0; i < n->nattrs; i++) if (lxb_dom_element_attr_append(el, attrs[i]) != LXB_STATUS_OK) {
            for (int j = i; j < n->nattrs; j++) lxb_dom_attr_interface_destroy(attrs[j]);
            free(attrs); return false;
        }
        free(attrs);
    } else if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(b->lex);
        if (cd->data.length != n->textlen || (n->textlen && memcmp(cd->data.data, n->text, n->textlen)))
            return lxb_dom_character_data_replace(cd, (const lxb_char_t *)n->text, n->textlen, 0, cd->data.length) == LXB_STATUS_OK;
    }
    return true;
}

bool html_bridge_export(struct html_parser *p) {
    struct html_bridge *b = p->bridge;
    /* The list grows while discovering script-created nodes, new parents and
       adopted subtrees. Existing Lexbor pointers remain stable for SOE/AFE. */
    for (size_t i = 0; i < b->length; i++) {
        struct binding *v = b->items[i];
        node_t *n = v->native;
        if (!export_fields(p, v)) return false;
        if (n->parent && !export_node(p, n->parent)) return false;
        for (node_t *ch = n->first; ch; ch = ch->next) if (!export_node(p, ch)) return false;
        if (n->template_content) {
            lxb_html_template_element_t *t = lxb_html_interface_template(v->lex);
            lxb_dom_node_t *content = lxb_dom_interface_node(t->content);
            if (!content) return false;
            struct binding *mapped = by_native(b, n->template_content);
            if (!mapped && !bind_node(p, content, n->template_content)) return false;
            if (mapped && mapped->lex != content) return false;
        }
    }
    for (size_t i = 0; i < b->length; i++) {
        lxb_dom_node_t *n = b->items[i]->lex;
        n->parent = n->prev = n->next = n->first_child = n->last_child = NULL;
    }
    for (size_t i = 0; i < b->length; i++) {
        struct binding *v = b->items[i];
        for (node_t *ch = v->native->first; ch; ch = ch->next) {
            struct binding *c = by_native(b, ch);
            if (!c) return false;
            lxb_dom_node_insert_child_wo_events(v->lex, c->lex);
        }
    }
    return true;
}

void html_bridge_destroy(struct html_parser *p) {
    struct html_bridge *b = p->bridge;
    if (!b) return;
    for (size_t i = 0; i < b->length; i++) {
        /* A failed Lexbor fragment may already have destroyed its synthetic
           root. We immediately destroy the remaining parser/document pools;
           do not dereference any Lexbor nodes during bridge teardown. */
        free(b->items[i]);
    }
    free(b->items); free(b->table); free(b);
    p->bridge = NULL;
}
