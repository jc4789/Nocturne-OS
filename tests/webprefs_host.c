/* webprefs（ブラウザーの設定）のホストの試験。製品の user/libc/webprefs.c をそのまま組み、nocturne_settings.h の口だけを
   記憶の中の値の表に差し替える（check_webprefs.py）。見張りは無い扱いで、別のプロセスの書き込みは試験が値の表を
   直に書き換えて表す。範囲を限った裏付けで、サービス settings・ゲストのブラウザーでの受け入れではない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webprefs.h>
#include <webcookie.h>
#include <nocturne_settings.h>
#include <threads.h>
#include <nocturne_language.h>

enum { KEYS = 8 };
static struct { const struct n_setting_decl *decl; char *bytes; size_t length; bool held; } table[KEYS];
static unsigned writes, conflicts_left;

n_result n_setting_declare(const struct n_setting_decl *d, size_t count, uint32_t *reason) {
    for (size_t i = 0; i < count; i++) table[d[i].key].decl = &d[i];
    if (reason) *reason = N_SETTING_OK;
    return 0;
}
n_result n_setting_get(struct n_setting_id id, void *out, size_t cap) {
    if (id.key >= KEYS || !table[id.key].decl) return -N_ERR_NOT_FOUND;
    const void *from = table[id.key].bytes;
    size_t n = table[id.key].length;
    int32_t fallback = (int32_t)table[id.key].decl->fallback;
    if (!table[id.key].held) {
        if (table[id.key].decl->type == N_SETTING_STRING) {
            from = table[id.key].decl->fallback_text;
            n = strlen(from);
        } else {
            from = &fallback;
            n = sizeof fallback;
        }
    }
    if (n > cap) return -N_ERR_TOO_LARGE;
    if (n) memcpy(out, from, n);
    return (n_result)n;
}
n_result n_setting_get_int(struct n_setting_id id, int32_t *out) {
    n_result r = n_setting_get(id, out, sizeof *out);
    return r < 0 ? r : 0;
}
static void put(uint32_t key, const void *value, size_t length) {
    free(table[key].bytes);
    table[key].bytes = malloc(length ? length : 1);
    if (length) memcpy(table[key].bytes, value, length);
    table[key].length = length;
    table[key].held = true;
}
n_result n_setting_set(struct n_setting_id id, const void *value, size_t length, uint32_t *reason) {
    if (reason) *reason = N_SETTING_OK;
    if (id.key >= KEYS || !table[id.key].decl) return -N_ERR_NOT_FOUND;
    if (table[id.key].decl->type == N_SETTING_STRING && (int64_t)length > table[id.key].decl->max) {
        if (reason) *reason = N_SETTING_OUT_OF_RANGE;
        return -N_ERR_INVALID;
    }
    put(id.key, value, length);
    writes++;
    return 0;
}
n_result n_setting_set_if(struct n_setting_id id, const void *expected, size_t expected_length, const void *value, size_t length,
                          uint32_t *reason) {
    char now[N_SETTING_VALUE_MAX];
    n_result n = n_setting_get(id, now, sizeof now);
    if (n < 0) return n;
    if (conflicts_left) {
        conflicts_left--;
        if (reason) *reason = N_SETTING_NOT_THE_SAME;
        return -N_ERR_BUSY;
    }
    if ((size_t)n != expected_length || memcmp(now, expected, expected_length)) {
        if (reason) *reason = N_SETTING_NOT_THE_SAME;
        return -N_ERR_BUSY;
    }
    return n_setting_set(id, value, length, reason);
}
size_t n_locale_languages(char *out, size_t cap) { return (size_t)snprintf(out, cap, "%s", "ja"); }
n_handle n_setting_watch(uint32_t domain, uint32_t key) { (void)domain; (void)key; return -N_ERR_NOT_FOUND; }
n_result n_setting_watch_take(n_handle watch, struct n_settings_changed *out) { (void)watch; (void)out; return -N_ERR_NOT_READY; }
n_result n_handle_close(n_handle h) { (void)h; return 0; }
n_result n_events_watch(n_handle q, n_handle h, uint32_t events, uint64_t token) { (void)q; (void)h; (void)events; (void)token; return -N_ERR_WRONG_KIND; }
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 0; }
int mtx_lock(mtx_t *m) { (void)m; return 0; }
int mtx_unlock(mtx_t *m) { (void)m; return 0; }

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL webprefs:%d: %s\n", __LINE__, #c); failed++; } } while (0)

/* 鍵の番号は webprefs.c の表の順。 */
enum { KEY_LANGUAGES, KEY_SEARCH_HISTORY, KEY_CLEAR_ON_CLOSE, KEY_SITE_EXCEPTIONS };
static void other_process(const char *sites) {
    put(KEY_SITE_EXCEPTIONS, sites, strlen(sites));
    webprefs_sync();
}
static bool sites_are(const char *text) {
    return table[KEY_SITE_EXCEPTIONS].held && table[KEY_SITE_EXCEPTIONS].length == strlen(text) &&
           !memcmp(table[KEY_SITE_EXCEPTIONS].bytes, text, strlen(text));
}
static bool host_is(const char *input, const char *expected) {
    char out[WEBPREFS_HOST_MAX + 1];
    n_result r = webprefs_site_host(input, out);
    return expected ? r == 0 && !strcmp(out, expected) : r == -N_ERR_INVALID;
}

