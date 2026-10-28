/* Nocturne search completion: private, opt-in native storage. The
   page has no history API and receives a past value only after the user chooses
   it. This is not a password manager or personal/contact/payment autofill. Whether
   it is on is one of the browser's preferences (webprefs.h), which the browser's
   settings page (about:settings, user/apps/browser_settings.h) changes. */
#include <stdio.h>
#include <nocturne.h>
#include <bearssl_hash.h>
#include <webprefs.h>
#include <webstorage.h>
#include <weburl.h>
#include "form_autocomplete.h"
#include "form_value.h"

#define HISTORY_PATH 1024
#define HISTORY_ORIGIN 192
#define HISTORY_NAME 64
#define HISTORY_VALUE 256
#define HISTORY_HEADER 56
/* Smallest serialized entry: three sizes plus three non-empty fields. */
#define HISTORY_ENTRY_MIN (12 + 3)
struct search_entry { char origin[HISTORY_ORIGIN], name[HISTORY_NAME], value[HISTORY_VALUE]; };
/* Entry count is dynamic (MRU first): storage and memory are the only limits. */
struct search_history { uint64_t generation; int count, capacity, slot; struct search_entry *entries; };
static struct {
    web_doc *doc;
    web_node *input;
    char origin[HISTORY_ORIGIN], name[HISTORY_NAME];
    /* The browser's popup labels point here, so the buffer only grows and is
       never released while a list may still be painted. */
    char (*values)[HISTORY_VALUE];
    int count, capacity;
} popup;
static void popup_reset(void) {
    popup.doc = NULL; popup.input = NULL; popup.count = 0; popup.origin[0] = popup.name[0] = 0;
}
static void history_free(struct search_history *history) { if (history) free(history->entries); free(history); }
static bool history_reserve(struct search_history *history, int count) {
    if (count <= history->capacity) return true;
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof *history->entries) return false;
    int capacity = history->capacity ? history->capacity : 16;
    while (capacity < count) capacity = capacity > INT32_MAX / 2 ? count : capacity * 2;
    if ((size_t)capacity > SIZE_MAX / sizeof *history->entries) capacity = count;
    void *grown = realloc(history->entries, (size_t)capacity * sizeof *history->entries);
    if (!grown) return false;
    history->entries = grown; history->capacity = capacity; return true;
}

/* The serialization of url's origin when it is the tuple origin of an http(s) URL (the URL
   record's owner gives it). Opaque origins, other schemes, out of memory and an origin longer
   than an entry holds have none: no entry is kept for them. */
static bool history_origin(const char *url, char out[HISTORY_ORIGIN]) {
    char *origin = NULL;
    bool ok = url && web_url_origin(url, &origin) == WEB_URL_TUPLE && web_origin_http(origin) &&
              strlen(origin) < HISTORY_ORIGIN;
    if (ok) memcpy(out, origin, strlen(origin) + 1);
    free(origin); return ok;
}
static bool contains_ascii(const char *value, const char *needle) {
    size_t n = strlen(needle);
    for (; *value; value++) if (!strncasecmp(value, needle, n)) return true;
    return false;
}
static bool sensitive(const char *value) {
    static const char *const terms[] = {"password", "passwd", "passcode", "secret", "token", "authorization", "bearer", "credential", "api-key", "apikey", "private-key", "one-time", "otp", "credit-card", "cc-number", "cc-csc", "cvv", "cvc", "ssn", "pin-code"};
    for (size_t i = 0; i < sizeof terms / sizeof *terms; i++) if (contains_ascii(value, terms[i])) return true;
    return false;
}
static bool history_name(const char *name) {
    if (!name || !*name || strlen(name) >= HISTORY_NAME || sensitive(name)) return false;
    static const char *const denied[] = {"pass", "pin", "auth", "login", "username", "email", "account", "card", "payment", "security", "session", "key"};
    for (size_t i = 0; i < sizeof denied / sizeof *denied; i++) if (contains_ascii(name, denied[i])) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) if (*p < 33 || *p > 126) return false;
    return true;
}
static bool history_value(const char *value) {
    if (!value || !*value || strlen(value) >= HISTORY_VALUE || sensitive(value)) return false;
    /* Reject obvious credentials, emails, encoded payloads and long identifiers.
       Search intent cannot prove arbitrary text non-secret: opt-in remains
       essential, and this deliberately conservative filter may omit searches. */
    int digits = 0, opaque = 0; bool nonspace = false, numeric = true;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 32 || *p == 127 || strchr("@=\\", *p)) return false;
        if (*p != ' ') nonspace = true;
        if (!(*p >= '0' && *p <= '9') && *p != ' ' && *p != '-') numeric = false;
        if (*p >= '0' && *p <= '9') { if (++digits >= 13) return false; }
        else if (*p != ' ' && *p != '-') digits = 0;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            if (++opaque >= 24) return false;
        } else opaque = 0;
    }
    return nonspace && !numeric;
}
static bool history_attribute(const char *value) {
    if (!value) return true;
    while (is_space((unsigned char)*value)) value++;
    size_t n = strlen(value); while (n && is_space((unsigned char)value[n - 1])) n--;
    /* Only automatic search completion. Personal/contact/credential field
       tokens (including unknown tokens) never enter this history. */
    return !n || strn_ieq(value, "on", n);
}
static bool history_control(web_doc *d, node_t *input, char origin[HISTORY_ORIGIN], const char **name) {
    if (!d || !input || input->owner != d || input->type != N_ELEM || input->foreign ||
        input->tag != T_input || web_input_type(input) != WEB_INPUT_SEARCH ||
        !doc_node_connected(input) || web_control_disabled(input) || node_attr(input, "readonly")) return false;
    node_t *form = web_form_owner(d, input);
    if (!history_attribute(node_attr(input, "autocomplete")) ||
        (form && !history_attribute(node_attr(form, "autocomplete")))) return false;
    *name = node_attr(input, "name");
    return history_name(*name) && history_origin(d->url, origin);
}
static void history_hash(const void *bytes, size_t n, uint8_t digest[32]) {
    br_sha256_context context; br_sha256_init(&context);
    br_sha256_update(&context, bytes, n); br_sha256_out(&context, digest);
}
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t value) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i)); }
/* The complete path of a document of the history, in the browser's profile folder; false without
   the folder or when it does not fit. */
