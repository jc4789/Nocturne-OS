/* Lexbor's tree-builder state is private to an active parse. The observable
   DOM remains Nocturne's node_t tree. Synchronize at script boundaries rather
   than relying on mutation callbacks: foster parenting/adoption/text merging
   in Lexbor deliberately bypass some of those callbacks. */
#include "html_lexbor.h"
#include "elements.h"
#include "html_pi.h"
#include <limits.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document_type.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/processing_instruction.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/tree/active_formatting.h>
extern bool web_js_shadow_allowed(web_doc *, node_t *);
extern void web_js_parser_remove(web_doc *, node_t *);

struct binding {
    lxb_dom_node_t *lex;
    node_t *native;
    bool changed;
};
struct html_bridge {
    struct binding **items, **table;
    size_t length, capacity, table_size;
    bool ce_pending;
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
    if (b->length == SIZE_MAX) return NULL;
    if (b->length + 1 >= b->table_size / 2) {
        if (b->table_size > SIZE_MAX / 2 / sizeof *b->table) return NULL;
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
        if (b->capacity > SIZE_MAX / 2 / sizeof *b->items) return NULL;
        size_t size = b->capacity ? b->capacity * 2 : 64;
        struct binding **items = realloc(b->items, size * sizeof *items);
        if (!items) return NULL;
        b->items = items; b->capacity = size;
    }
    struct binding *v = malloc(sizeof *v);
    if (!v) return NULL;
    v->lex = lex; v->native = native; v->changed = false;
    b->items[b->length++] = v;
    size_t i = ptr_hash(native) & (b->table_size - 1);
    while (b->table[i]) i = (i + 1) & (b->table_size - 1);
    b->table[i] = v;
    lex->user = v;
    return v;
}

static bool rebind_native(struct html_parser *p, struct binding *binding, node_t *native) {
    if (!native) return false;
    if (binding->native == native) return true;
    struct html_bridge *bridge = p->bridge;
    if (by_native(bridge, native)) return false;
    binding->native = native;
    memset(bridge->table, 0, bridge->table_size * sizeof *bridge->table);
    for (size_t i = 0; i < bridge->length; i++) {
        struct binding *item = bridge->items[i];
        size_t slot = ptr_hash(item->native) & (bridge->table_size - 1);
        while (bridge->table[slot]) slot = (slot + 1) & (bridge->table_size - 1);
        bridge->table[slot] = item;
    }
    return true;
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
        n->custom_registry_id = -2;
        if (!p->fragment && lex->ns == LXB_NS_HTML && lex->local_name == LXB_TAG_META)
            n->html_policy_processed = true;
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
        struct binding *parent = by_lex(lex->parent);
        n->custom_registry_id = parent && parent->native->custom_registry_id != -2 ?
                               parent->native->custom_registry_id : d->custom_registry_id;
        n->custom_registry_realm = parent && parent->native->custom_registry_id != -2 ?
                                  parent->native->custom_registry_realm : d->custom_registry_realm;
        if (lxb_dom_interface_element(lex)->is_value) {
            lexbor_str_t *is = lxb_dom_interface_element(lex)->is_value;
            n->custom_is = ar_strndup(&d->mem, (const char *)is->data, is->length);
        }
        /* Contextual Fragment mode is distinct from innerHTML's Inert mode.
           A detached eligible HTML script is not already started, but parsing
           never executes it. Preserve inert template owners/foreign scripts;
           html_resume independently marks EOF-truncated scripts as started. */
        n->script_started = n->tag == T_script && (d->inert ||
            (p->fragment && (!p->contextual_fragment || !p->scripting || n->foreign)));
        n->script_parse_eligible = n->tag == T_script && p->fragment && p->scripting && !d->inert &&
                                  (n->namespace_id == NS_HTML || n->namespace_id == NS_SVG);
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
        n->public_id_len = dt->public_id.length; n->system_id_len = dt->system_id.length;
        n->doctype_identifiers_sized = true;
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
        if (count == INT_MAX || count == SIZE_MAX / sizeof *n->classes) {
            if (d->mem.trap) longjmp(*d->mem.trap, 1);
            abort();
        }
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
            b->changed = true;
            bool was_open=node_attr(n,"open")!=NULL;
            size_t count = 0;
            for (lxb_dom_attr_t *a = el->first_attr; a; a = a->next) {
                if (count == INT_MAX || count == SIZE_MAX / sizeof(struct attr)) { d->mem.trap = old; return false; }
                count++;
            }
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
            b->changed = true;
            n->text = ar_strndup(&d->mem, s->data ? (const char *)s->data : "", s->length);
            n->textlen = s->length;
        }
    }
    d->mem.trap = old;
    return true;
}

