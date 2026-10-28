/* webprefs のサイトの例外と「閉じるときに消す」を、製品の user/libc/webprefs.c をそのまま組み、
   文書の口（n_document_*）だけを記憶の中の一つの文書に差し替えて確かめる（check_webprefs_sites.py）。
   見張りは無い扱い（N_ERR_UNSUPPORTED）なので、各読みは文書を一度読み、webprefs_sync で読み直す。
   別の窓のプロセスの書き込みは、試験が文書を直に書き換えて表す。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webprefs.h>
#include <webcookie.h>
#include <threads.h>

static char *doc;
static size_t doc_n;
static bool doc_exists;
static unsigned saves;

const char *web_profile_folder(bool temporary) { return temporary ? "C:\\Temp\\Browser" : "C:\\Users\\User\\AppData\\Browser"; }
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s\\%s", folder, name);
    return n < 0 || (size_t)n >= cap ? -N_ERR_TOO_LARGE : n;
}
n_handle n_document_open(const char *path, int access) {
    (void)path;
    if (access == N_DOCUMENT_CREATE) {
        if (doc_exists) return -N_ERR_EXISTS;
        doc_exists = true;
        return 7;
    }
    return doc_exists ? 7 : -N_ERR_NOT_FOUND;
}
n_result n_document_size(n_handle h) { (void)h; return (n_result)doc_n; }
n_result n_document_read_at(n_handle h, uint64_t at, void *buf, size_t n) {
    (void)h;
    if (at >= doc_n) return 0;
    if (n > doc_n - at) n = doc_n - (size_t)at;
    memcpy(buf, doc + at, n);
    return (n_result)n;
}
n_result n_document_claim(n_handle h, bool wait) { (void)h; (void)wait; return 0; }
n_result n_document_release(n_handle h) { (void)h; return 0; }
n_result n_document_save(const char *path, const void *data, size_t size) {
    (void)path;
    free(doc);
    doc = malloc(size ? size : 1);
    memcpy(doc, data, size);
    doc_n = size;
    doc_exists = true;
    saves++;
    return 0;
}
n_result n_handle_close(n_handle h) { (void)h; return 0; }
n_result n_folder_create(const char *path) { (void)path; return 0; }
n_handle n_entry_watch(const char *path, uint32_t what) { (void)path; (void)what; return -N_ERR_UNSUPPORTED; }
n_result n_events_watch(n_handle q, n_handle h, uint32_t events, uint64_t token) { (void)q; (void)h; (void)events; (void)token; return -N_ERR_WRONG_KIND; }
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 0; }
n_result n_watch_take(n_handle w) { (void)w; return 0; }
int mtx_lock(mtx_t *m) { (void)m; return 0; }
int mtx_unlock(mtx_t *m) { (void)m; return 0; }

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL webprefs_sites:%d: %s\n", __LINE__, #c); failed++; } } while (0)

static void write_doc(const char *text) {
    n_document_save(NULL, text, strlen(text));
    webprefs_sync();
}
static bool doc_has(const char *line) {
    char *text = malloc(doc_n + 2);
    text[0] = '\n';
    memcpy(text + 1, doc, doc_n);
    text[doc_n + 1] = 0;
    char want[512];
    snprintf(want, sizeof want, "\n%s\n", line);
    bool found = strstr(text, want) != NULL;
    free(text);
    return found;
}
static bool host_is(const char *input, const char *expected) {
    char out[WEBPREFS_HOST_MAX + 1];
    n_result r = webprefs_site_host(input, out);
    return expected ? r == 0 && !strcmp(out, expected) : r == -N_ERR_INVALID;
}

int main(void) {
    /* No document: every default, nothing written. */
    CHECK(webprefs_site_rule("example.com") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_site_kept("example.com") && !webprefs_clear_on_close() && !webprefs_site_data_transient());
    CHECK(webprefs_cookie_rule(NULL, "example.com") == WEBCOOKIE_ALLOW);
    CHECK(webprefs_site_exceptions(NULL, 0) == 0 && !doc_exists);
    uint64_t generation = webprefs_site_generation();

    /* Block covers the host and its subdomains, cookies and storage alike. */
    CHECK(webprefs_set_site_rule("example.com", WEBPREFS_SITE_BLOCK) == 0);
    CHECK(doc_has("site-data-exceptions=block:example.com"));
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
    CHECK(doc_has("site-data-exceptions=block:example.com allow:www.example.com"));

    /* Another process's exception written meanwhile stays: the change is made to the document. */
    write_doc("Nocturne browser preferences 1\nlanguages=ja,zh\nfuture-thing=1\n"
              "site-data-exceptions=block:example.com allow:www.example.com session:other.org\n");
    CHECK(webprefs_site_rule("x.other.org") == WEBPREFS_SITE_SESSION);
    CHECK(webprefs_set_site_rule("third.net", WEBPREFS_SITE_SESSION) == 0);
    CHECK(doc_has("site-data-exceptions=block:example.com session:other.org session:third.net allow:www.example.com"));
    CHECK(doc_has("future-thing=1") && doc_has("languages=ja,zh"));
    CHECK(webprefs_site_data_transient());
    CHECK(!webprefs_site_kept("other.org") && webprefs_site_kept("example.org"));
    CHECK(webprefs_cookie_rule(NULL, "a.other.org") == WEBCOOKIE_TRANSIENT);

    /* Clearing on close: only allowed hosts are kept; a blocked host is transient too. */
    generation = webprefs_site_generation();
    CHECK(webprefs_set_clear_on_close(true) == 0 && webprefs_clear_on_close());
    CHECK(webprefs_site_generation() != generation);
    CHECK(doc_has("clear-site-data-on-close=on"));
    CHECK(!webprefs_site_kept("example.org") && webprefs_site_kept("www.example.com"));
    CHECK(webprefs_cookie_rule(NULL, "example.org") == WEBCOOKIE_TRANSIENT);
    CHECK(webprefs_cookie_rule(NULL, "b.example.com") == (WEBCOOKIE_BLOCK | WEBCOOKIE_TRANSIENT));
    CHECK(webprefs_cookie_rule(NULL, "www.example.com") == WEBCOOKIE_ALLOW);
    CHECK(webprefs_set_clear_on_close(false) == 0 && !webprefs_clear_on_close());

    /* DEFAULT removes an exception; the last one removed removes the line. */
    struct webprefs_site_exception list[8];
    CHECK(webprefs_site_exceptions(list, 8) == 4 && !strcmp(list[0].host, "example.com") && list[3].rule == WEBPREFS_SITE_ALLOW);
    CHECK(webprefs_set_site_rule("example.com", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_site_rule("b.example.com") == WEBPREFS_SITE_DEFAULT);
    CHECK(webprefs_set_site_rule("www.example.com", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_set_site_rule("other.org", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_set_site_rule("third.net", WEBPREFS_SITE_DEFAULT) == 0);
    CHECK(webprefs_site_exceptions(NULL, 0) == 0 && !strstr(doc, "site-data-exceptions"));
    unsigned before = saves;
    CHECK(webprefs_set_site_rule("third.net", WEBPREFS_SITE_DEFAULT) == 0 && saves == before);

    /* Items that are not "rule:host" are left out, a host twice counts once (the first). */
    write_doc("Nocturne browser preferences 1\n"
              "site-data-exceptions=block:example.com bogus allow:Bad_Host deny:x.org session:ok.org allow:example.com\n");
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

    printf("webprefs_sites: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
