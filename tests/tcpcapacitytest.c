/* Real Nocturne global-TCB pressure, cooperative wait/kill and webfetch recovery.
   Uses scripts/test.py's TCP server and HTTP fixture; no POSIX socket layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"
#include "webnet.h"

#define HOST 0x0a000202
#define CAPACITY 16 /* finite native TCB budget, not the file-descriptor table */
static int checks, failed, tcp_port, web_port;
static webnet *net;
struct result { int calls, status; char error[160]; bool body_ok; };
static void check(bool ok, const char *name) {
    checks++; printf("%s tcp capacity %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failed++;
}
static void completed(webnet *n, uint64_t id, uint64_t generation, const struct webnet_response *r, void *opaque) {
    (void)n; (void)id; (void)generation;
    struct result *out = opaque; out->calls++; out->status = r->status;
    snprintf(out->error, sizeof out->error, "%s", r->error);
    const char expected[] = "export const value = 42;\n";
    out->body_ok = r->body_len == sizeof expected - 1 && r->body && !memcmp(r->body, expected, sizeof expected - 1);
}
static uint64_t request(struct result *r, uint64_t generation) {
    char url[256]; snprintf(url, sizeof url, "http://10.0.2.2:%d/api/module", web_port);
    struct webnet_request q = {.kind=WEBNET_MODULE, .generation=generation, .url=url,
        .origin=url, .method="GET", .credentials=WEBNET_CREDENTIALS_OMIT};
    return webnet_submit(net, &q, completed, r);
}
static void pump(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    do { webnet_pump(net, uptime_ms()); msleep(5); } while (uptime_ms() < until);
}
static bool idle(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    while (webnet_busy(net) && uptime_ms() < until) pump(5);
    return !webnet_busy(net);
}
static int child_wait(int pid, int *status, unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    int got;
    while ((got = waitpid(pid, status, WNOHANG)) == 0 && uptime_ms() < until) msleep(5);
    if (!got) { kill(pid); waitpid(pid, status, 0); return 0; }
    return got;
}
static int waiter(const char *path) {
    char port[16]; snprintf(port, sizeof port, "%d", tcp_port);
    char *args[] = {(char *)path, "--wait", port, NULL};
    int map[3] = {0, 1, 2};
    return spawn(path, args, map, 0);
}
static bool waiter_blocked(int pid) {
    static struct n_procinfo processes[64];
    int n = proclist(processes, 64);
    for (int i = 0; i < n; i++)
        if (processes[i].pid == pid)
            return processes[i].is_user && processes[i].state == 2; /* native TASK_BLOCKED */
    return false;
}
int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--wait")) {
        int s = tcp_connect(HOST, (uint16_t)atoi(argv[2]), 5000);
        if (s < 0) { printf("FAIL tcp capacity waiter errno=%d\n", errno); return 1; }
        tcp_close(s); return 0;
    }
    FILE *f = fopen("/data/tests/tcpport", "r"); char line[64] = "";
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    tcp_port = atoi(line);
    f = fopen("/data/tests/webports", "r");
    if (f) { fgets(line, sizeof line, f); fclose(f); web_port = atoi(line); }
    if (!tcp_port || !web_port || !net_wait_up(5000)) { puts("FAIL tcp capacity fixture/configuration missing"); return 1; }
    struct n_netinfo ni;
    if (net_info(&ni) == 0 && ni.ip[0] == 10 && ni.ip[1] == 0 && ni.ip[2] == 2 &&
        ni.mask[0] == 255 && ni.mask[1] == 255 && ni.mask[2] == 255 && ni.mask[3] == 0) {
        /* The runner's isolated slirp subnet has no guest at .250. Probe before
           any TCB saturation, so a fresh ARP miss must honor the same budget. */
        uint64_t before = uptime_ms();
        int s = tcp_connect(0x0a0002fa, (uint16_t)tcp_port, 150), error = errno;
        uint64_t elapsed = uptime_ms() - before;
        printf("TCP-CAPACITY-ARP elapsed=%lu errno=%d\n", (unsigned long)elapsed, error);
        check(s < 0 && error != EMFILE && elapsed < 1000 && (error != ETIMEDOUT || elapsed >= 100),
            "fresh isolated-subnet ARP lookup honors 150ms connect budget");
        if (s >= 0) tcp_close(s);
    } else puts("SKIP tcp capacity isolated-subnet ARP probe outside runner network");
    net = webnet_create();
    if (!net) { puts("FAIL tcp capacity worker allocation"); return 1; }
    int held[CAPACITY]; bool filled = true;
    for (int i = 0; i < CAPACITY; i++) held[i] = -1;
    /* No request line: the fixture deliberately waits until each client closes. */
    for (int i = 0; i < CAPACITY; i++) {
        held[i] = tcp_connect(HOST, (uint16_t)tcp_port, 5000);
        if (held[i] < 0) { filled = false; break; }
    }
    check(filled, "all finite active slots held without file descriptors");
    if (filled) {
        uint64_t before = uptime_ms(); int extra = tcp_connect(HOST, (uint16_t)tcp_port, 150), error = errno;
        uint64_t elapsed = uptime_ms() - before;
        check(extra < 0 && error == ETIMEDOUT && elapsed >= 100 && elapsed < 1000, "full pool waits within original deadline, not immediate EMFILE");
        if (extra >= 0) tcp_close(extra);
        struct result cancelled[2] = {0};
        check(request(&cancelled[0], 100) && request(&cancelled[1], 100), "two worker requests queued under TCB pressure");
        pump(200); before = uptime_ms(); webnet_cancel_generation(net, 100);
        check(!webnet_busy(net) && uptime_ms() - before < 2000 && !cancelled[0].calls && !cancelled[1].calls,
            "cancellation wakes killed capacity waiters and reaps both workers");
        int pid = waiter(argv[0]), status = -1;
        check(pid > 0, "spawn cooperative capacity waiter");
        if (pid > 0) {
            msleep(200);
            int got = waitpid(pid, &status, WNOHANG);
            check(got == 0, "waiter remains blocked while all slots are active");
            tcp_close(held[0]); held[0] = -1;
            if (got == 0) check(child_wait(pid, &status, 5500) == pid && status == 0, "FIN/TIME_WAIT release wakes waiting connection");
            else check(false, "capacity waiter survived until release");
        }
        held[0] = tcp_connect(HOST, (uint16_t)tcp_port, 5000);
        check(held[0] >= 0, "closing waiter slot is reclaimed only after protocol lifetime");
        pid = waiter(argv[0]); status = -1;
        if (pid > 0) {
            msleep(200);
            int got = waitpid(pid, &status, WNOHANG);
            bool blocked = got == 0 && waiter_blocked(pid);
            check(blocked, "kill target is an unreaped native blocked capacity waiter");
            if (got == 0) {
                before = uptime_ms();
                int killed = kill(pid);
                check(killed == 0, "native kill accepts the blocked capacity waiter");
                int reaped = child_wait(pid, &status, 1500);
                /* task_wait exposes task_exit's raw code; killed user returns
                   exit 130, not a POSIX-encoded wait status. */
                check(blocked && killed == 0 && reaped == pid && status == 130 && uptime_ms() - before < 1500,
                    "killed global-capacity waiter exits 130 promptly without a TCB");
            } else check(false, "capacity kill target did not exit before cancellation");
        } else check(false, "spawn killed capacity waiter");
    }
    for (int i = 0; i < CAPACITY; i++) if (held[i] >= 0) tcp_close(held[i]);
    /* More than the global budget: small module responses force repeated closes
       through the actual two-worker transport, with no sleep between batches. */
    for (int batch = 0; batch < 24; batch++) {
        struct result out[2] = {0}; uint64_t generation = 200 + (uint64_t)batch;
        bool submitted = request(&out[0], generation) && request(&out[1], generation);
        bool done = submitted && idle(15000);
        check(done && out[0].calls == 1 && out[1].calls == 1 && out[0].status == 200 && out[1].status == 200 &&
            !out[0].error[0] && !out[1].error[0] && out[0].body_ok && out[1].body_ok,
            "two-worker module burst reuses ended connections without fatal exhaustion");
        if (!done) webnet_cancel_generation(net, generation);
    }
    webnet_free(net);
    printf("tcpcapacitytest: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