static bool ascii_value_is(const lxb_char_t *value, size_t length, const char *expected) {
    if (!value || strlen(expected) != length) return false;
    for (size_t i = 0; i < length; i++)
        if (lower(value[i]) != (unsigned char)expected[i]) return false;
    return true;
}
static bool pi_target_is(lxb_dom_node_t *node, const char *target) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION) return false;
    lexbor_str_t *name = &lxb_dom_interface_processing_instruction(node)->target;
    return name->length == strlen(target) && !memcmp(name->data, target, name->length);
}
static lxb_dom_node_t *next_descendant(lxb_dom_node_t *scope, lxb_dom_node_t *node) {
    if (node->first_child) return node->first_child;
    while (node != scope && !node->next) node = node->parent;
    return node == scope ? NULL : node->next;
}
static bool prepare_patch(struct html_parser *p, lxb_html_template_element_t *t, lxb_dom_node_t *scope,
                          const lxb_char_t *name, size_t length) {
    if (!scope || !length) return false;
    lxb_dom_node_t *start = NULL, *end = NULL;
    for (lxb_dom_node_t *n = scope->first_child; n; n = next_descendant(scope, n)) {
        if (!pi_target_is(n, "marker") && !pi_target_is(n, "start")) continue;
        lexbor_str_t *data = &lxb_dom_interface_character_data(n)->data;
        struct html_pi_attribute *attrs; size_t count;
        if (!html_pi_parse((const char *)data->data, data->length, &attrs, &count)) continue;
        bool match = false;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(attrs[i].name, "name") && attrs[i].value_length == length &&
                !memcmp(attrs[i].value, name, length)) match = true;
        html_pi_free(attrs, count);
        if (!match) continue;
        start = n;
        if (pi_target_is(n, "marker")) end = n;
        else {
            size_t depth = 0;
            for (lxb_dom_node_t *s = n->next; s; s = s->next) {
                if (pi_target_is(s, "start")) depth++;
                else if (pi_target_is(s, "end")) {
                    if (!depth) { end = s; break; }
                    depth--;
                }
            }
        }
        break;
    }
    if (!start) return false;
    t->insertion_target = start->parent;
    t->insertion_start = start;
    t->insertion_end = end;
    t->parser_only = true;
    if (start != end) {
        if (!html_bridge_import(p)) goto fail;
        size_t count = 0;
        for (lxb_dom_node_t *n = start->next; n && n != end; n = n->next) count++;
        if (count > SIZE_MAX / sizeof(node_t *)) goto fail;
        node_t **removed = count ? malloc(count * sizeof *removed) : NULL;
        if (count && !removed) goto fail;
        size_t i = 0;
        for (lxb_dom_node_t *n = start->next; n && n != end; n = n->next) {
            struct binding *binding = by_lex(n);
            if (!binding) { free(removed); goto fail; }
            removed[i++] = binding->native;
        }
        for (i = 0; i < count; i++) if (removed[i]->parent) {
            web_js_parser_remove(removed[i]->owner, removed[i]);
            if (!html_bridge_export(p)) { free(removed); goto fail; }
        }
        free(removed);
    }
    return true;
fail:
    p->lex->tree->status = LXB_STATUS_ERROR_MEMORY_ALLOCATION; return false;
}
/* Called while the template is only on SOE. A successful DSD template is never
   inserted into the light tree; its Lexbor content is the native ShadowRoot. */
