/* detected、pure worker、実user scheduler役を区別する。 */
#include <nocturne.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* TinyCCのassemblerはAVX mnemonic/SIMD clobber名を持たない。
   noinline整数関数の通常call ABIでSIMD caller-saveを表し、固定GPRで命令を符号化する。 */
static __attribute__((noinline)) long avx_sleep_round(const unsigned long *in, unsigned long *out) {
    long result;
    __asm__ volatile(".byte 0xc5,0xfe,0x6f,0x06\n" /* vmovdqu (%rsi), %ymm0 */
                     "int $0x80\n"
                     ".byte 0xc5,0xfe,0x7f,0x02\n" /* vmovdqu %ymm0, (%rdx) */
                     ".byte 0xc5,0xf8,0x77\n"     /* vzeroupper */
                     : "=a"(result)
                     : "0"((long)SYS_SLEEP), "D"(2L), "S"(in), "d"(out)
                     : "memory", "cc");
    return result;
}

static int avx_boundary(unsigned flags) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    unsigned long xcr0 = 0;
    if ((c & ((1u << 26) | (1u << 27))) == ((1u << 26) | (1u << 27))) {
        __asm__ volatile(".byte 0x0f,0x01,0xd0" : "=a"(a), "=d"(d) : "c"(0u)); /* xgetbv */
        xcr0 = ((unsigned long)d << 32) | a;
    }
    int usable = (c & (1u << 28)) && (xcr0 & 7) == 7;
    if (!!(flags & N_CPU_AVX) != !!usable) return -1;
    if (!usable) return 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned long in[4] = {0x123456789abcdef0UL + i, 0xfedcba9876543210UL - i,
                               0x1020304050607080UL + i, 0x8877665544332211UL - i}, out[4];
        /* user YMM→実SYS_SLEEP/schedule→YMM読戻しの期待は32B/8回のまま。 */
        if (avx_sleep_round(in, out) || memcmp(in, out, sizeof in)) return -1;
    }
    return 1;
}

int main(void) {
    struct n_cpuinfo info;
    memset(&info, 0xa5, sizeof info);
    if (cpu_info(&info) < 0 || info.version != 1 || !info.detected_cpus ||
        !info.online_cpus || info.online_cpus > info.detected_cpus ||
        info.worker_cpus + info.scheduler_cpus != info.online_cpus ||
        !info.scheduler_cpus || info.scheduler_cpus > 2 ||
        !!(info.flags & N_CPU_AP_WORKERS) != !!info.worker_cpus ||
        !(info.flags & N_CPU_SSE2) ||
        (info.worker_cpus && (!info.parallel_jobs || !info.worker_chunks))) {
        puts("cpuinfotest: invalid capability snapshot");
        return 1;
    }
    if (cpu_info(NULL) != -1 || errno != EFAULT) {
        puts("cpuinfotest: invalid pointer was not rejected");
        return 1;
    }
    int avx = avx_boundary(info.flags);
    if (avx < 0) {
        puts("cpuinfotest: AVX capability/context mismatch");
        return 1;
    }
    printf("cpuinfotest: XSTATE AVX=%s; ok\n", avx ? "PASS" : "disabled");
    printf("cpuinfotest: detected=%u online=%u workers=%u scheduler=%u jobs=%lu chunks=%lu; ok\n",
           info.detected_cpus, info.online_cpus, info.worker_cpus, info.scheduler_cpus,
           info.parallel_jobs, info.worker_chunks);
    return 0;
}
