/* Keeping the system's state on C: (sys/keep.h). One thread of its own reads the parts' documents
   and writes their changes: reading and writing C: may sleep, and the parts change their state from
   anywhere (a system call on any CPU, the compositor, a remote session). */
#include "sys/keep.h"
#include "fs/store.h"
#include "mm/heap.h"
#include "sys/object.h"
#include "sys/sched.h"

#define READ_MOST (64u * 1024)  /* a longer document is no part's: it is not read */
#define TEXT_FIRST 1024u        /* the room text gets first */
#define PATH_ROOM 512u          /* C:\Users\<name>\AppData\... */

/* Where an older system kept everything, newest first (docs/settings-ownership-20261027.md §7):
   the first one there is read for the parts that have no document of their own yet. */
static const char *const older[] = {"AppData\\Nocturne\\Settings.conf", "AppData\\Settings\\desktop.conf"};

/* The parts, in the order they registered: appended, never removed. parts_lock guards the list's
   end, started, ready, and each part's read, dirty, retry and dirty_at, which any CPU changes; keep's
   thread alone has reading, missing and took. keep's thread looks at what is due under the lock and
   is in keep_wq before it is given back. The lock comes after any lock a part holds when it calls
   keep_changed, and before keep_wq's. A walk without it reads each next with acquire: a part is
   whole before it is linked. */
static struct spinlock parts_lock;
static struct keep *parts, **parts_end = &parts;
static bool started; /* keep_start ran: a part registered now is read on keep's thread */
static bool ready;   /* the parts registered before keep_start were read */
static struct wait_queue keep_wq, ready_wq;
/* One write at a time (keep's thread, keep_flush's caller), so an older text never lands last. */
static struct mutex write_lock;

static struct keep *first_part(void) { return __atomic_load_n(&parts, __ATOMIC_ACQUIRE); }
static struct keep *next_part(const struct keep *k) { return __atomic_load_n(&k->next, __ATOMIC_ACQUIRE); }

/* ---- the parts ---- */

void keep_register(struct keep *k) {
    k->next = NULL;
    k->read = k->dirty = k->retry = k->reading = k->missing = k->took = false;
    uint64_t f = spin_lock(&parts_lock);
    __atomic_store_n(parts_end, k, __ATOMIC_RELEASE);
    parts_end = &k->next;
    bool wake = started;
    spin_unlock(&parts_lock, f);
    if (wake) wq_wake_all(&keep_wq);
}

void keep_changed(struct keep *k) {
    uint64_t f = spin_lock(&parts_lock);
    bool kept = k->read;
    if (kept) {
        k->dirty = true;
        k->retry = false;
        k->dirty_at = clock_now_ns();
    }
    spin_unlock(&parts_lock, f);
    if (kept) wq_wake_all(&keep_wq);
}

bool keep_number(const char *s, int64_t low, int64_t high, int64_t *out) {
    bool negative = *s == '-';
    if (negative) s++;
    int64_t v = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (++digits > 18) return false;
        v = v * 10 + (*s - '0');
    }
    if (!digits || *s) return false;
    if (negative) v = -v;
    if (v < low || v > high) return false;
    *out = v;
    return true;
}

void keep_put(char *out, size_t cap, size_t *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool room = *n < cap;
    int k = kvsnprintf(room ? out + *n : NULL, room ? cap - *n : 0, fmt, ap);
    va_end(ap);
    if (k > 0) *n += (size_t)k;
}

/* ---- documents ---- */

/* The user's folder, then rel: false when it does not fit. */
static bool user_path(const char *rel, char *out, size_t cap) {
    const char *user = store_user_folder();
    size_t n = strlen(user);
    const char *sep = n && user[n - 1] == '\\' ? "" : "\\";
    return (size_t)ksnprintf(out, cap, "%s%s%s", user, sep, rel) < cap;
}

static bool part_path(const struct keep *k, char *out, size_t cap) {
    char rel[PATH_ROOM];
    return (size_t)ksnprintf(rel, sizeof rel, "AppData\\Nocturne\\%s.conf", k->name) < sizeof rel &&
           user_path(rel, out, cap);
}