static void languages(void) {
    char list[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1], accept[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1];
    CHECK(webprefs_languages(list, sizeof list) > 0 && !strcmp(list, "ja"));
    CHECK(webprefs_languages_default());
    CHECK(webprefs_accept_language(accept, sizeof accept) > 0 && !strcmp(accept, "ja"));
    uint64_t generation = webprefs_languages_generation();
    const char *tags[] = {"en-US", "de"};
    CHECK(webprefs_set_languages(tags, 2) == 0);
    CHECK(webprefs_languages_generation() != generation);
    CHECK(webprefs_languages(list, sizeof list) > 0 && !strcmp(list, "en-US,de") && !webprefs_languages_default());
    CHECK(webprefs_accept_language(accept, sizeof accept) > 0 && !strcmp(accept, "en-US,de;q=0.9"));
    const char *unknown[] = {"xx-unknown"};
    CHECK(webprefs_set_languages(unknown, 1) == -N_ERR_INVALID);
    const char *twice[] = {"de", "de"};
    CHECK(webprefs_set_languages(twice, 2) == -N_ERR_INVALID);
    CHECK(webprefs_set_languages(NULL, 0) == -N_ERR_INVALID);
    CHECK(webprefs_reset_languages() == 0 && webprefs_languages_default());
    CHECK(!webprefs_search_history());
    CHECK(webprefs_set_search_history(true) == 0 && webprefs_search_history());
    CHECK(webprefs_set_search_history(false) == 0 && !webprefs_search_history());
}