static bool modern_template_open(lxb_html_tree_t *tree, lxb_html_token_t *token,
        lxb_html_template_element_t *t, lxb_dom_node_t *host,
        lxb_dom_node_t *position, bool before, void *context) {
    (void)token;
    struct html_parser *p = context;
    lxb_dom_element_t *element = lxb_dom_interface_element(t);
    size_t length = 0;
    const lxb_char_t *mode = lxb_dom_element_get_attribute(element,
                                      (const lxb_char_t *)"shadowrootmode", 14, &length);
    bool closed = ascii_value_is(mode, length, "closed");
    if (closed || ascii_value_is(mode, length, "open")) {
        if (!p->allow_declarative_shadow || !host || !tree->open_elements->length ||
            host == tree->open_elements->list[0]) return false;
        t->parser_only = true; /* do not bind the temporary inert content */
        if (!html_bridge_import(p)) goto fail;
        node_t *native_host = p->fragment && host == tree->fragment ? p->context_node : html_bridge_native(p, host);
        if (native_host && native_host->shadow_host) native_host = native_host->shadow_host;
        if (!native_host || !doc_shadow_host_valid(native_host) || native_host->shadow_root) {
            t->parser_only = false; return false;
        }
        if (!web_js_shadow_allowed(native_host->owner, native_host)) {
            t->parser_only = false; return false;
        }
#define HAS(name) lxb_dom_element_has_attribute(element, (const lxb_char_t *)(name), sizeof(name)-1)
        size_t slot_length = 0;
        const lxb_char_t *slot = lxb_dom_element_get_attribute(element,
                   (const lxb_char_t *)"shadowrootslotassignment", 24, &slot_length);
        node_t *root = doc_shadow_attach(native_host->owner, native_host, closed,
                         HAS("shadowrootdelegatesfocus"), HAS("shadowrootclonable"),
                         HAS("shadowrootserializable"), ascii_value_is(slot, slot_length, "manual"));
        if (!root) { t->parser_only = false; return false; }
        root->shadow_declarative = true;
        root->custom_registry_id = HAS("shadowrootcustomelementregistry") ? -1 : native_host->owner->custom_registry_id;
        root->custom_registry_realm = native_host->owner->custom_registry_realm;
#undef HAS
        t->shadow_host = host;
        if (!bind_node(p, lxb_dom_interface_node(t->content), root)) goto fail;
        return true;
    }
    const lxb_char_t *name = lxb_dom_element_get_attribute(element,
                                             (const lxb_char_t *)"for", 3, &length);
    if (name) {
        lxb_dom_node_t *scope = before ? position->parent : position;
        if (scope && scope->ns == LXB_NS_HTML && scope->local_name == LXB_TAG_BODY)
            scope = scope->parent;
        return prepare_patch(p, t, scope, name, length);
    }
    return false;
fail:
    tree->status = LXB_STATUS_ERROR_MEMORY_ALLOCATION; return false;
}
static lxb_status_t modern_template_close(lxb_html_tree_t *tree,
                                          lxb_html_template_element_t *t, void *context) {
    (void)tree;
    struct html_parser *p = context;
    if (!html_bridge_import(p)) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    lxb_dom_node_t *markers[] = {t->insertion_start, t->insertion_end};
    for (size_t i = 0; i < 2; i++) {
        struct binding *binding = by_lex(markers[i]);
        if (binding && binding->native->parent) {
            web_js_parser_remove(binding->native->owner, binding->native);
            if (!html_bridge_export(p)) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }
    }
    return LXB_STATUS_OK;
}

