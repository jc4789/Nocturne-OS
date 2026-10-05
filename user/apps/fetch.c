/* fetch: download a URL over HTTP or HTTPS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "nocturne.h"

static void usage(void) {
    fprintf(stderr, "usage: fetch [-i] [-o file] [-X method] [-H 'Name: value']... [-d data] url\n"
                    "  -i  also print the response headers\n"
                    "  -o  save the body to a file instead of printing it\n"
                    "  -d  send data as the request body (POST unless -X is given)\n");
    exit(2);
}

static int to_file(void *ctx, const char *data, size_t n) {
    return fwrite(data, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *url = NULL, *out = NULL, *method = NULL, *data = NULL;
    bool show_headers = false;
    static char headers[2048];
    size_t hl = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) show_headers = true;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-X") && i + 1 < argc) method = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) {
            int n = snprintf(headers + hl, sizeof headers - hl, "%s\r\n", argv[++i]);
            if (n > 0 && hl + n < sizeof headers) hl += n;
        } else if (argv[i][0] == '-') usage();
        else url = argv[i];
    }
    if (!url) usage();

    FILE *f = stdout;
    if (out && !(f = fopen(out, "w"))) {
        perror(out);
        return 1;
    }
    struct http_req rq = {.method = method ? method : data ? "POST" : "GET",
                          .url = url,
                          .headers = hl ? headers : NULL,
                          .body = data,
                          .body_len = data ? strlen(data) : 0,
                          .timeout_ms = 30000,
                          .on_body = to_file,
                          .ctx = f};
    struct http_resp rs;
    uint64_t t0 = uptime_ms();
    int r = http_request(&rq, &rs);
    if (f != stdout) fclose(f);
    else fflush(stdout);
    if (show_headers && rs.status) fprintf(stderr, "HTTP %d\n%s", rs.status, rs.headers);
    if (r < 0) {
        fprintf(stderr, "fetch: %s\n", rs.error);
        return 1;
    }
    if (rs.status >= 400 && !show_headers) fprintf(stderr, "fetch: HTTP status %d\n", rs.status);
    uint64_t ms = uptime_ms() - t0;
    if (out) fprintf(stderr, "%zu bytes in %lu ms\n", rs.body_len, (unsigned long)ms);
    return rs.status >= 200 && rs.status < 400 ? 0 : 1;
}
