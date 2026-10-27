/* Real Nocturne TCB growth, cooperative wait/kill and webfetch recovery.
   The TCB table grows; what still waits is a connect whose peer never
   answers (no guest at .250 on the runner's isolated slirp subnet).
   Uses scripts/test.py's TCP server and HTTP fixture; no POSIX socket layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "webnet.h"

#define HOST 0x0a000202
#define SILENT 0x0a0002fa /* no guest answers ARP at .250 */
#define HELD 40 /* past the TCB table's first 16 slots, twice grown */
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
static uint64_t request_at(struct result *r, uint64_t generation, const char *host) {
    char url[256]; snprintf(url, sizeof url, "http://%s:%d/api/module", host, web_port);
    struct webnet_request q = {.kind=WEBNET_MODULE, .generation=generation, .url=url,
        .origin=url, .method="GET", .credentials=WEBNET_CREDENTIALS_OMIT};
    return webnet_submit(net, &q, completed, r);
}
static uint64_t request(struct result *r, uint64_t generation) { return request_at(r, generation, "10.0.2.2"); }
static void pump(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    do { webnet_pump(net, uptime_ms()); n_sleep_ms(5); } while (uptime_ms() < until);
}
static bool idle(unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    while (webnet_busy(net) && uptime_ms() < until) pump(5);
    return !webnet_busy(net);
}
/* Polls the app's result for up to ms; stops and reaps it when it overruns. Returns 1 when it ended by itself. */
static int child_wait(n_handle app, int *status, unsigned ms) {
    uint64_t until = uptime_ms() + ms;
    n_result got;
    while ((got = n_app_result(app, 0, status)) == -N_ERR_NOT_READY && uptime_ms() < until) n_sleep_ms(5);
    if (got == -N_ERR_NOT_READY) { n_app_stop(app, 0); n_app_result(app, -1, status); return 0; }
    return got == 0;
}
/* path is complete: the native ABI launches no relative path. */
static n_handle waiter(const char *path) {
    char port[16]; snprintf(port, sizeof port, "%d", tcp_port);
    const char *args[] = {path, "--wait", port};
    int32_t map[3] = {0, 1, 2};
    struct n_launch launch = {.arguments = args, .argument_count = 3, .handles = map, .handle_count = 3};
    return n_app_launch(path, &launch);
}
static bool waiter_blocked(n_handle app) {
    static struct n_procinfo processes[64];
    n_result id = n_app_id(app);
    int n = proclist(processes, 64);
    for (int i = 0; id > 0 && i < n; i++)
        if (processes[i].pid == id)
            return processes[i].is_user && processes[i].state == 2; /* native TASK_BLOCKED */
    return false;
}
int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--wait")) {
        n_result s = tcp_connect(SILENT, (uint16_t)atoi(argv[2]), 5000);
        if (s >= 0) { tcp_close((n_handle)s); return 0; }
        return s == -N_ERR_TIMEOUT ? 2 : 1;
    }
    char program[4096]; /* argv[0] is as it was typed; an app is launched by its complete path */
    if (n_path_complete(argv[0], program, sizeof program) >= 0) argv[0] = program;
    FILE *f = fopen("C:\\Tests\\tcpport", "r"); char line[64] = "";
    if (f) { fgets(line, sizeof line, f); fclose(f); }
    tcp_port = atoi(line);
    f = fopen("C:\\Tests\\webports", "r");
    if (f) { fgets(line, sizeof line, f); fclose(f); web_port = atoi(line); }
    if (!tcp_port || !web_port || !net_wait_up(5000)) { puts("FAIL tcp capacity fixture/configuration missing"); return 1; }
    struct n_netinfo ni;
    bool isolated = net_info(&ni) == 0 && ni.ip[0] == 10 && ni.ip[1] == 0 && ni.ip[2] == 2 &&
        ni.mask[0] == 255 && ni.mask[1] == 255 && ni.mask[2] == 255 && ni.mask[3] == 0;
    if (isolated) {
        /* A fresh ARP miss honors the connect's own deadline. */
        uint64_t before = uptime_ms();
        n_result s = tcp_connect(SILENT, (uint16_t)tcp_port, 150);
        uint64_t elapsed = uptime_ms() - before;
        printf("TCP-CAPACITY-ARP elapsed=%lu result=%s\n", (unsigned long)elapsed, s < 0 ? n_result_text(s) : "connected");
        check(s < 0 && s != -N_ERR_NO_MEMORY && elapsed < 1000 && (s != -N_ERR_TIMEOUT || elapsed >= 100),
            "fresh isolated-subnet ARP lookup honors 150ms connect budget");
        if (s >= 0) tcp_close((n_handle)s);
    } else puts("SKIP tcp capacity isolated-subnet probes outside runner network");
    net = webnet_create();
    if (!net) { puts("FAIL tcp capacity worker allocation"); return 1; }
    int held[HELD]; bool filled = true;
    for (int i = 0; i < HELD; i++) held[i] = -1;
    /* 先行worker試験のFIN待ちは既存kernelの20秒の生存期間を持つ。準備段階だけ長く待つ。 */
    for (int i = 0; i < HELD; i++) {
        held[i] = tcp_connect(HOST, (uint16_t)tcp_port, 25000);
        if (held[i] < 0) { filled = false; break; }
    }
    check(filled, "live connections grow the TCB table past its first slots");
    if (filled) {
        uint64_t before = uptime_ms(); n_result extra = tcp_connect(HOST, (uint16_t)tcp_port, 150);
        uint64_t elapsed = uptime_ms() - before;
        check(extra >= 0 && elapsed < 150, "a further connection needs no free slot to wait for");
        if (extra >= 0) tcp_close((n_handle)extra);
    }
    if (isolated) {
        struct result cancelled[2] = {0};
        check(request_at(&cancelled[0], 100, "10.0.2.250") && request_at(&cancelled[1], 100, "10.0.2.250"),
            "two worker requests wait on a peer that never answers");
        pump(200);
        check(webnet_busy(net) && !cancelled[0].calls && !cancelled[1].calls, "unanswered worker requests are still in flight");
        uint64_t before = uptime_ms(); webnet_cancel_generation(net, 100);
        check(!webnet_busy(net) && uptime_ms() - before < 2000 && !cancelled[0].calls && !cancelled[1].calls,
            "cancellation wakes the waiting workers and reaps both");
        n_handle app = waiter(argv[0]);
        int status = -1;
        check(app >= 0, "launch cooperative connect waiter");
        if (app >= 0) {
            n_sleep_ms(200);
            bool running = n_app_result(app, 0, &status) == -N_ERR_NOT_READY;
            bool blocked = running && waiter_blocked(app);
            check(blocked, "stop target is a still-running native blocked connect waiter");
            if (running) {
                before = uptime_ms();
                n_result stopped = n_app_stop(app, 0);
                check(stopped >= 0, "native stop accepts the blocked connect waiter");
                int reaped = child_wait(app, &status, 1500);
                /* A stopped app reports the kernel's kill code, 130. */
                check(blocked && stopped >= 0 && reaped == 1 && status == 130 && uptime_ms() - before < 1500,
                    "stopped connect waiter exits 130 promptly");
            } else check(false, "connect waiter did not exit before its stop");
            n_handle_close(app);
        }
    }
    for (int i = 0; i < HELD; i++) if (held[i] >= 0) tcp_close(held[i]);
    /* Small module responses force repeated closes through the actual
       two-worker transport, with no sleep between batches. */
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
