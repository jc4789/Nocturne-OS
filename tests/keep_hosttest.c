/* Keeping the system's state on C: (kernel/src/sys/keep.c) on the host: the lines of a document,
   each part's own document read back, an older system's document taken over by the parts without
   one and removed only when every line of it has a part and every part that took lines was
   written, a change written KEEP_AFTER_MS after the last one, keep_flush at once, a failed write
   left to the next change or flush, a part registered after the start read at once, and a text
   longer than the first room. C: is a table of documents, the clock is timer_ticks, and keep's
   thread is run a round at a time (keep_work); no guest, real C:, scheduler or part of the system
   (the desktop, sound, the screen's modes) runs here.
     tools/msys64/ucrt64/bin/clang -std=c2x -Wall -Wextra -Ikernel/src -Icommon
       -o build/host/keep-hosttest.exe tests/keep_hosttest.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "kernel.h"
#include "arch/cpu.h"

static uint64_t host_irq_save(void) { return 0; }
static void host_irq_restore(uint64_t f) { (void)f; }
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#include "../kernel/src/sys/keep.c"
#undef irq_save
#undef irq_restore

static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- the kernel's own ---- */
volatile uint64_t timer_ticks;
uint64_t hhdm_offset;
static char log_text[16384]; /* what keep said */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }
int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
int kprintf(const char *fmt, ...) {
    size_t at = strlen(log_text);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(log_text + at, sizeof log_text - at, fmt, ap);
    va_end(ap);
    return n;
}
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
static unsigned wakes;
void wq_wake_all(struct wait_queue *wq) { (void)wq; wakes++; }
void wq_wait(struct wait_queue *wq) { (void)wq; abort(); }
bool wq_wait_timeout(struct wait_queue *wq, uint64_t ms) { (void)wq; (void)ms; abort(); }
/* The boot task waiting in keep_start: keep's thread has its round. */
void wq_nap(struct wait_queue *wq, volatile bool *kick, uint64_t ms) { (void)wq; (void)kick; (void)ms; keep_work(); }
static bool thread_made;
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) {
    (void)name; (void)fn; (void)arg;
    thread_made = true;
    return (struct task *)&thread_made; /* not run: the test runs its rounds */
}

/* ---- C: a table of documents ---- */
#define USER "C:\\Users\\User"
#define NOCTURNE USER "\\AppData\\Nocturne\\"
#define SETTINGS NOCTURNE "Settings.conf"
#define DESKTOP_OLD USER "\\AppData\\Settings\\desktop.conf"
static struct doc {
    char path[256];
    char *data;
    size_t n;
    unsigned writes;
} docs[16];
static unsigned save_attempts;
static int save_fails; /* the next this many store_save fail */

static struct doc *doc_at(const char *path) {
    for (size_t i = 0; i < ARRAY_SIZE(docs); i++)
        if (docs[i].data && !strcmp(docs[i].path, path)) return &docs[i];
    return NULL;
}
static void put_doc(const char *path, const char *text, size_t n) {
    struct doc *d = doc_at(path);
    for (size_t i = 0; !d && i < ARRAY_SIZE(docs); i++)
        if (!docs[i].data) d = &docs[i];
    snprintf(d->path, sizeof d->path, "%s", path);
    free(d->data);
    d->data = malloc(n + 1);
    memcpy(d->data, text, n);
    d->data[n] = 0;
    d->n = n;
}
static void put(const char *path, const char *text) { put_doc(path, text, strlen(text)); }
static const char *text_of(const char *path) {
    struct doc *d = doc_at(path);
    return d ? d->data : NULL;
}
static unsigned writes_of(const char *path) {
    struct doc *d = doc_at(path);
    return d ? d->writes : 0;
}

const char *store_user_folder(void) { return USER; }
int store_folder_create(const char *path, bool user) { (void)path; (void)user; return -N_ERR_EXISTS; }
int store_document_open(const char *path, int access, bool user, struct handle **out) {
    (void)user;
    if (access != N_DOCUMENT_READ) return -N_ERR_INVALID;
    struct doc *d = doc_at(path);
    if (!d) return -N_ERR_NOT_FOUND;
    struct handle *h = calloc(1, sizeof *h);
    h->priv = d;
    *out = h;
    return 0;
}
int64_t store_document_size(struct handle *h) { return (int64_t)((struct doc *)h->priv)->n; }
int64_t store_document_read(struct handle *h, uint64_t at, void *buf, size_t n, bool user) {
    (void)user;
    struct doc *d = h->priv;
    if (at >= d->n) return 0;
    n = MIN(n, d->n - at);
    memcpy(buf, d->data + at, n);
    return (int64_t)n;
}
void handle_close(struct handle *h) { free(h); }
int store_save(const char *path, const void *data, size_t n, bool user) {
    (void)user;
    save_attempts++;
    if (save_fails > 0) {
        save_fails--;
        return -N_ERR_IO;
    }
    put_doc(path, data, n);
    doc_at(path)->writes++;
    return 0;
}
static unsigned removes;
int store_entry_remove(const char *path, bool user) {
    (void)user;
    struct doc *d = doc_at(path);
    if (!d) return -N_ERR_NOT_FOUND;
    free(d->data);
    d->data = NULL;
    removes++;
    return 0;
}

