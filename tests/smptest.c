/* 独自spawn/pipeによる2 processの同時進行とAP間XSTATE/stack保存。 */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 40
struct sample { unsigned long begin, end; unsigned cpu_begin, cpu_end; };
struct report { unsigned magic, count, xstate, stack; unsigned long value; struct sample s[SAMPLES]; };
static unsigned long ticks(void) {
    unsigned a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((unsigned long)d << 32) | a;
}
static unsigned apic_id(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    return b >> 24;
}
static __attribute__((noinline)) int stack_probe(void) {
    volatile unsigned char buffer[96 * 1024];
    for (unsigned i = 0; i < sizeof buffer; i += 4096) buffer[i] = (unsigned char)(i / 4096 + 17);
    for (unsigned i = 0; i < sizeof buffer; i += 4096)
        if (buffer[i] != (unsigned char)(i / 4096 + 17)) return 0;
    return 1;
}
static __attribute__((noinline)) long avx_sleep(const unsigned long *in, unsigned long *out) {
    long result;
    __asm__ volatile(".byte 0xc5,0xfe,0x6f,0x06\nint $0x80\n"
                     ".byte 0xc5,0xfe,0x7f,0x02\n.byte 0xc5,0xf8,0x77\n"
                     : "=a"(result) : "0"((long)SYS_SLEEP), "D"(2L), "S"(in), "d"(out) : "memory", "cc");
    return result;
}
static int child(unsigned long start, unsigned seed) {
    struct report r;
    memset(&r, 0, sizeof r);
    struct n_cpuinfo info;
    if (cpu_info(&info)) return 2;
    r.stack = stack_probe(); /* top64KiB外の本物user page faultをBSPへ移す */
    r.xstate = 1;
    for (unsigned round = 0; round < 4; round++) {
        if (info.flags & N_CPU_AVX) {
            unsigned long in[4] = {seed + round, seed * 17UL, 0x123456789abcdef0UL + seed,
                                   0xfedcba9876543210UL - round}, out[4];
            if (avx_sleep(in, out) || memcmp(in, out, sizeof in)) r.xstate = 0;
        } else if (msleep(2)) r.xstate = 0;
    }
    unsigned long t0 = ticks();
    if (msleep(20)) return 2;
    unsigned long per_ms = (ticks() - t0) / 20;
    if (!per_ms) return 2;
    unsigned long now = uptime_ms();
    if (now < start && msleep((unsigned)(start - now))) return 2;
    volatile unsigned long value = seed;
    r.magic = 0x534d5031;
    for (unsigned i = 0; i < SAMPLES; i++) {
        struct sample *s = &r.s[i];
        s->cpu_begin = apic_id();
        s->begin = ticks();
        do {
            for (unsigned n = 0; n < 4096; n++) value = value * 6364136223846793005UL + 1442695040888963407UL;
        } while (ticks() - s->begin < per_ms * 5);
        s->end = ticks();
        s->cpu_end = apic_id();
        r.count++;
    }
    r.value = value;
    return write(1, &r, sizeof r) == (ssize_t)sizeof r && r.stack && r.xstate ? 0 : 2;
}
static int read_all(int fd, void *p, size_t n) {
    unsigned char *b = p;
    while (n) {
        ssize_t got = read(fd, b, n);
        if (got <= 0) return 0;
        b += got;
        n -= got;
    }
    return 1;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--child")) return child(strtoul(argv[2], NULL, 10), (unsigned)atoi(argv[3]));
    if (argc == 2 && !strcmp(argv[1], "--fault")) {
        *(volatile unsigned char *)0x12345000UL = 1;
        return 3;
    }
    if (argc == 2 && !strcmp(argv[1], "--kill")) {
        char ready = 'K';
        if (write(1, &ready, 1) != 1) return 3;
        for (;;) __asm__ volatile("pause" : : : "memory");
    }
    struct n_cpuinfo info;
    if (cpu_info(&info)) return 1;
    int nullfd = open("/dev/null", O_RDWR), p[2][2], pid[2] = {-1, -1}, status;
    struct report r[2];
    memset(r, 0, sizeof r);
    if (nullfd < 0) return 1;
    char start[32];
    snprintf(start, sizeof start, "%lu", uptime_ms() + 200);
    for (unsigned i = 0; i < 2; i++) {
        if (pipe(p[i])) return 1;
        int map[3] = {nullfd, p[i][1], nullfd};
        char *args[5] = {argv[0], "--child", start, i ? "29" : "11", NULL};
        pid[i] = spawn(argv[0], args, map, 0);
        close(p[i][1]);
        if (pid[i] < 0) { if (pid[0] > 0) { kill(pid[0]); waitpid(pid[0], &status, 0); } return 1; }
    }
    int ok = 1;
    for (unsigned i = 0; i < 2; i++) {
        if (!read_all(p[i][0], &r[i], sizeof r[i])) ok = 0;
        close(p[i][0]);
        if (waitpid(pid[i], &status, 0) != pid[i] || status) ok = 0;
        if (r[i].magic != 0x534d5031 || r[i].count != SAMPLES || !r[i].stack || !r[i].xstate) ok = 0;
    }
    unsigned overlap = 0, cpu_a = 0, cpu_b = 0;
    unsigned first_a = 0, first_b = 0;
    if (ok) for (unsigned a = 0; a < SAMPLES; a++) for (unsigned b = 0; b < SAMPLES; b++) {
        struct sample *x = &r[0].s[a], *y = &r[1].s[b];
        if (x->cpu_begin == x->cpu_end && y->cpu_begin == y->cpu_end && x->cpu_begin != y->cpu_begin &&
            x->begin < y->end && y->begin < x->end) {
            overlap++;
            if (overlap == 1) { first_a = a; first_b = b; }
            cpu_a = x->cpu_begin;
            cpu_b = y->cpu_begin;
        }
    }
    if (info.scheduler_cpus > 1 && !overlap) ok = 0;
    if (info.scheduler_cpus == 1 && overlap) ok = 0;
    printf("smptest: user2 scheduler=%u apic=%u/%u overlap=%u XSTATE/stack=%s\n", info.scheduler_cpus,
           cpu_a, cpu_b, overlap, ok ? "PASS" : "FAIL");
    if (overlap) printf("smptest: raw overlap cpu%u [%lu,%lu] cpu%u [%lu,%lu]\n",
        r[0].s[first_a].cpu_begin, r[0].s[first_a].begin, r[0].s[first_a].end,
        r[1].s[first_b].cpu_begin, r[1].s[first_b].begin, r[1].s[first_b].end);
    /* CR2はBSPがfault処理。stderrの本物addrとexit状態を要求する。 */
    int fault[2];
    if (pipe(fault)) return 1;
    int fmap[3] = {nullfd, nullfd, fault[1]};
    char *fargs[3] = {argv[0], "--fault", NULL};
    int fpid = spawn(argv[0], fargs, fmap, 0);
    close(fault[1]);
    char text[512];
    ssize_t got = read(fault[0], text, sizeof text - 1);
    close(fault[0]);
    text[got > 0 ? got : 0] = 0;
    if (fpid < 0 || waitpid(fpid, &status, 0) != fpid || status != 142 || !strstr(text, "12345000")) ok = 0;
    int kp[2];
    if (pipe(kp)) return 1;
    int kmap[3] = {nullfd, kp[1], nullfd};
    char *kargs[3] = {argv[0], "--kill", NULL};
    int kpid = spawn(argv[0], kargs, kmap, 0);
    close(kp[1]);
    char ready = 0;
    if (kpid < 0 || read(kp[0], &ready, 1) != 1 || ready != 'K' || msleep(20) || kill(kpid) ||
        waitpid(kpid, &status, 0) != kpid || status != 130) ok = 0;
    close(kp[0]);
    close(nullfd);
    printf("smptest: fault/CR2/kill/retire %s\n", ok ? "PASS" : "FAIL");
    printf("smptest: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
