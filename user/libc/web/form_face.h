#pragma once
#include "webi.h"
#define WEB_FACE_ENTRIES 64u
#define WEB_FACE_BYTES (16u << 20)
struct web_face_entry {
    char *name, *filename, *mime;
    unsigned char *bytes;
    size_t name_length, filename_length, size;
    int64_t last_modified;
    bool file;
};
struct web_face_value {
    unsigned refs, count;
    bool single;
    size_t allocation;
    struct web_face_entry entries[WEB_FACE_ENTRIES];
};
struct web_face_state {
    bool attached;
    uint32_t flags;
    char *message;
    size_t message_length, allocation;
    node_t *anchor;
    struct web_face_value *value, *state;
};
struct web_face_value *web_face_value_create(bool single);
void web_face_value_free(struct web_face_value *value);
bool web_face_prepare(web_doc *d, node_t *n, bool associated);
bool web_face_set_value(web_doc *d, node_t *n, struct web_face_value *value, struct web_face_value *state);
bool web_face_set_validity(web_doc *d, node_t *n, uint32_t flags, const char *message, size_t length);
void web_face_release(node_t *n);
node_t *web_face_validation_anchor(node_t *n);
