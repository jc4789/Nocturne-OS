/* The system's own state on the real kernel (docs/nocturne-native-abi.md §12): the watches on the
 * sound and the screen and what they hear, and that the parts that own the volume and the pointer's
 * speed keep a change in their own documents in the user's AppData\Nocturne. Run on a test disk: it
 * changes the volume, muting and the pointer's speed, and puts them back at the end.
 *
 * "systemwatchtest placed": scripts/test.py also placed, before the start, a Desktop.conf of the
 * desktop's own and an older system's AppData\Settings\desktop.conf (pointer_speed=-2, volume=80,
 * muted=off, refresh=60); the start must have read the desktop's own document, taken the sound over
 * from the older one into Sound.conf, and removed the older one. It says nothing about the Settings
 * window. Prints "systemwatchtest: N checks, M failed". */
#include "nocturne.h"
#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("systemwatchtest FAIL line %d\n", __LINE__); } } while (0)

/* The user's AppData\Nocturne\<name>, or AppData\<rel> with rel holding a folder. */
static bool app_data(const char *rel, char *out, size_t cap) {
    char name[256];
    snprintf(name, sizeof name, "AppData\\%s", rel);
    return n_path_join(n_user_folder(), name, out, cap) >= 0;
}

/* The whole document at path with a zero after it (free it), or NULL. */
static char *text_of(const char *path) {
    void *data;
    size_t size;
    if (n_document_load(path, &data, &size) < 0) return NULL;
    char *text = malloc(size + 1);
    if (text) {
        if (size) memcpy(text, data, size);
        text[size] = 0;
    }
    if (data) n_memory_release(data, size);
    return text;
}

/* Whether AppData\<rel> holds line within ms (a part writes its document a moment after the last
   change). */
static bool kept(const char *rel, const char *line, unsigned ms) {
    char path[1024];
    if (!app_data(rel, path, sizeof path)) return false;
    for (uint64_t end = n_clock_uptime_ms() + ms;; n_sleep_ms(100)) {
        char *text = text_of(path);
        bool found = text && strstr(text, line);
        free(text);
        if (found) return true;
        if (n_clock_uptime_ms() >= end) {
            printf("systemwatchtest: %s does not say \"%s\"\n", path, line + 1);
            return false;
        }
    }
}

/* Whether q has a handle ready within ms. */
static bool ready(n_handle q, int64_t ms) {
    struct n_event ev[4];
    return n_events_wait(q, ev, 4, ms) > 0;
}

static struct n_audio_output sound(void) {
    struct n_audio_output o = {0};
    n_audio_output(&o, 0);
    return o;
}
static n_result set_volume(int v) {
    struct n_audio_output o = {.volume = v};
    return n_audio_output(&o, N_AUDIO_SET_VOLUME);
}
static n_result set_muted(bool m) {
    struct n_audio_output o = {.muted = m};
    return n_audio_output(&o, N_AUDIO_SET_MUTED);
}

static n_result desktop(struct desktop_settings *ds) {
    return (n_result)__syscall(SYS_SCREEN_INFO, (long)ds, sizeof *ds, SCREEN_SETTINGS_GET, 0, 0);
}
static n_result set_pointer_speed(int speed) {
    struct desktop_settings ds;
    if (desktop(&ds) < 0) return -N_ERR_UNSUPPORTED;
    ds.pointer_speed = (int8_t)speed;
    return (n_result)__syscall(SYS_SCREEN_INFO, (long)&ds, sizeof ds, SCREEN_SETTINGS_SET, 0, 0);
}

/* What the start did with the documents scripts/test.py placed. */
static void taken_over(void) {
    struct desktop_settings ds;
    CHECK(desktop(&ds) == 0 && ds.pointer_speed == 1); /* the desktop's own document, not the older one's -2 */
    if (ds.pointer_speed != 1) printf("systemwatchtest: the pointer's speed is %d after the start\n", ds.pointer_speed);
    struct n_audio_output o = sound();
    CHECK(o.volume == 80 && !o.muted);

    char path[1024];
    struct n_entry e;
    CHECK(app_data("Settings\\desktop.conf", path, sizeof path) && n_entry_info(path, &e) == -N_ERR_NOT_FOUND);
    CHECK(kept("Nocturne\\Sound.conf", "\nvolume=80\n", 0) && kept("Nocturne\\Sound.conf", "\nmuted=off\n", 0));

    /* what the start said: in the newest system log, or in this start's System.log further back */
    static const char *const said[] = {
        "keep: Desktop read from ",
        "keep: Sound taken over from ",
        "desktop.conf removed: what it kept is in each part's own document",
    };
    char *log = malloc(65536 + 1);
    n_result n = log ? n_system_log(log, 65536) : -N_ERR_NO_MEMORY;
    if (n >= 0) log[n] = 0;
    char *kept_log = NULL;
    if (app_data("Nocturne\\System.log", path, sizeof path)) kept_log = text_of(path);
    for (size_t i = 0; i < sizeof said / sizeof *said; i++) {
        bool found = (n >= 0 && strstr(log, said[i])) || (kept_log && strstr(kept_log, said[i]));
        if (!found) printf("systemwatchtest: the system log does not say \"%s\"\n", said[i]);
        CHECK(found);
    }
    free(log);
    free(kept_log);
}

