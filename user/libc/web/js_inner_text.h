/* Native rendered-text collection. Included by js.c, never author hooks.
 * https://html.spec.whatwg.org/multipage/dom.html#the-innertext-idl-attribute */
struct inner_text_buffer {
    JSContext *ctx;
    char *p;
    size_t n, cap;
    unsigned required;
    bool space, failed;
};
static bool inner_text_put(struct inner_text_buffer *b, const char *s, size_t n) {
    if (b->failed) return false;
    if (n > SIZE_MAX - b->n) {
        JS_ThrowRangeError(b->ctx, "Rendered text size is not representable");
        b->failed = true; return false;
    }
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) {
            if (cap > SIZE_MAX / 2) { cap = b->n + n; break; }
            cap *= 2;
        }
        char *p = js_realloc(b->ctx, b->p, cap);
        if (!p) { b->failed = true; return false; }
        b->p = p; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n; return true;
}
static void inner_text_required(struct inner_text_buffer *b, unsigned count) {
    if (!count) return;
    if (b->required < count) b->required = count;
    b->space = false;
}
static void inner_text_string(struct inner_text_buffer *b, const char *s, size_t n) {
    if (!n || b->failed) return;
    if (b->n && b->required) {
        static const char breaks[] = "\n\n";
        inner_text_put(b, breaks, b->required);
    }
    b->required = 0;
    if (b->space && b->n && b->p[b->n - 1] != '\n') inner_text_put(b, " ", 1);
    b->space = false;
    inner_text_put(b, s, n);
}
static void inner_text_box(struct inner_text_buffer *b, const box_t *box) {
    const style_t *st = box->st;
    if (!st || st->visibility) return;
    bool collapse = st->white_space == WS_NORMAL || st->white_space == WS_NOWRAP || st->white_space == WS_PRE_LINE;
    if (!collapse) { inner_text_string(b, box->text, box->len); return; }
    for (size_t at = 0; at < box->len && !b->failed;) {
        char c = box->text[at];
        if (c == ' ') { b->space = b->n && b->p[b->n - 1] != '\n' && !b->required; at++; continue; }
        if (c == '\n') { b->space = false; inner_text_string(b, "\n", 1); at++; continue; }
        size_t end = at + 1;
        while (end < box->len && box->text[end] != ' ' && box->text[end] != '\n') end++;
        inner_text_string(b, box->text + at, end - at); at = end;
    }
}
static unsigned inner_text_boundary(const node_t *n) {
    const box_t *box = n->box;
    const style_t *st = n->style;
    if (n->type != N_ELEM || !box || !st || st->visibility) return 0;
    if (!n->foreign && n->tag == T_p) return 2;
    if (box->abspos || box->floated || st->display == D_BLOCK || st->display == D_LIST_ITEM ||
        st->display == D_FLOW_ROOT || st->display == D_TABLE || st->display == D_FLEX ||
        st->display == D_GRID || st->display == D_TABLE_CAPTION) return 1;
    return 0;
}
static bool inner_text_following_table_box(const box_t *box, unsigned kind) {
    /* The renderer normalizes anonymous table wrappers. Follow those real
       boxes, not tag-name guesses, including rows in the next row group. */
    for (const box_t *p = box; p && p->parent; p = p->parent) {
        for (const box_t *next = p->next; next; next = next->next) {
            if (next->kind == kind) return true;
            if (kind == B_ROW && next->kind == B_ROW_GROUP)
                for (const box_t *row = next->first; row; row = row->next) if (row->kind == B_ROW) return true;
        }
        if (kind == B_CELL || p->parent->kind == B_TABLE) break;
    }
    return false;
}
static void inner_text_exit(struct inner_text_buffer *b, const node_t *n) {
    if (n->type != N_ELEM || !n->box || !n->style || n->style->visibility) return;
    if (!n->foreign && n->tag == T_br) { b->space = false; inner_text_string(b, "\n", 1); }
    if (n->style->display == D_TABLE_CELL && inner_text_following_table_box(n->box, B_CELL))
        inner_text_string(b, "\t", 1);
    if (n->style->display == D_TABLE_ROW && inner_text_following_table_box(n->box, B_ROW))
        inner_text_string(b, "\n", 1);
    inner_text_required(b, inner_text_boundary(n));
}
static void inner_text_collect(struct inner_text_buffer *b, node_t *element, bool rendered) {
    node_t *n = element->first;
    while (n && !b->failed) {
        if (n->type == N_TEXT) {
            if (!rendered) inner_text_put(b, n->text ? n->text : "", n->textlen);
            else if (n->box && n->box->kind == B_TEXT) inner_text_box(b, n->box);
        } else if (rendered) inner_text_required(b, inner_text_boundary(n));
        if (n->first) { n = n->first; continue; }
        while (n && n != element) {
            if (rendered) inner_text_exit(b, n);
            if (n->next) { n = n->next; break; }
            n = n->parent;
        }
        if (n == element) break;
    }
}
static void flush_layout(struct web_js_state *s);
static bool inner_text_flush_owner(web_doc *d) {
    size_t count = 0;
    for (web_doc *p = d; p; p = p->frame_parent) {
        if (count == SIZE_MAX / sizeof(web_doc *)) {
            JS_ThrowRangeError(d->js->ctx, "Frame ancestor chain size is not representable");
            return false;
        }
        count++;
    }
    web_doc **chain = count ? js_malloc(d->js->ctx, count * sizeof *chain) : NULL;
    if (count && !chain) return false;
    size_t at = 0;
    for (web_doc *p = d; p; p = p->frame_parent) chain[at++] = p;
    while (count) {
        web_doc *owner = chain[--count];
        struct web_js_state *s = owner->js;
        if (!s || s->disabled || !owner->live || owner->inert) continue;
        if (owner->frame_parent && owner->frame_element && owner->frame_element->box) {
            const box_t *box = owner->frame_element->box;
            int width = (double)box->w >= INT32_MAX ? INT32_MAX : box->w > 1 ? (int)box->w : 1;
            int height = (double)box->h >= INT32_MAX ? INT32_MAX : box->h > 1 ? (int)box->h : 1;
            uint64_t start = uptime_ms(), revision = owner->layout_revision;
            web_layout(owner, width, height);
            s->task_layout_ms += uptime_ms() - start;
            if (owner->layout_revision != revision) s->task_layout_flushes++;
        } else flush_layout(s);
    }
    js_free(d->js->ctx, chain);
    return true;
}
static JSValue rendered_inner_text(struct web_js_state *s, node_t *element) {
    JSContext *ctx = s->ctx;
    if (element->type != N_ELEM || element->namespace_id != NS_HTML)
        return JS_ThrowTypeError(ctx, "HTMLElement receiver required");
    web_doc *d = element->owner ? element->owner : s->doc;
    bool connected = d->live && !d->inert && doc_node_connected(element);
    /* Adopted wrappers may belong to a different realm. Flush the owning
       Document, and never reactivate a retired or inert browsing context. */
    if (connected && d->js && !d->js->disabled && !inner_text_flush_owner(d)) return JS_EXCEPTION;
    bool rendered = connected && d->layout_valid && element->box;
    struct inner_text_buffer b = {.ctx=ctx};
    inner_text_collect(&b, element, rendered);
    JSValue result = b.failed ? JS_EXCEPTION : JS_NewStringLen(ctx, b.p ? b.p : "", b.n);
    js_free(ctx, b.p); return result;
}
