/* The user's settings (docs/nocturne-native-abi.md §12) on the real kernel: the languages as the
 * system writes them, what it refuses, the watches and what they hear, and that the system itself
 * keeps a change in the user's AppData\Nocturne\Settings.conf. Run on a test disk: it changes the
 * user's languages and volume, and puts them back at the end. It says nothing about the Settings
 * window or the browser. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("usersettingstest FAIL line %d\n", __LINE__); } } while (0)

static n_result set(const char *list) { return n_user_languages_set(list, strlen(list)); }

static bool is(const char *want) {
    char list[N_USER_LANGUAGES_MAX];
    n_result n = n_user_languages(list, sizeof list);
    if (n < 0 || (size_t)n != strlen(want) || strcmp(list, want)) {
        printf("usersettingstest: languages are \"%s\" (%lld), not \"%s\"\n", n < 0 ? "" : list, (long long)n, want);
        return false;
    }
    return true;
}

/* Whether the watch in q is ready now. */
static bool ready(n_handle q) {
    struct n_event ev[4];
    return n_events_wait(q, ev, 4, 0) > 0;
}

/* Whether the saved file shows line within ms (the system saves a moment after the last change). */
static bool saved(const char *line, unsigned ms) {
    char path[1024];
    if (n_path_join(n_user_folder(), "AppData\\Nocturne\\Settings.conf", path, sizeof path) < 0) return false;
    for (uint64_t end = n_clock_uptime_ms() + ms;; n_sleep_ms(100)) {
        void *data;
        size_t size;
        if (n_document_load(path, &data, &size) >= 0) {
            char *text = malloc(size + 1);
            bool found = false;
            if (text) {
                memcpy(text, data, size);
                text[size] = 0;
                found = strstr(text, line) != NULL;
                free(text);
            }
            if (data) n_memory_release(data, size);
            if (found) return true;
        }
        if (n_clock_uptime_ms() >= end) return false;
    }
}

static void languages(void) {
    char before[N_USER_LANGUAGES_MAX];
    n_result n = n_user_languages(before, sizeof before);
    CHECK(n > 0 && (size_t)n == strlen(before));
    char small[2];
    CHECK(n_user_languages(small, sizeof small) == -N_ERR_TOO_LARGE || n < 2);
    CHECK(n_user_languages(NULL, 0) == -N_ERR_TOO_LARGE);
    CHECK(n_user_languages((char *)8, sizeof before) == -N_ERR_BAD_ADDRESS);

    /* written as BCP 47 writes them, spaces and a second "ja" dropped */
    CHECK(set("  JA , en-us,ja,\tzh-hant-tw , es-419,x-Private-Use,DE-ch-1996") == 0);
    CHECK(is("ja,en-US,zh-Hant-TW,es-419,x-private-use,de-CH-1996"));
    /* a singleton keeps the rest lower case: a two-letter subtag after it is no region */
    CHECK(set("en-a-bb-cc,sr-Latn-RS") == 0 && is("en-a-bb-cc,sr-Latn-RS"));

    /* refused, and the list stays as it was */
    static const char *const bad[] = {
        "", ",", "en,,ja", "en,", ",en", "e", "1en", "en-", "-en", "en--us", "en-x", "en_US", "en US",
        "toolongtag", "en-123456789", "ja,én",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        n_result r = set(bad[i]);
        if (r != -N_ERR_INVALID) printf("usersettingstest: \"%s\" gave %lld\n", bad[i], (long long)r);
        CHECK(r == -N_ERR_INVALID);
    }
    CHECK(is("en-a-bb-cc,sr-Latn-RS"));
    char *big = malloc(N_USER_LANGUAGES_MAX + 8);
    if (big) {
        memset(big, 'a', N_USER_LANGUAGES_MAX + 7);
        for (size_t i = 2; i < N_USER_LANGUAGES_MAX + 7; i += 3) big[i] = ',';
        CHECK(n_user_languages_set(big, N_USER_LANGUAGES_MAX + 7) == -N_ERR_TOO_LARGE);
        free(big);
    }
    CHECK(n_user_languages_set((const char *)8, 10) == -N_ERR_BAD_ADDRESS);

    /* the system keeps the change itself */
    CHECK(set("ja,en-US") == 0 && is("ja,en-US"));
    CHECK(saved("\nlanguages=ja,en-US\n", 3000));

    /* a light call: every request a browser makes asks for them */
    char list[N_USER_LANGUAGES_MAX];
    uint64_t t0 = n_clock_uptime_ms();
    for (int i = 0; i < 20000; i++) n_user_languages(list, sizeof list);
    uint64_t ms = n_clock_uptime_ms() - t0;
    printf("usersettingstest: 20000 n_user_languages in %llu ms\n", (unsigned long long)ms);

    CHECK(n_user_languages_set(before, (size_t)n) == 0 && is(before));
}

