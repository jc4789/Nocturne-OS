/* Only the trusted native picker calls the filesystem path entry point.
   Script file assignment transfers immutable bytes, never a local path. */
#include <stdio.h>
#include <limits.h>
#include "nocturne.h"
#include "form_file.h"
#include "form_value.h"
#include "form_validation.h"

struct web_form_files *web_input_files_create(void) {
    struct web_form_files *files = calloc(1, sizeof *files);
    if (files) { files->refs = 1; files->allocation = sizeof *files; }
    return files;
}
bool web_input_files_reserve(struct web_form_files *files, unsigned count) {
    if (!files || files->refs != 1) return false; /* published lists are immutable */
    if (count <= files->capacity) return true;
    unsigned capacity = files->capacity ? files->capacity : 8;
    while (capacity < count) {
        if (capacity > UINT_MAX / 2) { capacity = count; break; }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof *files->files) return false;
    size_t extra = (size_t)(capacity - files->capacity) * sizeof *files->files;
    if (extra > SIZE_MAX - files->allocation) return false;
    struct web_form_file *array = realloc(files->files, (size_t)capacity * sizeof *array);
    if (!array) return false;
    memset(array + files->capacity, 0, extra);
    files->files = array; files->capacity = capacity; files->allocation += extra;
    return true;
}
void web_input_files_dispose(struct web_form_files *files) {
    if (!files || --files->refs) return;
    for (unsigned i = 0; i < files->count; i++) free(files->files[i].bytes);
    free(files->files);
    free(files);
}
void web_input_files_release(node_t *input) {
    if (!input) return;
    web_input_files_dispose(input->files); input->files = NULL;
}
bool web_input_is_file(web_node *input) { return input && input->type == N_ELEM && !input->foreign && input->tag == T_input && web_input_type(input) == WEB_INPUT_FILE; }
bool web_input_files_replace(web_doc *d, node_t *input, struct web_form_files *files) {
    if (!d || !web_input_is_file(input) || input->owner != d) return false;
    web_doc *allocation = input->allocation_doc ? input->allocation_doc : d;
    size_t old = input->files ? input->files->allocation : 0, next = files ? files->allocation : 0;
    if (allocation->control_bytes < old || (files && files->refs == UINT_MAX)) return false;
    size_t base = allocation->control_bytes - old;
    if (next > SIZE_MAX - base) return false;
    if (files) files->refs++;
    web_input_files_release(input); input->files = files;
    input->file_revision++;
    allocation->control_bytes = base + next;
    doc_dom_budget(d);
    doc_mutated(d, input); return true;
}
void web_input_files_clear(web_doc *d, node_t *input) {
    if (!input || !input->files) return;
    web_doc *allocation = input->allocation_doc ? input->allocation_doc : d;
    if (allocation && allocation->control_bytes >= input->files->allocation) allocation->control_bytes -= input->files->allocation;
    web_input_files_release(input); input->file_revision++;
    if (d) { doc_dom_budget(d); doc_mutated(d, input); }
}
bool web_input_files_clone(web_doc *d, node_t *target, node_t *source) { return !source->files || web_input_files_replace(d, target, source->files); }
static const char *file_mime(const char *name) {
    const char *ext = strrchr(name, '.'); if (!ext) return "";
    static const struct { const char *ext, *mime; } known[] = {
        {".txt","text/plain"},{".html","text/html"},{".htm","text/html"},{".json","application/json"},
        {".jpg","image/jpeg"},{".jpeg","image/jpeg"},{".png","image/png"},{".gif","image/gif"},{".webp","image/webp"},
        {".pdf","application/pdf"},{".mp3","audio/mpeg"},{".wav","audio/wav"},{".ogg","audio/ogg"},{".mp4","video/mp4"},{".webm","video/webm"}
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (str_ieq(ext, known[i].ext)) return known[i].mime;
    return "";
}
static bool file_read(struct web_form_files *files, const char *path, const char *relative) {
    if (files->count == UINT_MAX || !web_input_files_reserve(files, files->count + 1)) return false;
    struct n_stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    if (fstat(fd, &st) < 0 || st.type != N_FT_FILE || st.size > SIZE_MAX ||
        (st.size ? (size_t)st.size : 1) > SIZE_MAX - files->allocation) { close(fd); return false; }
    struct web_form_file *file = &files->files[files->count];
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    if (strlen(name) >= sizeof file->name || (relative && strlen(relative) >= sizeof file->relative_path)) { close(fd); return false; }
    file->size = (size_t)st.size; file->bytes = malloc(file->size ? file->size : 1);
    if (!file->bytes) { close(fd); return false; }
    size_t offset = 0;
    while (offset < file->size) { ssize_t n = read(fd, file->bytes + offset, file->size - offset); if (n <= 0) { free(file->bytes); file->bytes = NULL; close(fd); return false; } offset += (size_t)n; }
    struct n_stat after;
    bool stable = fstat(fd, &after) == 0 && after.type == N_FT_FILE && after.size == st.size && after.mtime == st.mtime;
    close(fd);
    if (!stable) { free(file->bytes); file->bytes = NULL; return false; }
    strcpy(file->name, name); strcpy(file->relative_path, relative ? relative : ""); strcpy(file->type, file_mime(name));
    file->last_modified = st.mtime > INT64_MAX / 1000 ? INT64_MAX : st.mtime < 0 ? 0 : st.mtime * 1000;
    files->allocation += file->size ? file->size : 1; files->count++; return true;
}
static bool file_directory(struct web_form_files *files, const char *path, const char *relative, unsigned depth) {
    if (depth > 32) return false;
    int fd = open(path, O_RDONLY); if (fd < 0) return false;
    struct n_dirent entry; bool ok = true; int result = 0;
    for (int index = 0; (result = readdir(fd, index, &entry)) > 0; index++) {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..")) continue;
        char child[1024], child_relative[1024];
        int a = snprintf(child, sizeof child, "%s%s%s", path, !strcmp(path,"/") ? "" : "/", entry.name);
        int b = snprintf(child_relative, sizeof child_relative, "%s/%s", relative, entry.name);
        if (a < 0 || b < 0 || (size_t)a >= sizeof child || (size_t)b >= sizeof child_relative) { ok = false; break; }
        if (entry.type == N_FT_DIR) ok = file_directory(files, child, child_relative, depth + 1);
        else if (entry.type == N_FT_FILE) ok = file_read(files, child, child_relative);
        else continue; // Never upload device/window/pipe nodes.
        if (!ok) break;
    }
    close(fd); return ok && result >= 0;
}
bool web_input_file_paths(web_doc *d, web_node *input, const char *const *paths, unsigned count, bool directory) {
    if (!d || !web_input_is_file(input) || web_control_disabled(input) || !paths || !count) return false;
    if (directory && (!node_attr(input, "webkitdirectory") || count != 1)) return false;
    if (!directory && !node_attr(input, "multiple") && count != 1) return false;
    struct web_form_files *files = web_input_files_create(); if (!files) return false;
    bool ok = true;
    for (unsigned i = 0; i < count && ok; i++) {
        if (!paths[i] || paths[i][0] != '/') { ok = false; break; }
        if (directory) { const char *name = strrchr(paths[i], '/'); name = name ? name + 1 : paths[i]; if (!*name) name = "root"; ok = file_directory(files, paths[i], name, 0); }
        else ok = file_read(files, paths[i], NULL);
    }
    if (ok) ok = web_input_files_replace(d, input, files);
    web_input_files_dispose(files); return ok;
}
