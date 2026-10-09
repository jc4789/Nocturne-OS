/* Genuine FACE state belongs to the native node and native form collector. */
#include "form_face.h"
#include <limits.h>

struct web_face_value *web_face_value_create(bool single) {
    struct web_face_value *v = calloc(1, sizeof *v);
    if (v) { v->refs = 1; v->single = single; v->allocation = sizeof *v; }
    return v;
}
bool web_face_value_reserve(struct web_face_value *v, unsigned count) {
    if (!v || v->refs != 1) return false; /* retained native values are immutable */
    if (count <= v->capacity) return true;
    unsigned capacity = v->capacity ? v->capacity : 8;
    while (capacity < count) {
        if (capacity > UINT_MAX / 2) { capacity = count; break; }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof *v->entries) return false;
    size_t extra = (size_t)(capacity - v->capacity) * sizeof *v->entries;
    if (extra > SIZE_MAX - v->allocation) return false;
    struct web_face_entry *entries = realloc(v->entries, (size_t)capacity * sizeof *entries);
    if (!entries) return false;
    memset(entries + v->capacity, 0, extra);
    v->entries = entries; v->capacity = capacity; v->allocation += extra;
    return true;
}
void web_face_value_free(struct web_face_value *v) {
    if (!v || --v->refs) return;
    for (unsigned i = 0; i < v->count; i++) {
        free(v->entries[i].name); free(v->entries[i].filename);
        free(v->entries[i].mime); free(v->entries[i].bytes);
    }
    free(v->entries);
    free(v);
}
static bool face_size(size_t message, struct web_face_value *value,
                      struct web_face_value *state, size_t *out) {
    size_t size = sizeof(struct web_face_state), parts[] = {message,
        value ? value->allocation : 0, state && state != value ? state->allocation : 0};
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) {
        if (parts[i] > SIZE_MAX - size) return false;
        size += parts[i];
    }
    *out = size; return true;
}
static bool face_charge(web_doc *d, node_t *n, size_t next) {
    web_doc *a = n->allocation_doc ? n->allocation_doc : d;
    size_t old = n->internals ? n->internals->allocation : 0;
    if (a->control_bytes < old) return false;
    size_t base = a->control_bytes - old;
    if (next > SIZE_MAX - base) return false;
    a->control_bytes = base + next;
    doc_dom_budget(d);
    return true;
}
bool web_face_prepare(web_doc *d, node_t *n, bool associated) {
    if (!d || !n || n->owner != d || n->type != N_ELEM || n->foreign) return false;
    if (!n->internals) {
        struct web_face_state *f = calloc(1, sizeof *f);
        if (!f) return false;
        if (!face_charge(d, n, sizeof *f)) { free(f); return false; }
        f->allocation = sizeof *f; n->internals = f;
    }
    n->face_associated = associated;
    doc_mutated(d, n); return true;
}
bool web_face_set_value(web_doc *d, node_t *n, struct web_face_value *value, struct web_face_value *state) {
    if (!d || !n || n->owner != d) return false;
    struct web_face_state *f = n->internals;
    if (!f || !n->face_associated) return false;
    if ((value && value->refs > UINT_MAX - (state == value ? 2u : 1u)) ||
        (state && state != value && state->refs == UINT_MAX)) return false;
    size_t size;
    if ((f->message && f->message_length == SIZE_MAX) ||
        !face_size(f->message ? f->message_length + 1 : 0, value, state, &size)) return false;
    if (!face_charge(d, n, size)) return false;
    if (value) value->refs++;
    if (state) state->refs++;
    web_face_value_free(f->value); web_face_value_free(f->state);
    f->value = value; f->state = state; f->allocation = size;
    doc_mutated(d, n); return true;
}
bool web_face_set_validity(web_doc *d, node_t *n, uint32_t flags, const char *message, size_t length) {
    if (!d || !n || n->owner != d) return false;
    struct web_face_state *f = n->internals;
    if (!f || !n->face_associated) return false;
    if (!flags) length = 0;
    if (length == SIZE_MAX || (length && !message)) return false;
    size_t size;
    if (!face_size(length ? length + 1 : 0, f->value, f->state, &size)) return false;
    char *copy = length ? malloc(length + 1) : NULL;
    if (length && !copy) return false;
    if (length) { memcpy(copy, message, length); copy[length] = 0; }
    if (!face_charge(d, n, size)) { free(copy); return false; }
    free(f->message); f->message = copy; f->message_length = length;
    f->flags = flags & 1023u; f->allocation = size;
    doc_mutated(d, n); return true;
}
void web_face_release(node_t *n) {
    if (!n || !n->internals) return;
    struct web_face_state *f = n->internals;
    web_doc *a = n->allocation_doc ? n->allocation_doc : n->owner;
    if (a && a->control_bytes >= f->allocation) a->control_bytes -= f->allocation;
    web_face_value_free(f->value); web_face_value_free(f->state);
    free(f->message); free(f); n->internals = NULL; n->face_associated = false;
}
node_t *web_face_validation_anchor(node_t *n) {
    node_t *anchor=n && n->face_associated && n->internals ? n->internals->anchor : NULL;
    if(anchor){
        for(node_t *p=anchor;p;p=p->parent?p->parent:p->shadow_host)if(p==n)return anchor;
    }
    return n;
}
