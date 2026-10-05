/* fetch: download a URL over HTTP/1.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

static void usage(void) {
    fprintf(stderr, "usage: fetch [-i] [-o file] http://host[:port]/path\n"
                    "  -i  also print the response headers\n"
                    "  -o  save the body to a file instead of printing it\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *url = NULL, *out = NULL;
    bool headers = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) headers = true;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] == '-') usage();
        else url = argv[i];
    }
    if (!url) usage();
    if (!strncmp(url, "https://", 8)) {
        fprintf(stderr, "fetch: https is not supported (no TLS in Nocturne yet), try http://\n");
        return 1;
    }
    if (!strncmp(url, "http://", 7)) url += 7;

    char host[128], path[512];
    int port = 80;
    size_t hl = strcspn(url, ":/");
    if (hl == 0 || hl >= sizeof host) usage();
    memcpy(host, url, hl);
    host[hl] = 0;
    const char *p = url + hl;
    if (*p == ':') {
        port = atoi(p + 1);
        p += strcspn(p, "/");
    }
    snprintf(path, sizeof path, "%s", *p ? p : "/");

    if (!net_wait_up(5000)) {
        fprintf(stderr, "fetch: network is down\n");
        return 1;
    }
    uint32_t ip;
    if (net_resolve(host, &ip) < 0) {
        fprintf(stderr, "fetch: %s: %s\n", host, strerror(errno));
        return 1;
    }
    char ipbuf[16];
    fprintf(stderr, "connecting to %s (%s) port %d...\n", host, ip_format(ip, ipbuf), port);
    int s = tcp_connect(ip, (uint16_t)port, 10000);
    if (s < 0) {
        fprintf(stderr, "fetch: connect: %s\n", strerror(errno));
        return 1;
    }
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: fetch/1.0 (Nocturne OS)\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
    if (tcp_send(s, req, (size_t)n) < 0) {
        fprintf(stderr, "fetch: send: %s\n", strerror(errno));
        return 1;
    }

    FILE *f = stdout;
    if (out && !(f = fopen(out, "w"))) {
        fprintf(stderr, "fetch: %s: %s\n", out, strerror(errno));
        return 1;
    }
    /* read the header block first, then stream the body */
    static char buf[8192];
    size_t have = 0, total = 0;
    bool in_body = false;
    int status = 0;
    uint64_t t0 = uptime_ms();
    for (;;) {
        long r = tcp_recv(s, buf + have, sizeof buf - have - 1, 15000);
        if (r < 0) {
            fprintf(stderr, "fetch: %s\n", errno == EAGAIN ? "timed out" : strerror(errno));
            break;
        }
        if (r == 0) break;
        if (in_body) {
            fwrite(buf, 1, (size_t)r, f);
            total += (size_t)r;
            continue;
        }
        have += (size_t)r;
        buf[have] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (!end && have < sizeof buf - 1) continue;
        size_t head_len = end ? (size_t)(end - buf) + 4 : have;
        sscanf(buf, "HTTP/%*d.%*d %d", &status);
        if (headers) fwrite(buf, 1, head_len, out ? stderr : stdout);
        else if (status && status != 200) {
            char *eol = strstr(buf, "\r\n");
            if (eol) fprintf(stderr, "fetch: server says: %.*s\n", (int)(eol - buf), buf);
        }
        fwrite(buf + head_len, 1, have - head_len, f);
        total += have - head_len;
        in_body = true;
    }
    tcp_close(s);
    if (f != stdout) fclose(f);
    else fflush(stdout);
    uint64_t ms = uptime_ms() - t0;
    if (out)
        fprintf(stderr, "%zu bytes in %lu ms (%lu KB/s)\n", total, (unsigned long)ms,
                (unsigned long)(ms ? total / ms : total / 1));
    return status >= 200 && status < 400 ? 0 : 1;
}
