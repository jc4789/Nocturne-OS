/* The API distinguishes detected CPUs, real AP jobs, and the single scheduler. */
#include <nocturne.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void) {
    struct n_cpuinfo info;
    memset(&info, 0xa5, sizeof info);
    if (cpu_info(&info) < 0 || info.version != 1 || !info.detected_cpus ||
        !info.online_cpus || info.online_cpus > info.detected_cpus ||
        info.worker_cpus + 1 != info.online_cpus || info.scheduler_cpus != 1 ||
        !!(info.flags & N_CPU_AP_WORKERS) != !!info.worker_cpus ||
        !(info.flags & N_CPU_SSE2) || (info.flags & N_CPU_AVX) ||
        (info.worker_cpus && (!info.parallel_jobs || !info.worker_chunks))) {
        puts("cpuinfotest: invalid capability snapshot");
        return 1;
    }
    if (cpu_info(NULL) != -1 || errno != EFAULT) {
        puts("cpuinfotest: invalid pointer was not rejected");
        return 1;
    }
    printf("cpuinfotest: detected=%u online=%u workers=%u scheduler=%u jobs=%lu chunks=%lu; ok\n",
           info.detected_cpus, info.online_cpus, info.worker_cpus, info.scheduler_cpus,
           info.parallel_jobs, info.worker_chunks);
    return 0;
}
