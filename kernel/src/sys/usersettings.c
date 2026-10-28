/* The user's settings (sys/usersettings.h). */
#include "sys/usersettings.h"
#include "arch/cpu.h"
#include "dev/audio.h"
#include "fs/store.h"
#include "gui/screenmode.h"
#include "gui/wm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/object.h"
#include "sys/sched.h"
#include "sys/syscall.h"

#define SAVE_AFTER_MS 500 /* a change goes to C: once the user has stopped changing it this long */
#define TEXT_MAX 8192     /* the file: what is read of it at most, and room to write it */
#define DEFAULT_LANGUAGES "en-US"
#define SETTINGS 4        /* the N_USER_SETTING_* bits */
_Static_assert(N_USER_SETTINGS_ALL == (1u << SETTINGS) - 1, "a counter for every setting");

/* The languages, canonical (list_canonical). Written on the BSP with interrupts off between two
   moves of languages_seq (odd while they change), so that an AP reads them between two equal even
   values (user_languages_ap). */
static char languages[N_USER_LANGUAGES_MAX] = DEFAULT_LANGUAGES;
static uint32_t languages_length = sizeof DEFAULT_LANGUAGES - 1, languages_seq;

/* changes[i]: how often setting i changed. A watch is receivable while one of its interest moved
   since it last looked. Changed under the BSP IRQ guard; read in AP read sections too. */
static uint32_t changes[SETTINGS];

/* Saving: under the guard, but the file's text and the saving itself under save_lock. */
static bool started, dirty, migrating;
static uint64_t dirty_at;
static struct wait_queue saver_wq;
static struct mutex save_lock;
static char path[sizeof "C:\\Users\\" + 255 + sizeof "\\AppData\\Nocturne\\Settings.conf"];
/* The file the Settings app kept them in before the system did: read when path is not there yet,
   and removed once path is saved (migrating). */
static char old_path[sizeof "C:\\Users\\" + 255 + sizeof "\\AppData\\Settings\\desktop.conf"];

/* ---- the languages ---- */

static bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool digit(char c) { return c >= '0' && c <= '9'; }

/* One language tag as BCP 47 writes it, checked at the level of its subtags (RFC 5646 §2.1): 1 to
   8 letters and digits each with hyphens between, the first 2 to 8 letters or the singleton x or
   i, and no singleton last. Written to out in the case RFC 5646 §2.1.1 gives: lower case, but a
   two-letter subtag upper case and a four-letter one title case where they are neither first nor
   after a singleton (a region, a script). Its length, the same as n; 0 when it is not a tag. */
static size_t tag_canonical(const char *in, size_t n, char *out) {
    bool after_singleton = false;
    size_t start = 0, index = 0, length = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && in[i] != '-') {
            if (!letter(in[i]) && !digit(in[i])) return 0;
            continue;
        }
        size_t len = i - start;
        const char *s = in + start;
        bool letters = true;
        for (size_t k = 0; k < len; k++) letters = letters && letter(s[k]);
        if (!len || len > 8) return 0;
        if (!index && (!letters || (len == 1 && (s[0] | 32) != 'x' && (s[0] | 32) != 'i'))) return 0;
        bool upper = index && !after_singleton && len == 2 && letters;
        bool title = index && !after_singleton && len == 4 && letters;
        for (size_t k = 0; k < len; k++)
            out[start + k] = (char)(upper || (title && !k) ? toupper(s[k]) : tolower(s[k]));
        if (len == 1) after_singleton = true;
        length = len;
        if (i < n) out[i] = '-';
        start = i + 1;
        index++;
    }
    return length == 1 ? 0 : n;
}

/* The list as n_user_languages gives it: the tags between commas, each canonical, without the
   spaces around them, a tag already in it dropped. Its length (out ends with a zero), or a
   negative result: N_ERR_INVALID for an empty list or item or a malformed tag. */
static int64_t list_canonical(const char *in, size_t n, char *out, size_t cap) {
    size_t length = 0;
    for (size_t i = 0; i <= n; i++) {
        size_t a = i;
        while (i < n && in[i] != ',') i++;
        size_t b = i;
        while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
        while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) b--;
        if (a == b) return -N_ERR_INVALID;
        size_t at = length + (length ? 1 : 0), tn = b - a;
        if (at + tn + 1 > cap) return -N_ERR_TOO_LARGE;
        if (!tag_canonical(in + a, tn, out + at)) return -N_ERR_INVALID;
        bool again = false;
        for (size_t k = 0; k + tn <= length && !again; k++)
            if (!k || out[k - 1] == ',')
                again = !memcmp(out + k, out + at, tn) && (k + tn == length || out[k + tn] == ',');
        if (again) continue;
        if (length) out[length] = ',';
        length = at + tn;
    }
    out[length] = 0;
    return (int64_t)length;
}

