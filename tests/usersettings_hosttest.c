/* The kernel's user settings (kernel/src/sys/usersettings.c) on the host: the language tags as the
   system writes them and the lists it refuses, the saved file read back into the owners (an older
   Settings app's desktop.conf too) and written out, watches and what they count, and the AP read of
   the languages. The owners (desktop, sound, screen modes), C:, handles and the scheduler are
   stand-ins; nothing here runs the settings thread, a real C: or a guest.
     clang -std=c2x -Ikernel/src -Icommon -o build/host/usersettings-hosttest.exe tests/usersettings_hosttest.c */
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
#include "../kernel/src/sys/usersettings.c"
#undef irq_save
#undef irq_restore

static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- the kernel's own ---- */
volatile uint64_t timer_ticks;
uint64_t hhdm_offset;
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }
int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
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
void wq_nap(struct wait_queue *wq, volatile bool *kick, uint64_t ms) { (void)wq; (void)kick; (void)ms; abort(); }
struct task *kthread_create(const char *name, void (*fn)(void *), void *arg) { (void)name; (void)fn; (void)arg; return NULL; }
int copy_from_user(void *dst, const void *src, size_t n) { if (!src && n) return -N_ERR_BAD_ADDRESS; memcpy(dst, src, n); return 0; }
int copy_to_user(void *dst, const void *src, size_t n) { if (!dst && n) return -N_ERR_BAD_ADDRESS; memcpy(dst, src, n); return 0; }
static bool user_mapped = true;
bool vmm_ap_user_begin(const void *p, size_t n, bool write) { (void)p; (void)n; (void)write; return user_mapped; }
void vmm_ap_uaccess_end(void) {}

/* ---- handles: one table ---- */
static struct handle *table[16];
static unsigned notified;
struct handle *handle_open(struct object *o, bool writable) {
    struct handle *h = calloc(1, sizeof *h);
    h->object = o;
    h->refs = 1;
    h->writable = writable;
    return h;
}
int handle_install(struct handle *h) {
    for (int i = 0; i < 16; i++)
        if (!table[i]) { table[i] = h; return i; }
    return -N_ERR_NO_HANDLE;
}
struct handle *handle_get(int i) {
    if (i < 0 || i >= 16 || !table[i]) return NULL;
    table[i]->refs++;
    return table[i];
}
void handle_close(struct handle *h) {
    if (--h->refs) return;
    if (h->object->ops->close) h->object->ops->close(h);
    free(h);
}
static void handle_remove_(int i) { handle_close(table[i]); table[i] = NULL; }
void object_notify(struct object *o) { (void)o; notified++; }

/* ---- the owners ---- */
static int speed;
static bool smooth;
static int volume = 80, volume_sets;
static bool muted;
static char order[64]; /* which owner was set, in order: s(oun)d volume, m(ute) */
void wm_desktop_settings(int *s, bool *g) { *s = speed; *g = smooth; }
void wm_desktop_settings_set(int s, bool g) { speed = s; smooth = g; }
int audio_volume(void) { return volume; }
bool audio_muted(void) { return muted; }
void audio_set_volume(int v) { volume = v; muted = false; volume_sets++; strcat(order, "v"); }
void audio_set_muted(bool m) { muted = m; strcat(order, "m"); }
static struct { char monitor[32]; struct n_screen_mode mode; } chosen[4];
static unsigned nchosen;
void screen_modes_choose(const char *monitor, const struct n_screen_mode *m) {
    snprintf(chosen[nchosen].monitor, sizeof chosen[nchosen].monitor, "%s", monitor);
    chosen[nchosen++].mode = *m;
}
bool screen_modes_choice(unsigned i, char monitor[32], struct n_screen_mode *m) {
    if (i >= nchosen) return false;
    memcpy(monitor, chosen[i].monitor, 32);
    *m = chosen[i].mode;
    return true;
}

/* ---- C: ---- */
const char *store_user_folder(void) { return "C:\\Users\\User"; }
static char saved_name[512], saved_text[TEXT_MAX];
int store_save(const char *name, const void *data, size_t n, bool user) {
    (void)user;
    snprintf(saved_name, sizeof saved_name, "%s", name);
    memcpy(saved_text, data, n);
    saved_text[n] = 0;
    return 0;
}
static char removed[512];
int store_entry_remove(const char *name, bool user) { (void)user; snprintf(removed, sizeof removed, "%s", name); return 0; }
int store_folder_create(const char *name, bool user) { (void)name; (void)user; return 0; }
int store_document_open(const char *name, int access, bool user, struct handle **out) {
    (void)name; (void)access; (void)user; (void)out;
    return -N_ERR_NOT_FOUND;
}
int64_t store_document_size(struct handle *h) { (void)h; return -N_ERR_IO; }
int64_t store_document_read(struct handle *h, uint64_t at, void *buf, size_t n, bool user) {
    (void)h; (void)at; (void)buf; (void)n; (void)user;
    return -N_ERR_IO;
}

