/* Nocturne search completion: private, opt-in, bounded native storage. The
   page has no history API and receives a past value only after the user chooses
   it. This is not a password manager or personal/contact/payment autofill. */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <nocturne.h>
#include <bearssl_hash.h>
#include "form_autocomplete.h"
#include "form_value.h"

#define HISTORY_ROOT "/data/browser"
#define HISTORY_PREF HISTORY_ROOT "/search-history.enabled"
#define HISTORY_COUNT 64
#define HISTORY_FIELD_COUNT 8
#define HISTORY_ORIGIN 192
#define HISTORY_NAME 64
#define HISTORY_VALUE 256
#define HISTORY_HEADER 56
#define HISTORY_FILE_MAX (HISTORY_HEADER + HISTORY_COUNT * (12 + HISTORY_ORIGIN + HISTORY_NAME + HISTORY_VALUE))
struct search_entry { char origin[HISTORY_ORIGIN], name[HISTORY_NAME], value[HISTORY_VALUE]; };
struct search_history { uint64_t generation; int count, slot; struct search_entry entries[HISTORY_COUNT]; };
static struct {
    web_doc *doc;
    web_node *input;
    char origin[HISTORY_ORIGIN], name[HISTORY_NAME];
    char values[HISTORY_FIELD_COUNT][HISTORY_VALUE];
    int count;
} popup;

