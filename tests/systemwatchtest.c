/* The system's own state on the real kernel (docs/nocturne-native-abi.md §12): the watches on the
 * sound and the screen and what they hear. Run on a test disk: it changes the volume, muting and the
 * pointer's speed, and puts them back at the end. Prints "systemwatchtest: N checks, M failed". */
#include "nocturne.h"
#include "nocturne_settings.h"
#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("systemwatchtest FAIL line %d\n", __LINE__); } } while (0)

/* Whether q has a handle ready within ms. */
static bool ready(n_handle q, uint64_t ms) {
    struct n_event ev[4];
    return n_events_wait(q, ev, 4, n_clock_after(ms * N_NS_PER_MS)) > 0;
}

/* The volume and muting, settings of the service audio. */
static int32_t audio_setting(uint32_t key) {
    int32_t v = -1;
    n_setting_get_int(n_setting_at(N_DOMAIN_AUDIO, key), &v);
    return v;
}
static n_handle loud;
/* The changes the sound's watch has heard. */
static int loud_take(void) {
    struct n_settings_changed changed;
    int count = 0;
    while (n_setting_watch_take(loud, &changed) >= 0) count++;
    return count;
}
/* What the test run lets this test apply. */
static n_result apply(uint32_t domain, uint32_t key, int32_t value) {
    static n_handle cap = -1;
    if (cap < 0) cap = n_cap_find(N_CAP_SETTINGS_APPLY);
    return n_settings_apply(cap, domain, key, &value, sizeof value);
}
static n_result set_volume(int v) { return n_setting_set_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_VOLUME), v, NULL); }
static n_result set_muted(bool m) { return n_setting_set_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_MUTED), m, NULL); }
static n_result set_pointer_speed(int speed) { return apply(N_DOMAIN_INPUT, N_INPUT_POINTER_SPEED, speed); }

static n_result desktop(struct desktop_settings *ds) {
    return (n_result)__syscall(SYS_SCREEN_INFO, (long)ds, sizeof *ds, SCREEN_SETTINGS_GET, 0, 0);
}

static void watches(void) {
    n_handle q = n_events_create();
    CHECK(q >= 0);
    if (q < 0) return;
    CHECK(n_watch_take(q) == -N_ERR_WRONG_KIND);
    CHECK(n_watch_take(0x7ffffff0) == -N_ERR_NO_HANDLE);

    loud = n_setting_watch(N_DOMAIN_AUDIO, N_SETTING_ANY);
    n_handle screen = n_screen_watch();
    n_handle qs = n_events_create();
    CHECK(loud >= 0 && screen >= 0 && qs >= 0);
    if (loud < 0 || screen < 0 || qs < 0) {
        printf("systemwatchtest: the watches: %s, %s\n", n_result_text(loud), n_result_text(screen));
        return;
    }
    CHECK(n_handle_kind(screen) == N_KIND_WATCH);
    CHECK(n_events_watch(q, loud, N_EVENT_RECEIVABLE, 1) == 0 && n_events_watch(qs, screen, N_EVENT_RECEIVABLE, 2) == 0);
    /* fresh: nothing heard yet */
    CHECK(!ready(q, 0) && loud_take() == 0);
    n_watch_take(screen); /* a moment of the desktop's own (a size, a scanout) is not this test's */

    const int32_t before_volume = audio_setting(N_AUDIO_VOLUME), before_muted = audio_setting(N_AUDIO_MUTED);
    struct desktop_settings ds_before;
    CHECK(desktop(&ds_before) == 0);

    /* the volume: heard by the sound's watch, not the screen's */
    int v = before_volume == 37 ? 38 : 37;
    CHECK(set_volume(v) == 0 && audio_setting(N_AUDIO_VOLUME) == v);
    CHECK(ready(q, 1000));
    CHECK(loud_take() >= 1);
    CHECK(loud_take() == 0 && !ready(q, 0));
    CHECK(!ready(qs, 200));
    /* the same value is no change */
    CHECK(set_volume(v) == 0 && !ready(q, 200) && loud_take() == 0);
    /* muting */
    CHECK(set_muted(true) == 0 && audio_setting(N_AUDIO_MUTED) == 1);
    CHECK(ready(q, 1000) && loud_take() >= 1);
    CHECK(set_muted(true) == 0 && !ready(q, 200));

    /* the pointer's speed: heard by the screen's watch, not the sound's */
    int speed = ds_before.pointer_speed == 1 ? -1 : 1;
    CHECK(set_pointer_speed(speed) == 0);
    struct desktop_settings ds;
    CHECK(desktop(&ds) == 0 && ds.pointer_speed == speed);
    CHECK(ready(qs, 1000) && n_watch_take(screen) >= 1);
    CHECK(!ready(q, 200) && loud_take() == 0);

    /* a watch closed is gone from the queue it was in */
    n_handle q2 = n_events_create(), gone = n_setting_watch(N_DOMAIN_AUDIO, N_SETTING_ANY);
    CHECK(q2 >= 0 && gone >= 0 && n_events_watch(q2, gone, N_EVENT_RECEIVABLE, 3) == 0);
    CHECK(n_handle_close(gone) == 0);
    CHECK(set_volume(v == 37 ? 36 : 37) == 0 && !ready(q2, 300));
    CHECK(ready(q, 1000)); /* while the one still open hears it */
    n_handle_close(q2);

    /* the kernel's report of them, and the domain's watch */
    int32_t now = 0;
    CHECK(audio_setting(N_AUDIO_MUTED) == 1);
    CHECK(n_settings_effective(N_DOMAIN_INPUT, N_INPUT_POINTER_SPEED, &now, sizeof now) == sizeof now && now == speed);

    /* back as it was */
    CHECK(set_volume(before_volume) == 0 && set_muted(before_muted != 0) == 0);
    CHECK(set_pointer_speed(ds_before.pointer_speed) == 0);
    n_handle_close(loud);
    n_handle_close(screen);
    n_handle_close(q);
    n_handle_close(qs);
}

int main(void) {
    watches();
    printf("systemwatchtest: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
