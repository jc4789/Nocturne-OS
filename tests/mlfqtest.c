/* 実 Nocturne n_app_launch/n_channel_pair/window の scheduler 負荷。速度閾値で成功を偽装しない。
   4 CPU-bound 子を終了まで走らせ、GUI 子の sleep 復帰遅延を実測する。 */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BURNERS 4
#define CHILDREN (BURNERS + 1)
#define SAMPLES 60
#define WORK_BLOCKS 16000
#define REPORT_MAGIC 0x4d4c4651u
#define RUN_TIMEOUT_MS 20000
struct report {
    unsigned magic, kind, count, present_ok;
    unsigned long begin_ms, end_ms, cpu_ms, blocks, checksum, cpu_mask;
    unsigned long lateness[SAMPLES];
};
static unsigned long own_cpu_ms(void) {
    struct n_procinfo info[64];
    int n = proclist(info, 64), pid = (int)n_app_self();
    for (int i = 0; i < n; i++) if (info[i].pid == pid) return (unsigned long)info[i].cpu_ms;
    return 0;
}
static void await_start(unsigned long start) {
    unsigned long now = (unsigned long)uptime_ms();
    if (now < start) n_sleep_ms(start - now);
}
/* One report is one channel message. */
static int send_report(const struct report *r) {
    return n_channel_send(1, r, sizeof *r, -1) == (n_result)sizeof *r;
}
static int child_burn(unsigned long start, unsigned seed) {
    struct report r = {0};
    r.magic = REPORT_MAGIC; r.kind = 1;
    await_start(start);
    r.begin_ms = (unsigned long)uptime_ms();
    unsigned long cpu_begin = own_cpu_ms();
    volatile unsigned long value = seed;
    /* この区画には syscall/yield/sleep がない。本物の CPU-bound user 実行。 */
    for (unsigned block = 0; block < WORK_BLOCKS; block++) {
        for (unsigned i = 0; i < 8192; i++) value = value * 6364136223846793005UL + 1442695040888963407UL;
        r.blocks++;
    }
    r.end_ms = (unsigned long)uptime_ms();
    r.cpu_ms = own_cpu_ms() - cpu_begin;
    r.checksum = value;
    return send_report(&r) ? 0 : 2;
}
static int child_interactive(unsigned long start) {
    struct report r = {0};
    r.magic = REPORT_MAGIC; r.kind = 2;
    window_t *win;
    if (win_open(240, 80, "MLFQ: wake/present 診断", 0, &win) < 0) return 2;
    /* 窓を持つ app も他の app と同じく任意の scheduler CPU で走る（BSP 固定はない）。
       負荷下でも毎回の present が成功することを要求し、走った CPU は記録だけする。 */
    gfx_fill(&win->c, 0, 0, win->w, win->h, RGB(32, 28, 50));
    r.present_ok = win_update(win) == 0;
    await_start(start);
    r.begin_ms = (unsigned long)uptime_ms();
    unsigned long cpu_begin = own_cpu_ms();
    for (unsigned i = 0; i < SAMPLES; i++) {
        unsigned long due = (unsigned long)uptime_ms() + 20;
        n_sleep_ms(20);
        unsigned long woke = (unsigned long)uptime_ms();
        r.lateness[r.count++] = woke > due ? woke - due : 0;
        r.cpu_mask |= 1UL << (cpu_index() % 64);
        gfx_fill(&win->c, 8, 8, 24, 24, (i & 1) ? RGB(170, 130, 240) : RGB(90, 190, 180));
        if (win_update_rect(win, 8, 8, 24, 24) != 0) r.present_ok = 0;
    }
    r.end_ms = (unsigned long)uptime_ms();
    r.cpu_ms = own_cpu_ms() - cpu_begin;
    win_close(win);
    return r.present_ok && send_report(&r) ? 0 : 2;
}
/* Waits on an event queue until the child's report arrives or the deadline passes. */
static int receive_until(n_handle channel, void *data, size_t bytes, unsigned long deadline) {
    unsigned long now = (unsigned long)uptime_ms();
    if (now >= deadline) return 0;
    n_handle events = n_events_create();
    if (events < 0) return 0;
    struct n_event ev;
    int ok = n_events_watch(events, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1) == 0 &&
             n_events_wait(events, &ev, 1, (int64_t)(deadline - now)) == 1 &&
             n_channel_receive(channel, data, bytes, 0, NULL) == (n_result)bytes;
    n_handle_close(events);
    return ok;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--burn"))
        return child_burn(strtoul(argv[2], NULL, 10), (unsigned)atoi(argv[3]));
    if (argc == 3 && !strcmp(argv[1], "--interactive"))
        return child_interactive(strtoul(argv[2], NULL, 10));
    struct n_cpuinfo cpu;
    if (cpu_info(&cpu)) return 1;
    n_handle app[CHILDREN], input[CHILDREN];
    int ok = 1, completed = 0;
    struct report r[CHILDREN];
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < CHILDREN; i++) app[i] = input[i] = -1;
    unsigned long start = (unsigned long)uptime_ms() + 500, deadline = start + RUN_TIMEOUT_MS;
    char when[32]; snprintf(when, sizeof when, "%lu", start);
    /* GUI 子を先に準備し、4 burner 全員へ同じ開始時刻を渡す。 */
    for (unsigned i = 0; i < CHILDREN; i++) {
        n_handle c[2];
        if (n_channel_pair(c) < 0) { ok = 0; goto cleanup; }
        int32_t handles[3] = {-1, c[1], -1};
        char seed[16]; snprintf(seed, sizeof seed, "%u", i * 17 + 3);
        const char *args[4] = {argv[0], i ? "--burn" : "--interactive", when, seed};
        struct n_launch launch = {.arguments = args, .argument_count = i ? 4 : 3, .handles = handles, .handle_count = 3};
        app[i] = n_app_launch(argv[0], &launch);
        n_handle_close(c[1]); input[i] = c[0];
        if (app[i] < 0) { ok = 0; goto cleanup; }
    }
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (!receive_until(input[i], &r[i], sizeof r[i], deadline) || r[i].magic != REPORT_MAGIC) {
            ok = 0; goto cleanup;
        }
        int status = -1;
        if (n_app_result(app[i], -1, &status) < 0 || status) { ok = 0; goto cleanup; }
        n_handle_close(app[i]); app[i] = -1; completed++;
        if (i && (r[i].kind != 1 || r[i].blocks != WORK_BLOCKS)) ok = 0;
    }
    if (r[0].kind != 2 || r[0].count != SAMPLES || !r[0].present_ok) ok = 0;
    for (unsigned i = 0; i < SAMPLES; i++) for (unsigned j = i + 1; j < SAMPLES; j++)
        if (r[0].lateness[j] < r[0].lateness[i]) {
            unsigned long temp = r[0].lateness[i]; r[0].lateness[i] = r[0].lateness[j]; r[0].lateness[j] = temp;
        }
    printf("mlfqtest: scheduler_cpus=%u workers=%u completed=%d/%u interactive_present=%s cpu_mask=%lx\n",
           cpu.scheduler_cpus, cpu.worker_cpus, completed, (unsigned)CHILDREN, r[0].present_ok ? "PASS" : "FAIL",
           r[0].cpu_mask);
    printf("mlfqtest: wake_lateness_ms samples=%u p50=%lu p95=%lu max=%lu interactive_wall=%lu cpu=%lu (measurement, no speed threshold)\n",
           r[0].count, r[0].lateness[SAMPLES / 2], r[0].lateness[(SAMPLES * 95) / 100 - 1],
           r[0].lateness[SAMPLES - 1], r[0].end_ms - r[0].begin_ms, r[0].cpu_ms);
    for (unsigned i = 1; i < CHILDREN; i++)
        printf("mlfqtest: burn%u blocks=%lu wall_ms=%lu cpu_ms=%lu checksum=%lx\n", i,
               r[i].blocks, r[i].end_ms - r[i].begin_ms, r[i].cpu_ms, r[i].checksum);
cleanup:
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (app[i] >= 0) { int status; n_app_stop(app[i], 0); n_app_result(app[i], -1, &status); n_handle_close(app[i]); }
        if (input[i] >= 0) n_handle_close(input[i]);
    }
    printf("mlfqtest: %s\n", ok ? "ok" : "FAIL incomplete/timeout/protocol");
    return !ok;
}
