/* Every http(s) URL that WPT's url/resources/urltestdata.json expects the URL parser to give,
   through each native layer a URL reaches, all reading it from the URL record (weburl.h):
   - the request layer: http.c's http_target_of (the host, the Host field's authority, the port
     a connection goes to, the request target) and webfetch.c's http_url (the URL a request is
     made for, without its fragment, and its origin);
   - the cookie jar (webcookie.c): a host-only cookie set for the URL is sent back for it, and the
     jar files it under the site of the parser's host;
   - the origin entry: web_url_origin, the storage key check web_origin_http, same origin.
   tests/check_url_record.py writes the cases (url_record_cases.inc) and the slices of http.c and
   webfetch.c before this file. */

struct url_case { const char *href, *origin, *hostname, *host, *target, *url; int port; bool tls; };
#include "url_record_cases.inc"

static unsigned checks, failed;
static void check(bool ok, const struct url_case *c, const char *what) {
    checks++;
    if (!ok) { failed++; printf("FAIL %s: %s\n", c->href, what); }
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "rootfs/Nocturne/Web/public_suffix_list.dat", "rb");
    char *psl = f ? malloc(WEBCOOKIE_PSL_MAX + 1) : NULL;
    size_t psl_n = psl ? fread(psl, 1, WEBCOOKIE_PSL_MAX, f) : 0;
    if (f) fclose(f);
    webcookie_jar *jar = webcookie_create();
    if (!jar || !psl || !psl_n || !webcookie_psl_load(jar, psl, psl_n)) { printf("FAIL the cookie jar and its PSL\n"); return 1; }
    int64_t now = (int64_t)time(NULL);
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const struct url_case *c = &cases[i];

        struct http_target t;
        enum web_url_result r = http_target_of(c->href, &t);
        check(r == WEB_URL_TUPLE, c, "the request layer takes it");
        if (r == WEB_URL_TUPLE) {
            check(!strcmp(t.host, c->hostname), c, "the request's host is the parser's hostname");
            check(!strcmp(t.authority, c->host), c, "the Host field is the parser's host (and port), no credentials");
            check(t.port == c->port && t.tls == c->tls, c, "the connection's port and TLS are the URL's");
            check(!strcmp(t.path, c->target), c, "the request target is the path and query, no fragment");
            http_target_free(&t);
        }
        char *canonical = NULL, *origin = NULL;
        r = http_url(c->href, &canonical, &origin);
        check(r == WEB_URL_TUPLE && canonical && !strcmp(canonical, c->url), c, "the fetch process requests the URL without its fragment");
        check(r == WEB_URL_TUPLE && origin && !strcmp(origin, c->origin), c, "the fetch process's origin is the URL's");
        free(canonical);
        free(origin);

        origin = NULL;
        r = web_url_origin(c->href, &origin);
        check(r == WEB_URL_TUPLE && origin && !strcmp(origin, c->origin), c, "its origin serializes as HTML has it");
        check(web_origin_http(c->origin), c, "the storage layers take the origin as a key");
        check(origin && web_url_same_origin(c->href, origin) == 1, c, "the URL and its origin are same origin");
        free(origin);

        struct webcookie_context context = {c->href, c->href, "GET", true, true, false};
        check(webcookie_set(jar, &context, "wpt=1", 5, now) == 1, c, "the cookie jar sets a cookie for it");
        char out[64];
        check(webcookie_get(jar, &context, out, sizeof out, now) >= 0 && !strcmp(out, "wpt=1"), c, "the cookie jar sends it back");
        struct webcookie_site *sites = NULL;
        long n = webcookie_sites(jar, &sites, now);
        check(n == 1 && !strcmp(sites[0].site, webcookie_site_of(jar, c->hostname)), c, "the cookie's site is the site of the parser's host");
        free(sites);
        webcookie_clear(jar, NULL);
    }
    webcookie_free(jar);
    free(psl);
    printf("url_record: %zu URLs, %u checks, %u failed\n", sizeof cases / sizeof *cases, checks, failed);
    return failed != 0;
}
