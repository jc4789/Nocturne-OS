#include "js_navigator_native.h"

/* One watch per process on the user's languages (N_USER_SETTING_LANGUAGES), in the queue the
   process's event loop waits on (the window's web_wait_queue, a Worker's own). Level-triggered:
   the loop that waits hands every report to web_js_languages_events, which takes the change. */
static n_handle watch = -1;
static uint64_t generation;

void web_js_languages_watch(n_handle queue) {
    if (watch >= 0 || queue < 0) return;
    n_handle h = n_user_settings_watch(N_USER_SETTING_LANGUAGES);
    if (h < 0) return;
    if (n_events_watch(queue, h, N_EVENT_RECEIVABLE, WEB_LANGUAGES_TOKEN) < 0) { n_handle_close(h); return; }
    watch = h;
}

void web_js_languages_events(const struct n_event *events, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (events[i].token != WEB_LANGUAGES_TOKEN) continue;
        n_result changed = n_user_settings_changes(watch);
        if (changed > 0 && (changed & N_USER_SETTING_LANGUAGES)) generation++;
        return;
    }
}

uint64_t web_js_languages_generation(void) { return generation; }