static bool history_origin(const char *url, char out[HISTORY_ORIGIN]) {
    /* Native url_parse currently lacks IPv6. Fail closed instead of treating
       unparsed authorities, userinfo or opaque URLs as another site's origin. */
    bool tls = url && !strncasecmp(url, "https://", 8);
    const char *host = tls ? url + 8 : url && !strncasecmp(url, "http://", 7) ? url + 7 : NULL;
    if (!host) return false;
    size_t length = strcspn(host, "/?#:");
    if (!length || length >= 128 || host[0] == '.' || host[length - 1] == '.') return false;
    char normalized[128];
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        if (c == '.' && i && host[i - 1] == '.') return false;
        normalized[i] = (char)lower(c);
    }
    normalized[length] = 0;
    const char *tail = host + length; unsigned port = tls ? 443 : 80;
    if (*tail == ':') {
        tail++; port = 0; int digits = 0;
        while (*tail >= '0' && *tail <= '9') {
            if (++digits > 5) return false;
            port = port * 10 + (unsigned)(*tail++ - '0');
        }
        if (!digits || !port || port > 65535) return false;
    }
    if (*tail && *tail != '/' && *tail != '?' && *tail != '#') return false;
    int n = (port == (tls ? 443u : 80u)) ? snprintf(out, HISTORY_ORIGIN, "%s://%s", tls ? "https" : "http", normalized) :
        snprintf(out, HISTORY_ORIGIN, "%s://%s:%u", tls ? "https" : "http", normalized, port);
    return n > 0 && n < HISTORY_ORIGIN;
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
static void history_path(int slot, char path[96]) { snprintf(path, 96, HISTORY_ROOT "/search-history.%d", slot); }
static bool read_all(int fd, uint8_t *bytes, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t count = read(fd, bytes + done, n - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > n - done) return false;
        done += (size_t)count;
    }
    return true;
}
static bool write_all(int fd, const uint8_t *bytes, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t count = write(fd, bytes + done, n - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > n - done) return false;
        done += (size_t)count;
    }
    return true;
}
static bool history_enabled(void) {
    /* Re-read for every native operation: disabling in another browser window
       must not leave an enabled process cache. Missing/invalid/I/O means off. */
    struct n_stat st; static const char marker[] = "Nocturne search history opt-in v1\n";
    if (stat(HISTORY_PREF, &st) < 0 || st.type != N_FT_FILE || st.size != sizeof marker - 1) return false;
    int fd = open(HISTORY_PREF, O_RDONLY); if (fd < 0) return false;
    char bytes[sizeof marker]; bool ok = read_all(fd, (uint8_t *)bytes, sizeof marker - 1);
    int closed = close(fd);
    return ok && closed == 0 && !memcmp(bytes, marker, sizeof marker - 1);
}
/* 0 missing, 1 valid, -1 invalid/I/O. No unchecked raw struct serialization. */
static int history_read(int slot, struct search_history *out) {
    char path[96]; history_path(slot, path); struct n_stat st;
    if (stat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (st.type != N_FT_FILE || st.size < HISTORY_HEADER || st.size > HISTORY_FILE_MAX) return -1;
    size_t n = (size_t)st.size; uint8_t *bytes = malloc(n); if (!bytes) return -1;
    int fd = open(path, O_RDONLY); if (fd < 0) { free(bytes); return -1; }
    bool ok = read_all(fd, bytes, n); int closed = close(fd); uint8_t digest[32];
    if (!ok || closed < 0) { free(bytes); return -1; }
    history_hash(bytes + 40, n - 40, digest);
    ok = ok && closed == 0 && !memcmp(bytes, "NSEARCH1", 8) && !memcmp(bytes + 8, digest, 32);
    uint64_t generation = get32(bytes + 40) | (uint64_t)get32(bytes + 44) << 32;
    unsigned count = get32(bytes + 48);
    ok = ok && generation && count <= HISTORY_COUNT && !get32(bytes + 52);
    memset(out, 0, sizeof *out); out->generation = generation; out->slot = slot;
    size_t at = HISTORY_HEADER;
    for (unsigned i = 0; ok && i < count; i++) {
        if (n - at < 12) { ok = false; break; }
        size_t sizes[3] = {get32(bytes + at), get32(bytes + at + 4), get32(bytes + at + 8)}; at += 12;
        struct search_entry *entry = &out->entries[i];
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
        for (unsigned j = 0; ok && j < i; j++) if (!strcmp(entry->origin, out->entries[j].origin) &&
            !strcmp(entry->name, out->entries[j].name) && !strcmp(entry->value, out->entries[j].value)) ok = false;
    }
    ok = ok && at == n; free(bytes);
    if (!ok) return -1; out->count = (int)count; return 1;
}
static struct search_history *history_load(void) {
    struct search_history *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    if (!a || !b) { free(a); free(b); return NULL; }
    int ar = history_read(0, a), br = history_read(1, b);
    if (br == 1 && (ar != 1 || b->generation > a->generation)) { struct search_history *temp = a; a = b; b = temp; ar = 1; }
    free(b);
    if (ar != 1) {
        if (ar < 0 || br < 0) { free(a); return NULL; }
        memset(a, 0, sizeof *a); a->slot = -1;
    }
    return a;
}
static bool history_directory(void) {
    struct n_stat st;
    if (stat(HISTORY_ROOT, &st) == 0) return st.type == N_FT_DIR;
    if (errno != ENOENT || (mkdir(HISTORY_ROOT) < 0 && errno != EEXIST)) return false;
    /* /data itself is never manufactured; it is the native persistent volume. */
    return stat(HISTORY_ROOT, &st) == 0 && st.type == N_FT_DIR;
}
static bool history_save(struct search_history *history) {
    if (history->generation == UINT64_MAX || !history_directory()) return false;
    size_t n = HISTORY_HEADER;
    for (int i = 0; i < history->count; i++) n += 12 + strlen(history->entries[i].origin) + strlen(history->entries[i].name) + strlen(history->entries[i].value);
    if (n > HISTORY_FILE_MAX) return false;
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
    int slot = history->slot == 0 ? 1 : 0; char path[96], temp[128]; history_path(slot, path);
    snprintf(temp, sizeof temp, "%s.tmp-%d", path, getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC); bool ok = false;
    if (fd >= 0) {
        ok = write_all(fd, bytes, n); int closed = close(fd); ok = ok && closed == 0;
        if (ok) {
            /* Native rename copies then unlinks, not POSIX atomic rename. The
               older checksum snapshot is left intact; readback proves publish. */
            (void)rename(temp, path);
            struct search_history *check = calloc(1, sizeof *check);
            ok = check && history_read(slot, check) == 1 && check->generation == generation &&
                check->count == history->count && !memcmp(check->entries, history->entries, sizeof history->entries);
            free(check);
            if (ok) { history->generation = generation; history->slot = slot; }
            else (void)unlink(path);
        }
    }
    (void)unlink(temp); free(bytes); return ok;
}
static bool history_enable(bool enabled) {
    if (!enabled) {
        memset(&popup, 0, sizeof popup);
        if (unlink(HISTORY_PREF) < 0 && errno != ENOENT) return false;
        struct n_stat st; return stat(HISTORY_PREF, &st) < 0 && errno == ENOENT;
    }
    if (!history_directory()) return false;
    static const char marker[] = "Nocturne search history opt-in v1\n";
    int fd = open(HISTORY_PREF, O_WRONLY | O_CREAT | O_TRUNC); if (fd < 0) return false;
    bool ok = write_all(fd, (const uint8_t *)marker, sizeof marker - 1); int closed = close(fd);
    ok = ok && closed == 0 && history_enabled();
    if (!ok) (void)unlink(HISTORY_PREF); return ok;
}
static bool history_clear(void) {
    /* Disable first so other browser windows cannot offer old popup snapshots.
       A confirmed clear is removal of both native snapshots, not a tombstone
       that leaves old values available through recovery. */
    if (!history_enable(false)) return false;
    bool ok = true; char path[96];
    for (int slot = 0; slot < 2; slot++) {
        history_path(slot, path);
        if (unlink(path) < 0 && errno != ENOENT) ok = false;
        struct n_stat st; if (stat(path, &st) == 0 || errno != ENOENT) ok = false;
    }
    int fd = open(HISTORY_ROOT, O_RDONLY);
    if (fd < 0) return ok && errno == ENOENT;
    struct n_dirent entry; int index = 0, count = 0, attempts = 0;
    while (attempts++ < 2048 && (count = readdir(fd, index++, &entry)) > 0) {
        /* Only this module's interrupted native writes, never another store. */
        const char *tail = !strncmp(entry.name, "search-history.0.tmp-", 21) ||
            !strncmp(entry.name, "search-history.1.tmp-", 21) ? entry.name + 21 : NULL;
        if (!tail || !*tail) continue;
        bool digits = true;
        for (const char *p = tail; *p; p++) if (*p < '0' || *p > '9') digits = false;
        if (!digits) continue;
        char temporary[384]; snprintf(temporary, sizeof temporary, HISTORY_ROOT "/%s", entry.name);
        if (unlink(temporary) == 0) index = 0;
        else if (errno != ENOENT) ok = false;
    }
    if (count < 0 || attempts > 2048) ok = false;
    if (close(fd) < 0) ok = false;
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
        /* MRU: eight per origin/name, 64 globally; no unbounded input log. */
        int matches = 0;
        for (int i = 0; i < history->count; ) {
            struct search_entry *entry = &history->entries[i];
            bool pair = !strcmp(entry->origin, current_origin) && !strcmp(entry->name, name);
            if (pair && (!strcmp(entry->value, node->value) || ++matches >= HISTORY_FIELD_COUNT)) {
                memmove(entry, entry + 1, (history->count - i - 1) * sizeof *entry); history->count--;
            } else i++;
        }
        if (history->count == HISTORY_COUNT) history->count--;
        memmove(history->entries + 1, history->entries, history->count * sizeof *history->entries); history->count++;
        struct search_entry *entry = history->entries; memset(entry, 0, sizeof *entry);
        strlcpy(entry->origin, current_origin, sizeof entry->origin); strlcpy(entry->name, name, sizeof entry->name);
        strlcpy(entry->value, node->value, sizeof entry->value);
        memset(history->entries + history->count, 0, (HISTORY_COUNT - history->count) * sizeof *entry); changed = true;
    }
    if (changed && history_enabled()) (void)history_save(history);
    free(history);
}
int web_autocomplete_options(web_doc *d, web_node *input, const char **labels, int capacity) {
    memset(&popup, 0, sizeof popup); const char *name; char origin[HISTORY_ORIGIN];
    if (!labels || capacity <= 0 || !history_enabled() || !history_control(d, input, origin, &name)) return 0;
    struct search_history *history = history_load(); if (!history) return 0;
    const char *prefix = input->value ? input->value : ""; size_t length = strlen(prefix);
    for (int i = 0; i < history->count && popup.count < HISTORY_FIELD_COUNT && popup.count < capacity; i++) {
        struct search_entry *entry = &history->entries[i];
        if (strcmp(entry->origin, origin) || strcmp(entry->name, name) || strncasecmp(entry->value, prefix, length)) continue;
        strlcpy(popup.values[popup.count], entry->value, HISTORY_VALUE);
        labels[popup.count] = popup.values[popup.count]; popup.count++;
    }
    popup.doc = d; popup.input = input; strlcpy(popup.origin, origin, sizeof popup.origin); strlcpy(popup.name, name, sizeof popup.name);
    free(history); return popup.count;
}
bool web_autocomplete_choose(web_doc *d, web_node *input, int index) {
    const char *name; char origin[HISTORY_ORIGIN];
    if (d != popup.doc || input != popup.input || index < 0 || index >= popup.count || !history_enabled() ||
        !history_control(d, input, origin, &name) || strcmp(origin, popup.origin) || strcmp(name, popup.name)) return false;
    struct search_history *history = history_load(); if (!history) return false; bool present = false;
    for (int i = 0; i < history->count; i++) if (!strcmp(history->entries[i].origin, origin) &&
        !strcmp(history->entries[i].name, name) && !strcmp(history->entries[i].value, popup.values[index])) { present = true; break; }
    free(history);
    bool ok = present && web_input_user_value(d, input, popup.values[index], strlen(popup.values[index]));
    if (ok) { uint32_t end = doc_utf16_length(input->value); doc_control_selection(d, input, end, end, 0); }
    memset(&popup, 0, sizeof popup); return ok;
}
void web_autocomplete_settings(void) {
    window_t *window = win_open(600, 244, "Search history privacy", 0); if (!window) return;
    char message[120] = "Default is off. Enable only for non-secret searches.";
    int pressed = -1;
    for (;;) {
        bool enabled = history_enabled(); canvas_t *c = &window->c;
        gfx_fill(c, 0, 0, window->w, window->h, UI_BG);
        gfx_text(c, 16, 14, "Native search completion", UI_FG, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 16, 40, enabled ? "Search history: ENABLED" : "Search history: OFF", UI_ACCENT, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 16, 66, "Stores typed search fields, separated by site and field name.", UI_DIM, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 16, 88, "64 entries maximum; stored only in /data/browser.", UI_DIM, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 16, 110, "On a search field: Down opens suggestions; Enter chooses.", UI_DIM, TRANSPARENT, FONT_SMALL);
        gfx_text(c, 16, 132, "Never type secrets in a search. Clear also disables history.", UI_DIM, TRANSPARENT, FONT_SMALL);
        ui_button(c, 16, 164, 170, 30, enabled ? "Disable (D)" : "Enable (E)", false, false);
        ui_button(c, 200, 164, 170, 30, "Clear and disable (C)", false, false);
        ui_button(c, 384, 164, 170, 30, "Close (Esc)", false, false);
        gfx_text(c, 16, 212, message, UI_FG, TRANSPARENT, FONT_SMALL); win_update(window);
        struct gui_event event;
        if (win_event(window, &event, -1) < 0 || event.type == EV_CLOSE) break;
        int action = -1;
        if (event.type == EV_MOUSE_DOWN && (event.buttons & 1)) {
            pressed = ui_hit(event.x, event.y, 16, 164, 170, 30) ? 0 :
                ui_hit(event.x, event.y, 200, 164, 170, 30) ? 1 :
                ui_hit(event.x, event.y, 384, 164, 170, 30) ? 2 : -1;
        } else if (event.type == EV_MOUSE_UP) {
            if (pressed == 0 && ui_hit(event.x, event.y, 16, 164, 170, 30)) action = enabled ? 1 : 0;
            else if (pressed == 1 && ui_hit(event.x, event.y, 200, 164, 170, 30)) action = 2;
            else if (pressed == 2 && ui_hit(event.x, event.y, 384, 164, 170, 30)) break;
            pressed = -1;
        } else if (event.type == EV_KEY && event.pressed) {
            if (event.key == NKEY_ESC) break;
            unsigned key = lower(event.key);
            if (key == 'e') action = 0; else if (key == 'd') action = 1; else if (key == 'c') action = 2;
        }
        if (action >= 0) {
            bool ok = action == 2 ? history_clear() : history_enable(action == 0);
            strlcpy(message, !ok ? "Storage operation failed; verify the persistent /data volume." : action == 2 ?
                "History removed and completion disabled." : action == 0 ? "Enabled. Past values are offered only on explicit selection." :
                "Disabled. Stored history is kept until Clear is selected.", sizeof message);
        }
    }
    win_close(window);
}