/* The whole document at path, with a zero after it (kfree it): its length, or a negative result. */
static int64_t document_read(const char *path, char **out) {
    struct handle *h;
    *out = NULL;
    int r = store_document_open(path, N_DOCUMENT_READ, false, &h);
    if (r < 0) return r;
    int64_t size = store_document_size(h), n = size;
    if (size > (int64_t)READ_MOST) n = -N_ERR_TOO_LARGE;
    else if (size >= 0) {
        char *text = kmalloc((size_t)size + 1);
        n = text ? store_document_read(h, 0, text, (size_t)size, false) : -N_ERR_NO_MEMORY;
        if (n >= 0) {
            text[n] = 0;
            *out = text;
        } else kfree(text);
    }
    handle_close(h);
    return n;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* fn for each line of text with a key and a value; a note ('#' first), an empty line and one
   without a key before its '=' have none. text is cut up. */
static void each_line(char *text, void (*fn)(const char *key, const char *value, void *arg), void *arg) {
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        char *eq = strchr(line, '=');
        if (*line == '#' || !eq) continue;
        *eq = 0;
        char *key = trim(line);
        if (*key) fn(key, trim(eq + 1), arg);
    }
}

/* The part's text now, written as its document. */
static int write_part(struct keep *k) {
    char path[PATH_ROOM];
    if (!part_path(k, path, sizeof path)) return -N_ERR_TOO_LARGE;
    size_t cap = TEXT_FIRST;
    for (int tries = 0; tries < 4; tries++) {
        char *text = kmalloc(cap);
        if (!text) return -N_ERR_NO_MEMORY;
        size_t n = k->text(text, cap);
        if (n < cap) {
            int r = store_save(path, text, n, false);
            kfree(text);
            if (r < 0) kprintf("keep: %s cannot be saved (%d)\n", path, r);
            return r < 0 ? r : 0;
        }
        kfree(text);
        cap = n + 1; /* it grew: again with room for it */
    }
    return -N_ERR_TOO_LARGE;
}

/* The part's document now, when it changed; a failure leaves the change to the next change or
   keep_flush. */
static int save(struct keep *k) {
    mutex_lock(&write_lock);
    uint64_t f = spin_lock(&parts_lock);
    bool changed = k->dirty;
    k->dirty = k->retry = false;
    spin_unlock(&parts_lock, f);
    int r = changed ? write_part(k) : 0;
    if (r < 0) {
        f = spin_lock(&parts_lock);
        if (!k->dirty) k->dirty = k->retry = true; /* a change meanwhile is due as it says */
        spin_unlock(&parts_lock, f);
    }
    mutex_unlock(&write_lock);
    return r;
}

void keep_flush(void) {
    for (struct keep *k = first_part(); k; k = next_part(k)) {
        uint64_t f = spin_lock(&parts_lock);
        bool changed = k->dirty;
        spin_unlock(&parts_lock, f);
        if (changed) save(k);
    }
}

/* ---- reading back ---- */

static void own_line(const char *key, const char *value, void *arg) {
    struct keep *k = arg;
    k->line(key, value, true);
}

/* A line of an older system's document: the parts in this round without a document of their own
   take it, the others only say whether it is theirs. */
struct older_read {
    bool every_line_kept; /* some part owns every line */
};
static void older_line(const char *key, const char *value, void *arg) {
    struct older_read *o = arg;
    bool owned = false;
    for (struct keep *k = first_part(); k; k = next_part(k)) {
        bool take = k->reading && k->missing;
        if (!k->line(key, value, take)) continue;
        owned = true;
        if (take) k->took = true;
    }
    if (!owned) o->every_line_kept = false;
}

/* The parts of this round without a document of their own take their lines from the newest older
   document there is, and are written at once. The older document goes once every line of it is a
   part's and every part that took lines was written. */
static void read_older(void) {
    for (size_t i = 0; i < ARRAY_SIZE(older); i++) {
        char path[PATH_ROOM], *text;
        if (!user_path(older[i], path, sizeof path)) return;
        int64_t n = document_read(path, &text);
        if (n == -N_ERR_NOT_FOUND) continue;
        if (n < 0) {
            kprintf("keep: %s cannot be read (%lld); what it kept is not taken over\n", path, (long long)n);
            return;
        }
        struct older_read o = {.every_line_kept = true};
        each_line(text, older_line, &o);
        kfree(text);
        bool written = true;
        for (struct keep *k = first_part(); k; k = next_part(k)) {
            if (!k->reading || !k->took) continue;
            uint64_t f = spin_lock(&parts_lock);
            k->read = k->dirty = true; /* written now: from here on its changes are kept */
            spin_unlock(&parts_lock, f);
            int r = save(k);
            kprintf("keep: %s taken over from %s%s\n", k->name, path, r < 0 ? ", but not saved" : "");
            written = written && r >= 0;
        }
        if (!o.every_line_kept) kprintf("keep: %s stays: a line in it is no part's\n", path);
        else if (written) {
            int r = store_entry_remove(path, false);
            if (r < 0) kprintf("keep: %s cannot be removed (%d)\n", path, r);
            else kprintf("keep: %s removed: what it kept is in each part's own document\n", path);
        }
        return;
    }
}

