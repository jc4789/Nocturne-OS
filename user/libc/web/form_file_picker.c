/* Native Nocturne window picker. No author-supplied local path or UNIX layer.
   Called only from the browser's trusted input dispatch, not DOM .click().
   It is a window of the UI engine (ui.h), modal while the browser waits for it. */
#include <stdio.h>
#include "nocturne.h"
#include "ui.h"
#include "form_file.h"
#include "form_value.h"
#include "form_validation.h"

struct picker_entry { char name[256]; uint32_t kind; bool selected; };
struct picker_state {
    struct picker_entry *entries;
    int count, capacity, cursor;
    uint32_t folder;  /* counts folders shown: the list's key, so a new folder starts at its top */
    uint32_t reveal;  /* counts keyboard moves: the cursor's row is scrolled into view */
    char path[1024], message[160];
};
static int picker_compare(const void *a, const void *b) {
    const struct picker_entry *x = a, *y = b;
    if ((x->kind == N_KIND_FOLDER) != (y->kind == N_KIND_FOLDER)) return x->kind == N_KIND_FOLDER ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void picker_load(struct picker_state *picker) {
    picker->count = 0; picker->cursor = -1; picker->folder++;
    struct n_entry *entries;
    n_result count = n_folder_entries(picker->path, &entries);
    if (count < 0) { strcpy(picker->message, "Cannot open this folder"); return; }
    for (n_result index = 0; index < count; index++) {
        if (entries[index].kind != N_KIND_DOCUMENT && entries[index].kind != N_KIND_FOLDER) continue;
        if (picker->count == picker->capacity) {
            int capacity = picker->capacity ? picker->capacity * 2 : 64;
            void *grown = capacity > picker->capacity && (size_t)capacity <= SIZE_MAX / sizeof *picker->entries ?
                realloc(picker->entries, (size_t)capacity * sizeof *picker->entries) : NULL;
            if (!grown) { strcpy(picker->message, "Not enough memory to list this whole folder"); break; }
            picker->entries = grown; picker->capacity = capacity;
        }
        struct picker_entry *dest = &picker->entries[picker->count++];
        strlcpy(dest->name, entries[index].name, sizeof dest->name); dest->kind = entries[index].kind; dest->selected = false;
    }
    free(entries);
    if (picker->count) qsort(picker->entries, picker->count, sizeof *picker->entries, picker_compare);
    snprintf(picker->message, sizeof picker->message, "%d entries", picker->count);
}
/* The root has no parent: it stays where it is. */
static void picker_up(struct picker_state *picker) {
    char parent[sizeof picker->path];
    if (n_path_join(picker->path, "..", parent, sizeof parent) >= 0 && strcmp(parent, picker->path)) { strcpy(picker->path, parent); picker_load(picker); }
}
/* The places of the view's buttons and rows, for the events that name them. */
struct picker_places { uint32_t up, cancel, choose, first; };
static struct picker_places picker_show(struct ui_session *s, struct picker_state *picker, bool directory) {
    const struct ui_theme *t = &ui_theme;
    struct picker_places p = {0};
    struct ui_msg m;
    ui_view(&m, 1);
    ui_node(&m, "column");
        ui_node(&m, "row"); ui_nums(&m, "padding", (double[]){10, 12}, 2); ui_num(&m, "gap", 8);
        ui_str(&m, "align", "center"); ui_num(&m, "background", t->sidebar);
            p.up = ui_node(&m, "button"); ui_str(&m, "icon", "chevron-left"); ui_str(&m, "variant", "ghost");
            ui_num(&m, "compact", 1); if (!*n_path_name(picker->path)) ui_num(&m, "disabled", 1); ui_end(&m);
            ui_node(&m, "icon"); ui_str(&m, "icon", "folder"); ui_num(&m, "size", 16); ui_num(&m, "colour", t->textDim); ui_end(&m);
            ui_node(&m, "text"); ui_str(&m, "text", picker->path); ui_num(&m, "ellipsis", 1); ui_num(&m, "grow", 1); ui_end(&m);
            p.cancel = ui_node(&m, "button"); ui_str(&m, "text", "Cancel"); ui_str(&m, "variant", "secondary"); ui_end(&m);
            p.choose = ui_node(&m, "button"); ui_str(&m, "text", directory ? "Choose folder" : "Choose files");
            ui_str(&m, "variant", "primary"); ui_end(&m);
        ui_end(&m);
        ui_node(&m, "divider"); ui_end(&m);
        ui_node(&m, "scroll"); ui_num(&m, "key", picker->folder); ui_num(&m, "grow", 1);
            ui_node(&m, "column"); ui_nums(&m, "padding", (double[]){6, 8}, 2); ui_num(&m, "gap", 2);
            for (int i = 0; i < picker->count; i++) {
                const struct picker_entry *entry = &picker->entries[i];
                uint32_t place = ui_node(&m, "item");
                if (!i) p.first = place;
                ui_str(&m, "text", entry->name);
                if (entry->kind == N_KIND_FOLDER || entry->selected) ui_str(&m, "icon", entry->kind == N_KIND_FOLDER ? "folder" : "check");
                if (entry->selected || i == picker->cursor) ui_num(&m, "selected", 1);
                if (i == picker->cursor) ui_num(&m, "reveal", picker->reveal);
                ui_end(&m);
            }
            ui_end(&m);
        ui_end(&m);
        ui_node(&m, "divider"); ui_end(&m);
        ui_node(&m, "text"); ui_str(&m, "text", picker->message); ui_str(&m, "style", "caption");
        ui_nums(&m, "padding", (double[]){8, 14}, 2); ui_num(&m, "ellipsis", 1); ui_end(&m);
    ui_end(&m);
    ui_send(s, &m);
    return p;
}
static bool picker_commit(web_doc *d, node_t *input, struct picker_state *picker, bool directory) {
    if (directory) {
        char path[1024]; strlcpy(path, picker->path, sizeof path);
        if (picker->cursor >= 0 && picker->entries[picker->cursor].kind == N_KIND_FOLDER && n_path_join(picker->path, picker->entries[picker->cursor].name, path, sizeof path) < 0) return false;
        const char *paths[] = {path}; return web_input_file_paths(d, input, paths, 1, true);
    }
    unsigned count = 0;
    for (int index = 0; index < picker->count; index++)
        if (picker->entries[index].selected && picker->entries[index].kind == N_KIND_DOCUMENT) count++;
    if (!count) return false;
    char (*storage)[1024] = malloc((size_t)count * 1024);
    const char **paths = malloc((size_t)count * sizeof *paths);
    bool ok = storage && paths;
    unsigned used = 0;
    for (int index = 0; ok && index < picker->count; index++) {
        struct picker_entry *entry = &picker->entries[index]; if (!entry->selected || entry->kind != N_KIND_DOCUMENT) continue;
        if (n_path_join(picker->path, entry->name, storage[used], 1024) < 0) { ok = false; break; }
        paths[used] = storage[used]; used++;
    }
    ok = ok && web_input_file_paths(d, input, paths, used, false);
    free(storage); free(paths); return ok;
}
bool web_input_choose_files(web_doc *d, web_node *input) {
    if (!d || !web_input_is_file(input) || input->owner != d || web_control_disabled(input)) return false;
    bool directory = node_attr(input, "webkitdirectory") != NULL, multiple = node_attr(input, "multiple") != NULL;
    struct picker_state *picker = calloc(1, sizeof *picker); if (!picker) return false;
    n_path_join(n_user_folder(), ".", picker->path, sizeof picker->path); picker_load(picker);
    struct ui_session *s = ui_session_new();
    if (!s || ui_window(s, 1, 640, 460, WIN_RESIZABLE | WIN_CENTER, directory ? "Select upload folder" : "Select upload files", NULL, NULL) < 0) {
        ui_session_free(s); free(picker->entries); free(picker); return false;
    }
    bool chosen = false, done = false; uint64_t last_click = 0; int last_index = -1;
    uint32_t views = 0, folder_view = 0, shown_folder = 0; /* a row named by a view of an older folder is not a row of this one */
    while (!done) {
        struct picker_places places = picker_show(s, picker, directory);
        views++;
        if (picker->folder != shown_folder) { shown_folder = picker->folder; folder_view = views; }
        if (!ui_session_wait(s, -1)) continue;
        struct ui_event event;
        char text[16];
        while (!done && ui_next_event(s, &event, text, sizeof text)) {
            bool commit = false, enter = false;
            if (event.type == UI_EVENT_CLOSE) { done = true; break; }
            if (event.type == UI_EVENT_CLICK) {
                if (event.node == places.up) picker_up(picker);
                else if (event.node == places.cancel) { done = true; break; }
                else if (event.node == places.choose) commit = true;
                else if (places.first && event.view >= folder_view && event.node >= places.first && event.node < places.first + (uint32_t)picker->count) {
                    int index = (int)(event.node - places.first);
                    if (!multiple || !(event.mods & NMOD_CTRL)) for (int i = 0; i < picker->count; i++) if (i != index) picker->entries[i].selected = false;
                    picker->cursor = index; picker->entries[index].selected = !picker->entries[index].selected;
                    uint64_t now = uptime_ms(); enter = index == last_index && now - last_click < 450;
                    last_click = now; last_index = index;
                }
            } else if (event.type == UI_EVENT_KEY && !(event.flags & UI_UP)) {
                uint32_t key = (uint32_t)event.a;
                if (key == NKEY_ESC) { done = true; break; }
                if (key == NKEY_BACKSPACE) picker_up(picker);
                else if (key == NKEY_UP && picker->count) { picker->cursor = MAX(0, picker->cursor - 1); picker->reveal++; }
                else if (key == NKEY_DOWN && picker->count) { picker->cursor = MIN(picker->count - 1, picker->cursor + 1); picker->reveal++; }
                else if (key == NKEY_ENTER) enter = true;
                else if (key == ' ' && picker->cursor >= 0) {
                    if (!multiple) for (int i = 0; i < picker->count; i++) if (i != picker->cursor) picker->entries[i].selected = false;
                    picker->entries[picker->cursor].selected = !picker->entries[picker->cursor].selected;
                }
            }
            if (enter && picker->cursor >= 0 && picker->cursor < picker->count) {
                struct picker_entry *entry = &picker->entries[picker->cursor];
                if (entry->kind == N_KIND_FOLDER) {
                    char path[1024];
                    if (n_path_join(picker->path, entry->name, path, sizeof path) >= 0) { strcpy(picker->path, path); picker_load(picker); last_index = -1; }
                } else if (!directory) { entry->selected = true; commit = true; }
            }
            if (commit) {
                if (picker_commit(d, input, picker, directory)) { chosen = true; done = true; break; }
                strcpy(picker->message, "Selection failed: regular files, picker metadata and available memory required");
            }
        }
    }
    ui_session_free(s); free(picker->entries); free(picker); return chosen;
}