static void watches(void) {
    n_handle q = n_events_create();
    CHECK(q >= 0);
    if (q < 0) return;
    CHECK(n_watch_take(q) == -N_ERR_WRONG_KIND);
    CHECK(n_watch_take(0x7ffffff0) == -N_ERR_NO_HANDLE);

    n_handle loud = n_audio_output_watch(), screen = n_screen_watch();
    n_handle qs = n_events_create();
    CHECK(loud >= 0 && screen >= 0 && qs >= 0);
    if (loud < 0 || screen < 0 || qs < 0) {
        printf("systemwatchtest: the watches: %s, %s\n", n_result_text(loud), n_result_text(screen));
        return;
    }
    CHECK(n_handle_kind(loud) == N_KIND_WATCH && n_handle_kind(screen) == N_KIND_WATCH);
    CHECK(n_events_watch(q, loud, N_EVENT_RECEIVABLE, 1) == 0 && n_events_watch(qs, screen, N_EVENT_RECEIVABLE, 2) == 0);
    /* fresh: nothing heard yet */
    CHECK(!ready(q, 0) && n_watch_take(loud) == 0);
    n_watch_take(screen); /* a moment of the desktop's own (a size, a scanout) is not this test's */

    const struct n_audio_output before = sound();
    struct desktop_settings ds_before;
    CHECK(desktop(&ds_before) == 0);

    /* the volume: heard by the sound's watch, not the screen's */
    int v = before.volume == 37 ? 38 : 37;
    CHECK(set_volume(v) == 0 && sound().volume == v);
    CHECK(ready(q, 1000));
    CHECK(n_watch_take(loud) >= 1);
    CHECK(n_watch_take(loud) == 0 && !ready(q, 0));
    CHECK(!ready(qs, 200));
    /* the same value is no change */
    CHECK(set_volume(v) == 0 && !ready(q, 200) && n_watch_take(loud) == 0);
    /* muting */
    CHECK(set_muted(true) == 0 && sound().muted);
    CHECK(ready(q, 1000) && n_watch_take(loud) >= 1);
    CHECK(set_muted(true) == 0 && !ready(q, 200));

    /* the pointer's speed: heard by the screen's watch, not the sound's */
    int speed = ds_before.pointer_speed == 1 ? -1 : 1;
    CHECK(set_pointer_speed(speed) == 0);
    struct desktop_settings ds;
    CHECK(desktop(&ds) == 0 && ds.pointer_speed == speed);
    CHECK(ready(qs, 1000) && n_watch_take(screen) >= 1);
    CHECK(!ready(q, 200) && n_watch_take(loud) == 0);

    /* each kept by its part, in its own document */
    char line[64];
    snprintf(line, sizeof line, "\nvolume=%d\n", v);
    CHECK(kept("Nocturne\\Sound.conf", line, 3000));
    CHECK(kept("Nocturne\\Sound.conf", "\nmuted=on\n", 3000));
    snprintf(line, sizeof line, "\npointer_speed=%d\n", speed);
    CHECK(kept("Nocturne\\Desktop.conf", line, 3000));

    /* a watch closed is gone from the queue it was in */
    n_handle q2 = n_events_create(), gone = n_audio_output_watch();
    CHECK(q2 >= 0 && gone >= 0 && n_events_watch(q2, gone, N_EVENT_RECEIVABLE, 3) == 0);
    CHECK(n_handle_close(gone) == 0);
    CHECK(set_volume(v == 37 ? 36 : 37) == 0 && !ready(q2, 300));
    CHECK(ready(q, 1000)); /* while the one still open hears it */
    n_handle_close(q2);

    /* back as it was, and kept so */
    CHECK(set_volume(before.volume) == 0 && (!before.muted || set_muted(true) == 0));
    CHECK(set_pointer_speed(ds_before.pointer_speed) == 0);
    snprintf(line, sizeof line, "\nvolume=%d\n", (int)before.volume);
    CHECK(kept("Nocturne\\Sound.conf", line, 3000));
    snprintf(line, sizeof line, "\npointer_speed=%d\n", ds_before.pointer_speed);
    CHECK(kept("Nocturne\\Desktop.conf", line, 3000));

    n_handle_close(loud);
    n_handle_close(screen);
    n_handle_close(q);
    n_handle_close(qs);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "placed")) taken_over();
    watches();
    printf("systemwatchtest: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
