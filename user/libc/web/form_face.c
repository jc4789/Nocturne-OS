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
                      struct web_face_value *state, size_t states, size_t *out) {
    size_t size = sizeof(struct web_face_state), parts[] = {message,
        value ? value->allocation : 0, state && state != value ? state->allocation : 0, states};
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
        !face_size(f->message ? f->message_length + 1 : 0, value, state, f->custom_states_length, &size)) return false;
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
    if (!face_size(length ? length + 1 : 0, f->value, f->state, f->custom_states_length, &size)) return false;
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
    free(f->message); free(f->custom_states); free(f); n->internals = NULL; n->face_associated = false;
}
bool web_custom_state_has(const node_t *n, const char *name) {
    const struct web_face_state *f = n ? n->internals : NULL;
    if (!f || !f->custom_states) return false;
    for (const char *p = f->custom_states, *e = p + f->custom_states_length; p < e; p += strlen(p) + 1)
        if (!strcmp(p, name)) return true;
    return false;
}
bool web_custom_state_change(web_doc *d, node_t *n, char op, const char *name, size_t length) {
    struct web_face_state *f = n ? n->internals : NULL;
    if (!d || !f) return false;
    /* A name holding NUL never matches :state(<ident>); only the CustomStateSet keeps it. */
    if (op != 'c' && (!name || memchr(name, 0, length))) return true;
    size_t old = f->custom_states_length, next = old, at = old;
    if (op == 'c') next = 0;
    else {
        for (const char *p = f->custom_states, *e = p ? p + old : NULL; p && p < e; p += strlen(p) + 1)
            if (strlen(p) == length && !memcmp(p, name, length)) { at = (size_t)(p - f->custom_states); break; }
        if (op == 'a' ? at < old : at == old) return true; /* already in, or not in */
        if (op == 'a' && length >= SIZE_MAX - old) return false;
        next = op == 'a' ? old + length + 1 : old - length - 1;
    }
    if (op == 'a') {
        char *grown = realloc(f->custom_states, next);
        if (!grown) return false;
        f->custom_states = grown;
    }
    size_t size;
    if (!face_size(f->message ? f->message_length + 1 : 0, f->value, f->state, next, &size) ||
        !face_charge(d, n, size)) return false;
    if (op == 'c') { free(f->custom_states); f->custom_states = NULL; }
    else if (op == 'a') { memcpy(f->custom_states + old, name, length); f->custom_states[old + length] = 0; }
    else memmove(f->custom_states + at, f->custom_states + at + length + 1, old - at - length - 1);
    f->custom_states_length = next; f->allocation = size;
    css_mark_dirty(d, n);
    return true;
}
node_t *web_face_validation_anchor(node_t *n) {
    node_t *anchor=n && n->face_associated && n->internals ? n->internals->anchor : NULL;
    if(anchor){
        for(node_t *p=anchor;p;p=p->parent?p->parent:p->shadow_host)if(p==n)return anchor;
    }
    return n;
}
