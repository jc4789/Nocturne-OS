/* Native Nocturne window picker. No author-supplied local path or UNIX layer.
   Called only from the browser's trusted input dispatch, not DOM .click(). */
#include <stdio.h>
#include "nocturne.h"
#include "form_file.h"
#include "form_value.h"
#include "form_validation.h"
#define NATIVE_PICKER_FILE_COUNT 64 /* trusted picker selection storage, not script FileList quota */

struct picker_entry { char name[256]; uint32_t type; bool selected; };
struct picker_state { struct picker_entry entries[512]; char path[1024], message[160]; int count, cursor, top; };
static int picker_compare(const void *a, const void *b) {
    const struct picker_entry *x = a, *y = b;
    if ((x->type == N_FT_DIR) != (y->type == N_FT_DIR)) return x->type == N_FT_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static bool picker_join(char *out, size_t capacity, const char *path, const char *name) {
    int length = snprintf(out, capacity, "%s%s%s", path, !strcmp(path, "/") ? "" : "/", name);
    return length > 0 && (size_t)length < capacity;
}
static void picker_load(struct picker_state *picker) {
    picker->count = 0; picker->top = 0; picker->cursor = -1;
    int fd = open(picker->path, O_RDONLY);
    if (fd < 0) { strcpy(picker->message, "Cannot open this folder"); return; }
    struct n_dirent entry;
    for (int index = 0; picker->count < 512 && readdir(fd, index, &entry) > 0; index++) {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, "..") || (entry.type != N_FT_FILE && entry.type != N_FT_DIR)) continue;
        struct picker_entry *dest = &picker->entries[picker->count++];
        strlcpy(dest->name, entry.name, sizeof dest->name); dest->type = entry.type; dest->selected = false;
    }
    close(fd); qsort(picker->entries, picker->count, sizeof *picker->entries, picker_compare);
    snprintf(picker->message, sizeof picker->message, "%d entries; select up to %u files", picker->count, NATIVE_PICKER_FILE_COUNT);
}
static void picker_up(struct picker_state *picker) {
    char *slash = strrchr(picker->path, '/');
    if (slash) { if (slash == picker->path) slash[1] = 0; else *slash = 0; picker_load(picker); }
}
static void picker_draw(window_t *window, struct picker_state *picker, bool directory) {
    canvas_t *canvas = &window->c;
    gfx_fill(canvas, 0, 0, window->w, window->h, RGB(26, 24, 44));
    ui_button(canvas, 8, 8, 64, 28, "Up", false, false);
    ui_button(canvas, window->w - 212, 8, 112, 28, directory ? "Choose folder" : "Choose files", false, false);
    ui_button(canvas, window->w - 92, 8, 84, 28, "Cancel", false, false);
    canvas_t clip = *canvas; gfx_clip(&clip, 8, 44, window->w - 16, 20);
    gfx_text(&clip, 8, 44, picker->path, RGB(220, 220, 230), TRANSPARENT, FONT_SMALL);
    int rows = MAX(1, (window->h - 100) / 24);
    for (int row = 0; row < rows && picker->top + row < picker->count; row++) {
        int index = picker->top + row, y = 70 + row * 24; struct picker_entry *entry = &picker->entries[index];
        if (index == picker->cursor) gfx_fill(canvas, 4, y, window->w - 8, 24, RGB(84, 70, 170));
        gfx_text(canvas, 10, y + 4, entry->type == N_FT_DIR ? "[dir]" : entry->selected ? "[x]" : "[ ]", RGB(180, 180, 220), TRANSPARENT, FONT_SMALL);
        clip = *canvas; gfx_clip(&clip, 58, y, window->w - 66, 24);
        gfx_text(&clip, 58, y + 4, entry->name, RGB(238, 238, 244), TRANSPARENT, FONT_SMALL);
    }
    gfx_text(canvas, 8, window->h - 22, picker->message, RGB(190, 190, 210), TRANSPARENT, FONT_SMALL);
    win_update(window);
}
static bool picker_commit(web_doc *d, node_t *input, struct picker_state *picker, bool directory) {
    if (directory) {
        char path[1024]; strlcpy(path, picker->path, sizeof path);
        if (picker->cursor >= 0 && picker->entries[picker->cursor].type == N_FT_DIR && !picker_join(path, sizeof path, picker->path, picker->entries[picker->cursor].name)) return false;
        const char *paths[] = {path}; return web_input_file_paths(d, input, paths, 1, true);
    }
    char (*storage)[1024] = malloc(NATIVE_PICKER_FILE_COUNT * 1024); if (!storage) return false;
    const char *paths[NATIVE_PICKER_FILE_COUNT]; unsigned count = 0; bool ok = true;
    for (int index = 0; index < picker->count; index++) {
        struct picker_entry *entry = &picker->entries[index]; if (!entry->selected || entry->type != N_FT_FILE) continue;
        if (count >= NATIVE_PICKER_FILE_COUNT || !picker_join(storage[count], 1024, picker->path, entry->name)) { ok = false; break; }
        paths[count] = storage[count]; count++;
    }
    ok = ok && count && web_input_file_paths(d, input, paths, count, false); free(storage); return ok;
}
bool web_input_choose_files(web_doc *d, web_node *input) {
    if (!d || !web_input_is_file(input) || input->owner != d || web_control_disabled(input)) return false;
    bool directory = node_attr(input, "webkitdirectory") != NULL, multiple = node_attr(input, "multiple") != NULL;
    struct picker_state *picker = calloc(1, sizeof *picker); if (!picker) return false;
    strcpy(picker->path, "/home"); picker_load(picker);
    window_t *window = win_open(620, 440, directory ? "Select upload folder" : "Select upload files", WIN_RESIZABLE);
    if (!window) { free(picker); return false; }
    bool chosen = false; uint64_t last_click = 0; int last_index = -1;
    for (;;) {
        picker_draw(window, picker, directory); struct gui_event event;
        if (win_event(window, &event, -1) < 0 || event.type == EV_CLOSE || (event.type == EV_KEY && event.pressed && event.key == NKEY_ESC)) break;
        bool commit = false, enter = false;
        if (event.type == EV_MOUSE_UP && event.y < 40) {
            if (event.x < 80) picker_up(picker);
            else if (event.x >= window->w - 92) break;
            else if (event.x >= window->w - 212) commit = true;
        } else if (event.type == EV_MOUSE_DOWN && event.y >= 70 && event.y < window->h - 30) {
            int index = picker->top + (event.y - 70) / 24;
            if (index < picker->count) {
                if (!multiple || !(event.mods & NMOD_CTRL)) for (int i = 0; i < picker->count; i++) picker->entries[i].selected = false;
                picker->cursor = index; picker->entries[index].selected = !picker->entries[index].selected;
                uint64_t now = uptime_ms(); enter = index == last_index && now - last_click < 450;
                last_click = now; last_index = index;
            }
        } else if (event.type == EV_KEY && event.pressed) {
            if (event.key == NKEY_BACKSPACE) picker_up(picker);
            else if (event.key == NKEY_UP) picker->cursor = MAX(0, picker->cursor - 1);
            else if (event.key == NKEY_DOWN) picker->cursor = MIN(picker->count - 1, picker->cursor + 1);
            else if (event.key == NKEY_ENTER) enter = true;
            else if (event.key == ' ' && picker->cursor >= 0) {
                if (!multiple) for (int i = 0; i < picker->count; i++) if (i != picker->cursor) picker->entries[i].selected = false;
                picker->entries[picker->cursor].selected = !picker->entries[picker->cursor].selected;
            }
        } else if (event.type == EV_WHEEL) picker->top = MAX(0, picker->top + event.wheel * 3);
        if (enter && picker->cursor >= 0 && picker->cursor < picker->count) {
            struct picker_entry *entry = &picker->entries[picker->cursor];
            if (entry->type == N_FT_DIR) { char path[1024]; if (picker_join(path, sizeof path, picker->path, entry->name)) { strcpy(picker->path, path); picker_load(picker); } }
            else if (!directory) { entry->selected = true; commit = true; }
        }
        if (commit) { if (picker_commit(d, input, picker, directory)) { chosen = true; break; } strcpy(picker->message, "Selection failed: regular files, picker metadata and available memory required"); }
        int rows = MAX(1, (window->h - 100) / 24);
        picker->top = MAX(0, MIN(picker->top, picker->count - rows));
        if (picker->cursor >= 0) { if (picker->cursor < picker->top) picker->top = picker->cursor; else if (picker->cursor >= picker->top + rows) picker->top = picker->cursor - rows + 1; }
    }
    win_close(window); free(picker); return chosen;
}