/* ---- parts as the system's are: their own keys, checked; "applied" what they took, in order ---- */
struct part {
    struct keep keep;
    const char *const *keys; /* the part's; a key ending in '.' is a prefix */
    int value[4];            /* by key */
    char applied[256];
    unsigned long_lines;     /* lines of filler its text adds */
};
static bool is_key(const char *pattern, const char *key) {
    size_t n = strlen(pattern);
    return pattern[n - 1] == '.' ? !strncmp(key, pattern, n) && key[n] : !strcmp(key, pattern);
}
static bool any_line(struct part *p, const char *key, const char *value, bool apply) {
    for (int i = 0; p->keys[i]; i++) {
        if (!is_key(p->keys[i], key)) continue;
        int64_t v;
        if (apply && keep_number(value, -100, 100, &v)) {
            p->value[i] = (int)v;
            snprintf(p->applied + strlen(p->applied), sizeof p->applied - strlen(p->applied), "%s=%d;", key, (int)v);
            keep_changed(&p->keep); /* as a part does: its setter says it changed (ignored while reading) */
        }
        return true;
    }
    return false;
}
static size_t any_text(struct part *p, char *out, size_t cap) {
    size_t n = 0;
    keep_put(out, cap, &n, "# %s\n", p->keep.name);
    for (int i = 0; p->keys[i]; i++)
        if (p->keys[i][strlen(p->keys[i]) - 1] != '.') keep_put(out, cap, &n, "%s=%d\n", p->keys[i], p->value[i]);
    for (unsigned i = 0; i < p->long_lines; i++) keep_put(out, cap, &n, "# filler line %04u ........................\n", i);
    return n;
}

static const char *const desktop_keys[] = {"pointer_speed", "smooth_gradients", "refresh", NULL};
static const char *const sound_keys[] = {"volume", "muted", NULL};
static const char *const displays_keys[] = {"mode.", NULL};
static const char *const later_keys[] = {"later", NULL};
static const char *const big_keys[] = {"big", NULL};
static struct part desktop, sound, displays, later, big;
#define PART_FNS(p)                                                                                         \
    static bool p##_line(const char *key, const char *value, bool apply) { return any_line(&p, key, value, apply); } \
    static size_t p##_text(char *out, size_t cap) { return any_text(&p, out, cap); }
PART_FNS(desktop)
PART_FNS(sound)
PART_FNS(displays)
PART_FNS(later)
PART_FNS(big)
static void part_init(struct part *p, const char *name, const char *const *keys,
                      bool (*line)(const char *, const char *, bool), size_t (*text)(char *, size_t)) {
    memset(p, 0, sizeof *p);
    p->keep = (struct keep){.name = name, .line = line, .text = text};
    p->keys = keys;
}
static void change(struct part *p, int i, int v) {
    if (p->value[i] == v) return;
    p->value[i] = v;
    keep_changed(&p->keep);
}

/* ---- the cases ---- */

static void helpers(void) {
    int64_t v = 7;
    CHECK(keep_number("0", -2, 2, &v) && v == 0);
    CHECK(keep_number("-2", -2, 2, &v) && v == -2);
    CHECK(keep_number("100", 0, 100, &v) && v == 100);
    v = 7;
    CHECK(!keep_number("3", -2, 2, &v) && v == 7);
    CHECK(!keep_number("", 0, 9, &v) && !keep_number("-", 0, 9, &v) && !keep_number("1x", 0, 9, &v) &&
          !keep_number(" 1", 0, 9, &v) && !keep_number("+1", 0, 9, &v) && !keep_number("1234567890123456789", 0, 9, &v));
    char out[8];
    size_t n = 0;
    keep_put(out, sizeof out, &n, "ab%d", 12);
    CHECK(n == 4 && !strcmp(out, "ab12"));
    keep_put(out, sizeof out, &n, "%s", "cdefgh");
    CHECK(n == 10 && !strcmp(out, "ab12cde")); /* what fits, a zero, and all of it counted */
    keep_put(out, sizeof out, &n, "%s", "ij");
    CHECK(n == 12 && !strcmp(out, "ab12cde"));
}