/* ---- the cases ---- */
static bool canonical(const char *in, const char *want) {
    char out[N_USER_LANGUAGES_MAX];
    int64_t n = list_canonical(in, strlen(in), out, sizeof out);
    bool ok = want ? n == (int64_t)strlen(want) && !strcmp(out, want) : n == -N_ERR_INVALID;
    if (!ok) fprintf(stderr, "  \"%s\": %lld \"%s\"\n", in, (long long)n, n >= 0 ? out : "");
    return ok;
}

static void tags(void) {
    CHECK(canonical("en-US", "en-US"));
    CHECK(canonical("  JA , en-us,ja,\tzh-hant-tw , es-419,x-Private-Use,DE-ch-1996",
                    "ja,en-US,zh-Hant-TW,es-419,x-private-use,de-CH-1996"));
    CHECK(canonical("en-a-bb-cc,sr-Latn-RS", "en-a-bb-cc,sr-Latn-RS"));
    CHECK(canonical("i-klingon,yue,fil,EN", "i-klingon,yue,fil,en"));
    CHECK(canonical("en,en-US,EN", "en,en-US")); /* a tag again, not one that starts the same */
    CHECK(canonical("en-US,en", "en-US,en"));
    static const char *const bad[] = {"", ",", "en,,ja", "en,", ",en", "e", "1en", "en-", "-en", "en--us", "en-x",
                                      "en_US", "en US", "toolongtag", "en-123456789", "ja,\xc3\xa9n", "x"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(canonical(bad[i], NULL));
    char out[8];
    CHECK(list_canonical("en-US,ja", 8, out, sizeof out) == -N_ERR_TOO_LARGE);
    CHECK(list_canonical("en-US", 5, out, 6) == 5 && !strcmp(out, "en-US"));
}

static void languages_calls(void) {
    char buf[N_USER_LANGUAGES_MAX];
    CHECK(user_languages(buf, sizeof buf) == 5 && !strcmp(buf, "en-US")); /* the default */
    CHECK(user_languages(buf, 5) == -N_ERR_TOO_LARGE && user_languages(buf, 6) == 5);
    unsigned before = notified;
    CHECK(user_languages_set("ja, en-us", 9) == 0 && user_languages(buf, sizeof buf) == 8 && !strcmp(buf, "ja,en-US"));
    CHECK(notified == before + 1);
    CHECK(user_languages_set("ja,en-US", 8) == 0 && notified == before + 1); /* the same list: no change */
    CHECK(user_languages_set("ja,,en", 6) == -N_ERR_INVALID && user_languages(buf, sizeof buf) == 8);
    CHECK(user_languages_set("en", N_USER_LANGUAGES_MAX) == -N_ERR_TOO_LARGE);
    CHECK(user_languages_set(NULL, 4) == -N_ERR_BAD_ADDRESS);
    int64_t r = 0;
    CHECK(user_languages_ap(buf, sizeof buf, &r) && r == 8 && !strcmp(buf, "ja,en-US"));
    CHECK(user_languages_ap(buf, 8, &r) && r == -N_ERR_TOO_LARGE);
    user_mapped = false;
    CHECK(!user_languages_ap(buf, sizeof buf, &r)); /* the BSP serves it */
    user_mapped = true;
    languages_seq++; /* a writer in the middle of a change */
    CHECK(!user_languages_ap(buf, sizeof buf, &r));
    languages_seq++;
}

static void watches(void) {
    CHECK(user_settings_watch(0) == -N_ERR_INVALID && user_settings_watch(16) == -N_ERR_INVALID);
    int64_t words = user_settings_watch(N_USER_SETTING_LANGUAGES), all = user_settings_watch(N_USER_SETTINGS_ALL);
    CHECK(words >= 0 && all >= 0);
    if (words < 0 || all < 0) return;
    CHECK(!watch_ready(table[words]) && !watch_ready(table[all]) && user_settings_changes((int)words) == 0);
    user_settings_changed(N_USER_SETTING_SOUND);
    CHECK(!watch_ready(table[words]) && watch_ready(table[all]) == N_EVENT_RECEIVABLE);
    user_settings_changed(N_USER_SETTING_LANGUAGES | N_USER_SETTING_POINTER);
    CHECK(watch_ready(table[words]) == N_EVENT_RECEIVABLE);
    CHECK(user_settings_changes((int)all) == (N_USER_SETTING_SOUND | N_USER_SETTING_LANGUAGES | N_USER_SETTING_POINTER));
    CHECK(user_settings_changes((int)all) == 0 && !watch_ready(table[all]));
    CHECK(user_settings_changes((int)words) == N_USER_SETTING_LANGUAGES && !watch_ready(table[words]));
    CHECK(user_settings_changes(15) == -N_ERR_NO_HANDLE);
    struct object other = {.ops = &(struct object_ops){.kind = N_KIND_EVENTS}};
    table[15] = handle_open(&other, false);
    CHECK(user_settings_changes(15) == -N_ERR_WRONG_KIND);
    handle_remove_(15);
    handle_remove_((int)words);
    handle_remove_((int)all);
}

static void file(void) {
    /* What an older Settings app wrote, read back into the owners */
    char old[] = "# Nocturne desktop settings, written by Settings\r\n"
                 "pointer_speed=1\r\ndeep_colour=on\r\n  volume = 35 \r\nmuted=on\r\nrefresh=60\r\n"
                 "mode.DEL4321ABCD=2560x1440@143912\nmode.bad=0x1440@60000\nmode.=1x1@1\nmode.x=1x1@1x\n"
                 "languages=fr-ca, en\npointer_speed=7\nno line end";
    order[0] = 0;
    apply(old);
    CHECK(speed == 1 && smooth && volume == 35 && muted && !strcmp(order, "vm"));
    CHECK(nchosen == 1 && !strcmp(chosen[0].monitor, "DEL4321ABCD") && chosen[0].mode.width == 2560 &&
          chosen[0].mode.height == 1440 && chosen[0].mode.refresh_mhz == 143912);
    char buf[N_USER_LANGUAGES_MAX];
    CHECK(user_languages(buf, sizeof buf) > 0 && !strcmp(buf, "fr-CA,en"));
    /* smooth_gradients over deep_colour, a bad volume and bad languages left as they were */
    char newer[] = "smooth_gradients=off\ndeep_colour=on\nvolume=101\nlanguages=,\npointer_speed=-2\n";
    order[0] = 0;
    apply(newer);
    CHECK(speed == -2 && !smooth && volume == 35 && !order[0] && user_languages(buf, sizeof buf) > 0 &&
          !strcmp(buf, "fr-CA,en"));

    /* and written out */
    char text[TEXT_MAX];
    size_t n = settings_text(text, sizeof text);
    const char *want = "# The user's settings. Nocturne keeps this file: change them in Settings.\n"
                       "languages=fr-CA,en\npointer_speed=-2\nsmooth_gradients=off\nvolume=35\nmuted=on\n"
                       "mode.DEL4321ABCD=2560x1440@143912\n";
    CHECK(n == strlen(want) && !strcmp(text, want));
    if (strcmp(text, want)) fprintf(stderr, "%s", text);
    /* what it writes it reads back the same */
    speed = 0, smooth = true, volume = 80, muted = false, nchosen = 0;
    char again[TEXT_MAX];
    memcpy(again, text, n + 1);
    apply(again);
    char text2[TEXT_MAX];
    settings_text(text2, sizeof text2);
    CHECK(!strcmp(text, text2));
}

static void saving(void) {
    snprintf(path, sizeof path, "C:\\Users\\User\\AppData\\Nocturne\\Settings.conf");
    snprintf(old_path, sizeof old_path, "C:\\Users\\User\\AppData\\Settings\\desktop.conf");
    /* before the start, a change is counted but not saved */
    unsigned w = wakes;
    user_settings_changed(N_USER_SETTING_SOUND);
    CHECK(!dirty && wakes == w);
    user_settings_flush();
    CHECK(!saved_name[0]);
    started = true;
    migrating = true;
    timer_ticks = 1000;
    user_settings_changed(N_USER_SETTING_SOUND);
    CHECK(dirty && dirty_at == 1000 && wakes == w + 1);
    user_settings_flush();
    CHECK(!dirty && !strcmp(saved_name, path) && strstr(saved_text, "\nvolume=35\nmuted=on\n"));
    CHECK(!strcmp(removed, old_path) && !migrating); /* one place keeps them from now on */
    removed[0] = saved_name[0] = 0;
    user_settings_flush(); /* nothing changed: nothing written */
    CHECK(!saved_name[0] && !removed[0]);
}

int main(void) {
    tags();
    languages_calls();
    watches();
    file();
    saving();
    printf("usersettings-hosttest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