/* Every part not read yet, from its own document; one without takes its lines from an older
   system's. */
static void read_parts(void) {
    bool any = false, missing = false;
    uint64_t f = spin_lock(&parts_lock);
    for (struct keep *k = parts; k; k = k->next)
        if (!k->read) {
            k->reading = any = true;
            k->missing = k->took = false;
        }
    spin_unlock(&parts_lock, f);
    if (!any) return;
    for (struct keep *k = first_part(); k; k = next_part(k)) {
        if (!k->reading) continue;
        char path[PATH_ROOM], *text = NULL;
        int64_t n = part_path(k, path, sizeof path) ? document_read(path, &text) : -N_ERR_TOO_LARGE;
        if (n >= 0) {
            each_line(text, own_line, k);
            kprintf("keep: %s read from %s\n", k->name, path);
        } else if (n == -N_ERR_NOT_FOUND) k->missing = missing = true;
        else kprintf("keep: %s cannot be read (%lld); %s keeps the system's own state\n", path, (long long)n, k->name);
        kfree(text);
    }
    if (missing) read_older();
    f = spin_lock(&parts_lock);
    for (struct keep *k = parts; k; k = k->next)
        if (k->reading) k->read = true, k->reading = false;
    spin_unlock(&parts_lock, f);
}

/* When keep's thread has work next (clock_now_ns): 0 for a part not read yet, CLOCK_NEVER for none.
   parts_lock held. */
static uint64_t next_work(void) {
    uint64_t due = CLOCK_NEVER;
    for (struct keep *k = parts; k; k = k->next) {
        if (!k->read) return 0;
        if (k->dirty && !k->retry) due = MIN(due, k->dirty_at + KEEP_AFTER_NS);
    }
    return due;
}

/* One round of keep's thread: the parts not read yet read, the changes that are due written. */
static void keep_work(void) {
    read_parts();
    uint64_t f = spin_lock(&parts_lock);
    if (!ready) {
        ready = true;
        wq_wake_all(&ready_wq); /* keep_start */
    }
    spin_unlock(&parts_lock, f);
    uint64_t now = clock_now_ns();
    for (struct keep *k = first_part(); k; k = next_part(k)) {
        f = spin_lock(&parts_lock);
        bool due = k->dirty && !k->retry && now >= k->dirty_at + KEEP_AFTER_NS;
        spin_unlock(&parts_lock, f);
        if (due) save(k);
    }
}

static void keep_thread(void *arg) {
    (void)arg;
    char folder[PATH_ROOM];
    if (user_path("AppData\\Nocturne", folder, sizeof folder)) {
        int r = store_folder_create(folder, false);
        if (r < 0 && r != -N_ERR_EXISTS) kprintf("keep: %s cannot be made (%d)\n", folder, r);
    }
    for (;;) {
        keep_work();
        uint64_t f = spin_lock(&parts_lock);
        uint64_t due = next_work();
        if (due > clock_now_ns()) wq_wait_locked_until(&keep_wq, &parts_lock, due);
        else spin_give(&parts_lock);
        irq_restore(f);
    }
}

void keep_start(void) {
    uint64_t f = spin_lock(&parts_lock);
    started = true;
    spin_unlock(&parts_lock, f);
    if (!kthread_create("keep", keep_thread, NULL)) {
        kprintf("keep: the system's state is neither read back nor kept (no thread)\n");
        return;
    }
    /* Back before the first program starts (a display driver offers its monitor's modes). */
    f = spin_lock(&parts_lock);
    while (!ready) {
        wq_wait_locked(&ready_wq, &parts_lock);
        spin_take(&parts_lock);
    }
    spin_unlock(&parts_lock, f);
}