/* Puts a canonical list in place; whether it differs from the one there. BSP. */
static bool languages_put(const char *list, size_t n) {
    uint64_t f = irq_save();
    bool differs = n != languages_length || memcmp(list, languages, n);
    if (differs) {
        __atomic_store_n(&languages_seq, languages_seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memcpy(languages, list, n);
        languages[n] = 0;
        languages_length = (uint32_t)n;
        __atomic_store_n(&languages_seq, languages_seq + 1, __ATOMIC_RELEASE);
    }
    irq_restore(f);
    return differs;
}

/* The list and its length, copied on the BSP. */
static size_t languages_copy(char out[N_USER_LANGUAGES_MAX]) {
    uint64_t f = irq_save();
    size_t n = languages_length;
    memcpy(out, languages, n + 1);
    irq_restore(f);
    return n;
}

int64_t user_languages(char *user_out, uint64_t cap) {
    char list[N_USER_LANGUAGES_MAX];
    size_t n = languages_copy(list);
    if (cap <= n) return -N_ERR_TOO_LARGE;
    return copy_to_user(user_out, list, n + 1) ? -N_ERR_BAD_ADDRESS : (int64_t)n;
}

bool user_languages_ap(char *user_out, uint64_t cap, int64_t *result) {
    char list[N_USER_LANGUAGES_MAX];
    size_t n;
    for (int tries = 0;; tries++) {
        if (tries == 8) return false; /* it kept changing: the BSP reads it in order */
        uint32_t seq = __atomic_load_n(&languages_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) { __builtin_ia32_pause(); continue; }
        n = MIN(__atomic_load_n(&languages_length, __ATOMIC_RELAXED), sizeof list - 1);
        memcpy(list, languages, n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&languages_seq, __ATOMIC_RELAXED) == seq) break;
    }
    list[n] = 0;
    if (cap <= n) { *result = -N_ERR_TOO_LARGE; return true; }
    if (!vmm_ap_user_begin(user_out, n + 1, true)) return false;
    memcpy(user_out, list, n + 1);
    vmm_ap_uaccess_end();
    *result = (int64_t)n;
    return true;
}

int64_t user_languages_set(const char *user_list, uint64_t length) {
    char in[N_USER_LANGUAGES_MAX], list[N_USER_LANGUAGES_MAX];
    if (length >= sizeof in) return -N_ERR_TOO_LARGE;
    if (copy_from_user(in, user_list, (size_t)length)) return -N_ERR_BAD_ADDRESS;
    int64_t n = list_canonical(in, (size_t)length, list, sizeof list);
    if (n < 0) return n;
    if (languages_put(list, (size_t)n)) {
        kprintf("settings: the user's languages are %s\n", list);
        user_settings_changed(N_USER_SETTING_LANGUAGES);
    }
    return 0;
}

/* ---- watches ---- */

struct watch {
    uint32_t interest;
    uint32_t seen[SETTINGS]; /* changes[] as it last looked */
};

/* Guard held or an AP read section: it only reads. */
static uint32_t watch_pending(const struct watch *w) {
    uint32_t bits = 0;
    for (int i = 0; i < SETTINGS; i++)
        if ((w->interest & 1u << i) &&
            __atomic_load_n(&changes[i], __ATOMIC_RELAXED) != __atomic_load_n(&w->seen[i], __ATOMIC_RELAXED))
            bits |= 1u << i;
    return bits;
}
static uint32_t watch_ready(struct handle *h) { return watch_pending(h->priv) ? N_EVENT_RECEIVABLE : 0; }
static void watch_close(struct handle *h) { kfree(h->priv); }
static const struct object_ops watch_ops = {.kind = N_KIND_USER_SETTINGS, .ready = watch_ready, .close = watch_close};
/* One object for all watches: a change notifies every wait on any of them, each looks at its own. */
static struct object settings_object = {.ops = &watch_ops};

int64_t user_settings_watch(uint32_t interest) {
    if (!interest || (interest & ~N_USER_SETTINGS_ALL)) return -N_ERR_INVALID;
    struct watch *w = kzalloc(sizeof *w);
    if (!w) return -N_ERR_NO_MEMORY;
    w->interest = interest;
    uint64_t f = irq_save();
    for (int i = 0; i < SETTINGS; i++) w->seen[i] = changes[i];
    irq_restore(f);
    struct handle *h = handle_open(&settings_object, false);
    if (!h) {
        kfree(w);
        return -N_ERR_NO_MEMORY;
    }
    h->priv = w;
    return handle_install(h); /* a failure closes h, and watch_close frees w */
}

