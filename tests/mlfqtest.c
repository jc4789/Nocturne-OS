/* 実 Nocturne spawn/pipe/window の scheduler 負荷。速度閾値で成功を偽装しない。
   4 CPU-bound 子を終了まで走らせ、BSP GUI 子の sleep 復帰遅延を実測する。 */
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
    unsigned magic, kind, count, pin_ok;
    unsigned long begin_ms, end_ms, cpu_ms, blocks, checksum;
    unsigned long lateness[SAMPLES];
};
static unsigned apic_id(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return b >> 24;
}
static unsigned long own_cpu_ms(void) {
    struct n_procinfo info[64];
    int n = proclist(info, 64), pid = getpid();
    for (int i = 0; i < n; i++) if (info[i].pid == pid) return (unsigned long)info[i].cpu_ms;
    return 0;
}
static int await_start(unsigned long start) {
    unsigned long now = (unsigned long)uptime_ms();
    return now >= start || msleep((unsigned)(start - now)) == 0;
}
static int write_all(int fd, const void *data, size_t bytes) {
    const unsigned char *p = data;
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n <= 0) return 0;
        p += n; bytes -= (size_t)n;
    }
    return 1;
}
static int child_burn(unsigned long start, unsigned seed) {
    struct report r = {0};
    r.magic = REPORT_MAGIC; r.kind = 1;
    if (!await_start(start)) return 2;
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
    return write_all(1, &r, sizeof r) ? 0 : 2;
}
static int child_interactive(unsigned long start) {
    struct report r = {0};
    r.magic = REPORT_MAGIC; r.kind = 2; r.pin_ok = 1;
    window_t *win = win_open(240, 80, "MLFQ: wake/present 診断", 0);
    if (!win) return 2;
    /* writable window mapping により sched_pin_space が sticky BSP pin する。 */
    unsigned bsp = apic_id();
    gfx_fill(&win->c, 0, 0, win->w, win->h, RGB(32, 28, 50));
    win_update(win);
    if (!await_start(start)) { win_close(win); return 2; }
    r.begin_ms = (unsigned long)uptime_ms();
    unsigned long cpu_begin = own_cpu_ms();
    for (unsigned i = 0; i < SAMPLES; i++) {
        unsigned long due = (unsigned long)uptime_ms() + 20;
        if (msleep(20)) { win_close(win); return 2; }
        unsigned long woke = (unsigned long)uptime_ms();
        r.lateness[r.count++] = woke > due ? woke - due : 0;
        if (apic_id() != bsp) r.pin_ok = 0;
        gfx_fill(&win->c, 8, 8, 24, 24, (i & 1) ? RGB(170, 130, 240) : RGB(90, 190, 180));
        win_update_rect(win, 8, 8, 24, 24);
    }
    r.end_ms = (unsigned long)uptime_ms();
    r.cpu_ms = own_cpu_ms() - cpu_begin;
    win_close(win);
    return r.pin_ok && write_all(1, &r, sizeof r) ? 0 : 2;
}
static int read_until(int fd, void *data, size_t bytes, unsigned long deadline) {
    unsigned char *p = data;
    while (bytes) {
        unsigned long now = (unsigned long)uptime_ms();
        if (now >= deadline) return 0;
        struct n_pollfd f = {fd, N_POLLIN, 0};
        if (poll(&f, 1, (int)(deadline - now)) <= 0) return 0;
        ssize_t n = read(fd, p, bytes);
        if (n <= 0) return 0;
        p += n; bytes -= (size_t)n;
    }
    return 1;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--burn"))
        return child_burn(strtoul(argv[2], NULL, 10), (unsigned)atoi(argv[3]));
    if (argc == 3 && !strcmp(argv[1], "--interactive"))
        return child_interactive(strtoul(argv[2], NULL, 10));
    struct n_cpuinfo cpu;
    if (cpu_info(&cpu)) return 1;
    int pid[CHILDREN], input[CHILDREN], nullfd = -1, ok = 1, completed = 0;
    struct report r[CHILDREN];
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < CHILDREN; i++) pid[i] = input[i] = -1;
    nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) return 1;
    unsigned long start = (unsigned long)uptime_ms() + 500, deadline = start + RUN_TIMEOUT_MS;
    char when[32]; snprintf(when, sizeof when, "%lu", start);
    /* GUI 子を先に準備し、4 burner 全員へ同じ開始時刻を渡す。 */
    for (unsigned i = 0; i < CHILDREN; i++) {
        int p[2];
        if (pipe(p)) { ok = 0; goto cleanup; }
        int map[3] = {nullfd, p[1], nullfd};
        char seed[16]; snprintf(seed, sizeof seed, "%u", i * 17 + 3);
        char *args[5] = {argv[0], i ? "--burn" : "--interactive", when, i ? seed : NULL, NULL};
        pid[i] = spawn(argv[0], args, map, 0);
        close(p[1]); input[i] = p[0];
        if (pid[i] < 0) { ok = 0; goto cleanup; }
    }
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (!read_until(input[i], &r[i], sizeof r[i], deadline) || r[i].magic != REPORT_MAGIC) {
            ok = 0; goto cleanup;
        }
        int status = -1;
        if (waitpid(pid[i], &status, 0) != pid[i] || status) { ok = 0; goto cleanup; }
        pid[i] = -1; completed++;
        if (i && (r[i].kind != 1 || r[i].blocks != WORK_BLOCKS)) ok = 0;
    }
    if (r[0].kind != 2 || r[0].count != SAMPLES || !r[0].pin_ok) ok = 0;
    for (unsigned i = 0; i < SAMPLES; i++) for (unsigned j = i + 1; j < SAMPLES; j++)
        if (r[0].lateness[j] < r[0].lateness[i]) {
            unsigned long temp = r[0].lateness[i]; r[0].lateness[i] = r[0].lateness[j]; r[0].lateness[j] = temp;
        }
    printf("mlfqtest: scheduler_cpus=%u workers=%u completed=%d/%u interactive_pin=%s\n",
           cpu.scheduler_cpus, cpu.worker_cpus, completed, (unsigned)CHILDREN, r[0].pin_ok ? "PASS" : "FAIL");
    printf("mlfqtest: wake_lateness_ms samples=%u p50=%lu p95=%lu max=%lu interactive_wall=%lu cpu=%lu (measurement, no speed threshold)\n",
           r[0].count, r[0].lateness[SAMPLES / 2], r[0].lateness[(SAMPLES * 95) / 100 - 1],
           r[0].lateness[SAMPLES - 1], r[0].end_ms - r[0].begin_ms, r[0].cpu_ms);
    for (unsigned i = 1; i < CHILDREN; i++)
        printf("mlfqtest: burn%u blocks=%lu wall_ms=%lu cpu_ms=%lu checksum=%lx\n", i,
               r[i].blocks, r[i].end_ms - r[i].begin_ms, r[i].cpu_ms, r[i].checksum);
cleanup:
    for (unsigned i = 0; i < CHILDREN; i++) {
        if (pid[i] > 0) { int status; kill(pid[i]); waitpid(pid[i], &status, 0); }
        if (input[i] >= 0) close(input[i]);
    }
    close(nullfd);
    printf("mlfqtest: %s\n", ok ? "ok" : "FAIL incomplete/timeout/protocol");
    return !ok;
}
