/* Fetch's redirect mode "manual" at the native boundaries, on the host: webfetch.c's production
 * code before run_http (response_start, response_headers, body_cb; check_fetch_redirect_manual.py
 * extracts it) with the I/O it does not reach stubbed, the wire's flag rule (webnet_wire.h) and the
 * one reader of the transport's metadata line (njw_transport_flags, js_worker_wire.h) that js.c
 * and js_worker.c use. Not the redirect loop of run_http, not webnet.c's queue, not a guest
 * browser or a server. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define ssize_t nocturne_test_ssize_t
#include "../build/fetch-redirect-manual/webfetch-tested.inc"
#include "js_worker_wire.h"
int errno;
static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL redirect-manual:%d %s\n", __LINE__, #c); } } while (0)
/* The kernel and the jar are not reached by the steps under test; the stubs fail if they are. */
uint64_t n_clock_uptime_ms(void) { return 10; }
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach) { (void)h; (void)data; (void)n; (void)attach; return -N_ERR_INVALID; }
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, int64_t timeout_ms) { (void)q; (void)out; (void)max; (void)timeout_ms; return -N_ERR_INVALID; }
n_result n_handle_close(n_handle h) { (void)h; return 0; }
n_handle n_system_log_open(void) { return -N_ERR_INVALID; }
n_result webnet_wire_read(n_handle h, struct webnet_wire_in *in, void *dest, size_t n, int flags) { (void)h; (void)in; (void)dest; (void)n; (void)flags; return -N_ERR_INVALID; }
n_result webnet_wire_write(n_handle h, const void *data, size_t n) { (void)h; (void)data; (void)n; return -N_ERR_INVALID; }
bool webcookie_same_site(const webcookie_jar *jar, const char *a, const char *b) { (void)jar; return !strcmp(a, b); }
int webcookie_set(webcookie_jar *jar, const struct webcookie_context *c, const char *v, size_t len, int64_t now) { (void)jar; (void)c; (void)v; (void)len; (void)now; return -1; }
const char *http_response_headers(const struct http_resp *r) { return r->headers_full ? r->headers_full : r->headers; }
const char *http_response_status_text(const struct http_resp *r) { (void)r; return "Found"; }
/* A job of a fetch from https://site.test whose response arrived with no cookie events. */
static struct job fetch_job(uint32_t flags, const char *target) {
    struct job j = {.wire = {.kind = WEBNET_FETCH, .credentials = WEBNET_CREDENTIALS_SAME_ORIGIN,
        .user_navigation = WEBNET_WIRE_STATUS_LINE | flags, .deadline = 1000},
        .document = "https://site.test/page", .origin = "https://site.test", .hop_method = "GET"};
    j.target_origin = strdup(target);
    j.final_url = strdup("https://site.test/redirect?to=next");
    return j;
}
static void clear_job(struct job *j) {
    free(j->body); free(j->response_headers); free(j->target_origin); free(j->final_url);
}
int main(void) {
    /* The wire: "manual" only for a fetch with the metadata line, never with "error". */
    const uint32_t manual = WEBNET_WIRE_STATUS_LINE | WEBNET_WIRE_REDIRECT_MANUAL;
    CHECK(WEBNET_WIRE_REQUEST_FLAGS & WEBNET_WIRE_REDIRECT_MANUAL);
    CHECK(webnet_wire_redirect_flag_valid(WEBNET_FETCH, manual));
    CHECK(webnet_wire_redirect_flag_valid(WEBNET_NAVIGATION, WEBNET_WIRE_STATUS_LINE | WEBNET_WIRE_REDIRECT_ERROR));
    CHECK(!webnet_wire_redirect_flag_valid(WEBNET_NAVIGATION, manual));
    CHECK(!webnet_wire_redirect_flag_valid(WEBNET_RESOURCE, manual));
    CHECK(!webnet_wire_redirect_flag_valid(WEBNET_FETCH, WEBNET_WIRE_REDIRECT_MANUAL));
    CHECK(!webnet_wire_redirect_flag_valid(WEBNET_FETCH, manual | WEBNET_WIRE_REDIRECT_ERROR));
    CHECK(njw_load_kind_valid(2u | NJW_LOAD_REDIRECT_MANUAL) && njw_load_kind_valid(2u | NJW_LOAD_REDIRECT_ERROR));
    CHECK(!njw_load_kind_valid(2u | NJW_LOAD_REDIRECT_MANUAL | NJW_LOAD_REDIRECT_ERROR));
    CHECK(njw_loaded_kind_valid(NJW_LOADED_OPAQUE_REDIRECT) && !njw_loaded_kind_valid(NJW_LOADED_OPAQUE_REDIRECT | NJW_LOADED_CORS));
    CHECK(njw_frame_kind_valid(NJW_FRAME_HEADERS | NJW_LOADED_OPAQUE_REDIRECT) && !njw_frame_kind_valid(NJW_FRAME_CHUNK | NJW_LOADED_OPAQUE_REDIRECT));

    /* HTTP fetch, "manual": the redirect is the response; its fields and body stay here. */
    static char redirect_fields[] = "Location: /next\r\nX-Secret: never exposed\r\nSet-Cookie: a=b\r\n";
    struct http_resp redirect = {.status = 302, .headers_full = redirect_fields};
    struct job j = fetch_job(WEBNET_WIRE_REDIRECT_MANUAL, "https://site.test");
    CHECK(response_start(&j, &redirect) == 0 && j.opaque_redirect && j.stream_final && !j.cache_capture);
    CHECK(body_cb(&j, "<a href=/next>moved</a>", 23) == 0 && !j.body && j.body_len == 0);
    CHECK(response_headers(&j, &redirect, false) && j.response_headers);
    const char *block = j.response_headers ? j.response_headers : "";
    CHECK(!strncmp(block, "HTTP/1.1 302 Found\r\nHTTP/Nocturne-Meta cors=0 redirected=0 opaqueredirect=1\r\n", 74));
    CHECK(!strstr(block, "Location") && !strstr(block, "X-Secret") && !strstr(block, "Set-Cookie"));
    CHECK(njw_transport_flags(block) == (int)NJW_LOADED_OPAQUE_REDIRECT);
    clear_job(&j);
    /* With no Location it is still the redirect's response. */
    static char bare_fields[] = "Content-Type: text/plain\r\n";
    struct http_resp bare = {.status = 307, .headers_full = bare_fields};
    j = fetch_job(WEBNET_WIRE_REDIRECT_MANUAL, "https://site.test");
    CHECK(response_start(&j, &bare) == 0 && j.opaque_redirect);
    clear_job(&j);
    /* A CORS request: the CORS check comes first, so a redirect without
       Access-Control-Allow-Origin is a network error, not an opaque-redirect. */
    j = fetch_job(WEBNET_WIRE_REDIRECT_MANUAL, "https://other.test");
    j.cors_tainted = true;
    CHECK(response_start(&j, &redirect) == -1 && !j.opaque_redirect);
    clear_job(&j);
    /* "follow" leaves the redirect to run_http's loop; "error" fails. */
    j = fetch_job(0, "https://site.test");
    CHECK(response_start(&j, &redirect) == 1 && !j.opaque_redirect && !j.stream_final);
    clear_job(&j);
    j = fetch_job(WEBNET_WIRE_REDIRECT_ERROR, "https://site.test");
    CHECK(response_start(&j, &redirect) == -1 && strstr(j.error, "redirect mode"));
    clear_job(&j);
    /* A final response in "manual" mode is an ordinary one. */
    static char ok_fields[] = "Content-Type: text/plain\r\n";
    struct http_resp ok = {.status = 200, .headers_full = ok_fields};
    j = fetch_job(WEBNET_WIRE_REDIRECT_MANUAL, "https://site.test");
    CHECK(response_start(&j, &ok) == 0 && !j.opaque_redirect && response_headers(&j, &ok, false));
    CHECK(j.response_headers && strstr(j.response_headers, "Content-Type: text/plain\r\n") && njw_transport_flags(j.response_headers) == 0);
    clear_job(&j);

    /* The one reader of the metadata line. */
    CHECK(njw_transport_flags("HTTP/1.1 200 OK\r\nHTTP/Nocturne-Meta cors=1 redirected=1\r\n") == (int)(NJW_LOADED_CORS | NJW_LOADED_REDIRECTED));
    CHECK(njw_transport_flags("HTTP/1.1 200 OK\r\nHTTP/Nocturne-Meta cors=0 redirected=0 opaque=1 resource=5\r\n") ==
          (int)(NJW_LOADED_OPAQUE | (5u << NJW_LOADED_RESOURCE_SHIFT)));
    CHECK(njw_transport_flags("HTTP/1.1 200 OK\r\nHTTP/Nocturne-Meta cors=0 redirected=0 opaque=1\r\n") ==
          (int)(NJW_LOADED_OPAQUE | (2u << NJW_LOADED_RESOURCE_SHIFT)));
    CHECK(njw_transport_flags("HTTP/1.1 200 OK\r\nHTTP/Nocturne-Meta cors=0 redirected=0\r\nX: opaqueredirect=1\r\n") == 0);
    CHECK(njw_transport_flags("HTTP/1.1 200 OK\r\nX-Meta: HTTP/Nocturne-Meta cors=1\r\n") == -1 && njw_transport_flags(NULL) == -1);
    printf("Fetch redirect mode manual native boundaries: %u checks / %u failed\n", checks, failed);
    return failed ? 1 : 0;
}