static bool history_file(const char *name, char path[HISTORY_PATH]) {
    const char *root = web_profile_folder(false);
    return root && n_path_join(root, name, path, HISTORY_PATH) >= 0;
}
static bool history_path(int slot, char path[HISTORY_PATH]) {
    char name[32]; snprintf(name, sizeof name, "search-history.%d", slot); return history_file(name, path);
}
static bool history_enabled(void) {
    /* For every native operation, a change that waits is taken first: turning the history off
       on the settings page must not leave this process recording or offering. Off until the
       user turned it on. */
    webprefs_sync();
    return webprefs_search_history();
}
/* 0 missing, 1 valid, -1 invalid/I/O. No unchecked raw struct serialization. */
static int history_read(int slot, struct search_history *out) {
    char path[HISTORY_PATH]; void *data; size_t n;
    if (!history_path(slot, path)) return -1;
    n_result loaded = n_document_load(path, &data, &n);
    if (loaded < 0) return loaded == -N_ERR_NOT_FOUND ? 0 : -1;
    uint8_t *bytes = data; uint8_t digest[32];
    if (n < HISTORY_HEADER) { if (data) n_memory_release(data, n); return -1; }
    history_hash(bytes + 40, n - 40, digest);
    bool ok = !memcmp(bytes, "NSEARCH1", 8) && !memcmp(bytes + 8, digest, 32);
    uint64_t generation = get32(bytes + 40) | (uint64_t)get32(bytes + 44) << 32;
    unsigned count = get32(bytes + 48);
    ok = ok && generation && count <= (n - HISTORY_HEADER) / HISTORY_ENTRY_MIN && count <= INT32_MAX && !get32(bytes + 52);
    out->count = 0; out->generation = generation; out->slot = slot;
    if (ok && !history_reserve(out, (int)count)) ok = false;
    size_t at = HISTORY_HEADER;
    for (unsigned i = 0; ok && i < count; i++) {
        if (n - at < 12) { ok = false; break; }
        size_t sizes[3] = {get32(bytes + at), get32(bytes + at + 4), get32(bytes + at + 8)}; at += 12;
        struct search_entry *entry = &out->entries[i]; memset(entry, 0, sizeof *entry);
        char *fields[3] = {entry->origin, entry->name, entry->value};
        size_t caps[3] = {HISTORY_ORIGIN, HISTORY_NAME, HISTORY_VALUE};
        for (int j = 0; j < 3; j++) {
            size_t size = sizes[j];
            if (!size || size >= caps[j] || size > n - at || memchr(bytes + at, 0, size)) { ok = false; break; }
            memcpy(fields[j], bytes + at, size); fields[j][size] = 0; at += size;
        }
        char origin[HISTORY_ORIGIN];
        if (ok && (!history_origin(entry->origin, origin) || strcmp(origin, entry->origin) ||
                   !history_name(entry->name) || !history_value(entry->value))) ok = false;
    }
    ok = ok && at == n; n_memory_release(data, n);
    if (!ok) return -1; out->count = (int)count; return 1;
}
static struct search_history *history_load(void) {
    struct search_history *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    if (!a || !b) { free(a); free(b); return NULL; }
    int ar = history_read(0, a), br = history_read(1, b);
    if (br == 1 && (ar != 1 || b->generation > a->generation)) { struct search_history *temp = a; a = b; b = temp; ar = 1; }
    history_free(b);
    if (ar != 1) {
        if (ar < 0 || br < 0) { history_free(a); return NULL; }
        a->count = 0; a->generation = 0; a->slot = -1;
    }
    return a;
}
static bool history_directory(void) {
    const char *root = web_profile_folder(false);
    struct n_entry entry;
    if (!root) return false;
    n_result found = n_entry_info(root, &entry);
    if (found >= 0) return entry.kind == N_KIND_FOLDER;
    if (found != -N_ERR_NOT_FOUND) return false;
    n_result made = n_folder_create(root);
    if (made < 0 && made != -N_ERR_EXISTS) return false;
    /* Only the folder itself is made again; the user's AppData above it is not manufactured here. */
    return n_entry_info(root, &entry) >= 0 && entry.kind == N_KIND_FOLDER;
}
static bool history_save(struct search_history *history) {
    if (history->generation == UINT64_MAX || !history_directory()) return false;
    size_t n = HISTORY_HEADER;
    for (int i = 0; i < history->count; i++) n += 12 + strlen(history->entries[i].origin) + strlen(history->entries[i].name) + strlen(history->entries[i].value);
    uint8_t *bytes = calloc(1, n); if (!bytes) return false;
    uint64_t generation = history->generation + 1;
    memcpy(bytes, "NSEARCH1", 8); put32(bytes + 40, (uint32_t)generation); put32(bytes + 44, (uint32_t)(generation >> 32)); put32(bytes + 48, history->count);
    size_t at = HISTORY_HEADER;
    for (int i = 0; i < history->count; i++) {
        struct search_entry *entry = &history->entries[i]; const char *fields[] = {entry->origin, entry->name, entry->value};
        size_t sizes[] = {strlen(fields[0]), strlen(fields[1]), strlen(fields[2])};
        for (int j = 0; j < 3; j++) put32(bytes + at + 4 * j, (uint32_t)sizes[j]); at += 12;
        for (int j = 0; j < 3; j++) { memcpy(bytes + at, fields[j], sizes[j]); at += sizes[j]; }
    }
    history_hash(bytes + 40, n - 40, bytes + 8);
    int slot = history->slot == 0 ? 1 : 0; char path[HISTORY_PATH];
    /* The inactive slot is swapped in whole; a failed save leaves its older
       checksum snapshot, and the other slot stays the recovery copy. */
    bool ok = history_path(slot, path) && n_document_save(path, bytes, n) >= 0;
    if (ok) { history->generation = generation; history->slot = slot; }
    free(bytes); return ok;
}
/* Removed, or already absent. */
static bool history_remove(const char *path) {
    n_result removed = n_entry_remove(path);
    return removed >= 0 || removed == -N_ERR_NOT_FOUND;
}
bool web_autocomplete_clear(void) {
    /* Disable first so other browser windows cannot offer old popup snapshots.
       A confirmed clear is removal of both native snapshots, not a tombstone
       that leaves old values available through recovery. */
    popup_reset();
    if (webprefs_set_search_history(false) < 0) return false;
    bool ok = true; char path[HISTORY_PATH];
    for (int slot = 0; slot < 2; slot++) {
        if (!history_path(slot, path) || !history_remove(path)) ok = false;
    }
    /* Earlier builds kept the opt-in as this document beside the history; the
       preference replaced it, and its "on" is not carried over. */
    if (!history_file("search-history.enabled", path) || !history_remove(path)) ok = false;
    struct n_entry *entries = NULL;
    const char *root = web_profile_folder(false);
    n_result count = root ? n_folder_entries(root, &entries) : -N_ERR_NOT_FOUND;
    if (count < 0) return ok && count == -N_ERR_NOT_FOUND;
    for (n_result i = 0; i < count; i++) {
        /* Earlier builds staged snapshots under these names; a confirmed clear
           removes their values too. Never another store's entries. */
        const char *name = entries[i].name;
        const char *tail = !strncmp(name, "search-history.0.tmp-", 21) ||
            !strncmp(name, "search-history.1.tmp-", 21) ? name + 21 : NULL;
        if (!tail || !*tail) continue;
        bool digits = true;
        for (const char *p = tail; *p; p++) if (*p < '0' || *p > '9') digits = false;
        if (!digits) continue;
        char temporary[HISTORY_PATH];
        if (!history_file(name, temporary) || !history_remove(temporary)) ok = false;
    }
    free(entries);
    return ok;
}
void web_autocomplete_record(web_doc *d, web_node *submitter, const char *destination) {
    if (!d || !submitter || submitter->owner != d || !history_enabled()) return;
    node_t *form = submitter->tag == T_form ? submitter : web_form_owner(d, submitter);
    if (!form || !doc_node_connected(form)) return;
    char origin[HISTORY_ORIGIN], target[HISTORY_ORIGIN];
    if (!history_origin(d->url, origin) || !history_origin(destination, target) || strcmp(origin, target)) return;
    /* Do not store even search-looking controls from authentication forms. */
    for (node_t *node = d->owned_nodes; node; node = node->owned_next) if (node->owner == d && node->type == N_ELEM &&
        !node->foreign && node->tag == T_input && web_input_type(node) == WEB_INPUT_PASSWORD &&
        web_form_owner(d, node) == form && doc_node_connected(node)) return;
    struct search_history *history = history_load(); if (!history) return; bool changed = false;
    for (node_t *node = d->owned_nodes; node; node = node->owned_next) {
        const char *name; char current_origin[HISTORY_ORIGIN];
        if (!node->control_user_edited || !history_control(d, node, current_origin, &name) ||
            web_form_owner(d, node) != form || !history_value(node->value)) continue;
        /* MRU: a repeated search moves to the front instead of duplicating. */
        for (int i = 0; i < history->count; ) {
            struct search_entry *entry = &history->entries[i];
            if (!strcmp(entry->origin, current_origin) && !strcmp(entry->name, name) && !strcmp(entry->value, node->value)) {
                memmove(entry, entry + 1, (size_t)(history->count - i - 1) * sizeof *entry); history->count--;
            } else i++;
        }
        if (history->count == INT32_MAX || !history_reserve(history, history->count + 1)) break;
        memmove(history->entries + 1, history->entries, (size_t)history->count * sizeof *history->entries); history->count++;
        struct search_entry *entry = history->entries; memset(entry, 0, sizeof *entry);
        strlcpy(entry->origin, current_origin, sizeof entry->origin); strlcpy(entry->name, name, sizeof entry->name);
        strlcpy(entry->value, node->value, sizeof entry->value); changed = true;
    }
    if (changed && history_enabled()) (void)history_save(history);
    history_free(history);
}
/* Without labels, returns the number of matching values so the caller can
   size its list; with labels, fills at most capacity of them (MRU order). */