static char seen[512];
static void collect(const char *key, const char *value, void *arg) {
    (void)arg;
    snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "[%s|%s]", key, value);
}
static void lines(void) {
    char text[] = "# a note=not a line\r\n\r\n  a = 1 \r\n\tno equals sign\n=no key\nb=\n c = x = y \n  # note\nlast=9";
    each_line(text, collect, NULL);
    CHECK(!strcmp(seen, "[a|1][b|][c|x = y][last|9]"));
    if (strcmp(seen, "[a|1][b|][c|x = y][last|9]")) fprintf(stderr, "  %s\n", seen);
}

/* Desktop has its own document; Sound has none and takes its lines from the newest older document
   (Settings.conf before desktop.conf), which stays: languages= is no part's. */
static void start(void) {
    put(NOCTURNE "Desktop.conf", "# mine\npointer_speed=fast\r\npointer_speed=1\n");
    put(SETTINGS, "# The user's settings.\nlanguages=ja,en-US\npointer_speed=-2\nvolume=35\nmuted=1\n");
    put(DESKTOP_OLD, "volume=10\n");
    part_init(&desktop, "Desktop", desktop_keys, desktop_line, desktop_text);
    part_init(&sound, "Sound", sound_keys, sound_line, sound_text);
    keep_register(&desktop.keep);
    keep_register(&sound.keep);
    CHECK(wakes == 0); /* nothing runs before the start */
    timer_ticks = 100;
    change(&sound, 0, 50); /* before its document is read: not kept */
    CHECK(!sound.keep.dirty && !sound.keep.read);
    keep_start();
    CHECK(thread_made && ready && desktop.keep.read && sound.keep.read);
    CHECK(desktop.value[0] == 1 && !strcmp(desktop.applied, "pointer_speed=1;")); /* "fast" is no speed; -2 not its */
    CHECK(sound.value[0] == 35 && sound.value[1] == 1 && !strcmp(sound.applied, "volume=35;muted=1;"));
    CHECK(text_of(NOCTURNE "Sound.conf") && !strcmp(text_of(NOCTURNE "Sound.conf"), "# Sound\nvolume=35\nmuted=1\n"));
    CHECK(writes_of(NOCTURNE "Desktop.conf") == 0); /* read back, not written again */
    CHECK(text_of(SETTINGS) && text_of(DESKTOP_OLD) && removes == 0);
    CHECK(strstr(log_text, "keep: Desktop read from " NOCTURNE "Desktop.conf\n") &&
          strstr(log_text, "keep: Sound taken over from " SETTINGS "\n") &&
          strstr(log_text, "keep: " SETTINGS " stays: a line in it is no part's\n"));
    CHECK(!sound.keep.dirty && !desktop.keep.dirty && next_work() == UINT64_MAX);
}

/* Changes one after another: one write KEEP_AFTER_MS after the last; keep_flush at once. */
static void changes(void) {
    unsigned w = writes_of(NOCTURNE "Sound.conf"), before = wakes;
    timer_ticks = 1000;
    change(&sound, 0, 40);
    timer_ticks = 1300;
    change(&sound, 0, 41);
    CHECK(wakes == before + 2 && sound.keep.dirty && next_work() == 1300 + KEEP_AFTER_MS);
    timer_ticks = 1799;
    keep_work();
    CHECK(writes_of(NOCTURNE "Sound.conf") == w);
    timer_ticks = 1800;
    keep_work();
    CHECK(writes_of(NOCTURNE "Sound.conf") == w + 1 && strstr(text_of(NOCTURNE "Sound.conf"), "\nvolume=41\n"));
    CHECK(!sound.keep.dirty && next_work() == UINT64_MAX);
    timer_ticks = 2500;
    keep_work(); /* nothing changed: nothing written */
    CHECK(writes_of(NOCTURNE "Sound.conf") == w + 1);

    timer_ticks = 3000;
    change(&desktop, 1, 1);
    keep_flush();
    CHECK(writes_of(NOCTURNE "Desktop.conf") == 1 && strstr(text_of(NOCTURNE "Desktop.conf"), "\nsmooth_gradients=1\n") &&
          strstr(text_of(NOCTURNE "Desktop.conf"), "\npointer_speed=1\n"));
    timer_ticks = 3600;
    keep_work();
    CHECK(writes_of(NOCTURNE "Desktop.conf") == 1);
}