static lxb_status_t parser_element_before(lxb_html_tree_t *tree, lxb_dom_node_t *node,
                                          lxb_html_token_t *token, void *context) {
    (void)token;
    struct html_parser *p = context;
    if (node->ns != LXB_NS_HTML || p->fragment || !p->scripting ||
        lxb_html_tree_parsing_inert_template_contents(tree)) return LXB_STATUS_OK;
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    size_t name_length = 0;
    const lxb_char_t *name = lxb_dom_element_qualified_name(element, &name_length);
    lexbor_str_t *is_value = element->is_value;
    if (!is_value && !memchr(name, '-', name_length)) return LXB_STATUS_OK;
    char *local = malloc(name_length + 1);
    char *is = is_value ? malloc(is_value->length + 1) : NULL;
    if (!local || (is_value && !is)) { free(local); free(is); return LXB_STATUS_ERROR_MEMORY_ALLOCATION; }
    memcpy(local, name, name_length); local[name_length] = 0;
    if (is_value) { memcpy(is, is_value->data, is_value->length); is[is_value->length] = 0; }
    bool candidate = web_js_parser_candidate(p->d, local, is);
    free(local);
    if (!candidate) { free(is); return LXB_STATUS_OK; }
    if (!html_bridge_import(p)) { free(is); return LXB_STATUS_ERROR_MEMORY_ALLOCATION; }
    lxb_html_tree_insertion_position_t ipos;
    lxb_dom_node_t *position = lxb_html_tree_appropriate_place_inserting_node(tree, NULL, &ipos);
    lxb_dom_node_t *parent = ipos == LXB_HTML_TREE_INSERTION_POSITION_BEFORE && position ? position->parent : position;
    struct binding *parent_binding = parent ? by_lex(parent) : NULL;
    struct binding *binding = import_node(p, node, parent_binding ? parent_binding->native->owner : p->d);
    if (binding) {
        node_t *registry_parent = parent_binding ? parent_binding->native : NULL;
        while (registry_parent && registry_parent->custom_registry_id == -2)
            registry_parent = registry_parent->parent;
        if (registry_parent) {
            binding->native->custom_registry_id = registry_parent->custom_registry_id;
            binding->native->custom_registry_realm = registry_parent->custom_registry_realm;
        } else {
            binding->native->custom_registry_id = binding->native->owner->custom_registry_id;
            binding->native->custom_registry_realm = binding->native->owner->custom_registry_realm;
        }
    }
    node_t *created = binding ? web_js_parser_element(p->d, binding->native, is, false) : NULL;
    if (!binding || !rebind_native(p, binding, created)) {
        free(is); return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }
    free(is);
    p->bridge->ce_pending = true;
    return html_bridge_export(p) ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}
static lxb_status_t parser_element_after(lxb_html_tree_t *tree, lxb_dom_node_t *node,
                                         lxb_html_token_t *token, void *context) {
    struct html_parser *p = context;
    if (node->ns == LXB_NS_HTML && node->local_name == LXB_TAG_META &&
        !html_parser_meta_encoding(p, token))
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    struct binding *binding = by_lex(node);
    if (!binding || !p->bridge->ce_pending) return LXB_STATUS_OK;
    if (binding->native->face_associated && tree->form && !p->fragment &&
        !lxb_html_tree_parsing_template_contents(tree) &&
        !lxb_dom_element_has_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"form", 4)) {
        lxb_html_tree_insertion_position_t ipos;
        lxb_dom_node_t *parent = lxb_html_tree_appropriate_place_inserting_node(tree, NULL, &ipos);
        if (parent && ipos == LXB_HTML_TREE_INSERTION_POSITION_BEFORE) parent = parent->parent;
        lxb_dom_node_t *form = lxb_dom_interface_node(tree->form), *root = form;
        while (parent && parent->parent) parent = parent->parent;
        while (root->parent) root = root->parent;
        if (root == parent) lxb_html_interface_element(node)->parser_form_owner = form;
    }
    if (!import_fields(binding) || !web_js_parser_element(p->d, binding->native,
                                                        binding->native->custom_is, true))
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    return html_bridge_export(p) ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}
static lxb_status_t parser_token_after(lxb_html_tree_t *tree,
                                       lxb_html_token_t *token, void *context) {
    (void)tree; (void)token;
    struct html_parser *p = context;
    if (!p->bridge->ce_pending) return LXB_STATUS_OK;
    p->bridge->ce_pending = false;
    if (!html_bridge_import(p)) return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    web_js_parser_inserted(p->d);
    return html_bridge_export(p) ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}