int web_autocomplete_options(web_doc *d, web_node *input, const char **labels, int capacity) {
    popup_reset(); const char *name; char origin[HISTORY_ORIGIN];
    if ((labels && capacity <= 0) || !history_enabled() || !history_control(d, input, origin, &name)) return 0;
    struct search_history *history = history_load(); if (!history) return 0;
    const char *prefix = input->value ? input->value : ""; size_t length = strlen(prefix);
    int matches = 0;
    for (int i = 0; i < history->count; i++) {
        struct search_entry *entry = &history->entries[i];
        if (!strcmp(entry->origin, origin) && !strcmp(entry->name, name) && !strncasecmp(entry->value, prefix, length)) matches++;
    }
    if (!labels) { history_free(history); return matches; }
    if (matches > capacity) matches = capacity;
    if (matches > popup.capacity) {
        void *grown = (size_t)matches > SIZE_MAX / sizeof *popup.values ? NULL :
            realloc(popup.values, (size_t)matches * sizeof *popup.values);
        if (!grown) { history_free(history); return 0; }
        popup.values = grown; popup.capacity = matches;
    }
    for (int i = 0; i < history->count && popup.count < matches; i++) {
        struct search_entry *entry = &history->entries[i];
        if (strcmp(entry->origin, origin) || strcmp(entry->name, name) || strncasecmp(entry->value, prefix, length)) continue;
        strlcpy(popup.values[popup.count], entry->value, HISTORY_VALUE);
        labels[popup.count] = popup.values[popup.count]; popup.count++;
    }
    popup.doc = d; popup.input = input; strlcpy(popup.origin, origin, sizeof popup.origin); strlcpy(popup.name, name, sizeof popup.name);
    history_free(history); return popup.count;
}
bool web_autocomplete_choose(web_doc *d, web_node *input, int index) {
    const char *name; char origin[HISTORY_ORIGIN];
    if (d != popup.doc || input != popup.input || index < 0 || index >= popup.count || !history_enabled() ||
        !history_control(d, input, origin, &name) || strcmp(origin, popup.origin) || strcmp(name, popup.name)) return false;
    struct search_history *history = history_load(); if (!history) return false; bool present = false;
    for (int i = 0; i < history->count; i++) if (!strcmp(history->entries[i].origin, origin) &&
        !strcmp(history->entries[i].name, name) && !strcmp(history->entries[i].value, popup.values[index])) { present = true; break; }
    history_free(history);
    bool ok = present && web_input_user_value(d, input, popup.values[index], strlen(popup.values[index]));
    if (ok) { uint32_t end = doc_utf16_length(input->value); doc_control_selection(d, input, end, end, 0); }
    popup_reset(); return ok;
}
