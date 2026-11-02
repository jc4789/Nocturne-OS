/* Whether q has a handle ready within ms. */
static bool ready(n_handle q, uint64_t ms) {
    struct n_event ev[4];
    return n_events_wait(q, ev, 4, n_clock_after(ms * N_NS_PER_MS)) > 0;
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


    /* a watch closed is gone from the queue it was in */
    n_handle q2 = n_events_create(), gone = n_audio_output_watch();
    CHECK(q2 >= 0 && gone >= 0 && n_events_watch(q2, gone, N_EVENT_RECEIVABLE, 3) == 0);
    CHECK(n_handle_close(gone) == 0);
    CHECK(set_volume(v == 37 ? 36 : 37) == 0 && !ready(q2, 300));
    CHECK(ready(q, 1000)); /* while the one still open hears it */
    n_handle_close(q2);

    CHECK(set_volume(before.volume) == 0 && (!before.muted || set_muted(true) == 0));
    CHECK(set_pointer_speed(ds_before.pointer_speed) == 0);

    n_handle_close(loud);
    n_handle_close(screen);
    n_handle_close(q);
    n_handle_close(qs);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "placed"))    watches();
    printf("systemwatchtest: %u checks, %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