bool html_bridge_init(struct html_parser *p) {
    p->bridge = calloc(1, sizeof *p->bridge);
    if (!p->bridge) return false;
    if(!p->native_root)p->native_root = doc_node_create(p->d, p->fragment ? N_FRAGMENT : N_DOC, NULL, NULL, 0);
    if (p->fragment && p->native_root) {
        node_t *context = p->context_node;
        bool template_context = context && (context->template_host ||
                               (context->type == N_ELEM && context->tag == T_template && !context->foreign));
        p->native_root->custom_registry_id = template_context ? -1 :
                context && context->custom_registry_id != -2 ? context->custom_registry_id :
                context && context->owner ? context->owner->custom_registry_id : p->d->custom_registry_id;
        p->native_root->custom_registry_realm = template_context ? NULL :
                context && context->custom_registry_id != -2 ? context->custom_registry_realm :
                context && context->owner ? context->owner->custom_registry_realm : p->d->custom_registry_realm;
    }
    if (!p->native_root || !bind_node(p, p->root, p->native_root)) return false;
    if (p->lex && p->lex->tree) {
        p->lex->tree->template_open = modern_template_open;
        p->lex->tree->template_close = modern_template_close;
        p->lex->tree->template_callback_context = p;
        p->lex->tree->before_create_element = parser_element_before;
        p->lex->tree->after_create_element = parser_element_after;
        p->lex->tree->after_token = parser_token_after;
        p->lex->tree->element_callback_context = p;
    }
    return true;
}

node_t *html_bridge_native(struct html_parser *p, lxb_dom_node_t *node) {
    if (!node) return NULL;
    struct binding *b = import_node(p, node, p->d);
    return b ? b->native : NULL;
}

static node_t *bound_native(lxb_dom_node_t *lex) {
    struct binding *b=by_lex(lex);return b?b->native:NULL;
}
static bool links_equal(struct binding *v) {
    node_t *n=v->native;lxb_dom_node_t *lex=v->lex;
    if(n->parent!=bound_native(lex->parent) || n->prev!=bound_native(lex->prev) ||
       n->next!=bound_native(lex->next) || n->first!=bound_native(lex->first_child) ||
       n->last!=bound_native(lex->last_child))return false;
    /* First/last alone miss a reorder in the middle. The parent revision is
       also the cache key for native nth-child indexing. */
    node_t *child=n->first;
    for(lxb_dom_node_t *lc=lex->first_child;lc;lc=lc->next) {
        if(!child || child!=bound_native(lc))return false;
        child=child->next;
    }
    return child==NULL;
}
struct changed_documents { web_doc **items;size_t length,capacity; };
static bool changed_document_add(struct changed_documents *docs,web_doc *d) {
    if(!d)return true;
    for(size_t i=0;i<docs->length;i++)if(docs->items[i]==d)return true;
    if(docs->length==docs->capacity) {
        size_t max=SIZE_MAX/sizeof *docs->items;
        if(docs->capacity==max)return false;
        size_t capacity=docs->capacity?(docs->capacity>max/2?max:docs->capacity*2):8;
        web_doc **items=realloc(docs->items,capacity*sizeof *items);
        if(!items)return false;
        docs->items=items;docs->capacity=capacity;
    }
    docs->items[docs->length++]=d;return true;
}

