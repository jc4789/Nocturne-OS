/* tcptest: bulk TCP transfers against scripts/test.py's host server (10.0.2.2:PORT), clean and
   with simulated loss and reordering (SYS_NET_TEST), checking every byte.
   usage: tcptest [PORT]   (default: the port in /data/tests/tcpport)
   Server protocol: "GET n seed\n" -> n pattern bytes; "PUT n seed\n" + n bytes -> "OK n\n" or "BAD off\n". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define HOST 0x0A000202 /* 10.0.2.2 */

static int port, failed;

static uint8_t pat_next(uint32_t *x) {
    *x = *x * 1103515245u + 12345u;
    return (uint8_t)(*x >> 16);
}

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

static int conn(void) {
    int s = tcp_connect(HOST, (uint16_t)port, 5000);
    if (s < 0) printf("connect 10.0.2.2:%d: error %d\n", port, s);
    return s;
}

static bool send_all(int s, const void *buf, size_t n) {
    for (size_t done = 0; done < n;) {
        long r = tcp_send(s, (const char *)buf + done, n - done);
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

/* download n bytes and compare them with the pattern; returns ms, or -1 */
static long get(size_t n, uint32_t seed) {
    int s = conn();
    if (s < 0) return -1;
    char req[64];
    snprintf(req, sizeof req, "GET %lu %u\n", (unsigned long)n, seed);
    uint64_t t0 = uptime_ms();
    if (!send_all(s, req, strlen(req))) return tcp_close(s), -1;
    static uint8_t buf[16384];
    uint32_t x = seed;
    size_t got = 0;
    bool same = true;
    for (;;) {
        long r = tcp_recv(s, buf, sizeof buf, 30000);
        if (r <= 0) break;
        for (long i = 0; i < r; i++)
            if (buf[i] != pat_next(&x) && same) {
                printf("  byte %lu differs\n", (unsigned long)(got + i));
                same = false;
            }
        got += (size_t)r;
    }
    tcp_close(s);
    if (got != n) printf("  got %lu of %lu bytes\n", (unsigned long)got, (unsigned long)n);
    return same && got == n ? (long)(uptime_ms() - t0) : -1;
}

/* upload n pattern bytes; the server checks them */
static long put(size_t n, uint32_t seed) {
    int s = conn();
    if (s < 0) return -1;
    char req[64];
    snprintf(req, sizeof req, "PUT %lu %u\n", (unsigned long)n, seed);
    uint64_t t0 = uptime_ms();
    if (!send_all(s, req, strlen(req))) return tcp_close(s), -1;
    static uint8_t buf[16384];
    uint32_t x = seed;
    for (size_t done = 0; done < n;) {
        size_t k = n - done < sizeof buf ? n - done : sizeof buf;
        for (size_t i = 0; i < k; i++) buf[i] = pat_next(&x);
        if (!send_all(s, buf, k)) {
            printf("  send failed at %lu\n", (unsigned long)done);
            return tcp_close(s), -1;
        }
        done += k;
    }
    char reply[64];
    size_t rl = 0;
    long r;
    while (rl < sizeof reply - 1 && (r = tcp_recv(s, reply + rl, sizeof reply - 1 - rl, 30000)) > 0) rl += (size_t)r;
    reply[rl] = 0;
    tcp_close(s);
    char want[64];
    snprintf(want, sizeof want, "OK %lu\n", (unsigned long)n);
    if (strcmp(reply, want)) printf("  server said: %s\n", rl ? reply : "(nothing)");
    return strcmp(reply, want) ? -1 : (long)(uptime_ms() - t0);
}

static void report(const char *what, size_t n, long ms) {
    char line[160];
    if (ms < 0) snprintf(line, sizeof line, "%s", what);
    else snprintf(line, sizeof line, "%s: %lu KiB in %ld ms (%ld KiB/s)", what, (unsigned long)(n / 1024), ms,
                  (long)(n / 1024 * 1000 / (ms ? ms : 1)));
    check(ms >= 0, line);
}

static void faults(int rx_drop, int rx_reorder, int tx_drop) {
    struct n_netfault f = {(uint16_t)rx_drop, (uint16_t)rx_reorder, (uint16_t)tx_drop};
    net_test(&f, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1) port = atoi(argv[1]);
    else {
        FILE *f = fopen("/data/tests/tcpport", "r");
        char line[16] = "";
        if (f) fgets(line, sizeof line, f), fclose(f);
        if (!(port = atoi(line))) {
            fprintf(stderr, "usage: tcptest PORT\n");
            return 2;
        }
    }
    struct n_tcpstats a, b;
    if (net_test(NULL, &a) < 0) {
        printf("FAIL net_test syscall\n");
        return 1;
    }

    report("clean download", 4 << 20, get(4 << 20, 1));
    report("clean upload", 4 << 20, put(4 << 20, 2));

    /* the receiver: lost and reordered segments must be buffered and reassembled */
    net_test(NULL, &a);
    faults(30, 50, 0);
    report("download, 3% loss + 5% reordering", 2 << 20, get(2 << 20, 3));
    faults(0, 0, 0);
    net_test(NULL, &b);
    printf("  dropped %lu, reordered %lu, out-of-order segments kept %lu\n", (unsigned long)(b.faults_dropped - a.faults_dropped),
           (unsigned long)(b.faults_reordered - a.faults_reordered), (unsigned long)(b.ooo_in - a.ooo_in));
    check(b.ooo_in > a.ooo_in, "out-of-order segments were buffered");

    /* the sender: losses must be repaired, mostly by fast retransmit rather than timeouts */
    net_test(NULL, &a);
    faults(0, 0, 30);
    report("upload, 3% loss", 2 << 20, put(2 << 20, 4));
    faults(0, 0, 0);
    net_test(NULL, &b);
    unsigned long drop = (unsigned long)(b.faults_dropped - a.faults_dropped), fast = (unsigned long)(b.fast_retransmits - a.fast_retransmits),
                  retx = (unsigned long)(b.retransmits - a.retransmits), rto = (unsigned long)(b.timeouts - a.timeouts);
    printf("  dropped %lu, retransmitted %lu (%lu fast retransmits, %lu timeouts), dup acks %lu\n", drop, retx, fast, rto,
           (unsigned long)(b.dupacks_in - a.dupacks_in));
    check(fast > 0, "fast retransmit happened");
    check(fast > rto, "most losses were repaired without waiting for a timeout");

    /* both directions at once */
    faults(20, 20, 20);
    report("download, loss both ways", 1 << 20, get(1 << 20, 5));
    report("upload, loss both ways", 1 << 20, put(1 << 20, 6));
    faults(0, 0, 0);

    printf("tcptest: %d failed\n", failed);
    return failed != 0;
}