static void watches(void) {
    CHECK(n_user_settings_watch(0) == -N_ERR_INVALID);
    CHECK(n_user_settings_watch(N_USER_SETTINGS_ALL + 1) == -N_ERR_INVALID);
    n_handle q = n_events_create();
    n_handle words = n_user_settings_watch(N_USER_SETTING_LANGUAGES);
    n_handle loud = n_user_settings_watch(N_USER_SETTING_SOUND);
    CHECK(q >= 0 && words >= 0 && loud >= 0);
    if (q < 0 || words < 0 || loud < 0) return;
    CHECK(n_handle_kind(words) == N_KIND_USER_SETTINGS);
    CHECK(n_user_settings_changes(q) == -N_ERR_WRONG_KIND);
    CHECK(n_events_watch(q, words, N_EVENT_RECEIVABLE, 1) == 0);
    n_handle q2 = n_events_create();
    CHECK(q2 >= 0 && n_events_watch(q2, loud, N_EVENT_RECEIVABLE, 2) == 0);
    CHECK(!ready(q) && !ready(q2) && n_user_settings_changes(words) == 0);

    char before[N_USER_LANGUAGES_MAX];
    n_result n = n_user_languages(before, sizeof before);
    CHECK(n > 0);
    /* the same list again is no change */
    CHECK(n_user_languages_set(before, (size_t)n) == 0 && !ready(q));
    CHECK(set(strcmp(before, "fr") ? "fr" : "de") == 0);
    struct n_event ev[4];
    CHECK(n_events_wait(q, ev, 4, 1000) == 1 && ev[0].token == 1 && ev[0].events == N_EVENT_RECEIVABLE);
    CHECK(!ready(q2)); /* a watch hears only of its interest */
    CHECK(ready(q)); /* level-triggered: until it takes the change */
    CHECK(n_user_settings_changes(words) == N_USER_SETTING_LANGUAGES);
    CHECK(n_user_settings_changes(words) == 0 && !ready(q));
    CHECK(n_user_languages_set(before, (size_t)n) == 0 && ready(q) &&
          n_user_settings_changes(words) == N_USER_SETTING_LANGUAGES);

    /* the volume, whoever changes it */
    struct n_audio_output out = {0};
    CHECK(n_audio_output(&out, 0) == 0);
    struct n_audio_output io = out;
    io.volume = out.volume == 37 ? 38 : 37;
    CHECK(n_audio_output(&io, N_AUDIO_SET_VOLUME) == 0);
    CHECK(n_events_wait(q2, ev, 4, 1000) == 1 && n_user_settings_changes(loud) == N_USER_SETTING_SOUND);
    CHECK(!ready(q));
    char line[32];
    snprintf(line, sizeof line, "\nvolume=%d\n", io.volume);
    CHECK(saved(line, 3000));
    io = out;
    CHECK(n_audio_output(&io, N_AUDIO_SET_VOLUME | N_AUDIO_SET_MUTED) == 0);
    CHECK(n_user_settings_changes(loud) == N_USER_SETTING_SOUND);

    /* a closed watch is gone from the queue */
    CHECK(n_handle_close(words) == 0);
    CHECK(set(strcmp(before, "fr") ? "fr" : "de") == 0 && !ready(q));
    CHECK(n_user_languages_set(before, (size_t)n) == 0);
    n_handle_close(loud);
    n_handle_close(q);
    n_handle_close(q2);
}

int main(void) {
    languages();
    watches();
    printf("usersettingstest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