bool html_bridge_import(struct html_parser *p) {
    struct html_bridge *b = p->bridge;
    p->import_changed=false;
    for(size_t i=0;i<b->length;i++)b->items[i]->changed=false;
    /* Discover the entire forest, not just document descendants. Removed
       open elements and active formatting nodes still participate in parsing. */
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < b->length; i++) {
            struct binding *v = b->items[i];
            if (!import_fields(v)) return false;
            for (lxb_dom_node_t *ch = v->lex->first_child; ch; ch = ch->next)
                if (!import_node(p, ch, v->native->owner)) return false;
            if (v->native->type == N_ELEM && v->lex->ns == LXB_NS_HTML && v->lex->local_name == LXB_TAG_TEMPLATE) {
                lxb_html_template_element_t *t = lxb_html_interface_template(v->lex);
                if (t->parser_only) continue;
                node_t *content = doc_template_content(v->native->owner, v->native);
                if (!content || !t->content) return false;
                lxb_dom_node_t *lc = lxb_dom_interface_node(t->content);
                if (!by_lex(lc) && !bind_node(p, lc, content)) return false;
            }
            if (v->native->type == N_ELEM && v->lex->ns == LXB_NS_HTML) {
                lxb_dom_node_t *form = lxb_html_interface_element(v->lex)->parser_form_owner;
                if (form) {
                    struct binding *owner = import_node(p, form, v->native->owner);
                    if (!owner) return false;
                    doc_parser_form_set(v->native, owner->native);
                } else doc_parser_form_set(v->native, NULL);
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
    struct changed_documents changed={0};
    bool document_changed=!p->fragment && (p->d->root!=p->native_root ||
        p->d->document_mode!=(uint8_t)p->document->dom_document.compat_mode ||
        p->d->quirks!=(p->document->dom_document.compat_mode==LXB_DOM_DOCUMENT_CMODE_QUIRKS));
    if(document_changed && !changed_document_add(&changed,p->d))goto fail_changes;
    /* Compare against the still-coherent old links before adoption or commit.
       Exported native edits already agree with Lexbor and must not be notified
       twice. Removed roots/SOE/AFE/template forests are part of this list. */
    for(size_t i=0;i<b->length;i++) {
        struct binding *v=b->items[i];
        if(!links_equal(v))v->changed=true;
        lxb_dom_node_t *root=v->lex;
        while(root!=p->root && root->parent)root=root->parent;
        struct binding *ancestor=by_lex(root);
        if(ancestor && v->native->owner!=ancestor->native->owner) {
            v->changed=true;
            if(!changed_document_add(&changed,ancestor->native->owner))goto fail_changes;
        }
        if(v->changed && !changed_document_add(&changed,v->native->owner))goto fail_changes;
    }
    if(!changed.length){free(changed.items);return true;}
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
            !doc_node_adopt(ancestor->native->owner, v->native)) goto fail_changes;
    }
    /* Commit links after every referenced native node and field is available.
       No observer or script is entered halfway through this transaction. */
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
            if (!c) goto fail_changes;
            node_t *n = c->native;
            n->parent = parent; n->prev = parent->last;
            if (parent->last) parent->last->next = n; else parent->first = n;
            parent->last = n;
            if (n->type == N_ELEM) n->elem_index = ++index;
        }
    }
    /* Publish mutation/slot snapshots only after every native link is coherent.
       A root's children can occur later than its host in this binding forest. */
    for(size_t i=0;i<b->length;i++)if(b->items[i]->changed) {
        node_t *n=b->items[i]->native;
        n->resource_revision++;
        for(;n;n=n->parent)
            if(n->tag==T_textarea && !n->foreign && !n->value_dirty)n->control_ready=false;
    }
    if (!p->fragment) {
        p->d->root = p->native_root;
        p->d->document_mode = (uint8_t)p->document->dom_document.compat_mode;
        p->d->quirks = p->document->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS;
    }
    doc_details_parser_finish(p->native_root);
    for(size_t i=0;i<changed.length;i++)doc_mutated(changed.items[i],NULL);
    p->import_changed=true;
    if(!p->fragment)doc_sync_tree(p->d);
    free(changed.items);
    return true;
fail_changes:
    free(changed.items);return false;
}

static lxb_dom_node_t *new_lex(struct html_parser *p, node_t *n) {
    lxb_dom_document_t *d = &p->document->dom_document;
    switch (n->type) {
    case N_ELEM: {
        const char *name = n->raw_name ? n->raw_name : n->name;
        const char *ns = n->namespace_id == NS_SVG ? "http://www.w3.org/2000/svg" :
            n->namespace_id == NS_MATHML ? "http://www.w3.org/1998/Math/MathML" : "http://www.w3.org/1999/xhtml";
        return lxb_dom_interface_node(lxb_dom_element_create(d, (const lxb_char_t *)name, strlen(name),
            (const lxb_char_t *)ns, strlen(ns), NULL, 0,
            (const lxb_char_t *)n->custom_is, n->custom_is ? strlen(n->custom_is) : 0, false));
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
            (const lxb_char_t *)pub, n->doctype_identifiers_sized ? n->public_id_len : strlen(pub),
            (const lxb_char_t *)sys, n->doctype_identifiers_sized ? n->system_id_len : strlen(sys), NULL));
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
        if (b->lex->ns == LXB_NS_HTML) {
            struct binding *form = n->parser_form_owner ? by_native(p->bridge, n->parser_form_owner) : NULL;
            lxb_html_interface_element(b->lex)->parser_form_owner = form ? form->lex : NULL;
        }
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
        if (n->shadow_root && !export_node(p, n->shadow_root)) return false;
        if (n->shadow_host && !export_node(p, n->shadow_host)) return false;
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