/* A write that fails keeps the change for the next change or keep_flush, not a loop of tries. */
static void failures(void) {
    unsigned w = writes_of(NOCTURNE "Sound.conf");
    timer_ticks = 4000;
    change(&sound, 1, 0);
    save_fails = 1;
    unsigned tries = save_attempts;
    timer_ticks = 4500;
    keep_work();
    CHECK(save_attempts == tries + 1 && writes_of(NOCTURNE "Sound.conf") == w);
    CHECK(sound.keep.dirty && sound.keep.retry && next_work() == UINT64_MAX);
    CHECK(strstr(log_text, "keep: " NOCTURNE "Sound.conf cannot be saved"));
    timer_ticks = 9000;
    keep_work();
    CHECK(save_attempts == tries + 1); /* not tried again by itself */
    change(&sound, 0, 42);
    CHECK(!sound.keep.retry && next_work() == 9000 + KEEP_AFTER_MS);
    timer_ticks = 9500;
    keep_work();
    CHECK(writes_of(NOCTURNE "Sound.conf") == w + 1 && strstr(text_of(NOCTURNE "Sound.conf"), "\nvolume=42\nmuted=0\n"));

    timer_ticks = 10000;
    change(&sound, 0, 43);
    save_fails = 1;
    keep_flush();
    CHECK(sound.keep.dirty && sound.keep.retry);
    keep_flush(); /* before the power goes, it tries again */
    CHECK(!sound.keep.dirty && writes_of(NOCTURNE "Sound.conf") == w + 2 && strstr(text_of(NOCTURNE "Sound.conf"), "\nvolume=43\n"));
}

/* A part registered after the start is read at once. Without Settings.conf it takes its lines from
   desktop.conf, whose every line is a part's: it goes once the part was written. The parts with a
   document of their own only say the line is theirs. */
static void later_part(void) {
    store_entry_remove(SETTINGS, false);
    unsigned r = removes;
    put(DESKTOP_OLD, "# Nocturne desktop settings, written by Settings\npointer_speed=-2\nvolume=10\n"
                     "mode.DEL4321=25\nrefresh=60\nmode.QEMU=12\n");
    part_init(&displays, "Displays", displays_keys, displays_line, displays_text);
    unsigned before = wakes;
    keep_register(&displays.keep);
    CHECK(wakes == before + 1 && !displays.keep.read && next_work() == 0);
    keep_work();
    CHECK(displays.keep.read && !strcmp(displays.applied, "mode.DEL4321=25;mode.QEMU=12;"));
    CHECK(desktop.value[0] == 1 && sound.value[0] == 43 && !strstr(desktop.applied, "-2") && !strstr(sound.applied, "10"));
    CHECK(writes_of(NOCTURNE "Displays.conf") == 1 && removes == r + 1 && !text_of(DESKTOP_OLD));
    CHECK(strstr(log_text, "keep: " DESKTOP_OLD " removed"));
}

/* An older document stays while a part that took lines from it could not be written. A document
   too long to be a part's is not read, and its part is not given an older one's lines. A text
   longer than the first room is asked for again with room for it. */
static void edges(void) {
    put(SETTINGS, "later=5\n");
    part_init(&later, "Later", later_keys, later_line, later_text);
    later.long_lines = 100; /* about 4.5 KiB */
    save_fails = 1;
    keep_register(&later.keep);
    keep_work();
    CHECK(later.keep.read && later.value[0] == 5 && later.keep.dirty && later.keep.retry && text_of(SETTINGS));
    keep_flush();
    CHECK(!later.keep.dirty && text_of(NOCTURNE "Later.conf") && strlen(text_of(NOCTURNE "Later.conf")) > TEXT_FIRST * 4 &&
          strstr(text_of(NOCTURNE "Later.conf"), "\nlater=5\n# filler line 0000"));
    CHECK(text_of(SETTINGS)); /* removed by the next round that reads it, not by a flush */

    static char huge[READ_MOST + 2];
    memset(huge, '#', sizeof huge - 1);
    put(NOCTURNE "Big.conf", huge);
    part_init(&big, "Big", big_keys, big_line, big_text);
    put(SETTINGS, "big=3\n");
    keep_register(&big.keep);
    keep_work();
    CHECK(big.keep.read && !big.applied[0] && text_of(SETTINGS));
    CHECK(strstr(log_text, "keep: " NOCTURNE "Big.conf cannot be read"));
    timer_ticks = 20000;
    change(&big, 0, 4); /* read, though not from C:: its changes are kept */
    timer_ticks = 20000 + KEEP_AFTER_MS;
    keep_work();
    CHECK(text_of(NOCTURNE "Big.conf") && !strcmp(text_of(NOCTURNE "Big.conf"), "# Big\nbig=4\n"));
}

int main(void) {
    helpers();
    lines();
    start();
    changes();
    failures();
    later_part();
    edges();
    if (failed) fprintf(stderr, "keep said:\n%s", log_text);
    printf("keep-hosttest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
