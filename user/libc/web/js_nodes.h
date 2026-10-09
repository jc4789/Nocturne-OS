/* Native wrapper identity and GC-visible DOM forest ownership. */
static struct web_js_state *node_cache_states;
static void node_cache_unlink(struct js_node_ref *r) {
    if (!r->state) return;
    *r->prev = r->next;
    if (r->next) r->next->prev = r->prev;
    *r->hash_prev = r->hash_next;
    if (r->hash_next) r->hash_next->hash_prev = r->hash_prev;
    r->state = NULL; r->prev = r->hash_prev = NULL;
}
static void node_tree_unlink(JSRuntime *rt, struct js_node_ref *r) {
    JSValue root = r->root_object, object = r->tree_object;
    if (r->member_prev) {
        *r->member_prev = r->member_next;
        if (r->member_next) r->member_next->member_prev = r->member_prev;
    }
    r->tree_root = NULL; r->member_prev = NULL; r->member_next = NULL;
    r->root_object = r->tree_object = JS_UNDEFINED;
    JS_FreeValueRT(rt, object); JS_FreeValueRT(rt, root);
}
static void node_finalizer(JSRuntime *rt, JSValue object) {
    struct js_node_ref *r = JS_GetOpaque(object, node_class);
    if (!r) return;
    /* Unpublish first; releasing edges can cascade native finalizers. */
    JS_SetOpaque(object, NULL); node_cache_unlink(r);
    node_tree_unlink(rt, r);
    while (r->members) node_tree_unlink(rt, r->members);
    JS_FreeValueRT(rt, r->owner_object);
    JS_FreeWeakRef(rt, r->object); js_free_rt(rt, r);
}
static void node_gc_mark(JSRuntime *rt, JSValueConst object, JS_MarkFunc *mark) {
    struct js_node_ref *r = JS_GetOpaque(object, node_class);
    if (!r) return;
    JS_MarkValue(rt, r->root_object, mark);
    JS_MarkValue(rt, r->owner_object, mark);
    for (struct js_node_ref *m = r->members; m; m = m->member_next)
        JS_MarkValue(rt, m->tree_object, mark);
}
static node_t *node_tree_root(node_t *n) {
    for (;;) {
        node_t *parent = n->parent ? n->parent : n->shadow_host ? n->shadow_host :
                         n->template_host ? n->template_host : n->type == N_ATTR ? n->attr_owner : NULL;
        if (!parent) return n;
        n = parent;
    }
}
bool web_js_nodes_same_forest(web_doc *document, node_t *node, node_t *parent) {
    if (!document || !node || !parent || node->owner != document || parent->owner != document)
        return false;
    /* An earlier failed synchronization may still retain conservative old
       edges. A later move must retry that synchronization, not hide it. */
    for (struct web_js_state *s = node_cache_states; s; s = s->node_cache_next)
        if (s->node_cache_failed || s->node_cache_syncing) return false;
    return node_tree_root(node) == node_tree_root(parent);
}
static JSValue wrap_local(struct web_js_state *s, node_t *n);
static bool node_retree(struct web_js_state *s, struct js_node_ref *r, JSValueConst object) {
    node_t *root = node_tree_root(r->node);
    if ((root == r->node && !r->tree_root) || (r->tree_root && r->tree_root->node == root)) return true;
    JSValue root_object = root == r->node ? JS_UNDEFINED : wrap_local(s, root);
    if (JS_IsException(root_object)) return false; /* Old edges remain intact. */
    struct js_node_ref *owner = root == r->node ? NULL : JS_GetOpaque(root_object, node_class);
    JSValue old_root = r->root_object, old_object = r->tree_object;
    if (r->member_prev) {
        *r->member_prev = r->member_next;
        if (r->member_next) r->member_next->member_prev = r->member_prev;
    }
    r->tree_root = owner; r->root_object = root_object;
    r->tree_object = owner ? JS_DupValue(s->ctx, object) : JS_UNDEFINED;
    r->member_next = owner ? owner->members : NULL;
    r->member_prev = owner ? &owner->members : NULL;
    if (owner) {
        if (owner->members) owner->members->member_prev = &r->member_next;
        owner->members = r;
    }
    JS_FreeValue(s->ctx, old_object); JS_FreeValue(s->ctx, old_root);
    return true;
}
static JSValue wrap_local(struct web_js_state *s, node_t *n) {
    if (!s->node_cache_prev) {
        s->node_cache_next = node_cache_states; s->node_cache_prev = &node_cache_states;
        if (node_cache_states) node_cache_states->node_cache_prev = &s->node_cache_next;
        node_cache_states = s;
    }
    struct js_node_ref *r = node_ref(s, n);
    if (r) return JS_GetWeakRefValue(s->ctx, r->object);
    r = js_mallocz(s->ctx, sizeof *r);
    if (!r) return JS_EXCEPTION;
    JSValue object = JS_IsUndefined(s->node_protos[NP_NODE]) ? JS_NewObjectClass(s->ctx, node_class) :
                     JS_NewObjectProtoClass(s->ctx, node_prototype(s, n), node_class);
    if (JS_IsException(object)) { js_free(s->ctx, r); return JS_EXCEPTION; }
    r->root_object = r->tree_object = r->owner_object = r->hold = JS_UNDEFINED;
    r->object = JS_NewWeakRef(s->ctx, object);
    if (!r->object) { JS_FreeValue(s->ctx, object); js_free(s->ctx, r); return JS_EXCEPTION; }
    r->node = n; r->state = s; JS_SetOpaque(object, r);
    r->next = s->nodes; r->prev = &s->nodes;
    if (s->nodes) s->nodes->prev = &r->next;
    s->nodes = r;
    unsigned bucket = node_bucket(n);
    r->hash_next = s->node_buckets[bucket]; r->hash_prev = &s->node_buckets[bucket];
    if (r->hash_next) r->hash_next->hash_prev = &r->hash_next;
    s->node_buckets[bucket] = r;
    if (n->owner && n->owner->root && n->owner->root != n) {
        r->owner_object = wrap_local(s, n->owner->root);
        if (JS_IsException(r->owner_object)) { r->owner_object = JS_UNDEFINED; JS_FreeValue(s->ctx, object); return JS_EXCEPTION; }
    }
    if (!node_retree(s, r, object)) { JS_FreeValue(s->ctx, object); return JS_EXCEPTION; }
    return object;
}
static JSValue wrap(struct web_js_state *s, node_t *n) {
    if (!n) return JS_NULL;
    if(n->owner && n->owner->js && n->owner->js!=s && n->owner->js->ctx &&
       n->owner->js->rt==s->rt && web_frame_same_origin(s->doc,n->owner))return wrap(n->owner->js,n);
    return wrap_local(s, n);
}
static void node_cache_hold(struct web_js_state *s) {
    for (struct js_node_ref *r = s->nodes; r; r = r->next)
        r->hold = JS_GetWeakRefValue(s->ctx, r->object);
}
static void node_cache_release(struct web_js_state *s) {
    for (struct js_node_ref *r = s->nodes, *next; r; r = next) {
        next = r->next;
        JSValue value = r->hold; r->hold = JS_UNDEFINED;
        JS_FreeValue(s->ctx, value);
    }
}
static void node_cache_changed(struct web_js_state *s) {
    if (!s || !s->ctx || s->node_cache_syncing) return;
    s->node_cache_syncing = true; node_cache_hold(s);
    /* Pin without allocation before creating a root. Failed post-mutation
       allocation retains old lifetime edges, never discards author state. */
    for (struct js_node_ref *r = s->nodes; r; r = r->next) {
        if (JS_IsUndefined(r->hold)) continue;
        if (!node_retree(s, r, r->hold)) {
            JSValue error = JS_GetException(s->ctx); JS_FreeValue(s->ctx, error);
            if (!s->node_cache_failed) log_text(s, 2, "DOM wrapper root update failed; previous lifetime edges retained");
            s->node_cache_failed = true; break;
        }
        node_t *owner = r->node->owner ? r->node->owner->root : NULL;
        if (owner && owner != r->node && node_opaque(r->owner_object) != owner) {
            JSValue value = wrap_local(s, owner);
            if (JS_IsException(value)) {
                JSValue error = JS_GetException(s->ctx); JS_FreeValue(s->ctx, error);
                if (!s->node_cache_failed) log_text(s, 2, "DOM wrapper owner update failed; previous lifetime edges retained");
                s->node_cache_failed = true; break;
            }
            JSValue old = r->owner_object; r->owner_object = value; JS_FreeValue(s->ctx, old);
        }
    }
    node_cache_release(s); s->node_cache_syncing = false;
}
void web_js_nodes_changed(web_doc *d) {
    if (!d) return;
    /* Inert/template documents and adopted nodes can have wrappers in another
       context. This native registry owns no JS values or context references. */
    for (struct web_js_state *s = node_cache_states; s; s = s->node_cache_next)
        node_cache_changed(s);
}
static void node_cache_free(struct web_js_state *s) {
    if (s->node_cache_prev) {
        *s->node_cache_prev = s->node_cache_next;
        if (s->node_cache_next) s->node_cache_next->node_cache_prev = s->node_cache_prev;
        s->node_cache_prev = NULL; s->node_cache_next = NULL;
    }
    node_cache_hold(s);
    struct js_node_ref *refs = s->nodes;
    s->nodes = NULL; memset(s->node_buckets, 0, sizeof s->node_buckets);
    for (struct js_node_ref *r = refs; r; r = r->next) {
        r->state = NULL; r->prev = r->hash_prev = NULL;
    }
    /* Every opaque entry is held until its turn. No finalizer can invalidate
       this traversal, and escaped wrappers cannot point at a freed state. */
    for (struct js_node_ref *r = refs, *next; r; r = next) {
        next = r->next;
        node_tree_unlink(s->rt, r);
        while (r->members) node_tree_unlink(s->rt, r->members);
        JSValue owner = r->owner_object; r->owner_object = JS_UNDEFINED;
        JS_FreeValue(s->ctx, owner);
        JSValue object = r->hold; r->hold = JS_UNDEFINED;
        JS_FreeValue(s->ctx, object);
    }
}