int64_t user_settings_changes(int index) {
    struct handle *h = handle_get(index);
    if (!h) return -N_ERR_NO_HANDLE;
    int64_t r = -N_ERR_WRONG_KIND;
    if (handle_is(h, &watch_ops)) {
        struct watch *w = h->priv;
        uint64_t f = irq_save();
        r = watch_pending(w);
        for (int i = 0; i < SETTINGS; i++) __atomic_store_n(&w->seen[i], changes[i], __ATOMIC_RELAXED);
        irq_restore(f);
    }
    handle_close(h);
    return r;
}

void user_settings_changed(uint32_t bits) {
    uint64_t f = irq_save();
    for (int i = 0; i < SETTINGS; i++)
        if (bits & 1u << i) __atomic_store_n(&changes[i], changes[i] + 1, __ATOMIC_RELAXED);
    if (started) {
        dirty = true;
        dirty_at = uptime_ms();
    }
    irq_restore(f);
    object_notify(&settings_object);
    if (started) wq_wake_all(&saver_wq);
}

/* ---- the file ---- */

static void put(char *out, size_t cap, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t cap, size_t *n, const char *fmt, ...) {
    if (*n >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int k = kvsnprintf(out + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = MIN(*n + (size_t)k, cap - 1);
}

/* The settings as they are now, as the file has them. */
static size_t settings_text(char *out, size_t cap) {
    char list[N_USER_LANGUAGES_MAX];
    languages_copy(list);
    int speed;
    bool smooth;
    wm_desktop_settings(&speed, &smooth);
    size_t n = 0;
    put(out, cap, &n, "# The user's settings. Nocturne keeps this file: change them in Settings.\n");
    put(out, cap, &n, "languages=%s\npointer_speed=%d\nsmooth_gradients=%s\nvolume=%d\nmuted=%s\n", list, speed,
        smooth ? "on" : "off", audio_volume(), audio_muted() ? "on" : "off");
    char monitor[sizeof ((struct n_screen_modes *)0)->monitor];
    struct n_screen_mode m;
    for (unsigned i = 0; screen_modes_choice(i, monitor, &m); i++)
        put(out, cap, &n, "mode.%s=%ux%u@%u\n", monitor, m.width, m.height, m.refresh_mhz);
    out[n] = 0;
    return n;
}

/* Writes the file if something changed; its result. */
static int save(void) {
    mutex_lock(&save_lock);
    uint64_t f = irq_save();
    bool due = dirty;
    dirty = false;
    irq_restore(f);
    int r = 0;
    char *text = due ? kmalloc(TEXT_MAX) : NULL;
    if (due && !text) r = -N_ERR_NO_MEMORY;
    else if (due) r = store_save(path, text, settings_text(text, TEXT_MAX), false);
    kfree(text);
    if (r < 0) kprintf("settings: %s cannot be saved (%d)\n", path, r);
    else if (due && migrating) {
        migrating = false; /* one place keeps them from now on */
        int e = store_entry_remove(old_path, false);
        if (e < 0) kprintf("settings: %s cannot be removed (%d)\n", old_path, e);
    }
    mutex_unlock(&save_lock);
    return r;
}

void user_settings_flush(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) save();
}

static bool number(const char *s, int64_t low, int64_t high, int64_t *out) {
    bool negative = *s == '-';
    if (negative) s++;
    int64_t v = 0;
    int digits = 0;
    for (; digit(*s); s++)
        if (++digits > 9) return false;
        else v = v * 10 + (*s - '0');
    if (!digits || *s) return false;
    v = negative ? -v : v;
    if (v < low || v > high) return false;
    *out = v;
    return true;
}

/* "<width>x<height>@<millihertz>", none of them 0. */
static bool mode_value(const char *s, struct n_screen_mode *m) {
    uint32_t v[3] = {0};
    for (int i = 0; i < 3; i++) {
        int digits = 0;
        for (; digit(*s); s++)
            if (++digits > 7) return false;
            else v[i] = v[i] * 10 + (uint32_t)(*s - '0');
        if (!v[i] || *s != (i == 0 ? 'x' : i == 1 ? '@' : 0)) return false;
        if (i < 2) s++;
    }
    *m = (struct n_screen_mode){v[0], v[1], v[2], 0};
    return true;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* The saved settings back into their owners. Smooth gradients stay wished for while the screen
   cannot take them; a monitor's mode is shown whenever a display driver drives that monitor and
   offers it, which may be later; the volume is set even without a sound device, for when one plays
   (a Remote Desktop viewer). Settings an older system wrote and this one no longer has are left
   out; deep_colour is what smooth_gradients was called. */
static void apply(char *text) {
    int speed;
    bool smooth;
    wm_desktop_settings(&speed, &smooth);
    int64_t volume = -1, muted = -1, smooth_now = -1, smooth_old = -1, v;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        line = trim(line);
        if (*line == '#' || !eq) continue;
        *eq = 0;
        char *key = trim(line), *value = trim(eq + 1);
        struct n_screen_mode m;
        if (!strcmp(key, "pointer_speed") && number(value, -2, 2, &v)) speed = (int)v;
        else if (!strcmp(key, "smooth_gradients")) smooth_now = !strcmp(value, "on");
        else if (!strcmp(key, "deep_colour")) smooth_old = !strcmp(value, "on");
        else if (!strcmp(key, "volume") && number(value, 0, 100, &v)) volume = v;
        else if (!strcmp(key, "muted")) muted = !strcmp(value, "on");
        else if (!strcmp(key, "languages")) {
            char list[N_USER_LANGUAGES_MAX];
            int64_t n = list_canonical(value, strlen(value), list, sizeof list);
            if (n > 0) languages_put(list, (size_t)n);
            else kprintf("settings: the saved languages are not a list of language tags; %s instead\n", languages);
        } else if (!strncmp(key, "mode.", 5) && key[5] && strlen(key + 5) < sizeof ((struct n_screen_modes *)0)->monitor &&
                   mode_value(value, &m))
            screen_modes_choose(key + 5, &m);
    }
    if (smooth_now < 0) smooth_now = smooth_old;
    if (smooth_now >= 0) smooth = smooth_now;
    wm_desktop_settings_set(speed, smooth);
    if (volume >= 0) audio_set_volume((int)volume); /* first: setting it unmutes */
    if (muted >= 0) audio_set_muted(muted);
}

/* A document's content, at most max bytes and a zero; its length or a negative result. */
static int64_t read_text(const char *name, char *out, size_t max) {
    struct handle *h;
    int r = store_document_open(name, N_DOCUMENT_READ, false, &h);
    if (r < 0) return r;
    int64_t size = store_document_size(h), n = size < 0 ? size : store_document_read(h, 0, out, MIN((uint64_t)size, max), false);
    handle_close(h);
    if (n >= 0) out[n] = 0;
    return n;
}

/* The saved settings back, then every change saved. A thread of its own: reading C: may sleep. */
static void settings_thread(void *arg) {
    (void)arg;
    const char *user = store_user_folder();
    size_t ul = strlen(user);
    const char *sep = ul && user[ul - 1] == '\\' ? "" : "\\";
    char folder[sizeof path];
    ksnprintf(folder, sizeof folder, "%s%sAppData\\Nocturne", user, sep);
    ksnprintf(path, sizeof path, "%s\\Settings.conf", folder);
    ksnprintf(old_path, sizeof old_path, "%s%sAppData\\Settings\\desktop.conf", user, sep);
    int r = store_folder_create(folder, false);
    if (r < 0 && r != -N_ERR_EXISTS) kprintf("settings: %s cannot be made (%d)\n", folder, r);

    /* The file an older Settings app wrote, until the system has its own. */
    char *text = kmalloc(TEXT_MAX + 1);
    const char *from = path;
    int64_t n = text ? read_text(path, text, TEXT_MAX) : -N_ERR_NO_MEMORY;
    if (n == -N_ERR_NOT_FOUND) n = read_text(from = old_path, text, TEXT_MAX);
    if (n >= 0) {
        apply(text);
        kprintf("settings: the user's settings are back from %s\n", from);
    } else if (n != -N_ERR_NOT_FOUND)
        kprintf("settings: %s cannot be read (%lld); the system's own settings stay\n", from, (long long)n);
    kfree(text);

    uint64_t f = irq_save();
    dirty = migrating = from == old_path && n >= 0;
    dirty_at = uptime_ms();
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    irq_restore(f);

    for (;;) {
        f = irq_save();
        uint64_t now = uptime_ms(), due = dirty_at + SAVE_AFTER_MS;
        bool now_due = dirty && now >= due;
        if (!dirty) wq_wait(&saver_wq);
        else if (!now_due) wq_wait_timeout(&saver_wq, due - now);
        irq_restore(f);
        if (now_due) save();
    }
}

void user_settings_start(void) {
    if (!kthread_create("settings", settings_thread, NULL)) {
        kprintf("settings: the user's settings cannot be put back or kept (no thread)\n");
        return;
    }
    /* Back before the apps start (a display driver offers its monitor's modes). The boot task has
       no stack to sleep on a wait queue with: it lets the thread run until then. */
    while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) wq_nap(NULL, NULL, 0);
}

