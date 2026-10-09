/* Native address chrome must not confuse a child URL with the parent URL. */
static bool frame_address_live(web_doc *top, web_doc *child) {
    if (!top || !top->live || top->inert || !child || !child->live || child->inert) return false;
    for (web_doc *d = child; d != top; d = d->frame_parent) {
        if (!d->frame_parent || !d->frame_parent->live || d->frame_parent->inert ||
            !d->frame_element || !doc_node_connected(d->frame_element)) return false;
        struct web_frame *f = web_frame_find(d->frame_parent, d->frame_element);
        if (!f || f->detached || f->document != d) return false;
    }
    return true;
}
static web_doc *frame_address_document(web_doc *top, node_t *element) {
    if (!web_frame_element(element) || !element->owner || !doc_node_connected(element) ||
        !frame_address_live(top, element->owner)) return NULL;
    struct web_frame *f = web_frame_find(element->owner, element);
    return f && !f->detached && frame_address_live(top, f->document) ? f->document : NULL;
}
void web_frame_address_select(web_doc *top, web_node *node) {
    if (!node || !node->owner || !doc_node_connected(node)) return;
    web_doc *child = node->owner;
    if (child != top && frame_address_live(top, child)) top->address_frame = child->frame_element;
}
const char *web_frame_address(web_doc *top) {
    if (!top || !top->live || top->inert) return NULL;
    web_doc *child = frame_address_document(top, top->address_frame);
    if (child) return web_url(child);
    top->address_frame = NULL;
    /* A frameset's large content pane has a useful URL even before a click.
       Ordinary iframe-heavy pages do not arbitrarily select an advertisement. */
    if (!top->body || top->body->foreign || top->body->tag != T_frameset) return NULL;
    double best = 0;
    for (struct web_frame *f = top->frames; f; f = f->next) {
        node_t *n = f->element;
        web_doc *candidate = frame_address_document(top, n);
        if (!candidate || !n->box || !n->style || n->style->visibility ||
            !(n->box->w > 0) || !(n->box->h > 0)) continue;
        double area = (double)n->box->w * n->box->h;
        if (area > best) { best = area; child = candidate; top->address_frame = n; }
    }
    return child ? web_url(child) : NULL;
}