int main(void) {
    /* 値が無い：全部既定。 */
    CHECK(webprefs_site_rule("example.com") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_site_kept("example.com") && !webprefs_clear_on_close() && !webprefs_site_data_transient());
    CHECK(webprefs_cookie_rule(NULL, "example.com") == WEBCOOKIE_ALLOW);
    CHECK(webprefs_site_exceptions(NULL, 0) == 0 && !writes);
    uint64_t generation = webprefs_site_generation();

    languages();

    /* Block covers the host and its subdomains, cookies and storage alike. */
    CHECK(webprefs_set_site_rule("example.com", WEBPREFS_SITE_BLOCK) == 0);
    CHECK(sites_are("block:example.com"));
    CHECK(webprefs_site_generation() != generation);
    CHECK(webprefs_site_rule("example.com") == WEBPREFS_SITE_BLOCK);
    CHECK(webprefs_site_rule("WWW.Example.COM.") == WEBPREFS_SITE_BLOCK);
    CHECK(webprefs_site_rule("badexample.com") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_site_rule("example.org") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_cookie_rule(NULL, "a.example.com") == WEBCOOKIE_BLOCK);
    CHECK(!webprefs_storage_allowed("https://x.example.com/a?b"));
    CHECK(!webprefs_storage_allowed("HTTP://user@example.com:8080/"));
    CHECK(webprefs_site_rule_url("wss://example.com:443/socket") == WEBPREFS_SITE_BLOCK);
    CHECK(webprefs_storage_allowed("https://example.org/"));
    CHECK(webprefs_storage_allowed("file:///C:/Users/User/a.html"));
    CHECK(webprefs_site_kept("example.com")); /* blocked data stays while nothing is cleared on close */

    /* The most specific exception applies. */
    CHECK(webprefs_set_site_rule("www.example.com", WEBPREFS_SITE_ALLOW) == 0);
    CHECK(webprefs_site_rule("a.www.example.com") == WEBPREFS_SITE_ALLOW);
    CHECK(webprefs_site_rule("b.example.com") == WEBPREFS_SITE_BLOCK);
    CHECK(sites_are("block:example.com allow:www.example.com"));

    /* 別のプロセスの例外は残る：変更は今の値へ当てる。競り負ければ読み直す。 */
    other_process("block:example.com allow:www.example.com session:other.org");
    CHECK(webprefs_site_rule("x.other.org") == WEBPREFS_SITE_SESSION);
    conflicts_left = 2;
    CHECK(webprefs_set_site_rule("third.net", WEBPREFS_SITE_SESSION) == 0);
    CHECK(sites_are("block:example.com session:other.org session:third.net allow:www.example.com"));
    CHECK(webprefs_site_data_transient());
    CHECK(!webprefs_site_kept("other.org") && webprefs_site_kept("example.org"));
    CHECK(webprefs_cookie_rule(NULL, "a.other.org") == WEBCOOKIE_TRANSIENT);

    /* Clearing on close: only allowed hosts are kept; a blocked host is transient too. */
    generation = webprefs_site_generation();
    CHECK(webprefs_set_clear_on_close(true) == 0 && webprefs_clear_on_close());
    CHECK(webprefs_site_generation() != generation);
    CHECK(!webprefs_site_kept("example.org") && webprefs_site_kept("www.example.com"));
    CHECK(webprefs_cookie_rule(NULL, "example.org") == WEBCOOKIE_TRANSIENT);
    CHECK(webprefs_cookie_rule(NULL, "b.example.com") == (WEBCOOKIE_BLOCK | WEBCOOKIE_TRANSIENT));
    CHECK(webprefs_cookie_rule(NULL, "www.example.com") == WEBCOOKIE_ALLOW);
    CHECK(webprefs_set_clear_on_close(false) == 0 && !webprefs_clear_on_close());

    /* DEFAULT removes an exception; the last one leaves the empty list. */
    struct webprefs_site_exception list[8];
    CHECK(webprefs_site_exceptions(list, 8) == 4 && !strcmp(list[0].host, "example.com") && list[3].rule == WEBPREFS_SITE_ALLOW);
    CHECK(webprefs_set_site_rule("example.com", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_site_rule("b.example.com") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_set_site_rule("www.example.com", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_set_site_rule("other.org", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_set_site_rule("third.net", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_site_exceptions(NULL, 0) == 0 && sites_are(""));

    /* Items that are not "rule:host" are left out, a host twice counts once (the first). */
    other_process("block:example.com bogus allow:Bad_Host deny:x.org session:ok.org allow:example.com");
    CHECK(webprefs_site_exceptions(list, 8) == 2 && webprefs_site_rule("example.com") == WEBPREFS_SITE_BLOCK);
    CHECK(webprefs_site_rule("ok.org") == WEBPREFS_SITE_SESSION);

    /* An address covers only itself. */
    CHECK(webprefs_set_site_rule("192.168.0.1", WEBPREFS_SITE_BLOCK) == 0);
    CHECK(webprefs_site_rule("192.168.0.1") == WEBPREFS_SITE_BLOCK && webprefs_site_rule("1.192.168.0.1") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_set_site_rule("[::1]", WEBPREFS_SITE_BLOCK) == 0 && webprefs_site_rule_url("http://[::1]:8080/") == WEBPREFS_SITE_BLOCK);
    CHECK(webprefs_set_site_rule("Bad_Host", WEBPREFS_SITE_BLOCK) == -N_ERR_INVALID);
    CHECK(webprefs_set_site_rule("Example.com", WEBPREFS_SITE_BLOCK) == -N_ERR_INVALID); /* webprefs_site_host's form only */

    /* What the user typed. */
    CHECK(host_is("https://User@WWW.Example.COM:8080/path?q#f", "www.example.com"));
    CHECK(host_is("  example.com.  ", "example.com"));
    CHECK(host_is("*.example.com", "example.com"));
    CHECK(host_is(".example.com", "example.com"));
    CHECK(host_is("example.com/path", "example.com"));
    CHECK(host_is("[::1]:80", "[::1]"));
    CHECK(host_is("10.0.0.1:8080", "10.0.0.1"));
    CHECK(host_is("xn--wgv71a.jp", "xn--wgv71a.jp"));
    CHECK(host_is("", NULL));
    CHECK(host_is("bad_host.com", NULL));
    CHECK(host_is("-bad.com", NULL));
    CHECK(host_is("\xe6\x97\xa5\xe6\x9c\xac.jp", NULL));
    CHECK(host_is("https://", NULL));

    printf("webprefs: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
