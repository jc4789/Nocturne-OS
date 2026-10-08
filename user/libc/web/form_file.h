#pragma once
#include "webi.h"
#define WEB_FILE_COUNT 64
#define WEB_FILE_BYTES (16u << 20)
struct web_form_file {
    char name[256], relative_path[1024], type[96];
    unsigned char *bytes;
    size_t size;
    int64_t last_modified;
};
struct web_form_files {
    unsigned refs, count;
    size_t allocation;
    struct web_form_file files[WEB_FILE_COUNT];
};
void web_input_files_clear(web_doc *d, node_t *input);
void web_input_files_release(node_t *input);
bool web_input_files_replace(web_doc *d, node_t *input, struct web_form_files *files);
bool web_input_files_clone(web_doc *d, node_t *target, node_t *source);
struct web_form_files *web_input_files_create(void);
void web_input_files_dispose(struct web_form_files *files);
bool web_input_file_paths(web_doc *d, web_node *input, const char *const *paths, unsigned count, bool directory);
bool web_input_is_file(web_node *input);
bool web_input_choose_files(web_doc *d, web_node *input);
